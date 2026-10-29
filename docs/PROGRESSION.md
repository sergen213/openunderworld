# Progression map and gates

Per chapter (`game::kChapters`): the critical-path units, the gates between them and their status in our runtime.
Labels as elsewhere: **[O]** game data / tru.exe, **[I]** inference, **[U]** unknown.

Tools (`tests/test_gates.cpp`, `ou_gates_tests`):
- `ProgressionReal.CriticalGates` (suite): the gates marked **pass** below, each in a fresh streaming `World` of its unit:
  Lara is placed at the gate, performs the original interaction (switch use, push, grapple pull, walking into a
  volume / through a plane, earlier steps first), and the gated effect is asserted (target door / uber object / block
  visibly moved, cinematic played, chapter changed).
- `OU_PROGRESSION_SURVEY=1 … --gtest_filter=ProgressionReal.Survey`: portals (with stream unit ids), units named by
  scripts, level ends and the gate objects of every unit with their static links.
- `OU_PROGRESSION_SURVEY=1 … --gtest_filter=ProgressionReal.GateSweep`: every candidate gate of every critical-path
  unit found from the data (switches/levers, push blocks, mounted grapple targets, pressure plates, volumes / planes
  linked to a door, uber, physics or cinematic object), one line each `PASS/FAIL -> +changed -unchanged [first
  handler stop]`. `OU_PROGRESSION_UNIT`, `OU_PROGRESSION_GATE`, `OU_PROGRESSION_TRACE` narrow / trace it.
- `OU_PROGRESSION_DUMP="unit:uid,…"` (or `unit:*`): bytecode of intro scripts down to the native base (states,
  prototypes, members, calls resolved). The analyses below were read from it.

Sweep 2026-10-29: 116 / 289 candidate gates pass. The sweep over-counts failures: preload volumes (they only call
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
| 7 | l7_thin_ice → l7_arctic_sea → l7_helheim_hall01/02 → l7_helheim01/02/03 → l7_yggdrasil → l7_yggdrasil_hall01/02 → l7_yggdrasil_end → l7_monastery | portal into l7_himalaya_end [U: no cinematic names it] |
| 8 | l7_himalaya_end | ending cinematic 7996 → credits [U] |

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
| 1 | l1_kraken_room | switch 57 (use) | tentacle gear 32 | **fail** (no visible change [U]) |
| 1 | l1_baldrs_tomb | press plates 219/221/222 (stand on) | door 223, gate 45 (locks 69/70: native `multibody` 89 [U]) | pass |
| 1 | l1_ship_upper_hallway_a/b, upper_hallway, engine_hall | volumes 1531, 1492, 1465, 1436 | ship doors, cinematics 1453 / 1288 | pass (1531 in suite) |
| 1 | l1_ship_sinking_hall | plane 1074 (cross) | 070_med_dive → chapter 2 | pass (fixed) |
| 2 | l2_puzzleroom | elevator switch 2961, weight 2971 (push), shiva grapples 2982/2983 (pull) | elevator 2973, weight, shiva arms | pass |
| 2 | l2_towerconnector, l2_norsehub, l2_norsehall | wall grapples, chest volumes 2137/2138 | grapple handlers, chests | pass |
| 2 | l2_norsehub | mounted grapple 2393 (pull) | its handler | **fail**: VM opcode 0x34 (DBOUNDS) |
| 2 | l2_maproom | volumes 2637/2638 | thor bridge 2667 + cinematics | pass |
| 2 | l2_flushtunnel | switch 2543 (use) | flush gate 2541 | pass |
| 2 | l2_flushtunnel → l2_cliffclimb | plane 2542, then volume 2564 | 2565 → chapter 3 | pass (new gate test) |
| 3 | l3_chapel | wall switches 4819-4822 (use) | shadow pillars 4837 | **fail**: command runs; uber section state graphs (type 2 = trigger a transition, tru.exe 0x4D7680 → 0x4D4BE0) not modelled |
| 3 | l3_chapel | press plates 4832-4834 | gates 4830/4831, gold reward 4836 | pass |
| 3 | l3_chamberrockhall | switch 4617 (use) | secret door 4528 | pass |
| 3 | l3_secretchamber | volumes 4666/4669 | gauntlet cinematic 4654 | pass |
| 3 | l3_main_hall | plane 4128 (cross) | 160 + 170 → chapter 4 | pass |
| 4 | l4_trailstart | grapple lever post 6695 (pull) | lever | **fail** in the sweep (handler runs, nothing visible changes) |
| 4 | l4_ballcourt | pole lever 6502 (use), block 6504 (push) | door joint 6505, block | pass |
| 4 | l4_ballcourt | autoreset switch 5469 (use) | temple reveal 5464 | **fail** (no visible change) |
| 4 | l4_staircase_upper/xibalba | volumes/planes 6046, 6076, 6078, 6039 | hallway ramps 6045/6036, trap door 5702 | pass |
| 4 | l4_traprooms | plates 5717/5718 (stand on) | seal 5701 | **fail**: plates press (base 3938 + `multibody` 79 range 56); 5701 counts both via stickies (needs both held) |
| 4 | l4_traprooms | block 5884 (push), planes 5900/5901/5904/5914/5915 | block, thrones, door 5702 | pass |
| 4 | l4_pool | volumes 5566/5624/5626 | snake heads, reveal cinematics | pass |
| 4 | l4_ballcourt | volume 5468 | 5483 → chapter 5 | pass |
| 5 | l5_valgrind | volumes 7471/7511 | door 7334, cinematic 7464 | pass |
| 5 | l5_valaskjalf_entry | switch 7228 (use) | reward door 7227 | pass |
| 5 | l5_valaskjalf_entryb | levers 7188/7189 (use) | reward doors 7187/7227 | **fail** (lever handlers change nothing visible) |
| 5 | l5_valaskjalf | volumes 7025-7030 | gears / chains | pass |
| 5 | l5_valaskjalf | volume 6866 | 6846 → chapter 6 | pass |
| 6 | l6_ship_upper_hallway* | volumes 7622/7623/7644/7646 | ship doors 7621/7642/7643 have no script (the volumes' ISTYPE fails): they stand open | pass (`ChapterSixShipDoorsOpen`) |
| 6 | l6_ship_engine_hall | plane 7565 (cross) | 270 → chapter 7 | pass |
| 7 | l7_arctic_sea | switch 9103 (use) | seafloor drawbridge 9116 | **fail** |
| 7 | l7_helheim01 | rune switches 8993-8995 | dispatcher 8997 | not asserted (dispatcher only) |
| 7 | l7_yggdrasil(_end) | volumes 8804, 8303/8312, 8323-8329 | cinematics 8773/8261, spline mover 8404 | pass |

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
  ranges (−60..−40 id 8 pressed, −30..10 id 12 released; subclasses add `multibody` 79 ranges). World::tickPlates moves the
  joint at 120 units/s toward the lowest range while Lara, a push block or a loose rigid body rests on the plate's
  footprint, else back to 0; entering a range runs vt 34(…, id) → sticky +92 and vt 54/55. Weights: pulled `*_rb_grapple*`
  targets fall (Object::loose), `portablebase` objects are carried (interact) and set down. [I] no Havok mass/tumbling.
- VM fixes found here: START with a function word calls it; CALL3 is virtual; inline member defaults applied (ship doors:
  modifiable 1 opens, 2/3 close [I]; house door 0 opens / 1 closes).
- Uber section state graphs (chapel shadow pillars 4819-4822: commands of type 2 trigger transitions; section record stride / layout beyond the first section [U]); VM opcode 0x34 DBOUNDS (l2_norsehub
  2393); levers / switches whose handlers run but change nothing visible (l1 57, l4 5469, l5 7188/7189, l7 9103);
  l4_traprooms seal 5701 (both plates held: one weight source missing).
- Chapter 7 → 8 has no scripted level end in the data we decoded [U]; the ending (7996, credits) is not run.
