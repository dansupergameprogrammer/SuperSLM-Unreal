# Paged KV and shared prefixes

New in SuperSLM-Unreal 1.2.0, on SuperSLM 1.13.1. Paging is off unless you turn it on: with
`SequenceBudget` left at zero, a 1.0 project configures, generates and saves as before.

## Contents

1. [Select a mode](#1-select-a-mode)
2. [Size the page pool](#2-size-the-page-pool)
3. [Use a prefix](#3-use-a-prefix)
4. [Handle the generation limit](#4-handle-the-generation-limit)
5. [Restore and normalize](#5-restore-and-normalize)
6. [Read results and statistics](#6-read-results-and-statistics)
7. [CPU scheduling costs](#7-cpu-scheduling-costs)
8. [Saves across releases](#8-saves-across-releases)
9. [GPU pool size and the AMD card](#9-gpu-pool-size-and-the-amd-card)

## 1. Select a mode

`FSuperSLMRuntimeConfig` and `FSuperSLMGpuRuntimeConfig` both have `SequenceBudget`.
Zero selects the existing CPU legacy or GPU dedicated mode. A positive value selects the
shared page pool and gives each warm sequence that many positions of generation budget.
Configure refuses a negative value or one above the supported context cap by name.

Keep the existing required call-shape settings. For example, add these assignments to an
already valid CPU configuration for fifty prefix adopters:

```cpp
Config.BlockCount = 50;
Config.SequenceBudget = 512;
Config.PrefixBlockCount = 1;
Config.RestoreHeadroomBlocks = 1;
```

The Blueprint **Load SuperSLM Model** node exposes `SequenceBudget` as its final input,
defaulting to zero for existing graphs. A positive budget enables paging on both backends;
`PrefixBlockCount` then applies to the CPU and pooled GPU. Loading refuses to replace a
backend that still owns a vended sequence, live prefix or mapped GPU adapter.

For the GPU, also configure `ContextCap`, `DispatchBudget` and `K`. Increasing concurrency
can increase the required `K`; inspect `FSuperSLMGpuConfigureReport::MinimumK` and `Message`.
Successful reconfiguration creates a new configured epoch and invalidates old handles.

## 2. Size the page pool

The ledger reserves separate regions for warm sequences (`S`), prefix builds (`P`) and
restore headroom (`H`). `BlockCount`, `PrefixBlockCount` and `RestoreHeadroomBlocks` control
them. On the CPU, prefix and headroom blocks cover the model's full context. On the GPU,
those blocks cover the largest prefix or restore charge admitted at its configured
`ContextCap`; the engine's model cap can be larger. A warm budget sequence reserves only
the pages needed for its budget. Read `PagePositions`, `PageBytes` and the region capacities from
`GetKvPageStats()` instead of hard-coding a page size.

`RestoreHeadroomBlocks = 0` refuses restores in budget mode. A prefix or restore whose charge
cannot fit its region is refused as `KvPagesExhausted`, leaving the prior sequence untouched.
GPU pool creation can report `KvPoolRefused`; its message names the allocation status and size.
The GPU pool has a system-memory mirror of the same byte size as its device buffer. Include
both in memory planning; declared GPU residency counts the device buffer once.

`CacheGhostBlocks` is accepted and ignored in 1.2.0. `CacheSharing` and
`Cache` statistics remain zero; the shared-prefix API is explicit.

## 3. Use a prefix

Create a prefix with token IDs through `CreatePrefix`, check its result, and tick the subsystem
until `GetPrefixPhase()` reports `Ready`. The CPU retains its existing bool overload and also
offers the result-returning overload. The GPU prefix API requires pooled mode.

Queue an adopt on each vended sequence and wait for its lifecycle result before starting the
dependent generation. Whole prefix pages are shared; a partial tail is copied as needed.
`CreatePrefixFrom` extends a ready parent with additional tokens. It requires budget mode;
an unknown, released or unfinished parent returns `ParentNotUsable`.

`ReleasePrefix` removes the caller's usable prefix handle. Existing adopters and nested
children retain their shared pages: the released parent can remain a ghost until its last
dependent releases it. A queued adopt, nested build or hinted restore pins its parent through
application; a release while such a pin is open is refused with a `PrefixPinned` message.
Continue ticking while queued operations finish, then retry the release.

## 4. Handle the generation limit

A budget holder's limit is `min(origin + budget, context cap)`. Adopting a prefix establishes
its origin; a generation's prompt consumes positions within the remaining budget. Reaching
a below-cap budget limit faults that sequence with `KvBudgetExceeded`: read the CPU's
`GetLastDecodeOutcome()` or the GPU's `GetLastFaultReason()` by enum name. An earlier requested
stop can still complete the generation normally. The existing cap outcome applies at the cap.

For the tested 1,000-token persona, budget 512 and eight-token suffix, with no earlier stop,
each sequence emits `1 + (1512 - 1008) = 505` tokens before its budget stop. The extra ready
token is part of the runtime's token-boundary rule. Budget 512 therefore does not mean 512
output tokens after an arbitrary prompt.

## 5. Restore and normalize

Restore checks the wrapper's backend/tag, artifact, binding and payload before using it.
A sharing hint can reuse matching prefix pages; it does not permit a mismatched schema,
adapter or artifact. Keep the hinted prefix alive until the operation applies.

| Payload and target | Result |
| --- | --- |
| CPU budget `SSB6` into CPU legacy mode | `KvModeMismatch` |
| CPU legacy `SSB5` into CPU budget mode, with a compatible wrapper | Accepted as a whole-reserve holder; reset normalizes it when headroom is available |
| Adopt onto that restored whole-reserve holder before normalization | `KvModeMismatch`, with a reset-first diagnostic |
| GPU `SLM5`/`SLM6` crossing dedicated/pooled modes | `KvModeMismatch` |
| Valid GPU pooled state above the configured GPU `ContextCap` | `ContextCapMismatch` |

A whole-reserve CPU holder normalizes on reset only when the replacement reservation fits
free headroom and no earlier headroom waiter takes priority. Otherwise reset succeeds in
place, keeping the whole-reserve shape; a later reset can retry normalization. With
`RestoreHeadroomBlocks = 0`, reset always keeps that shape.

**Restore bursts.** In budget mode every restore, with or without a sharing hint, is charged
against the restore headroom (`RestoreHeadroomBlocks`, default 1) when it is admitted. A restore
that does not fit waits only when the restores ahead of it will free enough; otherwise it is
refused `KvPagesExhausted` on arrival, by name, and the sequence it targets is untouched. With
the default headroom, the 0.5B example model at cap 4,096 takes two restores at once and one
waiting, on either backend; with the 1.5B model at cap 32,768 the CPU takes 21 at once and the
GPU 5, with 2 waiting. A loader that restores many sequences at once should pace itself with
`GetKvPageStats()` (free headroom, waiting and in-flight restores) or raise
`RestoreHeadroomBlocks`, which widens the burst linearly. Restores are restore-first: the new
holder is drawn before the old one is released, so a refusal never loses the sequence.

Reset and restore are queued operations. Observe their completed results before depending
on a new holder shape. Releasing or reconfiguring an owner can end a wait with
`NotConfigured`; a pending result is not a promise that it will eventually succeed.

## 6. Read results and statistics

Check the named Configure, prefix and lifecycle results. An admitted request can remain
`Pending` while waiting or executing. Keep driving `Tick()` and read its completed result;
an elapsed frame count or an idle worker counter does not establish application.

`GetKvPageStats()` exposes region capacity/charge, live prefixes, ghosts, waiting/in-flight
restores and free restore headroom. Its engine pool snapshot is from the last applied job.
`LedgerFaults` counts unexpected engine pool exhaustion reached by the plugin's own admitted
requests and survives reconfiguration, including in Shipping. It should remain zero.

## 7. CPU scheduling costs

Cost selection and the distinction between planned and measured work are described in
[ARCHITECTURE.md](ARCHITECTURE.md#the-cpu-backend). Caller values take priority over matching
saved estimates, which take priority over shipped defaults. A matching saved estimate applies
to all unset cost fields, including adopt, restore and prefix Begin in budget mode.

Budget mode replaces the defaults for three fields, by the actual artifact cap and effective
decode mode. The other ten fields keep the profiles in ARCHITECTURE.md.

| Actual cap tier | Effective mode | Adopt (ms) | Restore (ms) | Prefix Begin (ms) |
| --- | --- | --- | --- | --- |
| At most 4,096 | 0 | 0.009 | 3.576 | 0.001 |
| At most 4,096 | 1 | 0.008 | 3.558 | 0.001 |
| Larger | 0 | 0.020 | 90.795 | 0.002 |
| Larger | 1 | 0.020 | 90.442 | 0.002 |

The measurements used real 0.5B/cap-4,096 and 1.5B/cap-32,768 artifacts on the reference
Zen 2 desktop, with four finish tasks. Each mode ran three sequential blocks, with fifty
warm-up and fifty timed calls per arm/block: a 1,000-token adopt, full-cap restore and
full-budget prefix Begin. Each default is the median of the three timed block medians,
rounded upward to 0.001 ms. Whole-run foreign CPU load was disclosed; no idle condition or
accuracy bound is claimed.

`SuperSLM.EstimateCosts` measures its legacy workloads. A matching saved profile overrides
these three budget defaults as well as the other unset fields. Use caller cost overrides when
the project needs different budget-mode estimates. These starting costs do not certify a
latency bound for another machine or workload.

## 8. Saves across releases

Every save names the SuperSLM release it was made with, and a restore refuses a save from a
different release by name, `Layer1Mismatch`, before reading its payload. 1.2.0 moves the
vendored SuperSLM from v1.9.0 (1.0.x) to v1.13.1, so:

- A save made by 1.0.x is refused by 1.2.0 with `Layer1Mismatch`, on both backends and in both
  modes. The refusal was run against saves from a build on SuperSLM 1.11.0; a 1.0.x save names
  v1.9.0 and reaches the same check, which was read from the restore code, not run.
- A save made by 1.2.0, in either mode, is refused by 1.0.x the same way: 1.0.x applies the same
  check against its own release. That direction was read from 1.0.x's restore code, not run.

Plan save formats with this in mind: a game that keeps player saves across this update must
regenerate the sequence instead of restoring it. Within 1.2.0, a legacy-mode CPU save (`SSB5`)
restores into budget mode as described in section 5, and the GPU's dedicated and pooled saves
(`SLM5`, `SLM6`) do not cross modes.

## 9. GPU pool size and the AMD card

The GPU pool is one D3D12 buffer, so its size is capped at the largest buffer D3D12 guarantees on
the card: a quarter of the card's dedicated video memory, at most 2 GiB and at least 128 MiB.
A pooled configuration that needs more is refused at `Configure()` with `KvPoolRefused`, naming
`SSLM_GPU_KV_POOL_TOO_LARGE` and the page count. On a card with 8 GiB or more that is 21,845 pages
with the 0.5B model, whose GPU pages hold 16 positions (98,304 bytes; the GPU page is the model's
K/V block size, not the CPU's page rule); an RTX 2080 SUPER reports slightly less than 8 GiB, so its
largest pool is 21,298 pages. The 50-NPC example's pool is 1,906 pages, 178.7 MiB.

Checked on an AMD Radeon RX 7900 XTX (driver 32.0.23033.1002): the full editor test battery,
243 tests in legacy and paged mode, and the packaged 50-NPC GPU example run without a renderer
all pass. **Known issue:** on that card and driver, the packaged example hangs the GPU while
rendering its scene, even with no model loaded and no SuperSLM work running, so SuperSLM's GPU
backend running beside Unreal's renderer is not certified on it. The rendering-timing test
(the GPU slice sweep with rendering) is certified on the RTX 2080 SUPER only.
