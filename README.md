# SuperSLM for Unreal Engine

**A deterministic language model that runs inside your game.** SuperSLM-Unreal is the Unreal
Engine 5.8 reference implementation of [SuperSLM](https://github.com/dansupergameprogrammer/SuperSLM),
a small-language-model runtime written for games. The model runs on the player's own machine, off
the game thread, and gives the same answer to the same prompt every time.

It needs no server, API key or internet connection. The model ships with your game as an Unreal
asset, and the plugin schedules its work around your frame.

> **Status: pre-release.** The source is public ahead of the first release, 1.0. It is built and
> tested on Windows x64. Performance tuning is the current focus: expect generation to take many
> frames, and measure on your own target hardware before you design around it. What 1.0 covers
> and does not cover is in [What 1.0 covers](docs/STATUS.md).

---

## What it is for

- **Understanding what the player says.** Turn typed or transcribed player input into structured
  data your game can act on: a shopkeeper that recognises *"two health potions, please"* as a
  purchase of two health potions.
- **In-character dialogue,** generated in-engine on demand.
- **Several specialised characters from one model,** by giving each its own LoRA adapter.
- **Reproducible AI.** The same input gives the same output, so an AI moment can be reproduced in a
  replay, a bug report or an automated test.

## Features

### Schema-constrained generation

Give the model a JSON schema and its output is guaranteed to fit it. Each candidate token is checked
against the schema before it is chosen, so the result contains the keys you declared, in the order
you declared them, with enum values from your own lists. It parses whenever generation finishes
within its token budget, so your code can read the answer directly rather than validating and
retrying.

Schemas are compiled into the model file ahead of time and selected by name per request. At the
current SuperSLM version they support objects with required keys in a fixed order, enums, booleans
and free-text strings. Constrained output is exactly as deterministic as free text. See
[Assets and Import](docs/ASSETS_AND_IMPORT.md#schemas).

### Runtime-switchable LoRA adapters

Keep one base model loaded and switch a LoRA adapter onto it at runtime, per sequence, so different
characters can use different adapters on the same base model at the same time. Adapters are
converted from standard PEFT LoRA checkpoints with SuperSLM's tools and validated against their base
model at import. A switch takes effect between tokens. In SuperSLM's own measurement (1.5B model,
RTX 2080 SUPER), switching an adapter took 0.128 s, against 7.52 s to reload a model with the
adapter merged in. A LoRA merged into its own `.sslm` file is also supported. See
[Assets and Import](docs/ASSETS_AND_IMPORT.md#adapters).

### Off the game thread, on CPU or GPU

The **CPU backend** runs inference on the plugin's own worker threads and sizes each job to a time
budget you set. The **GPU backend** (D3D12, Windows) splits each token into slices of whole
transformer layers and issues a set number of layers per frame, so you decide how much GPU work a
frame carries. Neither backend runs inference on the game thread.

### Deterministic

SuperSLM computes in integers. On the CPU backend, the same model, prompt and settings produce the
same tokens every time, however the work was sliced and however many frames it took. The plugin
includes a determinism self-check that runs in the editor or a packaged build and reports what it
verified. GPU determinism depends on the device and is not claimed in 1.0. See
[Determinism](docs/DETERMINISM.md).

### Built for shipping

- **Fixed memory.** The model's working memory is sized when you configure the plugin, and nothing
  the plugin keeps grows while requests run. A cooked model is memory-mapped rather than copied into
  the heap.
- **Models are assets.** A `.sslm` file imports through the Content Browser and is fully validated
  there, so a corrupt or unsupported file fails in the editor with a reason.
- **Conversation state.** Save and restore a sequence, and share one long prompt prefix across many
  sequences (CPU backend).
- **Blueprint and C++.** Latent Blueprint nodes for loading and querying, and a full C++ API.
- **Profiling.** Unreal Insights trace channels, timing scopes and counters on both backends.

## The example: a potion shop

`ExampleProject` is a small potion shop. You type what you would say at the counter, and the model
does two things with it. First it records your order against the shop's schema, for example:

```json
{ "intent": "buy", "item": "health_potion", "quantity": "two", "polite": true }
```

`intent` can only be `buy`, `sell`, `ask_price`, `haggle` or `leave`, and `item` only something the
shop stocks. Then the shopkeeper answers in one short sentence, in character, knowing what you
ordered. Both run across frames while the scene renders, on the backend you choose. The example is
plain C++ over the public API: [`ExampleProject/Source/ExampleProject`](ExampleProject/Source/ExampleProject).

## Requirements

- Unreal Engine 5.8.
- Windows x64. Linux x64 and macOS share the sources but have not been built or tested.
- A C++ project and the Visual Studio toolchain UE 5.8 requires. The plugin compiles from source.
- The Windows SDK's DirectX Shader Compiler (`dxc.exe`), used to build the GPU shaders.
- For the GPU backend, a D3D12 GPU.
- A model of 0.5B to 1.5B parameters, the range the plugin is designed for.

## Quick start

1. **Install.** Copy [`Plugins/SuperSLMUnreal`](Plugins/SuperSLMUnreal) into your project's
   `Plugins/` folder, regenerate project files and build.
2. **Get a model.** See [Models](#models) below.
3. **Import.** Drag the `.sslm` file into the Content Browser.
4. **Query it.** In Blueprint: **Load SuperSLM Model**, then **Create Query**, **Begin Query** with
   your prompt, and **Tick Query** once per frame until it finishes.

The same in C++, once the model is loaded:

```cpp
FString Error;
Query = USuperSLMQuery::CreateQuery(this, Model, Error);   // keep Query in a UPROPERTY

FSuperSLMQueryConfig Config;
Config.Backend = ESuperSLMBackendBP::GPU;      // or CPU
Config.FrameBudgetLayers = 4;                  // layers of work per frame
Config.StopTokenIds = { 151645, 151643 };      // end of turn / end of text for Qwen2.5

Query->BeginQuery(TEXT("<|im_start|>user\nWhat do you sell?<|im_end|>\n<|im_start|>assistant\n"),
                  /*MaxNewTokens*/ 64, Config, Error);

// Then once per frame, for example from an actor's Tick:
bool bSucceeded = false;
FSuperSLMQueryReadout Readout;
if (Query->TickQuery(DeltaSeconds, bSucceeded, Readout, Error) && bSucceeded)
{
    ShowSpeechBubble(Readout.VoicedReply);
}
```

[Getting Started](docs/GETTING_STARTED.md) covers the full setup and the lower-level API
(sequences, schemas, adapters, save and restore).

## Models

**The example model.** Each release attaches one ready-to-use model: Qwen2.5-0.5B-Instruct,
converted with SuperSLM at a context length of 4096, with the example project's schemas. It is a
release download rather than a file in the repository, and will appear on the
[Releases](https://github.com/dansupergameprogrammer/SuperSLM-Unreal/releases) page with 1.0.

| | |
|---|---|
| File | `qwen2.5-0.5b-instruct-cap4096.sslm` |
| Size | 510,316,184 bytes |
| SHA-256 | `9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da` |
| License | Apache-2.0, from the source checkpoint: Qwen2.5-0.5B-Instruct by the Qwen team (Alibaba Cloud), converted and quantized by this project |

**Your own models.** No other converted models are distributed. Download a checkpoint under its own
license, convert it with SuperSLM's converter, and import the `.sslm`. The plugin is not tied to one
model. See [Assets and Import](docs/ASSETS_AND_IMPORT.md).

## How it relates to SuperSLM

SuperSLM is the engine-independent runtime: the `.sslm` format, the converter, the inference kernels,
constrained decoding and the determinism guarantee. This plugin builds SuperSLM's sources, vendored
unmodified at a pinned release tag (currently `v1.9.0`), with the engine's own toolchain. Nothing is
downloaded at build time and no prebuilt library is used. For the runtime's internals, see the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM).

## Performance today

Measured in a packaged Development build of the example on one machine (AMD Ryzen 9 3950X,
NVIDIA RTX 2080 SUPER), with the 0.5B example model and the frame rate capped at 60:

| | CPU backend | GPU backend, 4 layers per frame |
|---|---|---|
| Potion-shop query (order plus reply), start to answer | about 9.6 s (579 frames) | about 15.8 s (946 frames) |
| Generation rate | about 7 tokens per second | |
| GPU time per 4-layer slice, nothing else on the GPU | | 1.36 ms median |

The game thread's own cost stays small throughout: the CPU backend's per-frame bookkeeping stays
under 1 ms, which the example's test checks on every tick. Most of the answer time is spent reading the prompt (about 44 ms per prompt token
on the CPU), and speeding that up is the current work. Every figure, with its conditions, is in
[What 1.0 covers](docs/STATUS.md#measured).

## Known limitations

- Windows x64 only for now. The GPU backend is Windows-only.
- GPU determinism is not claimed in 1.0, and the self-check's GPU verdict is withheld.
- Shared prompt prefixes are CPU-only.
- The plugin uses greedy decoding; sampling is not exposed.
- Performance is still being tuned, and every published figure comes from one development machine.

The complete list, with every measured figure and the machine it came from, is in
[What 1.0 covers](docs/STATUS.md) and the [changelog](CHANGELOG.md).

## Also in this repository

- **`Plugins/SuperSLMUnrealMCP`**, an optional MCP toolset through which an AI agent can inspect a
  model and run schema-constrained inference into data tables from the editor. It is disabled by
  default, experimental, and needs the engine's experimental MCP plugins.
- **`ExampleProject`**, the potion shop.
- **`docs/`**, the documentation below.

## Documentation

| | |
|---|---|
| [Getting Started](docs/GETTING_STARTED.md) | Install, import a model, make a first request, run the example. |
| [Blueprint and C++ API](docs/BLUEPRINT_API.md) | Every node and call. |
| [Assets and Import](docs/ASSETS_AND_IMPORT.md) | Models, schemas, adapters and cooking. |
| [Architecture](docs/ARCHITECTURE.md) | How the two backends schedule work, and where memory lives. |
| [Determinism](docs/DETERMINISM.md) | What is guaranteed, and the self-check. |
| [Profiling](docs/PROFILING.md) and [Tooling](docs/TOOLING.md) | Unreal Insights, editor tools, calibration and the MCP toolset. |
| [What 1.0 covers](docs/STATUS.md) | Tested platforms, limits and measurements. |

## Contributing, security and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for building and testing, and [SECURITY.md](SECURITY.md) to
report a vulnerability privately.

Apache License 2.0, the same as SuperSLM. See [LICENSE](LICENSE) and [NOTICE](NOTICE). The example
model is distributed under its source checkpoint's license, as above.

Unreal and Unreal Engine are trademarks or registered trademarks of Epic Games, Inc. in the United
States of America and elsewhere. This project is not affiliated with, endorsed by or sponsored by
Epic Games, Inc. or Alibaba Cloud.
