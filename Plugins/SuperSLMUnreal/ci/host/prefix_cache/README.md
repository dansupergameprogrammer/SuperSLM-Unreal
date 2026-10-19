# Prefix cache: host suite

This suite tests the prefix cache's cores on Linux with g++ and clang, with no Unreal. The cores are
the cache policy that needs no engine, in `Source/SuperSLMUnreal/Private/Core/`:

| File | What it holds |
|---|---|
| `SuperSLMResidencyCore.h/.cpp` | `FSuperSLMResidencyCore`, the kind-neutral residency core. It holds the ledgers, keyed by (kind, ledger id) with a memory class each, the charge rule, LRU eviction of unpinned entries, pins, leases (one handle per lease, minted from the caller's counter), key lookup for coalescing, release before reuse, and the diagnostics snapshot. Prefixes are kind 0 and its only user so far. The adapter step adds what the LoRA residency plan's §5.3 needs beyond this: a charge that lands in more than one ledger, a budget of 0 meaning unlimited, and External entries. |
| `SuperSLMPrefixCacheKey.h/.cpp` | The key (precache plan §5.2): SHA-256 over a length-prefixed preimage of 11 fields. The exact encoding is in the header. |
| `SuperSLMPrefixCacheWrapper.h/.cpp` | The `'SLUP'` disk and cooked wrapper (§5.7): encode, and every load check as a named miss. |
| `SuperSLMPrefixDiskCache.h/.cpp` | The disk tier's file rules over an abstract file layer: file and temporary names, write-then-rename with a fault seam, read, and an oldest-first trim. |
| `SuperSLMPrefixCacheCore.h/.cpp` | `FSuperSLMPrefixCacheCore`, the prefix-specific policy (§5.3–§5.4, §5.9, §5.10). It emits engine and IO work as commands and takes one completion per command. |

The cores are game-thread bookkeeping. They never call Layer 1's C ABI or touch a file. The
subsystem runs their commands (`ComputeKey`, `CheckCooked`, `ReadDisk`, `Restore`, `Build`, `Save`,
`WriteDisk`, `RemoveDiskFile`, `TouchDiskFile`, `TrimDisk`, `Release`, `DropPayload`) on pool
threads and worker lanes. It feeds each completion back in `Tick()`'s apply pass. In this suite,
`sim.h/.cpp` plays that part. It provides an in-memory file layer and a fake page pool, whose
`CacheDrawnPages` stands in for the fill probe. It also provides an executor and a fake `SPX1`
payload with a checksum standing in for the digest.

Build constraints on the cores: C++17, standard library only, no exceptions and no RTTI. The suite
builds them with `-fno-exceptions -fno-rtti -Wconversion -Wsign-conversion -Wundef -Wshadow
-Werror`. The cores avoid Unreal's macro names and have no dynamically initialised globals. Layer 1's
`src/sha256.cpp` builds as its own target, with exceptions, because it keeps Layer 1's "throws only
`std::bad_alloc`" contract. It needs only `include/` and `src/` (for `bad_alloc_wrap.h`) on Linux.

## Build and run

```sh
# from this directory
CXX=g++ cmake -S . -B build-gcc -DPREFIX_CACHE_SANITIZE=ON   # ASan + UBSan
cmake --build build-gcc -j && ./build-gcc/prefix_cache_host_tests

CXX=clang++ cmake -S . -B build-clang                        # add -DPREFIX_CACHE_SANITIZE=ON where clang's compiler-rt is installed
cmake --build build-clang -j && ./build-clang/prefix_cache_host_tests

./build-gcc/prefix_cache_host_tests cache_lru     # run the tests whose name contains a substring
```

The exit code is 0 only when every test that ran passed. `ctest` runs the whole binary as one test.

The container these were authored in has no clang sanitizer runtime (`libclang_rt.asan*` is absent).
So clang builds there without sanitizers, and g++ carries ASan and UBSan.

## Green run

The run was recorded on 2026-10-10 against the implemented cores, after the final review's fix round:

| Toolchain | Build | Result |
|---|---|---|
| g++ 13.3, ASan + UBSan | clean (`-Werror`) | **82 tests, 82 passed, 0 failed**; no sanitizer report |
| clang 18.1 | clean (`-Werror`) | **82 tests, 82 passed, 0 failed** |

Each core source compiles clean on its own under both compilers, at `-std=c++17` and at `-std=c++20`
(the module's standard), with `-pedantic-errors -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
-Wundef -Werror -fno-exceptions -fno-rtti`. The `prefix_cache_core_unity` target builds all five cores
as one translation unit, as Unreal's unity build may.

Tests changed rather than added:
- `cache_a_refused_lease_is_returned_once` called `RequestReady`, which returns 0 unless the lease is
  Ready, and then required the lease to be `UnknownSchema`, so no implementation could pass it. It now
  calls `Request`, then `RunAll`. Its assertions are unchanged.
- The residency tests pass a ledger id where they passed a memory class, because ledgers are now keyed
  by (kind, ledger id). Their assertions are unchanged.

### Mutant check

Each mutant was applied to the green cores and the suite was run under g++. None was committed.

| Mutant | Tests that turn red |
|---|---|
| 11.4, wrapper: the spec compare in `CheckPrefixWrapper` removed | `wrapper_each_load_check_names_its_reason`, `wrapper_spec_compare_is_load_bearing`, `cache_disk_misses_each_reason`, `cache_disk_spec_compare_is_load_bearing`, `cache_memory_collision_falls_through_the_tiers` |
| 11.4, memory: the spec compare at the memory hit or attach removed | `cache_memory_spec_compare_is_load_bearing`, `cache_memory_collision_falls_through_the_tiers` |
| Charge before draw: the restore or build is emitted uncharged, and charged its steady figure when its result is applied | `cache_build_is_charged_its_peak_before_it_draws`, `cache_restore_is_charged_before_it_draws_and_settled_on_apply`, `cache_every_entry_pinned_and_one_more_is_budget_exhausted`, `cache_release_before_reuse`, `cache_a_save_in_flight_pins_its_entry`, `cache_teardown_releases_every_engine_handle_exactly_once`, `cache_teardown_cancels_a_deferred_admission`, `cache_churn_keeps_every_invariant` |
| Charge at the peak: a build charged `ceil(L/B)` instead of `R(L)` | `cache_build_is_charged_its_peak_before_it_draws` |
| Pins block eviction: `LruOrder` ignores pins | `residency_budget_exhausted_evicts_nothing`, `residency_kinds_do_not_mix`, `residency_pins_are_counted_per_reason`, `cache_every_entry_pinned_and_one_more_is_budget_exhausted`, `cache_pool_exhausted_with_nothing_evictable_is_a_named_refusal`, `cache_a_save_in_flight_pins_its_entry`, `cache_eviction_skips_an_entry_with_a_queued_adopt`, `cache_churn_keeps_every_invariant` |
| Teardown sweep ignores open adopts | `cache_teardown_waits_for_an_open_adopt` |
| A build landing in teardown becomes Ready normally (and is saved) | `cache_build_landing_in_teardown_is_not_saved` |
| A save landing in teardown writes back | `cache_teardown_with_a_save_in_flight` |
| Teardown does not cancel a pool-retry waiter | `cache_teardown_cancels_a_pool_retry` |
| A memory collision jumps straight to a recompute | `cache_memory_collision_falls_through_the_tiers` |
| A memory collision's spec mismatch removes the key's file | `cache_memory_collision_falls_through_the_tiers` |
| A lease keeps its spec copy after its key is applied | `cache_ended_leases_hold_no_spec` |
| A restore ending `Failed` deletes the file or marks the cooked entry stale | `cache_restore_failure_keeps_the_bytes` |
| A failed build is not counted or logged | `cache_build_failure_is_counted_and_logged` |
| A failed save is not counted or logged | `cache_save_failure_is_counted_and_logged` |
| The teardown sweep leaves Ready leases Ready | `cache_teardown_sweep_ends_ready_leases_by_name` |

One mutant survives, and it is equivalent. If `BeginTeardown` does not cancel an admission that is
waiting on a release, the admission is cancelled anyway when the release completes, because `Drain`
resolves every admission made during teardown. The early cancel changes only when the entry is
destroyed: a waiting admission holds no charge.

## Red run (stubbed cores)

The run was recorded on 2026-10-10 against the stubs (since replaced, with `SuperSLMCoreStub.h`
deleted):

| Toolchain | Build | Result |
|---|---|---|
| g++ 13.3, ASan + UBSan | clean (`-Werror`) | **67 tests, 0 passed, 67 failed**; no sanitizer report |
| clang 18.1 | clean (`-Werror`) | **67 tests, 0 passed, 67 failed** |

Every red comes from the stubs. The stubs return an out-of-range enum value (`0xEE`), an empty
vector or string, a zero key, `false`, or a `Configure()` that refuses. Every cache cell's first
failure is `required: F.bConfigured`. Every pure-function cell's first failure is its own
first assertion against the stub's value (for example `Hex.size() == 64 (0 vs 64)` or
`Check(...) == Ok (238 vs 0)`). Every test also has a positive clause, so no refusal or absence
cell can pass on a stub. No test crashes: `REQUIRE_*` returns before any index past an empty result.

## Cell → test

Cell ids are the precache plan's §7 ids. "Host half" means the cell's bookkeeping is graded here,
and its engine half (fill probe, real pool, lanes) is graded on the box (see the next section).

| Cell | Test(s) |
|---|---|
| 1.4 (host half) | `cache_churn_keeps_every_invariant`: 500 requests, 40 keys, a budget of 10 entries, random leases and returns, random work left in flight, a seeded `mt19937` drawn without std distributions. The invariants are asserted after every completion. Both recompute-built and restore-built entries occur, every request resolves by name, and the pool ends at its initial count with every handle released once. |
| 1.5 | `cache_teardown_releases_every_engine_handle_exactly_once`, `cache_handles_from_before_a_reconfigure_are_unknown` |
| 1.6 | `cache_disk_tier_warm_across_two_lifetimes` |
| 1.7 (bookkeeping half) | `cache_teardown_waits_for_a_running_restore`, `cache_teardown_drops_a_queued_restore`, `cache_teardown_waits_for_a_running_build` |
| 2.8 | `cache_disk_misses_each_reason` covers seven cases: truncated, other wrapper version, other Layer-1 tag, spec mismatch under an equal key (forced collision), zero length, a foreign file, and a file replaced between scan and read. It is backed by `wrapper_each_load_check_names_its_reason` and `wrapper_every_truncation_is_a_miss`. |
| 2.10 | `cache_release_prefix_refuses_cache_handles`, `cache_adopt_of_an_ended_lease_is_refused`, `cache_return_prefix_dispositions`, `cache_a_refused_lease_is_returned_once`, `cache_handles_from_before_a_reconfigure_are_unknown`, `cache_teardown_releases_every_engine_handle_exactly_once` |
| 2.11 | `cache_unknown_schema` |
| 2.12 | `key_each_component_changes_the_key` (every one of the 11 components, pure), `key_length_prefix_pairs_do_not_collide` (two pairs derived from §5.2's order: schema name before tokens), `cache_key_mutation_through_the_cache` (tokens, schema name and the printable length-prefix pair through the cache: B is not `Memory`, B's handle carries B's tokens, and the spec-mismatch counter reads 0). The parent-key case is U6. |
| 3.5 | `cache_coalesces_sixteen_requests_into_one_build`, `cache_coalesced_shared_load_fails_then_recomputes`, `cache_coalesced_load_ending_pool_exhausted_resolves_all`, `cache_coalesced_first_requester_returns_before_ready` |
| 3.6 | `cache_eviction_skips_an_entry_with_a_queued_adopt` |
| 3.7 (host half) | `cache_teardown_with_disk_reads_and_writes_in_flight` (in flight > 0 at the call; no release under IO; no partial file; read bytes dropped) |
| 3.8 (names half) | `disk_file_and_temporary_names` (temporary names unique per instance and per write) |
| 5.4 | The `Fallbacks[]` counter rises by one and the log line names the reason, for each §5.9 row. Key row: `cache_a_different_model_or_pin_is_a_miss`. Wrapper rows: `cache_disk_misses_each_reason`, `cache_cooked_miss_marks_the_entry_stale_and_falls_through`. Payload corrupted and other producer: `cache_fallback_payload_refused_by_the_engine`. Unknown schema: `cache_unknown_schema`. §5.10 dispositions: `cache_release_prefix_refuses_cache_handles`, `cache_adopt_of_an_ended_lease_is_refused`. BudgetExhausted: `cache_every_entry_pinned_and_one_more_is_budget_exhausted`. PoolExhausted: `cache_pool_exhausted_with_nothing_evictable_is_a_named_refusal`. Disk read and write errors: `cache_fallback_disk_read_and_write_errors`, `cache_save_failure_is_counted_and_logged`. Engine failures that are not refusals: `cache_build_failure_is_counted_and_logged`, `cache_restore_failure_keeps_the_bytes`. Nested: `cache_request_refusals`. |
| 5.5 (host half) | `cache_every_entry_pinned_and_one_more_is_budget_exhausted`, `cache_pool_exhausted_with_nothing_evictable_is_a_named_refusal`, `residency_budget_exhausted_evicts_nothing` |
| 5.8 | `disk_write_is_atomic_and_the_fault_seam_leaves_no_key_file`, `cache_write_fault_then_the_next_request_recomputes` |
| 7.8 (host half) | `cache_key_is_computed_by_a_command`: `Request` emits `ComputeKey` with the key inputs, and the lease stays Pending until the key arrives as a completion. |
| 7.9 | Through 5.4's tests: every fallback ends in a recompute or a named refusal, and no restore is accepted that the fake engine refused. |
| 7.10 (host half) | `cache_churn_keeps_every_invariant`, `cache_every_entry_pinned_and_one_more_is_budget_exhausted`, `cache_build_is_charged_its_peak_before_it_draws`, `cache_release_before_reuse`, `residency_release_before_reuse` |
| 7.13 | `cache_memory_hit_oracle`, `cache_lru_oracle` (touch order C, A, D, B; the mutants "evict the most recent" and "evict in insertion order" each pick another entry) |
| 7.14 | `cache_disk_trim_oldest_first_and_a_hit_refreshes`, `disk_trim_plan_oldest_first`, `disk_trim_directory_removes_oldest`, `disk_write_sets_the_modification_time` |
| 8.10 (charge half) | `cache_build_is_charged_its_peak_before_it_draws` (R(L) charged before the draw, ceil(L/B) after the freeze, including R(64) = ceil(cap/B)) |
| 9.6 | `wrapper_versions_0_and_2_and_other_pins_are_misses` |
| 11.4 | `wrapper_spec_compare_is_load_bearing`, `cache_disk_spec_compare_is_load_bearing`, `cache_memory_spec_compare_is_load_bearing`. Each forces a key collision with a different spec. With the compare removed, each turns red: the wrapper check returns Ok, or the request is a `Disk`/`Memory` hit whose handle carries the other spec's tokens. |

Policy the plan states outside a numbered cell, also covered:

| Rule | Test(s) |
|---|---|
| §5.2 key encoding | `key_preimage_matches_documented_layout` (an independent encoder in the test), `key_hex_round_trip` |
| §5.3 lookup order, fall-through | `cache_tier_order_memory_cooked_disk_recompute`, `cache_cooked_miss_marks_the_entry_stale_and_falls_through` |
| §5.3 item 4, §5.7 write-back | `cache_recompute_is_saved_and_written_back` |
| §5.4 charge rule | `cache_build_is_charged_its_peak_before_it_draws`, `cache_restore_is_charged_before_it_draws_and_settled_on_apply`, `cache_failed_build_returns_its_whole_charge`, `residency_settle_and_return_charge` |
| §5.4 the pool is the hard limit | `cache_pool_exhausted_evicts_and_retries`, `cache_pool_exhausted_with_nothing_evictable_is_a_named_refusal` |
| §5.4 pins | `cache_a_save_in_flight_pins_its_entry`, `cache_eviction_skips_an_entry_with_a_queued_adopt`, `residency_pins_are_counted_per_reason` |
| §5.6 one handle per lease | `cache_one_handle_per_lease`, `residency_lease_handles_come_from_the_callers_counter` |
| §5.6 synchronous refusals, nested refused | `cache_request_refusals` |
| §5.7 wrapper layout | `wrapper_round_trip_and_layout` |
| §5.7 a vanished or mid-rename file is a miss; a read error is an error | `disk_read_vanished_is_not_found` |
| Shared core: LRU, coalescing key lookup | `residency_charge_evicts_lru_unpinned`, `residency_key_lookup_and_unkeyed_entries` |
| Shared core: per-kind budgets (LoRA plan §5.0) and one ledger per charge, two of one class allowed (the shape §5.3 needs) | `residency_kinds_do_not_mix` (kind 1 is test-only), `residency_classes_do_not_mix`, `residency_two_ledgers_of_one_class_do_not_mix` |

## [U2]–[U4] cells left to the box, and why

| Cell | Why it is not here |
|---|---|
| 1.4, 1.5, 1.7, 2.10, 5.5, 7.10, 8.10 (engine half) | These grade against the engine's fill probe on a real pool, read behind the lane-park barrier or after every lane has stopped. They also need the real `sslm_kv_pool_destroy` status and the release-counting seam on the plugin's `sslm_prefix_release` call site. Here the fake pool and the executor's release counts stand in, but the readings themselves are the box's. |
| 1.6, 2.8, 2.11, 3.5, 5.4, 5.8, 7.14, 11.4 (engine half) | Each says "tokens equal to the prefill twin", which needs the real model and decode. Here the fake engine's handle records the tokens and schema it was built or restored from, which grades the policy but not the K/V. |
| 2.12 (subsystem half) | The adopter-token clause needs the real decode. `FName` cannot carry the NUL bytes of the first length-prefix pair, so that pair is constructible only at the core, which is where the key lives. The printable pair also runs through the subsystem. |
| 2.15 [U2, U3] | Needs `Configure()` against the two-name model artifact and a three-schema model, which means a real Layer-1 map. |
| 3.7 (subsystem half) | `Deinitialize()` with real `Async` file tasks in flight; temporaries abandoned by a crash. |
| 3.8 (concurrency half) | Two subsystem instances in one PIE process racing writes, trims and renames on a real file system. |
| 3.9 | The on-game-thread assert in the cache's state accessor: this needs Unreal's thread identity. |
| 7.8 (assert half) | The off-game-thread assert and its must-reject: this needs Unreal's thread identity. |
| 8.8 | A sequence adopted from a cached prefix, saved and restored: real Layer-1 sequence verbs. |
| 8.10 (phases a–c) | 1.0.0's warm sequences and `CreatePrefix` beside a full cache, the 1.0.0-prefix counter, the batch gate and the lane park. |
| 2.9, 9.7 [U3] | Cooked prefix sets are U3 (asset, editor build, commandlet, cook validation). The runtime cooked-tier bookkeeping is here: `cache_tier_order_memory_cooked_disk_recompute` and `cache_cooked_miss_marks_the_entry_stale_and_falls_through`. |
| 4.5, 5.9, 8.9, 10.5, 10.6 [U4] | A packaged Windows build, the hitch counter with a lane-delay seam, the determinism self-check, and packaged-game oracles. |

## What the Unreal wiring (U2) must do

The cores leave these to the subsystem that runs their commands:

- **Serialise disk file operations per key.** The core emits `ReadDisk`, `RemoveDiskFile`,
  `TouchDiskFile` and `WriteDisk` for one key in the order it means them. The executor must run them
  in that order for any one key. Otherwise a `RemoveDiskFile` from a miss can land after a later
  `WriteDisk` for the same key and delete the good file. Commands for different keys may run in
  parallel. The host simulator runs every command in order, so this suite cannot catch a violation.
- **Trim ignores orphaned temporaries.** `TrimDisk` counts and removes only `<64 hex>.slup` files. A
  temporary left by a crash between write and rename stays in the directory. The plan accepts this:
  a temporary is never read as a cache file, and its name is unique per instance and write. Cleaning
  temporaries is not part of U2.
- **Feed exactly one completion per command taken,** in `Tick()`'s apply pass on the game thread.

## Choices the plan leaves to U2, made here

These are interface choices. The implementation may revisit them, and the tests that pin them are named.

- **Release before reuse.** An evicted entry's charge stays on the ledger, as Releasing, until its
  `Release` completes. An admission that fits once those bytes return is deferred, not refused.
  This comes from the LoRA residency plan's §5.3, which asks the shared core for it. It is also what
  keeps the precache plan's 7.10 operand (the fill probe never sees more than the budget) true while
  a release is still queued on a lane. Tests: `cache_release_before_reuse`,
  `residency_release_before_reuse`.
- **A refused admission evicts nothing.** If even evicting every unpinned entry would not make
  the charge fit, the result is BudgetExhausted and no entry is evicted. Test:
  `residency_budget_exhausted_evicts_nothing`.
- **The charge is taken when the restore or build is about to be emitted.** It is taken on the game
  thread in the apply pass, after the key and any read or cooked check. It is not taken inside
  `Request`, because a memory hit draws nothing.
- **Memory-tier collision.** The colliding request gets an unkeyed entry that tries the cooked tier,
  then disk, then a recompute, like any miss. It changes nothing stored under the key: it is never
  written back, and a wrapper it rejects is neither removed nor marked stale, because those bytes are
  the keyed entry's to judge. Test: `cache_memory_collision_falls_through_the_tiers`.
- **Wrapper check order.** Truncation below 4, 8 or 132 bytes is reported where it is first
  detectable, so a 12-byte foreign file is BadMagic and a 100-byte cut wrapper is Truncated.
- **Trim scope.** Only `<64 hex>.slup` files are counted or removed. Temporaries and other files are
  left alone.
- **ReturnPrefix on a refused lease** returns true once, then false.
- **BeginAdopt does not touch LRU.** This makes 3.6's discrimination independent of touch policy.

The implementation made these further choices where the headers left room. No test pins them.

- **Admission is FIFO per ledger.** While an admission waits, a later one waits behind it, even if it
  would fit now. Waiting admissions are retried whenever charged bytes fall: at a release, a settle,
  a returned charge or a budget change.
- **A restore the pool refuses** consumed its payload. After the eviction's release completes, the
  same tier is read again and charged again. A build the pool refuses keeps its charge and is
  re-emitted.
- **A restore that ends `Failed`** (an engine failure, not a refusal of the bytes) is a `RestoreFailed`
  miss, counted and logged, then the next tier. It keeps the file and the cooked entry: only
  `Refused` deletes or marks stale. Test: `cache_restore_failure_keeps_the_bytes`.
- **A build that ends `Failed`** resolves `Failed`, counted and logged as `BuildFailed`. **A save that
  ends `Failed`** writes nothing back, counted and logged as `SaveFailed`. Tests:
  `cache_build_failure_is_counted_and_logged`, `cache_save_failure_is_counted_and_logged`.
- **`ServedBySource`** counts leases served, so 16 coalesced leases on one build count 16.
- **`GetSource`** reports the tier that served the lease, and keeps it after the lease ends. It reports
  `Pending` for a lease that never became Ready.
- **The disk is trimmed after each successful write**, and not after a failed one.
- **Cooked registrations survive `Configure()`**, and their stale marks are cleared, because a new
  identity can make a stale blob valid again.
- **`Configure()` refuses** a zero page size, zero positions per page, or a non-positive context cap or
  vocabulary size.
- **`BeginAdopt` during teardown** returns NotReady, so no adopt opens after the release sweep.
- **At the teardown sweep every Ready lease resolves Cancelled**, so no lease reads Ready once its
  handle is released. It may still be returned once. Test: `cache_teardown_sweep_ends_ready_leases_by_name`.
