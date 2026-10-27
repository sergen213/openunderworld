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
- [O] Uber objects keep a section table in the ObjectBase DTP (`ms_ani_tombdoor` 5130: "closed", "open";
  `sa_uo_flush_gate`: "initial", "open") plus an animation state graph. [U] how commands (e.g. 176) and
  `modifiable` values select sections and clips.
- [O] Rigid multi-bone models (doors, switches) have skin indices but no weights in their vertex declaration;
  palette e.g. {0, 1} for the house door's 2-bone skeleton. [I] each vertex follows the bone of index byte 0
  (`formats::readSkin`); confirmed visually by the house door opening on its hinge bone.

## Runtime (`script::Runtime`) — what is implemented

Not a script VM. Links and handler slots come from the bytecode; behaviour is native C++:

## Toward a bytecode VM (2026-10-27 findings)

- `parseScriptType` already decodes every section: members with initial values, instanceref links,
  functions with their prototype and vtable slot, and the instruction words (op, arg, operand
  pointers, ISTYPE ref, PUSH immediate).
- The exe's `FUN_00432B20` is only a small state-chart condition evaluator (PUSH/POP, float and
  bool arithmetic, JMP/JMPF, CALLU, RET), not the full cdc script VM.
- Native types seen in real scripts: `instance` (id 3, 72 prototypes), `multibody` (3566, 94),
  `stickybool` (69), `globalsref` (81), `modifiable` (823), `instanceref` (51, 17). Native class
  vftables are RCObject/lifetime methods, not these prototypes; the CALLU function tables are built
  at runtime (static table memory is zero), so mapping `(type id, function index)` to an
  implementation needs live observation (a Wine hook) or more decomp work.
- cdcEngineDXHR/cdcScript/ByteCodeEnum.h has the opcode numbering; there is no interpreter in the
  research clones. Next: implement the interpreter for common opcodes and map native calls by
  observing them live.

| Piece | Status |
|---|---|
| Volume box/sphere test, +200 test point, enter/exit, FirstIn once, dispatch order Enter → FirstIn → its calls → In → its calls; Exit → Out → its calls | [O] as above |
| Plane crossing | [O] geometry; [I] side of one-sided planes, vt 54 |
| Handler targets: instanceref members the handler (and the non-virtual helpers it calls) pushes with PUSHSA | [O] data; [I] that each such target is "activated" |
| Object classes by objectlist name: `*triggervolume`, `triggerplane`, `*switch*`/`*lever*` (not cheat buttons), `*pu_block*`/`*rb_pushblock*`/`*pushable*`, `*door*`/`*gate*` | [I] the exe binds C++ types per object; not decoded |
| Switch: E / pad West within 200 units (±250 z) of the switch intro → plays `active_pulling`, then fires Use (vt 55 targets) and holds `active_idle` | [I] distances, timing; Lara plays no `obj_pull_lever` |
| Door: Activate → plays its `*opening*` (else `open`) clip, then holds `*open*idle*`/`*_open` | [I] stands in for the uber/modifiable state machine; doors only open |
| Push block: E held + stick forward/back within reach → moves along the world axis nearest Lara's facing, 120 units/s, stops at collision | [I] the real block snaps to a grid and moves Lara with it (Lara's controller is untouched) |
| Save state | `serialize()`/`restore()` JSON keyed by intro unique id: `state`, `t`, `inside`, `firstIn`, `offset`; state of units not loaded is kept and applied when they stream in |

Not done: running the bytecode (members like `stickybool`, `modifiable`, `uberobjectcommand`, unit scripts,
dispatchers, cinematics, reveal cameras), uber-object section tables, door closing, Lara's interaction clips, door
collision.

## Verification

- `ou_script_tests`: synthetic shapes, enter/exit, dispatch order, plane/switch/block, save round trip and
  restore-before-load; real data: every type-8 section of all 82 units parses, every unit's trigger intros load
  (1 332 volumes, 327 planes, 28 switches, 58 doors, 566 objects with links, 39 animated), bedroom switch → door
  link and l4_pool volume 5513 box 567 × 553 × 248.
- Replay (2026-10-18): `ou-game --level 'pc-w\l3_main_bedroom.drm' --spawn -8600,-8400,1850,-100
  --input-script bedroom.txt` with `0.5 interact 1` / `0.6 interact 0`: tick 31 the switch starts pulling, tick
  144 `use 4525 → activate 4520 (ma_ei_house_door)` and `→ 4086 (untracked; an intro of another unit)`; the door swings open (screenshots
  before/after viewed).
