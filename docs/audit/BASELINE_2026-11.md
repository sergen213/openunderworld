# Build / test / performance baseline, v0.51.0 (2026-11-16)

Audit pass, no source changes. Commit `9034839` (tag `v0.51.0`, preserved as branch `baseline/pre-audit-2026-11-16`).
Every number below is **Observed** in this run unless marked [INFERENCE]. Raw logs, screenshots and the run script:
`openunderworld-work/re-baseline/Baseline/` (called `$S` below; not in the repo).

## 1. Build

```sh
cmake -S . -B build-AuditBaseline -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DOU_SANITIZE=OFF -DOU_BUILD_TESTS=ON -DOU_LTO=ON -DOU_FSR=ON
systemd-run --user --scope -q -p MemoryMax=12G -- ninja -C build-AuditBaseline -j10
```

Option set: the release path (`packaging/appimage/build-appimage.sh:57`) uses `Release -DOU_SANITIZE=OFF
-DOU_BUILD_TESTS=OFF` with the default `OU_LTO=ON`, `OU_FSR=ON`; this baseline differs only in RelWithDebInfo
(`-O2 -g -DNDEBUG`, IPO on) and tests ON. (`build-release/` has no top-level CMakeCache; the cache used for comparison
was `build-release/appimage/CMakeCache.txt`.) `tools/release.sh --test` instead runs ctest on a Debug+ASan/UBSan build.

| Item | Value |
|---|---|
| Configure | 2.0 s (`$S/configure.log`) |
| Build | 160 s wall, 394 ninja steps, rc 0, 32-thread host, `-j10` (`$S/build.log`) |
| Outputs | `ou-game` 182 MB (with debug info), `ou-viewer` 37 MB, `ou-launcher` 16 MB; ctest registers 998 tests |

## 2. Toolchain and dependencies

| Component | Version / source | Evidence |
|---|---|---|
| Compiler | GCC 16.2.1 20260810 (`/usr/bin/c++`) | configure.log, `gcc --version` |
| CMake / Ninja | 4.5.0 / 1.13.2 | `cmake --version`, `ninja --version` |
| C++ standard | C++23, required (`CMAKE_CXX_STANDARD 23`) | `CMakeLists.txt:4-5` |
| Warning flags | `-Wall -Wextra -Wpedantic` | `CMakeLists.txt:26` |
| SDL3 | 3.4.20 (system) | `pacman -Q sdl3`, `SDL3_DIR=/usr/lib/cmake/SDL3` |
| Jolt Physics | v5.6.0, **downloaded by FetchContent** (GitHub tarball, SHA-256 pinned), not vendored; `third_party/jolt/` holds only `LICENSE`; `CROSS_PLATFORM_DETERMINISTIC ON`, RTTI on | `CMakeLists.txt:377-389`, `_deps/ou_jolt-src/Jolt/Core/Core.h` (5.6.0) |
| glm | 1.0.3 | pacman |
| Vulkan headers / loader | 1.4.363 / 1.4.363 | pacman, configure.log |
| shaderc (glslc) | 2026.4.1 | configure.log |
| Qt6 (launcher) | 6.12.0 (qt6-base) | pacman, `Qt6_DIR=/usr/lib/cmake/Qt6` |
| FidelityFX | AMD FSR SDK 2.4.0 subset (FSR 3.1.6 upscaler + FG), vendored `third_party/fidelityfx-sdk`, commit 7539ae8b | `third_party/fidelityfx-sdk/README.md` |
| DXC (for FSR shaders) | no system dxc (`OU_DXC-NOTFOUND`); pinned DXC v1.9.2609 fetched into `_deps/ou_dxc-src` | CMakeCache, `_deps/` |
| CORTAO | v1.0.3 (MIT), commit 420b168d, vendored unmodified | `third_party/cortao/VENDORED.txt` |
| Other vendored | `third_party/{abyssal-garden,neuralspecocean,opengl-water-simulation}` (water references; not audited here) | `ls third_party` |
| GoogleTest | 1.18.0 | configure.log |
| zlib | zlib-ng-compat 2.3.3 (reports zlib 1.3.1) | pacman, configure.log |
| libsodium / libepoxy | 1.0.22 / 1.5.10 | configure.log |
| OpenSSL / nlohmann-json / curl | 4.1.0 / 3.12.0 / 8.23.0 | configure.log |
| FFmpeg | 9.0.2 (libavformat/libavcodec 63.1.102) | pacman, configure.log |
| Python (FSR shader tool) | 3.12.12 (`~/.local/bin/python3.12`) | configure.log |
| Mesa / RADV | 26.2.6; KWin 6.8.3 (headless compositor) | pacman |

## 3. Warnings

275 `warning:` lines, 268 unique `file:line:col` (`$S/warnings_unique.txt`). **All in own code**; none from `_deps`
(Jolt, DXC) or `third_party`. No LTO (`lto-wrapper`) warnings.

| Flag | Unique | of which outside `tests/` |
|---|---:|---:|
| `-Wmissing-field-initializers` | 221 | 36 |
| `-Wdangling-else` | 40 | 1 (`src/collision/LevelCollision.cpp:117`) |
| `-Wformat=` (`%u` with `size_t`) | 3 | 0 (`tests/test_combat.cpp:1496,1600`, `tests/test_gates.cpp:3697`) |
| `-Wunused-parameter` | 2 | 0 (`tests/test_script.cpp:1039`) |
| `-Wunused-but-set-variable` | 1 | 0 (`tests/test_combat.cpp:2445`) |
| `-Wsign-compare` | 1 | 0 (`tests/test_save.cpp:1600`) |

By area: `tests/` 231, `src/` 31, `tools/` 6. Top files: `tests/test_game.cpp` 85, `tests/test_audio.cpp` 71,
`tests/test_fx.cpp` 30, `tests/test_save.cpp` 9, `src/app/game/main.cpp` 7, `src/game/Objects.cpp` 6,
`tools/ou-audio/main.cpp` 6. [INFERENCE] The `src/` warnings are designated-initializer omissions and one
dangling else; none looks like a correctness defect, but `LevelCollision.cpp:117` deserves a look.

## 4. ctest

Run inside a private KWin (`kwin_wayland --virtual --socket ou-baseline --width 1920 --height 1080
--exit-with-session ./inner.sh`, scope `ou-baseline-audit`, MemoryMax 14G), env `OU_GAME_DIR=<GOG install>`,
`QT_QPA_PLATFORM=offscreen OU_DISABLE_UPDATES=1` (as `tools/release.sh:78`), XDG dirs redirected to `$S/xdg`:

```sh
ctest --test-dir build-AuditBaseline -E game_vulkan_rt_smoke --output-on-failure -j4
```

| Result | Value |
|---|---|
| Selected | 997 of 998 (`game_vulkan_rt_smoke` excluded) |
| Passed | **980** |
| Failed | **0** ("100% tests passed out of 997") |
| Skipped (exit 77 / `GTEST_SKIP`) | 17 |
| Wall time | 664 s (ctest "Total Test time (real) = 663.43 sec") |
| Log | `$S/ctest.log` |

No failures, so there was nothing to rerun for flakiness. (A `ctest --rerun-failed` step in the script ran the
*whole* suite because no failure list existed, including the excluded RT smoke; it was killed after 103 s while in
`Sm3Real.EveryInstallShaderTranslates`, before reaching the RT smoke. Lesson: guard `--rerun-failed` with a
failure check.)

Skipped tests: 16 opt-in survey/dump tests — `GameReal.ScriptSurvey`, `AiReal.Survey`, `ProgressionReal.{GateSweep,
Survey, DumpScripts, CriticalPathInOrder, UberDump, ObjectFields, AnimNames, AnimGraphSurvey, AnimGraphRecords,
BuoyancySurvey, PushObjectSurvey, UberSections, RigidBodyMotors, DumpUber}` ([INFERENCE] gated by `OU_*` env vars
such as `OU_SCRIPT_SURVEY`, `OU_AI_SURVEY`, `OU_PROGRESSION_*`, `OU_UBER_DUMP`, `OU_RB_MOTORS` read in `tests/*.cpp`)
— and `vulkan_lavapipe` (CMakeCache `OU_LAVAPIPE_ICD-NOTFOUND`: no lavapipe ICD installed).

Composition (by name): 114 `game_selftest_*` passed (all of `tests/selftests/*.json`), 21 physics tests passed.
Slowest: `Sm3Real.EveryInstallShaderTranslates` 229 s, `StreamReal.SaveContinuesBitIdentically` 84 s,
`RealData.UnitLightsAllUnits` 63 s, `RealData.UberSectionAnimReferences` 52 s,
`game_replay_ballcourt_pole_lever_block` 44 s, `PhysicsRealData.AllObjectsParse` 42 s.

Matches the v0.51.0 CHANGELOG claim "ctest 997 passed (game_vulkan_rt_smoke excluded)", with the precision that 17
of the 997 are skips on this machine.

## 5. Self-tests (`ou-game --self-test`, docs/SELFTEST.md flags)

`--renderer opengl --no-audio --no-overlay --draw-last --sync-loads --width 160 --height 120`; reports in
`$S/selftest/<name>.report.json`.

| Scenario | Verdict | Steps | `sim` digest |
|---|---|---|---|
| `locomotion` | PASSED, exit 0 | 9/9 | `6657c32e07a15f34` |
| `ledge` | PASSED, exit 0 | 6/6 | `16d164107cf520e2` |
| `water_entry` | PASSED, exit 0 | 5/5 | `3eabdd64582a0bb9` |

## 6. Screenshots and frame times

`ou-game --level <drm> --width 1920 --height 1080 --no-vsync --no-overlay --no-audio --frames 600 --screenshot …`,
default config (empty `$XDG_CONFIG_HOME`), private KWin. Frame/tick numbers are the game's own `--frames` report
(after 30 warm-up frames, 569 frames). Read-back size is 1920x1052 (the virtual output's 1080 minus [INFERENCE]
window decoration). Without `--input-script` the tick count follows wall time (final tick 61-128 between runs), so
`sim` digests and Lara's idle phase differ run to run; image diffs below include that.

| Run (`$S/shots/*.png`, logs `$S/perf/*.stderr`) | AO / RT line printed | Frame mean / p95 / max ms | Tick mean ms |
|---|---|---|---|
| `bedroom_vk_default` (l3_main_bedroom, Vulkan) | AO CORTAO, RT shadows, 2 rays/px, 67 BLAS | 2.058 / 2.761 / 3.777 | 0.105 |
| `bedroom_vk_ao_off` | AO off, RT shadows | 1.770 / 2.425 / 2.804 | 0.101 |
| `bedroom_vk_ao_rt` (`--ao rt`) | AO ray traced, RT shadows | 3.289 / 5.254 / 7.322 | 0.103 |
| `bedroom_gl_default` (OpenGL) | AO CORTAO, RT unsupported -> shadow map | 2.378 / 2.560 / 3.246 | 0.104 |
| `bedroom_gl_ao_off` | AO off, shadow map | 2.355 / 2.587 / 2.946 | 0.131 |
| `cliff_vk_water_enhanced` (l2_cliffclimb, `--water enhanced`) | AO CORTAO, 6089 BLAS, 2565 instances; FFT ocean 128², 3 cascades | 6.305 / 7.973 / 9.677 | 0.301 |
| `cliff_vk_water_original` (`--water original`) | AO CORTAO, same RT scene, no FFT line | 5.946 / 6.593 / 7.362 | 0.186 |

`OU_VK_PRESENT_STATS=1` (bedroom, Vulkan default; `$S/perf/bedroom_vk_presentstats.stderr`): 264 presents/s,
GPU ms/frame: uploads+BLAS 0.64, TLAS 0.17, shadow map 0.12, scene 0.96, post 0.06; CORTAO (prepass + main + filter
+ temporal) 0.232 ms/frame.

Image diffs (`ou-imgdiff`, mean |Δ| /255, PSNR): VK vs GL default 0.200, 45.1 dB; VK vs GL AO-off 0.239, 40.9 dB;
VK CORTAO vs AO-off 0.403, 41.5 dB; VK RT-AO vs AO-off 0.746, 37.9 dB; GL CORTAO vs AO-off 0.319, 51.6 dB.
Viewed: both renderers show the lit corridor with Lara; the cliffclimb deck renders with enhanced (turquoise FFT sea)
and original water.

## 7. Protected systems, as observed in this run

| System | Status | Evidence |
|---|---|---|
| SDL3 init (video, Wayland) | OK in private KWin; every windowed ctest/self-test/shot ran | ctest, `$S/perf/rc.txt` (all rc 0) |
| Jolt init / physics | OK: 21 physics ctests pass; `physics` ms printed in every tick report | ctest.log, perf logs |
| Water, enhanced | OK: `water colour coral_lagoon`, `FFT ocean: 128^2 grid, 3 cascades (240 / 28 / 11.3 m)`; tick cost 0.30 vs 0.19 ms original [INFERENCE: CPU swell in gameplay surface, per 0.50.0 notes]; Lara rests at z 108 vs 250 with original waves after the same run (gameplay surface differs by water mode, as designed in 0.50.0) | `cliff_vk_water_*.stderr` |
| Water, original | OK, renders | `cliff_vk_water_original.png` |
| AO CORTAO (default) | Active by default in Vulkan and OpenGL | `AO CORTAO` lines |
| AO RT (`--ao rt`) | Active on Vulkan (`AO ray traced`), +1.2 ms mean on bedroom | `bedroom_vk_ao_rt.stderr` |
| AO off | OK both renderers | `*_ao_off.stderr` |
| Ray tracing on this GPU | Available: AMD Radeon RX 7900 XTX (RADV NAVI31), Mesa 26.2.6, Vulkan 1.4.354, driver 26.2.6; `VK_KHR_acceleration_structure` r13, `VK_KHR_ray_query` r1, `VK_KHR_ray_tracing_pipeline` r1, `VK_KHR_deferred_host_operations` r4; game reports `ray queries yes`, `Anti-Lag off` | `$S/vulkaninfo_summary.txt`, perf logs |
| OpenGL | 4.6 Core, Mesa 26.2.6 radeonsi; RT unsupported → shadow map (expected) | `bedroom_gl_*.stderr` |
| Vulkan validation | not active (Release-type build: `debug_utils (no validation layer)`) | perf logs |

## 8. Open points from this baseline

- l2_cliffclimb default spawn: the sky region at the top right is dark grey in both water modes
  (`$S/shots/cliff_vk_water_*.png`). Not diagnosed; compare against an original-game reference before calling it a bug.
- Every level load logs many `section(s) not used by the scene loader` and `skipped (no texcoords, no material
  textures)` warnings, and `script: uid … handler stopped: op 0x65` for nine `firetriggervolume`s in
  l3_main_bedroom — unimplemented-opcode/unused-data inventory for the bug/RE audit.
- `texture 2 not found in loaded DRMs` on l2_cliffclimb.
- 16 survey tests never run in default ctest; `vulkan_lavapipe` needs a lavapipe ICD to run at all.
- RT smoke (`game_vulkan_rt_smoke`) remains excluded (known real GPU hang); not exercised.

## 9. Cleanup

Scope `ou-baseline-audit` stopped (`inactive`). Remaining `ksecretd` / `xdg-desktop-portal-kde` processes all use
the desktop bus `/run/user/1000/bus`; none from this session remained.
