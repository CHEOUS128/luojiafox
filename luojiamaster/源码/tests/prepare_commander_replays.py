"""Create local HEVC clips for the commander's display and reproducible load tests."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib


def encode(ffmpeg, source, destination, width, height, embedded):
    def run(args, data=None):
        return subprocess.run(
            [str(ffmpeg), "-hide_banner", "-nostdin", "-loglevel", "error", *args],
            input=data, stdout=subprocess.PIPE, check=True, timeout=180,
        ).stdout

    fps = 15 if embedded else 30
    geometry = (f"fps={fps},scale={width}:{height}:force_original_aspect_ratio=decrease:"
                f"force_divisible_by=2,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,setsar=1")
    hevc = run([*source, "-t", "4", "-an", "-vf", geometry, "-pix_fmt", "yuv420p",
                "-c:v", "libx265", "-preset", "fast" if embedded else "veryfast",
                "-crf", "28" if embedded else "26",
                "-x265-params", f"keyint={fps}:min-keyint={fps}:scenecut=0:bframes=0:"
                f"repeat-headers=1:log-level=error:pools={2 if embedded else 4}", "-f", "hevc", "pipe:1"])
    checksums = run(["-f", "hevc", "-i", "pipe:0", "-c:v", "copy", "-f", "framecrc", "pipe:1"], hevc)
    output, position, count = bytearray(), 0, 0
    for line in checksums.decode("ascii").splitlines():
        if line.startswith("#"):
            continue
        columns = line.split(",")
        length, checksum = int(columns[4]), int(columns[5], 16)
        data = hevc[position:position + length]
        if len(data) != length or zlib.adler32(data, 0) != checksum:
            raise ValueError("HEVC access-unit boundary mismatch")
        output.extend(struct.pack(">I", length))
        output.extend(data)
        position += length
        count += 1
    if position != len(hevc) or not fps <= count <= 4 * fps:
        raise ValueError(f"Expected one to four seconds of complete access units; found {count}")
    destination.write_bytes(output)
    return {"file": destination.name, "width": width, "height": height, "fps": fps,
            "frames": count, "bytes": len(output), "sha256": hashlib.sha256(output).hexdigest()}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--client", type=Path, required=True, help="client source tree")
    cli.add_argument("--compression-war", type=Path, required=True, help="ffmpeg and source footage")
    cli.add_argument("--out", type=Path, required=True)
    cli.add_argument("--profile", choices=("embedded", "small-third", "benchmark"), required=True,
                     help="embedded creates all three built-in recordings; small-third updates only the third")
    args = cli.parse_args()
    ffmpeg = args.compression_war / "ffmpeg.exe"
    # Same three subjects for every benchmark mode. The output rate is for
    # decoder/render load; repeated source exposures are possible at 30 Hz.
    raw = args.compression_war / "素材/car_raw.bgr"
    raw_format = ["-f", "rawvideo", "-pixel_format", "bgr24", "-video_size", "1280x1024", "-framerate", "20"]
    inputs = [("official feed (测试画面.h265)", 0, (480, 270),
               ["-f", "hevc", "-i", str(args.client / "tests/fixtures/测试画面.h265")])]
    for start in (5, 10):
        inputs.append(("industrial camera (car_raw.bgr)", start, (400, 320),
                       [*raw_format, "-ss", str(start), "-i", str(raw)]))
    args.out.mkdir(parents=True, exist_ok=True)
    clips = []
    embedded = args.profile != "benchmark"
    for i, (title, start, embedded_size, source) in enumerate(inputs, 1):
        if args.profile == "small-third" and i < 3:
            continue
        size = embedded_size if embedded else (1920, 1080)
        result = encode(ffmpeg, source, args.out / f"peer-view-{i}.bin", *size, embedded=embedded)
        result.update(source=title, source_start_seconds=start)
        clips.append(result)
        print(result, flush=True)
    if args.profile == "benchmark":
        (args.out / "clips.json").write_text(json.dumps(clips, indent=2) + "\n", "utf-8")


if __name__ == "__main__":
    main()
