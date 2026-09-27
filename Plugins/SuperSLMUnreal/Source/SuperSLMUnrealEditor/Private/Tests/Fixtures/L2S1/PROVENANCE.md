# L2S1 fixture provenance

`l2s1_r_s1a_reference.json` holds 20 reference cases, extracted 2026-09-18 for T-2805 (the L2-S1
tests) and read by `SuperSLML2S1Fixtures.h`'s `LoadReferenceCases()`. Only its `cases` array is
read; `_provenance` is a note for people.

## What was measured

- **Model:** Qwen2.5-0.5B-Instruct, converted with SuperSLM's converter to the CPU reference
  model (test source calls it A-CPU): 514,717,780 bytes, SHA-256
  `8dcd082d1dace85874d6924aab7c2389126638a3dc8531566fb6b2298863a980`. That file is not published
  (CONTRIBUTING.md, *Test inputs*). `SuperSLML2S1Fixtures.h` records its size and hash, but no
  test checks either. The tests that need the file import it, and import checks only the file's
  own header integrity hash, which proves the file is internally consistent, not that it is this
  file. A different conversion of the same checkpoint imports without complaint and then shows up
  as token mismatches against the recorded outputs, not as a wrong-file failure.
- **Generator:** SuperSLM's own `tools/sslm_generate.cpp`, CPU build, at the source of the `v1.5.0`
  tag (T-2783; D-SLM7178, D-SLM7229). The run was made at a commit whose `src/`, `include/` and
  tool source are identical to `v1.5.0`.
- **Settings:** greedy decoding, at most 48 new tokens, stop tokens 151645 and 151643 (included in
  the output when reached). 19 of the 20 cases end on 151645; `open21` runs the full 48.
- **Prompts:** a set of 180 short prompts in six categories (`add`, `sub`, `mul`, `next`, `cap`,
  `open`), each wrapped in the model's chat template. The set itself is not published; the 20
  cases carry their prompts in full.

## Selection

Every ninth prompt in the set's order: index 0, 9, 18, ..., 171 (20 of 180). This covers every
category: `add` (6), `sub` (3), `mul` (4), `next` (2), `cap` (2), `open` (3). The first 20
prompts would all have been `add`.

## Fields

Each case carries:

- `id`, `sourceIndex`: the case's name and its position in the prompt set.
- `promptText`: the prompt as sent, the raw chat-template text.
- `promptTokens`: the token ids the generator recorded for that text. They were recorded
  separately from `promptText`, so a test can tokenize `promptText` with the plugin's own
  `Tokenize()` and compare the result to `promptTokens` before it compares outputs (§9 R-S1a).
- `expectedOutputTokens`, `expectedText`: the generator's recorded output, which the R-S1a tests
  compare the plugin's own decode against.

## What a reader can check

- `promptTokens` against `Tokenize(promptText)` on the release asset
  `qwen2.5-0.5b-instruct-cap4096.sslm`, which carries the same tokenizer.
- `expectedText` against the decode of `expectedOutputTokens`.
- The outputs themselves only with the CPU reference model above: build SuperSLM's
  `tools/sslm_generate.cpp` at the `v1.5.0` tag (the vendored tree is a later tag) and run it with
  the settings above, or run `SuperSLM.L2S1.CpuDeterminism`.

The fixture is a straight field copy from the run's recorded prompts and outputs; no plugin code
produced it.

## The example model (A-EX)

Test source calls the example model A-EX. The one the tests use is the release asset
`qwen2.5-0.5b-instruct-cap4096.sslm`: **510,316,184 bytes, SHA-256
`9ccf7e378bbe47aa0ffd3810aa82875926cbe51af1bdfa08393c3fae657463da`**, header integrity hash
`83e96aa46e1404c824e2bfea22e1511a528298a2edd32299e69a77f414e2007a`. The tests look for it as
`superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm` under `SUPERSLM_ARTIFACTS_DIR`, through
`SuperSLML2S1Fixtures::TryGetAExArtifactPath()`, and name its schema through `AExSchemaName()`.
`AExArtifactExpectedBytes` and `AExArtifactExpectedSha256Hex()` record the size and hash above,
but, as for the CPU reference model, no test checks them: import checks only the header integrity
hash, so a different build shows up as test failures such as token mismatches, not as a
wrong-file failure.

It is Qwen2.5-0.5B-Instruct at `context_cap` 4096, converted with SuperSLM's converter, with
Layer 1's `Tokenizer` (20), `UnicodeTables` (22) and chat-template (21) sections and one compiled
`SCM1` schema section (30) appended. Its section list is `[0, 2, 3, 4, 6, 7, 8, 9, 12, 30, 20,
22, 21]`. The schema section holds two schemas: `potion_shop_order` (the plan's demo schema, §8:
`intent`, `item` and `quantity` enums and a `polite` boolean) and `prompt_result`. The schema
sections were appended by scripts that are not published, so the file cannot be rebuilt from this
repository; its size and hash are what to check.

Two earlier builds of the example model are superseded and no longer read: the first carried no
tokenizer sections (D-SLM7306), and the second (D-SLM7339) added them and was the base of the
current one.

The 1.5B model the adapter tests use (A-AD's base) also has a tokenizer-bearing build (§11.3). No
test uses it yet: `SuperSLML2S1MisuseTests.cpp`'s `CpuAdapterSwapMidTokenDeferred` and
`SuperSLML2S1SaveRestoreTests.cpp`'s `AdapterRebindsOrRefuses` reuse this fixture's 0.5B token ids
instead of calling `Tokenize()` against the 1.5B model, and both pass as written.
