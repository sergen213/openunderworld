# Progression map and gates

Per chapter (`game::kChapters`): the critical-path units, the gates between them and their status in our runtime.
Labels as elsewhere: **[O]** game data / tru.exe, **[I]** inference, **[U]** unknown.

Tools (`tests/test_gates.cpp`, `ou_gates_tests`):
- `ProgressionReal.CriticalGates` (suite): the gates marked **pass** below, each in a fresh streaming `World` of its unit:
  Lara is placed at the gate, performs the original interaction (switch use, push, grapple pull, walking into a
  volume / through a plane, earlier steps first), and the gated effect is asserted (target door / uber object / block
  visibly moved, cinematic played, chapter changed).
- `ProgressionReal.CriticalPathPerChapter` (suite, ~2.5 min; `OU_PROGRESSION_CHAPTER=n` for one): each chapter's gates
  in route order in one `World` started at that chapter (`World::startChapter`), script state carried along.
  `OU_PROGRESSION_ROUTE=1 … CriticalPathInOrder` (~5 min, opt-in): the whole game in ONE `World` from chapter 0, each
  chapter end starting the next, a level load (`World::enterUnit`) standing in for the walk between gates, then the
  Natla fight and the credits (`playEnding`, shared with `EndgameCredits`). 2026-11-06: both pass. Fixed on the way:
  l1_kraken_entrance 748 — the harness put Lara under snake head 751 at its height (~2000 above the floor); she fell
  to her death and, in a chapter-started World, respawned at l1_seatop, unloading the gate's unit (the harness now
  stands her on the floor, and its teleports cross no trigger plane); l1_baldrs_tomb 221 passed alone only because gate
  45 swung open at load (no lock held it); it now opens from its locks (below).
- `OU_PROGRESSION_SURVEY=1 … --gtest_filter=ProgressionReal.Survey`: portals (with stream unit ids), units named by
  scripts, level ends and the gate objects of every unit with their static links.
- `OU_PROGRESSION_SURVEY=1 … --gtest_filter=ProgressionReal.GateSweep`: every candidate gate of every critical-path
  unit found from the data (switches/levers, push blocks, mounted grapple targets, pressure plates, volumes / planes
  linked to a door, uber, physics or cinematic object), one line each `PASS/FAIL -> +changed -unchanged [first
  handler stop]`. `OU_PROGRESSION_UNIT`, `OU_PROGRESSION_GATE`, `OU_PROGRESSION_TRACE` narrow / trace it.
- `OU_PROGRESSION_DUMP="unit:uid,…"` (or `unit:*`): bytecode of intro scripts down to the native base (states,
  prototypes, members, calls resolved). The analyses below were read from it.
- `OU_UBER_DUMP=<object>` (`UberDump`: ObjectBase hex, `*` = pointer), `OU_OBJ_FIELDS=<unit>:<uid>` (`ObjectFields`),
  `OU_ANIM_NAMES=<object>` (`AnimNames`): data research for uber records, script fields and animation lists.

Sweep classification (2026-11-06): a gate that does not pass is a **FAIL** unless it is excused as one of
- `preload`: its handler only called vt 63 on its cinematics (a later plane / volume plays them);
- `condition`: its handler ran but called none of its linked targets (links read as conditions, or an earlier step is
  missing; self-targets — grapple pulls, blocks — are never excused this way);
- `state`: it called its targets and changed only their script fields (flags later handlers read, e.g. the l1 kraken
  body's vt 75 / 76 "Lara in zone" flag);
- `encounter`: all its targets are enemies / generators (no AI runs in the harness).
The harness spawns Lara away from every trigger (l3_main_bedroom's intro sits inside volume 4521, so its FirstIn
ran before the gate), records script-to-script calls (`Runtime::onCall`), and also counts anim graph states and
joints (plates, uber joints) for targets a script called. Counts: 133 / 289 pass with the old classification
(156 "FAIL"); corrected harness, same code: 137 pass, FAIL 27, condition 61, encounter 36, preload 28; after the
fixes below: **138 / 289 pass; FAIL 24, state 11, condition 52, encounter 36, preload 28**.

Fixes (2026-11-06):
- [O] Triggers test instances besides Lara (0x5CD520): every object with ObjectBase oflags 0x100 (listed at birth,
  0x5BA810 -> 0x5CF680) and the instances a volume added with `triggervolumebase` 0 (before: any non-Lara Add stopped
  the handler). Volumes run prototype 8 / 9 (vt 56 / 57) with the instance, added ones also 16 / 17 (reason, instance);
  planes run prototype 7 (vt 55) on a crossing (`Runtime::tickTrackedInstances`; saved: `holds`, `lastAt`, setup
  `added`). Unblocks l4_ballcourt 6519 (block 6504), l2_junglehub 3638 / 3639, l2_hubhall 3168, l5_valaskjalf 7111,
  l4_temple plane 5247 (statue head 5236) — their handlers now run; what moves the statue head is [U].
- [O] `multibody` 4 (0x4A7070: an atom still fixed, body +0x15C bit 0; ours: not on a hinge and not loose [I]) and 59
  (0x4A93E0: INSTANCE_Post 0x35 breaks a joint; ours: the object falls loose [I]): l5_valaskjalf falling bridge 6974
  (volume 7010) passes (`CriticalGates`, `CriticalPathInOrder`).

Sweep 2026-11-07 (grapple handles): **136 / 251 pass; FAIL 11, state 11, condition 53, encounter 9, preload 31**. The
`sa_ei_grapple_wall*` / `sa_ei_grapple_mounted` objects are no longer pull candidates: they are swivelling grapple
handles, anchors (GAMEPLAY.md "Grapple"); only uber targets whose state machine watches the pulled joint are Mounted.
Their volumes pass as before (2038 / 2045 -> 2042, plane 2039 -> 2043). Attribution (same sweep on base ee7a57c:
136 / 260, FAIL 20, state 11, condition 53, encounter 9, preload 31; per-gate lines diffed): the only difference is
the nine handle "pull" lines gone (2005 / 2042 / 2043, 2383 / 2393 / 2394 / 2395, 2457, 2595, all FAIL in the base);
every other gate's line is identical. The 289 candidates / 36 encounters above predate ee7a57c (already 260 / 9 on
the base, not from this change). Shiva arms 2982 / 2983 spawn with uber action 0x1A (no grapple target); trigger
volume 3023's script commands them into the state with 0x19 (`GameReal.GrapplePullsUberTargets` walks Lara through
it before the pull). l5_valaskjalf 7025 / 7026 still print FAIL in the sweep (each gate fired alone) but are not
true failures: their chains 6943 / 6944 are started only by jammed gear 6985's vt 34, now run to its end
(`ProgressionReal.ValaskjalfChainsDetach`, see the chapter 5 rows). True failures left (9): l1_kraken_room soundvolumes
358/360/361/362/364/365/380/381 (vt 75 clears a flag already clear: not a gate); l7_helheim01 8945 (`anchor` 1,
below). Natives bound since: `multibody`
89 (0x4AA250: motor target speed of a joint, joint +0x108 -> `Object::Joint::speed`; a non-plate, non-hinge joint moves
at it within the body's limit, physical data +0x2C -> {lo, hi} [O values]), and the joint range watches report leaving
(vt 37) as well as entering (vt 34), starting "inside" as 0x49A5C0 does, so a watch outside its range reports leaving
on the first check [O] (l1_baldrs_tomb: locks 69 / 70 count gate 45 shut (+28 = 2); weight on plate 222 and Lara on
221 sink both, the counter reaches 0, 45's limit opens 0..90 and it falls); `larabase` 6 (0x777A70: Lara's query 0xF8,
the largest contact force with an instance; ours [I]: how deep its collision reaches into her capsule — the traproom
crushers 5905, whose jaws are [U]); `unit` 12 / 13 (0x776BA0 -> 0x7164D0 / 0x776C30 -> 0x71DB90: the instance's
flag-4 markers joined to / dropped from Lara's list: ours, its grapple target offered or not, saved — l4_pool 5511 via
5625); `instanceref` 0 (0x79A940 -> 0x4A52E0: same instance; l1_seafloor 978, l7_arctic_sea 9280). Open: `anchor` 1
(0x7966F0: the vector between two anchors, 0x4AB870; the VM has no vector values — l7_helheim01 8945, a sound loudness
loop, not a gate). l5_valaskjalf_entryb 7189 (pole-hole lever, reward door only): solved 2026-11-07 by inserting the
pole (below).

Fixes (2026-11-07):
- [O bytecode] l5_valaskjalf volumes 7025 / 7026 ("lara entered detach volume") are not failures: they need an earlier
  step. Chains 6943 / 6944 (`ar_ani_infinite_chain`, one uber section: state 0 rest, command 0 = trigger 16 param 0 ->
  state 1 running, command 1 = trigger 16 param 1 -> 2 detach -> after 1 s 3 -> 1) have no constructor, unlike
  6939-6942, which send their command member (+4, command 0) after 3 s. Only the jammed gear 6985's vt 34 (its joint
  entering 45..180, multibody 79) sends 6943 / 6944 their command members (`CALL4 uberobjectbase 1`); the volumes' vt 55
  sends the chain's own local command (`PUSHLA 0; CALL1 uberobjectbase 1`, a local `uberobjectcommand` initialised to
  command 1), which finds no transition from state 0 (0x4D4BE0) — as alone in the sweep. The call is the chain's own
  (CALL1's receiver is the frame's instance, the chain); no VM change was needed. 6985's vt 34 used to stop at two
  unbound natives: `multibody` 75 (thunk 0x79B6A0 -> 0x4A9AB0 -> 0x4D9F50: limit `args[1]` 1..3 of joint `args[0]`
  on / off, joint +0x174 bit 2 << (limit - 1), read by the joint solver 0x4E7550 only while set; hpoles 6984's vt 54
  turns it off. Ours: limit 1 off frees the hinge / a motor's clamp [I: the hpoles' own motion is not modelled]) and
  `multibody` 80 (0x79B760 -> 0x4A9D90 -> 0x493A80: drop the range watches of joint / limit with id `args[2]`, -1 =
  all; 6985 stops its own watch 3). Now 6985 vt 34 runs to the end, 6943 / 6944 run, and 7025 / 7026 detach them
  (`ProgressionReal.ValaskjalfChainsDetach`). The sweep, which fires each gate alone, still lists both as FAIL.
- [O tru.exe] Inserting a portable: Lara's chart action 0x4D posts INSTANCE message 0x165 to the held portable, which
  posts anim stimulus 0x20 to itself and 0x165 to the receptacle (0x6B5890), whose graph takes 0x20 (0x6C2530); 0x186 /
  0x40 is the removal (docs/GAMEPLAY.md). Ours: receptacles (scripts with vt 19 whose graph has an 0x20 transition) get
  their graph even without a `modifiable` member; a dataless one starts where its insert transition leaves [I].
  l5_valaskjalf_entryb lever 7189 now opens reward door 7227 (`ProgressionReal.PoleHoleLeverOpensRewardDoor`): its
  joint marker is a cylindrical joint (0x48F1D0: degree of freedom 0 = prismatic position), so its −600..−474 watch is
  a slide down; Lara's weight on the inserted pole slides it [I: Havok weight, 300 units/s, stop at −600]. Open [U]:
  the insert clip and engagement record, removal, l4_ballcourt 6502 (also a receptacle, still driven by a use).

Sweep 2026-10-29 (uber joints): 132 / 289. Every uber object now starts its sections at spawn, so its first script use no longer counts as a change: the seven `sa_ei_grapple_wall` pulls (l2_maproom 2595, l2_norsehall 2457, l2_norsehub 2383/2394/2395, l2_towerconnector 2005/2042/2043 and volume 2040 -> 2043) and l5_valaskjalf volumes 7025/7026 (targets 6944/6943) passed only by that lazy start [I: not re-examined]; l3_bedroom_hall plane 4375 now passes.

Sweep 2026-10-29 (ending pass): 141 / 289 candidate gates pass (135 before it on 839ea16; new, all from counting a change at
any tick: l1 tentacle volumes 562-564, switch 57's attempt reaction, resetting switch 748, l2_norsehall 2493). l4_ballcourt 5468 fails alone since
gates 4: it needs switch 5469 and plane 6410 first (the critical gate runs them). The sweep over-counts failures: preload volumes (they only call
cinematic vt 63, see below), sound volumes whose links are conditions, and handlers that need an earlier step all
show FAIL on their own. The interactive failures that block a route are listed under each chapter.

## Chapter routes

Portal order from the start unit; level changes not made by portals are named.

| ch | route | end |
|---|---|---|
| 0 | l3_main_bedroom → l3_bedroom_hall → l3_main_hall | plane 4129 (scripts: triggerplane) → cinematic 4106 (010) → chapter 1 |
| 1 | l1_seatop → l1_seafloor → l1_norse_hallway_a → l1_kraken_entrance → l1_krakenstairs_a/b → l1_kraken_room → l1_baldrsstairs_a/b → l1_baldrs_tomb (→ sideroom → hall → kraken_room) … l1_seatop → l1_cineseatop → l1_ship_exterior → l1_ship_upper_hallway_a/b → l1_ship_upper_hallway → l1_ship_engine_hall → l1_ship_natla_room → l1_ship_lower_hallway → l1_ship_sinking_hall | volume 1082 preloads 1005; plane 1074 fades and moves Lara onto 1005 in l1_cine_ship_sinking (instance 4/26), whose unit script plays it when she enters that unit (vt 11); 1005's end requests l2_cliffclimb (uigamedata 27) |
| 2 | l2_cliffclimb → l2_cliffhall → l2_cliffhall_b → l2_junglehub / l2_hubhall → l2_puzzleroom → l2_upperhallway → l2_towerroom → l2_towerconnector → l2_norsehub → l2_norsehall → l2_puzzleroom → l2_shrineroom → l2_maproom → l2_flushtunnel → l2_cliffclimb | flushtunnel plane 2542 → l2 unit script 9502 vt 26 sets volume 2564's "BeatLevel" sticky (+40); volume 2564 at the boat then plays 2565 → chapter 3 |
| 3 | l3_caverns_start → l3_chapel → l3_stairhall → l3_secretchamber → l3_chamberrockhall → l3_main_bedroom → l3_bedroom_hall → l3_main_hall | volume 4137 preloads 4108; plane 4128 plays 4108 (160), whose post-stream helper plays 4070 (170) in l3_mansion_exterior_afterburn → chapter 4 |
| 4 | l4_trailstart → l4_trailtube → l4_ballcourt → l4_templepath → l4_temple; l4_ballcourt → l4_staircase_upper → l4_staircase_xibalba → l4_traprooms → l4_staircase_mictlan → l4_ragnarok_room → l4_pool, back to l4_ballcourt | volumes 5468 / 6407 → 5483 → chapter 5 |
| 5 | l5_startsnow → l5_valgrind_tunnel → l5_valgrind → l5_valaskjalf_entry → l5_valaskjalf_entryb → l5_valaskjalf | volumes 6866 / 6872 → 6846 → chapter 6 |
| 6 | l6_ship_exterior → l6_ship_upper_hallway_a/b → l6_ship_upper_hallway → l6_ship_engine_hall | volume 7566 preloads 1387; plane 7565 plays it → chapter 7 |
| 7 | l7_thin_ice → l7_arctic_sea → l7_helheim_hall01/02 → l7_helheim01/02/03 → l7_yggdrasil → l7_yggdrasil_hall01/02 → l7_yggdrasil_end | buttresses A-D broken (dispatcher 8360) → 340_hel_endgame (8263); its end warps Lara to l7_monastery (instance 26 on 8138) and plays 342_hel_endgame, whose end saves and plays the credits: the ending (below) |
| 8 | l7_himalaya_end | not on the route: its 7996 (343_hel_engame) plays on its unit script's mc_CinematicData 7 when the unit is started directly, then the same end game save and credits [O bytecode] |

## Gates

| ch | unit | gate (interaction) | gated effect | status |
|---|---|---|---|---|
| 0 | l3_main_bedroom | switch 4525 (use) | house door 4520 opens | pass |
| 0 | l3_bedroom_hall | push block 4363 (push) | block moves | pass |
| 0 | l3_main_hall | plane 4129 (cross) | 010 → chapter 1 | pass (fixed) |
| 1 | l1_seafloor | eye puzzle pieces 231-233 | puzzle reveal | not covered: pieces are physics objects [U interaction] |
| 1 | l1_kraken_entrance | grooved plates 752/753 (snake head 751 pulled down onto 752, weight 749/750 carried onto 753) | locks of gate 743, oil doors 754/755 | pass |
| 1 | l1_kraken_entrance | resetting switch 748 (use, both plates down) | flints, torches, reveal 702 → cinematic 696, gate 743 | pass |
| 1 | l1_kraken_entrance | snake head 751 (grapple pull) | falls onto plate 752 (loose rigid body) | pass (in the 748 gate) |
| 1 | l1_kraken_room | switches 55 / 30 / 498 (use) | tentacle gear 36, sun-sphere platform 27, tomb door 497 | pass |
| 1 | l1_kraken_room | switch 57 (use) | tentacle gear 32 | pass after the column is shot (row below); alone the sweep's FAIL is the "attempt" reaction |
| 1 | l1_baldrs_tomb | press plates 219/221/222 (stand on; 221 + 222 together: a weight on one) | door 223, gate 45 (locks 69/70 on `multibody` 89 motors) | pass |
| 1 | l1_ship_upper_hallway_a/b, upper_hallway, engine_hall | volumes 1531, 1492, 1465, 1436 | ship doors, cinematics 1453 / 1288 | pass (1531 in suite) |
| 1 | l1_ship_sinking_hall | plane 1074 (cross) | 070_med_dive → chapter 2 | pass (fixed) |
| 2 | l2_puzzleroom | elevator switch 2961, weight 2971 (push), shiva grapples 2982/2983 (pull) | elevator 2973, weight, shiva arms | pass |
| 2 | l2_towerconnector, l2_norsehub, l2_norsehall | chest volumes 2137/2138; wall-handle volumes 2038 / 2039 / 2045 (2040: state only) | chests; handles 2042 / 2043 switched off / on as grapple targets (uber action 0x1A / 0x19) | pass |
| 2 | l2_towerconnector 2005/2042/2043, l2_norsehub 2383/2393/2394/2395, l2_norsehall 2457, l2_maproom 2595 | `sa_ei_grapple_wall*` / `sa_ei_grapple_mounted` | — | not gates: swivelling grapple handles, anchors to swing / wall-run on (GAMEPLAY.md "Grapple"; `GrappleHandlesSwivel`). 2393's op 0x34 is MOVEU (gates 3 below) |
| 2 | l2_maproom | volumes 2637/2638 | thor bridge 2667 + cinematics | pass |
| 2 | l2_flushtunnel | switch 2543 (use) | flush gate 2541 | pass |
| 2 | l2_flushtunnel → l2_cliffclimb | plane 2542, then volume 2564 | 2565 → chapter 3 | pass (new gate test) |
| 3 | l3_chapel | wall switches 4819-4822 (use), then plate 4834 (fire 4818) | shadow pillars 4837 sections, shadow door 4835 | pass (uber section state machines; `ChapelShadowPillars`) |
| 3 | l3_chapel | press plates 4832-4834 | gates 4830/4831, gold reward 4836 | pass |
| 3 | l3_chamberrockhall | switch 4617 (use) | secret door 4528 | pass |
| 3 | l3_secretchamber | volumes 4666/4669 | gauntlet cinematic 4654 | pass |
| 3 | l3_main_hall | plane 4128 (cross) | 160 + 170 → chapter 4 | pass |
| 4 | l4_trailstart | grapple lever post 6695 (pull) | lever | pull handler runs; the post slides out on its uber joint and its linked joint's range watch sends trigger 14 (section 1: 0 → 1, `GameReal.GrappleLeverJointIndependentOfRenderRate`); the sweep's snapshot does not cover joints [I: still listed as a sweep fail] |
| 4 | l4_ballcourt | pole 6503 inserted into pole lever 6502 and hung on, block 6504 (push) | door joint 6505, block | pass (`CriticalGates` action "hang", `BallcourtPoleLeverOpensDoor`, `GameReal.BallcourtLeverDrivesDoorJoint`: a use no longer opens it; the lever is a receptacle needing ca_pt_pole_a; its `multibody` 78 reads the slide, 0..−600 → 0..40 [I], ×2 → door 80°) |
| 2-7 | receptacles | portables inserted / removed (GAMEPLAY.md "Inserting a portable") | holes, keyholes, crown, eye piece, seal | pass (`ReceptaclesTakeTheirPortables`, `TraproomSealTakesTheRoomKeys`): l2_puzzleroom pole 2967 out of hole 2969 runs the elevator (2960 vt 54, 2961 vt 62) and into 2970; l3_chapel 4823 / l4_temple 5240, 5241 between holes (scriptless 5230 too); the placed gems and keys come out of their holders; seal 5701 vt 49(11) sets the dark-room sticky. Not covered by a teleport test: ballcourt keyhole 6501 (its key 5695 is in l4_ragnarok_room), l1 eye piece 232 (key 970 not picked up from a teleport spot), Kali crown 2392 (gem 2389 not present at load), l2_maproom track 2664 |
| 2-7 | uber receptacles | portables inserted into uber sections (engagement set +0x40 / action 0x1D, trigger 20 + 5 or 22) | shiva arms, calendars, throne, cog wheels, statue arms, gem holes | pass (`UberReceptaclesTakeTheirPortables`): l2_puzzleroom arms 2059 / 2060 lowered (vt 55 -> state 1) take gem 2964 / 2965 (state 1 -> 3 -> 1: vt 58 / vt 59 -> 2063 vt 56); l4_ballcourt calendar 6506 section 0 the headdress 6498 (-> state 3, vt 54), calendar_b 5477 section 5 the mask 6499 (state 3 -> 4); l4_traprooms throne 5891 the scepter weight 5883 (state 1 -> 2; the weight bears the press plate down); l5_valgrind cog wheel 7312 section 1 a power gem 7498 (0 -> 1); l7_arctic_sea (swimming) statue arms 9099 section 1 the thor gear 9274, gem hole 9120 an arctic gem out of holder 9276. [U] whether the route needs them (no critical gate waits on them in our sweep) |
| 4 | l4_ballcourt | autoreset switch 5469 (use) | temple reveal 5464 | pass (`ProgressionReal.TempleRevealReopens`: anim graph states 0 → 3 → 5 → 6 → 7, docs/formats/ANIMGRAPH.md) |
| 4 | l4_staircase_upper/xibalba | volumes/planes 6046, 6076, 6078, 6039 | hallway ramps 6045/6036, trap door 5702 | pass (`ProgressionReal.HallwayRampsJoint`: ramp section 0 falls on its prismatic joint to -948, trigger 14 tilts the ramps; 6078 raises it on its motor, 14 at the top) |
| 4 | l4_traprooms | plates 5717/5718 (stand on / scepter weight 5883) | seal 5701 | pass (`ProgressionReal.TraproomsSeal`, after the trap rooms: sticky +268 raised, int +240 = 4): each plate's vt 6 sticky notification counts it on the seal (+304), both held run vt 58 |
| 4 | l4_traprooms | block 5884 (push), planes 5900/5901/5904/5914/5915 | block, thrones, door 5702 | pass |
| 4 | l4_pool | volumes 5566/5624/5626 | snake heads, reveal cinematics | pass |
| 4 | l4_ballcourt | switch 5469, plane 6410 (190_mex), plane 5468 | 5483 → chapter 5 | pass (before gates 4 it passed only through puzzle_cheat_button 6512's vt 20 run at birth) |
| 5 | l5_valgrind | volumes 7471/7511 | door 7334, cinematic 7464 | pass |
| 5 | l5_valaskjalf_entry | switch 7228 (use) | reward door 7227 | pass |
| 5 | l5_valaskjalf_entryb | lever 7188 (use) | reward door 7187 | pass (`CriticalGates`; acts only while switch 7192 is not in its graph state 0 "on", read by `modifiable` 8) |
| 5 | l5_valaskjalf_entryb | pole-hole lever 7189 (insert pole 7190, hang on it) | reward door 7227 (optional) | pass (`PoleHoleLeverOpensRewardDoor`): the carried pole inserted from the lever's ledge (insert stimulus 0x20, GAMEPLAY.md "Inserting a portable") runs vt 19 "insert reaction" and vt 20(0) (watch −600..−474 on the slider); Lara hanging on the pole slides it down [I: speed / limit] → vt 34 → 7227 vt 57. A plain use still does nothing (the sweep's "use" shows condition) |
| 5 | l5_valaskjalf | volumes 7025-7030 | gears / chains | 7027-7030 pass; 7025 / 7026 detach chains 6944 / 6943 once the jammed gear 6985 started them (its vt 34; `ValaskjalfChainsDetach`); alone in the sweep they still show FAIL |
| 5 | l5_valaskjalf | volume 7010 | falling bridge 6974 breaks (multibody 59) | pass (`CriticalGates`) |
| 5 | l5_valaskjalf | volume 6866 | 6846 → chapter 6 | pass |
| 6 | l6_ship_upper_hallway* | volumes 7622/7623/7644/7646 | ship doors 7621/7642/7643 have no script (the volumes' ISTYPE fails): they stand open | pass (`ChapterSixShipDoorsOpen`) |
| 6 | l6_ship_engine_hall | plane 7565 (cross) | 270 → chapter 7 | pass |
| 7 | l7_arctic_sea | switch 9103 (use) | seafloor drawbridge 9116 | pass (uber transition 0 starts; was lowered at load by puzzle_cheat_button 9279) |
| 7 | l7_helheim01 | rune switches 8993-8995 | dispatcher 8997 | not asserted (dispatcher only) |
| 7 | l7_yggdrasil(_end) | volumes 8804, 8303/8312, 8323-8329 | cinematics 8773/8261, spline mover 8404 | pass |
| 7 | l7_yggdrasil_end | buttresses A (8360 vt 62) / B-D (mounts 8362-8364 vt 54), each revealed by 8296-8298 | 330_hel_thralls (C), 340_hel_endgame (last) → l7_monastery 342_hel_endgame → end game save, credits movie | pass (`ProgressionReal.EndgameCredits`, played: arena volume 8303 → 320_hel_boss breaks A and locks the mounts (anim graph state 2); Lara's use breaks each mount (stimulus 1 → state 3, vt 20); no handler called directly) |
| 1 | l1_kraken_room | column (gear 32 section 12, shot) then switch 57 (use) | column falls (trigger 15), gear 32 turns | pass (`ProgressionReal.KrakenColumnShot`; World::uberShot pushes the column's braked prismatic joint out of its watched range: trigger 15) |

## Fixes in this pass (each covered by `CriticalGates` / `ChaptersReal`)

- [O] Plane vs volume follows the script's native base (`triggerplanebase` / `triggervolumebase`), not the object
  name: l3_main_hall "triggervolume" intros 4128/4129 run plane scripts whose vt 54 is the crossing handler. Before,
  the prologue end 4129 never ran and the prologue could only end through 4137 → chapter 4.
- [O] Cinematic vt 63 only preloads (3523 fn 8 → state 2 = fn 13 creates the stream and waits); only vt 57 plays.
  The old rule counted preload volumes (1082, 4137, 7566) as level ends. `cinematic` 0/2 (Create / IsCreated) bound.
- [O] `health` 3 = INSTANCE_Post(5, n) = heal (0x4A5180); `unlockable` 1/2 (extras) passive.
- [O] Unit callbacks: gamebase prototypes 17/18/19 = vt 11/12/13 (bind 0x7B44A0). Level load calls vt 13(unit)
  (0x5D2310 → 0x4B0150), moving into a new stream unit vt 11(new) + vt 12(previous) (STREAM_MoveIntoNewStreamUnit
  0x5C1BF0 → 0x4B01E0); a unit script created later gets the current state (0x4B0050). Values are stream unit ids
  (Portal::toStreamId; unitref 0 = equality, 0x4A52E0). Unit scripts without a startup function are now created.
- [O] `instance` 26 (0x4A4140) on Lara with an `instance` 4 result: Lara is placed on that object (World streams
  around its unit).
- [O] SSEQU on a native that completes resumes after the call (0x57CE50 case 0x6A waits on the latent object);
  before, the handler ended there (l1_cine_ship_sinking vt 11 never reached its play call).

## Remaining blockers

- Pressure plates (base 3938) [O bytecode / I weights]: constructor `multibody` 1, `joint` 17 (prismatic), `limit` 3
  ranges (−60..−40 id 8 pressed, −30..10 id 12 released; subclasses add `multibody` 79 ranges). The plate is a Jolt body
  on its prismatic joint (docs/formats/PHYSICS.md "Pressure plates"), lifted by its motor and pressed down by real
  weight: loose bodies through their contacts, Lara (55 kg) and push blocks under gravity where they stand on it;
  entering a range runs vt 34(…, id) → sticky +92 and vt 54/55. Weights: pulled `*_rb_grapple*` targets fall
  (Object::loose), `portablebase` objects are carried (interact) and set down. [I] the plate's Havok motor numbers.
- VM fixes found here: START with a function word calls it; CALL3 is virtual; inline member defaults applied (ship doors:
  modifiable 1 opens, 2/3 close [I]; house door 0 opens / 1 closes).
- (2026-10-29, gates 3) l2_norsehub 2393's op 0x34 is MOVEU of a getter's return value; it runs since script returns
  work, and 2393 turns out to be only a camera-anchor helper (its instanceref +20 is unset), not a gate. The no-change handlers:
  l1 57 is the "attempt" reaction until the column (gear 32 section 12, physics-limit trigger 15 when shot) has fallen
  [shooting uber physics not modelled]; l4 5469 is the Xibalba-exit reopening (sticky 300), which drives `ca_ani_*`
  anim graph stimuli (now modelled, below); l5 7188 was skipped because our switch 7192 answered `modifiable` 8
  (graph state id) with 0 = its "on" state: switches now hold their graph state id (intro state 1, then the use
  transition's target; docs/formats/ANIMGRAPH.md), and 7188 opens 7187 from the start (it does nothing once 7192 is on);
  l5 7189 is a pole-hole lever [U: its 0x20 / 0x40 stimuli]; l7 9103's message reaches the unit script and
  commands drawbridge 9116, which the sweep counts as a change (uber transition 0; the table row). Previously listed as: VM opcode 0x34 DBOUNDS (l2_norsehub
  2393); levers / switches whose handlers run but change nothing visible (l1 57, l4 5469, l5 7188/7189, l7 9103);
  l4_traprooms seal 5701 (both plates held: one weight source missing). (gates 4) Solved: sticky Set notifications
  (vt 6 / 7) reach the plates' and the seal's handlers; the scepter weight is carried onto the second plate.
- (gates 4) Startup no longer runs a type's first function when it is vt 20 (puzzle_cheat_button debug objects sent their "solved" messages at birth). Killed objects (instance 10) no longer trigger. `uberobjectbase` 0 queries (+0x28/+0x2C condition lists) and larabase 53 (re-mount 52's bike) let 190_mex's end handler run. Open: triggers 14/15 still timed [I] (except shot bodies, below).
- (ending) [O bytecode] The play route ends in l7_monastery, not l7_himalaya_end: 342_hel_endgame's end (8138 vt 60)
  calls `uisavemenu` 6 (0x7A4640 -> 0x77D8A0: the autosave request with a unitref as the save's unit; ours: autosave)
  and `unlockablemovie` 3 "credits" (0x79DE20 -> 0x778F50 -> 0x5D6A80: Movies\credits.bik, then the frontend);
  Runtime::takeMovies -> ou-game plays it, marks the game finished and opens the main menu (TREASURE HUNT). Bound on the
  way: `larabase` 31 / 61 (0x776EF0 INSTANCE message 0xC / 0x778060 Lara clock fields), `cinematic` 1 (Play, 0x4ADB50),
  `hudscreenmgr` 4, `uigamedata` 43 (debug print), `uisavemenu` 9 (save header byte +0x1F9). Buttresses in play
  (anim graphs, below): arena volume 8303 plays 320_hel_boss, whose handler retracts the Thor towers 8398-8400 (uber
  command 0: their prismatic joints' motors pull them in, trigger 14 at the bottom → state 3, a weak brake); Lara pushes
  each back out until its joint enters [1074.6, 1137.8) (trigger 14 → state 2, "block_pushed"), whose event locks its
  mount 8362-8364 (state 2); Lara's use breaks each (stimulus 1 → state 3, vt 20). The endgame gun 8401 only reacts
  to broken mounts; it does not break them (the earlier [U] guess was wrong).
- (ending) `uberobjectbase` 3 (0x79D0D0 -> 0x4A4490) = AttachTo(anchor value, two words): the uber instance goes to the
  anchor's vtable slot 6 (INSTANCE link 0x5B4DE0 at its marker); passive for us (one model per uber object). The ~30 l1
  ship props (life preservers, lockers, cases, powerbox) and the endgame gun now run their constructors to the end.
- (ending) Shooting uber physics [I]: a bullet striking an uber object's collider (World::fire -> World::uberShot) pushes
  its watched joints (impulse 3000 along the shot); l1 gear 32's braked column slides out of its range (trigger 15,
  0x4D5B10), falls, and switch 57 turns the gear.
- (anim graphs) `ca_ani_*` objects, buttress mounts, flame traps and crates are anim-graph objects, not uber: physical
  class 6's state machine (docs/formats/ANIMGRAPH.md; no object uses the cdc AnimGraph). States / transitions play
  their clips, the timer stimulus runs each tick, Lara's use is stimulus 1, scripts hear vt 19 / 20; `modifiable` 0-8
  and `animated` 1 are bound. 5469 now opens the temple (0 → 3 → 5 → 6 → 7). [I] modifiable 8 reports a running
  transition's target (needed by l3_chapel fire 4818 / the shadow pillars).
- GateSweep counts a target changed at any tick of its 600-tick window (a hazard's swing back to rest, l1_kraken_room
  tentacles 494-496, counted as unchanged before).
