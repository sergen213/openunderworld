# Technical audit, part B: Jolt physics, combat, objects, puzzles, motorcycle, game systems, tests, quality

Scope: gameplaygoal.md §1 (audit) applied to §4, §8, §9, §10, §11, §14, §17, §19. Project state audited: OpenUnderworld
main 4437cb5 (v0.41.0, 658 gtest cases in `tests/test_*.cpp`, 28 files in `tests/replays/`). Where `int/0.42.0`
(4706283, `ou-game --self-test`) changes the picture it is said so. Method: the code was read (`src/physics/Physics.*`,
`src/game/Combat.*`, `Gadgets.cpp`, `Objects.cpp`, `Vehicle.*`, `Progress.*`, `Stream.cpp`, `World.cpp`, `Grapple.cpp`,
`src/script/Script.cpp` block code, `src/ui/Ui.hpp`, `CMakeLists.txt`), the test names were listed from the sources
(`audit/.scratchAuditB/tests.txt`) and the docs were grepped per mechanic. Nothing was built or run for this audit.
Movement, grapple gameplay, swimming, animation and camera are part A (`AUDIT_A.md`).

Abbreviations: Cmb = `src/game/Combat.cpp`, Cmb.h = `Combat.hpp`, Gad = `Gadgets.cpp`, Obj = `Objects.cpp`,
Veh = `Vehicle.cpp`, Veh.h = `Vehicle.hpp`, Str = `Stream.cpp`, W = `World.cpp`, G = `Grapple.cpp`, P = `Player.cpp`,
Phy = `src/physics/Physics.cpp`, Phy.h = `Physics.hpp`, Scr = `src/script/Script.cpp`, GP = `docs/GAMEPLAY.md`,
PR = `docs/PROGRESSION.md`. Test names without a file: `tests/test_combat.cpp` (Combat / Gadgets / Tailoring),
`test_game.cpp` (Game / GameReal), `test_gates.cpp` (ProgressionReal), `test_physics.cpp` (Physics),
`test_vehicle.cpp` (VehicleReal), `test_save.cpp` (Save / Stream / StreamReal), `test_progress.cpp` (Pickups / Menus /
Chapters / PdaReal), `test_ai.cpp` (Ai / AiReal).

Classification = the goal's §1 categories. Status = the contract's vocabulary. A row marked VERIFIED AGAINST ORIGINAL
means its numbers are decoded from the game data / tru.exe or measured under Wine and a named test pins them; behaviour
round those numbers that is inferred is named in the last column.

Headline findings (details in the rows and the worklist):

1. Lara does not move on Jolt. Her controller, every gameplay ray / sweep, the camera, the AI and the projectiles use
   the project's own collision BVH (`src/collision`). Jolt simulates joints, loose bodies, pressure plates, ragdolls and
   the motorcycle. The goal's CharacterVirtual evaluation had not been done or documented at the audit (done since:
   `docs/formats/PHYSICS.md` "Character controller"; the sweep mover stays).
2. Combat has no melee at all (no kicks, jump kicks, finishing moves), no evasive jumps or lock-relative dodges, no
   enemy vault, no knock-down of Lara, no dual targeting, and shooting from a ledge / ladder / pole is refused.
3. The reward table holds 78 treasures and 6 relics; the original has 179 and 6. The 101 missing treasures are the ones
   released by breakable urns / jars, whose rewards are never spawned. Relics have no effect on health.
4. FIXED (agent/GpSystems, 0.43): `World::respawn()` cleared `combat_` and did not roll scripts, pickups or objects
   back to the checkpoint. It now restores the checkpoint save as tru.exe's "Restore Game" 0x557130 does
   (docs/GAMEPLAY.md "Checkpoints and respawn").
5. The adrenaline head shot opens from adrenaline mode with any meter level (Cmb:1313); the documented rule is a full
   meter. The pip / window numbers are [I].
6. Breakables break only from bullets, bolts and the ground pound: contact triggers 6 / 7 / 8 (kick, roll, bike, falls)
   and the class-30 breakables are not implemented.

---

## §4 Jolt Physics

### Physics responsibilities ("use Jolt as the primary foundation for")

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Collision detection | needs refactor | IMPLEMENTED | Own BVH `collision::CollisionMesh` (per-unit parts + `dynamics`) for Lara, enemies, camera, shots; Jolt only between its own bodies (Phy `Impl` layers) | `Collision.*`, `CollisionRealData.*`, `Physics.DynamicCollisionQueriesFollowTheTransform` | terrain masks / groups [O] | Two collision worlds: the same terrain is a BVH for gameplay and a Jolt `MeshShape` for bodies (Phy:437 `meshShape`). Not "Jolt as the primary foundation" |
| Character movement constraints | needs refactor | IMPLEMENTED | P:348 `Player::slide`, P:381 `groundMove`, P:441 `airMove` on `col_.sweepCapsule` | `Game.WalkingIntoWallSlides`, `GameReal.StairsWalkAndRunUpAndDown` | step 110, capsule [O Wine] | Not Jolt. See "CharacterVirtual evaluation plan" |
| Collision queries | needs refactor | IMPLEMENTED | `col_.raycast / sweepSphere / sweepCapsule / groundProbe / closestPoint` everywhere in `src/game` | `Collision.MatchesBruteForce`, `Collision.SphereAndCapsuleSweeps` | — | Jolt queries are used only for wheel rays (`VehicleCollisionTesterRay`, Phy:1099) and the plate weight ray (Phy:778) |
| Raycasts | needs refactor | IMPLEMENTED | `CollisionMesh::raycast` (targeting LOS Cmb:912, shots Cmb:1018, rope G:39, camera) | `Collision.RaycastFloorAndWall`, `Collision.MaskAndIgnoredFaces` | masks [O] / object mask convention [I] (Phy.h `kObjectMask`) | Own code |
| Shape casts | needs refactor | IMPLEMENTED | `CollisionMesh::sweepSphere / sweepCapsule` | `Collision.SphereAndCapsuleSweeps` | — | Own code; rope uses one ray per segment where tru.exe casts a 4.74-radius sphere (G:57 [I]) |
| Rigid body simulation | implemented + works | IMPLEMENTED | Phy `Scene::setLoose`, `Impl::body`, `looseShape` (all HInfo primitives, data mass / inertia) | `Physics.LooseBodiesLandStackAndTumble`, `Physics.MassPropsBoxMatchesExeFormula`, `Physics.SavedSceneContinuesBitIdentically` | mass / inertia formulas [O 0x4E9100 ...]; friction, restitution [I] | Havok material numbers unknown |
| Dynamic environmental objects | partial | PARTIALLY IMPLEMENTED | Obj:1789 `stepPhysics`: hinge doors, uber joints + links, plates, loose bodies; push blocks are kinematic (Scr:2445) | `Physics.UberJointFallsLiftsAndLinks`, `GameReal.PressurePlateIndependentOfRenderRate`, `GameReal.PushBlockIndependentOfRenderRate` | joints [O data]; motors [I] | Push blocks are not Jolt bodies; uber joint body A is always the world; fixed joints and joint types 5 / 7 not simulated (ROADMAP "Jolt" step 1) |
| Physical object interaction | partial | PARTIALLY IMPLEMENTED | Lara's proxy capsule (Phy:752 `driveCharacter`) shoves loose bodies and ragdolls; `World::uberShot` (Obj:804); `Scene::blast`; plate weights (Phy.h `setPlates`) | `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`, `ProgressionReal.KrakenColumnShot` | Lara 55 kg [O ragdoll record]; shot impulse 3000 [I]; blast as velocity [I] | Contact impulse triggers 6 / 7 / 8 of uber sections not simulated (GP "Breakables"); a shot pushes every watched joint of the object, not the struck body (Obj:800) |
| Gravity | implemented + works | IMPLEMENTED | Phy:568 `SetGravity(-5.72443 x 30^2 / 280 m/s^2)`; Lara P `Tuning::gravity` | `Game.GravityJumpAndLanding`, `Physics.UberJointFallsLiftsAndLinks` | Lara's [O bit pattern 0xC0B72E88]; for bodies [I] | Cmb.h projectile comment cites "the world's -3 per frame^2" (0x6C91D0) while the scene uses 5.72443 for every body: which value Havok's world has is [U]; read 0x6C91D0 and the world set-up |
| Contact handling | partial | PARTIALLY IMPLEMENTED | Jolt contacts for bodies (penetration slop 1 unit, Phy:570); Lara's contacts are her own sweep loop | `Physics.LooseBodiesLandStackAndTumble` | slop [I] | No contact listener: nothing in gameplay reads Jolt contacts (crushers use a geometric depth, Obj:1017 [I]) |
| Moving platforms | implemented + works | IMPLEMENTED | kinematic colliders (`Scene::setColliders`, `CollisionMesh::dynamics`), `script_.carried()` / uber carry (W tick) | `GameReal.SplineMoverCarriesLara`, `GameReal.ElevatorCarriesPlayer`, `ProgressionReal.LoweredRampSectionCarriesLara` | paths / commands [O] | Camera on moving platforms [U] (W:729); carry is applied by the world, not by a character controller |
| Physical constraints | partial | IMPLEMENTED | Phy: `HingeConstraint`, `SliderConstraint`, custom `LinkConstraint` (Phy:370), `SixDOFConstraint` (ragdolls), `VehicleConstraint` | `Physics.HingeMotorOpensToTargetAndHolds`, `Physics.HingeLimitsStopAFreeSwing`, `Physics.RagdollFallsDownAStepJointsHoldAndSaves` | limits [O]; motor bounds per 1/60 s [I] | Fixed joints, cylindrical two-axis joints (`ca_rb_crusher_slim`, Obj:2097 [U]) |
| Motorcycle physics | works but differs from original | IMPLEMENTED | Phy:1064 `VehicleConstraintSettings` + `WheeledVehicleController`; engine as wheel forces (Phy:1129) | `Physics.VehicleDrivesBrakesAndSavesBitIdentically`, `VehicleReal.*` | record [O data]; Jolt tyre model, brake units [I] | See §11 |
| Vehicle collision | partial | IMPLEMENTED BUT UNVERIFIED | chassis body layer `kLayerVehicle` against solids, loose bodies, plates; `World::ramEnemies` (Veh:286) | `VehicleReal.RamsEnemyOnTheTrail` (enemy only) | ram numbers [I] | No test drives the bike into a wall; enemies are not bodies; crash braces not played |
| Grapple-related physical interactions | partial | PARTIALLY IMPLEMENTED | rope: own pendulum (G); pulled weights become loose Jolt bodies (`Object::loose`); pull of a block 240 u/s (W:596 [I]) | `GameReal.GrapplePullsUberTargets`, `GameReal.MountedGrapplePullsBlockWithoutSwinging`, `GameReal.GrappleLeverJointIndependentOfRenderRate` | [I] forces | The rope is not a Jolt constraint and applies no force to what it is hooked on; see "Grapple physics" |
| No second physics engine | needs refactor | IMPLEMENTED | — | — | — | There is no second engine library, but the hand-written character / rope / projectile / grenade integrators plus the BVH amount to a second, parallel physics path |
| Gameplay separated from Jolt details | implemented + works | IMPLEMENTED | Jolt is linked PRIVATE to `ou_physics` (CMakeLists:268); `Phy.h` exposes game-unit structs only | build | — | — |

### Lara's character controller: "investigate CharacterVirtual's ..."

None of these has been investigated in the repository: no file under `src` or `docs` names `CharacterVirtual` and
ROADMAP "Planned: Jolt Physics" has no character step. One row each, with what our own controller does today.

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Collision handling | missing | NOT IMPLEMENTED (evaluation) | today: P:348 `slide` (4 iterations) | — | — | Evaluate per the plan below |
| Character movement | missing | NOT IMPLEMENTED (evaluation) | today: root motion -> `groundMove` | — | — | same |
| Sliding | missing | NOT IMPLEMENTED (evaluation) | today: wall slide with flattened normals; slope slide state | — | — | same |
| Ground interaction | missing | NOT IMPLEMENTED (evaluation) | today: `groundProbe` ray + `standable` 4 probes (P:372) | — | — | same |
| Slope handling | missing | NOT IMPLEMENTED (evaluation) | today: `minFloorNormalZ` 0.7, `slideMinNormalZ` 0.3 | — | — | same |
| Stair stepping | missing | NOT IMPLEMENTED (evaluation) | today: capsule bottom lifted by `stepUp` 110 + snap ray | — | — | same |
| Collision filtering | missing | NOT IMPLEMENTED (evaluation) | today: face masks, terrain group enable | — | — | same |
| Moving platform interaction | missing | NOT IMPLEMENTED (evaluation) | today: world adds `carry` | — | — | same |
| Character update ordering | missing | NOT IMPLEMENTED (evaluation) | today: W:532 tick order (player before scripts, objects, physics) | — | — | same |
| Shape handling | missing | NOT IMPLEMENTED (evaluation) | today: capsule height switch `capHeight()` (crouch 260) | — | — | same |
| Selection through gameplay tests + written rationale | missing | NOT IMPLEMENTED | — | — | — | No evaluation, no document |

### Lara's character controller: "implement"

(Behaviour detail is in AUDIT_A §5; here only whether the mechanism exists and on what.)

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Character acceleration | works but differs from original | IMPLEMENTED | ground: none, the clip's root motion is the move (P `advanceClip` -> `groundMove`); swim ramps `Player::swim` | `Game.SwimSpeedMatchesTru`, `GameReal.LaraClipsAndRootMotion` | ground animation-driven [O 0x73FE10 warning]; swim 2475 / 3760 [O Wine], linear form [I] | No start / stop clips on foot (AUDIT_A) |
| Movement velocity | implemented + works | VERIFIED AGAINST ORIGINAL | root motion walk 348 / run 1274 | `GameReal.LaraClipsAndRootMotion`, `Game.SprintStaminaMatchesTru` | [O clips, Wine] | — |
| Directional movement | implemented + works | IMPLEMENTED | camera-relative stick (W tick), `turnRate` 6 rad/s | `Game.TurnInPlaceUsesClipRootRotation` | turn rate [I] | — |
| Grounded state detection | implemented + works | IMPLEMENTED | P:384 ground ray +-110, `standable` | `Game.RunsOverSteepSliverWithoutFalling`, `Game.WalkingSlowlyOffEdgeFallsOnce` | 110 [O Wine]; 4-probe rule [I] | `standable` is a fix for seams (comment "ponytail: four fixed probes") |
| Airborne movement | implemented + works | VERIFIED AGAINST ORIGINAL | P:441 `airMove`, exact integrator | `Game.GravityJumpAndLanding`, `Game.JumpTakeOffsMatchTruTraces` | [O 0x5DB370] | — |
| Gravity | implemented + works | VERIFIED AGAINST ORIGINAL | `Tuning::gravity` | same | [O] | — |
| Jumping | implemented + works | VERIFIED AGAINST ORIGINAL | P JumpPrep / Jump | `GameReal.BedroomJumpsMatchTruTraces` | [O Wine] | see AUDIT_A |
| Collision-aware movement | implemented + works | IMPLEMENTED | `slide`, `airMove` contact loop | `Game.WalkingIntoWallSlides` | skin, iteration count [I] | — |
| Sliding | implemented + works | VERIFIED AGAINST ORIGINAL (slope acceleration) | P State::Slide | `Game.SlidesDownSteepSlope`, `Game.SlideAcceleratesAtGravityAlongSlope` | g sin(theta) [O Wine]; 0.3 bound [I] | — |
| Moving platform support | implemented + works | IMPLEMENTED | W tick `carry` | `GameReal.SplineMoverCarriesLara`, `GameReal.ElevatorCarriesPlayer` | — | Rotating platforms carry only for calendar rings (W:647) |
| Environmental traversal interactions | implemented + works | IMPLEMENTED | markup queries (ledges, poles, ...) | AUDIT_A | — | AUDIT_A |
| Character collision dimensions | works but differs from original | IMPLEMENTED | `Tuning` radius 79.01, top 489.2, crouch 260, swim sphere | `GameReal.StairsWalkAndRunUpAndDown` | radius / top / crouch [O Wine, data] | Capsule bottom is `stepUp` (110) where tru.exe's proxy bottom is 141.6; the per-state proxy targets (+0x6B0 / +0x6B4) are [U]. GP "Numbers" still says "our capsule stays radius 100" (docs older than the code) |
| Surface interaction | partial | PARTIALLY IMPLEMENTED | footstep / impact effects by surface material | `FxReal.BulletHitPicksParticlesBySurfaceMaterial`, `AudioReal.MaterialListAndEngineReaction` | material ids [O] | No per-surface movement rule (ice, mud) is modelled; whether the original has any is [U] |

### Physics tuning

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Gravity | implemented + works | VERIFIED AGAINST ORIGINAL | `Tuning::gravity`, Phy.h `kJointGravity` | `Game.GravityJumpAndLanding` | [O] | bodies' gravity [I] (see above) |
| Character dimensions | works but differs from original | VERIFIED AGAINST ORIGINAL (radius, top) | `Tuning` | `GameReal.StairsWalkAndRunUpAndDown` | [O Wine] | capsule bottom (above) |
| Movement speeds | implemented + works | VERIFIED AGAINST ORIGINAL | clips; swim 1348.6; sprint | `Game.SwimSpeedMatchesTru`, `Game.SprintStaminaMatchesTru`, `Game.SwimSprintMatchesTru` | [O Wine] | — |
| Acceleration | works but differs from original | IMPLEMENTED | swim ramps | `Game.SwimSpeedMatchesTru` | S-shaped in tru.exe, linear here [I] | — |
| Deceleration | works but differs from original | IMPLEMENTED | swim 3760; ground `Stop` clip | same | [O Wine] / [I] | — |
| Ground friction | implemented + works | IMPLEMENTED BUT UNVERIFIED | none on foot (animation-driven); slide has none | `Game.SlideAcceleratesAtGravityAlongSlope` | no friction on slides [O Wine] | Body friction 0.6 for ragdolls (Phy:1341 [I]), Jolt default for loose bodies |
| Jump velocity | implemented + works | VERIFIED AGAINST ORIGINAL | `Tuning::jumpVz` etc. | `Game.JumpTakeOffsMatchTruTraces` | [O Wine] | — |
| Jump trajectory | implemented + works | VERIFIED AGAINST ORIGINAL | `airMove`, solver port of 0x73EDC0 | `GameReal.BedroomJumpsMatchTruTraces`, `GameReal.HangLeapsMatchTruTraces` | [O] | assist radius 300 [I] |
| Air control | implemented + works | IMPLEMENTED | none: horizontal speed constant in the air | `Game.JumpTakeOffsMatchTruTraces` | [O Wine: constant] | — |
| Landing behavior | partial | PARTIALLY IMPLEMENTED | `Player::land`, fall damage | `Combat.FallDamageThresholds` | damage [O]; landing clip choice [I] | hard-landing / roll-out rules not decoded |
| Slope limits | works but differs from original | IMPLEMENTED BUT UNVERIFIED | `minFloorNormalZ` 0.7 | `Game.SlidesDownSteepSlope` | [I] | The original's walkable limit is not measured |
| Collision tolerances | works but differs from original | IMPLEMENTED BUT UNVERIFIED | `kSkin`, `wallOffset` 100, grab windows | — | [I] | — |
| Environmental object mass | implemented + works | VERIFIED AGAINST ORIGINAL | Phy.h `massProps` (HInfo `mass` u16) | `Physics.MassPropsBoxMatchesExeFormula`, `PhysicsRealData.AllObjectsParse` | [O] | plate without primitive mass 50 kg [I] (Obj `kPlateMass`) |
| Physical interaction forces | works but differs from original | IMPLEMENTED BUT UNVERIFIED | `kShotImpulse` 3000 (Obj:805), plate motor `kHold` 20, blast speeds, push speed 120 (Scr:117), hinge `maxTorque` | `ProgressionReal.KrakenColumnShot` | all [I] except Lara's 55 kg | Bullet impulse: death record +0x4C x 2.5 is decoded for ragdolls (Obj:1959); for objects the value is not |
| Vehicle suspension | works but differs from original | VERIFIED AGAINST ORIGINAL (data) | Veh.h `springHz` 1.8 x wheel 1.5, damping 0.35, `forceRaise` 0.7 | `VehicleReal.BikeIntrosTuningAndClips` | [O data]; one fixed force point in Jolt [I] | — |
| Vehicle traction | works but differs from original | IMPLEMENTED BUT UNVERIFIED | wheel `friction` 1.5 / 1.7 scaled onto Jolt's slip curves | `VehicleReal.DonutAndPowerSlide` | value [O], meaning [I] | No Wine comparison of grip / slide |

### Physics architecture

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Fixed physics timestep | implemented + works | IMPLEMENTED | `kTickSeconds` 1/60 (Game.hpp:47); `Scene::step(dt)` once per tick (Phy:1235, 1 collision step) | `Game.FrameClockTickAlignedAndHitchCap`, `FrameClock.*` | ours; tru.exe runs a variable step on a 30 Hz base [O] | — |
| Physics / rendering synchronization | implemented + works | IMPLEMENTED | `World::interpolated(alpha)`, `pose(alpha)`, `bikeMatrix(alpha)` | `Game.SimulationIndependentOfRenderRate`, `Physics.DeterministicAcrossRenderRates`, ctest `game_render_rate_smoke` | — | — |
| Appropriate collision layers | implemented + works | IMPLEMENTED | Phy:331 `kLayerJoint, kLayerSolid, kLayerLoose, kLayerVehicle, kLayerPlate, kLayerLara`; 2 broad-phase layers | `Physics.*` | [I] design | No layer for enemies / projectiles (not bodies) |
| Collision filtering | implemented + works | IMPLEMENTED | pair table Phy:562-565; ragdoll `GroupFilterTable` (parent-child off); BVH face masks 0x0F / 0x10 / 0x20 / 0x40 | `Collision.MaskAndIgnoredFaces`, `CollisionRealData.CameraSkipsTheTerrainGroupsTruSkips`, `CollisionRealData.SightSkipsTheTerrainGroupsTruSkips` | camera / sight bits [O] | — |
| Stable contact resolution | implemented + works | IMPLEMENTED | `EMotionQuality::LinearCast` on loose / vehicle / ragdoll bodies; slop 1 unit | `Physics.LooseBodiesLandStackAndTumble` | [I] | — |
| Controlled substeps | partial | IMPLEMENTED BUT UNVERIFIED | `sys.Update(dt, 1, ...)`: one step, none | — | — | No substeps anywhere; whether the bike or stacked weights need them has not been measured |
| Reproducible test scenarios | implemented + works | IMPLEMENTED | `CROSS_PLATFORM_DETERMINISTIC`, single-threaded job system, fixed constraint order (Phy:799 `ordered`), `Scene::save / restore` | `Physics.SavedSceneContinuesBitIdentically`, `StreamReal.SaveContinuesBitIdentically`, 28 replays | — | — |
| Configurable gameplay physics parameters | partial | PARTIALLY IMPLEMENTED | `Tuning`, `BikeTuning`, `HealthTuning`, `GrappleTuning`, ... loaded from the game data | `CombatReal.DecodedCombatNumbers`, `GameReal.GrappleTuningFromData` | — | Many [I] numbers are `constexpr` in anonymous namespaces (G:19-22, Gad:12-18, Obj:565, Scr:115-117, Cmb:180-183); no config file / runtime override |
| Movement, animation and collision representations in sync | implemented + works | IMPLEMENTED | root motion moves the capsule; hands snapped to ledges; bones saved | `GameReal.TransitionsReducePops`, `GameReal.LaraPlaysInteractionClips` | — | Carried portables ignore collision (§9) |

### Grapple physics (goal list; analysis in the section "Grapple physics vs goal §4")

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Investigate Jolt constraints / queries for the rope | missing | NOT IMPLEMENTED | — | — | — | Not investigated; no document |
| Dedicated gameplay-level cable built on Jolt | needs refactor | IMPLEMENTED | G `Player::grapple`, `ropeConstrain` (G:104), `wrapRope` (G:62): no Jolt | `Game.GrappleSwingAndRelease` | [I] | Works, but outside Jolt |
| Current cable length | implemented + works | IMPLEMENTED | `PlayerState::ropeLength`, limits `GrappleTuning::minRope / maxRope` | `Game.GrappleSwingAndRelease` (never stretches), `GameReal.GrappleTuningFromData` | limits [O g_grapple +0x104 / +0x108] | per-unit / per-target overrides not read (Cmb.h [U]) |
| Anchor location | implemented + works | IMPLEMENTED | `grappleAnchor`, `ropePivot` | `GameReal.PoolGrappleTargetAndSwing` | [O intros] | hook attach point on the target [U] |
| Lara's position | works but differs from original | IMPLEMENTED | hand = root + 400 (G `kHandZ`) | same | [I] | — |
| Cable tension | partial | PARTIALLY IMPLEMENTED | `ropeConstrain` removes the outward velocity and projects the position | `Game.GrappleRopeBendsRoundGeometry` | [I] | No tension value: nothing is pulled by Lara's weight, slack / taut is not a state |
| Gravity | implemented + works | IMPLEMENTED | G:320 `vel.z -= gravity * dt` | `Game.GrappleSwingAndRelease` | g [O]; drag 0.15 [I] | — |
| Collision against the environment | works but differs from original | IMPLEMENTED | rope bends (rules [O 0x6C5490]); body `sweepSphere` r 100 (G:327) | `Game.GrappleRopeBendsRoundGeometry` | bend constants [O]; ray instead of sphere cast [I] | — |
| Swing momentum | works but differs from original | IMPLEMENTED BUT UNVERIFIED | stick pump 500 u/s^2 (G:319) | `Game.GrappleSwingAndRelease` | [I] | tru.exe runs "[GrappleRope]" Havok bodies; pump rate never measured |
| Rappelling | works but differs from original | IMPLEMENTED BUT UNVERIFIED | G:173 `rappel` | `GameReal.KrakenRoomRappel` | overhang constants [O]; rates [I] | — |
| Wall contact | implemented + works | IMPLEMENTED | G:159 `rappelOnto`, `kSteep` 0.35 | `GameReal.ValaskjalfWallRun`, `Game.GrappleWallRunFromGround` | [I] | — |
| Cable adjustment | works but differs from original | IMPLEMENTED BUT UNVERIFIED | G:378-385: 300 u/s up (interact) / down (walk) | `GameReal.KrakenRoomRappel` | [I] rate and bindings | — |
| Not an animation / teleport | implemented + works | IMPLEMENTED | dynamic pendulum | above | — | — |

### Motorcycle physics (goal statements)

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Investigate Jolt's vehicle module | implemented + works | IMPLEMENTED | docs/formats/PHYSICS.md "Vehicles", "Havok -> Jolt" | — | — | — |
| Use Jolt vehicle components | implemented + works | IMPLEMENTED | `VehicleConstraint`, `WheeledVehicleController`, `WheelSettingsWV`, ray tester | `Physics.VehicleDrivesBrakesAndSavesBitIdentically` | — | Jolt's own engine / gearbox are bypassed: tru.exe's torque curve is applied as wheel forces (Phy:1129) |
| Dedicated vehicle controller | implemented + works | IMPLEMENTED | Veh:314 `World::driveBike` (states of 0x790E40) | `VehicleReal.TrailstartRideReplay` | [O states] | §11 |
| No general-purpose vehicle engine | implemented + works | IMPLEMENTED | one `VehicleSpec` user (the bike) | — | — | — |

---

## Jolt integration

- Version: Jolt Physics 5.6.0, `FetchContent` URL tarball with pinned SHA-256 (CMakeLists:261-266), `SOURCE_SUBDIR Build`.
- Build flags (CMakeLists:248-260): `CROSS_PLATFORM_DETERMINISTIC ON`; `USE_AVX / AVX2 / AVX512 / LZCNT / TZCNT / F16C /
  FMADD OFF` (x86-64-v2 AppImage); debug renderer, profiler, object stream, install, GPU compute, floating-point
  exceptions OFF; `CPP_RTTI_ENABLED ON`; IPO follows `OU_LTO`. Double precision is not enabled (Jolt default) [I: not
  set in the file]. Linked PRIVATE into `ou_physics`: only `Physics.cpp` includes Jolt.
- System (Phy:479-572): `PhysicsSystem::Init(4096 bodies, 0 mutexes, 8192 pairs, 8192 contacts)`,
  `TempAllocatorMalloc`, `JobSystemSingleThreaded`, units metres = game units / 280, gravity Lara's, penetration slop
  1 unit.
- Step: `Scene::step(1/60)` once per world tick (Obj:1789 `World::stepPhysics`), `sys.Update(dt, 1, ...)`: one
  collision step, no substeps. Order inside a step (Phy:1205): new solids, pending restore, hinge / joint drives,
  vehicles, kinematic moves, Lara's proxy, buoyancy, fixed constraint order, Jolt update, read-back.
- Simulated by Jolt today:
  - class-34 hinge doors and levers (`Scene::addBody / addHinge`, motors to a target angle);
  - uber-object joints (hinge / slider, limits, motors) and link rows between them (custom `LinkConstraint`);
  - pressure plates (slider bodies with the plate mesh, lifted by a motor, pressed by contacts and by ray-placed
    weights for Lara and push blocks);
  - loose rigid bodies: grapple-pulled weights, set-down portables, thrall chunk pieces, with buoyancy and drag in
    water volumes;
  - ragdolls of dead enemies and of Lara (capsule per bone, `SixDOFConstraint`);
  - the ridden motorcycle (`VehicleConstraint`, chassis body, ray wheels);
  - static terrain meshes per resident unit and kinematic copies of the moving object collision, as what those bodies
    land on;
  - Lara's proxy: a dynamic, translation-only capsule velocity-driven to her position (layer `kLayerLara`, collides
    with `kLayerLoose` only), present only while loose bodies or ragdolls exist.
- NOT simulated by Jolt:
  - Lara's controller (kinematic capsule sweep on the own BVH), swimming (sphere sweep), all traversal;
  - the grapple rope (own pendulum + bend list) and the hook;
  - push blocks (kinematic, moved by the script runtime at 120 u/s along one of two axes, blocked by a terrain ray);
  - enemies (own capsule slide, nav mesh), their projectiles, Lara's bullets (BVH rays), spears / darts and grenades
    (own ballistic steps);
  - level queries of any kind (camera, line of sight, ground probes, markup);
  - carried portables (posed at her chest), inserted portables (posed at the marker).
- Layers / filters: object layers `Joint, Solid, Loose, Vehicle, Plate, Lara`; pairs enabled: Solid-Loose,
  Loose-Loose, Solid-Vehicle, Loose-Vehicle, Plate-Loose, Plate-Vehicle, Lara-Loose. Joint bodies collide with nothing
  (their collision is the kinematic proxy in `Solid`). Ragdoll bones: `GroupFilterTable` with parent-child pairs off.
  The own BVH filters by face mask (terrain 0x0F, objects 0x10, camera 0x20, sight 0x40) and terrain-group enable.
- Determinism: fixed tick, single thread, deterministic build flag, constraints re-ordered every step, body IDs
  reserved by key so a load gets the saved IDs, `PhysicsSystem::SaveState(All)` in the save (`objects.physics`).
  Evidence: `Physics.DeterministicAcrossRenderRates`, `Physics.SavedSceneContinuesBitIdentically`,
  `StreamReal.SaveContinuesBitIdentically`, `StreamReal.RagdollSaveLoad`, `VehicleReal.SaveRestoreMidRide`.

## CharacterVirtual evaluation plan

(Executed: measurements and decision in `docs/formats/PHYSICS.md` "Character controller". The plan as written:)

Why it is a fair candidate: tru.exe itself moves Lara as a proxy capsule in Havok (instance +0x288: radius +0x65C,
cylinder +0x660, created by 0x7018E0, resized by 0x490E60; state-chart condition `PSCCF_CheckProxySupport`, string
`physicsRollProxy`), while the decision what to do is the state chart's. `JPH::CharacterVirtual` is the same split:
a kinematic shape that Jolt slides, steps and keeps on the floor, driven by a velocity the gameplay code chooses.

What it would replace (and only this):

| Today (`Player.cpp`) | With CharacterVirtual |
|---|---|
| `slide` (P:348), the wall-slide loop of `groundMove` (P:381) | `CharacterVirtual::ExtendedUpdate` with velocity = clip root delta / dt |
| ground snap ray + `standable` 4 probes (P:372-384) | `GetGroundState()`, `mMaxSlopeAngle`, `ExtendedUpdateSettings::mStickToFloorStepDown` |
| capsule bottom lifted by `stepUp` | `mWalkStairsStepUp` |
| `airMove` contact loop (P:441-505), edge slide-off 200 u/s | `Update` in the air (velocity integrated by us exactly as 0x5DB370), contact callbacks clip the velocity |
| `capHeight()` crouch switch (P:207) | `SetShape` with penetration test |
| world-applied platform `carry` (W tick) | ground velocity of the kinematic collider (only one of the two may stay) |
| Lara's dynamic proxy capsule (Phy:752) | CharacterVirtual's own impulses on dynamic bodies (`mMass` 55, `mMaxStrength`) |

Not replaced: every markup query (ledges, poles, ladders, beams, holds), the jump solver, swimming's free 3-D motion
(evaluate separately, second), the rope, hurdles / climb-ups that play root motion without collision.

Implementation steps (code agent, own worktree):

1. `physics::Scene`: build the terrain and the kinematic object colliders always (today lazily, only when a body
   needs them); mirror what the BVH filters: terrain-group enable (script native, `GameReal.TerrainGroupNativeUpdates
   MergedCollision`) and the face masks, as an `ObjectLayer` per mask class or a `ShapeFilter` on sub-shape user data.
   Streaming must add / remove a unit's body with its offset exactly when `World::rebuildCollision` does.
2. `physics::Scene::Character` API in game units, Jolt hidden: `CharacterSpec {radius, top, stepUp, stepDown,
   maxSlopeCos, mass}`, `moveCharacter(pos, velocity, dt) -> {pos, grounded, groundNormal, groundVelocity, hitWall,
   contacts}`, `setCharacterShape(top)`; state in `Scene::save` (`CharacterVirtual::SaveState`).
3. `Player`: a mover interface with two implementations, the current sweep and the Jolt one, selected by
   `Tuning::mover` (CLI `--mover sweep|jolt` during the evaluation). Both are real gameplay code; the loser is deleted
   after the decision (no permanent switch, no test-only path).
4. Settings to start from: capsule radius 79.01, top 489.2 (shape bottom on the feet), crouch top 260,
   `mMaxSlopeAngle` = acos 0.7 = 45.57 degrees, `mWalkStairsStepUp` = 110 units, `mStickToFloorStepDown` = 110 units,
   `mCharacterPadding` and `mPredictiveContactDistance` small enough that the 100-unit `wallOffset` users do not
   change, back faces as the BVH treats them.

Numbers it must reproduce (the comparison is against these, not against the old code):

- step up / down 110: risers 97..100 of the manor staircase climbed at unchanged run speed, z changing as the root
  crosses each riser line; the 121-unit hearth edge blocks her, tru.exe stopping 38..72 units short [O Wine, GP
  "Numbers" Step up / down];
- walkable slope normal z >= 0.7 [I today: measure the original's limit under Wine on a ramp series first, so the
  evaluation is not tuned to an inferred number]; slide band 0.3..0.7 with acceleration g sin(theta), no cap [O Wine];
- wall slide: horizontal contact normal, no climb up walls, no speed gain;
- capsule radius 79.01, top 489.2, crouch 260 [O]; swim sphere unchanged;
- jump take-offs 1441.2 / 1499.1 + 1200 / 1275.9 + 1974.9 and the 201.6 apex [O Wine]: stick-to-floor must not eat
  the take-off tick;
- no fall / land flicker walking slowly off an edge and on 0.6-normal seam strips (l4_ragnarok_room, replay
  `pool_to_ragnarok.txt` tick 127);
- platforms: elevator, spline mover, lowered ramp carry her without drift.

Existing tests that are the comparison (run with both movers; every one must pass on the Jolt mover without touching
its [O] numbers): `Game.WalkingIntoWallSlides`, `Game.StepsUpAndDownSmallLedgesButNotWalls`,
`GameReal.StairsWalkAndRunUpAndDown`, `Game.RunsOverSteepSliverWithoutFalling`, `Game.WalkingSlowlyOffEdgeFallsOnce`,
`Game.RunsOffEdgeFallsAndLands`, `Game.SlidesDownSteepSlope`, `Game.SlideAcceleratesAtGravityAlongSlope`,
`Game.GravityJumpAndLanding`, `Game.JumpTakeOffsMatchTruTraces`, `GameReal.BedroomJumpsMatchTruTraces`,
`Game.CrouchUnderSlabAndRoll`, `GameReal.PoolRunReplayStaysOnFloor`, `GameReal.PoolRockStepDown`,
`GameReal.SplineMoverCarriesLara`, `GameReal.ElevatorCarriesPlayer`, `ProgressionReal.LoweredRampSectionCarriesLara`,
`Game.SimulationIndependentOfRenderRate`, `GameReal.CliffclimbIndependentOfRenderRate`,
`StreamReal.SaveContinuesBitIdentically`, `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`; on
`int/0.42.0` the self-tests `locomotion`, `ledge`, `traversal_chain`, `fall_damage`, `sprint`.

Measurements to record in the selection document (new `docs/formats/PHYSICS.md` section "Character controller"):
pass / fail per test above for both movers; per-tick position difference between the movers over
`tests/replays/pool_to_ragnarok.txt` and `save_move_load.txt`; stop distance at the hearth and z trace on the stairs
against the Wine traces (`openunderworld-work/screens/stairs`); count of Fall <-> Land state changes per replay;
player tick cost (`World::tickTimes`) in l4_pool and l2_junglehub.

Risks:

- Bit-exact replays: every position changes, so the 28 replays and the self-tests that pin positions must be
  re-recorded (stick input is camera-relative, small drifts move the camera too; this already forced re-recording in
  0.41.0). Plan for the autopilot plans, not the raw scripts, to be the source.
- Saves: `player.controller` gains the character's contact / ground state; version bump and a migration test
  (`Save.MigratesVersion1` pattern). `StreamReal.SaveContinuesBitIdentically` (14 scenarios) is the gate.
- Root-motion drive: CharacterVirtual takes a velocity; a clip that turns and translates in one tick must give the
  same displacement (velocity = delta / dt, no acceleration smoothing, `mEnhancedInternalEdgeRemoval` evaluated for
  seams).
- Double carry on platforms (ground velocity + world `carry`).
- A rounded capsule bottom slides off edges earlier than the lifted-capsule + ray scheme: the edge behaviour that the
  hurdle, drop-to-hang and saving-grab code expects (root may overhang) can change.
- Two collision worlds stay unless camera, AI and markup tests move too: cost and a consistency risk (terrain-group
  toggles must reach both).
- Unit streaming: adding / removing large `MeshShape` bodies on the main thread (the BVH join already costs ~12 ms).

Decision rule: adopt the Jolt mover when all comparison tests pass and the Wine-traced cases (stairs, hearth, slope
slide) are equal or closer than today; otherwise keep the sweep mover and write down why, with the numbers. Either
outcome satisfies the goal's "evaluate through actual gameplay tests and document".

## Grapple physics vs goal §4

Today (G): after the throw clip Lara is a point mass (her root; the rope ends at root + 400 [I]) integrated each tick:
stick acceleration 500 u/s^2 in the air [I], gravity, exponential drag 0.15 /s [I], then `ropeConstrain` projects the
hand back onto the sphere of the rope's free length round the last pivot and removes the outward velocity. The rope is
a polyline anchor -> up to 8 bends -> hand; `wrapRope` adds a bend where the pivot-hand segment first touches geometry
and removes bends that see past their neighbours, with tru.exe's constants (trim 9.48, lift 7.90, 15.8 apart, slide
15..200 [O 0x6C5490 / 0x6B2CC0 / 0x6B3570]); the total length is conserved over bends. Her body is one swept sphere
(r 100) that turns a steep wall contact into the rappel state and a floor contact into a landing. Rope length changes
at 300 u/s between `minRope` 553.1 and `maxRope` 4424.8 [O g_grapple]. Mounted targets: a one-second heave runs the
target's pull handler, or drags a block at 240 u/s [I] (W:590-601). The original runs the swing on "[GrappleRope]"
Havok bodies (decomp string; G:7).

What the goal asks: a gameplay-level cable built on Jolt's constraints / queries, with length, anchor, tension,
gravity, environment collision, momentum, rappel, wall contact and adjustment, and no animation-only or teleport
solution.

Gaps:

1. Not on Jolt: neither a `DistanceConstraint` (max-length mode) between a character body and the anchor nor Jolt
   shape casts for the rope. This follows from Lara not being a Jolt character; it should be decided together with the
   CharacterVirtual evaluation (a CharacterVirtual has no constraints; a rope on it stays a gameplay-level length
   constraint applied to its velocity, which is what `ropeConstrain` already is: then the honest position is "length
   constraint kept, queries moved to Jolt").
2. No tension: the rope cannot pull a dynamic target by Lara's weight or swing, and a mounted pull is a timer, not a
   force on the joint (Obj uber joints have `Joint1::push`; the pull does not use it).
3. Every dynamic rate is [I]: pump, drag, climb / descend speed, wall-run speeds (`kWallRunSpeed` 700, `kWallRunLift`
   350, `kWallRunAccel` 2500), kick-off 700 + 200, landing threshold 300. None has a Wine trace. Truth: trace
   Instance +0x20 / +0x2A0 on l4_pool's ring and l5_valaskjalf's wall run (harness `openunderworld-work/poleattach`),
   and decode the "[GrappleRope]" set-up (bodies, constraint type, stiffness) from the decomp.
4. Rope collision uses a ray per segment where tru.exe casts a 4.74-radius sphere: thin edges can be missed.
5. Jump while standing roped lets go instead of starting the swing (AUDIT_A §6, G:306): a gameplay defect that also
   blocks an honest physics comparison.

---

## §8 Combat

### Targeting

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Automatic target acquisition | implemented + works | IMPLEMENTED | Cmb:899 `World::candidates(false)`: soft target 5000 / 27.5 degrees | `Combat.PistolsLockFireAndHit` | sets [O lara.drm +0x470]; how they are used [I] | — |
| Target locking | implemented + works | IMPLEMENTED | Cmb:1232 (aim held), tiers `candidates(true)` | `Combat.LockOnSelectionAndSwitch` | tiers [O +0x480]; lock drops past 8000 [I] | Lara turns to the lock only standing (Cmb:1256 [I]) |
| Target switching | works but differs from original | IMPLEMENTED BUT UNVERIFIED | Cmb:1238: next in tier order | `Combat.LockOnSelectionAndSwitch` | [I] | The original's switch rule (stick direction?) is not decoded |
| Target prioritization | implemented + works | IMPLEMENTED | three tiers: nearest within 1264, then smallest camera angle | same | [O data] | — |
| Manual and precision aiming | partial | PARTIALLY IMPLEMENTED | free aim = camera ray (Cmb:1249) | `Combat.PistolsLockFireAndHit` | [I] | No manual-aim mode (over-the-shoulder camera, reticle control); `Actions::aim` is only the lock |
| Multi-target behavior | missing | NOT IMPLEMENTED | both guns fire at one `aimPoint` (Cmb:1044 `fire`) | — | — | Dual pistols / uzis cannot split onto two targets; decode from the weapon controller (0x6C9E50 hand byte, targeting slots) whether TRU PC supports it |
| Correct target distance | implemented + works | VERIFIED AGAINST ORIGINAL (ranges) | `TargetingTuning` 1264 / 8000 / 5000 | `CombatReal.DecodedCombatNumbers` | [O] | — |
| Line-of-sight checks | implemented + works | IMPLEMENTED | Cmb:912 ray from eye +375 | `Combat.LockOnSelectionAndSwitch` | eye height [O +0x48C] | Uses the default mask, not the sight mask (`kSightObjectMask`, oflags 0x08) that the AI's sight uses: [U] which the exe uses for Lara's targeting (0x713090) |
| Weapon-specific aiming | partial | PARTIALLY IMPLEMENTED | per-weapon aim grids (Cmb `kPistolCols`, rifle grid, bike / rope grids) | `CombatReal.RifleAndTranquilizerClips`, `CombatReal.MuzzleAtGunMarkers` | clip names [O]; grid angles measured from poses [I] | Targeting range is the same 8000 for every weapon although weapon data S+0x20 is "damage knee, targeting range" (Cmb.h); left-side aims on bike / rope clamp ([U] `switch_hands`) |

### Weapons (list)

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Dual pistols | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::Pistols`, Cmb:349 `weaponDefaults` | `CombatReal.WeaponData`, `Combat.PistolsLockFireAndHit`, `Combat.AmmoClipsAndReload` | [O]; refire from clip length [I] | no under-water use [U] |
| Dual submachine guns | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::Uzi` | `CombatReal.WeaponData`, `Combat.WeaponSwitchKeepsRoundsAndReloadsFromReserve` | [O]; spread recovery unit [I] | — |
| Shotgun | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::Shotgun` | `Combat.ShotgunPelletsInTheCone`, `CombatReal.WeaponData` | [O]; whole clip per reload [I] | — |
| Assault rifle | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::AssaultRifle` | `CombatReal.PoolRifleAndTranquilizer` | [O] | grid angles shared with the shotgun [I] |
| Tranquilizer gun | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::Tranquilizer`, `Hit::tranq`, `ai::Mode::Sleep` | `CombatReal.PoolRifleAndTranquilizer` | dose 500 [O]; sleep 20 s, non-animals take it as damage [I] | — |
| Spear gun | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::Speargun`, `tickProjectiles` | `CombatReal.PoolSpeargunUnderWater` | [O]; wet record = under water [I] | — |
| Sticky grenades | works but differs from original | IMPLEMENTED | Gad:40 `tickGadgets`, `explode` | `Gadgets.GrenadeKillsDummyAndHurtsLara`, `GadgetsReal.TuningLightsAndSpotUnits` | values [O], field meanings [I]; release point, arc solve, flight time, collision radius [I] | Ground throw only (crouch / perch / under-water placing clips unused, Gad:20); explosion barrels' blast objects not born |
| Mjolnir | implemented + works | VERIFIED AGAINST ORIGINAL (data) | `Weapon::ThorHammer`, `HammerAction`, `groundPound` | `Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings`, replay `mjolnir_thralls` | rings [O]; which record the pound uses [I]; aim yaws [I] | no adrenaline with it [I] |

### Weapons ("implement")

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Weapon selection | implemented + works | IMPLEMENTED | PDA EQUIP `World::equipWeapon` (Cmb:755); `chapterSecondary` | `Menus.PdaWeaponsInventoryAndSonar` | PDA list [V capture]; chapter default from unit deps [O], prologue [I] | — |
| Weapon switching | implemented + works | IMPLEMENTED | Cmb:1183 cycle pistols -> secondary -> hammer | `Combat.WeaponSwitchKeepsRoundsAndReloadsFromReserve`, `Combat.ReplayWithSwitchingIndependentOfRenderRate` | [O 0x704930] | — |
| Firing | implemented + works | IMPLEMENTED | Cmb:1044 `fire`, on fire held + cooldown | `Combat.FireIndependentOfRenderRate` | refire = clip lengths [I]; shot on event 0x75 only wired for Mjolnir | Guns fire on a timer, not on the fire clip's event 0x75 as 0x716F30 does |
| Projectile behavior | implemented + works | IMPLEMENTED | Cmb:1072 `tickProjectiles` | `CombatReal.PoolSpeargunUnderWater` | speed / gravity values [O], units [I] | Stuck spear / dart objects not left in the world |
| Weapon-specific damage | implemented + works | VERIFIED AGAINST ORIGINAL | `WeaponDef::damageAt` | `CombatReal.WeaponData`, `CombatReal.DecodedCombatNumbers` | [O] | Enemy-side damage classes: thralls only (`ai::Regenerate::kills`) |
| Ammunition consumption | implemented + works | IMPLEMENTED | Cmb:1328 clip per shot; reserves | `Combat.AmmoClipsAndReload` | [O 0x6C9E50] | — |
| Automatic reloading | implemented + works | IMPLEMENTED | Cmb:1324, 1331; `reloadWeapon` | same | reload lengths [O clips] | — |
| Underwater weapon behavior | partial | PARTIALLY IMPLEMENTED | Cmb:188 `canShoot`: speargun and Mjolnir only | `CombatReal.PoolSpeargunUnderWater`, `FxReal.GunFireBubblesOnlyUnderWater` | [U] which weapons the exe allows (every wp_* has `fire_submerged` clips) | Decide from the exe; rifle already has wet damage numbers [O] |
| Weapon animation | implemented + works | IMPLEMENTED | Cmb:1350 `aimLayer`, `combatLayerClip` | `CombatReal.RifleAndTranquilizerClips`, `GameReal.GrappleShootsOnTheRope` | [O clips] / blend [I] | — |
| Weapon switching transitions | implemented + works | IMPLEMENTED | `drawT` over equip / unequip clips; gun mount on events 0x74 / 0x76-0x78 (Cmb:679) | `Combat.WeaponSwitchKeepsRoundsAndReloadsFromReserve` | [O events] | — |
| Original ammunition / inventory rules | implemented + works | IMPLEMENTED | `chapterLoadout` (start amount per capacity level, the chosen secondary), `maxReserve` by tailoring | `Combat.LevelStartLoadout`, `CombatReal.WeaponData`, `Tailoring.PresetsScaleHealthAndCapacity`, `StreamReal.ChapterThorGear` | start [O +0x848 / +0x85C / +0x852 = the maximum tables], max [O]; clips full [I] | — (GAMEPLAY.md "Loadout") |
| No manual reload | implemented + works | IMPLEMENTED | no reload action in `Actions` | — | — | — |
| No invented ammunition pickups | works but differs from original | IMPLEMENTED BUT UNVERIFIED | Str:563 `collectPickups`: weapon intros [O 0x70E530 -> 0x6F5E70]; dead armed enemies' bodies [I] | `Pickups.InteractCollectsNearestOnceAndSaves` | amount "one clip" [I: property 0x55 not decoded] | The enemy-body rule is ours: check whether tru.exe births a wp_akrifle instance on a poacher's death (then it is the same 0x70E530 path) or drop it |

### Movement during combat

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Combat movement | partial | PARTIALLY IMPLEMENTED | `canShoot` allows idle / walk / run / jump / fall; upper-body layer over locomotion | `Combat.FireIndependentOfRenderRate` | [I] | No lock-relative movement (strafe / back-pedal facing the target): she runs where the stick points and only the torso aims |
| Evasive jumps | missing | NOT IMPLEMENTED | — | — | — | Jumps with a lock are the ordinary jumps; no side / back flips |
| Evasive rolls | partial | PARTIALLY IMPLEMENTED | `State::Roll`: run + drop, forward only (P:1930) | `Game.CrouchUnderSlabAndRoll` | clips [O]; trigger [I] | No directional dodge roll; no invulnerability rule decoded |
| Enemy vaults | missing | NOT IMPLEMENTED | — | — | — | Enemy death entries carry flag 7 "special (vault, thrown)" (ai/Enemy.hpp:67 [O]) with no gameplay that reaches it |
| Recovery after being knocked down | missing | NOT IMPLEMENTED | only `hit_light_*` / `hit_medium*` reactions (Cmb:659) | `Combat.HitReactionPlaysAndReturns` | thresholds [O 0x6EB590]; clip choice [I] | No knock-down / get-up state; reactions only on foot or swimming |
| Melee attacks | missing | NOT IMPLEMENTED | grep `melee|kick` in `src/game` finds only enemies' melee and Mjolnir's pound | — | — | Assigned to peer GpMelee (RULES addenda) |
| Jump kicks | missing | NOT IMPLEMENTED | — | — | — | same |
| Weapon-of-opportunity attacks | missing | NOT IMPLEMENTED | carried portables can only be set down / inserted (Obj:1393) | — | — | No swing or throw of a carried pole / object |
| Combat movement while climbing | partial | PARTIALLY IMPLEMENTED | rope only: `onRope` grids (Cmb:138) | `GameReal.GrappleShootsOnTheRope` | clip names [O]; grid angles [I] | Hang, ladder, pole, pillar, beam, perch: `canShoot` false |
| Single-handed weapon use while climbing | partial | PARTIALLY IMPLEMENTED | same (pistols / uzi, right hand) | same | [I] rule | Check lara / wp_handgun DRMs for `combat_pistol_ledge*` / pole / ladder one-handed clips and wire them |
| Underwater evasive movement | missing | NOT IMPLEMENTED | — | — | — | no swim dodge |

### Adrenaline head shot

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Adrenaline meter requirements | incorrect | PARTIALLY IMPLEMENTED | Cmb:1313: opens when `adrenalineMode` + lock + close; the meter level is not tested | `Combat.AdrenalineHeadShot` (fills the meter first, so it does not catch this) | max 500, drain [O]; gate [I] | Locals 1333-1335 and Cmb.h:317 say "full meter"; code allows any level once the mode is on |
| Valid enemy targeting | partial | PARTIALLY IMPLEMENTED | any locked `Damageable` | same | [I] | No per-enemy validity (bosses, animals, sharks); Mjolnir excluded [I] |
| Enemy approach | works but differs from original | IMPLEMENTED BUT UNVERIFIED | within `closeRange` 1264 of her eye | same | [I: "close" = the close lock tier] | — |
| Grapple-assisted enemy vault interaction | missing | NOT IMPLEMENTED | Cmb:948 comment: only lara / weapon / kraken DRMs name grapple clips, "grappling an enemy is not a thing in TRU" | `Game.GrappleIgnoresEnemies` | [O data claim] | Contradicts the goal text; settle it with the walkthrough controls page + the exe (0x715020 / 0x718EA0 "accurate-aim events 5 / 4") before implementing either way |
| Slow-motion transition | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | `AdrenalineTuning::headShotScale` 0.2, `ease` 10 frames | `Combat.AdrenalineHeadShot`, `CombatReal.DecodedCombatNumbers` | [O +0x2C8 / +0x2CC] | — |
| Precision aiming | works but differs from original | IMPLEMENTED BUT UNVERIFIED | pip steered by the move stick (Cmb:1293) | `Combat.AdrenalineHeadShot` | `kPipStart` 2.5, `kPipSpeed` 2 [I] | — |
| Head targeting | partial | PARTIALLY IMPLEMENTED | hits `aimPoint()` (chest, 0.7 x height) with `Hit::headshot` | same | [I] | No head position / camera move to the head |
| Timing | works but differs from original | IMPLEMENTED BUT UNVERIFIED | `kSlowWindow` 3 s | same | [I] | — |
| Firing | implemented + works | IMPLEMENTED | Cmb:1296: kills, one shot per window | same | [I one shot] | — |
| Successful and failed outcomes | partial | PARTIALLY IMPLEMENTED | success: death entry flag 3 clip (`Ai.DeathClipByQuadrantAndFlags`); failure: window ends, meter spent | same | flag [O]; failure rule [I] | No failed-vault / counter-attack outcome |

### Focused fire (adrenaline mode)

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Adrenaline threshold | implemented + works | IMPLEMENTED | Cmb:1319: meter > 0 + lock + interact toggles | `Combat.AdrenalineModeDrainsAndSlowsTime` | "meter not empty" [O 0x718BD0]; the button [I] | Button is interact; on the bike it cannot open (GP) |
| Target lock | implemented + works | IMPLEMENTED | Cmb:1273: ends without the lock | same | [O flag 0x400] | — |
| Precision aiming | partial | PARTIALLY IMPLEMENTED | ordinary lock aim | — | — | Nothing mode-specific (no spread / accuracy change decoded) |
| Time dilation | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | `modeScale` 0.5 over 10 frames; enemies tick with `dt x timeScale` (W tick) | same | [O +0x31C, 0x7189C0] | Scripts, physics bodies, projectiles and the bike are not slowed: only `enemies_.tick` takes the scale |
| Faster firing behavior | works but differs from original | IMPLEMENTED BUT UNVERIFIED | Lara keeps real time, so she fires twice per world second | same | Lara at real time [O 0x622418] | No refire change of its own; [U] whether the exe adds one |
| Duration | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | drain 225 / s | same | [O +0x2D8] | — |
| Return to ordinary combat | implemented + works | IMPLEMENTED | Cmb:1151-1155 ramp back | same | [O 0x718AD0] | — |
| No boss QTE | implemented + works | IMPLEMENTED | Natla fight runs from the level scripts | `ProgressionReal.EndgameCredits` | [O scripts] | — |

## Weapons table

Damage = at 0 / short range / max range; ranges in units. Reserve = by PLAYER TAILORING ammunition level (lenient /
normal / strict). All numbers are decoded from the weapon and ammo DRMs and Lara's data (`loadWeapon`), pinned by
`CombatReal.WeaponData` and `CombatReal.DecodedCombatNumbers`, except where marked.

| Weapon | Damage | Clip | Ammo rules | Under water | Availability | Number sources | Status |
|---|---|---|---|---|---|---|---|
| Dual pistols (`wp_handgun`, `pr_handgunbullet`) | 40 / 30 / 20; ranges 4000 / 6500; spread 1 deg | 12 per gun | infinite (S+0x88 bit 0x80); the clip still empties and reloads | cannot be drawn (`canShoot`) [U exe rule] | always | [O] S block / ammo record; refire 10 / 30 s = clip length [I] | VERIFIED AGAINST ORIGINAL (data); firing cadence [I] |
| Dual SMGs (`wp_uzi`, `pr_uzibullet`) | 25 / 25 / 10; 2000 / 4500; spread 1 -> 5 deg (+1 per shot) | 40 per gun | slot 0, reserve 400 / 240 / 160 | no [U] | loaded by every unit; default secondary in Thailand, Jan Mayen, epilogue; equippable from the PDA in any chapter | [O]; recovery 0.1 deg per frame unit [I]; refire 3 / 30 s [I] | VERIFIED AGAINST ORIGINAL (data) |
| Shotgun (`wp_shotgun`, `pr_shotgunbullet`) | 7 pellets x 50 / 50 / 40; 2000 / 3000; cone 15 deg | 6 | slot 1, reserve 90 / 72 / 42; reload refills the whole clip [I] | no [U] | default secondary in Croft Manor (ch 3) | [O]; refire fire 13 + pump 25 keys [I] | VERIFIED AGAINST ORIGINAL (data) |
| Assault rifle (`wp_assault_rifle`, `pr_akriflebullet`) | 27 / 27 / 21 (wet record 23 / 23 / 18); 5000 / 8000; spread 1 -> 5 deg | 30 | slot 2, reserve 300 / 210 / 120; `wp_akrifle` pickups fill the same slot | no [U], although a wet damage record exists | default secondary in the Andaman Sea (ch 6) | [O] | VERIFIED AGAINST ORIGINAL (data) |
| Tranquilizer (`wp_tranq`, `pr_tranquilizerdart`) | dose 500 (projectile, speed 1500 / frame, gravity 1 / 3 per frame^2); 4000 / 6000; spread 5 deg | 1 | slot 3, reserve 25 / 15 / 10 | no [U] | default secondary in Mexico (ch 4) | values [O]; units per 30 Hz frame [I]; sleep 20 s [I]; non-animals take the dose as damage [I] | VERIFIED AGAINST ORIGINAL (data); effect rules [I] |
| Spear gun (`wp_speargun`, `pr_spear`) | 250 dry, 550 with the +0x84 record; projectile speed 1000 / frame, gravity 2 / 6; 2500 / 5000 | 1 | slot 4, reserve 40 / 25 / 15 | yes: equip / reload `_submerged` clips | default secondary in the Mediterranean (ch 1) and Arctic (ch 7) | values [O]; "+0x84 record = under water" [I] | VERIFIED AGAINST ORIGINAL (data) |
| Sticky grenades (`g_grenade_thrower`, `pr_grenade`) | 1000 at the centre -> 200 at radius 1100; knock 300 -> 100; fuse 120 frames after sticking | — | carried max 10 / 6 / 4; cooldown 20 frames; `World::grenades` starts at 6 | not thrown while swimming (Gad:20) | every chapter [I: nothing gates it]; no pickups exist in the data [O survey] | values [O], field meanings [I]; throw arc, release 0.45 of the clip, 3 s max flight [I]; how the count refills [U] | IMPLEMENTED (`Gadgets.GrenadeKillsDummyAndHurtsLara`), numbers' meaning unverified |
| Mjolnir (`wp_thorhammer`, `pr_thorbolt`) | bolt 1000 at every range (wet record 1000 / 400 / 100); pound rings 1200 inside 500, 400 -> 100 out to 3000, class 13 | none (-1): never reloads | no ammunition | yes (`swim_submerge_thorhammer_attack_front`) [U exe rule] | from chapter 6 (given by `larabase` 37 at the end of l5_valaskjalf [O script]; later chapters [I]) | [O]; pound record choice [I] | VERIFIED AGAINST ORIGINAL (data); `Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings` |

Open for every gun: the per-chapter start reserve (ours: full; the exe's start tables +0x848 / +0x85C / +0x852 are
decoded and unused), whether a chapter change keeps rounds, and the refire timing (ours: a timer equal to a clip
length; the exe fires on the clip's event 0x75).

---

## §9 Environmental interactions and object physics

### Interactions

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Picking up objects | works but differs from original | IMPLEMENTED | Obj:1393 `carryPortables` (engagement kind 4), rewards Str:563 | `ProgressionReal.ReceptaclesTakeTheirPortables`, replays `pool_pickup`, `puzzleroom_pole_remove` | engagement records [O 0x719520]; portables without records 150 / 100 [I] | Taken at once, no pick-up clip for portables; script vt 20 "touched" callback not sent (Obj:1387) |
| Carrying objects | partial | PARTIALLY IMPLEMENTED | held object posed 60 ahead, 80 up of her root (Obj:1420) | `ProgressionReal.BallcourtPoleLeverOpensDoor`, replay `baldrs_plates_gate` | [I] | No carry clips, no movement restriction, the object ignores collision while carried (can pass through walls); `carryReady` only gates pick-up to idle / swim |
| Putting objects down | works but differs from original | IMPLEMENTED | Obj:1401: becomes a loose Jolt body in front of her | `ProgressionReal.TraproomsSeal`, replay `baldrs_plates_gate` | [I] placement | Placed by teleport to a point in front (no clip, no clearance test) |
| Throwing objects | missing | NOT IMPLEMENTED | — | — | — | matrix M54 (thrown weight) |
| Pushing objects | works but differs from original | IMPLEMENTED | Scr:2445 `Kind::Block`; Obj:564 `pushUberJoints`; Obj:640 `pushUberHandle` | `ScriptRuntime.PlaneCrossSwitchUseAndBlockPush`, `GameReal.PushBlockIndependentOfRenderRate`, `ProgressionReal.CalendarPushHandlesTurnTheRing` | grab clips [O]; speed 120, reach, axis choice [I] | The PushPull proxy body (0x7358E0) and its force are not simulated |
| Pulling objects | works but differs from original | IMPLEMENTED | same (stick back) | same | [I] | — |
| Moving crates | partial | IMPLEMENTED BUT UNVERIFIED | `Kind::Block` by object name (`pu_block`, `rb_pushblock`, `pushable`, Scr:110) | no crate-specific test | [I] name rule | Which crate objects exist and whether all match the name rule is not surveyed |
| Moving puzzle blocks | implemented + works | IMPLEMENTED | same; block top ledge markup (Obj `tickObjectMarkup`) | `ProgressionReal.BallcourtBlockUnderPoleReachesIt`, replay `ballcourt_pole_lever_block` | block ledge [O 0x5D0610, Wine] | Blocks test only terrain (mask 0x0F): they pass through other objects / enemies and do not fall off edges |
| Manipulating movable poles | implemented + works | VERIFIED AGAINST ORIGINAL (attach pose) | Obj:1652 `insertHeld`, `removeInserted`, `tickInserted` | `ProgressionReal.PoleHoleLeverOpensRewardDoor`, `ProgressionReal.BallcourtPoleLeverOpensDoor`, replays `valaskjalf_pole_lever_insert`, `puzzleroom_pole_remove` | pole at the marker [O Wine l5 7190]; slide speed 300 / bottom -600 [I] | No insert clip (none in lara.drm [O]) |
| Activating levers | implemented + works | IMPLEMENTED | Scr switch use, Obj:545 `useSwitchClip`, hinge levers | `GameReal.BallcourtLeverDrivesDoorJoint`, `ScriptRealData.BedroomSwitchOpensHouseDoor` | Lara's clip from the switch DRM [O]; stand point [I] | — |
| Pulling switches | implemented + works | IMPLEMENTED | same | `ProgressionReal.CriticalGates` | [O scripts] | — |
| Operating mechanisms | implemented + works | IMPLEMENTED | uber state machines (Obj `tickUber`, `sendUberCommand`), anim graphs | `ProgressionReal.TempleRevealReopens`, `ProgressionReal.ChapelShadowPillars` | [O] | Transition condition check 0x4D0E10 not run for triggers / inserts (Obj:595, 1627 [I]) |
| Opening doors | implemented + works | IMPLEMENTED | script doors, hinge doors (`kDoorOpen` 80 degrees [I]), uber doors | `ProgressionReal.TombDoorOpensByItsSectionClip`, `ProgressionReal.ChapterSixShipDoorsOpen` | angle / axis [I] | — |
| Pulling doors open with the grapple | works but differs from original | IMPLEMENTED BUT UNVERIFIED | W:590: one-second heave runs the target's pull handler | `GameReal.GrapplePullsUberTargets`, replay `grapple_pull` | [I] timing, no force | No test names a door; the pull is a timer |
| Breaking destructible objects | partial | PARTIALLY IMPLEMENTED | Obj:821 `damageObject` (triggers 17 / 18 / 19) | `ProgressionReal.CombatCrateBreaksOnHit`, `StreamReal.MjolnirHitBreaksUrnAndItStaysBroken`, `ProgressionReal.BreakPipeSwitchesItsModel` | [O records] | Contact triggers 6 / 7 / 8 (kick, roll, fall, bike) not simulated; class-30 breakables (ledge breaks, braced doors, falling floors, breakable pole) not implemented; barrel explosions do no damage; urn rewards never spawn (GP "Breakables") |
| Using environmental objects as weapons | missing | NOT IMPLEMENTED | — | — | — | — |
| Using portable objects to reach higher areas | implemented + works | IMPLEMENTED | block markup ledges; inserted poles' markup (Obj `tickInserted`) | `ProgressionReal.BallcourtBlockUnderPoleReachesIt` | [O] | — |

### "Correctly configure" (Jolt bodies of movable objects)

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Use Jolt rigid bodies for movable objects | partial | PARTIALLY IMPLEMENTED | loose bodies, joints, plates: yes; push blocks, carried / inserted portables: no | `Physics.LooseBodiesLandStackAndTumble` | — | Push blocks should become bodies on a planar / slider constraint, or the reason they stay kinematic must be documented from the exe |
| Object mass | implemented + works | VERIFIED AGAINST ORIGINAL | `massProps` | `Physics.MassPropsBoxMatchesExeFormula` | [O] | — |
| Collision shape | implemented + works | IMPLEMENTED | `looseShape` (boxes, capsules, spheres); for Lara's BVH spheres / capsules become bounding boxes (Phy.h `solidShape`) | `PhysicsRealData.AllObjectsParse` | [O prims] | Two different shapes of one object (Jolt exact, BVH boxed) |
| Friction | works but differs from original | IMPLEMENTED BUT UNVERIFIED | Jolt default; ragdolls 0.6 | — | [I] | Havok friction / restitution unknown (ROADMAP row "Jolt") |
| Physical resistance | works but differs from original | IMPLEMENTED BUT UNVERIFIED | push speed 120 u/s constant | `GameReal.PushBlockIndependentOfRenderRate` | [I] | No mass-dependent resistance |
| Movement constraints | implemented + works | IMPLEMENTED | joint limits from data, `multibody` limits | `ProgressionReal.HallwayRampsJoint`, `Physics.HingeLimitsStopAFreeSwing` | [O] | — |
| Collision response | partial | PARTIALLY IMPLEMENTED | Jolt for bodies; blocks: terrain ray only | — | — | see "Moving puzzle blocks" |
| Interaction range | implemented + works | VERIFIED AGAINST ORIGINAL (engagement records) | Obj:1487 `engages`, `engagesAt` | `ProgressionReal.ReceptaclesTakeTheirPortables`, `ProgressionReal.UberReceptaclesTakeTheirPortables` | [O 0x6F3C60, Wine]; blocks / switches `kReach` [I] | — |
| Lara's animations match the object's position | partial | PARTIALLY IMPLEMENTED | `Player::grip` (block clips), `use:<id>` clips | `GameReal.LaraPlaysInteractionClips` | [O clips], placement [I] | No carry / set-down / insert animation |
| No teleporting / passing through geometry | incorrect | PARTIALLY IMPLEMENTED | — | — | — | Carried object ignores collision; set-down and pick-up snap; blocks ignore non-terrain colliders |

### Context-sensitive interaction

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| What Lara can interact with | implemented + works | IMPLEMENTED | Obj:1530 `World::engaged` (first admitting record per instance) | `ProgressionReal.ReceptaclesTakeTheirPortables` | [O 0x719520] | Chooser "first active sensor" and LOS check 0x6E7300 for kinds other than 5 / 8 not modelled (Obj:1529) |
| Facing the correct direction | implemented + works | IMPLEMENTED | engagement shape 2 cone (Phy.h `Engagement::heading / half`) | same | [O 0x6F3A50] | Switches by script kind use reach only [I] |
| Within range | implemented + works | IMPLEMENTED | sphere / cylinder tests at three body points (`kEngageZ0`, `kEngageStep`) | same | [O 0x715DD0, Wine] | — |
| Currently valid | partial | PARTIALLY IMPLEMENTED | `needs` list, `graphAccepts`, uber current set | `ProgressionReal.TraproomSealTakesTheRoomKeys` | [O] | Uber transition conditions not evaluated; portable's flag narrowing to kind 5 not read |
| Which animation is required | partial | PARTIALLY IMPLEMENTED | `useSwitchClip`, pickup clips `gear_weapon_pickup(_crouch)` | `GameReal.LaraPlaysInteractionClips` | [O requests 49 / 98] | No clip selection for insert / carry / kick |
| Whether an object can be moved | partial | PARTIALLY IMPLEMENTED | block terrain rays; joint limits; `uberTrigger` | `ScriptRuntime.PlaneCrossSwitchUseAndBlockPush`, replay `ballcourt_door_blocked` | [I] | — |
| Whether the interaction is complete | implemented + works | IMPLEMENTED | `State::Use` locks input until the clip ends; use handler when the switch clip ends | `GameReal.BallcourtLeverIgnoresAUse` | [O] | — |
| Every interaction type found from the walkthroughs | partial | PARTIALLY IMPLEMENTED | — | — | — | Being produced this wave by peers (`matrix/A_mechanics.md`, `B_mechanics.md`) |

---

## §10 Puzzles and level-specific mechanisms

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Levers | implemented + works | IMPLEMENTED | script switches + class-34 hinges + anim graphs | `GameReal.BallcourtLeverDrivesDoorJoint`, `ProgressionReal.CriticalGates` | [O] | l5 lever 7189 stimuli 0x20 / 0x40 partly [U] (PR) |
| Switches | implemented + works | IMPLEMENTED | Scr `Kind::Switch`, anim-graph use stimulus | `ScriptRealData.BedroomSwitchOpensHouseDoor`, `ProgressionReal.CriticalGates` | [O] | — |
| Movable blocks | works but differs from original | IMPLEMENTED | Scr:2445 | `GameReal.PushBlockIndependentOfRenderRate`, `ProgressionReal.BallcourtBlockUnderPoleReachesIt` | [I] speed / rules | §9 |
| Mechanical platforms | implemented + works | IMPLEMENTED | uber movers, spline movers, elevators | `GameReal.ElevatorUberCommandCreatesMovingCollision`, `GameReal.SplineMoverFollowsItsPath`, `GameReal.ElevatorCarriesPlayer` | [O] | — |
| Rotating mechanisms | implemented + works | IMPLEMENTED | hinge uber joints + links (gears, calendar ring) | `ProgressionReal.CalendarPushHandlesTurnTheRing`, `ProgressionReal.KrakenColumnShot`, `Physics.UberJointFallsLiftsAndLinks` | [O] | — |
| Pressure-based interactions | implemented + works | IMPLEMENTED | Jolt plates (Obj:1815..), weights | `GameReal.PressurePlateIndependentOfRenderRate`, `ProgressionReal.TraproomsSeal`, replay `baldrs_plates_gate` | ranges [O bytecode]; motor / hold numbers [I] | — |
| Grapple-operated mechanisms | works but differs from original | IMPLEMENTED | mounted pull, swivelling handles, grapple lever joint | `GameReal.GrapplePullsUberTargets`, `ProgressionReal.GrappleHandlesSwivel`, `GameReal.GrappleLeverJointIndependentOfRenderRate` | [O targets]; pull timing [I] | pull is a timer, no force |
| Movable poles | implemented + works | IMPLEMENTED | §9 | `ProgressionReal.PoleHoleLeverOpensRewardDoor` | [O] | — |
| Large environmental objects | partial | PARTIALLY IMPLEMENTED | Thor towers pushed on uber joints (Obj:564); kraken sections loop | `ProgressionReal.KrakenRoomSectionsLoopTheirIdles`, `ProgressionReal.EndgameCredits` | [O] | l1 eye puzzle pieces 231-233 "not covered: pieces are physics objects [U interaction]" (PR Gates) |
| Doors and gates | implemented + works | IMPLEMENTED | §9 | `ProgressionReal.CriticalGates`, `ProgressionReal.TombDoorOpensByItsSectionClip` | [O] | — |
| Mechanism activation | implemented + works | IMPLEMENTED | script VM (`src/script`), messages, uber commands | `ScriptRealData.*`, `ScriptVm.*`, `ProgressionReal.CriticalPathPerChapter` | [O bytecode] | Triggers 14 / 15 partly timed [I] (PR "Remaining blockers") |
| Environmental hazards | partial | PARTIALLY IMPLEMENTED | crushers: `larabase` 6 from a geometric depth (Obj:1017 [I]); script `health` natives; drowning; fall damage | `ScriptRuntime.HealthNativeReturnsCurrentValue`, `Combat.FallDamageThresholds` | [I] contact force stand-in | No inventory of hazards (blades, darts, fire, eitr / poison pools, spikes) with a test each; no generic kill / damage volume test |
| Multi-step puzzle sequences | partial | IMPLEMENTED | script state carried across gates | `ProgressionReal.CriticalPathInOrder` (opt-in, `OU_PROGRESSION_ROUTE=1`), `CriticalPathPerChapter` | — | The harness teleports between gates (`World::enterUnit` "standing in for the walk"): no puzzle is played end to end by input except the 7 receptacle replays |
| Reusable systems, no hardcoded walkthrough | implemented + works | IMPLEMENTED | every mechanism runs from the level's own scripts / object data | — | — | — |
| Level-specific tests per mechanism | partial | PARTIALLY IMPLEMENTED | `ProgressionReal.GateSweep`: 136 of 251 candidate gates pass (FAIL 11, state 11, condition 53, encounter 9, preload 31; PR, sweep 2026-11-07) | — | — | The sweep is a survey (opt-in), not a pass / fail suite |

---

## §11 Motorcycle

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Acceleration | implemented + works | VERIFIED AGAINST ORIGINAL (data) | Veh:314 `driveBike`; engine curve Phy:1129 | `VehicleReal.TrailstartRideReplay`, `Physics.VehicleDrivesBrakesAndSavesBitIdentically` | torque curve, ratio, top speed 40 mph [O 0x791D10 / 0x78F860] | 0-to-top-speed time never compared under Wine |
| Reverse | implemented + works | IMPLEMENTED BUT UNVERIFIED | `VehicleDrive::reverse`, `reverseRatio / reverseTorque` | none asserts reversing | [O +0x6C / +0x74 / +0xBC] | add a test |
| Braking | works but differs from original | IMPLEMENTED | brake ramp 1.25 s, brake torques | `Physics.VehicleDrivesBrakesAndSavesBitIdentically`, `VehicleReal.TrailstartRideReplay` | ramp [O]; friction units -> N m [I] | stopping distance unverified |
| Steering | implemented + works | VERIFIED AGAINST ORIGINAL (data) | limits 30 -> 10 degrees, rates 95 -> 75 deg/s | `VehicleReal.LeanFollowsSteeringAtSpeed` | [O 0x793680] | — |
| Turning radius | works but differs from original | IMPLEMENTED BUT UNVERIFIED | follows from steer limit + Jolt tyres | — | [I] | not measured in either game |
| Traction | works but differs from original | IMPLEMENTED BUT UNVERIFIED | friction 1.5 / 1.7 on Jolt's curves | — | [I] | — |
| Suspension | works but differs from original | VERIFIED AGAINST ORIGINAL (data) | spring 2.7 Hz, damping 0.35 | `VehicleReal.BikeIntrosTuningAndClips` | [O]; single force point [I] | — |
| Wheel collision | implemented + works | IMPLEMENTED | ray per wheel (as tru.exe's down ray [O 0x792900]) | `VehicleReal.TrailstartRideReplay` | [O] | — |
| Wheel rotation | implemented + works | IMPLEMENTED | `bikePose` bones 5 / 8, `rearWheel` in a burnout | `VehicleReal.BikeIntrosTuningAndClips` | bones [O]; axes [I] | — |
| Ground contact | implemented + works | IMPLEMENTED | `VehicleState::contacts`, `Bike::air` | `VehicleReal.AirControlTurnsMidJump` | — | — |
| Uneven terrain | implemented + works | IMPLEMENTED BUT UNVERIFIED | simulation on unit terrain | `VehicleReal.TrailstartRideReplay`, `VehicleReal.RideIndependentOfRenderRate` | Jolt contact model [I] | steep-ground torques +0x15C / +0x160 not done (GP) |
| Slopes | partial | IMPLEMENTED BUT UNVERIFIED | same; tilt limit 42.5 degrees [O] | same | — | same |
| Ramps | implemented + works | IMPLEMENTED BUT UNVERIFIED | simulation | no ramp-specific test | — | add a ramp / gap scenario |
| Jumping | works but differs from original | IMPLEMENTED BUT UNVERIFIED | hop +1100 u/s (Veh:390) | `VehicleReal.AirControlTurnsMidJump` | [I]: "no hop in the record or the states" (Veh.h) | Whether the original bike can hop at all is [U]; jumps over gaps come from ramps |
| Landing | partial | PARTIALLY IMPLEMENTED | `landT`, bike fall damage 0x793680 | `VehicleReal.TrailstartRideReplay` | damage [O] | crash braces, `_fall_to_death`, landing sounds not done |
| Collision | partial | IMPLEMENTED BUT UNVERIFIED | chassis body vs world; `ramEnemies` | `VehicleReal.RamsEnemyOnTheTrail` | ram 800 / 0.5 / 30 % [I] | no wall-crash test; hit reactions while riding missing |
| Power slides | works but differs from original | IMPLEMENTED | `BikeStunt::PowerSlide` | `VehicleReal.DonutAndPowerSlide` | pushes / timings [O]; signs, 2.5 rad/s spin cap [I] | — |
| Fast turns | works but differs from original | IMPLEMENTED BUT UNVERIFIED | lean servo, hand brake | `VehicleReal.LeanFollowsSteeringAtSpeed` | lean [O], rate unit [I] | — |
| Donuts | works but differs from original | IMPLEMENTED | `BikeStunt::Donut` (front contact pinned) | `VehicleReal.DonutAndPowerSlide` | [O numbers]; the exe's "Donut Anchor Body" replaced by a pin [I] | — |
| Fast starts | partial | PARTIALLY IMPLEMENTED | `BikeStunt::Burnout` (rear wheel spin-up) | — | [O +0x52 / +0x34 / +0x144] | The burnout does not launch faster; the launch wheelie is off in the data [O]; no test |
| Speed bursts | missing | NOT IMPLEMENTED | — | — | [U] | No boost field found in the 0x20C record so far; check the controls page and the remaining record fields |
| Turbo behavior | missing | NOT IMPLEMENTED | — | — | [U] | same |
| Motorcycle headlights | missing | NOT IMPLEMENTED | grep `headlight` in `src` = 0 hits | — | — | Look for the light in motorcycle.drm's effect list (as `g_personal_light`'s, GP "Flashlight") |
| Motorcycle-mounted movement interactions | partial | PARTIALLY IMPLEMENTED | script mounts (`larabase` 52 / 53, `mountBike`) | `VehicleReal.TrailstartRideReplay` | [O scripts] | PDA on the bike, jump dismounts (`_jump_dismount(_leapover)`), dismount clearance, revisit spawner not done (GP) |
| Lara mounting and dismounting | works but differs from original | IMPLEMENTED | `BikePhase::Mount / Dismount`, clips | `VehicleReal.TrailstartRideReplay`, `VehicleReal.SaveRestoreMidRide` | clips [O]; reach 250, speed 200 [I] | — |
| Motorcycle combat and shooting | works but differs from original | IMPLEMENTED | Cmb bike grid, one gun | `VehicleReal.ShootsFromTheBike` | clips [O]; rules [I] | left-side aim clamps; only pistols / uzi [O data] |
| Works across terrain types / surfaces | partial | IMPLEMENTED BUT UNVERIFIED | one friction for every surface | — | — | No per-surface grip (snow in Jan Mayen vs jungle); [U] whether the original has one |
| Brake + accelerate combinations | implemented + works | IMPLEMENTED | states of 0x790E40 (Veh:330-388) | `VehicleReal.DonutAndPowerSlide` | [O] | — |
| Compared against the original | missing | NOT IMPLEMENTED | — | — | — | No Wine trace of the bike exists (acceleration, steering, suspension, jump); all [O] is data + decomp |
| No player-controlled speedboat | implemented + works | IMPLEMENTED | none exists | — | — | — |

---

## §14 Gameplay systems and progression

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Health | implemented + works | VERIFIED AGAINST ORIGINAL | Cmb.h `Health`, `HealthTuning` | `Combat.InvulnerabilityWindowAndPriority`, `Combat.FallDamageThresholds`, `CombatReal.DecodedCombatNumbers`, `Tailoring.PresetsScaleHealthAndCapacity` | [O 0x6F5030, data] | — |
| Gradual health recovery | implemented + works | VERIFIED AGAINST ORIGINAL | `Health::tick` | `Combat.RegeneratesToQuarterOnly` | [O 0x6F5160] | — |
| Health packs | partial | PARTIALLY IMPLEMENTED | Cmb:1147 (640 heal, 2 s cooldown); count = `found[2]` | `Combat.HealthPacks` | heal / cooldown [O] | Max carried 100 not enforced; enemy health-pack drops (difficulty +0x10) absent; GP "Pickups" still says "not yet usable" (docs behind code) |
| Adrenaline meter | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | `gainAdrenaline`, drain | `Combat.AdrenalineModeDrainsAndSlowsTime`, `CombatReal.DecodedCombatNumbers` | [O] | §8 |
| Underwater breath meter | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | W:1164 `tickAir` | `Game.WaterAirDrainsRefillsAndDrowns` | [O 0x755230]; "head under" test [I] | breath sounds not played |
| Death and respawning | implemented + works | IMPLEMENTED | Combat.cpp death, Stream.cpp `respawn` | `Combat.FallDamageDeathAndRespawn`, `Combat.RespawnKeepsTheLoadout`, `Stream.DeathRestoresTheCheckpointSave`, `StreamReal.DeathRestoresTheCheckpointSave`; self-tests `death_respawn`, `checkpoint_respawn` | fade 90 frames [O 0x75AA50 / 0x75AF90]; restore [O 0x557130]; death clip choice [I]; the 210-frame type-1 wipe unused [U which deaths] | — |
| Checkpoints | implemented + works | IMPLEMENTED | Stream.cpp `touchCheckpoints`: script event (every activation saves) or within 250 / 400 of the intro; a reward pickup saves again | `Stream.CheckpointAndRespawn`, `Stream.DeathRestoresTheCheckpointSave`, `ScriptRuntime.CheckpointNativeEmitsCheckpoint` | activation [O message 0x196, 0x5570F0, 0x5DB6A0]; the touch radius [I] | — |
| Saving and loading | implemented + works | IMPLEMENTED | `src/save`, `World::serialize*` | `Save.*`, `StreamReal.SaveContinuesBitIdentically`, `StreamReal.UberStateSaveLoad`, `VehicleReal.SaveRestoreMidRide` | own format; airborne save resumes at the checkpoint as tru.exe [O] | Original `.sav` import not done (not asked) |
| Weapon inventory | implemented + works | IMPLEMENTED | `CombatState::secondary`, hammer slot | `Menus.PdaWeaponsInventoryAndSonar`, `Combat.MjolnirCycleSwingAndGroundPound` | [O data +0xDC, 0x704930] | — |
| Ammunition | implemented + works | IMPLEMENTED | reserves, `maxReserve`, `startReserve` | `Combat.AmmoClipsAndReload`, `Combat.LevelStartLoadout`, `Tailoring.PresetsScaleHealthAndCapacity` | max and start [O] | — |
| PDA | implemented + works | IMPLEMENTED | `src/ui/Ui.*` `Screen::Pda*` | `Menus.PauseSlotsAndPda`, `MenusReal.RawImagesAndPdaArt` | layout [V captures] | — |
| Sonar map | works but differs from original | IMPLEMENTED | `src/ui/Pda.*`, `recordReveal` (Str:554) | `PdaReal.SonarMapFrameRevealAndModels` | reveal every 1000 units [I]; the exe marks PDAMap reveal volumes (0x6E33D0), not decoded | — |
| Area information | incorrect | PARTIALLY IMPLEMENTED | `chapterRewards`, `fillPda` (main.cpp:2471) | `ChaptersReal.RewardTableMatchesUnits`, `Menus.PauseSlotsAndPda` | table = loose reward intros [O] | Treasure totals are 10 / 22 / 7 / 10 / 10 / 0 / 19 instead of the original's per-area totals (jar treasures missing) |
| Gear and inventory | implemented + works | IMPLEMENTED | PDA inventory pages, HUD gear d-pad, Thor gear draw groups | `Menus.PdaWeaponsInventoryAndSonar`, `ScriptRealData.ThorGearDrawGroups`, `StreamReal.ChapterThorGear` | [O] | — |
| Journal | works but differs from original | IMPLEMENTED | `ui::journalEntries(chapter)` | `Menus.PdaJournal` | entries by chapter [I]; the exe unlocks them by script | bind the unlock native |
| Field assistance | implemented + works | IMPLEMENTED BUT UNVERIFIED | `Action::Hint / Answer`, help conditions from scripts | `ScriptRealData.SeafloorVolumeSetsHelpCondition`, `Menus.PauseSlotsAndPda` | strings [O] | No test checks the hint shown per area against the original |
| Field camera | works but differs from original | IMPLEMENTED | W:562-581 | `Game.FieldCameraTogglesEyeView` | LookAroundState exists [O]; zoom rate, limits [I] | — |
| Utility light | implemented + works | VERIFIED AGAINST ORIGINAL (data) | Gad:43 | `Gadgets.FlashlightToggleCooldownAndPose`, `GadgetsReal.TuningLightsAndSpotUnits` | cooldown, no drain [O] | origin offset [I] |
| Collectibles | partial | PARTIALLY IMPLEMENTED | Str:563 `collectPickups` | `Pickups.InteractCollectsNearestOnceAndSaves`, `ChaptersReal.PickupsAndMusic`, `FxReal.RewardPickupFadesOut` | names [O]; pickup cylinder 200 / 250 [I] | see "Collectibles" |
| Treasures | partial | PARTIALLY IMPLEMENTED | `PickupKind::Treasure` (`*_rw_artifact`) | `ChaptersReal.RewardTableMatchesUnits` | [O] | 78 of 179 |
| Relics | partial | PARTIALLY IMPLEMENTED | `PickupKind::Relic` (`*_rw_treasure`) | same | [O names], role [I] | 6 of 6 exist as pickups; no effect |
| Level transitions | implemented + works | IMPLEMENTED | Str:611 `checkLevelEnd`, `startChapter` | `ChaptersReal.StartUnitsAndLevelEnds`, `GameReal.ScriptedChapterEndChainsStartNextChapter`, `ChaptersReal.*Ending*` | [O scripts] | — |
| Campaign progression | implemented + works | IMPLEMENTED | chapters 0-8, profile, Treasure Hunt | `ProgressionReal.CriticalPathInOrder` (opt-in), `ProgressionReal.EndgameCredits`, `Progress.ProfileAndTreasureHuntSave` | [O] | Not played by input (teleports between gates) |
| Health / adrenaline respond to events | implemented + works | IMPLEMENTED | `damageLara`, `gainAdrenaline` | `Combat.HitReactionPlaysAndReturns`, `Combat.AdrenalineHeadShot` | [O] | — |
| Oxygen depletes under water | implemented + works | VERIFIED AGAINST ORIGINAL (numbers) | `tickAir` | `Game.WaterAirDrainsRefillsAndDrowns` | [O] | — |
| Recovery and damage behaviour match | implemented + works | VERIFIED AGAINST ORIGINAL | `Health` | above | [O] | enemy damage multiplier fixed at entry 1 [O] |
| Relics affect maximum health | missing | NOT IMPLEMENTED | `Health::heal(amount, overheal)` exists; `collectPickups` never calls it | — | [O] overheal up to 1.4 x max decaying over 900 frames only "from the reward path 0x5DB6A0" (GP "Combat") | Decode 0x5DB6A0 and the reward base script 3674 / 3924 (what a relic and a treasure give), then apply it |
| Level-specific inventory / ammunition rules | partial | PARTIALLY IMPLEMENTED | `chapterSecondary`, `chapterGear` | `StreamReal.ChapterThorGear`, `StreamReal.ChapterStartIntros` | deps [O], defaults [I] | — |
| Checkpoints restore the correct state | implemented + works | IMPLEMENTED | respawn = the checkpoint save restored (`World::serializeCheckpoint` / `restoreCheckpointSave`): clock, scripts, enemies, vehicles, weapons and rounds, objects and physics, water, pickups; saved in save games (version 3) | `Stream.DeathRestoresTheCheckpointSave`, `StreamReal.DeathRestoresTheCheckpointSave`, `Save.MigratesVersion2` | [O 0x5570F0 / 0x557130 / 0x556E90]; PDA map reveal kept [I] | — |
| Death returns to the saved checkpoint | implemented + works | IMPLEMENTED | Str:368 | `Stream.CheckpointAndRespawn` | [O] | — |
| 179 treasures / 6 relics documented and obtainable | incorrect | PARTIALLY IMPLEMENTED | — | — | — | see "Collectibles" |
| No Wii / PS2 mechanics | implemented + works | IMPLEMENTED | PC data only | — | — | — |

## Collectibles

Detection: `game::pickupKind` (Progress.cpp:28) classifies an intro by object name: `*_rw_artifact` = treasure,
`*_rw_treasure` = relic, `*_rw_shard` = health pack; `UnitData::pickups` holds them per unit. Interact within 200
(horizontal) / 250 (vertical) takes the nearest (Str:563); `World::collected` (ids) and `World::found[3]` (counts) are
saved. Per-chapter ids are a hand-kept table, `game::chapterRewards` (Progress.cpp:44), checked against the units by
`ChaptersReal.RewardTableMatchesUnits`.

Counts our survey yields (the table): treasures 10 (Mediterranean) + 22 (Thailand) + 7 (Croft Manor) + 10 (Mexico) +
10 (Jan Mayen) + 0 (Andaman Sea) + 19 (Arctic) = **78**; relics 1 in each of chapters 1, 2, 3, 4, 5, 7 = **6**.
Original: **179 treasures, 6 relics**. Relics agree; **101 treasures are missing**.

Where the missing ones are: the object list has `tp_rw_artifact_spawned` and per-theme `*_pt_weightbreak_reward`
objects (`ar_`, `sa_`, `ma_`, `ca_`), and the level data places 249 breakable urns `*_pt_weightbreak(_b)` (l1 32, l2
48, l4 41, l5 78, l7 50) plus l3's `ma_ei_breakable_b..v` (GP "Breakables" [O]). A Wine instance dump shows a
`sa_pt_weightbreak_reward` instance with a runtime id (0x4000010E), i.e. born at run time, not an intro
(`openunderworld-work/grapple/inst-norsehub.txt`). GP says "the urn rewards are separate intros, untouched" [U]; the
dump suggests they are spawned. The walkthrough matrix agrees on the size of the gap (Mediterranean 26 + 1 on the
pages against our 10 + 1: `matrix/A_mechanics.md` l.129). No total including urn rewards is recorded anywhere in the
repo or the work directory.

How to compute it: (1) decode what an urn gives: the urn's intro data / its script fields (`OU_OBJ_FIELDS=<unit>:<uid>`,
`ProgressionReal.ObjectFields`) and the `*_weightbreak_reward` script (`OU_PROGRESSION_DUMP`), looking for the reward
object id (artifact_spawned / shard / nothing); (2) survey every unit: count urns per reward kind; (3) the sum of loose
`*_rw_artifact` intros and treasure-giving urns must be 179 and health-giving urns must match the walkthrough's health
counts; (4) extend `chapterRewards` (or build it from the survey at load) and assert the totals in
`ChaptersReal.RewardTableMatchesUnits`. A cross-check without decoding: break one known treasure urn under Wine and
dump the instance list before / after.

Health packs: counted in `found[2]`, which is also the carried amount (a use takes one). The exe's carried maximum
(100, +0xE2..E6) and +1 per pickup (+0xE8) are decoded and not enforced.

Effects: a treasure or relic pickup only counts. The reward path's overheal (0x5DB6A0) is not applied.

---

## §17 Test infrastructure

What exists on main (4437cb5):

- 25 gtest files, 658 `TEST` cases (ctest 675 with the app-level entries). Synthetic-world suites (`Game.*`,
  `Combat.*`, `Physics.*`, `Ai.*`, `Save.*`, `Stream.*`, `Script*.*`) and real-data suites gated on `OU_GAME_DIR`
  (`GameReal.*` 100, `ProgressionReal.*` 34, `AiReal.*`, `CombatReal.*`, `StreamReal.*`, `VehicleReal.*`,
  `PhysicsRealData.*`). They construct `game::World` directly and assert its state (positions, clips, states, joints).
- Replays: 28 files in `tests/replays/` (input scripts and two recorded device sessions). Seven are ctest entries
  `game_replay_<name>` run through the real `ou-game` with `--report-objects` patterns (CMakeLists:452-470); others
  are read by gtests (`bike_ride`, `pool_to_ragnarok`, `save_move_load`, ...).
- `ou-game --autopilot PLAN --record-script OUT` (`src/app/game/Autopilot.*`): steers by coordinates / uids, records
  an input script.
- `--report-objects`: per-uid object / script-call report at exit.
- ctest `game_render_rate_smoke` (same replay at two render rates must end bit-identical), `game_input_replay_smoke`
  (keyboard and gamepad device replays end where the scripted actions do).
- Surveys (opt-in by env): `ProgressionReal.GateSweep / Survey / DumpScripts`, `AiReal.Survey`, `GameReal.ScriptSurvey`.
- CI: `.github/workflows/ci.yml`, `appimage.yml` (not read in detail; real-data tests need the game, so CI can only
  run the synthetic ones [I]).

`--self-test <file.json>`: absent on main. Present on `int/0.42.0` (4706283, `src/app/game/SelfTest.*`,
`docs/SELFTEST.md`): scenario = steps of input-script lines and / or autopilot plan lines, `expect` conditions over
World probes and per-step measurements, JSON report, exit 0 / 1 / 2; 17 scenarios in `tests/selftests/`
(`balance_beam, combat, death_respawn, fall_damage, grapple_swing, ladder, ledge, locomotion, motorcycle, pole,
pole_drop, puzzle_pole_block_door, rappel, sprint, swim, traversal_chain, wall_run`) and one known gap
(`known_gaps/pole_drop_after_fling`).

Goal §17 core-mechanic list against existing automated coverage (gtest on main; "ST" = a self-test on int/0.42.0):

| §17 item | Coverage | Tests |
|---|---|---|
| Idle and basic movement, running | yes | `GameReal.LaraClipsAndRootMotion`, `Game.WalkingIntoWallSlides`; ST `locomotion` |
| Sprinting and fatigue | yes | `Game.SprintStaminaMatchesTru`; ST `sprint` |
| Acceleration and deceleration | swim only | `Game.SwimSpeedMatchesTru`; on foot nothing measures start / stop distances |
| Jumping, running jumps, landing, falling | yes | `Game.JumpTakeOffsMatchTruTraces`, `GameReal.RunningJumpReachAndRush`, `Game.GravityJumpAndLanding`, `Game.RunsOffEdgeFallsAndLands`; ST `fall_damage` |
| Sprinting jumps | **none** | no test jumps out of a sprint |
| Automatic ledge grabbing, shimmying, corners, pulling up | yes | `Game.LedgeGrabHangShimmyDrop`, `GameReal.HangShimmyRoundCorners`; ST `ledge`, `traversal_chain` |
| Saving grabs | free climb only | `GameReal.ClimbBadAttach` (`savingGrab`); ledge saving grab + recover press: **none** |
| Ladder traversal | yes | `GameReal.LadderRungsClimbOffTop`, `GameReal.LadderSideJumpsReachAndBack`; ST `ladder` |
| Pole climbing, pole swinging | yes | `GameReal.PillarAttachRoundUpSlideOff`, `Game.PoleGrabSwingFlingDrop`, `GameReal.TrapRoomsPole*`; ST `pole` |
| Balance beams | yes | `Game.BalanceBeam*`, `Game.Beam*`; ST `balance_beam` |
| Chimney jumps | **none** | mechanic absent on main (peer GpChimney) |
| Grapple attachment, swinging, rappelling, wall running | yes | `GameReal.PoolGrappleTargetAndSwing`, `Game.GrappleSwingAndRelease`, `GameReal.KrakenRoomRappel`, `GameReal.ValaskjalfWallRun`; ST `grapple_swing`, `rappel`, `wall_run` |
| Cable length adjustment | weak | rope ascend / descend happens inside `KrakenRoomRappel`; no test asserts the length change and its limits |
| Grapple climbing | weak | same |
| Jumping from grapple | partly | `Game.GrappleSwingAndRelease` releases with momentum; no landing-on-destination test |
| Swimming, diving, surfacing, underwater movement | yes | `Game.SwimEnterCrossClimbOut`, `Game.DiveSwimDownAndSurface`, `GameReal.PoolSwimReplay`; ST `swim` |
| Underwater evasive rolls | **none** | mechanic absent |
| Object manipulation, environmental interactions, puzzles | yes | `ProgressionReal.*`, `game_replay_*`; ST `puzzle_pole_block_door` |
| Combat targeting, weapon firing | yes | `Combat.LockOnSelectionAndSwitch`, `Combat.PistolsLockFireAndHit`, `CombatReal.*`; ST `combat` |
| Melee attacks | **none** | mechanic absent |
| Adrenaline headshots | synthetic only | `Combat.AdrenalineHeadShot` (dummy target; no real enemy, no real level) |
| Focused fire | synthetic only | `Combat.AdrenalineModeDrainsAndSlowsTime` |
| Motorcycle acceleration, steering, braking | yes | `VehicleReal.TrailstartRideReplay`, `Physics.VehicleDrivesBrakesAndSavesBitIdentically`; ST `motorcycle` |
| Power sliding | yes | `VehicleReal.DonutAndPowerSlide` |
| Motorcycle jumping | weak | `VehicleReal.AirControlTurnsMidJump` (a hop); no ramp / gap jump with landing |
| Vehicle collision | enemy only | `VehicleReal.RamsEnemyOnTheTrail`; wall / obstacle crash: **none** |
| Checkpoint recovery | yes, shallow | `Stream.CheckpointAndRespawn`, `Combat.FallDamageDeathAndRespawn`; ST `death_respawn`. None checks weapons / ammo / object state after the respawn |
| Animation state transitions | partly | `GameReal.TransitionsReducePops`, ST `visits` / `clips` expectations |

No existing automated coverage at all: sprinting jumps, chimney jumps, underwater evasive rolls, melee attacks,
ledge saving grab + recovery, bike reverse, bike ramp / gap jump with landing, bike wall collision, thrown objects,
breakables broken by contact, urn rewards, relic effect, respawn loadout, manual aim, evasive jumps / dodges, enemy
vault, shooting while hanging / on a ladder / pole.

Goal §17 walkthrough-based tests: none exists as "a walkthrough sequence played by input from a chapter start". The
nearest are the 7 `game_replay_*` puzzle replays (spawn next to the puzzle), `ProgressionReal.CriticalPathInOrder`
(teleports between gates, opt-in), and on int/0.42.0 `traversal_chain` and `puzzle_pole_block_door`. The goal's four
example tests map as: traversal = ST `traversal_chain` (int/0.42.0 only); grapple swing with landing on a destination
platform = none; puzzle push-block + mechanism = ST `puzzle_pole_block_door` / replay `ballcourt_pole_lever_block`;
motorcycle with a jump over a gap and continued riding = none.

---

## §19 Development quality and performance

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Latest supported C++ standard | implemented + works | IMPLEMENTED | CMakeLists:4 `CMAKE_CXX_STANDARD 23`, extensions off | build | — | — |
| Compatible with Jolt's build requirements | implemented + works | IMPLEMENTED | Jolt built by its own CMake with our flags (CMakeLists:248-266) | `Physics.*` | — | — |
| Clean, modular, maintainable | needs refactor | IMPLEMENTED | libraries per subsystem (`ou_collision`, `ou_physics`, `ou_game`, ...) | — | — | `World` is one class across World / Stream / Objects / Combat / Gadgets / Vehicle / Camera .cpp (`Game.hpp` 1338 lines); `Player.cpp` 2846 lines; `tests/test_game.cpp` 6216 lines; `Script.cpp` 2817 |
| Avoid unnecessary allocations during gameplay | partial | IMPLEMENTED BUT UNVERIFIED | — | — | — | Per-tick heap use is visible in the code: `candidates()` vectors (Cmb:899) twice per tick with a weapon out, `stepPhysics` builds `std::map` / `std::vector` of joints and plates every tick (Obj:1803-1830), `unitRefs()`, string-built clip names (Cmb:113, 142), `std::string` clip comparisons as sub-states. Not measured |
| Avoid redundant physics calculations | partial | IMPLEMENTED BUT UNVERIFIED | Jolt terrain built only when a body needs it; joint bodies never sleep (Obj:2127 comment) | — | — | Terrain exists twice (BVH + Jolt mesh); not measured |
| Optimize collision queries | implemented + works | IMPLEMENTED | per-unit BVH parts appended on streaming (Str:468 `rebuildCollision`) | `Collision.PartsMatchOneBuild`, `Collision.MatchesBruteForce` | 8.3 ms -> per-unit parts (Str:467) | rope bisection = up to 10 rays per new bend (G:92) |
| Appropriate collision filters | implemented + works | IMPLEMENTED | masks + Jolt layer pairs | `Collision.MaskAndIgnoredFaces` | — | — |
| Avoid duplicate gameplay logic | needs refactor | IMPLEMENTED | — | — | — | Push logic twice (Scr:2445 blocks, Obj:564 uber joints with a copied `kSpeed`); Lara represented three times (controller capsule, Jolt proxy, ragdoll); ballistic integration four times (player air, projectiles, grenades, enemy leaps); object collision in two shapes |
| Separate gameplay from physics implementation | implemented + works | IMPLEMENTED | Jolt behind `physics::Scene` | — | — | — |
| Separate animation from movement decisions | needs refactor | PARTIALLY IMPLEMENTED | root motion drives movement by design [O]; decisions read clip names (AUDIT_A §12) | — | — | AUDIT_A |
| Keep parameters configurable | partial | PARTIALLY IMPLEMENTED | tuning structs filled from game data | `CombatReal.DecodedCombatNumbers` | — | [I] constants scattered as `constexpr` in .cpp files (list in §4 architecture row) |
| Robust error handling | implemented + works | IMPLEMENTED | `cdc::FormatError`, `save::SaveError`, `World::warnings` | `Save.RejectsCorruptAndTruncated`, `Anim.RejectsTruncatedAndInconsistent`, `Stream.MissingNeighbourIsAWarning` | — | — |
| Preserve existing functionality | implemented + works | IMPLEMENTED | 675 ctest entries pass at 0.41.0 (commit message 4437cb5) | — | — | `game_vulkan_rt_smoke` excluded (hang, project memory) |
| Avoid hardcoded test-only behavior | implemented + works | IMPLEMENTED | developer options are CLI (`--die`, `--target`, `--weapon`, `--spawn`); `puzzle_cheat_button` objects no longer fire at birth (PR "gates 4") | — | — | `DummyTarget` lives in the gameplay header (Cmb.h:83), harmless |
| Tests exercise the actual gameplay implementation | partial | IMPLEMENTED | gtests tick the real `World`; replays run `ou-game` | — | — | The gate harness places Lara (`place()` teleports) instead of walking; synthetic-clip worlds differ from the real clip set |
| Fix compiler warnings and runtime errors | partial | IMPLEMENTED BUT UNVERIFIED | `-Wall -Wextra -Wpedantic` (CMakeLists:26), `OU_SANITIZE` ASan + UBSan option | — | — | No `-Werror`; warning count not checked in this audit (no build) |
| Third-party dependencies properly managed | implemented + works | IMPLEMENTED | `FetchContent` with pinned hashes (Jolt, DXC), licences under `third_party/` | — | — | — |
| Fidelity not traded for frame rate | implemented + works | IMPLEMENTED | simulation independent of render rate | `Game.SimulationIndependentOfRenderRate`, `game_render_rate_smoke` | — | — |
| Optimize only after measuring | implemented + works | IMPLEMENTED | `World::tickTimes`, frame-time logs (`--frames`) | — | — | no per-system tick budget recorded for gameplay |

---

## Docs vs code disagreements found

- GP "Numbers" (Step up / down row): "our capsule stays radius 100"; code: `Tuning::radius` 79.01 (Game.hpp:164).
- GP "Pickups": "Health packs are not yet usable"; code: `Combat.HealthPacks` path at Cmb:1147.
- GP "Combat" adrenaline: "full meter + hard lock + close + interact opens the window" and "every hit fills the meter
  by damage / 1000" (text marked as predating the decode); code: window from adrenaline mode at any meter level
  (Cmb:1313), gain = damage / 500.
- GP "Combat" death: "the weapons, clips and reserves are kept"; code keeps them at death (Cmb:1170) and then clears
  them in `respawn` (Str:375).
- GP "Breakables": urn rewards "are separate intros"; the Wine instance dump shows a reward with a runtime id.
- ROADMAP "Planned: Jolt" step 2 "Not done: air control torques, wheelies / power slides / donuts, the lean"; code and
  tests have air control, lean, donut and power slide (`VehicleReal.AirControlTurnsMidJump`,
  `LeanFollowsSteeringAtSpeed`, `DonutAndPowerSlide`).
- ROADMAP menus row "not ported: PDA sonar map / weapon / inventory / journal sub-screens"; code and tests have them
  (`Menus.PdaWeaponsInventoryAndSonar`, `Menus.PdaJournal`, `PdaReal.SonarMapFrameRevealAndModels`).
- Cmb.h projectile comment "the world's -3 per frame^2" against the scene gravity 5.72443 (Phy:568): one of them is
  not the Havok world's value.

## Classification counts

Rows of the §4, §8, §9, §10, §11, §14, §19 tables (the §17 coverage table and the weapons table have no
classification column):

| Classification | Rows |
|---|---|
| implemented + works | 129 |
| works but differs from original | 49 |
| partial | 52 |
| placeholder | 0 |
| missing | 28 |
| incorrect | 5 |
| needs refactor | 10 |

## Prioritized worklist

Order: what blocks the campaign or loses player state first, then missing mechanics the walkthrough needs, then
fidelity of numbers, then architecture.

- **P1 DONE (agent/GpSystems).** Respawn restores the checkpoint save (taken at checkpoint activation, at a reward
  pickup with a checkpoint active and at the level start), as tru.exe 0x5570F0 / 0x557130; the loadout is the
  save's. docs/GAMEPLAY.md "Checkpoints and respawn"; `Stream.DeathRestoresTheCheckpointSave`,
  `StreamReal.DeathRestoresTheCheckpointSave`, self-test `checkpoint_respawn`.
- **P2 Urn / jar rewards and the 179-treasure total.** Decode the urn -> reward link and spawn the reward when an urn
  breaks (`World::damageObject`, Obj:821, state 2 action; new pickup instances in `UnitData::pickups`); rebuild
  `chapterRewards` from the survey; assert 179 / 6. Truth: `*_pt_weightbreak*` intro data and
  `*_weightbreak_reward` / `tp_rw_artifact_spawned` scripts (`OU_PROGRESSION_DUMP`, `OU_OBJ_FIELDS`), uber action 19
  births; Wine instance dump before / after breaking an urn.
- **P3 Breakables by contact.** Uber triggers 6 / 7 / 8 (0x4D5560: contact impulse above the record's value) for
  kicks, rolls, falling weights and the bike; class-30 breakables (0x6D3160: ledge breaks, braced doors, falling
  floors, breakable pole, temple floor). Code: Obj `tickUber` / `damageObject`, a Jolt contact listener in Phy.
- **P4 Melee, kicks, thrall finishers** (peer GpMelee): none exists. Code: new player states in `Player.cpp`, hits via
  `Damageable::onHit` with the attack records' classes; Lara's attack records are in lara.drm object data (the pound's
  record at section 850 +0x35EB8 is the pattern, Cmb.h:281). Truth: chart actions around 0x6E68B0 / 0x69F1E0, enemy
  `Regenerate::kills` classes.
- **P5 Relic / treasure effect on health.** Decode 0x5DB6A0 (reward path -> `heal(…, 1)`, overheal to 1.4 x max) and
  the reward base scripts 3674 / 3924; call it from `collectPickups` (Str:605). Test in `test_progress.cpp`.
- **P6 Adrenaline head shot rules.** Gate on a full meter (Cmb:1313), decode the window (0x715020, 0x718EA0, accurate
  aim events 5 / 4): length, pip motion, valid targets, failure; settle the "grapple-assisted vault" question against
  the controls page and the exe. Real-enemy test in `CombatReal`.
- **P7 Combat movement set.** Lock-relative movement, evasive jumps / directional rolls, shooting one-handed from
  ledge / ladder / pole / beam (clips exist per weapon DRM: survey `combat_pistol_*` names), knock-down and get-up,
  enemy vault (death entries flag 7), under-water dodge. Code: `canShoot` (Cmb:188), `Player::tick`, `aimLayer`.
  Truth: lara.drm state chart (dice tools) for the combat states.
- **P8 Under-water weapon rules and firing cadence.** Which weapons fire submerged (every wp_* has `fire_submerged`
  clips) and shots on the clip event 0x75 (0x716F30 -> 0x19B -> 0x6C9E50) instead of timers; chapter start reserves
  from the +0x848 / +0x85C / +0x852 tables (0x6EE930). Code: `canShoot`, `fire`, `chapterLoadout` (Cmb:765).
- **P9 Portables: carry clips, collision while carried, throw.** Obj:1393-1445: carried object is posed through
  walls and set down by teleport; no throw. Truth: Lara's chart actions 0x4D / 0x50 neighbourhood, `portablebase`
  script (base 3937), lara.drm carry / throw clip names.
- **P10 Push blocks as constrained bodies (or documented kinematics) with full collision.** Scr:2445-2466: terrain
  ray only, [I] speed 120. Truth: PushPull state 0x701310 / "PushPull Proxy" 0x7358E0; a Wine trace of block speed.
- **P11 CharacterVirtual evaluation** per the plan above (Phy `Scene`, P `slide / groundMove / airMove`), with the
  written selection. Do it before more [I] controller tuning accumulates; measure the original's slope limit first.
- **P12 Grapple rope: measured dynamics, force on targets, Jolt queries.** G:19-22, 319-327, 378-385 are [I]. Truth:
  "[GrappleRope]" set-up in the decomp; Wine traces of a swing period, pump gain, climb speed. Fix the jump-releases
  defect first (AUDIT_A).
- **P13 Motorcycle verification and gaps.** No Wine trace of the bike exists: record 0-40 mph time, braking distance,
  steady-turn radius, a ramp jump (Instance +0x20 / +0x2A0 of the bike). Then: headlight (motorcycle.drm effect
  list), boost / speed burst ([U]: remaining fields of the 0x20C record, controls page), hop ([I], maybe not
  original), wall-crash and reverse tests, steep-ground torques +0x15C / +0x160, jump dismounts. Code: Veh:314-391.
- **P14 Checkpoint activation and PDA truths.** Checkpoint scripts in checkpoint.drm (radius [I], Str:532), sonar
  reveal volumes (0x6E33D0, Str:554), journal unlock native (`ui::journalEntries` [I]).
- **P15 Dual targeting / manual aim.** `fire` (Cmb:1044) one aim point; no manual-aim mode. Truth: targeting slots in
  Lara's data (+0x464 grapple, +0x470 soft, +0x480 hard are decoded; look for per-hand targets) and the camera's aim
  mode.
- **P16 Hazard inventory with a test each.** Crusher force stand-in (Obj:1017 [I]); list blades / darts / fire / eitr
  from the matrix files, bind their script natives, one `ProgressionReal` or self-test per kind.
- **P17 Tests the goal names that have nothing today** (after the mechanics exist): sprint jump, ledge saving grab,
  grapple swing landing on a destination, bike gap jump + continue, bike wall crash, respawn loadout, relic effect,
  urn treasure, focused fire and head shot on a real enemy, walkthrough sequences from chapter starts by input.
- **P18 Havok world constants.** Gravity of the rigid-body world (5.72443 vs the "-3 per frame^2" in 0x6C91D0),
  friction / restitution, bullet impulse on objects (`kShotImpulse` Obj:805), plate motor numbers (Obj `kHold`).
- **P19 Refactor debts** (no behaviour change; schedule with the mechanics that touch them): split `World`
  (Game.hpp), single push implementation, single ballistic integrator, per-tick allocations in `candidates` /
  `stepPhysics`, [I] constants into the tuning structs, docs corrections listed above.
