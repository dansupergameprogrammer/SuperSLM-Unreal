# Blueprint and C++ API

> **Status:** the C++ API on the two subsystems and the Blueprint surface are built and tested.

---

## Contents

- [The C++ API (built)](#the-c-api-built)
- [The Blueprint surface](#the-blueprint-surface)
- [Results and refusals](#results-and-refusals)
- [Threading](#threading)

---

## The C++ API (built)

Both backends are `UGameInstanceSubsystem`s: `USuperSLMSubsystem` (CPU) and
`USuperSLMGpuSubsystem` (GPU, Windows). The CPU subsystem's surface, in the order a caller uses
it:

| Step | Call |
|---|---|
| Configure once with a model and a configuration | `Configure(Model, FSuperSLMRuntimeConfig)`, returns a report with a named result; `BeginConfigure(Model, Config, OnDone)` does the mapping and allocation on a pool thread and delivers the same report through `OnDone` |
| Drive the scheduler once per frame | `Tick(DeltaSeconds)`, or `TickOncePerFrame(DeltaSeconds)` when more than one caller ticks the subsystem (it ticks at most once per engine frame) |
| Get and return a sequence | `VendSequence`, `ReturnSequence` |
| Bind a schema (at the start of a sequence) | `SetSchema` |
| Choose how many layers each decode call on a sequence runs (on a new or reset sequence; at most `MaxLayerBudget`) | `SetLayerBudget(Sequence, LayerBudget)` |
| Choose an adapter | `RequestAdapterSwap`, `GetActiveAdapter` |
| Tokenize text | `Tokenize` |
| Start a generation | `BeginGeneration(Sequence, FSuperSLMGenerationRequest)` |
| Read progress and output | `GetPhase`, `GetGeneratedTokens`, `GetLastDecodeOutcome`, `GetStats` |
| Lifecycle operations, queued | `ResetSequence`, `SaveSequence`, `RestoreSequence`, `AdoptPrefix`, each returning a handle |
| Read a queued operation's result, then release its handle | `GetLifecycleOpResult`, `GetSaveResult` (which also returns the blob), `ReleaseLifecycleOpHandle` |
| Shared prefixes | `CreatePrefix`, `GetPrefixPhase`, `IsPrefixReady`, `AdoptPrefix`, `ReleasePrefix` |
| Diagnostics (timings, tick reports, pool and memory counts) | `GetLastTickReport`, `GetTickHistory`, `GetJobLedger`, `GetTokensPerSecond`, `GetTimeToFirstTokenMs`, `GetPoolFreeCount` and the other `Get*` readers in the header |

The GPU subsystem follows the same shape, with its own configuration
(`FSuperSLMGpuRuntimeConfig`: `ContextCap`, `BlockCount`, `DispatchBudget`, `K`, `TickBudgetMs`,
`MaxQueuedOperationsPerSequence` and `FinishParallelTasks`) and a choice of decode path per
sequence (`ESuperSLMGpuDecodePath`). `DispatchBudget` is required and has no default. Its unit is
SuperSLM dispatches, not layers: one layer is 24 dispatches for a model without QK-norm (the
example model, Qwen2.5) and 25 for a model with it, so N layers per slice is
`DispatchBudget = N * 24` (or `N * 25`). `Configure()` refuses a value below one layer's
dispatches. Its per-sequence requests are named `Request*`:
`RequestBeginGeneration`, `RequestResetSequence`, `RequestSaveSequence`,
`RequestRestoreSequence`, `RequestAdoptPrefix` (refused `UnsupportedOnGpu`: shared prefixes are
CPU-only in 1.0) and `RequestAdapterSwap`.

**How a GPU request is refused.** A GPU `Request*` call that is refused when it is made (a full
queue, a handle that is not a live sequence, a malformed generation request) returns an invalid
handle (`IsValid()` is false), not a handle with a named result as on the CPU. The reason is in
`GetLastLifecycleRequestError()`, which holds only the most recent refusal, so read it at once.
Do not poll `GetLifecycleOpResult` with an invalid handle: it reads `Pending` forever. A request
that is accepted and refused later, when its turn comes, resolves its handle to a named result.

The GPU subsystem also has calls the CPU one does not:

| What | Call |
|---|---|
| Slicing and latency, changed while no sequence is vended (per sequence: while it is not generating) | `SetLayersPerTick`, `GetLayersPerTick`, `SetLayersPerSlice`, `GetLayersPerSlice`, `SetFixedTickLatency`, `GetConfiguredK`, `GetConfiguredMinimumK` |
| Restore and save results | `GetRestoreResult` (the restored sequence), `GetSaveTokenCounts` |
| Refusals and faults | `GetLastLifecycleRequestError`, `GetLastFaultReason` |
| Adapters and the device | `MapAdapter`, `UnmapAdapter`, `ProbeContextUsable`, `IsGpuBackendActive`, `IsDeviceHeadActive` |
| Diagnostics | `GetLastGpuBusyMs`, `GetLastHostFinishMs`, `GetHitchCount`, `GetGpuDispatchCount` and the other `Get*` readers in the header |

The headers in `Plugins/SuperSLMUnreal/Source/SuperSLMUnreal/Public/` are the reference for exact
signatures.

**A shared prefix** is a context (for example a common system prompt) prefilled once and then
copied into any number of sequences, instead of being prefilled again for each one. Pool space for
prefixes is reserved with `PrefixBlockCount`.

## The Blueprint surface

The Blueprint surface wraps the C++ API above. Every node runs on the game thread and does no
inference there. Three parts:

**Loading a model: the Load SuperSLM Model node** (category *SuperSLM | Load*, latent). It
configures the CPU backend and then, if asked, the GPU backend, off the game thread, and fires
**Loaded** or **Failed** with a result (`bCpuConfigured`, `bGpuBackendActive`, `Message`). Inputs:
the model, `bConfigureGpu` (default true), `PoolSize` (4), `CpuTickBudgetMs` (16.6),
`GpuTickBudgetMs` (2.0, the per-slice figure the GPU hitch count compares against),
`SequenceLifecycleBudgetMs` (16.0) and `PrefixBlockCount` (0). A GPU refusal leaves the CPU
backend loaded and says why in the result. It refuses by name to reconfigure a backend that still
holds a vended sequence, a shared prefix or a mapped adapter. On the GPU it sets the tick's budget
to the model's whole depth.

**One query on either backend: `USuperSLMQuery`** (category *SuperSLM | Query*).

| Function | What |
|---|---|
| `CreateQuery` | For a model the CPU backend is configured with. Fails with a message otherwise. |
| `BeginQuery` | A prompt, `MaxNewTokens`, and an `FSuperSLMQueryConfig`: `Backend` (CPU or GPU), `FrameBudgetLayers` (layers per slice; 0 or less is a whole token), `ConcurrentQueries`, `RequestedK`, `bSchemaConstrainedDecoding`, `StopTokenIds`. |
| `TickQuery` | Once per frame. Returns true once the query has stopped, with `bSucceeded` and an `FSuperSLMQueryReadout`: the text, the token digest, frames to answer, the cost figures, the backend it ran on, and whether it moved to the CPU after a lost GPU device. |
| `CancelQuery`, `IsQueryRunning` | Stop a query; ask whether one is running. |

On the CPU backend, no setting in `FSuperSLMQueryConfig` changes the tokens except two, by design:
`bSchemaConstrainedDecoding`, which binds the model's `prompt_result` schema, and `StopTokenIds`,
which decides where generation stops. `Backend` is outside that claim: 1.0 does not claim that the GPU backend's
tokens match the CPU's (GPU determinism is per device; see [DETERMINISM.md](DETERMINISM.md)).

**The CPU backend's sequences: `USuperSLMBlueprintLibrary`** (category *SuperSLM*).

| Functions | What |
|---|---|
| `GetSuperSLMSubsystem` | The CPU backend of the world's game instance. |
| `VendSequence`, `ReturnSequence` | Take a sequence from the pool and give it back. |
| `Tokenize`, `Detokenize` | Text to token ids and back. |
| `SetSchemaByName`, `SetLayerBudget` | Bind a schema at the start of a sequence; set its layers per job. |
| `RequestAdapterSwap`, `GetActiveAdapter` | Re-point a sequence to an adapter. |
| `BeginGeneration` | Start a generation from an `FSuperSLMGenerationRequestBP` (prompt tokens, `MaxNewTokens`, `StopTokenIds`). Returns false, queueing nothing, when the sequence will not be Idle when the request's turn comes (one generation at a time: call `ResetSequence` first), when its queue is full, or when the request is malformed; the error output names the reason. |
| `Tick` | Drive the scheduler once per frame. |
| `GetPhase`, `GetGeneratedTokens`, `GetLastDecodeOutcome`, `GetStats` | Read progress and output. |
| `ResetSequence`, `SaveSequence`, `RestoreSequence` | Queued lifecycle operations, each returning a handle. |
| `GetLifecycleOpResult`, `GetSaveResult`, `ReleaseLifecycleOpHandle` | Read a queued operation's result (and a save's blob), then release its handle. The subsystem keeps each result until its handle is released. |
| `CreatePrefix`, `GetPrefixPhase`, `IsPrefixReady`, `AdoptPrefix`, `ReleasePrefix` | Shared prefixes (category *SuperSLM | Prefix*). |
| `GetLastTickDurationMs`, `GetHitchCount`, `GetPoolFreeCount`, `GetPoolOccupiedCount`, `GetKvPoolReservedBytes`, `GetWorkspaceReservedBytes` | Diagnostics (category *SuperSLM | Diagnostics*). |

**The self-check: the Run Determinism Self-Check node** (category *SuperSLM | Diagnostics*,
latent). It runs the self-check on a model configured on both backends, one bounded step per frame
(`StepBudgetMs`, default 4), and fires **Completed** with the verdict per backend and its scope. It
works in packaged builds. See [DETERMINISM.md](DETERMINISM.md).

Rules the surface keeps:

- **Decode status is an enum, never a sentinel number:** *Token Produced*, *Generating*,
  *Schema Dead End*, *Sequence No Longer Valid*.
- **Restore, Save, Reset and Adopt Prefix share one result type** (`ESuperSLMRestoreResultBP`), so
  every caller can tell a full queue from every other named failure. A queued call returns
  *Success* when it is queued, and its handle's read reports *Pending* until it finishes. Its last
  two values, *Reset Required* and *Out Of Memory*, only mirror results of the C++ GPU surface so
  the two enums keep the same numbering; no Blueprint node returns them (see the C++ results
  below).
- **The decoding mode is not exposed**; the plugin uses greedy decoding.
- **No Blueprint text states GPU determinism.** In 1.0 the GPU verdict is always withheld: it
  reads *Not Yet Run*, and its scope text says it is withheld (see
  [DETERMINISM.md](DETERMINISM.md)).

## Results and refusals

- `Configure` refuses a configuration it cannot honour and names why (an invalid pool size or call
  shape, a sequence lifecycle that would exceed its declared budget, a model that fails to map, an
  allocation failure, or cost settings outside the range the scheduler can plan with).
- Requests against a sequence queue behind each other; a busy sequence is not a reason to refuse,
  with one exception: a sequence runs one generation at a time, so a generation request is refused
  at the call, by name, unless the sequence is Idle or a reset is queued after its last generation
  (below). A request is refused at request time only when it cannot be honoured, for example: the
  sequence's own queue is full (`SequenceQueueFull`; raise `MaxQueuedOperationsPerSequence` if a
  project legitimately needs a deeper queue), the handle is not a live sequence, a generation request is malformed
  (`MaxNewTokens` below 1, a prompt token outside the vocabulary), or, on the CPU, the tick counter
  has reached its horizon (about 2^31 ticks) and `Configure()` has not run since. A CPU restore,
  which gets a fresh sequence rather than queueing against one, is also checked when it is
  requested, and can be refused at once with `Malformed`, `BackendMismatch`, `Layer1Mismatch`,
  `NotConfigured`, `ModelMismatch` or `PoolExhausted` (every warm sequence is vended).
- **Release every lifecycle handle.** Each queued operation's result is kept, by handle, until
  `ReleaseLifecycleOpHandle` releases it, and a save's blob is kept until the first successful
  `GetSaveResult`. Nothing evicts them: a game that never releases its handles keeps one small
  entry per request (and one blob per unread save) for as long as the backend stays configured.
- Restore refuses, by name, a blob from a different model, from the other backend
  (`BackendMismatch`), or from a build pinned to a different SuperSLM tag (`Layer1Mismatch`). A GPU
  restore that runs out of GPU memory reads `OutOfMemory`, and may be retried.
- A fault's reason is logged, not returned: a `LogSuperSLM` warning `Sequence N faulted:
  <reason>` (`GPU sequence N faulted: <reason>` on the GPU). `GetLastDecodeOutcome` on a faulted
  sequence names only `SchemaDeadEnd` or `SequenceNoLongerValid`, and reads `Generating` for
  every other fault; on the GPU, `GetLastFaultReason` says whether the fault is recoverable
  (`RecoverablePerSequenceRejection`) or the device was lost (`TerminalDeviceLost`).
- **A sequence runs one generation at a time, on both backends.** A generation request is refused
  at the call, by name, unless the sequence will be Idle when its turn comes: Idle with nothing
  queued, or a reset queued after its last generation. The refusal changes nothing on the
  sequence: on the CPU `BeginGeneration` returns false with the reason in its error; on the GPU
  `RequestBeginGeneration` returns an invalid handle and `GetLastLifecycleRequestError` names the
  reason. A generation that continues a completed one is not supported, so each generation after
  a sequence's first needs a reset first (`ResetSequence` on the CPU, `RequestResetSequence` on
  the GPU).
- A faulted sequence is refused the same way, at the call, on both backends. A generation that
  ended in *Schema Dead End* is faulted too (SuperSLM itself lets a dead end be retried; requiring
  a reset after every fault is the plugin's own rule, since SuperSLM requires one only after an
  allocation failure), so a sequence reused after its schema completed is refused as well. Reset
  it, or return it and restore a save, which gives a new sequence; a restore never clears the
  faulted one. A reset SuperSLM refuses leaves the sequence faulted, with its previous
  generation's tokens still readable, on both backends; a reset that succeeds leaves it Idle with
  no tokens. `ResetRequired` is what a GPU generation reads at its turn only when it was queued
  behind such a refused reset; on the CPU that generation is dropped at its turn without starting,
  with a `LogSuperSLM` warning, as it is behind a prefix adoption whose prefix faulted.
- A reset requested while a generation runs interrupts it, so "reset, then generate" in one frame
  starts the next generation without waiting for the running one to finish. Until the reset takes
  effect the sequence reads its old phase and tokens (*Complete* included), so before reading the
  new generation wait for its handle to read `Success` (GPU). On the CPU there is no handle: the
  new generation has ended when the phase reads *Complete* or *Faulted* and
  `GetPendingLifecycleOperationCount` reads 0 (a phase read between ticks can miss *Prefilling*
  and *Decoding* altogether; a refused reset leaves it *Faulted*). A call that needs a
  sequence that is not generating (on the GPU, `SetLayersPerSlice`) is refused until a reset
  requested mid-run has taken effect: to reset, set the slice and begin, drive the reset's handle
  to resolution before setting the slice.
- A schema dead end (no token the schema allows) ends the generation with its own outcome rather
  than producing text outside the schema.

## Threading

- Call the subsystems from the game thread. Neither runs inference on it, and the per-frame calls
  (`Tick`, the queued requests, the reads) never wait on a worker or on the GPU.
- CPU: `Tick` delivers finished results and hands the next job to a worker; the work runs on
  plugin-owned worker threads, named in Unreal Insights.
- GPU: all GPU submission runs on one dedicated thread, and a token requested at tick T is applied
  at tick T + K. See [ARCHITECTURE.md](ARCHITECTURE.md#the-gpu-backend).

**Blocking calls.** These block the thread that calls them; keep them off the frame path (at load
time, or behind a loading screen):

- `Configure()`, on either subsystem, maps the model on the calling thread and, on the GPU, waits
  for the device setup on the submission thread. `BeginConfigure()` does that work off the
  game thread and reports back a later frame; the **Load SuperSLM Model** node uses it.
- `BeginConfigure()` and `Configure()`, on a backend that is already configured, first tear the
  previous configuration down on the calling thread, and the teardown waits. The CPU backend joins
  each worker thread after it has run every job already queued on it, so the wait is the backlog
  of queued work, not one job. The GPU backend queues its teardown behind every job already on its
  submission thread, waits for all of them to run and then joins the thread, so its wait is the
  queued backlog too. If that thread is already unresponsive, the GPU backend leaks it instead,
  with no wait and no join; if it stops responding during the wait, the wait lasts until the stuck
  call overruns its time bound, and then the thread is leaked. The first configuration has nothing
  to tear down and does not wait. Reconfigure where a short stall is acceptable.
- GPU `MapAdapter()` and `UnmapAdapter()` wait on the GPU submission queue.
- GPU `ProbeContextUsable()` waits on the GPU submission queue. It is also a real GPU submission,
  and a false result tears the GPU backend down, so do not call it speculatively.
- The `SuperSLM.CalibrateCosts` console command runs its whole measurement on the game thread,
  prefills included, and the editor or game does not respond until it finishes. See
  [TOOLING.md](TOOLING.md#commands-and-c-tools).
