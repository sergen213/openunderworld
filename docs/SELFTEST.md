# Gameplay self-tests: `ou-game --self-test`

`ou-game --self-test FILE.json` runs a gameplay scenario in the real game: the bigfile, the level load and unit
streaming, the scripts, physics, enemies and the same `Actions` path as `--input-script`. The scenario says where the
game is entered and gives a list of steps; each step sends input, then its expectations are checked against the
World's own state and against what was measured while the step ran. The run writes a JSON report and exits with the
verdict. Nothing passes because input was sent: every step must say what has to be true afterwards.

Code: `src/app/game/SelfTest.{hpp,cpp}`, hooks in `src/app/game/main.cpp`. Scenarios: `tests/selftests/*.json`
(ctest `game_selftest_<name>`). Unit tests of the evaluator: `tests/test_selftest.cpp` (`SelfTest.*` in
`ou_game_tests`).

## Running

```
ou-game --game-dir DIR --self-test tests/selftests/locomotion.json [--self-test-report OUT.json] [--self-test-trace]
        [--renderer opengl --no-audio --no-overlay --draw-last --sync-loads --width 160 --height 120]
```

- Exit code: `0` every step passed; `1` a step failed (or the report could not be written); `2` malformed scenario
  (bad JSON, unknown key / probe / operator, a step without expectations, a bad input or plan line): nothing is loaded;
  `77` no display / renderer (skipped, as the other `ou-game` tests).
- The report goes to `--self-test-report`, by default `<scenario name>.report.json` in the working directory. A
  one-line-per-step summary and the verdict go to stdout; the game's log (and one `self-test step ...` line per step)
  to stderr.
- Ticks are fixed 60 Hz (`--render-hz` defaults to 60; one tick per frame). Movies are off; script cinematics are
  skipped unless the scenario says `"cinematics": true` (their events and placement still run, as `--no-cinematics`).
  `--draw-last` skips all drawing (the simulation never reads it); without it the run can be watched.
- The same scenario gives the same report on every run (the report's `sim` is the World's state digest at the last
  step's end); the desktop's mouse is ignored. `--sync-loads` loads neighbour units on the main thread, as the replay
  tests do.
- `--self-test-trace`: one stderr line per tick (`self-test <step> t <tick> <state> <clip> pos X Y Z yaw D speed S vz V
  hp H`), and at the start every checkpoint, grapple target, bike and pickup of the loaded units with its position.
  Use it to write steps.
- `--self-test` excludes `--input-script`, `--replay-input`, `--autopilot`, `--frames` and `--cine`. `--level`,
  `--spawn`, `--chapter` and `--load` come from the scenario.

Under ctest (`CMakeLists.txt` `ou_self_test`): every `tests/selftests/*.json` is registered as
`game_selftest_<file name>` (re-run CMake after adding one); it needs `OU_GAME_DIR` and a display, else it is skipped
(77). The report is written to `<build>/selftest/<name>.report.json`, the game's log beside it (`.log`).

```
OU_GAME_DIR="/path/to/Tomb Raider Underworld" ctest --test-dir build -R game_selftest_ -j2 --output-on-failure
```

Run it inside a private compositor, never on the desktop (the window is real):
`dbus-run-session -- env -u DISPLAY kwin_wayland --virtual --socket ou-selftest --exit-with-session ./inner.sh`.

## Scenario

```json
{
  "version": 1,
  "name": "ledge",
  "chapter": "Core mechanics: ledge grab, shimmy, pull up",
  "walkthrough": "https://tombraiders.net/stella/walks/TR8walk/details/croft1.html",
  "level": "l3_main_bedroom",
  "spawn": [-8484.3, -4330, 244, -90],
  "fixtures": [{"health": 1000}],
  "steps": [
    {"step": "jump_and_grab",
     "input": ["0.1 jump 1", "0.2 jump 0"],
     "duration": 3,
     "until": {"probe": "player.clip", "eq": "ledgeb_idle"},
     "expect": [
       {"visits": ["idle", "jumpprep", "jump", "hang"]},
       {"probe": "player.ledge", "eq": 973},
       {"probe": "player.saving_grab", "eq": 0}
     ]},
    {"step": "pull_up",
     "input": ["0 forward 1", "0.1 jump 1", "0.2 jump 0", "0.5 forward 0"],
     "duration": 4,
     "until": {"probe": "player.state", "eq": "idle"},
     "expect": [
       {"clips": ["ledgeb_climbup_fast", "ng_idle"]},
       {"probe": "step.height_gain", "near": 496, "tol": 6, "note": "the hang's hands are 495.6 over her root"},
       {"probe": "player.grounded", "eq": true}
     ]}
  ]
}
```

Unknown keys are errors everywhere. Comments (`//`, `/* */`) are allowed in the file. `"note"` (any text) is accepted
at the top, in a step and in a condition, and ignored.

### Top level

| key | value |
|---|---|
| `version` | `1` (required) |
| `name` | scenario name (required); the report's `scenario` |
| `chapter` | free label for the report: the chapter or mechanic group the scenario belongs to |
| `walkthrough` | URL of the walkthrough page the steps follow (optional; link only, never copy its text) |
| `level` | unit to enter (`"l4_pool"`; `pc-w\l4_pool.drm` works too), at its `player` intro or at `spawn` |
| `spawn` | `[x, y, z, yawDeg]` in the unit's frame, with `level`. As `--spawn`: she is put on the floor found up to 1000 below the point |
| `level_chapter` | with `level`: the chapter index the unit is played in, for units that belong to two chapters (the manor units: `0` = the prologue; without it the chapter is the one in the unit's name, `game::chapterOfUnit`: `l3_*` = 3) |
| `start_chapter` | `0..8` (`game::kChapters`): start that chapter as New Game / chapter select does (its intro message, gear and loadout) |
| `load` | save slot name (`$XDG_DATA_HOME/openunderworld/saves/<slot>.ousave`), as `--load` |
| `cinematics` | `true`: play script cutscenes (default `false`: skipped) |
| `stop_on_fail` | default `true`: after a failed step the rest are reported `"skipped": true` and not run. `false`: run them all |
| `fixtures` | set-up applied before the first tick (below) |
| `steps` | the steps, run in order (required, not empty) |

Exactly one of `level`, `start_chapter`, `load` enters the game.

### Steps

| key | value |
|---|---|
| `step` | id, unique in the scenario (required) |
| `input` | input-script lines (a string, or an array of lines), times in seconds from the step's start |
| `plan` | autopilot plan lines (a string or an array), run from the step's start |
| `duration` | seconds (required, > 0): how long the step runs, or its timeout |
| `until` | a condition or an array of conditions (all must hold): the step ends on the first tick they hold |
| `expect` | conditions (required, not empty): all must hold for the step to pass |
| `fixtures` | set-up applied when the step starts |

`input` is the `--input-script` syntax (`game::InputScript::parse`, `tests/replays/*.txt`): `<seconds> <action>
[value]`; a value persists until changed; actions `forward`, `right` (stick, -1..1, camera-relative), `walk`, `jump`,
`drop`, `interact`, `draw`, `fire`, `aim`, `switch`, `weapon`, `heal`, `grapple`, `sprint`, `light`, `grenade`,
`recenter`, `camera` (0 / 1), `manual` (Manual Aim), `lookx`, `looky` (camera rate, rad/s). Every step starts with all inputs released: a
stick or button held across steps must be given again at `0` (it is not released in between, so no new press edge
happens). `save`, `load`, `pause` and `pda` are ignored. A press at the scenario's very first tick is not an edge
(the game treats buttons as held on its first tick): press at `0.1` or later in the first step.

`plan` is the autopilot's syntax (`src/app/game/Autopilot.hpp`, docs/GAMEPLAY.md "Receptacle replays and the
autopilot"); it reads the World to steer, so it reaches places and objects by coordinates and uids:
`go X Y [R] [walk]`, `engage UID [X Y] [walk]` (a parked motorcycle's uid: to its mount spot), `face YAWDEG`, `press [ACTION]`, `hold ACTION SECONDS`,
`stick FORWARD RIGHT SECONDS [ACTION AT]`, `run YAWDEG SECONDS [ACTION AT]` (a world heading, whatever the camera
does), `push FORWARD RIGHT SECONDS [ACTION]`, `wait SECONDS`, `near R`, `map R [CELL] [UP]` (logs the floor round her as a grid), `ride X Y [R] [THROTTLE] [SPEED] [BRAKESTEER]` (on the
motorcycle: raw stick towards the point, throttle while slower than SPEED units/s, braking above 1.25 SPEED; while
braking the stick stays within BRAKESTEER, default 1: brake + stick past half is the power slide, so long descents
ride with 0.5), `swim X Y Z [R]` (in water: the stick towards the point, Jump / Drop held while it lies above / below
her, until within R, default 150, in space; 60 s). Coordinates are world frame. With both `plan`
and `input`, the plan steers and presses and the input lines add to it (a button is down if either holds it; a
non-zero stick or look rate from `input` wins).

When a step ends:

| step has | it ends | and fails outright when |
|---|---|---|
| `until` | on the first tick all `until` conditions hold | they do not hold within `duration` ("timeout") |
| `plan`, no `until` | when the plan's last line finished | a plan line fails (`go` / `engage` / `face` give up after 20 s, `engage` reaching the point without engaging) or the plan is not done within `duration` |
| neither | after `duration` | never |

A plan line failing always fails the step. The expectations are evaluated (and reported with their actual values) in
every case; the step passes when it ended properly and every expectation held.

### Conditions

A condition names a probe and one or more operators; all given operators must hold.

```json
{"probe": "player.speed", "over": "max", "near": 1274, "tol": 10}
```

| operator | holds when |
|---|---|
| `eq`, `ne` | the value equals / differs from a bool, number (within 1e-6 relative) or string |
| `min`, `max` | a number is >= / <= the bound |
| `near` + `tol` | a number is within `tol` of `near` (`tol` is required) |
| `in` | the value equals one of the array's entries |
| `seq` | a list contains these strings in this order (other entries may lie between): an ordered subsequence |
| `has` | a list contains every one of these strings; a string equals one of them |

A value of the wrong type never satisfies an operator, and an unavailable probe (see below) satisfies none, not even
`ne`.

`over` says which of the step's ticks the operators are tested on (expectations only; `until` is tested every tick):

| `over` | tested on | `actual` in the report |
|---|---|---|
| `end` (default) | the state when the step ends | that value |
| `max`, `min` | the largest / smallest number seen after any tick of the step | that extreme |
| `any` | holds at one tick at least | the first value that held, else the last |
| `all` | holds at every tick | the first value that did not, else the last |

Shorthands over the step's lists: `{"visits": [...]}` = `{"probe": "step.states", "seq": [...]}`,
`{"clips": [...]}` = `{"probe": "step.clips", "seq": [...]}`, `{"calls": [...]}` = `{"probe": "step.calls",
"has": [...]}`.

## Probes

Positions are world frame (unit frame + the unit's streaming offset) unless noted; angles are degrees in -180..180;
speeds units/s; 3.1605 units = 1 cm. "Unavailable" probes report `null` and fail their expectation.

### Lara

| probe | value |
|---|---|
| `player.state` | `game::stateName`: `idle walk run stop turn jumpprep jump fall land hang shimmy climbup swimsplash swimtread swimsurface swimdive swimunder surfacing swimclimbout dead crouch roll slide beam droptohang hangjump polehang climb ladder perch pillar grapplethrow grappleswing hit railslide grapplepull vehicle grapplerappel use block wallhug swimdodge` |
| `player.clip` | the animation clip she plays (`ng_run`, `ledgeb_idle`, `motorcycle:motorcycle_ride_fast`, ...) |
| `player.clip_time` | seconds into it (unbounded for loops) |
| `player.pos.x` `.y` `.z` | her root (feet; the swim clips' root in water) |
| `player.unit_pos.x` `.y` `.z` | the same in the current unit's frame |
| `player.yaw`, `player.pitch` | heading; swim pitch |
| `player.vel.x` `.y` `.z` | the controller's air velocity (`PlayerState::vel`; meaningful in the air and on the rope) |
| `player.speed`, `player.vspeed` | measured: horizontal / vertical root displacement of the last tick x 60 |
| `player.grounded` | on her feet: state is one of idle, walk, run, stop, turn, jumpprep, land, crouch, roll, slide, beam, use, block, wallhug |
| `player.airborne` | state is jump, fall or hangjump |
| `player.swimming`, `player.hanging`, `player.dead` | a swim state; hang or shimmy; dead (state or health <= 0) |
| `player.health`, `player.health_max` | health points (1000 at TOMB RAIDER level) |
| `player.air` | breath 0..1 |
| `player.adrenaline` | meter 0..1 |
| `player.sprint_used`, `player.sprint_left` | sprint stamina spent / left, seconds (9.7 full) |
| `player.saving_grab` | seconds left of a one-handed saving grab; 0 = a firm hang or not hanging |
| `player.ledge` | markup id of the ledge / pole / beam / ladder she is attached to (a hang jump: its target), -1 none |
| `player.fall_height` | the fall's height on the tick she lands, else 0 |

### Pose: limb locks, head look-at, bones

The passes of `World::limbLocks` on the last tick (docs/GAMEPLAY.md "Animation layers and IK").

| probe | value |
|---|---|
| `ik.feet`, `ik.hands`, `ik.look` | the pass ran: on her feet on an object floor; hanging in `ledgeb_idle`; a combat target with the weapon out |
| `ik.foot_planted.l` `.r` | the probe found a floor for that foot (and the clip has not lifted it out of reach) |
| `ik.foot_error.l` `.r` | the ankle's height after the pass minus the height the pass wanted, units; unavailable when not planted |
| `ik.pelvis` | how far the pelvis was lowered for the lower foot, units |
| `ik.hand_before.l` `.r`, `ik.hand_error.l` `.r` | the hand's distance from the ledge's polyline before / after the pass; unavailable when not hanging |
| `ik.look_yaw`, `ik.look_pitch` | the head's turn off the neck (+ = her left / up), within 60 / 15; unavailable without a target |
| `ik.second_arm` | degrees her left arm was swung to a second target (0 without one) |
| `pose.bone.<segment>.x` `.y` `.z` | the origin of that bone of Lara's skeleton in the world (6 / 11 ankles, 55 / 80 hands, 17 head, 1 pelvis) |

### Weapons and combat

| probe | value |
|---|---|
| `weapon.current`, `weapon.secondary` | `pistols uzi shotgun speargun thorhammer rifle tranquilizer` |
| `weapon.drawn` | in hand (or being drawn) |
| `weapon.clip`, `weapon.reserve` | rounds in the current weapon's gun(s); spare rounds |
| `weapon.<name>.clip`, `weapon.<name>.reserve` | the same of the named weapon (`uzi`, `shotgun`, ...), in hand or not |
| `weapon.grenades` | sticky grenades carried |
| `weapon.shots_fired`, `weapon.health_packs_used` | counts since the level start |
| `combat.has_target`, `combat.locked` | a soft or hard target; a hard lock |
| `combat.target` | the target: an enemy's intro uid; `-(n + 1)` for the n-th `target` fixture; 0 none |
| `combat.target_health` | its health; unavailable without a target |
| `combat.adrenaline_mode`, `combat.headshot_window`, `combat.time_scale` | adrenaline mode on; the head-shot window open; the world time scale (1 normal) |
| `combat.manual_aim`, `combat.headshot`, `combat.headshot_ready`, `combat.second_target`, `combat.world_scale` | manual aim view on; the head-shot move running; the meter latched full (head shot offered on Grapple); the left gun has its own target; the scripts' world-only scale |
| `enemies.count`, `enemies.alive` | enemies of the loaded units; those alive |
| `enemies.nearest`, `enemies.nearest_uid` | distance to / uid of the nearest living enemy (unavailable / 0 without one) |
| `enemy.<uid>.exists` `.alive` `.health` `.mode` `.clip` `.type` `.distance` `.pos.x/.y/.z` | one enemy by intro uid; mode is `ai::modeName` (`idle alert chase attack hit dead ...`). All but `exists` are unavailable while it is not spawned |
| `target.<n>.health` `.hits` `.alive` | the n-th `target` fixture (from 0) |

### Grapple, water, motorcycle

| probe | value |
|---|---|
| `grapple.attached` | the rope is out (state grapplethrow / grappleswing / grapplepull / grapplerappel) |
| `grapple.state` | that state's name, `none` otherwise |
| `grapple.target` | the anchor's intro uid, 0 when not attached |
| `grapple.offered` | the target a press would throw at now (`World::grappleTarget`), 0 none |
| `grapple.rope`, `grapple.rope_max` | rope length anchor -> hand; the longest this target allows (unavailable when not attached) |
| `grapple.kind` | `ceiling wall post mounted` |
| `grapple.bends` | bends of the rope round geometry |
| `grapple.swinging`, `grapple.pulling` | hanging on the rope; pulling a mounted target |
| `grapple.on_wall` = `grapple.rappel` | feet on the wall on the rope (state grapplerappel) |
| `grapple.wall_run` | running along the wall: grapplerappel playing `ng_ground_to_wallrun_*` or `grapple_rappel_left/right*` |
| `water.in`, `water.under`, `water.air` | in a water volume or swimming; head under water; breath 0..1 |
| `bike.count`, `bike.mounts` | bikes known to the World; mounts so far |
| `bike.mounted` | Lara is mounting, riding or leaving a bike. The probes below are unavailable when she is not |
| `bike.uid`, `bike.phase` | its intro uid; `parked mount ride dismount` |
| `bike.speed` | along its heading (< 0 reversing) |
| `bike.airborne`, `bike.wheels` | no wheel on the ground; wheels touching the ground as of the last physics step (0..2) |
| `bike.steer` | front wheel angle, + = left |
| `bike.stunt` | `none burnout powerslide donut` |
| `bike.throttle`, `bike.rpm`, `bike.brake_time` | engine state; seconds the brakes have been on |
| `bike.yaw`, `bike.pitch`, `bike.roll`, `bike.pos.x/.y/.z` | its attitude; Lara's root on it |

### Camera, world, objects

| probe | value |
|---|---|
| `camera.yaw`, `camera.pitch` | the orbit angles |
| `camera.distance` | the orbit camera's distance from its pivot |
| `camera.mode` | tru.exe's camera mode number (1 on foot, 4 free climb, 11 hang, 19 / 20 under water / surface; -1 before the first update) |
| `camera.spline`, `camera.field` | a scripted reveal camera plays; the field camera view is on |
| `camera.pos.x/.y/.z` | camera position |
| `world.unit`, `world.units` | the unit Lara is in; the loaded units (list) |
| `world.chapter` | `game::kChapters` index |
| `world.checkpoint`, `world.checkpoint_unit` | the last checkpoint's intro id (0 none) and its unit |
| `world.treasures`, `world.relics`, `world.health_packs` | pickups collected per kind |
| `world.ticks`, `world.streaming`, `world.autosaves` | the World's tick clock; neighbour loads pending; autosaves the scripts asked for |
| `world.flashlight`, `world.screen_fade` | the light is on; the screen wipe (1 = black) |
| `portable.held` | uid of the portable she carries in her hands, 0 none |
| `portable.stowed` | how many key items are stowed on her (docs/GAMEPLAY.md "Key items are stowed") |
| `object.<uid>.stowed`, `object.<uid>.unit` | that object is stowed on her; the unit the instance lives in (it changes unit with her while she carries it; a bike's is the unit it stands in) |
| `portable.<uid>.in`, `object.<uid>.in` | the receptacle that portable is inserted in, 0 none |
| `object.<uid>.exists` `.name` `.hidden` `.dead` | a script object by intro uid (objectlist name; instance hidden / killed) |
| `object.<uid>.pos.x/.y/.z` | its position with its own displacement (a pushed block, a fallen body) |
| `object.<uid>.body.x/.y/.z` | the translation of its own rigid motion from rest (doors on their joints, sliders) |
| `object.<uid>.graph` | its anim-graph state id |
| `object.<uid>.joint` | its multibody joint's position (plate units, hinge degrees) |
| `object.<uid>.uber.<n>` | uber section n's state, -1 while it is in a transition |
| `object.<uid>.uberjoint.<n>` | simulated uber joint n's position (units; a hinge: radians) |
| `object.<uid>.uberslide.<n>.x/.y/.z` | three-axis uber joint n's travel from where it was made (the Thor pillars) |

These object probes are what `--report-objects` prints (docs/GAMEPLAY.md); `--report-objects 0 --frames 1` lists every
script object of a level with its uid.

### The step's measurements (`step.*`)

Recorded for every step and written to the report whether or not anything expects them.

| probe | value |
|---|---|
| `step.start.x/.y/.z`, `step.end.x/.y/.z` | Lara's root when the step started / ended |
| `step.moved` | path length travelled |
| `step.distance`, `step.horizontal` | straight line start -> end, in 3D / in the plane (jump distance) |
| `step.height_gain` | end z - start z |
| `step.peak_height`, `step.drop` | highest z - start z (jump height); start z - lowest z |
| `step.peak_speed` | highest horizontal speed |
| `step.peak_rise_speed`, `step.peak_fall_speed` | highest vertical speed up / down |
| `step.air_time`, `step.air_ticks` | time airborne (states jump, fall, hangjump) |
| `step.ticks`, `step.seconds` | the step's length |
| `step.teleports` | relocations: ticks that moved her more than 600 units, and respawns (leaving the dead state). They are not counted as motion |
| `step.health_start`, `step.health_end`, `step.health_min`, `step.health_lost` | health; lost = start - min |
| `step.states` | the states she was in, in order (a state re-entered is listed again) |
| `step.clips` | the clips she played, in order |
| `step.calls` | script virtual calls made during the step as `"<callee uid>:<vt>"`, first-call order (`-1` = constructor; negative callees are unit / global scripts) |
| `step.events` | script events as `"<kind>:<source>><target>"` (`enter`, `in`, `out`, `activate`, ...), first-seen order |
| `step.pickups` | pickups collected as `"<kind>:<id>"` (`treasure`, `relic`, `healthpack`) |
| `step.hazards` | contact-hazard hits that hurt her as `"<object name>:<damage>"`, one entry per hit in order (`ca_ei_flame_trap:800`; docs/GAMEPLAY.md "Hazards") |

Speeds are measured from her root's displacement per tick, so clip-end root snaps show up in `peak_*_speed` (a
climb-up's last tick moves the root ~340 units): prefer `player.speed` at the end of a step, `step.horizontal` and
`step.air_time` for numbers that are compared with the original.

### Not available as probes

- What a collision was with (the contact's face or object): only its effect on position, speed and state can be tested.
- Per-wheel contact (which wheel); only the count `bike.wheels`.
- Loose rigid bodies that are not script objects (debris, ragdoll bones) and an object's rotation (`body` is its
  translation only; hinged doors are read through `object.<uid>.joint`).
- Health packs carried (only collected and used counts), the checkpoint's position, the camera mode's name.
- Enemy internals beyond mode / health / clip (its target, cooldowns), melee state, the adrenaline head-shot pip.
- HUD, sound and music state (the mixer is not inspected; scenarios run with `--no-audio`).

## Fixtures

A fixture is explicit test set-up: an object with one key. Every fixture applied is listed in the report's `fixtures`
(with the step it belongs to), so no result silently depends on one. Use them to set a scene up, not to make the
tested behaviour happen.

| fixture | effect |
|---|---|
| `{"weapon": "shotgun"}` | that secondary with its full reserve (`World::giveWeapon`); `thorhammer` adds Mjolnir |
| `{"ammo": {"weapon": "shotgun", "clip": 2, "reserve": 10}}` | rounds per gun and spare rounds (the weapon is holstered) |
| `{"health": 250}` | her health |
| `{"adrenaline": 1.0}` | the meter 0..1 |
| `{"give": "wp_thorhammer"}` | a carried object (Thor's gear: `g_belt`, ...) |
| `{"target": [x, y, z]}` or `{"target": {"pos": [x, y, z], "health": 700}}` | a standing target dummy (capsule r 100, h 500, 700 hp, enemy team) at that point of the current unit's frame; probes `target.<n>.*` |
| `{"tailoring": 0}` | PLAYER TAILORING preset 0..2 |
| `{"kill": true}` | kills her (message 4) |
| `{"teleport": [x, y, z, yawDeg]}` | puts her there (current unit's frame) standing idle, exactly at z: in the air she falls. The camera goes behind her; health is kept. Same loaded units: for another level use `level` |
| `{"camera": [yawDeg, pitchDeg]}` | turns the orbit camera |
| `{"level": ["unit", checkpoint]}` | a level load at that unit's checkpoint with the game's state kept (`World::enterUnit`, as a respawn makes one): stands for a way between two units the scenario does not play. What she carries goes with her; health is kept |

## Report

```json
{
  "version": 1,
  "game": "Tomb Raider: Underworld",
  "scenario": "ledge",
  "chapter": "Core mechanics: ledge grab, shimmy, pull up",
  "file": "tests/selftests/ledge.json",
  "entry": {"level": "l3_main_bedroom", "spawn": [-8484.3, -4330.0, 244.0, -90.0]},
  "fixtures": [{"step": null, "fixture": {"health": 1000}}],
  "steps": [
    {
      "step": "jump_and_grab",
      "passed": true,
      "detail": "3 of 3 expectations held after 62 ticks (ended by until)",
      "ended_by": "until",
      "measurements": {
        "start": [-8484.3, -4330.0, 244.382], "end": [-8364.862, -4323.605, 311.65],
        "moved": 173.281, "distance": 137.23, "horizontal": 119.609, "height_gain": 67.268, "peak_height": 89.674,
        "drop": 0.0, "peak_speed": 449.757, "peak_rise_speed": 918.461, "peak_fall_speed": 724.838,
        "air_time": 0.267, "air_ticks": 16.0, "ticks": 62.0, "seconds": 1.033, "teleports": 0.0,
        "health_start": 1000.0, "health_end": 700.0, "health_min": 700.0, "health_lost": 300.0,
        "states": ["idle", "jumpprep", "jump", "hang"],
        "clips": ["ng_idle", "ng_standing_jump", "NG_InAir_Feet_Forward_stage1_standing", "ledgeb_attach_M", "ledgeb_idle"],
        "calls": ["4538:73"], "events": ["enter:4478>4478"], "pickups": []
      },
      "end": {"player.state": "hang", "player.clip": "ledgeb_idle", "player.health": 700.0, "world.unit": "l3_main_bedroom",
              "grapple.state": "none", "bike.mounted": false},
      "expectations": [
        {"probe": "step.states", "expected": {"seq": ["idle", "jumpprep", "jump", "hang"]},
         "actual": ["idle", "jumpprep", "jump", "hang"], "passed": true},
        {"probe": "player.ledge", "expected": {"eq": 973}, "actual": 973.0, "passed": true},
        {"probe": "player.saving_grab", "expected": {"eq": 0}, "actual": 0.0, "passed": true}
      ]
    }
  ],
  "ticks": 62,
  "sim": "9f3c0a1d5e7b2c44",
  "passed": true
}
```

- `passed` (top): every step ran and passed. A step: it ended properly and every expectation held.
- `detail`: how many expectations held, after how many ticks, what ended the step (`until`, `plan`, `duration`,
  `timeout`), then why it failed: the timeout / plan failure and the first failed expectation with expected and actual.
- `ended_by`, `measurements`, `end` (a fixed set of probes at the step's end: state, clip, health, breath, yaw, unit,
  checkpoint, grapple, bike, weapon, target, enemies, portable) and `expectations` (probe, `over` when not `end`,
  expected operators, actual value, passed) are there for every step that ran. A step that did not run has
  `"skipped": true` and says why in `detail`.
- Numbers are rounded to 3 decimals in the report (the checks use the exact values).

## Shipped scenarios

| scenario | level | what it proves (measured) |
|---|---|---|
| `locomotion` | l3_main_bedroom | idle; walk 348 and run 1274 units/s; stopping on the spot inside the run loop's first 15 frames [O Wine]; standing jump 201.6 high / 0.56 s, stick jump 218.1 / 0.58 s, running jump 157.1 / 0.50 s at 1974.9 units/s [O Wine] |
| `sprint` | l4_trailstart | sprint_loop 9.7 s, the exhausted clips, ~2.2 s until a 0.9 s second sprint, refill at rest [O Wine] |
| `fall_damage` | l4_pool | falls of 1000 / 1500 / 2000: air time, landing, 0 / 204.7 / 729.4 damage [O curve] |
| `jump_brake` | l4_temple | a running jump with Jump tapped / held: the same arc; the stick pulled back in flight: 40 % of the take-off speed [O Wine] |
| `landing` | l4_temple | landings by the chart's dispatch: plain, running on, the stumble from 632.11, the hard landing from 1264.22, the Crouch + forward roll, a jump out of the landing clip [O chart] |
| `jump_timing` | l3_main_bedroom | press to leaving the ground: standing 10 frames, with the stick 8 frames and 128 along the ground, running 2 frames; the stick jump just after the run began; the run's start (303 units in 0.4 s) [O Wine] |
| `death_respawn` | l4_pool | checkpoint 5502 touched; a 2600 fall kills; 3 s later she stands at the checkpoint with full health |
| `ledge` | l3_main_bedroom | standing jump catches ledge 973; shimmy left and right; one Interact hustle move (371 in 0.57 s); pull up 496 onto the top |
| `traversal_chain` | l2_cliffclimb | run -> jump -> hang (911) -> shimmy -> climb up -> run on, as one state chain |
| `ladder` | l1_ship_sinking_hall | attach from the ground (234), climb up, climb down, step off |
| `ladder_fast` | l7_arctic_sea | ladder 2044: a rung per ladder_climb_up; Interact + up: ladder_climb_up_fast, four rungs at once, again on a second press [O chart] |
| `pole` | l4_traprooms | standing jump to pole 1336, swing, fling 1264 to pole 1337 in 23.5 frames [O Wine] |
| `pole_drop` | l4_traprooms | letting go of pole 1336: detach clip, fall, landing |
| `melee_kick` | l1_baldrs_tomb | Interact kicks a target dummy once for the kick record's 60 (melee_kick_high); the mid kick at the air |
| `urn_kick` | l1_baldrs_sideroom | Interact by urn 94 kicks it to pieces; its treasure is picked up |
| `carry_throw` | l1_baldrs_tomb | weight 220 picked up, carried at a walk, thrown from plate 219 through door 223; weight 218 put down with Crouch |
| `balance_beam` | l4_temple | beam 1079: walk 140 units/s on its line, jump along it, its far end, losing balance into a ledge catch (1136) |
| `grapple_swing` | l3_chapel | ring 4744 offered, throw, attach, Jump into the gap keeps the rope (hangs on 3632), no stick: the swing dies down, Forward held: it builds (peak height > 1000), Interact + forward / back climbs 711 / slides to 4400, jump off (1896 units/s), landing |
| `prologue_hall_swing` | l3_main_hall | ring 4089 from the balcony: roped walk to the edge, Jump with the rope, swing over the burning staircase, leap off onto the far balcony (x < -2150, z 1716) |
| `prologue_ring_door` | l3_main_bedroom | ring door 4526: throw onto its ring (mounted target), the stick alone walks her back roped and moves nothing, Interact held draws the leaf along its slide until it leaves the watched range (uber section 1: 0 -> 1 -> 2), the ring is no target and she lets go |
| `rappel` | l1_kraken_room | wall anchor 326: swing onto the wall, the stick alone leaves the rope, Interact + back / forward rappels down / up (1327 / 1043 units/s), Jump leaps back off the wall and lets go |
| `slide` | l1_kraken_room | the 0x0004 ramp into the lower hall: stands on its flagged flat top, the run onto the 44.6-degree face starts the slide (`slope_transitionto`, `slope_slidefront`), 1262.7 units/s along the slope (899 horizontal, 887 down), the run-out flatter than 28 degrees ends it (`slope_slide_stumble_off`), running back up is thrown back by the slide face [O tru.exe 0x5DCE20 / 0x73FF50] |
| `wall_run` | l5_valaskjalf | wall anchor 6750: camera behind her, right runs onto the wall and through the rope's arc over the pit, stepping onto the far ledge, rope let go |
| `swim` | l1_seatop | drop into the sea, surface swim 1348.6 units/s, dive, under water (on the wetsuit's tanks the breath stays full), surfacing |
| `swim_breath` | l2_cliffclimb | no tanks: the breath drains under the river, 1.5 times as fast sprinting, the meter refills in 2 s at the surface |
| `swim_moves` | l1_seatop | surface turn on the spot, dive, under-water dodges up / down by their clips, pistols drawn and fired under water |
| `swan_dive` | l2_cliffclimb | running jump + crouch off the cliff top: swan clips, 5900 down into the sea on `swim_swan_land`, no damage |
| `puzzle_pole_block_door` | l4_ballcourt | pole 6503 into lever 6502 (graph 0 -> 1), push block 6504 under it (two pushes), up onto the block, jump to the pole: lever joint slides, door 6505 rises |
| `push_block` | l4_ballcourt | push block 6504 gripped by its push / pull record: pushed 7 s along her facing (33.2 units a second, her pull strength 1.4), pulled back, moved sideways (80 degrees off forward); the three-axis joint's travel, her obj_block clips |
| `gauntlet_move` | l2_maproom | Thor door 2661: Interact + stick bare-handed moves nothing; with `g_gauntletleft` given, Interact grips its handle (`thor_engage`) and a sidestep slides it to its limit 663.7; let go: it stays, braked |
| `combat` | l2_cliffclimb | draw, hard lock, pistols kill three spiders and hit a 700 hp target dummy 8+ times, adrenaline fills |
| `motorcycle` | l4_trailstart | mount bike 5482, accelerate (wheels on the ground), steer left, brake and hold it with the Vehicle Brake (Space), dismount |
| `bike_ride` | l4_trailstart | Southern Mexico's first ride by autopilot `ride` lines: bike 5482 over the stair hump, down the valley, through the stone tunnel, off its ramp over the pit (no wheel down, landing beyond x 67700, no damage), to checkpoint 6620 |
| `chimney_floor_jumps` | l2_junglehub | at column A's unflagged face: a standing jump is a plain jump (no wall jump, apex 201.6); a stick jump plants her feet on the wall ~43 over the floor, and with no press she comes down again (both as tru.exe under Wine, docs/REFERENCE_COMPARISON.md M06) |
| `pole_drop_after_fling` | l4_traprooms | the fling 1336 -> 1337, then Drop: `Hpole_detach_dropPerp`, down 27 beside the rubble mound under the pole, standing within 1.2 s (not caught again by pole 1337) |
| `moving_markup` | l5_valgrind | by cog wheel 7312's rim (teleport fixture: the gate cinematic's placement is 20 s away on foot) a standing jump catches a passing rim ledge (markup 1614-1617), 2 s carried at 267 units/s with no input, Drop: she falls on at the rim's speed and lands (docs/GAMEPLAY.md "Moving markup") |
| `saving_grab` | l5_valaskjalf | drop-down ledges 1488 -> 1485 -> 1484: each Drop is caught one handed 1422 / 1387 lower (`ledgeb_bad_attach_M`, 50 frames), Interact pulls her up, Jump does not, left alone she falls after 1.67 s |
| `safety_drop` | l4_temple | running off the high platform's east edge: `drop_to_ledgeb` without a key, hanging on ledge 1168 (root 499 down), then Drop to the floor 251 under her feet |
| `ch07_cliffclimb_boat_to_cliff` | chapter 2 start | section 07's start: yacht cinematic, over the boat's side (safety drop), into the sea, 13.8 km surface swim across the bay, climb out at the low ledge (checkpoint 3831), the run into the rock attaches her to the holds |
| `ch07_cliffhall_grapple_gap` | l2_cliffhall | from checkpoint 3754 to the pit under ring 3820: thrown at from the ground (rope 2730), a running jump keeps the rope, the swing carries her over the pit; the release onto the far side is not played yet |
| `ch07_hubhall_statue_hall` | l2_hubhall | section 07's end from checkpoint 3100: up the staircase, stream change to l2_puzzleroom, arrival cinematic 2936, on the lift |
| `ch08_puzzleroom_lift_switch` | l2_puzzleroom | section 08's start on the lift: switch 2961 used with the mechanism held: the jammed reaction, the platform stays |
| `ch08_puzzleroom_lift_ring` | l2_puzzleroom | from checkpoint 2819: the mechanism's ring 2972 grappled and pulled out (2959 vt 54, 2961 vt 63) |
| `ch08_puzzleroom_lift_pole` | l2_puzzleroom | from checkpoint 2818: pole 2967 pulled out of hole 2969 and carried (2960 vt 54, 2961 vt 62) |
| `ch08_shrine_statue_pull` | l2_shrineroom | the naga statue 2794 grappled by its ring and pulled over (uber section 0: 5 -> 2), its script wakes the shaft's spiders |
| `ch09_maproom_gauntlet_door` | l2_maproom | from under the shaft: volume 2636 -> cinematic 2624, the scripts hand the left gauntlet over (no fixture), door 2661 gripped and slid open by a left sidestep, through it (spider trigger, checkpoint 2590), the pedestal cinematic 2625, checkpoint 2591 |
| `ch09_maproom_hub_panel` | l2_maproom | from checkpoint 2591: off the platform edge into a hang, drop-catches down the hub's panel (ledges 673 / 674 / 676 / 678), the ring ledge, the panel gripped with the gauntlet (2667 vt 49 / 50) |
| `ch09_maproom_hub_turn` | l2_maproom | as `ch09_maproom_hub_panel`, then the hub turned by its panel: forward does nothing, a right sidestep walks the panel round hinge joint 1 to its stop at 41 degrees (0.716 rad; the record sets swap at 20 degrees and she keeps her grip), braked when she lets go |
| `ch01_bedroom_new_game` | New Game | Prologue leg 1: the corridor onto the debris slab without a jump, wall 973, the stairs, switch 4525 opens house door 4520, down to the bedroom floor (checkpoint 4454) |
| `ch01_bedroom_ring_door` | l3_main_bedroom (chapter 0) | Prologue leg 2: a roll under the leaning beam (a walk does not pass), onto the flat strip, ring door 4526 grappled and pulled off its slide |
| `ch01_hall_first_aid` | l3_bedroom_hall (chapter 0) | the first-aid kit 4364 picked up from checkpoint 4342, stored, used by the heal key |
| `ch01_hall_floor_collapse` | l3_bedroom_hall (chapter 0) | Prologue leg 3: plane 4375 holds her while floor 4359 collapses and door 4376 is blown off, running jump to ledge 969, pull up, into l3_main_hall (checkpoint 4082). The blown leaf takes the same knock whatever the tick she crosses the plane (the leg was parked while the leaf could come to rest on ledge 969's top, 58 in from the ledge, where it made the ledge clearance class 1 and Jump did not pull up) |
| `ch01_ring_door_walk_through` | l3_main_bedroom (chapter 0) | Prologue leg 2 to its end: as `ch01_bedroom_ring_door`, then the squib's knock has thrown the freed leaf into the bedroom and Lara off the strip; up onto the platform, through the open doorway into l3_bedroom_hall |
| `natla_lower_ring` | l7_yggdrasil_end | a level start on the lower ring: the finale pool's surface drops away at once and she falls back onto the ring (no ride down through the goo); 12 s between two floor targets cost nothing |
| `known_gaps/natla_platform_ride` (parked) | l7_yggdrasil_end | the entry, then a timed running jump from the walkway's end at a passing floating stone (rig 8389): fails at the grab, the ledge is 1240 out and 540 up (docs/GAMEPLAY.md "Natla") |
| `ch01_main_hall` | l3_main_hall (chapter 0) | Prologue leg 4: ring 4089 from the west balcony's end, the swing east, the leap onto the east balcony, the safety drop onto ledge 945, Drop 1251 to the floor, plane 4129 -> chapter 1 |
| `ch02_seatop_dive` | chapter 1 start | off the yacht's stern, dive, the look stick tips the view down, 35 s of sprint swimming on the tanks (breath 1.0 throughout) through the stream change to l1_seafloor |
| `ch02_serpent_door` | l1_kraken_entrance | the dead switch 748, serpent head 751 pulled onto plate 752, one weight on plate 753 does not hold it, two do, the switch opens gate 743 |
| `ch03_gear_room_west` | l1_kraken_room | switch 57 jammed; from the lower hall the pillar ring of gear 32 pulled: the column falls, the mechanism is free |
| `ch03_gear_room_east` | l1_kraken_room | switch 55 jammed; gear 36 pulled out until it locks; the switch runs the mechanism |
| `ch04_pillar_entry` | l1_baldrs_tomb | the Norse Connection's way into the west room: pillar 14 climbed to z 4100, `pillar_jump_forward` along the stick 740 into the wall opening (floor z 3872), the safety drop at its far edge, Drop into the room behind door 223 |
| `ch04_gate_room` | l1_baldrs_tomb | both weights thrown from pad 219 through door 223, a roll through before it shuts, one weight on pad 222 (the pad rises again, lock 69 back), both (it stays down), gate 45's ring roped, pulled down from pad 221 (joint 91), the rope let go, plane 224 at the gauntlet |
| `ch04_column_hall` | l1_baldrs_sideroom | the hall of broken columns' first half: pillar 24 -> pillar 23 (948, caught in the air) -> back onto ledge 36, running jump onto perch 26, `perch_jump_forward` to pillar 31, pillar jump onto the floor behind ledge 35 |
| `ch04_kraken_swing` | l1_kraken_room | upper level: ceiling ring 309 thrown at from the north-east ledge, roped walk to the edge, Jump, 4500 across the pool with Forward held, stepping off onto the west ledge; first-aid kit 319 |
| `ch05_boarding` | l1_ship_exterior | running jump off the yacht's bow onto the anchor, Forward + Jump from hang to hang up the chain (3400 up, never down), over the rail (boarding plane 1633), first-aid kit 1564 |
| `ch05_container_deck` | l1_ship_exterior | the boarding, then down the bow stair to the container deck; Target Lock + Fire held while walking aft: mercenary 1652 leaves his patrol and chases to the edge of his guard zone; she steps into the zone (2600, 16200) and shoots him, then 1650 (5 -> 3 alive) |
| `ch05_below_decks` | l1_ship_exterior | aft doorway: door 1529 by volume 1531, down to door 1530, generator 1537's mercenaries on the second stair (pistols on the lock), door 1491, plane 1465 -> cinematic 1453's events, the hall beyond to the T junction |
| `ch06_sinking_hall_door` | l1_ship_lower_hallway | volume 1176 opens door 1167 (a rider of the rolling hallway 1056), through it into l1_ship_sinking_hall, plane 1201: the hall's first tilt (1056 command 1), carried down on its floor |
| `ch06_second_shaft_valves` | l1_ship_sinking_hall | checkpoint 1055: ladder 235 rung to rung, `ladder_jump_up` from its top rungs onto ledge 250 (the grating's edge), the hang's jump up onto the valve wheels (hold 227, 491 up), hold to hold up the first group, the DICE jump up to the second, its weave to the top wheel 209 (root z 11059) |
| `ch10_maproom_rope_step_off` | l2_maproom | section 10 (C10-03): wall ring 2595 offered and thrown at from the near bridge stub's corner (rope 3404); roped she walks off the stub: the arc takes her under the pool, the rope is let go and she swims (surface root -9662); the far stub's spiders do not bite a swimmer under them |
| `ch11_chapel_dais_furnace` | l3_chapel | section 11 (C11-16): onto pressure plate 4834 (joint -50): fire 4818 lit, gates 4830 / 4831 opening; off it: the fire goes out, the gates shut |
| `ch11_study_gauntlet` | l3_stairhall -> l3_secretchamber | section 11 (C11-24, C11-25): from checkpoint 4699 down the passage into the study: checkpoint 4621, volume 4669, cinematic 4654 (gauntlet), two thralls alive in the room |
| `ch11_rockhall_switch` | l3_chamberrockhall | section 11 (C11-32): the stone door 4528 stops her; switch 4617 used with Interact opens it; through to checkpoint 4453 |
| `ch12_trailstart_ride_in` | l4_trailstart | section 12 (C12-03, C12-04): the ride of `bike_ride`, then the upper road: plane 6712 starts reveal 6677, which sets her down beside the bike (larabase 54); mount again, ride into the temple courtyard (volumes 6713, 6703) |
| `ch13_ballcourt_calendar_locked` | l4_ballcourt | Xibalba: at calendar 5477 without the jaguar head a gripped push handle turns nothing (hinge joint 2 stays 0, section 5 waits in state 3) |
| `ch13_temple_headdress` | l4_temple | Xibalba: from the pit floor's checkpoint 4971 to the altar, the headdress 5228 taken out of its holder 5249 |
| `ch13_ballcourt_pole_gate` | l4_ballcourt | Xibalba: `puzzle_pole_block_door`'s six steps, then on the pole the turn across it, the swing, the fling to the gate (1550), through the doorway into calendar 2's court (cinematic 6388) |
| `ch13_mask_to_calendar` | l4_trailstart → l4_ballcourt | the jaguar head 6604 taken from its pedestal (stowed by the pick-up clip), a level load to checkpoint 5478 (the `level` fixture stands for the ride), set on calendar 5477's jaguar by one Interact (section 5: 3 -> 4 -> 5, vt 54), the jaguar handle then turns the ring |
| `ch14_traprooms_bike_arrival` | l4_traprooms | Midgard Serpent: the chapter's motorcycle 5482 (born in l4_trailstart) stood on marker 5704 by the arrival cinematic's handlers, its instance living in her unit; walked to, mounted with Interact, ridden across the hall |
| `ch14_traprooms_club_throne` | l4_traprooms | Midgard Serpent: arrival cinematic 5703, club 5883 picked up and set into throne 5891 (section 0: 1 -> 2) |
| `ch14_traprooms_crusher_house` | l4_traprooms | Midgard Serpent: the rattling house from checkpoint 5787: wait for the first pair of jaws to open, sprint through all six pairs unhurt (1927 units/s), key 5879 taken |
| `ch14_ragnarok_hall` | l4_ragnarok_room | Midgard Serpent: down the carved hall (cinematic 5675, checkpoint 5669), through the doorway: stream unit l4_pool |
| `ch14_pool_entry` | l4_pool | Midgard Serpent: down the entrance stairs (5547 preloaded, serpent head 5612 told), 198_mex_thralls at the stair foot, the jaguar thrall alive |
| `ch15_ragnarok_xibalba_key` | l4_ragnarok_room | Land of the Dead: the Xibalba key 5695 taken out of its holder at the rock fall and stowed |
| `ch15_xibalba_key_to_exit` | l4_ragnarok_room → l4_ballcourt | Land of the Dead: the key taken (stowed), a level load to checkpoint 6122 (the `level` fixture stands for the five-unit ride), one Interact at keyhole 6501 sets it in (6501 vt 20 -> switch 5469 vt 62), the switch pulled: reveal 5464 vt 54, open (graph 7) within 200 s |
| `ch15_ragnarok_rubble_block` | l4_ragnarok_room | Land of the Dead: block 5660 refused bare-handed; with the belt gripped and slid 1264 to its joint's limit in 1.8 s (the fitted push law [I]; 8.3 s under the earlier constant speed) |
| `ch16_tunnel_door` | chapter 5 start | Gate of the Dead leg 1: on bike 7229, the tunnel's 66 000 units by `ride` lines (10 230 down, no damage; brake steer 0.5), hand brake, off; Thor door 7400 gripped and walked open to 663.7; back on, through the doorway: plane 7472 -> 235_val_shaft, on foot at its end, the bike waiting on marker 7404, mounted there |
| `ch16_tower_power` | l5_valgrind | Gate of the Dead leg 3: the power blocks 7498 / 7499 / 7500 carried into cog wheel 7312's niches; the rings 7492-7509 stand until the third is in, then turn 10 degrees/s, neighbours opposite |
| `ch16_tower_base_panel` | l5_valgrind | Gate of the Dead leg 4 (C16-15): after the three power blocks cog wheel 7312's panel gripped (`thor_engage`, facing the tower's axis), the sidestep right (`thor_right`) turns the base from 89 degrees off with her carried round; at the range watch -2..2 degrees the wheel's solved state brakes it against her push and its event lights glyphs 7330 and beam 7331 (calls `7330:55`, `7331:55`) |
| `ch17_maze_to_relic_door` | l5_valaskjalf_entry | Valhalla: the maze's first half on foot from inside the gate to the relic-room door 7227 (41 000 units, 2530 down), which is shut and stays shut |
| `ch18_bow_deck` | chapter 6 start | Rituals Old leg 1: Change Weapon once = the secondary, twice = Mjolnir; a bolt; nobody on the bow, the first soldiers once she heads down to the container deck |
| `ch18_engine_hall_to_cell` | l6_ship_engine_hall | Rituals Old last leg: with the hammer from checkpoint 7557 through the hall firing on the run, volume 7566, plane 7565 -> 270_shp_escape -> chapter 7 in l7_thin_ice, unhurt |
| `ch19_thin_ice_dive` | chapter 7 start | section 19 (C19-01, C19-02): cinematic 9071 leaves her swimming under the ice in l7_arctic_sea (checkpoint 9094); the unit's constructor sets the puzzle up (wheels 9095 / 9096 vt 62, 9098 / 9103 vt 63); 63 s of diving on the tanks to the front of the building, breath 1.0 throughout (autopilot `swim` lines) |
| `ch19_arctic_sea_stone_niche` | l7_arctic_sea | section 19 (C19-17, C19-18), swimming: the locked drawbridge wheel 9103 refuses (chains 9114 / 9115 rattle, bridge 9116 stays up); power stone 9273 lifted, carried to niche 9120 and seated (9120 vt 56, rune strip 9137 lit, the unit counts a key); with one stone the wheel still refuses |
| `ch19_arctic_sea_tower_stone` | l7_arctic_sea | section 19 (C19-13): power stone 9270 lifted off its carving (holder 9276) on the tower's top ledge, carried at a walk, put down |
| `ch19_helheim01_entry` | l7_helheim01 | section 19 (C19-26): up the stairs into volume 8968, cinematic object 8950, checkpoint 8950; over the entry ledge to its edge over the eitr (a hang) |
| `ch20_helheim01_rune_symbols` | l7_helheim01 | section 20 (C20-02, C20-08): rune symbol 8995 on the entrance frame, 480 over the ledge, used with Interact through its section's kind-8 record: section state 1 -> 0, dispatcher 8997 vt 65; one symbol of four leaves eitr fall 8998 running |
| `ch20_helheim01_portal` | l7_helheim01 → l7_helheim02 | sections 19 / 20 (C19-33, C20-13): the portal ledge: volume 8969, cinematic object 8951 opens door 8956; through it into l7_helheim02 (checkpoint 8884) |
| `ch20_helheim02_passage` | l7_helheim02 → l7_helheim03 | section 20 (C20-13, C20-14): the passage to its crossing, the east arm's stairs, the slide down the broken stairs into the thrall hall |
| `ch20_yggdrasil_hall_slide` | l7_yggdrasil → l7_yggdrasil_hall01 | section 20 (C20-19): up the hall's north arm, the slide down its 30-degree slope (921 units/s), Jump on its last tick = `slope_jumpF`'s query q3 (2086 forward + 1043 up): over the eitr pit, the far bank's edge caught 1135 on, pull up |
| `ch20_yggdrasil_hall_eitr_pit` | l7_yggdrasil → l7_yggdrasil_hall02 | section 20 (C20-19): the same slide without the jump: off its foot into the pit, water volume 8742's eitr kills her as her feet go under |
| `ch20_helheim03_hall` | l7_helheim03 → l7_yggdrasil_hall01 | section 20 (C20-14 … C20-17): slide into the corridor, Mjolnir (Weapon twice, Draw), a bolt kills Viking thrall 8777 for good; the run through the hall and down the stairs into volume 8804: cinematic object 8773, checkpoint 8775 |
| `ch20_yggdrasil_reveal` | l7_yggdrasil → l7_yggdrasil_hall01 | section 20 (C20-17, C20-18): volume 8804 from checkpoint 8792, cinematic object 8773 leaves her at checkpoint 8775; the thralls after it met with the hammer |

`tests/selftests/known_gaps/` holds scenarios for behaviour that does not work yet; they fail today and are not
registered with ctest. Run them by hand; move one up a directory when it passes. Now (`pole_drop_after_fling` moved up 2026-11-10):

| known gap | level | fails at | cause |
|---|---|---|---|
| `ch10_maproom_wall_run` | l2_maproom | `leap` | the roped jump towards wall ring 2595 swings her under the pool's surface (rope 2670, ring 1200 over the bridge, water 870 under it), where the rope is let go: what shortens the original's rope in that jump is not decoded [U]. The later steps (reel to 1500, wall run, back jump to ledge 652, pull up) pass once she is on the wall |
| `ch12_trailstart_lever_gate` | l4_trailstart | `let_go` | lever 6695 is pulled out by Interact and stays out (joint 1264); the original holds it by her weight on the rope and it returns when unloaded |
| `ch04_column_hall_perches` | l1_baldrs_sideroom | the jump to perch 30 | ledge 35 -> perch 30 is 1620 away and 134 higher; the running jump comes down on pillar 29. The original's take-off spot is unknown |
| `ch05_natla_room` | l1_ship_exterior | the way west | from the T junction the west recess is shut; volume 1436 is not reached on foot |
| `ch06_hallway_roll` | l1_ship_sinking_hall | after plane 1202 | after the hall's second tilt she is stopped on a 45-degree floor against a 100-high step, 3850 short of plane 1203 |
| `ch06_second_shaft_exit` | l1_ship_sinking_hall | the jump to bar 238 | from the top valve wheel the bar is not reached: the side jump lands hanging on the beam's own ledge 253 |
| `ch17_hammer_chamber` | l5_valaskjalf | the central steps' lower flight | from checkpoint 7067 a running jump catches the chamber block's edge 1542, 790 over the floor; its top is the steps' ramp (1 in 2), 69.5 up at the 139 over the top where tru.exe 0x72BAE0 wants -30 .. 40: clearance class 1, no pull-up by the original's rule (`Game.HangPullUpWinsOverTheJumpUpTarget` pins the classes). The gap is the lower 790 of steps, absent from the unit's collision and objects [U]: volume 6866 is not reached on foot |
| `ch15_ballcourt_exit_switch` | l4_ballcourt | `switch_without_the_key` | with the socket empty Interact uses exit switch 5469 (through its use record) and reveal 5464 opens: the original keeps the switch locked (modifiable 7 (2), its state code) until the key goes in (keyhole 6501 vt 20 -> 5469 vt 62) [O bytecode]; ours honours a switch's modifiable 7 for `ar_ei_switch_oneway` only. The key itself now reaches the keyhole (`ch15_xibalba_key_to_exit`) |
| `ch13_ballcourt_calendar_solve` | l4_trailstart → l4_ballcourt | `jaguar_handle` | the mask now arrives and is set on calendar 5477's jaguar (`ch13_mask_to_calendar`; section 5: 3 -> 4 -> 5), but the push handle then gripped turns nothing: no joint of the calendar moves whichever way she pushes. What opens the handle sections once the mask is in is not decoded |
| `ch13_temple_block_topple` | l4_temple | the grapple | ring 4951 is not offered from the east ledge; the statue head's mover is unknown |
| `ch15_pool_rune_block` | l4_pool | the drag | rune block 5609 has no joint record, so nothing drags it |
| `ch21_platform_ride`, `natla_platform_ride` | l7_yggdrasil_end | `jump_to_a_platform` / the grab | the steps of `natla_fight`, then a jump from the entry walkway's end at a floating stone. The stones ride their orbit rigs now (they stood at their intro positions when `ch21_platform_ride` was written; that scenario was not rerun since), but the timed running jump at a passing stone falls short: its ledge is 1240 out and 540 up (docs/GAMEPLAY.md "Natla") |
| `ch09_shrine_shaft_descent` | l2_shrineroom | after the statue pull | its floor slabs stay solid over the shaft and its ring is still a target; the drop-catches and the mid-fall grapple at ring 2673 follow |

Each shipped scenario fails when its mechanic does not happen (checked by running altered copies, 2026-11-10):
`puzzle_pole_block_door` without the two pole steps fails at `jump_to_the_pole` (state `idle`, not `polehang`);
`grapple_swing` without the grapple press fails at `throw` (timeout, states `idle, hit`); `swim` without the dive
input fails at `dive` (states `swimsurface, swimtread`); `motorcycle` without the interact press fails at `mount`;
`fall_damage` from 1200 instead of 1500 fails `fall_1500` (drop 1199.97, damage 0); `locomotion` expecting a 250-high
standing jump fails with actual 202.187.

## Adding a chapter scenario

1. Pick the entry: `level` + `spawn` next to the sequence (docs/PROGRESSION.md "Chapter routes" and "Gates" list the
   units, gates and object uids per chapter), or `start_chapter`. Give `chapter` and the `walkthrough` URL.
2. Draft the steps with loose expectations and run with `--self-test-trace`; read the report's `measurements`,
   `states`, `clips`, `calls` and the trace lines to see what actually happens and when. Navigate with `plan` lines
   (`go`, `engage UID`, `press`, `push`, `run`), act with `input` lines.
3. Replace the loose expectations by what the sequence must achieve: the state chain (`visits`), the object's reaction
   (`object.<uid>.joint` / `.graph` / `.uber.<n>`, `calls`), where she ends (`player.pos.*` with a tolerance,
   `world.unit`, `world.checkpoint`), measured numbers with a tolerance and a `note` saying where the number comes
   from ([O] decoded / Wine-measured, else ours).
4. End steps with `until` where the game decides the timing (a grab, a landing, a door), so later steps do not depend
   on tick counts.
5. Use fixtures only for set-up the walkthrough takes for granted (a weapon, health), never for the thing under test.
   A chapter leg is named `chNN_<unit or leg>.json` (NN = the section's number in docs/matrix); a unit that belongs
   to two chapters takes `level_chapter`. `stick` is camera-relative: for a move along a known direction (a roll through
   a gap, a running jump) use `run YAWDEG ...`.
6. Save it as `tests/selftests/<chapter>_<what>.json`, re-run CMake, run `ctest -R game_selftest_<name>` in a private
   compositor. A sequence that shows a defect goes to `tests/selftests/known_gaps/` with a `note` describing it.
