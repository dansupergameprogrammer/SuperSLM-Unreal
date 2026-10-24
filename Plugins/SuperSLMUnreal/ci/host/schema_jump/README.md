# Schema jump-forward: host suite

This suite tests the plugin side of schema jump-forward (schema fast-decode plan rev 1.3.6, §6 slices
P1 and P1-X; §17.11, §17.12; code review round 1) on Linux with g++ and clang, with no Unreal and no engine. It covers the parts that need
neither:

| Under test | Where |
|---|---|
| The three-way setting, the scratch switch, effective threaded decode and its 6-token draft cap | `Source/SuperSLMUnreal/Private/Core/SuperSLMSchemaJumpCore.h/.cpp` |
| The D2-online draft table: merge, build, the 0.5 share-product gate, the stop at the first state the table does not hold, the clip, the cap of 4 (rev 1.3.6: Space-Saving eviction, the newcomer inheriting the lowest count as its error; the draft and share read the guaranteed count), the lowest id on a tie | same |
| Forcedness as the plugin observes it (`FForcednessObservations`, `FObservedSchemaAutomaton`) and what the table learns (P1X.12 (a) to (d)), against B7's own table; the span walk before the stop check (P1X.12 (f), `WalkSpanResult`) | same, and `fixtures/` |
| The shipped-table form (`ParseShippedDraftTable`, SFD-DRAFT-TABLE/1), including a range check that does not wrap (review finding 6) | same |
| The table store: per-(model, schema) snapshots, merge on completion (Off and Exact that end accepting; never Canonical), swap by pointer, seed, drop at unmap | same |
| The span-after-decode loop: which job a sequence takes next, and whether it may join a decode batch | same |
| P1.1's per-C prompt pricing and the bound; the C = 2..7 calibration arm's fit (`FitSmallChunkArm`, rev 1.3.6 price source) | `Source/SuperSLMUnreal/Private/SuperSLMPrefillPlanning.h` |
| The red engine-verb stubs compile beside the vendored engine header | `Source/SuperSLMUnreal/Private/SuperSLMSchemaJumpAbi.h`, `SuperSLMSchemaJumpAbiStubs.cpp` (built, never linked) |

**This is a red suite.** The core's `.cpp` and the `...WithTable` functions are the suite's
stubs, so every test fails on the stub's value. The implementation replaces them. The interface in
`SuperSLMSchemaJumpCore.h` is the contract the tests are written against. The implementer may add to
it but not take anything away.

Build constraints on the core: C++17, standard library only, no exceptions, no RTTI. The suite
builds the core with `-Wconversion -Wsign-conversion -Wundef -Wshadow -Werror`, and the cores as one
unity translation unit. The pricing cells compile the plugin's own `SuperSLMPrefillPlanning.h`
through `shim/CoreMinimal.h`.

## The B7 fixtures

`fixtures/` holds two files, which `fixtures/.gitattributes` keeps byte for byte (`-text`):

- `d2-table.txt` is a verbatim copy of B7's learned table. It comes from SuperSLM aa0bc94c,
  `tools/schema_fast_decode_box/results/2026-10-21-exact/d2-table.txt`, with sha256
  `4cc216d2fe691d060887d90b12b9267afe17d1c7b97a47e575137abf7fb2b853`, and is 1,502 bytes. Plan rev 1.3.5,
  P1X.12 (a), names it.
- `b4a-b2-walked.txt` holds B4a's 20 off streams and B2's off stream, each walked through `potion_shop_order` from
  state 0. Each line gives the state before each token.
  - B4a's streams come from `results/2026-10-20/B4a.json` (sha256 `8b93415b…`), and B2's from `B2.json` (digest
    `7e46330608d11704`).
  - The transitions are SCM1's, read through the library's `SchemaMasksTable::Transition` over F-SFD, whose
    `potion_shop_order` is compiled against the same Qwen TOK1.
  - The check that the walk is the box's: the 20 walks rebuild `d2-table.txt`'s counts exactly (513 tokens, 64
    states), and no stream is cut.
  - The `D` lines are the bench's learned drafts on B2's prompt (max-new 64, budget 64). They were recomputed from
    `d2-table.txt` by the bench's own rule (`MakeDraft` and the exact loop of
    `tools/schema_fast_decode_bench.cpp` at aa0bc94c), because the bench did not log its drafts.

`fixture.h`'s `LoadB7` reads both files, with code written in the suite and not taken from the code under test. The
`P1X.12 (a)` cells build the table from the walked B4a streams with the library's flags, which is the `forced` line
of `d2-table.txt`. They then compare it with `d2-table.txt` at its forced states, and its drafts with the bench's.

## Build and run

```sh
# from this directory
CXX=g++ cmake -S . -B build-gcc -DSCHEMA_JUMP_SANITIZE=ON    # ASan + UBSan
cmake --build build-gcc -j && ./build-gcc/schema_jump_host_tests

CXX=g++ cmake -S . -B build-tsan -DSCHEMA_JUMP_TSAN=ON       # P1X.8's host half
cmake --build build-tsan -j && ./build-tsan/schema_jump_host_tests store_

CXX=clang++ cmake -S . -B build-clang                         # no sanitizers: see below
cmake --build build-clang -j && ./build-clang/schema_jump_host_tests

./build-gcc/schema_jump_host_tests draft_gate   # run the tests whose name contains a substring
```

The exit code is 0 only when every test that ran passed. The container these were authored in has
no clang sanitizer runtime, so clang builds there without sanitizers, and g++ carries ASan, UBSan
and TSan.

## Red run

Recorded on 2026-10-21 against the stubs (rev 1.3.5):

| Toolchain | Build | Result |
|---|---|---|
| g++ | ASan + UBSan, `-Werror` | 58 tests, 0 passed, 58 failed |
| g++ | TSan, `-Werror` | 58 tests, 0 passed, 58 failed |
| clang++ | no sanitizers, `-Werror` | 58 tests, 0 passed, 58 failed |

Every failure is on the stub's value: `false`, `0`, `nullptr`, an empty draft or `End`. None is a
crash or a sanitizer report. A disposable reference implementation, written only to check the
tests and never committed, passed 58 of 58 under ASan + UBSan and under TSan. Each of the mutants
XD1, XD2, XD3, XD4, XD5 and XD7 failed against it; the red-suite record lists the cells that catch
each one. XD6 is checked only by the Unreal cells.

### Rev 1.3.6 and code review round 1

Recorded on 2026-10-21 over the implementation (branch tip 2a9897f4 plus these cells). One
rev 1.3.5 cell (`p1x12c_a_fifth_token_adds_to_the_total_only`, the rule rev 1.3.6 retires) is
replaced; eleven are new and red on the suite's stubs (`ErrorOf`, `GuaranteedCountOf`,
`WalkSpanResult`, `FitSmallChunkArm`) or on the shipped code itself (the parser's range check, the
cap's keep-the-first-four rule):

| Toolchain | Build | Result |
|---|---|---|
| g++ | ASan + UBSan, `-Werror` | 68 tests, 57 passed, 11 failed |
| g++ | TSan, `-Werror` | 68 tests, 57 passed, 11 failed |
| clang++ | no sanitizers, `-Werror` | 68 tests, 57 passed, 11 failed |

The eleven: `p1x12c_a_newcomer_at_a_full_state_evicts_the_lowest_count_and_inherits_it`,
`p1x12c_the_real_token_is_never_locked_out`, `p1x12c_the_share_is_the_guaranteed_count_over_the_total`,
the four `p1x12f_*`, `shipped_table_range_check_does_not_wrap_for_a_single_digit` and the three
`pricing_small_chunk_arm_*`. A disposable reference passed 68 of 68. Against it, mutant XD4 (either
rev 1.3.5's keep-the-first-four rule or a newcomer that takes no error) fails the three `p1x12c_*`
cells, and XD10 (the stop check before the walk) fails
`p1x12f_a_stop_token_inside_a_draft_walks_every_confirmed_id_first` and
`p1x12f_b7_stop_tokens_never_mark_a_forced_state_unforced`. XD9 (no source for the per-C prices) is
the `pricing_small_chunk_arm_*` cells here and `SuperSLM.SchemaJump.P1.PriceSource` in Unreal. XD8
and XD11 are checked only by the Unreal cells.

## The Unreal cells

The automation cells are in
`Source/SuperSLMUnrealEditor/Private/Tests/SuperSLMSchemaJumpP1Tests.cpp` (P1.1 to P1.9) and
`SuperSLMSchemaJumpP1XTests.cpp` (P1X.1 to P1X.11, and P1X.12 (e)), and, for rev 1.3.6 and the
review's findings, `SuperSLMSchemaJumpReviewTests.cpp` (P1.1's price source, P1.9's public parser,
P1.10, P1.11 (a) to (f), P1X.12 (f), the drain against ground truth) through the review seam
`Public/SuperSLMSchemaJumpReviewTestAccess.h`. The fixtures are in
`Fixtures/SuperSLMSchemaJumpFixtures.h`, and the test seam is in `Public/SuperSLMSchemaJumpTestSeam.h`.

**They are uncompiled.** The authoring container has no Unreal toolchain, so the first box build
compiles them. Each cell opens with a live gate. It checks two things:

- that `SuperSLMSchemaJumpVerbsAreReal()` is true, which needs the re-vendor at engine 1.15.0;
- that `FSuperSLMSchemaJumpTestAccess::IsBuilt()` is true, which needs the implementation.

Until both hold, every cell fails by name. After that, every cell that compares a jump-forward run
with an Off run first requires that a span job admitted at least one token.

The plan's full cell-to-test map, the red reason for each cell, and the clauses that could not be
turned into cells are in the red-suite record, not here.
