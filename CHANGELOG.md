# Changelog

All notable changes to SuperSLM-Unreal are recorded here. Versions follow the `VersionName` in
`Plugins/SuperSLMUnreal/SuperSLMUnreal.uplugin`. The plugin versions independently of SuperSLM; each entry
names the SuperSLM release it vendors.

## 1.3.0 - 2026-10-24

**Vendored SuperSLM:** `v1.15.0` (`165962acafe901753dfe37153d7116b132a18b5a`), up from `v1.13.1`.

**Saves:** a save made by 1.2.0 is refused by 1.3.0 with `Layer1Mismatch`, and the reverse, because the vendored
SuperSLM release changed. A cached prefix (below) written by another SuperSLM release is not refused: it counts as a
miss and is rebuilt by prefill.

### Added

- **The prefix cache** (CPU backend). Ask for a prompt prefix by its tokens and schema with
  `RequestPrefix`, and adopt the lease like a shared prefix. The cache serves it from memory, then from a cooked
  prefix set, then from disk, and builds it by prefill only when none of those has it. The tokens are the same
  whichever tier served it. The cache has its own memory budget, evicts the least recently used prefix that nothing is using, can
  hold a nested prefix on its parent's pages, and has explicit `WarmPrefix` and `DropPrefix` calls for level loads.
  It is off until `FSuperSLMRuntimeConfig::PrefixCacheBudgetBytes` is set, and the disk tier is off until
  `bPrefixDiskCacheEnabled` is set. See [ARCHITECTURE.md](docs/ARCHITECTURE.md#the-prefix-cache).
  Measured in a packaged Development build (AMD Ryzen 9 3950X, the 0.5B example model, at SuperSLM 1.14.0's
  engine, whose prefill and prefix restore 1.15.0 does not change):
  - A 128-token prefix holds 768 KiB.
  - Restoring it from a cooked set took 0.92 to 1.01 ms of worker time (10 restores).
  - Building it by prefill takes about 15.5 ms of worker time per token measured alone, about 2.0 s for
    128 tokens. Inside a running game, in a sliced 2,048-token build, it took a median of 30.1 ms per token.
  - While 10 cooked prefixes loaded, the longest game-thread tick was 0.24 ms (3,828 ticks).
- **Prefix sets**, an asset (`USuperSLMPrefixSet`) holding prompt prefixes built in the editor and cooked into the
  game. Its entries are defined by token ids or by text. The editor's **Build Prefixes** action builds them in the
  background, and the `SuperSLMBuildPrefixes` commandlet builds them for a build machine. The cook warns, by entry,
  about an entry the game would not use: one built for another model or another SuperSLM build, or edited since
  its build. At run time such an entry is built by prefill. A text entry whose text or model changed ships empty.
  Register a set with `RegisterPrefixSet`. **Rebuild your prefix sets after every plugin upgrade that changes the
  vendored SuperSLM**, or their entries are built by prefill at run time.
- **Skip-ahead for schema-bound generations** (CPU, C++): `FSuperSLMGenerationRequest::SchemaJumpForward`, per
  generation, `Off` by default. Where a bound schema allows only one text (a key name, a quote, a brace),
  skip-ahead writes several tokens in one batched pass instead of one per decode. It has two modes:
  - **`Exact`** gives the same tokens as `Off`, and the same save. The plugin guesses the next tokens from the
    schema's earlier finished generations in this configuration, and SuperSLM keeps only the guesses the model
    would have written itself.
  - **`Canonical`** writes the schema's own spelling of the forced text, so it writes fewer tokens and different
    ones. Check its output for your schema before using it.

  SuperSLM measured this on the same CPU and model, with the example's potion-shop order (41 prompt tokens and
  28 generated). The times are generation only, at one thread:
  - `Off` took 897 ms. It already includes 1.15.0's faster schema-bound finish; 1.13.1 took 1,257 ms.
  - `Exact`, guessing from earlier orders, took 604 ms against that 897 ms. Over 20 other orders it took 12,878 ms
    against 16,676 ms off, 1.3 times faster in total, and one order was slower.
  - `Canonical` took 471 ms for 21 tokens.
  - With threaded decode on (`bParallelDecode`, 4 tasks), `Exact` was about even with `Off`.

  These are SuperSLM's timings, not timings inside a running game. See
  [ARCHITECTURE.md](docs/ARCHITECTURE.md#skip-ahead-for-schema-bound-generations).
- **`SuperSLM.EstimateCosts` measures more costs:**
  - the price of prompt chunks of 2 to 7 tokens, which prices skip-ahead's batched passes;
  - a cache restore's cost per MiB;
  - the cache build's prefill price.

  `USuperSLMSubsystem::WriteInSituPrefixPrefillPrice` refits the build's price from a build run inside the game.
- **`-SuperSLMExampleLayers=<N>`** starts the example's GPU Frame Budget at N layers per slice
  (0 is a whole token), so a packaged build can be checked at several settings without a rebuild.
  **`-SuperSLMExampleJumpForward=off|exact|canonical`** sets the example's skip-ahead mode.
- **Frame times in the example's readout and packaged-check report:** each query's per-frame
  engine delta from Ask to the answer, its p50, p95, p99 and max, the frames that missed the cap
  (over its period plus 0.5 ms) and the frames over 20 ms.

### Changed

- **SuperSLM 1.15.0** (from 1.13.1, through 1.14.0). Without skip-ahead, the tokens are the same as 1.13.1's.
  What it brings to the plugin:
  - **A faster finish for schema-bound CPU generations.** When a schema is bound, each token's last step scores only
    the tokens the schema allows, with the same tokens. On the example order it took 921 ms against 1,257 ms at one
    thread, and 560 ms against 711 ms at 4 tasks (SuperSLM's timings, 2026-10-20; the 897 ms `Off` figure above is
    the same configuration timed in a later session, 2026-10-21, beside the skip-ahead runs). The plugin's `FinishCostMs` default is not
    re-measured for it, so the scheduler over-prices a schema-bound finish, which only lowers throughput.
    `SuperSLM.EstimateCosts` measures a project's own costs.
  - **Prefix save and restore** (CPU), which the prefix cache's cooked and disk tiers use.
  - **A model is refused at load** when two of its schema names share a 64-bit hash, or one hashes to zero. The
    certified models load.
- **The example project runs a whole token per frame on the GPU by default**, the plugin's own
  default (`FrameBudgetLayers = 0`, the one-call path), instead of 4 layers per slice. The README
  quick-start shows the same. In the 1.2 packaged runs (RTX 2080 SUPER, 60 fps cap) that setting
  answered the example query in 2,834 ms, 171 frames, against 15,766 to 15,816 ms at 4 layers.
  Heavier scenes can set 4 to 12 layers per slice. The example's test now holds the median GPU
  slice to one 60 fps frame rather than 2 ms, asserts the one-call path, and also runs a 4-layer
  composed query that must give the same tokens. The example's GPU hitch count compares slices
  with one 60 fps frame.

### Fixed

- **The determinism self-check's GPU arm finishes under a whole-token configuration.** Its
  one-layer-per-slice run sized its tick ceiling from the decode tokens alone, but the composed
  path also slices each prompt token one layer at a time. At K = 1 (a whole token per tick, as the
  Load node and now the example configure it) the run ran out of ticks and reported "the GPU
  self-check sequence did not finish". The ceiling now counts the prompt tokens.

- **`--save` writes only the setting its command chose.** `SuperSLM.SetDecodeMode … --save` saves
  `bParallelDecode` and `SuperSLM.SetFinishParallelTasks … --save` saves `FinishParallelTasks`; the
  other key is kept when the file already holds it and is otherwise left out, so it takes the
  shipped value. 1.2.0 wrote both keys, the unchosen one at its value at the time. Threaded decode
  stays off by default, as in 1.2.0.

### Known issues

- **Blueprint cannot turn the prefix cache on.** The cache's budget is a C++ configuration field, and the
  **Load SuperSLM Model** node has no input for it. The Blueprint cache nodes work once C++ has configured it.
- **Skip-ahead is CPU-only and C++-only.** The GPU backend ignores the setting, and no Blueprint request carries it.
- **`Exact` learns nothing from the first generation after `Configure()`.** That generation runs before the schema's
  walk table has loaded in the background. What it learns starts empty at every configuration and is not saved.
- **The GPU backend has no prefix cache yet.** `USuperSLMGpuSubsystem` declares the cache calls, but in 1.3.0
  they are not connected to its scheduler, so they refuse `NotConfigured`. SuperSLM 1.15.0 also does not release
  GPU prefix save and restore, which the GPU cache's cooked and disk tiers would need.
- **Nested entries in a prefix set are not built.** At run time a request for one is built by prefill.
- **A cache build's chunks overrun their planned time until the build's price is measured in place.** With the
  shipped default price a sliced CPU cache build's median chunk took 1.57 times its planned ticks, and 1.48 times with
  `SuperSLM.EstimateCosts`' price, which is measured outside a running game. `USuperSLMSubsystem::WriteInSituPrefixPrefillPrice`
  fits the price from a build inside the game; with it the median chunk took 0.78 to 0.92 of its planned ticks over two
  runs (AMD Ryzen 9 3950X). An overrun is a late job on the worker, counted as a hitch; the game thread is not held.
- **One prefix-cache check is graded only in a build with asserts compiled out**, which is the Test configuration.
  It checks that the drop's pin check and its disk-delete ordering are real branches, not asserts, so they hold in
  a shipped game. The editor's Development build runs the same two legs with asserts on and grades only those.
  1.3.0 has not been run in a Test-configuration build.
- **Prefix sets registered for one model are unregistered when another model is configured.** Register them again
  after configuring their model.
- **With threaded decode turned on, the mode-1 `LayerCostMs` (0.906) reads optimistic in a running
  game.** In the example query's in-game timing (packaged Development build, AMD Ryzen 9 3950X,
  60 fps cap, three threading-off and three threading-on runs interleaved), the worker's decode jobs
  had a median of about 33.5 ms with threaded decode on, against about 42 to 44 ms off, and the jobs
  delivered later than planned rose from 34 to about 66 per answer, while the frame rate held at 60.
  A late job is counted as a hitch and applied on the first later frame at which it exists; nothing
  is reordered. The value is to be re-measured at the next calibration on that machine, and keeps
  0.906 until then. `SuperSLM.EstimateCosts` measures a project's own costs.

## 1.2.0 - 2026-10-19

**Vendored SuperSLM:** `v1.13.1` (`d697927f6d5ce5fa73ecbb8660020a425518b923`). This release also carries the 1.11.0 integration
that was not released on its own, so it is the first release after 1.0.0.

**Saves:** a save made by 1.0.x is refused by 1.2.0 with `Layer1Mismatch`, and the reverse, because
the vendored SuperSLM release changed. See [PAGING.md](docs/PAGING.md#8-saves-across-releases).

**Known issue:** on an AMD Radeon RX 7900 XTX with driver 32.0.23033.1002, the packaged example's
rendering hangs the GPU even with no model loaded. The GPU backend's tests pass on that card
without a renderer; running it beside Unreal's renderer there is not certified.

### Changed

- **SuperSLM 1.11.0.** Tokens are identical to 1.0's. What it brings to the plugin:
  - SuperSLM 1.10.0's CPU speed-ups on AVX2 and AVX-512 CPUs: a tiled kernel for prefill calls of 8
    or more tokens (see `MinPrefillChunk` below), faster attention and per-row steps in prefill and
    decode, and a hardware SHA-256 for the model's load-time integrity hash on CPUs with the x86
    SHA extensions. On AVX-512 CPUs the tiled kernel and the attention steps keep 1.0's code in this
    build, because the plugin is compiled with MSVC or clang-cl, where SuperSLM leaves them off.
  - SuperSLM 1.11.0's threaded decode is off by default; `bParallelDecode` (below) turns it on for
    the CPU backend.
  - A prefill call that fails before admitting a token now leaves the sequence's saturation counts
    as they were before the call.
  - A save made by 1.0 is refused on restore, by name, because it records a different SuperSLM tag.

- **Prefill in chunks of 8 or more tokens** (CPU backend). A prefill job now carries at least
  `MinPrefillChunk` prompt tokens (new `FSuperSLMRuntimeConfig` field, default 8) while the prompt
  has that many left, so each prefill call reaches SuperSLM's tiled prefill kernel. A shorter tail
  goes as one shorter job. Set `MinPrefillChunk` to 1 to restore 1.0's one-token prefill jobs.
  Tokens are identical at every setting, and prefill stays on the worker thread.
- **A batched prompt price.** Chunks of 8 or more tokens are priced with two new fields,
  `BatchedPromptTokenCostMs` and `BatchedPromptTokenCostPerPositionMs`. With decode threading off
  they default to 15.023 and 0.004791, measured on a Zen 2 desktop, against 33.583 and 0.004547 for
  one token (with it on, 15.682 and 0.004437 against 22.108 and 0.005209). `SuperSLM.EstimateCosts`
  measures them with a second prompt arm timed at 8 tokens per call.
- **The cost defaults are two per-mode profiles.** Unset cost fields take the one-time measured
  profile of the effective decode mode (off, or on with more than one finish task), from six
  per-call capture blocks on one Zen 2 desktop with the 0.5B model. They are reasonable measured
  starting costs, not certified ones. `LayerCostMs` is 1.397 with decode threading off and 0.906
  with it on; `docs/ARCHITECTURE.md` lists all thirteen.
- **`Configure()` reads a matching saved estimate.** When `Saved/SuperSLM/CalibratedCosts.ini`
  holds a `SuperSLM.EstimateCosts` profile for the configured artifact, effective decode mode and
  task count, its values price the cost fields the caller left unset. A profile with a missing,
  non-finite or out-of-range value, or one that would put a job past the scheduler's tick limit
  where the mode profile would not, is skipped with one warning.
- **Changed, source-incompatible: `FSuperSLMRuntimeConfig`'s decode mode, task count and thirteen
  cost fields are `TOptional`.** An assignment sets a field whatever its value, so a caller's
  `false` or `4` overrides a saved setting. Each field resolves on its own: the caller's value,
  else the running editor's `SetDecodeMode`/`SetFinishParallelTasks` choice, else the saved file, else
  the shipped value or profile. Code that read these fields reads `FSuperSLMConfigureReport::Resolved`
  or `USuperSLMSubsystem::GetResolvedConfig()` instead. `Configure()` logs
  `SuperSLM: CPU Configure: bParallelDecode=<0|1> FinishParallelTasks=<n> cost profile=<caller|saved-estimate|mode-default> caller_cost_fields=<0..13>`.
- **An overrun is now a job that took longer than its own planned ticks.** A job planned for K
  ticks is not an overrun at tick 2. At K = 1 the rule is unchanged.
- **Configure** refuses the two batched fields by name, as it refuses the one-token pair, and
  refuses `MinPrefillChunk` below 1. It logs a warning when `MaxPrefillChunkBudget` or
  `MinPrefillChunk` is below 8: prefill will not reach the tiled kernel.

### Added

- **Opt-in paged KV on both backends.** Positive `SequenceBudget` selects a shared page pool,
  explicit shared/nested prefixes, per-sequence budget stops and restore headroom. Zero keeps
  the existing mode. [PAGING.md](docs/PAGING.md) documents the API and compatibility rules.
  On the GPU backend the whole page pool is one D3D12 buffer, so its size is capped at D3D12's
  required single-resource size for the card: a quarter of its dedicated video memory, at most
  2 GiB and at least 128 MiB. The largest GPU pool is therefore 2 GiB, or a quarter of video memory
  on a card with less than 8 GiB. A larger pool is refused at `Configure()` with `KvPoolRefused`,
  naming `SSLM_GPU_KV_POOL_TOO_LARGE` and the page count. The 50-NPC example's pool is 178.7 MiB.
- **Measured CPU budget lifecycle defaults.** Adopt, full-cap restore and prefix Begin use
  separate cap/mode profiles measured on the real 0.5B and 1.5B artifacts. All thirteen fields
  retain caller-over-saved-over-default priority; legacy defaults are unchanged. Native
  selection and release validation are still pending.

- **`bParallelDecode`** on `FSuperSLMRuntimeConfig`, off by default: splits CPU one-row decode
  projections through the finish hook. It takes effect only above one finish task, and the Insights
  scope for the hook's tasks is `SuperSLM.ParallelTask`. Tokens are identical either way.
- **`SuperSLM.SetDecodeMode 0|1 [--save]`** and **`SuperSLM.SetFinishParallelTasks <0..256> [--save]`**
  console commands: they choose the CPU decode mode and finish task count for the next
  `Configure()` that leaves them unset. `--save` keeps the choice in
  `Saved/SuperSLM/RuntimeSettings.ini`, which later sessions, packaged games included, load. A
  configuration already made keeps its settings.
- **`SuperSLM.EstimateCosts "<ArtifactPath.sslm>" [--write-ini]`**, a console command that measures
  both CPU decode modes on the machine it runs on (about 4-5 minutes for the 0.5B model) and prints
  a `CALIBRATION ESTIMATE — not verified`: per-arm medians and spread, thirteen suggested costs per
  mode rounded up, and the foreign CPU load it ran under. Desktop load is reported, never a reason to
  stop. Only `--write-ini` writes `Saved/SuperSLM/CalibratedCosts.ini`.

### Removed

- The `SuperSLM.CalibrateCosts` console command, replaced by `SuperSLM.EstimateCosts`. A script
  that calls the old name gets Unreal's unknown-command message; it is not an alias, because the old
  command wrote its file without a flag.
- The public header `SuperSLMCalibrateCostsCommand.h` (`EOutcome`, `GetLastOutcome`).
- The idleness types and accessors in `SuperSLMCalibrationTestAccess.h`. The header keeps the fit
  seams `BatchedArmDepthsForCap`, `FitBatchedArmFromSyntheticTimings` and `BatchedArmSkipValues`,
  which now takes the decode mode.

### Cost: waiting behind a prefill job

With one worker lane (the default), a longer prefill job holds the lane longer. With no worker
overrun, another sequence's reset, save, restore, prefix adoption or first prompt job can wait about
250 ms longer at the start of a context and about 400 ms longer at depth 1000, at the default
prices and any frame-rate budget; with worker continuation on, the added wait is about 505 ms and
799 ms. The calibrated batched price shortens it. While the worker runs late, a request can wait
behind up to one job per sequence slot and one per shared-prefix slot on its lane
(`BlockCount + PrefixBlockCount`; 4 with the default settings), for those jobs' real run time.
`MaxConcurrentCalls` 2 (a second lane) or `MinPrefillChunk` 1 shortens the wait.

## 1.0.0 - 2026-09-28

First release. Unreal Engine 5.8. **Windows x64**, CPU and GPU (D3D12) backends. Linux x64 and
macOS (Apple Silicon) share the same sources but have not been built or tested as an Unreal plugin.

**Vendored SuperSLM:** `v1.9.0`, commit `d870d27863c73afb0d558b376f5e8ad84f83951c`.

### Added

- `USuperSLMModel`, a `.sslm` model asset. Import runs SuperSLM's full validation and reports a
  specific reason on failure; a file needing a newer SuperSLM is reported as such, not as corrupt.
  On Windows the cooked asset is memory-mapped at load.
- `USuperSLMSubsystem`, the CPU backend: engine-owned KV pool and workspaces sized at
  `Configure()`, inference on plugin-owned worker threads (never the game thread), job sizing from
  configurable per-unit costs, results delivered as soon as they finish, and late results counted
  as hitches.
- `USuperSLMGpuSubsystem`, the GPU backend: fixed-tick delivery (a token requested at tick T is
  applied at tick T + K, where K is `FSuperSLMGpuRuntimeConfig::K`, set at configuration and changed
  only through a call: `SetFixedTickLatency`, `SetLayersPerTick` when it raises the minimum, or a
  GPU `USuperSLMQuery`, which restores the configured value after it), one submission thread,
  each token sliced into a caller-set number of whole layers per tick (at 4 layers on an
  RTX 2080 SUPER with the 0.5B model, a slice measured about 2 ms) or run as a whole token per
  call, sliced prompts with no ceiling below the context size, and an optional device-resident head for the token
  finish, on by default per model asset. The Load node and a default query run a whole token per
  tick.
- Queued per-sequence requests (generation, reset, save, restore, adopting a shared prefix),
  refused at request time only when the request cannot be honoured (a full queue, a dead handle, a
  malformed generation request); a CPU restore is also checked
  when it is requested (the blob, the backend, the model, the SuperSLM tag, a free sequence). Each
  request's result is kept until its handle is released with `ReleaseLifecycleOpHandle`.
- Shared prefixes, on the CPU backend only: prefill a common context once and adopt it into many
  sequences. On the GPU, adopting a prefix is refused by name (`UnsupportedOnGpu`).
  The Load node's `PrefixBlockCount` pin (pool blocks reserved for shared prefixes) defaults to
  0, matching `FSuperSLMRuntimeConfig`. A graph that uses shared prefixes must set it above 0, or
  `CreatePrefix` is refused for lack of capacity.
- Adapter re-pointing on a resident base model; merged standalone specialists also load.
- Save and restore, tagged with the backend and the SuperSLM tag and commit. A blob from the other
  backend, or from a different SuperSLM pin, is refused by name.
- The determinism self-check: a fixed workload per backend at two slicing settings, compared with
  the CPU on the same machine and with a shipped reference for the example model. Verdicts carry
  their scope. It runs from the editor's SuperSLM Query Window, from the Blueprint node
  **Run Determinism Self-Check** (also in packaged builds), and from C++. The example scene runs it
  on load and offers `SuperSLMExample.SelfCheck`.
- `SuperSLM.CalibrateCosts`, a console command that measures the CPU scheduler's per-unit costs
  on your hardware, with a warm-up pass and an idle-machine check.
- Unreal Insights instrumentation on both backends: trace channels `SuperSLM` and `SuperSLMGpu`,
  named worker threads, timing scopes, counters, a stat group (`stat SuperSLM`; display name
  *SuperSLMUnreal*), CSV categories,
  and bookmarks.
- The Blueprint surface: the **Load SuperSLM Model** node (configures both backends off the game
  thread), `USuperSLMQuery` (one query on either backend, re-issued on the CPU if the GPU device is
  lost), and a function library over the CPU backend's sequences, generation, lifecycle
  operations, shared prefixes and diagnostics.
- Editor tools: the SuperSLM Query Window (queries on either backend, the live inspection, cost and
  pooled-resource panels, and the self-check), and the model's asset editor (details, model
  inspector, adapter inspector and schema dry-run). The conversion pre-check is a C++ API in the
  editor module.
- `ExampleProject` with an example scene, written in C++ with its map authored in code, and the
  example model (Qwen2.5-0.5B-Instruct, context length 4096) as a release asset.
- `SuperSLMUnrealMCP`, an optional MCP toolset plugin, **disabled by default, beta and
  experimental; not a 1.0 claim**. It needs an engine carrying the Experimental ToolsetRegistry
  and ModelContextProtocol plugins.

### Behaviour to know

- A sequence runs one generation at a time, on both backends. A generation request is refused at
  the call, by name, and changes nothing, unless the sequence is Idle or a reset is queued after
  its last generation: the CPU's `BeginGeneration` returns false with the reason, and the GPU's
  `RequestBeginGeneration` returns an invalid handle with the reason in
  `GetLastLifecycleRequestError`. The remedy is a reset first (`ResetSequence` on the CPU,
  `RequestResetSequence` on the GPU), queued in the same frame as the generation. A generation that
  continues a completed one is not supported.
- A faulted sequence refuses a new generation at the call, on both backends, until it is reset.
  SuperSLM requires a reset only after an allocation failure; requiring one after every fault is
  the plugin's own rule. A generation that ended in *Schema Dead End* is faulted too (also the
  plugin's rule: SuperSLM lets a dead end be retried), so a sequence reused after its schema
  completed needs the same reset. To restore instead, return the sequence and restore a save,
  which gives a new sequence.
- A reset requested while a generation runs interrupts it. A reset that SuperSLM refuses leaves the
  sequence faulted rather than idle, on both backends, with its previous generation's tokens still
  readable; a GPU generation queued behind it reads `ResetRequired` at its turn, and a CPU one is
  dropped at its turn with a logged warning.
- A GPU restore that runs out of GPU memory reads `OutOfMemory`, which may be retried because the
  save is intact.

### Known limitations

- The shipped CPU cost defaults were measured on one machine (AMD Ryzen 9 3950X). `LayerCostMs`
  and `LayerCostPerPositionMs` were set by hand above the measured values, because the calibration
  command's layer measurement under-prices layers run inside a job. Do not lower them on the
  strength of a calibration run alone.
- The GPU backend's determinism is a per-device question, and 1.0 does not claim it: in 1.0 the
  self-check's GPU verdict is always withheld (recorded in the report, *Not Yet Run* on every
  public surface). A later release shows it.
- The self-check gives a CPU verdict only for the example model; any other model reads *Not Yet
  Run* on the CPU, because the plugin ships a reference only for the example model.
- Shared prefixes are CPU-only.
- The self-check compares tokens, not logits.
- The GPU backend has been run in a source-built engine's editor and in a packaged build. In an
  installed (launcher) engine's editor it has not been run.
- Saves do not carry across a change of the plugin's SuperSLM pin.
- A GPU restore that fails because the GPU device was lost reads `Malformed`, not a result of its
  own.
- GPU throughput: 1.0 states no GPU tokens-per-second figure. What the GPU backend buys is CPU
  time; see "The GPU claim: frame cost, not speed" in `docs/STATUS.md`.
- Under rendering, the GPU's per-slice time is a span that includes the renderer's own work on the
  same GPU, so it bounds the slice's cost from above (`docs/STATUS.md`, *Measured*).
- The calibration command's layer measurement under-prices layers run inside a real job; this is
  not fixed in 1.0 (see the first item).
- The CPU re-issue after a lost GPU device has no automated test: a device loss cannot be produced
  on demand. It is covered by code review only.
- The tracing-on token-identity test covers the CPU backend. On the GPU, tracing is tested for its
  output, not for token identity.
