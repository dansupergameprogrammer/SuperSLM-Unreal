"""T-2226 -- L2-S0 red suite. Guard G5 commissioning.

REWRITTEN AGAIN in the T-2226 fix round routed by CONFIRMATION code review T-2249
(the confirmation review, finding N1, closing
finding C2 for the second time).

**N1, as found.** The prior rewrite (T-2241 C2 fix round) compiled the REAL header for the
first time, which was real progress -- but computed its compile flags as
`real_flags + ["/we4062", "/std:c++20", "/EHsc"]`. It read the module's real flags (the right
instinct) and then ADDED the one flag whose absence was the original defect. The module's own
header carries `#pragma warning(error : 4061)` -- MSVC's C4061 is the wrong diagnostic (fires
only when a switch HAS a `default` label; this switch has none). The real diagnostic for a
default-less switch missing an arm is **C4062**, and the module's real build promotes neither
4061 (the pragma names the wrong number) nor 4062 (nothing else promotes it). Executed on this
machine, four configurations (T-2249's own table): the mutated real header under the module's
own real flags, unmodified, **compiles clean, exit 0 -- the guard does not fire in the
plugin's own build.** The commissioning test's two cells nonetheless both passed, because the
test's own added `/we4062` supplied the missing promotion the module does not carry -- the
control proved the SAME wrong thing the T-2241 rewrite existed to stop proving, one layer
over. The commissioning rule again: "a control exercising a failure mode the
production path cannot produce proves nothing about the readings the instrument will actually
emit."

**This rewrite removes every flag this test does not read from the module's own captured
compile line.** No `/we4062`. No hardcoded `/std:c++20` comment-justified as "confirmed
present" -- it is read from a real per-file `.rsp` when one exists, exactly as the warning
flags are. Nothing is added on top of what is read. This means:

  - **Until the build's own fix lands (correcting the pragma's `4061` to `4062`, per this
    round's routing), `test_must_reject_missing_arm_in_real_header` is RED** -- the mutated
    header compiles CLEAN under the module's real (currently wrong) flags, so the assertion
    that it must fail to compile FAILS. **This is the correct and intended state of this test
    right now**, not a defect in the test: a guard that cannot fire is a guard this
    commissioning suite must report as not firing, and reporting it as passing would be
    exactly N1's own failure shape repeated a third time (T-2249's own prediction: "G5 will be
    found dead a third time, by the same instrument that found it alive twice ... unless the
    test's flags are made to *be* the module's flags with nothing added").
  - Once the pragma is corrected, this test needs no further change -- it will read the
    corrected promotion from the header's own compiled behavior (the mutated header will then
    fail to compile under the module's real, now-correct flags) and both cells go green for
    the first time for the right reason.

The must-accept case (`test_must_accept_real_header_is_genuinely_exhaustive`) is unaffected by
the pragma bug either way -- the real, unmodified header is genuinely exhaustive today
regardless of which C-number is promoted, and continues to compile clean.

**What is unchanged from the prior rewrite:**
  (a) compiles the REAL header, Source/SuperSLMUnreal/Public/SuperSLMStatusMapping.h, never a
      synthetic stand-in;
  (b) constructs its must-reject mutation by deleting the ONE hand-written, non-X-macro-
      generated arm the real header contains -- the explicit `case SSLM_STATUS_NEXT_FREE:`
      block -- from a scratch COPY of the real file. Never touches the vendored Layer-1 header
      (out of this suite's writable scope) that the X-macro-generated arms come from;
  (c) discovers MSVC honestly: bare `PATH` first, then `vswhere.exe`, then a documented set of
      standard VS 2022 install roots -- skips only if neither route finds a toolchain, which
      has not happened on this machine across three rounds now.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
PLUGIN_ROOT = HERE.parent.parent  # .../Plugins/SuperSLMUnreal
REAL_HEADER = PLUGIN_ROOT / "Source" / "SuperSLMUnreal" / "Public" / "SuperSLMStatusMapping.h"

# Candidate roots for the module's captured compile line, tried in order. This checkout's own
# Intermediate/ (populated only after a build has run in this checkout) is preferred; the
# Plugins/SuperSLMUnreal/Intermediate of the checkout SUPERSLM_BUILD_ROOT names is the fallback,
# for a checkout that has not been built itself (T-2241's and T-2249's reviews read the real
# captured .rsp from a built checkout).
_INTERMEDIATE_ROOTS = [
    PLUGIN_ROOT / "Intermediate" / "Build" / "Win64" / "x64" / "UnrealEditor" / "Development" / "SuperSLMUnreal",
    # The fallback: a checkout that has built the plugin, named by SUPERSLM_BUILD_ROOT (the repository
    # root that holds Plugins/). Unset, there is no fallback, and a worktree with no .rsp skips.
    *([Path(os.environ["SUPERSLM_BUILD_ROOT"]) / "Plugins" / "SuperSLMUnreal" / "Intermediate" / "Build" / "Win64" / "x64"
       / "UnrealEditor" / "Development" / "SuperSLMUnreal"] if os.environ.get("SUPERSLM_BUILD_ROOT") else []),
]

THIRDPARTY_INCLUDE = PLUGIN_ROOT / "Source" / "ThirdParty" / "SuperSLM" / "include"

PROBE_SOURCE = (
    "// T-2226 -- compiled against the REAL SuperSLMStatusMapping.h (or a scratch mutated\n"
    "// copy of it), under the module's own captured flags and nothing else. See\n"
    "// test_guard_g5_status_switch_exhaustive.py.\n"
    "#include \"SuperSLMStatusMapping.h\"\n\n"
    "int main()\n"
    "{\n"
    "\tconst char* Text = SuperSLMStatusMapping::ToDiagnosticText(SSLM_OK);\n"
    "\treturn Text == nullptr;\n"
    "}\n"
)


def _find_intermediate_root():
    """The first candidate root that actually has SuperSLMUnreal.Shared.rsp in it, or None."""
    for root in _INTERMEDIATE_ROOTS:
        if (root / "SuperSLMUnreal.Shared.rsp").is_file():
            return root
    return None


def _read_real_module_flags(intermediate_root: Path):
    """Every flag this test compiles with, and NOTHING this test did not read from a real
    captured .rsp -- the whole point of this rewrite (T-2249 N1). Two files:
      - SuperSLMUnreal.Shared.rsp: the module-wide warning-promotion set (/W4, /we..., /wd...)
        plus /EHsc -- read as a plain substring/token search, not filtered to a guessed
        subset, so nothing this test "knows should matter" is silently added or omitted.
      - The narrowest per-file .rsp available for a .cpp that compiles THIS header
        (SuperSLMStatusMapping.cpp.obj.rsp when present, else any *.cpp.obj.rsp in the same
        directory) -- for /std:c++NN, which is set per-file, not in Shared.rsp.
    """
    shared_rsp = intermediate_root / "SuperSLMUnreal.Shared.rsp"
    shared_text = shared_rsp.read_text(encoding="utf-8", errors="replace")
    warning_flags = re.findall(r"/w[edE]\d{4}|/W[0-4]", shared_text)
    ehsc = re.findall(r"/EHsc\b", shared_text)

    std_flag = []
    per_file_candidates = sorted(intermediate_root.glob("*.cpp.obj.rsp"))
    preferred = intermediate_root / "SuperSLMStatusMapping.cpp.obj.rsp"
    ordered = ([preferred] if preferred.is_file() else []) + [
        p for p in per_file_candidates if p != preferred
    ]
    for candidate in ordered:
        text = candidate.read_text(encoding="utf-8", errors="replace")
        found = re.findall(r"/std:c\+\+\d+", text)
        if found:
            std_flag = [found[0]]
            break

    # Deduplicated, order-preserving -- nothing appended beyond what was found.
    return list(dict.fromkeys(warning_flags + ehsc + std_flag))


def _find_cl_on_path():
    return shutil.which("cl")


def _find_vcvars64():
    """Locate vcvars64.bat via vswhere.exe first, then a documented set of standard VS 2022
    install roots. Returns a Path, or None if truly not found by either route."""
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    if vswhere.is_file():
        try:
            proc = subprocess.run(
                [str(vswhere), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, timeout=30,
            )
            lines = [l for l in proc.stdout.splitlines() if l.strip()]
            if lines:
                candidate = Path(lines[0].strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
                if candidate.is_file():
                    return candidate
        except Exception:
            pass
    for program_files in (r"C:\Program Files", r"C:\Program Files (x86)"):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            candidate = Path(program_files) / "Microsoft Visual Studio" / "2022" / edition / \
                "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.is_file():
                return candidate
    return None


def _msvc_compile(source_path: Path, include_dirs, flags, out_dir: Path):
    """Compile `source_path` with MSVC, discovered either on PATH or via a located
    vcvars64.bat. Returns (returncode, combined_output). Raises RuntimeError if no MSVC
    toolchain can be located by either route -- the caller turns that into an honest skip."""
    include_args = " ".join(f'/I"{d}"' for d in include_dirs)
    flag_args = " ".join(flags)
    obj_path = out_dir / "g5_probe.obj"
    cl = _find_cl_on_path()
    if cl:
        cmd = f'cl /nologo /c {flag_args} {include_args} "{source_path}" /Fo"{obj_path}"'
        proc = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=out_dir)
        return proc.returncode, proc.stdout + proc.stderr

    vcvars = _find_vcvars64()
    if vcvars:
        # shell=True already invokes cmd.exe on Windows -- an explicit ["cmd", "/c", cmd] list
        # double-quotes the already-quoted vcvars path (subprocess's own MSVCRT
        # argv-quoting rules re-escape a string that is itself a quoted command line).
        cmd = (
            f'"{vcvars}" >nul && cl /nologo /c {flag_args} {include_args} '
            f'"{source_path}" /Fo"{obj_path}"'
        )
        proc = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=out_dir)
        return proc.returncode, proc.stdout + proc.stderr

    raise RuntimeError(
        "no MSVC toolchain found: neither cl.exe on PATH nor a locatable vcvars64.bat "
        "(checked vswhere.exe and the standard VS 2022 install roots)"
    )


def _mutate_remove_next_free_arm(header_text: str) -> str:
    """The one genuine, hand-written (non-X-macro-generated) arm this header contains --
    removing it is a real "one arm missing" mutation against the real file's real structure."""
    anchor = "case SSLM_STATUS_NEXT_FREE:"
    start = header_text.index(anchor)
    return_kw = header_text.index("return", start)
    end_of_statement = header_text.index(";", return_kw) + 1
    mutated = header_text[:start] + header_text[end_of_statement:]
    assert mutated != header_text, "mutation did not remove anything"
    assert "SSLM_STATUS_NEXT_FREE" not in mutated, "the enumerator name must be fully removed"
    return mutated


def _compile_probe_against(header_dir: Path, flags):
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        probe_path = out_dir / "g5_probe.cpp"
        probe_path.write_text(PROBE_SOURCE, encoding="utf-8")
        return _msvc_compile(probe_path, [header_dir, THIRDPARTY_INCLUDE], flags, out_dir)


def test_must_accept_real_header_is_genuinely_exhaustive():
    """Must-accept: the REAL, unmodified header compiles clean
    under the module's OWN real flags -- nothing added -- proving it is genuinely exhaustive
    right now under the build that actually ships it."""
    assert REAL_HEADER.is_file(), f"real header missing: {REAL_HEADER} -- L2-S0's own deliverable"
    if not THIRDPARTY_INCLUDE.is_dir():
        import pytest
        pytest.skip(f"{THIRDPARTY_INCLUDE} does not exist -- Layer 1 not vendored yet")
    intermediate_root = _find_intermediate_root()
    if intermediate_root is None:
        import pytest
        pytest.skip("no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set) -- "
                     "no UE build has produced one there")
    flags = _read_real_module_flags(intermediate_root)
    try:
        returncode, output = _compile_probe_against(REAL_HEADER.parent, flags)
    except RuntimeError as e:
        import pytest
        pytest.skip(f"no MSVC toolchain locatable by either route: {e}")
    assert returncode == 0, (
        f"the REAL SuperSLMStatusMapping.h must compile clean under the module's own real "
        f"flags {flags!r}; got returncode={returncode!r}, output={output!r}"
    )


def test_must_reject_missing_arm_in_real_header():
    """Must-reject: the mutation is applied to a SCRATCH COPY of the REAL file's actual text
    (never a synthetic stand-in), and must fail to compile under the module's OWN real flags
    -- nothing added.

    KNOWN RED as of this fix round (T-2249 N1): the module's real pragma promotes MSVC C4061,
    which cannot fire on a default-less switch; the correct diagnostic is C4062, which nothing
    in the module's real compile line promotes yet. Until the implementation corrects the pragma
    (4061 -> 4062, routed alongside this fix), the mutated header compiles CLEAN under these
    flags and this assertion FAILS -- correctly. Reporting green here would repeat N1/C2 a
    third time.
    """
    assert REAL_HEADER.is_file(), f"real header missing: {REAL_HEADER}"
    if not THIRDPARTY_INCLUDE.is_dir():
        import pytest
        pytest.skip(f"{THIRDPARTY_INCLUDE} does not exist -- Layer 1 not vendored yet")
    intermediate_root = _find_intermediate_root()
    if intermediate_root is None:
        import pytest
        pytest.skip("no captured SuperSLMUnreal.Shared.rsp found (the worktree, or SUPERSLM_BUILD_ROOT if set) -- "
                     "no UE build has produced one there")
    flags = _read_real_module_flags(intermediate_root)

    real_text = REAL_HEADER.read_text(encoding="utf-8")
    mutated_text = _mutate_remove_next_free_arm(real_text)

    try:
        with tempfile.TemporaryDirectory() as header_tmp:
            header_dir = Path(header_tmp)
            (header_dir / "SuperSLMStatusMapping.h").write_text(mutated_text, encoding="utf-8")
            returncode, output = _compile_probe_against(header_dir, flags)
    except RuntimeError as e:
        import pytest
        pytest.skip(f"no MSVC toolchain locatable by either route: {e}")

    assert returncode != 0, (
        f"removing SSLM_STATUS_NEXT_FREE's arm from a scratch copy of the REAL header must "
        f"fail to compile under the module's OWN real flags {flags!r} -- if this assertion "
        f"itself is failing, the module's pragma still promotes the wrong MSVC warning "
        f"number (C4061 instead of C4062, T-2249 N1) and guard G5 genuinely does not fire in "
        f"the plugin's own build; that is a real finding, not a test defect, and this test "
        f"must not be patched to hide it by adding a flag back"
    )
    assert "4062" in output or "switch" in output.lower(), (
        f"rejection should be attributable to the switch-exhaustiveness diagnostic; "
        f"got output={output!r}"
    )


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print(f"{name}: PASS/SKIPPED")
            except AssertionError as e:
                print(f"{name}: RED -- {e}")
