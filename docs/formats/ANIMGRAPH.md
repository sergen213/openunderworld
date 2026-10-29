# TRU object anim state graphs (physical class 6)

Code: `src/formats/AnimGraph.*` (`loadAnimGraph`), `src/script/Script.cpp` (`Runtime::graphSet`, `graphEnter`,
`graphBegin`, `graphStimulus`, `tickGraph`, the `modifiable` / `animated` natives, `Runtime::pose`).
Tests: `ProgressionReal.TempleRevealReopens`, `ProgressionReal.EndgameCredits`, `ProgressionReal.ChapelShadowPillars`,
`ProgressionReal.CriticalGates`.

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

State, 0x28 bytes, tag `0x2714`:

| Off | Type | Meaning |
|---|---|---|
| +0 | u16 | tag 0x2714 |
| +2 | s16 | id (modifiable 7 / 8, script vt 20); negative = lowest id < 32 no other state uses |
| +4 | u16 | flags: 2 loop the clip (else once and hold); 0x40 reported by modifiable 2 (query 0xED) |
| +7 | s8 | animation-list index played on entry, -1 keeps the pose |
| +8 | u32 | message posted to the instance on entry (INSTANCE message 0x1E2) [I: not modelled] |
| +0x14 / +0x18 | u32 / ptr | count / list of pointers to the transitions leaving the state |

Transition, 0x34 bytes, tag `0x2776`:

| Off | Type | Meaning |
|---|---|---|
| +0 | u16 | tag 0x2776 |
| +2 | s16 | id (modifiable 4 / 5 / 6, script vt 19); negative ids assigned as for states |
| +4 | u32 | stimulus mask that starts it |
| +9 | s8 | animation-list index played once; its end enters the target |
| +0xC | f32 | threshold the stimulus value must reach (timer: 30 Hz frames in the state) |
| +0x10 | f32 | second window [U: no route data uses it] |
| +0x14 | ptr | target state; null = the current state again (`0x6A5660`) |
| +0x1C / +0x20 | u32 / ptr | count / list of animation event ids (mask 0x800) |

## Stimuli

- [O] `0x1` use: Lara's use of the object (INSTANCE messages 0x166 / 0x168 → `0x6B4090`); buttress mounts. [I] Ours:
  switch reach (200 horizontal) from any bone's bind position (`Runtime::usePoints`) while the state has a use
  transition. The mounts' intros sit on Yggdrasil's trunk axis (x = y = 0, rotated 120° apart); bone 2, 8041 units
  out, is the end at the outer ring where Lara stands.
- [O] `0x800` animation events (the transition's event ids) [I: event-gated transitions are not started by us].
- [O] `0x8000` timer, posted every update with the frames spent in the state (`0x6B4390`).
- [O] Scripts post their stimulus members' masks (`modifiable` 3, `0x4AC390` → message 0x33; value < 0 = any).
- [O] A stimulus is accepted only between transitions (the accepted mask is cleared while one runs); the state's first
  transition whose mask matches and whose threshold is reached starts. ponytail: the exe picks among several eligible
  ones with its LCG (`0x48CA30`); ours takes the first. No route data has two eligible ones.

## Runtime (ours)

- An intro of `Kind::Scripted` whose script type (or a base) has a `modifiable` or `animated` member gets
  `Object::graph` when its object has a skeleton. [O] The intro's physob data word 15 (`+0x3C`) is the initial state
  (record index; <= 0 the first; `0x6B5890` message 0x23). Its vt 20 is not sent at birth [I: the script is not yet
  attached].
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
