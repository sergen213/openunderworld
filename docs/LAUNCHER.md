# ou-launcher

Code: `src/launcher/main.cpp` (Qt 6 Widgets, no moc), `src/config` (shared config library `ou_config`), packaging in `packaging/`.
Status labels: **observed** = run and seen; **inferred** = from the code, not exercised; **unknown**.

## What it does

The launcher is one window with six tabs and a bottom bar with **Save settings** and **Launch**.

| Tab | Function | Status |
|---|---|---|
| Game | Pick the game directory. Run the `ou_importer` scan in a worker thread, optionally hashing every file against the GOG hashdb. Show the detected build, `tru.exe` sha256/version, archive record counts and errors, hashdb results, and a verdict. Writes the full scan JSON to `$XDG_DATA_HOME/openunderworld/manifests/<exe-sha-prefix>.json`. | observed against the real GOG install (below) |
| Graphics | Renderer (OpenGL/Vulkan), resolution plus presets, fullscreen, FOV, vsync, start level (`drm`). | UI observed; values reach the viewer through config (observed via `ou-viewer --config`, below) |
| Controllers | SDL3 gamepad list (`SDL_GetGamepads`: name, type, GUID) with Refresh. | observed with no pad connected ("No gamepads detected"); listing a real pad: unknown, none attached |
| Saves | Back up `$XDG_DATA_HOME/openunderworld/saves` to `save-backups/<timestamp>/`. Restore a backup after first backing up the current saves as `<timestamp>-before-restore`. Open folder. | UI observed; backup/restore code inferred (the engine writes no saves yet) |
| Diagnostics | Report with versions (launcher, Qt, SDL), OS/session, viewer path, updater state, config, last scan summary, and the last 60 lines of the newest viewer log. Export to a text file. | report observed |
| Version & Updates | Version, updater-managed install, whether a release key is compiled in, changelog, enable switch, channel, manifest URL, Check / Download & install / Roll back. See `docs/UPDATES.md`. | UI observed; live HTTPS update unknown (no server) |

**Launch** saves the config, then starts `ou-viewer --config <config.json>` detached. stdout and stderr go to `$XDG_STATE_HOME/openunderworld/logs/viewer-<timestamp>.log`. The viewer is looked up in this order:
1. `install/current/bin/ou-viewer` (the updater-managed copy)
2. `ou-viewer` next to the launcher binary
3. `$PATH`

Clicking Launch inside the GUI was not exercised: the computer-use helper sees no windows on this Wayland session, so no clicks could be scripted (inferred).

Startup runs `ou::update::recover()` on the install root, so an interrupted update swap is repaired before anything else happens.

Never touched: the game directory (the scan is read-only) and the original game's own saves. No root is needed. All state lives under the XDG directories.

## Config file

`$XDG_CONFIG_HOME/openunderworld/config.json` (default `~/.config/openunderworld/config.json`). It is written atomically (temp file + rename).

```json
{
  "game_dir": "/path/to/Tomb Raider Underworld",
  "drm": "pc-w\\lara.drm",
  "renderer": "opengl",
  "width": 1280, "height": 720, "fullscreen": false, "fov": 60.0, "vsync": true,
  "updates": {"enabled": true, "channel": "stable", "manifest_url": ""}
}
```

Loading rules:
- Unknown keys are ignored, and missing keys keep their defaults.
- A bad `renderer`, size or FOV is an error.

`ou-viewer --config FILE` applies the file first, wherever the flag appears, so explicit flags such as `--width` override it. `--fullscreen` was added at the same time. Both are covered by the `Config.RoundTripAndDefaults` test.

## Command line

```
ou-launcher [--config FILE] [--game-dir DIR] [--tab N] [--scan] [--grab out.png] [--self-test]
```

- `--game-dir`: overrides the configured directory for this session only; it is saved only by Save settings.
- `--scan`: starts the import scan immediately.
- `--grab`: saves a PNG of the window (after the scan, if `--scan` is given) and quits.
- `--self-test`: builds the UI, processes events once and exits. It is the ctest `launcher_self_test`, run with `QT_QPA_PLATFORM=offscreen`.

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
- `packaging/PKGBUILD` builds `openunderworld-git` from the working tree: `cd packaging && makepkg`. Install the result yourself with `pacman -U`. `makepkg -i` was not run.
- `.github/workflows/ci.yml` runs on an `archlinux:latest` container. It does a Debug + ASan/UBSan build and runs ctest; real-data tests skip, display tests exit 77, Qt runs offscreen, and `OU_DISABLE_UPDATES=1` is set. It finishes with a `makepkg --nodeps` package build. The workflow has not run on GitHub: no remote repo exists yet (unknown).
