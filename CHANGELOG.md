# Changelog

All notable changes to SuperSLM-Unreal are recorded here. Versions follow the `VersionName` in
`Plugins/SuperSLMUnreal/SuperSLMUnreal.uplugin`. The plugin versions independently of SuperSLM; each entry
names the SuperSLM release it vendors.

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
