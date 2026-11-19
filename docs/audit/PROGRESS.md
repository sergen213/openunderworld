# Engineering progress log (master brief)

A dated entry per batch (execution rule 25). Requirement status lives in [BRIEF_COMPLIANCE.md](BRIEF_COMPLIANCE.md);
work item IDs (W*-*) come from its §16.

## 2026-11-16 — batch 1: Phase 0 + Phase 1 baseline

- **Snapshot:** branch `baseline/pre-audit-2026-11-16` at v0.51.0 (9034839), pushed. The tree was clean.
- **Delivered:**
  - [PHASE01_REPORT.md](PHASE01_REPORT.md), [PROJECT_AUDIT_2026-11.md](PROJECT_AUDIT_2026-11.md),
    [PROTECTED_SYSTEMS.md](PROTECTED_SYSTEMS.md), [BASELINE_2026-11.md](BASELINE_2026-11.md),
    [BUGS.md](BUGS.md), [EXTERNAL_RESEARCH_2026-11.md](EXTERNAL_RESEARCH_2026-11.md).
  - [../re/BINARIES.md](../re/BINARIES.md), [../re/FUNCTION_DATABASE.md](../re/FUNCTION_DATABASE.md),
    [../re/ENGINE_ARCHITECTURE.md](../re/ENGINE_ARCHITECTURE.md).
- **Baseline:** ctest 980 passed, 0 failed, 17 skipped (997 selected; `game_vulkan_rt_smoke` excluded).

## 2026-11-16 — batch 2: Phase 0/1 gaps

- **Delivered:**
  - **Per-function detail:** for all 18,766 functions: calling convention, parameters, frame size, globals,
    constants, this-offsets, confidence. Plus FID, 644 resolved virtual-call sites, xref, string-ref and
    module-dependency reports, and 10 more assembly checks (21 in total, all agree).
    ([FUNCTION_DATABASE.md](../re/FUNCTION_DATABASE.md))
  - **Data-structure inventory:** 1,015 RTTI types with their hierarchy, sizeof for 115 classes,
    1,159 structure fields (20 of them checked against the assembly: 18 match, 1 partial, 1 mismatch),
    and 11,650 globals. ([STRUCTURES.md](../re/STRUCTURES.md))
  - **Original frame pipeline:** with the top-level call order checked against the assembly.
    ([RENDER_PIPELINE.md](../re/RENDER_PIPELINE.md))
  - **Bug reproduction pass:** [BUGS.md](BUGS.md), 89 entries.
  - **Requirement-by-requirement compliance matrix:** with the Phase 2–11 queue.
    ([BRIEF_COMPLIANCE.md](BRIEF_COMPLIANCE.md))
- **Running:** ScriptNatives (script native binding table under Wine).

## 2026-11-16 — batch 3: Phase 2–11 queue, wave 1 (running)

| Agent | Items |
|---|---|
| SelfTestBrief | W8-1 platform key, W8-4 engine scenario, W8-3 HUD/inventory/audio/camera probes, W8-6 save/load steps |
| AoGolden | W8-2 AO regression tests, W8-5 golden images, W10-4 `protected` ctest label |
| FormatDocs | W2-1 UI.md, W2-2 endianness + validation in every format doc, W2-3 LIGHTING.md |
| DocDrift | W11-3 doc drift, W4-2 matrix refresh, W11-1 LIMITATIONS.md |
| RoundTrip | W2-4..W2-7 real-data round trips and ou-asset inspection subcommands |
| WineMeasure | W9-2 versioned Wine harness, W3-1 rest depth, W3-9 camera, W3-5 swimming |
| Bench | W10-2 deterministic benchmarks, W10-1 extended performance baseline |

Engine fixes (W6-*) wait for this wave's evidence (W3-*, ScriptNatives).

## 2026-11-17 — batch 4: RE wave 2 + first evidence-backed fixes (paused by the user)

- **Landed on main:**
  - Reverse-engineering docs: SCRIPT_NATIVES, RENDER_PIPELINE, STRUCTURES, VERIFICATION, CAMERA, UI, SAVEGAME,
    SHUTDOWN, PUZZLE_BLOCKERS.
  - Format docs: UI and LIGHTING docs; endianness and validation sections in all format docs.
  - LIMITATIONS.md; DocDrift corrections and refreshed chapter matrices.
  - Tests: self-test probes (engine, HUD, inventory, audio, camera, save/load), AO regression and golden tests,
    the `ctest -L protected` label.
  - Tools: real-data round trips and ou-asset subcommands; the Wine harness in tools/wine-ref; benchmark tooling in
    tools/bench.
  - The script VM fix: CALLU builtins and natives (8daa2ee, f9676fd).
- **Integration branch `int/brief`** (worktree `openunderworld-work/wt-int`). It builds cleanly. **The full ctest is
  not run yet, so it is not merged into main or released.** It contains:

  | Branch | Fixes |
  |---|---|
  | agent/SwellFix-2 | OU-WAT-006: sea within ~5 of tru.exe |
  | agent/PlatformCam-2 | OU-CAM-001: lag 0; ~115-unit height residual → OU-CAM-005 |
  | agent/SwimTurn-2 | OU-SWM-002 fixed; OU-SWM-001 partly fixed |
  | agent/PuzzleFix | OU-PZL-006 / 007 / 010 |
  | agent/SlideLedge | OU-PLY-014 fixed; OU-PLY-013 not a bug. Its own full ctest was interrupted. |

- **Unfinished:** agent/ScriptPuzzle (worktree `wt-ScriptPuzzle`). It has uncommitted edits for OU-PZL-009, kept
  on disk, which are not built yet. Still to do: multibody 3, OU-GRP-005 (vt 46 / larabase 26), multibody 92/93.
- **Next:**
  1. Full ctest on int/brief in a private kwin, excluding game_vulkan_rt_smoke.
  2. Re-bless the cliff_water_enhanced goldens on the integrated build. Swell and camera carry both change that
     frame; the merge kept SwellFix's PNG.
  3. Merge into main, bump to 0.52.0, write the CHANGELOG, release.
  4. Finish ScriptPuzzle.
  5. Run the idle-machine benchmark (`tools/bench/run.sh --runs 2`).

## 2026-11-18/19 — batch 5: integration of the fix branches

- **main moved:** it gained 0.52.0 (time of day, from another session). It already contained the script VM fix and
  the batch-3 tests, which were not listed in its CHANGELOG. int/brief merged main at 6a01070.
- **First full ctest on int/brief:** 1054 of 1060 passed, 6 failed. Each was root-caused and fixed on its own branch:

  | Branch | Failure | Cause and fix |
  |---|---|---|
  | agent/IntSave 7699d51 | `StreamReal.SaveContinuesBitIdentically` | SwimTurn-2's swim speed and last seen camera yaw were not saved. Now saved (`player.controller.swimSpeed`, optional `camera.seen`). |
  | agent/IntSwim 7792f25 | `game_selftest_ch19_arctic_sea_stone_niche` | The self-test autopilot assumed instant swim turns. It now eases the stick in water. The game model (measured tru.exe turn) is unchanged. |
  | agent/IntGolden 4986979 | baldrs replay and 3 cliff goldens | PlatformCam-2 had recorded and blessed them with a stale binary linked before the camera change. Re-recorded from the plan and re-blessed after an image review; only the framing changed (the camera rides the deck). |

- **agent/ScriptPuzzle** (ScriptPuzzle2) was merged: OU-PZL-009, OU-GRP-005, OU-PZL-015 (partly) and OU-PZL-008(c).
- **Lesson:** an agent's "re-blessed" golden or re-recorded replay can come from a stale build. Check the binary's
  link time against the source edit, or run the integration full ctest before trusting it.
- **Released:** int/brief was merged into main and released as 0.53.0 (74f58af). The full ctest had 1066 tests: 1064
  passed, and 2 test fixes were re-verified (game_vulkan_rt_smoke excluded).

## 2026-11-19 — batch 6: evidence-backed fixes for open BUGS entries (running)

Each agent works in its own worktree (`openunderworld-work/wt-<Name>`, branch `agent/<Name>`). The parent merges
them into an integration branch, runs the full ctest in a private kwin, and releases.

| Agent | Items |
|---|---|
| ScriptVm6 | OU-SCR-001 (op 0x65 in fire volumes), OU-SCR-004 (sound wait without audio), OU-SCR-003 / 002 (unbound natives) |
| DeckCam6 | OU-CAM-005 (deck camera distance 1264.2 vs 948.2), OU-PLY-015 (Land state on the dropping deck) |
| SharkAi6 | OU-CMB-003 (l2_cliffclimb shark aggression vs tru.exe) |
| Depen6 | OU-PLY-012 (no depenetration), OU-REN-009 (false `texture 2` log) |
