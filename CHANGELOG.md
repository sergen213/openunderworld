# Changelog

All notable changes to OpenUnderworld are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
`tools/release.sh` publishes the section of the version being released as the GitHub release notes.

## [Unreleased]

### Added
- Material particles and unit FX point sets: particle defs with a material (`+0x168` / `+0x16C`) draw as soft additive flipbook flames (depth fade) or screen-space refraction of the scene (heat haze, muzzle distortion) in GL and Vulkan; the unit data's FX point sets (+0x120/+0x124) start their effects, so the prologue corridor fire burns on the rubble (docs/formats/EFFECTS.md "Material particles", "FX point sets"); test `FxReal.MaterialParticlesAndUnitPointSets`.
- Original glows and effect lights: class 2 glow records (muzzle and impact flashes, lamp glows, sun glares, light rays) drawn as sprites with their colour cycles, growth, fades, facing threshold and depth bias, untextured ones with `particle.drm` texture 130; class 6 effect lights (muzzle flash and metal-impact lights) lit through 4 reserved scene-light slots, endless ones of placed objects as scene lights with their light DTP; effect-map and stop-by-id commands; every weapon's ammo hit objects are preloaded so their sprites no longer draw white after a shot, GL + Vulkan (docs/formats/EFFECTS.md "Glows", "Lights").
- Grapple: pulling levers, statues and other movable targets (hold E: the target's own script handler runs), rappelling on walls (up, down, kick off) and wall-running along the rope's arc, also from the ground; swinging onto a floor lands Lara, and on the ground she stands holding the rope instead of being dragged. Tested on l4_trailstart, l2_norsehall, l2_puzzleroom, l1_kraken_room and l5_valaskjalf (replays in tests/replays).
- Free climbing uses all five of Lara's hand stances (the hold's type picks one) and plays the original hold-to-hold transition animations when she moves between holds of different types.
- Reveal cameras: unit camera splines (unit data +0x68/+0x6C) decoded and played like tru.exe's AutoSplineCamera — Hermite camera track with key times / ease curves / constant speed, slerped key orientations or a look-at target track (may follow Lara), FOV events, pause and spline-switch key events, dispatcher done callbacks; `autosplinecamera` 0, `cameraspline` 0, `gamebase` 52/43 (camera switch / current type), `larabase` 56 (Lara's stick zeroed while a reveal camera plays), `microphone` 1-7; 0.5 s blend back to the orbit camera. Engine-created globals objects (type 74 etc.), `instance` 27 GetUnit, clock alarm callbacks (corebase prototype 25), enemybase 18/27, gamebase 2/23/24/30. Script survey 265 -> 183 handler stops (`GameReal.RevealCameraPlaysUnitSpline`).
- Original combat and animation-event effects: muzzle flashes from the weapon objects' `fire` clip events at their muzzle markers, bullet impacts from the ammo's hit object (`bullet_hit`, ...) picked by the hit surface's collision material (stone dust, sparks, water spray, blood on bodies), casings as small sprites, and Lara's / enemies' clip events (footstep dust, splashes) on their posed bones, GL + Vulkan; the placeholder line muzzle flashes/impacts are gone (docs/formats/EFFECTS.md "Animation events and combat").
- Spline movers: intro +0x68 paths (Bezier/Hermite position, slerped rotation, 30 Hz keys) move their objects with the original play modes (once, loop, loop count, ping-pong, clip, play-to-key, key-range callbacks) and all `spline` script natives; moved objects with collision carry Lara. The l7_yggdrasil_end Natla movers and l1_seafloor fish now move.
- Script VM: array opcodes (DREF/BOUNDS) and thread blocks (TSSTART/TPSTART/TEND, run inline); `playersensor`, `instance` 18/21/23, `modifiable` 1, `unitref` 2 and `larabase` 25/27/29/55/56 natives. Script survey stops 472 → 265 (with more code now reached).
- Script function arguments are placed at their prototype byte offsets (from 0) instead of after a `this` slot; handlers that read their arguments (e.g. Lara's reveal control toggle, mover PlayFromKey) got the object's own id before.

### Fixed
- Lara no longer wears Thor's gauntlets and belt before she has them: the outfit models' gear primgroups (draw groups 10-12) start hidden and only Lara's own gear scripts (instance native 61, carried `g_*` objects) show them; state saved; a chapter start gives Lara the gear she carries by then (both gauntlets and the belt from Jan Mayen on, as the original does); checked against tru.exe under Wine that carried gear stays hidden in every chapter. Script INT/FLOAT/BOOL member defaults (stored inline in the member record) are now parsed; instances do not apply them yet.
- Lara's hand is no longer a big white glove in the Mexico outfit (`lara_jungle_heavy`, e.g. l4_ballcourt; OpenGL and Vulkan): that outfit keeps one of Thor's gauntlets as a model of its own, and hiding the gauntlet left an empty model that was drawn whole and untextured. Such models are now dropped; a test checks every outfit's visible parts have real textures and materials.
- Particle effects no longer depend on the frame rate: emitters' camera-range check used the last rendered (in-between) camera, so at different frame rates different effects spawned; it now uses the game tick's camera. Audit of every simulation path for frame-time use (docs/GAMEPLAY.md "Timing"): physics, movers, scripts, enemies and effects all run in fixed 60 Hz ticks, checked bit-identical tick by tick on real puzzles at 24-1000 Hz and irregular frame times; render rates dividing 60 Hz (120, 240) now land exactly on each tick.
- The sea is no longer white at the l1_seatop and l2_cliffclimb starts (OpenGL and Vulkan): TRU's water materials read the back buffer and depth buffer, which are now copied after the opaque scene and fed to them, so the sea shows refraction, reflections and depth fade.
- The l2 elevator only starts when Lara pulls its switch: objects now start in state 0 of their script, as the original does, and switches whose script handles the use itself get it.
- Scripted screen fades (fade to/from black) are now drawn in OpenGL and Vulkan.
- Lara can no longer free-climb any wall: she now only climbs where the level places the original's free-climb holds (eight areas, such as the Thailand cliff climb), and stops where the holds end.
- Much higher frame rates in busy levels (Vulkan): l4_ballcourt at 1440p went from about 22 to 45 fps. Enemies the camera can't see are no longer re-skinned every frame, the original shaders' lighting variants are compiled while the level loads instead of when materials first come into view (which also removes stutter), and animated characters' vertex uploads no longer stall the GPU.
- FSR is now faster than native rendering (1440p: Quality 16.4 ms and Performance 15.0 ms per frame against 19.6 ms native; 4K: 20.8 / 17.8 ms against 28.1 ms), and frame generation no longer halves the real frame rate: it now nearly doubles the frames shown (1440p Quality: 97 per second from 48 real ones; before, 26 from 13). Torch and other effect sprites, water splashes and rain/snow are now marked for FSR so they should ghost less. Fixed Vulkan validation errors with frame generation on.
- Faster again, same image: l4_ballcourt at 1440p 50 → 67 fps native, 59 → 83 fps with FSR Quality, 65 → 94 fps with FSR Performance (4K: 36 → 44, 47 → 62, 55 → 77), 111 → 151 shown frames per second with FSR Quality + frame generation; l4_pool 269 → 445 fps. The sun shadow map skips objects outside each shadow cascade, animated objects' ray-tracing updates are batched, visible enemies are animated on several CPU cores (OpenGL too: 51 → 65 fps), and all original-shader pipelines are built before the first frame instead of when materials come into view (no stutter even on a fresh install). Levels load faster (l4_ballcourt 19 → 13 s). With FSR, textures are now sampled at display-resolution detail (negative mip bias) instead of looking blurrier than native.

### Added
- Original particle effects (torches, splashes, ambient fx objects): the objects' own GenericFXObject particle definitions and birth events are simulated at 60 Hz and drawn as sprites in OpenGL and Vulkan, with the original depth bias and HInfo marker emitters; fx proxy boxes are no longer drawn (docs/formats/EFFECTS.md). `ou-game --camera X,Y,Z,TX,TY,TZ` fixes the view for screenshots.
- AMD FSR upscaling and frame generation (Vulkan): the launcher's Upscaler setting (or `--upscaler fsr`) renders the 3D scene at a lower resolution and lets FSR 3.1.6 rebuild a sharp full-resolution image (Native AA, Quality, Balanced, Performance, Ultra Performance; adjustable sharpening); menus and HUD stay at full resolution. Frame generation (`--frame-generation on`) shows an extra generated frame between real ones for smoother motion. Built from AMD's FSR SDK 2.4.0 (MIT); FSR 4 is not possible on Linux (AMD ships it only as Windows DLLs). OpenGL is unchanged; off by default.
- Flashlight and sticky grenades. L (d-pad left) toggles Lara's personal light: the original's omni light everywhere, plus its long spot beam in the levels whose data asks for it. R (d-pad down) throws a sticky grenade at the locked target or where the camera points; it sticks to walls and enemies and goes off after four seconds, hurting everything (Lara included) within the original's blast radius and pushing loose objects. Lara carries six; the HUD shows the grenade row and lights the d-pad item. Both are saved.
- Level scripts now run their state machines: a script that switches state starts that state's code, and several handlers of one object run side by side as in the original. Script calls on enemies and enemy generators (enable, disable, spawn, kill, behaviour) reach the enemy system, scripted damage hurts enemies, and the l1 Kraken room's tentacle commands run. Script handler stops in the all-level survey fell from 404 to 314.
- Enhanced water, atmosphere and weather (on with the Enhanced preset, each switchable in the launcher or with `--water-detail`, `--atmosphere`, `--shafts`, `--weather`): water gets rolling swell and flowing currents, glittering sun glints that disappear in shade, steadier reflections with a sky fallback and reflections of nearby torches; F7 / `--water-debug` cycles water debug views. Height fog and ground mist settle into valleys and stop at the water surface, and sunlight streams through foliage and around buildings. Snow falls on Jan Mayen and in the Arctic and Himalaya levels and rain on the Andaman Sea ship where the original has them, hidden under roofs; rain leaves splash rings on the ground. The Original preset is unchanged.
- The pause, save/load, options, player tailoring, PDA and loading screens now look like the original's, drawn with its own art and font at layouts measured from the Windows game. Pause is the main menu's ring with RESUME GAME in the centre; LOAD GAME and SAVE GAME open the original's second ring of save slots on the right (page arrows, four slots per page; Backspace deletes a save). GAME OPTIONS, AUDIO/VISUAL and VOLUME SETTINGS are the original list screens with their sliders and selection bar, and they work: combat reticles, view inversion, subtitles, brightness, and SFX, music and cutscene volume. START NEW GAME first asks for the PLAYER TAILORING preset. Key 1 opens the PDA (Croft VX 2 frame, picture windows, AREA INFO with this chapter's relics and treasures found out of its total, FIELD ASSISTANCE playing Lara's hint or task). Chapter loads show the original loading screen with the chapter's title and story.
- The original in-game HUD, drawn with the game's own art at the original layout: Lara's health silhouette (coloured by health) and meter, the adrenaline bar (which doubles as the breath bar under water, replacing the old AIR bar), the current weapon with its clip rounds, and the health-pack/flashlight/weapon/grenade d-pad. Each group appears when it matters (damage, weapon drawn, d-pad use) and fades out after a few seconds. Lock-on reticles and the grapple target marker use the original reticle art; pickup and save notifications, chapter title cards and subtitles use the game's font. The HUD stays inside a 5 % safe margin at any resolution, including ultrawide.
- In-game sound events: ambient sound emitters placed in the levels now play as Lara approaches and stop as she leaves, level scripts start their music stingers, effect and Kraken sounds, Lara gasps when she runs out of air, and T / Y (Hint / Answer) play Lara's help-on-demand line with its subtitle once a level has given a hint. Gameplay subtitles use the same two-line display as cutscenes. Script-set music variables now drive the music mix.
- Lara wears each level's own outfit, as the original picks it from the level data: casual at Croft Manor, the warm wetsuit in the Mediterranean, jungle outfits in Thailand, Mexico and on the Andaman Sea ship, the snow gear on Jan Mayen and the cold wetsuit in the Arctic. The outfit changes with the chapter, is kept in saves and is the one cutscenes show (skeleton, hair and sounds from the outfit too).
- Configurable controls: every gameplay action (move, walk, sprint, jump, drop, interact, draw, fire, aim, switch target, weapon, health pack, grapple, quick save/load) can be bound to any keys, mouse buttons, gamepad buttons or triggers in `config.json` (`controls`) or on the launcher's Controllers tab, which captures the pressed input. Also mouse sensitivity and inverted look. Defaults are unchanged.
- Saves now include the enemies: every enemy's position, health, animation and AI state (also in streamed-out areas), fired spawn generators and the checkpoint's dead set. Loading no longer revives enemies you killed or resets ones mid-fight. Older saves still load (their enemies stay as they are).
- More level scripts run as in the original: timers (clocks), screen fades, death volumes and kill planes that hurt Lara, ledges that scripts switch off, objects scripts remove, triggers that wait for another area to load, and scripts that touch objects not loaded yet. The three chapter ends that only scripts reach (the sinking ship, the Croft Manor fire, the Natla ship) now run their real trigger scripts. Fixed a hang when two triggers linked to each other.
- Lara's motorcycle: the bike waits where the original parks it (Mexico's trail start, Jan Mayen). Press E beside it to get on with the original mount animation, then ride with the move stick (forward throttle, back brake/reverse, left/right steer) and Space to hop; slopes, steps, ramps and jumps follow the terrain, big drops hurt as in the original (its fall-damage numbers), and riding into enemies knocks them down. E near standstill gets off. Steering and top speed come from the bike's own data; acceleration and braking are approximations. Wheels spin and the handlebars turn, the camera follows behind the bike, the first ride in Mexico plays "Lara's new bike", and saves keep the bike and whether Lara is on it.

## [0.9.0] - 2026-10-28

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
