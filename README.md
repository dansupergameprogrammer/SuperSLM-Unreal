# SuperSLM for Unreal Engine

**A deterministic language model, running inside your game.** On the player's own machine, inside Unreal
Engine 5.8, never on the game thread, and giving the same answer to the same prompt every time.

No server. No API key. No per-request bill. No internet connection. The model ships with your game
as an ordinary Unreal asset, and the plugin runs it in the background while your game keeps its
frame rate.

SuperSLM-Unreal is the Unreal reference implementation of
[SuperSLM](https://github.com/dansupergameprogrammer/SuperSLM), a small-language-model runtime
built from the ground up for games. We don't know of anything else that does what it does: a
0.5B to 1.5B parameter model, on CPU or GPU, scheduled into your frame budget, with exactly
repeatable output.

---

## What you can build with it

- **Characters that understand the player.** A shopkeeper who reads *"two health potions, and be
  quick about it"* and knows it is a purchase, of health potions, two of them, and not very polite.
- **Text or voice commands that drive gameplay.** Turn free-form player input into JSON your code
  can act on, not a paragraph you have to parse and hope about.
- **Dialogue that stays in character,** generated in-engine, on demand, with no round trip to a
  service.
- **A cast of specialists from one model.** Keep one base model loaded and give each character its
  own small LoRA adapter, swapped in at runtime.
- **AI you can replay and test.** Because the same input gives the same output, an AI moment can be
  reproduced exactly: in a replay, in a bug report, in an automated test.

## Schema-constrained generation

This is the feature that turns a language model from a chat toy into a game system.

Give the model a **JSON schema** and it *cannot* write anything that breaks it. Every token it
considers is checked against the schema before it is picked, so an invalid token is never even a
candidate. What comes out is the keys you declared, in the order you declared them, with every
enum value one your game already knows, and it parses whenever the model finishes within its token
budget.

That means the model's answer goes straight into gameplay code: no retry loop, no regex, no
prompt-begging for "valid JSON please", no "the AI said something weird." Ask a guard whether the
player's excuse convinced him and get back `{ "convinced": true, "mood": "suspicious" }`. Turn
*"grab the torch and meet me at the bridge"* into an action your AI controller can execute. The
schema is the contract, and the model can't break it.

And it costs you nothing in repeatability: a schema-constrained answer is exactly as deterministic
as a free-text one. Schemas are compiled into the model file ahead of time and bound per request by
name. See [Assets and Import](docs/ASSETS_AND_IMPORT.md#schemas).

## Runtime-switchable LoRA adapters

Fine-tune a small **LoRA adapter** for each character, faction or job (the blacksmith, the oracle,
the quest-giver who only speaks in riddles), convert it with SuperSLM's tools, and swap it onto a
running model **at runtime**. The base model stays loaded; only the small adapter changes.

- **Fast.** Switching adapters took 0.128 s in SuperSLM's own measurement, against 7.52 s to reload
  a model with the adapter merged in: about 58 times faster (1.5B model, RTX 2080 SUPER).
- **Per conversation.** Each sequence picks its own adapter, so the blacksmith and the oracle can
  run from the same base model side by side.
- **Small to ship.** A cast of specialists is one base model plus a handful of small adapters, not
  a full model per character.
- **Safe.** An adapter is validated against its base model when you import it, and a swap takes
  effect cleanly between tokens.

Prefer a single file per specialist? A LoRA merged into its own `.sslm` works too. See
[Assets and Import](docs/ASSETS_AND_IMPORT.md#adapters).

## Why it works in a game

**It never blocks the game thread.** Inference runs on the plugin's own worker threads (CPU) or on
the GPU. On the GPU, each token is split into slices of a few layers, one slice per frame: at 4
layers per slice, a slice measured about 2 ms on an RTX 2080 SUPER with the 0.5B example model. You
choose the slice size, so you choose what AI costs your frame.

**Same prompt, same answer.** SuperSLM does its math in integers, so on the CPU backend the same
model, prompt and settings produce the same tokens every time, no matter how the work was sliced
or how many frames it took. The plugin even ships a determinism self-check you can run in a
packaged build, on your player's hardware.

**Memory you decide up front.** You size the model's working memory when you configure it, and
nothing the plugin keeps grows while requests run. A cooked model is memory-mapped, not copied: a
510 MB model mapped with about 108 KB of heap in a packaged build.

**Models are Unreal assets.** Drag a `.sslm` file into the Content Browser. The whole file is
validated at import, so a broken model fails in the editor with a reason, never in a player's game.

**And the rest of what a shipping game needs:** save and restore a conversation, share one long
system prompt across many characters, Blueprint nodes and a C++ API, and Unreal Insights trace
channels on both backends.

## See it: the potion shop

The example project is a tiny potion shop. You type what you'd say at the counter, and the model
does two things with it.

First it **records your order**, constrained to the shop's schema, so the result is always
something the game can use directly. For example:

```json
{ "intent": "buy", "item": "health_potion", "quantity": "two", "polite": false }
```

`intent` can only be `buy`, `sell`, `ask_price`, `haggle` or `leave`; `item` can only be something
the shop stocks. The model cannot invent a potion or a field.

Then the shopkeeper **answers you in character**, in one short sentence, knowing what you ordered.

Both run across frames while the scene keeps rendering, on the backend you pick. The example is
plain C++ over the plugin's public API, so it doubles as sample code:
[`ExampleProject/Source/ExampleProject`](ExampleProject/Source/ExampleProject).

## Quick start

You need **Unreal Engine 5.8** on **Windows x64**, a C++ project, and the Windows SDK (the build
uses its `dxc.exe` for the GPU shaders). The plugin compiles from source; nothing is downloaded at
build time.

1. **Install.** Copy [`Plugins/SuperSLMUnreal`](Plugins/SuperSLMUnreal) into your project's
   `Plugins/` folder, regenerate project files and build.
2. **Get a model.** Each release attaches a ready-to-use model, Qwen2.5-0.5B-Instruct converted
   for SuperSLM, on the [Releases](https://github.com/dansupergameprogrammer/SuperSLM-Unreal/releases)
   page (the first release, 1.0, is being finished now). Or convert a model you choose with
   SuperSLM's converter.
3. **Import.** Drag the `.sslm` into the Content Browser.
4. **Ask it something.** In Blueprint: **Load SuperSLM Model**, then **Create Query**,
   **Begin Query** with your prompt, and **Tick Query** once per frame until it finishes.

The same thing in C++, once the model is loaded:

```cpp
FString Error;
Query = USuperSLMQuery::CreateQuery(this, Model, Error);   // keep Query in a UPROPERTY

FSuperSLMQueryConfig Config;
Config.Backend = ESuperSLMBackendBP::GPU;      // or CPU
Config.FrameBudgetLayers = 4;                  // 4 layers of work per frame
Config.StopTokenIds = { 151645, 151643 };      // end of turn / end of text for Qwen2.5

Query->BeginQuery(TEXT("<|im_start|>user\nWhat do you sell?<|im_end|>\n<|im_start|>assistant\n"),
                  /*MaxNewTokens*/ 64, Config, Error);

// Then every frame, for example from your actor's Tick:
bool bSucceeded = false;
FSuperSLMQueryReadout Readout;
if (Query->TickQuery(DeltaSeconds, bSucceeded, Readout, Error) && bSucceeded)
{
    ShowSpeechBubble(Readout.VoicedReply);
}
```

The full walkthrough, including the lower-level C++ API with sequences, schemas and adapters, is in
[Getting Started](docs/GETTING_STARTED.md).

## Where it runs

| | |
|---|---|
| Engine | Unreal Engine 5.8 |
| Platform | Windows x64, verified. Linux and macOS share the sources but have not been built or tested yet. |
| CPU backend | Any x64 CPU; SuperSLM picks SSE2, AVX2 or AVX-512 at startup. Deterministic, verified on Windows. |
| GPU backend | D3D12, Windows. Runs on its own device alongside the engine's renderer. |
| Model size | 0.5B to 1.5B parameters, the range a game can afford in memory and frame time. |

For the precise picture, including what 1.0 has been tested on, what it does not claim yet, and
every measured figure with the machine it came from, see [What 1.0 covers](docs/STATUS.md).

## Documentation

| | |
|---|---|
| [Getting Started](docs/GETTING_STARTED.md) | Install, import a model, make a first request, run the example. |
| [Blueprint and C++ API](docs/BLUEPRINT_API.md) | Every node and call. |
| [Assets and Import](docs/ASSETS_AND_IMPORT.md) | Models, schemas, adapters, cooking. |
| [Architecture](docs/ARCHITECTURE.md) | How the two backends schedule work, and where memory lives. |
| [Determinism](docs/DETERMINISM.md) | What "same answer every time" guarantees, and the self-check. |
| [Profiling](docs/PROFILING.md) and [Tooling](docs/TOOLING.md) | Unreal Insights, editor tools, and the optional, experimental MCP toolset. |
| [What 1.0 covers](docs/STATUS.md) | Tested platforms, limits, and measurements. |

## License

Apache License 2.0, the same as SuperSLM. See [LICENSE](LICENSE) and [NOTICE](NOTICE). The example
model is Qwen2.5-0.5B-Instruct by the Qwen team (Alibaba Cloud), Apache-2.0, converted and
quantized by this project.

Unreal and Unreal Engine are trademarks or registered trademarks of Epic Games, Inc. in the United
States of America and elsewhere. This project is not affiliated with, endorsed by or sponsored by
Epic Games, Inc. or Alibaba Cloud.
