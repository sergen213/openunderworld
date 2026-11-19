# Master brief compliance matrix and remaining-work queue

Date: 2026-11-16. State checked: `main` at `ad672cc` plus the uncommitted work of the agents still running
(FuncDetail, StructDb, ScriptNatives; their files are named, not judged final). This document walks the master
engineering brief section by section (preservation policy, Phases 0-11, the 25 Phase 11 deliverables, the 30
execution rules, the first assignment and its report questions). For every requirement it gives a status, the
deliverable that satisfies it (repo-relative path) and the concrete gap. Section 16 turns the gaps into a prioritised,
separable work queue for Phases 2-11.

This document changes nothing else. It is an index over existing evidence; numbers are quoted from the cited files
(their dates apply), not re-measured here.

## Legend

| Status | Meaning |
|---|---|
| **Met** | A deliverable exists that satisfies the requirement as written; the path is the evidence. |
| **Partly** | Something exists, but a named part of the requirement is missing; the gap says what. |
| **Not met** | Nothing satisfies it yet. |
| **In progress (X)** | Agent X is producing it now; files may be uncommitted. |
| **N/A** | Not applicable, with the reason. |

Paths: `docs/…`, `src/…`, `tests/…`, `tools/…` are in the repo. `work/…` = `/mnt/1st4TB/Projects/openunderworld-work/…`
(private, outside the repo; bulky or game-derived artefacts). "Wine" marks work that needs the original tru.exe running
under Wine (the harness of docs/REFERENCE_COMPARISON.md "Method").

---

## 1. Non-negotiable preservation policy

### 1.1 Preserve existing work (protected systems)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Preserve SDL3; no GLFW/SFML/SDL2; no second window/input/lifecycle system | Met | docs/audit/PROTECTED_SYSTEMS.md "SDL3 platform layer"; BASELINE §7 | — |
| Preserve Jolt; do not assume CharacterVirtual / defaults equal the original | Met | PROTECTED_SYSTEMS "Jolt Physics"; docs/formats/PHYSICS.md "Character controller" (CharacterVirtual evaluated, 8/26 comparison failures, removed 84eb0be) | — |
| Water: preserve, audit and document before changing | Met (audit) | PROTECTED_SYSTEMS "Water"; docs/RENDERING.md water sections; docs/re/RENDER_PIPELINE.md §7 (original water passes) | Original-vs-ours water comparison is static only (no runtime capture of tru.exe water) |
| AO/RT: identify every technique, shader, option, quality, path; keep all | Met | PROTECTED_SYSTEMS "Ambient occlusion", "Ray tracing" (CORTAO default, `--ao rt`, `off`; RT shadows/reflections/AO/GI/local lights) | No AO image/regression test (BUGS OU-TST, PHASE01 §4) |
| Inventory every other improvement (shaders, lighting, shadows, reflections, optimisations, post, materials, textures, resources, physics, animation, camera, input, gameplay, audio, UI, perf) | Met | PROTECTED_SYSTEMS (28 systems, each with files, config keys, tests); PROJECT_AUDIT_2026-11 §5 "Improved beyond original" | — |

### 1.2 Establish a protected baseline (11 steps)

| # | Step | Status | Evidence | Gap |
|---|---|---|---|---|
| 1 | Inspect the Git repository | Met | PROJECT_AUDIT_2026-11 §1, §4 (750 commits, eras) | — |
| 2 | Tracked / untracked changes | Met | PHASE01_REPORT header (tree clean at 9034839) | — |
| 3 | Record current commit | Met | PHASE01_REPORT, BASELINE (9034839 = v0.51.0) | — |
| 4 | Preserve uncommitted work | Met | none existed; PHASE01_REPORT header | — |
| 5 | Preservation branch / snapshot | Met | branch `baseline/pre-audit-2026-11-16` (pushed) | — |
| 6 | Full subsystem inventory | Met | PROJECT_AUDIT §3 architecture map; PROTECTED_SYSTEMS | — |
| 7 | Build the project | Met | BASELINE §1 (GCC 16.2.1, LTO, 160 s) | — |
| 8 | Run all tests | Met | BASELINE §4 (997 selected, 980 pass, 0 fail, 17 skip) | `game_vulkan_rt_smoke` excluded (real GPU hang, OU-VK-001) |
| 9 | Build failures and warnings | Met | BASELINE §3 (268 unique warnings) | — |
| 10 | Screenshots and performance | Partly | BASELINE §6 (7 runs, frame mean/p95/max, GPU pass ms); shots in `work/re-baseline/Baseline/` | Only two scenes; no RAM/VRAM/CPU% (see Phase 10) |
| 11 | Record SDL3/Jolt/water/AO/RT/render functionality | Met | BASELINE §7 | Vulkan validation not active in the Release baseline run |

### 1.3 No unjustified replacements (7 questions before modifying)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Answer the 7 questions before modifying a subsystem | Partly | PROTECTED_SYSTEMS gives "how implemented / what works / tests" per system; BUGS.md entries give cause and correction | No per-change template; the queue in §16 requires each item to record the 7 answers in its commit/PR text |
| Replacement only with documented justification + migration | Met (so far) | no replacement proposed (PHASE01 §10 "Keep") | — |

---

## 2. Phase 0 — research and project audit

### 2.1 External research (0.1)

| Topic | Status | Evidence | Gap |
|---|---|---|---|
| TRU RE research, engine docs, modding projects | Met | docs/RESEARCH.md §4-§8 (TRLAU-menu-hook, Yura, ModLoader, BigfileLogger, cdcEngineTools, Templates); docs/audit/EXTERNAL_RESEARCH_2026-11.md §1 | — |
| File format docs, extraction tools | Met | RESEARCH §8; docs/formats/* cite cdcEngineTools where used | — |
| Previous native-port attempts | Partly | RESEARCH §8.7 (Joan of Arc port by the same author); EXTERNAL_RESEARCH §1 | No explicit "none found" statement for TRU native ports with the searches listed |
| Graphics/rendering research | Met | RESEARCH §8.8-§8.10 | — |
| Animation/gameplay investigations | Partly | EXTERNAL_RESEARCH §3 (speedrun leads, unverified) | Leads not yet checked against tru.exe (queue W9-3) |
| Patches and executable differences | Met | RESEARCH §2-§3; EXTERNAL_RESEARCH §2 (detection recipe, no other-edition hashes published) | Other editions not in hand |
| Relevance + "do not trust without verification" | Met | RESEARCH §5.3 checks TRLAU addresses against our exe | — |

### 2.2 Repository audit (0.2) — 27 items

All 27 listed aspects are covered by docs/audit/PROJECT_AUDIT_2026-11.md (§2 build system, compiler, C++23; §3
architecture map) together with PROTECTED_SYSTEMS (one section per subsystem: SDL3, Jolt 5.6.0, Vulkan/GL, shader
pipeline, water, RT, AO, lighting/shadows, reflections, materials/textures, animation, player controller via GAMEPLAY
references, camera, level/asset loading, audio, UI, input, self-tests) and BASELINE §2 (toolchain/dependency
versions). Debug tools and perf monitoring: RENDERING.md (`OU_VK_PRESENT_STATS`, overlay), BASELINE §6. Git history
eras: PROJECT_AUDIT §4.

| Item group | Status | Gap |
|---|---|---|
| Architecture, structure, build, compiler, C++ standard, SDL3 version, Jolt version | Met | — |
| Graphics API, shaders, shader compilation pipeline, water, RT, AO, lighting, shadows, reflections, materials | Met | Doc drift: RENDERING.md:1110 default preset, ROADMAP "Materials" row (OU-DOC-001/002) |
| Animation, player controller, camera, level/asset loading, audio, UI, input | Met | — |
| Test framework, debug tools, perf monitoring | Met | — |
| Git history / non-obvious improvements | Met | PROJECT_AUDIT §6: a row-by-row ROADMAP drift pass not done |

### 2.3 Bug inventory (0.3)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Reproduce before fixing | Partly | docs/audit/BUGS.md "Reproduction log" (BugRepro pass, ad672cc) | Index at a80bdb8 had 12 entries "needs repro"; see BUGS.md for the current count |
| 10 fields per bug (ID, subsystem, steps, expected, actual, files, suspected cause, confirmed cause, correction, regression test) | Met | BUGS.md "Legend" and "Entries" (IDs `OU-SUBSYS-NNN`) | Some entries carry no regression test yet (field says so) |
| Pre-existing bug vs regression | Met | BUGS.md legend (regression flag), e.g. OU-VK-001 regression 671e35e→v0.41.0 | — |

### 2.4 Audit deliverable (0.4) — 9 buckets

| Bucket | Status | Evidence |
|---|---|---|
| 1 Works, 2 Partial, 3 Broken, 4 Missing, 5 Improved, 6 Must-protect, 7 Needs RE, 8 Should repair, 9 Safe to reimplement | Met | PROJECT_AUDIT_2026-11 §5 (all nine buckets), summarised in PHASE01_REPORT §1-§10 |

---

## 3. Phase 1 — forensic binary analysis

### 3.1 Byte-level examination (1.1)

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Tools: Ghidra + scripting, objdump, strings, xxd, Python, PE parser, debuggers, runtime instrumentation | Met | tools/ghidra/*.sh + scripts/*.java; tools/re/pe_inventory.py (pefile); objdump listings in `work/re-baseline/*`; Wine memory helper `work/gameplaygoal/reference/ref2.c` | readelf/llvm-objdump N/A (no ELF originals) |
| File format, architecture, entry point | Met | docs/re/BINARIES.md §4.0 | — |
| Section layout and permissions | Met | BINARIES §4.1 (tru.exe sections), §4.2-§4.7 per DLL | — |
| Imports / exports | Met | BINARIES §4; `tools/ghidra/scripts/ExportImports.java` | — |
| Relocations | Met | BINARIES §5.2 (tru.exe: none; DLLs listed) | — |
| Resources | Met | BINARIES §4 per PE | — |
| Embedded data, packed/compressed regions, embedded archives | Met | BINARIES §5.3 (FMOD Ex, NavPower, libjpeg statically linked; entropy scan; binkw32 = ASI loader with 4 embedded Bink builds) | libjpeg functions not located (FUNCTION_DATABASE open questions) |
| Strings | Met | BINARIES §5.5; `ExportStrings.java`; docs/re/db/string_refs.tsv (StructDb, uncommitted) | — |
| Function boundaries | Met | docs/re/db/functions.tsv (18,766 functions, `entry`, `size`) | — |
| Virtual tables, RTTI | Met / In progress (StructDb) | BINARIES §5.4; docs/re/db/rtti.tsv; classes.tsv, class_hierarchy.tsv (uncommitted) | — |
| Exception handling | Met | BINARIES §5.2 (EH prologs, FuncInfo 0x19930522 ×43, 36 handler stubs) | FuncInfo tables not decoded per function (low value) |
| Global data | In progress (StructDb) | docs/re/STRUCTURES.md "Globals", db/globals.tsv (uncommitted) | — |
| Cross-references | Met / In progress | `tools/ghidra/scripts/Xrefs.java`, ExportGraph.java; db/xrefs_summary.tsv (uncommitted) | — |
| External module dependencies | Met | BINARIES §1, §5.3; db/module_deps.tsv (uncommitted) | — |
| Record offsets/sizes/signatures; preserve originals; SHA-256 | Met | BINARIES §1-§3 (all 23 package files MD5-match GOG hashdb; SHA-256 table; manifest in `work/re-baseline/BinaryInventory/`) | — |
| Every relevant DLL/module, not only the exe | Met | BINARIES §4.2-§4.7 (D3D9.dll, dxwrapper, DEPHook, binkw32, fmodex (unused), unins000) | — |

### 3.2 Comprehensive Ghidra analysis (1.2)

| Per-function item | Status | Evidence | Gap |
|---|---|---|---|
| Address, size, boundaries | Met | db/functions.tsv `entry`, `size` | — |
| Assembly analysed | Partly | FUNCTION_DATABASE "Verification sample" (11 citations checked against assembly); RENDER_PIPELINE objdump-verified paths | Instruction-level verification exists only for cited hot paths |
| Decompiler output | Met (artefact) | `tools/ghidra/scripts/DecompileAll.java`; output `work/ghidra/decomp/` | Not in repo (size, game-derived) |
| Arguments, calling convention, return type | In progress (FuncDetail) | db/functions.tsv columns `cc`, `params`, `stack_args`, `ret_guess` (uncommitted, ExportFunctionDetail.java) | — |
| Callers, callees | Met | db/functions.tsv `callers`/`callees`; edges in `work/re-baseline/FunctionDb/` | Indirect/virtual calls: In progress (FuncDetail: `vcall_edges.tsv`) |
| Control flow | Partly | `switches`, `switch_cases` columns (FuncDetail) | No per-function CFG export |
| Global data access, constants, offsets | In progress (FuncDetail) | `g_reads`, `g_writes`, `consts`, `this_offsets` columns | — |
| Structure references, class relationships | In progress (StructDb) | STRUCTURES.md, structures.tsv, this_offsets.tsv | — |
| State transitions | Partly | GAMEPLAY.md (Lara state chart), ANIMGRAPH.md, SCRIPTS.md | Not linked function-by-function in the DB |
| Confidence levels | In progress (FuncDetail) | `confidence` column | — |
| Rename functions; keep address + name | Met | tools/ghidra/names/tru_gog.csv, `ApplyNames.java` (168 functions + 23 labels applied) | 14.5 k functions still `FUN_*` (17.5 % code bytes cited) |
| Scripts: inventories, call graphs, xref reports, decompiled exports, structures, globals, string refs, module deps | Met / In progress | ExportFunctions/ExportGraph/Xrefs/DecompileAll/ExportStrings/ExportRtti/ExportImports.java; tools/re/function_db.py; tools/re/struct_db.py (uncommitted) | FID pass not run (library tags rest on address runs) |
| Verify critical findings against assembly | Partly | FUNCTION_DATABASE verification sample; STRUCTURES "Instruction checks" | Systematic for citations not done |

### 3.3 Internal engine architecture (1.3)

docs/re/ENGINE_ARCHITECTURE.md has one section per subsystem (§1-§26), each with main functions (addresses),
data structures, dependencies, state/constant notes and the Linux counterpart. The brief's 33 subsystems map onto it:

| Brief subsystem | Section | Status |
|---|---|---|
| Init, shutdown | §1, docs/re/STARTUP.md | Partly: shutdown unknown |
| Main loop, timing | §2, docs/re/TIMING.md | Met |
| Rendering pipeline, graphics resources | §18, docs/re/RENDER_PIPELINE.md | Met (static, objdump-verified) |
| Scene/world, level loading, asset loading | §3, §4, docs/re/FILESYSTEM.md | Met |
| Entity management | §5 | Met |
| Player controller, climbing/traversal | §6, §12, docs/GAMEPLAY.md | Met (reconstructed approximation) |
| Physics, collision | §7 | Met (cdc multibody → Jolt mapping) |
| Animation, skeleton | §8 | Met |
| Camera | §9 | Partly: camera-manager transitions open |
| Cinematics | §10 | Met |
| Interaction, environment interactions | §11, §16 | Met |
| Combat/targeting, weapons/inventory | §13, §14 | Partly: inventory layout unknown |
| Enemy AI | §15 | Partly: NavPower internals replaced by our A* |
| Particles | §17 | Met |
| Audio | §20 | Met |
| UI | §21 | Partly: cdc UI widget classes mostly uncited |
| Save/load, checkpoints | §22 | Partly: original `.sav` layout [U] (docs/SAVES.md "The original game's saves") |
| Scripting, events | §23 | Partly: native binding table built at runtime — In progress (ScriptNatives) |
| Input, configuration, localisation | §24, §25, §26 | Met |

---

## 4. Phase 2 — asset and data format reconstruction

### 4.1 Asset categories

Coverage of the 10 per-format items is coded as: H header, O field offsets, S sizes, T types, E endianness stated,
C compression, I indexing, R relationships between assets, X runtime interpretation (tru.exe addresses), V validation
requirements. "–" = absent, "(i)" = implicit only (e.g. little-endian assumed, not stated).

| Brief category | Doc(s) | Items covered | Inspection tool | Round-trip test | Status / gap |
|---|---|---|---|---|---|
| Archive formats | docs/formats/CONTAINERS.md §1-§2 (bigfile, patch.000) | H O S T E C I R X V | `ou-import` (docs/formats/INVENTORY.md), `ou-asset sections` | `Importer.SyntheticRoundTripWithSplitParts` (synthetic write → scan) | Met; no byte-identical re-pack of a real bigfile |
| Container structures | CONTAINERS §3-§4 (CDRM, DRM v19, sections, relocations) | H O S T E C I R X V | `ou-asset sections` | `Cdrm.RoundTripMixedBlocks` (synthetic) | Partly: no DRM serialise → parse identity test on real files |
| File indexes | CONTAINERS §1.3 path hash (CRC-32/BZIP2), §1.4 spec mask, §1.5 inventory | H O T I X | `ou-import` name recovery | `Bigfile` hash check value in test_cdc | Met |
| Compression | CONTAINERS §3 (CDRM zlib blocks); AUDIO §1 (IMA ADPCM); TEXTURES (BCn); VIDEO (Bink 1) | C documented per format | — | CDRM synthetic only | Partly: no real-data recompress identity |
| Texture formats | TEXTURES.md (PCD9, 24-byte header, channel order) | H O S T E C – R (i)X V | `ou-asset textures`, `dump-texture` (TGA/DDS) | – | Partly: thin X (4 addresses); no PCD9 re-encode identity test; cube/volume and "open questions" remain |
| Mesh formats | MESHES.md (render model type 26, terrain 24, VertexDecl) | H O S T (i)E – I R X V(survey) | `ou-asset dump-mesh`, `stats`, `render` | – | Partly: endianness implicit; validation only as a survey |
| Skeleton structures | ANIMATION.md "Skeleton", "Skin weights", hair chains | H O S T E – I R X | ou-viewer; `ou_anim_tests` | – | Met (decode); no dump tool |
| Animation data | ANIMATION.md clip section type 2; ANIMGRAPH.md | H O S T E C(key compression) I R X | ou-viewer | – | Met; no clip re-encode |
| Material definitions | MATERIALS.md (type 10 blob, blend modes 0x527A80) | H O S T (i)E – – R X | – | – | Partly: indexing/validation thin; no material dump tool |
| Shader resources | SHADERS.md (ShaderLib type 9, SM3 bytecode, translation) | H O T (i)E – I R X V(5,594 translate) | `ou-asset shaders`, `shader-dis` | `Sm3Real.EveryInstallShaderTranslates` (decode-only) | Met |
| Collision geometry | COLLISION.md (cdc Mesh 0x60, game BIH, markup2); PHYSICS.md (object primitives) | H O S T (i)E – I R X V | – (tests: CollisionRealData.*) | – | Partly: no collision dump tool; endianness implicit |
| Level geometry | MESHES.md "Terrain"; LEVELS.md (unit DRM, intros, IMF, stream groups, portals, splines, nav) | H O S T E – I R X V | `ou-asset --level` | – | Met; NavPower resource layout partial (LEVELS "Nav graphs") |
| Lighting data | LEVELS.md "Lights", "Irradiance volume"; MESHES/MATERIALS light maps; SHADERS "Engine inputs" | O T R X | – | – | Partly: scattered over 4 docs; no single lighting-data format section; light-map atlas rules (OU kelp fix) only in code |
| Audio | AUDIO.md (ADPCM, FSB4, Wave type 6, `.mul`, `.mus/.sam`, SoundPlex) | H O S T E C I R X V(verification log) | `ou-audio` | – | Met; no FSB/`.mul` re-encode identity |
| Cinematics | CINEMATICS.md (CINE header 0x4537A0, frames, channels); VIDEO.md (Bink) | H O S T E C(i) I R X V(tests) | `ou-movie --info`; `ou-game --cine` | – | Met |
| Scripts | SCRIPTS.md (type 8 ScriptTypeStreamData, bytecode VM, survey) | H O T – – I R X V(survey) | survey test `GameReal.ScriptSurvey`; `tools/re/script_types_dump.cpp` (ScriptNatives, uncommitted) | `ScriptRuntime.StateRoundTrip` (runtime state, not the format) | Partly: no standalone bytecode disassembler tool; natives In progress (ScriptNatives) |
| Localisation | TEXT.md (locals.bin, `.sch`, cinstream subtitles) | H O T – – I R X V(tests) | `ou-text dump/get/langs/movie` | – | Partly: endianness not stated; no locals.bin re-encode identity |
| UI graphics and fonts | **none in docs/formats**; layout in `src/ui/Art.hpp` header comment (font DTP 46, glyph record, 0x4758C0/0x475350), docs/GAMEPLAY.md "Menus" | H O T X (in code comment) | – | – | **Not met** as documentation: no format doc for the font DTP, generalbank.drm UI atlas, g_pda.drm icons or cdc RAW full-screen images (`pc-w\image\*.raw`) |

Additional formats documented beyond the brief list: EFFECTS.md (GenericFXObject, emitters), PHYSICS.md (physical
objects, hinge doors), ANIMGRAPH.md, INVENTORY.md (install inventory), docs/SAVES.md (our save format).

### 4.2 Phase 2 cross-cutting requirements

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Document the 10 items for every format | Partly | table above | Endianness stated in 6 of 17 docs only; explicit "validation requirements" (what a loader rejects) mostly expressed as tests (`Cdrm.TruncationThrows`, `Texture.CubeFacesAndBadSizes`, `Save.RejectsCorruptAndTruncated`), not written down per format |
| Extraction/inspection utilities | Partly | `ou-asset` (sections, textures, dump-texture, dump-mesh, stats, render, shaders, shader-dis), `ou-audio`, `ou-text`, `ou-movie`, `ou-import`, `ou-imgdiff` | No tool for collision, animation/skeleton, materials, scripts (bytecode listing), cinematics channels, UI art/font |
| Round-trip tests for re-encodable formats | Partly | synthetic: CDRM, bigfile (importer); our own formats: Save, Config, script runtime state | No byte-identical decode → encode test on real game data for DRM sections + relocations, PCD9, locals.bin, CDRM, bigfile index |
| Preserve originals; no approximations; never overwrite game files | Met | BINARIES §2 (install hash-verified); ou-import reads only; game data read from bigfiles at runtime | — |

---

## 5. Phase 3 — original game runtime investigation

Harness: tru.exe under Wine in a nested compositor, memory helper (`trace`, `tp`, `tpv`, `CAMFOLLOW`, `god`,
`wmem goto`) — docs/REFERENCE_COMPARISON.md "Method"; sessions in `work/gameplaygoal/reference/`,
`work/poleattach/`, `work/camfollow-wine/`, `work/waterentry-wine/`, `work/yachtwine/`, `work/musicwine/`,
`work/goofix-wine/`, `work/wine` (prefix), captures in `work/screens/`.

| Instrumentation (brief list) | Status | Evidence | Gap |
|---|---|---|---|
| Breakpoints / debugger instrumentation | Partly | hooks on 0x5E8210 (camfollow-wine); winedbg sessions (memory notes) | No reusable breakpoint script set in the repo |
| Memory inspection, runtime variables | Met | ref2.c / pt.c (Instance +0x20, +0x2A0, state pointer, anim id) | Helper sources live in `work/`, not the repo |
| Function tracing | Partly | targeted hooks only | — |
| API monitoring (D3D9, FMOD, input) | **Not met** | — | No D3D9 call trace (apitrace/DXVK capture) or FMOD event trace |
| Input recording | Partly | SendInput scripted presses (`trace <ms> vk:down:up`) | Playback only; no recording of a human session |
| Frame captures | Partly | `work/screens/` (UI, chapters, fxref, enemies, …) | Screenshots only; no per-draw frame capture |
| Performance profiling of the original | Not met | — | — |

| Investigated area | Status | Evidence | Gap |
|---|---|---|---|
| Player controller: walk, run, accel/decel, jump, fall, land | Met | REFERENCE_COMPARISON "Movement" (speeds within 1 %, jump heights within 0.2, gravity 5.72443/frame²) | Ground turn rates, stop distance, walk start/stop unmeasured (M94) |
| Climbing, ledge grab, mantling, wall interactions | Partly | "Ledges and climbing", chimney M06 measured | Shimmy / ladder / free-climb rates unmeasured (M01, M02, M05, M96, M12) |
| Swimming, diving, underwater movement | Partly | surface swim turn 117.5°/s, water entry (waterentry-wine) | Under-water turn/pitch, swim start/stop curve, breath time unmeasured (M29, M85) |
| Object manipulation, combat movement | Partly / Not met | push block law fitted [I] | Push/pull, every combat timing unmeasured (M28-M46, M52-M62) |
| Animation: selection, state machines, blending, root motion, timing, transitions | Partly | clip start = start − blend (0x441FB0) measured on take-offs; ANIMGRAPH; ANIMATION | Other clips' start frames unmeasured; interruption/sync not measured |
| Physics: gravity, integration, collision, ground, slopes, friction, momentum, jump | Partly | gravity, slopes (Wine ramp series), step bracket, capsule measured | Friction/momentum on surfaces, moving platforms, ropes unmeasured |
| Ropes, interactive objects, water interactions, environmental physics | Partly | yacht sin waves, loose-body damping 0x4EFB20 (static), water entry measured | Grapple swing (5 failed Wine attempts), bike entirely unmeasured |
| Camera: position, follow, rotation, smoothing, collision, constraints, transitions, tracking, climbing/combat/swimming | Partly | camfollow-wine; CAMFOLLOW pins; docs/GAMEPLAY.md "Cameras" decoded | Camera transitions and per-mode behaviour not measured live |
| Rendering: geometry … render ordering (15 items) | Partly | docs/re/RENDER_PIPELINE.md (static pass order, shadows, water passes, lighting limits); SHADERS.md; EFFECTS.md | All static/decoded; no runtime frame capture of tru.exe to confirm pass order, LOD switching or transparency sorting |
| Interface/progression: menus, options, inventory, HUD, loading, pause, checkpoints, save/load, transitions, cinematics, unlockables, progression | Partly | `work/screens/ui`; docs/GAMEPLAY.md "Progression … menus"; docs/formats/VIDEO.md §2, TEXT.md §4 | UI compared by eye only (M109); original save format [U]; unlockables not compared |
| Reproducible docs with measurements | Met | trace files named per row in REFERENCE_COMPARISON | — |

---

## 6. Phase 4 — walkthrough-based level validation

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Use tombraiders.net/stella/underworld.html and its chapter pages | Met | docs/underworld_chapter_gameplay_matrix.md (header cites it); every docs/matrix/NN_*.md starts with `Walkthrough: …/walks/TR8walk/NN<chapter>.html` and lists the sub-pages read (21/21 files) | — |
| Chapter names, level names | Met | docs/matrix file names and headers (21 sections); "Our units" line per chapter | — |
| Objectives | Partly | "Chapter" and "Progression" category rows per chapter | No explicit objective list per chapter (0 files use an objective field); the game's own PDA objective strings not tied in |
| Locations, required interactions, traversal sequences | Met | requirement tables `CNN-xx` in route order, with unit / object uid | — |
| Puzzles | Met | "Puzzles" category row in 21/21 | — |
| Collectibles | Met | "Collectibles" row in 21/21 (treasure/relic/health totals from the page header and the game's reward table `ChaptersReal.RewardSurveyMatchesTheRewardTable`) | — |
| Important objects | Met | "Objects" row with uids | — |
| Checkpoints | Partly | mentioned in 21/21; docs/PROGRESSION.md "Chapter routes", "Gates" | No per-chapter checkpoint list (uid, position) with a test per checkpoint ("per-chapter checkpoints unverified", matrix 0.45.0 row) |
| Level transitions, progression conditions | Met | docs/PROGRESSION.md "Gates" (251 gates), `ProgressionReal.CriticalPathInOrder`, `CriticalGates` | — |
| Chapter-by-chapter validation scenarios | Partly | 80 `chNN_*` self-tests carrying the walkthrough URL; 14 in `tests/selftests/known_gaps/` | No chapter plays end to end by self-test; matrix rows: 18 VERIFIED AGAINST ORIGINAL (count of that string in docs/matrix) |
| Walkthrough not used as mechanics source | Met | matrix header: rows are our own technical descriptions; mechanics cite tru.exe/Wine | — |
| Freshness | Partly | matrix "Status changes since the baseline" up to 0.46.0 | PHASE01 §4 / OU-DOC: rows still marked NOT IMPLEMENTED that are implemented |

---

## 7. Phase 5 — original engine specification

| # | Item | Status | Evidence | Gap |
|---|---|---|---|---|
| 1 | Executable module map | Met | docs/re/BINARIES.md §1, §4; db/module_deps.tsv (StructDb, uncommitted) | — |
| 2 | Function database | Met / In progress (FuncDetail) | docs/re/FUNCTION_DATABASE.md, db/functions.tsv | per-function detail columns uncommitted |
| 3 | Call graphs | Partly / In progress (FuncDetail) | 43,097 direct edges in `work/re-baseline/FunctionDb/`; db/vcall_edges.tsv | Edge lists not in the repo; no rendered per-subsystem graphs |
| 4 | Data structures | In progress (StructDb) | docs/re/STRUCTURES.md, db/structures.tsv, classes.tsv | — |
| 5 | Engine architecture | Met | docs/re/ENGINE_ARCHITECTURE.md | — |
| 6 | Main game loop | Met | ENGINE_ARCHITECTURE §2; docs/re/TIMING.md; docs/re/STARTUP.md | — |
| 7 | Rendering architecture | Met | docs/re/RENDER_PIPELINE.md | Runtime confirmation (Phase 3) |
| 8 | Physics architecture | Met | ENGINE_ARCHITECTURE §7; docs/formats/PHYSICS.md | cdc multibody internals partly decoded |
| 9 | Player controller | Met | ENGINE_ARCHITECTURE §6; docs/GAMEPLAY.md | — |
| 10 | Animation system | Met | §8; ANIMATION.md, ANIMGRAPH.md | — |
| 11 | Camera system | Partly | §9; GAMEPLAY "Cameras" | Transition manager open |
| 12 | Interaction system | Met | §11 | — |
| 13 | Audio system | Met | §20; AUDIO.md | — |
| 14 | UI and menus | Partly | §21 | cdc UI widgets uncited |
| 15 | Save/load | Partly | §22; docs/SAVES.md | Original save layout [U] |
| 16 | Level management | Met | §4; LEVELS.md | — |
| 17 | Resource formats | Met (Partly per §4) | docs/formats/* | UI art/font doc missing |
| 18 | Scripting and events | Partly / In progress (ScriptNatives) | §23; SCRIPTS.md | native binding table |
| 19 | Runtime dependencies | Met | BINARIES §4-§5.3 | — |
| 20 | Unresolved questions | Met | ENGINE_ARCHITECTURE "Open questions across subsystems"; per-doc "Open questions" | Not consolidated (Phase 11 #25) |
| — | Windows-to-Linux mapping (11 brief rows) | Met | ENGINE_ARCHITECTURE "Windows → Linux mapping" (19 rows, covers all 11) | — |
| — | Every decision traceable or marked approximation | Partly | [O]/[I]/[U] tags across docs; FUNCTION_DATABASE citation harvest (6,305 citations) | 1,321 cited functions vs reconstructed systems; untagged code paths not inventoried |

---

## 8. Phase 6 — improve the existing SDL3 engine

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| No parallel engine / duplicate abstractions | Met | PROTECTED_SYSTEMS; PHASE01 §10 | — |
| 6.1 Preserve SDL3 init, loop, window, input, keyboard, mouse, gamepad, audio, events, platform, timing, display | Met | PROTECTED_SYSTEMS "SDL3 platform layer", "Input", "Fixed-step timing" | — |
| 6.1 Reproduce original input and timing behaviour | Partly | docs/re/TIMING.md; docs/audit/controls.md (input differences); jump-timing latency measured | Input differences in controls.md "Input differences found" not all closed; ground turn rates unmeasured |
| 6.2 Preserve Jolt; investigate the 15 listed aspects | Partly | PHYSICS.md "Character controller" (measurements, decision); fixed 60 Hz step; slopes, gravity, collision tolerances measured | Friction, moving objects, swimming physics, physics-animation sync only partly measured (see Phase 3) |
| 6.2 Measurements guide corrections, not tuning by eye | Met (policy in practice) | REFERENCE_COMPARISON "Differences to fix" each cites a trace | — |
| 6.3 Identify graphics APIs, architecture, pipeline, deps | Met | RENDERING.md; PROTECTED_SYSTEMS | — |
| 6.3 Preserve water, RT AO, other AO, lighting, shadows, reflections, materials, textures, post, optimisations, GPU resources | Met | PROTECTED_SYSTEMS | — |
| Water: original as reference, improve without regression | Partly | RENDER_PIPELINE §7 (original ripple map `cdc::PCWaterSurface` = new finding); BUGS OU-WAT-* | Open: OU-WAT-006 swell clamp, OU-WAT-007 dry deck, OU-WAT-002 spray |
| RT AO: quality, perf, resources, temporal stability, integration | Partly | BASELINE (+1.2 ms), RENDERING.md | Vulkan reprojection camera-only (PROJECT_AUDIT "Should repair"); no AO image test |
| Other AO: inventory, keep options, clean method selection | Met | `graphics.ao` auto/off/cortao/rt, `ao_quality` (PROTECTED_SYSTEMS "Ambient occlusion") | — |
| No silent default changes | Partly | defaults recorded in PROTECTED_SYSTEMS | Doc drift: RENDERING.md:1110 says Original is the default (OU-DOC-001) |
| Original vs enhanced kept as two goals | Met | `--water original|enhanced`, preset original/enhanced, Original preset byte-identity tests | — |

---

## 9. Phase 7 — bug fixing and root-cause analysis

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Central bug/regression database, unique IDs, subsystem, steps, expected (Windows), actual (Linux), RE evidence, root cause, correction | Met | docs/audit/BUGS.md (owned by BugRepro) | — |
| Implement correction + regression test + verify existing functionality | Not started (by design) | first assignment forbids fixes; BUGS "Triage" lists safe fixes | Queue §16 W7-* |
| Do not fix symptoms independently; root-cause clusters | Met (analysis) | BUGS "Root-cause clusters" (C-VM, C-JOINT, …) | — |
| Verify the 13 protected areas after each change | Partly | PROTECTED_SYSTEMS gives the test per system | No single "protected smoke" ctest label to run after every change (W10-4) |
| Small isolated changes | Met (history) | PROJECT_AUDIT §4 | — |

---

## 10. Phase 8 — automated self-test system

### 10.1 Invocation and report schema

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| `--self-test <file.json>` | Met | `ou-game --self-test FILE.json` (docs/SELFTEST.md "Running"; `src/app/game/SelfTest.cpp`) | — |
| Initialise real engine, load resources, enter playable state, scripted inputs, inspect real state, save results, exit | Met | SELFTEST.md intro; exit codes 0/1/2/77; report to `--self-test-report` | Self-tests run with `--draw-last --no-audio` under ctest, so render/audio paths are not exercised by them |
| Report `version` | Met | `SelfTest.cpp:967` (`1`) | — |
| Report `game` | Met | `"Tomb Raider: Underworld"` | — |
| Report `platform` | **Not met** | report has no `platform` key (`SelfTest.cpp:967-971`) | Add `"platform": "Linux"` (W8-1) |
| `steps[]` with `step`, `passed`, `detail` | Met | `SelfTest.cpp:951`; SELFTEST.md "Report" | Extra fields (measurements, end, expectations) are a superset — fine |
| Tests validate state, not that a call happened | Met | SELFTEST.md: "every step must say what has to be true afterwards"; malformed if a step has no expectation; mutation check (altered copies fail) | — |
| Preserve and expand existing self-tests | Met | 115 scenarios + 14 known gaps; 28 replays | — |
| Visual baselines where possible | Partly | `viewer_vulkan_matches_gl*` (GL-vs-VK parity), `ou-imgdiff` | No stored golden images; parity tests run with AO off |
| Run after meaningful builds | Met | ctest `game_selftest_*` (needs `OU_GAME_DIR` + display) | — |

### 10.2 Required test categories

"Self-test" = a `tests/selftests/*.json` scenario; "ctest" = GoogleTest/ctest elsewhere.

| Category | Status | Where | Gap |
|---|---|---|---|
| SDL3 initialisation | Partly | implicit in every windowed ctest/self-test (`game_input_replay_smoke`, `launcher_self_test`) | No explicit step asserting SDL subsystems (video, audio, gamepad) came up |
| Input | Partly | replays `tests/replays/*_keyboard.events`, `*_gamepad.events`; `game_input_replay_smoke`; self-tests drive the `Actions` path | Self-tests bypass SDL events; no gamepad self-test step |
| Window and rendering initialisation | Partly | ctest `RendererSelect.AutoFallsBackToOpenGl`, `VulkanRenderer.ResizeRecreatesSwapchain`, `viewer_*_screenshot` | Not a self-test step |
| Resource loading | Met | `RealData.*`, `Level.*` (test_formats) | — |
| Level loading | Met | every self-test entry; `StreamReal.*` | — |
| Jolt initialisation, physics simulation | Met (ctest) / Partly (self-test) | `ou_physics_tests` (21); self-tests `push_block`, `carry_throw`, `ch16_tower_power` | No explicit `physics` step as in the brief example |
| Movement | Met | `locomotion`, `sprint` | — |
| Jumping | Met | `locomotion`, `jump_timing`, `jump_brake` | — |
| Falling | Met | `fall_damage`, `safety_drop` | — |
| Landing | Met | `landing` | — |
| Collision | Met | `ledge`, `slide`, `chimney_floor_jumps`, step bracket via StairsWalk tests | Contact identity not probeable (SELFTEST "Not available") |
| Animation | Met | `step.clips`, `step.states` probes in most scenarios; `ik_pose`, `ik_feet` | — |
| Camera | Partly | `camera.*` probes in 4 scenarios; `camera` fixture | Camera mode name not probeable |
| Climbing | Met | `ledge`, `ladder`, `ladder_fast`, `pole`, `traversal_chain`, `saving_grab` | — |
| Interaction | Met | `ch01_bedroom_new_game` (switch 4525), `push_block`, `gauntlet_move`, `puzzle_pole_block_door` | — |
| Combat | Met | `combat`, `melee_kick`, `manual_aim`, `adrenaline`, `kraken_kill` | — |
| Inventory | Partly | `ch01_hall_first_aid` (pick up, use), `ch13_temple_headdress`, `portable.*` probes | Health packs carried not probeable; no inventory-menu step |
| HUD | Not met (self-test) / Partly (ctest) | ctest `Hud.*` (4 unit tests, test_hud.cpp) | SELFTEST.md "Not available": HUD state not probeable |
| Water shader compilation and rendering | Partly | ctest `WaterSurface.GlSourcesExpandAndCompile`, `SplashFluid.GlPassSourcesCompile`, `VulkanRenderer.WaveFieldRunsScrollsAndRests`, `viewer_vulkan_matches_gl_enhanced`; self-tests `water_entry`, `swim*` (gameplay surface) | No water image baseline; self-tests do not draw |
| AO shader compilation and rendering | **Not met** | no test references CORTAO or RT AO outside config parsing (`test_update.cpp`) | W8-2 |
| Ray-tracing initialisation | Partly | ctest `VulkanRenderer.RayTracingBuildsUpdatesAndTearsDown`, `…TracesOneBatchLod`, `…FallsBackToShadowMap` | `game_vulkan_rt_smoke` excluded (OU-VK-001) |
| Audio | Partly | `ou_audio_tests` (64), `ScriptRuntime.AudioStateFollowsMusicRequestsAndRoundTrips` | Self-tests run `--no-audio`; mixer not probeable |
| Save/load | Met (ctest) / Partly (self-test) | `Save.*`, `StreamReal.SaveContinuesBitIdentically`; self-test `load` entry key | `save`/`load` input lines ignored by self-tests |
| Level transitions | Met | `ch07_hubhall_statue_hall`, `ch20_helheim01_portal`, `ch18_engine_hall_to_cell`, `level` fixture | — |

---

## 11. Phase 9 — Windows vs Linux comparison

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Repeatable workflow | Met | REFERENCE_COMPARISON "Method" (Wine harness, coordinate offsets per unit, `session4.sh` template); ours via `--self-test-trace` | Harness sources in `work/`, not versioned in the repo |
| Identical start positions, inputs, game state | Met | per-unit offset table; `tp`/`tpv`; scripted SendInput vs self-test input | One unit per Wine session (documented limitation) |
| Player coordinates, velocity | Met | traces (+0x20, +0x2A0) | — |
| Camera coordinates | Partly | camfollow-wine | No camera trace comparison table |
| Animation states | Partly | anim id and chart state in traces | Not compared clip-by-clip outside take-offs |
| Physics measurements | Partly | gravity, slopes, chimney | — |
| Interaction states | Not met | — | M52-M62 unmeasured |
| Frame timing | Partly | docs/re/TIMING.md (timeMult, 30 Hz constants) | No frame-pacing capture of tru.exe |
| Audio event timing | **Not met** | — | No FMOD event trace |
| UI captures | Partly | `work/screens/ui`, compared by eye (M109) | No side-by-side image set |
| Level progression | Partly | chapter matrices; 18 rows VERIFIED AGAINST ORIGINAL | — |
| Frame captures / rendering: defect vs API difference vs intentional improvement | Partly | RENDER_PIPELINE §9 (comparison with ours); RENDERING.md fidelity notes | No capture set classified into the three classes |
| Not screenshots alone; measure | Met | REFERENCE_COMPARISON rows are numeric | — |
| Comparison matrix for every major subsystem **and level** | Partly | per-mechanic matrix: 161 rows, 53 match, 1 differs, 107 unmeasured (REFERENCE_COMPARISON "Counts") | No per-level (82-unit / 21-chapter) comparison matrix of measured behaviour; no rendering/audio/UI subsystem rows |

---

## 12. Phase 10 — performance, stability and regression protection

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| CPU utilisation baseline | Not met | — | — |
| GPU utilisation | Partly | GPU ms per pass (`OU_VK_PRESENT_STATS`, BASELINE §6) | No utilisation % |
| VRAM usage | Not met | — | Allocator arenas exist (RENDERING.md:167); no measurement |
| RAM usage | Not met | — | — |
| Frame time | Met | BASELINE §6 (mean/p95/max, 7 runs) | 2 scenes only |
| Frame pacing | Partly | p95/max | No frame-time histogram/pacing metric; FG pacing unmeasured in baseline |
| Physics simulation time | Partly | tick mean ms in BASELINE §6; physics ms printed per tick report | Not tabulated separately |
| Rendering time | Met | GPU pass ms | — |
| Shader compilation | Not met | pipeline cache documented (RENDERING.md:171, 1189) | No cold/warm compile timing |
| Resource loading | Not met | — | No level-load time baseline |
| Profile before optimising; keep HQ features; configurable options | Met (policy) | quality options (`ao_quality`, `--rt`, `--water`, FSR) | — |
| AO and water paths stable | Partly | BASELINE §7 | No AO regression test |
| Leaks, lifetimes, sync, GPU validation, physics instability | Partly | docs/BUILDING.md ASan/UBSan + LSan suppression (`tests/lsan_vulkan.supp`), validation-clean runs (BUILDING:203-211, RENDERING.md:210); deterministic physics tests | Not part of the v0.51.0 baseline; RT smoke GPU hang open (OU-VK-001) |
| Reproducible benchmark scenarios | Partly | BASELINE §6 command lines | Tick count follows wall time without `--input-script` (BASELINE §6 note): no fixed benchmark script |

---

## 13. Phase 11 — documentation and delivery

### 13.1 The 25 deliverables

| # | Deliverable | Status | Path |
|---|---|---|---|
| 1 | Existing project audit | Met | docs/audit/PROJECT_AUDIT_2026-11.md, docs/audit/PHASE01_REPORT.md (older: docs/EXISTING_PROJECT_AUDIT.md, AUDIT_A/B.md) |
| 2 | Protected subsystem inventory | Met | docs/audit/PROTECTED_SYSTEMS.md |
| 3 | Original binary inventory | Met | docs/re/BINARIES.md |
| 4 | SHA-256 hashes | Met | BINARIES §1, §3; manifest `work/re-baseline/BinaryInventory/` |
| 5 | Ghidra project | Met (outside repo) | `work/ghidra/project/tru.gpr`; reproducible via `tools/ghidra/analyze.sh` + `tools/ghidra/names/tru_gog.csv` |
| 6 | Decompiled function database | Met / In progress (FuncDetail) | docs/re/FUNCTION_DATABASE.md, docs/re/db/functions.tsv; pseudocode `work/ghidra/decomp/` |
| 7 | Disassembly documentation | Partly | docs/re/EXECUTABLE.md; FUNCTION_DATABASE "Verification sample"; RENDER_PIPELINE §12 (objdump reads); STRUCTURES "Instruction checks". No systematic per-subsystem disassembly notes |
| 8 | Call graphs | Partly / In progress | edges in `work/re-baseline/FunctionDb/`; db/vcall_edges.tsv (FuncDetail) |
| 9 | Reconstructed structures | In progress (StructDb) | docs/re/STRUCTURES.md, db/structures.tsv; layouts in docs/formats/* |
| 10 | Engine architecture specification | Met | docs/re/ENGINE_ARCHITECTURE.md (+ RENDER_PIPELINE, TIMING, STARTUP, FILESYSTEM) |
| 11 | Asset format documentation | Partly | docs/formats/*.md (17 docs); UI art/font missing (§4) |
| 12 | Asset inspection tools | Partly | tools/ou-asset, ou-audio, ou-text, ou-movie, ou-import, ou-imgdiff (gaps in §4.2) |
| 13 | Windows-to-Linux subsystem mapping | Met | ENGINE_ARCHITECTURE "Windows → Linux mapping" |
| 14 | Bug database | Met | docs/audit/BUGS.md |
| 15 | Regression test suite | Met | ctest (997 tests), tests/selftests, tests/replays |
| 16 | Updated SDL3 implementation | N/A so far | no SDL3 change required by the audit; preserved (PROTECTED_SYSTEMS) |
| 17 | Updated Jolt integration | N/A so far | preserved; changes follow measured differences (queue W6-*) |
| 18 | Preserved and improved water shaders | Met (preserved) / Partly (improved) | PROTECTED_SYSTEMS "Water"; open OU-WAT-* |
| 19 | Preserved and improved AO | Met (preserved) / Partly | CORTAO + RT + off; no AO test |
| 20 | Existing renderer documentation | Met (with drift) | docs/RENDERING.md; OU-DOC-001 |
| 21 | Automated self-test system | Met | docs/SELFTEST.md, src/app/game/SelfTest.*; gaps §10 |
| 22 | Original-versus-Linux comparison reports | Partly | docs/REFERENCE_COMPARISON.md; RENDER_PIPELINE §9; 107 rows unmeasured |
| 23 | Chapter-by-chapter validation | Partly | docs/underworld_chapter_gameplay_matrix.md, docs/matrix/*.md, docs/PROGRESSION.md, `chNN_*` self-tests |
| 24 | Build and installation instructions | Met | docs/BUILDING.md, docs/ENVIRONMENT.md, docs/LAUNCHER.md, docs/RELEASING.md |
| 25 | Remaining limitations and unresolved questions | Partly | PHASE01_REPORT §9, ENGINE_ARCHITECTURE open questions, per-doc "Open questions", ROADMAP, SELFTEST known gaps — no single consolidated register |

### 13.2 Other Phase 11 requirements

| Requirement | Status | Evidence | Gap |
|---|---|---|---|
| Persistent documentation throughout | Met | docs/, CHANGELOG.md | — |
| Originals, RE artefacts, extracted data, generated files, source kept separate | Met | game dir untouched; RE artefacts `work/ghidra`, `work/re-baseline`; DB under docs/re/db; source src/ | — |
| Reproducible build instructions | Met | docs/BUILDING.md | — |
| Linux launcher + GitHub update mechanism that verifies releases, preserves settings and game data | Met | docs/LAUNCHER.md, docs/UPDATES.md (Ed25519-signed AppImage updater), PROTECTED_SYSTEMS "Launcher", "Updater" | — |

---

## 14. Execution rules (30)

| # | Rule | Status | Evidence / note |
|---|---|---|---|
| 1 | Preserve the project | Met | snapshot branch; audit-only commits |
| 2 | Preserve uncommitted work | Met | none at baseline; agents commit only their own files |
| 3 | Preserve SDL3 | Met | PROTECTED_SYSTEMS |
| 4 | Preserve Jolt | Met | PROTECTED_SYSTEMS |
| 5 | Preserve water shaders | Met | no shader edits in Phase 0/1 |
| 6 | Preserve RT AO | Met | idem |
| 7 | Preserve other AO | Met | idem |
| 8 | Preserve other improvements | Met | PROTECTED_SYSTEMS (28) |
| 9 | No replacement without justification | Met | PHASE01 §10 |
| 10 | No random bug fixes first | Met | Phase 0/1 changed no engine code |
| 11 | Don't assume existing RE is correct | Met | mislabels found (PHASE01 §7: 0x67ED20, 0x61DD10, 0x417F30); RENDER_PIPELINE §10, STRUCTURES "Mismatches", BINARIES §8 corrections |
| 12 | Binary analysis beyond strings/imports | Met | function DB, call graph, RTTI, structures, pass order |
| 13 | No "complete decompilation" claim | Met | PHASE01 §7 "Honest limit" (17.5 %) |
| 14 | Verify against assembly and runtime | Partly | 11 citations asm-checked; Wine measurements for 58 comparison rows; most citations unchecked |
| 15 | Don't invent structures or mechanics | Partly | [O]/[I]/[U] tagging; some fitted laws marked [I] (e.g. push block) — keep them flagged until measured |
| 16 | No placeholders when assets decode | Partly | real data used throughout; open: UI art/font undocumented, `texture 2 not found` (BASELINE §8), skipped sections logged per load |
| 17 | Don't simplify gameplay for convenience | Partly | known gaps listed honestly (SELFTEST known_gaps); e.g. our A* stands in for NavPower (documented approximation) |
| 18 | No unnecessary dependencies | Met | deps listed BASELINE §2; vendored CORTAO MIT |
| 19 | No parallel SDL3/physics | Met | — |
| 20 | No silent default changes | Partly | OU-DOC-001 drift (docs disagree with code default) |
| 21 | Don't remove modern features | Met | — |
| 22 | No fake/superficial tests | Met | SelfTest rejects steps without expectations; mutation checks (SELFTEST.md end) |
| 23 | Don't mark fixed without verification | Met | BUGS.md "fixed and verified" requires a test |
| 24 | Commit incrementally | Met | per-deliverable commits (d57055a, 81f14f5, 629d8cd, ad672cc, …) |
| 25 | Persistent engineering progress report | Partly | PHASE01_REPORT, CHANGELOG.md, ROADMAP.md, GAMEPLAY_DEVELOPMENT_REPORT.md — no single running log for this brief's phases (this file's §16 can serve as the queue; a dated progress log is W11-2) |
| 26 | Document every significant discovery | Met | e.g. ripple map (RENDER_PIPELINE §7.2), sin waves, Anti-Lag crash |
| 27 | Record unresolved questions | Met | open-question sections everywhere; consolidation W11-1 |
| 28 | Verify protected functionality after changes | Partly | tests named per system; no single protected-smoke label (W10-4) |
| 29 | Root-cause over workarounds | Met (policy) | BUGS clusters |
| 30 | Buildable and recoverable | Met | build + ctest green at baseline; snapshot branch |

---

## 15. First assignment

### 15.1 The five prohibitions

No new gameplay features, no renderer rewrite, no SDL3/Jolt replacement, no water/AO modification during the audit:
**Met** — Phase 0/1 commits (d57055a … ad672cc) touch only `docs/`, `tools/re/`, `tools/ghidra/` and analysis data.

### 15.2 The 13 items

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Audit the complete repository | Met | PROJECT_AUDIT_2026-11 |
| 2 | Identify existing improvements | Met | PROJECT_AUDIT §5, PHASE01 §2 |
| 3 | Identify all protected systems | Met | PROTECTED_SYSTEMS |
| 4 | Build the current implementation | Met | BASELINE §1 |
| 5 | Record test results | Met | BASELINE §4-§5 |
| 6 | Identify original executable versions | Met | BINARIES §5; EXTERNAL_RESEARCH §2 (GOG 1.1, FileVersion 1.1, VLV block) |
| 7 | Inventory every relevant binary | Met | BINARIES §1, §4 |
| 8 | SHA-256 hashes | Met | BINARIES §1, §3 |
| 9 | Import into Ghidra | Met | BINARIES §3 (input hash match); `work/ghidra/project` |
| 10 | Systematic disassembly and decompilation | Partly / In progress (FuncDetail) | DecompileAll export; function detail columns; asm verification sampled only |
| 11 | Function and data-structure inventory | Met (functions) / In progress (StructDb) | FUNCTION_DATABASE; STRUCTURES |
| 12 | Start reconstructing engine architecture | Met | ENGINE_ARCHITECTURE, RENDER_PIPELINE |
| 13 | Relationships original ↔ Linux | Met | ENGINE_ARCHITECTURE mapping; FUNCTION_DATABASE citations |

### 15.3 The 13 report questions (PHASE01_REPORT)

| Question | Answered in | Status |
|---|---|---|
| What already exists | §1 | Met |
| What has been improved | §2 | Met |
| What currently works | §3 | Met |
| What is broken | §4 | Met |
| Which subsystems must be protected | §5 | Met |
| Which original binaries were identified | §6 | Met |
| How much binary analysis is complete | §7 | Met |
| Which original functions have been reconstructed | §7 (counts) + FUNCTION_DATABASE / db/citations.tsv (list) | Met |
| Which engine systems are understood | §8 | Met |
| What is still unknown | §9 | Met |
| Which current implementations should be retained | §10 | Met |
| Which bugs can be addressed safely | §10 | Met |
| Next reverse-engineering priorities | §11 | Met |

---

## 16. Prioritised work queue (Phases 2-11)

Order follows "audit before modify": documentation/measurement items that unblock fixes come first; engine changes
come after their evidence exists and must name the protected-system tests they re-run (PROTECTED_SYSTEMS). Each item
is separable (one owner, one file set). **[Wine]** = needs tru.exe running under Wine; **[RE]** = static Ghidra/objdump
work only; **[code]** = engine/test change. Items already owned by a running agent are listed in §16.0 and not
duplicated.

### 16.0 Already in progress

| Owner | Item | Files |
|---|---|---|
| FuncDetail | Per-function cc/params/globals/consts/confidence, FID flags, virtual-call edges | docs/re/db/functions.tsv, vcall_edges.tsv, tools/ghidra/scripts/ExportFunctionDetail.java, tools/re/function_db.py |
| StructDb | Classes, structures, globals, module deps, string refs | docs/re/STRUCTURES.md, docs/re/db/{classes,class_hierarchy,structures,globals,module_deps,string_refs,this_offsets,xrefs_summary}.tsv, tools/re/struct_db.py |
| ScriptNatives | Script native binding table under Wine (op 0x65, missing natives) **[Wine]** | tools/re/script_types_dump.cpp, SCRIPTS.md section (owner decides) |

### 16.1 Priority 1 — cheap, unblocking, no engine behaviour change

| ID | Goal | Files likely touched | Evidence needed | Verification |
|---|---|---|---|---|
| W8-1 | Add `"platform": "Linux"` to the self-test report (brief schema) | src/app/game/SelfTest.cpp (report builder ~l.967), docs/SELFTEST.md "Report", tests/test_selftest.cpp | brief §8 example | `SelfTest.*` unit test asserting the key; one `game_selftest_locomotion` run |
| W8-2 | AO regression test: CORTAO and RT AO compile + render a fixed scene, compare against AO-off (non-zero, bounded difference) and GL-vs-VK with AO on | tests/test_vk_renderer.cpp or a new `viewer_*_ao` ctest in CMakeLists.txt | BASELINE §6 diffs (CORTAO vs off 0.403 /255, 41.5 dB) as initial bounds; static scene l3_main_bedroom (motion pollutes cliffclimb) | ctest green; mutated copy (AO forced off) fails |
| W11-3 | Doc drift fixes OU-DOC-001/002 + Havok comment | docs/RENDERING.md:1110, docs/ROADMAP.md "Materials", matrix rows, src/physics/Physics.cpp:1498 (comment only) | PHASE01 §4 | review; no behaviour change |
| W2-1 | Format doc for UI art: font DTP 46 (glyph table), generalbank.drm atlas, g_pda.drm icons 680-706, cdc RAW images | new docs/formats/UI.md (from src/ui/Art.*, docs/GAMEPLAY.md "Menus") **[RE]** to confirm 0x4758C0 / 0x475350 / RAW loader | existing code + tru.exe reads | parse test on real data exists or added (`ui::parseFont` over generalbank.drm) |
| W2-2 | Normalise the 10 per-format items: add an explicit endianness line and a "Validation" subsection (what the loader rejects, which test enforces it) to each docs/formats file | docs/formats/*.md (one commit per doc) | loader code in src/cdc, src/scene, src/audio | each Validation row names an existing test or opens a test item |
| W2-3 | Consolidate lighting data (unit lights, irradiance volumes, light maps incl. the no-TEXCOORD1 mean rule, PSSM inputs) | new docs/formats/LIGHTING.md, links from LEVELS/MESHES/MATERIALS | LEVELS, SHADERS, OriginalShading tests | links resolve; no duplicate layouts |
| W11-1 | Consolidated limitations/unresolved register (Phase 11 #25) | new docs/LIMITATIONS.md indexing the open-question sections | existing docs | every entry links its source |
| W11-2 | Persistent engineering progress log for this brief (rule 25) | docs/audit/PROGRESS.md (dated entries per batch) | — | updated with each batch |

### 16.2 Priority 2 — RE and measurement that gate fixes

| ID | Goal | Files | Evidence needed | Verification |
|---|---|---|---|---|
| W3-1 **[Wine]** | Measure OU-WAT-006: Lara rest z with Enhanced vs Original waves at the same spot; tru.exe rest depth | BUGS.md entry (BugRepro/owner), work/waterentry-wine | Wine trace + `ou-game --self-test-trace` | numbers recorded; then W6-1 |
| W3-2 **[Wine]** | Grapple swing session (M20, M99): rope length, period, decay, growth, jump-off speed | REFERENCE_COMPARISON rows, work/gameplaygoal/reference/session7a.sh | template in REFERENCE_COMPARISON "Unmeasured" | rows move to matches/differs |
| W3-3 **[Wine]** | Movement leftovers: ground turn rates, stop distance, crouch/roll (M94, M15) | REFERENCE_COMPARISON | corridor session | rows updated; self-test bounds added |
| W3-4 **[Wine]** | Climb rates (M01, M02, M05, M96, M12), beam (M10), poles (M07, M09), perches (M11, M80) | REFERENCE_COMPARISON | sessions listed there | rows updated |
| W3-5 **[Wine]** | Swim curves, under-water turn/pitch, breath time (M29, M85) | REFERENCE_COMPARISON | l2_cliffclimb sea | rows updated |
| W3-6 **[Wine]** | Combat timings and enemy speeds (M28-M46, M81-M84, M111) | REFERENCE_COMPARISON | l4_pool panther, ammo/health words | rows updated |
| W3-7 **[Wine]** | Objects/puzzles: push speed, lever/door times, plate thresholds (M52-M62); grapple heave (M25, M26) — also unblocks known gap `ch12_trailstart_lever_gate` | REFERENCE_COMPARISON, BUGS C-JOINT | `pole watch <uid>` traces | rows updated |
| W3-8 **[Wine]** | Motorcycle (M47-M49) | REFERENCE_COMPARISON | l4_trailstart session | rows updated |
| W3-9 **[Wine]** | Camera transitions and per-mode behaviour (climb, combat, swim); OU-CAM-001 platform carry | REFERENCE_COMPARISON new rows, GAMEPLAY "Cameras" | hooks on 0x5E8210 (camfollow-wine) | rows + a camera self-test |
| W3-10 **[Wine]** | Runtime render capture of tru.exe (D3D9 call trace via apitrace or DXVK/vkd3d capture) to confirm RENDER_PIPELINE pass order, transparency sort, LOD switching | docs/re/RENDER_PIPELINE.md §11 answers | capture of l3_main_bedroom + l2_cliffclimb | pass list matches static trace or corrections recorded |
| W3-11 **[Wine]** | Audio event timing trace (FMOD Ex event/channel starts) for a scripted sequence | new section in docs/formats/AUDIO.md, REFERENCE_COMPARISON audio rows | FMOD statically linked: hook its play entry | ours vs original event times within tolerance |
| W3-12 **[Wine]** | Hazard damage per hazard (M65-M70, M77, M79, M103-M106); check EXTERNAL_RESEARCH speedrun leads | REFERENCE_COMPARISON | health word trace | rows updated |
| W1-1 **[RE]** | FID pass (MSVC 2005 CRT, FMOD Ex 4.16.9) and libjpeg location | docs/re/FUNCTION_DATABASE.md, db/* (after FuncDetail lands) | Ghidra FID databases | Unknown pool shrinks with direct evidence |
| W1-2 **[RE]** | Instruction-level verification of every citation used by a protected system's [O] claim (start: water, timing, physics damping, slide, fall damage) | FUNCTION_DATABASE "Verification sample" extended | objdump/Ghidra listings | each row asm-checked |
| W5-1 **[RE]** | Camera transition manager, cdc UI widgets / PDA, original save layout `.sav` (0x556E90), engine shutdown | ENGINE_ARCHITECTURE §1, §9, §21, §22; docs/SAVES.md | Ghidra | open questions closed or narrowed |
| W5-2 **[RE]** | Puzzle blockers in BUGS (C-JOINT; ch13 calendar handle, ch15 exit switch modifiable 7, ch13 statue head mover, ch15 rune block) | BUGS.md, SCRIPTS.md | script bytecode + natives (after ScriptNatives) | known-gap scenarios have a decoded cause |

### 16.3 Priority 3 — Phase 2 tooling and round-trip tests

| ID | Goal | Files | Evidence needed | Verification |
|---|---|---|---|---|
| W2-4 | Real-data round-trip: DRM v19 parse → serialise (sections + relocations) byte-identical for all 1056 DRMs; CDRM recompress-identical where the zlib level is known, else decompressed-identical | src/cdc (serialiser, new), tests/test_cdc.cpp `RealData` test | CONTAINERS §4.5 relocation layout | ctest over the install (opt-in survey if slow) |
| W2-5 | Real-data round-trip: locals.bin and bigfile index (rebuild index from records → identical) | src/cdc, tests/test_locale.cpp / test_importer.cpp | TEXT §1, CONTAINERS §1 | identical bytes |
| W2-6 | PCD9 header/payload re-encode identity (dump → re-wrap) | tools/ou-asset, tests/test_formats.cpp | TEXTURES header | identical bytes for all textures |
| W2-7 | Inspection tools for collision (mesh/BIH/markup dump), animation/skeleton (clip list, track dump), materials, script bytecode listing, cinematic channels, UI font | tools/ou-asset (new subcommands) | existing decoders | each subcommand runs on a real DRM; documented in the format doc |

### 16.4 Priority 4 — engine fixes with evidence in hand (Phase 6/7)

Each item: answer the 7 questions of policy §3 in the commit, add/extend the regression test, re-run the protected
tests of the touched systems (PROTECTED_SYSTEMS).

| ID | Goal | Files | Evidence | Verification |
|---|---|---|---|---|
| W6-1 | OU-WAT-006 swell clamp (after W3-1) | src/render/shaders/water_surface.glsl:87-91, src/game/World.cpp:1716 | W3-1 numbers | `ch07_cliffclimb_boat_swell`, water ctests, `viewer_vulkan_matches_gl_enhanced`, Original-water byte identity |
| W6-2 | OU-WAT-007 dry-deck rule limited to the intended bodies | src/game/Player.cpp:3100 | BUGS entry | `StreamReal.YachtDecksAboveSea`, new test with another moving solid |
| W6-3 | OU-CAM-001 camera carried by moving platforms | src/game/World.cpp:959, Camera.cpp | W3-9 | camera probe in `moving_markup` |
| W6-4 | OU-WAT-002 spray over the dive centre; OU-PLY-003 climb out onto a shelf; OU-GRP-001 grapple throw clip (after W3-2 for the clip) | water/fx, Player.cpp, Grapple | BUGS entries | named self-tests |
| W6-5 | Script VM: op 0x65 and missing natives (after ScriptNatives) | src/script/Vm.*, Script.cpp | ScriptNatives output | fire-volume test on l3_main_bedroom (no `handler stopped: op 0x65`), `ScriptRealData.*` survey count drops |
| W6-6 | REFERENCE_COMPARISON "Differences to fix" #3, #5, #6, #7, #10 | Player.cpp, WallJump.cpp | traces cited there | rows become matches; self-test bounds tightened |
| W6-7 | Vulkan AO/RT per-object motion vectors (camera-only reprojection) | src/render/vk/* AO/RT temporal passes | RENDERING.md:152 vs 281; GL prevVbo approach | yacht scene image stability; W8-2 test; GL/VK parity |
| W6-8 | Known-gap scenarios as their causes are decoded (W5-2) | per gap | per gap | scenario moves out of `known_gaps/` |

### 16.5 Priority 5 — test, comparison, performance and delivery infrastructure

| ID | Goal | Files | Evidence | Verification |
|---|---|---|---|---|
| W8-3 | Self-test probes for HUD state, inventory contents (health packs carried), audio (mixer: playing events/music state), camera mode name | src/app/game/SelfTest.cpp, docs/SELFTEST.md "Probes" | — | scenarios using them; mutation check |
| W8-4 | Self-test "engine" steps matching the brief example: SDL3 subsystems, renderer init, Jolt world, water path, AO path, RT availability — run without `--draw-last` in one opt-in scenario | SelfTest.cpp probes `engine.*`, tests/selftests/engine.json | — | report shows each with real state |
| W8-5 | Stored golden images for a few fixed scenes (Original and Enhanced presets, AO on/off, water modes), compared with `ou-imgdiff` bounds | tests/, CMakeLists.txt | BASELINE diffs | ctest |
| W8-6 | Self-test save/load steps (honour `save`/`load` input lines with a redirected XDG_DATA_HOME) | SelfTest.cpp | memory: second-scene-load smoke recipe | scenario that saves, reloads, compares `sim` |
| W4-1 | Per-chapter checkpoint list (uid, unit, position) and objective list, each with a test | docs/matrix/*.md, docs/PROGRESSION.md | `--self-test-trace` prints checkpoints | `StreamReal`/self-test per checkpoint |
| W4-2 | Chapter matrix refresh to current code (status drift) | docs/underworld_chapter_gameplay_matrix.md, docs/matrix/*.md | ctest/self-test names | no row contradicts a passing test |
| W9-1 **done** | Per-level comparison matrix (21 chapters × measured subsystems) built from REFERENCE_COMPARISON + matrices | [docs/REFERENCE_LEVELS.md](../REFERENCE_LEVELS.md), generated by `tools/re/ref_levels.py` (`--check` detects a stale file) | W3-* results | every level has rows with status |
| W9-2 | Version the Wine harness sources (ref2.c, session scripts, tr.py/swing.py, start-kwin.sh) in the repo | tools/wine-ref/ | work/gameplaygoal/reference, work/poleattach | a session reproduces one existing trace |
| W9-3 **[Wine]** | Rendering capture classification: for a fixed scene set, label each visible difference as defect / API difference / intentional enhancement | docs/REFERENCE_COMPARISON.md new "Rendering" section | W3-10 captures | table complete |
| W10-1 | Performance baseline extension: RAM (RSS), VRAM (VK_EXT_memory_budget), CPU %, GPU busy %, level-load time, cold/warm pipeline compile time, physics ms column, frame-pacing histogram | docs/audit/BASELINE_*.md (new dated file), a benchmark script under tools/ | — | reproducible numbers on two runs |
| W10-2 | Fixed benchmark scenarios driven by `--input-script` (deterministic tick count) for 4-6 scenes incl. water and AO modes | tools/bench/, docs | BASELINE §6 note | identical `sim` digests between runs |
| W10-3 | Periodic sanitizer + validation run (ASan/UBSan + `OU_VK_VALIDATION=1`) recorded per release | docs/BUILDING.md, RELEASING.md | existing recipe | zero reports |
| W10-4 | ctest label `protected` covering one test per protected system (PROTECTED_SYSTEMS verification column) for after-every-change runs | CMakeLists.txt | PROTECTED_SYSTEMS | `ctest -L protected` runs in minutes |
| W10-5 | Bisect OU-VK-001 (`game_vulkan_rt_smoke` GPU hang) between 671e35e and v0.41.0 in a GPU-reset-safe session | src/render/vk/* (fix), CMakeLists.txt (re-enable) | BUGS OU-VK-001 | test re-enabled and green |
