#!/usr/bin/env python3
"""T-2241 review S6: "Seventeen wrappers, seventeen sources, and nothing joining them ... A
pin that adds an eighteenth source drops it from the module with nothing to say so."

Compares Source/ThirdParty/SuperSLM/CMakeLists.txt's SUPERSLM_CORE_SOURCES list (the sources
Layer 1's own `superslm` STATIC target compiles -- the CPU-only core) plus the
`add_library(superslm_gpu STATIC ...)` source list (the two src/gpu/*.cpp files, L2-S2) against
the one-wrapper-per-source files under
Source/SuperSLMUnreal/Private/Vendored/SuperSLMVendored_<name>.cpp, and fails if either side
has an entry the other does not.

Usage:
    python check_vendored_wrappers_complete.py --plugin-root <SuperSLMUnreal plugin dir>

Exit 0   -- every SUPERSLM_CORE_SOURCES and superslm_gpu source has exactly one wrapper, and no
            wrapper wraps a source outside those two lists.
Exit !=0 -- names every source missing a wrapper and every wrapper with no corresponding
            source.
"""
import argparse
import re
import sys
from pathlib import Path

CORE_SOURCES_BLOCK_RE = re.compile(
    r"set\(SUPERSLM_CORE_SOURCES\s*(.*?)\)", re.DOTALL
)
# L2-S2 (plan §10.3 item 3, D-SLM7337): Layer 1's GPU library, `add_library(superslm_gpu STATIC
# src/gpu/gpu_1p0.cpp src/gpu/superslm_gpu.cpp)`, is also compiled into the runtime module (on
# Win64, each wrapper guarded by SUPERSLMUNREAL_WITH_GPU), so its source list is part of the set
# every wrapper must map onto and every source must be wrapped by.
GPU_SOURCES_BLOCK_RE = re.compile(
    r"add_library\(superslm_gpu\s+STATIC\s*(.*?)\)", re.DOTALL
)
SOURCE_LINE_RE = re.compile(r"^\s*(\S+\.cpp)\s*$", re.MULTILINE)
WRAPPER_INCLUDE_RE = re.compile(r'#include\s+"([^"]+\.cpp)"')


def _core_sources(cmakelists_text: str):
    match = CORE_SOURCES_BLOCK_RE.search(cmakelists_text)
    if not match:
        return None
    return {m.group(1) for m in SOURCE_LINE_RE.finditer(match.group(1))}


def _gpu_sources(cmakelists_text: str):
    match = GPU_SOURCES_BLOCK_RE.search(cmakelists_text)
    if not match:
        return None
    return {m.group(1) for m in SOURCE_LINE_RE.finditer(match.group(1))}


def _wrapper_targets(vendored_dir: Path):
    """Maps each wrapper file to the Layer-1 source path (relative to ThirdParty/SuperSLM/)
    it #includes, e.g. Private/Vendored/SuperSLMVendored_artifact.cpp ->
    "../../../ThirdParty/SuperSLM/src/artifact.cpp" -> normalized to "src/artifact.cpp"."""
    targets = {}
    for wrapper in sorted(vendored_dir.glob("SuperSLMVendored_*.cpp")):
        text = wrapper.read_text(encoding="utf-8", errors="replace")
        includes = WRAPPER_INCLUDE_RE.findall(text)
        # Each wrapper is documented to #include exactly one Layer-1 .cpp (T-2226/T-2227's own
        # one-TU-per-source discipline, to avoid anonymous-namespace collisions) -- a wrapper
        # with zero or more than one real include is itself a defect this check surfaces.
        real_includes = [inc for inc in includes if "ThirdParty/SuperSLM/" in inc]
        if len(real_includes) != 1:
            targets[wrapper.name] = None
            continue
        inc = real_includes[0]
        marker = "ThirdParty/SuperSLM/"
        rel = inc[inc.index(marker) + len(marker):]
        targets[wrapper.name] = rel
    return targets


def check(plugin_root: Path):
    cmakelists = plugin_root / "Source" / "ThirdParty" / "SuperSLM" / "CMakeLists.txt"
    vendored_dir = plugin_root / "Source" / "SuperSLMUnreal" / "Private" / "Vendored"

    if not cmakelists.is_file():
        return [f"{cmakelists} does not exist -- cannot enumerate SUPERSLM_CORE_SOURCES"]
    if not vendored_dir.is_dir():
        return [f"{vendored_dir} does not exist -- no wrappers to check"]

    cmake_text = cmakelists.read_text(encoding="utf-8")
    core_sources = _core_sources(cmake_text)
    if core_sources is None:
        return [f"{cmakelists}: could not find a SUPERSLM_CORE_SOURCES set(...) block -- "
                f"the vendored CMakeLists.txt has changed shape since this checker was written"]
    gpu_sources = _gpu_sources(cmake_text)
    if not gpu_sources:
        return [f"{cmakelists}: could not find the add_library(superslm_gpu STATIC ...) source "
                f"list -- the vendored CMakeLists.txt has changed shape since this checker was written"]
    core_sources = core_sources | gpu_sources

    wrapper_targets = _wrapper_targets(vendored_dir)

    problems = []
    for wrapper_name, target in wrapper_targets.items():
        if target is None:
            problems.append(f"{wrapper_name}: does not #include exactly one ThirdParty/SuperSLM/*.cpp path")

    wrapped_sources = {t for t in wrapper_targets.values() if t is not None}

    missing_wrappers = sorted(core_sources - wrapped_sources)
    for src in missing_wrappers:
        problems.append(f"MISSING WRAPPER: {src} is in SUPERSLM_CORE_SOURCES or superslm_gpu but no Private/Vendored/*.cpp wraps it")

    extra_wrappers = sorted(wrapped_sources - core_sources)
    for src in extra_wrappers:
        problems.append(f"STALE WRAPPER: a wrapper includes {src}, which is not (or no longer) in SUPERSLM_CORE_SOURCES or superslm_gpu")

    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin-root", required=True, type=Path)
    args = parser.parse_args()

    if not args.plugin_root.is_dir():
        print(f"error: --plugin-root {args.plugin_root} is not a directory", file=sys.stderr)
        return 2

    problems = check(args.plugin_root)
    if not problems:
        print("OK: every SUPERSLM_CORE_SOURCES and superslm_gpu source has exactly one wrapper, and no stale wrappers")
        return 0

    for p in problems:
        print(p)
    return 1


if __name__ == "__main__":
    sys.exit(main())
