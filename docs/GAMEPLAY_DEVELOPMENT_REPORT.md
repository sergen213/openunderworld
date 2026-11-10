# Gameplay reconstruction: development report

State of the work toward a 1:1 gameplay reconstruction of Tomb Raider: Underworld (Windows PC) in OpenUnderworld.
This report is the summary; the evidence is in the documents it links. Labels as everywhere in the project:
**[O]** taken from the original (tru.exe code, game data, or a measurement of tru.exe running under Wine), **[I]**
inferred, **[U]** unknown. "Verified" below always means: compared against the original, not merely tested against
ourselves.

Reconstruction is **not complete**. What is done, what is only tested against ourselves, and what is missing is
listed without rounding up.

## 1. Documents

| Document | What it holds |
|---|---|
| `docs/GAMEPLAY_AUDIT.md`, `docs/audit/AUDIT_A.md`, `AUDIT_B.md`, `controls.md` | The audit of the code base against the goal: 579 goal bullets classified, prioritized work lists |
| `docs/underworld_chapter_gameplay_matrix.md`, `docs/matrix/NN_*.md`, `docs/matrix/shared_mechanics.md` | Requirements per walkthrough section (21 sections, 661 rows, 15 categories) and the shared-mechanics table (111 mechanics x 21 sections), each with a status |
| `docs/REFERENCE_COMPARISON.md` | Per mechanic: the original's numbers and their source, ours, the difference; which rows rest on live measurements |
| `docs/GAMEPLAY.md` | The rules of every mechanic as established, with addresses, data records and trace files |
| `docs/formats/PHYSICS.md` | The Jolt backend, the Havok -> Jolt mapping, the character controller evaluation |
| `docs/SELFTEST.md`, `tests/selftests/` | The scripted gameplay tests of the real game and how to write them |
| `docs/PROGRESSION.md` | Critical-path gates per chapter and how each is opened |
| `docs/ROADMAP.md` | One row per system: status, evidence, tests, what is open |

## 2. Audit findings (v0.41.0, before this work)

The audit classified the 579 requirement bullets of the goal against the code. The main findings:

- The systems existed and were tested (movement, ledges, poles, beams, swimming, grapple, combat, vehicles, scripts,
  streaming, saves), but large parts of them were **tested only against ourselves**; of the 661 chapter requirement
  rows 8 were verified against the original, 95 implemented and tested, 319 implemented but unverified, 135 partial
  and 100 missing.
- Whole mechanics were missing: chimney jumps, close combat, most collectibles (101 of 179 treasures were inside
  urns that could not be opened), manual aim and the adrenaline inputs, the under-water moves, breakables, Thor's
  gear, traps that hurt, markup on moving objects, checkpoint restore, the controller's input rules (saving grab,
  safety drop, landings).
- Several things were wrong in every level and nobody had noticed, because tests placed Lara instead of playing:
  ladders stood turned 90 degrees in their walls, one-sided trigger planes fired from the wrong side, pressure
  plates ignored weight, any heavy object slid when Lara stood next to it.
- There was no way to test the real game by input. Critical-path tests teleported between gates.

## 3. Physics architecture

- **Jolt Physics 5.6.0** (`src/physics`, hidden behind our own API in game units) simulates: jointed objects
  (hinges, sliders, the three-axis joint of Thor's pillars) with the data's limits, motors and brakes; loose bodies
  (thrown and dropped objects, freed sections, breakable pieces); ragdolls; the motorcycle; explosions' pushes.
  Deterministic, single-threaded, saved bit for bit. Mapping and evidence: `docs/formats/PHYSICS.md`.
- **Lara's mover stays our own kinematic sweep.** Jolt's `CharacterVirtual` was built in behind the mover
  interface and measured against the original on the manor stairs, step heights, the hearth edge, slopes, two
  replays, save continuity and cost, in two shapes. It failed the decision rule in both (8 of 26 comparison tests,
  risers, stair speed, root height on slopes, saves). The reason is structural: tru.exe's proxy floats 141.6 above
  her feet and the root takes the floor height under it, and a shape resting on the ground does not reproduce that.
  Tables and reasons: `docs/formats/PHYSICS.md` "Character controller".
- Level collision for Lara is our BVH with the original's face flags; Jolt holds its own static meshes for the
  bodies. The two are kept in step by the object sync (`World::syncObjects`).

## 4. What was built (releases 0.42.0 - 0.46.0)

Each line names where the rules and evidence are. "Wine" = measured in the running original.

| Area | Built | Evidence class |
|---|---|---|
| Self-tests | `ou-game --self-test`: scripted input in the real game, measured outcomes, JSON report; scenarios under ctest | ours |
| Chimney jumps | Wall plant, rebound, five in a row, ledge catch | [O] chart + data; Wine at l2_junglehub (holds, hold time, rebound, five-in-a-row) |
| Collectibles | 179 treasures / 6 relics, urn births, relic health | [O] data |
| Aim and adrenaline | Manual aim, head shot, focused fire, dual targets, one-handed hang shooting, script time scales, kraken kill | [O] chart + decomp; cameras not Wine-measured |
| Water | Scuba rule, breath, dodge, swan dive, climb-out, weapons in water, jellyfish | [O]; steering fitted to a Wine trace |
| Close combat, carry | Kicks, fatality, vault, urn kicks; pick up / put down / throw on the original's keys, encumbered walk, carried objects colliding | [O] chart + action map; carry sweep [I] |
| Mediterranean puzzles | Plate motors by weight, use records, grapple pulls on the object's joint, the tomb's full solution by input | [O] data; one-vs-two blocks not Wine-measured |
| Input rules | Input timing layer, saving grab by fall height + Interact, safety drop, ladder bad attach | [O]; Wine at l4_temple |
| Grapple | Roped walk, jump keeping the rope, leaps, rope climb / turn inputs, swing drive table, wall-run drive | [O] chart + decomp; **swing not Wine-measured** (five sessions failed) |
| Thor's gear | Gear required by the engagement records, push / pull on the joint, three-axis pillars, Valgrind gate natives | [O]; push speed's proxy mass [I] |
| Breakables, explosions | Class-30 breakables, explosion records, freed sections, plane side, ladder frame | [O] |
| Controller, part 2 | Landings, jump brake, pull-up / handstand, Interact hustle, run / sprint turn and stop | [O]; Wine at l4_temple |
| Checkpoints, loadout, tailoring | Death restores the checkpoint save, level-start loadout, revisit weapon page, Player Tailoring | [O] decomp |
| Hazards | Damaging collision primitives, trap scripts, crusher jaws, fire volumes | [O]; overlap-vs-solid split [I] |
| Moving markup | Object markup per model and segment, riding, carrier velocity, saved attachment | [O] decomp + data |
| Natla | Fireballs, blast and burn, anchors, the arena pool | [O] scripts + weapon records |
| Motorcycle | Brake on Jump, launch boost, reverse, standing start, the first ride by autopilot | [O]; Wine traces of launch, reverse, standing start |
| IK | Limb locks (feet on object floors, hands in the hang idle), head look-at, second-target arm | [O] decomp; finding: the original has **no** terrain foot IK |

## 5. Testing

- **Unit and real-data tests** (gtest, `tests/test_*.cpp`): 658 at the start, more than 860 ctest entries now.
  Real-data tests load the user's game files and are skipped without them.
- **Self-tests** (`tests/selftests/*.json`): the real game is started, played by scripted input or the autopilot,
  and the outcome is measured (positions, speeds, states, clips, object states). They run under ctest in a
  private compositor.
- **Replays**: recorded input replayed tick for tick with asserted outcomes; re-recorded from the plan in each
  file's header whenever a rule change moves them.
- **Saves**: every saved field is hashed; `StreamReal.SaveContinuesBitIdentically` continues a loaded save next to
  the original run and compares hashes every tick. Each mechanic that adds state adds a case.
- **Against the original**: tru.exe runs under Wine with a memory-trace harness. `docs/REFERENCE_COMPARISON.md`
  lists every row with its source; rows that rest only on decoded code or data say so.
- A full run is `ctest -E game_vulkan_rt_smoke` in a private compositor; every release in this series was cut from
  a tree whose full run was green, or green but for failures named in the release commit and fixed or parked
  before the tag (0.42.0, 0.43.0, 0.45.0, 0.46.0 say which).

## 6. What is verified, tested, or open

- **Verified against the running original** (Wine): flat-ground speeds and jumps, stairs and step heights, chimney
  jump holds and timing, saving grab heights, safety drop, landings and the roll input, pull-up / handstand, shimmy
  hustle, run / sprint turn and stop, bike launch / reverse / standing start, swim steering, slope standing /
  sliding faces. See the comparison document for the numbers.
- **Decoded from code or data, tested against ourselves, not measured live**: most of the rest of section 4. The
  comparison document counts 107 of 161 rows as unmeasured.
- **Measured differences fixed since they were found**: jump take-off timing, run start, chimney rebound height,
  wall attach phases (0.46.0), slides by face flag (0.46.0). Still differing or unexplained: the chimney reach /
  standoff pair, the bike's brake strength and slope behaviour, the push speed of Thor's mechanisms (fitted to
  one block trace, not measured themselves).

## 7. Chapter coverage

The matrix (`docs/underworld_chapter_gameplay_matrix.md`) holds the status of every requirement row per section:
661 rows, of which 9 are verified against the original, 133 implemented with a test in that chapter's units, 281
implemented but unverified there, 146 partial and 90 missing (baseline at v0.41.0: 8 / 95 / 319 / 135 / 100). Rows
changed only where a chapter leg or test actually exercised them; the matrix's section "Status changes since the
baseline" lists the mechanics that landed and the rows they affect.

Chapter walkthrough legs (`tests/selftests/chNN_*.json`) play parts of each section's critical path by input,
each leg starting at a checkpoint (a few start at a spot that is not one; the scenario says so). **No section plays
end to end yet.** Legs that cannot be played are kept under `tests/selftests/known_gaps/` with the cause.

| Section | Legs that pass | What stops the section |
|---|---|---|
| 01 Prologue | 4 | The pulled ring door's leaf and the blown hall door's leaf can stay standing in their frames; no input route into the pocket north of the bedroom beams |
| 02 Path to Avalon | 2 | The eye door, the flooded corridor and the entrance's ledge / pole chain are not played (the autopilot could not steer under water when they were written) |
| 03 Niflheim | 2 (+ the kraken kill) | Stair halls, the pit, walkways, the pool and the stairs' climb are not played |
| 04 The Norse Connection | 4 | The jump to perch 30 in the column hall; the boat at the end |
| 05 God of Thunder | 3 | The way west from the T junction below decks is shut; the upper-deck fights are not played |
| 06 Realm of the Dead | 2 | The hallway roll stops her short of plane 1203; the bar jump out of the second shaft |
| 07 Remnants | 3 | Most of the cliff, the far side of the grapple gap, the colonnade and the streambed climb are not played |
| 08 Bhogavati | 4 | The statue's floor slabs stay solid after the pull; `multibody` 3 unbound; the lift drop; most traversal chains |
| 09 The Ancient World | 3 | Everything after the first hub turn |
| 10 Puppet No Longer | 1 | The roped leap to the wall ring ends in the pool; pole 2665 does not slide on its track; the tunnel and the boat |
| 11 Protected by the Dead | 3 | The fights (an unarmed walk dies to the spiders); the serpent pillar leg is not written |
| 12 The Unnamed Days | 1 | Lever 6695 is not held by her weight on the rope; the temple interior |
| 13 Xibalba | 3 | Key items do not travel between units; ring 4951 is not offered |
| 14 The Midgard Serpent | 4 | The pool walkway gap; no motorcycle after a level entry; thrones stop at an unbound native |
| 15 Land of the Dead | 3 | Rune block 5609 has no joint; the key hand-over between units |
| 16 Gate of the Dead | 2 | The tower's base panel stalls short of its solved position |
| 17 Valhalla | 1 | The hammer chamber's ledge; the timed lever runs have no walkable route in ours |
| 18 Rituals Old | 2 | The deck fights (the autopilot does not fight) |
| 19 Helheim | 4 | The statue wheels, gears, the other stones, the flooded maze |
| 20 Yggdrasil | 4 | The rune symbols cannot be used; the slide's jump falls short; the eitr pit is plain water |
| 21 Out of Time | 0 (+ the arena entry) | No way down from the entry walkway; Natla cannot be hit; the fight's sequence is unplayed |

The legs of sections 07-09 and the work on the prologue's doors and on Natla's fight were still being integrated
when this report was written; `docs/SELFTEST.md` lists what is registered.

## 8. Remaining work

In the order it blocks the campaign:

1. The blockers of section 7, each recorded with its cause in `docs/SELFTEST.md` (known-gaps table) and in the
   matrix rows. The ones that recur across sections: key items carried between units, the motorcycle after level
   entries, freed door / slab bodies that stay in place, fights by the autopilot (a test-tool gap, not a game gap).
2. Natla's fight by input past the arena entry.
3. The legs not yet written (most of sections 02, 03, 07-09, 17-19), then joining legs into whole sections.
4. Push blocks and Thor's mechanisms: the push law is fitted to one block trace; the mechanisms themselves are
   unmeasured.
5. The motorcycle's brake strength, slopes, coasting, and the unmeasured manoeuvres.
6. The ship's escape shafts, burning barrels, roll and encounter counts.
7. The grapple swing measurement, and the other unmeasured rows of the comparison document.
8. Per-row refresh of the matrix for rows no leg has exercised (281 rows are "implemented but unverified").
