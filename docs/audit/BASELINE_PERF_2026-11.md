# Performance baseline: fixed benchmark scenarios (2026-11)

Status: **tooling validated, measurements pending an idle machine.** The validation runs below were taken while
parallel builds and Wine sessions were running on the same host, so their timings, CPU % and GPU busy % are not a
baseline. What they do show (Observed): the scenarios are deterministic (identical `sim` digests and final ticks
between runs, cold and warm, and across renderers and AO modes on the same level), and every column is filled.
The final tables (§5) are to be filled from one `tools/bench/run.sh --runs 2` on an idle machine.

Items: W10-2 (fixed scenarios, deterministic tick count), W10-1 (RSS, VRAM, CPU %, GPU busy %, level-load time,
cold/warm pipeline compile, physics ms, frame-pacing histogram) from `docs/audit/BRIEF_COMPLIANCE.md` §16.
Earlier numbers: `docs/audit/BASELINE_2026-11.md` §6 (no input script, so tick counts varied run to run).

## 1. One command

```sh
OU_GAME_DIR="/mnt/1st4TB/Tomb Raider Underworld" tools/bench/run.sh --runs 2 [--out DIR] [--ou-game PATH] [--only a,b] [--no-cold]
```

- Starts a private KWin (`kwin_wayland --virtual --socket ou-bench-<pid> --width 1920 --height 1080
  --exit-with-session`, own `dbus-run-session`) in a `systemd-run --user --scope -p MemoryMax=14G`, runs
  `tools/bench/bench.py` as KWin's session child, stops the scope afterwards. The desktop KWin is never touched.
- Output (default `../openunderworld-work/bench/<date-time>/`): `bench.csv` (one row per run × scenario × cache
  state), `bench.log`, and per run `run<N>/<scenario>_<cold|warm>/` with `cmd.txt` (exact ou-game command line),
  `stderr.log` (each line prefixed with seconds since spawn), `frames.csv` (per-frame dump), `samples.json`
  (100 ms resource samples) and `final.ppm` (last frame).
- At the end it prints the reproducibility check (`bench.py --compare bench.csv`): exits 1 if any scenario's
  `sim` digest or final tick differs between runs or a run failed; flags frame-mean spreads above `--noise`
  (default 5 %).
- `ou-game` defaults to `build-Bench/ou-game`. It needs `OU_FRAME_TIMES` support (commit `c7ca030`, after
  v0.51.0); an older binary still runs but leaves the p50/p99/histogram/startup columns empty.

Build used for the validation runs (release option set, RelWithDebInfo + LTO as in BASELINE_2026-11 §1):

```sh
cmake -S . -B build-Bench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOU_SANITIZE=OFF -DOU_BUILD_TESTS=ON -DOU_LTO=ON -DOU_FSR=ON
systemd-run --user --scope -q -p MemoryMax=6G -- ninja -C build-Bench -j4 ou-game
```

## 2. Scenarios

Every run: `--input-script tools/bench/scripts/<script> --render-hz 60 --frames 900` (900 frames, each advancing
1/60 s: exactly 900 simulated ticks, 15 s), `--width 1920 --height 1080 --no-vsync --no-overlay --no-audio
--no-movies --no-cinematics --sync-loads`, and the graphics flags spelled out (`--preset enhanced --shaders original
--rt auto --upscaler off --frame-generation off --anti-lag off --msaa 4`, today's defaults) so a later default change
does not silently change a scenario. Empty `XDG_CONFIG_HOME` / `XDG_DATA_HOME` / `XDG_STATE_HOME` under the output
directory (the user's config and saves are not read or written).

| Scenario | Renderer | Level | Input script | Extra flags |
|---|---|---|---|---|
| `bedroom_vk_ao_off` | Vulkan | `l3_main_bedroom` (static interior) | `bedroom.txt`: pan, run 2 s, pan back | `--ao off` |
| `bedroom_vk_ao_cortao` | Vulkan | same | same | `--ao cortao` |
| `bedroom_vk_ao_rt` | Vulkan | same | same | `--ao rt` |
| `bedroom_gl_ao_cortao` | OpenGL | same | same | `--ao cortao` |
| `cliff_vk_water_enh` | Vulkan | `l2_cliffclimb` (yacht, open sea) | `cliff_sea.txt`: one full camera turn | `--ao cortao --water enhanced` |
| `cliff_vk_water_orig` | Vulkan | same | same | `--ao cortao --water original` |
| `jungle_vk` | Vulkan | `l2_junglehub` (dense foliage, 4754 RT instances) | `jungle.txt`: 1.5 camera turns, Lara stays | `--ao cortao` |
| `seafloor_vk_water_enh` | Vulkan | `l1_seafloor` (underwater start) | `seafloor.txt`: swim 4 s, one turn | `--ao cortao --water enhanced` |

`tools/bench/bench.py --list` prints the table from the source.

Each scenario runs twice per pass: **cold** (its cache directory deleted first) and then **warm** (same directory
reused). `XDG_CACHE_HOME` and `MESA_SHADER_CACHE_DIR` both point at that directory, so cold means no
`openunderworld/vk_pipeline_cache.bin`, no Mesa `mesa_shader_cache` and no `radv_builtin_shaders` (Observed: all
three are created there by a cold Vulkan run). The OS page cache is **not** dropped (that needs root), so "cold"
covers shader/pipeline compilation, not disk reads.

## 3. Columns (`bench.csv`)

| Column | Source |
|---|---|
| `final_tick`, `sim`, `pos` | ou-game's `final tick … pos … sim <digest>` line (`World::stateHash()`) |
| `load_s` | wall seconds from spawn to the `ou-game: tick 0 in unit` line (process start, bigfile, level load, streaming with `--sync-loads`) |
| `loop_start_ms` | `OU_FRAME_TIMES` header: `SDL_GetTicksNS()` at main-loop entry (SDL init → renderer and pipelines created, level loaded) |
| `first3_frames_ms` | sum of the first three drawn frames' wall times (lazily created pipelines, first uploads, first BLAS builds) |
| `frame_mean/p50/p95/p99/max_ms`, `frames` | per-frame wall times from `OU_FRAME_TIMES` after the 30 warm-up frames (same window as the game's own report, 869 frames) |
| `hist_*` | frame counts in buckets <2, 2–4, 4–8, 8–16.7, 16.7–33.3, ≥33.3 ms (frame pacing) |
| `tick_mean_ms`, `tick_p95_ms`, `script_ms`, `physics_ms`, `player_ms` | ou-game's `tick time …` report (`World::tick` CPU time and its script / Jolt physics / player shares, per tick) |
| `cpu_pct` | process utime+stime / wall over the run phase (from 1 s after `tick 0` to exit), 100 = one core |
| `rss_run_mb`, `rss_peak_mb` | `/proc/<pid>/status` VmRSS median over the run phase, VmHWM peak |
| `vram_proc_mb` | peak of `drm-memory-vram` in `/proc/<pid>/fdinfo/*` (amdgpu, deduplicated by `drm-client-id`): this process only |
| `vram_dev_delta_mb` | peak of `/sys/class/drm/card*/device/mem_info_vram_used` minus its value before spawn (device-wide, includes other processes' changes) |
| `gpu_busy_pct` | mean of amdgpu `gpu_busy_percent` samples over the run phase (device-wide) |
| `gpu_gfx_pct` | this process's `drm-engine-gfx` time / wall over the run phase |
| `ao_rt_line` | the AO / RT summary line (confirms the AO mode and RT state actually used) |

VRAM from `VK_EXT_memory_budget`: not used; ou-game does not log it (grep of `src/render/vk` finds no
`memory_budget`), and fdinfo gives the same per-process number without a source change.

## 4. Validation runs (busy machine — timings are NOT a baseline)

Host: AMD Radeon RX 7900 XTX (RADV NAVI31, Mesa 26.2.6), 32 threads; at the time several ninja builds and
Wine sessions of other agents were running. Build `build-Bench` at `c7ca030` plus the working tree of that moment.

Commands: `tools/bench/run.sh --out ../openunderworld-work/w2/Bench/val1 --runs 2` (all scenarios) and, after the
jungle script was changed (see below), `tools/bench/run.sh --out ../openunderworld-work/w2/Bench/val2 --runs 2
--only jungle_vk,bedroom_vk_ao_rt`. CSVs: `openunderworld-work/w2/Bench/val{1,2}/bench.csv` (not in the repo).
Rows below: val1, jungle from val2.

| Scenario | Cache | final tick | `sim` run 1 / run 2 | frame mean ms r1 / r2 | p99 ms r1 / r2 | loop_start ms r1 / r2 | first3 frames ms r1 / r2 | physics ms | RSS MB | VRAM (proc) MB |
|---|---|---|---|---|---|---|---|---|---|---|
| `bedroom_vk_ao_off` | cold | 900 / 900 | `453aca6a28764174` (same) | 1.954 / 6.041 | 2.792 / 7.082 | 2081 / 1940 | 1622 / 1703 | 0.017 / 0.025 | 1407 / 1408 | 1035 / 1036 |
| `bedroom_vk_ao_off` | warm | 900 / 900 | `453aca6a28764174` (same) | 1.942 / 6.629 | 2.745 / 11.213 | 1374 / 1611 | 516 / 537 | 0.018 / 0.026 | 1399 / 1402 | 1035 / 1035 |
| `bedroom_vk_ao_cortao` | cold | 900 / 900 | `453aca6a28764174` (same) | 2.137 / 5.422 | 3.035 / 9.482 | 1824 / 2020 | 1605 / 1705 | 0.017 / 0.025 | 1410 / 1405 | 1035 / 1036 |
| `bedroom_vk_ao_cortao` | warm | 900 / 900 | `453aca6a28764174` (same) | 2.139 / 2.220 | 3.031 / 3.124 | 1525 / 1456 | 616 / 514 | 0.018 / 0.018 | 1397 / 1394 | 1035 / 1035 |
| `bedroom_vk_ao_rt` | cold | 900 / 900 | `453aca6a28764174` (same) | 2.280 / 2.338 | 3.206 / 3.068 | 1708 / 1768 | 1569 / 1646 | 0.018 / 0.017 | 1409 / 1406 | 1035 / 1035 |
| `bedroom_vk_ao_rt` | warm | 900 / 900 | `453aca6a28764174` (same) | 2.298 / 2.311 | 3.231 / 2.986 | 1762 / 1395 | 529 / 518 | 0.019 / 0.018 | 1395 / 1390 | 1035 / 1035 |
| `bedroom_gl_ao_cortao` | cold | 900 / 900 | `453aca6a28764174` (same) | 2.662 / 2.559 | 3.325 / 3.218 | 1655 / 1441 | 1617 / 509 | 0.027 / 0.023 | 1206 / 1209 | 710 / 710 |
| `bedroom_gl_ao_cortao` | warm | 900 / 900 | `453aca6a28764174` (same) | 3.067 / 2.533 | 4.707 / 3.114 | 323 / 298 | 501 / 483 | 0.037 / 0.022 | 1025 / 1026 | 710 / 710 |
| `cliff_vk_water_enh` | cold | 900 / 900 | `0dcc13978847c00f` (same) | 5.966 / 5.390 | 7.343 / 6.866 | 1902 / 1802 | **109826** / 1767 | 0.108 / 0.071 | 1303 / 1739 | 1690 / 1690 |
| `cliff_vk_water_enh` | warm | 900 / 900 | `0dcc13978847c00f` (same) | 9.074 / 5.445 | 11.886 / 6.889 | 3096 / 1456 | 2296 / 737 | 0.098 / 0.071 | 1701 / 1700 | 1697 / 1697 |
| `cliff_vk_water_orig` | cold | 900 / 900 | `7a9cb3f7225d3dc3` (same) | 9.139 / 5.946 | 11.415 / 8.525 | 1938 / 1730 | 2308 / 2198 | 0.057 / 0.038 | 1715 / 1715 | 1680 / 1680 |
| `cliff_vk_water_orig` | warm | 900 / 900 | `7a9cb3f7225d3dc3` (same) | 9.054 / 6.024 | 12.150 / 9.548 | 1521 / 1418 | 770 / 752 | 0.059 / 0.038 | 1711 / 1709 | 1680 / 1685 |
| `jungle_vk` | cold | 900 / 900 | `e2c1f1ef85d6b913` (same) | 10.298 / 9.013 | 15.599 / 14.477 | 1583 / 1637 | 1908 / 1927 | 0.017 / 0.017 | 1783 / 1769 | 1950 / 1950 |
| `jungle_vk` | warm | 900 / 900 | `e2c1f1ef85d6b913` (same) | 9.588 / 9.288 | 15.370 / 15.063 | 1212 / 1203 | 861 / 817 | 0.013 / 0.017 | 1757 / 1751 | 1950 / 1946 |
| `seafloor_vk_water_enh` | cold | 900 / 900 | `6ebd5c2f851e4c7d` (same) | 9.155 / 8.228 | 11.268 / 11.511 | 1800 / 1705 | 1702 / 1664 | 0.105 / 0.100 | 1528 / 1525 | 1526 / 1461 |
| `seafloor_vk_water_enh` | warm | 900 / 900 | `6ebd5c2f851e4c7d` (same) | 9.152 / 8.149 | 11.272 / 10.724 | 1399 / 1379 | 690 / 677 | 0.113 / 0.097 | 1519 / 1518 | 1398 / 1398 |

What the validation establishes (Observed):

- **Determinism:** every scenario reached tick 900 with the same `sim` digest and the same final position in both
  runs, cold and warm. The digest is also independent of renderer and AO mode (all four bedroom variants:
  `453aca6a28764174`).
- **The check catches a mismatch:** in val1 the jungle input script was edited between run 1 and run 2 (run 1 ran
  Lara off a ledge to her death at tick 141 and respawned — final tick 579, `sim 055b353047417497`; run 2 used the
  fixed pan-only script, `e2c1f1ef85d6b913`). `--compare` reported `DIFFER` for both cache states and exited 1.
  Scenario check: apart from those two val1 run-1 jungle logs, no log has a `died` / `respawned` line; seafloor
  ends in `swimunder`.
- **Cold vs warm caches are separated:** Vulkan cold runs spend ~1.0–1.5 s more in the first three frames
  (`first3_frames_ms` ~1570–2310 cold vs ~510–860 warm) and ~0.1–0.7 s more before the main loop; OpenGL compiles
  before the loop (`loop_start_ms` ~1440–1660 cold vs ~300 warm). Final compile-time numbers wait for §5.
- **Timings under load are not reproducible:** frame-mean spreads up to 109 % between runs, and two stalls of
  ~110 s and ~136 s (cliff enhanced cold `first3_frames_ms`, cliff enhanced warm `load_s` 139.7 s). During the
  110 s stall the process used 9.8 % CPU and the GPU was 13.9 % busy, i.e. ou-game was waiting, not compiling
  ([INFERENCE] contention from the parallel jobs; not reproduced in run 2). Hence no baseline numbers from these runs.
- RSS and per-process VRAM agree between runs within ~1 % except the stalled cliff run (RSS 1303 vs 1739 MB) and
  seafloor cold VRAM (1526 vs 1461 MB); `physics_ms` varies with machine load (bedroom 0.017 vs 0.025 ms). All are
  re-taken in §5.

Unknown: `bedroom_gl_ao_cortao` cold run 2 had `first3_frames_ms` 509 (run 1: 1617) although its cache directory
was deleted first; `loop_start_ms` was cold in both (1655 / 1441 vs ~300 warm). Not investigated; check again in §5.

## 5. Baseline (idle machine) — pending

To be filled from `tools/bench/run.sh --runs 2` with no other builds, Wine sessions or games running:
per scenario the warm row of run 1 and run 2 (frame mean/p50/p95/p99/max, histogram, tick/physics ms, CPU %,
GPU busy %, RSS, VRAM), the cold−warm difference of `loop_start_ms` + `first3_frames_ms` (compile time), and the
noise between the two runs. Acceptance: `sim` identical (checked by the script), warm frame-mean spread ≤ 5 %.

## 6. Unknowns / caveats

- `load_s` includes process start and bigfile reads; with the page cache not dropped, the first run after boot or
  after memory pressure reads from disk. Observed in validation: one warm run spent 136 s before ou-game printed its
  first line (`cliff_vk_water_enh`, run 1); [INFERENCE] I/O or memory contention from the parallel jobs, not shader
  compilation (its cache was warm and the next scenario loaded in 4 s). Treat `load_s` outliers with this in mind.
- `gpu_busy_pct` and `vram_dev_delta_mb` are device-wide; the desktop session's KWin and anything else on the GPU
  add to them. `vram_proc_mb` / `gpu_gfx_pct` are per process.
  In val1 `gpu_gfx_pct` is empty in about half the rows (the first or last run-phase sample had no readable fdinfo);
  bench.py now uses the first and last samples that have one, and every val2 row has it (46–72 %).
- The `sim` digest is the same for every bedroom variant (AO off / CORTAO / RT, Vulkan / OpenGL): Observed, as
  expected for a renderer-independent simulation. It differs between water modes on `l2_cliffclimb`: with Enhanced
  water and the FFT ocean the swell is part of the gameplay water surface (CHANGELOG.md, 0.50.0 entry), so this is by
  design; final positions differ by ~25 units (Observed: `-34608.5 -14648.3 160.1` vs `-34592.1 -14665.6 160.6`).
