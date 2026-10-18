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
blocks: `src/script`, docs/formats/SCRIPTS.md; script events are logged to stderr.

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
is not decoded. [O] A 5.5 s replay (run, running jump, walk) opened the SDL device, played l4_pool_mus_01 and 11
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

## Enemies

Code: `src/game/ai` (`Enemy`, `Enemies`, `loadEnemyType`), ticked by `World::tick` after the script runtime; each enemy
registers in `World::targets` (Combat's `Damageable`) and hurts Lara through `World::damageLara`. ou-game draws them
CPU-skinned (model 0) instead of their static intro meshes; `--show-enemies` draws capsules (colour = mode), heading
and line of sight; the overlay shows the nearest enemy; mode changes and blows are logged to stderr.

### Survey (intros per unit) [O]

- Mexico: `cat_panther` ×13 (ballcourt 4, temple 5, templepath 1, trailstart 3), `panther_thrall` ×1 (l4_pool 5645),
  `mayan_thrall_pool_generator` ×13 (l4_pool), `poacher` ×12 / `poacher_long_range` ×4 (ballcourt, temple),
  `bat_enemy` ×5 + `small_albino_spider` ×22 (temple), `giant_albino_tarantula` ×13 + `small_albino_spider_generator`
  ×9 (traprooms), `giant_tarantula_generator` ×3 (staircase_xibalba). Thailand: `tiger` ×8, spiders, bats.
- Enemy objects: ObjectBase oflags2 (+4) bit 0x400000 selects the "enemy" object family (tru.exe 0x5BF0E0 → family
  table 0x8ABA28 entry 14: init 0x63C190 (via 0x63C530), message 0x6164D0 (via 0x6187C0)).
- Generators: ObjectBase +0xDE u16 = spawned object (mayan_thrall_pool_generator → 600 `mayan_thrall`, others → bats,
  spiders, tarantulas).

### Enemy data (ObjectBase +0xC8, the instance+0x50 block 0x63C190 reads)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | magic 0xDAF00018 | [O] 0x63C190 bails without it |
| +0x04 | class: 1 bats/sharks, 3 cats, 4 soldiers, 7 spiders, 8 thralls | [O] values; meaning [I] |
| +0x0C | byte: behaviour-tree vtable choice (0x8ACF90..A8) | [O] 0x63C190 |
| +0x20 | component list node (next +0x1C); node +0x3C → health block {easy, normal, hard, +0xC} | [O] 0x620CC0 → 0x6E77B0 by difficulty 0x6E7710 (≤ 0 → normal) into comp+0x2C64 |
| +0x2C | 94.8 (0.3 m) on all types | [O] value; [I] body radius |
| +0x3C, +0xA0 | 1264 (4 m), 60° | [O] used as a vertical tolerance (0x6497C0, 0x67B610) and a tilt limit (0x620500) |

Normal hit points [O]: cat_panther / panther_thrall 700, tiger / poacher / giant_albino_tarantula / viking_thrall 500,
merc 450, mayan_thrall 350, yeti_thrall 1100, bat / small spiders 2. Distances in the data are multiples of 31.6056
(316.056 units = 1 m, consistent with the hair constants).

### Behaviour (ours) and numbers

Modes idle → alert (0.4 s, turn) → chase (run clip root motion, turn 5 rad/s) → attack (one blow in the hit window)
→ chase; hit reaction; death (holds the last frame); spawning (generator clip). Clips by role, first present name:
idle `passive_idle`/`idle`, alert `attack_idle`/`idle_aggresive`, run `run_forward_2`/`run`, attack
`simple_attack_static1`/`simple_attack`/`slash_attack`/`slash`, hit `hitreact_medium_front`/…, death
`death_static`/`death`/`damage_front_death`, spawn `spawn_from_pool`. Movement: capsule collide-and-slide, ground
snap 70 up / 140 down, gravity, ledge avoidance (no step leaving > 300 of drop ahead); never closer to Lara than
radius + 100. Panther `run_forward_2` root speed 2323 units/s [O].

| Quantity | Value | Provenance |
|---|---|---|
| Hit points | data (above) | [O] |
| Radius / height | +0x2C / highest bind-pose bone (panther 344.6) | [O] / [I] |
| Sight | 3160.6 (10 m), ±60°, raycast eye (0.8 h) → Lara (0.75 of 512) | [I] the exe's senses (EnemySensesPositionUpdate 0x659FA0) not decoded |
| Sense radius (any direction) | 790.1 (2.5 m) | [I]; +0xE4 list holds {300, ptr, 948.2/790.1, 60/100, 60/100} records [O], meaning [U] |
| Attack start / reach | 474 / 632 | [I] (+0x7C/+0x80 = 474, +0x88 = 632 are candidates) |
| Blow damage | 120 Lara hp (light) | [I]. Melee hits post msg 0x11C via 0x63A810 with `0x6460C0(...) * desc[+0x50]` (0x65E1B0, 0x667AB0); the desc layout is [U]; panther attack records (list node +0x80..+0x9C, 0x220 bytes) hold 12/10 at +0x40/+0x44 |
| Hit window | 35–60 % of the attack clip, 0.5 s cooldown | [I] |
| Stagger | after `block +0xC` damage (panther 80, thralls 0 = every hit) | [I] |
| Generator trigger | Lara within 2528 (8 m), ±2000 z, once | [I] the real trigger is scripted/EnemySpawnData 0x614FB0 [U] |

Not done: behaviour trees (search, flank, cover, taunt, jump/vault, nap), ranged soldiers, flying bats, sharks
(warned and not spawned), navmesh/routing, enemy sounds, respawn reset on Lara's death, generator spawns kept across
streaming (their generator stays fired).

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
  400 frames (+0x74) along `0x6E79D0`: base + cap·((1−e)t + e·t³), e = +0x7C = 0.7, capped at 25 %. Above max (health
  packs with overheal) it decays from data+0x80 = 1.4 × max to max over +0x84 = 900 frames. Health packs (heal
  `0x6F4EF0`) are Progression's pickups; `Health::heal` is the hook.
- Death [I]: at health ≤ 0 on the ground, `death_fall_normal` (fall) or `death_toknee` (other damage) plays; 1 s
  after it ends World::respawn (last checkpoint, full health as `0x700060`). Hit reactions are not played [U].

Pistols [O] (wp_handgun data = ObjectBase+0xC8; stats block S = data+0xE0, read by fire `0x6C9E50`; `loadWeapon`):
clip S+0x0B = 12 per gun, S+0x88 flags 0x80 infinite reserve / 0x20 dual, 1 pellet, spread S+0x1C = 1°, short range
S+0x20 = 4000, max range S+0x18 = 6500. Damage from the ammo object `pr_handgunbullet` (id S+0x06) record +0x18:
hitscan, 40 at 0 → 30 at 4000 → 20 at 6500 units. Uzi/shotgun/rifle blocks decode the same way (tests) but only
the pistols are wired. Fire rate [I]: the exe fires on an animation event (0x75) of Lara's per-hand fire clip; we
fire one shot per `combat_pistol_[RL]fireE` length (10 frames), hands alternating; reload 0.7 s
(`combat_pistols_reload`). Hitscan from the gun muzzle to the aim point vs collision and target capsules, spread via
a 32-bit LCG (deterministic replays).

Upper body: Lara's pistol clips live in wp_handgun.drm. The 3×3 aim grid `combat_pistol_poseA..I` (measured: columns
+109° / 0 / −104°, rows +40° / 0 / −57°) is blended bilinearly by the aim yaw/pitch over the subtree of spine
segment 14, the fire clip's motion relative to its first frame is added for recoil, faded in over 0.2 s
(equip clip length) and out over 0.633 s. Guns ride bones 80/55 (hands) when drawn, thighs 9/4 when holstered;
mount offsets fitted on screenshots [I] (the data's attach segments data+0xB8..+0xBB = 7,6,5,4 index an unknown
list [U]).

Targeting [O data, I use] (Lara data +0x480 / +0x470, criteria records {u16 type, u16 mode, f threshold, axis}):
hard lock (aim held) tries tiers in order: nearest within 1264 (3D), then the smallest camera angle among targets
within 8000 and ≤ 90° from the camera axis, then within 8000 and ≤ 90° from Lara's facing; soft target (drawn,
aim not held): nearest within 5000 (2D) and 27.5° of her facing. LOS from 375 above the feet (+0x48C) is required.
Switch target cycles through the hard-lock order [I]. Locked and standing, Lara turns toward the target [I]. No
target: free aim along the camera ray. Adrenaline (meter +0x3C, slow motion) is not implemented [U].

Input: F draw/holster (fire also draws), LMB fire, RMB aim/lock, Q switch; pad Y / RT / LT / R3; input scripts
`draw`, `fire`, `aim`, `switch`. HUD (ou-game): health bar, lock reticle / free-aim cross, clip counts; muzzle
flash, tracer and impact lines; `HG1_shot_c_*`, unholster and magazine waves from wp_handgun.drm. `--target X,Y,Z`
places a 700-hp dummy capsule.

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
- Swimming: no oxygen, drowning, wading clip (`swim_wading`), surface turn clips, underwater fast/sprint, or swim
  damage; the water surface is static (scripted level changes not run); the water box is sized from the surface model,
  not the exe's component.
- Traversal: no balance wobble/unbalance (`bbeam_unbalance_*`, `bbeam_aboutfall_*`), beam sprint, catching the beam
  when falling off it (`bbeam_fall_right_catch`), pole shimmy/parallel hang (`HPole_ParaForward`, `hpole_IdleHangPara`)
  or swing physics (the swing is the clip; fling ignores its phase), no mirrored left-side clips, no `ledgeu_*` free
  hang, hurdles/vaults, ladders (markup 9), perches (13), pillars (15), ropes, zip lines, rail slides, cover, or
  `Wall_Hug_*`. Root motion of the jump, attach and fling clips is not applied (the flight is ballistic), so their
  poses can sit off the computed path.
- Free climbing (`DICE_*` clips) is not implemented: no markup type marks climbable walls (types 16–20 are the five
  CONSTRAINEDGROUND sub-flags 0x4000..0x400 + 0x8000 [O], not climb surfaces) and how the exe finds climbable faces
  (collision material/face flags?) is [U].
- Grapple is not implemented. Found [O]: object `g_grapple` (objectlist id 46) and `pr_grapplinghook` (77); targets are
  object intros `*_ei_grapple_ceiling` / `_wall` / `_post` / `_mounted` (ids 151, 155, 340, 349, 363, 390, 410, 454,
  624, 661, 662, …), not markup; the exe's grapple logic is state-chart conditions (`PlayerSC_Grapple::IsBelowAttachPoint`,
  `WallDetected`, `RappelSpeed`, … 0x739C80..0x73C330) plus `[GrappleRope]` rope bodies, and 180+ `grapple_*` clips
  exist in lara.drm. Needs an input action and a rope simulation.
- Missing: sprint, hit reactions, adrenaline, weapons other than the pistols, state-chart conditions (`PlayerSC_*`,
  `PSCCF_*`). Saves do not store health (a load restores full).
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
ours draws the chapter-0 unit dimmed instead.

**Level ends** [O data, I trigger]: the script of exactly one `cinematic_object` per chapter end names the next chapter's
start unit: l3_main_hall 4106 (prologue → 1), l1_cine_ship_sinking 1005, l2_cliffclimb 2565, l3_mansion_exterior_afterburn
4070, l4_ballcourt 5483, l5_valaskjalf 6846, l6_ship_natla_room 1387. l3_main_hall's script also names
`l3_caverns_start`, so the target is the lowest-numbered start unit after the current chapter. Trigger volumes link to
these cinematics (l2 2564, l3_main_hall 4129, l4 5468/6407, l5 6866/6872); in our runtime the Activate of such a
cinematic plays its cutscene (its `cinematic` member names the stream, docs/formats/CINEMATICS.md) and then starts the
next chapter. The l1/l6/afterburn cinematics are only reached through unit scripts or other cinematics, which do not run
yet [U]; `--chapter N` starts any chapter, `--cine NAME` plays any cutscene.

**Pickups** [O names/counts, I roles]: `*_rw_artifact` (7..22 per chapter) = treasures, `*_rw_treasure` (one per
chapter) = relics, `*_rw_shard` = health packs (base script 3924 pushes "g_healthpack" and uses the health/invslot
natives). All share base script 3674 (playersensor, "popUpTitleMessageChannel", hint_14). Ours: interact press within
200 units horizontally / 250 vertically of the reward intro picks up the nearest one [I]; it joins `World::collected`,
`World::found` counts it, the mesh is hidden and the HUD shows `TREASURES|RELICS|HEALTH PACKS` (1251/1252/1274) with the
count [I format]. Saved (docs/SAVES.md). Health packs are not yet usable (Combat).

**Menus** [I layout, O strings]: Main 51 title, 124 START NEW GAME, 126 LOAD GAME, 125 GAME OPTIONS, 131 QUIT GAME;
Pause 1049 PAUSE, 122 RESUME GAME, 127 SAVE GAME (slot `save-<date>`), 126, 1050 ACCESS PDA, 125, 1062 BACK TO MENU;
Load lists `saveDir()/*.ousave` newest first (456 "Unavailable" if none); Options 491: 487 SFX VOLUME, 488 IN-GAME MUSIC
VOLUME (mixer bus volumes); PDA page: chapter name, 1075 AREA INFO, 1073/1072/1274 with the found counts. Objective text is
not mapped [U]. Keys: arrows/WASD, Enter/Space/E accept, Esc/Backspace back; pad d-pad, South, East/Start. Replays drive
menus with `forward`/`right` ±1, `jump`/`interact` (accept), `drop`/`pause` (back); the script clock keeps running in menus.
Text uses the renderer's 8×8 ASCII atlas: button glyphs U+1800.. are dropped, accents removed.

**Music** (docs/formats/AUDIO.md §5): one track, `pc-w\music\<name>\<name>.mul` with its `.mus` sequencer setting the
stem gains every tick (variables all 0: writers of e.g. 0x7D are [U]). The track changes (2 s crossfade) when the current
unit has a `<unit>_mus_01` track [I naming] and when Lara enters a volume whose intro script names a track
(184 `soundvolume`, 24 `soundtriggervolume` intros; soundvolume derives from triggervolumebase, soundvolume.drm 3485 →
3516 → 3515 [O]). [O] `--chapter 1 --music-wav`: tick 1 l1_seatop_mus_01 (unit), tick 100 l1_seafloor_mus_01 (Lara fell
into l1_seafloor), tick 105 l1_seatop_mus_01 (volume 927), tick 181 l4_pool_mus_01 after a quickload; 0.5 s RMS
0.084 → 0.043 across the crossfade, then 0.08–0.11 (WAV in openunderworld-work/screens/progression, not listened to).

Verified [O] (2026-10-18, GL, screenshots viewed): main menu over the dimmed bedroom; `menu_new_game.txt` → explode.bik
(45 frames) → "CROFT MANOR" card, Lara walking in l3_main_bedroom; `pool_pickup.txt` in l4_pool with `--spawn
-6045,-6100,2844,-90` → treasure 5489, "TREASURES 1".
