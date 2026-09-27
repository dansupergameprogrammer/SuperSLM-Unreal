# Determinism

> **Status:** the guarantee below holds for what is built. The self-check, its report, its editor
> entry and its Blueprint node are built and tested.

---

## Contents

- [What is guaranteed](#what-is-guaranteed)
- [Where it comes from](#where-it-comes-from)
- [What the plugin must not break](#what-the-plugin-must-not-break)
- [The GPU: a per-device question](#the-gpu-a-per-device-question)
- [The self-check](#the-self-check)
- [What the self-check does not cover](#what-the-self-check-does-not-cover)

---

## What is guaranteed

On the **CPU backend**, for the same model, prompt, schema and settings, the plugin produces the
**same tokens**:

- every run;
- however the work is sliced (layers per job), and however many frames it takes;
- however many other sequences run at the same time;
- whether tracing is on or off.

This is verified on Windows x64. **GPU determinism is per device, and 1.0 does not claim it** (see
[The GPU: a per-device question](#the-gpu-a-per-device-question)).

The guarantee is about **what** is generated. It is not about **which frame** a result arrives on.
The CPU backend delivers a result as soon as its worker finishes, so how many frames that takes
depends on the machine and its load.

## Where it comes from

The guarantee is SuperSLM's: it runs integer-only arithmetic whose result does not depend on how
the work is split, and it proves this with its own pinned reference hashes. See the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM) for how. This plugin
adds no arithmetic of its own on the inference path.

## What the plugin must not break

Everything the plugin adds is outside the arithmetic, and is tested not to disturb it:

- SuperSLM's sources are compiled unmodified, with precise floating-point semantics and without
  forcing any SIMD tier, as SuperSLM requires.
- Scheduling changes when work runs, never what it computes. On the maintainer's test machine (an
  AMD Ryzen 9 3950X with an NVIDIA RTX 2080 SUPER), the plugin's tests check token identity
  across slice settings, concurrency and backends against SuperSLM's own independently built
  reference. That is not a claim that the GPU's tokens match the CPU's on other devices (see
  below).
- Instrumentation only reads. See [PROFILING.md](PROFILING.md).

## The GPU: a per-device question

**The CPU backend's determinism is verified on Windows x64.** On the GPU, whether a given device
produces the same tokens as the CPU is a property of that device and its driver, so it is tested
per device rather than promised:

- In 1.0 the GPU verdict is always withheld: the self-check records it in the report, and every
  public surface shows *Not Yet Run*. A later release shows it.
- SuperSLM's own GPU certification covers two devices: the NVIDIA RTX 2080 SUPER and the AMD
  Radeon RX 7900 XTX.

The GPU backend is never refused because of the self-check, and a sequence never switches backend
on its own. The one exception is above the sequence API: a query (`USuperSLMQuery`, and the
editor's query window) whose GPU device is lost is re-issued on the CPU backend from its original
request ([ARCHITECTURE.md](ARCHITECTURE.md)). Running the reproducible work on the CPU backend is
always available.

## The self-check

The self-check runs a fixed workload on each backend and compares the tokens.

- **The workload:** the example model, a fixed prompt, greedy decoding, 32 decode steps, at two
  slicing settings per backend: one layer at a time and a whole token at a time on the CPU; one
  layer per slice and the one-call path on the GPU.
- **What it compares:**
  1. every GPU result against the CPU results on the same machine (this sets the GPU verdict);
  2. the CPU results against a reference shipped with the plugin for the example model, produced
     by SuperSLM's own command-line generator, built independently of the plugin (this sets the
     CPU verdict, and would catch the plugin or the toolchain disturbing the CPU arithmetic);
  3. the plugin's own previous run on this machine, reported as *changed* or *unchanged*, never as
     a pass.
- **The verdict** is *Verified*, *Diverged* or *Not Yet Run* per backend, and it always carries its
  scope, for example: "CPU tokens identical to Layer 1's own sslm_generate reference on the
  reference workload, unconstrained (32 steps x 2 layer budgets: LayerBudget1, LayerBudgetFull);
  final-logit digest unavailable at Layer 1 v1.9.0" (Layer 1 is SuperSLM; see
  [ARCHITECTURE.md](ARCHITECTURE.md#two-layers)). A withheld verdict reads *Not Yet Run*, and its
  scope says it is withheld.
- **The report** names the device, the backend, the GPU head setting, the SuperSLM tag and commit,
  the plugin version, the model's hash, and on a divergence the first step and slicing setting
  that differed. The model's hash is the `.sslm` header's integrity hash (SHA-256 of the file with
  the hash field zeroed), so it differs from the file's own SHA-256 that a download is checked
  against.
- The shipped reference exists only for the example model. For any other model the CPU verdict
  reads *Not Yet Run*, and its scope says the reference is absent; comparison 3 is still reported.
  **So in 1.0 the self-check gives a verdict only for the example model, and only on the CPU.**

The example scene runs the self-check when it loads, on the game thread in bounded steps (one per
frame, with the inference on the plugin's own threads), so the scene keeps rendering. It shows
the CPU verdict with its scope, and withholds the GPU verdict, as every surface does in 1.0.
`SuperSLMExample.SelfCheck` in its console runs it again (see
[GETTING_STARTED.md §6](GETTING_STARTED.md#6-the-example-project)).

**Where to run it:**

- **In the editor:** Tools > Miscellaneous > **SuperSLM Query Window**. Load a model there, then
  press **Run determinism self-check**. It runs one bounded step per editor frame, so the editor
  stays responsive.
- **In a game, including a packaged build:** the Blueprint node **Run Determinism Self-Check**
  (see [BLUEPRINT_API.md](BLUEPRINT_API.md#the-blueprint-surface)), on a model configured on both
  backends. The plugin adds no console command for the self-check (its one console command is
  `SuperSLM.CalibrateCosts`); a game that wants one adds it, as the example project does with
  `SuperSLMExample.SelfCheck`.
- **From C++:** `FSuperSLMSelfCheckRun` steps it a bounded amount per call, and
  `SuperSLMDeterminismSelfCheck::Run()` runs it to completion and blocks, for tests and
  commandlets.

## What the self-check does not cover

- **It compares tokens, not logits.** A device whose logits drift slightly while every chosen token
  stays the same passes. SuperSLM does not yet expose the logit digest that would catch that, and
  the report says so.
- **It covers its workload.** A device that matches on the reference workload could still diverge
  on another prompt. The report names the workload so the claim is never wider than the test.
