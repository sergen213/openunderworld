# Research notes: Tomb Raider: Underworld (PC, 2008, cdc engine "TR8")

This file is a compact index of what is known, where it comes from, and how much to trust it.
Verified container formats are documented separately in [formats/CONTAINERS.md](formats/CONTAINERS.md).

Evidence tags:

| Tag | Meaning |
|---|---|
| **[V]** | Verified locally against the read-only GOG install: static byte inspection with Python/objdump/openssl, or Ghidra export lookups. No game or third-party code was executed. |
| **[S]** | Read in a reference's source code or docs (cloned under `openunderworld-work/research/`). The reference was not run. |
| **[W]** | Web secondary source (wiki, forum, news). Not independently confirmed. |
| **[I]** | Our inference. |
| **?** | Unknown. Listed under [Open questions](#open-questions). |

Research was done on 2026-10-18. The clones are at the commits listed in [§4](#4-sources).

---

## 1. Our install: edition and binaries

### 1.1 `tru.exe`

| Property | Value | Tag |
|---|---|---|
| sha256 | `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011` | [V] |
| VS_VERSION_INFO | FileVersion and ProductVersion `1.1` (1.1.0.0); CompanyName "Eidos Inc."; translation 0800/04b0 | [V] |
| PE TimeDateStamp | **2008-12-16 12:31:02 UTC**, the release day of the official 1.1 patch ([TRC](https://www.tombraiderchronicles.com/headlines3583.html)) | [V], [W] |
| Linker | MSVC 8.0 (VS2005); image base `0x400000`; `DllCharacteristics = 0` (no NX-compat or ASLR flag) | [V] |
| Sections | `.text` `.rdata` `.data` `.shad` `.rsrc` `.extra`. There is no `.securom` or other packer section. | [V] |
| `.shad` (20 KB) | Precompiled D3D9 shader bytecode: version tokens such as `ps_2_0` and `CTAB` constant-table comments | [V] |
| `.extra` (8 KB) | A WIN_CERTIFICATE PKCS#7 blob signed by **Eidos Plc** (VeriSign Class 3 Code Signing 2004 CA), but the PE security directory is zeroed. The signature is therefore detached: the binary was post-processed after Eidos signed it. | [V], [I] |
| Static imports | KERNEL32, USER32, ADVAPI32, **BINKW32** (12 Bink calls), MSACM32, OLE32/OLEAUT32, SHELL32, COMCTL32, GDI32, WINMM, WS2_32 | [V] |
| Dynamic loads | `d3d9.dll`, `dsound.dll`, `openal32.dll`, `dsound3d.dll`, `avrt.dll`, `mscoree.dll`, and `XInputGetState`/`XInputSetState` | [V] |
| Middleware in exe | **FMOD Ex is statically linked** (RTTI `FMOD::ChannelDSound…`, `FMOD::ChannelOpenALEAX2..5`, `FMOD::CodecMPEG`). **NavPower** (`NavPower_version`, `<bfx_replay …>`). IJG libjpeg (`Adobe APP14 marker`). RTTI `PCXInputDevice`. String "ATI Driver Version … Forcing Fetch4 shadows". | [V] |
| Command-line flags | Strings `-setup` (the GOG "Game Settings" task runs `tru.exe -setup`), `-config`, `-relaunchmce` | [V] |

### 1.2 Third-party files shipped by GOG

These were identified from strings and headers only. None were executed.

| File | sha256 (prefix) | Identification | Tag |
|---|---|---|---|
| `dxwrapper.dll` | `e2066fbd…` | elishacloud dxwrapper. `dxwrapper-tru.log` reports **v1.2.7200.25**, run under Wine/Proton via Heroic (path `X:\Games\Heroic\…`). | [V] |
| `dxwrapper.ini` | — | All compatibility toggles are 0 except **`[d3d9] LimitPerFrameFPS = 60`**. This is the mechanism behind GOG's "FPS capped at 60". | [V] |
| `D3D9.dll` | `50dcc639…` | dxwrapper **stub** (strings "DxWrapper Stub", `stub.pdb`). It forwards to `dxwrapper.dll`. | [V] |
| `binkw32.dll` | `baba9992…` | **Ultimate ASI Loader** build (ThirteenAG): strings `ultimate-asi-loader\…\dinput8.pdb`, `*.asi`, `binkw32Hooked.dll`, `plugins\`, `global.ini`, the GitHub URL, plus embedded d3d8to9 and "DirectX Windower Embedded v2.3". It exports the full `_Bink*@N` API and contains RAD sections (`RADCODE`/`RADDATA`). This is what loads `DEPHook.asi`. GOG's 2021 build shipped a 356 KB `binkw32.dll` ([GOGDB](https://www.gogdb.org/manifest/15c0c58b91b0321a472005ee3c2d0177)), so GOG replaced it. | [V] (strings), [I] (that the original Bink code is embedded) |
| `DEPHook.asi` | `f5267a6a…` | 9 KB ASI built with VS2015+ (`C:\dev\Projects\DEPHook\…`). Its only functional import is **`SetProcessDEPPolicy`**. It implements GOG's "DEP disabled at runtime". | [V] (imports), [I] (it passes 0, i.e. disables DEP) |
| `fmodex.dll` | `5f9ad8f5…` | FMOD Ex **4.16.9**. `tru.exe` neither imports it nor names it, because FMOD is static. | [V] |
| `patch.000` | `c06631ad…` | A cdc bigfile with 9 records that override `bigfile.000` entries. See [CONTAINERS.md](formats/CONTAINERS.md#2-patch000). | [V] |
| `Movies/*.bik` | — | `credits`, `explode`, `intro`, `medsea_loop`, `prevontr` | [V] |

GOG install script registry values ([V], from `goggame-1900745791.script`; the game reads these keys):

- `HKLM\Software\Crystal Dynamics\Tomb Raider: Underworld`: `InstallPath`=`{app}`, `SKUType`="TRU D", `InstallType`=0, `Version`=256.
- `HKCU\…\Underworld`: `ExtendedDialog`=1.
- `HKCU\…\Underworld\Graphics`: `DontDeferShaderCreation`=0, `DisableDynamicTextures`=0.
- Save folder: `{userdocs}/Eidos/Tomb Raider - Underworld`.

---

## 2. Exe version determination

**Conclusion: our GOG `tru.exe` is the Underworld 1.1 build. TRLAU-menu-hook's `TR8` address set applies unchanged.** [V]

Evidence:

1. **Version metadata.** FileVersion is 1.1, and the link timestamp is 2008-12-16, the day patch 1.1 was released. GOGDB labels our build `58082205556264096` as **"1.1 GOG v3"**, published 2024-11-07 and live on 2025-03-20 ([GOGDB](https://www.gogdb.org/product/1900745791)). [V], [W]
2. **TRLAU-menu-hook addresses.** TRLAU has no hash or version check. It hard-codes one address per game, selected at compile time by `GET_ADDRESS(tr7, trae, tr8)` (`src/util/Hooking.h:50-56`). Its releases v1.8–v1.11 state "Version support: Underworld Steam, GOG.com". We extracted every TR8-active literal address by evaluating the `#if TR8` preprocessor state across `src/`. All of them checked out against our exe ([§5.3](#53-tr8-address-table-checked-against-our-exe)):
   - 49 code addresses are exact Ghidra function entries.
   - 4 more are valid function prologues that Ghidra's auto-analysis missed: `0x75AF90`, `0x795DB0`, `0x795E30`, and `0x79DB50` (a `jmp` thunk).
   - The NOP target `0x75AEDE` is exactly a 5-byte `E8` call inside `DeathState::Entry` (`0x75AA50`), as the hook expects.
   - All 23 data addresses fall in `.data`/BSS.
3. **Byte patterns from three independent tools match exactly once:**

| Pattern | Tool | Match in our exe | Resolves to |
|---|---|---|---|
| `8B 0D ? ? ? ? 8B 01 8B 15 ? ? ? ? 8B 00 68` | TRLAU `Hook.cpp:62` (D3D_Init, TR8 branch) | `0x478640` (fn entry) | `PCDeviceManager::s_pInstance` = `0xAD75E4` |
| `68 00 00 00 10 6A 00 C7 06` | TRLAU `Patches.cpp:240` (heap size) | `0x59A092` (in `FUN_0059a050`) | 256 MB heap: `0x10000000` at +1 and +19 |
| `A1 ? ? ? ? 83 78 10 00 75 39 80 B8` | TRLAU `render/RenderContext.cpp:25` | `0x524F20` | — |
| `80 7C 24 0C 00 8B C1 74 5A 53` | TRLAU `cdc/file/MultiFileSystem.cpp:9` | `0x472C50` (fn entry) | — |
| `8B 44 24 04 33 D2 39 51 08 89 50` | TRLAU `cdc/render/PCDeviceManager.cpp:37` | `0x521580` (fn entry) | — |
| `56 57 E8 ? ? ? ? 8B 7C 24 0C 8B` | ModLoader `src/ModLoader.cpp:124` (initPatch) | `0x478930` | Same address as TRLAU's hard-coded `InitPatchArchive` |
| `E8 ? ? ? ? 50 8D 4C 24 08 6A 00 51` | ModLoader (getFS) | `0x49A84F` | Call target `0x472B50`, same as TRLAU's `GetFS` |
| `8B 0D ? ? ? ? 89 35 ? ? ? ? 3B CE` | ModLoader (underworld diskFS) | `0x472B31` | `0x9CE27C`, same as TRLAU's diskFS |
| `83 EC 0C 8B 44 24 10 53 55 56 57 8B D9 50 89 5C 24 1C E8` | BigfileLogger `src/Main.cpp:128` | `0x4726E0` | Call target `0x472670` = `ArchiveFileSystem::CalculateHash` |

The TR7/TRAE-only patterns, as expected, match nothing.

Edition notes:

- **Retail** used SecuROM 7.38.0015; the PL/CZ/HU retail used 7.38.0014; the **GfW Marketplace** build used SecuROM PA 7.42 ([PCGW](https://www.pcgamingwiki.com/wiki/Tomb_Raider:_Underworld)). [W]
- **Steam** (AppID 8140, released 2008-11-21, no Dutch) is reported as v1.1 and "steamless"-strippable ([FearLess](https://fearlessrevolution.com/viewtopic.php?t=14860)). [W] TRLAU uses one address set for both Steam and GOG, so the Steam code layout is probably identical once unwrapped. [I]
- Whether any other edition's `tru.exe` is byte-identical to ours is **?**.

---

## 3. Patch history

| Version | Date | Notes | Tag |
|---|---|---|---|
| Demo | 2008-10-31 | PC demo; Steam demo AppID 8150 | [W] |
| 1.0 | NA 2008-11-18, EU 2008-11-21, AU 2008-12-04 | Retail with SecuROM 7.38 disc check | [W] |
| **1.1** | **2008-12-16** | Disc versions only; "NOT compatible with the purchased download versions", which were promised a separate update. Size is reported as 108–113 MB. See the fix list below. | [W] |
| GOG "1.1" | 2021-05-06 (build `54367908396077400`) | DRM-free 1.1 | [W] |
| **GOG "1.1 GOG v3"** | 2024-11-07, live 2025-03-20 (build `58082205556264096`, ours) | Adds dxwrapper, the 60 FPS cap, DEP disabled at runtime (`DEPHook.asi`), registry defaults, Win10/11 compatibility, cloud saves ([GOGDB notes](https://www.gogdb.org/product/1900745791/releasenotes)) | [W], [V] (files) |

Patch 1.1 fix list, from the official notes ([mirror](http://tombraiders.net/stella/walks/TR8walk/TR8_patch_info.txt)) [W]:

- Mouse buttons and "center camera" can now be mapped.
- Fixed occasional crashes, including crashes with Realtek audio drivers.
- Re-enabled ATI features with then-current drivers; progress on stalls on GeForce 6/7.
- Autosave per level/chapter, with more slots.
- Level fixes:
  - Xibalba ramp not raising
  - Black-screen deadlock when skipping the doppelganger cinematic
  - Kraken-room door lever
  - Headshot control glitch
- Save-name, PDA and subtitle fixes.

No later official PC patch is known. [W]

DLC "Beneath the Ashes" (2009-02-24) and "Lara's Shadow" (2009-03-10) were **Xbox 360 exclusive**. [W] The PC data still contains unused `dlc_costume_*` objects ([tombraidermodding](https://tombraidermodding.com/guide/tru-dlc-outfits)). [W]

**Our `patch.000` content** is consistent with the 1.1 level fixes. It replaces exactly `pc-w\l2_puzzleroom.drm`, `pc-w\g_pda.drm`, `pc-w\generalbank.drm` and the six `pc-w\local\locals.bin` language variants. [V] (by hash: `0x49CD2C36`, `0x87776B6B`, `0xB3AAAFD6`, `0x7CD333D3`; the names match gibbed's `patch.filelist`)

---

## 4. Sources

Licences marked "none" mean all rights reserved: use the source as reference only and never copy code from it.

| Reference | URL | Licence | Covers (versions) | Confidence | What it tells us |
|---|---|---|---|---|---|
| TRLAU-menu-hook @`3892806` (2026-10-04); latest release v2.6, 2026-10-12 | https://github.com/TheIndra55/TRLAU-menu-hook | GPL-3.0 | TR7 Legend, TRAE Anniversary, TR8 Underworld (Steam/GOG 1.1) | High; addresses verified against our exe | Runtime structures, function addresses, debug features, patches; see [§5](#5-trlau-menu-hook-findings) |
| ModLoader @`19357b6` | https://github.com/TombRaiderModding/ModLoader | GPL-3.0 | TR7, TRAE, TR8, LCGoL | High (patterns match) | File-system hook points; Underworld `InitPatchArchive`/diskFS patterns |
| BigfileLogger @`fade60a` | https://github.com/TombRaiderModding/BigfileLogger | MIT | TR7…TR2013+; Wii/PS2 TR8 codes | High (pattern match) | `ArchiveFileSystem::CalculateHash` location; Wii EU / PS2 TR8 cheat codes (`wii/tr8.*`, `ps2/tr8.s`) |
| Yura @`4ea22bb` | https://github.com/TheIndra55/Yura | GPL-3.0 | All cdc bigfile games incl. TRU | High for bigfile layout | Bigfile reader. Underworld alignment `0x7FF00000` (`Yura/OpenDialog.xaml:69-76`). Spec flags (`Yura/Formats/SpecialisationFlags.cs`). locals.bin layout (`Yura/Formats/LocaleFile.cs`), whose language enum does **not** match TR8 (see CONTAINERS) |
| Templates @`3022321` | https://github.com/TombRaiderModding/Templates | GPL-3.0 | TR7/TRAE, `tr8_cdrm.bt`, `tr8_section.bt` | High (matches our parser) | CDRM block list; TR8 section header bitfields; 5-kind relocation table; section type names. Its `magic` field is the tool export header `SECT`, not on-disk data. |
| cdcEngineTools @`6c81f40` (2019) | https://github.com/Gh0stBlade/cdcEngineTools | GPL-2.0-or-later (`License.txt`) | TR7, TRAE, TR8 (PC/Xenon), TRAS | Medium-high | TR8 DRM header = `version, nameSize, paddingSize, unk, unk, numSections` (`DRM/DRM.h:47-60`). Reloc block is a **byte** size for TR8 vs `count*8` for TR7 (`DRM/DRM.cpp:156-160`). Section-type extensions: `.tr8pcd9` texture, `.fsb` wave, `.matd`, `.obj`, `.tr8mesh` (`DRM/FileExtensions.h:20-38`). PCD9 header and 32bpp→TGA writer (`PCD2DDS/PCD.h:21-56`, `PCD2DDS/PCD.cpp:245+`) |
| cdcResearch @`3a31ef1` (2022) | https://github.com/TheIndra55/cdcResearch | none | Mostly TR7/TRAE | Medium; author warns it may be wrong | Bigfile and specMask concept (`files/bigfile.md`). objectlist.txt syntax (`files/objectlist.md`). TR7 DRM/relocs, which differ from TR8 (`files/drm.md`). Collision mesh, signals, markup, intros, PE event scripts (`level/*.md`) |
| cdcEngine @`405efd9` | https://github.com/TheIndra55/cdcEngine | GPL-3.0 | TR7 Legend | Medium (different game) | Legend decompilation: names and lineage only |
| cdcEngineDXHR @`647132c` | https://github.com/rrika/cdcEngineDXHR | **none** | Deus Ex: HR (later cdc), plus TR2013 debug-info leak names | Medium, as a later-engine reference | Later DRM header `{version, depDrmListSize, depObjListSize, unk, unk, flags, sectionCount, rootSection}` and 20-byte `DRMSectionHeader` (`cdcResource/DRM.h`). Section-type ids diverge from TR8 after 4. Engine module layout (`cdcFile`, `cdcResource`, `cdcRender`, …). Reference only. |
| TR7AE-level-viewer @`a477901` | https://github.com/TheIndra55/TR7AE-level-viewer | GPL-3.0 | TR7/TRAE | Low for TR8 | Three.js .drm level viewer; good for the architecture of a level renderer |
| Gibbed.CrystalDynamics | https://github.com/gibbed/Gibbed.CrystalDynamics | Zlib | TR7, TRAE, TR8 (PC/PS3/X360/PS2) | Medium [W, not cloned] | **TR8 file name lists** (`bigfile.filelist`: 3216/3218 names; `patch.filelist`; `pc-pch`/`pc-russ` regional lists); registry path for the install |
| cdcengine.re docs | https://cdcengine.re/docs/files/bigfile/ (+ `/drm/`, `/image/`, `/multiplexstream/`) | unstated | cdc family | Medium [W] | Bigfile alignment by game, `RAW!` image format, `.mul` overview, hash DB |
| vgmstream `mul.c` | https://github.com/vgmstream/vgmstream/blob/master/src/meta/mul.c | ISC-style | TR7/TRAE/TR8 .mul | Medium [W] | `.mul` header `{sample_rate, loop_start, num_samples, channels}`, which matches our observation in CONTAINERS |
| PCGamingWiki (local `research/pcgw_tru.wiki`) | https://www.pcgamingwiki.com/wiki/Tomb_Raider:_Underworld | CC BY-NC-SA | All PC editions | Medium [W] | DRM per store, fixes, registry keys, middleware (FMOD Ex, Bink 1.5.21.0, D3D9c), issues |
| GOGDB | https://www.gogdb.org/product/1900745791 | n/a | GOG builds | High for GOG metadata | Build ids/dates, v3 changelog, file manifests |
| WSGF | https://www.wsgf.org/dr/tomb-raider-underworld | n/a | PC | Medium [W] | Hor+; WS/UW Silver, MM Bronze, 4K Silver; width must be < 4096 |
| ModDB | https://www.moddb.com/games/tomb-raider-underworld | n/a | PC | Low-medium [W] | Patch 1.1 mirror, demo, ReShade preset; RTX Remix "partially playable" (water renders as sky) |
| tombraidermodding.com guides | https://tombraidermodding.com/guide/game-limits (+ `/codenames`, `/tru-dlc-outfits`, `/tool/trulau-mod-tools`) | unstated | TR8 | Medium [W] | TR8 limits: 256 MB heap, 204 loaded objects, 1024 instances, 42-entry bone map. Codenames. DLC object ids. Raq's Noesis `.tr8mesh`/`.pcd` plugin |
| Wikipedia | https://en.wikipedia.org/wiki/Tomb_Raider:_Underworld | CC BY-SA | — | Medium [W] | Credits, release dates, porters, middleware (NavPower, Illuminate Labs Turtle) |
| GOG forum, many-core crash | https://www.gog.com/forum/tomb_raider_series/tomb_raider_underworld_fix_for_crash_for_people_with_cpus_that_have_many_cores | n/a | GOG | Medium [W] | 0xC0000005 at launch on ≥32 threads; still reported on GOG v3 |

---

## 5. TRLAU-menu-hook findings

**Licence:** GPL-3.0 (compatible with this project). We read it for structure and address knowledge only and have copied no code.

**How it injects:**

- It ships as `TR8-Menu-Hook.asi`, loaded by Ultimate ASI Loader as `binkw32.dll`; the release notes say "binkw32.dll is provided by Ultimate ASI Loader". GOG v3 already ships a UAL `binkw32.dll`.
- It hooks `GetStartupInfoA` so it can wait for packed executables to unpack (`src/Main.cpp:29-34`), then hooks D3D init via a byte pattern (`src/Hook.cpp:53-68`).
- Build platforms are `TR7`, `TRAE` and `TR8`. TR8 additionally defines `BATCH_DRAW_CALLS` (`premake5.lua:50-52`).

### 5.1 Version support and detection

- There is **no runtime version or hash detection**. Addresses come from compile-time `GET_ADDRESS` (`src/util/Hooking.h:50-56`) plus literal TR8 addresses inside `#ifdef TR8`.
- A few lookups use byte patterns: `hook::pattern(...).count(1)`, which aborts on a mismatch.
- Supported Underworld exes: "Steam, GOG.com" (release notes v1.8–v1.11, 2021-07 to 2022-04).
- Our GOG 1.1 v3 exe matches; see [§2](#2-exe-version-determination).

### 5.2 Underworld structures

Offsets assume `cdc::Vector` is 16 bytes and 16-byte aligned (an `__m128` union, `src/cdc/math/Vector.h:5-21`). All of these are TRLAU's partial reconstructions.

| Structure | Layout (TR8 branch) | Source |
|---|---|---|
| `Instance` (size 0x2E8) | `NodeType node`; `next` +0x08, `prev` +0x0C; `Object* object` +0x10; `iflags` +0x14; `Vector3 position` +0x20; `Euler rotation` +0x30; `data` +0x50; `introUniqueID` +0x58; `Intro* intro` +0x100; `noDrawGroups` +0x23C; `AnimComponent*` +0x280; `extraData` +0x2D0 | `src/instance/Instance.h:99-295` |
| Instance list | Singly walked via `next`, starting at `*(Instance**)0xD98D54` | `src/instance/Instances.cpp:4-12` |
| `Intro` | `IntroData* data` at +0x78; `IntroData {int type}` | `src/instance/Instance.h:92-97`, `:15-18` |
| `ObjectData` (instance->data) | `{u16 version, family, id, type}` | `src/instance/Instance.h:20-26` |
| `Object` | `uniqueID` +0x14; `numModels` +0x3A; `numAnims` +0x3C; `Model** modelList` +0x40; `AnimListEntry* animList` +0x44; `char* name` +0x60 | `src/instance/Object.h:44-64` |
| `AnimListEntry` | `{i16 animationID, i16 animationNumber, void*, char* debugName}`. **TR8 objects embed animation debug names**, so no `animlist.ids` is needed. | `src/instance/Animation.h:198-207`, `src/modules/Instance.cpp:30-32,270-274` |
| `AnimComponent` / `AnimProcessor` | `AnimComponent{pad[20], AnimProcessor*}`; `AnimProcessor{pad[246], u8 mSectionCount, u8 mSectionsAllocated}` | `src/instance/Animation.h:157-190` |
| `ObjectList` (from `objlist`) | `{int numObjects, int size, {char* name, int trackerIndex}[]}`; `OBTABLE_GetObjectName(id)` = `object[id-1].name` | `src/level/Stream.h:46-59`, `src/game/Game.cpp:78-83` |
| `ObjectTracker` | `{ResolveObject*, char* objectName, Object*, i16 objectID, i16 objectStatus}`; status 2 = loaded | `src/level/Stream.h:37-44`, `src/modules/MainMenu.cpp:158-159` |
| `StreamUnit`, `STracker` | `STracker{StreamUnit StreamList[8]}` at `0xDBAB40`. Unit name max is 128 in TR8 vs 20 in TR7/TRAE, but the struct still declares `baseAreaName[20]`, so this is **unverified for TR8**. | `src/level/Stream.h:6-35` |
| `GameTracker` at `0xE7F088` | `Level* level` +0x08; `Instance* playerInstance` +0x0C; `debugFlags1..4` +0x10..; `displayFrameCount` +0x20; `streamFlags` +0x30; `char baseAreaName[128]` +0x38; `gameMode` +0xBB; `cheatMode` +0xBC; `StreamUnitID` +0xC0; `float timeMult` +0x100; `currentPlayerObjectID` +0x140; `altPlayerObjectID` +0x144; `float timeDilation` +0x148 | `src/game/Game.h:109-177` |
| `MainState` (int at `0xE7ED60`) | `MS_NONE 0`, `PLAYGAME 1`, `LOADLEVEL 2`, `PLAY_CINEMATIC 5`, `SHOW_STATIC_SCREEN 6`, `DISPLAY_MAIN_MENU 10`. Menu sub-state (char) at `0xFAE8DA`: `MainMenuScreen` = 3. | `src/game/Game.h:11-46`, `src/modules/MainMenu.cpp:195-202` |
| `GlobalInfo` (`*0xE7EE50`) | `{int, PlayerObjects* {i16 num, i16* objectIds}}`: the list of selectable player outfits | `src/game/Game.h:179-189`, `src/modules/Patches.cpp:295-322` |
| `Camera` (`*0xE80534`) | `Matrix transform` +0x10; `Matrix invTransform` +0x50; `Vector3 position` +0x90; `Quat orientation` +0xA0; `float fov` +0xB0 (radians; free-cam default π/4) | `src/game/Camera.h:40-51`, `src/modules/camera/Underworld.cpp:27-48` |
| `Level` | `Terrain* terrain` +0; `SignalListStart`, `SignalIDList`, `splineCameraData`, `relocModule` after `pad[140]`. Shared TR7/TRAE layout; TR8 accuracy beyond `terrain` is ?. | `src/level/Level.h:229-245` |
| `Terrain` | `{i16, i16 numStreamUnitPortals, StreamUnitPortal*, int numTerrainGroups, TerrainGroup*, TerrainGroup* signalTerrainGroup}` | `src/level/Level.h:216-226` |
| `TerrainGroup` (0x50) | `globalOffset` +0; `localOffset` +0x10; `ID` +0x24; `uniqueID` +0x28; `TerrainGroupExtra* extra {int flags}` +0x30; `Level*` +0x34; `Mesh* mesh` +0x38 | `src/level/Level.h:169-193` |
| `StreamUnitPortal` (0x100) | `char tolevelname[128]`; `i16 streamID` +0x84; `Vector3 min` +0x90, `max` +0xA0, `quad[4]` +0xB0, `normal` +0xF0 | `src/level/Level.h:195-214` |
| Collision `Mesh` | `BBox m_box` +0; `Vector3 m_position` +0x20; `m_vertices` +0x30; `IndexedFace* m_faces` +0x34; `KDNode* m_root` +0x38; `m_clientData` +0x3C; `u32 numNodes, numFaces, numVertices, numDegenerateFaces, numNonManifoldEdges` +0x40..+0x50; `u16 m_vertexType` +0x54 (0 = int16 xyz, 1 = float xyzw); `u16 m_height` +0x56. TR7/TRAE use 16-bit counts. | `src/level/Level.h:84-112`, `src/modules/Draw.cpp:17-31` |
| `IndexedFace` (20 B) / `SignalFace` | `{u32 i0,i1,i2; u8 adjacencyFlags; u8 collisionFlags; u16 clientFlags; u32 materialType}`. In the signal mesh the last part is `u16 id` (signal index). Indices are **32-bit in TR8** (16-bit in LAU). | `src/level/Level.h:11-55` |
| MarkUp | `MarkupManager{NodeType free, dynamic, static box lists}` at `*0xDB94CC`. `MarkUpBox`: `instance` +0x0C, `velocity` +0x10, `MarkUp*` +0x20, `flags` +0x28. `MarkUp`: `float pos[3]` +0x34, `float bbox[6]` +0x40, `PolyLine*` +0x58. TR8 `MUD_FLAG_*` bits: LEDGEGRAB 30, JUMPLANDING 29, ENEMYDROPDOWN 28, POLE 27, VERTPOLE 26, HORIZPOLE 25, ZIPLINE 24, RAILSLIDE 23, LADDER 22, CLOSEJUMPTARGET 21, WATER 20, ROPE 19, COVER 18, PERCH 17, BALANCEBEAM 16, CONSTRAINEDGROUND 15, PILLAR 4, WRAPS 2, INSTANCE 1, POLYLINE 0, DISABLED 31 | `src/level/Markup.h:8-166`, `src/modules/Draw.cpp:295-355` |
| Triggers | Count/array globals: planes `*(int*)0xDBA21C` / `NsTriggerPlaneBase*[]` at `0xDA61C8`; volumes `0xDBA220` / `0xDB6BE0`. `NsInstance::m_instance` +0x20. `IntroDataTrigger`: `shape` +0x30 (1 box, 2 sphere), `LocalX/Y/ZExtent` +0x34.., `Radius` +0x40. Boxes are built with `Matrix::Build_XYZOrder(rotation)` plus position. | `src/level/Trigger.h:5-36`, `src/modules/Draw.cpp:478-575` |
| `cdc::FileSystem` vtable | `RequestRead`, `OpenFile`, `FileExists`, `GetFileSize`, `Set/GetSpecialisationMask`, `GetStatus`, `Update`, `Synchronize`; **TR8 adds** `Suspend`, `Resume`, `IsSuspended`, `GetBufferPointer`, `ResetBufferPointer`; then a virtual destructor. `FileRequest`/`FileReceiver`/`File` interfaces likewise. | `src/cdc/file/FileSystem.h:5-79` |
| Script system | `cdc::ScriptManager` at `*0xCC4F5C` (thread and object HashSets). `ScriptType` → `ScriptTypeStreamData {version, name, nativeScriptPackageName/Name, super, size/align, states, enums, structs, members, startFunctions, prototypes, functions, dependencies}`. `DataType` base types `DTVOID…NTENUM`. `ScriptObject::CallFunction` `0x57B380`. | `src/cdc/script/*.h`, `src/modules/ScriptDebug.cpp:14-31` |
| Materials | `cdc::HashTable<u32, MaterialEntry>` at `0x8A6EBC`. `MaterialEntry{CommonMaterial*, int, refCount, size, versionID}`. `MaterialData`: `i16 numParameters` +0x58, `float* parameters` +0x5C (float4 each). | `src/modules/Materials.cpp:27-93`, `src/render/Material.h`, `src/cdc/render/MaterialData.h` |
| Render | `PCDeviceManager::s_pInstance` = `*0xAD75E4` (resolved from the pattern on our exe). `PCStateManager*` at device+644 (marked TODO). Wireframe = `SetRenderState(D3DRS_FILLMODE)` around `PCScene::Draw` (`0x5150E0`). | `src/Hook.cpp:61-63`, `src/modules/Render.cpp:34-66` |
| Instance messages (`INSTANCE_Post`) | 4 with data 3 = instant ragdoll death in TR8 (LAU uses 262167). 7 = unhide. 35 = birth player weapons. Costume index 12 has an empty object id, which restores default player switching. | `src/modules/MainMenu.cpp:243-249`, `src/modules/Instance.cpp:150-156`, `src/game/Game.cpp:21-36` |

### 5.3 TR8 address table checked against our exe

The **TRLAU name** column uses TRLAU's names. The **Our exe** column records the static check: Ghidra function entry, valid prologue, or section.

| Address | TRLAU name / meaning | Source (`src/…`) | Our exe |
|---|---|---|---|
| `0x472B50` | `GetFS()` → `cdc::FileSystem*` | file/FileSystem.cpp:7 | Ghidra fn entry |
| `0x472670` | `ArchiveFileSystem::CalculateHash` (via the BigfileLogger pattern) | BigfileLogger src/Main.cpp:128 | Ghidra fn entry |
| `0x474C90` | `Font::SetCursor` | render/Font.cpp:29 | Ghidra fn entry |
| `0x474CB0` | `Font::GetCursor` | render/Font.cpp:36 | Ghidra fn entry |
| `0x474D50` | `Font::SetScale` | render/Font.cpp:43 | Ghidra fn entry |
| `0x475870` | `Font::GetHeight` | render/Font.cpp:97 | Ghidra fn entry |
| `0x4758C0` | `Font::GetTextWidth` | render/Font.cpp:90 | Ghidra fn entry |
| `0x476BC0` | `Font::PrintFormatted` | render/Font.cpp:83 | Ghidra fn entry |
| `0x477970` | `LOAD_UnitFileName(name, unit, ext)` | game/Game.cpp:92 | Ghidra fn entry |
| `0x478930` | `InitPatchArchive(const char*)` | modules/ModLoader.cpp:62 | Ghidra fn entry |
| `0x478B00` | `MainG2_ErrorHandler(const char*)` | modules/Patches.cpp:168 | Ghidra fn entry |
| `0x480E50` | `InputSystem::GetAxisValue` | input/Input.cpp:34 | Ghidra fn entry |
| `0x48A450` | `TRANS_TransToDrawVertex` / `TRANS_TransToLineVertex` | render/Draw.cpp:26,33 | Ghidra fn entry |
| `0x49BE10` | `cdc::OrthonormalInverse3x4` | cdc/math/Math.cpp:35 | Ghidra fn entry |
| `0x49D500` | `cdc::Matrix::Build_XYZOrder` | cdc/math/Matrix.cpp:15 | Ghidra fn entry |
| `0x49DD70` | `cdc::Matrix::Build(Euler*)` | cdc/math/Matrix.cpp:7 | Ghidra fn entry |
| `0x49F630` | `TRANS_RotTransPersVectorf` (world → screen; z > 16 = on-screen) | render/Draw.cpp:18 | Ghidra fn entry |
| `0x49F990` | `TRANS_TransToDrawVertexV4f` | render/Draw.cpp:11 | Ghidra fn entry |
| `0x4A21D0` | `CPUCount` | modules/Patches.cpp:201 | Ghidra fn entry |
| `0x4A2680` | `cdc::JobChainImplWithThreads::StartSystem(int)` | modules/Patches.cpp:202 | Ghidra fn entry |
| `0x4A3200` | `ScriptLogInt` | modules/ScriptLog.cpp:66 | Ghidra fn entry |
| `0x4A3240` | `ScriptLogFloat` | modules/ScriptLog.cpp:67 | Ghidra fn entry |
| `0x4A3280` | `ScriptPrintInt` | modules/ScriptLog.cpp:64 | Ghidra fn entry |
| `0x4A32C0` | `ScriptPrintFloat` | modules/ScriptLog.cpp:65 | Ghidra fn entry |
| `0x502B70` | `cdc::PCStateManager::SetRenderState` | cdc/render/PCStateManager.cpp:7 | Ghidra fn entry |
| `0x5150E0` | `PCScene::Draw` (`__thiscall`) | modules/Render.cpp:65 | Ghidra fn entry |
| `0x52E880` | `GAMELOOP_IsWipeDone(type)` | game/Game.cpp:52 | Ghidra fn entry |
| `0x52E8B0` | `GAMELOOP_SetScreenWipe(type, target, time)` | game/Game.cpp:59 | Ghidra fn entry |
| `0x57B380` | `cdc::ScriptObject::CallFunction` | cdc/script/ScriptObject.cpp:7 | Ghidra fn entry |
| `0x5B1EA0` | `G2EmulationInstanceSetAnimation` | instance/Animation.cpp:6 | Ghidra fn entry |
| `0x5B1F50` | `G2EmulationInstanceSetMode` | instance/Animation.cpp:13 | Ghidra fn entry |
| `0x5B1FC0` | `G2EmulationInstanceQueryAnimation` | instance/Animation.cpp:20 | Ghidra fn entry |
| `0x5B3750` | `INSTANCE_Post(instance, message, data)` | instance/Instance.cpp:7 | Ghidra fn entry |
| `0x5B3DD0` | `INSTANCE_HideUnhideDrawGroup` | instance/Instance.cpp:14 | Ghidra fn entry |
| `0x5BC4E0` | `INSTANCE_ReallyRemoveInstance` | instance/Instance.cpp:21 | Ghidra fn entry |
| `0x5BD0F0` | `INSTANCE_BirthObjectNoParent` | instance/Instance.cpp:28 | Ghidra fn entry |
| `0x5BF770` | `OBTABLE_GetObjectID(name)` | game/Game.cpp:73 | Ghidra fn entry |
| `0x5BFA40` | `DRAW_DrawTriangles` | render/Draw.cpp:87 | Ghidra fn entry |
| `0x5BFB20` | `DRAW_DrawQuads` | render/Draw.cpp:55 | Ghidra fn entry |
| `0x5BFCD0` | `DRAW_DrawLines` | render/Draw.cpp:63 | Ghidra fn entry |
| `0x5C17D0` | `STREAM_GetObjectTrackerByName` | level/Stream.cpp:6 | Ghidra fn entry |
| `0x5C1DA0` | `STREAM_PollLoadQueue` | level/Stream.cpp:13 | Ghidra fn entry |
| `0x5C39D0` | `MarkUpBox::GetSegmentPos` | level/Markup.cpp:11 | Ghidra fn entry |
| `0x5C5A30` | Font flush (hooked for end-of-frame drawing) | render/Font.cpp:107 | Ghidra fn entry |
| `0x5DEF70` | `MAIN_DoMainInit` | modules/Patches.cpp:204 | Ghidra fn entry |
| `0x5DF8C0` | `GAMELOOP_RequestLevelChangeByName(name, gt, doneType)` | game/Game.cpp:45 | Ghidra fn entry |
| `0x5DFBE0` | `GAMELOOP_Process(GameTracker*)` (per-frame) | game/GameLoop.cpp:20 | Ghidra fn entry |
| `0x5EDF50` | CameraManager method that restores the gameplay camera (`this` = `0xE804F0`) | modules/camera/Underworld.cpp:46 | Ghidra fn entry |
| `0x5F39F0` | `CAMERA_SetMode(int)` (11 = free camera) | game/Camera.cpp:30 | Ghidra fn entry |
| `0x75AA50` | `DeathState::Entry` (`__stdcall`) | modules/Patches.cpp:171 | Ghidra fn entry |
| `0x75AEDE` | Death-wipe call inside `DeathState::Entry` | modules/Patches.cpp:175 | `E8` call in `FUN_0075aa50` |
| `0x75AF90` | `DeathState::Process` | modules/Patches.cpp:172 | prologue, no Ghidra fn |
| `0x795DB0` | `NsCoreBase::PrintString` (variant 2) | modules/ScriptLog.cpp:69 | prologue, no Ghidra fn |
| `0x795E30` | `NsCoreBase::PrintString` (variant 1) | modules/ScriptLog.cpp:68 | prologue, no Ghidra fn |
| `0x79DB50` | `PLAYER_DebugSwitchPlayerCharacter` = `NtUnlockableCostume::SwitchPlayerCostume` | game/Game.cpp:66, game/NtUnlockableCostume.cpp:6 | `jmp` thunk, no Ghidra fn |
| `0x8A6EBC` | Materials hash table | modules/Materials.cpp:38 | `.data` (initialised) |
| `0x8AB4E6` | `bool playerControlEnabled` | input/Input.cpp:12,17 | `.data` (initialised) |
| `0x9CE278` | Active `cdc::FileSystem*` (replaced by mod loaders) | modules/ModLoader.cpp:53 | BSS |
| `0x9CE27C` | Disk `cdc::FileSystem*` | modules/ModLoader.cpp:15 | BSS |
| `0x9DE6D4` | `Font* mainFont` | render/Font.cpp:24 | BSS |
| `0xA02B68` | `InputSystem*` | input/Input.cpp:29 | BSS |
| `0xA02B79` | `bool inputDisabled` | input/Input.cpp:7 | BSS |
| `0xAD75E4` | `PCDeviceManager::s_pInstance` (pattern-resolved) | Hook.cpp:62 | BSS |
| `0xCC4F5C` | `cdc::ScriptManager*` | cdc/script/ScriptManager.cpp:5 | BSS |
| `0xCDC9A0` | `bool hideUI` | modules/Frontend.cpp:25 | BSS |
| `0xD98D54` | First `Instance*` | instance/Instances.cpp:6 | BSS |
| `0xDA61C8` | `NsTriggerPlaneBase*[]` | modules/Draw.cpp:483 | BSS |
| `0xDB6BE0` | `NsTriggerVolumeBase*[]` | modules/Draw.cpp:514 | BSS |
| `0xDB94CC` | `MarkupManager*` | level/Markup.cpp:6 | BSS |
| `0xDB94D0` | `ObjectList*` | game/Game.cpp:80 | BSS |
| `0xDBA21C` | Number of trigger planes | modules/Draw.cpp:482 | BSS |
| `0xDBA220` | Number of trigger volumes | modules/Draw.cpp:513 | BSS |
| `0xDBAB40` | `STracker` (stream units) | game/Game.cpp:18 | BSS |
| `0xE7ED60` | `int mainState` | modules/MainMenu.cpp:195 | BSS |
| `0xE7EE50` | `GlobalInfo*` | modules/Patches.cpp:297 | BSS |
| `0xE7F088` | `GameTracker` | game/Game.cpp:13 | BSS |
| `0xE804F0` | CameraManager instance | modules/camera/Underworld.cpp:46 | BSS |
| `0xE80534` | `Camera*` current camera | game/Camera.cpp:9 | BSS |
| `0xFAE8DA` | `char` main-menu state | modules/MainMenu.cpp:200 | BSS |

TRLAU has no TR8 address (all `0x000000`) for:

- `IsInNextGenMode` (TR7-only feature)
- `CAMERA_CalcPosition`/`SetProjDistance`
- `IncrHealth`
- `RELOC_GetProcAddress` and event debug
- `Alloc::Get`/`GetHeap`
- `PCBasicVertexPool::Create`
- the TR7/TRAE `TerrainDrawable` constructor

### 5.4 Debug facilities restored or added (TR8)

Source: `docs/features.md` plus the module registration in `src/Hook.cpp:128-152`.

- **Menu (F8):**
  - Load unit (`GAMELOOP_RequestLevelChangeByName`, only if `pc-w\<unit>.drm` exists)
  - Birth object (load via `STREAM_GetObjectTrackerByName`, poll the queue, then `INSTANCE_BirthObjectNoParent` at the player)
  - Switch outfit (`altPlayerObjectID`)
  - Time multiplier
  - FOV slider
- **Keys:**
  - F2: skew/fly
  - F3: freeze
  - F4: free camera (`CAMERA_SetMode(11)`, then writes `Camera.transform`/`invTransform`)
  - F5: player control
  - F9: outfit
  - F11: ragdoll death
- **Instance viewer:** transforms, object info, draw groups (`INSTANCE_HideUnhideDrawGroup`), current animations with embedded debug names, play animation (`G2EmulationInstanceSetAnimation`), post messages, unhide.
- **Debug drawing:** instance names/intro ids/family, markup polylines and boxes, collision mesh (per terrain group, culled by distance in TR8), portals, signals, and trigger volumes/planes (TR8-only).
- **Renderer:** wireframe, hide UI. **Material editor** (TR8-only): live float4 parameter edit.
- **Script log and debugger** (TR8-only):
  - `ScriptLog` hooks the script print/log natives and `NsCoreBase::PrintString`.
  - `ScriptDebug` walks `ScriptManager` objects and types with RTTI names.
- **Mod loader:**
  - Hooks `InitPatchArchive` and inserts a `HookFileSystem` in front of the archive `MultiFileSystem`.
  - Paths are rewritten `pc-w\…` → `mods\…` and `\…` → `mods\…`.
  - A specialised file is tried first as `<path>_%03d` with `specMask & ~0x80000000` (`src/file/HookFileSystem.cpp:15-55,88-94`, `docs/mods.md`).
- **Patches applied to TR8:**
  - A `TaskDialog` replaces the fatal `MainG2_ErrorHandler`.
  - Optional no-death-fade.
  - `HeapSize` via `patches.ini` (default 256 MB).
  - **Multicore fix:** `CPUCount` returns `hardware_concurrency`, and `StartSystem` is clamped to 31 threads. The original crashes with ≥32 threads.
  - Player-outfit list pruning: entries whose `.drm` is missing are removed.
  - Source: `src/modules/Patches.cpp:147-219,292-322`, `src/modules/patches/Multicore.cpp`.
- Not used for TR8 (`#ifndef TR8`):
  - The DEP/reloc fix (`MakePeHandle`). LAU loads level event scripts as PE DLLs into data memory, and TR8 does not use that path.
  - Intro skip.
  - Motion-blur and cinematic-bar toggles.
  - The `LevelModule`/`Debug` modules (event-var debug).

### 5.5 How the LAU engines differ (as visible through TRLAU)

| Area | Legend (TR7) / Anniversary (TRAE) | Underworld (TR8) | Source |
|---|---|---|---|
| Level logic | Event scripts as **PE DLLs** inside level DRMs (`EventMain`/`EventDebug`; `Level.relocModule`); DEP crash when loaded into NX memory | **cdc script VM** (`cdc::ScriptManager`, `ScriptType`, native `Ns*` script classes such as `NsTriggerPlaneBase`, `NsCoreBase`); TRLAU uses ScriptLog/ScriptDebug instead of event debug | cdcResearch `level/event.md`; TRLAU `Hook.cpp:144-151`, `modules/patches/Reloc.cpp` |
| Bigfile split alignment | `0x9600000` | `0x7FF00000` | Yura `OpenDialog.xaml:69-76` |
| DRM | TR7: relocs `count×8` bytes | v19 with dependency name lists and a 5-kind reloc table sized in **bytes** | cdcEngineTools `DRM/DRM.cpp:156-160`; Templates `tr8_section.bt` |
| Section types | 0–9 | 0–13 (+MATERIAL, OBJECT, RENDERRESOURCE/mesh, PDA_MAP) | `tr8_section.bt`, cdcEngineTools `DRM/DRM.h:66-98` |
| Collision mesh | 16-bit indices and counts | 32-bit indices and counts | TRLAU `level/Level.h:11-112` |
| Unit names | 20 chars | 128 chars | TRLAU `level/Stream.h:6-10`, `level/Level.h:195-214` |
| Objects / anims | `Object` has full G2 animation lists; names via `animlist.ids` | Different `Object` layout; anim entries carry `debugName` | TRLAU `instance/Object.h`, `instance/Animation.h` |
| Camera | `CameraCore` with `projDistance`, Euler rotation | Matrix + inverse + quaternion + `fov` (radians); camera modes via `CAMERA_SetMode` and a CameraManager | TRLAU `game/Camera.h` |
| Graphics modes | TR7 "next-gen" toggle (`IsInNextGenMode`); specMask bit 31 = next-gen content | No toggle found by TRLAU; the GOG data still carries masks with bit 31 set (0xFFFFFFxx) | TRLAU `game/Game.cpp:38-41`; our CONTAINERS.md |
| Threads | — | `cdc::JobChainImplWithThreads`; crashes with ≥32 hardware threads | TRLAU `modules/patches/Multicore.cpp` |
| File system | Base vtable | Adds Suspend/Resume/IsSuspended/GetBufferPointer/ResetBufferPointer; separate patch archive (`patch.000`) | TRLAU `cdc/file/FileSystem.h` |
| Markup flags | Pattern-interact, enemy jump, attach points, mantle-down, wall pole | Pole, rail-slide, cover, balance beam, constrained ground, pillar | TRLAU `level/Markup.h:35-84` |

---

## 6. Known issues and community fixes (PC)

| Issue | Cause / fix | Tag |
|---|---|---|
| Crash at launch on ≥32 hardware threads (0xC0000005) | `cdc::JobChainImplWithThreads::StartSystem`. Fixes: TRLAU v2.3+ clamp to 31; affinity `7FFFFFFF`; `WINE_CPU_TOPOLOGY`. Still reported on GOG v3. Our host has 32 threads, so this matters for any original-game comparison runs. | [S], [W] |
| DEP crash | LAU PE-DLL event scripts (TRLAU v1.11 fix, non-TR8). GOG v3 calls `SetProcessDEPPolicy` via `DEPHook.asi`. The exact TR8 DEP failure site is **?**. | [S], [V], [W] |
| High-FPS physics/puzzle breakage; controls bug above ~120 fps | PCGW: "broken physics at high framerates", use VSync / 60 Hz. GOG caps at 60 via `dxwrapper.ini LimitPerFrameFPS=60`. The original simulation is frame-time-dependent: `GameTracker.timeMult` at +0x100 [S]. | [W], [V] |
| Aspect ratio / FOV | 4:3/16:9/16:10 native. Others via `HKCU\…\Graphics\AspectRatio` = w/h×10000 (the in-game menu overwrites it). Hor+ (WSGF Silver). Width must be < 4096. No FOV option; external tools: Flawless Widescreen, dlrudie Widescreen Fixer. | [W] |
| Hidden options | `Graphics\EnableDepthOfField`=1 (DoF ships disabled); `DisableDynamicTextures`, `DontDeferShaderCreation`=0 reduce stutter (GOG sets the latter two); `EAXSupport`. | [W], [V] (GOG values) |
| Missing vs X360 | Dynamic flashlight shadows, per-object motion blur | [W] |
| Misc | No Steam overlay; audio crackle above 96 kHz output; Bhogavati pole softlock; greyed-out Treasure Hunt | [W] |
| Controller | XInput (hot-plug, Xbox prompts) | [W], [V] (`XInputGetState`, `PCXInputDevice`) |
| Modern visuals | ReShade preset (ModDB); RTX Remix "partially playable" (2026, water = sky) | [W] |

---

## 7. Middleware and production facts

- **Audio:** FMOD Ex is statically linked [V]. The shipped `fmodex.dll` is 4.16.9 and appears unused [V]; the static FMOD version is ?. FMOD paths compiled into the exe: DirectSound, EAX2–5, OpenAL. Bigfile audio payloads [V] ([CONTAINERS §1.5](formats/CONTAINERS.md#15-payload-inventory-first-bytes-of-all-7339-records)):
  - 5835 `.mul` MultiplexStream records (1029 voice lines × 5 dubs: EN/FR/DE/IT/ES)
  - 75 `FSB4` banks (header version `0x40000`), each stored next to a `Mus!` music script
  - 40783 WAVE sections inside DRMs (FSB per cdcEngineTools)
- **Video:** Bink, through the 12 imported `_Bink*` calls listed in [§1.1](#11-truexe). PCGW says Bink **1.5.21.0** [W]. Movies are in `Movies/*.bik` [V]. Subtitles for movies use cdc "SchemaFile" `.sch` text (cdcResearch `files/schemafile.md`) [S]; whether this applies to TR8 is ?.
- **Rendering:** Direct3D 9.0c (`d3d9.dll` loaded dynamically) [V]. Shaders are precompiled in `.shad` and in `SHADERLIB` sections [V], [S]. ATI Fetch4 shadow path [V].
- **Other middleware:**
  - NavPower AI navigation [V]
  - libjpeg [V]
  - Illuminate Labs Turtle lighting (offline) [W]
  - The `ws2_32` import is probably debug/network tooling [I]
- **Development:**
  - Credits [W]:
    - Developer: Crystal Dynamics
    - Publisher: Eidos Interactive
    - PC and PS3 port: Nixxes
    - PS2/Wii ports and X360/PS3 support: Buzz Monkey
    - DS: Santa Cruz Games
    - OS X (2012): Feral
  - Wikipedia says a new engine was built from scratch and later dubbed the "CDC engine", and that Lara's animation used motion capture for the first time [W].
  - Codename TR8 [W], [S].

---

## 8. Other TR8-specific code

All repositories were read, never built or run. Licences as in [§4](#4-sources).

### 8.1 Yura (GPL-3.0) @`4ea22bb`

- Bigfile browser (WPF). Its `LegendArchive` reader covers Legend, Anniversary and Underworld: `u32 count`, `hash[count]`, then `{size, offset, specialisation, compressedSize}` records, `offset << 11`, zlib when `compressedSize != 0` (`Yura.Shared/Archive/LegendArchive.cs:16-93`). This matches our [V] layout. TRU never uses `compressedSize`.
- Split alignment `0x7FF00000` for Underworld and later (`Yura/OpenDialog.xaml:69-76`).
- `SpecialisationFlags`: bits 0–4 EN/FR/DE/IT/ES, `0x80000000` GfxNextGen (`Yura/Formats/SpecialisationFlags.cs`). Display masks off `0x7FFFFFE0` (`Yura/MainWindow.xaml.cs:318-323`).
- `LocaleFile`: `{language, numStrings, offsets[]}` with UTF-8 strings (`Yura/Formats/LocaleFile.cs:12-36`). Its `Language` enum does not match TR8's header ids ([CONTAINERS §5](formats/CONTAINERS.md#5-pc-wlocallocalsbin-ui-text)).
- `DrmFile` accepts only DRM version **14** (TR7/TRAE; `Yura/Formats/DrmFile.cs:25`), so Yura cannot parse TR8 DRMs.
- `HashDumper/` builds file lists from hash logs.

### 8.2 ModLoader (GPL-3.0) @`19357b6`

- An ASI that inserts a `HookFileSystem` (`src/file/HookFileSystem.cpp`) into the game's `cdc::MultiFileSystem`.
- Version detection is **pattern-based**: `getFS` plus one of `initArchive` (LAU), `initPatch` (TR8) or `initDlc` (LCGoL). Otherwise it shows "This version is not compatible" (`src/ModLoader.cpp:120-133,184-191`).
- Underworld path:
  - It hooks `InitPatchArchive`; pattern `56 57 E8 ? ? ? ? 8B 7C 24 0C 8B` hits `0x478930` in our exe (`src/ModLoader.cpp:87-94,124,145-150`).
  - It takes the disk FS from pattern `8B 0D ? ? ? ? 89 35 ? ? ? ? 3B CE` at +8, which gives `0x9CE27C` (`src/ModLoader.cpp:160,176-179`).
  - All patterns were checked against our exe in [§2](#2-exe-version-determination).
- Our exe decompilation confirms the mechanism: `InitPatchArchive` calls `MultiFileSystem::Add(…, bAddToFront=1)`, and lookups walk from the front ([CONTAINERS §2](formats/CONTAINERS.md#2-patch000)).

### 8.3 BigfileLogger (MIT) @`fade60a`

- An ASI that logs `hash → path` on every archive lookup, to build file lists. The README lists Underworld as tested.
- For 32-bit cdc it finds `ArchiveFileSystem::CalculateHash` through the call at +18 of pattern `83 EC 0C 8B 44 24 10 53 55 56 57 8B D9 50 89 5C 24 1C E8` (`src/Main.cpp:128-136`). In our exe that resolves to `0x472670`, which lowercases and CRCs the path [V].
- `wii/` and `ps2/` hold console TR8 cheat/patch codes (not relevant to PC).

### 8.4 cdcEngineTools (GPL-2.0-or-later) @`6c81f40`

- Four small tools, selected per game and platform at compile time (`TR7`, `TR8`, `TRAS` = the cancelled "Tomb Raider: Ascension"; PC-W, PS2-W, WII-W, XENON-W, PS3-W in `CDRM/PlatformSetup.h`, `CDRM/Game.h`).
- **CDRM**: decompresses CDRM blocks.
- **DRM**: splits sections.
  - TR8 header: `nameSize, paddingSize, unk00, unk01` after the version (`DRM/DRM.cpp:55-59`). Both blocks are skipped after the section table (`:101-104`).
  - The section blob is `size + (packed >> 8)` bytes for TR8 vs `(packed >> 8) × 8` for TR7/TRAE (`:194-198,226-235`).
  - In repack mode it **writes** a `SECT` (`0x54434553`) tool header (`:167,218`). This explains the `magic` field in Templates' `tr8_section.bt`.
  - Per-type output extensions (`DRM/FileExtensions.h:20-38`): `.tr8pcd9`, `.fsb`, `.tr8mesh`, and others.
- **PCD2DDS**: PCD9 header `{magic, format, textureDataSize, paletteDataSize, u16 width, u16 height, u8 depth, u8 numMipMaps, u16 flags}`. Formats `0x15` = 32 bpp, DXT1/3/5 FourCCs; filter flags "taken from TR7.pdb" (`PCD2DDS/PCD.h:21-56`).
- **WAVE2WAV**: wave sections to RIFF.
- Confidence: medium-high. It is old (2019) and was written against an unknown TR8 build.

### 8.5 Templates (GPL-3.0) @`3022321`

- 010 Editor templates. TR8: `tr8_cdrm.bt` (CDRM block list), `tr8_section.bt` (section header bitfields, SectionType 0–13, and the 5-kind relocation table: intern, extern 14/24/26-bit, three 25/7-bit kinds).
- [V] Everything except the tool-only `magic` field matches all 1056 DRMs in our install ([CONTAINERS §4](formats/CONTAINERS.md#4-drm-version-19)).
- The `tr7ae_*` templates (object, mul, wave, raw image, fxanim) document the LAU predecessors of TR8 formats. Use them as hints only.

---

## Open questions

1. Is our GOG `tru.exe` byte-identical to retail 1.1 (after SecuROM removal) and to the unwrapped Steam exe? The Eidos signature is detached (`.extra` present, security directory zeroed). What post-processing did GOG apply?
2. DRM header words +8/+12/+16: +8 is 0 in all 1056 DRMs. cdcEngineTools calls it `paddingSize`, and the exe rounds it to 16-byte alignment when `hdr[4] & 1` ([CONTAINERS §4.1](formats/CONTAINERS.md#41-header)), so it is probably padding rather than a second list. What are +12 and the +16 flags? How is the root section chosen, given there is no `rootSection` field (DXHR has one)? Our observation: OBJECT is first in object DRMs and GENERAL is first in DRMs that have dependencies.
3. What are the semantics of the extern and three 4-byte relocation kinds (`Relocation3`, `ResourceRelocation32/16`: offset:25 / sectionType:7)? These are needed for object, material and render-resource linking.
4. PCD9: does the `0x15` format mean `D3DFMT_A8R8G8B8` (memory order B,G,R,A, as cdcEngineTools' raw-to-TGA path implies)? What are the `u8 depth`/`u16 flags` semantics, and how are cube and volume textures handled? This is [I] until it is verified with pixel output.
5. What is the layout of RenderResource (`.tr8mesh`, type 12)? Layouts for Material (10) and Object (11) are also unknown (TRLAU gives only partial runtime views).
6. Animation section (type 2): is the TR8 format G2-derived like LAU or new?
7. `.mul`: which codec does the PC payload use? The header `{sample_rate, loop_start(-1), num_samples, channels}` is observed, but sample rates such as 44115 and 44098 look odd. Are FSB4 records whole banks? What are the `Mus!` music scripts?
8. Which simulation step does the game use? Is `timeMult` a variable-timestep factor? Where exactly does physics break above 60 fps?
9. The unit (level) list: is there a full set of names (gibbed `bigfile.filelist` has 3216/3218)? We need a GPL-compatible way to derive names locally from exe and data strings.
10. Is the `StreamUnit` layout correct for TR8, given the 128-char unit names?
11. Script VM bytecode format (SCRIPT sections) and the native `Ns*` bindings. This is the largest RE item for gameplay.
12. Spec-mask language bits: bits 0–4 are verified (EN/FR/DE/IT/ES). Bit 11 = Dutch is a strong inference. Bits 5–10 and 12 are unused in the data. What runtime mask does each language selection set (around `Win32_GameMain`'s language loop and `"No language specified"`)? See [CONTAINERS §1.4](formats/CONTAINERS.md#14-specialisation-mask-specmask).
13. What is the static FMOD Ex version, and is EAX/OpenAL output required for parity?
14. `SKUType`="TRU D" and `Version`=256: does the exe branch on them?

## Prioritized format and RE work

1. **Relocation semantics, root section, dependency loading**: extern and resource relocs; DRM header words. Approach: Ghidra on `ResolveReceiver_ReceiveData` (`0x54F9B0`, the DRM header/section loader with the version check) and the `g_sectionHandlers[type]` vtables, plus `LOAD_UnitFileName` `0x477970` (`"%s%s.drm"`). This unblocks everything below.
2. **Textures (PCD9)**: confirm channel order and mip layout by decoding to PNG and comparing visually; add a test fixture.
3. **Render meshes (type 12 RenderResource) and materials (type 10)**: needed for the viewer milestone. Reference: Raq's Noesis plugin (behaviour only), TRLAU `MaterialData` (params at +0x58/+0x5C).
4. **Level/terrain**: the `Level`→`Terrain`→`TerrainGroup`→collision `Mesh` chain (TRLAU layouts, [§5.2](#52-underworld-structures)); portals and stream units; markup. Verify against `l2_puzzleroom.drm`.
5. **Objects and instances/intros**: `Object` layout, intro lists, objectlist/objlist.dat, `ObjectData.family`.
6. **Animation sections** plus the embedded anim debug names.
7. **Game loop and timing**: `GAMELOOP_Process` `0x5DFBE0`, `timeMult`/`timeDilation`; establish the original tick model before writing simulation.
8. **Camera system**: CameraManager `0xE804F0`, modes (`CAMERA_SetMode`), fov.
9. **Script VM**: `ScriptManager`, `ScriptTypeStreamData`, SCRIPT sections, `Ns*` natives. Large; schedule after rendering.
10. **Audio**: `.mul`/FSB4/`Mus!`; decode the PC codec and compare with vgmstream behaviour.
11. **Locale/UI**: `locals.bin` (verified), font, PDA map (type 13).
