"""T-2226 -- L2-S0 red suite. §9 dim 4 (Shape and platform), the M5 cells the plan
names as L2-S0's own build-sequencing content (§10 L2-S0). This file checks that Layer 1 is
vendored at v1.9.0 (commit d870d27863) into the plugin's own ThirdParty tree and built from
source with the engine toolchain (D-SLM3812) on Windows x64, the only platform SuperSLM-Unreal
1.0 claims (D-SLM7226). The U1 re-pin is specified by
the plan §10.3.1.

REWRITTEN in the T-2226 fix round routed by confirmation code review T-2249
(the confirmation review, finding N3): the plan's
§9 dim 4(b)/(c) cells were retired and replaced under **D-SLM3931** on 2026-08-22, in this same
delta -- "the originals asserted properties of a CMake configure step this plugin never
performs." Grounded directly at the plan: **the plugin never
configures Layer 1's CMake at all** -- UBT compiles Layer 1's vendored `.cpp` files directly
into the module via `Private/Vendored/*.cpp`; there is no `PublicAdditionalLibraries`, no
prebuilt library, and no CMake invocation anywhere in the build path (verified at source, per
the plan's own text). The CMake>=3.20/`dxc.exe` Windows-configure guard this file's old (b)/(c)
cells asserted governs a configure step that does not exist for this plugin -- "the CMake >= 3.20
half has no successor at all: it is a property of a configure step this plugin never performs."

**The old (b) and (c) cells are DELETED** (HEAD holds only
current truth; a retired coverage model is not asserted alongside its replacement). **Replaced
with cells for the four module settings the plan's own §2.1/§9 dim 4(b) name as what the
Windows leg (and every leg) actually needs**, read from `SuperSLMUnreal.Build.cs` AND from the
real captured `.rsp` compile line, never from one alone: `CppStandard` C++20,
`bEnableExceptions` true, `FPSemantics = Precise`, `bUseUnity` false. The plan singles out
`FPSemantics` by name: *"the other three fail loudly at compile time, and that one fails
silently at compile time and loudly at the golden hash, which is why §9 dim 4(b) makes it a
cell."* Its cell (test_g) therefore carries a must-reject mutation proof -- a planted
`/fp:fast` -- that the other three settings do not need, because a wrong value for any of the
other three would already fail to compile.

Cell lettering below follows the plan's own dim 4 sub-lettering where it still applies:

(a) The vendored tree exists, at the pinned tag+commit, recorded by the plugin's own build
    files (unchanged from the prior round).
(d) The generality claim: no prebuilt Layer-1 binary is referenced (unchanged).
(e)-(h), new this round: the four module settings, each read from Build.cs AND the real .rsp.
"""
import os
import re
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parents[1].parent  # .../Plugins/SuperSLMUnreal
THIRDPARTY_ROOT = PLUGIN_ROOT / "Source" / "ThirdParty" / "SuperSLM"

EXPECTED_TAG = "v1.9.0"
EXPECTED_COMMIT_PREFIX = "d870d27863"
EXPECTED_FILE_COUNT = 892  # upstream files excluding .github/ and vendoring metadata

_RUNTIME_BUILD_CS_NAME = "SuperSLMUnreal.Build.cs"

# Candidate roots for the module's real captured compile line -- same search order and same
# reasoning as test_guard_g5_status_switch_exhaustive.py's _find_intermediate_root: this
# checkout's own Intermediate/ when a build has run here, and the built checkout
# SUPERSLM_BUILD_ROOT names as the fallback.
_INTERMEDIATE_ROOTS = [
    PLUGIN_ROOT / "Intermediate" / "Build" / "Win64" / "x64" / "UnrealEditor" / "Development" / "SuperSLMUnreal",
    # The fallback: a checkout that has built the plugin, named by SUPERSLM_BUILD_ROOT (the repository
    # root that holds Plugins/). Unset, there is no fallback, and in a worktree with no .rsp the
    # cells that read it (e, f, g, g2, h) FAIL -- they assert the root was found; they do not skip.
    *([Path(os.environ["SUPERSLM_BUILD_ROOT"]) / "Plugins" / "SuperSLMUnreal" / "Intermediate" / "Build" / "Win64" / "x64"
       / "UnrealEditor" / "Development" / "SuperSLMUnreal"] if os.environ.get("SUPERSLM_BUILD_ROOT") else []),
]

# A prebuilt Layer-1 binary reference: a quoted string ending in a native static/shared-lib
# extension that also names "superslm" -- narrow enough not to false-positive on an unrelated
# library, matching this cell's actual claim (D-SLM3812: "the plugin never consumes a prebuilt
# Layer-1 binary"), not "the file mentions .lib at all". Build.cs string literals do not span
# physical lines, so excluding newlines from the middle class is unnecessary; "." is escaped so
# it means a literal dot, not "any character".
_PREBUILT_BINARY_PATTERN = re.compile(r'superslm[^"]*\.(lib|a|dylib)"', re.IGNORECASE)


def _find_intermediate_root():
    for root in _INTERMEDIATE_ROOTS:
        if (root / "SuperSLMUnreal.Shared.rsp").is_file():
            return root
    return None


def _find_runtime_build_cs():
    source_dir = PLUGIN_ROOT / "Source"
    if not source_dir.is_dir():
        return None
    for p in source_dir.rglob("*.Build.cs"):
        if p.name == _RUNTIME_BUILD_CS_NAME:
            return p
    return None


def test_a_vendored_tree_exists_and_pinned():
    assert THIRDPARTY_ROOT.is_dir(), (
        f"Source/ThirdParty/SuperSLM/ does not exist yet at {THIRDPARTY_ROOT} -- "
        "L2-S0's own build-sequencing deliverable (D-SLM3812)"
    )
    version_file = THIRDPARTY_ROOT / "VENDORED_VERSION.txt"
    assert version_file.is_file(), (
        f"{version_file} does not exist -- the vendored tree must record its own tag+commit "
        "by name: a Tag: line and a Commit: line, written when the tree is vendored from the "
        "pinned upstream SuperSLM tag (see CONTRIBUTING.md)"
    )
    text = version_file.read_text(encoding="utf-8")
    assert re.search(rf"(?m)^Tag:[ \t]*{re.escape(EXPECTED_TAG)}[ \t]*$", text), (
        f"VENDORED_VERSION.txt must record tag {EXPECTED_TAG} on its Tag line; got: {text!r}"
    )
    assert re.search(
        rf"(?m)^Commit:[ \t]*{re.escape(EXPECTED_COMMIT_PREFIX)}[0-9a-f]{{30}}[ \t]*$",
        text,
    ), (
        f"VENDORED_VERSION.txt must record commit prefix {EXPECTED_COMMIT_PREFIX} "
        f"on its Commit line; got: {text!r}"
    )
    vendored_files = [
        path for path in THIRDPARTY_ROOT.rglob("*")
        if path.is_file()
        and ".github" not in path.relative_to(THIRDPARTY_ROOT).parts
        and path.relative_to(THIRDPARTY_ROOT).as_posix()
        not in {"VENDORED_VERSION.txt", "VENDORED_MANIFEST.sha256"}
    ]
    assert len(vendored_files) == EXPECTED_FILE_COUNT, (
        f"vendored tag {EXPECTED_TAG} must contain {EXPECTED_FILE_COUNT} upstream files "
        f"excluding .github/ and vendoring metadata; found {len(vendored_files)}"
    )
    cmakelists = THIRDPARTY_ROOT / "CMakeLists.txt"
    assert cmakelists.is_file(), (
        f"{cmakelists} does not exist -- the vendored tree must carry Layer 1's own build "
        "recipe (source dependency, not a prebuilt binary, D-SLM3812)"
    )


def test_d_no_prebuilt_layer1_binary_referenced():
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, (
        f"Source/SuperSLMUnreal/{_RUNTIME_BUILD_CS_NAME} does not exist yet -- the runtime "
        "module L2-S0 delivers"
    )
    text = build_cs.read_text(encoding="utf-8")
    match = _PREBUILT_BINARY_PATTERN.search(text)
    assert match is None, (
        f"{build_cs} references a prebuilt Layer-1 binary ({match.group(0)!r}) -- D-SLM3812 "
        "rules the plugin links Layer 1's C++ headers and builds it from source; a prebuilt "
        "binary reference here is exactly the generality claim §9 dim 4(d) says is NOT made"
    )


def test_d2_prebuilt_binary_reference_would_be_caught():
    # Must-reject: the mutation the plan's own gate line implies
    # ("the plugin never consumes a prebuilt Layer-1 binary") is a planted
    # PublicAdditionalLibraries reference to a prebuilt superslm.lib. Run against a scratch
    # copy of the REAL Build.cs with that one line prepended to its dependency block -- never
    # against a hand-written stand-in -- so this proves the pattern fires on the actual file's
    # real shape, not on a fixture built to be easy to match.
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, (
        f"Source/SuperSLMUnreal/{_RUNTIME_BUILD_CS_NAME} does not exist yet -- cannot mutate "
        "a file that is not there. This is the same red reason test_d_no_prebuilt_layer1_"
        "binary_referenced reports; it is not a second, independent gap"
    )
    real_text = build_cs.read_text(encoding="utf-8")
    anchor = "PublicDependencyModuleNames.AddRange(new string[]"
    assert anchor in real_text, (
        f"mutation anchor {anchor!r} not found in the real Build.cs -- update the anchor to "
        "match the current file before trusting this test's result"
    )
    planted_line = (
        'PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", '
        '"SuperSLM", "lib", "superslm.lib"));\n\n\t\t'
    )
    mutated_text = real_text.replace(anchor, planted_line + anchor, 1)
    assert mutated_text != real_text, "mutation did not apply"

    match_on_real = _PREBUILT_BINARY_PATTERN.search(real_text)
    assert match_on_real is None, (
        "the REAL file already matches the prebuilt-binary pattern before any mutation -- "
        "test_d_no_prebuilt_layer1_binary_referenced should already be failing; this test's "
        "own premise (real=clean, mutated=dirty) does not hold"
    )
    match_on_mutated = _PREBUILT_BINARY_PATTERN.search(mutated_text)
    assert match_on_mutated is not None, (
        "planted a prebuilt-binary reference into a scratch copy of the real Build.cs and the "
        "checker did not catch it -- the checker cannot discriminate"
    )
    assert "superslm.lib" in match_on_mutated.group(0).lower(), (
        f"checker matched something, but not the planted string -- got {match_on_mutated.group(0)!r}"
    )


def test_e_cppstandard_is_cpp20():
    """D-SLM3931 replacement cell (e): CppStandard = CppStandardVersion.Cpp20, read from
    BOTH Build.cs and the real per-file .rsp (a wrong Build.cs value that somehow still
    produced a stale/cached correct .rsp would not be caught by either source alone)."""
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, f"{_RUNTIME_BUILD_CS_NAME} does not exist yet"
    build_cs_text = build_cs.read_text(encoding="utf-8")
    assert "CppStandard = CppStandardVersion.Cpp20;" in build_cs_text, (
        "Build.cs must set CppStandard = CppStandardVersion.Cpp20 -- Layer 1 requires C++20 "
        "(plan §2.1, D-SLM3812 rider 5), and an engine default change must not silently drop "
        "below it"
    )

    intermediate_root = _find_intermediate_root()
    assert intermediate_root is not None, (
        "no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set) -- cannot "
        "confirm CppStandard reached the real compile line; Build.cs alone is not this "
        "cell's whole claim"
    )
    per_file = sorted(intermediate_root.glob("*.cpp.obj.rsp"))
    assert per_file, f"no per-file .obj.rsp found under {intermediate_root}"
    found = any("/std:c++20" in p.read_text(encoding="utf-8", errors="replace") for p in per_file)
    assert found, "no per-file .rsp under the captured build carries /std:c++20"


def test_f_exceptions_enabled():
    """D-SLM3931 replacement cell (f): bEnableExceptions = true, read from Build.cs AND the
    module-wide Shared.rsp (/EHsc)."""
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, f"{_RUNTIME_BUILD_CS_NAME} does not exist yet"
    build_cs_text = build_cs.read_text(encoding="utf-8")
    assert "bEnableExceptions = true;" in build_cs_text, (
        "Build.cs must set bEnableExceptions = true -- Layer 1's public C++ throw contract "
        "(src/bad_alloc_wrap.h) uses ordinary try/catch/throw unconditionally, and UE modules "
        "default to exceptions disabled"
    )

    intermediate_root = _find_intermediate_root()
    assert intermediate_root is not None, (
        "no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set)"
    )
    shared_text = (intermediate_root / "SuperSLMUnreal.Shared.rsp").read_text(
        encoding="utf-8", errors="replace")
    assert re.search(r"/EHsc\b", shared_text), (
        "the module's real captured compile line does not carry /EHsc"
    )


def test_g_fp_semantics_is_precise():
    """D-SLM3931 replacement cell (g), THE ONE THE PLAN SINGLES OUT: FPSemantics =
    FPSemanticsMode.Precise, read from Build.cs AND the real Shared.rsp (/fp:precise present,
    /fp:fast absent). "The other three fail loudly at compile time, and that one fails
    silently at compile time and loudly at the golden hash" -- so this is the one cell in the
    replacement set that needs a must-reject mutation proof; a wrong value for the other three
    settings is already unconstructible without a compile failure of its own."""
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, f"{_RUNTIME_BUILD_CS_NAME} does not exist yet"
    build_cs_text = build_cs.read_text(encoding="utf-8")
    assert "FPSemantics = FPSemanticsMode.Precise;" in build_cs_text, (
        "Build.cs must set FPSemantics = FPSemanticsMode.Precise -- Layer 1's scalar/SIMD-tier "
        "bit-equality depends on exact mul-then-add rounding; source pragmas do not stop "
        "backend fusion under fast-math (D-SLM3833 / guard G4's own FP-rounding half)"
    )

    intermediate_root = _find_intermediate_root()
    assert intermediate_root is not None, (
        "no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set)"
    )
    shared_text = (intermediate_root / "SuperSLMUnreal.Shared.rsp").read_text(
        encoding="utf-8", errors="replace")
    assert re.search(r"/fp:precise\b", shared_text), (
        "the module's real captured compile line does not carry /fp:precise"
    )
    assert not re.search(r"/fp:fast\b", shared_text), (
        "the module's real captured compile line carries /fp:fast -- this silently breaks "
        "Layer 1's cross-tier bit-equality without a compile error (the exact failure mode "
        "this cell exists to catch)"
    )


def test_g2_fp_fast_would_be_caught():
    """Must-reject, required specifically for FPSemantics because
    it is the one setting among the four that "fails silently at compile time" -- a planted
    /fp:fast in a scratch copy of the real Shared.rsp must trip the same assertion
    test_g_fp_semantics_is_precise makes, proving the check can actually fail and is not
    checking a string that happens to always be true."""
    intermediate_root = _find_intermediate_root()
    assert intermediate_root is not None, (
        "no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set) -- cannot "
        "construct the mutation without a real file to mutate a copy of"
    )
    real_text = (intermediate_root / "SuperSLMUnreal.Shared.rsp").read_text(
        encoding="utf-8", errors="replace")
    assert "/fp:precise" in real_text, (
        "the real .rsp does not carry /fp:precise -- test_g's own premise does not hold, "
        "fix that cell first"
    )
    mutated_text = real_text.replace("/fp:precise", "/fp:fast", 1)
    assert mutated_text != real_text, "mutation did not apply"
    assert not re.search(r"/fp:fast\b", real_text), (
        "the REAL .rsp already carries /fp:fast before any mutation -- this test's own "
        "premise (real=clean, mutated=dirty) does not hold"
    )
    assert re.search(r"/fp:fast\b", mutated_text), (
        "planted /fp:fast into a scratch copy of the real .rsp and the pattern this file's "
        "own test_g checks against did not detect it"
    )


def test_h_unity_disabled():
    """D-SLM3931 replacement cell (h): bUseUnity = false, read from Build.cs, corroborated
    structurally against the real captured build: a per-file .obj.rsp exists for more than
    one distinct real source file (SuperSLMModel.cpp, SuperSLMModelImport.cpp, etc.) rather
    than one aggregated unity translation unit compiling several sources together -- the
    structural signature unity=false leaves in a captured build."""
    build_cs = _find_runtime_build_cs()
    assert build_cs is not None, f"{_RUNTIME_BUILD_CS_NAME} does not exist yet"
    build_cs_text = build_cs.read_text(encoding="utf-8")
    assert "bUseUnity = false;" in build_cs_text, (
        "Build.cs must set bUseUnity = false -- unity would merge Layer 1's vendored "
        "translation units with the module's own"
    )

    intermediate_root = _find_intermediate_root()
    assert intermediate_root is not None, (
        "no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set)"
    )
    per_file = sorted(intermediate_root.glob("*.cpp.obj.rsp"))
    distinct_real_sources = {p.name for p in per_file if not p.name.lower().startswith("module.")}
    assert len(distinct_real_sources) > 1, (
        f"expected more than one distinct per-file .cpp.obj.rsp under {intermediate_root} "
        f"(the non-unity signature); found {sorted(distinct_real_sources)!r} -- a unity build "
        "would aggregate multiple real sources into one Module.<Name>.cpp translation unit "
        "instead"
    )


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print(f"{name}: PASS (unexpected before L2-S0 lands)")
            except AssertionError as e:
                print(f"{name}: RED -- {e}")
