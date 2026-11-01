# Multi-row GPU chooser: host suite

This suite tests the plugin's multi-row chooser (multi-row GPU plan, slice MR6: plugin 1.4 on engine 1.18; cells
10.6, 7.11 (a)-(c), N4, MX13, MX13b, MX22; rulings 37 and 39) on Linux with g++ and clang, with no Unreal and no
engine.

| Under test | Where |
|---|---|
| The chooser: the µs conversion (rev 1.3 F4), C(M) and G(M) from the table, ℓ(M), D(M), D_x(M), M_max, the Composed (M, ℓ, units) choice, M_exact over 4..8 (MR-D11) | `Source/SuperSLMUnreal/Private/Core/SuperSLMGpuRowsChooser.h` |
| Exact as a setting: runs at max M ÷ D_x ≤ 1 (7.11 a), runs as Off without a device head (7.11 b, ruling 26 Q2), plans nothing with the setting off (7.11 c) | same |
| The table's provenance (N4, ruling 39 Q5): flagged in development, the one-row plan in a shipping build | same |
| Ruling 39: the scene inside every slice (Q1), a budget below one row (Q2), Exact's fall to 3, 2, then Off (Q4), `bGpuMultiRow` off as the one-row plan (P4.4's chooser half) | same |
| Erratum E-13, ruling 48: the load-time row-cost calibration's pure pieces (the price is the max of the timed runs rounded up to 0.1 ms, the [1.24 ms, 124 ms] sanity range, retry once then fall back, the game value over the switch over the measurement); `test_calibration.cpp`, red on its stubs, and built alone under the strict flags (`multirow_calibration_strict`) | `Source/SuperSLMUnreal/Private/Core/SuperSLMGpuRowCostCalibration.h` |
| Erratum E-14, ruling 53: l(1) carries the margin with the knob set (l(1) = L at B = 0); the row cost as a line in position (two positions, P2 = min(256, cap − 8), C(1, pos) rounded up, sanity on C_a and at the cap, the game value and the fallback flat); `ChooseRowsPlanAt` (a slice priced at its end position). Ruling 54: calibration in two phases. Phase 1 (the early position only; the first generation waits for it) gives C_a + s_default x (pos - pos_a), s_default = 0.0015 x C_a per position, source Calibrated with the line marked default slope; phase 2 (B > 0 only, lowest priority, no timeout) measures P2 and replaces s_default, kept on a failure or a line out of sanity. The line carries its source (flat, default slope, measured); `StepTwoPositions` splits into `StepPhase1` and `StepPhase2`. Red on their stubs | both headers |
| Ruling 56 (1): the finish on the slice that ends a token, priced l x (C(1) - F) / L + F with the margin on the whole; F = ceil(g(1) x C(1) / 1000) when not measured (`DefaultFinishUs`, `FinishSliceScaledPrice`, `FinishSliceLayers`). Red on their stubs | `SuperSLMGpuRowsChooser.h` |
| Ruling 42 R2: an Exact schedule of D + 1 ticks counts only when one layer plus the logits stage fits (the engine has no logits-only tick), so Q4's ladder runs before Off | same |

**This is a red suite.** `SuperSLMGpuRowsChooser.h`'s bodies are stubs, so every chooser test fails on the stub's
value. The implementation replaces them; the interface is the contract and may be added to, not taken from. The header must
also build alone under `-Wconversion -Wsign-conversion -Wundef -fno-exceptions -fno-rtti` (target
`multirow_chooser_strict`).

## The reference and the grid

`chooser_ref.py` is 10.6's independent exact-rational reference, written from the plan's text and ruling 39 in Python
`fractions`. It checks itself against the plan's worked figures (§4.7's table, §5.2, §5.3, F4's conversions) and
ruling 39's (Q1, Q2, Q4) on every run, and writes `grid.txt`: 3,649 cases, of which 3,520 are the full grid
(five tables: the MR4 estimate, two §4.7 spike tables, the estimate with g = 48 ‰, and ruling 37's measured merged
table; L ∈ {24, 28}; eleven budgets including 0, three below one row, 6.2005 and one above `NominalTickMs`; scene
0 and 1.5 ms; m ∈ {0, 150, 300, 2000}; `LayersPerTick` ∈ {1, 2, 4, 8}), plus N4/P4.4 cases (provenance, shipping
build, `bGpuMultiRow`) and a `GpuRowCostMs` = 6.6 sweep. The chooser must equal it case for case.

`suite_model.h` is a second, C++ reading of the same text with the three chooser mutants as switches. The suite
requires the unmutated model to equal the grid and each mutant to differ from it somewhere, so the grid's power over
MX13, MX13b and MX22 is shown before the chooser exists.

Every field is graded. A one-row plan (multi-row off) is graded on the fields that plan has; the Exact reason is
ungraded only when Exact is chosen under the one-row plan (48 cases).

## Commands

    cmake -S ci/host/multirow -B out/multirow-host && cmake --build out/multirow-host -j
    (cd out/multirow-host && ctest --output-on-failure)
    python3 ci/host/multirow/chooser_ref.py check ci/host/multirow/grid.txt
    python3 ci/host/multirow/chooser_ref.py grid > ci/host/multirow/grid.txt    # after a deliberate reference change

`ci/tests/test_mr6_plugin_1p4.py` holds the tree-level MR6 checks: version 1.4; engine 1.18 vendored (or SuperSLM
277682d4 until the tag) with the measured table, read through its merged arrays only; ruling 39 Q9's knobs; the trace
counters; Canonical removed (errata E-11, ruling 39 Q7 and Q10). The four automation cells (P4.4, 7.12, 8.5, 9.4) are
`Source/SuperSLMUnrealEditor/Private/Tests/SuperSLMGpuMultiRowMr6Tests.cpp`, over the seam
`Public/SuperSLMGpuMultiRowTestSeam.h`.
