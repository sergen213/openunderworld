# tru.exe classes, structures and globals

An inventory of the GOG `tru.exe` (sha256 `1fdbd345…1011`) data side, to go with the per-function
[FUNCTION_DATABASE.md](FUNCTION_DATABASE.md). It covers four areas:

- the complete RTTI class hierarchy, parsed straight from the PE;
- object sizes recovered from allocation sites, with evidence;
- every structure field the repository already establishes, consolidated with its `path:line` evidence,
  plus instruction-level checks;
- every `.rdata`/`.data`/`.bss` address that code inside a function references.

No decompiled code is stored. The tables hold addresses, offsets, sizes, names, counts, single
instructions quoted as evidence, and `path:line` references. This is an audit/RE pass only; no engine
code changed. Numbers below come from `docs/re/db/structures_summary.json` (generated 2026-11-16 at repo `a80bdb8`+).

## Regenerating

```sh
tools/re/struct_db.py      # --exe (default $OU_GAME_DIR/tru.exe), --work (../openunderworld-work), --out (docs/re/db)
```

stdlib + `pefile` + binutils `objdump` (i386) + `llvm-undname` (demangling; a simple fallback is built in).
It takes about 5 s and needs no Ghidra JVM. Identical inputs give byte-identical outputs (checked by
running it twice). The inputs:

- `tru.exe`: RTTI read with pefile; `.text` disassembled with `objdump -d -M intel`.
- `re-baseline/FunctionDb/functions_full.tsv`: Ghidra function bodies, which map instructions to functions.
- `data_symbols.tsv`: user-defined data names.
- `ghidra/exports/{rtti,imports,strings}.tsv`: for validation and global kinds.
- `docs/re/db/{functions,rtti,citations}.tsv`: subsystems, cited slots, repo names.
- `tools/ghidra/names/tru_gog.csv`.
- The FuncDetail agent's Ghidra exports `re-baseline/FuncDetail/{globals,vft_stores}.tsv`
  (p-code global reads/writes, vftable stores), when present. The output is reproducible only against the
  same snapshot of these.
- `git ls-files` text under `src/ tests/ docs/ tools/ CHANGELOG.md`, for structure offsets.

## Outputs (`docs/re/db/`)

| File | Rows | Content |
|---|---:|---|
| `classes.tsv` | 1 015 | One row per RTTI type descriptor. Columns: mangled name, TypeDescriptor VA, primary vftable, secondary vftables (`va@offset`), slots, slots cited by the repo, CHD attributes (1 = multiple inheritance), base count, direct bases, family. Then sizeof with confidence and evidence (`site:allocator:how`), a lower bound from the constructor's own field writes, constructors, destructors, deleting destructors, static instances, and whether Ghidra's `rtti.tsv` has the vftable (`base-only` = type descriptor seen only as a base) |
| `class_hierarchy.tsv` | 1 499 | One row per (class, base) BaseClassArray entry: base, the class it is a direct base of (`via`), depth, mdisp/pdisp/vdisp, BCD and CHD attributes |
| `structures.tsv` | ≈1 160 | One row per (structure, offset). Columns: meaning; sources (`table` = markdown offset table, `prose` = `name +0x..` in code or docs, `mirror` = a `// +0x..` member comment inside a C++ struct, `code-ref` = a code-referenced address inside a static structure, `check`); best evidence tag (`observed` [O]/[OBS]/[V-*], `inference`, `reference` [S], `stated` = no tag, `unknown`); evidence count and files; first three `path:line`; tru.exe functions cited on those lines; how many of them contain a `[reg+offset]` operand; the instruction check result |
| `globals.tsv` | 11 650 | One row per data address referenced from a function. Columns: section (`.data(bss)` = beyond raw data); kind (`data`, `object` = static instance of an RTTI class, `float-const`, `rdata`, `string`, `vftable`, `import`, `rtti`); name (user symbol, names CSV, repo citation, class/string/import); `nearest_named` (`name+0xNN` within 0x400 bytes, an [INFERENCE] grouping); gap to the next referenced address (a size upper bound); access widths; indexed use; reader/writer/address-taken function counts; objdump vs Ghidra agreement; top subsystems of the referencing functions; first writers/readers; repo citations; meaning (below) |
| `structures_summary.json` | | every number quoted here |

## Method

### RTTI

For every CompleteObjectLocator (`signature 0, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor`)
in `.rdata` the script follows a chain:

- the TypeDescriptor in `.data` (vftable = `type_info` 0x869918, name `.?AV…`);
- the ClassHierarchyDescriptor;
- the BaseClassArray of BaseClassDescriptors (`pTypeDescriptor, numContainedBases, mdisp, pdisp, vdisp,
  attributes`). The pre-order `numContainedBases` gives each base's direct parent and depth.

A vftable is the dword after a pointer to a COL. Its slots are the consecutive `.text` pointers up to the
next COL pointer.

Validation against Ghidra's `rtti.tsv`:

- 991 vftables found vs Ghidra's 990. All 990 match by address, class name and slot count (0 mismatches).
- The extra one is 0x84DD74, a function-local class:
  `` `cdc::SceneCellContainer::InternalGetCellsFromVolume(…)'::`2'::GeneralCR ``.

| Count | |
|---|---:|
| Type descriptors | 1 015 |
| … with a primary vftable / only seen as a base (no vftable of their own) | 964 / 51 |
| Complete object locators = vftables (27 secondary, at a non-zero offset) | 991 |
| Hierarchy descriptors | 964 |
| Multiple inheritance (CHD attribute 1) / virtual inheritance (attribute 2) | 57 / 0 |
| Hierarchy rows (class, base) | 1 499 (depth 1: 893, 2: 427, 3: 114, 4: 40, 5: 21, 6: 4) |

### Constructors, destructors, sizeof

Each step below works on the disassembly.

1. **Vftable stores.** `mov DWORD PTR [reg(+d)],<vftable>` occurs in 933 functions (1 528 (function,
   vftable) pairs). FuncDetail's p-code export has 1 544 pairs, of which 1 528 are the same pairs.
2. **Deleting destructors.** A vftable slot that tests bit 0 of its stack argument and only returns `ret 4`;
   318 found. A vftable-storing function that one of them calls is a **destructor** (130). Every other
   vftable-storing function is a **constructor**: 426 constructor→class assignments.
   - The class is the last offset-0 store through `this`. `ecx` and its copies are tracked from the entry
     up to the first `ret`; a derived constructor stores its vftable after the base constructor's.
   - That walk also gives `ctor_min_size`: the highest `this+offset+width` the constructor writes, a lower
     bound.
3. **Allocators.** The deallocator most deleting destructors call is 0x596B10 (105), then 0x44DB20 (16)
   and 0x5977D0 (11). Allocators are the functions with at least 3 allocation sites (step 4):
   - 0x574940 (128 sites). It is cdc `operator new`: `[[*0xCD9CAC]+0x48]` vfunc +0x34 (size, 0) [O].
     0x596B10 is the matching delete, vfunc +0x38 (ptr) [O].
   - 0x427B00 (6), 0x558DA0 (5): which subsystems [U].
   - 0x7BA2E2 (10), 0x7BA5B7 (13): inside the FMOD Ex range.
4. **Allocation sites.** The scan matches this sequence:
   - `push imm` (the first argument, within 8 instructions) and `call <allocator>`;
   - then, within 30 instructions and before any unrelated call, either `mov [eax-alias],<vftable>` or
     `mov ecx,eax-alias; call <constructor>`;
   - the last event wins, so an inlined derived vftable store after a base constructor call counts for the
     derived class.

   The pushed immediate is `sizeof`.
5. **Static instances.** `mov ecx,<data address>` followed by a constructor call; found for 6 classes.

Result:

- `sizeof` was recovered for **115 classes**: 31 high (≥2 agreeing sites), 84 medium (one site).
  There are 0 conflicts between sites, and 0 cases where a base class is larger than its derived class.
  Every recovered size is ≥ that class's `ctor_min_size`.
- A further 284 classes have only the constructor lower bound.
- The rest are allocated with a register size, through factories, as members (`lea ecx,[esi+N]; call ctor`),
  or not at all (abstract bases). Their sizes stay unknown rather than guessed.

Examples:

| Class | sizeof | Confidence |
|---|---|---|
| `PlayerSM` | 0x560 | high |
| `BasicRenderMesh` | 0x344 | high; ctor writes up to 0x341 |
| `Bike` | 0x790 | medium; ctor writes up to 0x784 |
| `cdc::AnimInputSocket` | 0x28 | high |
| `CombatCameraResource` | 0x274 | medium |
| `ExplorationCameraResource` | 0x29C | medium |
| `FMOD::CodecWav` | 0x14C | medium |

### Repo-established fields

The harvest reads three kinds of source:

- **Markdown offset tables.** A header whose first cell contains "off"/"offset"/"field". Rows whose first
  cell is `+0x..`, `0x..`, `+N` or plain decimal. The struct name comes from the header ("Intro offset" →
  `Intro`), else from the section heading, with evidence tags stripped.
- **Prose.** `name +0x..` in code comments and docs. Names are canonicalised by a small alias table:
  - instance → `Instance`;
  - Lara/player data → `PlayerData`; player / Lara → `PlayerInstance`;
  - render settings → `RenderSettings`; face → `CollisionFace`.

  Other CamelCase or RTTI class names are kept as written, and two-word nouns are joined ("enemy data" →
  `EnemyData`). Single-letter locals (`o +0x10`, `r +0x..`) are skipped: about 1 000 occurrences whose
  structure the text alone cannot tell.
- **C++ mirror structs.** Members with `// +0x..` comments (e.g. `MusicPlayer` slots).

The prose canonicalisation is lexical, so a few names are split (`Markup` / `MarkUp`, `Material blob` /
`MaterialBlob`). Merging them is left to a reviewer, because they are not always the same record.

Static structures (`GameTracker` 0xE7F088, `MainTracker` 0xE7ED60) also get one row per code-referenced
address inside them. The scan lengths are where the dense run of references ends [I], not proven sizes.

| Count | |
|---|---:|
| Fields (struct, offset) / structures | 1 159 / 186 |
| By source: table / prose / mirror / code-ref | 373 / 436 / 287 / 86 |
| By best tag: observed / inference / reference / stated / unknown | 400 / 52 / 1 / 674 / 32 |
| Rows citing a tru.exe function on the same line | 410 |
| … where at least one cited function has a `[reg+offset]` operand | 263 (a weak, automatic check; base registers are not resolved) |
| Curated instruction checks | 20: 18 match, 1 partial, 1 semantic mismatch |

### Key structures (layouts stay in the format docs; `structures.tsv` indexes them)

| Structure | Rows | Observed | Main sources |
|---|---:|---:|---|
| Instance | 34 | 13 | [LEVELS.md](../formats/LEVELS.md) intros, `docs/GAMEPLAY.md`, src comments |
| Player data (`PlayerData`) / player instance | 52 / 29 | 24 / 5 | `docs/GAMEPLAY.md`, `src/game/Player.cpp`, `Aim.cpp` |
| GameTracker 0xE7F088 / MainTracker 0xE7ED60 | 68 / 18 | code-ref | [TIMING.md](TIMING.md), [STARTUP.md](STARTUP.md), names CSV |
| Camera (CameraManager vfuncs, camera records) | 4 + 13 + 1 | 1 | [COLLISION.md](../formats/COLLISION.md), `src/game/Camera.cpp` |
| DRM section header (20 bytes) | 7 | 1 | [CONTAINERS.md §4.2](../formats/CONTAINERS.md) (relocation records there are prose/C, not indexed) |
| Level / unit data | 11 / 10 | 7 / 1 | [LEVELS.md](../formats/LEVELS.md), `src/formats/Level.cpp` |
| Intro / intro data | 16 / 6 | 13 / 2 | [LEVELS.md](../formats/LEVELS.md) |
| Material blob | 19 + 7 | 19 | [MATERIALS.md](../formats/MATERIALS.md) |
| Animation clip / anim node / request | 11 / 19 / 1 | 0 / 13 / 0 | [ANIMATION.md](../formats/ANIMATION.md), [ANIMGRAPH.md](../formats/ANIMGRAPH.md) |
| Collision face / mesh / markup / markup box | 3 / 9 / 10+3 / 6 | | [COLLISION.md](../formats/COLLISION.md), [PHYSICS.md](../formats/PHYSICS.md) |
| Script section (type 8) | 12 | 12 | [SCRIPTS.md](../formats/SCRIPTS.md) |
| Effects: emitter definition / GenericFXObject / defs | 54 / 24 / 22+ | 47 / 2 | [EFFECTS.md](../formats/EFFECTS.md), `src/fx/Fx.hpp` |
| PCRenderDevice / PCRenderLight / render settings | 7 / 3 / 13 | 2 / 2 / 1 | [SHADERS.md](../formats/SHADERS.md), `src/scene/Assets.hpp` |
| Object data / ObjectBase / death record | 25 / 12 / 5 | 8 / 5 / 1 | `docs/GAMEPLAY.md`, [PHYSICS.md](../formats/PHYSICS.md) |
| Script instance / VM frame | 0 | | repo documents only our own VM (`src/script/Vm.cpp`), not the original frame layout [U] |
| Save structures | 0 rows by name | | a few scattered offsets (`save header byte +0x1F9`, PROGRESSION.md:250; game state +0x590, GAMEPLAY.md:4694) [U] |
| StreamUnit | 0 | | only "stream-unit portal array" prose in LEVELS.md [U] |

### Instruction checks (`asm_check` column)

Each check names the function the repo cites, plus the exact instruction pattern (register, width,
direction) the field must show there. Base registers were confirmed by reading each function's prologue.
In `INSTANCE_IntroduceInstance` 0x5BCC30, `edi` = the intro (`mov edi,[ebp+8]` at 0x5BCC3C) and `esi` =
the new instance.

| Field | Function | Instruction | Result |
|---|---|---|---|
| Intro +0x00 rotation | 0x5BCC30 | 5BCDEC `mov eax,[edi]` → 5BCDEE `mov [esi+0x30],eax` | match |
| Intro +0x10 position | 0x5BCC30 | 5BCDA8 `mov edx,[edi+0x10]` → `[esi+0x20]` | match |
| Intro +0x50 u16 object id | 0x5BCC30 | 5BCC7D `movzx eax,WORD PTR [edi+0x50]` | match |
| Intro +0x52 u16 → instance +0x40 | 0x5BCC30 | 5BCD67 `mov dx,[edi+0x52]`, 5BCD6D `mov [esi+0x40],dx` | match (two checks) |
| Intro +0x58 i32 id | 0x5BCC30 | 5BCC3F `mov esi,[edi+0x58]` | match |
| Intro +0x71 bit 0x10 (scale) | 0x5BCC30 | 5BCE03 `test BYTE PTR [edi+0x71],0x10` | match |
| Instance +0x30 rotation | 0x5BCC30 | 5BCDEE | match |
| Instance +0xE0 unit | 0x5B3A20 | 5B3A56 `mov [edi+0xe0],ebx` | match |
| Instance +0x100 → +0x78 intro data | 0x5CD1B0 | 5CD244 `mov eax,[esi+0x100]`; 5CD252 `mov edi,[eax+0x78]` | match |
| Instance +0x1B4 fade (float) | 0x52C920 | 52CA60 `fstp DWORD PTR [esi+0x1b4]` | match |
| Instance +0x2A0 velocity (float4) | 0x5DB370 | 5DB40D `movups [eax+0x2a0],xmm2` | match |
| Instance +0x23C draw-group mask | 0x5B3DD0 | 5B3E23 `and [ecx+0x23c],ebp` | match |
| Level +0x74 unit data | 0x5B3860 | 5B3872 `mov edi,[edx+0x74]` (edx = arg 1) | match |
| Collision face +0x0E u8 flags | 0x5D3BA0 | 5D3BBB `movzx ecx,BYTE PTR [esi+0xe]` | match |
| Render settings +0x38 far plane (float) | 0x513BE0 | 513E0A `fld DWORD PTR [ebx+0x38]` | match |
| Texture header +0x15 u8 mips | 0x515B40 | 515BE2 `cmp BYTE PTR [eax+0x15],0` | match |
| CameraManager +0xE4 | 0x7759F0 | 7759FE `mov eax,[edx+0xe4]`, edx = vftable | match as a **vfunc** (see below) |
| GameTracker +0xB8 | 0x5E0050 | 5E0055 `mov al,BYTE PTR [esi+0xb8]` | **partial** |
| CameraManager +0x54 | 0x7767C0 | 7767CD `mov edx,[edx+0x54]`; 7767D8 `call edx` | **semantic mismatch** |

## Mismatches and corrections found

- **CameraManager +0x54 / +0xE4 are vftable slots, not fields** [O]:
  - In 0x7767C0 `eax` = the manager (from 0x5E7770), `edx = [eax]` (the vftable), `[edx+0x54]` is called
    with `(arg, 1)`. In 0x7759F0 `[edx+0xE4]` is called with 8.
  - `src/script/Script.cpp:1989` already words +0x54 as "through CameraManager +0x54", i.e. a call.
  - `src/game/Game.hpp:1178` calls "CameraManager +0x54" the orbit camera's *mode* (1 on foot, 4 free climb,
    …). Either the mode values are arguments of that vfunc, or Game.hpp means a different field; the
    comment needs rewording [U which].
  - ENGINE_ARCHITECTURE.md:261 "CameraManager +0xE4 type" is likewise vfunc 57 called with the type.
- **gameTracker +0xB8 is a u8 state, not a flag** [O]:
  - `MAIN_PlayGame` 0x5E0050 calls 0x5DAB40 only when the byte is neither 0 nor 8 (5E0055..5E0061).
    0x5DAB40 then tests `== 0xC` (5DAB48).
  - The global copy 0xE7F140 is written as a byte: 0 (0x5DE9E9), 8 (0x5DEA49), 0xA (0x77C29C); 0x5DFC07 compares it with 3.
  - FUNCTIONS.md:36 ("taken when != 0") and `tru_gog.csv` (0x5E0050, "unless gameTracker+0xB8 is set")
    both miss the value-8 exclusion.
  - That `esi` in 0x5E0050 is the gameTracker comes from those docs and the 0xE7F140 writes [I].
- **Intro +0x70..+0x73** is one table row in LEVELS.md, so the harvest indexes +0x70 and +0x73 only.
  +0x71 (scale bit 0x10) and +0x72 (bits 0x80/0x01/0x10, all in 0x5BCC30) are separate bytes [O].
- **Prose ambiguity:**
  - "settings +0x.." in src means the unit's *render settings* DTP, so it is indexed as `RenderSettings`.
    It is not the same record as LEVELS.md's `Level +0x70` settings DTP without further proof.
  - `DTP +0x..` rows (LEVELS.md light DTP table) are generic; their struct is the table heading.

## Globals

Two scans are merged:

- **objdump.** Every memory operand or immediate in `0x83E000..0x10D4FFF` from an instruction inside a
  function. Write = destination of a non-compare instruction; RMW counts as both.
- **FuncDetail.** p-code references from its `globals.tsv`.

Of the 11 650 addresses, 1 170 are seen only by Ghidra and 61 only by
objdump.

| Kind | Count |
|---|---:|
| data (`.data`/`.bss`) | 6 261 (written by some function: 4 258) |
| static RTTI objects (`object`) | 8 |
| float constants (`.rdata`, FPU/SSE read only) | 1 589 |
| other `.rdata` | 863 |
| strings | 1 846 |
| vftables referenced by code | 822 of 991 |
| IAT imports | 256 |
| RTTI records | 5 |
| By section: `.rdata` / `.data` / `.bss` | 5 372 / 1 633 / 4 645 |

Only 32 data globals carry a repo or Ghidra name, and 138 are cited somewhere in the repo. The `meaning`
column adds the ones established in this pass.

### Top data globals by referencing functions

| Address | Funcs | R / W | Width | Main subsystems | Meaning |
|---|---:|---|---|---|---|
| 0xCD9CAC | 1 895 | 1 895 / 1 | 4 | formats, game | **memory manager pointer** [O]: set to static object 0xCDB538 by 0x59A1E0; `operator new` 0x574940 / delete 0x596B10 go through `[[p+0x48]]` vfuncs +0x34 / +0x38 |
| 0xFADB58 | 728 | 728 / 1 | 4 | game | player data pointer: only writer is player init 0x721F80 [O]; `*(p+0x500)` larabase slots (GAMEPLAY.md:912) |
| 0xE7F094 | 644 | 642 / 2 | 4 | game | gameTracker+0x0C, Lara's instance (GAMEPLAY.md:271); writers 0x5D3DF0 / 0x5D40C0 [O] |
| 0x8BD7E8 | 453 | 452 / 4 | 4 | all, FMOD | `__security_cookie` [O]: 447 `/GS` prologs `mov eax,[0x8BD7E8]; xor eax,esp` |
| 0xECFAEC / 0xECFAF0 | 223 / 187 | 185 / 2 | 4 | game AI (0x60D…–0x63C…) | [U]: compared with record fields in 0x60DD0A; a pair written by 0x63A460 / 0x63C550 |
| 0x8B929C | 209 | 186 / 36 | 4 | FMOD | FMOD global [U] |
| 0xE7F188 | 191 | 188 / 8 | 4 | game | `gameTracker_timeMult` ([TIMING.md](TIMING.md)) |
| 0xE7EE50 | 173 | 173 / 1 | 4 | game, script | `GlobalInfo` pointer |
| 0xCDA0C4 | 164 | 163 / 164 | 4 | game, fx | **free-list head** [O]: inlined `[eax]=head; head=eax` push in ~160 functions (e.g. 0x453ECC); which pool [U] |
| 0x105BBEC | 162 | 162 / 0 | addr | FMOD | FMOD table [U] |
| 0xCC5038 | 155 | 154 / 0 | 4 | formats, game, script | **debug/log channel** [O]: first argument of the printf-style 0x6457A0 (format strings like `NtCell::SetEnabled(%s) => Cell could not be found.`) |
| 0xAD75E4 | 154 | 154 / 2 | 4 | render | `g_pcDeviceManager` |
| 0xFAD764 | 119 | 119 / 1 | 4 | game | [U]: pointer to an object whose +0x62C / +0x630 are read at 0x5D7B42 / 0x5D7BA9 |
| 0xC59264 | 116 | 116 / 1 | 4 | game | game state / options (GAMEPLAY.md:4022) |
| 0xCC4F70 / 0xCC4F6C | 114 / 71 | | 4 | script | script table with 8-byte entries (`[p+idx*8]`, 0x4A2987) [U] |
| 0xECFB00 | 109 | 81 / 1 | 4/16 | game, collision | 16-byte value read with `movaps` (0x615636) [U] |
| 0xFADFB0 | 107 | 107 / 3 | 4 | game | aim controller (GAMEPLAY.md:1899) |
| 0x8C0F34 | 104 | 103 / 2 | 4 | bfx | pointer tested for null and walked by bfx code (0x407E11) [U] |
| 0xA037F4 | 90 | 90 / 2 | 4 | game, physics | object passed as `this` to 0x4E9A80 / 0x4E9B00 [U] |
| 0x8A9C78 | 89 | address only | | fx, game | lagged-Fibonacci RNG state (`src/game/Feedback.hpp:120`) |
| 0xA1407C | 85 | 84 / 2 | 4 | render | `g_pcRenderDevice` |
| 0xA02B68 | 73 | 73 / 2 | 4 | game | `InputSystem*` |

## Class families

`family` = the namespace for namespaced classes, else the root of the primary base chain.

| Family | Classes | Notes |
|---|---:|---|
| `cdc::` (namespaced) | 296 | engine core: AnimPipelineNode (16 derived), UIWidgetBase (16), PCInternalResource (12), Geom, RenderDrawable, SoundPlex, IAnchor, RenderResource; 90 without bases |
| `Nt*` script natives | 198 | 172 under `cdc::RCObject`, 6 `cdc::GCObject`; pairs `NtX` + `NtX_ScriptType` |
| `Ns*` script natives | 52 | 26 `cdc::GCObject`, 26 `cdc::RCObject` |
| `Enemy*` | 186 | `EnemyBehavior` 80, `EnemyState` 80, `EnemyProcess` 20, `EnemyBehaviorTree` 5 |
| `StateObj` (player states) | 35 | e.g. `AccurateAimState → TopLevelPlayerState → BasePlayerState → StateObj` |
| `ICamera` | 18 | `AutoSplineCamera` adds `ICameraSplineListener` at +0xD0 (multiple inheritance) |
| `FMOD::` | 52 | codecs, DSPs, outputs, files, channels; mostly multiple inheritance with `FMOD_*_STATE` C structs |
| `bfx::` (NavPower) | 35 | Reloc, LogDevice, Mover3D, LinkBase, Setting families |

The full root→class chains are in `class_hierarchy.tsv`. The multiple-inheritance classes and their
secondary-vftable offsets are in `classes.tsv` (`secondary_vftables`).

## Unknowns

- `sizeof` for 900 of 1 015 types. 284 have a constructor lower bound only. A register-sized allocation,
  a factory, or embedded members would need data-flow (Ghidra) to resolve.
- Allocators 0x427B00 and 0x558DA0 (subsystem unknown); pools behind the free-list head 0xCDA0C4.
- High fan-in globals still marked [U] in the table above.
- The original script-VM frame, script-instance and save layouts are not documented in the repo; only our
  own reimplementations are.
- 147 harvested fields cite functions where no `[reg+offset]` operand with that displacement appears. They
  may be reached through a computed base, or the cited function may be a caller. They need case-by-case
  review; they are not counted as mismatches.
- Structure names in `structures.tsv` are the repo's own words. No cross-structure identity (e.g.
  `ObjectData` vs `ObjectBase`) is asserted here.
