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
| Levels: unit, stream groups, IMFs, intros | Implemented | `ou-viewer --level` screenshots of l4_pool (pool surface fills pit, props on ledges) and l4_ballcourt (IMF foliage + intro plants on terrain); structure parsed in all 82 units; objects hidden at birth (oflags 0x800000 → instance flag 0x800, tru.exe 0x5BCC30) skipped (docs/formats/LEVELS.md) | `Level.*` (synthetic), `RealData.Level*` | `co_monitor_frame` / `fx_*` proxies still drawn; object model 0 only; no skydome; no in-game screenshot comparison |
| Skeleton, skin weights, animation clips (type 2), CPU skinning | Tested | lara.drm: 116-segment skeleton + 37 hair-chain bones, symmetric L/R; all 1744 clips decode exactly to their 0x3C end offset; bind pose through skinning = stored T-pose; viewed `ou-viewer --anim ng_run/ng_idle/...` screenshots (GL + Vulkan) show plausible poses (docs/formats/ANIMATION.md) | `Anim.*` (synthetic decode, sampling, interpolation, skinning), `AnimReal.*` | Hair rigid (no simulation); root motion reset in viewer; extra channels, scale order and multi-section clips unverified; only Lara's DRM exercised; no in-game comparison |
| Level collision + markup, BVH queries | Tested | Unit terrain-group meshes + markup2 polylines decode in all 82 units (875 749 faces, 1 779 polylines, 0 warnings); `ou-viewer --show-collision` l4_pool screenshots (GL and Vulkan, identical framing): wireframe on render edges, ledge markup on ledge stones; l4_pool floor probes: 88.6 % hit render terrain within ±64, median \|Δz\| 8.4 (docs/formats/COLLISION.md) | `Collision.*` (synthetic, brute-force cross-check), `CollisionRealData.*` | Face flags/material/client flags unknown; moving terrain groups at rest pose; object-DRM markup and Level+0x80 game use not traced |
| Launcher (Qt 6) + shared config | Implemented | Live Wayland screenshot with real import result (docs/LAUNCHER.md) | `launcher_self_test` (offscreen), `Config.*` | Launch button / save restore not GUI-exercised |
| Signed updater + ou-sign | Tested | Ed25519 (libsodium), stage + RENAME_EXCHANGE swap, journal recovery; AppImage self-update (verified download, `.bak`, atomic rename); live: published 0.1.0 reports up to date over HTTPS, a 0.0.9 AppImage updated itself to 0.1.0 from the GitHub release (docs/UPDATES.md) | `ou_update_tests` (14, file-backed transport, incl. `AppImageSelfUpdate`) | GUI update buttons not clicked (same code path as `--update`); download buffered in memory |
| Packaging / CI / releases | Tested | AppImage (`packaging/appimage/build-appimage.sh`, pinned + sha256-verified tools, host GPU stack excluded) runs l4_pool in `game` (GL) and `viewer` (Vulkan) from /tmp under `env -i`; v0.1.0 published by `tools/release.sh` (signed manifest, 122/122 ctest); GitHub CI `ci` + `appimage` green; PKGBUILD (docs/RELEASING.md) | `launcher_self_test`, release.sh download-back check, CI smoke | AppImage only tested on this Arch host; needs a glibc/libstdc++ as new as Arch's; CI AppImages unsigned |
| Vulkan 1.3 renderer | Tested | Placeholder, lara.drm, l4_pool.drm match GL (mean abs diff ≤ 0.015/255); validation layer clean (docs/RENDERING.md) | `viewer_vulkan_*`, `VulkanRenderer.*` | RT detected only; interactive drag-resize not hand-tested |
| Materials (blend/alpha test/two-sided) + presets Original / Enhanced (4x MSAA, sun shadow map, FXAA, HDR tonemap) in GL and Vulkan | Tested | Material blob +0x38/+0x40/+0x4C decoded from tru.exe 0x527A80/0x5066A0, 2248-material survey, ShaderLibs are D3D9 SM3 (docs/formats/MATERIALS.md); viewed l4_ballcourt screenshots: black/opaque foliage fixed, Enhanced shadows (GL + VK); GL/VK mean \|Δ\| ≤ 0.25/255; frame times in docs/RENDERING.md | `Model.MaterialStateFromBlob`, `Config.GraphicsPresets`, `RealData.LevelBallcourtStreamGroupsAndImfs`, `VulkanRenderer.*`, `viewer_vulkan_matches_gl_enhanced` | No shader translation: no light maps, fog, wind, material tints; our shadow map ≠ original PSSM; no in-game comparison |
| Movies (Bink via libav) + `ou-movie` | Tested | All 5 `Movies/*.bik` probed; intro.bik frame 300 screenshot viewed; 10 s A/V drift mean 10.6 ms, max 21 ms, no growth; German prevontr track bit-identical to ffmpeg CLI; exe track rule (id = language, else first) (docs/formats/VIDEO.md) | `VideoRealData.*`, `movie_intro_screenshot` | System TTF font instead of the game font; skip key not auto-tested; no side-by-side with the original |
| Localized text (locals.bin) + `ou-text` | Tested | All 6 variants decode (5136 ids, 1448 non-empty, strict UTF-8); exe getter semantics (out of range / empty / invalid UTF-8); id sources traced in exe (docs/formats/TEXT.md) | `Strings.*`, `LocaleRealData.AllLocalsBinVariantsDecode` | Game font / button-glyph rendering not implemented |
| Subtitles: movie `.sch` + cinstream `.mul` | Tested | prevontr.sch rendered in ou-movie (EN, RU screenshots viewed); CINE stream header/frames parse in all 53 cinematics, 3253 subtitle entries; exe two-line ticker ported | `MovieSchema.*`, `Subtitles.*`, `Cine.*`, `LocaleRealData.*` | Cinematic channel data (animation/camera) not decoded; ticker not shown in-game yet |
| Audio: Wave (FSB4 IMA), `.mul` voice/music/cinematic streams, voice subtitles, SDL3 mixer (`ou-audio`) | Tested | `ou-audio scan` decodes all 41 243 Wave sections, 5835 `.mul`, 75 FSB4 `.sam` with 0 failures; tru.exe `.mul` IMA decoder (0x5957A0) ported; spectrograms viewed (Lara line sc_04_01_answer EN: speech harmonics/sibilants, F0 187 Hz; thrall grunt Wave; l4_pool music); voice subtitle == locals.bin string; played through SDL (pipewire) (docs/formats/AUDIO.md) | `Ima.*` (known vectors), `Fsb4.*`, `Wave.*`, `Mul.*` (synthetic + truncation), `Mixer.*` (pan, 3D, loop, crossfade, stems, SDL dummy device), `AudioReal.*` | Mus! music sequencer not implemented (stems mixed by caller gains); FSB IMA rounding assumed Xbox-IMA; no reverb/FMOD effects; linear resampling; no in-game A/B |
| Lara controller: idle/walk/run/turn/stop, standing + running jump, fall, land, ledge hang/shimmy/climb (`ou-game`) | Tested | Root-motion locomotion from lara.drm clips (walk 348, run 1274 units/s); exe gravity 5.72443 units/frame² and integrator `FUN_005DB370` ported; spawn at l4_pool `player` intro; viewed GL screenshots in l4_pool: standing, running mid-stride (also Vulkan), mid-jump, hanging on ledge 1253, shimmying, standing after climb-up; `--intro` plays intro.bik first; stone footsteps/landings and the unit music via ou_audio (docs/GAMEPLAY.md) | `Game.*` (wall slide, gravity/landing, edge fall, steps, ledge grab/shimmy/drop, 30/60/144 Hz replay identity), `GameReal.*` (clips, l4_pool run replay on floor, ledge grab + climb) | Jump speeds, grab window, step limits, camera distance inferred (exe reads them from lara.drm tuning, not decoded); no clip blending; turn ignores root rotation; no crouch/roll/swim/slopes/combat/damage; no in-game comparison |
| Third-person follow camera | Implemented | Orbit with sphere-swept pull-in against level collision, FOV from config; viewed in all screenshots above | — (exercised by `ou-game` replays) | Distance/height/pitch inferred; exe LaraCamera data-driven, no auto-recentre |
| Input: keyboard/mouse/gamepad mapping, `--input-script` replays | Tested | SDL3 mapping; scripted replays drive the screenshots above | `Game.InputScriptParse`, `Game.SimulationIndependentOfRenderRate` | Bindings not configurable; live keyboard/mouse/gamepad play not exercised by an automated or recorded run |
| Other gameplay (combat, swimming, puzzles, saving, AI) | Research Required | — | — | — |

## Principles

- Fixed-step simulation (60 Hz initial hypothesis, to be verified against the original main loop) with render interpolation.
- Gameplay/simulation independent of the render backend; both renderers consume `src/scene/Assets.hpp` data.
- Original Fidelity and Enhanced rendering modes; ray tracing optional with raster fallback.
- Original installation is read-only; no copyrighted data in the repo or releases.
