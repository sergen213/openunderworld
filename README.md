# OpenUnderworld

A native Linux reimplementation of the engine of **Tomb Raider: Underworld** (2008, Crystal Dynamics' cdc engine), written from scratch in C++20 with OpenGL 4.6 and Vulkan 1.3 renderers.

> **You need your own legally owned copy of Tomb Raider: Underworld.** This project contains no game data, extracted assets or original code, and the releases contain none either. OpenUnderworld reads the files of your installation (tested: GOG build, `tru.exe` 1.1.0.0) and never modifies them.

Status: experimental. You can walk, jump and climb as Lara in the first level (`l4_pool`), view any level and play the game's movies and audio. It is far from a playable game; see [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/GAMEPLAY.md](docs/GAMEPLAY.md).

## Download and test

Every release ships an AppImage: [latest release](https://github.com/sergen213/openunderworld/releases/latest).

```sh
chmod +x OpenUnderworld-*-x86_64.AppImage
sha256sum -c OpenUnderworld-*-x86_64.AppImage.sha256

./OpenUnderworld-*-x86_64.AppImage            # launcher: pick the game directory, validate it, launch
./OpenUnderworld-*-x86_64.AppImage game --game-dir "/path/to/Tomb Raider Underworld"            # play l4_pool
./OpenUnderworld-*-x86_64.AppImage viewer --game-dir "/path/to/Tomb Raider Underworld" --level --drm 'pc-w\l4_pool.drm' --renderer vulkan
./OpenUnderworld-*-x86_64.AppImage tools     # list the bundled tools (game viewer movie audio import asset text sign)
```

- Controls in `game`: WASD/arrows move, Shift walk, Space jump/climb, Ctrl or C drop, mouse look, gamepad supported, Esc quits.
- Updates: the launcher's **Version & Updates** tab (or `--update`) replaces the AppImage in place with the newest signed release and keeps the old one as `.bak`. The manifest is signed with the project's Ed25519 key ([docs/UPDATES.md](docs/UPDATES.md)).
- Requirements: x86_64 Linux with a current glibc/libstdc++ (built on Arch Linux), an OpenGL 4.6 or Vulkan 1.3 driver (Mesa works), and FUSE (or add `--appimage-extract-and-run`).
- Unsigned test AppImages of every commit on main are available as artifacts of the `appimage` workflow on GitHub Actions.

## Build from source

See [docs/BUILDING.md](docs/BUILDING.md). Releases are made with `tools/release.sh` ([docs/RELEASING.md](docs/RELEASING.md)).

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). Tomb Raider is a trademark of its owners. This project is not affiliated with or endorsed by them.
