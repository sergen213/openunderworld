# Controls and gameplay rules: original (Windows PC) vs OpenUnderworld 0.41.0

Reference for the original column: Stella's controls and gameplay page, Windows PC column only
(https://tombraiders.net/stella/walks/controls/TR8controls.html), restated in our own words; the default key table is
also decoded from tru.exe 0x487710 (docs/GAMEPLAY.md "Controls" [O]). Our column was read from the code at main
4437cb5 (`src/game/*.cpp`, `src/config/Config.cpp:141-175`). Nothing was built or run for this audit: "passing" means
the test exists in the tree whose full suite the release notes report green (675 tests, v0.41.0).

Status vocabulary: `NOT IMPLEMENTED`, `PARTIALLY IMPLEMENTED`, `IMPLEMENTED BUT UNVERIFIED`, `IMPLEMENTED`,
`VERIFIED AGAINST ORIGINAL`. Test names are in `tests/test_game.cpp` (suites `Game` synthetic / `GameReal` real data),
`tests/test_combat.cpp`, `tests/test_vehicle.cpp` unless a file is given.

## Basic moves

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes (differences from the original found) |
|---|---|---|---|---|---|
| Run forward / left / right / back | W A S D; direction is relative to the camera | `forward/back/left/right` -> `Actions::move`; `World::tick` rotates it by `cam_.yaw` (World.cpp:584); `Player::tick` Idle/Run | VERIFIED AGAINST ORIGINAL | `StairsWalkAndRunUpAndDown`, `PoolRunReplayStaysOnFloor`; run speed is the clip's root motion; GAMEPLAY.md "Numbers" | Heading turns at a fixed 6 rad/s (`Tuning::turnRate`, no provenance tag = [I]); the original's run-turn clips are not used |
| Walk | Left Alt held + direction | `walk` (Left Alt); also stick < 0.5 (`wantsWalk`, Player.cpp:1908) | IMPLEMENTED | `ControlBindings`; `ng_walk` root motion | Walk off edges: she steps off (matches). No walk start / stop clips |
| Sprint | Left Shift held + direction; tires after a while, then must run to recover | `sprint`; `sprint_loop`, `ng_sprint_to_exhaust`, `ng_run_exhaust`; `PlayerState::sprintUsed` | VERIFIED AGAINST ORIGINAL | `SprintStaminaMatchesTru` (Wine l4_trailstart 9.5..9.9 s, resume 0.9 s) | No sprint on stairs / slopes clips (`ng_sprint_up/_down`), no sprint jump (see below) |
| Pivot on a post | Direction keys while perched on a post / narrow column | `Player::perch` turning (Traverse.cpp:606) | IMPLEMENTED | `TemplePerchTurnsAndPillarTop`, `PerchAttachAndJumpOff` | Stick bands [I] |
| Sidestep on a narrow ledge | Direction keys with her back to the wall | `State::WallHug`, `Player::wallHug` | IMPLEMENTED | `PuzzleRoomWallHug`, `WallHugRoundsConcaveAndConvexCorners` | Shelf detection from tru.exe 0x74BB50 [O]; depth / carry [I] |
| Jump (straight up) | Space; grabs automatically what is in reach | `jump` -> `State::JumpPrep` -> `Jump`; `aimGroundJump` q2, `standJumpToLedge` | VERIFIED AGAINST ORIGINAL | `JumpTakeOffsMatchTruTraces`, `BedroomJumpsMatchTruTraces`, `StandingJumpCatchesHighLedgeWithinReach` | Take-off 48.04 / frame [O Wine] |
| Interact / pick up | E | `interact`; `World::collectPickups`, `carryPortables`, `script_.tick`, `Player::use` | IMPLEMENTED | `LaraPlaysInteractionClips`, `BallcourtLeverDrivesDoorJoint` (part B audits interactions) | E is NOT wired to: faster climbing, saving grab, melee (rows below) |
| Drop held object | Left Ctrl | `World::carryPortables` (Objects.cpp) | unknown in part A scope | part B | Not checked here |
| Throw held object | Caps Lock | `grenade` action throws grenades only (Gadgets.cpp) | NOT IMPLEMENTED (object throw) | no `throw` path for carried portables found by grep in Player/World tick; part B to confirm | Original shares the grenade key |
| Manual grab | Does not exist, except re-grabbing a ladder lower down: Ctrl to let go, then E quickly | Ladder: `drop` -> `ladder_detach_drop` + `regrabCooldown 0.3`, then `tryLadder(true)` re-attaches by itself in `airMove` | PARTIALLY IMPLEMENTED | Traverse.cpp:376-381, Player.cpp:508; no test | Ours re-grabs automatically after 0.3 s if still facing the ladder; the original needs the E press (and falls without it) |
| Saving grab | E pressed quickly when she catches an edge with one hand (warning icon), else she falls; window depends on difficulty | `tryGrab` sets `savingGrab` when falling faster than `grabMinVz` (up to 2x); `ledgeb_bad_attach_M`; recover in `Player::tick` Hang (Player.cpp:2178) | PARTIALLY IMPLEMENTED | `ClimbBadAttach` (free climb variant); timer from the difficulty table (`DifficultyTable::savingGrabSeconds`, frames 90/50/15 [I: TR7 meaning]) | INCORRECT INPUT: ours recovers on **Jump**, the original on **Interact**. Trigger rule (fall speed > grab window) is [I]; original triggers on far / odd-angle jumps. No warning icon check here (HUD: part B) |
| Crouch / duck; creep | Left Ctrl held; with direction keys she creeps | `drop` held -> `State::Crouch`, `Crouch_Front` | IMPLEMENTED | `CrouchUnderSlabAndRoll` | Headroom check keeps her down under slabs |
| Let go and drop (hanging) | Left Ctrl once; grabs a handhold below automatically | Hang: `dropPressed` -> `startFall`, `regrabCooldown 0.4`; `tryGrab` on the way down | IMPLEMENTED | `LedgeGrabHangShimmyDrop` | Regrab below works only after 0.4 s and inside the grab speed window (else saving grab) [I] |
| Recenter camera / rotate camera | Q (patch 1.1; shares the grapple key) / mouse | `recenter` (Q); look = mouse / right stick; World.cpp:559 | IMPLEMENTED BUT UNVERIFIED | `RecenterTurnsCameraBehindLara`, `SharedGrappleKeyThrowsWithoutRecentering`, `SharedQThrowsTheGrappleNotTheCamera` | Binding [O]; the snap behaviour itself is [I] (original's recenter motion not traced) |

## Lara's gear

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes |
|---|---|---|---|---|---|
| Use health pack | V | `heal`; Combat.cpp | IMPLEMENTED | `Combat.HealthPacks` | part B |
| Grapple | Q | `grapple`; `World::grappleTarget`, `Player::attachGrapple` | IMPLEMENTED | see grapple rows | |
| Utility light | F toggles; fixed to her body | `light`; Gadgets.cpp `tickGadgets` | IMPLEMENTED | `GadgetsReal.TuningLightsAndSpotUnits` (ROADMAP) | part B |
| Field camera | T toggles; fire / target-lock keys zoom in / out | `camera` (T); `World::updateCamera` field view; zoom on **forward / back** | PARTIALLY IMPLEMENTED | `FieldCameraTogglesEyeView` | Zoom keys differ (original: Left Mouse / H and Right Mouse / G); zoom range, eye view and its clips [I] (GAMEPLAY.md:505-510) |
| PDA | Tab | `pda`; `src/ui` | IMPLEMENTED | `Menus.*` (ROADMAP) | part B |

## Combination moves

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes |
|---|---|---|---|---|---|
| Push / pull object | E to grip, direction keys to move, E or Ctrl to let go | `Player::grip`, `State::Block`, script blocks | IMPLEMENTED | `PushBlockIndependentOfRenderRate`, `LaraPlaysInteractionClips` | part B |
| Directional / running jump | Direction + Space; from a run it goes farther; holding Space keeps her in the air a little longer | `JumpPrep` with `runJump`; q43 / q26 velocities | VERIFIED AGAINST ORIGINAL (take-offs) / NOT IMPLEMENTED (hold) | `JumpTakeOffsMatchTruTraces`, `RunningJumpReachAndRush`, `JunglehubRunningJumpReachesHigherLedge` | **Holding Jump has no effect**: `airMove` never reads `Actions::jump` (Player.cpp:441) |
| Sprinting jump | Shift + direction + Space: extra-long jump | Same branch as the running jump (`State::Run` whatever the clip) | NOT IMPLEMENTED | Player.cpp:1922 uses `runJumpSpeed` for `sprint_loop` too; GAMEPLAY.md:2548 lists it missing | Needs its own take-off numbers (Wine trace) and clip |
| Safety drop | Running / stepping off a grabbable edge: she turns and hangs by herself | Only on **drop pressed** at an edge (`tryDropToHang`); walking off = `startFall` | PARTIALLY IMPLEMENTED | `DropToHangFromEdge`, `PoolDropToHang`; `RunsOffEdgeFallsAndLands` shows the plain fall | Original (per the controls page) grabs with no key when stepping off; ours needs Ctrl. Verify in tru.exe (walk vs run) before changing |
| Shimmy | Direction keys while hanging | `State::Shimmy`, `ledgeb_right` (mirrored rate for left) | IMPLEMENTED | `LedgeGrabHangShimmyDrop`, `HangShimmyRoundCorners` | Left shimmy is the right clip played backwards (rate -1), not the mirrored clip |
| Vault / pull up; jump to a higher handhold | Space tap | Hang + jump: `hangClearance` class 2 -> `ledgeb_climbup_fast`; else `startHangJump(0)` | IMPLEMENTED | `HangJumpsUpRightAndBack`, `TempleLedgeJumpsUpAndBack`, `HangJumpRecordsReachAndClass` | Clearance classes from 0x74E290 [O] |
| Pull up into handstand | Space held while hanging | none | NOT IMPLEMENTED | no handstand clip in `kLaraClipNames` (578 of lara.drm's 1744 clips loaded); grep "handstand" = 0 hits in src and docs | |
| Jump and grab while climbing | Lean with the direction key at the end of the handhold, Space | lean clips `ledgeb_idle_to_jump_*_antic`; `startHangJump(kind)`; free leap if nothing there | VERIFIED AGAINST ORIGINAL (velocities) | `HangLeapsMatchTruTraces`, `HangJumpLeans`, `PoolSideJump` | Which clip each lean state plays is [I: by name] |
| Chimney / wall jump | Jump at a wall with another behind her, Space again at each contact; gains height each time | none on main | NOT IMPLEMENTED | grep "chimney / wall_jump" = no code; GAMEPLAY.md mentions it once (l.250, a test note). Peer agent `GpChimney` is implementing it this wave | |
| Vertical poles | Grabs automatically; direction keys climb and turn round it; jump / drop as on ledges | `State::Pillar`, `Player::pillar` (markup 15) | IMPLEMENTED | `PillarAttachRoundUpSlideOff`, `PillarJumpsByStickAngle`, `TemplePerchTurnsAndPillarTop` | Slide speeds [O Wine]; climb speed from the clip |
| Horizontal poles: hand over hand, swing, leap at the bottom of the swing | Direction keys; Space while swinging | `State::PoleHang`, `Player::pole`, `poleFling` | VERIFIED AGAINST ORIGINAL (attach / fling timing) | `TrapRoomsPoleFling`, `MainHallPoleFlingLandsOnPlatform`, `TrapRoomsPoleParallelHandOverHandAndTurns`, `TrapRoomsPoleSwingStops` | Fling leaves a fixed delay after the press, not "at the lowest point"; the target pick is [I] |
| Pole top: pull up and squat, leap, walk along, drop back | Space while hanging still; Space + forward; direction keys; Ctrl | `Player::poleTop` (State::Perch on a pole) | PARTIALLY IMPLEMENTED | `TrapRoomsPoleTop` | **Walking along the top is not ported** (Player.cpp:1771 comment: `hpole_perch_to_top_run`) |
| Balance beams | Space pulls up; direction keys walk; Ctrl drops to a hang; Shift walks faster; Space leaps forward; Ctrl while walking = walkover; release the keys to regain balance | `State::Beam`, `Player::beam`, `beamAboutFall` | PARTIALLY IMPLEMENTED | `BalanceBeamWalkTurnStepOff`, `BeamOffBalanceCounterStickRecovers`, `TempleBalanceBeamWalkAndJump`, `ChapelHangClimbsOntoBalanceBeam` | Missing: **faster walk (Shift)**, **walkover (Ctrl)**; Ctrl makes her fall off to her right instead of dropping to a hang (Player.cpp:2375 [I]); recovery: ours needs the stick back within the tolerance (decoded controller), releasing holds the last heading (0x742770 +0x5C [O]) so "let go to recover" needs a Wine check |
| Climb faster | E tapped in rhythm on ledges, poles, ladders | none | NOT IMPLEMENTED | `Actions::interact` is read only in Grapple.cpp:378 among the traversal files | |
| Ladders | Up to get on; direction keys; E faster; Ctrl lets go; Space + direction leaps | `State::Ladder`, `Player::ladder`, `ladderLeap` | IMPLEMENTED | `LadderRungsClimbOffTop`, `LadderJumpUpToLedge`, `LadderSideJumpsReachAndBack`, `ArcticSeaLadderSideJumpOntoBeam`, `ShipSinkingHallLadder` | No faster climb; re-grab after Ctrl is automatic (see Manual grab) |
| Forward roll / somersault | Ctrl tapped while running | `State::Roll`, `ng_roll_front` / `ng_roll_front_sprinting` | IMPLEMENTED | `CrouchUnderSlabAndRoll` | |
| Other acrobatics | various key combinations (not needed to finish the game) | wall-hug somersault only | NOT IMPLEMENTED | — | Low priority; no list decoded from the state chart yet [U] |
| Grapple: throw, auto aim, walk with it, E pulls, Q again releases | Q; E; Q | `World::grappleTarget` (six decoded target sets), `attachGrapple`, `State::GrapplePull` (E held heaves), `releaseGrapple` | IMPLEMENTED | `PoolGrappleTargetAndSwing`, `ChapelRingFromTheColumnTop`, `MainHallRingByItsOwnRange`, `MountedGrapplePullsBlockWithoutSwinging`, `GrapplePullsUberTargets` | Target sets [O lara.drm +0x464]; pull = 1 s heave [I] |
| Grapple swing | Attached at an edge, jump into the gap; camera behind her, hold Forward to build the swing; Space leaps off, Ctrl lets go | `State::GrappleSwing`: pendulum + stick pumping 500 u/s^2 any direction; jump / drop release | PARTIALLY IMPLEMENTED | `GrappleSwingAndRelease`, `PoolGrappleTargetAndSwing` | Pendulum, pump, drag, jump-off boost (+400) all [I] (Grapple.cpp:7); original runs Havok rope bodies. Pumping is not phase dependent |
| Grapple wall run | As the swing, feet on the wall; E + Forward / Back adjusts height; alternate Left / Right to build momentum; Space + direction leaps | `State::GrappleRappel`, `Player::rappel` side branch; `groundWallRun` | PARTIALLY IMPLEMENTED | `GrappleWallRunFromGround`, `ValaskjalfWallRun` | All rates [I] (`kWallRunSpeed 700`, `kWallRunAccel 2500`); jump = fixed kick off the wall (700 out + 200 up) whatever the stick; rope length changes without E |
| Climb and pivot on the cable | E held + Forward / Back climbs, Left / Right pivots | Free hang: **E alone** ascends, **walk (Alt)** descends (Grapple.cpp:378-386); no pivot | PARTIALLY IMPLEMENTED | `KrakenRoomRappel` (rope length change) | INPUT DIFFERS: original is E + stick; ours E / Alt without the stick. Pivot missing |
| Rappel | Feet on the wall under the anchor: E held + Forward / Back | `Player::rappel`: stick toward the wall shortens, away lengthens, **no E needed** | PARTIALLY IMPLEMENTED | `KrakenRoomRappel` | Input differs (no E); 300 u/s [I] |

## Combat (detail: audit part B)

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes |
|---|---|---|---|---|---|
| Target lock (draws / holsters) | Right Mouse or G held | `aim`; Combat.cpp:1233 hard lock | IMPLEMENTED | `Combat.PistolsLockFireAndHit`, `Combat.LockOnSelectionAndSwitch` | We add a `draw` key (X) the original lacks |
| Fire | Left Mouse or H; held = continuous | `fire` | IMPLEMENTED | `Combat.AmmoClipsAndReload`, `Combat.FireIndependentOfRenderRate` | |
| Grenade | Caps Lock | `grenade`; Gadgets.cpp | IMPLEMENTED | ROADMAP row "Combat" | |
| Reload | none (automatic) | automatic | IMPLEMENTED | `Combat.AmmoClipsAndReload` | |
| Change weapon | R | `weapon` | IMPLEMENTED | `Combat.WeaponSwitchKeepsRoundsAndReloadsFromReserve` | |
| Change target | Release target lock, turn the camera, lock again | `switch` (Z) cycles the lock | PARTIALLY IMPLEMENTED (works, differs from the original) | `Combat.LockOnSelectionAndSwitch` | Z is the original's Manual Aim key; a cycle key exists only on console pads (stick flick) |
| Manual / precision aim | Z toggles an over-shoulder aim view with a reticle | none; free aim = camera ray when nothing is locked (Combat.cpp:1250) | NOT IMPLEMENTED | GAMEPLAY.md:530 "no manual aim yet" | No aim camera mode either (AUDIT_A "Camera and input") |
| Recover after knock-down | Space tapped repeatedly | none | NOT IMPLEMENTED | GAMEPLAY.md:2552 (thrown / knock-down reactions missing) | |
| Evasive leap / roll | Target lock + direction + Space or Ctrl | none (plain jump / roll while locked) | NOT IMPLEMENTED | grep "evasive / dodge" hits only src/game/ai | |
| Melee kick; brush off / stomp; weapon of opportunity | E near an enemy; E repeatedly; E with a held pole | none for Lara (E + lock = adrenaline toggle; Mjolnir ground pound only) | NOT IMPLEMENTED | Combat.cpp:1290-1320; no kick clip in `kLaraClipNames` | |
| Jump kick | Running at an enemy + E | none | NOT IMPLEMENTED | — | |
| Enemy vault | Target lock + run at the enemy + Space | none | NOT IMPLEMENTED | grep "vault" in Combat = 0 | |
| Adrenaline head shot | Meter full: target lock, close in, **Q (grapple key)**: she vaults off the enemy, time slows, steer the small reticle onto the head ring with the mouse, fire | Locked + **E**: first press toggles adrenaline mode, a second press close to the target opens the window; **move stick** steers the pip; fire | PARTIALLY IMPLEMENTED (works, differs from the original) | `Combat.AdrenalineHeadShot`; tuning from Lara data +0x2C8.. [O], window length and pip [I] | Key (E vs Q), no vault, pip steered by the move stick instead of the mouse, mode-then-window sequence is ours |
| Focused fire | Meter partly full: target lock, then Z: time slows for the enemy and she fires faster for a short while | Adrenaline mode toggled with E while locked: `timeScale -> modeScale`, meter drains at `modeDrain` | PARTIALLY IMPLEMENTED (works, differs from the original) | `Combat.AdrenalineModeDrainsAndSlowsTime` | Key differs (E vs Z); whether the fire rate rises in the mode: unknown (not read in this part) |

## Swimming

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes |
|---|---|---|---|---|---|
| Surface (swim up) | Space | `jump` held under water = up component (`Player::swim` SwimUnder) | IMPLEMENTED BUT UNVERIFIED | `DiveSwimDownAndSurface` | Control scheme marked [I] in code (Player.cpp:2813) |
| Dive | Crouch key (the page labels the PC key Left Shift in the table and Crouch in the text; tru.exe's default for Crouch is Left Ctrl); a tap moves down a little, hold to keep going | `drop`: at the surface plays `swim_surfacedive` (667 down); under water held = down | IMPLEMENTED BUT UNVERIFIED | `DiveSwimDownAndSurface`, `PoolSwimReplay` | Tap-for-a-little: under water yes; at the surface always the full dive clip |
| Swim / steer with the camera | W A S D; Forward + mouse steers | camera-relative stick; yaw eases with `kSwimTurnAccel` | VERIFIED AGAINST ORIGINAL (speed, accel, turn lag) | `SwimSpeedMatchesTru`, `SwimTurnLagsLikeTru` | Under water the stick is horizontal only; the camera pitch does not steer her up / down (original: Forward follows the camera) — verify under Wine |
| Swim faster | Shift | `sprint`: x1.2, shared stamina | VERIFIED AGAINST ORIGINAL | `SwimSprintMatchesTru` | |
| Exit water | Keep pressing toward a low edge: she climbs out by herself | `tryClimbOut` only on **Jump pressed** at the surface (Player.cpp:2782) | PARTIALLY IMPLEMENTED (works, differs from the original) | `SwimEnterCrossClimbOut` | Input differs; edge height band 50..400 [I] |
| Swan dive | Forward + Space, then Ctrl at once (water, or a short drop on land with a roll) | none | NOT IMPLEMENTED | grep "swan" = 0 hits in src, docs, clip list | |
| Underwater evasive dodge | Target lock + direction + Ctrl | none | NOT IMPLEMENTED | — | |

## Motorcycle (detail: audit part B)

| Move or rule | PC default input (own words) | Our action / code path | Status | Evidence | Notes |
|---|---|---|---|---|---|
| Mount / dismount | E | `interact`; Vehicle.cpp `mountBike` | IMPLEMENTED | `VehicleReal.*` (ROADMAP) | |
| Shoot | H / Left Mouse, one pistol | `fire` on the bike (pistols / uzi) | IMPLEMENTED | ROADMAP row "Motorcycle" | GAMEPLAY.md:541 still says "no bike shooting yet" (doc stale vs ROADMAP / Combat.cpp:1196) |
| Headlight | F | `light` | unknown | part B | |
| Accelerate / reverse / steer | W / S / A D | raw move stick | IMPLEMENTED | `tests/test_vehicle.cpp` | |
| Brake / power slide | Space | hand brake on the jump action (Space / pad A); no hop (Wine: GAMEPLAY.md "Motorcycle") | IMPLEMENTED | Vehicle.cpp:310-341 | Original binds Vehicle Brake to Space (button 22 [O]); ours moved it to Ctrl and gave Space a hop the original may not have |
| Fast start / speed burst | Hold brake + Forward, release brake | burnout + launch charge (0x793680 bits 23 / 22): 10 x torque for 0.1 s | VERIFIED AGAINST ORIGINAL (Wine ref_launch.txt, `VehicleReal.LevelGroundManoeuvresMatchTheOriginal`) | Vehicle.cpp:329-366; tuning record [O], state logic from 0x790E40 | Key differs as above |
| Donut / fast turn | Fast start + steer; or brake + steer at speed | `BikeStunt::Donut` / `PowerSlide` | IMPLEMENTED BUT UNVERIFIED | Vehicle.cpp:332-378 | ROADMAP "Planned" section still says not done (stale) |

## Rules (HUD, health, saving): pointers only, audited in part B

| Rule | Original (own words) | Ours | Status | Evidence |
|---|---|---|---|---|
| Health regenerates partly; packs; relics raise the maximum and heal | 4 ways to heal; 6 relics double max health | Combat.cpp health / regen; pickups in Progress | IMPLEMENTED (regen, packs); relic max-health: unknown | `Combat.RegeneratesToQuarterOnly`, `Combat.HealthPacks` |
| Breath meter replaces the adrenaline meter under water | — | `World::tickAir`, AIR bar | IMPLEMENTED BUT UNVERIFIED | `WaterAirDrainsRefillsAndDrowns`; rates [I] 45 s (GAMEPLAY.md:2531) |
| Checkpoints autosave; death reloads the last one; manual save from the pause menu | — | `touchCheckpoints`, saves | IMPLEMENTED | `Save.*`, `Stream.*` (ROADMAP row "Saves") |
| Difficulty / player tailoring: health, enemy health, ammo, saving-grab time | — | `World::setTailoring` | IMPLEMENTED | `Tailoring.PresetsScaleHealthAndCapacity` |
| PDA: sonar map, area info, weapon selection, inventory, field assistance, journal | — | `src/ui` | IMPLEMENTED (sonar reveal radius [I]) | `Menus.*` |

## Input differences found (summary)

1. Saving grab recovers on Jump, original on Interact.
2. Climb faster (E in rhythm) missing on ledges, poles, ladders.
3. Rope climb: E / Alt instead of E + Forward / Back; rappel needs no E; no pivot.
4. Water exit needs Jump instead of holding Forward.
5. Z = change target instead of Manual Aim; adrenaline moves on E instead of Q (head shot) / Z (focused fire).
6. Beam: Shift (faster), Ctrl (walkover), Ctrl (drop to hang) not as the original.
7. Field camera zoom on forward / back instead of the fire / target-lock keys.
8. Bike brake on Ctrl instead of Space.
9. Holding Jump does not lengthen a jump; no sprint jump; no swan dive; no handstand pull-up.
10. Safety drop needs Ctrl instead of happening when she steps off a grabbable edge.
