# What 1.0 covers

The front page is the [README](../README.md). This page is the precise version: what 1.0 has been built and tested on, what it does not claim, and every figure on file with the machine it came from.

> **Status: 1.0.** Built and tested on Windows x64: the model asset and importer, the CPU and GPU
> backends with their schedulers, save and restore, shared prefixes (CPU backend only), the
> determinism self-check, the Blueprint surface, the editor tools, the Unreal Insights
> instrumentation, and the example project, which has been built, run and packaged. What 1.0 does not claim is listed under
> *Known limitations* in [CHANGELOG.md](../CHANGELOG.md).

## The guarantees, precisely

What 1.0 provides:

1. **Deterministic on-device inference within a frame budget.** On the CPU backend the plugin owns
   the memory the runtime needs (on the GPU backend, SuperSLM allocates device memory per sequence),
   and on both it schedules the work against the engine's frame. On the CPU backend, the same
   request produces the same tokens however the work was sliced and however many frames it took;
   this is verified on Windows x64. GPU determinism is a property of each device, and 1.0 does not
   claim it.
2. **Engine-owned memory.** On the CPU backend the plugin sizes and owns the KV pool and the
   workspaces when you configure it, and, provided each lifecycle handle is released
   (`ReleaseLifecycleOpHandle`), nothing the plugin keeps grows while requests run. What it does
   allocate per request is small bookkeeping, freed when the request completes or its handle is
   released (see [ARCHITECTURE.md](ARCHITECTURE.md#memory) for the exact list).
3. **Two backends, each with its own scheduler.** The CPU backend runs inference on plugin-owned
   worker threads, never on the game thread; each token's finish is spread over the engine's task
   workers (`FinishParallelTasks`, default 4). The GPU backend (D3D12) slices each token into a
   caller-set number of whole layers per tick: at 4 layers on an RTX 2080 SUPER with the 0.5B
   model, a slice measured about 2 ms (see [Measured](#measured)). The Load node and a default
   query run a whole token per tick.
4. **Specialization by adapter.** A base model stays resident and a sequence is re-pointed to a
   different adapter between requests. A specialist merged into its own `.sslm` file is the
   supported alternative, and the plugin loads either form.
5. **A cooked `.sslm` model asset.** Import validates the whole artifact in the editor, so a
   corrupt or unsupported file fails there and never reaches a running game. On Windows the cooked
   asset is designed to be memory-mapped rather than copied onto the heap.
6. **A determinism self-check** you can run in the editor or in a packaged build, because the
   device that matters is your player's. It reports *Verified*, *Diverged* or *Not Yet Run* per
   backend, and states exactly what it covered. In 1.0 it gives a verdict only for the example
   model, and only on the CPU: another model reads *Not Yet Run* on the CPU, and the GPU verdict is
   always withheld.
7. **Editor tooling and Unreal Insights instrumentation:** trace channels, timing scopes,
   counters and bookmarks on both backends.

The product tier is **0.5B to 1.5B parameters**. A game has little VRAM and frame time to spare,
and that is the range this plugin is built for. Every performance figure published here is for the
0.5B example model, except the GPU backend's model-upload rate behind its load-time bound (142 MB/s),
which was measured loading the 1.5B model on the maintainer's machine (AMD Ryzen 9 3950X, NVIDIA RTX
2080 SUPER); the tests also run a 1.5B model.

## The GPU claim: frame cost, not speed

What the GPU backend buys over the CPU backend is CPU time: inference runs on the GPU in slices of whole layers. At 4 layers per slice on an RTX 2080 SUPER with the 0.5B model, a slice measured about 2 ms.

The per-slice GPU times on file, with and without the engine rendering, are under
[Measured](#measured).

## Not a 1.0 claim: the MCP toolset

The repository also carries a second, separate plugin, `SuperSLMUnrealMCP`, through which an AI
agent can, over MCP, describe a model, run the conversion pre-check, inspect a sequence, report the
determinism self-check's prior runs, and run schema-constrained inference with a local model whose
results are written into data table rows as undoable edits. It ships **disabled by default, marked beta and experimental,
and it is not a 1.0 claim.** It builds against the engine's Experimental ToolsetRegistry plugin,
and an MCP client reaches its tools through the engine's Experimental ModelContextProtocol plugin,
which you enable in your project. In practice both mean a source-built engine.

## Platforms

| Platform | 1.0 status |
|---|---|
| **Windows x64**, CPU backend | Supported and verified. |
| **Windows x64**, GPU backend (D3D12) | Supported. Its determinism is not claimed in 1.0: GPU determinism is per device, and in 1.0 the self-check's GPU verdict is always withheld (recorded in the report, *Not Yet Run* on every public surface). A later release shows it. SuperSLM itself certifies two GPUs: the NVIDIA RTX 2080 SUPER and the AMD Radeon RX 7900 XTX. |
| **Linux x64**, **macOS (Apple Silicon)** | Share the same sources, **not verified**: neither has been built or tested as an Unreal plugin. The GPU backend is Windows-only. |

- **Unreal Engine 5.8.**
- The GPU backend runs its own D3D12 device, separate from the engine's renderer, and loads its
  compute shaders from the plugin's own directory.
- The GPU backend has been run in a source-built engine's editor and in a packaged Development
  build. In an installed (launcher) engine's editor it has
  {INSTALLED_ENGINE_GPU}.

## Relationship to SuperSLM

SuperSLM is the engine-agnostic runtime. It owns the `.sslm` format, the converter, the inference
kernels, constrained decoding and the determinism guarantee, and it builds and tests without
Unreal. This plugin depends on SuperSLM; SuperSLM never depends on the plugin.

**The plugin compiles SuperSLM's sources with the engine's own toolchain, at a pinned release
tag.** The sources are vendored, unmodified, under `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM/`.
`VENDORED_VERSION.txt` there names the tag and commit, and a manifest records every file's hash.
Nothing is downloaded at build time and no prebuilt SuperSLM library is used. Because the plugin
builds against SuperSLM's C++ headers as well as its C ABI, a change of pin is a deliberate plugin
change, not a drop-in upgrade.

**1.0 vendors SuperSLM `v1.9.0`** (commit `d870d27`).

For the runtime's internals (the C ABI, the artifact format, the converter, and how determinism
is achieved), see the [SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM).
These docs do not repeat them.

## Getting a model

**The example model.** Each release attaches one ready-to-use model artifact to its GitHub
release: **Qwen2.5-0.5B-Instruct**, converted with SuperSLM at a context length of 4096, with two
JSON schemas (`potion_shop_order` and `prompt_result`) added to its schema section by a tool that
is not published, so the file cannot be rebuilt from this repository. It is
what the example project and the self-check's shipped reference use. It is a release asset, not a
file in this repository, so clones stay small.

| Release asset | |
|---|---|
| File name | `qwen2.5-0.5b-instruct-cap4096.sslm` |
| Size | 510,316,184 bytes |
| SHA-256 | `9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da` |
| License | Apache-2.0, the source checkpoint's license, as its model card states |
| Attribution | Qwen2.5-0.5B-Instruct by the Qwen team (Alibaba Cloud), Apache-2.0; converted and quantized (modified) by this project |

**Your own models.** Beyond that one example, no pre-converted models are distributed, and none
are planned. A converted artifact is a derivative of its source checkpoint and carries that
checkpoint's license, so the supported path is: **obtain a checkpoint yourself, convert it locally
with SuperSLM's converter, and import the `.sslm`.** Any checkpoint the converter accepts imports
through the same path; the plugin is not tied to one model. See
[ASSETS_AND_IMPORT.md](ASSETS_AND_IMPORT.md).

## Scheduling costs are configuration, measured on one machine

The CPU scheduler sizes each worker job from fixed per-unit costs (milliseconds per layer, per
prompt token, per token finish, and per lifecycle operation) rather than from a live estimate, so
the job size is predictable. The shipped defaults were measured on one machine, an AMD Ryzen 9
3950X, and two of them were then **set by hand**: the layer cost measured by the calibration
command came out too low for layers run inside a real job, so `LayerCostMs` (2.0) and
`LayerCostPerPositionMs` (0.001) ship above the measured values.

The `SuperSLM.CalibrateCosts` console command measures these costs on your own hardware. **Its
layer measurement under-prices layers run inside a job**, so do not lower `LayerCostMs` below the
shipped value on the strength of a calibration run alone. An under-priced cost makes jobs overrun
their CPU budget; an over-priced one only lowers throughput. See
[ARCHITECTURE.md](ARCHITECTURE.md#the-cpu-backend).

## Measured

Every figure here is a measurement, or computed where marked, with the machine it came from.

| What | Result | Where |
|---|---|---|
| Determinism self-check, CPU | *Verified* at SuperSLM `v1.9.0`, in a packaged Windows Development build of the example | AMD Ryzen 9 3950X CPU, Windows x64 |
| Determinism self-check, GPU | Withheld: in 1.0 the GPU verdict is always withheld (recorded in the report, shown as *Not Yet Run*); a later release shows it | |
| GPU device-resident head, VRAM per mapped 0.5B model | 137,353,728 B requested buffers (a lower bound on what the driver occupies) | Computed from the model's shape (not a measurement); SuperSLM measured 137,433,088 B on the RTX 2080 SUPER |
| GPU time per slice, 4 layers per slice | Below the table | NVIDIA RTX 2080 SUPER, 0.5B example model |
| Cooked-load heap growth for a memory-mapped model, packaged Windows build | 108,418 bytes peak net allocation while mapping the 510,316,184-byte example model (0.02 %) | Packaged Windows Development build of the example, AMD Ryzen 9 3950X |

**GPU time per slice.** Measured on one RTX 2080 SUPER at 4 layers per slice (the example
project's setting): the
median GPU slice takes {HEADLESS_MS} ms with nothing else on the GPU. With the example scene
rendering at {FPS} frames per second, the measured slice span has a median of {RENDERED_MS} ms.
That span includes time the GPU spent on the scene's own rendering while the slice ran, so it is an
upper bound on the slice's own cost. In the editor, with its viewport rendering, the median at 4
layers was {RS3F_MS} ms.

A caller who configures the GPU backend directly sets the slice size (`DispatchBudget` is
required). The Blueprint Load node and a default query run a whole token per tick, and 4 layers
per slice is the example project's setting.

