# Technical audit, part A: movement, grapple, swimming, animation, camera / input, state machine

Scope: gameplaygoal.md §1 (audit) applied to §5, §6, §7, §12, §13, §15. Project state audited: OpenUnderworld main
4437cb5 (v0.41.0). Method: the code was read (`src/game/Player.cpp`, `Climb.cpp`, `Traverse.cpp`, `Grapple.cpp`,
`Camera.cpp`, `Input.cpp`, `World.cpp`, `Game.hpp`, `src/anim/Anim.hpp`, `src/config/Config.cpp`), test names taken
from `tests/test_game.cpp` (suites `Game` = synthetic collision / clips, `GameReal` = real units, need `OU_GAME_DIR`),
docs grepped. Nothing was built or run (docs-only agent); "passing" = the test is in the tree whose full suite the
0.41.0 notes report green. Companion file: `controls.md` (per-control table against the original's PC controls).

Legend
- Classification (goal §1): `implemented + works`, `works but differs from original`, `partial`, `placeholder`,
  `missing`, `incorrect`, `needs refactor`.
- Status: `NOT IMPLEMENTED`, `PARTIALLY IMPLEMENTED`, `IMPLEMENTED BUT UNVERIFIED`, `IMPLEMENTED`,
  `VERIFIED AGAINST ORIGINAL`.
- Provenance: [O] decoded from data / measured in tru.exe, [I] inferred, [U] unknown.
- P.cpp = `src/game/Player.cpp`, T.cpp = `Traverse.cpp`, G.cpp = `Grapple.cpp`, C.cpp = `Climb.cpp`, W.cpp = `World.cpp`.

## §5 Lara movement

### Basic movement

| Requirement | Classification | Status | Code (file:symbol) | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Idle | implemented + works | IMPLEMENTED | P.cpp `Player::tick` case Idle (`ng_idle`) | `LaraClipsAndRootMotion`, `TransitionsReducePops` | clip [O] | No idle variants / fidgets; wading idle clip choice [I] |
| Walking | implemented + works | IMPLEMENTED | P.cpp:1965 Walk (`ng_walk`), `groundMove` | `StairsWalkAndRunUpAndDown`, `WalkingSlowlyOffEdgeFallsOnce` | speed = clip root motion 348 u/s [O]; walk when stick < 0.5 [I] | No walk start / stop clips |
| Running | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:1970 Run (`ng_run_start`, `ng_run`, `ng_running_stop`) | `StairsWalkAndRunUpAndDown` (Wine stair risers), `PoolRunReplayStaysOnFloor` | root motion 1274 u/s [O]; step 110 [O Wine] | — |
| Sprinting | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:1974-1982 (`sprint_loop`) | `SprintStaminaMatchesTru` | [O Wine l4_trailstart]; clip speed 1927 vs ~1860 measured on a curve | No `ng_sprint_up/_down` on stairs / slopes |
| Directional movement | works but differs from original | IMPLEMENTED BUT UNVERIFIED | P.cpp:1907 `target`, `approachAngle(.., turnRate)` | none asserting the turn rate | `Tuning::turnRate` 6 rad/s [I] | Original picks start / turn clips from the state chart by stick angle; ours rotates the heading under one run loop |
| Camera-relative movement | implemented + works | IMPLEMENTED | W.cpp:584 (stick rotated by `cam_.yaw`) | every World replay test drives it (`PoolRunReplayStaysOnFloor`, `CameraSwingsBehindRunningLara`); no test found by name that asserts heading = camera yaw + stick | rule per the controls page [O]; free climbing uses the raw stick (P.cpp:2074) [O chart quadrants per C.cpp] | Add one explicit test |
| Character turning | partial | IMPLEMENTED BUT UNVERIFIED | P.cpp:1957, 1996 `State::Turn` (`ng_turn_center` root rotation) | `TurnInPlaceUsesClipRootRotation` | clip rotation [O]; 1.75 rad threshold and the moving turn rate [I] | No 180-degree run pivot, no left / right turn clip choice (one clip either way) |
| Acceleration | works but differs from original | IMPLEMENTED BUT UNVERIFIED | none as such: speed = clip root motion (`advanceClip` -> `groundMove`) | — | [I] | No velocity state on the ground; idle -> run goes through `ng_run_start`, idle -> walk and walk <-> run switch instantly. Original ground speeds per tick not traced for starts |
| Deceleration | works but differs from original | IMPLEMENTED BUT UNVERIFIED | P.cpp:1972 `State::Stop` (`ng_running_stop`) | — | [I] | Walk stops instantly; sprint stop = run stop |
| Movement transitions | partial | IMPLEMENTED | `clipBlend` (lara.drm anim-request blend / start frames), `anim::AnimController` | `TransitionsReducePops` | blend frames [O]; which transition exists is hand-coded [I] | The original's state chart transitions are ported piecemeal (S / T ids quoted per feature), not as data |
| Crouching | implemented + works | IMPLEMENTED | P.cpp:2017 `State::Crouch` | `CrouchUnderSlabAndRoll` | crouch height 260 [O player data +0x14] | — |
| Forward rolls | implemented + works | IMPLEMENTED | P.cpp:1930, 2030 `State::Roll` | `CrouchUnderSlabAndRoll` | clips [O]; trigger = drop while running (controls page) | Roll out of a sprint uses its own clip; no roll after a high landing |
| Somersaults | partial | PARTIALLY IMPLEMENTED | roll above; wall-hug somersault T.cpp `wallHugJump` | `WallHugSideAndTwistJumpsByTheirQueries` | [O clips] | Other acrobatic combinations (flips, handsprings) absent; their chart states not decoded [U] |
| Dropping from ledges | partial | PARTIALLY IMPLEMENTED | P.cpp:755 `tryDropToHang` (drop key at an edge), `groundMove` -> `startFall` | `DropToHangFromEdge`, `PoolDropToHang`, `RunsOffEdgeFallsAndLands` | clip `drop_to_ledgeb` [O]; edge reach [I] | The automatic safety drop (stepping off a grabbable edge turns into a hang without a key) is absent; hurdle-down exists (`tryHurdleDown`, `PoolRockStepDown`) |
| Sprint fatigue and recovery | implemented + works | VERIFIED AGAINST ORIGINAL | `PlayerState::sprintUsed`, P.cpp:1902 | `SprintStaminaMatchesTru`, `SwimSprintMatchesTru` | [O Wine] 9.7 s, resume 0.9 s | — |
| No Wii jogging on PC | implemented + works | IMPLEMENTED | P.cpp:1908 (`wantsWalk`: walk key or stick < 0.5; else run) | `ControlBindings` | [O default bindings 0x487710] | — |

### Jumping

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Standing jumps | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:2103 JumpPrep, `Tuning::jumpVz` | `JumpTakeOffsMatchTruTraces`, `BedroomJumpsMatchTruTraces`, `GravityJumpAndLanding` | [O Wine] 48.04 / frame, 359 ms delay | — |
| Directional jumps | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:2109 (`standMoveJumpVz`, `standJumpSpeed`); stick behind her = turn into a running jump (P.cpp:1922) | same | [O Wine] 49.97 up + 40 along | No back / side flip clips (one air clip set `NG_InAir_Feet_Forward_*`) |
| Running jumps | implemented + works | VERIFIED AGAINST ORIGINAL | `runJumpVz`, `runJumpSpeed` | same + `RunningJumpReachAndRush`, `JunglehubRunningJumpReachesHigherLedge` | [O Wine] 42.53 up + 65.83 fwd | — |
| Sprinting jumps | missing | NOT IMPLEMENTED | P.cpp:1922 treats `sprint_loop` as a run | — | [U] | Needs a Wine take-off trace (as `openunderworld-work/screens/controls/s_*.txt`) and its clip; GAMEPLAY.md:2548 agrees |
| Longer jumps | partial | PARTIALLY IMPLEMENTED | run-up jump only | `RunningJumpReachAndRush` | [O] | The two original ways to go farther besides the run-up (sprint, holding Jump) are absent |
| Jump duration control | missing | NOT IMPLEMENTED | `Player::airMove` (P.cpp:441) never reads `Actions::jump` | — | [U] | Controls page: holding Jump keeps her in the air a little longer. Trace player +0x2A0 velocity with / without the key held |
| Midair directional adjustment | missing | NOT IMPLEMENTED | `airMove(dt, move)`: `move` is used only by `land()` | — | [U] | Whether tru.exe steers in the air at all is not measured; trace the same velocity while flipping the stick mid-flight |
| Jumping toward ledges | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:1834 `aimGroundJump`, 1360 `ledgeOnArc`, 1509 `standJumpToLedge`, `assistJump` | `StandingJumpCatchesHighLedgeWithinReach`, `StandingJumpBehindLedgeCatchesNothing`, `RunningJumpReachAndRush`, `AssistedJumpLandsOnJumpLandingMarkup` | queries q2 / q26 / q43 and class records [O lara.drm]; flight time by hit frame, `kQueryStep`, `assistRadius` 300 [I] | Hurdle-band ledges go to plain jumps (P.cpp:1830 comment) |
| Automatic grabbing | implemented + works | IMPLEMENTED | P.cpp:517 `tryGrab`, 560 `tryGrabPole`, `tryLadder`, `tryPerch`, `tryClimb` in `airMove` | `LedgeGrabHangShimmyDrop`, `CliffclimbObstaclesAndLedgeGrabs`, `PoolRockGrabNotHurdle`, `FallingOntoBeamCatchesIt` | zVel window [O player data +0x833 / +0x834]; reach 60 / window 40 / facing cos 0.5 [I]; decoded per-markup capsules exist but are not used for the air grab (memory note 0.34.0) | Air-grab geometry is ours |
| Landing | partial | IMPLEMENTED BUT UNVERIFIED | P.cpp:416 `land` (`ng_generic_land_forwards`, `ng_running_land`) | `GravityJumpAndLanding`, `RunsOffEdgeFallsAndLands` | 600 u/s run-on threshold [I] | No hard-landing / landing-roll clips by fall height; fall damage is part B |
| Recovery after landing | partial | IMPLEMENTED BUT UNVERIFIED | P.cpp:1989 `State::Land` until the clip ends; jump refused during it (P.cpp:1919) | — | [I] | Lock-out length = whole clip; original interrupt frames not decoded |
| Failed jumps | implemented + works | IMPLEMENTED | `airMove` slides along walls and falls; saving grab | `WalkingIntoWallSlides`, `StandingJumpBehindLedgeCatchesNothing` | [I] | No wall-bounce / bad-jump clips |
| Falling | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:400 `startFall`, 441 `airMove` | `GravityJumpAndLanding`, `RunsOffEdgeFallsAndLands` | gravity 5.72443 / frame^2 and integrator 0x5DB370 [O]; no terminal velocity [O] | One fall clip only |

### Ledge traversal

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Automatic ledge grabbing | implemented + works | IMPLEMENTED | P.cpp:517 `tryGrab` | `LedgeGrabHangShimmyDrop`, `CliffclimbObstaclesAndLedgeGrabs` | window [O], geometry [I] | see Automatic grabbing |
| Safety grabs | partial | PARTIALLY IMPLEMENTED | `tryDropToHang` needs the drop key | `DropToHangFromEdge` | [I] | No key-less grab when she steps or runs off a grabbable edge |
| One-handed saving grabs | incorrect | PARTIALLY IMPLEMENTED | P.cpp:519, 548 (`ledgeb_bad_attach_M`), 2178 recover; C.cpp:269 (DICE bad attach) | `ClimbBadAttach` | time table [O generalbank +0x24, meaning I]; trigger "falling up to 2x the grab window" [I] | Recovery key is Jump; the original uses Interact. Trigger rule not the original's (far / odd-angle jumps). No ledge-hang test of the timer |
| Two-handed hanging | implemented + works | IMPLEMENTED | `State::Hang`, `placeOnLedge` (hands of the hang pose on the line) | `LedgeGrabHangShimmyDrop` | hand offset from `ledgeb_idle` pose [O] | No `ledgeu_*` free hang (no wall under the ledge) (GAMEPLAY.md:2542) |
| Horizontal movement | implemented + works | IMPLEMENTED | P.cpp:2271 Shimmy | same | speed = clip root motion [O] | Left = right clip at rate -1 (played backwards), not the mirrored request |
| Shimmying | implemented + works | IMPLEMENTED | same | `CameraHangYawForceFollowsShimmy` | [O clip] | — |
| Moving around corners | implemented + works | IMPLEMENTED | P.cpp:657 `startHangCorner`, 680 `hangCorner` | `HangShimmyRoundCorners` | clips [O], mapping of concave / convex [I] | — |
| Changing handholds | implemented + works | IMPLEMENTED | P.cpp:1197 `startHangJump`, `launchHangJump` | `HangJumpsUpRightAndBack`, `HangJumpRecordsReachAndClass` | queries q7..q10 [O] | — |
| Dropping to lower handholds | partial | IMPLEMENTED BUT UNVERIFIED | P.cpp:2191 drop -> `startFall`, `regrabCooldown` 0.4 s, then `tryGrab` | no test found by name for the re-grab below | [I] | A ledge closer than 0.4 s of fall is missed; faster than the window becomes a saving grab |
| Pulling up | partial | IMPLEMENTED | P.cpp:2218 `climb("ledgeb_climbup_fast")`, `hangClearance` | `LedgeGrabHangShimmyDrop`, `ChapelHangClimbsOntoBalanceBeam` | clearance classes 0x74E290 [O] | Hold-Jump handstand pull-up missing; only the fast climb-up clip |
| Jumping to higher handholds | implemented + works | VERIFIED AGAINST ORIGINAL | `startHangJump(0)`, `hangJumpUpVz` 75.85 / frame | `TempleLedgeJumpsUpAndBack`, `HangJumpsUpRightAndBack` | q10 [O data] | — |
| Jumping between handholds | implemented + works | VERIFIED AGAINST ORIGINAL | `startHangJump(+-1 / 2)` | `PoolSideJump`, `HangJumpLeans` | q7 / q8 / q9 [O] | — |
| Transitioning from hanging into a jump | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:2237 free leap (`hangLeapBack / Side / Vz`) | `HangLeapsMatchTruTraces` | [O Wine l3_main_bedroom 973] | — |
| Falling when no valid handhold exists | implemented + works | IMPLEMENTED | `tryGrab` false -> fall; saving-grab timeout | `StandingJumpBehindLedgeCatchesNothing`, `BeamFallWithoutLedgeDrops` | — | — |

### Climbing

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Vertical climbing | implemented + works | IMPLEMENTED | C.cpp:207 `Player::climb` (DICE clips, holds = markup 16-20) | `ClimbAttachClimbUpTopOut`, `CliffclimbStances`, `ClimbStaysWithinHolds` | queries q80 / q81 [O]; 2D sweep [I] | — |
| Horizontal climbing | implemented + works | IMPLEMENTED | same (q73) | `ClimbStanceFollowsHolds`, `CliffclimbDiceJumps` | [O] / sweep [I] | — |
| Climbing around corners | partial | PARTIALLY IMPLEMENTED | ledges: `hangCorner`; free climb: C.cpp:300 accepts only holds within 150 of the same plane facing the same way | `HangShimmyRoundCorners` | [I] | No free-climb move round a rock corner |
| Climbing across wall-mounted handholds | implemented + works | IMPLEMENTED | C.cpp (holds), `tryClimb` | `ClimbOnlyAtHolds`, `ClimbNeedsHolds` | hold markup [O]; hold-type semantics [U] | — |
| Climbing ladders | implemented + works | IMPLEMENTED | T.cpp:93 `tryLadder`, 300 `ladder` | `LadderRungsClimbOffTop`, `ShipSinkingHallLadder` | rung alignment [O 0x734B70 / player data] | — |
| Faster climbing using timed interaction input | missing | NOT IMPLEMENTED | — (`Actions::interact` unused in P.cpp / C.cpp / T.cpp) | — | [U] | Applies to shimmy, poles, pillars, ladders. Find the chart's conditions on virtual button 0 in those groups |
| Jumping away from ladders | implemented + works | IMPLEMENTED | T.cpp:247 `ladderLeap`, 180 `ladderJumpTarget` | `LadderSideJumpsReachAndBack`, `ArcticSeaLadderSideJumpOntoBeam`, `LadderJumpUpToLedge` | q22 / q23 [O data] | — |
| Releasing and re-grabbing lower ladder positions | works but differs from original | PARTIALLY IMPLEMENTED | T.cpp:376 drop; P.cpp:508 `tryLadder(true)` | — | [I] | Re-grab is automatic after 0.3 s; original needs Interact (the one manual grab in the game) |
| Pulling up onto surfaces | implemented + works | IMPLEMENTED | `ladder_detach_ground_top`, `ladder_jump_up_at_top`, DICE jump up -> ledge -> climb up | `LadderRungsClimbOffTop`, `ClimbAttachClimbUpTopOut` | [O clips] | — |

### Chimney jumps (all rows: main has no code; peer `GpChimney` implements it this wave on its own branch)

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Opposing-wall detection | missing | NOT IMPLEMENTED | — | — | [U] | grep for chimney / wall_jump / walljump over src = 0 hits |
| Correct wall contact | missing | NOT IMPLEMENTED | — | — | [U] | `airMove` only slides along walls |
| Jump initiation | missing | NOT IMPLEMENTED | — | — | [U] | |
| Rebound timing | missing | NOT IMPLEMENTED | — | — | [U] | |
| Directional movement | missing | NOT IMPLEMENTED | — | — | [U] | |
| Upward progression | missing | NOT IMPLEMENTED | — | — | [U] | |
| Collision handling | missing | NOT IMPLEMENTED | — | — | [U] | |
| Animation transitions | missing | NOT IMPLEMENTED | — | — | [U] | no wall-jump clip in `kLaraClipNames` (W.cpp:234) |
| Final ledge detection | missing | NOT IMPLEMENTED | — | — | [U] | `tryGrab` would apply once the rebound exists |

### Horizontal and vertical poles

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Automatic pole attachment | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:560 `tryGrabPole`, 596 `tryPoleLeap`; pillars T.cpp `tryLadder` | `TrapRoomsJumpToPoleStraightLine`, `TrapRoomsPoleAttachBackAndTurnTimes`, `PoleGrabSwingFlingDrop` | leap 11 ticks [O Wine]; reach 330 / 250, perp / para split [I] | — |
| Climbing vertical poles | implemented + works | IMPLEMENTED | T.cpp:441 `pillar` | `PillarAttachRoundUpSlideOff` | slide speeds [O Wine]; climb = clip | — |
| Rotating around vertical poles | implemented + works | IMPLEMENTED | `pillar_climb_right` (mirrored left), `kPillarRoundRate` | same | requests 623 / 624 [O]; rate [I] | — |
| Climbing along horizontal poles | implemented + works | IMPLEMENTED | P.cpp:1669 parallel hang | `TrapRoomsPoleParallelHandOverHandAndTurns` | requests [O]; stick bands [I] | — |
| Hand-over-hand movement | implemented + works | IMPLEMENTED | `HPole_ParaForward*` | same | [O clips] | — |
| Swinging around horizontal poles | implemented + works | IMPLEMENTED | P.cpp:1633 `HPole_StartSwing / HPole_swing` | `TrapRoomsPoleSwingStops`, `PoleGrabSwingFlingDrop` | clips [O]; stop-clip choice [I] | — |
| Jumping off poles | works but differs from original | IMPLEMENTED | P.cpp:1714 `poleFling` | `TrapRoomsPoleFling`, `MainHallPoleFlingLandsOnPlatform` | flight times [O Wine]; target pick [I] | Leaves a fixed delay after the press; original (controls page) rewards pressing at the bottom of the swing: phase dependence not modelled |
| Pulling up onto horizontal poles | implemented + works | IMPLEMENTED | P.cpp:1581 `hpole_to_perch` | `TrapRoomsPoleTop` | chart T1231 / T1235 [O] | — |
| Crouching and dropping from poles | implemented + works | IMPLEMENTED | P.cpp:1572 drop clips; 1791 `hpole_perch_fall_to_hpole` | `PoleGrabSwingFlingDrop`, `TrapRoomsPoleTop` | [O clips] | — |
| Traversing the top of poles | missing | NOT IMPLEMENTED | P.cpp:1771 comment: `hpole_perch_to_top_run` not ported | — | [O chart exists] | Walk along the top onto the beam states |

### Balance beams

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Automatic balance detection | implemented + works | IMPLEMENTED | P.cpp:727 `tryBeam` (markup 14) | `BalanceBeamWalkTurnStepOff`, `FallingOntoBeamCatchesIt` | markup [O]; `beamAttach` 30 [I] | — |
| Walking across narrow beams | implemented + works | IMPLEMENTED | P.cpp:2356 `beam` | `TempleBalanceBeamWalkAndJump` | clip [O] | — |
| Faster movement | missing | NOT IMPLEMENTED | — | — | [U] | Sprint key on a beam (GAMEPLAY.md:2542 "beam sprint") |
| Jumping off beams | partial | IMPLEMENTED BUT UNVERIFIED | P.cpp:2367 -> ground `JumpPrep` | `TempleBalanceBeamWalkAndJump` | [I] | Uses the ground jump; the beam's own jump transitions T1540 / T1541 are only named |
| Gymnastic walkovers | missing | NOT IMPLEMENTED | — | — | [U] | Crouch while walking on a beam |
| Balance recovery | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:2464 `beamAboutFall` | `BeamOffBalanceCounterStickRecovers`, `BeamUnbalanceAndWobble` | controller 0x742770 + tuning objdata +0xF88 [O decoded]; entry (`kBeamTipFrames`) [I] | How the walk enters about-to-fall (chart conditions kind 1000) not decoded |
| Proper foot positioning | partial | IMPLEMENTED BUT UNVERIFIED | root snapped to the beam line (P.cpp:2423) | — | [I] | No foot IK; correctness rests on the clips |
| Falling when balance is lost | implemented + works | IMPLEMENTED | P.cpp:2551 `startBeamFall`, 2601 `beamFalling` | `BeamOffBalanceEscalatesToFallAndCatchesLedge`, `BeamFallsCatchScaffoldLedge`, `BeamFallWithoutLedgeDrops` | fall flag rules [O] | Drop key = falls to her right [I] instead of dropping to a hang (`bbeam_drop_*` unused) |

### Traversal state transitions

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Run -> jump -> ledge grab -> shimmy -> corner -> pull up -> run as one chain | partial | IMPLEMENTED BUT UNVERIFIED | every link exists (rows above) | links are tested apart (`CliffclimbObstaclesAndLedgeGrabs`, `HangShimmyRoundCorners`, `TraversalIndependentOfRenderRate`); no single test runs the whole chain | — | Add the chain as one scripted test |
| Jumps between poles, wall jumps, ladder jumps and other combinations | partial | PARTIALLY IMPLEMENTED | pole -> pole / ledge (`poleFling`), ladder -> ledge / ladder / beam, perch jumps, wall-hug jumps | `TrapRoomsPoleFling`, `ArcticSeaLadderSideJumpOntoBeam`, `CavernsPerchJumpReaches`, `PerchJumpsByTheirQueries` | [O queries] | Wall (chimney) jumps missing |

## §6 Grapple

General: `G.cpp:7` states that the swing and all its rates are [I] (the original runs Havok rope bodies). Decoded [O]:
target sets, range / rope limits, bend rules, overhang constants.

### Basic grapple

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Valid grapple target detection | implemented + works | IMPLEMENTED | `UnitData::grapples`, Combat.cpp:953 `World::grappleTarget`, `uberGrappleOff` | `PoolGrappleTargetAndSwing`, `GrappleIgnoresEnemies` | intros, 0x404 block, uber actions 0x19 / 0x1A [O] | — |
| Automatic target selection | implemented + works | VERIFIED AGAINST ORIGINAL | same: six sets from player data +0x464 | `ChapelRingFromTheColumnTop`, `MainHallRingByItsOwnRange` | sets [O]; tie-break "nearest the view" [I] | — |
| Grapple firing | partial | PARTIALLY IMPLEMENTED | W.cpp:549-593, G.cpp:118 `attachGrapple` | `SharedQThrowsTheGrappleNotTheCamera` | [I] | No hook flight: the rope exists the tick Q is pressed; the throw clip is `grapple_rappel_free_jumpon` for every non-mounted throw. No state gate found: W.cpp:549 throws from any player state (hanging, swimming, mid-air); the original's allowed states are [U] |
| Grapple attachment | implemented + works | IMPLEMENTED BUT UNVERIFIED | `attachGrapple` | `GrappleSwingAndRelease` | instant [I] | Attach timing / miss cases |
| Cable creation | implemented + works | IMPLEMENTED | `PlayerState::ropeLength / ropeBend`, G.cpp:62 `wrapRope` | `GrappleRopeBendsRoundGeometry` | bend rules 0x6C5490 / 0x6B3570 [O]; one ray instead of sphere casts, hand at root + 400 [I] | — |
| Cable length adjustment | works but differs from original | PARTIALLY IMPLEMENTED | G.cpp:378-386 | `KrakenRoomRappel` | limits [O g_grapple]; 300 u/s [I] | Free hang: E alone up, Walk (Alt) down; original: E + Forward / Back |
| Pulling on the cable | implemented + works | IMPLEMENTED | `State::GrapplePull`, W.cpp:606-618 | `MountedGrapplePullsBlockWithoutSwinging`, `GrapplePullsUberTargets`, `GrappleLeverJointIndependentOfRenderRate` | handler slots [O]; 1 s heave, 240 u/s block pull [I] | — |
| Cable release | implemented + works | IMPLEMENTED | `releaseGrapple`, Q again (W.cpp:591) | `GrappleSwingAndRelease` | [I] | — |
| Reattachment | implemented + works | IMPLEMENTED BUT UNVERIFIED | no cooldown; a new Q throws again | — | [I] | No test of a mid-air re-throw; original's rule [U] |
| Proper animation transitions | partial | PARTIALLY IMPLEMENTED | clip picks in `grapple()` / `rappel()` | — | chart states S314 / S378 / S379 / S421 quoted [O]; clip per state [I] | No throw / catch / reel clips distinct from the hang set; standing with the rope plays `ng_idle` / `ng_walk` |

### Grapple swinging

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Attachment to overhead rings | implemented + works | IMPLEMENTED | `GrappleKind::Ceiling` | `PoolGrappleTargetAndSwing`, `ChapelRingFromTheColumnTop` | [O] | — |
| Jumping into the gap | incorrect | PARTIALLY IMPLEMENTED | G.cpp:306: `jumpPressed` in `GrappleSwing` calls `releaseGrapple(true)` before the grounded branch (G.cpp:313) | — | [I] | Standing roped at the edge, pressing Jump lets go of the rope instead of starting the swing; only walking off the edge swings. The original's sequence is grapple, then jump into the gap |
| Initial swing momentum | partial | IMPLEMENTED BUT UNVERIFIED | walk-off velocity (300 u/s, G.cpp:316) + gravity | `GrappleSwingAndRelease` | [I] | Should come from the jump |
| Pendulum-like swinging | implemented + works | IMPLEMENTED BUT UNVERIFIED | G.cpp:105 `ropeConstrain`, gravity [O value] | same, `PoolGrappleTargetAndSwing` | model [I] | — |
| Forward and backward movement | partial | IMPLEMENTED BUT UNVERIFIED | G.cpp:318 stick force in any world direction | — | 500 u/s^2 [I] | — |
| Swing acceleration | partial | IMPLEMENTED BUT UNVERIFIED | same | — | [I] | — |
| Swing momentum accumulation | works but differs from original | PARTIALLY IMPLEMENTED | same + drag `exp(-0.15 dt)` | no test asserts amplitude growth with Forward held | [I] | The stick adds a constant world-direction force; a constant force on a pendulum only shifts its rest point, so holding Forward cannot keep raising the swing as the original does (it needs a push in phase with the swing). Analysis from the code, not a run: verify with a test |
| Cable tension | implemented + works | IMPLEMENTED BUT UNVERIFIED | `ropeConstrain` (outward velocity removed; slack allowed) | `GrappleRopeBendsRoundGeometry` | [I] | — |
| Jumping away from the cable | partial | IMPLEMENTED BUT UNVERIFIED | G.cpp:147 `releaseGrapple(true)`: keeps velocity + 400 up | `GrappleSwingAndRelease` | [I] | Original take-off boost not measured |
| Releasing the cable | implemented + works | IMPLEMENTED | drop / Q | same | — | — |
| Correct landing behavior | partial | IMPLEMENTED BUT UNVERIFIED | G.cpp:336-354 `standUp` on a floor (vz < -300, arc-end rule), else `Jump` -> `land` | `ValaskjalfWallRun` | all thresholds [I] | — |

### Grapple climbing

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Climbing upward along the cable | works but differs from original | PARTIALLY IMPLEMENTED | G.cpp:378 (E held) | `KrakenRoomRappel` | 300 u/s [I]; min rope [O] | Input (no stick) |
| Descending | works but differs from original | PARTIALLY IMPLEMENTED | G.cpp:384 (Walk held) | same | [I] binding (comment says so) | Input |
| Cable length adjustment | partial | PARTIALLY IMPLEMENTED | same | same | clamp [O] | see above |
| Horizontal positioning | partial | IMPLEMENTED BUT UNVERIFIED | stick force only | — | [I] | — |
| Pivoting where supported | missing | NOT IMPLEMENTED | yaw follows velocity or faces the pivot (G.cpp:391-396) | — | [U] | E + Left / Right pivot |
| Correct hand and leg placement | partial | IMPLEMENTED BUT UNVERIFIED | clips only; rope drawn to root + 400 | — | `kHandZ` [I] | No IK to the rope |
| Cable interaction animations | implemented + works | IMPLEMENTED | `grapple_rappel_free_ascend / _descend`, overhang clips | `KrakenRoomRappel` | [O clips] | — |

### Rappelling

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Grapple attachment above a wall | implemented + works | IMPLEMENTED | any non-mounted anchor | `KrakenRoomRappel` | [O targets] | — |
| Movement over an edge | partial | IMPLEMENTED BUT UNVERIFIED | walk off the edge roped -> swing -> G.cpp:328 sweep finds the wall -> `rappelOnto` | — | [I] | No edge-over clip; passes through a free swing first |
| Feet braced against the wall | implemented + works | IMPLEMENTED | G.cpp:159 `rappelOnto` (`grapple_rappel_wall_land*`) | `KrakenRoomRappel`, `ValaskjalfWallRun` | `wallOffset` 100, `kSteep` [I] | — |
| Controlled descent | works but differs from original | PARTIALLY IMPLEMENTED | G.cpp:183-184 (stick away from the wall) | `KrakenRoomRappel` | 300 u/s [I] | No Interact needed (original: E + Back) |
| Upward cable traversal | works but differs from original | PARTIALLY IMPLEMENTED | stick into the wall | same | [I] | same |
| Cable length management | implemented + works | IMPLEMENTED | clamp `minRope..maxRope()` (0x698B40) | `GrappleTuningFromData` | [O] | — |
| Wall collision | implemented + works | IMPLEMENTED | G.cpp:198-223 (follow the wall, overhang drop, floor) | `KrakenRoomRappel` | overhang constants [O player data]; 150-deep steps [I] | — |
| Detachment | partial | IMPLEMENTED BUT UNVERIFIED | drop releases; jump kicks off into a swing still roped (G.cpp:176); floor -> `standUp` | — | 700 out + 200 up [I] | see wall run row 8 |

### Grapple wall running

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| 1. Grappling a valid overhead anchor | implemented + works | IMPLEMENTED | as above | `ValaskjalfWallRun` | [O] | — |
| 2. Jumping into the gap | incorrect | PARTIALLY IMPLEMENTED | G.cpp:306 (jump releases); alternative `groundWallRun` G.cpp:282 for Wall anchors | `GrappleWallRunFromGround` | [I] | Same defect as the swing; the ground start is ours |
| 3. Bracing Lara's feet against the wall | implemented + works | IMPLEMENTED | `rappelOnto` | `ValaskjalfWallRun` | [I] | — |
| 4. Maintaining contact with the wall | implemented + works | IMPLEMENTED | G.cpp:207-223 | same | [I] | — |
| 5. Adjusting cable position | works but differs from original | PARTIALLY IMPLEMENTED | stick up / down | — | [I] | No Interact |
| 6. Running alternately left and right | implemented + works | IMPLEMENTED BUT UNVERIFIED | G.cpp:186-189 | `ValaskjalfWallRun`, `GrappleWallRunFromGround` | `kWallRunSpeed` 700, `kWallRunAccel` 2500 [I] | — |
| 7. Building momentum through repeated movement | implemented + works | IMPLEMENTED BUT UNVERIFIED | feet push toward the stick side, pendulum in the wall plane, friction only with the stick released | same | [I] | Numbers unmeasured |
| 8. Jumping away at the appropriate height and direction | incorrect | PARTIALLY IMPLEMENTED | G.cpp:176: fixed kick straight off the wall into `GrappleSwing`, rope kept, stick ignored | — | [I] | Original: Jump + direction leaps off along the run or square off the wall and lets go. Ours needs a second press and has no sideways leap |

## §7 Swimming and underwater

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Surface swimming | implemented + works | VERIFIED AGAINST ORIGINAL | P.cpp:2778 SwimTread / SwimSurface | `SwimSpeedMatchesTru`, `SwimEnterCrossClimbOut`, `PoolSwimReplay` | 44.95 / frame, surface depth 44.1 [O Wine] | Surface turn clips unused |
| Underwater swimming | implemented + works | VERIFIED AGAINST ORIGINAL (speed) | P.cpp:2811 SwimUnder | `DiveSwimDownAndSurface`, `SwimSpeedMatchesTru` | speed [O]; control scheme [I] (P.cpp:2813) | — |
| Vertical movement | works but differs from original | IMPLEMENTED BUT UNVERIFIED | Jump / drop held = up / down component | `DiveSwimDownAndSurface`, `UnderwaterModelFollowsSwimPitch` | [I] | Camera pitch does not steer (controls page: Forward + camera steers); verify under Wine |
| Diving | implemented + works | IMPLEMENTED | P.cpp:2783 `swim_surfacedive` (112 fwd, 667 down) | `DiveSwimDownAndSurface` | clip root motion [O] | A tap at the surface always plays the whole dive |
| Surfacing | implemented + works | IMPLEMENTED | `State::Surfacing`; SwimUnder reaching the surface | same | [I] rise at swim speed | — |
| Directional swimming | implemented + works | IMPLEMENTED | camera-relative stick | `CameraSwingsBehindSwimmingLara` | — | — |
| Faster swimming | implemented + works | VERIFIED AGAINST ORIGINAL | `stroke()` P.cpp:2738 | `SwimSprintMatchesTru` | x1.2 and stamina [O Wine] | — |
| Acceleration | implemented + works | VERIFIED AGAINST ORIGINAL | `swimAccel` 2475 | `SwimSpeedMatchesTru` | fitted to Wine traces [O] | — |
| Deceleration | implemented + works | VERIFIED AGAINST ORIGINAL | `swimDecel` 3760 | same | fitted [O] | — |
| Turning | partial | VERIFIED AGAINST ORIGINAL (surface) | P.cpp:2787 `kSwimTurnAccel`; under water `turnRate` | `SwimTurnLagsLikeTru` | surface [O]; under water 6 rad/s [I] | — |
| Underwater collision | implemented + works | IMPLEMENTED | P.cpp:2682 `swimSlide` (sphere 100) | `PoolSwimReplay` | radius [I] | Sphere, not the body's extent along the swim axis |
| Swimming through narrow spaces | partial | IMPLEMENTED BUT UNVERIFIED | same | no test in a tunnel | [I] | Needs a real-unit test (l1 / l6 passages) |
| Automatic climbing out of suitable water edges | works but differs from original | PARTIALLY IMPLEMENTED | P.cpp:2697 `tryClimbOut`, called only on Jump (2782) | `SwimEnterCrossClimbOut` | 50..400 band [I] | Original: holding the direction climbs out; the wall-hug swim climb-out is missing (GAMEPLAY.md:2541) |
| Diving from heights | partial | IMPLEMENTED BUT UNVERIFIED | `enterWater` -> `SwimSplash` (velocity kept, drag `exp(-3 dt)`) | `WaterEntrySplashMakesRingsAndDrops` | drag [I] | Plunge depth not compared |
| Swan dives | missing | NOT IMPLEMENTED | — | — | [U] | Jump then Crouch; also the land version with a roll |
| Underwater evasive rolls | missing | NOT IMPLEMENTED | — | — | [U] | Target lock + direction + Crouch |
| Oxygen and breath management | implemented + works | IMPLEMENTED BUT UNVERIFIED | `World::tickAir` | `WaterAirDrainsRefillsAndDrowns` | 45 s / refill rates [I] (GAMEPLAY.md:2531) | Find the exe's air numbers (player data near the health block) |
| Breath meter behavior | implemented + works | IMPLEMENTED BUT UNVERIFIED | HUD AIR bar (part B) | `tests/test_hud.cpp` | [I] timing | — |
| Correct death conditions | implemented + works | IMPLEMENTED | drowning -> `death_drown` | `WaterAirDrainsRefillsAndDrowns` | [I] | — |
| Water-entry and exit animations | partial | IMPLEMENTED | `swim_jump_land`, `swim_climbout`, `swim_to_wade`, `swim_land_wading`, `wading_*` | `WadingClipsAtTheDecodedDepth`, `SwimEnterCrossClimbOut` | depth classes [O 0x5D3860]; wading graph [I] | Walk-in entry jumps straight to `Surfacing` |
| Own underwater physics / controller (not a slowed ground controller) | implemented + works | IMPLEMENTED | `Player::swim`: sphere slide, own accel / decel, pitch, water flow (`waterFlow`) | `WaterFlowPullsTowardTheSink`, `SwimSimulationIndependentOfRenderRate` | flow [O 0x6ACB60, added straight on I] | Buoyancy / sink when idle: none modelled; original [U] |
| Movement and animation synchronized | partial | IMPLEMENTED BUT UNVERIFIED | stroke clips loop independently of the velocity | — | [I] | Stroke rate is not tied to speed |
| Underwater weapons | partial | unknown (part B) | Combat.cpp (speargun under water per ROADMAP) | part B | — | Pistols under water (PC original) not checked here |
| Every underwater sequence of the walkthroughs | partial | IMPLEMENTED BUT UNVERIFIED | — | `PoolSwimReplay`, `SeaCircleCameraLevelsOut`; progression tests (part of `ProgressionReal`) | — | Per-chapter list is the walkthrough agents' output |

## §12 Animation

### Assets to identify

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Skeleton structure | implemented + works | IMPLEMENTED | `anim::decodeSkeletons` | `AnimReal.*` (tests/test_anim.cpp) | 116 segments + 37 hair bones [O] | — |
| Animation clips | implemented + works | IMPLEMENTED | `anim::decodeClip` | `Anim.*`, `AnimReal.*` | all 1744 lara.drm clips decode [O]; the controller loads 578 (`kLaraClipNames`) | 2/3 of the clips are unused |
| Animation naming | implemented + works | IMPLEMENTED | `anim::animList` | `LaraClipsAndRootMotion` | [O] | — |
| Bone hierarchy | implemented + works | IMPLEMENTED | `scene::Skeleton`, `worldMatrices` | `Anim.*` | [O] | — |
| Animation timing | implemented + works | IMPLEMENTED | `Clip::timePerKey`, 3000 units/s | `Anim.*` | [O] | — |
| Root motion | implemented + works | IMPLEMENTED | `rootTranslation`, `rootYaw`, `advanceRoot` | `RootTranslationUnwrapsLoops`, `TurnInPlaceUsesClipRootRotation` | [O] | Not applied to jump / attach / fling flights (ballistic paths) |
| Animation events | partial | PARTIALLY IMPLEMENTED | `src/fx` clip events (effects); gameplay events (grab frames, interrupt windows) not read by the controller | ROADMAP "Original effects" row | [O for fx] | Controller timings are constants (e.g. `standMoveTakeoff`, `kPoleFlingDelay`) rather than clip events |
| Contact points | partial | PARTIALLY IMPLEMENTED | `AnimLibrary::handsAt`, `hangHandOffset`, `markers` (HInfo) | `LaraClipsAndRootMotion` | hand bones 55 / 80 [O] | Feet contacts not extracted |
| Animation transitions | partial | PARTIALLY IMPLEMENTED | hand-written per state; chart ids quoted in comments | `TransitionsReducePops`, `DiceTransitionNames` | per-feature [O], no general decode | lara.drm's player state chart is not loaded as data |
| Blend behavior | partial | IMPLEMENTED | `clipBlend` (request blend / start frames), `playMix` blend spaces (hurdles, beam lean), mirror flag | `TransitionsReducePops` | [O requests] | Multi-clip requests beyond those three uses; `anim::mirrorBones` twin table [I] |

### Required categories (clip present and driven by a state?)

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Locomotion: idle | implemented + works | IMPLEMENTED | `ng_idle` | `LaraClipsAndRootMotion` | [O] | — |
| Locomotion: walking | implemented + works | IMPLEMENTED | `ng_walk` | same | [O] | — |
| Locomotion: running | implemented + works | IMPLEMENTED | `ng_run` | same | [O] | — |
| Locomotion: sprinting | implemented + works | IMPLEMENTED | `sprint_loop`, exhaust clips | `SprintStaminaMatchesTru` | [O] | — |
| Locomotion: turning | partial | IMPLEMENTED | `ng_turn_center` | `TurnInPlaceUsesClipRootRotation` | [O] | one clip |
| Locomotion: starting | partial | IMPLEMENTED | `ng_run_start` | — | [O] | run only |
| Locomotion: stopping | partial | IMPLEMENTED | `ng_running_stop` | — | [O] | run only |
| Locomotion: crouching | implemented + works | IMPLEMENTED | `crouch_*`, `Crouch_Front` | `CrouchUnderSlabAndRoll` | [O] | — |
| Locomotion: rolling | implemented + works | IMPLEMENTED | `ng_roll_front(_sprinting)` | same | [O] | — |
| Jumping: standing jump | implemented + works | IMPLEMENTED | `ng_standing_jump`, `NG_InAir_Feet_Forward_stage1_standing` | `JumpTakeOffsMatchTruTraces` | [O] | — |
| Jumping: running jump | implemented + works | IMPLEMENTED | `ng_running_jump_forwards` | same | [O] | — |
| Jumping: sprinting jump | missing | NOT IMPLEMENTED | — | — | [U] | — |
| Jumping: midair movement | partial | IMPLEMENTED | `NG_InAir_Feet_Forward_stage1..3` | — | [O clips] | One air set for all directions |
| Jumping: falling | implemented + works | IMPLEMENTED | stage3 loop | — | [O] | — |
| Jumping: landing | partial | IMPLEMENTED | 2 land clips | — | [O] | no height variants |
| Jumping: recovery | partial | IMPLEMENTED BUT UNVERIFIED | land clip tail | — | [I] | — |
| Climbing: ledge grabbing | implemented + works | IMPLEMENTED | `ledgeb_attach_M / U / S / F` | `LedgeGrabHangShimmyDrop` | [O] | — |
| Climbing: one-handed grab | implemented + works | IMPLEMENTED | `ledgeb_bad_attach_M`, `_recover` | `ClimbBadAttach` (DICE) | [O] | — |
| Climbing: two-handed hanging | implemented + works | IMPLEMENTED | `ledgeb_idle` | same | [O] | — |
| Climbing: shimmy | implemented + works | IMPLEMENTED | `ledgeb_right` | same | [O] | left = reversed |
| Climbing: corner movement | implemented + works | IMPLEMENTED | `ledgeb_concave / convex_*` | `HangShimmyRoundCorners` | [O], mapping [I] | — |
| Climbing: climbing | implemented + works | IMPLEMENTED | 181 `DICE_*` transitions | `DiceTransitionNames`, `CliffclimbStances` | [O] | — |
| Climbing: pulling up | partial | IMPLEMENTED | `ledgeb_climbup_fast`, `ledgeb_jumpuptocrouch` | — | [O] | handstand missing |
| Climbing: jumping between handholds | implemented + works | IMPLEMENTED | `ledgeb_jump_*`, DICE jumps | `HangJumpLeans`, `CliffclimbDiceJumps` | [O] | — |
| Climbing: ladder climbing | implemented + works | IMPLEMENTED | `ladder_*` | `LadderRungsClimbOffTop` | [O] | — |
| Climbing: ladder release | implemented + works | IMPLEMENTED | `ladder_detach_drop` | — | [O] | — |
| Gymnastics: chimney jumps | missing | NOT IMPLEMENTED | — | — | [U] | — |
| Gymnastics: pole climbing | implemented + works | IMPLEMENTED | `pillar_*`, `HPole_Para*` | pole / pillar tests | [O] | — |
| Gymnastics: pole swinging | implemented + works | IMPLEMENTED | `HPole_swing*`, flings | `TrapRoomsPoleSwingStops` | [O] | — |
| Gymnastics: balance beam movement | partial | IMPLEMENTED | `bbeam_*` (18 clips) | beam tests | [O] | sprint / walkover / drop clips unused |
| Gymnastics: gymnastic transitions | partial | PARTIALLY IMPLEMENTED | `hurdle_*`, `bbeam_ledgeu_climbup`, `bbeam_flip_foward` | `JunglehubVaultOver`, `ChapelHangClimbsOntoBalanceBeam` | [O] | handstand, pole-top run |
| Gymnastics: acrobatic movements | missing | NOT IMPLEMENTED | — | — | [U] | — |
| Grapple: firing | placeholder | PARTIALLY IMPLEMENTED | `grapple_rappel_free_jumpon` stands in for the throw | — | [I] | find the throw clip / g_grapple animations |
| Grapple: attachment | partial | IMPLEMENTED BUT UNVERIFIED | same | — | [I] | — |
| Grapple: hanging from cable | implemented + works | IMPLEMENTED | `grapple_rappel_idle` | `GrappleSwingAndRelease` | [O clip] | — |
| Grapple: swinging | implemented + works | IMPLEMENTED | `grapple_rappel_free_swing_forward / _backward` | same | [O clip] | chosen by velocity sign [I] |
| Grapple: climbing | implemented + works | IMPLEMENTED | `_free_ascend / _descend` | `KrakenRoomRappel` | [O] | — |
| Grapple: rappelling | implemented + works | IMPLEMENTED | `grapple_rappel_up / down(_left / _right_45)` | same | [O] | — |
| Grapple: wall running | implemented + works | IMPLEMENTED | `grapple_rappel_(walk_)left / right`, `ng_ground_to_wallrun_*` | `ValaskjalfWallRun` | [O clips], stick thresholds [I] | — |
| Grapple: jumping away | partial | IMPLEMENTED | `grapple_rappel_free_jumpoff` | `GrappleSwingAndRelease` | [O clip] | no wall-run leap clips |
| Swimming: surface swimming | implemented + works | IMPLEMENTED | `swim_surface_fast_forward`, `_sprint_forward`, `swim_surfaceidle` | swim tests | pose fit vs tru.exe [O] | 3-way surface blend (request 881) reduced to one clip |
| Swimming: underwater swimming | implemented + works | IMPLEMENTED | `swim_sub`, `swim_submerged_sprint / _idle` | same | [O] | `swim_sub_up / _down` unused by design (model pitched) |
| Swimming: diving | implemented + works | IMPLEMENTED | `swim_surfacedive` | `DiveSwimDownAndSurface` | [O] | — |
| Swimming: surfacing | partial | IMPLEMENTED | `swim_sub_up` loop | same | [I use] | — |
| Swimming: turning | missing | NOT IMPLEMENTED | — | — | [U] | surface turn clips (GAMEPLAY.md:2532) |
| Swimming: water entry | implemented + works | IMPLEMENTED | `swim_jump_land`, `swim_land_wading` | `WaterEntrySplashMakesRingsAndDrops` | [O clips] | — |
| Swimming: water exit | implemented + works | IMPLEMENTED | `swim_climbout`, `swim_to_wade` | `SwimEnterCrossClimbOut` | [O clips] | — |
| Combat: targeting / drawing / firing / switching / aiming | implemented + works | IMPLEMENTED (part B) | Combat.cpp aim grid, equip / fire / reload clips per weapon | `Combat.*` | [O] | audited in part B |
| Combat: evasive movements | missing | NOT IMPLEMENTED | — | — | [U] | — |
| Combat: melee attacks | missing | NOT IMPLEMENTED | (Mjolnir ground pound only) | `Combat.MjolnirCycleSwingAndGroundPound` | — | kicks, stomp, brush off |
| Combat: enemy vaulting | missing | NOT IMPLEMENTED | — | — | [U] | — |
| Combat: adrenaline headshots | partial | PARTIALLY IMPLEMENTED | Combat.cpp:1296 (no vault clip) | `Combat.AdrenalineHeadShot` | tuning [O], window [I] | — |
| Combat: focused fire | partial | PARTIALLY IMPLEMENTED | adrenaline mode time scale | `Combat.AdrenalineModeDrainsAndSlowsTime` | [O tuning] | — |
| Combat: knockdown recovery | missing | NOT IMPLEMENTED | — | — | [U] | `Hit_Thrown*` unused (GAMEPLAY.md:2552) |
| Interaction: picking up | implemented + works | IMPLEMENTED | `Player::use`, `gear_weapon_pickup*` | `LaraPlaysInteractionClips` | [O] | part B |
| Interaction: carrying | partial | unknown (part B) | `World::carryPortables` | part B | — | — |
| Interaction: pushing / pulling | implemented + works | IMPLEMENTED | `obj_block_*` | `PushBlockIndependentOfRenderRate` | [O] | — |
| Interaction: throwing | missing | NOT IMPLEMENTED (object throw) | grenade throw only | — | — | part B to confirm |
| Interaction: lever / switch activation | implemented + works | IMPLEMENTED | `useSwitchClip` | `LaraPlaysInteractionClips`, `BallcourtLeverDrivesDoorJoint` | [O] | — |
| Interaction: grapple interaction | implemented + works | IMPLEMENTED | `grapple_pull_*_light(_begin)` | `MountedGrapplePullsBlockWithoutSwinging` | [O clips] | — |
| Motorcycle: mount / dismount / riding / steering / braking / leaning / riding clips / shooting | implemented + works | IMPLEMENTED (part B) | Vehicle.cpp `motorcycle:*` clips | `tests/test_vehicle.cpp` | tuning record [O] | part B |

### Animation synchronization

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| State-driven animation selection | needs refactor | IMPLEMENTED | `Player::play` calls inside each state; many sub-states are encoded as comparisons of `s_.clip` strings | all | hand port of chart fragments | The clip name is the sub-state (e.g. P.cpp:1569-1571, 2213, T.cpp:305-308): fragile, blocks a data-driven chart |
| Correct transitions | partial | PARTIALLY IMPLEMENTED | as above | `TransitionsReducePops` | [O]/[I] mixed | — |
| Animation blending | implemented + works | IMPLEMENTED | `anim::AnimController` (linear crossfade as the request's interp frames, additive overlays, blend-space mix, mirror) | `Anim.*`, `TransitionsReducePops` | [O] | — |
| Animation interruption | partial | IMPLEMENTED BUT UNVERIFIED | new `play()` fades over the old layers | — | [I] | Per-clip interrupt windows not decoded |
| Root-motion handling | partial | IMPLEMENTED | `advanceClip` / `advanceRoot`, `warpStep` (clip carries her to a target), `rootLeft` | `RootTranslationUnwrapsLoops`, `LaraClipsAndRootMotion` | [O] | Flights are ballistic while the clip plays in place (GAMEPLAY.md:2543) |
| Inverse kinematics | missing | NOT IMPLEMENTED | none in `src/anim`, `World::updatePose` (W.cpp:1212) | — | [U] | tru.exe anim targets (e.g. target 0x273 named in Game.hpp:259) and any IK nodes of the anim graph not decoded |
| Foot placement | missing | NOT IMPLEMENTED | — | — | [U] | Stairs, slopes, beams, walls: feet follow the clip only |
| Hand placement | partial | PARTIALLY IMPLEMENTED | root positioned so the pose's hands meet the ledge / pole (`placeOnLedge`, `attachPose`, `handsAt` for climb-up, climb-out, beam) | `LedgeGrabHangShimmyDrop` ("hand" check per GAMEPLAY.md Tests) | hand offsets from poses [O] | Whole-body offset, not per-hand; uneven ledges / moving clips not corrected |
| Contact synchronization | partial | PARTIALLY IMPLEMENTED | `warpStep`, hurdle draw-on (`Hurdle::drawOff`) | `PoolRockGrabNotHurdle`, `CliffclimbObstaclesAndLedgeGrabs` | some [O Wine], eases [I] | — |
| Body orientation | partial | IMPLEMENTED | yaw; swim pitch; bike roll (`Player::modelMatrix`) | `UnderwaterModelFollowsSwimPitch` | [I] | No lean on slopes / in run turns |
| Correct physical alignment | partial | IMPLEMENTED BUT UNVERIFIED | capsule only on the ground / in the air; climb, hang, ClimbUp, DropToHang move without collision (P.cpp:2096, 2295) | — | capsule [O Wine] | No check that the pose stays inside free space in those states |
| Movement recovery | partial | IMPLEMENTED BUT UNVERIFIED | land / stop / `_to_idle` clips | — | [I] | — |

## §13 Camera and input

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Third-person follow camera | implemented + works | VERIFIED AGAINST ORIGINAL | W.cpp:731 `updateOrbitCamera` (port of LaraCameraRewrite 0x602690) | `CameraRestAndPitchReturn`, `CameraSwingsBehindRunningLara`, `CameraForcesIndependentOfRenderRate` | camera DTP 0x14, force sets [O]; Wine traces | Level camera records (cmd 0x12), look-target blend, moving platforms [U] (W.cpp:729) |
| Independent camera rotation | implemented + works | VERIFIED AGAINST ORIGINAL | look input; turn-about-pivot 0x600710 | `StreamReal.YachtCameraTurnsRoundTheCabin` (GAMEPLAY.md:2394) | [O Wine] | — |
| Camera recentering | works but differs from original | IMPLEMENTED BUT UNVERIFIED | W.cpp:559 (snap behind her at the rest pitch) | `RecenterTurnsCameraBehindLara`, `SharedGrappleKeyRecentersWithoutTarget` | binding [O]; motion [I] | Original motion (snap or swing) not traced |
| Camera collision | implemented + works | IMPLEMENTED | collider radius 13.91, `collision::kCameraMask`, pivot clearance 0x5EA050 | `CliffclimbCameraRecord` | [O] | — |
| Environmental obstruction | implemented + works | IMPLEMENTED | pull-in 0x5F0810 / swing | same | [O] | — |
| Camera distance | implemented + works | VERIFIED AGAINST ORIGINAL | `Mode::distance` 1264.22; hang / ladder 1896.3 | `CameraRestAndPitchReturn` | [O] | — |
| Camera positioning | implemented + works | VERIFIED AGAINST ORIGINAL | focus records, dead zones, ease curve | same | [O] | — |
| Camera sensitivity | partial | IMPLEMENTED BUT UNVERIFIED | `Controls::mouseScale` x 0.0025 rad / pixel; stick 2.5 rad/s | `ControlBindings`, `SticksAndMenuNavigationRebind` | [I] | Original mouse / stick rates and the slider curve [I] |
| Camera behavior during climbing | implemented + works | VERIFIED AGAINST ORIGINAL (free climb, hang) | modes 4 / 11 [O Wine hook 0x5E8210]; ladder 10, perch 16, pillar 17, beam 3, pole 9 from the markup-type table | `CameraHangYawForceFollowsShimmy`, `CliffclimbCameraRecord`, `CameraLadderPitchesUpAfterClimbingUp` | observed [O] for 4 / 11; the others [I] (W.cpp:740) | — |
| Camera behavior during jumping | implemented + works | VERIFIED AGAINST ORIGINAL | mode 1 (observed for jumping and falling) | `CameraRestAndPitchReturn` | [O Wine] | — |
| Grapple camera | missing | NOT IMPLEMENTED | grapple states fall to the on-foot mode 1 (W.cpp:741 "[U]") | — | [U] | Hook 0x5E8210 while swinging / rappelling / wall running to read the mode |
| Swimming camera | implemented + works | VERIFIED AGAINST ORIGINAL | modes 20 / 19 | `CameraSwingsBehindSwimmingLara`, `SeaCircleCameraLevelsOut` | [O Wine] | — |
| Combat camera | missing | NOT IMPLEMENTED | no lock-on or manual-aim mode in `updateOrbitCamera` (no combat term in W.cpp:731-1129) | — | [U] | Target-lock framing and the manual-aim view |
| Motorcycle camera | placeholder | IMPLEMENTED BUT UNVERIFIED | W.cpp:845 distance 1711 [I], 1002 swing behind [I] | `tests/test_vehicle.cpp` (unknown which asserts the camera) | [I] | Original VehicleCamera not decoded |
| Cooperation: normal movement | implemented + works | IMPLEMENTED | camera-relative stick W.cpp:584 | replays | [O] | — |
| Cooperation: jumping | implemented + works | IMPLEMENTED | take-off heading = stick (P.cpp:2110) | `JumpTakeOffsMatchTruTraces` | [O] | — |
| Cooperation: climbing | implemented + works | IMPLEMENTED | hang: stick vs her right / forward; free climb: raw stick | `HangJumpsUpRightAndBack`, `CliffclimbStances` | chart quadrants [O] | — |
| Cooperation: grapple swinging | partial | IMPLEMENTED BUT UNVERIFIED | world-direction stick force | — | [I] | see §6 |
| Cooperation: wall running | partial | IMPLEMENTED BUT UNVERIFIED | stick projected on the wall's right / in | `ValaskjalfWallRun` | [I] | — |
| Cooperation: underwater movement | partial | IMPLEMENTED BUT UNVERIFIED | yaw from the stick, pitch from keys | `UnderwaterModelFollowsSwimPitch` | [I] | camera pitch not used |
| Cooperation: combat | partial | unknown (part B) | free aim = camera ray | `Combat.LockOnSelectionAndSwitch` | — | — |
| Cooperation: motorcycle riding | works but differs from original | IMPLEMENTED BUT UNVERIFIED | raw stick (not camera-relative), camera swings behind | — | [I] | — |
| Camera must not hide traversal surfaces | partial | IMPLEMENTED BUT UNVERIFIED | markup camera hints (`orbit_.hint`, W.cpp:926-981), per-mode yaw limits | `CliffclimbCameraRecord` | records [O] | No test per traversal spot |
| Input: bindings, pads, rebinding, replays | implemented + works | IMPLEMENTED | `Input.cpp`, `Config.cpp:141-175` | `ControlBindings`, `SticksAndMenuNavigationRebind`, `ReplayedDeviceSessions`, `InputScriptParse` | defaults [O 0x487710] | Manual Aim action absent (Z = `switch`) |

## §15 State machine

### The actual states (`Game.hpp:133` `enum class State`)

`Idle, Walk, Run, Stop, Turn, JumpPrep, Jump, Fall, Land, Hang, Shimmy, ClimbUp, SwimSplash, SwimTread, SwimSurface,
SwimDive, SwimUnder, Surfacing, SwimClimbOut, Dead, Crouch, Roll, Slide, Beam, DropToHang, HangJump, PoleHang, Climb,
Ladder, Perch, Pillar, GrappleThrow, GrappleSwing, Hit, RailSlide, GrapplePull, Vehicle, GrappleRappel, Use, Block,
WallHug` (41). One `switch` in `Player::tick` (P.cpp:1910) dispatches to per-state code or a member function.
Combat is not a player state: it is a parallel layer in `World::tickCombat` (`CombatState`: drawn, locked, slowMo,
adrenalineMode) blended onto the pose by `aimLayer`. The bike is `State::Vehicle` with its own `BikePhase` / `BikeStunt`.
Sprint is the clip `sprint_loop` inside `Run`; pole top is `Perch` with a pole as `ledge`; wall run is the sideways
branch of `GrappleRappel`.

### Goal example states mapped

| Requirement (goal state) | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Idle | implemented + works | IMPLEMENTED | `State::Idle` | see §5 | — | — |
| Walking | implemented + works | IMPLEMENTED | `State::Walk` | — | — | — |
| Running | implemented + works | IMPLEMENTED | `State::Run` (+ `Stop`, `Turn`) | — | — | — |
| Sprinting | needs refactor | IMPLEMENTED | `Run` + clip `sprint_loop` | `SprintStaminaMatchesTru` | — | not a state; tested through clip names |
| Crouching | implemented + works | IMPLEMENTED | `State::Crouch` | — | — | — |
| Rolling | implemented + works | IMPLEMENTED | `State::Roll` | — | — | — |
| Jumping | implemented + works | IMPLEMENTED | `JumpPrep`, `Jump`, `HangJump` | — | — | — |
| Falling | implemented + works | IMPLEMENTED | `State::Fall`, `Slide` | — | — | — |
| Landing | implemented + works | IMPLEMENTED | `State::Land` | — | — | — |
| Hanging | implemented + works | IMPLEMENTED | `State::Hang`, `DropToHang` | — | — | saving grab = `Hang` + timer |
| Shimmying | implemented + works | IMPLEMENTED | `State::Shimmy` | — | — | corners live in `Hang` by clip name |
| Climbing | implemented + works | IMPLEMENTED | `State::Climb`, `ClimbUp` | — | — | — |
| Ladder climbing | implemented + works | IMPLEMENTED | `State::Ladder` | — | — | — |
| Pole climbing | implemented + works | IMPLEMENTED | `State::Pillar`, `PoleHang` (parallel) | — | — | — |
| Pole swinging | needs refactor | IMPLEMENTED | `PoleHang` + swing clips | — | — | sub-state by clip |
| Balance beam | implemented + works | IMPLEMENTED | `State::Beam` (+ `Balance` struct) | — | — | — |
| Chimney jumping | missing | NOT IMPLEMENTED | — | — | — | in progress (GpChimney) |
| Grappling | implemented + works | IMPLEMENTED | `GrappleThrow`, `GrapplePull` | — | — | — |
| Grapple swinging | partial | PARTIALLY IMPLEMENTED | `GrappleSwing` | — | — | also holds "standing with the rope" and "climbing the rope" |
| Grapple climbing | needs refactor | PARTIALLY IMPLEMENTED | inside `GrappleSwing` (interact branch) | — | — | no own state |
| Rappelling | implemented + works | IMPLEMENTED | `GrappleRappel` | — | — | — |
| Wall running | needs refactor | PARTIALLY IMPLEMENTED | inside `GrappleRappel` | — | — | no own state |
| Swimming | implemented + works | IMPLEMENTED | `SwimTread`, `SwimSurface`, `SwimUnder`, `Surfacing`, `SwimSplash`, `SwimClimbOut` | — | RTTI names quoted [O] | tru.exe has one SwimState; ours splits it |
| Diving | implemented + works | IMPLEMENTED | `SwimDive` | — | — | swan dive missing |
| Underwater combat | partial | unknown (part B) | combat layer while `water >= 0` | part B | — | — |
| Ground combat | partial | IMPLEMENTED (part B) | combat layer | `Combat.*` | — | not a player state |
| Aiming | partial | PARTIALLY IMPLEMENTED | `CombatState::locked`, free aim | — | — | manual aim missing |
| Firing | implemented + works | IMPLEMENTED (part B) | combat layer | `Combat.PistolsLockFireAndHit` | — | — |
| Melee attack | missing | NOT IMPLEMENTED | — | — | — | — |
| Adrenaline headshot | partial | PARTIALLY IMPLEMENTED | `CombatState::slowMo` | `Combat.AdrenalineHeadShot` | — | no vault; input differs |
| Focused fire | partial | PARTIALLY IMPLEMENTED | `CombatState::adrenalineMode` | `Combat.AdrenalineModeDrainsAndSlowsTime` | — | input differs |
| Object interaction | implemented + works | IMPLEMENTED | `State::Use` | `LaraPlaysInteractionClips` | — | — |
| Pushing and pulling | implemented + works | IMPLEMENTED | `State::Block` | `PushBlockIndependentOfRenderRate` | — | — |
| Motorcycle riding | implemented + works | IMPLEMENTED (part B) | `State::Vehicle`, `BikePhase::Ride` | `tests/test_vehicle.cpp` | — | — |
| Motorcycle jumping | partial | IMPLEMENTED BUT UNVERIFIED (part B) | Jolt simulation + air clips | part B | — | no explicit state |
| Motorcycle landing | partial | IMPLEMENTED BUT UNVERIFIED (part B) | same | part B | — | — |
| Death | implemented + works | IMPLEMENTED | `State::Dead`, ragdoll | `LaraRagdollIndependentOfRenderRate`, `Combat.FallDamageDeathAndRespawn` | — | — |
| Checkpoint recovery | implemented + works | IMPLEMENTED | World respawn (Combat.cpp / Stream.cpp), `Player::spawn` | `Combat.FallDamageDeathAndRespawn`, `Save.*` | 0x5D40C0 [O] | not a player state |

### Transition handling

| Requirement | Classification | Status | Code | Tests | Provenance | What is missing or wrong |
|---|---|---|---|---|---|---|
| Explicit transition conditions | needs refactor | IMPLEMENTED | inline `if` chains per state in `Player::tick` and the member functions | all | chart ids in comments | No transition table; conditions cannot be listed or tested as data |
| State interruption | partial | IMPLEMENTED | `hitReact` (gated to on-foot / swim states in `World::damageLara`, Combat.cpp:664), `die`, `use`, `attachGrapple`, `enterWater` force a state | `Combat.HitReactionPlaysAndReturns` | — | No hit reactions while hanging / climbing / crouching (damage only); `attachGrapple` is not gated by the state it leaves (W.cpp:549: throws from any state; original's rule [U]) |
| Animation completion | implemented + works | IMPLEMENTED | `clipDone()` | — | — | — |
| Collision interruption | partial | IMPLEMENTED | ground loss -> `startFall`; rappel wall loss -> swing; ClimbUp end without floor -> fall | `RunsOverSteepSliverWithoutFalling`, `WalkingSlowlyOffEdgeFallsOnce` | — | Root-motion-only states ignore collision |
| Input timing | partial | PARTIALLY IMPLEMENTED | press edges for jump / drop only (`jumpHeld_`, `dropHeld_`); no input buffer; no hold detection | `ReplayedDeviceSessions` | — | Needed by: chimney rhythm, climb faster, handstand hold, jump hold, saving grab |
| Invalid interactions | partial | IMPLEMENTED | `canInteract()`; jump on a hang with nothing to do stays hanging | `BallcourtLeverIgnoresAUse` | — | — |
| Falling | implemented + works | IMPLEMENTED | `startFall` from 20+ sites | — | — | — |
| Landing | implemented + works | IMPLEMENTED | `land()` (beam, wading, run-on, generic) | — | — | — |
| Death | implemented + works | IMPLEMENTED | `Player::die` | — | — | — |
| Recovery | partial | IMPLEMENTED BUT UNVERIFIED | Land / Stop / Hit tails | — | — | knock-down recovery missing |
| Movement transitions | partial | IMPLEMENTED | see §5 | — | — | — |
| Long chain: run -> sprint -> jump -> grapple -> swing -> release -> air -> ledge grab -> shimmy -> pull up -> run, tested | partial | PARTIALLY IMPLEMENTED | links exist except air control and a sprint jump | no chain test | — | Add once the grapple jump-in defect is fixed |

Missing states / transitions (summary): chimney jump; sprint jump; swan dive; safety drop (ground -> hang without a
key); handstand pull-up; pole-top run; beam sprint / walkover / drop-to-hang; rope pivot; wall-run leap with release;
evasive roll (land and water); melee, jump kick, enemy vault, knock-down and recovery; manual aim; free hang (`ledgeu`).

## Animation (summary of what exists against §12)

Exists: full clip / skeleton decode; per-state clip selection with the original requests' crossfade and start frames;
blend spaces for hurdles and the beam lean; mirrored requests; additive hit overlays; aim-grid upper-body layer
(`aimLayer`) and throw layer; root motion with loop unwrapping, root yaw, and target warps; Verlet hair; death ragdoll.
Absent: inverse kinematics of any kind; foot placement on stairs / slopes / beams / walls; per-hand placement;
slope / turn leaning; clip events as gameplay timing; the state chart as data; 1166 of 1744 clips unused (acrobatics,
melee, evasive moves, knock-downs, swim turns, handstand, beam extras among them [I: by the missing features]).

## Camera and input (summary)

Present: orbit follow camera ported from tru.exe with per-mode records for on-foot (1), free climb (4), ledge hang (11),
ladder (10), perch (16), pillar (17), beam (3), pole (9), under water (19), surface (20); scripted spline cameras; field
camera (eye view, [I]); bike camera (distance and swing [I]). Absent: grapple / rappel / wall-run mode ([U], uses 1),
combat lock-on framing, manual-aim camera (and the Manual Aim action), level camera records (command 0x12), the
default-yaw look-target blend, moving-platform carry. Input: every original PC default is bound as decoded except
Manual Aim (Z drives `switch`); differences in what the keys do are listed in `controls.md`.

## Docs vs code disagreements found

- GAMEPLAY.md "Deviations" (l.2535-2558) still lists as missing: wall-running / rappel, pulling mounted objects, rope vs
  world, the hook model, pole-to-perch (`hpole_perch_*`), rigid hair, assault rifle / tranquilizer / Mjolnir. The code
  (G.cpp, P.cpp:1774, anim::HairSim, Combat) and ROADMAP rows have them. The section is stale.
- GAMEPLAY.md l.541 "no bike shooting yet" vs ROADMAP "Motorcycle" row and Combat.cpp:1196 (pistols / uzi on the bike).
- ROADMAP "Planned: Jolt" step 2 "Not done: wheelies / power slides / donuts, the lean" vs Vehicle.cpp:329-378
  (burnout, donut, power slide, lean present).
- ROADMAP rows use "Tested" for areas whose numbers are [I] (grapple swing, rappel); under this audit's vocabulary
  those are `IMPLEMENTED BUT UNVERIFIED`.

## Prioritized worklist

Each item: the gap, where to change the code, where the truth is.

- **P1 Chimney / wall jump** (missing, goal calls it out). Code: new state beside `Jump` in `Player::tick` (P.cpp:2125),
  wall contact from `airMove` (P.cpp:449), clip names into `kLaraClipNames` (W.cpp:234). Truth: lara.drm state chart
  group for the wall jump + its jump query; `openunderworld-work/gameplaygoal/chimney/` (GpChimney's disassembly).
  Owner this wave: GpChimney.
- **P2 Grapple jump-in and leap-off are wrong.** (a) G.cpp:306: Jump while standing roped releases the rope instead of
  jumping into the gap; (b) G.cpp:176: Jump on the wall kicks into a swing and keeps the rope, ignoring the stick;
  (c) G.cpp:318: constant stick force cannot build swing height while Forward is held; (d) inputs G.cpp:183, 378-386
  (E + stick). Truth: PlayerSC_Grapple (0x73AB70 area), chart states S314 / S378 / S379 / S421, Wine trace of player
  +0x2A0 velocity on l4_pool's ring and l5_valaskjalf's wall run (`poleattach/*.sh` harness).
- **P3 Saving grab on the wrong key and an invented trigger.** P.cpp:519-548, 2178-2189; C.cpp:133, 269-283. Truth:
  the hang group's bad-attach condition in the chart; difficulty table generalbank +0x24 (+0x14 frames); Wine: far jump
  to a ledge with Interact pressed / not pressed.
- **P4 Jump control: hold-to-extend, sprint jump, air steering.** P.cpp:441 (`airMove`), 1922, 2109-2111;
  `Tuning` (Game.hpp:171-176). Truth: Wine velocity traces as `JumpTakeOffsMatchTruTraces` (hold vs tap, sprint_loop
  take-off, stick flipped mid-air); query table q-ids of the sprint take-off state.
- **P5 Climb faster with Interact** on ledge shimmy (P.cpp:2271), pole hand-over-hand (P.cpp:1681), pillar
  (T.cpp:441), ladder (T.cpp:423). Needs press timing (see P12). Truth: chart conditions on virtual button 0 in those
  groups; Wine timing of rung clips with / without E.
- **P6 Safety drop without a key** when stepping off a grabbable edge. P.cpp:381 `groundMove` -> `startFall`, reuse
  `tryDropToHang` (P.cpp:755). Truth: Wine: walk and run off l3_main_bedroom ledges; chart transitions into
  `drop_to_ledgeb`.
- **P7 Swimming inputs and moves.** Climb out on Forward (P.cpp:2782), camera-pitch steering (P.cpp:2816), swan dive
  (new: Jump then Crouch in `Jump`, P.cpp:2125), underwater evasive roll, surface turn clips, air numbers
  (`World::tickAir`, [I] 45 s). Truth: SwimState handlers (RTTI names in Game.hpp:135), Wine in l2_cliffclimb sea.
- **P8 Balance beam extras.** Sprint, walkover, drop to hang (`bbeam_drop_*`), beam jump transitions. P.cpp:2367-2380,
  2414. Truth: chart S934 / S897 neighbours, T1540 / T1541; walk-balance tuning objdata +0xE98.
- **P9 Pole-top run and phase-dependent fling.** P.cpp:1774 (`hpole_perch_to_top_run`), 1591-1595. Truth: chart
  around S668 / S688 / S689; Wine l4_traprooms 1336.
- **P10 Handstand pull-up (Jump held on a hang) and free hang (`ledgeu_*`).** P.cpp:2215-2233. Truth: hang group
  S439 / S530 transitions; clip names in lara.drm's animation list.
- **P11 Manual aim + combat camera + grapple camera mode.** Config.cpp:163 (Z), Combat.cpp:1230-1250, W.cpp:752-766.
  Truth: hook CameraManager 0x5E8210 while aiming / locked / swinging (as done for modes 1 / 4 / 11 / 19 / 20); camera
  DTP 0x14 mode list.
- **P12 Input timing layer.** `Player::tick` has press edges for jump / drop only (P.cpp:1896). Add hold time and a
  short buffer for jump / interact; prerequisite of P1, P3, P4, P5, P10.
- **P13 IK and contact placement.** After `anim_.pose()` in `World::updatePose` (W.cpp:1234): hands to the ledge /
  pole / rope, feet to stairs / beams / walls. Truth: tru.exe anim-graph IK / anim-target nodes (target 0x273 is the
  only one named so far); `research/cdcEngineDXHR` anim node classes.
- **P14 State machine as data.** Replace clip-string sub-states (P.cpp:1569, 2213; T.cpp:305, 851; G.cpp:23) with
  explicit sub-states or the decoded chart; give sprint, rope climb, wall run, pole swing their own states; gate
  `attachGrapple` (W.cpp:549) by the state it leaves. Truth: lara.drm state chart
  (S / T ids already quoted throughout the code).
- **P15 Remaining [I] numbers on verified-looking features**: turn rate 6 rad/s (Game.hpp:177), air-grab reach
  (Game.hpp:178), `assistRadius`, swim drag and radius, rappel / rope speeds 300, wall-run 700 / 2500, grapple pull
  heave, recenter motion, bike camera 1711. Each needs one Wine trace.
- **P16 Ladder manual re-grab** (T.cpp:376, P.cpp:508): require Interact after the Crouch release.
- **P17 Tests for chains and missing assertions**: camera-relative heading; drop to a lower handhold; swing amplitude
  with Forward held; the two long chains of §5 / §15; a narrow underwater passage.
- **P18 Stale docs** (section above): GAMEPLAY.md "Deviations", l.541; ROADMAP Jolt step 2.
