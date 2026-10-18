# Changelog

All notable changes to OpenUnderworld are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
`tools/release.sh` publishes the section of the version being released as the GitHub release notes.

## [Unreleased]

### Added
- Object collision and physics doors: doors and push blocks now block Lara (and enemies and the camera) with their original collision boxes, and stop blocking once they open; the Mexico ballcourt's lever-driven door swings open on its hinge when Lara uses the pole lever; Lara is carried by objects she stands on.
- Optional hardware ray tracing in the Vulkan renderer (Enhanced preset): `--rt shadows` for soft ray-traced sun shadows (alpha-tested foliage included, no distance limit), `--rt full` adds water reflections; `--rt-quality low|medium|high`, config keys `graphics.rt` / `graphics.rt_quality`, status in the overlay. Falls back to the shadow map on GPUs without ray queries. `ou-game` now takes the graphics flags too (`--preset`, `--msaa`, `--shadows`, `--fxaa`, `--hdr`, `--rt`, `--rt-quality`).
- In-engine cutscenes: the 53 cinematic streams are decoded (camera with field of view and cuts, Lara's and the other actors' animation) and play in `ou-game` with their sound track and subtitles; chapter-closing cinematics play before the next chapter, script-triggered ones where the level activates them; Esc skips; `--cine NAME` plays one cutscene.

## [0.3.0] - 2026-10-18

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- Health and damage from the original data: 1000 max health, fall-damage curve (safe below 1280 units, lethal at 2215), hit invulnerability, regeneration to 25%, death animation and checkpoint respawn.
- Dual pistols with original stats (12-round clips, range-dependent damage), upper-body aim poses, lock-on and soft targeting, target switching, reload, HUD (health bar, reticle, ammo). F draw, LMB fire, RMB aim, Q switch.
- Enemy AI: panthers, thralls, tigers, spiders spawn from level intros and pool generators with original hit points; sight/chase/attack/hit-reaction/death; `--show-enemies`.
- Main menu (START NEW GAME / LOAD GAME / GAME OPTIONS / QUIT GAME, original strings), pause menu (Esc / pad Start), load menu listing saves, options (SFX / music volume), PDA page with treasures/relics/health packs found; keyboard and gamepad navigation.
- New Game plays `explode.bik` and starts the prologue in Croft Manor; the original chapter sequence (Mediterranean Sea … Arctic Sea) with title cards, `--chapter N`, and level-to-level transitions at each chapter's closing cinematic.
- Treasures, relics and health packs can be picked up (E / pad West) with an on-screen notification; collected items, found counts and the chapter are saved.
- Music: the `.mus` sequencer is decoded and mixes the music stems; the track changes with the unit and with sound volumes; `--music-wav` captures the mix.
- Traversal: balance beams (walk, turn round, jump along, step off), jumps pulled onto jump-landing and beam markup as the original's jump-to-target solver does, dropping off an edge into a hang (Ctrl/C), jumping from ledge to ledge (stick + Space: up, sideways, back), horizontal poles (grab, swing, fling to the next pole), crouch/crawl and roll (Ctrl/C), sliding down steep slopes.

### Fixed
- Walking slowly off an edge no longer flickers between falling and landing; small steep floor bumps (e.g. `l4_ragnarok_room`) no longer interrupt running with a fall.
- Lara's body now tilts with the swim direction under water.

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

[Unreleased]: https://github.com/sergen213/openunderworld/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/sergen213/openunderworld/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/sergen213/openunderworld/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/sergen213/openunderworld/releases/tag/v0.1.0
