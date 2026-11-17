# Project audit 2026-11 (baseline v0.51.0)

Audit pass, no engine changes. Baseline: `main` at `9034839` = tag `v0.51.0` (preservation branch
`baseline/pre-audit-2026-11-16`). Companion: [PROTECTED_SYSTEMS.md](PROTECTED_SYSTEMS.md) (per-system implementation,
defaults, limits, verification). Labels: **[O]** observed in a file or command output (path:line or command given),
**[I]** inference. This document links the existing docs instead of repeating them; where they disagree with the code it
says so.

## 1. Key numbers [O]

| Item | Value | Source |
|---|---|---|
| Commits / tags | 750 commits on `main` at v0.51.0 (+1 audit commit after); 61 tags `v0.1.0` … `v0.51.0` | `git log --oneline \| wc -l`, `git tag` |
| Development span | 2026-10-18 (`a967145`, cdc parsers) → 2026-11-16 (`9034839`, CORTAO AO) | `git log --reverse` |
| `src/` | 213 files (85 `.cpp`, 63 `.hpp`, 52 GLSL stages/libs + 13 `.glsl`), 77 728 lines, 4.6 MB | `find src -type f \| xargs wc -lc` |
| `tests/` | 27 GoogleTest files, 34 644 lines, 866 `TEST`/`TEST_F`/`TEST_P` macros; 114 self-test scenarios (`tests/selftests/*.json`) + 14 blocked ones (`tests/selftests/known_gaps/`); 28 input replays (`tests/replays/`) | `grep -c TEST(`, `ls` |
| ctest at v0.51.0 | 997 passed, `game_vulkan_rt_smoke` excluded | commit message `9034839` (not re-run here; the Baseline audit owns the re-run) |
| Version | `project(OpenUnderworld VERSION 0.51.0 LANGUAGES CXX)` | `CMakeLists.txt:2` |

## 2. Build system [O]

- CMake ≥ 3.25, C++23 (`CMAKE_CXX_STANDARD 23`, required, extensions off), `-Wall -Wextra -Wpedantic`,
  compile_commands exported (`CMakeLists.txt:1-31`).
- Options (all `option(` in the tree):

| Option | Default | Effect | Where |
|---|---|---|---|
| `OU_BUILD_TESTS` | ON | GoogleTest executables + ctest entries | `CMakeLists.txt:9` |
| `OU_SANITIZE` | OFF | ASan + UBSan (also disables IPO) | `CMakeLists.txt:10,25-30` |
| `OU_LTO` | ON | IPO for Release / RelWithDebInfo only (Jolt + FidelityFX included; `check_ipo_supported`) | `CMakeLists.txt:11-23,378-382` |
| `OU_FSR` | ON | Build AMD FidelityFX SDK 2.4.0 (FSR 3.1.6 upscaler + frame generation); needs DXC (system `dxc` or pinned download v1.9.2609); sets `OU_HAS_FSR` | `third_party/fidelityfx-sdk/CMakeLists.txt:4-24` |
| `OU_UPDATE_PUBLIC_KEY` (cache string) | `""` → `packaging/release-pubkey.hex` | Ed25519 key compiled into `ou_update` | `CMakeLists.txt:434-447` |

- Fetched dependencies: Jolt Physics v5.6.0 tarball (SHA-256 pinned, `CROSS_PLATFORM_DETERMINISTIC ON`, no AVX/AVX2/
  AVX512/F16C/FMA, RTTI on) `CMakeLists.txt:366-391`; DXC v1.9.2609 for FSR shaders when no system `dxc`.
- System packages: ZLIB, SDL3 (CONFIG), glm, libepoxy, libshaderc (pkg-config), Vulkan + glslc, OpenSSL (Crypto),
  nlohmann_json, CURL, libsodium, libav (pkg-config `LIBAV`), Qt6 ≥ 6.5 Widgets + Concurrent (optional: launcher only
  when found), GTest (`CMakeLists.txt:33-489`).
- Shader build: Vulkan GLSL → SPIR-V by `glslc --target-env=vulkan1.3 -O -mfmt=num` into `build/vk_shaders/*.inc`
  (embedded `uint32_t[]`), with permutations `scene_rt.*` (`-DOU_RT`), `water_rt.frag`, `water_tess.vert`
  (`-DOU_TESS`), `rt_indirect`/`rt_indirect_ms`/`rt_denoise`, CORTAO `main{8,12,16,32}[_ms]`, `prepass[_ms]`,
  `filter`, `temporal` (`CMakeLists.txt:109-272`). GL compiles the same shared GLSL at run time from raw-string
  `.inc` files (`CMakeLists.txt:66-108`). Translated original D3D9 shaders are compiled at load time (GL: driver;
  Vulkan: libshaderc).
- Packaging: `install()` of `ou-viewer ou-game ou-asset ou-import ou-sign ou-audio` (+ `ou-launcher` when Qt), desktop
  file and icon (`CMakeLists.txt:700-710`). AppImage via `packaging/appimage/` (Ubuntu 24.04 container, SDL 3.4.18,
  Qt 6.8.3 pinned: `packaging/appimage/Containerfile:26,40`); PKGBUILD; CI `.github/workflows/{ci,appimage}.yml`
  (ci = Debug + sanitizers on `ubuntu-latest`).

### Libraries and executables [O] (`CMakeLists.txt`)

| Target | Kind | Sources | Links (public → private) |
|---|---|---|---|
| `ou_cdc` | lib | `src/cdc` | ZLIB |
| `ou_formats` | lib | `src/formats` | ou_cdc, ou_anim |
| `ou_anim` | lib | `src/anim` | ou_cdc, glm |
| `ou_collision` | lib | `src/collision` | ou_cdc, glm |
| `ou_physics` | lib | `src/physics/Physics.cpp` | ou_collision, ou_formats → **Jolt** (private: only Physics.cpp sees Jolt) |
| `ou_render_common` | lib | `src/render/{RenderCommon,ShaderTranslate,OriginalShading,…}.cpp` | SDL3, glm, ou_formats → shaderc |
| `ou_render_gl` | lib | `src/render/gl/GlRenderer.cpp` | ou_render_common → epoxy |
| `ou_render_vk` | lib | `src/render/vk/{VkRenderer,VkFsr}.cpp` + SPIR-V | ou_render_common → Vulkan, ou_ffx (FSR) |
| `ou_audio` | lib | `src/audio` | ou_cdc, glm → SDL3 |
| `ou_importer` | lib | `src/importer` | ou_cdc, nlohmann_json → OpenSSL |
| `ou_config`, `ou_input` | lib | `src/config`, `src/platform/Input.cpp` | nlohmann_json; SDL3 |
| `ou_update` | lib | `src/update` | nlohmann_json → sodium, curl |
| `ou_locale`, `ou_video` | lib | `src/locale`, `src/video` | ou_cdc; SDL3, ou_ui, libav |
| `ou_script`, `ou_cine` | lib | `src/script`, `src/cine` | ou_formats, ou_anim, ou_collision, json; ou_script, ou_locale |
| `ou_game`, `ou_save` | lib | `src/game` (+`ai/`), `src/save` | collision, anim, formats, script, cine, input, physics; ou_game, json, zlib |
| `ou_ui`, `ou_fx`, `ou_placeholder` | lib | `src/ui`, `src/fx`, `src/scene/Placeholder.cpp` | render_common, locale, formats; formats, physics |
| `ou-game` | exe | `src/app/game/*` | ou_game ou_save ou_ui ou_render_gl ou_render_vk ou_config ou_video ou_audio ou_fx |
| `ou-viewer` | exe | `src/app/viewer/main.cpp` | render_gl, render_vk, placeholder, formats, collision, config |
| `ou-launcher` | exe (Qt 6) | `src/launcher/main.cpp` | config, input, update, importer, locale, SDL3, Qt6 |
| `ou-asset`, `ou-audio`, `ou-import`, `ou-sign`, `ou-text`, `ou-movie`, `ou-imgdiff` | exe tools | `tools/ou-*/main.cpp` | see §3 |

## 3. Architecture map [O]

LOC = `find <dir> -type f | xargs cat | wc -l` (all files incl. shaders).

| Dir | LOC (files) | Purpose | Key files / classes | Feeds | Main deps |
|---|---|---|---|---|---|
| `src/cdc` | 366 (2) | cdc containers: bigfile + `patch.000` overlay (`BigfileSet`), spec-mask language select, CRC32 path hash, CDRM, DRM v19 sections/relocs | `Cdc.hpp/.cpp` | everything | zlib |
| `src/formats` | 3 612 (12) | Asset parsers: PCD9 textures, models/terrain, level/unit (streams, IMFs, intros, portals, irradiance volume), scene build, D3D9 SM3 ShaderLib, Uber objects, AnimGraph (object state graphs), births | `Formats.hpp`, `Level.cpp` (926), `Model.cpp`, `Scene.cpp`, `Shader.cpp`, `Uber.cpp`, `AnimGraph.cpp` | viewer, game, tools | ou_cdc, ou_anim |
| `src/anim` | 1 022 (5) | Skeletons, type-2 clip decode, sampling, CPU skinning, crossfades/additive layers, Verlet hair, two-bone IK | `Anim.hpp`, `Blend.cpp`, `Hair.cpp`, `Ik.cpp` | formats, game | glm |
| `src/collision` | 866 (3) | Level collision meshes + markup polylines, BVH queries (ray/sphere/capsule), face flags, dynamic object colliders | `Collision.hpp`, `LevelCollision.cpp` | physics, game, viewer | glm |
| `src/physics` | 2 310 (2) | Object collision primitives (HInfo), mass/inertia (exe formulas), and the Jolt-backed `physics::Scene`: hinges, uber joints (1-axis, 3-axis), loose bodies (+buoyancy/waves), plates, character proxy, vehicles, ragdolls, blasts, bit-exact save state | `Physics.hpp` (`Scene`, `ObjectPhysics`, `VehicleSpec`, `RagdollDef`), `Physics.cpp` (1 900) | game, fx | Jolt 5.6.0 |
| `src/script` | 4 910 (4) | Script sections (type 8), trigger volumes/planes, runtime + natives, bounded SCMD VM (tru.exe 0x57CE50) | `Script.hpp/.cpp` (3 342), `Vm.hpp/.cpp` | game, cine | formats, json |
| `src/cine` | 658 (2) | In-engine cutscene streams (CINE): channels, cameras, actors, events, subtitles | `Cine.hpp/.cpp` | game, ou-text | script, locale |
| `src/game` | 24 516 (38) | Gameplay: `World`, Lara controller (`Player.cpp` 3 439), traversal (Climb, Traverse, Grapple, WallJump), objects/uber/movers (`Objects.cpp` 3 279), streaming (`Stream.cpp`), combat/melee/aim/gadgets, vehicle, hazards, breakables, water sim, weather, feedback (rumble/shake), IK passes, progress; `ai/` enemies + nav | `Game.hpp` (1 886), `World.cpp`, `ai/Enemy.*`, `ai/Nav.cpp` | ou-game, tests | collision, anim, formats, script, cine, input, physics |
| `src/save` | 620 (2) | Versioned JSON saves + CRC, atomic write + `.bak`, migrations, slot names | `Save.hpp/.cpp` | ou-game | ou_game, json, zlib |
| `src/render` (root) | 4 465 (shared) | Backend interface (`Renderer.hpp`, `FrameOptions`), shared helpers, SM3→GLSL `ShaderTranslate`, engine register files `OriginalShading` (lights, PSSM, irradiance), Enhanced water (`Water.*`, `Ocean.*` FFT + CPU swell, `WaterField.*` eWave, `Fluid.*` PBF), CORTAO config, `sun.glsl` | see left | ou_render_common | SDL3, glm, shaderc |
| `src/render/gl` | 2 754 (6) | OpenGL 4.6 core backend (DSA, libepoxy), GL CORTAO compute glue | `GlRenderer.cpp` (2 593), `shaders/cortao_*.comp` | ou_render_gl | epoxy |
| `src/render/vk` | 7 901 (30) | Vulkan 1.3 backend (dynamic rendering, sync2), ray queries (BLAS/TLAS), RT AO/GI, CORTAO, FSR 3.1 glue, Anti-Lag markers | `VkRenderer.cpp` (6 653), `VkFsr.cpp`, `AntiLag.hpp`, `shaders/*` | ou_render_vk | Vulkan, ou_ffx |
| `src/render/shaders` | 3 203 (33) | GLSL shared by GL + VK: Enhanced water/tessellation/ocean/eWave/PBF splash/under-water medium, atmosphere, screen filters, tonemap | see PROTECTED_SYSTEMS "Water" | both backends | — |
| `src/fx` | 2 720 (6) | Original particle effects (GenericFXObject): particles, ribbons, glows, lightning, screen filters, effect lights, instance fades; original weather; breath fog | `Fx.hpp/.cpp` (1 645), `Weather.cpp`, `Breath.cpp` | ou-game | formats, physics |
| `src/audio` | 2 601 (11) | Own software mixer on an SDL3 stream (no FMOD), FSB4/IMA/`.mul` decode, `.mus` sequencer, 8-slot music player, SoundPlex graphs, FMOD-Ex DSP stand-ins + SFX reverb | `Mixer.*`, `Dsp.*`, `Mus.cpp`, `MusicPlayer.*`, `SoundPlex.*` | ou-game, ou-audio | SDL3 |
| `src/video` | 511 (4) | Bink movies via libav (binkvideo/binkaudio_dct), SDL playback + subtitles | `Movie.*`, `Player.*` | ou-game, ou-movie | libav, SDL3 |
| `src/locale` | 351 (2) | `locals.bin` strings, languages, subtitle ticker | `Locale.*` | ui, video, launcher | ou_cdc |
| `src/ui` | 2 874 (7) | Menus (ring, slots, options, tailoring, EXTRAS), PDA, loading screens, HUD, game font/art from `generalbank.drm` | `Ui.*` (1 511), `Hud.cpp`, `Pda.*`, `Art.*` | ou-game, ou-movie | render_common, locale |
| `src/app` | 7 034 (11) | `ou-game` (`main.cpp` 3 805: loop, input, render frame assembly, menus wiring), self-test runner, autopilot, cinematic player, game sound; `ou-viewer` | `app/game/{main,SelfTest,Autopilot,Cinematic,Sound}.cpp`, `app/viewer/main.cpp` | ou-game, ou-viewer, gate/vehicle tests | all |
| `src/config` | 530 (2) | `config.json` model, presets → `GraphicsSettings`, CLI graphics flags, bindings | `Config.hpp/.cpp` | apps, launcher | json |
| `src/platform` | 294 (3) | SDL3 init RAII, input name parsing, `DeviceState` from events, record/replay event logs | `Input.*`, `Sdl.hpp` | game, launcher | SDL3 |
| `src/engine` | 51 (1) | `FrameClock`: fixed 60 Hz step, ≤ 8 steps/frame, render alpha | `FrameClock.hpp` | apps | — |
| `src/scene` | 467 (3) | `scene::SceneData` CPU contract between formats and renderers; placeholder scene | `Assets.hpp`, `Placeholder.*` | renderers | glm |
| `src/importer` | 1 095 (2) | Read-only install scan, manifest (hashes, GOG hashdb), diffs | `Importer.*` | launcher, ou-import | OpenSSL, json |
| `src/launcher` | 1 166 (1) | Qt 6 Widgets launcher (no moc): import, graphics, controls capture, saves backup, diagnostics, updates, Play/Viewer | `main.cpp` | ou-launcher | Qt6, SDL3 |
| `src/update` | 831 (2) | Ed25519-signed updater: install root swap (`RENAME_EXCHANGE`) + journal, AppImage self-update, resumable downloads | `Update.*` | launcher, ou-sign | libsodium, curl |
| `tools/` | 2 848 | `ou-asset` (431), `ou-audio` (428), `ou-sign` (174), `ou-movie` (121), `ou-text` (98), `ou-import` (97), `ou-imgdiff` (64); `ghidra/` headless scripts + `names/tru_gog.csv` (183 lines); `release.sh` (150); `ffx_shaders.py` (202, HLSL→SPIR-V for FSR) | — | dev/release | — |
| `tests/` | 34 644 + data | GoogleTest suites (`*Real*` suites need `OU_GAME_DIR`), self-test scenarios, replays, `lsan_vulkan.supp` | `test_game.cpp` (267 tests), `test_combat.cpp`, `test_gates.cpp` | ctest | GTest |
| `packaging/` | 343 | AppImage (`AppRun`, Containerfile, build/smoke scripts), PKGBUILD, desktop, icon, release public key | — | releases | — |
| `third_party/` | 52 097 | `fidelityfx-sdk` (145 files, FSR 3.1.6 subset + `ou/` patches), `cortao` (v1.0.3 @420b168, unmodified, MIT), and licence-only dirs `jolt`, `abyssal-garden`, `neuralspecocean`, `opengl-water-simulation` (code fetched or adapted into our shaders, credits in docs/RESEARCH.md) | — | vk renderer, GL AO | — |

Observed structural risks [O]: `src/app/game/main.cpp` (3 805 lines, 247 KB) and `src/render/vk/VkRenderer.cpp`
(6 653 lines, 441 KB) are monoliths; `src/game/Game.hpp` (1 886 lines) is one header for the whole gameplay layer.
[I] These are the files where concurrent agent edits most often collided (see §5 history reverts).

## 4. Git history: eras [O]

Eras grouped from `CHANGELOG.md` headings and commit counts (`git rev-list --count A..B`).

| Era | Tags (dates) | Commits | What landed |
|---|---|---|---|
| 1 Foundations | v0.1.0–v0.4.0 (2026-10-18) | 50 | cdc parsers, PCD9/meshes/levels, GL viewer, Vulkan backend, Lara controller, swimming, health/pistols, launcher + signed updater + AppImage; v0.4.0 translated original D3D9 shaders and object collision/physics doors |
| 2 Traversal + Enhanced water | v0.5.0–v0.10.0 (10-27…10-29) | 92 | Enhanced water + ripple sim (v0.5.0), free climb, grapple, ladders/pillars/perches, SCMD VM, original main menu, object anim graphs, ending from scripts |
| 3 Fidelity measured under Wine, performance, FSR/RT | v0.11.0–v0.25.0 (10-30…11-05) | 83 | tru.exe jump/capsule numbers, Godot-derived water look, FSR 3.1 + frame gen, 4K RT performance pass, RT local lights, camera collision as tru.exe, Anti-Lag, all settings in the game's menus, licence → source-available + key rotation (0.21.1) |
| 4 Jolt + systems | v0.26.0–v0.41.0 (11-06…11-09) | 178 | Jolt replaces the Havok stand-ins (0.26.0), bike on Jolt, ragdolls, weighed plates, `.mus` beat clock, language setting, nav meshes, Original preset screen filters / PSSM / fog, traversal and jumps re-measured, water colour, chromatic caustics |
| 5 Gameplay reconstruction | v0.42.0–v0.47.0 (11-10) | 239 | Audit-driven: chimney jumps, all 179 treasures, close combat, carried objects, breakables, Thor's gear, checkpoint restore, hazards, moving markup, slopes, take-off timing, key items |
| 6 Water physics + AO | v0.48.0–v0.51.0 (11-15…11-16) | 108 | Under-water medium, sun shafts, physically formed caustics, GPU eWave field, PBF splash fluid, water tessellation, tru.exe water entry, sin sea waves, Anti-Lag crash fix (0.49.2), CORTAO AO default (0.51.0) |

Non-obvious points visible only in history [O]:
- Jolt `CharacterVirtual` was built as a full second mover on branch `agent/GpCharVirtual`, failed 8/26 comparison
  tests and was removed (merge `89d3e82`, release `8c44515`; reproduction commit `84eb0be` cited in
  docs/formats/PHYSICS.md:339). Lara's mover is deliberately not Jolt.
- RT water reflection calibration (`OuRtCalib`) was added and reverted (`6ba336d`); the l4_pool rock-1253 terrain-group
  rule was reverted (`3eec584`). Neither is in CHANGELOG.
- Three commits re-applied work silently reverted by staging from a stale index (`237a89b`, `87959ff`, `434738b`,
  `ad307d0`; also `7e4a97f` "lost in 34b752d"). Process hazard for parallel agents; per-path commits are the mitigation.
- Physics save state is Jolt's `PhysicsSystem::SaveState` + our body IDs (`b175fd5`), which is why saves continue bit
  for bit; hard-coded footstep/shot waves were replaced by animation-event sounds (`133a36b`).
- `tools/release.sh` stopped running ctest by default at 0.14.0 (`--test` runs Debug + ASan); releases rely on prior
  ctest runs and hand-testing (CHANGELOG 0.14.0).

## 5. Classification buckets

Evidence column cites docs/ROADMAP.md compatibility-matrix rows (by area name, [ROADMAP](../ROADMAP.md)) and test
names. Where a row is "Tested" it means tested against ourselves unless the row says Wine/[O].

| Bucket | Item | Evidence |
|---|---|---|
| **Works** | cdc containers, textures, meshes, levels, collision decode (all 82 units) | ROADMAP rows "Bigfile…", "Textures (PCD9)", "Meshes / terrain", "Levels…", "Level collision"; `test_cdc`, `RealData.*`, `CollisionRealData.*` |
| Works | Vulkan 1.3 + OpenGL renderers at parity | "Vulkan 1.3 renderer"; `viewer_vulkan_matches_gl{,_enhanced,_agx}`, `VulkanRenderer.*`, `vulkan_lavapipe` |
| Works | Translated original shaders (5 594 programs) | "Original shaders"; `Sm3Real.EveryInstallShaderTranslates`, `OriginalShading.*` |
| Works | Unit streaming, saves bit-identical, checkpoints | "Unit streaming", "Saves", "Checkpoints…"; `StreamReal.SaveContinuesBitIdentically`, `Stream.*`, `Save.*` |
| Works | Movies, text, subtitles, cinematics | "Movies", "Localized text", "Subtitles", "In-engine cutscenes"; `VideoRealData.*`, `CineReal.*`, `movie_intro_screenshot` |
| Works | Launcher, updater, packaging | "Launcher (Qt 6)", "Signed updater", "Packaging / CI"; `launcher_self_test`, `LauncherGui.*`, `UpdateTest.*`, `AppImageUpdateTest.*` |
| Works | Fixed-step timing / render-rate independence | ROADMAP "Principles"; `FrameClock.*`, `game_render_rate_smoke`, `Game.*IndependentOfRenderRate` |
| **Partial** | Sound events (animation-event coverage, DSP stand-ins) | ROADMAP "Sound events" = Partial; `SoundPlex.*`, `AudioReal.*` |
| Partial | Script VM (bounded; handler stops remain: 314 in survey) | "Scripts" = Implemented; `ScriptVm.*`, `ScriptRealData.*` |
| Partial | Campaign by input: no walkthrough section plays end to end by self-test; 14 blocked scenarios | "Gameplay self-tests", "Gameplay reconstruction" = In progress; `tests/selftests/known_gaps/`, docs/SELFTEST.md, docs/GAMEPLAY_DEVELOPMENT_REPORT.md |
| Partial | Chapter matrix: 9 verified vs original, 145 partial, 88 missing of 661 rows | "Gameplay reconstruction"; docs/underworld_chapter_gameplay_matrix.md |
| Partial | Reference comparison: 161 rows, 49 match, 5 differ, 107 unmeasured | "Reference comparison"; docs/REFERENCE_COMPARISON.md |
| Partial | Progression gates: 251 gates, 136 pass, 9 true FAIL | "Progression gates"; `ProgressionReal.*`, docs/PROGRESSION.md |
| Partial | Natla fight by input past the entry walkway | "Natla"; `ProgressionReal.NatlaFightByInput` (entry only) |
| **Broken** | `game_vulkan_rt_smoke`: real GPU hang, permanently excluded from ctest | `CMakeLists.txt:650-667`; standing instruction |
| Broken [I, pending verification] | User-visible regressions listed by the BugInventory audit | see that agent's document (not duplicated here) |
| **Missing** | Edition support other than GOG 1.1.0.0 | ROADMAP milestone 8 |
| Missing | Temporal denoise of RT sun/local shadows; per-object motion vectors in Vulkan AO/RT reprojection | docs/RENDERING.md:207,242 |
| Missing | Custom per-option tailoring screen, original costume screen (ours replaces it) | "PLAYER TAILORING", "Lara outfits" rows |
| Missing | Grapple swing Wine trace (period, growth, release speed) | "Grapple inputs and swing", "Reference comparison" |
| **Improved beyond original** | Vulkan RT (shadows, reflections, local lights, AO, GI), CORTAO AO, FSR 3.1 + FG, Anti-Lag, MSAA/FXAA/HDR/AgX | PROTECTED_SYSTEMS "Ray tracing", "Ambient occlusion", "Post-processing" |
| Improved | Enhanced water (FFT ocean, eWave, PBF, tessellation, caustics, under-water medium), atmosphere, enhanced weather | ROADMAP "Enhanced water", "Water detail", "Enhanced atmosphere", "Enhanced weather" |
| Improved | Jolt physics (deterministic, saved bit for bit), ragdolls | "Jolt Physics backend"; `Physics.*`, `StreamReal.RagdollSaveLoad` |
| Improved | Self-tests, replays, autopilot, signed updater, launcher | "Gameplay self-tests"; `SelfTest.*`, `game_selftest_*` |
| **Must-protect** | All of [PROTECTED_SYSTEMS.md](PROTECTED_SYSTEMS.md); especially fixed-step determinism, save bit-identity, translated shader path, Original preset byte-identity, GL/VK parity | `game_render_rate_smoke`, `StreamReal.SaveContinuesBitIdentically`, `Physics.SavedSceneContinuesBitIdentically`, `viewer_vulkan_matches_gl*`, `Config.GraphicsPresets` |
| **Needs RE** | Script handler stops / remaining natives; collision face flags/material/client flags; extern/resource reloc semantics; Havok plate motor numbers; Lara death ragdoll rules; camera-manager transitions; sound animation events | ROADMAP rows "Scripts", "Level collision", "DRM v19", "Jolt" step 1/3, "Scripted reveal cameras", "Sound events" |
| Needs RE (Wine measurement) | 107 unmeasured comparison rows; chimney rebound, grapple swing, bike top speed / crashes / water | docs/REFERENCE_COMPARISON.md; ROADMAP "Motorcycle against the original" |
| **Should repair** | Doc drift: docs/RENDERING.md:1110 says Original Fidelity is the default (code default is `enhanced`, `Config.hpp:25`); docs/RENDERING.md:290-291 says irradiance-volume programs keep generic shading, while docs/formats/SHADERS.md:90,128 and `OriginalShading.hpp:86-93` implement IrradianceState; ROADMAP "Materials" row says "No shader translation" (superseded by the "Original shaders" row); ROADMAP "Subtitles" says no language option (superseded by `game.language`) | files cited |
| Should repair | Vulkan AO/RT reprojection is camera-only (moving yacht risk); GL uses per-pixel motion | docs/RENDERING.md:152 vs 281 |
| **Safe to reimplement** [I] | Tools (`ou-asset`, `ou-imgdiff`, …), the launcher UI layer, docs; internal structure of `main.cpp` / `VkRenderer.cpp` splits, provided the must-protect tests stay green | no gameplay or format semantics live there; covered by `LauncherGui.*`, `viewer_*`, `game_*` ctest entries |

## 6. Open questions

- Re-run of the full ctest at v0.51.0 is owned by the Baseline audit; this document uses the count from `9034839`.
- Which docs/ROADMAP rows still carry superseded "Known deviations" text beyond the four listed under Should repair
  (a row-by-row doc-drift pass was not done here).
- Whether Vulkan AO/RT camera-only reprojection is visible on moving bodies in practice (GL needed per-pixel motion for
  the yacht; docs/RENDERING.md:283 measured no grain on Vulkan at the cliffclimb spawn).
