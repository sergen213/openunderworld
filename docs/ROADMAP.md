# Roadmap and status

Status vocabulary: **Research Required → Designed → Implemented → Tested → Verified Against Original Behavior**.
A feature is only "Tested" with automated tests in `tests/`, and only "Verified" with a recorded comparison to the original game.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Environment, project audit, research, repo + docs | In progress |
| 2 | Original game analysis: inventory, Ghidra, verified parsers | In progress |
| 3 | Native rendering: SDL3 + OpenGL viewer of original assets, debug camera | In progress |
| 4 | Engine foundations: scene/streaming, animation, collision, character control | Research Required |
| 5 | Playable section: traversal, puzzles, combat, saving | Research Required |
| 6 | Vulkan backend, enhanced rendering, optional RT | Research Required |
| 7 | Qt launcher, packaging, CI, signed updates | Research Required |
| 8 | Edition compatibility, fidelity verification, release | Research Required |

## Compatibility matrix

| Area | Status | Evidence | Tests | Known deviations |
|---|---|---|---|---|
| Bigfile index + split parts | Tested | Index parsed for 7339 records; 0x7FF00000 part alignment confirmed by CDRM magics | `test_cdc` | — |
| Path hash (CRC32) | Tested | Known names resolve in bigfile index | `test_cdc` | — |
| CDRM decompression | Tested | `pc-w\lara.drm` → 53.6 MB | `test_cdc` (synthetic, truncated, corrupt) | — |
| DRM v19 sections/relocs | Tested | Reloc table sizes match on all 1396 reloc-bearing sections of lara.drm | `test_cdc` | extern/resource reloc semantics unverified |
| Textures (PCD9) | Research Required | Header observed | — | — |
| Meshes / terrain | Research Required | — | — | — |
| Everything gameplay | Research Required | — | — | — |

## Principles

- Fixed-step simulation (60 Hz initial hypothesis, to be verified against the original main loop) with render interpolation.
- Gameplay/simulation independent of the render backend; both renderers consume `src/scene/Assets.hpp` data.
- Original Fidelity and Enhanced rendering modes; ray tracing optional with raster fallback.
- Original installation is read-only; no copyrighted data in the repo or releases.
