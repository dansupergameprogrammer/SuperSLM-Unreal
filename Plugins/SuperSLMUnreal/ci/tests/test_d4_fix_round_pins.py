"""T-3050/T-3102: executable/source pins for the repaired standalone witness and docs.

The command-path estimate and settings pins live in SuperSLMD4EstimateTests.cpp and
SuperSLMD4SettingsTests.cpp. This file keeps the five local vendored tool edits, the
profiling text and the D4 estimate/settings documentation tied to their consumers.
"""

import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


PLUGIN = Path(__file__).resolve().parents[2]
REPO = Path(subprocess.run(
    ["git", "rev-parse", "--show-toplevel"], cwd=PLUGIN,
    check=True, capture_output=True, text=True,
).stdout.strip())
VENDOR = PLUGIN / "Source" / "ThirdParty" / "SuperSLM"
TOOLS = VENDOR / "tools" / "consumer_reach" / "route_p_hook"
HOOK = PLUGIN / "Source" / "SuperSLMUnreal" / "Private"
PUBLIC = PLUGIN / "Source" / "SuperSLMUnreal" / "Public"
LOCAL_TOOL_FILES = (
    "route_p_hook_witness.cpp",
    "route_p_run_count.cpp",
    "run_route_p_hook_witness.sh",
    "run_merge_acceptance.sh",
    "shim/CoreMinimal.h",
)


def _linux_path(path: Path) -> str:
    path = path.resolve()
    if os.name != "nt":
        return str(path)
    return f"/mnt/{path.drive[0].lower()}/{path.as_posix()[3:]}"


def _compile(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    argv = [str(a) if not isinstance(a, Path) else _linux_path(a) for a in arguments]
    if os.name == "nt":
        command = " ".join(shlex.quote(a) for a in argv)
        return subprocess.run(["wsl.exe", "-e", "bash", "-lc", command], capture_output=True, text=True)
    return subprocess.run(argv, capture_output=True, text=True)


def test_route_p_witness_uses_two_argument_hook_and_runs():
    """Compile the real adapted witness, then compile the old one-argument call as a reject."""
    with tempfile.TemporaryDirectory(prefix="d4_route_p_") as tmp:
        scratch = Path(tmp)
        for source in (HOOK / "SuperSLMFinishHook.cpp", HOOK / "SuperSLMFinishHook.h",
                       PUBLIC / "SuperSLMParallelDecodeTestAccess.h", TOOLS / "route_p_hook_witness.cpp",
                       TOOLS / "route_p_run_count.cpp"):
            shutil.copy2(source, scratch / source.name)
        command = ["g++", "-std=c++20", "-I" + _linux_path(scratch),
                   "-I" + _linux_path(TOOLS / "shim"), "-I" + _linux_path(VENDOR / "include")]
        executable = scratch / "route_p_witness"
        built = _compile(command + [scratch / "SuperSLMFinishHook.cpp",
                                    scratch / "route_p_hook_witness.cpp", "-o", executable])
        assert built.returncode == 0, built.stdout + built.stderr
        ran = _compile([executable])
        assert ran.returncode == 0 and "route P: H = not hooked" in ran.stdout, ran.stdout + ran.stderr
        count = _compile(command + ["-fsyntax-only", scratch / "route_p_run_count.cpp"])
        assert count.returncode == 0, count.stdout + count.stderr

        for name, old_call in (("route_p_hook_witness.cpp", "Make(k)"),
                               ("route_p_run_count.cpp", "Make(k)")):
            path = scratch / name
            current = path.read_text(encoding="utf-8")
            assert current.count("Make(k, 0)") >= 1
            path.write_text(current.replace("Make(k, 0)", old_call), encoding="utf-8")
            rejected = _compile(command + ([scratch / "SuperSLMFinishHook.cpp", path, "-o", executable]
                                           if name == "route_p_hook_witness.cpp" else ["-fsyntax-only", path]))
            assert rejected.returncode != 0, "the pre-remedy one-argument witness unexpectedly compiled"
            path.write_text(current, encoding="utf-8")


def test_route_p_drivers_keep_the_new_mutation_anchor():
    driver = (TOOLS / "run_route_p_hook_witness.sh").read_text(encoding="utf-8")
    merge = (TOOLS / "run_merge_acceptance.sh").read_text(encoding="utf-8")
    for text in (driver, merge):
        assert "SuperSLMParallelDecodeTestAccess.h" in text
        assert "Hook\\.reserved = Bits;" in text
    for variant in ("control", "H1_hex_zero", "H2_named_zero", "H3_reserved_deleted_Hook_value_init",
                    "Hplus_prefill_bit", "Hbad_other_bit", "Hplus_named_bit",
                    "Hk16_bit_only_at_k_ge_16", "Hk_lt16", "dead-counter fixture"):
        assert variant in driver
    assert "COMPILE FAIL" in merge and "=> REFUSED" in merge and "=> ACCEPT" in merge


def test_local_tool_delta_is_manifested_and_disclosed():
    manifest = (VENDOR / "VENDORED_MANIFEST.sha256").read_text(encoding="utf-8")
    entries = dict((path, digest) for digest, path in re.findall(r"^([0-9a-f]{64})  (.+)$", manifest, re.M))
    version = (VENDOR / "VENDORED_VERSION.txt").read_text(encoding="utf-8")
    committed_contents = {}
    for name in LOCAL_TOOL_FILES:
        rel = f"tools/consumer_reach/route_p_hook/{name}"
        assert rel in entries, f"local repair is absent from the manifest: {rel}"
        # The manifest signs repository bytes. Windows text checkouts may be CRLF; verify
        # the checkout's Git-clean identity too, so an uncommitted vendor edit cannot hide
        # behind the committed blob while ordinary checkout conversion stays permissible.
        path = (VENDOR / rel).relative_to(REPO).as_posix()
        expected_oid = subprocess.run(["git", "rev-parse", f"HEAD:{path}"], cwd=REPO,
                                      capture_output=True, text=True, check=True).stdout.strip()
        checkout_oid = subprocess.run(["git", "hash-object", f"--path={path}", str(VENDOR / rel)],
                                      cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
        assert checkout_oid == expected_oid, f"uncommitted vendored content: {rel}"
        contents = subprocess.run(["git", "show", f"HEAD:{path}"], cwd=REPO,
                                  capture_output=True, check=True).stdout
        committed_contents[rel] = contents
        assert hashlib.sha256(contents).hexdigest() == entries[rel], rel
        assert name in version, f"local tool repair is not disclosed: {name}"
    assert "Commit: 165962acafe901753dfe37153d7116b132a18b5a" in version
    assert "Base release commit: 165962acafe901753dfe37153d7116b132a18b5a" in version
    assert "Release status: published release v1.15.0" in version
    assert "sslm_seq_jump_forward" in version
    architecture = (REPO / "RepoDrafts" / "SuperSLMUnreal" / "docs" / "ARCHITECTURE.md").read_text(encoding="utf-8")
    assert "Five route-P witness tool files carry" in architecture
    assert "a tracked hook-API adaptation" in architecture

    # Exercise the actual checker on a scratch tree of the five local tool repairs.
    # The separate committed-tree test covers every other vendored file; this copy
    # avoids Windows checkout line-ending conversion of unrelated upstream files.
    with tempfile.TemporaryDirectory(prefix="d4_manifest_") as tmp:
        root = Path(tmp)
        repaired = [f"tools/consumer_reach/route_p_hook/{name}" for name in LOCAL_TOOL_FILES]
        (root / "VENDORED_MANIFEST.sha256").write_text(
            "".join(f"{entries[rel]}  {rel}\n" for rel in repaired), encoding="utf-8")
        for rel in repaired:
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(committed_contents[rel])
        checker = PLUGIN / "ci" / "check_vendored_tree_matches_manifest.py"
        command = [sys.executable, str(checker), "--thirdparty-root", str(root)]
        accepted = subprocess.run(command, capture_output=True, text=True)
        assert accepted.returncode == 0, accepted.stdout + accepted.stderr
        changed = root / "tools/consumer_reach/route_p_hook/route_p_hook_witness.cpp"
        changed.write_bytes(changed.read_bytes() + b"\n// unrecorded local edit\n")
        refused = subprocess.run(command, capture_output=True, text=True)
        assert refused.returncode != 0 and "CONTENT CHANGED: tools/consumer_reach/route_p_hook/route_p_hook_witness.cpp" in refused.stdout


# T-3102 (plan §10.3.1 item 5b; D-SLM8074, D-SLM8110, D-SLM8111, D-SLM8143, D-SLM8168): what the
# guides must say about D4's estimate and settings, and what they must no longer say about the
# retired quiet-box calibration command.
GUIDES_ROOT = REPO / "RepoDrafts" / "SuperSLMUnreal"
_ESTIMATE_REQUIRED = {
    "docs/TOOLING.md": ("`SuperSLM.EstimateCosts", "--write-ini", "CALIBRATION ESTIMATE — not verified",
                        "`SuperSLM.SetDecodeMode", "`SuperSLM.SetFinishParallelTasks", "--save",
                        "RuntimeSettings.ini", "CalibratedCosts.ini"),
    "docs/ARCHITECTURE.md": ("`SuperSLM.EstimateCosts", "--write-ini", "`SuperSLM.SetDecodeMode", "RuntimeSettings.ini",
                             "1.397", "0.906"),
    "docs/STATUS.md": ("1.397", "0.906"),
}
_ESTIMATE_STALE = ("otherwise idle", "refuses to report", "verified-idle", "[bParallelDecode: 0|1]",
                   "does not read back", "SuperSLM.CalibrateCosts",
                   # The thirteen U1 calibration-group cells are retired (D-SLM8111); no guide names them.
                   "U1.Calibration", "U1.Relevance")
# The "-ExecCmds=..." form (D-SLM8110): from the double quote immediately before -ExecCmds= to the
# next double quote. Unreal splits -ExecCmds on ',' only, and the editor's exit command is
# QUIT_EDITOR (Quit is a game-engine command the editor echoes and ignores).
_EXEC_CMDS = re.compile(r'"-ExecCmds=([^"]*)"')


# D-SLM8168 read the topmost release section because D4 was to ride the next, then unnumbered,
# release. It shipped as 1.2.0, so the pin reads that section; a later section on top of it
# (an Unreleased section) no longer moves what is pinned.
_D4_RELEASE = "1.2.0"


def _changelog_d4_section(changelog: str) -> str:
    """The release section D4 shipped in: from its '## 1.2.0' heading to the next '## ' heading."""
    for part in re.split(r"(?m)^## ", changelog)[1:]:
        if part.split(None, 1)[0] == _D4_RELEASE:
            return part.split("\n## ", 1)[0]
    return ""


def _estimate_docs_problems(guides: dict[str, str], changelog: str) -> list[str]:
    """Every unmet claim over the guides (every .md under RepoDrafts/SuperSLMUnreal except
    CHANGELOG.md, keyed by relative path) and the CHANGELOG; empty when all hold."""
    problems = []
    for name, required in _ESTIMATE_REQUIRED.items():
        doc = guides.get(name)
        if doc is None:
            problems.append(f"{name} is missing")
            continue
        problems += [f"{name} lacks {text!r}" for text in required if text not in doc]
    for name, doc in guides.items():
        problems += [f"{name} still says {text!r}" for text in _ESTIMATE_STALE if text in doc]
    contributing = guides.get("CONTRIBUTING.md", "")
    lock_rows = [line for line in contributing.splitlines() if "SUPERSLM_BUILD_LOCK" in line and line.lstrip().startswith("|")]
    if not lock_rows:
        problems.append("CONTRIBUTING.md has no SUPERSLM_BUILD_LOCK table row")
    problems += [f"CONTRIBUTING.md's SUPERSLM_BUILD_LOCK row says the lock refuses: {row.strip()}"
                 for row in lock_rows if re.search(r"(?i)refus", row)]
    tooling = guides.get("docs/TOOLING.md", "")
    examples = [body for body in _EXEC_CMDS.findall(tooling) if "SuperSLM.EstimateCosts" in body]
    if not examples:
        problems.append('TOOLING.md has no "-ExecCmds=..." example running SuperSLM.EstimateCosts')
    for body in examples:
        if re.search(r"SuperSLM\.EstimateCosts\s+'", body):
            problems.append("TOOLING.md's -ExecCmds artifact path uses unsupported apostrophe quoting")
        if ";" in body or ",Quit" in body or not body.rstrip().endswith(",QUIT_EDITOR"):
            problems.append(f"TOOLING.md's -ExecCmds example must separate commands with ',' and end ',QUIT_EDITOR': {body}")
    top = _changelog_d4_section(changelog)
    for text in ("SuperSLM.CalibrateCosts", "SuperSLM.EstimateCosts", "15.023"):
        if text not in top:
            problems.append(f"CHANGELOG.md's {_D4_RELEASE} release section lacks {text!r}")
    return problems


def _read_guides() -> tuple[dict[str, str], str]:
    guides = {path.relative_to(GUIDES_ROOT).as_posix(): path.read_text(encoding="utf-8")
              for path in GUIDES_ROOT.rglob("*.md") if path.name != "CHANGELOG.md"}
    return guides, (GUIDES_ROOT / "CHANGELOG.md").read_text(encoding="utf-8")


def test_estimate_docs_state_the_d4_contract():
    guides, changelog = _read_guides()
    assert _estimate_docs_problems(guides, changelog) == []


def test_estimate_docs_check_refuses_each_stale_or_missing_claim():
    """Mutation proof: a guide set meeting every claim passes, and each single wrong edit fails."""
    tooling = ("`SuperSLM.EstimateCosts \"<ArtifactPath.sslm>\" [--write-ini]` prints CALIBRATION ESTIMATE — not verified; "
               "only --write-ini writes Saved/SuperSLM/CalibratedCosts.ini. `SuperSLM.SetDecodeMode 0|1 [--save]` and "
               "`SuperSLM.SetFinishParallelTasks <0..256> [--save]` persist to Saved/SuperSLM/RuntimeSettings.ini.\n"
               "UnrealEditor-Cmd.exe Project.uproject -unattended -log \"-ExecCmds=SuperSLM.EstimateCosts Models/model.sslm,QUIT_EDITOR\"\n")
    guides = {
        "docs/TOOLING.md": tooling,
        "docs/ARCHITECTURE.md": ("`SuperSLM.EstimateCosts` reports an estimate; --write-ini saves it. `SuperSLM.SetDecodeMode` "
                                 "saves to RuntimeSettings.ini. LayerCostMs defaults: 1.397 (mode 0), 0.906 (mode 1).\n"),
        "docs/STATUS.md": "Per-mode LayerCostMs 1.397 and 0.906.\n",
        "CONTRIBUTING.md": "| `SUPERSLM_BUILD_LOCK` | the estimate reports the lock's state |\n",
    }
    changelog = ("# Changelog\n\n## Unreleased\n\n- `--save` writes only the chosen setting.\n\n"
                 "## 1.2.0 - 2026-10-19\n\n- Removed `SuperSLM.CalibrateCosts`; added `SuperSLM.EstimateCosts`. "
                 "Mode-0 BatchedPromptTokenCostMs 15.023.\n\n## 1.0.0\n\n- `SuperSLM.CalibrateCosts`\n")
    assert _estimate_docs_problems(guides, changelog) == []

    def edit(name: str, old: str, new: str) -> dict[str, str]:
        mutated = dict(guides)
        assert old in mutated[name], f"mutant base lacks {old!r}"
        mutated[name] = mutated[name].replace(old, new)
        return mutated

    mutants = {
        "apostrophe path": (edit("docs/TOOLING.md", "SuperSLM.EstimateCosts Models/model.sslm", "SuperSLM.EstimateCosts 'Models/model.sslm'"), changelog),
        "comma Quit": (edit("docs/TOOLING.md", ",QUIT_EDITOR", ",Quit"), changelog),
        "semicolon QUIT_EDITOR": (edit("docs/TOOLING.md", ",QUIT_EDITOR", ";QUIT_EDITOR"), changelog),
        "no Cmd example": (edit("docs/TOOLING.md", '"-ExecCmds=', "-Exec="), changelog),
        "quiet-box wording": (edit("docs/ARCHITECTURE.md", "reports an estimate", "needs an otherwise idle machine"), changelog),
        "retired command named in a guide": (edit("docs/STATUS.md", "Per-mode", "Run SuperSLM.CalibrateCosts. Per-mode"), changelog),
        "retired mode argument": (edit("docs/TOOLING.md", "[--write-ini]", "[bParallelDecode: 0|1]"), changelog),
        "no read-back": (edit("docs/ARCHITECTURE.md", "saves it", "saves it, which the plugin does not read back"), changelog),
        "label dropped": (edit("docs/TOOLING.md", "CALIBRATION ESTIMATE — not verified", "a calibration result"), changelog),
        "opt-in dropped": (edit("docs/TOOLING.md", "--write-ini", "--write"), changelog),
        "settings file dropped": (edit("docs/ARCHITECTURE.md", "RuntimeSettings.ini", "settings"), changelog),
        "ARCHITECTURE old default": (edit("docs/ARCHITECTURE.md", "1.397", "2.0"), changelog),
        "STATUS old default": (edit("docs/STATUS.md", "0.906", "2.0"), changelog),
        "lock row refuses": (edit("CONTRIBUTING.md", "reports the lock's state", "refuses while the lock is held"), changelog),
        "retired U1 group named": (edit("docs/STATUS.md", "Per-mode", "Run SuperSLM.U1.Relevance. Per-mode"), changelog),
        "CHANGELOG lacks the removal": (guides, changelog.replace("Removed `SuperSLM.CalibrateCosts`; ", "")),
        "CHANGELOG lacks the estimate": (guides, changelog.replace("added `SuperSLM.EstimateCosts`. ", "")),
        "CHANGELOG lacks 15.023": (guides, changelog.replace("15.023", "15.025")),
        "CHANGELOG has no 1.2.0 section": (guides, changelog.replace("## 1.2.0 - ", "## 1.2.1 - ")),
    }
    for name, (mutant_guides, mutant_changelog) in mutants.items():
        assert _estimate_docs_problems(mutant_guides, mutant_changelog), f"the docs check accepted the {name} mutant"


# T-3102 (plan §10.3.1 item 5b "Type-change readers", D-SLM8144, D-SLM8165): only the resolver reads
# the fifteen TOptional fields. Two rules over every first-party module that depends on
# SuperSLMUnreal (SuperSLMUnreal, SuperSLMUnrealEditor with its Tests, SuperSLMUnrealTests, and the
# SuperSLMUnrealMCP plugin's modules; vendored and third-party trees excluded):
#  1. an expression ending in one of the fifteen field names is never used in a boolean context
#     (`?`, `&&`, `||`, `!`, or as the whole condition of `if`/`while`) -- TOptional's explicit
#     operator bool would silently test presence;
#  2. `.IsSet()`, `.GetValue()` or `.Get(` on those names appears only in
#     SuperSLMRuntimeResolve.cpp and the fixture accessor header SuperSLMConfigAccess.h.
_PIN_FIELDS = ("bParallelDecode", "FinishParallelTasks", "LayerCostMs", "LayerCostPerPositionMs", "PromptTokenCostMs",
               "PromptTokenCostPerPositionMs", "BatchedPromptTokenCostMs", "BatchedPromptTokenCostPerPositionMs",
               "FinishCostMs", "ResetCostMs", "AdoptCostMs", "SaveCostMs", "RestoreCostMs", "PrefixBeginCostMs",
               "PrefixReleaseCostMs")
_PIN_ALLOWED_OPTIONAL_READERS = ("SuperSLMRuntimeResolve.cpp", "SuperSLMConfigAccess.h")
_PIN_ROOTS = (PLUGIN / "Source", REPO / "Plugins" / "SuperSLMUnrealMCP" / "Source")
_F = "(?:" + "|".join(_PIN_FIELDS) + ")"
_OPERAND = r"[A-Za-z_][\w\[\]]*(?:(?:\.|->)[A-Za-z_][\w\[\]]*)*"
_BOOLEAN_CONTEXT = (
    re.compile(r"(?:\.|->)" + _F + r"\b(?!\s*(?:\.|->|\(|\[))\s*(?:\?|&&|\|\|)"),
    re.compile(r"(?:&&|\|\|)\s*" + _OPERAND + r"(?:\.|->)" + _F + r"\b\s*(?=\)|&&|\|\||;|\?|,)"),
    re.compile(r"(?<![!=<>])!\s*" + _OPERAND + r"(?:\.|->)" + _F + r"\b(?!\s*(?:\.|->|\(|\[|[=<>!+\-*/]))"),
    re.compile(r"\b(?:if|while)\s*\(\s*" + _OPERAND + r"(?:\.|->)" + _F + r"\s*\)"),
)
_OPTIONAL_READ = re.compile(r"(?:\.|->)" + _F + r"\s*\.\s*(?:IsSet\s*\(|GetValue\s*\(|Get\s*\()")


def _strip_comments_and_strings(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return re.sub(r"//[^\n]*", "", text)


def _pin_sources() -> dict[str, str]:
    files = {}
    for root in _PIN_ROOTS:
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".h", ".cpp") or {"Vendored", "ThirdParty"} & set(path.parts):
                continue
            files[path.relative_to(REPO).as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    return files


def _source_pin_violations(files: dict[str, str]) -> list[str]:
    violations = []
    for name, raw in files.items():
        text = _strip_comments_and_strings(raw)
        for number, line in enumerate(text.splitlines(), 1):
            if any(rule.search(line) for rule in _BOOLEAN_CONTEXT):
                violations.append(f"{name}:{number}: boolean-context read of a D4 config field: {line.strip()}")
            if _OPTIONAL_READ.search(line) and not name.endswith(_PIN_ALLOWED_OPTIONAL_READERS):
                violations.append(f"{name}:{number}: IsSet/GetValue/Get on a D4 config field outside the resolver: {line.strip()}")
    return violations


def test_source_pin_only_the_resolver_reads_the_optional_fields():
    files = _pin_sources()
    assert any(name.endswith("SuperSLMConfigAccess.h") for name in files), "the accessor header is not in the scanned trees"
    assert any("SuperSLMUnrealMCP" in name for name in files), "the MCP plugin's modules are not in the scanned trees"
    assert _source_pin_violations(files) == []


def test_source_pin_refuses_each_planted_read():
    """Mutation proof over the real trees: each plant adds a violation the unplanted trees lack."""
    files = _pin_sources()
    base = set(_source_pin_violations(files))
    target = next(name for name in files if name.endswith("SuperSLMSubsystem.cpp"))
    plants = {
        "boolean read": "\nstatic int T3102PinPlant(const FSuperSLMRuntimeConfig& Config) { return Config.bParallelDecode ? 1 : 0; }\n",
        "stray IsSet": "\nstatic bool T3102PinPlant(const FSuperSLMRuntimeConfig& Config) { return Config.LayerCostMs.IsSet(); }\n",
    }
    for what, plant in plants.items():
        mutated = dict(files)
        mutated[target] = files[target] + plant
        assert set(_source_pin_violations(mutated)) - base, f"the source pin accepted the planted {what}"
    allowed = dict(files)
    resolver = "Plugins/SuperSLMUnreal/Source/SuperSLMUnreal/Private/SuperSLMRuntimeResolve.cpp"
    allowed[resolver] = allowed.get(resolver, "") + plants["stray IsSet"].replace("T3102PinPlant", "T3102ResolverRead")
    assert not {v for v in set(_source_pin_violations(allowed)) - base if "outside the resolver" in v}, \
        "the source pin refused an optional read inside the resolver"


def _docs_name_the_scope(text: str) -> bool:
    timing = text.split("## Timing scopes and threads", 1)[-1].split("**GPU backend.**", 1)[0]
    return ("`SuperSLM.ParallelTask`" in timing and "`SuperSLM.FinishTask`" not in timing
            and "`bParallelDecode` enabled" in timing and "decode" in timing)


def test_profiling_doc_names_the_emitted_decode_scope():
    profiling = (REPO / "RepoDrafts" / "SuperSLMUnreal" / "docs" / "PROFILING.md").read_text(encoding="utf-8")
    assert _docs_name_the_scope(profiling)
    assert not _docs_name_the_scope(profiling.replace("`SuperSLM.ParallelTask`", "`SuperSLM.FinishTask`"))
