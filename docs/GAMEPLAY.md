# Gameplay: ou-game, Lara's controller, camera, input

Code: `src/game` (`Game.hpp`; `Player.cpp` controller, `World.cpp` world/anim set/camera/spawn, `Input.cpp` devices and
replay scripts), `src/app/game/main.cpp` (`ou-game`), tests `tests/test_game.cpp`.

Labels: **[O]** observed in game data, in our tru.exe decompile (GOG v1.1.0.0 VAs, `decomp_all.c`), or in a viewed
render; **[I]** inference; **[U]** unknown.

## Running

```
ou-game --game-dir "/path/to/Tomb Raider Underworld"            # main menu (New Game = chapter 0)
ou-game --game-dir DIR --level 'pc-w\l4_pool.drm'                # straight into a level
ou-game --game-dir DIR --chapter 4                              # start a chapter (0-8)
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

Controls (defaults; full table and rebinding: "Controls" below): WASD/arrows move relative to the camera, Left Alt
walks, Left Shift sprints, Space jumps (while hanging: climbs up, or with the stick jumps up/sideways/back to another
ledge; on a pole: flings), Left Ctrl lets go of a ledge/pole and on the ground crouches (held), rolls (running) or drops
into a hang at an edge, E uses switches / pushes and pulls blocks (with forward/back held), mouse orbits the camera.
Gamepad: left stick, right stick, South = jump/climb, East = drop, North = use, a light stick push walks, left shoulder
sprints. F1 toggles the overlay, Esc / pad Start opens the pause menu ("Progression" below; without `--level`/`--chapter`/`--load` ou-game
starts at the main menu). `--fov` / the launcher config set the vertical FOV. Switches, doors, trigger volumes/planes and
blocks: `src/script`, docs/formats/SCRIPTS.md; script events are logged to stderr. Supported handlers execute their
original SCMD bytecode, including nested calls, strings, sticky/modifiable state and SLEEP/WAIT resume across ticks;
unsupported natives use the static-link fallback.

Input scripts: one event per line, `<seconds> <action> [value]`, `#` comments. Actions `forward`, `right` (stick
axes −1..1), `walk`, `jump`, `drop`, `interact`, `save`, `load` (0/1, default value 1; `save`/`load` = F5 quicksave /
F9 quickload, docs/SAVES.md), `lookx`, `looky` (camera rate, rad/s). A value holds
until changed; an event at `t` applies from tick `round(t · 60)`. With `--input-script` each rendered frame advances
`1/--render-hz` seconds (default 60), so `--frames N` screenshots are reproducible. At the end, `--frames` prints
`frame time mean / p95 / max` (wall time per frame) and `tick time mean / median / p95` (`World::tick` CPU time) with
its script runtime, physics step and `Player::tick` means, all after 30 warm-up frames; with `--no-vsync` they compare builds.

Audio (`ou_audio`, docs/formats/AUDIO.md; `--no-audio` turns it off): lara.drm Wave sections are registered and the
unit's music stream `pc-w\music\<unit>_mus_01\<unit>_mus_01.mul` loops (all stems at gain 1; [I] the naming is seen
in the bigfile, how the game's Mus! sequencer picks and mixes tracks is [U]). Footsteps and every other character sound
come from the animation event lists, picked by the floor's surface material (AUDIO.md §9.7). Sound events (sfxmarker
ambience, script sounds, object SoundComponent loops such as waterfalls / fires / gates / gears, physics joint sounds of
moving doors, gates, levers, cogs and press plates (their joint records' graphs fed the joint's speed, an impact at a
limit, AUDIO.md §9.16), drowning line, T / Y help-on-demand Hint / Answer lines with subtitles; input-script actions
`hint`, `answer`): docs/formats/AUDIO.md §9. Script sounds played as sequences hold their thread until they end (§9.3).
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
| HangJump up / back / side | `ledgeb_jump_up` (905) / `ledgeb_jump_back` (903) / `ledgeb_jump_right` (894) | 5, 0.37 s / 7, 0.23 s / 5, 0.17 s | up: not applied (ballistic flight); back / side: the take-off, played with its root motion before she leaves (61.6 out + 32.2 up / 122 aside + 107 up; root rot Z −π / −π/2 drives the yaw) | no |
| Hang (after a jump) | `ledgeb_attach_U` (213) / `_F` (208) / `_S` (210) | 8 / 8 / 9 keys | ignored (end pose = hang pose) | no |
| PoleHang (grab) | `Hpole_perp_attach_M` (1541) | 45, 1.5 s | ignored | no |
| PoleHang | `HPole_idleHangPerp` (127); swing `HPole_StartSwing` (143) → `HPole_swing` (146); stop `Hpole_swingstopA` (152) | 30, 1.0 s; 70, 2.33 s → 41, 1.37 s; 41, 1.37 s | none | idle/swing yes |
| Pole fling | `hpole_perp_fling` (423) | 7, 0.5 s | (−1.1, −382.6, 69.5) not applied (ballistic flight) | no |
| ClimbUp (obstacles, `Player::tryHurdle` / `tryHurdleDown`) | lara.drm anim requests (blend spaces, the second clip weighted by height / depth): 127 `hurdle_ground_attach_low_50`/`_100`, 128 `_mid_100`/`_150`, 129 `_high_150`/`_200`; 132 `hurdle_low_onto`/`_100`, 133 `hurdle_onto`; 130 `hurdle_low_over`/`_100` (long: `hurdle_over_low_long_50`/`_100`), 131 `hurdle_over`/`_long_100`; 134 / 135 `hurdle_to_ledgeb_low_*` / `_mid_*` (25 / 65 / 100); 139 / 141 / 140 `hurdle_down_*`; 147 / 149 / 148 `*_cancel` | attach 0.17 / 0.33 / 0.57 s; onto 0.3 / 0.8; over 0.5 / 0.6; to_ledgeb 1.0 / 1.13; down 0.3 / 0.63; cancel 0.3 | blended between the request's clips (`PlayerState::clipMix` / `mix`, phase-synced as tru.exe 0x4428A0); onto / down rise scaled to the top / floor, over drawn to her floor, to_ledgeb drawn onto the far hang pose | no |

- [O] No exe string names these clips; they are the TR8 anim-list debug names in lara.drm. The state → clip choice is
  ours [I]. `ledgeb_*` (braced: feet on the wall) was chosen over `ledgeu_*` (free hang) [I].
- Hurdles [O-runtime] (tru.exe under Wine, new game, l3_main_bedroom = ours + (9622, 333, −2977); traces in
  openunderworld-work/screens/prologue): running down the prologue corridor into the 253-high debris slab (ledge 974,
  34° off square) she climbs onto it **without a jump**: the root is ~64 in front of the ledge line when it starts, the
  rise takes 0.27 s and ends exactly on the ledge height, ~220 forward, and she runs on. The 121-unit hearth has no
  ledge markup and stays a wall; the 725-high wall at the far end of the left corridor (ledge 973) is taken with a
  standing jump from the rubble in front of it (50 and 101 steps), a ledge grab and a climb-up. Ours
  (`Player::tryHurdle`). Blend spaces [O-data]: the requests above (object DTP +0x60, 0x24-byte records, `+4` clip list,
  `+0x10` count; dumped from lara.drm) name heights / depths in cm, 1 cm = 3.1605 units: the exe's own floats
  158.02501 / 316.05002 / 474.075 / 632.1 (0x707C60, 0x72CCD0, 0x7396E0). Bands (the ledge class table below):
  low 154.87..312.90 (132 / 130 / 134 / 139), mid ..474.08 (128 / 131 / 135 / 141), high ..635.27 (129 / 133 / 140),
  replacing our old 340 / 650 guesses; the weight is the position on the request's axis. The Wine slab confirms request 132:
  0.267 s = its 0.3 s from start frame 1, ~220 forward = the blend's 237 at 253 high. Clip geometry [O-data, our
  probe]: attach clips have no root motion (mid / high hands end 103 ahead on the edge); `hurdle_over` / `_onto` /
  `_to_ledgeb_mid_*` start in the mid attach's end pose; `_to_ledgeb_*` turn half round and end in the hang pose
  (forward 332 / 459 / 571 for 25 / 65 / 100 cm deep tops); over forward 640 / 860 (low), 594 / 722 (mid); down
  forward ~310 (low) / ~360. 0x759260 / 0x7594C0 are not the ledge grab: they are the `PSCCF_WallJumpAnticipate` /
  `WallJumpAttach` / `WallJumpCollide` conditions (wrappers 0x763330 / 0x7633E0 / 0x763490).
- Player state chart [O-data + O] (lara.drm object data = ObjectBase +0xC8 block; dump tool
  openunderworld-work/hurdles/sc.py). Header +0x12508: u16 states 1445, u16 transitions 2374, ptr states[], ptr
  transitions[], ptr events[] (69); the runtime chart object is 8 bytes earlier (0x12500: tru.exe reads +0xC / +0x10
  / +0x14); a second chart at +0x12530 (103 / 203 / 14). State (0x28): +4 s16 anim request (played on entry,
  0x783470), +6 s16 parent (transitions inherited, 0x787570), +0xA / +0x14 properties `{u16 id, u16 type, u32 value}`,
  +0xC / +0x18 entry actions (u16 ids into the PSCPF table 0x8B0A50 `{id, f1..f6}`), +0xE / +0x1C transition indices,
  +0x10 / +0x20 events. Transition (0x14): +0 s16 target, +2 / +8 flags, +4 / +0xC conditions `{s16 kind, s16 id, s16
  value, u8 negate}` (all must hold, 0x785920); the condition registry is `{id, group, fn}` at 0x8B2CE8 / 0x8B2838.
  For obstacles:
  - Ground locomotion (S724 / S730) runs markup query 96 (property 0x21D). The winning ledge is classed by the ledge
    markup table (FUN_005D5930 → objdata +0x116B0 → +0x90 / +0x94 = objdata +0x121C0, 18 × `{class, min, max, kind,
    test}`, evaluated in order by 0x728650; kind 1 / 2 test 0x7260B0 on the floor 94.815 out from the ledge point):
    floor 154.87..312.90 / ..474.08 / ..635.27 below the ledge = class 5 / 6 / 7 (up), the same past a ledge she
    stands on (ledge < 6.33 above her feet, facing it) = 8 / 9 / 10 (down); each also needs test 0 (0x727BF0,
    clearance; we apply its over-the-top and standing-column segments, not its first one [U: frame]). Class 5 on the
    ground → S958 with no attach clip (low onto / over / far-side hang); 6 → wall_collide_mid only; 8 / 9 / 10 →
    S996 / S995 / S994 (requests 139 / 141 / 140). Mid / high (6 / 7) climb only from the jump states (S289, running
    jump S775 → S959 / S957 attach mid / high); a low obstacle jumped at is S958 again. So the jump is required for
    mid / high and not for low (Wine: the 253 slab).
  - The hurdle parent S954 runs action 0x201 = 0x73DFF0 on entry (tuning objdata +0x530): rays along her heading
    from 6.32 behind the ledge point to 474.08 past it at 237.04 and then 31.61 above it (a hit within 237.04 = class
    −1, beyond = 1); else a down ray 353.3 past the point from 37.9 above to 53.7 below it: a top = 1 (onto); none →
    query 100 for a ledge along the far end: found = 2 (hang on the far side) unless a floor lies within 632 below 158
    past it (then 0), not found = 0 (over). Condition 0x1D1 (player +0x6C) then picks S971 / S981 (0 over), S974 /
    S980 (1 onto), S966 / S983 (2, jump target a ledge); −1 falls through to onto. 3 / 4 / 5 are the same with flair
    clips when 0x72F360 finds room (not played).
  - Positioning: anim target 0x273 (0x73DD70, property 0x19A = 627): root at the ledge point + property 0x119 out
    (S954: 86.94) at her own height (0x11A = 0); down (S997): on the line, heading the normal + 180 (0x11C).
  - Blend axes (properties 0x0E5..0x0EA, floats): attach / onto / down 158.1..316.2 / 316.2..474.1 / 474.1..632.2 by
    height; far-side hang 79.06 / 205.4 / 316.2 by depth; over 316.2..632.2 × 158.1..316.2 (which distance the first
    axis is: [U], ours root to the top's far end).
  - Cancel: stick back (condition 0x192 MoveDir 411) during the attach → S961 (request 149 / 148 / 147).
  Ours (`Player::tryHurdle`, `tryHurdleDown`, `hurdleClass`): the above, measured from the face under the ledge line
  instead of the line. (l4_pool rock 1253 used to be the exception: 14 faces 8.5 over the rock top, reaching ~180 out past
  its face, made the class test call it no hurdle. They are terrain group 3, the enemies' and the camera's, not the player's
  (docs/formats/COLLISION.md, per query class); without them a standing jump there is the high hurdle onto the top at the
  ledge's own height 3562.5, `GameReal.PoolRockIsHurdledOntoItsTop`.) [U]: query 96's reach (ours:
  the root within radius + 40 of the face, radius + 120 jumping) — the query sweep is v·t + ½gt² over 40 frames with v
  from her velocity scaled by player +0x6E4 (DiceJumps), not measured yet; the far-ledge search radius of query 100.
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
| Walkable slope | by face: unflagged faces of normal z ≥ cos 52° = 0.6157 are stood on; faces with client flag 0x0004 / 0x0040 are slid down between 52° and 28° (normal z 0.6157..0.8829) and stood on when flatter; 0x0020 faces are never too steep, 0x0100 faces are stood on however steep. A too-steep face is still stood on when one of four probes 60 units around the root finds a standable face within 20 units of its height (bumps, seams) | [O] tru.exe ground check 0x5DCE20 with lara.drm player data shorts +0x100C = 28, +0x100E = 52 and the face's client flags; [O-runtime] Wine ramp series in l2_junglehub (section "Slopes and slides"). [I] the four-probe exception (ours) |
| Slide | on a slide-flagged face: along the fall line at 35..45 units per 30 Hz frame (1050..1350 units/s) by the slope angle 29°..51°, approached at 2250 (up) / 4500 (down) units/s²; on a too-steep floor (normal z 0.3..0.6157): accelerated by gravity along the slope (g·sinθ), no friction, no cap; the descent rate is carried into a fall | [O] slide controller 0x73FF50 with the props of chart group S814 (0x204 / 0x205 / 0x1FE / 0x1FF); [O-runtime] Wine: +1.25 units per frame each 60 Hz tick, steady 35.0 .. 43.8 per frame on twelve 0x0004 faces of 28.4° .. 48.3° in l2_junglehub (the 29°..51° range is their fit); l3_main_bedroom roof, normal z 0.45, trace slide1.txt: speed 51.0 → 69.0 → 91.3 units per 30 Hz frame over 0.25 s = ~4.7k units/s² against g·sin 63.3° = 4601, no cap up to 2738 units/s. [I] the 0.3 lower bound |
| Edge slide-off | a capsule resting on an edge corner (floor-like contact, no floor under the root) gets ≥ 200 units/s outwards | [I] ours (fixes the Fall/Land alternation walking slowly off a ledge) |
| Crouch capsule height | **260** | [O] player data +0x14 |
| Turn rate (walk/run steering, swimming) | 6 rad/s; surface swimming at most **2765 / speed** rad/s (**117.5°/s** at full swim speed) | [I] from `ng_turn_center`; turning in place uses the clip's root rotation instead (above). [O-runtime] surface swim: tru.exe under Wine (l1_seatop sea, camfollow-wine/turn1.txt, rotz of Instance +0x30): with the stick swung round faster than she can follow she turns at a steady 117.5°/s at full speed, much faster from a slow swim (~300–480°/s at ~450 units/s) [I: the 1 / speed form; under water still 6 rad/s] |
| Ledge grab | hands from 60 units short of the line to anywhere over it (the root on her side; within 60 along it past its ends), ledge between hand height −40 and +10, falling, facing the ledge (cos ≥ 0.5), **−2232.5 ≤ zVel ≤ 1545.6 units/s** [I: the window is the one below, from the wall-jump test]; a standing jump whose query (q2) class tests meet a ledge more than 635.27 over her feet instead leaps straight onto its hang (`Player::standJumpToLedge`: line within 424.5, 782 / 830 ones out to 424, 892 / 907 ones only within ~108; see "Air-grab window") [O] | Correction (2026-10-31): 0x759260 / 0x7594C0 are the state-chart conditions `PSCCF_WallJumpAnticipate` / `WallJumpAttach` / `WallJumpCollide` (registry 0x8B2840; wrappers 0x763330 / 0x7633E0 / 0x763490 name them), not the ledge grab. What they do [O]: zVel window `−byte[data+0x834]·5.72443 ≤ zVel ≤ byte[data+0x833]·5.72443` (bytes 13 and 9, units per 30 Hz frame); 0x758DD0 gives the pelvis (segment 1) position and its prediction N frames ahead (N = byte +0x820 = 0, resp. +0x821 = 3), both raised by int +0x824 (**+63**), swept with collision query 0x4935F0 (mask 0x2C19E7); a kind 3 / 5 hit accepted by 0x5D3BA0 / 0x758F70 is repeated at int +0x828 (**−158**); 0x7591E0: no floor within int +0x82C (**31**) below the root; 0x758B00: within float +0x838 (**951.3**) of player-state +0x9C0. The ledge grab's own test is in the state chart (not decoded) [U] |
| Ledge targets (drop to hang, hang jumps) | the hang side must drop > 200 within 240 units of the ledge line | [I] ours; l4_pool's ledge 1253 sits up to ~180 inside the collision top, so a fixed 60-unit probe misses the drop |
| Balance beam | root within 30 (x/y and z) of a type-14 line, heading within 45° of it from the ground (any heading from the air); walk 140 units/s (clip) | [I] attach distances ours; [O] markup type 14 = MUD 0x10000 BALANCEBEAM |
| Jump assist | at take-off, of the jump-landing (2) and beam (14) lines the point nearest the natural landing (at its height) within **300** becomes the landing: horizontal velocity = Δxy / flight time, vertical unchanged | [O] solver 0x73EDC0: `v = (target − pos − ½·g·t²) / t` with g = −5.72443/frame², re-aimed every frame with the remaining time (0x751D10, player-state +0x77C) at the markup in +0xA64 whose MUD flags match 0x20830000 (jump landing, rail slide, perch, balance beam); speed cap player data +0xC10 = 18963 (never reached). [I] the 300 radius and solving once at take-off. Ground jumps: the take-off state's query (S248 standing q2, S245 / S247 q43, S249 running q26) meets ledges as the "Jump query hit test" (`Player::ledgeOnArc`, `aimGroundJump`) — its own arc's capsule — and the flight is re-aimed onto the hang pose by the hit frame; only ledges 158 or more over the root count [I: lower ones are landed on, vaulted or hurdled]. Standing jumps at higher ledges (635..907 over her feet, up to ~410 out) are a straight 11-tick leap onto the hang ("Air-grab window", `Player::standJumpToLedge`). Wine: under the prologue slab (ledge 974, 1100 ahead) a running jump left at the plain 65.83 (no re-aim); the reach q26 gives a running jump (l2_junglehub ledge 851, `GameReal.JunglehubRunningJumpReachesHigherLedge`) is [I] |
| Hang jumps | from a hang (up: stick forward/neutral, no top to climb; back turns her round): the first ledge met by the hang jump query from the attach point — q10 up (from 79 out, 75.85 up a frame, coming down, cone 65°, record 0x2A9EC), q7 / q8 sides (from 474.1 under and 63.21 out, the hands' arc from the attach point, 70.80 aside + 34.77 up, cone 45°, the hit **553** or more aside, record 0x2AA4C), q9 back (the same starts, 57.33 out + 42.54 up, cone 90°, record 0x2A9EC); classes 6 and 8..13 refused ("Jump query hit test"). Back / side play their take-off clip first (root motion), then launch: vertical speed **75.85 / 34.77 / 42.54** a frame (up / side / back), flight time when that rise and fall reaches the target's height (rising for a target above), horizontal speed set to arrive exactly on its hang pose; a target above that arc rises 60 above the higher end. With nothing to jump to: back / side leap off at **53.73 out / 70.80 aside + 34.77 up** a frame after the clip | [O-runtime] free leaps: tru.exe under Wine, l3_main_bedroom ledge 973 (`GameReal.HangLeapsMatchTruTraces`): back leaves 0.23 s after the clip starts, 61.6 out and 32.2 up of the hang, rise 105.6, 932 to the corridor floor in 0.578 s; right leaves after ~0.15 s, ~122 aside, turned a quarter round, 70.80 + 34.77 (= queries q7 / q8). [O-data] launch speeds of the jump queries q10 / q7 / q8 / q9 (lara.drm, states S254 / S257 / S259; dice/queries.py), solver 0x73EDC0. [O-data] the queries, their records and test lists (decoded, tru.exe 0x7296C0 / 0x728F00); [I] the attach point = the ledge point she hangs from, flight time (the exe times it by the query arc's first markup box hit) |
| Poles | types 4 (`pole`) and 6 (`horizpole`); hands (pole pose) within 60 of the axis, rising or falling within the ledges' zVel window, from 40 below to **138** above the axis, facing across it (cos ≥ 0.5). Fling (and pole-top jumps forward / back): to the nearest pole or ledge 300..1800 ahead (±300 aside, ±500 height) in **23.5** frames — a pole's / a bare ledge's hang, a ledge with a floor on top: landing one capsule radius past its edge — else q33's free leap | [I] ours; the 138 [O-data, I meaning]: `Hpole_perp_attach_M` starts with the hands 481 above the root against the pole pose's 619, so the catch pulls her down 138 onto the hang (a standing stick jump at the measured speed passes l4_traprooms pole 1336 with the pole-pose hands 62 above it); l4_traprooms' poles are 1264 apart; [O] the markup's matrix carries real translations there (COLLISION.md) |
| Water probe | box ±221 × ±221 × ±572 centred 320 above the feet | [O] shorts data+0xC3E/+0xC40/+0xC42 (221, 572, 320), box overlap against each water volume box in 0x710A20; the highest surface wins |
| Swim speed | **1348.6 units/s** (surface and under water), reached at **2475 units/s²**, slowing at **3760 units/s²** when the stick is released (she coasts); at the surface the velocity is steered after her heading as a vector at those rates, so it lags her heading in a turn and dips in speed (Wine turn1.txt: ~16° behind her heading while she turns at 117.5°/s; ours ~9° in a 90° turn) [I: the vector form] | [O-runtime] tru.exe under Wine (l2_cliffclimb sea, traces swim_l2b / swim_l2c): 44.95 units per 30 Hz frame at full stick both at the surface and after a dive; from rest 0.54 → 8.3 → 33.1 → 44.3 per frame at 0.05 / 0.2 / 0.5 / 0.87 s (S-shaped; the linear slope is fitted to its middle), after the release 44.6 → 21.6 in 0.18 s. [I] linear ramps. The old 562 (root speed of `swim_surfaceidle_to_swim` / `swim_surfacedive`) was the clips', not the controller's |
| Swim depth | water at least **391** deep (surface − floor) is swum, shallower walked | [O] tru.exe 0x700530 → 0x5D77E0 → 0x5D3860 classes the water with lara.drm player data shorts +0xAC/+0xAE/+0xB0/+0xB2 = 154 / 391 / 391 / 948 (every outfit): floor depth < 154 → 0x200, < 391 → 0x100 (wading), else 0x40 (at the surface) / 0x20 (root ≤ 391 under) / 0x10 (≤ 948 under); dry 0x400; stored at player +0x8EC (query 9, SCCondFunction_InWater 0x7824F0 tests 0x400 / 0x4800 / 0x3000). Ours: class 0x100 plays `wading_idle` / `wading_walk` (walk pace at any stick), landing in it `swim_land_wading`, standing up into it from swimming `swim_to_wade`; class 0x200 keeps the on-foot clips [I: the wading graph; `wading_walk_low`, the wading turns, jumps and combat clips unused] (`Game.WadingClipsAtTheDecodedDepth`) |
| Swim sprint | **1.2 × swim speed = 1618.3 units/s** while Shift is held (surface and under water), same ramps; stamina shared with the run sprint (`sprintSeconds` 9.7, `sprintResume` 0.9); at exhaustion the exhausted clip plays once at normal speed without refill | [O-runtime] tru.exe under Wine (l2_cliffclimb sea, 2026-10-30; player velocity +0x2A0, traces in openunderworld-work/scratch/swimsprint): 53.94 units per 30 Hz frame sprinting vs 44.95, at the surface and ~2000 under it; 44.8 → 53.4 in 0.36 s after the press; with Shift held 9.95 s of sprint, then 2.13 s at 44.95 and 0.95 s bursts (the run sprint's 9.5..9.9 / 2.2 / 0.9 s). Clips [O-runtime, rms 18.7 vs 21+ for any other clip, z of the body bones over 14 snapshots]: the surface sprint pose fits `swim_surface_sprint_forward` (= `swim_sprint_butterfly`), lara.drm request 896; under water request 934's `swim_submerged_sprint` [I: pose not compared]. Ours: `Tuning::swimSprintFactor`, `Player::swim` |
| Water drag (splash) | velocity × e^(−3 t) | [I] ours |
| Breath | **4000 frames = 133.3 s** under water (**× 1.5** drain in the under-water sprint; **no drain with scuba gear**, "Water moves"), refilled from empty in **60 frames = 2 s**; once spent, **10 %** of max health (damage type 3) every **20 frames** (every other one lands: 40) | [O] tru.exe 0x755230 (under water, while state-chart variable 0x322 is set) / 0x756C40 (surfacing: a spent meter restarts at 0; sounds +0x94 / +0x98 = 962 / 963 [U: not played]): player data +0xF8 / +0xFA / +0xFC / +0xFE; the HUD gets the percent (0x5D5100(0x2E)), death type 8 (ours: `death_drown` when the periodic damage kills her in the water [I]). Buoyancy of bodies: water density 4.556e-5 kg/unit³ = 1000 kg/m³, linear drag 0.5, angular 0.01 (0x69D350 defaults, scaled per object by physics property bytes [I: 1], 0x6AD070). Ours: `physics::Scene::setWater` gives every loose body in a volume Jolt's `ApplyBuoyancyImpulse` with factor 1000 × volume / mass, drag 0.5 / 0.01 and the current as the fluid velocity (`Physics.LooseBodiesFloatOrSinkInWater`); ragdolls not [U] |
| Fall damage | 0 up to **1280.03**, linear to 500 at **1817.32**, linear to 1000 (= death) at **2215.55**, death above | [O] floats data+0x4C/+0x50/+0x54 and the curve 0x6F1790; height = z on becoming airborne (player +0x6DC, set on FallState entry 0x75C550) − landing z; applied on landing (0x75D080 → 0x702FC0), never scaled by difficulty (Combat below) |
| Camera | Orbit at the mode's distance (**1264.22**; hanging and ladders **1896.3**) round a pivot; its yaw/pitch are read back each tick from where it ended up relative to the moved pivot, so it **drags after her** (a held sideways stick circles her round the camera; running away flattens a looked-down pitch; climbing up a wall leaves it below her, pulled in by the rock) and keeps whatever the collider swung it to; rest pitch **−0.0225 rad**. Pivot = focus after the clearance; focus goal = bone **108** (a helper the clips carry: 320 over her feet standing, by the pelvis ~95 off the rock climbing) through the mode's focus record: **(0, 0, 142.23)** on foot, **(0, 0, 79.01)** free climbing, **(0, 63.21, 316.06)** (63 out behind her) hanging, **0** under water; it lags its goal per axis of the camera's horizontal frame by dead zones **3.16 / 126.42 / 252.84** (none under water): past a zone caught up at once, the rest at up to **1896.3** units/s along the record's 64-step ease curve; a new record blends in over 0.5 s, new limits over 0.25 s. Clearance: when the **474.075** above her root is blocked the pivot is pulled toward bone 1 to keep **27.82** clear. Distance returns at up to **4741.4** units/s, accelerating **1106.36** /s². Collider (r **13.91**): the proposed point stands if a sphere swept from the pivot reaches it; else the straight pull-in competes with the smallest swing round the pivot that clears (45° about the vertical, the way it was moving 10 bisection steps, the other way 5; 25° up/down 5 steps; those three cost 5×): the swing wins when cheaper than the pull-in. Climbing, hanging, ladders, perches, pillars, beams and poles use only the pull-in (contacts nearer than 10 ignored, never nearer than **97.98**). On foot (and on beams) a ray probe pulls it in to what it sees, never under **300**. What it collides with [O]: terrain faces through mask 2 (0x5EB9B0 → 0x5EAF60 → 0x462E10), skipping terrain groups whose TerrainGroupExtra flags have **0x200** (0x5EB9B0 / 0x5EC360; its rays 0x493330 → filter 0x48EF50 skip 0x300): 21 903 faces in 76 groups over the 82 units, e.g. l1_kraken_entrance group 3 (5667 faces), l3_chapel group 5; and only objects with ObjectBase oflags bit **0x02** (0x5E94C0 / 0x5E91D0 / 0x5E8940: 101 of the 335 solid placed objects, e.g. big doors and towers, not poles, gears, breakables or pickups). Ours: `collision::kCameraMask` [not modelled: the rays' extra 0x100-only skip, 5 groups]. Look limits: climbing, hanging and ladders yaw within **90°** of her heading (camera on her back side), pitch **±55°** (pillars pitch ±55°); surface swimming pitch **−70..−10°**, under water **−85..75°**; on foot our −1.2..0.7 rad [I]. Forces (after **5 s** without look input, full at 7 s; never while the camera is turned): each camera mode has a yaw and a pitch force set (generalbank DTP 0x1C / 0x1D; the mode list's command 0x10 / 1, else yaw set **11** and no pitch set) turning the angle toward the set's target at speed × the product of its forces. On foot, swimming and on ladders (yaw set 11) the camera **swings round behind her at up to 180°/s × curve(angle off behind / 180°) × min(1, 0.02 × her move per frame across the view)**: the curve is 0 within ~20° of behind or of in front and peaks at 0.6 at her side, so it swings only while she runs or swims sideways to the view (≈ 0.6 × 0.42 × 180 = **45°/s** running at her side), never while she stands or runs straight away; free climbing / hanging (set 15) **10°/s** × min(1, 20 × her mean move), toward **60°** off behind her on the side she moves along the ledge (sign 0 = behind her until she moves); on foot pitch toward **−7.5°** at **5°/s** × min(1, her move); ladders and pillars pitch toward **+45°** while she climbs up / **−45°** while she climbs down at 5°/s × curve(|stick|); poles 15°/s × curve(|stick|), perches 70°/s (pitch −10° at 70°/s × min(1, dt)), pillars 20°/s × min(1, 0.1 × her move), beams speed 0. A mode change blends the sets over 0.25 s (rate × f × (1 − f) × the old set's strength meanwhile). Ours logs `camera swing starts / stops`. [O-runtime] tru.exe under Wine (l1_seatop sea, camfollow-wine traces swim_right*.txt): yaw controller camera +0x8F8 = set 11 (2 forces, speed π), pitch controller +0xDA0 = set 4 blended out (B none) at the surface, side flag +0x4A4 = 1, look clock +0x1E4 not reset by the move stick; her move history 21.97 a frame at 1318 units/s (60 fps); the camera's x row = its right (row 2 = the look); holding the stick right after a mouse nudge the camera circles her at **60°/s** for 5 s, ramps, and holds **101°/s** from 7 s (the swing adds 41°/s against 41–43 predicted by set 11 from her heading and velocity there). **Level camera records**: on a markup carrying one (COLLISION.md "Camera records"), in a mode the settings list for the markup's type, the camera turns toward the record's point (a Hermite spline point chosen by her markup) at speed × strength (l2_cliffclimb's hold wall: 300°/s × min(1, 0.2 × dt) = **1°/s** at 60 Hz) and its distance toward the point's (948.17 × the same = 3.16 units/s), after the same 5 s look delay, instead of the forces and the distance return. It follows its point at **3793.1** units/s plus how far the proposed point moved. Bike: 1711 [I] | [O] LaraCameraRewrite (runtime vtable 0x85600C; update 0x602690; angles 0x5FF320; distance/forces 0x5FFB00; proposal and probe 0x600200; follow 0x600710; focus 0x609830 / 0x609440 / 0x607D40; clearance 0x5EA050; collider 0x5ED010 → 0x5EC5B0 / 0x5ECCE0 / 0x5EC8F0 / 0x5EC660, pull-in 0x5F0810; limits 0x609C00 / 0x607A30 / 0x608700; forces 0x608B60 / 0x5F2180 weighted by 0x5FFA90; records 0x601D30 / 0x5FE800 → 0x606690, point 0x606160 / 0x605ED0 / 0x605A20 / 0x5F7340, pull 0x606B00 / 0x606310). Settings DTP = global data (*0xE7EE50) +0x144 = 0x14: +0x60 distance, +0x68 radius, +0x6C / +0x70 return cap / acceleration, +0x74 follow speed, +0x78 record blend 0.5, +0xC4 / +0xC8 force delay 5 / ramp 2, +0x50 focus records (0x6C bytes: bone, offset, zones, speed, curve), +0x148 mode list (0x5F99F0 / 0x5FF080 commands: 2 distance, 7 / 8 yaw / pitch limit, 10 pivot record, 15 pull-in only, 0x10 / 1 yaw / pitch force set in global data +0x164 / +0x168, 0x13 floor probe; defaults 0x601D30 / 0x5FEE10), +0x14C markup type → modes (ledge 1 → 11 / 14 / 15 / 42, 37–39, 21 / 22; beam 14 → 3 / 43; ladder 9 → 10; perch 13 → 16; pillar 15 → 17; pole 4 → 9 / 3; holds 16–20 → 4). Mode = CameraManager 0xE804F0 +0x54, requested by the player's state machine with message 0x44E (0x7683B0 → 0x5E8210). [O-runtime] tru.exe under Wine (0x5E8210 hooked, l2_cliffclimb): standing, jumping, falling 1; free climb 4; ledge hang and shimmy 11; surface swim 20; under water 19. l1_seatop / l2_cliffclimb rest distance 1264.2, pivot 462.6 over her feet = bone 108 + 142.2; hold wall (hold 11): pivot = root + (−3.5, −93.8, 203.9) = bone 108 + 79.01; climbing up to hold 908 the camera ended 229 from the pivot, 32° below it; idle there (after a mouse turn) it held 5 s, then turned 1.0°/s toward the record's point (spline u = 0.1 / 0.3 / 0.5 at holds 900 / 901 / 908 = list index / 10); ledge hang: pivot 628 over her root, camera pulled in to 864. l3_main_bedroom: 1260.8 behind / 491.1 above / pitch −0.0225 at rest; running forward after a look down to −1.05: pitch −0.47 / −0.25 / −0.10 at 1.0 / 1.6 / 2.3 s (the drag; ours within 0.08: Game.CameraRestAndPitchReturn). [I] ladder / perch / pillar / beam / pole modes from the markup-type table (not observed); record types 1 / 2 use our ledge distance over its length. [O] force sets: generalbank GlobalInfo +0x164 / +0x168 = DTP 0x1C (18 yaw sets) / 0x1D (36 pitch sets), entries {angle, speed, dir, count, forces} loaded by 0x6088B0, forces 0x5F2180 (1 |move · camera x row| × k, 2 |move| × k, 3 curve(|off behind| / π), 4 curve(|stick|), 5 k × dt, 6 |move z| × k, 7 mean move × k), dir signs 0x608A50 (1 move across the view, 2 move z, 3 her position along her markup 0x6EC660), blend 0x609C40 / 0x608B60, controller ctor 0x607AD0 (side flag +0x4A4 = 1); mode list (DTP 0x14 +0x144 / +0x148, 25 entries) yaw sets: 1/2 → 11 (default +0xE4), 3 → 6, 4 / 11 / 14 / 15 → 15, 9 → 4, 10 → 11, 16 → 5, 17 → 1, 19 / 20 → 11; pitch sets 1/2 → 4, 10 / 17 → 16, 16 → 17, others none; look clock rate while moving +0xCC = 1. [I] which side of behind is + for the dir-type sets (ours: the camera looks the way she moves along the ledge). Surface swimming in a circle (Wine swim_right.txt): the camera that dropped in 37° above her levels out to the mode-20 limit −10° within ~6 s (29.5 / 23.1 / 18.5 / 12.5° at 2 / 3 / 4 / 6 s): her velocity lags her heading through the turn (vector-steered at the swim acceleration, Turn rate / Swim speed rows), so she drifts away from the camera; the pivot (bone 108, 43 ahead and 26 aside of her root in the stroke, the same in tru.exe) swinging round with her heading would otherwise pull it toward the camera and steepen it (ours did, toward −70°, while her velocity turned with her heading at once). [U] grapple states (on-foot mode); command 3–6 rotation targets (0x609D40; modes 18 / 42 / 43 / 46); record force types other than 5 / 7; moving platforms (0x7027B0 / 0x5FF650); the look-target blend 0x6009C0 |
| FOV | config / `--fov` (default 60° vertical, Hor+) | [O] BaseCamera ctor 0x5E1B60 defaults fov to π/2; the gameplay camera takes the camera settings DTP 0x14 +0x64 = **45°** (0x602690: +0xB0 = degrees × π/180); with `--fov 45` l2_cliffclimb climb captures match the original's framing |

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
- [O] Costume records (0x5D6940): generalbank DTP 36 `+0x40` count / `+0x44` list of 0xC-byte records {u16 locals id,
  u16, i32 unlock, u16 objectlist id, u16}; natives 0x778C90 name, 0x778C60 unlocked (unlock `-1` or 0x5DB8A0), 0x778CA0
  wear. The shipped list holds all twelve `lara*` objects, objectlist 8..19 (lara, bikini, casual, doppelganger,
  jungle_a, jungle_b, jungle_heavy, mom_thrall, snow_heavy, snow_light, wetsuit_cold, wetsuit_warm), named 357..359
  "Legend" / "Biker" / "Evening Gown" (Legend-era leftovers) four each, all with unlock record 1. Unlock records (DTP
  `+0xC`, 0x20 bytes, 0x5DB8A0): `+0x17` set = needs the game-state bit (`+0x3EE10`, set by `unlockable` 2 = 0x5D6600);
  record 1 has it clear and no percentage needs, so data alone does not lock any costume [U: its `+0xC` index 0 test
  reads before the list]. Of every object and unit DRM only g_pda (its game-globals / REVISIT LOCATION scripts) imports
  `unlockablecostume`; no level script does. locals.bin names the outfits 67..75 (Jungle Shorts .. Bathing Suit) and
  1083 says a new outfit is worn from Lara's bedroom in Croft Manor or Load Level; neither screen was captured [U].
- Ours [I]: EXTRAS OUTFITS (below, "Menus") picks one of the nine outfits with a DRM; each unlocks when its chapter ends
  (Casual the prologue, Wetsuit the Mediterranean Sea, Jungle Shorts Thailand, Jungle Heavy Mexico, Snow Heavy Jan Mayen,
  Jungle Pants the Andaman Sea, Drysuit Light the Arctic Sea), Snow Light and Bathing Suit (worn by no level) when the
  game is finished; unlocks come from `profile.json` and 1083 shows when a chapter end unlocks one. The choice
  (`World::costume`, `profile.json` `outfit`) overrides the default rule on every level load and save load and is worn at
  once; saves keep the level's own outfit (`World::levelOutfit`), so DEFAULT restores it.

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
  0x6E68B0: one 0x69F1E0 area hit per ring of the weapon controller's attack record (+0xC4 -> +0x20 count, +0x24
  rings), under water swim_submerge_thorhammer_attack_* (ours: _front, shot at frame 10 [I]). Fire = throw, target lock + interact = ground pound (walkthroughs).
- [O] Mount: weapon data +0xB8 / +0xB9 = Lara HInfo markers 5 (segment 71, the LEFT hand's grip child of 55) and 31
  (chest segment 15, on her back): she draws it from her back with the left hand, holds it up and points with the
  right. (Handgun +0xB8..BB = markers 7/6/5/4: right grip seg 96, right thigh, left grip seg 71, left thigh.)
- Effects: each bolt plays wp_thorhammer's `fire` events at the hammer head (bolt 2 from the hammer to the shot end,
  sparks, glow, light, filter) and bullet_hit_thor at the impact; wp_thorhammer's events of the swing / pound clips run
  on Lara (ground pound: three bolt-1 arcs from her root). Shot sound `thor_hammer_shot_c/_d`.
- Effect on enemies: 1000 per bolt kills viking thralls (500 hp) in one hit, yeti thralls in two on Normal (l7
  replay `tests/replays/mjolnir_thralls.txt`, viewed GL + Vulkan). No scripted Thor mechanism checks for the hammer in
  our data survey (Thor doors / pillars / rune switches react to the gauntlets' sensors); its hits break urns (below).
- [O] Ground pound rings (`game::kPoundRings`, `CombatReal.GroundPoundRings`): lara.drm section 850, attack record
  +0x35EB8 {+4 120.0, +0xC damage class 13, +0x20 2, +0x24 -> +0x399F0}; rings of 0x1C bytes {inner, outer, damage at
  outer, damage at inner, knock at outer, knock at inner, knock angle}: {0, 500, 1200, 1200, 400, 500, 65°},
  {500, 3000, 100, 400, −1, 0, 0}. 0x69F1E0 (disassembly at 0x6E68B0: 17 stack arguments, 0x814D20 = ftol): every
  instance but Lara whose origin is strictly inside (inner, outer) in 3D takes message 0x11C, damage lerped
  inner → outer, class 13, type ((class != 4) − 1) & 3 = 0; rings with a knock speed post 0x91 (knock lerped the same
  way unless knock-at-outer < 0, angle × π/180). [I] which record +0xC4 holds: the only one with rings and class 13.
  Ours: enemies take the damage (the inner ring knocks them, Hit::knock; speed / angle unused), uber objects their sections'.

## Breakables

- [O] Uber sections take hit message 0x11C (UberObjectSection_TR8 0x6D4690) when section record +0x52 bit 0 is set:
  health (+0xDC, from record +0x3C) −= damage, floored at 0; an idle section in a state starts its first transition of
  trigger 17 (damage > param float) or 19 (damage class == param); trigger 18 (0x6D49D0) fires when health <= param +
  0.001. Triggers 6 / 7 / 8 are impulses (0x4D5560; "Carried objects": thrown and falling portables pass theirs on, a
  kick delivers the hit message; the bike and falls of Lara herself do not yet).
- [O] Users (survey of all class-79 objects): the urns `*_pt_weightbreak(_b)` (l1 32, l2 48, l4 41, l5 78, l7 50) and
  l3's `ma_ei_breakable_b..v`: health 30, 0 → 2 on trigger 17 param 0 (any hit), 7 (impulse > 379.3) or 6; state 2
  actions 5 (instance +0x18 |= 0x20000000: collision callbacks skip it, 0x48D590 / 0x697970) and 29 (section +0xD4 = 0);
  2 → 1 always, playing `fx_smash` (1 s, effects only) to its end; state 1 action 28 (0x6D5D90 -> 0x6D54F0: the
  section's removal record, all of them one fade over 0 s, then 0x6D5E20 removes the instance). `sh_ei_exploading_barrel`
  (health 50): 0 → 2 on trigger 18, state 2 = 5 + two action-19 births (`squib_exploding_barrel`: "Explosion records" below),
  `fx_boom` (3 s), removed.
- Ours (`World::damageObject`): Lara's bullets / bolts on an uber object's collider and the pound's rings; action 5
  drops the collider, action 28 marks the section removed and the game hides the object's meshes when model 0's section
  is gone (`World::removedObjects`); transition clips with flag 1 run their events (`objectClips`). Health, removal and
  collision state are saved (`StreamReal.MjolnirHitBreaksUrnAndItStaysBroken`). The treasure urns
  (`*_pt_weightbreak_reward`) are the same machine plus a birth action: "Collectibles" below.
- [O] Class-30 breakables (0x6D3160; `ca_uo_ledgebreak*`, `ar_ei_break_braced_*`, `ar_ei_breakable_hpole`,
  `ar_rb_falling_floor_straight`, `ca_rb_breakable_temple_floor`): record +0x10 hits (3), +0x18 / +0x1C pieces
  (0x20 each, spawned as instances), +0x28 / +0x34 break conditions (0x1C: flags, two floats, damage-class list). 0x11C
  counts a hit when the class is listed (0x69BFC0; only the temple floor lists one: 6, explosions) or by flags 0x400
  (class 6) / 0x800.. (classes 0-2 from Lara); the others break on Lara's contact (message 0x2B, 0x69C0A0). None lists
  Mjolnir's class 13. Implemented: "Class-30 breakables" below.
- **Class-30 breakables** (`src/game/Breakables.*`, `UnitData::breakables`). Record [O data, all 9 objects]: +0x10 hits
  (3 everywhere), +0x18 / +0x1C pieces (model byte +0x0A), +0x28 / +0x34 conditions (0x1C: flags, speed, spin, class
  list, +0x14 flags2, +0x18 action), action: +0 id, +2 type, +8 flags, +0x1C delay in 30 Hz frames, +0x20 shake, +0x38
  grace. Lara's contact kinds by condition flag [O flags / I which contact]: 0x08 standing on it, 0x40 hanging from its
  ledge, 0x20 on its pole, flags2 bit 0 on its beam; 0x200 + class list = damage of that class (temple floor: 6).
  Contact starts the action's clock (the piece shakes), at its end the piece's collider and markup go, its pieces fall
  as loose bodies and Lara lets go (`Player::letGo`). Survey (`GameReal.BreakableSurvey`, `OU_BREAKABLE_SURVEY=1`):

  | object | condition | delay (frames) | instances |
  |---|---|---|---|
  | `ar_rb_falling_floor_straight` | stand 0x08 | 23 | l5_valaskjalf 6975 |
  | `ca_uo_ledgebreak`, `_u` | hang 0x40 | 120 | l4_pool 5614..5619, l4_temple 5234 |
  | `ca_uo_balancebreakb` | hang 0x40 | 370 | (no intro in the campaign units surveyed) |
  | `ar_ei_break_braced_ledge` / `_corner` | hang 0x40 | 105 / 160 | l5_valaskjalf 6959..6962 |
  | `ar_ei_breakable_hpole` | pole 0x20 | 176 | (none in the surveyed units) |
  | `ca_rb_breakable_temple_floor` | damage class 6 (0x200) | action type 1 | (none in the surveyed units) |

  [I]: which of the two condition lists applies when, the piece impulses (they drop under gravity), hits (3) is not
  counted for contact breaks. Saved ("breakables"). `GameReal.FallingFloorBreaksUnderLara`: l5 6975 breaks 46 ticks
  after she steps on it walking north, she catches ledge 1551; the jump across works too.
- **Explosion records** [O layout from data; I field meanings beyond damage / radius]: tag 0x2775 at an object's
  physical record +0x70: +4 damage, +6 damage at the edge [I], +0xC radius, +0xE knock, +0x10 knock at the edge, +0x12
  cone angle. `squib_exploding_barrel` 800 / 800, radius 948, knock 500 / 250; `squib_explode_door` 5 / 2, radius 1896,
  knock 1000 / 400, angle 30. An uber action-19 birth of such an object blasts at the section's marker one tick later
  (`World::birthBlast` / `tickBlasts` / `blast`): damage class 4 to Lara, enemies and damageable objects in the radius
  (so tanks set each other off). The knock [O decomp]: the area hit 0x69F1E0 (called from 0x6A5C50's record hand-over
  with +4 damage, +6, +0xC radius, +0xE knock, +0x10 knock at the edge, +0x12 elevation) sends instances message 0x91 at knock .. edge
  by distance, heading away from the centre at the elevation, and ends in 0x493170 → 0x492CC0 for the Havok shapes in
  the radius: an impulse of 0.0004 × the shape's surface area × knock × (1 − distance / radius), capped per shape
  (body +0x14C × 200 [U]), at the shape's point nearest the centre (0x4DF090), along (shape − centre) raised by half
  their distance. Ours [I] (`physics::Scene::blast`): grenades change each body's velocity by near .. edge along that
  raised line; a squib gives the same momentum along the body's heading from the centre at the record's elevation, at
  the point of the body's bounds nearest the centre. A centre inside the body (the door squib is born at the foot of its
  own leaf) has no heading: the body goes the way it already moves, else the way its centre of mass lies from the centre.
  l3_bedroom_hall's leaf 4376 (blown off untouched) is kicked out at the foot and falls into the pit; pushed through its
  centre of mass it only hopped and stood in its frame (494 wide in a doorway barely wider).
- **Freed uber sections** [O action 10 removes a joint; I the body's motion]: a non-main section left without a joint
  (13 objects: `ma_uo_bedroom_door`, tentacle gears, the snake head, ship floor panels / grates / console, thor slide
  doors, chain debris, the naga statue) becomes a loose body with its collider, as do sections riding on it; the main
  section uses the object's own loose flag. It starts with the velocity its prismatic joint had when removed (the ring
  door 4526 comes off its slide moving toward the rope's pull). Saved with the section state (not the velocity). Its
  shape is the model's solid primitives (`looseShape`): the leaf's non-solid lower plank (633 wide) is no shape. Open
  [U]: leaf 4526, pulled 44 out into its frame when freed, is pushed back by the level collision and stays standing in
  the doorway (`ProgressionReal.PrologueRingDoorPulledOffByTheGrapple` notes it; what clears it in tru.exe is not
  decoded: nothing in the object's states moves it but the removed joint and the squib).

## Hazards

What hurts Lara apart from enemies, falls and drowning, per kind and unit, with the rule tru.exe applies and what
ours does. Two mechanisms carry every trap of the game [O]: **damaging collision primitives** (the object data) and
**script damage** (the `health` natives called by volumes, planes and trap scripts). Survey tools (scratch):
`openunderworld-work/gameplaygoal/hazards/` (`primsurvey`, `hzsurvey`, `healthsurvey`, `intros_all.txt`).

### Damaging collision primitives (`src/game/Hazards.cpp`)

- **Data** [O]: every HInfo collision primitive of a model carries an s16 amount: sphere `+0x1C`, box `+0x3E`, capsule
  `+0x32` (the mesh type 13 `+0x62`, not decoded); tru.exe reads it with `0x48E040`. 72 primitives of 30 objects have
  one (all the others 0): the table below.
- **Rule** [O]: a collision pair handler that meets such a primitive calls `0x4923C0`: the primitive's owner is the
  attacker (skipped when its instance flag `+0x1C & 0x100` is set), the other instance the victim. The damage manager
  (`0x5B2990`, slot `+0x58` = `0x5D4C60`) posts the hit message `0x11C` with the amount and **damage class 6** to the
  victim (skipped when its flag `+0x14 & 0x20` is set); then the scripts of both instances get instance prototype 34
  (**vt 21**, `0x7990A0`, arguments attacker, victim and each side's query `0xF7`) and prototype 35 (`0x799100`).
  Lara takes `0x11C` in `0x6F5030`: class 6 has priority 4, so the touches of the next 30 frames (player data `+0x60`)
  are ignored; 1000 health at the normal difficulty, so 1000 and more is death at once.
- **Two ways in** [O]: the overlap handlers (`0x4928C0`, `0x492980`) call `0x4923C0` for every overlap whose two
  bodies have an owner; the rigid contact handler (`0x496220`, registered for the category pairs 1-1 and 11-1) only
  when a contact point moves at 5 or more (`v.v >= 25.0`), then posts the collision message `0x3B`, which Lara's
  handler (`0x75A020`) passes to `0x4923C0`. **[I]** which primitive takes which way: flag bytes 2-3 not 0 (flame
  jets, rollers, the weights' under-plates, the barrel run, the squib spheres) = an overlap volume that hurts whenever
  she is in it; 0 = solid collision that hurts only while the primitive moves at 5 units a second or more (the unit
  and "her own walking into a resting one does not count" are [I]: the door plates are inset in their slabs so a
  resting door cannot be touched on its plate, but the gate of Baldr's tomb is one 2000 plate she walks up to).
  Open: a Wine check of a walk into gate 45, and of standing in a jet for several seconds.
- **vt 21 in the trap scripts** [O bytecode]: every trap compares the victim with the player and plays its impact
  sound at her (`sound` 3); the Xibalba spear and blade rows also kill their trigger planes (5900 / 5901), wait 1 s
  and give the weapons back (`larabase` 55); the ship's barrel run adds `health` 1 (5000).
- **Ours**: `World::syncHazards` collects the primitives of the loaded objects (the object's model and its uber
  sections' models), `World::tickHazards` poses each with its segment (collider transform, uber section joint + clip,
  or the anim graph's pose) and tests Lara's capsule (`hazardDistance`; [I] 6 units of skin). A touch that hurts
  (`World::damageLara`, class `kDamageContact`) is listed in `World::hazardHits()` and calls the attacker's vt 21
  (`Runtime::contactDamage`; [I] only the touches that hurt, tru.exe calls it from every contact callback). Hit
  reaction as an enemy blow of that amount (`hit_medium*` from 250). Where each primitive stood a tick ago is saved
  (object state `hazards`, written only where there are any). Self-test measurement `step.hazards`.

| Object (class) | Units x instances | Amount, primitives [O] | Way [I] | How it is driven [O script / data] | Ours |
|---|---|---|---|---|---|
| `ca_ei_flame_trap` (6) | l4_traprooms x13 | 800; box 224 x 745 x 224 on the jet bone | overlap | anim graph: the bone travels 948 out of the nozzle in 0.25 s, script clocks hold it out 3 s and in 3 s (members +28 / +32, phase +36 per intro); message `deact_flames` (the house's key taken) stops them | yes: `GameReal.FlameJetBurnsLaraOncePerWindow`, `game_selftest_hazards` |
| `ca_rb_crusher_slim` (34) | l4_traprooms x6 | 12345; plate 3 x 588 x 316 on the face of jaw 0 (model 0); model 1 = the mirrored jaw | solid | two prismatic joint markers + a link marker (kind 11), joint 0..900, bind pose closed (68 apart); volume 5905 starts the clocks while Lara is in the house (vt 58 / 59): motor 15 a frame open for 2 s, then -65 shut; a `multibody` 79 watch (-100..20) plays the dust effect; the script also kills (`health` 1, 3000) when `larabase` 6 (contact force) exceeds 30 | yes: both jaws collide and slide (`linkedJaws` [I: one body per model, equal travel]); `GameReal.CrusherJawsOpenCloseAndKill`, self-test; the contact force stays the depth stand-in [I] |
| `ca_ani_adren_spears` (79) | l4_traprooms x3 | 1000; the tips' box 2632 x 70 x 198 over a 1363 tall shaft box | solid | uber slide joint 0..1106 (out of the floor / ceiling), commands 629 from planes 5900 / 5901 / 5904 | damage yes while they move (`GameReal.RestingSolidHazardsDoNotHurt` for the standing row); the adrenaline sequence itself is the uber scripts', not compared with tru.exe [U] |
| `ca_ani_adren_blades` (79) | l4_traprooms x2 | 1000; two blade boxes 1264 x 95 x 218 | solid | clips on commands 630 / 631 | as the spears |
| `ar_ei_grinders` (6), `ar_ei_grinders_two` (6) | l5_valaskjalf x4, x4 | 400; 3 / 2 roller boxes 217 x 1362 x 217 | overlap | anim (rolling) | yes: `GameReal.RollersCost400ATouch`; [U] the rollers are not solid for her in ours (class-6 anim objects do not collide) |
| `ar_ei_grinders_rb` (79) | l5_valaskjalf x1 | base plate 700; the two rollers (section models 2 / 3) 13331 | solid plate, overlap rollers | uber slide joint 0..1580 + roller hinges (the travelling ceiling set) | damage yes; untested by input |
| `ar_ani_weight_with_ledge`, `_without_ledge` (6) | l5_valaskjalf x2, x4 | 500; under-plate 289 x 345 x 115 | overlap | animated with their pair (`ar_ani_weight_pair`) | yes (posed by the clip); untested by input |
| `ar_ani_thor_hammer_two` (6) | l5_valaskjalf x1 | 12345 on all 18 boxes (hammers one / three / four: none) | solid | stimulus 0x10000 from its trigger | yes while it swings; untested by input |
| `ms_ani_tentacle_hazard_a..d` (79) | l1_kraken_room x4 | 500; four spheres r 567 / 449 / 343 / 218 along the arm | solid | uber clips (swats), removed when the kraken is dead | yes while they swing; untested by input |
| `ms_ani_tentacle_gearr` (79) | l1_kraken_room x1 | 100; 8 boxes (section models) | solid | uber clips | as above |
| `squib_explosion_detector` (6) | l1_kraken_room x57 | 200; sphere r 927, flag 5 | overlap | squib spheres under the pool's surface | hurt on overlap [I: whether Lara's pair meets flag-5 spheres at all] |
| `squib_lara_throwback`, `squib_ma_lara_throwback` (6) | l1_kraken_room x1, l3_main_hall x1 | 200 (sphere r 316), 333 (box 161 x 7826 x 1107) | solid | the scripted blast that throws her | as data |
| `ms_ani_ship_adren_trap` (6) | l1_ship_sinking_hall x1 | 9999; a plate and four capsules r 101 (the barrels) | overlap | the barrel run's clip; its script adds `health` 1 5000 (vt 21) / 9999 | yes; untested by input |
| `ms_ani_ship_barrel_a` (79), `ms_ani_explosion_damage_blocker` (6) | l1_ship_sinking_hall x1, x1 | 150 (capsule r 93), 500 (box 4741 x 316 x 4741) | solid | clips | as data |
| closing doors and falling pieces: `ms_rb_tomb_door` 100, `ms_rb_tomb_gate` 2000, `ms_rb_grapple_snake_head` 12345, `sa_uo_shivas_right_arms` 2000, `sa_uo_naga_statue` 2000, `ma_rb_shadow_door_b` 12345, `ca_ani_temple_reveal` 2000, `ca_rb_wall_hpole_door` 200, `ar_uo_reward_door` 2000 | l1_baldrs_tomb (223, 45), l1_kraken_entrance (751), l2_puzzleroom (2060), l2_shrineroom (2794), l3_chapel (4835), l4_ballcourt (5464, 6505), l5_valaskjalf_entry (7227), l5_valaskjalf_entryb (7187) | a thin plate on the closing edge (reward door: 1207 x 2802 x 125 under the slab; shadow door: 94 x 16 x 2519 between the halves) | solid | their joints / clips | crush = touching the plate while it moves; untested by input |

### Script damage (`health` natives; survey of every intro script calling `health` 1 / 2)

`health` 1 (`0x4A50F0`) posts message 0xD with the amount, `health` 2 (`0x4A50D0` -> `0x5D3C90`) the hit message
0x11C with amount and type; ours `World::bindScripts` (`setHealthNative`).

| Kind | Units x instances | Rule [O bytecode] | Ours |
|---|---|---|---|
| `firetriggervolume` (script 6516) | l3_main_bedroom x14, l3_main_hall x14, l3_bedroom_hall x7, l1_ship_sinking_hall x5, l1_ship_lower_hallway x4, l3_mansion_exterior_afterburn x2 | while Lara is in the volume (vt 70 -> state 1): `health` 2 with member +12 (300), then the burn sound as a sequence (`sound` 1), again when the sound ended; leaving (vt 71) ends it. Its player sensors count on global byte 13 ("fireanim": her arm raised against the heat [I]) | the first 300 at once (`GameReal.FireVolumeBurns300`); the repeats run on the app's sound clock (`GameSound::scriptSoundBusy`), so a World without sound burns once per entry [open]. The constructor stops at the sensor array's length call (op 0x65 without a target: dynamic arrays are not modelled), so "fireanim" never counts [open] |
| `slowfiretriggervolume`, `deathfiretriggervolume` | l3_main_hall x5, x5 | the same script type; their amounts come from the intro data [U: not dumped] | 300 measured in ours for both [U] |
| `jellyfish_a` | l1_seafloor x31, l1_norse_hallway_a x12 | 100 every 2 s inside its sensors | yes (`GameReal.JellyfishStingOnTouch`) |
| `fx_steam_val_column` | l5_valgrind x16 | two player sensors on markers 2101 / 2102 of cog wheel 7314, on during the blast clip; a hit sends player message 0x1D4 (`larabase` 58, `0x778000`; [I] the scald that breaks her grip), takes 250 (`health` 1) and switches the sensors off until the next blast; a dispatcher of the unit takes 350 | `larabase` 58 is bound (the handler ran into it and stopped before the damage); message 0x1D4 has no effect on her yet [U]; untested by input |
| kill planes, `health` 1 99999 | l5_valaskjalf x4, l3_bedroom_hall x2, l4_staircase_upper x2, l1_baldrsstairs_a, l4_ballcourt, l4_staircase_xibalba, l4_trailstart x1 | pits (the spike pits of the Mexican roads and stairs are these and the volumes below: no spike object exists) | yes (one-sided planes fire toward local +Y since 0.42) |
| kill volumes, `health` 1 99999 | l4_ballcourt x8 | the road's spike pits | yes |
| planes `health` 2 2000 / 12345 / 9999 | l3_main_bedroom x6, l3_main_hall x4, l2_maproom x2; l1_ship_sinking_hall x2, l5_valaskjalf x1; l4_pool x1 | fire pits, shafts | yes |
| volumes `health` 2 2000 / 1000 / 300 | l2_towerconnector x2, l4_traprooms x1, l2_maproom x2 + 1 + 1, l2_hubhall x1 | drops | yes |
| eitr: volumes `health` 2 12345 (l4_pool x3), 6000 (l7_helheim01 x1), `watervolume` 12345 (l4_pool, l7_yggdrasil_end), l7_yggdrasil_end volumes x66 (amount in a member) | | the pool and its falls (`ca_ani_goo_falls` x6, `ar_uo_goo_fall` x6, `floater_goo` x10 carry no damage of their own) | l4_pool verified against the original (Water); the others run the same natives, untested by input |
| `sh_ei_exploading_barrel` | l1_ship_exterior x29, l6_ship_exterior x14, engine halls and hallways x13 | explosion records | Breakables |

No object or script of the game deals damage as darts, lava, electricity or poison gas: the electrical boxes are
switches (`ma_ei_wall_switch_electric`), steam on the ship is effects only (`fx_steam*`), and the campaign's "spikes"
are kill planes / volumes under the pits.

Open, in order of use on the critical path: the fire volumes' repeat without the app's sound, steam (message 0x1D4, a
test by input on the tower), the hammer / weights / tentacles / barrel run by input, the crushers' contact force
(`larabase` 6), collision of class-6 animated traps (rollers), drawing the crusher's second jaw (model 1; the renderer
draws model 0 with the object's body), a Wine comparison of one hit per kind (amount, window, reaction clip).

## Close combat

Code: `src/game/Melee.cpp` (`World::meleeInput` before the player ticks, `World::tickMelee` after the tick's
interactions, `Player::actTick`), enemy reactions in `src/game/ai/Enemy.cpp` (`Mode::Knocked`). Player-visible rules
checked against https://tombraiders.net/stella/walks/controls/TR8controls.html. Decode notes:
`openunderworld-work/gameplaygoal/melee/` (chart dumps `m1.py`..`m6.py` over hurdles/sc.py, event probe `ev.cpp`).

- **Chart** [O lara.drm state chart, player data +0x12508; conditions 0x8B2838, actions 0x8B0A50]: the chart's input
  ids are game actions, not buttons (0x760C40 indexes a 0x30-byte action table): 32 = Interact (melee), 99 = Interact
  (an engaged object), 50 / 51 = Crouch, 52 = Jump, 42 = Sprint, 101 = put a carried object down, 102 = throw it (the
  weapon chart).
  - **The action map** [O generalbank.drm DTP 47, named by GlobalInfo +0x1C; tru.exe 0x4868F0 hands it to the mapper
    0x480BC0]: `+0` 167 bindings of 4 bytes {action id, context, control, second control (0xFF none)}, `+8` 63 controls
    of 0x18 bytes {u16 kind (0x10xx button, 0x20xx axis), u16 code, u32 hold time in ms, ..., float range}. A control's
    code under 25 is one of the Control Configuration's virtual buttons ("Controls": 0 Interact, 1 Grapple, 2 Crouch,
    3 Jump, 4 Target Lock, 5 Shoot, 6 Sprint, 7 Grenade, 9 Manual Aim), above that a raw key (0x81 Esc, 0x148 the
    arrows). The bindings of the ids above: 32 and 99 -> control 2 (button 0, Interact), 50 / 51 -> control 1 (button
    2, Crouch), 52 -> control 0 (button 3, Jump), 42 -> control 6 (button 6, Sprint), **101 -> control 1: Crouch puts
    the carried object down, 102 -> control 5 (button 7): Grenade throws it**. Dump:
    `openunderworld-work/gameplaygoal/melee/am.cpp` (`am.txt`). Condition 0x7D1 = state property 1002 "melee allowed" (the ground and combat families and the carry
  family S1313); 0x7D3 = the melee target (targeting slot 5, 0x71CAF0(5)) is the combat target; 0x7D8 (0x765810) its
  height class; 0x7DA the fatality target (slot 6); 0x7DB a vault target; 0x591 the kind of the engagement record she
  stands in (0 none); 0x5DD / 0x5DE she holds a portable / one that encumbers her.
  - T36 (Interact + 0x7D1 + 0x7D3) -> S1422 -> by 0x7D8: S1429 `melee_kick_low`, S1430 `_mid`, S1428 `_high`, S1431
    `_highest`. T38 (Interact + 0x7D1 + 0x591 = 0) -> S1423: `melee_kick_move` while the stick runs her forward
    (S1424), else `melee_kick_mid`. S59 (the sprint) T177 -> S1432 `melee_kick_move`.
  - T1347 (Interact + 0x7DA) -> S741 -> S738 `melee_thrall_fatality` (holding a pole: S740, the pole's
    `hosted_melee_thrall`).
  - T1405 (S724, the combat run: Jump + stick forward + 0x7D1 + 0x7DB, not encumbered) -> S736, the enemy vault.
  - T1355 (S746: Crouch + stick, in combat) -> S744 -> `combat_rollfront` / `_back` / the side rolls by the stick's
    quadrant relative to her (condition 0x193 = 414..417); T1367 (Jump, in combat) -> S751 -> `combat_jump<f|b|U>_takeoff`
    and the side leaps.
  - Kind-3 engagement records (the urns): S1390 T2306 -> S1301, whose action 1406 (0x72C790) posts 0x158 to the object
    and starts the kick (0x71EF60). `Phys_kick_low` / `_high` (requests 13 / 64) have no caller state [O].
- **Attack records** [O player data, 0x28 bytes: +1 hit effect id, +4 damage, +0xC hit class, +0x20 / +0x24 rings;
  state property 1005 (type 27) names the one in force, weapon controller +0xC4]:

  | state | move | damage | class |
  |---|---|---|---|
  | S1426 (parent of every kick) | kicks | **60** | 2 |
  | S1432 | the sprint kick | **3** | 2 |
  | S737 | thrall fatality | **100** | 16 |
  | S1311 | a swung portable | **100** | 2 |
  | S736 | enemy vault | **0** | 1 |
  | S788 / S795 / S796 | Mjolnir's pound (see "Weapons") | 120 | 13 |

  `loadMeleeTuning` reads them through the chart (`CombatReal.MeleeDataDecodes`).
- **Targets** [O]: slot 5 (+0x474) = within **640** of her eye (3D) and **90°** [I: criterion type 5 read as the
  horizontal angle from her facing]; slot 6 (+0x478) = within 640 and criterion 0xE [I: a regenerating thrall]. Height
  class (0x765810): the target's top over her feet (target z - her z + its query 0xD4) under **63.21** low, under
  **316.06** mid, under **537.29** high, else highest (+0x2B4 / +0x2B8 / +0x2BC = 0.2 / 1 / 1.7 m). Ours: the top is the
  enemy's highest bind-pose bone (`EnemyTuning::top`) [I].
- **Hit windows** [O clip events, 30 Hz frames; 0x716F30 cases 0xE / 0xD post 0x8F / 0x94 with the limb to Lara and to
  the portable she holds: the limb's contacts deliver the record]: low 4..10 (0.70 s clip), mid 4..10 (1.20 s), high
  3..13 (0.93 s), highest 5..10 (1.03 s), move 6..11 (0.70 s); weight swing `hosted_melee` 10..20, pole swing
  `hosted_melee_front` 3..14. The fatality's blow is event 0x6D at frame **23** (0x6E6840: message 0x11C straight to
  the target with the record's damage and class, no contact test); `combat_killing_stomp` carries the same events but
  no chart state plays it [O].
- **Ours**: Interact (a press no pickup, portable, switch, block or handle took) starts the move; the input is locked
  for the clip (`State::Melee`, root motion). She turns to the target and closes on it at 1200 units/s until the window
  opens [I: tru.exe interpolates her onto a point by the target, action 1007]. In the window her leading foot (the
  lower of each side's bind-pose bones, the one farther forward) is a sphere of radius 70 [I]: every enemy capsule it
  touches, and the target she kicked at if the gap between the two capsules is within 190 [I: her leg], take the
  record once. Breakables: an uber object's collider in the sphere, or the kind-3 object she engages (else one within
  reach [I]), takes the hit message (`World::damageObject`: the urns break on any damage); she kicks by its top.
- **Enemies** (`Hit::melee`): knocked off their feet (`ai::Mode::Knocked`): the fall clip by the side the blow came
  from, then the get-up [O clips, I pairing]: soldiers `hitreact_heavy_forward` + `get_up_back` (from behind
  `hitreact_heavy_back` + `get_up_front`), viking / mayan / yeti thralls `hitreact_heavy_knockdown` +
  `hitreact_heavy_getup` (rear: `heavy_hitreact_knockdown_rear` + `heavy_hitreact_getup_rear`), cats
  `hitreact_thrown_back` / `_forward` + `hitreact_thrown_getup`, nagas `stun_start` + `stun_end`; types without them
  (spiders, bats) stagger. A small spider (2 hit points, top under 63.2) dies to the low kick: the stomp.
- **Thralls** [O 0x67F040 / 0x620380 / 0x613B40]: bullets, kicks and explosions (viking / mayan) only put them down to
  regenerate ("Enemies"). The finishing move's class 16 is in no regenerate list; ours: it ends a regenerating thrall
  whatever its health [I: the lying body's damage path 0x682680 -> 0x65AC80 is not traced], with `death_permanent`
  (viking / mayan), then its chunks. An adrenaline head shot is the hit's +0x6E flag, which no thrall's entry carries,
  so it kills standing and lying thralls [O] (`Enemy::getsUp`). Not done: `death_drown` in the eitr pool, the pole's
  `hosted_melee_thrall`.
- **Enemy vault**: with the target locked, running at the melee target, Jump: her clip is the enemy's own
  `hosted_enemy_vault` (in its DRM, it animates her; kept as `vault:<type>`), the enemy plays `hitreact_vault` /
  `hit_react_vault` / `damage_vault` and takes the record's 0. Soldiers have only `hitreact_vault_adren` (the
  adrenaline head shot's vault): not vaulted [I].
- **Evasive moves**: target lock + Crouch with the stick = `combat_roll<front|back|left|right>`; + Jump with the stick
  aside or back = `combat_jump<l|r|b>_takeoff`, then a leap of 1200 units/s that way and 900 up [I] playing
  `combat_jump*_inair`; forward and neutral stay the plain jump. No invulnerability rule is decoded.
- **Knocked down** (`World::throwLara`, `State::Knocked`): a blast's knock (message 0x91; ours: her own grenades)
  throws her: `hitReact_thrownBack` -> `hitReact_landBack` -> `Hit_getupback` when the blast is in front of her (onto
  her back), else `Hit_ThrownFront` -> `Hit_LandFront` -> `Hit_GetupFront` (onto her front) [O clips: `Hit_LandFront`'s
  root moves 215 forwards, `hitReact_landBack`'s 60 backwards; I: which blast picks which]. She skids with the knock, lies
  1 s [I] and gets up; Jump gets her up at once (the controls page). tru.exe's ThrownState and the reaction types 7 / 9 /
  10 / 12 / 15 (0x6EB590 passes those hit classes through) are not traced [U].
- **Open** [U]: the lying thrall's damage path (0x682680 -> 0x65AC80), `death_drown` in the eitr pool and the pole's
  `hosted_melee_thrall` (chart S740); tru.exe's ThrownState and its reaction types; any invulnerability of the evasive
  moves; the interpolation of action 1007 that closes her on the target; soldiers' `hitreact_vault_adren`.
- Saved with the objects (`melee`: the move, its target, phase and timers; docs/SAVES.md).
- Tests: `Combat.KickLandsInItsWindow`, `Combat.KnockedDownAndGetsUp`,
  `CombatReal.GrenadeBlastKnocksLaraDownAndSheGetsUp` (l1_baldrs_tomb: her own grenade's blast, the three clips, up
  again), `Combat.EvasiveRollsAndLeapsUnderLock`,
  `CombatReal.MeleeDataDecodes`, `CombatReal.KickKnocksAPoacherDown` (l4_pool), `CombatReal.StompKillsASpider`,
  `CombatReal.SprintKickAndAirKicks`, `CombatReal.VaultOffAnEnemy` (a tiger), `CombatReal.FatalityEndsADownedThrall`
  (l1_baldrs_tomb: shot down, finished, chunks; the other one rises), `CombatReal.KickBreaksAnUrn` (l1_baldrs_sideroom
  94), `AiReal.MeleeKnockDownsAndThrallFinishers`; self-tests `melee_kick`, `urn_kick`.

## Carried objects

Code: `World::carryPortables` (`src/game/Objects.cpp`: pick-up, insert, removal), `src/game/Melee.cpp` (`portableDef`,
`pickUp`, `heldPoint`, `releaseHeld`, `tickThrown`, `throwVelocity`, `Player::encumberedMove`).

- **Portable data** [O object data, ObjectBase +0xC8, magic 0xB00B]: +0x18 it encumbers her; the throw (0x6B53A0):
  +0x34 near distance, +0x38 aim distance, +0x3C / +0x40 horizontal launch speed for a near / far aim point (units per
  30 Hz frame), +0x44..+0x50 the same with a combat target.

  | object | encumbers | near / aim | speed near / far | with a target |
  |---|---|---|---|---|
  | `ms_pt_weight_c` (the carved weights) | yes | 632.1 / **2528.4** (2 / 8 m) | 53.73 / **53.73** | the same |
  | `ar_pt_pole_a` (poles) | no | 237.0 / 1264.2 | 25.28 / 56.89 | 316.1 / 1896.3, 37.93 / 69.53 |

- **Lara's clips with a portable live in its DRM** [O]: `hosted_pickup` (weights 1.5 s, event 0x4E at frame 17 -> message
  0x17B: in her hands; poles frame 14), `hosted_putdown` (event 0x4F at frame 20 / 8 -> 0x156 with parameter 1),
  `hosted_throw` (event 0x53 at frame 27 / 14 -> 0x1AF), `hosted_melee` / `hosted_melee_front`, `hosted_insert` (0x4D
  at frame 48), `hosted_remove` (0x50 at 18), `hosted_idle`, the swim variants; the chart plays them by slot (state
  property 213 type 33: 4 put down, 5 throw, 6 pick up, 7 insert, 8 remove, 10 swing, 13 thrall fatality). Keys have
  no put-down / throw / swing clips (`obj_store`). Loaded on first use as `hosted:<animation id>`
  (`World::portableDef`). The weights' walk is lara.drm's `portable_encumbered_idle` / `_idle_to_walk` / `_walk` /
  `_walk_to_idle` (chart S1316..S1322; the `_turn45` / `_turn90` clips are not used).
- **Messages** [O 0x6B5890]: 0x156 = let go: the body takes her velocity, with parameter 0 plus 300 [I unit: units/s]
  along her heading; 0x1AF = thrown: velocity from 0x6B53A0, and it is a weapon for **1500 ms**; 0x1A9 / 0x188 stow /
  unstow (not done).
- **The throw** [O 0x6B53A0 -> 0x6A7510 -> 0x6A6F40]: from 0.7 of her height over her feet (0x6EFC40) at the point
  `aim distance` along her heading (with a combat target: the target), stopped at the first thing in the way (0x6F2530);
  the horizontal speed is the data's (the near one when that point is closer than `near`), the flight time the distance
  over it, the vertical speed what reaches the point under gravity **3 per frame²**, replaced by the horizontal speed
  when it exceeds the far speed. A weight thrown down a room leaves at 1612 units/s forward and 1612 up.
  `game::throwVelocity` (`Combat.ThrowVelocityAsTheExe`). [U] A short press throwing shorter (the walkthrough) is the
  weapon chart's state S100, held while input 102 is down: its effect on the launch is not decoded; ours always throws
  in full. Ours flies under the scene's gravity for bodies, 5.72443 per frame² [I, PHYSICS.md], so it lands nearer
  than tru.exe's solve assumes (about 1500 units from her on a level floor instead of about 2200). Landed, a weight
  lies on its cube: its loose body is made of the primitives tru.exe gives shapes that rest on the world
  (`looseShape`, PHYSICS.md "HInfo" flag bytes), not of its r 104 sphere, on which it used to roll some 20 m and off
  the press plates (`game_replay_baldrs_plates_gate`: thrown from plate 219 it comes to rest ~2100 units on).
- **Inputs** [O the action map, "Close combat"]: Interact picks up, and with it in her hands inserts it into the
  receptacle she engages (chart input 99); Crouch (input 101) puts it down; Grenade (input 102) throws it; Interact
  elsewhere swings it (input 32). Before 0.42 ours set it down on Interact: the tests and the `baldrs_plates_gate`
  replay that did so now press Crouch.
- **Ours**: Interact picks up (the clip, then in her hands), inserts into an engaged receptacle or else swings it (it
  stays in her hands); **Crouch puts it down** (the clip; without the data at once), **Grenade throws it** (no grenade
  leaves), Jump or a gun key lets go of it with the shove; a weight walks only. The body leaves her hands as a loose
  rigid body (`releaseHeld`: from her hands, moved out along her heading until it clears her proxy capsule) and from then
  on is Jolt's: it collides with the level, the plates and other bodies. While thrown, the first enemy it reaches takes
  the swung-portable record (100, class 2) and goes down [I: the thrown body's own hit numbers are not decoded]. Carried,
  it sits between her hands (weights, hosted clips) or 60 ahead / 80 up of her root (poles, keys [I]). Swimming: put
  down at once, no clips [I].
- **Collision while carried** [I: tru.exe keeps the object on her hand markers; what keeps it out of walls there is not
  traced, U]: the object's thickness is the smallest half extent of its collision box (`PortableDef::thick`; the carved
  weight's and the poles' from their Havok shapes). A sphere of that radius (at most her own radius less 5, so it
  starts inside her capsule) swept from her chest to her hands stops at the first wall, door or object
  (`World::heldPoint`), and, for an encumbering object (the carved weights and the power gems, held out in both hands; the weight's
  thickness 104; not while she engages a receptacle's record), on her feet a wall
  of the level within the thickness of her hands pushes her back by the overlap
  (`World::carryCollide`; objects only stop the sweep, as receptacles are approached with the portable in her hands):
  she cannot walk a weight into a wall, and it is never posed through one. Not during the
  pick-up, put-down, throw and swing clips (her hands go to the floor). Let go, it is a Jolt body from that point
  (`releaseHeld`), moved out along her heading until it clears her proxy capsule.
- **Breakables by contact** [O 0x4D5560]: the uber sections' triggers 6 / 7 / 8 test the lengths of an impulse
  message's three vectors against the transition's float; vector A is an impulse applied at a point (0x4ECFA0). The urns:
  6 over **0** (any blow), 7 over **379.27**. Ours (`World::impulseObject(uid, applied, contact)`): a let-go or thrown
  portable moving faster than 30 units/s passes its speed as the contact [I: B / C read as a contact speed in units/s]
  to every uber object its bounds reach: a weight thrown or dropped onto an urn breaks it. The bike's ram and Lara's
  roll do not call it yet.
- **Open** [U]: what keeps a carried object out of walls in tru.exe; the short throw (weapon chart S100); stowing a
  pole (0x1A9 / 0x188); the insert / remove clips (`hosted_insert`, `hosted_remove`: ours inserts and takes out at
  once); the swim states' carry clips; whether Lara's own body (a roll, a fall onto it) or the motorcycle posts the
  impulse message of triggers 6 / 7 / 8 (ours: only let-go and thrown portables and the kick's hit message do); the
  thrown body's own hit numbers.
- Walkthrough requirement C04-03 (l1_baldrs_tomb's west room): from press plate 219's door-side edge, facing door 223,
  the thrown weight passes the doorway (x = -4109) in the air and rests beyond it
  (`CombatReal.WeightThrownThroughTheDoor`).
- Saved with the objects (`held`, `melee`: thrown timers, a pending launch); the loose body is in `objects.physics`.
- Tests: `CombatReal.WeightPickedUpCarriedAndPutDown`, `CombatReal.WeightThrownThroughTheDoor` (also: a poacher in its
  way takes 100 and goes down), `CombatReal.ThrownWeightPressesThePlate`, `CombatReal.CarriedWeightCollidesWithTheLevel`
  (walked at the west room's walls: the weight stays out of them, she is held off, put down there it is a loose body on
  the floor), `Combat.ThrowVelocityAsTheExe`; the receptacle tests and replays ("Receptacle replays and the autopilot":
  pick-up by Interact with its clip, put-down by Crouch); self-test `carry_throw`.

## Collectibles

Treasures, relics and health packs. Code: `src/game/Progress.*` (tables), `formats::birthIntros`
(`src/formats/Births.cpp`), `World::collectPickups` (`src/game/Stream.cpp`), `World::uberAction` case 0x13,
`World::maxHealth`. Player-visible counts checked against https://tombraiders.net/stella/walks/TR8walk/rewards.html.

- **The reward table** [O]: generalbank.drm's DTP 36, named by GlobalInfo `+0x194` (`*0xE7EE50`; loaded on first use,
  0x5D5F80 builds the totals). `+0` reward count 206 (index 0 unused), `+4` -> rewards of 8 bytes {u16 type}, `+0x10`
  13 groups, `+0x14` -> groups of 0x10 bytes {category, found (runtime), count, -> reward indices}, `+0x58` float[3]
  health given per category (0, 0, 300), `+0x64` float[3] headroom (0, 0, 50). Category 1 = treasures, 2 = relics,
  0 = twenty rewards (59..78) no unit places. A reward's type is its area and equals our chapter index: 1
  Mediterranean, 2 Thailand, 3 Croft Manor, 4 Mexico, 5 Jan Mayen, 7 Arctic Sea; types 1..7 add into type 0 (the
  0x5D5F80 / 0x5DB6A0 switch; 8..13 -> 14, 0 and 14 -> 15 are unused levels of the same tree). The script type
  `rewardsystem` (NtRewardSystem, 9 natives 0x79C0E0..) reads it for the PDA: 0x5D60A0 "is reward n collected",
  0x5D60D0 found[category][type], 0x5D6120 total[category][type], 0x5D6380 "group complete". `game::loadRewardTable`
  reads it; `ChaptersReal.RewardSurveyMatchesTheRewardTable`.
- **Totals** [O table = units]: every treasure / urn / relic intro carries its reward index in its intro data `+0x30`
  (185 intros, indices 1..205, each once). Per area, treasures (lying in the world + in urns) and relics:

  | area (reward type) | treasures | lying | in urns | relic (intro) | health packs |
  |---|---|---|---|---|---|
  | Mediterranean Sea (1) | 26 | 10 | 16 | l1_kraken_room 321 | 10 |
  | Coastal Thailand (2) | 30 | 22 | 8 | l2_maproom 2588 | 19 |
  | Croft Manor (3) | 13 | 7 | 6 | l3_chapel 4757 | 5 |
  | Southern Mexico (4) | 50 | 10 | 40 | l4_pool 5496 | 22 |
  | Jan Mayen Island (5) | 30 | 10 | 20 | l5_valaskjalf_entry 7280 | 17 |
  | Andaman Sea (-) | 0 | 0 | 0 | - | 3 |
  | Arctic Sea (7) | 30 | 19 | 11 | l7_helheim01 8936 | 18 |
  | all | **179** | 78 | 101 | **6** | 94 |

  These are the walkthrough's per-area totals (26 / 30 / 13 / 50 / 30 / 0 / 30) with no difference. `game::chapterRewards`
  lists them per chapter in table order (the PDA's AREA INFO and TREASURES rows, the EXTRAS "every treasure" / "every
  relic" unlocks); a treasure in an urn is listed by its born id.
- **Treasure urns** [O]: `*_pt_weightbreak_reward` (ar_ in l1 / l5 / l7, sa_, ma_, ca_) is the plain urn's uber machine
  (health 30, 0 -> 2 on any hit / impulse, "Breakables") with one more state-2 action: 0x13 (0x4D86A0 -> 0x4D3B50) gives
  birth to the record's object {u16 object 103 `tp_rw_artifact_spawned`, s8 model 0, i32 marker 0, no messages} with
  INSTANCE_BirthObjectNoParent at the section's HInfo marker (0x4705B0; the urn models' only one: id 0 at (0, 0,
  79.01)), then posts it message 0x1E8 with the urn. The born artifact's script (type 3664 over the artifact's 3671)
  differs from a placed artifact's only in the clip its birth plays (animation 10 of its list, a 100-byte still clip,
  against 8) and its sound's delay (0.5 against 0.2): it appears in place, nothing flies. Other users of action 0x13
  leave debris or effects (ship props `ms_uo_*` -> `*_spawned`, the exploding barrel, tentacle gear squibs,
  `revisit_motorcycle_spawner`): `ObjectsReal.OnlyRewardUrnsGiveBirthToPickups`.
- Ours: `formats::birthIntros` describes each urn's treasure as a dormant intro of object 103 at the marker, id
  `formats::bornUid(urn)` (urn id | 0x20000000 [ours]); `loadUnitData` makes it a `Pickup` with `parent` = the urn and the
  urn's reward index [I: 0x4D3B50 hands the urn's intro to the birth; the index is the urn's]. `World::uberAction`
  0x13 puts the id into `World::born` when the urn enters state 2, whatever sent the hit (`World::damageObject`: bullets,
  the pound, any other caller). From then on it is a pickup like a placed treasure; before, interact finds nothing
  there. The level scene carries its meshes tagged ` @dormant @intro <id>` and ou-game shows them and starts its
  effects once `World::introduced(id)` (scene rebuild on `World::bornVersion`). `born` is saved (docs/SAVES.md) and
  lives as long as the urns' uber state (a load replaces both). Streaming the unit out and in
  keeps both the broken urn and the treasure. Tests: `Pickups.BornTreasureWaitsForItsUrn`,
  `PickupsReal.{Mediterranean,Thailand}UrnGivesItsTreasure` (l1_baldrs_sideroom 94 = reward 12, l2_towerroom 1909 =
  reward 53: break, save with the treasure lying there, load, collect, save), `PickupsReal.BornTreasureSurvivesStreaming`,
  `PickupsReal.BornTreasureIsInTheLevelScene`; self-test `tests/selftests/treasure_urn.json`.
- **Collecting** (0x5DB6A0) [O]: sets bit n of the saved 2048-bit set (game state `+0x3ED90`), counts
  found[category][type] up the type tree (`+0x3EE90`), then for the group's category: heal = table `+0x58`[category],
  capped so that health <= maximum + `+0x64`[category], through 0x6F4EF0(amount, overheal = 1). Treasures give
  nothing (0 / 0). [I] the reach: interact within 200 units / 250 up or down of the item (`kPickupRadius`; the reward
  scripts use a `playersensor`, its size is not decoded) and the HUD line format.
- **First-pickup popups** [O, not shown in ours]: the reward base script 3674 opens a `uipopupmgr` popup once per
  game (a global stickybool): title locals 1287 "Rewards", text 1286 "The number of Treasures and Relics found so far
  is tracked in the PDA"; the health pack script 3924 uses 1291 / 1292 the same way. Ours shows the HUD line
  `TREASURES|RELICS|HEALTH PACKS  <count>` (1251 / 1252 / 1274) [I format].
- **Relics** [O]: Lara's maximum health is `difficulty table[level] +8 x Lara data +0x44 x (1 + found[relics] /
  total[relics])` (0x6E7990 -> 0x5D6170), read live: each relic adds a sixth of the base, all six double it. The
  pickup does not rescale health; it then heals 300, to no more than the new maximum + 50 (so a relic taken at full
  health leaves her 50 over, which decays as after a health pack). The game also adds 50 to a float at game state
  `+0x3EF90` per relic; no reader was found. The walkthrough's "heals fully" is the visible effect in most cases; the
  exe's numbers are used. Ours: `World::maxHealth` (base x PLAYER TAILORING level x (1 + `relicsFound()` / 6)),
  `Health::raiseMax` on the pickup, on a load (`World::syncMaxHealth`) and at a chapter start; `World::setTailoring`
  rescales as before (0x6F5160 keeps the fraction on a level change only). Tests: `Pickups.RelicRaisesMaxHealthAndHeals`,
  `PickupsReal.RelicRaisesMaxHealthAndHeals` (l1_kraken_room 321: 400 of 1000 -> 700 of 1166.7).
- **Health packs** [O]: g_healthpack data `+0xE8` = 1 per pickup, `+0xE2 / +0xE4 / +0xE6` = 100 carried at every AMMO
  level; 0x6F5E70 takes a pack only while she carries fewer (`kHealthPackMax`). A level start (player message 0x23,
  0x71FFE0 -> 0x7198D0) gives the unit's loadout: level settings `+0x2B4` -> {DTP id, alternate DTP id}, each DTP {->
  u16 weapon objects, count, -> u16 gadget objects, count}:

  | units | weapons | gadgets |
  |---|---|---|
  | l1 (DTP 3451) | wp_handgun, wp_speargun | g_binoculars, g_grapple, g_grenade_thrower, g_pda, g_personal_light, g_divinggear, g_lighter |
  | l2 (8214) | wp_handgun, wp_uzi | binoculars .. personal_light, g_lighter |
  | l3, prologue (11546) | wp_handgun, wp_shotgun | the same |
  | l4 (13122) | wp_handgun, wp_tranq | the same |
  | l5 (16172) | wp_handgun, wp_uzi | the same + g_gauntletleft, g_gauntletright, g_belt |
  | l6 (7013) | wp_handgun, wp_thorhammer, wp_assault_rifle | binoculars .. personal_light, gauntlets, belt |
  | l7 (18578) | wp_handgun, wp_thorhammer, wp_speargun | binoculars .. personal_light, g_divinggear, g_tanks, belt, gauntlets |
  | alternate, every unit (838) | wp_handgun, wp_uzi | binoculars .. personal_light, g_lighter, g_tanks |

  No list has g_healthpack, so Lara starts each level with none (the walkthrough's note that the inventory resets per
  level). Ours: `World::startChapter` zeroes the carried count. [U] the other branch of message 0x23 (game state `+0x390`
  set: 0x715EF0 rebuilds the saved slots and ammunition fractions): when it is taken was not traced.
  `Pickups.HealthPacksCapAndLevelStart`.

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
- Player sensors (vt 14) fire since 2026-11-10 ("Water moves"). Not done: the per-chapter equip scripts do not run in our runtime yet
  (probe of 12 chapter units: no equip call), so the gear stays hidden outside a volume with carried objects. The
  cinematic base type 3523's gear events 10-21 are never sent (no retail stream has events).

## Thor's gauntlets (heavy mechanisms)

- **Owning them** [O]: the gear is three objects in Lara's 15 object slots (`g_gauntletleft` 57, `g_gauntletright` 58,
  `g_belt` 56; larabase 37 gives, 38 tests, 42 drops: "Thor's gear" above; ours `script::Runtime::playerObjects`,
  saved, `game::chapterGear`). No global flag: what asks for them is the **engagement record** of each mechanism.
- **The requirement** [O data + tru.exe]: a push / pull record (kind 1) lists object ids at `+0x6C / +0x70`
  (`physics::Engagement::needs`). 0x6F3C60 admits Lara to a record with a list only if 0x6F3BB0 finds one of the
  objects: the portable in her hands (`+0x858`), else her weapon slots (0x6E7E10), else her 15 object slots. Records
  (dump `OU_UBER_SECTIONS=<object>`, now with the needs and every section set):

  | object (units) | record | needs | script strength |
  |---|---|---|---|
  | `sa_ei_thor_slide_door` (l2_maproom 2661, 2662), `ar_ei_thor_slide_door` (l5_valgrind_tunnel 7400) | section 1, set 540: two half-cylinders r 711 on handle markers 11 / 12, cone 90 +- 90 | g_gauntletleft | 15 |
  | `ma_pu_thor_pillar` (l3_secretchamber 4674) | section 0: sphere r 948 | g_gauntletright | 3 |
  | `ca_pu_thor_pillar` 5610, `ca_pu_thor_debris` 5609 (l4_pool), `ca_pu_thor_block_rubble` (l4_ragnarok_room 5660) | section 0: cylinder r 790 / sphere r 948 | g_belt | 4 / 4 / 10 |
  | `ar_ei_thor_valgrind_door` (l5_valgrind 7334) | sections 0 / 1, sets 464 / 476 given by their state 1 | none | 8 |
  | `ar_pu_thor_tower_short / _tall` (l7_yggdrasil_end 8398-8400) | section 0: cylinder r 790, h 4740 | none | 5.5 |

  All are class-79 uber objects ("heavy" = their mass: doors 1005 / 1015, rubble 1000, pool pillar 500, towers 175 /
  224, manor pillar 150). Ours: `World::engaged` admits a record through `game::carriesNeeded` (held portable, else
  `playerObjects`). Before: a record with a list never admitted an empty-handed Lara, so the Thor records were dead, and
  `World::pushUberJoints` instead gripped ANY uber object riding a level prismatic joint from within 120 units of its
  collision and slid it at 120 units/s whatever its record, gear or brake (even while she swam): removed.
- **Engaging** [O]: Interact inside the record puts her in the PushPull state; its entry 0x73F4D0 builds the "PushPull
  Proxy" body, posts message 0x158 to the object (uber trigger 20) and calls its script's vt 49 (0x701310 -> 0x799900,
  prototype 67) with (record segment, kind); leaving posts 0x14F (trigger 21) and calls vt 50 (0x701350 -> 0x799950,
  68). The Thor base script (type 9744) answers vt 49 with `larabase` 35 (0x79A270 -> player `+0x1240`, **her pull
  strength**, 1 at birth; debug text "Overriding Lara pull strength to =") = its member +12 (table above) and vt 50
  with 1; the slide doors add uber command 0 / 1: braked state 2 (motor 0 speed, 3.16e6) <-> free state 1 (no motor);
  the rubble block does the same by triggers 20 / 21; the towers' vt 49 / 50 also switch a push camera (gamebase 50 +
  53 (4), back 53 (1); gamebase 50 is unbound in ours, so the towers' vt 49 stops there after setting the strength).
  Ours: `script::Runtime::engage`, `pullStrength()`.
- **The move** [O 0x74AC00 / 0x73F390 / 0x6F1450, lara.drm player data]: every frame the stick picks one of four ways
  by its angle off her facing: forward within 30 deg (+0xC8C = 60), back within 15 deg of straight back (+0xC90 =
  30), else left / right; the target speed is `min(23.704 (+0xC7C) x strength, 2054.36 (+0xC80) x strength / M)` and
  the impulse applied per step is a quarter of the velocity error x mass, at most `min(m x 0.7585 (+0xC84), 142.225
  (+0xC88)) x strength`. [I] M = the proxy's mass = 0.125 (+0xC98) x the object's (with the object's own mass the
  doors would crawl at 31 units/s; with the proxy's: doors 245 units/s, Valgrind gate 130, rubble 164, towers 130, the
  manor pillar 71) - not measured under Wine. Clips [O lara.drm 1659-1665]: `thor_engage` (15 frames), `thor_idle`,
  `thor_forward / _backward / _left / _right` (root motion 406-582 units/s, not applied), `thor_disengage`; [I] that
  they are the PushPull state's clips (the calendar handles use them too now).
  Ours (`World::pushUberHandle`, `game::pushWay`, `game::pushDrive`): Interact pressed inside the record and held
  grips, the stick (measured on the stick itself [I]) drives the prismatic joint that moves the section - a joint
  across her facing (the doors: she faces the handle, against the record's cone) by left / right, one along it by
  forward / back - as a velocity motor bounded by the impulse; a joint motor braking with at least that impulse keeps
  it still (the doors outside state 1), a weaker one is overcome by the difference (the retracted towers' 474 / 632
  against 5.5 x 132.7 / 142.2); she is carried with it. When the section swaps its record set (the towers locking,
  action 0x1D) she lets go and vt 50 runs before the new state's events. A press a switch took grips nothing.
  [U] whether the original toggles on a press instead of holding; the proxy constraint; hinge joints keep the old
  120 units/s at her grip point.
- **Results** (`ProgressionReal.ThorMechanismsNeedTheGearAndSlide`, each refused without its gear): l2_maproom door
  2661 (C09-05) and l5_valgrind_tunnel door 7400 (C16-04) slide their 663.7 units in 2.9 s by sidestepping; the
  l4_ragnarok_room rubble block 5660 (C15-12) 2054 units in 12.7 s; the l7 towers by their record
  (`ProgressionReal.EndgameCredits`, from floors inside it; before, the test pushed them while she swam). Synthetic:
  `Game.ThorGauntletsMoveHeavyObjects`; self-test `tests/selftests/gauntlet_move.json`.
- **Open**: l5_valgrind gate 7334 (its section never reaches state 1 in ours: vt 55 after `240_val_gate`; `larabase`
  54 = "is that her bike" 0x777E40 and 19 = "gauntlet charged" [U effect] stop volume 7511's and cinematic 7464's
  handlers: decoded, not bound, because binding them lets handlers elsewhere run on into cinematics untested here);
  `ma_pu_thor_pillar`, `ca_pu_thor_pillar` ride joint type 7 (planar, limit / motor records of kind 7) and
  `ca_pu_thor_debris` has no joint (a free body): neither is simulated, so C11-28 and C15-03 stay open; l2_maproom's hub
  and wall drag (C09-09 / -11) are not surveyed; the speed's M and the hold / toggle need a Wine trace.

## Natla (l7_yggdrasil_end, the final fight)

Survey of the unit's data and scripts (dumped from the type-8 sections and the object DRMs; nothing here is a per-level
rule in code: the level scripts run in the VM and call general natives).

- **Who does what** [O scripts]. Natla is `natla_end` 8403 (base script 19035: anim-graph states Idle 0 / Attack 3 /
  Warping 4 / PoofIn 5 / 7 / 8, clock +52 = 0.7 s). Five more `natla_end` intros stay hidden until their set piece: 8246
  (top of the tower, eight slow balls), 8233 / 8234 / 8236 / 8237 (one slow ball each). The three ring managers are
  dispatchers 8448 (lower ring: zone volumes 8649, 8652-8655, 8665), 8450 (middle / upper: 8647, 8648, 8651, 8664) and
  the zone selector 8460 with one dispatcher per zone (8452-8459). The yeti manager is 8451.
- **Flight** [O scripts]. She does not fly a path of her own: on every "warp" the manager poofs her out (graph state 4,
  message `natla_poofedOut_warpHer` on channel `natla_warp_channel`), then a warp point (`natla_end_mover` 8598-8637, a
  spline mover circling its zone) calls `instance` 25 on her and hangs her on its marker anchor: `instance` 8 (13) →
  `instance` 23 (thunk 0x798CA0 → 0x4AB320 → 0x5BE1A0: a marker anchor on an instance's HInfo marker; 23 = 0x4A41D0).
  She rides that mover until the next warp (`natla_warp_done`). Ours: `script::Runtime::anchors`, `World::objectFrame`
  (anchor chains) and `World::poseAnchored` (the anchored object's body follows, so it is drawn and found there).
  The marker sits on its bone as the host's anim-graph clip poses it (bind frame without a clip); [I] a host without
  the marker gives its own frame. The same natives hang the 24 `ar_ani_floating_runestone` platforms on the markers of
  the eight `ar_ani_runestone_orbit_a` rings, three to a ring, so they now go round with the rings' clips.
- **Fireballs** [O exe + data]. Each `natla_end` hangs its weapon intro (`wp_natla_fireball` 8433, `_adren` 8238-8258)
  on its own marker 13 and fires it with `weaponbase` 0 (bind 0x7B69F0, thunk 0x79E2E0 → 0x7784F0): the weapon's stats
  block (query 0x36, data +0xE0) gives the launch speed (+0x10) and the fire message 0x19B carries the **position of the
  target instance at that moment**: no homing. Decoded (`game::loadObjectWeapon`, `CombatReal.NatlaFireballWeaponsDecoded`):

  | | wp_natla_fireball → pr_natla_fireball | wp_natla_fireball_adren → pr_natla_fireball_adren |
  |---|---|---|
  | speed (stats +0x10, per 30 Hz frame) | 300 = 9000 units/s | 50 = 1500 units/s |
  | direct hit (fire record +0x1C) | 0 | 3000 (three times her health) |
  | blast (record +0x44, kind 2): damage centre → edge, radius | 250 → 50, 650 | 250 → 50, 200 |
  | knock centre → edge, degrees up (+0x58 / +0x5C / +0x54) | 50 → 20, 10 | same |
  | gravity (fire record +0x4C) | 0: a straight line | 0 |
  | +0x80 | 150 frames [I: ours drops a ball after that flight time, 5 s] | 150 |

  Ours: `World::fireObjectWeapon` / `tickHostile` (Combat.cpp): the ball flies from the weapon's point (its anchor chain)
  straight to the target point; the first world face, enemy or Lara's capsule it meets sets it off; a direct hit takes
  the projectile's own damage, then the explosion record goes through `World::blast` (the squibs' area hit 0x69F1E0),
  which now also throws Lara off her feet (message 0x91 → `throwLara`) as it throws every other instance. Balls in
  flight are saved (`hostile` in the combat block, written only while one flies). [U] the knock's unit (50 is used as
  units/s like the grenade's 300: a small shove, but she is thrown down); whether +0x70..+0x80 describe a burn.
- **What she throws at** [O scripts]. On the rings 8403 does **not** aim at Lara: the manager picks floor targets of
  Lara's zone (dispatchers 8461-8522, e.g. 8463 / 8464 on the lower ring by tower 8399), calls the target's vt 62 (1.5)
  and Natla's vt 60 (target); 0.7 s after her attack state starts her clock fires the ball at the target (`instance` 28).
  Two balls per stop, about 2 s apart, then she warps: a pair every ~5 s (measured: throws at 2.72 / 4.73 / 7.77 /
  9.78 s after Lara enters a lower zone). Without a target override the ball goes at Lara (`playerref` → `instance` 14):
  the hidden set-piece Natlas do that.
- **Burning patches** [O scripts]. Every floor target owns a trigger volume (8523-8584, a 600-unit box) and the shared
  effect 8585 `fx_natla_fireball_explosion`: 1.5 s after the throw was ordered the target's clock moves the effect there,
  plays it and switches the volume's damage on for 3 s (second clock). While on, Lara inside it takes `health` 2 (100)
  at once and every second (the volume's state 1 loop). So a ball costs her up to 250 from the blast and 100 a second
  while she stays in the fire; standing between two targets (640 and 1100 from them) costs nothing.
- **Timed dodges** [O scripts]. Volumes 8323 (tower top), 8324, 8325, 8327, 8329 (climbing walls) start adrenaline
  trackers 8318-8322 (time scale down), reveal the matching hidden Natla with a camera (reveals 8313-8317) and have her
  throw the slow ball(s) at Lara: 1500 units/s and 3000 on a direct hit, so only leaving the line saves her.
- **Hitting her** [O scripts]. Damage to 8403 (vt 22, any `damagetype`) posts `hit_by_player`; the lower-ring manager
  counts them and at the second one ends its fight (vt 66: fireballs off, yetis off, manager 8450 takes over). She has
  no health of her own.
- **Yetis** [O scripts]. Lower ring: the four `yeti_thrall_pool_generator`s 8675-8678 are enabled when Lara enters a
  lower zone (`generatorbase` 3) and each `yeti_dead` message asks for the next (manager vt 65 → 8451 vt 69) until ten
  have died. Upper rings: 8451 gives birth to dormant yetis: vt 63 three (8667, 8668, 8669: volume 8650, the page's "1 +
  2"), vt 64 one (8667), vt 65 one (8672), vt 67 one (8674), each once, with a reveal camera (8339 / 8341).
- **The pool** [O scripts]. Ten flow volumes (8209-8218, 8438-8446; surface −316.2) lie under the lower ring; 8209's
  thread polls `larabase` 3 for each every 0.1 s and kills (`health` 2 (12345), "Goo death") when Lara is in one. The
  eleventh volume 8309 is the finale's flood: its constructor sets it to marker 8680 (z −11116.5) at once, `goo rise`
  (vt 56) to 8681 then 8682 at speed 3, `goo rise higher` (vt 57) to 8683 at speed 2. Two fixes made this work:
  `marker` **locals** start as their declared intro (Vm.cpp; before, 8309 kept its box top, z 2869.5, and flooded the
  lower ring from the start), and the VM's 100 000-instruction budget now bounds one resume, not a thread's life (the
  kill thread died after ~90 s).
- **Towers, mounts, checkpoints**: "Thor's gauntlets" above; docs/PROGRESSION.md chapter 7. Checkpoints 8178 / 8179 /
  8157 are activated by towers 8398 / 8399 / 8400 (vt 54 links), 8180 and 8242-8245, 8248 by the rings' volumes.

- **A patch burns who walks into it** [O scripts]: the volume's OnIn (vt 70) starts the burn thread only while its
  damage is on, so standing on a target when the ball lands costs the blast alone; walking into the flames costs 100 at
  once and 100 a second until the patch goes out (the thread does not ask whether she is still inside).
- **Route geometry met by input** (`ProgressionReal.NatlaFightByInput`): the unit's start walkway (z 7383) ends in a
  ledge at x −26750 (hang, drop to z 6160), a ramp runs down into arena volume 8303 (x −14800, z 4724); 320_hel_boss
  leaves her on the middle ring's entry walkway at (−8143, 793, 4321), which ends at x −6620 over the rings (lower ring
  z 474, 3850 below: a fatal fall, so the original's way down is the rope). There no zone volume holds her and Natla
  throws nothing.

Tests: `CombatReal.NatlaFireballWeaponsDecoded`, `CombatReal.NatlaFireballsBlastThrowAndBurn` (throw from her warp
point at 9000 units/s, blast on the floor target, 50-250 damage and thrown down, the patch's 100 a second on entering
it lit, nothing between the targets, save round trip, pool level, no script stop; Lara placed on the ring),
`ProgressionReal.NatlaFightByInput` and self-test `natla_fight` (input only: the entry to 320_hel_boss, the towers
retract, nothing thrown at the entry walkway, the walkway's end), `ProgressionReal.EndgameCredits` (the whole script
chain to the credits, Lara placed at the six interactions).

Open (the fight is not yet playable from the entry to the credits by input alone):
- [U] the way from the entry walkway to the rings by input: the platform ride and the rope down (matrix C21-01 / -02).
  The 24 floating stones hang on the orbit rings' markers and the rings' sections are born by dispatcher 8360 as the
  buttresses break; whether Lara can stand on / grab them as they move is untested (moving ledge markup: none).
- a level start on the lower ring (a `--spawn` there, not a checkpoint load) dies at once: the World's first tick finds
  her inside pool 8309's box before its constructor has lowered it (box top z 2869.5), she treads its surface down to
  the marker, and 8209's goo flag, once set, kills at every poll. A tread that loses its surface by more than a step
  should fall instead [U: tru.exe's rule]; the rings' input tests and a ring self-test wait for that.
- the two chimney climbs, the tower's moving handholds, the buttress slide (C21-07, -12, -21, -23): no input route.
- Natla as a shot target: nothing in ours delivers a hit to 8403's vt 22, so the lower ring's fight does not end after
  two hits as the scripts provide; the manager keeps throwing while Lara stays on the ring.
- [U] yeti counts actually spawned by the pool generators (enabled by the scripts; `ai::Enemies` spawns by its own
  generator data) were not measured here; the upper rings' births are script calls (above).
- [I] the knock's unit, the 5 s flight limit, lingering-flame fields of the explosion record (+0x70..+0x80).

## Controls

- Bindings live in `config.json` under `controls` (`bindings`: action → list of inputs; `mouse_sensitivity` 0.1..10,
  a multiplier on 0.0025 rad/pixel; `invert_y` / `invert_x` for mouse and look stick; `move_stick` / `look_stick`
  `left` | `right` | `none`, defaults left / right). `ou-game --config` reads them; actions a file omits keep their
  defaults, an unknown action, input or stick name is an error. Edit them on the launcher's Controllers tab (Add input…
  captures a key, mouse button, pad button or trigger; Esc cancels; "Move with" / "Look with" pick the sticks) or in the
  game (below); either adopts what the other wrote.
- Actions: forward, back, left, right, walk, sprint, jump, drop, interact, draw, fire, aim, switch, weapon, heal,
  grapple, save, load, hint, answer, pda, light, grenade, recenter, camera, pause (Esc / Start), and the menu keys
  menu_up / menu_down / menu_left / menu_right (arrows, WASD, d-pad), menu_accept (Return, Keypad Enter, Space, A),
  menu_back (Esc, B, Start), menu_delete (BackSpace, Delete, Y: deletes a save slot, UNMAP), menu_alt (E, X: the PDA's
  TASK). Prompts name the bound pause / menu keys (Return / Escape / BackSpace placeholders 25..27).
- In-game GAME OPTIONS (tru.exe's screens [O `openunderworld-work/screens/controls/keyboard1.png`, `gamepad1.png`,
  `buttons1.png`, `buttons2.png`; locals.bin 492 / 493, 1007..1015, 1037..1063]): CONTROL CONFIGURATION has MOUSE
  SENSITIVITY (slider 0..10 = 2^((v − 5) / 2.5) [I]), BUTTON CONFIGURATION (one row per action in tru.exe's order, ours
  after it; the inputs of the device in use joined by "/", pad buttons as their icons; 15 rows scroll; Return SELECT
  waits for the next key / mouse button / pad button / trigger ("Press a button for the selected action", 1063) and
  makes it that device's only input for the action, Escape cancels [I]; BackSpace UNMAP clears the device's inputs,
  refused on the menu keys [I]; its VEHICLE rows are our forward / back / jump / fire and not listed), RESTORE DEFAULTS.
  GAMEPAD CONFIGURATION shows the pad in use (else "Disabled", 7), VIBRATION (pad rumble on / off, "Vibration and
  camera shake"), GAMEPAD AXES (the
  stick layout: default, southpaw, move stick only, look stick only [I: the original's values were not captured]),
  INVERT X-AXIS / Y-AXIS. Every change applies at once and is written back to `--config`.
- Gamepads: every connected pad is opened (hotplug); the first one added is used, the others are ignored; when it is
  unplugged the next pad to press a button or push an axis past half takes over. Buttons are positional (SDL's a / b /
  x / y = south / east / west / north on Xbox, PlayStation, Nintendo and Steam Deck layouts); the Steam Deck's / Elite
  pad's back buttons bind as `Pad paddle1` .. `Pad paddle4`, Steam Deck trackpads reach the game as Steam Input's mouse.
- Recenter camera [I: the press snaps the orbit camera's yaw behind Lara at the rest pitch; the orbit reseeds from those
  angles at its distance]. Q is also the grapple's key, as in the original.
- Field camera (tru.exe `LookAroundState`, vtable 0x8842C0; viewfinder `pc-w\g_binoculars.drm` texture 649, a
  translucent black frame with zoom - / + marks [O data]): the press, while she is on foot (idle, walk, run, stop,
  turn), switches to a view from her eyes along her facing; she holds still, look input aims it, forward / back zoom
  (full in 1 s, 0.75 x FOV down to 6 degrees, look slowed with the zoom), the next press or leaving those states ends
  it and the orbit camera swings back behind her. [I] the eye view, the zoom range and keys: the state's tuning (player
  variables 0x200..0x25F read by 0x75B7F0 / 0x75BD30) is not decoded, and its clips (g_binoculars.drm animations
  2329..2331) are not played. Inputs: SDL scancode names (`W`, `Space`, `Left Shift`, `F5`), `Mouse Left/Right/Middle`,
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
  | 2 | Crouch/Roll (drop, dive) | Left Ctrl | B | drop (carrying a portable: puts it down; target lock + stick: evasive roll) |
  | 3 | Jump | Space | A | jump |
  | 1 | Grapple | Q | X | grapple |
  | 0 | Interact | E | Y | interact (nothing to interact with / a melee target: the kick, "Close combat") |
  | 5 | Shoot | H, mouse left | RT | fire |
  | 7 | Grenade (throw held object) | Caps Lock | RB | grenade (carrying a portable: throws it, "Carried objects") |
  | 9 | Manual Aim | Z | right stick click | switch (change target; no manual aim yet) |
  | 10 | Pause | Esc | Start | pause |
  | 11 | Access PDA | Tab | Back | pda |
  | 12 | Use Medipack | V | d-pad up | heal |
  | 15 | Change Weapon | R | d-pad right | weapon |
  | 20 | Walk | Left Alt | — (light stick) | walk |
  | 14 | Light | F | d-pad left | light |
  | 13 | Camera (field camera) | T | d-pad down | camera |
  | 4 | Target Lock (draws) | G, mouse right | LT | aim |
  | 6 | Sprint (swim faster) | Left Shift | LB | sprint |
  | 8 | Recenter Camera (patch 1.1) | Q | left stick click | recenter |
  | 23 / 24 / 22 / 21 | Vehicle Gas / Reverse / Brake / Shoot | W / S / Space / H, mouse left | RT / LT / A / RB | stick forward / back; the jump action is the Vehicle Brake (no hop: Wine, "Motorcycle"); pistols / uzi shoot |

  So Lara runs by default; walking is Alt held (or the stick pushed lightly) and Shift sprints. Ours without an
  original key: draw / holster X (target lock and fire also draw), save / load F5 / F9, hint / answer F2 / F3. A config
  written before 0.12 (Shift walk, Alt sprint, …) gets these defaults for every action still on its old default;
  rebound actions are kept.
- Sprint (Tuning in Game.hpp; tru.exe under Wine, l4_trailstart, `openunderworld-work/screens/controls/s_*.txt`):
  sprint held while running plays `sprint_loop` (root motion 1927 units/s; tru.exe 1851..1939, mean 1915, on the
  straight of the l3_main_bedroom corridor, `openunderworld-work/jumpfid/sprint.txt`; ~1860 on a curve) for 9.7 s of
  stamina (measured 9.5..9.9), then `ng_sprint_to_exhaust` + `ng_run_exhaust` (no refill while they play) and `ng_run`.
  Stamina refills a second per second of anything else; a sprint starts again once 0.9 s is back (held throughout:
  ~2.2 s running, 0.9 s sprinting; 4 s without it after exhaustion: 4.1 s). Crouch while sprinting rolls with
  `ng_roll_front_sprinting`.
- Fixed: F1 toggles the overlay, F7 cycles the water debug view; the look stick turns at 2.5 rad/s at full deflection,
  both sticks have a 0.15 dead zone.

### Recorded input

Every keyboard, mouse and gamepad event ou-game handles goes through one function (`handleInput` in
`src/app/game/main.cpp`): it updates `platform::DeviceState` (keys, mouse buttons, the pad in use and its buttons /
axes, built from SDL events alone), which `game::readDevices` maps through the bindings each tick, and it drives menu
navigation, BUTTON CONFIGURATION captures, the pause key and mouse look. `ou-game --record-input FILE` writes each event
with the tick it preceded; `--replay-input FILE` feeds a recording back through the same function before that tick
(live devices are ignored; render rate 60 Hz unless `--render-hz`). Lines (`platform::eventLine` / `EventLog`, SDL
numbers): `<tick> key <scancode> <0|1> [repeat]`, `mouse <button> <0|1>`, `motion <dx> <dy>`, `padadd <id>`,
`padremove <id>`, `button <id> <button> <0|1>`, `axis <id> <axis> <value>`; `#` comments. Not recorded: cinematic skips
(the movie player polls its own events). Tests: `Game.ReplayedDeviceSessions` (keyboard + mouse and gamepad sessions with
a hotplug, synthesised SDL events, recorded and replayed tick for tick), `game_input_replay_smoke` (ou-game replays
`tests/replays/ballcourt_door_lever_keyboard.events` and `_gamepad.events`, each through the pause menu, and must end
where `ballcourt_door_lever.txt`'s scripted actions do).

### Receptacle replays and the autopilot

`ou-game --autopilot PLAN --record-script OUT` (developer tool, `src/app/game/Autopilot.*`) steers Lara through a
plan and writes the actions it gave as an input script; the script replays the same ticks (each value round-trips
through its text, the stick is re-aimed every 4 ticks in 0.05 steps from the camera's yaw, as `World::tick` maps it).
Plan lines: `go X Y [R] [walk]`, `engage UID [X Y] [walk]` (to the nearest of its engagement records,
`World::engagementCentres`, until `World::engaged` names it), `face YAWDEG`, `press [ACTION]` (6 ticks held, 6
released; `press` picks up / inserts, `press drop` puts a carried object down, `press grenade` throws it: follow each
with a `wait` for its clip), `hold ACTION SECONDS`, `stick FORWARD RIGHT SECONDS [ACTION AT]`, `run YAWDEG SECONDS [ACTION AT]` (a world
heading; ACTION pressed AT seconds in: running jumps), `push FORWARD RIGHT SECONDS [ACTION]` (ACTION, default interact,
held throughout, the raw stick from the 6th tick: push blocks grip, then move along the block axis nearest her facing
while the stick's forward is over 0.5), `wait SECONDS`, `near R` (logs the markup polylines within R: poles, ledges).
It logs where each engagement, press, push and run ended. `--no-cinematics` leaves script cinematics
unshown (their events, Lara's placement at the stream's tails and the chapter change still run): a spawn inside a
level-start cinematic's trigger volume (l4_traprooms 195_mex_lords, l5_valgrind 240_val_gate) is placed at the
cinematic's end on tick 1 either way, so the replays start from there.

`--report-objects UID,...` (with `--frames`) prints at exit `ou-game: calls TO:VT ...` — every distinct script call
(callee, vt; -1 = constructor) to or from the listed objects, in first-call order — and one line per object:
`ou-game: object UID NAME pos X Y Z [graph S] [joint P] [uber S0,S1,.. jN X ..] [in R] [body X Y Z]` (anim-graph state
id, the multibody joint's position, the uber section states with `-` in a transition and the simulated uber joints, the
receptacle it is inserted in, the translation of its own rigid motion: doors on their joints, sliders). UID 0 lists
every script object (finding uids for a plan).

The replays (`tests/replays/*.txt`, ctest `game_replay_<name>`, CMake `ou_receptacle_replay`: the real ou-game under
`--no-cinematics --sync-loads`) check the `portables held N inserted P>R` exit line, Lara's state and the puzzle's
downstream reaction as tru.exe's scripts define it (`--report-objects`; PROGRESSION.md has the chains):

| replay | spawn | what happens | asserted reaction |
|---|---|---|---|
| `traprooms_throne_insert` | l4_traprooms 7400,-34900,4440,-135 (cinematics leave her at 8101.6,-32185.8) | scepter weight 5883 picked up, set into the empty throne 5891 (uber section 0) | throne section 0 state 2, its press plate in "in position" (-150..-90), seal 5701 vt 54 |
| `valgrind_cog_gem_insert` | l5_valgrind 0,3072,180,180 (cinematic: 1517.3,-6209.9) | round the central column (waypoints) to power gem 7498, carried to cog wheel 7312, set into section 1 while the wheel turns | cog wheel section 1 state 0 -> 1 |
| `valaskjalf_pole_lever_insert` | l5_valaskjalf_entryb 1700,-45719,-2499,180 | pole 7190 picked up, inserted into pole-hole lever 7189 | lever graph 1 -> 0 and its slider watch (a joint) |
| `puzzleroom_pole_remove` | l2_puzzleroom -17500,40565,5540,180 | walks west until pole 2967 engages (its kind-4 record reaches 3.9 m), pulls it out of floor hole 2969 | elevator 2960 vt 54 (graph 0 -> 1), switch 2961 vt 62 |
| `ballcourt_pole_lever_block` | l4_ballcourt -20300,14700,3484,-90 | pole 6503 inserted into lever 6502, push block 6504 pushed along its axes (21° for 6.2 m, -69° for 1.9 m) under the inserted pole; a jump onto the block (its ledge loop), a standing jump from its top to the pole | lever graph 0 -> 1, its slider at -600 under her hang (state polehang), block under the pole, door 6505 raised 1200 |
| `baldrs_plates_gate` | l1_baldrs_tomb -5000,-10000,2560,180 (west room) | the room's full solution: weights 220 and 218 picked up (Interact), carried onto press plate 219 (door 223 opens while she stands on it) and thrown through the door (Grenade, `press grenade`), a running roll through the door before it shuts, both weights put down (Crouch, `press drop`) side by side on plate 222, gate 45's ring grappled from in front, she walks back roped onto plate 221 and holds Interact | plates 221 / 222 at their pressed joint (-50), door 223 vt 57 / 58, locks 69 / 70 out (-1250), gate 45 vt 54 and its joint pulled open (>= 85°, 91), the weights lying on plate 222 |
| `ballcourt_calendar_handle_turn` | l4_ballcourt -14240,10200,3700,90 | round calendar 6506's east side, grip a push handle and push 4 s | the ring turned on hinge joint 2 (> 0.2 rad, ~0.56), section 1 back in 3 |

Objects' ledges and poles [O data, tru.exe 0x5D0610 -> 0x5CE4F0]: an instance whose model carries markup items (model
DTP +0x54 / +0x58, docs/formats/COLLISION.md) owns them as markup boxes that move with it. Push block `ca_pu_block_a`
(l4_ballcourt 6504) has a ledgegrab loop round its top, 410.8 up; `ar_pu_valaskjalf_entry` (l5 7191) the same at
410.9 (Wine, session 5: a jump next to where it stands put Lara on it, 414 over the floor). Ours (`World::tickObjectMarkup`):
each object's model-0 items after the units' markup in the merged collision, re-placed every tick by its first collision
segment, so the block's edges are ordinary ledges that ride its pushes: up against a face, a jump hurdles onto it (class 6,
`hurdle_ground_attach_mid`), and from the edge she drops to hang or shimmies like on any ledge. Standing on block 6504
pushed under the inserted pole 6503, a standing jump's pole leap catches the pole (695 over the block's top), and the
hang slides lever 6502 and raises door 6505 (`ProgressionReal.BallcourtBlockUnderPoleReachesIt`, the replay below).
Every model and segment, objects without collision and the ride: "Moving markup" below. Portables add none (an
inserted pole keeps tickInserted's horizpole).
Wine [O, openunderworld-work/poleattach/session6.sh -> s6.txt]: block 6504 at (-16042.5, 3478.8, -11969.6) yaw
-2.7737 (tru.exe frame); a standing jump with Lara placed 260 from its centre, facing it, squares her to the face
(heading -1.2029 = the block's yaw + 90°), attaches and ends standing on the top at +409.8 (the ledge 410.8). Walking
into the face and jumping with the stick still held vaults over it instead (root up to +425.6, down 350 past the far
face) [U: why 0x73DFF0 classes that one 0 (over): its down ray 353 past the ledge point should meet the 379-deep top;
ours climbs onto it both ways].

Not replayed [U]: hanging on the inserted l5 pole (door 7227, `PoleHoleLeverOpensRewardDoor` drops her onto it). The
pole's markup is 1164 over the floor she inserts it from (2742,-46810..-46296, z -1339), above the standing jump's
reach (ledges 635-910 over her feet); no unit markup lies within 12.5 m of it. Block 7191 (climbable, above) stood
~5670 (20 m) from lever 7189 in Wine (Lara on it at 11637,-14901 vs the lever at 16965,-16856; whether it slides
there is [U]), so the route up the original intends is not found yet.
l1_baldrs_tomb's west room (plate 219, both weights) is entered by the spawn: door 223 is shut from the east
and Lara alone on 219 lets it shut again when she steps off (219 vt 55 -> 223 vt 58), so a weight holds it.
Also not replayed [U: route]: the calendar headdress 6498 lies at -4143,6282,-917, 16 m below and 36 m from calendar
6506 (route unknown to the autopilot), and the shiva arms' gems 2964 / 2965 lie on the upper floor (z 5447) while the
arms are lowered from the lower floor (z 1149: volume 3023 offers grapples 2982 / 2983, 2982 vt 55 / 2983 vt 55 —
ou-game reaches that much from `--spawn -27614,38823,2700,0`), so those inserts stay with
`UberReceptaclesTakeTheirPortables` (teleports).

## Controller

- Ground: each tick the current clip's root delta (rotated by heading) is the desired move. A capsule from
  `feet + stepUp` to the head is swept and slid along walls (up to 4 iterations, contact normals flattened), then a ray
  from `feet + stepUp` down `stepUp + stepDown` snaps to the floor. Whether the face under her is stood on is `Player::floorKind`
  (section "Slopes and slides": by client flag and angle); a too-steep floor still counts
  when a standable face lies within 20 units of its height 60 units to a side (`Player::standable`: floor bumps, seams;
  the l4_ragnarok_room Fall/Run flicker at tick 127 of `tests/replays/pool_to_ragnarok.txt` came from 0.6-normal
  strips 0.5 units high). Nothing under her starts a fall with the last horizontal velocity; a slide-flagged face between 28 and
  52 degrees, or a too-steep floor of normal z 0.3..0.6157, starts a slide.
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
  On ledge 1253 the remaining snap is 8.7 units, the rock top sitting 9 above the ledge markup (it was ~150 before). (2026-11-10: those 9 units were faces of a terrain group that is not the player's, docs/formats/COLLISION.md; the rock top she
  stands on now is at the ledge's height.)
- Which way up from the hang (jump, stick neutral or up) is the hang's clearance class, tru.exe 0x74E290 (run on the
  attached ledgegrab in its segment frame by 0x76D010, kept in player data +0xEDC, read by the chart's
  `PSCCF_Markup_ClearanceResult`) with lara.drm's player data [O: decomp]: a ray 252 above the ledge from 100 on her
  side to 139 over the top hitting something sends it to the shelf test 0x74E160 (0x74BB50 == 1: class 8, the
  wall-hug shelf; == 2: 11), else 1; otherwise the first surface down from 252 over the point 139 over the top must lie
  30 below to 40 above the ledge (0x72BAE0, else 1), and the room from 252 up to 512.01 over the point 69.5 over the
  top gives 2 (all of it), 5 (from 260) or 1. The hang states (S530 / S531 / S542 / S548): class 2 → S551
  `ledgeb_climbup_fast` (T558), 11 → `ledgeb_jump_up` (T573; ours: the hang jump up, which needs a target), anything else → S439: 5 → S455 (onto the top
  crouching, `crouch_Idle` after; ours `ledgeb_jumpuptocrouch` [I: clip by name]), 8 with the attached markup 628 →
  S1251 `bbeam_flip_foward` → `Wall_Hug_Idle` (ours carried onto the shelf as the walk-on [I]), a balance beam as the
  jump target (T698, 625) → S882 → S885 `bbeam_ledgeu_climbup` (request 184) → the beam idle [O-data]. That clip has
  no root motion: her root stands on the beam line from its first frame, the hands 105 behind it and 28.6 to her
  side, on the ledge [O: clip]; ours takes the beam whose line lies within 100 over the top and 60 of the ledge's
  height [I: the hang-up query's pick]. Nothing else (class 1 without a beam or a hang-jump target): she stays hanging
  [I]. Before, every jump without the stick climbed up, so on l3_chapel's 63-wide scaffold beams (ledge loop 1009
  round beam 1031) `ledgeb_climbup_fast` carried her 102 past the ledge and she fell off the far side.
  `GameReal.ChapelHangClimbsOntoBalanceBeam`.
- Turn in place: the yaw advances by the change of `ng_turn_center`'s root Z rotation each tick (7.85 rad/s looped),
  towards the stick direction, clamped to the remaining angle.
- Interpolation: position, heading, swim pitch and clip time are blended between the last two ticks.
- Crouch (drop held on the ground): `crouch_idleToCrouch` → `crouch_Idle` / `Crouch_Front` (328 units/s root
  motion), capsule height 260 (player data +0x14); releasing drop stands up only with headroom (a capsule sweep up to
  512). Drop pressed while running: `ng_roll_front` (1364 units/s root motion), then run or idle.
- Slide: down the fall line of the floor under the root (heading = downhill): on a slide-flagged face at the slide
  controller's speed, on a too-steep floor accelerated by g·sinθ (section "Slopes and slides"); a standable floor ends
  it through `slope_slide_stumble_off`, no floor below falls, jump leaps off with the slide velocity + the standing jump’s 1441 up.
- Balance beams (markup 14, MUD BALANCEBEAM): from the ground, the root within 30 of the line (x/y and height, within
  the line's extent) heading along it (cos ≥ 0.7) gets on with `bbeam_walk_attach`; falling through the line from above
  lands on it with `bbeam_jumpland` (any heading). On the beam the root is pinned to the polyline: the stick along the
  walking direction plays `bbeam_walk`, whose forward root motion moves her along the line (joints followed); against it
  `bbeam_walk_turn180` turns her round by its root rotation. At an end: another beam ending within 60 takes over (she
  turns onto it), a standable floor 80 beyond is stepped onto (Walk; 0.5 s before a beam can catch her again), else she
  stops. Jump = standing jump (the assist below puts the landing back on the line), drop = falls off to her right [I].
  Off balance [I: the walk's stick rule]: walking with the stick more than 0.3 across the beam plays `bbeam_walk`'s
  blend space (lara.drm request 192) fully toward `bbeam_unbalance_walk_right` / `_left` (same root motion, 501 per
  3.57 s cycle), standing with it more than 0.5 across `bbeam_idle_wobble_fwd` (request 158).
  About to fall, falling off, the catch (2026-11-09, `Player::beamAboutFall` / `startBeamFall`):
  - Chart [O-data] (lara.drm player chart, openunderworld-work/hurdles/sc.py): beam root S873 (action 572: the beam
    heading, 0x76DCD0 / 0x7326B0 / 0x732500), walking S934, standing S876 / S921, idle S949. S897 dispatches on
    `PSCCF_MoveDirUnbalanced` (cond 0x199, 0x761780 → 0x7868F0: the sign of the controller angle chart context +0x390,
    hysteresis = property 0x1FB degrees; value 412 = positive = left, 413 = right) to the about-to-fall states S896 /
    S895 (request 177: `bbeam_aboutfall_center` / `_left` / `_right`, loops of 1.2 s without root motion). The fall
    flag is cond 0x200 (byte +0x3BD = balance controller +0x65, 0x764C30). From about to fall: flag + side → S893 / S890
    (`bbeam_fall_left` / `_right`, requests 187 / 190); flag clear → back to the beam (S935 with the stick, S919
    without); jump → the beam jumps. From walking (S934) the flag drops her straight off (T1578 / T1579); it enters
    S897 on kind-1000 conditions (T1597 / T1602 / T1604) not decoded [U]. From idle (S949) she falls only on a
    scripted wobbly beam (cond 0x294: an object spawned on entry, 0x75E5C0 → 0x70CC50, reports state 3).
  - Balance controller [O] (chart context +0x358; action 301: enter 0x742330, update 0x742770 every frame, exit
    0x72A5E0; slots: the action node's f1 runs on entry 0x785A30, f4 every tick 0x7857D0, f3 on exit 0x7859A0). Tuning
    by state property 0x209 through 0x72A430: walk (0x193) objdata +0xE98 = sway ±50° at 0.5..1°/frame, tolerance 10°,
    fall angle 180°, limits 2 frames, a 1000-frame grace when entered without property 912 (objdata +0xE94 bit 0
    clear) — it never sets the fall flag; about to fall (0x191) +0xF88 = no sway, tolerance 15°, fall angle 100°,
    wiggle smoothing 0.35 / period 10 frames / penalty 1, grace 20, not improving 30, unbalanced 45, recovery 2,
    full lean 10 frames, flags 3. Every 30 Hz frame: error = stick heading − (beam heading + sway), the last stick
    heading held while the stick is released (+0x5C); |error| ≤ tolerance: balanced time; beyond: weight
    (|error| − tolerance) / (fall angle − tolerance), to the stick's side (left negative), unbalanced time, and with
    flag 2 the stick swung to the other side counts as balanced (the side holds). The fall flag needs a full lean
    (|+0x44| ≥ 0.99, objdata +0xE94 bit 3) held 10 frames while the stick is pushed, then: |error| > fall angle, not
    improving > 30 frames, or unbalanced > 45 frames. Balanced > 2 frames after leaning: recovered (flag, side and
    times cleared). Quick stick flips (mean period < 10 frames) narrow the tolerance and the limits by
    (10 − period) / 10. Lean target = 2·weight + 2·unbalanced time·side (objdata +0xE78 / +0xE7C) [O]; ours divides the
    time by 30 frames and moves the lean 0.1 a frame (the record's +0x28) [I: the spring 0x7431F1..0x7437D5 not
    reproduced].
  - Falling off [O]: the fall states' action 416 (0x76D4D0) runs query 64 (0.32 out + 0.32 up a frame, falling,
    gravity 5.72443, cone 90°, from frame 1) from her root moved 47.4075 to the fall side (property 912: 452 = +90°,
    453 = −90°); a ledge (flag 610) → `bbeam_fall_<side>` then the ledge hang (S526 / S527, under the hang superstate
    S556), none → `bbeam_fall_<side>_no_ledge` then the fall (S894 → S1012). Clips [O]: `bbeam_fall_left` 0.33 s, root
    172 to her left and 486 down, a quarter turn to face the beam, ending in the ledge hang pose (hands 146.6 in front,
    i.e. 25.6 off the line, 12.9 over it: l3_chapel's scaffold beams have ledge loops 31.6 each side); `_no_ledge` the
    same off-step facing on (458 down); right: 193 aside, 491 / 458 down.
  - Ours: the stick held more than 30° off the line for 45 frames (90 ticks) tips her into about to fall [I: the walk's
    entry conditions are not decoded]; there the controller above runs on the +0xF88 tuning [O]; recovered → the beam
    idle [I: the chart leaves on the flag being clear, which it is from entry]; the fall side = her lean (toward the
    stick) [I: the factor of +0x38 not traced]. The catch: a ledge (markup 1) whose hang faces the fall side with
    room under it, inside the ledge capsule round q64's arc within 10 frames and 200 of her [I: q64's class record
    0x31C40 not decoded; the wall-side test would refuse a fall from the top]; `bbeam_fall_<side>` carries her onto
    its hang (`warpStep`), then `ledgeb_idle`. No ledge: the `_no_ledge` clip's root motion (stopping on a floor met),
    then the fall with its last speed (0.2 s before a beam can catch her). Drop on the beam still steps off to her right
    [I] (`bbeam_drop_*`, S888 / S899 not ported). Saved in `player.controller.balance`.
  `Game.BeamOffBalanceEscalatesToFallAndCatchesLedge`, `Game.BeamOffBalanceCounterStickRecovers`,
  `Game.BeamFallWithoutLedgeDrops`, `Game.BeamFallIndependentOfRenderRate`, `GameReal.BeamFallsCatchScaffoldLedge` (l3_chapel
  beam 1031 off both sides onto loop 1009; l4_temple beam 1079 to her right onto ledge 1085, 31.6 off its line).
- Jump assist: at take-off a ledge (1) met by the take-off state's jump query arc (q2 / q43 / q26, 158+ over her root)
  re-aims the flight onto its hang pose by the hit frame, and the air grab catches it (running: a close catch is a
  straight 11-tick rush onto the hang, a far one an arc arriving at its top, the reach 1233; "Running jump reach");
  otherwise the landing is pulled onto the nearest jump-landing (2) or beam (14) line point within 300 of the natural landing (Numbers).
- Drop to hang: drop pressed standing within 200 of a ledge line at floor height (±40) whose far side drops away plays
  `drop_to_ledgeb`; her yaw and root are set so the clip (start frame 3, half turn) ends exactly on the hang pose of the
  nearest ledge point, root motion and turn played without collision, then Hang.
- Hang jumps: jump while hanging — stick neutral/forward climbs up when a walkable top lies within 150 above the ledge,
  else jumps up to a ledge above; stick sideways jumps to a ledge to that side; stick back jumps to a ledge behind
  (turning round with `ledgeb_jump_back`'s root rotation); with nothing in reach back or sideways she leaps off that way
  (tru.exe's free leaps, Numbers). Targets: the first ledge the hang jump query (q10 / q7, q8 / q9) meets by its
  class record (`Player::hangJumpQuery`, Numbers, "Jump query hit test"), never the ledge she hangs on (that is
  shimmying [I]) nor one whose hang side has floor (platform edges hung from the platform) [I]. Back and sideways the take-off clip plays first with its
  root motion (as tru.exe: she leaves the hang only when it ends); flight: ballistic with the exe gravity at the jump
  query's take-off vertical speed, timed to arrive exactly on the target's hang pose (`Player::launchHangJump`); then
  `ledgeb_attach_U/S/F`. The jump left is `ledgeb_jump_right` mirrored: lara.drm
  request 979 (its +0x20 bit 0x80; 1009 is the clip unmirrored) [O], pose reflected through the bone twins
  (docs/formats/ANIMATION.md "+0x20").
- Poles (markup 4 / 6): grabbed like ledges while falling, squared to the pole (`Hpole_perp_attach_M`) or, falling
  along it, parallel (`Hpole_para_attach_S`). Perpendicular: forward held swings (`HPole_StartSwing` → `HPole_swing`,
  root fixed); letting go stops at the loop's next phase boundary with the stop clip that starts there (phases
  7 / 5 / 19 / 10 frames: `Hpole_swingstopB` after phase 2, `C` after 3, `A` after 4; in `HPole_StartSwing`'s first
  phase `Hpole_startswing_endA`); back during the swing turns her round at the loop's end (`Hpole_swingreverseA`); back
  while hanging does nothing (tru.exe under Wine [O]); along the pole `Hpole_PerptoPara`, played in **0.30 s** as the
  original turns her [O]. The catch hands over to the idle hang (input again) **0.62–0.85 s** after it (three Wine jump
  grabs; ours 0.7 s: `Hpole_perp_attach_M` cut at clip time 0.83, it starts at frame 4). Parallel (`hpole_IdleHangPara`): forward moves
  hand over hand along it (`HPole_ParaForward_trans` → `HPole_ParaForward`, 281.7 per 1.37 s of root motion; the pole's
  end stops her), letting go `HPole_ParaForward_to_idle_L_hand` / `_R_hand` (the hand behind: first / second half of
  the cycle [I]), back `Hpole_Paraturn180`, across `Hpole_ParatoPerp`. A quarter turn the other way plays the clip
  mirrored (root yaw negated, as tru.exe's mirrored requests). Jump plays the fling clip (`hpole_perp_fling` /
  `hpole_para_fling`) on the pole and she leaves **0.64 s** after the press (Wine, l4_traprooms 1336 → 1337, presses
  1.6 s and 2.8 s into the swing: identical flights; 0.31 / 0.81 s seen without a target [U] the rule): to the next
  pole ahead with query q33's vertical take-off (**52.10** up a 30 Hz frame, gravity 5.72443) and the horizontal speed
  that brings her down to the catch point **242** short of it and **357** below it — 23.5 frames, the root 237 over the
  pole at the top, 1303 units/s across 1264 [O]. l3_main_hall pole 948 → ledge 946 (the edge of a platform 1052
  ahead, 152 under the axis): she lands on the edge **23.0** frames after leaving, (35.9 forward, 74.1 up) a frame
  (Wine, polefid2/mhsw.txt) [O] — not q33's vertical speed, so the flight time is the rule: ours flies to the nearest
  pole or ledge ahead in 23.5 frames (both samples within half a frame; the fling query q32's sweep fits neither) [I],
  the root onto a pole's hang pose (no jump at the catch) [I], onto a ledge with a floor on top one capsule radius past
  its edge, landing (tru.exe touches down 3 short of it and steps on) [I], else onto the ledge's hang [I]
  (`GameReal.MainHallPoleFlingLandsOnPlatform`). Pole-top jumps forward / back aim the same way. Nothing in reach: q33 as it is (**73.72** forward, 52.10 up a frame) [O]. Jump from the
  idle hang with the stick neither along nor across (chart T1231 / T1235 [O]) climbs onto the pole's top instead
  (`hpole_to_perch`, along it `hpole_para_to_perch`; root **19** over the axis, Wine 19.9): there `hpole_perch_ready`,
  the stick across leans into a jump (`hpole_perch_Jump_forward_Antic` / `_back_Antic`), jump leaps forward / back
  (`hpole_perch_Jump_forward` / `_back`, q33 / q34, aimed as the fling), along it sideways
  (`hpole_perch_Jump_left` / `_right`, q69 / q70: 45 out + 70 up a frame after the clip's quarter turn), alone straight
  up (`hpole_perch_Jump_up`, q35 53.73 up), Ctrl back down onto the hang (`hpole_perch_fall_to_hpole`) [O]; falling
  through a pole's top within 40 of its axis lands her on it [I]; the 45° stick bands are [I]; the top run along it
  (`hpole_perch_to_top_run`, balance-beam states) and the chart's clearance check are not ported. A jump (from the
  floor or a run) with a pole's hang **330** out and **0–250** up of her root [I] flies her straight there, no gravity,
  in **11 frames at 60 Hz** (0.183 s; Wine on 1336: a standing jump 315 out at 1718 units/s, a running one 208 out at
  1135, both 11 frames) [O]. Drop lets go
  (`Hpole_detach_dropPerp` / `_dropPara`). Clip choice by the stick against the hang [I] (the chart's conditions
  0x195 / 0x196, orientation values 450–453 [O]); the swing is clip-driven: no physics condition sits on the swing
  states [O], so there is no swing simulation to port.
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
  at turnRate; drop = SwimDive (`swim_surfacedive` root motion, 667 down); the stick held toward a wall whose top is
  50..400 above the water = SwimClimbOut (no button; "Water moves" below); floor within 400 below = stand up (Idle).
- Under water (SwimUnder): the stick swims (its forward share along the camera's look pitch, "Water moves" below), jump /
  drop held swim up / down; the swim direction's pitch turns
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
- Oxygen (Numbers, "Breath"): `World::tickAir` drains the breath while the head is under water (SwimUnder / SwimDodge /
  SwimDive / Surfacing / SwimSplash with root + 150 below the surface [I]: tru.exe's state-chart property 0x322 = 802,
  set on the submerged swim group S155, the surface dive S106 and the swan-dive entry S105 [O chart]) and refills it at
  the surface. Spent, `damageLara(10 % max, kDamagePeriodic)` every 20 frames (the health invulnerability window drops
  every other one, as 0x6F5030's priorities do: 10 % every 40 frames, dead 12 s after the meter ran out). Not with
  scuba gear, 1.5 times as fast sprinting, and the HUD rule: "Water moves" below. Saved with the runtime state.
- Currents (`waterFlow`, tru.exe 0x6ACB60): watervolume intro data +0x38 / +0x3A source / sink marker ids (the last
  intro whose +0x52 is the id; 0x69D690), +0x3C strength in units per 30 Hz frame, +0x40 radius, +0x4E bit 0 level only.
  Data: l7_yggdrasil_end's ten volumes pull toward marker 1 at 47.41 (1422 units/s), l7_arctic_sea's six at 15.8 / 47.41
  toward marker 10. Lara's swim adds it to her stroke (0x755A00) [I: added straight on, the exe steers to the sum].
- Scripted levels (`World::waterNative`, `waterStates_`, saved in the runtime state and hashed): watervolumebase natives (bind 0x7B6930): 0 SetFlow(source, sink, strength)
  (0x7781D0 → message 0x1B3), 1 SetLevel(depth, speed) and 2 SetLevelToMarker(instance, speed) (0x7782D0 / 0x778370 →
  0x1B4 {mode 2 bottom + depth / 3 the marker intro's z, speed}; script lengths × 3.1605 + 0.5), 3 SetFlowStrength
  (0x778430), 4 GetDepth ((surface − bottom) × 0.3164); handler 0x6AC340 sets target +0x5C and rate +0x60 (units per
  frame), 0x6C10D0 moves the surface. l7_yggdrasil_end's 8309 calls 2 at load (GameReal.ScriptSurvey), larabase 3
  (0x777780: Lara's volume +0x8DC is the argument and +0x914 < 0; 0x5D77E0 sets +0x914 = floor under her − surface when she is below the surface, else her z − surface; ours: `World::waterNative` "larabase", her probe box's volume with its surface over her feet [I]) is bound: l4_pool's 5608 polls it every 3 ticks and calls health 2 (12345, 0) on Lara in the goo, a kill. [O] tru.exe under Wine (l4_pool, 2026-11-06; tru = ours + (4003.8, 158, −6637)): 0x710A20 tests her probe box against the volume's own box (instance data +0x34 centre, +0x40..+0x54 extents; no surface-mesh footprint), as `findWater`; +0x914 = −79.5 on the goo bed (floor 2582) and she dies; on the walkways (2844) and rocks (2865) inside the same box +0x914 > 0 and she lives. Ours gives the same outcome at the same eight spots (dies at (0, 0), (−2000, 2000), (−6072, 2898), (3210, 2429); lives at (1884, −3042), (5000, 3000), (−6610, −4845), (−3000, −4000)). A save goes on the same (`StreamReal.SaveContinuesBitIdentically` goo case: saved with the loop sleeping, both runs die in the goo); the earlier "loaded run takes no damage" was a save taken while dead, which loads at full health by design. `GameReal.ScriptedWaterLevelsRun`: the watervolumebase calls of 8209 / 8309 / 5608 run. 8309's instance argument is null in our runtime (an unbound instanceref) and the exe's thunk skips a null one [U: what it names]; `GameReal.ScriptedWaterLevelMoves` drives SetLevel (100, 10 -> 316 units, 32 per frame), GetDepth and a save. Marker ids resolve through the unit's intros (`UnitData::markers`).
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

### Water moves: scuba, breath meter, dodges, swan dive, surface turns, climb out, weapons, jellyfish (2026-11-10)

- **Scuba** [O rule, I mapping]. tru.exe 0x6EEF50 looks at the gadget in Lara's inventory slot 10 (player +0x500, 15
  slots of 0x10) and returns its in-use flag (weapon component +0x54 bit 0x4000, the flag 0x6A2990 / 0x6B17C0 set for
  the binoculars and the light). While it is set the breath function 0x755230 takes its not-submerged branch (the
  meter refills, no drowning) and the swim update 0x7572F0 hides HUD element 2 (the breath meter) under water. Ours:
  `World::scuba()` = the outfit is `lara_wetsuit_*` [I: slot 10 = `g_tanks`, in use in the wetsuits; Wine had shown
  the wetsuits' mask / regulator draw group on while she swims, `laraHiddenGroups`; the slot object was not read
  live]. So the Mediterranean (l1_*, wetsuit_warm) and Arctic (l7_arctic_sea .., wetsuit_cold) dives never drain;
  Thailand, Mexico and every other outfit do. Tests `Game.SwimScubaKeepsTheBreath`,
  `GameReal.SeafloorScubaSwimKeepsTheBreath` (150 s under water in l1_seafloor: full), `GameReal.
  ThailandDiveDrownsAtTheBreathTime` (l2_cliffclimb river: first loss 4020 frames after going under, dead 12 s later,
  `death_drown`).
- **Breath numbers** [O]: lara.drm player data (identical in every outfit DRM) +0xF8 4000 frames, +0xFA 60 frames to
  refill, +0xFC 10 %, +0xFE 20 frames (Numbers, "Breath"). The drain is multiplied by chart property 0x392 = 914 of the
  current state (0x6EEED0): the chart root S0 has 1.0 and only the under-water sprint state S159 overrides it with
  {3, 1.5}, so a sprint under water spends the breath 1.5 times as fast (`World::kAirSprintDrain`; `GameReal.
  SwimSprintSpendsBreathFaster`). Surfacing (0x756C40) plays sound +0x94 = 962 when the meter was spent and +0x98 =
  963 when it was just not full [U: not played, no sound path from the game layer].
- **Breath meter** [O 0x7572F0]: under water without scuba the swim update shows HUD element 2 and flags element 1
  (the adrenaline meter): the breath meter takes the adrenaline meter's place, full or not; out of the water (or with
  scuba) element 2 is hidden. Ours: `World::breathShown()` -> `ui::HudState::breathBar`; also while the 2 s refill
  runs at the surface [I]. Test `Hud.BreathBarReplacesAdrenalineUnderWater`.
- **Depth classes** [O]: player data +0xAC 154, +0xAE 391, +0xB0 391, +0xB2 948, +0xA8 / +0xAA −44 (the surface
  swim's root offset, measured 44.1). 0x764340 (conditions 0x321..0x323) classes the water depth at her spot (surface −
  floor): > 0 0x321, ≥ 154 0x322 (wading), ≥ 391 0x324 (swimming), ≥ 948 0x325; 0x764550 (0x324..0x326) the same with
  her own depth for 0x325. The surface dive (S115 -> S106 on the crouch button, cond 0x98 = 118) needs water of class
  0x325: at least 948 deep [O chart T887]; ours: `Tuning::diveDepth`, no floor within 948 under the surface, else the
  crouch button does nothing at the surface (`Game.SwimNoSurfaceDiveInShallowWater`). Request 903
  `swim_wading` is requested by no state of the chart [O], so it is not played; wading uses `wading_idle` /
  `wading_walk` (above).
- **Under-water steering** [O-runtime fit]: the stick's forward share swims along the camera's view line, back against
  it, sideways level; jump / drop add straight up / down (`Player::setCamera`, `Player::swim`). tru.exe under Wine
  (l2_cliffclimb sea, a dive then forward / sprint held; openunderworld-work/scratch/swimsprint/under1.txt: velocity
  player +0x2A0 and the camera position): with the camera looking down at her after the dive (root-to-camera pitch p)
  her velocity's pitch is p + 9.2° for p below −14° (−28.8 at −36.8, −21.3 at −30.5, −13.4 at −22.5, −5.5 at −13.9)
  and the lead shrinks to nothing near p = +8.5° (0.9 at −6.4, 5.7 at −0.3, 7.8 at 3.7, 8.2 at 7.5); the speed along
  the path is the swim speed (54.0 a frame sprinting). So she goes on down after a dive (1034 units there), levels
  out within ~3.5 s as the camera trails behind her, and ends rising gently. Ours: lead = 9.2° × sqrt((8.5° − p) /
  22.5°) clamped [I: the form of the fit]. The under-water camera (mode 19) is free in pitch (command 8, no pitch
  force set [O]). tru.exe's own function (0x755A00: chart properties 0x200..0x20F, speed table 512..519 = 10.5 / 20 /
  28.5 / 34 / 42.5 / 45 / 46 / 54 a frame) is not ported [U]. Test `Game.SwimFollowsTheCameraPitchUnderWater`.
- **Evasive dodge under water** [O chart, clips]: in combat mode (the lock held) the submerged idle S185 goes to S167
  on the roll button (cond 0x98 = 51) and to S196 on the jump button (52); there the stick's quadrant as on screen
  (cond 0x195, 0x764270: 450 up / 451 down / 452 / 453 the sides) picks the state:

  | stick | clip (request) | root motion, frames |
  |---|---|---|
  | none (T996 / T1060) or up | `swim_submerged_dodge_up` (878) | 883.4 up, 30 |
  | down | `swim_submerged_dodge_down` (864) | 881.2 down, 20 |
  | a side | `swim_submerged_dodge_side` (876 / 877) | 791.6 to her right (mirrored: left), 30 |
  | a side, cond 0x218 set | `swim_submerged_dodge_forward` (863) / `_backward` (875) | 468.5 ahead / 449.9 back, 26 |

  Cond 0x218 (0x762F70) is a byte of the aim controller (0xFADFB0 + 0x50 + 0x195) [U: its setter]; ours [I]: she is
  side-on to the camera (heading more than 45° off its view line), so the stick side along her heading is "ahead".
  Ours: `Player::startDodge`, `State::SwimDodge` (the clip's root motion slid along walls, then the submerged idle or
  the tread when it ended at the surface); `Actions::aim` held and a crouch or jump press. Chaining out of a dodge
  (the dodge states' own transitions, cond 0xD6) is not done. Tests `GameReal.SwimDodgesMoveAsTheirClips`,
  `GameReal.SeafloorScubaSwimKeepsTheBreath` (in l1_seafloor).
- **Swan dive** [O clips / requests, I window]: `ng_inair_swan_stage1` (request 67, 25 frames) -> `_stage2` (69, 6) ->
  the `_stage3` hold (68); landings `swim_swan_land` (897; chart state S105, a submerged state: root 268.5 on and 437.7
  down over 31 frames, played from its start frame 4: 235 / 346), `ng_swan_land` (66: 591 ahead in 11 frames, a
  roll), `swim_land_swan_wading` (843) in wading water, `death_fall_swan` (104). Ours: crouch pressed while a forward
  ground jump still rises [I: the controls page's "Jump followed immediately by Crouch"; the chart's window is not
  decoded] switches the jump to the swan clips; the flight is the jump's. Into swimmable water at any height she
  enters on `swim_swan_land` and ends under water, unhurt (no landing: no fall damage, as every water entry); onto
  the ground she rolls (`State::Roll`) and the fall counts as any other (`Health::fall`; a fatal one plays
  `death_fall_swan` [I]). Tests `GameReal.SwanDiveIntoWaterAndOntoLand`, `GameReal.CliffSwanDiveIntoTheSea`
  (l2_cliffclimb: off the cliff top 5950 over the sea, unhurt).
- **Surface turns on the spot** [O chart]: from the surface group S115, the stick to a side quadrant (MoveDir 412 /
  413) -> S145 `swim_surface_idle_turn_90` (request 832), behind her (411) -> S144 `swim_surface_idle_turn_180` (833);
  transition flags 2 / 5 mirror one of each pair. Clips: a quarter turn right / a half turn left of the root in 15
  frames. Ours: treading with no speed, the stick ≥ 45° off her heading plays the quarter turn (mirrored to the left),
  ≥ 135° the half turn (`Player::advanceRoot`), then the stroke. Moving turns keep the measured turn rate; the moving
  turn clips (`swim_surface_fast_turn_L / _R`, `swim_surface_moving_180`, requests 830 / 831 / 834) are not played.
  Test `GameReal.SwimSurfaceIdleTurns`.
- **Climb out** [O chart, I geometry]: no button. Chart: the swim root S101 goes to S237 (T882) when a markup query
  found a ledge (cond 0x262 = 610) and the stick is pushed, not neutral (0x191 = 401) and not behind her (0x192 = 411);
  S237 then plays `swim_climbout` (request 891, S242) when cond 0x32F holds (0x764890: the ledge point's height over
  her root, 0x756FF0, under player data +0x138 = 90), else `swim_climbout_A` (890) -> the `_B` hang (889) -> `_C` (888)
  on cond 0xDA, or `swim_climbout_to_wallhug` (887) onto a shelf (clearance result 657). Ours: swimming at the surface
  with the stick within 45° of her heading, a wall within 230 ahead (the clip's hands start 205.4 before the root)
  whose top is 50..400 over the water starts `swim_climbout` (`Player::tryClimbOut`: geometry, not the ledge markup
  [I]); Jump no longer does it. The `_A / _B / _C` variant for the higher edge and the wall-hug exit are not played
  [U: 0x756FF0's reference points]. Test `Game.SwimEnterCrossClimbOut` (rewritten: the stick alone).
- **Weapons in the water** [O data, I rule]: the weapon stats have no under-water flag (S+0x88: 0x80 infinite, 0x40,
  0x20 dual, 0x08 two-handed); every wp_* DRM has Lara's `combat_water_submerged_equip / _unequip` and
  `combat_water_surface_equip / _unequip` clips (wp_handgun and wp_uzi reloads of both too), a `fire_submerged` gun
  clip, and the bullets an under-water record (ammo +0x84: handgun 40 / 40 / 20, rifle 23 / 23 / 18); the chart has
  under-water aim sets for one-handed guns, two-handed guns and Mjolnir (requests 924 / 926 / 925). Ours: every weapon
  draws and fires while she swims (surface or under water; not during the surface dive, the entry or the climb out),
  with those handling clips at their own lengths (`handling()`), bullets with the +0x84 record, spears as before
  (projectile, 550). The weapon in hand stays selected through water entry and exit; it is holstered only while the
  state does not allow it. Before: only the speargun and Mjolnir under water. Sticky grenades under water: unchanged.
  Tests `CombatReal.PoolSpeargunUnderWater` (rewritten: pistols 40 a hit under water, then the speargun),
  `GameReal.SeafloorSpearKillsShark`.
- **Jellyfish** [O]: `jellyfish_a` (31 in l1_seafloor, 12 in l1_norse_hallway_a) is a class-79 uber object: one
  section, health 0, takes no damage (it cannot be killed); states 0 / 6 loop its idle, command 0 plays the shock
  animation (transitions 0 -> 1 / 6 -> 1 on trigger 2), random start delays desynchronise the swarm. Its script (type
  5594) holds three `playersensor` members attached (fn 2) to `uberobjectbase` 2 anchors on HInfo markers 10 / 20 /
  30 (the bell, mid tentacles, tentacle tips), radii 278.1 / 208.6 / 192.8. Its vt 14 keeps a flag while Lara is
  inside any of them and a loop then runs the shock command, takes 100 health (its float member +36) and waits, again
  and again until she leaves. Ours: the script runs as it is once sensors fire (below); 100 a sting, every 1.8-3 s.
  Test `GameReal.JellyfishStingOnTouch` (also: far from it nothing, swimming away ends it, a save goes on the same).
- **Player sensors** [O] (`script::Runtime::tickSensors`): NtPlayerSensorInst (ctor 0x77A120, update 0x77A020, test
  0x7799F0, callback 0x779950 -> 0x798430). Member init {u32 0, u8 onEnter, u8 onExit, u8 oneShot, u8 enabled, float
  radius}; natives (bind 0x7ACDD0..): 0 SetEnabled, 1 IsEnabled, 2 attach to an anchor (an instance or a marker
  anchor), 3 same sensor. An enabled, attached sensor tests |Lara root − anchor| ≤ radius and calls the owner's vt 14
  (sensor, inside) on entering (onEnter) and leaving (onExit); a oneShot sensor disables itself as it fires. tru.exe
  tests each sensor every other frame, ours every tick [I]. Saved with the runtime (`sensors`). Before, sensors never
  fired.
- **Treading "drift"** (self-test agent: ~450 units/s with no input in l1_seatop): it is vertical, the root riding the
  four sea waves (surface = top + Σ heightᵢ (cos(..) − 1)); x / y do not move and the volume has no current. tru.exe
  does the same [O Wine: Lara's z fits the wave surface − 44.1 at rms < 1 while treading, "Surface" above].
- Self-tests: `tests/selftests/swim.json` (l1_seatop: on the tanks the breath stays full), `swim_breath.json`
  (l2_cliffclimb river: drain, sprint drain, refill), `swim_moves.json` (surface turn, dive, dodges, guns under
  water), `swan_dive.json` (the l2_cliffclimb cliff).
- Open [U]: the exe's dodge side-on flag and chaining, the swan-dive window, the climb-out conditions and its other
  clips (`_A / _B / _C`, to wall hug), the moving surface turn clips, the
  under-water steering function, breath sounds 962 / 963, grenades under water, what the flag on HUD element 1 does.

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
- Hold to hold [O]: Lara's root is always at a hold — point 0 − 158.025 z (tru.exe 0x707C60) — or travelling between
  two. The holds lie on a 158-unit grid (l2_cliffclimb columns at z 614, 772, 932, 1058, 1217, 1377, …) and every
  `DICE_<from>_to_<to>_<dir>` clip's root motion is one grid step in Lara's frame (`DICE_B_to_B_Up` (0, 157),
  `DICE_B_to_B_Left` 159 left, `DICE_B_to_Lw_Left` 236 left for the wide stances, 0.8-1.3 s). The stick (as on
  screen: up = up the rock, whichever way the camera looks) picks the next hold with lara.drm's markup-detection
  queries (below): the first hold box the DICE move sweep enters along the stick — 15.80 units a frame sideways /
  19.28 vertically for up to 10 frames (q73 / q80 / q81), within 90° of the stick; then the
  `DICE_<stance>_to_<its stance>_<dir>` clip whose root motion fits that step best. No diagonal fallback [O]: tru.exe
  under Wine on l2_cliffclimb hold 11 stays put with up or right alone and goes to hold 38 (159 right, 160 up) only
  with both held. That clip
  carries her there (`Player::warpStep`: its root motion scaled per axis to the real offset, the residual eased in
  where the clip has none; the heading turns to the new hold's rock with it, so corners turn smoothly). Holding the
  stick chains the next step; a step's last frame is the new stance's idle pose (`DICE_B_to_B_Up` ends with the chest
  within 4 of `DICE_B_Idle`'s), so she then idles. The `DICE_<s>_settle_<dir>` clips are not played after a step: they
  start far off idle (`DICE_R_settle_right`: chest 211 to her left). Pushing toward no hold plays `DICE_<s>_reach_<dir>` (held at its last frame) without moving.
  lara.drm has 181 of the 300 stance pairs × 12 directions and spells the stances inconsistently (`Lw`/`LW`,
  `Rw`/`RW`), so `kLaraClipNames` lists every DICE clip exactly and names are looked up case-insensitively. The
  `<dir>` suffixes are not exact angles (`DICE_B_to_B_DownR` is straight down), hence the root-motion match.
  Before 2026-10-31 Lara slid at a fixed 240 units/s inside the band the holds cover with the transition clips'
  root motion ignored, re-pinned every tick to the rock face under her chest (her heading jumping with each face's
  normal, up to 0.067 rad a tick) and the settle clips looping as a "moving" pose: feet sliding, jitter, pops.
  Ladders likewise moved with their clips' root motion; since 2026-11-06 they climb rung to rung (see "Ladders, pillars,
  perches").
- Markup-detection queries [O]: lara.drm object data +0xDE4 = 106 queries, +0xDE8 = (id, record) pairs (tru.exe
  0x6F1950; dump: openunderworld-work/dice/queries.py). 0x771830 builds a query from its record: launch velocity
  +0x08..+0x10 per 30 Hz frame turned to the direction +0xA0 (0x724F50: 1 facing, 2/3 ±90°, 4 back, 5 the stick,
  14-17 relative to the attached hold), gravity −5.72443 a frame² (−0.001 with flag 0x400), start offset +0x58 / +0x5C
  / +0x60 (down / out / sideways) from bone +0xC8, hits from frame +0x20 to +0xA4 (≤ 0: object +0xBDE = 40); 0x70F300
  sweeps it through the markup boxes, 0x70EC60 then needs the horizontal distance > +0x28 (0x6EC360), the direction
  within +0x24 (0x6FC530) and +0x90; 0x710100 keeps the first accepted. Per target type +0x8C adds an offset. The DICE
  queries: moves q73 (sideways along the stick, 15.80 / frame), q80 / q81 (19.28 up / down), q76 / q77 (left / right
  from the hands, bones 55 / 80), frames 1..10, no gravity, cone 90°, +0x6C 30; jumps q31 (74.12 up), q28 / q29
  (63.21 sideways + 34.77 up), q30 (50.57 out + 34.77 up: back), gravity 5.72443, frames 1..40, sideways beyond 410,
  cone 120° (up 180°), start 316.06 (sideways) / 158.03 (up) down and 63.21 out with the same offsets for target type 1.
  Player state chart [O] (lara.drm objdata +0x12508, decoded with Hurdles' openunderworld-work/hurdles/sc.py; tru.exe
  under Wine reading player+0xB00 / +0xC90, the query ids of state props 541 / 546, and the current state at
  *(player+0xF10)+0x510): on a hold (S571) the stick's quadrant (cond 0x195 = 450..453, 0x764270) picks S575..S578,
  whose props pair a move query with a jump query — up q80 / q31, down q81 / q30, left q82 / q28, right q83 / q29
  (live: 80/31 idle, 82/28 holding A, 83/29 D, 81/30 S); action 423 (0x75EB50) runs both every frame into player+0x980
  / +0xB10. The jump button (cond 0x98 = 52) from the hold or the lean states S583..S587 goes to S601..S604, which run
  the jump query of the stick's quadrant (neutral or down: q30), then the jump (S605). The jump goes even with no
  target: tru.exe jumping with the stick neutral at hold 1 ran q30 and leapt 1517 units/s out (= 50.57 a frame), g
  5100. With a target (16 → 1 pushing right: 1064 away, 152 up) the launch clip plays first (~0.15 s before she
  leaves), she flies ~0.6 s and is caught 167 out from the rock and 163 below the hold, then carried onto it over
  0.8 s. Cond 0x262 tests the result target's markup flags (0x782810: 1 = any, 618 = 0x8000 hold, 625 = 0x10000).
  Ours [I]: the sweep is 2D on her rock (right, up) from her hold to each hold's anchor (start and target offsets taken
  to cancel), a hold's box = half its points' extent in its own plane + the record's +0x6C (30 moves, 25 jumps; its
  reader not traced); moves take the first box touched from frame 0 (tru.exe steps down to the adjacent hold, which
  this 2D model overlaps at frame 0; its 3D sweep starts at the chest), jumps from frame 1; a jump up only to holds
  above (its arc falls back past the holds below; 0x6FC6B0's direction test not traced). The lean (antic) plays when
  no move but a jump target lies straight that way; the chart enters its lean states from the move states on a move
  query result whose markup lacks flag 0x10000 and whose direction class (cond 0x19E, 0x786490) matches — not
  reproduced exactly.
- DICE jumps to far holds: the lean `DICE_<s>_antic_<dir>` loops (lara.drm request flag b16 = 2) until jump is
  pressed; then `DICE_<s>_jump_<dir>` carries her off the rock with its root motion (70-224 out and sideways in
  0.17-0.23 s), a ballistic flight (g 5.72443 / frame²) reaches the catch point at the query's hit time — 168 out and
  161 below the hold, `DICE_attach_M`'s root motion (tru.exe: 167 / 163) — and the stance's `air_attach` clip carries
  her onto the hold (0.9 s; tru.exe 0.8 s), then its idle. No settle clip is played (none seen in tru.exe's catch).
  l2_cliffclimb: hold 18 → 10 up (475, hit at frame 7.2), 10 → 1 right (905, frame 12.8). Jump with the stick neutral
  or down: q30 to a hold on rock facing back at her (3D arc, out 50.57 + up 34.77 a frame, frames 1..40), else the
  free leap with q30's launch. Jump left / right with nothing that way: the free leap with q28 / q29's launch (63.21
  sideways, 34.77 up a frame), as the chart does. There is no DICE jump down (down + jump = q30).
- Hand stance [I, from the hold shapes]: the hold she is at sets Lara's `DICE_*` clip set. Each hold is a
  triangle; points 1 and 2 relative to point 0 (right, up along the rock, means over all 336 holds in the 8 units,
  2026-10-29 survey) are 16: (−67, 144) (69, 174) both hands high → `B`; 17: (−75, 174) (41, 16) left high → `L`;
  18: (−134, 143) (136, 16) left high, wide → `LW`; 19: (−72, 14) (70, 143) right high → `R`; 20: (−96, 20)
  (125, 169) right high, wide → `RW`.
- Attach: pushing into such a wall from the ground (chest probe at +300, reach = capsule radius + 90,
  facing it within ~53°) or falling onto it (after ledge/pole grabs, which win). The hold is the one nearest (within
  300 [I]) to where the stance's attach clip ends — ground: its root motion from her feet (`DICE_B_ground_attach`
  268 forward, 185 up; `DICE_L_ground_attach` 134, 317), air: where she is (`DICE_<L/LW/R/Rw>_air_attach_mid` have
  none) — and that clip carries her onto it as a hold-to-hold step does, then its idle.
- Top out / jumps to ledges: lara.drm has no DICE top-out clip (anim-list survey: no `DICE_*climb*` / `*top*`) [O].
  q28 / q29 (sideways) meet ledges (markup 1) by their class record 0x2AA4C and q31 (up) by 0x2AE2C's entry 1 (flags
  0x1B: not holds; no 0x04, so the hold she is on does not gate it) [O-data]. Ours (`Player::ledgeOnArc`, the jump
  query hit test below: q31 from the hold point (its first point, 158 over her root [I]) 63.21 out and 158.03 down,
  i.e. at root height, its ledge target type 3 arc from 110.62 over the hold point, only coming down; q28 / q29 316.06
  down, type 3 at the hold point) takes the first ledge met, up ones not more than one hold step below her [I]. With
  no hold that way for the move query but such a ledge (or a far hold) for the jump query, the stick leans
  (`DICE_<s>_antic_<dir>`) and jump leaps onto it. Up [O: tru.exe under Wine, l2_cliffclimb, Lara air-attached at
  hold 9, up to hold 5, then up + jump; openunderworld-work/runjump2 dice.sh]: no ballistic arc — 0.39 s after the
  press (states 0x…753A8 / 0x…75600 alternating while up is held, then 0x…75A60) her root rushes straight at a
  constant 15.1 a tick for **11 ticks** (184 units: 153 out from the rock, 75 up) onto the hang of ledge 45 (161 out of
  its line, 494 under it), then the hang (S605 → S289 → S535, the standing jump's rush). Ours: `DICE_<s>_jump_up`
  and the 11/60 s straight leap onto the hang pose, `ledgeb_attach_U` [I: the 0.39 s wait]. Sideways (unmeasured):
  `DICE_<s>_jump_right` / `_left` (take-off 34.77 up a frame) and the re-aimed flight, `ledgeb_attach_S`, the
  hang. The jump goes on the button only (chart S601..S605); with no target at all up + jump leaps straight up with
  q31's launch. l2_cliffclimb's wall ends that way: hold 5 (z 2480) → ledge 45 (z 2891). Foot: no hold below but a
  floor where `DICE_<L/LW/R>_step_off_ground` ends (226 back, 317 down; B and RW have none) = that clip onto it, Idle.
  Free leaps (no jump target) use the queries' launch velocities (above); drop = its `drop` clip and fall. Cooldown
  0.4 s before re-attaching.
- Bad attach [I: trigger]: caught from the air falling faster than a firm ledge grab (grabMinVz, −13 × 5.72443 a
  30 Hz frame) and up to twice that — the ledge saving-grab rule; faster still she falls past — the stance's
  `DICE_<B/LW/R/Rw>_air_attach_bad_start` carries her onto the hold (the hands end ~115 below the idle pose's), its
  `_loop` holds her there for the saving-grab time (difficulty "TIME FOR SAVING GRABS"); jump plays `_end` (back to the
  idle pose: hands at the idle's 301.5) and the idle, else she drops (`drop`, fall). L has `DICE_L_air_attach_bad`
  (ending low) and `DICE_L_air_attach_bad_recovery`. Request fades from lara.drm (B 256 / 255 / 254, LW 226 / 224 /
  225, R 231 / 229 / 230, Rw 280 / 281 / 282, L 228 / 227) [O]. Not used [U]: `DICE_shared_air_attach_bad_D / _FD /
  _F` (request 299, 3.17 s with root motion from 225 out / 149 down: a bad catch out of a DICE jump; chart state S612
  has no incoming transition, so what enters it is unread), `DICE_<s>_bad_attach_recover_*_hand`, the wrestle clips.
- Tests `GameReal.ClimbAttachClimbUpTopOut` (a synthetic hold column with lara.drm's clips: attach at hold 0 exactly,
  one `DICE_B_to_B_Up` = 158 up, `DICE_B_jump_up` onto the top ledge, hang, climb up), `GameReal.ClimbJumpOff`,
  `GameReal.ClimbNeedsHolds`, `GameReal.ClimbStaysWithinHolds` (reach, no movement), `GameReal.ClimbStanceFollowsHolds`
  (L → R → LW → RW → B with `DICE_L_to_R_UpL`, `DICE_R_to_LW_UpR`, `DICE_Lw_to_Rw_UpL`, `DICE_Rw_to_B_UpR`, then
  `DICE_B_Idle`), `Game.DiceTransitionNames`, `GameReal.ClimbBadAttach` (a 1200 fall: bad start / loop, jump pulls
  up to `DICE_B_Idle`, or she lets go; a short fall catches firmly), `GameReal.CliffclimbIndependentOfRenderRate` (the
  whole route at 30 / 60 / 144 Hz: same clips, same end on ledge 45), `GameReal.CliffclimbStances` (the l2_cliffclimb wall's whole route,
  holds 21 11 38 12 7 19 17 13 18 14 16 15 6 4 40 8 3 9 5, then `DICE_B_jump_up` onto ledge 45: the root exactly at a
  hold whenever a step ends, chest (bone 14) step change < 16 units a tick, heading ≤ 0.02 rad a tick; on a fixed
  zig-zag stick script the old code changed the chest's step by > 3 units on 212 of 749 ticks, the root's on 32,
  and turned up to 0.067 rad a tick; now 27, 2 and 0.002),
  `GameReal.ClimbOnlyAtHolds` (12 reachable walls ≥ 1000 from any hold in each of l3_main_bedroom, l4_pool,
  l4_ballcourt, l2_hubhall, l2_cliffclimb, l1_baldrsstairs_a: 0 attach, the old rule attached 10-12 of 12;
  l2_cliffclimb hold 910 ground attach + climb up; l1_baldrsstairs_a air attach at hold 44),
  `GameReal.CliffclimbDiceJumps` (antic, launch root motion, catch 161 below, air attach, idle exactly at the hold: 18 → 10 up, 10 → 1 right). Hand test:
  `ou-game --level 'pc-w\l2_cliffclimb.drm' --spawn '-23835,-3600,300,90'`, walk into the rock, then stick up / left /
  right (F1 overlay shows the clip); the route goes up and right around the corner column, then left along the top
  to the ledge. On the corner column's top B hold (18, z 1533) push up: `DICE_B_antic_Up`, Space jumps to hold 10; there
  push right and Space: the far column (hold 1). `ou-game --level 'pc-w\l3_main_bedroom.drm'` and run into any wall
  (no climb).

## Grapple (src/game/Grapple.cpp)

- Feature targets [O data]: an object's physics record lists model features; mask bit 4 marks a grapple point
  (`physics::ObjectPhysics::features`, `kFeatureGrapple`). Uber objects and class-34 rigid bodies with one are offered
  as `GrappleKind::Mounted` targets at the feature's bone while its model is shown (`World::uberShowsModel`):
  `ma_uo_bedroom_door`'s ring (l3_main_bedroom 4526) among them.
- Targets [O]: intros named `*_ei_grapple_*` (`ca_ei_grapple_post`, `sa_ei_grapple_ceiling(_invisible)`,
  `ma_ei_grapple_ceiling`, `ca_ei_grapple_wall_a`, `tp_ei_grapple_wall`, `ar_ei_grapple_wall`, …; not
  `fx_grapple_bling_*`), collected per unit into `UnitData::grapples`. Kind: Mounted (a pull target) when the intro's
  script type derives from native `uberobjectbase` and its uber state machine watches the pulled joint (a trigger 14 /
  15 transition: levers, shiva arms, the grapple block, the snake heads) [O data]; else wall/post/ceiling by name
  (ceiling for an unnamed uber anchor). [O: script type 3565's subclasses are the uber ones; fixed anchors derive from
  type 3941.] l4_pool's ceiling anchor is uid 5511 at (4362, -70, 2607). `sa_ei_grapple_wall_rigid_body` (l2_maproom
  2676) is a plain anchor (script type 4734); its attached-instance field is filled at run time [U].
- Grapple handles [O]: `tp_ei_grapple_wall`, `ca_ei_grapple_wall_a`, `ar_ei_grapple_wall`, `sa_ei_grapple_wall`,
  `sa_ei_grapple_mounted` and `sa_ei_grapple_wall_naga_statue` are class-79 uber objects of two sections: the plate
  (section 1, model 0, fixed to the world) and the handle (section 0, model 1) on a free hinge, limit +-50 degrees
  (the naga statue's 0..120). tru.exe simulates the handle in Havok: under Wine every l2 handle whose hinge is
  horizontal hangs at its limit (l2_towerconnector 2043 / 2042 / 2005: its instance 49-52 degrees round the hinge,
  origin moved (-79.6, 0, 27.5) / (34.3, 0, 81.6) / (0, -43.2, 77.4); vertical hinges — l2_norsehub 2383, 2393, 2382
  — stay put). Ours: the joint simulates under gravity from spawn and ou-game draws the handle on it
  (`ProgressionReal.GrappleHandlesSwivel` checks the three offsets); while Lara is on the rope, its pull (her 55 kg
  along the rope's first segment [I]) turns the handle toward her within its limits. The `sa_ei_grapple_wall` scripts
  (type 8732) only send uber commands 0 / 1 when their trigger volumes call vt 56 / 57: command 1 -> state 1 (action
  0x1A: no grapple target), command 0 -> state 2 (0x19: a target again); l2_towerconnector volume 2038 switches 2042
  off while Lara is inside it [O data]. Their main loop (onAttach / onDettach to a type-3588 helper) never runs: its
  instanceref +36 is unset in every placement. None is a pull gate. [U] where the hook attaches (the handle's instance
  carries the bit 0x6ECCA0 reads; ours anchors at the intro origin, on the plate).
- Tuning [O g_grapple data, `GrappleTuning`]: hook object 77 = `pr_grapplinghook` (+0xF8), hook launch speed 1580.28
  (+0xFC, the projectile's speed in 0x6C85C0 -> 0x6A1980), throw range 4424.78 = 14 m (+0x100, 0x698AC0), rope
  553.1..4424.78 (+0x108 / +0x104, clamped along anchor -> bends -> hand by 0x6A4A40), 8 bends (+0x10C, 0x6A3430).
  A target's intro data 0x404 block (tag 0x404, byte +4 flag, +8 throw range, +12 longest rope; tru.exe 0x698A70 reads
  instance +0x104, else its parent's) overrides both when flagged [O]: `GrappleTarget::range / rope`, the rope kept in
  `PlayerState::ropeMax` (saved). 24 of the 37 grapple intros set it, e.g. l3_chapel 4744 4424.78, l3_main_hall 4089
  3792.67, l4_temple 4951 5689, l2_norsehall 2457 rope 6005. A unit's settings record (+0x2B4: +0x18 flag, +0x1C / +0x20,
  0x5D3FF0 -> 0x698BC0) could override them too; no unit sets it [O data]. The hook does
  not fly in ours (it attaches at once; at 1580 units per 30 Hz frame [I unit] it would take 1-3 frames) [I].
- Selection [O lara.drm player data +0x464, criteria 0x712090]: flags 0xEC000000 (line of sight from her eye), six
  target sets in order, the first with a candidate wins: (0) horizontal angle from her facing <= 25, distance <= 2880,
  camera angle <= 90; (1) in range, <= 15, <= 5120, camera <= 90; (2) <= 2880, camera <= 90; (3) <= 2880, horizontal <=
  90, no more than 60 below level; (4) in range, camera <= 90, at least 5 above level; (5) in range, horizontal <= 90,
  at least 5 above level (`World::grappleTarget`). Distances from her eye (0x6ECE40: position + data +0x48C) to the
  target point (0x6FF040: origin + its message-0xDB offset; ours the intro origin) [O]; "in range" (criterion 15) is
  measured by tru.exe from the instance message 0x175 returns (grapple controller +0x34: the g_grapple instance, stowed
  at Lara's HInfo marker 9 on her pelvis, g_grapple data +0xB4 {in hand 30, stowed 9}) [O; Wine idle: 30 ahead, 37
  right, 295 up of her root; ours marker 9 at 22 / 37 / 303], ours from that marker (`GrappleTuning::stowMarker`). Line of
  sight (0x713200 -> 0x713090, ray flags 0xC5AC1983 -> 0x4935F0 -> 0x493330): terrain faces with mask bit 1 block
  except in groups whose flags have 0x8 / 0x20000 / 0x40000 / 0x80000 (filter 0x48EF50), instances only with object
  flag 8, never the target or its parent [O]; ours the same (`collision::kSightMask`: the loader gives the other mask-1
  terrain faces bit 0x04, `physics::solidShape` objects with oflags 8 bit 0x40), the target's own colliders excepted.
  Within a set nearest the camera's
  view [I]. l3_chapel 4744 hangs 116 under the barrel vault, so it is offered only from high up: the column top by the
  stone angel (ledge 1008, checkpoint 4738), within ~330 of its west edge — the timed run to the chapel doors
  (`GameReal.ChapelRingFromTheColumnTop`). A crosshair marks it in ou-game and, with GAME OPTIONS >
  HELPER BUTTONS shown, the grapple's helper button above it: locals.bin 363, the grapple placeholder U+1800 alone
  (364 / 365 hold the jump / interact ones), expanded as the popups' (the bound key, "Q" by default, or the pad's X
  icon) [O string; I placement: not captured].
- Enemies [O]: not grapple targets. No enemy or other object names a grapple clip or reaction (only lara, the weapons
  and the kraken scripts do), so the original has no grapple pull / disarm; its grapple combat is shooting on the rope:
  pistols / uzi one-handed in the right hand, `combat_pistol_grapple_rappel(_free)_1haim{U,,D}{F,FR,R,RB,B}` plus
  `_equip / _unequip / _reload` (wp_handgun / wp_uzi; `_free` hanging free). Column yaws 0 / -45 / -90 / -135 / -180
  and rows +-45 by the names [I]; aims to her left clamp (`_switchhands` unused) [U].
- Q / pad X (`Actions::grapple`, virtual button 1) throws (`grapple_rappel_free_jumpon`) and attaches; Q again or
  Ctrl/C releases. Space: see "Rope inputs" below. tru.exe's keyboard
  defaults put Recenter Camera (button 8) on Q too [O: 0x487710 binds button i to key 0x80 + DirectInput scan code,
  buttons 1 and 8 both 0x90 = 0x80 + DIK_Q 0x10; the registry Input list holds "1"="144" and "8"="144"]; a press that
  throws or releases the grapple does not also recenter, a press with no target offered recenters (`World::tick`)
  [I]. ou-game logs every press: `grapple thrown at UID: attached`, `grapple released`, or `grapple pressed: no
  target offered, recentered the camera (shared key)` with her position.
- Rope vs world [O rules, tru.exe 0x6C8840 -> 0x6C5490]: the rope wraps. Its bends ("Bend Body", 0x699A50) are a chain
  from the anchor; she swings on the last one (`ropePivot`), the free part = total - anchor -> bends length. Each tick,
  from the hand side, a bend whose neighbours see each other slides toward their line by 15 x its clear ticks
  (15..200) and goes once nearer than that (0x6B3570); then a pivot -> hand segment newly blocked between last tick's
  hand and this one gets a bend where it first touched, 7.90125 off the surface, none within 15.8 of another
  (0x6A50B0). Segments are tested shortened by min(9.48, 0.45 x length) at each end (0x6B2CC0). Ours: one ray per
  segment instead of the 4.74-radius sphere casts, the touch point by bisection [I]. Saved in `player.controller`.
- Rope inputs [O state chart + tru.exe 0x746F60 / 0x745FA0; input actions 44 / 48 = Interact, 47 / 52 = Jump, 46 =
  Grapple (generalbank DTP 0x2F)]. On the rope the stick is read in its own frame (the pad's angle and deflection,
  input +0x10 / +0x18; dead below 0.35, player data +0x9C8 / +0x920), not the camera's:
  - On the ground with the rope in hand she is in the ordinary ground set (chart S705, `grapple_idle` S722): she walks
    and the rope's length is her distance (0x6A47E0), up to the target's longest. Walking off an edge under the anchor
    she hangs on it (T1409). **Jump keeps the rope**: at an edge `grapple_rappel_free_jumpon` (T1407), else the
    ordinary jump (S752 -> S753 `combat_jumpf_inair_grapple_attached`), which becomes the hang on its way down with no
    ground under her (T1387 / T291) — the rope she hangs on is as long as her distance then. Ours: one launch (the
    stick jump's 40.00 along + 49.97 up per frame, or the standing jump's), walk 300 units/s, "no ground" = none
    within 316 under her feet [I].
  - Hanging free (S381): **Interact held + forward / back climbs / slides** (S391 -> S395 `grapple_rappel_free_ascend`
    / S392 -> S396 `_descend`; stick within 1 - sin 32.5 deg of the axis; 23.70 up / 41.09 down units per frame x the
    stick, +0x9E4 / +0x9E8; not while swung out past 40 deg, +0x9EC; rope 553.1..longest). Without Interact the stick
    forward / back drives the swing (below). Not swinging, **left / right turns her** on the rope (S413..S416
    `grapple_rappel_free_rotate_left/right`, 3.5 deg per frame eased in over 50 frames, +0x9F4 / +0x9FC). **Jump leaps
    off** (S406, `grapple_rappel_free_jumpoff`): along her facing max(63.21, 0.45 x her horizontal speed), up max(47.41,
    0.85 x her vertical speed) per frame (0x73C640 case 734, +0xA48..+0xA54); Crouch lets go; Grapple again lets go.
  - Feet on a wall (S311): **Interact held + forward / back** climbs / descends (34.77 / 44.25 per frame x the stick,
    +0x924 / +0x928, where the stick is more up / down than sideways); without Interact left / right runs along the
    wall. **Jump leaps off the wall** (T379 -> S320): the stick left / right -> `grapple_rappel_jumpoff_left/right`
    (she turns that way: 45.83 along the wall + 44.25 up per frame, +0x994 / +0x998), else `grapple_rappel_jumpoff_back`
    (53.73 out + 44.25 up, +0x9A4 / +0x9A8). [I] ours turns her at once and reads left / right as the stick's 90
    degree sectors. Jump with the stick back during a wall run (T424 -> S324 `grapple_rappel_jumpoff_back_dynamic_*`,
    +0x9B4.. 53.73 sideways + 53.73 up) is not implemented.
  - The jump-offs let the rope go [I: their chart states S321 / S406 are outside the rope states; a kept rope would
    re-enter the hang by T291].
- Swing law [O tru.exe 0x746F60, player data +0x8D8..+0xA3C; all per 30 Hz frame]. The original swings a rigid body
  ("[GrappleRope] User Body": sphere radius 30, mass +0x8D8 = 55, damping +0x8DC = 0.022) on the rope; ours is a point
  pendulum (hand = root + 400 [I]) under the same forces, gravity 5.72443 [I: Lara's own].
  - The swing is driven while the stick is along her facing — |cos| of its angle over 1 - sin 12.5 deg = 0.784,
    forward or back — and for 10 frames after it left (+0x9D8); from rest only forward starts it; the stick held back
    for 150 frames (+0x9DC) stops it. Interact held never drives.
  - Driven, a force along her facing pushes her **the way she is moving** (forward while she swings forward or hangs
    still, backward while she swings back): mass x stick x the table value by the rope's free length (0 -> 948.2 ->
    2844.5 -> longest, +0x8E4 / +0x8E8): forward 3.003..3.161 / 3.003..3.161 / 4.267..4.425, backward 3.003 / 3.319..3.477
    / 4.425..4.741 units per frame^2 (+0xA10..+0xA3C; gravity is 5.72). So holding Forward builds the swing every pass;
    there is no phase to hit — the phase is taken from her motion.
  - Past 45 deg from straight down (+0xA04), going outward: no drive, and a brake force of 2.5 x her velocity (+0xA08).
    Motion across her facing is braked (3.5, +0xA0C). Not driven and going outward: a brake of 1.0 x velocity (+0x9E0).
    Speed cap 379.27 per frame (+0xA00). Her facing stays while she swings.
  - Ours matches these numbers (`Grapple.cpp` constants); [I]: the brakes and the body damping as per-frame rates over
    the body's mass, the start of a swing from rest going forward, the point pendulum.
- Swing, original vs ours (l3_main_hall ring 4089, the prologue's crossing; ours `GameReal.MainHallSwingToFarBalcony`,
  self-test `prologue_hall_swing`):

  | | original [O data / code; not yet measured in the running game] | ours (measured) |
  |---|---|---|
  | rope she hangs on after the jump off the balcony edge (2614 from the edge) | her distance when the jump comes down with no ground under her (T1387) | 2170 ("no ground" = none within 316 [I]) |
  | drive, rope 2170 | 3.003 + 0.158 x (2170 - 948) / 1896 = 3.10 units/frame^2 forward, 3.42 backward, x stick | the same |
  | swing period, rope 2170 | [U] | 3.67 s (220 ticks) |
  | far end of the first swing, Forward held | [U] | x -1686, z 1993 (107 ticks after the jump; 53 deg) |
  | swing held with Forward (steady) | no drive past 45 deg going out, brake 2.5 x v / 55 per frame | ends at 56 deg (x -1755 / +1871, z 2090 / 2140) |
  | no stick | brake 1.0 x v / 55 per frame going out, body damping 0.022 | dies down within ~3 s (self-test `grapple_swing`: peak speed 1214 -> 80 units/s) |
  | leap off (Jump) | 63.21 forward, 47.41 up per frame (or 0.45 / 0.85 of her speed) | 1896.3 / 1422.3 units/s; lands at x -3014 on the far balcony (z 1727) |
  | rope climb / slide (Interact + forward / back) | 23.70 / 41.09 per frame | 711.1 / 1232.6 units/s (self-test: 711.1 in 1 s) |
  | rappel up / down | 34.77 / 44.25 per frame | 1043.0 / 1327.4 units/s |

  A Wine trace of player +0x2A0 on this ring (period, growth per pass, the rope length the hang starts with, the
  roped walk's speed) was attempted twice and not obtained: teleported to the balcony (unit offset from the ring's
  instance position: tru = ours + (1583.83, 1911.31, -1939.58)) she falls through the world after the first camera
  calibration run. Scripts and notes: `openunderworld-work/gameplaygoal/grapin/` (`wine2.sh`, `an.py`, `NOTES.md`).
  Three more sessions (2026-11-10, `openunderworld-work/gameplaygoal/reference/failed/`) did not get her onto a rope
  either: on this balcony she is 277 lower a second after being set down and then gone (the game restarts the
  prologue, which is why the earlier sessions' later teleports fell through the world), and in l3_chapel the unit
  opens with a scripted sequence that owns her. docs/REFERENCE_COMPARISON.md "Grapple" has ours' measured swing on
  both rings and, under "Unmeasured", the session to run next.
  Open until then [U]: the time base of the brakes and of the body damping (ours: per frame), gravity on the rope
  body, the "no ground" distance, the roped walk speed, whether the jump-offs let the rope go.
- Landing from the rope [I]: swinging down onto a floor (> 300 u/s down) off a taut rope, or coming down just over a
  floor out at the end of the arc (more than 0.3 x the free rope from under the pivot), she lets go standing; landing
  her own jump with the rope slack keeps it.
- Overhangs [O PlayerSC_Grapple::OverhangDetected 0x73AB70, player data +0x980..+0xA44]: climbing down a wall, nothing
  within 553.1 into it at 110.6 below her feet -> `grapple_rappel_overhang_descend_begin`, `_descend` into a free swing
  (S379 -> S378); climbing the rope with a wall within 63.2 at 316.1 over her root (60..120 degrees from up) whose lip
  is more than 158 under the pivot -> `grapple_rappel_free_overhang_ascend` onto that wall (S421 -> S314).
- Pull (Mounted targets): `grapple_pull_<up/mid/down>_light_begin` then the loop by anchor height (±400) [I];
  holding E: push blocks follow her (240 u/s [I]); other uber targets run their pull handler after a 1 s heave
  (`script::Runtime::grapplePulled`: the first overridden void slot 55..60 that does more than return — levers 59/60,
  shiva arms 55/56, snake head 57 [O]; which slot tru.exe calls on a pull is [I]),
  then she lets go standing; `World::lastPulled()` reports the target.
- Rappel (`State::GrappleRappel`): a swing into a wall (|normal.z| <= 0.35) puts her feet on it,
  `grapple_rappel_wall_land(_left/_right)` by sideways speed, facing the wall [I]. Inputs and rates as above [O]:
  Interact + forward shortens the rope (`grapple_rappel_up(_left/_right_45)`), + back lengthens it
  (`grapple_rappel_down…`); gravity keeps the pendulum in the wall plane; she follows wall steps up to 150 deep, else
  swings free again; set onto a spot the taut rope does not reach she is moved along the wall toward the pivot.
  Jump leaps off, drop lets go; descending onto a floor ends it standing. Body angle: the rappel clips carry it.
- Wall run: on the wall, a sideways stick (no Interact) runs along it (`grapple_rappel_left/right`,
  `grapple_rappel_walk_*` below 0.6 deflection [I]). Drive [O tru.exe 0x745FA0]: a force along the wall of mass x
  stick x 1.896..2.528 / 3.161..3.951 / 5.531..6.321 units per frame^2 by rope length (+0x95C..+0x970, the lengths as
  the swing's), not past 55 deg from under the pivot (+0x944), where going outward she is braked (2.5, +0x948); speed
  cap 97.98 per frame (+0x940). So she runs the rope's arc down past the point below the anchor and up the other
  side. Not decoded: the reversal rule (+0x94C / +0x950 / +0x954, flags 0x4000 / 0x8000 / 0x40000), the idle brake
  (+0x92C / +0x930) and the body damping it switches (0.02 / 0.05) [U]. From the ground: roped to a Wall anchor, on
  a floor with the anchor's wall (the surface closest to the anchor) within 400 of her capsule, a stick along that
  wall toward the point below the anchor plays `ng_ground_to_wallrun_left/right` and starts the run (700 u/s, 350
  up); while that clip plays or she runs downhill along a floor the rope reels in its slack. Use sites [O]: the Wall
  anchors (name `*wall*`) — l1_kraken_room 326, l2_maproom 2676, l2_norsehub 2382, l2_puzzleroom 2974,
  l4_traprooms 5752, l5_valaskjalf 6750/6751, l7_helheim01 8897, l7_yggdrasil_end 8165, and the uber handles of
  l2_towerconnector 2005/2042/2043, l2_norsehub 2383/2394/2395, l2_norsehall 2457, l2_maproom 2595; no markup marks wall-run
  walls (COLLISION.md types). l5_valaskjalf 6750 has ledges 1600 to either side of the anchor over a pit — the
  wall run carries her from one to the other.
- ou-game draws `pr_grapplinghook`'s model at the anchor along the rope's last segment (prongs = model +Y [I]) and the
  rope as a line hand -> bends -> anchor (the original's rope strip is not decoded [U]). Tests `Game.GrappleSwingAndRelease`,
  `Game.GrappleRopeBendsRoundGeometry`, `Game.GrappleIgnoresEnemies`, `GameReal.GrappleTuningFromData`,
  `GameReal.GrappleShootsOnTheRope`,
  `Game.GrappleWallRunFromGround`, `GameReal.PoolGrappleTargetAndSwing`, `GameReal.MountedGrapplePullsBlockWithoutSwinging`,
  `GameReal.GrapplePullsUberTargets` (replay `tests/replays/grapple_pull.txt` at l4_trailstart lever 6695,
  l2_puzzleroom shiva 2982), `ProgressionReal.GrappleHandlesSwivel`, `GameReal.KrakenRoomRappel` (replay
  `tests/replays/kraken_rappel.txt`: swing onto 326's wall, Interact + back / forward down and up, Jump leaps back off
  the wall), `Game.GrappleSwingDrive`, `Game.GrappleJumpIntoGapKeepsRope`, `Game.GrappleRopeClimbNeedsInteract`,
  `Game.GrappleRappelInputsAndWallJump`, `GameReal.MainHallSwingToFarBalcony`; self-tests `prologue_hall_swing`,
  `grapple_swing`, `rappel`, `wall_run`,
  `GameReal.ValaskjalfWallRun` (ledge → wall run 370 down through the arc → far ledge, standing). Replay
  `tests/replays/valaskjalf_wallrun.txt` plays the wall run in ou-game.
- Viewed (2026-10-29, GL + Vulkan identical): l1_kraken_room rappel at tick 300 from the side
  (`--camera -2700,-13900,1450,-2980,-14862,1450`): feet on the wall, body out along the rope, roughly level; the
  l5_valaskjalf wall run at tick 205 from the follow camera (`grapple_rappel_right`: crouched run on the wall facing
  it, rope from the hand to the anchor). That shot first showed a bind pose: the run/walk clips were not loaded; the
  replay tests now assert every clip they play is loaded.
- Not yet: rope-vs-world collision, hook model, throwing while hanging from ledges, grapple combat
  (`grapple_rappel_combat_*`), overhangs, exe rope/pull numbers. Rope length/physics are [I].

## Ladders, pillars, perches, wall hug (src/game/Traverse.cpp)

- Markup [O]: ladders (9) have points `{top, bottom, the bottom's other corner}` (see "Ladder frame" below), pillars (15) `{top, bottom}`; perches
  (13) are a short segment; 13 of the 29 pillars carry a perch on their top (within 9 units; survey of all units,
  2026-11-06). `climbNormal` holds the outward direction. Clips are those of lara.drm's anim requests for them
  (ladder 1158–1198, pillar 616–646, perch 647–676), with their fades and start frames; the `lad_*` set (requests
  1199–1212, the same animations) is not used.
- Ladder frame [O data, corrected in 0.42]: the third point is not an outward hint: `points[2] − points[1]` runs
  across the ladder (158..243) and the wall faces `width × up`. Rays in the units: l1_ship_sinking_hall 234 / 236 lie
  in the valve wall y = 29 and 235 in y = −1247, open toward −Y; l7_arctic_sea 2044 / 2045 (width −X) and 2046 / 2047
  (width +X) face each other across their chamber. Ours had the width as outward, so every real ladder stood turned
  90° in its wall's plane (which is also why the Wine teleport below never attached). `ladderOut`, `rungPos`.
- Ladder rungs [O]: tru.exe 0x734B70 / 0x72B920 / 0x74DCD0 (`LadderRungAlignAnimTarget`) with lara.drm player data:
  rung r's root lies **575.221** (+0x784) + r × **110.619** (+0x770) below the top point, half the ladder's width along it and
  **110.619** (−(+0x778)) out from the wall; rungs run 0 .. trunc(trunc(length) / 110.619) + 1 − **6** (byte +0x774),
  the lowest putting her feet at the bottom point (l1_ship_sinking_hall 234: 933.6 vs 940). Every climb clip
  (`ladder_climb_up` 98.4 / `_down` −89.9 of root motion) carries her from one rung to the next (scaled to 110.619, as
  the align target corrects it); attaches end on a rung (from the floor the lowest, `ladder_attach_ground_bottom`; from
  the air the nearest, `ladder_attach_M`; from the floor above, facing out over the top, `ladder_attach_ground_top`
  turns her round onto rung 0). Off rung 0 upward `ladder_detach_ground_top` (216.4 forward, 575.4 up), off the lowest
  downward `ladder_detach_ground_bottom` (95 back, half turn), Space = `ladder_jump_back` (half turn; q22 below),
  Ctrl = `ladder_detach_drop`.
- Ladder jump up [O-data, player chart ladder group T1871 / T1872 / T1817]: jump with the stick up, only on the top
  three rungs (0x72B920's rung ≤ chart variable 195 = 2; [U] the LookaheadResult 642 the chart also excludes) and
  with nothing within 1027 over her (cond 0x20, variable 396 = 325 × 3.1605). Top clear (cond 0x28A, player +0xEDC
  == 2: sweeps from her root up to 110.62 over the ladder top, then 297 (+0x788) on over it): `ladder_jump_up_at_top`
  (910 up, 215 forward of root motion; ours ends on the floor there, or falls) — else `ladder_jump_up` with query q23
  (80.15 up a 30 Hz frame) onto the ledge its arc meets (as the DICE jumps above; `ledgeb_attach_U`), or straight up.
  Climbing off the top needs the clear top too; with it blocked the stick up on rung 0 leans
  (`ladder_idle_to_jump_up_prepare`, the `ladder_jump_up_prepare` loop, released `_prepare_to_idle`) [I: which request
  of the ladder group plays them is not decoded]. Real ladders are few (7 in 97 units); none in the main game has a
  ledge above a blocked top [O-data survey].
- Ladder jumps to the side and back [O-data, player chart ladder group S1049]: jump with the stick's quadrant left /
  right (cond 0x195 = 452 / 453) takes T1869 / T1870 to S1113 / S1115, neutral or down T1868 to S1111, all under S1109
  whose query is **q22** (prop 541; prop 543 = 412 / 413 / 411 the way): **47.41** that way + **60.05** up a 30 Hz frame,
  gravity 5.72443, frames 1..40. Take-off clips `ladder_jump_left` / `ladder_jump_right` (0.17 s, 60 / 70 aside and 47 /
  85 up of root motion, a quarter turn toward the jump) and `ladder_jump_back` (half turn); then S289 flies her onto what
  q22 met by its class (ledge, ladder 611, beam 625, …), else the free leap at q22's speeds (the jump back used to be
  500 out / 900 up [I]). Ours: the take-off's root motion, then the first ledge (`ledgeOnArc`: q22 from the attach
  point 568.9 down and 63.21 out, taken as her root, its ledge type 3 arc 568.9 over it [I]), ladder rung (root within
  220 [I]) or balance beam (coming down through its height within 252.84 of it: the beam record's radius, its use [I])
  met, a beam landed on winning over a ledge met first (beams are flanked by ledges: 2042 between 2065 / 2066, which
  the ledge capsule meets earlier) [I]; a ledge or ladder is flown onto (HangJump, `ledgeb_attach_S` / `_F`, `ladder_attach_M` [I: the
  attach request 1161 has U/M/D/S/F/FS variants, not decoded]), a beam landed on. Leans [O-data]: the stick to the side
  enters S1083 / S1082, whose move queries q91 / q90 (from the hand, bones 53 / 78, 15.80 a frame sideways for 10
  frames, beyond 158) finding a ladder (not a beam) go to the reach states S1093 / S1094 (`ladder_reach_left` /
  `_right`) and through S614 / S1068 onto it (`ladder_settle_left` / `_right`); else she leans
  (`ladder_jump_<side>_prepare` when q22 has a target, `_prepare_untargeted` otherwise [I: by the clips' first poses])
  until the stick lets go. Our reach: a parallel ladder on her wall 158..341 aside with a rung at her height [I: the
  reach / settle clips hand over 311 + the 30 box]. On the lowest rung the chart steps off only on LookaheadResult 640
  and leans back on 642 (S1086 → S1088): ours leans (`ladder_idle_to_jump_back_antic`, `ladder_jump_back_prepare`,
  released `ladder_jump_back_antic_to_idle`) when no floor is where `ladder_detach_ground_bottom` ends [I: 640 = floor].
  Real spot: l7_arctic_sea ladder 2044 hangs over a pit, balance beam 2042 (between ledges 2065 / 2066) crossing 853 out
  from its wall (behind her) at z −18899: rungs ~9..12 jump back onto the beam (`GameReal.ArcticSeaLadderJumpBackOntoBeam`). Not measured under Wine [U]: teleporting into that chamber
  works (tru = ours − Level+0x80 = (5610.33, −22439.56, −19571.3)), but she did not attach to the ladder from a
  teleported fall; side-by-side ladders (reach) exist in no unit (stacked ones: l1_ship_sinking_hall 28 / 30, reached up
  by q88 S1095, not done).
- Ledge corners [O: clips; I: chart]: lara.drm's hang corner set (requests 1030..1046): `ledgeb_concave_right_phase1`
  (45° right, ending with the hands 38 in front: `ledgeb_concave_idle`'s pose) / `_phase2`, `ledgeb_convex_right_phase1`
  (206 right, 45° left, hands 212 out: `ledgeb_convex_idle`) / `_phase2`, and the corner leans `ledgeb_<kind>_jump_up_antic`
  / `_jump_back_antic` (loops). 740 ledge joints turn more than 45° in all units, 573 of them 85..95°
  (openunderworld-work/ladderleans/survey.txt). Ours: shimmying onto such a joint plays phase1 into the corner pose
  (hands on the joint, facing between the walls; concave where the next segment turns toward her side), the corner idle,
  the stick up / back leans, the stick on along the ledge phase2 onto the next segment (back: phase2 mirrored onto the
  first); left = mirrored. The hang group's states use local request numbers, so which state plays which is [I];
  jumps at a corner are the hang's; corner attaches (`ledgeb_<kind>_attach_*`) are not used [U].
- Jump query hit test [O: decomp 0x771830 / 0x70F300 / 0x6F9940 / 0x70FFB0 / 0x70EC60 / 0x6FCB10 / 0x6FC530 /
  0x6FC6B0, disassembly of 0x70EC60's stack]: 0x771830 builds the query: start = base + up (+0x58) + out (+0x5C) along
  her facing (+ +0x60 sideways), the base being her root (+0xCC = 0; + bone +0xC8's offset when set), the attach point
  (player +0xCA0, +0xCC = 1) or a point of the markup she is on (+0xCC = 3, 0x72AD10); velocity +0x08 turned by the
  query's yaw, gravity 5.72443 (0.001 with +0x2C bit 0x400), frames +0xA4 or 40 (object +0xBDE). For each markup the
  sweep 0x70F300 takes each target type of the markup's record (lara.drm player data +0x584..+0x5C0, chosen by MUD
  flags in 0x5D57A0; ledges: the default record, types 3 then 1) with its own arc start (the query's +0x8C entry for it
  replaces the start offset, 0x6F9560; none: the start), the times of closest approach of that arc to the segment's
  line (0x6F9940: a cubic, at most two minima, clamped to the frame range) and the nearest segment point (the
  polyline's first / last ends inset by the record's +0x98 / +0x9A), past an end the closest approach to that end
  (0x6F96C0). 0x70EC60 then accepts a hit when: its time > +0x20; for +0x2C bit 0x80 the query's own arc is coming down
  (vz < 0.05 a frame, 0x6EC3D0), otherwise it is farther than +0x28 horizontally (0x6EC360); the point lies in the
  record's capsule round the **query's own arc point** at that time (0x6FCB10, `this` = 0x70EC60's arc argument, not the
  target type's arc): centre = arc point + the record's +0x30 offset turned by the query's yaw (+Y = behind her),
  half-height +0x54 / 2, distance to the axis or the nearer end point ≤ +0x58 (+ the query's +0x78, and +0x7A on the
  offset, when the query's +0x65 flag is set); unless that flag: within +0x24 of its heading (0x6FC530) and, for ledges
  (MUD 0x40000000), the arc's horizontal motion (standing still: the way from the start to the point) has a dot ≤ 0.2618
  with the ledge's outward normal (0x737D60; 0.5236 for a moving markup, 0x6FC6B0); the record's height window (+0x84 ..
  +0x8A, −900..300 for ledges) is tested against 0 (0x6EC320). Records: ledges (0, 12.64, −219.03), height 252.84,
  radius 316.06, insets 79; ladders (0, 0, 100) + 189.63, 150, 379.27; balance beams 252.84, 252.84; holds 284.45,
  221.24. Ledges hang to the right of their polyline's direction: all 106 ledges with a wall under one side and their
  top behind it in twelve surveyed units are wound so [O-data]. Ours (`Player::ledgeOnArc`): exactly this for ledges
  (types 3 and 1, the hang side by winding), plus a clear hang (`openBelow`) and the earliest hit [I: 0x710100 keeps
  its list's first]. This default path (query +0x94 = 0: 0x70FFB0 → 0x70EC60) is not what Lara's jump queries take:
  every one checked (q2, q26, q43, q31, q28 / q29, q22 / q23, q7 / q9 / q10, q32 / q33) names a class record at +0x94,
  so 0x7296C0 runs it (0x729670 → 0x729540) on each swept candidate instead, then classes the winner by +0x9C
  (0x728C30 picks it, 0x729540 sets the class bits) [O: decomp; Wine hooks below]. A candidate counts when one entry
  passes (0x729540 stops at the first). An entry is gated by the candidate markup's type (0x723330: entry flags 0x10,
  bit 0 = not in its list) and, while she is attached (query state +0x280 = the candidate built from player data
  +0xC94), the attached markup's type (0x723380: flags 0x04, bit 3 = not in the list), then is an AND of test records
  `{kind, point source a1, point source a2, value, …, flags: bit 0 the record's value (else the query's own: kind 1
  +0x28, 13 +0x20, 22 +0x24), bit 1 negate, bit 2 active}` (0x728F00); point sources (0x723AB0): 0 her root, 4 / 31 the
  hit (ledge point), 9 / 10 the heading / right, 13 the target type's arc at the hit time, 24 the hit less the query
  start and linear motion, 27 / 32 her z / the hit less the record's +0x68, 28 the query's base point, 33 a hold's
  point. Kinds [O: decomp]: 1 horizontal distance < v; 4 z(a2) − z(a1) ≤ v; 10 the ledge record's capsule round source
  13 holds the hit; 11 a moving markup's angle (static: passes); 12 the arc's vertical speed at the hit < v; 13 hit
  time < v; 14 / 18 / 19 / 20 the ledge class (0x728A80: not 3 / 11 / 12 / 13, = 6, = 5, 8..10); 21 wall side; 22
  cone; 31 the hit at most player data +0xC18 = **1583.44** under her; 33 for classes 5 / 6 the class table's +0xA4
  value (0x726420) > v; 34 source a1 along direction a2 < v; 36 horizontal distance < v + the drop when the hit is
  below her, only while her base and the markup are still (≤ 3.1605 a frame, 0x725F40: always in the static world).
  The ledge class (only for queries with +0x2C bit 0x40; table objdata +0x121C0 by 0x728650): the floor 94.815 out of
  the face below the ledge point 0..154.87 down = 11, 154.87..312.90 / ..474.08 / ..635.27 down = 5 / 6 / 7, or 8 / 9
  / 10 when she stands on the ledge (< 6.33 over her feet, facing out of it), each needing the clearance test 0; later
  entries need a moving markup (0), the chart's test 1 (0x74BB50, class 4) or 0x726420's value, a floor over 632.1
  down with clearance = 12, the rest 3 [O-data, decomp]. Ours: 4 for everything past the bands [I: test 1 not decoded;
  the Wine standing jumps catching 782..907 ledges need a class kind 14 admits]. The records (ledges = markup type 1;
  holds 16..20) [O-data, `openunderworld-work/runjump/cls.py`]:
  - **0x2AB8C** (q2 / q26 / q43, and q1, q6, q20, q94, q102..q104): entry 4 (types 4 / 1, the "box") = rise of the
    ledge over her root in (300, 885] (kind 4), source 24 along her heading in [−150, 632) and to her right in
    [−316, 316) (kind 34), wall side, cone, class 14, not 19, not 20; entry 2 (not types 14 / 15 / 2 / 13: ledges) =
    the capsule (kind 10) round source 13 — the TARGET TYPE's own arc point, not the query's: a hook on 0x6FCB10 under
    Wine logged its centre at root + 707.27 (the hands' apex, 505.69 + 201.6) for type 3 and root + 201.6 for type 1
    — with the hit within **1581** of her (kind 36), cone, wall side, after +0x20, depth (31), class 14, not 20, not
    19, kind 33; entries 0 / 1 / 3 for beams (14, the arc's vertical speed < 1, 1700), type 15 and types 2 / 13.
  - **0x2AA4C** (q7 / q8 hang sides, q28 / q29 DICE sides, q30, q38): from a hold (entry 3, any target) the hit 410
    or more (+0x28; kind 1 a2 = hit, its record value 360 unused) from the base point, capsule, cone, wall side, after
    +0x20, depth — no class; not from a hold (entries 1 / 2, any target) the hit **553** or more from the base point
    (q7 / q8 +0x28), capsule, cone, wall side, after +0x20, depth, class 14, not 18 (6), kind 33, and either a rise
    over her root ≤ 700 (entry 1) or not 20 (entry 2); entry 0: beams.
  - **0x2A9EC** (q9 / q10 hang back / up, q22 / q23 ladder, q32 / q33 poles and 17 more): entry 2 (not types 14 /
    15: ledges) = capsule, cone, wall side, after +0x20, depth, class 14, kind 33, not 18 (6), not 20 (8..10); entry
    1 (type 15): rise ≤ 700, …; entry 0: beams.
  - **0x2AE2C** (q31, the DICE jump up): entry 0 {flags 0x16: holds, attached to a hold; hdist(base, the hold's
    point) < 120, capsule, cone, wall side, after +0x20, depth} and entry 1 {0x1B: not holds (the attach list unchecked:
    no 0x04); capsule, cone, wall side, after +0x20, depth, class 14} — ledges from a hold by entry 1.
  - **0x2A9CC** (q44, the air grab of the in-air states S1006..S1012, S1013..S1015, S1030, S267, S1296; q45 = record
    0x2AB0C, the same entries 1..4 with an entry 0 for ledges beside her, S1028 / S1029): entry 0 type 8; entry 1 any
    type but needs her base moving (kind 27: query state +0x30 squared > 1); entry 2 (not holds: ledges) = capsule,
    cone, wall side, after +0x20, her motion within **24°** of the way from the query's linear point to the hit (kind 25,
    0x725980: both flattened; a nil one passes), the hit within **316** of the target type's arc point (kind 1), at most
    **1106** under her (kind 31's own value), the ledge class not 5..11 (kind 16, 0x728A80); entries 3 / 4 holds (45° /
    hold point within 120 / |Δz| ≤ 900, kind 3 = 0x725870's absolute variant). q44 starts 505.7 over her root (target
    types 3 there, 1 at the root), velocity (0, 0, −0.01) with +0x04 = 1 [I: plus hers], frames 0..24 (+0xA4), cone
    120°, +0x2C 0x99E0 (coming down, the ledge classes) [O-data].
  Ours (`ledgeOnArc` with `JumpQuery::record`): these entries and tests as listed for ledges (an interpreter over the
  decoded tables, Player.cpp), the sweep's candidate pad below for every record query, and two of ours: a clear hang
  (`openBelow`) and, for queries with +0x2C bit 0x80, only hits while the arc comes down (a default check, 0x70EC60,
  not in the record path; ours keeps it to time the re-aimed flight [I]). The winning entry is kept (`entry`). q44 /
  q45 are decoded only: ours keeps `tryGrab` for catches in the air [I].
  Ground jumps: q2 standing (from 300 over the root, 48.04 up, coming down only, cone 120°), q43 with the stick (from
  the root, 40 + 50, 90°), q26 running (from 300 over, 90.25 + 42.54, 90°), their type 3 from 505.69 over the root,
  type 1 at it; DICE q31 / q28 / q29 and ladder q22 / q23 as described above. l3_main_hall ledge 944: the Wine jump stood
  on its back (it hangs toward −x; Lara 113 behind its line on +x), which the wall-side test refuses.
- Air-grab window [O: tru.exe under Wine, l4_temple, Lara teleported to rest facing the ledge, then the jump key;
  openunderworld-work/ledgecatch, runjump]: a **standing jump** (S248, q2) catches ledges 782 and 830 over her feet from
  100..**424** out of their line but none from 425 (a plain 201.6 rise; hooks on 0x723AB0 / 0x6FCB10: at 425 no class
  test runs at all — the sweep-and-prune pass over x / y, 0x70F300 → 0x463DE0 axes mask 3, yields no candidate), the
  892 and 907 ones from 100 but not from 125 / 150 / 175 / 200 (over the box's 885 only the capsule round the hands'
  apex reaches them: 907 from 107.6 at most), a 1233 one not even from 100, and 698 / 748 / 802 / 883 ones from 100.
  A hit sends S278 (T256) to S289, which picks by the ledge class (0x728650, the floor 94.8 out below the ledge): 0 /
  2 / 12 → S535, 5 / 6 / 7 → the hurdle attaches S958 / S959 / S957 — so ledges up to the obstacle bands' top (635.27)
  are hurdled (a 610 one from 100..200 out: `hurdle_ground_attach_high`) [O-data]. S535 is no arc: the root rushes
  straight at a constant speed, whatever the distance, for 11 ticks of 60 Hz to 113.8 out of the line and 604.2 under
  it, then blends into the hang (root 161 out, 506 under) over 0.15 s. Ours (`Player::standJumpToLedge`): q2's class
  hit above 635.27, the hang clear: a straight 11/60 s leap onto the hang pose, then `ledgeb_attach_S` [I: one leg
  for the rush and blend]; the candidate pad is **424.5** [O: between 424 caught and 425 not; its source, the markup
  boxes in the sweep-and-prune set, is not decoded]. Earlier teleported falls (l7_arctic_sea beam 2042 / ledge 2065)
  gave no window: the result depended on the fall state the teleport left her in. Falling catches remain `tryGrab`
  [I].
- Running jump reach [O: tru.exe under Wine, l4_temple ledge 1204 (782 over the floor, hanging toward −y), Lara run
  along +y from 2400 short with the camera pinned behind her (the stick is camera-relative: the camera's position +0x90
  and its target +0x150 written each millisecond 1264 behind her, so her heading follows the axis; locking her yaw
  +0x38 alone fought the steering), jump at varied times; openunderworld-work/runjump2 rj2.sh / tk.sh]: from a take-off
  **1222.9** short of the line she is caught, from **1243.4** not (a plain 65.83 + 42.54 jump, landing 285..300 short);
  from 1218..649 the flight is re-aimed (state 0x…550): launch 59 up and Δ / 10.5 forward a 30 Hz frame (92.9 from
  1222.9, 50.3 from 806), **10.5 frames** in the air for take-offs 781..1218 (8.0 from 649..678, 11.5 from 741..763), to
  a catch 239..259 out of the line and 477..501 under it, then the blend into the hang (161 out, 494 under); from 614
  and closer (204, 614) S535's straight rush: 11 ticks at constant speed to 153.6 out / 579 under, then the hang. In
  the chart S289 picks S535 when cond 0x2BA = 2 (a class bit of the winner, set by the query's +0x9C record in
  0x729540) and the ledge class is 0 / 2 / 12, else S296 (the re-aimed jump) [O-data]. Ours
  (`Player::aimGroundJump`, running): q26's class hit by entry 2 (and a ledge class outside the hurdle bands) = the
  straight rush (11/60 s onto the hang pose; ours switches near 634..719 out against tru.exe's 614..649); by entry 4
  (the box) = the re-aimed arc, rising to the hang pose and arriving at the top of the arc (vz = √(2 g Δz): 10.5 frames
  for 1204's hang) [I: tru.exe's flight time is not the query's hit time — constant over 781..1218 — and the rise fits
  59 = √(2 g · 306) to the catch point]; box hits farther than **1233** from her root are refused for ledges outside the hurdle bands [O: the limit; I:
  its mechanism — kind 34's along-heading window does not cut there with the query's own motion].
- Hang leans [O: tru.exe under Wine, l3_main_bedroom ledge 973, chart states read at *(player+0xF10)+0x510]: the stick
  held up keeps her in S434 (no climb, the top climbable), back S456, right at the ledge's end S457 → S449, until
  released (S444, the idle). These states run the hang jump queries (q10 up, q7 / q8 sides, q9 back). Ours plays
  `ledgeb_idle_to_jump_<up|right|back>_antic`, its `ledgeb_jump_<…>_antic` loop, and `_antic_to_idle` on release
  (left: right mirrored); jump from the side lean takes off with `ledgeb_jump_right_from_antic` (it starts in the
  lean's pose, as `ledgeb_jump_back` starts in the back lean's) [I: clip by name, the group's requests are not decoded].
- Pillars: attach facing the shaft from the floor (`pillar_attach_ground`, **199** up the shaft: player data short
  +0x7EE read by the `PillarAttachGroundAnimTarget` 0x753F80 [O]) or the air (`pillar_attach_M`); the root on the axis
  (tru.exe under Wine, l4_temple 1189 / 1194: Lara's instance stays on it; the clips hold her body round the shaft) [O].
  The stick is screen-relative like the original's quadrant (cond 0x195; Wine: W climbed whichever way she faced):
  up = `pillar_climb_up` at a steady **312 units/s** (10.40 a frame [O]), down = `pillar_slide_down_start` (its root
  motion: 237 in 0.33 s, as the Wine trace) → `pillar_slide_down` at the code's speed, not the loop's root motion: 10 a
  60 Hz frame for three frames, then a steady **900 units/s** (Wine, 1194 [O]) → `_stop`; right / left =
  `pillar_climb_right` (mirrored to the left) taking her round the shaft at **π/2 rad/s** [O]. Screenshots (Vulkan,
  mid-climb and mid-round): her body stays outside the shaft with the root on the axis. At the top a perch on it: `pillar_detach_perch` (341.8 up) onto it;
  at the foot `pillar_detach_ground` (193 down, half turn). Space jumps by the stick's angle from straight out
  (state S1213, conds 0x1A9 / 0x1AA [O]): within 22° `pillar_jump_forward`, to 78° the same mirrored on the left, to
  112° `pillar_jump_right`, to 155° `pillar_jump_back_45R` (left mirrored; which side mirrors is [I]), else or no stick
  `pillar_jump_back`; she leaves along the stick (no stick: straight out) at query q48's **55.63** out + **28.45** up a
  30 Hz frame [O] (Wine: 55.8 out). Ctrl = `pillar_detach_drop`.
  The climb limits (top − 341.8, foot + 193) are [I].
- Perches: from the air or a standing jump next to the segment (`perch_attach_air/_ground`), balance with
  `perch_idle`; the stick turns her on the spot (`perch_turn_right_45` / `_90` / `_180`, requests 668–670; to the left
  the same clips mirrored, 671–673 [O]; bands < 67.5° / < 135° / beyond [I]); Ctrl on a pillar's top perch =
  `pillar_attach_perch` (315.9 down onto the pillar), elsewhere `perch_detach_drop`. Jumps by lara.drm's chart [O]:
  Space with the stick (S849 → S850) = `perch_jump_forward` on query **q37**, **74.32** out + **42.54** up a 30 Hz frame
  from the root (cone 90°; its class record 0x2A98C is the ground jumps' 0x2AB8C but for its third entry), re-aimed at
  a ledge its arc meets as the ground jumps (`aimGroundJump` with the ground record [I]), else the jump assist; ~1100
  across on the level (it was a placeholder 600 out + 1200 up: ~280). Space alone (S848 → S846 / S847) =
  `perch_jump_up` on **q36** (63.21 straight up), back down onto the perch; the launch at the press is [I] (the chart
  plays `perch_jump_up` on the perch, then `perch_jump_up_inair`). `GameReal.CavernsPerchJumpReaches`.
- Wall hug [O: chart and exe; FINDINGS in openunderworld-work/poleshimmy/wallhug]: a ledgegrab (1) point whose top is a
  narrow shelf — tru.exe 0x74B920 / 0x74BB50 (markup condition value 628) in the segment frame of 0x737D60 (X = the
  horizontal of p[i] − p[i+1], Y = Z × X): rays 252 (+0x6F0) above the line from 100 (+0x6E8) on the hang side toward
  −Y hit a wall within 139 (+0x6EA) but not within 88.496 (+0x71C), and floor lies 30 below to 40 above the line 88.496
  behind it. 92 of the 1 227 ledgegrab markups in 26 units have such stretches. Walking along the stick onto one within
  30 / 200 of it (query 96's rule): `bbeam_walk_turn180` (request 193) turns her back to the wall, `Wall_Hug_Idle`;
  sideways `Wall_Hug_Shimmy_Left` (mirrored to the right [I]); where the shelf ends `Wall_Hug_Walk_Off` (mirrored);
  She stands 88.496 in from the line [I]. Not done: the swim climb-out onto a shelf (`swim_climbout_to_wallhug`), the
  jump-landing attach and its bad-attach clips (`bbeam_idle_adjust_fwd` → `Wall_Hug_attach_bad_*`), the stick-forward
  / back wobble states (S1233 / S1234), combat in the hug (S1272).
- Wall-hug corners [O: chart S1236 / S1237 T2076 / T2078 / T2079 / T2081; tru.exe 0x764AA0 → 0x733E00, radians ×
  57.29578, compared by 0x783BD0 (mode 1 ≥, mode 2 ≤)]: shimmying with the stick held onto a corner of 70..110 plays
  `Wall_Hug_Shimmy_to_Concave_left_<70|90|110>` (request 1400), of 250..290 `Wall_Hug_Shimmy_to_Convex_left_*`
  (1399); the right-hand shimmy's transitions carry flag 2 (mirrored [I]). The clips turn the root only 45° (±0.61 /
  0.79 / 0.96 rad for 110 / 90 / 70) and move it ~100 sideways; their states set property 425 = 0 [I: root motion
  off] and go on to `bbeam_idle_adjust_fwd` (156 / 157, property 1102 = 22; [U] which clip that resolves to, ours
  skips it). Ours: the angle on the wall side between the segment she leaves and the next (inside corner 90, outside
  270) [I: 0x733E00 measures it from the 0x74BCF0 lookahead's point, not decoded]; her stand line turns D·tan(φ/2)
  before the joint (D = 88.496, φ the turn toward the wall), the corner starts s before that point and ends s past it
  on the next segment, s making the chord the clip's own travel (≈73 for 90°) [I], the clip carrying her between
  them (`warpStep`) and the heading turning a quarter round; the clip of the three nearest the angle (concave at it,
  convex at 360 − it) [I: the original blends them]. Corners outside both bands are passed through as before [I].
  Survey (`openunderworld-work/wallhug2/corners.cpp`, 97 units): concave hug corners on l7_arctic_sea 2054–2062,
  convex ones on l5_valaskjalf (1494, 1502, 1516, …) and l4_traprooms 1364–1377.
- Wall-hug jumps [O: chart S1232 T2070 / T2066 / T2067 (MoveDir 426 / 428 / 427) → S272 `Wall_Hug_Jump_Forward`
  (1416), S273 `Wall_Hug_jump_left` (1410), S274 the same with property 201 (right, mirrored); all under S275 whose
  jump query (prop 541) is **q68**: from the attach point (+0xCC 1) 63.21 out along the jump, **74.32** that way +
  **42.54** up a 30 Hz frame, cone 90°, +0x28 20, +0x2C 0x1850 (ledge classes), class record 0x2A9EC, target types 1
  at that start / 3 from 505.7 over it (+0x8C); S289 then flies her onto what it met. Jump with the stick neutral or
  back (T2060: not MoveDir 410 / 412 / 413) → S1261 (prop 546 = **q55**: **50.82** straight up from the attach point,
  frames 1..17, cone 180°, +0x28 −1, +0x2C 0x38A0 (coming down), record 0x2A9EC) → S1258 `Wall_hug_Jump_Up_Twist`
  (1403: rises 63, turns 150°) → S1256 in the air onto the target; with no target (T2056, JumpTargetSameAsAttached 0
  [I: read as "none"]) S1257 `bbeam_somersault_forward` (161, property 425 = 0) back to `Wall_Hug_Idle`.] Ours
  (`Player::wallHugJump`, Traverse.cpp): the stick's dominant axis relative to her picks forward / left / right /
  twist [I: MoveDir's mapping]; `ledgeOnArc` runs q68 / q55 from the ledge point she is at (the attach point [I]);
  with a ledge met the take-off clip plays with its root motion and turn, then `launchHangJump` aims the flight at
  its hang (take-off vertical speed q68's 42.54 / q55's 50.82 a frame), `ledgeb_attach_S` (twist: `_U`); else the
  free leap at q68's launch after the clip, or the somersault on the spot for the twist. Beams, ladders and other
  shelves as q68 targets (S289's other classes) are [U].
- Wall-hug drop to water [O: chart T2069 c66:1 (input action 0x32) + c32d:1 → S1270 `Wall_Hug_Drop_to_Water` (1377,
  mirrored; 214 forward, 188 down of root motion) → S1012, the fall; from the swim group S1326 → S1327 → S101 swim;
  c32d = tru.exe 0x7420D0: a ray from **15** (+0x72C) over her root and **47.41** (+0x730) in front of it down
  **632.11** (+0x734) whose first hit is water]. Ours: the drop button (c66 [I]), our water volumes against a terrain
  ray; standing 88.496 in [I], the probe lies 41 in from the line, over the shelf's own floor wherever that floor
  runs the full depth, so it fires only over a narrower lip (and only l2_cliffclimb 936 sits within reach of water at
  all, FINDINGS) [U: her real stand depth, which decides this].
- Tests `GameReal.LadderRungsClimbOffTop`, `GameReal.LadderSideJumpsReachAndBack`, `GameReal.ArcticSeaLadderSideJumpOntoBeam`,
  `GameReal.HangShimmyRoundCorners`, `GameReal.PillarAttachRoundUpSlideOff`, `Game.PerchAttachAndJumpOff`,
  `GameReal.ShipSinkingHallLadder`, `GameReal.TemplePerchTurnsAndPillarTop` (l4_temple perch 1190 on pillar 1191),
  `GameReal.PuzzleRoomWallHug` (l2_puzzleroom shelf 476), `GameReal.WallHugRoundsConcaveAndConvexCorners`,
  `GameReal.WallHugSideAndTwistJumpsByTheirQueries`, `GameReal.WallHugDropToWater`, `GameReal.WallHugIndependentOfRenderRate`,
  `GameReal.TrapRoomsPoleParallelHandOverHandAndTurns`,
  `GameReal.TrapRoomsPoleSwingStops`, `GameReal.PoleTraversalIndependentOfRenderRate`; saves mid-climb, mid-hand-over-hand,
  mid-swing, mid-turn on a perch, mid-way onto a pillar and mid-shuffle in `StreamReal.SaveContinuesBitIdentically`.
  Object-DRM markup does not exist in this data (COLLISION.md), so all traversal markup comes from the units.
- Not in TRU [O]: ropes (`rope_*` clips 1798–1813), vertical poles (`vpole_*` 1814–1817) and zip lines: no anim
  request names a `rope_` or `vpole_` clip (all 1 502 of lara.drm's requests), no state requests them, no unit has
  rope (11), vertpole (5), zipline (7) or cover (12) markup (all units, 2026-10-18 and 2026-11-06), and lara.drm has no
  zip-line clip at all. The `rope_*` / `vpole_*` animations are leftovers; the only rope is the grapple's.

## Moving markup (ledges, poles, beams and holds that ride objects)

Code: `World::tickObjectMarkup`, `World::tickObjects` (the ride), `World::tick` (the velocity she leaves with),
`Player::attachedMarkup` / `rideFrame`; data `physics::ObjectPhysics::markup` + `markupSegment`. Tests:
`Game.MovingMarkup*`, `GameReal.MovingMarkup*`, self-test `moving_markup`, a `StreamReal.SaveContinuesBitIdentically` case.

### The original [O tru.exe, object data]

- **Where moving markup comes from.** Only from object models. A model DTP lists markup items (+0x54 count, +0x58 ->
  0x5C-byte items: +0x28 flags, +0x2E id, +0x30 s16 **segment**, +0x33 type, +0x58 -> polyline). `0x5D0610` hands the
  items of an instance's current model to `0x5CE4F0` with the instance as owner: one markup box per item (box +0x0C = the
  instance, +0x20 = the item), listed on the instance (+0x2D8). It runs at instance creation (`0x5D07E0`), on a model
  change (`0x5BA480` / `0x5BA650`) and for uber object sections (`0x4D78D0`), which are instances of their own with their
  own models. The earlier note that `0x5D0610` "parses a global list" was wrong: the level's 0x5C list (Level +0x08) is
  empty in every unit, and the function is called per instance with that instance's model.
- **No level markup is tied to an instance.** A unit ("markup2") item's +0x4A is copied to the box's +0x2C, which
  `0x5C41A0`'s debug text calls the box's introID; it is 0 on every markup item of the 75 units reachable through
  portals (survey 2026-11-10), and unit boxes have no owner (+0x0C = 0). So a ledge that moves is always an object's.
- **How it moves.** `0x5C41A0` places a box by its owner's matrix of the item's segment (instance +0x90 + 0x40 x item
  +0x30): item points are in that segment's space, and the box follows whatever moves the segment — a joint, a clip, a
  spline, a push — with no per-mover code. Examples: `ar_ani_infinite_chain` has one pole item on each of segments 1-6,
  all with the same local points; `ar_ani_thor_hammer_*` ledges ride segment 2 of 5, `ca_ani_adren_blades` a ledge and
  a beam on each blade segment.
- **How Lara is carried.** She is on a box (player +0xC94), and her place is a position on that box, so she moves with
  it. Her carrier's velocity is one function, `0x7027B0`: the instance and segment are taken from, in this order, an
  explicit handle, player +0x610 (+0x618 segment), her markup box (owner +0x0C, segment item +0x30; `0x5BE2F0` first
  checks the box is enabled and its owner not flagged 0x2 / 0x20000000), player +0x420 (+0x428), and last the instance
  and segment she stands on (her instance +0x228 / +0x226). `0x5D75B0` then gives (segment matrix now x its inverse one
  frame ago) applied to her last position, minus that position, per frame. So hanging on moving markup and standing on
  a moving floor are the same case with a different source of the segment.
- **Leaving it.** `0x714630` = her velocity (instance +0x2A0) + `0x7027B0`: every reader of "Lara's velocity" gets the
  carrier's added. `0x7726D0` (a launch from the current jump query, `0x76FFC0`) adds it to the launch velocity
  (player +0xA40). `0x7694B0` is the same velocity for the jump queries' solver (`0x7296C0`), zeroed when it is no more
  than 3.1605 units per frame (94.8 units/s). So a jump or a let-go off a moving ledge, pole or platform inherits its
  velocity.
- [U] Not decoded: what a held markup's owner feels of her weight (the walkthrough's counterweight column in
  l4_staircase_xibalba, the snake-head poles of l4_pool and the scales of l2_puzzleroom come down under her; no
  message or force from the player's markup states to the owner was found yet), the camera's use of the same velocity
  (`0x5E7EF0`, `0x5FA920`, `0x5FF650`, `0x609830`), and what happens to her on a link of `ar_ani_infinite_chain` when
  its clip loops (every link segment steps back one spacing, 1264.2).

### Ours

- Every scripted object's model-0 items, and for an uber object the items of each section's current model, are markup
  in the merged collision after the units' own, keyed `(uid, model x 100 + item)` and named `"@uid"` in saves. Each
  tick an item is re-placed by its segment under the pose that draws and collides the object: intro x (script pose:
  body, push offset, clip palette) x the segment's bind frame; a section: intro x its joint's motion x its clip. Before
  this, items were placed by the object's first collision segment only, so objects without collision (the valgrind
  pole rings, chains, hammers, weights, runestones), items on other segments and every section model (the retracting
  `*_ei_hpole_*` bars, the column handhold, the endgame tower's rings) had none or misplaced markup.
- Queries (grabs, jump targets, pole leaps, beams) read the merged list, so they see the items where they are now.
- Attached (`Player::attachedMarkup`: hang, shimmy, drop-to-hang, pole, beam, ladder, perch, pillar, climb-up, a
  targeted leap in flight, a free-climb hold), the change of her item's frame over the tick is applied to her root, her
  heading (the turn about Z [I: a tilting frame does not pitch or roll her]) and the world-space points of a move in
  progress (`Player::rideFrame`). A targeted leap therefore flies in its target's frame and arrives on a pole or ledge
  that moved meanwhile.
- When a tick takes her from a carrier (the markup she held, else the dynamic collider she stood on) into free flight
  (jump, fall), the carrier's velocity at her position is added to hers (`World::tick`), except for a leap aimed at
  markup (solved to its target [I]) and a carrier that jumped more than 6000 units/s (removed or re-placed [I]).
  Nothing is stored for it: the carrier's frame before and after the tick is compared, so saves need no new field and
  a save while riding continues bit-identically.
- Saves name her item `"@uid"` + key as before; a load now puts the attachment back once the scripts and objects are
  restored (`World::restoreObjectMarkup`: the items exist only when their objects stand as saved). Before, an object
  ledge was dropped at load and she fell. The state hash (`World::stateParts`) names an object's item by its owner
  for the same reason: its index in the merged list depends on when the object was first placed. A script's terrain-group switch rebuilds the merged collision and renumbers
  the objects' items: her ledge, water volume and hold are kept by owner there too. An item that goes away under her
  (its object hidden, streamed out, its section on another model) lets her go.
- Real cases (`GameReal.MovingMarkup*`): by input, l5_valgrind cog wheel 7312 (standing jump onto a passing rim
  ledge, 10 degrees/s, let go at the rim's speed; the self-test `moving_markup` does the same in ou-game); rotating,
  l7_yggdrasil_end tower 8401's ring handholds; sliding, l5_valaskjalf chain 6939's link poles (174 units/s);
  counterweight, l4_staircase_upper ramps 6045's column handhold falling 948 on its joint. The last three set her
  hanging on the item (no floor leads there in ours yet) and use input from there.
- [I] / open: model 0 of a non-uber object is the model it is born with (script model switches are not followed);
  her weight does not move what she hangs from except the inserted-pole levers (`tickInserted`); the 94.8 units/s
  threshold of the jump queries is not applied; the camera does not use the carrier's velocity.

### Movers and other objects that carry markup, per unit [O data: every model of every placed object]

Class 79 = uber object, 34 = rigid body on a joint, 6 = anim-graph object, 30 = breakable (its pieces' items:
"Breakables"), 50 = portable (carried poles: no markup while loose; inserted: `tickInserted`). Combat crates
(`sh_ei_combat_crate_*`, a ledge loop each) are left out.

| unit | object, uids | class | items |
|---|---|---|---|
| l1_seatop | `ms_uo_yacht` 998 | 79 | 3x ledgegrab (model 0, segment 0) |
| l2_cliffclimb | `sa_ei_hpole_a` 3837 | 79 | 1x horizpole (model 1, segment 0) |
| l2_cliffclimb | `sa_uo_cliffclimb_ledge` 3844 | 79 | 1x ledgegrab (model 0, segment 0) |
| l2_cliffclimb | `ms_uo_yacht` 2566 | 79 | 3x ledgegrab (model 0, segment 0) |
| l3_bedroom_hall | `ma_rb_pushblock_training` 4363 | 79 | 1x ledgegrab (model 0, segment 0) |
| l7_yggdrasil_end | `ar_ani_floating_runestone` 8365..8388 (24) | 6 | 1x ledgegrab (model 0, segment 1) |
| l7_yggdrasil_end | `ar_uo_yggdrasil_endgame_gun` 8401 | 79 | 14x ledgegrab (model 0, segment 1), 12x ledgegrab (model 1, segment 0), 12x ledgegrab (model 2, segment 0), 12x ledgegrab (model 3, segment 0) |
| l5_valgrind | `ar_pu_cog_wheel_level_1a` 7312 | 79 | 4x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_pu_cog_wheel_level_2b` 7313 | 79 | 4x ledgegrab (model 0, segment 0), 4x ledgegrab (model 1, segment 0), 4x ledgegrab (model 2, segment 0), 4x pole (model 3, segment 0), 12x ledgegrab (model 3, segment 0) |
| l5_valgrind | `ar_pu_cog_wheel_level_3c` 7314 | 79 | 4x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringa` 7492 | 79 | 4x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringd` 7493 | 79 | 4x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringe` 7494 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringf` 7495 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringg` 7496 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorc_ringb` 7497 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floora_ringa` 7501 | 79 | 8x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floora_ringb` 7502 | 79 | 8x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floora_ringc` 7503 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floora_ringd` 7504 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringb` 7505 | 79 | 8x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorb_ringc` 7506 | 79 | 4x pole (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorc_ringc` 7507 | 79 | 16x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorc_ringa` 7508 | 79 | 16x ledgegrab (model 0, segment 0) |
| l5_valgrind | `ar_uo_valgrind_column_floorc_ringd` 7509 | 79 | 16x ledgegrab (model 0, segment 0) |
| l2_junglehub | `sa_ei_hpole_a` 3222, 3223, 3224 | 79 | 1x horizpole (model 1, segment 0) |
| l1_kraken_room | `ms_ei_hpole_a` 322, 323, 324 | 79 | 1x horizpole (model 1, segment 0) |
| l1_kraken_room | `ms_ani_sunsphere_plat` 27 | 79 | 3x ledgegrab (model 1, segment 1), 1x ledgegrab (model 1, segment 0) |
| l1_kraken_room | `ms_ani_tentacle_pillar_b` 34 | 79 | 3x ledgegrab (model 0, segment 0), 1x ledgegrab (model 0, segment 61), 1x pillar (model 0, segment 0), 1x pole (model 0, segment 0) |
| l1_kraken_room | `ms_ani_tentacle_platform_a` 35 | 79 | 1x ledgegrab (model 0, segment 60), 1x ledgegrab (model 0, segment 61), 1x ledgegrab (model 0, segment 0) |
| l2_hubhall | `sa_ei_hpole_b` 3102 | 79 | 1x horizpole (model 1, segment 0) |
| l1_ship_exterior | `ms_uo_yacht` 1552 | 79 | 3x ledgegrab (model 0, segment 0) |
| l2_maproom | `sa_pt_pole_maproom` 2665 | 50 | 1x pole (model 0, segment 0) |
| l2_maproom | `sa_uo_thor_bridge_combo` 2667 | 79 | 6x ledgegrab (model 0, segment 0) |
| l3_secretchamber | `ma_pu_thor_pillar` 4674 | 79 | 1x ledgegrab (model 0, segment 0) |
| l3_secretchamber | `ma_pt_pole_a` 4686 | 50 | 1x pole (model 0, segment 0) |
| l4_ballcourt | `ca_pt_pole_a` 6503 | 50 | 1x pole (model 0, segment 0) |
| l4_ballcourt | `ca_pu_block_a` 6504 | 79 | 1x ledgegrab (model 0, segment 0) |
| l5_valaskjalf_entryb | `ar_uo_wall_lever` 7188 | 79 | 1x pole (model 1, segment 0) |
| l5_valaskjalf_entryb | `ar_pt_pole_a` 7190 | 50 | 1x pole (model 0, segment 0) |
| l5_valaskjalf_entryb | `ar_pu_valaskjalf_entry` 7191 | 79 | 1x ledgegrab (model 0, segment 0) |
| l2_puzzleroom | `sa_ei_hpole_b` 2829 | 79 | 1x horizpole (model 1, segment 0) |
| l2_puzzleroom | `sa_pt_pole_a` 2967, 2968 | 50 | 1x pole (model 0, segment 0) |
| l2_puzzleroom | `sa_rb_pushable_weight` 2971 | 79 | 1x ledgegrab (model 0, segment 0) |
| l2_puzzleroom | `sa_uo_shiva_left_scale` 2066 | 79 | 2x pole (model 2, segment 0) |
| l1_kraken_entrance | `ms_ei_hpole_a` 681, 682, 683, 684 | 79 | 1x horizpole (model 1, segment 0) |
| l4_staircase_upper | `ca_ei_hpole_a` 6047, 6048 | 79 | 1x horizpole (model 1, segment 0) |
| l4_staircase_upper | `ca_ei_hpole_b` 6049 | 79 | 1x horizpole (model 1, segment 0) |
| l4_staircase_upper | `ca_rb_hallway_ramps_b` 6045 | 79 | 1x ledgegrab (model 1, segment 0) |
| l5_valaskjalf | `ar_ani_infinite_chain` 6939..6944 (6) | 79 | 1x pole (model 0, segment 6), 1x pole (model 0, segment 5), 1x pole (model 0, segment 4), 1x pole (model 0, segment 3), 1x pole (model 0, segment 2), 1x pole (model 0, segment 1) |
| l5_valaskjalf | `ar_ani_thor_hammer_four` 6945 | 6 | 2x ledgegrab (model 0, segment 2) |
| l5_valaskjalf | `ar_ani_thor_hammer_one` 6946 | 6 | 1x ledgegrab (model 0, segment 2) |
| l5_valaskjalf | `ar_ani_thor_hammer_three` 6947 | 6 | 1x ledgegrab (model 0, segment 2) |
| l5_valaskjalf | `ar_ani_thor_hammer_two` 6948 | 6 | 1x ledgegrab (model 0, segment 2) |
| l5_valaskjalf | `ar_ani_weight_with_ledge` 6953, 6954 | 6 | 2x ledgegrab (model 0, segment 1) |
| l5_valaskjalf | `ar_ani_weight_without_ledge` 6955, 6956, 6957, 6958 | 6 | 1x ledgegrab (model 0, segment 1) |
| l5_valaskjalf | `ar_ei_break_braced_corner` 6959 | 30 | 1x ledgegrab (model 0, segment 0) |
| l5_valaskjalf | `ar_ei_break_braced_ledge` 6960, 6961 | 30 | 1x ledgegrab (model 0, segment 0) |
| l5_valaskjalf | `ar_ei_breakable_hpole` 6962 | 30 | 1x pole (model 4, segment 0) |
| l5_valaskjalf | `ar_ei_running_chains` 6972 | 34 | 1x pole (model 0, segment 0) |
| l5_valaskjalf | `ar_pu_valaskjalf_entry` 6973 | 79 | 1x ledgegrab (model 0, segment 0) |
| l5_valaskjalf | `ar_rb_hpole_hammer_release` 6979, 6980, 6981 | 34 | 1x pole (model 0, segment 0) |
| l5_valaskjalf | `ar_rb_hpoles_on_gears` 6982, 6983, 6984 | 34 | 4x pole (model 0, segment 0) |
| l2_upperhallway | `sa_ei_hpole_b` 1869, 1870, 1871 | 79 | 1x horizpole (model 1, segment 0) |
| l3_chapel | `ma_ei_hpole_a` 4745 | 79 | 1x pole (model 1, segment 0) |
| l3_chapel | `ma_ei_hpole_b` 4746, 4747, 4748 | 79 | 1x horizpole (model 1, segment 0) |
| l3_chapel | `ma_pt_pole_a` 4823 | 50 | 1x pole (model 0, segment 0) |
| l4_staircase_xibalba | `ca_ei_hpole_a` 6015, 6016 | 79 | 1x horizpole (model 1, segment 0) |
| l4_staircase_xibalba | `ca_rb_hallway_ramps_b` 6036 | 79 | 1x ledgegrab (model 1, segment 0) |
| l4_temple | `ca_ei_hpole_a` 4952, 4953 | 79 | 1x horizpole (model 1, segment 0) |
| l4_temple | `ca_pt_pole_b` 5240, 5241 | 50 | 1x pole (model 0, segment 0) |
| l2_towerroom | `sa_ei_hpole_b` 1905 | 79 | 1x horizpole (model 1, segment 0) |
| l4_traprooms | `ca_pu_block_grapple` 5884 | 79 | 1x ledgegrab (model 0, segment 0) |
| l4_traprooms | `ca_rb_crusher_slim` 5885..5890 (6) | 34 | 1x ledgegrab (model 0, segment 0), 1x ledgegrab (model 1, segment 0) |
| l4_traprooms | `ca_ani_adren_blades` 5897, 5898 | 79 | 1x ledgegrab (model 0, segment 2), 1x balancebeam (model 0, segment 2), 1x ledgegrab (model 0, segment 1), 1x balancebeam (model 0, segment 1) |
| l2_towerconnector | `sa_ei_hpole_b` 2035, 2036, 2037 | 79 | 1x horizpole (model 1, segment 0) |
| l4_pool | `ca_rb_snake_head` 5612 | 79 | 1x pole (model 0, segment 0) |
| l4_pool | `ca_rb_snake_head_scripted_fx` 5613 | 79 | 1x pole (model 0, segment 0) |
| l4_pool | `ca_uo_balancebreakb` 5614 | 30 | 1x ledgegrab (model 0, segment 0), 1x balancebeam (model 0, segment 0), 1x ledgegrab (model 3, segment 0) |
| l4_pool | `ca_uo_ledgebreak` 5615, 5616 | 30 | 1x ledgegrab (model 0, segment 0) |
| l4_pool | `ca_uo_ledgebreak_u` 5617, 5618, 5619 | 30 | 1x ledgegrab (model 0, segment 0) |
| l4_pool | `ca_uo_thor_statue_arms` 5620 | 79 | 2x ledgegrab (model 0, segment 0), 1x ledgegrab (model 1, segment 0) |
| l4_pool | `ca_rb_snake_head_revisit` 5632, 5633 | 79 | 1x pole (model 0, segment 0) |

What moves them [O data / scripts, as far as ours runs them]: the valgrind tower rings 7492-7509 and cog wheels
7312-7314 turn on uber hinge joints (the rings stand until the tower puzzle starts them; in ours the wheels turn for
about 9 s after the unit starts and then rest [U: what starts and stops them]; later Lara turns them by their push
panels); the endgame tower 8401's ring sections turn from the start; `ar_ani_infinite_chain` links run by clip (6939-6942
a few seconds after the unit starts, 6943 / 6944 from gear 6985); the hammers, weights and `ms_ani_*` kraken pieces by
anim-graph / uber clips their scripts start; the hallway ramps' column section, the snake heads, crushers and
`ar_rb_hpole*` on physics joints; push blocks and Thor pillars by Lara. The final level's platforms [O scripts, `OU_PROGRESSION_DUMP`]: the 24 `ar_ani_floating_runestone` 8365-8388 are
placed in rows far off the arena and are anchored by script (`instance` 8 (marker id) gives an anchor, `instance` 23
on the stone takes it) to markers 1-6 of the eight `ar_ani_runestone_orbit_a` rigs 8389-8396, three stones a rig, at
staggered heights round the tower; each stone has one ledge loop on its segment 1 and a player sensor on its marker
5. The 82 `natla_end_mover` spline movers carry `natla_end` (Natla: 8404's child is 8403), not platforms. Posing
anchored riders is implemented on agent/GpNatla (`instance` 8 anchors, `World::poseAnchored`); this branch only makes
a rider that carries markup collide once it is anchored to a spline mover or an animated rig (`World::syncObjects`),
and its items follow its pose like any other object's. Open after both are merged: the rigs do not turn in ours (a trial here with
the stones anchored: they sat at their markers, e.g. 8365 at 1948,-3375,11346, for 10 s; the rig is posed by a
5-bone clip palette whose time does not advance and it has no anim graph in ours [U: what plays its clip]), and the
rides themselves (C21-01, -10, -19, -20 of docs/matrix/21_out_of_time.md).

## Chimney jumps (src/game/WallJump.cpp)

A free jump that meets a wall plants Lara's feet on it; a jump press there turns her round and leaps back off it, higher,
toward whatever is behind her. Between two facing walls that climbs a shaft, and the last leap ends on a ledge. The
controls page of Stella's walkthrough describes the move
(https://tombraiders.net/stella/walks/controls/TR8controls.html, "Chimney Jump / Wall Jump"), its Coastal Thailand
pages the first place that needs it (https://tombraiders.net/stella/walks/TR8walk/details/remnants8.html).

**The original** (lara.drm player state chart and player data; tru.exe 1.1.0.0; dump script
openunderworld-work/gameplaygoal/wj.py, disassembly openunderworld-work/gameplaygoal/chimney/*.asm):

- States [O-data]: S1291 `wall_jump_attach_air` (request 1153, 3 frames, entered at frame 1), S1294 `wall_jump_idle`
  (1155, loop), S1295 `wall_jump_jump_back` (1156, 5 frames, a half turn of the root and no translation), S1296
  `wall_jump_detach_fall` (1157, 8 frames, root 33.7 back and 154.4 down), all under S1290; S1292
  `wall_jump_anticipate` (1154) and S1293 (the plain wall collision) beside them.
- Into it [O-data]: T2348 from S288 (the parent of every free jump's flight: S289 sends all jump take-offs, from the
  ground, a hang, a ladder, a pole or a wall, on to S294 / S306 under it), S293 and S1292, when the jump has no markup
  target (cond 0x262 kind 600 = 0) and `PSCCF_WallJumpAttach` holds (cond 0x203, 0x7633E0: 0x7594C0 returns 2). T2349
  goes to S1292 on `PSCCF_WallJumpAnticipate` (0x20B, 0x763330 -> 0x759260), T2350 to S1293 on `PSCCF_WallJumpCollide`
  (0x217, 0x763490: 0x7594C0 returns 1) once 5 frames into the state (cond 0x14).
- The wall test 0x7594C0 [O] (player data = lara.drm object data, offsets from its start):
  - The jump query in effect (player +0xA84 -> 0x6F1950) must not carry flag 0x80 in its +0x2C word, else the test
    returns 0 at once. The queries that carry it [O-data, lara.drm]: q2 (the standing jump), q10 (hang jump up), q23
    (ladder jump up), q31 (DICE jump up), q35 (pole-top jump up), q55 (the wall hug's twist jump), q13, q36, and the
    falls / drops (q44..q47, q49, q62, q64, q65, q72, q92, q93, q104, q106): the jumps launched straight up and the
    falls. q43 (stick jump), q26 (running jump), q9 / q7 / q8 (hang jumps back / sideways), q22 (ladder), q33 / q34
    (pole) and q12 (the wall jump itself) do not. tru.exe under Wine [O-runtime]: a standing jump 111 from l2_junglehub's
    unflagged column face, facing it, is a plain jump (apex 201.6, 0.55 s; three runs,
    `openunderworld-work/gameplaygoal/reference/chimney-wine2.txt` ja / jb / jc). An earlier reading of this gate as
    "not attached to markup with flag 0x80" was wrong.
  - 0x758DD0: the pelvis (segment 1) now and N frames on, N = byte +0x821 = **3** (the anticipate test uses byte
    +0x820 = 0, a ray of no length): along her heading by N x |velocity| (a frame's), and 2.862215 x N^2 down.
  - Ray 1 (0x4935F0, mask 0x2C19E7) between the two points raised by int +0x824 = **63**; ray 2 lowered by int +0x828 =
    **-158**. Both must hit terrain or an object (hit kinds 3 / 5); no hit on ray 1 = 0 (no wall).
  - 0x5D3BA0 refuses a terrain face whose client flags (face +0x0E) have **0x80**, and an object whose data byte +0x30
    has 0x10. 372 023 of the 814 628 collision faces of the 70 units carry the flag (all of l1_ship_lower_hallway,
    17 % of l2_cliffclimb), so the level builders chose the walls she may kick off.
  - 0x758F70: the wall's normal at most byte +0x831 = **7** degrees off horizontal (|n.z| <= sin 7); her horizontal
    velocity (her heading when she has none) within byte +0x830 = **45** degrees of square into the wall; her heading
    within byte +0x832 = **65** degrees of it.
  - zVel between -byte +0x834 (**13**) x 5.72443 and byte +0x833 (**9**) x 5.72443 a frame (-2232.5 .. 1545.6 units/s).
  - 0x7591E0: no floor within int +0x82C = **31** under her root.
  - 0x758B00: ray 2's hit within float +0x838 = **951.33** (horizontally) of player state +0x9C0, the jump query's
    +0x40 [I: taken as where the jump began].
  - All true = 2: the wall's normal goes to player state +0x5D0 and ray 2's hit to +0x5E0. A wall met that fails a later
    test = 1 (collide).
- On the wall [O]: S1291's anim target 0xEB (property 410, action 230 -> 0x758BB0) is ray 2's hit + int +0x83C = **110**
  along the normal at her own height, facing the wall. Its clip done (cond 0xC9) -> S1294; there state time >= chart
  variable 208 = **12** -> S1296 -> the fall S1006 (jump query q44). From S1291 and S1294 T2352: the jump button (cond
  0x98 value 136, the pressed bit) and `PSCCF_WallJumpAllowed` (0x21C, 0x7660D0: chart context +0x4EC < byte +0x843 =
  **5**) -> S1295.
- The jump back [O-data]: S1295 has property 917, so action 917 (0x75E350) adds 1 to +0x4EC; its exit function
  (0x75E390) zeroes it. The action is listed by S287 (the jump flights), S289, S1290 and S1292, so the count runs
  through a chain of wall jumps and ends with it [I: that an action common to both states is not exited between them].
  S1295's jump query is **q12** (property 541; run on entry, property 479 = 597): velocity **46.81** along direction
  source 0x13 (the wall normal at +0x5D0, 0x724F.. switch) + **77.27** up a 30 Hz frame under gravity 5.72443, start
  79.01 out, cone 90 degrees, +0x28 20, flags 0x1850, class record 0x2A9EC (the hang / ladder one), ledge target types
  1 at the start and 3 **372.95** over it (the hands of the wall pose are 370..377 over the root). Its clip done ->
  S289, which flies her onto what the query met or lets her fly free.
- Timing that follows [I]: off the wall she rises 77.27 a frame, more than the attach allows (51.52), for 4.5 frames,
  and the collide transition waits for frame 5: a rebound in a shaft too narrow to fly in still gains 77.27 x 5 -
  2.862 x 25 = **314.8** before her feet are on the other wall. The time unit of cond 0x14 is taken as 30 Hz frames
  for that reason [I].

**Ours** (State::WallJump; `Player::tryWallJump`, `wallJump`, `wallJumpQuery`): the test above, literally, on the 30 Hz
frames of a State::Jump flight that was not aimed at markup (`jumpAimed`: `aimGroundJump` / `assistJump` found a
target) and was not launched straight up (the flag-0x80 jump queries q2 / q10 / q23 / q31 / q35 / q55 all launch with
no horizontal speed, so ours tests the launch's horizontal speed [I: two straight-up queries, q27 (50.57 up) and
q110 (80.15 up), lack the flag; ours refuses the wall jump for them as well]), with collision rays for 0x4935F0 and
the bind pose's segment 1 height for the pelvis [I] (300 without a
skeleton). On the wall she is drawn to the 110 standoff over two frames, turned to face it, and holds her height:
nothing moves her up but the jumps. A jump press (edge) while `wallJumps` < 5 plays wall_jump_jump_back; when it ends
she faces away from the wall and either flies onto the ledge `ledgeOnArc(q12)` met (State::HangJump, the existing ledge
catch) or leaps free at q12's velocity (State::Jump: the existing air movement, ledge grabs and landings, and the next
wall). 14 frames without a press: wall_jump_detach_fall and State::Fall. `wallJumps` clears whenever she is not
jumping. Two limits on q12's ledge targets are ours [I], both needed for l2_junglehub's columns to play as the
walkthrough describes them: not the ledge the jumps began on (`jumpLedge`; tru.exe keeps the markup she was attached
to through the jump, player state +0xA84, and its chart has a `JumpTargetSameAsAttached` condition), and not a ledge
hidden from the arc's start by solid geometry (`JumpQuery::sight`: the query's 316 capsule reaches through the 320
thick column to ledge 804 on its far side). Saves carry the state (`controller.wallJump`; a save in the air still
resumes at its checkpoint, as before). Not ported [U]: wall_jump_anticipate (its test's ray has no length with the
data as shipped), the wall-collision state S1293, objects' refusal bit (data byte +0x30 & 0x10: every dynamic counts
as a wall).

**Where the game uses it** [O-data geometry; the first also by the walkthrough]:

- l2_junglehub, the streambed's square columns (320 x 320): ledge 796 on column A's east face (x = -12659, z 6349),
  ledge 797 round the same column 1248 higher, column B's face 638 behind her (x ~ -12021). Ours: the leap back off
  the hang holds on B 138 over the hang's root height, the jump back on A 435 higher, the next on B 435 higher again,
  and from there q12 meets 797: four jumps, the count the walkthrough gives.
- l5_valaskjalf (Jan Mayen): ledge 1512 (5057, -79645, -13429) to ledge 1510 on the same wall 1893 higher, the facing
  wall 822 away: the leap back and five wall jumps (~520 each), all the count allows.
- l2_towerroom: ledge 418 (308, 14792, 3645) to ledge 417, 553 higher: one kick off the wall 603 behind her.
- Found by openunderworld-work/gameplaygoal/chimney/scan.cpp (a ledge hang with an unflagged facing wall 300..950 behind
  it and the next ledge out of an up-jump's reach) but not climbed this way by ours (reached otherwise, or not at
  all; not looked into [U]): l2_cliffclimb 940, l2_towerroom 420, l4_temple 1157, l4_traprooms 1367, l5_valaskjalf
  1558 and 1515, l7_arctic_sea 2073, l7_yggdrasil_end 1860, l1_ship_exterior 365.

**Tests**: `Game.ChimneyJumpGainsHeightEachRebound` (walls 700 apart: ~487 a rebound, 110 out, no teleport),
`Game.ChimneyJumpNeedsThePress` (14 frames, then the fall), `Game.ChimneyJumpFallsAfterFiveInARow`,
`Game.ChimneyJumpTopsOutOntoLedge`, `Game.ChimneyJumpRefusesFlaggedWall`, `Game.ChimneyJumpNotFromAStandingJump`,
self-test `chimney_floor_jumps`,
`Game.ChimneyJumpSaveLoadContinuesIdentically`, `GameReal.JunglehubChimneyJumpClimbsColumns`,
`GameReal.ValaskjalfChimneyJumpTakesAllFive` (and l2_towerroom's kick), `StreamReal.SaveContinuesBitIdentically`
(its l2_junglehub case: saved on column B mid-climb, bit for bit).

**Measured in tru.exe under Wine** (2026-11-10, l2_junglehub, the streambed floor between the columns, facing column
A's unflagged east face; `openunderworld-work/gameplaygoal/reference/chimney-wine2.txt` ja / jb / jc,
`chimney3-wine.txt` j3 / j4, scripts `session6.sh`; docs/REFERENCE_COMPARISON.md M06) [O-runtime]:

- A standing jump 111 from the face: a plain jump (apex 201.6), three runs: no wall jump (q2's flag 0x80, above). Ours
  planted her 45 over the floor until this gate was added (`Game.ChimneyJumpNotFromAStandingJump`).
- A stick jump (q43: 40.00 + 49.97 a frame) run up to the face: one frame after the take-off she is in the wall
  state, her root **43.2 / 43.6** over the floor, and is drawn in 0.19..0.20 s (6 frames) to **98.5** out of ledge
  796's line (ours: 39.7 over the floor, 105.6 out, drawn in 2 frames [I: `kAttachSeconds`; the 3-frame clip entered
  at frame 1 does not explain tru.exe's 6]). Ours used to miss this catch when she took off within a tick's travel of
  the wall: the test read her contact-clipped velocity (self-test `chimney_floor_jumps`).
- No press: the idle state lasts **0.416 s** (12.5 frames; chart variable 208 = 12, so its unit is the 30 Hz frame
  [O]), then `wall_jump_detach_fall` takes 0.267 s (8 frames), her root 32.9 back and down to the floor.
- A press 58 ms into the idle: the turn state lasts **0.200 s** (6 frames; ours plays the 5-frame clip from its start
  frame: 0.133 s), then the flight starts 22.8 out and 37.0 up of the wall position at **46.81** out + **77.27** up a
  frame (first sample 74.48): q12's velocity, as ours.

The climb itself, ledge 796 to 797 (`chimney4-wine.txt` h1; the hang reached by pinning her 600 over its root, where
the fall's air grab takes the ledge) [O-runtime]:

- Hang root z 5855.5 (ours 5855.2). Back + Jump: she leaves 407 ms after the press at **53.73** out + **34.77** up a
  frame, the free leap's speeds, and is in the wall state on column B 0.149 s later, **130.6** over the hang's root
  (ours 138), drawn to her hold in 0.2 s.
- Each jump press: the 0.2 s turn, the flight at 46.81 + 77.27, and the wall state again **0.183..0.200 s** into the
  flight: on the 5th frame, the first with zVel inside the window (the timing inferred above holds), while she is
  still 105..115 short of her hold, to which she is drawn level in 0.2 s. Holds at +130.6 (B), +491.2 (A), +829.6
  (B): **360.6** and **338.4** a rebound. From the third hold the jump is re-aimed (first sample 36.32 out + 67.06 up
  a frame) onto ledge 797: hanging 1247.5 higher 3.17 s after the first press. Four jumps, as ours.
- **Ours differs**: its rays (3 frames x speed from the pelvis) reach the facing column only on the 8th frame, so she
  arrives at the wall itself and has risen **435** a rebound (holds at +138, +573, +1008;
  `GameReal.JunglehubChimneyJumpClimbsColumns` asserts that 435). What lets tru.exe take the wall from ~110 farther out
  is not decoded [U]: the vector at player state +0xA20 that 0x758DD0 scales may be the jump's launch velocity
  (90.3 a frame for q12, not the current 69.6), and 0x4935F0 may cast with a radius.
- No press after the leap back (h3): 0.200 s of draw, **0.404 s** idle, `wall_jump_detach_fall` 0.259 s (32.6 back,
  149.4 down), then the fall.
- Five in a row (h2: from the hang on 797, where the walls give no more height, a press every 150 ms): the leap back
  and **five** jumps back; on the wall after the fifth the presses do nothing, she holds 0.416 s, drops and catches
  797 again. The count is tru.exe's, as ours (`wallJumpMax`).

Open [U]: the look-ahead above; the camera on the wall (the jump camera is kept); l5_valaskjalf's climb that needs
all five; of the nine further shaft candidates above none was tried. Coordinates for such sessions: after
`wmem goto <unit>` tru.exe's world is our unit frame + (its start - our `player` intro): l4_ballcourt + (5881.21,
-10787.21, -15435.93), l2_junglehub + (8558.2, -19414.5, -18073.1); only the first `goto` of a session can be relied
on (docs/REFERENCE_COMPARISON.md "Method").

## Slopes and slides (src/game/Player.cpp `floorKind` / `standable` / State::Slide)

Whether Lara stands on a floor or slides down it is decided per face by its **collision client flags and its angle**,
not by one normal-z threshold (decoded 2026-11-10; before that ours stood from normal z 0.7 and slid below it).

### The original's rule [O]

- **Where**: tru.exe's ground check `0x5DCE20` (called for Lara from `0x71B910` every frame): a line down through her
  root, then for the face hit (terrain: `clientFlags` = face +0x0E; objects: flags of the object's collision, masks in
  brackets; both read through `0x5D3AF0`):
  - slide kind = 2 when the face has 0x0004 (object 0x200), 1 when it has 0x0040 (object 0x800), else 0; stored in
    the player globals +0x11D0;
  - L = cos(player data short +0x100E) = cos 52° = **0.6157**; U = cos(player data short +0x100C) = cos 28° =
    **0.8829** (U is computed once at spawn, `0x71B54A`, into the globals +0x644 and passed as the check's slope limit);
  - **slope slide**: kind != 0 and L <= normal z <= U (and < 1): message 0x1CA to the player, not grounded;
    +0x11CF = 1 (L < normal z), which with +0x11D0 >= 1 is the chart condition `PSCCF_OnSlopeSlide` (0x1FD,
    `0x763670`);
  - **too steep**: 0 < normal z < L on any face that is slide-flagged or lacks 0x0020 (object 0x400): message
    0x1C9, not grounded (she falls along it), unless the face has 0x0100 (object 0x20): then she stands;
  - everything else is stood on: unflagged faces from 0.6157 up, slide-flagged faces flatter than 28° (normal z >
    0.8829; a flagged flat floor is ordinary ground), 0x0020 faces at any angle.
- **Client flag census** (up-facing faces, `gameplaygoal/charvirtual/client_hist.cpp`): the values in use are 0x0000,
  0x0004, 0x0080, 0x00A0 and 0x01A0. 0x0004 is painted on purpose-built slides (l1_kraken_room's ramp into the lower
  hall: flat top, 44.6° face, 23° / 17.6° run-out, all flagged; l7_yggdrasil_hall01's slope before the eitr pit: one
  30.1° surface, normal z 0.865) and on rock faces (l2_junglehub: 120 x 10^6 units² up-facing). 0x0040, 0x0020
  alone and 0x0100 alone were not seen on terrain.
- **Chart** (lara.drm player state chart, `PSCCF_OnSlopeSlide` = cond 0x1FD):
  - every ground state leaves for the slide: T84 (and T83 with the stick downhill) -> S804 `slope_transitionto`
    (request 1491, 0.67 s), then T1701 -> S801 `slope_slidefront` (request 1490, loop); a landing on a slide face:
    T12 -> S802 `slope_landfront` (1488); a roll: T73 -> S801. So a slide face **cannot be walked or run up**: the
    first frame on it starts the slide.
  - during S804 (states S819 / S820): Jump -> `slope_jumpF` / `L` / `R` / `U` by the stick (T1704 / T1710 / T1711 /
    T1709, requests 1486 / 1484 / 1483 / 1485); the stick uphill (`PSCCF_MoveDirDownhill` 411) -> S807 `ng_stepback`
    (T1708), with a side steer also S817 (T1716).
  - sliding (S801 / S811 / S812, group S814): Jump -> S805 `slope_jumpF` (T1703); off the slide face onto ground
    (not OnSlopeSlide, not falling) -> S821 `slope_slide_stumble_off` (T1699, request 1492, 1.67 s); off it into the
    air -> S812 (the same loop, falling) or the ledge states (T1713 / T1715 when a jump target is attached).
  - `slope_slide_off_to_ledgeb` (request 1480) is played by no chart state: the slide-to-ledge-grab at a slide's end
    goes through the air states and the ordinary ledge catch [O: no state requests 1480; U: where 1480 is used].
- **Slide speed** (controller `0x73FF50`, props of group S814; lara.drm tuning data): target speed along the fall
  line (direction (n x z) x n of the floor normal, `0x7308B0`) = prop 0x204 .. 0x205 = **35 .. 45 units per 30 Hz
  frame** (1050 .. 1350 units/s) interpolated over the slope angle (mode prop 0x209 = 0x194); approached at prop
  0x1FE = 5 (speeding up) / 0x1FF = 10 (slowing) units per frame², both multiplied by the frame's time multiplier and
  applied as an acceleration: at 60 Hz 2.5 / 5 per frame² (2250 / 4500 units/s²) [O-runtime: +1.25 per 60 Hz tick];
  the rate depends on the frame rate in the original. The angle range: the controller's defaults are the player
  data's 28 / 52 (overridable by props 0x192 / 0x193, which no state of the group carries), but the measured steady
  speeds fit **29° .. 51°** (table below, within 0.03 units per frame over twelve faces) and not 28 .. 52 (up to 0.35
  off) [O-runtime; U: where the 29 / 51 come from]. Slide kind 1 scales speed and rate by player data floats +0x1010
  / +0x1014 (both 1.0). During `slope_transitionto` (group S818) the props are 1.0 / 1.0. Steering props of S814
  (0x1F5..0x1F8 = ±45 / ±135, 0x23B = 45, 0x259 = 140) do not enter the velocity: the controller always drives along
  the fall line [O]; what they turn (her facing) is [U].
- **Too-steep floors** are not this controller: she is in the falling physics on the surface, gravity along the
  slope without cap (Wine, l3_main_bedroom roof, normal z 0.45: 51 -> 69 -> 91 units per frame, g sin 63°).

### Ours

- `Player::floorKind(hit)` returns Stand / Slide / Steep by the rule above from `collision::Face::clientFlags`
  (terrain faces keep the raw +0x0E through unit merging and streaming; object faces count as unflagged [U: the
  object flag words are not loaded]); `Tuning::minFloorNormalZ` = cos 52°, `slideMaxNormalZ` = cos 28°.
- `standable`: Stand; a Steep hit still counts when a standable face lies within 20 units of its height 60 units to a
  side ([I] ours: floor bumps and narrow steep strips; the original has no such test but passes a 20-unit strip in
  one frame). A Slide face is never standable.
- State::Slide: on a Slide face the speed along the slope follows the controller (35..45 per frame over 29..51°, 2250 up
  / 4500 down units/s² at our 60 Hz tick). Entered from the ground `slope_transitionto` plays first and the target
  is 1 unit per frame while it does ([O] props of S818; [I] that the controller runs during it); a landing on a slide
  face stops her and `slope_landfront` plays at rest ([O-runtime] 0.40 s still, below); then the `slope_slidefront`
  loop. On a Steep floor g sin(theta) without cap as before (down to normal z 0.3 [I]; steeper is fallen past).
- A standable floor ends the slide through `slope_slide_stumble_off` (State::Land, root motion). Off the end into the
  air: an edge that carries a ledge is caught as by the safety drop (`trySafetyDrop`, chart T1713 -> S534 -> the hang
  attach states; [I] played with `slope_slide_off_to_ledgeb`), else a fall with the slide velocity.
- Jump during a slide: the slide's horizontal velocity + the standing jump's 1441 up [I]. The chart's
  `slope_jumpF` / `U` / `L` / `R` run jump queries q3 / q39 / q40 / q41 (prop 541; the slide itself q42), whose
  launch numbers are not decoded.
- Not done: `ng_stepback` at the entry with the stick uphill, the S817 side run, facing changes during the slide,
  the `slope_jump*` clips and queries, slide flags on object collision.

### Measured under Wine [O-runtime]

l2_junglehub, tru.exe = ours + (8558.23, -19414.44, -18073.17); Lara put 15 over a face and left alone, traced at
60 Hz (`pt trace`: position, velocity in units per 30 Hz frame, chart state); each sample looked up in our collision
mesh for the face under her. Traces and tools: `openunderworld-work/gameplaygoal/charvirtual/` (`slope-wine2.txt`, the
first series) and `openunderworld-work/gameplaygoal/slope/` (`session.sh` / `session2.sh`, `wine.txt` / `wine2.txt`,
`find_ramps2.cpp`, `analyse.py`).

| Face: client flags, normal z (angle) | tru.exe | Rule |
|---|---|---|
| 0x0000: 0.633, 0.647, 0.665, 0.763; 0x0080: 0.626, 0.659, 0.679, 0.703, 0.716; 0x00A0: 0.974, 1.000 | stands at rest (2.5 .. 3 s) | not slide-flagged, normal z >= 0.6157 |
| 0x0004: 0.933, 0.965 | stands at rest | slide-flagged but flatter than 28° (normal z > 0.8829) |
| 0x0004: 0.880 (28.4°), two faces | slides, steady **35.0** units per frame | slide (under 28° it would stand); speed clamped at the 29° end |
| 0x0004: 0.860 (30.7°) / 0.850 (31.8°) / 0.828 (34.1°) / 0.807 (36.2°) / 0.799 (37.0°) | slides, steady **35.8 / 36.3 / 37.3 / 38.3 / 38.6** | 35 + 10 (angle - 29) / 22 = 35.76 / 36.27 / 37.32 / 38.27 / 38.64 |
| 0x0004: 0.765 (40.1°) / 0.753 (41.1°) / 0.741 (42.2°) / 0.714 (44.4°) / 0.706 (45.1°) / 0.665 (48.3°) | slides, steady **40.0 / 40.5 / 41.0 / 42.0 / 42.3 / 43.8** | 40.05 / 40.52 / 41.00 / 42.00 / 42.31 / 43.77 |
| 0x0004 0.706: a landing on it | at rest 0.40 s in the landing state, then +1.25 per 60 Hz tick from 0 to 42.3, held | `slope_landfront`, then the controller |
| 0x0004: 0.388, 0.470, 0.595, 0.599 (reached from a slide above) | the slide state ends; she accelerates along it without cap (56 -> 140 units per frame) in the falling states | too steep (< 0.6157); the slide flag does not cap it |
| 0x0080: 0.45 (l3_main_bedroom roof), 0.245 .. 0.366 | falls along it, g sin(theta), no cap | too steep |
| the lower edge of the 0.706 face (a ledge) | off the edge she turns (7.5° a tick) while dropping and ends attached (a teleport does not move her until Crouch lets go) | slide end -> ledge catch |
| the lower edge of a 0.880 face (no ledge) | 0.1 s in a slide-off state, then the fall | slide end -> fall |

Not obtained: an unflagged face between normal z 0.60 and 0.6157 that she stayed on (the two tried dropped her onto
neighbours), 0x0020 / 0x0100 faces steeper than 52° (the 0x00A0 trials fell through terrain not yet solid in that
part of the unit; ours follows the decompiled test), and everything with keys during a slide: the jump off it
(`slope_jump*`, queries q3 / q39..q41), the stick across and against the slide, the entry from a walk
(`slope_transitionto`) and its `ng_stepback`. The keyed session is prepared
(`openunderworld-work/gameplaygoal/slope/session3.sh`: the 2.5 s steady slide over the 0.765 / 0.769 / 0.668 / 0.746
faces, Jump at 0.7 s, A / D / S / W held); tru.exe did not come up / did not load the unit in its two runs.

## Rail slides (src/game/Traverse.cpp)

- Type-8 markup attaches from the ground or air after ledges, poles, ladders, perches and rock faces. Lara follows the
  polyline downhill with tangential gravity; Space jumps with rail momentum, Ctrl/C drops, and the endpoint enters a
  normal fall with a short regrab cooldown. Attach tolerances, detach behavior and reuse of `slope_slidefront` are [I].
- `GameReal.ValaskjalfRailSlideTraversesAndDetaches` traverses all of rail 1562 and checks the low endpoint. A rendered
  `ou-game` run also landed at `7269.1,-76525.5,-13337.3` with `ng_generic_land_forwards` [O].

## Motorcycle (src/game/Vehicle.*)

Lara mounts the bike with E (interact) next to its left side, rides it with the move stick (forward = throttle, back =
brake, and reverse once it stands and the brake has been held 7 frames; left/right = steer; raw stick, not
camera-relative), hand-brakes with Space / pad South (the jump action: tru.exe's "Vehicle Brake", button 22) and gets off
with E once it is almost stopped. There is no hop. Throttle + hand brake revs a burnout and charges the launch: letting
the brake go with the throttle held shoves the bike off (the "fast start" / speed burst); add the stick (standing) for a
donut; hand brake or brake with the stick over above 15 mph slides the rear out (power slide).
While she is on it, `World::tick` skips `Player::tick`, drives her from the bike and masks move, jump, drop, use and the
grapple; the weapon keys stay live (see "Weapons on the bike" below); the camera pulls back and swings behind the moving
bike. No new bindable actions.

Evidence:

- Intros [V-data] (survey of every unit DRM against objectlist.txt): `motorcycle` (object 540) in `l4_trailstart` 5482
  at (38051, -51966, 1463) rot z 0.93, `l5_startsnow` 7528, `l5_valgrind_tunnel` 7229; `revisit_motorcycle_spawner`
  (657) in `l4_trailstart` 6699 and `l5_valgrind_tunnel` 6715; `co_motorcycle` (641, a cinematic copy) in
  `l4_ballcourt` 6403 and `l7_thin_ice` 9078. No Thailand (l2) unit places a bike. `UnitData::vehicles` keeps the
  `motorcycle` intros; spawners and cinematic copies are not instantiated [U: the spawner's script for revisits].
- Classes [V-exe] RTTI `Bike : Vehicle` (vtables 0x861974 / 0x861A18; 0x886064 / 0x8860E4 are their RTTI locators), `VehicleCamera`, `NtMotor_ScriptType`; state-chart
  conditions `PSCCF_Vehicle_SteerDirection/StuntDirection/IdlePose/DismountClearance/State`. 0x6AB960 creates the Bike
  (0x790 bytes, ctor 0x78EDA0) when the instance's vehicle data (+0x50 = ObjectBase +0xC8) has `+8 == 1`, with the tuning
  record `+0x10`; 0x793680 is its per-frame update (throttle, steering, gear/torque 0x791D10, Havok wheel forces 0x792900,
  fall damage).
- Vehicle data [V-data] motorcycle.drm ObjectBase +0xC8 (section 13582 +0x1160): `u16 +4 = 0xB00B`, class 0x40,
  `+8 = 1` (Bike), four 0x20C-byte tuning records at +0xB0 (count +0x6C), default `+0x10` = record 2; `+0x90` four
  wheel pointers (two set: front 0x1D18, rear 0x1CF0).
- Havok vehicle [V-exe] (0x6AB960, 0x795080, 0x7953F0, 0x792900, 0x791D10, 0x78F860): one rigid body from the model's
  HInfo primitives (0x497270; material-9 primitives, the two side capsules, get other flags [U meaning]), an upright
  limit of record +0x40 (42.5°, 0x795080), and per wheel a ray from its suspension top down `+0x00 + radius`; the hit
  makes a spring constraint (0x4E1A40): stiffness `m / wheels · ω²`, damping `2 m / wheels · +0xA8 · ω`, ω = record
  +0xA4 Hz × the wheel's +0x18; the wheel's forces act `+0x28` (0.7) × (top → contact) above the contact. A wheel
  definition (0x28 bytes): `+0x00` rest length, `+0x08` friction [I], `+0x0C` / `+0x10` brake / slide friction [I],
  `+0x14` engine share, `+0x18` spring factor, `+0x1C` flags (1 steered, 2 its sign), `+0x22` the model's wheel sphere
  (HInfo head[3]), `+0x23` the HInfo marker at the suspension top. The engine (0x791D10): rpm = Σ share · v / radius
  (rad per 30 Hz frame) × 286.48 (= 30 · 60 / 2π) × ratio, clamped to [+0x78, +0x80]; torque = the quadratic through
  (+0x78, +0x90), (+0x7C, +0x94), (+0x80, +0x98) N·m (× 0.3111 · 280 = 280² / 30² into game units), none above +0x80;
  reverse: +0xBC; the force on each driven wheel = share · torque · throttle · ratio / radius along the body's forward
  axis (0x792900). Ratio (0x78F860) = +0x80 / (286.48 · top speed / Σ share · radius) × gear / top gear: the top rpm at
  the top speed; the record has one gear (+0x70 = 1: ratio +0x54 6); reverse: speed +0x74, ratio +0x6C, so top rpm at
  10 mph. States (0x790E40): below +0x04 (2 mph) back reverses and no input brakes; back while rolling forward (throttle
  while rolling back) brakes; the brakes ramp in over +0x30 seconds (0x792900 states 3 / 5).
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
- Weapons on the bike [V-data]: `wp_handgun`/`wp_uzi` carry `combat_pistol_motorcycle_*` (arm 19 keys, disarm 25,
  reload 31, switch_hands 15, an aim grid straight/left_front_diag/left_side/left_back_diag/left_back × level/`_u`/`_d`)
  and lara.drm `hit_motorcycle_*` hit reactions. No other weapon DRM has bike clips.
  - Grid [O, measured]: every aim pose points the **right** arm (bones 78 → 80, model space) and keeps the left hand on
    the handlebar, despite the names: straight −2°, front_diag −47°, side −94°, back_diag −134°, back −189° (her right
    round to behind); rows `_u` +33°, `_d` −30° (means; `_left_front_diag_d` is a copy of `_u` in the data;
    `_straight_u` is a 40-key move into its pose, the others hold one pose). How the original aims to her left
    (`switch_hands`, mirroring?) [U].
  - ou-game (Combat.cpp): while riding (not mounting / dismounting) the pistols or the uzi can be drawn (F, or fire / lock
    draw as on foot); with another weapon ready, drawing takes the pistols and X cycles only pistols/uzi [I]. One gun,
    the right; the left stays holstered. The upper body plays the bike grid (each cell at its last key) over the riding
    clip, arm / disarm / reload use the bike clips' lengths [O]; refire, damage, spread, ammo, targeting (soft target
    in front, lock tiers) and the hitscan are the on-foot ones [I]. Aims to her left clamp to straight; behind-left
    counts as behind. Mount holsters at once; dismount plays disarm and she stays holstered [I]. Steering is untouched
    (the stick drives the bike; the adrenaline window cannot open: E is the dismount).
- Skeleton [V-data] 19 bones: front wheel 5 (axle (0, -206.6, 104.1)), rear wheel 8 ((0, 256.8, 98.8)), fork/bars 3-4,
  swingarm 7, frame 17. ou-game spins 5 and 8 about model X and turns 3 about the steering head axis [I].
- Collision: every l4 unit's faces use mask 3 like Lara's; no bike-only faces or markup were found [U how the original
  keeps the bike on its paths beyond terrain].
- Music: `l4_laras_new_bike` is named by script 13918 (in every l4 unit and `revisit_motorcycle`); ou-game starts it on
  the first mount in an l4 unit [I trigger].

Numbers (default tuning record, the wheel definitions, the model):

| Value | Ours | Source |
|---|---|---|
| top speed | 40 mph = 5008 units/s: the engine's top rpm, no torque above it | record +0x00 [V-data]; mph per 0x105B30C, 0x791D10 [V-exe] |
| engine | 30 / 20 / 20 N·m at 1000 / 10 000 / 18 000 rpm (quadratic), ratio 37.4 (engine / wheel), one gear; reverse 20 N·m, ratio 149.7 | +0x78..+0x98, +0x54, +0x6C, +0x70, +0x74, +0xBC [V-data]; formulas 0x791D10 / 0x78F860 [V-exe] |
| drive | front 0.4 / rear 0.6 of the torque, pushed along the body's forward axis at each wheel's force point | wheel +0x14 [V-data], 0x792900 [V-exe] |
| chassis | 265 kg (capsule 250 + side capsules 5 + 5 + seat sphere 5), centre of mass 196 units up; the wheel spheres (5 kg each, radius 103 / 97) are the wheels | HInfo [V-data]; massProps [V-exe] |
| wheels | front: top (0, −206, 258.9), rest 170, radius 103, steered; rear: top (0, 260, 220.6), rest 150, radius 97 | wheel defs + spheres + markers 30 / 31 [V-data] |
| suspension | 1.8 Hz × 1.5 = 2.7 Hz, damping ratio 0.35; forces 0.7 of the travel above the contact | +0xA4 / +0xA8 / wheel +0x18 / +0x28 [V-data], 0x792900 [V-exe]; one fixed point in Jolt [I] |
| tyres | Jolt's slip curves scaled to peak 1.5 front / 1.7 rear | wheel +0x08 [V-data]; its meaning [I] |
| brakes | front 5, rear 10 units/frame² per mass share → 783 / 1466 N·m, ramping in over 1.25 s; held at rest (below 2 mph) | wheel +0x0C, record +0x30 / +0x04 [V-data]; units [I] |
| tilt | the body's up stays within 42.5° of the world's | +0x40 [V-data], 0x795080 [V-exe] |
| lean | the roll servo targets (steer / wheel limit) × min(1, \|v\| / (0.675 × 40 mph)) × 30° into the turn with a wheel down, 0 in the air; 0.1 of the error per 30 Hz frame (3/s) | +0xB0 / +0xB8, 0x793680 (a motor with +0xB4 5000 N·m, target velocity 0.1 / frame × error) [V-exe]; units of the rate and its reference up (the exe blends the wheels' ground normals in above 0.4 steer) [I] |
| air control | no wheel down: f = (steer / limit) × v / 40 mph; 300 × f N·m about the body's up, 2600 × f N along its side, towards the stick | +0x44 / +0x48, 0x793680 (torque × 280²/30², force × 280/30² into game units) [V-exe]; signs [I] |
| hand brake | Space / pad South (the Vehicle Brake, button 22; under Wine Space puts the Bike in state 5 with flag bit 25, no hop): hand brake torque from wheel +0x10 (front 2.8, rear 0.5, as the brakes' units), biting over 0.5 s, no drive | +0x10 / +0x38 / +0x52 bit 2, 0x792900 [V-exe]; the key [O, Wine ref_hand.txt] |
| burnout (state 7) | throttle + hand brake, stick centred: no drive, hand brake on, the rear wheel spins up to 500 / 286.48 rad per frame (52 rad/s) over 0.2 s; Lara `motorcycle_accelerate_loop` | +0x52 bit 0, +0x34, +0x144, 0x790E40 / 0x793680 [V-exe]; the clip [I] |
| donut (state 10) | from a standstill (or a burnout) throttle + hand brake + stick past half: hand brake, 6750 N outward on the rear contact (ramped over 10 frames), lean 10° in; the front contact held horizontally, spin capped at 2.5 rad/s; `motorcycle_donut_left/right` | +0xEC / +0xF4 / +0x114, 0x790920 / 0x790E40 [V-exe]; the exe anchors it with a "Donut Anchor Body" (0x78F520, mass ramping to +0x13C 400 at +0x158 90 per frame, pivot ±190, 0, −400 in the body frame) [I: ours pins the front contact; the cap: Jolt's tyres let the push spin it several turns a second] |
| power slide (state 11) | above 15 mph brake or hand brake + stick past half: the brakes ramp over 4.25 s, the rear contact pushed 1524 N out / 1016 N back / 161 N down for 5 + 10 + 5 frames (in, hold, out), lean 15° in, spin capped at 2.5 rad/s; until the brake or stick is let go or below 2 mph; `motorcycle_power_skid_left/right` | +0xE0 / +0x1B8 / +0x1D4 / +0x1D8 / +0xC4 / +0x1E4 / +0x1EC / +0x1F4 / +0xE4, 0x7918B0 / 0x790E40 [V-exe]; push signs and the cap [I] |
| wheelie | not in the data: record +0x52 bit 3 (the launch wheelie after a burnout, +0x19C..+0x1A4) is clear | [V-data] |
| front wheel limit | 30° at low speed → 10° at top speed, blended over [0.3, 1] × top speed | +0x10 / +0x14 / +0x1C, 0x793680 [V-exe] |
| steering rate | 95°/s → 75°/s, same blend | +0x18 / +0x1B4, 0x793680 [V-exe] |
| fall damage | drop ≥ 3792.67: (drop−3792.67)/(4819.85−3792.67)·95; > 4819.85: 95 + clamp(…/(4977.88−4819.85))·1550; in the air ≥ 4977.88: 1650 (lethal), type 8; airborne = no wheel on the ground | +0x184..+0x198, 0x793680 [V-exe] |
| launch (speed burst) | throttle with the brake or hand brake held revs from 3000 to 18 000 rpm over 0.3 s (the charge); the brake let go with the throttle held drives 0.1 s with 10 × the torque read at the revved rpm × (charge / 0.3 s); the throttle let go first drops the charge | +0x88 / +0x8C / +0x1C4 / +0x1C0 / +0x0C, 0x793680 flag bits 23 / 22, 0x791D10 [V-exe]; Wine: state 7 flags 0x02800620 while revving, 0x00400600 at the release, 17.2 mph 0.13 s later |
| reverse | standing with only the brake held, the reverse gear takes after a 30 Hz frame counter passes 6 (7 frames, 0.233 s); a hold cut short keeps its count; top rpm at 10 mph | +0x1F8, 0x790E40 state 3 → 2 [V-exe]; Wine: state 2 0.25 s after S (0.14 s after an earlier brake to a stop), −10.3 .. −11 mph, also backwards up the 33° stairs |
| hop | none: Space is the Vehicle Brake | 0x790E40 has no such state; Wine ref_hand.txt [O] (ours had an [I] hop until 0.42) |
| gravity | 5.72443 units per 30 Hz frame² (Lara's) | [V-exe] Lara, [I] for the bike |
| terrain, ramps, jumps, walls | Jolt: wheel rays and the chassis on the resident units' collision and the object colliders | the original's Havok equivalents [O]; Jolt's contact model [I] |
| mount / dismount | E within 250 units of the mount spot; off below 200 units/s | [I]; Wine: see the table below |
| ramming | enemies touched at > 800 units/s take 0.5 health per unit/s, the bike loses 30 % of its velocity, one hit per contact (enemies are not bodies) | [I] |
| camera | 950 units back (700 on foot), yaw follows the heading at up to 3/s × min(1, v/1500) | [I] (`VehicleCamera` data not decoded) |

Simulation (docs/formats/PHYSICS.md "Vehicles"): the ridden bike is a Jolt vehicle in `physics::Scene` while it is in
its ride phase; `World::driveBike` turns the stick into throttle / brake / reverse / steer before `stepPhysics`,
`readBikes` takes the pose, speed and wheel contacts after it and sits Lara on it. A parked, mounting or dismounting
bike has no body and stands where it was left (sat on its two wheel contacts when it first loads and when a ride
starts).

Saves: `vehicles` (docs/SAVES.md) keeps every bike (unit, unit-frame position, heading, pitch, roll, speed, steer, wheel
spins, air state, brake time, pending kick, take-off / landing timers, launch charge / time / rpm, reverse counter, stunt + its time and direction, phase) and the ridden
one; the bike's Jolt body, wheels and contacts are in `objects.physics` (key `v<uid>`); Lara's state `vehicle` + clip
are in `player`. Lara's model is rolled with the bike (`PlayerState::roll`, `Player::modelMatrix`).

Tests: `VehicleReal.BikeIntrosTuningAndClips` (record, wheel definitions, engine ratio), `VehicleReal.TrailstartRideReplay`
(replay `tests/replays/bike_ride.txt`: mount, full throttle down the trail to about 4000 units/s, part throttle into
the left curve up the steps, brake; the Vehicle Brake holds it; dismount; no damage), `VehicleReal.RamsEnemyOnTheTrail`, `VehicleReal.SaveRestoreMidRide` (bit for bit), `VehicleReal.RideIndependentOfRenderRate`
(30 / 60 / 144 Hz), `VehicleReal.ShootsFromTheBike` (the replay with the pistols drawn and fire held: right-gun hits on
a dummy ahead on the trail, the curve still steered, holstered after the dismount); `VehicleReal.LeanFollowsSteeringAtSpeed`
(roll into the stick both ways, Lara rolled with it), `VehicleReal.AirControlTurnsMidJump` (steering while it flies off the tunnel's ramp over the pit yaws
the bike towards the stick, the same each run), `VehicleReal.DonutAndPowerSlide`; `Physics.VehicleDrivesBrakesAndSavesBitIdentically`.

Further tests (0.42): `VehicleReal.LaunchBoostAfterRevvingAgainstTheBrake`, `ReverseAfterTheBrakeIsHeld`,
`LevelGroundManoeuvresMatchTheOriginal` (the Wine numbers below with the same inputs on the tunnel floor) and
`FirstRideOverThePitByAutopilot`: the plan lines of self-test scenario `bike_ride` (autopilot `ride X Y [R] [THROTTLE]
[SPEED]`, raw stick) ride bike 5482 from the trail over the stair hump between the two pillars (stairs 33°, x 43000 →
44200, top z 1842), down the valley, into the stone tunnel (floor −686, x 60200 → 65500) and off its ramp (lip x 66100,
z −614) over the pit (to x 67700, 6000 deep) to checkpoint 6620: take-off 4985 units/s, 17 ticks without a wheel down,
down at x 67779, no damage.

### Original vs ours (tru.exe under Wine, 2026-11-10)

Tool `openunderworld-work/gameplaygoal/bike/bt.c` (`bt trace`: keys by SendInput + one line per change of the Bike:
+0x430 signed mph, +0x438 state, +0x4B0 flags, +0x414 wheels on the ground, Lara's instance; `bt ride`: closed-loop
W / A / D to waypoints). Coordinates: tru.exe = our l4_trailstart frame + (−47917.4, 43431.5, −8576.6). Traces in that
directory: `wine_a.txt` (chapter start, W 3 s), `ref_hand.txt`, `ref_launch.txt`, `ref_back.txt`, `ref_accel.txt`,
`ref_dismount.txt`, `ref_mount.txt`, `ref_turn.txt`, `ref_trail.txt` (session `session2.sh`); ours: `ours_level.txt`,
`oursA.txt`. After a level change the intro cinematic must end by itself before any key: only then is she riding (Bike
state 3; 0 = no rider). `onfoot/` holds traces taken too early (Lara on foot): not bike data.

| Manoeuvre | Original (measured) | Ours | |
|---|---|---|---|
| Standing start, level ground, full throttle | 7.4 mph after 0.33 s, 15.1 after 0.73 s, 19.7 after 1.02 s (stair-hump top; key latency ≤ 0.06 s) | 8.3 / 16.6 / 21.4 mph at the same times without latency (tunnel floor; asserted within 2.5 mph) | match |
| Chapter start, W held (first up 0.25 rad, then down the trail) | 3.7 / 9.4 / 13.9 / 20.7 / 28.0 / 31.9 mph after 0.28 / 0.66 / 1.06 / 1.46 / 1.86 / 2.06 s | 4.0 / 9.5 / 15.2 / 23.7 / 34.3 / 34.9 after 0.2 / 0.6 / 1.0 / 1.4 / 1.8 / 2.0 s | equal on the climb; ours gains 25–28 mph/s on the descent where the original gains 20 [U: drive only through wheels in contact, or the body's gravity] |
| Launch (Space + W 0.6 s, Space let go) | 0 mph while revving; 17.2 mph 0.13 s after the release, 22.2 after 0.52 s | 0 mph; 18.4 mph after 0.13 s, 24.9 after 0.5 s (asserted within 3 / 4 mph) | match |
| Reverse | gear takes 0.25 s after S (7 frames + latency); −11.0 mph 0.28 s later, −10.3 .. −11 steady, −12.5 backing up the stairs | 0.233 s; −10.5 mph on the level (`ReverseAfterTheBrakeIsHeld`) | match; the original reverses up the 33° stairs, ours untested |
| Vehicle Brake (Space) from 19.7 mph | state 5, flags 0x02000020; 0.3 mph lost in the first 0.13 s (the 0.5 s ramp), 13.2 → 1.8 mph in 0.37 s at the stair foot (0.24 rad uphill): about 2600 units/s² from the brake | from 21.4 mph on the tunnel floor: stopped in 2.48 s over 3735 units, about 1100 units/s² (1440 at full bite from the [I] unit: front 2.8 + rear 0.5 per mass share) | ours brakes about half as hard: the unit of wheel +0x0C / +0x10 is probably per wheel, not per mass share [U, one sloped sample: measure on the tunnel floor before changing `brakeScale`] |
| Dismount (E, standing) | Bike state 0 at once; Lara on her feet 1.56 s later 199 left of and 52 behind the bike's origin, turned 101° | same clip: 158 left, 52 behind, 90° | 41 units / 11° apart [U: the clip's end pose vs a settle] |
| Mount (E from that spot) | seated 1.16 s later, Bike state 3 after 1.26 s | reach 250 around (125 left, 180 behind) covers the spot; clip time the same clip's | spot rule [I] |
| Turning, W + A held on rough ground | yaw rate 1.68 rad/s at 5.9 mph, 0.92 rad/s at 22.4 mph (radius about 1600) | wheel limit 30° → 10° by speed [V-exe]: 22.6° at 22.4 mph = radius 1110 without slip | not compared on equal ground [U] |
| Stairs (33°) | climbs them at 24 mph with the throttle held (`wine_a.txt`), reverses up them at 10–12 mph | climbs with a run-up; stalls from a slow start | [U], same cause as the descent row |

Not measured (the session time went into the harness): top speed on a long straight, braking distance with S on level
ground, the pit jump in the original (air time, pitch), crash / wall rules, water depth, shooting, ramming. To continue:
`start.sh l4_trailstart session2.sh` (it waits for Bike state 3); ride with `bt ride` using more waypoints through the
stair hump (the closed loop circled at waypoint 2 of `ref_trail.txt`: it needs the passed-the-point rule the autopilot
`ride` has), stop inside the tunnel, then the level-floor runs of `session2.sh`.

Verification [O] (ou-game, Vulkan, 1280×720, the replay with `--spawn 37981,-51758,1463,-126.7`, invisible KWin;
screenshots viewed, kept privately): Jolt bike (2026-11-06) frame 200 Lara leaning into the left curve at the foot of
the steps (2952 units/s at tick 180), 270 in the jump clip going up the left side of the steps, the bike pitched up
on them; wheels and Lara stay on the bike. Older stand-in frames (2026-10): 40 mount, 420 dismount beside the bike.

Decoded but not built: after the throttle is let go the engine keeps pushing with a torque fading over record +0x1B0
(7 s; 0x793680 state 4) while every wheel gets a rolling friction (1 − throttle) × +0x1A8 (1.0, the brakes' unit;
0x792900): ours coasts freely. The pad's Gas / Reverse are the triggers in the original (buttons 23 / 24), ours the stick.

Not done [U/I]: gears beyond the record's one, engine/skid/landing sounds, the launch wheelie (off in the data), the
both-wheels-on-steep-ground torques of 0x793680 (+0x15C / +0x160 when two wheels hit slopes past the limit), crash
braces (a crash is only the simulation's), the jump dismounts, PDA on the bike, aiming to her left from the bike, hit
reactions while riding (damage applies without a clip), dismount clearance checks (`PSCCF_Vehicle_DismountClearance`),
the revisit spawner, water, the rear tyre's burnout smoke.

## Interactions: switches, push blocks, pickups (src/game/Objects.cpp `useSwitchClip`, Player `use` / `grip`)

Where Lara's clips are [O] (animation lists, 2026-11-05 survey of every objectlist.txt switch / lever / button / pickup /
door / block object; Lara's clips = 113-track animations on her skeleton):

- Switches carry Lara's clip in their own DRM next to the switch's: `obj_pull_lever` (anim 2605, 2.17 s; 2671, 2.43 s in
  `ar_ei_switch_oneway` / `sa_ei_switch_oneway_flushtunnel`), `obj_pull_fail` (2606 / 3263), `obj_pushbutton` (2441,
  1.30 s, 97 tracks; 2608 in the cheat buttons; `sh_ei_shiphall_door` names 2441 `hosted_push_button`), the rune wall
  switches `interact` (4129, 2.03 s). Animation ids are shared between DRMs; `ms_ei_switch_oneway_wall`,
  `ar_ei_switch_oneway`, `is_cheat_button` and `ar_uo_rune_wall_switch` hold all of them (`loadLaraAnims`: `use:<id>`).
  The switch's own clips pair with them (`active_pulling` 1.87 s with `obj_pull_lever`, `active_fail` with the fail).
- `ca_pt_phole_lever` (l4_ballcourt 6502), the push blocks (`ca_pu_block_a`, `ma_rb_pushblock_training`) and the doors have
  no Lara clips. lara.drm has the push-block set — requests 60 `obj_block_grab` (0.83 s), 61 `obj_block_idle`, 62 / 58 /
  1450 `obj_block_front/left/right/back` (1.5 s), 57 `obj_block_upper*`, 1452 `obj_block_release` (0.67 s), 99 `obj_no` —
  and the pickups, 49 `gear_weapon_pickup` (0.63 s), 98 `gear_weapon_pickup_crouch`. None of these clips has root motion.
- Engagement (tru.exe `[Engagement]` 0x6F3C60, called from 0x719620 for each instance's query 0x53): a list of 0x8C-byte
  records; +4 segment (−1 = the instance frame), +0x10 shape (0 sphere, 1 cylinder, 2 cylinder + facing cone 0x6F3A50),
  +0x14.. centre / radius / height in cm (× 3.1605 units), +0x28 cone heading and +0x2C half angle in degrees. The
  switches' list is at physical record (+0xC8) +0x70: +4 → records, +8 count (wall switch: cylinder r 175 h 200 centred
  100 up, heading 0, ±95°). Script message 0x1B1 (`UseEngagementProperties`) picks a set from object data +0xA8 / +0xAC.
  [I] record +0x44 = how far in front of the switch Lara stands (wall switches 135 units; her hand arc in
  `obj_pull_lever` centres 139 units ahead at the lever pivot height 427 = bone 1; cheat button 110.6); +0x48, +0x58,
  +0x60 [U].

Ours:

- Using a switch (src/script picks it as before): Lara's clip = the switch DRM's `obj_pull_lever`, else `obj_pushbutton`
  / `hosted_push_button`, else `interact` [I: by name; `obj_pull_fail` unused]. She is placed `useDistance` (record
  +0x44) in front of it on her side of its local horizontal axis she is most along, facing it, and plays the clip in
  `State::Use` with the input locked (move, jump, drop, interact, grapple, weapons); Idle at its end. The use handler
  still fires when the switch's own clip ends (src/script), inside her clip. Clip-less switches (pole lever 6502) play
  `obj_pushbutton` as she stands (position and heading kept) and fire at once [I].
- Push blocks: interact held within reach grips (`State::Block`, `obj_block_grab`, turned to the block's own horizontal
  axis closest to her facing [I: the block's faces; no grid data found]); the block moves only after the grab clip
  (where she cannot grip — swimming, in the air — the stick moves it at once, as before); pushing plays
  `obj_block_front`, pulling `obj_block_back`, standing `obj_block_idle` (looped) while the block's motion carries her
  (no platform carry from it meanwhile); letting go plays `obj_block_release` (State::Use). Uber objects pushed along a
  level prismatic joint (l7 Thor towers) use the same clips facing the joint axis; uber objects that are also script
  push blocks (`ca_pu_block_a`) are moved by src/script only (both moved them before).
- Pickups: taking an item or ammo plays `gear_weapon_pickup` (crouched: `_crouch`) where she stands, facing it [I].
- Inserting a portable (pole into a pole hole) [O tru.exe]: carrying a portable, Lara's chart action 0x4D (0x716F30)
  posts INSTANCE message 0x165 with her engaged object (Lara +0x854 +0xA0) to the portable she holds (+0x858). The
  portable (0x6B5890) posts itself the anim-graph stimulus 0x20 (message 0x33, value "any"), attaches to the
  receptacle by its 0x173 data (0x6B55D0) and posts 0x165 to it; the receptacle (0x6C2530) posts 0x20 to its own graph
  and calls a script prototype with (Lara's engagement record, 5) [U: which]. 0x186 is the removal (stimulus 0x40),
  0x17B the pick-up (stimulus 4, 0x6A6A20). The pole-hole levers' graphs (`ar_uo_phole_lever`, `ca_pt_phole_lever`)
  have one transition each way: 0x20 into the "inserted" state, 0x40 back; `ar_pt_pole_a` has no graph. The levers'
  joint marker (type 2: a cylindrical joint, 0x48F1D0 reads its degree of freedom 0 as a prismatic position) carries
  the hole; their range watches (`multibody` 79) are on that slider.
  Removal [O]: Lara's chart action 0x50 posts 0x186 to her engaged object; the portable (0x6B5890) posts 0x186 to its
  receptacle, takes stimulus 0x40 itself and is carried (as the pick-up: 0x6A6A20, 0x69AA50(1)). The receptacle
  (0x6C2530, 0x165 and 0x186 alike) calls its script's instance prototype 67 = vt 49 (0x799900) with (the segment of
  Lara's engagement record, 5) — l4_traprooms seal 5701's vt 49 tells its four keys apart by it (11 dark, 12 blades,
  13 fire, 14 crusher) — then posts 0x20 / 0x40 to its graph. Portables have no anim graph (their vt 19 / 20 do not
  run); the l2 / l4 poles' vt 57 therefore never runs from an insert.
  Engagement records [O data, 0x6F3C60 / 0x719520]: 0x8C bytes, sets {id, -> records, count} at physical record +0x68
  (anim-graph objects) / +0xA8 (portables, class 50); +0 kind (4 pick up, 5 insert into, 8 use), +4 the HInfo marker
  whose frame they are in (0x46DF20; -1 the instance), +0x10 shape (sphere / cylinder / cylinder + cone), +0x14 centre,
  +0x20 radius, +0x24 height (cm x 3.1605), +0x28 / +0x2C cone heading / half angle (degrees, from +X), +0x6C -> u16
  object ids / +0x70 count: holding a portable, a kind-5 record engages only if she holds one of those objects
  (0x6F3BB0). Every receptacle in the game (survey of all objects): `ar_uo_phole_lever` (ar_pt_pole_a), `ca_pt_phole_lever`
  (ca_pt_pole_a), `sa_pt_pole_hole_a` (sa_pt_pole_a), `ma_pt_pole_hole` (ma_pt_pole_a), `ca_pt_phole` (ca_pt_pole_b),
  `sa_pt_phole_track` (sa_pt_pole_maproom), `ca_pt_keyhole_xibalba_exit` (ca_pt_key_xibalba_exit),
  `ms_ani_eye_puzzle_piece` (ms_pt_seafloor_key_a), `ca_ani_traproom_seal_b` (the four room keys), `sa_pt_kalis_crown`
  (sa_pt_gem_kali) — anim-graph receptacles — and uber objects (below): `sa_uo_shivas_left/right_arms` (either gem,
  sa_pt_gem_left / right), `ca_uo_calendar(_b)` (section 0 ca_pt_key_headdress, section 5 ca_pt_key_mask),
  `ca_rb_empty_throne` (ca_pt_scepter_weight), `ar_pu_cog_wheel_level_1a` (sections 1-3, ar_pt_valgrind_power_gem),
  `ar_ani_statue_arms` (sections 1 / 2, ar_pt_thor_gear), `ar_ei_gemhole` (ar_pt_arcticsea_gem).
  `portable_key_preinsert` holders have a graph (0x40 out, 0x20 back) but no record: they only give.
  Uber receptacles [O tru.exe]: every one of them is an uber object (class 79), each section its own instance. A
  section answers query 0x53 (0x6D59C0) with its engagement set +0xD4: the section record's +0x40 at birth (0x6D60B0),
  then whatever its states' entry actions 0x1D set (UberObjectSection_TR8 0x6D5EC0; 0x1E sets +0xD8) — the arms, gem
  holes, statue arms and throne offer their kind-5 record only in their waiting state and swap in a kind-0 (dead) set
  once filled; calendar_b's mask section offers it only in state 3. The portable posts 0x165 to the section Lara
  engages; the section's handler (0x6D5AC0, its jump table 0x11C-0x186) ties the portable to it from the constraint
  record at section +0x4C (0x6B6440; its first byte is the model 0 marker, also query 0x173's attach point) and runs
  0x6D4890(1): an idle section (no transition, not removed) starts its state's first transition with trigger 20 and
  parameter 5, or trigger 22; 0x186 runs 0x6D4890(0): trigger 21 / 5 or 23 (no receptacle has one [O data]). No script
  slot is called (the receptacles' vt 49 runs only for class-6 objects, 0x6C2530, and Lara's uses, 0x701310: message
  0x158 = trigger 20 with the record's kind, e.g. calendar push handles kind 1); the transitions' uber events reach the
  script's vt 54: arm 2059 vt 58, 2060 vt 59 (-> 2063 vt 56), calendar 6506 / 5477 vt 54. The shiva arms take a gem
  only lowered (state 1: 2 -> 4 by their command 0, which their vt 55 sends when grapple handle 2066 / 2065's events
  run, then 15 s), the throne is also a press plate (`multibody` joint, ranges -150..-90 "in position" / -30..10) that
  the tied weight bears down on: its prismatic uber joint 0 (world → section 0, created in state 0) is that joint, so
  the plate's body poses the throne's model and collision (`World::uberModelJoint` skips plates; the unweighted uber
  joint used to hold the drawn throne up while only the plate sank [I: one Havok constraint in tru.exe]).
  `ar_pu_cog_wheel_level_1a` failed to load before (two unused transition slots -1 -> -1);
  `ca_uo_thor_statue_arms` (l4_pool) has no engagement set: not a receptacle. l7_arctic_sea's statue arms, gem holes and
  thor gears lie under water: Lara carries, inserts and takes out portables swimming too (records there are spheres
  of radius 632-948 [O data]; tru.exe's swim carry clips are not traced [I]).
  Push handles [O tru.exe]: kind-1 records (ca_uo_calendar(_b) section 1's set 528 at segments 10 / 20, section 0's
  set 540 once the headdress is in, the valgrind cog wheels' section 0): interact inside one puts Lara into her
  PushPull state, whose entry posts 0x158 to the engaged section (0x701310); 0x6D5AC0 case 0x158 (record in the
  section's current set, section idle) runs 0x6D4790: the transition with trigger 20 and parameter = the record's kind
  (calendar section 1: 3 -> 1); letting go posts 0x14F (0x701350 -> 0x6D4810, trigger 21: 1 -> 5, the ring's hinge
  locked, back to 3 after 1.5 s). Ours (`World::pushUberHandle`): the stick pushes / pulls the joint moving the section
  (following fixed joints: the handles are fixed to the ring, which turns on hinge joint 2) so that her grip point
  moves at the push-block speed [I], and she moves with it, facing along the push. [I] No PushPull proxy body
  (0x7358E0); kind 3 (also game-tracker message 0x1D) not used. larabase 35 (0x79A270 -> 0x6EE920, the calendars'
  vt 54) stores a float in player +0x1240 [U: its reader].
  Placed portables [O]: a portable's intro data starting with the instance reference 0x405 {u16 1, u16 uid} starts
  inserted there (message 0x23 -> 0x6A6DC0 -> 0x165): l2_puzzleroom pole 2967 -> hole 2969, l2_maproom 2665 -> track
  2664, l3_chapel 4823 -> 4824, l4_temple 5240 -> 5233, the gems and keys in their holders. The attach point is the
  receptacle model's marker named by physical record +0x3C (query 0x173 -> 0x6B55D0); the level data's placed pairs lie
  on it (pole 2967 within 7 units of hole 2969's marker 713.8 up). [O decomp + Wine] The portable's 0x165 handler
  (0x6B5890 -> 0x6B55D0, this = receptacle, args portable, marker, marker + 0x10) sets the portable's instance to the
  receptacle's instance frame (+0x20 position, +0x30 Euler) x the marker (position, XYZ Euler at +0x10): the
  portable's own model origin lands on the marker, no offset, the lever is not lowered first. Wine, l5_valaskjalf_entryb
  (insert forced with INSTANCE_Post 0x165): lever 7189 at (16965.07, -16856.20, -4083.15) rot (0, 0, 90°) -> pole 7190
  at (16965.07, -16825.34, -2913.89), settling to -2913.13 within 0.5 s and staying there: lever + (0, 30.86, 1169.3
  .. 1170.0), rotation the lever's (its +X, the pole's length, out of the wall). So the pole really hangs 4.2 m over
  the floor (1167 units). Standing jumps under it (four facings, and under its free end) rise 201.5 and never catch
  it [O Wine]. The route up to it is [U]: no ledge / hold markup lies within 6 m; wall lever 7188's handle marker is
  at the same 1169 height; `ar_pu_valaskjalf_entry` 7191 (448 x 448 x 413) is an uber puzzle object, not a push
  block [O Wine: interact + forward at its west face neither grabs nor moves it; Lara walks on through its instance
  position]. In tru.exe an insert from 250 in front of 7189 (dx 0)
  fails: the insert record's cone (heading 90°, half-angle 90° in the lever frame) admits only her standing on the
  lever's world -X side.
  Engagement [O tru.exe 0x719520, every frame]: Lara's query points are her position + z0, + step, + 2 x step, from
  the last active category-3 shape of her sensor list (player +0x854: +0x6C entries of 0x60, +0x51 active; 0x715DD0:
  z0 = shape +0x10 x 3.1605, step = shape +0x20 x 3.1605 / 2 for a type-1 shape, else one point). Wine (prologue and
  l4_traprooms, standing): {+0x10 -25, +0x14 100, +0x20 225} -> -79.01, 276.54 and 632.10 above her feet; two more
  category-3 shapes ({-125, 160, 250}, {150, 60, 90}) were inactive there [U: their states]. Per instance (query 0x53:
  an anim-graph object's set, a portable's +0xA8 sets, an uber section's current set) the first record of a nonzero
  kind that admits her (0x6F3C60: carrying, only kinds 3 / 5 / 7; a record naming objects only while she carries one
  of them [I: tru.exe also accepts one in her inventory, 0x6F3BB0]) with a point inside counts; between instances
  (0x704480) the lower |angle of the record frame's position off her facing| / pi + its horizontal distance^2 / the
  larger of the two wins. [I] Not modelled: the overlap list the instances come from (sensor +0x7C) and the line of
  sight check 0x6E7300 (kinds other than 5 and 8).
  Ours (`World::engaged`, `insertHeld`, `removeInserted`, `placePortables`, `tickInserted`): carrying, interact inserts
  into the receptacle she engages when its record is kind 5 and its graph takes 0x20 now (or it is an uber section).
  Empty-handed, interact on an inserted portable's kind-4 record takes it out; on a loose one's picks it up (portables
  without kind-4 records: within 150 / 100 [I]). A portable is told by its script's native base (`portablebase`) or by its physical class 50
  (`World::portableClass`: the four keys of the Xibalba trap houses, l4_traprooms 5878-5881, have a kind-4 record and a script whose
  native base is `instance` [O data]; before, Interact at them did nothing). Inserted portables sit at the attach marker; poles (longest axis >= 3 x the others) become horizontal
  poles; a receptacle whose script watches its joint slides down it under Lara hanging there (300 units/s to -600 [I]);
  `multibody` 78 on it answers that slide in units: GetJointValue (0x4A9BB0 -> 0x4A5D90) reads a cylindrical joint's
  (type 2) limit 1 as its linear position (0x4DA460, no conversion; limit 2 is its angle in degrees, a hinge's limit 1
  degrees) [O decomp]. l4_ballcourt 6502's loop passes twice it to door 6505's `multibody` 77 limit 1, both ends — on
  the door's own cylindrical joint (marker kind 2, its Z pointing down) a linear limit (0x4A5C40 takes limit 1 of type
  2 as is) — so the door rises twice the lever's drop (ours: kinematically, pinned at its limit, which starts 0..0)
  [O decomp; I: marker kind = joint type, the slide numbers].
  Receptacles get their graph also without a `modifiable` member or a script (l4_temple holes 5230-5232) and start empty
  when their start record is their insert target [I]; they have no use (l4_ballcourt lever 6502 now needs its pole).
  Uber receptacles: per section its current set (`UberSectionState::engagement`, from +0x40 and actions 0x1D; records in
  the section model's frame, moved with the section's joint), the same reach rules; the portable sits at the section's
  attach marker and the section starts its insert / removal transition (`World::uberInsert`). An inserted portable
  weighs on a press plate under it as a push block does (its primitives' mass). The uber events of the arms and
  calendars run their scripts; [U] what the cog wheels' and statue arms' filled sections open further (the cog wheels'
  and arms' later states wait for the other sections / their own joint watches; not traced to a gate).
  Saved (objects `inserted`, `insertedSections`, `slides`, `placed`; uber sections carry their set). Not done: the
  insert / remove clips (`hosted_insert` / `hosted_remove` in the portable DRMs, "Carried objects"), the lever's Havok slide numbers (no Wine measurement), l2_maproom's track slide, the
  uber transitions' condition check (0x4D0E10) on insert.

Test: `GameReal.LaraPlaysInteractionClips` (switch 4525: State::Use with `use:2605` at `useDistance` while the use
event fires; block 6504: grab, push clip, Lara moves with the block, release).
Not done [U/I]: the engagement cone / required items and the anim target's exact offsets, `obj_pull_fail` for locked
switches, sideways / upper block clips, a block grid (blocks move freely along the axis), buttress mounts (anim-graph
objects) keep their own handling.

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
+0x4C impulse; count +0xC8 of 0x14-byte entries at +0xCC `{clip ref, s32 min, s32 max, category, flag}` (min / max:
the ragdoll delay in counts, docs/formats/PHYSICS.md "Ragdolls"). FUN_00642210 takes the
attacker's angle relative to the heading: category 1 front (±45°), 3 left, 5 back, 7 right; falls back to front/back
by ±90°, then front; picks randomly among matching entries with the requested flag (FUN_00642170) and turns the heading
for 3/5/7 (a − 90°, a + 180°, a + 90°). Poacher: 3/7 `death_shot_3`, 5 `death_shot_back`, 1 a type-2 ref, flag 3
`death_placed_shot`, flag 7 `hosted_enemy_vault`. Panther: 5 `hitreact_thrown_forward`, 1 a type-2 ref, flag 3
`death_adren`, flag 5 `death_running`, flag 7 `hitreact_thrown_forward`. Bat: 1 `death`. After the timer the exe
hands the body to physics: the chunk object (+0x46, thralls; 0x696F90 budget check → impulse +0x4C via 0x696A60) or
its ragdoll (0x617EE0 → 0x60EF50 → 0x4F1920) [O]; both implemented (docs/formats/PHYSICS.md "Ragdolls").

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
4 s unseen [I] → search, or idle without a search record ("Behaviour trees, patrols and search"). Clips by role, first present name: idle `passive_idle`/`idle_passive`/`idle`, alert
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
  +0x70). Each shot is a ray from the muzzle [O] (its gun's shot origin, the wp_akrifle on the poacher's marker −75; eye
  height one radius ahead [I] for a type without one) at Lara's chest; [I] 35 % of shots
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
  clip (`death_static`, `death_shot`). Once the clip is done the body becomes a Jolt ragdoll (every enemy object with
  ragdoll data except sharks; docs/formats/PHYSICS.md "Ragdolls"), which falls off ledges and down stairs and freezes
  when it sleeps. Without ragdoll data the clip's last frame is held. Clips whose last frame has its median
  bone under a third of the bind pose's median count as lying (panther `death_static` 37 vs 306, `death_running` 47,
  `death_adren` 53; poacher `death_shot` 55 vs 473, `death_shot_3` 40, `death_shot_back` 69; not `death_placed_shot`
  178) [O measured]; others without a ragdoll tip onto their side over 0.5 s about the forward axis at the feet [I]
  (flyers do not).

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

Checkpoints and streaming: a death restores the checkpoint save ("Checkpoints and respawn"), so every enemy is back
as that save holds it (`World::restoreEnemies`): dead ones dead, the others where they were at that moment, generator
spawns included. `Enemies::checkpoint()` / `Enemies::reset()` (the dead-at-checkpoint set; the others back at their
placement with full health, generators re-armed) remain for a World without units. Generator spawns are stashed with
their generator's uid when their unit streams out and re-spawned when it streams back in.

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
  sees her). Patrol paths come from the intro data, not the scripts ("Behaviour trees, patrols and search" below;
  1650 walks nodes 128 126 108 114 117 131 ping-pong, inside its zone).
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
- Enemy behaviour index (enemybase 16/17, 0x62DE00 selects the n-th behaviour-list node of the enemy data) and 26 are
  kept for scripts but do not change our AI [U]; 22 selects a patrol path (below).

### Behaviour trees, patrols and search

Behaviour trees [O]: four EnemyBehaviorTree classes (RTTI; their one virtual each registers the tree as nested
`(name, priority, priority)` nodes through 0x672060, children between 0x66FF10 / 0x66FF20): Soldier 0x612030,
Creature 0x6126B0, CreatureVolumetric 0x612BD0, Thrall 0x612F80, picked by enemy data +0x0C. Soldier, top level:
Despawn 101, Spawn 100, Death 95, Stand 90, Nap 88, ReactToPlayerDeath 70, ScriptAttack 60, Investigate 30, Puppet,
ReactGrenadeAttach 84, PanicDive 83, ReactToOutsideNavigableRegion 90, Combat 20 {EmergencyMelee, EmergencyAttack,
MoveByCombat, CloseInCover {Cover, CloseCombat {Melee, Close, Sidestep, Circle, Flank}}, AtCover {DiveToCover,
StepIntoCover, Crouch}, RangedCombat {Reload, Attack, Close}, Cover, MoveToGuardZone, Flank, Reload, Search 25
{SearchStart, SearchToLastKnownPos, SearchIdle, SearchToPossiblePos}, CombatStepIdle}, NonCombat 1 {Patrol 15
{PatrolMove 4..14, PatrolLookTurn 3..13, PatrolAnim 2..12, PatrolIncr 1..11, PatrolAttack 20}, MoveToGuardZone 10,
Idle 1}. Creatures have no cover or ranged nodes and add Circle / MoveBy / JumpOver; CreatureVolumetric (bats,
sharks) has IdleParent {Idle, Circle}, MoveToIdlePosition 2 (back to the birth position instance +0xF0 when more than
94.8 away, 0x639600) before Patrol. Ours stays a native state machine (Enemy.cpp) following these priorities for the
parts below; the combat sub-trees are the earlier modes (chase, fire, cover).

Patrol paths [O] (intro +0x78 data, type 13 = enemy; tru.exe birth 0x63C7A0 → 0x63AC40 → 0x62F520): data +0x34 →
the unit's planner record (LEVELS.md "Enemy intro data"), +0x38 → path {byte +2 speed 1..4, u32 +4 count, +8 → u32
node indices}, at most 16 points (EnemyPatrolPath, AI +0x3780: points at +0x50, count +0x708); with +0x38 null and
+0x6C bit 31 every node of the record (0x62F2C0); +0x3C byte count of alternative paths at +0x40.. chosen by
enemybase 22 n (0x777010 → 0x63AC40; n < 1 the own one, n beyond the count: none). Order +0x88 (0x637870): 0 loop,
1 once (then done), 2 ping-pong (a one-point path is done), 3 a random other point no other enemy has reserved
(0x633F10 / 0x633F60), 4 a random unvisited point; start point 0, or for 3 / 4 a random free one (0x63A410). Survey:
280 paths (bats 54, small spiders 65, merc_generator_run 36, thralls 31, tarantulas 17, nagas 20, sharks 18, mercs 12,
cats 17, poachers 10); orders 1 (160), 3 (96), 2 (13), 0 (9), 4 (2); speeds 1 (81), 2 (35), 3 (11), 4 (153).
Patrol record [O] (behaviour-list node +0x48): +0x00 → MoveNav set (merc `idle_passive_to_passive_walk` +0x14,
`walk_forward_passive` +0x18; arrival s16 max(+0x1C0, +0x1C2) = 205, cats 316), +0x04 → weighted idle list (merc:
`idle_passive` 70, `idle_passive_2..5` 8 each, s16 pairs 90/120, 70/80, 70/80, 145/168, 140/150 counts), +0x0C
PatrolAnim count (merc 1, cats 0), +0x18 −1, +0x20 → the combat set. Weighted lists (0x60F450): count +0x0C,
0xC-byte entries at +0x10 {clip ref, u32 weight, s16, s16}, picked by weight.
Search record [O] (node +0xD8; 0x61E480 needs its +0x00 set): +0x00 → MoveNav set (merc `combat_walk_search`,
cats `run_forward_2`, nagas `creep_seach`, spiders `walk`), +0x04 SearchStart clip (merc `combat_idle_search`, cats
`sniff_corpse`, nagas / spiders none), +0x24 → SearchIdle list (merc `combat_idle_search` 70 60/120,
`idle_combat_stand` 30 30/60; cats / nagas / spiders none, so SearchIdle is skipped), s32 +0x18 (merc 1600, poacher
240, cats 75, nagas 240, spiders 0). Steps (0x61E570): 0 start, 1 to the last known position, 2 idle, 3 to a possible
position (navmesh query 0x60F810 / 0x630B90 from where the search began, its heading and a random side), then 2 and
3 again; a disturbance more than 1580 away restarts it (0x61E630).

Ours (`Mode::Patrol`, `Mode::Search`; `Enemy::patrolPaths`, `EnemyType::patrol` / `search`; saved): an idle ground
enemy with a path patrols (before returning to its guard zone): walks (the set's walk clip, root motion) to the point
pulled into zone 0 (0x61F990) until within the arrival distance, plays a weighted idle there for [I] s16 +8..+0xA
counts (30 Hz; none: goes straight on), takes the next point by the order; mode 3 skips points another enemy walks
to or idles at. Losing Lara (4 s unseen, [I]) a ground enemy with a search record searches: the start clip, walks to
where she was (pulled into its zone), idles, walks to a possible position, idles, ... until [I] +0x18 counts (merc
53 s, poacher 8 s, cats 2.5 s) have passed, then idle → its patrol (from the current point) or its zone; seeing her
again alerts it (bark). Searchers keep the combat senses, patrollers the default ones. Logged by ou-game as mode
changes `patrol` / `search`. Tests: `Ai.PatrolsAlertsSearchesAndReturnsToPost` (synthetic: ping-pong path, alert,
chase, search, back on the path), `AiReal.MercPatrolsSearchesAndReturns` (l1_ship_exterior: the decoded paths and
records, merc 1650 idles at 5 of its points in 60 s, searches after an alert and patrols again).
[I] / [U]: the idle counts' unit, the search time's meaning (ours: total length), possible positions (ours: 4 m
within ±90° of its heading by a straight walk onto floor, 4 tries), a point it cannot reach in 20 s is skipped.
The path byte +2 (was "speed") is the default point mode [O] (0x62F520 → AI path +0x48): a point's node +0x40 byte
when ≥ 1, else the path's; 2 / 3 select the patrol record's +0x10 / +0x14 attack (PatrolAttack 0x635920 / 0x61B260,
MeleeWindup), ≤ 3 also sets AI flag 0x20000000 (0x61B220, [U]). Ours: patrolling at a mode-2/3 point and seeing Lara
in reach ([I] attack range / eye range for 0x634B20) it attacks with that record without an alert. PatrolLookTurn
[O] (0x628730 / 0x628800): a point whose node +0x50 has bit 1 turns to the node's facing (cdc -Y of its frame) with
the set's +0x58 / +0x5C turn clips before PatrolAnim; no node of the surveyed units sets it. Flyers (bats, sharks)
and generator spawns (the generator intro's own type-13 data) now patrol too; flyers fly straight to the 3D point at
[I] half the chase speed.

### Navigation, flank, investigate, sidestep, taunt

Nav graph [O] (LEVELS.md "Nav graphs"): tru.exe links NavPower (`<bfx_replay NavPower_version>`, bfx::NavGraph); each
unit's Level +0x60 holds a version-8 resource of graphs per layer (l1_ship_exterior: layer 3, agent radius 100,
height 700, 398 convex areas, 710 shared edges; l4_ballcourt layers 4 (radius 250) and 3 (100); l2_norsehub 6 (250) and
1 (70)). Ours (`ai::NavMesh`, Nav.cpp; `Enemies::navFor`): the loaded units' graphs merged per layer (unit-name
order), each ground enemy on the smallest layer built for at least its radius [I: tru.exe reads AI +0x1C50]; A*
over the areas through their shared edges, funnel for the corners; chase, return, patrol, search, cover, flank and
investigate steer to the next corner every tick (no stored path: saves continue bit for bit). Off the mesh or
without a route: straight, as before. Flyers have none (NavPower VolumeGraph, bfx::Fish / Surfer3D, not decoded).

Flank [O] (EnemyBehavior_Flank 0x6365B0 / 0x639B20 / 0x62A180; position 0x637D10 → 0x632F80; cost 0x622670; side
0x62D930; Soldier / Creature / Thrall trees): behaviour-list node +0xCC, record R = +0x10 {+0 MoveNav set, +4 bit 0
Lara inside [min, max) (+0x20 / +0x24), +8 / +0xC / +0x10 / +0x14 weights travel / Lara distance / direction / outside
the node's cover cone, +0x18 max cost, +0x1C / +0x28 the side test's front range and degrees, s16 +0x2C close distance
with +0x30 cost, s16 +0x38 / +0x3A preferred travel / Lara distance, s16 +0x44 near distance with +0x34 cost};
candidates = planner nodes in a box of enemy data s16 +0x0E (merc 7901) around it, not reserved (0x61F5D0), inside a
guard zone (0x622C80); +2000 for one of its last 8 destinations within 640 (0x61F6B0); the cover cone (0x623610) is
the node frame's −Y: Lara farther than node +0x48 + 632.1, within node +0x44°, under the node +0x4C height line.
Every soldier, cat, naga, spider type reads the same record: weights 0, close / near cost 1e11 (1264 from Lara, 252
from itself), so the first free node ≥ 4 m from Lara and ≥ 0.8 m from it wins. Ours: a ranged ground soldier after a
burst without cover flanks ([I] every 600 counts = 20 s; ties: nearest first for the kd-tree order); runs there
(the set's clip is an anim request: run clip), [I] gives up after 8 s, then fights on. Tests `Ai.MercFlanksToANodeBesideLara`,
`AiReal.ShipNavGraphFlankAndInvestigate` (merc 1650 flanks, the save taken then continues identically).

Investigate [O] (0x61DA60 / 0x61DD10, InvestigateStart / Move / End 0x61DF40 / 0x62C070 / 0x61E320; node +0x4C
{+8 MoveNav set, +0xC / +0x10 clip refs (merc: anim requests), +0x18 4740.83}): message 0xBF (a disturbance;
0x6FFFC0 / 0x69E510 send it from Lara's position, broadcast 0x655280) more than 1580 from the current one
(0x611BA0) restarts it; it walks there (pulled into its zone at 0.8, 0x61F990). Ours: every Lara shot
(`World` → `Enemies::disturb`) reaches calm enemies (idle, patrol, return, search, investigate) within [I] +0x18;
[I] 40 s give-up; seeing Lara alerts. Test `Ai.InvestigatesAGunshot`.

Sidestep (dodge) [O] (0x677D70 / 0x678530 / 0x678B50 / 0x674320; node +0xE4 {+0 s32 step 300, +4 → MoveNav set,
+8 trigger 948, +0xC / +0x10 timer after a step / a failed try}): Lara closer than the trigger → a random side, the
step checked on the mesh. Ours: in a chase, the set's +0x34 / +0x38 clips (cats `strafe_left` / `strafe_right`);
soldiers' are anim requests, so mercs / poachers do not sidestep [U]. [I] timers in 30 Hz counts. Test
`Ai.SidestepsWhenLaraIsClose`.

Nav links (NavPower links; LEVELS.md "Nav links"; 176 in 22 units, none on the ship) [O]: bfx keeps them beside
the graphs, both ends snapped to areas of every layer. A path query (0x662EC0) builds its PathSpec {obstacle
blockage 0xFF, user-data match from node +0x134 flags, linkUsageFlags = node +0x14C u8[16] as bits when the
request's +0x50 has bits 2 and 4, else 0} and asks CreatePolylinePath (0x41FAD0). Usage: cat_panther / tiger /
panther_thrall bit 2, poacher / poacher_long_range / merc bit 1, small_albino_spider bit 0; thralls, nagas, yeti,
tarantula, bats, sharks none. Every link's flags are 0xFFFF, so any of those may use any link. The AI path
(0x647CA0, 32 points) marks a link segment kind 1 when its far end is higher, else 2 (kinds 5+ carry area user
flags); the follower gives link segments no walking length (0x649F50) [O]. How the game then runs the link is not
traced [U]: cats have jump_up_* / jump_down_* / jump_across_* / jump_over_* clips (start, inair_static, end) and
0x44-byte records with those refs in their enemy data block (down, up, over: start, −1, in-air, landing, −9, 94.8 /
363.5 / 379.3, ...), soldiers no jump clips at all. Ours (`NavMesh::addLink` / `route`, `Enemy::startLeap` /
`leapTick`): A* also crosses a link the type's usage allows (strips at the take-off point nearest to where it
walks from [I], cost = walk + flight × penalty [I]); corners are funnelled per stretch between links; reaching the
take-off ([I] within its radius + 60: ledge avoidance halts it a radius short) it faces the landing and plays the kind's start clip in place, flies a ballistic arc
([I] its gravity, top 158 above the higher end) with the in-air loop, lands with the end clip, and its mode carries
on (hits during a jump hurt but get no reaction [I]). Kind: up / down as 0x647CA0, across [I] within 158 of level.
Types without jump clips (soldiers) route without links [I: their link clips are anim requests or absent]. Saves
keep the jump (EnemyState leap*). Tests `Ai.NavMeshRoutesOverALink`, `Ai.CatJumpsDownALink`,
`AiReal.BallcourtPantherJumpsDownALink`.

Taunt [O]: only `panther_thrall` has a taunt record (node +0xC8, flag AI +0x3718 set when its Adrenaline behaviour
ends, 0x6116E0); its clip ref is {0, −1} and +0x10 is null, so Taunt_Anim (0x61F2D0 needs s16 +2 ≠ −1) and
Taunt_Turn never start: no enemy taunts.

Not done: JumpOver (EnemyBehavior JumpOver 0x62BC20 / 0x62B620 vaults over a cover node's low wall with the cover
record's +0x174 clip, not a nav link; not modelled), long-range specifics beyond their records, rifle
mesh, bat latch, the original's wall-walking transitions, thralls' chunks.

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

## Checkpoints and respawn

What tru.exe does [O, decomp]:

- **A checkpoint is a full save of the game.** The engine keeps two save buffers of 0x140004 bytes: the working one
  (`0xC59258`) and the committed one (`0xC5925C`). "Save Game" `0x5570F0(0)` buffers every live instance (`0x5568C0`
  -> `0x556680`: block types 2 script-saved, 3 full intro with the instance's own save data (message 0x17), 4 moved /
  rotated only, 5 flags only; type 1 = a stream unit's moved terrain groups `0x5563D0`), the six script variable
  tables (`0x555820`), the player's own save (vtable +0x5C) and the game state block type 6 (`0x555BE0`: version
  0xC2C, the player's level data 0x62 = current unit name + checkpoint id, the clock `0xE7F154`, outfit id, reverb,
  PDA map), then copies working -> committed.
- **What saves:** (a) the checkpoint object (`checkpoint` / `invis_checkpoint`) on message **0x196 / 1**, posted by
  the script native `checkpointbase` 3 (`NsCheckpointBase`, `0x776CB0`): the previous checkpoint gets 0x196 / 0, the
  unit name and intro id go into the player's level data (+0, +0x80), the checkpoint's own music / reverb settings
  apply (`0x698150`: data +0x78 -> `0x52E1B0`, `0x582B90`), the "activated" clip plays and `0x5570F0(0)` runs; there
  is no "already active" test, so every activation saves; (b) **collecting a reward** for the first time
  (`0x5DB6A0`, treasures and relics): it posts 0x196 / 1 to the *active* checkpoint again, so the game is saved as it
  is at the pickup; (c) the **level start**: `0x5D40C0` births Lara and, when no checkpoint id is stored, calls
  `0x5570F0`.
- **Death:** DeathState entry `0x75AA50` starts wipe 10 to black over **90 frames** (death type 0, 3, 4 and the
  default) or **210 frames** (type 1, which also zeroes her velocity [U: which deaths are type 1]); a negative
  request plays a scripted death clip with its own frame count. `0x75AF90` waits for the wipe, then `0x6F55A0` sets
  game flag 0x80000; messages 0x15 / 0x8A during the state reload at once [U: their senders]. The game loop
  (`0x5DAB40` case 6) calls "Restore Game" `0x557130(0)`: committed -> working, the script tables back
  (`0x5558A0`), `0x556E90` ("MAIN checkpoint restore %s ... ID wanted = %d": the clock, chapter, outfit, the ten
  inventory words `0xE7EE76..`, reverb), and the level loads again: every instance is born from its saved block,
  `0x5D40C0` puts Lara on the intro with the stored checkpoint id (`0x698180`), message 0x2A (`0x700060`) gives her
  full health.

So a death returns **the whole world to the last save point**: scripts, objects, enemies, pickups, weapons and
rounds, music / reverb and the clock, with Lara standing on the checkpoint at full health. Rewards stay collected
(they saved), and with them everything else done up to that pickup.

Ours (`World::respawn`, `World::serializeCheckpoint` / `restoreCheckpointSave`, Stream.cpp):

- The checkpoint save is an in-memory save body (`World::checkpointSave()`): the clock, `collected` / `born` /
  `found`, grenades, the light, the script runtime (with music and reverb requests), enemies, vehicles, weapons with
  clips / reserves / adrenaline, objects with the physics scene, hinge limits and scripted water levels. It is taken
  at the end of the tick in which a checkpoint was activated (the script event of `checkpointbase` 3, every time; or
  Lara within 250 / 400 units of a checkpoint intro not yet active [I]), in which a treasure or relic was collected
  with a checkpoint active, and at the end of a level's first tick (the level start). Never of a dead Lara.
- Death: the clip plays, the screen fades for 90 frames (`kDeathFade`), then `respawn()` when the next tick starts
  (between ticks, as the original's game loop does it; the restore replaces the units and the physics scene, and
  with the scene the object colliders and hinges of the world before it, `World::restoreObjects`): the clock goes back, the
  checkpoint's unit loads afresh (`enterUnit`), the save is restored, Lara stands on the checkpoint intro (or the
  start spawn) with full health, full air and empty hands (the weapon state of her hands - draw, lock, reload,
  cooldowns - is not restored; her secondary, clips, reserves and meter are). The camera starts behind her. ou-game
  stops every sound and resumes the save's music and reverb as after a load (`World::respawns`; the original's level
  reload stops all sound, `0x5DAB40` -> `0x593D10`).
- [I] / differences: the PDA map's revealed points are kept (the original's save block holds its map); a bike she
  was riding at the checkpoint comes back at its saved place with Lara on foot beside it; the 210-frame wipe and
  the scripted death requests are not used (no death of ours asks for them).
- Saves: the checkpoint save travels inside a save game (`checkpointSave`, format version 3, docs/SAVES.md), so a
  loaded game dies back to the same world as the game it was saved from.
- Tests: `Stream.DeathRestoresTheCheckpointSave` (synthetic: collected / found / grenades / light / weapons / clock,
  twice, after a load, and with no checkpoint), `StreamReal.DeathRestoresTheCheckpointSave` (l3_chapel: switch 4819's
  pillars 4837 and the shotgun's rounds come back, the switch works again, a save game carries it, a treasure
  re-saves), `Save.MigratesVersion2`, `Combat.RespawnKeepsTheLoadout`, self-test `checkpoint_respawn` (l4_pool by
  ordinary input: rounds fired, a grenade thrown and the torch put out after checkpoint 5502 are back after the
  fall).

## Loadout

- [O] A level start (player message 0x23, `0x71FFE0` -> `0x7198D0`) gives the unit's loadout list (level settings
  `+0x2B4`; the table under "Collectibles": wp_handgun + one secondary per area, the gadgets, from l5 Thor's gear,
  from l6 Mjolnir) and then `0x6F57F0(1)`: each of the six ammunition slots gets its **start amount** (Lara data
  u16 `+0x848` / `+0x85C` / `+0x852` + 2 x slot for AMMUNITION CAPACITY 0 / 1 / 2, `0x6EE930`) and its maximum
  (`+0x868` / `+0x87C` / `+0x872`, `0x6EEAE0`). The data's start table **equals its maximum table**: uzi 400 / 240 /
  160, shotgun 90 / 72 / 42, rifle 300 / 210 / 120, tranquilizer 25 / 15 / 10, speargun 40 / 25 / 15
  (`CombatReal.WeaponData`), so every level starts full. Grenades: `g_grenade_thrower` 10 / 6 / 4. No list holds
  `g_healthpack`: she starts each level without packs (cap 100).
- [O] **The weapon chosen before a level.** `0xE7F204` holds an object id set by script native `0x77BD60` ->
  `0x5D9F70(name)` (beside `0x77BD50`, which sets the outfit); a level started from the menu (`0x77E450` ->
  `0x5DE9C0`) sets player `+0x7F0`, and `0x7198D0` then gives the list **without its last weapon and the chosen one
  instead**. A level reached by playing on (`+0x7F0` clear) gets the list as it is. The other branch of message 0x23
  (game state `+0x390` set) rebuilds the slots from a save and rescales the rounds to the capacity level
  (`0x715EF0`); native `0x777F50` = `0x6F57F0(0)` refills keeping each slot's fill fraction (`0x6F56A0`).
  [U] which script calls `0x77BD60` and what its screen looks like: locals.bin has no text for a weapon page other
  than the PDA's WEAPON SELECTION / WEAPONS (1076, 1100); only g_pda imports the neighbouring costume natives.
- [O] After a death the rounds, grenades and packs are the checkpoint save's ("Checkpoints and respawn").
- Ours: `World::chapterLoadout(chapter, chosen)` (called by `startChapter`): every secondary a full clip [I] and
  `WeaponDef::startReserve` at the capacity level, holstered; the secondary is `chapterSecondary` - shotgun
  (prologue and Croft Manor: their units share one list), speargun, uzi, shotgun, tranquilizer, uzi, rifle,
  speargun - or the weapon chosen. The choice is offered where ours starts a level from a menu: the PDA's REVISIT
  LOCATION lists the chapters, then the secondaries under WEAPONS (`ui::Screen::RevisitWeapon`,
  `Menus::revisitWeapon`) [I: the page's look]. In the level the PDA's WEAPON SELECTION still equips any of the five
  (observed in the original after a fresh l4 load).
- Tests: `Combat.LevelStartLoadout`, `CombatReal.WeaponData` (the start table), `Menus.MainCentreTreasureHuntAndRevisit`,
  `Pickups.HealthPacksCapAndLevelStart`.

## Player tailoring

- [O] Game state `0xC59264`: `+0x3EF94` the preset last picked, `+0x3EF98` "EnemyHP", `+0x3EF9C` "AmmoAdj" (copied
  to `+0x3EFA0` at a level load), `+0x3EFA4` "PlayerHP", `+0x3EFA8` "SavingGrabAdj", `+0x3EFAC` a fifth level, bytes
  `+0x3EFB0` (GAME OPTIONS toggle beside it [I: "Meta Icons" / helper buttons, locals 369]) and `+0x3EFB1`
  "TrainingText". Levels 0..2, 0 the lenient end; **defaults 1** and both toggles on (`0x6E7500`, run by the save
  reset `0x556940`); a setter refuses values above 2.
- [O] Script natives `uiexperienceadjustmentmenu` (registration `0x7AEDE0`): 0..3 the menu screen itself, 4 set
  training text, 5 set `+0x3EFB0`, 6 EnemyHP, 7 AmmoAdj, 8 PlayerHP, 9 SavingGrabAdj, 10 the fifth, 11..17 read them
  in the same order, 18 reads the preset. Unit scripts read 17 and compare it with 2.
- [O] What each changes: enemy health = the enemy's health block [level] (`0x6E77B0`); ammunition capacity = the
  start / maximum tables above and the grenade maximum 10 / 6 / 4 (`0x6E7760`); Lara's health = max health x
  generalbank difficulty `+0x08` (1.3 / 1.0 / 0.8, `0x6E7960`), keeping her health fraction; saving grabs = the
  window difficulty `+0x14` 90 / 50 / 15 frames [I: no exe reader of the level or of the table column was found; the
  level is offered to scripts by native 16]; the fifth level has no exe reader either [U: what the unit scripts do
  when native 17 returns 2]. Sidekick hints are not a tailoring level: FIELD ASSISTANCE is a PDA entry.
- Ours: `game::Tailoring` (`World::setTailoring`, saved per game); the scripts read the real levels and toggles
  (`script::Runtime::Experience`; until 0.43 every native read 0, the lenient level) and a script that sets one sets
  the World's. Config `game.tailoring` = [enemy health, ammunition capacity, damage to Lara, saving grabs], each
  0..2, default [1, 1, 1, 1]: the levels a game starts with; START NEW GAME's preset and GAME OPTIONS > PLAYER
  TAILORING write the player's choice back, a loaded save brings its own without changing the config.
  `game.training_text` / `game.helper_buttons` are the two toggles.
- Tests: `Tailoring.PresetsScaleHealthAndCapacity`, `Tailoring.ScriptsAndConfigSeeTheLevels`, `Combat.LevelStartLoadout`.

## Combat (src/game/Combat.*)

Health [O] (player globals `DAT_00fadb58`: health +0x628, regen clock +0x630/+0x634, invulnerability +0x638, last
damage type +0x63C):
- Max health = Lara data +0x44 (1000) × generalbank difficulty table (GlobalInfo = generalbank object data, +0x24:
  three 0x18-byte entries) `+0x08` at the PLAYER TAILORING "DAMAGE TO LARA" level (1.3 / 1.0 / 0.8, 0x6E7960); a level
  change keeps the health fraction (0x6F5160). The damage multiplier `+0x0C` (0.5 / 1.0 / 1.3) is read only at entry 1
  (`0x5D9E50` = (int) 1.0f at 0x8AB648), so it is always 1.0 [O].
- PLAYER TAILORING [O] (`game::Tailoring`, `World::setTailoring`, saved per game): levels 0..2 in game state
  `DAT_00c59264` +0x3EF98 enemy health, +0x3EF9C ammunition capacity (copied to +0x3EFA0 at level load), +0x3EFA4 Lara's
  health, +0x3EFA8 saving grabs, +0x3EFAC a fifth level read only by scripts; `uiexperienceadjustmentmenu` natives
  6..10 set them ("Player tailoring"); a preset (generalbank
  script 132 fn 4, `SetPlayerTailoringLevel`) sets all five to 0 CASUAL EXPLORER / 1 TOMB RAIDER / 2 MASTER SURVIVALIST.
  Enemy health = the enemy's health block [level] (0x6E77B0, ≤ 0 → [1]). Capacity: Lara data u16 columns +0x868 / +0x87C /
  +0x872 per ammo slot (0x6EEAE0: uzi 400/240/160, shotgun 90/72/42, speargun 40/25/15), start amounts +0x848 / +0x85C /
  +0x852 (0x6EE930: the same numbers, "Loadout"), grenades g_grenade_thrower +0xE6 / +0xE4 / +0xE2 = 10 / 6 / 4 (0x6E7760).
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
  [O]: g_healthpack.drm's `use` (1.03 s) animates the pack object, so nothing plays on her. Max carried 100 (+0xE2..E6),
  +1 per pickup (+0xE8) and the empty start of every level: "Collectibles".
- Death: at health ≤ 0 on the ground, `death_fall_normal` (fall) or `death_toknee` (other damage) plays [I]; DeathState
  (0x75AA50) fades the screen to black over 90 frames (wipe 10; `World::screenFade`) and respawns when it is black
  (0x75AF90 -> 0x6F55A0 checkpoint reload, 3 s) [O]. The respawn restores the checkpoint save: "Checkpoints and respawn".
- Hit reactions: the exe turns an accepted enemy hit (message 0x11C → 0x719DD0 → 0x6F5030) into a reaction by type
  (0x6EB590: type 1 light → state value +0x204, type 2 heavy → +0x208) and writes the attacker's angle from Lara's heading
  into the anim graph (0x6EB250; mirror flag 0x20 when positive), which picks the clip; the graph's thresholds are
  [U]. Ours (`World::damageLara` → `Player::hitReact`, state `Hit`, saved as "hit"): only when `Health::damage`
  accepted the hit and Lara is on the ground (idle/walk/run/stop/turn/land/hit) or swimming; light = `hit_light_{front,
  back,side}`, heavy = `hit_medium{front,back,right}` (lara.drm, 0.33 / 0.70 s [O]); front within ±45° of her heading,
  back beyond ±135°, else side [I]; no mirroring: the side / right clip plays for both sides; swimming the `*_swim`
  variants. The clip plays once with its root motion (ground: collide-and-slide, water: the swim sphere), then idle /
  tread or submerged idle. Guns stay drawn and fire during it [I]. The light clips are overlays [O]: they key only 12-23
  of 113 segments (spine, head, arm twists; not the arms), as offsets that end at 0 (like `combat_pistol_fire_overlay_*`,
  `face_overlay_*`), so as a full pose they left Lara in her bind T-pose (shark bites while swimming). They play as an
  additive layer on the pose under them (`AnimController`, their change from their own last key [I]).

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
  shotgun grid's columns / rows [I: not measured separately]; shot origins from weapon data S+0x14 (docs/formats/EFFECTS.md).
- Tranquilizer darts (`Hit::tranq`): animals with `sleep_start` / `sleep_loop` / `sleep_end` (cats; nagas'
  `tranq_sleep_start` / `_loop`, waking with `rest_to_idle` [I]; tru.exe EnemyBehavior_Nap / EnemyState_Nap) add the 500 dose instead of damage and fall asleep once the dose reaches their
  health (`ai::Mode::Sleep`, saved): sleep_start, sleep_loop for 20 s [I], sleep_end, then alert. Other enemies take the
  dose as damage [I]. Panther (700): two darts.
- Chapter loadout (`chapterSecondary`, `World::chapterLoadout` on a chapter start and for saves without combat state)
  [O object deps of every pc-w\l<N>_*.drm unit: all load wp_handgun + wp_uzi; l1 / l7 wp_speargun, l3 wp_shotgun, l4
  wp_tranq, l6 wp_assault_rifle, l2 / l5 / l7_himalaya_end nothing else; no unit script names a wp_* but
  wp_thorhammer]: Mediterranean / Arctic speargun, Thailand / Jan Mayen / epilogue uzi, Croft Manor shotgun, Mexico
  tranquilizer, Andaman Sea rifle; the prologue the shotgun (Croft Manor's list). Every secondary starts the chapter
  with a full clip and its start amount = the maximum ("Loadout") [V: the original's WEAPON SELECTION after a fresh l4
  load lists all five full, the tranquilizer equipped].
- PDA WEAPON SELECTION's EQUIP (`World::equipWeapon`) makes the row's weapon the secondary with the rounds it has (no
  refill); in hand, Lara switches as with Change Weapon.
- Saves: `combat` = weapon in hand, secondary, per weapon clips + reserve, adrenaline (`World::serializeCombat`); a load
  holsters.

- Max reserve: Lara data u16 tables at +0x868 / +0x872 / +0x87C (slot × 2; uzi, shotgun, rifle, tranq, spear =
  400/90/300/25/40, 160/42/120/10/15, 240/72/210/15/25) picked by `0x3efa0` = 0 / 2 / other (0x6EEAE0) [O]; we use
  +0x87C, taking `other` (1) to be Normal [I]. Live counts sit at `fadb58+0x51C` + 2·slot [O]. Reloads move rounds
  from the reserve into each gun's clip; the pistols' reserve is infinite; a secondary out of rounds and reserve falls
  back to the pistols [I].
- Projectiles [O] (pr_spear, pr_tranquilizerdart; mode 2, 0x6C95F0 → 0x6C9300, update 0x6C91D0, hits 0x6C9670): launch
  speed weapon S+0x10 (1000 / 1500), gravity record +0x4C (−2 / −1; the +0x84 record −6 / −3), per 30 Hz frame [I unit];
  damage record +0x1C (spear 250, +0x84 record 550; dart 500 dose); every hit posts class 4, type 0. `World::tickProjectiles`
  sweeps each tick's segment as a ray; past the max range it is dropped [I].
- Spread: per pellet, yaw and pitch offsets within ±spread/2 (0x69E300) from a 32-bit LCG (deterministic replays);
  grows per shot to the max, recovers per frame, resets on draw (0x6E6C60).
- Switching (`Actions::weapon`: X, pad d-pad right [I U+180A per locals 1298]; script `weapon`): pistols ↔ the secondary
  (exe current / previous weapon at inventory +0x134 / +0x138 [O] = `CombatState::weapon` / `previous`); the weapon in
  hand plays its unequip clip, then the other its equip clip; each keeps its own clip and reserve. In the water every
  weapon can be drawn and fired ("Water moves": no under-water flag in the weapon data, every wp_* has the submerged
  and surface handling clips) [I].
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
between holster and hand half way through [I]. In hand [O]: weapon data +0xB8 / +0xBA name Lara's grip markers, 7
(right, segment 96) / 5 (left, 71; two-handed weapons in the left hand), with the marker's frame. Stowed [O]:
weapon data +0xB9 / +0xBB name Lara's HInfo markers (lara model, `AnimLibrary::markers`) and the model takes the
marker's frame: pistols thighs 6 / 4 (segments 9 / 4), uzis on her back 36 / 37, every two-handed weapon 15 (chest
segment 15, across her back), Mjolnir 31. The secondary is shown stowed while it is not in hand.

Targeting [O data, I use] (Lara data +0x480 / +0x470, criteria records {u16 type, u16 mode, f threshold, axis}):
hard lock (aim held) tries tiers in order: nearest within 1264 (3D), then the smallest camera angle among targets
within 8000 and ≤ 90° from the camera axis, then within 8000 and ≤ 90° from Lara's facing; soft target (drawn,
aim not held): nearest within 5000 (2D) and 27.5° of her facing. LOS from 375 above the feet (+0x48C) is required.
Switch target cycles through the hard-lock order [I]. Locked and standing, Lara turns toward the target [I]. No
target: free aim along the camera ray.

Adrenaline [O numbers: Lara data +0x2D0 max 500, +0x2D4 / +0x2D8 drain 20 / 225 per s, +0x31C mode scale 0.5, +0x2C8
head-shot scale 0.2, +0x2CC ramp 10 frames; gain = the health her hit took, message 0xD9 from 0x63AD50; mode toggle
0x718BD0 (ours: interact with a lock [I]), head shot in the mode when close (ours) [I]. The text below predates them]: locals 1310 "Build up adrenaline for combat by damaging enemies", 1333–1335 "When
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
- Turning the orbit camera (look input; 0x5FFB00 flags the frame, 0x600200 then leaves the input out of the proposed
  point): once the camera has followed its point, 0x600710 turns that point about the pivot at its distance (limits
  0x608700), brings it in with the plain pull-in 0x5F0810 only (no swing) and puts the camera there at once. So a
  turn goes round whatever is in the way, pulled in, never swung back. [O] Wine, l2_cliffclimb yacht deck
  (ms_uo_yacht has oflags bit 2, the camera collides with it): full circles, 845..1130 from the pivot across the cabin
  side, 1264.2 elsewhere (StreamReal.YachtCameraTurnsRoundTheCabin; before: the swing turned it back every tick and
  the camera stood still).
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

## Vibration and camera shake (src/game/Feedback.*)

Both are output only: the simulation steps them each 60 Hz tick but never reads them, so the VIBRATION option, the pad
and the render rate change nothing the game decides.

- [O] Rumble manager (0xA02BD0, ticked by 0x4817B0): 16 slots of `pc-w\padshock\padshocklib.tfb` sequences (32: hit,
  landing, weapon fire, bike, loops; format in Feedback.hpp) plus two direct "shocks". A sequence's channel n drives
  motor n (XInput's 2 motors both take either channel kind, 0x481030): linear keys `clamp(lerp * scale + offset, 0, 1)`,
  step keys the last key's value, times the play scale; the slot frees after a channel's last key; looping sequences
  (+0x4C == 1) wrap between their loop points until released (0x483130). Shocks: 0x481CC0 motor 0 fully on / off,
  0x481CF0 motor 1 at level / 255, each for duration / 4096 frames of 30 Hz. Per motor the sum, clamped to 1, times the
  Vibration flag (DAT_008904C4, registry "Vibration", default on) -> XInputSetState(v * 65535), motor 0 the left (low
  frequency): ours SDL_RumbleGamepad low / high, renewed every 0.1 s for 0.25 s, 0 in every menu.
- [O] Sources: effect records (0x547B40) 0xD padshock by name {u16 player-only, char*, f32 rate, f32 scale} (931
  records: Lara's hits, ledge / pole attaches, landings, pole and weight puzzles), 0xE shock 0 {on, frames} (175),
  0xF shock 1 {level, frames} (322: shotgun / Mjolnir hits, bridges, backdrafts), 0x10 shock 1 by distance
  `min(a1, (8000 - d) * a1 / 8000 + 50)` (11) [I: d from Lara; tru.exe 0x453EE0's point]; records flagged player-only
  run only for the player or an instance parented to her (ours: Lara's clips and her shots). Script `padshock`
  (NtPadshock, bind 0x7ACD10) on {name, f32 library, rate, scale | on, duration 0, level 1, duration 1}: 1 Play
  ("shock" both shocks, "shock0" / "shock1" one, else the sequence at rate / scale), 4 / 5 Stop (release / now),
  6 IsPlaying, 7 IsLooping, 8 SetRate, 9 SetScale, 0 same member; 2 / 3 return a sequence object [U: not modelled,
  3 still plays].
- [O] Camera shake (8 slots 0xE80020): `camerashakes.drm`'s 20 DTP records (Feedback.hpp: 7 channels of a 128-byte
  envelope, amplitude, period, randomness %; duration, falloff radius, positional flag). Channel value
  `amplitude * envelope(t / duration) * sin(2 pi t / period) * (1 - r + 2 r u)` (0x5F2F80 / 0x5F3080: translation x 3.1605
  units, rotation and FOV in degrees), stepped by timeMult / 30 s (0x5F3390), summed onto the camera in its local axes
  (x right, y down, z forward; 0x5EEA10) and the FOV (0x5E2150, clamped 0.1 .. 3.0416 rad). Script `camerashake`
  (bind 0x7ABFC0) on the shake's DTP id: 0 Start(seconds) (0x5EE8C0), 1 Stop (0x5E8750). Ours: on the rendered camera
  only, interpolated between the last two ticks [I: euler order z, y, x; u from our own generator, tru.exe's global
  one at 0x8A9C78].
- [U] Not done: camera shakes by key from animation events (Lara 0x716F30 case 0xB, enemies 0x6258D0; camerashakes'
  key table: 20 entries {DTP id, duration, key}) and the cinematic camera's (0x5E21A0 command 7) — no record carrying
  them was found in the event lists; the positional falloff (0x5EB460) only applies to those. Rumble and shakes are not
  saved (a loop running at a save stops at load).

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
  from the lower floor (`PoolRockHurdle`) pulls her up onto the rock top past ledge 1253 (`hurdle_ground_attach_mid_150`
  → `hurdle_onto`, hands within 25 across the switch, z 3571 ± 1, end snap < 10).
  `CliffclimbObstaclesAndLedgeGrabs` (l2_cliffclimb shore plateau): walking into the 169-high step 941 hurdles onto it;
  a standing jump at the 474-high block 939 pulls her up onto it (z 3638 ± 5); the 614-high wall 911 is caught from a
  standing stick jump and from a running jump into its face, then climbed (z 3807 ± 5, end snap < 10).
  `PoolSwimReplay`: `findWater` gives one box (id 5608, surface 2699.3 ± 0.5); from the west walkway (−6000, 0, 2844.4)
  Lara runs into the goo, swims east 2 s (> 80 % of 2 s × swim speed), dives below surface − 600, swims up to tread,
  swims back west and climbs out (720 ticks) onto a pool rock at (−4190, −183, 2864.9), standing on its floor.
- Traversal, real data: `TempleBalanceBeamWalkAndJump` (l4_temple beam 1079: on it from a spawn on the beam, 5 s walk
  within ±1 of the line, a standing jump lands back on it 300+ ahead, then walks to its far end x −3986 and stops);
  `TempleLedgeJumpsUpAndBack` (from the floor: grab 1187, jump up to 1162; grab 1158, jump back to 1159 turning round;
  hands on the targets ± 1); `PoolSideJump` (1250 → 1251, set up hanging via `World::restore`); `PoolDropToHang` (onto
  the 1253 rock top, drop back into the hang, hands at 3562 ± 1); `TrapRoomsPoleFling` (l4_traprooms: standing jump onto
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
- hanging braced on rock ledge 1253 with both hands on its top edge (`--spawn -4291,4225.6,3100,-157.9`, jump; since
  the pull-up onto 340..650 ledges that jump takes her onto the rock instead);
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
- Jump timing differs from the original [O-runtime, docs/REFERENCE_COMPARISON.md]: tru.exe leaves the ground 342..359 ms
  after the press standing, 273..290 ms with the stick, 70..109 ms running; ours 133 / 67 / 33 ms, because the take-off
  leaves when its clip ends and `clipBlend` starts `ng_standing_jump` at request frame 6 and
  `ng_running_jump_forwards` at frame 1. The run start (`ng_run_start` from frame 6) leads tru.exe's by 0.07..0.10 s
  for the same reason. Not fixed yet: every recorded replay with a jump shifts with it.
- The original has no longer jump from a sprint and none from holding Jump on flat ground: a jump out of the sprint
  and one with Jump held 1 s are the running jump's flight (65.83 forward + 42.53 up a frame, 987 long, apex 158.0)
  [O-runtime, `openunderworld-work/jumpfid/sprint.txt`, `hold_run.txt`]; ours does the same.
- Swimming: breath, scuba, dodges, swan dive, surface turns, the automatic climb out and jellyfish are in ("Water
  moves"); its "Open" line lists what is left. Scripted water levels run ("Scripted levels"). Ripple/caustic
  parameters are ours [I].
- Free climbing is implemented on the levels' free-climb holds (markup 16-20, see "Free climbing"): hold to hold
  with the transition clips' root motion and DICE jumps up / left / right to far holds; hold-type semantics [U], hold
  choice/reach from lara.drm's queries and the player state chart, the sweep geometry [I] (2D), no
  bad-attach/wrestle clips. Ladders (9, rung to rung), perches (13, turning), pillars (15, onto top perches), poles
  (parallel hang, hand over hand, turns, phased swing stops), wall hug on shelves and rail slides (8) work (see their
  sections); ropes, vertical poles and zip lines do not exist in TRU ("Ladders, pillars, perches, wall hug"). Still
  missing: the wall-hug swim climb-out and jump-landing attach, pole-to-perch / pole-to-ledge (`hpole_para_to_perch`, `hpole_perch_*`),
  beam sprint, the beam drops (`bbeam_drop_*`), `ledgeu_*` free hang, vaults over / down (hurdles onto low ledges are done),
  the bad-attach / wrestle clips of poles, ladders and pillars. Root motion of the jump, attach and fling clips is not applied (the flight is
  ballistic), so their poses can sit off the computed path.
- Grapple is implemented for ceiling/post/wall targets (throw, pendulum swing, rope ascend/descend, jump off;
  see "Grapple"). Not done: wall-running/rappel, pulling mounted objects, rope-vs-world collision, the hook
  model, ledge-grapple transitions, exe rappel speeds (ours are [I]).
- Missing: sprint on stairs / slopes (`ng_sprint_up` / `_down`), beam sprint,
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
(`Runtime::playerJumped`; else the 7652 warp crosses kill plane 7650); so does the tick in which a climb-up clip is
seated on its ledge (`Player::takeReseat` [I]: the whole pull-up clip sets her position 148 lower, which read as a
fall through l3_bedroom_hall's kill plane 4379 under ledge 969). Water boxes keep their half extents and
rotation when a unit is offset (only min / max / centre move): l7_arctic_sea's boxes failed at its offset before.
Stowed weapons are placed from the data as the original (speargun across her back in ch1, the rifle in ch6; weapon
data +0xB9 names Lara's stow marker, "Combat").

**Pickups** (the reward table, urn treasures, relic and health-pack rules: "Collectibles") [O names, roles from the
reward table]: `*_rw_artifact` = treasures lying in the world, `*_rw_treasure` (one per chapter) = relics, `*_rw_shard` = health packs (base script 3924 pushes "g_healthpack" and uses the health/invslot
natives). All share base script 3674 (playersensor, "popUpTitleMessageChannel", hint_14). Ours: interact press within
200 units horizontally / 250 vertically of the reward intro picks up the nearest one [I]; it joins `World::collected`,
`World::found` counts it, the mesh is hidden and the HUD shows `TREASURES|RELICS|HEALTH PACKS` (1251/1252/1274) with the
count [I format]. Saved (docs/SAVES.md).

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
top (cell top + 4.52·scale, cap height 14·scale). Button placeholders in locals text (U+1800 + i, TEXT.md) become the
bound control of tru.exe virtual button `0x8A9EF8[i]` (0x5A7C20; U+182A/182B/182C = the keys Return/Escape/BackSpace)
between two 0x1B: on the keyboard its key name, drawn in the highlight 0x7D6932 that 0x1B toggles (0x476D80; on screen
(99, 209, 247) [O captures]); on the pad its Xbox 360 name (5061..5076: an icon, "Start Button", "Back Button"). Icons
(U+1800..18AF but 1820/1821) draw white with the text's alpha (0x476540). ou-game names the device used last
(`game::promptNames`, `ui::expandPrompts`); unbound controls read "<Unknown>" (0x851C68) [O].
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
- EXTRAS OUTFITS (ours, `ui::kCostumes`): a sixth category at ring position 7 under the CHARACTER icon 44, label "OUTFITS"
  [I: the data has no outfit category, icon or label]. Rows DEFAULT, then the outfits by their locals names 74 Casual,
  70 Wetsuit, 67 Jungle Shorts, 69 Jungle Heavy, 72 Snow Heavy, 68 Jungle Pants, 71 Drysuit Light, 73 Snow Light,
  75 Bathing Suit (470 LOCKED until unlocked; the one worn white); the preview is the outfit's character concept (295
  casual, 297 heavy jungle, 298 heavy snow, 303 wetsuits, 301 Lara Outfits Suite otherwise); prompt 120 SELECT. Return
  wears it at once, in play too; DEFAULT gives each level its own outfit back. See "Outfits" for the unlocks.
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
  BUTTONS (stored only), TRAINING TEXT (the tutorial popups, below), GAMEPAD / CONTROL
  CONFIGURATION (their screens: "Controls" above), VIEW AXIS INVERSION (ON/OFF
  inline, mouse/stick Y [I: the original opens a 501–504 sub-screen, not captured]), PLAYER TAILORING; LANGUAGE (the
  install languages by their names 110..119, right-aligned like ON / OFF; Left / Right / Return step; applies at once:
  TEXT.md §0), SUBTITLES ON / OFF, VOLUME SETTINGS, BRIGHTNESS (inline slider, graphics brightness 0.5·2^(v/5) [I: the
  original opens a screen, not captured]), DISPLAY MODE SELECTION, DISPLAY SETTINGS; SFX / IN-GAME MUSIC / CINEMATIC
  VOLUME (mixer buses; cutscene audio).
  Changed settings are written back to the `--config` file (launcher config: `game` volumes/subtitles/reticles/helper
  buttons/training text/language, `controls.invert_y`, `graphics.*`) and read at start.
- Tutorial popups (training text, locals.bin 1276..1369): unit scripts show them, not the exe. Trigger volumes'
  OnFirstIn call the unit script's hint handler with `"hint_NN"` (the same slot as help on demand, AUDIO.md §9.5); for a
  tutorial condition it runs `uipopupmgr` 8 (Hide), `uilocalstrproperty` 1 on one of its members (the member's initial
  value is the string id) and `uipopupmgr` 0 Show(text, condition, 10.0) (thunks 0x7A3F30 / 0x7A4040 -> 0x4CDEE0 /
  0x4D03E0), each once behind a stickybool [V-data bytecode, V-exe thunks]. 31 script types in generalbank and 16 units
  hold such members (l2_cliffclimb 9502 vt 25: hint_15 -> 1326, volume 4040). ou-game: `script::Runtime::takePopups` ->
  `ui::Hud::hint`, shown 10 s while GAME OPTIONS > TRAINING TEXT is on [I: the exe's popup screen script reads it through
  the TrainingText native 0x77BAF0, not traced]. Layout [I, not captured]: centred at the top (virtual y 56), the
  toast's font at 0.55 on a shaded box, wrapped at 56 characters, button placeholders named as in menus.
- Graphics (`ui::GraphicsOptions`, `Menus::graphics`) [I: the original's PC display screens were not captured; same
  list art under the AUDIO/VISUAL icon, original strings where one fits (1016.. DISPLAY / SETTINGS / MODE SELECTION,
  VSYNC, ANTI ALIASING, SHADOWS, REFLECTIONS, WATER EFFECTS, ON / OFF), the rest uppercase text]: the launcher's
  Graphics settings, applied live by the app (no restart). DISPLAY MODE SELECTION: RENDERER, RESOLUTION, FULLSCREEN,
  VSYNC, FIELD OF VIEW (30–120, step 5). DISPLAY SETTINGS opens QUALITY (PRESET, ANTI ALIASING, SHADOWS, ORIGINAL
  SHADERS, PARTICLE EFFECTS, TONE MAPPING ORIGINAL / AGX), RAY TRACING (mode, RT QUALITY, LOCAL LIGHT SHADOWS, REFLECTIONS, AMBIENT OCCLUSION, SUN
  BOUNCE; tri-state rows DEFAULT / ON / OFF), WATER & SKY (WATER EFFECTS, WATER DETAIL, ATMOSPHERE, SUN SHAFTS,
  WEATHER) and UPSCALING (AMD FSR, FSR QUALITY, SHARPNESS slider, FRAME GENERATION, AMD ANTI-LAG). Values sit centred over the slider
  track with its arrows on the selected row; Left/Right step and stop at the ends, Return steps forward and wraps.
  Rows that cannot apply are dim and skipped: RT rows with preset ORIGINAL or no ray-query device, the RT detail rows
  with RAY TRACING OFF, the FSR rows with FSR off, AMD FSR without FSR support, AMD ANTI-LAG without VK_AMD_anti_lag. PRESET ORIGINAL resets an explicit RAY
  TRACING SHADOWS / FULL to AUTO and FSR off clears FRAME GENERATION, so the settings always resolve.
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
voice lines and menu sounds play on. New Game, Continue, Load, quick load, Treasure Hunt, Quit to Menu and a level end's
chapter change stop every in-world sound, the reverb and the music first (0x593D10); a load then plays the scripts' last
music request and the reverb of the volume Lara stands in again, saved with the game (AUDIO.md §9.11) [I].

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

**Music** (docs/formats/AUDIO.md §5, §9.9): tru.exe's music player, 8 slots that each hold one track
(`pc-w\music\<name>\<name>.mul` with its `.mus` sequencer setting the stem gains every tick, clocked by the stream's
played samples) and fade on their own: a script crossfade is a Play in one slot with its fade-in and a Stop of another
with its fade-out (pool_to_ragnarok tick 101: 2 s in / 3 s out, both audible); a new track in the same slot waits for
the old one's fade-out; a request for the playing track never restarts it. The unit's `<unit>_mus_01` plays (slot 0)
when Lara enters a unit and no slot holds a track [I naming], and scripts request tracks when Lara enters a volume whose
intro script names one
(184 `soundvolume`, 24 `soundtriggervolume` intros; soundvolume derives from triggervolumebase, soundvolume.drm 3485 →
3516 → 3515 [O]). [O] `--chapter 1 --music-wav`: tick 1 l1_seatop_mus_01 (unit), tick 100 l1_seafloor_mus_01 (Lara fell
into l1_seafloor), tick 105 l1_seatop_mus_01 (volume 927), tick 181 l4_pool_mus_01 after a quickload; 0.5 s RMS
0.084 → 0.043 across the crossfade, then 0.08–0.11 (WAV in openunderworld-work/screens/progression, not listened to).

Verified [O] (2026-10-28, GL + Vulkan, screenshots viewed): `medsea_loop.bik` behind the radial main menu, Options
opened by replay, no gameplay HUD, and no Vulkan validation errors. `menu_new_game.txt` with `--no-movies` reached
CROFT MANOR and Lara walking. Earlier [O] (2026-10-18, GL): `menu_new_game.txt` → explode.bik
(45 frames) → "CROFT MANOR" card, Lara walking in l3_main_bedroom; `pool_pickup.txt` in l4_pool with `--spawn
-6045,-6100,2844,-90` → treasure 5489, "TREASURES 1".

## Manual aim, adrenaline inputs, dual targets, time scales (decoded 2026-11)

Sources: generalbank DTP 0x2F (input action → virtual button map, GlobalInfo +0x1C), the player state chart, tru.exe.

- **Manual aim** [O]: virtual button 9 (Z / right-stick click; config action `manual_aim`). Action 25 → chart T55 enters
  S718 when Target Lock is not held; T1887 / T1886 leave on Manual Aim / Target Lock. Camera mode 23 = mode-list entry 20
  (distance 632.11, focus offset z 158.03, zones 126.42 / 3.16 / 252.84, speed 632.11; its command 11 record 35 is not
  modelled [U]). Shots follow the reticle ray (`World::shotRay`, `manualAimPoint`; src/game/Aim.cpp). [I]: she may only
  walk, turns to the view when standing; a reveal / spline camera or a hit reaction ends the aim view. The lock-on framing camera was not
  found in the mode list [U].
- **Focused fire** [O]: in combat the same button (action 37, group 17) toggles adrenaline mode (0x718BD0 → 0x7189C0).
  The adrenaline ramps use the global scale 0xE7F1D4, so Lara slows with the world; no extra fire-rate change exists.
- **Head shot** [O]: Grapple button (action 46 → chart T1404, condition 0x21A = flag 0x400000). The meter latches at
  full (0x6F28C0 / 0x6E6630) and does not drain while latched (0x6F2930); the move (0x71D4F0) needs a target answering
  query 0x165 and spends the meter. It is a paired clip hosted in the enemy DRM (`hosted_enemy_aa_<weapon>`; enemy list
  node +0x54: distance 316.06, blend 6 frames); clip events 5 / 4 open and close the window (0x715020 / 0x718EA0, time
  0.2 and back). No grapple rope is involved. [I]: close range = `targeting.closeRange`; Lara is snapped, not blended,
  to the distance; the clip plays in place; pip start 2.5 radii, look 10 radii/rad, stick 2 radii/s.
- **Dual targets** [O]: 0x71E490 / 0x7035F0 (weapon controller +0xC / +0x10, soft set Lara data +0x470, hold +0x286 =
  1000 frames). Ours: the left gun fires at the second target; the arm pose is not split [I]. The PC original has no
  change-target key.
- **Time scales** [O]: `gamebase` 24 / 2 ramp / set the global scale (record 0x8ABE94), 23 / 1 the world scale
  (0xE7F1A0, record 0x8ABEB4, divided out of Lara's update at 0x622418); ease 0x78C120. `World::tick` splits real / Lara
  / world dt. Real script: docs/formats/SCRIPTS.md "Time scale"
  (`ProgressionReal.KrakenTentacleSlowMotionByItsScript`).
- **Firing cadence** [O]: every fire clip carries event 0x75 on frame 0, so "shot at clip start, cooldown = clip
  length" equals the original's event-driven shot.
- **One-handed shooting** [I mapping]: hang / shimmy (`ledgeb_`), free climb (`DICE_`), ladder, pole hang, pillar use the
  `combat_pistol_<set>_1haim*` sets of wp_handgun / wp_uzi only (other weapons have none).
- **Shot-only shapes**: anim-graph objects with a damage (stimulus 0x800) transition, a solid prim, no collider and
  ObjectBase oflags bit 0x40 [I] take bullets only (`World::shotOnlyShapes`); `instance` 23 anchors them to a marker of
  another object (`script::Runtime::anchor`, saved).
- Tests: `Combat.ManualAimHitsWhatTheReticleCovers`, `Combat.DualTargetsTakeOneGunEach`, `Combat.AdrenalineHeadShot`,
  `Combat.HeadShotConditionsMissAndInterrupt`, `Combat.AdrenalineModeDrainsAndSlowsTime`,
  `Combat.TimeScaleSlowsLaraWithTheWorld`, `Combat.ScriptTimeRampEasesInAndOut`,
  `ProgressionReal.KrakenChainLinksByManualAim`, `ProgressionReal.KrakenLinkEarlyShot`,
  `ProgressionReal.KrakenTentacleSlowMotionByItsScript`; on a real panther in l4_pool:
  `CombatReal.PoolLedgeHangShootsPantherOneHanded` (ledge 1253, right-hand gun only, she keeps hanging),
  `CombatReal.PoolHeadShotOnPanther` (clip `cat_panther:hosted_enemy_aa_handguns`, window 0.67..1.67 s, `death_adren`),
  `CombatReal.PoolManualAimHitsPanther`. Self-tests: `tests/selftests/manual_aim.json`, `adrenaline.json`,
  `kraken_kill.json`. Research aid:
  `OU_UBER_DUMP="unit:uid" … ProgressionReal.DumpUber`.
- **Switch reach** [I]: a switch is used within 200 units of its origin or of the point she stands at to use it
  (`useDistance` along its local axes), whichever is nearer — l1_kraken_room's wall switch 30 keeps her 217 from its
  origin by its own collision (`tests/selftests/kraken_kill.json` walks up and presses Interact).
- **Dual targets and damage**: with two enemies in the soft set each pistol takes its own, so one enemy falls later than
  under a single lock (`StreamReal.SaveContinuesBitIdentically`'s l1_ship_exterior fight runs 1400 ticks for it). The
  gate on weapon-controller flags 0x9000 / bit 27 in 0x71E490 is not decoded [U].

## Mediterranean puzzles (l1_seafloor, l1_kraken_entrance, l1_kraken_room, l1_baldrs_tomb)

Walkthrough rows: gameplay matrix C02-09 / 11 / 13, C02-30, C03-11 / 16, C04-06 / 07 / 08.

- Eye door [O bytecode, graph data]: `ms_ani_eye_puzzle_piece` 232 (right) / 233 (left) and `_center` 231 are
  anim-graph objects. A side piece starts empty (state 0, a kind-5 record needing `ms_pt_seafloor_key_a`); the key,
  picked up and carried while swimming, is inserted (stimulus 0x20 -> state 2; vt 20(2): "has key" sticky +244, draw
  group 7, checkpoint). Each piece has one kind-8 ("use") record: Lara's use reaches the piece she engages
  (`World::engagedUse` -> `TickInput::engagedUse`; before, the nearest object in reach took it), turning it 120
  degrees (side 2 -> 3 -> 2, centre 0 -> 3 -> 0). The scripts hand the eye draw groups round and switch the other
  pieces' records off during a turn (instance 59). Centre vt 61: its position stickies +64 / +120 / +176 all set -> vt
  62 "Eye Puzzle Solved" (modifiable 4 (2) = the opening clip; plugs 236-238 `ms_uo_ping_map_eye_puzzle` command 0 ->
  state 1, action 28: their collision leaves the doorway; reveal 235). From the start state: centre once, left once,
  right twice. `ProgressionReal.EyeDoorSolvedByItsHandles` (to l1_norse_hallway_a). [I] no handle-turn clip on Lara.
- Press plates: docs/formats/PHYSICS.md "Pressure plates" (joint record; one weight does not press, two do).
- Grapple pulls on joints (`World::pullGrappled`; only targets collected by their model feature: the gears, the tomb
  gate — the `*_grapple*`-named pull targets keep the one-second script heave [I]): with Interact held, the simulated joints
  that move the section carrying the grapple feature are driven toward her at 240 units/s at the rope's end [I], each
  only when the rope has a component along its freedom (prismatic: more than cos 60 degrees along the axis; hinge: by
  the rope's moment). The states' range watches do the rest; action 0x1A (no target) lets go. [O data] gear 36: feature
  on model 1 = section 1, prismatic, limit 0..515, watch 512..537 -> trigger 14 -> locked at 512..518; gear 32: feature
  on model 12 = the ring (section 1) hinged on the column (section 12), whose braked prismatic joint leaves -15.8..237:
  trigger 15, it falls. Before its pull each room's one-way switch only tries; the gear's event calls the switch's off
  reaction (type 3637 vt 58: modifiable 4 (1)), which frees it for another pull (`Runtime`: the switch's state id moves
  along that transition). The bullet on the column (`World::uberShot`, [I] impulse) still works; whether tru.exe's
  bullets move it is [U]. `ProgressionReal.KrakenGearsPulledByTheGrapple`.
- Roped walk [I]: attached to a pull target and without Interact, the stick walks Lara (350 units/s, rope paid out to
  its longest); Interact braces her and pulls (Grapple.cpp). The page's gate solution needs it: the ring is grappled
  near the gate, she steps back onto the pad, then pulls.
- Tomb gate 45 [O data]: `ms_rb_tomb_gate` is a class-34 hinge with a limit and no motor (record flags 1) and a
  grapple feature 1036 up; its hinge marker lies at its foot (46.4 in front of its plane), axis = the marker's Z (level,
  along the foot): unlocked (script: multibody 77 0..90 once both locks left their range) it stands against its lower
  limit until the rope tips it over (`World::pulledHinge`; before, a door motor swung it open about the marker's X as
  soon as the locks were out). [O Wine, l1_baldrs_tomb] Lara on plate 221: plate -51, lock 70 down 1254 at 300
  units/s, back when she leaves; with one lock out the gate does not move. [U] The both-locks-out case (does it stand
  without a pull) was not reached under Wine: the harness's pick-up key press did not take the weights.
  `ProgressionReal.TombGateNeedsBothBlocksAndThePull`.

## Input rules: button timing, saving grab, ladder catch, pole drop (decoded 2026-11)

Sources: the player state chart of lara.drm (`openunderworld-work/hurdles/sc.py`, `gameplaygoal/inputrules/q.py`),
tru.exe (`ghidra/exports/decomp_all.c`, `hurdles/cond_decomp.c`, a full disassembly), lara.drm object data, generalbank
DTP 0x2F (`gameplaygoal/aim/inputmap.py`). This section supersedes the "Bad attach [I: trigger]" paragraph of the free
climb and the "Saving grabs [I]" sentence of PLAYER TAILORING above.

- **Button timing** [O]: the chart reads an input action through `0x7607C0` (conditions 0x65..0x6D, one per action:
  0x65 = action 52 Jump, 0x66 = 50 Crouch, 0x67 = 42 Sprint, 0x68 = 99 Interact, 0x6B = 25 Manual Aim, ...). The
  value is 1 on the frame of the press (action flag 0x10), frames held + 1 while down (flag 0x04; milliseconds x 30 /
  1000, at least 1), -1 on the frame of the release (flag 0x20), -1 - frames while up; the condition's parameter picks
  the compare (table 0x7609C0): 0 = up (< 0), 1 = down (> 0), 2 = released (== -1), 3 = pressed (== 1), 0x65 = held
  longer than Lara data +0x6E2 = **15 frames**. Conditions 0x96..0x99 test one action's flags directly (0x98 = pressed).
  Ours: `game::ButtonTime` / `Buttons` in `PlayerState` for jump, interact, drop, grapple (down, edge, seconds since
  the change, `heldFrames()`, `heldLong(15)`), updated first thing in `Player::tick`, saved (`controller.buttons`,
  older saves load them as held) and part of the state hash. A press also stays usable for `Tuning::pressBuffer` =
  one 30 Hz frame (`buffered()` / `take()`) [I: tru.exe flags a press for one frame; ours ticks twice a frame, and the
  saving grab's catch and press can fall on the same tick]. Not decoded [U]: what transition flag 7 does (on most
  button transitions out of clips with a critical section).
- **Saving grab** [O]:
  - Trigger: `0x74FDF0` runs on every markup attach. Bad attach (player `+0xEE8` = 1) when the attached markup's record
    has bit 0 of `+0x50`, or when `fall start z (+0x6DC) + (+0x6E0) - attach point z (+0x9F8) + markup +0x68 > limit`.
    The limit is float `+0xC` of the fall record (`+0xC4`: the three fall-damage heights, then this) of the jump query
    in effect (player `+0xA84`, looked up by `0x6F1950`): **711.13** for the ground, hang, DICE, pole and ladder jump
    queries (q2, q7..q10, q22, q23, q26, q28..q34, q43, ...), **1280.03** for q35, q44 (the air grab of the fall
    states), q46, q47, q51, q64, q71, q98, q106; with no query Lara data `+0xC14` = **951.33**. State property 0x1A7
    forces the flag either way and property 0x1F2 raises it (the bad-attach clip states S566 / S567, S1117 / S1118).
    [I]: `+0x68` is the root's offset under the attach point, so the height is root to root: the game's stacked
    drop-down ledges are 1304 (l1_ship_sinking_hall 242 -> 241), 1387..1427 (l5_valaskjalf 1488 -> 1485 -> 1484 ->
    1483) and 1501 / 1738 (l3_chapel) apart, all just over the fall limit, and ordinary catches stay under it. Ours
    (`Player::badAttach`): `State::Jump` / `HangJump` / `WallJump` = 711.13, anything else 1280.03 [I: our states for
    the query in effect]; ledges (`tryGrab`, a `HangJump` onto a ledge), DICE holds (`tryClimb`), ladders
    (`tryLadder`). The markup flag is not in our markup data [U]; poles (`Hpole_*_bad_attach_*`), perches
    (`perch_attach_air_bad_recover`) and wall hug have their own bad-attach clips, not played yet. The catch itself
    still needs `vel.z >= 2 x grabMinVz` [I: tru.exe's limit on a falling catch is not decoded].
  - tru.exe under Wine [O] (l4_temple ledge 1155, Lara pinned in the air beside it for 1.3 s, then let go;
    `gameplaygoal/inputrules/session2.sh`, `wine2.txt`): with her root 600 / 850 / 1100 over the hang's root she ends in
    the firm hang; 1350 / 1600 over it she is held in a lower pose (root 149 under the hang's, states 0x0D9DA070 /
    0x0D9DEF58) for 1.70 s from the catch and then falls to the floor: the limit lies in (1100, 1350], root to root, as
    the decoded 1280.03 of q44; 50 frames = 1.67 s. The air grab takes the ledge from afar while she is still pinned
    (caught with the ledge 1106 under her root, q44's kind-31 value; not 1406 under it) - ours still catches when the
    hands pass the ledge (`tryGrab`, [I]). The Interact press was not exercised under Wine (the scripted press came
    after the fall): it rests on the decode below.
  - While it lasts (chart S528, action 2101 = table 0x4B0F40: enter `0x75E5C0`, exit `0x75E6E0`): the HUD shows button
    icon Lara data `+0x70C` = 52 (`0x70CC50`; ours has no prompt yet), player timer 2 (`+0x74C`) is set to the
    difficulty table's `+0x14` at the PLAYER TAILORING level (`0x75E5EC` -> `0x6E7740`: **90 / 50 / 15 frames**; plus
    data `+0x2E0` while player `+0x504` -> `+0x50` is set [U]) and counted down every frame (`0x6EE24E`).
  - **Interact** recovers: input action 75 (virtual button 0) arrives as message `0x200004B` in `0x76FC30` ->
    `0x768C90` sets the result 2 -> T734 / T736 (ledge: `ledgeb_bad_attach_recover`; ladder T1829:
    `ladder_bad_attach_recover`). The timer's expiry message `0x9E` (index 2) -> `0x768CC0` sets 3 -> T735 / T1830: she
    falls. Crouch pressed (action 135) -> T553: she lets go. **Jump does nothing.** Ours: `savingGrab` seconds, counted
    from the catch (through a hold's start clip too), `Buttons::interact` buffered press.
- **Ladder: letting go and catching again** [O]: Crouch on a ladder (T1823 / T1820, cond 0x66) plays
  `ladder_detach_drop` (S1079, jump query q22) and falls (S1006, air grab q44). No chart transition onto a ladder reads
  Interact; the catch lower down is the air grab's, automatic as ours (`regrabCooldown` 0.3 s [I]). The key the
  controls page mentions is the saving grab of a bad catch (S1117 / S1118 `ladder_attach_air_bad`), implemented as
  above. [U]: whether tru.exe flags every catch of the ladder she just left (the markup flag / a state property).
- **Safety drop** [O-chart]: transition T114 leaves the ground states (S7 the roll, S8, S59, S75 and the gear states
  S708 / S727 / S730 / S733 / S743) for S534 of the hang group - whose T748..T750 pick the attach state S542 / S543 / S544
  - on: `PSCCF_IsFalling` (0x762C60: instance flag 0x80000, she has left the ground), the state's jump target a
  ledgegrab (cond 0x262, type 610), result flag 6 set and 7 clear (0x76F760: the bit set at query result +0x140), the
  target's entry (player `+0xA80`) not 5 / 7 / 8 / 9 / 10, cond 0x2BE = 0. No button and no gait condition: walking,
  running or rolling off an edge that carries a ledge she hangs from it by herself. T235 / T236 are the same for a
  ladder under the edge (`ladder_run_off_grab` / `ladder_attach_ground_top`, not implemented). T112 (`edge_stop`,
  request 54) stops her at an edge instead when the stick is pushed, cond 0x1EA (0x764FD0, a move-set value) = 2 and
  there is no target: not implemented [U: what sets 0x1EA]. Crouch at an edge stays the keyed drop (T777, query q46).
  tru.exe under Wine [O] (l4_temple block ledge 1155, west edge; `openunderworld-work/gameplaygoal/inputrules/
  session2.sh`, `wine2.txt`): from standing on the top 70 inside the edge, **walking** (Walk + the stick) she reaches the
  edge line 0.57 s later and without a key goes straight into the hang (root 158 out and 491 down in ~1.0 s =
  `drop_to_ledgeb`'s 31 frames, then the hang state of a jump's catch); **running** she overshoots the line by 82
  with her feet still at the top's height (one state for ~0.08 s), is taken back to the edge and down into the same
  hang (hang pose reached ~1.2 s after the line).
  Ours (`Player::trySafetyDrop` from `groundMove` when the ground is gone): the nearest ledge level with her feet
  (±40), within 150 of where she left the ground, crossed at more than 30 degrees, open below on the side she is
  heading to; `drop_to_ledgeb` as the Crouch drop [I: the result flags' geometry and the attach states' requests
  ("MOD" requests 16 / 17 / 21) are not decoded].
- **Pole drop** (ours): after Drop the pole she let go of is not caught again in that fall (`PlayerState::droppedPole`),
  and a fall propped up in place with no floor under her root for 0.25 s ends standing (`fallStall`) [I]. l4_traprooms
  pole 1337 hangs her feet 9..25 over a rubble mound: she comes down 27 beside it.
- Tests: `Game.ButtonTimingLayer`, `Save.ButtonTimingRoundTrip`, `Game.SavingGrabByFallHeightAndInteract`,
  `Game.SavingGrabAfterAJumpDown`, `GameReal.SavingGrabClips`, `GameReal.ValaskjalfDropDownSavingGrabs`,
  `GameReal.ClimbBadAttach` (rewritten), `GameReal.LadderLetGoCatchesAgainAndBadAttach`,
  `Game.PoleDropOverSteepGroundIsNotCaughtAgain`, `GameReal.TrapRoomsPoleDropAfterFling`,
  `Game.SafetyDropOffALedgeEdge`, `GameReal.TempleSafetyDropOffThePlatform`; self-tests `saving_grab` (l5_valaskjalf),
  `safety_drop` (l4_temple), `pole_drop_after_fling` (l4_traprooms).

## Controller rules: jumps, landings, ledge extras, sprint (decoded 2026-11)

Sources: the player state chart of lara.drm (`openunderworld-work/hurdles/sc.py`, `gameplaygoal/ctrl2/c.py` - states,
transitions, the actions a state runs - and `v.py` - the chart variables), tru.exe (`ghidra/exports/decomp_all.c`,
`gameplaygoal/inputrules/full.asm`), lara.drm clips (`hurdles/clipprobe`) and tru.exe under Wine in l4_temple
(`gameplaygoal/ctrl2/s1.sh`, traces `w1.txt`; tru = ours + (500.70, 9432.76, -19909.81)). Chart conditions used below:
0x65..0x68 the timed buttons ("Input rules"), 0x96 / 0x97 / 0x98 an action held / released / pressed (51 Crouch, 52
Jump, 42 Sprint), 0x191 the stick's level (401 none, 402 / 403 light, 404 full), 0x192 its quadrant against her heading
(410 forward, 411 back, 412 / 413 the sides), 0x14 the state's time in frames, 0x1E the fall so far, 0xC9 the clip done,
0xCA the clip's critical section; a parameter group of 1000 compares with a chart variable.

### Jumps: the Jump button's hold, the gait, the stick in flight

- **Holding Jump does nothing** [O chart, decomp, Wine]: no transition out of the take-off states (S248 standing, S245 /
  S247 with the stick, S280 / S281 running, S249 from the sprint) or of the free flight (S287 -> S288 -> S294) tests
  the Jump button (conds 0x65, 0x96..0x99 with action 52), and the flight's action 510 (table at tru.exe file 0x4B11D4:
  enter 0x7522A0, tick 0x752410) reads no input. tru.exe under Wine, Jump tapped (60 ms) against held to the landing:
  standing jump apex 201.5 both, in the air 550 ms both; running jump apex 155.8 both, take-off (65.83, 42.54) a frame
  both, 954.7 from the first airborne sample to the landing both. The controls page's "a little longer in the air" is
  not in the PC game.
- **Gait** [O chart, Wine]: the walk and the light stick take the stick jump (S245, query q43: 40 along + 50 up), the
  run the running jump (S280 / S281 by the planted foot, query q1: launch 65.83 + 42.54 [Wine]; its record's velocity
  reads 90.25 + 42.54), the **sprint** S249 (T182 from S59 and its fall-off state S13), whose query q26 has q1's
  velocity: under Wine a jump from the sprint (state S59 at 1879 units/s) takes off at the same (65.83, 42.54), apex
  155.8, 954.6 to the landing - it is longer only by the faster run-up and the sprint that resumes 0.15 s after the
  landing (the landing group's T2108). The roll's jump (T53 -> S251 `ng_rolling_jump_forward`, query q20: 53.83 +
  53.83) is not ported.
- **The stick in flight: a brake, nothing else** [O decomp, Wine]: the flight state S287 (and the wall jump's S1292)
  runs action 511 (enter 0x73ECE0, tick 0x750040) beside 510. Enter stores the horizontal take-off velocity (chart
  context +0x430) and the stick's forward part against her heading s0 (+0x420). Each frame, when the jump has no
  markup target (player +0xA64 = 0), the jump query's +0x64 >= 0.05 (0.9 for the ground / hang / pole / ladder jump
  queries, 0 for the straight DICE leap q27), the stick is off centre, |s - s0| > 0.05 and s < query +0x74 (-0.2; -1
  for q3 and q19: never):
  `horizontal velocity = take-off velocity x (1 - clamp((-0.2 - s) / 0.8, 0, 1) x query +0x68 (0.6))`, the vertical
  speed untouched. Straight back = 40 % of the take-off speed; nothing gives speed back and nothing steers sideways
  or adds speed. Wine: Back pressed 0.1 s into a running jump: 65.83 -> **26.33** a frame on the next frame, landing
  500.3 from the first airborne sample instead of 954.7; the stick let go in flight: no change.
  [U]: 0x750040's two time limits (frames since context +0x444 > query +0x70 = 15 once +0x444 is set - its setter
  was not found; frames in the air > 2 x Lara data +0x4C (1280.03) / g = 447) are not applied.
- Ours: `Player::airBrake` (State::Jump with no aimed target; `Tuning::airBrakeStick` / `airBrake`;
  `PlayerState::jumpVel0` / `jumpStick0`, saved as `controller.airBrake`). Nothing else changed: ours already flew one
  arc per query whatever the button did, and treated the sprint as a run for the jump.
- Tests: `Game.JumpHoldNoEffectAndStickBackBrakes`, `GameReal.TempleJumpHoldStickBackAndSprintJump` (the Wine
  numbers); self-test `jump_brake` (l4_temple).

### Landings

- **Dispatch** [O-data]: every landing passes the chart state S1275, whose transitions are tried in this order:
  1. T2101 / T2090 / T2102: the swan dive's and the water landings ("Water moves").
  2. T2100 -> S1283 `ng_land_rolling` (request 677, start frame 4; root 808 forward in 0.67 s): Crouch down (cond
     0x96, action 51) but not held long (`!0x9B:51`), the stick pushed (cond 0x191 not 401) forward of her heading
     (0x192 = 410), nothing carried (0x5DD = 0). No height condition: the roll comes at any height, before the height
     tests. It wants a press shortly before the landing: tru.exe under Wine with Crouch held from 0.7 s before the
     landing and the stick forward lands in the stumble (S1278) and crouches 0.15 s into it (T2106, S80); ours takes
     0x9B as the buttons' held-long limit of 15 frames [I].
  3. T2091 -> S1280 / T2092 -> S1281 `ng_land_hard` (request 39; 1.1 s in place): the fall >= chart variable 345 =
     **3185.84** / 346 = **1264.22**. The fall is cond 0x1E (tru.exe 0x764B40): z on becoming airborne (player
     `+0x6DC`) - z, compared with a chart variable (0x783BD0 / 0x782060; variables = the pointer table at lara.drm
     object data +0x5C290, 400 entries of {type, value}).
  4. T2118 -> S1278 `ng_running_land_stumble` (request 725, start frame 3; root 950 in 1.07 s): the fall >= variable
     344 = **632.11**, the stick pushed and not backward (0x192 != 411), no low ceiling.
  5. T2089 -> S1284 `ng_running_land`: the stick at run level (0x191 not 401 / 402), not backward.
  6. T2093 -> S1281: a low ceiling (0x1F7) [not ported: ours lands plainly and stands].
  7. T2094 -> S1279 `ng_generic_land_forwards`.
  No transition tests her speed (ours used to need 600 units/s to run on). Fall damage starts higher (1280.03,
  Numbers "Fall damage"): the hard landing from 1264.22 to 1280.03 costs nothing.
- **While the landing clip plays** (group S1276) [O-data]: Jump pressed takes off again at once (T2112..T2115 into the
  running / standing jump states, flag 7); Crouch pressed with the stick forward within state time < variable 347 =
  **4** frames rolls after all (T2107); from 4 frames on, Crouch held with the stick pushed crouches (T2106), Sprint
  held at full stick sprints (T2108), and the stick pushed anywhere but forward leaves the clip for the ground moves
  (T2099) - the last three only outside the clip's critical section (cond 0xCA); the clip's end goes back to the ground
  group (T2097). Crouch held with no stick crouches out of the plain landing once its critical section is over
  (T2104; Wine: 0.35 s into it after falls of 400 and 800, 0.61 s after 1200; ours `Tuning::landCrouchTime` 0.36 s).
- Ours (`Player::land`, `State::Land` in `Player::tick`; `Tuning::landStumbleFall` / `landHardFall` /
  `landRollFrames`, `PlayerState::landTime`): exactly this dispatch, the stick quadrants taken as 45 degrees either
  side of her heading [I] and the run level as the run's (stick >= 0.5, no Walk). [I]: `ng_land_hard` is treated as
  critical to its end (no jump, no leaving: 1.1 s), the other landings as never critical - the clips' critical
  sections are not read. tru.exe under Wine: see the trace
  list at the end of this section.
- Tests: `Game.LandingByFallHeightStickAndCrouch`, `Game.LandingClipJumpRollCrouchAndLeave`,
  `GameReal.TempleLandingsByHeightStickAndCrouch`; self-tests `landing` (l4_temple), `fall_damage` (its 1500 fall now
  expects `ng_land_hard`).

### Ledge extras: the pull-up's button, the handstand, hand over hand faster, the free hang

- **Pull-up and handstand** [O chart, Wine l4_temple ledge 1155]: Jump pressed in the idle hang with a top to climb
  onto (clearance 650) enters S439, which waits on the button: let go -> S456, the pull-up (T608; Wine: Jump down 150
  ms, the climb starts 13 ms after the release and she stands 0.82 s later, 231 in and 494 up = `ledgeb_climbup_fast`
  played whole); held past the buttons' 15 frames (cond 0x65 mode 0x65), state time >= 10, the stick not back or
  aside -> S440, the **handstand** `ledgeb_climbupflair` (request 1003; Wine: S440 0.50 s after the press, 2.47 s
  long, ending 228.5 in and 494 up; free hang: `ledgeu_climbupflair`). With the stick up she is in the lean S435 and
  Jump pulls up at once (T613 -> S456), held or not: no handstand there (Wine). T558 (request 1002 from its start
  frame 6, the quick one) is the Jump press straight out of the catch states S530 / S531 / S542 / S548. Pushing the
  stick ends S456 early into the run (T244: Wine 0.58 s instead of 0.82 s) [not ported].
  Ours (`PlayerState::pullWait`, `Player::tick` Hang): the wait in the hang's idle pose [I: S439's own pose], the
  release / the 15-frame hold as above, the pull-up clip whole (it used to start at frame 6 on the press: 0.57 s).
- **Hand over hand faster with Interact** [O chart, clips, Wine]: a press while the stick shimmies (T581 / T582 from
  S427 / S432 / S431, not at the ledge's end) plays one `ledgeb_right_hustle` (S441 / S442, request 968, mirrored to
  the left: 371 aside in 0.57 s against `ledgeb_right`'s 196 in 0.70 s; Wine: 368 in 0.47 s from the press to the
  ledge's corner); pressed again from the clip's frame 8 on it starts over (T621 / T624, cond 0xD7), else its end
  returns to the shimmy (T622 / T623). Holding Interact is one press. Ladders: Interact - or Jump (T1814) - with the
  stick up, more than chart variable 194 = 4 rungs under the top (cond 0x2B3), plays `ladder_climb_up_fast` (S1057:
  four rungs, 429 up in 0.47 s, against a rung, 110.6, per 0.30 s); again from frame 10 (T1809, variable 193).
  Pillars the same way (S1197 `pillar_climb_up_fast` with `_init` / `_end`, T1980 / T1981) [not ported: the pole
  agents' file]. No such transition exists for the free climb (DICE), the ladder downward, or horizontal poles
  (`HPole_ParaForward_fast` is not reached through cond 0x68). Sprint does nothing on a ledge (Wine).
  Ours: `Player::tick` Shimmy (`Tuning::hustleAgainFrame`), `Player::ladder` (`ladderFastRungs`,
  `ladderFastAgainFrame`). Not ported: the hustle's corner clips (`ledgeb_concave / convex_right_phase*_hustle`; ours
  rounds corners with the ordinary ones).
- **Free hang** [O chart, decomp; not ported]: lara.drm has a second clip set `ledgeu_*` ("unbraced": idle, right 161
  in 0.80 s, right_hustle 324, attach_*, drop_antic, climbup_antic / climbupground / climbupcrouch / climbupflair,
  convex / concave corners, `ledgeb_to_ledgeu` / `ledgeu_to_ledgeb` for a shimmy across the change). The hang states'
  requests pick the set by cond 0x26C = 691 (tru.exe 0x7622D0: bit 1 = player `+0xED4` == 0): `+0xED4` is the feet's
  support, set on every attach and shimmy step by 0x75D6C0 / 0x75F2D0 (0 braced, 1 free, 2 not a ledge), for a ledge
  (MUD 0x40000000) = no wall found by the probe 0x74F480 under the attach point. In the free hang the hang jumps up /
  aside / back are not offered (T573..T580 need 691; the leans are S434 instead of S435) and climbing up is the
  `ledgeu` clips. Continue at 0x74F480 (the probe's reach) and `Player::placeOnLedge` (the `ledgeu` hand offset is
  (-5.2, -145.1, 474.0) against the braced (25, -139, 492)).
- Tests: `Game.ShimmyHustleWithInteract`, `GameReal.TempleShimmyHustle`,
  `GameReal.ArcticSeaLadderClimbsFasterWithInteract`, `GameReal.TemplePullUpOnReleaseAndHandstandOnHold`; self-tests
  `ledge` (step `hustle_left`), `ladder_fast` (l7_arctic_sea).

### Sprint, the run's turn and stop

- **Sprint** [O chart, data, Wine]: S59 is entered by the Sprint press at full stick (T224 from the run's states) or,
  held, from the run loop S65 (T187, state time >= variable 154 = 0) while CanSprint (cond 0x205 = tru.exe 0x752220);
  it ends on the release (T184, cond 0x97) into S65 or exhausted into S73. Under Wine the run's start S63 lasts 0.83 s
  (855 units) whether Sprint is held or not, S65 one frame, then S59 at **1879 units/s** (`sprint_loop`'s root motion:
  1927; the earlier trace: ~1860). Its moves: the jump S249 (the run's arc, "Jumps" above), the roll S6
  `ng_roll_front_sprinting` (Wine: 1366 in 0.93 s; the run's S7: 1327 in 0.88 s), the kick S1432, the stair / slope
  blends (request 740 `ng_sprint_up_stair` / `_down_stair`, not ported), and after a landing the sprint resumes from
  the landing clip's 4th frame (T2108; Wine 0.15 s). There is no slide under anything and no sprint-only stop.
  Stamina data [O-data, semantics U]: Lara data +0x520 = 300, +0x524 = 30, +0x528 = 30, +0x52C = 300 (frames; drain
  0x74A090, refill 0x74A100, the test 0x74FD70); ours keeps the Wine-timed 9.7 s / 0.9 s model.
- **Turning** [O Wine]: with the stick swung 45 degrees the run loop turns her at **1.755 rad/s** and the sprint at
  **1.835 rad/s**, linearly, overshooting ~0.03 rad (run: yaw 1.543 -> 0.841 in 0.400 s). Ours: `Tuning::runTurnRate`
  / `sprintTurnRate` in the loops; the start clips and the walk keep `turnRate` 6 rad/s [I] (the original's start /
  180 clips S48 / S36 / S61 `ng_running_turn` are not ported).
- **Stopping** [O chart, Wine]: the stick let go leaves the run loop S65 for S57 `ng_running_stop` (223 on) only
  after variable 374 = **15 frames** in it (T176); earlier - and from the run's start S63 and the sprint S59 - the
  group's T40 puts her into S39 at once: she stops on the spot (Wine: W let go 0.32 s into the loop: 0 units further;
  out of a sprint: 0; 1.2 s into the loop: S57, 266 further). Ours: `PlayerState::runTime`,
  `Tuning::runStopFrames`; S39's 0.48 s settle clip is the idle blend [I].
- Tests: `Game.RunStopsOnTheSpotOrSlides`, `GameReal.TempleSprintTurnAndStops`; self-tests `locomotion` (step
  `run_stop`), `sprint`, `jump_brake`, `landing`.

Wine traces of this section (`gameplaygoal/ctrl2/w1.txt`, 46 runs; `an.py` prints the chart states by index: state
address = 0x...242D0 + 40 x index): standing / stick / running / sprint jumps tapped and held, the stick let go and
pulled back in flight; run and sprint stops, rolls, turns; falls of 200..2100 with no key, Forward, Crouch, both
(plain landing S1279 up to 1200, hard S1281 at 1400, running landing S1284 at 400 and the stumble S1278 at 800 with
Forward, the crouch 0.35..0.61 s into the plain landing with Crouch held, the stumble then the crouch with Crouch held
from the start: no roll); the pull-up tapped / held / with Forward; the shimmy with Interact tapped / held / Sprint.


## Animation layers and IK (decoded 2026-11; src/anim/Ik.cpp, src/game/Ik.*, `World::limbLocks`)

Sources: tru.exe (Ghidra export and disassembly at the addresses given), lara.drm object data (ObjectBase +0xC8; every
outfit carries the same numbers), the player state chart. Nothing here was measured under Wine: every [O] below is
read from code and data, and what the code was not followed far enough to say is [U].

### The original

- **Limb locks** [O]. Lara's IK is `cdc::GenericLimbLockController` (RTTI 0x843130, base `cdc::LimbLockController`;
  constructor 0x445160, per-tick update 0x445350). The player set-up (0x6EFDA0) builds one per record of the object
  data list **+0x5C8 (u16 count = 30) / +0x5CC (records of 0x4C bytes)** and keeps them at player +0xF34; +0xF20 /
  +0xF24 / +0xF28 / +0xF2C cache the active one for the left foot / right foot / left hand / right hand (limb ids
  0x30 / 0x31 / 0x40 / 0x41, 0x767280 → 0x6F1A60). Record layout (`game::LimbLock`): +0 IK chain of the instance's IK
  system (instance +0x2E0, chains of 0x120 bytes solved joint by joint at 0x443D00), +4 type, +8 group, +0xC / +0xE
  limb ids, +0x14 / +0x18 probe reach up / down, +0x1C offset, +0x28 / +0x2C blend in / out frames, +0x34 / +0x36
  segments, +0x30 / +0x3C a list of further chains switched with it, +0x40 flags. The type picks the target callback
  (controller +0x14C): 1 → 0x73E6F0 (the floor under a foot), 5 → 0x732EC0 (a markup box's segment), 6 → 0x7445A0,
  7 → 0x737790 (the ledge under a hand), 8 → 0x735770, 9 → 0x741490 (the bike: hands to its grip segments, feet to its
  pegs), 10 → 0x758CD0, 0xB → 0x7406E0 (wading).

  | records | type | limbs | probe up / down | offset | in / out | flags | used by |
  |---|---|---|---|---|---|---|---|
  | 13, 15 | 1 | feet (segments 6 / 11) | 158.03 / 158.03 | z 26.86 | 2 / 3 | 0x1F | on from the start |
  | 16, 17 | 1 | feet | 158.03 / 158.03 | z 32.60 | 2 / 6 | 0x0D | not found switched on [U] |
  | 4, 7 | 7 | hands | — | — | 2 / 3 | 1 | ledge hang S460 / S530 / S531 |
  | 5, 8, 12, 14 | 9 | hands, feet | 316 (feet) | z 8.74 (feet) | 2 / 3 | 1, 5 | S1329 / S1353 / S1354 (the bike) |
  | 26..29 | 0xB | hands, feet | 158 / 31.6, 237 / 31.6 | z 9.48, 31.6 | 6 / 6 | 1 | S107 / S202 (wading) |
  | 22..25 | 0 | left hand, left foot | 158 / 63.21 | — | 1 / 3 | 1 | S1291 `wall_jump_attach_air` |
  | 0..3 | 5, 2 | hands, feet | 158 / 63.21, 316 | (−15.8, −9.48, 0) | 2 / 3 | 0 | S595 `death_shotunderwater` |
  | 6, 9, 10, 11, 18..21 | 0, 6, 8 | hands, feet | | | | | not found switched on [U] |

- **Which are on** [O]. A record with flags 1 and 2 is on from its construction (0x443C60): only the foot records
  13 / 15. State property **0x141** (0x767390) switches a list of records on for a state, property **0x142** switches
  a list, or with a bool all of them, off; leaving the state rebuilds the defaults. All-off states (46 of 1445):
  the rappel tree (S309), ladders (S1051), pillar, perch, wall jump / wall slide, the swim climb-outs, the in-air
  "feet up" stages, S1 (the cheat-mode start state). The four the cache names are re-read after every change.
- **The foot lock's gate** [O]. Every controller but the wading ones carries the per-tick gate 0x6EFD60: a type-1
  controller may lock only when Lara has a mount (player +0xF00: the bike), when the floor moved her this tick
  (+0x11CC, set by the floor query 0x5DCE20 when it adds an instance's motion to her), or when an instance is under her
  feet (the handle at +0x610, set by the same query when the floor hit is an object). **On unit terrain the foot locks
  never engage: on slopes and stairs her feet are the clip's.** The lock itself (0x73E6F0 → 0x438F60): one ray along the
  instance's up axis from `up` over the foot segment to `down` under it (collision mask 0x2C19E7; flag 8 lets object
  hits count), the target = the hit + the hit's normal × offset z; flag 0x10 turns the foot to the normal (0x443B50 →
  0x443A80). A foot is locked for its support phase: anim events 0x34 / 0x36 (and 0x35 = none; handler 0x716F30) set the support foot
  (player +0xF3C, state property 0x12F with values 301..306 overrides it per state) and events send the controllers
  messages 0x37 / 0x38 (0x767520 / 0x767570). There is **no pelvis bone offset**: +0xF1C, which looks like one, is the
  root's own vertical correction of the tick (0x71BFA0), added to the probe reach.
- **The support-foot root** [O existence, U rule]: player +0xF18 is a 0xD0-byte object (0x434F90, segments from data
  +0x168 = 1 and +0x164 = 0) fed by the same events (0x435030); it belongs to the root motion, not to the pose.
- **Hands on a ledge** [O]. Grabbing and moving on a ledge is root alignment by anim targets
  (`HandMatchingFullAnimInterpTarget`, RTTI 0x85EC34, with the other attach targets): the clip's hands meet the ledge
  because the root is put there. On top of that the hang idle states switch the type-7 locks on: 0x737790 takes the
  ledge markup she is on (player +0xC94, flag 0x40000000) and her place on its polyline (+0xC98, segment + fraction)
  and gives each hand (limb 0x40 / 0x41) a point on that polyline, 20 units inside a segment end when she is near
  one. The exact point along the ledge is [U] (the callback's control flow did not decompile cleanly).
- **Head look-at** [O] (0x6FD080, called by 0x6FD7D0 each tick): the head segment (data +0x1B0 = 17) is turned to the
  combat target's marker 0xB (the targeting system's current target, player +0x854 → +0x128), or to a scripted
  point (player message 0x72 stores it at +0x380 with a radius / timeout; message 0x69 clears it) when that point is
  within 12000 units, in front of her and in sight from 60 units (+0xCC8) short of it. The one sender found is a
  state-chart action of the player (0x76E190): the point at player +0xCA0 (what she is about to engage) plus a
  per-case offset of 350 / 500 units, pulled back to the first collision hit. Which states run it is [U]. Angles are taken relative to
  the neck segment (+0x1A8 = 16) and clamped to **±60° aside (+0xCD4) and ±15° up / down (+0xCD8)**; script variables
  0xFE / 0xFF / 0x100 narrow them. The turn is a segment rotation override (mode 10) interpolated over **3 frames**
  (+0xB6C), restarted when the wanted angle jumps by more than 30° (+0xB68), and released over 9 frames (900 anim
  time units, 0x448710) when there is nothing to look at.
- **Aim layers, draw / holster, blend times**: unchanged from the sections "Combat" (the aim grids over the spine-14
  subtree, equip / unequip clips), "Manual aim, adrenaline inputs, dual targets" (0x7035F0 hands the second target to
  one arm's aimer) and "Animation mapping" / docs/formats/ANIMATION.md "Transitions" (each anim request's interp
  frames and start frame, [O]).

### Ours

- `anim::rotateBoneModel` / `translateBoneModel` / `solveTwoBone` (src/anim/Ik.cpp): bone turns in model space and an
  analytic two-bone solver (the middle joint bends in the plane it is bent in, the upper bone swings, the end keeps
  its facing; out of reach = 0.999 of full stretch) [I: the original's chain records are not decoded].
- `game::loadIkTuning` reads the 30 records and the look-at numbers; `IkTuning`'s defaults are the same four records
  and numbers for synthetic libraries.
- `World::limbLocks` runs after the clips, the weapon layer and the throw layer, before the ragdoll and the hair:
  - **feet** (`game::plantFeet`, records 13 / 15) in the on-foot states (idle, walk, run, stop, turn, land, jump
    prep, crouch, use, block, hit) **when the floor under her root is an object** (the original's gate; the mount
    case belongs to the bike records, below). Probe as the record. Where the original holds the ankle 26.86 off the
    floor for the support phase, ours keeps the clip's ankle height, now over the floor found under that foot (plus
    26.86 × (1/n.z − 1) for a tilted sole), fading out for a lifted foot between half and all of the probe's reach
    under the ankle [I: no support events]; the pelvis (segment 1) comes down by what the lower foot needs [I: the
    original has no such offset; without it a leg cannot reach a lower floor]; the foot turns to the normal.
  - **hands** (`game::lockHands`, records 4 / 7) in the `ledgeb_idle` hang (not during a one-handed saving grab):
    each hand (bones 55 / 80) to the nearest point of the ledge's polyline, at least 20 from its ends [O the line and
    the 20; I the point along it = where the clip has the hand].
  - **head** (`game::lookAt`): to the combat target's aim point within ±60° / ±15° of the neck, weighted by the
    weapon layer's draw ramp [I: the original looks at the targeting system's target, its 3-frame interpolation is
    replaced by that ramp]. Only the head bone turns; the clip's head roll is kept.
  - **second arm** (`game::aimArm`): with a second target her left arm (the gun that fires at it) swings at the
    shoulder from the primary target to the second, at most 90° [I].
- The passes keep **no state**: a pose is a function of the game state (replays, saves and `stateHash` need nothing
  new; no save version change). The price: no 2 / 3-frame fades in or out. They are visual only: the controller, the
  collision capsule and the root motion never read the pose. What does read posed bones: a gun's muzzle (with a
  second target the left gun's shots start from the swung arm) and the follow camera's sight ray from the pelvis
  (lowered with it on an uneven object floor).
- Cost (`GameReal.IkPassesAreCheap`, Release, one core, 153 bones): feet + pelvis + head + second arm together
  **0.015 ms** a tick, the hands **0.005 ms** (a 60 Hz tick is 16.7 ms).
- Probes: `ik.*` and `pose.bone.<segment>.x|y|z` (docs/SELFTEST.md); scenarios `ik_pose` (hands on ledge 973's line
  in l3_main_bedroom: the clip alone leaves them 1.5 / 13.4 units off the sloping line, the lock under 0.01; the
  shimmy without locks; head limits on a target) and `ik_feet` (l1_baldrs_tomb: no foot pass
  on terrain, both feet planted on press plate 219).
- Tests: `Anim.RotateBoneModelTurnsTheSubtreeAboutTheBone`, `Anim.TwoBoneIk*` (synthetic leg);
  `GameReal.IkTuningDecodesTheLimbLocks`, `GameReal.IkFeetFollowASlopeAndStairs` (ng_idle and ng_run on a 20° slope:
  planted ankles within 1 unit of the wanted height, straight under the clip's, the sole turned 20°, the root bone
  untouched; a 40-unit stair: left foot 40 down, pelvis 40 down, right foot in place; level floor: pose bit-identical),
  `GameReal.IkHandsLockToTheLedgeLine` (a ledge 8 units off and 8° turned: both hands within 0.1 of the line, not
  moved along it; the 20-unit end gap), `GameReal.IkHeadLooksAtATargetWithinItsLimits`,
  `GameReal.IkLeftArmGoesToTheSecondTarget`, `Game.IkPassesStayOffOnTerrain` (the gate in `World`: no foot pass on
  a terrain ramp).

### Differences and open points

| | original | ours |
|---|---|---|
| foot lock on terrain slopes / stairs | none (gate 0x6EFD60) | none |
| foot lock on objects | support foot held at 26.86 off the hit, 2 / 3-frame blends | clip height over the hit, lifted feet fade, pelvis drop, no blends [I] |
| hands on a ledge | type-7 lock in the hang idle states; point along the ledge [U] | same states' clip (`ledgeb_idle`); nearest point of the line |
| hands in shimmy / corners / pole / ladder | clips + anim-target root alignment (`LadderRungAlignAnimTarget`, `HPoleAirAttachAnimTarget`, ...) | clips + root fit |
| head | target marker 0xB or scripted point; 3-frame interp | combat target's aim point; draw ramp; scripted look-at point (message 0x72) not wired |
| bike (type 9), wading (type 0xB), wall-jump (type 0) and death (types 5 / 2) records | on in their states | not applied: their callbacks (0x741490, 0x7406E0, the bike's grip / peg data +0x4C / +0x4D / +0x710) are the next thing to decode |
| per-arm aim | one aimer per arm (0x7035F0) | grid for both arms + shoulder swing of the left to the second target |
| anim support events 0x34..0x38, the +0xF18 support-foot root | drive the lock phase and part of the root motion | not decoded |

Render sanity (viewed, kept privately in the work dir `gameplaygoal/ik/`): `hang.png` (ledge 973, tick 200: both hands
on the ledge) and `look.png` (pistols drawn on a dummy up to her left: head turned to it). No comparison against the
original's frames was made for these passes.
