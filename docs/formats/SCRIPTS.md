# TRU scripts, trigger volumes, switches and doors

Code: `src/script` (`ou_script`): `parseScriptType`, `functionTargets`, `parseShape`, `inside`/`crosses`, `Runtime`.
ou-game: `World::script()` ticks it after Lara moves; `--show-triggers` draws volumes and planes; script events are
logged to stderr (`ou-game: tick N script <event> <uid> (<object>) -> <uid> (<object>)`).

Labels: **[O]** observed in game data or in our tru.exe decompile (GOG 1.1.0.0 VAs); **[I]** inference; **[U]**
unknown. Section ids below are DRM section ids; script type ids are global (the same id names the same type in
every DRM that carries it).

## No per-level native event code

- [O] cdcResearch `level/event.md`: earlier cdc games stored level events as native PE code (`EventMain`) in the
  unit DRM, and "the event system was removed in Tomb Raider: Underworld". Confirmed: none of the 82 unit DRMs
  contains a PE image or the string `EventMain`, and tru.exe has no `EventMain`/`EventRelocate` string.
- [O] TRU uses the cdc script system instead (as DXHR): bytecode **script types** in DRM sections of type 8,
  run by a VM in the exe (`ScriptExec`, "Script callstack overflowed", "Branch occurred before
  ScriptObject::StartObject() called!") plus **native script types**: 187 RTTI classes `*_ScriptType`, of which
  the gameplay bases are `NsInstance`, `NsUnit`, `NsTriggerVolumeBase`, `NsTriggerPlaneBase`, `NsUberObjectBase`,
  `NsDispatcherBase`, `NsCheckpointBase`, `NsWaterVolumeBase`, `NsLaraBase`, … The native types are registered by
  name in `FUN_007a8650` ("triggervolumebase", "uberobjectbase", "uberobjectcommand", …).
- [O] Package paths of the stock types: `game/script/game/game`, `game/script/core/core`,
  `game/script/camera/camera`, `game/script/core/patch01`. No member, function or type *names* are stored for
  bytecode types on PC: names are 16-bit symbol ids.

## Section type 8: ScriptTypeStreamData

Offsets from the section start. Pointers are DRM relocations (`cdc::sectionPointers`); "ref" = type-5 resource
relocation whose id stays in the data.

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | u32 version = 4 | [O] all 20 568 type-8 sections of the 82 units; exe "Invalid script type version" |
| +0x08 | u16 ×2 script name (symbol ids) | [O] shape; meaning as DXHR `ScriptName` [I] |
| +0x10 | `char*` native script name, null for bytecode-only types | [O] "triggervolumebase", "instance", … |
| +0x14 | ref: own section id | [O] |
| +0x18 | ref: super type section id | [O] e.g. triggervolume 3484 → triggervolumebase 3515 |
| +0x1C | u16 instance size, u16 alignment | [O] |
| +0x20 | SArray states | [O] pointer; layout [U] |
| +0x2C | SArray members, 0x14 bytes each | [O] |
| +0x34 | SArray prototypes, 0x1C bytes each, +6 = i16 vtIndex | [O] |
| +0x38 | SArray functions, 0x1C bytes each | [O] |
| +0x44 | vtables pointer | [O] pointer; layout [U] |
| +0x48 | SArray dependencies (refs to other script types) | [O] |

An SArray is a pointer to the elements with the element count stored in the u32 just before them [O].

- **Member** (0x14): `+0 u8 DataType` (low nibble `DTVOID BOOL INT FLOAT STRING STATE ARRAY DYNARRAY MAP UNKNOWN
  NATIVE SCRIPT STRUCT STRUCT_REF ENUM NTENUM`, as DXHR; bit 0x80 = compound `A<B>`), `+8` ref to the native/script
  type (compound: pointer to `{ref A, DataType* B}`), `+0xC u16 offset` in the instance, `+0xE u16 name` symbol,
  `+0x10` pointer to the initial value. NATIVE/SCRIPT initial values point at a pointer to the value; scalars point
  at the value [O]. An `instanceref` member (type id 51) has the **unique id of the target intro** as its value [O].
- **Function** (0x1C): `+0 Prototype*`, `+0x18` SArray of u32 bytecode words [O]. The prototype's vtIndex
  (+6) is the virtual slot; −1 = non-virtual helper [O].
- **Bytecode** [O]: one u32 per instruction, opcode in the top byte with the cdc `SCMD_*` numbering
  (`cdcEngineDXHR/cdcScript/ByteCodeEnum.h`, 0x00 NOP … 0x85 MOVEP), signed 24-bit argument. Operand words follow
  the opcode and are marked by relocations: CALLU/CALL1–5 point at a Function or Prototype, PUSHC at a constant
  (e.g. the debug string " OnFirstIn();"), ISTYPE carries a type ref; PUSH (0x02) takes the next raw word (e.g.
  0x3DCCCCCD = 0.1f). NOP's argument is a statement number. `parseScriptType` decodes every function of all
  20 568 sections with no opcode ≥ 0x86 (test `ScriptRealData.AllUnitsParse`).

## Intros → scripts

- [O] Intro `+0x7C` u32 = section id of the intro's **script type** in the same unit DRM: 6 217 of 8 628 intros
  in the 82 units have one and every id resolves to a type-8 section of that unit; the others are 0.
- [O] The intro script is a per-instance subclass of the object's script (super = e.g. triggervolume 3484, the
  switch's object script), adding members. Its instanceref members are the links between intros. Examples:
  - l3_main_bedroom switch 4525 (`ma_ei_wall_switch_electric`, script 12131): member +0x24 → 4520
    `ma_ei_house_door`, +0x2C → 4086 (another unit). Its only function (vt 55) gets +0x24, casts it to the door's
    intro script 12126 and calls its function 0, which calls door base script 12171 function 0:
    `stickybool(+8).Set(true); modifiable(+4).Set(0)`.
  - l1_kraken_room switch 498 → 497 `ms_ani_tombdoor`; the door's base script 4743 function 0 is
    `uberobjectbase.SendCommand(uberobjectcommand +0x18 = 176)`. tru.exe 0x4AB760 forwards the command to the
    object's C++ uber-object component (vtable call).
  - l4_ballcourt: pole lever 6502 → door 6505, pole 6503, volume 5480; volume 6519 → block 6504, lever 6502.
- [O] Intro `+0x78` → intro data (DTP), the per-object init block; trigger shapes live there (below).

## Trigger volumes and planes

[O] `INSTANCE` birth of a volume registers it (`0x5C89E0`, at most 0x200); `0x5CD520` tests each one against the
player every frame:

- **Volume** test `0x5CD1B0(player, volume)`: shape data = `*(instance+0x100)+0x78` (the intro data).
  `data+0x30` u8 type: **1 = box**, half extents `data+0x34/+0x38/+0x3C` in the volume's rotated frame
  (`0x46B730`: |local| ≤ half on every axis); **2 = sphere**, radius `data+0x40` (squared distance < r²). Other
  types never fire. The tested point is the player position **+ (0, 0, 200)**, for both the previous and current
  position; result 2 = entered, 3 = left, 1 = still inside. All l4_pool volumes are boxes; `data+0x40` = 150 in
  them (unused for boxes).
- **Plane** test `0x5CCC20`: a rectangle in the plane's local XZ plane, half size `data+0x34` × `data+0x3C`
  (`+0x38` = 100, unused), crossed by the segment from the previous to the current test point (two triangle
  tests, `0x461640`). `data+0x44 == 0` flips the segment when it runs along the normal, i.e. both directions
  count; otherwise one-sided [O]. Which side counts for one-sided planes [I]: we count local +Y → −Y.
- [O] On enter/leave the exe calls the volume script's virtual functions at the slots read from the native
  type's prototypes 6 and 7 (`0x7B63A0`, `0x79CC80`/`0x79CCC0`). The base script 3484 (triggervolume.drm)
  overrides them as vt 54 **Enter**: `if !firstIn.Get(): firstIn.Set(true); OnFirstIn()`; `inside = 1`; `OnIn()`
  and vt 55 **Exit**: `inside = 0; OnOut()`, with empty `OnIn` 70, `OnOut` 71, `OnFirstIn` 72 that intro scripts
  override (297 / 137 / 219 overrides in the 82 units).
- [I] Planes call their script at vt 54 (296 of the plane intro scripts override it). [U] the native slot.

## Doors, switches, uber objects

- [O] Switch objects (`*_ei_switch_*`, `ma_ei_wall_switch_electric`) share the clips `inactive_idle`,
  `active_pulling`, `active_idle`, `inactive_reset`, and Lara clips `obj_pull_lever`, `obj_pull_fail`. Their
  intro scripts override vt 55 in most units [O]; that vt 55 is the "switch used" handler is [I].
- [O] Door objects carry their own clips, e.g. `ms_ani_tombdoor`: `idle_door_closed`, `doors_opening`,
  `idle_door_open`; `ma_ei_house_door`: `0_idle`, `10_opening`, `20_idle_open`, `30_closing`; `sa_uo_flush_gate`:
  `gate_closed`, `gate_opening`, `gate_open`.
- [O] Switch reset: the object's physical-class record (ObjectBase +0xC8) +0x34 points at its state records; the
  first has flags at +4 (bit 0 = one-way: `ms_ei_switch_oneway_wall`, `ma_ei_wall_switch_electric`,
  `ar_ei_switch_oneway`) and a reset delay at +0xC (`ma_ei_switch_wall_chapel` 1, `ca_ei_switch_wall_autoreset` 60,
  `ms_ei_resetting_switch` 150; [I] frames at 30 per second). Resetting switches return to idle after it and can be used
  again (`Object::resetAfter`).

### Uber objects

[O] The class-79 record (ObjectBase +0xC8, `formats::loadUberObject`): +8/+0xC sections (0x54 each), +0x10/+0x14
commands (0xC each), +0x18/+0x1C events (0x1C each). Every section is a state machine run by its component
(tru.exe 0x4D7550): +0x78 → its record, +0x84 current state (−1 while in a transition), +0x80 running transition,
+0x88 seconds in the state, +0x60 bits of events already fired.

| record | fields |
|---|---|
| section (0x54) | +4/+8 states, +0xC/+0x10 transitions, +0x14 default state, +0x18 model, +0x28 float angle (0x4D3050) |
| state (0x1C) | +0/+4 floats, +8 name, +0xC/+0x10 transitions it leaves by (pointers), +0x14/+0x18 entry actions (0x10 each: type, 3 words; 0x4D86A0: 2/3 play an animation, 9/10 create/remove a physics joint, 12/13 its limit/motor, 0x14/0x15 add/remove a range watch; PHYSICS.md "Uber joints") |
| transition (0x38) | +0 from, +4 to, +8 float, +0x18 trigger, +0x1C parameter, +0x20/+0x24 joint and range records (triggers 14/15), +0x2A animation-list index (0xFFFF none), +0x2C probability, +0x30 effect, +0x34 flags (1 play the animation, 2 wait for its end, 0x10 random) |
| command (0xC) | +0 id (0 in every object seen), +4/+8 targets {section, type, payload} |
| event (0x1C) | +4/+8 condition lists, +0xC section, +0x10 kind, +0x14 value, +0x18 once, +0x1A script event id |
| condition list | +4/+8 entries {section, kind, value}: kind 1 = section in state `value`, 2 = section missing (0x4D1360) |

- Commands (0x4D77A0): the command member's word (`uberobjectcommand` {object DTP id, index}) indexes the table; each
  target (0x4D7680): type 1 sets the section's state, 2 starts the current state's transition whose trigger equals the
  payload (0x4D4BE0), 3 / 4 the one with trigger 0xD / 0x10 and that parameter. A section in a transition ignores
  them.
- Automatic transitions (0x4D4B60 → 0x4D31E0): trigger 1 after `parameter` seconds in the state, 3 at once, 9 when
  its condition list holds, 0xB/0xC on the section's facing flags (not modelled). 14 / 15 are a physics joint's
  value entering / leaving a watched range (0x4D7380 → 0x4937A0 → 0x4D5A50), from the simulated joints
  (docs/formats/PHYSICS.md "Uber joints").
- A transition with flags 1 + 2 and an animation lasts the clip; then the target state is entered (0x4D7440).
  Events: kind 2 leaving a state, 3 starting a transition (0x4D4A00), 4 finishing one, 1 entering a state; a match
  (all condition lists holding) runs the object script's vt 54 with the event index, which `uberobjectevent` 0
  compares with its member's index (CALLU: the event is the receiver, the member the argument).
- l3_chapel shadow pillars `ma_uo_shadow_pillars` (4837): sections 0-3 are pillars turning a → b → c → d (trigger 2),
  rising from "lowered" into a, c, b, a; section 4 is the solver (conditions "sections 1 and 3 at a" → left solved,
  "0 and 2" → right solved, all four → solved); its events 2 / 3 / 0 are the script's members +44 / +56 / +68. The fire
  4818 (lit by plate 4834) calls the pillars' vt 60, which opens shadow door 4835 when both halves are solved.
- [O] Rigid multi-bone models (doors, switches) have skin indices but no weights in their vertex declaration;
  palette e.g. {0, 1} for the house door's 2-bone skeleton. [I] each vertex follows the bone of index byte 0
  (`formats::readSkin`); confirmed visually by the house door opening on its hinge bone.

## Runtime (`script::Runtime`) — what is implemented

The runtime executes handlers through `script::vm` and retains static-link/native C++ fallbacks for unsupported paths:

## Bytecode VM (`script::vm`)

- [O] The full interpreter is `tru.exe 0x57CE50` (not `FUN_00432B20`, which is only a state-chart condition
  evaluator). `src/script/Vm.cpp` ports its common scalar core: constants/local/member loads and addresses,
  scalar/reference/string moves, typed native `MOVEU` for the eight observed 4/8/20-byte layouts, integer/float/bool/bit arithmetic and conversions, string comparison and numeric
  conversions, raw-code-word JMP/JMPF, ISTYPE/STATE, CALLU/CALL1–5, SSEQU/SSEQ1, START and RET. Calls are bounded to
  100 frames / 100 000 instructions per handler.
- [O] Stickies (28-byte members: key = owner instance uid + sticky id from the member init, 0x7B3450) live in a
  global store; Set (bool 0x7B6200 one bit, int 0x7B7B50) always notifies the registered owner (0x4AF6D0 / 0x4AF740 ->
  0x4AF5C0 -> 0x7981F0): corebase vt 5 float / 6 bool / 7 int (ids 0x13A / 0x156 / 0x172 of the corebase table) with
  (the sticky, the new value); the owner slot is cleared during the call, so its own handler's Sets do not re-notify.
  Equality (3, 0x7B2940) compares keys, not values. Ours delivers the notifications after the running machine step
  (`Runtime::adopt`) [I: the exe calls them synchronously inside Set]. l4_traprooms plates 5717 / 5718 count on seal
  5701 from their vt 6; the seal's vt 7 opens door 5702 when sticky int +240 (trap rooms done) reaches 4.
- [O] Function records now retain their own address, prototype, argument/local counts, return type and native
  flag; branches use raw word offsets, including relocated operand words. Inheritance and virtual calls resolve
  against the actual linked instance.
- [O] Native bindings implemented by behavior observed in the scripts: `playerref.Get` (0) and loaded wait (2),
  `instanceref` loaded/birthed waits (3/6), Get (7), SetProperty (9), GetProperty (12/15), `unitref.Get` (3),
  `globalsref.Get` (0), sticky bool/int/float Set (0: notifies the sticky's owner, below), Get (1/2) and identity equality (3), `modifiable` Set/Get (4/8) and its two-argument setter (3), native
  instance debug/marker/modifiable interfaces (0/1, 4–9, 14–16, 28/61), Reset (25), engagement-property selection (59), Hide (60), `playersensor` void interface call (2),
  `checkpointbase` activation (3), static-water reset/depth (3/4), `terraingroup` SetEnabled/IsEnabled (2/3), `light` SetEnabled/IsEnabled (1/2),
  `health` current-value Get (9), `gamebase` global-byte Get/Set/Add/Subtract (4–7), `unit` post-effect scalar Set (15), and `multibody` Enable (2), SetJointLimit (77),
  GetJointValue (78) and JointDrive (79). `messagechannel` Send (2) delivers channel (named: member init {mode, name}, 0x4A3680; listeners of mode 1/2, mode 3 only its own posts, 0x5D0670), message id and string payload
  to the registered native subscriber; channel equality is fn 3 and fn 0/1 are the original debug stubs.
  `playerref.Get` returns Lara's actual type-205 script from lara.drm. The bedroom path
  4525 → 12131 → 4520 → 12126 → 12171 runs its real bytecode: `stickybool(+8).Set(true)` and
  `modifiable(+4).Set(0)`. The ballcourt lever's polling loop drives its door at twice the lever angle.
  Unsupported bindings stop execution and use the old static-link fallback unless the VM already activated something.
- [O] Uber objects run their section state machines (see "Uber objects" above); command targets drive the
  associated hinge and moving collision from actual travel clips, including translation and axis-angle rotation. Unit
  constructors start after all intros are registered, so forward `instanceref` links (including the l2 elevator chain)
  resolve. The l1 kraken tomb door, l2 `sa_uo_elevator_adrenaline`, l4 `ca_rb_hallway_ramps_b` and the l3 chapel
  pillars are covered by real-data tests.
- [O] A script function with a return value writes it through local 0, which holds the caller's result address
  (l3_chapel 12855 vt 54: `PUSHL32 0 … MOVE8`); PUSHL32 of a local holding an address pushes the address.
- [I] Unit/global script objects live for the runtime; sound/music/UI natives on the proven bedroom path return
  zero without new side effects. Function 0 runs as the constructor when its slot is −1 or 20; function 2 starts
  when it is a WAIT-based multibody polling loop. START starts a fresh fiber at the first instruction at or beyond
  the next statement boundary, with clean locals/stack; its caller resumes when it returns. SLEEP/WAIT retain the
  nested frame stack and resume on later 60 Hz ticks;
  sequential calls replace the parent handler. Dirty `scriptFields` and `scriptStrings` persist in saves; active
  sleep frames do not. The 256 `gamebase` global bytes persist as `gameValues`; script-controlled light enable states
  version the streamed render scene so disabled unit lights are removed and re-enabled lights return.
  Collections, arrays/maps, object construction, other thread/PSEQ fibers and most native types
  are not implemented.
- [O] Function local records (+0x0C, DataMember like members, DXHR `cdc::Function::m_locals`) carry initial values;
  ours applies those of `symbol` locals (property names, e.g. 10814). PUSHL32 of a local holding a string keeps it a
  string (unit hint handlers compare/store their argument). `instanceref` natives accept a ref held as an object value
  (`instance` 14/15 return the intro id, 0x4AB400). `uiexperienceadjustmentmenu` 0–18 read 0 (fns 13–18 read the
  options block [0xC59264]+0x3EF94..; no such menu in ours) [I]. [O] The exe's CALL2/CALL3 (cases 0x66/0x67 of
  0x57CE50) look the callee up by vtable slot (virtual on `this`); ours keeps them direct because the virtual form
  stops l3 switch 4525 at a null CALL5 (test BedroomSwitchOpensHouseDoor) [U].
- cdcEngineDXHR/cdcScript/ByteCodeEnum.h supplies the opcode numbering; no interpreter exists in the research
  clones. No Wine process is needed for this implementation.
- [O] Interpreter details read from 0x57CE50 (switch on opcode − 1): **SLEEP 0x7C** pops nothing and yields until
  the next frame (only WAIT 0x80 takes seconds); **STATE 0x7D** pops a state index and `0x57C830` ends the object's
  threads and enters that state: `0x57B560` runs entry n of the type's state table (type section +0x40, count u8
  +0x3E; inherited from the nearest super type that has one) as the object's new thread. Ours: in place when the
  machine belongs to that object, else as a separate runtime thread beside the caller, which resumes at once; the
  object's other threads end. Each event now runs as its own thread (a sleeping earlier handler keeps running).
  State 0 (entered by Run) is the startup function when the type has a state table (`Program::startup`); the older
  "function 0 with slot −1/20" rule remains for types without one. The old rule had run the l2 elevator switch's vt 20
  at construction (elevator moved without a pull); a switch Use now calls vt 20 (argument 0) when the script overrides
  vt 20 below the type defining vt 55 [I: state value] (switch base 3637: vt 20 dispatches state m+16 → vt 54,
  m+20 → vt 55; those members are set natively [U]). Startup now reaches l2 `sa_uo_naga_statue` 2794's `instance` 21 (unbound). PUSHM*/PUSHMA
  on a null object push 0 and MOVE through a null address is skipped (no failure). Native receivers held in a
  local (e.g. `PUSHLA 0` for `this`) are dereferenced before the call.
- [O] Native function tables: each `*_ScriptType` vtable slot 3 is a bind function calling `FUN_005768f0(index,
  thunk)` per native function (rtti.tsv slots). Added bindings, by the thunks' targets:
  `instanceref` Get (7) returns null when unbound (0x4AC070) and WaitUntilLoaded/Birthed (3/6) **block** — the VM
  retries the call each tick (`NativeContext::wait`) until the target's unit is loaded; `clock` 0–10 (Start/Stop/
  Pause/Resume/IsRunning/GetTime/GetAlarm/SetAlarm/same-clock, 0x4A3450..0x4A34F0, advanced per tick as 0x4A3960;
  reaching the alarm fires once and calls the owner's corebase prototype-25 slot with the clock, 0x7961A0); `globaleffect` FadeOut/FadeIn (5/6, 0x4A4EB0/0x4A4EF0 screen wipe → `Runtime::screenFade`,
  not rendered yet); `larabase` 59/60 post messages 0x1D5/0x1D6 to Lara (`EventKind::PlayerMessage`, meaning [U]);
  `markup` SetEnabled/IsEnabled (0/1, 0x4A56B0: box flag bit 31) → `World` keeps disabled (unit, markup id) pairs and
  gives that markup type 0xFF so no traversal query matches it; `triggervolumebase` Add/Remove instance (0/1) accept
  Lara or null (ours only test Lara), `triggerplanebase` 0/1 are empty in the exe; `health` 1/2 on Lara's health
  (Post 0xD / hit 0x11C, scripts pass 12345 or 1000: death volumes/planes) damage her [I: absolute / given type];
  `instance` Kill (10, 0x5B5740 dead flag) hides the object; `watervolumebase` reset (3) takes no argument;
  `messagechannel` Send always succeeds (unconsumed posts are no-ops in the exe too).
- [O] More bindings: `enemybase` (bind 0x7B4060) 16/17 Set/Get the AI behaviour index (0x776F10 → 0x62DE00),
  22 selects list entry n and posts 0xE1 (0x777010 → 0x63AC40), 26 the MoveNav animation variant (0x7771E0, field
  0x37C0 read by 0x61B0C0) — ours keep the values (`World::enemyValues_`), their AI effect is [U];
  27 GuardNode(unitref, noderef, r1, r2) (0x777210 → 0x004AAF20 → 0x5C8EA0 → 0x61F7A0): the noderef is a type-57
  native member {record, node} (its declared value; Script passes it to the World as record << 16 | node) naming a
  planner node of the unit (LEVELS.md "Planner nodes") → `ai::Enemies::guard` (GAMEPLAY.md "Mercs");
  `generatorbase` (0x7B4B60) 1/2 Spawn, 3 Enable / 4 Disable (generator flag 0x84 bit 0, read by the update 0x781510),
  5 kill spawns, 6 spawn count → `ai::Enemies::generatorNative`; `health` 1/2 on an enemy's own script damage it;
  `modifiable` 7 (Post 0x2E: enter an anim graph state, docs/formats/ANIMGRAPH.md); `uigamedata` 1 (0x60D630 returns 0) and 14 (a UI byte).
  `uberobjectbase` 1 takes the command member as its argument (thunk 0x79D070); a command member is {object id,
  command word}. Uber command targets of type 0 are no-ops (switch 0x4D7680), which the l1 Kraken tentacles use.
- [O] A caller that discards a return value pushes no result address (l3_chapel soundvolumes calling
  `globals.member12` vt 19); the VM only consumes an address-kind slot.

### Survey (`OU_SCRIPT_SURVEY=1 ou_game_tests --gtest_filter=GameReal.ScriptSurvey`)

Static: bytecode of every distinct script type of the 82 units, 362 distinct native functions called; opcodes the VM lacks:
DREF/BOUNDS 0x1D–0x22 (57 sites), TSSTART/TPSTART/TEND 0x78–0x7A (4 sites). Dynamic: every unit loaded as a streaming
`World` (natives bound), every volume entered, plane crossed, switch used, 300 ticks; handler failures counted as
(start unit, object, reason) triples:

| | Failures | Top reasons |
|---|---|---|
| Before (HEAD c3c69f2) | 1 390 | markup 0 (546), instanceref Get on unloaded target (232), SLEEP with empty stack (98), CALL5 (39), MOVE32 (34), music 2 (37, receiver in a local) |
| After | 404 | enemybase 26/16/22 (80, AI), CALL5 0x6A (36, l3_chapel globals member 12 is not modelled), tentacle uber commands (31: `formats::loadUberObject` rejects their target types), spline 5 (27, l7 `natla_end_mover`), generatorbase 3/4 (22), uigamedata 1 (16), modifiable 7 (16) |
| After states/enemy natives | 314 | playersensor 0 (28), spline 5 (27, l7 `natla_end_mover`: intro +0x68 multispline not decoded), CALL5 0x6A (24, l5 soundvolumes), larabase 56 (20), generatorbase 3 (20: surveyed Worlds have no enemy bigfile, so no generators), modifiable 1 (15). Gone: enemybase 16/22/26, tentacle uber commands, l3_chapel 0x6A, uigamedata 1, modifiable 7, enemy health 1. State threads now run code the previous survey never reached. |
| Baseline 2026-10-29 (state 0 startup reaches more code) | 472 | playersensor 0 (72), instance 27 (39), spline 5 (27), CALL5 0x6A (24), instance 21 (23), larabase 56 (20), generatorbase 3 (20), multibody 1 (18), modifiable 1 (15), instance 18 (15), unitref 2 (14), larabase 55 (14) |
| After spline movers, argument offsets, sensor/instance/larabase natives, array/thread opcodes | 265 | instance 27 (39, mercs: returns a 12-byte unitref value), CALL5 0x6A (24: globals member 12 is an engine-created object of type 74 that we do not create, so the call dispatches on the globals object), generatorbase 3 (20, survey artifact), multibody 1 (18), autosplinecamera 0 (20, reveal cameras now reached), uberobjectbase 1 (11), plannerobstacle 1 (11). Gone: spline (all), playersensor 0/1, instance 18/21/23, larabase 25/27/29/55/56, modifiable 1, unitref 2. Static: DREF/BOUNDS and TSSTART/TPSTART/TEND now execute (no unsupported opcode left). |
| After reveal cameras, globals engine objects, instance 27, clock alarms | 183 | generatorbase 3 (20, survey artifact), multibody 1 (18), uberobjectbase 1 (11), plannerobstacle 1 (11), gamebase 1 (10), generatorbase 4 (9), triggervolumebase 0 (8), camerashake 0 (8). Gone: autosplinecamera 0, CALL5 0x6A, instance 27, microphone 1, gamebase 23/24/30/52, enemybase 18/27, clock 10. |

- [O] **Arguments** live in the callee's locals at the byte offsets of the prototype's argument records (prototype
  +0x0C: 0x14-byte records, u16 offset +0x0C), from 0, in declaration order, without a `this` local. Before this
  fix our VM put argument i at local i+1, so every `PUSHL 0` of an argument read the object's own uid (e.g.
  `Lara.SetControlEnabledForReveal(bool)`, natla_end_mover `PlayFromKey(key)`).
- [O] **Arrays** (0x57CE50 cases 0x1C-0x21): DREF32 / DREFA index an element (operand = element size), BOUNDS n
  nulls the address for index >= n, DREFD / DBOUNDS read a dynamic array's {data +0x20, count +0x18}. Ours: static
  arrays address members/locals by offset; dynamic arrays are not modelled and read as empty [I].
- [I] **Threads** TSSTART / TPSTART n / TEND (cases 0x77-0x79: a new exec context continues at the next word, the
  parent skips n words; TEND parks the thread) run their block inline (only l7_yggdrasil_end type 18627 uses them).
- [O] **Splines** (`spline` bind 0x7AD0D0): 0 last key, 1 current key, 2 GoToKey, 3 mode, 4 SetPlayMode(mode,
  from, to), 5 Play / 6 Stop / 7 IsPlaying (instance flag 0x2000000), 8 PlayToKey(key, dir), 9/10 direction,
  11/12 loop count, 13 loop / 14 loop-count / 16 ping-pong over a clip, 15 once, 17/18 clip flag, 19-21 key ranges
  whose enter/leave calls vt 40 / 41 (type 3 functions 53/54, 0x7120B0) with (spline, id, direction). Path decode
  and per-frame update: LEVELS.md "Spline movers".
- [O] `playersensor` 0/1 Set/Get its enable byte (+0xF); `instance` 18/21/23, `modifiable` 1, `unitref` 2,
  `larabase` 25/27/29/55 are accepted (effects [U]); 56 disables Lara's stick while a reveal camera plays
  (GAMEPLAY.md "Cameras").

- [O] Globals (gameglobalsbase type 5) SCRIPT members without initial values (+4/+8/+12/+272/+356/+432/+436 ->
  types 76/75/74/72/73/60/77) are engine-created objects; ours instantiate them with the globals (l5 soundvolumes
  call `globals.member12` type 74 vt 20). `instance` 27 GetUnit (0x4B06F0) returns a unitref (ours: a unit identity);
  a local holding another local's address is followed to its value for receivers and native arguments.

The survey also found a crash: cyclic static links (l2 planes) recursed forever in the legacy fallback; it is now
bounded to depth 8. The three script-only chapter ends (l1_ship_sinking_hall plane 1074 / volume 1082 → 1005,
l6_ship_engine_hall plane 7565 → 1387, l3_main_hall volume 4128 → 4108 → post-stream helper → 4070) run their real
handlers without a stop (`GameReal.ScriptedChapterEndChainsStartNextChapter`).

| Piece | Status |
|---|---|
| Volume box/sphere test, +200 test point, enter/exit, FirstIn once, dispatch order Enter → FirstIn → its calls → In → its calls; Exit → Out → its calls | [O] as above |
| Plane crossing | [O] geometry; [I] side of one-sided planes, vt 54 |
| Handler execution | [O] actual SCMD handler + nested calls; [I] static instanceref-target activation only as fallback |
| Object classes by objectlist name: `*triggervolume`, `triggerplane`, `*switch*`/`*lever*` (not cheat buttons), `*pu_block*`/`*rb_pushblock*`/`*pushable*`, `*door*`/`*gate*`; every other scripted intro uses `Kind::Scripted`, including cinematic objects | [I] specialized behavior classes by name; [O] all scripted intros instantiate and run available handlers |
| Switch: E / pad West within 200 units (±250 z) of the switch intro → plays `active_pulling`, then fires Use (vt 55 targets) and holds `active_idle` | [I] distances, timing; Lara plays no `obj_pull_lever` |
| Door: `modifiable.Set(0)` opens, `Set(1)` closes; opening plays its `*opening*` (else `open`) clip, then holds `*open*idle*`/`*_open` | [I] modifiable mapping and clip choice; closing is immediate (no closing clip yet) |
| Push block: E held + stick forward/back within reach → moves along the world axis nearest Lara's facing, 120 units/s, stops at collision | [I] the real block snaps to a grid and moves Lara with it (Lara's controller is untouched) |
| Save state | `serialize()`/`restore()` JSON keyed by intro unique id: `state`, `t`, `inside`, `firstIn`, `offset`, `hidden`, `engagement`, plus dirty `scriptFields`, `scriptStrings` and `scriptProperties`; state of units not loaded is kept and applied when they stream in. The 256 `gamebase` global bytes are stored in `gameValues`. |

Not done: collection/array/map opcodes, other thread/PSEQ fibers, most native bindings,
full uber-object animation state graphs, dispatchers, cinematics and reveal cameras,
closing-door animations, Lara's interaction clips.

- [O] Progression findings (docs/PROGRESSION.md "Fixes"): the native base, not the object name, makes a trigger a plane
  or a volume; cinematic vt 63 preloads (state 2) and only vt 57 plays; gamebase prototypes 17/18/19 = unit callbacks
  vt 11/12/13 with stream unit ids (unitref 0 compares them); `instance` 26 places Lara on an `instance` 4 transform;
  SSEQU on a native waits for it and then continues (0x57CE50 case 0x6A). The physical pressure plate base 3938
  (`multibody` 1, `joint` 17, `limit` 3 ranges, engine callback vt 34 with the range id) is not modelled.

## Verification

- `ou_script_tests`: synthetic shapes, enter/exit, dispatch order, plane/switch/block, VM arithmetic/raw-word
  branches/native calls, string copy/compare/conversion, nested sleep/resume, sequential tail calls, scalar/string
  save round trip and restore-before-load, instance/modifiable and checkpoint natives, message delivery, real chapter-end
  cinematic instantiation; real data: every type-8 section of all 82 units parses, every unit's
  trigger intros load (1 790 volumes, 327 planes, 28 switches, 58 doors, 640 objects with links, 39 animated),
  bedroom switch → door bytecode, ballcourt construction calling multibody SetJointLimit/Enable, playerref returning
  type 205, loaded instance waits, dynamic property get/set, and l4_pool volume 5513 box 567 × 553 × 248.
  `GameReal.BallcourtLeverDrivesDoorJoint` checks the live 2:1 lever/door coupling. `GameReal.ElevatorUberCommandCreatesMovingCollision`
  and `GameReal.ElevatorCarriesPlayer` cover the l2 elevator's native command, travel and player carry;
  `GameReal.TerrainGroupNativeUpdatesMergedCollision` covers l2 trigger-plane terrain toggles. START also runs fresh fibers
  through nested calls; `StartFibersClearInitialFields` covers real l2_shrineroom/l2_towerroom field creation.
- Replay (2026-10-18): `ou-game --level 'pc-w\l3_main_bedroom.drm' --spawn -8600,-8400,1850,-100
  --input-script bedroom.txt` with `0.5 interact 1` / `0.6 interact 0`: tick 31 the switch starts pulling, tick
  144 `use 4525 → activate 4520 (ma_ei_house_door)` and `→ 4086 (untracked; an intro of another unit)`; the door swings open (screenshots
  before/after viewed).
