# Changelog

All notable changes to OpenUnderworld are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
`tools/release.sh` publishes the section of the version being released as the GitHub release notes.

## [Unreleased]

### Added
- Main menu (START NEW GAME / LOAD GAME / GAME OPTIONS / QUIT GAME, original strings), pause menu (Esc / pad Start), load menu listing saves, options (SFX / music volume), PDA page with treasures/relics/health packs found; keyboard and gamepad navigation.
- New Game plays `explode.bik` and starts the prologue in Croft Manor; the original chapter sequence (Mediterranean Sea … Arctic Sea) with title cards, `--chapter N`, and level-to-level transitions at each chapter's closing cinematic.
- Treasures, relics and health packs can be picked up (E / pad West) with an on-screen notification; collected items, found counts and the chapter are saved.
- Music: the `.mus` sequencer is decoded and mixes the music stems; the track changes with the unit and with sound volumes; `--music-wav` captures the mix.

## [0.2.0] - 2026-10-18

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- Swimming: surface swim, tread, dive, underwater swim, surfacing and climb-out, using the level's water volumes (`l4_pool`).
- Original Lara tuning read from `lara.drm` (capsule, ledge-grab speeds, water test box); turning in place follows the clip's root rotation.
- Animation crossfades driven by the original anim-request table (fade lengths, start frames, walk/run phase matching); simulated hair cloth from `lara.drm` parameters.
- Level streaming across unit portals (e.g. `l4_pool` → `l4_ragnarok_room`), checkpoints/respawn, versioned atomic saves with F5/F9 quicksave/quickload and `--load`.
- Script runtime: trigger volumes and planes, switches (E / pad West), animated doors, push blocks, script state saved with the game; `--show-triggers`.

### Fixed
- Ledge climb-up no longer snaps ~150 units at the end (now under 10).

## [0.1.0] - 2026-10-18

First experimental build. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build, `tru.exe` 1.1.0.0); no game data is included.

### Added
- cdc container parsers: bigfile (with `patch.000` in front and the exe spec-mask selection), CDRM, DRM v19 sections and relocations.
- Asset formats: PCD9 textures (BC1/2/3, A8R8G8B8), render models, terrain, full level scenes (unit terrain, stream groups, IMF placements, intro instances), TRU material state (blend mode, alpha test, two-sided).
- `ou-viewer`: SDL3 viewer with OpenGL 4.6 and Vulkan 1.3 backends, fly camera, overlay, screenshots, Original/Enhanced presets (MSAA, sun shadow map, FXAA, HDR framework).
- Animation: skeletons, skin weights, animation clip decoding and sampling, CPU skinning.
- Collision: unit terrain collision meshes, markup polylines, own BVH with raycast/sweep/ground-probe queries; `--show-collision` overlay.
- `ou-game`: playable Lara in `l4_pool` (idle/walk/run/turn, jumps, fall, land, ledge hang/shimmy/climb-up) on a fixed 60 Hz tick, follow camera, keyboard/mouse/gamepad, deterministic `--input-script` replays, `--intro` movie, footstep audio and music.
- Audio: `.mul` IMA ADPCM, FMOD FSB4 banks, DRM Wave sections, SDL3 mixer; `ou-audio`.
- Video and text: Bink playback via libav (`ou-movie`), `locals.bin` strings and movie/voice subtitles (`ou-text`).
- `ou-import`: read-only install inventory and GOG hash check; `ou-asset` CLI with OBJ export.
- `ou-launcher` (Qt 6): game directory validation, graphics, controllers, save backups, diagnostics, signed updates.
- Signed updater (Ed25519, staged swap with journal recovery, anti-rollback) and `ou-sign`.
- AppImage packaging (`packaging/appimage/build-appimage.sh`), AppImage self-update in the launcher (`--check-update`, `--update`), local signed release script `tools/release.sh`, CI AppImage artifact.

[Unreleased]: https://github.com/sergen213/openunderworld/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/sergen213/openunderworld/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/sergen213/openunderworld/releases/tag/v0.1.0
