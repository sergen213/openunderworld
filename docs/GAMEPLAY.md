# Gameplay: ou-game, Lara's controller, camera, input

Code: `src/game` (`Game.hpp`; `Player.cpp` controller, `World.cpp` world/anim set/camera/spawn, `Input.cpp` devices and
replay scripts), `src/app/game/main.cpp` (`ou-game`), tests `tests/test_game.cpp`.

Labels: **[O]** observed in game data, in our tru.exe decompile (GOG v1.1.0.0 VAs, `decomp_all.c`), or in a viewed
render; **[I]** inference; **[U]** unknown.

## Running

```
ou-game --game-dir "/path/to/Tomb Raider Underworld"            # pc-w\l4_pool.drm, 'player' intro, OpenGL
ou-game --game-dir DIR --renderer vulkan --level 'pc-w\l4_ballcourt.drm'
ou-game --game-dir DIR --input-script run.txt --frames 100 --screenshot run.ppm   # deterministic replay
ou-game --game-dir DIR --spawn -4291,4225.6,3100,-157.9          # X,Y,Z,yaw-degrees override (l4_pool ledge 1253)
ou-game --game-dir DIR --intro                                   # Movies/intro.bik first (ou_video, skippable)
ou-game --game-dir DIR --show-triggers                           # trigger volumes (orange, green inside) and planes
```

`--intro` plays the movie in its own plain window (the movie player needs a window without a GL/Vulkan context), then
opens the game window; `--intro-frames N` stops it after N frames. [O] `--intro-frames 60` played 60 frames, then the
level loaded (log only; the movie frames themselves are verified in VIDEO.md). Under ASan, SDL's renderer may pick
Vulkan, so the RADV 256-byte leak needs `tests/lsan_vulkan.supp`.

Controls: WASD/arrows move relative to the camera, Shift walks, Space jumps (while hanging: climbs up, or with the
stick jumps up/sideways/back to another ledge; on a pole: flings), Ctrl or C lets go of a ledge/pole and on the ground
crouches (held), rolls (running) or drops into a hang at an edge, E uses switches / pushes and pulls blocks (with
forward/back held), mouse orbits the camera.
Gamepad: left stick, right stick, South = jump/climb, East = drop, West = use, left shoulder = walk. F1 toggles the
overlay, Esc / pad Start opens the pause menu ("Progression" below; without `--level`/`--chapter`/`--load` ou-game
starts at the main menu). `--fov` / the launcher config set the vertical FOV. Switches, doors, trigger volumes/planes and
blocks: `src/script`, docs/formats/SCRIPTS.md; script events are logged to stderr. Supported handlers execute their
original SCMD bytecode, including nested calls, strings, sticky/modifiable state and SLEEP/WAIT resume across ticks;
unsupported natives use the static-link fallback.

Input scripts: one event per line, `<seconds> <action> [value]`, `#` comments. Actions `forward`, `right` (stick
axes −1..1), `walk`, `jump`, `drop`, `interact`, `save`, `load` (0/1, default value 1; `save`/`load` = F5 quicksave /
F9 quickload, docs/SAVES.md), `lookx`, `looky` (camera rate, rad/s). A value holds
until changed; an event at `t` applies from tick `round(t · 60)`. With `--input-script` each rendered frame advances
`1/--render-hz` seconds (default 60), so `--frames N` screenshots are reproducible.

Audio (`ou_audio`, docs/formats/AUDIO.md; `--no-audio` turns it off): lara.drm Wave sections are registered and the
unit's music stream `pc-w\music\<unit>_mus_01\<unit>_mus_01.mul` loops (all stems at gain 1; [I] the naming is seen
in the bigfile, how the game's Mus! sequencer picks and mixes tracks is [U]). Footsteps [I]: during walk/run/stop a
foot (bones 8 and 13) dropping below 12 units in model space plays `fst_boot_stone_b_heel_01..10` (Wave 0x11A..0x123);
leaving the air plays `fst_boot_stone_a_land_01..05` (0x1D3..0x1D7). Stone only: the surface-material → sound mapping
is not decoded. Sound events (sfxmarker ambience, script sounds, drowning line, T / Y help-on-demand Hint / Answer
lines with subtitles; input-script actions `hint`, `answer`): docs/formats/AUDIO.md §9. [O] A 5.5 s replay (run, running jump, walk) opened the SDL device, played l4_pool_mus_01 and 11
footstep/landing sounds without errors; the result was not listened to or captured.

## Structure

- `World` owns the unit's `CollisionMesh`, Lara's `AnimLibrary`, the `Player` and the camera. `World::tick(Actions)`
  is one fixed 1/60 s step; it keeps the previous state so the app renders `interpolated(alpha)` with
  `engine::FrameClock::alpha()`. `src/game` has no renderer types; SDL is used only for device mapping.
- `ou-game` merges `formats::loadLevelScene(unit)` with Lara's skinned meshes from `loadDrmScene("pc-w\lara.drm")`,
  CPU-skins her every frame (`anim::sample` → palette → `skinVertices`), moves the vertices by the player matrix and
  uploads them with `Renderer::updateVertices`. Texture ids already loaded by the level are not added twice.
- Spawn: the intro whose object id maps to `player` in `objectlist.txt` (LEVELS.md). [O] l4_pool has one, at
  (−14885.4, −42.6, 4721.2) with rotation z = 0, at the top of the entrance stairs. Heading = rotation z − 90°
  because the model's forward is −Y ([O] all locomotion root motion is along −Y). The spawn snaps to the floor below.

## Animation mapping (pc-w\lara.drm) [O]

Measured with `rootTranslation` (segment 0 translation; 3000 anim units/s). Model space: −Y forward, −X right.

| State | Clip (anim id) | Keys, length | Root motion | Loop |
|---|---|---|---|---|
| Idle | `ng_idle` (6) | 50, 1.67 s | none | yes |
| Walk | `ng_walk` (251) | 38, 1.27 s | −Y 11.6 → 440.6: **348 units/s** | yes |
| Run (from idle) | `ng_run_start` (277) | 27, 0.90 s | −Y 0 → 849.4 | no, then `ng_run` |
| Run | `ng_run` (265) | 20, 0.67 s | −Y 42.5 → 849.4: **1274 units/s** | yes |
| Stop | `ng_running_stop` (248) | 17, 0.57 s | −Y 21 → 244.5 | no |
| Turn (idle, > 100°) | `ng_turn_center` (257) | 4, 0.27 s | root rot Z 0 → −1.57 rad (a quarter turn right), **7.85 rad/s** looped | yes; its root rotation drives the yaw, either direction |
| JumpPrep (standing) | `ng_standing_jump` (291) | 5, 0.33 s | none (pelvis dips 324 → 264) | no |
| JumpPrep (running) | `ng_running_jump_forwards` (259) | 2, 0.067 s | −Y 42.5 → 67.3 | no |
| Jump (standing) | `NG_InAir_Feet_Forward_stage1_standing` (313) → stage2 → stage3 | 14, 0.97 s | none | no |
| Jump (running) | `NG_InAir_Feet_Forward_stage1` (315) → `_stage2` (316) → `_stage3` (317) | 21/14/15 keys | none | stage3 loops |
| Fall | `NG_InAir_Feet_Forward_stage3` (317) | 15, 1.0 s | none | yes |
| Land (slow) | `ng_generic_land_forwards` (290) | 11, 0.73 s | none | no |
| Land (running, stick held) | `ng_running_land` (261) | 20, 0.67 s | −Y 84.9 → 891.8 | no, then `ng_run` |
| Hang (grab) | `ledgeb_attach_M` (209) | 9, 0.63 s | zero | no |
| Hang | `ledgeb_idle` (223) | 15, 1.0 s | none | yes |
| Shimmy | `ledgeb_right` (227); left = played backwards | 21, 0.70 s | −X 4.3 → 200.2: **294 units/s** | yes |
| ClimbUp | `ledgeb_climbup_fast` (221) | 23, 0.77 s | (52.5, −178.0, 644.1) | no |
| SwimSplash (fall into water) | `swim_jump_land` (390) | 16, 1.10 s | none (standing → swim pose) | no |
| SwimTread | `swim_surfaceidle` (386) | 24, 1.60 s | none; feet at −352, root = water surface | yes |
| SwimSurface | `swim_surface` (379) | 19, 1.27 s | none (body horizontal at the root) | yes |
| (swim speed source) | `swim_surfaceidle_to_swim` (385) | 5, 0.37 s | −Y 206.1 | — |
| SwimDive | `swim_surfacedive` (382) | 18, 1.20 s | (5.0, −112.3, −666.8) | no |
| SwimUnder idle | `swim_submerged_idle` (199) | 47, 1.57 s | none | yes |
| SwimUnder | `swim_sub` (370) at every pitch (the model is pitched); Surfacing = `swim_sub_up` (373) | 21, 1.40 s | none; body pitch 8.8° (`swim_sub_up` 44.1°, `swim_sub_down` −30.6°) | yes |
| SwimClimbOut | `swim_climbout` (368) | 47, 1.57 s | (0, −308.7, 172.8) | no |
| Crouch (enter) | `crouch_idleToCrouch` (55) | 10, 0.33 s | none | no |
| Crouch | `crouch_Idle` (54) / `Crouch_Front` (53) moving | 30, 2.0 s / 31, 1.03 s | none / −Y 11.3 → 350.2: **328 units/s** | yes |
| Roll | `ng_roll_front` (1254) | 25, 0.83 s | −Y 63.7 → 1200.3 (1364 units/s) | no |
| Slide | `slope_slidefront` (358) | 37, 2.47 s | none | yes |
| Beam (get on) | `bbeam_walk_attach` (767), from the air `bbeam_jumpland` (722) | 5, 0.33 s / 23, 1.56 s | −Y 63.8 / none | no |
| Beam | `bbeam_idle` (721) / `bbeam_walk` (468) | 50, 3.37 s / 53, 3.57 s | none / −Y 4.7 → 505.8: **140 units/s** | yes |
| Beam turn | `bbeam_walk_turn180` (469) | 7, 0.47 s | root rot Z 0 → −π (drives the yaw); −Y 46.4 not applied | no |
| DropToHang | `drop_to_ledgeb` (1559) | 31, 1.03 s | (11.0, −218.8, −502.2), root rot Z 0 → π | no |
| HangJump up / back / side | `ledgeb_jump_up` (905) / `ledgeb_jump_back` (903) / `ledgeb_jump_right` (894) | 5, 0.37 s / 7, 0.23 s / 5, 0.17 s | not applied (ballistic flight); `_back` root rot Z −π drives the yaw | no |
| Hang (after a jump) | `ledgeb_attach_U` (213) / `_F` (208) / `_S` (210) | 8 / 8 / 9 keys | ignored (end pose = hang pose) | no |
| PoleHang (grab) | `Hpole_perp_attach_M` (1541) | 45, 1.5 s | ignored | no |
| PoleHang | `HPole_idleHangPerp` (127); swing `HPole_StartSwing` (143) → `HPole_swing` (146); stop `Hpole_swingstopA` (152) | 30, 1.0 s; 70, 2.33 s → 41, 1.37 s; 41, 1.37 s | none | idle/swing yes |
| Pole fling | `hpole_perp_fling` (423) | 7, 0.5 s | (−1.1, −382.6, 69.5) not applied (ballistic flight) | no |

- [O] No exe string names these clips; they are the TR8 anim-list debug names in lara.drm. The state → clip choice is
  ours [I]. `ledgeb_*` (braced: feet on the wall) was chosen over `ledgeu_*` (free hang) [I].
- [O] The air clips have no root motion: flight is physics-driven (below). There is no `ledgeb_left` or
  `ledgeb_jump_left`; [I] the game probably mirrors the right clips. We play the right clips for both sides.
- Clips whose root rotates (`drop_to_ledgeb`, `bbeam_walk_turn180`, `ledgeb_jump_back`, `ng_turn_center`) turn the
  heading by the root's Z rotation each tick; their root translation is applied in the frame the clip started in
  (`Player::advanceRoot`), since the pose is rendered with the root at bind.
- Pole hand anchor [O]: hands (bones 55, 80) in `HPole_idleHangPerp` at t = 0: (−3.4, 17.0, 619.4) — above and
  slightly behind the root; placed on the pole axis.
- [O] The swim loops have no root motion: swimming is code-driven in the original. Only the one-shots
  (`swim_surfaceidle_to_swim`, `swim_surfacedive`, `swim_climbout`) move the root.
- Hand anchor [O]: bones 55 and 80 (last arm segments before the fingers) in `ledgeb_idle` at t = 0 sit at
  (72.8, −141.5, 495.2) and (−60.1, −141.9, 496.0); their mean (6.3, −141.7, 495.6) is placed on the grabbed ledge.
  `AnimLibrary::handsAt` computes the same anchor for any clip and time, with the root segment held at bind as rendered.
- Start frames: clips start at the original anim request's start frame (`clipBlend`, ANIMATION.md); the climb-up
  alignment below uses whatever start time `play` chose.

## Numbers

| Quantity | Value | Provenance |
|---|---|---|
| Unit scale | 3.1605 units = 1 cm | [O] hair chain converts m/s² with `* 100 * 3.1605 / 30 / 30` (`decomp_all.c` ~line 73478) |
| Simulation tick | 1/60 s fixed, render interpolated | ours; exe uses variable `timeMult = dt / 33.3 ms` (TIMING.md) |
| Gravity | 5.72443 units/frame² at 30 Hz = **5151.99 units/s²** (≈ 16.3 m/s²) | [O] bit pattern 0xC0B72E88 passed to the gravity setters 0x70E100 / 0x70E050 (27 occurrences); literal −5.72443 in the jump-to-target solver `FUN_0073EDC0` |
| Air integration | `pos += v·t + a·t²/2; v += a·t` | [O] `FUN_005DB370` (Instance +0x2A0 velocity, +0x2B0 acceleration); ported exactly, so results do not depend on the step |
| Terminal fall speed | none | [O] no clamp of zVel found; we do not clamp |
| Walk / run speed | root motion: 348 / 1274 units/s | [O] clips above; [O] "ART WARNING: Walk-onto-slope animation covers no distance!" (0x73FE10) shows ground locomotion is animation-driven; [O] physics mode 3 (ground) is not integrated by 0x715340 |
| Running-jump horizontal speed | ng_run root speed (1274) | [I] the exe launches with an "animation-driven velocity snapshot" (player data +0x1F0, `case 99/2` of 0x716F30); computed from `ng_run` at load |
| Jump vertical speed (standing, running) | **1803 units/s** → apex 315.5 units (≈ 1.0 m), 0.70 s air time on flat ground | [I] chosen so a flat running jump lasts exactly `NG_InAir_Feet_Forward_stage1` (0.70 s = 2v/g). [U] not in Lara's player data block (below): no exe code was found writing a take-off zVel from it; [I] it is set by the state-chart scripts (lara.drm script sections) |
| Standing forward jump speed | 637 units/s (= run / 2) | [I] guess, no evidence |
| Capsule | radius 100, height **512** | radius [I] from the player physics proxy 0x7018E0 → `FUN_0049a410(inst, 100.0, …)` (capsule volume code 0x4EE310); height [O] player data +0x10 = 512.01 (used as Lara's height: ×0.5 centre, ×0.75 head, capsule test `FUN_007144b0(pos, height, …)`), TRLAU-menu-hook `GeneralAttributes.height` [I name] |
| Step up / down | 70 / 70 units | [I] ours; [U] no step height found in the player data |
| Walkable slope | normal z ≥ 0.7 (≈ 45.6°); a steeper face is still stood on when one of four probes 60 units around the root finds a walkable face within 20 units of its height (bumps, seams: l4_ragnarok_room's 0.6-normal floor strips) | [I] ours |
| Slide | floors with 0.3 ≤ normal z < 0.7 under an unsupported root: down the fall line at +2500 units/s² up to 1400 units/s | [I] ours; [O] the exe has `SlopeSlideState` / `SkiSlopeSlideState` / `SlideState` (RTTI) whose virtuals are all the empty 0x7987B0: the state chart drives them |
| Edge slide-off | a capsule resting on an edge corner (floor-like contact, no floor under the root) gets ≥ 200 units/s outwards | [I] ours (fixes the Fall/Land alternation walking slowly off a ledge) |
| Crouch capsule height | **260** | [O] player data +0x14 |
| Turn rate (walk/run steering, swimming) | 6 rad/s | [I] from `ng_turn_center`; turning in place uses the clip's root rotation instead (above) |
| Ledge grab | hands within 60 units horizontally, ledge between hand height −40 and +10, falling, facing the ledge (cos ≥ 0.5), **−2232.5 ≤ zVel ≤ 1545.6 units/s** | zVel window [O]: 0x759260 allows a grab when `−byte[data+0x834]·5.72443 ≤ zVel ≤ byte[data+0x833]·5.72443` (units per 30 Hz frame); the bytes are 13 and 9. Geometry [I] ours. What the exe does with the other bytes [O read, I meaning]: 0x759260 / 0x7594C0 get two anchor positions from 0x758DD0 (anchor selector byte +0x820 = 0, resp. +0x821 = 3), raise both by int +0x824 (**+63**) and sweep the segment between them with collision query 0x4935F0 (mask 0x2C19E7); a hit of kind 3 or 5 accepted by 0x5D3BA0/0x758F70 is repeated at int +0x828 (**−158**): the ledge face must span 158 below to 63 above the anchor. 0x7591E0: a probe from the root down int +0x82C (**31**) must find no floor. 0x758B00: the grab point must lie within float +0x838 (**951.3**) horizontally of a position stored at player-state +0x9C0 ([U] probably the take-off point). Which bone/point the anchor is is [U], so these are not used |
| Ledge targets (drop to hang, hang jumps) | the hang side must drop > 200 within 240 units of the ledge line | [I] ours; l4_pool's ledge 1253 sits up to ~180 inside the collision top, so a fixed 60-unit probe misses the drop |
| Balance beam | root within 30 (x/y and z) of a type-14 line, heading within 45° of it from the ground (any heading from the air); walk 140 units/s (clip) | [I] attach distances ours; [O] markup type 14 = MUD 0x10000 BALANCEBEAM |
| Jump assist | at take-off, of the jump-landing (2) and beam (14) lines the point nearest the natural landing (at its height) within **300** becomes the landing: horizontal velocity = Δxy / flight time, vertical unchanged | [O] solver 0x73EDC0: `v = (target − pos − ½·g·t²) / t` with g = −5.72443/frame², re-aimed every frame with the remaining time (0x751D10, player-state +0x77C) at the markup in +0xA64 whose MUD flags match 0x20830000 (jump landing, rail slide, perch, balance beam); speed cap player data +0xC10 = 18963 (never reached). [I] the 300 radius and solving once at take-off |
| Hang jumps | from a hang: up (stick forward/neutral, no top to climb): hands 150..700 above, ±100 aside; side: 150..800 aside, ±150 in/out, ±400 height; back: 200..1200 behind, ±200 aside, −700..+200 height, turning round. Target nearest to 400 up / 400 aside / 700 back + 100 down. Flight rises 60 above the higher end and lands exactly on the target's hang pose | [I] ours (the exe's ranges are in the state chart) |
| Poles | types 4 (`pole`) and 6 (`horizpole`); falling hands (pole pose) within 60 of the axis, same zVel/height window as ledges, facing across it (cos ≥ 0.5). Fling: to the pole 300..1800 ahead (±300 aside, ±500 height), else a leap at run-jump speeds | [I] ours; l4_traprooms' poles are 1264 apart; [O] the markup's matrix carries real translations there (COLLISION.md) |
| Water probe | box ±221 × ±221 × ±572 centred 320 above the feet | [O] shorts data+0xC3E/+0xC40/+0xC42 (221, 572, 320), box overlap against each water volume box in 0x710A20; the highest surface wins |
| Swim speed | **562 units/s** (surface and under water) | [I] mean root speed of the one-shots that start swimming: `swim_surfaceidle_to_swim` 206 units / 0.367 s, `swim_surfacedive` 676 units / 1.2 s (562 and 563 each); [U] the exe's swim speed |
| Swim depth | water ≥ 400 under the surface is swum, shallower walked | [I] ours (tread pose feet at −352) |
| Water drag (splash) | velocity × e^(−3 t) | [I] ours |
| Buoyancy, air/oxygen | none | [U] no buoyancy or air timer found (only `death_drown` exists); not implemented |
| Fall damage | 0 up to **1280.03**, linear to 500 at **1817.32**, linear to 1000 (= death) at **2215.55**, death above | [O] floats data+0x4C/+0x50/+0x54 and the curve 0x6F1790; height = z on becoming airborne (player +0x6DC, set on FallState entry 0x75C550) − landing z; applied on landing (0x75D080 → 0x702FC0), never scaled by difficulty (Combat below) |
| Camera | orbit 700 units behind a point 450 above the feet, pitch −1.2..0.7 rad, sphere-swept (r 20) pull-in | [I] ours. [O] LaraCamera (vtable 0x880574) clamps distance at 1000 and has a 300-unit floor in its collision pull-in; defaults are data-driven (`ExplorationCameraResource`) [U] |
| FOV | config / `--fov` (default 60° vertical, Hor+) | [O] BaseCamera ctor 0x5E1B60 defaults fov to π/2; the game's exploration FOV is data-driven [U] |

### Lara's player data (lara.drm) [O]

- The object section (type 11, id 8) holds resource references (+0 → DTP 850, +4 → 849, +8 → 848). DTP 850 is the
  ObjectBase: ObjectSection construct 0x54E200 sets `Object+0x5C (data) = ObjectBase+0xC8` when non-null. In lara.drm
  that points to DTP 850 + 0x28F80. Player init 0x721F80 copies it to instance+0x50; the exe reads every player tuning
  value as `*(DAT_00e7f094 + 0x50) + offset` (DAT_00e7f094 = player instance, set by 0x5BD080 at level start).
- Identified fields: +0x10 height 512.01, +0x14 260 (crouch height, TRLAU name [I]), +0x44 1000 (max health
  [I], health regen 0x6F5160), +0x4C/+0x50/+0x54 fall-damage heights, +0x820..+0x838 ledge-grab probes, +0xC3E..+0xC42
  water probe box (Numbers). 250 distinct offsets up to +0xFDA are read by the exe (direct `DAT_00e7f094 + 0x50`
  accesses); the rest are not mapped [U].
- [U] Jump take-off speeds, step heights and swim speeds were not found in this block; the swim loops and the air clips
  have no root motion either, so these stay inferred.

## Outfits (Lara's player object per level)

Selection rule (tru.exe 0x5DE2B0, run by the main-state level load 0x5DE7E0):

- [O] Default: the unit's settings DTP (`Level+0x70`, the render-settings block) holds at `+0x294` a u16 objectlist id;
  `name = g_objectList[id]` is the object Lara is loaded as (`pc-w\<name>.drm`). Read again in 0x5D4340 (player
  reload) when the requested object does not exist. [O] 0x5D40C0 then rewrites the unit's `player` intro (object
  id from `OBTABLE_GetObjectID("player")`, or any id in the outfit list `GlobalInfo+4`) to that object id, so the
  player instance *is* the outfit object.
- [O] Override: game-state buffer `DAT_00c59264 +0x20C` (`+0x210` when `+500` is set, a mode we do not know [U]) is
  written with the current outfit by every checkpoint save (0x555BE0) and read back by checkpoint restore (0x556E90);
  it is cleared by the reset 0x556940. If non-zero and its DRM exists (0x5BF7B0 file check), the level load keeps it,
  except that a load into `l1_seatop` forces `lara_wetsuit_warm`.
- [O] Script natives: 0x77BD50 → 0x5D9F40 sets the outfit by object name; `unlockablecostume` 0x778CA0 sets the next
  outfit from the costume record (+8) and reloads the player (0x6F0A90, also `PLAYER_DebugSwitchPlayerCharacter`).
  [U] which scripts call them; the extras/PDA costume choice is not implemented, so the default rule always applies
  outside saves.

Data (all 82 units of `unitlist.txt`, `RealData.UnitOutfitObjects`) [V-data]: one outfit per chapter.

| Units | Outfit (objectlist id) |
|---|---|
| `l3_*` (prologue, Croft Manor, `l3_mansion_exterior_afterburn`) | `lara_casual` (10) |
| `l1_*` incl. `l1_ship\*`, `l1_cine_ship_sinking` (Mediterranean) | `lara_wetsuit_warm` (19) |
| `l2_*` (Coastal Thailand) | `lara_jungle_a` (12) |
| `l4_*` (Southern Mexico) | `lara_jungle_heavy` (14) |
| `l5_*` (Jan Mayen) | `lara_snow_heavy` (16) |
| `l6_*` (Andaman Sea ship) | `lara_jungle_b` (13) |
| `l7_*` (Arctic Sea, Helheim, epilogue) | `lara_wetsuit_cold` (18) |
| `container1` | `lara` (8) |

`lara_bikini`, `lara_snow_light` and the `dlc_*` costumes are named by no unit [V-data] (costume unlocks [I]);
`lara_doppelganger` / `lara_mom_thrall` have objectlist ids but no DRM. `lara.drm` uses the same render model
(2366) as `lara_jungle_a`, so before this change chapter 2 alone looked right.

Outfit DRMs [V-data] (`AnimReal.OutfitsShareClipsAndSkeletonTopology`): each is a whole Lara object. All 1744
animation sections are byte-identical to lara.drm's; the player data DTP (lara.drm 850, e.g. casual 2643) is the
same 550112 bytes and differs only in 38 bytes of u16 resource ids. Skeletons have lara.drm's 153 bones and parents
but their own pivots (up to 13.4 units apart, `lara_wetsuit_warm`); every outfit has the 37-particle hair rig.
Wetsuits carry extra Wave sections (556 vs 493) and each outfit one extra script type (280 bytes) [U] content.

Ours: `UnitData::playerObject`; `World::enterUnit` (level load, respawn, chapter start, save load) sets
`World::outfit`; saves store it (`outfit`) and loading applies the 0x5DE2B0 override rule above. ou-game loads the
outfit's skinned meshes and textures, re-rigs the pose skeleton, hair and hand offsets from it (`rigOutfit`; clips
stay lara.drm's, identical) and adds its waves. Cutscenes pose the same meshes. Streaming into a neighbour keeps the
outfit (the original only reads `+0x294` on a level load).

## Controls

- Bindings live in `config.json` under `controls` (`bindings`: action → list of inputs; `mouse_sensitivity` 0.1..10,
  a multiplier on 0.0025 rad/pixel; `invert_y` for mouse and right stick). `ou-game --config` reads them; actions a
  file omits keep their defaults, an unknown action or input name is an error. Edit them on the launcher's
  Controllers tab (Add input… captures a key, mouse button, pad button or trigger; Esc cancels).
- Actions: forward, back, left, right, walk, sprint, jump, drop, interact, draw, fire, aim, switch, weapon, heal,
  grapple, save, load, hint, answer, pda, light, grenade. Inputs: SDL scancode names (`W`, `Space`, `Left Shift`, `F5`), `Mouse Left/Right/Middle`,
  `Pad <SDL button>` (`Pad a`, `Pad dpright`, `Pad leftshoulder`, `Pad leftstick`), `Pad lefttrigger/righttrigger`
  (pressed past half). Case-insensitive.
- Defaults: WASD/arrows, Shift walk, Alt sprint, Space jump, Ctrl/C drop, E interact, F draw, mouse left fire / right
  aim, Q switch target, X weapon, H health pack, G grapple, L flashlight, R grenade, F5/F9 quick save/load; pad South
  jump, East drop, West interact, North draw, left shoulder walk, right shoulder grapple, triggers fire/aim, right stick
  click switch, d-pad right weapon, d-pad up health pack, d-pad left flashlight, d-pad down grenade, left stick click
  sprint. [I] the original's PC default for the light seems to be T, which is our hint key.
- Fixed: left stick moves, right stick looks (2.5 rad/s at full deflection, 0.15 dead zone), Esc / Start opens the
  pause menu, F1 toggles the overlay, menu navigation (arrows / Return / Esc, d-pad / South / East).

## Controller

- Ground: each tick the current clip's root delta (rotated by heading) is the desired move. A capsule from
  `feet + stepUp` to the head is swept and slid along walls (up to 4 iterations, contact normals flattened), then a ray
  from `feet + stepUp` down `stepUp + stepDown` snaps to the floor. A floor steeper than the slope limit still counts
  when a walkable face lies within 20 units of its height 60 units to a side (`Player::standable`: floor bumps, seams;
  the l4_ragnarok_room Fall/Run flicker at tick 127 of `tests/replays/pool_to_ragnarok.txt` came from 0.6-normal
  strips 0.5 units high). Nothing under her starts a fall with the last horizontal velocity; an unsupported floor with
  normal z 0.3..0.7 starts a slide.
- Air: exact constant-acceleration step, full capsule sweep; contacts clip the velocity. Landing = a floor-like
  contact with a standable floor under the root, or such a floor within a few units below the capsule while falling.
  A capsule resting on an edge's corner with nothing under the root (walking slowly off a ledge) does not land: it
  slides off with at least 200 units/s outwards (this used to alternate Fall/Land every tick). Running landing keeps
  running if the stick is held. `land()` sets `fallHeight` (Combat's fall damage).
- Ledges: markup type 1 (`ledgegrab`) polylines. While falling, the hand anchor (rotated by heading) must pass the
  ledge height within reach of a segment, with Lara facing it. She is then placed so the anchor lies on the ledge,
  facing across it. Shimmy moves along the polyline by the shimmy clip's root motion projected on the segment, crosses
  into adjacent segments, and stops at the polyline ends. Drop falls with a 0.4 s regrab cooldown.
- Climb-up: `ledgeb_climbup_fast` starts at its request start frame (6). Lara's root is first moved so the clip's hands
  at that time lie on the ledge (x/y) and its root at the clip end lands on the ledge height (z = hand height − rise
  from the start time to the end); then the full 3-D root motion plays (no collision) and the end snaps to the floor.
  On ledge 1253 the remaining snap is 8.7 units, the rock top sitting 9 above the ledge markup (it was ~150 before).
- Turn in place: the yaw advances by the change of `ng_turn_center`'s root Z rotation each tick (7.85 rad/s looped),
  towards the stick direction, clamped to the remaining angle.
- Interpolation: position, heading, swim pitch and clip time are blended between the last two ticks.
- Crouch (drop held on the ground): `crouch_idleToCrouch` → `crouch_Idle` / `Crouch_Front` (328 units/s root
  motion), capsule height 260 (player data +0x14); releasing drop stands up only with headroom (a capsule sweep up to
  512). Drop pressed while running: `ng_roll_front` (1364 units/s root motion), then run or idle.
- Slide: down the fall line of the floor under the root (heading = downhill), accelerating at 2500 units/s² to 1400;
  a standable floor ends it through `land()`, no floor below falls, jump leaps off with the slide velocity + 1803 up.
- Balance beams (markup 14, MUD BALANCEBEAM): from the ground, the root within 30 of the line (x/y and height, within
  the line's extent) heading along it (cos ≥ 0.7) gets on with `bbeam_walk_attach`; falling through the line from above
  lands on it with `bbeam_jumpland` (any heading). On the beam the root is pinned to the polyline: the stick along the
  walking direction plays `bbeam_walk`, whose forward root motion moves her along the line (joints followed); against it
  `bbeam_walk_turn180` turns her round by its root rotation. At an end: another beam ending within 60 takes over (she
  turns onto it), a standable floor 80 beyond is stepped onto (Walk; 0.5 s before a beam can catch her again), else she
  stops. Jump = standing jump (the assist below puts the landing back on the line), drop = falls off to her right [I].
- Jump assist: at take-off the landing is pulled onto the nearest jump-landing (2) or beam (14) line point within 300 of
  the natural landing (Numbers).
- Drop to hang: drop pressed standing within 200 of a ledge line at floor height (±40) whose far side drops away plays
  `drop_to_ledgeb`; her yaw and root are set so the clip (start frame 3, half turn) ends exactly on the hang pose of the
  nearest ledge point, root motion and turn played without collision, then Hang.
- Hang jumps: jump while hanging — stick neutral/forward climbs up when a walkable top lies within 150 above the ledge,
  else jumps up to a ledge above; stick sideways jumps to a ledge to that side; stick back jumps to a ledge behind
  (turning round with `ledgeb_jump_back`'s root rotation), or with nothing in reach leaps off backwards. Targets
  (Numbers) exclude the ledge she hangs on (that is shimmying) and ledges whose hang side has floor (platform edges
  hung from the platform). Flight: ballistic with the exe gravity, rising 60 above the higher end, timed to arrive
  exactly on the target's hang pose; then `ledgeb_attach_U/S/F`.
- Poles (markup 4 / 6): grabbed like ledges while falling, squared to the pole; forward held swings
  (`HPole_StartSwing` → `HPole_swing`, root fixed), letting go stops; jump flings to the next pole ahead (same flight
  solver as hang jumps) or leaps off at run-jump speeds; drop lets go.
- Under water the model is pitched by the swim pitch about the root (`Player::modelMatrix(pos, yaw, pitch)`), and
  treading/surface swimming levels it back at turnRate.

## Water

- Volumes [O]: l4_pool has one `watervolume` intro (uid 5608, object id 7, a 316-unit editor cube at z −534.5). Its
  intro data is `{flags 0xB, 0, 0, 0, u16 +0x12 = 5607, …}`; 5607 is the `ca_goo_pool_watersurface` intro: the blue goo
  is water. The exe gives each water volume instance a type-10 markup box (0x5D7130, MUD flag 0x100000, centre + half
  extents); the volume's handler (0x6AC340) keeps bottom/surface heights (+0x50/+0x58 of its component) and moves the
  surface on script messages (0x1B4: absolute, relative to the bottom, to a marker intro, or by a delta, with a rate).
  The level markup has no type-10 items (73 items: 61 ledgegrab, 10 balancebeam, 2 jumplanding).
- Ours: `findWater` builds the box from the linked surface model's bounds: x/y of the 17 257-unit goo plane, surface
  = its top (intro z 2690.7 + model z 8.6 = **2699.3**), bottom 100 000 below [I]; how the exe sizes the box (a
  component at +0x13C) is [U]. The initial surface is the surface intro's height [I]; the scripted level changes are not
  run. `loadUnitData` appends the boxes to the unit's markup.
- In water: the water probe box overlaps a volume, the feet are below its surface and there is no floor within
  `swimDepth` (400) below the surface. From the air → SwimSplash (plunge velocity damped by drag) → Surfacing (rise at
  swim speed) → SwimTread. From the ground (standing on a deep floor) → Surfacing.
- Surface (SwimTread / SwimSurface): root on the surface; the stick swims at 562 units/s along the heading, which turns
  at turnRate; drop = SwimDive (`swim_surfacedive` root motion, 667 down); jump facing a wall whose top is 50..400
  above the water = SwimClimbOut; floor within 400 below = stand up (Idle).
- Under water (SwimUnder): stick swims horizontally, jump / drop held swim up / down; the swim direction's pitch turns
  towards the wish at turnRate and pitches the model; the clip is `swim_sub` (level stroke) at every pitch. No input:
  `swim_submerged_idle`, no drift. Reaching the surface = SwimTread. Movement is a sphere (r 100) swept around the
  root, slid along contacts and clamped to the surface.
- Climb out: a sphere sweep 300 forward must hit a wall (|n.z| ≤ 0.5, facing it); a ray 50 past the contact finds the
  top. The root is placed so the clip's hands (at its start time) reach the contact and its root ends on the top
  (`top − rise`); the clip's root motion then plays without collision and the end snaps to the floor.
- Oxygen (ours; no tru.exe number decoded yet, rates [I]): `World::tickAir` drains the meter over 45 s while the head
  is under water (SwimUnder / SwimDive / Surfacing / SwimSplash with root + 150 below the surface) and refills it in
  2 s at the surface. At 0, `damageLara(max / 6 per second, kDamageBypass)` until Lara surfaces or dies. ou-game draws
  an AIR bar whenever the meter is not full. Air is not saved (a load starts at full health/air).
- Interactive surface (`src/game/Water.{hpp,cpp}`, `World::water()`; technique and parameters [I], TRU does not
  simulate ripples): a deterministic 256×256 damped wave-equation height field (28-unit cells, wave speed 420 units/s,
  damping 0.994/tick) rides the surface Lara is in or nearest, follows her in whole cells and resets when the surface
  changes. Impulses: entry splash (strength from the plunge speed), surface-swim wake and stroke pulses, tread rings,
  near-surface under-water swimming, wading (feet below the surface but not swimming), climb-out drips, living enemies
  and moving collision dynamics crossing the surface, and this tick's hitscan rays crossing it (`CombatState::shots`).
  `World::splashWater` is the entry point for other impacts. Splash droplets are ballistic (gravity as Tuning, drag by
  lifetime, capped at 256, xorshift RNG) and die on the surface or after 0.35–0.75 s. ou-game copies the field, water
  volumes and droplets into `FrameOptions` for the Enhanced water shader (docs/RENDERING.md "Water"); Original water
  does not use them. Tests `Game.Water*`; real-data l4_pool: entry rings, swim wake, AIR bar and drowning verified by
  replay/screenshot (2026-10-27).

## Free climbing (src/game/Climb.cpp)

- Climbable rock = the free-climb holds the level places [O]: markup types 16-20 (MUD 0x8000 plus one of
  0x4000/0x2000/0x1000/0x800/0x400), 3-point triangles ~150-250 across lying 20-35 units off the rock,
  100-250 apart along a route. tru.exe disables 0x8000 markup unless it has exactly 3 points (0x5D6F00),
  accepts it as an attach candidate after a vector test (0x6FC6B0, a 0x49D270/0x4898E0 result < 0.2; operands
  [U]) and places Lara at point 0
  − 158.025 z (0x707C60). Only 8 units have any (l1_baldrsstairs_a, l1_ship_sinking_hall, l2_cliffclimb,
  l2_norsehub, l2_puzzleroom, l4_temple, l4_traprooms, l7_yggdrasil_end; COLLISION.md markup survey).
  Before 2026-10-29 we treated faces with flags bits 0x01..0x20 as climbable; that byte is the
  `adjacencyFlags` of TRLAU's IndexedFace and is set on ~99.5-100 % of all walls, so Lara could climb
  every wall (survey of l3_main_bedroom, l4_pool, l4_ballcourt, l2_hubhall, l3_main_hall, l5_startsnow).
- Our rule [I]: a nearly vertical terrain face (|normal.z| ≤ 0.35, not dynamics) is climbable where a hold's
  centroid lies within 350 of Lara's chest (root + 300) and within 120 of the face's plane (not a hold behind
  a thin wall). The original positions Lara per hold [O, 0x707C60] and so presumably moves hold to hold [I]; we
  move freely inside the band the holds cover. Which hold type selects which `DICE_*` clip set is [U].
- Attach: pushing into such a wall from the ground (chest probe at +300, reach = capsule radius + 90,
  facing it within ~53°) or falling onto it (after ledge/pole grabs, which win). `DICE_B_ground_attach` /
  `DICE_B_air_attach_M` plays, then `DICE_B_Idle`.
- Climbing: the stick's into-wall component climbs up (away = down), its along-wall component moves
  sideways at 240 units/s [I]; the settle clips (`DICE_B_Settle_up/_down/_Left/_right`) have no root
  motion [O], so the controller moves the root and pins it at capsule distance from the rock, following
  the face (and around corners) with a back probe at chest height. Movement stops where no hold is in reach.
- Top out: when no hold is in reach and a standable floor above Lara's feet is within reach past the wall,
  Lara steps onto it (Idle). Otherwise movement stops at the edge. Jump = `DICE_B_Jump_Back` + a leap away
  (700 out, 1400 up [I]); drop = `DICE_B_Drop` and fall. Cooldown 0.4 s before re-attaching.
- Tests `Game.ClimbAttachClimbUpTopOut`, `Game.ClimbJumpOff`, `Game.ClimbNeedsHolds`, `Game.ClimbStaysWithinHolds`,
  `GameReal.ClimbOnlyAtHolds` (12 reachable walls ≥ 1000 from any hold in each of l3_main_bedroom, l4_pool,
  l4_ballcourt, l2_hubhall, l2_cliffclimb, l1_baldrsstairs_a: 0 attach, the old rule attached 10-12 of 12;
  l2_cliffclimb hold 910 ground attach + climb up; l1_baldrsstairs_a air attach at hold 44). Hand test:
  `ou-game --level 'pc-w\l2_cliffclimb.drm' --spawn '-23835,-3600,300,90'` and hold forward (climbs);
  `ou-game --level 'pc-w\l3_main_bedroom.drm'` and run into any wall (no climb).

## Grapple (src/game/Grapple.cpp)

- Targets [O]: intros named `*_ei_grapple_*` (`ca_ei_grapple_post`, `sa_ei_grapple_ceiling(_invisible)`,
  `ma_ei_grapple_ceiling`, `ca_ei_grapple_wall_a`, `tp_ei_grapple_wall`, `ar_ei_grapple_wall`, …), collected per
  unit into `UnitData::grapples` (kind by name: wall/post/ceiling/mounted). l4_pool's ceiling anchor is uid 5511
  at (4362, -70, 2607).
- Selection (ours [I]): within 300..5000 units of Lara's eye (+375), inside a 60-degree cone of the camera,
  clear line of sight (`World::grappleTarget`). Best by angle then distance; a crosshair marks it in ou-game.
- G / pad right shoulder (`Actions::grapple`) throws (`grapple_rappel_free_jumpon`) and attaches; G again or
  Ctrl/C releases. Space jumps off with the swing's momentum (`grapple_rappel_free_jumpoff`).
- Swing (ours [I], no exe numbers): gravity + stick pumping (500 u/s²), a rope constraint at hand = root + 400
  (outward radial velocity removed), light air drag. `grapple_rappel_free_swing_forward/backward` by travel
  direction, `grapple_rappel_idle` otherwise. E ascends the rope, Shift descends (300 u/s, clamped 200..6000).
- The rope is drawn from the hand to the anchor in ou-game. Tests `Game.GrappleSwingAndRelease`,
  `GameReal.PoolGrappleTargetAndSwing` (anchor selected and attached).
- Not yet: wall-running/rappel on walls, pulling mounted objects, rope-vs-world collision, hook model,
  throwing while hanging from ledges. Rope length/physics are [I].

## Ladders, pillars, perches (src/game/Traverse.cpp)

- Markup [O]: ladders (9) and pillars (15) have points `{top, bottom, bottom + outward hint}`; perches (13) are a
  short ledge segment. `climbNormal` holds the outward direction (reused field).
- Ladders/pillars: attach from the ground (push into them) or the air; stick into = up at 240 u/s [I],
  `lad_climbup/_down`, `lad_Idle`; top = `lad_ClimbOffTop` onto the top point, bottom = `lad_ClimbOffBot`;
  Space = `lad_detach` (out 500, up 900), Ctrl = `lad_detach_drop`. Pillars also rotate around the shaft with
  the stick's sideways component. l1_ship_sinking_hall ladder 234 is climbed in a real-data test.
- Perches: from the air or a standing jump next to the segment (`perch_attach_air/_ground`), balance on the
  point with `perch_idle`, Space = `perch_jump_forward`, Ctrl = `perch_detach_drop`.
- Tests `Game.LadderAttachClimbOffTop`, `Game.PillarAttach`, `Game.PerchAttachAndJumpOff`,
  `GameReal.ShipSinkingHallLadder`. Object-DRM markup does not exist in this data (COLLISION.md), so all
  traversal markup comes from the units.

## Rail slides (src/game/Traverse.cpp)

- Type-8 markup attaches from the ground or air after ledges, poles, ladders, perches and rock faces. Lara follows the
  polyline downhill with tangential gravity; Space jumps with rail momentum, Ctrl/C drops, and the endpoint enters a
  normal fall with a short regrab cooldown. Attach tolerances, detach behavior and reuse of `slope_slidefront` are [I].
- `GameReal.ValaskjalfRailSlideTraversesAndDetaches` traverses all of rail 1562 and checks the low endpoint. A rendered
  `ou-game` run also landed at `7269.1,-76525.5,-13337.3` with `ng_generic_land_forwards` [O].

## Motorcycle (src/game/Vehicle.*)

Lara mounts the bike with E (interact) next to its left side, rides it with the move stick (forward = throttle, back =
brake then reverse, left/right = steer; raw stick, not camera-relative), hops with Space and gets off with E once it is
almost stopped. While she is on it, `World::tick` skips `Player::tick`, drives her from the bike and masks move, jump,
drop, use, weapons and the grapple; the camera pulls back and swings behind the moving bike. No new bindable actions.

Evidence:

- Intros [V-data] (survey of every unit DRM against objectlist.txt): `motorcycle` (object 540) in `l4_trailstart` 5482
  at (38051, -51966, 1463) rot z 0.93, `l5_startsnow` 7528, `l5_valgrind_tunnel` 7229; `revisit_motorcycle_spawner`
  (657) in `l4_trailstart` 6699 and `l5_valgrind_tunnel` 6715; `co_motorcycle` (641, a cinematic copy) in
  `l4_ballcourt` 6403 and `l7_thin_ice` 9078. No Thailand (l2) unit places a bike. `UnitData::vehicles` keeps the
  `motorcycle` intros; spawners and cinematic copies are not instantiated [U: the spawner's script for revisits].
- Classes [V-exe] RTTI `Bike : Vehicle` (vtables 0x886064 / 0x8860E4), `VehicleCamera`, `NtMotor_ScriptType`; state-chart
  conditions `PSCCF_Vehicle_SteerDirection/StuntDirection/IdlePose/DismountClearance/State`. 0x6AB960 creates the Bike
  (0x790 bytes, ctor 0x78EDA0) when the instance's vehicle data (+0x50 = ObjectBase +0xC8) has `+8 == 1`, with the tuning
  record `+0x10`; 0x793680 is its per-frame update (throttle, steering, gear/torque 0x791D10, Havok wheel forces 0x792900,
  fall damage).
- Vehicle data [V-data] motorcycle.drm ObjectBase +0xC8 (section 13582 +0x1160): `u16 +4 = 0xB00B`, class 0x40,
  `+8 = 1` (Bike), four 0x20C-byte tuning records at +0xB0 (count +0x6C), default `+0x10` = record 2.
- Speed unit [V-exe]: 0x105B30C = 0.1071429 (3/28) * 2.237 is applied to the body velocity (0x795560): the record's
  speeds are mph with 280 game units per metre (Lara's 512-unit height ≈ 1.83 m).
- Riding clips [V-data] motorcycle.drm's animation list (Lara's 113-segment skeleton): `motorcycle_mount_cycle`,
  `_dismount_cycle`, `_idle_foot_down*`, `_ride_slow/_fast`, `_accelerate(_loop)`, `_braking`, `_hard_brake`,
  `_lean_left/right`, `_reverse_left/right`, `_jump`, `_jump_inair`, `_jump_land`, `_air_tilt_left/right`,
  `_power_skid_*`, `_donut_*`, `_wheelie`, `_crash_brace_*`, `_fall_to_death`, `_jump_dismount(_leapover)`,
  `_mount_cycle_jump`, `_dismount_front/back`, PDA equip; plus 1-segment `vehicle*Anim` clips. Riding clips have no root
  motion: Lara's root is the bike's origin. `motorcycle_mount_cycle` ends with her root at (180.1, -124.9) turned +90°,
  so she starts at bike-model (124.9, 180.1) — the bike's left, behind the seat — facing it; `motorcycle_dismount_cycle`
  ends at (158.3, 52.1) turned +90°. Their root yaw passes ±180° (axis-angle sign flip): Vehicle.cpp unwraps the keys.
- Weapons on the bike [V-data]: `wp_handgun`/`wp_uzi` carry `combat_pistol_motorcycle_*` (arm, disarm, reload,
  switch hands, a left-side aim grid straight/front/side/back × level/up/down) and lara.drm `hit_motorcycle_*` hit
  reactions. Not implemented: weapons are holstered on mount and stay holstered [I].
- Skeleton [V-data] 19 bones: front wheel 5 (axle (0, -206.6, 104.1)), rear wheel 8 ((0, 256.8, 98.8)), fork/bars 3-4,
  swingarm 7, frame 17. ou-game spins 5 and 8 about model X and turns 3 about the steering head axis [I].
- Collision: every l4 unit's faces use mask 3 like Lara's; no bike-only faces or markup were found [U how the original
  keeps the bike on its paths beyond terrain].
- Music: `l4_laras_new_bike` is named by script 13918 (in every l4 unit and `revisit_motorcycle`); ou-game starts it on
  the first mount in an l4 unit [I trigger].

Numbers (default tuning record):

| Value | Ours | Source |
|---|---|---|
| top speed | 40 mph = 5008 units/s (asymptote of the throttle curve) | record +0x00 [V-data]; mph per 0x105B30C [V-exe] |
| front wheel limit | 30° at low speed → 10° at top speed, blended over [0.3, 1] × top speed | +0x10 / +0x14 / +0x1C, 0x793680 [V-exe] |
| steering rate | 95°/s → 75°/s, same blend | +0x18 / +0x1B4, 0x793680 [V-exe] |
| heading | bicycle model `v / wheelbase · tan(steer)`, wheelbase 463.4 | [I]; wheelbase from bones 5/8 [V-data] |
| fall damage | drop ≥ 3792.67: (drop−3792.67)/(4819.85−3792.67)·95; > 4819.85: 95 + clamp(…/(4977.88−4819.85))·1550; in the air ≥ 4977.88: 1650 (lethal), type 8 | +0x184..+0x198, 0x793680 [V-exe] |
| acceleration / brake / coast | 2400 · throttle · (1 − (v/top)²) / 5000 / 600 units/s² | [I] (original: gears +0x54.., rpm 1000/10000/18000 +0x78.., torque 30/20/20 +0x90.., Havok wheels) |
| reverse | 700 units/s, 900 units/s² | [I] |
| hop | 1100 units/s up | [I] |
| gravity | 5.72443 units per 30 Hz frame² (Lara's) | [V-exe] Lara, [I] for the bike |
| terrain | two wheel probes (axles) set height and pitch; > 43° up stops it; no ground under a wheel = airborne with the slope velocity; walls: axle capsule sweep + body-sphere push-out at 200 units up (rolls up ~120-unit steps) | [I] |
| mount / dismount | E within 250 units of the mount spot; off below 200 units/s | [I] |
| ramming | enemies touched at > 800 units/s take 0.5 health per unit/s, the bike loses 30 % speed, one hit per contact | [I] |
| camera | 950 units back (700 on foot), yaw follows the heading at up to 3/s × min(1, v/1500) | [I] (`VehicleCamera` data not decoded) |

Saves: `vehicles` (docs/SAVES.md) keeps every bike (unit, unit-frame position, heading, pitch, speed, steer, air state,
phase) and the ridden one; Lara's state `vehicle` + clip are in `player`.

Tests: `VehicleReal.BikeIntrosTuningAndClips`, `VehicleReal.TrailstartRideReplay` (replay `tests/replays/bike_ride.txt`:
mount, 4429 units/s top speed down the trail, left curve up the steps, hop, brake, dismount; bike parked 8 300 units
away, no damage), `VehicleReal.RamsEnemyOnTheTrail`, `VehicleReal.SaveRestoreMidRide`.

Verification [O] (ou-game, Vulkan, 1280×720, the replay with `--spawn 37981,-51758,1463,-126.7`; screenshots viewed,
kept privately): frame 40 Lara swings her leg over the bike's left side (mount clip), 200 leaning into the left curve
towards the steps, 262 in the hop clip over the stones at the top, 420 stepping off beside the bike parked nose-down on
the slope; wheels and Lara stay on the bike through slope pitch. One OpenGL frame (200) matched.

Not done [U/I]: gears, engine/skid/landing sounds, wheelies, power slides, donuts, crash braces and crashes, the jump
dismounts, PDA on the bike, shooting from the bike, hit reactions while riding (damage applies without a clip), dismount
clearance checks (`PSCCF_Vehicle_DismountClearance`), the revisit spawner, water.

## Enemies

Code: `src/game/ai` (`Enemy`, `Enemies`, `loadEnemyType`), ticked by `World::tick` after the script runtime; each enemy
registers in `World::targets` (Combat's `Damageable`) and hurts Lara through `World::damageLara`. ou-game draws them
CPU-skinned (model 0) instead of their static intro meshes; `--show-enemies` draws capsules (colour = mode), heading
and line of sight; the overlay shows the nearest enemy; mode changes (with the clip) and blows are logged to stderr.
Enemy shots (`Enemies::shots`, this tick's) are logged (`enemy N shot hits Lara / misses from … to …`) and drawn as
0.2 s tracers (yellow = hit Lara, white = miss / stopped by a wall).

### Survey (intros per unit) [O]

- Mexico: `cat_panther` ×13 (ballcourt 4, temple 5, templepath 1, trailstart 3), `panther_thrall` ×1 (l4_pool 5645),
  `mayan_thrall_pool_generator` ×13 (l4_pool), `poacher` ×12 / `poacher_long_range` ×4 (ballcourt, temple),
  `bat_enemy` ×5 (temple 5250..5254, z 1043) + `small_albino_spider` ×22 (temple), `giant_albino_tarantula` ×13 +
  `small_albino_spider_generator` ×9 (traprooms), `giant_tarantula_generator` ×3 (staircase_xibalba). Thailand: `tiger`
  ×8, spiders, 15 bats. Sharks: `shark_blueshark` (l1_seafloor 980..985, l2_cliffclimb 4020..4023), `shark_greenland`
  (l7_arctic_sea 9125..9133).
- Enemy objects: ObjectBase oflags2 (+4) bit 0x400000 selects the "enemy" object family (tru.exe 0x5BF0E0 → family
  table 0x8ABA28 entry 14: init 0x63C190 (via 0x63C530), message 0x6164D0 (via 0x6187C0)). Sharks are in it too.
- Generators: ObjectBase +0xDE u16 = spawned object (mayan_thrall_pool_generator → 600 `mayan_thrall`, others → bats,
  spiders, tarantulas).

### Enemy data (ObjectBase +0xC8, the instance+0x50 block 0x63C190 reads)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | magic 0xDAF00018 | [O] 0x63C190 bails without it |
| +0x04 | class: 1 bats/sharks, 3 cats, 4 soldiers, 7 spiders, 8 thralls | [O] values; meaning [I] |
| +0x0C | byte: behaviour-tree vtable choice (0x8ACF90..A8): 0 soldiers, 1 cats, 2 bats + sharks (flyer: EnemyState_FlyerMove RTTI), 3 thralls | [O] 0x63C190, values |
| +0x20 | component list head (next +0x1C); each field below is read from the first node where it is non-null | [O] getters 0x614400..0x614940 |
| +0x2C | 94.8 (0.3 m) on all types | [O] value; [I] body radius |
| +0x3C, +0xA0 | 1264 (4 m), 60° | [O] used as a vertical tolerance (0x6497C0, 0x67B610) and a tilt limit (0x620500) |
| node +0x20 | weapon `{u16 +2 object, s16 +8, f +0x10}`: poacher/long_range 114 `wp_akrifle`, −75, 100 | [O] 0x614400 → 0x614950 attaches it |
| node +0x30 / +0x34 / +0x38 | senses: default / mode 2 / combat (layout below) | [O] 0x614490/0x6144C0/0x6144F0, modes FUN_0063F7F0 |
| node +0x3C | health block {easy, normal, hard, +0xC} | [O] 0x620CC0 → 0x6E77B0 by difficulty 0x6E7710 (≤ 0 → normal) into comp+0x2C64 |
| node +0x44 / +0x4C | pointers to sleep / investigate senses | [O] |
| node +0x64 | death record (below) | [O] 0x614610, EnemyState_Death 0x66C500 |
| node +0x68..+0x9C | attack records (below) | [O] 0x642D80, EnemyState_Fire 0x666D10/0x666EB0 |
| node +0xE4 | `"Sidestep"` behaviour parameters `{300, ptr, 948.2/790.1, 60/100, …}` — not senses | [O] FUN_00623000 |

Clip refs in the records are `{s16 type, s16 idx}`: type 1 → ObjectBase +0x60 table (count +0x5C, 0x24-byte entries),
entry +4 → s16 animation-list index (FUN_00626F10, FUN_004325d0) [O]; type 2 refs (anim requests) are not resolved [U].

Senses record [O] (EnemySense 0x664E80 → 0x664B20): seen = inside the eye cone (s16 range/yaw°/pitch° at +0x10/+0x12/
+0x14, each falling back to +4/+6/+8 when negative; FUN_0064CF10) **or** a body cone (+4/+6/+8, +0xA/+0xC/+0xE;
0x60DBA0), then a line-of-sight ray (0x493330, mask 0x42C13AB) unless byte +0x44 is set. +0x20 = lose-track distance
(2528), +0x24 awareness falloff [I], +0x3A..+0x40 [U]. Combat enter (0x610430) switches to the combat record, exit
(0x610470) back.

| Type, record | Eye cone | Body cone A | Body cone B | No ray |
|---|---|---|---|---|
| poacher default / combat | 5720 / 45° / 30° | 5689 / 10° / 25° | 3792 / 180° / 30° (combat 45°) | no |
| poacher_long_range default / combat | 6637 / 6321, 45° / 30° | 9481 / 7901, 10° / 30° | 3792 / 180° | no |
| cat_panther (tiger eye 15802) | 11061 / 120° / 60° | 9481 / 60° / 30° | 2370 / 180° / 90° | yes |
| bat_enemy default / combat | 6321 / 4740, 180° / 90° | 3792 / 4740, 180° / 90° | — | yes |
| shark_blueshark default | 11378 / 180° / 180° | 6321 / 180° / 90° | 2370 / 180° / 90° | yes |

Attack record [O]: clip refs +0x78 (blow), +0x7C (per-shot), +0x84 (follow-up); damage +0x154 × 0x6460C0 [I ≈ 1] with
type +0x158 (second hit +0x164/+0x168, unused by our types); s16 +0x13E shots per burst, +0x144 gap, +0x13A start delay
(enemy-clock counts, unit not found: [I] 1/30 s); +0x34 cooldown ([I] ms); byte +0x1F0 maximum yaw (°) before turning.
Poacher: +0x68 fire 75 × 3, gap 5, delay 30, 7000, `combat_stand_burstfire_2` → `reload2`, yaw 10°; +0x6C (type-2 shot
ref); +0x70 fire 75 × 3, gap 5, delay 5, 8000, `combat_crouch_burstfire` → `reload2`; melee +0x80 `kick_forward`,
+0x84 `rifle_bash`, +0x88 `rifle_bash_b`, 100 type 1. Panther: +0x80/+0x84 `simple_attack_90_right` 100 type 1,
+0x88 `simple_attack_static1` **50** type 1, +0x8C `simple_attack_static2` 100, +0x90 `simple_attack_moveto` 125
type 2, +0x94 `lunge_attack_forward` 125 type 2, +0x98 `stun_attack2` 100 type 2, +0x9C `attack_overcover_high` 150
type 2. Bat: +0x80 `attach_bite_loop` 75 type 0, cooldown 10000. Thralls: none. Sharks: none.

Death record [O] (node +0x64; poacher and panther identical apart from the clips): timer s16 +0x1C = 120, +0x46,
+0x4C impulse; count +0xC8 of 0x14-byte entries at +0xCC `{clip ref, ?, ?, category, flag}`. FUN_00642210 takes the
attacker's angle relative to the heading: category 1 front (±45°), 3 left, 5 back, 7 right; falls back to front/back
by ±90°, then front; picks randomly among matching entries with the requested flag (FUN_00642170) and turns the heading
for 3/5/7 (a − 90°, a + 180°, a + 90°). Poacher: 3/7 `death_shot_3`, 5 `death_shot_back`, 1 a type-2 ref, flag 3
`death_placed_shot`, flag 7 `hosted_enemy_vault`. Panther: 5 `hitreact_thrown_forward`, 1 a type-2 ref, flag 3
`death_adren`, flag 5 `death_running`, flag 7 `hitreact_thrown_forward`. Bat: 1 `death`. After the timer the exe
hands the body to physics (0x696F90 budget check → impulse 0x696A60) [I: ragdoll].

Normal hit points [O]: cat_panther / panther_thrall 700, tiger / poacher / giant_albino_tarantula / viking_thrall /
shark_blueshark 500, merc 450, mayan_thrall 350, shark_greenland 800, yeti_thrall 1100, bat / small spiders 2.
Distances in the data are multiples of 31.6056 (316.056 units = 1 m, consistent with the hair constants).

### Behaviour (ours) and numbers

Modes idle → alert (0.4 s, turn) → chase → attack (one blow in the hit window) / fire → reload → chase; hit
reaction; death; spawning (generator clip). Perception uses the decoded senses (the combat record once out of idle):
eye or body cone from the eye (0.8 h; [I] body cones are measured from the eye too) to Lara's chest (0.75 of 512),
then the ray unless the record says no ray (cats, bats, sharks see through walls inside their cones). Unseen Lara
closer than the lose-track distance is still tracked [I]; chasing enemies that reach her last seen spot wait there;
4 s unseen [I] → idle. Clips by role, first present name: idle `passive_idle`/`idle_passive`/`idle`, alert
`attack_idle`/`idle_aggresive`/`idle_combat_stand`, walk `walk_forward_2`/`walk`/`combat_walk_forward`/
`bat_fly_passive`, run `run_forward_2`/`run`/`combat_run_forward`/`bat_fly_aggressive`, attack
`simple_attack_static1`/`rifle_bash`/`attack_bite`/`simple_attack`/`slash_attack`/`slash`, hit
`hitreact_medium_front`/…, death `death_static`/`death_shot`/`death`/`damage_front_death`, spawn `spawn_from_pool`;
fire, reload and directional death clips come from the records. Ground movement: capsule collide-and-slide, ground
snap 70 up / 140 down (a floor inside the lower half of the body lifts it: ballcourt poacher 6541's intro is 96 under
its floor [O]) [I], gravity, ledge avoidance (no step leaving > 300 of drop ahead); never closer to Lara than both
radii. Panther `run_forward_2` root speed 2323 units/s [O].

- Soldiers (armed: node +0x20 weapon, a fire record with a resolved shot clip; poacher, poacher_long_range, merc):
  seeing Lara inside the combat eye range (poacher 5720) they stand (`idle_combat_stand`), turn to her and, within the
  record's max yaw, enter fire: wait the start delay, start the per-shot clip and fire the burst's shots `gap` apart;
  then the follow-up clip (`reload2`) and the record's cooldown, rotating through the fire records (poacher +0x68,
  +0x70). Each shot is a ray from the muzzle (eye height, one radius ahead) [I] at Lara's chest; [I] 35 % of shots
  (per-enemy LCG seeded by uid) aim 1.5–3 of her radii aside; it stops at the first wall (line of fire: no damage) or
  Lara's capsule (cylinder), which takes rec+0x154 (75, light). Lara out of range or out of sight: they run
  (`combat_run_forward`) to where she was; within 474 [I]: the melee blow (`rifle_bash`, 100). No cover: the
  EnemyBehavior_*Cover RTTI classes and `cover_*` clips exist, cover markup was not looked for [U]. The rifle
  (`wp_akrifle`) is not drawn: model 0 has no gun, the skeleton's bones are unnamed and the hand bone was not found [U].
- Flyers (behaviour 2): bats hover where placed (root motion dropped), fly in 3D (sphere of the body radius, slid along
  walls) at 700 units/s [I] towards Lara's head (0.85 of her height) until within 0.8 × (radius + 150); bite
  (`attack_bite`) within radius + 150, hitting within radius + 250, damage of the bat record (75) once per 1.5 s [I]
  (the latch `attach_bite_loop` and its 10 s cooldown are not used); dead: `death` then gravity to the floor.
  EnemyState_FlyerMove's numbers were not read.
- Death clip as FUN_00642210 (above) with flag 3 on a head shot (`Hit::headshot`), 5 when hit while running, else 0
  [I]; first matching entry instead of a random one; type-2 entries are skipped, so a plain front kill plays the role
  clip (`death_static`, `death_shot`). No ragdoll: the clip's last frame is held. Clips whose last frame has its median
  bone under a third of the bind pose's median count as lying (panther `death_static` 37 vs 306, `death_running` 47,
  `death_adren` 53; poacher `death_shot` 55 vs 473, `death_shot_3` 40, `death_shot_back` 69; not `death_placed_shot`
  178) [O measured]; others tip onto their side over 0.5 s about the forward axis at the feet [I] (flyers do not).

| Quantity | Value | Provenance |
|---|---|---|
| Hit points | data (above) | [O] |
| Radius / height | +0x2C / highest bind-pose bone (panther 344.6, poacher 596.2) | [O] / [I] |
| Sight | senses records (above); thralls have none: eye 3160.6 (10 m) ±60°, 790.1 (2.5 m) all round | [O] / thralls [I] |
| Attack start / reach | 474 / 632; flyers radius + 150 / + 250 | [I] |
| Blow damage | the record whose clip we play: panther 50 (`simple_attack_static1`), poacher 100 (`rifle_bash`), bat 75; type 2 → heavy, else light | [O]; thralls 120 [I] |
| Hit window | 35–60 % of the attack clip, 0.5 s cooldown (bats 1.5 s); the records' +0x34 cooldowns are used for fire only, as we play one melee attack | [I] |
| Shots | rec+0x154 per hit; shots/gap/start delay from the record in 1/30 s; cooldown rec+0x34 ms | [O] values; units [I] |
| Stagger | after `block +0xC` damage (panther 80, thralls 0 = every hit) | [I] |
| Generator trigger | Lara within 2528 (8 m), ±2000 z, once | [I] the real trigger is scripted/EnemySpawnData 0x614FB0 [U] |

Checkpoints and streaming: `Enemies::checkpoint()` snapshots the dead uids when a new checkpoint is touched;
`World::respawn` → `Enemies::reset()`: enemies dead at the checkpoint stay dead, the others are back at their placement
with full health (`Enemy::start()`), generator spawns are removed and their generators re-armed [I: tru.exe restores
the checkpoint save; the restore path was not traced]. Generator spawns are stashed with their generator's uid when
their unit streams out and re-spawned when it streams back in.

Sharks [O]: enemy family, class 1, behaviour 2 (the bats' flyer tree), health 500 / 800, senses above, death record
with four type-2 entries, no attack records (bite damage stays at the inferred 120 default [I]); clips
`swim_forward_slow/medium/fast`, `burst_attack`, `bite_2` (blueshark), `death` (8 s), `death_float_loop`. They spawn,
chase with 3D clip-driven swimming and sphere collision, bite, and die into the floating loop. Type-10 boxes confine
swimming when present; the real shark units expose none, so their live behavior uses collision-based free swimming.
`AiReal.SharksSpawnSwimBiteAndDie` verifies populations and shark 4020 chasing, biting and dying.

Not done: behaviour trees (search, flank, cover, taunt, dodge, jump/vault, nap), merc / long-range specifics beyond
their records, rifle mesh, bat latch, wall-hung spiders (they drop), navmesh/routing, enemy sounds, ragdoll.

Verification [O] (ou-game, GL, 1792×1008, ASan debug build; screenshots viewed, kept privately): replay
`tests/replays/poacher_firefight.txt` (l4_ballcourt, `--spawn 17520,10383,2650,180`): poachers 6540/6541 alert at tick
1, fire at tick 26, first shots at tick 87 (start delay 60 ticks), bursts of 3 ten ticks apart, hits and misses
logged, `reload2` at tick 127; frame 99 shows the yellow (hit) and white (miss) tracers from the poacher on the temple
stairs to Lara while panthers 6531/6533 attack (50 per blow). Replay `tests/replays/bat_attack.txt` (l4_temple,
`--spawn -5096,-10909,1800,-90`): the five bats alert at tick 1 and fly up from z 1043 towards the platform at 1725
(with Lara drawing at once, her pistols killed all five by tick 117 as they climbed; the delayed-draw version that
lets them bite was not rendered). Death poses, the bat bite in a real unit and the poacher bursts are covered by
`AiReal.*` instead of screenshots; the panther's `death_static` end pose was not re-viewed.

## Combat (src/game/Combat.*)

Health [O] (player globals `DAT_00fadb58`: health +0x628, regen clock +0x630/+0x634, invulnerability +0x638, last
damage type +0x63C):
- Max health = Lara data +0x44 (1000) × generalbank difficulty table (object data +0x24, 0x18-byte entries: Easy
  1.3 / Normal 1.0 / Hard 0.8 at +0x08; damage multiplier 0.5 / 1.0 / 1.3 at +0x0C; ours: Normal) [O values, I labels].
- Damage `0x6F5030(amount, type, msg)`: scaled by the damage multiplier unless type 8 (absolute); after a hit Lara is
  invulnerable for data+0x60 = 30 frames, during which only a type of higher priority (table `DAT_0085de28`
  {0,1,2,4,3,5,4,4,5,4,4}) or type 5 hurts. Enemy hit message 0x11C: types 1 light / 2 heavy. Message 4 kills
  (type 6, amount = health). Fall damage (above) bypasses the window and records type 8.
- Regeneration `0x6F5160`: below data+0x70 = 25 % of max, 45 frames (+0x78) after the last damage, health rises over
  400 frames (+0x74) along `0x6E79D0`: base + cap·((1−e)t + e·t³), e = +0x7C = 0.7, capped at 25 %. Above max
  (overheal, only from the reward path 0x5DB6A0 `heal(…, 1)`) it decays from data+0x80 = 1.4 × max to max over +0x84 =
  900 frames.
- Health packs [O] (g_healthpack data; use = gadget table 0x8AFC70 entry 2 "Healthpack", fnC 0x6CBB20, on the input
  message 0x800003F): only with the cooldown at 0, a pack carried (count s16) and Lara alive; cooldown = data+0xEA = 60
  frames (2 s [I 30 Hz]); count − 1; message 5 → `heal(data+0xF0 = 640, 0)` = 0x6F4EF0, clamped to max (no overheal).
  The exe does not check full health: a pack used at full health is spent. Ours: `Actions::heal` (H, pad d-pad up:
  locals 1291 "Press U+1808 to use a health pack", U+1808 = d-pad up [I TR7 button order]; script `heal`); the carried
  count is `World::found[2]` (picked-up `ca_rw_shard` packs, saved) and drops by one per use. Lara has no use clip
  [O]: g_healthpack.drm's `use` (1.03 s) animates the pack object, so nothing plays on her. Max carried 100 (+0xE2..E6)
  and +1 per pickup (+0xE8) are not enforced (Progression counts pickups).
- Death [I]: at health ≤ 0 on the ground, `death_fall_normal` (fall) or `death_toknee` (other damage) plays; 1 s
  after it ends World::respawn (last checkpoint, full health as `0x700060`). The weapons, clips and reserves are kept.
- Hit reactions: the exe turns an accepted enemy hit (message 0x11C → 0x719DD0 → 0x6F5030) into a reaction by type
  (0x6EB590: type 1 light → state value +0x204, type 2 heavy → +0x208) and writes the attacker's angle from Lara's heading
  into the anim graph (0x6EB250; mirror flag 0x20 when positive), which picks the clip; the graph's thresholds are
  [U]. Ours (`World::damageLara` → `Player::hitReact`, state `Hit`, saved as "hit"): only when `Health::damage`
  accepted the hit and Lara is on the ground (idle/walk/run/stop/turn/land/hit) or swimming; light = `hit_light_{front,
  back,side}`, heavy = `hit_medium{front,back,right}` (lara.drm, 0.33 / 0.70 s [O]); front within ±45° of her heading,
  back beyond ±135°, else side [I]; no mirroring: the side / right clip plays for both sides; swimming the `*_swim`
  variants. The clip plays once with its root motion (ground: collide-and-slide, water: the swim sphere), then idle /
  tread or submerged idle. Guns stay drawn and fire during it [I].

Weapons [O] (object data = ObjectBase+0xC8; stats block S = data+0xE0, read by fire `0x6C9E50`; `loadWeapon`, values
= `weaponDefaults`, checked by `CombatReal.WeaponData`): Lara carries the pistols plus one secondary (data+0xDC: 0
pistols, 1 uzi/shotgun/speargun/rifle/tranq, 2 Thor's hammer; a new secondary replaces the old one).

| | pistols (wp_handgun) | uzi | shotgun | speargun |
|---|---|---|---|---|
| ammo object (S+6), mode | pr_handgunbullet, hitscan | pr_uzibullet, hitscan | pr_shotgunbullet, hitscan | pr_spear, projectile (2) |
| damage at 0 / short / max range | 40 / 30 / 20 | 25 / 25 / 10 | 50 / 50 / 40 per pellet | 250 [I] |
| short / max range (S+0x20 / +0x18) | 4000 / 6500 | 2000 / 4500 | 2000 / 3000 | 2500 / 5000 |
| clip (S+0xB), dual (S+0x88 0x20) | 12 per gun, dual | 40 per gun, dual | 6 | 1 |
| pellets (S+5), spread / max (S+0x1C / +0x2C) | 1, 1° / 1° | 1, 1° / 5° | 7, 15° / 15° | 1, 1° / 1° |
| spread per shot / recovery (S+0x30 / +0x34) | 0 / 0 | 1° / 0.1° per frame [I unit] | 0 / 0 | 0 / 0 |
| ammo slot (S+4), max reserve | none (S+0x88 0x80: infinite) | 0, 240 | 1, 72 | 4, 25 |
| refire [I] | `combat_pistol_[RL]fireE` 10 keys, hands alternate | the uzi's own `fire` clip, 5 keys, alternate | `combat_shotgun_fire_*` 13 + `pump` 25 keys | 13 keys, then reload |
| equip / unequip / reload | 6 / 19 / 21 keys | 15 / 15 / 21 (wp_uzi's own) | 24 / 24 / 117 | 24 / 24 / 31; under water 17 / 19 / 31 |

- Max reserve: Lara data u16 tables at +0x868 / +0x872 / +0x87C (slot × 2; uzi, shotgun, rifle, tranq, spear =
  400/90/300/25/40, 160/42/120/10/15, 240/72/210/15/25) picked by `0x3efa0` = 0 / 2 / other (0x6EEAE0) [O]; we use
  +0x87C, taking `other` (1) to be Normal [I]. Live counts sit at `fadb58+0x51C` + 2·slot [O]. Reloads move rounds
  from the reserve into each gun's clip; the pistols' reserve is infinite; a secondary out of rounds and reserve falls
  back to the pistols [I].
- The speargun's pr_spear record is a projectile (mode 2, 0x6C95F0 → 0x6C9300; its fields +0x34 = 250, +0x38 = 100,
  +0x50 = 10000, +0x54 = 45 are not decoded): we fire it as hitscan with 250 damage [I].
- Spread: per pellet, yaw and pitch offsets within ±spread/2 (0x69E300) from a 32-bit LCG (deterministic replays);
  grows per shot to the max, recovers per frame, resets on draw (0x6E6C60).
- Switching (`Actions::weapon`: X, pad d-pad right [I U+180A per locals 1298]; script `weapon`): pistols ↔ the secondary
  (exe current / previous weapon at inventory +0x134 / +0x138 [O] = `CombatState::weapon` / `previous`); the weapon in
  hand plays its unequip clip, then the other its equip clip; each keeps its own clip and reserve. Under water only the
  speargun can be drawn and fired (the others holster): which weapons the exe allows submerged is [U] (every wp_* has
  `fire_submerged` / `combat_water_submerged_equip`, the speargun alone has submerged reload clips).
- Pickups [O]: no ammo objects exist (all `pu_*` are puzzle items); ammo comes from picking up a weapon (0x70E530 →
  0x6F5E70: the intro's property 0x55 added to the slot, capped by the max reserve). A survey of every unit's intros
  finds no uzi / shotgun / speargun / grenade pickups (only `wp_akrifle` in l1, `wp_handgun` in l3_main_hall,
  `wp_thorhammer` and `wp_natla_fireball(_adren)` in l7_yggdrasil_end, `co_wp_*` cinematic props): the secondaries
  come from scripts / enemy drops not decoded [U]. `ou-game --weapon uzi|shotgun|speargun` gives one with its max
  reserve for testing (`World::giveWeapon`).
- Grenades: see "Grenades" below.

Upper body (subtree of spine segment 14; Lara's weapon clips live in each wp_* DRM and are kept as
`"<object>:<clip>"` because the DRMs reuse names): the aim grid is blended bilinearly by the aim yaw/pitch. Pistols /
uzi: 3×3 `combat_pistol_poseA..I` (each DRM's own), columns measured from the shoulder→hand directions +109° / 0 /
−104°, rows +40° / 0 / −57°; the hand that fired adds its `combat_pistol_[RL]fireE` motion relative to its first
frame. Shotgun / speargun: 3×5 `combat_shotgun_aim_{U,,D}{R,FR,F,FL,L}`, columns measured from the barrel line (rear
left hand 55 → front right hand 80) relative to `_F`: −90° / −47° / 0 / +58° / +90°, rows +45° / −39° (row means;
`_UFR` is a copy of `_FR` in the data); for 13 keys after a shot the matching `combat_shotgun_fire_*` cells replace the
aim cells, then the shotgun adds `combat_shotgun_pump`. The reload clip replaces the layer while reloading. Drawing /
holstering play the weapon's equip / unequip clip while `drawT` ramps over its length, blended into the aim pose over
the last / first 30 % of the ramp, the layer fading in / out over 0.1 s at the holstered end [I]; the gun moves
between holster and hand half way through [I]. Mounts [I, fitted on screenshots]: pistols / uzi on hand bones 80/55,
pistols in thigh holsters 9/4 (the uzi is not shown out of hand); shotgun / speargun with the grip in the left hand
along the line to the right hand, on the back (spine 14) when holstered.

Targeting [O data, I use] (Lara data +0x480 / +0x470, criteria records {u16 type, u16 mode, f threshold, axis}):
hard lock (aim held) tries tiers in order: nearest within 1264 (3D), then the smallest camera angle among targets
within 8000 and ≤ 90° from the camera axis, then within 8000 and ≤ 90° from Lara's facing; soft target (drawn,
aim not held): nearest within 5000 (2D) and 27.5° of her facing. LOS from 375 above the feet (+0x48C) is required.
Switch target cycles through the hard-lock order [I]. Locked and standing, Lara turns toward the target [I]. No
target: free aim along the camera ray.

Adrenaline [O strings, I numbers]: locals 1310 "Build up adrenaline for combat by damaging enemies", 1333–1335 "When
the adrenaline meter is full … hold LT to lock on, get close, and press X … Steer the pip into the target reticle and
then press RT for a Headshot". The world time scale `_DAT_00e7f1a0` (setter 0x4A3DC0; eased by the script natives
0x776030 / 0x78C4E0) multiplies `gameTracker_timeMult`, and Lara's update divides it back out (0x622418), so she runs
at real time while the world slows [O]. The fill rate, the slow-motion factor, the window length and the pip
behaviour are not in the data we decoded [U]. Ours: every hit fills the meter by damage / 1000 [I]; full meter + hard
lock + target within 1264 of her eye (the close lock tier [I]) + interact press opens a 3 s window (real time) [I]:
`CombatState::timeScale` eases to 0.2 over 0.25 s [I] (enemies tick with dt·timeScale), Lara stands still, the pip
starts 2.5 reticle radii off in a random (LCG) direction and the move stick steers it at 2 radii/s [I]; fire with
the pip inside the reticle hits with `Hit::headshot` and damage = the target's health; any fire press or the window's
end spends the meter and time eases back.

Input: F draw/holster (fire also draws), LMB fire, RMB aim/lock, Q switch target, X switch weapon, H health pack, E
adrenaline; pad Y / RT / LT / R3 / d-pad right / d-pad up / X; input scripts `draw`, `fire`, `aim`, `switch`, `weapon`,
`heal`, `interact`. HUD (ou-game): the original display groups and reticles (see **HUD** under Progression; F1 adds a
text line with health, packs, adrenaline, weapon clip / reserve, carried secondary); muzzle flash, tracer and impact lines;
shot / reload waves per weapon (`HG1_shot_c_*`, `MP5_shot_*`, `SG1_shot_*`, `speargun_shot_b_*`; HG1 magazine,
`SG1_reload`, `speargun_cock_a_*`). `--target X,Y,Z` places a 700-hp dummy capsule.

Tests (`tests/test_combat.cpp`): `Combat.HealthPacks` (640 heal, clamp, 2 s cooldown, count, none left),
`WeaponSwitchKeepsRoundsAndReloadsFromReserve` (uzi given with a capped reserve, holster → switch → draw, per-weapon
clips, reload of 80 rounds from 100 spare), `ShotgunPelletsInTheCone` (7 pellets within ±7.5°, next shot after fire +
pump), `HitReactionPlaysAndReturns` (front / back heavy / side clips, invulnerable re-hit ignored, root motion, back to
idle, guns stay out), `AdrenalineHeadShot` (meter from 1000 damage, window, time scale 0.2, pip steered in, kill, time
scale restored), `ReplayWithSwitchingIndependentOfRenderRate` (pistols → shotgun → pistols + a health pack, 30/60/144 Hz
bit-identical); `CombatReal.WeaponData` (all four weapons decode to `weaponDefaults`, every weapon clip loads, clip
lengths). Viewed (l4_pool, GL, 1792×1008, 2026-10-18; openunderworld-work/screens/combatextra): `--weapon uzi` dual uzis
on the hands firing at a `--target` dummy (pistols in the thigh holsters), `--weapon shotgun` firing 7 tracers into the
dummy from the hip hold, and a health pack picked up after an 1800-unit drop (517 hp) and used: 1000 hp, packs 1 → 0.

## Flashlight (src/game/Gadgets.cpp)

- Gadget "Personal Light" (g_personal_light): its object effect list (DTP at object section +8, +0x3C count, +0x40 →
  0x24-byte records, light DTP id at +0x20; tru.exe 0x546710 → 0x5C35D0) holds two lights [O]: a spot (type 1, reach
  5000, intensity 2, cone 1.02 / 1.80) and an omni (type 0, reach 1800, intensity 0.6), both white with the DTPs'
  falloff curves (`formats::objectLights`, `lightFromDtp`).
- Toggle (`Actions::light`: L / pad d-pad left; script `light`) on the press edge, then a 4-frame cooldown (data +0xEA)
  [O]. No battery: drain / recharge frames (+0xF0 / +0xF2) are −1, which 0x6B1810 skips [O]. Saved (`flashlight`).
- Spot or not [O]: the unit's render settings (Level +0x70 DTP +0x2B4 → byte +0x0A, tru.exe 0x5D3FF0 → flag 0xED00AA,
  read by 0x6B17C0) turn on the spot ("Both"); 38 units set it (l4_pool among them), elsewhere (l3_main_bedroom) only
  the omni lights up (`UnitData::flashlightSpot`).
- Placement [I]: chest bone 15, 25 units forward along the torso, pointing where the torso faces. ou-game reserves the
  first environment light slots for spot, omni and a grenade flash and moves them every frame
  (`FrameOptions::dynamicLights`, GL + Vulkan, original-shader path only; the generic shader ignores point / spot
  lights). Toggling lights the HUD d-pad's flashlight item.

## Grenades (src/game/Gadgets.cpp)

- Data [O values, I meanings where marked]: g_grenade_thrower (gadget slot 4) max carried +0xE2/+0xE4/+0xE6 = 4 / 6 /
  10 (Easy / Normal / Hard; we use Normal), cooldown +0xEA = 20 frames, launch vertical speed +0x10C = 632.1 and
  gravity +0x110 = 4741 [I], throw range +0x134 / +0x138 = 200..1000 [I]; projectile +0xF0 = pr_grenade, whose explosion
  record (+0x44 = 2) gives damage 1000 at the centre (+0x48), radius 1100 (+0x4C), 200 at the edge (+0x50), knock
  speeds 300 / 100 (+0x58 / +0x5C) and +0x80 = 120 frames (the fuse once stuck [I]) — `loadGrenadeTuning`.
- Throw (`Actions::grenade`: R / pad d-pad down; script `grenade`; locals 1309 "Press U+1806 to throw a sticky
  grenade"): from the ground states, at the locked target's aim point or along the camera ray, range clamped to
  200..1000. Lara turns to it and plays g_grenade_thrower's `combat_grenade_Throw1` over the upper body; the grenade
  leaves the right hand at 45 % of the clip [I] on an arc with the fixed vertical speed that lands on the point [I].
- Flight: gravity; it sticks to the first surface or living target it hits (and rides on the target), then the fuse
  runs; one that never sticks goes off 3 s after the throw [I].
- Blast (area damage after 0x69F1E0 [O]): every target and Lara within 1100, damage linear 1000 → 200 from the centre
  to the edge, no occlusion test; physics bodies get the knock speed away from the centre [I as velocity].
- Count: Lara starts with 6 [I] (no unit has grenade pickups; +1 per pickup in the original); saved (`grenades`). HUD:
  grenade row (81 full / 82 empty at y 52) under the clip pips, throwing lights the d-pad grenade item. ou-game draws
  live grenades with pr_grenade's mesh (5 at once, as tru.exe tracks 5 attached ones) and an orange flash light [I]
  for 0.3 s at each explosion.

## Tests

- `Game.*` (synthetic collision + synthetic clips with the measured root speeds): loop-seam root motion, wall slide,
  jump apex/air time vs. analytic, running off an edge, 50-unit step up/down vs. 200-unit block, ledge grab + hand
  placement + shimmy across a polyline joint + drop, input-script parsing, and the same script rendered at 30/60/144 Hz
  giving bit-identical state. Water (`poolMesh`: walkways, 3000-deep pit, surface −150): run in, splash, tread on the
  surface, swim across, climb out onto the far walkway exactly at its height; dive, swim down (pitch < −1.2), no drift
  when idle, swim up to the surface at swim speed; a swim script at 30/60/144 Hz is bit-identical. Turning in place
  follows a synthetic turn clip's root rotation (π − 0.2 rad in 0.375 s, not the 6 rad/s fallback).
- Bugs: `WalkingSlowlyOffEdgeFallsOnce` (one Fall, one Land, Land only on the ground below) and
  `RunsOverSteepSliverWithoutFalling` (a 20-wide 0.6-normal strip: never Fall/Slide) both fail with the previous
  landing/floor rules; `UnderwaterModelFollowsSwimPitch` (model forward = (cos pitch · heading, sin pitch), interpolated
  pitch, levelled at the surface). Traversal (synthetic geometry and clips): `BalanceBeamWalkTurnStepOff` (on, 3 s at
  the clip speed pinned to the line, turn round, step off onto the far platform), `FallingOntoBeamCatchesIt`
  (fallHeight 400), `AssistedJumpLandsOnJumpLandingMarkup` (lands at the markup ± 2 vs. 446 unassisted),
  `DropToHangFromEdge` (synthetic `drop_to_ledgeb` with translation + half turn: no step > 25 units, hands exactly on
  the ledge, facing the wall), `HangJumpsUpRightAndBack` (three targeted jumps, hands exactly on each target, no snap),
  `PoleGrabSwingFlingDrop`, `TraversalIndependentOfRenderRate` (pole grab/fling/drop script bit-identical at
  30/60/144 Hz), `CrouchUnderSlabAndRoll` (crawls under a 350-high slab, stays crouched under it, stands past it; roll
  root motion), `SlidesDownSteepSlope` (normal z 0.45: slides > 1000 units/s, stops on the flat).
- `GameReal.*` (gated by `OU_GAME_DIR`): the clips load, `ng_run` root speed 1274 ± 5 units/s, hand anchor; l4_pool:
  `'player'` spawn, 1.5 s run replay never below the floor and covers 50–105 % of the same replay on a flat floor; jump
  from the lower floor grabs ledge 1253 (hands at z 3562) and climbs onto the rock top (z 3571 ± 1, end snap < 10).
  `PoolSwimReplay`: `findWater` gives one box (id 5608, surface 2699.3 ± 0.5); from the west walkway (−6000, 0, 2844.4)
  Lara runs into the goo, swims east 2 s (> 80 % of 2 s × swim speed), dives below surface − 600, swims up to tread,
  swims back west and climbs out (720 ticks) onto a pool rock at (−4190, −183, 2864.9), standing on its floor.
- Traversal, real data: `TempleBalanceBeamWalkAndJump` (l4_temple beam 1079: on it from a spawn on the beam, 5 s walk
  within ±1 of the line, a standing jump lands back on it 300+ ahead, then walks to its far end x −3986 and stops);
  `TempleLedgeJumpsUpAndBack` (from the floor: grab 1187, jump up to 1162; grab 1158, jump back to 1159 turning round;
  hands on the targets ± 1); `PoolSideJump` (1250 → 1251, set up hanging via `World::restore`); `PoolDropToHang` (grab
  and climb 1253, drop back into the hang, hands at 3562 ± 1); `TrapRoomsPoleFling` (l4_traprooms: standing jump onto
  pole 1336 from 345 short, swing, fling 1264 to pole 1337).
- Gadgets (`tests/test_combat.cpp`): `Gadgets.FlashlightToggleCooldownAndPose` (press edge, 4-frame cooldown, light
  25 units ahead facing forward), `Gadgets.GrenadeKillsDummyAndHurtsLara` (thrown at a locked dummy 800 ahead: sticks
  to it, goes off after the fuse, kills it, Lara takes 1000..0), `GadgetsReal.TuningLightsAndSpotUnits` (decoded
  grenade data, personal light spot 5000 / omni 1800, spot on in l4_pool and off in l3_main_bedroom); replay
  `tests/replays/pool_gadgets.txt` (l4_pool: light on, grenade thrown and exploding).

## Verification (viewed screenshots, 2026-10-18) [O]

All on `pc-w\l4_pool.drm`, 1792×1008, GL unless noted:

- standing at the `player` intro on the entrance stairs, facing the pool hall (ng_idle);
- running down the stairs mid-stride (replay `0 forward 1`, frame 80); the same in Vulkan at frame 100;
- mid running jump, legs tucked (`NG_InAir_Feet_Forward_stage1`, frame 92);
- hanging braced on rock ledge 1253 with both hands on its top edge (`--spawn -4291,4225.6,3100,-157.9`, jump);
- shimmying right along it; standing on the rock top after the climb-up;
- swimming at the goo surface (`swim_surface`, body horizontal, root at z 2699; replay `0 forward 1`, `--spawn
  -6000,0,2900,0`, frame 230), rising after the splash (only the hair bun above the goo, frame 150), and swimming down
  under the goo between the submerged pillars (`swim_sub_down`, z 1822, frame 330, drop held from 4.0 s).

Traversal (`ou-game --input-script … --frames N --screenshot`, GL, 1792×1008, viewed; scripts and PNGs kept privately):

- balance beam, l4_temple beam 1079 (`--spawn -5500,-8099,3100,0`, `0 forward 0.3`): walking along it arms out
  (`bbeam_walk`, frame 200, pos y −8099 z 3061); standing on it seen from the side (`bbeam_idle`, camera turned); a
  standing jump along it in the air above the line (frame 256, y −8099);
- ledge to ledge, l4_temple: mid-air between ledges 1187 and 1162 on the stone wall (`ledgeb_jump_up`, `--spawn
  -8771.2,-9922.7,2600,0`, frame 108) and hanging on the upper ledge with both hands on it (frame 170); a back jump
  from 1158, turned round mid-air (frame 112) and hanging on the pillar ledge 1159 behind (frame 200);
- drop to hang, l4_pool rock 1253: dropping off the top edge (`drop_to_ledgeb` t 0.28, frame 205) and hanging on it
  (frame 280);
- poles, l4_traprooms (`--spawn 14300,-21200,6400,90`): hanging from pole 1336, swinging with the legs over the bar
  (`HPole_StartSwing`, frame 280), mid-fling between the poles (frame 345) and hanging from pole 1337 (frame 400);
- crouch-walking (`Crouch_Front`) and rolling (`ng_roll_front` t 0.33) at the l4_pool intro; sliding down the l4_temple
  slope at (−2103, −10850) (`slope_slidefront`, frame 22);
- swimming down at a pitch under the goo, the model tilted head-down (`swim_sub`, z 1310, camera above, frame 330).

No side-by-side comparison with the original game was made.

## Deviations and open questions

- Jump velocities, step limits, swim speed, camera distance/height, all traversal reach boxes and the jump-assist radius
  are inferred; they are not in Lara's player data block (Numbers) [U]. The grab zVel window, height, crouch height and
  water probe are taken from it.
- Swimming: oxygen/drowning are implemented with inferred rates (45 s / refill 2 s / max-per-6 s; no exe numbers).
  Still missing: wading clip (`swim_wading`), surface turn clips, underwater fast/sprint, swim damage from other
  sources; the water surface is static (scripted level changes not run); the water box is sized from the surface model,
  not the exe's component. Ripple/caustic parameters are ours [I].
- Free climbing is implemented on the levels' free-climb holds (markup 16-20, see "Free climbing"): free movement
  inside the hold band instead of hold-to-hold, hold-type semantics [U], movement speed/reach [I], no
  bad-attach/wrestle/corner-specific clips, no `DICE_L/R/LW/RW` wall
  variants. Ladders (9), perches (13), pillars (15) and rail slides (8) work (see their sections); vertical poles
  (type 5) have no markup in any unit. Still missing: cover, `Wall_Hug_*`,
  balance wobble, beam sprint/catch, pole shimmy/parallel hang, `ledgeu_*` free hang, hurdles/vaults,
  mirrored left-side clips. Root motion of the jump, attach and fling clips is not applied (the flight is
  ballistic), so their poses can sit off the computed path.
- Grapple is implemented for ceiling/post/wall targets (throw, pendulum swing, rope ascend/descend, jump off;
  see "Grapple"). Not done: wall-running/rappel, pulling mounted objects, rope-vs-world collision, the hook
  model, ledge-grapple transitions, exe rappel speeds (ours are [I]).
- Missing: sprint, state-chart conditions (`PlayerSC_*`, `PSCCF_*`); combat: grenade crouch / perch / under-water
  placing, the
  assault rifle / tranquilizer / Thor's hammer, projectile flight for the speargun (hitscan), mirrored side hit
  reactions, thrown / knock-down reactions (`Hit_Thrown*`, `hitReact_*`, types 7/9/10/12/15), hit reactions while
  hanging/crouching, weapon and ammo pickups (none of uzi / shotgun / speargun in any intro), aiming from ledges /
  poles / ladders (the `combat_pistol_ledgeb_*`, `_hpole_*` … sets), the adrenaline numbers (fill, slow factor,
  window) and the exe's anim-graph hit-reaction thresholds. Saves do not store weapons, clips or reserves.
- Collision: no depenetration pass (an already-touching contact being left is ignored); query mask 0xFF.
- Live keyboard/mouse/gamepad play was not exercised by any recorded run: every screenshot is a scripted replay.
- Hair is rigid; editor proxy meshes (yellow/red boxes) are drawn (LEVELS.md).

## Progression: chapters, level ends, pickups, menus, music

Code: `src/game/Progress.{hpp,cpp}` (tables, `scriptStrings`), `World::startChapter/collectPickups/checkLevelEnd`
(`Stream.cpp`), `src/ui/Ui.{hpp,cpp}` (menus, PDA page, HUD as `FrameOptions::ui` quads), `ou-game` (`src/app/game/main.cpp`).
Tests `tests/test_progress.cpp`; replays `tests/replays/menu_new_game.txt`, `pool_pickup.txt`.

**Chapter sequence** [O]: `pc-w\generalbank.drm` script section 6 lists the chapter start units at 0xF920..0xF9C0:
`l3_main_bedroom` (prologue), `l1_seatop`, `l2_cliffclimb`, `l3_caverns_start`, `l4_trailstart`, `l5_startsnow`,
`l6_ship_exterior`, `l7_thin_ice`, `l7_himalaya_end`; generalbank DTP 2 holds `chapter0` + `l3_main_bedroom` (New Game).
The cinstream headers place the prologue cinematics (001_pro_corridor, 010_pro_surprise) in l3_main_bedroom / l3_main_hall
and 030_med_yacht in l1_seatop [O]. `FUN_005de2b0` special-cases `l1_seatop` (wetsuit) [O]. Chapter names: locals.bin
52..58 in that order [I mapping]; prologue = 54 CROFT MANOR [I]; "ONE WEEK EARLIER..." (1307) is shown under the chapter-1
title after the prologue [I placement]. New Game: frontend state 8 "PlayNewGameMovie" plays `explode.bik`
(`0x5DB110`, state names `0x77CB20`) [O], then chapter 0. The original main menu plays `medsea_loop.bik` behind it [O];
ours now decodes and loops that movie through either renderer's post pass. Its audio track is not mixed yet [U].

**Level ends** [O data, I trigger]: the script of exactly one `cinematic_object` per chapter end names the next chapter's
start unit: l3_main_hall 4106 (prologue → 1), l1_cine_ship_sinking 1005, l2_cliffclimb 2565, l3_mansion_exterior_afterburn
4070, l4_ballcourt 5483, l5_valaskjalf 6846, l6_ship_natla_room 1387. l3_main_hall's script also names
`l3_caverns_start`, so the target is the lowest-numbered start unit after the current chapter. Trigger volumes link to
these cinematics (l2 2564, l3_main_hall 4129, l4 5468/6407, l5 6866/6872); in our runtime the Activate of such a
cinematic plays its cutscene (its `cinematic` member names the stream, docs/formats/CINEMATICS.md) and then starts the
next chapter. The l1/l6/afterburn cinematics are reached through other scripts: l1_ship_sinking_hall plane 1074 /
volume 1082 → 1005, l6_ship_engine_hall plane 7565 → 1387, and l3_main_hall volume 4128 → cinematic 4108, whose
post-stream helper plays 4070 [O bytecode]; all run natively (docs/formats/SCRIPTS.md "Survey"). `--chapter N` starts
any chapter, `--cine NAME` plays any cutscene.

**Pickups** [O names/counts, I roles]: `*_rw_artifact` (7..22 per chapter) = treasures, `*_rw_treasure` (one per
chapter) = relics, `*_rw_shard` = health packs (base script 3924 pushes "g_healthpack" and uses the health/invslot
natives). All share base script 3674 (playersensor, "popUpTitleMessageChannel", hint_14). Ours: interact press within
200 units horizontally / 250 vertically of the reward intro picks up the nearest one [I]; it joins `World::collected`,
`World::found` counts it, the mesh is hidden and the HUD shows `TREASURES|RELICS|HEALTH PACKS` (1251/1252/1274) with the
count [I format]. Saved (docs/SAVES.md). Health packs are not yet usable (Combat).

**Menus** [O layout/art/strings from tru.exe captures, I where marked]: the main menu is the original's
`ScriptMainMenu` screen (cdc script UI `uimainmenu`, generalbank.drm script 8; the scripts themselves are not
executed). Reference: tru.exe under Wine+DXVK (4 CPUs: its `CPUCount` 0x4A21D0 overflows the stack with 32) captured at
1024×768 4:3 and 1920×1080 16:9 (openunderworld-work/screens/menu, screens/ui). Everything is in the 640×480 UI space at
the window height's scale, centred horizontally (16:9 keeps the 4:3 layout in the middle). Art = generalbank.drm textures
plus g_pda.drm 680–705 packed into one atlas (`ui::Art`, `FrameOptions::uiImages[0]`); full-screen backgrounds are
`pc-w\image\*.raw` (`ui::loadRaw`: `!WAR`, u32 header size 0x80, u32 pixel bytes, 3, 0x10, width, height, row bytes;
B,G,R,A pixels) in `uiImages[1]` (the menu movie in the main menu, else the PDA or loading background). Font = DTP 46
(header and 12-byte glyph table over textures 109/110, advance `(spacing + width) · scale − 1` as `Font_GetTextWidth`
0x4758C0; text scale 0.625 × the font's 0.5; glyphs sampled with a half-texel inset); text below is placed by its cap
top (cell top + 4.52·scale, cap height 14·scale). Locals placeholders U+182A/182B/182C/1803/1801 draw as the key names
Return/Escape/BackSpace/space/E in (99, 209, 247).
- Radial ring: texture 17 glyph art 672² at (317.5, 242.25), ¾ opaque; texture 50 quarter ring 208² mirrored four times
  around the centre (320.3, 240.3); ring radius 136.8, 7 items from −90° in steps of 180°/7; discs (15) and icons
  (18–26) at 0.67 per texel, unselected icons tinted (50, 115, 185); the selected item: black disc 51 (×1.17 on the ring,
  ×1 in the centre), white icon (×1.125 ring), glow 11 (×1.22 ring / ×1.17 centre, 0.7 opaque), label fading in over
  ~0.5 s on bar 12 (centre, above) or 13 (ring, to the left, ink ending 53.6 left of the item centre); footer lines
  (texture 105, 686 wide at y 432 / 450.5) and prompt 120. The movie `medsea_loop.bik` is fitted to the window width,
  centred, at ~0.66 brightness. Items: centre 123 PLAY GAME (loads the newest save, else a new game), ring 124 START NEW
  GAME (PLAYER TAILORING first), 125 GAME OPTIONS, 126 LOAD GAME, 127 SAVE GAME (main menu: message 161 "only … in
  gameplay mode" in box 106 with OK 383), 128 AUDIO/VISUAL, 129 EXTRAS (not ported: message 456 "Unavailable"), 131
  QUIT GAME. Navigation as tru.exe: Left from the centre to the last ring item (first time SAVE GAME, the one level with
  the centre), Right back to the centre, Up/Down along the ring and off either end to the centre.
- Pause [O]: the same ring with 122 RESUME GAME in the centre over the game dimmed to ~0.56 (HUD hidden); SAVE GAME opens
  the Save screen, QUIT GAME returns to the main menu [I: no confirmation 494]. No PDA item: the `pda` action (key 1,
  replays `pda`) opens the PDA.
- LOAD / SAVE GAME [O empty slots]: the ring stays (the opened item: black disc ×1.17 with a white circle r 29.8, 1.3
  wide; the centre label stays) and a mirrored ring (items 2..7 mirrored about x 320.3): page up 27, four slots, page
  down 28, icons (114, 124, 165); empty slot 29, used slot 30 [I: filled slots were not captured], selection as on the
  ring with the save name on the mirrored bar 13. Right/Accept enter the slots, Left leaves; Accept loads / saves
  (an empty slot gets `save-<date>`, a used one is overwritten), Backspace / pad North deletes (11 / 9); footer string
  121 split at its spaces, centres 193.6 / 433.3.
- List screens (GAME OPTIONS 490/491, AUDIO/VISUAL 497/491, VOLUME SETTINGS 486/497) over the glyph art and ring shadow:
  icon (20 / 23 / 32, 2/3 texel, white) centred (191, 88), white divider x 218 y 73.3–104, title 1 white at x 226.7 cap
  75.1 scale 0.95, title 2 (110, 165, 200) at x 262.2 cap 91.6 scale 0.67; white line 105 409 wide at y 110 and under
  the rows; black panel solid ±125 fading to ±181; rows cap 127.6 + 24·i (fewer than five rows: +8.4), scale 0.6, at
  x 152.4 (VOLUME 164.4), selected white, others (129, 130, 131); values right-aligned at 498.7 (GAME OPTIONS) / 492.6;
  bottom line 25.7 under the last row, at least at 246.7; selection bar (23, 69, 97) 15.5 tall, solid ±107.5 fading to
  ±217.5; sliders: frame (5, 50, 80), fill (185, 230, 255), x 322.7–461.3, 7 tall, arrows 113/114 at 307.6 / 475.6 on
  the selected row; footer 120 / 375 at 271.6 / 379. Rows and what they drive: COMBAT RETICLES (HUD reticle), HELPER
  BUTTONS and TRAINING TEXT (stored only: ours has no helper buttons/training text toggles), GAMEPAD / CONTROL
  CONFIGURATION and DISPLAY MODE / SETTINGS (message 456: the launcher configures them), VIEW AXIS INVERSION (ON/OFF
  inline, mouse/stick Y [I: the original opens a 501–504 sub-screen, not captured]), PLAYER TAILORING; LANGUAGE (ENGLISH,
  fixed), SUBTITLES ON / OFF, VOLUME SETTINGS, BRIGHTNESS (inline slider, graphics brightness 0.5·2^(v/5) [I: the
  original opens a screen, not captured]); SFX / IN-GAME MUSIC / CINEMATIC VOLUME (mixer buses; cutscene audio).
  Settings last for the session (not written to the config).
- PLAYER TAILORING [O]: box 106 stretched to 672×237 at y 216 over the backdrop alone; title 484 cap 117.8 scale 0.8,
  line under it (y 139.6, 190 wide); 481/482/483 centred at 176.9 + 28.9·i, scale 0.66, selected white on a (30, 90, 127)
  bar (solid ±100, fading to ±182), others (203, 203, 203); footer line y 287 and prompts at 266.7 / 373.3 (scale 0.76).
  Picking starts the game from START NEW GAME, or only sets the preset from GAME OPTIONS; the preset changes nothing
  in ours yet (ammunition/damage/grab-time tailoring is not ported).
- PDA [O]: `pda_bg_1280` (`_960` when the window is narrower than 1.556:1) fitted to the window height and centred; in
  bg pixels (virtual = bg·2/3 − 106.7, bg·2/3, the same for both widths): items 1074 SONAR MAP, 1075 AREA INFO, 1076,
  1077, 1078, 1081 / 1082 greyed (91, 91, 91, skipped), 122 RESUME GAME, right-aligned at 502.7, caps 173.3 + 43.3·i,
  scale 0.605, a line 17.4 below each from x 251.3 ((46, 86, 121); selected 2 px (68, 130, 184)); the selected item in
  (76, 139, 193) with arrow 114. Picture window (g_pda texture, tab bar included) at (544.7, 166) 1 px/texel, tab label
  right-aligned at 994 cap 360.7 scale 0.57: SONAR 688 + prompt 1085 (Accept: 1093 "unavailable"); AREA INFO 680 +
  chapter (680–687) and RELICS / TREASURES found/total at caps 442.7 / 478.7 scale 0.8 (x 602.7, values to 963.3) in a
  black box from 405.3 (line (16, 60, 66)) to 525; WEAPON SELECTION 689 + 1086, INVENTORY 690 + 1087 (Accept: 456);
  FIELD ASSISTANCE two boxes 155–344 / 357–544 with 1091 (Accept: hint) / 1092 (E / pad West: answer); prompts scale
  0.63, wrapped at 300, centred on the pane; footer 375 at x 289.8 and 120 at 621.3 (not on AREA INFO), cap 600. Totals
  come from `game::chapterRewards` [O survey of every l<N>_ unit's reward intros]: 10/22/7/10/10/0/19 treasures and one
  relic for chapters 1–5 and 7; found = those ids in `World::collected`.
- Loading screen [O]: `loading_bg_<area>` (640×360) covering the window, black 0.69 outside the band y 88.9–390.2 and
  0.08 inside, white 0.3 band edges 2.2 tall, emblem 5 (345², 0.35); title 51 + chapter cap 220.4 scale 1.17, line 105
  at 242.4, story 59 + chapter at scale 0.66 from cap 256.9 every 13.15, wrapped at 540. Image per chapter [I by area]:
  mansion, medsea, thailand, dungeon (Croft Manor), mexico, snow (Jan Mayen), ship, arcticsea (and the epilogue). Shown
  for one frame before New Game, PLAY GAME / LOAD (the save's chapter) and after a level end's cinematic (chapter + 1).
Keys: arrows/WASD, Enter/Space accept, E = Alt (accept; PDA TASK), Esc back, Backspace/Delete delete; pad d-pad,
South accept, West Alt, North delete, East/Start back. Replays drive menus with `forward`/`right` ±1, `jump` (accept),
`interact` (Alt), `drop`/`pause` (back), `pda`; the script clock keeps running in menus. Not reproduced: the attract
mode (intro.bik after ~1 min idle), title music, confirmation dialogs (154–160, 494), the decorative small text under
the list panels, the PDA's sonar map / weapon / inventory / journal sub-screens, filled save slot details, and the
footer always names the keyboard key.

**Original UI reference (2026-10-28)** [O captures; implemented: HUD below, menus/PDA/loading above]: tru.exe captures at 1920×1080 (and
HUD at 1024×768) in openunderworld-work/screens/ui (`hud_weapon_*`, `hud_dpad_*`, `pause_*`, `game_options_1080`,
`av_1080`, `av_volume_1080`, `load_game_1080`, `save_game_1080`, `panel_player_tailoring_1080`, `pda_*_1080`,
`loading_1080`). Under Wine the paused game applies key presses several seconds late; wait ~4 s between keys.
- Default keys [O]: R draws weapons (shows the HEALTH and WEAPONS groups), Q/F show the GEAR d-pad, 1 opens the PDA,
  Esc pauses.
- Pause [O]: the main menu's ring (124 START NEW GAME, 125, 126, 127, 128, 129, 131 QUIT GAME) with 122 RESUME GAME in
  the centre; no PDA item. New Game first opens a PLAYER TAILORING list panel (CASUAL EXPLORER / TOMB RAIDER / MASTER
  SURVIVALIST). LOAD/SAVE GAME keep the ring and add a mirrored ring on the right: arrow 27, four slot discs (29 empty
  page), arrow 28; the opened item gets a white circle; footer `Return SELECT` left, `BackSpace DELETE` right.
- HUD [O]: implemented, see **HUD** below (positions refitted against these captures).
- List screens (GAME OPTIONS, AUDIO/VISUAL, VOLUME SETTINGS), 4:3-centred virtual: icon (2/3 texel) centred
  (191, 88), white divider x 220 y 73–104, title line 1 white at x 226.7 (font scale ~0.95), line 2 (110,165,200) at
  x 262 (~0.67); white line 105 at y 110, centre 320, width 409; black panel solid ±125 v fading to ±181 v; rows 24 v
  apart, labels scale ~0.57 at x 152–165, selected white, others (129,130,131); selection bar (23,69,97) 15.5 v tall,
  centred, ~435 v wide with ~110 v fades; slider frame (5,50,80), fill (185,230,255), x 322.7–461.3, 7 v tall.
- PDA [O]: pc-w\image\pda_bg_1280 (960 for 4:3, screen shifted −162 px) fitted to the window height; left items
  1074/1075/1076/1077/1078 then 1081/1082 greyed and RESUME GAME, right-aligned at bg x 504, from y 178.7 every 43.3;
  picture windows = g_pda.drm textures 680 + chapter (AREA INFO), 688 sonar, 689 weapons, 690 inventory, at bg
  (544, 166) at 1 px/texel; RELICS/TREASURES `found/total` at bg y 450/485.
- Loading screen [O]: pc-w\image\loading_bg_<area> full screen, darkened to ~0.31 outside the band and ~0.92 inside
  (band edges at y 200/878 of 1080), chapter title (51 prologue, 52–58) at font scale ~1.2 centred at y ~220 v, the
  description (59–66) below at ~0.45 in 13 v lines. `!WAR` pixels are B,G,R,A bytes (the PDA bezel is blue only when
  decoded so).
- Subtitles [V-exe, TEXT.md §4]: game font scale 0.6 at virtual y 352/374. Pickup toasts, "Save Successful", chapter
  title animation and reticles were not captured [U] (a 2026-10-28 retry: the paused opening cutscene ignored menu keys).

**HUD** (`src/ui/Hud.cpp`, `ui::Hud`; fed by `ui::HudState` from `CombatView::hudState` each tick) [O captures, V fits,
I where marked]. Space: origin (5 % W, 5 % H) for the top groups and (5 % W, 95 % H) for the d-pad, 1 unit = H/480, art
2/3 unit per texel; ultrawide keeps the groups inside the 5 % margin. Positions are top-left in units, fitted by
template-matching the generalbank textures against `hud_weapon_1080/_768` and `hud_dpad_1080` (±1 px):
- HEALTH: panel 78 at (−5.3, −3.1); green fill x 14.9–22.3, y 16–60 under bar 77 at (9.3, 16.4) = adrenaline, or breath
  while it is below full (HUD script "Showing adrenaline bar as breathbar"; replaces our old AIR bar); yellow (255,255,0)
  fill of mask 76's opaque rows 25–102 at (35.1, −2.7) = health / max [I mapping]; silhouette 75 at (9.8, −2.7) tinted by
  the HUD script's four "Player health is between …" branches [V bytecode: (r,g,b) constants]: 2000–1500 (0, v, 1),
  1500–1000 green, 1000–500 (v, 1, 0), 500–0 red, v linear over the band [I]; absolute health, so Normal's full 1000 is
  the captures' pure green.
- WEAPONS: icon (HUD script list wp_handgun 96, wp_thorhammer 97, wp_uzi 98, wp_shotgun 99, wp_assault_rifle 100,
  wp_tranq 101, wp_speargun 102) at (59.1, −4.9), 0.65 unit/texel; clip pips per gun row: bullets 83 full / 84 spent /
  85 white from (63.6, 36.0 / 43.1) every 6 (12 per row; uzi's 40 squeezed into the same width [I]); shells 86/87/88
  from (66.8, 36.6) every 11.7; spears 89/90 [I]. One gun's last loaded round is white (second row at rest) [V; I: the
  next hand]. Grenade row: 81 full / 82 empty at y 52, one per carried maximum, x from 63.6 every 9 [I step].
- GEAR d-pad: cross 91 (half opaque) centred (104, −73); 92 health packs (count in the box, game font 0.55) at
  (104, −106.5), 93 flashlight (71.1, −72), 94 weapon (136.4, −73.8), 95 grenades (103.6, −38.2), centres, all 2/3
  unit/texel, at 0.3 brightness except the last pressed item [V captures: R lights 94].
- Visibility [I timing; the script plays per-group fade anims]: hidden at rest; HEALTH on health / adrenaline change,
  breath below full or a weapon out; WEAPONS while drawn and on clip / weapon change; GEAR on heal, weapon switch or
  draw. Each holds 3 s after its trigger, fades in at 4/s and out at 2/s; death hides all.
- Reticles [I, not captured]: free aim 69, soft target 65, hard lock 72, adrenaline window 71 with pip 74, 0.35
  unit/texel at the target's aim point; grapple target marker 70 at the anchor's screen point.
- Notifications (pickups, "Save Successful", load errors) [I]: game font 0.55 on a dark band at y 398; chapter title
  card: font 1.2 at y 208 and the subtitle at 0.45, as the loading screen [O]; subtitles via `Hud::subtitles`.
Tests `tests/test_hud.cpp`: `Hud.SpaceKeepsTheSafeMarginAtEveryAspect` (16:9, 4:3, 21:9, 32:9, 5:4),
`Hud.HealthColourBands`, `Hud.GroupsShowOnTriggersAndFadeOut`.

**Music** (docs/formats/AUDIO.md §5): one track, `pc-w\music\<name>\<name>.mul` with its `.mus` sequencer setting the
stem gains every tick (variables all 0: writers of e.g. 0x7D are [U]). The track changes (2 s crossfade) when the current
unit has a `<unit>_mus_01` track [I naming] and when Lara enters a volume whose intro script names a track
(184 `soundvolume`, 24 `soundtriggervolume` intros; soundvolume derives from triggervolumebase, soundvolume.drm 3485 →
3516 → 3515 [O]). [O] `--chapter 1 --music-wav`: tick 1 l1_seatop_mus_01 (unit), tick 100 l1_seafloor_mus_01 (Lara fell
into l1_seafloor), tick 105 l1_seatop_mus_01 (volume 927), tick 181 l4_pool_mus_01 after a quickload; 0.5 s RMS
0.084 → 0.043 across the crossfade, then 0.08–0.11 (WAV in openunderworld-work/screens/progression, not listened to).

Verified [O] (2026-10-28, GL + Vulkan, screenshots viewed): `medsea_loop.bik` behind the radial main menu, Options
opened by replay, no gameplay HUD, and no Vulkan validation errors. `menu_new_game.txt` with `--no-movies` reached
CROFT MANOR and Lara walking. Earlier [O] (2026-10-18, GL): `menu_new_game.txt` → explode.bik
(45 frames) → "CROFT MANOR" card, Lara walking in l3_main_bedroom; `pool_pickup.txt` in l4_pool with `--spawn
-6045,-6100,2844,-90` → treasure 5489, "TREASURES 1".
