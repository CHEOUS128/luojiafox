# Notices

## The CAD data is not ours

This repository contains **tooling only**. The field models it operates on
are the official RoboMaster University Championship (RMUC) and RoboMaster
University League (RMUL) arena CAD releases published by **DJI / RoboMaster**
for competition teams (`RMUC2026_V1.2.0.step`, `RMUC2026_V2.0.0.stp`,
`RMUL2026.stp`, `能量单元.stp`, the 2027 RMUC/RMUL releases recorded in
`source/SOURCES.json`, and the files derived from them). Those models,
their geometry, names, colours and structure are the intellectual property
of SZ DJI Technology Co., Ltd. and its RoboMaster organising committee and
are provided by them under the RoboMaster competition rules and download
terms. Obtain them from the official RoboMaster website
(https://www.robomaster.com) and keep them under those terms.

Accordingly:

- **No STEP, IGES, glTF, XBF or mesh files of the DJI models are committed
  here**, and none should ever be. `.gitignore` blocks the common formats;
  split packages, sidecar indexes and mesh bundles are regenerated from the
  official files on demand.
- `out/` is ignored, regenerable staging for local split packages, indexes,
  meshes and intermediate reports. Retained derived reports and rendered
  PNG audit images are committed under `docs/` and `benchmarks/` to explain
  and check the tooling. They remain derivatives of DJI's models and do not
  transfer ownership of the underlying CAD or change DJI's terms.
- Product names, entity ids and numbers quoted in the documentation are
  likewise taken from DJI's files.

This project is not affiliated with, endorsed by or sponsored by DJI or
RoboMaster.

## Software licence

The code and documentation in this repository (the Rust crates, the Python
prototypes, the preview builder and template, and `docs/`) are licensed
under either of

- Apache License, Version 2.0 (`LICENSE-APACHE`)
- MIT License (`LICENSE-MIT`)

at your option. Unless you explicitly state otherwise, any contribution
intentionally submitted for inclusion in this work by you shall be
dual-licensed as above, without any additional terms or conditions.

## Third-party software

| Component | Used for | Licence |
|---|---|---|
| [Open CASCADE Technology](https://dev.opencascade.org/) via [`cadquery-ocp`](https://github.com/CadQuery/OCP) | validation, tessellation and glTF writing of the split parts (Python prototypes only; not linked into the Rust crates) | LGPL-2.1 with the Open CASCADE exception |
| [three.js](https://threejs.org/) r128, loaded from cdnjs at view time | the 3D preview pages | MIT |
| Barlow, Barlow Condensed, JetBrains Mono (Google Fonts, loaded at view time) | preview page typography | SIL Open Font License 1.1 |
| Rust crates listed in `Cargo.toml` (`memmap2`, `rayon`, `memchr`, `clap`, `anyhow`, `serde`, `serde_json`, `thiserror`) | the CLI and library | MIT / Apache-2.0 |
| numpy | Python prototypes | BSD-3-Clause |
| python-zstandard and its bundled libzstd | streaming tar.zst CAD archives and read-back verification | BSD-3-Clause |

The earlier whole-file conversion recorded in `benchmarks/whole-file-reference/occt-conversion.json`
(`RMUC2026_full.glb`) was produced with Open CASCADE from the DJI file and is
subject to the same DJI terms as its source.
