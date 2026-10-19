# SuperSLM-Unreal Documentation

> **Status: 1.0.** Every number is a measurement, or computed where marked, labelled with where it
> came from.

Start with the repository [README](../README.md) for what the plugin does, and
[STATUS.md](STATUS.md) for exactly what 1.0 claims, the supported platforms, and every measured figure.

## Reading order

1. [GETTING_STARTED.md](GETTING_STARTED.md): install the plugin, import a model, make a first
   request.
2. [ARCHITECTURE.md](ARCHITECTURE.md): the modules, the two backends and their schedulers,
   memory, and how SuperSLM is built in.
3. [BLUEPRINT_API.md](BLUEPRINT_API.md): the C++ API and the Blueprint surface.

## Reference

| Document | Scope |
|---|---|
| [STATUS.md](STATUS.md) | What 1.0 has been built and tested on, what it does not claim, platforms, and the measured figures. |
| [ASSETS_AND_IMPORT.md](ASSETS_AND_IMPORT.md) | The `.sslm` model asset, import validation, cooking, schemas, adapters, the GPU head option. |
| [DETERMINISM.md](DETERMINISM.md) | What is guaranteed, the per-device GPU question, and the self-check with its limits. |
| [PROFILING.md](PROFILING.md) | Trace channels, timing scopes, counters, stats, CSV categories and bookmarks. |
| [PAGING.md](PAGING.md) | 1.2.0's budget mode, shared/nested prefixes, generation limits, restore bursts and compatibility, saves across releases, page statistics and the GPU pool cap. |
| [TOOLING.md](TOOLING.md) | The editor tools, the cost estimate and decode-settings commands, and the optional MCP toolset. |

## SuperSLM

The runtime's internals (the C ABI, the `.sslm` format, the converter, constrained decoding, and
how determinism is achieved) are documented in the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM), not repeated here.

## Contributing and license

- [CONTRIBUTING.md](../CONTRIBUTING.md): building, testing and proposing changes.
- [LICENSE](../LICENSE): Apache License 2.0.
