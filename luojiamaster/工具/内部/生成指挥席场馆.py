"""Offline import for the two named RMUC2027 visual meshes. Run once on the build PC.

Python 3.12 with assimp_py 1.2, numpy, Pillow. Inputs and outputs can be anywhere.
The client only needs the resulting arena/*.mesh and requires no Internet connection.
"""
import argparse
import hashlib
import io
import json
import os
import struct
import tempfile
import tempfile
from contextlib import ExitStack
from pathlib import Path

import assimp_py as assimp
import numpy as np
from PIL import Image


def glb_texture(data):
    length = struct.unpack_from("<I", data, 12)[0]
    spec = json.loads(data[20:20 + length])
    if len(spec["images"]) != 1:
        raise ValueError("Expected a single image in the pinned GLB.")
    view = spec["bufferViews"][spec["images"][0]["bufferView"]]
    start = 28 + length + view["byteOffset"]
    image = Image.open(io.BytesIO(data[start:start + view["byteLength"]])).convert("RGBA")
    return image


def convert(path, dest, detail):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if not detail and digest != "ef40412bae1324475aab47e908e150b7b21d66f233740561232df5adf7919882":
        raise ValueError("Light map does not match the pinned author release.")
    if detail and digest != "9ee719596d28923248a26bf3f2ccca187f9974bc357860aea9b68bce2a2f6fec":
        raise ValueError("Detailed map does not match the pinned author's FBX.")
    image = Image.new("RGBA", (1, 1), "white") if detail else glb_texture(data)
    del data
    # Assimp's Windows narrow importer accepts relative ASCII filenames, even
    # when the actual parent directory contains Chinese characters.
    original = Path.cwd()
    try:
        os.chdir(path.parent)
        scene = assimp.import_file(path.name, assimp.Process_Triangulate |
            assimp.Process_PreTransformVertices | assimp.Process_JoinIdenticalVertices |
            assimp.Process_GenSmoothNormals | assimp.Process_FlipUVs)
    finally:
        os.chdir(original)
    vertex_count = sum(mesh.num_vertices for mesh in scene.meshes)
    index_count = sum(mesh.num_faces * 3 for mesh in scene.meshes)
    if index_count != (7071310 if detail else 880181) * 3:
        raise ValueError("Unexpected number of imported triangles.")
    bounds = np.stack([np.stack((p.min(0), p.max(0))) for mesh in scene.meshes
                       if (p := np.asarray(mesh.vertices).reshape(-1, 3)).size])
    lo, hi = bounds[:, 0].min(0), bounds[:, 1].max(0)
    # Assimp resolves the FBX's units and root transform. Identify the published
    # field's long / vertical axes by its known dimensions, then explicitly orient
    # both meshes towards the official Fig 4-1. Checked visually after import.
    long_axis, vert_axis = int(np.argmax(hi-lo)), int(np.argmin(hi-lo))
    short_axis = 3 - long_axis - vert_axis
    axes = [long_axis, vert_axis, short_axis]
    scale = 28.0 / float(hi[long_axis] - lo[long_axis])
    offset = (lo + hi) * 0.5
    # Both top-down views use X to the right and positive Z towards the bottom.
    signs = np.array([-1, 1, -1], np.float32)
    if detail and vert_axis == 2:
        signs[2] = -1
    # Normalize only a common origin and units. Height is preserved.
    offset[vert_axis] = lo[vert_axis] + 0.301 / scale
    arr = np.dtype([("pos", "<i2", (4,)), ("color", "u1", (4,)), ("uv", "<f2", (2,))])
    dest.parent.mkdir(parents=True, exist_ok=True)
    with ExitStack() as stack:
        out = tempfile.NamedTemporaryFile("wb", dir=dest.parent, suffix=".tmp", delete=False)
        staged = Path(out.name)
        stack.callback(staged.unlink, missing_ok=True)
        stack.enter_context(out)
        out.write(struct.pack("<8sIIII", b"RMUF27V1", vertex_count, index_count, *image.size))
        light = np.array([-.28, .92, -.27], np.float32)
        for mesh in scene.meshes:
            p = (np.asarray(mesh.vertices).reshape(-1, 3) - offset)[:, axes] * signs * scale
            n = np.asarray(mesh.normals).reshape(-1, 3)[:, axes] * signs
            mat = scene.materials[mesh.material_index]
            base = np.clip(mat["COLOR_DIFFUSE"][:3], 0, 1)
            if not detail:
                base = np.where(base < 0.0031308, 12.92 * base, 1.055 * base**(1/2.4) - 0.055)
            brightness = 0.65 + 0.35 * np.maximum((n * light).sum(axis=1), 0)
            verts = np.zeros(mesh.num_vertices, arr)
            verts["pos"][:, :3] = np.rint(p * 1024).astype(np.int16)
            verts["color"][:, :3] = np.rint(np.clip(brightness[:, None] * base, 0, 1) * 255)
            if not detail and mat.get("TEXTURES"):
                verts["color"][:, 3] = 255
                verts["uv"] = np.asarray(mesh.texcoords[0]).reshape(mesh.num_vertices, -1)[:, :2]
            out.write(verts.tobytes())
        first_vertex = 0
        for mesh in scene.meshes:
            indices = np.asarray(mesh.indices, np.uint32).ravel()
            if len(indices) != mesh.num_faces*3 or indices.max() >= mesh.num_vertices:
                raise ValueError("Invalid triangle data")
            out.write((indices + first_vertex).astype("<u4").tobytes())
            first_vertex += mesh.num_vertices
        out.write(image.tobytes())
        out.close()
        staged.replace(dest)
    print(f"{path.name} sha256={digest} bounds={lo}..{hi} axes={axes} signs={signs} scale={scale:.8f}")
    print(f"{dest.name} vertices={vertex_count} triangles={index_count//3} atlas={image.size} bytes={dest.stat().st_size}")


def weld(source, target):
    with source.open("rb") as fp:
        magic, count, elements, width, height = struct.unpack("<8sIIII", fp.read(24))
    if magic != b"RMUF27V1" or source.stat().st_size != 24+16*count+4*elements+4*width*height:
        raise ValueError("Invalid arena mesh.")
    vertices = np.memmap(source, mode="r", dtype="V16", offset=24, shape=(count,))
    indices = np.memmap(source, mode="r", dtype="<u4", offset=24+count*16, shape=(elements,))
    atlas = np.memmap(source, mode="r", dtype="u1", offset=24+count*16+elements*4)
    if int(indices.max()) >= count:
        raise ValueError("Invalid index.")
    # The original order improves GPU fetch locality for neighboring triangles.
    unique, first, inverse = np.unique(vertices, return_index=True, return_inverse=True)
    order = np.argsort(first)
    packed = unique[order]
    to_packed = np.empty(len(unique), dtype="<u4")
    to_packed[order] = np.arange(len(order), dtype="<u4")
    remap = to_packed[inverse]
    digest = hashlib.sha256()
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, suffix=".tmp", delete=False) as out:
        staged = Path(out.name)
    try:
        with staged.open("wb") as out:
            out.write(struct.pack("<8sIIII", magic, len(packed), elements, width, height))
            out.write(packed.data)
            for start in range(0, elements, 65536):
                idx = indices[start:start+65536]
                exact = vertices[idx]
                local = remap[idx]
                if not np.array_equal(exact, packed[local]):
                    raise ValueError("Triangle vertex attributes changed.")
                digest.update(exact.data)
                out.write(local.data)
            out.write(atlas.data)
        os.replace(staged, target)
    finally:
        staged.unlink(missing_ok=True)
    print(f"{count} -> {len(packed)} vertices; {elements//3} verified triangles; "
          f"corner data SHA256 {digest.hexdigest()}; {target.stat().st_size} bytes")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["light", "detailed"])
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    output = args.destination.resolve()
    if output.exists():
        parser.error("输出文件已存在，请选择新的文件名。")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as scratch:
        full = Path(scratch) / "import.mesh"
        convert(args.source.resolve(), full, args.mode == "detailed")
        weld(full, output)
