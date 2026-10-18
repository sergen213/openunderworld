# Function naming: status and open hypotheses

## Applied names

`tools/ghidra/names/tru_gog.csv` is the source of truth for applied names. Each row is
`address,name,confidence,evidence`. `ApplyNames.java` writes the rows into the Ghidra
project on every `tools/ghidra/analyze.sh` run.

The current CSV has 75 high, 76 medium and 16 low rows. 144 functions and 23 data labels
were applied with 0 errors.

| Confidence | Meaning |
|---|---|
| high | our own decompile shows the behaviour (strings, imports, structure) |
| medium | a TRLAU-menu-hook TR8 address that is a function entry here, with a consistent decompile, or our own analysis with a plausible but unconfirmed role |
| low | role guessed from context |

The TRLAU-menu-hook TR8 addresses apply 1:1 to this GOG build (FileVersion 1.1, PE timestamp
2008-12-16):

* 50 of 54 are Ghidra function entries.
* The other 4 are valid prologues that Ghidra missed. `ApplyNames` creates them.

`ExportRtti` labels all 990 MSVC vftables as `<Class>::vftable`. Use `rtti.tsv` for the
class → vftable → slot map.

## Open hypotheses (not in the CSV yet)

Tags: **[OBS]** seen in decompile, **[INF]** inference, **[?]** unknown.

| Address | Hypothesis | Evidence | Conf. |
|---|---|---|---|
| 0x774550 | per-frame service A in `MainG2` (state 6 → 0x7743C0 with level data) [?] | called every `MainG2` iteration before `MAIN_ProcessMainState` [OBS] | low |
| 0x789220 | per-frame queue drain in `MainG2` (list at 0xFAF2D0) [?] | same [OBS] | low |
| 0x5DF930 | engine shutdown | called once after the `MainG2` loop; 66 callees [OBS] | medium |
| 0x5DAB40 | level-start / load-fade path of `MAIN_PlayGame` | taken when `gameTracker+0xB8` != 0; sets fog/clear colours, calls `GAMELOOP_UpdateTime` [OBS] | low |
| 0x5DAAB0, 0x5DAE20 | other main states (menu/load) with their own `GAMELOOP_UpdateTime` call | callers = `MAIN_ProcessMainState` [OBS]; 0x5DAE20 string `medsea_loop` | low |
| 0x5D3DF0 | `GAMELOOP_ResetLevelTime` | zeroes/sets game-time ms (+0xCC/+0xD0) from level data +0x19C; looks up object `"player"` [OBS] | medium |
| 0x5D9E70 | game-tracker init | only caller of `GAMELOOP_InitTimeFields`; called from `MainG2_InitEngine` [OBS] | medium |
| 0x4A3DC0 / 0x4A3DD0 | script natives setting the two eased time-dilation factors (+0x118 / +0x14C) | single store each, no direct callers [OBS] → function-pointer table [INF] | low |
| 0x78C4E0 | ease/approach toward a target over N frames | applied to both dilation factors in `GAMELOOP_UpdateTime` [OBS] | low |
| 0x6ADB70 | world clock (time of day?) in seconds, wraps at 345 600 | `+= timeMult/30`, wrap, then 0x6ADA00 [OBS] | low |
| 0x6D7CB0, 0x696650 | per-frame updaters taking seconds | `GAMELOOP_Process` passes `timeMult*0.03333333` [OBS] | — |
| 0x4F3540 | collision/physics pair list swap + qsort | double-buffered arrays swapped each frame [OBS] | low |
| 0x709D10 | player-related per-frame update with `timeMult` | reads player instance `0xE7F094` [OBS] | low |
| 0x5904D0, 0x5954D0 | sound / stream updaters | `MainG2_PlatformUpdate` passes `timeMultUnscaled/30` s [OBS] | low |
| 0x5790B0 | per-frame updater taking `timeMult/30` [?] | `MainG2_PlatformUpdate` [OBS] | — |
| 0x4825E0 | input/device poll using `GetTickCount` | called every frame from `MainG2_PlatformUpdate` [OBS] | low |
| 0x4869F0 / 0x4885F0 | QPC stopwatch + a profiler/sampler using it | QPC accumulate pattern [OBS]; role [?] | low |
| 0x472990 | `ArchiveFileSystem_SetSpecMask` (vftable slot 4) | stores this+8, the mask tested by `FindEntry` [OBS]; no direct callers | medium |
| 0x5033E0 | third `PCStreamDecl_CreateFromVertexDecl` caller (particles/immediate draw?) [?] | xref [OBS] | low |
| 0x8AB798 | countdown in seconds decremented in `GAMELOOP_UpdateTime` while playing; at ≤0 sets `gameTracker+0xBB = 10` | [OBS]; role [?] (timed sequence / game-over timer) | low |
| DRM section header +0x10 | language/spec mask | position matches other cdc games [INF]; not read in the parts of the loader traced so far | low |
| PCD9 +0x08/+0x0C/+0x14 | data size / unknown / unknown | [?] | — |

## Workflow

```
tools/ghidra/analyze.sh [--exports-only] [--decompile-all]   # applies names, writes exports
tools/ghidra/decompile.sh -o DIR 0x5da230 GAMELOOP_UpdateTime @list.txt
tools/ghidra/xrefs.sh 0xe7f188 QueryPerformanceCounter "str:Unable to open"
```

* Wrap these in `systemd-run --user --scope -q -p MemoryMax=6G -- …` and run only one Ghidra JVM at a time. The default heap is `GHIDRA_HEADLESS_MAXMEM=4G`.
* To promote a hypothesis: add the CSV row with its evidence, then re-run `analyze.sh --exports-only`.
