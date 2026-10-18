# Container formats: bigfile, patch.000, CDRM, DRM v19, locals.bin

Scope: the PC GOG "1.1 GOG v3" install (`tru.exe` sha256 `1fdbd345…1011`). Parser: `src/cdc/Cdc.{hpp,cpp}`.
Payload formats inside the containers (textures, meshes, audio codecs) are out of scope here except for a magic-number inventory.

Evidence tags (same as [RESEARCH.md](../RESEARCH.md)):

| Tag | Meaning |
|---|---|
| **[V-data]** | Observed by scanning **every** record of the GOG install with a throwaway Python script (2026-10-18). Counts below are exact. |
| **[V-exe]** | Observed in our Ghidra decompilation of `tru.exe` (`openunderworld-work/ghidra/exports/decomp_all.c`). Function names such as `ArchiveFileSystem_Open` are our own Ghidra labels (`tools/ghidra/names/`). |
| **[S]** | Read in a reference's source (cloned under `openunderworld-work/research/`). |
| **[I]** | Our inference. |
| **?** | Unknown. |

All integers are little-endian.

---

## 1. Bigfile (`bigfile.000` … `bigfile.003`)

### 1.1 Layout

```
u32 count
u32 hash[count]                      // sorted ascending; duplicates allowed
struct { u32 size; u32 sector; u32 specMask; u32 compressedSize; } record[count]
...payloads...
```

| Fact | Evidence |
|---|---|
| `count` = 7339; index ends at 4 + 20·7339 = 146784; first payload at sector 72 (= 147456, i.e. the index is padded to the next 2048-byte boundary). | [V-data] |
| `hash[]` is sorted ascending; 3218 distinct hashes (1030 hashes have several records, see §1.3). 3218 is also the size of gibbed's TR8 `bigfile.filelist`. | [V-data], [S] |
| `compressedSize` is 0 in all 7339 records. Bigfile-level compression is never used; compression lives inside payloads (CDRM, §3). | [V-data] |
| Lookup is a binary search over `hash[]`, then a scan of equal-hash neighbours for a matching `specMask` (§1.3). `FUN_004726e0`. | [V-exe] |

### 1.2 Offsets and split parts

- Absolute offset = `sector << 11` (×2048). Part index = `offset / 0x7FF00000`; offset within part = `offset % 0x7FF00000`. [V-exe]: `__aulldvrm(sector * 0x800, 0x7ff00000)` in `FUN_00472890` and `FUN_00473020`; `ArchiveFileSystem_Open` (`0x4730B0`) uses the same constant to count parts. [V-data]: every record resolves inside its part and no record straddles a part boundary; every CDRM record starts with the `CDRM` magic under this mapping.
- Part sizes: `.000` 2145345536, `.001` 2139930624, `.002` 2146330624, `.003` 857427968 bytes. Records reference parts 0–3. [V-data]
- The game opens the archive as `"BIGFILE.DAT"`; `ArchiveFileSystem_Open` strips the extension and appends `.000` (literal `0x3030302e`), and logs `"Num of files in big file %d"`. [V-exe] (call in `InitArchive`, `0x473840`)
- Yura agrees: "in Underworld and later use 0x7FF00000"; Legend/Anniversary use `0x9600000` (`Yura/OpenDialog.xaml:69-76`). [S]

### 1.3 Path hash

`hash = CRC32(lowercase(path))`, MSB-first, polynomial `0x04C11DB7`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF` (no bit reflection). Paths use backslashes.

- [V-exe]: `ArchiveFileSystem_CalculateHash` (`0x472670`) copies the path, calls `_strlwr`, then a CRC routine. BigfileLogger's pattern (`BigfileLogger/src/Main.cpp:128`) lands on the caller of this function. [S]
- [V-data]: these names resolve to existing hashes: `pc-w\lara.drm` → `0x5FF91900`, `pc-w\local\locals.bin` → `0x7CD333D3` (6 records), `\tr8\pc-w\objectlist.txt` → `0x4823BFDD`, `pc-w\generalbank.drm` → `0xB3AAAFD6`, `pc-w\g_pda.drm` → `0x87776B6B`, `pc-w\l2_puzzleroom.drm` → `0x49CD2C36`.

### 1.4 Specialisation mask (`specMask`)

**Selection rule** [V-exe]: a record matches when `(record.specMask & fs.specMask) == fs.specMask`, i.e. the record's mask must contain **all** bits of the file system's current mask (`FUN_004726e0`). The archive file system's mask is initialised to `1` (`DAT_009ce288[2] = 1` in `InitArchive`, `0x473840`) and later changed through the `SetSpecialisationMask` vtable slot. DRM sections use the same superset rule (§4.4).

> `ou::cdc::Bigfile::find(path, specMask)` implements this rule (default mask `kSpecEnglish` = 1); `ou::cdc::BigfileSet` puts patch.000 first (§2).

Observed masks [V-data]:

| specMask | Records | Content |
|---|---|---|
| `0xFFFFFFFF` | 2188 | Everything language-independent (all 1056 CDRM/DRMs, music, FSB4, 260 `!WAR`, 700 `.mul`, text files) |
| `0xFFFFFFE1` | 1029 | Voice: 1027 `.mul` + 2 `!WAR`, English (bit 0; also bits 5–31) |
| `0xFFFFE002` / `E004` / `E008` / `E010` | 1029 each | Same 1029 hashes in French / German / Italian / Spanish (bits 1/2/3/4; bits 5–12 clear) |
| `0xFFFFF7E1` `F7E2` `F7E4` `F7E8` `F7F0` | 1 each | `locals.bin` EN / FR / DE / IT / ES text (bit 11 clear) |
| `0xFFFFFFE0` | 1 | `locals.bin` Dutch text (bits 0–4 clear) |

Interpretation:

- Bits 0–4 = English, French, German, Italian, Spanish. [V-data] by locals.bin string content (§5); matches Yura's `SpecialisationFlags` (`Yura/Formats/SpecialisationFlags.cs`). [S]
- Bit 11 = Dutch. [I], strong: with a current mask of `1<<11`, only the Dutch `locals.bin` matches (English text has bit 11 cleared), while the English voice record (`0xFFFFFFE1`) still matches. That yields Dutch text with English voice, which fits the absence of a Dutch dub.
- Bits 5–10 and 12: unused here; probably further language slots. [I]
- Bit 31: Yura names it `GfxNextGen` [S]; it is a TR7 concept. In TRU every mask has bit 31 set, so it never discriminates. [V-data], [I]

### 1.5 Payload inventory (first bytes of all 7339 records)

[V-data] unless a row says otherwise.

| Kind | Count | Identification |
|---|---|---|
| `.mul` | 5835 | No magic. 16-byte header `{u32 sampleRate, i32 loopStart, u32 numSamples, u32 channels}`, matches vgmstream `mul.c` [S, web]. Rates 31874–48011 (mostly ~44100 ± a few Hz); channels 1 (5260), 2 (524), 4 (40), 6 (11); loopStart = −1 in 5739. Codec: cdc IMA ADPCM in sound packets, see [AUDIO.md](AUDIO.md) [V-exe, V-data] |
| `CDRM` | 1056 | Compressed DRM, §3 |
| `!WAR` | 270 | cdc "RAW!" image (magic stored byte-reversed). Example header: `!WAR`, 0x80, 0, size 0x12C000, 3, 0x10, width 640, height 480: a 640×480×4 loading screen [V-data]; field semantics per cdcengine.re `/docs/files/image/` [S, web] |
| `FSB4` | 75 | FMOD Sound Bank 4: header version `0x40000`, header mode `0x22` in all 75; first sample mode `0x400020` in all 75 (probably IMA ADPCM mono, from FMOD's FSB4 flag values) [I] |
| `Mus!` | 75 | Music scripts. Each of the 75 sits directly next to an `FSB4` record in sector order [V-data]; pairing semantics [I] |
| PNG | 8 | `89 50 4E 47` |
| other | 20 | 6× `locals.bin` (§5); `\tr8\pc-w\objectlist.txt` (CSV `821`, `1,generalbank`, …); a text file `86\r\ncontainer\co…`; `"Build 0680 (10-20-2008)"` (23 bytes, a data-build stamp); `"Version 1\r\nTextEntry…lang_english …"` (movie subtitle/text table); **one uncompressed DRM v19** (hash `0x737C378F`, 720 bytes: 4 sections OBJECT + 3 DTPDATA, parses exactly); 9 binary blobs with unknown layouts |

---

## 2. `patch.000`

- Same layout as the bigfile (§1.1), single part. 9 records; index ends at 184; all 9 hashes also exist in `bigfile.000`. [V-data]
- Contents: `pc-w\l2_puzzleroom.drm` (13751556 B), `pc-w\g_pda.drm` (2037679 B), `pc-w\generalbank.drm` (4445187 B), all CDRM with mask `0xFFFFFFFF`; plus the six `locals.bin` language variants with the same masks as in `bigfile.000`. [V-data]
- **Priority** [V-exe]: `Win32_GameMain` (`0x479C80`) calls `InitPatchArchive("PATCH.000")` (`0x478930`) after language selection and aborts with `"Error loading %s"` on failure. `InitPatchArchive` opens a second `ArchiveFileSystem` and calls `MultiFileSystem::Add(fs, bReprioritize=0, bAddToFront=1)` (`0x472C50`), which shifts existing entries and inserts at index 0. The `MultiFileSystem` lookup (`FUN_00472b70`) walks entries from index 0 and returns the first one whose `FileExists` succeeds, so **patch.000 overrides bigfile**. `MultiFileSystem` entries are 0x28 bytes, entry count at `+0x57C`.
- ModLoader and TRLAU hook exactly this function to insert their own file system (ModLoader `src/ModLoader.cpp:87-94,124`; TRLAU `src/modules/ModLoader.cpp:62`). [S]

---

## 3. CDRM (compressed DRM wrapper)

```
char magic[4] = "CDRM"
u32  version            // 0 in all 1056
u32  numBlocks          // 1..401
u32  padding            // 0 in all 1056
struct { u32 type:8, uncompressedSize:24; u32 compressedSize; } block[numBlocks]
pad to 16
block data, each block padded to 16 bytes except after the last one
```

| Fact | Evidence |
|---|---|
| Header and block table as above | Templates `tr8_cdrm.bt` [S]; [V-data] |
| Block type: 0 empty, 1 stored, 2 zlib (`uncompress`). **All 29871 blocks in the install are type 2.** | [S] for 0/1, [V-data] |
| Every non-final block decompresses to exactly `0x40000` bytes (28815/28815). | [V-data] |
| The last block ends exactly at the record's end, so no trailing pad (1056/1056). | [V-data] |
| The exe checks the `CDRM` magic and `numBlocks < limit`, then reads a `numBlocks·8 + 0x10` byte table; on failure it calls `ERROR_Fatal("CDRM file corrupt")`. `ResolveDecompressor_ParseCdrm` `0x552BF0`. | [V-exe] |

The `version`/`padding` semantics and type-1 handling have not been exercised. Our parser supports types 0/1/2 (`decompressCdrm`).

---

## 4. DRM version 19

1056 DRMs are CDRM-wrapped and one is stored raw (hash `0x737C378F`, §1.5). That is why parsers must accept both (`parseDrm` does). After decompression, the parse consumes every byte exactly in all 1057 files. The statistics below cover the 1056 CDRM-wrapped files. [V-data]

### 4.1 Header

```
u32 version             // 19
u32 depNamesBytes       // size of the first NUL-separated name block
u32 depNamesBytes2      // 0 in all 1056 files
u32 unk3                // 0 in all 1056
u32 flags               // 0 in all 1056
u32 numSections
SectionHeader sections[numSections]   // 20 bytes each, starting at +24
char names[depNamesBytes]             // NUL-separated
char names2[depNamesBytes2]
per section: relocation table, then data
```

| Fact | Evidence |
|---|---|
| Version check: `if (hdr[0] != 0x13) ERROR_Fatal("Wrong DRM version data %x, code %x\nrebuild of %s needed\n")` in `ResolveReceiver_ReceiveData` (`0x54F9B0`). | [V-exe] |
| The exe reads `numSections = hdr[5]`, copies `numSections × 0x14` bytes of section headers from `hdr + 24`, allocates and keeps the `hdr[1]` name block, and keeps `hdr[2]` and `hdr[4]`. If `hdr[4] & 1`, it rounds `hdr[2]` so that `hdr[2] + 4·numSections − 8 + hdr[1]` is 16-aligned. | [V-exe] |
| 82 DRMs have a non-empty first name block; `hdr[2]`, `hdr[3]`, `hdr[4]` are 0 everywhere. Example `pc-w\l2_puzzleroom.drm`: 100 bytes = `g_binoculars, g_grapple, g_grenade_thrower, g_lighter, g_pda, g_personal_light, g_tanks, wp_handgun` (names of other DRMs: `pc-w\g_pda.drm` exists). | [V-data] |
| Same field order in cdcEngineTools: `version, nameSize, paddingSize, unk, unk, numSections` (`DRM/DRM.h:47-60`). | [S] |
| The parser treats block 1 as object dependencies and block 2 as unit dependencies. That naming is [I]; block 2 is never populated, and the exe's alignment use of `hdr[2]` suggests "padding" rather than a second list (?). | [I] |

### 4.2 Section header (20 bytes)

| Offset | Type | Field | Observed |
|---|---|---|---|
| 0 | u32 | data size | |
| 4 | u8 | section type (§4.3) | |
| 5 | u8 | flags | 0 on disk in all 226122 sections. At runtime the exe ORs in bit 0 for sections rejected by the spec-mask test. [V-exe] |
| 6 | u16 | versionID | |
| 8 | u32 | packed: bit 0 `hasDebugInfo`, bits 1–7 `resourceType`, bits 8–31 relocation table size **in bytes** | |
| 12 | u32 | id (resource id for non-General sections) | |
| 16 | u32 | specMask | `0xFFFFFFFF` in all 226122 sections |

Sources: Templates `tr8_section.bt` [S]; cdcEngineTools `DRM/DRM.cpp:156-160` (TR8 reloc size is bytes, TR7 is `count×8`) [S]; [V-data].

### 4.3 Section types [V-data] (1056 DRMs, 226122 sections)

| Id | Name (Templates) | Count | `resourceType` values seen |
|---|---|---|---|
| 0 | GENERAL | 2011 | 0 |
| 1 | EMPTY | 0 | — |
| 2 | ANIMATION | 22400 | 0 |
| 3 | PUSHBUFFER_WC | 0 | — |
| 4 | PUSHBUFFER | 0 | — |
| 5 | TEXTURE (`PCD9`) | 36220 | 5 |
| 6 | WAVE (FSB, per cdcEngineTools `.fsb`) | 40783 | 0 |
| 7 | DTPDATA | 39766 | 0 (1889), 11, 12, 13 (9057), 14, 15, 16 (8398), 17, 19 (14484), 20 (793), 21 (82), 22 (2345), 25 (793), 28, 32 (1037), 34 (501) |
| 8 | SCRIPT | 56071 | 0 |
| 9 | SHADERLIB | 16769 | 0 |
| 10 | MATERIAL | 9278 | 18 |
| 11 | OBJECT | 793 | 30 |
| 12 | RENDERRESOURCE (mesh; `.tr8mesh`) | 1863 | 24 (336), 26 (1527) |
| 13 | PDA_MAP | 168 | 0 |

- First section: OBJECT in 793 DRMs (object DRMs; 793 OBJECT sections in total, so exactly one per object DRM), GENERAL in 83, PDA_MAP in 82, DTPDATA in 54, RENDERRESOURCE in 43, SHADERLIB in 1. All 82 DRMs with a dependency list start with GENERAL (probably level units). [V-data], [I] for the "level" label.
- Texture ids: 36220 texture sections carry only 6438 distinct ids, and 2882 ids occur once. Textures are therefore duplicated across DRMs and keyed by id. [V-data]; sharing at runtime by id is [I].
- `PDA MAP DRM "%s" Has a mesh and/or waterMesh!` and related strings (`FUN_006df600`) confirm PDA-map DRMs carry their own mesh. [V-exe]

### 4.4 Section loading and spec masks [V-exe]

In `ResolveReceiver_ReceiveData`, a section is loaded only if `(section.specMask & fs.GetSpecialisationMask()) == fs.GetSpecialisationMask()`. Otherwise its pointer slot is set to 0 and flags bit 0 is set. Accepted sections are dispatched through `g_sectionHandlers[type]` (vtable slot `+0x28`). The function also filters by a requested section type, where `0xE` means "any".

### 4.5 Relocation table (when `relocBytes > 0`)

```
u32 count[5]
struct { u32 offset; u32 target; }                      intern[count[0]]   // pointer at data+offset -> data+target (same section)
struct { u64 sectionIndex:14, offset:24, target:26; }   extern[count[1]]   // into another section of this DRM
u32 { offset:25, sectionType:7 }                        kind2[count[2]]
u32 { offset:25, sectionType:7 }                        resource32[count[3]]
u32 { offset:25, sectionType:7 }                        resource16[count[4]]
```

| Fact | Evidence |
|---|---|
| Layout and field names | Templates `tr8_section.bt` (`Relocation3`, `ResourceRelocation32/16`) [S] |
| `relocBytes == 20 + 8·c0 + 8·c1 + 4·(c2 + c3 + c4)` for all 95808 sections that have a table. | [V-data] |
| Totals: intern 3552671, extern 677085, kind2 34609, resource32 **0**, resource16 1213552. | [V-data] |
| All 3552671 intern `offset+4` and `target` fall inside their own section's data. | [V-data] |
| All 677085 extern `sectionIndex` values are `< numSections` of the same DRM. This supports "index into this DRM's section list". | [V-data], [I] for the meaning |
| The semantics of kind2/resource16 (which resource, which table, meaning of 16 vs 32) | ? |

`ou::cdc::internalPointer()` implements the intern kind; `ou::cdc::sectionPointers()` resolves intern and extern (`lo & 0x3FFF` = section index, patch = `((lo >> 14) | (hi & 0x3F) << 18) * 4`, target = `hi >> 6`, per `ResolveSection_ApplyRelocations` 0x54EC60). The extern layout is verified by the level structures in [LEVELS.md](LEVELS.md).

Templates' `SECT` magic (`0x54434553`) belongs to a tool export header, not to on-disk data. [V-data]: none of the 226122 on-disk section payloads starts with it.

---

## 5. `pc-w\local\locals.bin` (UI text)

```
u32 language            // EN 0, FR 1, IT 2, DE 3, ES 4, NL 5
u32 numStrings          // 5136 in all six
u32 offset[numStrings]  // absolute; 0 = no string
UTF-8 NUL-terminated strings
```

| Record (bigfile.000) | specMask | `language` | First strings |
|---|---|---|---|
| 108396 B | `0xFFFFF7E1` | 0 | `Complete`, `Load failed.`, `Saving data...` |
| 120854 B | `0xFFFFF7E2` | 1 | `Terminé`, `Echec du chargement !` |
| 115585 B | `0xFFFFF7E8` | 2 | `Completato`, `Caricamento fallito.` |
| 114934 B | `0xFFFFF7E4` | 3 | `Erledigt`, `Laden gescheitert.` |
| 112580 B | `0xFFFFF7F0` | 4 | `Finalizado`, `Error al cargar.` |
| 110573 B | `0xFFFFFFE0` | 5 | `Voltooid`, `Laden mislukt.` |

- [V-data]: string 1 is at `offset[1] = 8 + 4·5136 = 20552`, so the table is directly followed by the strings; `offset[0] = 0`.
- The `language` header value order (IT = 2, DE = 3) **differs** from the specMask bit order (DE = bit 2, IT = bit 3). [V-data]
- Yura's `LocaleFile.cs` layout (`language, numStrings, offsets`) matches [S], but its `Language` enum (German 2, Italian 3, …) does not match TR8's header values. [V-data]
- `patch.000` carries updated copies of all six (§2).
- Loader/getter semantics, id ranges and where ids come from: [TEXT.md §1](TEXT.md#1-pc-wlocallocalsbin).

---

## 6. Open container questions

1. `hdr[2]`/`hdr[3]`/`hdr[4]` semantics (padding vs second dependency list; the flag bit 0 alignment rule).
2. kind2 / resource16 / resource32 relocation semantics: needed for material, object and render-resource linking.
3. DTPDATA `resourceType` sub-ids (0–34): which DTP schema each one is.
4. `Mus!` sequence semantics (`.mul` codec and FSB4 sample mode: [AUDIO.md](AUDIO.md)).
5. Names for the 20 "other" payloads and for hashes not in any public file list.
6. Runtime spec mask actually used per language (bits beyond 0–4 and 11); whether bit 31 is ever cleared.
