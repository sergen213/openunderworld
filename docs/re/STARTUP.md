# Startup, configuration and main loop

Addresses are VAs in the GOG `tru.exe`; names are applied from
`tools/ghidra/names/tru_gog.csv`. **[OBS]** = seen in decompile, **[INF]** = inference.

## Call chain [OBS]

```
entry 0x818421 -> ___tmainCRTStartup 0x818241
  -> WinMain 0x47A280 (hInstance=0x400000, cmdline, wShowWindow)
     -> Win32_GameMain 0x479C80
```

`Win32_GameMain` does, in order:

1. Duplicates the main thread handle, `GetModuleFileNameW` check (0x479BA0), loads
   registry settings (`Registry_LoadSettings` 0x4791D0, `Registry_LoadOptions` 0x477EA0).
2. Command line (lower-cased, `strstr`):
   - `-setup` → mode 1 (show launcher/setup dialog, then quit without running the game)
   - `-config` → mode 2 (show dialog, then run)
   - `-relaunchmce` → sets flag 0x9EEDCA (Media Center relaunch) [INF from name]
3. `TIMER_Init` 0x597EE0, `InitArchive` 0x473840 (BIGFILE), language probe over 4 entries
   at 0x890428 (fatal "No language specified"), `InitPatchArchive("PATCH.000")` 0x478930.
4. Single-instance: `CreateMutexW(L"TRUMUTEX")`; if another window of class `TOMBRAIDER8`
   exists it is brought forward (EnumWindows).
5. OS/driver checks with localized message boxes (string ids 0x138D..0x1396).
6. `ShowSetupDialogIfNeeded` 0x47B530 → `PCSetupDialog_Run` 0x51FBB0 (`DialogBoxParamW`)
   when `-setup`/`-config` was given or the stored display settings are invalid.
7. If continuing: `SetThreadAffinityMask(GetCurrentThread(), 1)` — **the main thread is
   pinned to CPU 0** — and `SystemParametersInfoW(0x3A/0x3B ...)` (sticky/toggle keys off).
8. Loop: `CreateGameWindow` 0x4790A0 (class `TOMBRAIDER8`, `WS_POPUP|WS_SYSMENU`, title
   "Tomb Raider: Underworld", WndProc `MainWndProc` 0x478BC0, cursor clipped), `Win32_InitRenderDevice`
   0x478640 (`g_pcDeviceManager` vt+0 with the settings block 0x890388, installs `DeviceCallback`), then `MainG2(0)` 0x5E0190 at normal priority; on device-creation failure a
   message box and the setup dialog are shown again.
9. Shutdown: save registry (`Registry_SaveSettings` 0x4796B0), restore accessibility keys.

## MainG2 [OBS]

```
MainG2 0x5E0190:
  if (MainG2_InitEngine 0x5DF390()) {          // RESOLVE_Init, section handlers, MAIN_DoMainInit 0x5DEF70
    do {
      0x774550(); 0x789220();                   // [?] per-frame engine services
      MAIN_ProcessMainState 0x5E0090(&mainTracker 0xE7ED60, &gameTracker 0xE7F088);
      STREAM_PollLoadQueue 0x5C1DA0();
      if (!MainG2_PlatformUpdate 0x47AFD0(gameTracker.timeMultUnscaled /*0xE7F194*/)) break;
    } while (mainTracker.done /*0xE7ED6C*/ == 0);
    0x5DF930();                                 // [INF] shutdown
  }
```

`MAIN_ProcessMainState` switches on `mainTracker.mainState`: 1 = play
(`MAIN_PlayGame` 0x5E0050 → `GAMELOOP_Frame` 0x5DFFE0), 2/5/6/7/9/10 = other states
(load, menu, etc. — not yet named), 8 = quit (sets `+0xC`).

`GAMELOOP_Frame` 0x5DFFE0 = `GAMELOOP_Process` 0x5DFBE0 → `STREAM_PollLoadQueue` →
`GAMELOOP_DrawFrame` 0x5DA1C0 → `GAMELOOP_UpdateTime` 0x5DA230 → `gameTracker+0x24++`.
So the timing for frame N+1 is computed **after** frame N is simulated and drawn; see
[TIMING.md](TIMING.md).

`MainG2_PlatformUpdate` 0x47AFD0: passes `dt_seconds = arg/30` to two sound/stream
updaters (0x5904D0, 0x5954D0), updates input/jobs (0x4AAFF0, 0x4AFF70, 0x4AB030, 0x4B05B0,
0x778820, 0x77A020), `0x5790B0(gameTracker.timeMult/30)`, ticks the script manager
(0xCC4F80), then `PumpWindowMessages` 0x47AC30 (`PeekMessageW`/`GetMessageW`/
`DispatchMessageW`; `Sleep(100)` while minimized/inactive; device-lost re-setup dialog).

## Configuration [OBS]

Registry root `HKLM\SOFTWARE\Crystal Dynamics\Tomb Raider: Underworld` (`RegOpenKeyExW`,
HKEY from caller):

| Key | Values (loader / saver) |
|---|---|
| (root) | SfxVolume, MusicVolume, VoVolume, DontSleepOnFocusLoss, DontPauseOnFocusLoss, DisableForceFeedback, UseReverb, EAXSupport, MouseSensitivity, Topmost, LanguageString, SKUType, FullInstall (`Registry_LoadSettings` 0x4791D0 / `Registry_SaveSettings` 0x4796B0); AimY/XInverted, CameraY/XInverted, CombatMode, Vibration, Language, SubtitleLanguage (`Registry_LoadOptions` 0x477EA0 / `Registry_SaveOptions` 0x478010) |
| `\Graphics` | AdapterID, AdapterIdentifier, FullscreenModeID, Fullscreen, Lock\*Window\*, AspectRatio, TextureDetail, Brightness, AntiAliasing, EnableFullscreenEffects, EnableDepthOfField, EnableVSync, EnableTripleBuffer, EnableShadows, EnableWaterFX, EnableReflection, EnableVolumetricFX, BestTextureFilter, Disable\* driver workarounds, UseRefDevice, UseD3DFPUPreserve, CreateGameFourCC, PresentationIntervalTwo, ... (`PCDisplaySettings_LoadFromRegistry` 0x51E120 / `_SaveToRegistry` 0x51DA80; in-memory block at 0x890388, 0x15 dwords) |
| `\Input` | key bindings (0x47AF20 / 0x478230) |

Also read: `HKLM\SOFTWARE\Microsoft\Direct3D\LoadDebugRuntime` (`PCDeviceManager_ctor`
0x523810) and the Explorer Shell Folders key (saves location, 0x78B340).

## Direct3D 9 device [OBS]

* `PCDeviceManager_Create` 0x5239C0: `LoadLibraryA("d3d9.dll")`, `Direct3DCreate9(0x20)`,
  singleton `g_pcDeviceManager` 0xAD75E4.
* `PCDeviceManager_BuildPresentParams` 0x522060 fills `D3DPRESENT_PARAMETERS` at
  `this+0x208`: `PresentationInterval` = `D3DPRESENT_INTERVAL_TWO` if PresentationIntervalTwo,
  else `ONE` if EnableVSync, else `IMMEDIATE`; `BackBufferCount` 2 with triple buffering;
  fullscreen mode id packs width (12 bit) | height (12 bit) | refresh (8 bit).
* `PCDeviceManager_CreateDevice` 0x522580: `IDirect3D9::GetDeviceCaps` (vt+0x38),
  behaviour flags 0x20/0x40/0x50 (SW/HW/HW+PURE vertex processing) |2 (FPU preserve)
  |0x100 [?], `IDirect3D9::CreateDevice` (vt+0x40).
* `PCDeviceManager_HandleDeviceLost` 0x522900: `TestCooperativeLevel`,
  `D3DERR_DEVICENOTRESET` → `Reset` with the saved present parameters.
* Vertex declarations: only `PCStreamDecl_CreateD3DResource` 0x528DB0 calls
  `IDirect3DDevice9::CreateVertexDeclaration` (vt+0x158); declarations are built by
  `PCStreamDecl_CreateFromVertexDecl` 0x529020 (callers 0x50EE60 model, 0x510DD0 terrain,
  0x5033E0). Details in [FILESYSTEM.md](FILESYSTEM.md#render-resources).
