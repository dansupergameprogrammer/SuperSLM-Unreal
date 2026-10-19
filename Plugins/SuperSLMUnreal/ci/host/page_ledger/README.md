# Page ledger host suite

A host-only C++17 suite for the plugin's page ledger core
(`Source/SuperSLMUnreal/Private/Core/SuperSLMPageLedgerCore.{h,cpp}`). It needs no Unreal and no engine. The core
contains only bookkeeping:
- regions S/P/H/A/C (plugin paging plan §3.2), and the region-C seam `ISuperSLMCacheRegionCharge`;
- charge at admission and settle at apply;
- ghosts and the A pre-charge;
- the guaranteed-return wait queue (§3.5);
- create-first normalization;
- cancellation of every wait at epoch end.

**State: GREEN.** The core implements the header's contract, two caps included (UP-15 (c)), with the
`ContextCapMismatch` outcome on restores. All 73 tests pass on every build below, and every armed mutant is killed.

## Build and run

```sh
# from this directory
cmake -S . -B build/gcc   -DCMAKE_CXX_COMPILER=g++     && cmake --build build/gcc   && ./build/gcc/page_ledger_tests
cmake -S . -B build/clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/clang && ./build/clang/page_ledger_tests

# ASan + UBSan (needs the compiler's sanitizer runtime; on Linux g++ ships it)
cmake -S . -B build/san -DCMAKE_CXX_COMPILER=g++ -DPAGE_LEDGER_SANITIZE=ON && cmake --build build/san && ./build/san/page_ledger_tests

# UBSan in trap mode, for a clang without compiler-rt installed (no runtime needed)
cmake -S . -B build/trap -DCMAKE_CXX_COMPILER=clang++ -DPAGE_LEDGER_UBSAN_TRAP=ON && cmake --build build/trap && ./build/trap/page_ledger_tests

./build/gcc/page_ledger_tests cell_3_2     # run only the tests whose name contains the filter
```

Compiler flags:
- The core builds as a static library with `-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
  -fno-exceptions -fno-rtti` and `SUPERSLM_PAGE_LEDGER_TEST_HOOKS=1`.
- The tests build with `-Wall -Wextra -Werror -fno-rtti`. They share the core's no-RTTI setting so that UBSan's
  vptr check links.

The runner prints one line per test, naming the plan cell it grades, and the first failing check with its
file:line. It exits 1 on any failure. `ctest` runs it as `page_ledger_tests`.

## Counts (against the shipped core)

| Build | Passed | Failed | Total |
|---|---|---|---|
| g++, `-Werror` | 73 | 0 | 73 |
| clang++, `-Werror` | 73 | 0 | 73 |
| g++ ASan + UBSan | 73 | 0 | 73 |
| clang++ UBSan (trap) | 73 | 0 | 73 |

No sanitizer report on either sanitized build. With `SUPERSLM_PAGE_LEDGER_TEST_HOOKS=0`, the core compiles
warning-free with `-Wpedantic` on g++ and clang++, at `-std=c++17` and `-std=c++20`, and that object carries no
test symbol. Every mutant and under-charge site goes through two file-local macros that reduce to "no mutant" and
"the full charge" when the hooks are off. The macros are `#undef`'d at the end of the file.

**Unity build.** The core also compiles in one translation unit with the five other cores in its directory, with
the hooks on and off, on both compilers. This is how Unreal's unity build may compile them. Before the two-cap
round, g++ refused it (`-Wsubobject-linkage`): `FPageLedger::FImpl` has external linkage, and it held members
whose types were in an anonymous namespace. Those types now live in a named namespace,
`SuperSLMPaging::PageLedgerState`. The check was run by hand. This suite's CMake does not build it, and the prefix
cache suite's `unity_cores.cpp` does not include the ledger core.

The history of red counts:
- The stub bodies ran 0 of the original 51.
- The two-cap red legs ran 55 of 69. Eleven tests were red for the engine cap, and three for mutant switches
  that were not implemented yet.
- The legs for the private-holder figure, budget 0 and `ContextCapMismatch` run **69 of 73** against the two-cap
  core, on all four builds below. Two tests are red because the core has no `ContextCapMismatch` outcome:
  `cell_2_1_two_cap_origin_bound` and `cell_2_6_two_cap_budget_bound`. Two hold on the correct path and are red
  only because a new mutant switch is not implemented: `cell_4_4_private_holders_per_block`
  (`HoldSettledOnePageSmaller`) and `cell_2_1_budget_zero` (`BudgetDomainFromZero`). `cell_2_6_two_cap_prefix_length_bound`
  is green on arrival. All 73 pass against a scratch implementation of the declared contract (not shipped), and
  every new mutant is killed there.
- The implementation's core then ran **73 of 73** on all four builds.

Each red test failed on its own first assertion, never on a crash or a sanitizer report.

### Mutant results

Every mutant the suite arms is killed. `LT_KILLS` passing means the named check failed with the mutant armed, and
the paired `LT_HOLDS` passed without it.

| Mutant | Killed by |
|---|---|
| `OneLate`, `OneEarly` (limit) | `limit_token_law_exact_counts` |
| Under-charge `PrefixBuild` (ceil(L/B) for R(L)) | `cell_1_3_prefix_undercharge_boundary` (and trips `cell_11_2_ledger_fault_detector`) |
| Under-charge `NestedBuild` (charged 0) | `cell_1_3_nested_undercharge_boundary` |
| Under-charge `BudgetRestore` (without E) | `cell_1_3_restore_without_e_boundary`, at B = 16 and B = 64 |
| Under-charge `WholeReserveRestore` (SSB5 charged R(b)) | `cell_1_3_ssb5_undercharge_boundary` |
| `RestoredSharerNotRegistered` | `cell_1_3_restored_sharer_not_registered_boundary`, `invariant_random_workload` |
| `PreChargeSkipped` | `cell_1_3_precharge_skipped_boundary`, `cell_8_3_eviction_with_adopters_zero_slack`, `invariant_random_workload` |
| `PrivateRestoreRefundedAtApply` | `cell_1_3_private_restore_refunded_at_apply_boundary`, `invariant_random_workload` |
| `RefundAtEnqueue` | `cell_1_5_enqueue_window`, `invariant_random_workload` |
| `FailedRestoreKeepsCharge` | `cell_1_6_failures_refund_exactly`, `cell_1_9_gpu_twins`, `invariant_random_workload` |
| `GhostReleasedAtFirstDependent` | `cell_1_7_ghosts`, `invariant_random_workload` |
| `DependencyKeptAtReadopt` | `cell_1_1_lifecycle_cycles`, `invariant_random_workload` |
| `ShareCountedAsGuaranteed` | `cell_3_2_d_constructions_vs_probe_model` (it also reproduces the model's own mutant: 7 short waiters, 9 for SSB5), `wait_every_wait_ends_in_admission_random` |
| `WaiterAdmittedWithoutGuarantee` | `cell_3_2_ab_wait_on_guarantee_cpu`, `cell_3_2_c_wait_on_guarantee_gpu_twin` |
| `WaitersSurviveEpochEnd` | `cell_1_11_epoch_end_resolves_waits` |
| `LedgerFaultNotCounted` | `cell_11_2_ledger_fault_detector` |
| `NormalizationUncharged` | `cell_1_8_normalization_create_first` |
| *Two caps and the coverage re-audit (UP-15 (c), §7):* | |
| `NormalizationTakesHeadroomWhileWaiting` | `normalization_yields_to_waiting_restores` |
| `FlatGuarantee` | `cell_3_2_f_guarantee_capped_by_charge` |
| `EmptyChildSettledToZero` | `cell_4_3_empty_child_on_b_and_b_plus_1` (at the B + 1 parent, both shapes) |
| `RPricedAtContextCap` | `two_cap_cell_1_2_zero_slack` (both kinds: the warm creates exhaust; the prefix's charge < drawn in flight), `two_cap_cell_1_3_prefix_r_boundary` |
| `EPricedAtContextCap` | `two_cap_cell_1_3_e_boundary` |
| `BlocksAtContextCap` (option (b)) | `two_cap_configure_regions`, `two_cap_cell_1_2_zero_slack`, `two_cap_cell_4_3_cap_minus_one_prefix`, `two_cap_cell_4_4_headroom_burst`, `two_cap_cell_4_5_largest_restores` |
| `BlocksAtEngineCap` (option (a)'s blocks) | `two_cap_configure_regions` (P/A 2,048), `two_cap_cell_4_4_headroom_burst` (10 / 0 / 0) |
| `ValidationAtEngineCap` | `cell_2_1_two_cap_origin_bound`, `cell_2_6_two_cap_budget_bound`, `cell_2_6_two_cap_prefix_length_bound` |
| `LimitAtEngineCap` | `two_cap_cell_4_2_limit_at_context_cap` |
| `ContextCapMismatchReportedAsInvalid` | `cell_2_1_two_cap_origin_bound`, `cell_2_6_two_cap_budget_bound` |
| `BudgetDomainFromZero` | `cell_2_1_budget_zero` (SSB6 and SLM6) |
| `HoldSettledOnePageSmaller` | `cell_4_4_private_holders_per_block` (form 1 at the first apply on every shape; at CPU 1.5B it also admits 35 holders, not 33) |
| *Driver mutants (the test's own model of the queue):* admission at queue time; the queue narrowed to 1 | `cell_3_5_one_job_per_slot` |

Three source mutants have no switch in the header. They were applied by hand to the core and each turned exactly
one test red (the rest green, at the 53-test suite of the time), then reverted:

| Source mutant | Killed by |
|---|---|
| An epoch end leaves a waiter that was admitted but not yet drained resolved `Admitted` | `cell_1_11_epoch_end_resolves_waits` (its undrained-admission leg, all four epoch ends, CPU and GPU) |
| No re-test of the wait queue at a reset's apply, or at an adopt's apply | `wait_retested_at_reset_and_adopt_apply` (one leg each) |
| A normalization reported `Refused` (create and reset both refused) is treated as reset in place | `normalization_apply_outcomes` |

`EChargeKind::Normalization` and `EChargeKind::CachePreCharge` exist in the seam, but no test arms them. Their rows
are graded through the `NormalizationUncharged` and `PreChargeSkipped` mutants instead.

### Where the header left room: the readings the core takes

No test grades these, except where a test is named. Each is the reading the core takes, so that a later fold
can see it.
- **Releasing a prefix that is still building** is `PrefixPinned`: its own build job is queued and names it.
- **A prefix hint that names a node that is not live and built** makes the restore `InvalidRequest`. So does a
  cache-entry hint with key 0 or a length outside `[0, cap]`. A budget payload whose admission charge is 0 (budget
  or origin outside its domain) is `InvalidRequest`, too.
- **A hint on a whole-reserve payload** is ignored, as the header says. It is not validated, pins nothing and
  takes no A pre-charge. The `ShareCountedAsGuaranteed` mutant still counts it as hinted, because the probe
  model's SSB5 mutant does.
- **An adopt of a cache entry onto a whole-reserve slot** is `KvModeMismatch`, the same as a prefix adopt (§3.3:
  a holder whose mode differs is refused "reset first").
- **Admission check order** for an adopt: a bad slot, a busy slot or an unusable target is `InvalidRequest`
  first, then `KvModeMismatch`, then (for a cache entry) A's fit.
- **Configure check order:** `LegacyMode` (budget 0), `InvalidSequenceBudget` (budget < 0), `InvalidGeometry`,
  `InvalidSequenceBudget` (budget > cap), then the pool and region C's byte size against their integer bounds.
- **A reset reports what it did** (`EResetResult`), the same three ways on both paths. `Normalized` (a
  normalization only; refused on a reset in place) normalizes the slot. `ResetInPlace` resets the holder as it
  is: a reset in place that ran, or a normalization whose create was refused. `Refused` changes nothing, since
  nothing ran. Every normalization outcome refunds the create's charge. (`normalization_apply_outcomes`)
- **`SharedPages > 0` with no hint** settles as a private restore: there is no node to register on.
- **At an epoch end**, a waiter that was admitted but not yet drained has its resolution turned into
  `NotConfigured`, because its job was never enqueued. Each ticket is still resolved exactly once.
  (`cell_1_11_epoch_end_resolves_waits`)
- **Waiters are re-tested after every apply**, not only restore applies. Any H refund (reset, adopt, normalization)
  can admit the head of the queue, at that apply. (`wait_retested_at_reset_and_adopt_apply`)
- **Tickets and node ids rise for the ledger's whole life**, across epochs. A stale ticket can never name a job
  of the next epoch.
- **Two caps: where each cap applies.**
  - The plugin's cap `Cp` bounds every admissible input before any charge: budget, origin, prefix length,
    parent + child, `SequenceBudget`, and also a cache entry's length (an adopt's, or a restore hint's). The
    header names the first five; the entry length is the core's reading.
  - Every price is the engine's cap `Ce`: `R` (including `R(SequenceBudget)`, so S and the guarantee), `E`, a
    build's peak, and a whole-reserve draw.
  - Settled pages (`ceil(L/B)`, the nested child's pages, the region-A pre-charge `floor(L/B)`) depend on no cap.
- **Configure check order with two caps:** `LegacyMode`, then `InvalidSequenceBudget` (below 0), then
  `InvalidGeometry` (an engine cap below `ContextCap` among its causes), then `InvalidSequenceBudget` (above `Cp`).

## What is graded, and how

Plan §7 defines three grading forms. The host suite reproduces each one against `FFakePagePool`
(`fake_page_pool.h`), a host model of the engine's page pool. It implements the verbs the ledger prices: create,
prefix build, `begin_from`, freeze, adopt, reset, the budget and whole-reserve restore (sharing exactly when the
hint is frozen, its length equals the origin, and the bytes match), and release. Shared page groups are
refcounted. Every page is tagged Seq, Prefix or Cache, and a draw past the pool counts as an *exhaustion*, the
engine's refusal.
- **Form 1** (`FWorld::Form1`) checks four things:
  - the ledger's total ≥ the pool's drawn pages;
  - per tag: S+H ≥ Seq, P ≥ Prefix and A+C ≥ Cache;
  - `ChargesWithinPool()`;
  - zero exhaustions.
- **Form 2** puts a mutant at the boundary: it must be admitted by the ledger and then refused by the pool.
- **Form 3** is zero slack: the ledger refuses, and the pool would have refused too.

The wait rule is also graded against `restore_wait_model.h`, a port of the probe model, generalized so that a
restore guarantees `min(charge, R(SequenceBudget))` and holds `max(0, draw − R(SequenceBudget))`.

The global invariant is that charges never exceed the pool and charges ≥ drawn at every instant.
`invariant_random_workload` checks it after **every step** of a seeded random workload:
- 40 seeds, each on a CPU configuration with a cache and on a GPU configuration;
- jobs split across admit, draw, release and apply steps;
- the run then drains to quiescence, and the ledger must equal its post-Configure value;
- each of seven unsafe mutants must be killed on some seed.

## Cell → test

| Plan cell | Test (file) |
|---|---|
| §2 arithmetic, E2, E6 | `geometry_reservation_pages`, `geometry_materialized_and_restore_charge`, `geometry_prefix_and_nested_charges` (test_arithmetic.cpp) |
| 4.2 exact counts at the limit (ledger leg), E7 | `limit_token_law_exact_counts` (mutants `OneLate`, `OneEarly`) |
| 5 `KvBudgetExceeded` at activation, §3.4 | `limit_activation_precheck` |
| 9.7 (arithmetic leg) | `limit_restored_holder_keeps_saved_limit` |
| 7.1 | `pricing_budget_mode_three_shapes` |
| §3.2 regions and pool, 10.1 (host leg: 2,162 and 1,713 pages) | `configure_regions_and_pool` |
| 5 `InvalidSequenceBudget`, §3.1 | `configure_refusals` |
| §3.2/§3.4 GPU `CacheRegionPages()` | `configure_gpu_cache_regions_zero_until_cache` |
| §3.2 region C seam | `cache_region_seam` |
| 1.1 | `cell_1_1_lifecycle_cycles` (test_lifecycle.cpp) |
| 1.2 | `cell_1_2_zero_slack_each_kind` (seven kinds) |
| 1.3 | `cell_1_3_prefix_undercharge_boundary`, `cell_1_3_nested_undercharge_boundary`, `cell_1_3_restored_sharer_not_registered_boundary`, `cell_1_3_precharge_skipped_boundary` (test_prefix_adopt.cpp); `cell_1_3_restore_without_e_boundary`, `cell_1_3_ssb5_undercharge_boundary`, `cell_1_3_private_restore_refunded_at_apply_boundary` (test_restore_wait.cpp) |
| 1.4 | `cell_1_4_inflight_window` |
| 1.5 | `cell_1_5_enqueue_window` |
| 1.6 | `cell_1_6_failures_refund_exactly` |
| 1.7 | `cell_1_7_ghosts` |
| 1.8 | `cell_1_8_normalization_create_first`, `normalization_yields_to_waiting_restores` (rebuilt so it can fail: free 16, one restore in flight guaranteeing 9, a 24-page waiter), `normalization_apply_outcomes` |
| 1.9 (ledger legs of 1.1, 1.2, 1.4, 1.6) | `cell_1_9_gpu_twins` |
| 1.11 | `cell_1_11_epoch_end_resolves_waits` |
| 2.5 | `cell_2_5_nested_parent_not_usable` |
| 3.1 | `cell_3_1_release_pinned_while_queued`, `cell_3_1_release_pinned_by_waiting_restore` |
| 3.2 (a)–(e) | `cell_3_2_ab_wait_on_guarantee_cpu`, `cell_3_2_c_wait_on_guarantee_gpu_twin`, `cell_3_2_d_constructions_vs_probe_model`, `cell_3_2_e_sharing_burst` |
| 3.2 (f) | `cell_3_2_f_guarantee_capped_by_charge`: CPU pair 49 / 50, each on its own rebuild |
| 3.5 (host leg) | `cell_3_5_one_job_per_slot` |
| 4.3 (host leg) | `cell_4_3_empty_child_on_b_and_b_plus_1` (B and B + 1, both shapes); `two_cap_cell_4_3_cap_minus_one_prefix` ([two-cap]) |
| 1.3 at B = 64 | `cell_1_3_early_refund_boundary_b64` (14 / 15) |
| §2 CPU figures, 10.1 at B = 64 | `configure_and_sharing_figures_cpu_b64` (578, 1,474, 466) |
| UP-15 (c), §3.2 *Two caps* | `two_cap_block_arithmetic`, `two_cap_configure_regions` (S 1,650, P 257, H 513, pool 2,420), `two_cap_configure_engine_cap_field` (test_two_cap.cpp) |
| 1.2 [two-cap] | `two_cap_cell_1_2_zero_slack` |
| 1.3 [two-cap] | `two_cap_cell_1_3_prefix_r_boundary` (P free 256 / 257), `two_cap_cell_1_3_e_boundary` (H free 282 / 283) |
| 2.1 [two-cap] | `cell_2_1_two_cap_origin_bound`: origin 4,500 → `ContextCapMismatch`; 40,000 → `InvalidRequest` (the subsystem's `Malformed`) |
| 2.1 | `cell_2_1_budget_zero`: budget 0 in an `SSB6` (CPU) and an `SLM6` (GPU, two caps) → `InvalidRequest` (`Malformed`) |
| 2.6 and 5 [two-cap] | `cell_2_6_two_cap_budget_bound` (budget 4,097 → `ContextCapMismatch`; 40,000 → `InvalidRequest`; `SequenceBudget` 4,097 → `InvalidSequenceBudget`); `cell_2_6_two_cap_prefix_length_bound` (a 4,097-token prefix and a nested child past `ContextCap` → `InvalidRequest`, at `PrefixBlockCount` 2) |
| 4.2 [two-cap] | `two_cap_cell_4_2_limit_at_context_cap` |
| 4.4 [two-cap] | `two_cap_cell_4_4_headroom_burst` (5 / 2 / 3; ≤ 16 at three blocks) |
| 4.5 [two-cap] | `two_cap_cell_4_5_largest_restores` (charges 288 and 513) |
| 4.4 | `cell_4_4_headroom_blocks`; `cell_4_4_private_holders_per_block` (3 / 3 / 33 / 7 holders, the next restore refused on arrival and admitted after one reset); [two-cap] below |
| UP-6 | `prefix_count_and_page_refusal_up6`, `prefix_charge_settles_at_freeze` |
| UP-7 | `up7_default_burst_about_three_per_block` (B = 16 and B = 64, CPU and GPU) |
| 8.3 (host leg) | `cell_8_3_eviction_with_adopters_zero_slack` |
| 8.4, UP-14 | `cell_8_4_a_full_up14` |
| §3.2 A pre-charge keyed by entry | `adopt_precharge_once_per_entry`, `cache_hint_restore_precharge` |
| 5 `KvModeMismatch` | `adopt_mode_mismatch_whole_reserve` |
| §3.3 nested, 10.3 (host leg) | `nested_charge_and_settle` |
| 9.2 / 9.3 / 2.3 (charge legs) | `restore_settle_at_apply` |
| §3.5 wait predicate | `wait_retested_at_reset_and_adopt_apply`, `wait_guaranteed_return_capped_by_charge`, `wait_every_wait_ends_in_admission_random`, `probe_model_random_cross_check` |
| 11.2 | `cell_11_2_ledger_fault_detector` (mutant `LedgerFaultNotCounted`) |
| §0/§7 global invariant | `invariant_random_workload` |

### Mutants (test hooks, `SUPERSLM_PAGE_LEDGER_TEST_HOOKS`)

Each mutant is armed with `SetMutationForTest`, `SetForceUnderChargeForTest` or `SetLimitMutationForTest`.
`LT_KILLS` requires the check to fail with the mutant armed, and `LT_HOLDS` requires it to pass without one.

- **Under-charges** (`SetForceUnderChargeForTest`, per `EChargeKind`):
  - a prefix charged ceil(L/B);
  - a nested child charged 0;
  - a restore without E;
  - SSB5 charged R(b).
- **Ledger mutations** (`ELedgerMutation`):
  - `RestoredSharerNotRegistered`
  - `PreChargeSkipped`
  - `PrivateRestoreRefundedAtApply`
  - `RefundAtEnqueue`
  - `FailedRestoreKeepsCharge`
  - `GhostReleasedAtFirstDependent`
  - `DependencyKeptAtReadopt`
  - `ShareCountedAsGuaranteed`
  - `WaiterAdmittedWithoutGuarantee`
  - `WaitersSurviveEpochEnd`
  - `LedgerFaultNotCounted`
  - `NormalizationUncharged`
- **Limit mutations:** `OneLate` and `OneEarly`.

## Cells left to the box

The host suite grades the ledger's bookkeeping against a host model of the pool. Everything else needs at least
one of these:
- a real model and the engine's inference;
- the real page pool and its fill probe;
- Unreal (the subsystem, the drain, Blueprint, packaging);
- a GPU.

| Cell | Step | Why it is not here |
|---|---|---|
| 1.1–1.9, 1.11, 3.1, 3.2, 3.5, 4.4, 8.3, 8.4, 11.2 and the [two-cap] legs (engine legs) | K2/K3/K6/K7 | The host legs are here. The box re-grades each cell against the real pool with the two-sided fill probe, through the subsystem's queue and `HoldApply`/`HoldStart` seams. Those seams are Unreal code. |
| 1.10 | K8 | Memory growth over 1,000 cycles in a packaged build. |
| 1.12 | K3/K7 | No lost state needs the engine to refuse a restore after admission, and the slot's tokens to be checked afterwards. |
| 2.1, 2.2, 2.3 | K3/K7 | Payload-header parsing, and the wrapper's mode and size checks, are subsystem code with real `SSB6`/`SLM6` bytes. Only the charge formula of 2.3 and the ledger's half of 2.1's [two-cap] bound (origin 4,500 refused, nothing charged) are here. |
| 2.4 | K7 | Handle identity across the CPU and GPU subsystems. |
| 3.3, 3.4 | K7/K8 | `SSLM_BUSY` under stress, and cadence under the build seat, are GPU scheduler properties. |
| 4.1, 4.3, 4.5, 4.6 | box / K8 | Model × device matrices, and token-level checks at prefix lengths and origins. Packaging. Here are the ledger legs of 4.3 (the empty child; the [two-cap] `cap − 1` prefix) and of 4.5 [two-cap]. |
| 4.2 (token leg) | K2/K3/K6/K7 | Emitted-token counts need generation. The ledger's limit arithmetic (E7) is here. |
| 6.1–6.5 | K2/K3/K7 | Token equality and the build's independence from load. |
| 8.1, 8.2, 8.5, 8.6 | K2/K6/K7/K8 | Existing suites in legacy mode, schema/adapter adopt, cache pairs in the real cache, and multi-plugin coexistence. |
| 9.1–9.6, 9.8–9.10 | K2/K3/K7 | Round trips, wrappers, mode crossings, Blueprint binary compatibility and downgrade. These are serialization and token checks. 9.7's arithmetic leg is here. |
| 10.1–10.9 | K4/K7/K8 | Sharing, nesting and memory at real size, read by the probe and the adapter; 10.4's engine-only oracle; armed transitions. The arithmetic of 10.1 and 10.3 is here. |
| Test-hook mapping | Unreal wiring step | `SUPERSLM_PAGE_LEDGER_TEST_HOOKS` must be defined to `WITH_DEV_AUTOMATION_TESTS` in the module's build rules, which cannot be compiled here. See *For the Unreal wiring step*. |

### For the Unreal wiring step

These belong to the step that wires the core into the subsystems. None of them can be done or checked here.
- **The test hooks are not mapped in Unreal yet** (plan §3.9 now says how). The header defaults
  `SUPERSLM_PAGE_LEDGER_TEST_HOOKS` to 0, and the module's build rules do not define it. The wiring step must define it to `WITH_DEV_AUTOMATION_TESTS`
  there, so that the box's seams (`ForceUnderCharge` and the mutants) exist in development builds and not in
  Shipping.
- **One job per slot** (plan §3.2). The ledger refuses a second admission for a slot
  that has a job open. The subsystem admits a request when it reaches the head of its sequence's queue, not when
  it is queued, and the queue keeps its depth of 4. `cell_3_5_one_job_per_slot` grades the ledger's half, and a
  test-local model of the queue stands in for the subsystem's half.
- **The engine cap** (UP-15 (c)). The GPU subsystem passes the model's `context_cap` as
  `EngineContextCap` and its own `ContextCap` as `ContextCap`. The CPU subsystem may leave `EngineContextCap` at 0.
  Its pool create takes `PoolPages()`, so the pool is `S + P + H` with the two block sizes. The subsystem's
  pre-checks read `SlotLimit(Slot)`. For a restore, the ledger answers `ContextCapMismatch` when a budget or
  origin is above `ContextCap` and within the model's cap, and `InvalidRequest` when it is above the model's cap or
  the budget is 0; the subsystem maps `InvalidRequest` to `Malformed` and passes `ContextCapMismatch` through. A
  payload's `context_length` never reaches the ledger, so the subsystem checks it against the two caps itself, the
  same way. A prefix length above `ContextCap` is `InvalidRequest` (§3.6: `Malformed`).
- **A whole-reserve hint.** The ledger ignores a hint on a whole-reserve payload and does not pin it. So the hint
  may reach the engine only as null, or as a handle the subsystem has resolved to a live prefix at the job's run.
- **The three readings the first round asked the planner to fold** are in the plan (§15.4): an empty
  nested child settles to its tail page; the guaranteed return is `min(charge, R(SequenceBudget))`; normalization
  yields to waiting restores. Each now has a cell with a mutant (4.3, 3.2 (f), 1.8).

### The optional engine leg: not added

The engine at the plan's K0 commit builds on Linux, and its paged-KV test fixtures can be generated on the host.
Engine-graded host cells were nevertheless left to the box, for three reasons:
1. **It would add a second dependency.** Such a cell links the engine library and a generated model fixture. That
   gives this suite a dependency on an engine build tree and on the fixture generator (Python with numpy). No CI
   runs this suite: it is run by hand, and by the box step. Every extra dependency is one more thing each of those
   runs must set up, and a suite that is hard to build gets run less.
2. **No GPU verbs.** The engine at that commit has no GPU paged verbs (pooled KV, GPU prefixes, budgeted GPU
   create), so the GPU half of every cell could not be engine-graded anyway.
3. **The box grades the same thing better.** The two-sided fill probe grades the ledger as the plugin drives it:
   through the subsystem's queue and its `HoldApply`/`HoldStart` seams. A host harness calling the engine
   directly would grade a third driver that ships nowhere. The fake pool here is written to the engine's draw
   rules (restore draw = R(budget) + min(full, cap_pages − R), or shares; whole reserve draws cap_pages). The box
   cells catch any drift between the two.

## Readings this suite pins (plan ambiguities)

Each of these is a test the implementation must satisfy. If the plan's author rules otherwise, the named test changes.
- **Guaranteed return** is `min(charge, R(SequenceBudget))`, not R(SequenceBudget) flat. A restore charged less
  than R(SB) can return only what it holds, and the flat reading can strand a waiter.
  (`wait_guaranteed_return_capped_by_charge`)
- **A restore whose draw is ≤ R(SB) holds nothing** past its guarantee: hold = max(0, draw − R(SB)).
  (`restore_wait_model.h`)
- **A cache-hinted restore when A is full** is refused `KvPagesExhausted`, the same way as a first adopt. The
  hint is not dropped. (`cache_hint_restore_precharge`)
- **An empty nested child** settles to its copied tail page (`NestedSettledPages`), not 0.
  (`nested_charge_and_settle`)
- **Count and pins.** A prefix's count slot is freed when its release is admitted. A queued adopt,
  `begin_from` or hinted restore — waiting ones included — pins the prefix. (`cell_3_1_*`)
- **Normalization yields to waiting restores.** It takes no H while a restore waits.
  (`normalization_yields_to_waiting_restores`)
- **A waiter's A pre-charge** is taken when it arrives, not when it is admitted. `FAdmission::ChargedPages`
  reports only that pre-charge. (`cache_hint_restore_precharge`)
- **`LedgerFaults` is cumulative** across epochs. (`cell_11_2_ledger_fault_detector`)
- **The two caps.** These are the readings the two-cap legs pin:
  - **The engine-cap field.** `EngineContextCap` 0 means "equal to `ContextCap`", so the CPU and every earlier
    configuration are unchanged. A value below `ContextCap` is `InvalidGeometry`. The plan names no outcome for
    it. A model whose cap is below the game's `ContextCap` cannot serve it.
  - **The ledger's names for a restore field out of range.** A budget or origin above `ContextCap` and within the
    model's cap is `ContextCapMismatch`, a core outcome. The plan names it a plugin admission check (§3.5, §3.6),
    and its cells 2.1 and 2.6 grade the distinction, including the mutant "the first reported `Malformed`". The
    ledger is the one place that holds both caps, so the distinction is made there and nowhere else. A budget of
    0, or a field above the model's cap, is `InvalidRequest`, as it is for every out-of-domain argument; the
    subsystem names it `Malformed`. A prefix length or parent + child above `ContextCap` stays `InvalidRequest`:
    there is no save that could be valid under a larger cap. `InvalidSequenceBudget` above `ContextCap` is the
    ledger's own.
  - **The limit is the ledger's to state:** `SlotLimit(Slot)` = `HolderLimit(ContextCap, SlotShape(Slot))`. The
    plan puts the limit in the subsystem; it was given one home in the core so that "the limit at the model's
    cap" has a mutant site.
  - **A whole-reserve restore is charged `ceil(Ce/B)`**, as the engine draws. Only the CPU accepts one in budget
    mode, and there `Cp = Ce`, so no leg can tell this apart from `ceil(Cp/B)`.
  - **"R priced at `ContextCap`"** (the plan's 1.2 and 1.3 mutant) is a pricing mutant only: block sizes stay
    UP-15 (c)'s. Blocks are graded by their own two mutants. As the coverage re-audit says (R1-B1), 1.2's prefix
    kind cannot kill this mutant by exhaustion. The host leg kills it in the build's in-flight window (charge 256
    < drawn 257), and 1.3's [two-cap] prefix row kills it at the boundary.
  - **1.3's [two-cap] E row** places H free 282 with an applied private hold of 231 (b = 512, origin 3,696), not
    an in-flight restore (N-b). Nothing is guaranteed, so the 283 is refused rather than made to wait.
  - **3.2 (f)'s two waiters** each run on their own rebuild (N-a).
  - **The shapes are named per backend:** the B = 16 shape is the GPU's (0.5B), and the B = 64 shape is the
    CPU's. Tests that run the B = 16 shape through a CPU configuration grade the ledger's arithmetic at that page
    size, which is an input whatever the backend.
- **Private holders per H block** are `floor((H_blk − q)/net) + 1`, not `floor(H_blk/net)`. The last holder's
  restore occupies its whole charge `q` until it applies. At the executed shape this is 3 / 3 / 33 / 7 (CPU 0.5B,
  GPU 0.5B, CPU 1.5B, GPU two-cap), with H free 19 / 70 / 17 / 79 afterwards. (`cell_4_4_private_holders_per_block`)
  - The plan's mutant "a hold settled one page smaller" admits an extra restore only where the slack allows: at
    CPU 1.5B (35 holders). On every shape, form 1 kills it at the first apply.
- **B is read, not assumed.** The engine derives the page size per model. The configuration takes `PagePositions`
  and `ContextCap`, and every figure is computed from them: B = 16 gives 95/33, and B = 64 gives 24/9. Both give
  UP-7's 2 in flight, 1 waiting and 7 refused per headroom block at the executed shape (budget 512, origin 1,000). `B = cap` (one page per sequence) also works.
