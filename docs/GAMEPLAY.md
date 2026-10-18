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

Controls: WASD/arrows move relative to the camera, Shift walks, Space jumps (and climbs up while hanging), Ctrl or C
lets go of a ledge, E uses switches / pushes and pulls blocks (with forward/back held), mouse orbits the camera.
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
| SwimUnder | `swim_sub` (370) / `swim_sub_up` (373) / `swim_sub_down` (371) by pitch, Surfacing = `swim_sub_up` | 21, 1.40 s | none | yes |
| SwimClimbOut | `swim_climbout` (368) | 47, 1.57 s | (0, −308.7, 172.8) | no |

- [O] No exe string names these clips; they are the TR8 anim-list debug names in lara.drm. The state → clip choice is
  ours [I]. `ledgeb_*` (braced: feet on the wall) was chosen over `ledgeu_*` (free hang) [I].
- [O] The air clips have no root motion: flight is physics-driven (below). There is no `ledgeb_left`; [I] the game
  probably mirrors `ledgeb_right`.
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
| Walkable slope | normal z ≥ 0.7 (≈ 45.6°) | [I] ours |
| Turn rate (walk/run steering, swimming) | 6 rad/s | [I] from `ng_turn_center`; turning in place uses the clip's root rotation instead (above) |
| Ledge grab | hands within 60 units horizontally, ledge between hand height −40 and +10, falling, facing the ledge (cos ≥ 0.5), **−2232.5 ≤ zVel ≤ 1545.6 units/s** | zVel window [O]: 0x759260 allows a grab when `−byte[data+0x834]·5.72443 ≤ zVel ≤ byte[data+0x833]·5.72443` (units per 30 Hz frame); the bytes are 13 and 9. Geometry [I] ours: the exe probes along the swept anchor at +63 (int data+0x824) and −158 (+0x828) units, needs no floor within 31 (+0x82C) below, and a reach ≤ 951.3 (float +0x838); which anchor (instance matrix +0x90 → +0x70) is [U], so these are not used |
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
  from `feet + stepUp` down `stepUp + stepDown` snaps to the floor. Nothing under her, or a floor steeper than the
  slope limit, starts a fall with the last horizontal velocity.
- Air: exact constant-acceleration step, full capsule sweep; contacts clip the velocity. Landing = a floor contact or a
  floor within a few units below the capsule while falling. Running landing keeps running if the stick is held.
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
- Interpolation: position, heading and clip time are blended between the last two ticks.

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
  towards the wish at turnRate; the clip follows the pitch (`swim_sub_up` > 0.35 rad, `swim_sub_down` < −0.35). No
  input: `swim_submerged_idle`, no drift. Reaching the surface = SwimTread. Movement is a sphere (r 100) swept around
  the root, slid along contacts and clamped to the surface.
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
- `GameReal.*` (gated by `OU_GAME_DIR`): the clips load, `ng_run` root speed 1274 ± 5 units/s, hand anchor; l4_pool:
  `'player'` spawn, 1.5 s run replay never below the floor and covers 50–105 % of the same replay on a flat floor; jump
  from the lower floor grabs ledge 1253 (hands at z 3562) and climbs onto the rock top (z 3571 ± 1, end snap < 10).
  `PoolSwimReplay`: `findWater` gives one box (id 5608, surface 2699.3 ± 0.5); from the west walkway (−6000, 0, 2844.4)
  Lara runs into the goo, swims east 2 s (> 80 % of 2 s × swim speed), dives below surface − 600, swims up to tread,
  swims back west and climbs out (720 ticks) onto a pool rock at (−4190, −183, 2864.9), standing on its floor.

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

No side-by-side comparison with the original game was made.

## Deviations and open questions

- Jump velocities, step limits, swim speed, camera distance/height are inferred; they are not in Lara's player data
  block (Numbers) [U]. The grab zVel window, height and water probe are taken from it.
- No blending between clips (pops at state changes).
- Swimming: the model is not pitched (only the clip choice follows the pitch); no oxygen, drowning, wading clip
  (`swim_wading`), surface turn clips, underwater fast/sprint, or swim damage; the water surface is static (scripted level
  changes not run); the water box is sized from the surface model, not the exe's component.
- Edge teetering: walking slowly off a ledge can alternate Fall/Land on the edge corner (the capsule lands on the
  corner while the floor probe under the root misses); running off works.
- Missing: crouch, roll, sprint, slopes/sliding, wall/poles/beams, hit reactions, adrenaline, weapons other than
  the pistols, state-chart conditions (`PlayerSC_*`, `PSCCF_*`). Saves do not store health (a load restores full).
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
cinematic starts the next chapter at once (the cinematic itself is not played). The l1/l6/afterburn cinematics are only
reached through unit scripts or other cinematics, which do not run yet [U]; `--chapter N` starts any chapter.

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
