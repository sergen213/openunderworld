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
`go X Y [R] [walk]`, `engage UID [X Y] [walk]`, `face YAWDEG`, `press [ACTION]`, `hold ACTION SECONDS`,
`stick FORWARD RIGHT SECONDS [ACTION AT]`, `run YAWDEG SECONDS [ACTION AT]` (a world heading, whatever the camera
does), `push FORWARD RIGHT SECONDS [ACTION]`, `wait SECONDS`, `near R`. Coordinates are world frame. With both `plan`
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

### Weapons and combat

| probe | value |
|---|---|
| `weapon.current`, `weapon.secondary` | `pistols uzi shotgun speargun thorhammer rifle tranquilizer` |
| `weapon.drawn` | in hand (or being drawn) |
| `weapon.clip`, `weapon.reserve` | rounds in the current weapon's gun(s); spare rounds |
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
| `portable.held` | uid of the portable she carries, 0 none |
| `portable.<uid>.in`, `object.<uid>.in` | the receptacle that portable is inserted in, 0 none |
| `object.<uid>.exists` `.name` `.hidden` `.dead` | a script object by intro uid (objectlist name; instance hidden / killed) |
| `object.<uid>.pos.x/.y/.z` | its position with its own displacement (a pushed block, a fallen body) |
| `object.<uid>.body.x/.y/.z` | the translation of its own rigid motion from rest (doors on their joints, sliders) |
| `object.<uid>.graph` | its anim-graph state id |
| `object.<uid>.joint` | its multibody joint's position (plate units, hinge degrees) |
| `object.<uid>.uber.<n>` | uber section n's state, -1 while it is in a transition |
| `object.<uid>.uberjoint.<n>` | simulated uber joint n's position |

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
| `locomotion` | l3_main_bedroom | idle; walk 348 and run 1274 units/s; stopping; standing jump 201.6 high / 0.56 s, stick jump 218.1 / 0.58 s, running jump 157.1 / 0.50 s at 1974.9 units/s [O Wine] |
| `sprint` | l4_trailstart | sprint_loop 9.7 s, the exhausted clips, ~2.2 s until a 0.9 s second sprint, refill at rest [O Wine] |
| `fall_damage` | l4_pool | falls of 1000 / 1500 / 2000: air time, landing, 0 / 204.7 / 729.4 damage [O curve] |
| `death_respawn` | l4_pool | checkpoint 5502 touched; a 2600 fall kills; 3 s later she stands at the checkpoint with full health |
| `ledge` | l3_main_bedroom | standing jump catches ledge 973; shimmy left and right; pull up 496 onto the top |
| `traversal_chain` | l2_cliffclimb | run -> jump -> hang (911) -> shimmy -> climb up -> run on, as one state chain |
| `ladder` | l1_ship_sinking_hall | attach from the ground (234), climb up, climb down, step off |
| `pole` | l4_traprooms | standing jump to pole 1336, swing, fling 1264 to pole 1337 in 23.5 frames [O Wine] |
| `pole_drop` | l4_traprooms | letting go of pole 1336: detach clip, fall, landing |
| `balance_beam` | l4_temple | beam 1079: walk 140 units/s on its line, jump along it, its far end, losing balance into a ledge catch (1136) |
| `grapple_swing` | l3_chapel | ring 4744 offered, throw, attach (rope <= 4424.78), swing past the ring, climb / slide the rope 450, jump off, landing |
| `rappel` | l1_kraken_room | wall anchor 326: swing onto the wall, rappel down (rope pays out) and up, kick off |
| `wall_run` | l5_valaskjalf | wall anchor 6750: wall run through the rope's arc over the pit, landing on the far ledge, rope let go |
| `swim` | l1_seatop | drop into the sea, surface swim 1348.6 units/s, dive, under water (on the wetsuit's tanks the breath stays full), surfacing |
| `swim_breath` | l2_cliffclimb | no tanks: the breath drains under the river, 1.5 times as fast sprinting, the meter refills in 2 s at the surface |
| `swim_moves` | l1_seatop | surface turn on the spot, dive, under-water dodges up / down by their clips, pistols drawn and fired under water |
| `swan_dive` | l2_cliffclimb | running jump + crouch off the cliff top: swan clips, 5900 down into the sea on `swim_swan_land`, no damage |
| `puzzle_pole_block_door` | l4_ballcourt | pole 6503 into lever 6502 (graph 0 -> 1), push block 6504 under it (two pushes), up onto the block, jump to the pole: lever joint slides, door 6505 rises |
| `combat` | l2_cliffclimb | draw, hard lock, pistols kill three spiders and hit a 700 hp target dummy 8+ times, adrenaline fills |
| `motorcycle` | l4_trailstart | mount bike 5482, accelerate (wheels on the ground), steer left, hop (both wheels off, then down), brake to a stop, dismount |
| `pole_drop_after_fling` | l4_traprooms | the fling 1336 -> 1337, then Drop: `Hpole_detach_dropPerp`, down 27 beside the rubble mound under the pole, standing within 1.2 s (not caught again by pole 1337) |
| `saving_grab` | l5_valaskjalf | drop-down ledges 1488 -> 1485 -> 1484: each Drop is caught one handed 1422 / 1387 lower (`ledgeb_bad_attach_M`, 50 frames), Interact pulls her up, Jump does not, left alone she falls after 1.67 s |
| `safety_drop` | l4_temple | running off the high platform's east edge: `drop_to_ledgeb` without a key, hanging on ledge 1168 (root 499 down), then Drop to the floor 251 under her feet |
| `safety_drop` | l4_temple | running off the high platform's east edge: `drop_to_ledgeb` without a key, hanging on ledge 1168 (root 499 down), then Drop to the floor 251 under her feet |

`tests/selftests/known_gaps/` holds scenarios for behaviour that does not work yet; they fail today and are not
registered with ctest. Run them by hand; move one up a directory when it passes. Now: none (`pole_drop_after_fling`
moved up 2026-11-10).

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
6. Save it as `tests/selftests/<chapter>_<what>.json`, re-run CMake, run `ctest -R game_selftest_<name>` in a private
   compositor. A sequence that shows a defect goes to `tests/selftests/known_gaps/` with a `note` describing it.
