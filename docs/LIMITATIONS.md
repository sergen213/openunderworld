# Limitations and unresolved questions (register)

One index of everything OpenUnderworld does not know or does not do yet. It does not repeat the details: every entry
names the section that owns them, and that section stays the place to update. Counts are the items listed in the
source section when this register was written (2026-11-16, main after the W4-2 matrix refresh); the source wins when
they disagree. Labels as everywhere in the docs: [O] observed, [I] inferred, [U] unknown.

How to use it: before starting work in an area, read its rows here and follow the links; when a question is settled,
change the source section (and the bug entry, where there is one), not this file. Add a row here only for a new
open-question section.

## Defects and deviations

| Source | What is open | Items |
|---|---|---|
| [audit/BUGS.md](audit/BUGS.md#entries) | the bug and regression database: defects, deviations from tru.exe, regressions, each with repro, expected (tru.exe), cause, fix and status; grouped by area (water, rendering, Vulkan / FSR / streaming, UI, audio, camera, controller, swimming, grapple, progression, script VM, combat / AI / vehicles / physics, tests and docs, save / load) | 76 entries |
| [audit/BUGS.md "Root-cause clusters"](audit/BUGS.md#root-cause-clusters) | shared causes behind several bugs (C-SWELL, C-UWPORT, C-SPLASH / C-WATERTUNE, C-VM, C-JOINT, C-CHART, C-MEASURE, C-DOC) | 8 clusters |
| [REFERENCE_COMPARISON.md "Differences to fix"](REFERENCE_COMPARISON.md#differences-to-fix) | measured differences against tru.exe beyond the tolerance, with the owning code (several marked fixed) | 10 rows |
| [REFERENCE_COMPARISON.md "Unmeasured"](REFERENCE_COMPARISON.md#unmeasured-what-still-lacks-a-live-measurement-and-the-session-that-would-give-it) | gameplay quantities with no live Wine measurement yet (grapple swing, climbing rates, ground turn rates, ...) and the session that would give each; [audit/PHASE01_REPORT.md](audit/PHASE01_REPORT.md#9-what-is-still-unknown) counts 107 of 161 comparison rows unmeasured | 21 sessions |
| [GAMEPLAY.md "Deviations and open questions"](GAMEPLAY.md#deviations-and-open-questions) | where ours knowingly differs from tru.exe or is not measured (jumps, swimming, grapple, combat, saves) | 10 bullets |
| [ROADMAP.md](ROADMAP.md#compatibility-matrix) "Known deviations" column | per-feature limitations of every row of the compatibility matrix | one per row |

## Gameplay coverage

| Source | What is open | Items |
|---|---|---|
| [underworld_chapter_gameplay_matrix.md "Status summary"](underworld_chapter_gameplay_matrix.md#status-summary) | campaign requirements by status: 7 NOT IMPLEMENTED, 99 PARTIALLY IMPLEMENTED, 398 IMPLEMENTED BUT UNVERIFIED (code exists, not tested in that chapter's units) of 661 | 661 rows |
| [underworld_chapter_gameplay_matrix.md "Shared mechanics"](underworld_chapter_gameplay_matrix.md#shared-mechanics) | mechanics not yet complete: 4 NOT IMPLEMENTED (M19 walk-over, M27 rope snag topple, M50 bike headlight / urn ram, M106 ship roll), 20 PARTIALLY IMPLEMENTED | 111 mechanics |
| `docs/matrix/NN_*.md` "Gaps" (e.g. [17_valhalla.md](matrix/17_valhalla.md#gaps)) | per chapter: what blocks or is unplayed on that chapter's route | 21 lists |
| [SELFTEST.md known gaps](SELFTEST.md) and `tests/selftests/known_gaps/` | scenarios that fail today on purpose, each with its cause (lever gate by rope weight, temple block topple, calendar solve, pool rune block, hammer chamber pull-up, Natla platform ride, bedroom door save, ...) | 15 files |
| [GAMEPLAY_DEVELOPMENT_REPORT.md §8 "Remaining work"](GAMEPLAY_DEVELOPMENT_REPORT.md#8-remaining-work) | ordered remaining gameplay work (blockers, Natla's fight by input, unwritten chapter legs, push blocks and Thor's mechanisms, ...) | 8 items |
| [PROGRESSION.md "Remaining blockers"](PROGRESSION.md#remaining-blockers) | gate-level blockers and VM findings from the gate sweeps | 9 bullets |

## Reverse engineering of tru.exe

| Source | What is open | Items |
|---|---|---|
| [audit/PHASE01_REPORT.md §9](audit/PHASE01_REPORT.md#9-what-is-still-unknown) | the audit's summary of unknowns: runtime-built native tables, unresolved indirect calls, library tags, unmeasured gameplay values | 5 bullets |
| [re/ENGINE_ARCHITECTURE.md "Open questions across subsystems"](re/ENGINE_ARCHITECTURE.md#open-questions-across-subsystems) | uncited functions without neighbours, unresolved virtual / indirect calls, large uncited functions next to known subsystems | 3 bullets |
| [re/FUNCTION_DATABASE.md "Open questions"](re/FUNCTION_DATABASE.md#open-questions) | library-tag confidence, libjpeg location, unresolved vptr call sites, caller-less functions | 6 bullets |
| [re/FUNCTIONS.md "Open hypotheses"](re/FUNCTIONS.md#open-hypotheses-not-in-the-csv-yet) | function roles proposed but not confirmed (per-frame services in `MainG2`, shutdown, ...) | 22 rows |
| [re/STRUCTURES.md "Mismatches and corrections found"](re/STRUCTURES.md#mismatches-and-corrections-found) | places where repo code or names disagree with the disassembly (some corrected since; `src/game/World.cpp` still names CameraManager +0x54 a mode field) | 14 bullets |
| [re/STRUCTURES.md "Unknowns"](re/STRUCTURES.md#unknowns) | type sizes, allocators, globals, script-VM / save layouts of the original | 6 bullets |
| [re/RENDER_PIPELINE.md §11](re/RENDER_PIPELINE.md#11-open-questions) | render passes and targets not fully traced (alpha-only pass, ripple map, refraction view, under-water lists, ...) | 12 items |
| [re/SCRIPT_NATIVES.md "Unknowns"](re/SCRIPT_NATIVES.md#unknowns) | AI-manager record, render constants, unlinked native types, heuristic implementation status | 4 bullets |
| [re/TIMING.md](re/TIMING.md) | per-frame order and clocks: entries marked [I] / [U] in place | inline |
| [RESEARCH.md "Open questions"](RESEARCH.md#open-questions) | early research questions on the exe, containers and formats (several answered later in docs/formats) | 14 items |

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
| [EXISTING_PROJECT_AUDIT.md "Open questions"](EXISTING_PROJECT_AUDIT.md#open-questions) | licensing of copied files, canonical trees of the sister projects | 3 bullets |
| [audit/PROTECTED_SYSTEMS.md](audit/PROTECTED_SYSTEMS.md) | not limitations: the systems that must not regress, with their tests (read before changing any of them) | 28 systems |
