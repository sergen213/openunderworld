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
| 6 | Vulkan backend, enhanced rendering, optional RT | In progress (Vulkan 1.3 backend at GL parity; RT detection only) |
| 7 | Qt launcher, packaging, CI, signed updates | In progress |
| 8 | Edition compatibility, fidelity verification, release | Research Required |

## Compatibility matrix

| Area | Status | Evidence | Tests | Known deviations |
|---|---|---|---|---|
| Bigfile index + split parts | Tested | Index parsed for 7339 records; 0x7FF00000 part alignment confirmed by CDRM magics; exe spec-mask rule `(rec & m) == m` (default English) and patch.000-first `BigfileSet` | `test_cdc` (synthetic spec/patch), `RealData.PatchOverridesBigfile` | Language only selectable by mask; no Dutch/locale mapping |
| Path hash (CRC32) | Tested | Known names resolve in bigfile index | `test_cdc` | — |
| CDRM decompression | Tested | `pc-w\lara.drm` → 53.6 MB | `test_cdc` (synthetic, truncated, corrupt) | — |
| DRM v19 sections/relocs | Tested | Reloc table sizes match on all 1396 reloc-bearing sections of lara.drm | `test_cdc` | extern/resource reloc semantics unverified |
| Textures (PCD9) | Research Required | Header observed | — | — |
| Meshes / terrain | Research Required | — | — | — |
| Levels: unit, stream groups, IMFs, intros | Implemented | `ou-viewer --level` screenshots of l4_pool (pool surface fills pit, props on ledges) and l4_ballcourt (IMF foliage + intro plants on terrain); structure parsed in all 82 units (docs/formats/LEVELS.md) | `Level.*` (synthetic), `RealData.Level*` | Editor proxy meshes (trigger volumes etc.) drawn; object model 0 only; no skydome; no in-game screenshot comparison |
| Skeleton, skin weights, animation clips (type 2), CPU skinning | Tested | lara.drm: 116-segment skeleton + 37 hair-chain bones, symmetric L/R; all 1744 clips decode exactly to their 0x3C end offset; bind pose through skinning = stored T-pose; viewed `ou-viewer --anim ng_run/ng_idle/...` screenshots (GL + Vulkan) show plausible poses (docs/formats/ANIMATION.md) | `Anim.*` (synthetic decode, sampling, interpolation, skinning), `AnimReal.*` | Hair rigid (no simulation); root motion reset in viewer; extra channels, scale order and multi-section clips unverified; only Lara's DRM exercised; no in-game comparison |
| Level collision + markup, BVH queries | Tested | Unit terrain-group meshes + markup2 polylines decode in all 82 units (875 749 faces, 1 779 polylines, 0 warnings); `ou-viewer --show-collision` l4_pool screenshots: wireframe on render edges, ledge markup on ledge stones; l4_pool floor probes: 88.6 % hit render terrain within ±64, median \|Δz\| 8.4 (docs/formats/COLLISION.md) | `Collision.*` (synthetic, brute-force cross-check), `CollisionRealData.*` | Face flags/material/client flags unknown; moving terrain groups at rest pose; object-DRM markup and Level+0x80 game use not traced; GL overlay only until the Vulkan side lands |
| Launcher (Qt 6) + shared config | Implemented | Live Wayland screenshot with real import result (docs/LAUNCHER.md) | `launcher_self_test` (offscreen), `Config.*` | Launch button / save restore not GUI-exercised |
| Signed updater + ou-sign | Tested | Ed25519 (libsodium), stage + RENAME_EXCHANGE swap, journal recovery (docs/UPDATES.md) | `ou_update_tests` (12, file-backed transport) | Live HTTPS server untested; no release server yet |
| Packaging / CI | Implemented | install rules, .desktop, icon, PKGBUILD (local `makepkg -f` built + 71/71 check), Arch CI workflow | — | CI never run on GitHub |
| Vulkan 1.3 renderer | Tested | Placeholder, lara.drm, l4_pool.drm match GL (mean abs diff ≤ 0.015/255); validation layer clean (docs/RENDERING.md) | `viewer_vulkan_*`, `VulkanRenderer.*` | No enhanced mode, RT detected only; interactive drag-resize not hand-tested |
| Everything gameplay | Research Required | — | — | — |

## Principles

- Fixed-step simulation (60 Hz initial hypothesis, to be verified against the original main loop) with render interpolation.
- Gameplay/simulation independent of the render backend; both renderers consume `src/scene/Assets.hpp` data.
- Original Fidelity and Enhanced rendering modes; ray tracing optional with raster fallback.
- Original installation is read-only; no copyrighted data in the repo or releases.
