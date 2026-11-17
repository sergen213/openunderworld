# Bug and regression database

Central list of known defects, deviations from tru.exe and regressions in OpenUnderworld. Baseline: `main` 9034839
(tag v0.51.0, 2026-11-16, 997 ctest entries, `game_vulkan_rt_smoke` excluded). Audit pass: no engine code was changed.

Sources harvested: `docs/ROADMAP.md` "Known deviations" column (cited as `ROADMAP L<line>`), `docs/GAMEPLAY_AUDIT.md`,
`docs/audit/AUDIT_A.md` / `AUDIT_B.md` (rows classified incorrect / placeholder / needs refactor),
`docs/GAMEPLAY_DEVELOPMENT_REPORT.md` §7-8, `docs/REFERENCE_COMPARISON.md` "Differences to fix" and "Unmeasured",
`docs/SELFTEST.md` known-gap table and `tests/selftests/known_gaps/` (14 files), `docs/PROGRESSION.md` gate sweep,
`docs/RENDERING.md`, `docs/GAMEPLAY.md` "Deviations and open questions" (L4387), `docs/matrix/NN_*.md`,
`CHANGELOG.md`, markers in `src/`, GitHub issues, the user's run logs and coredumps.

## Legend

- **Status**: `Open` = present at 9034839; `Fixed-verified` = the fix is in HEAD and the cited code / doc / test shows
  it (this audit read the code, it did not re-run visuals or the game unless it says so); `Needs-repro` = reported or
  plausible but no evidence at HEAD either way.
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
| OU-WAT-002 | water render | Splash spray hides the dive crater centre | Open | new-feature defect (0.50.0) | C-SPLASH |
| OU-WAT-003 | water render | Whitecaps drawn as soft blobs / plates | Fixed-verified | pre-existing | — |
| OU-WAT-004 | water render | Foam / whitewater thresholds tuned on calm seas only | Needs-repro | pre-existing | C-WATERTUNE |
| OU-WAT-005 | water render | Interactive field rings only 1-3 cm (hard to see) | Fixed-verified | new-feature defect | — |
| OU-WAT-006 | water / gameplay | FFT swell clipped at the rest plane (troughs only, sea ~35 units low) | Open | new-feature defect (0.50.0) | C-SWELL |
| OU-WAT-007 | water / gameplay | Dry-deck rule applies to any moving solid, not only floating hulls | Open | new-feature defect (0.50.0) | C-SWELL |
| OU-WAT-008 | water render | Under-water sun shafts weak | Needs-repro | pre-existing | C-UWPORT |
| OU-WAT-009 | water render | Marine snow specks too large | Fixed-verified | pre-existing | C-UWPORT |
| OU-WAT-010 | water render | Shaft shadow map looked up along the unrefracted sun | Fixed-verified | pre-existing | C-UWPORT |
| OU-WAT-011 | water render | Water reflections miss off-screen geometry (SSR) | Open (limit) | pre-existing | — |
| OU-WAT-012 | water render / RT | RT reflection of a shadowed cliff overhang reads too dark | Needs-repro | pre-existing | — |
| OU-WAT-013 | water render | Geometry swell small outside the tessellated window (coarse TRU sea meshes) | Open (limit) | pre-existing | C-SWELL |
| OU-REN-001 | render | Dark sky near the chapter 2 cliff | Needs-repro | unknown | — |
| OU-REN-002 | render | HDR off by default: bright window highlights band | Open | pre-existing | — |
| OU-REN-003 | render / cine | `co_*` cutscene actor faces render green | Open | pre-existing | — |
| OU-REN-004 | render | Jointless uber sections drawn in bind pose | Open | pre-existing | — |
| OU-REN-005 | render | Under-water hair mode not switched; hair has no water response | Open | pre-existing | — |
| OU-REN-006 | render | Batches skipped "no texcoords, no material textures" (bedroom units) | Needs-repro | pre-existing | — |
| OU-REN-007 | render | Blended foliage receives the shadow map under RT; no roughness | Open | pre-existing | — |
| OU-REN-008 | render / FSR | Translated wind vertex animation has no motion vectors (FSR ghosting) | Open | pre-existing | — |
| OU-VK-001 | Vulkan RT | `game_vulkan_rt_smoke` hangs the GPU (l1_kraken_room) | Open | regression | — |
| OU-VK-002 | Vulkan AO | CORTAO temporal pass is camera-only (risk of halos on moving meshes) | Needs-repro | new-feature defect (0.51.0) | — |
| OU-VK-003 | Vulkan | AMD Anti-Lag crash a few frames into cutscenes / loading screen | Fixed-verified | regression (Anti-Lag support) | — |
| OU-VK-004 | Vulkan / FSR | `VK_ERROR_OUT_OF_DEVICE_MEMORY` then FSR frame generation context failed | Needs-repro | unknown | — |
| OU-VK-005 | Vulkan / FSR | FSR SDK validation messages (FG 3-component write, storage formats) | Open (upstream) | pre-existing | — |
| OU-STR-001 | streaming | 2-9 s `stream scene` stalls at level load | Needs-repro | unknown | — |
| OU-UI-001 | UI | Game font / button-glyph rendering missing | Fixed-verified | pre-existing | — |
| OU-UI-002 | UI | Chapter title / letterbox screens, confirmation dialogs, PDA sub-screens not drawn | Open | pre-existing | — |
| OU-UI-003 | UI / text | Game font has no Cyrillic; movie / voice subtitles English only | Open | pre-existing | — |
| OU-AUD-001 | audio | No Mus! sequencer / FMOD effects | Fixed-verified (Mus!), see OU-AUD-002 | pre-existing | — |
| OU-AUD-002 | audio | Reverb is a Freeverb tank, not FMOD's I3DL2; music bus not fed to reverb | Open | pre-existing | — |
| OU-AUD-003 | audio | One global music volume fade unidentified | Needs-repro | unknown | — |
| OU-AUD-004 | audio | Script sound wait threads, uber mover / physics-lever sounds missing | Open | pre-existing | C-VM |
| OU-AUD-005 | audio | Music loads synchronously (tracks start a few frames early) | Open | pre-existing | — |
| OU-CAM-001 | camera | Moving platforms do not carry the camera focus | Open | pre-existing | — |
| OU-CAM-002 | camera | Swim camera follows the wave bob exactly | Needs-repro | new-feature defect [I] | C-SWELL |
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
| OU-PLY-012 | collision | No depenetration pass | Needs-repro | pre-existing | — |
| OU-SWM-001 | swimming | Under-water turn rate unmeasured; tru.exe steering 0x755A00 not ported | Open | pre-existing | C-MEASURE |
| OU-SWM-002 | swimming | Surface swim velocity lags heading ~9° vs ~16° | Open | pre-existing | C-MEASURE |
| OU-SWM-003 | swimming | Scuba slot not read live; grenades under water unchanged | Open | pre-existing | — |
| OU-GRP-001 | grapple | Throw uses a stand-in clip; hook attaches at once | Open | pre-existing | — |
| OU-GRP-002 | grapple | Swing dynamics unmeasured (five failed Wine sessions) | Open | pre-existing | C-MEASURE |
| OU-GRP-003 | grapple | Jump into the gap released the rope; leap off a wall ignored the stick | Fixed-verified | pre-existing | — |
| OU-GRP-004 | grapple | ch10 roped leap to wall ring 2595 swings under the pool surface | Open | pre-existing | — |
| OU-GRP-005 | grapple | ch12 lever 6695 not held by Lara's weight on the rope | Open | pre-existing | C-JOINT |
| OU-PZL-001 | progression | ch01: no input route into the pocket north of the bedroom beams | Open | pre-existing | — |
| OU-PZL-002 | progression | ch04: jump to perch 30 lands on pillar 29 | Open | pre-existing | C-MEASURE |
| OU-PZL-003 | progression | ch05: west recess from the T junction is shut | Open | pre-existing | — |
| OU-PZL-004 | progression | ch06: hallway roll stops on a 45° floor before plane 1203 | Open | pre-existing | — |
| OU-PZL-005 | progression | ch06: bar 238 not reached from the top valve wheel | Open | pre-existing | C-CHART |
| OU-PZL-006 | progression | Freed slab bodies stay in place (ch09 shrine, ch08 statue floor) | Open | pre-existing | C-JOINT |
| OU-PZL-007 | progression | ch13: calendar push handle turns nothing after the mask is set | Open | pre-existing | C-JOINT |
| OU-PZL-008 | progression | ch13: grapple ring 4951 not offered from the east ledge | Open | pre-existing | — |
| OU-PZL-009 | progression | Switch `modifiable` 7 lock honoured only for one-way switches (ch15 exit switch) | Open | pre-existing | C-VM |
| OU-PZL-010 | progression | ch15: rune block 5609 has no joint record | Open | pre-existing | C-JOINT |
| OU-PZL-011 | progression | ch17: hammer chamber lower steps absent from collision | Open | pre-existing | — |
| OU-PZL-012 | progression | ch21: Natla platform jump falls short; fight sequence unplayable by input | Open | pre-existing | C-VM |
| OU-PZL-013 | progression | Motorcycle absent at level entries without a placing script | Open | pre-existing | C-VM |
| OU-PZL-014 | progression | Freed prologue ring-door leaf stays standing in its doorway | Open | pre-existing | C-JOINT |
| OU-PZL-015 | progression | ch08 `multibody` 3 unbound; ch14 thrones stop at an unbound native | Open | pre-existing | C-VM |
| OU-SCR-001 | script VM | Fire-volume handlers stop at opcode 0x65 | Open | pre-existing | C-VM |
| OU-SCR-002 | script VM | Native `anchor` 1 (vector values) unbound | Open | pre-existing | C-VM |
| OU-SCR-003 | script VM | Natives `instanceref` 10, `instance` 27 / 63, `gamebase` 27, `cinematic` 10, `unitref` 1 unbound | Open | pre-existing | C-VM |
| OU-SCR-004 | script VM | Handlers stop on the VM budget (`firetriggervolume`, `dispatcher`) | Needs-repro | pre-existing | C-VM |
| OU-SCR-005 | script VM | Level-change fade to white unsupported (always black) | Open | pre-existing | C-VM |
| OU-CMB-001 | combat | Head shot opened below a full adrenaline meter | Fixed-verified | pre-existing | — |
| OU-CMB-002 | AI | No enemy jump-over (NavPower links), flyers patrol straight, poacher rifle undrawn | Open | pre-existing | — |
| OU-VEH-001 | motorcycle | Brake strength, slopes, coasting, top speed, pit jump unmeasured | Open | pre-existing | C-MEASURE |
| OU-PHY-001 | physics | Fixed joints, body-A motion, joint types 5 / 7 on moving bodies not simulated | Open | pre-existing | C-JOINT |
| OU-PHY-002 | physics | Push-block / Thor-mechanism speed fitted to one block trace | Open | pre-existing | C-MEASURE |
| OU-TST-001 | tests | No walkthrough section plays end to end; 14 parked known-gap scenarios | Open | pre-existing | — |
| OU-TST-002 | tests | The self-test autopilot does not fight (blocks combat legs ch11 / ch18) | Open | pre-existing | — |
| OU-DOC-001 | docs | Chapter matrices mark implemented mechanics as NOT IMPLEMENTED | Open | doc drift | C-DOC |
| OU-DOC-002 | docs | Contradictions between ROADMAP rows / RENDERING / REFERENCE_COMPARISON / GAMEPLAY | Open | doc drift | C-DOC |

## Root-cause clusters

| Cluster | Shared cause | Bugs |
|---|---|---|
| C-SWELL | 0.50.0 made the drawn FFT swell part of the gameplay water surface but kept the render's `min(…, 0)` crest clamp (`src/render/shaders/water_surface.glsl:87-91`, `src/game/World.cpp:1710-1718`); decks that now sit under the volume top got a blanket dry-deck rule instead of a hull model; the camera focus follows Lara's bone 108 on that surface | OU-WAT-006, OU-WAT-007, OU-WAT-013, OU-CAM-002 |
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
- Status · Origin: Open · new-feature defect (0.50.0 PBF splash).

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
- Status · Origin: Needs-repro · pre-existing.

**OU-WAT-005 Interactive field rings only 1-3 cm**
- Actual (0.50.0): rings 2-4 cm, no rebound, swimmer acted as a wall (CHANGELOG 0.50.1).
- Fix landed (0.50.1): crater ~0.5 m, rebound ~20 cm at 1/3 s, rings up to 10 cm; field clipping removed
  (RENDERING L656). Status · Origin: Fixed-verified (changelog + RENDERING) · new-feature defect.

**OU-WAT-006 FFT swell clipped at the rest plane**
- Repro: `--chapter 1` or `--chapter 2` with Enhanced water + FFT ocean; compare drawn sea level with the level's
  water volume top / tru.exe wave height.
- Expected: tru.exe's sea is its own sine waves only, `Σ h(sin(k·x + φ) − 1)` (0x69DA80, GAMEPLAY / World.cpp
  `waveHeight`) — never above rest, mean below it. Whether a crest bound applies to an added swell is unknown (the
  FFT swell is ours).
- Actual: `ouBaseHeight` returns `min(h + ouOceanHeight(p) − 0.5·kM·w.ocean[0].w, 0.0)` and the gameplay surface
  `std::min(h + gSeaSwell(…), 0)`: crests are cut flat at the rest plane, the visible sea is all troughs and its mean
  sits roughly half a swell amplitude (~35 units on l1 / l2 [memory note, not re-measured]) below the volume top.
- Files: `src/render/shaders/water_surface.glsl:83-91`, `src/render/shaders/water.vert:51`, `src/game/World.cpp:1710-1718`,
  `src/render/Ocean.*` (`render::ocean::Swell`).
- Cause (confirmed by code): the clamp keeps yacht decks above the drawn sea; with the swell in the gameplay surface
  (c033ab5, 2026-11-15) buoyancy, swimming and the under-water test all see the lowered, crest-less sea.
- Fix: centre the swell on the rest plane (no clamp) and keep decks dry by the hull model (OU-WAT-007) instead; or
  clamp only the render inside hull footprints. Needs a decision with the yacht buoyancy tests.
- Test: `game_selftest_*` water scenarios with `"sea_swell": true`; proposed `World.SwellMeanAtRestPlane`.
- Status · Origin: Open · new-feature defect (render clamp 0.4x; gameplay coupling 0.50.0, c033ab5).

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
- Status · Origin: Open · new-feature defect (0.50.0).

**OU-WAT-008 Under-water sun shafts weak**
- Repro: under water, sun above the horizon, Enhanced water, `light_shafts` on (l1 sea, l2_cliffclimb).
- Expected: Enhanced addition (tru.exe has its own light rays drawable, 0x6D8C20, RENDERING L554).
- Actual: reported weak before 0.49.0; since 0.49.0 the shafts are abyssal-garden's half-res march (RENDERING L897-915,
  `min(acc·…·0.54, 20)`, mist term 0 because no density volume exists here).
- Cause (suspected): the demo's mist term `1 + 1.2 mist²` is dropped (mist = 0), so shafts get the demo's open-water
  minimum only.
- Fix: decide after a hand-test at HEAD; if still weak, feed a mist estimate (depth / near rock) instead of 0.
- Test: none. Status · Origin: Needs-repro · pre-existing.

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
- Status · Origin: Needs-repro · pre-existing.

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
- Status · Origin: Needs-repro · unknown.

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
- Status · Origin: Needs-repro · pre-existing.

**OU-REN-007 RT: blended foliage still receives the shadow map; no roughness** — ROADMAP L39. Status · Origin: Open ·
pre-existing.

**OU-REN-008 Wind vertex animation has no motion vectors** — FSR ghosting on foliage (RENDERING L1221). Status · Origin:
Open · pre-existing.

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
- Status · Origin: Needs-repro · new-feature defect risk (0.51.0).

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
- Fix: log heap budgets (`VK_EXT_memory_budget`) at FG creation. Status · Origin: Needs-repro · unknown.

**OU-VK-005 FSR SDK validation messages** — FG writes a 3-component texel to RGBA8
(`VUID-vkCmdDispatch-OpImageWrite-08795`), upscaler storage formats `Rg32f` / `R32f` over R16G16 / R16 images
(RENDERING L1222-1226). Status · Origin: Open (upstream SDK) · pre-existing.

**OU-STR-001 Multi-second `stream scene` at level load**
- Evidence: tick 14 `stream scene` 1990 / 3284 / 6793 / 3696 / 2646 / 8953 / 3286 ms in five logs 2026-11-06..10 [O];
  none above 1 s in 2026-11-15/16 logs.
- Note: ROADMAP L55 still lists the neighbour join rebuilding the whole render scene (~53 + 12 + 12 ms) next to the
  newer per-unit residency (4 + 2 + 2.5 ms).
- Status · Origin: Needs-repro (possibly fixed by resident units) · unknown.

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
- Status · Origin: Needs-repro / RE · unknown.

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
- Status · Origin: Open · pre-existing (27a5d8e, 2026-10-31).

**OU-CAM-002 Swim camera follows the wave bob exactly**
- Repro: `--chapter 1` surface swim / tread on the swell (Enhanced + FFT).
- Expected: unknown; tru.exe's surface-swim camera traces exist (`camfollow-wine/`, GAMEPLAY.md camera section)
  but its focus record dead zones for mode 19 / surface swim were not compared under swell.
- Actual [I]: focus goal = bone 108 through the mode's focus record; an axis with zone ≤ 1e-3 is followed exactly
  (`World.cpp:971`), so a zero vertical zone copies her bob.
- Fix: read the swim modes' focus records (settings +0x50) and check the z dead zone; if tru.exe follows exactly,
  the bob comes from our larger swell (C-SWELL).
- Status · Origin: Needs-repro · new-feature defect [I] (swell in gameplay, 0.50.0).

**OU-CAM-003 Motorcycle camera placeholder** — `kDistance = bike ? 1711` [I] (`World.cpp:993`), swing 1002 [I];
VehicleCamera not decoded (AUDIT_A L361). Status · Origin: Open · pre-existing.

**OU-CAM-004 Reveal cameras** — blend in / out ours, key event 7 [U], gamebase time scales not applied (ROADMAP L51).
Status · Origin: Open · pre-existing.

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
(GAMEPLAY.md L4426). Needs a repro of a stuck-in-geometry case. Status · Origin: Needs-repro · pre-existing.

### Swimming (OU-SWM)

**OU-SWM-001 Under-water turn rate unmeasured** — tru.exe steering function 0x755A00 (properties 0x200..0x20F, speed table
512..519) not ported [U] (GAMEPLAY.md L1886-1887, ROADMAP L82); turn / pitch rates are an unmeasured row (M29 / M85,
REFERENCE_COMPARISON "Unmeasured": l2_cliffclimb sea session). Test `Game.SwimFollowsTheCameraPitchUnderWater`.
Status · Origin: Open · pre-existing.

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
| OU-PZL-008 | l4_temple east ledge | ring 4951 offered; statue head topples | ring not offered; head mover unknown | `known_gaps/ch13_temple_block_topple` |
| OU-PZL-009 | l4_ballcourt exit switch 5469, socket empty | locked until keyhole 6501 (modifiable 7 (2)) [O bytecode] | switch usable; ours honours modifiable 7 only for `ar_ei_switch_oneway` | `known_gaps/ch15_ballcourt_exit_switch` |
| OU-PZL-010 | l4_pool rune block 5609 | dragged | no joint record → nothing drags it | `known_gaps/ch15_pool_rune_block` |
| OU-PZL-011 | l5_valaskjalf checkpoint 7067 | reach volume 6866 | lower 790 of the central steps absent from collision / objects [U]; edge 1542 is clearance class 1 (no pull-up, tru.exe 0x72BAE0) | `known_gaps/ch17_hammer_chamber` |
| OU-PZL-012 | l7_yggdrasil_end entry walkway | jump onto a passing stone (rig 8389); towers / mounts | ledge 1240 out, 540 up: falls short; fight unplayed by input. ROADMAP L95 says Natla cannot be hit (vt 22), REPORT §7 says Mjolnir hits reach her script (OU-DOC-002) | `known_gaps/ch21_platform_ride`, `natla_platform_ride`; `ProgressionReal.NatlaFightByInput` |
| OU-PZL-013 | level entries with a bike but no placing script (ch14 et al.) | bike present | placed only where a level script places it (REPORT §8.1) | ch14 legs |
| OU-PZL-014 | prologue ring door pulled by grapple | leaf falls away [U] | freed leaf stays standing (CHANGELOG L260, ROADMAP L89) | `ProgressionReal.PrologueRingDoorPulledOffByTheGrapple` |
| OU-PZL-015 | ch08 Bhogavati statue / lift; ch14 thrones | statue floor, lift drop, thrones run | `multibody` 3 unbound; thrones stop at an unbound native (REPORT §7) | ch08 / ch14 legs |

All: Status Open · pre-existing.

### Script VM (OU-SCR)

**OU-SCR-001 Fire-volume handlers stop at opcode 0x65**
- Evidence: `handler stopped: op 0x65` for `firetriggervolume` 505, `slowfiretriggervolume` 15, `deathfiretriggervolume` 15
  across all logs, 6 in the 0.51.0 logs [O]; AUDIO.md L527 "[U: which one]".
- Expected: tru.exe interpreter 0x57CE50 executes 0x65 (meaning unknown).
- Fix: decode 0x65 in the interpreter switch (cdcEngineDXHR ByteCodeEnum.h gives numbering, RESEARCH), implement in
  `src/script/Vm.cpp`. Test: proposed `ScriptRealData.FireVolumeRunsPastOp65` (l3 fire volume uid from logs).
- Status · Origin: Open · pre-existing.

**OU-SCR-002 Native `anchor` 1 unbound** — 0x7966F0, the vector between two anchors (0x4AB870); the VM has no vector
values (PROGRESSION.md L76-78). Logs: `triggervolume` 158, `triggerplane` 6 stops [O]; l7_helheim01 8945 (sound loudness
loop, not a gate). Fix: add a vec3 value kind to the VM. Status · Origin: Open · pre-existing.

**OU-SCR-003 Other unbound natives seen in user runs** — `instanceref` fn 10 (unit scripts, 64), `instance` fn 27
(l2_cliffclimb 37, l1_seafloor 10, l7_arctic_sea 5), `gamebase` fn 27 (`ms_uo_yacht` 7, `co_yacht` 1), `instance` fn 63
(`sh_ei_combat_crate_a/c` 12), `cinematic` fn 10 (3), `unitref` fn 1 (2) [O logs]. Effect per native unknown; the yacht's
`gamebase` 27 stop is in the 0.50.0 yacht work area. Fix: bind from the native tables (docs/formats/SCRIPTS.md method).
Status · Origin: Open · pre-existing.

**OU-SCR-004 VM budget stops** — `firetriggervolume` 16, `dispatcher` 6 `handler stopped: budget` [O logs]; the per-resume
budget is ours (ROADMAP L95 "the VM budget per resume"). Needs a repro to tell an infinite loop (missing wait
semantics) from a long legitimate handler. Status · Origin: Needs-repro · pre-existing.

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
Status · Origin: Open · pre-existing.

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
- Fix: relabel with a distinct status ("implemented, not exercised here"). Status · Origin: Open · doc drift.

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
- Fix: one doc pass correcting each against the code. Status · Origin: Open · doc drift.

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

**Needs a hand-test / repro on current HEAD** (no code change until seen): OU-WAT-004, OU-WAT-008, OU-WAT-012,
OU-VK-002, OU-VK-004, OU-STR-001, OU-SCR-004, OU-PLY-012, and OU-VK-001 (bisect only in a GPU-reset-safe session).
