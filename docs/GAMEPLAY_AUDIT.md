# Gameplay audit (baseline: v0.41.0, main 4437cb5)

Audit of the existing gameplay implementation against the gameplay-reconstruction goal: what exists, what works, what
works but is not the original's behaviour, what is partial, placeholder, missing, incorrect or needs refactoring, and
the prioritized work that follows. Made 2026-11-10 by reading the code and the tests (the docs were treated as leads,
not as proof) and by reading all 21 chapter pages plus the controls page of the PC walkthrough.

Parts:

| File | Covers |
|---|---|
| `docs/audit/AUDIT_A.md` | Movement, jumping, ledges, climbing, chimney jumps, poles, beams, grapple, swimming, animation, camera / input, the state machine: one row per goal bullet (306 rows) with code locations, tests, provenance of the numbers, what is missing; worklist A-P1..P18 |
| `docs/audit/AUDIT_B.md` | Jolt Physics integration, combat, weapons, objects, puzzles, motorcycle, game systems and progression, test infrastructure, quality: one row per goal bullet (273 rows); Jolt integration as built; the CharacterVirtual evaluation plan; grapple physics; weapons table; collectibles; worklist B-P1..P19 |
| `docs/audit/controls.md` | Every Windows PC control and gameplay rule of the controls page against our action and code path, with the input differences found |
| `docs/underworld_chapter_gameplay_matrix.md` + `docs/matrix/NN_*.md` | The 21 chapters: 661 requirement rows with status and evidence, per-chapter gaps and self-test scenarios |

Labels as elsewhere: [O] observed in the game data / tru.exe, [I] inferred, [U] unknown.

## What the project is

- C++23, CMake, 49 k lines of gameplay / physics / tests in `src/game`, `src/physics`, `src/app/game`, `tests`.
  `World::tick` is one fixed 1/60 s step; rendering interpolates; nothing gameplay-relevant reads the frame time
  (`game_render_rate_smoke`, real-puzzle tests bit-identical at 24-1000 Hz).
- Lara: an animation-driven kinematic controller of our own (`Player.cpp` 2.8 k lines + `Climb.cpp`, `Traverse.cpp`,
  `Grapple.cpp`): root motion from lara.drm's clips, a capsule swept on our own collision BVH, traversal by the
  levels' markup (ledges, poles, beams, ladders, pillars, perches, free-climb holds, rails), tru.exe's jump queries
  and solver, anim requests with the original fades. Many numbers are measured in tru.exe under Wine.
- Jolt Physics 5.6.0 (pinned, deterministic, single-threaded, one step per tick, no substeps) simulates joints and
  doors, uber-object joints with limits / motors / links, pressure plates, loose bodies with buoyancy, ragdolls and
  the motorcycle (`VehicleConstraint`). It does not move Lara, the grapple rope, push blocks, enemies or projectiles,
  and no level query goes through it (AUDIT_B "Jolt integration").
- The SCMD script VM runs the levels' own scripts; 251 data-derived gates are swept (136 pass at the baseline);
  `ProgressionReal.CriticalPathInOrder` goes from the prologue to the credits, but through gate activations and
  placements, not by walking the route with input.
- 675 automated tests (gtest suites, 28 input replays of the real binary); since this audit also
  `ou-game --self-test` scenarios (`docs/SELFTEST.md`).

## Classification (rows of AUDIT_A + AUDIT_B, 579 goal bullets)

| Classification | A (movement .. state machine) | B (physics .. systems) | Total |
|---|---|---|---|
| implemented + works | 162 | 129 | 291 |
| works but differs from the original | 16 | 49 | 65 |
| partial | 82 | 52 | 134 |
| missing | 35 | 28 | 63 |
| incorrect | 4 | 5 | 9 |
| needs refactor | 6 | 10 | 16 |
| placeholder | 1 | 0 | 1 |

Walkthrough requirements at the baseline (661 rows): 8 verified against the original in their chapter, 95
implemented with a test in their chapter, 319 implemented but unverified there, 135 partial, 100 not implemented.

## Main findings

Works and is the original's (measured or decoded): walk / run / sprint with fatigue, turn in place, stairs and step
limits, standing / stick / running jumps and their targeting, fall and fall damage, ledge hang / shimmy / corners /
climb-up / hang jumps with leans, hurdles, free climbing with DICE jumps, ladders, pillars, perches, wall hug, rail
slides, balance beams with the off-balance controller, horizontal poles with flings, slope slides, swimming states
and speeds, the follow camera's rest pose and force sets, the bike's engine and wheels from its data records, health /
regeneration / fall-damage curves, weapons data, enemy senses and behaviour trees, the music player.

Missing mechanics the campaign needs (each is on some chapter's critical path):

1. Chimney / wall jump (no code; critical in 8 of the 21 sections).
2. Lara's melee: kicks, jump kick, stomp, enemy vault; thrall finishing (thralls cannot be destroyed before Mjolnir).
3. Manual (precision) aim; shooting one-handed from ledges / ladders / poles; dual targeting [U].
4. Scripted slow motion ("situational adrenaline"): the time-scale natives are bound but not applied.
5. Jar / urn treasures: 78 of the 179 treasures exist; the 101 spawned by breaking jars do not. Relics have no
   health effect. Breakables break only from shots (no kick, roll, fall or bike contact; class-30 breakables absent).
6. Thor's gauntlets: player sensors never fire; no heavy-object movers (doors, pillars, hub, wall).
7. Moving traversal geometry: ledges / poles / holds do not ride rotating rings, machinery or spline movers.
8. Trap hazards (flame jets, spears, blades, crushers, steam, rollers), Natla's fireballs, explosions' damage.
9. Under-water rules: scuba breath, weapon rules, evasive roll, swan dive, jellyfish, scripted water levels.
10. Throwing carried objects; carried objects ignore collision; set-down by teleport.
11. Level-specific sequences without evidence or passed by shortcuts: the eye door, kraken sequence, ring door,
    collapsing floors, ship boarding, temple block topple, calendar solutions, tower puzzle, rune circuit, the final
    fight by input (details per chapter in the matrix).

Works but differs from the original (inputs and rules): grapple jump-in / leap-off / rope inputs and swing pumping;
saving grab key and trigger; safety drop needs a key; climb-out of water; Interact speed-up of climbing absent; no
hold-time input layer; head shot opens below a full meter; guns fire on timers, not on the clips' fire events; respawn
drops the loadout and does not roll the world back to the checkpoint; push blocks are kinematic at an inferred speed;
plates accept one block where the original needs two; several gates open by themselves.

Architecture and quality: no inverse kinematics or contact placement; sub-states keyed on clip-name strings; `World`
is one class across seven files; duplicated push and ballistic code; per-tick allocations in `candidates` /
`stepPhysics`; inferred constants scattered as `constexpr`; `GAMEPLAY.md` "Deviations" and `ROADMAP.md` list as
missing things the code has. CharacterVirtual had never been evaluated as Lara's mover (plan in AUDIT_B); it has been since: result and
decision (the sweep mover stays) in `docs/formats/PHYSICS.md` "Character controller".

Tests: mechanics are well covered on synthetic geometry and at single real spots; almost no traversal chain, fight
or puzzle was played by input in its own chapter. No original-vs-ours table existed (`docs/REFERENCE_COMPARISON.md`).

## Prioritized work

Order: what blocks the campaign or loses player state, then mechanics the walkthrough needs, then fidelity of
numbers, then architecture. Each line names the reusable system, not a level.

1. Self-test harness in the real binary (`--self-test`), then chapter scenarios for all 21 sections.
2. Chimney jumps.
3. Collectibles: jar treasures (179 / 6), relic effect; breakables by contact; class-30 breakables.
4. Melee, thrall finishing, combat movement set; throwing / carrying objects by physics.
5. Manual aim, combat camera, head-shot and focused-fire rules, scripted slow motion, hang shooting.
6. Water rules (scuba, weapons, dodge, swan dive, jellyfish, scripted levels).
7. Thor's gauntlets and player sensors; moving markup and rideable machinery; campaign hazards.
8. Checkpoint restore and loadout on respawn; chapter start reserves; pre-level choices; PDA truths.
9. Controller inputs and timing (saving grab, jump control, speed-up, safety drop, handstand, free hang), beam and
   pole extras, grapple inputs / swing dynamics / cable physics, bike verification against the original.
10. Per-chapter puzzle and encounter fidelity (prologue .. Out of Time), each proven by input in its units.
11. Push blocks and world constants on Jolt; IK / contact placement. (CharacterVirtual evaluation: done,
    `docs/formats/PHYSICS.md` "Character controller".)
12. Reference comparison table with Wine traces for every remaining [I] number.
13. Refactors (state machine as data, `World` split, allocations) and doc corrections.

Status of each item is tracked in the chapter matrix (per requirement) and in `docs/ROADMAP.md`.
