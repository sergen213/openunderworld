# Limitations and unresolved questions (register)

One index of everything OpenUnderworld does not know or does not do yet. It does not repeat the details: every entry
names the section that owns them, and that section stays the place to update. Counts are the items listed in the
source section when this register was last refreshed (2026-11-20, main at v0.58.0; first written 2026-11-16); the
source wins when they disagree. Labels as everywhere in the docs: [O] observed, [I] inferred, [U] unknown.

How to use it: before starting work in an area, read its rows here and follow the links; when a question is settled,
change the source section (and the bug entry, where there is one), not this file. Add a row here only for a new
open-question section.

## Defects and deviations

| Source | What is open | Items |
|---|---|---|
| [audit/BUGS.md](audit/BUGS.md#entries) | the bug and regression database: defects, deviations from tru.exe, regressions, each with repro, expected (tru.exe), cause, fix and status; grouped by area (water, rendering, Vulkan / FSR / streaming, UI, audio, camera, controller, swimming, grapple, progression, script VM, combat / AI / vehicles / physics, tests and docs, save / load). Of the 98 index rows, 45 are Open, 4 Reproduced, 3 Partly fixed and 7 Not reproduced | 98 entries |
| [audit/BUGS.md "Root-cause clusters"](audit/BUGS.md#root-cause-clusters) | shared causes behind several bugs (C-SWELL, C-UWPORT, C-SPLASH / C-WATERTUNE, C-VM, C-JOINT, C-CHART, C-MEASURE, C-DOC) | 8 clusters |
| [REFERENCE_COMPARISON.md "Differences to fix"](REFERENCE_COMPARISON.md#differences-to-fix) | measured differences against tru.exe beyond the tolerance, with the owning code (most marked fixed; #3 and the open part of #10 remain) | 15 rows |
| [REFERENCE_COMPARISON.md "Unmeasured"](REFERENCE_COMPARISON.md#unmeasured-what-still-lacks-a-live-measurement-and-the-session-that-would-give-it) | gameplay quantities with no live Wine measurement yet (grapple swing, climbing rates, ground turn rates, ...) and the session that would give each; [REFERENCE_COMPARISON.md "Counts"](REFERENCE_COMPARISON.md#counts): 107 of 169 comparison rows unmeasured | 21 sessions |
| [GAMEPLAY.md "Deviations and open questions"](GAMEPLAY.md#deviations-and-open-questions) | where ours knowingly differs from tru.exe or is not measured (jumps, swimming, grapple, combat, saves) | 10 bullets |
| [ROADMAP.md](ROADMAP.md#compatibility-matrix) "Known deviations" column | per-feature limitations of every row of the compatibility matrix | one per row |

## Gameplay coverage

| Source | What is open | Items |
|---|---|---|
| [underworld_chapter_gameplay_matrix.md "Status summary"](underworld_chapter_gameplay_matrix.md#status-summary) | campaign requirements by status: 7 NOT IMPLEMENTED, 99 PARTIALLY IMPLEMENTED, 398 IMPLEMENTED BUT UNVERIFIED (code exists, not tested in that chapter's units) of 661 | 661 rows |
| [underworld_chapter_gameplay_matrix.md "Shared mechanics"](underworld_chapter_gameplay_matrix.md#shared-mechanics) | mechanics not yet complete: 4 NOT IMPLEMENTED (M19 walk-over, M27 rope snag topple, M50 bike headlight / urn ram, M106 ship roll), 20 PARTIALLY IMPLEMENTED | 111 mechanics |
| `docs/matrix/NN_*.md` "Gaps" (e.g. [17_valhalla.md](matrix/17_valhalla.md#gaps)) | per chapter: what blocks or is unplayed on that chapter's route | 21 lists |
| [SELFTEST.md known gaps](SELFTEST.md) and `tests/selftests/known_gaps/` | scenarios that fail today on purpose, each with its cause (temple block topple, hammer chamber pull-up, Natla platform ride, bedroom door save, ...) | 11 files |
| [GAMEPLAY_DEVELOPMENT_REPORT.md §8 "Remaining work"](GAMEPLAY_DEVELOPMENT_REPORT.md#8-remaining-work) | ordered remaining gameplay work (blockers, Natla's fight by input, unwritten chapter legs, push blocks and Thor's mechanisms, ...) | 8 items |
| [PROGRESSION.md "Remaining blockers"](PROGRESSION.md#remaining-blockers) | gate-level blockers and VM findings from the gate sweeps | 9 bullets |

## Reverse engineering of tru.exe

| Source | What is open | Items |
|---|---|---|
| [audit/PHASE01_REPORT.md §9](audit/PHASE01_REPORT.md#9-what-is-still-unknown) | the audit's summary of unknowns: runtime-built native tables, unresolved indirect calls, library tags, unmeasured gameplay values | 5 bullets |
| [re/ENGINE_ARCHITECTURE.md "Open questions across subsystems"](re/ENGINE_ARCHITECTURE.md#open-questions-across-subsystems) | uncited functions without neighbours, unresolved virtual / indirect calls, large uncited functions next to known subsystems | 3 bullets |
| [re/FUNCTION_DATABASE.md "Open questions"](re/FUNCTION_DATABASE.md#open-questions) | library-tag confidence, the empty libjpeg message table (libjpeg itself located 2026-11-20), unresolved vptr call sites, caller-less functions | 6 bullets |
| [re/FUNCTIONS.md "Open hypotheses"](re/FUNCTIONS.md#open-hypotheses-not-in-the-csv-yet) | function roles proposed but not confirmed (per-frame services in `MainG2`, shutdown, ...) | 22 rows |
| [re/STRUCTURES.md "Mismatches and corrections found"](re/STRUCTURES.md#mismatches-and-corrections-found) | places where repo code or names disagreed with the disassembly (most corrected since, e.g. CameraManager +0x54 in 4b58147) | 14 bullets |
| [re/STRUCTURES.md "Unknowns"](re/STRUCTURES.md#unknowns) | type sizes, allocators, globals, script-VM / save layouts of the original | 6 bullets |
| [re/RENDER_PIPELINE.md §11](re/RENDER_PIPELINE.md#11-open-questions) | render passes and targets not fully traced (alpha-only pass, ripple map, refraction view, under-water lists, ...) | 12 items |
| [re/SCRIPT_NATIVES.md "Unknowns"](re/SCRIPT_NATIVES.md#unknowns) | AI-manager record, render constants, unlinked native types, heuristic implementation status | 4 bullets |
| [re/CAMERA.md §8 "Unknowns"](re/CAMERA.md#8-unknowns) | camera manager details not decoded (see also BUGS OU-CAM-006: command 0x16, AnchoredCamera) | 4 bullets |
| [re/UI.md §7 "Unknowns"](re/UI.md#7-unknowns) | UI widget and screen details not decoded | 3 bullets |
| [re/SAVEGAME.md §7 "Unknowns"](re/SAVEGAME.md#7-unknowns) | save buffer / `.TRUSave` details not decoded | 4 bullets |
| [re/SHUTDOWN.md §5 "Unknowns"](re/SHUTDOWN.md#5-unknowns) | shutdown calls whose role is not decoded | 3 bullets |
| [re/VERIFICATION.md "Batch 10"](re/VERIFICATION.md#batch-10-2026-11-20-settled-rows-and-batch-6-9-citations) | every cited claim now match or mismatch; open: V020 (record-less body damping), V056 (MustDetach limits) and the branches the comments omit (BUGS OU-PLY-016..018, OU-PHY-003, OU-DOC-003) | 2 mismatches, 6 branches |
| [re/TIMING.md](re/TIMING.md) | per-frame order and clocks: entries marked [I] / [U] in place | inline |
| [RESEARCH.md "Open questions"](RESEARCH.md#open-questions) | early research questions on the exe, containers and formats (several answered later in docs/formats) | 14 items |

## Evidence tags in game and script code

`tools/re/tag_inventory.py` (2026-11-20, master brief P5-3) lists every function definition in `src/game` and
`src/script` whose comment block, body comments and header declaration carry no `[O]` / `[I]` / `[U]` tag:
[`re/db/untagged_paths.tsv`](re/db/untagged_paths.tsv) (path, line range, function, `address-only` when a comment
cites a tru.exe address without a tag, `trivial` for bodies of at most 3 lines). The parser is a heuristic (braces,
comments, literals); it does not judge whether a path needs original evidence, and save / restore, loaders and
test glue have no tru.exe counterpart. Re-run it after code changes; the counts below are from int/batch10.

| | definitions | tagged | address only | untagged | untagged, more than 3 lines |
|---|---:|---:|---:|---:|---:|
| src/game (26 files) | 803 | 221 | 79 | 503 | 372 |
| src/script (Script.cpp, Vm.cpp) | 134 | 23 | 16 | 95 | 74 |
| total | 937 | 244 | 95 | 598 | 446 |

Largest untagged files (non-trivial definitions): `script/Script.cpp` 50, `game/Objects.cpp` 42, `game/Player.cpp` 40,
`game/Combat.cpp` 38, `game/ai/Enemy.cpp` 37, `game/Stream.cpp` 29, `script/Vm.cpp` 24, `game/World.cpp` 22. The
longest untagged bodies: the water simulation step `WaterSim::tick` (155 lines), the save / restore paths
(`World::restoreRuntime`, `Runtime::restore` / `serialize`, `World::stateParts`, `World::restoreCombat`), the
lara.drm / tuning loaders (`loadLaraAnims`, `weaponDefaults`, `loadMeleeTuning`, `stateFromJson`) and traversal
steps (`Player::hurdleTick`, `Player::hangCorner`).

## File formats

| Source | What is open | Items |
|---|---|---|
| [formats/CONTAINERS.md §6](formats/CONTAINERS.md#6-open-container-questions) | DRM header words, relocation kinds, DTPDATA resource types, `Mus!` | 6 items |
| [formats/INVENTORY.md "Unknown formats"](formats/INVENTORY.md#unknown-formats-no-magic-based-classifier-yet) | install files without a classifier (locals.bin, ShaderUsage, padshock, ...) | 8 rows |
| [formats/LEVELS.md "Open questions"](formats/LEVELS.md#open-questions) | intro flags and data types, IMF fields, cell BSP / portals, Level+0x80 | 4 bullets |
| [formats/MESHES.md "Open questions"](formats/MESHES.md#open-questions) | primgroup texture indices, mesh header +0x74, terrain node tree | 5 bullets |
| [formats/MATERIALS.md "Open questions"](formats/MATERIALS.md#open-questions) | material flags, light-map source, PSSM and dynamic lights | 3 bullets |
| [formats/LIGHTING.md §5](formats/LIGHTING.md#5-open-questions) | light record fields, irradiance region floats, GlobalParams drivers, spot cone convention | 4 bullets |
| [formats/TEXTURES.md "Open questions"](formats/TEXTURES.md#open-questions) | flags bits 0 / 1, cube face order, low-alpha white textures | 3 bullets |
| [formats/ANIMATION.md "Open questions"](formats/ANIMATION.md#open-questions) | clip header floats, extra channels, scale order, root-motion extraction | 7 bullets |
| [formats/COLLISION.md "Open questions"](formats/COLLISION.md#open-questions) | face flags, Level+0x80, free-climb hold types, terrain group switches | 6 bullets |
| [formats/PHYSICS.md "Open questions"](formats/PHYSICS.md#open-questions) | where class-34 joints are created, HInfo markers, primitive type 13, moving terrain groups | 4 bullets |
| [formats/AUDIO.md §8](formats/AUDIO.md#8-open-questions) | FMOD IMA expansion, Mus! clock, Wave header fields, FSB sync points, ... | 16 items |
| [formats/UI.md §9](formats/UI.md#9-open-questions) | font header fields, RAW header, controller-glyph texture choice | 3 bullets |
| [formats/EFFECTS.md "Missing"](formats/EFFECTS.md#missing) | effect classes 4 / 8 / 9, model crossfades, ribbon subdivision, lightning jitter tables, segment-following particles, casing meshes, per-enemy hit materials, the line emitters' density rule | 1 paragraph |
| Other format docs (SHADERS, SCRIPTS, CINEMATICS, VIDEO, TEXT, ANIMGRAPH) | no separate open-question section; unknowns are marked [U] where they occur | inline |

## Engine, rendering, tooling

| Source | What is open | Items |
|---|---|---|
| [audit/BASELINE_2026-11.md §8](audit/BASELINE_2026-11.md#8-open-points-from-this-baseline) | open points of the baseline run (dark sky region on l2_cliffclimb, scene-loader warnings, missing texture 2, survey tests outside default ctest, lavapipe) | 5 bullets |
| [audit/BASELINE_PERF_2026-11.md §6](audit/BASELINE_PERF_2026-11.md#6-unknowns--caveats) | caveats of the performance baseline (load time, device-wide GPU counters, sim digest) | 3 bullets |
| [audit/PROJECT_AUDIT_2026-11.md §6](audit/PROJECT_AUDIT_2026-11.md#6-open-questions) | ROADMAP text still superseded, Vulkan camera-only AO / RT reprojection on moving bodies | 3 bullets |
| [UPDATES.md "Not done / open"](UPDATES.md#not-done--open) | updater: no per-channel persistent highwater against a local downgrade | 1 bullet |
| [audit/BRIEF_COMPLIANCE.md §16](audit/BRIEF_COMPLIANCE.md#16-remaining-work-queue-phases-0-11) | the master brief's remaining-work queue, ordered by phase ([Wine] / [RE] / [code] / [doc]) | one per item |
| [audit/PROTECTED_SYSTEMS.md](audit/PROTECTED_SYSTEMS.md) | not limitations: the systems that must not regress, with their tests (read before changing any of them) | 28 systems |
