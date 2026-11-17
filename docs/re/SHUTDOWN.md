# Engine shutdown (tru.exe)

From the main-loop exit to the process exit. Startup is [STARTUP.md](STARTUP.md); this closes the "Shutdown
(0x5DF930 and what follows) is unknown" gap in [ENGINE_ARCHITECTURE.md §1](ENGINE_ARCHITECTURE.md). Evidence tags as
in [CAMERA.md](CAMERA.md): **V** objdump, **D** decompiler only, **[I]** inference.

## 1. How the loop ends

`MainG2` 0x5E0190 (V, 5E01CB..5E01E7):

```
do { 0x774550; 0x789220; MAIN_ProcessMainState; STREAM_PollLoadQueue;
     if (!MainG2_PlatformUpdate(timeMult)) break;      // window closed / WM_QUIT
} while (mainTracker+0xC == 0);                         // quit flag
MAIN_Shutdown();  // 0x5DF930
return 0;
```

Two exits:

1. **Quit from the menu.** Script native `uigamedata` fn 45 → thunk 0x7A0F30 → 0x77C190 `UIGameData_QuitGame`
   (V, 77C19F `mov [0xE7ED60],8`) also reached from 0x77E93E. Main state 8 in `MAIN_ProcessMainState` 0x5E0090 sets
   `mainTracker+0xC = 1` (V, 5E0125). The loop test sees it after this iteration.
2. **Platform quit.** `MainG2_PlatformUpdate` 0x47AFD0 returns false (window messages: close / WM_QUIT) → `break`.

Both run the same shutdown. There is no save on quit: the last checkpoint is already in the committed SAVE buffer
and disk saves are explicit ([SAVEGAME.md](SAVEGAME.md)).

## 2. MAIN_Shutdown (0x5DF930), in call order

Roles are from each callee's decompile (D) unless marked; "frees X" means it deletes the object or block at that
global and zeroes it.

| # | Call | Role | Conf. |
|---|---|---|---|
| 1 | 0x593D10 | stop sound: active voice object vf 0x5C, 0x58B300(6), 0x593BE0, music stop 0x57FFB0, 0x483030(0), 0x5895F0 | medium |
| 2 | 0x478010 Registry_SaveOptions | copy the device settings (+0x15C, 0x15 dwords) to 0x890388 and write the options to the registry (existing name) | high |
| 3 | 0x4D0430, 0x4B0EA0, 0x4D01E0 | free script-side singletons at 0xA13F5C, 0xA13F10, 0xA13F4C / 0xA13F58 | medium |
| 4 | 0x77F390 HUD_Destroy | delete the `ScriptHUDMenu` at 0xFAE920 | high |
| 5 | 0x6EDCE0 | delete the object at 0xFADFB0 | low |
| 6 | 0x593D10 (again), 0x591EE0 → 0x590540 | stop sound again; free a sound bank table | medium |
| 7 | `*0xCCDEA0 = 0`; 0x5D1A80(0,0) STREAM_DumpAllLevels | "STREAM_DumpAllLevels(..)": unload every loaded unit | high (string) |
| 8 | 0x5C1990 STREAM_FreeUnitResources | "Freeing unit %s resources" for units in state 4 | high (string) |
| 9 | 0x5C1A80 OBTABLE_DumpObjects | "Clearing NO_DUMP flag in object %s", "Marking player object %s for dumping" | high (strings) |
| 10 | 0x4B1400 | free 0xA13F14 | low |
| 11 | 0x6E4E50 PDAMap_Shutdown | PDAMap_Save 0x6E4800, map data reset, two resource releases | medium |
| 12 | 0x774540 → 0x788720, 0x7887D0 | storage-category shutdown; free the category buffers (0xFAF100 ×count) | medium |
| 13 | 0x6D9650 → 0x6D8110 | walk the table 0x8AFEB4 calling each entry's vf 0x28(0) / 0x44 | low |
| 14 | 0x53CD20(0xAD7D08) | reset a render list block (0x52F4D0, 0x52F430, 0x52CF70, 0x52AF30) | low |
| 15 | 0x52C470, 0x48E2A0 | clear 0xAE4D74; release 0xA037F4 | low |
| 16 | `GetFS()->vf 0x20` | archive file system: close / flush | medium |
| 17 | 0x555B80 SAVE_Shutdown | free both SAVE buffers | high |
| 18 | 0x4761D0 | free the font table 0x9DE6D4 | medium |
| 19 | 0x5B1C70, 0x5C0100, 0x5C13D0 | `0xDBA170 = 0`; delete 0xDBA164; free `g_objectList` 0xDB94D0 and 0xDBA188 | high (5C13D0) |
| 20 | 0x5DF6D0 | instance system reset (0x5BDA40, delete 0xE7F060, 0x63C940, re-create 0xD98F94) | medium |
| 21 | 0x63E930 | clear the 0x60-byte table 0xEA63C8..0xEA78C8 (two 0x414380 per entry) | low |
| 22 | 0x489BB0 | NavPower (bfx) shutdown: 0x42CE50, 0x42FB60, delete 0xA03594, 0x8C0F3C, 0x8C0F08 … | medium |
| 23 | 0x54F020(0xE7EE90) | release the global-data DRM | medium |
| 24 | 0x478230 | save the input bindings to `SOFTWARE\Crystal Dynamics\Tomb Raider: Underworld\Input` | high (string) |
| 25 | 0x4838D0 | release the input devices (0xA02B70.., 0xA02B68 vf 0x40) | medium |
| 26 | 0x5E9FF0, 0x550590 | `0xE7F3FC = 0`; delete every RESOLVE section handler (`g_sectionHandlers`) | high (550590) |
| 27 | 0x57FEC0, 0x594BC0 | music player and sound system shutdown | medium |
| 28 | eight pairs 0x77Dxxx / 0x77F320 / 0x77FA40..0x77FB90 | release the eight UI script singletons 0xFAE8E4..0xFAE934 ([UI.md](UI.md) §5) | high |
| 29 | 0x5A99E0 | free the UI widget pools 0xCDC9E0 / 0xCDC9E4 … | medium |
| 30 | 0x579040 | ScriptManager: destroy every script type (vf 0xC), 0x577660 | medium |
| 31 | `*0xCC4F80` vf 4; delete `*0xCC4F5C` | script heap / GC [I] | low |
| 32 | 0x5BD890, 0x77A300, 0x4AFC10, 0x5752E0, 0x4B15B0, 0x77A490, 0x4B1160, 0x77A370, 0x5D2A80 | free 0xD98FA8, 0xFAE844, 0xA13F0C, the script GC `*0xCC4F80`, the six script variable stores `*0xA13F18`, 0xFAE880, 0xA13EF4, 0xFAE874, 0xDB6A50 | medium |
| 33 | 0x49A7D0 | free the localisation strings 0xA03854 | medium |

## 3. After MainG2 returns (Win32_GameMain 0x479C80, V 47A196..47A26E)

| Call | Role | Conf. |
|---|---|---|
| 0x4789F0 | delete `*0x9EEE08` (vf 0x38(1)) | low |
| 0x472AD0 | delete the file systems: 0x9CE280, 0x9CE284, 0x9CE288, `g_diskFileSystem` 0x9CE27C, 0x9CE28C | high |
| 0x4786D0 | delete the device sub-object 0x9EEDFC; 0x5BF810 | medium |
| 0x5237D0 | release `g_pcDeviceManager` (refcount +0xC; at 0: 0x5236E0 destroy, free) | high |
| 0x4796B0 Registry_SaveSettings | write the display settings (existing name) | high |
| `SystemParametersInfoW(0x3B, 8, 0x9DE708, 2)` | restore the user's StickyKeys setting saved at startup (SPI_SETSTICKYKEYS, SPIF_SENDCHANGE) | high (V import) |
| 0x4A1950 → 0x4A1730 | job system: release the worker semaphore, wait for and close every worker thread, delete the critical section, close the handles | high |
| `CloseHandle(mutex)` | the `TRUMUTEX` handle from startup [I, high: same stack slot] | high |
| `*0xE7EDB5 = 1`; return 0 | WinMain returns; the CRT exits the process. The window is not destroyed explicitly on this path [I] | medium |

## 4. Differences vs OpenUnderworld

| # | tru.exe | OpenUnderworld | Evidence |
|---|---|---|---|
| 1 | Options, display settings and input bindings are written to the registry at shutdown | Settings are written to the JSON config when the menus change them | src/app/game/main.cpp:2468-2480 |
| 2 | Explicit ordered teardown (sound → streams → objects → NavPower → input → script runtime → localisation; then file systems, device, job threads) | RAII destructors; `renderer->shutdown()` before the window goes; the mixer closes its device before SDL_Quit | main.cpp:3867, :2229 |
| 3 | Single-instance mutex released at exit; StickyKeys restored | No single-instance lock, no accessibility-key changes | grep of src: no `TRUMUTEX` / sticky-keys code |
| 4 | Quit request is a script native (uigamedata 45) → main state 8 | `Action::Quit` sets `running = false` | main.cpp:2833 |
| 5 | Window close also ends the loop and runs the same shutdown | `SDL_EVENT_QUIT` → `running = false` | main.cpp:3061 |
| 6 | PDA map saved into the save data at shutdown (0x6E4E50 → 0x6E4800) [I: purpose] | PDA state lives in the save slots only | src/ui/Pda.cpp |

None of these is a behavioural defect for the player; #1 means a crash in tru.exe loses option changes made in that
session, ours does not.

## 5. Unknowns

- The objects behind 0x9EEE08, 0xFADFB0, 0xA13F0C/10/14/4C/58/5C, 0xD98FA8, 0xDB6A50, 0xFAE844/74/80 (rows marked
  low).
- 0x774550 / 0x789220 at the top of every MainG2 iteration (storage / async task pumps [I]).
- What 0xE7EDB5 = 1 signals (read by the CRT exit path or a relaunch check [U]).
