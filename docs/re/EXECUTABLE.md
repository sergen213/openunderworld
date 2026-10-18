# tru.exe — identification

Target: GOG build (gameId 1900745791, buildId 58082205556264096),
`tru.exe` sha256 `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011`.
Tags: **[OBS]** observed in the file or decompile, **[INF]** inference, **[?]** unknown.

## PE headers [OBS]

| Field | Value |
|---|---|
| Machine | i386 (0x14c), 32-bit, GUI subsystem 4.0 |
| TimeDateStamp | 2008-12-16 17:31:02 UTC (`MainG2_InitEngine` logs `Dec 16 2008 18:30:39`) |
| Linker | 8.0 (Visual Studio 2005) |
| ImageBase / Entry | 0x400000 / 0x818421 (MSVC CRT `WinMainCRTStartup`, in `.text`) |
| DllCharacteristics | 0 (no ASLR, no NX-compat flag) |
| Debug dir | CodeView PDB `D:\tr8\output\win32_gm\game\tr8_win32_gm.pdb` (age 1; PDB not shipped) |
| VERSIONINFO | Eidos Inc., "Tomb Raider: Underworld", FileVersion/ProductVersion 1.1 |
| Rich header | none recognised (bytes at 0x80..0xD8 are non-zero but no `Rich` marker) [?] |
| Security directory | empty (no attached Authenticode) |

| Section | VA | VSize | RawSize | Notes |
|---|---|---|---|---|
| .text | 0x401000 | 0x43d000 | 0x43d000 | code, entropy 6.59 (normal compiled code) |
| .rdata | 0x83e000 | 0x50000 | 0x50000 | IAT at 0x83e000, RTTI, strings, vftables |
| .data | 0x88e000 | 0x847000 | 0x31000 | ~8.2 MB zero-initialised globals |
| .shad | 0x10d5000 | 0x5000 | 0x5000 | compiled D3D9 shader bytecode (`ps_2_0`, `ps_3_0`, `vs_1_1`, "Microsoft (R) HLSL Shader Compiler 9.19.949.1104") |
| .rsrc | 0x10da000 | 0x15a000 | 0x15a000 | entropy 7.95 (icons/dialog/compressed resources) |
| .extra | 0x1234000 | 0x2000 | 0x2000 | PKCS#7 SignedData blob (VeriSign/Thawte timestamping certs); not referenced by the security directory [INF: leftover of the retail signing] |

**Packing / DRM:** no packer (sane entropy, full import table, entry in `.text`), no
SecuROM/StarForce/Steam strings or sections. The GOG executable is DRM-free as shipped.
Nothing was bypassed or modified.

The game directory additionally contains GOG-side wrappers that are *not* part of the
original game: `D3D9.dll` + `dxwrapper.dll/.ini` and `DEPHook.asi`. Because the game loads
`d3d9.dll` with `LoadLibraryA` (see below), the local `D3D9.dll` is picked up first [INF].

## Compiler / runtime [OBS]

MSVC 2005 static CRT (`___tmainCRTStartup` 0x818241, `__security_check_cookie` /GS cookie
at 0x8BD7E8, `__SEH_prolog4`), C++ with RTTI enabled (990 vftables recovered,
`tools/ghidra/scripts/ExportRtti.java`). Namespaces seen: `cdc::` (engine, 290 classes),
`FMOD::` (51), `bfx::` (35, a third-party AI/flocking/navigation library; its code sits roughly at 0x401000-0x430000 [INF]),
plus game classes (`UberObjectSection`, `PCXInputDevice`, `Screenshots::*`, ...).

## Imports [OBS]

Static imports: KERNEL32 (127), USER32 (55), ADVAPI32 (registry, 13), GDI32, COMCTL32 (#17
InitCommonControls), ole32, OLEAUT32, SHELL32 (ShellExecuteW), WINMM (`timeGetTime`,
`timeSetEvent`, waveIn/waveOut), MSACM32, WS2_32 (sockets; FMOD net-stream code references `icy-*` headers [INF: used by FMOD]), and
`binkw32.dll` (12 Bink video functions, `_BinkOpen@8` ... `_BinkSetSoundSystem@8`).

Dynamically loaded:

| DLL | Where | How |
|---|---|---|
| `d3d9.dll` | `PCDeviceManager_Create` 0x5239C0 | `LoadLibraryA` + `GetProcAddress("Direct3DCreate9")`, called with 0x20 (D3D_SDK_VERSION) |
| `xinput9_1_0.dll` | 0x4824F0 | `LoadLibraryW`, `XInputGetState`, `XInputSetState` (`PCXInputDevice`) |
| `dsound.dll`, `dsound3d.dll`, `avrt.dll`, `openal32.dll` | FMOD Ex (statically linked, `..\src\fmod_*.cpp` paths) | FMOD output plugins |

No `dinput8`/DirectInput is used (no import, no string) [OBS]; keyboard/mouse go through
`PCKeyboardDevice`/`PCMouseDevice` classes [INF: Win32 messages / Get*State].
`fmodex.dll` ships in the folder but is **not** imported and its name is not referenced;
FMOD Ex is linked statically [OBS].

## Reproducing

```
tools/ghidra/analyze.sh            # first run: import + auto-analysis (~90 s) + post-scripts
tools/ghidra/analyze.sh            # later runs: apply names + RTTI labels, re-export (~6 s)
tools/ghidra/decompile.sh 0x5dfbe0 GAMELOOP_UpdateTime @list.txt   # -> $OU_WORK/ghidra/decomp
tools/ghidra/xrefs.sh QueryPerformanceCounter 0xe7f188 str:BIGFILE  # TSV of references
tools/ghidra/analyze.sh --decompile-all                              # exports/decomp_all.c
```

Exports land in `$OU_WORK/ghidra/exports` (`functions.tsv`, `strings.tsv`, `imports.tsv`,
`rtti.tsv`). All outputs are proprietary-derived and stay outside the repo. Names live in
`tools/ghidra/names/tru_gog.csv` (address,name,confidence,evidence) and are re-applied on
every `analyze.sh` run; see [FUNCTIONS.md](FUNCTIONS.md).
