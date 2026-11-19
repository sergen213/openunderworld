# tru.exe SCMD native binding table

Table: [`db/script_natives.tsv`](db/script_natives.tsv), one row per (native script type, function index). Generators:
`tools/re/script_natives.py` (static, from tru.exe), `tools/re/script_types_dump.cpp` (type ids and prototypes from the
82 unit DRMs), `tools/re/scriptnat_reader.c` (runtime capture under Wine). Binary: GOG tru.exe 1.1 (SHA-256
1fdbd345…1011). Labels: **[O]** observed (address / command), **[I]** inference, **[U]** unknown; confidence
high / medium / low per item.

## Summary

| Quantity | Value | Evidence |
|---|---|---|
| Native script types registered | 187 (all in `0x7A8650`) | [O] objdump: 187 calls to `0x574B60` / `0x574B20`, each followed by the class vftable store |
| ... with a DRM script type in the 82 units | 115 (type id = DRM section id) | [O] `script_types_dump` |
| `0x5768F0(index, handler)` bind calls | 2 267 (2 283 rows: identical-code-folded bind functions shared by several classes) | [O] objdump, all parsed (0 unparsed) |
| Distinct handler (thunk) addresses | 1 884 | [O] |
| Native functions the bytecode calls | 362 (distinct (type, index)) | [O] `script_types_dump` `R` rows; equals the SCRIPTS.md survey count |
| ... handled by OpenUnderworld | 220 yes, 27 stub (zero result), 0 argument-count mismatch, 115 no | [I] heuristic over `src/script/Script.cpp` + `src/game` forwarders (below) |
| All bound natives (2 283 rows) | 285 yes, 74 stub, 0 mismatch, 1 924 no | [I] same heuristic |
| Runtime verification | 1 920 of 1 920 checked rows match (153 linked types with functions; 7 more linked with no functions, 27 not linked in the captured scene); 0 runtime-only handlers | [O] two Wine captures, new-game prologue (Croft Manor bedroom: 21 `firetriggervolume`, `ma_uo_bedroom_door`, `lara_casual` in the instance list) |

## How the table is built (high)

- [O] Registration `0x7A8650`: for each type `operator new(0x24)`, `NativeScriptType::NativeScriptType(package, name,
  size, align, flag)` (`0x574B60`; `0x574B20` = package + name only for `corebase`-style bases), store the class
  vftable, then `0x57ED00(type)` adds it to the type manager `*0xCC4F5C` (keyed by package and name).
  Object layout: `+8` package `char*`, `+0xC` name `char*`, `+0x10` u8, `+0x11` flag, `+0x14` size, `+0x18` align,
  `+0x1C` linked `cdc::ScriptType*`, `+0x20` u8 bound.
- [O] Each class's vftable **slot 3** is its bind function. It calls `0x574BC0(scriptType)` (`this+0x1C = scriptType`,
  `scriptType+0xC = this`) and then `0x5768F0(index, handler)` per native function:
  `*(scriptType->streamData(+8)->functions(+0x38) + index*0x1C + 0x14) = handler`. Function index = position in the
  DRM type's function array, the number OpenUnderworld logs as `native <type> fn <n>`.
- [O] So the handlers are written at run time (into the loaded DRM data) but fully determined by immediates in the bind
  functions (`push handler; push index; mov ecx,esi; call 0x5768F0`). 11 classes bind nothing (bytecode-only bases such as
  `gameglobalsbase`, `dispatcherbase`, `actor`, `uberobjectcommand`).
- [O] Handler calling convention (every thunk): `cdecl handler(receiver, argCount, args*, result*)`; `args` points at
  `argCount` 4-byte stack slots, `result` is the caller's result address (non-void prototypes only). The thunk unpacks
  them and calls the engine function (`handler_calls` column: its first direct calls / tail jump).
- Correction: `PTR_PTR_008ac128` (globals.tsv) is **not** this table. It belongs to the anim/state-chart condition
  evaluator `0x432B20` (its own opcode `0x65` calls `table[i][j](ctx)`), is statically initialised (`{0x88F238,
  0x8ABF90}`, two vtable-like lists), and is unrelated to SCMD natives [O].

## Columns of `script_natives.tsv`

`type_id` (DRM section id, `-` when no unit carries the type), `native_name`, `class`, `package`, `bind_fn` (vftable slot
3), `fn_index`, `handler` (thunk VA), `handler_name` (functions.tsv applied name), `handler_calls`, `callee_names`,
`proto_vt` / `args` / `ret_type` (prototype vt slot, argument count, return DataType low nibble from the DRM),
`called_by_types` (distinct script types whose bytecode calls it), `implemented_ou`, `evidence` (`runtime match` =
identical handler in the Wine capture).

`implemented_ou` is a **source heuristic**, not a test: `yes` = a `strcmp(name, "x")` condition (or a `switch` /
helper such as `splineNative`, or the `src/game` forwarders `setMultibodyNative` / `setEnemyNative` / `waterNative` /
`Enemies::generatorNative`) names that index; `stub` = the type is in a "passive" list that returns 0 for any function;
`args-mismatch(n)` = the binding demands n arguments but the prototype has a different count, so it never runs; `no` =
no binding found. A `yes` may still fail at run time on a further condition (receiver kind, object lookup).

## Opcode 0x65 (CALLU) and the fire-volume stop (OU-SCR-001)

- [O] high. `0x57CE50` dispatches `opcode − 1` through the table at `0x57E52C`; entry `0x64` = `0x57DE34`:
  1. operand word = `Function*`; pop the receiver;
  2. receiver null → `0x5766F0(prototype, sp)` drops the arguments and the result slot (prototype `+0xC` argument
     SArray count, `+0x10 & 0xF` non-void), no call, result untouched;
  3. else `argc` = prototype argument count, `args = &stack[sp − argc + 1]`; non-void → pop the result address and
     **skip the call if it is null**; call `function+0x14(receiver, argc, args, result)` (the native handler); continue.
  CALLU is therefore the "native method on a receiver" call (OpenUnderworld's CALLU already does this for DRM functions).
- [O] high. Script-type link pass `0x57EE20` (walks every non-native function's code with the exe's operand lengths: one
  operand word for 02 05 34 35 3B 3C 65 66 68–6C 6E–72 74–77 7E 7F, two for 67 6D 73) rewrites operands: `0x34` / `0x3B`
  take `*(type ref + 0xC)` (the native type object), and **CALLU operands with bit 0 set** are builtin container methods
  `{u8 1, u8 DataType, u16 index}` replaced by `NativeBuiltInImpl(*0xCC4F68)->vt0(DataType) + index*0x1C`.
- [O] high. `cdc::NativeBuiltInImpl` (`0x57BF90`, selector `0x575170`: DataType 8 → `+8`, 7 → `+0xC`) builds two builtin
  Function arrays from static records `{handler, name, return DataType, arg name, arg DataType, …}`:
  | DataType | index | name | handler | behaviour |
  |---|---|---|---|---|
  | 8 MAP (`0x8A7688`) | 0 | Size → int | `0x5767A0` | `*result = map+0x20` |
  | | 1 | Clear | `0x5793E0` | |
  | | 2 | Find(key: string) → bool | `0x578630` | |
  | | 3 | Erase(key: string) | `0x579F60` | |
  | 7 DYNARRAY (`0x8A7718`) | 0 | Size → int | `0x574A30` | `*result = array+0x18` (count) |
  | | 1 | Clear | `0x5795A0` | → `0x579540` |
  | | 2 | Resize(size: int) | `0x579520` | → `0x5794A0` |
  | | 3 | Remove(index: int) | `0x57CE30` | bounds `0 <= i < +0x18`, → `0x57CDD0` |
  | | 4 | Push(value) | `0x57BE50` | element kinds 1 / 5 → `0x57BE00` |
  | | 5 | Pop | `0x5795B0` | `--count`, destroys element at `+0x20 + count*(+0x14)` |
- [O] high. The failing site is `firetriggervolume` type **6516** function 4 (vt −1; the state-0 constructor), code word
  21 (`deathfiretriggervolume` 11671 has the same function): `PUSHL32 4; PUSHLA 8; 0x10 16 (dynamic-array member +16);
  CALLU 0x00000701; PUSHL32 8; LT; JMPF` = `for (i = 0; i < sensors.Size(); ++i)`. The operand word `0x00000701` has
  **no relocation** (checked in all three relocation kinds), i.e. builtin DYNARRAY function 0 = **Size**. Runtime
  (Wine, both captures): the same code at `0x0A890A2C` reads `… 65000000 02269574 08000008 …`; `0x02269574` = builtin
  DYNARRAY entry 0, handler `0x00574A30`.
- [O] Why OpenUnderworld stops: `parseScriptType` treats an unrelocated operand word as the next instruction, so CALLU gets
  no operand (fails as `op 0x65`) and `0x701` decodes as `NOP 1793`. Across all 82 units only two decode mismatches
  against the exe's operand lengths exist: this CALLU (2 types) and START `0x77` with a null (unrelocated 0) function word
  (11 types; harmless, decoded as `NOP 0`). Fix shape (not done here): decode an odd unrelocated CALLU operand as
  `{builtin, DataType, index}`, implement DYNARRAY Size over the VM's dynamic arrays (the exe's `{data +0x20, count
  +0x18}` that DREFD / DBOUNDS already read) and the other builtins as needed; then the fire volumes build their player
  sensors (`fireanim` global byte 13, GAMEPLAY.md).

## Natives reported missing in user logs (OU-SCR-002 / 003)

Handler = thunk; target = engine function it calls. `args` / `ret` from the prototypes; `called` = calling script types.

| native | handler → target | args / ret | decoded behaviour | OU status | confidence |
|---|---|---|---|---|---|
| `instanceref` 10 (type 51) | `0x799B30` → `0x4B1870` | 2 / void | **SetFloatProperty(symbol, float)**: args[0] is the address of a 2-byte property symbol (null → no call), args[1] the float. `0x4B1700` finds/creates the (instance, symbol) slot in the dynamic property store `*0xA13F18`'s float block (`+8`, values at `+0x18`), stores the value, and `0x4AF7B0` notifies a registered owner (`0x4AF630`, the sticky notification path). Family: 8 SetInt (`0x4B17C0`, int block), 9 SetBool (`0x4B0900`, bit block `+0x14`), 11/14 GetInt, 12/15 GetBool, 13/16 GetFloat. | `args-mismatch(1)`: `Script.cpp` binds fn 10 only for `args.size() == 1`; the prototype has 2, so every call stops (64 log stops, 11 calling types) | high (store), medium (notify target) |
| `instance` 27 (type 3) | `0x7B2C20` → `0x4B06F0` | 0 / NATIVE | GetUnit: 12-byte `unitref` of the instance's unit (`instance+0x20 → +0xE0`), copied into the result | yes, but returns false when the receiver is neither a World object nor Lara's script (52 log stops: receivers without an object) | high |
| `instance` 63 (type 3) | `0x798F50` → `0x7769A0` | 1 / void | args[0] non-null: `this = receiver+0x14`; the stream tracker record of the instance's unit (`0x5C1B50(unit id +0xE4)`, state 2 = loaded; its first word is passed on); then `0x66A640` → `0x66A240(rec[0], args[0]+4, 0, 0, 0)` on the AI manager `*0xE922A8`: pushes the 5-word record on the queue at `+0x25FE4` (`0x661050`) and, for the entry `i` of the key list `+8` (8-byte `{key << 4 \| kind, id}`, `+0` count) with kind 1, key = rec[0] and id = args[0]+4, stores the three floats (all 0) at `+0x1F50 + 0x90·i + 0x78 / +0x7C / +0x80` | no (12 log stops, 90 calling types incl. `sh_ei_combat_crate_*`; the survey never reaches them) | high (data flow, 2026-11-19 re-read); [U] what the AI manager's 0x90-byte kind-1 records are and who reads `+0x78..+0x80` (no binding without it) |
| `gamebase` 27 (type 55) | `0x797D90` → `0x776130` → `0x773290` | 5 / void | **SetColourRamp(slot, r, g, b, seconds)**: slot table `0xFAE680[slot] = seconds`, target `0xFAE6F0 + 16·slot = (r, g, b, 1)`; `0x773BF0` each frame lerps the current colours `0xFAE6A0 + 16·slot` toward the target over the remaining seconds and uploads slot 0 / 1 through `g_pcRenderDevice` vtable `+0x84` with ids 8 / 9; reset to white by `0x7731B0` | no (8 log stops: `ms_uo_yacht`, `co_yacht`) | high (data flow); medium [I] on what render constants 8 / 9 tint |
| `cinematic` 10 (type 3521) | `0x797040` → `0x4A4CC0` | 0 / void | returns a latent `NtCineSequence` (vftable `0x845B50`, update `0x4ADAA0`) on the receiver with end time FLT_MAX (`0x845884`): wait until the cinematic has played to its end; 9 is the same with a float time | yes (blocking), but only for Member / Object receivers (3 log stops) | high |
| `unitref` 1 (type 58) | `0x79D6B0` → `0x4AAEC0` | 0 / bool | **IsLoaded**: `0x5C1B50(unitref id)` scans `g_streamTracker` (0x36-dword records) for that unit in state 2; result byte = found | no (2 log stops, 6 calling types) | high |
| `anchor` 0 / 1 / 3 / 4 / 5 (type 54) | `0x7966B0` / `0x7966F0` / `0x796770` / `0x7967D0` / `0x796830` → `0x4A3CB0` (vtable `+8` position), `0x4AB870`, `0x4A3D20` (vtable `+0xC` rotation) + `0x4A3840` / `0x4A3870` / `0x4A38A0` | 0 or 1 / NATIVE (vector) | 0 **Position**; 1 **VectorTo(anchor)**: args[0] non-null → `position(args[0]) − position(receiver)`, w = 0 (`0x84111C` = 0.0); 3 / 4 / 5 rows 0 / 1 / 2 of the rotation's matrix (`0x49D500`: identity, then `0x49BD40` z with rows `(c, s)`, `(−s, c)`, then y, then x), the local x / y / z axes. A null anchor reads zeros. Lara's model faces −Y, so her y axis is her back | yes (`Script.cpp`, 16-byte results via `NativeContext::words`): Lara's script = her root / rotation (0, 0, yaw + π/2) (TickInput::yaw), other objects their origin or marker (`setAnchorAt`) and intro rotation [I: a marker's own frame]; `ScriptRealData.FireVolumeFlinchSide` | high |
| `vector` 0–12 (type 220) | `0x796540` / `0x7962A0` / `0x796440` (0 / 3, 1 / 4, 2 / 5: get x / y / z), `0x796550` / `0x796560` / `0x796450` (6 / 7 / 8: set x / y / z), `0x796570` (9), `0x796460` (10), `0x7ABAB0` (11), `0x7ABAF0` (12) | | 16-byte value `{x, y, z, w}` inline in the frame / instance; the thunk gets its address. 9 copies the 16 bytes of args[0] (an address, non-null); 10 Set(x, y, z), w = 0; 11 length `sqrt(x²+y²+z²)`; 12 scales all four by args[0]. 13 / 14 (`0x7964D0` → `0x79D8E0`, `0x7962B0` → `0x7987B0`, bool arguments) are not called by shipped scripts | yes (`Machine::valueNative`), 13 / 14 no | high |
| `rotation` 0–15 (type 219) | `0x796360` → `0x4A2F40` (0); `0x796380` / `0x7963C0` / `0x796400` → `0x4A3840` / `0x4A3870` / `0x4A38A0` (1–3); the `vector` thunks (4–14); `0x7964B0` (15) | | euler `{x, y, z, w}`, z = yaw. 0 rotation between args[0] and args[1] (both null: nothing): `0x49DA10` normalises both, `h = (a + b)·0.5`, quaternion `(a × h, a · h)`, normalised (`0x49B000`); `0x49D450` → matrix `0x49CF90` → euler `0x49B440`: `x = atan2(m12, m22)`, `y = atan2(−m02, |(m00, m01)|)`, `z = atan2(m01, m00)`; two horizontal vectors give (0, 0, signed angle from a to b, counter-clockwise about +z). 1–3 matrix rows; 4–9 get, 10–12 set components, 13 copy, 14 Set(x, y, z); 15 z += args[0] · 0.0174533 (degrees). 16 / 17 not called | yes (`Machine::valueNative`; antiparallel input turns π about z [I], general euler order [I] beyond yaw) | high (horizontal case), medium (tilted) |
| `larabase` 21 / 22 (type 213) | `0x79A110` / `0x79A130` → `0x777B10` / `0x777B70` | 1 / void | args[0] = address of an `animation` value `{u16 object (8), u16 1, u32 pattern}` (non-null); `0x4A4800` reads `{u16 kind 1, u16 pattern}` (record word `+2` behind value `+4`) into `+0x488` of her upper-body chart object (`*(0xFADB58 + 0xF14)`), then INSTANCE_Post(Lara, **0x1DA** (21) / **0x1DB** (22), 0) (`0x5B3750`). Her message filter (`0x76FC40`, case 0x8E → `0x76FD78`) makes 0x1DA of a message 0x8E the same way. That chart's root S0 takes 0x1DA / 0x1DB into S90 while her locomotion state's property 0x645 holds; S90's action `0x760530` plays the pattern on anim section 1 (`+0x320`, segments 14..106; `0x444BA0`) — docs/GAMEPLAY.md "Script flinch overlay". The patterns the fire volumes name are Lara's `fire_walk_front / _left / _right` (1501 / 1500 / 1499) and `fire_standing_front / _left / _right` (1493 / 1494 / 1495) | yes (`Runtime::playerAnimPosts` → `World::tickFlinch`; `GameReal.FireVolumeFlinchPlaysUpperBodySection`); condition 0x1F5 (swimming entry) [U] | high (data flow, Wine) |
| `sound` 1 / 2 (type 71) as SSEQU | `0x4AF230` / `0x4AF290` | latent | play (`0x4AE380` / `0x4AE290`), then always allocate a `SoundSequence` latent (vftable `0x846F64`, 16 bytes, `+0xC` the ref-counted sound handle) whether or not the sound started. SSEQU (`0x57CE50` case 0x6A, `0x57E016`) calls `0x574960` (native, then the latent's update `vt+0x18` with dt 0) and, for a non-null latent, queues it on the thread (`0x576A30`) and returns: the thread is suspended. The thread update `0x57E970` runs each latent's update and IsActive (`0x4ACC30`: enabled and the handle's sound instance alive), drops the inactive ones and runs the interpreter only when none is left: the earliest resume is the next update | yes (`Script.cpp`: always one tick, then while `SoundBusy`; OU-SCR-004) | high |

Other frequently called natives without a binding (by calling types): `instance` 63 (90), `enemybase` 28 (25),
`waypointnode` 2 (23), `inputaction` 0 (19), `padshock` 1 (15), `marker` 1 (11), `plannerobstacle` 1 (10). Full list:
rows with `called_by_types > 0` and `implemented_ou = no` (the `implemented_ou` column predates the vector values).

### The fire volumes' burn flinch (type 6515, OU-SCR-002)

- [O] Type 6515 (l3_main_bedroom intro scripts 12135..12163, volumes 4529..4557, and the other burning units) enters
  state 1 (fn 7) from its sticky notification vt 6 when OnIn (vt 70) sets sticky `+168`, state 2 (empty) on OnOut. fn 7:
  every 1/30 s (WAIT 0.0333) unless sticky `+120` (message `LaraFireAnim_Disable` on channel `fireanim`) is set:
  `+44 = vt73(Lara's anchor, own anchor)`, `+52 = vt74()`; when `+44` or `+52` changed, larabase 22, else larabase 21,
  with the pattern for (side, moving): front / left / right × moving (`+96 / +104 / +112`) or standing (`+196 / +204 /
  +212`); "back" posts nothing.
- [O] vt 73: `VectorTo` from the volume to Lara (anchor 1), Lara's y axis (anchor 4), both z = 0 (vector 8), rotation 0
  between them, its yaw (rotation 9); FLOAT local `+12` = π (its record's init: the frame setup `0x57A850` constructs
  every local from its record through `0x579770`, so BOOL / INT / FLOAT locals start at their declared values); sides
  `[−π/4, π/4)` front, `[−3π/4, −π/4)` left, `[π/4, 3π/4)` right, else back. vt 74: `+80 = +64` (vector 9), `+64 =
  position(Lara)` (instance 6 → anchor 0 → vector 9), moving = length(`+80 − +64`) > 1 (vector 0/1/2, 10, 11); `+64`
  starts at 0, so the first pass counts as moving.

## Puzzle natives (OU-PZL-009 / 015 / 008, OU-GRP-005)

| native | handler → target | args / ret | decoded behaviour | OU status | confidence |
|---|---|---|---|---|---|
| `modifiable` 7 (type 823) | `0x79ABA0` → `0x4A5A90` | 1 / void | INSTANCE_Post(0x2E, value) to the instance, for every receiver; the anim-graph handler `0x6B4440` → `0x6B453B` enters the state with that id at once (no class check) | yes on every switch (before: `ar_ei_switch_oneway` only); `ProgressionReal.BallcourtExitSwitchLockedUntilTheKey`, CriticalGates 498 / 5469 | high |
| `multibody` 3 (type 3566) | `0x79ACD0` → `0x4A6FF0` → `0x4D8FF0` | 0 / void | Disable: queries 0x13F / 0x140 (body count / array); each body with a physics component (`+0x274`) gets `+0x15C \|= 1` ("fixed") and `+0x160 = 0`; inverse of 2 (`0x4D8FD0`) | yes: World keeps the set (saved as `multibodyFixed`), `multibody` 4 reads it; no motion change of ours [I]. `ProgressionReal.MaproomMultibodyDisableFixesTheBodies` (l2_maproom planes 2637 / 2638 on bridge 2667) | high (semantics) |
| `multibody` 92 (type 3566) | `0x79B8F0` → `0x4A7300` | 1 / void | `if (*this) INSTANCE_Post(*this, 0x2C, value)`; a class-30 breakable takes 0x2C while whole: the break record whose id is the value starts (`World::breakBreakable`) | yes for breakables; other receivers' 0x2C [U] (no effect). `ProgressionReal.TempleFloorBreaksByMultibody92` (l4_temple floor 5234) | high (thunk), medium (breakable handler) |
| `multibody` 93 (type 3566) | `0x79B910` → `0x4A7320` | 0 / bool | `query(*this, 0xCA) != 0`; for a breakable: broken | yes for breakables (false elsewhere) [U other receivers] | high (thunk), medium (breakable answer) |
| `larabase` 26 (type 213) | `0x79A1A0` → `0x777C30` | 1 / bool | her grapple controller (query 0x175, then 0x17C) has its rope's target (`+0x4C` → `+0x44`) on the argument instance | yes (`Player::grappleUid`); the rope attach `0x6A35E0` also calls the target's instance prototype 64 = vt 46 (`0x799840`). `GameReal.GrappleLeverReturnsWhenReleased` | high |

## Reproduce

```sh
# 1. Script types (type id, prototypes, native call references) from the unit DRMs (~2-4 min, deterministic)
B=build-AuditBaseline; W=../openunderworld-work/re-baseline/ScriptNatives
c++ -std=c++23 -O1 -Isrc -I$B/gen tools/re/script_types_dump.cpp $B/libou_script.a $B/libou_formats.a \
    $B/libou_cdc.a -lz -o $W/script_types_dump
OU_GAME_DIR="/mnt/1st4TB/Tomb Raider Underworld" $W/script_types_dump > $W/script_types.tsv
#    OU_DUMP_TYPE=6516 adds that type's decoded instructions (#ins rows).
# 2. Static table (+ optional runtime check) -> docs/re/db/script_natives.tsv (deterministic, ~2 s, needs pefile, objdump)
tools/re/script_natives.py --types $W/script_types.tsv [--runtime $W/run2/runtime_natives.txt]
# 3. Runtime capture (private compositor, own WINEPREFIX copy, never the desktop session)
i686-w64-mingw32-gcc -O2 -o $W/scriptnat_reader.exe tools/re/scriptnat_reader.c
cp -a ../openunderworld-work/wine $W/wineprefix      # copy of the established prefix (DXVK d3d9, settings)
cp ../openunderworld-work/scratch/climbcam/ouk/wkey.exe $W/; cp -a ../openunderworld-work/scratch/climbcam/ouk/cfg $W/cfg
pgrep -af 'wine|tru.exe'                              # must be empty first
systemd-run --user --scope --unit=ou-scriptnat -q -p MemoryMax=14G -- env -u DISPLAY W=$W XDG_CONFIG_HOME=$W/cfg \
  XDG_DATA_HOME=$W/data dbus-run-session -- kwin_wayland --virtual --xwayland --width 1280 --height 720 \
  --socket ou-scriptnat --no-lockscreen --no-global-shortcuts --no-kactivities --exit-with-session \
  $PWD/tools/re/scriptnat_capture.sh
```

`tools/re/scriptnat_capture.sh` (the session's child): starts `wine ./tru.exe` from `openunderworld-work/wine-drive/game`
(`WINEDLLOVERRIDES="d3d9=n;dxwrapper=d"`), presses the setup dialog's Ok (`wkey.exe`), starts a new game (keys Up, Enter,
Down, Enter, Enter, then 90 s: the prologue, 21 `firetriggervolume` instances in the instance list), runs
`scriptnat_reader.exe code 08000004 09000008 10000010 65000000` (the native table `T` / `F` lines, builtin arrays `B`,
the fire-volume code pattern `P`), then `wineserver -k`; the compositor exits with it. Reader method: scan committed
private memory for `NativeScriptType` objects (image vftable, `+8` → `"game/script/…"`, `+0xC` → name), follow
`+0x1C → ScriptType → +8 → +0x38` and print each function's `+0x14`.

## Unknowns

- [U] Meaning of the AI-manager record that `instance` 63 clears (type-1 entries keyed (unit, id) in `*0xE922A8`), and
  the argument type whose `+4` word is the id.
- [U] Which render constants ids 8 / 9 of `g_pcRenderDevice` `+0x84` are (gamebase 27 colour ramps).
- [U] The 27 registered types not linked in the prologue capture (e.g. `light`, `weaponbase`, `portablebase`, most UI
  widgets) are verified statically only (bind immediates); no runtime row was checked for them.
- `implemented_ou` is heuristic; per-index status for natives handled inside generic blocks may be over- or
  under-reported.
