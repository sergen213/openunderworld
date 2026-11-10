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

### Requirements
| ID | Where in the chapter (own words, in route order) | Mechanic(s) exercised | Our unit / object uid / gate if identifiable | Status | Evidence | Visual reference |
|---|---|---|---|---|---|---|
| C10-01 | Step onto the broken bridge: spiders come out of wall cracks | M42 | l2_maproom [I] | IMPLEMENTED BUT UNVERIFIED | generic spider tests; lairs not modelled (GP "Behaviour") | — |
| C10-02 | Dive to the pool floor right of the far bridge stub for treasure 29; swim back and climb out on the small ledge by the near stub | M29, M30 (pickup submerged), climb-out | l2_maproom [I] | IMPLEMENTED BUT UNVERIFIED | `Game.DiveSwimDownAndSurface`, `Game.SwimEnterCrossClimbOut`; pickup while swimming untested | screenshots/puppet1.jpg |
| C10-03 | Running jump off the right corner and throw the grapple at the wall ring in the air (or throw first, then jump towards the ring); a plain step-off ends in the water with the rope released | M21, rope released on water entry | l2_maproom anchor 2676 / 2595 [U which] | IMPLEMENTED BUT UNVERIFIED | no test for an airborne throw or for release on entering water | details/puppet2.html |
| C10-04 | Hanging on the wall: set the rope so her feet are ~2 ft above the water; wall run back and forth until the arc passes the light / dark line on the wall | M23, rope length | l2_maproom | IMPLEMENTED BUT UNVERIFIED | `GameReal.ValaskjalfWallRun` (l5), numbers [I] | details/puppet2.html |
| C10-05 | At the left apex release Left, press Back + Jump: she springs off the wall and grabs the bridge end behind her; pull up | **M24** | l2_maproom | IMPLEMENTED BUT UNVERIFIED | GP "Grapple": jump on the wall "kicks off (700 out, 200 up) into a swing" — ours stays on the rope, so the original's release-and-ledge-grab is probably not reproduced; unknown until tested | details/puppet2.html |
| C10-06 | Back-left corner: two slab handholds, shimmy left, jump to the pole sticking out of the wall and keep hanging: her weight draws it down its track, the underwater door opens fully | M02, M07, **M52** | l2_maproom pole 2665 on track 2664 (`sa_pt_pole_maproom` in `sa_pt_phole_track`) | NOT IMPLEMENTED | GP "Interactions" "Not done: … l2_maproom's track slide"; the pole starts inserted there ("Placed portables" [O]) but does not slide | — |
| C10-07 | Drop into the water, swim through the opened door | M29 | l2_maproom → l2_flushtunnel portal | IMPLEMENTED BUT UNVERIFIED | depends on C10-06; streaming generic (`Stream.CrossesPortalLoadsNeighboursUnloadsOld`) | — |
| C10-08 | Tunnel: swim to the ceiling air pocket, breathe, dive again | M29 (breath refill at a pocket) | l2_flushtunnel | IMPLEMENTED BUT UNVERIFIED | refill at any surface 2 s [O]; that the pocket is a water surface in our volume data is unknown | — |
| C10-09 | Treasure 30 in a round hollow on the floor half-way to the exit, before the steep rise | M30, M64 | l2_flushtunnel | IMPLEMENTED BUT UNVERIFIED | pickup rule [I] | screenshots/puppet9.jpg |
| C10-10 | At the carved doors swim up to the wheel and Interact to turn it; swim through | **M30** (use under water) | l2_flushtunnel switch 2543 (`sa_ei_switch_oneway_flushtunnel`) → gate 2541 | IMPLEMENTED BUT UNVERIFIED | `CriticalGates` l2_flushtunnel 2543 Use → 2541 and sweep PASS prove the script; the test places Lara at the switch — a swimming use (State::Use while submerged, clip, alignment) is untested | — |
| C10-11 | Crossing the tunnel end arms the level end | plane trigger | l2_flushtunnel plane 2542 → unit script 9502 vt 26 (sticky +40 on 2564) | IMPLEMENTED | `CriticalGates` case "BeatLevel plane, then the boat" | — |
| C10-12 | Surface, swim to the boat: level-end cinematic, next chapter Croft Manor | M29, M72 | l2_cliffclimb volume 2564 → cinematic 2565 → chapter 3 | IMPLEMENTED | same `CriticalGates` case (chapter → 3), `CineReal.ChapterEndCinematics`, `GameReal.ScriptedChapterEndChainsStartNextChapter` | — |

### Gaps
- **Weight pole on its track (C10-06, M52)** is the only way to open the underwater exit and is explicitly "not done". Critical path.
- **Wall run + back jump to a ledge (C10-05, M24)**: the original releases the rope and catches the ledge behind; ours documents only a kick-off into a free swing. Critical path, unverified / probably different.
- **Airborne grapple throw and rope release on hitting water (C10-03)**: no evidence either way.
- **Under-water Interact (C10-02, -09, -10; M30)**: treasure pickups and the wheel are untested with Lara swimming; the wheel plays a standing lever clip [I].
- Air pocket (C10-08): depends on water-volume data; not checked in l2_flushtunnel.
- Spider swarm trigger untested.

### Proposed self-test scenarios
1. `puppet_wallrun_backjump` — l2_maproom, spawn on the near bridge stub's right corner. Input: run + Jump, Grapple in the air, adjust rope, alternate Left / Right 4×, at the left apex Back + Jump, then Up. Assert: attached before touching water; apex height over the water each pass; after the back jump state = ledge hang on the far stub's edge polyline; ends standing on the far stub.
2. `puppet_pole_track_door` — l2_maproom, spawn on the far stub. Input: climb the two slabs, shimmy, Jump to pole 2665, hang 6 s, Drop. Assert: track 2664's slider reaches its lower limit while she hangs (time recorded), the underwater door object is open, it stays open after she lets go. Expected FAIL today.
3. `puppet_tunnel_swim` — l2_flushtunnel, spawn at the tunnel mouth under water. Input: swim forward, surface at the air pocket, dive, Interact at the treasure, Interact at wheel 2543, swim on through plane 2542. Assert: breath never reaches 0 and refills at the pocket; treasure count +1; gate 2541 open after a use made while swimming; plane 2542 sets 2564's sticky.
4. `puppet_boat_level_end` — l2_cliffclimb after scenario 3 (state carried). Input: surface, swim to the boat volume 2564. Assert: cinematic 2565 plays, chapter becomes 3, start unit l3_caverns_start (already asserted by `CriticalGates`; here reached by swimming, not a teleport).
