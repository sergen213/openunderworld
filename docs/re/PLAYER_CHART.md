# Lara's player state chart (lara.drm)

## Runtime

How tru.exe runs the state charts stored in Lara's player data (lara.drm ObjectBase +0xC8 data block). Port:
`src/game/Chart.{hpp,cpp}` (loader, `ChartRuntime`, `Registry`, `chart-dump`), `src/game/ChartShadow.cpp` (shadow
mode), `src/game/chart/<Family>.cpp` (conditions / actions, listed in `src/game/chart/Families.cpp`). Tests:
`tests/test_chart.cpp` (`ou_chart_tests`). Evidence tags: [O] read in tru.exe / the data, [I] inferred, [U] open.

### Data layout [O]

All offsets below are from the start of the player data block.

- Two chart headers of 0x28 bytes: **+0x12500** Lara (agent 0, 1445 states / 2374 transitions / 69 events) and
  **+0x12528** the upper body / weapons (agent 1, 103 / 203 / 14). Header: +0 s16 root state, +2 s16 agent index,
  +4 -> linked chart (the upper chart points at Lara's), +8 u16 state count, +0xA u16 transition count, +0xC ->
  states[], +0x10 -> transitions[], +0x14 -> events[] (the event count is the run of pointers; Lara's states reference
  events 0..68), +0x18 / +0x1C the state / transition record pools. Both charts' root is S0.
- State (0x28): +0 s16 id, filled lazily with -index on first entry (0x787890); +2 s16 anim request kind
  (0x444BA0: 0 / 1 / 2, 2 = mirror-aware); +4 s16 anim request (< 0 none); +6 s16 parent; +8 u16 (always 0 in
  lara.drm; 0x781FA0 tests it up the chain while applying properties); +0xA/+0x14 properties; +0xC/+0x18 action ids
  (u16); +0xE/+0x1C transition indices; +0x10/+0x20 event indices.
- Property (8): {u16 id, u16 type, u32 value}. Value readers: bool 0x783770 (type 2 byte, 0x10 int, 0x29 tuning
  record), int 0x783590 (2 byte; 7, 9, 10, 11, 0x10, 0x11, 0x17, 0x1A, 0x1D, 0x23 int; 0xC / 0xD float truncated;
  0x29 record), float 0x783680 (0xC / 0xD float, 0x10 / 0x11 int, 0x29 record). Type 0x29 points at a tuning record
  {u32 type, value} (1 int, 3 float frames, 4 float distance, 5 bool). Type 0x2B takes the linked chart's current
  value of the property it names (0x785B90).
- Transition (0x14): +0 s16 target, +2 u16 flag count, +4 u16 condition count, +8 -> s16 flags, +0xC -> conditions.
- Condition (8): {s16 group, s16 id, s16 value, u8 flags}. `id` selects the function; `value` is its parameter;
  flags bit 0 negates (each condition function applies it itself). The group is only the family number (100 buttons,
  150 action flags, 400 stick, 500 player, 600 markup, ...) except **1000**: then `value` is an index into the chart
  variables (0x7820E0 int: floats truncated, bools 0 / 1; 0x782060 float). Functions that read a mode from `value`
  get 1000 itself (0x781FF0), which no mode case handles.
- Event (0x10): {u32 event id, u8 result when a transition fired, u8 result when none did, u8 record, u16 count,
  -> {s16 transition, s16}[count]}.
- Chart variables: a run of 400 pointers to tuning records, pointed at by data +0x18CC (at run time tru.exe reaches
  the same table through the instance's object, 0x782B40 / 0x7820E0). E.g. variable 344 = 632.11, 347 = 4.0, 193 = 10.
- Anim request = the object's anim pattern of that index (anim::animPatterns; e.g. S1280 request 39 =
  `ng_land_hard`). [I] all three request kinds index the patterns.

### Function registry [O]

Conditions, property handlers and actions live in one splay tree of "SC enum" nodes keyed by s16 id (global tree at
manager 0xFAF030 +8, per-agent override tree at agent +0x520; lookups 0x783F50 / 0x784C50). A node holds: +0x18
condition function, +0x1C property handler, +0x20 Entry, +0x24 Reentry, +0x28 PreAnimEntry, +0x2C PreAnimReentry,
+0x30 Exit, +0x34 Process, +0x38 the property record in force, +0x3C the condition cache {agent, frame, value}, +0xA
an entry/exit balance byte (warnings only). Tables (filled by 0x784EE0 / 0x784F60 / 0x784FE0 for the base,
0x785120 / 0x785390 / 0x785600 per agent, from 0x768B60):

| | conditions {id, group, fn} | property handlers {id, fn} | actions {id, Entry, Reentry, PreAnimEntry, PreAnimReentry, Exit, Process} |
|---|---|---|---|
| base | 0x8B57E0 | 0x8B6650 | 0x8B4200 |
| Lara (agent 0) | 0x8B2CE8 | 0x8B2280 | 0x8B0A50 |
| upper body (agent 1) | 0x8B2838 | 0x8B26E0 | 0x8B1E00 |

An agent table entry copies the base node's functions, then overrides its own. Base entries that are only
placeholders point at 0x782140, which logs "unhandled" and returns **true**; every id Lara's chart uses is overridden
or real. Ours: an id without a registered function is **false** (whatever the negation) and counted
(`ChartRuntime::stats`), so a missing port can never open a transition.

Generic base conditions: 0x1 (0x782700) always true; 0x14 (0x783B70) state time: (int) agent +0x508 >= value
(0x782B40 mode 1; mode 2 is <=, mode 0 ==); 0xC9 (0x783840) AnimationEnded on the end counter +0x24 (mode 0: < 1,
1: > 0, 2: == 0, 3: == 1; it also counts the clip's time having reached its length while the counter is under 2).
Float compares go through 0x783BD0 (mode 1 >=, 2 <=, 0 within epsilon 0x8454C4).

### Agent and tick [O]

Agent (0x7873C0; Lara's at player +0xF10, the upper chart's at +0xF14): +4 -> chart header, +8 instance, +0x24 anim
end counter, +0x500 active, +0x504 total time, +0x508 **state time**, +0x50C frame counter (condition caches are
valid while it is unchanged), +0x510 current state, +0x514 queued transition, +0x518 last transition, +0x51C cleared
on every change [U], +0x520 agent enum tree, +0x524 / +0x528 linked child / parent agents, +0x53C defers flag-7
transitions [U what sets it], +0x70 / +0x74 the move-dir / move-speed conditions' last zones.

- **Per game frame** (0x768C50 ticks agent 0 then agent 1; 0x787E60): if active, +0x504 and +0x508 += timeMult
  (30 Hz frames: 0.5 per 60 Hz frame), the end counter steps away from zero (< 1: -1, else +1), Process of every
  action of the state's chain (parent first, 0x7857D0), then the evaluation 0x787C50. The chart therefore evaluates
  every game frame, with times in 30 Hz frames. [U] where 0x768C50 sits in the frame relative to the anim and
  physics updates (it is reached through a pointer, 0x85E1D0).
- **Start** (0x787E00): current = root, active, properties applied, anim request, Entry of the root chain's
  actions (no PreAnim pass), then one evaluation.
- **Evaluation** (0x787C50): frame counter + 1; find a transition (0x787570) and change (0x787890); after a change
  to a new state the search repeats in the same frame only while property **0x65** reads true (each repeat bumps the
  frame counter).
- **Finding** (0x787570): first the queued transition, if it is flagged 1 and the end counter is exactly 1, or
  flagged 7 and +0x53C is clear. Then the state's **own transitions in stored order; the first whose conditions all
  hold wins** (0x785920 evaluates the conditions in order and stops at the first false one). Only if none holds,
  the parent's (recursively up to the top). A winner flagged 1 while the counter is not 1, or flagged 7 while
  +0x53C is set, is queued (+0x514) instead and nothing fires.
- **Change** (0x787890): flag 6 takes the queued transition instead. Target < 0 keeps the current state. Same state:
  a positive end counter is reset to 0, and without flag 4 nothing else happens (no re-entry; last / queued updated).
  Otherwise, in order: Exit of the old chain's actions (parent first) that the new chain does not also run
  (0x7859A0 / 0x781DC0); current = target; flag 2 sets the instance mirror bit (+0xF4 0x20), flag 5 clears it;
  properties (below); PreAnimEntry for actions new to the chain, PreAnimReentry for the others (0x785AE0); the anim
  request plays and the end counter becomes 0 (0x783470); Entry / Reentry likewise (0x785A30); state time 0, queue
  cleared, last transition = this one.
- **Properties** (0x785E70 -> 0x785CC0): the new state's properties, then its parent's, up the chain, then (if the
  chain's top is not the root) the root's chain; a pass stamp lets each id be applied **once per pass, so the most
  derived state wins**. Each applied property runs its handler (+0x1C) and becomes the id's value in force. Ids none
  of them sets keep their old value.
- **Animation end** (0x768CF0, from the anim controller's notifications 0x7181A0 / 0x718520 reasons 1 / 2 / 8 and
  anim event command 0x72 in 0x716F30): if the end counter is negative it becomes 1, the evaluation runs at once
  (flag-1 transitions and the queued one can fire now), and if the state time is above 0 event **0x15** is posted.
  The counter then grows each frame, so flag-1 transitions are a one-frame window.
- **Events** (0x787CD0 posting, 0x7876E0 handling): frame counter + 1; the state's handlers of the event id (else the
  nearest ancestor's, else the root chain's) in order, each handler's transitions in order; the first that holds
  wins. A winner flagged 1 or 7 is queued. The result byte of the handler decides whether the post counts as
  handled. A fired transition changes state, with the same property-0x65 chaining as the evaluation. Event ids in
  Lara's chart: 21 (0x15), 64, 133, 145, 159, 235, 262, 269, 274, 284, 455, 457, 458, 466, 468, 469, 470.

### Input timing [O]

Lara's conditions 0x65..0x6E (0x760A30..0x760B30) each read one input action through 0x7607C0: 0x65 action 52
(Jump), 0x66 50 (Crouch), 0x67 42 (Sprint), 0x68 / 0x69 99 (Interact), 0x6A 112, 0x6B 34, 0x6C 25 (Manual Aim),
0x6D 28, 0x6E 79. Value: 1 on the press frame, frames held + 1 while down, -1 on the release frame, -1 - frames
while up (frames = ms x 30 / 1000, at least 1); mode (table 0x7609C0) 0 up, 1 down, 2 released, 3 pressed, 0x65 held
longer than player data +0x6E2 (15). Conditions 0x96 / 0x97 / 0x98 (= 0x9E) / 0x99 (0x760B50..0x760CB0) test the
flags of the action `value` names: down, not down, pressed, released. The stick level 0x191 (0x761CD0 -> 0x786AF0)
puts the stick magnitude (player globals 0xFADFB0 -> +0xB0 -> +0x18 [I: 0..1]) in zones 1 / 2 / 4 / 8 by the
thresholds of properties 0x20A / 0x20B / 0x20C (Lara: 0.10 / 0.70 / 0.91), raised by the hysteresis 0x20D / 0x20E /
0x20F (0.02 each) above the last frame's zone and lowered below it; values 401..404 test zones 1 / 2 / 4 / 8 (405
"sprint" tests 8). Fall 0x1E (0x764B40): player +0x6DC - z >= value (float compare).

### Ours

- `ChartRuntime` follows the order above: `tick` = 0x787E60, `evaluate` = 0x787C50, `find` = 0x787570, `holds` =
  0x785920, `change` = 0x787890, `post` = 0x787CD0, `animEnded` = 0x768CF0. The requested clip's clock is kept by the
  runtime itself (pattern start frame, rate, length from the clip header) and calls `animEnded` when a one-shot clip
  reaches its end or a loop wraps [I: notification order].
- Generic conditions ported (`src/game/chart/Generic.cpp`): 0x1, 0x14, 0xC9, 0x65..0x6E, 0x96..0x99 / 0x9E, 0x1E,
  0x191. Everything else waits for its family.
- Porting a family: add `src/game/chart/<Family>.cpp` with `void register<Family>(Registry& player, Registry& upper)`
  and one line in `Families.cpp`. A condition is `bool fn(ChartContext&, const Cond&)` returning the raw truth (the
  runtime negates); it reads the agent through `ctx.chart()` (`stateTime()`, `value(c)` / `valueF(c)` with the
  group-1000 variables, `propBool/Int/Float(id)` = the property in force, `cache(id)` for per-frame caches,
  `field(offset)` for agent scratch such as +0x74, `animCounter()`, `animFrames()`, `charts().data<T>(offset)` for
  player data) and the game through `ChartContext` (`action(id)`, `stickLevel()`, `stick()`, `fallDistance()`,
  `player()`, `world()`); when it cannot answer it returns `ctx.chart().unported(c)`. Actions register
  `ActionFns{entry, reentry, preEntry, preReentry, exit, process}` as `void fn(ChartContext&, uint16_t id)`;
  property handlers `void fn(ChartContext&, const Property&)`. Lara's registry is `Registry::player()`, the upper
  chart's `Registry::upper()`.
- `ou-game chart-dump [--state N] [--coverage] [--upper] [--names conditions.json]` prints states, transitions
  (clip names, flags, conditions with `?` marking unported ids) and the coverage of conditions / actions over the
  states reachable from S0 (own, inherited and event transitions).

### Shadow mode

`ou-game ... --chart-shadow` runs Lara's chart after every `World::tick` with the same `Actions` and world (input
action timings kept by the shadow; timeMult = 30 x the fixed tick, ignoring the combat time scale [I]) and logs each
run where the chart's requested clip differs from the controller's clip; the summary at `--frames` end lists the
most evaluated unported conditions. `--chart-trace FILE` writes the shared TSV (`tick state parent request stateTicks
px py pz vx vy vz yaw inputs`, then `t30=` the chart state time and `var_anim=` the chart clip clock): stateTicks =
60 Hz ticks since entering the state (0 on the entry tick, also on a re-entry), position / velocity (units/s) / yaw
(degrees) are ours, `inputs` is the hex mask of the same tick's actions (1 Fwd, 2 Back, 4 Left, 8 Right, 0x10 Jump,
0x20 Interact, 0x40 Crouch, 0x80 Sprint, 0x100 Walk, 0x200 Grapple).

Open [U]: what sets agent +0x53C; the frame order of 0x768C50 against animation and physics; the exact notification
that ends a looping clip; state +8; field +0x51C; property type 0x2B between linked charts (ours ignores it).
