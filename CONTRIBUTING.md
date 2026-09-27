# Contributing to SuperSLM-Unreal

## Scope

This repository is the Unreal Engine 5.8 integration of SuperSLM. The runtime itself lives in the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM) and is vendored, at a
pinned release tag, under `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM/`.

**Do not edit the vendored tree.** A change to SuperSLM is made upstream, released, and brought in
by moving the pin: the tree is replaced with the new tag's files (read as raw git blobs, so line
endings are not converted), `VENDORED_VERSION.txt` and `VENDORED_MANIFEST.sha256` are regenerated,
and the coherence checks confirm the result (see *Moving the SuperSLM pin* below). A change that would be easier with a change to
SuperSLM's API is proposed upstream on its own merits, never as a convenience for this plugin.

## Building

1. Unreal Engine 5.8 on Windows x64 (the only verified platform at 1.0), and the Windows 10/11
   SDK: the build compiles the GPU backend's compute shaders with its `dxc.exe`, found under
   `Windows Kits\10\bin`, or the file `SUPERSLM_DXC` names.
2. Open `ExampleProject/ExampleProject.uproject`, which finds the plugin in `Plugins/` through
   `"AdditionalPluginDirectories": ["../Plugins"]`, and build when prompted. Or build from the command line:

   ```
   <UE>\Engine\Build\BatchFiles\Build.bat ExampleProjectEditor Win64 Development -Project="<repo>\ExampleProject\ExampleProject.uproject" -WaitMutex
   ```

## Testing

The automation tests are named `SuperSLM.*`, and the example project's own test
`SuperSLMExample.*`. The commands below take full paths, as the build line above does: `<UE>` is
the engine's install directory and `<repo>` your clone. `UnrealEditor-Cmd.exe` is not on `PATH` on
a stock install, and the editor resolves a relative project path against its own
`Engine\Binaries\Win64` directory, so a relative `.uproject` path is not found.

These command lines, and the build line above, are written for Command Prompt (`cmd.exe`). In
PowerShell, set a variable with `$env:NAME = "value"` instead of `set NAME=value`, continue a line
with a backtick instead of `^`, and put `& ` before an executable path you have quoted. The pin move
below is written for Git Bash.

The test names' middle part (`L2S0` to `L2S3`, `U1`) is the stage of the private design history
that added the test, and `R-S…` ids in the source name the requirement a test covers. Roughly:
`SuperSLM.L2S0` is import, cooking and memory mapping; `L2S1` the CPU backend; `L2S2` the GPU backend
and the self-check; `L2S3` the Blueprint surface, the editor tools and MCP; `U1` later fixes to both
backends (`U1.Cpu`, `U1.Gpu`) and the cost calibration (`U1.Calibration`, `U1.Relevance`). `U0`,
which appears only in source comments, was a clean-up stage: it removed tests, a commandlet and a
CI workflow, and no current test is named after it.

**On any machine**, run this. It leaves out every test whose result depends on which GPU the
machine has or on figures measured on the maintainer's machine (the GPU backend's tests and the
example test), the calibration tests and the rendering test. Two tests it keeps are timed: see
below.

```
<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\ExampleProject\ExampleProject.uproject" -ExecCmds="Automation RunTests SuperSLM.L2S0+SuperSLM.L2S1+SuperSLM.U1.Cpu+SuperSLM.L2S3.BlueprintParity+SuperSLM.L2S3.PreCheck+SuperSLM.L2S3.MCP;Quit" -unattended -nopause -nullrhi
```

It runs the import, cooking and memory-mapping tests, the CPU backend's tests, and the Blueprint
parity, pre-check and MCP tests. One of them, `SuperSLM.U1.Cpu.TickBudgetNonFinite`, also checks a
GPU configuration refusal, so it needs a D3D12 GPU (any model). Two others hold the CPU
scheduler's game-thread work to under 1 ms per tick, the plugin's own bound:
`SuperSLM.L2S1.AsyncScheduling.GameThreadTickCostBounded` (the maintainer's machine reads about
three orders of magnitude below it) and `SuperSLM.U1.Cpu.RestoreTickBound`. A very slow or heavily
loaded machine can fail them without a defect in your change.

**The routine run**, the one the maintainer runs before a release, uses the same filter as
`.github/workflows/plugin-build.yml` (`TEST_FILTER`), headless. It adds the GPU backend's tests and
the example test to the run above:

```
<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\ExampleProject\ExampleProject.uproject" -ExecCmds="Automation RunTests SuperSLM.L2S0+SuperSLM.L2S1+SuperSLM.L2S2+SuperSLM.U1.Cpu+SuperSLM.U1.Gpu+SuperSLM.L2S3.AdapterSwitch+SuperSLM.L2S3.Blueprint+SuperSLM.L2S3.PreCheck+SuperSLM.L2S3.PromptResult+SuperSLM.L2S3.Query.+SuperSLM.L2S3.QueryWindow.Checkbox+SuperSLM.L2S3.QueryWindow.GpuQueryRunsWithinDrainBound+SuperSLM.L2S3.QueryWindow.InvariantHolds+SuperSLM.L2S3.MCP+SuperSLMExample;Quit" -unattended -nopause -nullrhi
```

- The GPU tests need a D3D12 GPU. They run under `-nullrhi`, because the GPU backend creates its
  own D3D12 device.
- Some tests hold the plugin to figures from the maintainer's machine (AMD Ryzen 9 3950X, NVIDIA
  RTX 2080 SUPER): the example test's median GPU slice of at most 2.0 ms at 4 layers per slice and
  its CPU game-thread `Tick()` under 1 ms, and GPU tests that compare the GPU backend's tokens with a
  recorded SuperSLM run (`SuperSLM.L2S2.SliceInvariance.AgainstSslmGenerateAnchor`). GPU
  determinism is per device, and SuperSLM certifies only the RTX 2080 SUPER and the AMD Radeon RX
  7900 XTX. On other hardware these tests can fail without a defect in your change.
- The calibration tests (`SuperSLM.U1.Calibration`, `SuperSLM.U1.Relevance`) need an otherwise idle
  machine and are not in the filter.
- `SuperSLM.L2S3.QueryWindow.GpuSliceDurationSweepWithRendering` is not in the filter either: it
  measures slices with the editor rendering, so under `-nullrhi` it refuses by name, and it also
  refuses on any GPU but the NVIDIA RTX 2080 SUPER its reference figures come from. Run it in an
  editor with rendering.
- The `SuperSLM.L2S3.MCP` tests run only when the optional MCP plugin is enabled.

On an AMD Ryzen 9 3950X with an NVIDIA RTX 2080 SUPER, with the MCP plugin enabled, a routine run
took about 78 minutes.

**Every test**, the calibration and rendering tests included, is
`-ExecCmds="Automation RunTests SuperSLM;Quit"` on the same command line. Off the maintainer's
machine, expect those to fail.

**Not every file the routine run needs is published.** Three are not: the CPU reference model
(`qwen2.5-0.5b-instruct.sslm`, the exact file whose size and SHA-256 `SuperSLML2S1Fixtures.h`
records, because the recorded reference outputs came from it; the tests do not check those two
values, so a different conversion shows up as token mismatches), the converted Qwen2.5-1.5B-Instruct
model, and the LoRA adapter. No recipe here reproduces them byte for byte. Without them, the
tests that need them (listed under `SUPERSLM_HF_CACHE` below) fail when they load them. The
failure names the model that did not load by its short name in test source (A-CPU is the CPU
reference model, A-AD the 1.5B model and its adapter); it does not always name the variable or
the path. Everything else in the routine run needs only published inputs: the release asset
(`qwen2.5-0.5b-instruct-cap4096.sslm`), the public Hugging Face snapshots, and the files in this
repository. A contributor can run the rest, and the maintainer runs the whole suite before a
release.

### Test inputs

The models and converted artifacts the tests run against are too large to commit. A machine says
where they are through environment variables; when one is unset, a test that needs it fails at
its file check, and the path in the message starts with `<VARIABLE>-is-unset/`, naming what to
set (`Plugins/SuperSLMUnreal/Source/SuperSLMUnrealEditor/Private/Tests/Fixtures/SuperSLMTestDataPaths.h`).
The other `Fixtures/` paths on this page are in that same `Tests/` directory.

Test source names three of these inputs by code: **A-EX** is the example model (the release
asset), **A-CPU** the CPU reference model, and **A-AD** the converted Qwen2.5-1.5B-Instruct model
with its LoRA adapter.

| Variable | What it points to | Tests that need it |
|---|---|---|
| `SUPERSLM_ARTIFACTS_DIR` | A directory holding `superslm/aex/qwen2.5-0.5b-instruct-cap4096-aex-pr.sslm`: the example model, byte-identical to the release asset (same size and SHA-256). Download it from the release and place it there under that name. `SUPERSLM_L2S1_AEX_PATH` or `SUPERSLM_L2S2_AEX_PATH` can point to the file directly instead. | Most of `SuperSLM.L2S1`, `SuperSLM.L2S2`, `SuperSLM.L2S3`, `SuperSLM.U1.Cpu` and `SuperSLM.U1.Gpu` |
| `SUPERSLM_HF_CACHE` | A model-cache root holding `hub/` (Hugging Face snapshots of Qwen2.5-0.5B-Instruct, Qwen2.5-1.5B-Instruct and Qwen3-Embedding-0.6B, at the revisions named in `Fixtures/L2S3/SuperSLML2S3Fixtures.h`) and `superslm_artifacts/` (artifacts converted with SuperSLM's converter: `qwen2.5-0.5b-instruct.sslm`, the CPU reference model, and `qwen2.5-1.5b-instruct.sslm` with the LoRA adapter named in `Fixtures/SuperSLML2S1Fixtures.h`). The CPU reference model must be the exact file whose size and SHA-256 `SuperSLML2S1Fixtures.h` records, because the recorded reference outputs came from it (the tests do not check those values; a different file shows up as token mismatches); `SUPERSLM_L2S1_ACPU_PATH` can point to it directly. The CPU reference model, the 1.5B model and the adapter are not published (see above). | `hub/`: `SuperSLM.L2S3.PreCheck`. The CPU reference model: `SuperSLM.L2S1.CpuDeterminism`, `.Misuse`, `.SaveRestore`, `.Teardown`, `SuperSLM.L2S3.BlueprintParity`. The 1.5B model and adapter: `SuperSLM.L2S1.Misuse`, `.SaveRestore`, `SuperSLM.L2S3.AdapterSwitch`, `SuperSLM.L2S3.Blueprint`, `SuperSLM.U1.Cpu`, `SuperSLM.U1.Gpu` |
| `SUPERSLM_BUILD_ROOT` | A checkout that has built the plugin (the directory holding `Plugins/`), for the Python checks under `Plugins/SuperSLMUnreal/ci/tests` that read the module's captured compile line (`SuperSLMUnreal.Shared.rsp` and the per-file `.rsp` files a build writes under the plugin's `Intermediate/`) when the current checkout has not built it. With neither this checkout's build nor the variable, the two status-switch checks skip, but five vendoring checks (`test_thirdparty_vendoring.py`'s `test_e`, `test_f`, `test_g`, `test_g2` and `test_h`) **fail** with "no captured SuperSLMUnreal.Shared.rsp found": they pass once a build has produced the compile line. | `ci/tests`: the vendoring and status-switch checks |
| `SUPERSLM_BUILD_LOCK` | The path of a build lock directory, when builds on the machine take one. `SuperSLM.CalibrateCosts` refuses to write its figures while a foreign owner holds it. Unset, the lock check is not configured and never refuses. | `SuperSLM.U1.Calibration`, `SuperSLM.U1.Relevance` |
| `SSU_BUILD_LOCK_TOKEN` | The token a build wrapper that holds the lock passes to the run it launches: the first line of `owner.txt` inside the lock directory. With it, a lock held by that wrapper reads as the run's own; without it, any lock present reads as foreign. | `SuperSLM.U1.Calibration`, `SuperSLM.U1.Relevance` |
| `SUPERSLM_EXAMPLE_MODEL_FILE` | The release asset `qwen2.5-0.5b-instruct-cap4096.sslm`, for the example project's test when the project has not imported it as `/Game/SuperSLMExample/ExampleModel`. With neither, the test fails and says so. | `SuperSLMExample` |

The self-check's shipped reference is found in `Plugins/SuperSLMUnreal/Resources/SelfCheck/`
(`SUPERSLM_L2S2_REFERENCE_DIGEST_PATH` overrides it). The packaged memory-mapping test's real-scale case
loads the cooked model package named by `SUPERSLM_L2S0_REALSCALE_MAPPED_PACKAGE`, and skips by name
when it is unset. The small hostile `.sslm` files
the import tests use are committed, with their generators, under `Fixtures/SSLM/`.

### Packaged tests

Two checks run only in a packaged build. Import the example model first ([docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)), then package the example project (`<out>` is any empty directory):

```
<UE>\Engine\Build\BatchFiles\RunUAT.bat BuildCookRun -project="<repo>\ExampleProject\ExampleProject.uproject" -platform=Win64 -clientconfig=Development -build -cook -stage -pak -iostore -archive -archivedirectory="<out>" -unattended -utf8output
```

- **The example's packaged check.** Run `<out>\Windows\ExampleProject.exe -SuperSLMExamplePackagedCheck -windowed -log`.
  It runs the self-check and a query on each backend, logs one `PASS`/`FAIL` line per check
  (`SuperSLMExample.PackagedCheck:`), writes `Saved/SuperSLMExample/packaged-check-report.json`,
  and exits with status 0 only when every check passed. It needs a D3D12 GPU.
- **The plugin's packaged memory-mapping tests.** They live in the plugin's `SuperSLMUnrealTests`
  module, a developer-tool module that a Development build includes. The real-scale case must be
  the first code to load the model package, so `-SuperSLMExampleSkipLoad` keeps the example from
  loading it:

  ```
  set SUPERSLM_L2S0_REALSCALE_MAPPED_PACKAGE=/Game/SuperSLMExample/ExampleModel
  <out>\Windows\ExampleProject.exe -SuperSLMExampleSkipLoad -ExecCmds="Automation RunTests SuperSLM.L2S0.MemoryMapping;Quit" -unattended -log
  ```

The most important tests are the token-identity tests: the same tokens across slicing settings,
concurrency, backends and tracing, checked against SuperSLM's own independently built generator.
A change on the inference path keeps them green.

The coherence checks need Python 3, git, and network access to GitHub: the first one fetches the
pinned tag from the upstream SuperSLM repository.

```
python .github/scripts/check_vendored_against_tag.py --vendored-dir Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM --manifest-checker Plugins/SuperSLMUnreal/ci/check_vendored_tree_matches_manifest.py
python Plugins/SuperSLMUnreal/ci/check_vendored_wrappers_complete.py --plugin-root Plugins/SuperSLMUnreal
python .github/scripts/check_version_identity.py --uplugin Plugins/SuperSLMUnreal/SuperSLMUnreal.uplugin --changelog CHANGELOG.md --vendored-dir Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM --allow-untagged
```

The plugin's own Python checks, `Plugins/SuperSLMUnreal/ci/tests`, are a pytest suite: install
pytest (`python -m pip install pytest`), then run `python -m pytest Plugins/SuperSLMUnreal/ci/tests`
from the repository root. Without a build, five of its checks fail and two skip (see
`SUPERSLM_BUILD_ROOT` above).

### Moving the SuperSLM pin

There is no script for this; the steps are these, in Git Bash, with `<tag>` the new release tag,
`<superslm>` a clone of the upstream SuperSLM repository that has fetched it, and `<dst>` this
repository's `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM`:

1. Replace the tree with the tag's raw blobs, every file except `.github/`. Read each file with
   `git cat-file blob`, never `git archive` or a checkout: those apply the upstream
   `.gitattributes` line-ending rules, and the tree must be byte-identical to the tag.

   ```
   tag=<tag>; src=<superslm>; dst=<dst>
   find "$dst" -type f ! -name VENDORED_VERSION.txt ! -name VENDORED_MANIFEST.sha256 -delete
   git -C "$src" ls-tree -r --full-tree "$tag" | while IFS=$'\t' read -r meta path; do
     set -- $meta
     [ "$2" = blob ] || continue
     case "$path" in .github/*) continue ;; esac
     mkdir -p "$dst/$(dirname "$path")"
     git -C "$src" cat-file blob "$3" > "$dst/$path"
   done
   git -C "$src" rev-parse "$tag^{commit}"
   ```

2. In `VENDORED_VERSION.txt`, set `Tag:` to the tag and `Commit:` to the commit the last command
   printed, and record the pin move.
3. Regenerate `VENDORED_MANIFEST.sha256` from the files just written, before anything checks them
   out again (a checkout converts their line endings and changes their hashes): one
   `<sha256>  <path>` line per file, two spaces, paths relative to `<dst>`, sorted bytewise, with
   `#` comment lines at the top.

   ```
   {
     echo "# SuperSLM $tag. SHA-256 of every vendored file, from the raw tag blobs."
     ( cd "$dst" && find . -type f ! -name VENDORED_VERSION.txt ! -name VENDORED_MANIFEST.sha256 \
         | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do
           printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
         done )
   } > /tmp/VENDORED_MANIFEST.sha256 && mv /tmp/VENDORED_MANIFEST.sha256 "$dst/"
   ```

4. If the new tag adds a library source, add its `SuperSLMVendored_*.cpp` wrapper;
   `check_vendored_wrappers_complete.py` names any that are missing.
5. Run the three coherence checks above and `Plugins/SuperSLMUnreal/ci/tests`, then the routine
   run, and add the pin move to `CHANGELOG.md`.

## CI

Every workflow is manual (`workflow_dispatch`); nothing runs on push or pull request. The
maintainer dispatches `coherence-checks` and, on a self-hosted Windows runner with Unreal Engine,
`plugin-build`, before a release. `release` cuts the tag. See
[.github/BRANCH_PROTECTION.md](.github/BRANCH_PROTECTION.md).

## Documentation rules

- Describe the plugin as it is. Mark what is planned as planned.
- A number in the docs is a measurement, or computed where marked, labelled with the machine and
  settings it came from; otherwise it is not there.
- Describe what the plugin does, not what a game should build with it.
- Where a fact cannot be checked, leave a visible TODO rather than a guess.
- A change that alters behaviour updates the affected documentation in the same pull request.

Source comments cite plan sections, tickets (`T-…`) and decisions (`D-…`) from the project's private
design history, which is not published. Some cite SuperFAISS as precedent: it is a sibling plugin
in that private history, and is not published here either.

## Versioning

The `.uplugin` `VersionName`, the top entry of `CHANGELOG.md` and any release tag must agree;
`check_version_identity.py` enforces it. Between releases, `VersionName` ends in `-dev` and the top
CHANGELOG entry is marked Unreleased. Release tags are cut by the `release` workflow, not by hand.

## License

This project is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)). Unless you state
otherwise, a contribution you submit is licensed under the same terms, as section 5 of the license
provides.

No sign-off (DCO) or separate contributor agreement is required.
