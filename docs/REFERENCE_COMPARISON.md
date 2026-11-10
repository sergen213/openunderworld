# Reference comparison: the original (tru.exe 1.1.0.0, Windows PC) against OpenUnderworld

Goal §18 of the gameplay reconstruction asks for a standing comparison of every major mechanic with the original
Windows game, with numbers. This is that document: one row per mechanic of goal §§4-12 (the 111 mechanics of
[underworld_chapter_gameplay_matrix.md](underworld_chapter_gameplay_matrix.md) "Shared mechanics", grouped as the
goal groups them; a mechanic with several measurable parts has several rows), giving the original's behaviour with its
numbers, where they come from, ours with its code location and value, the measured difference and a status.

State compared: branch `int/0.42.0` at 89d3e82 (ours: `ou-game --self-test`, docs/SELFTEST.md) against the GOG
release of tru.exe 1.1.0.0 run under Wine. Written 2026-11-10. Nothing here claims 1:1 fidelity for a row whose
status is not "matches", and "matches" means only what its row measured.

## Reading the table

- **Evidence class**: [O-runtime] = measured in the running original under Wine (the trace file is named; paths are
  under the private work directory `openunderworld-work/`, which is not part of the repository); [O] = read from tru.exe
  (address given); [O-data] = read from the game's data (record / chart state given); [I] = inferred; [U] = unknown.
- **†** marks an original side that is only decoded, not measured live. Decoded constants that ours copies cannot
  disagree with themselves: such rows are "unmeasured (same data)", never "matches".
- **Status**: `matches` = both sides measured and inside the tolerance of "Method"; `differs: ...` = by how much;
  `unmeasured` = one side (nearly always the original) has no live measurement; the session that would give it is in
  "Unmeasured".
- Units: game units (3.1605 units = 1 cm); "a frame" = one 30 Hz frame of tru.exe (its velocities are stored per
  frame: x 30 = units/s).

## Summary

| Goal §18 quantity | State |
|---|---|
| Character movement speed | walk, run, sprint, swim, swim sprint measured on both sides: within 1 % |
| Sprint duration | measured: 9.5..9.9 s against 9.7 |
| Jump height | standing 201.6, stick 218.2, running 158.0: equal within 0.2 |
| Jump distance | running 987.4 against 990.3; stick 680 against 680 (to where tru.exe's velocity ends), 806 against 808 from the press |
| Jump timing | press to leaving the ground 342..359 / 273..290 / 70..109 ms (standing / stick / running; the key reaches tru.exe up to 35 ms late) against 333 / 267 / 67 ms: equal |
| Chimney jump | measured at l2_junglehub (wall attach 6 frames, hold 12 frames, turn 6 frames, rebound 46.81 + 77.27 a frame, the four-jump climb): rebounds rise 360.6 / 338.4 against 360.6 / 360.6 |
| Falling speed | gravity 5.72443 a frame² on both sides, no terminal speed |
| Grapple swing duration, trajectory | **unmeasured**: five Wine attempts did not get her onto the rope ("Grapple"); ours measured: half period 2.5..2.6 s on a 3632 rope, 54..58° with Forward held |
| Climbing speed | clip root motion on both sides; no live trace of shimmy / ladder / free-climb rates: unmeasured |
| Animation transitions | take-off clips' start frames wrong (jump timing, run start); otherwise not measured |
| Character rotation | surface swim turn 117.5°/s measured and equal; ground turn rates unmeasured |
| Motorcycle acceleration, turning, jumping | unmeasured: no Wine trace of the bike exists |
| Collision boundaries | capsule (79.01 / 489.2), step bracket (100..121), ledge reach limits (424 / 1222.9) measured |
| Animation transitions | a request's clip starts at (start − blend) frames (tru.exe 0x441FB0): applied to the take-off, run start and wall jump clips, whose timings are measured and equal; the other clips still start at their +0x1A frame and are not measured |

Rows by status are counted at the end ("Counts").

## Method

**The original.** tru.exe runs under Wine (prefix `openunderworld-work/wine`) in its own nested compositor
(`poleattach/start-kwin.sh`, 1280x720), one session at a time (`wine.lock`). `poleattach/newgame.sh` starts a new game;
`wmem goto <unit>` (tru.exe 0x5DF8C0) loads a unit. A helper process (`climbq/pt.c`, this pass: `gameplaygoal/
reference/ref2.c`) reads and writes the game's memory:

- `trace <ms> [vk:down:up ...]` sends key presses (SendInput, scan codes) at the given milliseconds and prints a line
  whenever the player's state changes: time, position (Instance +0x20), rotation (+0x30, z = heading), velocity
  (+0x2A0, units per 30 Hz frame; the game clears it on the ground, where motion is the clips' root motion), the
  state-chart state pointer and the animation id. tru.exe renders at ~60 Hz under Wine with a variable time step
  (`timeMult = dt / 33.3 ms`), so samples are ~16.7 ms apart.
- `tp` / `tpv x y z ms` hold her at a point (`tpv` also zeroes the velocity: a held fall otherwise builds speed and
  she tunnels through the floor on release); `pole yaw` sets her heading; `CAMFOLLOW=dx,dy,dist,h` pins the camera
  behind her so that Forward means the world direction (dx, dy) (the stick is camera-relative; writing her heading
  alone fights the steering); `god` keeps her health full.
- Speeds on the ground are the least-squares slope of path length over time in 0.5 s windows; flights are the runs
  of samples with a non-zero velocity field: take-off = the sample before the first, landing = the last (the game
  ends the velocity 13..23 units over the floor and the landing clip takes her down: "landing" below). Tools:
  `gameplaygoal/reference/tr.py` (jumps, speeds), `swing.py` (pendulum turning points and periods).

**Coordinates.** After `wmem goto <unit>` tru.exe's world is our unit frame plus a per-unit offset = its start
position minus our `player` intro of that unit (checked on three units: l4_ballcourt + (5881.21, −10787.21,
−15435.93), l2_junglehub + (8558.2, −19414.5, −18073.1), l3_main_hall + (1583.82, 1911.28, −1939.49); the hall's
agrees with the offset from grapple ring 4089's instance within 0.1). In the prologue's own frame (new game, no
`goto`) l3_main_bedroom is ours + (9622, 333, −2977). Headings: tru.exe's forward is (sin yaw, −cos yaw), ours
(cos yaw, sin yaw): tru yaw = ours + 90°. A death reloads the last checkpoint in the frame of the *chapter*, not of
the `goto` unit: every later teleport with `goto` coordinates then drops her through the world (this cost the first
two grapple sessions and the first chimney session; check the player instance and position after every placement).
Only the first `goto` of a session is reliable (a second one left the old unit loaded twice out of three times): one
unit per session. A hang is reached by pinning her (`tp`, not `tpv`: the fall state must run) 600 over the hang's root
and 6 farther out for 1.5 s, facing the wall: the fall's own air grab takes the ledge.

**Ours.** `ou-game --self-test FILE.json --self-test-trace` (docs/SELFTEST.md) plays the same inputs at the same spot
in the real unit and prints state, clip, position and speed every tick. Scenarios and logs of this pass:
`gameplaygoal/reference/ours_*.json`, `out/*.log`; `ours.py` summarises a log in the terms of `tr.py`. The binary was
the worktree build of `int/0.42.0` (89d3e82), Release.

**Tick rates.** tru.exe steps its chart and physics once per rendered frame with `timeMult`, and its constants are
per 30 Hz frame; ours ticks at a fixed 60 Hz and integrates the air exactly (`pos += v t + a t²/2`), so the results
do not depend on the step. A chart condition "N frames" is N / 30 s in ours. Times read off a tru.exe trace have a
resolution of one rendered frame (16.7 ms); a key reaches the game on the frame after SendInput, so a press-to-effect
time carries 0..35 ms of latency that ours (input read on the same tick) does not have.

**Landing.** tru.exe ends a flight's velocity when the landing state takes over, 13..23 units over the floor, and
the landing clip's root motion covers the rest; ours flies to the floor. A jump length "to the end of the velocity"
is therefore up to 0.03 s x the horizontal speed shorter in the original's trace than ours to the floor (36 units at
1200, 59 at 1975); heights and apexes are not affected.

**Tolerances** (a row "matches" inside them): speeds ±2 %; lengths and heights ±25 units, or ±1 frame's travel
where the landing rule above applies; times ±35 ms (one 30 Hz frame + the key latency); angles ±2°; rates fitted
over less than 0.5 s are quoted with their spread and not called a match.

**What was re-opened.** The values of this document were read from the trace files in this pass for: walk, run,
sprint, all ground jumps, hold-jump, hang leaps, the slide, swim and swim sprint speeds, pole flings, the standing
jump beside a wall, the chimney and grapple sessions. Where a row says "as recorded in docs/GAMEPLAY.md" the trace was
not re-opened and the number is the doc's.

## Movement (goal §5 "Basic movement", "Jumping")

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M94 | Walk | Walk held: steady **351** units/s (0.5 s windows 338..363) | [O-runtime] `gameplaygoal/inputrules/wine2.txt` segment "walkoff" (l4_temple block top); clip `ng_walk` root motion 348 [O-data] | `Player.cpp` Walk: `ng_walk` root motion, self-test `ours_flat` step `walk`: **348.0** | −3 (0.9 %) | matches |
| M94 | Run | Steady **1266..1293**, median **1272** units/s on the flat | [O-runtime] `jumpfid/hold_run.txt` (2.2..2.9 s), `screens/stairs/j_run.txt` (1.1..1.7 s) | `Player.cpp` Run: `ng_run` root motion; `ours_flat` step `run`: **1274.0** | +2 (0.2 %) | matches |
| M94 | Run start from rest | Path after the Forward press: 13 / 66 / 145 / 255 / 385 / 515 / 643 / 770 units at 0.1 .. 0.8 s (two traces agree within 1); her run state begins 37..41 ms after the key (three traces of `chimney3-wine.txt`, `chimney4-wine.txt`) | [O-runtime] `jumpfid/hold_run.txt`, `jumpfid/sprint.txt`; [O] tru.exe 0x4428A0 -> 0x441FB0: a request's clip starts at (start − blend) frames, here 6 − 4 = 2 | `ng_run_start` from frame 2 (`clipBlend`, `World.cpp`): 35 / 99 / 187 / 307 / 440 / 567 / 695 / 823 (`gameplaygoal/jumptiming/out/ours_flat.log`; self-test `jump_timing` `run_start`) | ours leads the key by 0.037..0.041 s (53 units at 0.8 s) = the key's latency under Wine; from the start of her run state 0 ± 4 ms | matches (was: 2..3 frames early with the clip started at frame 6) |
| M94 | Run stop | `ng_running_stop` carries her on after the release | [O-data] clip; † no stop distance traced | `Player.cpp` Stop: 120..250 units (`locomotion.json` `run_stop`) | — | unmeasured |
| M94 | Turning | Turn in place by `ng_turn_center`'s root rotation; moving turns from the chart by stick angle | [O-data] clip (7.85 rad/s looped); † rates on the move not traced | `Tuning::turnRate` 6 rad/s [I]; turn in place = the clip | — | unmeasured |
| M14 | Sprint speed | Steady **1851..1939**, mean **1915** units/s on the straight (0.1 s slices 1.1..1.8 s after the press); 1863 reached 1.0 s after Forward + Sprint | [O-runtime] `jumpfid/sprint.txt` (l3_main_bedroom corridor); `screens/controls/s_*.txt` gave ~1860 on a curve | `sprint_loop` root motion; `ours_flat` `sprint_jump`: **1927.0**, reached 0.9 s after the press | +12 (0.6 %); 0.1 s earlier | matches |
| M14 | Sprint stamina | Sprint **9.5..9.9 s**, then ~**2.2 s** of running before the next **0.9 s** burst with Sprint held | [O-runtime] `screens/controls/s_spr.txt`, `s_rec.txt`, `s_r4.txt` (l4_trailstart) | `Tuning::sprintSeconds` 9.7, `sprintResume` 0.9; self-test `sprint`: 9.7 / 2.2 / 0.9 | inside the measured band | matches |
| M14 | Sprint jump | Jump pressed while sprinting (1867 units/s): the **running jump's** take-off, 65.83 forward + 42.53 up a frame; 987.4 long, apex 158.0, 0.500 s. No longer than a running jump | [O-runtime] `jumpfid/sprint.txt` (press at 2.0 s) | Jump from `sprint_loop` = the running jump (`Player.cpp` JumpPrep): 990.4 long, apex 158.0 | +3.0 | matches (the goal's "extra distance" does not exist in the original on flat ground) |
| M13 | Standing jump | **48.04** up a 30 Hz frame (first airborne sample 45.18 after one frame of gravity); apex **201.5..201.6**; 0.550 s in the air; no horizontal motion | [O-runtime] `screens/stairs/j_stand.txt`, `jumpfid/hold_stand.txt`, `gameplaygoal/reference/chimney-wine2.txt` ja / jc (l2_junglehub) | `Tuning::jumpVz` 1441.2; `ours_flat` `stand_jump`: apex **201.6**, 0.567 s by state | apex 0.0; air +0.017 (one tick of state bookkeeping) | matches |
| M13 | Standing jump with the stick | Forward + Jump from rest: she moves **128** along the ground in the 291 ms before leaving it (2.5 frames turning on the spot, then ~28 a frame along the stick; 2.98 rad turned in 0.15 s), then **40.00** along the stick + **49.97** up a frame (first sample 47.14..47.18); apex **218.2**; flight **679..680** long to where the velocity ends 23 over the floor, 0.566 s; **806** from the press to there, 840 ms after it. The take-off state lasts 8 frames (250..267 ms in three traces) | [O-runtime] `screens/stairs/j_57.txt`; `gameplaygoal/reference/chimney3-wine.txt` j3 / j4 (181..205 on the ground before the take-off with Forward 50 ms ahead of Jump, the last of it against the column); [O-data] chart S245 / S247 (query q43), chosen at the press (S246's T283 / T281) | `Player.cpp` JumpPrep: the stick jump runs on under `ng_run_start`'s root motion for `Tuning::stickJumpFrames` = 8 frames [I: what S245's request 213 plays is not decoded], turning onto the stick at 20 rad/s; `ours_flat` `stick_jump`: **127.9** on the ground in 267 ms, then apex **218.0**, **680.0** to 22 over the floor; **808** from the press | ground travel −0.1; flight 0; from the press +2 units, −24 ms (the key's latency) | matches (was: no ground travel before the take-off). † The first four frames' motion with Forward ahead of Jump (9 / 26 / 45 / 65 units a frame in j3 / j4, faster than any clip of hers) is not reproduced [U] |
| M13 | Running jump | **65.83** forward + **42.53** up a frame (first sample 39.67); apex **158.0**; **987.4** long (landing 6 lower); **0.500 s** | [O-runtime] `jumpfid/sprint.txt`, `jumpfid/hold_run.txt` (954.4 long onto ground 15 higher), `screens/stairs/j_run.txt` | `runJumpSpeed` 1974.9, `runJumpVz` 1275.9; `ours_flat` `run_jump`: apex **158.0**, **990.3** long, 0.517 s by state | +2.9 length | matches |
| M13 | Jump timing: press to leaving the ground | Standing **342..359 ms** (356, 351, 338 in l2_junglehub; 359 l3); with the stick **273..290 ms**; running **70..109 ms** (93, 93, 70) | [O-runtime] the traces above; `chimney-wine2.txt` ja / jb / jc; [O] tru.exe 0x4428A0 -> 0x441FB0: a request's clip starts at (start − blend) frames, so `ng_standing_jump` (blend 6, start 6) plays its whole 10 frames and `ng_running_jump_forwards` (2 / 1) its 2 | `World.cpp` `clipBlend` (the lead rule), `Player.cpp` JumpPrep (the clip's end; the stick jump 8 frames): **333 / 267 / 67 ms** (`gameplaygoal/jumptiming/out/ours_flat.log`; self-test `jump_timing`) | standing −9..−26 ms, stick −6..−23 ms, running −3..−42 ms: the Wine key latency (at most 35 ms; 37..41 ms from the key to her state in three traces) | matches (was 133 / 67 / 33 ms) |
| M13 | Which jump a press gives just after the stick | Jump 15 ms after her run state begins (Forward 50 ms ahead): the stick jump (40.00 + 49.97); 71 ms after it (stick 100 ms ahead): the running jump (65.83 + 42.53) | [O-runtime] `chimney3-wine.txt` j3 / j4 against h3 (l2_junglehub) | `Player.cpp`: the stick jump within `Tuning::stickJumpLeadFrames` = 2 frames (67 ms) of the run's start, the running jump after [I: the frame count; the chart condition (T56's cond 0x14 on S64) is not decoded]; self-test `jump_timing` `stick_after_the_run_began` | the switch lies between 15 and 71 ms in tru.exe, at 67 ms in ours | matches (inside the measured bracket) |
| M91 | Holding Jump through a running jump | Jump held 1.0 s: the same flight as a tap (65.83 + 42.53 a frame, apex 158.0, 0.484 s onto ground 15 higher) | [O-runtime] `jumpfid/hold_run.txt` against `jumpfid/sprint.txt`; `hold_stand.txt` against `j_stand.txt` (identical) | no hold rule (`Player::airMove` never reads the button): `ours_flat` `hold_run_jump` = `run_jump` | none | matches: holding Jump does not lengthen a ground jump on the flat in the original. † The jumps at a ledge (re-aimed flights) were not traced with the key held |
| M13 | Jump to a ledge: standing | Ledges 782 / 830 over her feet caught from 100..**424** out of their line, not from 425; 892 / 907 ones only from 100; the catch is a straight 11-tick (60 Hz) rush | [O-runtime] `ledgecatch/`, `runjump/` (l4_temple), docs/GAMEPLAY.md "Air-grab window" | `Player::standJumpToLedge`: pad 424.5, 11/60 s rush | boundary inside (424, 425) | matches |
| M13 | Jump to a ledge: running | Caught from a take-off **1222.9** short of the line, not from 1243.4; re-aimed flight 10.5 frames for take-offs 781..1218; straight rush from 614 and closer | [O-runtime] `runjump2/rj*_*.txt` (l4_temple ledge 1204) | `Player::aimGroundJump`: reach 1233, arc 10.5 frames; rush / arc switch at 634..719 | switch point 20..70 farther out than tru.exe's 614..649 | matches reach; differs: rush / arc switch by 20..70 units |
| M13 | Falling: gravity | **5.72443** units per 30 Hz frame² (5151.99 units/s²); every airborne trace steps its velocity by 2.86 per 60 Hz sample; no terminal speed up to −231 a frame (6950 units/s) | [O] tru.exe literal 0xC0B72E88 (0x70E100 / 0x70E050, solver 0x73EDC0); [O-runtime] `chimney-wine2.txt` d (free fall 2.0 s: −2.86 .. −231.84 in steps of 2.86 per sample) | `Tuning::gravity` 5151.99; exact integration (`Player::airMove`); no clamp | 0 | matches |
| M15 | Crouch / crawl | Capsule height **260** crouched; crawl by `Crouch_Front` | [O-data] player data +0x14; clip root motion 328 units/s; † speed not traced | `Tuning::crouchHeight` 260; clip root motion | — | unmeasured (same data) |
| M15 | Forward roll | `ng_roll_front` (Crouch while running), `ng_roll_front_sprinting` | [O-data] clips (1364 units/s root motion); † not traced | `Player.cpp` Roll: the clips' root motion | — | unmeasured (same data) |
| M16 | Hurdle / vault | Ledges up to **635.27** over her feet are hurdled (`hurdle_ground_attach_high` at 610); bands by ledge class | [O-data] chart S958 / S959 / S957, 0x728650; [O-runtime] `hurdles/` sessions, `GameReal.JunglehubVaultOver` positions | `Player::tryHurdle` (bands from the same records) | — | matches (band limits); clip timing not compared |
| M17 | Slope slide | Along the fall line (vz / vxy 1.97 = tan 63.1°): **51.05 → 91.23** a frame in 0.248 s = **4855** units/s²; up to 104.2 a frame (3125 units/s) in the trace, no cap. Slides only on faces with client flag 0x0004 (normal z 0.523..0.753 seen); stands on unflagged faces down to normal z 0.626 | [O-runtime] `screens/stairs/slide1.txt` (l3_main_bedroom roof); `gameplaygoal/charvirtual/slope-wine2.txt` (l2_junglehub ramps, docs/formats/PHYSICS.md "Walkable slope limit under Wine") | `Player.cpp` Slide: g·sinθ = 4601 on that roof, no friction, no cap; slides where 0.3 ≤ normal z < 0.7 (`Tuning::slideMinNormalZ`, `minFloorNormalZ`) | acceleration −5 % (one 0.25 s trace; the velocity field is read between game frames); the rule that starts a slide is by angle in ours, by face flag in the original | differs: −5 % acceleration (within the trace's noise, not settled); **differs: slide trigger** (Differences to fix 4) |
| M17 | Jump out of a slide | † not traced | [U] | slide velocity + 1441 up [I] | — | unmeasured |
| M19 | Walk-over / cartwheel | † not traced; chart states not decoded | [U] | none | — | unmeasured (not implemented) |
| M90 | Swan dive | Jump then Crouch in the air; chart and clips decoded | [O-data] docs/GAMEPLAY.md "Water moves"; † no Wine trace | `Player.cpp` / `Water.cpp`, self-test `swan_dive` (l2_cliffclimb) | — | unmeasured |
| M90 | Landing roll | † not traced | [U] | none | — | unmeasured (not implemented) |
| M95 | Steps / stairs | Risers 97..100 are walked and run at unchanged speed, the root taking each step's height as it crosses the riser line; a 121 edge stops her 56 short | [O-runtime] as recorded in docs/GAMEPLAY.md Numbers "Step up / down" (`wmem trace` / `tp` on the manor staircase; the trace files were not kept under a name the doc gives, so the values are the doc's) | `Tuning::stepUp` / `stepDown` **110** [I: between 100 and 121] | inside the measured bracket | matches |
| — | Capsule | Radius **79.01**, cylinder 189.63, centre 315.4 over the feet (top 489.2); swimming 80 / 228 | [O-runtime] Havok proxy (instance +0x288 +0x65C / +0x660 / +0x648) | `Tuning::radius` 79.01, `capsuleTop` 489.2 | 0 | matches |
| M65 | Fall damage | 0 up to **1280.03**, 500 at **1817.32**, death at **2215.55** of fall height | [O] player data +0x4C / +0x50 / +0x54, curve 0x6F1790; [O-runtime] `gameplaygoal/inputrules/wine2.txt` (catch limit between 1100 and 1350 = the same 1280.03 record); † the damage numbers themselves not read back live | `Combat.cpp` fall damage, the same three floats; self-test `fall_damage` | 0 | matches (data); damage values unmeasured live |

## Ledges and climbing (goal §5 "Ledge traversal", "Climbing", "Chimney jumps")

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M01 | Ledge grab window | Caught while −13 × 5.72443 ≤ zVel ≤ 9 × 5.72443 a frame (**−2232.5 .. 1545.6** units/s); hang root **161** out of the ledge line and **494..506** under it | [O] player data bytes +0x833 / +0x834 (0x7594C0 family); hang pose [O-runtime] `runjump2/rj*.txt`, `gameplaygoal/inputrules/wine2.txt` | `Tuning::grabMinVz` / `grabMaxVz`; hang pose from `ledgeb_idle` (hands 495.6 over the root) | pose within 2..10 | matches (pose); the falling catch's geometry is ours [I] |
| M01 | Shimmy, corners, pull up | Clip root motion (`ledgeb_right`, corner clips, `ledgeb_climbup_fast`); which way up by the clearance class 0x74E290 | [O-data] clips, decomp; † speeds not traced | `Traverse.cpp` / `Player.cpp`: the clips' root motion, the clearance classes | — | unmeasured (same clips) |
| M02 | Free leaps off a hang | Back **53.73** out + **34.77** up, sideways **70.80** + 34.77 a frame, launched when the take-off clip ends | [O-runtime] `jumpfid/hang1.txt`, `hang_41.txt`, `hang_44.txt` (l3_main_bedroom ledge 973) | `Tuning::hangLeapBack` / `hangLeapSide` / `hangLeapVz` | 0 | matches |
| M02 | Hang jumps to a ledge | Up q10 **75.85** a frame, sides q7 / q8 **70.80 + 34.77**, back q9 **57.33 + 42.54**; hit classes by record 0x2A9EC / 0x2AA4C | [O-data] lara.drm jump queries; [O-runtime] the leaps above for the take-off, † the targeted flights' times not traced | `Player::hangJumpQuery`, `launchHangJump` (`Tuning::hangJumpUpVz` ...) | — | matches (take-off); flight times unmeasured |
| M03 | Drop to hang | `drop_to_ledgeb`, 31 frames: root 158 out and 491 down in ~1.0 s | [O-runtime] `gameplaygoal/inputrules/wine2.txt` (walkoff / runoff, l4_temple 1155) | `Player::tryDropToHang`, `trySafetyDrop`: the clip from start frame 3 | — | matches |
| M03 | Safety drop (no key) | Walking off an edge with a ledge: reaches the line 0.57 s after the start and goes straight into the hang; running: overshoots 82, taken back, hang ~1.2 s after the line | [O-runtime] `gameplaygoal/inputrules/wine2.txt` | `Player::trySafetyDrop`; self-test `safety_drop` | overshoot and take-back not reproduced | matches (outcome); differs: no 82-unit overshoot when running |
| M04 | Saving grab | Triggered when the catch is more than **711.13** (jump queries) / **1280.03** (falls) under where the flight began; held **90 / 50 / 15 frames** by difficulty, then she falls; Interact recovers | [O] 0x74FDF0, jump query records +0xC4; [O-runtime] `gameplaygoal/inputrules/wine2.txt`: firm hang from 600 / 850 / 1100 over the hang's root, weak catch from 1350 / 1600, held **1.70 s** (50 frames = 1.67 s); † the Interact recovery not exercised | `Player::badAttach`, `Tuning::badAttachJump` / `badAttachFall`, `savingGrabSeconds` 50 / 30; self-test `saving_grab` | 0.03 s | matches (limit and hold time); recovery input unmeasured |
| M05 | Free climb | Hold-to-hold moves by lara.drm queries q73 / q80 / q81; DICE jump: no ballistic arc, the hands arrive 0.39 s after the press | [O-data] queries; [O-runtime] `runjump2/dice_jump.txt`, `dice_attach.txt` (l2_cliffclimb) | `Climb.cpp` | — | matches (DICE jump time); move speeds unmeasured |
| M06 | Chimney / wall jump: which jumps cling | A **standing jump** (query q2) 111 from an unflagged wall, facing it: a plain jump, apex 201.6, no cling (3 runs). A **stick jump** (q43: 40.00 + 49.97 a frame) into the same wall: feet on the wall one frame after take-off (2 runs) | [O-runtime] `gameplaygoal/reference/chimney-wine2.txt` ja / jb / jc, `chimney3-wine.txt` j3 / j4 (l2_junglehub, column A's east face); [O] tru.exe 0x7594C0: the jump query's flag 0x80 at +0x2C (q2, q10, q23, q31, q35, q55 and the falls carry it) | `Player.cpp` (State::Jump wall test) + `WallJump.cpp` `tryWallJump`; since this pass no wall jump from a flight launched without horizontal speed (`Game.ChimneyJumpNotFromAStandingJump`, self-test `chimney_floor_jumps`). Before: the standing jump clung 45 over the floor for 0.47 s (`gameplaygoal/chimney/ours3.txt`), and a stick jump taken within a tick's travel of the wall did not cling at all (the test read the contact-clipped velocity; fixed in `WallJump.cpp`) | none after the two fixes | matches (was: differs twice; fixed in this pass) |
| M06 | On the wall: height, standoff | Root **43.2 / 43.6** over the floor she jumped from, drawn in 0.19..0.20 s to **98.5** out of the column's ledge line | [O-runtime] `chimney3-wine.txt` j3 / j4; [O-data] request 1153 (blend 4, start 1) starts the 3-frame `wall_jump_attach_air` 3 frames before its first: 6 frames | `tryWallJump`: **47.1** over the floor (the test of the tick 0.5 frames in fails on the floor rule 0x7591E0, as tru.exe's does at 24.6; self-test `chimney_floor_jumps`); standoff 110 from the ray's hit = 105.5 out of the same line; drawn over 6 frames (0.200 s, `kAttachSeconds`) | +3.7 height; +7 standoff; draw 0 | matches |
| M06 | Hold without a press | Idle on the wall **0.416 s** (12.5 frames) after the attach phase, then `wall_jump_detach_fall`: 0.267 s, root 32.9 back and down to the floor; on the wall 0.617 s in all | [O-runtime] `chimney3-wine.txt` j3; [O-data] chart variable 208 = 12 frames | `Tuning::wallJumpHold` 12 / 30 = 0.400 s, then the clip from its frame 0 (request 1157: 3 − 3); on the wall 0.600 s in all (`Game.ChimneyJumpNeedsThePress`: 36 ticks) | idle −0.016 s; total −0.017 s | matches |
| M06 | Rebound | Press 58 ms into the idle: `wall_jump_jump_back` for **0.200 s** (half turn; 200 / 200 / 201 ms in `chimney4-wine.txt` h1), then **46.81** out + **77.27** up a frame (first sample 74.48 after a frame of gravity), from 22.8 out and 37.0 up of the wall position. A press while she is still drawn onto the wall does nothing and is not kept (h1: presses 99, 43 / 193 and 60 ms into the three attach phases, one held 48 ms into the idle) | [O-runtime] `chimney3-wine.txt` j4, `chimney4-wine.txt` h1; [O-data] query q12; request 1156 (blend 2, start 1) starts the 5-frame clip a frame before its first: 6 frames | `Tuning::wallJumpOut` 46.81, `wallJumpVz` 77.27; the turn clip 0.200 s; the press only from `wall_jump_idle` (`WallJump.cpp`, `Game.ChimneyJumpNeedsThePress`); first flight sample 23.4 out, 38.0 up | velocity 0; turn 0 | matches (was: turn 2 frames short, press taken during the draw) |
| M06 | The climb at l2_junglehub: ledge 796 → 797 between the columns | From the hang (root z 5855.5): Back + Jump leaps back 407 ms after the press at **53.73** out + **34.77** up a frame (the free leap's speeds); feet on column B 0.149 s later, **130.6** over the hang's root; then three presses: B → A → B → ledge 797, hanging **1247.5** higher 3.17 s after the first press. Four jumps | [O-runtime] `gameplaygoal/reference/chimney4-wine.txt` h1 | `GameReal.JunglehubChimneyJumpClimbsColumns`: the same four jumps; on B **138** over the hang's root; rise 1248 | +7 on the first wall; same count, same end | matches |
| M06 | Rise per rebound, where the next wall takes her | The flight after a rebound ends in the wall state 6.0 and 5.5 frames in (the test runs every game frame, 60 a second under Wine; zVel is inside its window from 4.5) with her root 224.1 and 235.5 short of the wall's face, not from 247.4 (5.5 frames into the first), still 105..115 short of where she will hold; she is drawn there level in 0.2 s. Wall to wall she gains **360.6** and **338.4** | [O-runtime] `chimney4-wine.txt` h1: holds at +130.6, +491.2, +829.6 over the hang's root, 119..131 out of the faces; [O] tru.exe 0x758DD0: the rays are 3 x the length of player +0xA20 long, and +0xA20 is the velocity the jump was launched with (stored by the launch function before 0x7522A0): 271 for the rebound's 46.81 + 77.27 | `tryWallJump`: rays 3 x the launch speed, tested every tick; the walls taken 6.0 frames into both flights: **360.6** and **360.6** (`GameReal.JunglehubChimneyJumpClimbsColumns`, re-pinned to tru.exe's numbers ± 25): holds at +138, +499, +859 | 0 and +22 a rebound; holds +7 / +7 / +30. † Ours' holds are 110 out of the faces (418 apart against 395 / 383) and its rays reach 271 from over her root against a measured 235.5..247.4: the two differences are of the same size and [U] (0x4935F0's hit point, the pelvis in the blended pose) | matches (each rebound within 25; the third hold is 30 high; was 435 a rebound) |
| M06 | Hold after a leap from a hang, no press | On column B 0.200 s of draw, idle **0.404 s** (12 frames), `wall_jump_detach_fall` 0.259 s (32.6 back, 149.4 down), then the fall | [O-runtime] `chimney4-wine.txt` h3 | as the floor case above: 0.400 s idle; clip 33.7 back, 154.4 down | −0.004 s | matches |
| M06 | Last jump onto the ledge | From B at +829.6: a re-aimed flight (first sample 36.32 out + 67.06 up a frame), 0.283 s, then 0.53 s into the hang on 797 | [O-runtime] `chimney4-wine.txt` h1 | `wallJump`: `ledgeOnArc(q12)` → `launchHangJump` from +859 | ours takes the ledge from 30 higher up (the rebounds above) | matches (outcome); flight not compared |
| M06 | Five in a row | From the hang on ledge 797 with a press every 150 ms: the leap back, then **five** jumps back (the walls give no height there); on the wall after the fifth the presses do nothing: she holds 0.416 s (12.5 frames), drops with `wall_jump_detach_fall` and catches 797 again | [O-runtime] `gameplaygoal/reference/chimney4-wine.txt` h2; [O-data] `PSCCF_WallJumpAllowed` (chart cond 0x21C, 0x7660D0: count < byte +0x843 = 5) | `Tuning::wallJumpMax` 5; `Game.ChimneyJumpFallsAfterFiveInARow`, `GameReal.ValaskjalfChimneyJumpTakesAllFive` | none | matches |
| M11 | Perch | Attach, turns and jumps by the perch group's clips and queries | [O-data]; † not traced | `Traverse.cpp` perches | — | unmeasured (same data) |
| M12 | Thin column (pillar) | Climb by clips; slide down measured | [O-runtime] `polefid2/pil1.txt`, `pil2.txt`, `pil3.txt` (as cited in docs/GAMEPLAY.md "Ladders, pillars, perches") | `Traverse.cpp` `pillar` | — | matches (slide, per GAMEPLAY.md); not re-fitted here |
| M18 | Faster climbing by timed Interact | † not traced; no chart condition found | [U] | none | — | unmeasured (not implemented) |
| M76 | Moving markup | † not traced | [U] | none | — | unmeasured (not implemented) |
| M78 | Rideable machinery | † not traced | [U] | none | — | unmeasured (not implemented) |
| M80 | Wall hug | Shelf class 8 of the clearance test; jumps q68 **74.32 + 42.54**, twist q55 **50.82** up a frame | [O-data] chart + queries (`poleshimmy/wallhug` FINDINGS); † not traced | `Traverse.cpp` wall hug, `Tuning::wallHugJump*` | — | unmeasured (same data) |
| M96 | Ladder | Rungs **110.619** apart, the first 575.221 under the top; jumps q22 **47.41 + 60.05**, q23 **80.15** up a frame | [O] 0x734B70 + player data; † climb rate and jumps not traced | `Traverse.cpp` ladders, `Tuning::ladderJump*` | — | unmeasured (same data) |
| M98 | Anchor and chain | † not surveyed | [U] | generic ledge code | — | unmeasured |

## Poles and beams (goal §5 "Horizontal and vertical poles", "Balance beams")

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M07 | Pole grab from a jump | A jump with the pole's hang ~330 out and 0..250 up flies straight there in **11 ticks of 60 Hz** (standing 315 out at 1718 units/s, running 208 out at 1135); the catch hands over to the idle hang **0.62..0.85 s** later | [O-runtime] `polefid2/grab1.txt`, `grab2.txt`, `poleattach/s2..s6.txt` (l4_traprooms pole 1336) | `Player::tryPoleLeap` 11 ticks; hand-over 0.70 s (`Hpole_perp_attach_M` cut at 0.83) | inside the measured band | matches |
| M07 | Pole swing | Clip-driven (`HPole_StartSwing` → `HPole_swing`), no physics condition on the swing states; stop clips by loop phase | [O-data] chart; [O-runtime] `polefid2/swing1.txt` (phases) | `Player.cpp` pole states: the same clips and phase boundaries (7 / 5 / 19 / 10 frames) | — | matches (clip-driven both sides) |
| M07 | Pole fling to the next pole | Leaves **624..628 ms** after the press; **43.42** forward + **52.09** up a frame (first sample 49.23..49.28); **0.782..0.783 s** (23.5 frames) in the air; apex +237.0; 1020 long, 356 down to the catch | [O-runtime] `polefid2/fling1.txt`, `fl3.txt` (l4_traprooms 1336 → 1337) | `Player::poleFling`: leaves 0.64 s after the press, q33's 52.10 up, 23.5 frames to the nearest pole / ledge 300..1800 ahead | ≤ 16 ms, ≤ 0.5 frame | matches |
| M07 | Pole fling onto a platform | l3_main_hall 948 → 946: **35.87** forward + **74.1** up a frame (first sample 71.26); 0.783 s; apex +479.7; 824.8 long, landing 190.6 higher | [O-runtime] `polefid2/mhsw.txt` | same rule (23.5 frames to the target); lands one capsule radius past the edge [I] | touch-down 3 short of the edge in tru.exe against 79 past it in ours | matches (flight); differs: landing point by ~80 units |
| M07 | Fling timing by swing phase | Presses 1.6 s and 2.8 s into the swing give identical flights; 0.31 / 0.81 s delays seen without a target | [O-runtime] `polefid2/fling1.txt`, `fling2.txt`; the no-target rule [U] | fixed 0.64 s | — | matches with a target; unmeasured without |
| M08 | Hand over hand | `HPole_ParaForward`: 281.7 units per 1.37 s cycle (clip root motion) | [O-data] clip; [O-runtime] `polefid2/hoh.txt`, `hoh2.txt` | the clip's root motion | — | matches (same clip); speed from the trace not re-fitted here |
| M08 | Turns on the pole | Along ↔ across in **0.30 s** | [O-runtime] `polefid2/turn1.txt` | `Hpole_PerptoPara` played in 0.30 s | 0 | matches |
| M08 | Onto the pole's top | `hpole_to_perch`: root **19.9** over the axis | [O-runtime] `polefid2/pclimb.txt` | root 19 over the axis | −0.9 | matches |
| M08 | Jumps from the pole top | Forward / back q33 / q34 (73.72 forward + 52.10 up a frame without a target), sideways q69 / q70 (45 + 70), up q35 (53.73) | [O-data] lara.drm jump queries; † not traced | `Player.cpp` `hpole_perch_Jump_*` with the same query speeds | — | unmeasured (same data) |
| M09 | Pole to collinear pole | † behaviour at the pole's end not traced | [U] | pole fling code | — | unmeasured |
| M97 | Hang-jump / drop onto a pole | † not traced | [U] | hang jumps + pole grab | — | unmeasured |
| M10 | Beam: walk | `bbeam_walk` root motion 501 units per 3.57 s cycle (140 units/s) | [O-data] clip; † not traced | `Traverse.cpp` beam: the clip's root motion | — | unmeasured (same clip) |
| M10 | Beam: balance, about to fall, fall | Controller 0x742330 / 0x742770 on objdata +0xE98 (walk: sway ±50°, tolerance 10°) and +0xF88 (about to fall: tolerance 15°, fall angle 100°, not improving 30 frames, unbalanced 45, full lean 10 frames); falls by query q64 from 47.41 to the side | [O] decomp + lara.drm; † no Wine trace of a beam | `Player::beamAboutFall`, `startBeamFall` with the same tuning; entry into about-to-fall after 45 frames off the line [I] | — | unmeasured (same data; entry rule inferred) |
| M10 | Beam: catch after the fall | `bbeam_fall_left`: 0.33 s, root 172 aside and 486 down onto the ledge hang | [O-data] clips | the clips' root motion (`warpStep`) | — | unmeasured (same clips) |
| M10 | Beam sprint | † not traced | [U] | none | — | unmeasured (not implemented) |

## Grapple (goal §6, §4 "Grapple physics")

The original swings a Havok rigid body on the rope (sphere radius 30, mass 55, damping 0.022); ours is a point
pendulum under the forces decoded from tru.exe 0x746F60 / 0x745FA0 and Lara's player data. Ours was measured on
l3_chapel ring 4744 from the column top (`gameplaygoal/reference/ours_chapel.json`, `out/ours_chapel.log`; the same
spot as self-test `grapple_swing`) and on l3_main_hall ring 4089 (`ours_swing.json`).

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M20 | Target selection, throw range | Six target sets (25° / 2880, 15° / 5120, ...), range 4424.78 or the target's own (ring 4089: 3792.67), rope 553.1..4424.78 | [O-data] † player data +0x464, g_grapple +0xF8..+0x10C, target block 0x404; [O-runtime] only the stowed-hook point (30 ahead, 37 right, 295 up of her root) | `World::grappleTarget`, `GrappleTuning` | hook point 22 / 37 / 303 | unmeasured (same data) |
| M20 | Hook flight | 1580.28 a frame [I unit] | [O-data] † g_grapple +0xFC | attaches on the tick of the press | — | unmeasured |
| M20 | Rope she hangs on after a jump into the gap | Her distance to the anchor when the jump comes down with no ground under her (chart T1387 / T291) | [O-data] † chart | `Grapple.cpp`: l3_chapel 4208.5 on the column top → **3632.0** hanging ("no ground" = none within 316, `kHangClear` [I]); l3_main_hall 2154 | — | unmeasured |
| M20 | Swing period | Pendulum of a 55-unit body under [U] gravity | † no live trace | free swing, rope 3632: half period **2.47..2.65 s** (2π√(L / g) / 2 = 2.64 s at g = 5152) | — | unmeasured |
| M20 | Swing with no input: decay | Brake 1.0 x velocity (+0x9E0) going outward, body damping 0.022 (+0x8DC); time base [U] | [O] † 0x746F60, player data | amplitude 58.1° → 24.1° → 7.3° → rest; speed under the pivot 3270 → 1014 → 309 → 96 → 30 units/s (x 0.31 a pass) | — | unmeasured |
| M20 | Swing with Forward held: growth and limit | Drive 3.00..4.74 a frame² by rope length along her motion; no drive and a brake of 2.5 x velocity past **45°** going out (+0xA04 / +0xA08); speed cap 379.27 a frame | [O] † 0x746F60, player data +0x9D8..+0xA3C | l3_chapel: **54.4..54.9°** on the forward side every pass, 4660..4680 units/s (155.3..155.9 a frame) under the pivot going forward, 3882..3894 going back (the back swing meets the wall 2121 behind the pivot); l3_main_hall, rope 2154: 56.1..58.0°, 3470..3570 units/s, half period 1.89 s | — | unmeasured |
| M20 | Jump off the rope | max(63.21, 0.45 x horizontal speed) along her facing + max(47.41, 0.85 x vertical speed) up a frame | [O] † 0x73C640 case 734, player data +0xA48..+0xA54 | `kFreeJumpFwd` 1896.3, `kFreeJumpUp` 1422.3 units/s from rest (3D 1978 in the trace) | — | unmeasured (same data) |
| M20 | Roped walk, walking off the edge | Ordinary ground set with the rope in hand; hangs when she walks off an edge under the anchor (T1409) | [O-data] † chart | `kRopedWalk` 300 units/s [I]; l3_chapel walk-off: rope 4127; l3_main_hall walk-off: she comes down on the staircase and runs on (no hang) | — | unmeasured |
| M99 | Rope climb / slide | Interact + forward **23.70**, + back **41.09** a frame x the stick; not past 40° | [O] † player data +0x9E4 / +0x9E8 / +0x9EC | `kFreeAscend` 711.1, `kFreeDescend` 1232.6 units/s: 698 up in 1.0 s, 1394 down in 1.2 s (`ours_chapel` step `full`) | — | unmeasured (same data) |
| M21 | Grapple thrown in mid-air | † allowed states not decoded | [U] | thrown from any state | — | unmeasured |
| M22 | Rappel | Interact + forward **34.77**, + back **44.25** a frame on a wall; overhang rules (553.1 / 110.6 / 316.1 / 63.2) | [O] † 0x745FA0, player data +0x924 / +0x928, 0x73AB70 | `kWallAscend` 1043.0, `kWallDescend` 1327.4 units/s; self-test `rappel` | — | unmeasured (same data) |
| M23 | Wall run | Drive 1.90..6.32 a frame² by rope length, not past **55°**, brake 2.5 there, cap 97.98 a frame | [O] † 0x745FA0, player data +0x940..+0x970; reversal rule not decoded | `Grapple.cpp` wall run (`kWallRun`, `kWallMaxAngle`, `kWallMaxSpeed`); self-test `wall_run` | — | unmeasured |
| M24 | Jump off the wall | Sideways 45.83 along the wall + 44.25 up; back 53.73 out + 44.25 up a frame | [O] † 0x73C640, player data +0x994..+0x9A8 | `kWallJumpSide` / `kWallJumpBack` / `kWallJumpUp` | — | unmeasured (same data) |

**Sessions.** No live trace of the rope was obtained; five attempts failed before she was on the rope:

- l3_main_hall ring 4089, three sessions (two by an earlier pass, `gameplaygoal/grapin/wine1.txt`, `wine2.txt`; one
  here, `gameplaygoal/reference/failed/grapple2-session4-hall.txt`): Lara set on the balcony (ours 2441, 3912, 1709)
  is not standing there a second later: she comes to rest 277 lower and is then gone (the player instance is freed and
  the game restarts the prologue in the bedroom), so every later placement used the wrong frame. Whether the balcony's
  floor is missing after `wmem goto l3_main_hall` (a neighbour unit not streamed in) or a script kills her there is
  not known.
- l3_chapel ring 4744 (`failed/grapple3-session7a-chapel.txt`): 100 s after `wmem goto l3_chapel` she is not at rest:
  she has moved 820 from the start by herself and a 6 s position hold leaves her where she was: the unit starts with
  a scripted sequence that owns her.
- l4_pool ring 5511: not tried; the only known stand point (our test's) is in the eitr.
- A second `goto` in one session did not load its unit twice out of three times (`failed/`), which cost two more runs.

What to try next is in "Unmeasured".

## Water (goal §7)

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M29 | Swim speed | **45.00** units per 30 Hz frame (1350 units/s) at full stick, at the surface and under water (median and maximum of the velocity field) | [O-runtime] `screens/stairs/swim_l2b.txt`, `swim_l2c.txt` (l2_cliffclimb sea) | `Tuning::swimSpeed` **1348.6** | −1.4 (0.1 %) | matches |
| M29 | Swim start / stop | From rest 0.54 → 8.3 → 33.1 → 44.3 a frame at 0.05 / 0.2 / 0.5 / 0.87 s; after the release 44.6 → 21.6 in 0.18 s | [O-runtime] the same traces (values as fitted in docs/GAMEPLAY.md Numbers) | `swimAccel` 2475, `swimDecel` 3760 units/s² (linear fits of an S-shaped ramp) | linear against S-shaped: up to ~0.1 s apart mid-ramp | matches within the fit |
| M29 | Swim sprint | **54.00** a frame (1620 units/s) sprinting against 45.0; 9.95 s of sprint, then 2.13 s at normal speed and 0.95 s bursts | [O-runtime] `scratch/swimsprint/surf1.txt`, `under1.txt` | `swimSprintFactor` 1.2 → **1618.3**; stamina shared with the run sprint | −1.7 (0.1 %) | matches |
| M29 | Surface turn | Steady **117.5°/s** at full speed; ~16° of lag between velocity and heading | [O-runtime] `camfollow-wine/turn1.txt` (l1_seatop) | 2765 / speed rad/s → 117.5°/s; lag ~9° in a 90° turn | lag −7° | matches (rate); differs: lag by ~7° |
| M29 | Depth classes | Wading from **154**, swimming from **391** of depth; dive needs **948** | [O] player data shorts +0xAC..+0xB2, 0x5D3860; † not traced | `Tuning::wadeDepth` / `swimDepth` / `diveDepth` | 0 | unmeasured (same data) |
| M29 | Surface level | Root **44.1** under the wave surface | [O-runtime] l2_cliffclimb sea (docs/GAMEPLAY.md Numbers; rms < 1) | `Tuning::swimSurfaceDepth` 44.1 | 0 | matches |
| M29 | Breath | **4000 frames** (133.3 s), x 1.5 drain sprinting, refill in **60 frames**; then 10 % of health every 20 frames | [O] 0x755230 / 0x756C40, player data +0xF8..+0xFE; † not timed live | `World::tickAir`; self-test `swim_breath` | 0 | unmeasured (same data) |
| M29 | Climb out, dive, dodges, swan dive entry | Chart and clips decoded | [O-data] docs/GAMEPLAY.md "Water moves"; † not traced | `Water.cpp`, `Player.cpp` swim states; self-tests `swim`, `swim_moves` | — | unmeasured |
| M29 | Under-water steering by camera pitch | Goes on down after a dive at swim speed along the path (1034 units deep in the trace) | [O-runtime] `scratch/swimsprint/under1.txt` | `Player.cpp` SwimUnder | — | matches (speed along the path); pitch rates unmeasured |
| M30 | Under-water interaction | † not traced | [U] | `Objects.cpp` use / pick-up | — | unmeasured |
| M85 | Scuba | No drain while the chart variable 0x322 is clear (scuba levels) | [O] 0x755230; † not traced | `World.cpp` breath rule | — | unmeasured (same rule) |
| M86 | Carry items while swimming | † not traced | [U] | `Objects.cpp` portables | — | unmeasured |
| M100 | Under-water combat | Speargun projectile 1000 a frame [I unit], damage 250; rifle 23 / 23 / 18 under water | [O-data] weapon records; † not traced | `Combat.cpp` | — | unmeasured (same data) |
| M66 | Deadly liquid (eitr) | Contact kills: l4_pool 5608 at eight spots | [O-runtime] docs/GAMEPLAY.md "Water" (Wine, same outcome at eight spots) | `Water.cpp` + script poll | same outcome | matches (l4_pool only) |

## Combat (goal §8)

None of the combat numbers was measured in the running original: every row's original side is decoded from tru.exe and
the game's data (†), and ours reads the same records, so "matches" would only say that a constant was copied. The
status is therefore "unmeasured" throughout, with what a live session would have to time.

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M31 | Lock on / soft target | Hard lock tiers: nearest within **1264**, then smallest camera angle within **8000** and 90°, then within 8000 and 90° of her facing; soft target within **5000** and 27.5°; line of sight from 375 over her feet | [O-data] † Lara data +0x480 / +0x470 criteria records | `Combat.cpp` targeting, the same records | tier use [I] | unmeasured |
| M31 | Weapons: damage, range, clip, spread | Pistols 40 / 30 / 20 at 0 / 4000 / 6500, 12 a gun; uzi 25 / 25 / 10, 40 a gun, spread 1° → 5°; shotgun 7 pellets x 50 / 50 / 40 in 15°, 6 shells; rifle 27 / 27 / 21, 30; tranquilizer dose 500 | [O-data] † weapon stats block S (fire 0x6C9E50); `CombatReal.WeaponData` checks ours against the DRMs | `Combat.cpp` `weaponDefaults` / `loadWeapon` | 0 | unmeasured (same data) |
| M31 | Refire, reload, draw times | Clip lengths: pistols fire 10 keys, equip / unequip / reload 6 / 19 / 21; shotgun 13 + pump 25, reload 117 | [O-data] † clips; the frame a shot leaves is [I] | `Combat.cpp` (clip lengths) | — | unmeasured |
| M31 | Reserve ammunition | uzi 400 / 240 / 160, shotgun 90 / 72 / 42, speargun 40 / 25 / 15 by tailoring level | [O-data] † Lara data +0x868 / +0x87C / +0x872 (0x6EEAE0) | `Combat.cpp`, `Tailoring` | which column is "normal" [I] | unmeasured |
| M28 | Manual aim | Free reticle; input action 25; chart decoded | [O-data] † docs/GAMEPLAY.md "Manual aim, adrenaline inputs" | `Aim.cpp`; self-test `manual_aim` | — | unmeasured |
| M32 | One-handed pistols while hanging | Clip sets `combat_pistol_ledgeb_*`, `_hpole_*` | [O-data] † | `Combat.cpp` `oneHand` | — | unmeasured |
| M33 | Tranquilizer | Dose 500 a dart against the animal's health; sleep clips | [O-data] † ; sleep length [I] 20 s | `Combat.cpp` + `ai/Enemy.cpp` | — | unmeasured |
| M34 | Sticky grenades | Carried 4 / 6 / 10, cooldown 20 frames, blast 1000 at the centre → 200 at **1100**, fuse 120 frames once stuck | [O-data] † g_grenade_thrower, pr_grenade records; arc and fuse meaning [I] | `Gadgets.cpp` `loadGrenadeTuning` | — | unmeasured |
| M35 | Adrenaline head shot | Meter max 500, drain 20 / 225 per s, mode scale 0.5, head-shot scale 0.2, ramp 10 frames | [O-data] † Lara data +0x2C8..+0x31C | `Combat.cpp` adrenaline; self-test `adrenaline` | fill rule and window [I] | unmeasured |
| M36 | Scripted slow motion | World time scale by script natives 0x776030 / 0x78C4E0, Lara at real time (0x622418) | [O] † decomp + scripts | `Script.cpp` → `Combat.cpp` time scale | ease shape [I] | unmeasured |
| M37 | Enemy vault | Chart decoded on a branch; † not traced | [O-data] † (docs/GAMEPLAY.md "Close combat") | `Melee.cpp` (enemy vault) | — | unmeasured |
| M38 | Melee | Kick states S1424 / S1432, damage per the chart table | [O-data] † docs/GAMEPLAY.md "Close combat" | `Melee.cpp`; self-test `melee_kick` | — | unmeasured |
| M39 | Finishing a downed thrall | Thralls only go down to bullets, kicks and explosions; regenerate fields of the death record | [O] † 0x67F040 / 0x620380 / 0x613B40 | `ai/Enemy.cpp` `getsUp` | — | unmeasured |
| M40..M46, M82..M84 | Enemies (bats, nagas, spiders, cats, poachers, thralls, mercenaries, sharks) | Per type: senses record (ranges, cones, lose-track 2528), health block by difficulty, attack records, behaviour tree by enemy class | [O-data] † enemy data ObjectBase +0xC8 (0x63C190 and getters 0x614400..0x614940) | `ai/Enemy.cpp`, `ai/EnemyData.cpp` read the same blocks; movement speeds = clip root motion | attack timing and tactics partly [I] | unmeasured: no enemy was timed under Wine |
| M81 | Mjolnir | Bolt, swing, ground pound rings from the weapon data | [O-data] † `CombatReal.GroundPoundRings` | `Combat.cpp` | — | unmeasured |
| M88 | Natla | † not decoded | [U] | none | — | unmeasured (not implemented) |
| M92 | Breakable crates / fuel tanks | Breakable records, triggers 17 / 18 / 19 | [O-data] † | `Objects.cpp` breakables | contents, blast [U] | unmeasured |
| M111 | Health | Max 1000 x 1.3 / 1.0 / 0.8; invulnerable 30 frames after a hit; regeneration below 25 %, 45 frames after the last damage, over 400 frames; health pack heals 640, cooldown 60 frames | [O] † 0x6F5030, 0x6F5160, 0x6E79D0, Lara data +0x44..+0x84, g_healthpack | `Combat.cpp` health | 0 | unmeasured (same data) |

## Objects, puzzles, hazards (goal §9, §10)

Again no row here has a live measurement of the original except where a file is named: the mechanisms run from the
levels' own scripts and object records in both games, and what is ours ([I]) is the motion between the records'
states.

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M51 | Portable poles | Pole seated at the receptacle's marker; engagement records kind 4 / 5 / 8 | [O-runtime] l5_valaskjalf 7190 (attach pose, docs/audit/AUDIT_B.md §9); [O-data] records | `Objects.cpp` `insertHeld`, `removeInserted` | slide 300 units/s [I] | matches (attach pose); motion unmeasured |
| M52 | Pole lever by hanging weight | Door angle = 2 x the lever's slide (l4_ballcourt 6502 → 6505) | [O-data] † uber joint link (docs/formats/PHYSICS.md "The ballcourt door") | `Objects.cpp` uber joints; slide 0..−600 → 0..40° [I] | — | unmeasured |
| M53 | Carried items | Engagement records (shape, cone, required ids); Crouch puts down (input 101), Grenade key throws (102) | [O-data] † generalbank DTP 0x2F, records | `Objects.cpp` `carryPortables`; self-test `carry_throw` | carry pose and set-down physics [I] | unmeasured |
| M54 | Throwing a carried object | Input action 102; † arc not decoded | [O-data] † | `Melee.cpp` (carry and throw); self-test `carry_throw` | — | unmeasured |
| M55 | Pressure pads | Weight ranges in the plates' script bytecode | [O-data] † | `Objects.cpp` plates (Jolt contacts); motor / hold numbers [I] | — | unmeasured |
| M56 | Push / pull blocks | PushPull proxy body 0x7358E0; grab clips | [O] † decomp; block ledge [O-runtime] (0x5D0610 under Wine) | `Objects.cpp` push: 120 units/s [I] | speed not from the original | unmeasured |
| M57 | Objects on a track | † not traced | [U] | uber joints (prismatic) | — | unmeasured |
| M58 | Switches and levers | Script handlers; Lara's clip from the switch's DRM | [O-data] † | `Objects.cpp` `useSwitchClip` | stand point [I] | unmeasured |
| M59 | Push handles round a hub | Uber joint + links | [O-data] † | `Objects.cpp` push handles | rate [I] | unmeasured |
| M60 | Thor's strength objects | Uber objects with player sensors (script vt 14) | [O-data] † | none at 89d3e82 (player sensors only) | — | unmeasured |
| M61 | Light-beam puzzle | † beam logic not decoded | [U] | receptacles only | — | unmeasured |
| M62 | Timed doors | Script timers (seconds in the bytecode) | [O-data] † ; no door timed under Wine | `script/Vm.cpp` timers | — | unmeasured |
| M63 | Breakable urns | Breakable records; contact triggers 6 / 7 / 8 | [O-data] † | `Objects.cpp` breakables, reward births | — | unmeasured |
| M71 | Objects that carry Lara | Movers on uber / spline records | [O-data] † | `Objects.cpp` movers | — | unmeasured |
| M87 | Timed multi-switch | † | [U] | none | — | unmeasured (not implemented) |
| M89 | Mjolnir power nodes | Script stimulus → uber state | [O-data] † | `Script.cpp` / `Objects.cpp` | — | unmeasured |
| M101 | Shooting a mechanism part | `uberShot` on the hit section | [O-data] † | `World::uberShot`, `Aim.cpp`; self-test `kraken_kill` | — | unmeasured |
| M102 | Automatic doors | Script volume handlers | [O-data] † | `Script.cpp` | — | unmeasured |
| M25 | Grapple pull | Pull allowed with the rope longer than **395.07** (chart cond c2e4, 0x72D720) | [O] † decomp + player data +0x8A0 | `Grapple.cpp` pull: 1 s heave, block follows at 240 units/s [I] | timing [I] | unmeasured |
| M26 | Hanging weight on the rope | Rope body mass **55** | [O-data] † player data +0x8D8 | pull handler only; the rope loads no joint | — | unmeasured |
| M27 | Rope snagged round an object | † | [U] | none | — | unmeasured (not implemented) |
| M67 | Crushers | Contact force native (larabase 6) | [O-data] † | `Script.cpp` larabase 6 contact force, geometric stand-in [I] | — | unmeasured |
| M68, M69, M77, M79 | Flame jets, spear rows / blades, steam vents, rollers | † not decoded | [U] | none or partial (see docs/GAMEPLAY_AUDIT.md) | — | unmeasured |
| M70 | Break-away holds, collapsing floors | Class-30 breakables | [O-data] † | none at 89d3e82 | — | unmeasured |
| M103 | Fire / script death volumes | Script health natives | [O-data] † ; [O-runtime] the prologue hall kills a Lara teleported onto the balcony without the health lock (`gameplaygoal/grapin/wine2.txt`, `gameplaygoal/reference/grapple-wine.txt`: the player instance is gone at once and she restarts at the bedroom) | `Script.cpp` death volumes | — | unmeasured (which volume or script killed her is not identified) |
| M104 | Jellyfish | Sting on touch | [O-data] † | `World.cpp` | — | unmeasured |
| M105 | Kraken tentacle swats | † damage unknown | [U] | level script objects | — | unmeasured |
| M106 | Ship roll: containers, barrels | † | [U] | none | — | unmeasured (not implemented) |

## Motorcycle (goal §11)

No Wine trace of the bike exists. The original side of every row is the default tuning record of motorcycle.drm and
the decompiled update 0x793680 / 0x791D10 / 0x792900 (†); ours feeds the same record into a Jolt vehicle.

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| M47 | Top speed, acceleration | **40 mph = 5008 units/s** (280 units per metre); engine 30 / 20 / 20 N·m at 1000 / 10 000 / 18 000 rpm, ratio 37.4, one gear | [O-data] † record +0x00, +0x54, +0x78..+0x98; 0x791D10 / 0x78F860 | `Vehicle.cpp` `driveBike`, `Physics` engine curve; replay `bike_ride` reaches ~4000 units/s down the trail | 0-to-top time never compared | unmeasured |
| M47 | Braking | Front 5 / rear 10 units/frame² per mass share, ramping in over 1.25 s | [O-data] † wheel +0x0C, record +0x30 | 783 / 1466 N·m [I units] | — | unmeasured |
| M47 | Steering | Front wheel 30° → 10° and 95°/s → 75°/s between 0.3 and 1.0 of top speed | [O] † record +0x10..+0x1C, +0x1B4; 0x793680 | `Vehicle.cpp`, the same blend | 0 | unmeasured (same data); turning radius measured in neither game |
| M47 | Suspension | 2.7 Hz, damping ratio 0.35, forces 0.7 of the travel above the contact | [O] † record +0xA4 / +0xA8, wheel +0x18 / +0x28 | Jolt wheel springs; one force point [I] | — | unmeasured |
| M47 | Mount / dismount | Clips `motorcycle_mount_cycle` / `_dismount_cycle` (root offsets 180.1, −124.9 / 158.3, 52.1) | [O-data] † | E within 250, off below 200 units/s [I] | — | unmeasured |
| M48 | Jumps, air control | In the air: 300 x f N·m about up and 2600 x f N sideways, f = steer x v / top speed; bike fall damage from **3792.67** of drop, lethal at **4977.88** | [O] † record +0x44 / +0x48, +0x184..+0x198 | `Vehicle.cpp` air control, fall damage; hop +1100 units/s [I: not in the record] | the hop may not exist in the original | unmeasured |
| M48 | Launch boost | Burnout: rear wheel to 52 rad/s in 0.2 s; launch wheelie bit clear in the data | [O-data] † record +0x52, +0x34, +0x144 | `BikeStunt::Burnout` | — | unmeasured |
| M49 | Donut, power slide, ramming | Donut 6750 N on the rear contact; power slide above 15 mph: 1524 N out / 1016 N back for 5 + 10 + 5 frames, lean 15° | [O] † record +0xE0..+0x1F4; 0x790920 / 0x7918B0 | `Vehicle.cpp` stunts; spin cap 2.5 rad/s, ram damage [I] | — | unmeasured |
| M50 | Headlight, breaking jars, weight on a pad | † not decoded | [U] | none | — | unmeasured (not implemented) |

## Systems, camera (goal §13, §14; for completeness of the 111 mechanics)

| ID | Mechanic | Original: behaviour and numbers | Evidence (class, source) | Ours (code, value) | Difference | Status |
|---|---|---|---|---|---|---|
| — | Follow camera | Orbit distance **1264.22** (hanging / ladders 1896.3), rest pitch −0.0225 rad, FOV 45° | [O-runtime] CameraManager 0x5E8210 hooked under Wine for modes 1 / 4 / 11 / 19 / 20 (`camfollow-wine/`, `camdump/`); camera DTP 0x14 | `Camera.cpp`, the same mode records | per-mode values equal; blends [I] | matches (distances, modes); transitions unmeasured |
| M64 | Pickups | Reward table: 179 treasures and 6 relics; a relic raises max health | [O-data] † reward records of every unit | `Progress.cpp` reward table | 0 | unmeasured (same data) |
| M72 | Cutscenes, reveal cameras | Cine streams and the units' camera splines | [O-data] † | `src/cine`, `Camera.cpp` | blends [I] | unmeasured |
| M73 | Checkpoints, saves | Checkpoint intros; respawn after a 90-frame fade | [O] † 0x75AA50 / 0x75AF90 / 0x6F55A0 | `Stream.cpp`, `save/Save.cpp` | — | unmeasured |
| M74 | Burning-manor escape | The section-01 units and scripts | [O-data] † | `Stream.cpp` | — | unmeasured |
| M75 | Outfit / weapon choice | Selection rule 0x5DE2B0 | [O] † | `ui/Ui.cpp` (no per-level screen) | screen missing | unmeasured |
| M93 | Ending flow | † rules inferred | [I] | `ui/Ui.cpp`, `Progress.cpp` | — | unmeasured |
| M107 | Utility light | Spot reach 5000 / omni 1800; toggle cooldown 4 frames; no battery | [O-data] † g_personal_light | `Gadgets.cpp` | placement [I] | unmeasured (same data) |
| M108 | Field camera | † | [I] | `Camera.cpp` | — | unmeasured |
| M109 | PDA | Menus as captured (`screens/ui`) | [O-runtime] screenshots | `ui/Ui.cpp` | layout compared by eye only | unmeasured |
| M110 | Training text / helper buttons | Tutorial natives, locals.bin strings | [O-data] † | `Script.cpp` → `src/ui` | — | unmeasured |

## Differences to fix

Measured differences beyond the tolerance, with the area that owns the code. None of them is fixed by this document's
pass except where it says so.

| # | Difference | Evidence | Where in ours | Owner area |
|---|---|---|---|---|
| 1 | **Fixed** (branch `agent/GpJumpTiming`): jump take-off delay. tru.exe leaves the ground 342..359 ms after the press standing, 273..290 ms with the stick (moving 128 along the ground meanwhile), 70..109 ms running; ours was 133 / 67 (standing still) / 33 ms, now 333 / 267 (127.9 along the ground) / 67 ms | `screens/stairs/j_stand.txt`, `j_57.txt`, `j_run.txt`, `jumpfid/*.txt`, `gameplaygoal/reference/chimney-wine2.txt` against `gameplaygoal/jumptiming/out/ours_flat.log` | Cause: tru.exe 0x4428A0 -> 0x441FB0 starts a request's clip at (start − blend) frames, ours started it at the start frame. `World.cpp` `clipBlend` applies the lead to the take-off, run start and wall jump clips; `Player.cpp` JumpPrep chooses the take-off at the press and gives the stick jump its 8 frames under the run start's motion. Open: every other clip still starts at its +0x1A frame (the same rule should hold: each needs its own measurement and its replays re-recorded); what S245's request 213 plays | Lara controller (`Player.cpp`), animation requests (`World.cpp`) |
| 2 | **Fixed** (same change): run start led tru.exe's by 0.07..0.10 s; now by the key's latency only (0.037..0.041 s from the key, her run state beginning 37..41 ms after it) | `jumpfid/hold_run.txt`, `sprint.txt` against `gameplaygoal/jumptiming/out/ours_flat.log` | `clipBlend`: `ng_run_start` from frame 6 − 4 = 2 | same |
| 3 | Running jump at a ledge: straight rush / re-aimed arc switch 20..70 farther out | `runjump2/rj*.txt` (614..649 against 634..719) | `Player::aimGroundJump` | Lara controller |
| 4 | **Slide trigger**: the original slides on faces flagged 0x0004 (seen at normal z 0.523..0.753) and stands on unflagged ones down to 0.626; ours decides by angle (0.3 ≤ normal z < 0.7) | `gameplaygoal/charvirtual/slope-wine2.txt`, docs/formats/PHYSICS.md "Walkable slope limit under Wine" | `Player::standable`, `Tuning::minFloorNormalZ` / `slideMinNormalZ`; the client flag is already loaded (`CollisionMesh::Face::clientFlags`) | character controller / collision |
| 5 | Pole fling onto a platform lands 79 past its edge; tru.exe touches down 3 short and steps on | `polefid2/mhsw.txt` | `Player::poleFling` (ledge with a floor on top) | Lara controller |
| 6 | Safety drop while running: no 82-unit overshoot and take-back | `gameplaygoal/inputrules/wine2.txt` | `Player::trySafetyDrop` | Lara controller |
| 7 | Surface swim: velocity lags the heading ~9° in a turn against ~16° | `camfollow-wine/turn1.txt` | `Player.cpp` SwimSurface (`kSwimTurnAccel`) | water |
| 8 | **Fixed** (branch `agent/GpJumpTiming`): the draw onto the wall took 2 frames (tru.exe 6) and the turn before the rebound 4 (tru.exe 6); a press during the draw was taken (tru.exe: ignored). Now 6 / 6 frames, the press from the idle | `gameplaygoal/reference/chimney3-wine.txt` j3 / j4, `chimney4-wine.txt` h1 | `WallJump.cpp` `kAttachSeconds`, `clipBlend`'s lead for `wall_jump_attach_air` (4 / 1) and `wall_jump_jump_back` (2 / 1): these keep a negative lead (held at their first frame), the ground take-offs do not (0x4428A0's fresh-crossfade branch); which branch a request takes is [U] | Lara controller (`WallJump.cpp`) |
| 9 | **Fixed** (branch `agent/GpJumpTiming`): a jump pressed within 2 frames of the run's start is the stick jump (tru.exe: between 15 and 71 ms) [I: the frame count] | `chimney3-wine.txt` j3 / j4 / h3 | `Player.cpp` (`Tuning::stickJumpLeadFrames`) | Lara controller |
| 10 | **Fixed within the tolerance** (branch `agent/GpJumpTiming`): chimney rebound rose 435 in ours against 338..361; now 360.6 / 360.6 against 360.6 / 338.4. tru.exe's rays are 3 x the launch speed long (0x758DD0, +0xA20), not 3 x the current speed, and the test runs every game frame. Open: tru.exe takes the walls from 24..35 nearer than these rays reach from over her root, and holds 119..131 out of the faces where ours holds 110 (the third hold ends 30 high) | `gameplaygoal/reference/chimney4-wine.txt` h1 against `GameReal.JunglehubChimneyJumpClimbsColumns` | `WallJump.cpp` `tryWallJump`: what 0x4935F0 returns as the hit (a radius?) and where segment 1 is in the blended flight pose | Lara controller (`WallJump.cpp`) |

## Unmeasured: what still lacks a live measurement, and the session that would give it

Each line is one Wine session (harness of "Method"; `gameplaygoal/reference/session4.sh` is the newest working
template: unit offset from the start position, `tpv` placement checked by a position dump, `CAMFOLLOW`, `god`).

| What | Rows | Session |
|---|---|---|
| Ground turn rates, run stop distance, walk start / stop | M94 | l3_main_bedroom corridor: Forward 2 s then release (stop distance); Forward then the stick swung 90° / 180° with the camera pinned (`CAMFOLLOW`), heading (+0x38) against time |
| Crouch / crawl speed, roll length | M15 | same corridor: Crouch + Forward 3 s; Forward 1.5 s then Crouch |
| Jumps at a ledge with Jump held; mid-air stick | M13, M91 | `runjump2/rj2.sh` at l4_temple ledge 1204 from 1100 short, the key held 1 s against a tap; the stick reversed 0.2 s into a flat running jump |
| Slide jump, slide on a second slope | M17 | l3_main_bedroom roof (`screens/stairs/slide1.txt` spot): Jump 0.3 s into the slide; a 30° flagged ramp of l2_junglehub (`gameplaygoal/charvirtual/slope-points.txt`) for the acceleration fit over > 0.5 s |
| Shimmy, corner, pull-up, hang-jump flight times | M01, M02 | l4_temple ledge 1155 (hang reached by `gameplaygoal/inputrules/session2.sh` "ontop"): Left held 3 s; Jump + Left to ledge 1157; Jump alone |
| Saving grab recovered by Interact | M04 | `inputrules/session2.sh` "air 1350" with E pressed 0.3 s after the catch (the earlier press came after the fall) |
| Free-climb move rates, ladder climb rate, pillar climb | M05, M96, M12 | l2_cliffclimb hold column (`runjump2/dice.sh`), l1_ship_sinking_hall ladder 234, l4_traprooms pillar: Forward held 3 s each |
| Beam walk, balance loss, fall and catch | M10 | l3_chapel beam 1031: Forward 3 s; Forward + Left held until she falls (time to about-to-fall, to the fall; the side; the ledge catch) |
| Pole end, pole to collinear pole, fling without a target | M07, M09 | l4_traprooms pole 1337 (no pole beyond): fling at 4 swing phases; hand over hand to the end |
| Perch and wall-hug jumps | M11, M80 | l4_temple perches (`GameReal.TemplePerchTurnsAndPillarTop` spots); l2_puzzleroom shelf |
| Swim start / stop curve, under-water turn and pitch rates, breath time, dive depth | M29, M85 | l2_cliffclimb sea (`scratch/swimsprint` spots): the stick swung 90° under water; stay under until the first damage tick (133.3 s expected) |
| Every combat timing | M28..M46, M81..M84, M111 | l4_pool panther (`--target` spot of `CombatReal.Pool*`): fire held 5 s per weapon (shots counted from the ammo word at fadb58 + 0x51C + 2 x slot), reload, draw; damage from the enemy's health (comp +0x2C64); regeneration after one bite |
| Enemy speeds and attack periods | M40..M46, M82..M84 | one unit per type with the enemy's instance position traced (`pole watch <uid>`) |
| Push block speed, lever / door times, pressure pad thresholds, timed doors | M52..M62 | l4_ballcourt block and lever 6502 / door 6505 (`pole watch` of both while Lara pushes / hangs); l1_baldrs_tomb plates |
| Grapple pull heave, rope weight on a lever | M25, M26 | l4_trailstart lever 6695: Q, E held; the lever's joint traced |
| **Grapple swing**: rope length at the hang, period, decay with no input, growth and limit with Forward held, jump-off speed, rope climb rates | M20, M99 | One unit per session (`session7a.sh` is the template; ours: `ours_chapel.json`). Either l3_chapel after its opening sequence has ended (trace her state until it is the idle state, then place her on the column top at ours −5126, 1900, −3141) or l4_temple ring 4951 from the upper ledge (placement works in l4_temple: `gameplaygoal/inputrules/session2.sh`). Pin the camera for the Q press (`CAMFOLLOW`), check the placement by a position dump, keep `god` and the fall-damage heights (player data +0x4C..+0x54) out of reach: a death restarts the chapter in another frame |
| Chimney: the look-ahead that takes the next wall; a climb that needs all five | M06 | l5_valaskjalf ledge 1512 alone in a session (`session7b.sh` `hang`, hang root ours 5057, −79806, −13922.8, yaw π, `CAMFOLLOW=0,1`): Back + Jump, then a press every 150 ms |
| Wall run, rappel rates, wall kick | M22..M24 | l5_valaskjalf anchor 6750 from ledge 1600 (hang placement as session4.sh `hang`) |
| Motorcycle: 0 to top speed, braking distance, steering radius, a ramp jump | M47..M49 | l4_trailstart: mount by E beside bike 5482 (38051, −51966, 1463), throttle 8 s on the trail's straight; the bike instance's position (+0x20) and velocity traced; hand brake at speed; the stick held over at 2000 units/s |
| Camera transitions | — | hooks on 0x5E8210 as `camfollow-wine/` while she grabs a ledge and drops |
| Hazards | M65..M70, M77, M79, M103..M106 | per hazard, Lara held in it with the health word (fadb58 + 0x628) traced and no `god` |

## Counts

161 rows: **53** match (5 of them with a part that differs), **1** differs, **107** are unmeasured. 58 rows rest on a live measurement of the
original ([O-runtime]); the rest are decoded only (†).
