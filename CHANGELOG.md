# Changelog

All notable changes to OpenUnderworld are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
`tools/release.sh` publishes the section of the version being released as the GitHub release notes.

## [Unreleased]

### Added
- Lara wears each level's own outfit, as the original picks it from the level data: casual at Croft Manor, the warm wetsuit in the Mediterranean, jungle outfits in Thailand, Mexico and on the Andaman Sea ship, the snow gear on Jan Mayen and the cold wetsuit in the Arctic. The outfit changes with the chapter, is kept in saves and is the one cutscenes show (skeleton, hair and sounds from the outfit too).
- Configurable controls: every gameplay action (move, walk, sprint, jump, drop, interact, draw, fire, aim, switch target, weapon, health pack, grapple, quick save/load) can be bound to any keys, mouse buttons, gamepad buttons or triggers in `config.json` (`controls`) or on the launcher's Controllers tab, which captures the pressed input. Also mouse sensitivity and inverted look. Defaults are unchanged.
- Saves now include the enemies: every enemy's position, health, animation and AI state (also in streamed-out areas), fired spawn generators and the checkpoint's dead set. Loading no longer revives enemies you killed or resets ones mid-fight. Older saves still load (their enemies stay as they are).
- More level scripts run as in the original: timers (clocks), screen fades, death volumes and kill planes that hurt Lara, ledges that scripts switch off, objects scripts remove, triggers that wait for another area to load, and scripts that touch objects not loaded yet. The three chapter ends that only scripts reach (the sinking ship, the Croft Manor fire, the Natla ship) now run their real trigger scripts. Fixed a hang when two triggers linked to each other.

## [0.9.0] - 2026-10-28
- Lara's motorcycle: the bike waits where the original parks it (Mexico's trail start, Jan Mayen). Press E beside it to get on with the original mount animation, then ride with the move stick (forward throttle, back brake/reverse, left/right steer) and Space to hop; slopes, steps, ramps and jumps follow the terrain, big drops hurt as in the original (its fall-damage numbers), and riding into enemies knocks them down. E near standstill gets off. Steering and top speed come from the bike's own data; acceleration and braking are approximations. Wheels spin and the handlebars turn, the camera follows behind the bike, the first ride in Mexico plays "Lara's new bike", and saves keep the bike and whether Lara is on it.

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- The main menu now reproduces the Windows game's: its own art and font from `generalbank.drm` (glyph overlay, ring shadow, discs, icons, label bars, footer lines) at the layout measured from the original running at 1024×768 and 1920×1080, the full item set (PLAY GAME in the centre continues the newest save; START NEW GAME, GAME OPTIONS, LOAD GAME, SAVE GAME, AUDIO/VISUAL, EXTRAS, QUIT GAME on the ring), the original navigation (Left/Right between centre and ring, Up/Down along it), selection growth and label fade, the "Return SELECT" prompt, the "only in gameplay mode" message box for SAVE GAME, and `medsea_loop.bik` fitted to the window width behind it. Widescreen keeps the 4:3 layout centred, as the original. The pause menu uses the same radial art. EXTRAS (rewards) is not ported and shows "Unavailable".
- Native uber-object command dispatch and section state: the Kraken tomb door opens through its original command, and the l2 adrenaline elevator now moves its collision and carries Lara using the actual `falling` animation travel. Unit startup waits for all intros, and added the observed `instance` and two-argument `modifiable` bindings required by the elevator chain.
- Terrain-group SetEnabled/IsEnabled now reach the addressed streamed unit and preserve group state in merged collision, so trigger planes can enable or disable the terrain Lara queries.
- Script-controlled lights now enable/disable their rendered unit lights and invalidate the streamed scene. Added the original health getter and persistent `gamebase` global-byte get/set/add/subtract, allowing the l2 tutorial trigger and cinematic scripts to complete without unsupported-native fallbacks.
- Uber state-thread initialization now completes, duplicate command IDs retain member identity, and translational/rotational clip travel drives moving collision. The Mexico hallway ramps raise through their real `raising` animation; a real-data regression covers the second same-ID command.
- Added instance Reset, engagement selection, and hidden-state natives. Hidden intros are removed from the streamed render scene, visibility changes trigger rebuilds, and hidden/engagement state survives save/load.
- Added the original `unit.fn15` post-effect scalar binding; observed zero-valued reset calls now complete instead of falling back.
- Native SCMD script VM for common scalar opcodes, arithmetic, raw-word branches, nested/virtual calls and instance type checks. Real intro handlers now run with `instanceref`, `unitref`, `globalsref`, sticky boolean/integer and modifiable bindings; the Croft Manor electric switch opens its door through the original bytecode. Unsupported paths keep the previous static-link fallback.
- Level scripts now compare, copy and convert constant strings, preserve object references across moves, suspend on SLEEP/WAIT and resume nested handlers on later game ticks, and run sequential calls without returning to the parent handler. `MOVEU` copies the eight observed native value layouts at their actual widths. Dirty scalar and string script fields survive save/load.
- Script construction and WAIT-based multibody polling loops run for level objects. Native `multibody` enable/limit/read/drive calls connect to the hinge solver: the Mexico ballcourt pole lever now turns and its original script drives the door at twice the lever angle instead of using the old open-fully fallback. Also corrected SCMD local byte addressing, move stack order, CALL3 receiver semantics and null-call continuation.
- Added `playerref` lookup/loaded wait, loaded `instanceref` waits, dynamic instance property get/set and native instance-interface getters. Lara's actual type-205 script is returned to scripts, and dynamic properties persist in saves.
- START now launches a fresh fiber at the next statement boundary with clean locals/stack and returns to the caller when it finishes, instead of dropping the fiber. Real `l2_shrineroom` and `l2_towerroom` constructors now reach their field initialization.
- Every scripted intro, including cinematic objects, now reaches the runtime. Added native message-channel delivery, checkpoint events, target-load waits, sticky floats and additional instance/sensor/marker interfaces; original constructors continue through those calls instead of falling back to simplified links.
- Rail slides: type-8 markup now attaches from ground or air, accelerates downhill along the real polyline, and detaches on jump, drop or the endpoint. Verified on Valaskjalf rail 1562 in a real-data test and a rendered native descent.
- Sharks now spawn and swim in 3D using their original locomotion, bite, hit and death/floating clips. Restored 6 Mediterranean and 4 Thailand bluesharks plus 8 Arctic Greenland sharks; verified a Thailand shark chasing, biting and dying. Bite damage remains inferred because shark DRMs have no attack records.

## [0.8.0] - 2026-10-27

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- Ladders, pillars and perches: Lara climbs ladders with the original animations (push into them, climb off at the top or bottom, jump off), rotates around pillars, and balances on perches before jumping off. Verified on the sinking ship's ladder in chapter 1. Object-DRM markup was investigated: no extra traversal markup exists in this data.

## [0.7.0] - 2026-10-27

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- Grapple: G (or the right shoulder button) throws at the original `*_ei_grapple_*` anchors — a crosshair marks a hookable target. Lara swings on the rope with stick pumping, climbs up/down it (E / Shift), and jumps off with her momentum (Space or G/Ctrl to let go). Rope and target rendering, line-of-sight and range checks. Wall-running and pulling objects are still to come.

## [0.6.0] - 2026-10-27

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included.

### Added
- Free climbing: Lara now climbs the original game's climbable rock faces (the walls whose collision faces carry the climb flags, e.g. the cliffs in `l2_cliffclimb` and `l1_baldrsstairs_a`) with the original DICE animations. Push into a wall to attach (or fall onto it), climb up/down/sideways with the stick, climb onto the top, jump off (Space) or let go (Ctrl/C). Ledge and pole grabs keep priority.

## [0.5.0] - 2026-10-27

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included. Enhanced water is now on by default (Vulkan or OpenGL).

### Added
- Enhanced water shading in both renderers (`--water original|enhanced`, `graphics.water`; the enhanced preset uses it, the original preset keeps TRU's own water shaders): animated waves, screen-space refraction with depth-based absorption, screen-space reflections with Fresnel, sun glints, shoreline and crest foam, caustics on the pool floor, and an underwater fog/caustics view. Written for this project (no third-party water shaders).
- Water reacts to the world: a deterministic ripple simulation spreads rings from Lara diving in, her surface swimming and wading, nearby enemies and moving objects, and gunshots hitting the surface, with ballistic splash droplets. Original water rendering is unchanged.
- Oxygen: an air meter while Lara's head is under water (AIR bar next to the HUD), refilling at the surface; when it runs out she takes drowning damage until she surfaces or dies. Rates are inferred (45 s of breath), not yet read from the original executable.

## [0.4.0] - 2026-10-18

Experimental. Requires your own legally owned copy of Tomb Raider: Underworld (GOG build); no game data is included. Now defaults to Vulkan + original shaders + ray-traced sun shadows + 4x MSAA.

### Added
- Original Fidelity shading: level geometry now draws with Tomb Raider: Underworld's own shaders, translated from the game's Direct3D 9 shader bytecode to OpenGL and Vulkan at load time — baked light maps, the original multi-layer terrain blending, fog, the level's ambient and sun light, wind-animated foliage and environment reflections. Choose with `--shaders original|generic` or `graphics.original_shaders` (original is the default). Cube-map textures now load. `ou-asset shaders` surveys and `ou-asset shader-dis` disassembles the game's shader libraries.
- Object collision and physics doors: doors and push blocks now block Lara (and enemies and the camera) with their original collision boxes, and stop blocking once they open; the Mexico ballcourt's lever-driven door swings open on its hinge when Lara uses the pole lever; Lara is carried by objects she stands on.
- Optional hardware ray tracing in the Vulkan renderer (Enhanced preset): `--rt shadows` for soft ray-traced sun shadows (alpha-tested foliage included, no distance limit), `--rt full` adds water reflections; `--rt-quality low|medium|high`, config keys `graphics.rt` / `graphics.rt_quality`, status in the overlay. Falls back to the shadow map on GPUs without ray queries. `ou-game` now takes the graphics flags too (`--preset`, `--msaa`, `--shadows`, `--fxaa`, `--hdr`, `--rt`, `--rt-quality`).
- In-engine cutscenes: the 53 cinematic streams are decoded (camera with field of view and cuts, Lara's and the other actors' animation) and play in `ou-game` with their sound track and subtitles; chapter-closing cinematics play before the next chapter, script-triggered ones where the level activates them; Esc skips; `--cine NAME` plays one cutscene.
- Lara's combat: health packs (H / d-pad up: heal 640, 2 s cooldown, uses the packs she picked up), the uzi, shotgun (7 pellets) and speargun (also under water) with original stats, clips, reserve ammo and reloads, weapon switching (X / d-pad right) with each weapon's equip / unequip animations and models, hit reactions when enemies strike her, and adrenaline head shots (fill the meter by damaging enemies, then lock on close and press E: time slows, steer the pip into the reticle and fire). `ou-game --weapon uzi|shotgun|speargun` gives a secondary weapon for testing. Grenades are not in yet.
- Enemies: poachers now fight with rifles (bursts with the original shot count, spacing and damage, line of fire, reloads, rifle-butt melee) and bats fly at Lara and bite; every enemy sees and hits with its own data (sight cones, per-attack damage) instead of guessed values and dies with a hit-direction / head-shot / running death animation. Enemy shots draw tracers. Sharks are not in yet.
- Best look by default: the game, viewer and launcher now start with Vulkan (falling back to OpenGL with the reason logged when no Vulkan device works; `--renderer auto|opengl|vulkan`), the Enhanced preset (original TRU shaders, 4x MSAA, sun shadows, 16x anisotropic filtering) and ray-traced sun shadows on GPUs with ray queries (`--rt auto`). `--preset original` keeps the purist look. New brightness setting (`--brightness`, `graphics.brightness`, launcher), like the original game's brightness option.
- Sun shadows and ray tracing now work inside the original shaders: the level's real sun casts the shadow-map or ray-traced shadow onto the original terrain and foliage shading, and `--rt full` water reflections show on the original water shader. The shadow map is cascaded (three cascades out to 16000 units) instead of one map ending 8000 units away, in both OpenGL and Vulkan.
- Launcher: the Graphics page now has renderer, preset, ray tracing (+ quality), MSAA, sun shadows, shaders and brightness; the main **Play** button starts the game (main menu) with these settings, **Viewer** opens the level viewer.
- Original lighting: the levels' own point and spot lights (torches, lamps) now light the original shaders — positions, colours, radii, falloff curves, spot cones and torch flicker from the level data, each object getting its own nearest lights as the original engine does — so torch-lit interiors such as the opening bedroom are no longer near black. The sun's dappled-light cookie texture is applied in the levels that have one. Lara, enemies and cutscene actors now use their original shaders too, lit by the level ambient, nearby lights and the sun.

### Fixed
- Natla's and Amanda's faces and hands no longer render green (a detail normal map was taken for their skin colour).
- Rotating doors and other rigid moving objects, guns and cutscene props now turn their shading normals with them.
- Ray-traced alpha-tested shadow casters and water reflections read the wrong texture coordinates since the original-shader vertex inputs were added.

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

[Unreleased]: https://github.com/sergen213/openunderworld/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/sergen213/openunderworld/compare/v0.8.0...v0.9.0
[0.4.0]: https://github.com/sergen213/openunderworld/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/sergen213/openunderworld/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/sergen213/openunderworld/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/sergen213/openunderworld/releases/tag/v0.1.0
