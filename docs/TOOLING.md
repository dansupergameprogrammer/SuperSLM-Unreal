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
- **`SuperSLM.CalibrateCosts <path-to-model.sslm>`**, a console command that measures the CPU
  scheduler's per-unit costs on the machine it runs on, after a warm-up pass. It checks that the
  machine is idle and refuses to report when another process is loading the CPU, naming the
  processes it saw. **It runs synchronously on the game thread**: it prefills the model to each
  measured depth, up to one token below the context capacity (4,095 tokens for the example model),
  then times each cost, and the editor or game does not respond until it finishes. It writes its figures to `Saved/SuperSLM/CalibratedCosts.ini` in the project:
  the eleven costs under `[SuperSLM.CalibratedCosts]`, named as the `FSuperSLMRuntimeConfig`
  fields they correspond to, and the per-depth medians under `[SuperSLM.CalibratedCosts.Depths]`.
  The plugin does not read the file back; copy the values into `FSuperSLMRuntimeConfig` before
  `Configure()`. **Its layer measurement under-prices layers run inside a real job**; see
  [ARCHITECTURE.md](ARCHITECTURE.md#the-cpu-backend) before using its output.
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
self-check's prior runs (not the cost calibration). Its action tools are the next section's.

## Not a 1.0 claim: the MCP action tier

Through the same optional plugin, an AI agent can run schema-constrained inference with a local
model over project data (prompts, or a data table column) and write the results into data table
rows as transactional, undoable edits. It is built and tested, and it is **not a 1.0 claim**:
nothing in 1.0 depends on it, and 1.0's promises do not cover it.
