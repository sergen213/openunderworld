# OpenUnderworld

A native Linux reimplementation of the engine of **Tomb Raider: Underworld** (2008, Crystal Dynamics' cdc engine), written from scratch in C++23 with OpenGL and Vulkan 1.3 renderers.

## Status

Experimental. OpenUnderworld loads the levels of your own copy of the game and lets you play parts of them as Lara. The main menu starts a new game at the prologue, and every chapter can be started directly. Lara can run, sprint, jump, climb, free-climb, swim, use the grapple, fight and ride the motorcycle. Many level scripts, switches and cutscenes run, and the game can be saved and loaded. It also plays the game's movies, music and sounds.

It is far from a complete game. Many puzzles and level transitions do not work yet, and a full play-through has not been verified. See [docs/ROADMAP.md](docs/ROADMAP.md) for the status of each area and [docs/PROGRESSION.md](docs/PROGRESSION.md) for the state of each chapter route.

## You need the original game

**You need your own legally owned copy of Tomb Raider: Underworld.** This project contains no game data, extracted assets or original code, and the releases contain none either. OpenUnderworld reads the files of your installation (tested: GOG build, `tru.exe` 1.1.0.0) and never modifies them.

Tomb Raider is a trademark of its owners. This project is not affiliated with or endorsed by them.

## Download

Every release ships a signed AppImage: [latest release](https://github.com/sergen213/openunderworld/releases/latest). Releases are experimental builds.

```sh
chmod +x OpenUnderworld-*-x86_64.AppImage
sha256sum -c OpenUnderworld-*-x86_64.AppImage.sha256
```

Unsigned test AppImages of every commit on `main` are available as artifacts of the `appimage` workflow on GitHub Actions.

## Requirements

- x86_64 Linux with a current glibc and libstdc++. Releases are built on Arch Linux; older distributions may not run them.
- A GPU driver with Vulkan 1.3 or OpenGL 4.5 or newer (4.6 is requested). Mesa works.
- Optional: a Vulkan GPU with ray queries for ray-traced shadows, reflections and lighting.
- FUSE to mount the AppImage, or run it with `--appimage-extract-and-run`.

## Quick start

```sh
./OpenUnderworld-*-x86_64.AppImage            # launcher: pick the game directory, validate it, Play (main menu)
./OpenUnderworld-*-x86_64.AppImage game --game-dir "/path/to/Tomb Raider Underworld"            # main menu, New Game = chapter 0
./OpenUnderworld-*-x86_64.AppImage viewer --game-dir "/path/to/Tomb Raider Underworld" --level --drm 'pc-w\l4_pool.drm'
./OpenUnderworld-*-x86_64.AppImage tools     # list the bundled tools (game viewer movie audio import asset text sign)
```

- **Controls** (defaults of the original game, rebindable on the launcher's Controllers tab): WASD or arrows move, Left Shift sprints, Left Alt walks, Space jumps and climbs, Left Ctrl drops or crouches, E interacts, Q grapple, left mouse fires, right mouse locks on a target, F5 / F9 quick save / quick load, Esc opens the pause menu. Gamepads are supported. Full table: [docs/GAMEPLAY.md](docs/GAMEPLAY.md#controls).
- **Graphics**: Vulkan by default, with OpenGL as fallback. The default Enhanced preset adds 4x MSAA, sun shadows and enhanced water on top of TRU's own translated shaders, plus ray-traced sun shadows where the GPU supports ray queries. Change them in the launcher, in the game's own display menus, or with flags such as `--renderer auto|opengl|vulkan`, `--preset enhanced|original` (purist), `--rt auto|off|shadows|full`, `--water original|enhanced` and `--brightness 0.5..2.0` ([docs/LAUNCHER.md](docs/LAUNCHER.md)).
- **Updates**: the launcher's **Version & Updates** tab (or `--update`) replaces the AppImage with the newest signed release and keeps the old one as `.bak`. The update manifest is signed with the project's Ed25519 key ([docs/UPDATES.md](docs/UPDATES.md)).

## Build from source

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja sdl3 glm libepoxy zlib openssl nlohmann-json gtest \
  libsodium curl qt6-base vulkan-headers vulkan-icd-loader shaderc ffmpeg
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure      # tests that need the game read OU_GAME_DIR and skip without it
build/ou-game --game-dir "/path/to/Tomb Raider Underworld"
```

Details, optional parts (AMD FSR, Qt launcher) and the viewer: [docs/BUILDING.md](docs/BUILDING.md).

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md): dependencies, build, tests, `ou-viewer`.
- [docs/LAUNCHER.md](docs/LAUNCHER.md): the launcher, `config.json` and graphics defaults.
- [docs/GAMEPLAY.md](docs/GAMEPLAY.md): `ou-game` options, controls, Lara's moves, combat, enemies.
- [docs/PROGRESSION.md](docs/PROGRESSION.md): chapter routes and which puzzles and gates work.
- [docs/SAVES.md](docs/SAVES.md): save files.
- [docs/RENDERING.md](docs/RENDERING.md): OpenGL and Vulkan renderers, ray tracing, FSR.
- [docs/UPDATES.md](docs/UPDATES.md): signed updates and AppImage self-update.
- [docs/RELEASING.md](docs/RELEASING.md): how AppImages and releases are built.
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones and status per feature.
- [docs/RESEARCH.md](docs/RESEARCH.md), [docs/formats/](docs/formats/), [docs/re/](docs/re/): file formats and reverse-engineering notes.
- [CHANGELOG.md](CHANGELOG.md): changes per release.

## Contributing

Changes are accepted only as pull requests, reviewed and merged by the maintainer. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Source-available, all rights reserved; see [LICENSE](LICENSE). Contributions via pull request only; see [CONTRIBUTING.md](CONTRIBUTING.md). Versions published before this change remain under GPL-3.0-or-later. Third-party components in `third_party/` keep their own licenses. Tomb Raider is a trademark of its owners. This project is not affiliated with or endorsed by them.
