# Install inventory (`ou-import`)

`ou-import` is a read-only inventory tool for a Tomb Raider: Underworld install. It writes a JSON
manifest. The source is in `src/importer/` and `tools/ou-import/`. It opens game files
read-only (`open(O_RDONLY)`/`pread`). It refuses any output path inside the game directory. That
check runs on the resolved path, so `<game>/../<game>/x` is refused too.

```
ou-import scan <gameDir> [-o manifest.json] [--deep] [--no-hash] [--names list.txt]... [-j N]
ou-import diff <a.json> <b.json>            # exit 0 identical, 1 different
ou-import extract <gameDir> <name|0xHASH> -o <file> [--spec 0xMASK] [--raw]
```

- Default manifest path: `$XDG_DATA_HOME/openunderworld/manifests/<sha256(tru.exe)[:16]>.json`.
  Without `XDG_DATA_HOME`, it falls back to `~/.local/share/...`.
- Header mode (the default) does three things:
  - Classifies every record by its magic.
  - For CDRM records, inflates only the CDRM blocks that the DRM header needs. That gives the
    section count, the section types and the dependency names.
- `--deep` inflates every CDRM fully and runs `cdc::parseDrm` on it. It also harvests
  NUL-delimited identifiers from DRM bodies to use as name candidates.
- `--names` loads external name lists. The manifest reports coverage from these lists separately
  from install-derived coverage.
- `extract` checks for `patch.000` first, then the bigfile, which is the override order the
  inventory found. It inflates CDRM unless `--raw` is given.

Every number below comes from the GOG build (`tru.exe` sha256 `1fdbd345…1011`), measured on
2026-10-18. Labels: **[obs]** observed or measured, **[inf]** inference, **[unk]** unknown.

## Executable

- **[obs]** PE machine i386. Timestamp 1229448662 (2008-12-16T17:31:02Z).
- **[obs]** VERSIONINFO: FileVersion and ProductVersion "1.1"; fixed version 1.1.0.0; CompanyName "Eidos Inc.".
- **[obs]** The known-builds table has one entry: `GOG 1900745791 (buildId 58082205556264096)`.
- **[obs]** A bigfile record with no name (hash `0x9475e80b`, 23 bytes) holds the ASCII string
  `Build 0680 (10-20-2008)`.

## Runtime

Test machine: 32 hardware threads. The install is 7.4 GiB on an HDD (Seagate ST4000DM004). All
runs used a Release build with `systemd-run --user --scope -p MemoryMax=6G`.

- "Cold" means every install file's page cache was evicted first, with
  `posix_fadvise(DONTNEED)` on read-only fds.
- "Warm" means a repeat run.
- **[obs]** All times are measured.

| mode | cold wall | warm wall | peak RSS | notes |
|---|---|---|---|---|
| header (default) | 78.5 s | 2.85 s | 77 MiB | cold time is the sha256+md5 of 7.86 GB (≈100 MB/s, disk-bound); record analysis 0.63 s cold, 0.02 s warm |
| header, `-j 1` | 76.3 s | – | – | one hashing thread is no slower on this HDD |
| `--deep` | 93.0 s | 4.0 s | 1.6–2.2 GiB | full inflate of 7.69 GB of DRM: 10.5 s cold, 1.13 s warm |
| `--deep --no-hash` | 65.5 s | 1.3 s | 1.7–2.2 GiB | the cold time is random reads from 32 threads |
| `--deep` + 2 name lists | – | 4.3 s | – | name matching 0.24 s |

- **[obs]** The ASan/UBSan Debug build took 13.0 s warm for `--deep`. No sanitizer reports.

## Files

- **[obs]** 37 regular files, 7,864,052,499 bytes in total.
- **[obs]** Detected types:

  | type | count |
  |---|---|
  | text | 8 |
  | rtf | 6 |
  | pe-dll | 5 |
  | bink-video | 5 |
  | bigfile-data | 3 |
  | ico | 3 |
  | pe-exe | 2 |
  | windows-shortcut | 1 |
  | bigfile-index+data | 1 |
  | bigfile-patch | 1 |
  | gog-hashdb(zip) | 1 |
  | inno-uninstall-log | 1 |

  No file is left as `unknown`.
- **[obs]** `goggame-1900745791.hashdb` layout:
  - It is a ZIP that holds one deflated table: {u32 12, u32 1, u32 count, count × (char path[1024], char md5hex[32])}.
  - Paths are UTF-8.
  - Of 24 entries, 23 md5-match the install and 1 is missing (`add\gameux.cmd`). None mismatch.

## Bigfile set

- **[obs]** `bigfile.000`–`.003` are referenced by records and all present (2.15 + 2.14 + 2.15 + 0.86 GB).
  No part is missing and no part is unreferenced.
- **[obs]** 7339 records, 3218 unique hashes, 0 record errors.
  - Every `compressedSize` field is 0.
  - Records per hash: 2188 hashes have 1 record, 1029 have 5 and 1 has 6.
- **[obs]** The 5-variant hashes are 1027 `.mul` and 2 `.raw`. Each variant uses one of the
  spec masks `0xffffe002`, `0xffffe004`, `0xffffe008`, `0xffffe010` or `0xffffffe1`.
  **[inf]** These are the five voice languages. **[unk]** Which bit is which language.
- **[obs]** The 6-variant hash is `pc-w\local\locals.bin`, with masks `0xfffff7e1/e2/e4/e8/f0` and
  `0xffffffe0`. **[inf]** These are the text-language variants.

Payload kinds, classified by magic:

| kind | records | bytes | note |
|---|---|---|---|
| `CDRM` | 1056 | 4,250,543,298 | all parse as DRM v19; 7,687,510,353 bytes inflated |
| `MUL?` | 5835 | 2,746,899,760 | heuristic header (hz 8k–96k, channels 1–12); **[obs]** every recovered name ends `.mul` (5835/5835) |
| `!WAR` | 270 | 282,840,668 | all named `.raw` (`pc-w\image\…`) |
| `Mus!` | 75 | 802,684 | music scripts; they name `\tr8\audio\music\<n>\…` |
| `FSB4` | 75 | 76,800 | all named `.sam` (FMOD sample-bank headers) |
| `PNG` | 8 | 574,174 | savegame icons |
| `DRM` | 1 | 720 | uncompressed DRM v19 |
| `unknown` | 19 | 797,319 | see below |

- **[obs]** DRM section-type totals across all 1057 DRMs (226,126 sections; 1 to 4262 per DRM):

  | section type | count |
  |---|---|
  | script | 56,071 |
  | wave | 40,783 |
  | dtpdata | 39,769 |
  | texture | 36,220 |
  | animation | 22,400 |
  | shaderlib | 16,769 |
  | material | 9,278 |
  | general | 2,011 |
  | renderresource | 1,863 |
  | object | 794 |
  | pdamap | 168 |

- **[obs]** 82 DRMs list object dependencies: 1019 references to 71 distinct names. No DRM uses the
  unit-dependency block.

## patch.000

- **[obs]** 1 part, 9 records, 4 hashes, 0 errors.
  - 3 records are CDRM: `pc-w\l2_puzzleroom.drm`, `pc-w\g_pda.drm` and `pc-w\generalbank.drm`.
    They hold 2792 sections and inflate to 43,720,045 bytes.
  - The other 6 records are the `locals.bin` language variants.
- **[obs]** Every patch record overrides a bigfile record with the same hash and the same spec
  mask (9/9). **[inf]** The game prefers the patch copy, as in TR7/TRAE (`extract` follows this).

## Name recovery

The path hash is CRC-32/MSB over the lowercased path. Install-derived candidates come from these
sources:

- exe literals and templates
- `objectlist.txt` (821/821 entries parsed)
- `unitlist.txt` (86/86)
- `objlist.dat` (86 of 87 entries are named)
- DRM dependency names
- music names from `Mus!` scripts
- `Movies/*.bik` stems mapped to `.sch`
- with `--deep` only: 61,224 identifiers harvested from DRM bodies

| | resolved unique hashes | coverage |
|---|---|---|
| header mode, install-derived | 1122 / 3218 | 34.9 % |
| `--deep`, install-derived | 1480 / 3218 | 46.0 % (249,413 candidates tried) |
| `--deep` + Gibbed.CrystalDynamics `tr8-{bigfile,patch}.filelist` | 3216 / 3218 | 99.94 % |

- **[obs]** The Gibbed list agrees with all 1480 install-derived names and conflicts with none. It
  adds 1736 names.
- **[obs]** Install-derived names by source:

  | source | names |
  |---|---|
  | objectlist.txt | 794 |
  | drm-strings | 358 |
  | mus-script | 225 |
  | unitlist.txt | 82 |
  | exe-literal | 15 |
  | exe-template | 5 |
  | movies-dir | 1 |

- **[obs]** Two hashes have no name: `0x9475e80b` (the build string above) and `0xa606131b` (a 71-byte CDRM that inflates to a 104-byte single-section DRM).
- **[obs]** The 1736 names that only the external list supplies are mostly audio:
  - 1599 `MUL?` under `pc-w\audio\`
  - 81 `*_map.drm`
  - 53 CDRM under `pc-w\cinstream\`
  - 2 other `.drm` and 1 `.raw`
- **[inf]** The install does not seem to list the `pc-w\audio\…` stream names anywhere.

## Unknown formats (no magic-based classifier yet)

| name | records | first bytes / note |
|---|---|---|
| `pc-w\local\locals.bin` | 6 + 6 (patch) | u32 0–5 (**[inf]** language index), then the string table; ~108–121 KB |
| `\tr8\game\pc\shaderlib\ShaderUsage_l1..l7.bin` | 7 | no common magic (`7e3bc303` ×2, `714c3100` ×2, `704c3100`, `dd02a400`, `b7c38505`); 5–11 KB |
| `pc-w\padshock\padshocklib.tfb` | 1 | u32 0x1f |
| `\tr8\pc-w\objlist.dat` | 1 | u32 count (87), then 132-byte records with name[128] first (parsed) |
| `\tr8\pc-w\objectlist.txt`, `\tr8\pc-w\unitlist.txt` | 2 | text: a count line, then entries (parsed) |
| `\tr8\local\localization\movies\prevontr.sch` | 1 | text starting "Vers…" (**[inf]** subtitle script) |
| unnamed `0x9475e80b` | 1 | text "Build 0680 (10-20-2008)" |

- **[unk]** How `MUL?` and `!WAR` are laid out internally, beyond the headers used for
  classification.

## diff

- **[obs]** `diff` compares these fields:
  - files: size and sha256
  - records: keyed by hash/specMask; compares size, part, offset, kind and error
  - the exe sha256
- **[obs]** On the real install, all of these diffs reported `identical`:
  - two header scans
  - a header scan against a deep scan
  - a deep scan against a deep + name-list scan
- **[obs]** A synthetic test edited a manifest (changed one file hash, one record size, and
  removed one patch record). `diff` reported exactly those three changes and exited 1.
- **[obs]** `diff` does not compare names or DRM details. Name coverage depends on the mode, so it
  is left out on purpose.
