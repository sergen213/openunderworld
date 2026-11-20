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

## 2026-11-19 — batch 6: evidence-backed fixes for open BUGS entries (released as 0.54.0)

Each agent works in its own worktree (`openunderworld-work/wt-<Name>`, branch `agent/<Name>`). The parent merges
them into an integration branch, runs the full ctest in a private kwin, and releases.

| Agent | Items |
|---|---|
| ScriptVm6 | OU-SCR-001 (op 0x65 in fire volumes), OU-SCR-004 (sound wait without audio), OU-SCR-003 / 002 (unbound natives) |
| DeckCam6 | OU-CAM-005 (deck camera distance 1264.2 vs 948.2), OU-PLY-015 (Land state on the dropping deck) |
| SharkAi6 | OU-CMB-003 (l2_cliffclimb shark aggression vs tru.exe) |
| Depen6 | OU-PLY-012 (no depenetration), OU-REN-009 (false `texture 2` log) |
| (parent) RefLevels | W9-1: [REFERENCE_LEVELS.md](../REFERENCE_LEVELS.md), generated by `tools/re/ref_levels.py` |

Merged into `int/batch6` (worktree `openunderworld-work/wt-int6`):

- **agent/RefLevels:** the per-level comparison matrix. It covers 21 chapters × 8 subsystems: our implementation
  status, the Wine-measured REFERENCE_COMPARISON rows that name the chapter's units, and open bugs. It is generated
  from the existing docs, and `--check` detects a stale file.
- **agent/ScriptVm6:**
  - OU-SCR-001 was fixed-verified (already fixed on main by f9676fd).
  - OU-SCR-004 is fixed: a `sound` 1/2 wait always suspends at least one tick and resumes when the sound stops
    (tru.exe 0x4AF230 → 0x57E016, 0x57E970 / 0x4ACC30). Measured under Wine: one burn per entry into fire
    volume 4494, because the burn sound sustains.
  - GameSound now also runs under `--no-audio`, with a device-less mixer, so script timing does not depend on
    audio.
  - A use-after-free of streamed-out marker sounds that this exposed (helheim02 self-test) is fixed.
  - OU-SCR-003 is partly fixed (`instance` 63 is open); OU-SCR-002 is narrowed (the anchor / vector / rotation chain
    is decoded; vector values in the VM are still missing).
  - The script ctests on the integration build: 61 passed, 2 skipped (surveys).
- **agent/SharkAi6:** OU-CMB-003 is fixed. In tru.exe a treading Lara is outside the sharks' reach: the 395.1
  vertical limit from enemy data +0x88, and shark origins stay ≥ 892.2 under the surface. Measured under Wine: 0
  bites in 600 s of treading. Below that depth each shark makes a pass about every 14 s and 6 bites came in 232 s.
  Ours now applies the reach rule, a circling mode and the data bite damage (200). The pass bite chance and the
  circling time are fitted, not decoded. Open: shark 4023 exists only in ours.
- **agent/DeckCam6:**
  - OU-CAM-005 is fixed: camera trigger volumes now apply their distance commands (2 / 0xD) and inline force sets
    (0x17 / 0x18) as tru.exe does (0x5CD520 → 0x5F29A0 → 0x601D30 / 0x6019D0). The deck now gives 948.2 as tru.exe,
    and volume 3857 gives 790.1 with the camera 231.0 above the pivot, as tru.exe. The +5° pitch set accounted for
    the old ~115-unit OU-CAM-001 height residual.
  - OU-PLY-015 is fixed: colliders are posed after a mid-game rebuild.
  - The cliff goldens were re-blessed and the old and new frames compared.
  - The volume rule changes the camera at all 91 volumes that carry camera commands.
- **agent/Depen6:**
  - OU-REN-009 is fixed.
  - OU-PLY-012 is partly fixed: Lara is pushed out of terrain walls she is placed into. tru.exe does this every
    frame (measured under Wine); ours does it only on placement, because several approximated moves of ours end
    inside our own collision.
  - The first full ctest found 5 ProgressionReal failures. All five tests stood her inside a wall at a switch or
    receptacle origin; they now use `placeToUse` (follow-up 766a5d6).
- **Full ctest on int/batch6** (private kwin, game_vulkan_rt_smoke excluded): 100 % of 1074 passed (18 surveys
  skipped as usual).
- **Lesson:** an agent's narrow ctest set (`-R` by subsystem) misses tests that place Lara by object origin. Any
  change to placement or collision needs `ProgressionReal` in its set.
- **Released:** main 5751cf9, tag v0.54.0 (`tools/release.sh`, signed AppImage verified).

## 2026-11-19 — batch 7: follow-ups from batch 6 (released as 0.55.0 without TickDepen7)

| Agent | Items |
|---|---|
| TickDepen7 | OU-PLY-012 remainder: depenetration every tick, as tru.exe does; first fix the approximated moves that end inside our collision |
| CamZones7 | Unmodelled camera trigger commands (0x19-0x1C, 0x12/0x14/0x15/4/6/8), trigger kinds 1/2, force sets with forces, volumes riding moving objects; OU-CAM-004 if on the same path |
| ScriptVec7 | OU-SCR-002: vector values in the VM, anchor/vector/larabase natives; OU-SCR-003 `instance` 63 |
| EnemySpawn7 | The enemy presence rule at unit load (shark 4023 exists only in ours); shark pass and circle rules in place of the fitted constants |

Merged into `int/batch7`:

- **agent/EnemySpawn7:**
  - Dormant intros (`+0x72 & 0x80`, skipped by tru.exe's load loop 0x5C2360) now wait for Birth in the bare
    `ai::Enemies` harness too. The game was already right; batch 6's 1.47 bites/min was measured with a third shark.
  - Shark CloseCombat time slots come from 0x627F00 / 0x63A4C0 / 0x6108A0 (2-4 s, Circle vs Sidestep 50/50; sharks
    have no sidestep, so the other slots are passes [I]). Bite range is 370 / 405 from the Wine traces.
  - 0.98 bites/min per shark vs tru.exe 0.78. The share of passes that bite is still calibrated.
  - The ch19 stone-niche step 1 check changed from "swimming the whole time" to `water.in` (a greenland shark now
    reaches her).
- **agent/ScriptVec7:**
  - OU-SCR-002 is fixed: VM vectors / rotations are 16-byte inline values; natives write multi-word results;
    vector 0-12, rotation 0-15, anchor 0/1/3/4/5 and larabase 21/22 are bound; locals start at their declared
    values (0x57A850 → 0x579770).
  - Script survey stops: 132 → 89.
  - Fire volumes pick Lara's side as tru.exe does. The flinch clip is not played yet (chart states for messages
    0x1DA / 0x1DB are undecoded).
  - `instance` 63 is still unbound.
- **agent/CamZones7:** OU-CAM-006 is partly fixed.
  - The survey found 459 command-carrying volumes, and all commands are decoded from 0x6019D0.
  - Implemented: 0x12, 0x15, 0x17 / 0x18 with forces, 0x19-0x1C pivot and look offsets, and volumes riding their
    objects. Wine-verified at volumes 3852 / 3859 / 3861.
  - The cliff goldens were re-blessed (the deck view is level). The ch06 shaft jump input was rotated for the new
    climbing camera.
  - Open: commands 1, 3-8, 0xC, 0x14, 0x16, and the AnchoredCamera kind.
- **agent/TickDepen7: not merged.**
  - The slide fix (`kSweepSkipLeaving`) and per-tick ground push-out work on its branch.
  - The one-sided ground sweep rests only on tru.exe's ray test 0x461640, not on the player proxy's wall contacts.
    Its own survey found 37,712 single-sided walls Lara could pass from behind.
  - 3 tests fail on the combined tree (ch03 gear room west / east, save case 9).
  - Carried over to batch 8: proxy sidedness evidence, a slide-only split, the ch03 switch stand point, and save
    case 9.
- **Full ctest on int/batch7** (private kwin, game_vulkan_rt_smoke excluded): 100 % of 1078 passed.

## 2026-11-19 — batch 8 (released as 0.56.0)

Merged into `int/batch8` (worktree `openunderworld-work/wt-int8`):

- **agent/TickDepen7** (carried over): OU-PLY-012 is fixed.
  - Wine (prologue faces 236, 112 / 113): walked into from behind, tru.exe lets Lara through a terrain wall; from
    the front she stops 68 short. So her wall contacts are one-sided, as the segment-triangle test 0x461640 is.
  - Changes: the slide skips only faces it leaves (`kSweepSkipLeaving`); the ground sweep is one-sided; push-out
    runs every ground tick from front faces; the roll ends crouched under a low ceiling (chart S7).
  - The switch stand point now follows the nearest collision face's normal (0x704730 → 0x6F4740, Wine 174.0 vs
    ours 179.4).
  - The save case 9 setup moved off a path that relied on the double-sided sweep.
  - Open: deep-behind pull-out, air / hang / swim push-out, the walk-stop gap (66-68 vs 79), the double-sided
    raycast.
- **agent/Flinch8:** the fire flinch overlay is done.
  - larabase 21/22 write to the upper-body chart (`*(0xFADB58+0xF14)`, lara.drm `+0x12530`). S0 → S90 is gated by
    query 0x645. Action 0x760530 plays the pattern on animation section 1 (mask 1 = segments 14..106).
  - Wine-verified in volume 4529: fade 0.25 / frame in, 0.2 / frame out; restarts while standing.
  - Open: no flinch in water (condition 0x1F5).
- **agent/CamRest8:** OU-CAM-006 commands 1, 3-6 (overrides, 0x609F10 / 0x609D40) and 7 / 8 (limits,
  0x607A30) are implemented. Triggers now apply in entry order and only on change (0x601D30, 0x5E85B0 / 0x5F2A70).
  - Wine-verified: 4468 (command 4; the distance is 790.1 because of entry order) and 4349 (command 8).
  - Open: 0xC (cut), 0x14 (focus points), 0x16 (combat camera), 9, AnchoredCamera, OU-CAM-004.
- **agent/Joint8:**
  - OU-PHY-001 is fixed: a section pose is body A × joint motion; live fixed joints follow body A; freed riders
    keep the carried pivot.
  - OU-PZL-014 was already fixed (6d6c9eb). The ring now rides the leaf, and the test is strengthened against
    Wine s4.txt. The leaf's launch speed still differs; cause unknown.
  - OU-PZL-008 (a) is fixed: the sight ray ends at the target's grapple feature (0x713090 → 0x6FF040). The
    either-end rule is [I].
  - OU-PZL-008 (b) is narrowed: tru.exe's rope bends are chained physics bodies (0x6A4D40 / 0x699A50 / 0x699960).
- **Full ctest on int/batch8** (private kwin, game_vulkan_rt_smoke excluded): 100 % of 1084 passed.

## 2026-11-20 — batch 9 (released as 0.58.0)

Between batch 8 and batch 9, 0.56.1 / 0.56.2 (time of day under water) and 0.57.0 / 0.57.1 (sky domes, OU-REN-001)
were released from single fixes. Batch 9 worktrees moved to NVMe (`/mnt/4TBNVMEXT4/openunderworld-work`, game data
copy `/mnt/4TBNVMEXT4/ou-gamedata`): four agents' real-data tests had saturated the HDD (IO pressure full 85 %,
selftest / StreamReal timeouts).

Merged into `int/batch9` (worktree `/mnt/4TBNVMEXT4/openunderworld-work/wt-int9`):

- **agent/SurfTint** (player report): OU-WAT-014 fixed. The above-surface under-water pass needs the camera looking
  down, acos(camera row 2 z) > 2.0 rad (0x6D9DA0 / 0x5EA200 / 0x431CA0). Open: third gate 0x69E270.
- **agent/CamCut9:** OU-CAM-006 0xC cut (0x601D30 → 0x6017D0, distance state +0x1D4 kept) and 0x14 focus points
  (unit data +0x200 / +0x204, 0x5F0B30 / 0x5F0DF0..0x5F1060 / 0x5F12F0, look-idle ramp 5..7 s in 0x6009C0;
  Wine 0.394 share at volume 3859). 9: no volume uses it. Open: 0x16 (needs LaraCombatCamera), focuspoint script
  type, a clean Wine walk-in of the cut.
- **agent/Drop9:** OU-PLY-007 (fling aims at the ledge point, 0x73EDC0; lands at the line) and OU-PLY-008 (S11
  holds 3 frames over the edge, S542 10-tick attach blend; Wine `w2/Drop9/s1`). Open: plain run-off hold, the
  24-tick descent path, S543 attach.
- **agent/ShelfOut9:** OU-PLY-003 (S237 T1110, clearance 0x74E290 → 0x74E160 → shelf test 0x74BB50 result 8).
  Open: Wine check at l2_cliffclimb ledge 936, the swim ledge query's reach.
- **agent/Swim9:** OU-SWM-001 / 002. The stick is read against the view, which follows a second tracker (+0x620,
  0x6009C0 / 0x6022F0 → 0x609830, dead zones from mode 20 memory); circles 47.3 / 47.1 (surface) and 47.8 / 47.1
  (under water) vs tru.exe 46.7 / 46.4 and 48.3 / 48.0. Conflict with CamCut9 resolved on the agent branch: focus
  points turn the look point in modes 19 / 20 (0x6022F0 → 0x5F1540). Open: under-water look record, other modes.
- **Full ctest on int/batch9** (private kwin, NVMe game data, game_vulkan_rt_smoke excluded): 100 % of 1089 passed
  in 7 min.

## 2026-11-20 — batch 10 (released as 0.59.0)

Picked from docs/audit/BRIEF_COMPLIANCE.md §16 across phases (the user pointed out that batches 6-9 only took
BUGS.md player fixes). Wine sessions now run in parallel (private NVMe prefixes, separate CPU sets); private
compositors run as transient services (scopes leaked xdg-desktop-portal / ksecretd).

- **Compliance10:** BRIEF_COMPLIANCE re-audited at 0.58.0 (§0 summary, §16 remaining queue); doc drift P0-1, P11-1,
  P11-2, P11-4.
- **WineCheck10** (Wine): camera cut shown at the distance state (0x6017D0 writes the proposed point); under-water
  look record Wine-verified; the 0.58.0 wall-hug climb-out disproved at ledge 936 and removed; swim climb-out
  A / B / C onto ledges 90+ above (hang 61.9 out, end 62 in, as traced).
- **GapsA10 / GapsB10** (Phase 4, Wine): known gaps 11 → 1. ch09 live grapple points (0x713090 → 0x6FF040), ch04
  ground jumps to perches, ch05 route, ch06 route (not a bug), lean-then-rush to bar 238 and bar drop onto beam 233,
  ch10 air throw (rope taut at the shortest reach), ch17 player-ray flag 0x800 (0x48EF50), bedroom doors compare
  fix. Natla grab narrowed (early take-off at a moving target, 0x7296C0 / 0x7027B0 lead).
- **VkHang10** (Phase 10): game_vulkan_rt_smoke passes on RADV (stale grep since 0.51.0; the Oct-30 hang does not
  reproduce); four validation fixes (OU-VK-006); first ASan/UBSan + validation run recorded. Integration ctest runs
  the test again (user's choice).
- **RenderCap10** (Phase 3/9): apitrace D3D9 captures of l3_main_bedroom, l2_cliffclimb and the sharks' LODs;
  RENDER_PIPELINE §13; 19-row rendering classification; OU-REN-010/011/012, OU-WAT-015.
- **Chapters10** (Phase 4): docs/CHECKPOINTS.md, 303 checkpoints + 499 help lines from the data, respawn tests
  (288 stay, 25 pinned with reasons); matrices refreshed.
- **ReVerify10** (Phase 1/5): libjpeg 6b located (195 functions); verification table settled + 23 batch 6-9 rows;
  call edges + CFG summaries in docs/re/db; untagged-path inventory; contradictions OU-PLY-016..018, OU-PHY-003,
  OU-DOC-003.
- **Pages10:** tools/progress (progress.py, tests), .github/workflows/pages.yml, README badges and map; GitHub Pages
  enabled (Actions source).
- **Full ctest on int/batch10** (private kwin, NVMe data, game_vulkan_rt_smoke INCLUDED): 100 % of 1111 passed.
- Still running at release: WineSweep10 (combat / object / hazard measurements) — goes into batch 11.
