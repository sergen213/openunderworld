# Master brief compliance matrix and remaining-work queue

Date: 2026-11-20. State checked: `int/batch11` at `ab11185` = `main` `5a646ce` (v0.59.0, batch 10 of
[PROGRESS.md](PROGRESS.md)) plus the WineSweep10 merge (hazard / object / combat Wine sweep). First written
2026-11-16 at `ad672cc`; re-audited at 0.58.0 (`9d2935c`) and refreshed here for batch 10: each path below was checked
to exist, each test name was checked against `ctest -N` of a Release build of `ab11185` (1111 tests registered,
41 of them in `ctest -L protected`).

This document walks the master engineering brief section by section (preservation policy, Phases 0-11, the 25
Phase 11 deliverables, the 30 execution rules, the first assignment and its report questions). For every requirement
it gives a status, the deliverable that satisfies it (repo-relative path) and the concrete gap. Section 16 is the
ordered queue of what remains, by phase.

It is an index over existing evidence; numbers are quoted from the cited files (their dates apply) unless the row
says "counted 2026-11-20".

## 0. Summary

Rows per section by status, counted by script over the Status column of every table in §1-§15 of this revision
(2026-11-20; the same count over the 0.58.0 revision gives its old totals 256 / 60 / 6 / 2).

| Section | Met | Partly | Missing (Not met) | N/A |
|---|---|---|---|---|
| §1 Preservation policy | 17 | 1 | 0 | 0 |
| §2 Phase 0 research and audit | 15 | 1 | 0 | 0 |
| §3 Phase 1 binary analysis | 46 | 3 | 0 | 0 |
| §4 Phase 2 asset formats | 22 | 1 | 0 | 0 |
| §5 Phase 3 original runtime | 4 | 13 | 1 | 0 |
| §6 Phase 4 walkthrough validation | 11 | 1 | 0 | 0 |
| §7 Phase 5 engine specification | 21 | 1 | 0 | 0 |
| §8 Phase 6 SDL3 engine | 9 | 4 | 0 | 0 |
| §9 Phase 7 bug fixing | 5 | 0 | 0 | 0 |
| §10 Phase 8 self-tests | 30 | 2 | 0 | 0 |
| §11 Phase 9 Windows vs Linux | 6 | 8 | 0 | 0 |
| §12 Phase 10 performance and stability | 6 | 5 | 3 | 0 |
| §13 Phase 11 deliverables | 26 | 3 | 0 | 2 |
| §14 Execution rules | 26 | 4 | 0 | 0 |
| §15 First assignment | 26 | 1 | 0 | 0 |
| **Total** | **270** | **48** | **4** | **2** |

What moved in batch 10 (0.59.0) and WineSweep10: libjpeg 6b located and tagged (P1-1), the verification table settled
with the batch 6-9 citations (P1-2), call edges and CFG summaries in the repo (P1-3), the untagged-path inventory
(P5-3), per-chapter checkpoint and objective lists with a respawn test per chapter (P4-1), the second matrix refresh
(P4-2), apitrace D3D9 captures of tru.exe with runtime pass lists (P3-10) and a 19-row rendering classification
(P9-1), the sanitizer + Vulkan validation record per release (P10-2), `game_vulkan_rt_smoke` passing again (P10-3,
OU-VK-001), the hazard / health / push-block Wine sweep (P3-9, parts of P3-5 / P3-6), known gaps 11 → 1, the batch 10
progress entry (P11-3) and the public progress site (tools/progress, GitHub Pages). What stays open is mostly live
measurement of tru.exe (Phase 3 / 9: 103 comparison rows unmeasured; combat needs enemies the chapter scripts birth),
the Lara state chart as data (P5-2), performance numbers on an idle machine (Phase 10) and the open BUGS entries
(48 Open + 8 Reproduced + 4 Partly fixed of 114).

## Legend

| Status | Meaning |
|---|---|
| **Met** | A deliverable exists that satisfies the requirement as written; the path is the evidence. |
| **Partly** | Something exists, but a named part of the requirement is missing; the gap says what. |
| **Not met** | Nothing satisfies it yet (counted as "Missing" in §0). |
| **N/A** | Not applicable, with the reason. |

Paths: `docs/…`, `src/…`, `tests/…`, `tools/…`, `CMakeLists.txt` are in the repo. `work/…` =
`/mnt/1st4TB/Projects/openunderworld-work/…` (private, outside the repo: Ghidra project, traces, captures); every row
that cites `work/` also cites a repo path. Test names are ctest names (`Suite.Name`, `game_selftest_<name>`,
`render_*`, `protected_*`). "Wine" marks work that needs the original tru.exe running under Wine
(docs/REFERENCE_COMPARISON.md "Method", harness in tools/wine-ref/).

---

## 1. Non-negotiable preservation policy

### 1.1 Preserve existing work (protected systems)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Preserve SDL3; no GLFW/SFML/SDL2; no second window/input/lifecycle system | Met | docs/audit/PROTECTED_SYSTEMS.md "SDL3 platform layer"; docs/audit/BASELINE_2026-11.md §7; ctest `protected_sdl3_frame_clock`; `game_selftest_engine` step `sdl` | — |
| Preserve Jolt; do not assume CharacterVirtual / defaults equal the original | Met | PROTECTED_SYSTEMS "Jolt Physics"; docs/formats/PHYSICS.md "Character controller" (CharacterVirtual evaluated and removed); `protected_jolt_physics` | — |
| Water: preserve, audit and document before changing | Met | PROTECTED_SYSTEMS "Water"; docs/RENDERING.md water sections; docs/re/RENDER_PIPELINE.md §7, §13.2 (tru.exe D3D9 capture of the l2_cliffclimb sea); `protected_water`, `render_golden_cliff_water_*` | Original-preset sea differs from the capture (OU-WAT-015, §16 P7-3) |
| AO/RT: identify every technique, shader, option, quality, path; keep all | Met | PROTECTED_SYSTEMS "Ambient occlusion", "Ray tracing"; `render_ao_opengl`, `render_ao_vulkan`, `render_ao_rt_vulkan`, `render_ao_parity`, `game_vulkan_rt_smoke` (passes again, OU-VK-001 Fixed-verified 0.59.0) | — |
| Inventory every other improvement | Met | PROTECTED_SYSTEMS (28 systems with files, config keys, tests); docs/audit/PROJECT_AUDIT_2026-11.md §5 | — |

### 1.2 Establish a protected baseline (11 steps)

| # | Step | Status | Evidence | Gap |
|---|---|---|---|---|
| 1 | Inspect the Git repository | Met | docs/audit/PROJECT_AUDIT_2026-11.md §1, §4 | — |
| 2 | Tracked / untracked changes | Met | docs/audit/PHASE01_REPORT.md header | — |
| 3 | Record current commit | Met | PHASE01_REPORT, docs/audit/BASELINE_2026-11.md (9034839 = v0.51.0) | — |
| 4 | Preserve uncommitted work | Met | none existed; PHASE01_REPORT header | — |
| 5 | Preservation branch / snapshot | Met | branch `baseline/pre-audit-2026-11-16` (docs/audit/PROGRESS.md batch 1) | — |
| 6 | Full subsystem inventory | Met | PROJECT_AUDIT §3; docs/audit/PROTECTED_SYSTEMS.md | — |
| 7 | Build the project | Met | BASELINE_2026-11 §1 | — |
| 8 | Run all tests | Met | BASELINE_2026-11 §4; every batch since: PROGRESS.md (batch 10: 1111 of 1111, `game_vulkan_rt_smoke` included) | — |
| 9 | Build failures and warnings | Met | BASELINE_2026-11 §3 | — |
| 10 | Screenshots and performance | Partly | BASELINE_2026-11 §6; docs/audit/BASELINE_PERF_2026-11.md (tooling, validation runs) | The idle-machine tables (BASELINE_PERF §5) are still "pending" (§16 P10-1) |
| 11 | Record SDL3/Jolt/water/AO/RT/render functionality | Met | BASELINE_2026-11 §7; `game_selftest_engine` (tests/selftests/engine.json) | — |

### 1.3 No unjustified replacements (7 questions before modifying)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Answer the 7 questions before modifying a subsystem | Met | CONTRIBUTING.md "Before changing a subsystem" (seven-question template for commits and pull requests); BUGS.md entries (Cause / Fix / Test) | — |
| Replacement only with documented justification + migration | Met | no replacement through 0.59.0 (CHANGELOG.md; PHASE01_REPORT §10 "Keep") | — |

---

## 2. Phase 0 — research and project audit

### 2.1 External research (0.1)

| Topic | Status | Evidence | Gap |
|---|---|---|---|
| TRU RE research, engine docs, modding projects | Met | docs/RESEARCH.md §4-§8; docs/audit/EXTERNAL_RESEARCH_2026-11.md §1 | — |
| File format docs, extraction tools | Met | RESEARCH §8; docs/formats/*.md | — |
| Previous native-port attempts | Met | docs/audit/EXTERNAL_RESEARCH_2026-11.md §1 "Previous native ports of TRU" (queries, dates, result: none other than OpenUnderworld) | — |
| Graphics/rendering research | Met | RESEARCH §8.8-§8.10 | — |
| Animation/gameplay investigations | Partly | EXTERNAL_RESEARCH §3 (speedrun leads) | Leads not checked against tru.exe (§16 P3-12) |
| Patches and executable differences | Met | RESEARCH §2-§3; EXTERNAL_RESEARCH §2 | Other editions not in hand |
| Relevance + "do not trust without verification" | Met | RESEARCH §5.3; docs/re/VERIFICATION.md | — |

### 2.2 Repository audit (0.2) — 27 items

All 27 aspects: docs/audit/PROJECT_AUDIT_2026-11.md (§2 build, §3 architecture, §4 history), PROTECTED_SYSTEMS (one
section per subsystem) and BASELINE_2026-11 §2 (toolchain / dependency versions).

| Item group | Status | Evidence | Gap |
|---|---|---|---|
| Architecture, structure, build, compiler, C++ standard, SDL3 version, Jolt version | Met | PROJECT_AUDIT §2-§3 | — |
| Graphics API, shaders, pipeline, water, RT, AO, lighting, shadows, reflections, materials | Met | docs/RENDERING.md; drift fixed in 550830a (OU-DOC-002 partly) | — |
| Animation, player controller, camera, level/asset loading, audio, UI, input | Met | PROTECTED_SYSTEMS; docs/GAMEPLAY.md | — |
| Test framework, debug tools, perf monitoring | Met | docs/SELFTEST.md; docs/BUILDING.md; tools/bench/ | — |
| Git history / non-obvious improvements | Met | PROJECT_AUDIT §4, §6 | — |

### 2.3 Bug inventory (0.3)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Reproduce before fixing | Met | docs/audit/BUGS.md "Reproduction log"; every batch 6-10 fix cites a Wine or ctest repro (PROGRESS.md) | 7 entries "Not reproduced" kept for hand tests |
| 10 fields per bug | Met | BUGS.md "Legend", "Entries" | — |
| Pre-existing bug vs regression | Met | BUGS.md "Origin" column | — |

### 2.4 Audit deliverable (0.4) — 9 buckets

| Bucket | Status | Evidence | Gap |
|---|---|---|---|
| 1 Works … 9 Safe to reimplement | Met | docs/audit/PROJECT_AUDIT_2026-11.md §5; docs/audit/PHASE01_REPORT.md §1-§10 | Snapshot of 2026-11-16 (not refreshed; history in PROGRESS.md) |

---

## 3. Phase 1 — forensic binary analysis

### 3.1 Byte-level examination (1.1)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Tools: Ghidra + scripting, objdump, strings, Python, PE parser, debuggers, runtime instrumentation | Met | tools/ghidra/*.sh, tools/ghidra/scripts/*.java; tools/re/pe_inventory.py; tools/wine-ref/src/ (wmem.c, ref.c, pole.c, section.c, enemy.c) | — |
| File format, architecture, entry point | Met | docs/re/BINARIES.md §4.0 | — |
| Section layout and permissions | Met | BINARIES §4.1-§4.7 | — |
| Imports / exports | Met | BINARIES §4; tools/ghidra/scripts/ExportImports.java | — |
| Relocations | Met | BINARIES §5.2 | — |
| Resources | Met | BINARIES §4 | — |
| Embedded data, packed regions, archives | Met | BINARIES §5.3; libjpeg 6b located: docs/re/FUNCTION_DATABASE.md "libjpeg 6b", docs/re/db/libjpeg.tsv (269 chunks, 195 Ghidra functions; tools/re/libjpeg_id.py) | — |
| Strings | Met | BINARIES §5.5; tools/ghidra/scripts/ExportStrings.java; docs/re/db/string_refs.tsv | — |
| Function boundaries | Met | docs/re/db/functions.tsv (18,766 functions) | — |
| Virtual tables, RTTI | Met | BINARIES §5.4; docs/re/db/rtti.tsv, classes.tsv, class_hierarchy.tsv | — |
| Exception handling | Met | BINARIES §5.2 | FuncInfo tables not decoded per function (low value) |
| Global data | Met | docs/re/STRUCTURES.md "Globals"; docs/re/db/globals.tsv (11,650) | — |
| Cross-references | Met | tools/ghidra/scripts/Xrefs.java, ExportGraph.java; docs/re/db/xrefs_summary.tsv | — |
| External module dependencies | Met | BINARIES §1, §5.3; docs/re/db/module_deps.tsv | — |
| Offsets/sizes/signatures; preserve originals; SHA-256 | Met | BINARIES §1-§3 | — |
| Every relevant DLL/module | Met | BINARIES §4.2-§4.7 | — |

### 3.2 Comprehensive Ghidra analysis (1.2)

| Per-function item | Status | Evidence | Gap |
|---|---|---|---|
| Address, size, boundaries | Met | docs/re/db/functions.tsv | — |
| Assembly analysed | Partly | docs/re/VERIFICATION.md, docs/re/db/verification.tsv (120 claims, all settled: 116 match, 4 mismatch; V075–V097 = batch 6-9 citations); objdump checks in docs/re/CAMERA.md, UI.md, SAVEGAME.md, SHUTDOWN.md (tag V) | Only cited claims are checked; mismatches V020 (OU-PHY-003) and V056 (OU-PLY-016) open; wrong citation addresses OU-DOC-003 (§16 P11-5) |
| Decompiler output | Met | tools/ghidra/scripts/DecompileAll.java; output `work/ghidra/exports/decomp_all.c` | Not in repo by design (size, game-derived) |
| Arguments, calling convention, return type | Met | functions.tsv `cc`, `params`, `stack_args`, `ret_guess` (4e51373, tools/ghidra/scripts/ExportFunctionDetail.java) | — |
| Callers, callees | Met | functions.tsv `callers` / `callees`; docs/re/db/call_edges.tsv (44,146 direct edges, tools/ghidra/scripts/ExportGraph.java); docs/re/db/vcall_edges.tsv (virtual sites) | — |
| Control flow | Met | docs/re/db/cfg.tsv (18,766 functions: blocks, edges, exits, cyclomatic number, back edges); functions.tsv switch columns | — |
| Global data access, constants, offsets | Met | functions.tsv globals / consts columns; docs/re/db/this_offsets.tsv | — |
| Structure references, class relationships | Met | docs/re/STRUCTURES.md; docs/re/db/structures.tsv, class_hierarchy.tsv | — |
| State transitions | Partly | docs/GAMEPLAY.md (state chart), docs/formats/ANIMGRAPH.md, docs/formats/SCRIPTS.md | Not linked function-by-function in the DB; Lara's chart still hand-ported per move (BUGS C-CHART) |
| Confidence levels | Met | functions.tsv `confidence` | — |
| Rename functions; keep address + name | Met | tools/ghidra/names/tru_gog.csv; ApplyNames.java | Most functions still `FUN_*` (FUNCTION_DATABASE coverage table) |
| Scripts: inventories, call graphs, xrefs, decompiled exports, structures, globals, strings, deps | Met | tools/ghidra/scripts/*.java; tools/re/function_db.py, struct_db.py | — |
| Verify critical findings against assembly | Met | docs/re/VERIFICATION.md (every cited protected-system claim settled 2026-11-20) | Mismatches tracked as bugs (see "Assembly analysed") |
| FID / library identification | Met | FUNCTION_DATABASE.md "fid" column, crt row (381 Ghidra FID + 4 new matches), libjpeg row (28 entry points checked at instruction level + 167 reached) | — |

### 3.3 Internal engine architecture (1.3)

docs/re/ENGINE_ARCHITECTURE.md has one section per subsystem (§1-§26). The brief's 33 subsystems:

| Brief subsystem | Status | Evidence | Gap |
|---|---|---|---|
| Init, shutdown | Met | ENGINE_ARCHITECTURE §1; docs/re/STARTUP.md; docs/re/SHUTDOWN.md (58c435b) | — |
| Main loop, timing | Met | §2; docs/re/TIMING.md | — |
| Rendering pipeline, graphics resources | Met | §18; docs/re/RENDER_PIPELINE.md (§13: runtime D3D9 capture under Wine) | — |
| Scene/world, level loading, asset loading | Met | §3, §4; docs/re/FILESYSTEM.md | — |
| Entity management | Met | §5 | — |
| Player controller, climbing/traversal | Met | §6, §12; docs/GAMEPLAY.md | Chart reconstructed per move (C-CHART) |
| Physics, collision | Met | §7; docs/formats/PHYSICS.md, COLLISION.md | — |
| Animation, skeleton | Met | §8 | — |
| Camera | Met | §9; docs/re/CAMERA.md (CameraManager, 15 camera types, transitions) | Combat camera (0x16) and AnchoredCamera unported (OU-CAM-006) |
| Cinematics | Met | §10 | — |
| Interaction, environment interactions | Met | §11, §16 | — |
| Combat/targeting, weapons/inventory | Met | §13, §14 | — |
| Enemy AI | Partly | §15 | NavPower internals replaced by our A* (documented approximation; OU-CMB-002) |
| Particles | Met | §17 | — |
| Audio | Met | §20 | FMOD reverb not reproduced (OU-AUD-002) |
| UI | Met | §21; docs/re/UI.md | Several screens not drawn (OU-UI-002) |
| Save/load, checkpoints | Met | §22; docs/re/SAVEGAME.md (`.TRUSave`, block types) | — |
| Scripting, events | Met | §23; docs/re/SCRIPT_NATIVES.md, docs/re/db/script_natives.tsv (2267 binds) | `instance` 63 unbound (OU-SCR-003) |
| Input, configuration, localisation | Met | §24-§26 | — |

---

## 4. Phase 2 — asset and data format reconstruction

### 4.1 Asset categories

Every file in docs/formats/ (19 docs) now states its endianness and has a Validation section (2bd586f, 2ac13bd,
1896903; counted 2026-11-20: 19 of 19).

| Brief category | Status | Doc | Inspection tool | Round-trip / validation test | Gap |
|---|---|---|---|---|---|
| Archive formats | Met | docs/formats/CONTAINERS.md §1-§2 | `ou-asset sections`, `ou-import` | `RealData.RoundTripBigfileIndexAndLayout`, `Importer.SyntheticRoundTripWithSplitParts` | — |
| Container structures | Met | CONTAINERS §3-§4 | `ou-asset sections`, `ou-asset roundtrip` | `RealData.RoundTripEveryDrm`, `RoundTrip.RelocationFieldsBitExact` | — |
| File indexes | Met | CONTAINERS §1.3-§1.5 | `ou-import` | `RealData.RoundTripBigfileIndexAndLayout` | — |
| Compression | Met | CONTAINERS §3; AUDIO, TEXTURES, VIDEO | `ou-asset roundtrip` | `Cdrm.RoundTripMixedBlocks`, `RoundTrip.CdrmWriterSplitsBlocksAndRewraps` | — |
| Texture formats | Met | docs/formats/TEXTURES.md | `ou-asset textures`, `dump-texture` | `RealData.Pcd9RewrapEveryTexture`, `Texture.CubeFacesAndBadSizes` | — |
| Mesh formats | Met | docs/formats/MESHES.md | `ou-asset dump-mesh`, `stats`, `render` | MESHES "Validation" | — |
| Skeleton structures | Met | docs/formats/ANIMATION.md | `ou-asset anim` | `AnimReal.LaraSceneLinksSkeletonAndBindPoseIsIdentity` | — |
| Animation data | Met | ANIMATION.md, ANIMGRAPH.md | `ou-asset anim <drm> <clip>` | ANIMATION "Validation" | No clip re-encode (no writer needed) |
| Material definitions | Met | docs/formats/MATERIALS.md | `ou-asset materials` | MATERIALS "Validation" | — |
| Shader resources | Met | docs/formats/SHADERS.md | `ou-asset shaders`, `shader-dis` | `Sm3Real.EveryInstallShaderTranslates` | — |
| Collision geometry | Met | docs/formats/COLLISION.md, PHYSICS.md | `ou-asset collision` | COLLISION "Validation" | — |
| Level geometry | Met | MESHES "Terrain"; docs/formats/LEVELS.md | `ou-asset --level` | `RealData.UnitLightsAllUnits` and LEVELS "Validation" | NavPower resource layout partial |
| Lighting data | Met | docs/formats/LIGHTING.md (cbee972) | — (read through LEVELS / MESHES dumps) | `RealData.LightMapsWithoutCoordinatesReadTheMean`, `OriginalShading.*` | — |
| Audio | Met | docs/formats/AUDIO.md | `ou-audio` | AUDIO "Validation" | No FSB / `.mul` re-encode (no writer needed) |
| Cinematics | Met | docs/formats/CINEMATICS.md, VIDEO.md | `ou-asset cine`, `ou-movie --info` | `CineReal.*` | — |
| Scripts | Met | docs/formats/SCRIPTS.md; docs/re/SCRIPT_NATIVES.md | `ou-asset script <drm> <type>` (bytecode listing) | `ScriptRuntime.StateRoundTrip`, `GameReal.ScriptSurvey` | — |
| Localisation | Met | docs/formats/TEXT.md | `ou-text` | `LocaleRealData.LocalsBinRoundTrip` | — |
| UI graphics and fonts | Met | docs/formats/UI.md (2c6f64a); docs/re/UI.md | `ou-asset font` | `MenusReal.ArtAndFontMetrics` | — |

### 4.2 Phase 2 cross-cutting requirements

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Document the 10 items for every format | Met | docs/formats/*.md (endianness lines, Validation sections) | — |
| Extraction/inspection utilities | Met | tools/ou-asset/main.cpp `usage()` (sections, textures, dump-texture, dump-mesh, stats, render, shaders, shader-dis, collision, anim, materials, script, cine, font, roundtrip); tools/ou-audio, ou-text, ou-movie, ou-import, ou-imgdiff | — |
| Round-trip tests for re-encodable formats | Met | `RealData.RoundTripEveryDrm`, `RealData.RoundTripBigfileIndexAndLayout`, `RealData.Pcd9RewrapEveryTexture`, `LocaleRealData.LocalsBinRoundTrip` (623d059) | — |
| Loaders reject corrupt / truncated input, with tests | Partly | `Cdrm.TruncationThrows`, `Texture.CubeFacesAndBadSizes`, `Save.RejectsCorruptAndTruncated` | Most loaders lack synthetic rejection tests (BUGS OU-TST-003; §16 P2-1) |
| Preserve originals; never overwrite game files | Met | BINARIES §2; ou-import reads only | — |

---

## 5. Phase 3 — original game runtime investigation

Harness: tools/wine-ref/ (README.md: compositor, prefix, helpers tools/wine-ref/src/{wmem,ref,pole,section,enemy,wkey}.c,
trace readers `py/*.py`, scripted sessions `sessions/*.sh`; cee6c4d). Traces live in
`work/w2/<Agent>/`, `work/gameplaygoal/reference/` and others named per row in docs/REFERENCE_COMPARISON.md.

| Instrumentation (brief list) | Status | Evidence | Gap |
|---|---|---|---|
| Breakpoints / debugger instrumentation | Partly | tools/wine-ref/camhook.sh (hook on 0x5E8210); winedbg in tools/wine-ref/README.md | No general breakpoint script set |
| Memory inspection, runtime variables | Met | tools/wine-ref/src/ref.c (`trace`, `rec`, `tp`, `tpv`), wmem.c, section.c | — |
| Function tracing | Partly | targeted hooks (camhook.sh, tools/wine-ref/py/regcap.py PS-constant capture) | — |
| API monitoring (D3D9, FMOD, input) | Partly | D3D9: apitrace over DXVK, tools/wine-ref/sessions/render_capture.sh, tools/wine-ref/py/d3d9_frames.py (docs/re/RENDER_PIPELINE.md §13) | No FMOD event trace (§16 P3-11); no input API trace |
| Input recording | Partly | tools/wine-ref/src/wkey.c (SendInput playback) | No recording of a human session |
| Frame captures | Met | tools/wine-ref/shot.sh (screenshots); per-draw D3D9 frames (render_capture.sh: 504 draws bedroom, 5204 draws cliffclimb, RENDER_PIPELINE §13.1-§13.2) | — |
| Performance profiling of the original | Not met | — | §16 P3-13 |

| Investigated area | Status | Evidence | Gap |
|---|---|---|---|
| Player controller: walk, run, accel/decel, jump, fall, land | Partly | docs/REFERENCE_COMPARISON.md "Movement" (speeds within 1 %, jump timing, gravity) | Ground turn rates, stop distance, crouch / roll unmeasured (M94, M15; §16 P3-1) |
| Climbing, ledge grab, mantling, wall interactions | Partly | "Ledges and climbing"; chimney measured; safety drop and pole fling measured (`w2/Drop9`, Differences #5 / #6) | Shimmy / ladder / free-climb / beam / perch rates unmeasured (M01, M02, M05, M96, M12, M10, M11, M80; §16 P3-2) |
| Swimming, diving, underwater movement | Partly | "Water": rest depth, surface and under-water swim turn, velocity lag, dive, breath (`w2/WineMeasure`, `w2/Swim9`) | Drowning tick, under-water sprint drain, dive depth classes (M29, M85; §16 P3-3) |
| Object manipulation, combat movement | Partly | shark reach and passes measured (`w2/SharkAi6`, `Ai.SharkIgnoresLaraOutOfWater`); push block acceleration (M56), health / regeneration (M111), hazards M65, M67, M68, M79, M103, M104 measured (WineSweep10, REFERENCE_COMPARISON "Objects, puzzles, hazards") | Lever / door / plate times (M52-M55, M57-M62) and every combat timing (M28-M46) unmeasured; a `goto` session holds no AI instance (§16 P3-5, P3-6) |
| Animation: selection, state machines, blending, root motion, timing, transitions | Partly | clip lead 0x441FB0 on take-offs; flinch overlay fade measured (PROGRESS batch 8) | Other clips' start frames, interruption / sync unmeasured |
| Physics: gravity, integration, collision, ground, slopes, friction, momentum, jump | Partly | gravity, slopes, step bracket, capsule; one-sided wall contacts (PROGRESS batch 8) | Friction on surfaces, ropes unmeasured |
| Ropes, interactive objects, water interactions, environmental physics | Partly | yacht heave and sea level (`w2/WineMeasure`), ring door leaf (`GameReal`, PROGRESS batch 8) | Grapple swing unmeasured (OU-GRP-002; §16 P3-4); motorcycle unmeasured (§16 P3-7) |
| Camera: position, follow, rotation, smoothing, collision, constraints, transitions | Partly | REFERENCE_COMPARISON "Systems, camera" (deck distance, trigger commands 0x12-0x1C, cut, focus points, swim modes; `w2/CamZones7`, `w2/CamCut9`) | Land transitions (climb / hang / combat) unmeasured (§16 P3-8) |
| Rendering (15 items) | Met | docs/re/RENDER_PIPELINE.md (§13 runtime D3D9 capture: pass order, transparency sort, batch LOD switching; §13.6 answers the open questions), SHADERS.md, EFFECTS.md; water under-surface gate measured (OU-WAT-014) | Two captured scenes; the rest rests on the static read |
| Interface/progression | Partly | docs/re/UI.md, docs/re/SAVEGAME.md; captures in `work/screens/ui` (M109) | UI compared by eye only; unlockables not compared |
| Reproducible docs with measurements | Met | trace paths per row in REFERENCE_COMPARISON; sessions in tools/wine-ref/sessions/ | — |

---

## 6. Phase 4 — walkthrough-based level validation

Counted 2026-11-20 over docs/matrix/*.md: 661 requirement rows; status strings 18 `VERIFIED AGAINST ORIGINAL`, 494
`IMPLEMENTED BUT UNVERIFIED`, 243 `PARTIALLY IMPLEMENTED`, 10 `NOT IMPLEMENTED` (a row may carry more than one).

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Use the tombraiders.net walkthrough and its chapter pages | Met | docs/underworld_chapter_gameplay_matrix.md header; docs/matrix/*.md (21 chapter files) | — |
| Chapter names, level names | Met | docs/matrix file names, "Our units" lines | — |
| Objectives | Met | docs/CHECKPOINTS.md "Objectives" (TRU has no objective list; the PDA FIELD ASSISTANCE help lines per chapter, 499 lines, generated by tools/re/chapter_data.py); `ChaptersReal.ObjectiveListMatchesTheHelpLines` | Help on demand plays the wrong chapter's lines in ours (OU-AUD-006, §16 P7-9) |
| Locations, interactions, traversal sequences | Met | requirement tables `CNN-xx` | — |
| Puzzles | Met | "Puzzles" rows (21/21); docs/re/PUZZLE_BLOCKERS.md | — |
| Collectibles | Met | "Collectibles" rows; `ChaptersReal.RewardSurveyMatchesTheRewardTable` | — |
| Important objects | Met | "Objects" rows with uids | — |
| Checkpoints | Met | docs/CHECKPOINTS.md (303 checkpoints in 72 units: uid, unit, position, yaw, respawn result); `ChaptersReal.CheckpointListMatchesTheData`, `ChaptersReal/CheckpointRespawn.PlacesLaraOnSolidGround/*` (one per chapter); `game_selftest_checkpoint_respawn` | 25 respawns pinned with reasons (OU-SAV-002, §16 P7-10) |
| Level transitions, progression conditions | Met | docs/PROGRESSION.md "Gates"; `ProgressionReal.CriticalPathInOrder`, `ProgressionReal.CriticalGates` | — |
| Chapter-by-chapter validation scenarios | Partly | 130 scenarios in tests/selftests/ (most `chNN_*`), 1 parked in tests/selftests/known_gaps/ (`ch13_temple_block_topple`) | No chapter plays end to end (OU-TST-001; §16 P4-3) |
| Walkthrough not used as mechanics source | Met | matrix header | — |
| Freshness | Met | second refresh 2026-11-20 (P4-2, Chapters10) for batches 5-9 and the legs unparked in batch 10: docs/underworld_chapter_gameplay_matrix.md "Status changes since the baseline", docs/matrix/*.md; `tools/re/ref_levels.py --check` passes | — |

---

## 7. Phase 5 — original engine specification

| # | Item | Status | Evidence | Gap |
|---|---|---|---|---|
| 1 | Executable module map | Met | docs/re/BINARIES.md §1, §4; docs/re/db/module_deps.tsv | — |
| 2 | Function database | Met | docs/re/FUNCTION_DATABASE.md, docs/re/db/functions.tsv | — |
| 3 | Call graphs | Met | functions.tsv callers / callees; docs/re/db/call_edges.tsv (direct), docs/re/db/vcall_edges.tsv (virtual); FUNCTION_DATABASE.md documents both | No rendered per-subsystem graph images (not needed for the tables) |
| 4 | Data structures | Met | docs/re/STRUCTURES.md; docs/re/db/structures.tsv, classes.tsv | — |
| 5 | Engine architecture | Met | docs/re/ENGINE_ARCHITECTURE.md | — |
| 6 | Main game loop | Met | ENGINE_ARCHITECTURE §2; docs/re/TIMING.md; docs/re/STARTUP.md | — |
| 7 | Rendering architecture | Met | docs/re/RENDER_PIPELINE.md (runtime capture §13) | — |
| 8 | Physics architecture | Met | ENGINE_ARCHITECTURE §7; docs/formats/PHYSICS.md | Rope bend bodies (OU-PZL-008 b) |
| 9 | Player controller | Met | ENGINE_ARCHITECTURE §6; docs/GAMEPLAY.md | — |
| 10 | Animation system | Met | §8; docs/formats/ANIMATION.md, ANIMGRAPH.md | — |
| 11 | Camera system | Met | §9; docs/re/CAMERA.md; docs/GAMEPLAY.md "Cameras" | — |
| 12 | Interaction system | Met | §11 | — |
| 13 | Audio system | Met | §20; docs/formats/AUDIO.md | — |
| 14 | UI and menus | Met | §21; docs/re/UI.md | — |
| 15 | Save/load | Met | §22; docs/re/SAVEGAME.md; docs/SAVES.md | — |
| 16 | Level management | Met | §4; docs/formats/LEVELS.md | — |
| 17 | Resource formats | Met | docs/formats/*.md (§4) | — |
| 18 | Scripting and events | Met | §23; docs/formats/SCRIPTS.md; docs/re/SCRIPT_NATIVES.md | Unbound natives listed there |
| 19 | Runtime dependencies | Met | BINARIES §4-§5.3 | — |
| 20 | Unresolved questions | Met | docs/LIMITATIONS.md (register, counts refreshed 2026-11-20); ENGINE_ARCHITECTURE "Open questions across subsystems" | — |
| — | Windows-to-Linux mapping | Met | ENGINE_ARCHITECTURE "Windows → Linux mapping" | — |
| — | Every decision traceable or marked approximation | Partly | [O]/[I]/[U] tags; docs/re/db/citations.tsv; untagged-path inventory docs/re/db/untagged_paths.tsv, docs/LIMITATIONS.md (tools/re/tag_inventory.py: 598 of 937 definitions in src/game, src/script untagged, 446 longer than 3 lines) | Untagged paths not yet tagged or justified (§16 P5-4) |

---

## 8. Phase 6 — improve the existing SDL3 engine

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| No parallel engine / duplicate abstractions | Met | PROTECTED_SYSTEMS; PHASE01_REPORT §10 | — |
| 6.1 Preserve SDL3 init, loop, window, input, audio, events, timing, display | Met | PROTECTED_SYSTEMS "SDL3 platform layer", "Input", "Fixed-step timing"; `protected_sdl3_frame_clock`, `protected_fixed_step` | — |
| 6.1 Reproduce original input and timing behaviour | Partly | docs/re/TIMING.md; docs/audit/controls.md; jump timing, swim turn against the view (OU-SWM-001) | Ground turn rates unmeasured (§16 P3-1); controls.md "Input differences found" not all closed |
| 6.2 Preserve Jolt; investigate the 15 listed aspects | Partly | docs/formats/PHYSICS.md; one-sided contacts and per-tick push-out (OU-PLY-012); body-A joint poses (OU-PHY-001); push block acceleration measured (M56) | Friction, air / hang / swim push-out unmeasured; ours' push speed not yet moved to M56 (OU-PHY-002); loose-body damping (OU-PHY-003) |
| 6.2 Measurements guide corrections | Met | REFERENCE_COMPARISON "Differences to fix" each cites a trace | — |
| 6.3 Identify graphics APIs, architecture, pipeline, deps | Met | docs/RENDERING.md; PROTECTED_SYSTEMS | — |
| 6.3 Preserve water, AO, lighting, shadows, reflections, materials, post, GPU resources | Met | PROTECTED_SYSTEMS; `ctest -L protected` (41 tests) | — |
| Water: original as reference, improve without regression | Partly | OU-WAT-006 / 014 fixed; `render_golden_cliff_water_*` | Open: OU-WAT-002, 007, 008 (§16 P7-3) |
| RT AO: quality, perf, resources, temporal stability, integration | Partly | `render_ao_rt_vulkan`, `render_ao_parity`; docs/RENDERING.md | Vulkan temporal pass camera-only (OU-VK-002; §16 P7-4) |
| Other AO: inventory, keep options, clean method selection | Met | `graphics.ao` auto/off/cortao/rt; `render_ao_opengl`, `render_ao_vulkan` | — |
| No silent default changes | Met | docs/RENDERING.md "--preset" (drift fixed in 550830a); `game_selftest_engine` checks the defaults (Enhanced water, CORTAO) | — |
| Original vs enhanced kept as two goals | Met | `--water original|enhanced`, presets; `render_golden_bedroom_original_gl`, `render_golden_cliff_water_original_vk` | — |
| Time-of-day / sky additions keep both presets | Met | docs/RENDERING.md "Time of day"; `TimeOfDay.ConfigUnitsAndLight` | — |

---

## 9. Phase 7 — bug fixing and root-cause analysis

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Central bug database, IDs, subsystem, steps, expected, actual, RE evidence, cause, correction | Met | docs/audit/BUGS.md (114 index rows, counted 2026-11-20 with tools/progress/progress.py: 44 fixed, 4 partly fixed, 48 open (3 of them Needs-Windows-ref), 8 Reproduced, 7 Not reproduced, 3 Not a bug; each fix names the release that shipped it) | — |
| Correction + regression test + verify existing functionality | Met | batches 4-10 (docs/audit/PROGRESS.md): each fix with a test, full ctest per integration (batch 10: 1111 of 1111) | Remaining open bugs: §16 P6/P7 |
| Root-cause clusters, not symptoms | Met | BUGS.md "Root-cause clusters" | — |
| Verify the protected areas after each change | Met | `ctest -L protected` (CMakeLists.txt `ou_protected`, 41 tests; 6627f6b) | — |
| Small isolated changes | Met | one branch per fix (PROGRESS.md batches 6-10) | — |

---

## 10. Phase 8 — automated self-test system

### 10.1 Invocation and report schema

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| `--self-test <file.json>` | Met | docs/SELFTEST.md "Running"; src/app/game/SelfTest.cpp | — |
| Real engine, resources, playable state, scripted inputs, real state, results, exit | Met | docs/SELFTEST.md | — |
| Report `version`, `game`, `platform`, `steps[]` | Met | src/app/game/SelfTest.cpp report builder (`"platform", "Linux"`, ecde8b8); tests/test_selftest.cpp asserts it | — |
| Tests validate state, not that a call happened | Met | SELFTEST.md "Steps" / "Conditions"; `SelfTest.ScenarioChecks` | — |
| Preserve and expand existing self-tests | Met | 130 scenarios + 1 known gap (tests/selftests/); 7 `game_replay_*` ctests over tests/replays/ | — |
| Visual baselines where possible | Met | tests/golden/*.png (6 scenes) compared by tests/golden/render_check.sh; `render_golden_*` | — |
| Run after meaningful builds | Met | ctest `game_selftest_*`, `ctest -L protected` | — |

### 10.2 Required test categories

| Category | Status | Where | Gap |
|---|---|---|---|
| SDL3 initialisation | Met | `game_selftest_engine` step `sdl` (`engine.sdl.*`) | — |
| Input | Partly | replays tests/replays/*.txt / *.events; `game_input_replay_smoke` | Self-tests drive the `Actions` path, not SDL events; no gamepad self-test step (§16 P8-2) |
| Window and rendering initialisation | Met | `game_selftest_engine` step `renderer`; `RendererSelect.AutoFallsBackToOpenGl`, `VulkanRenderer.ResizeRecreatesSwapchain` | — |
| Resource loading | Met | `RealData.*`, `Level.*` | — |
| Level loading | Met | every scenario; `StreamReal.*` | — |
| Jolt initialisation, physics simulation | Met | `game_selftest_engine` step `jolt_world` (`physics.bodies`, `physics.active`); `protected_jolt_physics` | — |
| Movement | Met | `game_selftest_locomotion` | — |
| Jumping | Met | `game_selftest_locomotion`, jump scenarios | — |
| Falling | Met | fall scenarios (SELFTEST.md "Shipped scenarios") | — |
| Landing | Met | SELFTEST.md "Shipped scenarios" | — |
| Collision | Met | `game_selftest_ledge`; `protected_collision` | Contact identity not probeable |
| Animation | Met | `step.clips`, `step.states` probes; `protected_animation` | — |
| Camera | Met | `camera.mode`, `camera.mode_name`, `camera.distance` probes; `StreamReal.CameraRidesTheYachtDeck`; `protected_camera_input` | — |
| Climbing | Met | ledge / ladder / pole scenarios | — |
| Interaction | Met | `game_selftest_ch01_bedroom_new_game`, `game_selftest_push_block` | — |
| Combat | Met | `game_selftest_combat` | Autopilot does not fight (OU-TST-002) |
| Inventory | Met | `inventory.*` probes (tests/selftests/save_load.json); `game_selftest_ch01_hall_first_aid` | — |
| HUD | Met | `hud.*` probes (tests/selftests/engine.json step `hud`); `Hud.*`; `protected_hud` | Reticles / toasts not probeable |
| Water shader compilation and rendering | Met | `engine.water*` probes; `WaterSurface.GlSourcesExpandAndCompile`; `render_golden_cliff_water_*`; `game_selftest_water_entry` | — |
| AO shader compilation and rendering | Met | `engine.ao*` probes; `render_ao_opengl`, `render_ao_vulkan`, `render_ao_rt_vulkan`, `render_ao_parity` | — |
| Ray-tracing initialisation | Met | `engine.rt*` probes; `VulkanRenderer.RayTracingBuildsUpdatesAndTearsDown`, `VulkanRenderer.RayTracingFallsBackToShadowMap`, `game_vulkan_rt_smoke` (passes, OU-VK-001 Fixed-verified 0.59.0) | — |
| Audio | Met | `audio.*` probes (`game_selftest_engine` with SDL's dummy driver); `protected_audio` | Sound by sound not probeable |
| Save/load | Met | `save.*` probes, `game_selftest_save_load`; `StreamReal.SaveContinuesBitIdentically` | — |
| Level transitions | Met | `game_selftest_ch07_hubhall_statue_hall` and other unit-change scenarios | — |
| Checkpoint position probe | Partly | `game_selftest_checkpoint_respawn` | Checkpoint position not probeable (SELFTEST.md "Not available") (§16 P8-1) |

---

## 11. Phase 9 — Windows vs Linux comparison

docs/REFERENCE_COMPARISON.md, counted 2026-11-20 with tools/progress/progress.py over the mechanic tables: 180 rows,
73 `matches` (6 of them with a part that differs), 4 `differs` (M17, M79, M103, M104), 103 `unmeasured`. Its "Counts"
section says 172 rows / 65 match (it counts some rows differently; §16 P11-5). Per chapter: docs/REFERENCE_LEVELS.md
(`tools/re/ref_levels.py --check` passes).

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Repeatable workflow | Met | REFERENCE_COMPARISON "Method"; tools/wine-ref/ (README, sessions); ours via `--self-test-trace` | — |
| Identical start positions, inputs, state | Met | per-unit offsets, `tpv`; tools/wine-ref/sessions/*.sh | One unit per Wine session |
| Player coordinates, velocity | Met | tools/wine-ref/src/ref.c traces | — |
| Camera coordinates | Met | `rec` camera columns (d8c4d76); REFERENCE_COMPARISON "Systems, camera" rows | Land transitions unmeasured (§16 P3-8) |
| Animation states | Partly | chart state and anim id in traces; tools/wine-ref/src/section.c | Not compared clip by clip |
| Physics measurements | Partly | gravity, slopes, chimney, sea level, yacht heave, fall damage (M65), push block acceleration (M56) | Friction, ropes |
| Interaction states | Partly | push block (M56); hazard contact damage and periods (M67, M68, M79, M103, M104) | Lever / door / plate / timed-door times M52-M55, M57-M62 unmeasured (§16 P3-5) |
| Frame timing | Partly | docs/re/TIMING.md | No frame-pacing capture of tru.exe (§16 P3-13) |
| Audio event timing | Partly | fire-volume burn sound timing measured (OU-SCR-004, PROGRESS batch 6) | No FMOD event trace (§16 P3-11) |
| UI captures | Partly | docs/GAMEPLAY.md "Menus" (fitted to captures) | No side-by-side image set (M109; §16 P9-2) |
| Level progression | Partly | docs/REFERENCE_LEVELS.md; 18 matrix rows VERIFIED | — |
| Rendering: defect vs API difference vs intentional improvement | Met | docs/REFERENCE_COMPARISON.md "Rendering" (two captured scenes × two presets × two renderers, rows R1-R19 classified); docs/re/RENDER_PIPELINE.md §13 | Defects open as bugs (OU-REN-010/011/012, OU-WAT-015; §16 P7-5, P7-3) |
| Not screenshots alone; measure | Met | REFERENCE_COMPARISON rows are numeric | — |
| Comparison matrix for every major subsystem and level | Partly | docs/REFERENCE_LEVELS.md (21 chapters × 8 subsystems, 7b8db1b) | 103 rows unmeasured; no rendering / audio / UI rows (§16 P9-3) |

---

## 12. Phase 10 — performance, stability and regression protection

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| CPU utilisation baseline | Not met | tools/bench/bench.py samples CPU % (docs/audit/BASELINE_PERF_2026-11.md §3) | Idle-machine run pending (BASELINE_PERF §5; §16 P10-1) |
| GPU utilisation | Partly | GPU pass ms (BASELINE_2026-11 §6); GPU busy % column in tools/bench | Idle-machine run pending |
| VRAM usage | Not met | VRAM column in tools/bench (BASELINE_PERF §3) | Idle-machine run pending |
| RAM usage | Not met | RSS column in tools/bench | Idle-machine run pending |
| Frame time | Met | BASELINE_2026-11 §6 | — |
| Frame pacing | Partly | frame-time histogram column (`OU_FRAME_TIMES`, c7ca030) | Idle-machine run pending |
| Physics simulation time | Partly | physics ms column in tools/bench | Idle-machine run pending |
| Rendering time | Met | GPU pass ms (`OU_VK_PRESENT_STATS`) | — |
| Shader compilation | Partly | cold / warm runs in tools/bench (BASELINE_PERF §2) | Idle-machine run pending |
| Resource loading | Partly | level-load time column in tools/bench | Idle-machine run pending |
| Profile before optimising; keep HQ features; configurable options | Met | quality options (`ao_quality`, `--rt`, `--water`, FSR) | — |
| AO and water paths stable | Met | `render_ao_*`, `render_golden_*` | — |
| Leaks, lifetimes, sync, GPU validation, physics instability | Met | docs/BUILDING.md "Sanitizer + validation runs" (dated log: 0.58.0 baseline 12 / 11 validation errors → OU-VK-006 fixed, then 0 reports); docs/RELEASING.md "Sanitizer + validation run (once per release)"; tests/lsan_vulkan.supp; `game_vulkan_rt_smoke` green (OU-VK-001) | The log has no line tagged with the 0.59.0 release commit yet (§16 P10-4) |
| Reproducible benchmark scenarios | Met | tools/bench/scripts/{bedroom,cliff_sea,jungle,seafloor}.txt, tools/bench/run.sh (identical `sim` digests, BASELINE_PERF §4) | — |

---

## 13. Phase 11 — documentation and delivery

### 13.1 The 25 deliverables

| # | Deliverable | Status | Path | Gap |
|---|---|---|---|---|
| 1 | Existing project audit | Met | docs/audit/PROJECT_AUDIT_2026-11.md, docs/audit/PHASE01_REPORT.md, docs/audit/AUDIT_A.md, AUDIT_B.md | — |
| 2 | Protected subsystem inventory | Met | docs/audit/PROTECTED_SYSTEMS.md | — |
| 3 | Original binary inventory | Met | docs/re/BINARIES.md | — |
| 4 | SHA-256 hashes | Met | BINARIES §1, §3 | — |
| 5 | Ghidra project | Met | `work/ghidra/` (outside the repo); reproducible via tools/ghidra/analyze.sh + tools/ghidra/names/tru_gog.csv | — |
| 6 | Decompiled function database | Met | docs/re/FUNCTION_DATABASE.md, docs/re/db/functions.tsv | — |
| 7 | Disassembly documentation | Met | docs/re/EXECUTABLE.md; docs/re/VERIFICATION.md (120 claims settled with instruction addresses); V-tagged sections of docs/re/CAMERA.md, UI.md, SAVEGAME.md, SHUTDOWN.md | — |
| 8 | Call graphs | Met | docs/re/db/call_edges.tsv, cfg.tsv, vcall_edges.tsv; functions.tsv callers / callees | — |
| 9 | Reconstructed structures | Met | docs/re/STRUCTURES.md, docs/re/db/structures.tsv | — |
| 10 | Engine architecture specification | Met | docs/re/ENGINE_ARCHITECTURE.md (+ RENDER_PIPELINE, TIMING, STARTUP, SHUTDOWN, CAMERA, UI, SAVEGAME, FILESYSTEM) | — |
| 11 | Asset format documentation | Met | docs/formats/*.md (19) | — |
| 12 | Asset inspection tools | Met | tools/ou-asset/main.cpp and the other tools/ou-* | — |
| 13 | Windows-to-Linux subsystem mapping | Met | ENGINE_ARCHITECTURE "Windows → Linux mapping" | — |
| 14 | Bug database | Met | docs/audit/BUGS.md | — |
| 15 | Regression test suite | Met | 1111 ctests (CMakeLists.txt), tests/selftests/, tests/replays/, tests/golden/ | — |
| 16 | Updated SDL3 implementation | N/A | no SDL3 change needed; preserved (PROTECTED_SYSTEMS) | — |
| 17 | Updated Jolt integration | Met | measured changes on Jolt: one-sided contacts, push-out (OU-PLY-012), joint poses (OU-PHY-001); docs/formats/PHYSICS.md | — |
| 18 | Preserved and improved water shaders | Partly | PROTECTED_SYSTEMS "Water"; OU-WAT-006 / 014 fixed | OU-WAT-002, 007, 008 open |
| 19 | Preserved and improved AO | Met | CORTAO + RT + off; `render_ao_*` | — |
| 20 | Existing renderer documentation | Met | docs/RENDERING.md | — |
| 21 | Automated self-test system | Met | docs/SELFTEST.md, src/app/game/SelfTest.cpp | — |
| 22 | Original-versus-Linux comparison reports | Partly | docs/REFERENCE_COMPARISON.md (incl. "Rendering"), docs/REFERENCE_LEVELS.md | 103 rows unmeasured (§16 P3) |
| 23 | Chapter-by-chapter validation | Partly | docs/underworld_chapter_gameplay_matrix.md, docs/matrix/*.md, docs/PROGRESSION.md, docs/CHECKPOINTS.md | No chapter end to end (§16 P4-3) |
| 24 | Build and installation instructions | Met | docs/BUILDING.md, docs/LAUNCHER.md, docs/RELEASING.md, docs/UPDATES.md | — |
| 25 | Remaining limitations and unresolved questions | Met | docs/LIMITATIONS.md (8fc17a3; counts refreshed 2026-11-20) | — |
| — | Persistent engineering progress report | Met | docs/audit/PROGRESS.md | — |
| — | This compliance matrix | N/A | (self) | — |

### 13.2 Other Phase 11 requirements

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Persistent documentation throughout | Met | docs/, CHANGELOG.md, docs/audit/PROGRESS.md | — |
| Originals, RE artefacts, extracted data, generated files, source kept separate | Met | RE artefacts in `work/`; DB under docs/re/db; harness sources in tools/wine-ref (no game data) | — |
| Reproducible build instructions | Met | docs/BUILDING.md | — |
| Linux launcher + GitHub update mechanism | Met | docs/LAUNCHER.md, docs/UPDATES.md; `protected_updater`, `protected_appimage` | — |

---

## 14. Execution rules (30)

| # | Rule | Status | Evidence / note |
|---|---|---|---|
| 1 | Preserve the project | Met | snapshot branch; agents in their own worktrees (docs/audit/PROGRESS.md batch 6) |
| 2 | Preserve uncommitted work | Met | PROGRESS.md (ScriptPuzzle edits kept on disk, batch 4) |
| 3 | Preserve SDL3 | Met | PROTECTED_SYSTEMS; `protected_sdl3_frame_clock` |
| 4 | Preserve Jolt | Met | `protected_jolt_physics` |
| 5 | Preserve water shaders | Met | `protected_water`, `render_golden_cliff_water_*` |
| 6 | Preserve RT AO | Met | `render_ao_rt_vulkan` |
| 7 | Preserve other AO | Met | `render_ao_opengl`, `render_ao_vulkan` |
| 8 | Preserve other improvements | Met | `ctest -L protected` |
| 9 | No replacement without justification | Met | none through 0.59.0 (CHANGELOG.md) |
| 10 | No random bug fixes first | Met | fixes followed evidence; batch 10 picked from §16 across phases (PROGRESS.md batches 4-10) |
| 11 | Don't assume existing RE is correct | Met | docs/re/VERIFICATION.md mismatch section (V002 false positive, V042 fixed) |
| 12 | Binary analysis beyond strings/imports | Met | function DB, call graph, RTTI, structures, native table |
| 13 | No "complete decompilation" claim | Met | PHASE01_REPORT §7 "Honest limit" |
| 14 | Verify against assembly and runtime | Partly | 120 claims asm-checked (VERIFICATION.md); 87 `[O-runtime]` mentions in REFERENCE_COMPARISON; uncited functions unchecked |
| 15 | Don't invent structures or mechanics | Partly | [O]/[I]/[U] tags; fitted values flagged [I] (shark pass bite share, push block law, either-end sight rule) |
| 16 | No placeholders when assets decode | Partly | real data throughout; motorcycle camera placeholder (OU-CAM-003), grapple throw stand-in clip (OU-GRP-001) |
| 17 | Don't simplify gameplay for convenience | Partly | tests/selftests/known_gaps/ (1); A* for NavPower (documented) |
| 18 | No unnecessary dependencies | Met | BASELINE_2026-11 §2 |
| 19 | No parallel SDL3/physics | Met | — |
| 20 | No silent default changes | Met | docs/RENDERING.md defaults; `game_selftest_engine` checks them |
| 21 | Don't remove modern features | Met | `ctest -L protected` |
| 22 | No fake/superficial tests | Met | SELFTEST.md (steps need expectations; each engine step fails with its subject off) |
| 23 | Don't mark fixed without verification | Met | BUGS.md "Fixed-verified" needs a test; Wine checks per fix (PROGRESS.md) |
| 24 | Commit incrementally | Met | one branch and commit per item (PROGRESS.md) |
| 25 | Persistent engineering progress report | Met | docs/audit/PROGRESS.md |
| 26 | Document every significant discovery | Met | e.g. docs/re/CAMERA.md trigger commands, PUZZLE_BLOCKERS.md |
| 27 | Record unresolved questions | Met | docs/LIMITATIONS.md |
| 28 | Verify protected functionality after changes | Met | `ctest -L protected`; full ctest per integration |
| 29 | Root-cause over workarounds | Met | BUGS clusters; e.g. OU-PLY-012 fixed through the one-sided contact rule |
| 30 | Buildable and recoverable | Met | full ctest green per release (PROGRESS.md) |

---

## 15. First assignment

### 15.1 The five prohibitions

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| No features / renderer rewrite / SDL3-Jolt replacement / water-AO change during the audit | Met | Phase 0/1 commits (c644c40 … ba6b1eb) touch only docs/, tools/re/, tools/ghidra/ | — |

### 15.2 The 13 items

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Audit the complete repository | Met | docs/audit/PROJECT_AUDIT_2026-11.md |
| 2 | Identify existing improvements | Met | PROJECT_AUDIT §5, PHASE01_REPORT §2 |
| 3 | Identify all protected systems | Met | docs/audit/PROTECTED_SYSTEMS.md |
| 4 | Build the current implementation | Met | BASELINE_2026-11 §1 |
| 5 | Record test results | Met | BASELINE_2026-11 §4-§5 |
| 6 | Identify original executable versions | Met | docs/re/BINARIES.md §5; EXTERNAL_RESEARCH §2 |
| 7 | Inventory every relevant binary | Met | BINARIES §1, §4 |
| 8 | SHA-256 hashes | Met | BINARIES §1, §3 |
| 9 | Import into Ghidra | Met | BINARIES §3; tools/ghidra/analyze.sh |
| 10 | Systematic disassembly and decompilation | Partly | DecompileAll.java; functions.tsv detail; asm verification of cited claims only (VERIFICATION.md) |
| 11 | Function and data-structure inventory | Met | FUNCTION_DATABASE.md; STRUCTURES.md |
| 12 | Start reconstructing engine architecture | Met | ENGINE_ARCHITECTURE.md, RENDER_PIPELINE.md |
| 13 | Relationships original ↔ Linux | Met | ENGINE_ARCHITECTURE mapping; docs/re/db/citations.tsv |

### 15.3 The 13 report questions (docs/audit/PHASE01_REPORT.md)

| Question | Status | Answered in |
|---|---|---|
| What already exists | Met | §1 |
| What has been improved | Met | §2 |
| What currently works | Met | §3 |
| What is broken | Met | §4 |
| Which subsystems must be protected | Met | §5 |
| Which original binaries were identified | Met | §6 |
| How much binary analysis is complete | Met | §7 |
| Which original functions have been reconstructed | Met | §7 + docs/re/db/citations.tsv |
| Which engine systems are understood | Met | §8 |
| What is still unknown | Met | §9 |
| Which current implementations should be retained | Met | §10 |
| Which bugs can be addressed safely | Met | §10 |
| Next reverse-engineering priorities | Met | §11 |

---

## 16. Remaining-work queue (phases 0-11)

Only what remains (refreshed 2026-11-20 after batch 10 / 0.59.0 and WineSweep10). Order: phase by phase; inside a
phase, items whose evidence is already in hand or that unblock others come first. Flags: **[Wine]** needs tru.exe under
Wine (tools/wine-ref/; parallel sessions use a private prefix and CPU set each); **[RE]** static Ghidra / objdump
work; **[code]** engine or test change (re-run `ctest -L protected` plus the suites named in the item); **[doc]**
documentation only. IDs `Pn-m` are kept across revisions; the old W-ID is given where an item continues one; "rest"
marks the part of an item that a batch did not finish.

### 16.0 Done since 2026-11-16 (no longer queued)

| Old ID | Done by | Evidence |
|---|---|---|
| W8-1 platform key | ecde8b8 | src/app/game/SelfTest.cpp; tests/test_selftest.cpp |
| W8-2 AO regression tests | 6627f6b, a24b5e2 | `render_ao_opengl`, `render_ao_vulkan`, `render_ao_rt_vulkan`, `render_ao_parity` |
| W8-3 HUD / inventory / audio / camera-mode probes | ecde8b8, b7da43c | docs/SELFTEST.md "Engine, HUD, audio", "Inventory, physics, save / load" |
| W8-4 engine scenario | 4594f2c | tests/selftests/engine.json; `game_selftest_engine` |
| W8-5 golden images | 6627f6b | tests/golden/*.png; `render_golden_*` |
| W8-6 self-test save / load | ecde8b8, 4594f2c | tests/selftests/save_load.json; `game_selftest_save_load` |
| W10-4 `protected` label | 6627f6b | CMakeLists.txt `ou_protected`; 41 tests |
| W10-2 fixed benchmark scenarios | 84ae803 | tools/bench/; docs/audit/BASELINE_PERF_2026-11.md §4 |
| W2-1 UI format doc | 2c6f64a | docs/formats/UI.md |
| W2-2 endianness + validation | 2bd586f, 2ac13bd, 1896903 | docs/formats/*.md |
| W2-3 lighting doc | cbee972 | docs/formats/LIGHTING.md |
| W2-4 / W2-5 / W2-6 real-data round trips | 623d059 | `RealData.RoundTripEveryDrm`, `RealData.RoundTripBigfileIndexAndLayout`, `LocaleRealData.LocalsBinRoundTrip`, `RealData.Pcd9RewrapEveryTexture` |
| W2-7 inspection subcommands | 7639633, b31646f | tools/ou-asset/main.cpp |
| W9-1 per-level matrix | 7b8db1b | docs/REFERENCE_LEVELS.md, tools/re/ref_levels.py |
| W9-2 Wine harness in the repo | cee6c4d | tools/wine-ref/ |
| W11-1 limitations register | 8fc17a3 | docs/LIMITATIONS.md |
| W11-2 progress log | — | docs/audit/PROGRESS.md (batches 1-10) |
| W11-3 doc drift | 550830a, 4b58147, b4c1dba, 69a523c | OU-DOC-001 Fixed-verified; OU-DOC-002 mostly fixed |
| W4-2 matrix refresh (first pass) | f3bd9d0 … 34535f1 | docs/matrix/*.md (2026-11-16) |
| W5-1 camera / UI / save / shutdown decode | 58c435b | docs/re/CAMERA.md, UI.md, SAVEGAME.md, SHUTDOWN.md |
| W5-2 puzzle blocker decode | 8b2543a | docs/re/PUZZLE_BLOCKERS.md |
| ScriptNatives (native binding table) | 1f71837 | docs/re/SCRIPT_NATIVES.md, docs/re/db/script_natives.tsv |
| FuncDetail / StructDb | 4e51373, ffae429 | docs/re/db/functions.tsv detail columns, STRUCTURES.md |
| W1-2 first pass (protected-system claims) | 6435c6b, dd85da1 | docs/re/VERIFICATION.md, docs/re/db/verification.tsv |
| W3-1 rest depth / W6-1 swell | f03439b, bf9179e | OU-WAT-006 (REFERENCE_COMPARISON "Water") |
| W3-9 water part / W6-3 platform camera | f03439b, DeckCam6, CamZones7, CamRest8, CamCut9 (PROGRESS batches 6-9) | OU-CAM-001 / 005 / 006 |
| W6-4 shelf climb-out | ShelfOut9 (PROGRESS batch 9); WineCheck10 removed the wall-hug part (Wine-disproved) and added climb-out A / B / C | OU-PLY-003 stays Open (§16 P7-2) |
| W6-5 script VM op 0x65, natives, vectors | 8daa2ee, f9676fd, ScriptVm6, ScriptVec7 | OU-SCR-001 / 002 / 004 |
| W6-6 differences #5, #6, #7, #13 | Drop9, SwimTurn-2, Swim9 | REFERENCE_COMPARISON "Differences to fix" |
| W6-8 known gaps (part) | PuzzleFix, ScriptPuzzle, Joint8 | known gaps 14 → 11 |
| P0-1 native-port search | batch 10 (agent/Compliance10) | docs/audit/EXTERNAL_RESEARCH_2026-11.md §1 "Previous native ports of TRU" |
| P11-1 status drift | batch 10 (agent/Compliance10) | docs/audit/BUGS.md index and entry statuses carry the release; docs/REFERENCE_COMPARISON.md Counts, M29 sea level, moving-platform camera, Differences #11 / #14; docs/REFERENCE_LEVELS.md regenerated |
| P11-2 limitations / shutdown line | batch 10 (agent/Compliance10) | docs/LIMITATIONS.md, docs/re/ENGINE_ARCHITECTURE.md §1 |
| P11-4 seven-question template | batch 10 (agent/Compliance10) | CONTRIBUTING.md "Before changing a subsystem" |
| P1-1 libjpeg 6b | batch 10 (ReVerify10, merge 1be8668) | docs/re/db/libjpeg.tsv, tools/re/libjpeg_id.py, docs/re/FUNCTION_DATABASE.md "libjpeg 6b" (195 functions tagged) |
| P1-2 verification settled + batch 6-9 citations | ReVerify10 (1be8668) | docs/re/VERIFICATION.md "Batch 10", docs/re/db/verification.tsv (V075–V097; 120 rows, none partial / not found) |
| P1-3 call edges + CFG summaries | ReVerify10 (1be8668) | docs/re/db/call_edges.tsv, docs/re/db/cfg.tsv, tools/ghidra/scripts/ExportGraph.java, FUNCTION_DATABASE.md |
| P5-3 untagged-path inventory | ReVerify10 (1be8668) | docs/re/db/untagged_paths.tsv, tools/re/tag_inventory.py, docs/LIMITATIONS.md (follow-up P5-4) |
| P4-1 checkpoint and objective lists | Chapters10 (add2b7e) | docs/CHECKPOINTS.md (tools/re/chapter_data.py, tools/re/chapter_data_dump.cpp); `ChaptersReal.CheckpointListMatchesTheData`, `ChaptersReal.ObjectiveListMatchesTheHelpLines`, `ChaptersReal/CheckpointRespawn.PlacesLaraOnSolidGround/*` |
| P4-2 second matrix refresh | Chapters10 (add2b7e) | docs/underworld_chapter_gameplay_matrix.md "Status changes since the baseline", docs/matrix/*.md, docs/REFERENCE_LEVELS.md (`ref_levels.py --check` passes) |
| P3-10 runtime render capture | RenderCap10 (2c2da70) | docs/re/RENDER_PIPELINE.md §13; tools/wine-ref/sessions/render_capture.sh, tools/wine-ref/py/d3d9_frames.py |
| P9-1 rendering classification | RenderCap10 (2c2da70) | docs/REFERENCE_COMPARISON.md "Rendering" (R1-R19); OU-REN-010, 011, 012, OU-WAT-015 |
| P10-2 sanitizer + validation record | VkHang10 (8a3846c) | docs/BUILDING.md "Sanitizer + validation runs", docs/RELEASING.md "Sanitizer + validation run (once per release)"; OU-VK-006 |
| P10-3 OU-VK-001 | VkHang10 (8a3846c) | CMakeLists.txt `game_vulkan_rt_smoke` (stale log check fixed; the hang does not reproduce); batch 10 full ctest 1111 / 1111 with it |
| P3-9 hazards (reachable rows) | WineSweep10 (ab11185) | REFERENCE_COMPARISON M65 (live), M67, M68 match; M79, M103, M104 differ → OU-CMB-004, OU-SCR-006, OU-SCR-007; rest in P3-9 rest |
| P3-5 part: push block | WineSweep10 (ab11185) | REFERENCE_COMPARISON M56 (constant acceleration) |
| P3-6 part: health | WineSweep10 (ab11185) | REFERENCE_COMPARISON M111; finding: a `wmem goto` unit holds no AI instance (blocks the rest of P3-6) |
| P4-3 part: known gaps 11 → 1 | GapsA10 / GapsB10 (a83c9da, 09f65eb, e14c36f, c06d661) | tests/selftests/known_gaps/ (only `ch13_temple_block_topple` left); OU-PZL-002, 005, 011, 016, OU-GRP-004, OU-SAV-001 fixed |
| P11-3 batch 10 progress entry | batch 10 | docs/audit/PROGRESS.md "batch 10" |
| Progress site (user request, outside the brief queue) | Pages10 (4c9805e) | tools/progress/progress.py, tools/progress/test_progress.py, .github/workflows/pages.yml, README.md badges |

### 16.1 Phase 0 — research

Nothing remains (P0-1 done, §16.0).

### 16.2 Phase 1 — binary analysis

Nothing queued (P1-1, P1-2, P1-3 done, §16.0). What the batch 10 pass found is tracked as bugs: mismatches V020
(OU-PHY-003, §16 P7-10) and V056 (OU-PLY-016, §16 P7-10), wrong citation addresses (OU-DOC-003, §16 P11-5).

### 16.3 Phase 2 — formats

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P2-1 | [code] | Synthetic rejection tests (corrupt / truncated input) for every loader whose Validation section names none (OU-TST-003) | tests/test_formats.cpp, test_cdc.cpp, test_audio.cpp, test_locale.cpp | each docs/formats Validation row names a passing test |

### 16.4 Phase 3 — original runtime measurement (all [Wine])

Sessions per row: docs/REFERENCE_COMPARISON.md "Unmeasured". Each item ends with rows moved to matches / differs and,
for a difference, a BUGS entry. P3-6 comes first: its harness step (enemies present) unblocks P7-10 and P8-3.

| ID | Goal | Rows / bugs |
|---|---|---|
| P3-6 rest (W3-6) | Harness: a session that starts from a chapter start or a save, so the chapter scripts birth the enemies (a `wmem goto` unit holds none); then every combat timing, enemy speeds / attack periods, shark pass bite share | M28-M46, M81-M83; OU-CMB-002 |
| P3-5 rest (W3-7) | Lever / door times, plate thresholds, timed doors (the l4_ballcourt pole must be carried in first); grapple heave and rope weight (the grapple attach under Wine failed five times); ring-door leaf launch speed | M52-M55, M57-M62, M25, M26; OU-PZL-010 pull speed |
| P3-9 rest (W3-12) | Hazards a teleport cannot start: spear / blade rows (adrenaline sequence started by walking in), steam vent blast timing at l5_valgrind, kraken swats (scripted fight), the rest of M70 / M106; repeat the one-trial roller burst (OU-CMB-004); check the EXTERNAL_RESEARCH speedrun leads | M69, M70, M77, M105, M106; OU-CMB-004 |
| P3-2 (W3-4) | Shimmy / corner / pull-up / hang-jump, free-climb / ladder / pillar rates, beam, poles, perches; the Drop9 open points (plain run-off hold, S543 attach) | M01, M02, M04, M05, M96, M12, M10, M07, M09, M11, M80; OU-PLY-001, 002, 005, 009, 010 |
| P3-1 (W3-3) | Ground turn rates, run stop distance, walk start / stop, crouch / roll, jump held at a ledge, mid-air stick, slide jump | M94, M15, M13, M91, M17 |
| P3-3 (W3-5) | Drowning tick, under-water sprint drain, dive depth classes | M29, M85; OU-SWM-003 |
| P3-4 (W3-2) | Grapple swing: rope length, period, decay, growth, jump-off, rope climb | M20, M99; OU-GRP-002 |
| P3-8 (W3-9) | Camera on land: climb, hang, combat transitions; ours' swim camera bob; a clean walk-in of the 0xC cut | OU-CAM-002, OU-CAM-004, OU-CAM-006 (0x16) |
| P3-7 (W3-8) | Motorcycle: acceleration, braking, steering radius, ramp jump; bike camera | M47-M49; OU-VEH-001, OU-CAM-003 |
| P3-11 (W3-11) | FMOD Ex event / channel start trace for a scripted sequence | docs/formats/AUDIO.md; OU-AUD-002, OU-AUD-005 |
| P3-12 | Animation start frames of the other clips (0x441FB0 lead), interruption | REFERENCE_COMPARISON "Animation transitions" |
| P3-13 | Frame pacing and CPU / GPU load of tru.exe for the bench scenes | docs/audit/BASELINE_PERF_2026-11.md (new section) |

### 16.5 Phase 4 — chapter validation

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P4-3 rest | [code] | Unpark the last known gap (`ch13_temple_block_topple`); chapter end-to-end self-tests (OU-TST-001) | tests/selftests/, tests/selftests/known_gaps/ | one chapter plays end to end by input |

### 16.6 Phase 5 — specification

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P5-2 | [RE] | Lara's state chart as data: transition table instead of per-move ports (C-CHART: OU-PLY-001, 002, 004, 006, 010) | docs/formats/ANIMGRAPH.md, docs/GAMEPLAY.md | transitions listed with conditions and addresses |
| P5-1 | [RE] | Remaining undecoded natives and chart states: `instance` 63 (OU-SCR-003), ch14 throne native (OU-PZL-015), fade to white (OU-SCR-005), flinch in water (condition 0x1F5), under-water third gate 0x69E270, LaraCombatCamera (0x16), AnchoredCamera | docs/re/SCRIPT_NATIVES.md, docs/re/CAMERA.md, docs/GAMEPLAY.md | each with a decoded rule or a recorded [U] |
| P5-4 | [doc] + [RE] | Tag or justify the 446 non-trivial untagged definitions of docs/re/db/untagged_paths.tsv, largest files first (`script/Script.cpp`, `game/Objects.cpp`, `game/Player.cpp`, `game/Combat.cpp`, `game/ai/Enemy.cpp`); mark save / loader / glue paths as having no tru.exe counterpart | src/game, src/script, tools/re/tag_inventory.py, docs/LIMITATIONS.md | re-run of tag_inventory.py: no untagged non-trivial gameplay path without a reason |

### 16.7 Phase 6 / 7 — engine fixes with evidence (all [code])

Each: the 7 policy answers in the commit, a regression test, `ctest -L protected` and the suites named in the
batch rules (`ProgressionReal|GameReal|StreamReal|^Game|selftest|replay|ChaptersReal`). Items whose tru.exe evidence
is already recorded come first.

| ID | Goal | Evidence needed | Bugs |
|---|---|---|---|
| P7-11 | Hazards as measured: fire volume repeats its 300 burn every 1.2-4.4 s; jellyfish first sting after ~2 s; roller burst then stop | REFERENCE_COMPARISON M103, M104, M79 (WineSweep10; M79 one trial: repeat under P3-9 rest first) | OU-SCR-006, OU-SCR-007, OU-CMB-004 |
| P7-9 | Audio: help on demand by chapter index (`sc_CC`); FMOD reverb, uber mover / lever sounds, async music load | docs/CHECKPOINTS.md "Objectives" (OU-AUD-006); P3-11 for the rest | OU-AUD-006, 002, 004, 005 |
| P7-10 | Gameplay leftovers: push speed to the M56 law; push / pull release conditions; loose-body damping; scuba slot; enemy jump-over / flyers / poacher rifle; motorcycle | M56 (in hand); 0x73F900 (OU-PLY-016); P3-3, P3-6, P3-7; Wine for OU-PHY-003 | OU-PHY-002, OU-PLY-016, OU-PHY-003, OU-SWM-003, OU-CMB-002, OU-VEH-001 |
| P7-5 | Rendering defects, the capture-backed ones first (bloom `c · a` term, two-sided translucent order, lit smoke) | docs/re/RENDER_PIPELINE.md §13, REFERENCE_COMPARISON "Rendering"; BUGS entries (OU-REN-006 needs RE) | OU-REN-010, 011, 012, 002, 003, 004, 005, 006, 007 |
| P7-3 | Water: Original-preset sea against the capture; spray over the dive centre; dry-deck rule per hull; under-water shafts | RENDER_PIPELINE §13.2 (OU-WAT-015); BUGS entries | OU-WAT-015, 002, 007, 008 |
| P7-12 | Checkpoint respawn at a fresh unit load: under-water intros, floorless l7_yggdrasil_end intros, yacht deck settle (25 pinned rows of docs/CHECKPOINTS.md) | Wine respawn at the same checkpoints | OU-SAV-002 |
| P7-6 | Progression blockers | docs/re/PUZZLE_BLOCKERS.md; P5-1; Natla grab lead 0x7296C0 / 0x7027B0 | OU-PZL-001, 008 (b), 012, 013, 015; OU-GRP-001 |
| P7-1 | Push-out remainder: air / hang / swim, deep-behind pull-out, walk-stop gap 66-68 vs 79, double-sided raycast | PROGRESS batch 8 open list; Wine | OU-PLY-012 remainder |
| P7-2 | Traversal after P3-2 / P5-2: running-jump ledge aim, chimney standoff, beam fall, ladder side jumps, free hang, wall-hug shelf climb-out, jump-to-target lead / speed cap, jump-query fall-damage override | P3-2, P5-2; 0x73EDC0, 0x702E90 | OU-PLY-001, 002, 003, 004, 005, 006, 009, 010, 017, 018 |
| P7-4 | Vulkan AO / RT per-object motion vectors; wind animation motion vectors | RENDERING.md temporal passes | OU-VK-002, OU-REN-008 |
| P7-7 | Camera: combat camera 0x16, AnchoredCamera, reveal blends, bike camera | P3-7, P3-8, P5-1 | OU-CAM-003, 004, 006 rest |
| P7-8 | UI screens and text: chapter titles, letterbox, confirmations, PDA sub-screens; Cyrillic font | docs/re/UI.md; P9-2 | OU-UI-002, 003 |

### 16.8 Phase 8 — self-tests

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P8-1 | [code] | Probes still missing: checkpoint position, HUD reticles / toasts, sound by sound | src/app/game/SelfTest.cpp, docs/SELFTEST.md "Not available" | probes used by a scenario; mutation check |
| P8-2 | [code] | One scenario driven through SDL events (keyboard and gamepad) rather than `Actions` | src/app/game/SelfTest.cpp, tests/selftests/ | scenario fails with the gamepad mapping broken |
| P8-3 | [code] | Self-test autopilot that fights (unblocks ch11 / ch18 combat legs) | src/app/game/SelfTest.cpp | OU-TST-002 closed |

### 16.9 Phase 9 — comparison

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P9-3 | [doc] | Rendering subsystem rows in docs/REFERENCE_LEVELS.md from the P9-1 classification now; audio and UI rows once P3-11 / P9-2 exist | tools/re/ref_levels.py | rows generated; `--check` passes |
| P9-2 | [Wine] | Side-by-side UI image set (menus, HUD, PDA) with measured layout differences | docs/GAMEPLAY.md "Menus", REFERENCE_COMPARISON M109 | M109 measured |
| P9-4 | [Wine] | More capture scenes for the rendering classification (under water, jungle, a cinematic) | tools/wine-ref/sessions/render_capture.sh, REFERENCE_COMPARISON "Rendering" | each new scene classified row by row |

### 16.10 Phase 10 — performance and stability

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P10-1 (W10-1) | [code] (run) | Fill BASELINE_PERF §5 from `tools/bench/run.sh --runs 2` on an idle machine | docs/audit/BASELINE_PERF_2026-11.md | §5 tables filled, `--compare` passes |
| P10-4 | run | Add the 0.59.0 / 0.60.0 lines to the sanitizer + validation log at each release (P10-2 recipe) | docs/BUILDING.md "Sanitizer + validation runs" | one dated line per release, zero reports |

### 16.11 Phase 11 — documentation

| ID | Flag | Goal | Files | Done when |
|---|---|---|---|---|
| P11-5 | [doc] | Status drift after batch 10: OU-TST-001 still says "11 parked known-gap scenarios" (1 left); REFERENCE_COMPARISON "Counts" (172 rows / 65 match) disagrees with its tables (180 rows / 73 match by tools/progress/progress.py); the wrong citation addresses of OU-DOC-003 in src comments | docs/audit/BUGS.md, docs/REFERENCE_COMPARISON.md, src/ comments | counts agree with the parser; OU-DOC-003 closed |
| P11-6 | [doc] | Batch 11 entry in the progress log | docs/audit/PROGRESS.md | entry written |
