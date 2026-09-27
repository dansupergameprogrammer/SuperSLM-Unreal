# Assets and Import

> **Status:** the model asset, its importer, its validation and its asset editor are built and
> tested. Schema lookup and adapter import exist in the C++ API; the asset editor's inspector lists
> a model's schemas, dry-runs one, and inspects an adapter file (see [TOOLING.md](TOOLING.md)).
> Schemas are compiled offline by SuperSLM's tools; 1.0 has no in-editor schema authoring.

---

## Contents

- [Where a `.sslm` comes from](#where-a-sslm-comes-from)
- [The model asset](#the-model-asset)
- [Import and validation](#import-and-validation)
- [Cooking](#cooking)
- [Schemas](#schemas)
- [Adapters](#adapters)
- [The GPU device-resident head](#the-gpu-device-resident-head)

---

## Where a `.sslm` comes from

A `.sslm` file is a model converted and quantized by SuperSLM's converter. The conversion, the
converter's supported model shapes and the file format are SuperSLM's; see the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM). This document covers
the Unreal side.

There are two ways to get one:

- **The example model**, attached to each GitHub release: Qwen2.5-0.5B-Instruct converted at a
  context length of 4096, with two JSON schemas added by a tool that is not published. It is the model the example project and the self-check's shipped
  reference use.
- **Your own**: obtain a checkpoint under its own license and convert it locally. No other
  pre-converted models are distributed, because a converted artifact carries its checkpoint's
  license, model by model.

The plugin is built for the **0.5B to 1.5B** range; every performance figure published here is
for the 0.5B example model, except the GPU load-time bound's upload rate, measured on the 1.5B
model. Larger models convert and import through the same path, but their memory and frame cost are outside what 1.0 is built for.

## The model asset

`USuperSLMModel` wraps one `.sslm` file. Every artifact imports through the same path; the plugin
checks the artifact against SuperSLM's format, not against any particular model.

## Import and validation

Drag a `.sslm` into the Content Browser, or use the import factory. Import runs SuperSLM's full
audit of the file: the magic number and version, every section's bounds, alignment, overlap and
data type, and the header's integrity hash (a SHA-256 of the file with the hash's own bytes
zeroed).

- **A bad file fails in the editor, with a specific reason.** The importer reports the section and
  the message SuperSLM gives, not a bare "rejected".
- **A file from a newer SuperSLM** (a header flag the pinned SuperSLM does not know) is reported
  as needing a newer SuperSLM than the plugin pins, not as corrupt.
- The artifact's embedded provenance (source checkpoint name, license identifier, source hash)
  appears in the asset's details.

## Cooking

On Windows, a cooked model asset is stored as a memory-mappable bulk-data payload, and at load the
asset takes the file mapping instead of copying the weights onto the heap. The mapping's alignment
on Windows satisfies every `.sslm` section alignment. In the editor, the asset holds an aligned
copy instead.

**Measured:** in a packaged Windows Development build of the example project, mapping the
510,316,184-byte example model grew the heap by at most 108,418 bytes (peak net allocation, 0.02 %
of the model), so the weights are not copied. Measured on an AMD Ryzen 9 3950X.

The mapping has been measured only on Windows. On Linux and macOS, which are not verified, whether
the engine maps the payload or copies the model onto the heap at load has not been checked.

## Schemas

Constrained decoding uses JSON schemas compiled offline, by SuperSLM's tools, into a section of
the `.sslm` file. The plugin looks a schema up by name in a model (`FSuperSLMSchemaLookup`, and
`FSuperSLMGpuSchemaLookup` for the GPU backend); a name the model does not carry fails at lookup.
A schema is bound to a sequence when the sequence starts, and changing it requires a reset.

What the schema compiler accepts is SuperSLM's to define: at the current pin, objects with known,
required keys in a fixed order, enums, booleans, and unbounded free-text strings. See SuperSLM's
documentation for the exact subset.

## Adapters

A base model stays resident and a sequence is re-pointed to a different adapter between requests,
so a set of specialists is one base plus several small adapters rather than several full models.
`FSuperSLMAdapterImport` validates an adapter against its base model and reports its resident
size. A swap takes effect only at a token boundary.

A specialist merged into its own standalone `.sslm` file is the supported alternative. The plugin
loads either form.

## The GPU device-resident head

`USuperSLMModel::bGpuDeviceResidentHead` (on by default, editable in the asset's details, cooked
with the asset) chooses whether the GPU backend computes each token's logits on the GPU. It is a
property of the asset, not of the `.sslm` file: the same file can be used with the head on in one
asset and off in another, and its hash does not change. The CPU backend ignores it.

With the head on, the GPU backend declares the head's buffers as part of its VRAM residency. For
the 0.5B example model the requested buffers total 137,353,728 bytes (computed from the model's
shape, not measured); SuperSLM measured
137,433,088 bytes on an NVIDIA RTX 2080 SUPER. Declared residency is a lower bound on what the
driver occupies.
