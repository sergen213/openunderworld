# Protected systems (baseline v0.51.0, `9034839`)

Every system below works at v0.51.0 and must not regress during the audit / RE / repair phases. For each: where it is
implemented, its config keys / CLI flags with defaults, what works, known limits, and the existing tests that prove it
still works. Overview and buckets: [PROJECT_AUDIT_2026-11.md](PROJECT_AUDIT_2026-11.md).

Labels: **[O]** observed (path:line or command), **[I]** inference. Class: **Original-faithful** (reproduces tru.exe
from code/data/Wine evidence), **Enhancement** (beyond the original, switchable), **Mixed**.

Config file: `$XDG_CONFIG_HOME/openunderworld/config.json`, model and defaults in `src/config/Config.hpp`
(`GraphicsConfig`, `InputConfig`, `GameConfig`, `Config`); presets resolved by `config::resolve` (`Config.cpp`);
shared CLI graphics flags by `config::applyGraphicsFlag` (`Config.hpp:121-128`). Defaults below are the code's
(`Config.hpp:22-71`, `144-175`), not the docs'.

Standing verification rules: run ctest excluding `game_vulkan_rt_smoke` (real GPU hang); `*Real*`, `game_*` tests need
`OU_GAME_DIR="/mnt/1st4TB/Tomb Raider Underworld"` and a display (else exit 77 = skipped); GPU runs on a private
`kwin_wayland --virtual` session.

## Quick index

| System | Class | Primary verification |
|---|---|---|
| SDL3 platform layer | Original-faithful behaviour on a new platform | `FrameClock.*`, `Game.ControlBindings`, `game_input_replay_smoke`, `Mixer.PlaysThroughSdlDummyDevice` |
| Fixed-step timing | Original-faithful (60 Hz [I] vs exe 30 Hz chart frames) | `game_render_rate_smoke`, `Game.SimulationIndependentOfRenderRate` |
| Jolt physics | Mixed (Havok replaced; data from exe) | `Physics.*`, `PhysicsRealData.*`, `VehicleReal.*`, `StreamReal.RagdollSaveLoad` |
| Lara mover / collision | Original-faithful | `GameReal.StairsWalkAndRunUpAndDown`, `Collision.*`, self-tests `locomotion`, `ledge` |
| Vulkan renderer | Enhancement (backend) | `VulkanRenderer.*`, `viewer_vulkan_matches_gl*`, `vulkan_lavapipe` |
| OpenGL renderer | Enhancement (backend, reference path) | `viewer_placeholder_screenshot`, `viewer_vulkan_matches_gl*` |
| Shader pipeline (translated SM3) | Original-faithful | `Sm3Real.*`, `Sm3Parser.*`, `OriginalShading.*` |
| Water (Original / Enhanced) | Mixed | `Ocean.*`, `WaterField.*`, `WaterSurface.*`, `SplashFluid.*`, `VulkanRenderer.WaveFieldRunsScrollsAndRests`, `Game.Water*` |
| Ambient occlusion | Enhancement | `render_ao_{opengl,vulkan,rt_vulkan,parity}`, `render_golden_bedroom_enhanced_vk_ao_*`, `Config.GraphicsPresets` |
| Ray tracing | Enhancement | `VulkanRenderer.RayTracing*`, `VulkanRenderer.ResidentMeshesKeepBlases` |
| Shadows (CSM / PSSM / RT) | Mixed | `OriginalShading.PssmSplitsMatchOriginal`, `Config.GraphicsPresets` |
| Reflections (planar / SSR / RT) | Mixed | `VulkanRenderer.RayTracing*`, `Config.WaterQuality` |
| Post: MSAA/FXAA/HDR/tonemap/filters/FSR/Anti-Lag | Mixed | `Config.Tonemap`, `viewer_vulkan_matches_gl_agx`, `VulkanRenderer.ScreenFeedbackUsesPreviousFrame`, `AntiLag.*`, `vulkan_lavapipe` |
| Materials / textures | Original-faithful | `Model.MaterialStateFromBlob`, `Texture.*`, `RealData.LaraTexturesAndModel` |
| Irradiance probes / lights | Original-faithful | `OriginalShading.KrakenHeadIrradianceMatchesOriginal`, `OriginalShading.*Light*` |
| Effects (`src/fx`) | Original-faithful | `Fx.*`, `FxReal.*` |
| Audio | Mixed (own mixer replaces FMOD) | `Mixer.*`, `Mus.*`, `MusicPlayer.*`, `SoundPlex.*`, `AudioReal.*`, `Dsp.*` |
| Animation (clips, blend, IK, hair, anim graphs) | Original-faithful | `Anim.*`, `AnimReal.*`, `GameReal.Ik*`, self-tests `ik_pose`, `ik_feet` |
| Camera (follow + reveal) | Original-faithful | `Game.Camera*`, `GameReal.RevealCameraPlaysUnitSpline`, `GameReal.SeaCircleCameraLevelsOut` |
| Input (kb/mouse/pad, rumble) | Original-faithful | `Game.ControlBindings`, `Game.SticksAndMenuNavigationRebind`, `Game.ReplayedDeviceSessions`, `Rumble.*` |
| UI / menus / HUD / PDA | Original-faithful | `Menus.*`, `MenusReal.*`, `Hud.*`, `PdaReal.*` |
| Save / load | Mixed (own format, original slots/rules) | `Save.*`, `Stream.*`, `StreamReal.SaveContinuesBitIdentically` |
| Script VM | Original-faithful (bounded) | `ScriptVm.*`, `ScriptRuntime.*`, `ScriptRealData.*`, `ProgressionReal.*` |
| Cinematics / movies | Original-faithful | `Cine.*`, `CineReal.*`, `VideoRealData.*`, `movie_intro_screenshot` |
| Launcher (Qt 6) | Enhancement (tooling) | `launcher_self_test`, `LauncherGui.*` |
| Updater (Ed25519) | Enhancement (tooling) | `UpdateTest.*`, `AppImageUpdateTest.*`, `UpdateCore.*` |
| AppImage packaging / release | Enhancement (tooling) | `packaging/appimage/smoke-in-container.sh`, `vulkan_lavapipe` |
| Self-test system | Enhancement (tooling) | `SelfTest.*`, `game_selftest_*` (114), `game_replay_*` |

---

## SDL3 platform layer — Original-faithful behaviour, new platform

- **Version** [O]: dev builds use the system SDL3 (`find_package(SDL3 REQUIRED CONFIG)`, no minimum,
  `CMakeLists.txt:49`); AppImage bundles SDL 3.4.18 (`packaging/appimage/Containerfile:26`).
- **Init**: `platform::SdlInit` RAII (`src/platform/Sdl.hpp:10-13`); ou-game and ou-viewer
  `SDL_INIT_VIDEO | SDL_INIT_GAMEPAD` (`src/app/game/main.cpp:2197`, `src/app/viewer/main.cpp:312`), audio subsystem on
  demand (`src/audio/Mixer.cpp:19`, movies `main.cpp:2204`); launcher `SDL_INIT_GAMEPAD` only (`src/launcher/main.cpp:1147`).
  No display → exit 77 (ctest skip).
- **Window**: `SDL_CreateWindow` (`main.cpp:2266`; recreated on a live renderer switch `main.cpp:3276`; movies get a
  plain window `main.cpp:2205`). Config `width 1280`, `height 720`, `fullscreen false`, `vsync true`, `fov 60`,
  `renderer "auto"` (`Config.hpp:160-166`).
- **Event loop**: `SDL_PollEvent` (`main.cpp:3046`), gamepad hotplug via `SDL_EVENT_GAMEPAD_ADDED` → `SDL_OpenGamepad`
  (`main.cpp:3049-3050`); every device event becomes `platform::DeviceState` (`src/platform/Input.hpp:1-3`), so live
  play, `--record-input` and `--replay-input` share one path. Anti-Lag INPUT marker sits right before the poll
  (docs/RENDERING.md "Anti-Lag").
- **Audio device**: `SDL_OpenAudioDeviceStream(DEFAULT_PLAYBACK, F32 stereo)` with a pull callback
  (`src/audio/Mixer.cpp:17-27`); movies open their own stream (`src/video/Player.cpp:33`).
- **Rumble**: `SDL_RumbleGamepad` from `game::Rumble` (`main.cpp:3349`, `src/game/Feedback.hpp:39`), config
  `controls.vibration true`.
- **Timing**: see next section.
- **Limits**: Wayland-native launcher GUI not automated; real pad capture unexercised in tests (ROADMAP "Launcher").
- **Verify**: `Game.ControlBindings`, `Game.SticksAndMenuNavigationRebind`, `Game.ReplayedDeviceSessions`,
  `game_input_replay_smoke` (keyboard+mouse and gamepad with hotplug), `Mixer.PlaysThroughSdlDummyDevice`, `Rumble.*`.

## Fixed-step timing — Original-faithful [I on rate]

- `engine::FrameClock` (`src/engine/FrameClock.hpp:1-20`): fixed step, ≤ 8 steps per frame (excess dropped), render
  alpha; `game::kTickSeconds = 1/60` (`src/game/Game.hpp:49`); loop `clock.advance(dt)` (`main.cpp:3066`).
  `--render-hz` paces the render rate for tests. Rule: nothing gameplay-relevant reads frame time or the rendered camera
  (docs/ROADMAP.md "Principles", docs/GAMEPLAY.md "Timing"). CPU FFT swell (not GPU readback) feeds gameplay water.
- **Limits**: 60 Hz is a project choice [I]; tru.exe's player chart runs on 30 Hz frames (docs/re/TIMING.md).
- **Verify**: `FrameClock.*`, `game_render_rate_smoke` (30 vs 240 Hz same final tick), `Game.SimulationIndependentOfRenderRate`,
  `Game.CameraForcesIndependentOfRenderRate`, `Physics.DeterministicAcrossRenderRates`, `VehicleReal.RideIndependentOfRenderRate`.

## Jolt Physics — Mixed

- **Version / build** [O]: Jolt 5.6.0 via FetchContent, SHA-256 pinned, `CROSS_PLATFORM_DETERMINISTIC ON`, no AVX+,
  single-threaded, linked PRIVATE so only `src/physics/Physics.cpp` sees Jolt (`CMakeLists.txt:366-391`).
- **What Jolt does** (`physics::Scene`, `src/physics/Physics.hpp:296-410`): hinge doors (`addHinge`), uber joints
  1-axis (`setJoints`, links, motors, range watches triggers 14/15) and 3-axis (`setJoints3`, Thor pillars, push
  blocks), loose bodies (`setLoose`: thrown/dropped objects, freed uber sections, breakable pieces, chunkdolls,
  **floating yachts** with buoyancy + waves via `setWater`, `src/game/Objects.cpp:54,2794-2880`), pressure plates
  weighed by contacts and kinematic weights (`setPlates`), Lara's push proxy capsule (`setCharacter`: shoves loose
  bodies/ragdolls only), explosions (`blast`, `knock`), the motorcycle (`setVehicles`: `VehicleConstraint`, ray wheels,
  tru.exe engine curve), ragdolls (`setRagdolls`, from ObjectBase +0x7C records), terrain as per-unit static meshes
  (`setTerrain`) and kinematic object colliders (`setColliders`); bit-exact saves via `PhysicsSystem::SaveState`
  (commit `b175fd5`).
- **What Jolt does not do** [O]: Lara's character controller. She keeps the own kinematic capsule sweep on our BVH
  (`src/game/Player.cpp` `slide` / `groundMove` / `airMove`); Jolt `CharacterVirtual` was tried and rejected
  (docs/formats/PHYSICS.md:332-339). Level collision for Lara, camera, rope and bike probes = `src/collision`.
- **Config**: none (always on). Mapping and evidence: docs/formats/PHYSICS.md "Havok → Jolt", ROADMAP "Planned: Jolt".
- **Limits**: Jolt limits settle within a few units; fixed joints / body A motion not done; plate Havok motor numbers
  [I]; bike air torques partly, lean servo; Lara death ragdoll rules [U/I] (ROADMAP:121-139).
- **Verify**: `Physics.*` (15), `PhysicsRealData.*` (6), `Physics.SavedSceneContinuesBitIdentically`,
  `Physics.LooseBodiesFloatOrSinkInWater`, `Physics.FloatingSphereRidesTheWaves`, `StreamReal.Yacht*`,
  `StreamReal.RagdollSaveLoad`, `GameReal.KilledPoacherRagdollIndependentOfRenderRate`, `VehicleReal.*`,
  `ProgressionReal.*`, `game_replay_baldrs_plates_gate`, `game_render_rate_smoke`.

## Vulkan renderer — Enhancement (backend)

- `src/render/vk/VkRenderer.cpp` (6 653 lines) behind `render::Renderer` (`src/render/Renderer.hpp`). Vulkan 1.3,
  dynamic rendering, sync2, 2 frames in flight, linear block allocator, pipeline cache in `$XDG_CACHE_HOME`,
  validation via `OU_VK_VALIDATION`; design table docs/RENDERING.md:157-180.
- **Config**: `renderer "auto"` = Vulkan when a 1.3 device is usable, else OpenGL (`render::selectRenderer`);
  `--renderer opengl|vulkan`. Env: `OU_VK_NO_RT`, `OU_VK_NO_FSR`, `OU_VK_CAPTURE_DIR`, `OU_VK_PRESENT_STATS`.
- **Works**: GL parity (mean |Δ| ≤ 0.015/255 placeholder; translated ≤ 1.34/255), RT, FSR, Anti-Lag, CORTAO, lavapipe.
- **Limits**: `upload()` / swapchain recreation `vkDeviceWaitIdle`; readback stalls (test path).
- **Verify**: `VulkanRenderer.*` (8), `RendererSelect.AutoFallsBackToOpenGl`, `viewer_vulkan_placeholder_screenshot`,
  `viewer_vulkan_matches_gl`, `_enhanced`, `_agx`, `vulkan_lavapipe` (validation + FSR on llvmpipe).

## OpenGL renderer — Enhancement (backend; portability reference)

- `src/render/gl/GlRenderer.cpp` (2 593 lines), OpenGL 4.6 core + DSA via libepoxy (docs/RENDERING.md:12). Shared GLSL
  compiled at run time; translated shaders compiled by the driver. Has: MSAA/FXAA/HDR/tonemap, CSM + PSSM, Enhanced
  water incl. tessellation, CORTAO (with per-pixel motion). Lacks: RT, FSR, Anti-Lag (Vulkan-only by design).
- **Verify**: `viewer_placeholder_screenshot`, `viewer_vulkan_matches_gl*` (GL is the reference side); all `game_*`
  ctest entries run `--renderer opengl`.

## Shader pipeline — Original-faithful

- **Translated**: D3D9 SM3 ShaderLib parser/disassembler (`src/formats/Shader.cpp`), SM3 → GLSL translator
  (`src/render/ShaderTranslate.cpp`), engine register files / light sets / PSSM / IrradianceState
  (`src/render/OriginalShading.cpp`); GL compiles the GLSL, Vulkan compiles it to SPIR-V with libshaderc at `upload()`;
  `sun.glsl` appended (sun visibility multiplies c212 only; AO scales c15, c70-c76, c148/c149). 5 594 programs from
  16 882 ShaderLibs translate and compile (ROADMAP "Original shaders").
- **Generic**: own scene shaders (`vk/shaders/scene.*`, GL `kSceneFs`) for materials without composite programs or with
  `--shaders generic`.
- **Build-time SPIR-V**: all Vulkan-own shaders by glslc (PROJECT_AUDIT §2); FSR HLSL by DXC (`tools/ffx_shaders.py`).
- **Config**: `graphics.original_shaders` unset = on (both presets); `--shaders original|generic`.
- **Limits**: docs/formats/SHADERS.md; doc drift at docs/RENDERING.md:290-291 (irradiance programs are translated now).
- **Verify**: `Sm3Parser.*`, `Sm3Translate.SyntheticProgramsCompile`, `Sm3Real.EveryInstallShaderTranslates`,
  `Sm3Real.CharacterProgramsCompile`, `Sm3Real.LevelEnvironmentAndMaterials`, `OriginalShading.*` (8).

## Water — Mixed

- **Original** (`graphics.water "original"`, or preset original): TRU's own water materials through the translated
  shaders, incl. planar reflection (`original::reflection`, docs/RENDERING.md:381) and the original sea waves (sin,
  tru.exe 0x69DA80, `game::waveHeight` in `src/game/World.cpp`, `waves.glsl ouTruWaveHeight`).
- **Enhanced** (`graphics.water "auto"` → enhanced with the enhanced preset): `src/render/Water.{hpp,cpp}` + shared GLSL.
  - Surface: `water.vert`, `water.frag` (Godot-derived base, SSR, refraction, absorption, foam), `water_surface.glsl`,
    `waves.glsl`; tessellation `water.tesc` / `water.tese` (`water_tess.vert` permutation; GL too).
  - FFT ocean: `ocean.comp` + `render/Ocean.*` (CPU `Swell` = gameplay surface; `graphics.ocean "fft"` | `"procedural"`).
  - Interactive eWave field: `water_field.comp`, `render/WaterField.*` (512² × 7.8 cm, `uRipples` h, dh/dx, dh/dy,
    whitewater), inputs from `game::WaterSim` (`src/game/Water.*`).
  - PBF splash fluid + screen-space fluid rendering: `fluid.comp`, `fluid.glsl`, `splash.vert`, `splash_splat.frag`,
    `splash_screen.vert`, `splash_blur.frag`, `splash_composite.frag`, `render/Fluid.*`; VK FSR mask `fsr_splash.frag`.
  - Caustics: `uw_caustics.vert` / `uw_caustics.frag` (caustic mesh splat into `uAgWater` g), `uw_water.frag`.
  - Under-water medium / post (both qualities for the base look): `underwater.glsl`, `uw_medium.glsl`, `uw_light.glsl`,
    `uw_particles.glsl`, `uw_snow.vert/.frag`, `uw_bubble.vert/.frag`, `uw_shafts.frag`, `uw_shafts_blur.frag`,
    `uw_bloom.frag`, `uw_grade.frag`; atmosphere `atmosphere.glsl`; `filter.glsl`, `tonemap.glsl` in the post pass.
  - RT reflections: `water_rt.frag` (= `water.frag -DOU_RT`).
- **Config**: `water "auto"`, `water_detail "auto"`, `water_color "auto"` (`kWaterColors`), `ocean "fft"`,
  `atmosphere "auto"` (→ medium), `light_shafts` unset = on, `weather "auto"`; CLI `--water`, `--water-detail`,
  `--water-color`, `--ocean`, `--water-debug`, F7.
- **Works**: GL + VK; Original preset byte-identical when Enhanced is off (ROADMAP "Water detail").
- **Limits**: field rings subtle in normal view; swell clipped at rest plane; SSR misses off-screen geometry; coarse TRU
  water meshes (docs/RENDERING.md "Water"; ROADMAP "Enhanced water").
- **Verify**: `Ocean.CpuSwellIsTheDrawnHeight`, `WaterField.DispersionAndDamping`, `WaterField.StepperScrollsRestartsAndPacks`,
  `WaterSurface.GlSourcesExpandAndCompile`, `SplashFluid.*` (4), `UnderwaterRays.DrawsColumnsInView`,
  `VulkanRenderer.WaveFieldRunsScrollsAndRests`, `WaveWindow.ScrollKeepsWallsAndAsksNewCellsOnce`, `Game.Water*`,
  `GameReal.WaterEntryDepthAndRiseMatchTru`, `StreamReal.OpenSeaWater`, `StreamReal.YachtRidesTheDrawnSwell`,
  `WaterColor.*`, `WaterColumn.*`, `Config.WaterQuality`, `FxReal.EnhancedWaterBubblesLeaveTheSprites`,
  `viewer_vulkan_matches_gl_enhanced`, self-test `water_entry`, `swan_dive`.

## Ambient occlusion — Enhancement

- **CORTAO** (default via `ao "auto"` with enhanced): `third_party/cortao` (v1.0.3 @420b168, MIT, unmodified,
  `VENDORED.txt`), shared `src/render/Cortao.{hpp,cpp}`, VK `VkRenderer::recordCortao` + `vk/shaders/cortao_*.comp`,
  GL `GlRenderer::runAmbientOcclusion` + `gl/shaders/cortao_*.comp`; composite `sun.glsl ouAo()` (ambient only).
- **RT AO** (`ao "rt"`): `vk/shaders/rt_indirect.comp` (half-res, temporal denoise), falls back to CORTAO without RT.
- **Off**: `ao "off"`; preset original `auto` = off.
- **Quality**: `ao_quality "medium"` → 8 / **12** / 16 / 32 samples (low/medium/high/ultra; 32 rays at ultra).
  CLI `--ao auto|off|cortao|rt`, `--ao-quality`; menu DISPLAY SETTINGS > QUALITY > AMBIENT OCCLUSION.
- **Other AO**: none. [O] `grep -i 'ssao|hbao|gtao'` finds only CORTAO's own `cortao_gatherSSAO` names and the plan
  line docs/RENDERING.md:1121. TRU's light maps / IrradianceState are baked lighting, not AO.
- **Limits**: VK reprojection camera-only (GL per-pixel motion); AO accumulates, so GL-vs-VK parity tests run `--ao off`.
- **Verify**: `render_ao_opengl`, `render_ao_vulkan`, `render_ao_rt_vulkan` (bounded darkening vs `--ao off`),
  `render_ao_parity` (GL vs VK with AO on), goldens `render_golden_bedroom_enhanced_vk_ao_{on,off}` (6627f6b; all in
  `ctest -L protected`); `Config.GraphicsPresets`; `RtStatus.ao` overlay line.

## Ray tracing — Enhancement (Vulkan only)

- `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` in raster shaders (no RT pipeline). One BLAS per mesh (static
  fast-trace; skinned refit; rigid rebuild), TLAS updated in place; resident units keep BLASes; alpha test in the
  query loop (docs/RENDERING.md:182-207).
- Features: sun shadows (`scene_rt.*`, 1/2/4 rays), local-light shadows (`rt_lights`), water reflections
  (`rt_reflections`, `water_rt.frag`), RT AO, one-bounce sun GI (`rt_gi`) (`rt_indirect.comp`).
- **Config**: `rt "auto"` (enhanced → shadows on RT devices), `"off" | "shadows" | "full"`; `rt_quality "medium"`;
  `rt_lights` / `rt_reflections` / `rt_gi` unset = on with `full`; explicit RT rejected with the original preset.
- **Limits**: no temporal denoise of sun shadows; camera-only reprojection; non-finite ray inputs guarded
  (`ouRayInputsFinite`); `game_vulkan_rt_smoke` hangs the GPU (excluded).
- **Verify**: `VulkanRenderer.RayTracingBuildsUpdatesAndTearsDown`, `.RayTracingTracesOneBatchLod`,
  `.RayTracingFallsBackToShadowMap`, `.ResidentMeshesKeepBlases`, `Config.GraphicsPresets`.

## Shadows — Mixed

- Enhanced: own cascaded shadow map, 3 cascades 2048² in a 6144×2048 atlas, PCF 3×3 (`src/render/sun.glsl`,
  `RenderCommon sunShadow`; docs/RENDERING.md:90-107) — Enhancement; or RT rays.
- Original preset: tru.exe's PSSM (4 splits, 4096×3072 atlas, Wine-measured) — Original-faithful; `pssm` set when
  preset original + original shaders + `shadows` unset/on (`src/config/Config.cpp:33`).
- **Config**: `shadows` unset = preset default; `--shadows on|off`.
- **Verify**: `OriginalShading.PssmSplitsMatchOriginal`, `Config.GraphicsPresets`, `viewer_vulkan_matches_gl_enhanced`.

## Reflections — Mixed

- Planar reflection of TRU's water materials (Original, `original::reflection`) — Original-faithful.
- Enhanced water SSR (thickness-tested with `water_detail`) — Enhancement; RT mirror rays replace SSR with `rt_reflections`.
- **Verify**: `VulkanRenderer.RayTracing*`, `Config.WaterQuality`, `viewer_vulkan_matches_gl_enhanced`.

## Post-processing — Mixed

| Feature | Default | Key / flag | Class | Files |
|---|---|---|---|---|
| MSAA | enhanced 4x, original 1x | `msaa`, `--msaa` | Enhancement | both backends |
| FXAA | off | `fxaa`, `--fxaa` | Enhancement | post pass |
| HDR (RGBA16F + ACES) | off | `hdr`, `--hdr` | Enhancement | `tonemap.glsl` |
| Tone mapping | `original` (AgX optional) | `tonemap`, `--tonemap` | Enhancement (AgX) | `tonemap.glsl` |
| Screen filters (ColorDOF, Finish bloom, Feedback, Noise, tints), unit fog/bloom | on | `effects` | Original-faithful | `filter.glsl`, `src/fx` |
| Brightness gamma | 1.0 | `brightness`, `--brightness` | Original-faithful | post pass |
| FSR 3.1.6 upscaling (+ RCAS) | off | `upscaler "off"`, `fsr_quality "quality"`, `sharpness 0.5`, `--upscaler`, `--fsr-quality`, `--sharpness` | Enhancement (VK) | `vk/VkFsr.*`, `third_party/fidelityfx-sdk`, `vk/shaders/fsr_*.frag` |
| FSR frame generation | off | `frame_generation`, `--frame-generation` (needs FSR) | Enhancement (VK) | same |
| AMD Anti-Lag | off | `anti_lag`, `--anti-lag` (off under frame gen) | Enhancement (VK) | `vk/AntiLag.hpp` |
| Cutscene DOF | off | `cine_dof` | Enhancement | `Cine.DepthOfFieldAsTheExe` |
| XeSS / DLSS / TAA | not implemented [O] (no hits in `src/`) | — | — | — |

- **Verify**: `Config.Tonemap`, `viewer_vulkan_matches_gl_agx`, `VulkanRenderer.ScreenFeedbackUsesPreviousFrame`,
  `Fx.ScreenFilterWeightsFadeAndStop`, `FxReal.UnitFilterFogMatchesOriginal`, `AntiLag.*` (5, model of Mesa's layer),
  `vulkan_lavapipe` (FSR), `Cine.DepthOfFieldAsTheExe`.

## Materials / textures — Original-faithful

- PCD9 BC1/BC2/BC3/BGRA8 + cube maps (`src/formats/Texture.cpp`); material blob blend/alpha/two-sided
  (`src/formats/Model.cpp`, docs/formats/MATERIALS.md); gamma-space UNORM everywhere (as tru.exe); 16x anisotropic.
- **Verify**: `Texture.*`, `Model.*`, `RealData.LaraTexturesAndModel`, `RealData.LaraOutfitsDrawTextured`,
  `RealData.LevelUberSectionModels`.

## Irradiance probes and lights — Original-faithful

- Unit irradiance volume parsed (`formats::parseIrradianceVolume`, `src/formats/Level.cpp:656,685`; docs/formats/LEVELS.md:182),
  sampled per entity (`formats::sampleIrradiance`) into IrradianceState c70-c76 (`src/render/OriginalShading.hpp:86-93`);
  point/spot light sets and spot cookies (`OriginalShading`). No Enhanced probe system exists [O].
- **Verify**: `OriginalShading.KrakenHeadIrradianceMatchesOriginal`, `.LightTableAndSets`, `.LightSetGroups`,
  `.SpotCookieProjection`, `.BedroomLightSetMatchesOriginal`, `.BedroomLightsSwitchByCameraDistance`,
  `.TerrainLeafLightsMatchOriginal`, `StreamReal.BedroomNeighbourLights`.

## Effects (`src/fx`) — Original-faithful

- `Fx.cpp` (GenericFXObject parser + deterministic 60 Hz sim: particles, class 1 ribbons, 2 glows, 3 lightning/beams,
  5 screen filters, 6 effect lights, 7 instance fades), `Weather.cpp` (original rain/lightning/snow), `Breath.cpp`
  (breath fog); drawn by VK `fx.vert/.frag`, GL equivalent; Enhanced weather layers are separate (Enhancement).
- **Config**: `effects true`, `--fx on|off`; `weather "auto"`.
- **Verify**: `Fx.*` (13), `FxReal.*` (18), `FxBreath.*`, `Fx.WeatherRainFillsFallsAndFlashes`, `GameReal.UnitWeatherSurvey`.

## Audio — Mixed (FMOD-free)

- Own mixer (`src/audio/Mixer.*`: voices, 3D attenuation/pan, buses, music stems) on SDL3; FSB4 / IMA (tru.exe 0x5957A0)
  / `.mul` decode (`Audio.cpp`); `.mus` sequencer on the stream beat clock (`Mus.cpp`); 8-slot music player with
  crossfades (`MusicPlayer.*`); SoundPlex DTP graphs, sfxmarkers (`SoundPlex.*`); FMOD-Ex DSP stand-ins: low/high-pass,
  EQ, SFX reverb (`Dsp.*`, docs/formats/AUDIO.md §9.15); game glue `src/app/game/Sound.cpp`. `fmodex.dll` unused.
- **Config**: `game.sfx_volume 8`, `music_volume 8`, `cinematic_volume 8` (0..10); `--no-audio`.
- **Limits**: ROADMAP "Sound events" = Partial (animation-event coverage, DSPs are stand-ins).
- **Verify**: `Mixer.*` (10), `Mus.*`, `MusicPlayer.*`, `SoundPlex.*`, `Dsp.SfxReverbTailDecaysAtDecayTime`, `Ima.*`,
  `Fsb4.*`, `Mul.*`, `SfxMarker.*`, `SoundPlayer.*`, `AudioReal.*` (12), `ScriptRuntime.AudioStateFollowsMusicRequestsAndRoundTrips`.

## Animation — Original-faithful

- Clips/skin/sampling `src/anim/Anim.cpp`; crossfades with the anim-request fades and additive overlays `Blend.cpp`;
  Verlet hair on the render model's cloth constraints `Hair.cpp` (tru.exe 0x45FAB0 reference); two-bone IK `Ik.cpp`,
  limb-lock passes `src/game/Ik.*` (30 records; no foot IK on terrain, as the original); object anim state graphs
  `src/formats/AnimGraph.*` (docs/formats/ANIMGRAPH.md). Lara's controller states are hand-written in
  `src/game/Player.cpp` with rules and numbers taken from the decoded player state chart (chart S/T ids cited in
  `src/game/Game.hpp:182-260`) [O]; it does not interpret the chart at run time [I].
- **Limits**: underwater hair mode not switched; bike/wading/wall-jump limb-lock records (ROADMAP rows "Skeleton", "Limb locks").
- **Verify**: `Anim.*` (13), `AnimReal.*` (7), `Anim.HairChainIsStableAndSettlesUnderGravity`, `GameReal.Ik*` (6),
  `Game.IkPassesStayOffOnTerrain`, self-tests `ik_pose`, `ik_feet`.

## Camera — Original-faithful

- Follow camera from tru.exe LaraCameraRewrite 0x602690 (`src/game/World.cpp`, `Game.hpp`), collision only with
  camera-flagged groups/objects; reveal cameras = AutoSplineCamera (`src/game/Camera.cpp`); camera shake from data
  (`src/game/Feedback.*`); Hor+ projection (`src/render/Camera.hpp`). Config `fov 60`, `controls.invert_x/invert_y`.
- **Verify**: `Game.CameraRestAndPitchReturn`, `Game.CameraSwingsBehindRunningLara`, `Game.CameraSwingsBehindSwimmingLara`,
  `Game.CameraHangYawForceFollowsShimmy`, `Game.CameraForcesIndependentOfRenderRate`, `GameReal.RevealCameraPlaysUnitSpline`,
  `GameReal.SeaCircleCameraLevelsOut`, `PhysicsRealData.CameraCollidesOnlyWithFlaggedObjects`, `Camera.*`, `CameraShake.*`.

## Input — Original-faithful

- Names ↔ SDL devices `src/platform/Input.*`; game actions `src/game/Input.cpp`; press/release/hold timing layer and
  saving-grab rules in `Player.cpp`; bindings `config::InputConfig` (default bindings in its ctor; 35 actions incl.
  menu_*), `mouse_sensitivity 1.0`, `invert_y/invert_x false`, `move_stick "left"`, `look_stick "right"`,
  `vibration true`; launcher Controllers tab; in-game GAME OPTIONS > CONTROL / GAMEPAD CONFIGURATION.
  Replays: `--input-script`, `--record-input`, `--replay-input`.
- **Verify**: `Game.ControlBindings`, `Game.SticksAndMenuNavigationRebind`, `Game.InputScriptParse`,
  `Game.ReplayedDeviceSessions`, `Game.ButtonTimingLayer`, `Config.Controls*`, `game_input_replay_smoke`,
  `LauncherGui.ControlsCaptureClearResetAndValidate`.

## UI / menus / HUD / PDA — Original-faithful

- `src/ui/Ui.cpp` (menus, slots, options, tailoring, EXTRAS, message box, loading screens), `Hud.cpp`, `Pda.cpp`,
  `Art.cpp` (generalbank art + font, `!WAR` RAW), layouts fitted to tru.exe captures; original UI scripts not executed
  (`src/ui/Ui.hpp:1-5`). Our additions: EXTRAS OUTFITS, graphics menus for Enhanced settings. Config `game.*`
  (`subtitles`, `reticles`, `helper_buttons`, `training_text` true; `language "en"`; `tailoring {1,1,1,1}`).
- **Verify**: `Menus.*` (14), `MenusReal.*`, `Hud.*` (4), `PdaReal.*`, `Menus.ButtonPrompts`, `Menus.GraphicsOptions`.

## Save / load — Mixed

- `src/save/Save.cpp`: versioned JSON (format 3, migrations v1/v2) + CRC, atomic write + `.bak`; World/script/physics/
  combat runtime serialized so a load continues bit for bit; checkpoint saves restore on death; original slot naming
  (docs/SAVES.md). Keys F5/F9, `--load`, `--die`. Saves under `$XDG_DATA_HOME/openunderworld/saves` (docs/SAVES.md:11).
- **Verify**: `Save.*` (9), `Stream.*` (6), `StreamReal.SaveContinuesBitIdentically`, `StreamReal.*SaveLoad`,
  `Physics.SavedSceneContinuesBitIdentically`, `Stream.SaveMoveLoadLandsOnSameState`, replay `save_move_load`.

## Script VM — Original-faithful (bounded)

- `src/script/Vm.cpp` (SCMD interpreter from tru.exe 0x57CE50: scalar/string/reference ops, raw-word branches,
  nested/virtual/sequential calls, SLEEP/WAIT fibers), `Script.cpp` (sections, volumes/planes, natives, runtime, save
  state). Static-link fallback for unsupported paths.
- **Limits**: 314 handler stops in the survey; many natives unbound (ROADMAP "Scripts" = Implemented).
- **Verify**: `ScriptVm.*` (8), `ScriptRuntime.*` (14), `ScriptRealData.*` (19), `ProgressionReal.*` (56),
  `game_replay_*` receptacle replays.

## Cinematics / movies — Original-faithful

- In-engine: `src/cine/Cine.*` + `src/app/game/Cinematic.cpp` (actors, cameras, `.mul` track, subtitles, Esc skip;
  `--cine NAME`, `--no-cinematics`). Movies: Bink via libav, not `binkw32.dll` (`src/video/Movie.*`, `Player.*`;
  `ou-movie`; `--no-movies`).
- **Verify**: `Cine.*` (6), `CineReal.*` (6), `VideoRealData.DecodesBinkVideoAndAudio`, `VideoRealData.SelectsAudioTrackByLanguage`,
  `Subtitles.*`, `movie_intro_screenshot`.

## Launcher (Qt 6) — Enhancement (tooling)

- `src/launcher/main.cpp` (Qt ≥ 6.5 Widgets + Concurrent, no moc; AppImage bundles Qt 6.8.3): import scan (read-only),
  graphics, controls capture, saves backup/restore, diagnostics, updates, Play (`ou-game --config`) / Viewer.
  Optional target: built only when Qt6 is found (`CMakeLists.txt:456-476`). Docs: docs/LAUNCHER.md.
- **Verify**: `launcher_self_test` (offscreen), `LauncherGui.*` (7).

## Updater (Ed25519) — Enhancement (tooling)

- `src/update/Update.*`: detached Ed25519 signature (libsodium) over manifest bytes before parsing, size + SHA-256 per
  file, staged swap with `renameat2(RENAME_EXCHANGE)` + journal, rollback, AppImage self-update with rename and
  `.desktop` retarget, resumable HTTPS downloads (curl). Key compiled from `packaging/release-pubkey.hex`; signing tool
  `ou-sign`. Config `updates_enabled true`, `update_channel "stable"`, `update_manifest_url` = GitHub latest
  `manifest.json` (`Config.hpp:167-171`). Docs: docs/UPDATES.md.
- **Verify**: `UpdateTest.*` (10), `AppImageUpdateTest.*` (6), `UpdateCore.*` (2), `LauncherGui.AppImageUpdateCheckInstallResumeRollback`,
  `LauncherGui.InstallRootUpdateViewerAndRollback`.

## AppImage packaging / release — Enhancement (tooling)

- `packaging/appimage/{Containerfile,build-in-container.sh,build-appimage.sh,smoke-in-container.sh,AppRun}` (Ubuntu
  24.04, pinned SDL/Qt/Vulkan-Headers/glm, floor GLIBC 2.38), `tools/release.sh` (signed manifest, GitHub release;
  ctest only with `--test`), PKGBUILD, CI `.github/workflows/{ci,appimage}.yml`. Docs: docs/RELEASING.md.
- **Verify**: `packaging/appimage/smoke-in-container.sh` (self-test, import scan, GL llvmpipe, VK lavapipe/RADV frames),
  `vulkan_lavapipe`, `launcher_self_test`.

## Self-test system — Enhancement (tooling)

- `src/app/game/SelfTest.*` (`ou-game --self-test FILE.json --self-test-report OUT`), probes and operators per
  docs/SELFTEST.md; `src/app/game/Autopilot.*`; scenarios `tests/selftests/` (114 + 14 `known_gaps`), replays
  `tests/replays/` (28), receptacle replays via `ou_receptacle_replay()` (`CMakeLists.txt:585-600`).
- **Verify**: `SelfTest.*` (4), `game_selftest_*` (one per scenario), `game_replay_*` (7), `game_render_rate_smoke`,
  `game_input_replay_smoke`, `ou_gates_tests` (`ProgressionReal.*`), `ou_vehicle_tests`.
