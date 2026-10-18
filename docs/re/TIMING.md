# Frame timing

GOG `tru.exe` VAs. Tags: **[OBS]** seen in our decompile, **[INF]** inference,
**[?]** unknown. The main loop structure is in [STARTUP.md](STARTUP.md#maing2-obs).
`gameTracker` is at 0xE7F088. Offsets below are relative to it.

## Clock [OBS]

| Function | What it does |
|---|---|
| `TIMER_Init` 0x597EE0 | `QueryPerformanceFrequency * 1e-6` → 0xCD9CA0; first sample → 0xCDB4E0 |
| `TIMER_SampleMicroseconds` 0x597E90 | `QPC * 1000000 / QPF` → u64 at 0xCDB520 |
| `TIMER_GetTimeMicroseconds` 0x597F10 | monotonic u64 µs: adds only **positive** deltas since the last sample to 0xCDB4D8 |
| `TIMER_GetTimeMilliseconds` 0x597F70 | the above / 1000 |
| `TIMER_MeasureTSCRate` 0x598090 | `rdtsc` across a QPC busy-wait (caller 0x576200) |

* The only caller of `TIMER_GetTimeMicroseconds` is `GAMELOOP_UpdateTime`.
* The main thread is pinned to CPU 0 with `SetThreadAffinityMask` (in `Win32_GameMain`). [INF] This guards against QPC/TSC skew between cores on 2008-era systems.

Other timer users. None of them feed the game clock:

| API | Users |
|---|---|
| `QueryPerformanceCounter` | the `TIMER_*` functions above; bfx library stopwatches 0x428600/0x428630/0x428660 and 0x42A890, 0x42E990, 0x42EA20, 0x42F900 (bfx code range, profiling [INF]); 0x4869F0/0x4885F0 (stopwatch-like accumulator [?]); CRT `__security_init_cookie` |
| `timeGetTime` | only 0x7CE8D7 (QPC with `timeGetTime` fallback) and 0x7CE92E, both inside the statically linked FMOD Ex code range [INF from RTTI `FMOD::*` around 0x7C0000–0x810000] |
| `GetTickCount` | 0x4825E0 (called every frame from `MainG2_PlatformUpdate`) [?], 0x77FE50/0x77FEA0/0x77FEF0/0x780160 [?], CRT cookie |
| `TIMER_GetTimeMilliseconds` | 0x483520, 0x4CCDB0, 0x4CCF20, 0x59A800, 0x789000 [?], and the level-load waits 0x5DE040/0x5DE480 (`Sleep(5)` polling loops, e.g. the `ShaderUsage_l%d.bin` load) |
| `Sleep` | load/wait loops (0x5D2870, 0x5D2AA0, 0x5DE040, 0x5DE3E0, 0x5DE480, 0x5DDED0); `PumpWindowMessages` 0x47AC30 `Sleep(100)` only while minimized/inactive; job threads; `CPUCount` |

**There is no frame limiter in the main loop** [OBS].

* `MainG2` 0x5E0190 runs `MAIN_ProcessMainState` → `GAMELOOP_Frame` back to back.
* Pacing comes only from `IDirect3DDevice9::Present`, through `D3DPRESENT_PARAMETERS.PresentationInterval`, set in `PCDeviceManager_BuildPresentParams` 0x522060:
  * `TWO` if the `PresentationIntervalTwo` registry value is set (30 fps on a 60 Hz display);
  * else `ONE` if `EnableVSync`;
  * else `IMMEDIATE` (uncapped).

## Per-frame order [OBS]

```
GAMELOOP_Frame 0x5DFFE0:
  GAMELOOP_Process(gameTracker)    // simulate with timeMult computed at the end of the previous frame
  STREAM_PollLoadQueue
  GAMELOOP_DrawFrame               // build + submit the frame (Present is inside the render path)
  GAMELOOP_UpdateTime              // measure, produce timeMult for the NEXT frame
  gameTracker+0x24++               // frame counter
```

The simulation of frame N uses a time step measured over frames before N. That is one frame of latency plus smoothing (see below).

## GAMELOOP_UpdateTime 0x5DA230 [OBS]

1. `now = TIMER_GetTimeMicroseconds()`; `raw = now - last` (last = u64 at 0xE7EE38).
2. **6-sample moving average:** `frameTimeUs = (raw + 5 previous raws) / 6`. The ring is 0xE7EE10[6], the index 0xE7EDC4. It is clamped to 1 000 000 and stored at +0xE8 (0xE7F170).
3. `frameRateLock` (+0x120, 0xE7F1A8) is clamped to 1..3.
4. Step selection:

| condition | dt (µs) | notes |
|---|---|---|
| `variableTimestep` (+0xC8, 0xE7F150) == 0 | lock 1: 33 333, 2: 50 000, 3: 16 667 | fixed step; `timeMult = timeScale * (+0x11C/4096) * dt / 33333.33` |
| variable, flag `+0x1C bit 2` (0xE7F0A4 & 4) | 33 333 | forced 30 Hz step [?] who sets the flag |
| variable, first frame after the "+0x2C bit 28" mode | lock-based 33 333 / 50 000 / 16 667 | one-shot [INF] |
| variable (normal) | `min(frameTimeUs, 66 666)` | **the shipping path** |

   In the variable cases `timeMultUnscaled` (+0x10C, 0xE7F194) = `dt / 33333.33` and
   `timeMult` (+0x100, 0xE7F188) = `timeMultUnscaled * timeScale` (+0x148, 0xE7F1D0, default 1.0).
5. Both values are then multiplied by two eased scale factors:
   * +0x118 (0xE7F1A0), set by 0x4A3DC0;
   * +0x14C (0xE7F1D4), set by 0x4A3DD0.

   `FUN_0078C4E0` eases them [INF: script/cinematic slow-motion]. A special mode (`+0x2C & 0x10000000`) forces `timeMult = 0.5`.
6. Derived fields:

| Field | Meaning |
|---|---|
| +0xD8 (0xE7F160) | step in ms, int (`dt/1000`, or `round(timeMult*33)` in fixed mode) |
| +0xCC (0xE7F154) | accumulates +0xD8: game time in ms |
| +0xD4 | accumulated in `GAMELOOP_Process` |
| +0xDC (0xE7F164) | `timeMult * 4096` (fixed point) |
| +0x110 (0xE7F198) | saved `timeMult` |

7. A third state: `mainTracker == 1` with a countdown at 0x8AB798 decremented by `timeMultUnscaled/30` per frame [?: role].

**`variableTimestep` is written only by `GAMELOOP_InitTimeFields` 0x5D3CF0, which sets it to 1.** [OBS: xrefs.]
`frameRateLock` is also written there (=1) and is referenced from the debug-menu table
entry "Frame Rate Lock" at 0x8AAD34 (stripped debug menu). [INF] So the shipping game always
runs the variable path. The fixed 30/20/60 Hz path is debug-only.

Consequences [OBS → INF]:

* One `timeMult` unit = one 30 Hz frame. The game was tuned around 30 fps, where `timeMult = 1`.
* At 60 fps `timeMult ≈ 0.5`, at 144 fps ≈ 0.21.
* Frames longer than 66.7 ms are clamped. Below 15 fps the game runs in slow motion.
* The 6-frame average means a single long frame is spread over the next 6 steps instead of being simulated once.

## Consumers [OBS]

* `gameTracker_timeMult` has 307 read references in 186 functions (whole-program decompile, `analyze.sh --decompile-all`).
* `timeMultUnscaled` appears in 8 functions.
* `GAMELOOP_Process` 0x5DFBE0 calls each subsystem **once per rendered frame** with the variable step:

| Callee | Argument | Notes |
|---|---|---|
| 0x6D7CB0, 0x696650, 0x6ADB70 | `timeMult * 0.03333333` (seconds) | 0x6ADB70 accumulates seconds and wraps at 345 600 [?] |
| 0x492B80 (cdc physics/multibody step, "Multibody Contact Cache Full!") | `timeMult` | see below |
| 0x4F3540 | | |
| 0x489C30 (bfx AI/navigation update → 0x42A890) | `timeMult * 0.033` (seconds; **0.033, not 1/30**: bfx time runs 1 % slow) | |
| 0x709D10 | `timeMult` | |

* Instances: `FUN_005B6DC0` (pre-update) and `FUN_005B3090` walk `g_instanceList` 0xD98D54.
  * Before each instance's process callback (instance+0x118) they reset `timeMult` from the saved +0x110 copy. The callback can rescale it per instance: 0x454550 multiplies by a factor, 0x45E100 sets 0.5, 0x752C50 clamps. This is per-object time dilation.
  * `timeMult*4096` is refreshed for each instance.
* `MainG2_PlatformUpdate` 0x47AFD0 gets `timeMultUnscaled`. Sound/stream updaters 0x5904D0/0x5954D0 receive `arg/30` seconds; 0x5790B0 receives `timeMult/30`.

Physics step 0x492B80 → 0x4F1900 → 0x4F0AB0 → 0x4F0800:

* `dt = min(timeMult, 2.0)`. That is a second clamp at 66.7 ms.
* If a frame had `timeMult > 1.1538` (> 38.5 ms), a recovery flag is set. It stays set until 90 `timeMult` units (3 s of game time) accumulate. While it is set, the step runs as **two half-steps**.
* Flag `+0x1C bit 8` scales physics dt by 0.05 (slow motion).
* Otherwise it is **one integration step per rendered frame with variable dt**. There is no fixed-rate substepping [OBS for this function; the solver internals of 0x4F0800 are not traced].

## Frame-rate dependence: what this means

**[INF]**

* Code that multiplies by `timeMult` is first-order frame-rate independent.
* Code that uses per-frame constants (exponential damping `v *= k`, counters, "every N frames" logic) is not.
* No function both references `timeMult` and calls a `pow` routine (0 of 186). So damping, where it exists, is either linear in `timeMult` (wrong for large/small steps) or per-frame.
* At high frame rates the physics/multibody step and animation blending get much smaller dt than the 30 Hz they were tuned for. Anything tuned at `timeMult ≈ 1` (fixed solver iterations, contact thresholds) can behave differently.

**[?]**

* The concrete gameplay symptoms reported by players at > 60 fps.
* Which of the 186 consumers are per-frame-tuned.

The next RE step is to classify the consumers. Grep `decomp_all.c` for `gameTracker_timeMult` and sort them into linear-scaled vs. constant-per-frame.

For OpenUnderworld [INF]:

* Running the simulation at a fixed 30 Hz (`timeMult = 1`, dt 33 333 µs) reproduces the tuned behaviour exactly.
* Rendering can interpolate between simulation states.
* The 6-frame averaging and the 66.7 ms clamp only need emulating if variable-step parity is wanted.
