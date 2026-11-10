# TRU physics objects, object collision, hinge doors

Code: `src/physics` (`ou_physics`: `loadObjectPhysics`, `solidShape`, `massProps`, `Scene` on Jolt Physics: hinge bodies,
uber joints and links, loose bodies, terrain and object solids),
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
| 0 sphere | 0x24 | `+0` 4 flag bytes, `+4` u16 id, `+6` u16 radius, `+8` s16 x, y, z, `+0x14` u16 mass |
| 1 box | 0x50 | `+0` float4 **full** side lengths, `+0x10` float4 centre, `+0x20` quaternion x y z w, `+0x30` flag bytes, `+0x34` u16 id, `+0x36` u16 mass |
| 2 marker | 0x20 | not decoded (Lara's bones) |
| 3 capsule | 0x40 | `+0` float4 centre, `+0x10` quaternion, `+0x20` flag bytes, `+0x24` u16 id, `+0x26` u16 radius, `+0x28` u16 length, `+0x2A` u16 mass |

- Strides from multi-entry lists (pr_spear and motorcycle spheres: 0x24, the second sphere's flag bytes start there;
  an earlier 0x20 misread every sphere after the first; ms_uo_floor_grate_b capsules, door boxes) [O]; field offsets
  from the mass function above [O]. Full box sizes: the door's boxes then fit its render bounds (893 × 295 × 1122)
  [O]; half extents would not.
- Capsule axis = local Z [I]: the floor grate's four bars only form a square outline that way.
- Flag byte 1: `0x11` on doors, keys, switches, blocks; `0x10` on every `bend_*` plant [O]. [I] bit 0 = solid
  for the player (plants only bend). Byte 0 differs per box (door: 0x10, 0x40, 0x50) [U].
  [O] tru.exe `0x48E710` makes a Havok shape of a primitive only with bit 0x100 of the flag u16 (byte 1 bit 0), and of
  a sphere only when byte 2 is not 2 (wheel spheres). [I] Byte 0 bit 0x40 = a shape that does not rest on the world:
  `ms_pt_weight_c` = a 126.4-unit cube (flags 10 11, 25 kg, base at z 1.4) inside a sphere of r 104 at z 64 (flags
  40 11 09 1e, 5 kg) that reaches 40 units under the base its intros stand on [O data]; also on `ar_pt_pole_a`'s end
  sphere (r 80) and the snake head's two handle capsules. Loose bodies (`looseShape`) are built from the primitives
  with the solid bit, without bit 0x40 (all of them when none is left); masses and inertia still count every
  primitive. [O, not used yet] 0x6AD070 scales a water volume's density / drag per body by the primitive's percent
  byte after its mass (sphere +0x16, box +0x38, capsule +0x2C; no buoyancy when 0: both weight primitives 0) and the
  current by the next byte; ours floats every loose body at 100 %.
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
  its first marker's X axis. Script `multibody` 2/77/78/79 enables physics, changes limits, reads the joint value and
  issues drive commands. Function-0 construction and the lever's WAIT-based function-2 polling loop run through the
  original bytecode. Other animated doors still use their script state and clips.
- Joint types and `multibody` 77 / 78 limit indices [O decomp 0x4A5C40 / 0x4A5D90, joint +0x178]: 1 hinge (limit 1 =
  angle, degrees), 2 cylindrical (1 = linear position, units; 2 = angle, degrees), 3 prismatic (1 linear), 4 (1, 2
  linear, 3 angle), 8 (1, 2 angles), 9 (1 linear, 2, 3 angles), 10 (1, 2, 3 linear); 77 converts degree limits to
  radians (0x4A5C40), 78 returns degrees for angles (0x4DA930 x 57.2958) and units for positions (0x4DA460 / 0x4DA710 /
  0x4DA820). A class-34 door whose first marker is kind 2 (l4_ballcourt `ca_rb_wall_hpole_door` 6505; [I] marker kind =
  joint type) slides along that marker's Z within its limit 1 (record limit 0..0, on) instead of swinging: pole lever
  6502's slide (78, cylindrical, units) x 2 lifts it (`GameReal.BallcourtLeverDrivesDoorJoint`).
- Simulation: [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.6.0 (CMake `FetchContent`, pinned SHA-256, MIT
  licence in `third_party/jolt/LICENSE`, `CROSS_PLATFORM_DETERMINISTIC`, single-threaded job system), one
  `Scene::step(kTickSeconds)` per 60 Hz tick from `World::stepPhysics` (inside `World::tickUber`, after the section
  updates and before the uber events run; the range watches then read the new joint positions). See "Havok → Jolt"
  below for the parameter mapping.
- Timing rule: physics, uber/multibody commands and the scripts driving them advance only in `World::tick` (no
  time-based substeps); the render rate, vsync and FSR frame generation never step them (docs/GAMEPLAY.md "Timing",
  audit table). Real puzzles are checked bit-identical tick by tick at 24–1000 Hz and irregular frames:
  `GameReal.{BallcourtLever,ElevatorUber,PushBlock,SplineMover,GrappleLeverJoint}IndependentOfRenderRate`,
  `Physics.DeterministicAcrossRenderRates`, ctest `game_render_rate_smoke`.
- Uber joints (class 79 objects, `formats::UberJoint` / `UberParam`, `physics::Joint1`, `World::stepPhysics`,
  `World::uberWatches`):
  - [O] State entry actions (0x10 bytes {type, 3 words}, run on entering the state, 0x4D7440 -> 0x4D5C60 -> 0x4D86A0):
    9 creates the joint of the record its first word points at (0x4D8000; nothing if it exists), 10 removes it
    (0x4D6220), 12 sets its limit (0x4D6310 -> 0x4D4510), 13 its motor (0x4D6350 -> 0x4D2B00 prismatic / 0x4D29F0
    hinge), 0x14 / 0x15 add / remove a range watch (0x4D7380 -> 0x49A5C0 / 0x493A80; word 1 = mode, 2 = joint, 3 = range),
    0x19 / 0x1A clear / set the section instance's +0x1C bit 2, which the grapple target filter 0x6ECCA0 (called by
    0x7131A0 before the target sets) refuses: no grapple target while set (`World::uberGrappleOff`, saved as
    `grappleOff`). The grapple handles' volumes use it (GAMEPLAY.md "Grapple").
  - [O] Each section with a model of its own is an instance (tru.exe under Wine, l2_towerconnector: 2043 is three
    instances, uid 2043 and two 0x40000000+n section instances, the handle's tilted by its hinge). Ours draws a
    section's model when it is body B of a hinge / prismatic joint record (the level scene tags it " @section <k>";
    ou-game poses it by `World::uberSectionBody`); [U] other extra-model sections are not drawn.
  - [O] Joint record: +0 type byte (0x4D1040: 0 hinge -> engine 1, 2 prismatic -> 3, 5 -> 8, 7 -> 10, 8 fixed -> 0xB,
    11 link -> 0xD), +4 / +8 bodies A / B = section indices (each section spawns its instance into the component's
    +0x6C list, 0x4D3DA0; -1 = world), +0xC model byte, +0xD marker byte: the frame is that object model's marker
    (0x30 bytes, Euler XYZ) × the instance transform; the joint axis is the frame's Z (0x4D4240 passes row 2 to the
    hinge / prismatic constructors 0x4DF5D0 / 0x4DFC10). +0x10 block (0x4D4690): +0x2C / +0x30 limit (hinge degrees),
    +0x34 motor speed per second (× 1/30, hinge × π/180/30, 0x4D1F00), +0x38 motor bound (<= 0: unbounded),
    +0x3C bit 0 limit on, bit 1 motor on. Link (type 11, 0x4D7ED0 -> 0x4E17B0): {type, joint1 ptr, joint2 ptr, ratio};
    joint2 = ratio × joint1 (degrees per unit when joint1 is prismatic and joint2 a hinge). Survey of all object DRMs:
    222 joints (78 hinges, 42 prismatic, 92 fixed, 3 type 5, 7 type 7; 61 with a body A), links 0 → 0 ×9, 2 → 0 ×6,
    2 → 2 ×8 (l4 grapple post 6695: prismatic 1264 × 0.75 = 948, the second prismatic's limit); every hinge limit lies
    within ±180°.
  - [O] Limit / motor / range records {u8 kind, a, b, u8 flag} (0x4D11E0): limit lo, hi, on; motor speed, bound, on;
    range lo, hi. Watch check (0x4937A0, every instance update): value = mode 0 position (prismatic units, hinge
    degrees), 1 velocity per second; `inside` starts true, so a watch registered outside its range reports leaving on
    the first check; `lo <= v < hi` turning true calls back with 1, false with 0 (0x4D5B10 -> 0x4D5A50), which starts the
    watching section's transition of trigger 14 (entered) / 15 (left) with the same mode (+0x1C), joint (+0x20) and
    range (+0x24), only while the section is in a state.
  - Simulation: each joint is a Jolt body (body B: its section model's mass and centre of mass, `massProps`; a hinge's
    inertia about its axis through the pivot) on a world-fixed Jolt `SliderConstraint` / `HingeConstraint` with the
    data's limit and a velocity motor (speed, force bound); links are one constraint row `r·q1 − q2 = 0` between the
    two joint bodies (ours, `LinkConstraint`: Jolt's gear / rack-and-pinion parts lack slider → slider). x and v are
    written back to `Joint1` after each step; a hinge x is unwrapped across ±π. [I] A tru.exe solver step of 1/60 s:
    of the vertical prismatic uber joints, five motors are weaker than their body's weight per 30 Hz frame but stronger
    than per 1/60 s (hallway ramps 395 vs 100 × 5.72, wall lever 319 vs 60 × 5.72, cliffclimb ledge, empty throne,
    windup statue), and only the latter lets them hold their bodies as the states expect. [I] Body A is the world (its
    own joint motion does not carry B); fixed joints (8) therefore pin nothing and are not created; the multi-axis
    types 5 / 7 (push blocks, scales) are not simulated; watches on them never fire.
  - Every uber object's sections start in their default state at spawn (each section component exists from the
    instance's creation, 0x4D3DA0), not when a script first touches it: the l7 Thor towers extend on their motors at
    load. Model 0 of an uber object follows the joint of the section that uses it when it has no loose / carried
    offset (`World::uberJointPose`). Bullets push watched joints (`World::uberShot`: impulse 3000 [I]
    along the shot at the body's centre of mass, as a velocity change of the joint's degree of freedom that the Jolt
    body takes on the next step). Lara grips a model-0 body on a level prismatic joint within 120 units of its collision and the stick
    moves it along the axis at 120 units/s, over its motor and gravity, and she follows (`World::pushUberJoints` [I], as
    push blocks): the Thor towers are pushed back out after the fight retracts them (`ProgressionReal.EndgameCredits`).
  - Verified: `ProgressionReal.HallwayRampsJoint` (ramp 6045 falls to -948 and tilts the ramps, rises back on its
    motor), `ProgressionReal.KrakenColumnShot` (the shot column leaves [-15.8, 237)), `GameReal.GrappleLeverJoint
    IndependentOfRenderRate` (l4 grapple post 6695: pulled out, its linked joint enters [158, 1580): trigger 14),
    `StreamReal.UberJointSaveLoad`, `Physics.UberJointFallsLiftsAndLinks`, `RealData.UberJointsDecode`.
- Loose rigid bodies (`Object::loose`: grapple-pulled `*_rb_grapple*` targets, set-down portables): a dynamic Jolt
  body from all of the object's primitives (box / sphere / capsule; tru.exe builds the rigid body from every one,
  0x497270, the "solid" bit gating Lara only [I]; `massProps` mass and inertia; no primitives: a 20-unit ball, mass 666
  as tru.exe's fallback), starting at rest where it was; it falls, tumbles, slides and stacks on the resident units'
  terrain (one static `MeshShape` per unit, built the first time a loose body exists, dropped when the unit streams
  out) and on every other object collision (`collision().dynamics` as kinematic mesh bodies following their
  transforms: blocks, doors, other uber bodies; push blocks stay grid / axis driven by the scripts and shove loose bodies
  as kinematic bodies). Its pose goes back to `Object::offset` (displacement) and `Object::body` (rotation), so its
  collision, rendering and saves follow. Lara's proxy (`Scene::setCharacter`): a 55 kg capsule of her radius and height
  (mass: her ragdoll record, lara ObjectBase +0x7C +4 [O]), translation only and no gravity, velocity-driven each step
  toward her capsule (at most 1000 units/s; more than 60 units away it is moved), colliding only with loose bodies and
  ragdolls: walking into one shoves it by mass (a 50 kg box slides, a 666 kg weight barely moves) [I: the original's
  kicks of small objects are not evidenced]. It exists while loose bodies, rail-riding bodies or ragdolls do (key `a`).
  - A pulled body that still rides a model-0 uber joint keeps riding it, its joint body taking its shape and colliding:
    l1 snake head 751 sits on a rail (prismatic joint 0, axis 20° off vertical, [0, 15 803]) held by a heavier
    counterweight (joint 1, link 2 → 2 × 1); section 1 watches joint 0 in [−15.8, 31.6) and, leaving it, removes the
    link, then (after 0.95 s) the rail [O] (ms_rb_grapple_snake_head). [I] The pull drags the watched joint downhill at
    240 units/s while it is in range (Lara's rope on the Havok body; our heave runs no force); the head then slides
    down its rail onto grooved plate 752 and drops free when the rail goes.
  - Pressure plates (`Object::Joint`, base 3938; `World::stepPhysics`, `Scene::setPlates`): a Jolt body with the
    plate's collision mesh on a prismatic joint along its model Z, limited to [its lowest range, 0], the motor lifting it
    at 120 units/s with the force to hold its own weight (`massProps`, 50 kg without primitive mass [I]) plus 20 kg
    [I]. It collides with loose bodies, vehicles and ragdolls (not the terrain it sits in), so a landed weight presses
    it through its contacts; Lara (55 kg) and push blocks (their primitives' mass, 666 without) press it under gravity
    where a ray below them (40 above to 30 below their base) meets it. `World::tickJoints` hands the joint position
    (clamped to the limits) to `Runtime::setJointPosition`, whose ranges run vt 34. Saves: the plate body is a joint
    (`j<uid << 32 | 0xFFFFFFFE>`); a new plate starts at its saved position. [O] A class-34 rigid body's joint record
    (physical data +0x2C; tru.exe 0x6B6440: the joint type is the first model marker's kind, 0 hinge / 2 prismatic, its
    axis the marker frame's Z): +4 / +8 limit (on = +0x34 bit 0), +0x1C motor speed per second, +0x20 its bound (<= 0
    unbounded), on = +0x34 bit 3 (`ObjectPhysics::jointMotor`). Plates with one use it instead of the numbers above:
    ms_rb_press_plate, ms_rb_grooved_press_plate, ma_rb_press_plate -50..0, +40 / s, bound 400; ca_ei_rb_plate_normal
    bound 460 (survey: `OU_RB_MOTORS=1 ... ProgressionReal.RigidBodyMotors`). By the uber joints' unit rule (bound =
    impulse per 1/60 s step [I]) a 400 bound carries 139.7 kg: the 100 kg plate and 39.7 kg more. Lara bears on a plate
    with her object's primitive mass (500 kg [O data, I use]). Wine (l1_baldrs_tomb): Lara on 221 -> -51, back up when
    she leaves. `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`,
    `ProgressionReal.CriticalGates` (l1_kraken_entrance 748: head 751 on plate 752, weight 750 on plate 753).
- Saves: `World::serializeObjects` writes `physics` {version 1, hex of `Scene::save`}: each body's Jolt ID by entity
  key (hinge index, joint key, loose id, unit name, collider index), each constraint's zero (`Axis::ref`), the constraint
  count, then `PhysicsSystem::SaveState(All)` (velocities, sleep, contact cache, warm starts; our link rows record
  theirs). Body IDs are ours (the lowest free index, never Jolt's allocation order) and constraints are re-listed in a
  fixed order every step, so a load that re-creates the same entities gets the saved IDs and zeros and its next step
  applies the recorded state: the run continues bit for bit (`StreamReal.UberJointSaveLoad`,
  `StreamReal.UberStateSaveLoad`, `Physics.SavedSceneContinuesBitIdentically`). Saves without it (older ones) rebuild
  the scene from the objects; a loose body's rotation is also saved with its object (`body`), for those.
- Ragdolls (`physics::loadRagdoll`, `Scene::setRagdolls` / `ragdoll`, `World::syncRagdolls`, `ai::EnemyState::ragdoll*`):
  - [O] Data: ObjectBase +0x78 non-zero, +0x7C -> record {+0 0xBEEBBEEB, +4 total mass (poacher 60, panther 90, naga
    400), +8 radius scale (soldiers / thralls 2, animals 1), +0x2C bones, +0x30 count; +0xC / +0x10..+0x28 passed to the
    Havok system (0x4EC0B0) [U]}; 32 objects carry one: Lara and her outfits (15 bones), poacher / merc / thralls (15),
    cats / tiger (16), nagas (21), tarantulas (17), sharks / bats (5), small spiders (1). Bone (0x78 bytes, 0x4F1390;
    at most 24, 0x4F0B40): +4 s16 start segment (+6 overrides when >= 0), +8 offset, +0x14 s16 end segment (else the
    start), +0x18 offset, +0x24 parent bone, +0x28 % of the total mass (sum 100), +0x2C radius × scale, +0x30 / +0x34
    [U] (+0x34 goes to the body's +0x74), +0x38 joint type, +0x3C pivot (from the start point), +0x48 axis 1, +0x54
    axis 2, +0x60 / +0x64 / +0x68 / +0x6C / +0x70 limits in degrees, +0x74 friction (max with 0.01, written to the
    joint's per-axis +0x10C / +0x114 / +0x11C). 0x4F0B40 builds a capsule body from the start to the end point
    (capsule inertia 0x4E9230) named "ragdoll bone %d" and its joint to the parent: type 1 hinge (0x4DF5D0, axis 1,
    limits +0x60 / +0x64), 2 (0x4E08A0, axes 1 and 2, limits +0x60..+0x6C), 3 (0x4EE5C0: pivot, axis 1, a twist axis =
    axis 1, or the bone direction when 0x49AFA0 rejects axis 1, cone +0x70, limits +0x60 / +0x64); 0 = the root
    (pelvis). Poacher: pelvis, spine (2), head (3), upper arms (3), forearms (1), hands (2), thighs (3), shins (1), feet (2).
  - [O] Trigger (enemies): death state entry 0x66C500 picks the death entry (FUN_00642210 / 0x642170: a random one of
    the attacker's quadrant and flag) and arms the death timer (state block +0x10) for min + rand(max - min) counts, the
    entry's s32 +4 / +8 (poacher 52..60, panther 1..3 / 18..26, thralls 6..46; no entry: 0); at once instead when the
    summed impulses / damage of the hits it took (enemy +0x4380: +8 count of vec4s at +0xA0, +0x140 count of floats at
    +0x204) reach death record +8 / +0xC, or a hit flag +0x18 / the +0x45D0 counters say so — the thresholds (1e9 /
    101138 soldiers and cats, 2.8e6 thralls) are beyond any hit, so ours always waits (`EnemyState::ragdollAt`). When it
    expires 0x66C950 runs the chunk path (death record s16 +0x46 object: thralls) if the set budget allows (below), else
    0x617EE0 -> 0x60EF50 (at most 6 live ragdolls, the object's +0x78, else ObjectBase +0x50 +0x24's object) ->
    0x4F1920; a refused ragdoll re-arms the timer for death record s16 +0x1C counts (120). Anim graph `AnimRagdollNode` /
    `AnimDrivenRagdollNode` (0x44EF10) also start one (posting message 1). Counts are [I] 30 Hz (the timer's high dword,
    0x670470; the clock's unit is not found). The clip keeps playing until the takeover, which is a pure ragdoll from
    the pose and the segments' last motion (0x4F1920 builds it unpowered): no blend from the clip [O]. Sharks keep
    floating (no ragdoll).
  - [O] Lara: her outfit's record (lara / lara_* ObjectBase +0x7C: 55 kg, radius scale 1.25, 15 bones) is built by
    0x6F1400 -> 0x4F1920 from DeathState_Entry (0x75AA50) for death types 3 / 4 (at once; which deaths those are [U]) or
    from its message handler 0x75B000 on message 1 ([I] at the death clip's end; ours: when the clip is done). The
    death timer is then cut to 90 counts (3 s) before the reload [O]; ours keeps its respawn timing (clip + 1 s).
    `CombatState::ragdoll*`, scene key -1, no hit impulse; saved with the combat state (`ragdollPhase`, frames in her
    model space) and `World::stateParts` "player"; `GameReal.LaraRagdollIndependentOfRenderRate`.
  - [O] Chunks (thralls; `ai::ChunkDoll`, `World::startChunks` / `chunkPieces`): death record s16 +0x46 = objectlist id
    of a `*_chunkdoll` object (viking / mayan 602 `viking_thrall_chunkdoll`, panther 518, yeti 706). 0x696F90: at most 4
    sets (0xECFC70), pieces limited by the free instance slots (0x3FC - live instances) against data +0x10 (minimum) /
    +0x14 (kept); its ObjectBase +0xC8 data {+0x18 count, +0x1C -> 0x14-byte pieces {+0 u8 model, +2 s16 segment A, +4
    s16 segment B, +8 t, +0xC -> body properties {0.4, 0.2, 0.005, 0.005, 1, 0.03, 30} [U], +0x10 flags}}; viking: 14
    pieces of 6 models (100 kg each by their primitives). 0x696B90 makes each an instance of that model at mix(B, A, t)
    of the body's world segments, rotated by 0x696870's slerp of the two ([I] which end t = 1 takes), centred on the
    model's segment-0 box (0x696B00), velocity = mean of the two segments' (0x78DED0), spin from 0x78DF80 ([I] ours:
    segment B's), a Havok body from its model's primitives (0x497270, "ChunkDoll: failed to create geom"); then
    0x696A60 gives every piece whose owner (+0x58) is the enemy death record +0x4C x 2.5 along the hit (thralls 2000;
    ours: velocity += that / piece mass, as the ragdolls' impulse). The enemy instance is hidden (+0x14 |= 0x20). Ours:
    loose bodies keyed -(uid x 64 + k + 1), made right after the enemy tick (`EnemyState::pieces` / `pieceVel`, so a save
    between ticks holds them), poses read back after each step, drawn by ou-game in place of the body; saved with the
    enemy and in the scene's state. [U]: piece flags bit 0 (instance +0x14 bit 0x40000000 vs +0x18 0x20000), the +0x71
    mark on the first unflagged piece, piece 0's 0x69A900 (when the object's s16 +0x3C > 0), when 0xECFC70 drops (ours:
    live sets among the loaded enemies), fading pieces out. `AiReal.DeathTimerAndThrallChunks`,
    `GameReal.KilledThrallBreaksIntoChunks`.
  - Simulation [I]: one dynamic Jolt body per bone (layer loose: it lands on the terrain, the colliders and loose
    bodies), its frame the start segment's (the model's bind frames are rotation-free), a capsule from start to end
    (radius ≥ 1 unit), mass = share × total, inertia from the shape; friction 0.6, angular damping 0.5. Joints: Jolt
    `SixDOFConstraint` (pyramid swing), translation fixed at start + pivot; type 1 rotation about axis 1 in
    [+0x60, +0x64], others fixed; type 2 axis 1 in [+0x60, +0x64], axis 2 in [+0x68, +0x6C], the third fixed; type 3
    twist about axis 1 (else the bone) in [+0x60, +0x64], swing within ±+0x70 about the other two. Joints are built with
    the bodies in the bind pose, so a joint's zero is the bind pose: then the poacher's death pose (l4_ballcourt 6540)
    lies inside every limit to a few degrees (knees 43° in −10..140 and −51° in −140..10, elbows −5° / 3° at their
    −125..−5 / 5..125 ends, hips −22° / 30°, shoulders −3° / 17°, head −13° in ±20°) [O measured]; friction
    as the motor bounds, × 30 × 60 / 280² N·m. Parent and child bones do not collide (`GroupFilterTable`). It starts at
    the pose at the takeover, each body moving as its segment did over the last tick, with the killing hit's impulse
    (death record +0x4C × 2.5 along the shot as 0x696A60; [I] 2000 when the record has none: soldiers, cats) at the bone
    nearest the hit point. Every other skeleton segment follows its nearest ancestor-or-self that starts a bone, with its
    takeover pose offset. When every body sleeps (Jolt's 0.5 s rest) or 20 s after the death [I], the pose freezes and the
    bodies go (`ragdollPhase` 3).
  - Timing and saves: bodies are made right after the enemy tick that starts the ragdoll (so a save between ticks holds
    them), poses are read after each `World::stepPhysics`; enemy saves carry `ragdollPhase`, each bone's frame in
    model space and the killing hit; the scene's save keys the bodies `r<uid>.<bone>` and lists the joints after the
    vehicles, so a load re-creates them at the saved IDs and the fall continues bit for bit
    (`GameReal.KilledPoacherRagdollIndependentOfRenderRate`, `StreamReal.RagdollSaveLoad`).

- Vehicles (`physics::VehicleSpec`, `Scene::setVehicles` / `vehicle`; the bike's data and decode: docs/GAMEPLAY.md
  "Motorcycle"): a Jolt `VehicleConstraint` (ray collision tester, `WheeledVehicleController`) on a body of the
  vehicle model's primitives (layer vehicle: it hits the terrain, the colliders and loose bodies). Per wheel: attachment
  at the suspension top, maximum length = the rest length (the ray is rest + radius to the contact, as 0x792900), the
  spring at its frequency / damping ratio, its forces at one point 0.7 of the travel above full extension, the data's
  tyre peak and brake torque, the max steer angle on steered wheels. The drive is tru.exe's: `Impl::drive` computes
  rpm from the forward speed (Σ share · v / radius × ratio), the torque (curve, none above the top rpm; reverse fixed)
  and adds share · torque · throttle · ratio / radius along the body's forward axis at each driven wheel on the
  ground, before the step. Jolt's drivetrain is not used: its clutch and light wheels spun the wheels up and starved
  the drive; its `MotorcycleController` lean spring pumped energy into this tall 265 kg chassis and let a parked bike
  fall over, so the roll about the forward axis (from the world's up) is servoed to the caller's `VehicleDrive::lean`
  (angular velocity `leanRate` × (lean − roll), on the ground and in the air; tru.exe 0x793680 drives a motor on the body
  toward its lean target). `VehicleDrive` also carries a yaw torque and a side force at the centre of mass (air control),
  a force at one wheel's contact (stunts), a horizontal pin of one wheel's contact and a cap on the spin about the up
  axis (donut / power slide [I]), and the hand brake (`mMaxHandBrakeTorque`). Inputs arrive with `setVehicles` and are
  applied after a restored state; `shift` follows the caller's stream offsets, `kick` adds velocity (hop, ram). Saves:
  the body is keyed `v<id>`, the constraint is listed after the
  links (before the ragdoll joints); Jolt's vehicle state (wheel spin, contacts, lambdas, inputs) is in the recorded
  state; the drive reads only the saved contact bodies, so a load continues bit for bit
  (`Physics.VehicleDrivesBrakesAndSavesBitIdentically`, `VehicleReal.SaveRestoreMidRide`).

## Character controller

Decision (2026-11-10, gameplay goal §4, plan in docs/audit/AUDIT_B.md "CharacterVirtual evaluation plan"): **Lara keeps
her own mover** (the capsule sweep on the collision BVH with a ground ray under the root, `src/game/Player.cpp`
`slide` / `groundMove` / `airMove`). Jolt's `CharacterVirtual` was built in as a second, complete mover, run through
the comparison tests and measured against the Wine traces; it fails 8 of the 26 comparison tests in either shape
tried and is further from tru.exe than the sweep mover on every traced case. The Jolt mover and its switch were
removed again; everything below is reproducible from commit `84eb0be` of branch `agent/GpCharVirtual` ("Reproduce").

### What tru.exe does [O]

Lara's body in the level is a Havok proxy capsule on her instance (+0x288: radius +0x65C 79.01, cylinder +0x660
189.63, centre +0x640 315.4 above the feet; built by 0x7018E0, resized by 0x490E60). Its bottom is 141.6 above her
feet: it does not rest on the floor. The root takes the floor height under it: running up the manor staircase (Wine
trace `screens/stairs/t4.txt`, risers 94.5..109.7) her z changes by a whole riser between two 16 ms samples as the
root crosses the riser line, at unchanged run speed; the 121-unit hearth edge stops her 38..72 short; on a floor of
normal z 0.45 she slides at g sin(theta). So the original is "a floating capsule that walls push sideways + the floor
height under the root", which is the sweep mover's scheme (capsule bottom lifted by `stepUp` 110, ground ray).

### What was built for the evaluation

- `physics::Scene::setController(CharacterSpec)` / `moveCharacter(feet, velocity, top, dt, walk)` /
  `characterFits(feet, top)`: one `JPH::CharacterVirtual` (up +Z, mass 55 kg, max slope acos 0.7, padding 2 units,
  back faces collide as the BVH's double-sided queries, enhanced internal edge removal on), `ExtendedUpdate` for
  walking (stick to floor 110, stairs 110) and `Update` in the air; its state in `Scene::save` (layout version 2).
- Collision filtering as the BVH: while a controller exists the terrain of every resident unit and the kinematic
  object colliders are always present (they were built only while a loose body could land on them). Jolt 5.6 calls a
  `ShapeFilter` per sub-shape, not per triangle of a `MeshShape`, so a unit's terrain became a `StaticCompoundShape`
  of one mesh per terrain group (user data -> unit mesh + group; the filter reads `CollisionMesh::groupEnabled` live,
  so the script native that toggles a group needs no rebuild) plus one mesh of the faces no BVH query returns (flag
  0x40, mask 0), which the character skips and loose bodies still land on. Layers: Solid, Loose, Plate.
- `Player`: `Tuning::mover` (`OU_MOVER=sweep|jolt|jolt-capsule`, `ou-game --mover`) switched `groundMove`, the air
  step, the slope slide and the crouch headroom test to the controller (`src/game/Mover.cpp`): velocity = clip root
  delta / dt without smoothing, tilted along the ground face so the horizontal speed holds on slopes; in the air our
  0x5DB370 integration with Jolt's contacts clipping the velocity. The state chart, markup queries, jump solver,
  swimming and the rope stayed Player's. World fed the scene before the player tick; the dynamic proxy capsule
  (`setCharacter`) was not built with a controller.
- Two shapes: `jolt-capsule` = the plan's capsule (radius 79.01, top 489.2, bottom on the feet); `jolt` = a cylinder
  of the same radius and top, added when the capsule could not hold the 110 / 121 riser rule (below).

### The aspects investigated

| Aspect | CharacterVirtual (measured) | Against the sweep mover / tru.exe |
|---|---|---|
| Collision handling | Collide + cast against the Jolt broad phase, constraint solve over the contact planes (5 collision / 15 constraint iterations). Walls stop and slide her; no tunnelling seen | Equal to the 4-iteration sweep for walls. Rests a 0.05..0.25-unit tolerance above the floor (tests pin the root to the floor to 1e-3) |
| Character movement | Takes a velocity; the solved velocity is not written back (`GetLinearVelocity` returns the input): the achieved one is displacement / dt | Root-motion drive works (delta / dt); the air integrator stays exact; jump take-offs and apex unchanged (`Game.JumpTakeOffsMatchTruTraces`, `GameReal.BedroomJumpsMatchTruTraces` pass) |
| Sliding | Wall slide keeps the tangential speed (300 along, 0 into: `Physics.CharacterSlidesAlongWall`); steep ground is reported (`OnSteepGround`), the slide itself (g sin(theta), no cap) stays ours | Equal (`Game.SlideAcceleratesAtGravityAlongSlope`, `Game.SlidesDownSteepSlope` pass up to the hover tolerance) |
| Ground interaction | Ground = the contact of the shape's bottom with the most upward normal; needs the shape on the floor | Root height over a ramp: capsule 4.3 / 9.0 / 20.3 / 31.5 units at normal z 0.95 / 0.90 / 0.80 / 0.72, cylinder 25.5 / 37.7 / 58.8 / 75.8 (r tan(theta): its uphill rim); sweep mover and tru.exe 0 (floor under the root) |
| Slope handling | `mMaxSlopeAngle`; `ExtendedUpdate` cancels velocity toward steep contacts. Without tilting the velocity along the ground Jolt takes the slope's share off the speed uphill (cos^2: 0.49 at the limit); with the tilt 1274 of 1274 units/s at every walkable slope. The tilt must use the face normal: the contact normal at a step's edge leans and launched her off 50-unit ledges | Steep slivers and bevels count as steep ground: `Game.RunsOverSteepSliverWithoutFalling` (the l4_ragnarok_room 0.6-normal strips) fails with the cylinder (blocked) and the capsule alternates Run / Fall on the real strips (13 changes in 180 ticks, sweep 0) |
| Stair stepping | `WalkStairs` steps up, forward, down when a steep contact blocks. Capsule: the round bottom also rides up an edge by itself, risers past the step height are climbed depending on which triangle the down-cast reports at the edge (table "Riser limit"), each riser takes 4..10 ticks at reduced speed and starts 62..196 units before the riser line. Cylinder: crisp limit (108 climbed, 121 blocked) in one tick, 60..84 units before the riser line (its radius) | tru.exe: at the riser line, one sample. Sweep: within one tick's travel of tru's line (-21..+8 units). Riser survey (346 walk / run attempts in 7 units): sweep 346, cylinder 336, capsule 332 |
| Collision filtering | Object layer filter + the per-group compound and a `ShapeFilter` (above) | Equal: `GameReal.TerrainGroupNativeUpdatesMergedCollision` passes on both; mask-0 faces skipped (`Physics.CharacterCrouchShapeAndFaceMask`) |
| Moving platforms | A kinematic collider under her gives `GetGroundVelocity`; rising platforms lift her, `StickToFloor` follows them down; standing still she needs the ground velocity added by the caller or she is left behind | World's `carry` kept (no double carry: the ground velocity was not added). `GameReal.ElevatorCarriesPlayer`, `GameReal.SplineMoverCarriesLara` pass; `ProgressionReal.LoweredRampSectionCarriesLara` fails with the cylinder (root 40.8 above the tilted ramp) |
| Update ordering in `World::tick` | The controller updates inside `Player::tick`, before scripts, objects and the physics step, reading the bodies as the last step left them (kinematic colliders one tick behind, as the BVH's dynamics are for the sweep mover); the terrain / collider lists have to be synced before the player tick as well as before the step | Works; costs a second sync per tick. A loaded game must rebuild controller, terrain and colliders before the first save: `StreamReal.SaveContinuesBitIdentically` reports differences in 5 of 14 scenarios (physics state of a load without a tick; one uber object one tick after the load) |
| Shape handling | `SetShape` (crouch 260) with the penetration test as `characterFits`: blocked standing under a 300-unit slab, passes crouched, cannot stand up there | Equal (`Game.CrouchUnderSlabAndRoll` passes) |
| Inner edges on mesh seams | Enhanced internal edge removal works inside one body. Between two units' bodies it does not apply: the doorway l4_pool -> l4_ragnarok_room (the next unit's floor starts 19.5 higher, no riser face) stops the cylinder for good (the edge of a horizontal face is not a "steep" contact, so `WalkStairs` is never tried; independent of the forward test 42 / 90 / 170, padding 0.5 / 2 / 5.6, edge removal on / off); the capsule rolls over it losing 15 units and overshooting 2.6 in z | Sweep: steps at the seam in one tick (replay `pool_to_ragnarok` tick 104). The cylinder also rises 1.7 units crossing the far edge of a 50-unit box |
| Impulses on dynamic bodies | Contacts with dynamic bodies push them by `mMass` / `mMaxStrength` inside the character update: a 50 kg box moved 201 units in 2 s of walking at 348 units/s, she 456 of 696 (`Physics.CharacterPushesLooseBody`); a 60-unit box is a stair to her and she walks over it | Replaces the dynamic proxy capsule of `setCharacter`; no original numbers exist for either ([U]: tru.exe's proxy mass and push are not traced) |

### Measurements

Comparison tests (the plan's list; P pass, F fail, T = fails only an assertion that pins the root to the floor
within 1e-3 while the controller rests 0.05..0.25 above it):

| Test | sweep | jolt (cylinder) | jolt-capsule |
|---|---|---|---|
| `Game.WalkingIntoWallSlides` | P | T (z 0.09) | T (z 0.12) |
| `Game.StepsUpAndDownSmallLedgesButNotWalls` | P | F (z 51.7 on the 50-unit box) | T |
| `GameReal.StairsWalkAndRunUpAndDown` | P | F (first: l4_staircase_mictlan) | F (first: l3_main_hall; climbs the hearth) |
| `Game.RunsOverSteepSliverWithoutFalling` | P | F (blocked at x 219) | P |
| `Game.WalkingSlowlyOffEdgeFallsOnce` | P | P | P |
| `Game.RunsOffEdgeFallsAndLands` | P | P | T (z 0.24) |
| `Game.SlidesDownSteepSlope` | P | P | T (z 0.09) |
| `Game.SlideAcceleratesAtGravityAlongSlope` | P | P | P |
| `Game.GravityJumpAndLanding`, `Game.JumpTakeOffsMatchTruTraces`, `GameReal.BedroomJumpsMatchTruTraces` | P | P | P |
| `Game.CrouchUnderSlabAndRoll` | P | P | P |
| `GameReal.PoolRunReplayStaysOnFloor`, `GameReal.PoolRockStepDown` | P | P | P |
| `GameReal.SplineMoverCarriesLara`, `GameReal.ElevatorCarriesPlayer` | P | P | P |
| `ProgressionReal.LoweredRampSectionCarriesLara` | P | F (40.8 above the ramp) | P |
| `Game.SimulationIndependentOfRenderRate`, `GameReal.CliffclimbIndependentOfRenderRate` | P | P | P |
| `StreamReal.SaveContinuesBitIdentically` | P | F (5 of 14 scenarios) | F (5 of 14) |
| `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies` | P | n/a (no proxy; `Physics.CharacterPushesLooseBody` instead) | n/a |
| self-tests `locomotion`, `ledge`, `fall_damage` | P | P | P |
| self-test `traversal_chain` | P | F (`run_on` 729 of >= 1000 units) | F (745) |
| self-test `sprint` | P | F (not exhausted in 12 s; 9.7 expected) | F (same) |
| Failed of 26 | 0 | 8 (1 T) | 8 (4 T) |

Riser limit (Scene level, `tools/mover_eval/cv_steps.cpp`: floor, a box of height h1, then a riser h2; walking at 348):

| riser h2 | capsule from h1 = 0 | capsule from h1 = 50 | capsule from h1 = 80 / 90 / 100 | cylinder from h1 = 0 / 50 / 100 |
|---|---|---|---|---|
| 97, 100 | climbed | — | climbed (from 100) | climbed |
| 105, 108 | climbed | — | climbed (105 from 100) | climbed (108) |
| 110 | blocked | — | climbed (from 100) | — |
| 112 | — | — | climbed (from 100) | blocked from 0, climbed from 50 and 100 |
| 121 | blocked | blocked | **climbed** | blocked |
| 130 | — | — | **climbed** (from 100; 300 is blocked) | blocked |

(`mWalkStairsStepUp` had to be 110 - r (1 - 0.7) = 86.3 for the capsule: its round bottom rides the last 23.7 by
itself. The 121 / 130 climbs happen when the down-cast after the step lands the sphere on the edge and reports the
top face's normal: she is then "on the ground" on the edge and the plain update rolls her up it. The real hearth is
such a case: the capsule climbs it and leaves the level.)

Manor staircase against tru.exe (`GameReal.MoverStairsTrace`, risers 1..9 of the Wine trace t4; lead = our y where z
changes minus the middle of tru's two samples around its jump, + = earlier):

| | tru.exe | sweep | jolt (cylinder) | jolt-capsule |
|---|---|---|---|---|
| z change of a riser | one 16 ms sample | 1 tick | 1 tick | 4..10 ticks |
| lead before tru's riser line | 0 (± 10 sample spacing) | -20.6..+7.8 | +60.3..+84.3 (risers 2..7) | starts +62..+196, on the tread +16..+32 (risers 2..9) |
| run speed on the flight (units per tick) | 21.2 (unchanged) | 21.23, min 21.23 | 19.87, min 9.93 | 19.96, min 6.85 |
| Fall states on the flight | 0 | 0 | 4 (top not reached in 130 ticks) | 0 |
| hearth edge (121): stops short by | 38..72 | 40.8 | 81.3 | climbs it |

Replays (ou-game, per-tick distance to the sweep mover's position; Fall / Land state changes):

| Replay | sweep | jolt (cylinder) | jolt-capsule |
|---|---|---|---|
| `pool_to_ragnarok` (180 ticks) | reaches l4_ragnarok_room at tick 104; 0 Fall / Land changes | stuck at the unit seam from tick 100 (x -16352.7): mean 388, final 1712 units behind; never leaves l4_pool | mean 9.7, max 34.1 units (z up to 28.0); 13 Run <-> Fall changes |
| `save_move_load` (to the load) | saved = loaded line | saved = loaded line (still in l4_pool) | saved in Fall; the loaded line differs ("saved scene not rebuilt alike") |

Cost: `Player::tick` 0.007 / 0.007 / 0.009 ms per tick in l4_pool and 0.011 / 0.010 / 0.007 ms in l2_junglehub (sweep /
cylinder / capsule; 569 ticks each, `World::tickTimes.playerMs`): no difference that matters. The always-present
terrain body costs about twice the BVH build per unit at stream-in (l4_pool 6043 faces: BVH 0.8 ms, Jolt 1.8 ms;
l2_junglehub 15052: 2.1 / 4.2 ms; l4_ragnarok_room 27971: 3.9 / 8.1 ms; the per-group compound costs the same as
one mesh).

### Decision and reasons

Rule (AUDIT_B): adopt the Jolt mover when all comparison tests pass and the Wine-traced cases are equal or closer
than today. Neither holds, so the sweep mover stays:

1. tru.exe's body does not stand on the floor (proxy bottom 141.6 up, floor height under the root). CharacterVirtual
   is built on the opposite model, a shape resting on the ground. Every traced number that depends on it came out
   further from the original: riser timing (60..196 units early against the sweep mover's ±21), the hearth stop
   (81.3 short, or climbed; the sweep mover 40.8, tru.exe 38..72), run speed on stairs, the root's height over ramps.
2. The capsule of the plan cannot keep the 110 / 121 riser rule on mesh edges; the cylinder that can is stopped by
   unit seams and steep bevels and floats over slopes. Each needs its own geometry-dependent repairs.
3. To match the original one would lift the shape off the floor and probe the ground under the root with a ray:
   that is the sweep mover on Jolt queries, with CharacterVirtual's ground state, stair walk and stick-to-floor
   unused, and a second copy of the level collision (plus filters and streaming) to keep consistent with the BVH that
   the camera, AI, markup and bullets use.
4. Adoption would re-record the 28 replays and the self-tests, bump the save format and still leave the failures
   above; it buys no measured gain (cost equal, wall slide equal, jumps equal).

Limits of the evaluation: knobs tried were the shape (capsule / cylinder), the stair step height, the second stair
test's distance (42 / 90 / 170), the padding (0.5 / 2 / 5.6) and internal edge removal (on / off); `mMaxStrength`,
the supporting volume and the predictive distance stayed at their defaults. Not pursued once the rule was decided:
the cause of 3 of the 5 failing save scenarios (2 are a load saved before the controller's bodies exist), why the
`sprint` self-test's run takes longer than 9.7 s, the other 26 replays, swimming's sphere sweep (not a candidate: free
3-D motion without ground).

What Jolt keeps doing: joints, loose bodies, ragdolls, the bike (above). What stays open for the sweep mover is
listed in docs/GAMEPLAY.md "Numbers"; the walkable slope limit, measured for this evaluation, is the next section.

### Walkable slope limit under Wine (2026-11-10)

Measured before trusting the 0.7 the evaluation was held to (`Tuning::minFloorNormalZ`, [I]). l2_junglehub, tru.exe =
ours + (8558.23, -19414.44, -18073.17): Lara put 15 units over large open faces of one normal z each
(`find_ramps`), left alone 3 s, traced (`pt trace`); every sample's position was then looked up in our collision mesh
(floor within 0.1 of her z: the frames agree) for the face under her. Traces and tools:
`openunderworld-work/gameplaygoal/charvirtual/` (`slope-wine2.txt`, `slope-session2.sh`, `slope-analyse.py`,
`probe_normals.cpp`, `client_hist.cpp`).

| Face under her: normal z | Face `clientFlags` (+0x0E) | tru.exe |
|---|---|---|
| 0.626, 0.679, 0.703, 0.716 | 0x0080 | stands still for the 3 s (speed 0) |
| 0.763 | 0x0000 | stands still |
| 0.974, 1.000 | 0x00A0 | stands still |
| 0.523 | 0x0004 | slides (153 -> 881 units/s in 0.5 s) |
| 0.584, 0.595 | 0x0004 | slides (2154 units/s on the 0.595 face) |
| 0.753 | 0x0004 | slides: 915 units/s horizontally with 800 down (twice, two trials); in one trial she is at rest on it for a sample and moving again 0.5 s later (197, then 915 units/s) |
| 0.770 | 0x0004 | at rest in the last sample of a trial (not conclusive) |
| 0.45 (l3_main_bedroom roof, earlier trace slide1.txt) | the unit's faces of normal z 0.3..0.5 are 0x0080 | slides |

[O] So the original's rule is not one normal-z threshold: she stands at rest on faces as steep as normal z 0.626
(ours slides below 0.7), and she slides on faces of normal z 0.753 that carry client flag 0x0004 (ours stands from
0.7). [I] Reading: bit 0x0004 marks slide surfaces (l2_junglehub has 120 x 10^6 units^2 of up-facing 0x0004 faces, a
third of it between normal z 0.5 and 0.8); for faces without it the limit lies between 0.45 (slides, 0x0080) and
0.626 (stands, 0x0080). [U] the exact limit for unflagged faces, whether a 0x0004 face is slid on at any slope or
from its own limit (>= 0.753), where tru.exe tests the bit (the slope-slide state's condition), and whether walking
onto such a face differs from standing on it. Not changed in the code by this evaluation: the sweep mover has the
face under her in every hit (`collision::Hit::face`), so the rule is a change to `Player::standable` / `groundMove`
once the two limits are bracketed (a second ramp series between 0.45 and 0.626 on 0x0080 / 0x0000 faces, and on
0x0004 faces above 0.753).

### Reproduce

`/usr/bin/git worktree add <dir> 84eb0be`, configure and build `ou_physics_tests ou_game_tests ou_gates_tests
ou_save_tests ou-game`; then per mover (`OU_MOVER=sweep|jolt|jolt-capsule`, `OU_GAME_DIR` set): the gtest filters of
the table (`ou_game_tests`, `ou_gates_tests`, `ou_save_tests`), `GameReal.MoverRiserSurvey`, and inside a private
compositor `tools/mover_eval/eval.sh` followed by `tools/mover_eval/analyse.py` (stairs lead, hearth, replay
distances, state changes, tick costs, the five self-tests). Scene-level tools in `tools/mover_eval/`: `cv_steps.cpp`
(riser series), `cv_slope.cpp` (speed and root height on ramps), `cv_real.cpp` (a run in real units, `OU_UNIT2` for
the seam), `cv_cost.cpp` (terrain body build), `find_ramps.cpp` + `slope-session.sh` (the Wine ramp series). The
Scene API's own tests are `Physics.Character*` in that commit (step 110 / 121, slope limit, wall slide, crouch shape
and face mask, moving platform, push, save / restore bit-exact).

## Havok → Jolt

| Havok (tru.exe) | Jolt (ours) | Evidence |
|---|---|---|
| Game units, Z up | metres = units / 280 | bike speed unit 0x105B30C: 280 units per metre [O] |
| gravity 5.72443 units / 30 Hz frame² | `SetGravity(0, 0, −5.72443 · 900 / 280)` m/s² | Lara's gravity [O]; same on bodies [I] |
| solver step | `Update(1/60 s, 1 collision step)` per game tick | motor bounds vs weights [I] (above) |
| rigid body mass / inertia (0x497270, 0x48E650 / 0x48E330) | `EOverrideMassProperties::MassAndInertiaProvided`, centre of mass via `OffsetCenterOfMassShape` | `massProps` = the exe's per-shape formulas [O] |
| primitives 1 sphere, 5 box, 10 capsule (13 mesh-like) | `SphereShape`, `BoxShape` (full sides / 2), `CapsuleShape` (length / 2 along Z) in a `StaticCompoundShape` | HInfo layout [O]; capsule axis [I]; type 13 not decoded [U] |
| revolute (1) / prismatic (3) joint, frame = model marker × instance, axis = frame Z | `HingeConstraint` / `SliderConstraint`, body 1 = world | 0x4D4240, 0x4DF5D0 / 0x4DFC10 [O]; body A = world [I] |
| joint limit (degrees / units), on flag | `SetLimits(lo, hi)` (rad / m), hard; the constraint's zero is moved into the limits (Jolt wants 0 inside them, hinges within ±π) | 0x4D4510 [O] |
| motor speed per second, bound (<= 0 unbounded) | velocity motor: hinge deg/s · π/180 rad/s, slider units/s / 280 m/s; bound · 30 · 60 N (/280 for sliders, /280² N·m for hinges); unbounded = `FLT_MAX` | 0x4D2B00 / 0x4D29F0 [O]; bound = impulse per 1/60 s step [I] |
| link joint (gear ratio) | `LinkConstraint` row `r·q1 − q2 = 0` (+ Baumgarte drift fix) | 0x4D7ED0 -> 0x4E17B0 [O] |
| range watch (0x4937A0) | reads `Joint1::x / v` written back after the step | [O] |
| hinge door / lever (class 34 / 6 marker X axis) | `HingeConstraint`, velocity motor toward the target at up to 1 rad/s, landing on it in the step that reaches it; angular damping 0.5 / s | all [I] (our controller) |
| friction / restitution / damping of loose bodies | Jolt defaults (friction 0.2, restitution 0, damping 0.05); penetration slop 1 unit | [U] Havok values not decoded |
| ragdoll bone capsule (0x4F0B40), mass share, radius × scale | dynamic body per bone, `CapsuleShape` from start to end, `CalculateInertia` with the given mass | [O] layout; Havok friction / damping not decoded [U] |
| ragdoll joints 1 / 2 / 3 (0x4DF5D0 / 0x4E08A0 / 0x4EE5C0) | `SixDOFConstraint`, pyramid swing, zero = bind pose (above) | constructors [O]; axis roles and zero [I] |
| vehicle body (0x6AB960 -> 0x497270) and its upright limit (0x795080, record +0x40) | dynamic body of the model's primitives minus the wheel spheres, `LinearCast`; `VehicleConstraintSettings::mMaxPitchRollAngle`; roll servoed to the lean target | [O] data; the servo's rate [I] (above) |
| wheel: ray from the suspension top, spring constraint at the hit (0x792900 / 0x4E1A40) | `WheelSettingsWV` + `VehicleCollisionTesterRay`: max length = rest, radius = sphere, spring Hz / damping ratio, `mSuspensionForcePoint` at 0.7 of the travel above full extension | [O] layout and formulas; one fixed force point [I] |
| engine torque on the wheels (0x791D10 / 0x792900) | `Body::AddForce` per driven wheel at its force point (Jolt's engine, gearbox (neutral) and clutch unused) | [O] |
| wheel friction / brake (0x4E1A40 parameters) | Jolt slip curves scaled to wheel +0x08; `mMaxBrakeTorque` from +0x0C, `mMaxHandBrakeTorque` from +0x10 | [I] meanings |
| body forces of 0x793680 / 0x7918B0 / 0x790920 (`+0xF0` torque, `+0xE0` force, force at a point) | `AddTorque` / `AddForce` before the step, N·m and N (game units / (280²/30²) and / (280/30²)) | accumulators 0x4DD7E0 / 0x4DD710 / 0x4DD750 [O]; units [I] |

## Verification

- `ou_physics_tests`: box inertia vs the exe formula, hinge motor opens to 80° and closes, limits stop a free swing,
  render-rate determinism, uber joint fall / lift / link, loose bodies land, stack and tumble, dynamic raycast / sweep /
  closest / ground probe follow the transform and the mask; real data: door decode, all objects parse.
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
