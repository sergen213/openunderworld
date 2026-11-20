# Puzzle blockers: decoded causes and minimal fixes

Static reverse engineering of the parked puzzle scenarios (`tests/selftests/known_gaps/`) and the C-JOINT / C-VM puzzle
entries of [docs/audit/BUGS.md](../audit/BUGS.md) (OU-PZL-006/007/008/009/010/015, OU-GRP-005, OU-PHY-001). Binary:
GOG `tru.exe` 1.1 (SHA-256 `1fdbd345…1011`). Labels: **[O]** observed (objdump address, bytecode dump, data dump,
harness output, `path:line`), **[I]** inference with confidence, **[U]** unknown. Source citations are `HEAD` at
84ae803. No engine code was changed for this document.

## Summary

| Blocker | Decoded cause | Confidence | Minimal fix | In-flight ScriptVm work covers it? |
|---|---|---|---|---|
| OU-PZL-007 ch13 calendar handle (`ch13_ballcourt_calendar_solve`) | The scenario grips the wrong record: calendar_b's section 0 lever (record 540), locked in tru.exe's data too. The jaguar handle is section 1's record 528, and ours turns the ring with it | high (harness-verified) | Scenario only: grip section 1's record; probe the sign of joint 2. Optional: body-A / link propagation for the section-0 lever | no |
| OU-PZL-009 ch15 exit switch (`ch15_ballcourt_exit_switch`) | `modifiable` 7 (post 0x2E: enter the anim-graph state by id) is honoured only for `ar_ei_switch_oneway`; 5469's constructor calls 7(2) | high | Honour 7 on every switch. CriticalGates' gate 498 then needs the "Kraken dead" broadcast first | no. **Fixed** on agent/ScriptPuzzle (`BallcourtExitSwitchLockedUntilTheKey`; scenario out of known_gaps; `kraken_kill` takes the gear switches' reports as `call` fixtures) |
| OU-PZL-010 ch15 rune block (`ch15_pool_rune_block`) | `ca_pu_thor_debris` has no joint record: one free body. `pushUberHandle` moves only jointed sections | high | A gripped jointless section takes the same impulse-bounded push velocity as a type-7 body | no |
| OU-GRP-005 ch12 lever gate (`ch12_trailstart_lever_gate`) | The post's return works in ours (harness). What never runs is the detach: vt 46 "attached" is not called, `larabase` 26 is unbound, and the 1 s heave lets go of the rope | high | Call vt 46 on attach (instance prototype 64), bind `larabase` 26, keep this target roped until she lets go | no (`larabase` 26 is not in the list). **Fixed** on agent/ScriptPuzzle (`GrappleLeverReturnsWhenReleased`; scenario out of known_gaps) |
| OU-PZL-008 ch13 statue head (`ch13_temple_block_topple`) | (a) the sight ray ended at the post's origin, 4 under the top of its floor (tru.exe: at the feature, query 0xDE); (b) nothing in ours drags a rigid body that the rope bends round (tru.exe's bends are physics bodies, 0x699A50); (c) `multibody` 92 / 93 are unbound | (a) high; (b) high; (c) high | (a) end the ray at the feature; (b) bend bodies on Jolt; bind 92 / 93 | no. (c) **bound** on agent/ScriptPuzzle (`TempleFloorBreaksByMultibody92`); (a) **fixed** on agent/Joint8 (`TempleRingOfferedFromTheEastLedge`); (b) **fixed** on agent/Rope11 (`TempleHeadToppledByTheRopeBend`, scenario moved up) |
| OU-PZL-006 freed slabs (`ch09_shrine_shaft_descent`) | Fixed joints (type 8) are never created, so action 10 on them frees nothing | high | Track fixed joints; action 10 on one frees its body B | no |
| OU-PZL-015 ch08 `multibody` 3 | `multibody` 3 = make every body fixed again (bit 0 of physics +0x15C, timer +0x160 = 0); ours leaves it unbound | high (semantics) | Bind 3 as the inverse of 2 (Enable) | no. **Fixed** on agent/ScriptPuzzle (`MaproomMultibodyDisableFixesTheBodies`: l2_maproom planes 2637 / 2638 call it, not a ch08 object) |
| OU-PHY-001 joint types 5 / 7 on moving bodies | Type 7: no record has a moving body A. Type 5: only the shiva scale pans, which nothing watches | high (survey) | None needed for progression. Body-A motion: **done** on agent/Joint8 (poses follow body A; fixed riders follow a freed body A) | n/a |

None of these scripts calls a native from the in-flight ScriptVm list (CALLU builtins, `instanceref` 10, `instance` 27,
`unitref` 1, `gamebase` 27, `cinematic` 10) [O: natives in the dumps below: `instanceref` 7, `stickybool` 0/2,
`stickyint` 0/2, `modifiable` 4/7, `checkpointbase` 3, `larabase` 26/35, `instance` 6/14/21/28/61, `unitref` 3,
`markup` 0, `multibody` 2/4/92/93, `uberobjectbase` 0/1/2, `uberobjectevent` 0, `playersensor` 2, `triggerplanebase` 0,
`sound`, `padshock`, `messagechannel` 2, `globalsref` 0, `corebase` 3/7, `uisavemenu` 5]. Landing that work therefore
unblocks none of these scenarios.

## Method and reproduction

- Script bytecode: `OU_PROGRESSION_DUMP="<unit>:<uid,...>"` (or `<unit>:*` for every intro and type)
  `build-AuditBaseline/ou_gates_tests --gtest_filter=ProgressionReal.DumpScripts`.
- Uber data: `OU_UBER_SECTIONS=<object> … --gtest_filter=ProgressionReal.UberSections`; live states / positions
  `OU_UBER_DUMP=<unit>:<uid> … ProgressionReal.DumpUber`; switch graphs `OU_ANIMGRAPH_RECORDS=<object> …
  ProgressionReal.AnimGraphRecords`; script fields after one tick `OU_OBJ_FIELDS=<unit>:<uid> … ProgressionReal.ObjectFields`.
- Harnesses (scratch, not in the repo: `openunderworld-work/w2/PuzzleRe/h/`): `calendar.cpp`, `lever.cpp`,
  `ring.cpp`, `jsurvey.cpp`. Each links the `build-AuditBaseline` libraries and uses helpers copied from
  `tests/test_gates.cpp` (`helpers.hpp`). Build each against `git archive HEAD src` headers (the working tree's
  `Game.hpp` was being edited and no longer matched the libraries):
  `c++ -std=c++23 -O1 -flto=auto -Iinc/src -I<build>/gen X.cpp $(cat link.txt) -o X`.
- tru.exe: `objdump -d -M intel` on the addresses quoted; Ghidra export `openunderworld-work/ghidra/exports/decomp_all.c`
  was used for orientation only, and every claim below rests on the disassembly.

---

## OU-PZL-007: ch13 calendar_b push handle turns nothing

Scenario `known_gaps/ch13_ballcourt_calendar_solve`, step `jaguar_handle`: `go 6301 11287 80 walk`, `engage 5477`,
`push 1 0 4`, expecting `object.5477.uberjoint.2` over 0.2.

### Decoded cause (high)

The step grips calendar_b's **section 0 lever** (record 540). That lever is locked in the data. The jaguar ring is
turned by **section 1's records 528**, which sit at the jaguar head. Ours already turns the ring with those.

- [O data, `OU_UBER_SECTIONS=ca_uo_calendar_b`] Joints: j0 prismatic (A section 2, B section 0); j1 prismatic (world →
  section 6); **j2 hinge (A section 6 → B section 5, the jaguar ring)**; j3 / j4 hinges (world → sections 2 / 3); j6
  fixed (5 → 1, the handle bar on the ring); j7 link j3 = 0.15·j1; j8 link j4 = 0.667·j2.
- [O data] Section 0 (model 4, default state **4**): its only transition from 4 is `4 → 2` (trigger 3, immediate).
  State 2's sole action is `0x1D` → engagement set 540 (kind 1, segment 11). There is **no transition out of
  state 2** and no trigger-20 transition anywhere in section 0's state 2. Section 5 state 0 limits j0 to `0..0`, on
  (param 11). In calendar 6506 the same section starts in state 0 and reaches state 2 only after the headdress insert,
  via state 3 (j0 limit −379..31.6, motor 316 / 1264). calendar_b skips that.
- [O data] Section 1 (model 5, records 528 on markers 10 / 20 near the model origin) goes `0 → 3` when section 5 is in
  state 4 (the mask insert). State 3 + trigger 20 / kind 1 → state 1 (motor brake on j1). Its handle is fixed to the
  ring (j6), which turns on j2. j2's limit is lifted by section 5 state 4 (param 13, flag 0) after the insert.
- [O `DumpUber`] 5477 stands at (4064, 11214, 3613). Section 0's model-4 hull is at local x ≈ −2110 … −2180, so it
  lies ≈2237 east of the origin. That is the scenario's (6301, 11287).
- [O harness `calendar.cpp`] After the real insert (mask 6499 carried and inserted by Lara; section states `2 3 0 3 0 5
  3 1 1 1`):
  - section 1's nearest kind-1 spot is (3975, 10908), yaw −135°, 2201 from the scenario's spot. Gripping it moves
    section 1 3 → 1. Four seconds of `move {0, 1}` turn **j2 0 → −0.646 rad** and j4 0 → −0.430 (the 0.667 link).
  - section 0's nearest spot is 19 units from (6301, 11287). Interact there changes no state and moves no joint.
- [O] Ours chooses the joint to move as the first live joint with body B = section, following fixed joints by data
  (`src/game/Objects.cpp:964` `uberSectionJoint`). For section 0 that is j0: prismatic and limit-locked, so nothing
  moves (`Objects.cpp:1039` `pushUberHandle`).

### tru.exe evidence

- 0x701310 posts 0x158 on PushPull entry. 0x6D5AC0 case 0x158 runs 0x6D4790: the transition with trigger 20 and the
  record's kind ([O] docs/GAMEPLAY.md "Push handles", already decoded). With no such transition in section 0's
  state 2, the post changes nothing.
- [I medium] Havok would still transmit Lara's proxy impulse through the locked j0 into section 2 (j3 hinge to the
  world), and through link j7 into j1 (section 6's slide, limited −3.2..214.9 once section 6 is in state 3). So in
  tru.exe the side lever may turn section 2. Ours cannot do that: it moves only the first joint
  (`uberSectionJoint`), and a hinge with a moving body A is posed as if A stood (`Objects.cpp:834` comment). This is
  not needed for the ring turn.

### Puzzle logic (for the follow-up steps) [O data]

Section 9 is a counter (states 0..3) driven by commands 0..5 (trigger 16, params 0..5). Commands 4 / 0 / 2 advance it
and commands 5 / 3 / 1 move it back. The senders, via action 11 = command:

| Section | Watches | In range → advance | Out of range → back |
|---|---|---|---|
| 2 | j3 in −3..5° | command 4 | command 5 |
| 3 | j4 in 148..153° | command 0 | command 3 |
| 5 | j2 in 85..95° | command 2 | command 1 |

Section 9 reaching state 3 fires event 0, which runs the script's "Solved!", then vt 55 ("ready",
`ca_ani_temple_reveal90001.centerBSolved`).

### Minimal fix

- Scenario: change `jaguar_handle` to stand at section 1's record. Use `uberEngageSpots(…, 5477, 1, kind 1)`, e.g.
  (3975, 10908) yaw −135°.
- Probe: the push turns j2 negative. Replace `"over": "max", "min": 0.2` with an absolute value (or `max` −0.2 for
  this push direction).
- No engine change is needed for the ring. Body-A / link propagation (the section-0 lever driving j3 / j1) belongs to
  OU-PHY-001 and should be decided by a Wine trace.

### Proving test

- `ProgressionReal.CalendarBJaguarHandleTurnsTheRing` (new): `receptacleWorld("l4_ballcourt")`, carry 6499 into 5477
  section 5, grip section 1's kind-1 spot, push 4 s, expect `|uberJoint(5477, 2)->x| > 0.2` and section 1 → 1 → 5 → 3.
  This is the harness turned into a test.
- Then `known_gaps/ch13_ballcourt_calendar_solve` moves up to `tests/selftests/`.

---

## OU-PZL-009: ch15 exit switch usable without the key

Scenario `known_gaps/ch15_ballcourt_exit_switch`, step `switch_without_the_key`: reveal 5464 reaches graph state 3.

### Decoded cause (high)

- [O bytecode, `OU_PROGRESSION_DUMP=l4_ballcourt:5469,6501,5464`] Switch 5469 (`ca_ei_switch_wall_autoreset`,
  script 13914) has a state-0 constructor (fn 1) that reads sticky +36 (7714). When the sticky is false it logs
  "Disabling xibablba exit switch" and calls vt 61 (type 3637 fn 9): `PUSHS32 24; PUSHSA 4; CALLU modifiable fn 7`, i.e.
  `modifiable` 7 with member +24 = **2**. Keyhole 6501 vt 20 with argument 1 calls 5469 vt 62: set sticky +36, then
  vt 60 → `modifiable` 7 with +16 = **1**.
- [O data, `AnimGraphRecords`] The autoreset graph: state id 1 (off) —use→ id 0 (on); id 0 —timer 60 frames→ id 1;
  id 2 —use→ id 2 (a failed try; the locked state).
- [O `ObjectFields` l4_ballcourt:5469] After the first tick the switch's modifiable member +4 = **1**: the
  constructor's 7(2) was dropped.
- [O] `src/script/Script.cpp:1761-1768` honours `modifiable` 7 on a `Kind::Switch` only when
  `o->name == "ar_ei_switch_oneway"`.

### tru.exe evidence [O]

- `modifiable` 7 thunk 0x79ABA0: `push [args[0]]; ecx = receiver; call 0x4A5A90`.
- 0x4A5A90: `eax = *this (the member's instance); if (eax) INSTANCE_Post(eax, 0x2E, value)` (`push ecx; push 0x2E;
  push eax; call 0x5B3750`).
- Anim-graph handler 0x6B4440 (attached when instance +0x294 is non-null): message − 0x15 indexes the byte table at
  0x6B49C4 and jumps through 0x6B4998. For 0x2E: index 0x19 → table entry 4 → **0x6B453B**. That code queries 0xE6
  (state list, `call 0x5B36F0`), walks records of 0x28 bytes comparing the s16 id at +2 with the value (sentinel
  +0 = 0x2714), and enters the match (`call 0x6A5240`). The switch therefore enters state id 2 at once and stays
  locked until 7(1).

### Why the name check exists, and what it breaks [I high]

The code comment says honouring 7 everywhere stopped `kraken_kill` and the critical gates "at the first pull". In
l1_kraken_room the only switch put into state 2 by a script is **498** (script 4656, not 30). Its constructor
(state 0 fn 0) sets sticky 7037 and calls `modifiable` 7(+24 = 2). Its vt 0 message handler frees it with 7(+16 = 1)
on the broadcast "Kraken dead, so remove tentacle hazards", which the unit script type 3477 vt 0 sends after "bridge
dropped" [O bytecode]. `ProgressionReal.CriticalGates` uses gate `{l1_kraken_room, 498, Use, {497}}` in a fresh World,
where the kraken is alive (`tests/test_gates.cpp:537`). With 7 honoured, 498 is correctly locked there. Switch 30 is
never locked by a script [O: the only `modifiable` 7 call sites in l1_kraken_room are 30's vt 62 / 63 / 60 (value 1)
and 61 (unused), 55's constructor (1), 61's vt 55 (other member), and 498's two]. Whether `kraken_kill` itself stops
on 498 depends on the broadcast reaching 498's vt 0 [U: not run].

### Minimal fix

- `Script.cpp:1768`: drop the name check. Every `Kind::Switch` with a graph set takes 7 as "enter state by id" (field
  = id; free it when the new state can be used, as the existing branch does).
- `CriticalGates`: before gate 498, deliver the kraken-dead broadcast (or run the unit script's vt 0 with it).
- 6501 → 5469: keyhole vt 20(1) needs the key 5695 to reach l4_ballcourt (the scenario's second half,
  `known_gaps/ch15_ballcourt_exit_key`).

### Proving test

- `known_gaps/ch15_ballcourt_exit_switch` passes: 5464's graph stays 0.
- New `ProgressionReal.BallcourtExitSwitchLockedUntilTheKey`: field +4 = 2 after the first tick; a use runs
  transition 2 (vt 19 with id 2, no vt 55); after `w.script().call(6501, 20, {1})` the field is 1 and a use opens 5464
  (`TempleRevealReopens`).
- Regression: `CriticalGates` (498 after the broadcast), `kraken_kill`, l7_arctic_sea wheel 9103 (existing).

---

## OU-PZL-010: ch15 rune block 5609 is not dragged

Scenario `known_gaps/ch15_pool_rune_block`, step `pull_the_block`.

### Decoded cause (high)

- [O data, `OU_UBER_SECTIONS=ca_pu_thor_debris`] Class 79, **one section** (model 0: one 632-unit box, mass 200), **no
  joint records**. Record 400: kind 1, sphere r 948, needs g_belt. Section states: default 1 → 0 and 2 → 0 (trigger 3),
  whose action 8 sets the instance's hidden draw groups (+0x23C, 0x4D87DB). One command (section 0, type 1, payload 2).
- [O bytecode] Script 14080 / 14158 / 9744 (the Thor base): `playersensor` 2, `uberobjectbase` 1 / 2, `larabase` 35 on
  vt 49. Nothing scripted moves it.
- [O] `pushUberHandle` (`Objects.cpp:1039`) moves a gripped section through `uberSectionJoint3` (three-axis joint) or
  `uberSectionJoint` (1-DOF). With neither it stops at `Objects.cpp:1106` ("gripped, nothing moves").

### tru.exe evidence

- [O, docs/GAMEPLAY.md "Push and pull objects"; 0x74AC00] PushPull puts an impulse-bounded velocity on the
  **object's body**, turned off her facing by the stick, through the proxy body (0x73F390). No joint is consulted.
  With no joint the body is simply a free Havok body: the push slides it.
- [U] Whether an uber section without any joint is dynamic at rest or keyframed until touched (bit 0 of physics
  component +0x15C is the "fixed" flag, set by `multibody` 3 and cleared by `multibody` 2 → 0x4D8FD0) has not been
  traced.

### Minimal fix

In `pushUberHandle`, when the gripped section has no live joint of any kind and is not the removed / freed one,
synthesize a world three-axis joint for it (as `makeUberJoint3` does for type 7: the section's collision and mass, no
limits, no motors, the data's default friction) on the first grip, then run the existing Joint3 push path. That path
already does `pushDrive`, the brake and the strength from g_belt. Save it in `joints3` under a reserved index.

### Proving test

- New `ProgressionReal.ThorDebrisIsDraggedOffItsCorner`: l4_pool with `give g_belt`; grip 5609 from the walkway side;
  stick back 4 s; expect origin y ≥ −5800 (300 out of the corner) and the two treasures reachable.
- `known_gaps/ch15_pool_rune_block` passes.

---

## OU-GRP-005: ch12 lever 6695 stays out after she lets go

Scenario `known_gaps/ch12_trailstart_lever_gate`, step `let_go`.

### Decoded cause (high)

The post's return is not what is missing. Ours simulates it correctly once the script sends command 1. The
"detached" path of the lever script never runs, for three reasons:

1. Ours never calls vt 46 ("attached"), so the script never enters its state 1.
2. `larabase` 26, which that state's loop polls, is unbound.
3. Ours releases the rope after a 1 s heave anyway.

- [O bytecode, `l4_trailstart:6695`] Script 16028 / 16052:
  - vt 46 logs "attached", calls vt 57 (empty) and does `STATE 1`.
  - State 1 (fn 9) loops `while (larabase 26(instance 14(self))) SLEEP;`, then logs "detached" and calls vt 60
    (command 1) and vt 58 (empty).
  - vt 59 sends command 0.
- [O data, `ca_ei_lever_grapple_post`]
  - j0 is the post: prismatic world → section 0, limit 0..1264, record motor −158/s bound 47.4.
  - j1 is the gate: prismatic world → section 1, limit 0..948. The link j2 sets j1 = 0.75·j0. j3 fixes the ring
    (section 2) to the post.
  - Section 0 starts in state 0 and goes `0 → 2` at once: brake, motor 0/s bound 158.
  - Command 0: 2 → 1, motor +158/s bound 474 out. Command 1: 1 → 3 (no actions); after 1.5 s 3 → 2 (brake 158).
- [O harness `lever.cpp`, HEAD libraries]
  - The axes: j0 is (1, 0, 0), horizontal; j1 is (0, 0, 1), vertical, mass 100.
  - Command 0 drives the post out by itself (787 after 10 s).
  - After command 1 with the post drawn to 1264, it holds in state 3 for 1.5 s. Then, in state 2 (brake 158 < the
    gate's weight per step, 100 × 5.72443 / 2 ≈ 286), the **gate falls and drags the post back**: j0 1264 → 1167 →
    867 → 366 → 0 within about 4 s.
- [O] Ours runs the first non-empty vt in 55..60 (here vt 59, command 0) after a 1 s heave and then
  `releaseGrapple` (`src/game/World.cpp:699`, `src/script/Script.cpp:1026`). Nothing calls vt 46, and `larabase` 26
  is `no` in `db/script_natives.tsv`. The harness log shows the stop: "handler stopped: native larabase fn 26".

### tru.exe evidence [O]

- `larabase` 26 thunk 0x79A1A0 → **0x777C30**:
  - It resolves the argument instance (0x4AA530 → INSTANCE_Find 0x5B3950).
  - It queries the player instance `*0xE7F094` with 0x175 (grapple controller), then 0x17C on that.
  - It returns `target->+0x4C->+0x44 == instance->+0x44`: "Lara's rope is attached to this instance".
- `instance` 14 thunk 0x798D60 → 0x4AB400: `*result = (this+0x20)->+0x58`, the instance's own handle.
- Rope attach 0x6A35E0: when the target's object has a script (`+0x60 != 0`) it calls 0x799840. That thunk reads
  prototype slot `0x105B8A0`, three slots before vt 49's `0x105B8AC` (0x799900, prototype 67). That makes it
  prototype 64 = **vt 46** [I high on the index arithmetic: slots are 4 bytes and the thunks 0x40 apart].
- [U] Who sends vt 59 (command 0) in tru.exe: it is the type's own vt, so no engine prototype names it. Ours sends it
  from the heave.

### Minimal fix

1. Call the target script's vt 46 when the rope attaches (Grapple attach → `script_.call(uid, 46)`).
2. Bind `larabase` 26(handle) = `player.grappleUid == uid(handle) && rope attached`.
3. For targets whose script has a state that polls `larabase` 26 (here: the `ca_ei_lever_grapple_post` family), do
   not auto-release after the heave; she stays roped until Interact / jump lets go. vt 59 (the existing heave call)
   keeps the post out meanwhile.

The tru.exe "her weight holds it" behaviour falls out: the state-1 motor holds the post while she is attached, and the
gate falls once she is not.

### Proving test

- New `GameReal.GrappleLeverReturnsWhenReleased`: l4_trailstart, throw at 6695, hold Interact until
  `uberJoint(6695, 0)->x > 1000`, let go. Expect calls 6695 vt 46 and vt 60, section 0 states 1 → 3 → 2, and
  `uberJoint(6695, 0)->x < 50` within 8 s.
- `known_gaps/ch12_trailstart_lever_gate` passes `let_go`. The window-sill route (C12-06) is a separate traversal gap.

---

## OU-PZL-008: ch13 temple statue head (ring not offered; head not moved)

Scenario `known_gaps/ch13_temple_block_topple`.

### (a) Ring 4951 not offered from the east ledge: decoded symptom high, cause medium

- [O harness `ring.cpp`] 4951 at (−7392, −13906, 3889), kind Mounted, **own range 5689** (above g_grapple's 4424.8, so
  range passes). From the ledge (x −2790, y −14380 … −11800, z 3687), d = 4605..5064. Only target sets 1 / 4 / 5 apply
  (d > 2880). In every case the sight ray (`Combat.cpp:1166`: eye → target, length d − 60, `kSightMask`) is **blocked by
  terrain ≈115–126 units before the ring** (hit at d − 115 … d − 126). Nothing is offered.
- [O 0x713090] tru.exe's sight test:
  - the ray end comes from 0x6FF040(target, feature index): query 0xDE (feature point), else 0x159, else the instance
    position + query 0xDB offset;
  - the ray mask word is `0x1F`; the cast is 0x4935F0 with hash 0xC5AC1983;
  - a hit blocks only when it is terrain or an instance with flag byte & 8 whose hit type is not 5 / 1 / 10;
  - type 5 / 1 / 10 hits on the target itself or its parent (+0x4C) count as visible.
- **Cause found and fixed (agent/Joint8):** the end point. The hit is the top face (z 3893, normal +Z) of the floor the
  post stands on, at x −7277: our ray ended at the named anchor's intro origin (z 3888.6, 4.4 under that face), while
  0x713090's ray ends at 0x6FF040's query-0xDE point, the post's grapple feature 117.8 up its axis (z 4006.4). The
  filter was not the difference: with every mask (0xFF too) the same face is hit, and with the feature as the end
  nothing is hit from any ledge point. Ours: `GrappleTarget::sight` (the feature through the intro matrix) ends the
  sight ray; the rope point stays the origin. Test: `ProgressionReal.TempleRingOfferedFromTheEastLedge`.

### (b) The head 5236 is not pulled off: decoded high

- [O data] `ca_rb_statue_head_temple`: class **34** (rigid body), mass 100, one box. Its script 13231 has only a
  `multibody` member and no handlers.
- [O bytecode] Plane 5247 (script 13242):
  - registers 5236 as its watched instance (`triggerplanebase` 0 with instanceref 5236);
  - its vt 55 fires when the crossing instance (`instance` 28) is 5236;
  - it then logs "break floor" and calls floor 5234 vt 54.
- [O bytecode] Floor 5234 (class 30 breakable, script 13229):
  - vt 54 calls `multibody` 92(1) on itself, sets stickies, then calls 5239 vt 73;
  - its state 1 loops on `multibody` 93 until true, then plays a sound and broadcasts "broken" on `floor_break`;
  - its state 0 polls `multibody` 4 on the head (the head's "fixed" flag) with a 60 s sound loop.
- So in tru.exe the head falls as a free body under the rope's pull, crosses plane 5247, and that breaks the floor.
- [O] Ours has rope bends (`src/game/Grapple.cpp` wrapRope) as geometric points with no body. `pullGrappled` moves
  only the rope target's own joints. Nothing applies the rope tension to a body the rope wraps.
- [O decomp, Joint8] tru.exe's bends are physics bodies: 0x6A4D40 (a new bend node from the free list +0xA4 into the
  rope's list +0xA0, prev +0x40 / next +0x44) calls 0x699A50, which makes a body per bend (0x4E98E0, shape size
  4.74 = 0x4097B43A via 0x4EE2B0, damping 0.02 = 0x3CA3D70A, 0x4DC770(5.0, 4.74) [U: mass?], 0x4E9590 its physics
  instance) at the bend position, and, for the bend next to the hand, 0x699900 / 0x6996F0 (a second body, size 3.95
  = 0x407CD70A, 0x4DC770(55.0, 4.74)) / 0x699960 (a joint, 0x4E03F0, between the rope body +0x38 and the bend,
  callback 0x698BE0) re-chain the rope's end. 0x6C5490 reads each bend's position back from its body (0x4D8D90: body
  +0x80). [Corrected 2026-11-20, agent/Rope11] The bend body is not pushed by contacts: it is pinned to the touched
  instance's body by the type-11 joint (0x4E1260, re-made every frame by 0x6B3570 while it touches), so the rope's
  pull on the bend reaches the body through that joint; 0x698BE0 is the joints' removal callback (it unlinks the
  bend node), and the joint removal 0x4E9B00 clears both bodies' fixed bit, which enables the head. The rope's end
  is the "[GrappleRope] Rappel Body" (0x6996F0, 55 kg, world-pinned at the last bend by a type-5 joint 0x4E0220)
  with a type-6 rope joint (0x4E03F0) to her. [U] the type-6 joints' tension law.
- Done (agent/Rope11; docs/GAMEPLAY.md "Rope bends on bodies"): the post's rope tied at its feature (from its origin
  every rope segment ran into the floor, so no bend formed), fixed rigid bodies as rope-only collision, bends that
  record and ride the object they touch, the head enabled (loose) by a bend, and a Jolt bend body (5 kg sphere 4.74,
  point constraint) per bend on a loose body, pulled by the rope while she tugs (one 0.215 s tug per Interact
  press: Wine) with a fitted tension [I]. Wine (2026-11-20): eight taps tip the head off after the eighth; ours
  after the fifth. `multibody` 92 / 93 (below) finish the floor.

### (c) `multibody` 92 / 93 [O]

- 92: thunk 0x79B8F0 → 0x4A7300: `if (*this) INSTANCE_Post(*this, 0x2C, value)`.
- 93: thunk 0x79B910 → 0x4A7320: `query(*this, 0xCA) != 0` → bool.
- Both are `no` in `script_natives.tsv`. [U] the breakable's 0x2C / 0xCA handlers (expected: "break" and "is broken").
- Fix: 92(n) → `damageObject(self, ∞)` / break the class-30 object; 93 → `object broken`.

### Proving test

`ProgressionReal.TempleHeadToppledByTheRopeBend` (agent/Rope11): from (−2790, −14380) offered 4951; throw; walk north
until the rope bends on 5236 (first at y −13365) and on to y −12940; one held tug and seven taps. Expect 5236 z < 1700,
5234 broken. The scenario `ch13_temple_block_topple` passes and is no longer parked.

---

## OU-PZL-006: freed slab sections stay solid (ch09 shrine, ch08 statue floor)

### Decoded cause (high)

- [O data, `OU_UBER_SECTIONS=sa_uo_naga_statue`]
  - Section 0 state 0 creates j0 (hinge world → statue), j1 / j3 / j4 (**type 8 fixed**, world → slab sections
    1 / 3 / 4) and j2.
  - At 45..60° (range 0) the statue goes to state 1: action 10 on j0 (the statue falls).
  - 0.48 s later it goes to state 4 and then 2: **action 10 on j1, j3, j4** (the slabs are released).
  - Slab sections then go `0 → 1` on cond (section 0 in state 2) and play anim-list clip 1 once (action 2, value
    0x10000).
- [O] Ours:
  - `makeUberJoint` returns nothing for type 8 (`Objects.cpp:651`), so action 9 creates no runtime joint and does not
    mark body B `held` (`Objects.cpp:523`).
  - Action 10 computes the freed body only from `rt.joints` (`Objects.cpp:539`): a fixed joint is never found, so
    `uberFreeSection` is not called.
  - `uberFreeSection` would also refuse, because the section is not `held` (`Objects.cpp:797`).
  - Net effect: the slabs stay posed and solid.

### tru.exe evidence [O]

- Action dispatch 0x4D86A0 (types 1..0x1A via table 0x4D8A78): 9 → 0x4D87F9 (create, 0x4D8000; type 8 maps to an
  engine fixed constraint, 0x4D1040, docs/formats/PHYSICS.md); 10 → 0x4D881D (remove, 0x4D6220).
- Removing the fixed constraint leaves the slab body free under gravity, with no other joint.

### Minimal fix

- On action 9 of a type-8 joint, record it in a `rt.fixed` set and mark body B `held` (the data already poses it at
  rest).
- On action 10 of a recorded fixed joint, take body B's pose (`uberSectionBody`), erase the joint, and call
  `uberFreeSection(uid, B, pose, 0)`.
- Make `uberSectionJoint` follow only live fixed joints.
- Save the set with the uber runtime.

### Proving test

New `ProgressionReal.NagaStatueSlabsFallAfterThePull`: pull 2794 to state 2. Expect sections 1 / 3 / 4 `freed`, their
bodies below z −3068 within 2 s, and `player.hanging` on ledge 679 by input (`known_gaps/ch09_shrine_shaft_descent`
`over_the_edge`). The statue's ring still being offered (step `pull_the_statue`) is separate: the grapple feature's
section (model 1, section 0) is freed but stays a target. Fix: drop freed / loose sections from
`World::grappleTarget` [I].

---

## OU-PZL-015: `multibody` 3 unbound (ch08); thrones (ch14)

- [O] `multibody` 3: thunk 0x79ACD0 jumps to 0x4A6FF0. That queries 0x13F (body count) and 0x140 (body array), and for
  each body with a physics component (+0x274) calls **0x4D8FF0**: `or [ecx+0x15C], 1; [ecx+0x160] = 0.0` (set the
  "fixed" bit, zero its timer).
- `multibody` 4 (0x4A7070 → 0x4D9000) returns true if any body has bit 0. `multibody` 2 (Enable) clears it via
  0x4D8FD0 (already noted at `Objects.cpp:1731`).
- Fix: bind 3 as the inverse of 2. The object becomes fixed again (ours: `loose = false`, hinge motor locked) and 4
  reads true. [U] which ch08 object calls it and what it then expects: not dumped here.
- ch14 throne "unbound native" (REPORT §7) was not identified in this pass [U]. `OU_PROGRESSION_DUMP` on the throne
  intros plus the self-test log line names it.

## OU-PHY-001: joint types 5 / 7, body-A motion

- [O survey `jsurvey.cpp`, every object DRM] **Type 7**: 7 records (`sa_rb_pushable_weight`, `ma_rb_pushblock_training`,
  `ma_pu_thor_pillar`, `ca_pu_thor_pillar`, `ca_pu_block_grapple`, `ca_pu_block_a`, `ar_pu_valaskjalf_entry`), all with
  **body A = world**. "Type 7 on a moving body" does not occur in the data. The hinge riders on them (body A = the
  type-7 section) are already merged into one body (`makeUberJoint3`).
- **Type 5**: 3 records, only `sa_uo_shiva_right_scales_linked` j2 / j3 and `sa_uo_shiva_left_scale` j4 (the pans,
  body A = the beam on a world hinge). Their only parameters are kind-5 limits (0, 0, off). No watch, transition or
  link references a type-5 joint (the scales watch their beam hinges j0). They are cosmetic swing joints and block no
  progression [I high].
- **Body-A motion**: 61 records have a section as body A. Those whose body A itself moves on a joint include
  calendar / calendar_b j2 (on j1 prismatic), ms_ani_entrance_gate, the tentacle gears, the snake heads, the bedroom
  door ring and the windup statue. Only calendar_b's section-0 lever (OU-PZL-007 above) touches a parked scenario, and
  even there it is not on the critical step.
- Fixed joints (92 records): see OU-PZL-006. Their release is the progression-relevant part.
- **Done (agent/Joint8):** body B is posed by body A's pose × its joint's motion (`World::uberSectionBody`,
  `uberJointPose`), a live fixed joint poses body B where body A is, also on a freed body A. Survey of riders left on
  a freed body (every state whose action 10 frees a section while a joint with that section as body A stays):
  `ma_uo_bedroom_door` states 1 / 4 / 5 (hinge j0, the ring), `ms_rb_grapple_snake_head` (hinge j2),
  `sa_uo_chain_debris` (hinge j1), `sa_uo_naga_statue` (hinge j2), and the only fixed riders,
  `ms_ani_tentacle_gearr` (j9-j15 between the sections; each section's state 1 removes its own world fixed joint after
  clips of different lengths). [I] Body A's motion is not fed back into the Jolt joint body's dynamics, and a fixed
  rider follows its freed body A without adding its mass.

## OU-PZL-014 (prologue ring door)

`ma_uo_bedroom_door`: the door section is on slide j1; its state 1 removes the joint and gives birth to
`squib_explode_door` (`ProgressionReal.PrologueRingDoorPulledOffByTheGrapple`). The ring (section 0) rides the door
through the body-A hinge j0 (pivot 595 up, 47 out).
- "Leaf stays standing in its doorway" no longer reproduces: fixed by 6d6c9eb (2026-11-10, the squib's knock on its
  parent, Wine-measured) before the audit listed it.
- Joint8: the ring now rides the leaf's slide while it is drawn, and its loose body and hinge are made where the leaf
  carries them (they were 36 units off). The knock gives the leaf (−930, −26, +39) units/s after one step, yaw
  −6.5 rad/s, tip −1.75 rad/s; tru.exe (s4.txt) (−870, 0, +364), yaw −8, tip ≈ 4.6 rad/s. The door's record
  elevation is 0 (`2775 b00b 0005 0002 0768 000a 0768 03e8 0190 0000`), so tru.exe's lift and faster tip come from
  something not decoded [U] (its knock handler on a Havok body, or the shape blast 0x492CC0 kept on the parent, which
  ours erases). Where the tumble ends is chaotic in ours: 1.1 thousand into the room, flat (tru.exe 656, flat).

## Proposed docs/audit/BUGS.md updates (for the parent)

- OU-PZL-007: cause found. The scenario grips section 0's locked lever (record 540); the jaguar handle is section 1's
  record 528, and ours turns the ring with it (j2 −0.65 rad in 4 s). Fix the scenario step and probe sign. Move from
  C-JOINT to "test bug".
- OU-PZL-009: the cause is confirmed at instruction level (0x4A5A90 → 0x2E → 0x6B453B). The regression behind the name
  check is CriticalGates gate 498 in a fresh World (locked until "Kraken dead"), not switch 30.
- OU-PZL-010: cause confirmed. A jointless uber section is never moved by `pushUberHandle`. Fix: synthesize a
  three-axis joint on grip.
- OU-GRP-005: new cause. vt 46 is not called on attach, `larabase` 26 is unbound (0x777C30: "rope attached to me"),
  and the heave auto-releases. The return mechanics already work (harness). Reclassify C-JOINT → C-VM.
- OU-PZL-008: (a) the sight ray is blocked about 115 units short of 4951 (end point / filter versus 0x713090); (b) the
  rope bend does not drag class-34 bodies; (c) `multibody` 92 / 93 are unbound.
- OU-PZL-006: cause confirmed. Fixed joints are never created, so action 10 frees nothing (`Objects.cpp:651`, `:539`,
  `:797`).
- OU-PZL-015: `multibody` 3 = set the bodies' fixed bit (0x4D8FF0).
- OU-PHY-001: no type-7 record has a moving body A. Type 5 is only the shiva pans (unwatched). Narrow the entry to
  "fixed joints + body-A motion".
