# tru.exe function database

A per-function inventory of the GOG `tru.exe` (sha256 `1fdbd345…1011`, see [EXECUTABLE.md](EXECUTABLE.md)) joined
with every tru.exe address the repository cites. It answers: which original functions has OpenUnderworld
already reconstructed (cited from code or tests), which are only documented, which are third-party library
code, and which uncited functions sit closest to what we already reimplement. Generated 2026-11-16 at
repo `9034839`/`5cda880` (v0.51.0) from the Ghidra 12.2 project in `openunderworld-work/ghidra/project`.

No decompiled code is stored here: the tables hold addresses, sizes, counts, names, RTTI class names,
library tags and `path:line` citations, plus one short string per priority row as a hint.

## Regenerating

```sh
# 1. Graph export (read-only on the project; one Ghidra JVM at a time, 4G heap, ~30 s):
W=../openunderworld-work; mkdir -p $W/re-baseline/FunctionDb
GHIDRA_HEADLESS_MAXMEM=4G /opt/ghidra/support/analyzeHeadless $W/ghidra/project tru -process tru.exe \
    -noanalysis -readOnly -scriptPath tools/ghidra/scripts -postScript ExportGraph.java $W/re-baseline/FunctionDb
# 2. Join (stdlib Python, ~2 s, deterministic: identical inputs give byte-identical outputs):
tools/re/function_db.py            # --work DIR (default ../openunderworld-work), --out DIR (default docs/re/db)
```

`tools/ghidra/scripts/ExportGraph.java` writes `functions_full.tsv` (entry, name, namespace, symbol source,
size, body ranges, thunk target, FID flag), `edges.tsv` (caller, callee, call|jump|thunk, site count),
`func_refs.tsv` (distinct data addresses, reads, writes, string refs, vftable refs, pointer refs to the
function), `data_symbols.tsv` and `blocks.tsv`. `function_db.py` also reads the older analyze.sh exports
`rtti.tsv` (990 vftables with slot lists) and `strings.tsv` (string → referencing functions), and
`tools/ghidra/names/tru_gog.csv`. The exports themselves stay in the work dir (they are exe-derived).

## Outputs (`docs/re/db/`)

| File | Rows | Content |
|---|---:|---|
| `functions.tsv` | 18 766 | entry, size, callers, callees (distinct, from `edges.tsv`), thunk, thunk_target, ghidra_name, applied_name (CSV), repo_name, subsystem, cited_by, cited_files, cite_scope, citations (first 3 `path:line`), rtti_class (classes whose vftable holds the entry), library (`family:evidence`), data_refs, string_refs, status |
| `citations.tsv` | 6 305 | address, kind (`func_entry`, `func_interior`, `vftable`, `string`, `data_symbol`, `data`), containing function, path, line, citing subsystem, name the citing text attaches |
| `priorities.tsv` | 200 | uncited non-library functions reachable from cited engine roots, ranked (top 100 below) |
| `rtti.tsv` | 990 | class, vftable, slot count, slots cited, vftable itself cited, bases |
| `summary.json` | | every number quoted in this file |

### Status

| Status | Rule |
|---|---|
| Library | direct library evidence (below), or uncited and inside a library's address run |
| Reimplemented-cited | cited from `src/` or `tests/` (a code comment or self-test note names the address or an instruction inside the function) |
| Named-only | cited only from docs/CHANGELOG, or named in `tru_gog.csv` / by a user-defined Ghidra symbol, with no code or test citation |
| Unknown | none of the above |

"Reimplemented-cited" means the repo code says it follows that function; it is not a claim that the
reimplementation is complete or faithful. A library function cited from code (e.g. a bfx NavPower
routine whose data we parse) keeps status Library; `cited_by` shows the citations.

`subsystem` is the weighted majority of the citing areas (src citations weigh 3, others 1;
`docs/GAMEPLAY.md`, self-test notes, chapter matrices map to `game`, `docs/RENDERING.md` to `render`,
`docs/formats/*` to `formats`, `docs/re/*` to `re`). For uncited functions, `~area` is the majority area of
cited callers and callees (one hop) — an [INFERENCE] hint only.

`repo_name` is a heuristic: the CamelCase/underscore/`::` identifier written immediately before or after the
address in the citing line (e.g. `` `INSTANCE_IntroduceInstance` (`0x5BCC30`) ``). It also picks up our own
type names written next to an address (`BikeTuning`, `GpuMesh`), so treat it as a pointer into the citing
text, not as the original function's name.

### Citation harvest rules

All `git ls-files` text under `src/ tests/ docs/ tools/` and `CHANGELOG.md` are scanned for `0x` + 6–8 hex
digits, Ghidra-style `FUN_/DAT_/LAB_/PTR_…_/switchD_xxxxxxxx`, and (docs only) bare `00XXXXXX`. A value counts
only if it lies in 0x401000..0x10D9FFF (`.text` through `.shad`; the brief said ..0x9FFFFF, but the game's
globals such as `gameTracker` 0xE7F088 live in `.data` up to 0x10D4FFF) and resolves to a function entry, an
instruction inside a function body (`func_interior`), a vftable, a string, a named data symbol, or other
data. Filters against unrelated constants:

- code files (`.cpp/.hpp/.glsl/.py/.sh/…`): only inside comments; JSON only on lines with `tru`, `note` or `[O`;
- values whose low 12 or 16 bits are zero (flags, page-aligned section values) or whose three low bytes are
  equal (colours such as 0x808080) are dropped;
- unnamed `.data`/`.rdata` addresses in code/JSON need `tru`, `global`, `DAT_` or `[O` on the line;
- bare `00XXXXXX` must hit a function entry, vftable or named symbol;
- skipped files: `tools/ghidra/names/` (names, not reconstruction evidence), `docs/re/BINARIES.md` and
  `tools/re/pe_inventory.py` (their PE tables hold RVAs/sizes that would mis-resolve as VAs), and this
  database's own outputs.

Result: 6 305 citations from 213 files: 1 874 distinct function entries, 57 interior addresses, 24
vftables, 23 named data symbols, 124 other data addresses (0 strings). [INFERENCE] Residual false positives
are possible for interior/data hits in prose; a spot check of `func_interior` and `data` hits found them to
be real instruction/global references (e.g. `src/fx/Fx.cpp` instruction addresses inside 0x540B80/0x543F70,
`docs/re/STARTUP.md` settings block 0x890388).

### Library classification

| Family | Functions | Bytes | Evidence |
|---|---:|---:|---|
| fmod (FMOD Ex, statically linked) | 1 232 | 277 078 | 51 `FMOD::` RTTI classes' slots; functions referencing `..\..\src\fmod_*.cpp` paths or FMOD error strings; address-run fill (0x7B…0x82) |
| crt (MSVC CRT/STL/EH) | 650 | 105 927 | 381 Ghidra Function-ID bookmarks, `FID_conflict:`/`_`/`__` names, `std::`/`type_info`/`exception` RTTI |
| bfx (NavPower/behaviour middleware, `bfx::` namespace) | 509 | 111 073 | 35 `bfx::` RTTI classes' slots, `bfx_` names; address-run fill at 0x401000..0x42xxxx |

Evidence counts: 381 FID, 411 RTTI slot, 149 string, 15 name/import, and 1 435 address-run fills. The fill
rule ([INFERENCE]): an uncited function whose nearest classified neighbours on both sides belong to the same
family and lie ≤ 0x2000 bytes apart is that family's object-file run. No zlib or D3DX code was found
statically linked (D3D9 comes through `D3D9.dll`); a statically linked IJG libjpeg exists (its "Copyright
(C) 1998, Thomas G. Lane" and error strings are in `.rdata`) but its message table has no direct function
references in the Ghidra export, so no function is tagged `libjpeg` yet — open item.

[INFERENCE] 0x830000..0x83DFFF holds 303 small functions (23 576 bytes) in `Unknown`, 302 with no callers,
that store constants into `.data` (e.g. 0x835000 writes SSE vectors to 0xAFCA20): compiler-generated
dynamic initialisers of game globals reached through the CRT `_initterm` table. They are game data set-up,
not library code, and are left `Unknown`.

## Coverage

18 766 functions, 4 005 555 body bytes, 43 097 distinct direct call/jump/thunk edges (indirect and virtual
calls are not edges).

| Status | Functions | Bytes | % of all bytes |
|---|---:|---:|---:|
| Reimplemented-cited | 1 321 | 615 926 | 15.4 |
| Named-only | 567 | 188 657 | 4.7 |
| Library | 2 375 | 492 039 | 12.3 |
| Unknown | 14 503 | 2 708 933 | 67.6 |

**17.5 % of non-library code bytes (615 926 of 3 513 516) are cited from code or tests**; 22.8 % counting
docs-only citations too. Of the 567 Named-only, 557 are cited only from docs and 10 are named but cited
nowhere.

| Subsystem (Reimplemented-cited) | Functions | Bytes |
|---|---:|---:|
| game | 683 | 336 378 |
| fx | 87 | 72 373 |
| formats | 127 | 62 592 |
| script | 227 | 41 721 |
| render | 49 | 29 365 |
| physics | 29 | 22 025 |
| audio | 53 | 16 396 |
| cine | 13 | 11 358 |
| collision | 17 | 7 799 |
| ui / locale / anim / scene / cdc / video / tests / re | 36 | 15 919 |

Named-only by area: formats 212, game 170, re 155, render 10, other 20. Unknown functions with a one-hop
area hint: 5 093 (`~game` 1 992, `~formats` 1 507, `~re` 628, `~script` 309, `~fx` 181, `~audio` 113,
`~render` 91, …); 9 410 Unknown functions (1.24 MB) have no cited neighbour at all.

Citations by citing area: src/game 1 294, docs/formats 1 204, docs/GAMEPLAY 1 121, src/script 392,
docs/re 273, tests 275, src/fx 220, src/formats 219, docs/ROADMAP 214.

### RTTI class families

990 vftables; 494 classes have at least one cited slot.

| Family | Classes | Own slots (non-CRT) | Slots cited | Slots named |
|---|---:|---:|---:|---:|
| (global namespace: game classes, e.g. `EnemyBehavior_*`, `PlayerSC_*`, `*State`) | 594 | 2 386 | 86 | 22 |
| `cdc::` (engine) | 290 | 1 318 | 33 | 11 |
| `FMOD::` | 51 | 297 | 0 | 0 |
| `bfx::` | 35 | 125 | 3 | 1 |
| `Screenshots::` / `PDAMap::` / `AsyncTaskManager::` / other | 20 | 74 | 7 | 3 |

Best-covered classes (cited slots / slots): `cdc::PCRenderDevice` 8/69, `EnemyBehavior_PatrolMove` 6/16,
`EnemyBehavior_Sidestep` 6/16, `EnemyBehavior_Flank` 5/18, `EnemyBehavior_PatrolAttack` 5/15,
`EnemyBehavior_PatrolLookTurn` 5/15, `TRGameNotifier` 5/18, the `EnemyBehavior_Investigate*` /
`EnemyBehavior_Search*` / `EnemyBehavior_Patrol*` families 4 each, `EnemyState_RegenerateDeath` 4/13,
`TRMarkupManager` 4/12, `cdc::PCRenderLight` 4/26, `AutoSplineCamera` 3/23. Shared slots (base-class
methods, `_purecall` 0x813237) count for every class that holds them.

## Next RE priorities

Uncited, non-library, non-thunk functions reachable from the 1 308 cited engine roots (Reimplemented-cited
functions in game/render/anim/physics/collision/script/fx/cine/scene/audio/ui/save/formats/platform/cdc/app)
by forward call edges, plus one step from a root to the other vftable slots of its classes (virtual
dispatch has no edge): 7 870 functions, 1 543 884 bytes. Score = size × √(1 + callers) / (1 + 0.25 ×
(depth − 1)): big functions and widely used helpers close to code we already follow rank first.
`depth` = call steps from the nearest root, `reached from` = that root's area. Full list (200) in
`db/priorities.tsv`.

| # | entry | size | callers | callees | depth | reached from | Ghidra name / RTTI | string hint |
|---:|---|---:|---:|---:|---:|---|---|---|
| 1 | `0043f7f0` | 5759 | 2 | 23 | 1 | anim | FUN_0043f7f0 |  |
| 2 | `0046e460` | 4240 | 4 | 17 | 1 | collision | FUN_0046e460 |  |
| 3 | `006faeb0` | 5613 | 1 | 6 | 1 | game | FUN_006faeb0 |  |
| 4 | `00465780` | 4477 | 2 | 15 | 1 | game | FUN_00465780 |  |
| 5 | `0043b870` | 4185 | 2 | 16 | 1 | game | FUN_0043b870 |  |
| 6 | `0047d850` | 5149 | 3 | 4 | 3 | collision | FUN_0047d850 |  |
| 7 | `00526ef0` | 2333 | 6 | 0 | 1 | fx | FUN_00526ef0 |  |
| 8 | `00538930` | 3013 | 3 | 2 | 1 | fx | FUN_00538930 |  |
| 9 | `004ff600` | 5821 | 2 | 6 | 4 | script | FUN_004ff600 |  |
| 10 | `004fa2a0` | 3847 | 4 | 3 | 3 | formats | FUN_004fa2a0 |  |
| 11 | `0045ab00` | 3987 | 2 | 21 | 2 | cine | FUN_0045ab00 |  |
| 12 | `0078fa10` | 3731 | 1 | 0 | 1 | game | FUN_0078fa10 |  |
| 13 | `006fe7e0` | 1206 | 17 | 16 | 1 | game | FUN_006fe7e0 |  |
| 14 | `0043cc10` | 2045 | 5 | 9 | 1 | game | FUN_0043cc10 |  |
| 15 | `00735970` | 2879 | 2 | 17 | 1 | game | FUN_00735970 |  |
| 16 | `006cb060` | 2024 | 5 | 28 | 1 | game | FUN_006cb060 | IS NOT |
| 17 | `00500cd0` | 6105 | 1 | 7 | 4 | script | FUN_00500cd0 |  |
| 18 | `0046a1d0` | 2443 | 5 | 6 | 2 | collision | FUN_0046a1d0 |  |
| 19 | `005b0f60` | 1196 | 22 | 8 | 2 | script | FUN_005b0f60 |  |
| 20 | `0049bf70` | 1710 | 6 | 0 | 1 | render | FUN_0049bf70 |  |
| 21 | `0049da10` | 686 | 41 | 5 | 1 | anim | FUN_0049da10 |  |
| 22 | `005b0110` | 694 | 59 | 6 | 2 | script | FUN_005b0110 |  |
| 23 | `0053d3f0` | 3003 | 2 | 10 | 2 | fx | FUN_0053d3f0 | PCSurface@cdc@@ |
| 24 | `004c9410` | 3115 | 3 | 9 | 3 | script | FUN_004c9410 |  |
| 25 | `005b0890` | 515 | 64 | 9 | 1 | script | FUN_005b0890 |  |
| 26 | `00532f30` | 3579 | 1 | 13 | 2 | fx | FUN_00532f30 |  |
| 27 | `004361c0` | 3510 | 1 | 11 | 2 | game | FUN_004361c0 |  |
| 28 | `005c3aa0` | 443 | 74 | 4 | 1 | game | FUN_005c3aa0 |  |
| 29 | `006615a0` | 2353 | 3 | 19 | 2 | game | FUN_006615a0 | ground old |
| 30 | `005ad590` | 960 | 23 | 6 | 2 | script | FUN_005ad590 |  |
| 31 | `0056dfd0` | 3249 | 3 | 21 | 4 | script | FUN_0056dfd0 |  |
| 32 | `005dc170` | 2553 | 1 | 17 | 1 | game | FUN_005dc170 | attachment |
| 33 | `004cb0c0` | 3122 | 2 | 9 | 3 | script | FUN_004cb0c0 |  |
| 34 | `004c8760` | 3058 | 2 | 9 | 3 | script | FUN_004c8760 |  |
| 35 | `0054a510` | 2478 | 1 | 9 | 1 | fx | FUN_0054a510 |  |
| 36 | `00736860` | 2005 | 2 | 6 | 1 | game | FUN_00736860 |  |
| 37 | `005ada20` | 926 | 13 | 1 | 1 | script | FUN_005ada20 (cdc::UIPDAMapWidget|cdc::UIWidgetBase) |  |
| 38 | `0056ff40` | 2444 | 1 | 7 | 1 | formats | FUN_0056ff40 |  |
| 39 | `004ed7c0` | 1753 | 5 | 8 | 2 | physics | FUN_004ed7c0 |  |
| 40 | `00533d30` | 2407 | 1 | 17 | 1 | fx | FUN_00533d30 |  |
| 41 | `005ea630` | 2351 | 1 | 8 | 1 | collision | FUN_005ea630 |  |
| 42 | `006b1cc0` | 2349 | 1 | 9 | 1 | game | FUN_006b1cc0 |  |
| 43 | `00572db0` | 1028 | 15 | 6 | 2 | script | FUN_00572db0 |  |
| 44 | `00471c90` | 2288 | 1 | 11 | 1 | physics | FUN_00471c90 |  |
| 45 | `005689e0` | 2796 | 2 | 16 | 3 | script | FUN_005689e0 |  |
| 46 | `0074a1c0` | 1804 | 4 | 9 | 2 | game | FUN_0074a1c0 |  |
| 47 | `007b6d00` | 3174 | 0 | 2 | 1 | script | FUN_007b6d00 (NsUIScreen_ScriptType) |  |
| 48 | `005aedb0` | 482 | 63 | 10 | 2 | script | FUN_005aedb0 |  |
| 49 | `004358e0` | 1158 | 6 | 6 | 1 | game | FUN_004358e0 |  |
| 50 | `00727100` | 1530 | 3 | 10 | 1 | game | FUN_00727100 |  |
| 51 | `00447130` | 2699 | 1 | 15 | 2 | formats | FUN_00447130 |  |
| 52 | `005af760` | 519 | 53 | 6 | 2 | script | FUN_005af760 |  |
| 53 | `0056ec90` | 4269 | 1 | 18 | 5 | script | FUN_0056ec90 |  |
| 54 | `004e9610` | 718 | 16 | 1 | 1 | physics | FUN_004e9610 |  |
| 55 | `004ca460` | 3138 | 1 | 9 | 3 | script | FUN_004ca460 |  |
| 56 | `00445ba0` | 3093 | 1 | 21 | 3 | script | FUN_00445ba0 |  |
| 57 | `005b0b70` | 486 | 55 | 7 | 2 | script | FUN_005b0b70 |  |
| 58 | `0053e320` | 3080 | 1 | 26 | 3 | game | FUN_0053e320 |  |
| 59 | `004cbd30` | 3074 | 1 | 9 | 3 | script | FUN_004cbd30 |  |
| 60 | `00773540` | 1280 | 4 | 3 | 1 | render | FUN_00773540 |  |
| 61 | `00726690` | 2500 | 1 | 8 | 2 | game | FUN_00726690 |  |
| 62 | `005b8100` | 1736 | 3 | 15 | 2 | formats | FUN_005b8100 |  |
| 63 | `0049cd40` | 193 | 205 | 1 | 1 | game | FUN_0049cd40 |  |
| 64 | `0066bcc0` | 1208 | 7 | 23 | 2 | script | FUN_0066bcc0 |  |
| 65 | `005d1cd0` | 1578 | 2 | 21 | 1 | scene | FUN_005d1cd0 | Loaded unit: %s |
| 66 | `00549ef0` | 1561 | 2 | 10 | 1 | fx | FUN_00549ef0 |  |
| 67 | `00703b50` | 2352 | 1 | 16 | 2 | game | FUN_00703b50 |  |
| 68 | `004714d0` | 1919 | 2 | 12 | 2 | physics | FUN_004714d0 |  |
| 69 | `0049d720` | 307 | 73 | 1 | 1 | game | FUN_0049d720 |  |
| 70 | `00567620` | 935 | 11 | 13 | 2 | script | FUN_00567620 |  |
| 71 | `005f53b0` | 1594 | 3 | 7 | 2 | game | FUN_005f53b0 |  |
| 72 | `004de560` | 1801 | 1 | 1 | 1 | physics | FUN_004de560 |  |
| 73 | `0056d5c0` | 2220 | 1 | 8 | 2 | script | FUN_0056d5c0 |  |
| 74 | `005088a0` | 2216 | 1 | 23 | 2 | render | FUN_005088a0 |  |
| 75 | `00409970` | 1188 | 9 | 2 | 3 | game | FUN_00409970 |  |
| 76 | `00518970` | 1113 | 4 | 9 | 1 | fx | FUN_00518970 |  |
| 77 | `0049b650` | 292 | 70 | 0 | 1 | cine | FUN_0049b650 |  |
| 78 | `006b25f0` | 1737 | 1 | 12 | 1 | game | FUN_006b25f0 |  |
| 79 | `00706770` | 1725 | 1 | 6 | 1 | game | FUN_00706770 |  |
| 80 | `005ebd90` | 1408 | 2 | 3 | 1 | game | FUN_005ebd90 |  |
| 81 | `005afae0` | 469 | 41 | 4 | 2 | script | FUN_005afae0 |  |
| 82 | `0059f9d0` | 2431 | 0 | 4 | 1 | script | FUN_0059f9d0 (cdc::UISlider) |  |
| 83 | `006ebef0` | 799 | 8 | 5 | 1 | game | FUN_006ebef0 |  |
| 84 | `007020e0` | 831 | 7 | 4 | 1 | game | FUN_007020e0 |  |
| 85 | `006a5dd0` | 497 | 21 | 5 | 1 | game | FUN_006a5dd0 |  |
| 86 | `00464b40` | 2045 | 1 | 6 | 2 | game | FUN_00464b40 |  |
| 87 | `005b1490` | 584 | 23 | 6 | 2 | script | FUN_005b1490 |  |
| 88 | `0047be90` | 1964 | 2 | 5 | 3 | collision | FUN_0047be90 |  |
| 89 | `006b7710` | 2278 | 2 | 30 | 4 | physics | FUN_006b7710 |  |
| 90 | `00635150` | 1260 | 4 | 3 | 2 | script | FUN_00635150 |  |
| 91 | `00437ba0` | 2383 | 1 | 3 | 3 | game | FUN_00437ba0 |  |
| 92 | `005b7720` | 2235 | 0 | 8 | 1 | formats | FUN_005b7720 (MonitorDrawable|cdc::InstanceDrawable) |  |
| 93 | `00469720` | 2732 | 1 | 10 | 4 | physics | FUN_00469720 |  |
| 94 | `007128e0` | 1911 | 2 | 18 | 3 | game | FUN_007128e0 |  |
| 95 | `007b5490` | 2200 | 0 | 2 | 1 | script | FUN_007b5490 (NsLaraBase_ScriptType) |  |
| 96 | `00653bb0` | 319 | 46 | 4 | 1 | game | FUN_00653bb0 |  |
| 97 | `006a3790` | 1927 | 1 | 14 | 2 | game | FUN_006a3790 |  |
| 98 | `004edeb0` | 820 | 10 | 7 | 2 | physics | FUN_004edeb0 |  |
| 99 | `00646870` | 961 | 7 | 4 | 2 | script | FUN_00646870 | ballistic |
| 100 | `006292d0` | 1082 | 3 | 12 | 1 | game | FUN_006292d0 |  |

## Verification sample (instruction level)

Eleven high-value citations were checked against `objdump -d` of the original `tru.exe` (bodies bounded by the
Ghidra function sizes; listings in `openunderworld-work/re-baseline/FunctionDb/asm/`). "Agrees" means the
instructions show what the citing text claims; decompiler output was not used as proof.

| Address | Repo claim (citation) | Instructions | Verdict |
|---|---|---|---|
| 0x5DA230 `GAMELOOP_UpdateTime` | 6-frame moving average of the frame delta, clamp 66 666 µs (docs/re/TIMING.md:51) | calls 0x597F10; 6-entry ring at 0xE7EE10 indexed `mod 6` (`idiv 6`), 5-iteration sum; `cmp $0x1046a` / `mov $0x1046a` (= 66 666) | agrees |
| 0x5E0190 `MainG2` | loop `MAIN_ProcessMainState(0xE7ED60, 0xE7F088)`; `STREAM_PollLoadQueue`; `MainG2_PlatformUpdate` until 0xE7ED6C set (docs/re/STARTUP.md:40) | `push $0xe7f088; push $0xe7ed60; call 0x5e0090`; `call 0x5c1da0`; `call 0x47afd0`; `cmpl $0,0xe7ed6c; je` loop; also leaves when 0x47AFD0 returns 0 | agrees (the second exit, platform update returning false, is also in docs as "returns false on quit") |
| 0x597E90 `TIMER_SampleMicroseconds` | QPC × 1 000 000 / QPF (docs/re/TIMING.md:12) | two IAT calls (QPC/QPF), `push $0xf4240` into `__allmul` 0x8145B0, then `__alldiv` 0x816940 | agrees |
| 0x69DA80 sea waves | Σ hᵢ(sin(…) − 1) via CRT sin 0x813680 (docs/GAMEPLAY.md:1671, CHANGELOG 0.50.0) | four `call 0x813680`; 0x813680 = MSVC `_CIsin` shape (SSE2 dispatch `jmp 0x81cb40`, x87 path `fsin` at 0x8136ED) | agrees; Ghidra FID did not name 0x813680 (it is `FUN_00813680`, so it falls outside the `crt` tag) |
| 0x4EFB20 body integration | `v *= 1 − dt × damping`, damping at body +0x154 / +0x158 (src/physics/Physics.cpp:731) | `fld1`, `fsub` of `dt × [edi+0x154]`, clamp to [0, 1], `mulps` into linear velocity +0xA0; same with `[edi+0x158]` for angular | agrees (the factor is clamped to 0..1; irrelevant for 0.01/frame) |
| 0x57CE50 SCMD interpreter | switch on the opcode, `case = op − 1` (CHANGELOG, docs/ROADMAP.md:53) | opcode = word >> 24, per-opcode counter at 0xCC4B58, `add $-1`, `cmp $0x84; ja`, `jmp *0x57e52c(,%eax,4)` (133 cases) | agrees |
| 0x6F1790 fall-damage curve | 0 up to t1, linear to 500 at t2, to 1000 at t3 (docs/GAMEPLAY.md:262) | pure x87 piecewise-linear of its stack args: above t3 → max, t2..t3 and t1..t2 lerps with a 0.5 × max knee (constant 0x8413C0), below t1 → 0 | agrees (the +0x4C/+0x50/+0x54 reads are in the caller) |
| 0x7594C0 wall-jump check | returns 0 when the query has flag 0x80, else 1/2 (tests/selftests/chimney_floor_jumps.json:5, docs/GAMEPLAY.md:2868) | `testb $0x80,0x2c(%eax)` → `xor %eax,%eax; ret`; exits `mov $2` and `mov $1` | agrees |
| 0x5570F0 Save Game | buffers every live instance via 0x5568C0 (docs/GAMEPLAY.md:3935) | `call 0x5568c0` then 0x555BE0 | agrees |
| 0x6AD070 water volume update | gathers primitives via 0x4DEE60 (docs/GAMEPLAY.md:1681) | contains `call 0x4dee60` | agrees |
| 0x473840 `InitArchive` | sets the archive spec mask to 1, opens BIGFILE (docs/re/FILESYSTEM.md:21, docs/formats/CONTAINERS.md:42) | `call 0x4737f0` (MultiFileSystem ctor), `movl $0x1,0x8(%eax)`, `call 0x4730b0` (`ArchiveFileSystem_Open`), `ERROR_Fatal` 0x596CB0 on failure | agrees |

Disagreements and naming notes found while building the database:

- 0x67ED20 is slot 7 of `EnemyState_RegenerateDeath`'s vftable; `src/game/ai/Enemy.hpp:87` and
  docs/GAMEPLAY.md:3619 call it "EnemyRegenerateData". The string `EnemyRegenerateData` is referenced by the
  neighbouring 0x67ECF0 and 0x682460, so the repo's label is the record's type name, not the function's.
  The role (regenerate fields of the death record) is not contradicted. Applied as
  `EnemyState_RegenerateDeath_vf7`.
- 0x61DD10 is cited as "InvestigateStart" by name extraction, but it is slot 13 of
  `EnemyBehavior_Investigate` (src/game/ai/Enemy.cpp:233 describes it as the message-0xBF handler);
  `InvestigateStart` is a separate class (`EnemyBehavior_InvestigateStart`).
- 0x417F30 is cited as bfx `CreateLinkStrip`; the only `CreateLinkStrip` string reference is in 0x4084C0, so
  the name is applied at medium confidence.
- No sampled role disagreed with the code.

## Names added to `tools/ghidra/names/tru_gog.csv`

24 rows (12 high, 12 medium), each with `src/...:line` evidence: role from repo code + citations, confirmed
by an RTTI vftable slot, a referenced string, or (constructors) the class vftable stored at `this+0`:
`PCStateManager_SetBlendMode` 0x527A80, `INSTANCE_IntroduceInstance` 0x5BCC30, `UberObjectComposite_ctor`
0x4D7D50, `UberObjectSection_ModelMarkerAnchor_ctor` 0x4D3110, `bfx_NavGraph_AddResource` 0x41C8C0,
`bfx_AddNavGraph` 0x41C7D0, `bfx_CreateLink` 0x417D50, `bfx_CreateLinkStrip` 0x417F30,
`AutoSplineCamera_Start` 0x5E26F0, `AutoSplineCamera_vf1_Update` 0x5E2D90, `ScriptNative_autosplinecamera`
0x7759F0, `PlayerSC_Grapple_OverhangDetected` 0x73AB70, `EnemyBehavior_Sidestep_vf3` 0x677D70,
`EnemyState_Fire_vf7`/`_vf9` 0x666D10/0x666EB0, `EnemyBehavior_Flank_vf3` 0x6365B0,
`EnemyBehavior_PatrolAttack_vf4` 0x635920, `EnemyBehavior_Investigate_vf4`/`_vf13_OnMessage`
0x61DA60/0x61DD10, `EnemyState_RegenerateDeath_vf7` 0x67ED20, `EnemyState_Death_vf7` 0x66C500,
`UberObjectSection_TR8_vf2` 0x6D5EC0, `PCRenderLight_BuildRegisters` 0x50B910, `UberObject_AttachTo` 0x4A4490.

Applied to the Ghidra project with `ApplyNames.java` (headless, `-noanalysis`, saved): **168 functions, 23
labels, 0 errors** (was 144 / 23). The CSV now has 87 high, 88 medium, 16 low rows (191);
[FUNCTIONS.md](FUNCTIONS.md)'s "144 functions" count predates this. The analyze.sh exports in
`openunderworld-work/ghidra/exports/` were not regenerated; `ExportGraph` output carries the new names.

## Open questions

- 1 435 of 2 375 library tags come from the address-run fill. The FMOD/CRT run boundaries (0x7B…0x83) and
  the bfx run (0x401000..0x42xxxx, which also holds NavPower code we parse) should be spot-checked; an FID
  pass with an MSVC 2005/2008 + FMOD Ex signature set would replace most fills with direct evidence.
- libjpeg: locate its functions (the error-message table at 0x85B7E4.. is referenced through a pointer
  table, not directly).
- Virtual calls are not edges; reachability through vftables is only approximated (one step from cited
  slots). An indirect-call resolver (vtable offset + receiver class) would sharpen the priority list.
- 6 306 non-library functions have no direct caller (vftable slots, callbacks, initialisers, jump-table
  targets); they cannot be reached by the call-edge walk.
- The RTTI slot names (`Class_vfN`) could be applied wholesale for the 594 + 290 game/engine classes as
  low-confidence names; not done here (the brief limits names to roles established by repo code).
