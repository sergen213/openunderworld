# Save games and checkpoints (tru.exe)

Static reconstruction of tru.exe's save system: the in-memory SAVE buffer and its block types, the game-state block
(type 6) that 0x556E90 restores, the checkpoint flow, and the PC `.TRUSave` file container. What triggers a checkpoint
and what a death restores is decoded in docs/GAMEPLAY.md "Checkpoints and respawn"; OpenUnderworld's own format is
docs/SAVES.md. Evidence tags as in [CAMERA.md](CAMERA.md): **V** objdump, **D** decompiler only, **[I]** inference.

## 1. Function table

| Address | Name (tru_gog.csv) | Role | Conf. | Check |
|---|---|---|---|---|
| 0x556D20 | SAVE_Init | allocate the two 0x140004-byte buffers, zero them, sentinel 0xDEADBEEF at +0x140000, then SAVE_ClearMemory(1) | high | V (556D35..556D9D) |
| 0x556940 | SAVE_ClearMemory | "SAVE_ClearMemory()": reset both buffers, allocate the two game-state blocks (type 6, 0x41028 bytes) at 0xC59264 / 0xC59268, defaults (§3) | high | D |
| 0x555280 | SAVE_ResetBuffer | zero a buffer, sentinel, clear the intro index (0xC57258, 0x800 slots) and its count 0xC59260 | high | V (5552A9) |
| 0x556310 | SAVE_AllocBlock | `(type ≤ 6, size)`: header dword `(dwords << 8) | type`, appended at `[buf+0x1357F4]`; purges (0x555FC0) when full; "illegal save type", "save %d is too big!", "ran out of saved memory" | high | V (556350..5563C0) |
| 0x5557B0 | SAVE_DeleteBlock | remove a block, compact the area, fix the intro index | high | D |
| 0x5570F0 | SAVE_SaveGame | `(0)`: "Save Game", SAVE_BufferAll + SAVE_WriteGameState; always copies working → committed (0x140004 bytes) | high | V (557120) |
| 0x5568C0 | SAVE_BufferAll | every live instance → 0x556680; every loaded stream unit (state 2) → 0x5563D0; script tables 0x555820; player object vf 0x5C | high | D |
| 0x556680 | SAVE_BufferIntro | one instance → block type 2 / 3 / 4 / 5 (§2) | high | D |
| 0x5563D0 | SAVE_BufferUnit | block type 1: a unit's moved terrain groups | high | D |
| 0x5560E0 | SAVE_RestoreUnit | applies a type-1 block to a loaded unit (16-byte entries onto 0x50-byte terrain-group records; unit float +4 unless ±16384000) | medium | D |
| 0x555820 / 0x5558A0 | SAVE_StoreScriptTables / SAVE_LoadScriptTables | six fixed areas +0x135FF4..+0x13FBFC ↔ the script variable stores (`*0xA13F18`) | high | D |
| 0x555BE0 | SAVE_WriteGameState | type-6 block: version 0xC2C, checkpoint unit + id, clock, music / reverb, outfit (§3) | high | V (555BE5) |
| 0x557130 | SAVE_RestoreGame | `(arg)`: "Restore Game": committed → working, sentinel check ("Load save game error"), script tables, 0x556E90, keeps the larger 64-bit counter +0x1D8 and the subtitle language +0x208 across the copy | high | V (557194, 5571C6) |
| 0x556E90 | SAVE_RestoreCheckpoint | version 0xC2C or "Wrong version for save. Not restoring save." + ClearMemory; "MAIN checkpoint restore %s ID wanted = %d"; applies §3 to the gamebase variables | high | V (556E98) |
| 0x556DD0 | SAVE_RestoreInventory | the ten inventory words from +0x260 (chapter flag set) or +0x230 into gamebase slots 1..10 (0x5D5150) | high | V |
| 0x555B70 | SAVE_GetCommitted | `return [0xC5925C]` | high | D |
| 0x555B80 | SAVE_Shutdown | free both buffers | high | D |
| 0x7744E0 / 0x788870 | Storage_RegisterCategories / Storage_RegisterCategory | categories `savegame%02d` `.sav` ×10 (0x140000), `Screenshot%03d` `.ss` ×100 (0x80000), `profile` `.pro` ×1; called from MAIN_DoMainInit (5DF234) | high | D |
| 0x78AB60 | SaveFile_Update | PC save/load/delete state machine (byte 0x8B7158, states 0..0x11; §5) | high | D |
| 0x78A3F0 | SaveFile_BeginWrite | build name, metadata and the one 0x140000-byte block from the committed buffer | high | V (78A4C6..78A641) |
| 0x78B550 | Save_GetDirectory | My Documents (CSIDL 5 → "Personal" → `C:\My Documents`) + `\Eidos\` + `Tomb Raider - Underworld` | high | D (strings) |
| 0x789700 | Registry_GetGUID | 16 bytes `HKLM\Software\Crystal Dynamics\Tomb Raider: Underworld\GUID` | high | D |
| 0x6DB2F0 | SaveContainer_SetMetadata | the 0x2028-byte `RGMH` record (§5) | high | V (6DB328) |
| 0x6DB410 / 0x6DB4A0 | SaveContainer_SetBlockCount / _SetBlock | block table `{size, offset, crc}` and data pointers | high | D |
| 0x6DBDF0 | SaveContainer_BeginWrite | offsets, `PCUD` header, CRCs | high | V (6DBDF0..6DBE7C) |
| 0x6DB810 / 0x6DB9B0 | SaveContainer_WriteStep / _ReadStep | overlapped WriteFile / ReadFile sequences | high | D |
| 0x6DB700 / 0x6DB750 | SaveContainer_Write / _Read | overlapped `WriteFile` / `ReadFile` at an offset | high | D |
| 0x596D80 / 0x596CE0 | CRC32 / CRC32_InitTable | MSB-first CRC-32, polynomial 0x04C11DB7, init and final xor 0xFFFFFFFF (CRC-32/BZIP2) | high | D |

## 2. The SAVE buffer (0x140004 bytes)

Two buffers: **working** `*0xC59258` (blocks are written here) and **committed** `*0xC5925C` (the last save point);
current-buffer base for allocation `*0xC57250`.

| Offset | Content | Evidence |
|---|---|---|
| +0x0 .. used | blocks, each `u32 header = (size_in_dwords << 8) | type`, payload follows | 0x556310 (V) |
| +0x1357F4 | used bytes (allocation cursor; capacity 0x1357F4) | 0x556310 |
| +0x135FF4 / +0x1361F4 / +0x13E1F8 / +0x13E9F8 / +0x13F9FC / +0x13FBFC | the six script variable stores (sizes from the gaps: 0x200, 0x8004, 0x800, 0x1004, 0x200, 0x404) [I: sizes from offsets] | 0x555820, 0x5558A0 |
| +0x140000 | 0xDEADBEEF sentinel (checked by RestoreGame) | 0x555280 (V), 0x557130 (V) |

Block types (0x556680 / 0x5563D0 / 0x556940, D; GAMEPLAY.md:3939 agrees):

| Type | Written for | Payload |
|---|---|---|
| 1 | a loaded stream unit | unit id, float (+8), count (byte +0xC), byte +0xD, 16-byte entries `{vec3 offset, index}` (moved terrain groups) |
| 2 | instance flag +0x18 bit 2 (script-saved) | +8 intro uid (instance +0x58), then the instance's own save data (object property 0x17: size + bytes) |
| 3 | 0x556020 class 1 (full intro) | 0x38 bytes + the property-0x17 data (filled by 0x555370) |
| 4 | class 2 (moved / rotated only) | 0x1C bytes, flags +0x18 / +0x1A for optional position (+0x20 vs intro) and rotation (+0x30), 12 bytes each |
| 5 | class 3 (flags only) | 0x14 bytes: +8 intro uid, +0xC flags (bits 0x100 / 0x200 from instance flags 0x1000000 / 0x2000000), +0xE, +0x10, +0x12 |
| 6 | game state | 0x41028 bytes (§3) |

The intro index 0xC57258 (0x800 pointers, count 0xC59260) lists the blocks of instances waiting for their unit;
0x555640 consumes them when the unit loads (type 3 → 0x5BB160 "INSTANCE_IntroduceSavedInstance", others 0x5BB640).

## 3. Game-state block (type 6, 0x41028 bytes)

Two blocks: `*0xC59264` (current, restored by 0x556E90) and `*0xC59268` (a second block in the same buffer; read by
the save-slot naming 0x789770 / 0x7898E0 and by SaveFile_Update, and partly preserved by RestoreGame [I, medium:
"last loaded / saved game" info]). Offsets from the block start (the header dword is +0):

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x08 | u16 | flags; bit 1 = 0x481C50() at save | 0x555BE0 |
| +0x0A | u16 | version **0xC2C** | 0x555BE5 (V), 0x556E98 (V) |
| +0x10 | char[0x80] | checkpoint unit name (player level data, property 0x62) | 0x555BE0 copies 0x21 dwords |
| +0x90 | i32 | checkpoint intro id ("ID wanted") | 0x556E90 |
| +0x94 / +0x114 | u8 / i32 | cleared by ClearMemory (+0x114: second checkpoint id read by 0x5D40C0, SAVES.md) | 0x556940 |
| +0x19C | u32 | `0xE7F154` | 0x555BE0 / 0x556E90 |
| +0x1A0 | u32 | seconds → game clock `0xE7F1E8` (µs, ×1 000 000) | 0x556E90 |
| +0x1D8..+0x1EC | u64 + 4×u32 | counter kept at its maximum across RestoreGame [I: play time / statistics] | 0x557130 |
| +0x1F2 | i16 | chapter index (< 0x12) → gamebase 0x23 and 0x24 (level record +0x34 or index+1) | 0x556E90, 0x556DD0 |
| +0x1F9 | u8 | (persistent block) > 7 adds the description to the file name | 0x78A3F0 |
| +0x200 / +0x202 | i16 | 0 / gamebase 0x13 | 0x556940 / 0x556E90 |
| +0x204 | i16 | −1 default | 0x556940 |
| +0x206 | u16 | 0x49A730() | 0x555BE0 |
| +0x208 | i16 | subtitle language (TEXT.md:23); survives RestoreGame | 0x557130 |
| +0x20C | i16 | player object override → `0xE7F1C8` (GAMEPLAY.md:288) | 0x555BE0 / 0x556E90 |
| +0x212 | i16 | gamebase 0x22 (default 1.0) | 0x556E90 |
| +0x218 | f32 | 2.0 default | 0x556940 |
| +0x220 / +0x223 | u8 | gamebase 0x2F / 0x44 (0 or 1.0) | 0x556E90 |
| +0x224 / +0x226 / +0x228 | i16 | 100 each; +0x224 → gamebase 0x28 | 0x556940 / 0x556E90 |
| +0x22C | u32 | flags; bit 0 = gamebase 0x12 ≠ 0 | 0x555BE0 |
| +0x230 | 10×u16 | inventory words (checkpoint) | 0x556DD0 (V) |
| +0x260 | 10×u16 | inventory words used when the chapter's flag byte `+0x424 + 20·chapter` is set | 0x556DD0 (V) |
| +0x278 | char[] | current unit name (0x5B2230) | 0x555BE0 |
| +0x338 / +0x33C / +0x340 | u32 | 0x5870A0 / 0x5870F0 / 0x587190 [I: music state] | 0x555BE0 |
| +0x344 / +0x348 | u32 | reverb / music settings → 0x582B90 | 0x555BE0 / 0x556E90 |
| +0x360..+0x3EC | | zeroed by ClearMemory (three 0xC-byte groups with 0xFF at +2, …) | 0x556940 |
| +0x3ED90 / +0x3EE90 | bits / u32 | collected rewards / count | tru_gog.csv 0x5DB6A0 |
| +0x3EF94..+0x3EFA4 | | difficulty preset, EnemyHP, AmmoAdj … | GAMEPLAY.md:4025, :4056 |

## 4. Checkpoint flow

```
checkpoint touched / first reward pickup / level start
   └─ 0x5570F0(0): SAVE_BufferAll → SAVE_WriteGameState → memcpy working → committed      (memory only)
death: DeathState 0x75AA50 → wipe 90/210 frames → 0x6F55A0 (flag 0x80000)
   └─ game loop 0x5DAB40 case 6 → 0x557130(0) RestoreGame: committed → working → 0x556E90 → level reload,
      instances born from their blocks, Lara on the checkpoint intro (0x5D40C0 / 0x698180)
save to disk (menu or autosave): SaveFile_Update states 1..4 / 5..8: 0x78A3F0 copies the COMMITTED buffer into the file
load from disk: states 9..0xC: file block → committed buffer → 0x557130(1)
```

A disk save therefore stores the last checkpoint, not the moment of saving [I, high: 0x78A3F0 copies
0x555B70() = the committed buffer, and no SAVE_SaveGame call precedes it in SaveFile_Update].

## 5. The PC file (`.TRUSave`)

Directory: 0x78B550 (strings `\Eidos\`, `Tomb Raider - Underworld`); names: `Autosave.TRUSave` or the slot name
(0x7898E0; `%s - %s` area / title, 0x789770) + `.TRUSave` (strings `.TRUSave`, `*.TRUSave`). State 8 copies
`Autosave.TRUSave` to `<area> - <title>.TRUSave` and sets FILE_ATTRIBUTE_READONLY on the copy (D).

Write order (0x6DB810, D) and fields (0x6DB2F0 / 0x6DBDF0, V for the constants):

| File offset | Size | Content |
|---|---|---|
| 0x0000 | 0x2028 | `RGMH` metadata: `u32 'RGMH' (0x484D4752)`, `u32 1`, `u32 0x2028`, `u32 extra offset (0x1C + 12·n, relative to 0x2028)`, `u32 0`, `u32 extra size`, 16-byte install GUID (registry), four UTF-16 fields of 0x400 chars at +0x28 / +0x828 / +0x1028 / +0x1828 (localised title from global data +0x1BC → +0x20C, description, slot name, empty) |
| 0x2028 | 0x1C | header: `u32 'PCUD' (0x44554350)`, `u32 0x100`, `u32 game 'SURT' (0x54525553)`, `u32 game version 0x100`, `u32 block count n (1)`, `u32 CRC32(block table)`, `u32 CRC32(previous 0x18 bytes)` |
| 0x2044 | 12·n | block table `{u32 size, u32 absolute offset, u32 CRC32(data)}` |
| 0x2044 + 12·n | extra size | extra data (pointer 0xFAF7AC, size 0xFAF7B0) [I, medium: the save thumbnail] |
| … | 0x140000 | block 0: the committed SAVE buffer without its sentinel |

The reader (0x6DB9B0) accepts files starting with either `RGMH` v1 or `PCUD` 0x100, checks the header CRC, and
SaveFile_Update state 10 requires game version 0x100, game id 0x54525553, one block of 0x140000 bytes.
[U] The reader fetches the extra data at the metadata's +0xC offset as if absolute; whether that is relative to
0x2028 at run time needs a real file.

## 6. Differences vs OpenUnderworld

| # | tru.exe | OpenUnderworld | Evidence |
|---|---|---|---|
| 1 | Binary memory image (1.25 MB) in an RGMH/PCUD container with CRC-32/BZIP2 | Text header + JSON body, zlib crc32, versioned with migration | docs/SAVES.md "Format"; src/save |
| 2 | Version mismatch (≠ 0xC2C) → save discarded, memory cleared | Older versions migrated, newer rejected | 0x556E90; SAVES.md "Versions and migration" |
| 3 | A disk save writes the committed (last checkpoint) buffer [I, high] | A slot saves the live world (`World::serialize…`) plus `checkpointSave` | SAVES.md "Body fields" |
| 4 | Files `<name>.TRUSave` / `Autosave.TRUSave` in `My Documents\Eidos\Tomb Raider - Underworld\`; the `savegame%02d.sav` ×10 table (0x7744E0) is the generic storage-category registration | docs/SAVES.md:220-223 names `savegame%02d.sav` (10 slots) as the PC slot files | strings `.TRUSave`, `*.TRUSave` in tru.exe; 0x78A3F0 / 0x78AB60. **SAVES.md correction needed** |
| 5 | The autosave is copied to a read-only `<area> - <title>.TRUSave` | `autosave` slot named "<AREA> - <TITLE>", protected from overwriting by the menus | SAVES.md "Where saves live"; src/ui/Ui.hpp:122 |
| 6 | RestoreGame keeps the subtitle language (+0x208) and the larger +0x1D8 counter | Options live outside saves (config) [I: no conflict] | 0x557130 |
| 7 | Instances saved per class (types 2-5) by intro uid; units by moved terrain groups (type 1) | Per-system JSON objects keyed by intro uid | SAVES.md body fields |
| 8 | Save buffer full → purge blocks (0x555FC0) or fatal "ran out of saved memory" | No size limit | 0x556310 |

Original `.TRUSave` import stays out of scope (SAVES.md "Import is not implemented"); §5 is now enough to unwrap the
container, and §2-§3 to start decoding block 0.

## 7. Unknowns

- 0x556020's instance classes (which objects are type 3 vs 4 vs 5) and the 0x38-byte type-3 layout (0x555370).
- The roles of +0x19C, +0x1D8..+0x1EC, +0x204, +0x218 and the 0x3xx groups.
- What fills the extra data (0xFAF7AC) and the order of the four metadata strings (passed through 0x599BF0).
- The PDA map's place in the buffer (0x6E4800 writes into "the save data block").
