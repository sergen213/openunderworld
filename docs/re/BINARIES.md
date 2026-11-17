# Original binary inventory (GOG install)

Audit baseline 2026-11-16 of `"/mnt/1st4TB/Tomb Raider Underworld"` (GOG gameId 1900745791,
buildId 58082205556264096, installed through Heroic). Read-only: nothing in the game directory
was executed or modified for this document.

This extends [EXECUTABLE.md](EXECUTABLE.md) (tru.exe identification, imports, dynamic loads) and
[RESEARCH.md §1](../RESEARCH.md) (edition, GOG wrappers, patch history). Archive and movie formats
are in [formats/CONTAINERS.md](../formats/CONTAINERS.md) and [formats/VIDEO.md](../formats/VIDEO.md);
the boot-time mount order is in [FILESYSTEM.md](FILESYSTEM.md). Corrections to those documents are
listed in §8.

Tags: **[OBS]** seen in a file or command output; **[INF]** inference.

Reproduce:

```sh
G="/mnt/1st4TB/Tomb Raider Underworld"; W=$OU_WORK/re-baseline/BinaryInventory
(cd "$G" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > $W/game.sha256
python3 tools/re/pe_inventory.py --json $W/pe.json --imports-tsv $W/imports_all.tsv \
    "$G"/{tru.exe,D3D9.dll,dxwrapper.dll,DEPHook.asi,binkw32.dll,fmodex.dll,unins000.exe}
```

`tools/re/pe_inventory.py` uses `pefile` (2024.8.26 here). Scratch outputs live in
`openunderworld-work/re-baseline/BinaryInventory/`: `game.sha256` (sha256sum format, all 42 files),
`game.md5`, `pe.md`/`pe.json` (full per-PE fact sheets), `imports_all.tsv`, `tru_imports.tsv`
(266 imports), `tru_strings_{ascii,utf16}.txt`.

## 1. File inventory and origin

Origin codes:

- **O**: original Eidos / Crystal Dynamics / Nixxes content.
- **G**: added by GOG's package. Each **O** and **G** file is listed in the package's own
  `goggame-1900745791.hashdb`, see §2.
- **I**: written at install time by the GOG Inno Setup installer or by Heroic.
- **R**: written at run time.
- **L**: local non-game files written into the game directory after install (by the user or
  by agents). Not part of any release.

| File | Size (B) | SHA-256 | Origin / identity |
|---|---|---|---|
| `tru.exe` | 6422528 | `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011` | O. Game executable 1.1.0.0 (§3) |
| `bigfile.000` | 2145345536 | `4e9bede0316d63d9501e139b0edc9bae3d4d533a5eaea4ca3d1c61c1adc7705a` | O. Bigfile part 0 with the index (§6) |
| `bigfile.001` | 2139930624 | `c2c6fbe1967a0287983a98f762b9f3a19d47adebd385d4bc1d233ea2620e18c1` | O. Bigfile part 1 |
| `bigfile.002` | 2146330624 | `c25b8485d16c8ec5603fe450bcd79a7c256fbd2a256a00c7c2bf21bc1496cc93` | O. Bigfile part 2 |
| `bigfile.003` | 857427968 | `b0a092f7862627e55e2831eddcc7ee97a96ff6adb8b18b4076fbff206ad993c5` | O. Bigfile part 3 |
| `patch.000` | 20924416 | `c06631ad419f4f7e65cc4ec49316618ed8c05eb1356b2c392aefb3d9c9974705` | O. 1.1 patch archive (§6) |
| `Movies/credits.bik` | 66420200 | `bb8de7755494127ccff93c127e9c14a025cb3e87260c9780891285fb747adade` | O. Bink 1 `BIKi` |
| `Movies/explode.bik` | 35596216 | `60f58fcca7d3ef36179cc430354e15bfe82d6cc2159fba1af68fb8e6b671b74d` | O. Bink 1 `BIKi` |
| `Movies/intro.bik` | 240262084 | `939af01aab9a39456da510fa63300d4a5098fe5364825c6c09bc95c78ce78d79` | O. Bink 1 `BIKi` |
| `Movies/medsea_loop.bik` | 60076556 | `56c21ca0523f0ec0947f288a34f792671019b2a0e935a0ae12fbbd05eee56075` | O. Bink 1 `BIKi` |
| `Movies/prevontr.bik` | 128026204 | `bf85554fbf02e2f56d57accb569d28cf295a3732694f0aa05b4817504791da48` | O. Bink 1 `BIKi`, 5 audio tracks |
| `readme.rtf` | 544376 | `44eff4d081f2cccf339d84927417a1a2037fc1434e7f461e75dd75c65b2cd830` | O. English readme |
| `Leesmij.rtf` | 540236 | `a7b67bd3650e6be0af7200a0e981549d04ef5e7346d8448e1f93b26dc3011ea9` | O. Dutch readme |
| `Leggimi.rtf` | 619296 | `2038c0335dab1ca21292ada9f1082cd4d2bd641e623cafdfcda0b9d2075294a2` | O. Italian readme |
| `LisezMoi.rtf` | 554710 | `addedd0269b14b3b8e3e48a9a629c6a1157206c1b7a8c2d81cd4c1bef5164534` | O. French readme |
| `Léeme.rtf` | 561736 | `3e0eb33f10969fa2f1453818c214489f6ec1b078c283e6d2e352a8103618bf66` | O. Spanish readme |
| `Readme-Datei.rtf` | 629036 | `06a25b55d46695c684c1351698d8546080e055ff9deb838eecaebdb87797c103` | O. German readme |
| `fmodex.dll` | 330504 | `5f9ad8f5d6a1d652804cfa1f0284d60ff23a8a0f09b7263637b48c8a78e24fa7` | O [INF: retail redistributable, timestamp 2008-10-19]. FMOD Ex 4.16.9, UPX-packed, signed. Unused: tru.exe links FMOD statically (§4.6) |
| `binkw32.dll` | 2169856 | `baba99929487b005bb9b168acfd852550055f22e5f1059c9032765209bb185e5` | G. **Ultimate ASI Loader**, not RAD's DLL. Embeds four RAD binkw32 builds (§4.5) |
| `D3D9.dll` | 421376 | `50dcc63931d884292bef5f5615ed03034e357a5122d9e8b417b8d72dc56f96b7` | G. DxWrapper stub 1.0.300.1 (§4.2) |
| `dxwrapper.dll` | 7939584 | `e2066fbdef2d7de4421ff11edb44cf028c4ed03590f4b08da836264b47ba71c2` | G. DxWrapper 1.2.7200.25 (§4.3) |
| `dxwrapper.ini` | 3152 | `4eba00563bb312be1137309e49c86af22a96fb4a4acc21e452c53426080f992e` | G. DxWrapper configuration (§4.3) |
| `DEPHook.asi` | 9216 | `f5267a6a267686455dc776478e389ff9d37f8479246118d2d945833527949bde` | G. Calls `SetProcessDEPPolicy(0)` on attach (§4.4) |
| `goggame-1900745791.hashdb` | 951 | `2eef06c731b1ccd4dd740ba610ada748dc80ee2880677abd29ca8f49e0bf8834` | G. ZIP with the package MD5 list (§2) |
| `goggame-1900745791.info` | 1075 | `4d45ab4c65ea362050c0463ea14df4e9a62c5c2224ac680cf02029b1ee7d7d9e` | G. Play tasks: `tru.exe`; `tru.exe -setup` |
| `goggame-1900745791.script` | 5203 | `4c35fa94601828d701b789cc999e610a523de5b7bd80d5ba00e0e243c08821bd` | G. Registry, save-folder and `gameux.cmd` actions (see RESEARCH.md §1.2) |
| `gog.ico` | 69248 | `f6a71321521ba89713f0bd38b21f809e87e8a789cb172e8e4693f9479e30b1e4` | G. Icon (mtime 2017-09-28) |
| `support.ico` | 62895 | `05fe749ef47d1ec862d6c55be78e66d1011226bf1f78409acf57cb79cec5eb20` | G. Icon (mtime 2019-08-26) |
| `goggame-1900745791.ico` | 112222 | `76e1e822857883e1169ea032d3d7e47f92109ac95dbca68e46ef9e0dff2633ae` | I. Icon written by the installer (`goglog.ini` `File_0`) |
| `unins000.exe` | 1343048 | `5761e7789d813626cd68ee1e62429cfeb92bdd814cd29ef12fc4ae9ec1dbaff3` | I. Inno Setup 5.6.2 (Unicode) uninstaller (§4.7) |
| `unins000.dat` | 1327471 | `e3029a35dd353e86fcf2deca257de620ba4007f544be71ebee19d48e18caf96c` | I. Inno uninstall log |
| `unins000.msg` | 23147 | `e43d06ec2b3dbe3d81bcd6b7880d28d074dac54b38646a605cfc5c809939da16` | I. Inno messages |
| `unins000.ini` | 41 | `f43712ede068cf4c37124b4d48a29d6ae7676db01b1b47e0e8ecf9d9407be902` | I. `[InstallSettings] productID=1900745791` |
| `goglog.ini` | 1528 | `7de320af03f1b2820ce9edcb88d6acf11a7f58e9d8c8199d32a57fdbe587ea86` | I. Uninstall record; Wine prefix (`C:\users\steamuser`), install path `X:\Games\Heroic\Tomb Raider Underworld` |
| `Launch Tomb Raider - Underworld.lnk` | 710 | `3cb419509f9913fbcf42386c8efc494889a1db0975eecce537d799fab4f32b76` | I. Shortcut to `X:\Games\Heroic\Tomb Raider Underworld\tru.exe` |
| `dxwrapper-tru.log` | 745 | `3e0b58a7601ae2c9cd1374480830794f7431ec4ce9bac2ef779cd753d5ae41aa` | R. DxWrapper log of a Wine run on 2026-11-02 (RADV NAVI31, "Steam game detected!") |
| `goal.txt` | 18077 | `5ee7e5b31dd93f054857cb7702bfd3385c16b606232d54c211253364924108de` | L. Project goal prompt (Markdown text) |
| `gameplaygoal.md` | 44511 | `d1eecafefc42d338be796fb7a67039c414f2252bfe92ee566c462138f504c353` | L. Project goal prompt |
| `-key.pem` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | L. Empty (sha256 of zero bytes). Created 2026-10-30, owner group `sergeng`, unlike the game's `plugdev` |
| `.pem` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | L. Empty, same creation time as `-key.pem` |
| `.claude-flow/console/.gitignore` | 66 | `b4ff23d1890a234593b3339a2e7d90864a7ca591a8718e3a7638db45bad954c5` | L. Agent tool state |
| `.claude-flow/console/events.jsonl` | 519 | `b8ba636077334434215351925d580b208c677902dd8a53e9da8e23cac1e30767` | L. Agent tool state |

Total: 42 files. The **O** data is 7.37 GB: bigfiles 7289034752 B, movies 530381260 B,
patch 20924416 B.

**The key-pem question [OBS]:** `-key.pem` and `.pem` are not key material. Both are empty, and they
are neither game nor GOG files. Their name pattern suggests a mis-parsed `openssl … -out -key.pem`
style command run with this directory as the working directory [INF]. The same goes for
`goal.txt`, `gameplaygoal.md` and `.claude-flow/`: the game directory is meant to be treated as
read-only, but it has picked up local writes. They are harmless for hashing purposes because
none of them is in the GOG hashdb. Moving them out is the user's call.

## 2. GOG package verification [OBS]

`goggame-1900745791.hashdb` is a ZIP holding one member of the same name. Its layout: a 12-byte
header (`12, 1, 24`), then 24 records of 1056 bytes: `char name[1024]` (Windows-1252, NUL-padded)
followed by `char md5_hex[32]`. Every record except `add\gameux.cmd` (GOG support directory, not
installed here) was compared with the local `md5sum`:

| Result | Files |
|---|---|
| **23 / 23 match** | `tru.exe` (`76b4da2f…`), `bigfile.000`–`003`, `patch.000`, 5 × `Movies/*.bik`, 6 readmes, `fmodex.dll`, `binkw32.dll`, `D3D9.dll`, `dxwrapper.dll`, `dxwrapper.ini`, `DEPHook.asi` |
| not installed | `add\gameux.cmd` (`3864fd00…`): `.script` action `regUX`, run once as admin from `{supportDir}/add` |

So the install is byte-identical to GOG build 58082205556264096. The compatibility files
(`D3D9.dll`, `dxwrapper.*`, `DEPHook.asi`, the ASI-loader `binkw32.dll`) are GOG-shipped, not
community additions made by the user. RESEARCH.md §3 dates them to GOG's "1.1 GOG v3" update
(2024-11). Their PE timestamps (2024-09-16 and 2024-10-29) agree.

## 3. Ghidra input check [OBS]

| Source | SHA-256 |
|---|---|
| `openunderworld-work/ghidra/input.sha256` | `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011` |
| `sha256sum tru.exe` (2026-11-16) | `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011` |

**Match.** The Ghidra project, `exports/*` and `tools/ghidra/names/tru_gog.csv` (182 names) all
describe exactly the shipped GOG 1.1 executable.

## 4. Per-PE facts [OBS]

Every value below is from `pe_inventory.py` (pefile). The full JSON is in `pe.json`. Perm =
section R/W/X; entropy is Shannon bits per byte over the raw section.

### 4.0 Overview

| | tru.exe | D3D9.dll | dxwrapper.dll | DEPHook.asi | binkw32.dll | fmodex.dll | unins000.exe |
|---|---|---|---|---|---|---|---|
| Machine / format | i386 PE32 EXE | i386 PE32 DLL | i386 PE32 DLL | i386 PE32 DLL | i386 PE32 DLL | i386 PE32 DLL | i386 PE32 EXE |
| TimeDateStamp (UTC) | 2008-12-16 17:31:02 | 2024-09-16 23:56:37 | 2024-09-16 23:56:52 | 2024-10-29 11:28:09 | 2022-02-08 16:41:03 | 2008-10-19 20:10:48 | 2018-08-20 10:26:21 |
| Linker | 8.0 | 14.41 | 14.41 | 14.41 | 14.30 | 8.0 | 2.25 (Delphi) |
| Subsystem | GUI 4.0 | GUI 6.0 | GUI 6.0 | GUI 6.0 | GUI 6.0 | GUI 4.0 | GUI 5.0 |
| ImageBase | 0x400000 | 0x10000000 | 0x10000000 | 0x10000000 | 0x10000000 | 0x10000000 | 0x400000 |
| Entry RVA / VA | 0x418421 / 0x818421 (`.text`) | 0x271d9 / 0x100271d9 | 0x1670ab / 0x101670ab | 0x1368 / 0x10001368 | 0x2d9fb / 0x1002d9fb | 0x1787f0 / 0x101787f0 (**`UPX1`**) | 0x106618 / 0x506618 (`.itext`) |
| DllCharacteristics | 0x0 | 0x140 (DYNAMIC_BASE, NX) | 0x140 | 0x140 | 0x140 | 0x0 | 0x8140 (+TS-aware) |
| Header checksum | 0 (computed 0x62a87a) | 0 | 0 | 0 | 0 | 0x5a289 (valid) | 0x14a4f8 (valid) |
| Relocations | **none** (`RELOCS_STRIPPED`) | yes | yes | yes | yes | yes (12 B, UPX stub) | yes |
| TLS | none | none | 2 callbacks (0x101669ed, 0x10166a71) | none | directory, 0 callbacks | none | directory, 0 callbacks |
| Load config | **none** (no SafeSEH table, no CFG) | 192 B, cookie 0x1005e080, SafeSEH 41 | 192 B, cookie 0x10764080 | 192 B, SafeSEH 1 | 188 B, SafeSEH 115 | none | none |
| Debug dir | CodeView `D:\tr8\output\win32_gm\game\tr8_win32_gm.pdb`, GUID 54d105fb-c9e5-4076-a18b-15abe9ddbcd1, age 1 | `…\dxwrapper\bin\Release\stub.pdb` (+VC_FEATURE, POGO, ILTCG) | `…\dxwrapper\bin\Release\dxwrapper.pdb`, age 50 | `C:\dev\Projects\DEPHook\Release\DEPHook.pdb` | `C:\projects\ultimate-asi-loader\bin\Win32\Release\dinput8.pdb` | none | none |
| Rich header | **absent**: overwritten by a `VLV` block (§5.1) | VS2022 17.11 (Linker1400 b34120) | VS2022 (b34120) | VS2022 (b34123) | VS2022 17.0 (b30709) | VS2005 RTM (Linker800 b50727) | n/a (Delphi) |
| Overlay | none | none | none | none | none | 5384 B = Authenticode | 7240 B = Authenticode |
| Authenticode (security dir) | none (detached copy in `.extra`, §5.1) | none | none | none | none | **present** | **present** |
| Packer / DRM | none | none | none | none | none | **UPX** (`UPX0` raw 0, `UPX1` entropy 7.93) | none |
| Exports | 0 | 1151 (`stub.dll`) | 1193 | 0 | 1178 (internal name `vorbisfile.dll`, incl. 81 `_Bink*`) | 663 | 0 |
| FileVersion / company | 1.1 (1.1.0.0) / Eidos Inc. | 1.0.300.1 / "Sadrate Presents" (Elisha Riedlinger) | 1.2.7200.25 / same | none | 1.0.0.0 (fixed 1.2.0.0) / ThirteenAG | 4.16.9 / Firelight Technologies | 51.1052.0.0 ("Setup/Uninstall") |

Imports (DLL → number of functions):

| PE | Imports |
|---|---|
| tru.exe (266) | KERNEL32 127, USER32 55, WINMM 21, WS2_32 18, ADVAPI32 13, binkw32 12, ole32 9, MSACM32 6, OLEAUT32 2, GDI32 1, COMCTL32 1, SHELL32 1. Full list: `tru_imports.tsv` (file, dll, ordinal, name, IAT VA). Per-function xrefs: Ghidra `exports/imports.tsv` |
| D3D9.dll | KERNEL32 86, USER32 1 |
| dxwrapper.dll | USER32 176, KERNEL32 159, GDI32 142, ADVAPI32 7, MSIMG32 3, OLEACC 2, SHLWAPI 2, OLEAUT32 1 |
| DEPHook.asi | KERNEL32 12 (`SetProcessDEPPolicy` + CRT startup), VCRUNTIME140 3, api-ms-win-crt-runtime 8 |
| binkw32.dll | KERNEL32 127, WS2_32 39, USER32 17, dbghelp 6, SHELL32 2, ole32 2, GDI32 1, d3d9 1 |
| fmodex.dll | KERNEL32 3, then one each from ADVAPI32, MSACM32, ole32, USER32, WINMM, WSOCK32 (UPX stub IAT) |
| unins000.exe | user32 182, kernel32 163, gdi32 72, comctl32 30, advapi32 25, oleaut32 20, ole32 12, shell32 8, mpr 5, version 3, comdlg32 2, msimg32 1 |

### 4.1 tru.exe sections

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x401000 | 0x43d000 | 0x43d000 | R-X | 6.590 |
| `.rdata` | 0x83e000 | 0x50000 | 0x50000 | R-- | 5.674 |
| `.data` | 0x88e000 | 0x847000 | 0x31000 | RW- | 4.163 |
| `.shad` | 0x10d5000 | 0x5000 | 0x5000 | R-X | 4.357 |
| `.rsrc` | 0x10da000 | 0x15a000 | 0x15a000 | R-- | 7.953 |
| `.extra` | 0x1234000 | 0x2000 | 0x2000 | --- (characteristics 0) | 5.608 |

The last section ends exactly at EOF (raw 0x61e000 + 0x2000 = 0x620000 = 6422528), so there is
no overlay. `.rsrc` is dominated by 11 × 122615 B PNG `__GDF_THUMBNAIL` resources, which explains
its high entropy (§5.3). It is not packed.

### 4.2 D3D9.dll — DxWrapper stub

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x10001000 | 0x43806 | 0x43a00 | R-X | 6.48 |
| `.rdata` | 0x10045000 | 0x1891c | 0x18a00 | R-- | 5.58 |
| `.data` | 0x1005e000 | 0x45cc | 0x2200 | RW- | 4.43 |
| `.rsrc` | 0x10063000 | 0x700 | 0x800 | R-- | 3.67 |
| `.reloc` | 0x10064000 | 0x7b88 | 0x7c00 | R-- | 6.59 |

FileDescription: "Stub for DxWrapper. Supports: bcrypt.dll, cryptsp.dll, d2d1.dll, d3d8.dll,
d3d9.dll, …, winmm.dll". It exports the union of those DLLs' APIs (1151 names). `tru.exe` loads
`d3d9.dll` with `LoadLibraryA` (EXECUTABLE.md), so Windows' DLL search order picks up this local
stub, which in turn loads `dxwrapper.dll` [INF: DxWrapper's documented stub design;
not traced].

### 4.3 dxwrapper.dll and dxwrapper.ini

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x10001000 | 0x1984dc | 0x198600 | R-X | 6.64 |
| `.rdata` | 0x1019a000 | 0x5c9468 | 0x5c9600 | R-- | 6.67 |
| `.data` | 0x10764000 | 0x628c0 | 0x7000 | RW- | 4.38 |
| `.rsrc` | 0x107c7000 | 0x630 | 0x800 | R-- | 3.39 |
| `.reloc` | 0x107c8000 | 0x28c8c | 0x28e00 | R-- | 6.80 |

Elisha Riedlinger's DxWrapper 1.2.7200.25 (GitHub `elishacloud/dxwrapper`). The "tages" and
"steam_api" string hits are DxWrapper's own game-detection tables, not DRM in TRU. Every
`dxwrapper.ini` toggle is 0 / AUTO except `[d3d9] LimitPerFrameFPS = 60` and the dsound
primary-buffer defaults (16 bit, 44100 Hz, 2 ch). Whether the FPS limit applies while
`EnableD3d9Wrapper = 0` was not verified here. RESEARCH.md §1.2 attributes GOG's 60 FPS cap
to it [INF].

### 4.4 DEPHook.asi

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x10001000 | 0xe54 | 0x1000 | R-X | 5.84 |
| `.rdata` | 0x10002000 | 0x88e | 0xa00 | R-- | 3.94 |
| `.data` | 0x10003000 | 0x3dc | 0x200 | RW- | 0.36 |
| `.rsrc` | 0x10004000 | 0xf8 | 0x200 | R-- | 2.51 |
| `.reloc` | 0x10005000 | 0x16c | 0x200 | R-- | 5.16 |

Verified in assembly (`objdump -d`, 0x10001000): the user DllMain does
`if (reason == DLL_PROCESS_ATTACH) SetProcessDEPPolicy(0); return TRUE;`, i.e. `push 0;
call [0x10002000]`. **It turns DEP off for tru.exe.** This upgrades RESEARCH.md's "[I] passes 0"
to observed. The ASI loader in `binkw32.dll` loads it.

### 4.5 binkw32.dll — Ultimate ASI Loader with embedded RAD Bink

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x10001000 | 0x59bdf | 0x59c00 | R-X | 6.58 |
| `.rdata` | 0x1005b000 | 0x2656c | 0x26600 | R-- | 5.43 |
| `.data` | 0x10082000 | 0x93928 | 0x1a00 | RW- | 3.64 |
| `.rsrc` | 0x10116000 | 0x18aa48 | 0x18ac00 | R-- | 6.79 |
| `.reloc` | 0x102a1000 | 0x4f80 | 0x5000 | R-- | 6.69 |

The version resource reads "Ultimate ASI Loader", ThirteenAG, MIT, with UpdateUrl
`https://github.com/ThirteenAG/Ultimate-ASI-Loader`. Strings include `binkw32Hooked.dll`, `*.asi`,
`scripts\`, `plugins\` and `global.ini`. Embedded `RT_RCDATA` resources:

| Id | Size | Content |
|---|---|---|
| 101 | 37376 | PE, export name `vorbisfile.dll` (2018-08-12) |
| 103 | 778752 | `wndmode.dll` "DirectX Windower Embedded v2.3 (based on D3D Windower v1.88)" 2.3.0.12 |
| 104 | 525 | default `[WINDOWMODE]` ini text |
| 105 | 166912 | RAD `binkw32.dll` **1.8r** (2006-10-05) |
| 106 | 187392 | RAD `binkw32.dll` 1.9u (2010-04-27) |
| 107 | 227328 | RAD `binkw32.dll` 1.99w (2012-11-28) |
| 108 | 215552 | RAD `binkw32.dll` 1.994i (2014-10-24) |

So the original RAD Bink code is present, as embedded DLLs. The loader's own sections are a
normal `.text/.rdata/.data/.rsrc/.reloc` set with no `RADCODE`/`RADDATA`: those section names
belong to the embedded RAD DLLs. Which embedded build TRU ends up using was not traced [INF: the
loader picks one by the importer's needs]. The game's own movies are `BIKi` (§6), which all four
builds decode.

### 4.6 fmodex.dll — FMOD Ex 4.16.9, unused

| Section | VA | VSize | Raw | Perm | Entropy |
|---|---|---|---|---|---|
| `UPX0` | 0x10001000 | 0x132000 | 0x0 | RWX | 0.0 |
| `UPX1` | 0x10133000 | 0x46000 | 0x45a00 | RWX | 7.932 |
| `.rsrc` | 0x10179000 | 0xa000 | 0x9800 | RW- | 5.858 |

UPX-packed, with the entry point in `UPX1`, and Authenticode-signed (overlay WIN_CERTIFICATE,
5384 B). Built with VS2005 RTM (Rich: Utc1400 C/C++ and Linker800, build 50727). `tru.exe` neither
imports nor names it (EXECUTABLE.md). The exe contains the dword 0x00041609 (FMOD_VERSION
4.16.09) exactly once [OBS], so the statically linked FMOD in tru.exe is the same 4.16.9
[INF: the dword is unique, but its use as the version constant was not traced].

### 4.7 unins000.exe — Inno Setup 5.6.2 uninstaller

Delphi linker 2.25; sections `.text .itext .data .bss .idata .tls .rdata .reloc .rsrc`. Strings
"Inno Setup Setup Data (5.6.2) (u)" and "Inno Setup Messages (5.6.2) (u)"; manifest
`JR.Inno.Setup`; Authenticode-signed overlay of 7240 B. GOG installer infrastructure only.

## 5. tru.exe in depth

### 5.1 Header oddities: `VLV` block and detached Eidos signature [OBS]

- File offsets 0x40–0xCB, where the DOS stub, the "This program cannot be run in DOS mode" text
  and a Rich header would normally sit, hold `56 4C 56 00` ("VLV\0"), u32 1, u32 0x00674000,
  then 128 high-entropy bytes. `e_lfanew` = 0x100. This is why EXECUTABLE.md found no `Rich`
  marker. The layout (`VLV`, version, signed size, RSA-1024 signature) matches Valve's Steam
  executable signature block [INF]. The recorded size 0x674000 is 0x54000 bytes larger than the
  file (0x620000) [OBS]. That is consistent with the GOG exe deriving from a Steam build signed
  before a section, such as a SteamStub `.bind`, was removed [INF]. There are no Steam strings,
  no `steam_api` import and no `.bind` section [OBS].
- `.extra` (raw 0x61e000, 0x2000 B, characteristics 0) starts with a WIN_CERTIFICATE header
  (`dwLength` 0x1508, revision 0x0200, type 2 = PKCS#7), followed by SignedData from **Eidos Plc**
  (London; VeriSign Class 3 Code Signing 2004 CA; timestamp `081216173104Z`, two seconds after
  the PE timestamp). It has 5381 non-zero bytes. The security data directory is zero, so
  Windows does not see a signature. This agrees with RESEARCH.md §1.1.

### 5.2 Compiler and runtime identification

The Rich header is destroyed, so no tool build numbers are available. Identification rests on
other evidence [OBS]:

| Evidence | Points to |
|---|---|
| Linker version 8.0, OS/subsystem 4.0 | Visual Studio 2005 (MSVC 14.0, `_MSC_VER` 1400) |
| `__EH_prolog3` (0x8187f2, 47 callers), `__EH_prolog3_catch`, `__EH_prolog3_GS`, `__SEH_prolog4` (0x81d094, 49 callers) | VS2005+ static CRT helpers (`/GS`-era EH prologs) |
| `___security_init_cookie` 0x8286be, `__security_check_cookie` 0x813be6 (436 callers) | `/GS` buffer-security cookies |
| C++ EH FuncInfo magic dwords: 0x19930520 ×6, 0x19930521 ×8, **0x19930522 ×43** | VS2005 `__CxxFrameHandler3` FuncInfo (0x19930522 = with EH-flags field). 36 handler stubs (`mov eax, FuncInfo; jmp ___CxxFrameHandler3@0x8184b4`) found by byte scan |
| `___tmainCRTStartup` 0x818241, entry `WinMainCRTStartup` 0x818421; no `MSVCR80` import | **Static** MSVC 2005 CRT (`/MT`) |
| "Microsoft Visual C++ Runtime Library", "Runtime Error!", "bad allocation" | MSVC CRT/STL messages |
| No load config, no relocations, DllCharacteristics 0 | Default VS2005 settings at the time: no SafeSEH table, no ASLR, no NX opt-in |

Function names are from Ghidra `exports/functions.tsv` (18,766 functions, 559 with non-`FUN_`
names); the addresses are Ghidra's.

### 5.3 Statically linked libraries and embedded data

| Library / data | Evidence [OBS] |
|---|---|
| **FMOD Ex 4.16.9** (Firelight) | 50 `FMOD::` RTTI classes (DSound/EAX2-4, OpenAL/EAX2-5, codecs MPEG/WAV/FSB/raw, DSP effects); 62 `..\..\src\fmod_*.cpp` paths; dword 0x00041609 ×1 |
| **NavPower** (BabelFlux; namespace `bfx::`) | 31 `bfx::` RTTI classes (Mover3D, NavGraph, KDTree, Planner...), `<bfx_replay version="%u" NavPower_version="%d.%d.%d" …>`, "NavPower Warning: %s", `BFX LEAK` |
| **IJG libjpeg 6b** | "6b  27-Mar-1998", "Copyright (C) 1998, Thomas G. Lane", full JPEG error-message table [INF: used for screenshots / PDA images] |
| D3D9 shader bytecode (`.shad`) | 76 aligned shader version tokens in `.shad`: vs_1_1 ×44, ps_3_0 ×26, ps_2_0 ×4, vs_3_0 ×2. The exe has 25 "Microsoft (R) HLSL Shader Compiler 9.19.949.1104" CTAB strings |
| Vista Game Explorer GDF | `.rsrc` DATA `__GDF_XML` ×11 languages (UTF-16): version 1.1.0.0, release date 2008-11-18, publisher Eidos, developers **Crystal Dynamics and Nixxes Software BV**, WinSAT min 4 / rec 5.9, saved games `Documents\Eidos\Tomb Raider - Underworld`. `__GDF_THUMBNAIL` ×11 PNG (122615 B each) |
| Other resources | 4 dialogs (101, 102, 125, 126; the `-setup` launcher), 1 icon `GAME_ICON`, version, manifest (`Eidos.Tomb.Raider.Underworld`, `asInvoker`) |

Not found (string search over the whole exe) [OBS]: zlib (`deflate`/`inflate`/"incorrect
header check"), Lua, Havok, PhysX, Granny, Scaleform, D3DX, DirectInput, SecuROM/StarForce/
SolidShield/Tages. The `drm` substring hits are `.drm` asset paths. Physics, animation and
scripting are cdc engine code (`cdc::` classes). FaceFX appears only as the cdc `FaceFxNode`
anim-node DTP name.

### 5.4 RTTI families (from `ghidra/exports/rtti.tsv`) [OBS]

There are 990 vftables, which belong to **963 distinct classes**. The raw file holds 1014 RTTI
TypeDescriptor names (`.?AV/.?AU/.?AW…@@`), which includes classes without a vftable.

| Family | Classes | Notes |
|---|---|---|
| `cdc::` | 269 | Engine: allocators, anim graph nodes (`cdc::Anim*Node`), render, resource, physics |
| `Nt*` (global) | 198 | Script-native types; most are `Nt…_ScriptType` (script type descriptors), plus `NtAnimatedSequence`, `NtCineSequence`... |
| `Enemy*` (global) | 184 | AI behaviour tree: `EnemyBehaviorTree_{Creature,Soldier,Thrall,…}`, `EnemyBehavior_*` |
| `Ns*` (global) | 52 | Script base classes (`NsActor`, `NsLaraBase`, `NsEnemyBase`, `NsDLCSystem`...) with `_ScriptType` twins |
| `FMOD::` | 50 | FMOD Ex internals |
| `bfx::` | 31 | NavPower |
| Other global game classes | ~150 | Player states (`CrawlState`, `CrouchState`, `DeathState`, `Swim*`...), cameras (`CinematicCamera`, `CombatCamera`...), `PC*` devices (`PCXInputDevice`...), `ArchiveFileSystem`, sections (`DTPDataSection`, `AnimationSection`, `UberObjectSection`) |
| `Screenshots::` 9, `PDAMap::` 3, `AsyncTaskManager::` 2, `std::` 2, `Ne*` 5, `df::` 1, anonymous 1 | 23 | |

### 5.5 String inventory [OBS]

`strings -n5` finds 17,883 ASCII and 682 UTF-16LE runs. Ghidra defines 4,157 strings
(`exports/strings.tsv`). Heuristic split of the ASCII runs (first matching rule wins):

| Category | Count |
|---|---|
| Identifier-like (no spaces, ≥5 chars) | 2940 |
| RTTI TypeDescriptor names | 1012 |
| Prose / messages | 749 |
| printf format strings | 661 |
| FMOD source paths | 62 |
| Asset paths (`.drm` `.mul` `.dtp`...) | 26 |
| Other source paths | 18 |
| Short / noise (code bytes misread as text) | 12415 |

## 6. Data file headers [OBS]

Formats are in CONTAINERS.md and VIDEO.md; these are the observed header values for this install.

| File | Header facts |
|---|---|
| `bigfile.000` | u32 count = **7339** (0x1CAB); hashes sorted, 3218 distinct; index ends at 146784; first payload at 147456 (sector 72, 2048-byte alignment). All `compressedSize` = 0. Highest payload end 7296731159 = part 3 + 857425943, inside `bigfile.003` (857427968 B). Parts 0–2 are under the 0x7FF00000 part stride |
| `bigfile.001`–`.003` | No header; raw continuation data (001 starts with a `CDRM` payload, 003 with a `.mul` header `44ac0000 ffffffff`) |
| `patch.000` | count = **9**, 4 distinct hashes (3 DRMs + one `locals.bin` hash × 6 language masks), index ends at 184, first payload at 2048, payload end 20924238 |
| `Movies/*.bik` | All `BIKi` (Bink 1, revision i); the size field + 8 equals the file size for all five. 1280×720 except `credits` 960×720. Frames: intro 3600, prevontr 1932, explode 533, medsea_loop 900, credits 12163. Audio tracks: 1 each, except `prevontr` with 5 |

These counts agree with CONTAINERS.md §1–2 and VIDEO.md.

## 7. What OpenUnderworld needs

| File | Needed by OU? |
|---|---|
| `bigfile.000`–`003`, `patch.000` | **Yes**: all game assets (`ou::cdc::BigfileSet`, patch first) |
| `Movies/*.bik` | **Yes**: decoded with libavcodec (`src/video/Movie.hpp`, `tools/ou-movie`) |
| `tru.exe` | Not at run time. It is the RE reference: Ghidra, the Wine comparison harnesses, Wine-measured bounds |
| `D3D9.dll`, `dxwrapper.*`, `DEPHook.asi`, `binkw32.dll` (ASI loader), `fmodex.dll` | **No.** Windows/Wine compatibility shims around tru.exe (ENVIRONMENT.md: "ignored by the native port"). They matter only when running the original under Wine for reference, where the DxWrapper FPS limit and the DEP-off hook change tru.exe's behaviour relative to retail 1.1 [INF] |
| readmes, icons, GOG metadata, Inno files, `.lnk` | No |
| L-class files | No; not game files |

## 8. Corrections to existing documents

| Document | Claim | Correction |
|---|---|---|
| RESEARCH.md §1.1 | PE TimeDateStamp "2008-12-16 **12:31:02 UTC**" | 0x4947E5D6 = **17:31:02 UTC** (12:31 is US Eastern). EXECUTABLE.md has it right |
| EXECUTABLE.md | Rich header "none recognised (bytes at 0x80..0xD8 are non-zero but no `Rich` marker) [?]" | The region 0x40–0xCB is a `VLV` signature block (§5.1). The Rich header was overwritten |
| RESEARCH.md §1.2 | binkw32 "contains RAD sections (`RADCODE`/`RADDATA`)", "[I] that the original Bink code is embedded" | The loader has no RAD sections. Four RAD binkw32 builds (1.8r, 1.9u, 1.99w, 1.994i) are embedded as RCDATA 105–108 [OBS]. Its resources show `wndmode.dll` and a `vorbisfile.dll` stub but no separate d3d8to9 resource (it may be compiled into the code; not checked) |
| RESEARCH.md §1.2 | DEPHook "[I] it passes 0" | Observed in assembly: `push 0; call SetProcessDEPPolicy` on `DLL_PROCESS_ATTACH` |
| RESEARCH.md §1.2 | (implicit) wrappers are community additions | All of them are in GOG's own hashdb with matching MD5s (§2) |
| EXECUTABLE.md | `bfx::` "a third-party AI/flocking/navigation library" | It is **NavPower** (BabelFlux), per the `NavPower_version` replay-header string |
