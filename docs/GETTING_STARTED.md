# Getting Started

> **Status:** everything below is built. The example scene has been built, run and packaged on
> Windows x64.

---

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Install the plugin](#2-install-the-plugin)
3. [Get a model](#3-get-a-model)
4. [Import it](#4-import-it)
5. [A first request](#5-a-first-request)
6. [The example project](#6-the-example-project)
7. [Next](#7-next)

---

## 1. Prerequisites

- Unreal Engine 5.8.
- Windows x64, the only verified platform at 1.0. For the GPU backend, a D3D12 GPU.
- The C++ toolchain Unreal Engine 5.8 requires on Windows (Visual Studio with its C++ game
  development workload, at the version Epic's 5.8 documentation lists), since the plugin compiles
  from source (it includes SuperSLM's sources; nothing else is downloaded).
- The Windows 10/11 SDK's DirectX Shader Compiler (`dxc.exe`), which the build uses to compile the
  GPU backend's compute shaders. The build looks under `Windows Kits\10\bin`; set `SUPERSLM_DXC`
  to the full path of a `dxc.exe` to use another. If none is found, the build stops and says so.
- **A C++ project.** A Blueprint-only project cannot compile the plugin; add a C++ class to it
  first (Tools > New C++ Class), which makes it a C++ project.

## 2. Install the plugin

1. Copy `Plugins/SuperSLMUnreal` from this repository into your project's `Plugins/` directory,
   or add this repository's `Plugins` folder to `AdditionalPluginDirectories` in your `.uproject`,
   as `ExampleProject` does. An additional plugin directory must hold plugin folders and nothing
   else: pointing it at the repository root, which also holds `ExampleProject`, or at a plugin's
   own folder, makes packaging stage files under the wrong names.
2. The plugin is enabled by default. To enable or disable it, use `SuperSLMUnreal` in your
   `.uproject`, or **SuperSLM For Unreal Engine** in the Plugins window.
3. Regenerate project files and build the editor target.

## 3. Get a model

The quickest start is the example model attached to each GitHub release: Qwen2.5-0.5B-Instruct,
converted with SuperSLM at a context length of 4096, with the example's two schemas added by a
tool that is not published. Download it from
[github.com/dansupergameprogrammer/SuperSLM-Unreal/releases](https://github.com/dansupergameprogrammer/SuperSLM-Unreal/releases).

The asset is `qwen2.5-0.5b-instruct-cap4096.sslm`, 510,316,184 bytes.
Check the download against its SHA-256:

```
9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da
```

On Windows, `certutil -hashfile <file> SHA256` prints it.

**Rename it `ExampleModel.sslm`.** The example project loads it under that name (see
[§6](#6-the-example-project)); for your own project any name works.

To use your own model, obtain a checkpoint under its own license and convert it with SuperSLM's
converter; see [ASSETS_AND_IMPORT.md](ASSETS_AND_IMPORT.md#where-a-sslm-comes-from). The plugin is
built for models of 0.5B to 1.5B parameters.

## 4. Import it

Drag the `.sslm` file into the Content Browser. Import validates the whole file; a bad file fails
here with the reason, not later in a running game. The resulting `USuperSLMModel` asset shows the
model's provenance in its details, and has the GPU head option (`bGpuDeviceResidentHead`, on by
default).

## 5. A first request

**In Blueprint**, on either backend (see [BLUEPRINT_API.md](BLUEPRINT_API.md#the-blueprint-surface)):

1. **Load SuperSLM Model** with your model asset. Continue from **Loaded**; **Failed** says why.
2. `CreateQuery` for the same model.
3. `BeginQuery` with your prompt text, `MaxNewTokens`, and an `FSuperSLMQueryConfig`: pick the
   `Backend`, and set `StopTokenIds` for your model (the example model's end-of-turn and
   end-of-text ids are 151645 and 151643; the artifact does not record them, so the caller supplies
   them).
4. Call `TickQuery` once per frame, for example from an actor's Tick. When it returns true, read
   `bSucceeded` and the readout's `DisplayedText`.

The prompt is tokenized exactly as given; no chat template is applied.

**In C++**, on the CPU backend. This outline shows the order of calls; see the public headers for
exact signatures.

1. Get `USuperSLMSubsystem` from the game instance.
2. Call `BeginConfigure(Model, Config, OnDone)` once, which maps and measures the model off the
   game thread and calls `OnDone` on it a later frame (`Configure(Model, Config)` does the same
   work on the calling thread and blocks it), with a `FSuperSLMRuntimeConfig` that sets at least
   `BlockCount` (sequences resident at once), `TickBudgetMs` (worker time per job),
   `SequenceLifecycleBudgetMs` (the ceiling on a sequence reset or prefix adopt; at its default of
   0 `Configure()` refuses with `SequenceLifecycleBudgetExceeded`, and the report names the lever)
   and the call shape (`MaxSequencesPerDecodeCall`, `MaxPrefillChunkBudget`, `MaxLayerBudget`).
   Check the returned report's result.
3. Call the subsystem's `Tick(DeltaSeconds)` once per frame from your game code.
4. `VendSequence`, then `Tokenize` your prompt and call `BeginGeneration` with a
   `FSuperSLMGenerationRequest`: the prompt tokens, `MaxNewTokens`, and `StopTokenIds` for your
   model. (The example model's end-of-turn and end-of-text ids are 151645 and
   151643; the artifact does not record them, so the caller supplies them.)
5. Each frame, read `GetPhase` (`Prefilling`, `Decoding`, then `Complete` or `Faulted`) and
   `GetGeneratedTokens`. A fault's reason is written to the log, as a `LogSuperSLM` warning
   `Sequence N faulted: <reason>` (`GPU sequence N faulted: <reason>` on the GPU).
   `GetLastDecodeOutcome` does not carry it: on a faulted sequence it names only a schema dead
   end (`SchemaDeadEnd`) or a released sequence (`SequenceNoLongerValid`), and reads
   `Generating` for every other fault. On the GPU, `GetLastFaultReason` also says whether the
   fault is recoverable or the device was lost. The work runs on a worker
   thread, never on the game thread. Turn the finished tokens back into text with
   `SuperSLM::DetokenizeTokens(Model, Tokens, Text, Error)`, on the game thread.

   A sequence runs one generation at a time. To generate again on the same sequence, call
   `ResetSequence` first and then `BeginGeneration`, in the same frame; without the reset,
   `BeginGeneration` returns false and names the reason. Until the reset takes effect the sequence
   still reads its previous generation, so the new one has ended when `GetPhase` reads `Complete`
   or `Faulted` and `GetPendingLifecycleOperationCount` reads 0; a phase read between ticks can miss
   `Prefilling` and `Decoding` altogether.
6. `ReturnSequence` when finished. If you queue lifecycle operations (`ResetSequence`,
   `SaveSequence`, `RestoreSequence`, `AdoptPrefix`), read each handle's result and then call
   `ReleaseLifecycleOpHandle`: the subsystem keeps every result until its handle is released.

## 6. The example project

> **Status:** the scene below has been built, run and packaged (Windows x64 Development), and its
> packaged check passed, on an AMD Ryzen 9 3950X with an NVIDIA RTX 2080 SUPER. Nothing else in
> this section is a measured result.

`ExampleProject/` is a minimal host project that finds the plugin in the repository's `Plugins/`
folder through `"AdditionalPluginDirectories": ["../Plugins"]`, so a fresh clone builds, runs and
packages with nothing copied. Open `ExampleProject/ExampleProject.uproject` and build when
prompted. Keep the project outside any other Unreal project's directory tree.

**Import the example model first.** Import the `ExampleModel.sslm` from [§3](#3-get-a-model) to
`/Game/SuperSLMExample/` (drag it into that Content Browser folder, or run the import
commandlet; `<UE>` is the engine's install directory and `<repo>` your clone, and both paths must
be full paths, because a stock install does not put `UnrealEditor-Cmd.exe` on the shell's `PATH`,
and the editor resolves a relative project path against its own `Engine\Binaries\Win64` directory;
the command is written for Command Prompt, `cmd.exe`, and in PowerShell the line continuation `^`
is a backtick instead):

```
<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\ExampleProject\ExampleProject.uproject" -run=ImportAssets ^
  -source=<path>\ExampleModel.sslm -dest=/Game/SuperSLMExample -replaceexisting -nosourcecontrol
```

The scene loads the asset `/Game/SuperSLMExample/ExampleModel.ExampleModel`.
`-SuperSLMExampleModel=<object path>` on the command line loads another asset. It takes the object
path, the package path followed by `.` and the asset name (`/Game/Dir/MyModel.MyModel`), not the
package path alone. That folder is ignored by git and is cooked into packaged
builds.

**What the scene does.** There is no map asset: the project's game mode spawns the scene into the
engine's empty entry map, and the scene builds its set, camera and panel in code. On load it
configures both backends and runs the determinism self-check, and shows the CPU verdict with its
scope. In 1.0 the GPU verdict is always withheld (recorded in the report, not shown or used); a
later release shows it. Asking the shopkeeper then runs, across frames:

1. a schema-constrained extraction of a potion-shop order (`potion_shop_order`) from what the
   customer said, checked as valid JSON against the schema's fields;
2. a free-text reply in the shopkeeper's voice, generated from the extracted order.

The panel shows the structured result, the reply, a digest of the tokens, and a cost readout in
which every figure names where it came from.

**Threads.** Nothing heavy runs on the game thread, and while a query runs it does not wait on
another thread. The model package loads asynchronously. The backends are configured with `BeginConfigure`, which maps
and measures the model on the plugin's pool threads and reports back on the game thread a later
frame; the same call reconfigures the GPU backend when a control changes its configuration (a
change of K alone is `SetFixedTickLatency`, which is immediate). Reconfiguring is the one wait:
`BeginConfigure` first tears down the previous GPU configuration on the game thread: it waits for
the submission thread to run every job already queued on it and the teardown, then joins it (a
thread already unresponsive is leaked instead, with no wait and no join; one that stops responding
during the wait is leaked once its stuck call overruns its time bound). The self-check is the plugin's
stepped run: the scene advances it one bounded step per frame on the game thread while its
inference runs on the plugin's own threads, so the scene keeps rendering. A query's inference
runs on the plugin's own threads. The game thread ticks the backends' bookkeeping, reads results,
turns the finished tokens into text, and draws the panel.

**Controls.** Thinking/Frame Budget (one control: layers per slice; the whole-token setting uses
the GPU's one-call path); Backend (CPU or GPU); concurrent queries (up to four); and K (GPU only:
the fixed-tick latency; the panel shows the minimum the current settings need). On the CPU
backend no control changes the answer: the frame budget and the number of concurrent queries
change only how many frames an answer takes and what it costs, and the tokens and the digest stay
the same. The panel says whether the digest matches the first run with the same input. 1.0 does
not claim that the GPU backend's tokens match the CPU's, so GPU runs are compared with that only
when the GPU verdict is shown and reads *Verified*, which in 1.0 it never is.

**Console commands** (the game console, in the editor or a packaged Development build):

| Command | What |
|---|---|
| `SuperSLMExample.Ask [text]` | Asks with the panel's current settings, or with `text` as what the customer said. |
| `SuperSLMExample.SelfCheck` | Runs the self-check again, one bounded step per frame, and shows the CPU verdict (in 1.0 the GPU verdict is always withheld). |
| `SuperSLMExample.PackagedCheck [exit]` | For a packaged build only: in the editor or in PIE its first precondition (`pre.packaged_build`, cooked data) fails, and so does the check. Runs the self-check through the command above, then the default query on the CPU and on the GPU. It checks that the extraction is schema-valid and the reply is voiced, across frames; that the self-check reported; that the GPU dispatched with its device head on, from the shaders staged under the packaged plugin; and that GPU busy time per slice and host finish time were reported at the default setting with the scene rendering. It logs one `PASS`/`FAIL` line per check and writes `Saved/SuperSLMExample/packaged-check-report.json`. With `exit`, the process exits with status 0 when every check passed. |

**Command-line switches:** `-SuperSLMExamplePackagedCheck` runs the packaged check after load and
exits; `-SuperSLMExampleSkipLoad` loads no model, for running the plugin's packaged memory-mapping
test, which must be the first code to load the model package.

The scene carries only generic example schemas.

## 7. Next

- [ARCHITECTURE.md](ARCHITECTURE.md): how the backends schedule work, and what the configuration
  fields mean.
- [DETERMINISM.md](DETERMINISM.md): what is guaranteed, and the self-check.
- [PROFILING.md](PROFILING.md): seeing where the time goes in Unreal Insights.
