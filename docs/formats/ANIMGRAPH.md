# TRU object anim state graphs (physical class 6)

Code: `src/formats/AnimGraph.*` (`loadAnimGraph`, `AnimGraph::pick*`, `graphRandom`), `src/script/Script.cpp`
(`Runtime::graphSet`, `graphEnter`, `graphBegin`, `graphStimulus`, `graphPost`, `graphUse`, `graphHit`, `tickGraph`,
the `modifiable` / `animated` natives, `Runtime::pose`).
Tests: `AnimGraph.LcgAndTransitionPicks`, `ProgressionReal.CombatCrateBreaksOnHit`, `ProgressionReal.TempleRevealReopens`,
`ProgressionReal.EndgameCredits`, `ProgressionReal.ChapelShadowPillars`, `ProgressionReal.CriticalGates`; research
`ProgressionReal.AnimGraphRecords` (`OU_ANIMGRAPH_RECORDS=1` or `=<object>`).

Labels: **[O]** observed in game data or our tru.exe decompile (GOG 1.1.0.0 VAs), **[I]** inference, **[U]** unknown.

## What the "anim graph" is

- [O] No TR8 object uses the cdc AnimGraph: ObjectBase `+0x64` hasAnimGraph is 0 (and `+0x68` graph null) in every
  object DRM, so `0x453100` never builds one.
- [O] What scripted animated objects (`ca_ani_*` temple reveals, buttress mounts, flame traps, combat crates, ...) run
  is the anim state machine of **physical class 6**: ObjectBase `+0xC8` points at a record with `u16 +4 == 0xB00B`
  and `u16 +2 == 6` (see PHYSICS.md for the class dispatch). The instance's anim component (Instance `+0x294`) runs
  it (`0x6A5660` enter state, `0x6A5C50` / `0x6B3DD0` transitions, `0x6B4B10` setup).
- [O] 95 object types carry a graph. Survey (`ProgressionReal.AnimGraphSurvey`, `OU_ANIMGRAPH_SURVEY=1`) over every
  unit: 4117 scripted objects, 677 run a graph, 359 of them posed by a state clip, 10 change state on their own (l4
  flame traps, timer transitions).

## Records

Queries `0xE6` / `0xE7` (`0x6C7A60`) read record `+0x30` / `+0x34`: each is a pointer to a pointer to the first
state / transition. Records follow each other while their tag holds (`0x6B4B10` walks them the same way).

Record `+0x2C` points at the state an instance starts in [O data: set in all 95 graph objects; in 25 it is not the
first record: the receptacles' empty state (pole holes, key holes, levers), `ma_ani_foor_collapse`'s `idle` before its
first record `idle_collapsed`, the artifacts' record 4, `ma_ei_ignitable` 3, …]. `AnimGraph::initial`; ours starts
intros without intro data there (with data, message 0x23 picks record `data word 15`, 0 = the first, as before).

State, 0x28 bytes, tag `0x2714`:

| Off | Type | Meaning |
|---|---|---|
| +0 | u16 | tag 0x2714 |
| +2 | s16 | id (modifiable 7 / 8, script vt 20); negative = lowest id < 32 no other state uses |
| +4 | u16 | flags: 2 loop the clip (else once and hold); 0x40 reported by modifiable 2 (query 0xED); 0x800 / 0x400 / 0x200 / 0x100 / 0x80 post INSTANCE message 0x138 with 1 / 3 / 5 / 0 / -1 on entry (`0x6A5660`) to the burn component (`0x6B0C00`; only `ma_ei_ignitable` sets them) [not modelled] |
| +7 | s8 | animation-list index played on entry, -1 keeps the pose |
| +8 | u32 | posted on entry as INSTANCE message 0x1E2, which `0x6B5890` stores in the anim record `+0x80` [U: no reader found]; 0xBEEBBEEB in every object but a few 0s [not modelled] |
| +0x14 / +0x18 | u32 / ptr | count / list of pointers to the transitions leaving the state |

Transition, 0x34 bytes, tag `0x2776`:

| Off | Type | Meaning |
|---|---|---|
| +0 | u16 | tag 0x2776 |
| +2 | s16 | id (modifiable 4 / 5 / 6, script vt 19); negative ids assigned as for states |
| +4 | u32 | stimulus mask that starts it |
| +8 | s8 | "miss" clip played once when this is the nearest transition the value did not reach (`0x6B3DD0`, `0x6B4090`, `0x6A52E0`); -1 in every object |
| +9 | s8 | animation-list index played once; its end enters the target |
| +0xC | f32 | threshold the stimulus value must reach (timer: 30 Hz frames in the state) |
| +0x10 | f32 | > 0: window, the stimulus' running total since the state's entry must reach it instead (only `ca_ei_flame_trap` transition 0, mask 0) |
| +0x14 | ptr | target state; null = the current state again (`0x6A5660`) |
| +0x1C / +0x20 | u32 / ptr | count / list of hit damage classes (message 0x11C `+0x50`) that start it, with mask 0x800; such a transition ignores plain stimuli |

## Stimuli

- [O] `0x1` use: Lara's use of the object (INSTANCE messages 0x166 / 0x168 → `0x6B4090`); buttress mounts. [I] Ours:
  switch reach (200 horizontal) from any bone's bind position (`Runtime::usePoints`) while the state has a use
  transition. The mounts' intros sit on Yggdrasil's trunk axis (x = y = 0, rotated 120° apart); bone 2, 8041 units
  out, is the end at the outer ring where Lara stands.
- [O] `0x800` hits. Message 0x11C (`0x6B4440`; `+0x4C` damage, `+0x50` damage class): if the class is in the state's
  event mask (`+0xA8`, bits of its transitions' class lists, cleared while one runs) `0x6A52E0` draws among its mask
  0x800 transitions listing the class (value = damage). The damage also adds to `+0x98`, posted on the next update as a
  plain 0x800 stimulus (`0x6B4390`). Only `sh_ei_combat_crate_a`-`_d` (l1_ship_exterior, l1_ship_engine_hall) have
  event-gated transitions: classes 13 (pr_thorbolt) and 4 (every projectile hit: pr_spear, pr_tranquilizerdart; 0x6C9670) [O], threshold 0.1, no clip,
  to state 1, whose timer plays clip 1 into state 13. Ours: `Runtime::graphHit` from `World::fire` (the crates get
  dynamics colliders so bullets reach them) [I: the exe flags the clip done (0x100000) instead of entering at once].
- [O] `0x8000` timer, posted every update with the frames spent in the state (`0x6B4390`).
- [O] Also posted by the engine [not modelled]: 0x400 / 0x1400 (messages 0x2B, 0x3B, 0x91), 0x4000 (0x30, modifiable
  1), 0x40000 (0x15D).
- [O] Scripts post their stimulus members' masks (`modifiable` 3, `0x4AC390` → message 0x33; value < 0 = any).
- [O] Engine stimuli first add to the per-bit totals (`0x699D60`: `+0x18 + 4 * bit`, the last one also `+0x14`;
  cleared on entry), which the windows read.
- [O] Selection, all with tru.exe's LCG `0x48CA30` (`DAT_00A038E0 = DAT_00A038E0 * 0x41C64E6D + 0x3039`, bits 16..30
  / 32767 × hi; the seed is .bss, 0 at start; ours `formats::graphRandom`, one seed per script runtime, saved):
  - value ≥ 0 (`0x6B3DD0`): only between transitions; the state's transitions on the mask without class lists;
    reached = max(value − after, window > 0 ? total − window : −10⁶); each reached one replaces the pick with
    probability reached / (sum of reached so far), the first without a draw (`AnimGraph::pick`).
  - value < 0 (`0x6B3BD0`): every transition on the mask, thresholds and class lists ignored, uniform (the k-th
    replaces with probability 1 / k); also while a transition runs, then from its target state (`pickAny`).
  - use (`0x6B4090`): the nearest by |value − after| (after 0 counts 0.1), ties drawn uniformly; it starts only within
    0.1, else the miss clip (`pickUse`; mask 1, [U] 2 while Lara's `+0x68` flag 0x40000 is set).
  - hits (`0x6A52E0`): as value ≥ 0 among the class-listing ones (`pickHit`).
  No state of any object has two transitions on one mask (`ProgressionReal.AnimGraphRecords`), so the draws never
  decide in shipped data. Test: `AnimGraph.LcgAndTransitionPicks`.

## Runtime (ours)

- An intro of `Kind::Scripted` whose script type (or a base) has a `modifiable` or `animated` member gets
  `Object::graph` when its object has a skeleton. [O] The intro's physob data word 15 (`+0x3C`) is the initial state
  (record index; <= 0 the first; `0x6B5890` message 0x23). Its vt 20 is not sent at birth [I: the script is not yet
  attached].
- [I] Switches (`Kind::Switch`, e.g. `ms_ei_switch_oneway_wall`) keep their own use flow and get no `Object::graph`,
  but scripts read their graph state through the `modifiable` member: its field holds the state id, the intro's at
  birth, then each use moves it along the use transition (`Runtime::switchUsed`, `pickUse`) and `modifiable` 4 to the
  started transition's target; `modifiable` 8 returns it. l5_valaskjalf_entryb lever 7188 opens reward door 7187 only
  while switch 7192 is not in its state 0 "on" (7192 starts in state 1).
- Entering a state starts its clip (looping with flag 2), restarts the timer and calls script vt 20 (instance
  prototype 33, `0x799060`) with the state id. Starting a transition plays its clip once and calls vt 19 (prototype
  32, `0x799020`) with its id; without a clip the target is entered at once.
- `modifiable` (bind `0x7AC970`) natives on a graph object: 0 same component (`0x4A52E0`), 1 post 0x30 (two floats;
  not modelled), 2 state flag 0x40, 3 stimulus(mask member, value), 4 start transition by id (0x32), 5 / 6 running
  transition id or -1 (0xE9), 7 enter state by id (0x2E), 8 state id (0xE8). [I] While a transition runs 8 reports its
  target, not the state it left (the exe: component `+4`); l3_chapel plate 4834 lights fire 4818 and checks it in the
  same handler, which the shadow pillars' vt 60 needs.
- `animated` 1(animation {object, list index}) plays that clip once (l4_traprooms plate 5717 → glyph 5724).
- Saved per object: state, transition, frames, clip, time, loop (`graph` in the object's save state); part of
  `World::stateHash`.

## Examples

- l4_ballcourt temple reveal 5464: switch 5469's vt 55 calls 5464 vt 54 "ReopenFromXibalba": modifiable 7 enters
  state 3 (BothSidesOpen), the 0x20000 stimulus starts transition 4 (TimerA) → 5, whose 300-frame timer plays
  TimerB → 6 → TimerB_Done → 7.
- l7_yggdrasil_end buttress mounts 8362-8364: the Natla fight locks them (state 2: modifiable 7 (1), then the Thor
  towers' state-2 event's modifiable 4 (2)); Lara's use (stimulus 1) runs transition 3 "break" → state 3, whose vt 20
  sends buttress_mount_broken and the mount's vt 54 tells dispatcher 8360. The endgame gun 8401 only reacts to broken
  mounts.
