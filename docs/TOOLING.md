# Editor Tooling

> **Status:** built. The tools below are exercised by the automation tests on Windows x64; this
> repository has no screenshots of them.

---

## Contents

- [The SuperSLM Query Window](#the-superslm-query-window)
- [The model's asset editor](#the-models-asset-editor)
- [Commands and C++ tools](#commands-and-c-tools)
- [The optional MCP toolset](#the-optional-mcp-toolset)

---

Every tool reads state the runtime already exposes, or a mechanism the plugin owns. The runtime
panels are read-only.

## The SuperSLM Query Window

**Tools > Miscellaneous > SuperSLM Query Window**, a dockable tab. Load a model there (it
configures the CPU backend and, if ticked, the GPU backend, off the game thread), then:

- **Run a query** on the CPU or GPU backend with the Frame Budget (layers per slice; 0 is a whole
  token), concurrent queries and k controls, and an option to constrain the answer to a one-field
  JSON schema (`prompt_result`). On the CPU backend, moving concurrency or the frame budget
  changes only how long the answer takes and what it costs, never the answer or its token digest.
  Moving the backend is outside that claim: 1.0 does not claim that the GPU backend's tokens match
  the CPU's (see [DETERMINISM.md](DETERMINISM.md)). Turning schema
  constraint on or off changes the answer, by design; the two settings are not compared with each
  other.
- **Live inspection (read-only):** the context fed to a running query, the tokens returned, and the
  schema state.
- **Cost / budget:** the measured time per job or slice, layers per slice, host finish time, tokens
  per second, GPU time per slice, hitches, declared VRAM, and whether the GPU head is on (and why,
  if it fell back).
- **Pooled resources:** CPU pool and workspace occupancy; GPU declared residency, labelled as a lower
  bound.
- **Model inspector and schema dry-run**, as in the asset editor below.
- **Run determinism self-check**, one bounded step per editor frame. See
  [DETERMINISM.md](DETERMINISM.md).

## The model's asset editor

Double-click a `USuperSLMModel` asset. Two tabs:

- **Details:** the asset's properties, including its provenance (read-only) and the GPU
  device-resident head switch. Edits are ordinary, undoable property edits.
- **Inspector:**
  - **Inspect model:** the artifact's format version, sections, provenance and schemas, and its
    memory footprint for your configuration (workspace, KV block, save-buffer upper bound, and, with
    the GPU head on, the head's VRAM), with the predicted reset and adopt time against your
    sequence lifecycle budget. It runs off the game thread.
  - **Inspect adapter:** validates an adapter file against the model and reports its resident size.
  - **Schema dry-run:** runs one of the model's schemas on a scratch CPU sequence, one tick per
    frame. It needs the model loaded in the query window first.

## Commands and C++ tools

- **The `.sslm` importer.** Validates on import and reports a specific reason on failure. See
  [ASSETS_AND_IMPORT.md](ASSETS_AND_IMPORT.md).
- **`SuperSLM.EstimateCosts "<path-to-model.sslm>" [--write-ini]`**, a console command that
  estimates the CPU scheduler's thirteen per-unit costs on the machine it runs on, for both decode
  modes, at the `FinishParallelTasks` the next `Configure()` would resolve. Its report is headed
  `CALIBRATION ESTIMATE — not verified`. Per mode it warms up with five repetitions of every
  measured call, then times twelve, and reports each call's median and spread (the IQR, which is
  the within-run spread of serial calls, not a confidence interval), thirteen suggested costs
  rounded up, and the CPU load other processes put on the machine (250 ms interval averages,
  which disclose the load and never stop the estimate). The per-call and per-sample series are kept
  under `Saved/SuperSLM/Estimates/<run id>/`. Each reset starts from the deepest prepared context
  (capacity minus one token) and must leave it empty; input restoration and verification are
  outside its timed call. This measures releasing the live pages rather than an empty reset.
  **It runs synchronously on the game thread**: it
  prefills the model to each measured depth, up to one token below the context capacity (4,095
  tokens for the example model), and the editor or game does not respond until it finishes, about
  4-5 minutes for the example model. A run that cannot complete (an invalid model, a failed call,
  a non-finite figure) is reported as an incomplete estimate and suggests nothing. Only
  `--write-ini` writes: after a complete run, both modes' suggestions, with the artifact hash, the
  mode and the task count, replace `Saved/SuperSLM/CalibratedCosts.ini` in one step. The next
  `Configure()` reads a profile from it when the artifact, the effective decode mode and the task
  count match, for every cost field the caller left unset. From a command line, separate commands
  with a comma and end with the editor's exit command:

  ```
  UnrealEditor-Cmd.exe "<Project>.uproject" -unattended -log "-ExecCmds=SuperSLM.EstimateCosts C:/Models/model.sslm,QUIT_EDITOR"
  ```

  Add `--write-ini` inside the quotes, after the path, to save the result.
- **`SuperSLM.SetDecodeMode 0|1 [--save]`** and **`SuperSLM.SetFinishParallelTasks <0..256> [--save]`**,
  console commands that choose the CPU decode mode (1 splits one-row decode projections across the
  finish hook's tasks) and the finish task count for the next `Configure()` whose config leaves
  them unset. A configuration already made keeps its settings. `--save` also keeps the choice in
  `Saved/SuperSLM/RuntimeSettings.ini`, which later sessions, packaged games included, load before
  their first configuration. Both work in the editor and in packaged builds.
- **The conversion pre-check** (`SuperSLMConversionPreCheck::Run`, editor module, C++). It reads a
  checkpoint's configuration and tokenizer files and reports, before you spend time converting,
  every blocker it finds, the context length, the KV block size and the predicted reset and adopt
  cost. A pass means "no known blocker", not "conversion will succeed". The conversion itself is
  SuperSLM's own converter, run by you; the plugin does not run it. There is no editor window for
  the pre-check in 1.0.
- **Unreal Insights instrumentation** on both backends. See [PROFILING.md](PROFILING.md).

## The optional MCP toolset

`SuperSLMUnrealMCP` is a separate plugin in this repository, **disabled by default, marked beta and experimental,
and not a 1.0 claim**. It builds against the engine's Experimental ToolsetRegistry plugin, and an
MCP client reaches its tools through the engine's Experimental ModelContextProtocol plugin, which
you enable in your project; in practice both mean a source-built engine. Its read tools
describe a model, run the conversion pre-check, inspect a sequence and report the determinism
self-check's prior runs (not the cost estimate). Its action tools are the next section's.

## Not a 1.0 claim: the MCP action tier

Through the same optional plugin, an AI agent can run schema-constrained inference with a local
model over project data (prompts, or a data table column) and write the results into data table
rows as transactional, undoable edits. It is built and tested, and it is **not a 1.0 claim**:
nothing in 1.0 depends on it, and 1.0's promises do not cover it.
