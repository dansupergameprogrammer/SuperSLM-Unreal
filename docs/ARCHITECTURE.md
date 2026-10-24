# Architecture

> **Status:** the implemented Windows x64 architecture as of 1.3.0. See [PAGING.md](PAGING.md) for
> the paging API, and [BLUEPRINT_API.md](BLUEPRINT_API.md) and [TOOLING.md](TOOLING.md).

---

## Contents

- [Two layers](#two-layers)
- [How SuperSLM is built into the plugin](#how-superslm-is-built-into-the-plugin)
- [Modules](#modules)
- [Two backends](#two-backends)
- [Memory](#memory)
- [The CPU backend](#the-cpu-backend)
- [The GPU backend](#the-gpu-backend)
- [Save and restore](#save-and-restore)
- [The prefix cache](#the-prefix-cache)
- [Skip-ahead for schema-bound generations](#skip-ahead-for-schema-bound-generations)
- [Scope](#scope)

---

## Two layers

- **SuperSLM** (a separate repository) is the engine-agnostic runtime: the `.sslm` format and
  loader, the converter, the kernels, constrained decoding, the memory contract, and the
  determinism guarantee. It builds and tests with no Unreal Engine present.
- **SuperSLM-Unreal** (this repository) turns that runtime into engine assets, subsystems,
  schedulers, instrumentation and editor tooling.

In the plugin's messages, identifiers and code comments, **Layer 1** means SuperSLM, the runtime,
and the plugin is the layer above it (for example the restore result `Layer1Mismatch`, or the
message "Layer 1 refused the payload"). The documentation says SuperSLM.

The dependency runs one way. Nothing from Unreal, and nothing from this plugin, goes into
SuperSLM. If something here would be easier with a change to SuperSLM, the change is proposed
upstream on its own merits, never made as a convenience for the plugin.

| Concern | Owner |
|---|---|
| `.sslm` format, validation, kernels, decoding, determinism | SuperSLM |
| The memory contract (caller-owned pools and workspaces) | SuperSLM defines it; the plugin fulfils it |
| The model asset, import and cooking | Plugin |
| Pool ownership, sequence handles and their lifetime | Plugin |
| Frame-budget scheduling on the CPU and GPU | Plugin |
| Save-blob tagging and the refusal rules around restore | Plugin (the blob format itself is SuperSLM's) |
| Instrumentation, the self-check, editor tooling | Plugin |

## How SuperSLM is built into the plugin

The plugin compiles SuperSLM's sources with the engine's toolchain. It never links a prebuilt
SuperSLM library.

- The library sources of one exact SuperSLM commit are vendored under
  `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM/`; 1.3.0 vendors SuperSLM v1.15.0
  (165962acafe901753dfe37153d7116b132a18b5a). `VENDORED_VERSION.txt` names the tag and its commit,
  and `VENDORED_MANIFEST.sha256` records each file's hash. Five route-P witness tool files carry
  a tracked hook-API adaptation described in `VENDORED_VERSION.txt`. The build reads the tag and commit from
  `VENDORED_VERSION.txt` and compiles them into the plugin, so every report and save blob can name
  the exact SuperSLM it came from.
- At 1.0, the runtime module compiles SuperSLM's CPU library and, on Windows, its D3D12 GPU
  library, through one small wrapper file per SuperSLM source file. A check fails if SuperSLM's own
  build lists a source that has no wrapper, or if a wrapper has no source.
- The module is built with precise floating-point semantics, C++20, exceptions enabled and unity
  builds off, because SuperSLM's bit-exactness and its C++ error contract depend on them.
- The plugin uses SuperSLM's C++ headers as well as its C ABI: the GPU backend, the specific import
  diagnostics and a few other features are reachable only from C++. The C++ surface carries no
  compatibility promise between releases, so **what protects the plugin is the pinned tag and the
  from-source build**, not ABI stability. Moving the pin is a deliberate change, checked by the
  coherence workflow.

## Modules

| Module | Type | Contents |
|---|---|---|
| `SuperSLMUnreal` | Runtime | The model asset, both subsystems and their schedulers, save and restore, the self-check, the cost estimate and decode-settings commands, instrumentation, the model inspector, the Blueprint surface (the function library, the **Load SuperSLM Model** and **Run Determinism Self-Check** nodes, and `USuperSLMQuery`), and the compiled SuperSLM sources. |
| `SuperSLMUnrealEditor` | Editor | The `.sslm` import factory and asset actions, the model's asset editor, the SuperSLM Query Window, the schema dry-run, the conversion pre-check, and the automation tests. |
| `SuperSLMUnrealTests` | DeveloperTool | Tests that must run in a packaged build. |

The optional MCP toolset is a separate plugin, `SuperSLMUnrealMCP`, with one Editor module. It is
disabled by default and is not a 1.0 claim; see [TOOLING.md](TOOLING.md).

## Two backends

There are two `UGameInstanceSubsystem`s, one per backend:

- **`USuperSLMSubsystem`**, the CPU backend. Available everywhere the plugin builds.
- **`USuperSLMGpuSubsystem`**, the GPU backend. Windows only, on a D3D12 device.

Each is configured once with a model and a configuration struct (`FSuperSLMRuntimeConfig` or
`FSuperSLMGpuRuntimeConfig`), and a configuration the backend cannot honour is refused with a
named reason. Sequences are vended from a fixed pool. Every request against a sequence
(generation, reset, save, restore, adopting a shared prefix) is queued and completed through a
handle. A busy sequence is not a reason to refuse: the request queues, with one exception. A
sequence runs one generation at a time, so a generation request is refused at the call, by name
and changing nothing, unless the sequence is Idle or a reset is queued after its last generation;
each generation after a sequence's first needs a reset first. A request is refused at
request time, by name, only when it cannot be honoured, for example: a handle that is not a live
sequence, the sequence's own queue full (`MaxQueuedOperationsPerSequence`), a malformed generation
request (`MaxNewTokens` below 1, a prompt token outside the vocabulary), or, on the CPU, a backend
whose tick counter has reached its horizon (about 2^31 ticks) until `Configure()` runs again. A CPU restore, which
gets a fresh sequence rather than queueing against one, is also checked when it is requested and
can be refused at once, by name: a malformed blob, a blob from the other backend, another model or
another SuperSLM tag, an unconfigured backend, or no free sequence. **Shared prefixes are CPU-only in
1.0:** SuperSLM has no GPU verb for adopting one, so the GPU backend resolves every adopt request
`UnsupportedOnGpu`, by name.

When the GPU backend loses its device, it confirms the loss with a probe and reports it
(`IsGpuBackendActive()` turns false). A GPU call that does not return within its bound is treated
the same way, with no probe: 10 s for the per-frame calls (slices, tokens, prefill chunks, resets,
saves, restores, binds), and for the load-time calls (context creation, model and adapter maps)
the call's bytes at 142 MB/s (the slowest upload rate measured, loading the 1.5B model on the
maintainer's machine, an AMD Ryzen 9 3950X with an NVIDIA RTX 2080 SUPER), times 5,
and never less than 10 s. A query run through `USuperSLMQuery` (and the editor's query
window, which uses the same code) is then re-issued on the CPU backend from its original request,
and its readout reports that it moved (`bMovedBackend`, with a message). The partial answer is not
carried over. A caller driving `USuperSLMGpuSubsystem` directly handles the loss itself. **The
re-issue has no automated test**, because a terminal device loss cannot be produced on demand; it
is covered by code review.

## Memory

SuperSLM never allocates its KV cache or its workspaces itself; the caller provides them. The
plugin is that caller.

- **CPU backend.** At `Configure()` the subsystem sizes the KV pool from the model and
  `BlockCount` (the number of sequences resident at once, plus `PrefixBlockCount` for shared
  prefixes), and creates one workspace per concurrent worker. Save buffers are sized from
  SuperSLM's upper bound. All of it is created up front. Too many concurrent sequences are refused
  by the plugin's own pool, by name, before SuperSLM's pool could run out.
- **GPU backend.** Device memory is allocated by SuperSLM per sequence when the sequence is
  created. The subsystem reports the device-local buffers it declared as a **lower bound**; the
  driver occupies somewhat more, by an amount that depends on the driver and configuration.

**What the plugin allocates while running, exactly.** Provided the caller releases each
lifecycle handle (`ReleaseLifecycleOpHandle`), nothing the plugin keeps grows with how long a game
runs. The KV pool, the workspaces, the save staging buffers, and the CPU scheduler's
diagnostic history (the job ledger and the tick history, each a fixed ring holding the most recent
1,024 rows) are all created at `Configure()`; on the GPU, each sequence's request queue is reserved
there and compacted as requests complete. One exception on the GPU: while a generation runs,
requests resolved behind it (a save, for example) stay in the sequence's queue, so one long
generation with many such requests grows the queue past its reservation. The generation ending
does not remove them: they are removed the next time a request on that sequence completes, or
when the sequence is returned. The growth is bounded by what one generation was asked to do, and
the queue keeps that memory for the sequence's later requests. What the plugin does allocate per
request is small bookkeeping, freed when the request completes or its handle is released:

- per worker job: the queued job, its completion callback and its output record (on the GPU, the
  per-tick job and its list of actions);
- per CPU sequence: a schema bind or adapter re-point requested while a job runs, held until the
  sequence's next job applies it;
- per CPU tick: the planner's grouping of the sequences that are due;
- per generation: a copy of its prompt tokens, and room for its output tokens, reserved at
  `MaxNewTokens`;
- per lifecycle request: the result entry its handle reads, until the handle is released.
  Nothing evicts it, so a handle that is never released keeps its entry while the backend stays
  configured;
- per save or restore: the returned save blob (held until the first successful `GetSaveResult`),
  and the restore's copy of the blob it was given;
- per `USuperSLMQuery` on the GPU: one GPU-busy reading per slice, for the query's median.

Two things outside the plugin also allocate:

- SuperSLM's compute kernels keep documented, shape-stable internal scratch allocations. A
  correctly sized workspace removes SuperSLM's own transient buffers at the call boundary, not
  those.
- Each token's finish (the final norm, the logits over the vocabulary and the choice of token)
  runs across `FinishParallelTasks` tasks on the engine's task system (default 4). Above 1, the
  engine's `ParallelFor` allocates its own per-call bookkeeping. Set `FinishParallelTasks` to 1 to
  remove that allocation, at the cost of a serial finish. Tokens are identical at every setting.

## The CPU backend

The game thread never runs inference, and its per-frame work never waits on it. A few calls
outside the frame do block their caller, reconfiguring among them (the teardown of the previous
configuration joins the worker threads); they are listed under *Blocking calls* in
[BLUEPRINT_API.md](BLUEPRINT_API.md#threading).

- The host calls the subsystem's `Tick(DeltaSeconds)` once per frame. `Tick` does bookkeeping
  only: it delivers results the workers have finished, then plans the next job and hands it to a
  plugin-owned worker thread (`MaxConcurrentCalls` workers, default 1). Every inference and
  lifecycle call into SuperSLM runs on a worker. A few short calls run on the calling (game)
  thread: tokenizing (`sslm_tokenize`), statistics reads (`sslm_stats`), a schema bind, and an
  adapter re-point applied at the call.
- A result is delivered at the first tick after its worker finishes it, in order per worker. A
  result that arrives later than planned is counted as a hitch and reported; nothing is reordered
  or dropped.
- **Job sizing.** A job is sized so its planned cost fits `TickBudgetMs × BudgetHeadroom`
  (`BudgetHeadroom` defaults to 0.7). `TickBudgetMs` bounds the worker's CPU time per job, not the
  game's frame. The planned cost comes from **fixed per-unit costs in the configuration**:

  | Field | Mode 0 / mode 1 default (ms) | Priced unit |
  |---|---|---|
  | `LayerCostMs` + `LayerCostPerPositionMs` × depth | 1.397 + 0.000186 × depth / 0.906 + 0.000254 × depth | one decode layer at a given context depth |
  | `PromptTokenCostMs` + `PromptTokenCostPerPositionMs` × depth | 33.583 + 0.004547 × depth / 22.108 + 0.005209 × depth | one prompt token, in a prefill call of fewer than 8 tokens |
  | `BatchedPromptTokenCostMs` + `BatchedPromptTokenCostPerPositionMs` × depth | 15.023 + 0.004791 × depth / 15.682 + 0.004437 × depth | one prompt token, in a prefill call of 8 tokens or more |
  | `FinishCostMs` | 5.836 / 5.929 | one token finish |
  | `ResetCostMs`, `AdoptCostMs`, `SaveCostMs`, `RestoreCostMs` | 2.369, 2.648, 2.691, 4.644 / 2.397, 2.731, 2.744, 4.766 | one lifecycle operation |
  | `PrefixBeginCostMs`, `PrefixReleaseCostMs` | 2.261, 2.247 / 2.425, 2.440 | one shared-prefix operation |

  Mode 0 is decode threading off and mode 1 is `bParallelDecode` on (below). The two profiles are
  rounded-up medians from six per-call capture blocks on one machine (an AMD Ryzen 9 3950X, Zen 2,
  with the 0.5B example model, `FinishParallelTasks` 4): reasonable measured starting costs, not
  certified ones. An under-priced cost lets a job run past its CPU share; an over-priced one only
  lowers throughput. Layers timed inside a real job have read higher than layers timed alone, and
  the job's planned and measured times are both reported, so a project can check its own.
  In budget mode, the three adopt/restore/prefix-Begin defaults come from the separate
  full-cap measurements in [PAGING.md](PAGING.md#7-cpu-scheduling-costs). Legacy mode and
  the other ten default fields retain the table above; caller and saved values keep priority.
- **How each field resolves.** The decode mode, `FinishParallelTasks` and the thirteen cost fields
  are optional (`TOptional`). An assignment sets a field whatever its value. At `Configure()` (or
  `BeginConfigure()`, on the calling thread) each field resolves on its own: the caller's value if
  set; else, for the mode and task count, the running editor's `SuperSLM.SetDecodeMode` /
  `SuperSLM.SetFinishParallelTasks` choice, then the value saved in `Saved/SuperSLM/RuntimeSettings.ini`,
  then off and 4; for a cost field, a saved estimate in `Saved/SuperSLM/CalibratedCosts.ini` for the
  same artifact, effective mode and task count, then the effective mode's profile above. The
  effective mode is `bParallelDecode` with `FinishParallelTasks` above 1, because the decode
  projections ride on the finish hook. `FSuperSLMConfigureReport::Resolved` and
  `USuperSLMSubsystem::GetResolvedConfig()` carry what was resolved, and `Configure()` logs
  `SuperSLM: CPU Configure: bParallelDecode=<0|1> FinishParallelTasks=<n> cost profile=<caller|saved-estimate|mode-default> caller_cost_fields=<0..13>`.
  A saved estimate with a missing, non-finite or out-of-range value, or one that would put a job
  past the scheduler's tick limit where the mode profile would not, is skipped with one warning.
- **Decode threading.** `bParallelDecode` (off by default) splits each CPU one-row decode
  projection across the finish hook's tasks. Tokens are identical either way.
  `SuperSLM.SetDecodeMode 0|1 [--save]` and `SuperSLM.SetFinishParallelTasks <0..256> [--save]`
  choose the mode and task count for the next configuration that leaves them unset; a
  configuration already made keeps its settings. `--save` keeps the choice in
  `Saved/SuperSLM/RuntimeSettings.ini`, which later sessions, packaged games included, load.
- **Estimating costs on your hardware.** `SuperSLM.EstimateCosts "<path-to-model.sslm>" [--write-ini]`
  measures both decode modes on the machine it runs on, at the task count the next configuration
  would resolve, and prints a `CALIBRATION ESTIMATE — not verified`: per-arm medians and spread,
  thirteen suggested costs per mode, and the CPU load other processes put on the machine while it
  ran. Desktop load is reported, never a reason to stop. With `--write-ini` it saves both modes'
  suggestions to `Saved/SuperSLM/CalibratedCosts.ini`, which the next `Configure()` reads for a
  matching artifact, mode and task count; without it nothing is written. See
  [TOOLING.md](TOOLING.md).

- **Prefill chunks.** A prefill job carries at least `MinPrefillChunk` prompt tokens (default 8)
  while the prompt has that many left and `MaxPrefillChunkBudget` allows it, and more when their
  price still fits the share. Eight is where SuperSLM's prefill matrix multiply switches to its
  tiled kernel. A chunk costs more than a typical `TickBudgetMs`; the job still runs, on the
  worker, and simply takes more ticks to deliver. Set `MinPrefillChunk` to 1 for one-token prefill
  jobs, as in 1.0. Tokens are identical at every setting. Time to first token is reported.
- **Overruns.** A job is a worker overrun when its worker time exceeds its own planned ticks,
  K × `TickBudgetMs`. A job planned for K ticks is not an overrun at tick 2.

## The GPU backend

- **Fixed-tick delivery.** A token requested at tick T is applied at tick T + K, where K is a
  configured constant that the plugin never adapts to load or timing. It changes only through a
  call, while no sequence is vended: `SetFixedTickLatency` sets it, `SetLayersPerTick` raises it to
  the new minimum when it falls below, and a GPU `USuperSLMQuery` sets it to its own k and restores
  the configured schedule once its sequences are returned. The subsystem reports the minimum K the
  configuration allows. A device that misses T + K produces a counted hitch; work is never
  reordered.
- **One submission thread.** All GPU work, including restore and adapter binds, is issued from one
  dedicated thread that is never the game thread, and every submission is drained before the next.
  The per-frame calls never wait on it; `MapAdapter`, `UnmapAdapter`, `ProbeContextUsable` and a
  reconfigure (which waits for this thread's queued backlog and the teardown, then joins it; it
  leaks the thread with no wait if it is already unresponsive, and after the stuck call overruns
  its time bound if it stops responding during the wait) do, so they are not for the frame path (see *Blocking calls* in
  [BLUEPRINT_API.md](BLUEPRINT_API.md#threading)).
- **Slicing within a token.** Each sequence uses one of two paths, chosen at creation or reset and
  never switched while it runs:
  - the **composed path** splits a token into a caller-set number of whole layers per tick. At 4
    layers on an RTX 2080 SUPER with the 0.5B model, a slice measured about 2 ms (see the
    [Measured section of STATUS.md](STATUS.md#measured)). Concurrent sequences share the tick's
    budget through SuperSLM's batch call, in an order that rotates each tick so none starves;
  - the **one-call path** runs a whole token per call, one token per tick across all its
    sequences in rotation.

  Prompts are sliced the same way, so no prompt is too long for one call: the only ceiling is the
  context size (`context_cap`), where a sequence faults by name.
- **The device-resident head.** By default, each token's logits are computed on the GPU in one
  extra small submission per token, and the host keeps only the final choice of token. This is a
  property of the model asset (`bGpuDeviceResidentHead`), not of the `.sslm` file. With it off,
  the logits run on the CPU through the engine's task system, as on the CPU backend. If the head's
  buffers cannot be allocated when the model is mapped, the backend falls back to the head off
  once, and reports it.

  The head's per-token dispatch is outside the slice budget, and its GPU time is not included in
  the per-slice GPU time the backend reports. The host-side finish time the backend reports is an
  upper bound on it.

**The slice size is the caller's.** `FSuperSLMGpuRuntimeConfig::DispatchBudget` sets how many
SuperSLM dispatches one tick may issue; the backend issues the whole layers that fit
(`DispatchBudget` divided by the dispatches per layer). One layer is 24 dispatches for a model
without QK-norm (the example model) and 25 for a model with it, so 4 layers per slice on the
example model is `DispatchBudget = 96`. It has no default: `Configure()` refuses a value below one
layer's dispatches.
Where a figure below names a "default", it names its host:

- **The example project** runs a whole token per frame (the one-call path), as the Load node and a
  default query do. Heavier scenes can set 4 to 12 layers per slice. The measured slice times at
  each setting are in the [Measured section of STATUS.md](STATUS.md#measured).
- **The Load SuperSLM Model node** sets the tick's budget to the model's whole depth, shared by the
  sequences in flight. Set a smaller slice from C++, or per query with
  `FSuperSLMQueryConfig::FrameBudgetLayers`.

With the engine rendering, the per-slice GPU time the backend reports is the span from the slice's
first GPU timestamp to its last, and it includes the renderer's work on the same GPU inside that
span. It bounds the slice's own cost from above.

## Save and restore

A save produces opaque bytes bound to the model: restoring against a different model is refused.
The plugin adds a small wrapper that records the backend and the SuperSLM tag and commit. It
refuses, by name, a restore onto the other backend, and a restore of a blob saved by a build
pinned to a different SuperSLM tag. **So a save does not carry across a change of the plugin's
SuperSLM pin.** The plugin does not design your game's save format: it hands over the bytes, and
your save system stores them.

## The prefix cache

A game often starts many generations from the same opening: a world description, an NPC's persona, a shop's rules.
The prefix cache keeps those openings, called prefixes, already computed, so a generation starts from a stored prefix
instead of prefilling it again.

**Turning it on.** Set `FSuperSLMRuntimeConfig::PrefixCacheBudgetBytes` (0, the default, is no cache). In budget mode
the cache takes that many bytes of the page pool. In legacy mode the pool grows by enough blocks for that many bytes. For the disk tier,
also set `bPrefixDiskCacheEnabled`. `PrefixDiskCacheDirectory` defaults to `<ProjectSaved>/SuperSLM/PrefixCache`, and
the directory is trimmed to `PrefixDiskCacheMaxBytes` (256 MiB by default), oldest file first. A write's temporary file
that a crash left behind is removed at the next trim once it is an hour old. A re-`Configure()` empties the memory
tier, and disk files stay. Registered prefix sets stay while the model stays the same: a `Configure()` of another model
unregisters the sets built for a different one (logged `PrefixSetModelMismatch`), so register them again after
configuring their model. In 1.3.0 the budget is set in C++: the **Load SuperSLM Model** node has no input for it.

**Using it.** `RequestPrefix(Spec, OutPrefix, OutError)` takes a spec: the prefix's tokens, its schema name
(`NAME_None` for none) and, for a nested prefix, its parent. It returns a lease at once. When `GetPrefixPhase` reads
Ready, adopt the lease with `AdoptPrefix`, as you would a shared prefix. Return it with `ReturnPrefix` when you are done
with it. A returned prefix stays cached until it is evicted. `GetPrefixLeaseState` names a refusal:

- `BudgetExhausted`: the prefix does not fit the budget even after every possible eviction.
- `PoolExhausted`: the page pool is short.
- `UnknownSchema`: the schema name does not resolve.
- `Cancelled`: a re-`Configure()` came while the lease was pending.
- `Failed`: the build failed. The log names the reason.

**Where a prefix comes from.** A request is served from the first tier that has it, and `GetPrefixSource` says which:

1. **Memory**: a prefix already resident. Nothing is restored or built.
2. **Cooked**: an entry of a registered prefix set (below), restored from the set's bytes.
3. **Disk**: a file the disk tier wrote in an earlier session, restored.
4. **Recomputed**: built by prefill, as `CreatePrefix` builds a shared prefix. With the disk tier on, the result is
   also written to disk.

The tokens a generation writes from the prefix are the same whichever tier served it. The automation and packaged
checks compare every tier's tokens with a recompute's, and the determinism self-check gives the same verdict with
the cache on as with it off.

**The key.** An entry is found by:

- its tokens;
- its schema;
- its parent, for a nested prefix;
- the model;
- the engine that built it, which is the backend, the vendored SuperSLM build and, on the GPU, the adapter's vendor
  and device.

So a cooked entry or a disk file built for another model, another SuperSLM release or another GPU is never served.
Its request is a miss, and the prefix is built by prefill.

**Memory and eviction.** Each resident prefix is charged its pages: a 128-token prefix of the 0.5B example model holds
768 KiB. When a new prefix does not fit, the least recently used prefix that nothing is using is evicted. A prefix is
in use while it is leased, or while a cached nested prefix still maps its pages. `GetPrefixCacheStats` reports the
charge, resident bytes, evictions and the tier that served each request.

**Nested prefixes** (budget mode only). A spec with a `Parent` lease is a suffix on that parent: the nested prefix
shares the parent's full pages and holds only its own. For example, a world description can be the parent, and each
NPC's persona a nested prefix on it. A child takes its parent's schema. Legacy mode refuses nested specs with
`NestedRequiresBudgetMode`.

**Level loads: warm and drop.** `WarmPrefix(Spec)` is a request that takes no lease: when it resolves, the prefix is
resident and evictable like any unleased one. Warm a level's prefixes while it loads, and its first requests are
`Memory` hits. `DropPrefix(Spec, Scope)` evicts a prefix now, or when the last thing using it lets go.
`MemoryAndDisk` also deletes its disk file. Neither call ends or reaches through a lease, and neither touches a cooked
set. Both return an op, which resolves on a later tick: read it with `GetPrefixOpResult`.

**Prefix sets: cooked prefixes.** A `USuperSLMPrefixSet` asset names its model and lists entries. Each entry is
defined by its token ids or by text, with an optional schema and an optional parent entry.

- The editor's **Build Prefixes** action builds the set in the background: tokenize the text entries, prefill each
  entry, and store its bytes in the asset. The progress notification has a Cancel button. A cancelled build, or one
  whose set was edited while it ran, stores nothing.
- The `SuperSLMBuildPrefixes` commandlet runs the same build for a build machine. Its output is byte-equal to the
  action's.
- The game never tokenizes a set's text: the ids stored at the build are the key.
- The cook checks every entry. It logs a warning naming each entry the game would not use: one not built, built for
  another model or another SuperSLM build, or edited since its build. The game builds such an entry by prefill when
  it is requested. A text entry whose text or model changed ships with no ids and no bytes.
- **Rebuild your sets whenever the plugin's vendored SuperSLM changes.**
- In 1.3.0 a nested entry is not built; the build says so, and its requests are built by prefill.
- `RegisterPrefixSet(Set)` adds a set's entries to the cooked tier, and registering the set again after a rebuild
  replaces them. `FindEntrySpec(ConfiguredModel, Entry, OutSpec, OutParentEntry)` gives the spec to request an entry
  by name.

**How the work is scheduled.** Every cache operation runs on a worker lane and is planned like other jobs:

- **A restore** is priced at `RestoreCostMs` plus `RestoreCostPerMiBMs` per MiB of its bytes.
- **A build by prefill is sliced.** Each chunk is one prefill call of at least `MinPrefillChunk` tokens, and
  generations on the same lane run between the chunks. A 2,000-token build ran as 250 chunks of 8 tokens, with at
  most one generation job between two of its chunks. The tokens and the saved bytes equal a single-call build's.
- **The prices come from `SuperSLM.EstimateCosts`.** It measures a restore's per-MiB cost and the build's prefill
  price. `USuperSLMSubsystem::WriteInSituPrefixPrefillPrice` refits the build's price from a build that ran inside the
  game. On an AMD Ryzen 9 3950X with the 0.5B example model, the in-game price was about 1.5 times the one measured
  alone.

**What it costs, measured.** These readings are from a packaged Development build on an AMD Ryzen 9 3950X with the
0.5B example model. They were taken at SuperSLM 1.14.0's engine; 1.15.0 does not change prefill or prefix restore.

| What | Measured |
|---|---|
| Restore a 128-token prefix from a cooked set | 0.92 to 1.01 ms of worker time (10 restores) |
| Build a prefix by prefill, timed alone | about 15.5 ms per token plus 0.0046 ms per position: about 2.0 s for 128 tokens |
| Build a prefix inside a running game (sliced, 2,048 tokens) | a median of 30.1 ms per token |
| Longest game-thread tick while 10 cooked prefixes loaded | 0.24 ms (3,828 ticks) |

Worker time is not frame time: the plugin spreads a long job over frames, and the game thread only hands jobs out and
collects their results.

**On the GPU.** The cache is CPU-only in 1.3.0. `USuperSLMGpuSubsystem` declares the same calls
(`ConfigurePrefixCache`, `RequestPrefix` and the rest, for pooled sequences, keyed by the adapter), but they are not
yet connected to the GPU scheduler, so they refuse `NotConfigured`. SuperSLM 1.15.0 also does not release GPU prefix
save and restore, which the GPU cache's cooked and disk tiers need.

## Skip-ahead for schema-bound generations

When a schema is bound, there are stretches where it allows only one text: the rest of a key name, a quote, a colon,
a closing brace. Normally the model still spends one decode on each of those tokens. Skip-ahead (the
`SchemaJumpForward` setting) writes such a stretch in one batched pass through the prefill path. It is set per
generation on `FSuperSLMGenerationRequest`, and it is CPU-only: the GPU backend ignores it. One decode job can mix
sequences in different modes.

| Setting | Tokens | How it works |
|---|---|---|
| `Off` (default) | the model's own | One decode per token. |
| `Exact` | identical to `Off`, and so is the save | The plugin guesses the next tokens, and SuperSLM checks each guess with the model's own decode rule. It keeps only the guesses the model would have written. A wrong guess costs time, never a different token. |
| `Canonical` | fewer, and different | SuperSLM writes the schema's own spelling of the forced text, for example `"name"`, even where the model would have split it into other tokens. The model then continues from text it did not write. |

**Where `Exact`'s guesses come from.** The plugin learns them per model and schema, from that schema's earlier
finished generations in this configuration. It learns from `Off` and `Exact` generations that reached the end of
their schema, never from `Canonical` ones. It learns only at points where the schema allows one text, and keeps at most
4 tokens per point. It stops guessing where its history is unsure. What it learned starts empty at every
`Configure()`, is dropped when the model is unmapped, and is never saved.

So `Exact` speeds up a schema the game uses repeatedly. Its first generations, before anything is learned, run as
`Off` does. The schema's walk table loads on a pool thread at the first schema-bound `Exact` or `Canonical`
generation. Until it has loaded, `Exact` guesses nothing.

**What it buys.** SuperSLM measured skip-ahead with the same model and the same CPU as the table above. The prompt was
the example's potion-shop order: 41 prompt tokens, and 28 tokens to the schema's end. The times are generation only,
the median of 15 runs.

| Setting | Tokens written | One thread | 4 tasks (threaded decode) |
|---|---|---|---|
| `Off` | 28 | 897 ms | 549 ms |
| `Exact`, guessing from earlier orders | 28, the same | 604 ms (1.48x) | 557 ms (about even) |
| `Canonical` | 21, different | 471 ms | 456 ms |

Over 20 other orders, `Exact` took 12,878 ms against 16,676 ms off, 1.3 times faster in total. The median order was
1.42 times faster, and one order was slower (0.82x). `Exact` pays only when most guesses are right: about 60% of the
guessed tokens must be accepted before it gains, and a wrong guess costs about 0.6 of a decode. With threaded decode
on (`bParallelDecode` with `FinishParallelTasks` above 1), `Exact` is about even with `Off`, and the plugin caps its
guesses at 6 tokens.

These are SuperSLM's timings, not timings inside a running game. In the game, the scheduler sizes a skip-ahead pass
from the prices of 2- to 7-token prompt chunks. `SuperSLM.EstimateCosts` measures those prices. Until a saved estimate
has them, each pass is priced as that many one-token prompt calls, which over-prices it: the pass takes more ticks to
deliver, and the tokens are unchanged.

**Rules it keeps.**

- Skipped tokens count toward `MaxNewTokens`.
- In `Exact`, a pass never writes the generation's last allowed token. So at a `MaxNewTokens` cut, an `Exact` sequence
  saves exactly the state an `Off` one saves. A `Canonical` pass may write it, so a `Canonical` save at a cut is not
  `Off`'s (its tokens differ anyway).
- A guess stops before the first of the request's `StopTokenIds`. So an `Exact` generation stops where `Off` stops,
  with the same save.
- Skip-ahead needs to know where the sequence is in its schema. That is true of a fresh sequence, and of one that
  adopted a prompt-only prefix. On a restored sequence it is not known, so `Exact` runs as `Off` does there, and
  `Canonical` still writes the forced text.
- An `Exact` or `Canonical` generation does not use the worker's decode continuations.

**Choosing a mode.** Use `Exact` for a schema the game fills many times, at one decode thread. The output is
unchanged, so nothing else in the game needs to know. Use `Canonical` only after checking its output for your
schema. Its tokens differ from `Off`'s, so its token digests and determinism comparisons differ too. On SuperSLM's 40
test orders, 32 came out different from `Off`, and the quality verdict was inconclusive.

## Scope

In scope: the `.sslm` asset and cooked serialization; engine-owned KV and workspace memory on the
CPU backend; the CPU and GPU schedulers; the prefix cache and cooked prefix sets; skip-ahead on the CPU backend;
a Blueprint surface; the determinism self-check as an
editor tool and a packaged-build entry point; the editor tooling; a minimal example project.

Out of scope: any change to SuperSLM; game-specific logic built on top of the plugin; the design
of your game's save format.
