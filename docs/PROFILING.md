# Profiling and Instrumentation

> **Status:** built on both backends. Names below are read from the source.

---

## Contents

- [Trace channels](#trace-channels)
- [Timing scopes and threads](#timing-scopes-and-threads)
- [Counters](#counters)
- [Stats and CSV](#stats-and-csv)
- [Bookmarks](#bookmarks)
- [Instrumentation does not change results](#instrumentation-does-not-change-results)

---

## Trace channels

Two Unreal Insights trace channels, one per backend:

| Channel | Backend |
|---|---|
| `SuperSLM` | CPU: prefill, decode, the token finish, lifecycle operations, pool events |
| `SuperSLMGpu` | GPU: slices, the token finish, the self-check, lifecycle operations |

Enable them with the rest of your trace, for example `-trace=cpu,SuperSLM,SuperSLMGpu`. A
disabled channel costs a single branch per scope.

## Timing scopes and threads

**CPU backend.** The game thread shows `SuperSLM.Tick`, with `SuperSLM.Tick.Plan` and
`SuperSLM.Tick.Apply` inside it; that is the only per-frame time the plugin spends on the game
thread (the blocking calls outside the frame are listed in
[BLUEPRINT_API.md](BLUEPRINT_API.md#threading)). Each
worker is a named thread (`SuperSLM CPU Worker N`) with its own track in the Timing view. On the
worker, `SuperSLM.WorkerJob` wraps each job, and inside it scopes name the prefill (per prompt
token), the decode layers, the token finish, and each lifecycle and pool operation. When
`FinishParallelTasks` is above 1, `SuperSLM.FinishTask` scopes show where the finish's pieces ran on
the engine's task workers.

**GPU backend.** The game thread shows `SuperSLM.Gpu.Tick`, with `SuperSLM.Gpu.Tick.Plan` and
`SuperSLM.Gpu.Tick.Apply`. The submission thread shows `SuperSLM.Gpu.SubmissionJob`, and inside it
`SuperSLM.Gpu.Embed`, `SuperSLM.Gpu.Submit`, `SuperSLM.Gpu.Drain`, `SuperSLM.Gpu.Readback`,
`SuperSLM.Gpu.Finish`, `SuperSLM.Gpu.Save`, `SuperSLM.Gpu.Restore` and `SuperSLM.Gpu.SelfCheck`.
The GPU time of each slice is SuperSLM's own timestamp measurement on its own queue, reported as a
counter.

## Counters

Insights counters, set once per job, slice or tick (never per layer):

- **CPU** (`SuperSLM/CPU/...`): `HitchCount`, `TokensFinishedTotal`, `LayersPerJob`, `K`, `DueJobs`,
  `DeliveredJobs`, `HostFinishMs`, `PrefillMsPerToken`, `TokensPerSecond`, `PoolOccupied`,
  `PoolFree`, `RetainedResultBytes`.
- **GPU** (`SuperSLM/GPU/...`): `GpuBusyMsPerSlice`, `HostFinishMs`, `LayersPerSlice`, `HitchCount`,
  `DeliveredTokens`, `K`, `DueTokens`, `RetainedResultBytes`.

`HostFinishMs` on the GPU is the wall time of the finish call, which includes the device-resident
head's dispatch when the head is on. `GpuBusyMsPerSlice` does not include that dispatch.

## Stats and CSV

- The stat group `SuperSLM` (`stat SuperSLM` in the console; the group's display name is
  *SuperSLMUnreal*) shows pool occupancy, the hitch count and timing figures for the CPU backend.
- CSV profiler categories `SuperSLM` and `SuperSLMGpu` record per-frame figures for regression
  runs.

## Bookmarks

Insights bookmarks mark the events you would otherwise hunt for: a CPU job delivered late (and by
how many ticks), a GPU slice over budget, a GPU token applied late, an adapter swap, a pool
refusal, and a confirmed GPU device loss.

## Instrumentation does not change results

Every scope, counter and bookmark reads state and writes it out; none of it feeds a decision the
inference makes, so profiling cannot change what the model generates.

**What the tests cover.** On the CPU backend, the token-identity test runs once with the
`SuperSLM` channel on (`SuperSLM.L2S1.CpuDeterminism.TraceOn`) and must produce the same tokens as
the reference. It is in the suite that gates 1.0. On the GPU backend, the tests capture a trace with
the `SuperSLMGpu` channel on and check its scopes and counters, and check that nothing is emitted
with it off; no GPU test compares tokens with tracing on against tracing off.
