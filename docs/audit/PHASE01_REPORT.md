# Phase 0 + Phase 1 report: audit baseline and first forensic binary pass

Date: 2026-11-16. Baseline: `main` 9034839 = tag `v0.51.0`. A recoverable snapshot of it is the branch
`baseline/pre-audit-2026-11-16`. The tree was clean, with no uncommitted work to preserve. This pass changed
no engine source, shaders, CMake, tests or defaults; it added only documentation, analysis data and
analysis scripts.

Detailed deliverables (this report summarises them and does not repeat them):

| Deliverable | File |
|---|---|
| Build, test and performance baseline | [BASELINE_2026-11.md](BASELINE_2026-11.md) |
| Repository audit with the classification buckets | [PROJECT_AUDIT_2026-11.md](PROJECT_AUDIT_2026-11.md) |
| Protected subsystems: 28 systems with files, defaults and verification tests | [PROTECTED_SYSTEMS.md](PROTECTED_SYSTEMS.md) |
| Bug and regression database: 88 entries in 8 root-cause clusters | [BUGS.md](BUGS.md) |
| External research and edition detection | [EXTERNAL_RESEARCH_2026-11.md](EXTERNAL_RESEARCH_2026-11.md) |
| Original binary inventory: SHA-256 hashes, PE facts | [../re/BINARIES.md](../re/BINARIES.md), `tools/re/pe_inventory.py` |
| tru.exe function database and coverage | [../re/FUNCTION_DATABASE.md](../re/FUNCTION_DATABASE.md), `docs/re/db/`, `tools/re/function_db.py` |
| Reconstructed engine architecture and the Windows-to-Linux mapping | [../re/ENGINE_ARCHITECTURE.md](../re/ENGINE_ARCHITECTURE.md) |
| Ghidra exporter (call edges, refs, FID flags) | `tools/ghidra/scripts/ExportGraph.java` |

Bulky outputs, such as the call-graph edges, objdump listings, ctest logs, screenshots and the SHA-256
manifest, are in `openunderworld-work/re-baseline/<Agent>/`.

## 1. What already exists

The repo is a mature native Linux reimplementation, not a prototype:

- History: 750 commits and 61 releases, v0.1.0 through v0.51.0.
- Source: 77.7 k lines in 213 files under `src/`.
- Tests: 27 GoogleTest files, 114 self-test scenarios and 28 recorded replays.
- Executables: `ou-game`, `ou-viewer`, the Qt 6 `ou-launcher`, and the asset, audio, import, sign, text,
  movie and imgdiff tools.
- Toolchain: C++23 with CMake.
- Platform layer: SDL3 for windowing, input, audio and timing.
- Physics: Jolt 5.6.0, pinned and deterministic.
- Renderers: Vulkan 1.3 (default) and OpenGL 4.6.
- Packaging and updates: an Ed25519-signed AppImage updater.

The game can be played from chapter 0 to the credits (`ProgressionReal.CriticalPathInOrder`). See PROJECT_AUDIT §1–§3.

## 2. What has been improved beyond the original

Everything below is an Enhancement and must be protected. See PROTECTED_SYSTEMS for files, config keys and tests.

- **Ambient occlusion:**
  - CORTAO (default, full resolution, temporal, Vulkan and GL).
  - Ray-traced AO (`--ao rt`, Vulkan).
  - `off`.
  - No other screen-space AO exists. Baked light maps and irradiance volumes are original lighting, not AO.
- **Ray tracing (Vulkan):**
  - Sun shadows.
  - Reflections.
  - AO.
  - Sun-bounce GI.
  - Local-light shadows.
  - BLAS refit for skinned meshes.
  - Fallback to the original PSSM shadows.
- **Water (Enhanced):**
  - NeuralSpecOcean FFT with 3 cascades.
  - GPU eWave interaction field.
  - PBF splash fluid with SSFR.
  - Tessellation.
  - Physically formed caustics.
  - Underwater medium (Snell window, total internal reflection, shafts, particles).
  - Original water stays available.
- **Image pipeline:**
  - FSR 3.1.6 upscaling and frame generation.
  - Anti-Lag.
  - MSAA.
  - FXAA.
  - HDR tonemap.
- **Infrastructure:**
  - Qt launcher.
  - Signed updater.
  - Deterministic replays.
  - The `--self-test` JSON framework.

## 3. What currently works (measured at v0.51.0, BASELINE)

- **Build:** GCC 16.2.1 with LTO and FSR, RelWithDebInfo; 160 s. 268 unique warnings, all in our own code;
  231 of them are in tests, mostly `-Wmissing-field-initializers`.
- **ctest:** 997 selected (`game_vulkan_rt_smoke` excluded), 980 passed, **0 failed**, 17 skipped (opt-in
  surveys and lavapipe).
- **Self-tests:** locomotion 9/9, ledge 6/6 and water_entry 5/5 passed.
- **Performance** (RX 7900 XTX, RADV, 1920×1052, mean / p95 frame time):

| Scene and settings | mean ms | p95 ms |
|---|---|---|
| l3_main_bedroom, VK, CORTAO (default) | 2.06 | 2.76 |
| l3_main_bedroom, VK, AO off | 1.77 | 2.43 |
| l3_main_bedroom, VK, RT AO | 3.29 | 5.25 |
| l3_main_bedroom, GL, default | 2.38 | 2.56 |
| l2_cliffclimb, VK, Enhanced water | 6.31 | 7.97 |
| l2_cliffclimb, VK, Original water | 5.95 | 6.59 |

  CORTAO costs 0.23 ms of GPU time. The difference between the VK and GL images is 0.20/255 (PSNR 45.1 dB).
- **Protected systems:** each was observed working in this run: SDL3 init, Jolt (21 physics tests), both
  water modes, all three AO modes, RT (VK_KHR_acceleration_structure, ray_query and ray_tracing_pipeline
  present), and the GL shadow-map fallback.

## 4. What is broken or open

The full list is in BUGS.md. Its index has 88 entries: 65 open (some are design limits), 11 fixed and
verified, and 12 waiting for a repro.
The most important are:

- **OU-VK-001 `game_vulkan_rt_smoke` hangs the GPU.** This is a regression. The test passed when it was
  added in 671e35e (2026-10-29) and was already excluded by v0.41.0. Suggested next step: bisect in a
  GPU-reset-safe session.
- **OU-WAT-006 Swell clamped at the rest plane.** The FFT swell is only ever pushed down, through `min(...,0)`
  in `water_surface.glsl:87-91` and `World.cpp:1716`. The baseline run shows a symptom: after the same run,
  Lara rests at z≈108 with Enhanced water and z≈250 with Original. That 142-unit gap is larger than the
  ~35-unit offset recorded earlier and needs a measured repro.
- **OU-WAT-007 Dry-deck rule.** It accepts any moving solid (`Player.cpp:3100`).
- **OU-CAM-001 Moving platforms.** They do not carry the camera (`World.cpp:959`).
- **Script VM gaps (cluster C-VM).** Op 0x65 halts fire-trigger volumes; it was hit 535 times in the user's
  own logs and 9 times on every l3_main_bedroom load. Several natives are missing: instanceref 10,
  instance 27/63, gamebase 27, cinematic 10 and unitref 1.
- **Documentation drift (OU-DOC-001/002).**
  - RENDERING.md:1110 calls Original the default preset; the code default is Enhanced.
  - The ROADMAP "Materials" row still says "no shader translation".
  - The chapter matrices mark rows NOT IMPLEMENTED that are implemented.
  - `src/physics/Physics.cpp:1498` says the original vehicle is a "Havok body", but tru.exe contains no
    Havok code (BINARIES §5.3).
- **Test gap:** AO has no image or regression test. The GL-vs-VK parity tests run with AO off.

## 5. Subsystems that must be protected

All 28 entries in PROTECTED_SYSTEMS.md.

- **The core set:**
  - SDL3 platform layer.
  - Jolt integration: joints, loose bodies, buoyant yachts, pressure plates, the push proxy, explosions,
    the motorcycle `VehicleConstraint`, ragdolls and exact save state.
  - Both renderers and the shader translator.
  - Every water, AO, RT and FSR path.
  - Effects, audio DSP, the anim graph and IK, hair, the script VM, save/load, the launcher, the updater,
    packaging and self-tests.
- **Design decisions that must not be undone by accident:**
  - Lara's controller is deliberately not Jolt `CharacterVirtual`. It was tried, failed 8 of 26 comparison
    tests and was removed (84eb0be). Level collision is our own BVH.
  - FSR, RT and Anti-Lag are Vulkan-only by design.

## 6. Original binaries identified (BINARIES.md)

- **tru.exe:**
  - GOG v3 / FileVersion 1.1, 6,422,528 bytes.
  - SHA-256 `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011`. This matches the input
    of the Ghidra project.
  - PE32 i386, built 2008-12-16 17:31 UTC with VS2005 and a static CRT.
  - PDB path `D:\tr8\output\win32_gm\game\tr8_win32_gm.pdb`.
  - No DRM.
  - A Valve `VLV` block replaces the Rich header, so this build derives from the Steam build [INFERENCE].
  - Statically linked: FMOD Ex 4.16.9, NavPower (`bfx::`) and IJG libjpeg 6b.
- **Install integrity:** all 23 installed package files match GOG's `goggame-*.hashdb` by MD5.
- **D3D9.dll, dxwrapper.dll, DEPHook.asi and binkw32.dll:** these are GOG's own compatibility layer.
  - binkw32.dll is the Ultimate ASI Loader, with four embedded RAD Bink builds.
  - DEPHook disables DEP.
  - dxwrapper caps the frame rate at 60.
  - OpenUnderworld needs none of these. They matter only when the original runs under Wine.
- **fmodex.dll:** unused by tru.exe.
- **Data:** bigfile.000-003 hold 7339 records (2048-byte alignment) and patch.000 holds 9. The five movies
  are Bink 1 (BIKi).
- **Editions:** no hashes for other editions are published. A detection recipe is in EXTERNAL_RESEARCH §3.

## 7. How much binary analysis is done (FUNCTION_DATABASE.md)

| Status | Functions | Bytes |
|---|---|---|
| Reimplemented-cited (an address cited in src/tests) | 1,321 | 615,926 |
| Named-only (cited only in docs, or named in Ghidra) | 567 | 188,657 |
| Library (FMOD 1,232, CRT 650, NavPower 509) | 2,375 | 492,039 |
| Unknown | 14,503 | 2,708,933 |
| **Total** | **18,766** | **4,005,555** |

- **Byte coverage:** 17.5 % of non-library code bytes are cited from code or tests; 22.8 % if docs-only
  citations count.
- **Citations:** 6,305 citations point at 1,874 distinct functions.
- **Call graph and RTTI:** 43,097 internal call edges were exported. 494 of the 990 RTTI classes have at
  least one cited vtable slot.
- **Names applied in Ghidra:** 168 functions and 23 labels, 0 errors (previously 144).
- **Instruction-level checks:** 11 high-value citations were checked against the assembly, and all match:
  - sin waves 0x69DA80.
  - Loose-body damping 0x4EFB20 (clamped to 0..1).
  - The SCMD interpreter 0x57CE50 with its 133-case jump table.
  - 0x5DA230, 0x5E0190, 0x597E90, 0x6F1790, 0x7594C0, 0x5570F0, 0x6AD070 and 0x473840.
- **Mislabels found:**
  - 0x67ED20 is slot 7 of `EnemyState_RegenerateDeath`; the repo calls it "EnemyRegenerateData", which is
    a type name.
  - 0x61DD10 is a message handler, not "InvestigateStart".
  - 0x417F30 "CreateLinkStrip" is unconfirmed.

**Honest limit:** 17.5 % byte coverage is not "complete decompilation". The code paths behind gameplay,
rendering and data are covered far more densely than the rest. The 14.5 k unknown functions are mostly
uncalled static initializers (0x830000..0x83DFFF), enemy and NavPower internals, and UI.

## 8. Which engine systems are understood (ENGINE_ARCHITECTURE.md)

| Fidelity | Systems |
|---|---|
| Decoded from tru.exe and verified | timing, resource/bigfile/DRM loading, streaming, animation clips, cinematics, effects, render materials and translated shaders, localization, interaction geometry |
| Reconstructed approximation | player controller (a native state machine reconstructed from many decoded pieces), physics (Jolt in place of cdc multibody), AI (native state machine plus our own A* in place of NavPower), script VM (bounded), audio mixer/DSP, save format, UI |
| Unknown | engine shutdown, the overall inventory layout, the full PCScene_Draw 0x5150E0 pass order, the script native binding table |

The 19-row Windows-to-Linux mapping table is at the end of ENGINE_ARCHITECTURE.md.

## 9. What is still unknown

- **Script natives:** the native dispatch tables are built at runtime, so a Wine runtime capture is needed.
  Op 0x65, collections and sleeping fibers are also unsolved.
- **Indirect calls:** virtual and indirect calls are not resolved. 6,306 non-library functions have no
  direct caller.
- **Library tags:** 1,435 library tags rest on the address-run rule. An FID pass with MSVC 2005/2008 and
  FMOD Ex signatures would turn them into direct evidence. The libjpeg functions have not been located.
- **Gameplay values:** 107 of the 161 REFERENCE_COMPARISON rows are unmeasured against Wine. Examples are
  the underwater turn rate, obstacle reach, ledge-aim reach and the grapple throw clip.
- **Speedrun claims to check:** EXTERNAL_RESEARCH lists them, for example fall damage after a swan-dive
  landing, the sprint cancelling the landing stumble, and a crouch-drop that takes no damage.

## 10. Which implementations to keep and what can be fixed safely

- **Keep:** every system in §5. Nothing in this audit argues for replacing a subsystem.
- **Safe next fixes:** each is small, the evidence is in hand and a test exists or is easy to add.
  - OU-WAT-007 dry-deck rule.
  - OU-WAT-006 swell clamp, after a measured repro of the z gap.
  - OU-CAM-001 platform camera.
  - OU-WAT-002 spray covering the dive centre.
  - OU-PLY-003 climbing out of water onto a shelf.
  - OU-GRP-001 grapple throw clip.
  - The documentation drift (OU-DOC-001/002) and the Havok comment.
  - An AO regression image test.
- **Needs RE first:** the C-VM natives, the C-JOINT puzzle blockers, OU-SWM-001 (0x755A00), and the
  camera and render items listed in BUGS §triage.

## 11. Next reverse-engineering priorities

1. **Script native binding capture under Wine.** This unblocks op 0x65 and the missing natives, which have
   the biggest effect the user sees (fire volumes, puzzles).
2. **Resolve virtual calls.** Turn RTTI vftable slots and constructor stores into call-graph edges. Then
   re-rank `docs/re/db/priorities.tsv`, which holds 7,870 uncited functions reachable from 1,308 cited roots.
3. **Run FID** with MSVC 2005 CRT and FMOD Ex signatures, to shrink the Unknown pool with direct evidence.
4. **Trace the render pass order** in PCScene_Draw 0x5150E0 end to end. Compare it with the translated
   pipeline.
5. **Wine measurement campaign** for the unmeasured REFERENCE_COMPARISON rows and the speedrun claims.
6. **Bisect OU-VK-001** (`game_vulkan_rt_smoke`) between 671e35e and v0.41.0.

## Addendum (2026-11-19)

This report describes the state on 2026-11-16. For everything after it, read these instead:
- [PROGRESS.md](PROGRESS.md): batches 2-5.
- [BUGS.md](BUGS.md): current statuses.
- [BRIEF_COMPLIANCE.md](BRIEF_COMPLIANCE.md) §16: the work queue.
- The newer docs/re files: STRUCTURES, RENDER_PIPELINE, SCRIPT_NATIVES, VERIFICATION, CAMERA, UI, SAVEGAME,
  SHUTDOWN, PUZZLE_BLOCKERS.

The fixes from that work shipped in 0.52.0 (script VM) and 0.53.0 (sea level, platform camera, swim turn, puzzles).
