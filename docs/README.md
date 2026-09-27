# SuperSLM-Unreal Documentation

> **Status: 1.0.** Every number is a measurement, or computed where marked, labelled with where it
> came from.

Start with the repository [README](../README.md) for what the plugin does, what 1.0 claims, and
the supported platforms.

## Reading order

1. [GETTING_STARTED.md](GETTING_STARTED.md): install the plugin, import a model, make a first
   request.
2. [ARCHITECTURE.md](ARCHITECTURE.md): the modules, the two backends and their schedulers,
   memory, and how SuperSLM is built in.
3. [BLUEPRINT_API.md](BLUEPRINT_API.md): the C++ API and the Blueprint surface.

## Reference

| Document | Scope |
|---|---|
| [ASSETS_AND_IMPORT.md](ASSETS_AND_IMPORT.md) | The `.sslm` model asset, import validation, cooking, schemas, adapters, the GPU head option. |
| [DETERMINISM.md](DETERMINISM.md) | What is guaranteed, the per-device GPU question, and the self-check with its limits. |
| [PROFILING.md](PROFILING.md) | Trace channels, timing scopes, counters, stats, CSV categories and bookmarks. |
| [TOOLING.md](TOOLING.md) | The editor tools, the calibration command, and the optional MCP toolset. |

## SuperSLM

The runtime's internals (the C ABI, the `.sslm` format, the converter, constrained decoding, and
how determinism is achieved) are documented in the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM), not repeated here.

## Contributing and license

- [CONTRIBUTING.md](../CONTRIBUTING.md): building, testing and proposing changes.
- [LICENSE](../LICENSE): Apache License 2.0.
