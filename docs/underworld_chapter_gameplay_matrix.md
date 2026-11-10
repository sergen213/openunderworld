# Tomb Raider: Underworld — chapter gameplay matrix

What the original Windows PC campaign asks of the player, chapter by chapter, and where OpenUnderworld stands on each
requirement. The 21 sections follow the PC / Mac / PS3 / Xbox walkthrough of Stella's Tomb Raider Site
(<https://tombraiders.net/stella/underworld.html>; controls: <https://tombraiders.net/stella/walks/controls/TR8controls.html>).
The Wii and PS2 editions and the two Xbox expansions are out of scope. Nothing here quotes the walkthrough: every row
is our own technical description, with a link to the page (and to the screenshot that shows the move, where one does).

How to read it:

- One section per chapter with the fifteen categories of the project goal. The full requirement tables (ids `CNN-xx`,
  one row per required action sequence in route order, with our unit / object ids, status, evidence and the visual
  reference) are in `docs/matrix/NN_<chapter>.md`, linked from each section, together with each chapter's gap list and
  its self-test scenarios.
- Status of every requirement, exact strings:
  - `NOT IMPLEMENTED`: no code.
  - `PARTIALLY IMPLEMENTED`: code covers part of it; the missing part is named.
  - `IMPLEMENTED BUT UNVERIFIED`: code exists, but no automated test exercises it in this chapter's units, or its
    numbers are inferred ([I]).
  - `IMPLEMENTED`: code and a named automated test in this chapter's context.
  - `VERIFIED AGAINST ORIGINAL`: additionally a recorded comparison with tru.exe (a Wine measurement or decoded game
    data, [O]), cited.
- A status is chapter-specific: a mechanic tested only in another unit is `IMPLEMENTED BUT UNVERIFIED` here.
- Labels: [O] observed in the game data or tru.exe, [I] inferred, [U] unknown. Test names are `Suite.Name` in
  `tests/*.cpp`; `game_selftest_<name>` are `ou-game --self-test` scenarios (`docs/SELFTEST.md`); `game_replay_<name>`
  are input replays.
- The shared-mechanics matrix at the end lists every mechanic against the chapters that use it; a mechanic used in
  several chapters is one reusable implementation (named there), never per-level code.


## Status changes since the baseline

The chapter sections below record the state at v0.41.0 (the audit baseline). These mechanics have landed since; the
rows that name them move as stated until the per-row refresh of the chapter files:

| Since | Mechanic (reusable implementation) | Rows it changes | New status | Evidence |
|---|---|---|---|---|
| 0.42.0 | Chimney / wall jump (`src/game/WallJump.cpp`; M06) | every row listing the chimney jump as missing: C03-22, C05-09, C07-24, C07-37, C07-54, C07-56, C08-05, C08-32, C08-36, C08-44, C14-04, C14-14, C14-27, C14-49, C15-18, C17-28, C17-35, C17-38, C17-53, C17-60, C17-67, the two of section 21 | IMPLEMENTED at l2_junglehub 796 → 797, l5_valaskjalf 1512 → 1510, l2_towerroom 418 → 417; IMPLEMENTED BUT UNVERIFIED elsewhere (not measured against tru.exe yet) | `Game.ChimneyJump*`, `GameReal.JunglehubChimneyJumpClimbsColumns`, `GameReal.ValaskjalfChimneyJumpTakesAllFive` |
| 0.42.0 | Treasure urns and the reward table (M63, M64) | "Collectibles" of every chapter; all "jar treasure not collectable" rows | IMPLEMENTED (urns broken by shots; totals 26 / 30 / 13 / 50 / 30 / 0 / 30 and 6 relics from the game's own table); kick / bike breaking still NOT IMPLEMENTED | `ChaptersReal.RewardSurveyMatchesTheRewardTable`, `PickupsReal.*`, `game_selftest_treasure_urn` |
| 0.42.0 | Relic effect: max health + heal (M73) | C04-20, C09 relic, C11 relic 3, C15-05, C17 relic, C20 relic | IMPLEMENTED | `Pickups.RelicRaisesMaxHealthAndHeals`, `PickupsReal.RelicRaisesMaxHealthAndHeals` |
| 0.42.0 | Manual (precision) aim (M28) | C03-21, C08-40, C12 panthers, C13, C18-05 | IMPLEMENTED in l1_kraken_room and l4_pool; elsewhere IMPLEMENTED BUT UNVERIFIED | `Combat.ManualAimHitsWhatTheReticleCovers`, `ProgressionReal.KrakenChainLinksByManualAim`, `game_selftest_manual_aim` |
| 0.42.0 | Kraken kill by shooting the chain links | C03-21 | IMPLEMENTED | `ProgressionReal.KrakenChainLinksByManualAim`, `game_selftest_kraken_kill` |
| 0.42.0 | Situational adrenaline: script time scales (M36) | C03-35, C06-05, C08-14, C14 razor house, C17 bridge slide, C21 dodge events | IMPLEMENTED on l1_kraken_room's script; the other sequences IMPLEMENTED BUT UNVERIFIED (their objects are still open) | `ProgressionReal.KrakenTentacleSlowMotionByItsScript`, `Combat.ScriptTimeRampEasesInAndOut` |
| 0.42.0 | Adrenaline head shot / focused fire inputs, dual targets | head-shot and focused-fire rows of sections 7, 8, 12-14 | IMPLEMENTED (head shot on a real panther); enemy vault still NOT IMPLEMENTED | `Combat.HeadShotConditionsMissAndInterrupt`, `CombatReal` head-shot test in l4_pool, `game_selftest_adrenaline` |
| 0.42.0 | One-handed shooting while hanging / climbing (M32) | C07, C08-33, C14, C17 rows | IMPLEMENTED at l4_pool ledge 1253; elsewhere IMPLEMENTED BUT UNVERIFIED | real-level test in `tests/test_combat.cpp` (pistols from ledge 1253 on a panther) |
| 0.42.0 | Scuba / breath rule (M85) | C02-02, C02-04, section 19 dive | IMPLEMENTED in l1_seafloor; l7_arctic_sea IMPLEMENTED BUT UNVERIFIED | `GameReal.SeafloorScubaSwimKeepsTheBreath`, `GameReal.ThailandDiveDrownsAtTheBreathTime` |
| 0.42.0 | Weapons in the water, under-water dodge | C02-06, C02-08 | IMPLEMENTED | `GameReal.SeafloorSpearKillsShark`, `GameReal.SwimDodgesMoveAsTheirClips`, `game_selftest_swim_moves` |
| 0.42.0 | Jellyfish | C02 hazard rows | IMPLEMENTED | `GameReal.JellyfishStingOnTouch` |
| after 0.43.0 | Trap damage: damaging collision primitives (M67, M68, M69, M79; `src/game/Hazards.cpp`) | C14 trap-house rows (flame jets, crushers, spear rows, blades), C17 rollers / weights / hammer blow, C03-07 tentacle swats, C06-05 barrel run, closing doors | IMPLEMENTED for flame jets, crusher pairs and rollers (played); the others IMPLEMENTED BUT UNVERIFIED (same system, their amounts from the data); steam (M77): script runs, UNVERIFIED | `GameReal.FlameJetBurnsLaraOncePerWindow`, `GameReal.CrusherJawsOpenCloseAndKill`, `GameReal.RollersCost400ATouch`, `GameReal.RestingSolidHazardsDoNotHurt`, `GameReal.FireVolumeBurns300`, `Combat.ContactDamageClassWindowAndKill`, `game_selftest_hazards`; docs/GAMEPLAY.md "Hazards" |
| 0.42.0 | Swan dive | C03-23, C05-10, C07-19 | IMPLEMENTED at the l2_cliffclimb cliff; the other spots IMPLEMENTED BUT UNVERIFIED | `GameReal.CliffSwanDiveIntoTheSea`, `game_selftest_swan_dive` |
| 0.42.0 | Player sensors (script vt 14) | prerequisite of the gauntlet rows (M60); those stay NOT IMPLEMENTED until the heavy movers exist | — | `docs/formats/SCRIPTS.md` "playersensor" |

## 01. Prologue: No Illusions (Prologue - Croft Manor)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/00prologue.html (no sub-pages; two inline screenshots)
Our units: l3_main_bedroom → l3_bedroom_hall → l3_main_hall (chapter 0, docs/PROGRESSION.md "Chapter routes")

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic whose only tests run in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | New Game → explode movie → corridor cinematic → three manor units → Zip/Winston cinematic → Mediterranean | IMPLEMENTED | `ChaptersReal.StartUnitsAndLevelEnds`, `ProgressionReal.CriticalPathPerChapter` (chapter 0), `StreamReal.PrologueCorridorToStaircase` (new-game pose vs Wine [O]) |
| Movement | Run, corner turns, stairs, standing jump, running jump, crouch, forward roll | VERIFIED AGAINST ORIGINAL (run / stairs / jumps); roll IMPLEMENTED BUT UNVERIFIED | `GameReal.BedroomJumpsMatchTruTraces`, `GameReal.StairsWalkAndRunUpAndDown` (Wine traces in l3_main_bedroom, GAMEPLAY.md "Numbers"); roll: `Game.CrouchUnderSlabAndRoll` (synthetic) |
| Traversal | Low-wall grab + pull-up, hurdle onto debris, ledge shimmy, hang jump up, drop-and-regrab, lean back + back jump, side jump off a ledge, drop to hang, climb onto a pushed block | VERIFIED AGAINST ORIGINAL (hurdle, wall 973, hang leans / leaps in l3_main_bedroom); the l3_bedroom_hall molding chains IMPLEMENTED BUT UNVERIFIED | `StreamReal.PrologueCorridorToStaircase`, `GameReal.HangJumpLeans`, `GameReal.HangLeapsMatchTruTraces`; no test on the bedroom-hall moldings |
| Physics | Capsule vs burning debris, step limits 110 [O Wine], floor collapse, push block as kinematic body | PARTIALLY IMPLEMENTED | steps: `GameReal.StairsWalkAndRunUpAndDown`; block: `GameReal.PushBlockIndependentOfRenderRate`; collapsing floor: unknown (class-30 breakables "Not implemented", GAMEPLAY.md "Breakables") |
| Grapple | (a) ring on a wooden door: throw + pull tears the door off; (b) ceiling ring in the main hall: swing over the burning staircase, pump, jump release; rope length adjust | PARTIALLY IMPLEMENTED | (b) target offer: `GameReal.MainHallRingByItsOwnRange`, `GameReal.SharedQThrowsTheGrappleNotTheCamera` (l3_main_hall); swing landing not tested here (mechanic test `Game.GrappleSwingAndRelease`); (a) door pull: no gate row in PROGRESSION.md chapter 0, unknown |
| Animation | Idle / run / turn, hurdle blend (request 132), hang leans, block grab / push / release, switch pull, pickup, grapple throw / swing | IMPLEMENTED | `StreamReal.PrologueCorridorToStaircase` asserts `hurdle_low_onto` mix; `GameReal.LaraPlaysInteractionClips` (l3_main_bedroom switch); `GameReal.TransitionsReducePops` |
| Combat | n/a (no enemies; Lara has pistols only [I]) | n/a | `StreamReal.ChapterStartEnemiesStayInTheirMedium` starts in l3_main_bedroom |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Electrical-box switch 4525 → door 4520; grapple-ring door; lacquered chest = push block 4363; first-aid kit | PARTIALLY IMPLEMENTED | gates 4525 / 4363 pass (`ProgressionReal.CriticalGates`, `ScriptRealData.BedroomSwitchOpensHouseDoor`); ring door unknown; health pack: `Pickups.InteractCollectsNearestOnceAndSaves`, `Combat.HealthPacks` (mechanic tests) |
| Puzzles | Tutorial-level only: open door by switch, pull door by grapple, push chest to reach molding | PARTIALLY IMPLEMENTED | see Objects |
| Hazards | Open flames (contact damage), flaming pits (death on fall), wall flames that do not hurt, scripted floor collapse | PARTIALLY IMPLEMENTED | fire volumes hurt: `StreamReal.PrologueCorridorToStaircase` ("Fire volumes on the way hurt her"); `FxReal.PrologueFireScreenFilter`; death volumes via `health` 1/2 natives (SCRIPTS.md); collapse unknown |
| Collectibles | Health items 1, treasures 0, relics 0, keys 0 (page header) | IMPLEMENTED | `chapterRewards(0)` is empty (`ChaptersReal.RewardTableMatchesUnits`: relic 0); health-pack intro `*_rw_shard` |
| Progression | Checkpoints / autosave, manual saves, subtitles option, helper buttons / training text; chapter end plane 4129 → cinematic 4106 (010) → chapter 1 | IMPLEMENTED (chapter end, saves); helper buttons PARTIALLY IMPLEMENTED | `ProgressionReal.CriticalGates` (4129), `ChaptersReal.CroftManorEndingRunsBothCinematics` (chapter 3 revisit), `Stream.CheckpointAndRespawn`, `Save.*`; HELPER BUTTONS "stored only" except the grapple prompt (GAMEPLAY.md "Menus") |
| Automated tests | Existing: `StreamReal.PrologueCorridorToStaircase`, `GameReal.BedroomJumpsMatchTruTraces`, `GameReal.HangJumpLeans`, `GameReal.HangLeapsMatchTruTraces`, `GameReal.MainHallRingByItsOwnRange`, `GameReal.SharedQThrowsTheGrappleNotTheCamera`, `GameReal.MainHallPoleFlingLandsOnPlatform`, `GameReal.RunningJumpReachAndRush`, `ProgressionReal.CriticalGates` (4525, 4363, 4129), ctest `game_replay_*` none for this chapter | Required: bedroom-hall roll + ring-door pull, molding chain, chest climb, floor-collapse jump, main-hall swing landing, end-to-end prologue self-test | — |

Requirement rows: 16 in [docs/matrix/01_prologue.md](matrix/01_prologue.md) — IMPLEMENTED BUT UNVERIFIED 8, IMPLEMENTED 5, VERIFIED AGAINST ORIGINAL 3.

Open in this chapter:

- C01-07 ring door: no evidence that the manor's grapple door can be pulled (not in the gates table, not in `GateSweep` notes). If it does not open, the prologue cannot be walked.
- C01-11 collapsing floor: unknown object class; class-30 breakables are documented as not implemented.
- C01-09 / C01-10 molding chains in l3_bedroom_hall have no test; the drop-and-regrab of a lower ledge has no named test anywhere.
- C01-12 no test lands the main-hall swing on the far balcony; swing physics are [I].
- C01-13 rope-length input: walkthrough gives "hold Interact + Up / Down"; our binding is not documented against it.
- Helper buttons option only stores its value; checkpoint chime not confirmed.
- Roll (C01-05) is tested only on synthetic collision.

## 02. The Path to Avalon (Level 1 - Mediterranean Sea, part 1)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/01avalon.html
Sub-pages read: details/avalon2.html … avalon8.html (treasure routes, ledge climb), screenshots/mediterranean_map.jpg (treasure map), bugnotes.html
Our units: l1_seatop → l1_seafloor → l1_norse_hallway_a → l1_kraken_entrance (chapter 1 route, first four units; ends at reveal cinematic 696 / gate 743)

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Boat start after the uplink cinematic, open-sea dive, submerged ruins hub with five side chambers, eye-door puzzle, flooded corridor, two dry rooms, serpent-head door room | PARTIALLY IMPLEMENTED | start: `StreamReal.ChapterStartIntros` (l1_seatop, l1_cineseatop), `StreamReal.YachtDecksAboveSea`; route by teleport only: `ProgressionReal.CriticalPathPerChapter`; eye puzzle "not covered" (PROGRESSION.md) |
| Movement | Deck walk, jump off the stern, wading, ledge steps, ramps | IMPLEMENTED | `Game.JumpTakeOffsMatchTruTraces` (l1_seatop traces), `Game.WadingClipsAtTheDecodedDepth` (mechanic test) |
| Traversal | Pool climb-out, vault, handhold chain round a block corner, hang jump up / back, back jump from a hang onto a horizontal pole, pole swing + release to a handhold, triple drop-down | IMPLEMENTED BUT UNVERIFIED | mechanic tests `GameReal.HangShimmyRoundCorners`, `Game.HangJumpsUpRightAndBack`, `Game.PoleGrabSwingFlingDrop`, `GameReal.TrapRoomsPole*`; nothing in l1_norse_hallway_a |
| Physics | Carried weights dropped on pressure plates, grapple-pulled serpent head sliding down its rail onto a plate, plate weight thresholds | IMPLEMENTED (behaviour); thresholds [I] | `ProgressionReal.CriticalGates` (748 with 751 pull + 753 weigh), `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`; plate motor "+20 kg" is [I] (PHYSICS.md "Pressure plates") |
| Grapple | Ring on the right serpent head: throw, repeated Interact taps pull it loose | IMPLEMENTED | gate 751 "pass (in the 748 gate)" |
| Animation | Swim set (surface, dive, submerged, sprint), carrying a large handle while swimming, turning the door handles, jar smash kick, block carry / set-down | PARTIALLY IMPLEMENTED | swim clips matched to Wine snapshots (GAMEPLAY.md "Animation mapping"); "[I] No carry clips" (Objects.cpp `carryReady` comment); no kick / handle-turn clip use |
| Combat | 3-4 sharks in open water; pistols usable under water, spear gun, sticky grenades with target lock, underwater evasive dodge (lock + strafe + Crouch) | PARTIALLY IMPLEMENTED | `AiReal.SharksSpawnSwimBiteAndDie` (l1_seafloor); speargun only under water in ours (GAMEPLAY.md "Combat": which weapons the exe allows submerged is [U]); under-water grenade placing listed missing (GAMEPLAY.md "Deviations"); no dodge code |
| Swimming | Dive from the boat, automatic scuba, long 3D swims through doorways, vertical tunnel, dark tunnels, surfacing in a pool | IMPLEMENTED (swim); scuba air rule unknown | `Game.SwimSpeedMatchesTru`, `Game.SwimSprintMatchesTru`, `Game.SwimTurnLagsLikeTru` (Wine [O]), `GameReal.SeaCircleCameraLevelsOut` (l1_seatop); breath drains whenever the head is under water [I] (GAMEPLAY.md "Oxygen") |
| Vehicle | n/a | n/a | — |
| Objects | Two loose door handles on pedestals (carried while swimming, inserted), three rotating door circles, breakable jars, small carved blocks, pressure pads 752 / 753, serpent heads, reset switch 748, health potion | PARTIALLY IMPLEMENTED | 748 / 751 / 752 / 753 pass; eye pieces 231-233 not covered; jars break only by shots (GAMEPLAY.md "Breakables") |
| Puzzles | (1) Eye door: fit two handles, rotate top ×1, left ×1, right ×2 so the top circle shows only open eyes; (2) serpent door: head on right pad, two blocks on left pad, then the switch | (1) PARTIALLY IMPLEMENTED, uncovered; (2) IMPLEMENTED | (1) PROGRESSION.md gate row "not covered: pieces are physics objects [U interaction]"; (2) `ProgressionReal.CriticalGates` |
| Hazards | Sharks, luminous jellyfish (poison on proximity), harmless tentacle swat at the round hole, dark tunnels | PARTIALLY IMPLEMENTED | sharks only; no jellyfish code or doc entry (no match for "jelly" in src/game, docs/GAMEPLAY.md) |
| Collectibles | Page header: health 1, treasures 10 (1/26..10/26 of 26 in the whole Mediterranean), relics 0, keys / puzzle items 2 | PARTIALLY IMPLEMENTED | `game::chapterRewards(1)` lists 10 treasures + relic 321 for the WHOLE Mediterranean (Progress.cpp) against the page's 26 + 1: the 16 jar treasures (`*_weightbreak_reward`) are "untouched" (GAMEPLAY.md "Breakables") |
| Progression | Journal entry unlock, checkpoint behind the eye door, reveal cinematic 696 and gate 743 at the end | IMPLEMENTED (696 / 743); journal IMPLEMENTED | `ProgressionReal.CriticalGates`, `Menus.PdaJournal` |
| Automated tests | Existing: `AiReal.SharksSpawnSwimBiteAndDie`, `GameReal.SeafloorKelpSwaysInTheUnitWind`, `ScriptRealData.SeafloorVolumeSetsHelpCondition`, `Pickups.InteractCollectsNearestOnceAndSaves` (l1_seatop data), `ProgressionReal.CriticalGates` (748), `StreamReal.Yacht*`, `OriginalShading.KrakenHeadIrradianceMatchesOriginal` | Required: eye-door solve, swim-carry + insert, jar smash + reward, treasure-10 ledge / pole chain, plate threshold (one block must not hold pad 753), jellyfish damage, underwater pistols | — |

Requirement rows: 32 in [docs/matrix/02_path_to_avalon.md](matrix/02_path_to_avalon.md) — IMPLEMENTED BUT UNVERIFIED 15, PARTIALLY IMPLEMENTED 7, IMPLEMENTED 6, NOT IMPLEMENTED 3, unknown / n/a 1.

Open in this chapter:

- Eye-door puzzle (C02-09, C02-11, C02-13) is not covered by any test and its interaction is [U]: the critical path is only passed by a level load.
- Jar treasures: 16 of the Mediterranean's 26 treasures sit in breakable jars; our reward table has only the 10 loose ones, jar rewards are not spawned / collectible, and jars cannot be smashed with Interact (no Lara melee).
- Jellyfish hazard: NOT IMPLEMENTED.
- Scuba / breath rule under water is unknown; with the current rule Lara would start drowning after 133 s in a level that is mostly under water.
- Underwater combat: pistols and grenades under water, evasive dodge missing; carried handle is not dropped on firing.
- Pressure pad 753 accepts one block (plate numbers [I]); set-down key differs (Interact vs Crouch).
- Treasure-10 / exit ledge-pole chain in l1_kraken_entrance untested; hang-jump onto a pole unverified.
- Treasure proximity chime, carry clips, handle-turn clips: unknown / missing.

## 03. Niflheim (Level 1 - Mediterranean Sea, part 2)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/02niflheim.html
Sub-pages read: details/niflheim2.html (high route), niflheim3.html (statue arm to switch ledge), niflheim4.html (chimney jump to treasure 15), details/swandive.html, bugnotes.html
Our units: l1_krakenstairs_a / _b → l1_kraken_room → l1_baldrsstairs_a / _b (ends on entering l1_baldrs_tomb, Thor-statue cinematic)

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Stair halls with pits, the kraken hall (two gear rooms, statue, spiked platform, pool), exit stairs with a climb wall and a beam | PARTIALLY IMPLEMENTED | switches and door pass as gates (PROGRESSION.md chapter 1 rows l1_kraken_room); the kill sequence (chains) and the gear pulls are not covered |
| Movement | Stairs, crouch tunnels, roll through a low gap, slope slide, wading | IMPLEMENTED (stairs VERIFIED AGAINST ORIGINAL) | `GameReal.StairsWalkAndRunUpAndDown` includes l1_krakenstairs_a / _b risers; slide: `Game.SlideAcceleratesAtGravityAlongSlope` (Wine trace from l3) |
| Traversal | Handhold-to-handhold side jumps over pits, slide → jump → pole grab, hand-over-hand, pole swing to ledge, corner shimmy, back jump to a ledge, jumps onto statue fingers, hanging pillars (high route), free-climb wall, balance beam, chimney jump, swan dive | PARTIALLY IMPLEMENTED | free climb attach rule tested in l1_baldrsstairs_a (`GameReal.ClimbOnlyAtHolds`); chimney jump and swan dive absent from src/game; everything else mechanic tests in other units |
| Physics | Toppling pillar, gear pulled out on its joint, spiked platform raised then dropped when both chains break, retracting walkways | PARTIALLY IMPLEMENTED | column on a braked prismatic joint: `ProgressionReal.KrakenColumnShot` (moved by a bullet impulse [I]); platform 27 raise: `CriticalGates` (30 → 27); chain break / drop: no evidence |
| Grapple | Pull ring on the support pillar (right room), pull ring on the recessed gear (left room); wall anchor 326 exists for rappel | PARTIALLY IMPLEMENTED | rappel: `GameReal.KrakenRoomRappel`, ctest replay `kraken_rappel`; the two pulls are not gates in PROGRESSION.md (the column is shot in our test) |
| Animation | Kraken body / tentacle idles, gear idles, door opening clip, Lara pole / beam / climb sets, beam walkover, swan dive | PARTIALLY IMPLEMENTED | `ProgressionReal.KrakenRoomSectionsLoopTheirIdles` (Wine section clips [O]), `ProgressionReal.TombDoorOpensByItsSectionClip`; walkover / swan dive missing |
| Combat | Precision (manual) aim with pistols at two chain weak links; kraken immune to gunfire | PARTIALLY IMPLEMENTED | "no manual aim yet" (GAMEPLAY.md "Controls" row 9); free aim along the camera ray exists (GAMEPLAY.md "Combat"); `World::uberShot` generic |
| Swimming | Kraken pool after the kill: dive for a treasure, climb out on submerged stairs, swim to the exit | IMPLEMENTED BUT UNVERIFIED | mechanic tests `Game.DiveSwimDownAndSurface`, `Game.SwimEnterCrossClimbOut` |
| Vehicle | n/a | n/a | — |
| Objects | Switches 57 / 55 (gear rooms), 30 (platform), 498 (exit), gears 32 / 36, platform 27, tomb door 497, tentacle hazards 494-496, jars, health potions | IMPLEMENTED (switch gates) | PROGRESSION.md: "switches 55 / 30 / 498 pass", "57 pass after the column is shot" |
| Puzzles | Free both gears (pillar pull, gear pull), run both mechanisms to retract the walkways, raise the platform, shoot both weak links → kraken dies → exit switch | PARTIALLY IMPLEMENTED | as above; no test runs the sequence in order |
| Hazards | Tentacle swats near the pool (painful to lethal by distance), deep pits, beam fall, scripted ledge smash with slow motion (high route) | PARTIALLY IMPLEMENTED | tentacles 494-496 swing (`GateSweep` note in test_gates.cpp); damage to Lara unknown; situational adrenaline absent (gamebase 2 / 24 time scale "not applied", Script.cpp) |
| Collectibles | Page header: health 2, treasures 9 (11/26..19/26), relics 0 | PARTIALLY IMPLEMENTED | only the two loose ones (16, 18) can be in `chapterRewards(1)`; the seven jar treasures are not collectible (see file 02) |
| Progression | Kraken intro cinematic + reveal camera, kill cutscene, door 497, Thor-statue cinematic on entering the tomb | IMPLEMENTED (reveal, 497); kill cutscene unknown | `GameReal.RevealCameraPlaysUnitSpline` (plane 37 → reveal 53), `ProgressionReal.TombDoorOpensByItsSectionClip` |
| Automated tests | Existing: `ProgressionReal.CriticalGates` (498 → 497, 30 → 27), `ProgressionReal.KrakenColumnShot`, `ProgressionReal.KrakenRoomSectionsLoopTheirIdles`, `ProgressionReal.TombDoorOpensByItsSectionClip`, `GameReal.KrakenRoomRappel`, `GameReal.RevealCameraPlaysUnitSpline`, `GameReal.ClimbOnlyAtHolds`, `GameReal.StairsWalkAndRunUpAndDown`, `StreamReal.SaveContinuesBitIdentically` (l1_kraken_room) | Required: ordered kraken sequence, chain shots, grapple pulls, chimney jump, swan dive, pit handhold jumps, climb wall, beam + walkover, tentacle damage | — |

Requirement rows: 36 in [docs/matrix/03_niflheim.md](matrix/03_niflheim.md) — IMPLEMENTED BUT UNVERIFIED 17, PARTIALLY IMPLEMENTED 9, IMPLEMENTED 5, NOT IMPLEMENTED 4, unknown / n/a 1.

Open in this chapter:

- C03-21 kill sequence: no manual / precision aim mode, no evidence that the chain links can be shot or that the platform drops; critical path.
- C03-11 / C03-16 gear-room pulls: the walkthrough uses grapple pulls on a pillar ring and a gear ring; ours shoots the column and needs nothing for the left gear. Needs a Wine check of both interactions.
- C03-22 chimney / wall jump: NOT IMPLEMENTED (optional here, mandatory in file 07).
- C03-23 swan dive, C03-31 beam walkover, C03-13 Interact-tap speed climb: NOT IMPLEMENTED.
- C03-35 situational adrenaline (scripted slow motion + breaking ledge): NOT IMPLEMENTED (alternate route only).
- C03-07 tentacle damage to Lara: unknown.
- Seven jar treasures (11-15, 17, 19) not collectible.
- No test walks any traversal chain of this chapter (pit hold jumps, slide → pole, statue jumps, climb wall, beam).

## 04. The Norse Connection (Level 1 - Mediterranean Sea, part 3)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/03norse.html
Sub-pages read: details/norse2.html (column room), norse3.html (grapple swing), norse4.html (two treasures and the relic), bugnotes.html (major bug 1: boat cinematic not triggering, PS3 / Xbox 360 only)
Our units: l1_baldrs_tomb → l1_baldrs_sideroom → l1_baldrs_hall → l1_kraken_room (upper level) → back through l1_krakenstairs_b / _a, l1_kraken_entrance, l1_norse_hallway_a, l1_seafloor → l1_seatop → l1_cineseatop (unit-to-room mapping of sideroom / hall is [I] from the route order in docs/PROGRESSION.md)

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Gauntlet-room gate puzzle, gauntlet cinematic + explosion, detour through a column hall to the kraken hall's upper level, relic, full backtrack to the boat | PARTIALLY IMPLEMENTED | gate puzzle: ctest replay `game_replay_baldrs_plates_gate`, `ProgressionReal.CriticalGates` (219, 221); the rest untested; boat trigger not in the gates table |
| Movement | Stairs, crouch tunnels, roll through a closing door, running jumps | IMPLEMENTED (stairs VERIFIED AGAINST ORIGINAL) | `GameReal.StairsWalkAndRunUpAndDown` (l1_baldrs_tomb risers) |
| Traversal | Pillar climb + jump into a wall hole, pillar-to-pillar jumps, jump onto / between perches, pole swing, slope slide to ledge, hang traverse with gap jumps, reverse handhold route over the pits | IMPLEMENTED BUT UNVERIFIED | mechanic tests `GameReal.PillarAttachRoundUpSlideOff`, `GameReal.PillarJumpsByStickAngle`, `Game.PerchJumpsByTheirQueries`, `GameReal.CavernsPerchJumpReaches`, `GameReal.TemplePerchTurnsAndPillarTop` (l3 / l4 units) |
| Physics | Thrown blocks (ballistic, bounce off the door frame), grenade blast pushing loose blocks, plates with weight thresholds, gate torn out by a grapple pull, press plate rising again | PARTIALLY IMPLEMENTED | plates: `GameReal.PressurePlateIndependentOfRenderRate` (219), `GameReal.JointSoundsFollowMovingJoints`; no throw; gate 45 falls by its own joint once the locks leave |
| Grapple | Pull the gate by its ring while standing on a pad; ceiling-ring swing across the kraken hall | PARTIALLY IMPLEMENTED | gate pull not required in ours (PROGRESSION.md: locks 69 / 70 out, "45's limit opens 0..90 and it falls"); swing: mechanic test `Game.GrappleSwingAndRelease` |
| Animation | Carry / throw / set-down, pillar and perch sets, gauntlet cinematic with mercenaries | PARTIALLY IMPLEMENTED | no carry or throw clips (Objects.cpp "[I] No carry clips"); cinematics: `CineReal.EveryStreamPlaysWithItsActors` |
| Combat | n/a (mercenaries appear only in cinematics); sticky grenades as an optional block-mover | n/a | — |
| Swimming | Return swim through the flooded corridor and up to the boat | IMPLEMENTED BUT UNVERIFIED | see file 02 |
| Vehicle | n/a | n/a | — |
| Objects | Press plates 219 / 221 / 222, door 223, weights 218 / 220, gate 45 with locks 69 / 70, jars, health potions, relic orb, blast-dislodged block | IMPLEMENTED (plates, door, locks) | PROGRESSION.md chapter 1 row l1_baldrs_tomb "pass"; GAMEPLAY.md "Receptacle replays" row `baldrs_plates_gate` |
| Puzzles | Get both blocks out of the side room through a door that only stays open while its plate is loaded (by throwing them), load one gate pad with both blocks, stand on the other, pull the gate | PARTIALLY IMPLEMENTED | solved differently in ours: one weight left on 219 holds door 223, ONE weight on 222 + Lara on 221 open the gate, no throw, no pull |
| Hazards | Deep pits on the way back, long falls in the column hall, jellyfish on the swim out | PARTIALLY IMPLEMENTED | fall damage [O curve] `Combat.FallDamageThresholds`; jellyfish NOT IMPLEMENTED (file 02) |
| Collectibles | Page header: health 2, treasures 7 (20/26..26/26, all in jars), relics 1 (1/6); a relic heals fully and raises maximum health for good | PARTIALLY IMPLEMENTED | relic 321 is in `chapterRewards(1)` and its intro is in l1_kraken_room (`ChaptersReal.RewardTableMatchesUnits`); no max-health or heal effect on relic pickup in src/game (search "relic": counting / PDA only); jar treasures not collectible |
| Progression | Gauntlet cinematic (item taken by mercenaries), cutaway on re-entering the kraken hall, boat cinematic that changes to the ship level | PARTIALLY IMPLEMENTED | route "… l1_seatop → l1_cineseatop → l1_ship_exterior" (PROGRESSION.md); the trigger at the boat has no gate row / test |
| Automated tests | Existing: ctest `game_replay_baldrs_plates_gate`, `ProgressionReal.CriticalGates` (219 → 223; 221 + 222 → 45), `GameReal.PressurePlateIndependentOfRenderRate`, `GameReal.JointSoundsFollowMovingJoints`, `GameReal.LaraPlaysInteractionClips`, `GameReal.StairsWalkAndRunUpAndDown`, `ChaptersReal.RewardTableMatchesUnits` | Required: block throw, two-block threshold, gate pull, column hall pillar / perch chain, kraken-hall swing, relic effect, boat trigger | — |

Requirement rows: 25 in [docs/matrix/04_norse_connection.md](matrix/04_norse_connection.md) — IMPLEMENTED BUT UNVERIFIED 16, PARTIALLY IMPLEMENTED 5, IMPLEMENTED 2, NOT IMPLEMENTED 1, unknown / n/a 1.

Open in this chapter:

- C04-03 throwing a carried object: NOT IMPLEMENTED; the walkthrough's solution of the side room depends on it.
- C04-06 pad threshold: the original needs both blocks on a pad; ours accepts one (plate numbers [I]) and the replay solves the room with a different block distribution.
- C04-08 gate pull: the original needs a grapple pull from the pad; ours lets the gate fall once the locks are out. Needs a Wine check.
- C04-20 relic effect (full heal + permanent max health): not in code.
- C04-24 boat trigger → l1_cineseatop → ship: no gate row or test.
- Seven jar treasures (20-26) not collectible.
- Column hall (pillars + perches), kraken-hall swing, upper-level pole / slide / hang chain, and the reverse pit route have no tests in these units.
- C04-04 grenade impulse on loose blocks: unknown.

## 05. God of Thunder (Level 1 - Mediterranean Sea, part 4)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/04thunder.html
Sub-pages read: details/swandive.html (Swan Diver II spot on the upper deck), bugnotes.html (nothing for this section)
Our units: l1_ship_exterior → l1_ship_upper_hallway_a / _b → l1_ship_upper_hallway → l1_ship_engine_hall → l1_ship_natla_room (chapter 1 route, ship part up to the Natla cinematic)

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Board the ship by its anchor, fight across the container deck, upper deck, helipad and aft deck (19 mercenaries on the page), go below to the engine room and the Natla room | PARTIALLY IMPLEMENTED | enemies and doors tested; boarding climb and the fights as a route are not |
| Movement | Run, strafe under fire, stairs, cover behind crates | IMPLEMENTED BUT UNVERIFIED | no Lara-movement test in the ship units |
| Traversal | Jump from the boat to the anchor, climb anchor and chain, four ledge jumps to the deck; container stack by chimney jump or by box steps; jumps across containers | PARTIALLY IMPLEMENTED | chimney jump NOT IMPLEMENTED; anchor / chain markup not surveyed (no unit has rope / vertical-pole markup, GAMEPLAY.md "Ladders…" survey note, so it must be ledges or holds) |
| Physics | Exploding fuel tanks (blast damage to enemies and Lara), crates shot to pieces, sticky grenades, enemy ragdolls | PARTIALLY IMPLEMENTED | crates: `ProgressionReal.CombatCrateBreaksOnHit`; ragdolls: `GameReal.KilledPoacherRagdollIndependentOfRenderRate` (mechanic); barrel explosion objects "not modelled: no blast damage" (GAMEPLAY.md "Breakables") |
| Grapple | n/a on the page | n/a | — |
| Animation | Two-gun / rifle aim grids, hit reactions, mercenary cover, bark and death sets, kick and jump-kick | PARTIALLY IMPLEMENTED | `Combat.HitReactionPlaysAndReturns`, `AiReal.DecodedSensesAttacksDeaths`; Lara kick clips unused |
| Combat | Weapon choice in the PDA, target lock that holds through cover, re-lock to change target, strafing, kick (Interact) and running jump-kick knock-downs, grenades, shootable fuel tanks, enemies using cover, patrolling and flanking | PARTIALLY IMPLEMENTED | `AiReal.MercsGuardZonesCoverAndAlert`, `AiReal.MercPatrolsSearchesAndReturns`, `AiReal.ShipNavGraphFlankAndInvestigate` (all l1_ship_exterior), ctest replay `merc_squad`; `Combat.LockOnSelectionAndSwitch`, `Combat.PistolsLockFireAndHit`, `CombatReal.RifleAndTranquilizerClips` (mechanic); no Lara melee |
| Swimming | Optional swan dive off the upper deck into the sea | NOT IMPLEMENTED | swan dive absent; sea volume exists (`StreamReal.OpenSeaWater`) |
| Vehicle | n/a | n/a | — |
| Objects | First-aid kits ×5, fuel tanks, breakable crates, cable spool, automatic ship doors, rifles dropped by mercenaries | PARTIALLY IMPLEMENTED | doors: volumes 1531 / 1492 / 1465 / 1436 "pass (1531 in suite)" (PROGRESSION.md); rifle ammo from bodies (`AiReal.MercsGuardZonesCoverAndAlert`) |
| Puzzles | n/a | n/a | — |
| Hazards | Gunfire, own grenades sticking nearby, tank explosions, fall into the sea | PARTIALLY IMPLEMENTED | grenade self-damage: `Gadgets.GrenadeKillsDummyAndHurtsLara`; tank blast missing |
| Collectibles | Page header: health 5, treasures 0, relics 0 | IMPLEMENTED BUT UNVERIFIED | health packs are `*_rw_shard` intros; positions in the ship units not asserted |
| Progression | Engine-room cinematic (fuel pipe explosion, sprinklers), Natla cinematic, then the escape section | IMPLEMENTED | cinematics 1453 / 1288 via volumes 1465 / 1436 (PROGRESSION.md), `CineReal.NatlaSubtitlesCameraAndVoiceLines` |
| Automated tests | Existing: `AiReal.MercsGuardZonesCoverAndAlert`, `AiReal.MercPatrolsSearchesAndReturns`, `AiReal.ShipNavGraphFlankAndInvestigate`, `AiReal.DeathTimerAndThrallChunks` (ship data), `ProgressionReal.CombatCrateBreaksOnHit`, `ProgressionReal.CriticalGates` (1531 → 1529), `CineReal.NatlaSubtitlesCameraAndVoiceLines`, `StreamReal.SaveContinuesBitIdentically` (l1_ship_exterior), ctest `game_replay_merc_squad` | Required: anchor boarding climb, deck fight with counts, tank explosion damage, kick / jump-kick, container climb, door chain down to the Natla room | — |

Requirement rows: 20 in [docs/matrix/05_god_of_thunder.md](matrix/05_god_of_thunder.md) — IMPLEMENTED BUT UNVERIFIED 10, IMPLEMENTED 6, NOT IMPLEMENTED 2, PARTIALLY IMPLEMENTED 2.

Open in this chapter:

- C05-01 boarding by anchor and chain: markup and behaviour unknown, no test; it is the only way onto the ship.
- C05-06 Lara melee (kick, jump-kick, knock-down): NOT IMPLEMENTED.
- C05-07 fuel-tank explosions deal no damage.
- C05-09 chimney jump alternative missing (box route should still work, untested).
- C05-10 swan dive missing.
- Encounter composition per deck (4 / 3 / 7 / 3 / 2 on the page) is not asserted against our spawns and generators.
- No test moves Lara through the ship: all AI tests place her.

## 06. Realm of the Dead (Level 1 - Mediterranean Sea, part 5)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/05realm.html
Sub-pages read: details/realm2.html (first shaft), details/realm3.html (second shaft), bugnotes.html (nothing for this section)
Our units: l1_ship_natla_room (start) → l1_ship_lower_hallway → l1_ship_sinking_hall → l1_cine_ship_sinking (chapter 1 end, PROGRESSION.md "Chapter routes")

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Escape from the sinking ship, no time limit: burning corridors, the ship rolling on its side, a slow-motion barrel run, two vertical shafts climbed on bars, valve wheels and ladders, a beam, the exit hatch | PARTIALLY IMPLEMENTED | chapter end passes (`ChaptersReal.MediterraneanEndingRunsThroughTriggerScript`); the escape itself is untested except one ladder attach |
| Movement | Run, sprint (Shift), sideways jump out of the barrels' path | IMPLEMENTED BUT UNVERIFIED (sprint numbers VERIFIED AGAINST ORIGINAL elsewhere) | `Game.SprintStaminaMatchesTru` (Wine, l4_trailstart: 1927 u/s, 9.7 s); nothing in these units |
| Traversal | Bar (ledge) grabs and up-jumps, valve-wheel holds climbed in all directions with up-jumps between groups, ladders with sideways transfers to holds, long bar shimmy round a corner, drop from a bar onto a beam, three up-jumps to the exit | PARTIALLY IMPLEMENTED | ladder attach in this unit: `GameReal.ShipSinkingHallLadder` (ladder 234); stacked ladders 28 / 30 reached by q88 "not done" (GAMEPLAY.md "Ladders…"); hold ↔ ladder transfers undocumented |
| Physics | Ship roll (level geometry changes orientation), loose containers sliding, rolling burning barrels | unknown / NOT IMPLEMENTED for the dynamic parts | no doc or test for sliding containers or barrels in l1_ship_lower_hallway |
| Grapple | n/a | n/a | — |
| Animation | Sprint loop and exhaustion, ladder set, free-climb (DICE) set on valve wheels, beam set, adrenaline slow-motion presentation | IMPLEMENTED BUT UNVERIFIED | clip sets exist (GAMEPLAY.md "Animation mapping", "Free climbing", "Ladders…") |
| Combat | n/a (no enemies after the engine room) | n/a | — |
| Swimming | n/a (the dive is in the closing cinematic) | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Automatic doors, burning barrels, containers, a breaking steam pipe, ladders, valve wheels | PARTIALLY IMPLEMENTED | pipe 1190: `ProgressionReal.BreakPipeSwitchesItsModel` (l1_ship_sinking_hall) |
| Puzzles | n/a (route finding only) | n/a | — |
| Hazards | Fire blocking corridors, smoke, sliding containers, flaming barrels during situational adrenaline, shaft falls | PARTIALLY IMPLEMENTED | fire / death volumes through the `health` natives (SCRIPTS.md); barrels, containers, slow motion: no evidence |
| Collectibles | Page header: health 0, treasures 0, relics 0 | IMPLEMENTED | no reward ids for these units in `chapterRewards(1)` beyond the Mediterranean set |
| Progression | New journal entries (Amanda, Natla); final cinematic (Amanda escapes, gauntlet recovered) → Thailand | IMPLEMENTED | volume 1082 preloads 1005, plane 1074 moves Lara into l1_cine_ship_sinking, whose end requests l2_cliffclimb (PROGRESSION.md); `ProgressionReal.CriticalGates` (1074), `ChaptersReal.MediterraneanEndingRunsThroughTriggerScript`, `GameReal.ScriptedChapterEndChainsStartNextChapter`, `Menus.PdaJournal` |
| Automated tests | Existing: `GameReal.ShipSinkingHallLadder`, `ProgressionReal.BreakPipeSwitchesItsModel`, `ProgressionReal.CriticalGates` (1074), `ChaptersReal.MediterraneanEndingRunsThroughTriggerScript`, `GameReal.ScriptedChapterEndChainsStartNextChapter`, `StreamReal.SaveContinuesBitIdentically` (l1_ship_sinking_hall), `CineReal.ChapterEndCinematics` | Required: situational-adrenaline run, both shaft climbs end to end, hold ↔ ladder transfers, bar → beam drop, ship-roll state | — |

Requirement rows: 14 in [docs/matrix/06_realm_of_the_dead.md](matrix/06_realm_of_the_dead.md) — IMPLEMENTED BUT UNVERIFIED 5, PARTIALLY IMPLEMENTED 4, IMPLEMENTED 3, unknown / n/a 1, NOT IMPLEMENTED 1.

Open in this chapter:

- C06-05 situational adrenaline (scripted slow motion with Lara at real time) and the burning barrels: NOT IMPLEMENTED; it is on the critical path.
- C06-04 ship roll and sliding containers: unknown, nothing documented.
- C06-06..C06-10 valve-wheel / ladder shafts: hold ↔ ladder and ledge ↔ hold transfers are undocumented; stacked ladders (q88) are "not done"; only a floor attach to ladder 234 is tested. Whether the valve wheels are free-climb holds (markup 16-20) or ledges in this unit has not been surveyed.
- C06-11 hang → beam drop untested in this unit.
- No test walks from the Natla room to plane 1074.

## 07. The Remnants (Level 2 - Coastal Thailand, part 1)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/06remnants.html (also the PC demo level)
Sub-pages read: details/remnants2.html … remnants13.html (treasure routes, climbs, chimney jumps, shortcut to treasure 9 and its return), details/swandive.html (Master Swan Diver spot), bugnotes.html (major bug 2 is Wii only: ignored)
Our units: l2_cliffclimb → l2_cliffhall → l2_cliffhall_b → l2_junglehub → l2_hubhall (ends on entering l2_puzzleroom, the Shiva / Kali statue hall). Room-to-unit mapping is [I] from the route order and the enemy survey (tigers ×6 in l2_junglehub, brown spiders in l2_cliffclimb, albino spiders ×6 in l2_hubhall; GAMEPLAY.md "Enemies: Survey").

Reading notes: statuses use the session vocabulary. "Passing" means the test is in the v0.41.0 suite (675 pass per
RULES.md); this agent ran no tests. A status is chapter-specific: a mechanic tested only in another unit is
`IMPLEMENTED BUT UNVERIFIED` here and the generic test is named as "mechanic test". [O]/[I]/[U] as in the repo docs.
Screenshot base: https://tombraiders.net/stella/walks/TR8walk/screenshots/
The page labels its treasures "1/26 … 10/26" in the header note but numbers them n/30 in the text; Coastal Thailand has 30.

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Boat start in a bay with sharks, sea-cliff free climb, jungle path with a pole gap and a grapple gap, ruined colonnade with two chimney jumps, streambed tiger fight, courtyard tiger fight, temple pit with beams, arrival at the Shiva / Kali hall | PARTIALLY IMPLEMENTED | start (boat, bay, ledge, holds) and the arrival in the statue hall played by input (`ch07_cliffclimb_boat_to_cliff`, `ch07_hubhall_statue_hall`); cliff climb verified; chimney jumps exist (tested from the streambed hang in l2_junglehub); the path, colonnade, streambed, fights and temple pit between them are not played by input |
| Movement | Run, sprint from tigers, crouch under roots / branches, long running jumps with a landing roll, slope slides | IMPLEMENTED (sprint, jumps VERIFIED AGAINST ORIGINAL elsewhere); landing roll NOT IMPLEMENTED | `Game.SprintStaminaMatchesTru`, `GameReal.JunglehubRunningJumpReachesHigherLedge` (l2_junglehub), `GameReal.JunglehubVaultOver`; no roll-on-landing code in Player.cpp |
| Traversal | Free-climb holds with gap and up jumps, man-made ledges, column-to-column back jumps, corner shimmies, narrow-ledge stand + sidestep + lean jump, slide → pole, pole-top stand, pole-to-pole, drop onto a pole, perches, pillar, projecting beams, chimney jump from the ground and from a hang, single wall kick, jump extension by holding Jump, swan dive | PARTIALLY IMPLEMENTED | free climb: `GameReal.CliffclimbDiceJumps`, `GameReal.CliffclimbStances`, `GameReal.CliffclimbObstaclesAndLedgeGrabs`, `GameReal.ClimbOnlyAtHolds`, `GameReal.ClimbBadAttach`, `GameReal.CliffclimbIndependentOfRenderRate`; the rest are mechanic tests from other units; chimney / wall kick / swan dive absent |
| Physics | Fall damage on the long drop to treasure 4, water landings, tigers pushing Lara down | IMPLEMENTED (fall curve [O]) | `Combat.FallDamageThresholds`; knock-down by enemies: heavy hit reactions (`Combat.HitReactionThresholds`) |
| Grapple | Ceiling ring over a shallow pit on the jungle path: attach first, running jump, pump, release | IMPLEMENTED BUT UNVERIFIED | mechanic test `Game.GrappleSwingAndRelease`; ring uid not documented |
| Animation | DICE climb set (verified against Wine snapshots on this cliff), swim set, pole / perch / pillar / beam sets, tiger attack / sleep sets, adrenaline head-shot vault, chimney-jump set | PARTIALLY IMPLEMENTED | GAMEPLAY.md "Free climbing" [O Wine reads on l2_cliffclimb]; `GameReal.CliffclimbCameraRecord`; no wall-jump clips in use |
| Combat | Sharks (from the deck or in the water), small spiders that leap on her (shoot or stomp with Interact), tigers ×3 twice, bats at two places; pistols one-handed from a hang, kick, vault off a charging enemy, tranquilizer naps, manual aim on sleeping targets, grenades, adrenaline head shot (Grapple key when locked and close), safe-spot play | PARTIALLY IMPLEMENTED | `AiReal.SharksSpawnSwimBiteAndDie` (l2_cliffclimb sharks), `AiReal.SpidersClingDropAndBite` (l2_hubhall), `AiReal.EnemyIntrosResolveToSkinnedEnemyTypes` (tiger spawnable), `Combat.AdrenalineHeadShot` (mechanic, trigger key ours [I]), `CombatReal.PoolRifleAndTranquilizer`; no kick / stomp / vault, no shooting from a ledge hang, no manual aim |
| Swimming | Bay swim, dive between anemones for treasure 1, climb out on a low ledge, re-entry after falls | VERIFIED AGAINST ORIGINAL (speeds, sprint, turn lag measured in this bay); climb-out IMPLEMENTED BUT UNVERIFIED | `Game.SwimSprintMatchesTru`, `Game.SwimTurnLagsLikeTru` (Wine traces l2_cliffclimb sea), GAMEPLAY.md "Numbers" |
| Vehicle | n/a | n/a | — |
| Objects | Breakable jars (four with treasures), health potions ×6, horizontal poles, grapple ring | PARTIALLY IMPLEMENTED | urns `*_pt_weightbreak` ×48 in l2 (GAMEPLAY.md "Breakables"); Interact smash and jar rewards missing |
| Puzzles | n/a (traversal only) | n/a | — |
| Hazards | Sharks, long falls, lethal drop-off at the courtyard edge, tigers knocking her down, spider swarms, bats | PARTIALLY IMPLEMENTED | see Combat |
| Collectibles | Page header: health 6, treasures 10 (1/30..10/30 of 30 in Coastal Thailand), relics 0 | PARTIALLY IMPLEMENTED | `chapterRewards(2)` has 22 treasures + relic 2588 for all of Thailand (`ChaptersReal.RewardTableMatchesUnits` asserts 22) against the page's 30: jar treasures (5, 6, 8, 10 here) are not collectible |
| Progression | Opening cinematic, outfit and weapon choice before the level (full game), tiger cutscenes, training-text hints, closing cinematic (Bhogavati carvings) | PARTIALLY IMPLEMENTED | `ChaptersReal.StartUnitsAndLevelEnds` (l2_cliffclimb start), `ScriptRealData.CliffclimbVolumeShowsTutorialPopup` (hint_15 → string 1326, volume 4040); outfit choice exists only as our EXTRAS OUTFITS (ROADMAP "Lara outfits": the original's choice screen not reproduced) |
| Automated tests | Existing (in these units): `GameReal.CliffclimbObstaclesAndLedgeGrabs`, `GameReal.CliffclimbDiceJumps`, `GameReal.CliffclimbStances`, `GameReal.CliffclimbIndependentOfRenderRate`, `GameReal.CliffclimbCameraRecord`, `GameReal.ClimbBadAttach`, `GameReal.ClimbOnlyAtHolds`, `GameReal.JunglehubRunningJumpReachesHigherLedge`, `GameReal.JunglehubVaultOver`, `AiReal.SharksSpawnSwimBiteAndDie`, `AiReal.SpidersClingDropAndBite`, `AiReal.EnemyIntrosResolveToSkinnedEnemyTypes`, `ScriptRealData.CliffclimbVolumeShowsTutorialPopup`, `StreamReal.CliffhallBHubhallPair`, `StreamReal.YachtRenderedSeaBelowDeck`, `GameReal.TerrainGroupNativeUpdatesMergedCollision` (l2_hubhall), ctest `game_replay_spider_drop` | Required: three chimney / wall-kick spots, colonnade chains, slide → pole → pole top, tiger fights, hang shooting, jar treasures, temple-pit beams, grapple gap | — |

Requirement rows: 57 in [docs/matrix/07_remnants.md](matrix/07_remnants.md) — IMPLEMENTED BUT UNVERIFIED 32, PARTIALLY IMPLEMENTED 16, IMPLEMENTED 7, VERIFIED AGAINST ORIGINAL 1, NOT IMPLEMENTED 1.

Open in this chapter:

- Chimney / wall jump (C07-24, C07-37, C07-54, C07-56): implemented since 0.42.0 (`src/game/WallJump.cpp`); tested in this section from the streambed hang (C07-37) and at the first wall contact from the floor; the alcove climb (C07-24) and the hubhall wall kick (C07-54) are not played.
- Jar treasures 5, 6, 8, 10 (and 8 of Thailand's 30 overall) cannot be collected; no Interact smash.
- Lara melee (kick, stomp on spiders, vault off a tiger): NOT IMPLEMENTED.
- Shooting one-handed from a ledge hang / while climbing: NOT IMPLEMENTED.
- Adrenaline head shot: trigger key differs from the original (Grapple), no vault, timings [U].
- Tiger encounters (2 + 1, twice) and bat / spider generators are not tested in these units; tranquilizer sleep time [I] (20 s vs "about a minute").
- Landing roll damage reduction (C07-20), jump extension by holding Jump (C07-50), Interact-tap speed traverse (C07-25), swan dive (C07-19): missing or unknown.
- Pole variants used here have no named tests: slide → jump → pole, drop from a hang onto a pole, hang-jump onto a pole, back jump from a hang onto a pillar.
- Colonnade, streambed descent, treasure-9 route and the temple-pit crossing have no tests; the grapple ring gap is untested.
- Pre-level outfit / weapon choice screen is not reproduced.

## 08. Bhogavati (Level 2 - Coastal Thailand, part 2)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/07bhogavati.html (plus its `details/bhogavati2..18.html` screenshot pages, all read)
Our units: l2_puzzleroom (Shiva / Kali courtyard, all three levels), l2_norsehall (rune hallway below the shaft), l2_norsehub (overgrown ruins with the two gates), l2_towerconnector (grey hallway with pits), l2_towerroom (pagoda + tiger), l2_upperhallway (deep pit with beams and poles), back to l2_puzzleroom, exit towards l2_shrineroom (naga-statue yard).
Unit ↔ area mapping is **[I]** (mine): from the enemy survey (GP "Enemies / Survey": nagas ×16 puzzleroom, ×11 norsehub; tiger ×2 + bats ×15 towerroom; spiders ×14 shrineroom), the object names of the gate sweep and the grapple-handle list (GP "Grapple"). `docs/PROGRESSION.md` lists the same units in portal order, which is the reverse of the play order for norsehall → upperhallway.

Abbreviations: GP = docs/GAMEPLAY.md, PR = docs/PROGRESSION.md, Mnn = row of `B_mechanics.md`. "sweep" = `ProgressionReal.GateSweep` run per unit on the v0.41.0 build (2026-11-10, output kept in `matrix/.scratchB/sweep_<unit>.txt`). Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 2 of 4 of Thailand; starts on the wooden lift platform above the courtyard, ends when the naga statue is pulled through the floor of the small yard. 13 health items, 2 key items (the two Shiva gems) | IMPLEMENTED BUT UNVERIFIED | Units stream and the chapter's scripted gates pass in order (`ProgressionReal.CriticalPathPerChapter` ch 2), but the route is driven by teleports between gates, never walked |
| Movement | Run, jump, crouch under stone bars, roll under a bar, stairs, vault onto ledges, sprint to cover | IMPLEMENTED BUT UNVERIFIED | M13, M15, M16 tests are in other units (`Game.CrouchUnderSlabAndRoll`, `GameReal.StairsWalkAndRunUpAndDown`); none in an l2 unit of this section |
| Traversal | Very long ledge / handhold chains: free-climb rock, banded-wall ledges, corner shimmies, back jumps to perches, beams with side traverse, pole swings and pole-top stands, four chimney jumps | PARTIALLY IMPLEMENTED | Chimney jump missing (M06); the rest exists generically (M01-M11) with no test on these units except `GameReal.PuzzleRoomWallHug`, `GameReal.TemplePerchTurnsAndPillarTop` (touch l2_puzzleroom) |
| Physics | Falling lift platform, scale platforms moved by weight, cage as a prop under a scale pan, statue crashing through a floor | PARTIALLY IMPLEMENTED | Lift 2973 moves and carries Lara (`GameReal.ElevatorCarriesPlayer`, `GameReal.ElevatorUberIndependentOfRenderRate`); scale / cage / statue fall: no evidence |
| Grapple | Pull ring (mechanism), swing ×3, rappel ×5, reverse rappel ×1, climb the rope to a ledge, pull-down brackets, mid-fall catch, precision-aimed throw (optional shortcut), statue pull | PARTIALLY IMPLEMENTED | Pulls: `GameReal.GrapplePullsUberTargets` (2982 via volume 3023), sweep 2982 / 2983 PASS; swing / rappel generic (M20, M22); no M36, no M28; naga-statue pull untested |
| Animation | Climb / ledge / pole / perch / chimney sets, pole carry, statue push, grapple pull loops, platform-fall adrenaline pose | PARTIALLY IMPLEMENTED | Clips by name per GP "Animation mapping"; chimney clips unused; no insert clip (GP "Interactions") |
| Combat | Bats (6+ flocks), nagas: 4-5 + 4 + 1 + 1 + 2 + 2 + 2 + 1 + 1 + (3 optional), one tiger, small spiders; safe-spot sniping, shooting while hanging, enemy vault, grenades discouraged | PARTIALLY IMPLEMENTED | `AiReal.NagasClingDropBiteSpitAndDie` (l2_puzzleroom, l2_norsehub), `AiReal.PoachersFireAndBatsBite`, replay `naga_drop`; missing M32, M37; scripted ambush triggers unverified |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Lift switch, carried poles ×2 with sockets, breakable jars, lotus gem holders, gates, scale pans, metal cage, pull-down brackets, two fan statues on a track, crystal, naga statue with ring | PARTIALLY IMPLEMENTED | sweep l2_puzzleroom: 2961 → 2959 / 2960 / 2973 PASS, 2971 push PASS; `ProgressionReal.ReceptaclesTakeTheirPortables` (pole 2967 / holes 2969, 2970), `ProgressionReal.UberReceptaclesTakeTheirPortables` (arms 2059 / 2060, gems 2964 / 2965); fan statues, crystal, lotus holders, naga statue: no evidence |
| Puzzles | (1) free both sides of the lift mechanism; (2) two gems open four gates; (3) right-arm scale: weight → insert gem → weight; (4) left-arm scale with cage + pole weight; (5) mirror statues align two beams on the crystal | PARTIALLY IMPLEMENTED | (1) PR gates ch 2 "pass" + replay `puzzleroom_pole_remove`; (3)(4) gem insert passes, the scales and arm motion by weight are unverified; (2)(5) no evidence |
| Hazards | Fatal falls in the courtyard shaft and the two hallway pits, falling platform, nagas knocking Lara off ledges, poison spit | IMPLEMENTED BUT UNVERIFIED | M65 generic (`Combat.FallDamageDeathAndRespawn`); knock-back off a ledge: no evidence |
| Collectibles | 15 treasures (11/30 - 25/30), 0 relics; 4 of the 15 are in breakable jars (11, 12, 17, 20) | PARTIALLY IMPLEMENTED | Ours: 22 loose `*_rw_artifact` in all of chapter 2 (`game::chapterRewards(2)`, `ChaptersReal.RewardTableMatchesUnits`) vs 30 in the walkthrough: the jar treasures are not collectable (GP "Breakables": urn rewards "untouched" [U]) |
| Progression | Lift drop → rappel → rune hall → gems → gates → pits → pagoda → pit → courtyard mid level → both arms → statues → floor lowers → yard → statue pull opens the shaft | PARTIALLY IMPLEMENTED | Gates table PR ch 2 (2961, 2971, 2982 / 2983, chest volumes 2137 / 2138, handle volumes 2038 / 2039 / 2045): pass; the gem gates, floor lowering and statue pull have no gate test |
| Automated tests | Existing: `ProgressionReal.CriticalGates` (l2_puzzleroom 2961, 2982), `CriticalPathPerChapter` ch 2, `GameReal.GrapplePullsUberTargets`, `GameReal.ElevatorCarriesPlayer`, `GameReal.PuzzleRoomWallHug`, `ProgressionReal.GrappleHandlesSwivel`, `ReceptaclesTakeTheirPortables`, `UberReceptaclesTakeTheirPortables`, `AiReal.NagasClingDropBiteSpitAndDie`, replays `puzzleroom_pole_remove`, `naga_drop`. Required: the scenarios below + a walked route per unit | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 64 in [docs/matrix/08_bhogavati.md](matrix/08_bhogavati.md) — IMPLEMENTED BUT UNVERIFIED 38, PARTIALLY IMPLEMENTED 12, IMPLEMENTED 7, NOT IMPLEMENTED 7.

Open in this chapter:

- **Chimney jump (M06)** is on the critical path three times (C08-05, C08-32, C08-44) and on one treasure route (C08-36). NOT IMPLEMENTED on main.
- **Situational adrenaline (M36)** at the lift drop (C08-14): no scripted slow motion; without it the original's two outcomes (grapple catch / ride the platform crouched) cannot be told apart from an ordinary fall.
- **Naga statue pull (C08-64)** ends the chapter and has no implementation evidence: the object is classified as a passive handle. Needs its script (`OU_PROGRESSION_DUMP=l2_shrineroom:*`) decoded and a gate test.
- **Scales (C08-51, -53, -58, -60)**: gem insertion is tested, but nothing shows the pans moving under Lara's / the pole's weight, the arms following, or the cage holding a pan. UNVERIFIED end to end.
- **Fan statues on the track and the light beams (C08-61, -62; M57, M61)**: no evidence of track movers with a junction, of the reflection condition, or of the floor lowering as its result.
- **Gem gates (C08-24, -35)**: no gate test that taking a gem opens the paired gates; and docs disagree on where gems 2964 / 2965 start (GP "Placed portables" vs PR "Receptacle replays" note "on the upper floor (z 5447)").
- **Jar treasures (11, 12, 17, 20)** cannot be obtained: no kick, urn rewards untouched (M63). Chapter 2 total in ours 22 vs 30.
- **Combat extras**: shooting from a hang (M32, C08-33), enemy vault (M37), spider stomp (M38), precision aim (M28, C08-40), Lara knocked down / off ledges by red nagas: all missing or without evidence. Tiger flee-and-hide unverified.
- **Carried pole auto-drop on a jump (C08-13)** and **pole removal while hanging (C08-12)**: ours needs Interact from an engagement record; behaviour differs or is unknown.
- **No traversal test in any of l2_norsehall, l2_norsehub, l2_towerconnector, l2_towerroom, l2_upperhallway**: every chain C08-21..49 is IMPLEMENTED BUT UNVERIFIED in context.
- Saving grab (C08-26, -28) numbers are [I].

## 09. The Ancient World (Level 2 - Coastal Thailand, part 3)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/08ancient.html (plus `details/ancient2..5.html`; the page also links a gameplay video for the relic route, not viewed)
Our units: l2_shrineroom (shaft under the naga statue, gauntlet door) and l2_maproom (Thor statue room with the rotating hub). Mapping **[I]**: GP "Thor's gear" (l2_maproom volume 9626 gives the left gauntlet after its cinematic, 9628 takes it back), enemy survey (small spiders ×14 shrineroom, ×19 + bats ×12 maproom), grapple anchors 2676 / 2595 in l2_maproom.

Abbreviations as in `08_bhogavati.md` (GP, PR, Mnn = `B_mechanics.md`, "sweep" = per-unit `ProgressionReal.GateSweep`, v0.41.0, 2026-11-10). Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 3 of 4; from the shaft opened by the fallen statue to the exit doorway of the Thor room, where the gauntlet stops working. 0 health items, no key items | PARTIALLY IMPLEMENTED | By input: cutscene + gauntlet, the carved door slid open, the walk to the pedestal, the descent down the hub panel and the grip on it (`ch09_maproom_gauntlet_door`, `ch09_maproom_hub_panel`). Blocked: the way in through the shaft (statue slabs stay solid) and every turn of the hub (known gaps) |
| Movement | Walk / run on walkways, jump gaps in the ring ledge near the hub | IMPLEMENTED BUT UNVERIFIED | M13 generic; no test in l2_maproom |
| Traversal | Descent by three drop-catches (one needs a saving grab), drop-catches down the hub's carved panel, climbing the panel back up, hanging from the walkway side | IMPLEMENTED BUT UNVERIFIED | M03, M04, M01 generic |
| Physics | Rope swing with full pay-out; the hub, walkway and movable wall turn as one body about the room's axis | NOT IMPLEMENTED | no evidence of the rotating structure (sweep l2_maproom shows only volumes) |
| Grapple | Mid-fall catch of a ceiling ring, pay out and swing off; rappel down / up beside the entrance (target choice depends on facing); wall run ×3 (one started from a drop + mid-air throw); swing back from the movable wall's ring | IMPLEMENTED BUT UNVERIFIED | wall-run anchors l2_maproom 2676 / 2595 named in GP "Grapple"; tests are l5 / l1 (`GameReal.ValaskjalfWallRun`, `GameReal.KrakenRoomRappel`) |
| Animation | Gauntlet door slide, panel grab + sidestep, wall drag (push / pull), wall-run set | PARTIALLY IMPLEMENTED | wall-run clips loaded (GP "Grapple" viewed); gauntlet interaction clips: no evidence |
| Combat | Small spider swarms ×2 (shaft mouth, behind the door), one bat flock | IMPLEMENTED BUT UNVERIFIED | `AiReal.SpidersClingDropAndBite` (l3 / l4 units), `AiReal.NagasClingDropBiteSpitAndDie` touches l2_maproom intros; no encounter test |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Gauntlet-moved door, hub panel (grab and walk to rotate), movable wall segment with grapple rings on both faces, pedestal, ceiling and wall rings | PARTIALLY IMPLEMENTED | door 2661 slides with the gauntlet (`ProgressionReal.ThorMechanismsNeedTheGearAndSlide`, `ch09_maproom_gauntlet_door`); hub panel gripped but the hub hinge (2667 joint 1) does not turn (`known_gaps/ch09_maproom_hub_turn`); wall drag, rings on the moving wall: not exercised |
| Puzzles | Rotate the hub to (a) uncover a cache, (b) line the walkway up with a side ledge (treasure), (c) with the exit, (d) half-way for the relic ledge — using the panel at hub level and the wall at the rim | NOT IMPLEMENTED | all four alignments need the hub to turn: blocked (C09-09) |
| Hazards | Fatal fall into the pit under the shaft and round the hub | IMPLEMENTED BUT UNVERIFIED | M65 generic |
| Collectibles | 3 treasures (26/30 - 28/30), 1 relic (2/6; raises max health and heals) | PARTIALLY IMPLEMENTED | relic id 2588 is in l2_maproom (`ChaptersReal.RewardTableMatchesUnits`); pickup rule [I]; relic health bonus: no evidence; treasures 27 / 28 and the relic are unreachable without M60 |
| Progression | Shaft → gauntlet cinematic → slide the door → Thor room cinematic (map, pedestal inscription) → align the walkway with the exit → leave (gauntlet fades cinematic) | PARTIALLY IMPLEMENTED | sweep l2_maproom: 2636 → 2624 (cinematic, larabase 19), 2637 / 2638 → 2667 + 2625 / 2626, 2669 → 2666, 2698 → 2667 PASS; PR gates "l2_maproom volumes 2637/2638 → thor bridge 2667 + cinematics: pass" |
| Automated tests | Existing: sweep volumes above, `ProgressionReal.GrappleHandlesSwivel` (2595), `ChaptersReal.RewardTableMatchesUnits` (l2_maproom), `ScriptRealData.ThorGearDrawGroups`, `StreamReal.ChapterThorGear`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 18 in [docs/matrix/09_ancient_world.md](matrix/09_ancient_world.md) — IMPLEMENTED BUT UNVERIFIED 6, IMPLEMENTED 5, NOT IMPLEMENTED 4, PARTIALLY IMPLEMENTED 3.

Open in this chapter:

- **Gauntlet-powered movers (M60)**: the door slide (C09-05) works by play since 0.44.0; the hub rotation by panel (C09-09, -14) and the wall drag (C09-11, -16) are gripped but do not move (hinge joint 1 of bridge 2667; see the blockers below). Without C09-14 the exit is not reachable by the original route.
- **Rotating walkway / hub / wall as one rigid assembly** (collision, ledges and grapple rings moving with it): no evidence. GP "Grapple" marks 2676's attached instance [U].
- **Wall run in l2_maproom** (three uses, two with a mid-air throw after a drop): generic code only, numbers [I], no test in this unit; rope-length adjustment to a target height is unverified.
- **Mid-fall grapple catch** (C09-03, -15) untested anywhere (M21).
- **Relic** 2/6: pickup counted, but the permanent max-health increase and heal are not evidenced.
- Spider swarms and the bat flock: encounter triggers untested.
- The exit cinematic and gauntlet removal (C09-18) have no test.

## 10. A Puppet No Longer (Level 2 - Coastal Thailand, part 4)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/09puppet.html (plus `details/puppet2.html`)
Our units: tail of l2_maproom (flooded room with the broken bridge — pole 2665 on track 2664, **[I]**), l2_flushtunnel (underwater tunnel, wheel-operated doors), l2_cliffclimb (open sea, the boat). PR "Chapter routes" ch 2: "… l2_maproom → l2_flushtunnel → l2_cliffclimb; flushtunnel plane 2542 → unit script 9502 vt 26 sets volume 2564's BeatLevel sticky; volume 2564 at the boat then plays 2565 → chapter 3".

Abbreviations as in `08_bhogavati.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 4 of 4; flooded room → underwater tunnel → sea → boat; ends Thailand. 0 health items, no key items | IMPLEMENTED | `ProgressionReal.CriticalGates` (l2_flushtunnel 2543; 2542 then 2564 → chapter 3), `CriticalPathPerChapter` ch 2 |
| Movement | Walk on the bridge stubs, running jump off a corner | IMPLEMENTED BUT UNVERIFIED | M13 generic |
| Traversal | Climb out of water onto a low ledge, two slab handholds, shimmy, jump to a wall pole, grab the bridge end from a back jump | IMPLEMENTED BUT UNVERIFIED | `Game.SwimEnterCrossClimbOut` (generic), M01, M07 |
| Physics | Pole sliding down a wall track under Lara's weight, opening an underwater door while she hangs | NOT IMPLEMENTED | GP "Interactions" "Not done: … l2_maproom's track slide" (pole 2665 on `sa_pt_phole_track` 2664) |
| Grapple | Running-jump throw at a wall ring (stepping off drops her in the pool), set rope length, wall run, back jump off the wall to grab the bridge end behind her | IMPLEMENTED BUT UNVERIFIED | M21, M23, M24; anchors l2_maproom 2676 / 2595 |
| Animation | Wall-run set, wall back-jump, pole hang, swim set, underwater wheel turn, climb aboard (cinematic) | PARTIALLY IMPLEMENTED | swim / wall-run clips present; the wheel uses `obj_pull_lever` 2671 from the switch DRM (GP "Interactions"), played standing [I] |
| Combat | One small-spider swarm on the bridge | IMPLEMENTED BUT UNVERIFIED | M42 |
| Swimming | Dive for a treasure in murky water; climb out; swim a tunnel with one ceiling air pocket; pick up a treasure on the tunnel floor; turn a wheel under water to open doors; surface; swim to the boat | PARTIALLY IMPLEMENTED | swim speeds / breath [O Wine] (`Game.SwimSpeedMatchesTru`, `Game.WaterAirDrainsRefillsAndDrowns`); under-water Interact (M30) unverified; open sea: `StreamReal.OpenSeaWater` |
| Vehicle | n/a (the boat is a level-end trigger and cinematic) | n/a | — |
| Objects | Wall ring, weight pole on a track, underwater exit door, air pocket, wheel + double doors, boat volume | PARTIALLY IMPLEMENTED | sweep l2_flushtunnel: `sa_ei_switch_oneway_flushtunnel` 2543 → 2541 PASS; track pole not done |
| Puzzles | Cross the gap by wall run, hold the door open with body weight, then swim through before… (the door stays open once fully lowered) | PARTIALLY IMPLEMENTED | see Physics |
| Hazards | Drowning in the tunnel if the air pocket is skipped | IMPLEMENTED | `Game.WaterAirDrainsRefillsAndDrowns` (4000 frames [O 0x755230]); the air pocket as a surfacing spot: generic water surface, untested here |
| Collectibles | 2 treasures (29/30, 30/30), both under water; 0 relics | IMPLEMENTED BUT UNVERIFIED | loose `*_rw_artifact` pickups (rule [I]); no under-water pickup test |
| Progression | Bridge → wall run → pole lever → underwater door → tunnel → wheel doors → surface → boat → cinematic → Croft Manor | IMPLEMENTED | PR gates ch 2: "l2_flushtunnel switch 2543 → flush gate 2541 pass", "plane 2542, then volume 2564 → 2565 → chapter 3 pass (new gate test)" |
| Automated tests | Existing: `CriticalGates` (two l2_flushtunnel cases), `CineReal.ChapterEndCinematics`, `ChaptersReal.StartUnitsAndLevelEnds`, `StreamReal.OpenSeaWater`, swim tests on l2_cliffclimb (`Game.SwimTurnLagsLikeTru`). Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 12 in [docs/matrix/10_puppet_no_longer.md](matrix/10_puppet_no_longer.md) — IMPLEMENTED BUT UNVERIFIED 9, IMPLEMENTED 2, NOT IMPLEMENTED 1.

Open in this chapter:

- **Weight pole on its track (C10-06, M52)** is the only way to open the underwater exit and is explicitly "not done". Critical path.
- **Wall run + back jump to a ledge (C10-05, M24)**: the original releases the rope and catches the ledge behind; ours documents only a kick-off into a free swing. Critical path, unverified / probably different.
- **Airborne grapple throw and rope release on hitting water (C10-03)**: no evidence either way.
- **Under-water Interact (C10-02, -09, -10; M30)**: treasure pickups and the wheel are untested with Lara swimming; the wheel plays a standing lever clip [I].
- Air pocket (C10-08): depends on water-volume data; not checked in l2_flushtunnel.
- Spider swarm trigger untested.

## 11. Protected by the Dead (Level 3 - Croft Manor)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/10manor.html (plus `details/manor2..11.html`; a relic gameplay video is linked, not viewed)
Our units (PR "Chapter routes" ch 3): l3_caverns_start (tunnels under the crypt) → l3_chapel (chapel, side corridors, stained-glass room) → l3_stairhall (dark stairway) → l3_secretchamber (Richard Croft's study) → l3_chamberrockhall (exit tunnel, door switch) → l3_main_bedroom → l3_bedroom_hall → l3_main_hall (burning manor, tech room); level end plane 4128 → cinematics 160 + 170 → chapter 4.

Abbreviations as in `08_bhogavati.md` (GP, PR, Mnn = `B_mechanics.md`, "sweep" = per-unit `ProgressionReal.GateSweep`, v0.41.0, 2026-11-10). Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Whole of level 3: crypt tunnels, chapel puzzle, study + thrall fight, tunnel back into the burning house, tech-room cinematic. 4 health items, no key items. Outfit and secondary weapon chosen at the start | IMPLEMENTED | `ProgressionReal.CriticalGates` (l3_chapel 4832, 4833, 4834 with switches; l3_chamberrockhall 4617; l3_main_hall 4128 → chapter 4), `ChaptersReal.CroftManorEndingRunsBothCinematics`; the start-of-level choice screen is not reproduced (M75) |
| Movement | Vaults, stairs, roll / sprint through a closing gate, crouch through a low relic niche, flip onto a crate with guns drawn | IMPLEMENTED BUT UNVERIFIED | `GameReal.StairsWalkAndRunUpAndDown` (l3_chapel stairs [O Wine risers]); guns-drawn flip: no evidence |
| Traversal | Vertical pole climb + perch jump, scaffold drops, narrow beams, wall poles (swing, stand, pole to higher pole), handholds to alcoves, column climb with shimmy and up-jump, pillar ledges, stone-beam hang-traverses, back jumps, fireplace climb, glass-wall top | IMPLEMENTED | `GameReal.CavernsPerchJumpReaches` (l3_caverns_start), `GameReal.ChapelHangClimbsOntoBalanceBeam`, `GameReal.BeamFallsCatchScaffoldLedge`, `GameReal.MainHallPoleFlingLandsOnPlatform` [O Wine], `GameReal.RunningJumpReachAndRush` (l3_main_hall); the study and tech-room climbs are untested |
| Physics | Small stone blocks as weights on pressure pads (two per pad), blocks thrown, movable serpent pillar, closing door on a timer | PARTIALLY IMPLEMENTED | plates weighed by loose bodies (`Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`, PR "Remaining blockers"); throw (M54) and gauntlet pillar (M60) missing |
| Grapple | One swing: from the column top to the closing chapel doors; one swing over the burning staircase (prologue route) | VERIFIED AGAINST ORIGINAL | `GameReal.ChapelRingFromTheColumnTop` (ring 4744 offered only from ledge 1008, range data [O]), `GameReal.MainHallRingByItsOwnRange`, `GameReal.SharedQThrowsTheGrappleNotTheCamera`; swing physics [I] |
| Animation | Vertical-pole set, carry (pole: one-handed pistol only), block carry / drop / throw, lever pulls, turnstile push, thrall bone smash, pillar drag | PARTIALLY IMPLEMENTED | pole / perch / switch clips [O] (GP "Interactions"); throw, stomp, carry-while-armed rules: no evidence |
| Combat | Bats (chapel entrance, left upper corridor), many small-spider swarms (ledge, floor, corridors, stairs, hall), two tiger thralls with bone-smash finish / head shot / grenade / knock into the pit; no other enemies | PARTIALLY IMPLEMENTED | `AiReal.SpidersClingDropAndBite` (uses l3_secretchamber), thralls regenerate (`AiReal.ThrallsGetUpUnlessMjolnir`); finishing (M39) missing; `panther_thrall` ×2 placed in l3_secretchamber (GP "Survey") |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Vertical pole, scaffold, jars / urns, 4 small blocks, 3 pressure pads, gates, relic bars, removable wall pole + second socket, furnace, octagon with 4 golden switches, turnstile with two handles, timed double door, tape-recorder desk, serpent pillar, exit switch, burning-house set | PARTIALLY IMPLEMENTED | sweep l3_chapel: switches 4819-4822 → 4837, plates 4832 → 4830, 4833 → 4831 + 4836, 4834 → 4818 + 4830 + 4831 PASS; pole 4823 ↔ hole 4824 (`ReceptaclesTakeTheirPortables`); turnstile and pillar: no evidence |
| Puzzles | (1) two blocks on the corridor pad open the gate and the relic bars; (2) blocks moved to the dais pad keep the furnace lit and both side gates open; (3) switch A ×2 and the opposite switch ×3 light each window's angel and retract the two door bars; (4) crank the door open at the turnstile, timed climb + swing through it | PARTIALLY IMPLEMENTED | (1)(2)(3): `ProgressionReal.ChapelShadowPillars`, `ChapelPillarSectionsPlayTheirClips`, `CriticalGates` l3_chapel 4834 with uses 4819 ×2 + 4822 ×3 → door 4835 — the same pull counts as the walkthrough; (4) turnstile / timed closing unknown |
| Hazards | Fatal drop from scaffold / poles, pit along the study floor, fire in the manor | IMPLEMENTED BUT UNVERIFIED | M65; study pit kill untested; fire damage: none found (M68) |
| Collectibles | 13 treasures, 1 relic (3/6). In jars: 1, 2, 5, 7, 9, 13 (six) | PARTIALLY IMPLEMENTED | ours: 7 loose artifacts {4643, 4644, 4701, 4751-4754} + relic 4757 (`game::chapterRewards(3)`): exactly the 7 non-jar treasures; the six jar treasures are not collectable (M63). Relic reward 4836 appears with plate 4833 (sweep) |
| Progression | Tunnels → chapel → corridor pad → relic → dais pad → switches → timed door → stairway → study cinematic (second gauntlet) → thralls → pillar climb → exit tunnel → switch → burning house → main hall cinematic → climb to the tech room → doppelganger cinematics → Mexico | PARTIALLY IMPLEMENTED | PR gates ch 3 all "pass"; sweep l3_secretchamber 4666 → 4654, 4670 → 4655 PASS; the study exit needs M60; the tech-room climb is untested |
| Automated tests | Existing: `CriticalGates` (4 cases), `ProgressionReal.ChapelShadowPillars`, `ChapelPillarSectionsPlayTheirClips`, `GameReal.ChapelRingFromTheColumnTop`, `ChapelHangClimbsOntoBalanceBeam`, `BeamFallsCatchScaffoldLedge`, `CavernsPerchJumpReaches`, `MainHallPoleFlingLandsOnPlatform`, `ChaptersReal.CroftManorEndingRunsBothCinematics`, `StreamReal.PrologueCorridorToStaircase`, `StreamReal.UberStateSaveLoad` (l3_chapel), `ScriptRealData.ThorGearDrawGroups`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 36 in [docs/matrix/11_protected_by_the_dead.md](matrix/11_protected_by_the_dead.md) — IMPLEMENTED 14, IMPLEMENTED BUT UNVERIFIED 11, PARTIALLY IMPLEMENTED 7, NOT IMPLEMENTED 4.

Open in this chapter:

- **Thrall finishing (M39, C11-26)**: tiger thralls cannot be destroyed with guns in ours (Mjolnir kills; a grenade possibly, untested); the original offers a bone-smash Interact, a head shot, a grenade or the pit. The mandatory fight of this level cannot be completed as designed. No leash / unreachable-Lara behaviour (C11-27).
- **Gauntlet pillar (M60, C11-28, -29)**: the serpent pillar cannot be moved; it is the only route to the exit tunnel. NOT IMPLEMENTED.
- **Throwing blocks (M54, C11-15)** not implemented; carry-state rules (one pistol while carrying a pole, Crouch drops) unverified.
- **Turnstile + timed door (C11-21..23)**: our gate test opens door 4835 from the switch puzzle alone. Whether the crank and the closing timer exist is unknown; the timed climb is untested; door timing not compared with tru.exe.
- **Two-blocks-per-pad rule (C11-13, -16)**: tests step on the plates with Lara; block weights vs the plate's motor are [I] (PR "Remaining blockers").
- **Jar treasures 1, 2, 5, 7, 9, 13** not obtainable (M63): ours 7 of 13.
- **Relic 3/6** health bonus not evidenced.
- Pole removal while hanging on it (C11-10) unverified.
- Level start outfit / weapon choice missing (M75).
- Study pit, fire damage in the manor: no evidence.
- Tech-room climb (C11-35) untested; position of level-end plane 4128 relative to that climb unchecked.

## 12. The Unnamed Days (Level 4 - Southern Mexico, part 1)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/11unnamed.html (plus `details/unnamed2..4.html`, the area map `screenshots/mexico.gif` and the bug note it shares with section 14)
Our units (PR "Chapter routes" ch 4): l4_trailstart (start tunnel, pit, small temple with the lever gate, first road), l4_trailtube (stair tunnel), l4_ballcourt (road fork, first calendar building — the section ends at its cinematic). Mapping **[I]** except the bike (GP "Motorcycle": `motorcycle` intro 5482 in l4_trailstart) and the lever (`ca_ei_lever_grapple_post` 6695 in l4_trailstart).

Abbreviations as in `08_bhogavati.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 1 of Mexico: ride in, fetch the jaguar-head key from the small temple, ride to the first calendar building. 3 health items, 1 key item. Outfit (3) + secondary weapon choice; new journal entries | PARTIALLY IMPLEMENTED | chapter 4 starts in l4_trailstart with the bike (`StreamReal.ChapterStartIntros`, `VehicleReal.TrailstartRideReplay`); key pickup and the calendar cinematic trigger untested; M75 |
| Movement | On foot around the temple and ruins, roll through a low opening | IMPLEMENTED BUT UNVERIFIED | sprint numbers were measured in this unit (GP "Controls": Wine l4_trailstart) → `Game.SprintStaminaMatchesTru`; roll generic |
| Traversal | Pillar-top → slab handholds → building handholds → narrow ledge → up ×3 → shimmy over a gap → light side hop → down to a windowsill; back jump from the sill to a beam; optional chimney inside | IMPLEMENTED BUT UNVERIFIED | generic M01-M03, M10; none tested in l4_trailstart |
| Physics | Bike over a pit (launch boost), stairs on the bike; lever arm pulled down by Lara's weight on the rope, gate rising with it and falling when unloaded | PARTIALLY IMPLEMENTED | bike = Jolt vehicle (`Physics.VehicleDrivesBrakesAndSavesBitIdentically`); lever by rope weight: M26 |
| Grapple | One use: ring on the lever above the window — hang to hold the gate open, set the rope, swing, jump into the doorway | PARTIALLY IMPLEMENTED | `GameReal.GrapplePullsUberTargets` + replay `grapple_pull` (6695 pulled with Interact), `GameReal.GrappleLeverJointIndependentOfRenderRate`; the original's hang-weight + swing-in is not what is tested |
| Animation | Bike mount / dismount / ride / lean, donut, power slide; grapple hang + swing; tranquilizer rifle; item take from a pedestal | IMPLEMENTED | `VehicleReal.BikeIntrosTuningAndClips`, `VehicleReal.LeanFollowsSteeringAtSpeed`, `CombatReal.RifleAndTranquilizerClips` |
| Combat | Two panthers at the small temple (one below, one leaping from a window when Lara nears the bike); tranquilize + manual-aim kill, or run them over (donut / power slide) | PARTIALLY IMPLEMENTED | `VehicleReal.RamsEnemyOnTheTrail`, `VehicleReal.DonutAndPowerSlide`, `VehicleReal.ShootsFromTheBike` (all l4_trailstart), replay `panther_fight`; manual aim missing (M28) |
| Swimming | n/a | n/a | — |
| Vehicle | Motorcycle throughout: drive, brake, reverse to line up, launch boost over a pit, stairs down, tunnel with headlight, twisting track, fork; jars can be smashed with the bike | PARTIALLY IMPLEMENTED | `VehicleReal.TrailstartRideReplay`, `RideIndependentOfRenderRate`, `SaveRestoreMidRide`, `AirControlTurnsMidJump`, replay `bike_ride`; headlight and jar smashing missing (M50); the pit jump is untested |
| Objects | Motorcycle, jars, lever with grapple ring, timed gate, jaguar-head pedestal, low entrance | PARTIALLY IMPLEMENTED | sweep l4_trailstart: pull 6695 PASS, volume 6696 → 6695 PASS, volume 6700 → 6699 condition |
| Puzzles | Gate opened by body weight on a rope, entered by a swing before it drops | PARTIALLY IMPLEMENTED | see Grapple |
| Hazards | Fatal pit at the tunnel end; panthers; (bug-prone left fork is a walkthrough warning, not a mechanic) | IMPLEMENTED BUT UNVERIFIED | M65; bike falling into the pit: `VehicleReal` has crash / fall damage (GP "Motorcycle") untested at this pit |
| Collectibles | 9 treasures (1/50 - 9/50), all in breakable jars; 0 relics | NOT IMPLEMENTED | jar rewards untouched (M63): none of the nine can be collected. Ours lists 10 loose artifacts for all of chapter 4 (`game::chapterRewards(4)`) vs 50 |
| Progression | Tunnel → pit jump → temple approach cutscene → climb in → gate → jaguar head → panthers → road → stair tunnel → right fork → calendar building → cinematic | PARTIALLY IMPLEMENTED | PR gates ch 4 row "l4_trailstart grapple lever post 6695 (pull): pull handler runs; the post slides out on its uber joint … trigger 14"; nothing asserts the gate, the key pickup or the cinematic |
| Automated tests | Existing: `VehicleReal.*` (9 tests, l4_trailstart), `GameReal.GrapplePullsUberTargets`, `GameReal.GrappleLeverJointIndependentOfRenderRate`, `GameReal.PressurePlateIndependentOfRenderRate` (unit load), `PdaReal.SonarMapFrameRevealAndModels`, replays `bike_ride`, `grapple_pull`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 19 in [docs/matrix/12_unnamed_days.md](matrix/12_unnamed_days.md) — IMPLEMENTED BUT UNVERIFIED 8, PARTIALLY IMPLEMENTED 5, NOT IMPLEMENTED 5, IMPLEMENTED 1.

Open in this chapter:

- **Lever gate by rope weight + swing-in (C12-07, -08; M26)**: ours operates lever 6695 with a pull and leaves Lara standing; the original holds the gate open only while she hangs and requires a swing into the doorway against a closing gate. Critical path; the gate object and its timing are not identified.
- **All nine treasures are in jars** → none collectable (M63, M64). Bike-smashes-jar (M50) also missing.
- **Manual aim (M28)** for finishing tranquilized panthers; sleep time [I]; tranquilizer never tested on a real panther.
- **Pit jump with launch boost (C12-03, M48)**: untested; distance / speed not compared with tru.exe.
- **Headlight (C12-16)** not implemented.
- **Jaguar-head pickup (C12-09)**: the key's placement and take are untested; unit of uid 6499 not confirmed.
- Scripted dismount at the approach cutscene (C12-04), panther 2's scripted leap, cinematic trigger at the calendar (C12-19): unverified.
- Short side hop by tapping (C12-06): no evidence.
- Start-of-level outfit / weapon choice (M75).
- No traversal test in l4_trailstart.

## 13. Xibalba (Level 4 - Southern Mexico, part 2)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/12xibalba.html (plus `details/xibalba2..13.html`, incl. the "alternate exit" page `xibalba11.html`, and the map `screenshots/mexico.gif`)
Our units (PR "Chapter routes" ch 4): l4_ballcourt (both calendar buildings, crossroads, ball court, roads), l4_templepath (road + tunnel to the large temple), l4_temple (large temple, outside and inside), and the first metres of l4_staircase_upper (the ramp under the ball court where the timed run ends). Mapping **[I]** except where PR names objects: calendars `ca_uo_calendar` 6506 (takes headdress 6498) and `ca_uo_calendar_b` 5477 (takes mask 6499), pole 6503 / pole lever 6502 / block 6504 / door 6505, temple poles 5240 / 5241 and holes 5229-5233, temple ring 4951, statue head 5236 / plane 5247, reveal 5464.

Abbreviations as in `08_bhogavati.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 2 of Mexico: solve calendar 1, fetch the stone headdress from the large temple, solve calendar 2, timed ride into the opened ramp. 4 health items, 1 key item. New journal entries | PARTIALLY IMPLEMENTED | objects decoded and several tested in isolation (below); calendar solutions, the temple floor break and the timed gate have no end-to-end test |
| Movement | Roll through low openings and under a closing gate, sprint, jump out of a wall hole to the bike | IMPLEMENTED BUT UNVERIFIED | generic M13-M15 |
| Traversal | Outside: pole-top start, pillar handholds, nubby-wall climbs with gap jumps, beams with hang-traverses. Inside: stacked poles, long pole, wet ledges with saving grabs, thin column slide, long free-climb circuit with corner turns, broken-column perches | IMPLEMENTED | `GameReal.TempleBalanceBeamWalkAndJump`, `GameReal.TempleLedgeJumpsUpAndBack`, `GameReal.TemplePerchTurnsAndPillarTop`, `GameReal.PillarAttachRoundUpSlideOff`, `GameReal.StandingJumpCatchesHighLedgeWithinReach` (all l4_temple); `GameReal.ClimbOnlyAtHolds` (l4_ballcourt); the walkthrough's chains themselves are not replayed |
| Physics | Calendar rings on hinges; block toppled off a pillar through the floor; pole lever sinking under Lara and lifting a gate; bike jumps over spike pits on log ramps; timed ramp gate | PARTIALLY IMPLEMENTED | `ProgressionReal.CalendarPushHandlesTurnTheRing`, `BallcourtPoleLeverOpensDoor`, `BallcourtBlockUnderPoleReachesIt`; floor break (class 30) and rope-snag topple not implemented (M70, M27) |
| Grapple | Rappel into the temple's upper floor; rope snagged over a block and tugged to topple it; optional ring climb out (bug-prone in the original) | PARTIALLY IMPLEMENTED | ring 4951 data (range 5689 [O], GP "Grapple"); M27 missing |
| Animation | Calendar handle push, key insert, pole carry / insert / yank, block drag, bike stunts, tranquilizer, saving-grab recover | PARTIALLY IMPLEMENTED | handle / block / switch clips [O] (GP "Interactions"); no insert clip (none in lara.drm) |
| Combat | Panthers: 2 (calendar 1 exit) + 1 (road) + 1 (road 2) + 1 (calendar 2); temple front: 3 panthers + 5 poachers who fight each other (wait → 3 men + 1 cat), a sniper on a ledge; 3 more poachers on leaving; calendar 2: 3 poachers on ledges; bats and small spiders in the temple | PARTIALLY IMPLEMENTED | `AiReal.PoachersFireAndBatsBite`, `AiReal.BallcourtPantherJumpsDownALink`, `GameReal.BallcourtWithEnemiesIndependentOfRenderRate`, `GameReal.KilledPoacherRagdollIndependentOfRenderRate`, replays `poacher_firefight`, `panther_fight`; panther-vs-poacher infighting and steep-angle fire limits: no evidence |
| Swimming | n/a | n/a | — |
| Vehicle | Bike between all sites: log-ramp jumps over two spike pits (one avoidable by a side bank), a third pit on the second road, tunnel, stairs into the ball court, parking for a quick mount, the timed run down the ramp and over a last pit | IMPLEMENTED BUT UNVERIFIED | `VehicleReal.*` are all l4_trailstart; no ride test in l4_ballcourt / l4_templepath |
| Objects | Calendar: 3 concentric rings, jaguar handle (inner + middle), skull handle (outer), key sockets; ball-court blocks and serpent statues; poles ×3 with 5 sockets; pull-topple block; breakable floor; headdress; carved doors with a hole; drag block; golden pole socket + gate; ramp gate | PARTIALLY IMPLEMENTED | `ProgressionReal.UberReceptaclesTakeTheirPortables` (6506 section 0 ← 6498 → state 3; 5477 section 5 ← 6499: 3 → 4), `ReceptaclesTakeTheirPortables` (l4_temple 5240 → 5233, 5241, scriptless 5230-5232), `PhysicsRealData.BallcourtDoorObject`, `BallcourtLeverObject` |
| Puzzles | Calendar (×2): insert key → align middle with outer (jaguar handle) → shift outer (skull handle) to free the inner → align inner with middle → return outer; each alignment clicks | IMPLEMENTED BUT UNVERIFIED | handles turn the ring (`CalendarPushHandlesTurnTheRing`, replay `ballcourt_calendar_handle_turn`: "ring turned on hinge joint 2, section 1 back in 3"); no test reaches the solved state or its cinematic |
| Hazards | Spike pits on the roads (poacher traps), deep pit in the temple, slippery ledges, crossfire | IMPLEMENTED BUT UNVERIFIED | M65 generic; spike-pit kill volumes unverified |
| Collectibles | 19 treasures (10/50 - 28/50), nearly all in jars (17 is loose in bushes; 19 and 23 on pillar tops in jars); 0 relics | NOT IMPLEMENTED | jar rewards untouched (M63); chapter-4 loose artifacts in ours: 10 for all four Mexico sections |
| Progression | Calendar 1 → ball-court blocks move → road with pits → crossroads → (ball-court treasures) → arch → temple front fight → climb in → rappel → topple block → descend → headdress → climb out → door hole → second road → calendar 2 → gate opens → timed ride → ramp → cinematic | PARTIALLY IMPLEMENTED | PR gates ch 4: 6502 / 6504 "pass", receptacles "pass"; 5247 → 5236 "what moves the statue head is [U]"; reveal 5464 opens only in `ProgressionReal.TempleRevealReopens` (from switch 5469, i.e. the later exit) |
| Automated tests | Existing: `ProgressionReal.CalendarPushHandlesTurnTheRing`, `BallcourtPoleLeverOpensDoor`, `BallcourtBlockUnderPoleReachesIt`, `UberReceptaclesTakeTheirPortables`, `ReceptaclesTakeTheirPortables`, `TempleRevealReopens`, `CriticalGates` (l4_ballcourt 6502 Hang → 6505), `GameReal.BallcourtLeverDrivesDoorJoint`, `BallcourtLeverIgnoresAUse`, `BallcourtDormantIntrosBornByScripts`, the l4_temple traversal tests above, `AiReal.*` on l4_ballcourt, replays `ballcourt_calendar_handle_turn`, `ballcourt_pole_lever_block`, `ballcourt_door_lever`, `ballcourt_door_blocked`, `poacher_firefight`, `panther_fight`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 44 in [docs/matrix/13_xibalba.md](matrix/13_xibalba.md) — IMPLEMENTED BUT UNVERIFIED 26, PARTIALLY IMPLEMENTED 10, IMPLEMENTED 6, NOT IMPLEMENTED 2.

Open in this chapter:

- **Block toppled by a snagged rope + breakable temple floor (C13-22; M27, M70)**: NOT IMPLEMENTED and on the critical path (the only way down to the headdress). PR records the statue head's mover as [U]; class-30 breakables are not implemented.
- **Calendar solutions (C13-02..04, -40, -41)**: handles turn a ring, keys insert, but no test reaches "solved", the ball-court cinematics, or the opening of reveal 5464; ring coupling and detents are unasserted.
- **Timed ramp gate (C13-41..43)**: open duration and the re-arm from calendar 1 unknown; the only test opens 5464 from the inside switch.
- **Jar treasures**: 18 of this section's 19 are in jars → not collectable (M63).
- **Wet-ledge saving grabs (C13-23, -28)**: saving grab numbers [I]; no evidence that slick ledges force it.
- **Enemy infighting (C13-11)** and poacher fire-arc limits (C13-33): no evidence. Manual aim (M28) missing.
- **Bike**: no ride / jump test outside l4_trailstart; log-ramp pit jumps, stairs into the court, parked-bike persistence, the final ramp jump are unverified.
- **Pole yank while hanging (C13-14)**, walking along a pole top (C13-19): unknown.
- Headdress pickup at the altar (C13-24): PR notes the route to it is unknown to the autopilot.
- Sweep (v0.41.0, per unit): l4_ballcourt "use 6502 FAIL" is expected (needs the pole; the Hang test passes).

## 14. The Midgard Serpent (Level 4 - Southern Mexico, part 3)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/13midgard.html (plus `details/midgard2..17.html`, incl. the alternate "pool shortcut" page `midgard16.html` and the thrall strategy page `midgard17.html`; bug footnotes read; linked videos not viewed)
Our units (PR "Chapter routes" ch 4): l4_staircase_upper (entrance room with the spike pit), l4_staircase_xibalba (long hallway with three pits), l4_traprooms (Lords of Xibalba statue room + the four trap houses), l4_staircase_mictlan (long dark tunnel), l4_ragnarok_room (carved hall, Xibalba key), l4_pool (Midgard Serpent pool, three levels, Thor statue, belt). Mapping **[I]** except objects PR names: ramps 6045 / 6036, door 5702, seal 5701, plates 5717 / 5718, throne 5891 + scepter 5883, grapple block 5884, crusher volume 5905, key 5695, pool volumes 5566 / 5624 / 5626, ceiling ring 5511, goo volume 5608.

Abbreviations as in `08_bhogavati.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 3 of Mexico: ride down the pit hallway, solve the statue room (four trap houses → four keys → exit below the table), ride the tunnel, lower the poison pool in two steps, turn the screw, reach Thor's belt. 14 health items, 5 key items (4 house keys + Xibalba key) | PARTIALLY IMPLEMENTED | gate scripts largely pass (`ProgressionReal.TraproomsSeal`, `TraproomSealTakesTheRoomKeys`, `HallwayRampsJoint`, pool volumes); the trap houses' hazards, the bike-on-pad step, the serpent-head poles and the screw are unimplemented or unverified |
| Movement | Sprint through opening crushers, crouch under blades, sprint-jump (optional), run past flame jets, walk-over / sprint / jump on a cracking beam | PARTIALLY IMPLEMENTED | sprint [O] `Game.SprintStaminaMatchesTru`; sprint jump and walk-over missing (M14, M19) |
| Traversal | Pole stand + wall-kick to a higher pole; long pole circuits (hand over hand, collinear pole gap, stand, pole-to-pole swings); thin-column hops; chimney jumps ×3; slide down a fallen column with a jump; drop-catch with saving grab; break-away handholds needing a fast shimmy; beams; shield crevice traverse | PARTIALLY IMPLEMENTED | pole work is tested **in this unit**: `GameReal.TrapRoomsPoleFling`, `TrapRoomsPoleParallelHandOverHandAndTurns`, `TrapRoomsPoleAttachBackAndTurnTimes`, `TrapRoomsJumpToPoleStraightLine`, `TrapRoomsPoleTop`, `TrapRoomsPoleSwingStops` (Wine-timed turns per their names / GP "Ladders…"), `GameReal.HangJumpLeans`; chimney (M06), break-away holds (M70), fast shimmy (M18) missing |
| Physics | Ramps raised by a counterweight column; crushers; block dragged by rope; statues turned by their clubs; thrones pushed; table rising on two pads (bike + Lara); poison level dropping; Thor's shield swung by rope; screw turned by rope; cracking beam | PARTIALLY IMPLEMENTED | `ProgressionReal.HallwayRampsJoint`, `LoweredRampSectionCarriesLara`, `GameReal.RampUberCommandMovesCollision`, `GameReal.MountedGrapplePullsBlockWithoutSwinging` (5884), `GameReal.ScriptedWaterLevelsRun`; rest see rows |
| Grapple | Pull a ring block along the crusher aisle; ring climb to a ledge; ceiling swing over the pool (must jump off, not step); pull Thor's shield round; pull the screw from alternating sides; (do not pull the serpent's chin ring: it re-floods) | PARTIALLY IMPLEMENTED | 5884: test above + `CriticalGates` l4_traprooms 5884 Push; pool swing: `GameReal.PoolGrappleTargetAndSwing` (ring 5511); shield / screw / chin ring: no evidence |
| Animation | Bike set; pole / perch / pillar sets; club carry + place; statue turn by club; throne push; crouch-idle under blades with a forced side camera; thrall stomp; flying kick; grapple pull loops | PARTIALLY IMPLEMENTED | clips for poles / pillars / blocks / pulls loaded (tests above); stomp / kick / adrenaline camera missing |
| Combat | Giant spiders: 3 (pit 3), 2 (statue room start), then 1-3 after each house, 1 when the white key is placed; small spiders with each wave; jaguar thrall ×1; Mayan thralls: 1 + 4 + 3 (upper), 2 + 2 (middle), 2 + 2 (shield ledge), 2 (lower) rising from the poison, spitting; finish by stomp / head shot / grenade / knocking back into the pool | PARTIALLY IMPLEMENTED | `AiReal.PoolGeneratorSpawnsThrall` (l4_pool), `AiReal.SpidersClingDropAndBite` (l4_pool spiders), `AiReal.ThrallsGetUpUnlessMjolnir`, replay `thrall_regenerate`; thralls cannot be finished without Mjolnir (M39): the pool fights cannot end; giant-spider behaviour untested (M43) |
| Swimming | n/a (the pool liquid kills on contact) | n/a | GP "Water": l4_pool 5608 kill [O Wine] |
| Vehicle | Bike down stairs and the hallway; three ramp jumps (third after raising its ramps); bike parked on a pressure pad as a weight; down the revealed ramp, more stairs, a stone-ramp jump over a rock pile; left behind at the cave-in | PARTIALLY IMPLEMENTED | ride generic (`VehicleReal.*`, l4_trailstart only); bike as plate weight not implemented (M50) |
| Objects | Collapsible pit ramps + counterweight column; entry door; clubs ×2 and statue hands; 4 turning statues; 4 corner doors; crushers ×6 pairs; ring block; flame gargoyles; spear rows + blade pendulums; 4 keys + coloured niches; 4 thrones; 2 pads; rising table; Xibalba key; 2 serpent heads with nostril poles and chin rings; brittle handholds; weak beam; Thor's shield; screw; belt chamber | PARTIALLY IMPLEMENTED | sweep l4_traprooms: push 5884 PASS; planes 5900 / 5901 / 5904 → 5894-5898 PASS; volume 5905 → 5885-5890 PASS; 5906 → 5884 PASS; volumes 5852 / 5854 → 5703 PASS; plates 5717 / 5718 → 5701 "condition" (alone); **planes 5914 / 5915 → 5702 FAIL, stop at unbound `multibody` fn 84**; l4_staircase_xibalba 6039 → 5702 FAIL (same native) |
| Puzzles | (1) raise the third pit's ramps; (2) two clubs → table rises, corner statues unlock → turn four statues → four doors; (3) four houses: crushers, flames, dark sprint-jump room, spears / blades; (4) four keys in the table → thrones unlock → push four thrones → entry door; (5) bike + Lara on the two pads → table lifts off the ramp; (6) serpent head 1 pole → level drops; (7) descend, swing, swing the shield, serpent head 2 → level drops; (8) screw down → Thor opens → belt | PARTIALLY IMPLEMENTED | (4) `ProgressionReal.TraproomsSeal` (plates' sticky notifications count on seal 5701, both held → vt 58), `TraproomSealTakesTheRoomKeys` (vt 49 tells the four keys apart: 11 dark, 12 blades, 13 fire, 14 crusher); (2) `UberReceptaclesTakeTheirPortables` + replay `traprooms_throne_insert` (5891 takes 5883, state 1 → 2); (1) `HallwayRampsJoint`; (6)-(8): volumes pass, the physical triggers are unverified |
| Hazards | Spike pits; crushers; flame jets (floor, wall, low wall); spear rows; swinging blades; deadly poison pool and stream cracks; brittle handholds; cracking beam; fall damage from alcoves; thrall spit | PARTIALLY IMPLEMENTED | poison [O Wine] (M66); crushers [I / U] (M67); flames, spears, blades (M68, M69), brittle holds / beam (M70): not implemented or no evidence |
| Collectibles | 20 treasures (29/50 - 48/50): 12 in jars (29, 30, 33, 34, 35, 37, 39, 43, 44, 45, 46, 47), 8 loose (31, 32, 36, 38, 40, 41, 42, 48); 0 relics | PARTIALLY IMPLEMENTED | ours, chapter 4: loose artifacts {5488, 5489, 5490, 5770-5776} (`game::chapterRewards(4)`): seven 577x ids fit the seven loose trap-room treasures [I]; jar treasures not collectable (M63); treasure 40 needs a sprint jump (M14) |
| Progression | Entrance → hallway pits → statue room (door shuts) → 4 houses → table → pads → tunnel → key → carved hall cinematics → pool upper → middle → lower → screw → belt cinematic (next section: Land of the Dead, the way back) | PARTIALLY IMPLEMENTED | PR gates ch 4 rows for l4_staircase_upper / xibalba, l4_traprooms, l4_pool: "pass"; my per-unit sweep disagrees on door 5702 (above); `StreamReal.PoolToRagnarokOnFootThenSaveLoad`, replay `pool_to_ragnarok` (streaming on foot) |
| Automated tests | Existing: `ProgressionReal.TraproomsSeal`, `TraproomSealTakesTheRoomKeys`, `HallwayRampsJoint`, `LoweredRampSectionCarriesLara`, `UberReceptaclesTakeTheirPortables`, `ReceptaclesTakeTheirPortables`, `AnimGraphRecords`, `CriticalGates` (l4_traprooms 5884), the six `GameReal.TrapRooms*` pole tests, `GameReal.PoleTraversalIndependentOfRenderRate`, `MountedGrapplePullsBlockWithoutSwinging`, `PushBlockIndependentOfRenderRate`, `ScriptedWaterLevelMoves`, the l4_pool set (`PoolGrappleTargetAndSwing`, `PoolDropToHang`, `PoolSwimReplay`, `PoolRunReplayStaysOnFloor`, `AiReal.PoolGeneratorSpawnsThrall`, `CombatReal.Pool*`, `StreamReal.SaveContinuesBitIdentically`), replays `traprooms_throne_insert`, `pool_to_ragnarok`, `pool_gadgets`, `pool_pickup`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 57 in [docs/matrix/14_midgard_serpent.md](matrix/14_midgard_serpent.md) — IMPLEMENTED BUT UNVERIFIED 26, PARTIALLY IMPLEMENTED 15, NOT IMPLEMENTED 9, IMPLEMENTED 6, VERIFIED AGAINST ORIGINAL 1.

Open in this chapter:

- **Thrall finishing (M39)**: ~17 Mayan thralls + 1 jaguar thrall are met here; in ours none can be destroyed without Mjolnir (not owned until chapter 6): stomp, head-shot kill, grenade kill for Mayan thralls, and dissolving in the pool are missing. Thrall spit: no evidence.
- **Chimney jump (M06)**: critical in C14-27 (razor house) and C14-49 (pool middle level), in C14-14 (crusher tops, the designed way out of the rattling house) and as the wall-kick of C14-04.
- **Trap hazards**: flame jets (M68), spear rows and blades (M69) do no damage / have no timing model; crushers' jaws [U] (M67); the razor house's slow-motion crouch sequences (M36) missing.
- **Break-away handholds and the cracking beam (M70, C14-46, -48)** not implemented; fast shimmy (M18) missing.
- **Bike on a pressure pad (M50, C14-35)**: ours satisfies the two plates with the scepter weight instead; the original needs the bike (the clubs are already in the statues' hands).
- **Door 5702**: per-unit sweep on v0.41.0 shows planes 5914 / 5915 (l4_traprooms) and 6039 (l4_staircase_xibalba) FAIL, stopping at unbound native `multibody` fn 84, while PR lists them as pass. Either a regression or an order dependence — needs a look.
- **Counterweight column (C14-04)**: ramps are script / motor driven in ours; Lara's weight on the handhold as the trigger is not modelled.
- **Serpent-head poles (C14-44, -54)** and the chin-ring reversal: level natives run, the physical trigger is unverified. **Shield** (C14-47, -51) and **screw** (C14-56) pulls: continuous / rotational pulls with the rope kept attached have no evidence.
- **Statue turning by club (C14-10)**, second club + table rise (C14-09), throne pushes → door (C14-34): partly scripted, interactions unverified.
- **Jar treasures** (12 of 20) not collectable; treasure 40 needs a sprint jump (M14).
- **Giant spiders**: no behaviour test (wall climbing after Lara, being shot off, crushed by blocks). Shooting while hanging (M32), enemy vault (M37), flying kick (M38) missing.
- **Bike** in l4_staircase_* / l4_traprooms: no ride or pit-jump test; pole-end jump to a collinear pole (M09) and walking along pole tops unknown; slide-jump to a doorsill (C14-23) untested.

## 15. Land of the Dead (Level 4 - Southern Mexico, part 4)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/14landofdead.html (plus `details/landofdead2.html` alternate relic route, `details/landofdead3.html` relic screenshots, `details/landofdead4.html` climb-out screenshots; all read)
Our units (PR "Chapter routes" ch 4, walked backwards): l4_pool (Midgard Serpent pool: floor, Thor statue, upper walkway) → l4_ragnarok_room / l4_staircase_mictlan (stairs with the two cave-ins; Xibalba key 5695 lies in l4_ragnarok_room per PR) → l4_traprooms (Lords of Xibalba statue room: `195_mex_lords` plays there, GP "Recorded input") → l4_staircase_xibalba / l4_staircase_upper (twisting passage, three pits, log ramps 6036 / 6045) → l4_ballcourt (room with the key socket 6501 and exit switch 5469; `190_mex` on plane 6410; chapter end 5468 → 5483). Area ↔ unit mapping is **[I]** except where a uid is quoted from PR.

Abbreviations as in `08_bhogavati.md`: GP = docs/GAMEPLAY.md, PR = docs/PROGRESSION.md, RM = docs/ROADMAP.md, Mnn = mechanic row (M01-M75 in `B_mechanics.md`, M76+ in `C_mechanics.md`). "sweep" = `ProgressionReal.GateSweep` per unit on the v0.41.0 build (l4 units: `matrix/.scratchB/sweep_<unit>.txt`; l5-l7: `matrix/.scratchC/`). I ran no other test: "IMPLEMENTED" means the named test is in the v0.41.0 suite. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 4 of Mexico. Starts on the drained pool floor wearing belt + gauntlets, ends with the bike jump out of the Xibalba gate and the phone-call cinematic. 0 health items, 0 new key items (the Xibalba key must still be carried or fetched) | PARTIALLY IMPLEMENTED | chapter end passes (`ProgressionReal.CriticalGates` ch 4: 5469 use, 6410, 5468 → chapter 5; `CriticalPathPerChapter` ch 4), reached by teleports; nothing between the pool floor and the exit is walked or ridden |
| Movement | Run, standing jump onto a statue arm, roll under an overhang, roll through a cave-in gap, sprint optional | IMPLEMENTED BUT UNVERIFIED | generic M13, M15 (`Game.CrouchUnderSlabAndRoll`); none on these spots |
| Traversal | Pillar-top jumps (3), ledge grab of the statue stairs, rope climb + swing to a block, rock climb, narrow handhold round a corner, back jump; cave-in mantle; pole → crouch on pole → chimney jump → higher pole → pole across the hall → swing to a column handhold | PARTIALLY IMPLEMENTED | chimney missing (M06); the rest generic (M01, M02, M05, M07, M08, M11); `GameReal.PoolSideJump`, `PoolDropToHang`, `PoolRockGrabNotHurdle` are l4_pool but other ledges |
| Physics | Screw raised by rope pulls; Thor statue re-posing; heavy pillar slid along a floor groove; serpent jaw opened by Lara's weight on the rope; eitr level rising; column segment sinking under her weight and raising two log ramps; bike launch over pits | PARTIALLY IMPLEMENTED | water level scripts (`GameReal.ScriptedWaterLevelsRun` l4_pool), ramp joints (`ProgressionReal.HallwayRampsJoint`); screw, statue, pillar, jaw-by-weight: no evidence |
| Grapple | (1) repeated pull on the screw ring; (2) ring under the upper serpent's chin: hang (weight opens the jaw), climb the rope, turn, swing, jump to a block. The ring only becomes targetable after touching the jutting slab | PARTIALLY IMPLEMENTED | 5511 selectable + swing: `GameReal.PoolGrappleTargetAndSwing`; offered / withdrawn by scripts: PR `unit` 12 / 13 "l4_pool 5511 via 5625" (sweep: 5625 condition, 5634 → −5511); weight-on-rope (M26) and screw pull untested |
| Animation | Thor push of a tall pillar, block pull, roll, rope ascend, perch jumps, relic take, bike mount / launch, key insert, switch pull | PARTIALLY IMPLEMENTED | clips by name (GP "Animation mapping", "Interactions"); no insert clip exists (GP) |
| Combat | Optional: Mayan thralls (2-4) if the walkway is touched (alternate route: 2 + 2 + 2 + 2 with grenades from a pillar); 3 giant spiders in the statue room (shoot, donut, or ignore) | PARTIALLY IMPLEMENTED | `AiReal.PoolGeneratorSpawnsThrall` (5637), `AiReal.ThrallsGetUpUnlessMjolnir`; no stomp / permanent kill without Mjolnir (M39); tarantula behaviour untested (M43) |
| Swimming | n/a (the liquid is lethal) | n/a | — |
| Vehicle | Bike from the first cave-in up to the exit: gap opened by a pushed block, statue room, two log-ramp pit jumps, stop before pit 3, launch boost over pit 3, final launch over the doorway pit onto a ramp | IMPLEMENTED BUT UNVERIFIED | `VehicleReal.*` (l4_trailstart only); no ride, pit jump or unit crossing tested in l4_staircase_* / l4_traprooms / l4_ballcourt |
| Objects | Screw with ring, Thor statue (arms / head / hammer), rune pillar on a groove, serpent head with chin ring, rune block hiding treasures, cave-in block, statue-room door, log ramps, weight-sensitive column handhold, key socket, exit switch | PARTIALLY IMPLEMENTED | sweep l4_pool: 5566 / 5586 / 5587 / 5623 / 5624 / 5626 / 5634 PASS (cinematics, snake heads 5612 / 5613); l4_staircase_upper 4 / 5; l4_staircase_xibalba 6039 → 5702 FAIL (`multibody` fn 84 unbound); no candidate for screw, pillar or rune block |
| Puzzles | Set the level up for the relic: screw fully up (statue lowers its head, hammer hand back) + pillar at the groove end; then leave by re-flooding the pool on purpose; ramp raise by body weight; key + switch at the exit | PARTIALLY IMPLEMENTED | exit switch: `ProgressionReal.TempleRevealReopens` (5469 → 5464 states 0 → 3 → 5 → 6 → 7); key 5695 → socket 6501 "not covered by a teleport test" (PR receptacles row); statue / pillar logic: no evidence |
| Hazards | Eitr: falling stream in the statue doorway, rising pool (timed escape), slip off the statue arm (heavy fall damage); spike / deep pits on the ride; crumbled beam gap; the original's inventory-loss bug is not a mechanic | PARTIALLY IMPLEMENTED | pool kill [O Wine] (GP "Water": 5608 polls larabase 3; `StreamReal.SaveContinuesBitIdentically` goo case); falling eitr stream as a hazard: no evidence; pits generic M65 |
| Collectibles | 2 treasures (49/50, 50/50), 1 relic (4/6), 0 health | PARTIALLY IMPLEMENTED | relic 5496 + treasures 5488-5490 are loose reward intros of chapter 4 (`game::chapterRewards(4)`: 10 treasures + 1 relic vs 50 + 1 on the pages; `ChaptersReal.PickupsAndMusic` picks up in l4_pool); whether 49 / 50 are among them and reachable only after the block pull is unknown; relic health bonus missing (M64) |
| Progression | Pool floor → screw up → pillar → statue arm → relic → pillars → slab touch → chin ring → flood → block → rocks → upper walkway → rune block → stairs → cave-in 1 (key) → bike → cave-in 2 block → statue room → pits 1-2 → poles / column → pit 3 → key + switch → doorway pit → cinematic | PARTIALLY IMPLEMENTED | gates PR ch 4: l4_pool volumes 5566 / 5624 / 5626 pass, staircase 6046 / 6076 / 6078 pass, ballcourt 5469 + 6410 + 5468 pass; relic set-up, flood escape, key insert, ramp-by-weight not asserted |
| Automated tests | Existing: `GameReal.PoolGrappleTargetAndSwing`, `GameReal.ScriptedWaterLevelsRun`, `AiReal.PoolGeneratorSpawnsThrall`, `ChaptersReal.PickupsAndMusic`, `ProgressionReal.HallwayRampsJoint`, `LoweredRampSectionCarriesLara`, `TempleRevealReopens`, `CriticalGates` / `CriticalPathPerChapter` (ch 4), `StreamReal.PoolToRagnarokOnFootThenSaveLoad`, `GameReal.StairsWalkAndRunUpAndDown` (l4_staircase_mictlan), replay `pool_to_ragnarok`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 21 in [docs/matrix/15_land_of_the_dead.md](matrix/15_land_of_the_dead.md) — IMPLEMENTED BUT UNVERIFIED 13, PARTIALLY IMPLEMENTED 5, NOT IMPLEMENTED 3.

Open in this chapter:

- **Statue / screw / pillar set-up (C15-02, -03)**: no evidence that the screw (5611) can be wound by grapple pulls, that the statue re-poses, or that the rune pillar can be pushed; the relic (C15-05) and the quick way out (C15-07) depend on all three. Statue head 5606's handler stops at an unbound native (`unitref` fn 1) in every sweep line.
- **Weight on the rope opens the serpent's jaw (C15-09; M26)**: not modelled; this is the main route's only way to re-flood and leave.
- **Chimney jump to the higher pole and the weight-sensitive column handhold (C15-18; M06)**: missing; our ramp rises on a plane crossing instead.
- **Trap door / statue-room door 5702 from l4_staircase_xibalba plane 6039**: FAIL in the sweep (`multibody` fn 84 unbound) although PR's table says pass.
- **Bike**: no test rides outside l4_trailstart; four pit jumps (two ramp, two launch-boost) and three unit crossings are unverified; bike position carried over from section 14 unknown.
- **Xibalba key → socket 6501 → switch 5469** chain is untested; 5469 is only tested with the harness setting its sticky.
- Falling eitr stream as a kill volume (C15-04); thrall spawn trigger on the walkway (C15-08); puzzle state after death (C15-06); Thor-pull block and treasures 49 / 50 (C15-12): no evidence.
- Collectibles: chapter 4 total is 10 + 1 in `chapterRewards(4)` against 50 + 1 on the pages (jar treasures, M63); relic health bonus missing.
- Section start state (drained pool, open statue arm, belt worn) cannot be produced without playing section 14: a save / state fixture is needed for any test here.

## 16. Gate of the Dead (Level 5 - Jan Mayen Island, part 1)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/15gateofdead.html (plus `details/gateofdead2..5.html`: first gate lock and the three tower climbs; all read)
Our units (PR "Chapter routes" ch 5): l5_startsnow (arrival cinematic `230_val_intro`, bike 7528), l5_valgrind_tunnel (serpentine tunnel, bike 7229, Thor slide door 7400 `ar_ei_thor_slide_door`, cinematic 7403 `235_val_shaft`), l5_valgrind (spiral ramp, Valgrind gate 7334 `ar_ei_thor_valgrind_door`, cinematic 7464 `240_val_gate`, mechanical tower: cog wheel 7312 + rings 7492-7509 `ar_uo_valgrind_column_floor{a,b,c}_ring*`, lock glyphs 7330 / 7326 / 7323 / 7319, beams `fx_beam_val_door_unlock` 7331 / 7327 / 7324 / 7320). uids from `ProgressionReal.Survey` (`matrix/.scratchC/survey_all.txt`) and GP; area ↔ unit mapping [I].

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 1 of Jan Mayen: ride down the tunnel and the spiral ramp, fight Viking thralls, start the tower and turn its four tiers so four light beams unlock the gate; the 4th beam ends the section. 5 health items; outfit choice (snow heavy / light) + secondary weapon | PARTIALLY IMPLEMENTED | chapter 5 start riding 7229 after `230_val_intro` (`StreamReal.ChapterStartIntros`, GP "Chapter starts vs the original" [O Wine]); sweep l5_valgrind 3 / 6 (7471, 7472, 7511 PASS); the tower puzzle and climb are untested; M75 |
| Movement | On foot round the tower base and tiers, sprint away from thralls, hop onto a block out of reach | IMPLEMENTED BUT UNVERIFIED | generic (M13, M14, M16); no locomotion test in an l5 unit |
| Traversal | Three tower climbs on **rotating** rings: timed straight-up jumps between moving handholds and moving poles, stand on a handhold / pole, pole walk towards the tower, hand-over-hand outward, stacked static handholds, grab a tier ledge from a pole top | NOT IMPLEMENTED (no evidence) | moving markup (M76): no doc, test or source mention of ledges / poles carried by the ring objects; static equivalents exist (M01, M02, M07, M08, M80) |
| Physics | Bike on a banked spiral with holes; tower rings turning at different speeds; tier turned by walking a panel round; steam blast impulse | PARTIALLY IMPLEMENTED | cog wheel 7312 turns and takes a gem while turning (replay `valgrind_cog_gem_insert`, `ProgressionReal.UberReceptaclesTakeTheirPortables`); ring motion, steam: no evidence |
| Grapple | n/a | n/a | — |
| Animation | Bike ride, Thor door slide, block carry + place in a niche, panel push walking sideways, pole-top stand / walk, saving-grab recover, thrall stomp | PARTIALLY IMPLEMENTED | bike clips `VehicleReal.BikeIntrosTuningAndClips` (l5 intros), push-handle clips (GP "Interactions": push handles, "the valgrind cog wheels' section 0"); stomp missing (M39) |
| Combat | Viking thralls: 1 crouched on a block at the top, 3 down the ramp, 4 at the gate after the cinematic (grenades, bike donut / ramming, or snipe from a block they cannot climb; they spit at range); every downed thrall must be stomped | PARTIALLY IMPLEMENTED | `AiReal.ThrallsGetUpUnlessMjolnir` (l5_valaskjalf_entry vikings: get up after 15 s unless Mjolnir), `AiReal.DeathTimerAndThrallChunks`; `viking_thrall` ×12 placed in l5_valgrind (GP "Survey"); no stomp, no permanent kill before Mjolnir (M39); spit attack: thralls have no attack records decoded (GP "Enemy data": "Thralls: none") |
| Swimming | n/a | n/a | — |
| Vehicle | Bike: tunnel descent, through the slid door, spiral ramp at speed (holes on the outer side, full-width gaps passed on the banked inner wall), ramming thralls, donut at the gate; optional drop through a hole as a shortcut (fall damage if moving) | IMPLEMENTED BUT UNVERIFIED | bike generic (`VehicleReal.*` in l4_trailstart); l5 intros decode (`VehicleReal.BikeIntrosTuningAndClips`); no ride in l5_valgrind_tunnel / l5_valgrind; wall-banking and the bike respawn position after death untested |
| Objects | Thor slide door, breakable jars, 3 carved power blocks + niches, base panel and 3 tier panels (rune-lit on touch), rotating rings with handholds / poles / steam vents, 4 light beams, gate | PARTIALLY IMPLEMENTED | gems 7498-7500 `ar_pt_valgrind_power_gem` → cog wheel 7312 sections 1-3 (GP "Interactions"); door 7400: no gate candidate in the sweep; rings / glyphs load as scripted uber objects (survey) with no test |
| Puzzles | Power the tower (3 blocks in 3 niches) → turn the base and each of the three tiers by its panel until that tier's beam hits the ground pattern (any order); 4 beams open the gate locks | PARTIALLY IMPLEMENTED | one gem insert + state change tested (7312 section 1: 0 → 1); turning to alignment, beams, lock count and the section change are not asserted |
| Hazards | Holes and gaps in the ramp (fatal fall), steam vents on tier 3 (scald + knock off; saving grab), long falls off the tower, thrall spit; the original's "bike left at the bottom after a reload" bug is not a mechanic | NOT IMPLEMENTED (steam) / IMPLEMENTED BUT UNVERIFIED (falls) | no hit for steam in `src/game`, `src/script`, GP; falls M65; saving grab M04 |
| Collectibles | 6 treasures (1/30 - 6/30; five in jars: 1, 2, 3, 4 and one of 5 / 6), 0 relics, 5 health | PARTIALLY IMPLEMENTED | `chapterRewards(5)`: 10 loose treasures + relic 7280 for all of Jan Mayen vs 30 + 1 on the pages; jar rewards not spawned (M63); which of ours lie in l5_valgrind unknown (candidates 7273-7276, 7430 by uid range [I]) |
| Progression | Cinematic → tunnel → slide door → shaft cutscene → ramp → gate cinematic + 4 thralls → blocks → base panel → climb 1 → panel → climb 2 → panel → climb 3 (steam) → panel → section ends | PARTIALLY IMPLEMENTED | PR gates ch 5: "l5_valgrind volumes 7471 / 7511 → door 7334, cinematic 7464: pass" (a volume opens 7334 in the sweep, with no lock condition asserted); plane 7472 → 7403 PASS; plane 7439 → gems 7498-7500 "condition" |
| Automated tests | Existing: `StreamReal.ChapterStartIntros`, `VehicleReal.BikeIntrosTuningAndClips`, `ProgressionReal.UberReceptaclesTakeTheirPortables` (l5_valgrind 7498 → 7312), replay `valgrind_cog_gem_insert`, `AiReal.ThrallsGetUpUnlessMjolnir`, `CriticalPathPerChapter` ch 5, `GameReal.UnitWeatherSurvey`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 23 in [docs/matrix/16_gate_of_the_dead.md](matrix/16_gate_of_the_dead.md) — IMPLEMENTED BUT UNVERIFIED 9, NOT IMPLEMENTED 8, PARTIALLY IMPLEMENTED 3, IMPLEMENTED 2, VERIFIED AGAINST ORIGINAL 1.

Open in this chapter:

- **Rotating-ring climbing (C16-16, -18, -20; M76)**: the whole second half of the section is timed jumps between handholds and poles that move with the tower rings. No evidence that ledge / pole markup can ride an object in ours. Critical path.
- **Steam vents (M77)** and hazard-triggered saving grabs: not implemented.
- **Thor slide door 7400 (C16-04; M60)**: no interaction candidate; critical path (blocks the tunnel).
- **Thrall stomp / permanent kill before Mjolnir (M39)**: missing; eight thralls on this section's route.
- **Tower puzzle logic**: only one gem insert is tested. Three gems → rings start, panel alignment → beam, four beams → gate: unasserted; the sweep opens gate 7334 from volume 7511 alone.
- **Bike on the spiral ramp**: banked-wall gap crossing, holes, drop shortcut, respawn with / without the bike (spawner 6715 not instantiated): untested.
- Thrall ranged spit: no decoded attack record ("Thralls: none").
- Five of six treasures are in jars (M63); shiny-jar hint missing.
- Start-of-level outfit / weapon choice (M75).

## 17. Valhalla (Level 5 - Jan Mayen Island, part 2)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/16valhalla.html (plus `details/valhalla2..11.html`, the maze map `screenshots/valhalla.gif` and the hammer-room map `screenshots/valhalla2.gif`; all read)
Our units (PR "Chapter routes" ch 5): l5_valgrind (tower descent, gate 7334) → l5_valaskjalf_entry (maze half next to the gate: relic room door 7227 `ar_uo_reward_door` with inside switch 7228) → l5_valaskjalf_entryb (maze half with the pole intersection: fixed pole lever 7188 `ar_uo_wall_lever`, pole socket lever 7189 `ar_uo_phole_lever` + loose pole 7190, treasure-room door 7187 with inside switch 7192) → l5_valaskjalf (hammer room: hammers `ar_ani_thor_hammer_three` 6947, weights `ar_ani_weight_pair` 6951, chains `ar_ani_infinite_chain` 6939-6944, shafts `ar_rb_hpoles_on_gears` 6982-6984, bars `ar_rb_hpole_hammer_release` 6979-6981, jammed gear 6985, falling bridge 6974, falling floor 6975, falling columns 6976-6978, lying yeti 7036, wall-run anchors 6750 / 6751, level end 6866 → 6846 `250_val_hammer`). uids: PR gates ch 5, `ProgressionReal.Survey` (`matrix/.scratchC/survey_l5_valaskjalf.txt`), GP "Grapple". Which door is which follows from the links (7189 → 7227, 7188 → 7187) and the map; room numbering ↔ uids [I].

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 2 of Jan Mayen: climb down the tower, slide the gate, clear the thrall maze (two timed bike runs for treasure 21 and the relic), cross the hammer room through seven side rooms, take Mjolnir. 12 health items | PARTIALLY IMPLEMENTED | level end passes (`ProgressionReal.CriticalGates` l5_valaskjalf 6866 → chapter 6; Mjolnir granted by 6846's end handler, GP "Mjolnir" [O]); `CriticalPathPerChapter` ch 5 teleports between gates |
| Movement | Run, sprint over collapsing walkways, crouch / roll through low gaps, jump over and duck under rollers, running jumps between walkways | PARTIALLY IMPLEMENTED | generic M13-M15; collapsing floor not implemented (M70) |
| Traversal | Moving-pole drops down the tower; chimney jumps ×4 (one against a moving weight, twice); column circuits with corner shimmies, ledge stand + side jump to re-grab, beams, slide down fallen columns, bar-to-bar swings, hammer / weight / shaft / chain rides, wall run, rappel swing to a handhold | PARTIALLY IMPLEMENTED | in this unit: `GameReal.ValaskjalfWallRun` (anchor 6750), `GameReal.ValaskjalfRailSlideTraversesAndDetaches` (rail 1562), `GameReal.WallHugRoundsConcaveAndConvexCorners` (convex corners 1494…); missing M06, M76, M78, M18 |
| Physics | Hammer strike breaking a walkway, tilting walkway slide, cracking handhold, snapping pole, counterweights sinking under Lara / rising, gears freed by pulling out a wedge block, chains carrying her, pole lever sinking under her weight with a door timer | PARTIALLY IMPLEMENTED | `ProgressionReal.ValaskjalfChainsDetach` (jammed gear 6985 vt 34 starts chains 6943 / 6944; volumes 7025 / 7026 detach), `CriticalGates` 7010 → falling bridge 6974 (multibody 59), `PoleHoleLeverOpensRewardDoor`; PR: "the hpoles' own motion is not modelled"; class-30 breakables not implemented (GP "Breakables") |
| Grapple | Room 4: ceiling ring, rappel, set length, wall-run arc, jump to a handhold. Room 5: wall ring, running jump onto the rope, climb, wall run, jump to a handhold. Hammer room: ring under a walkway, wall run, jump to a small ledge | PARTIALLY IMPLEMENTED | hammer-room wall run tested (`GameReal.ValaskjalfWallRun`, replay `valaskjalf_wallrun`, numbers [I]); rooms 4 / 5: anchor 6751 exists (GP "Grapple" anchor list), untested; a wall-run jump that ends in a ledge grab is untested (M24) |
| Animation | Bike donut, pole carry / insert, block drag, adrenaline slide, fast shimmy, hammer cling and side-climb, weight cling, shaft squat, chain hand-over-hand, roller duck, one-handed pistols on a column, kick, Mjolnir pick-up cinematic | PARTIALLY IMPLEMENTED | bike / pole / block clips present (GP); no clips wired for M06, M18, M32, M38, M78 |
| Combat | Viking thralls: maze 1 + 2 + 2 + 2 + 1 + 3 + 1 (ledge) + 4 in the relic room; 2 at the hammer-room entrance; pool spawns in rooms 1 and 5 (shotgun them back into the eitr); 1 on a walkway (kick it off); bats (maze ×2, rooms 1, 3, hammer ride); small spiders (treasure-21 room); giant spiders (hammer room columns ×3+, rooms 2, 3, corridors); one Yeti thrall (cinematic wake-up; SMG circle-strafe, adrenaline head shot, enemy vault, grenade finish) | PARTIALLY IMPLEMENTED | types present: `viking_thrall` ×7 + ×7 + ×4 + 2 pool generators, `yeti_thrall` ×1, `giant_albino_tarantula` ×14, `small_albino_spider` ×10, bats ×35 (GP "Survey"); `AiReal.ThrallsGetUpUnlessMjolnir` (l5_valaskjalf_entry); missing M39 (stomp, knock into eitr), M32, M37, M38; yeti and tarantula behaviour untested |
| Swimming | n/a (blue pools are lethal) | n/a | — |
| Vehicle | Bike through the maze: ramming / donut on thralls, rock-slab ramp jump, parked as a timed-run start, two timed runs (straight ≈ short; 8-turn route within ≈ 35 s), bike relocation on reload | IMPLEMENTED BUT UNVERIFIED | `VehicleReal.*` (l4 only); no ride in l5_valaskjalf_entry / entryb; PR notes Lara's standing jump cannot reach the inserted pole (bike / block position matters) |
| Objects | Sliding gate, jars, movable block, wall pole lever, loose pole + socket, two timed stone doors with inside switches, low rubble gaps, hammers, tilting walkway, cracking handhold, snapping pole, weights (banded / top-band), gear shafts, chains, roller sets (floor, ceiling, tracking), bars, jammed-gear block, yeti corpse, Mjolnir pedestal | PARTIALLY IMPLEMENTED | sweep: 7228 → 7227 PASS, 7188 → 7187 PASS, 7192 → 7187 PASS, 7189 use FAIL alone (needs the pole: `PoleHoleLeverOpensRewardDoor`), l5_valaskjalf 7010 / 7014 / 7027-7030 / 6866 / 6868 PASS, 7025 / 7026 FAIL alone, 6808 condition, 7105 / 7106 encounter (generators 7124 / 7125) |
| Puzzles | (1) wall pole + block + bike staging → timed door; (2) carry pole, insert from the block, re-stage, hang, timed 8-turn ride; (3) free the jammed gear to start the third hammer; route puzzle through rooms 1-7 | PARTIALLY IMPLEMENTED | (2) lever logic `ProgressionReal.PoleHoleLeverOpensRewardDoor`, replay `valaskjalf_pole_lever_insert` (hang not replayed); (3) `ValaskjalfChainsDetach` (how Lara frees 6985 is not shown); timers unmeasured |
| Hazards | Steam vents on the tower descent, bottomless hammer room, collapsing walkways (×3), cracking handhold, snapping pole, hammer blows, descending weights (crush), rollers, eitr pools, thrall spit knocking her off a ledge, yeti blows knocking her off walkways | PARTIALLY IMPLEMENTED | eitr boxes kill where a script polls (GP "Water" l4_pool; the l5 pools untested); roller / crush / hammer damage: no evidence (B's M67-M69 state trap damage missing); falls M65 |
| Collectibles | 24 treasures (7/30 - 30/30), 1 relic (5/6), 12 health | PARTIALLY IMPLEMENTED | `chapterRewards(5)` = 10 loose treasures (6740-6742, 7167, 7168, 7273-7276, 7430) + relic 7280 vs 30 + 1; at least 9 of this section's 24 are stated to be in jars (7, 9, 15, 19, 22, 23, 24, 29, 30), not spawned (M63) |
| Progression | Tower descent → gate → maze (areas 1-9) → [timed door 10 / treasure 21] → [timed door 11 / relic] → chimney out at 12 → hammer room entrance → adrenaline slide → columns → room 1 → room 2 (weights, chain) → rollers → hammer 2 → rollers → room 3 (free gear) → weight climb → rollers → hammer 3 → room 4 → slide / columns → [room 5 → 6] → wall run → room 7 (yeti) → chain against the flow → rollers → hammer 4 → collapsing walkway → Mjolnir | PARTIALLY IMPLEMENTED | PR gates ch 5 rows (7228, 7188, 7189, 7025-7030, 7010, 6866); `CriticalPathPerChapter` ch 5 |
| Automated tests | Existing: `ProgressionReal.CriticalGates` (7228, 7188, 7010, 6866), `PoleHoleLeverOpensRewardDoor`, `ValaskjalfChainsDetach`, `GameReal.ValaskjalfWallRun`, `ValaskjalfRailSlideTraversesAndDetaches`, `WallHugRoundsConcaveAndConvexCorners`, `AiReal.ThrallsGetUpUnlessMjolnir`, `CineReal.ChapterEndCinematics`, `ChaptersReal.StartUnitsAndLevelEnds`, replays `valaskjalf_wallrun`, `valaskjalf_pole_lever_insert`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 70 in [docs/matrix/17_valhalla.md](matrix/17_valhalla.md) — IMPLEMENTED BUT UNVERIFIED 23, PARTIALLY IMPLEMENTED 21, NOT IMPLEMENTED 20, IMPLEMENTED 6.

Open in this chapter:

- **Chimney jump (M06)** is required six times here (C17-28, -35, -38, -53, -60, -67), twice as a wall-kick onto a moving weight. Without it the maze cannot be left.
- **Rideable machinery (M78) and moving markup (M76)**: tower poles / handholds, counterweight bands, gear shafts, chains (also against their direction), four swinging hammers. Only the scripts around them are covered (chains start / detach, weights animate); PR itself says the shafts' motion is not modelled.
- **Break-away geometry (M70)**: cracking handhold, snapping pole, three collapsing walkways, the tilting bridge slide — class-30 breakables are "Not implemented"; only bridge 6974's joint break is tested.
- **Situational adrenaline (M36)** at the bridge slide: no time-scale support.
- **Faster traverse by tapping Interact (M18)**: required on the cracking handhold and on the reverse chain.
- **Timed doors**: 7188 is opened by a plain use in ours (the original needs her weight on the wall pole); neither door's timer (≈ 35 s for 7227) is measured; no bike test in either maze unit. The page supplies the missing route to pole 7189 (block under the socket) that PR lists as [U].
- **Thralls**: no stomp, no kick, no enemy vault, no "knocked into eitr / pit = destroyed", no spit (M38, M37, M39); before Mjolnir no thrall can be put down for good, and this section has ~25 of them plus the yeti.
- **Hazards without damage**: rollers, descending weights, hammer blows, steam (M79, M67, M77); eitr pools in l5 untested.
- **One-handed pistols while hanging (M32)**; saving grab after long jumps (M04) unverified; wall-run jump into a ledge grab (M24) untested.
- **Gate 7334** opens from a volume in the sweep; the Thor slide by Lara is not modelled.
- Collectibles: 10 loose treasures for the whole chapter vs 30; the jar treasures are missing.

## 18. Rituals Old (Level 6 - Andaman Sea)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/17andaman.html (plus `details/andaman2.html`: container climb screenshots; read)
Our units (PR "Chapter routes" ch 6): l6_ship_exterior (bow deck, container deck, upper deck, helipad, stern; start cinematic 7652 `260_ship_intro`) → l6_ship_upper_hallway_a / _b (stairs and halls behind the stern doors 7642 / 7643 / 7621) → l6_ship_upper_hallway (door 7589) → l6_ship_engine_hall (engine room, crate corners, T-junction; plane 7565) → l6_ship_natla_room (cinematic 1387 → l7_thin_ice). uids from PR gates ch 6 and `ProgressionReal.Survey` (`matrix/.scratchC/survey_l6_ship.txt`).

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Single-part level: cross Amanda's second ship from bow to stern with Mjolnir, go below, reach Natla's cell (cinematic). 3 health items, no treasures, no relics. Outfit choice (3 jungle outfits) + secondary weapon | PARTIALLY IMPLEMENTED | start on the deck after `260_ship_intro` (`StreamReal.ChapterStartIntros`); end `ChaptersReal.NatlaShipEndingPassesLoadedGuard`, `ProgressionReal.CriticalGates` {l6_ship_engine_hall, 7565}; `CriticalPathPerChapter` ch 6; the fights are untested in l6 units |
| Movement | Run, stairs, cover behind crates | IMPLEMENTED BUT UNVERIFIED | generic (`GameReal.StairsWalkAndRunUpAndDown` elsewhere) |
| Traversal | One climb: container top → jump to grab a container stack → running jump to a triple stack → across container tops to the upper deck; alternative chimney jump between close stacks | IMPLEMENTED BUT UNVERIFIED | generic M13, M01; chimney alternative missing (M06, optional) |
| Physics | Crates and fuel tanks destroyed by bolts / shockwave; enemies thrown (off the deck) by blasts; ship in a storm | PARTIALLY IMPLEMENTED | crate break by a class-13 hit: `ProgressionReal.CombatCrateBreaksOnHit` (l1_ship_exterior crate 1670 `sh_ei_combat_crate_a`); barrel blast not modelled (GP "Breakables"); pound knock: "speed / angle unused" (GP "Mjolnir") |
| Grapple | n/a | n/a | — |
| Animation | Hammer equip / unequip, three aim poses, throw, ground pound; merc cover / fire / death clips | IMPLEMENTED | GP "Mjolnir" clip list [O]; `Combat.MjolnirCycleSwingAndGroundPound`; merc clips via `AiReal.MercsGuardZonesCoverAndAlert` (l1) |
| Combat | Mjolnir tutorial (bolt, lock, ground pound = Target Lock + Interact, precision aim at crates / tanks). Mercenaries: 5 on the container deck (+ respawns while lingering, also behind on the bow deck), 2 behind crates on the upper deck, 2 at the grate + 2 up the stern stairs (+ 1 late), 3 at the helipad, 3 at the stern (+ 3 on backtracking), below: 2 engine room, 2 behind crates, 1 at the stairs, 2 at the T | PARTIALLY IMPLEMENTED | Mjolnir bolt / pound / cycle (`Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings` [O]); merc AI (`AiReal.Mercs*`, `AiReal.ShipNavGraphFlankAndInvestigate`, replay `merc_squad`: all on l1_ship_exterior); l6 has `merc` ×2, `merc_deux` ×3, 23 + 6 generators (GP "Survey") — generator / respawn rules and precision aim (M28) unverified / missing |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Breakable wooden crates (one hides a first aid kit), fuel tanks, cargo containers, stairs, automatic doors, grate, helicopter (prop) | PARTIALLY IMPLEMENTED | doors: `ProgressionReal.ChapterSixShipDoorsOpen` (7642 / 7643 / 7621 have no script: they stand open); crate reward: no evidence |
| Puzzles | n/a (route finding on the container deck only) | n/a | — |
| Hazards | Gunfire; exploding tanks; falling overboard (kill plane) | PARTIALLY IMPLEMENTED | kill plane 7650 exists (GP "Chapter-start flow": "the 7652 warp crosses kill plane 7650"); tank blast damage missing |
| Collectibles | 0 treasures, 0 relics, 3 health (bow crate, container stack, helipad) | IMPLEMENTED BUT UNVERIFIED | `game::chapterRewards(6)` is empty (matches the page; PDA totals 0, GP "PDA"); health-pack intros of l6 not surveyed here; the crate-held kit: no evidence |
| Progression | Bow → stairs → container deck → container climb → upper deck (either side) → grate → helipad → stern → doors → down → engine room → halls → T left → cinematic | PARTIALLY IMPLEMENTED | sweep l6: 7565 PASS (→ 1387, chapter → 7), 7566 preload, 7569 / 7571 encounter (generators 7573 / 7572), hallway volumes 7644 / 7646 / 7622 / 7623 condition (doors already open), 7645 PASS (→ 7651 yacht), 7650 condition |
| Automated tests | Existing: `StreamReal.ChapterStartIntros`, `ProgressionReal.ChapterSixShipDoorsOpen`, `CriticalGates` (7565), `ChaptersReal.NatlaShipEndingPassesLoadedGuard`, `GameReal.ScriptedChapterEndChainsStartNextChapter`, `Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings`, `ProgressionReal.CombatCrateBreaksOnHit`, `FxReal.WeatherObjectAndRainMap`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 18 in [docs/matrix/18_rituals_old.md](matrix/18_rituals_old.md) — IMPLEMENTED BUT UNVERIFIED 9, PARTIALLY IMPLEMENTED 3, IMPLEMENTED 3, NOT IMPLEMENTED 2, VERIFIED AGAINST ORIGINAL 1.

Open in this chapter:

- **Precision aim with Mjolnir (C18-05; M28)** and **exploding fuel tanks' blast damage (M92)**: missing.
- **Mercenary encounters in the l6 units are untested**: 29 generators against 5 placed soldiers means almost every fight here is generator-driven; their arming volumes, the "keeps respawning while you linger / stops when you move on" rule and the backtracking triggers are unread.
- **Crate contents** (first aid kit) are not spawned / evidenced; crate cover invalidation unknown.
- **Stern / hallway doors** stand open in ours by a bytecode reading; the page says they open on approach. Needs a Wine check.
- Blast knock-back throwing bodies overboard: ground-pound knock values decoded but unused.
- No traversal or combat test loads l6_ship_exterior beyond the chapter-start check; chimney alternative (M06) missing; outfit choice (M75).

## 19. Helheim (Level 7 - Arctic Sea, part 1)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/18helheim.html (plus `details/helheim2.html` trench treasures, `details/helheim3.html` pool crossing, the passage map `screenshots/helheim.gif`; all read)
Our units (PR "Chapter routes" ch 7): l7_thin_ice (cinematic 9071 `275_hel_explosion`; its script changes level) → l7_arctic_sea (submerged ruins: statue wheels 9095-9098 `ar_ei_switch_oneway`, statue arms `ar_ani_statue_arms` 9099…, loose gears `ar_pt_thor_gear` 9274…, power stones `ar_pt_arcticsea_gem` 9270… with holder 9276, niches `ar_ei_gemhole` 9120…, rune strips 9134-9137, chains 9114 / 9115, drawbridge wheel 9103, drawbridge 9116, both towers with ladders 2044… and beams 2042…) → l7_helheim_hall01 / hall02 (flooded maze up to the stairs) → l7_helheim01 (portal room: cinematics 8950 / 8951, eitr pool, rune symbols 8993-8996 → dispatcher 8997, eitr falls 8998-9003, portal door 8956). uids from PR, GP "Interactions", test cases of `ProgressionReal.UberReceptaclesTakeTheirPortables` and `ProgressionReal.Survey` (`matrix/.scratchC/survey_l7_.txt`); the split of the flooded maze between l7_arctic_sea and the hall units is [I].

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 1 of the Arctic Sea: dive under the ice, free the drawbridge chains (three statue swords), collect and seat four power stones, open the drawbridge, swim the flooded maze, cross the eitr pool room to the Helheim portal. 8 health items, 4 puzzle items. Outfit choice (Drysuit Light / Wet Suit) + secondary weapon | PARTIALLY IMPLEMENTED | chapter 7 start swimming under the ice [O Wine] (GP "Chapter starts" row 7; `StreamReal.ChapterStartIntros`); drawbridge switch gate passes (`ProgressionReal.CriticalGates` l7_arctic_sea 9103 → 9116); receptacles pass under water; the dependency chain (swords + stones → bridge) is not asserted |
| Movement | Tower interiors and the pool room on foot: jumps between islands, long running jumps (held Jump), walking / sprinting / hopping along a fallen column | PARTIALLY IMPLEMENTED | generic M13; held-Jump extension (M91): no code; sprint on a beam missing (GP "Deviations": "beam sprint") |
| Traversal | Towers: climb out of water, corner ledge stand + side jump to a beam, beam → perpendicular beam → ladder, ladder top back-jump, ledge traverse with a gap side-jump, beam / stub / stacked handholds; drop through a floor hole into water. Pool room: column top, low-wall hang + traverse, corner back-jump to a thin column, climb, perch, perch → tilted platform, perch → hug → drop | IMPLEMENTED | in this unit: `GameReal.ArcticSeaLadderSideJumpOntoBeam` (ladder 2044 → beam 2042), `GameReal.LadderSideJumpsReachAndBack`, `GameReal.WallHugRoundsConcaveAndConvexCorners` / `PuzzleRoomWallHug` (hug corners 2054-2062); pillar / perch generic (`GameReal.PillarAttachRoundUpSlideOff`, `TemplePerchTurnsAndPillarTop`); the l7_helheim01 chain itself untested |
| Physics | Statue arms raised by a wheel, gears carried and seated on shafts, chains released, drawbridge lowering, currents | PARTIALLY IMPLEMENTED | `ProgressionReal.LoweredDrawbridgeIsSolid` (9116), `UberReceptaclesTakeTheirPortables` (gear 9274 → arms 9099 section 1 → state 2), currents: `Game.WaterFlowPullsTowardTheSink` (l7_arctic_sea's six flow volumes, GP "Water") |
| Grapple | n/a (a ring exists in the pool room but is only used in section 20) | n/a | — |
| Animation | Swim set, under-water wheel turn, swim carry / place, speargun and hammer under water, ladder set, perch / pillar set, pickups | PARTIALLY IMPLEMENTED | swim / ladder / pillar clips mapped (GP); "tru.exe's swim carry clips are not traced [I]" (GP "Interactions"); under-water switch clip unknown (B's M30) |
| Combat | Greenland sharks: about 6 at the start, 1 after the left tower, 4 after the right tower (speargun preferred; Mjolnir weak under water beyond point-blank). Pool room: Yeti and Viking thralls rising from the eitr and respawning while she stays on the entry ledge (hammer from the ledge: ≈ 3 bolts per yeti, 1 per Viking on medium; lure stuck Vikings by hanging from the edge; grenade on a locked one) | PARTIALLY IMPLEMENTED | `AiReal.SharksSpawnSwimBiteAndDie` (l7_arctic_sea population, `shark_greenland` ×8, 800 hp), `CombatReal.PoolSpeargunUnderWater`, replay `mjolnir_thralls` (l7_yggdrasil_hall01: Viking 1 bolt, yeti 2 on Normal); under-water bolt falloff 1000 / 400 / 100 [O data, I when]; respawn rule unread |
| Swimming | Whole first half: free 3D swimming, long dives with scuba (no surfacing needed), carrying heavy items, squeezing through gaps in rubble near ceilings, air pockets in the towers, utility light in the trench | PARTIALLY IMPLEMENTED | swim VERIFIED generically (`Game.SwimSpeedMatchesTru`, `Game.SwimTurnLagsLikeTru`, `StreamReal.OpenSeaWater` on this unit); scuba air: no "scuba" handling in `src/game` (grep 0) — breath (4000 frames [O]) presumably drains here too: unverified suspected gap; light `Gadgets.FlashlightToggleCooldownAndPose` |
| Vehicle | n/a (the bike appears only in the cinematic: `co_motorcycle` 9078) | n/a | — |
| Objects | Statue wheels ×3 used (+1 unused / broken statues), 2 giant gears + 2 shafts, 4 power stones (2 on carvings, 1 loose, 1 in the right tower), 4 niches in two pairs, rune strips, drawbridge wheel, jars, ladders, rune symbols (only noted here) | PARTIALLY IMPLEMENTED | sweep l7_arctic_sea: use 9103 PASS (+9116), volume 9280 → 9281 / 9274 condition; wheels 9095-9098 are not gate candidates (no static links) and untested; stones: `ProgressionReal.ReceptaclesTakeTheirPortables` {l7_arctic_sea, 9270, 9276} and `UberReceptaclesTakeTheirPortables` {9270 → 9120: calls 9120:56, 9137:55, 9112:57} |
| Puzzles | (1) raise three swords: two wheels directly, the third after seating two gears; (2) four power stones into four niches (each pair lights the runes on its side); (3) the drawbridge wheel works only after (1) and (2) | PARTIALLY IMPLEMENTED | parts pass in isolation (see Objects); test comment: "puzzle_cheat_button 9279 solves them [the statue arms] at load" — the solved state may pre-exist in ours; 9103 opens 9116 with nothing else done in the gate test (PR: "was lowered at load by puzzle_cheat_button 9279" before the fix) |
| Hazards | Sharks; eitr pool (instant death) under every jump in the portal room; thralls on the islands | IMPLEMENTED BUT UNVERIFIED | eitr kill verified only for l4_pool 5608 (GP "Water"); l7 volumes: `GameReal.ScriptedWaterLevelsRun` covers l7_yggdrasil_end, not l7_helheim01 |
| Collectibles | 29 treasures (1/30 - 29/30; ten in jars: 6, 14, 15, 16, 23, 24, 25, 26, 28, 29), 0 relics, 8 health, 4 power stones | PARTIALLY IMPLEMENTED | `chapterRewards(7)`: 19 loose treasures (8931, 9138-9141, 9164, 9165, 9198-9209) + relic 8936 — exactly the page's 30 minus its 11 jar treasures; `ChaptersReal.RewardTableMatchesUnits` (l7_arctic_sea); jar rewards not spawned (M63) |
| Progression | Dive → statues L1, L2 → broken statue treasure → gears → statue R3 → [trench] → left tower (stone 1) → corridor to the front room (stone 2) → outside (stone 3, seat two) → [side treasures] → right tower (stone 4) → seat two → wheel → drawbridge → flooded maze → stairs → cinematic → pool room → islands → portal → cinematic | PARTIALLY IMPLEMENTED | PR gates ch 7: "l7_arctic_sea switch 9103 (use) → drawbridge 9116: pass"; "l7_helheim01 rune switches 8993-8995 → dispatcher 8997: not asserted"; sweep l7_helheim01: volumes 8968 → 8950, 8969 → 8951 PASS |
| Automated tests | Existing: `StreamReal.ChapterStartIntros`, `StreamReal.OpenSeaWater`, `AiReal.SharksSpawnSwimBiteAndDie`, `FxReal.UnderwaterSettings`, `GameReal.ArcticSeaLadderSideJumpOntoBeam`, `GameReal.LadderSideJumpsReachAndBack`, `ProgressionReal.LoweredDrawbridgeIsSolid`, `ReceptaclesTakeTheirPortables`, `UberReceptaclesTakeTheirPortables`, `CriticalGates` (9103), `ChaptersReal.RewardTableMatchesUnits`, `StreamReal.ChapterThorGear`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 33 in [docs/matrix/19_helheim.md](matrix/19_helheim.md) — IMPLEMENTED BUT UNVERIFIED 18, IMPLEMENTED 8, PARTIALLY IMPLEMENTED 4, NOT IMPLEMENTED 2, VERIFIED AGAINST ORIGINAL 1.

Open in this chapter:

- **Scuba (M85)**: nothing in the game code suspends breath for the diving outfits; with the decoded 4000-frame breath this whole level would drown her. Needs verification first, then a fix.
- **Under-water interactions (M30)**: the three statue wheels and the drawbridge wheel are used while swimming; ours has no tested swimming "use" (the gate test stands her at the switch). Wheels 9095-9098 are unlinked in data and untested.
- **Puzzle dependencies not enforced / asserted**: 9103 lowers the bridge alone in the gate test; nothing ties it to three swords + four stones; a debug `puzzle_cheat_button` (9279) is reported by a test comment to solve the statue arms at load.
- **Power stones as carried objects vs inventory items**: unknown which the original uses; affects how two stones are brought to the niches.
- **Held-Jump long jump (M91)**: not implemented; required (as described) for two island jumps.
- **Thrall respawn while on the entry ledge**, yeti toughness (2 bolts in ours vs ≈ 3 on the page), enemies stuck under the ledge: unverified.
- **Ten jar treasures** (6, 14, 15, 16, 23-26, 28, 29) cannot be collected (M63); our 19 loose artifacts match the page's non-jar count exactly.
- Swim collision through rubble gaps, streaming while swimming into the hall units, pickups and the light under water, hammer under water: untested.
- Eitr in l7_helheim01 as a kill volume is untested (only l4_pool is verified).
- Outfit / weapon choice (M75).

## 20. Yggdrasil (Level 7 - Arctic Sea, part 2)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/19yggdrasil.html (plus `details/yggdrasil2.html` and `details/yggdrasil3.html`: the relic run in two parts; all read)
Our units (PR "Chapter routes" ch 7): l7_helheim01 (portal room again: rune symbols 8993-8996 `ar_uo_rune_wall_switch_{c,b,d}` / `…_swich_a` → dispatcher 8997, eitr falls 8998-9003 `ar_uo_goo_fall`, wall anchor 8897, relic 8936, portal door 8956) → l7_helheim02 / l7_helheim03 (hallway beyond the portal: broken-stair slide, thrall waves) → l7_yggdrasil (waterfall room: volume 8804 → cinematic 8773) → l7_yggdrasil_hall01 / hall02 (hallway to the device: thralls, eitr pit) → l7_yggdrasil_end (arrival walkway; arena volume 8303 → 8261 `320_hel_boss`). uids from PR, GP "Grapple", `ProgressionReal.Survey` and the sweeps (`matrix/.scratchC/`); hallway ↔ unit split [I] (generators are placed in helheim03 and both yggdrasil halls, GP "Survey").

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Part 2 of the Arctic Sea: optional timed relic run in the portal room, then two thrall-filled hallways with the long story cinematic between them, ending with the cinematic that starts the Midgard Serpent device. 4 health items | PARTIALLY IMPLEMENTED | cinematic triggers pass (sweep: l7_yggdrasil 8804 → 8773, l7_yggdrasil_end 8303 → 8261); `CriticalPathPerChapter` ch 7 enters the units by level loads; relic run and fights untested |
| Movement | Sprint, running jumps (one deliberately into a pillar), held-Jump long jump, standing jump from a platform's back edge to a column top, slides down broken stairs with a jump at the end | PARTIALLY IMPLEMENTED | sprint [O] (`Game.SprintStaminaMatchesTru`); M91 missing; slide `Game.SlideAcceleratesAtGravityAlongSlope` [O Wine], slide-jump untested |
| Traversal | Relic circuit: column climb (fast), perch, perch → tilted platform, platform → column top, column → low wall, fast shimmy, lean-back jump to a broken column, fallen-column crossings, back-jump from a wall corner to a column, grapple swing into an alcove | PARTIALLY IMPLEMENTED | generic pillar / perch / hang-jump tests (other units); tap-to-go-faster (M18) missing; nothing tested in l7_helheim01 |
| Physics | Eitr fall switched off by four timed symbols; pendulum swing | PARTIALLY IMPLEMENTED | swing generic (`Game.GrappleSwingAndRelease`); the fall objects and dispatcher are scripted objects with no test (PR: "not asserted (dispatcher only)") |
| Grapple | One use: ring on a pole between two serpent pillars — swing from the tilted platform to the relic alcove and back | IMPLEMENTED BUT UNVERIFIED | anchor 8897 is in GP "Grapple"'s anchor list for l7_helheim01; no test |
| Animation | Symbol activation (control taken briefly), fast pillar climb, fast shimmy, hammer throw + ground pound, relic take | PARTIALLY IMPLEMENTED | Mjolnir clips [O]; rune-switch use clip unknown; M18 none |
| Combat | 4 Vikings below the stair slide; a wave hall (≈ 20 Vikings + 3-4 yetis spawning at its end; they stop following past the next stairs and the rest die by script); after the cinematic 2 Vikings, a yeti from the left, another yeti round a corner. Tools: bolt, ground pound to stagger groups, grenades at walls (stuck grenades come back with the enemy) | PARTIALLY IMPLEMENTED | `Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings` [O], replay `mjolnir_thralls` (l7_yggdrasil_hall01), `AiReal.ThrallsGetUpUnlessMjolnir`, `GameReal.KilledThrallBreaksIntoChunks` (l7_yggdrasil_hall01); wave generators, leash and scripted die-off unverified |
| Swimming | n/a | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Four rune symbols with individual timers, eitr waterfall, grapple ring, relic, jars, portal, broken stairs, eitr pit | PARTIALLY IMPLEMENTED | symbols load as switches linked to 8997 (survey); no use candidate was swept; jars M63 |
| Puzzles | Timed circuit: all four symbols must be lit at once (any order) to stop one eitr fall long enough to swing behind it | NOT IMPLEMENTED (no evidence) | PR gates ch 7: "l7_helheim01 rune switches 8993-8995 → dispatcher 8997: not asserted"; GP "Thor's gear": rune-switch player sensors (vt 14) never fire |
| Hazards | Eitr pool and falls (touch = death), pillar smack (small damage), eitr pit after the second slide, yeti blows, own grenades | IMPLEMENTED BUT UNVERIFIED | eitr kill verified for l4_pool only; grenade self-damage `Gadgets.GrenadeKillsDummyAndHurtsLara` |
| Collectibles | 1 treasure (30/30, in a jar), 1 relic (6/6), 4 health | PARTIALLY IMPLEMENTED | relic 8936 in `chapterRewards(7)` (collectable by the generic rule once reachable); treasure 30 is a jar reward (M63); relic health bonus missing (M64) |
| Progression | [treasure 30, health] → [relic run] → portal → hall, health → stair slide (one-way) → 4 Vikings → wave hall → health → stairs → waterfall cinematic → 2 Vikings → yeti → health → slide + pit jump → yeti → device room walkway → cinematic | PARTIALLY IMPLEMENTED | PR gates ch 7 "l7_yggdrasil(_end) volumes 8804, 8303 / 8312, 8323-8329 → cinematics 8773 / 8261, spline mover 8404: pass" |
| Automated tests | Existing: replay `mjolnir_thralls`, `GameReal.KilledThrallBreaksIntoChunks`, `StreamReal.MjolnirHitBreaksUrnAndItStaysBroken` (l7_yggdrasil_hall01), `StreamReal.SaveContinuesBitIdentically`, `AiReal.ThrallsGetUpUnlessMjolnir`, `ProgressionReal.CriticalPathPerChapter` (ch 7), `CineReal.EveryStreamPlaysWithItsActors`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 21 in [docs/matrix/20_yggdrasil.md](matrix/20_yggdrasil.md) — IMPLEMENTED BUT UNVERIFIED 10, NOT IMPLEMENTED 5, PARTIALLY IMPLEMENTED 4, IMPLEMENTED 2.

Open in this chapter:

- **Timed rune-symbol circuit (M87; C20-02 … C20-11)**: the four symbols, their timers, the dispatcher and the eitr fall are unimplemented or at least unevidenced; the game's last relic depends on them.
- **Fast climb / shimmy by tapping Interact (M18)** and **held-Jump long jump (M91)**: not implemented; the run is tuned around both.
- **Thrall waves**: generator counts, cadence, the leash at the stairs and the scripted die-off are unread; the spawn after the waterfall cinematic stops at an unbound native (`generatorbase` fn 5).
- Jump into a pillar with a slide-down, slide-jump over the eitr pit, eitr falls / pool as kill volumes in l7_helheim01: untested.
- Relic health bonus and "relic kept after a death" (M64, M73): no evidence.
- Treasure 30 is a jar reward (M63).
- Ground-pound stagger: decoded knock values are not applied.

## 21. Out of Time (Level 7 - Arctic Sea, part 3)

Walkthrough: https://tombraiders.net/stella/walks/TR8walk/20outoftime.html (plus `details/outoftime2..10.html`: nine screenshot sequences; all read)
Our units (PR "Chapter routes" ch 7): l7_yggdrasil_end (the Midgard Serpent device: three levels of ring walkways, floating platforms = `natla_end_mover` ×82 spline movers incl. 8404, Natla = `natla_end` ×6 scripted objects, fireballs `wp_natla_fireball(_adren)` ×17, rune panels = Thor towers 8398 / 8399 / 8400, power nodes = buttress mounts 8362 / 8363 / 8364, dispatcher 8360, endgame gun 8401, wall anchor 8165, water volume 8309 + ten flow volumes, yeti generators) → l7_monastery (cinematic 8138 `342_hel_endgame`, end save, credits). l7_himalaya_end is not on the route (PR ch 8). uids from PR, GP "Enemies / Survey", "Grapple", "Water" and the sweep (`matrix/.scratchC/sweep_l7_yggdrasil_end.txt`). Panel / node numbering (first / second / third) ↔ uids is not established.

Abbreviations and evidence conventions as in `15_land_of_the_dead.md`. Screenshot base: https://tombraiders.net/stella/walks/TR8walk/

| Category | Required information | Status | Evidence / tests |
|---|---|---|---|
| Chapter | Final section: expose three power nodes by pushing three rune panels (two on the lower level, one on the upper), smash the three nodes (in a forced order by reachability), under yeti attacks and Natla's fireballs; final cinematics, credits, Treasure Hunt unlock. 6 health items, no treasures | PARTIALLY IMPLEMENTED | the scripted skeleton is played without direct handler calls: `ProgressionReal.EndgameCredits` (arena 8303 → `320_hel_boss`, towers pushed, mounts used, `330_hel_thralls`, `340_hel_endgame`, warp to l7_monastery, `342_hel_endgame`, end save, credits); Lara is teleported between the six interaction points — the route, enemies and fireballs are not exercised |
| Movement | Run / sprint on curved walkways through fire patches, running jumps between wedge ledges at their narrow ends, long drop softened by a swan dive or landing roll | PARTIALLY IMPLEMENTED | generic M13, M14; swan dive / landing roll missing (M90) |
| Traversal | Jump-grab onto moving platforms, platform → platform (next ring) → moving handholds on the central tower, stacked handhold groups, rides on moving handholds, drop onto and slide down a buttress; chimney jumps ×2; rough-stone free climb; column circuits | PARTIALLY IMPLEMENTED | platform carry: `GameReal.SplineMoverCarriesLara`, `SplineMoverFollowsItsPath`, `SplineMoverIndependentOfRenderRate` (l7_yggdrasil_end 8404); free-climb holds exist in this unit (GP "Free climbing" unit list); missing M06, M76 |
| Physics | Platforms on circular splines at several heights and speeds; buttresses collapsing into new platforms; fireball knock-back; rising water in the finale | PARTIALLY IMPLEMENTED | spline movers [tests above]; water level script: `GameReal.ScriptedWaterLevelsRun` (l7_yggdrasil_end 8309: SetLevelToMarker at load); knock-back none |
| Grapple | Ring on the reddish middle ledge: rappel down, pay out, swing forward, release onto the lower ledge. Ring on the red upper wall: (a) plain swing across a gap, (b) climb the rope, wall run, jump right to a ledge | IMPLEMENTED BUT UNVERIFIED | wall anchor 8165 listed for l7_yggdrasil_end (GP "Grapple"); rappel / swing / wall run generic (`GameReal.KrakenRoomRappel`, `GameReal.ValaskjalfWallRun`); nothing tested here; wall-run jump into a ledge grab (M24) untested |
| Animation | Panel push (Thor strength), node smash with the hammer, dodge poses in slow motion, platform balance, final cinematics | PARTIALLY IMPLEMENTED | push clips on uber prismatic joints "(l7 Thor towers) use the same clips facing the joint axis" (GP "Interactions"); mount break is anim-graph stimulus 1 → state 3 (PR); a hammer-strike clip for it: unknown |
| Combat | Yeti thralls spawning from the pool and from waterfalls on every level (lower level: continuous; upper: 1 + 2 + 1); Natla hovering and throwing fireballs (small damage, strong push, burning floor patches), invulnerable; Mjolnir throughout | PARTIALLY IMPLEMENTED | yeti + Mjolnir generic (replay `mjolnir_thralls`); fireballs: the trigger volumes' calls into the fireball objects stop at an unbound native (sweep: 8323 / 8324 / 8325 / 8327 / 8329 → +8404, fireballs 8246 / 8234 / 8233 / 8236 / 8237 "[native weaponbase fn 0]"); no fireball code in `src/game` |
| Swimming | n/a on the route (the pool is a hazard) | n/a | — |
| Vehicle | n/a | n/a | — |
| Objects | Floating platform rings (two heights on the route + a top ring), 2 grapple rings, 3 rune panels in alcoves, 3 power nodes behind the buttress supports, an alcove that only exists after its panel is pushed, moving handhold rings on the tower, sloping buttress | PARTIALLY IMPLEMENTED | towers 8398-8400 and mounts 8362-8364 tested (`EndgameCredits`); mover 8404 tested; the rest (other 81 movers, tower handhold rings, alcove reveal) untested |
| Puzzles | Order / reachability: lower panels 1 and 2 (the third lower panel's ledge is broken) → node 1 from the middle level → upper level → panel 3 → node 2 → tower climb → node 3 | IMPLEMENTED BUT UNVERIFIED | state logic passes in the test order towers ×3 then mounts ×3 ("the pushed Thor towers lock the buttress mounts B-D", PR); the page's interleaved order (node 1 before panel 3) is not tested |
| Hazards | Pool (knocked in = lost), fireballs + lingering flames, long falls, yeti blows near edges, two timed dodge events (fail = death, checkpoint reload) | PARTIALLY IMPLEMENTED | falls M65; what the pool does to Lara here is unknown (ten flow volumes pull to a sink at 1422 units/s, GP "Water"); fireballs / flames none; M36 missing |
| Collectibles | 0 treasures, 0 relics, 6 health | IMPLEMENTED BUT UNVERIFIED | no reward id of `chapterRewards(7)` lies in the 82xx-84xx uid range of this unit [I by uid]; health packs generic (`Combat.HealthPacks`) |
| Progression | Health → platform ride → ring ledge → rappel to the lower level → panel 1 → panel 2 → chimney climb to the middle → node 1 (cinematic) → platform ride → alcove chimney → free climb + dodge 1 → upper level → yetis → ring swing → panel 3 → rope climb + wall run → checkpoint → platform → node 2 (cinematic: Amanda) → two platform rings → tower handholds + dodge 2 → moving handhold ride → buttress slide → node 3 → final cinematics → Tibet → credits → Treasure Hunt | PARTIALLY IMPLEMENTED | PR gates ch 7 last row "pass (`ProgressionReal.EndgameCredits`, played…)"; opt-in `CriticalPathInOrder` ends with the same `playEnding` |
| Automated tests | Existing: `ProgressionReal.EndgameCredits`, `ProgressionReal.CriticalPathInOrder` (opt-in), `GameReal.SplineMoverCarriesLara`, `SplineMoverFollowsItsPath`, `SplineMoverIndependentOfRenderRate`, `GameReal.ElevatorUberCommandCreatesMovingCollision`, `GameReal.ScriptedWaterLevelsRun`, `GameReal.ScriptedWaterLevelMoves`, `Game.WaterFlowPullsTowardTheSink`, `GameReal.SwimWaterDataDecoded`, `CineReal.NatlaSubtitlesCameraAndVoiceLines`, `Menus.MainCentreTreasureHuntAndRevisit`, `Progress.ProfileAndTreasureHuntSave`. Required: scenarios below | PARTIALLY IMPLEMENTED | see left |

Requirement rows: 25 in [docs/matrix/21_out_of_time.md](matrix/21_out_of_time.md) — NOT IMPLEMENTED 8, IMPLEMENTED 8, IMPLEMENTED BUT UNVERIFIED 5, PARTIALLY IMPLEMENTED 4.

Open in this chapter:

- **Natla's fireballs (M88)**: not launched (unbound `weaponbase` fn 0), no projectile, knock-back or burning-floor code. They are the fight's only threat besides the yetis, and both timed dodge events hang on them.
- **Situational adrenaline (M36)** ×2: no time-scale support.
- **Chimney jump (M06)** ×2 on the critical path (lower → middle, middle → upper).
- **Moving markup (M76)**: grabbing platform edges, platform-to-platform grabs, the handhold rings on the central tower — no evidence; only standing on a mover is tested.
- **The fight is tested only as a state machine**: Lara is placed at the three towers and three mounts. Order constraints from reachability (broken ledge, alcove appearing with a panel), yeti generators, checkpoints and both grapple rings are unexercised.
- **Damage-free landings (M90)** and **wall-run jump to a ledge (M24)**: missing / untested.
- Fate of Lara in the pool (kill, current, respawn) is unknown; the finale's water rise runs only as a level script.
- Hammer-strike animation on a node and the Thor glow on panels: unknown / sensors never fire.
- Treasure Hunt rules after the credits are inferred ([I] in GP).

## Status summary

| Chapter | Rows | VERIFIED AGAINST ORIGINAL | IMPLEMENTED | IMPLEMENTED BUT UNVERIFIED | PARTIALLY IMPLEMENTED | NOT IMPLEMENTED | unknown / n/a |
|---|---|---|---|---|---|---|---|
| 01. Prologue: No Illusions (Prologue - Croft Manor) | 16 | 3 | 5 | 8 | 0 | 0 | 0 |
| 02. The Path to Avalon (Level 1 - Mediterranean Sea, part 1) | 32 | 0 | 6 | 15 | 7 | 3 | 1 |
| 03. Niflheim (Level 1 - Mediterranean Sea, part 2) | 36 | 0 | 5 | 17 | 9 | 4 | 1 |
| 04. The Norse Connection (Level 1 - Mediterranean Sea, part 3) | 25 | 0 | 2 | 16 | 5 | 1 | 1 |
| 05. God of Thunder (Level 1 - Mediterranean Sea, part 4) | 20 | 0 | 6 | 10 | 2 | 2 | 0 |
| 06. Realm of the Dead (Level 1 - Mediterranean Sea, part 5) | 14 | 0 | 3 | 5 | 4 | 1 | 1 |
| 07. The Remnants (Level 2 - Coastal Thailand, part 1) | 57 | 1 | 3 | 33 | 14 | 6 | 0 |
| 08. Bhogavati (Level 2 - Coastal Thailand, part 2) | 64 | 0 | 5 | 39 | 12 | 8 | 0 |
| 09. The Ancient World (Level 2 - Coastal Thailand, part 3) | 18 | 0 | 2 | 9 | 1 | 6 | 0 |
| 10. A Puppet No Longer (Level 2 - Coastal Thailand, part 4) | 12 | 0 | 2 | 9 | 0 | 1 | 0 |
| 11. Protected by the Dead (Level 3 - Croft Manor) | 36 | 0 | 14 | 11 | 7 | 4 | 0 |
| 12. The Unnamed Days (Level 4 - Southern Mexico, part 1) | 19 | 0 | 1 | 8 | 5 | 5 | 0 |
| 13. Xibalba (Level 4 - Southern Mexico, part 2) | 44 | 0 | 6 | 26 | 10 | 2 | 0 |
| 14. The Midgard Serpent (Level 4 - Southern Mexico, part 3) | 57 | 1 | 6 | 26 | 15 | 9 | 0 |
| 15. Land of the Dead (Level 4 - Southern Mexico, part 4) | 21 | 0 | 0 | 13 | 5 | 3 | 0 |
| 16. Gate of the Dead (Level 5 - Jan Mayen Island, part 1) | 23 | 1 | 2 | 9 | 3 | 8 | 0 |
| 17. Valhalla (Level 5 - Jan Mayen Island, part 2) | 70 | 0 | 6 | 23 | 21 | 20 | 0 |
| 18. Rituals Old (Level 6 - Andaman Sea) | 18 | 1 | 3 | 9 | 3 | 2 | 0 |
| 19. Helheim (Level 7 - Arctic Sea, part 1) | 33 | 1 | 8 | 18 | 4 | 2 | 0 |
| 20. Yggdrasil (Level 7 - Arctic Sea, part 2) | 21 | 0 | 2 | 10 | 4 | 5 | 0 |
| 21. Out of Time (Level 7 - Arctic Sea, part 3) | 25 | 0 | 8 | 5 | 4 | 8 | 0 |
| **All 21** | **661** | **8** | **95** | **319** | **135** | **100** | **4** |

(The first status named in a row counts; rows that split a status between two parts count under the first.)

## Shared mechanics

One table for the whole campaign: every gameplay mechanic the 21 walkthrough sections ask for, one row per mechanic,
unified from the three per-range tables (sections 01-07, 08-14, 15-21). Ids M01-M93 are those of the 08-14 and 15-21
tables; M94-M111 are mechanics met only in sections 01-07. Columns 01 … 21 are the walkthrough sections: **R** = needed
on the main route, **o** = only on an optional / treasure / alternate route, empty = not met. `Chapters` counts the
sections that use the mechanic. `Reusable implementation` names the one place the mechanic lives, so a mechanic met in
several chapters is one implementation and not per-level code (file names from the 0.42.0 tree; `(none yet)` = no code).
`Status` is the mechanic's general state now: the per-range tables' v0.41.0 status, with the least favourable taken where
they disagree (said in Evidence), then the 0.42.0 changes of "Status changes since the baseline" applied. A mechanic
whose source status was "unknown" is rated by its evidence (M98, M106; said in Evidence). A status here is not a
chapter's status: the chapter sections say where a mechanic is tested in that chapter's own units.

Count: **111 mechanics**; **90** are used in 2 or more sections (4 in all 21), 21 in one section only.
By status: VERIFIED AGAINST ORIGINAL 5, IMPLEMENTED 57, IMPLEMENTED BUT UNVERIFIED 11, PARTIALLY IMPLEMENTED 18, NOT IMPLEMENTED 20.

### Movement

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M13 | Standing / running jump to land or grab a ledge; jump straight up to a handhold | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | 21 | `src/game/Player.cpp` jumps (`aimGroundJump`, solver 0x73EDC0) | VERIFIED AGAINST ORIGINAL | `GameReal.BedroomJumpsMatchTruTraces`, `Game.JumpTakeOffsMatchTruTraces`, `GameReal.RunningJumpReachAndRush`; query hit boxes [U]; A, B, C agree |
| M14 | Sprint; sprint-jump for extra distance |   |   |   |   | o | R | o | o |   |   | R |   | R | o |   | o | R |   | o | o | R | 12 | `src/game/Player.cpp` sprint / stamina | PARTIALLY IMPLEMENTED | sprint: `Game.SprintStaminaMatchesTru` (Wine); sprint jumps and beam sprint missing (GP "Deviations"). A rates sprint VERIFIED AGAINST ORIGINAL, B / C the row PARTIALLY IMPLEMENTED: least favourable taken |
| M15 | Crouch / crawl / forward roll through low gaps, under bars and closing doors | R |   | R | R |   |   | R | R |   |   | R | R | R | R | R |   | R |   |   |   |   | 11 | `src/game/Player.cpp` crouch / roll | IMPLEMENTED | `Game.CrouchUnderSlabAndRoll` (synthetic); A's two rows (crouch; forward roll) merged; A, B, C agree |
| M16 | Hurdle / vault / mantle onto low and waist-high obstacles | R | R | R | R | R |   | R | R |   |   | R | R | R | R | R |   | R | R | R |   | R | 16 | `src/game/Player.cpp` hurdle | IMPLEMENTED | `GameReal.JunglehubVaultOver`, `GameReal.CliffclimbObstaclesAndLedgeGrabs`, `GameReal.PoolRockGrabNotHurdle`, `StreamReal.PrologueCorridorToStaircase`. A rates it VERIFIED AGAINST ORIGINAL, B / C IMPLEMENTED: least favourable taken |
| M17 | Slope / ramp / column / rail slide, and the jump out of it |   |   | R | o |   |   | R | o |   |   |   |   |   | R |   |   | R |   |   | R | R | 8 | `src/game/Player.cpp` slide; rail slide in `src/game/Traverse.cpp` | IMPLEMENTED BUT UNVERIFIED | slide: `Game.SlideAcceleratesAtGravityAlongSlope` (Wine), `GameReal.ValaskjalfRailSlideTraversesAndDetaches`; the slide-jump has no test. A rates the slide VERIFIED AGAINST ORIGINAL; the row follows B / C (least favourable) |
| M19 | Walk-over / cartwheel (Forward + Crouch; front walkover on a beam) |   |   | o |   |   |   |   |   |   |   |   |   |   | o |   |   |   |   |   |   |   | 2 | (none yet) | NOT IMPLEMENTED | no code (A's "beam front walkover" row, B's M19) |
| M90 | Swan dive; roll on landing to cut fall damage |   |   | o |   | o |   | o |   |   |   |   |   |   |   |   |   |   |   |   |   | o | 4 | swan dive in `src/game/Player.cpp` / `src/game/Water.cpp`; landing roll (none yet) | PARTIALLY IMPLEMENTED | A, C: NOT IMPLEMENTED at v0.41.0. changes.md (0.42.0): swan dive `GameReal.CliffSwanDiveIntoTheSea`, `game_selftest_swan_dive` (l2_cliffclimb; other spots unverified). The landing roll that cuts fall damage still has no code. A's two rows (swan dive; landing roll) are C's one M90 |
| M91 | Holding Jump to lengthen a running jump |   |   |   |   |   |   | o |   |   |   |   |   |   |   |   |   |   |   | R | o |   | 3 | (none yet) | NOT IMPLEMENTED | no code; existence in the original unverified (A "unknown", C NOT IMPLEMENTED) |
| M94 | Walk / run / turn, root-motion locomotion | R | R | R | R | R | R | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 7 | `src/game/Player.cpp` locomotion | VERIFIED AGAINST ORIGINAL | A only: `GameReal.LaraClipsAndRootMotion`, `StreamReal.PrologueCorridorToStaircase` (Wine). B / C do not list it; it is needed in every section |
| M95 | Stairs / step up-down (110 units) | R | R | R | R | R |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 6 | `src/game/Player.cpp` steps | VERIFIED AGAINST ORIGINAL | A only: `GameReal.StairsWalkAndRunUpAndDown` |

### Ledges and climbing

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M01 | Ledge grab, hang, shimmy (round corners, across small gaps), pull up, let go | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | o | R | 21 | `src/game/Traverse.cpp` ledge hang / shimmy; catch by `Player::ledgeOnArc` (`src/game/Player.cpp`) | IMPLEMENTED | `Game.LedgeGrabHangShimmyDrop`, `GameReal.HangShimmyRoundCorners`, `GameReal.StandingJumpCatchesHighLedgeWithinReach`. A rates the grab VERIFIED AGAINST ORIGINAL (prologue wall 973, Wine), B / C rate the row IMPLEMENTED: least favourable taken. A's two rows (grab; shimmy round corners) merged here |
| M02 | Ledge-to-ledge jumps: up, sideways, back (with lean), across a gap | R | o | R | R | R | R | R | R | o | R | R | R | R | R | R | R | R |   | R | o | R | 20 | `src/game/Traverse.cpp` hang jumps (jump queries via `Player::ledgeOnArc`) | VERIFIED AGAINST ORIGINAL | `GameReal.HangLeapsMatchTruTraces` (Wine traces), `GameReal.HangJumpLeans`, `GameReal.TempleLedgeJumpsUpAndBack`, `GameReal.PoolSideJump`; A, B, C agree |
| M03 | Drop from an edge to hang; drop and catch a lower ledge / handhold | R | o |   | R |   |   | R | R | R |   | R | R | R | R | R |   | R |   | R |   | R | 14 | `src/game/Traverse.cpp` drop to hang | IMPLEMENTED | `Game.DropToHangFromEdge`, `GameReal.PoolDropToHang`; A, B, C agree |
| M04 | Saving grab (weak catch after a long jump / drop, slick ledge or hazard hit; Interact recovers) |   |   |   |   |   |   |   | o | R |   |   |   | R | R |   | R | R |   |   |   |   | 6 | `src/game/Traverse.cpp` / `src/game/Climb.cpp` bad-attach state (`ledgeb_bad_attach_M`) | IMPLEMENTED BUT UNVERIFIED | `GameReal.ClimbBadAttach` (free-climb variant), `Tailoring.PresetsScaleHealthAndCapacity`; trigger speed and window [I]; slick ledges and hazard hits never trigger it; B, C agree |
| M05 | Free-climb handholds (rough stone): 8 directions, DICE jumps, top out, jump off |   |   | R |   |   | R | R | R |   |   | R |   | R | R | R |   |   |   |   |   | R | 9 | `src/game/Climb.cpp` | IMPLEMENTED | `GameReal.CliffclimbDiceJumps`, `GameReal.ClimbAttachClimbUpTopOut`, `GameReal.ClimbOnlyAtHolds`, `GameReal.CliffclimbStances`. A rates it VERIFIED AGAINST ORIGINAL (l2_cliffclimb), B / C IMPLEMENTED: least favourable taken |
| M06 | Chimney / wall jump (wall-to-wall up a slot; ground, hang or pole-crouch start; single wall kick) |   |   | o |   | o |   | R | R |   |   |   | o | o | R | R |   | R | o |   |   | R | 11 | `src/game/WallJump.cpp` | IMPLEMENTED | A, B, C: NOT IMPLEMENTED at v0.41.0. changes.md (0.42.0): `Game.ChimneyJump*`, `GameReal.JunglehubChimneyJumpClimbsColumns` (l2_junglehub 796 → 797), `GameReal.ValaskjalfChimneyJumpTakesAllFive`, l2_towerroom 418 → 417; other sites unverified, not yet measured against tru.exe |
| M11 | Perch on a column / pillar top; jumps perch → perch / ledge / platform |   |   |   | R |   |   | R | R |   |   | R |   | R | R | R |   |   |   | R | o |   | 9 | `src/game/Traverse.cpp` perches | IMPLEMENTED | `Game.PerchAttachAndJumpOff`, `Game.PerchJumpsByTheirQueries`, `GameReal.TemplePerchTurnsAndPillarTop`, `GameReal.CavernsPerchJumpReaches`; A, B, C agree |
| M12 | Thin vertical column (pillar): grab, climb, turn, slide down, jump column → column, onto its top |   |   | o | R |   |   | o |   |   |   | R |   | R | R |   |   |   |   | R | o |   | 8 | `src/game/Traverse.cpp` pillars | IMPLEMENTED | `GameReal.PillarAttachRoundUpSlideOff`, `GameReal.PillarJumpsByStickAngle`; back-jump catch untested (C); A, B, C agree |
| M18 | Faster shimmy / climb / hand-over-hand by tapping Interact in rhythm |   |   | o |   |   |   | o | o |   |   |   |   |   | R |   |   | R |   |   | o |   | 6 | (none yet) | NOT IMPLEMENTED | no code or doc hit (A, B, C agree) |
| M76 | Moving markup: handholds, poles and ledges carried by rotating rings / moving platforms; timed jumps onto them; riding while hanging |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | R |   |   |   | R | 3 | (none yet) | NOT IMPLEMENTED | markup is static per unit (GP "Controller"); only object collision moves (`GameReal.RampUberCommandMovesCollision`) |
| M78 | Rideable machinery: counterweights, gear shafts, moving chains, swinging hammers |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | their scripts pass (`ProgressionReal.ValaskjalfChainsDetach`); PR: "the hpoles' own motion is not modelled" |
| M80 | Wall hug: stand on a narrow ledge, sidestep round corners, lean / side jump that re-grabs past a gap |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   | R | R |   | R |   |   | 4 | `src/game/Traverse.cpp` wall hug | IMPLEMENTED | `GameReal.PuzzleRoomWallHug`, `GameReal.WallHugRoundsConcaveAndConvexCorners`, `GameReal.WallHugSideAndTwistJumpsByTheirQueries`; A, C agree |
| M96 | Ladder: climb, ladder ↔ holds / ledges, ladder jumps (up, side, back) |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Traverse.cpp` ladders | PARTIALLY IMPLEMENTED | A only: `GameReal.ShipSinkingHallLadder`, `GameReal.LadderRungsClimbOffTop`, `GameReal.LadderJumpUpToLedge`, `GameReal.LadderSideJumpsReachAndBack`; stacked ladders (q88) not done. C uses the same code in section 19 (`GameReal.ArcticSeaLadderSideJumpOntoBeam`) without a row of its own |
| M98 | Climbing a ship's anchor and chain |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Traverse.cpp` ledge / hold code (no chain-specific code) | IMPLEMENTED BUT UNVERIFIED | A only, "unknown": the anchor's and chain's markup is not surveyed and there is no test; rated as the generic ledge code it would use (chapter row C05-01: "IMPLEMENTED BUT UNVERIFIED at best") |

### Poles and beams

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M07 | Horizontal pole: grab, swing, jump off (fling) to pole / ledge / handhold |   | o | R | o |   |   | R | R |   | R | R |   | R | R | R | R | R |   |   |   |   | 12 | `src/game/Player.cpp` horizontal-pole states; fling aimed by the jump queries (`Player::ledgeOnArc`) | IMPLEMENTED | `GameReal.MainHallPoleFlingLandsOnPlatform` (Wine l3_main_hall 948 → 946), `GameReal.TrapRoomsPoleFling`, `Game.PoleGrabSwingFlingDrop`. B / C rate the fling VERIFIED AGAINST ORIGINAL, A rates the row IMPLEMENTED: least favourable taken. A's row also holds hand-over-hand (M08) |
| M08 | Pole: hand over hand, turn round, pull up to stand / crouch on it, jump up / forward from it |   |   |   |   |   |   | R | R |   |   | R |   | R | R | R | R | R |   |   |   |   | 8 | `src/game/Player.cpp` horizontal-pole states (`hpole_perch_*` clips) | IMPLEMENTED | `GameReal.TrapRoomsPoleParallelHandOverHandAndTurns`, `GameReal.TrapRoomsPoleAttachBackAndTurnTimes`, `GameReal.TrapRoomsPoleTop`; A's "pole-top stand and jump" row; A, B, C agree |
| M09 | Pole to collinear pole across a gap (legs swing out at the end, then jump) |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   | 1 | `src/game/Player.cpp` pole fling (same code as M07) | IMPLEMENTED BUT UNVERIFIED | B only: no test of an end-of-pole jump; behaviour at the pole end unknown |
| M10 | Balance / jutting beam: walk, wobble, turn, jump off / onto, fall and catch, hang from its side |   |   | R |   |   | R | R | R |   |   | R | R | R | R | R |   | R |   | R | o |   | 12 | `src/game/Traverse.cpp` balance beam (controller 0x742330 / 0x742770 [O]) | IMPLEMENTED | `Game.BalanceBeamWalkTurnStepOff`, `GameReal.TempleBalanceBeamWalkAndJump`, `GameReal.BeamFallsCatchScaffoldLedge`, `Game.FallingOntoBeamCatchesIt`; beam sprint missing (GP "Deviations"); A, B, C agree |
| M97 | Hang-jump / drop from a ledge onto a pole |   | o |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/game/Traverse.cpp` hang jumps + `src/game/Player.cpp` pole grab | IMPLEMENTED BUT UNVERIFIED | A only: no named test |

### Grapple

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M20 | Grapple: select a ring, throw, swing, pump, jump off / land | R | R | R | R |   |   | R | R | R |   | R | R | R | R | R |   |   |   |   | o | R | 14 | `src/game/Grapple.cpp` | IMPLEMENTED | `Game.GrappleSwingAndRelease`, `GameReal.PoolGrappleTargetAndSwing`, `GameReal.GrappleTuningFromData`, `GameReal.MainHallRingByItsOwnRange`, `GameReal.SharedQThrowsTheGrappleNotTheCamera`; tuning / selection [O], swing physics [I]; A's two rows (target + throw; swing) merged; A, B, C agree |
| M21 | Grapple thrown in mid-air while falling |   |   |   |   |   |   |   | R | R | R |   |   |   |   |   |   |   |   |   |   |   | 3 | `src/game/Grapple.cpp` (selection is state-independent) | IMPLEMENTED BUT UNVERIFIED | B only: no test throws in the air; whether the original restricts it is unknown |
| M22 | Rappel down a wall, climb / pay out the rope, reverse rappel, release |   |   |   |   |   |   |   | R | R |   |   |   | R | R | R |   | o |   |   |   | R | 7 | `src/game/Grapple.cpp` rappel | IMPLEMENTED | `GameReal.KrakenRoomRappel` (replay `kraken_rappel`); speeds [I], overhang rules [O]. A's "rappel / wall run" row (no use in 01-07) agrees |
| M23 | Wall run on the rope (pendulum run), jump off sideways at the top of the arc |   |   |   |   |   |   |   |   | R | R |   |   |   |   |   |   | R |   |   |   | R | 4 | `src/game/Grapple.cpp` wall run | IMPLEMENTED | `GameReal.ValaskjalfWallRun` (replay `valaskjalf_wallrun`), `Game.GrappleWallRunFromGround`; all numbers [I]; A (same row as M22), B, C agree |
| M24 | Wall run / rappel jump straight back off the wall into a ledge or handhold grab |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   | o |   |   |   | R | 3 | `src/game/Grapple.cpp` wall kick + `src/game/Traverse.cpp` ledge catch | IMPLEMENTED BUT UNVERIFIED | wall kick 700 out / 200 up [I]; no test of a back jump that releases the rope and grabs; B, C agree |
| M25 | Grapple pull (Interact) on a pull target: door, statue, pillar, gear, lever post, block with ring, screw | R | R | R | R |   |   |   | R |   |   |   | R | R | R | o |   |   |   |   |   |   | 9 | `src/game/Grapple.cpp` pull → uber objects in `src/game/Objects.cpp` | PARTIALLY IMPLEMENTED | `GameReal.GrapplePullsUberTargets`, `GameReal.MountedGrapplePullsBlockWithoutSwinging`, gate 751; manor door, kraken pillar / gear, gate 45 and l4_pool screw 5611 not covered. B / C rate it IMPLEMENTED, A PARTIALLY IMPLEMENTED: least favourable taken |
| M26 | Lara's hanging weight on the rope operates a lever / mechanism |   |   |   |   |   |   |   |   |   |   |   | R |   |   | R |   |   |   |   |   |   | 2 | `src/game/Grapple.cpp` (pull handler only; rope does not load a joint) | PARTIALLY IMPLEMENTED | `GameReal.GrappleLeverJointIndependentOfRenderRate` (l4_trailstart 6695 by the pull handler); l4_pool 5511 only swings; B, C agree |
| M27 | Rope snagged round an object drags / topples it (tug with Interact) |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | rope bends are geometric only (`Game.GrappleRopeBendsRoundGeometry`); l4_temple statue head 5236 [U] |
| M99 | Rope length adjust while hanging on the grapple | o |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Grapple.cpp` | IMPLEMENTED | A only: GP "Grapple" (rope 553.1..4424.78 [O], speeds [I]); the binding against "Interact held + Up / Down" is unchecked [U] |

### Water

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M29 | Swim: surface, dive, 3D under-water swim, surface for air, wade, climb out; breath / drowning |   | R | o | R |   |   | R |   |   | R |   |   |   |   |   |   |   |   | R |   |   | 6 | `src/game/Water.cpp` (swim states in `src/game/Player.cpp`) | IMPLEMENTED | `Game.SwimSpeedMatchesTru`, `Game.SwimTurnLagsLikeTru`, `Game.SwimSprintMatchesTru`, `Game.WaterAirDrainsRefillsAndDrowns`, `Game.SwimEnterCrossClimbOut`, `Game.WadingClipsAtTheDecodedDepth`. Swimming itself is VERIFIED AGAINST ORIGINAL in A, B, C; A rates climb-out / wading (its second row, merged here) IMPLEMENTED: least favourable taken |
| M30 | Under-water interaction: pick up an item, turn a handle / wheel, use a switch while submerged |   | R |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   | R |   |   | 3 | `src/game/Objects.cpp` use / pick-up (no swim-specific path) | PARTIALLY IMPLEMENTED | A's "insert / turn puzzle handles (eye door)" row: PARTIALLY IMPLEMENTED, uncovered (PR: l1_seafloor eye pieces are physics objects [U]); B / C: IMPLEMENTED BUT UNVERIFIED (use places Lara standing; gate 9103 is not swum). Least favourable taken |
| M85 | Scuba: no air limit on diving levels |   | R |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   | 3 | `src/game/World.cpp` breath rule (scuba flag in `src/game/Game.hpp`) | IMPLEMENTED | A, C: unknown at v0.41.0. changes.md (0.42.0): `GameReal.SeafloorScubaSwimKeepsTheBreath`, `GameReal.ThailandDiveDrownsAtTheBreathTime`; l7_arctic_sea unverified |
| M86 | Carry and seat puzzle items / heavy objects while swimming |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   | 2 | `src/game/Objects.cpp` portables (`carryReady`, `insertHeld`) | PARTIALLY IMPLEMENTED | `ProgressionReal.UberReceptaclesTakeTheirPortables` (l7_arctic_sea 9274 → 9099, 9270 → 9120); l1 eye pieces not covered; swim carry clips [I]. C rates it IMPLEMENTED, A PARTIALLY IMPLEMENTED: least favourable taken |
| M100 | Under-water combat: pistols, spear gun, grenades, dodge |   | R |   |   |   |   | o |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/game/Combat.cpp` weapons in water; dodges in `src/game/Player.cpp` | IMPLEMENTED | A only: PARTIALLY IMPLEMENTED at v0.41.0 (`CombatReal.PoolSpeargunUnderWater`). changes.md (0.42.0): `GameReal.SeafloorSpearKillsShark`, `GameReal.SwimDodgesMoveAsTheirClips`, `game_selftest_swim_moves`; grenades under water have no named test |

### Combat

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M28 | Precision / manual aim (free reticle) for grapple targets, mechanism parts, downed animals |   |   | R |   |   |   | o | o |   |   |   | R | o | o |   |   |   | o |   |   |   | 7 | `src/game/Aim.cpp` | IMPLEMENTED | A, B, C: NOT IMPLEMENTED at v0.41.0. changes.md (0.42.0): `Combat.ManualAimHitsWhatTheReticleCovers`, `ProgressionReal.KrakenChainLinksByManualAim`, `game_selftest_manual_aim` (l1_kraken_room, l4_pool); elsewhere unverified |
| M31 | Gunplay: lock on, re-lock, pistols / SMG / shotgun / rifle, weapon switch, reload |   | R |   |   | R |   | R | R | R | R | R | R | R | R | o | R | R | R | R |   |   | 15 | `src/game/Combat.cpp` | IMPLEMENTED | `Combat.PistolsLockFireAndHit`, `Combat.LockOnSelectionAndSwitch`, `Combat.ShotgunPelletsInTheCone`, `Combat.WeaponSwitchKeepsRoundsAndReloadsFromReserve`, `CombatReal.WeaponData` [O data]; lock tiers [I]; A's two rows (lock / fire; weapon change) merged; A, B, C agree |
| M32 | One-handed pistols while hanging from a ledge / handhold / column / pole |   |   |   |   |   |   | o | R |   |   | R |   |   | R |   |   | R |   |   |   |   | 5 | `src/game/Combat.cpp` one-handed aim (`oneHand`) | IMPLEMENTED | A, B, C: NOT IMPLEMENTED at v0.41.0 (rope / bike only). changes.md (0.42.0): real-level test in `tests/test_combat.cpp` (pistols from l4_pool ledge 1253 on a panther); other hang types unverified |
| M33 | Tranquilizer gun (animal sleeps), then kill the sleeper |   |   |   |   |   |   | o |   |   |   |   | R | R |   |   |   |   |   |   |   |   | 3 | `src/game/Combat.cpp` (+ sleep in `src/game/ai/Enemy.cpp`) | IMPLEMENTED | `CombatReal.RifleAndTranquilizerClips`, `CombatReal.PoolRifleAndTranquilizer`, replay `panther_fight`; sleep length [I]; A, B agree |
| M34 | Sticky grenades (throw, stick, blast hurts Lara too; shoot to detonate) |   | o |   | o | o |   | o | o |   |   | o | o | o | o | o | o | o |   | o | o |   | 14 | `src/game/Gadgets.cpp` | PARTIALLY IMPLEMENTED | `Gadgets.GrenadeKillsDummyAndHurtsLara`; detonation by a bullet and crouch / perch placing missing (GP "Deviations"). A rates it IMPLEMENTED, B / C PARTIALLY IMPLEMENTED: least favourable taken |
| M35 | Adrenaline head shot |   |   |   |   |   |   | o |   |   |   | o |   |   | o |   |   | o |   |   |   |   | 4 | `src/game/Combat.cpp` adrenaline / head shot (`src/game/Aim.cpp`) | IMPLEMENTED | A: PARTIALLY IMPLEMENTED, B / C: IMPLEMENTED BUT UNVERIFIED at v0.41.0. changes.md (0.42.0): `Combat.HeadShotConditionsMissAndInterrupt`, `CombatReal` head shot on a real panther (l4_pool), `game_selftest_adrenaline`; fill / slow factor [I] |
| M36 | Situational adrenaline: scripted slow motion with one required input |   |   | o |   |   | R |   | R |   |   |   |   |   | R |   |   | R |   |   |   | R | 6 | `src/script/Script.cpp` gamebase time-scale natives → world time scale in `src/game/Combat.cpp` | IMPLEMENTED | A, B, C: NOT IMPLEMENTED at v0.41.0. changes.md (0.42.0): `ProgressionReal.KrakenTentacleSlowMotionByItsScript`, `Combat.ScriptTimeRampEasesInAndOut`; the other sequences are unverified because their objects (barrels M106, break-away geometry M70, Natla M88) are still open |
| M37 | Enemy vault (lock + run + Jump off the enemy's back) |   |   |   |   |   |   | o | o |   |   |   |   |   | o |   |   | o |   |   |   |   | 4 | (none yet) | NOT IMPLEMENTED | no code ("vault" exists for ledges only); changes.md: still NOT IMPLEMENTED. In A it is part of the melee row (C07-33) |
| M38 | Melee: kick, flying kick (run + Interact), stomping a spider |   |   |   |   | o |   | o | o |   |   | o |   |   | o |   |   | o |   |   |   |   | 6 | (none yet) | NOT IMPLEMENTED | no Lara melee in `src/game/Combat.cpp` (only the Mjolnir pound); unmerged work on branches `agent/MeleeCombat` / `agent/GpMelee`; A, B, C agree |
| M39 | Finish a downed thrall: Interact stomp; permanent kill by head shot / grenade / knocking it into eitr or a pit |   |   |   |   |   |   |   |   |   |   | R |   |   | R | o | R | R |   |   |   |   | 5 | (none yet) | NOT IMPLEMENTED | `Enemy::getsUp` (`src/game/ai/Enemy.cpp`): only Mjolnir kills; `AiReal.ThrallsGetUpUnlessMjolnir` pins that; B, C agree |
| M40 | Enemies: bats |   |   |   |   |   |   | R | R | R |   | R |   | R |   |   |   | R |   |   |   |   | 6 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | IMPLEMENTED | `Ai.BatFliesToLaraAndBites`, `AiReal.PoachersFireAndBatsBite`, replay `bat_attack`; A, B, C agree |
| M41 | Enemies: nagas (melee / spit, wall crawling, ambush triggers) |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | IMPLEMENTED | B only: `AiReal.NagasClingDropBiteSpitAndDie`, replay `naga_drop`; no corner transitions on walls |
| M42 | Enemies: small spiders (swarm from cracks, leap on Lara, drop from walls) |   |   |   |   |   |   | R | R | R | R | R |   | R | R |   |   | o |   |   |   |   | 8 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | PARTIALLY IMPLEMENTED | `AiReal.SpidersClingDropAndBite`, replay `spider_drop`; latch-on and lairs not modelled. B / C rate it IMPLEMENTED, A PARTIALLY IMPLEMENTED: least favourable taken |
| M43 | Enemies: giant spiders (climb walls / ceilings, can be shot off) |   |   |   |   |   |   |   |   |   |   |   |   |   | R | o |   | R |   |   |   |   | 3 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | IMPLEMENTED BUT UNVERIFIED | type resolves (`AiReal.EnemyIntrosResolveToSkinnedEnemyTypes`); no behaviour test; B, C agree |
| M44 | Enemies: tigers / panthers (stalk, flee and hide, leap down) |   |   |   |   |   |   | R | R |   |   |   | R | R |   |   |   |   |   |   |   |   | 4 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp`, links in `src/game/ai/Nav.cpp` | PARTIALLY IMPLEMENTED | `AiReal.BallcourtPantherJumpsDownALink`, `Ai.CatJumpsDownALink`, replay `panther_fight`; tigers themselves and "flees when hit" untested. B rates it IMPLEMENTED, A PARTIALLY IMPLEMENTED: least favourable taken |
| M45 | Enemies: poachers (rifles, snipers, fight the panthers) |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   | 1 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | IMPLEMENTED | B only: `AiReal.PoachersFireAndBatsBite`, replay `poacher_firefight`; infighting: no evidence |
| M46 | Enemies: tiger / Mayan thralls (rise from the pool, poison spit) |   |   |   |   |   |   |   |   |   |   | R |   |   | R | o |   |   |   |   |   |   | 3 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` (generators) | IMPLEMENTED | `AiReal.PoolGeneratorSpawnsThrall`, `AiReal.ThrallsGetUpUnlessMjolnir`, `AiReal.DeathTimerAndThrallChunks`, replay `thrall_regenerate`; ranged spit: no evidence; B, C agree |
| M81 | Mjolnir: acquisition, weapon cycle, bolt, ground pound, under water |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | R | R | R | R | 5 | `src/game/Combat.cpp` (Mjolnir, `groundPound`) | IMPLEMENTED | C only: `Combat.MjolnirCycleSwingAndGroundPound`, `CombatReal.GroundPoundRings`, `StreamReal.ChapterThorGear`, replay `mjolnir_thralls`; data [O]; under-water use untested |
| M82 | Enemies: Viking / Yeti thralls (regenerate, generators, waves, spit, blows that throw Lara) |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | R |   | R | R | R | 5 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | PARTIALLY IMPLEMENTED | C only: `AiReal.ThrallsGetUpUnlessMjolnir`, `GameReal.KilledThrallBreaksIntoChunks`; no thrall attack records decoded, generator rules unread |
| M83 | Enemies: mercenaries (patrol, cover, squads, flank, reinforcements) |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   | 2 | `src/game/ai/Enemy.cpp` + `Nav.cpp` | IMPLEMENTED | `AiReal.MercsGuardZonesCoverAndAlert`, `AiReal.MercPatrolsSearchesAndReturns`, `AiReal.ShipNavGraphFlankAndInvestigate`, replay `merc_squad`; l6 generators untested; A, C agree |
| M84 | Enemies: sharks |   | R |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   | R |   |   | 3 | `src/game/ai/Enemy.cpp` + `EnemyData.cpp` | IMPLEMENTED | `AiReal.SharksSpawnSwimBiteAndDie`, `Ai.SharkStaysInsideWaterVolume`; A, C agree |
| M88 | Natla: hover positions, fireballs (push, damage, burning patches), line-of-fire cover |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | 1 | (none yet) | NOT IMPLEMENTED | C only: fireball calls stop at `weaponbase` fn 0; no projectile code |
| M92 | Breakable crates (cover, contents) and shootable exploding fuel tanks |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   | 2 | `src/game/Objects.cpp` breakables (shot via `World::uberShot`) | PARTIALLY IMPLEMENTED | `ProgressionReal.CombatCrateBreaksOnHit`; no contents, no blast damage (GP "Breakables"); A, C agree |

### Objects and puzzles

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M51 | Portable poles: pick up, carry, drop, insert in a socket, yank out, use as a pole |   |   |   |   |   |   |   | R |   |   | R |   | R |   |   |   | o |   |   |   |   | 4 | `src/game/Objects.cpp` portables / receptacles | IMPLEMENTED | `ProgressionReal.ReceptaclesTakeTheirPortables`, `ProgressionReal.PoleHoleLeverOpensRewardDoor`, replays `puzzleroom_pole_remove`, `valaskjalf_pole_lever_insert`; no insert clip; B, C agree |
| M52 | Pole / handle lever worked by Lara's hanging weight |   |   |   |   |   |   |   | o |   | R |   |   | R | R | R |   | o |   |   |   |   | 6 | `src/game/Objects.cpp` uber joints (lever slide → door joint) | IMPLEMENTED | `ProgressionReal.BallcourtPoleLeverOpensDoor`, `GameReal.BallcourtLeverDrivesDoorJoint` (door = 2 × slide [O]); l5 pole 7188 opens on a plain use, the l4 column handhold is unidentified; B, C agree |
| M53 | Carried key items / weights / small blocks: pick up, carry, set down, place in a receptacle or on a pad |   | R |   | R |   |   |   | R |   |   | R | R | R | R | R | R |   |   | R |   |   | 10 | `src/game/Objects.cpp` portables (`World::carryPortables`) | IMPLEMENTED | `ProgressionReal.UberReceptaclesTakeTheirPortables`, `ProgressionReal.TraproomSealTakesTheRoomKeys`, replay `baldrs_plates_gate`; set-down key differs (Interact vs Crouch, A); A, B, C agree |
| M54 | Throw a carried object (Grenade key) |   |   |   | R |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   | 2 | (none yet) | NOT IMPLEMENTED | `World::carryPortables` only sets down; A, B agree |
| M55 | Pressure pads with weight thresholds (Lara, objects) |   | R |   | R |   |   |   | R |   |   | R |   |   | R |   |   |   |   |   |   |   | 5 | `src/game/Objects.cpp` plates | IMPLEMENTED | `GameReal.PressurePlateIndependentOfRenderRate`, `Physics.PlatesWeighedByContactsAndProxyPushesLooseBodies`; thresholds [I] (one block suffices where the original needs two, A); A, B agree |
| M56 | Push / pull blocks, cages, statues, thrones | R |   |   |   |   |   |   | R |   |   | R |   | R | R | R |   | R |   |   |   |   | 7 | `src/game/Objects.cpp` push objects | IMPLEMENTED | `GameReal.PushBlockIndependentOfRenderRate`, `GameReal.LaraPlaysInteractionClips`, gate 4363; no block grid, sideways / upper clips missing; A, B, C agree |
| M57 | Objects pushed along a fixed track (straight + circular rail) |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Objects.cpp` uber joints (prismatic) | IMPLEMENTED BUT UNVERIFIED | B only: prismatic pushes exist (l7 Thor towers); curved tracks: no evidence |
| M58 | Wall / floor switches and levers (single pull, repeated pulls) | R | R | R |   |   |   |   | R |   |   | R |   |   |   | R |   | o |   |   |   |   | 7 | `src/game/Objects.cpp` use → script handlers (`src/script/Script.cpp`) | IMPLEMENTED | `ProgressionReal.CriticalGates` (4525, 748, 7228, 5469), `ScriptRealData.BedroomSwitchOpensHouseDoor`, `ProgressionReal.ChapelShadowPillars`; A, B, C agree |
| M59 | Push handles walked round a hub (calendar wheels, turnstile, tower tiers) |   |   |   |   |   |   |   |   |   |   | R |   | R |   |   | R |   |   |   |   |   | 3 | `src/game/Objects.cpp` push handles | IMPLEMENTED | `ProgressionReal.CalendarPushHandlesTurnTheRing`, replay `ballcourt_calendar_handle_turn`; valgrind wheel handles untested; B, C agree |
| M60 | Thor's gauntlet strength: slide a huge door / gate, rotate a hub, drag a wall, push a pillar or block, endgame panels |   |   |   |   |   |   |   |   | R |   | R |   |   |   | R | R | R |   |   |   | R | 6 | (none yet) — prerequisite player sensors now in `src/script/Script.cpp` | NOT IMPLEMENTED | B: NOT IMPLEMENTED, C: PARTIALLY IMPLEMENTED (l7 towers on their joints, `ProgressionReal.EndgameCredits`): least favourable taken. changes.md: player sensors (script vt 14) landed in 0.42.0, the rows stay NOT IMPLEMENTED until the heavy movers exist |
| M61 | Light-beam puzzle (gems refract sun, movable mirrors, crystal) |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Objects.cpp` receptacles only; beam logic (none yet) | PARTIALLY IMPLEMENTED | B only: gem receptacles pass (`UberReceptaclesTakeTheirPortables`); beam / mirror logic: no evidence |
| M62 | Timed doors / gates, timed runs |   |   |   |   |   |   |   |   |   |   | R | R | R |   | o |   | o |   |   | o |   | 6 | `src/script/Vm.cpp` script timers | IMPLEMENTED BUT UNVERIFIED | `ProgressionReal.TempleRevealReopens`, `ScriptRuntime.VolumeHandlerSleepsAcrossTicks`; no door time measured against tru.exe; B, C agree |
| M63 | Breakable jars / urns (kick with Interact, shoot, bike) releasing their treasure |   | o | o | o |   |   | o | R |   |   | R | R | R | R |   | o | o |   | o | o |   | 13 | `src/game/Objects.cpp` breakables + reward births (`src/game/Progress.cpp` reward table) | PARTIALLY IMPLEMENTED | A, B, C: PARTIALLY IMPLEMENTED (rewards not spawned). changes.md (0.42.0): urns broken by shots give their treasure (`PickupsReal.*`, `game_selftest_treasure_urn`); kick and bike breaking still NOT IMPLEMENTED. A's two rows (smash with Interact; jars with treasures) merged |
| M71 | Objects that carry Lara: lowering / falling platforms, moving block tops, platforms on splines |   |   |   |   |   |   |   | R |   |   |   |   |   | R |   |   |   |   |   |   | R | 3 | `src/game/Objects.cpp` movers (elevator, spline mover) | IMPLEMENTED | `GameReal.ElevatorCarriesPlayer`, `ProgressionReal.LoweredRampSectionCarriesLara`, `GameReal.SplineMoverCarriesLara`; standing only (hanging on moving markup is M76); B, C agree |
| M87 | Timed multi-switch sequence (four rune symbols, each on its own timer) stopping a hazard |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | o |   | 1 | (none yet) | NOT IMPLEMENTED | C only: PR gates ch 7: rune switches "not asserted (dispatcher only)" |
| M89 | Smash a power node with Mjolnir (Interact) → buttress collapse |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | 1 | `src/script/Script.cpp` stimulus → `src/game/Objects.cpp` uber state | IMPLEMENTED | C only: `ProgressionReal.EndgameCredits` (mounts 8362-8364) |
| M101 | Shoot a mechanism part (chain links) |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `World::uberShot` (`src/game/Objects.cpp`), aimed by `src/game/Aim.cpp` | IMPLEMENTED | A only: unknown at v0.41.0. changes.md (0.42.0): `ProgressionReal.KrakenChainLinksByManualAim`, `game_selftest_kraken_kill` |
| M102 | Automatic doors (trigger volumes) |   |   |   |   | R | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/script/Script.cpp` volume handlers | IMPLEMENTED | A only: `ProgressionReal.CriticalGates` (1531) |

### Hazards

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M65 | Fall damage / fatal drops, bottomless and spike pits | R |   | R | R | o | R | R | R | R |   | R | R | R | R | R | R | R | o |   |   | R | 17 | `src/game/Combat.cpp` fall damage [O 0x6F1790] | IMPLEMENTED | `Combat.FallDamageThresholds`, `Combat.FallDamageDeathAndRespawn`, `CombatReal.PoolFallDamageDeathAndPistols`. A rates it VERIFIED AGAINST ORIGINAL (curve decoded), B / C IMPLEMENTED: least favourable taken |
| M66 | Deadly liquid (eitr): contact kills; pools, falls, level changed by script |   |   |   |   |   |   |   |   |   |   |   |   |   | R | R |   | R |   | R | R |   | 5 | `src/game/Water.cpp` + script poll (larabase 3) in `src/script/Script.cpp` | VERIFIED AGAINST ORIGINAL | GP "Water" (l4_pool 5608, Wine: same outcome at eight spots), `GameReal.ScriptedWaterLevelsRun`; verified in l4_pool only, l5 / l7 pools and falling streams untested; B, C agree |
| M67 | Crushing blocks / weights (timed), riding on their tops |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   | R |   |   |   |   | 2 | `src/script/Script.cpp` larabase 6 contact force (planned: `src/game/Hazards.cpp`) | PARTIALLY IMPLEMENTED | contact force [I], crusher jaws [U]; no damage test; B, C agree |
| M68 | Flame jets (timed, damage) |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | B only: the traps animate; no fire damage to Lara |
| M69 | Spear rows and swinging blades (timed; crouch under) |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | B only: no trap damage code |
| M70 | Break-away handholds / poles, cracking beams, collapsing floors, walkways and bridges | R |   | o |   |   |   |   |   |   |   |   |   |   | R |   |   | R |   |   |   |   | 4 | (none yet) | NOT IMPLEMENTED | GP "Breakables": class-30 breakables not implemented; only bridge 6974's joint break is tested. A ("unknown / NOT IMPLEMENTED"), B, C agree |
| M77 | Steam vents: timed hazard that scalds and breaks her grip |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | R |   |   |   |   | 2 | (none yet) | NOT IMPLEMENTED | C only: no hit for steam in the source |
| M79 | Roller traps (floor, ceiling, tracking sets) |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | C only: no trap damage code |
| M103 | Fire damage / script death volumes | R |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/script/Script.cpp` death volumes (health natives) | IMPLEMENTED | A only: `StreamReal.PrologueCorridorToStaircase` |
| M104 | Jellyfish (sting on touch) |   | R |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/game/World.cpp` (0.42.0 water hazards) | IMPLEMENTED | A only: NOT IMPLEMENTED at v0.41.0. changes.md (0.42.0): `GameReal.JellyfishStingOnTouch` |
| M105 | Kraken tentacle swats |   | o | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/script/Script.cpp` (level script objects 494-496) | PARTIALLY IMPLEMENTED | A only: damage unknown (test_gates.cpp sweep note); the slow motion around them is M36 |
| M106 | Ship roll: sliding containers, rolling burning barrels |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | (none yet) | NOT IMPLEMENTED | A only, "unknown": no doc or test mentions the roll, the containers or the barrels (chapter rows C06-04 / C06-05); rated NOT IMPLEMENTED for lack of any evidence |

### Vehicle

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M47 | Motorcycle: mount / dismount, ride, brake, steer, stairs, unit crossings |   |   |   |   |   |   |   |   |   |   |   | R | R | R | R | R | R |   |   |   |   | 6 | `src/game/Vehicle.cpp` | IMPLEMENTED | `VehicleReal.TrailstartRideReplay`, `VehicleReal.BikeIntrosTuningAndClips`, `VehicleReal.SaveRestoreMidRide`, replay `bike_ride`; tested in l4_trailstart only; B, C agree |
| M48 | Motorcycle: launch boost, ramp / pit jumps, air control |   |   |   |   |   |   |   |   |   |   |   | R | R | R | R |   | o |   |   |   |   | 5 | `src/game/Vehicle.cpp` | IMPLEMENTED BUT UNVERIFIED | burnout state [O states], `VehicleReal.AirControlTurnsMidJump`; no real pit / ramp jump tested; launch wheelie not done; B, C agree |
| M49 | Motorcycle: donut / power slide hurting enemies, running enemies over |   |   |   |   |   |   |   |   |   |   |   | o | o |   | o | o | o |   |   |   |   | 5 | `src/game/Vehicle.cpp` | IMPLEMENTED | `VehicleReal.DonutAndPowerSlide`, `VehicleReal.RamsEnemyOnTheTrail` (ram damage [I]); B, C agree |
| M50 | Motorcycle: headlight; smashing jars by riding into them; bike as a weight on a pressure pad |   |   |   |   |   |   |   |   |   |   |   | o | o | R |   |   |   |   |   |   |   | 3 | (none yet) | NOT IMPLEMENTED | B only: no headlight; contact-impulse breaking not simulated; no bike-on-plate path. changes.md: bike breaking of urns still NOT IMPLEMENTED |

### Systems and collectibles

| ID | Mechanic | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | Chapters | Reusable implementation (source file / system) | Status | Evidence |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M64 | Pickups: treasures, relics, health items; counters; relic raises max health and heals | R | o | o | o | R |   | o | R | R | R | R | R | R | R | R | R | R | R | R | R | R | 20 | `src/game/Progress.cpp` reward table + pickups in `src/game/World.cpp` | IMPLEMENTED | A, B, C: PARTIALLY IMPLEMENTED at v0.41.0 (counts, relic bonus). changes.md (0.42.0): reward table 26 / 30 / 13 / 50 / 30 / 0 / 30 + 6 relics (`ChaptersReal.RewardSurveyMatchesTheRewardTable`, `PickupsReal.*`), `Pickups.RelicRaisesMaxHealthAndHeals` |
| M72 | In-engine cutscenes at triggers, reveal cameras, section / chapter changes | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | 21 | `src/cine` + `src/game/Camera.cpp` reveal cameras + `src/game/Stream.cpp` chapter changes | IMPLEMENTED | `CineReal.EveryStreamPlaysWithItsActors`, `CineReal.ChapterEndCinematics`, `GameReal.RevealCameraPlaysUnitSpline`, `ProgressionReal.CriticalPathPerChapter`; blends [I]; A's two rows (cinematics; reveal cameras) merged; A, B, C agree |
| M73 | Checkpoints, autosave, manual save / load, respawn | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | R | 21 | `src/game/Stream.cpp` checkpoints + `src/save/Save.cpp` | IMPLEMENTED | `Stream.CheckpointAndRespawn`, `Save.*`, `StreamReal.SaveContinuesBitIdentically`, `StreamReal.AirborneSaveResumesAtCheckpoint`; puzzle-object and bike state at a respawn untested; A, B, C agree |
| M74 | Burning-manor escape (the section 01 route replayed) |   |   |   |   |   |   |   |   |   |   | R |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Stream.cpp` (same unit and scripts as section 01) | IMPLEMENTED | B only: `StreamReal.PrologueCorridorToStaircase`, `ChaptersReal.CroftManorEndingRunsBothCinematics`, `FxReal.PrologueFireScreenFilter` |
| M75 | Outfit / weapon choice at level start |   |   |   |   |   |   | o |   |   |   | R | R |   |   |   | R |   | R | R |   |   | 6 | `src/ui/Ui.cpp` (EXTRAS outfits; no per-level choice screen) | PARTIALLY IMPLEMENTED | `Menus.ExtrasOutfits` (ours), `Menus.PdaWeaponsInventoryAndSonar`; the original's per-level choice screen is not reproduced; A, B, C agree |
| M93 | Ending flow: end save, credits, Treasure Hunt / revisit |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | R | 1 | `src/ui/Ui.cpp` + `src/game/Progress.cpp` | IMPLEMENTED | C only: `ProgressionReal.EndgameCredits`, `Menus.MainCentreTreasureHuntAndRevisit`, `Progress.ProfileAndTreasureHuntSave`; rules [I] |
| M107 | Utility light |   | o |   |   |   |   | o |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 2 | `src/game/Gadgets.cpp` | IMPLEMENTED | A only: `Gadgets.FlashlightToggleCooldownAndPose`, `GadgetsReal.TuningLightsAndSpotUnits`; light data [O] |
| M108 | Field camera |   |   | o |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 1 | `src/game/Camera.cpp` | IMPLEMENTED | A only: `Game.FieldCameraTogglesEyeView` |
| M109 | PDA: journal, weapons, inventory, sonar, area totals |   | R |   |   | R | R | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 4 | `src/ui/Ui.cpp` | IMPLEMENTED | A only: `Menus.PdaJournal`, `Menus.PdaWeaponsInventoryAndSonar`; A's caveat (totals wrong where jars hold treasures) is closed by the 0.42.0 reward table (see M64) |
| M110 | Training text / helper buttons | R |   | o |   |   |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 3 | `src/script/Script.cpp` tutorial natives → `src/ui` | PARTIALLY IMPLEMENTED | A only: `ScriptRealData.CliffclimbVolumeShowsTutorialPopup`; helper buttons stored only |
| M111 | Health model: regeneration to a quarter, health-pack use | R | R |   | R | R |   | R |   |   |   |   |   |   |   |   |   |   |   |   |   |   | 5 | `src/game/Combat.cpp` | IMPLEMENTED | A only: `Combat.RegeneratesToQuarterOnly`, `Combat.HealthPacks`. A rated its row PARTIALLY IMPLEMENTED only for the missing relic bonus, which is M64 and landed in 0.42.0 |
