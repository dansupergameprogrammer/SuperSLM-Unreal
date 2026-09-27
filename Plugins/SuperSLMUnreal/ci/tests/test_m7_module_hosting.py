"""T-2448 -- pins for the T-2447 M7 hosting remedies.

The T-2258 amendment's M7 mechanism (the plan §9's M7 re-cross,
RULING D-SLM5340/5341/5342) added a third plugin module, `SuperSLMUnrealTests`, to host the
`ClientContext` memory-mapping cells a packaged `TargetType.Game` executable must run. T-2447
built it. The coverage audit record
named the re-cross's dimensions 3, 4 and 8 BARE; this file is dimensions 4, 8 and 3, plus the
Source-tree-read requirement D-SLM5342 states as a requirement of the module's contents.

Each cell is a pure predicate over file TEXT, applied twice per the commissioning rule
(a check is trusted only once shown to accept a good input and reject a bad one): once to the real committed file (must-accept) and once to a
scratch copy of that SAME real file with the pin's stated mutation planted (must-reject), so
the mutation proof is re-executed on every run rather than being a claim in a record about a
hand-edit somebody once did. This is `test_thirdparty_vendoring.py`'s `test_d`/`test_d2`
shape.

Pins and their mutations:

- **Dim 4 -- a packaged Shipping build contains no `SuperSLMUnrealTests` binary.**
  Mutation: flip the module's `"Type"` to `"Runtime"` in `SuperSLMUnreal.uplugin`.
  A packaged Shipping client cannot be built in this project's CI, so the property is pinned
  at the coordinate that DECIDES it: UBT's own module-type rule. `ModuleDescriptor.cs:785-787`
  admits a `Runtime` module to every target except `Program`; `:796-797` admits a
  `DeveloperTool` module only when `bBuildDeveloperTools` holds, whose default
  (`TargetRules.cs:1215`) is false for a `TargetType.Game` target in `Shipping` or `Test`.
  The second half of the same property, which the `"Type"` string alone does not carry: no
  other module in this plugin may DEPEND on `SuperSLMUnrealTests`, since a dependency edge
  from a module that IS in Shipping would drag it in regardless of its own type.

- **Dim 8 -- the module's dependency set contains no editor-only module.**
  Mutation: add `UnrealEd` to `SuperSLMUnrealTests.Build.cs`.

- **Dim 3 -- the allocator proxy's own oracle cell runs in packaged Development as well as in
  the editor.** Mutation: remove `ClientContext` from
  `FSuperSLMAllocationScopePeakNetOracleTest`'s flags.

- **D-SLM5342 -- no cell hosted in this module reads a file from the plugin's `Source/` tree.**
  Mutation: add `#include "Fixtures/SuperSLMImportFixtures.h"` to a file in the module.
  Staging copies a plugin's `Content`, `Config` and `Binaries` and never its `Source/`, so
  such a read returns zero bytes in a packaged client -- the exact defect D-SLM5342 repaired
  in MM-1, where `HasAllocationAtLeast(0)` answers `MaxObservedSingleAllocation >= 0`,
  unconditionally true.
"""
import json
import re
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parents[2]
UPLUGIN = PLUGIN_ROOT / "SuperSLMUnreal.uplugin"
SOURCE_ROOT = PLUGIN_ROOT / "Source"
TESTS_MODULE_ROOT = SOURCE_ROOT / "SuperSLMUnrealTests"
TESTS_BUILD_CS = TESTS_MODULE_ROOT / "SuperSLMUnrealTests.Build.cs"
PACKAGED_CELLS_CPP = TESTS_MODULE_ROOT / "Private" / "Tests" / "SuperSLMPackagedMemoryMappingTests.cpp"

TESTS_MODULE_NAME = "SuperSLMUnrealTests"

# UBT module types that can be compiled into a packaged Shipping TargetType.Game client. Read
# from the engine's own rule rather than named by convention: ModuleDescriptor.IsCompiledInConfiguration
# (Programs/UnrealBuildTool/Configuration/Descriptors/ModuleDescriptor.cs) admits Runtime and its
# variants to every non-Program target in every configuration; DeveloperTool is gated on
# bBuildDeveloperTools (false for a Game target in Shipping or Test, TargetRules.cs:1215); every
# Editor* and UncookedOnly type is gated on TargetType == Editor.
MODULE_TYPES_PRESENT_IN_SHIPPING_GAME = {
    "Runtime",
    "RuntimeNoCommandlet",
    "RuntimeAndProgram",
    "ClientOnly",
    "ClientOnlyNoCommandlet",
    "ServerOnly",
    "CookedOnly",
}

# The dependency set RULING D-SLM5340's own text states for this module, and the set the
# T-2447 build log records as built. A future dependency added here is not automatically
# wrong -- it is an event that must be looked at, which is what test_e asserts.
RULED_DEPENDENCY_SET = {"Core", "CoreUObject", "Engine", "SuperSLMUnreal"}

# Editor-only engine modules, named so the failure message can say WHY an addition is a defect
# rather than only that the set changed. Not exhaustive by construction -- no denylist can be --
# which is why test_e's allow-list equality is the load-bearing half and this is the diagnostic
# half. UnrealEd and AssetTools are the two the plan's own M7 re-cross dimension 8 names.
EDITOR_ONLY_MODULE_NAMES = {
    "UnrealEd",
    "AssetTools",
    "EditorFramework",
    "EditorStyle",
    "EditorSubsystem",
    "EditorWidgets",
    "LevelEditor",
    "ContentBrowser",
    "ContentBrowserData",
    "BlueprintGraph",
    "Kismet",
    "KismetCompiler",
    "ToolMenus",
    "MainFrame",
    "MaterialEditor",
}

# A dependency-array element in a .Build.cs: a quoted module name inside a
# Public/PrivateDependencyModuleNames (or *IncludePathModuleNames) AddRange/Add call. A quoted
# string ANYWHERE in the file is deliberately not matched -- the editor module's own comments
# name SuperSLMUnrealTests in prose, and three editor cells use "/Temp/SuperSLMUnrealTests/..."
# as a transient PACKAGE path, neither of which is a dependency edge.
_DEPENDENCY_BLOCK_PATTERN = re.compile(
    r"(?:Public|Private)(?:Dependency|IncludePath)ModuleNames\s*\.\s*"
    r"(?:AddRange\s*\(\s*new\s+string\s*\[\s*\]\s*\{(?P<range>[^}]*)\}|Add\s*\(\s*\"(?P<one>\w+)\"\s*\))",
    re.DOTALL,
)

_QUOTED_NAME_PATTERN = re.compile(r'"(\w+)"')

_AUTOMATION_TEST_PATTERN = re.compile(
    r"IMPLEMENT_SIMPLE_AUTOMATION_TEST\(\s*(?P<klass>\w+)\s*,\s*"
    r'"(?P<pretty>[^"]*)"\s*,\s*(?P<flags>[^)]*)\)',
    re.DOTALL,
)

# Any of these appearing in a file hosted by SuperSLMUnrealTests means that file reaches for
# something that lives only under the plugin's Source/ tree, which staging never copies.
_SOURCE_TREE_READ_MARKERS = (
    "Fixtures/SuperSLMImportFixtures.h",
    "SuperSLMImportFixtures",
    "FFileHelper::LoadFileTo",
    "FPaths::ProjectPluginsDir",
)


# --------------------------------------------------------------------------------------- #
# Pure predicates. Every cell below drives these; every must-reject drives the SAME function
# against a scratch copy of the real file, so what is proven able to fail is the checker the
# must-accept ran, not a paraphrase of it.
# --------------------------------------------------------------------------------------- #

def _module_types(uplugin_text: str) -> dict[str, str]:
    """{module name: declared "Type"} for every entry in the .uplugin's Modules list."""
    descriptor = json.loads(uplugin_text)
    return {m["Name"]: m["Type"] for m in descriptor.get("Modules", [])}


def _modules_present_in_a_shipping_game_client(uplugin_text: str) -> set[str]:
    return {
        name
        for name, module_type in _module_types(uplugin_text).items()
        if module_type in MODULE_TYPES_PRESENT_IN_SHIPPING_GAME
    }


def _dependency_module_names(build_cs_text: str) -> set[str]:
    names: set[str] = set()
    for match in _DEPENDENCY_BLOCK_PATTERN.finditer(build_cs_text):
        if match.group("one"):
            names.add(match.group("one"))
        else:
            block = match.group("range")
            # Strip // comments so a module name mentioned in a comment inside the array is
            # not read as an edge.
            block = re.sub(r"//[^\n]*", "", block)
            names.update(_QUOTED_NAME_PATTERN.findall(block))
    return names


def _automation_test_flags(cpp_text: str, class_name: str) -> set[str]:
    """The EAutomationTestFlags:: names on one IMPLEMENT_SIMPLE_AUTOMATION_TEST declaration."""
    for match in _AUTOMATION_TEST_PATTERN.finditer(cpp_text):
        if match.group("klass") == class_name:
            return set(re.findall(r"EAutomationTestFlags::(\w+)", match.group("flags")))
    return set()


def _strip_cpp_comments(text: str) -> str:
    """Comments out, code kept. The instrument header NAMES the fixtures header in prose --
    "matching SuperSLMImportFixtures.h's own test-only convention" -- and a checker that reads
    prose as a dependency would fire on a sentence rather than on a read. `#include` lines are
    not comments and survive, which is what the marker set is actually looking for.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def _source_tree_reads(module_root: Path, overrides: dict[Path, str] | None = None) -> dict[str, list[str]]:
    """{relative file path: markers found} across every .cpp/.h the module compiles.

    `overrides` substitutes text for one path without touching the tree, which is how the
    must-reject plants its mutation into a scratch copy of a real file.
    """
    overrides = overrides or {}
    found: dict[str, list[str]] = {}
    for path in sorted(module_root.rglob("*")):
        if path.suffix.lower() not in (".cpp", ".h"):
            continue
        text = _strip_cpp_comments(overrides.get(path, path.read_text(encoding="utf-8")))
        hits = [marker for marker in _SOURCE_TREE_READ_MARKERS if marker in text]
        if hits:
            found[str(path.relative_to(module_root)).replace("\\", "/")] = hits
    return found


# --------------------------------------------------------------------------------------- #
# Dim 4 -- Shipping absence
# --------------------------------------------------------------------------------------- #

def test_a_tests_module_type_keeps_it_out_of_a_packaged_shipping_client():
    """Must-accept. The module is declared with a type UBT cannot compile into a packaged
    Shipping TargetType.Game client, and specifically the type RULING D-SLM5340 names.
    """
    assert UPLUGIN.is_file(), f"plugin descriptor not found: {UPLUGIN}"
    text = UPLUGIN.read_text(encoding="utf-8")

    types = _module_types(text)
    assert TESTS_MODULE_NAME in types, (
        f"{UPLUGIN} declares no {TESTS_MODULE_NAME} module -- the M7 mechanism "
        "(RULING D-SLM5340) is not built, so its five ClientContext cells have no host a "
        "packaged TargetType.Game executable loads"
    )
    assert types[TESTS_MODULE_NAME] == "DeveloperTool", (
        f'{TESTS_MODULE_NAME} is declared "Type": "{types[TESTS_MODULE_NAME]}" -- '
        "RULING D-SLM5340 names DeveloperTool, whose bBuildDeveloperTools gate is false for a "
        "Game target in Shipping or Test (TargetRules.cs:1215), the same extent as "
        "WITH_DEV_AUTOMATION_TESTS"
    )
    assert TESTS_MODULE_NAME not in _modules_present_in_a_shipping_game_client(text)


def test_b_must_reject_a_runtime_type_would_reach_a_shipping_client():
    """Must-reject for test_a, with the mutation stated by the plan's own dim 11: flip the
    module's "Type" to "Runtime". Applied to a scratch copy of the REAL descriptor, never to a
    hand-written stand-in, so what is proven is that this checker fires on the real file's
    actual shape.
    """
    real_text = UPLUGIN.read_text(encoding="utf-8")
    assert TESTS_MODULE_NAME not in _modules_present_in_a_shipping_game_client(real_text), (
        "the real descriptor already places the tests module in a Shipping client -- test_a "
        "should already be failing; this test's premise (real=clean, mutated=dirty) does not hold"
    )

    anchor = '"Name": "SuperSLMUnrealTests",\n\t\t\t"Type": "DeveloperTool"'
    assert anchor in real_text, (
        f"mutation anchor not found in {UPLUGIN} -- update the anchor to match the current "
        "descriptor before trusting this test's result"
    )
    mutated_text = real_text.replace(
        anchor, '"Name": "SuperSLMUnrealTests",\n\t\t\t"Type": "Runtime"', 1
    )
    assert mutated_text != real_text, "mutation did not apply"

    assert TESTS_MODULE_NAME in _modules_present_in_a_shipping_game_client(mutated_text), (
        'flipped "Type" to "Runtime" in a scratch copy of the real descriptor and the checker '
        "did not catch it -- the checker cannot discriminate the mutation dim 11 names"
    )


def test_c_no_shipping_bound_module_depends_on_the_tests_module():
    """Must-accept, second half of dim 4. The "Type" string keeps the module out of a Shipping
    client only while nothing that IS in that client depends on it: UBT resolves a dependency
    edge regardless of the dependee's own type, so an edge from the runtime module would drag
    the test binary in and the "Type" check above would still be green.
    """
    offenders: dict[str, set[str]] = {}
    for build_cs in sorted(SOURCE_ROOT.glob("*/*.Build.cs")):
        if build_cs.parent.name == TESTS_MODULE_NAME:
            continue
        deps = _dependency_module_names(build_cs.read_text(encoding="utf-8"))
        if TESTS_MODULE_NAME in deps:
            offenders[build_cs.name] = deps
    assert not offenders, (
        f"{sorted(offenders)} declare a dependency on {TESTS_MODULE_NAME} -- a module that "
        "ships would pull the DeveloperTool test binary into a packaged Shipping client "
        "through the dependency edge, which the module's own \"Type\" does not prevent"
    )


def test_d_must_reject_a_dependency_edge_from_the_runtime_module_is_caught():
    """Must-reject for test_c: plant the edge into a scratch copy of the REAL runtime Build.cs."""
    runtime_build_cs = SOURCE_ROOT / "SuperSLMUnreal" / "SuperSLMUnreal.Build.cs"
    assert runtime_build_cs.is_file(), f"runtime Build.cs not found: {runtime_build_cs}"
    real_text = runtime_build_cs.read_text(encoding="utf-8")
    assert TESTS_MODULE_NAME not in _dependency_module_names(real_text), (
        "the real runtime Build.cs already depends on the tests module -- test_c should "
        "already be failing"
    )

    anchor = "PublicDependencyModuleNames.AddRange(new string[]"
    assert anchor in real_text, (
        f"mutation anchor {anchor!r} not found in {runtime_build_cs} -- update the anchor "
        "before trusting this test's result"
    )
    mutated_text = real_text.replace(
        anchor, f'{anchor}\n\t\t{{\n\t\t\t"{TESTS_MODULE_NAME}",\n\t\t}});\n\t\t{anchor}', 1
    )
    assert TESTS_MODULE_NAME in _dependency_module_names(mutated_text), (
        "planted a dependency edge on the tests module into a scratch copy of the real "
        "runtime Build.cs and the checker did not catch it"
    )


# --------------------------------------------------------------------------------------- #
# Dim 8 -- composition
# --------------------------------------------------------------------------------------- #

def test_e_tests_module_dependency_set_is_exactly_the_ruled_set():
    """Must-accept, the load-bearing half of dim 8. Asserted as SET EQUALITY against the four
    names RULING D-SLM5340 states, not as absence from a denylist: no denylist can enumerate
    every editor-only engine module, so a denylist-only cell is green against the editor module
    nobody thought to list. An addition here is not automatically wrong -- it is the event that
    has to be looked at, and this cell is what forces the look.
    """
    assert TESTS_BUILD_CS.is_file(), f"{TESTS_BUILD_CS} does not exist -- the M7 module is unbuilt"
    deps = _dependency_module_names(TESTS_BUILD_CS.read_text(encoding="utf-8"))
    assert deps == RULED_DEPENDENCY_SET, (
        f"{TESTS_BUILD_CS.name}'s dependency set is {sorted(deps)}, and RULING D-SLM5340 "
        f"states {sorted(RULED_DEPENDENCY_SET)}. If a dependency was added deliberately, "
        "confirm it is compiled into a packaged TargetType.Game client (an editor-only module "
        "compiles in the editor and fails the packaged link) and then update "
        "RULED_DEPENDENCY_SET here with the reason"
    )


def test_f_tests_module_names_no_editor_only_module():
    """Must-accept, the diagnostic half of dim 8: the plan's own wording, so the failure names
    the editor-only module rather than only reporting that the set changed.
    """
    deps = _dependency_module_names(TESTS_BUILD_CS.read_text(encoding="utf-8"))
    editor_only = deps & EDITOR_ONLY_MODULE_NAMES
    assert not editor_only, (
        f"{TESTS_BUILD_CS.name} depends on editor-only module(s) {sorted(editor_only)} -- "
        "§9 dim 8's own claim is that this dependency set contains none, and the packaged "
        "link is what that claim protects"
    )


def test_g_must_reject_an_unreal_ed_dependency_is_caught():
    """Must-reject for test_e and test_f, with the mutation the plan's own dim 11 names: add
    an UnrealEd dependency to the module's Build.cs. Planted into a scratch copy of the real
    file, and both checkers are required to fire on it.
    """
    real_text = TESTS_BUILD_CS.read_text(encoding="utf-8")
    anchor = '\t\t\t"Core",'
    assert anchor in real_text, (
        f"mutation anchor {anchor!r} not found in {TESTS_BUILD_CS} -- update the anchor "
        "before trusting this test's result"
    )
    mutated_text = real_text.replace(anchor, '\t\t\t"UnrealEd",\n' + anchor, 1)
    assert mutated_text != real_text, "mutation did not apply"

    mutated_deps = _dependency_module_names(mutated_text)
    assert mutated_deps != RULED_DEPENDENCY_SET, (
        "planted UnrealEd into a scratch copy of the real Build.cs and the set-equality "
        "checker did not catch it"
    )
    assert "UnrealEd" in mutated_deps & EDITOR_ONLY_MODULE_NAMES, (
        "planted UnrealEd into a scratch copy of the real Build.cs and the editor-only "
        "denylist did not catch it"
    )


# --------------------------------------------------------------------------------------- #
# Dim 3 -- the instrument's own oracle cell runs where the instrument is used
# --------------------------------------------------------------------------------------- #

def test_h_allocation_scope_oracle_cell_runs_in_both_contexts():
    """Must-accept for M7 re-cross dimension 3. `FSuperSLMAllocationScopePeakNetOracleTest` is
    the commissioning cell for the allocator proxy MM-1 decided through and MM-5 decides
    through. Its flags
    must carry ClientContext as well as EditorContext: an instrument commissioned only where
    it is not used is not commissioned, and the packaged
    Development client is where it is used.
    """
    assert PACKAGED_CELLS_CPP.is_file(), f"{PACKAGED_CELLS_CPP} not found"
    flags = _automation_test_flags(
        PACKAGED_CELLS_CPP.read_text(encoding="utf-8"),
        "FSuperSLMAllocationScopePeakNetOracleTest",
    )
    assert flags, (
        "FSuperSLMAllocationScopePeakNetOracleTest was not found in "
        f"{PACKAGED_CELLS_CPP.name} -- the instrument's own oracle cell is gone, or its "
        "declaration no longer matches this checker's pattern"
    )
    assert "ClientContext" in flags, (
        f"the allocator proxy's oracle cell is tagged {sorted(flags)} -- without ClientContext "
        "it never executes in the packaged Development client, which is the configuration MM-1 "
        "read its verdicts in and MM-5 reads its verdicts in (M7 re-cross dimension 3)"
    )
    assert "EditorContext" in flags, (
        f"the allocator proxy's oracle cell is tagged {sorted(flags)} -- without EditorContext "
        "it drops out of the editor SuperSLM.L2S0.* pass, which is the only pass this suite "
        "has ever executed it in"
    )


def test_i_must_reject_dropping_client_context_is_caught():
    """Must-reject for test_h, with its stated mutation: remove ClientContext from the cell's
    flags. Planted into a scratch copy of the real .cpp.
    """
    real_text = PACKAGED_CELLS_CPP.read_text(encoding="utf-8")
    anchor = ("EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | "
              "EAutomationTestFlags::ProductFilter")
    assert anchor in real_text, (
        f"mutation anchor not found in {PACKAGED_CELLS_CPP} -- update the anchor before "
        "trusting this test's result"
    )
    mutated_text = real_text.replace(
        anchor, "EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter", 1
    )
    assert mutated_text != real_text, "mutation did not apply"

    mutated_flags = _automation_test_flags(
        mutated_text, "FSuperSLMAllocationScopePeakNetOracleTest"
    )
    assert "ClientContext" not in mutated_flags, (
        "removed ClientContext in a scratch copy of the real .cpp and the checker still "
        "reported it present -- the checker cannot discriminate"
    )
    assert "EditorContext" in mutated_flags, (
        "the mutation removed more than the one flag it names -- the construction is not the "
        "stated mutation"
    )


# --------------------------------------------------------------------------------------- #
# D-SLM5342 -- the module's contents never read the plugin's Source/ tree
# --------------------------------------------------------------------------------------- #

def test_j_no_cell_in_the_tests_module_reads_the_plugin_source_tree():
    """Must-accept. RULING D-SLM5342 states this as a requirement of the module, not a
    preference: staging copies a plugin's Content, Config and Binaries and never its Source/,
    so a fixture read here returns zero bytes in a packaged client. That zero is exactly what
    drove MM-1's TestFalse arm red against a correctly memory-mapped load.
    """
    assert TESTS_MODULE_ROOT.is_dir(), f"{TESTS_MODULE_ROOT} does not exist"
    reads = _source_tree_reads(TESTS_MODULE_ROOT)
    assert not reads, (
        f"file(s) hosted by {TESTS_MODULE_NAME} reach for the plugin's Source/ tree: {reads}. "
        "Staging never copies Source/, so this read returns nothing in the packaged client "
        "these cells exist to run in (RULING D-SLM5342). A cell that needs a committed fixture "
        "belongs in SuperSLMUnrealEditor by the ruling's own partition rule"
    )


def test_l_hosted_cell_list_matches_the_module_s_real_cells():
    """The editor-side hosting cell (`SuperSLMModuleHostingTests.cpp`) asserts that every
    automation cell `SuperSLMUnrealTests` hosts is registered with the automation framework, and
    it does so from a hand-written list of class names -- because a cell inside a module that
    did not load contributes no cell that can fail, so the question has to be asked from
    outside.

    A hand-written list drifts. This cell is what stops it drifting, in both directions: a cell
    added to the module and not listed there would silently go unpinned, and a name listed there
    that no longer exists in source would red the hosting cell for a reason that is not a
    hosting failure.
    """
    hosting_cpp = (
        SOURCE_ROOT / "SuperSLMUnrealEditor" / "Private" / "Tests" / "SuperSLMModuleHostingTests.cpp"
    )
    assert hosting_cpp.is_file(), f"{hosting_cpp} not found"

    listed = set(
        re.findall(r'TEXT\("(F[A-Za-z0-9]*Test)"\)', hosting_cpp.read_text(encoding="utf-8"))
    )
    real = set()
    for cpp in sorted((TESTS_MODULE_ROOT / "Private" / "Tests").glob("*.cpp")):
        real.update(
            m.group("klass")
            for m in _AUTOMATION_TEST_PATTERN.finditer(cpp.read_text(encoding="utf-8"))
        )

    assert real, (
        "found zero automation cells under SuperSLMUnrealTests/Private/Tests -- the glob is "
        "broken, not a genuinely empty module"
    )
    assert real - listed == set(), (
        f"{sorted(real - listed)} are hosted by {TESTS_MODULE_NAME} but not listed in "
        f"{hosting_cpp.name} -- they are unpinned: if the module stopped loading, nothing would "
        "name their disappearance"
    )
    assert listed - real == set(), (
        f"{sorted(listed - real)} are listed in {hosting_cpp.name} but no longer exist in "
        f"{TESTS_MODULE_NAME} -- the hosting cell would go red for a renamed or removed cell "
        "rather than for a hosting failure"
    )


def test_k_must_reject_a_planted_fixture_include_is_caught():
    """Must-reject for test_j, with its stated mutation: add the Source-tree fixtures include
    to a file the module compiles. Planted into a scratch copy of the real packaged-cells file.
    """
    real_text = PACKAGED_CELLS_CPP.read_text(encoding="utf-8")
    anchor = '#include "SuperSLMModel.h"'
    assert anchor in real_text, (
        f"mutation anchor {anchor!r} not found in {PACKAGED_CELLS_CPP} -- update the anchor "
        "before trusting this test's result"
    )
    mutated_text = real_text.replace(
        anchor, '#include "Fixtures/SuperSLMImportFixtures.h"\n' + anchor, 1
    )
    assert mutated_text != real_text, "mutation did not apply"

    reads = _source_tree_reads(TESTS_MODULE_ROOT, overrides={PACKAGED_CELLS_CPP: mutated_text})
    assert reads, (
        "planted the Source-tree fixtures include into a scratch copy of the real "
        "packaged-cells file and the checker did not catch it"
    )
