# ou-launcher

Code: `src/launcher/main.cpp` (Qt 6 Widgets, no moc), `src/config` (shared config library `ou_config`), packaging in `packaging/`.
Status labels: **observed** = run and seen; **inferred** = from the code, not exercised; **unknown**.

## What it does

The launcher is one window with six tabs and a bottom bar with **Save settings**, **Viewer** and **Play** (the default button).

| Tab | Function | Status |
|---|---|---|
| Game | Pick the game directory. Run the `ou_importer` scan in a worker thread, optionally hashing every file against the GOG hashdb. Show the detected build, `tru.exe` sha256/version, archive record counts and errors, hashdb results, and a verdict. Writes the full scan JSON to `$XDG_DATA_HOME/openunderworld/manifests/<exe-sha-prefix>.json`. | observed against the real GOG install (below) |
| Graphics | Renderer (Auto = Vulkan, else OpenGL / Vulkan / OpenGL 4.6), resolution plus presets, fullscreen, FOV, vsync, start level (`drm`, viewer only). Graphics: preset (Enhanced / Original (purist)), ray tracing (Auto: shadows on capable GPUs / Off / Shadows / Shadows + water reflections), RT quality (1/2/4 rays per pixel), MSAA (preset default/1x/2x/4x/8x), sun shadows (preset default/on/off), shaders (original TRU / generic), brightness (0.50–2.00, 1.00 = neutral; like the original game's brightness option). The RT widgets are disabled with the Original preset (RT needs Enhanced; `rt` is then saved as `auto`, which resolves to off). Save validates the graphics block with `ou::config::resolve` and shows the error instead of saving a config the apps would reject. | UI observed (offscreen grab); values reach the apps through config |
| Controllers | Per-action control bindings (table, Add input… capture of key / mouse / pad button / trigger, Clear, Reset all to defaults), mouse sensitivity, invert look Y; validated on Save (docs/GAMEPLAY.md "Controls"). SDL3 gamepad list (`SDL_GetGamepads`: name, type, GUID) with Refresh. | observed with no pad connected ("No gamepads detected"); listing a real pad: unknown, none attached |
| Saves | Back up `$XDG_DATA_HOME/openunderworld/saves` to `save-backups/<timestamp>/`. Restore a backup after first backing up the current saves as `<timestamp>-before-restore`. Open folder. | UI observed; backup/restore code inferred. `ou-game` writes `<slot>.ousave` and `<slot>.ousave.bak` there (docs/SAVES.md) |
| Diagnostics | Report with versions (launcher, Qt, SDL), OS/session, game and viewer paths, updater state, config, last scan summary, and the last 60 lines of the newest game/viewer log. Export to a text file. | report observed |
| Version & Updates | Version, updater-managed install, whether a release key is compiled in, changelog, enable switch, channel, manifest URL, Check / Download & install / Roll back / Restart (AppImage mode: the update is installed as `OpenUnderworld-<version>-x86_64.AppImage`, desktop entries follow, Restart starts the new file). See `docs/UPDATES.md`. | UI clicked on Xvfb 2026-10-30 against a local HTTPS server with a test-key-signed manifest (docs/UPDATES.md) |

**Play** saves the config, then starts `ou-game --config <config.json>` detached; it refuses to start until a game directory is set. Without `--level`, ou-game opens the main menu, where New Game starts chapter 0. **Viewer** does the same with `ou-viewer --config <config.json>` (debug fly camera over `drm`). stdout and stderr go to `$XDG_STATE_HOME/openunderworld/logs/game-<timestamp>.log` / `viewer-<timestamp>.log`. Each binary is looked up in this order:
1. `install/current/bin/ou-game` / `ou-viewer` (the updater-managed copy; skipped inside the AppImage)
2. next to the launcher binary
3. `$PATH`

Clicking Play/Viewer inside the GUI was not exercised: the computer-use helper sees no windows on this Wayland session, so no clicks could be scripted (inferred).

Startup runs `ou::update::recover()` on the install root, so an interrupted update swap is repaired before anything else happens.

Never touched: the game directory (the scan is read-only) and the original game's own saves. No root is needed. All state lives under the XDG directories.

## Config file

`$XDG_CONFIG_HOME/openunderworld/config.json` (default `~/.config/openunderworld/config.json`). It is written atomically (temp file + rename).

```json
{
  "game_dir": "/path/to/Tomb Raider Underworld",
  "drm": "pc-w\\lara.drm",
  "renderer": "auto",
  "width": 1280, "height": 720, "fullscreen": false, "fov": 60.0, "vsync": true,
  "updates": {"enabled": true, "channel": "stable", "manifest_url": "https://github.com/sergen213/openunderworld/releases/latest/download/manifest.json"},
  "graphics": {"preset": "enhanced", "msaa": 0, "rt": "auto", "rt_quality": "medium", "brightness": 1.0}
}
```

Loading rules:
- Unknown keys are ignored, and missing keys keep their defaults.
- A bad `renderer` (`auto` | `opengl` | `vulkan`), size, FOV or graphics value is an error.

Defaults (best look out of the box): `renderer` `auto` = Vulkan, falling back to OpenGL with a note on stderr when Vulkan is unavailable; preset `enhanced` = 4x MSAA + sun shadow map + TRU's translated shaders, FXAA and HDR off (the HDR tonemap compresses the original shaders' LDR output); `rt` `auto` = ray-traced sun shadows on Vulkan devices with ray queries (off with the `original` preset); `brightness` 1.0 = neutral (post-pass gamma `out = in^(1/brightness)`, 0.5–2.0). `msaa` 0 and absent `shadows`/`fxaa`/`hdr`/`original_shaders` mean "preset default". The `original` preset is the purist option: no MSAA, no shadow map, no post, no RT.

`ou-game --config FILE` and `ou-viewer --config FILE` apply the file first, wherever the flag appears, so explicit flags such as `--width`, `--renderer`, `--preset`, `--rt` or `--brightness` override it. Covered by the `Config.RoundTripAndDefaults` and `Config.GraphicsPresets` tests.

## Command line

```
ou-launcher [--config FILE] [--game-dir DIR] [--tab N] [--scan] [--grab out.png] [--self-test] [--check-update] [--update]
```

- `--game-dir`: overrides the configured directory for this session only; it is saved only by Save settings.
- `--scan`: starts the import scan immediately.
- `--grab`: saves a PNG of the window (after the scan, if `--scan` is given) and quits.
- `--self-test`: builds the UI, processes events once and exits. It is the ctest `launcher_self_test`, run with `QT_QPA_PLATFORM=offscreen`.
- `--check-update` / `--update`: headless updater using the config's update settings. Prints one result line and exits 0, or prints `Update failed: …` and exits 1. `--check-update` never writes anything. Inside an AppImage, `--update` replaces the AppImage itself (docs/UPDATES.md). Without a display, set `QT_QPA_PLATFORM=offscreen`.

When run from the AppImage (`$APPIMAGE` set), the Version & Updates tab updates and rolls back the AppImage file. **Play** and **Viewer** start the bundled `ou-game` / `ou-viewer` and ignore the install root.

## Verification (2026-10-18)

- **Live Wayland session (observed).** `ou-launcher --game-dir "/mnt/1st4TB/Tomb Raider Underworld" --scan`, captured with `spectacle -b -n -a`.
- `ou-viewer --config` (observed): a config with `drm "pc-w\\lara.drm"`, 800×450, FOV 70, vsync off rendered Lara with overlay "vFOV 70.0", "vsync off", 1120×630 framebuffer (HiDPI 1.4×).
- `launcher_self_test` (`--self-test`, offscreen, LSan on): exit 0 (observed).
  - The Game tab showed: "VALIDATION: OK", build "GOG 1900745791 (buildId 58082205556264096)", tru.exe sha256 `1fdbd345…`, file version 1.1.0.0, PE timestamp 2008-12-16T17:31:02Z, 37 files, bigfile 7339 records in 4 parts with 0 errors, patch 9 records, scan 0.5 s.
  - One warning: the GOG hashdb lists `add\gameux.cmd`, which is absent from the install. This matches `docs/formats/INVENTORY.md`.
  - Hash-every-file mode was not run from the launcher in this session. `ou-import` measured it earlier: 23/24 entries match and none mismatch (INVENTORY.md).
- **Other tabs (observed).** Captured with `--grab` on the live session: Graphics, Controllers, Diagnostics, Version & Updates.

## Packaging

- `cmake --install` installs `ou-viewer`, `ou-launcher`, `ou-import`, `ou-asset` and `ou-sign` to `bin/`, `packaging/openunderworld.desktop` to `share/applications/`, and `packaging/openunderworld.svg` (original artwork) to `share/icons/hicolor/scalable/apps/`.
- `packaging/PKGBUILD` builds `openunderworld-git` from the working tree: `cd packaging && makepkg`. Install the result yourself with `pacman -U`. Observed 2026-10-18: `makepkg -f` built `openunderworld-git-0.1.0.r12.g12a8f56-1`; `check()` passed 71/71 ctest; `package()` installed the 5 binaries, the .desktop file and the icon. The artifacts were then deleted. `makepkg -i` was not run.
- `.github/workflows/ci.yml` runs on an `archlinux:latest` container. It does a Debug + ASan/UBSan build and runs ctest; real-data tests skip, display tests exit 77, Qt runs offscreen, and `OU_DISABLE_UPDATES=1` is set. It finishes with a `makepkg --nodeps` package build. It is green on GitHub (observed 2026-10-18).
- AppImage and signed GitHub releases: see `docs/RELEASING.md`.
