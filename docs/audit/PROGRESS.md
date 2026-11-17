# Engineering progress log (master brief)

A dated entry per batch (execution rule 25). Requirement status lives in [BRIEF_COMPLIANCE.md](BRIEF_COMPLIANCE.md);
work item IDs (W*-*) come from its §16.

## 2026-11-16 — batch 1: Phase 0 + Phase 1 baseline

- **Snapshot:** branch `baseline/pre-audit-2026-11-16` at v0.51.0 (9034839), pushed. The tree was clean.
- **Delivered:**
  - [PHASE01_REPORT.md](PHASE01_REPORT.md), [PROJECT_AUDIT_2026-11.md](PROJECT_AUDIT_2026-11.md),
    [PROTECTED_SYSTEMS.md](PROTECTED_SYSTEMS.md), [BASELINE_2026-11.md](BASELINE_2026-11.md),
    [BUGS.md](BUGS.md), [EXTERNAL_RESEARCH_2026-11.md](EXTERNAL_RESEARCH_2026-11.md).
  - [../re/BINARIES.md](../re/BINARIES.md), [../re/FUNCTION_DATABASE.md](../re/FUNCTION_DATABASE.md),
    [../re/ENGINE_ARCHITECTURE.md](../re/ENGINE_ARCHITECTURE.md).
- **Baseline:** ctest 980 passed, 0 failed, 17 skipped (997 selected; `game_vulkan_rt_smoke` excluded).

## 2026-11-16 — batch 2: Phase 0/1 gaps

- **Delivered:**
  - **Per-function detail:** for all 18,766 functions: calling convention, parameters, frame size, globals,
    constants, this-offsets, confidence. Plus FID, 644 resolved virtual-call sites, xref, string-ref and
    module-dependency reports, and 10 more assembly checks (21 in total, all agree).
    ([FUNCTION_DATABASE.md](../re/FUNCTION_DATABASE.md))
  - **Data-structure inventory:** 1,015 RTTI types with their hierarchy, sizeof for 115 classes,
    1,159 structure fields (20 of them checked against the assembly: 18 match, 1 partial, 1 mismatch),
    and 11,650 globals. ([STRUCTURES.md](../re/STRUCTURES.md))
  - **Original frame pipeline:** with the top-level call order checked against the assembly.
    ([RENDER_PIPELINE.md](../re/RENDER_PIPELINE.md))
  - **Bug reproduction pass:** [BUGS.md](BUGS.md), 89 entries.
  - **Requirement-by-requirement compliance matrix:** with the Phase 2–11 queue.
    ([BRIEF_COMPLIANCE.md](BRIEF_COMPLIANCE.md))
- **Running:** ScriptNatives (script native binding table under Wine).

## 2026-11-16 — batch 3: Phase 2–11 queue, wave 1 (running)

| Agent | Items |
|---|---|
| SelfTestBrief | W8-1 platform key, W8-4 engine scenario, W8-3 HUD/inventory/audio/camera probes, W8-6 save/load steps |
| AoGolden | W8-2 AO regression tests, W8-5 golden images, W10-4 `protected` ctest label |
| FormatDocs | W2-1 UI.md, W2-2 endianness + validation in every format doc, W2-3 LIGHTING.md |
| DocDrift | W11-3 doc drift, W4-2 matrix refresh, W11-1 LIMITATIONS.md |
| RoundTrip | W2-4..W2-7 real-data round trips and ou-asset inspection subcommands |
| WineMeasure | W9-2 versioned Wine harness, W3-1 rest depth, W3-9 camera, W3-5 swimming |
| Bench | W10-2 deterministic benchmarks, W10-1 extended performance baseline |

Engine fixes (W6-*) wait for this wave's evidence (W3-*, ScriptNatives).
