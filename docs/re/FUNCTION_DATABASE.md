# tru.exe function database

A per-function inventory of the GOG `tru.exe` (sha256 `1fdbd345…1011`, see [EXECUTABLE.md](EXECUTABLE.md)) joined
with every tru.exe address the repository cites. It answers: which original functions has OpenUnderworld
already reconstructed (cited from code or tests), which are only documented, which are third-party library
code, what each function looks like at the machine level (calling convention, stack arguments, frame, globals,
strings, constants, `this` offsets, switch tables, virtual calls), and which uncited functions sit closest to what
we already reimplement. First generated 2026-11-16 at v0.51.0; this revision (second pass, same day, repo
`ffae429` + this commit) adds the per-function detail export, Function ID results, virtual-call resolution and the
cross-reference / string / module reports. Ghidra 12.2 project in `openunderworld-work/ghidra/project`.

No decompiled code is stored here: the tables hold addresses, sizes, counts, names, RTTI class names, library
tags, `path:line` citations, short string values (≤ 60 characters, ≤ 8 per function) and one string per
priority row as a hint. Bulk per-function lists (constants, globals, raw `this` accesses, vptr call sites) stay in
the work dir.

## Regenerating

```sh
W=../openunderworld-work; mkdir -p $W/re-baseline/FunctionDb $W/re-baseline/FuncDetail
H="systemd-run --user --scope -q -p MemoryMax=8G -- env GHIDRA_HEADLESS_MAXMEM=4G /opt/ghidra/support/analyzeHeadless"
# 1. Graph export (read-only on the project; one Ghidra JVM at a time; ~10 s):
$H $W/ghidra/project tru -process tru.exe -noanalysis -readOnly -scriptPath tools/ghidra/scripts \
    -postScript ExportGraph.java $W/re-baseline/FunctionDb
# 2. Per-function detail + Function ID (read-only; ~35 s; mode detail | fid | all):
$H $W/ghidra/project tru -process tru.exe -noanalysis -readOnly -scriptPath tools/ghidra/scripts \
    -postScript ExportFunctionDetail.java $W/re-baseline/FuncDetail all
# 3. Join (stdlib Python, ~2 s):
tools/re/function_db.py            # --work DIR (default ../openunderworld-work), --out DIR (default docs/re/db)
```

Determinism: `ExportFunctionDetail all` was run twice into separate directories and every output file was
byte-identical; `function_db.py` run twice gives byte-identical `docs/re/db/` outputs. (Citation counts move
when other commits add docs that cite addresses; the join itself is a pure function of its inputs.)

`tools/ghidra/scripts/ExportGraph.java` writes `functions_full.tsv` (entry, name, namespace, symbol source,
size, body ranges, thunk target, FID flag), `edges.tsv` (caller, callee, call|jump|thunk, site count),
`func_refs.tsv`, `data_symbols.tsv` and `blocks.tsv`. `tools/ghidra/scripts/ExportFunctionDetail.java` writes
`detail.tsv`, `globals.tsv`, `string_xrefs.tsv`, `consts.tsv`, `this_offsets.tsv`, `vcalls.tsv`,
`vft_stores.tsv`, `ptr_stores.tsv`, `obj_calls.tsv`, `switches.tsv` (detail mode) and `fid_vsOlder_x86.tsv`,
`fid_vs2012_x86.tsv` (fid mode); see the script header for every column. `function_db.py` also reads the older
analyze.sh exports `rtti.tsv` (990 vftables), `strings.tsv`, `imports.tsv`, and `tools/ghidra/names/tru_gog.csv`.
The global-variable inventory is StructDb's (`docs/re/db/globals.tsv`, built from FuncDetail's `globals.tsv`); this
database does not duplicate it.

## Outputs (`docs/re/db/`)

| File | Rows | Content |
|---|---:|---|
| `functions.tsv` | 18 766 | one row per function, columns below (2.7 MB) |
| `citations.tsv` | 7 308 | address, kind (`func_entry`, `func_interior`, `vftable`, `string`, `data_symbol`, `data`), containing function, path, line, citing subsystem, name the citing text attaches |
| `priorities.tsv` | 200 | uncited non-library functions reachable from cited engine roots over direct + resolved virtual edges, ranked (top 100 below) |
| `rtti.tsv` | 990 | class, vftable, slot count, slots cited, vftable itself cited, bases |
| `vcall_edges.tsv` | 845 | caller, site, receiver class (suffix `+n` after a bar = n more classes with the same callee), slot index, callee, evidence, candidate count |
| `xrefs_summary.tsv` | 18 766 | entry, direct callers/callees, virtual callers/callees, vftables holding it, pointer refs to it, data refs, globals read/written, strings |
| `string_refs.tsv` | 2 010 | entry, string address, sites, value (≤ 60 chars; ≤ 8 strings per function, by address) |
| `module_deps.tsv` | 266 | DLL, import, calling functions (direct refs + callers of the import thunk), their subsystems (`lib:<family>` for library callers, `?` uncited), first 8 callers |
| `this_offsets.tsv` | 11 950 | entry, base (`this` / `arg1`), count, `offset:rw:sizes` list ([INFERENCE], method below; 0.48 MB) |
| `summary.json` | | every number quoted in this file |

### `functions.tsv` columns

| Column | Meaning |
|---|---|
| entry, size, callers, callees, thunk, thunk_target | Ghidra function, body bytes, distinct direct callers/callees from `edges.tsv` |
| ghidra_name, applied_name, repo_name | Ghidra symbol (with applied CSV names), the CSV name, the identifier the citing text attaches |
| subsystem, cited_by, cited_files, cite_scope, citations | citing area (`~area` = one-hop hint for uncited code), citation counts, first 3 `path:line` |
| rtti_class, vft_slots | classes whose vftables hold the entry; `Class#slot` in primary (offset-0) vftables (first 2, then a `+n` count) |
| library, status | `family:evidence`, Library / Reimplemented-cited / Named-only / Unknown (rules below) |
| data_refs, string_refs | ExportGraph counts (distinct data addresses, string references) |
| cc | own calling-convention check: `thiscall` (ECX read before written on the entry path), `fastcall` (ECX and EDX), `stdcall` (`ret imm16` > 0, no ECX), `cdecl` (plain `ret`), `noret` (no RET in the body), `thunk` |
| cc_ghidra, ecx_first | Ghidra's calling convention (`unknown` for 18 503: the project never ran the decompiler parameter-ID pass), first ECX use on the entry path (read / write / call / none / jmp) |
| stack_args, params, ret_imm | stack argument bytes (`ret imm16` when callee-clean, else Ghidra's observed parameter-area size), `stack_args / 4`, every distinct `ret` immediate |
| ret_guess | majority over RETs of the instruction before the epilogue: `int` (writes EAX), `float` (FLD), `call` (tail value of a call), `void` |
| frame | Ghidra stack frame local size (bytes) |
| g_reads, g_writes, g_addr | distinct non-executable addresses read / written / only address-taken (per-address list: work-dir `globals.tsv`) |
| strings, consts, this_offsets | distinct strings referenced; immediate constants kept (floats/doubles loaded from `.rdata`, ints with \|v\| > 256, ≤ 24 per function, list in work-dir `consts.tsv`); distinct (base, offset) pairs on `this`/`arg1` |
| switches, switch_cases, icalls, vcall_sites | computed jumps with > 1 target and their total targets, indirect calls, indirect calls through a vptr (`call [vptr+N]`) |
| vcallers, vcallees | distinct callers/callees over resolved virtual edges |
| fid | `vsOlder` / `vs2012` : FID status : name (recorded only, never applied as a name) |
| confidence | confidence in the repo role: `high` = cited and the citation verified at instruction level; `medium` = cited (code, tests or docs) or library with direct evidence; `low` = heuristic only (address-run library fill, uncited CSV name, `~area` hint); `none` |

Method notes ([INFERENCE] where marked):

- The ECX/EDX first-use walk follows fall-through and unconditional jumps from the entry, ignores `push ecx`
  (MSVC's 4-byte stack allocation idiom), treats `xor/sub reg,reg` as a write, and stops at the first call. ECX
  live-in is what MSVC `__thiscall` produces; VS2005 link-time code generation can also pass arguments in
  registers, so `thiscall` means "ECX is an input", not proof of a C++ method. Where Ghidra had a convention it
  agrees in 231 of 263 rows; of the 32 differences, 12 are Ghidra `__cdecl` functions that read ECX/EDX first,
  10 have no RET in the body (tail jump or no-return), 7 are thunks and 3 are Ghidra `__fastcall` guesses.
- `this` offsets: registers tagged `this` (ECX at entry, for thiscall) or `arg1` (loaded from stack offset +4) are
  propagated through `mov reg, reg` in address order and killed on any other write or a call (EAX/ECX/EDX);
  every `[tagged + disp]` operand (0 ≤ disp < 0x10000, LEA excluded) is recorded with read/write from Ghidra's
  operand ref type and the access size from the instruction's p-code. Address order is not control flow, so
  merges are optimistic [INFERENCE]. 11 950 functions have at least one access; 32 137 (this) + 8 136 (arg1)
  raw (function, offset) rows.
- Access sizes come from p-code LOAD/STORE or RAM varnodes, so `?` only remains where an operand has neither.

### Status

| Status | Rule |
|---|---|
| Library | direct library evidence (below), or uncited and inside a library's address run |
| Reimplemented-cited | cited from `src/` or `tests/` (a code comment or self-test note names the address or an instruction inside the function) |
| Named-only | cited only from docs/CHANGELOG, or named in `tru_gog.csv` / by a user-defined Ghidra symbol, with no code or test citation |
| Unknown | none of the above |

"Reimplemented-cited" means the repo code says it follows that function; it is not a claim that the
reimplementation is complete or faithful. A library function cited from code (e.g. a bfx NavPower routine whose
data we parse, or MSVC `_rand` 0x814118 cited by `src/fx/Weather.hpp:79`) keeps status Library.

`subsystem` is the weighted majority of the citing areas (src citations weigh 3, others 1). For uncited
functions, `~area` is the majority area of cited callers and callees (one hop) — an [INFERENCE] hint only.
`repo_name` is a heuristic: the identifier written next to the address in the citing line; treat it as a
pointer into the citing text, not as the original function's name.

### Citation harvest rules

All `git ls-files` text under `src/ tests/ docs/ tools/` and `CHANGELOG.md` are scanned for `0x` + 6–8 hex
digits, Ghidra-style `FUN_/DAT_/LAB_/PTR_…_/switchD_xxxxxxxx`, and (docs only) bare `00XXXXXX`, in
0x401000..0x10D9FFF, resolving to a function entry, an instruction inside a function, a vftable, a string, a named
data symbol, or other data. Filters: code files only inside comments; JSON only on lines with `tru`, `note` or
`[O`; values whose low 12/16 bits are zero or whose three low bytes are equal are dropped; unnamed data addresses
in code/JSON need `tru`, `global`, `DAT_` or `[O`; bare `00XXXXXX` must hit a function entry, vftable or named
symbol; `tools/ghidra/names/`, `docs/re/BINARIES.md`, `tools/re/pe_inventory.py`, `tools/re/function_db.py`
(its `VERIFIED` set) and this database's own outputs are skipped.

Result now: 7 308 citations from 218 files: 2 012 distinct function entries, 127 interior addresses, 35
vftables, 23 named data symbols, 152 other data addresses.

### Library classification

| Family | Functions | Bytes | Evidence |
|---|---:|---:|---|
| fmod (FMOD Ex, statically linked) | 1 232 | 277 078 | 51 `FMOD::` RTTI classes' slots; functions referencing `..\..\src\fmod_*.cpp` paths or FMOD error strings; address-run fill |
| crt (MSVC CRT/STL/EH) | 650 | 105 927 | 381 Ghidra analysis-time FID bookmarks, 4 new FID matches (below), `FID_conflict:`/`_`/`__` names, `std::`/`type_info`/`exception` RTTI |
| bfx (NavPower/behaviour middleware) | 509 | 111 073 | 35 `bfx::` RTTI classes' slots, `bfx_` names; address-run fill at 0x401000..0x42xxxx |

Evidence counts: 381 ghidra-fid, 4 fid-vsOlder, 411 RTTI slot, 149 string, 15 name/import, 1 431 address-run.
The fill rule ([INFERENCE]): an uncited function whose nearest classified neighbours on both sides share a family
and lie ≤ 0x2000 bytes apart is that family's object-file run. No zlib or D3DX code is statically linked; IJG
libjpeg is present but its functions are still untagged (open item).

### Function ID (second pass)

`ExportFunctionDetail fid` queries each shipped signature database alone (all other `.fidbf` files deactivated
for the query, previous active states restored afterwards) through `FidService.processProgram` at Ghidra's
default score threshold, and classifies each hit with Ghidra's own `MatchNameAnalysis` (single name / same base
name / different names; "below_multi" = different names under the multi-name threshold, which the FID analyzer
rejects). Read-only: no FID name is applied to the project.

| Database | Matched | Single | Multi (accepted) | Below multi (rejected) | Accepted | Also a Ghidra FID bookmark | Libraries (top) |
|---|---:|---:|---:|---:|---:|---:|---|
| `vsOlder_x86` (VS 6 … 2008; tru.exe is VS2005) | 375 | 356 | 14 | 5 | 370 | 366 | VS2005 Release 321, VS2005 Debug 18, VS2003 Release 13, VS6 9 |
| `vs2012_x86` (comparison) | 56 | 49 | 1 | 6 | 50 | 49 | VS2012 Debug 41, Release 15 |

- Every accepted vs2012 match is also an accepted vsOlder match (version-stable asm routines such as `__chkstk`,
  `__alloca_probe_*`); vs2012 adds no evidence.
- 4 accepted vsOlder matches are new relative to the analysis-time bookmarks: `__purecall` 0x813237 (score 72.7;
  the shared abstract-slot stub, now used by the virtual-call resolver), `__set_abort_behavior` 0x8146D7,
  `__seh_longjmp_unwind4@4` 0x81FACA, `std::exception` copy constructor 0x82D758.
- 13 of the 381 analysis-time bookmarks match neither database alone (e.g. `__endthreadex` 0x81645C,
  `terminate` 0x826F6D, `__NLG_Notify` 0x8279C5, `__crtLCMapStringW_stat` 0x82F5F6): they came from the newer
  VS2015+ databases that were active during auto-analysis. They keep their `ghidra-fid` tag.
- **Address-run fill vs FID: of the 1 435 address-run tags, 4 are confirmed (all four were `crt` fills and are
  now `crt:fid-vsOlder`), 0 are contradicted, 1 431 stay unverified.** 1 176 of those are fmod (786) / bfx (390)
  fills, which no Microsoft signature set can confirm or refute; of the 259 `crt` fills only 4 have a signature
  match. So FID neither supports nor undermines the run boundaries; an FMOD Ex 4.x signature set (or a matching
  `fmodex_vc.lib`) remains the way to verify the 0x7B…0x83 run.
- FID vs analyst names: no accepted match lands on a function with a CSV or user-defined name (0 conflicts).
  One accepted match is cited from code: `_rand` 0x814118 (src/fx/Weather.hpp:79), consistent with the citation.
- Not identified by FID although CRT-shaped: 0x813680 (`_CIsin` shape) and 0x814D20 (`_ftol2_sse` shape:
  `cvttsd2si` behind a SSE2 flag test).

## Coverage

18 766 functions, 4 005 555 body bytes, 43 097 distinct direct call/jump/thunk edges, plus 710 distinct resolved
virtual caller→callee pairs (845 edge rows at 644 call sites).

| Status | Functions | Bytes | % of all bytes |
|---|---:|---:|---:|
| Reimplemented-cited | 1 321 | 615 926 | 15.4 |
| Named-only | 704 | 258 561 | 6.5 |
| Library | 2 374 | 492 013 | 12.3 |
| Unknown | 14 367 | 2 639 055 | 65.9 |

**17.5 % of non-library code bytes (615 926 of 3 513 542) are cited from code or tests**; 24.9 % counting
docs-only citations too (Named-only grew from 567 to 704 since the first pass because new audit/RE docs cite
addresses; code coverage is unchanged). Confidence column: high 22, medium 2 959, low 7 130, none 8 655.

| Subsystem (Reimplemented-cited) | Functions | Bytes |
|---|---:|---:|
| game | 683 | 336 378 |
| fx | 87 | 72 373 |
| formats | 126 | 62 375 |
| script | 227 | 41 721 |
| render | 47 | 27 995 |
| physics | 27 | 21 585 |
| audio | 53 | 16 396 |
| cine | 13 | 11 358 |
| collision | 17 | 7 799 |
| ui / re / anim / scene / locale / video / tests / cdc | 41 | 17 946 |

Unknown functions with a one-hop area hint: 5 712 (`~game` 1 895, `~re` 1 523, `~formats` 1 411, `~script` 302,
`~fx` 171, `~audio` 108, …); 8 655 Unknown functions (1.12 MB) have no cited neighbour at all.

### Machine-level profile (all 18 766 functions)

| Property | Count |
|---|---:|
| cc thiscall / cdecl / stdcall / fastcall / noret / thunk | 10 034 / 6 189 / 1 957 / 73 / 312 / 201 |
| stack arguments > 0 | 8 723 (1 arg 4 642, 2 args 2 033, 3 args 922, 4+ args 1 125) |
| `ret` immediates: 0 / 4 / 8 / 12 / 16 | 9 134 / 4 581 / 1 979 / 884 / 604 |
| return guess int / void / call / float | 6 164 / 5 959 / 4 987 / 549 |
| frame ≥ 256 bytes | 586 |
| with a switch table (464 functions, 619 tables) | 464 |
| with ≥ 1 global write | 1 762 |
| with ≥ 1 string reference | 1 233 |
| with `this`/`arg1` offsets | 11 950 |
| primary-vftable slots | 4 040 |
| indirect calls / of them through a vptr | 11 556 / 9 146 |

### Virtual calls

ExportFunctionDetail records each `call [reg+N]` (or `mov r2,[reg+N]; call r2`) whose `reg` was loaded as
`mov reg, [obj+k]` (the vptr), and traces `obj` back along straight-line code only (any jump target is a merge
point and stops the walk; calls clobber EAX/ECX/EDX). `function_db.py` resolves a site only when the receiver
class is provable:

| Evidence | Sites | Rule |
|---|---:|---|
| `this-method` | 333 | `obj` is ECX at entry (or a callee-saved register written exactly once, from entry ECX) and the caller is a slot of class C: the dynamic class is one of the classes whose vftable holds the caller; one distinct callee |
| `this-method-poly` | 92 | as above, several distinct callees (one row each, 293 rows; `__purecall` dropped) |
| `global-ptr-stores` | 143 | `obj` loaded from a global pointer all of whose direct stores resolve (null / `xor` stores ignored); union of classes |
| `getter` | 72 | `obj` returned by a function that is `mov eax, imm; ret` (static object) or `mov eax, [g]; ret` (global pointer) |
| `global-object` | 4 | static object: most derived class among vftables stored into it (absolute stores, and constructors/destructors called with ECX = its address, including the CRT initialiser stubs outside Ghidra functions) |
| `ctor-store`, `new-object` | 0 | supported (vftable stored into `obj` in the same function; `obj` returned by a constructor), but no site survives the straight-line rule: MSVC null-checks the `new` result, so the constructed object reaches the call through a merge |

Resolved: **644 of 9 146 vptr call sites (7.0 %)**, 710 distinct caller→callee pairs, 342 callees reached. Most
used classes: `cdc::PCRenderDevice` 121 edge rows (global device pointer), `CameraManager` 82 (static object
0xE804F0, constructed by 0x5F3A70 from the initialiser stub at 0x837000; getter 0x5E7770),
`cdc::ScriptUIScreen` 35. Example checked against the instructions: 0x5E2CCB in 0x5E2CA0 calls
`[[0x5E7770()]+0x50]`; 0x5E7770 is `mov eax, 0xE804F0; ret`, 0x837000 does `mov ecx, 0xE804F0; call 0x5F3A70`, and
0x5F3A70 stores vftable 0x855AE4 (`CameraManager`) at `[esi]` → slot 20 = 0x5EDFC0.

Unresolved sites by origin: object loaded from a field 5 497 (needs member types), merge point 945, global
pointer without a provable store set 642 (390 sites read pointers that no instruction stores directly), returned
by a call that neither constructs nor is a getter 360, parameter 349, method that is no vftable slot 184, call
clobbered 175, other 350 (entry register 61, walk limit 61, local 101, other instruction 102, sub-object 19,
static object without a class 6).

### Functions without a direct caller

6 306 non-library functions have no direct call/jump edge (unchanged count). Breakdown now:

| Reached by | Functions |
|---|---:|
| a resolved virtual call | 270 |
| no resolved call, but a vftable slot | 3 270 |
| no call, but a pointer to it is stored (callbacks, tables) | 760 |
| inside the 0x830000..0x83DFFF dynamic-initialiser run | 6 |
| nothing found | 2 000 |

Reachability from the 1 302 cited engine roots: 7 740 uncited functions (1 479 235 bytes) over direct edges, 7 822
(1 489 893 bytes) over direct + resolved virtual edges (+82 functions).

### Module dependencies

12 imported DLLs (266 imports; Direct3D, DirectSound and DirectInput are not in the import table [INFERENCE]:
loaded at run time). Callers by area (`lib:` = library code, `?` = uncited):

| DLL | Imports | Calling functions | Callers by area |
|---|---:|---:|---|
| KERNEL32 | 127 | 315 | lib:crt 194, uncited 190, lib:bfx 112, re 60 |
| USER32 | 55 | 33 | uncited 45, re 37, lib:fmod 2 |
| WINMM | 21 | 20 | lib:fmod 29, re 2 |
| WS2_32 | 18 | 8 | lib:fmod 26 (only FMOD uses sockets: net streaming) |
| ADVAPI32 | 13 | 17 | re 28, uncited 12, lib:fmod 11 |
| BINKW32 | 12 | 6 | formats 6, video 6 |
| OLE32 | 9 | 18 | lib:fmod 25, uncited 6, re 4 |
| MSACM32 | 6 | 2 | lib:fmod 6 |
| OLEAUT32, GDI32, COMCTL32, SHELL32 | 5 | 6 | uncited / re |

(The area counts sum over imports, so one caller of several imports counts several times.) The two most called
imports are `EnterCriticalSection` / `LeaveCriticalSection` (117 / 116 callers, 54 of them bfx).

### RTTI class families

990 vftables; 494 classes have at least one cited slot.

| Family | Classes | Own slots (non-CRT) | Slots cited | Slots named |
|---|---:|---:|---:|---:|
| (global namespace: game classes, e.g. `EnemyBehavior_*`, `PlayerSC_*`, `*State`) | 594 | 2 386 | 86 | 23 |
| `cdc::` (engine) | 290 | 1 318 | 33 | 11 |
| `FMOD::` | 51 | 297 | 0 | 0 |
| `bfx::` | 35 | 125 | 3 | 1 |
| `Screenshots::` / `PDAMap::` / `AsyncTaskManager::` / other | 20 | 74 | 7 | 3 |

## Next RE priorities

Uncited, non-library, non-thunk functions reachable from the 1 302 cited engine roots by forward direct and
resolved virtual edges, plus one step from a root to the other vftable slots of its classes: 7 822 functions,
1 489 893 bytes. Score = size × √(1 + direct and virtual callers) / (1 + 0.25 × (depth − 1)). Full list (200) in
`db/priorities.tsv`; `cc` from the own calling-convention check.

| # | entry | size | callers (direct+virtual) | callees | depth | reached from | cc | Ghidra name / RTTI | string hint |
|---:|---|---:|---:|---:|---:|---|---|---|---|
| 1 | `005a1430` | 1397 | 18 | 6 | 1 | script | thiscall | FUN_005a1430 (cdc::ScriptUIScreen / cdc::UIBitmap / cdc::UIButton / ...) |  |
| 2 | `004ff600` | 5821 | 2 | 6 | 4 | script | thiscall | FUN_004ff600 |  |
| 3 | `004fa2a0` | 3847 | 4 | 3 | 3 | formats | cdecl | FUN_004fa2a0 |  |
| 4 | `0045ab00` | 3987 | 2 | 21 | 2 | cine | thiscall | FUN_0045ab00 |  |
| 5 | `0078fa10` | 3731 | 1 | 0 | 1 | game | thiscall | FUN_0078fa10 |  |
| 6 | `006fe7e0` | 1206 | 17 | 16 | 1 | game | cdecl | FUN_006fe7e0 |  |
| 7 | `0043cc10` | 2045 | 5 | 9 | 1 | game | thiscall | FUN_0043cc10 |  |
| 8 | `00735970` | 2879 | 2 | 17 | 1 | game | cdecl | FUN_00735970 |  |
| 9 | `006cb060` | 2024 | 5 | 28 | 1 | game | cdecl | FUN_006cb060 | IS NOT |
| 10 | `00500cd0` | 6105 | 1 | 7 | 4 | script | thiscall | FUN_00500cd0 |  |
| 11 | `0046a1d0` | 2443 | 5 | 6 | 2 | collision | cdecl | FUN_0046a1d0 |  |
| 12 | `005b0f60` | 1196 | 22 | 8 | 2 | script | thiscall | FUN_005b0f60 |  |
| 13 | `0049bf70` | 1710 | 6 | 0 | 1 | render | cdecl | FUN_0049bf70 |  |
| 14 | `0049da10` | 686 | 41 | 5 | 1 | anim | thiscall | FUN_0049da10 |  |
| 15 | `005b0110` | 694 | 59 | 6 | 2 | script | thiscall | FUN_005b0110 |  |
| 16 | `0053d3f0` | 3003 | 2 | 10 | 2 | fx | cdecl | FUN_0053d3f0 | PCSurface@cdc@@ |
| 17 | `004c9410` | 3115 | 3 | 9 | 3 | script | thiscall | FUN_004c9410 |  |
| 18 | `005b0890` | 515 | 64 | 9 | 1 | script | thiscall | FUN_005b0890 |  |
| 19 | `00532f30` | 3579 | 1 | 13 | 2 | fx | cdecl | FUN_00532f30 |  |
| 20 | `004361c0` | 3510 | 1 | 11 | 2 | game | thiscall | FUN_004361c0 |  |
| 21 | `005c3aa0` | 443 | 74 | 4 | 1 | game | thiscall | FUN_005c3aa0 |  |
| 22 | `006615a0` | 2353 | 3 | 19 | 2 | game | thiscall | FUN_006615a0 | ground old |
| 23 | `005ad590` | 960 | 23 | 6 | 2 | script | thiscall | FUN_005ad590 |  |
| 24 | `0056dfd0` | 3249 | 3 | 21 | 4 | script | thiscall | FUN_0056dfd0 |  |
| 25 | `005dc170` | 2553 | 1 | 17 | 1 | game | thiscall | FUN_005dc170 | attachment |
| 26 | `004cb0c0` | 3122 | 2 | 9 | 3 | script | thiscall | FUN_004cb0c0 |  |
| 27 | `004c8760` | 3058 | 2 | 9 | 3 | script | thiscall | FUN_004c8760 |  |
| 28 | `0054a510` | 2478 | 1 | 9 | 1 | fx | thiscall | FUN_0054a510 |  |
| 29 | `00736860` | 2005 | 2 | 6 | 1 | game | cdecl | FUN_00736860 |  |
| 30 | `005ada20` | 926 | 13 | 1 | 1 | script | thiscall | FUN_005ada20 (cdc::UIPDAMapWidget / cdc::UIWidgetBase) |  |
| 31 | `0056ff40` | 2444 | 1 | 7 | 1 | formats | thiscall | FUN_0056ff40 |  |
| 32 | `004ed7c0` | 1753 | 5 | 8 | 2 | physics | thiscall | FUN_004ed7c0 |  |
| 33 | `00533d30` | 2407 | 1 | 17 | 1 | fx | cdecl | FUN_00533d30 |  |
| 34 | `005ea630` | 2351 | 1 | 8 | 1 | collision | cdecl | FUN_005ea630 |  |
| 35 | `006b1cc0` | 2349 | 1 | 9 | 1 | game | thiscall | FUN_006b1cc0 |  |
| 36 | `00572db0` | 1028 | 15 | 6 | 2 | script | thiscall | FUN_00572db0 |  |
| 37 | `005689e0` | 2796 | 2 | 16 | 3 | script | thiscall | FUN_005689e0 |  |
| 38 | `0074a1c0` | 1804 | 4 | 9 | 2 | game | cdecl | FUN_0074a1c0 |  |
| 39 | `007b6d00` | 3174 | 0 | 2 | 1 | script | thiscall | FUN_007b6d00 (NsUIScreen_ScriptType) |  |
| 40 | `005aedb0` | 482 | 63 | 10 | 2 | script | thiscall | FUN_005aedb0 |  |
| 41 | `004358e0` | 1158 | 6 | 6 | 1 | game | cdecl | FUN_004358e0 |  |
| 42 | `00727100` | 1530 | 3 | 10 | 1 | game | cdecl | FUN_00727100 |  |
| 43 | `00447130` | 2699 | 1 | 15 | 2 | formats | cdecl | FUN_00447130 |  |
| 44 | `005af760` | 519 | 53 | 6 | 2 | script | thiscall | FUN_005af760 |  |
| 45 | `0056ec90` | 4269 | 1 | 18 | 5 | script | thiscall | FUN_0056ec90 |  |
| 46 | `004e9610` | 718 | 16 | 1 | 1 | physics | thiscall | FUN_004e9610 |  |
| 47 | `004ca460` | 3138 | 1 | 9 | 3 | script | thiscall | FUN_004ca460 |  |
| 48 | `00445ba0` | 3093 | 1 | 21 | 3 | script | fastcall | FUN_00445ba0 |  |
| 49 | `005b0b70` | 486 | 55 | 7 | 2 | script | thiscall | FUN_005b0b70 |  |
| 50 | `0053e320` | 3080 | 1 | 26 | 3 | game | cdecl | FUN_0053e320 |  |
| 51 | `004cbd30` | 3074 | 1 | 9 | 3 | script | thiscall | FUN_004cbd30 |  |
| 52 | `00773540` | 1280 | 4 | 3 | 1 | render | cdecl | FUN_00773540 |  |
| 53 | `00726690` | 2500 | 1 | 8 | 2 | game | cdecl | FUN_00726690 |  |
| 54 | `005b8100` | 1736 | 3 | 15 | 2 | formats | thiscall | FUN_005b8100 |  |
| 55 | `0049cd40` | 193 | 205 | 1 | 1 | game | thiscall | FUN_0049cd40 |  |
| 56 | `005e6d40` | 1583 | 2 | 13 | 1 | game | thiscall | FUN_005e6d40 (CombatCamera) |  |
| 57 | `0066bcc0` | 1208 | 7 | 23 | 2 | script | thiscall | FUN_0066bcc0 |  |
| 58 | `005d1cd0` | 1578 | 2 | 21 | 1 | scene | cdecl | FUN_005d1cd0 | Loaded unit: %s |
| 59 | `00549ef0` | 1561 | 2 | 10 | 1 | fx | fastcall | FUN_00549ef0 |  |
| 60 | `005a3960` | 697 | 14 | 8 | 1 | script | thiscall | FUN_005a3960 (cdc::ScriptUIScreen / cdc::UIBitmap / cdc::UIButton / ...) |  |
| 61 | `00703b50` | 2352 | 1 | 16 | 2 | game | fastcall | FUN_00703b50 |  |
| 62 | `0049d720` | 307 | 73 | 1 | 1 | game | thiscall | FUN_0049d720 |  |
| 63 | `00567620` | 935 | 11 | 13 | 2 | script | thiscall | FUN_00567620 |  |
| 64 | `00471c90` | 2288 | 1 | 11 | 2 | physics | cdecl | FUN_00471c90 |  |
| 65 | `005f53b0` | 1594 | 3 | 7 | 2 | game | thiscall | FUN_005f53b0 |  |
| 66 | `004de560` | 1801 | 1 | 1 | 1 | physics | thiscall | FUN_004de560 |  |
| 67 | `005088a0` | 2216 | 1 | 23 | 2 | render | thiscall | FUN_005088a0 |  |
| 68 | `00409970` | 1188 | 9 | 2 | 3 | game | cdecl | FUN_00409970 |  |
| 69 | `00518970` | 1113 | 4 | 9 | 1 | fx | stdcall | FUN_00518970 |  |
| 70 | `0049b650` | 292 | 70 | 0 | 1 | cine | cdecl | FUN_0049b650 |  |
| 71 | `006b25f0` | 1737 | 1 | 12 | 1 | game | thiscall | FUN_006b25f0 |  |
| 72 | `00706770` | 1725 | 1 | 6 | 1 | game | cdecl | FUN_00706770 |  |
| 73 | `005ebd90` | 1408 | 2 | 3 | 1 | game | thiscall | FUN_005ebd90 |  |
| 74 | `005afae0` | 469 | 41 | 4 | 2 | script | thiscall | FUN_005afae0 |  |
| 75 | `0059f9d0` | 2431 | 0 | 4 | 1 | script | thiscall | FUN_0059f9d0 (cdc::UISlider) |  |
| 76 | `006ebef0` | 799 | 8 | 5 | 1 | game | cdecl | FUN_006ebef0 |  |
| 77 | `007020e0` | 831 | 7 | 4 | 1 | game | cdecl | FUN_007020e0 |  |
| 78 | `006a5dd0` | 497 | 21 | 5 | 1 | game | cdecl | FUN_006a5dd0 |  |
| 79 | `00464b40` | 2045 | 1 | 6 | 2 | game | fastcall | FUN_00464b40 |  |
| 80 | `005a3030` | 1146 | 3 | 9 | 1 | script | thiscall | FUN_005a3030 (cdc::UIModel) |  |
| 81 | `005b1490` | 584 | 23 | 6 | 2 | script | thiscall | FUN_005b1490 |  |
| 82 | `0047be90` | 1964 | 2 | 5 | 3 | collision | cdecl | FUN_0047be90 |  |
| 83 | `006b7710` | 2278 | 2 | 30 | 4 | physics | thiscall | FUN_006b7710 |  |
| 84 | `00635150` | 1260 | 4 | 3 | 2 | script | cdecl | FUN_00635150 |  |
| 85 | `00437ba0` | 2383 | 1 | 3 | 3 | game | thiscall | FUN_00437ba0 |  |
| 86 | `005a2660` | 1119 | 3 | 5 | 1 | script | thiscall | FUN_005a2660 (cdc::ScriptUIScreen / cdc::UIBitmap / cdc::UIButton / ...) |  |
| 87 | `005b7720` | 2235 | 0 | 8 | 1 | formats | thiscall | FUN_005b7720 (MonitorDrawable / cdc::InstanceDrawable) |  |
| 88 | `004714d0` | 1919 | 2 | 12 | 3 | physics | cdecl | FUN_004714d0 |  |
| 89 | `007128e0` | 1911 | 2 | 18 | 3 | game | cdecl | FUN_007128e0 |  |
| 90 | `007b5490` | 2200 | 0 | 2 | 1 | script | thiscall | FUN_007b5490 (NsLaraBase_ScriptType) |  |
| 91 | `00653bb0` | 319 | 46 | 4 | 1 | game | thiscall | FUN_00653bb0 |  |
| 92 | `006a3790` | 1927 | 1 | 14 | 2 | game | thiscall | FUN_006a3790 |  |
| 93 | `004edeb0` | 820 | 10 | 7 | 2 | physics | thiscall | FUN_004edeb0 |  |
| 94 | `00646870` | 961 | 7 | 4 | 2 | script | cdecl | FUN_00646870 | ballistic |
| 95 | `006292d0` | 1082 | 3 | 12 | 1 | game | cdecl | FUN_006292d0 |  |
| 96 | `005ab9e0` | 1902 | 1 | 21 | 2 | formats | cdecl | FUN_005ab9e0 |  |
| 97 | `004cee50` | 1890 | 1 | 13 | 2 | script | thiscall | FUN_004cee50 |  |
| 98 | `00621cf0` | 478 | 19 | 11 | 1 | game | cdecl | FUN_00621cf0 |  |
| 99 | `0046b9c0` | 745 | 7 | 2 | 1 | physics | cdecl | FUN_0046b9c0 |  |
| 100 | `00623fb0` | 1172 | 4 | 16 | 2 | script | cdecl | FUN_00623fb0 | PanicDive |

Changes against the first pass: entries that were cited since (by the audit docs) left the list (e.g. the former
#1 0x43F7F0 and #2 0x46E460, now Named-only), and the root set shrank to 1 302 because new doc citations moved a
few cited functions' majority area outside the root areas; resolved virtual edges add callers to UI script
classes (`cdc::ScriptUIScreen` family, now #1 and #60 with only virtual callers, and #86) and pull in
`CombatCamera` slot 0x5E6D40 (2 virtual callers, no direct one).

## Verification sample (instruction level)

Bodies are bounded by the Ghidra function sizes; listings are `objdump -d -M intel` of the original `tru.exe`
(first pass in `openunderworld-work/re-baseline/FunctionDb/asm/`, second pass in
`openunderworld-work/re-baseline/FuncDetail/asm/`). "Agrees" means the instructions show what the citing text
claims; decompiler output was not used as proof. The 22 functions below carry `confidence = high`.

### First pass

| Address | Repo claim (citation) | Instructions | Verdict |
|---|---|---|---|
| 0x5DA230 `GAMELOOP_UpdateTime` | 6-frame moving average of the frame delta, clamp 66 666 µs (docs/re/TIMING.md:51) | calls 0x597F10; 6-entry ring at 0xE7EE10 indexed `mod 6` (`idiv 6`), 5-iteration sum; `cmp $0x1046a` / `mov $0x1046a` (= 66 666) | agrees |
| 0x5E0190 `MainG2` | loop `MAIN_ProcessMainState(0xE7ED60, 0xE7F088)`; `STREAM_PollLoadQueue`; `MainG2_PlatformUpdate` until 0xE7ED6C set (docs/re/STARTUP.md:40) | `push $0xe7f088; push $0xe7ed60; call 0x5e0090`; `call 0x5c1da0`; `call 0x47afd0`; `cmpl $0,0xe7ed6c; je` loop; also leaves when 0x47AFD0 returns 0 | agrees (the second exit, platform update returning false, is also in docs as "returns false on quit") |
| 0x597E90 `TIMER_SampleMicroseconds` | QPC × 1 000 000 / QPF (docs/re/TIMING.md:12) | two IAT calls (QPC/QPF), `push $0xf4240` into `__allmul` 0x8145B0, then `__alldiv` 0x816940 | agrees |
| 0x69DA80 sea waves | Σ hᵢ(sin(…) − 1) via CRT sin 0x813680 (docs/GAMEPLAY.md:1671, CHANGELOG 0.50.0) | four `call 0x813680`; 0x813680 = MSVC `_CIsin` shape (SSE2 dispatch `jmp 0x81cb40`, x87 path `fsin` at 0x8136ED) | agrees; FID did not name 0x813680 |
| 0x4EFB20 body integration | `v *= 1 − dt × damping`, damping at body +0x154 / +0x158 (src/physics/Physics.cpp:731) | `fld1`, `fsub` of `dt × [edi+0x154]`, clamp to [0, 1], `mulps` into linear velocity +0xA0; same with `[edi+0x158]` for angular | agrees (the factor is clamped to 0..1; irrelevant for 0.01/frame) |
| 0x57CE50 SCMD interpreter | switch on the opcode, `case = op − 1` (CHANGELOG, docs/ROADMAP.md:53) | opcode = word >> 24, per-opcode counter at 0xCC4B58, `add $-1`, `cmp $0x84; ja`, `jmp *0x57e52c(,%eax,4)` (133 cases) | agrees |
| 0x6F1790 fall-damage curve | 0 up to t1, linear to 500 at t2, to 1000 at t3 (docs/GAMEPLAY.md:262) | pure x87 piecewise-linear of its stack args: above t3 → max, t2..t3 and t1..t2 lerps with a 0.5 × max knee (constant 0x8413C0), below t1 → 0 | agrees (the +0x4C/+0x50/+0x54 reads are in the caller) |
| 0x7594C0 wall-jump check | returns 0 when the query has flag 0x80, else 1/2 (tests/selftests/chimney_floor_jumps.json:5, docs/GAMEPLAY.md:2868) | `testb $0x80,0x2c(%eax)` → `xor %eax,%eax; ret`; exits `mov $2` and `mov $1` | agrees |
| 0x5570F0 Save Game | buffers every live instance via 0x5568C0 (docs/GAMEPLAY.md:3935) | `call 0x5568c0` then 0x555BE0 | agrees |
| 0x6AD070 water volume update | gathers primitives via 0x4DEE60 (docs/GAMEPLAY.md:1681) | contains `call 0x4dee60` | agrees |
| 0x473840 `InitArchive` | sets the archive spec mask to 1, opens BIGFILE (docs/re/FILESYSTEM.md:21, docs/formats/CONTAINERS.md:42) | `call 0x4737f0` (MultiFileSystem ctor), `movl $0x1,0x8(%eax)`, `call 0x4730b0` (`ArchiveFileSystem_Open`), `ERROR_Fatal` 0x596CB0 on failure | agrees |

### Second pass (camera, climbing, wall jump, combat, AI, save, UI)

| Address | Repo claim (citation) | Instructions | Verdict |
|---|---|---|---|
| 0x5E2950 camera FOV | FOV mode 0 spline FOV, 1 towards fovTo, 2 back from fovFrom (src/game/Camera.cpp:40) | thiscall; `mov eax,[esi+0x19C]`, `cmp eax,1` → t = `[esi+0x194] / [esi+0x198]` clamped to ≤ 1 (`fld1`/`fcom`), lerp from `[esi+0x190]`; `cmp eax,2` → same t via helper 0x5E26B0; else the spline value | agrees |
| 0x5E2CA0 camera done | dispatchers are told once, the camera holds its pose (src/game/Camera.cpp:220) | guard `cmp byte [esi+0x1A0],0; jne ret`; either CameraManager (getter 0x5E7770) slot 20 `call [edx+0x50]` with (1, 0) or `0x775A30` + dispatch `0x77A3D0` on `[0xFAE874]`; then `mov byte [esi+0x1A0],1` | agrees (one-shot flag +0x1A0) |
| 0x5D6F00 markup validation | 0x8000 (free-climb) markup is disabled unless it has exactly 3 points (src/game/Climb.cpp:81) | stdcall `ret 4`; `eax = [[ecx+0x58]] − 1`; tail: `test edx,0x8000; je; cmp eax,2; je` else `or edx,0x80000000` into `[ecx+0x28]` | agrees; caveat: with flag 0x8000000 also set the count is decremented once more first |
| 0x707C60 hold → root | Lara's root at hold point 0 − 158.025 z (src/game/Climb.cpp:81) | copies a 16-byte point into `[esi]`, `fld [esi+8]; fsub qword [0x85DFC8]` (= 158.025); also uses 237.0375 (0x85DFC4, the wide-stance step) and 31.605 | agrees |
| 0x5D3BA0 wall face test | a terrain face with client flag 0x80 is no wall (src/game/WallJump.cpp:45) | type 3: `movzx ecx, byte [face+0xE]; and ecx,0x80`; type 5: `[[+0x18]+4]+0x30 & 0x10`; `test cl,0x90` → 0 else 1 | agrees (plus a type-5 case on flag 0x10 the comment does not mention) |
| 0x63C9B0 hit message | hit message 0x11C built by 0x63C9B0, +0x50 damage class, +0x64 damage type (src/game/Combat.hpp:55) | `push 0x80; call 0x5C0B10` (allocate), stores args into +0x30..+0x72 incl. `[eax+0x50]` and `[eax+0x64]`; caller 0x5D3C90: `call 0x63c9b0; push eax; push 0x11c; push esi; call 0x5b3750` | agrees (the id is pushed by the caller) |
| 0x5D9E50 difficulty index | returns 1 (src/game/Combat.hpp:35, docs/GAMEPLAY.md:4051) | `fld [0x8AB648]; call 0x814D20` (float → int), `cmp eax,3; jne; mov eax,1`; 0x8AB648 holds 1.0f in the file and has no direct writer | agrees: returns (int) 1.0f = 1 (3 would also map to 1) |
| 0x60EF50 / 0x617EE0 enemy ragdoll | ragdoll when fewer than 6 are live (0x617EE0 → 0x60EF50 → 0x4F1920) (src/game/ai/Enemy.cpp:1129) | 0x60EF50: `cmp dword [0xE91FE0],6; jge fail`, `call 0x4f1920`, on success `mov [eax*4+0xE91FE4],esi; inc` count; 0x617EE0: thiscall `ret 4`, skips when `[esi+0x374]` bit 8 or 9, `call 0x60ef50`, `or [esi+0x374],0x100` | agrees |
| 0x5DB6A0 collect reward | sets bit n of the saved set at +0x3ED90 and counts found at +0x3EE90 (src/game/Progress.hpp:86) | `shl edx,cl`; `test [ecx+eax*4+0x3ED90],edx` / `or [ecx+eax*4+0x3ED90],edx`; `add dword [edx+ecx*4+0x3EE90],1`; `call 0x5d5f80` | agrees |
| 0x476D80 key-name colour | sets 0x7D6932 with alpha kept (src/ui/Ui.cpp:96) | on char 0x1B (`cmp eax,0x1b`), toggles a flag, `and ecx,0xff000000; add ecx,0x7d6932; mov [0x9CE6C0],ecx` | agrees |

Not confirmed this pass: `src/game/Grapple.cpp:23` says 0x746F60 reads player-data +0x9C8 / +0x9CC; no
displacement 0x9C8 or 0x9CC occurs in 0x746F60's body (the reads may go through a helper or a different base).
Left as an open check, not a disagreement.

Disagreements and naming notes from the first pass:

- 0x67ED20 is slot 7 of `EnemyState_RegenerateDeath`'s vftable; `src/game/ai/Enemy.hpp:87` and
  docs/GAMEPLAY.md call it "EnemyRegenerateData". The string `EnemyRegenerateData` is referenced by the
  neighbouring 0x67ECF0 and 0x682460, so the repo's label is the record's type name, not the function's.
  The role is not contradicted. Applied as `EnemyState_RegenerateDeath_vf7`.
- 0x61DD10 is cited as "InvestigateStart" by name extraction, but it is slot 13 of
  `EnemyBehavior_Investigate` (src/game/ai/Enemy.cpp:233 describes it as the message-0xBF handler);
  `InvestigateStart` is a separate class (`EnemyBehavior_InvestigateStart`).
- 0x417F30 is cited as bfx `CreateLinkStrip`; the only `CreateLinkStrip` string reference is in 0x4084C0, so
  the name is applied at medium confidence.

## Names in `tools/ghidra/names/tru_gog.csv`

First pass: 24 rows (12 high, 12 medium) — `PCStateManager_SetBlendMode` 0x527A80, `INSTANCE_IntroduceInstance`
0x5BCC30, `UberObjectComposite_ctor` 0x4D7D50, `UberObjectSection_ModelMarkerAnchor_ctor` 0x4D3110, the bfx
NavGraph/link functions, `AutoSplineCamera_Start` 0x5E26F0 / `_vf1_Update` 0x5E2D90, `ScriptNative_autosplinecamera`
0x7759F0, `PlayerSC_Grapple_OverhangDetected` 0x73AB70, enemy behaviour/state slots, `UberObjectSection_TR8_vf2`,
`PCRenderLight_BuildRegisters` 0x50B910, `UberObject_AttachTo` 0x4A4490.

Second pass: 11 rows from the verification above (8 high, 3 medium; the medium ones carry an [I] class prefix or
whole-function role): `GetDifficultyIndex` 0x5D9E50, `Markup_DisableBadPointCount` 0x5D6F00,
`FreeClimb_RootFromHold` 0x707C60, `WallJump_FaceIsWall` 0x5D3BA0, `HitMessage_Build` 0x63C9B0,
`Enemy_TryStartRagdoll` 0x60EF50, `Enemy_DeathRagdollOnce` 0x617EE0, `Rewards_Collect` 0x5DB6A0 (high);
`Text_DrawKeyHighlight` 0x476D80, `AutoSplineCamera_UpdateFov` 0x5E2950, `AutoSplineCamera_NotifyDone` 0x5E2CA0
(medium). No FID name was added or applied.

Applied with `ApplyNames.java` (headless, `-noanalysis`, saved): **179 functions, 23 labels, 0 errors** (was
168 / 23). The CSV now has 202 rows. `ExportGraph` was re-run afterwards, so `ghidra_name` carries the names.

## Open questions

- 1 431 library tags still rest on the address-run fill; FID confirmed 4 and contradicted none, because the
  shipped signature sets hold no FMOD Ex or NavPower code. An FMOD Ex 4.x / VS2005 signature set would settle the
  fmod run; the bfx run (0x401000..0x42xxxx) needs NavPower signatures or string anchors.
- libjpeg: locate its functions (the error-message table at 0x85B7E4.. is referenced through a pointer table).
- Virtual calls: 93 % of vptr call sites stay unresolved; the biggest group (5 497) loads the object from a
  member field. Field types from StructDb (`docs/re/db/structures.tsv`) plus constructor stores into
  `[this+k]` would resolve many; so would following the `new`-then-null-check merge (MSVC
  `test eax,eax; je` around constructor calls) instead of stopping at it.
- 2 000 non-library functions have no caller, no pointer reference, and no vftable slot: candidates for jump-table
  targets inside other functions, functions reached only through computed pointers, or dead code.
- `cc = thiscall` is ECX live-in; separating real methods from LTCG register-passing functions needs the
  vftable/constructor evidence above or decompiler parameter recovery.
- The `this_offsets` tracking is address-order and optimistic at merges; StructDb should treat it as candidates.
