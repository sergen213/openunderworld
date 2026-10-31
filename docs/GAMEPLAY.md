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
in the bigfile, how the game's Mus! sequencer picks and mixes tracks is [U]). Footsteps and every other character sound
come from the animation event lists, picked by the floor's surface material (AUDIO.md §9.7). Sound events (sfxmarker
ambience, script sounds, object SoundComponent loops such as waterfalls / fires / gates / gears, drowning line, T / Y
help-on-demand Hint / Answer lines with subtitles; input-script actions `hint`, `answer`): docs/formats/AUDIO.md §9.
3D sounds fade with tru.exe's rolloff (full within 158 units, silent past 3160 or the sound's own distance) heard from
halfway between the camera and Lara (AUDIO.md §9.12); looping ambiences hold until stopped and then fade out with
their release envelope (§9.13).

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

## Timing

Rule: everything that can change gameplay runs in fixed 1/60 s ticks (`game::kTickSeconds`), fed that tick's input;
the render rate only chooses how many ticks a frame runs (`engine::FrameClock::advance`) and how far between the last
two ticks it draws (`alpha`). Puzzles therefore play out identically at 30, 60, 144, 240+ Hz, without vsync and with
FSR frame generation (generated frames are made inside the Vulkan renderer from presented images; nothing in the
renderer calls back into the game). A frame with more than 8 ticks due (hitch, debugger, a blocking cinematic) drops
the excess time: the world falls behind the wall clock, but no tick is shortened or merged, so outcomes are unchanged.
Render rates dividing 60 Hz land on the tick exactly (a 1e-9 tick slack in `advance` against 1/240 rounding).

Audit (2026-10-29; path → tick-based? evidence):

| Path | Tick-based | Evidence |
|---|---|---|
| `World::tick` (src/game/World.cpp) | yes | `dt = kTickSeconds`; no time parameter at all; everything below is called from it |
| Player controller, climbing, grapple, ladders/rails, swimming (`Player::tick`, Climb/Grapple/Traverse.cpp) | yes | dt from `World::tick` only |
| Bike (Vehicle.cpp) | yes | `tickVehicles(input, dt)` from `World::tick` |
| Objects, physics doors, uber/multibody, terrain groups (`tickObjects`) | yes | `physics_.step(dt)` once per tick; `Scene::step` fixed iterations, no substeps by time |
| Spline movers (`Object::spline`) | yes | advanced in `script::Runtime::tick` by kDt (30 frames per 60 ticks; `GameReal.SplineMoverFollowsItsPath`) |
| Script VM: SLEEP/WAIT, fibers, clocks (src/script) | yes | `Machine::tick(kDt)`, clock natives `elapsed += kDt` (Script.cpp `Runtime::tick`) |
| Enemies (src/game/ai) | yes | `enemies_.tick(..., dt * timeScale)`; the head-shot slow-mo scales the tick's dt, not the frame's |
| Combat, health regen, gadgets, oxygen, ripples/splash sim (Combat/Gadgets/World/Water.cpp) | yes | dt from `World::tick` |
| Camera orbit and collision pull-in | yes | `updateCamera(a, dt)` in the tick; rendering blends `interpolatedCamera(alpha)` |
| Mouse look | yes | motion summed per frame, applied as a rate over the next tick (no motion lost at > 60 Hz) |
| Particle effects (src/fx) | yes, fixed | `System::tick` per tick with its own LCG; the emitter camera-range gate read the last *rendered* (interpolated) camera, so spawns and the shared random stream depended on the frame rate. Now the tick camera (`World::camera()`, or the `--camera` eye) |
| Sound triggers writing music variables (sfxmarkers, Sound.cpp) | yes | `GameSound::tick` once per tick on Lara's tick position |
| Music sequencer | yes | `seq->update(kTickSeconds, vars)` per tick |
| Menus / pause | yes | menu open: the world is not ticked; input-script time (`appTicks`) still counts ticks |
| Cinematics → gameplay | yes | played inside the tick by `onCinematic` (own wall-clock movie loop); Lara is left at the stream's tail position from the file, independent of how many frames played |
| Rendering, HUD toasts, menu animation, weather shelter fade, menu movie | render rate (no gameplay) | `menus.tick(dt)`, `shelter` (now `1 - exp(-3 dt)`: same fade at any rate), `MenuMovie` wall clock |
| FSR frame generation | never ticks | VkRenderer present path only |
| Enemy skinning culled by the view (ou-game `EnemyMeshes::pose`: off-cone and > 1500 units keep their last upload) | render only | shots hit `Damageable` capsules from the tick state (Combat.cpp `rayCapsule`), AI reads its tick `anim_`; animation-event effects take bones from `Enemy::pose(1.0)` / `World::pose(1.0)` for every enemy each tick, never the skinned vertices. The test reads a frame-dependent subset of enemy poses |
| Input sampling | per frame | devices are polled once per frame; every tick of a frame sees that frame's input (replays index input by tick) |

Tests: `Game.SimulationIndependentOfRenderRate`, `Game.SwimSimulationIndependentOfRenderRate`,
`Ai.SimulationIndependentOfRenderRate`, `Game.FrameClockTickAlignedAndHitchCap`, and on real data
`GameReal.{BallcourtLever,BallcourtWithEnemies,ElevatorUber,PushBlock,SplineMover}IndependentOfRenderRate`: the same input script at render
rates 24, 30, 60, 75, 144, 240, 1000 Hz and irregular 1–50 ms frames with 250 ms hitches, with ou-game's render-path
reads between frames, must give a bit-identical `World::stateHash` (Lara, camera, bones, rigid bodies, object
collision, script objects and fields, enemies) after every tick. ou-game's `final tick` line prints the simulated
state, `sim <stateHash>` and the particle count: `--render-hz 30 --frames 180` and `--render-hz 240 --frames 1440`
give the same line (ctest `game_render_rate_smoke`, l4_ballcourt lever replay). `--draw-last` keeps the render rate's
frame pacing and ticks but draws only the final frame, so the smoke takes ~35 s under Debug+ASan.

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
| SwimTread | `swim_surfaceidle` (386) | 24, 1.60 s | none; feet at −352, root 44.1 under the water surface | yes |
| SwimSurface | `swim_surface_fast_forward` (984), the forward clip of lara.drm's surface blend request 881 (fast_right / _forward / _left; its sprint twin is 896). [O-runtime] tru.exe surface swimming (24 Wine snapshots, l2_cliffclimb sea, bone z, root at surface − 44.1; mean rms best phase, free offset / fixed root): full skeleton `swim_surface_fast_forward` 16.7 / 18.3, `swim_normal_freestyle` 18.3 / 18.7, `swim_sub` 31.9 / 32.7, `swim_surface` 36.4 / 48.8; body (no hair) 10.7 / 14.5, 12.6 / 13.2, 30.0 / 30.9, 32.8 / 41.5; spine (pelvis → head) 1.8 / 2.4, 2.7 / 7.6, 2.0 / 2.8, 7.7 / 20.9. The spine cannot tell `swim_sub` from it; the limbs can. Which request index tru.exe plays was not read at runtime [U] | 17, 1.13 s | none | yes |
| (swim speed source) | `swim_surfaceidle_to_swim` (385) | 5, 0.37 s | −Y 206.1 | — |
| SwimDive | `swim_surfacedive` (382) | 18, 1.20 s | (5.0, −112.3, −666.8) | no |
| SwimUnder idle | `swim_submerged_idle` (199) | 47, 1.57 s | none | yes |
| SwimUnder | `swim_sub` (370) at every pitch (the model is pitched); Surfacing = `swim_sub_up` (373) | 21, 1.40 s | none; body pitch 8.8° (`swim_sub_up` 44.1°, `swim_sub_down` −30.6°) | yes |
| SwimClimbOut | `swim_climbout` (368) | 47, 1.57 s | (0, −308.7, 172.8) | no |
| Swim sprint (Shift) | `swim_surface_sprint_forward` (1716) at the surface, `swim_submerged_sprint` (194) under water; exhausted: `swim_surface_exhausted` (1425) / `swim_submerged_exhausted` (1426), once | 28, 0.93 s / 14, 0.47 s; 20, 1.37 s / 23, 1.53 s | none | yes / no |
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
| ClimbUp (hurdle onto a low ledge) | running `hurdle_low_onto` (1351) / `hurdle_low_onto_100` (1350); walking or standing `hurdle_onto` (1352) | 0.3 s / 0.3 s / 0.8 s | (0, −282.6, 162.7) / (0, −206.7, 316.1) / (0, −311.5, 317.3); applied, the rise scaled to the ledge | no |

- [O] No exe string names these clips; they are the TR8 anim-list debug names in lara.drm. The state → clip choice is
  ours [I]. `ledgeb_*` (braced: feet on the wall) was chosen over `ledgeu_*` (free hang) [I].
- Hurdles [O-runtime] (tru.exe under Wine, new game, l3_main_bedroom = ours + (9622, 333, −2977); traces in
  openunderworld-work/screens/prologue): running down the prologue corridor into the 253-high debris slab (ledge 974,
  34° off square) she climbs onto it **without a jump**: the root is ~64 in front of the ledge line when it starts, the
  rise takes 0.27 s and ends exactly on the ledge height, ~220 forward, and she runs on. The 121-unit hearth has no
  ledge markup and stays a wall; the 725-high wall at the far end of the left corridor (ledge 973) is taken with a
  standing jump from the rubble in front of it (50 and 101 steps), a ledge grab and a climb-up. Ours
  (`Player::tryHurdle`): a ledge (markup 1) stepUp..340 above the feet, the root within radius + 40 of its line, facing
  it within 53°, the stick pushing towards it (or jump pressed), and a walkable floor within 40 of the ledge where the
  clip ends: the clip of the nearest rise plays with its root rise scaled to the ledge (State::ClimbUp, no collision).
  [I] the bounds, walking clip, and jump trigger; `hurdle_ground_attach_*` / `_over` / `_to_ledgeb` / `_down_*` (vaults
  over, onto higher ones) are not played. The anim names count heights in cm: `_50` / `_100` / `_150` / `_200` rise
  158 / 316 / 474 / 632 units.
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
| Jump take-offs | standing, no stick: **1441.2** up (apex 201.6, 0.56 s air), leaving the ground when `ng_standing_jump` ends (0.33 s); stick held: **1499.1** up + **1200** along the stick, at 0.26 s into the clip; running: **1275.9** up + **1974.9** forward (apex 158, 0.50 s air) after `ng_running_jump_forwards`; the stick behind her (> 1.75 rad from her heading): she turns round and takes the running jump; to a side: the standing stick jump that way (units/s) | [O-runtime] tru.exe under Wine, `wmem trace` of Instance +0x20 position / +0x2A0 velocity (Lara-local, units per 30 Hz frame: 48.04 / 49.97 + 40.00 / 42.53 + 65.83; take-off 359 / 290 / 109 ms after the press, back + jump 121 ms; horizontal speed constant in the air), l3_main_bedroom start, traces openunderworld-work/screens/stairs `j_*.txt`. [I] the 0.26 s take-off point (359 − 290 ms before the clip end) and the 1.75 rad back threshold (our turn-in-place threshold); free leaps (back off a ledge, pole fling without a target, slide jump) reuse these speeds [I] |
| Capsule | radius **79.01**, top **489.2** (height 512) | [O-runtime] tru.exe under Wine, Lara's Havok proxy (instance +0x288) while standing: radius +0x65C = 79.0139, cylinder +0x660 = 189.633 (targets +0x6B0 / +0x6B4 equal: steady), centre 315.4 above the feet (proxy +0x648 vs Instance +0x28), so the capsule spans 141.6..489.2; swimming it is 80 / 228. 0x490E60 steps radius / cylinder towards those targets by ≤ 20 / 40 per frame; 0x7018E0 creates it (`FUN_0049a410(inst, 100.0, …, 28.82, …)`); [U] which data sets the per-state targets. Height [O] player data +0x10 = 512.01 (used as Lara's height: ×0.5 centre, ×0.75 head, capsule test `FUN_007144b0(pos, height, …)`), TRLAU-menu-hook `GeneralAttributes.height` [I name]. Ours: on the ground the capsule's bottom is the step height (110), not 141.6, so the 121-unit hearth stops her one radius short (tru.exe: 38..72 short, pushed to and fro, trace t2); what stops tru.exe there [U] (the proxy's bottom clears it). Walls climbed / hung on keep the root 100 out (`wallOffset`) [I] |
| Step up / down | **110 / 110** units: the root takes the floor height under it within ±110 (capsule slide starts 110 above the feet), so 97..100-unit stair risers are climbed and descended walking or running without a fall | [O-runtime] tru.exe under Wine (`wmem trace`/`tp`, l3_main_bedroom, unit offset ours + (9622, 333, −2977)): running up the manor staircase (risers 97..100, treads 186) her z jumps by each riser exactly as the root crosses the riser line, at unchanged run speed; the 121-unit hearth edge beside the start stops her ~56 units short. [I] 110 is between the two. [O-runtime] Lara's Havok proxy (instance+0x288) capsule: radius 79.01 (+0x65C), cylinder 189.63 (+0x660), centre 315.4 above the feet (+0x640): its bottom is 141.6 up, so the 121 block is not the capsule; which probe stops her is [U]. Our capsule stays radius 100 [I]. Regression: `GameReal.StairsWalkAndRunUpAndDown` (every riser of seven units' staircases, the bedroom flight up and down, the hearth) |
| Walkable slope | normal z ≥ 0.7 (≈ 45.6°); a steeper face is still stood on when one of four probes 60 units around the root finds a walkable face within 20 units of its height (bumps, seams: l4_ragnarok_room's 0.6-normal floor strips) | [I] ours |
| Slide | floors with 0.3 ≤ normal z < 0.7 under an unsupported root: down the fall line, accelerated by gravity along the slope (g·sinθ), no friction, no cap; the descent rate is carried into a fall | [O-runtime] tru.exe under Wine (l3_main_bedroom roof, normal z 0.45, trace slide1.txt): velocity along the slope (vz/vxy = 1.97 = tan 63.3°), speed 51.0 → 69.0 → 91.3 units per 30 Hz frame over 0.25 s = ~4.7k units/s² against g·sin 63.3° = 4601; no cap up to 2738 units/s. [I] the 0.3 lower bound; [O] the exe has `SlopeSlideState` / `SkiSlopeSlideState` / `SlideState` (RTTI) whose virtuals are all the empty 0x7987B0: the state chart drives them |
| Edge slide-off | a capsule resting on an edge corner (floor-like contact, no floor under the root) gets ≥ 200 units/s outwards | [I] ours (fixes the Fall/Land alternation walking slowly off a ledge) |
| Crouch capsule height | **260** | [O] player data +0x14 |
| Turn rate (walk/run steering, swimming) | 6 rad/s | [I] from `ng_turn_center`; turning in place uses the clip's root rotation instead (above) |
| Ledge grab | hands within 60 units horizontally, ledge between hand height −40 and +10, falling, facing the ledge (cos ≥ 0.5), **−2232.5 ≤ zVel ≤ 1545.6 units/s** | zVel window [O]: 0x759260 allows a grab when `−byte[data+0x834]·5.72443 ≤ zVel ≤ byte[data+0x833]·5.72443` (units per 30 Hz frame); the bytes are 13 and 9. Geometry [I] ours. What the exe does with the other bytes [O read, I meaning]: 0x759260 / 0x7594C0 get from 0x758DD0 the pelvis (segment 1, instance+0x90 matrix 1) position and its prediction N frames ahead (N = byte +0x820 = 0, resp. +0x821 = 3: the horizontal speed × N along the pelvis forward, z − ½·5.72443·N²), raise both by int +0x824 (**+63**) and sweep the segment between them with collision query 0x4935F0 (mask 0x2C19E7); a hit of kind 3 or 5 accepted by 0x5D3BA0/0x758F70 is repeated at int +0x828 (**−158**): the ledge face must span 158 below to 63 above the anchor. 0x7591E0: a probe from the root down int +0x82C (**31**) must find no floor. 0x758B00: the grab point must lie within float +0x838 (**951.3**) horizontally of a position stored at player-state +0x9C0 ([U] probably the take-off point). Which bone/point the anchor is is [U], so these are not used |
| Ledge targets (drop to hang, hang jumps) | the hang side must drop > 200 within 240 units of the ledge line | [I] ours; l4_pool's ledge 1253 sits up to ~180 inside the collision top, so a fixed 60-unit probe misses the drop |
| Balance beam | root within 30 (x/y and z) of a type-14 line, heading within 45° of it from the ground (any heading from the air); walk 140 units/s (clip) | [I] attach distances ours; [O] markup type 14 = MUD 0x10000 BALANCEBEAM |
| Jump assist | at take-off, of the jump-landing (2) and beam (14) lines the point nearest the natural landing (at its height) within **300** becomes the landing: horizontal velocity = Δxy / flight time, vertical unchanged | [O] solver 0x73EDC0: `v = (target − pos − ½·g·t²) / t` with g = −5.72443/frame², re-aimed every frame with the remaining time (0x751D10, player-state +0x77C) at the markup in +0xA64 whose MUD flags match 0x20830000 (jump landing, rail slide, perch, balance beam); speed cap player data +0xC10 = 18963 (never reached). [I] the 300 radius and solving once at take-off |
| Hang jumps | from a hang: up (stick forward/neutral, no top to climb): hands 150..700 above, ±100 aside; side: 150..800 aside, ±150 in/out, ±400 height; back: 200..1200 behind, ±200 aside, −700..+200 height, turning round. Target nearest to 400 up / 400 aside / 700 back + 100 down. Flight rises 60 above the higher end and lands exactly on the target's hang pose | [I] ours (the exe's ranges are in the state chart) |
| Poles | types 4 (`pole`) and 6 (`horizpole`); hands (pole pose) within 60 of the axis, rising or falling within the ledges' zVel window, from 40 below to **138** above the axis, facing across it (cos ≥ 0.5). Fling: to the pole 300..1800 ahead (±300 aside, ±500 height), else a leap at run-jump speeds | [I] ours; the 138 [O-data, I meaning]: `Hpole_perp_attach_M` starts with the hands 481 above the root against the pole pose's 619, so the catch pulls her down 138 onto the hang (a standing stick jump at the measured speed passes l4_traprooms pole 1336 with the pole-pose hands 62 above it); l4_traprooms' poles are 1264 apart; [O] the markup's matrix carries real translations there (COLLISION.md) |
| Water probe | box ±221 × ±221 × ±572 centred 320 above the feet | [O] shorts data+0xC3E/+0xC40/+0xC42 (221, 572, 320), box overlap against each water volume box in 0x710A20; the highest surface wins |
| Swim speed | **1348.6 units/s** (surface and under water), reached at **2475 units/s²**, slowing at **3760 units/s²** when the stick is released (she coasts) | [O-runtime] tru.exe under Wine (l2_cliffclimb sea, traces swim_l2b / swim_l2c): 44.95 units per 30 Hz frame at full stick both at the surface and after a dive; from rest 0.54 → 8.3 → 33.1 → 44.3 per frame at 0.05 / 0.2 / 0.5 / 0.87 s (S-shaped; the linear slope is fitted to its middle), after the release 44.6 → 21.6 in 0.18 s. [I] linear ramps. The old 562 (root speed of `swim_surfaceidle_to_swim` / `swim_surfacedive`) was the clips', not the controller's |
| Swim depth | water ≥ 400 under the surface is swum, shallower walked | [I] ours (tread pose feet at −352) |
| Swim sprint | **1.2 × swim speed = 1618.3 units/s** while Shift is held (surface and under water), same ramps; stamina shared with the run sprint (`sprintSeconds` 9.7, `sprintResume` 0.9); at exhaustion the exhausted clip plays once at normal speed without refill | [O-runtime] tru.exe under Wine (l2_cliffclimb sea, 2026-10-30; player velocity +0x2A0, traces in openunderworld-work/scratch/swimsprint): 53.94 units per 30 Hz frame sprinting vs 44.95, at the surface and ~2000 under it; 44.8 → 53.4 in 0.36 s after the press; with Shift held 9.95 s of sprint, then 2.13 s at 44.95 and 0.95 s bursts (the run sprint's 9.5..9.9 / 2.2 / 0.9 s). Clips [O-runtime, rms 18.7 vs 21+ for any other clip, z of the body bones over 14 snapshots]: the surface sprint pose fits `swim_surface_sprint_forward` (= `swim_sprint_butterfly`), lara.drm request 896; under water request 934's `swim_submerged_sprint` [I: pose not compared]. Ours: `Tuning::swimSprintFactor`, `Player::swim` |
| Water drag (splash) | velocity × e^(−3 t) | [I] ours |
| Buoyancy, air/oxygen | none | [U] no buoyancy or air timer found (only `death_drown` exists); not implemented |
| Fall damage | 0 up to **1280.03**, linear to 500 at **1817.32**, linear to 1000 (= death) at **2215.55**, death above | [O] floats data+0x4C/+0x50/+0x54 and the curve 0x6F1790; height = z on becoming airborne (player +0x6DC, set on FallState entry 0x75C550) − landing z; applied on landing (0x75D080 → 0x702FC0), never scaled by difficulty (Combat below) |
| Camera | **1261** from a point **462.6** above the feet, rest pitch **−0.0225 rad** (camera 1260.8 behind, 491.1 above the feet); while she moves (no vertical look input) the pitch returns to rest at 1.3 /s, at rest it holds; the yaw never swings behind her; pitch −1.2..0.7 rad, sphere-swept (r 20) pull-in. Bike: 1711 [I] | [O-runtime] tru.exe under Wine (l3_main_bedroom start, camera *0xE80534: position +0x40/+0x90, forward +0x30; camst.sh, traces camrec / campitch): rest 1260.8 / 491.1 / pitch −0.0225, look ray 462.6 above her feet at her; idle after a mouse turn or a look down: unchanged; running sideways from a turned camera: no swing behind her (the camera stayed put against the bedroom wall while she circled it; whether it is a drag camera in the open [U]); running forward after a look down to −1.05: pitch −0.47 / −0.34 / −0.25 / −0.17 / −0.10 at 1.0 / 1.3 / 1.6 / 1.9 / 2.3 s (fitted 1.3 /s). [I] the 200-unit teleport guard, the clamp, the probe radius. [O] LaraCamera (vtable 0x880574) clamps distance at 1000 and has a 300-unit floor in its collision pull-in [the runtime rest distance exceeds that clamp: U]; defaults are data-driven (`ExplorationCameraResource`) |
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

- [O] Wine (2026-10-29, `GAMELOOP_RequestLevelChangeByName(unit, gameTracker, 4)` from a remote thread): a type-4 level
  change keeps `+0x20C`, so jumping from a new game's prologue (its checkpoint save wrote 10, casual) into
  l2_cliffclimb shows Lara in `lara_casual` - the outfit the earlier Thor's-gear captures showed there. With `+0x20C`
  cleared first (as a new chapter starts: the reset 0x556940), the outfit id `0xE7F1C8` after each load is the start
  unit's `+0x294` object: l1_seatop 19, l2_cliffclimb 12, l3_caverns_start 10, l4_trailstart 14, l5_startsnow 16,
  l6_ship_exterior 13, l7_thin_ice (-> l7_arctic_sea) 18 - the table below, as ours (`StreamReal.ChapterOutfits`).
  Level-change types 2/3/8/10 also zero `0xE7F1C8` (0x5DF760).

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

## Mjolnir

- [O] Object `wp_thorhammer` (objectlist 42), ammo `pr_thorbolt` (71): weapon data +0xDC = 2 (its own inventory slot
  +0x24; player +0x50D = 2 selects it, the HUD names `wp_thorhammer`, icon 97). S block: no ammo slot, clip −1 (never
  empties, no reload; the HUD clip fraction is 0, 0x6E8370), range 1000 / 8000, spread 1, flags 0xC8 (infinite, one
  hand). pr_thorbolt record +0x18: hitscan, 1000 damage at every range (record +8 = 13, +0xC = 3 [U]); record +0x84
  (0x69E770 picks it while instance +0x131 is set, [I] under water: its hit object is `bullet_hit_thor_underwater`)
  1000 / 400 / 100. Hit object +0x3C `bullet_hit_thor`.
- [O] Acquisition: l5_valaskjalf's level-end cinematic object 6846 (250_val_hammer), end handler: `larabase` 37
  ("wp_thorhammer", 1, 1) on `playerref` 0 (0x7161F0 -> 0x716130: births the object, adds it to her inventory and posts
  its max ammo), then `uigamedata` 27 "L6_ship_exterior". No other unit script names it (string survey of every DRM:
  only l5_valaskjalf and generalbank's HUD); the `wp_thorhammer` intro in l7_yggdrasil_end and `co_thorhammer` are
  cinematic props. Ours: "wp_thorhammer" joins `script::Runtime::playerObjects` (larabase 37), `World::hasHammer`;
  `chapterGear(6..8)` carry it [I: the Andaman Sea start has it, walkthroughs]; our larabase 42 drops only `g_*` gear.
- [O] Change Weapon (tru.exe 0x704930) cycles pistols -> secondary -> hammer, skipping what she lacks.
- [O] Clips in wp_thorhammer.drm: ground_equip / ground_unequip (26 keys), aim poses ground_aim_right / _front / _left
  (ours: by aim yaw −52 / 0 / +32 deg [I], measured from the pointing right hand), the throw ground_fire_idle (29 keys,
  Lara animation event 0x75 at frame 10 fires), the ground pound ground_melee_idle (61 keys, event 0x4B at frame 12 ->
  0x6E68B0: 0x69F1E0 area hits from a runtime table, not traced: [U] damage, ours knocks enemies within 948 units — the
  reach of its bolt-1 lightning — into a hit reaction without damage), under water swim_submerge_thorhammer_attack_*
  (ours: _front, shot at frame 10 [I]). Fire = throw, target lock + interact = ground pound (walkthroughs).
- [O] Mount: weapon data +0xB8 / +0xB9 = Lara HInfo markers 5 (segment 71, the LEFT hand's grip child of 55) and 31
  (chest segment 15, on her back): she draws it from her back with the left hand, holds it up and points with the
  right. (Handgun +0xB8..BB = markers 7/6/5/4: right grip seg 96, right thigh, left grip seg 71, left thigh.)
- Effects: each bolt plays wp_thorhammer's `fire` events at the hammer head (bolt 2 from the hammer to the shot end,
  sparks, glow, light, filter) and bullet_hit_thor at the impact; wp_thorhammer's events of the swing / pound clips run
  on Lara (ground pound: three bolt-1 arcs from her root). Shot sound `thor_hammer_shot_c/_d`.
- Effect on enemies: 1000 per bolt kills viking thralls (500 hp) in one hit, yeti thralls in two on Normal (l7
  replay `tests/replays/mjolnir_thralls.txt`, viewed GL + Vulkan). No breakable objects or scripted Thor mechanisms
  check for the hammer in our data survey (Thor doors / pillars / rune switches react to the gauntlets' sensors).

## Thor's gear (draw groups)

- [O] Every outfit model (all 8 Lara DRMs) carries Thor's gauntlets and belt as model primgroups with a draw group
  (primgroup `+0x20`, DXHR `dword20`): 10 and 11 the two gauntlets (T-pose hands), 12 the belt; the wetsuits also
  use 1-3 for diving gear. An instance hides group n while bit n-1 of its mask `+0x23C | +0x240` is set
  (draw 0x5BBA90 -> 0x5B4150 -> PCRenderModelInstance vt 0x30 = 0x4F50F0); `+0x23C` is saved with the player.
- [O] Script instance native 61 = HideUnhideDrawGroup(bit, show) (0x798F10 -> 0x5B3DD0). Lara's script type 205
  (lara.drm) holds the group ids as INT member defaults stored inline in the member records `+40/+44/+48` = 10, 11, 12 and wraps them: vt 62-64
  show one piece and set its stickybool (`+52/+80/+108`, ids 4033-4035), vt 65-67 hide it, but a show helper does
  nothing unless Lara carries the piece's object (`g_gauntletleft`, `g_gauntletright`, `g_belt`: larabase 38
  0x777E90); the equip helpers vt 59-61 attach (larabase 37) or drop (42) that object.
- [O] Callers: Thor mechanisms show all three while Lara is at them and hide them after - trigger volumes
  (l2_maproom 9632, l3_secretchamber 12379: OnIn/OnOut) and player sensors (Thor doors/pillars 9744, cog wheels
  17325, rune switches 19581, l7_arctic_sea 19969: vt 14); l2_maproom 9626 shows the left gauntlet after its
  cinematic. Equip calls come from per-chapter setup scripts (types 5789, 9502, 11733, 13929, 17775, 19515, 19597)
  and l3_secretchamber 12371/12372 (right gauntlet).
- Ours: `SubMesh::drawGroup`; `script::Runtime::playerHiddenGroups()` starts with groups 10-12 hidden (the
  original's player mask reads `0xE03`/`0xE02` at every checked chapter start, below; where it is set at birth is
  [U]); instance 61 on Lara's script changes it; larabase 37/38/42 track the carried `g_*` objects
  (`playerObjects()`; 42 drops all three, its slot argument is not tracked [I]). Mask, objects and Lara's script
  fields are saved (`script.player`); a chapter start without a save resets them. ou-game drops hidden groups'
  submeshes from Lara's meshes when building the scene (gameplay and cutscenes, GL and Vulkan), and meshes left with
  none (`scene::hideDrawGroups`: lara_jungle_heavy's group-11 gauntlet is a mesh of its own; kept empty, the
  renderers drew it whole and untextured, a white hand).
- Wetsuit dive gear (lara_wetsuit_warm/cold groups 1 = face mask + regulator, 2 = the same gear stowed [I], 3 = tank
  and harness). [O] Wine, player mask `+0x23C` / `+0x240`: chapter-1 start on l1_seatop's boat `0xE03` / `2` (1 and 2
  hidden, the harness drawn, as the capture shows), swimming at the l1_seafloor and l7_arctic_sea starts `0xE02` / `2`
  (only 2 hidden: the mask is on); treasure-hunt l1_seatop `0xE07` (3 hidden too); `+0x240` is 2 in every capture.
  tru.exe sets masks from instance data (`0x63C7A0` -> `0x698250`: visible-group bits of instance `+0x104` data;
  player intros have none), anim-effect commands 0x17 / 0x22 (`0x547B40`; none in Lara's clip lists; `0x69EB50` is
  registered in the table at `0xDA2024`) and scripts; what switches them on entering the water is not found [U].
  Ours (`World::laraHiddenGroups`): wetsuits hide 2 always and 1 unless Lara swims [I: the switch point]; 3 follows the
  scripts' mask.
- [O] Grants in story order: l2_maproom volume 9626 gives the left gauntlet after its cinematic, volume 9628 takes it
  back; l3_secretchamber cinematic 4654 gives the right one at its end (vt 60), 4655 takes it at its start (vt 58);
  l5_startsnow's chapter-start cinematic 7530 takes both at its start and gives both at its end (also sets story
  progress 5 via globals type 5 fn 4); l7_helheim01 8950 does the same. `g_belt`: the chapter setup scripts' `globals
  +16 stickyint > 7` branches give it, but loading l5_startsnow also gives it (below). Ours: `game::chapterGear(n)` =
  what Lara carries entering chapter n (0-4 none, 5-8 both gauntlets and the belt), applied by
  `World::startChapter`, `ou-game --chapter` and loads of saves without gear state (`StreamReal.ChapterThorGear`).
- [O] Checked in the original (tru.exe under Wine, 2026-10-29; chapters reached with GAMELOOP_RequestLevelChangeByName
  0x5DF8C0(unit, gameTracker 0xE7F088, 4) from a remote thread; carried objects read from larabase 38's slots,
  `*(*0xFADB58+0x500)+4+16i` = (instance, id) handle pairs left/right, name `*(instance+0x10)+0x60`; mask = player
  `*(0xE7F094)` `+0x23C`). Carried / mask / visible:

  | unit (state) | carried gear | `+0x23C` | on Lara |
  |---|---|---|---|
  | l3_main_bedroom (new game) | none | `0xE03` | nothing |
  | l2_cliffclimb (load from the prologue) | none | `0xE03` | nothing |
  | l4_trailstart (load) | none (a load drops what she carried) | `0xE03` | nothing |
  | l5_startsnow (load, also from a fresh prologue) | both gauntlets + belt | `0xE03` | nothing (bare hands, gameplay) |
  | l1_seatop (Treasure Hunt after the ending) | none | `0xE07` | nothing |
  | l7_thin_ice → l7_arctic_sea (load) | both gauntlets + belt | `0xE02` | nothing (bare hands, wetsuit) |

  So carrying is not wearing: outside the Thor-mechanism volumes/sensors the gear is never drawn, in any chapter.
  The ending (342_hel_endgame in l7_monastery, see docs/PROGRESSION.md) is the credits, then the main menu (a GAME
  COMPLETE save, Treasure Hunt).
  l6_ship_exterior did not finish loading by a level change (once a crash at 0x4A1720) and was not checked.
  Captures: openunderworld-work/screens/gear (`orig_*`, `ours_*`, `side_*.png`).
- Not done: player sensors (vt 14) never fire, and the per-chapter equip scripts do not run in our runtime yet
  (probe of 12 chapter units: no equip call), so the gear stays hidden outside a volume with carried objects. The
  cinematic base type 3523's gear events 10-21 are never sent (no retail stream has events).

## Controls

- Bindings live in `config.json` under `controls` (`bindings`: action → list of inputs; `mouse_sensitivity` 0.1..10,
  a multiplier on 0.0025 rad/pixel; `invert_y` for mouse and right stick). `ou-game --config` reads them; actions a
  file omits keep their defaults, an unknown action or input name is an error. Edit them on the launcher's
  Controllers tab (Add input… captures a key, mouse button, pad button or trigger; Esc cancels).
- Actions: forward, back, left, right, walk, sprint, jump, drop, interact, draw, fire, aim, switch, weapon, heal,
  grapple, save, load, hint, answer, pda, light, grenade. Inputs: SDL scancode names (`W`, `Space`, `Left Shift`, `F5`), `Mouse Left/Right/Middle`,
  `Pad <SDL button>` (`Pad a`, `Pad dpright`, `Pad leftshoulder`, `Pad leftstick`), `Pad lefttrigger/righttrigger`
  (pressed past half). Case-insensitive.
- Defaults are tru.exe's [O]: `FUN_00487710` (0x487710) binds the 25 virtual buttons below (key code = 0x80 + DirectInput
  scan code, 0x60 / 0x61 = mouse left / right; arrows as the second key of 16..19 and 23 / 24); the registry
  `HKCU\Software\Crystal Dynamics\Tomb Raider: Underworld\Input` holds the same list ("6"="170" = Left Shift) and
  `Input\Xbox 360 Controller` maps button i to pad code 64 + i, whose order is the PS2 pad's (tru.exe 0x844E40 /
  0x844E78: Y, X, B, A, LT, RT, LB, RB, LS, RS, Start, Back, d-pad up / down / left / right). Names: the Control
  Configuration screen (`openunderworld-work/screens/controls/buttons1.png`, `buttons2.png`).

  | button | original name | key | pad | ours |
  |---|---|---|---|---|
  | 19 / 18 / 17 / 16 | Up / Down / Left / Right | W S A D, arrows | left stick | forward / back / left / right |
  | 2 | Crouch/Roll (drop, dive) | Left Ctrl | B | drop |
  | 3 | Jump | Space | A | jump |
  | 1 | Grapple | Q | X | grapple |
  | 0 | Interact | E | Y | interact |
  | 5 | Shoot | H, mouse left | RT | fire |
  | 7 | Grenade (throw held object) | Caps Lock | RB | grenade |
  | 9 | Manual Aim | Z | right stick click | switch (change target; no manual aim yet) |
  | 10 | Pause | Esc | Start | fixed |
  | 11 | Access PDA | Tab | Back | pda |
  | 12 | Use Medipack | V | d-pad up | heal |
  | 15 | Change Weapon | R | d-pad right | weapon |
  | 20 | Walk | Left Alt | — (light stick) | walk |
  | 14 | Light | F | d-pad left | light |
  | 13 | Camera (field camera) | T | d-pad down | — (not implemented) |
  | 4 | Target Lock (draws) | G, mouse right | LT | aim |
  | 6 | Sprint (swim faster) | Left Shift | LB | sprint |
  | 8 | Recenter Camera (patch 1.1) | Q | left stick click | — (not implemented) |
  | 23 / 24 / 22 / 21 | Vehicle Gas / Reverse / Brake / Shoot | W / S / Space / H, mouse left | RT / LT / A / RB | stick forward / back; jump hops [I]; no bike shooting yet |

  So Lara runs by default; walking is Alt held (or the stick pushed lightly) and Shift sprints. Ours without an
  original key: draw / holster X (target lock and fire also draw), save / load F5 / F9, hint / answer F2 / F3. A config
  written before 0.12 (Shift walk, Alt sprint, …) gets these defaults for every action still on its old default;
  rebound actions are kept.
- Sprint (Tuning in Game.hpp; tru.exe under Wine, l4_trailstart, `openunderworld-work/screens/controls/s_*.txt`):
  sprint held while running plays `sprint_loop` (root motion 1927 units/s; tru ~1860 on a curve) for 9.7 s of
  stamina (measured 9.5..9.9), then `ng_sprint_to_exhaust` + `ng_run_exhaust` (no refill while they play) and `ng_run`.
  Stamina refills a second per second of anything else; a sprint starts again once 0.9 s is back (held throughout:
  ~2.2 s running, 0.9 s sprinting; 4 s without it after exhaustion: 4.1 s). Crouch while sprinting rolls with
  `ng_roll_front_sprinting`.
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
- Slide: down the fall line of the floor under the root (heading = downhill), accelerated by g·sinθ along the slope;
  a standable floor ends it through `land()`, no floor below falls, jump leaps off with the slide velocity + the standing jump’s 1441 up.
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

- Volumes [O]: each `watervolume` intro's data holds its box: half extents at intro data +0x54/+0x58/+0x5C (centre
  offsets +0x60/+0x64 are 0 in all 82 units), centred on the intro and rotated with it (only yaw occurs). tru.exe gives
  each instance a type-10 markup box from it (0x5D7130, MUD flag 0x100000); the water component takes its bottom/top
  from the box (0x69D350: +0x50 bottom, +0x54 top, +0x58 surface, set to the top). Intro data +0x10/+0x12 are marker
  intro ids (0x69D690, the surface-to-marker message 0x1B4 case 3), not the surface. The volume's handler (0x6AC340)
  moves the surface on script messages (0x1B4: absolute, relative to the bottom, to a marker intro, or by a delta).
  Open sea is such a box too: l2_cliffclimb 2563 (top z 108), l1_seatop 1851 (1672.7), l1_seafloor, l6/l1 ship
  exteriors, l7_arctic_sea's 11 volumes; l4_pool 5608's top is 2661.5 (the goo plane is at 2699.3).
- Ours: `findWater` builds these boxes (points: AABB min/max, centre, half extents, yaw axis; `insideWater` tests in the
  box frame); `loadUnitData` appends them to the unit's markup. Scripted surface moves are not run. Freshly loaded units
  get their scripted objects and colliders before Lara's first tick, so the l2_cliffclimb start stands on the yacht deck
  (z 71) instead of dropping into the sea under it (`StreamReal.OpenSeaWater`).
- Sea waves [O: decomp; register values read in tru.exe under Wine]: the volume's type hook 0x6AC720 gets intro data
  + 4; when data +0x4E has bit 1 (l1_seatop, l1_cineseatop, l1_seafloor, l1/l6 ship exteriors, l2_cliffclimb) the
  water component keeps four waves: kx at data +0x70 + 8i, ky at +0x74 + 8i, height = data +0x6C × share at +0x90 + 4i,
  speed (rad/s) at +0xA0 + 4i (l2: kx (1, 5, −14, −16)·10⁻⁴, ky (3, 0, −14, 2)·10⁻⁴, heights (100, 75, 50, 25), speeds
  (0.7, 1, 0.7, 0.3); l1_seatop heights × 0.885, last speed 2). The surface at (x, y) is the top + Σ heightᵢ (cos(kxᵢ x +
  kyᵢ y + speedᵢ t) − 1) (0x69DA80, used by 0x6AD010 / 0x6ACFD0 for Lara and floating objects), t = game seconds
  (0x6ADB70, 1/30 per frame unit) mod 200π (0x6C10D0); the sea drawable's vertex shader lowers its vertices by the same
  sum (VS InstanceParams 3–6, 0x69DFB0; RENDERING.md "Planar reflection"). The surface is never above the top: the
  l2_cliffclimb sea at the yacht swings between z 53 and −363 (mean −142, the deck is at 71–87) and l1_seatop's between
  1648 and 1245 under the deck at 1728, so both chapter starts are dry. Ours: `findWater` appends the waves to the
  markup (4 × (kx, ky, speed), 4 × (height, 0, 0)); `waterSurface` / `waveHeight` give the surface the player swims on
  (tread rides the waves) and `waveRegisters` the shader's c60–c63 (`StreamReal.YachtDecksAboveSea`).
- In water: the water probe box overlaps a volume, the feet are below its surface and there is no floor within
  `swimDepth` (400) below the surface. From the air → SwimSplash (plunge velocity damped by drag) → Surfacing (rise at
  swim speed) → SwimTread. From the ground (standing on a deep floor) → Surfacing.
- Surface (SwimTread / SwimSurface): root **44.1** under the wave surface in both [O-runtime: tru.exe under Wine,
  l2_cliffclimb sea, 2026-10-30: Lara's z fits the player's water top (player +0x8E0, −18895.8 there) + the 0x69DA80
  waves − 44.1 at rms < 1 treading and swimming at full speed; `swimSurfaceDepth`]. Treading plays
  `swim_surfaceidle` (its bones fit tru.exe's at rms 1.2, same root); swimming plays `swim_surface_fast_forward`
  [O-runtime: of the candidate clips it fits tru.exe's whole body best and its spine as well as `swim_sub` does (clip
  table, SwimSurface)], so only her head and shoulders break the surface. The stick swims up to 1348.6 units/s (ramping, Numbers) along the heading, which turns
  at turnRate; drop = SwimDive (`swim_surfacedive` root motion, 667 down); jump facing a wall whose top is 50..400
  above the water = SwimClimbOut; floor within 400 below = stand up (Idle).
- Under water (SwimUnder): stick swims horizontally, jump / drop held swim up / down; the swim direction's pitch turns
  towards the wish at turnRate and pitches the model; the clip is `swim_sub` (level stroke) at every pitch. No input:
  `swim_submerged_idle`, no drift. Reaching the surface = SwimTread. Movement is a sphere (r 100) swept around the
  root, slid along contacts and clamped to the surface.
- Sprint (Shift held while swimming, surface or under water): the sprint stroke (`swim_surface_sprint_forward` /
  `swim_submerged_sprint`) at 1.2 × swim speed on the run sprint's stamina; exhausted, `swim_surface_exhausted` /
  `swim_submerged_exhausted` plays once at normal speed, then a sprint resumes once 0.9 s is refilled (Numbers,
  "Swim sprint").
- Climb out: a sphere sweep 300 forward must hit a wall (|n.z| ≤ 0.5, facing it); a ray 50 past the contact finds the
  top. The root is placed so the clip's hands (at its start time) reach the contact and its root ends on the top
  (`top − rise`); the clip's root motion then plays without collision and the end snaps to the floor.
- Oxygen (ours; no tru.exe number decoded yet, rates [I]): `World::tickAir` drains the meter over 45 s while the head
  is under water (SwimUnder / SwimDive / Surfacing / SwimSplash with root + 150 below the surface) and refills it in
  2 s at the surface. At 0, `damageLara(max / 6 per second, kDamageBypass)` until Lara surfaces or dies. ou-game draws
  an AIR bar whenever the meter is not full. Air is not saved (a load starts at full health/air).
- Interactive surface (`src/game/Water.{hpp,cpp}`, `World::water()`; TRU does not simulate ripples): the wave field of
  our Wars and Warriors: Joan of Arc port (same author, GPLv3: its `water_waves.cpp` and `world_view_water.cpp`),
  `game::WaveField`. A damped 2D wave equation (Verlet, fixed 1/60 s substeps, velocity damping 2.5/s, viscosity
  0.005, a weak spring to rest) on a 256×256 grid that follows Lara in whole cells. Waves interfere, reflect off walls
  and scatter around bodies: a cell is a static wall when no water volume (markup type 10) covers it or terrain lies
  within 0.75 cell of its volume's rest surface (shores, rocks, hulls, pool sides; asked once as the cell scrolls in),
  and each body in the water (Lara, enemies, animals, moving dynamics) is a wall for the next step. Kicks are Mexican
  hats (zero volume, so the level never creeps). Units: Joan's lengths × `kWaterScale` = 6.4 (body heights: Lara
  ~512, a Joan soldier ~80), so cells are 25.6 units (a 6554-unit window), wave speed 512 units/s (CFL 0.33 as in
  Joan), splash crater 45 units deep / 64 wide; slopes, damping and times are Joan's. Sources: a body with its feet
  in the water and not under it wades (bow mound + stern trough, the wake; standing or treading, a bob every
  1.2–1.5 s: a kick as wide as the body, 25.6 units over sigma 90 [I], since a narrower one falls inside the body's
  own cells and leaves no visible ring).
  A body faster than the waves (swimming Lara, 1349 units/s) would trail a Mach cone, so it is no wall and has no
  continuous wake; it rings the water (strength 0.6) every 0.4 s instead [I], leaving flat rings behind it;
  Lara's entry splash (strength from the plunge speed) and climb-out; hitscan rays crossing the surface
  (`CombatState::shots`) and an enemy dying in the water splash as a projectile / body (strength 1); a dynamic body
  falling through the surface 0.8; droplets falling back leave pinpricks. `World::splashWater` takes other impacts.
  The field reports empty after 8 quiet seconds (the renderer skips it). Splash droplets (ours) are ballistic
  (gravity as Tuning, capped at 256, xorshift RNG) and die on the surface or after 0.35–0.75 s. ou-game copies the
  field, water volumes and droplets into `FrameOptions` for the Enhanced water shader (docs/RENDERING.md "Water": the
  Godot / afl_ext surface, which layers the field's height and slopes on its own waves); Original water does not use
  them. The drawn waves are centred on the gameplay surface (the original sea waves) and never rise above the rest plane. Tests `WaveField.*` (Joan's `water_waves_test` in TRU units: superposition,
  shore reflection, bodies as obstacles, damping, scrolling, wake height) and `Game.Water*`.

### Wetness (`game::Wetness`, `World::wetness()`)

- tru.exe [O: decomp] keeps one wetness object (global 0xFADF98, 0xA70 bytes, built by 0x6FF1E0): wetness of seven
  body regions at +0x00, dirt at +0x1C, region masks +0x38 (in water), +0x3C (in mud), +0x40 (soaking slowly), rain
  +0x44, the dirt colour +0x50 (PS InstanceParams 0 = c132, starts (0.625, 0.5, 0.359, 1), eases to the unit's colour
  bytes while muddy) and per-bone vec4 (wetness, dirt) from +0x60. The game loop ticks it (0x5DFF0A → 0x709D10 →
  0x6FF200, `timeMult` frames): a region in water gains +0xB84 per frame (lara.drm object data: 1, so at once) and loses
  dirt at +0xB80 (0.05); else it dries at +0xB78 (0.0002: 1 → 0 in 5000 frames, 167 s); 0x6ED3F0 clamps to +0xB70 /
  +0xB74 (1, 1). 0x6ED130 then walks bone lists (0x85DC60.., Lara's skeleton): head and face 17–51 (region 6), spine
  and neck 14–16, 52, 77, 102–105 (5), pelvis 1–3 (4), legs 4–13 (1), arms 53/54/72–76 and 78/79/97–101 (3), fingers
  55–70 and 80–95 (2); region 0 has no bones. A bone takes its region's value when that is higher; a wetter bone dries
  (same rate) only once the bone before it in the chain fell below 1 − 1/(5 n), so drying runs from the head down and the
  feet stay wet longest. 0x5B60F0 copies (wetness, dirt) into the model instance's per-bone rows, uploaded as the 4th
  row of every skin-palette bone (`SkinMatrices` c88 + 4 i, 0x50F0E0); Lara's skinned VS (slot 0x21) blends them into
  COLOR0 and her PS (lara.drm lib 25..) darkens the diffuse by `c110.y × wet` and adds a wet specular / environment
  sheen (`× c104`) masked by `s9` and the setup map; dirt lerps the albedo to c132.
- The region masks come from INSTANCE message 0x641 (handler 0x7686F0 → 0x6ED050: 1 = all regions, 2–8 one region,
  9–16 the same for mud, 17 slow soak, 0 / other clear). Read live in tru.exe under Wine (l2_cliffclimb sea, 2026-10-30):
  mask 0x7F and every region 1.0 while she swims. larabase 67 (0x778140 → 0x6ED510) soaks every region, 68 (0x778150 →
  0x6ED460) resets wetness and dirt; the chapter start cinematics run 68.
- Drying timeline [O: tru.exe under Wine, l1_seatop stern, 2026-10-30, *0xFADF98 sampled every 0.75 s]: the mask
  clears the frame she leaves the water; regions dry at 0.006 / s (0 after 166 s); per bone, seconds from leaving until
  it starts drying / is dry: head 17 0 / 166, face 51 33 / 199, neck 16 37 / 203, arm 53 38 / 204, spine 14 67 / 233,
  pelvis 1 78 / 244, hand 95 100 / 266, thigh 4 116 / 283, shin 5 133 / 300, feet 8 / 13 166 / 333. Ours matches to
  the sample spacing (test `Game.WetnessDryingMatchesOriginalTimeline`).
- Drips [O: lara.drm, Wine]: swim_climbout (and _A/_B/_C) frame 0 runs command 3 particle 208 on segment 17 (head):
  65 frames at 1 per frame, life 30, size 6 → 1, gravity −1.58, grey-white texture 164 (_A adds the 209 spray); in
  tru.exe the droplets fall from her head and shoulders for about 2 s after she climbs out, then stop. No other drip
  source exists [I: no wetness-driven emitter; 0x6FF200 and its callers start no effects]. Emitters re-read their
  segment every frame (0x540B80 reads instance +0x90 + 0x40 × segment), so they follow her onto the deck; ours did not
  (now `fx::System::place`, re-run each tick from the owner's tracked pose), which left the drips in the sea.
- Ours: `World::tick` sets the mask to 0x7F while Lara swims (`PlayerState::water` ≥ 0) and clears it otherwise [I:
  the poster of 0x641 is data we have not traced], then ticks `Wetness` with the same constants, bone lists and order;
  script events `Wetness` run 67 / 68. No soaking behind a black screen fade (`screenFade` 1): l1_seatop's chapter start
  leaves her 12 ticks in the sea after 030_med_yacht before 997 moves her onto the stern [I: tru.exe's Lara is dry
  there]. ou-game writes the weighted bone wetness into each skinned vertex's color0 (the
  sum tru.exe's VS forms) and translates the materials whose skinned VS reads the palette's 4th row
  (`scene::Material::skinParams`, 7 of lara.drm's 25) with COLOR0 taken from that attribute (`sm3::translate`
  `colorFromVertex`; formats/SHADERS.md). Not modelled: dirt / mud and its colour, rain soaking (0x52E180), partial
  wetness while wading; the generic (non-original) shading shows no wetness. Test `Game.WetnessSoaksAndDries`.

## Bend plants (src/game/Bend.*)

- [O] tru.exe "Bendie" (per-instance update 0x499B40, setup 0x48FD70) drives the 14 `bend_*` objects (kelp a/b/c,
  palm kelp, jungle broadleaf, bamboo, red plants, ...). Object data = ObjectBase +0xC8, class 57 / 0xB00B: +0x08 byte
  physical (phantom body; contact push), +0x14 activation distance (0 on all kelp), +0x24 -> 0x48-byte bone records,
  +0x28 count (kelp 1 bone = segment 1; jungle plants 3-5). Bone: +0x08 mass, +0x0C linear, +0x10 quadratic,
  +0x14 omega, +0x18 damping ratio, +0x1C push gain, +0x20 push radius, +0x24 push pad (x 3.1), +0x28 wind gain,
  +0x30/+0x34 rain-gust rate/gain, +0x3C s16 segment, +0x40/+0x44 sound thresholds.
- [O] Setup: the segment's first HInfo box gives the bend range (corners centre -/+ size/2 = state +0xC0 min / +0xD0
  max) and 1/(2 size.xy). Wine, l1_seafloor kelp 886: min (-407.7, -348.2, -34.9), max (407.7, 348.2, 3188.4),
  0.000613 / 0.000718 — exactly the box of `bend_kelp_a`.
- [O] Update (dt = timeMult/30): F += unit wind in instance axes x wind gain (+ rain gusts while it rains,
  DAT_00AE4D84, set by effect type 8; not modelled); Lara's push; then F -= 2 w zeta v + w^2 x (not scaled by mass),
  a = F/mass, v += a dt, x += a dt^2/2 + v dt, x clamped to the unit square (outward v zeroed) and disc. Output per
  segment (a.x, a.y, b.x, b.y): a = |x| x quadratic / span (span = min - max for x >= 0, else max - min), b = linear
  x - |x| x quadratic / 2 - (min + max) a / 2, uploaded as the 4th skin-palette row (0x5B60F0 mask 0xF) = the bend
  VS's c88: s = a v.xy + b, pos += t3 s.x + t4 s.x^2 + t5 s.y + (t3.w, t4.w, t5.w) s.y^2 (TEXCOORD3-5, decl hashes
  0x6E3DDDE9 / 0x637EFB30 / 0x67BFE687, FLOAT4).
- [O] tru.exe updates a plant only when it was drawn the frame before (instance flag 0x200: cleared for all instances
  by 0x5C0380, toggles on screen in Wine) or within +0x14 of Lara: on-screen kelp sways (Wine: kelp_b 873 x ~0..0.12,
  20 s series), off-screen kelp is frozen (state all zero).
- [O] Push: distance plants (+0x08 clear, 0x490450) within the push radius: away from Lara in the plane x
  push gain / (2 size) x (1 - reach/radius) x (Lara frame speed^2 + 1) while |x| < 1 - reach/radius; physical ones by
  the collision callback (0x490590 from 0x491CF0, 0.75 speed^2 + 150). Ours: [I] contact = Lara within 100 of the box.
- [O] Unit wind = settings DTP +0x2A4 (`formats::Unit::wind`), wind source type 1 (0x546170). Per 60 Hz wind tick
  (0x53C3F0): when the countdown (rand(+0x04, +0x08) x 60 ticks; constant 60.0 at 0x84BD00) runs out, targets
  strength rand(+0x0C, +0x10) x 8, heading rand(+0x14, +0x18) deg, wavelength rand(+0x1C, +0x20) x 100.6, rate
  |target - cur| / countdown; frequency +0x24 x 2 pi/60. rand(lo, hi) = lo + (hi - lo) sin(u pi) (0x52E750, LCG
  0x41C64E6D/0x3039). Evaluate (0x52E460): strength x sin(phase mod pi) along (cos, sin) heading, phase = ticks x
  frequency + |x + y| / wavelength — the wind pulses and never reverses (period 5 s on l1_seafloor). Wine l1_seafloor:
  strength 0.04..0.79, heading 6..72 deg, wavelength 700..1006, frequency 0.01047, clock ~60 ticks/s.
- Ours: `game::Bends` ticks every loaded plant (ponytail: off-screen ones keep swaying); `ou-game` applies the bend
  on the CPU each tick to plants within the fog range (`BendMeshes`), so both presets and the shadow pass sway; c88
  stays zero. Not modelled: rain gusts, bend sounds (0x5BA000 events 0xD/0xE), point/cylinder/vortex wind objects.
  Tests `Game.BendSettlesUnderSteadyWind`, `Game.BendGivesWayToLara`, `GameReal.SeafloorKelpSwaysInTheUnitWind`.

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
  move freely inside the band the holds cover.
- Hand stance [I, from the hold shapes]: the hold nearest the chest sets Lara's `DICE_*` clip set. Each hold is a
  triangle; points 1 and 2 relative to point 0 (right, up along the rock, means over all 336 holds in the 8 units,
  2026-10-29 survey) are 16: (−67, 144) (69, 174) both hands high → `B`; 17: (−75, 174) (41, 16) left high → `L`;
  18: (−134, 143) (136, 16) left high, wide → `LW`; 19: (−72, 14) (70, 143) right high → `R`; 20: (−96, 20)
  (125, 169) right high, wide → `RW`. tru.exe's own selection (state charts) is not traced [U].
- Attach: pushing into such a wall from the ground (chest probe at +300, reach = capsule radius + 90,
  facing it within ~53°) or falling onto it (after ledge/pole grabs, which win). The stance's ground attach
  (`DICE_B_ground_attach`, `DICE_L_ground_attach`, …) / air attach (`DICE_B_air_attach_M`, `DICE_<L/LW/R/Rw>_air_attach_mid`)
  plays, then its idle.
- Climbing: the stick's into-wall component climbs up (away = down), its along-wall component moves
  sideways at 240 units/s [I]; the stance's settle clips (`DICE_<s>_settle_up/_down/_left/_right`) have no root
  motion [O], so the controller moves the root and pins it at capsule distance from the rock, following
  the face (and around corners) with a back probe at chest height. Movement stops where no hold is in reach.
- Hold to hold [I]: when the nearest hold changes while moving, its stance takes over and
  `DICE_<from>_to_<to>_<dir>` plays once (its root motion not applied), then the new stance's settle clip. `<dir>`
  is the move's direction in 30° steps clockwise from up: `Up`, `UpR`, `UpDiagR`, `Right`, `DownDiagR`, `DownR`,
  `Down`, `DownL`, `DownDiagL`, `Left`, `UpDiagL`, `UpL` (the 12 suffixes lara.drm uses; the angles are ours). lara.drm
  has 181 of the 300 combinations and spells the stances inconsistently (`Lw`/`LW`, `Rw`/`RW`, `Dice_L_attach_*`), so
  `kLaraClipNames` lists every DICE clip exactly and `diceTransition` looks names up case-insensitively, falling
  back to the nearest existing direction within 60° (none: no transition clip).
- Top out: when no hold is in reach and a standable floor above Lara's feet is within reach past the wall,
  Lara steps onto it (Idle). Otherwise movement stops at the edge. Jump = the stance's `jump_back` clip + a leap away
  (700 out, 1400 up [I]); drop = its `drop` clip and fall. Cooldown 0.4 s before re-attaching.
- Tests `Game.ClimbAttachClimbUpTopOut`, `Game.ClimbJumpOff`, `Game.ClimbNeedsHolds`, `Game.ClimbStaysWithinHolds`,
  `Game.ClimbStanceFollowsHolds` (a column of 17/19/18/20/16 holds: L → R → LW → RW → B with `DICE_L_to_R_UpL`,
  `DICE_R_to_LW_UpR`), `Game.DiceTransitionNames`, `GameReal.CliffclimbStances` (l2_cliffclimb hold field zig-zag:
  3 stances, `DICE_L_to_B_Up`, `DICE_B_to_R_UpDiagR`, `DICE_R_to_L_UpDiagR`, every clip loaded; before 2026-10-29 the
  climb's "right" was Lara's left, so the settle clips were mirrored),
  `GameReal.ClimbOnlyAtHolds` (12 reachable walls ≥ 1000 from any hold in each of l3_main_bedroom, l4_pool,
  l4_ballcourt, l2_hubhall, l2_cliffclimb, l1_baldrsstairs_a: 0 attach, the old rule attached 10-12 of 12;
  l2_cliffclimb hold 910 ground attach + climb up; l1_baldrsstairs_a air attach at hold 44). Hand test:
  `ou-game --level 'pc-w\l2_cliffclimb.drm' --spawn '-23835,-3600,300,90'` and hold forward, then forward + left/right
  (stance changes; F1 overlay shows the clip); `ou-game --level 'pc-w\l3_main_bedroom.drm'` and run into any wall (no climb).

## Grapple (src/game/Grapple.cpp)

- Targets [O]: intros named `*_ei_grapple_*` (`ca_ei_grapple_post`, `sa_ei_grapple_ceiling(_invisible)`,
  `ma_ei_grapple_ceiling`, `ca_ei_grapple_wall_a`, `tp_ei_grapple_wall`, `ar_ei_grapple_wall`, …; not
  `fx_grapple_bling_*`), collected per unit into `UnitData::grapples`. Kind: Mounted when the intro's script type
  derives from native `uberobjectbase` [O: the movable targets are script type 3565's subclasses — levers, the naga
  statue, shiva arms, the grapple block, the snake head and the `sa_ei_grapple_wall` slabs (e.g. l2_towerconnector
  2005/2042/2043, l2_norsehub 2383/2394/2395); fixed anchors derive from type 3941]; else wall/post/ceiling by name.
  l4_pool's ceiling anchor is uid 5511 at (4362, -70, 2607). `sa_ei_grapple_wall_rigid_body` (l2_maproom 2676) is a
  plain anchor (script type 4734); its attached-instance field is filled at run time [U].
- Selection (ours [I]): within 300..5000 units of Lara's eye (+375), inside a 60-degree cone of the camera,
  clear line of sight (`World::grappleTarget`; the target object's own collision does not block it). Best by angle then distance; a crosshair marks it in ou-game.
- G / pad right shoulder (`Actions::grapple`) throws (`grapple_rappel_free_jumpon`) and attaches; G again or
  Ctrl/C releases. Space jumps off with the swing's momentum (`grapple_rappel_free_jumpoff`).
- Swing (ours [I], no exe numbers): gravity + stick pumping (500 u/s²), a rope constraint at hand = root + 400
  (outward radial velocity removed), light air drag. `grapple_rappel_free_swing_forward/backward` by travel
  direction, `grapple_rappel_idle` otherwise. E ascends the rope, Shift descends (300 u/s, clamped 200..6000).
  Swinging down onto a floor (> 300 u/s down), or coming down just over a floor out at the end of the arc (more than
  0.3 × rope from under the anchor), lands her: she lets go standing [I].
- Pull (Mounted targets): `grapple_pull_<up/mid/down>_light_begin` then the loop by anchor height (±400) [I];
  holding E: push blocks follow her (240 u/s [I]); other uber targets run their pull handler after a 1 s heave
  (`script::Runtime::grapplePulled`: the first overridden void slot 55..60 that does more than return — levers 59/60,
  shiva arms 55/56, walls and the naga statue 56/57, snake head 57 [O]; which slot tru.exe calls on a pull is [I]),
  then she lets go standing; `World::lastPulled()` reports the target.
- Rappel (`State::GrappleRappel`) [I]: a swing into a wall (|normal.z| ≤ 0.35) puts her feet on it,
  `grapple_rappel_wall_land(_left/_right)` by sideways speed, facing the wall. Stick into the wall shortens the
  rope (up, `grapple_rappel_up(_left/_right_45)`), away lengthens it (down, `grapple_rappel_down…`); gravity keeps
  the pendulum in the wall plane; she follows wall steps up to 150 deep, else swings free again. Jump kicks off
  (700 out, 200 up) into a swing, drop lets go; descending onto a floor ends it standing. Body angle: the rappel clips
  carry it (feet on the wall, body out along the rope; viewed, below).
- Wall run [I]: on the wall, a sideways stick runs along it (`grapple_rappel_left/right`, `grapple_rappel_walk_*`
  below 0.6 deflection): the feet push her along the wall up to 700 u/s (2500 u/s², never braking), so she runs the
  rope's arc down past the point below the anchor and up the other side. From the ground: roped to a Wall anchor, on
  a floor with the anchor's wall (the surface closest to the anchor) within 400 of her capsule, a stick along that
  wall toward the point below the anchor plays `ng_ground_to_wallrun_left/right` and starts the run (700 u/s, 350
  up); while that clip plays or she runs downhill along a floor the rope reels in its slack. Use sites [O]: the Wall
  anchors (name `*wall*`, not uber) — l1_kraken_room 326, l2_maproom 2676, l2_norsehub 2382, l2_puzzleroom 2974,
  l4_traprooms 5752, l5_valaskjalf 6750/6751, l7_helheim01 8897, l7_yggdrasil_end 8165; no markup marks wall-run
  walls (COLLISION.md types). l5_valaskjalf 6750 has ledges 1600 to either side of the anchor over a pit — the
  wall run carries her from one to the other.
- The rope is drawn from the hand to the anchor in ou-game. Tests `Game.GrappleSwingAndRelease`,
  `Game.GrappleWallRunFromGround`, `GameReal.PoolGrappleTargetAndSwing`, `GameReal.MountedGrapplePullsBlockWithoutSwinging`,
  `GameReal.GrapplePullsUberTargets` (replay `tests/replays/grapple_pull.txt` at l4_trailstart lever 6695,
  l2_norsehall naga statue 2457, l2_puzzleroom shiva 2982), `GameReal.KrakenRoomRappel` (replay
  `tests/replays/kraken_rappel.txt`: swing onto 326's wall, rappel down 460 and up 590, kick off),
  `GameReal.ValaskjalfWallRun` (ledge → wall run 370 down through the arc → far ledge, standing). Replay
  `tests/replays/valaskjalf_wallrun.txt` plays the wall run in ou-game.
- Viewed (2026-10-29, GL + Vulkan identical): l1_kraken_room rappel at tick 300 from the side
  (`--camera -2700,-13900,1450,-2980,-14862,1450`): feet on the wall, body out along the rope, roughly level; the
  l5_valaskjalf wall run at tick 205 from the follow camera (`grapple_rappel_right`: crouched run on the wall facing
  it, rope from the hand to the anchor). That shot first showed a bind pose: the run/walk clips were not loaded; the
  replay tests now assert every clip they play is loaded.
- Not yet: rope-vs-world collision, hook model, throwing while hanging from ledges, grapple combat
  (`grapple_rappel_combat_*`), overhangs, exe rope/pull numbers. Rope length/physics are [I].

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

Every unit of unitlist.txt (`OU_AI_SURVEY=1 ou_ai_tests --gtest_filter=AiReal.Survey`; `OU_AI_SURVEY_POS=1` adds
positions). Placed enemy intros, generators (→ spawned object) and the tilted crawler intros that cling:

| Chapter | Enemies |
|---|---|
| 1 Mediterranean | `shark_blueshark` ×6 (l1_seafloor); ship: `merc` ×9 + `merc_deux` ×3 + 8 `merc_generator_run` intros (l1_ship_exterior), `merc_generator_run` in the upper hallways and engine hall |
| 2 Thailand | `tiger` ×6 (junglehub) + ×2 (towerroom); `naga_green` ×5 / `naga_red` ×6 (norsehub, 7 clinging), ×12 / ×4 (puzzleroom, 4 clinging); `small_albino_spider` ×6 hubhall (all on a pillar), ×19 maproom (16 on walls), ×14 shrineroom (all on walls); `small_brown_spider` ×11 + `_no_jump` ×5 (cliffclimb, 6 clinging); `bat_enemy` ×12 maproom, ×9 puzzleroom, ×15 towerroom; bat / spider generators; `shark_blueshark` ×4 (cliffclimb) |
| 3 Croft Manor | `panther_thrall` ×2 (secretchamber); `small_brown_spider` ×38 chapel (27 on walls / ceiling) + 7 generators, ×11 stairhall (8 on walls); bats in the chapel |
| 4 Mexico | `cat_panther` ×13, `poacher` ×12, `poacher_long_range` ×4, `bat_enemy` ×5, `small_albino_spider` ×22 (temple, 17 on walls), `giant_albino_tarantula` ×13 (traprooms), `panther_thrall` ×1 + 13 `mayan_thrall_pool_generator` (pool), tarantula / spider generators |
| 5 Jan Mayen | `viking_thrall` ×7 entry, ×7 entryb, ×12 valgrind, ×4 + 2 pool generators valaskjalf; `yeti_thrall` ×1 valaskjalf; `giant_albino_tarantula` ×14; `small_albino_spider` ×10 (entryb, all on walls); bats ×35 |
| 6 Andaman Sea | `merc` ×2, `merc_deux` ×3, `merc_generator_run` ×23 + 6 `merc_generator` (→ `merc_generator_run`) |
| 7 Arctic Sea | `shark_greenland` ×8 (arctic_sea); `viking_thrall` ×12 + 10 generators, `yeti_thrall` ×11 + pool / plain generators (helheim01/03, yggdrasil_hall01/02, yggdrasil_end) |

- Not enemies (no oflags2 0x400000) [O]: the Kraken (`ms_ani_kraken_body`, `ms_ani_tentacle_*` hazards/door/gears/
  pillar/platform, `co_kraken_*`: animated scene objects driven by the l1_kraken_room scripts and uber commands),
  Natla's end (`natla_end` ×6 and `natla_end_mover` ×82 in l7_yggdrasil_end: physical script objects / spline movers,
  `wp_natla_fireball(_adren)` ×17 physical projectiles), `amanda` (2 clips; l1_cine_ship_sinking, l6_ship_natla_room,
  l7_thin_ice, l7_yggdrasil_end) and the doppelganger (`co_doppelganger` cinematic props only; no `lara_doppelganger`
  object exists, tru.exe only names it as a costume string). So there is no doppelganger or Amanda fight, and the
  Kraken / Natla encounters are scripted set pieces (docs/PROGRESSION.md), not enemy AI.
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
| node +0x64 | death record (below), with the regenerate fields | [O] 0x614610, EnemyState_Death 0x66C500, 0x67ED20 |
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

Regenerate (death record +0x20..+0x70; tru.exe EnemyRegenerateData 0x67ED20, decision 0x620380, hit filter
0x682490) [O]: +0x20 / +0x24 clip refs (fall, get up), +0x28 time (counts; [I] 1/30 s), +0x64 count of 0xC-byte
entries at +0x68 `{class (-1 any), type (0 any), flag}`. A lethal hit whose message +0x50 class / +0x64 type matches an
entry (0x67F040; needs a ref at +0x22 or +0x26) enters the regenerate state: full health at once (0x620CC0 →
0x637850), the fall clip, lying, then the get-up clip; any other lethal hit kills. While down, matching hits are
absorbed (0x682490 → 0x613BE0 only posts 0x3E to its attachments); others damage the full-health body normally.
Hit class / type come from Lara's ammo record +0x20 / +0x24 (0x6C64D0 → 0x63C9B0): pr_handgun / uzi / akrifle bullet
0 / 1, pr_shotgunbullet 2 / 1, pr_thorbolt 13 / 3.

| Type | Fall → get up | Time | Entries | So |
|---|---|---|---|---|
| viking_thrall, mayan_thrall | `death` → `death_reanimate` (5.4 s) | 450 (15 s) | {any, 1}, {1}, {2}, {10} | bullets, blows, [I] grenades knock down; Mjolnir kills |
| yeti_thrall | `death_standing` → `regenerate_01` | 210 (7 s) | {any, 1}, {1}, {2} | as above, but grenades kill [I] |
| panther_thrall | type-2 ref (ours: role `death_static`) → `death_thrall_regen` | 400 (13.3 s) | {any, 1}, {1}, {2} | as viking |
| others | none (+0x28 = 150 unused) | | | die |

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
  (`combat_run_forward`) to where she was; within 474 [I]: the melee blow (`rifle_bash`, 100). Mercs: guard zones,
  cover, squad alerts and barks below. The rifle (`wp_akrifle`) is not drawn: model 0 has no gun, the skeleton's bones
  are unnamed and the hand bone was not found [U]; it is attached by an EnemyWeapon process (3 per enemy, plus one
  EnemyHealthPack; 0x634020 → 0x616FE0 / 0x617040; update 0x62E3E0 births object +0x28 at marker +0x2C through 0x622BF0
  and posts 0x17B) [O], and a dead soldier's body gives its rounds (World::collectPickups, the 0x6F5E70 below-max
  rule) [I: our pickup at the body]. No grenades: no merc / poacher has a throw clip or a grenade record; their
  grenade classes are reactions to Lara's (EnemyBehavior_ReactGrenadeAttach, `hitreact_stickygrenade`) [O].
- Crawlers (enemy data class 5 `naga_red`, 6 `naga_green`, 7 spiders): an intro whose rotated up axis has z < 0.5
  (Rx(x) Ry(y), [I] 60° from vertical) clings to its wall or ceiling (`Mode::Cling`; drawn with the intro's tilt, idle
  clip, no gravity) [O: ~130 such spiders and 11 nagas, see Survey]. It notices Lara by eye range and ray only [I: on a
  wall the cones' heading means nothing], then walks its surface (`Mode::Crawl`) [I]: the run clip's root speed along
  the surface, down a wall (drifting sideways towards her) or across a ceiling towards her, held on by a ray along its
  up axis, heading the way it walks; at a wall's foot (a floor within 3 radii, 2 radii off the wall) it stands upright
  and hunts; above Lara on a ceiling, where the surface ends, after 8 s, or when shot it lets go (`Mode::Drop`: a
  radius off its surface, upright, falling with `fall_loop` / `falling_loop`, landing with `jump_end` /
  `fall_land_front`). A crawler killed on the wall falls off it. The original walks the planner mesh's wall faces
  (EnemyState_CornerWalk* / CornerJump*, the spiders' `concave_*` / `convex_*` transition clips), not decoded; ours
  plays no transition clip. The spiders' lairs (`idle_lair` / `exit_lair`) are not modelled.
- Nagas [O data]: roles `idle_passive`, `idle_aggressive`, `walk_forward1`, `run_forward2`, `simple_attack1` (red
  175, green 125: their +0x80 records), `hit_react_medium_front`, `dead_shot_front`; tranquilizer sleep
  `tranq_sleep_start` / `tranq_sleep_loop`, waking with `rest_to_idle` [I: no sleep end clip]. The green naga's +0x88
  record (`spit_attack`, 150, type 15) is a ranged blow [I]: out of melee reach and within 2528 units (8 m, the
  lose-track distance) with Lara in sight, once per its record cooldown; it hits in the clip's hit window if she is
  still in sight and range (no projectile).
- Thralls (behaviour 3) regenerate as above (`Mode::Down`, `Mode::Rise`; down time and clips from the record); after
  getting up they hunt again. Their stagger damage is 0: [I] 0 means no full hit reaction (they have additive
  `hitreact_*_add` clips; reacting to every pistol round pushed them back for good).
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
| Stagger | after `block +0xC` damage (panther 80); 0 (thralls, spiders, sharks) = never; a reaction is not restarted by hits during it | [I] |
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
`AiReal.NagasClingDropBiteSpitAndDie`, `AiReal.SpidersClingDropAndBite` and `AiReal.ThrallsGetUpUnlessMjolnir`
cover the crawlers and thralls (spawn counts, clinging, walking down the wall / pillar to the floor in l2_norsehub and
l2_hubhall, bites, the green spit, sleep, the regenerate record, knock-down / get-up timing, Mjolnir kills, cling
saved); `AiReal.MercsGuardZonesCoverAndAlert` the mercs (below).
Sharks sense Lara only while she swims (`Target::inWater` = `PlayerState::water >= 0`) [I]: l2_cliffclimb's placed
sharks 4021-4023 (1100-1900 below the start boat) used to see her on the deck, chase her out of the sea and land on it;
the original's chapter-2 start (Wine capture, "Chapter starts") has no shark in sight. Out of their medium they stay
idle (`Ai.SharkIgnoresLaraOutOfWater`, `StreamReal.ChapterStartEnemiesStayInTheirMedium`: no hunting enemy and no
shark above a dry Lara 5 s into any chapter start).

Mercs (l1 / l6 ship units: `merc`, `merc_deux`, `merc_generator_run`):
- Guard zones [O]: each merc intro's own script type (l1_ship_exterior 7780 for 1650, ...) has a native member of
  type 57 at +24, a noderef {record, node} (its declared value: 1650 {0, 127}, 1651 {0, 129}, 1652 {0, 168}, 1653 {0, 8},
  1654 {0, 14}, 1655 {0, 91}, 1656 {0, 75}, 1657 {0, 72}, 1660 {0, 77}, 1667 {0, 98}, 1668 {0, 42}, 1669 {0, 41}); the constructor calls enemybase 27
  `GuardNode(instance.GetUnit(), noderef, r1, r2)` (1650: 2500 / 4000; 1651 3000 / 4500; 1652 1400 / 2800; 1653..1657
  and the generator spawn 1658 500 / 1250; 1660 2000 / 3500; merc_deux 1668 / 1669 re-guard with 800 / 3500 and 600 / 3500 in a vt-62 handler,
  not run by ours). 0x777210 → 0x004AAF20 → 0x5C8EA0: the point = planner record `record`'s matrix × node `node`'s
  translation + the unit's offset (unit data +0x118 count / +0x11C 0x60-byte records, +0x4C node count, +0x50 →
  0x60-byte nodes: LEVELS.md "Planner nodes"); 0x61F7A0 keeps up to six {point, inner r1, outer max(r1, r2)}. (The
  earlier reading "node 1" was the script constant of another call.) Placed mercs stand inside or near their zone
  (1650 2 700 from node 127). Ours (`Enemy::zones`, `Enemies::guard`, saved as "guards"): 0x61F810 holds a point within
  320 along the up axis and within the radius across it; a merc idle outside its inner radius walks back
  (EnemyBehavior_MoveToGuardZone; `Mode::Return`, walk clip) until within 0.8 × inner [I: the stop]; a chase goal
  outside zone 0's outer radius is pulled to 0.8 × outer from the point (0x61F990, called with factor 0.8 by
  PatrolMove / InvestigateMove / SearchToLastKnownPos / Close) and the merc holds there facing her (and fires when it
  sees her). Patrol paths: no merc script or intro sets one (EnemyPatrolPath is unused on the ships) [I].
- Cover [I]: tru.exe's EnemyBehavior_Cover / AtCover* use the AI's planner node (+0x4E68) with cover flags (+0x84
  bits 6, 12..14) derived at load from 14 probes of radius 200 around each node (0x669D00; node +0x44 angle ≤ 85,
  +0x48 / +0x4C distances 316 / 284 / 474 on about half the ship's nodes, meaning [U]). Ours: after a burst a soldier
  with crouch clips picks the nearest planner node within 1500 (flat), within 320 in height and its zone's outer
  radius, not more than halfway towards Lara, reachable by a straight ray at knee height, from which a crouched eye
  (150) cannot see her chest; it runs there (`Mode::Cover`; gives up after 4 s), `combat_stand_to_crouch`,
  `combat_idle_crouch` for 2.5 s (reloaded), `combat_crouch_to_stand`, then hunts on (it keeps tracking her while
  hidden) and seeks cover again 6 s later. The `cover_enter_*` / `cover_exit_*` clips are not used.
- Alerts [I]: three EnemyFireTeam processes link soldiers (0x6170A0, update 0x62E590, team at +0x2C) [O]; ours: a
  soldier that notices Lara itself (idle or returning) plays `combat_bark_01` / `_02` (alert ends with the clip) and
  every other idle soldier within 3000 turns to where she was and hunts.
- Chatter [I categories]: the ship units hold the merc lines as DTPs of 5–10 streams each,
  `vo\enemy\merc\b_<voice>_<CAT>_<g|s>_NN` (voices M02 / M10 / M11, categories EVG, EAR, FDY, ADV, FLK, CBR, INV,
  SWP, GAT; AUDIO.md §9.10), and the `bark` native script type (generalbank game script, 3 functions) plus a BarkTimer
  pace them [O]; `combat_bark_*` clips carry no sound events [O]. Ours (`Enemies::barks`, played by GameSound from the
  loaded units' DTPs of that category at the soldier): EAR on spotting her, CBR on taking cover, FDY by the nearest
  squad mate on a death, SWP on losing her; one per 4 s.
- Enemy behaviour index (enemybase 16/17, 0x62DE00 selects the n-th behaviour-list node of the enemy data) and 22 /
  26 are kept for scripts but do not change our AI [U].

Not done: behaviour trees (search, flank, taunt, dodge, jump/vault), long-range specifics beyond their records, rifle
mesh, bat latch, the original's wall-walking transitions, navmesh/routing, ragdoll.

Verification [O] (ou-game, Vulkan, 1280×720, invisible KWin; screenshots viewed, kept privately; 2026-10-29):
replay `tests/replays/thrall_regenerate.txt` (l7_yggdrasil_hall01, `--spawn 1000,0,0,90`): viking 8833 charges, bites,
goes down to the pistols at tick 369 (full health), lies (tick 600 frame, fixed camera: on the floor beside Lara),
rises with `death_reanimate` at tick 1270 (15 s; frame 1400 mid-rise), attacks again; after X to Mjolnir a bolt kills
it for good (tick 1802); yeti 8831 down 7 s, then killed by the hammer. `tests/replays/naga_drop.txt` (l2_norsehub,
`--spawn -20348,27300,-4494,90`): red naga 2417 sees Lara at tick 1 and walks down its wall; fixed-camera frames
(`--camera -20900,26200,-3900,-20348,28200,-3700`) show it on the wall at tick 12 and standing up at its foot (tick 28),
biting Lara at tick 40. `tests/replays/spider_drop.txt` (l2_hubhall, `--spawn -19231,32752,5216,-90`): the six albino
spiders walk down the pillar and reach its foot at ticks 38..63 (log), then run in. `tests/replays/merc_squad.txt`
(l1_ship_exterior, `--spawn 3222,13593,3809,90`): merc 1650 spots Lara and barks (`combat_bark_01`, frame 60, fixed
camera `3000,15300,4500,4100,17500,4000`), fires from tick 122, takes cover at tick 223 and kneels behind it (frame 420,
camera `4900,18300,4250,4192,17669,4000`), stands up and fires again at tick 528.

Earlier verification [O] (ou-game, GL, 1792×1008, ASan debug build; screenshots viewed, kept privately): replay
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
- Max health = Lara data +0x44 (1000) × generalbank difficulty table (GlobalInfo = generalbank object data, +0x24:
  three 0x18-byte entries) `+0x08` at the PLAYER TAILORING "DAMAGE TO LARA" level (1.3 / 1.0 / 0.8, 0x6E7960); a level
  change keeps the health fraction (0x6F5160). The damage multiplier `+0x0C` (0.5 / 1.0 / 1.3) is read only at entry 1
  (`0x5D9E50` = (int) 1.0f at 0x8AB648), so it is always 1.0 [O].
- PLAYER TAILORING [O] (`game::Tailoring`, `World::setTailoring`, saved per game): levels 0..2 in game state
  `DAT_00c59264` +0x3EF98 enemy health, +0x3EF9C ammunition capacity (copied to +0x3EFA0 at level load), +0x3EFA4 Lara's
  health, +0x3EFA8 saving grabs, +0x3EFAC (no reader); `uiexperienceadjustmentmenu` natives 4..8; a preset (generalbank
  script 132 fn 4, `SetPlayerTailoringLevel`) sets all five to 0 CASUAL EXPLORER / 1 TOMB RAIDER / 2 MASTER SURVIVALIST.
  Enemy health = the enemy's health block [level] (0x6E77B0, ≤ 0 → [1]). Capacity: Lara data u16 columns +0x868 / +0x87C /
  +0x872 per ammo slot (0x6EEAE0: uzi 400/240/160, shotgun 90/72/42, speargun 40/25/15), start amounts +0x848 / +0x85C /
  +0x852 (0x6EE930, not used by ours), grenades g_grenade_thrower +0xE6 / +0xE4 / +0xE2 = 10 / 6 / 4 (0x6E7760).
  Saving grabs [I]: difficulty `+0x14` frames 90 / 50 / 15 (TR7's savingGrabTime; no exe reader found, the getter
  0x6E7740 is only reachable from scripts): ours catches a ledge one-handed (`ledgeb_bad_attach_M`) when falling up to
  twice the grab limit (−13 → −26 units/frame [I]); jump plays `ledgeb_bad_attach_recover`, else she falls.
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

| | pistols (wp_handgun) | uzi | shotgun | speargun | assault rifle (wp_assault_rifle) | tranquilizer (wp_tranq) |
|---|---|---|---|---|---|---|
| ammo object (S+6), mode | pr_handgunbullet, hitscan | pr_uzibullet, hitscan | pr_shotgunbullet, hitscan | pr_spear, projectile (2) | pr_akriflebullet, hitscan | pr_tranquilizerdart, projectile (2) |
| damage at 0 / short / max range | 40 / 30 / 20 | 25 / 25 / 10 | 50 / 50 / 40 per pellet | 250 [I] | 27 / 27 / 21 (under water 23 / 23 / 18) | 500 dose (+0x1C) [I] |
| short / max range (S+0x20 / +0x18) | 4000 / 6500 | 2000 / 4500 | 2000 / 3000 | 2500 / 5000 | 5000 / 8000 | 4000 / 6000 |
| clip (S+0xB), dual (S+0x88 0x20) | 12 per gun, dual | 40 per gun, dual | 6 | 1 | 30 | 1 |
| pellets (S+5), spread / max (S+0x1C / +0x2C) | 1, 1° / 1° | 1, 1° / 5° | 7, 15° / 15° | 1, 1° / 1° | 1, 1° / 5° | 1, 5° / 5° |
| spread per shot / recovery (S+0x30 / +0x34) | 0 / 0 | 1° / 0.1° per frame [I unit] | 0 / 0 | 0 / 0 | 1.2° / 0.125° | 0 / 0 |
| ammo slot (S+4), max reserve | none (S+0x88 0x80: infinite) | 0, 240 | 1, 72 | 4, 25 | 2, 210 | 3, 15 |
| refire [I] | `combat_pistol_[RL]fireE` 10 keys, hands alternate | the uzi's own `fire` clip, 5 keys, alternate | `combat_shotgun_fire_*` 13 + `pump` 25 keys | 13 keys, then reload | `combat_rifle_fire_*` 3 keys | 3 keys, then reload |
| equip / unequip / reload | 6 / 19 / 21 keys | 15 / 15 / 21 (wp_uzi's own) | 24 / 24 / 117 | 24 / 24 / 31; under water 17 / 19 / 31 | `combat_rifle_equip` 19 / `_unequip` 24 / `_reload` 41 | 19 / 24 / `combat_tranqgun_reload` 19 |

- Rifle and tranquilizer upper body: each DRM's own 3×5 `combat_rifle_aim_*` / `combat_rifle_fire_*` grid, blended with the
  shotgun grid's columns / rows [I: not measured separately]; muzzles [I] from the mesh bounds.
- Tranquilizer darts (`Hit::tranq`): animals with `sleep_start` / `sleep_loop` / `sleep_end` (cats; nagas'
  `tranq_sleep_start` / `_loop`, waking with `rest_to_idle` [I]; tru.exe EnemyBehavior_Nap / EnemyState_Nap) add the 500 dose instead of damage and fall asleep once the dose reaches their
  health (`ai::Mode::Sleep`, saved): sleep_start, sleep_loop for 20 s [I], sleep_end, then alert. Other enemies take the
  dose as damage [I]. Panther (700): two darts.
- Chapter loadout (`chapterSecondary`, `World::chapterLoadout` on a chapter start and for saves without combat state)
  [O object deps of every pc-w\l<N>_*.drm unit: all load wp_handgun + wp_uzi; l1 / l7 wp_speargun, l3 wp_shotgun, l4
  wp_tranq, l6 wp_assault_rifle, l2 / l5 / l7_himalaya_end nothing else; no unit script names a wp_* but
  wp_thorhammer]: Mediterranean / Arctic speargun, Thailand / Jan Mayen / epilogue uzi, Croft Manor shotgun, Mexico
  tranquilizer, Andaman Sea rifle; the prologue the pistols [I]. Every secondary starts the chapter with a full clip and
  reserve [V: the original's WEAPON SELECTION after a fresh l4 load lists all five full, the tranquilizer equipped].
- PDA WEAPON SELECTION's EQUIP (`World::equipWeapon`) makes the row's weapon the secondary with the rounds it has (no
  refill); in hand, Lara switches as with Change Weapon.
- Saves: `combat` = weapon in hand, secondary, per weapon clips + reserve, adrenaline (`World::serializeCombat`); a load
  holsters.

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
  come with the chapter (loadout above). Ammunition pickups (`ammoWeapon`, `World::collectPickups` on interact within the
  pickup cylinder, only while the reserve is below its max as 0x6F5E70): wp_* intros of a secondary and `wp_akrifle`
  (same pr_akriflebullet slot 2 as the assault rifle); an armed enemy's body (poachers' `wp_akrifle`, enemy data node
  +0x20, `EnemyType::weaponObject`) gives its rounds [I]. Each gives one clip [I: intro property 0x55 not decoded].
  `ou-game --weapon uzi|shotgun|speargun|rifle|tranquilizer|thorhammer` gives one with its max reserve for testing
  (`World::giveWeapon`).
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
between holster and hand half way through [I]. In hand [I, fitted on screenshots]: pistols / uzi on hand bones 80/55;
shotgun / speargun / rifle / tranquilizer with the grip in the left hand along the line to the right hand. Stowed [O]:
weapon data +0xB9 / +0xBB name Lara's HInfo markers (lara model, `AnimLibrary::markers`) and the model takes the
marker's frame: pistols thighs 6 / 4 (segments 9 / 4), uzis on her back 36 / 37, every two-handed weapon 15 (chest
segment 15, across her back), Mjolnir 31. The secondary is shown stowed while it is not in hand.

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

## Cameras (src/game/Camera.cpp)

- Orbit camera: "Numbers" table. Scripted reveal cameras replace it while they play.
- [O] `autosplinecamera` 0 (thunk 0x7B37D0 -> 0x7759F0) starts camera spline `n` of the owner's unit on the
  AutoSplineCamera (camera type 8, 0x5E26F0); the cameraspline value is {initial data, owner's stream unit, data
  word 0} (0x775760). `gamebase` 52 = CameraManager::Switch(type) (0x5EDFC0: 8 the spline camera, 1 gameplay),
  43 = current type == n. Reveal objects: `larabase` 56(false), autosplinecamera 0, gamebase 52(8); in the
  dispatchers' done slot (NsDispatcherBase prototype 7, 0x7B3E60, called by 0x5E2CA0 when the spline ends):
  gamebase 52(1), gamebase 30(true), larabase 56(true).
- [O] Tracks (LEVELS.md "Camera splines"): Hermite between keys (0x5F7340), by key times (0x60A950), through the
  keys' 64-entry ease curves (mode 3, 0x60A880) or at constant speed (0x60A540); orientation = slerp of the keys'
  quaternions (local x right, y down, z forward), or look-at the target track (0x5E2AB0). Key events: 1 switch
  spline, 2 switch to gameplay, 3 pause n s; target events 1/2 FOV blend (0x5E25E0, 0x5E2950).
- [I] ours: a spline camera cuts in, holds its last pose after finishing until a script switches (10 s safety), and
  blends back to the orbit camera over 0.5 s. Lara's stick is zeroed only while a spline camera plays and control
  is disabled (a reveal that never re-enables control cannot lock her). All per 60 Hz tick.
- [I] The renderer's portal walk starts from the spline's unit while one plays (`World::cameraUnit`), else Lara's:
  l2_hubhall's shiva-scales reveal (l2_puzzleroom spline 4) looks at the puzzleroom statue from inside that unit, and
  starting from hubhall clipped everything away (empty frame).

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
  `JumpTakeOffsMatchTruTraces` (standing / stick / running / back / side take-offs, apex 201.6 and 158 against the
  tru.exe Wine traces), `CameraRestAndPitchReturn`, `SwimSpeedMatchesTru`, `SwimSprintMatchesTru` (1618 units/s at the
  surface and under water, 9.7 s stamina, 2.13 s / 0.95 s with Shift held), `SlideAcceleratesAtGravityAlongSlope`.
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

- Jump take-offs, step limits, swim speed, slides, the capsule and the camera rest pose are measured in tru.exe under
  Wine (Numbers); traversal reach boxes and the jump-assist radius are inferred; none are in Lara's player data block
  (Numbers) [U]. The grab zVel window, height, crouch height and
  water probe are taken from it.
- Swimming: oxygen/drowning are implemented with inferred rates (45 s / refill 2 s / max-per-6 s; no exe numbers).
  Still missing: wading clip (`swim_wading`), surface turn clips, swim damage from other
  sources; the water surface is static (scripted level changes not run); the water box is sized from the surface model,
  not the exe's component. Ripple/caustic parameters are ours [I].
- Free climbing is implemented on the levels' free-climb holds (markup 16-20, see "Free climbing"): free movement
  inside the hold band instead of hold-to-hold, hold-type semantics [U], movement speed/reach [I], no
  bad-attach/wrestle/corner-specific clips, no `DICE_L/R/LW/RW` wall
  variants. Ladders (9), perches (13), pillars (15) and rail slides (8) work (see their sections); vertical poles
  (type 5) have no markup in any unit. Still missing: cover, `Wall_Hug_*`,
  balance wobble, beam sprint/catch, pole shimmy/parallel hang, `ledgeu_*` free hang, vaults over / down (hurdles onto low ledges are done),
  mirrored left-side clips. Root motion of the jump, attach and fling clips is not applied (the flight is
  ballistic), so their poses can sit off the computed path.
- Grapple is implemented for ceiling/post/wall targets (throw, pendulum swing, rope ascend/descend, jump off;
  see "Grapple"). Not done: wall-running/rappel, pulling mounted objects, rope-vs-world collision, the hook
  model, ledge-grapple transitions, exe rappel speeds (ours are [I]).
- Missing: sprint on stairs / slopes (`ng_sprint_up` / `_down`), sprint jumps and beam sprint,
  state-chart conditions (`PlayerSC_*`, `PSCCF_*`); combat: grenade crouch / perch / under-water
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
`l3_caverns_start`, so the target is the lowest-numbered start unit after the current chapter. Gate-by-gate routes and the
original end chains (preload volume, then the playing plane / unit callback): docs/PROGRESSION.md. Trigger volumes link to
these cinematics (l2 2564, l3_main_hall 4129, l4 5468/6407, l5 6866/6872); in our runtime the Activate of such a
cinematic plays its cutscene (its `cinematic` member names the stream, docs/formats/CINEMATICS.md) and then starts the
next chapter. The l1/l6/afterburn cinematics are reached through other scripts: l1_ship_sinking_hall plane 1074 /
volume 1082 → 1005, l6_ship_engine_hall plane 7565 → 1387, and l3_main_hall volume 4128 → cinematic 4108, whose
post-stream helper plays 4070 [O bytecode]; all run natively (docs/formats/SCRIPTS.md "Survey"). `--chapter N` starts
any chapter, `--cine NAME` plays any cutscene.

**Chapter starts vs the original** [O] (tru.exe under Wine, 2026-10-29: new game, then each start unit loaded by a
type-4 level change with the outfit override `+0x20C` cleared; captures, side-by-sides with ours (`--chapter N`,
Vulkan) and notes in openunderworld-work/screens/chapters):

| ch | start unit | original after load | ours |
|---|---|---|---|
| 0 | l3_main_bedroom | casual, corridor facing the fire; after `001_pro_corridor` (new game): its tail holds her at (−4336, 5847) facing +x for a 5 s corridor camera, then she stands on the player intro 4456 facing down the corridor, camera 1261 behind | same: a start cinematic tail within 500 of the start unit's player intro keeps the intro pose [I rule] (was: facing the side wall) |
| 1 | l1_seatop | wetsuit_warm on the boat deck, harness, no face mask | after `030_med_yacht`, standing on the stern deck (marker 13) |
| 2 | l2_cliffclimb | jungle_a on the boat bow, no enemy in sight | same (sharks 4021-4023 left the sea and landed on the deck) |
| 3 | l3_caverns_start | casual, cavern stairs | same |
| 4 | l4_trailstart | jungle_heavy, **riding** the motorcycle on the trail | after `180_mex_intro`, riding bike 5482 |
| 5 | l5_startsnow | snow_heavy, after intro cinematic 7530 **riding** in l5_valgrind_tunnel | after `230_val_intro`, riding 7229 in l5_valgrind_tunnel |
| 6 | l6_ship_exterior | jungle_b on the deck, rifle on her back | same (was black: the fade-in after the cutscene did not run) |
| 7 | l7_thin_ice | wetsuit_cold, after its intro the script changes level to l7_arctic_sea: **swimming** under water, mask on | after `275_hel_explosion`, swimming under the ice in l7_arctic_sea |
| 8 | l7_himalaya_end | ending cinematic, credits, main menu | the unit's player intro (no gameplay to compare) |

Chapter-start flow (ours, `StreamReal.ChapterStartIntros`) [O bytecode]: `World::startChapter` (new game, level end,
`ou-game --chapter N`; not a save load) sends id = chapter on channel `mc_CinematicData` as generalbank's game script
(type 6 fn 30, member +924). Message delivery [O]: channels are named (a `messagechannel` member's init is {mode, name};
0x4A3680 registers it by name), and each other referrer of that name with mode 1 or 2 (mode 3: only its own posts,
0x5D0670) runs its vt 0 OnMessage(channel, id, text), queued to the start of the next tick; script Sends queue too.
Ours had compared every channel equal (the mode word), so l4's start script re-entered STATE 1 on other channels'
id-4 posts and replayed `180_mex_intro` every tick. Unit scripts
without a state 0 still exist and listen (l5_startsnow 17775). The start unit's script enters STATE 1, which Activates
(vt 57) its intro cinematic_object (l2 2562 `080_tha_yacht`, l3 4318 `120_man_vault` on id 3, l4 5484 `180_mex_intro`,
l5 7530 `230_val_intro`, l6 7652 `260_ship_intro`, l7 9071 `275_hel_explosion`). World then sends cinematic event 0
(vt 45), plays the stream, places Lara and the bikes at the header tails (0x455D90: tail + the end unit's marker
intro; a (0, 0, 0) tail is none, 0x49AFA0), and sends event 1. Event-1 handlers: l4 larabase 52 on bike 5482, l5 0.6 s
later larabase 52 on l5_valgrind_tunnel's 7229 (thunk 0x79A4B0 -> 0x777DD0: INSTANCE message 0x85 to bike and Lara,
mounted at once); base 3523's "Lara warping to cine object" when the instance type's +140 is set (7652: `instance` 8
= its model-marker anchor 0x4AB320, `instance` 26 onto it: the deck, as the original after loading l6). Results:
ch2 on the yacht at its tail, ch6 on the deck, ch7 swimming (her tail under the ice is inside arctic_sea's water
box; placed deeper than the swim depth she stays under, SwimUnder). ch3: `120_man_vault`'s Lara tail
(-2704.7, -4321, -7.3) lies in l3_main_hall, not loaded beside l3_caverns_start; a tail over no loaded floor or water
leaves her at the cavern-stairs player intro [I]. The cinematic bases' end handlers then request the autosave
(`uisavemenu` 5: 0x7A4620 -> 0x77D880 -> 0x788A70(arg, 0, 1, -1), which requests it through 0x789E40(1); ours an
`Autosave` event, `World::autosaveRequests`) and fade in (`globaleffect` 6). Stopping at that call left ch6 black
(7652's handler never reached its FadeIn). ch1: 030_med_yacht's Lara tail is in the sea; cinematic object 997's
state 3 (fn 5) reads `uigamedata` 18 (0x77BB90: byte 0xFAE8DB, set by 17 / 0x77BB70; while set, level changes fade
to white, 0x5DAB40), then after 0.2 s puts her onto `uberobjectbase` 2's anchor (0x79D090 -> 0x4AB800: a
ModelMarkerAnchor, 0x4D3110, of ms_uo_yacht 998 at HInfo marker 13, 0x4D4820 -> 0x46DF20) with `instance` 26:
standing on the stern deck, facing the frame's -Y as intros (ours: `World::markerFrame`, `Player::spawn`, the camera
behind her [I]). Teleports and tails reset plane-crossing tracking
(`Runtime::playerJumped`; else the 7652 warp crosses kill plane 7650). Water boxes keep their half extents and
rotation when a unit is offset (only min / max / centre move): l7_arctic_sea's boxes failed at its offset before.
Stowed weapons are placed from the data as the original (speargun across her back in ch1, the rifle in ch6; weapon
data +0xB9 names Lara's stow marker, "Combat").

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
  centred, at ~0.66 brightness. Items: centre [O mainmenu, e4/e5] 49 TREASURE HUNT once the game is finished, else 122
  RESUME GAME with an autosave, else 123 PLAY GAME (icon 18 for all; RESUME / PLAY load the autosave, else the newest
  save, else start a new game), ring 124 START NEW GAME (PLAYER TAILORING first; with an autosave after 157 [I]), 125 GAME OPTIONS, 126 LOAD GAME, 127 SAVE GAME (main menu: message 161 "only … in
  gameplay mode" in box 106 with OK 383), 128 AUDIO/VISUAL, 129 EXTRAS (below), 131
  QUIT GAME. Navigation as tru.exe: Left from the centre to the last ring item (first time SAVE GAME, the one level with
  the centre), Right back to the centre, Up/Down along the ring and off either end to the centre.
- Autosave and profile [I]: a game started from the main menu autosaves (slot `autosave`) at every chapter start and
  checkpoint, and when a script requests one (`uisavemenu` 5, after the chapter-start cutscenes) (key: chapter +
  checkpoint + requests; loads do not re-save). `<saveDir>/profile.json` (`save::Profile`) keeps the
  chapters ended (a level end's cinematic), whether the ending played, and whether every treasure / relic was found by a
  chapter end (EXTRAS unlocks). The ending [O bytecode, docs/PROGRESSION.md chapter 7]: 342_hel_endgame's end handler
  (l7_monastery 8138 vt 60) requests the end game save (`uisavemenu` 6, our autosave of what was found) and plays the
  credits (`unlockablemovie` 3 "credits"); ou-game marks the game finished, plays `credits.bik` and returns to the main
  menu, whose centre is then TREASURE HUNT.
- Treasure Hunt [O start unit l1_seatop; I rules]: starts chapter 1 keeping the autosave's `collected` / `found` (what the
  finished game picked up stays picked up), `World::treasureHunt` set and saved (its autosave reads "<AREA> - TREASURE
  HUNT"); the PDA's REVISIT LOCATION is enabled only then and lists the chapters 52–58 (a PDA list page as JOURNAL
  ENTRIES, the chapter's loading-screen story 59 + n beside [I: not captured]); Return starts that chapter, finds kept.
- EXTRAS [O extras, extras_sel, extras_env, extras_gear, x0/x1; generalbank DTP 36 reward lists, `ui::loadRewards`]: as
  Load/Save, the ring stays (EXTRAS circled, the centre still labelled) and a mirrored ring at positions 2..6 holds the
  categories 146 ENVIRONMENT (icon 43), 147 CHARACTER (44), 148 GEAR CONCEPTS (45), 149 STORYBOARDS (46), 151 TRAILERS &
  CREDITS (48); Right/Return enter it (the centre is then unselected), the selected one's label white, left-aligned 39.2
  right of its centre on bar 13 mirrored. A category is a list screen: icon | name (title cap 75.1 from 228.4) and blue 129
  "EXTRAS" (cap 93.8, 0.72, from 247.6); the preview (uiImages[2], `pc-w\image\<image>.raw` 4:3, `locked_concept` while
  locked; 640×480 with their own white frame) at x 200.4..440, y 114.2..294.2; footer-wide lines 105 at 307.4 / 403.4; five rows from cap
  318.2 every 16.8 (scale 0.54, tracked 0.25; grey 192, the selected (194, 219, 234) on a (26, 77, 108) bar 16.9 high,
  solid ±115 fading to ±205): the name or 470 LOCKED from 147.6, "01".. right-aligned at 492.2, arrows 111 / 112 (0.56 per
  texel) left of the first / last row when the list scrolls; BACK TO PREVIOUS MENU (a label, Esc) centred at 316.4 cap 412.9
  (0.57, tracked 0.65); prompts 472 VIEW FULLSCREEN (468 VIEW MOVIE for trailers) / 375 at 271.8 / 403.8. Return on an
  unlocked image shows it 4:3 fitted over black [I: not captured], any key returns; on a trailer it plays
  `Movies\<movie>.bik` (previously → prevontr, teaser → intro, credits → credits) with the Bink player, from the pause
  menu after 471 and then back to the main menu. Unlock codes (`ui::unlocked`) against `profile.json`: −1 always; 3..10 a
  chapter ended (3 Mediterranean Sea, 4 the prologue, 5..10 Thailand .. Arctic Sea), also once the game is finished; 11
  the game finished (storyboards); 12 every treasure, 13 every relic (checked at chapter ends) [O codes, I meanings].
- Pause [O]: the same ring with 122 RESUME GAME in the centre over the game dimmed to ~0.56 (HUD hidden); SAVE GAME opens
  the Save screen, QUIT GAME returns to the main menu (after 494 [I]). No PDA item: the `pda` action (key Tab as
  tru.exe, rebindable as `controls.pda`; replays `pda`) opens the PDA.
- Confirmation dialogs [O dlg_quit, dlg_load(_yes), dlg_newgame, dlg_autosave_*]: box 106 over the backdrop alone (no ring);
  the question at 0.74 centred, wrapped at 356, caps from 164 every 16; YES 384 / NO 385 at 0.74 with their ink ends at
  302.2 / 339.6 and "/" at 320 (white); the selected one (181, 217, 255) on bar 107 (YES, right end 322.2) or 108 (NO, left
  end 318.5) at 0.6 × 0.83 per texel, white at 0.5, centred y 291.1; the other (76, 76, 76); NO selected, Esc does nothing,
  footer 120. The quit dialog (494) is laid out differently: caps from 168.9, ink ends 310.7 / 325, "/" at 319. Asked by:
  QUIT GAME 494 (main menu [O]; pause [I]), Load on a used slot 12 [O], pause START NEW GAME 154 [O], Save on a used slot
  16 and Delete 10 [I placement].
- LOAD / SAVE GAME [O empty slots]: the ring stays (the opened item: black disc ×1.17 with a white circle r 29.8, 1.3
  wide; the centre label stays) and a mirrored ring (items 2..7 mirrored about x 320.3): page up 27, four slots, page
  down 28, icons (114, 124, 165); empty slot 29, used slot 30 with its number (1.. over all pages) centred (+4.8, +5) at
  0.6 in the icon's circle (scaled with the icon). The selected slot gets a panel [O load_slot_filled, load_slots]: a black
  box from +23.6 right of the disc centre to x 585.3, 27 above to 26.3 below, with a soft blue rim (under the discs);
  a used slot's name in (89, 168, 239) at +31, cap −24.3, scale 0.57, tracking 0.94, cut at a word with "..." to fit;
  grey separators at −14.1 / +14.6; "<AREA>:" (chapter name 52–58) and the level title (386–407) at caps −7.4 / +2.8
  and the save's time / date ("HH:MM:SS", "DD-MM-YYYY", file time) at +29.7 / +82.6 cap +18.3, all 0.45 with tracking
  0.72. Names: the autosave (slot `autosave`) is "<AREA> - <TITLE>" (tru.exe 0x789770 `"%s - %s"`) and cannot be
  overwritten (message 17); a new save becomes `savegame-<n>` shown "Savegame <n>" (the original's default name; there
  is no name entry yet), older slots their file name. Level titles (`ui::levelTitle`): tru.exe keeps the current one in
  the scene object `*0xC59268 +0x1B0` (area +0x1AC; overrides +0x1B4/+0x1B8), set through a vtable (no table found);
  [O] l2_cliffclimb 393, l4_trailstart 398, l5_startsnow 402; [I] by unit: prologue 386; Med Sea seatop 387, seafloor 388,
  kraken / norse 389, baldrs halls 390, baldrs tomb / side room 391, ship 392; Thailand cliff / jungle hub 393, the
  hub / tower / shrine / puzzle rooms 394, norse hub 395, norse hall 396; Croft Manor 397; Mexico trail / temple 398,
  Xibalba stairs / ball court / trap rooms 399, pool / Ragnarok room 400, Mictlan stairs 401; Jan Mayen 402,
  Valaskjalf 403; Andaman Sea 404; Arctic Sea 405, Yggdrasil 406, monastery and the epilogue 407. Right/Accept enter
  the slots, Left leaves; Accept loads / saves (a used one is overwritten), Backspace / pad North deletes (11 / 9);
  footer string 121 split at its spaces, centres 193.6 / 433.3.
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
  Changed settings are written back to the `--config` file (launcher config: `game` volumes/subtitles/reticles/helper
  buttons/training text, `controls.invert_y`, `graphics.brightness`) and read at start.
- PLAYER TAILORING [O]: box 106 stretched to 672×237 at y 216 over the backdrop alone; title 484 cap 117.8 scale 0.8,
  line under it (y 139.6, 190 wide); 481/482/483 centred at 176.9 + 28.9·i, scale 0.66, selected white on a (30, 90, 127)
  bar (solid ±100, fading to ±182), others (203, 203, 203); footer line y 287 and prompts at 266.7 / 373.3 (scale 0.76).
  Picking starts the game from START NEW GAME with every level at the preset (Combat "PLAYER TAILORING").
- GAME OPTIONS > PLAYER TAILORING [O tailor_options, tailor_changed, tailor_changed2]: a list screen (icon 42, title 366 /
  125 GAME OPTIONS, rows from 164.4 tracked 0.25, unselected rows and values (78, 80, 81)) of 368 ENEMY HEALTH, 370
  AMMUNITION CAPACITY, 371 DAMAGE TO LARA, 372 TIME FOR SAVING GRABS and 469 BACK (367 / 369 are not on PC); Left / Right
  step a level 0..2 (0 the lenient end), shown right-aligned at 478.2 as 376 LESS → / 377 ← NORMAL → / 378 ← EXTRA, or for
  ammunition capacity and saving-grab time reversed 379 EXTRA → / 377 / 380 ← LESS (the arrows are U+180A / U+180B font
  glyphs); each change sets that level of the running game (`World::setTailoring`, saved with it).
- PDA [O]: `pda_bg_1280` (`_960` when the window is narrower than 1.556:1) fitted to the window height and centred; in
  bg pixels (virtual = bg·2/3 − 106.7, bg·2/3, the same for both widths): items 1074 SONAR MAP, 1075 AREA INFO, 1076,
  1077, 1078, 1081 JOURNAL / 1082 greyed (91, 91, 91, skipped; 1081 enabled with entries, 1082 in a Treasure Hunt), 122 RESUME GAME, right-aligned at 502.7, caps 173.3 + 43.3·i,
  scale 0.605, a line 17.4 below each from x 251.3 ((46, 86, 121); selected 2 px (68, 130, 184)); the selected item in
  (76, 139, 193) with arrow 114. Picture window (g_pda texture, tab bar included) at (544.7, 166) 1 px/texel, tab label
  right-aligned at 994 cap 360.7 scale 0.57: SONAR 688 + prompt 1085 (Accept: 1093 "unavailable"); AREA INFO 680 +
  chapter (680–687) and RELICS / TREASURES found/total at caps 442.7 / 478.7 scale 0.8 (x 602.7, values to 963.3) in a
  black box from 405.3 (line (16, 60, 66)) to 525; WEAPON SELECTION 689 + 1086, INVENTORY 690 + 1087, JOURNAL 693 + 1088,
  REVISIT LOCATION 694 + 1089;
  FIELD ASSISTANCE two boxes 155–344 / 357–544 with 1091 (Accept: hint) / 1092 (E / pad West: answer); prompts scale
  0.63, wrapped at 300, centred on the pane; footer 375 at x 289.8 and 120 at 621.3 (not on AREA INFO), cap 600. Totals
  come from `game::chapterRewards` [O survey of every l<N>_ unit's reward intros]: 10/22/7/10/10/0/19 treasures and one
  relic for chapters 1–5 and 7; found = those ids in `World::collected`.
- PDA sub-pages [V screens/pda captures; `ui::Menus` Screen::PdaSonar / PdaWeapons / PdaInventory / PdaItems; 3D views
  rasterised on the CPU (`src/ui/Pda.*`) into `uiImages[1]`, so GL and Vulkan show the same pixels]:
  - WEAPON SELECTION (1076): header 1100, the five secondaries in the original's order (76 SUBMACHINE GUNS, 77, 78, 80,
    79) + 469 BACK, the equipped one orange; right pane: document icon 698 + title + window buttons 699, the model panel
    at bg (573, 214) 422×163 (pda_bg_960: 160 px left) — ground (3, 4, 5), grid every 9 px through the centre (9, 32,
    36), border (13, 52, 58), inner frame lines 12.7 px in — with the weapon's model in a three-quarter view (barrels
    right, turned 45° towards the camera, 10° above, near-flat 10° lens, fitted to 88% of the panel) [I view, fitted to
    the captures]; AVAILABLE AMMUNITION reserve / max and CURRENTLY EQUIPPED YES / NO (1102 / 1103, 384 / 385) in the
    lower box; footer 1104 Return EQUIP.
  - INVENTORY (1077): BACKPACK / GEAR / WEAPONS / TREASURES (1101, 1235, 1100, 1251) with pictures 702–705 and prompts
    1236–1239; a category lists its items + BACK: BACKPACK = HEALTH PACKS [n] (picture 706, 1275) and STICKY GRENADE [n]
    while carried (1248 / 1249); GEAR = GRAPPLE, UTILITY LIGHT, FIELD CAMERA, SCUBA, PDA (1260–1269; the units' g_* gear)
    and Thor's gauntlets / belt while carried (1270–1273); WEAPONS = PISTOLS (1241 / 1242), the secondaries (1243–1247)
    and THOR'S HAMMER (81 / 1250); TREASURES = each area's treasures / relics found [I layout]. Items show their model in
    the panel, descriptions at scale 0.58 from cap 445 every 16.7.
  - JOURNAL (1081) [O j3..j5]: the PDA list page (header 1144 JOURNAL ENTRIES, nine scrolling rows, Escape BACK only) of
    entries 1145.., the selected one's text (id + 45) in the right pane; Return does nothing. Available: the entries up to
    the current chapter's (`ui::journalEntries`: 7 after the Mediterranean Sea, 14 Thailand, 20 Croft Manor, 31 Mexico,
    38 Jan Mayen, 39 Andaman Sea, all 45 in the Arctic Sea and a Treasure Hunt) [I: grouped by topic; tru.exe unlocks
    them by script].
  - SONAR MAP (1074; "unavailable" 1093 when the unit has no pc-w\<unit>_map.drm): every loaded unit's map mesh, grey
    by view angle with a teal world-space grid (250 units [I]) on black, Lara (`ping_map_icon_lara`, green) and bikes
    (`ping_map_icon_motorcycle`, grey) pawns, rendered at half the window size when the view moves; free camera behind
    Lara (reset on open and E), move keys turn / fly, H up, G down [I speeds]; overlay rulers (697), brackets, the curved
    bottom panel with E RESET VIEW (1140) / Escape BACK, G DOWN / H UP (1141 / 1142).
  - Reveal: the map shows triangles within 4000 units of `World::mapReveal` (a point per unit every 1000 units Lara
    moves, saved as `reveal`) [I]. The original marks per-instance reveal volumes (`PDAMap::RevealedInfo_Box` / _Sphere /
    _Capsule, serialised with the save at game state +0x590.. by 0x6E4410) containing Lara's position + 200 z
    (0x6E33D0, over the volumes of instances whose object +0xDD is set); those volumes' data was not decoded.
- Loading screen [O]: `loading_bg_<area>` (640×360) covering the window, black 0.69 outside the band y 88.9–390.2 and
  0.08 inside, white 0.3 band edges 2.2 tall, emblem 5 (345², 0.35); title 51 + chapter cap 220.4 scale 1.17, line 105
  at 242.4, story 59 + chapter at scale 0.66 from cap 256.9 every 13.15, wrapped at 540. Image per chapter [I by area]:
  mansion, medsea, thailand, dungeon (Croft Manor), mexico, snow (Jan Mayen), ship, arcticsea (and the epilogue). Shown
  for one frame before New Game, PLAY GAME / LOAD (the save's chapter) and after a level end's cinematic (chapter + 1).
Keys: arrows/WASD, Enter/Space accept, E = Alt (accept; PDA TASK), Esc back, Backspace/Delete delete; pad d-pad,
South accept, West Alt, North delete, East/Start back. Replays drive menus with `forward`/`right` ±1, `jump` (accept),
`interact` (Alt), `drop`/`pause` (back), `pda`; the script clock keeps running in menus. Not reproduced: the attract
mode (intro.bik after ~1 min idle), the decorative small text under
the list panels, save name entry (496), and the
footer always names the keyboard key.

**Menu audio** (docs/formats/AUDIO.md §9.11): the main menu plays the title music (generalbank DTP 78, stream
`music\tomb_raider_underworld_main_theme`, Music bus) and `medsea_loop.bik`'s own sound track looped with the picture
(Sfx bus [I]); both stop when a game starts or a movie plays and start again on the main menu. Every menu screen plays the
uiscreen scripts' sounds: selection moves DTP 214, accept / opening a screen 243, back 217, a value change 247 / 248
[I: one mapping for all screens]. The pause menu and the PDA hold the in-world sound and the music (tru.exe 0x5874B0);
voice lines and menu sounds play on. New Game, Continue, Load, quick load, Treasure Hunt and Quit to Menu stop every
in-world sound and the music first (0x593D10); a load within the same chapter restarts the track that was playing [I].

**Original UI reference (2026-10-28)** [O captures; implemented: HUD below, menus/PDA/loading above]: tru.exe captures at 1920×1080 (and
HUD at 1024×768) in openunderworld-work/screens/ui (`hud_weapon_*`, `hud_dpad_*`, `pause_*`, `game_options_1080`,
`av_1080`, `av_volume_1080`, `load_game_1080`, `save_game_1080`, `panel_player_tailoring_1080`, `pda_*_1080`,
`loading_1080`). Under Wine the paused game applies key presses several seconds late; wait ~4 s between keys.
- Default keys [O]: R draws weapons (shows the HEALTH and WEAPONS groups), Q/F show the GEAR d-pad, Tab opens the PDA,
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
