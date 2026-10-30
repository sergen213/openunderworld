# TRU physics objects, object collision, hinge doors

Code: `src/physics` (`ou_physics`: `loadObjectPhysics`, `solidShape`, `massProps`, `Scene` with bodies, hinges, movers),
`collision::CollisionMesh::dynamics` (moving solids queried with the terrain), `src/game/Objects.cpp` (World glue).
Tests: `tests/test_physics.cpp` (`Physics.*` synthetic, `PhysicsRealData.*` with `OU_GAME_DIR`).

Labels: **[O]** observed in game data or our tru.exe decompile (GOG 1.1.0.0 VAs), **[I]** inference, **[U]** unknown.

## Physical objects in tru.exe

- [O] ObjectBase `+0` oflags bit `0x80000000` selects the object family "physical" (`0x5BF0E0`, family table
  `0x8ABA28`, 7 dwords per entry: name + init/process/... callbacks). Its init `0x6D00E0` dispatches on the
  **physical class**: instance `+0x50` (= ObjectBase `+0xC8` data) `u16 +4 == 0xB00B` and `u16 +2 == class`
  (`0x4D0D50`).
- [O] Class histogram over the 794 object DRMs: 6 ×404, 79 ×204 (uber-object composite, `0x6D5F80`,
  `UberObjectComposite_TR8`), none ×61, 34 ×28, 50 ×24, 57 ×14, 35 ×13, …
- [O] Class **34 (0x22)** = single rigid body (`0x6BA570` init, `0x6BA6E0` process, `0x6BA730` query): every `*_rb_*`
  object (press plates, tomb gates, falling bridges, `ca_rb_wall_hpole_door`, …). It builds the rigid body with
  `0x497270` when ObjectBase oflags2 (`+4`) has `0x800000`.
- [O] Rigid bodies come from the model's collision primitives (`0x497270` → `0x48E650`/`0x48E330`): primitive types
  1 (sphere), 5 (box), 10 (capsule), 13 (mesh-like, not decoded) carry a u16 mass; inertia per shape:
  box `m(b²+c²)/12` with each side ≥ 0.2 × the longest (`0x4E9100`), sphere `0.4 m r²` (`0x4DC770`), capsule =
  cylinder + hemispheres (`0x4E9230`). Invalid mass properties fall back to 666 [O]. `massProps` ports these.
- [O] Joints (`0x4D4240`, "physics: invalid joint type %d"): types **1 revolute, 2 cylindrical, 3 prismatic,
  4 planar, 8 universal** (the `Revolute/Prismatic/Cylindrical/Universal/Planar…: Joint must be of type …` checks,
  `0x4A6810`…), plus 5, 9, 10, 11, 12 [U]; link joints (gear ratios) exist. Script angles are degrees
  (`× 57.2958`), speeds degrees per 30 Hz frame (`× 1718.87`).
- [O] Scripts drive them through the native type **`multibody`** (script 3566, 94 natives registered by
  `0x7B2CF0`): 2 = enable physics on the atoms (`0x4A6F70`), 77 = set joint limit (joint, limit 1..3, lo, hi;
  `0x4A9C40`), 78 = get joint value (`0x4A9BB0`), 79 = joint drive (`0x4A9CF0` → `0x49A5C0`).

## Object collision primitives (dtp Model, model 0 of the object)

Model DTP (ObjectBase `+0xA4` → resource ref): `+4` segment count, `+0xC` segments (0x40 each: pivot float4
`+0x20`, parent i32 `+0x38`, **HInfo\*** `+0x3C`), `+0x38` marker count, `+0x3C` markers [O].

HInfo = `{u32 count, ptr list}` pairs [O, survey of all 1 057 models]: slot 0 spheres (977 segments use slot 2,
606 slot 1, 369 slot 0, 98 slot 3):

| slot | stride | layout [O] |
|---|---|---|
| 0 sphere | 0x20 | `+0` 4 flag bytes, `+4` u16 id, `+6` u16 radius, `+8` s16 x, y, z, `+0x14` u16 mass |
| 1 box | 0x50 | `+0` float4 **full** side lengths, `+0x10` float4 centre, `+0x20` quaternion x y z w, `+0x30` flag bytes, `+0x34` u16 id, `+0x36` u16 mass |
| 2 marker | 0x20 | not decoded (Lara's bones) |
| 3 capsule | 0x40 | `+0` float4 centre, `+0x10` quaternion, `+0x20` flag bytes, `+0x24` u16 id, `+0x26` u16 radius, `+0x28` u16 length, `+0x2A` u16 mass |

- Strides from multi-entry lists (pr_spear spheres, ms_uo_floor_grate_b capsules, door boxes) [O]; field offsets
  from the mass function above [O]. Full box sizes: the door's boxes then fit its render bounds (893 × 295 × 1122)
  [O]; half extents would not.
- Capsule axis = local Z [I]: the floor grate's four bars only form a square outline that way.
- Flag byte 1: `0x11` on doors, keys, switches, blocks; `0x10` on every `bend_*` plant [O]. [I] bit 0 = solid
  for the player (plants only bend). Byte 0 differs per box (door: 0x10, 0x40, 0x50) [U].
- Positions are in the segment frame [I]; `ObjectPhysics::segments` accumulates the pivots.
- All 794 object DRMs decode: 1 668 primitives (1 195 boxes, 374 spheres, 99 capsules) (`PhysicsRealData.AllObjectsParse`).

Model markers (`+0x3C`, 0x30 each): float4 position, float4 Euler rotation, `+0x20` u32, `+0x24` i32 [O].
`ca_rb_wall_hpole_door`: one marker at (0, 0, 952.9), rotation (−π, 0, 0), u32 2, −1. tru.exe's joint creation
(`0x4D8000`) reads its frames from model `+0x3C` entries (stride 0x30) [O]; that this one is the door's joint, of
type 2, is [I].

## The ballcourt door (l4_ballcourt)

- [O] Intros: pole lever 6502 `ca_pt_phole_lever`, door 6505 `ca_rb_wall_hpole_door` (class 34, oflags2 0x800000,
  3 solid boxes, masses 5 + 90 + 5, no animation clips; neither object has clips).
- [O] Door script 15695 fn 0: `multibody.SetJointLimit(joint m8, 1, 2, 2); multibody.Enable()` — locked at 2°.
- [O] Lever script 15637: fn 2 loops every 0.033 s: drives its own joint (native 79 with −600, −350, 1), reads its
  joint value (78) into m8 and, when it changed, calls fn 3 (vt 54): `door.SetJointLimit(door.m8, 1, m24, m24)`
  with `m24 = m8 * 2` (opcode 0x40 = MULI). So the door follows the lever 2:1 while Lara works it [O].
- [O] Geometry (raycasts): the door sits in a 600-wide, 1 047-high passage through a wall; the room behind
  (−y) is 1 300 high. A flap hinged at z = 952.9 about model X swings up into that room [I].

## Our implementation

- `CollisionMesh::dynamics`: `{owner id, shape mesh, rigid transform}`; `raycast`, `sweepSphere/Capsule`,
  `closestPoint`, `groundProbe` query the terrain and every dynamic (query mapped into the shape's frame), `Hit::dynamic`
  says which. Shape faces use mask `physics::kObjectMask` = 0x10 (our convention [I]); mask-0x0F queries skip them
  (push-block probes, so a block does not hit its own box).
- World (`Objects.cpp`): every script Door and Block gets one dynamic per segment with solid primitives
  (spheres/capsules as their bounding boxes), placed by intro × pose (animated doors follow their bone, blocks their
  offset). Doors therefore block while closed and stop blocking once their animation (or swing) moves them away.
  Lara rides the dynamic under her feet.
- Hinge doors and levers [I]: a class-34 door or class-6 lever without clips becomes a rigid body on a hinge about
  its first marker's X axis. Script `multibody` 2/77/78/79 enables physics, changes limits, reads the angle and
  issues drive commands. Function-0 construction and the lever's WAIT-based function-2 polling loop run, so using
  the pole lever drives the door to twice the lever angle through the original bytecode rather than a full-open
  fallback. Other animated doors still use their script state and clips.
- Solver (`Scene::step`, fixed tick): semi-implicit Euler, sequential impulses (16 iterations, Baumgarte 0.2):
  anchor point (3 rows), axis alignment (2), motor (clamped by `maxTorque·dt`), limits; tru.exe gravity
  5.72443 units/frame². Deterministic: same ticks ⇒ bit-identical bodies at 30/60/144 Hz render rates
  (`Physics.DeterministicAcrossRenderRates`).
- Timing rule: physics, movers, uber/multibody commands and the scripts driving them advance only in
  `World::tick` (one `Scene::step(kTickSeconds)` per 60 Hz tick, no time-based substeps); the render rate, vsync and
  FSR frame generation never step them (docs/GAMEPLAY.md "Timing", audit table). Real puzzles are checked
  bit-identical tick by tick at 24–1000 Hz and irregular frames: `GameReal.{BallcourtLever,ElevatorUber,PushBlock,
  SplineMover,GrappleLeverJoint}IndependentOfRenderRate`.
- Uber joints (class 79 objects, `formats::UberJoint` / `UberParam`, `physics::Joint1`, `World::tickUberJoints`):
  - [O] State entry actions (0x10 bytes {type, 3 words}, run on entering the state, 0x4D7440 -> 0x4D5C60 -> 0x4D86A0):
    9 creates the joint of the record its first word points at (0x4D8000; nothing if it exists), 10 removes it
    (0x4D6220), 12 sets its limit (0x4D6310 -> 0x4D4510), 13 its motor (0x4D6350 -> 0x4D2B00 prismatic / 0x4D29F0
    hinge), 0x14 / 0x15 add / remove a range watch (0x4D7380 -> 0x49A5C0 / 0x493A80; word 1 = mode, 2 = joint, 3 = range).
  - [O] Joint record: +0 type byte (0x4D1040: 0 hinge -> engine 1, 2 prismatic -> 3, 5 -> 8, 7 -> 10, 8 fixed -> 0xB,
    11 link -> 0xD), +4 / +8 bodies A / B = section indices (each section spawns its instance into the component's
    +0x6C list, 0x4D3DA0; -1 = world), +0xC model byte, +0xD marker byte: the frame is that object model's marker
    (0x30 bytes, Euler XYZ) × the instance transform; the joint axis is the frame's Z (0x4D4240 passes row 2 to the
    hinge / prismatic constructors 0x4DF5D0 / 0x4DFC10). +0x10 block (0x4D4690): +0x2C / +0x30 limit (hinge degrees),
    +0x34 motor speed per second (× 1/30, hinge × π/180/30, 0x4D1F00), +0x38 motor bound (<= 0: unbounded),
    +0x3C bit 0 limit on, bit 1 motor on. Link (type 11, 0x4D7ED0 -> 0x4E17B0): {type, joint1 ptr, joint2 ptr, ratio};
    joint2 = ratio × joint1 (degrees per unit when joint1 is prismatic and joint2 a hinge: l4 grapple post 1264 × 0.75
    = 948, the second joint's limit).
  - [O] Limit / motor / range records {u8 kind, a, b, u8 flag} (0x4D11E0): limit lo, hi, on; motor speed, bound, on;
    range lo, hi. Watch check (0x4937A0, every instance update): value = mode 0 position (prismatic units, hinge
    degrees), 1 velocity per second; `inside` starts true, so a watch registered outside its range reports leaving on
    the first check; `lo <= v < hi` turning true calls back with 1, false with 0 (0x4D5B10 -> 0x4D5A50), which starts the
    watching section's transition of trigger 14 (entered) / 15 (left) with the same mode (+0x1C), joint (+0x20) and
    range (+0x24), only while the section is in a state.
  - [I] Simulation (`physics::stepJoint`, one per 60 Hz tick = half a 30 Hz frame): each joint is one degree of
    freedom of body B (its section model's mass, centre of mass and inertia, `massProps`) against a fixed frame, with
    linked joints folded into one effective mass; gravity 5.72443 units/frame², each motor a velocity row whose impulse
    is clamped to its bound per step, inelastic limits. A tru.exe solver step of 1/60 s is inferred from the motors: of
    the vertical prismatic uber joints, five motors are weaker than their body's weight per 30 Hz frame but stronger
    than per 1/60 s (hallway ramps 395 vs 100 × 5.72, wall lever 319 vs 60 × 5.72, cliffclimb ledge, empty throne,
    windup statue), and only the latter lets them hold their bodies as the states expect. Body A's own motion, the
    multi-axis types 5 / 7 (push blocks, scales) and fixed joints (8) are not simulated; watches on them never fire.
  - Every uber object's sections start in their default state at spawn (each section component exists from the
    instance's creation, 0x4D3DA0), not when a script first touches it: the l7 Thor towers extend on their motors at
    load. Model 0 of an uber object follows the joint of the section that uses it when it has no travel clip and no
    loose / carried offset (`World::uberJointPose`). Bullets push watched joints (`World::uberShot`, impulse 3000 [I]
    along the shot). Lara grips a model-0 body on a level prismatic joint within 120 units of its collision and the stick
    moves it along the axis at 120 units/s, over its motor and gravity, and she follows (`World::pushUberJoints` [I], as
    push blocks): the Thor towers are pushed back out after the fight retracts them (`ProgressionReal.EndgameCredits`).
  - Verified: `ProgressionReal.HallwayRampsJoint` (ramp 6045 falls to -948 and tilts the ramps, rises back on its
    motor), `ProgressionReal.KrakenColumnShot` (the shot column leaves [-15.8, 237)), `GameReal.GrappleLeverJoint
    IndependentOfRenderRate` (l4 grapple post 6695: pulled out, its linked joint enters [158, 1580): trigger 14),
    `StreamReal.UberJointSaveLoad`, `Physics.UberJointFallsLiftsAndLinks`, `RealData.UberJointsDecode`.
- `Mover`: kinematic ping-pong path for platforms; no moving platform object was identified yet [U], so World does
  not create any.

## Verification

- `ou_physics_tests` (2026-10-18): box inertia vs the exe formula, motor opens to 80° and closes, limits stop a free
  swing, render-rate determinism, mover path, dynamic raycast/sweep/closest/ground probe follow the transform and the
  mask; real data: door decode, all objects parse.
- Replays (l4_ballcourt, GL screenshots viewed): `tests/replays/ballcourt_door_blocked.txt` with
  `--spawn -18520,14600,3540,-90`: Lara stops at y = 13 944.9, against the closed door (door at 13 706 + box half
  depth 136 + capsule 100). `GameReal.BallcourtLeverDrivesDoorJoint` verifies a 40° lever and 80° door through the
  native 2:1 script coupling. `GameReal.BallcourtLeverReplayPassesDoor` completes the same route with physics
  isolated. With the poacher enabled, its genuine hit reactions interrupt the rendered replay at (-18871.9, 14058.3);
  this is combat, not a remaining door-collision blocker.

## Open questions

- [U] Where class-34 joints are created (the query `0x6BA730`/`0x6B4EC0` answers no joint count) and the cdc joint
  axis convention; the lever's joint and Lara's interaction with it (she pushes the lever round).
- Wall climbing (implemented, GAMEPLAY.md "Free climbing"): no climb RTTI state exists (state-chart driven) [O].
  Face material / clientFlags show no climb-specific value (l2_cliffclimb walls: material 27/6/10/14/18) [O]. The
  climb surfaces are the **markup types 16–20** (MUD bits 0x4000…0x400 + 0x8000): 3-point triangles ~150–250 units
  across lying 20–35 units off vertical walls, e.g. l2_cliffclimb wall y ≈ −3404 from z 677 (519 above the floor)
  to 2 270 [O]; tru.exe attaches Lara to 0x8000 markup (0x707C60) [O], so they are free-climb holds.
- [U] HInfo slot 2 markers, primitive type 13, flag bytes 0 and 2–3.
- [U] Moving terrain groups: none of l4_ballcourt's 17 groups sits at the door; the small offset groups remain unexplained.
