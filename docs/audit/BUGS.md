# Bug and regression database

Central list of known defects, deviations from tru.exe and regressions in OpenUnderworld. Baseline: `main` 9034839
(tag v0.51.0, 2026-11-16, 997 ctest entries, `game_vulkan_rt_smoke` excluded). Audit pass: no engine code was changed.
Reproduction pass (Phase 0.3): `main` a80bdb8, binaries of `build-AuditBaseline` (= v0.51.0 sources), private KWin;
results per entry under **Repro result** and in "Reproduction log" at the end.

Sources harvested: `docs/ROADMAP.md` "Known deviations" column (cited as `ROADMAP L<line>`), `docs/GAMEPLAY_AUDIT.md`,
`docs/audit/AUDIT_A.md` / `AUDIT_B.md` (rows classified incorrect / placeholder / needs refactor),
`docs/GAMEPLAY_DEVELOPMENT_REPORT.md` §7-8, `docs/REFERENCE_COMPARISON.md` "Differences to fix" and "Unmeasured",
`docs/SELFTEST.md` known-gap table and `tests/selftests/known_gaps/` (14 files), `docs/PROGRESSION.md` gate sweep,
`docs/RENDERING.md`, `docs/GAMEPLAY.md` "Deviations and open questions" (L4387), `docs/matrix/NN_*.md`,
`CHANGELOG.md`, markers in `src/`, GitHub issues, the user's run logs and coredumps.

## Legend

- **Status**: `Open` = present at 9034839; `Fixed-verified` = the fix is in HEAD and the cited code / doc / test shows
  it (this audit read the code, it did not re-run visuals or the game unless it says so); `Needs-repro` = reported or
  plausible but no evidence at HEAD either way. Reproduction pass (a80bdb8): `Reproduced` = seen on HEAD with the
  recorded command (still open); `Not reproduced` = tried on HEAD as recorded, not seen (kept for a hand-test with
  the user's settings); `Needs-Windows-ref` = ours measured, the correct behaviour needs a tru.exe capture under Wine.
- **Origin**: `pre-existing` = never implemented / present since the feature landed; `regression` = worked once (a
  green test or a release that had it), broken later; `new-feature defect` = shipped with its feature. Dates from
  `git log -S` where cheap.
- **Evidence labels**: [O] observed (file:line, command output, log), [I] inference. "tru.exe" expectations cite the
  doc that holds the measurement / decode; "unknown" where nothing in the repo establishes the original behaviour.
- Spawn / CLI: `ou-game --game-dir "$OU_GAME_DIR" --level 'pc-w\<unit>.drm' --spawn x,y,z,yaw` or `--chapter N`;
  self-tests: `ou-game --self-test tests/selftests/<name>.json` (docs/SELFTEST.md).

## Source survey results

- **GitHub issues**: `gh issue list -R sergen213/openunderworld --state all --limit 200` returned none (exit 0, empty) [O].
- **`src/` markers**: 0 `TODO` / `FIXME` / `HACK` / `XXX` in `src` and `tools` (grep, excluding `third_party`) [O].
  Uncertainty is tracked with `[U]` / `[I]` comments instead: `[U]` 94 lines (game 38, script 27, fx 12, formats 5,
  cine 3, collision 2, scene 2, app/game 2, render/shaders 1, physics 1, audio 1), `[I]` ~840 lines (game 438,
  game/ai 105, script 61, fx 43, ui 41, app/game 41, render 47, physics 28, audio 19). Themes of the `[U]` lines:
  unbound script natives and their receivers (`src/script/Script.cpp` 1301-2727: UI/HUD natives, level description,
  fade-to-white, NtSplineSequence), camera look target / default force set / moving-platform carry
  (`src/game/World.cpp` 871, 883, 912, 962), query-96 reach for obstacles (`src/game/Player.cpp` 1088, 1159), death
  types 3 / 4 (`World.cpp` 1394, `Combat.cpp` 93), uber joint corner cases with no data (`Objects.cpp` 694, 826, 3055),
  effect fields with no data (`src/fx/Fx.*`). Bugs below cite the relevant lines.
- **User logs** (`~/.local/state/openunderworld/logs`, 68 files, newest 40 scanned, plus all for script signatures) [O]:
  no `error` / `VUID` / validation / assert / fatal lines except MangoHud's own CPU-power error (15) and
  `[vk warn] VK_ERROR_OUT_OF_DEVICE_MEMORY` (3, OU-VK-004). Abnormal endings (last line not `menu action 8`): three end
  right after `cinematic 001_pro_corridor` (`game-20261106-151224`, `-20261109-214853`, `-20261110-232156`) = the
  Anti-Lag crash (OU-VK-003); others end in a pause menu / mid-play with no error line (cannot be attributed: window
  close vs crash; `game-20261116-201042`, tick 381, l1 shark, Needs-repro if the user saw a crash). Script VM stop
  signatures over all logs: `firetriggervolume` / `slowfire…` / `deathfire…` `op 0x65` 535, `anchor fn 1` 164,
  `instanceref fn 10` 64, `instance fn 27` 52, `budget` 22, `instance fn 63` 12, `gamebase fn 27` 8,
  `cinematic fn 10` 3, `unitref fn 1` 2 (OU-SCR-001..004). Level-load `stream scene` 2.0-9.0 s at tick 14 in five
  logs 2026-11-06..10, none since (OU-STR-001).
- **Coredumps** (`coredumpctl list`): 11 `ou-game` SIGSEGVs, all 2026-11-15, all agent dev builds in
  `openunderworld-work/{wt-UwDeep,int-050,int-abyss}` plus `ou_save_tests` in the YachtFix / YachtBounce worktrees;
  none from an AppImage [O]. They belong to integration work that landed as 0.50.0; no open bug is attributed to them.
- **Chapter matrices**: per-section counts and the stale rows are in OU-DOC-001.

## Index

| ID | Subsystem | Title | Status | Origin | Cluster |
|---|---|---|---|---|---|
| OU-WAT-001 | water render | Under-water floor caustics looked unnatural (white sheet / blobs) | Fixed-verified | pre-existing | — |
| OU-WAT-002 | water render | Splash spray hides the dive crater centre | Reproduced | new-feature defect (0.50.0) | C-SPLASH |
| OU-WAT-003 | water render | Whitecaps drawn as soft blobs / plates | Fixed-verified | pre-existing | — |
| OU-WAT-004 | water render | Foam / whitewater thresholds tuned on calm seas only | Not reproduced | pre-existing | C-WATERTUNE |
| OU-WAT-005 | water render | Interactive field rings only 1-3 cm (hard to see) | Fixed-verified | new-feature defect | — |
| OU-WAT-006 | water / gameplay | FFT swell lowered the sea ~70 units below tru.exe's waves; now zero-mean, treading 256.6 / 253.9 vs tru.exe 254.6 / 247.6 | Fixed on int/brief (bf9179e) | new-feature defect (0.50.0) | C-SWELL |
| OU-WAT-007 | water / gameplay | Dry-deck rule applies to any moving solid, not only floating hulls | Open (not reproduced off-hull) | new-feature defect (0.50.0) | C-SWELL |
| OU-WAT-008 | water render | Under-water sun shafts weak | Reproduced (haze, no beams) | pre-existing | C-UWPORT |
| OU-WAT-009 | water render | Marine snow specks too large | Fixed-verified | pre-existing | C-UWPORT |
| OU-WAT-010 | water render | Shaft shadow map looked up along the unrefracted sun | Fixed-verified | pre-existing | C-UWPORT |
| OU-WAT-011 | water render | Water reflections miss off-screen geometry (SSR) | Open (limit) | pre-existing | — |
| OU-WAT-012 | water render / RT | RT reflection of a shadowed cliff overhang reads too dark | Not reproduced | pre-existing | — |
| OU-WAT-013 | water render | Geometry swell small outside the tessellated window (coarse TRU sea meshes) | Open (limit) | pre-existing | C-SWELL |
| OU-REN-001 | render | Dark sky near the chapter 2 cliff | Fixed (sky dome object) | pre-existing | — |
| OU-REN-002 | render | HDR off by default: bright window highlights band | Open | pre-existing | — |
| OU-REN-003 | render / cine | `co_*` cutscene actor faces render green | Open | pre-existing | — |
| OU-REN-004 | render | Jointless uber sections drawn in bind pose | Open | pre-existing | — |
| OU-REN-005 | render | Under-water hair mode not switched; hair has no water response | Open | pre-existing | — |
| OU-REN-006 | render | Batches skipped "no texcoords, no material textures" (bedroom units) | Reproduced (log), needs RE | pre-existing | — |
| OU-REN-007 | render | Blended foliage receives the shadow map under RT; no roughness | Open | pre-existing | — |
| OU-REN-008 | render / FSR | Translated wind vertex animation has no motion vectors (FSR ghosting) | Open | pre-existing | — |
| OU-REN-009 | render / log | False `texture 2 not found in loaded DRMs` for back-/depth-buffer samplers | Fixed on agent/Depen6 (f6e4264) | pre-existing | — |
| OU-VK-001 | Vulkan RT | `game_vulkan_rt_smoke` hangs the GPU (l1_kraken_room) | Open | regression | — |
| OU-VK-002 | Vulkan AO | CORTAO temporal pass is camera-only (risk of halos on moving meshes) | Not reproduced | new-feature defect (0.51.0) | — |
| OU-VK-003 | Vulkan | AMD Anti-Lag crash a few frames into cutscenes / loading screen | Fixed-verified | regression (Anti-Lag support) | — |
| OU-VK-004 | Vulkan / FSR | `VK_ERROR_OUT_OF_DEVICE_MEMORY` then FSR frame generation context failed | Not reproduced | unknown | — |
| OU-VK-005 | Vulkan / FSR | FSR SDK validation messages (FG 3-component write, storage formats) | Open (upstream) | pre-existing | — |
| OU-STR-001 | streaming | 2-9 s `stream scene` stalls at level load | Not reproduced | unknown | — |
| OU-UI-001 | UI | Game font / button-glyph rendering missing | Fixed-verified | pre-existing | — |
| OU-UI-002 | UI | Chapter title / letterbox screens, confirmation dialogs, PDA sub-screens not drawn | Open | pre-existing | — |
| OU-UI-003 | UI / text | Game font has no Cyrillic; movie / voice subtitles English only | Open | pre-existing | — |
| OU-AUD-001 | audio | No Mus! sequencer / FMOD effects | Fixed-verified (Mus!), see OU-AUD-002 | pre-existing | — |
| OU-AUD-002 | audio | Reverb is a Freeverb tank, not FMOD's I3DL2; music bus not fed to reverb | Open | pre-existing | — |
| OU-AUD-003 | audio | One global music volume fade unidentified | Not reproducible (RE item) | unknown | — |
| OU-AUD-004 | audio | Script sound wait threads, uber mover / physics-lever sounds missing | Open | pre-existing | C-VM |
| OU-AUD-005 | audio | Music loads synchronously (tracks start a few frames early) | Open | pre-existing | — |
| OU-CAM-001 | camera | Moving platforms do not carry the camera focus | Fixed on int/brief (lag/band); height residual fixed with OU-CAM-005 (agent/DeckCam6) | pre-existing | — |
| OU-CAM-002 | camera | Swim camera follows the wave bob exactly | Not reproduced | new-feature defect [I] | C-SWELL |
| OU-CAM-003 | camera | Motorcycle camera distance / swing are placeholders | Open | pre-existing | — |
| OU-CAM-004 | camera | Reveal camera blend in/out ours; gamebase time scales not applied | Open | pre-existing | — |
| OU-PLY-001 | Lara / beams | Beam-walk entry into "about to fall" undecoded | Open | pre-existing | C-CHART |
| OU-PLY-002 | Lara / wall hug | Wall-hug corners reconstructed, not decoded | Open | pre-existing | C-CHART |
| OU-PLY-003 | Lara / water | No climb out of water onto a wall-hug shelf | Open | pre-existing | C-CHART |
| OU-PLY-004 | Lara / ladders | No ladder side jumps or reaches | Open | pre-existing | C-CHART |
| OU-PLY-005 | Lara / jumps | Ledge-aim reach: running-jump rush/arc switch 20-70 farther out | Open | pre-existing | C-MEASURE |
| OU-PLY-006 | Lara / obstacles | Low-obstacle detection distance (query 96 reach) unknown | Open | pre-existing | C-CHART |
| OU-PLY-007 | Lara / poles | Pole fling onto a platform lands 79 past the edge (tru.exe 3 short) | Open | pre-existing | C-MEASURE |
| OU-PLY-008 | Lara / ledges | Running safety drop lacks the 82-unit overshoot and take-back | Open | pre-existing | C-MEASURE |
| OU-PLY-009 | Lara / chimney | Chimney wall reach / standoff pair differs (holds 110 vs 119-131) | Open | pre-existing | C-MEASURE |
| OU-PLY-010 | Lara | Free hang (`ledgeu_*`), beam drops / sprint, pole-to-perch transitions not ported | Open | pre-existing | C-CHART |
| OU-PLY-011 | Lara | One-handed saving grab recovered by Jump instead of Interact | Fixed-verified | pre-existing | — |
| OU-PLY-012 | collision | Push-out every ground tick; one-sided ground sweep + front-only push-out (Wine: tru.exe lets her through a wall from behind); switch stand point from the nearest collision box face (Wine 4525) | Fixed on agent/TickDepen7 | pre-existing | — |
| OU-PLY-013 | Lara | Slope-slide upper bound (V002) — not a bug: 0x5DCE20 reads the 28° limit via the query record from 0x71B910 (fc669b5) | Not a bug | — | — |
| OU-PLY-014 | Lara | Ledge wall-side limit loosened to 0.5236 on a moving ledge (V042) | Fixed on int/brief (21678c2; full ctest pending) | pre-existing | — |
| OU-PLY-015 | Lara | On the dropping yacht deck Lara enters State::Land once; tru.exe stays in one state | Fixed on agent/DeckCam6 (colliders posed after a mid-game rebuild) | pre-existing | — |
| OU-SWM-001 | swimming | Swim turn: symmetric and view-relative now (d122a62); steady circle still +2.3 / +1.8 deg/s fast (tru.exe settles 42.1° off its camera line, ours 44.2°) | Partly fixed on int/brief | pre-existing | C-MEASURE |
| OU-SWM-002 | swimming | Surface swim velocity lag: now 5.25 / 5.37° vs tru.exe 5.4 / 5.6° (0x755A00 blend 0.25 = player data +0x240) | Fixed on int/brief (d122a62) | pre-existing | C-MEASURE |
| OU-SWM-003 | swimming | Scuba slot not read live; grenades under water unchanged | Open | pre-existing | — |
| OU-GRP-001 | grapple | Throw uses a stand-in clip; hook attaches at once | Open | pre-existing | — |
| OU-GRP-002 | grapple | Swing dynamics unmeasured (five failed Wine sessions) | Open | pre-existing | C-MEASURE |
| OU-GRP-003 | grapple | Jump into the gap released the rope; leap off a wall ignored the stick | Fixed-verified | pre-existing | — |
| OU-GRP-004 | grapple | ch10 roped leap to wall ring 2595 swings under the pool surface | Open | pre-existing | — |
| OU-GRP-005 | grapple | ch12 lever 6695: rope attach calls vt 46, larabase 26 bound, she stays roped while Interact is held | Fixed on int/brief (f832536) | pre-existing | C-VM |
| OU-PZL-001 | progression | ch01: no input route into the pocket north of the bedroom beams | Open | pre-existing | — |
| OU-PZL-002 | progression | ch04: jump to perch 30 lands on pillar 29 | Open | pre-existing | C-MEASURE |
| OU-PZL-003 | progression | ch05: west recess from the T junction is shut | Open | pre-existing | — |
| OU-PZL-004 | progression | ch06: hallway roll stops on a 45° floor before plane 1203 | Open | pre-existing | — |
| OU-PZL-005 | progression | ch06: bar 238 not reached from the top valve wheel | Open | pre-existing | C-CHART |
| OU-PZL-006 | progression | Freed slab bodies stay in place (ch09 shrine, ch08 statue floor) | Fixed on int/brief (ecf03db); ch09 scenario still parked (2 grapple issues) | pre-existing | C-JOINT |
| OU-PZL-007 | progression | ch13: calendar push handle turns nothing after the mask is set | Fixed on int/brief (test bug, 2e6ef11) | pre-existing | C-JOINT |
| OU-PZL-008 | progression | ch13 temple: (c) multibody 92/93 bound (1d3a9c1); (a) ring 4951 sight ray ended 4 under the post's floor (fixed: ends at the 0xDE feature); (b) rope bends don't tug bodies remains (tru.exe's bends are physics bodies, 0x699A50) | Partly fixed (a on agent/Joint8) | pre-existing | — |
| OU-PZL-009 | progression | Switch `modifiable` 7 now honoured for every switch (ch15 exit switch locked until the key) | Fixed on int/brief (30cc49f) | pre-existing | C-VM |
| OU-PZL-010 | progression | ch15: rune block 5609 has no joint record | Fixed on int/brief (b716e88); pull speed unverified | pre-existing | C-JOINT |
| OU-PZL-011 | progression | ch17: hammer chamber lower steps absent from collision | Open | pre-existing | — |
| OU-PZL-012 | progression | ch21: Natla platform jump falls short; fight sequence unplayable by input | Open | pre-existing | C-VM |
| OU-PZL-013 | progression | Motorcycle absent at level entries without a placing script | Open | pre-existing | C-VM |
| OU-PZL-014 | progression | Freed prologue ring-door leaf stays standing in its doorway | Fixed-verified (6d6c9eb; ring rides the leaf on agent/Joint8; landing place chaotic: ours 1.1k flat, tru.exe 656 flat) | pre-existing | C-JOINT |
| OU-PZL-015 | progression | `multibody` 3/4 bound and saved (caller is l2_maproom planes 2637/2638, not ch08); ch14 throne native still unknown | Partly fixed on int/brief (171100e) | pre-existing | C-VM |
| OU-SCR-001 | script VM | Fire-volume handlers stop at opcode 0x65 (CALLU without a function pointer) | Fixed-verified (8daa2ee) | pre-existing | C-VM |
| OU-SCR-002 | script VM | Native `anchor` 1 (vector values) unbound | Fixed on agent/ScriptVec7 (vector values, anchor / vector / rotation / larabase 21-22 bound); fire overlay played on agent/Flinch8 (upper-body chart S90, anim section 1) | pre-existing | C-VM |
| OU-SCR-003 | script VM | Natives `instanceref` 10, `instance` 27 / 63, `gamebase` 27, `cinematic` 10, `unitref` 1 unbound | Partly fixed (f9676fd; `instance` 63 open, record narrowed) | pre-existing | C-VM |
| OU-SCR-004 | script VM | Fire-volume damage loop spins to the VM budget when its sound wait does not block | Fixed-verified (agent/ScriptVm6) | pre-existing | C-VM |
| OU-SCR-005 | script VM | Level-change fade to white unsupported (always black) | Open | pre-existing | C-VM |
| OU-CMB-001 | combat | Head shot opened below a full adrenaline meter | Fixed-verified | pre-existing | — |
| OU-CMB-002 | AI | No enemy jump-over (NavPower links), flyers patrol straight, poacher rifle undrawn | Open | pre-existing | — |
| OU-VEH-001 | motorcycle | Brake strength, slopes, coasting, top speed, pit jump unmeasured | Open | pre-existing | C-MEASURE |
| OU-PHY-001 | physics | Body-A motion and fixed riders on a freed body A not simulated (fixed joints done, ecf03db) | Fixed on agent/Joint8 (poses follow body A; fixed riders follow a freed body A [I: mass not merged]) | pre-existing | C-JOINT |
| OU-PHY-002 | physics | Push-block / Thor-mechanism speed fitted to one block trace | Open | pre-existing | C-MEASURE |
| OU-TST-001 | tests | No walkthrough section plays end to end; 14 parked known-gap scenarios | Open | pre-existing | — |
| OU-TST-002 | tests | The self-test autopilot does not fight (blocks combat legs ch11 / ch18) | Open | pre-existing | — |
| OU-DOC-001 | docs | Chapter matrices mark implemented mechanics as NOT IMPLEMENTED | Fixed-verified | doc drift | C-DOC |
| OU-DOC-002 | docs | Contradictions between ROADMAP rows / RENDERING / REFERENCE_COMPARISON / GAMEPLAY | Open (mostly fixed) | doc drift | C-DOC |
| OU-SAV-001 | save/load | Quickload in l3_main_bedroom does not resume doors 4526 / 4376 exactly | Reproduced | pre-existing | — |
| OU-TST-003 | tests | Loaders lack synthetic rejection tests (corrupt / truncated input) in most formats | Open | pre-existing | — |
| OU-CAM-005 | camera | Yacht deck camera distance 1264.2 vs tru.exe 948.2 | Fixed on agent/DeckCam6 (camera trigger volumes, Wine-verified at two spots) | pre-existing | — |
| OU-CAM-006 | camera | Camera trigger commands beyond distance / pitch ignored; yacht volumes stay at their intro place | Fixed on agent/CamZones7 (0x12 / 0x15 / 0x17-0x1C with forces, riding volumes; Wine-verified at 3852 / 3859 / 3861) and agent/CamRest8 (1, 3-6 overrides, 7 / 8 limits; Wine-verified at 4468 / 4349); 9 / 0xC / 0x14 / 0x16 and kind 1 open | pre-existing | — |
| OU-CMB-003 | AI | l2_cliffclimb sharks attack within 5 s of entering the water (tru.exe: no bite while treading; only a Lara 497+ under the top is in reach) | Fixed on agent/SharkAi6 (76cc040); presence, CloseCombat slots, bite range on agent/EnemySpawn7 | pre-existing | — |

## Root-cause clusters

| Cluster | Shared cause | Bugs |
|---|---|---|
| C-SWELL | 0.50.0 made the drawn FFT swell part of the gameplay water surface but kept the render's −0.5 σ centring offset (`src/render/Ocean.cpp:202`, measured −68.6 of the −70 mean shift) and `min(…, 0)` crest clamp (`src/render/shaders/water_surface.glsl:87-91`, `src/game/World.cpp:1710-1718`); decks that now sit under the volume top got a blanket dry-deck rule instead of a hull model; the camera focus follows Lara's bone 108 on that surface | OU-WAT-006, OU-WAT-007, OU-WAT-013, OU-CAM-002 |
| C-UWPORT | 0.49.0 replaced the under-water look with a literal port of Noniv's abyssal-garden (shafts, snow, caustics, grade); look complaints from before the port must be re-judged against it | OU-WAT-008, OU-WAT-009, OU-WAT-010 (and OU-WAT-001 via 0.49.1) |
| C-SPLASH / C-WATERTUNE | Enhanced water thresholds and opacities are ours [I] (`water.frag` 415-432, fluid composite RENDERING L744-746) and were tuned on l1_seatop / l2_cliffclimb calm-sea views | OU-WAT-002, OU-WAT-004 |
| C-VM | SCMD VM gaps: unimplemented opcode 0x65, natives without bindings, no vector values, partial `modifiable` semantics, no script UI; a stopped handler silently leaves its object idle | OU-SCR-001..005, OU-PZL-009, OU-PZL-012, OU-PZL-013, OU-PZL-015, OU-AUD-004 |
| C-JOINT | Objects whose motion the data defines outside our joint model: missing joint records, fixed joints / body A motion, freed-section bodies that never get released, joints held by rope tension | OU-PZL-006, OU-PZL-007, OU-PZL-010, OU-PZL-014, OU-GRP-005, OU-PHY-001 |
| C-CHART | Lara's player state chart is hand-ported per move (AUDIT_A L331 / L433: sub-states keyed on clip names, no transition table); every undecoded condition becomes a guessed rule or a missing move | OU-PLY-001..004, OU-PLY-006, OU-PLY-010, OU-PZL-005 |
| C-MEASURE | Numbers decoded or fitted but not measured in tru.exe under Wine (`docs/REFERENCE_COMPARISON.md`: 107 of 161 rows unmeasured, "Unmeasured" table lists the session for each) | OU-PLY-005, OU-PLY-007..009, OU-SWM-001, OU-SWM-002, OU-GRP-002, OU-PZL-002, OU-VEH-001, OU-PHY-002 |
| C-DOC | Status columns not refreshed when later work landed | OU-DOC-001, OU-DOC-002 |

## Entries

Format per entry: **Repro** / **Expected** (tru.exe) / **Actual** / **Files** / **Cause** (suspected; confirmed where
established) / **Fix** (suggested) / **Test** / **Status · Origin**.

### Water rendering and the water surface (OU-WAT)

**OU-WAT-001 Under-water floor caustics looked unnatural**
- Repro: `--chapter 1`, dive to the l1 sea bed; or any sunlit pool floor, Enhanced water.
- Expected: tru.exe draws no caustics at all [O: RENDERING.md L554, "no caustic pass or texture"]; ours are an
  Enhanced addition, judged against real caustics.
- Actual (before 0.49.1): abyssal-garden texture caustics `pow(a, 2.3) * 5` clamped at 4 left a white plateau with
  dark blotches (RENDERING.md L572-575, CHANGELOG 0.49.1).
- Files: `src/render/shaders/uw_caustics.vert/.frag`, `uw_medium.glsl` (`agCausLod`), `render::kAgCausVertices`.
- Cause (confirmed, RENDERING L572-575): the demo's shaping curve. Fix landed: Wallace caustic mesh splatted into
  `uAgWater.g` (256² grid, 6.6 m tile, mean ≈ 1) (RENDERING L554-571).
- Test: none automated for the look; user hand-test pending.
- Status · Origin: Fixed-verified (code + doc at HEAD; visual acceptance by the user outstanding) · pre-existing.

**OU-WAT-002 Splash spray hides the dive crater centre**
- Repro: `--chapter 1`, swan dive / fall from the yacht into the sea, Enhanced water, watch the entry point.
- Expected: unknown in tru.exe (its splash is its own effect, kept on top: CHANGELOG 0.50.0); goal: the crater and
  rebound jet visible as in a real entry.
- Actual: "The splash spray still covers the very centre" [O: CHANGELOG 0.50.1, L33].
- Files: `src/render/WaterFluid*` / `shaders/fluid.comp` (PBF), the screen-space fluid composite (RENDERING L704-757),
  the game's own splash particles (`src/fx`).
- Cause (suspected): spray particles (neighbour count < 0.32, RENDERING L744) composited with a haze at the crater
  centre plus the original splash's white water drawn on top.
- Fix: fade spray haze inside the crater radius for the first ~0.3 s, or draw the original splash's white water
  only around, not over, the fluid crown.
- Test: proposed `WaterFluid.CraterCentreVisible` (capture at entry + 0.2 s, centre luminance vs ring).
- Repro result (a80bdb8): **Reproduced.** Swan dive from the `swan_dive.json` cliff top, Vulkan default (Enhanced,
  FFT): `ou-game --level 'pc-w\l2_cliffclimb.drm' --spawn -29056,2361,6100,180 --input-script swan.txt --frames N
  --screenshot …` (`swan.txt`: `0.6 forward 1`, `1.25 jump 1`, `1.3 jump 0`, `1.35 drop 1`, `1.45 drop 0`,
  `1.5 forward 0`; 60 Hz fixed). Entry (`swimsplash`, z 105.5) at tick 187, as the self-test trace. At tick 199
  (entry + 0.2 s) the crater centre is a flat white patch: mean luma 236 over 80² px at the centre vs 110 in the ring
  and 108 on open water (`BugRepro/shots/swan_199.png`). Additional observation at tick 211 (+0.4 s): the follow
  camera passing down through the splash fills the frame with large translucent discs (spray / bubble particles drawn
  at screen size near the eye, `swan_211.png`) [O]; not in this entry's original report.
- Status · Origin: Reproduced · new-feature defect (0.50.0 PBF splash).

**OU-WAT-003 Whitecaps drawn as soft blobs / plates**
- Repro: `--chapter 1` surface swim, FFT ocean on.
- Actual (0.40.0): `smoothstep(0.2, 0.7, …)` drew solid soft plates on 9.6 % of the sea (RENDERING L520-523).
- Fix landed: Jacobian fold density + fine fold pattern with a pixel-wide edge (`water.frag` L235-239, L427-432);
  measured mask > 0.2 on 1.8 % of the sea, 0.4 % after 15 s (RENDERING L522-523); white, not light-tinted (0.50.0).
- Status · Origin: Fixed-verified (measured in RENDERING) · pre-existing.

**OU-WAT-004 Foam / whitewater tuned only for calm seas**
- Repro: rough-sea or wind-heavy unit (l7_arctic_sea), Enhanced + FFT, compare foam coverage with l1_seatop.
- Expected: unknown (ours [I]).
- Files: `src/render/shaders/water.frag` L415-432 (edge foam, `ouFieldFoam` churn, whitecap density),
  `water_field.comp` L231 (breaking-crest foam).
- Cause (suspected): fixed thresholds (`kEdge`, `smoothstep(-0.25, 0.15, …)`, the 55 % aged foam) independent of the
  sea state / wind of the level.
- Fix: scale thresholds with the FFT spectrum's significant wave height; verify on two sea states.
- Test: proposed coverage metric per unit in the water debug view (`--water-debug`).
- Repro result (a80bdb8): **Not reproduced as described.** There is no rough sea state to test: the FFT spectrum is
  one global parameter set, logged identically on every sea (`FFT ocean: 128^2 grid, 3 cascades (240 / 28 / 11.3 m),
  choppiness 2.24, height RMS 0.47 / 0.026 m` on l1_seatop and l2_cliffclimb); only tru.exe's own wave heights
  differ per level. l7_arctic_sea's default spawn is under the ice (no surface view). Foam pixel share (min channel
  > 190, chroma < 40) after 300 / 600 fixed ticks: l1_seatop water 0.001 %, l2_cliffclimb water 0.11 % (Original water
  0.02 %) (`BugRepro/shots/seatop.png`, `cliff_enh_fft.png`). Re-test only if a per-level sea state is added.
- Status · Origin: Not reproduced · pre-existing.

**OU-WAT-005 Interactive field rings only 1-3 cm**
- Actual (0.50.0): rings 2-4 cm, no rebound, swimmer acted as a wall (CHANGELOG 0.50.1).
- Fix landed (0.50.1): crater ~0.5 m, rebound ~20 cm at 1/3 s, rings up to 10 cm; field clipping removed
  (RENDERING L656). Status · Origin: Fixed-verified (changelog + RENDERING) · new-feature defect.

**OU-WAT-006 FFT swell lowers the sea below tru.exe's waves**
- Repro: `--chapter 1` or `--chapter 2` with Enhanced water + FFT ocean; compare drawn sea level with the level's
  water volume top / tru.exe wave height.
- Expected: tru.exe's sea is its own sine waves only, `Σ h(sin(k·x + φ) − 1)` (0x69DA80, GAMEPLAY / World.cpp
  `waveHeight`) — never above rest, mean below it. Whether a crest bound applies to an added swell is unknown (the
  FFT swell is ours).
- Actual: `ouBaseHeight` returns `min(h + ouOceanHeight(p) − 0.5·kM·w.ocean[0].w, 0.0)` and the gameplay surface
  `std::min(h + gSeaSwell(…), 0)`, where `render::ocean::Swell::height` already includes the same centring
  `offset_ = −0.5 · kMetre · σ` (`src/render/Ocean.cpp:202`).
- Repro result (a80bdb8): **Reproduced, measured.** Scratch program `BugRepro/tex2/swell.cpp` linked against the
  built libraries evaluates the exact gameplay functions (`game::waterSurface` / `waveHeight`,
  `render::ocean::swell(128)`) on the l2_cliffclimb sea (volume top 108) at the yacht (−34600, −14670), every 1/60 s
  for 600 s, and on a 9 × 9 grid (2000-unit spacing, every 0.5 s):

  | sample | tru.exe waves: mean / min / max | + swell (as played): mean / min / max | mean shift | swell's own mean | crest clip active |
  |---|---|---|---|---|---|
  | yacht point | −142.0 / −349.9 / 41.8 | −211.9 / −749.2 / 108.0 | **−69.9** | −68.6 | 2.58 % of samples |
  | 9 × 9 grid | −142.1 / −391.7 / 103.4 | −212.8 / −833.0 / 108.0 | **−70.7** | −68.8 | 2.98 % |
  | l1_seatop grid (top 1672.7) | 1451.4 / 1231.5 / 1669.6 | 1380.1 / 742.6 / 1672.7 | −71.3 | −68.8 | 3.83 % |

  The swell's RMS is 137.2 (modes 256, residual 9.2). The waves-only mean −142.0 matches the tru.exe Wine
  measurement (GAMEPLAY.md L1676: "53 and −363, mean −142") [O]. So the played sea sits ~70 units below tru.exe's,
  and ~98 % of that is the centring offset −0.5 σ = −68.6, not the crest clip (clip adds ~1.3 units; it is active
  on only 2.6-3.8 % of samples). The earlier "~35 units" note was wrong.
- Lara rest height gap (baseline z 108 Enhanced vs 250 Original): the player intro stands on the floating yacht, which
  heaves continuously in both sea models; the baseline runs were wall-clock paced (249 vs 237 ticks). With fixed
  60 Hz ticks (`--input-script idle.txt`, `0 forward 0`, `--frames 600`) Lara ends at z **−103.4** (Enhanced + FFT,
  repeated: identical sim digest `aa658bbc448797bb`), **24.8** (Enhanced + `--ocean procedural`) and **24.8**
  (Original; same digest `ba37a30333bf7b16` as procedural: the water quality itself does not touch the sim). Over a
  20 s self-test series from the chapter-2 start (80 × 0.25 s samples) her root on the deck spans −151.4 .. 420.2
  with the swell and −25.5 .. 382.3 on tru.exe's waves (with `fall` / `land` states as the deck drops away). The gap
  is a heave phase difference, not a rest-height offset; the lowered mean above is the real offset.
- Files: `src/render/shaders/water_surface.glsl:83-91`, `src/render/shaders/water.vert:51`, `src/game/World.cpp:1710-1718`,
  `src/render/Ocean.cpp:202` (`render::ocean::Swell`).
- Cause (confirmed by measurement + code): the "centring" −0.5 σ shifts the whole swell down so that, after the clip,
  decks stay above the drawn sea; with the swell in the gameplay surface (c033ab5, 2026-11-15) buoyancy, swimming and
  the under-water test all see the lowered sea. The clip is a minor contributor.
- Fix: centre the swell on zero (drop the −0.5 σ offset) and decide the crest bound separately; keep decks dry by the
  hull model (OU-WAT-007). Needs a decision with the yacht buoyancy tests.
- Test: `game_selftest_*` water scenarios with `"sea_swell": true`; proposed `World.SwellMeanAtRestPlane` (mean of
  `surfaceHeight` with swell − without, over a grid, within ±5).
- Status · Origin: Reproduced · new-feature defect (render clamp 0.4x; gameplay coupling 0.50.0, c033ab5).

**OU-WAT-007 Dry-deck rule applies to any moving solid**
- Repro: stand on any collision dynamic (moving uber section, platform, loose crate) inside a water volume below its
  top; water entry is suppressed while her feet are above the wave surface.
- Expected: tru.exe decks never sit under the volume top [I: Player.cpp:3096 comment], so it has no such rule.
- Actual: `Player.cpp:3100-3101` accepts any ground probe hit with `deck->dynamic >= 0` and a standable normal.
- Files: `src/game/Player.cpp:3093-3101`.
- Cause (confirmed by code): rule keyed on "dynamic collision", not on "floating hull" (introduced 25c5703,
  2026-11-15).
- Fix: restrict to loose bodies with buoyancy (the yacht / motorboat objects) or to bodies whose top is above the
  undisturbed rest plane; better, remove once OU-WAT-006 stops lowering the sea.
- Test: proposed `Game.DryDeckOnlyOnFloatingHulls` (a submerged moving platform still wets Lara).
- Repro result (a80bdb8): **Not reproduced off a hull; scope narrowed by code.** `Player.cpp:3092`
  (`if (s_.pos.z > (airborne ? vol.points[1].z : surf)) return false;`) returns before the rule for a grounded Lara
  above the surface, so the rule (`:3099-3101`) only changes anything while she is **airborne** with her root between
  the wave surface and the volume's static top and a dynamic standable face within the step range: landing on a moving
  solid in that band. No non-hull moving solid inside a water volume was found to exercise; the hull case works as
  designed (`ch07_cliffclimb_boat_swell`). A repro needs a dynamic platform whose top lies in that band (candidates:
  flooding-ship rooms, water-level puzzles).
- Status · Origin: Open (not reproduced off-hull) · new-feature defect (0.50.0).

**OU-WAT-008 Under-water sun shafts weak**
- Repro: under water, sun above the horizon, Enhanced water, `light_shafts` on (l1 sea, l2_cliffclimb).
- Expected: Enhanced addition (tru.exe has its own light rays drawable, 0x6D8C20, RENDERING L554).
- Actual: reported weak before 0.49.0; since 0.49.0 the shafts are abyssal-garden's half-res march (RENDERING L897-915,
  `min(acc·…·0.54, 20)`, mist term 0 because no density volume exists here).
- Cause (suspected): the demo's mist term `1 + 1.2 mist²` is dropped (mist = 0), so shafts get the demo's open-water
  minimum only.
- Fix: decide after a hand-test at HEAD; if still weak, feed a mist estimate (depth / near rock) instead of 0.
- Test: none.
- Repro result (a80bdb8): **Reproduced as a look (haze, no beams); judgement left to the user.** l7_arctic_sea default
  spawn (Lara under the sunlit ice), Vulkan default, `--input-script idle.txt --frames 300`, `--shafts on` vs
  `--shafts off`: mean luma 164.7 vs 148.3 (+16.4, max +38; 93 % of pixels ≥ +5, 61 % ≥ +15). The difference image is
  a smooth veil, weakest over the bright caustic patches; no beam structure is visible
  (`BugRepro/shots/uw_shafts_on.png`, `uw_shafts_off.png`). Consistent with the suspected cause (mist term 0 → the
  demo's open-water minimum only) [I].
- Status · Origin: Reproduced (haze, no beams) · pre-existing.

**OU-WAT-009 Marine snow too large** — 0.49.0 replaced the bright cyan blobs with 9000 specks of 0.012-0.057 m
(`uw_snow.vert`, RENDERING L978-980). Status · Origin: Fixed-verified (doc + code) · pre-existing.

**OU-WAT-010 Shaft shadow map used the unrefracted sun** — the march now looks the air shadow map up at
`agShadowPoint`, the air-line point through the refracted ray's surface crossing, so hull shadows follow the shafts
(RENDERING L911-915). Status · Origin: Fixed-verified (doc + code) · pre-existing.

**OU-WAT-011 Reflections miss off-screen geometry (SSR)**
- Actual: ROADMAP L66 / L67 "SSR misses off-screen geometry". With RT the water uses `water_rt.frag` (mirror ray,
  RENDERING L203), so the limit applies to non-RT devices / `--rt off`.
- Fix: none planned (planar fallback would cost a pass). Status · Origin: Open (design limit) · pre-existing.

**OU-WAT-012 RT reflection of a shadowed cliff overhang reads dark**
- Repro: `--chapter 2` (l2_cliffclimb) with `--rt full`, look at the cliff's reflection in the sea.
- Expected: unknown in tru.exe (its water reflects by planar/screen copies); physically the overhang's underside gets
  sky / bounce light.
- Files: `src/render/vk/shaders/water_rt.frag` (= `water.frag -DOU_RT`), RT hit shading (RENDERING "Ray tracing").
- Cause (suspected): hit shading uses sun × shadow plus a small ambient; no GI / sky term at the hit, so a sun-shadowed
  surface goes near black in the reflection.
- Fix: add the irradiance/ambient (c15 / c70-c76 equivalent) at RT hits.
- Repro result (a80bdb8): **Not reproduced as an RT-specific darkening.** l2_cliffclimb player intro, Vulkan, fixed
  60 Hz (`--input-script idle.txt --frames 300/600`), `--rt full` vs `--rt shadows` (SSR water) vs `--rt off`, four
  views: Lara's default camera, `--camera -35070,-15040,1500,-31590,-12040,0`, `-33000,-13300,800,-31000,-11300,-100`
  and the grazing `-32600,-12900,250,-30600,-10900,300`. Water under the pillar overhang: luma 66.9 / 67.2 / 66.6 and
  89.8 / 89.9 / 89.6 (full / shadows / off); grazing view water strip RGB 54/111/104 (full) vs 56/112/107 (SSR), dark
  streaks (luma < 80) 17.8 % vs 17.1 %, their mean 53/71/66 vs 49/70/65: RT is not darker than SSR. RT does show the
  cliff's colour where SSR shows grey (`BugRepro/shots/refl_c_*.png`). Dark reflection streaks exist in every mode;
  they plausibly mirror the dark grey sky of OU-REN-001 [I]. Needs the user's camera / screenshot.
- Status · Origin: Not reproduced · pre-existing.

**OU-WAT-013 Geometry swell small outside the tessellated window** — TRU sea meshes are ~3.5 m quads (RENDERING L542-546);
only the wave-field window is tessellated (`water.tesc/.tese`), elsewhere relief is normals only (ROADMAP L67).
Status · Origin: Open (limit) · pre-existing.

### Rendering (OU-REN)

**OU-REN-001 Dark sky near the chapter 2 cliff**
- Repro: `--chapter 2` (l2_cliffclimb start), Vulkan Enhanced, look up / out along the cliff.
- Expected: original capture `screens/chapters/orig_ch2` (work dir, RENDERING L1275 sweep) [not re-viewed here].
- Files: skydome = the unit's exterior terrain drawn behind everything (`render::followExterior`, ROADMAP L29,
  RENDERING L362 sky band comparison), unit filter / fog (RENDERING L312).
- Cause: unknown; candidates: exterior terrain drawn with the wrong unit's sky in a neighbour join, or AgX / fog
  darkening the sky band.
- Fix: same-camera A/B against the original capture (RENDERING L362 method).
- Repro result (a80bdb8): **Reproduced; needs a Windows reference.** `ou-game --level 'pc-w\l2_cliffclimb.drm'
  --input-script idle.txt --frames 120 --camera -34700,-14800,600,-33000,-13000,3000` (looking up NE from the boat):
  sky top-right region mean RGB **44 / 48 / 47** (Vulkan and OpenGL identical), **21 / 24 / 30** with `--preset
  original` (`BugRepro/shots/sky_*.png`). The unit has **no exterior terrain** (0 exterior submeshes in
  `loadLevelScene`; l2_junglehub 5, l1_seatop 12, l7_arctic_sea 1; scratch `BugRepro/tex2/ext.cpp`), so the sky is
  whatever the clear / fog leaves. `screens/chapters/orig_ch2.png` shows no sky. Wine capture needed: tru.exe at the
  l2_cliffclimb start with the camera pitched up to the NE (camarg method, RENDERING L354), plus its clear colour /
  any sky draw (D3D9 Clear and the first draws of the frame).
- Cause (2026-11-19): the unit's sky dome object (Level+0x70 DTP +0x2B4 sky record → `sa_skydome_c`, offset -500;
  tru.exe 0x6D4480 / 0x6D4560) was never placed; LEVELS.md "Sky dome". Fixed: ou-game draws the one dome behind
  everything at the camera + offset (sky, clouds and the horizon's island cards; GL and Vulkan).
- Status · Origin: Fixed · pre-existing.

**OU-REN-002 HDR off by default: highlights band**
- Repro: default Enhanced, any bright window / sun-lit opening (l3_main_bedroom windows).
- Expected: tru.exe writes LDR gamma-space colour (RENDERING L40, L56-60) — banding may exist in the original too [I].
- Actual: `hdr = false` default (`src/config/Config.hpp:72`), scene target UNORM8.
- Fix: dither the post output (no fidelity cost) or default `hdr` on with the Enhanced preset.
- Test: `Config.GraphicsPresets` (would change). Status · Origin: Open · pre-existing.

**OU-REN-003 `co_*` cutscene actor faces render green** — ROADMAP L45 "co_* faces render green (materials)".
Files: `src/cine`, material decode (`docs/formats/MATERIALS.md`). Fix: decode the face materials' blend / texture
stages. Test: proposed `CineReal.ActorFacesTextured` screenshot check (`ou-game --cine 060_med_natla`).
Status · Origin: Open · pre-existing.

**OU-REN-004 Jointless uber sections drawn in bind pose** — their record +0x30 animations not run (ROADMAP L29).
Status · Origin: Open · pre-existing.

**OU-REN-005 Under-water hair mode not switched** — ROADMAP L31. Files: `src/anim` Verlet hair. Status · Origin:
Open · pre-existing.

**OU-REN-006 Batches skipped "no texcoords, no material textures"**
- Repro: any start in l3_main_bedroom; log lines `pc-w\l3_main_bedroom.drm#section 12025 batch 0: skipped …` (48 in
  the last 12 logs, also l3_chamberrockhall 36, l3_bedroom_hall 24) [O].
- Expected: unknown whether tru.exe draws them (likely untextured helper / shadow geometry [I]).
- Fix: check the sections' material blobs against tru.exe's draw rule (0x511810 colour pass) before dropping them.
- Repro result (a80bdb8): **Reproduced (log).** `--level 'pc-w\l3_main_bedroom.drm'` logs 9: l3_main_bedroom sections
  12025 / 12027 / 12030 / 12032, l3_bedroom_hall 11890 / 11892, l3_chamberrockhall 12245 / 12247 / 12249 (batch 0
  each). The skip is deliberate (`src/formats/Model.cpp:303-313`: no TEXCOORD1 and no material texture = "level-enclosing
  boxes with depth/shadow/fog-only shaders (material 0xd78)", hypothesis). No visible defect attributed; whether
  tru.exe draws them (fog / occlusion volumes) needs RE of the material's passes.
- Status · Origin: Reproduced (log), needs RE · pre-existing.

**OU-REN-007 RT: blended foliage still receives the shadow map; no roughness** — ROADMAP L39. Status · Origin: Open ·
pre-existing.

**OU-REN-008 Wind vertex animation has no motion vectors** — FSR ghosting on foliage (RENDERING L1221). Status · Origin:
Open · pre-existing.

**OU-REN-009 False `texture 2 not found in loaded DRMs`**
- Repro: any l2_cliffclimb load (`--level 'pc-w\l2_cliffclimb.drm'`): the warning appears twice (scene + neighbour
  rebuild) [O, every run of this pass].
- Repro result (a80bdb8): **Reproduced; cause confirmed.** Scratch `BugRepro/tex2/tex2.cpp` (`loadLevelScene`, then
  scan every reference): the only references to id 2 are material samplers with `source 1` (engine-global table) —
  l2_cliffclimb material 8035 (reg 8, cls 9 = back buffer), material 10930 (reg 7 cls 10 = depth buffer, reg 3 cls 9);
  l2_flushtunnel material 9544 (reg 1 cls 10, reg 0 cls 9); l2_cliffhall none. Per `scene::MaterialSampler`
  (`src/scene/Assets.hpp:90-96`) source 1 binds the global slot and `textureId` is only a fallback, so the missing
  section is never needed; `loadTextures` (`src/formats/Scene.cpp:119-131`) still asks for it.
- Fix (f6e4264, agent/Depen6): `scene::readsRenderTarget` (source 1, cls 9 / 10); `loadTextures` no longer collects
  those samplers' `textureId`. Every other missing id still warns.
- Tests: `Formats.RenderTargetSamplersNeedNoTexture`, `RealData.NoMissingTextureWarningsForRenderTargets`
  (l2_cliffclimb, l2_flushtunnel). ou-game `--level` l3_main_bedroom / l2_cliffclimb and `--chapter 1`: 0 such lines
  (the BugRepro logs had 88, all id 2).
- Status · Origin: Fixed on agent/Depen6 · pre-existing (cosmetic).

### Vulkan, FSR, streaming (OU-VK, OU-STR)

**OU-VK-001 `game_vulkan_rt_smoke` hangs the GPU**
- Repro: `ctest -R game_vulkan_rt_smoke` = `ou-game --renderer vulkan --preset enhanced --shaders original --rt shadows
  --level 'pc-w\l1_kraken_room.drm' --spawn -2204,-15106,2212,180 --frames 40` (and the l4_ballcourt spot)
  (`CMakeLists.txt:650-663`). Not re-run in this audit (real GPU hang on the user's desktop GPU).
- Expected: 40 frames, `RT shadows … BLAS` line, exit 0.
- Actual: GPU hang in l1_kraken_room (project process rule: always excluded; `docs/audit/AUDIT_B.md:733` "excluded
  (hang, project memory)" at 0.41.0; `docs/GAMEPLAY_DEVELOPMENT_REPORT.md:95`).
- Files: `src/render/shaders/sun.glsl` (`ouRayInputsFinite`), Vulkan RT path (`VkRenderer`, `scene_rt.*`).
- Cause: the original hang (non-finite ray origins from translated shaders, RENDERING L205) was fixed in 671e35e
  (2026-10-29), which added this test green; something later reintroduced a hang [I]. Unconfirmed.
- Fix: in a private compositor with a GPU-reset-safe setup, bisect 671e35e..v0.41.0 with the test; check new ray users
  (RT AO / GI `rt_indirect.comp`, water RT, BLAS refits) for non-finite inputs.
- Test: `game_vulkan_rt_smoke` itself.
- Status · Origin: Open · regression (between 671e35e and v0.41.0).

**OU-VK-002 CORTAO temporal is camera-only on Vulkan**
- Repro: l2_cliffclimb spawn (bobbing yacht), `--ao cortao`, Vulkan, watch the deck / Lara edges.
- Expected: no AO trails / halos (Enhanced feature).
- Actual: `cortao_temporal.comp` reprojects through the previous view-projection only (RENDERING L280); GL uses per-pixel
  motion because camera-only reprojection gave grainy halos on the CPU-moved yacht in GL [memory note]. Vulkan was
  measured stable on the deck (second-difference 17.6 / 41.1 vs 18.4 / 41.6 AO off, RENDERING L283).
- Files: `src/render/vk/shaders/cortao_temporal.comp`, `VkRenderer::recordCortao`.
- Fix: feed the pre-pass motion (GL's `prevVbo` scheme) to the Vulkan temporal pass if a hand-test shows halos on
  fast movers (Lara running, swinging objects).
- Repro result (a80bdb8): **Not reproduced in single frames.** l2_cliffclimb intro, Vulkan, `--input-script idle.txt
  --frames 400`, `--ao cortao` / `off` / `rt` (same sim digest, deck heaving): the cortao − off difference follows
  Lara's and the rail's silhouettes exactly, no displaced copy; open-water mean |Δ| 0.73 (`BugRepro/shots/ao_boat_*.png`).
  A bedroom run with Lara running did not move her (she starts against geometry; no useful frame). Temporal halos
  need a frame sequence while something moves fast: hand-test.
- Status · Origin: Not reproduced · new-feature defect risk (0.51.0).

**OU-VK-003 Anti-Lag crash in cutscenes / loading**
- Repro (pre-0.49.2): AMD GPU, `graphics.anti_lag on`, New Game → `001_pro_corridor`.
- Actual: crash in `libVkLayer_MESA_anti_lag.so+0x104e`; user logs `game-20261106-151224`, `-20261109-214853`,
  `-20261110-232156` end at the cinematic line [O].
- Cause (confirmed, CHANGELOG 0.49.2): nested present loops (cinematics, loading screen) presented without INPUT
  markers, filling the layer's 8-slot ring. Fix landed: `src/render/vk/AntiLag.hpp` pairs a marker with every present;
  Anti-Lag off under frame generation.
- Status · Origin: Fixed-verified (code + no recurrence in logs after 11-16) · regression (shipped with Anti-Lag support).

**OU-VK-004 `VK_ERROR_OUT_OF_DEVICE_MEMORY`, FSR frame generation context failed**
- Evidence: `game-20261031-201908.log:86` then `ou: FSR off: FSR frame generation context failed (ffx-api 3)`; also
  `game-20261102-224911` / `-230311` (MangoHud loaded) [O]. Not in logs since 2026-11-06.
- Cause (suspected): VRAM exhaustion at FG context creation with another overlay layer present.
- Fix: log heap budgets (`VK_EXT_memory_budget`) at FG creation.
- Repro result (a80bdb8): **Not reproduced.** Same GPU (RX 7900 XTX), Mesa 26.2.6 (the user's failing logs were Mesa
  26.2.5 builds of 2026-10-31 / 11-02): `--level 'pc-w\l2_cliffclimb.drm' --upscaler fsr --frame-generation on
  --frames 600` with `MANGOHUD=1` (layer loaded, log line `[MANGOHUD] [info]`) and `OU_VK_PRESENT_STATS=1`: FG active
  ("presents 1200 … real 600, generated 600"), no `OUT_OF_DEVICE_MEMORY`, no "FSR off"; also `--fsr-quality
  performance` 300 frames. Untested: the user's display resolution (the virtual output is 1920×1052) and a long play
  session. Status · Origin: Not reproduced · unknown.

**OU-VK-005 FSR SDK validation messages** — FG writes a 3-component texel to RGBA8
(`VUID-vkCmdDispatch-OpImageWrite-08795`), upscaler storage formats `Rg32f` / `R32f` over R16G16 / R16 images
(RENDERING L1222-1226). Status · Origin: Open (upstream SDK) · pre-existing.

**OU-STR-001 Multi-second `stream scene` at level load**
- Evidence: tick 14 `stream scene` 1990 / 3284 / 6793 / 3696 / 2646 / 8953 / 3286 ms in five logs 2026-11-06..10 [O];
  none above 1 s in 2026-11-15/16 logs.
- Note: ROADMAP L55 still lists the neighbour join rebuilding the whole render scene (~53 + 12 + 12 ms) next to the
  newer per-unit residency (4 + 2 + 2.5 ms).
- Repro result (a80bdb8): **Not reproduced.** Every load of this pass (async loads, Vulkan): `stream scene` ≤ 21.35 ms
  (tick 1), 0.55-1.08 ms later (`--chapter 1` / `--chapter 2 --no-cinematics --frames 300`, l2_cliffclimb, l3, l7).
  Second scene load via quicksave + quickload (`saveload.txt`: `2 save`, `4 load`, XDG dirs redirected): 0.61 ms
  (ch1) / 1.20 ms (ch2) at tick 121. Consistent with the per-unit residency having fixed it [I].
- Status · Origin: Not reproduced · unknown.

### UI and text (OU-UI)

**OU-UI-001 Game font / button glyphs missing** — fixed: text draws in the game font (generalbank DTP 46, glyph table
over textures 109 / 110, `src/ui/Art.cpp`, GAMEPLAY.md L4522-4524); button placeholders show the bound control of the
last device (keyboard names, Xbox 360 pad icons; tru.exe 0x5A7C20 / 0x8A9EF8) (ROADMAP L43). Test `Menus.ButtonPrompts`.
Remaining deviation: keyboard spellings are SDL's ("Space" vs "space") [I]. Status · Origin: Fixed-verified · pre-existing.

**OU-UI-002 Missing UI screens** — chapter title / letterbox screens, confirmation dialogs, PDA sonar map / weapon /
inventory / journal sub-screens, tailoring screen; Scaleform not executed (ROADMAP L60, L62, L63). Status · Origin:
Open · pre-existing.

**OU-UI-003 No Cyrillic; subtitles English only** — ROADMAP L44 (the original's font may lack Cyrillic too [I]); PL / RU /
CS / HU installs show English text (ROADMAP L43). Status · Origin: Open · pre-existing.

### Audio (OU-AUD)

**OU-AUD-001 No full FMOD / Mus! effects** — Mus! sequencer implemented: `.mus` scripts drive `.mul` stem gains
(`MusSequencer`, AUDIO.md L401; ROADMAP L64 "Tested"); FMOD DSP stand-ins (lowpass / highpass / EQ) in (AUDIO.md L398).
ROADMAP L46 still says "Mus! music sequencer not implemented" (stale, OU-DOC-002). Remaining gap: OU-AUD-002.
Status · Origin: Fixed-verified (Mus!) · pre-existing.

**OU-AUD-002 Reverb not FMOD's I3DL2; music bus dry**
- Expected: FMOD SFX reverb with 13 I3DL2 parameters set by 0x582270 (AUDIO.md L812).
- Actual: Freeverb-style tank, density / rolloff ignored, loudness unmatched, music bus not fed (AUDIO.md L819,
  ROADMAP L46); FMOD rolloff curve and absolute levels never compared with the original (AUDIO.md L420).
- Fix: implement an I3DL2-style reverb (or port FMOD's published SFX reverb model) and A/B against a Wine capture.
- Status · Origin: Open · pre-existing.

**OU-AUD-003 One global music volume fade unidentified**
- No repo doc or code names this fade; known music fades are per stem (0x5866B0 / 0x586650, AUDIO.md L343) and per
  slot master (+0x38..+0x44, 0x589370, AUDIO.md L611-619) [O]. Origin of the item: earlier session note.
- Fix: RE — trace writers of the music master besides the slot masters (e.g. pause / cinematic ducking) in tru.exe.
- Repro result (a80bdb8): **Not reproducible at runtime**: no symptom, level or log signature names this fade; it stays an
  RE item. Status · Origin: Not reproducible (RE item) · unknown.

**OU-AUD-004 Script sound waits and mover sounds** — script wait threads, uber mover / physics-lever sounds, kind-1 i16
+4 [U], 0x7B / 0x0D replay (ROADMAP L65). Status · Origin: Open · pre-existing.

**OU-AUD-005 Synchronous music loads** — tracks start a few frames earlier than in tru.exe; after a load ours replays the
saved slot requests with a 2 s fade-in [I] (ROADMAP L64). Status · Origin: Open · pre-existing.

### Camera (OU-CAM)

**OU-CAM-001 Moving platforms do not carry the camera focus**
- Repro: ride moving markup (`GameReal.MovingMarkupCogWheelByInput` spot) or a yacht deck; watch focus lag.
- Expected: tru.exe carries the focus with the platform (0x7027B0) [O decode, comment World.cpp:959-962].
- Actual: focus eases toward bone 108 at ≤ 1896.3 units/s per axis with no carrier velocity (`World.cpp:955-978`).
- Fix: add the carrier's frame delta (already known for Lara's riding, `Objects.cpp` moving markup) to `focus_`
  before easing. Test: proposed `Game.CameraFocusRidesMovingMarkup`.
- Repro result (a80bdb8): **Reproduced on the floating yacht** (vertical axis). Self-test series from the chapter-2
  start (`BugRepro/sc/deck_series_{swell,tru}.json`: 80 steps × 0.25 s recording `player.pos.z`, `camera.pos.z`,
  `camera.mode`), Lara idle on the deck, camera mode 1: with the swell the camera z stays **597.0 for the first 5 s**
  while her root heaves 79 → 158 → 89 → 132; on tru.exe's waves it stays **611-612 for 3.75 s** while she rises
  125 → 247. Only past that does the camera move, and then late: camera z peaks at 730 (swell) 1.5 s after the deck's
  peak (158), when the deck is already down at 54; 760 (tru) 2.5 s after its peak (291). Over 20 s: deck −151 .. 420 vs camera 511 .. 946 (swell); −25 .. 382 vs
  611 .. 977 (tru). The focus behaves as a vertical dead zone plus easing, not as a carried platform [O].
- Wine reference (WineMeasure 2026-11-16, `tools/wine-ref/sessions/l2_cliffclimb_sea.sh`; REFERENCE_COMPARISON new rows):
  in tru.exe the pivot and camera ride the deck exactly. Camera minus root stays 369..375 while the deck heaves ±203,
  lag 0.00 s. Ours: camera minus root 321..714, lag 0.75 s (original waves) / 1.0 s (FFT).
- Fix (agent/PlatformCam-2 80ce86f, merged into int/brief): the carrier's per-tick translation is added to the
  focus, the camera position and the desired point before easing, as tru.exe 0x609830 / 0x5FF650 do, and her move is
  made carrier-relative (0x5E7EF0). Measured: lag 0 ticks (was 57) and band 3.7 wide (tru.exe 5.9).
- **Residual:** camera minus root was 486-490 in ours vs 369-375 in tru.exe, about 115 units higher. Cause: the deck's
  camera trigger volume 3852 (OU-CAM-005) sets distance 948.17 and a pitch force set of +5° (camera 83 under the
  pivot). With it (agent/DeckCam6) camera − root is 376.0 (tru.exe 369.0..374.9; the 1.1 over is our pivot − root
  458.6 against 451.6..457.5). `StreamReal.CameraRidesTheYachtDeck` now asserts the absolute level from 8 s on (after
  the 5..7 s look-idle ramp of the forces).
- Tilt is not carried [U], and freeze request 55 is not modelled.
- Status · Origin: Fixed (lag / band on int/brief, level on agent/DeckCam6) · pre-existing.

**OU-CAM-002 Swim camera follows the wave bob exactly**
- Repro: `--chapter 1` surface swim / tread on the swell (Enhanced + FFT).
- Expected: unknown; tru.exe's surface-swim camera traces exist (`camfollow-wine/`, GAMEPLAY.md camera section)
  but its focus record dead zones for mode 19 / surface swim were not compared under swell.
- Actual [I]: focus goal = bone 108 through the mode's focus record; an axis with zone ≤ 1e-3 is followed exactly
  (`World.cpp:971`), so a zero vertical zone copies her bob.
- Fix: read the swim modes' focus records (settings +0x50) and check the z dead zone; if tru.exe follows exactly,
  the bob comes from our larger swell (C-SWELL).
- Repro result (a80bdb8): **Not reproduced.** Self-test series `BugRepro/sc/tread_series_{swell,tru}.json` (the
  ch07 route over the side and into the sea, then 80 × 0.25 s samples; only `swimtread` samples with `water.depth`
  44.1 ± 3 are used, a shark interrupts after ~9 s): surface-swim camera (mode 20) z moves **132** while her root
  moves **566** with the swell (23 %); 101 vs 430 on tru.exe's waves (24 %). The camera does not copy the bob, and
  the ratio does not depend on the swell. Status · Origin: Not reproduced · new-feature defect [I] (swell in gameplay,
  0.50.0).
- Wine reference (2026-11-16, tru.exe side only): the swim pivot follows the wave bob 0.41..0.53 s late at gain
  0.87..0.91; the camera height follows at gain 0.14..0.45. Camera mode switches in the same frame as the chart request
  (dive → 19, surfacing → 20). Ours' side is not measured yet.

**OU-CAM-003 Motorcycle camera placeholder** — `kDistance = bike ? 1711` [I] (`World.cpp:993`), swing 1002 [I];
VehicleCamera not decoded (AUDIT_A L361). Status · Origin: Open · pre-existing.

**OU-CAM-004 Reveal cameras** — blend in / out ours, key event 7 [U], gamebase time scales not applied (ROADMAP L51).
Status · Origin: Open · pre-existing.

**OU-CAM-005 Deck camera distance** — on the l2_cliffclimb yacht deck tru.exe's camera distance is 948.2. Ours was
1264.2 (mode 1 `distance`, `World.cpp`). Source: WineMeasure 2026-11-16, REFERENCE_COMPARISON new rows.
- Cause (Wine 2026-11-19, `openunderworld-work/w2/DeckCam6/camtrig.py`): a camera trigger volume. With her on the
  deck, CameraManager 0xE804F0 +0x10C = 1 active trigger. Its +0x8C entry is the trigger's intro data +0x18 block
  (priority 1, commands {0xD 948.17}, {0x18 inline pitch force set 5° at 250°/s, no forces}, {0x1C (0, 0, −79.01)}).
  Its intro is our l2_cliffclimb volume 3852 at (−34899, −15044, 322), half (711, 1128, 479). The earlier DTP reading
  (CAMERA.md) had paired the boxes with the wrong command lists. Code: 0x5CD520 → 0x5F29A0 / 0x5F2A70 (enter / leave)
  → 0x5E85B0, applied by 0x601D30 → 0x6019D0 (2 / 0xD → +0x1320, 0x17 / 0x18 → 0x609C40; with count 0 the strength
  is 1, 0x607B90).
- The same rule away from the deck: Lara held in volume 3857 ([2 790.14], [0x18 −17° 300°/s]) gives distance 790.1
  and a camera 231.0 over the pivot in tru.exe.
- Fix (agent/DeckCam6): `script::Object::camera` holds a volume's inline commands (Script.cpp), and
  `World::updateOrbitCamera` applies 2 / 0xD and inline yaw / pitch sets without forces of the volumes she is inside
  (priority 0 first), in every gameplay-camera mode. Measured: deck 948.2 and camera − root 376.0; 3857 790.1 and
  +231.0. Tests: `StreamReal.CameraRidesTheYachtDeck`, `StreamReal.CameraTriggerVolumeSetsDistanceAndPitch`.
- Knock-on effects: on the deck the camera is now 948.2 from her, as it is in tru.exe. `StreamReal.YachtCameraTurnsRoundTheCabin`
  was re-measured in Wine (`spin-deck2.txt`): a mouse turn circles at 948.2 and the cabin pulls it in to 879.9..905.2.
  The older "1264.2 elsewhere" was measured outside the volume. The `render_golden_cliff_water_*` goldens frame the
  deck at tick 120 and were re-blessed: Lara is drawn larger, matching the 948/1264 distance ratio.
- Not modelled [U] at the time: the other commands, kinds 1 / 2 and sets with forces; the deck volume riding the
  yacht. Followed up as OU-CAM-006.
- Status · Origin: Fixed on agent/DeckCam6 · pre-existing.

**OU-CAM-006 Camera trigger commands** — `World::updateOrbitCamera` applied only a volume's distance (2 / 0xD) and its
inline force sets without forces; the l2_cliffclimb yacht volumes (3851..3854) were tested at their intro places.
- Survey (`ScriptRealData.CameraTriggerCommandSurvey`, docs/re/CAMERA.md §9): 459 kind-0 volumes carry commands (58
  with priority); uses: 0x18 155, 2 129, 0x12 116, 0xD 109, 0x1A 73, 0x19 65, 0x16 54, 0x14 53, 0x1C 51, 0x1B 47, 0x15
  33, 0x17 24, 6 15, 4 7, 0 / 1 / 8 4, 3 / 0xC 2, 5 / 7 1; 89 force sets carry forces (types 2, 3, 5, 7). 9 kind-1 volumes
  name an AnchoredCamera record (0x5EE4A0); 8 of them have none, so 0x5E7BF0 refuses them.
- tru.exe (0x6019D0, jump table 0x601CC0): 0x12 = a level camera record of the unit (applied as a markup's; priority-0
  volumes under the mode records, the others over them, 0x601D30); 0x15 clears +0x1338 (the on-foot ray probe,
  0x600591); 0x17 / 0x18 sets with forces (0x609C40, strength = the product of the forces, 0x607B90); 0x19 / 0x1B and
  0x1A / 0x1C add (x, y, z) to the focus / look trackers (+0x3B0 / +0x620 +0x20 via 0x608280; 0x1B / 0x1C blend over arg 3
  when byte arg 4 is set, 0x608200); the camera orbits the focus and looks at the look point (0x6009C0 / 0x600B20).
  0x16 is the LaraCombatCamera's distance (0x603190), 0x14 flags a unit fade record (0x5ED2D0), 3..6 drive the
  yaw / pitch override +0x1248 / +0x1278 (0x609F10 / 0x609D40), 7 / 8 limits, 1 sets by id.
- Wine 2026-11-19 (`openunderworld-work/w2/CamZones7/camz.py`): 3852 look tracker +0x20 = (0, 0, −79.01), look 0.22°
  up with the pivot 5.00° up; 3859 pitch −6.00°, back from a nudge at 19.5°/s at ~31 fps (300 × min(1, 2 dt) per
  frame); 3861 record (unit 9, record 7) holds the camera 13 s. The yacht volumes are linked to the yacht (instance
  +0x214) and sit at its markers (3852 − yacht (1497, 1551, 811), ours at the marker (1547, 1530, 685)).
- Fix (agent/CamZones7): Script.cpp keeps the forces and the record pointer of each command; CollisionMesh keeps the
  unit's record list; `World::updateOrbitCamera` applies 0x12, 0x15, 0x17 / 0x18 with forces and the tracker offsets
  (`World::cameraPivot()`; `camera().target` is the look point); `script::Runtime` tests a volume whose body / offset is
  set (anchored: `World::poseAnchored`) in that frame. The orbit state (trigger sets, offsets, a volume's record) is saved.
- Tests: `ScriptRealData.CameraTriggerCommandSurvey`, `StreamReal.CameraRidesTheYachtDeck` (look 0.22°, stays in the
  riding volume), `StreamReal.CameraTriggerForceSetEasesPitch`, `StreamReal.CameraTriggerRecordHoldsTheCamera`.
- Knock-on effects: the `render_golden_cliff_water_*` goldens frame the deck at tick 120 and were re-blessed (the view
  now looks at the point 79 under the pivot: level, as tru.exe's 0.22°, where it looked 5° up; image-reviewed). In
  l1_ship_sinking_hall's second shaft volume 1061's yaw set (0°, force type 2 k 0.1) now holds the climbing camera
  behind her (yaw 89.5 where the hang set 15 had swung it to 117.3), so `ch06_second_shaft_valves`'s jump-up stick
  was turned to keep the old world direction (0.884 forward, −0.468 right).
- Batch 8 (agent/CamRest8, docs/re/CAMERA.md §9 "Angle overrides"): 0x601D30 runs on a mode or trigger-list change, not
  per frame. 3 / 4 (global data +0x170 / +0x174 by id) and 5 / 6 (inline) set the yaw / pitch override (+0x1248 / +0x1278,
  0x609F10): it turns its angle in place of the force set (0x609D40, not scaled by the look-idle weight), its speed
  blending in over the 0.25 s clock from the previous override's, and switches itself off within 1° after `hold`
  seconds; look input and level records switch it off. 1 picks a pitch set by id (sets 0 / 6 / 26 read from global data
  +0x168 under Wine). 7 / 8 set the look limits {centre, range} as the mode lists' do (blend 0.25 s).
  Wine (w2/CamRest8, l3_main_bedroom): walking into 4468 ({4 id 9 = (−5°, 1500°/s, 0)}) inside 4476 the pitch went −10 →
  −5 in two frames and the override switched off with cur = target × 0.133 (+0x1278 read back), leaving reset it; 4349
  ({8 −10 10}): +0x8C4 centre 80° range 10°, the mouse stops at 0.00 / −20.00. Ours, walked in from tru.exe's start: off
  after 2 ticks with cur (0.198, 3.491), peak −5.00°; −20 / 0 limits; `StreamReal.CameraTriggerPitchOverrideAndLimits`.
  Triggers now apply in CameraManager list order (entering appends 0x5E85B0, leaving moves the last into the hole
  0x5F2A70): 4468 entered inside 4476 gives 790.1 as tru.exe, where our uid order gave 948.2.
  New saved orbit state: both overrides, the limits' clock and target, the trigger list.
- Narrowed, still open: 0xC is a cut (0x6017D0: blends end, the camera put at the targets' angles at +0x1320) but a Wine
  check at l1_krakenstairs_b 599 was confounded by the teleport's own camera reset; 0x14 flags a **focus point** (unit
  data +0x200 / +0x204 records, the `focuspoint` script type), which the focus-point blend 0x5F0B30 / 0x5F1540 (+0x14C0)
  turns the look toward — a subsystem not ported; 0x16 is read only by the LaraCombatCamera (0x603190), which ours lacks;
  9 (+0x133C) unread; kind 1 (AnchoredCamera, one usable record of 9); the frame of x / y tracker offsets; a volume
  record of type 1 / 2 off a markup (her place 0 [I]); the look tracker's own lag (ours uses the focus's). OU-CAM-004 (reveal
  blends) not touched.
- Status · Origin: Fixed on agent/CamZones7 + agent/CamRest8 (partial: see open) · pre-existing.

**OU-CMB-003 Shark aggression**
- Repro: l2_cliffclimb, into the sea and tread (`w2/WineMeasure/ours/tread_*`): ours bit within ~7 s and every 1.5..3 s.
- Expected [O Wine 2026-11-19, `tools/wine-ref/sessions/l2_cliffclimb_sharks.sh`, traces `w2/SharkAi6/s1`, `s2`]:
  treading 300 s at the yacht and 300 s in open water: 0 bites; the sharks see her (sense flags 0x80F7) but run
  EnemyBehavior_Search, never Close / Circle. Close (vftable 0x856B3C), Circle (0x6298C0) and CloseCombat (0x6391F0) need her
  reachable: 0x660640 → 0x64A5B0 takes the nearest point of the shark's NavPower 3D space (AI +0x5030, bfx Mover3D from
  node +0x154, 0x637B20) within enemy data +0x84 (948.2) across / +0x88 (395.1) up-down. That space's top: shark origins
  never above 892.2 under the sea volume's top; pinned 500 under she is engaged, treading down to 494 under not. In
  reach (dive, pinned 500..1100 under): Close 2.0..5.6 s / Circle median 7.0 s at 4164, a pass per 14 s per shark, 6
  bites of 200 in 232 s by two sharks (0.78 / min each, first 13..26 s); every pass within 370 bit. No guard zones.
- Fix (76cc040): `Enemy::reachable` / `kSwimCeiling` (out of reach she counts as unseen), sharks swim no higher than
  that top; Chase = a pass of the combat record's +4..+8 counts (0x6108A0), then `Mode::Circle`; bite 200 from node
  +0xC4 record +0x50; speed 1780 / circle 1492 measured. [I] calibrated: kPassBite 6/29, circle 7..13 s (EnemyBehavior_Circle
  end condition and what makes a pass come near not decoded); ours idles where tru.exe searches.
- Test: `AiReal.CliffclimbSharksSpareATreadingLara` (treading 300 s at 64 / 64 / -380: 0 bites, 0 engaged); before: 113
  bites in 120 s, first 6.7 s. `AiReal.SharksSpawnSwimBiteAndDie`, combat / ch07 self-tests, `ctest -L protected` pass.
- Follow-up (agent/EnemySpawn7, batch 7):
  - Presence [O]: shark 4023 (and 4024-4032) is a dormant intro (+0x72 & 0x80); 0x5C2360 skips +0x72 & 0x82 at the
    unit load and a script's Birth introduces it. The game already obeyed it (World → `script::Runtime::born`); the
    bare `ai::Enemies` the AiReal tests use spawned every dormant intro (31 units, e.g. all l1_ship_exterior mercs), so
    the 1.47 / min came from a three-shark harness. Now dormant intros wait for `birth()` there too;
    `AiReal.SharksSpawnSwimBiteAndDie` asserts the l2_cliffclimb load set {4020, 4021, 4022} (Wine list) and l1_seafloor /
    l7_arctic_sea 3 placed. The only other load-time skip, an intro dependency list (+0x80 / +0x84, 0x5BD500 / 0x5BD650 /
    0x5B43E0), occurs once (l4_pool 5607, not an enemy).
  - Passes [O]: EnemyBehavior_CloseCombat 0x6108A0 rolls slots of combat record +4 .. +8 counts (2 .. 4 s) and gives each
    to its Circle or its Sidestep child by record bytes +0xC / +0xD (50 / 50; rand % 101); a shark's non-Circle slots are
    passes [I: no sidestep]. Replaces the fitted 7 .. 13 s circling (Circle's own timer, record +0xBC / +0xBE, is 0).
  - Bite range [O Wine, s2 traces]: origin to origin, bites at 196 .. 405 (shark 0 .. 172 under her): 370 / 405 replace
    the flyer centre-to-head range, so 800 / 950 under now bite as in tru.exe (before only ~1100 under could); 500 under
    cannot (shark origin 392+ under hers), as tru.exe's 28 s there.
  - Measured (twelve rolls, 300 s each at 800 / 950 / 1100 under): 0.98 bites / min per engaged shark (tru.exe 0.78;
    6 bites, 95 % band 0.3 .. 1.6), a pass per 11.6 s (14), first bite median 36.2 s (13 .. 26), Circle median 4 .. 6 s
    (7.0). Test bounds 0.5 .. 1.1 / min, 10 .. 18 s per pass, first-bite median 8 .. 45 s.
  - Still [I]: kPassBite 6 / 29 (how near tru.exe's Close path comes is a NavPower 3D move, not decoded); non-biting passes
    swim by at 580 (the nearest measured); out of reach ours idles where tru.exe searches; tru.exe's short Search phases
    between passes are not modelled (our Circle runs shorter, passes slightly more often).
- Status · Origin: Fixed on agent/SharkAi6; presence and pass timing on agent/EnemySpawn7 · pre-existing.

### Lara's controller and traversal (OU-PLY)

**OU-PLY-001 Beam-walk entry into "about to fall" undecoded**
- Repro: l3_chapel beam 1031, Forward + Left held (REFERENCE_COMPARISON "Unmeasured", M10).
- Expected: tru.exe kind-1000 chart conditions (not decoded); controller 0x742330 / 0x742770 and fall query 0x76D4D0
  decoded [O] (ROADMAP L48, GAMEPLAY.md L1583).
- Actual: ours enters after 45 frames of stick across [I]; lean spring and catch ledge test [I].
- Fix: decode the kind-1000 conditions (chart transitions into the about-to-fall state), then Wine-measure M10.
- Test: `BeamOffBalanceCounterStickRecovers`, `BeamUnbalanceAndWobble` (AUDIT_A L130). Status · Origin: Open · pre-existing.

**OU-PLY-002 Wall-hug corners reconstructed** — corner trigger point and angle source (lookahead 0x74BCF0), stand-off
88.496, MoveDir mapping [I] (ROADMAP L75, GAMEPLAY.md L2608-2650). Fix: decode 0x74BCF0's result use. Status · Origin:
Open · pre-existing.

**OU-PLY-003 No climb out of water onto a shelf**
- Expected: `swim_climbout_to_wallhug` (887) onto a shelf when the clearance result is 657 (GAMEPLAY.md L1926) [O chart].
- Actual: not done (GAMEPLAY.md L2614, ROADMAP L75); ordinary climb-out only.
- Fix: add the shelf branch to the automatic climb-out using the wall-hug shelf test 0x74E160 / 0x74BB50.
- Test: proposed `GameReal.SwimClimbOutOntoShelf` (l2_puzzleroom shelf). Status · Origin: Open · pre-existing.

**OU-PLY-004 No ladder side jumps or reaches**
- Expected: ladder side jumps exist (ship escape shafts, ROADMAP L89); clips in lara.drm [I].
- Actual: not done (ROADMAP L75); blocks OU-PZL-005 / ch06 escape shafts.
- Fix: decode the ladder states' side-jump transitions (chart) and add them in `src/game/Traverse.cpp`.
- Status · Origin: Open · pre-existing.

**OU-PLY-005 Ledge-aim reach / running jump** — tru.exe switches from straight rush to re-aimed arc at 614..649, ours
at 634..719 (REFERENCE_COMPARISON M13, Differences #3); the 1233 box limit is [O] but its mechanism [I]
(GAMEPLAY.md L2567). Files `Player::aimGroundJump`. Test `RunningJumpReachAndRush`. Status · Origin: Open · pre-existing.

**OU-PLY-006 Low-obstacle detection distance unknown**
- Actual: candidates accepted within `radius + 40` (`radius + 120` after a jump) — `// [U] query 96's reach`
  (`Player.cpp:1088`, `:1159`); clearance test 0's first segment not applied [U: frame] (GAMEPLAY.md L194, `Player.cpp:1062`).
- Fix: decode query 96's reach record (property 0x21D, objdata +0x116B0) or Wine-measure hurdle onset distances on
  l2_cliffclimb step 941. Test `CliffclimbObstaclesAndLedgeGrabs`. Status · Origin: Open · pre-existing.

**OU-PLY-007 Pole fling lands 79 past the edge** — tru.exe touches down 3 short and steps on (`polefid2/mhsw.txt`,
REFERENCE_COMPARISON M07 / Differences #5). Files `Player::poleFling`. Status · Origin: Open · pre-existing.

**OU-PLY-008 Running safety drop lacks overshoot** — tru.exe overshoots 82 and is taken back, hang ~1.2 s after the line
(REFERENCE_COMPARISON M03 / Differences #6). Files `Player::trySafetyDrop`. Test self-test `safety_drop`.
Status · Origin: Open · pre-existing.

**OU-PLY-009 Chimney reach / standoff** — tru.exe takes walls 24..35 nearer and holds 119..131 out of the face, ours 110
(third hold 30 high) (REFERENCE_COMPARISON Differences #10, `WallJump.cpp:43`). Test
`GameReal.JunglehubChimneyJumpClimbsColumns`. Status · Origin: Open · pre-existing.

**OU-PLY-010 Unported moves** — free hang `ledgeu_*` (decoded, not ported, ROADMAP L90), `bbeam_drop_*` and beam sprint
(ROADMAP L48, GAMEPLAY.md L1590, L4412), pole-to-perch / pole-to-ledge (`hpole_para_to_perch`), vaults over / down,
bad-attach / wrestle clips of poles, ladders, pillars, free-climb wrestle (ROADMAP L72). Status · Origin: Open · pre-existing.

**OU-PLY-011 Saving grab recovered by Jump** — AUDIT_A L65 (incorrect at v0.41.0); now Interact:
`Player.cpp:2557` `b.interact.buffered()` → `ledgeb_bad_attach_recover` (ROADMAP L85, tests `Game.SavingGrab*`,
`GameReal.SavingGrabClips`). Interact recover not yet Wine-traced. Status · Origin: Fixed-verified · pre-existing.

**OU-PLY-012 No depenetration pass** — "an already-touching contact being left is ignored; query mask 0xFF"
(GAMEPLAY.md L4426).
- Repro result (a80bdb8): **Reproduced for an idle Lara.** l3_main_bedroom, the wall in front of the `ledge.json`
  spawn: running into it (`wall_contact.json`, yaw −90) stops her root at y −4385.4. Spawned 35 units past that face
  (`--spawn`-style scenario `pen_stand_35.json`, `[-8470, -4420, 244, 90]`) and left alone for 1.5 s she stays at
  y −4420.0 (moved 0.0). Moving resolves it: running away (`pen_out_35`, `pen_out_75` at −4460) leaves freely
  (1598 / 1465 horizontal), running further in (`pen_in_35`) ends at the face y −4385.4 (moved 37 the other way). No
  in-play stuck case was found; the risk is placements (respawns, loads, moved platforms) that start her inside.
- tru.exe (Wine, 2026-11-19, agent Depen6; traces `re-baseline/Depen6/`): after a new game, `ref tpv` holding her
  3..15 ms at x 1152 (ours −8470), y −4076 / −4087 / −4107 / −4127 / −4152 / −4182 (10..116 past the y −4066.2 where a
  walk into this wall stops her in tru.exe's frame = ours + (9622, 333, −2977)): the next frame she is at
  y −4065.6 / −4064.5 / −4062.5 / −4062.0 / −4064.3 / −4065.3, x unchanged — pushed out along the wall normal, fully, in
  one frame, at every depth (even with the root past the face plane); afterwards a sub-unit creep. A 600 ms hold
  inside instead throws her ~4000 units (harness artefact: re-embedding every frame). The code is not decoded
  (her proxy, instance +0x288, created by 0x7018E0 / 0x494E60, is a physics body) [U].
- Fix (96d30d4): `CollisionMesh::overlapCapsule` + `Player::depenetrate`: the deepest overlapping terrain wall face
  pushes the capsule out by depth + 1 along its horizontal normal, up to 4 passes, in one tick. It runs once, on the
  tick after a placement (`Player::spawn` / `restore`: spawns, loads, restores, teleports) in ground states.
  Tests: `GameReal.EmbeddedLaraIsPushedOutOfTheWall` (35 and 75 past the y −4385.4 contact: at it after 1 tick, x
  unchanged, Idle 1.5 s), `Game.EmbeddedCapsuleIsPushedOutInOneTick` (synthetic wall and corner, a 0.3 touch left
  alone, the run into the wall unchanged), `Collision.CapsuleOverlaps`. Tests that put her at an object's origin
  inside its wall and relied on staying there now stand her where she can be: the l3_chapel switches 4819 / 4822
  (`test_gates` `placeToUse`, `test_save` `placeToUse`: origin, else the first side 100 out where she still engages
  it once settled) and receptacle spots (`engageSpotsIn` drops floor points whose standing capsule is inside a terrain
  wall: l5_valgrind cog 7312's nearest socket spots were ~73 units in).
- Left open: tru.exe does it every frame. Tried per tick in `groundMove`: 9 scenarios broke because our own [I] moves
  end inside our collision — the ch01 roll's stand-up under the leaning beam (`under_the_beams`), ch03's switch
  stand point 76 units into the gear room's wall (`down_to_the_lower_hall`), the chapel switch 4819's origin 24 inside
  its wall (`StreamReal.UberStateSaveLoad`), a running landing 34 into a ch06 wall, a rune block (dynamic) pushing every
  tick. Also not like tru.exe: a capsule whose axis passed through the face stays (tru.exe still came out on her side
  from 38 past the face plane), and air / hang / swim states have none. Our walk contact at this wall is 14 units short
  of tru.exe's (y −4385.4 vs −4399.2 in our frame).
- Batch 7 (agent/TickDepen7, on int/batch7): the moves that ended inside our collision were mostly `Player::slide`'s.
  (1) 1158366: a face the sweep touched or overlapped at t = 0 that the move does not close on (tangent / leaving) made
  slide() apply the whole move past every face ahead — brushing one wall walked her through the next (l2_puzzleroom shelf
  476 is 157.2 wide, her capsule 158). Sweep flag `kSweepSkipLeaving`. (2) f5c5eed: tru.exe's segment-triangle test
  0x461640 is one-sided [O] (hit only when -dot(dir, (v1-v0)x(v2-v0)) > 1.19e-5; serves BIH queries 0x462FA0 / 0x46C450,
  0x46C450 <- 0x4E8FC0 <- the player-code ray 0x493330; whether her proxy's contacts use it is [U]); Lara's ground sweep
  is one-sided (`kSweepOneSided`) and the push-out only uses faces whose front her axis is on. The ch01 roll under the
  leaning beam passes the beam's faces 965 / 966 / 907 from behind, as the walkthrough plays it. (3) 91405ad: push-out on
  every ground tick (placed_ removed); the roll's end follows chart S7 (T250 low ceiling -> crouch, T253 Crouch + stick ->
  Crouch_Front), so it crawls out from under the beam. GameReal.PuzzleRoomWallHug starts clear of the 293.5-high overhang.
  Tests: Collision.SweepSkipsFacesItLeaves, Collision.OneSidedSweepPassesFacesFromBehind,
  Game.RollEndsCrouchedUnderALowCeiling. Scenario results on the merged tree: ch01_bedroom_ring_door,
  ch01_ring_door_walk_through, kraken_kill, ch06_second_shaft_valves (with CamZones7's rotated stick; its route lands on
  grate 1245 and air-attaches to hold 212 without passing through object 1245), chapel 4819 / UberStateSaveLoad,
  GameReal.PuzzleRoomWallHug pass; `-L protected` 41/41.
- Sidedness, Wine (batch 8, 2026-11-19, prologue frame = ours + (9622, 333, −2977); traces in
  openunderworld-work/w2/TickDepen7/{behind,front,behind2,behind3}.txt): l3_main_bedroom wall face 236 (plane x −10672,
  facing −x, nothing behind it, floor 1593 / 1605 both sides). Walking −x into it from 220 behind, tru.exe lets her
  through: no stop at the plane; as her axis passes it she jumps 91 in one sample to 79 in front of it (−1041.9 →
  −1133.3 tru x; plane −1051) and walks on. From the front she stops 68 short of it. Face 112 / 113 from behind: the
  same jump through (−1022.2 → −1102.9, plane −1018). A third try (face 176, −y) stopped 106 short of the plane with
  no other terrain face there (an object, [U]). So her wall contacts against terrain are one-sided and the per-frame
  push-out works from the front only — what f5c5eed / 91405ad do [O-runtime]. The routine her proxy uses is still [U]
  (0x4ECBB0, a body-point contact via 0x46C450, also one-sided, is a candidate).
- Slide-only (1158366 on the merged tree, without f5c5eed / 91405ad; build wt-TickDepen7s): 7 of 516 fail
  (Collision.SweepSkipsFacesItLeaves as committed in C1, GameReal.PuzzleRoomWallHug, StreamReal.SaveContinuesBitIdentically
  timeout, ch01_bedroom_ring_door, ch01_ring_door_walk_through, ch03_gear_room_east / _west) and protected_collision:
  the three commits only stand together.
- ch03 gear rooms (fixed): switch stand point. tru.exe's engagement start 0x704730 -> 0x6F4740 targets the point of
  the switch's collision shapes nearest her (0x4DF090 per shape type; box 0x46AE20) with the shape's normal there made
  horizontal. Wine at switch 4525 (`ma_ei_wall_switch_electric`, useDistance 135): she ends 174.0 out of its origin
  along +X, 3.0 aside; our old rule (135 out of the origin) put her 39 short. `World::useSwitchClip` now stands her
  useDistance out of the nearest point of the switch's solid boxes along that face's normal (179.4 / 1.6 at 4525; [I]
  the distance). Switch 57 / 55 (`ms_ei_switch_oneway_wall`) stand on a 2282-high pedestal; the old point (135 from the
  origin) was inside the pedestal, the new one is 249.5 out, beside a post (x −12941..−12893) that pushes her 48 west.
  ch03_gear_room_east passes unchanged; ch03_gear_room_west's `go` from there to the lower hall ran through that post:
  it now goes round its south end and joins the old line (waypoints only; expectations unchanged).
  GameReal.LaraPlaysInteractionClips pins 4525 to the Wine numbers (±6 / ±3).
- SaveContinuesBitIdentically case 9 (setup changed): she walks roped on the drained goo's floor of l4_pool; −y she
  meets the back of face 5096 (normal (−0.48, −0.79, −0.38)) at y −554 and, past it, the 64° slope at y −540, which
  stumbles her off the rope (Land), so she is not in GrappleSwing at the save. The old setup relied on the double-sided
  sweep stopping her at 5096's back. She now walks +y, where the floor is level for 700 units.
- Survey (2026-11-19): of 427 802 terrain wall faces (>= 100 sq units) 244 933 have nothing within 300 behind them and
  37 712 have a floor 150 behind at body height (walk-through from behind, as tru.exe at face 236; mostly level shells:
  l1_seafloor 4 022, l1_kraken_room 2 892, l7_arctic_sea 2 757, l4_ballcourt 2 693). Open: deep placements behind a face
  (Wine: out from 38 past the plane), air / hang / swim push-out, the walk contact (tru.exe stops her root 66.2 / 68
  from the face, under the proxy radius 79.01), our raycast is double-sided where 0x493330 is not, the switch stand
  distance (5.4 off at 4525).
- Status · Origin: Fixed on agent/TickDepen7 (1158366, f5c5eed, 91405ad + stand point / tests; see the commits) · pre-existing.

**OU-PLY-013 Slope-slide upper bound** (docs/re/VERIFICATION.md V002, medium confidence)
- tru.exe: the ground check 0x5DCE20 reads only player data +0x100E (52°; cos read at 0x5DD3BA / 0x5DD4F4 /
  0x5DD578). It flags a slide face when cos52 < nz < 1.
- The 28° value (+0x100C) is read only by the slide controller 0x73FF50 (0x740036), 0x71B550 and 0x720170.
- OU: `floorKind` (`Player.cpp:394`) also requires nz <= cos 28°, so a slide-flagged face flatter than 28° is
  standable in OU but a slide in the original.
- Fix: move the 28° test into the slide controller as tru.exe does; test with a synthetic 20° slide face.
- Status · Origin: Open · pre-existing.

**OU-PLY-014 Ledge wall-side limit for fast arcs** (VERIFICATION.md V042)
- tru.exe 0x6FC6B0 compares a 2-D dot (0x4898E0) with 0.2618. It loosens the limit to 0.5236 when the velocity at
  `[edi+0x80]` is longer than 3.1605 per frame (0x6FCAD2-0x6FCAE4).
- OU `wallSide` (`Player.cpp:1526-1530`) always uses 0.2618.
- Which object +0x80 belongs to is unresolved [U].
- Status · Origin: Open · pre-existing.

**OU-PLY-015 Land state on a dropping deck** (PlatformCam-2, 2026-11-16)
- Repro: chapter 2 start, 15 s idle on the yacht deck (tru.exe waves); test `StreamReal.CameraRidesTheYachtDeck`.
- Actual: ours enters State::Land once as the deck drops (bone 108 drops 60 for 0.7 s; pivot−root dips to 426).
- tru.exe (Wine deck traces): stays in one chart state (rq 46) for all 15 s.
- Cause: at 6.5 s a script object spawned, so `syncObjects` rebuilt every collider at its intro frame before her
  player step. The heaving deck's collider stood at its intro height for that step; her ground probe found nothing and
  she fell for one tick, then landed (`World::tick` → `syncObjects`, then `Player::tick`, then `tickObjects` poses them).
- Fix (agent/DeckCam6): after a rebuild in a running world (`ticks_ > 0`) `syncObjects` poses the colliders and uber
  models where their objects are, as it already did for a restored world. tru.exe has no such global rebuild: each
  instance's collision follows its own segment matrices. Test: `StreamReal.CameraRidesTheYachtDeck` asserts State::Idle
  on every tick for 15 s and pivot − root ≥ 446.6 (now 458.6 constant, was dipping to 425.7).
- Status · Origin: Fixed on agent/DeckCam6 · pre-existing.

### Swimming (OU-SWM)

**OU-SWM-001 Under-water turn rate unmeasured** — tru.exe steering function 0x755A00 (properties 0x200..0x20F, speed table
512..519) not ported [U] (GAMEPLAY.md L1886-1887, ROADMAP L82); turn / pitch rates are an unmeasured row (M29 / M85,
REFERENCE_COMPARISON "Unmeasured": l2_cliffclimb sea session). Test `Game.SwimFollowsTheCameraPitchUnderWater`.
- Wine-measured 2026-11-16 (WineMeasure, REFERENCE_COMPARISON new turn rows). Forward + Right/Left held, camera free:
  - tru.exe settles at 46.7/46.4 deg/s at the surface and 48.3/48.0 under water.
  - Ours settles at 43/56 at the surface and 57..60/56 under water: different from tru.exe, and left/right uneven.
  - On release ours snaps back at 118..143 deg/s; tru.exe at 52..73.
  - Breath matches: 4000 frames last 133.5..134.0 s in tru.exe, 133.3 s in ours.
- Status · Origin: Reproduced · pre-existing.

**OU-SWM-002 Surface swim lag** — velocity lags heading ~9° vs tru.exe ~16° (`camfollow-wine/turn1.txt`, M29 /
Differences #7). Files `Player.cpp` SwimSurface `kSwimTurnAccel`. Status · Origin: Open · pre-existing.

**OU-SWM-003 Scuba slot, grenades under water** — scuba slot not read live; grenades under water unchanged; swan-dive
input window and dodge side flag [I] (ROADMAP L82). Status · Origin: Open · pre-existing.

### Grapple (OU-GRP)

**OU-GRP-001 Throw stand-in; hook attaches at once**
- Actual: free (non-mounted) targets play `grapple_rappel_free_jumpon` as the throw (`Grapple.cpp:170-181`); hook flight
  absent, rope strip look [U] (ROADMAP L74, AUDIT_A L297 "placeholder").
- Fix: find the throw clip in `g_grapple` animations, add hook flight time. Status · Origin: Open · pre-existing (46b2ef4).

**OU-GRP-002 Swing unmeasured** — rope length at hang, period, decay, growth, jump-off speed unmeasured; five Wine
sessions failed; next: l4_temple ring 4951 / l3_chapel (REFERENCE_COMPARISON M20 / M99, ROADMAP L91).
Status · Origin: Open · pre-existing.

**OU-GRP-003 Jump into the gap / leap off a wall** — AUDIT_A L166 / L207 / L213 (incorrect). Now only a mounted pull
target releases on Jump (`Grapple.cpp:364`); roped at an edge Jump starts the jump into the gap keeping the rope
(`Grapple.cpp:430-445`, T1407 / T1409); `jumpOffRope(move)` takes the stick (`:260`, `:466`). Tests `Game.Grapple*`.
Status · Origin: Fixed-verified · pre-existing.

**OU-GRP-004 ch10 roped leap ends in the pool** — known gap `ch10_maproom_wall_run` (l2_maproom, step `leap`): rope 2670
swings her under the pool (water 870 under the bridge) where the rope is let go; what shortens tru.exe's rope is [U]
(SELFTEST.md known-gap table). Status · Origin: Open · pre-existing.

**OU-GRP-005 ch12 lever 6695 not held by her weight** — known gap `ch12_trailstart_lever_gate` (`let_go`): lever pulled by
Interact stays out (joint 1264); tru.exe holds it by rope tension and it returns unloaded. Grapple pull heave / rope
weight unmeasured (M25 / M26). Status · Origin: Open · pre-existing.

### Progression blockers (OU-PZL)

Each is a parked scenario in `tests/selftests/known_gaps/` (cause text in docs/SELFTEST.md) or a blocker in
docs/GAMEPLAY_DEVELOPMENT_REPORT.md §7; regression test = the scenario moved up to `tests/selftests/` once it passes.

| ID | Level / repro | Expected (original) | Actual / suspected cause | Test |
|---|---|---|---|---|
| OU-PZL-001 | l3_main_bedroom, beams north pocket | walkthrough route by input | no input route; second leg starts off-checkpoint; fire volumes cost health (REPORT §7 row 01) | ch01 legs |
| OU-PZL-002 | l1_baldrs_sideroom ledge 35 → perch 30 (1620 away, 134 up) | jump reaches perch 30 | running jump lands on pillar 29; tru.exe take-off spot unknown | `known_gaps/ch04_column_hall_perches` |
| OU-PZL-003 | l1_ship_exterior T junction | way west to volume 1436 | west recess shut | `known_gaps/ch05_natla_room` |
| OU-PZL-004 | l1_ship_sinking_hall after plane 1202 | roll reaches plane 1203 | stopped on 45° floor against a 100-high step, 3850 short | `known_gaps/ch06_hallway_roll` |
| OU-PZL-005 | l1_ship_sinking_hall top valve wheel → bar 238 | side jump to bar | lands hanging on beam ledge 253 (ladder / side-jump rules, OU-PLY-004) | `known_gaps/ch06_second_shaft_exit` |
| OU-PZL-006 | l2_shrineroom after the statue pull; l1/ch08 statue floor | slabs fall, shaft opens | freed slab bodies stay solid; ring stays a target | `known_gaps/ch09_shrine_shaft_descent` |
| OU-PZL-007 | l4_ballcourt calendar 5477, mask set | jaguar handle turns sections | no joint moves; unlock rule after the mask not decoded | `known_gaps/ch13_ballcourt_calendar_solve` |
| OU-PZL-008 | l4_temple east ledge | ring 4951 offered; statue head topples | ring offered (agent/Joint8); head not moved by the rope's bend | `known_gaps/ch13_temple_block_topple` |
| OU-PZL-009 | l4_ballcourt exit switch 5469, socket empty | locked until keyhole 6501 (modifiable 7 (2)) [O bytecode] | switch usable; ours honours modifiable 7 only for `ar_ei_switch_oneway` | `known_gaps/ch15_ballcourt_exit_switch` |
| OU-PZL-010 | l4_pool rune block 5609 | dragged | no joint record → nothing drags it | `known_gaps/ch15_pool_rune_block` |
| OU-PZL-011 | l5_valaskjalf checkpoint 7067 | reach volume 6866 | lower 790 of the central steps absent from collision / objects [U]; edge 1542 is clearance class 1 (no pull-up, tru.exe 0x72BAE0) | `known_gaps/ch17_hammer_chamber` |
| OU-PZL-012 | l7_yggdrasil_end entry walkway | jump onto a passing stone (rig 8389); towers / mounts | ledge 1240 out, 540 up: falls short; fight unplayed by input. ROADMAP L95 says Natla cannot be hit (vt 22), REPORT §7 says Mjolnir hits reach her script (OU-DOC-002) | `known_gaps/ch21_platform_ride`, `natla_platform_ride`; `ProgressionReal.NatlaFightByInput` |
| OU-PZL-013 | level entries with a bike but no placing script (ch14 et al.) | bike present | placed only where a level script places it (REPORT §8.1) | ch14 legs |
| OU-PZL-014 | prologue ring door pulled by grapple | leaf thrown into the bedroom, lands flat 656 from its frame [O Wine s4.txt] | fixed (6d6c9eb): leaf and ring thrown into the room, flat (1.1k) | `ProgressionReal.PrologueRingDoorPulledOffByTheGrapple` |
| OU-PZL-015 | ch08 Bhogavati statue / lift; ch14 thrones | statue floor, lift drop, thrones run | `multibody` 3 unbound; thrones stop at an unbound native (REPORT §7) | ch08 / ch14 legs |

All: Status Open · pre-existing.

Decoded causes, 2026-11-16 ([../re/PUZZLE_BLOCKERS.md](../re/PUZZLE_BLOCKERS.md), commit 8b2543a; evidence and
proving tests there):
- **OU-PZL-007:** a test bug. The scenario grips section 0's locked lever. The jaguar handle is section 1's record
  528; with it, ours already turns ring joint j2 by −0.646 rad in 4 s.
- **OU-PZL-009:** confirmed. tru.exe's modifiable 7 posts message 0x2E (0x4A5A90 → anim-graph 0x6B453B). Drop the
  `ar_ei_switch_oneway` name check. The regression it guarded is gate {l1_kraken_room, 498}, which needs the
  kraken-dead message first.
- **OU-PZL-010:** confirmed. `ca_pu_thor_debris` has no joint records; PushPull 0x74AC00 drives the body directly.
  Fix: a world three-axis joint on first grip.
- **OU-PZL-006:** confirmed. Fixed joints (type 8) are never created, so removing them releases nothing
  (`Objects.cpp:651`, `:539`, `:797`).
- **OU-PZL-008:** split into three causes. (a) The sight ray ended at the named post's origin, 4.4 under the top face
  of its floor; tru.exe ends it at 0x6FF040's query-0xDE feature (117.8 up the post), the filter is not the difference.
  Fixed on agent/Joint8 (`TempleRingOfferedFromTheEastLedge`; either end seen counts [I]). (b) Rope bends don't tug
  the bodies they wrap: tru.exe's bends are small physics bodies chained to the rope (0x6A4D40 → 0x699A50 / 0x699960
  joint 0x4E03F0); needs a Jolt rope chain. (c) multibody 92 / 93 bound.
- **OU-PZL-015:** multibody 3 = set "fixed" bit 0 at physics +0x15C on every body (inverse of 2). The throne native
  is still unknown.
- **OU-GRP-005:** moves to C-VM. Vt 46 is not called on rope attach, larabase 26 (0x777C30) is unbound, and our heave
  releases the rope.
- **OU-PHY-001:** narrowed to fixed joints plus body-A motion. Type 5 / 7 joints never occur on moving bodies in the data.
  Body-A motion done on agent/Joint8: `World::uberSectionBody` / `uberJointPose` pose body B by body A's pose × its
  joint, live fixed joints follow body A (also freed: `ms_ani_tentacle_gearr`), freed riders hang about the carried
  pivot and leave with the freed body's velocity.
- **OU-PZL-014:** not reproduced at HEAD: 6d6c9eb (2026-11-10) added the squib's Wine-measured knock on its parent.
  Joint8 found the ring (hinge 0 on the leaf) stayed in the doorway while the leaf slid and was hinged 36 off; fixed
  with OU-PHY-001. Open detail [U]: tru.exe's leaf leaves at (−870, 0, +364) tipping ≈ 4.6 rad/s, ours (−930, −26,
  +39) tipping 1.75: the door's squib record has elevation 0, so the lift is not from the record.

### Script VM (OU-SCR)

**OU-SCR-001 Fire-volume handlers stop at opcode 0x65**
- Evidence: `handler stopped: op 0x65` for `firetriggervolume` 505, `slowfiretriggervolume` 15, `deathfiretriggervolume` 15
  across all logs, 6 in the 0.51.0 logs [O]; AUDIO.md L527 "[U: which one]".
- Expected: tru.exe interpreter 0x57CE50 executes 0x65 (meaning unknown).
- Fix: decode 0x65 in the interpreter switch (cdcEngineDXHR ByteCodeEnum.h gives numbering, RESEARCH), implement in
  `src/script/Vm.cpp`. Test: proposed `ScriptRealData.FireVolumeRunsPastOp65` (l3 fire volume uid from logs).
- Repro result (a80bdb8): **Reproduced; failing instruction identified.** `--level 'pc-w\l3_main_bedroom.drm'` logs
  23 `op 0x65` stops (fire volumes 4492-4501, 4516-4518, …); l2_cliffclimb 4080 / 4081. Bytecode dump
  (`OU_PROGRESSION_DUMP="l3_main_bedroom:4492" ou_gates_tests --gtest_filter=ProgressionReal.DumpScripts`,
  `BugRepro/logs/dump_4492.txt`): 0x65 is CALLU; the stop is the constructor (type 6516 fn 4, state 0) at word 21:
  `PUSHL32 4, PUSHLA 8, PUSHSR 16, CALLU` with **no relocated function pointer** (`in.ptrs` empty → `Vm.cpp:551`
  fail), followed by a word the dump shows as `NOP 1793` (0x701) [I: the call's own operand, a builtin / array method
  on member +16, type 55]. The constructor's loop that builds the volume's player sensors never runs. RE next: the
  0x57CE50 case for CALLU with an unrelocated operand.
- Root cause (2026-11-16, [SCRIPT_NATIVES.md](../re/SCRIPT_NATIVES.md), commit 1f71837, runtime-verified under Wine): an
  odd unrelocated CALLU operand is a builtin reference `{u8 1, u8 DataType, u16 index}` that the link pass 0x57EE20
  rewrites. 0x701 = DYNARRAY builtin 0 `Size` (handler 0x574A30, count at +0x18); the patched word was seen as
  0x02269574 at runtime. Our parser decodes it as `NOP 1793`, so CALLU loses its operand. It is the only real
  operand-length mismatch with the exe over all 82 units.
- Fix (8daa2ee, on main since 0.53.0): operands decoded per 0x57EE20, builtin CALLU `{1, DataType, index}` → DYNARRAY
  builtins in the VM. Tests `ScriptParse.OperandWordsAndBuiltinCallU`, `ScriptRealData.FireVolumeConstructorBuildsSensors`.
- Re-verified 2026-11-19 (agent/ScriptVm6, main bcdf1db): no `op 0x65` stop in `ch01_bedroom_new_game` (with and without
  audio), in a 900-tick l3_main_bedroom run standing in 4494, or in 600-tick idle runs of l3_main_bedroom and
  l2_cliffclimb (the fire volumes' constructors 4080 / 4081 finish: no handler stops at all).
- Status · Origin: Fixed-verified · pre-existing.

**OU-SCR-002 Native `anchor` 1 unbound** — 0x7966F0, the vector between two anchors (0x4AB870); the VM has no vector
values (PROGRESSION.md L76-78). Logs: `triggervolume` 158, `triggerplane` 6 stops [O]; l7_helheim01 8945 (sound loudness
loop, not a gate). Fix: add a vec3 value kind to the VM. Status · Origin: Fixed on agent/ScriptVec7 · pre-existing.
- Narrowed 2026-11-19 (agent/ScriptVm6): the chain the fire triggervolumes (type 6515 vt 73, e.g. l3_main_bedroom 4529)
  run is decoded (SCRIPT_NATIVES.md rows `anchor` 4, `vector` 5/8/9/11, `rotation` 0/9).
- Fixed 2026-11-19 (agent/ScriptVec7). Current implementation before: no vector values, `NativeContext` returned one
  word, `anchor` / `vector` / `rotation` / `larabase` 21-22 unbound, so every handler reaching them stopped (survey: 41
  anchor 1, 2 larabase 21). What tru.exe does [O]: vectors / rotations are 16-byte `{x, y, z, w}` values inline in the
  frame or instance data whose address the interpreter passes to the thunk (`handler(receiver*, argc, args*, result*)`,
  0x57DE34); thunks 0x796540.. (get / set / copy / Set / length / scale), 0x796360 → 0x4A2F40 (rotation between two
  vectors: 0x49DA10 quaternion `(a × h, a · h)`, 0x49D450 → 0x49B440 euler), anchor 0x7966B0 / 0x7966F0 / 0x796770..
  (position, VectorTo, matrix rows of 0x49D500), larabase 0x79A110 / 0x79A130 → 0x777B10 / 0x777B70 (an `animation`
  value's pattern into Lara +0x488, INSTANCE_Post 0x1DA / 0x1DB). Also [O] the frame setup 0x57A850 constructs every
  local from its record (0x579770), so BOOL / INT / FLOAT locals start at their init (6515 fn 1's π lives there).
  Change: vectors live in four byte-offset slots (`Machine::valueAt`, `Machine::valueNative`), multi-word results
  (`NativeContext::words`), MOVEU 16 bytes, scalar local inits, anchor 0/1/3..5 and larabase 21/22 in `Script.cpp`,
  `TickInput::yaw` (Lara's rotation z = yaw + π/2: her model faces −Y). The fire volumes now pick the side as tru.exe:
  `ScriptRealData.FireVolumeFlinchSide` (volume 4529, Lara placed front / left / right / back / −63°; expected sides
  evaluated by hand from vt 73's formula; the posts are larabase 22 walk → 22 standing → 21 standing, patterns
  1501/1500/1499 and 1493/1494/1495 = Lara's `fire_walk_*` / `fire_standing_*`). Survey (`GameReal.ScriptSurvey`):
  132 → 89 handler stops (anchor 1 41 → 0, larabase 21 2 → 0, no new reason). ctest (private kwin, build-ScriptVec7):
  `-R 'Script|ProgressionReal|GameReal|StreamReal|^Game|selftest|replay'` 545 / 545, `-L protected` 41 / 41.
- Overlay played 2026-11-19 (agent/Flinch8). Before: `Runtime::playerAnimPosts` recorded the posts and nothing played
  them. What tru.exe does [O, docs/GAMEPLAY.md "Script flinch overlay"]: larabase 21 / 22 store `{kind 1, pattern}` at
  `+0x488` of her upper-body chart (`*(0xFADB58 + 0xF14)`, lara.drm player data `+0x12530`). That chart's root S0 lists
  0x1DA → T11 and 0x1DB → T10 (flag 4: also re-enters from S90), into S90 under `PSCCF_OnGround` and condition 0x21B
  (`0x765FD0`: property 0x645, type 43 = the locomotion chart's value, `0x785BF6`; set on idle / walk / run / turns /
  landings / crouch / slopes / wall hug / hurdles). S90's entry action 0x641 = `0x760530` plays the pattern on anim
  section 1 (`+0x320`). Its bone mask is `0x432780(obj, 1)`: globalaniminfo id 1 → lara.drm ObjectBase `+0xC0` mask 1
  = segments 14..106 at weight 1. S90 returns to S0 on clip done (0xC9) or 0x645 clear; S0's request 121 fades the
  section out over the pattern's `+0x1B` frames. Wine (l3_main_bedroom 4529, `tools/wine-ref` helper `section`): walking
  `fire_walk_right` (request 1499), standing `fire_standing_right` (1495); weight +0.25 / −0.2 per 30 Hz frame; mask runs
  `{0, 0..13} {1, 14..106} {0, 107..152}`; standing, the clip restarts after one frame in S0. Change: `World::tickFlinch`
  / `flinchLayer` (a second `AnimController` blended over segments 14..106 before the weapon layer), pattern fields
  `+0x19..+0x1B` decoded, saved as runtime `flinch`. Tests: `GameReal.FireVolumeFlinchPlaysUpperBodySection` (front /
  left / right standing, walking right; fade-in, restart, mask, fade-out), `StreamReal.SaveContinuesBitIdentically`
  (new l3_main_bedroom case). ctest (private kwin, build-Flinch8): `-R
  'ProgressionReal|GameReal|StreamReal|^Game|selftest|replay|Script|Anim|Save'` 595 / 595, `-L protected` 41 / 41.
- Still open: condition 0x1F5 (the swimming entry T24 / T23) [U], so she does not flinch in water; combat mode is taken
  as a weapon out [I]; general (tilted) euler order of rotation 0 / anchor 3..5 beyond yaw is [I]; a marker anchor stored
  in a member keeps only the object's uid [I].

**OU-SCR-003 Other unbound natives seen in user runs** — `instanceref` fn 10 (unit scripts, 64), `instance` fn 27
(l2_cliffclimb 37, l1_seafloor 10, l7_arctic_sea 5), `gamebase` fn 27 (`ms_uo_yacht` 7, `co_yacht` 1), `instance` fn 63
(`sh_ei_combat_crate_a/c` 12), `cinematic` fn 10 (3), `unitref` fn 1 (2) [O logs].
- Fixed in f9676fd (on main since 0.53.0): `instanceref` 10/13/16, `unitref` 1, `gamebase` 27, `instance` 27 and
  `cinematic` 10 for every receiver (SCRIPT_NATIVES.md; test `ScriptRealData.MissingNativesBound`,
  `GameReal.UnitScriptsStoreFloatProperty`).
- Open: `instance` 63 (0x798F50 → 0x7769A0 → 0x66A640 → 0x66A240 on the AI manager `*0xE922A8`). Narrowed 2026-11-19
  (agent/ScriptVec7, SCRIPT_NATIVES.md row): it queues `{unit record word, args[0]+4, 0, 0, 0}` at +0x25FE4
  (0x661050) and zeroes floats +0x78 / +0x7C / +0x80 of the kind-1 entry keyed (unit, id) in the 0x90-byte records at
  +0x1F50; what those records are and who reads the three floats is [U], so no binding (the survey never reaches the
  crates).
- Status · Origin: Partly fixed · pre-existing.

**OU-SCR-004 VM budget stops** — `firetriggervolume` 16, `dispatcher` 6 `handler stopped: budget` [O logs]; the per-resume
budget is ours (100000 instructions, `Vm.cpp:352`).
- Repro result (a80bdb8): **Reproduced deterministically; cause confirmed (fire volumes).** `ou-game --self-test
  tests/selftests/ch01_bedroom_new_game.json --self-test-trace --renderer opengl --no-audio --draw-last --sync-loads`
  (PASSED) logs `uid 4492 … budget` as she enters 4492 at tick 325 and `uid 4494 … budget` at tick 700; the same run
  **with audio** logs none. Bytecode (`dump_4492.txt`): state 1 (fn 8) is `while (true) { health damage (member +12
  = 300); log; instance fn 8; SSEQU sound fn 1 }` (`JMP −49`), whose only yield is the sound sequence; `Script.cpp:2309-2329`
  blocks only when the sound starts and reports busy, else falls through to the passive return → the loop spins until
  the budget kills the thread. Health is identical in both runs (1000 → 700 at 4492, → 400 at 4494: one hit per entry,
  none while she stays 3.2 s in 4494). User logs with audio show the same stop at times (e.g. `game-20261106-111105`
  tick 551, 4493) [I: the sound not starting / not busy]. tru.exe repeats the damage per sound length [I]. The
  `dispatcher` 2559 / 663 budget stops (l2_cliffclimb sharks) were not reproduced here.
- tru.exe (2026-11-19, agent/ScriptVm6, SCRIPT_NATIVES.md row `sound` 1 / 2): `sound` 1 / 2 (0x4AF230 / 0x4AF290) always
  return a SoundSequence latent (vftable 0x846F64), also when the sound did not start. SSEQU (0x57CE50 case 0x6A,
  0x57E016 → 0x574960) queues a non-null latent on the thread (0x576A30) and returns, so the thread always suspends.
  The thread update 0x57E970 runs the interpreter again only once IsActive (0x4ACC30: the sound instance lives) is false,
  checked from the next update. A sound that does not play therefore costs one tick per pass, never a spin.
- Wine (tools/wine-ref, new-game prologue, Lara held with `ref tpv` inside fire volume 4494 for 14 s,
  `openunderworld-work/w2/ScriptVm6/rec4494.txt`): health 1000 → 700 once, then no further hit. The burn sound
  (Assignment → Envelope → DTP 6559) sustains, so its sequence never ends while she stays. The earlier "repeats per
  sound length [I]" is wrong. Ours with audio matched (SoundPlayer::busy stays true); without audio it spun.
- Fix (agent/ScriptVm6): `Script.cpp` `sound` 1 / 2 always wait one tick, then while `SoundBusy` reports the sound
  playing (no clock: exactly one tick). `ou-game` also builds GameSound under `--no-audio`, with the device-less mixer
  (nothing is played; it is drained once a tick so voices end), so the burn clock runs on the sound's game-time length
  with or without audio. Streamed-out units' marker sounds are now cut, not released: with the drained mixer,
  `game_selftest_ch20_helheim02_passage` crashed in `SoundPlayer::apply` on l7_helheim01's freed marker plex.
- Tests: new `ScriptRealData.FireVolumeBurnWaitsOnItsSound` (4493: a silent sound gives one pass per tick and no
  failure; a busy sound gives one hit in 600 ticks). It fails before the fix with `handler stopped: budget`.
  `ctest -R 'Script|selftest|replay|Fire'` (203, every self-test and replay with `--no-audio`) and `-L protected` (41)
  pass. No self-test or replay changed result, and none needed re-recording. ch01 health is unchanged (700 / 400) and
  no self-test log has a `budget` or `op 0x65` stop. The `dispatcher` budget stops remain unreproduced.
- Status · Origin: Fixed-verified · pre-existing.

**OU-SCR-005 Fade to white unsupported** — level changes that fade to white (0x5DAB40) always fade black (`Script.cpp:2052`).
Status · Origin: Open · pre-existing.

### Combat, AI, vehicles, physics (OU-CMB, OU-VEH, OU-PHY)

**OU-CMB-001 Head shot below a full meter** — AUDIT_B L410 (incorrect). Now `if (c.adrenaline >= 1 && !c.adrenalineMode)
c.headShotReady = true` (`Combat.cpp:1206`). Status · Origin: Fixed-verified · pre-existing.

**OU-CMB-002 AI gaps** — no jump-over (NavPower links not decoded), flyers patrol straight (VolumeGraph), mercs never
sidestep, cover flags not derived, poacher rifle mesh not drawn, shark bite damage [I] (ROADMAP L57). Status · Origin:
Open · pre-existing.

**OU-VEH-001 Motorcycle unmeasured** — brake strength, slopes, coasting, top speed, pit jump, crash rules, water,
ramming, dismount position, respawn with the bike (ROADMAP L77, L96; Wine session in REFERENCE_COMPARISON
"Unmeasured"). Status · Origin: Open · pre-existing.

**OU-PHY-001 Joint model gaps** — fixed joints and body-A motion not simulated; uber body A is the world; types 5 / 7 on
moving bodies (ROADMAP L54, "Planned: Jolt" step 1; `Objects.cpp:694`, `:826`). Plates' Havok motor numbers [I].
Fixed joints: ecf03db. Body-A motion (agent/Joint8): body B's pose follows body A's (recursively), live fixed joints pose
body B on body A also once it is freed, freed riders hang about the pivot their body A carries; [I] body A's motion is
not fed into the Jolt joint body's dynamics and a fixed rider does not add its mass. Test:
`ProgressionReal.PrologueRingDoorPulledOffByTheGrapple` (ring hinged to the leaf, < 20 apart at the pivot).
Status · Origin: Fixed on agent/Joint8 · pre-existing.

**OU-PHY-002 Push speed fitted to one trace** — push blocks and Thor mechanisms: proxy mass 0.125 × object mass [I], law
fitted to one block trace (ROADMAP L87, REPORT §6, §8.4). Status · Origin: Open · pre-existing.

### Tests and docs (OU-TST, OU-DOC)

**OU-TST-001 No section end to end** — chapter legs per section 6/2/2/4/3/2/3/4/3/1/3/1/4/5/3/3/1/2/4/7/0, 14 parked
scenarios (ROADMAP L100, SELFTEST.md L523-551). Status · Origin: Open · pre-existing.

**OU-TST-002 Autopilot does not fight** — blocks ch11 (spiders) and ch18 (deck fights) legs (REPORT §7-8). Status · Origin:
Open · pre-existing (test-tool gap).

**OU-DOC-001 Chapter matrices stale**
- `docs/matrix/NN_*.md` keep rows `NOT IMPLEMENTED` for mechanics that landed in 0.42-0.46: chimney jumps (C08-05,
  C08-32, C12-10, C14-14, C14-27, C17-28, C17-35, C17-38 vs ROADMAP L79), jar treasures (C08-09, C08-20, C12-02,
  C12-05, C13-18, C14-18, C16-06 vs ROADMAP L80, 179 treasures), swan dive (C03-23, C05-10, ch05 "Swimming" vs L82),
  under-water dodge / jellyfish (C02-06, C02-08 vs L82), melee (C05-06, C07-33 vs L83), situational adrenaline (C03-35,
  C06-05 vs L81) [O grep]. REPORT §7 says rows changed only where a leg exercised them, so these are "not exercised
  in the chapter", not "missing".
- Raw grep over the 21 files: VERIFIED 6, IMPLEMENTED (tested) 134, IMPLEMENTED BUT UNVERIFIED 318, PARTIALLY 323,
  NOT IMPLEMENTED 67 (counts include secondary tables, so they differ from the headline 9 / 135 / 282 / 145 / 88 in
  `docs/underworld_chapter_gameplay_matrix.md`).
- Fix: relabel with a distinct status ("implemented, not exercised here").
- Fixed 2026-11-16 (DocDrift, f3bd9d0..34535f1): chapters 02-21 and the master matrix were refreshed row by row against
  the baseline's passing tests. Totals moved from 9/135/282/145/88 to 9/146/398/99/7 (+2 n/a) of 661 rows. The 7 rows
  still NOT IMPLEMENTED are C03-31, C10-06, C11-27, C12-08, C13-22, C15-02 and C16-21.
- Status · Origin: Fixed-verified · doc drift.

**OU-DOC-002 Contradictions between docs** [O]
- ROADMAP L46 "Mus! music sequencer not implemented" vs L64 music row Tested and AUDIO.md L401.
- REFERENCE_COMPARISON M17 status "differs: slide trigger" vs Differences #4 "done 2026-11-10".
- GAMEPLAY.md "Deviations" (L4387-4428): grapple "Not done: wall-running/rappel, pulling mounted objects", "Hair is
  rigid", "aiming from ledges / poles / ladders" missing, "Saves do not store weapons" vs ROADMAP L74 / L86 (grapple
  rappel, wall run, pulls), L31 (Verlet hair), L81 (one-handed hang shooting), L92 (loadout saved).
- ROADMAP L66 "enhanced water does not use the RT reflection path" vs RENDERING L203 (`water_rt.frag` replaces SSR).
- ROADMAP L54 "Lara does not push loose bodies" vs "Planned: Jolt" step 1 ("Lara's velocity-driven proxy shoves loose
  bodies").
- ROADMAP L95 "Natla cannot be hit (script vt 22)" vs REPORT §7 "Mjolnir hits do reach Natla's script".
- ROADMAP L55 lists both the whole-scene neighbour join cost and the per-unit residency numbers.
- `tests/selftests/ch07_cliffclimb_boat_swell.json` note and GAMEPLAY.md L1726-1727 "deck between z 78 and 154 (150–159
  on the waves alone)": a 20 s series from the same start measures her root on the deck −151 .. 420 (swell) and
  −25 .. 382 (waves alone) (OU-WAT-006 repro); the quoted ranges cover only the first seconds.
- Fix: one doc pass correcting each against the code.
- Mostly fixed 2026-11-16 (DocDrift, 550830a, b4c1dba, 4b58147): ROADMAP L46/L51/L54/L55/L66/L95/L102, GAMEPLAY
  Deviations, the ch07 deck range, RENDERING/SHADERS, TIMING, FUNCTIONS, and the Havok and CameraManager comments.
- Still open: REFERENCE_COMPARISON M17 vs Differences #4; the note in
  `tests/selftests/ch07_cliffclimb_boat_swell.json`.
- From VERIFICATION.md:
  - V049: 0x622418 is not an instruction start. The world-time-scale divide is at 0x7228E0. It is cited wrongly in
    `World.cpp:580`, `Combat.hpp:376`, `SCRIPTS.md:435` and `GAMEPLAY.md:4185`.
  - V074: the `Combat.hpp:32-33` comment says difficulty table +0x08 / +0x14 are unread. They are read, by 0x6E7960,
    0x702F5E and 0x75E600 (behaviour is already correct).
- tru_gog.csv rows 24 / 78 were fixed in 207426b.
- The World.cpp:877 comment edit in 207426b was itself wrong. CameraManager "+0x54" is both the field holding the
  committed mode and vtable slot 21 (docs/re/CAMERA.md). STRUCTURES.md and ENGINE_ARCHITECTURE.md were corrected on
  main. The World.cpp / Game.hpp comments are being corrected on branch agent/PlatformCam-2.
- Status · Origin: Open · doc drift.

**OU-TST-003 Loaders lack synthetic rejection tests** — listed per format in each `docs/formats/*.md` "Validation"
section (FormatDocs, 2026-11-16). Examples:
- UI: `ui::parseFont` and `ui::loadRaw` on broken input, and `decodeUtf8` edge cases.
- Bigfile index: too many records, unsorted hashes, nonzero compressed size.
- Textures: palette and unknown-format rejections.
- Meshes: corrupt index buffer, primgroup or vertex declaration.
- Animation: skeleton parent and segment-count checks.
- Anim graph: all of its rejections.
- Scripts: wrong version, unknown opcode, code starting with an operand.
- Levels: nav, portal and irradiance rejections.
- Text: malformed `.sch`.
- Video: corrupt movie file.

Effects read out-of-range values as zero; only `FxReal.EveryFxObjectParses` would catch a truncated object.
Status · Origin: Open · pre-existing.

### Save / load (OU-SAV)

**OU-SAV-001 Bedroom doors not restored exactly after quickload**
- Repro: self-test `tests/selftests/known_gaps/save_load_bedroom_doors.json` (SelfTestBrief, 4594f2c).
- Actual: `save.differs = ["dynamic 4526", "dynamic 4376"]` (`ma_uo_bedroom_door`). Their own motion is tiny
  (body.z −0.003 / −0.006), but the difference persists 2, 5 and 30 ticks after the load. Every other state part
  matches.
- Comparison: the same steps on the l1_seatop yacht and in l1_baldrs_tomb restore exactly.
- Suspected cause [I]: door hinge/joint body state is not restored bit-identically.
- Status · Origin: Reproduced · pre-existing.

## Triage

**Safely addressable next** (small, evidence in hand, no RE needed):
1. OU-WAT-007 dry-deck rule → restrict to buoyant loose bodies (`Player.cpp:3100`), with a test.
2. OU-WAT-006 swell clamp → decide (unclamped swell + hull rule) and rerun the `sea_swell` self-tests; pairs with 1.
3. OU-CAM-001 carry the camera focus with the platform frame Lara already rides (`World.cpp:955-978`).
4. OU-REN-002 dither the post output (no change to the LDR look).
5. OU-WAT-002 spray haze fade at the crater centre.
6. OU-DOC-001 / OU-DOC-002 doc corrections.
7. OU-PLY-003 shelf climb-out: chart state and shelf test are already decoded (GAMEPLAY.md L1926, L2608).
8. OU-GRP-001 throw clip lookup in the grapple DRM (data search, no exe work).

**Needs RE first** (tru.exe decode before code):
- OU-SCR-001 opcode 0x65; OU-SCR-002 vector values / `anchor` 1; OU-SCR-003 the listed natives; OU-PZL-009
  `modifiable` 7 semantics; OU-PZL-015 `multibody` 3 and the throne native; OU-SCR-005.
- OU-PLY-001 kind-1000 beam conditions; OU-PLY-002 lookahead 0x74BCF0; OU-PLY-004 ladder side jumps; OU-PLY-006
  query 96 reach; OU-PLY-010 free hang / beam drops; OU-SWM-001 port 0x755A00.
- OU-PZL-006 / -007 / -008 / -010 / -014 (joint and freed-section rules: C-JOINT), OU-PZL-011 (missing step
  collision), OU-GRP-004 (rope shortening), OU-CAM-002 (swim focus records), OU-CAM-003 VehicleCamera,
  OU-AUD-003, OU-REN-003 face materials, OU-REN-006.

**Needs Windows / Wine runtime measurement** (sessions listed in REFERENCE_COMPARISON "Unmeasured"):
- OU-GRP-002 swing, OU-GRP-005 rope weight on lever 6695, OU-SWM-001 / -002 turn rates, OU-PLY-005 / -007 /
  -008 / -009, OU-PZL-002 take-off spot, OU-VEH-001 bike, OU-PHY-002 push speeds, OU-AUD-002 reverb A/B,
  OU-REN-001 sky A/B against `orig_ch2`.

**Needs a hand-test / repro on current HEAD** (no code change until seen): OU-WAT-012 and OU-VK-002 (not reproduced
headlessly), OU-VK-004 (user's resolution), OU-WAT-008 (look judgement), OU-VK-001 (bisect only in a GPU-reset-safe
session). Reproduced in this pass and ready for a fix decision: OU-WAT-002, OU-WAT-006, OU-CAM-001, OU-SCR-004
(OU-REN-009 fixed, OU-PLY-012 partly fixed on agent/Depen6).

## Reproduction log

Pass of 2026-11-16 on `main` a80bdb8 (agent BugRepro). Binaries: `build-AuditBaseline` (read-only). Scratch:
`openunderworld-work/re-baseline/BugRepro/` (`inner{1,2,3}.sh` = the exact runs, `sc/` scenarios and input scripts,
`logs/`, `rep/` self-test reports, `shots/` PNGs, `tex2/` scratch probes `tex2.cpp`, `ext.cpp`, `swell.cpp`).

- Session: `systemd-run --user --scope --unit=ou-bugrepro{1,2,3} -q -p MemoryMax=14G -- env -u DISPLAY dbus-run-session
  -- kwin_wayland --virtual --socket ou-bugrepro --width 1920 --height 1080 --exit-with-session ./innerN.sh`; env
  `OU_GAME_DIR`, `QT_QPA_PLATFORM=offscreen OU_DISABLE_UPDATES=1`, XDG dirs under the scratch dir (user saves untouched).
  Scopes stopped afterwards; the leaked `ksecretd` / `xdg-desktop-portal-kde` of each session went with its scope.
- Shots: `ou-game --game-dir … --renderer vulkan|opengl --width 1920 --height 1080 --no-vsync --no-overlay --no-audio
  --screenshot X.ppm …`; every gameplay-dependent shot uses `--input-script` (60 Hz fixed ticks, so `--frames N` = tick N
  and reruns give the same `sim` digest — checked: `cliff_enh_fft` twice, `aa658bbc448797bb`).
- Self-tests: `ou-game --self-test X.json --self-test-trace --renderer opengl --no-audio --no-overlay --draw-last
  --sync-loads --width 160 --height 120`; time series by steps of 0.25 s whose expectations always hold and record the
  probes' values in the report.
- Not run: `game_vulkan_rt_smoke`, anything in l1_kraken_room with RT.

| ID | Result | Key measurement |
|---|---|---|
| OU-WAT-002 | Reproduced | crater centre luma 236 vs 110 ring at entry + 0.2 s; camera-in-splash discs at + 0.4 s |
| OU-WAT-004 | Not reproduced | one global FFT sea state; foam share ≤ 0.11 % |
| OU-WAT-006 | Reproduced | played sea mean −70 vs tru.exe waves; −68.6 of it the −0.5 σ centring; clip active 2.6-3.8 % |
| z 108 / 250 gap | Explained | yacht heave phase: fixed-tick z −103.4 (FFT) vs 24.8 (procedural = Original) at tick 600 |
| OU-WAT-007 | Not reproduced off-hull | rule only reachable airborne between surface and top |
| OU-WAT-008 | Reproduced (haze) | shafts +16.4 mean luma, no beam structure |
| OU-WAT-012 | Not reproduced | RT vs SSR water luma within ±1 at four views |
| OU-REN-001 | Fixed 2026-11-19 | the unit's sky dome object (`sa_skydome_c`) was never placed; now drawn |
| OU-REN-006 | Reproduced (log) | 9 sections skipped at the bedroom start |
| OU-REN-009 (new) | Reproduced | texture 2 = fallback id of back/depth-buffer samplers (materials 8035, 10930, 9544) |
| OU-VK-002 | Not reproduced | AO difference aligned with geometry on the heaving deck |
| OU-VK-004 | Not reproduced | FG active with MangoHud, 600 generated frames, no OOM |
| OU-STR-001 | Not reproduced | stream scene ≤ 21.35 ms; after quickload 0.6 / 1.2 ms |
| OU-AUD-003 | Not reproducible | no runtime symptom |
| OU-CAM-001 | Reproduced | camera z flat for 5 s (swell) / 3.75 s (tru) while the deck heaves 79-158 / 125-247 |
| OU-CAM-002 | Not reproduced | tread camera moves 23-24 % of the bob in both sea models |
| OU-PLY-012 | Reproduced (idle) | embedded 35 units: idle 1.5 s, moved 0.0 |
| OU-SCR-001 | Reproduced | 23 stops in l3_main_bedroom; CALLU without function pointer, constructor word 21 |
| OU-SCR-004 | Reproduced | ch01 self-test, `--no-audio`: 4492 @ tick 325, 4494 @ tick 700; with audio: none |
