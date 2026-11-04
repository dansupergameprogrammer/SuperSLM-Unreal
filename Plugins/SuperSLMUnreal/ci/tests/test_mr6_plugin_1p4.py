"""Multi-row GPU plan, MR6 (plugin 1.4 on engine 1.18; red-suite-mr6.md): what the plugin's own tree must say before
any MR6 cell can run.

  - the plugin is 1.4 and vendors engine 1.18 (errata E-6; ruling 39: until the 1.18 tag it vendors SuperSLM
    a 1.18.0 candidate, then re-vendors the v1.18.0 tag), or a later plugin on a later engine (plugin 1.5 vendors
    v1.19.0, which keeps the multi-row verbs and the measured cost table);
  - the vendored cost table is the measured one (N4, ruling 37), and the chooser is fed from its merged arrays only
    (ruling 39 Q11);
  - the configuration carries ruling 39 Q9's knobs: int32 GpuRowsMarginPerMille (default 150) and bool bGpuMultiRow
    (default true);
  - the trace counters ruling 39 names exist: HostFinishUs (Q6) and ExactFellBackToOff (Q4), beside §5.5's
    RowsPerChunk and RowsSliceMs;
  - Canonical is removed from the setting and from the core's mode (errata E-11 (e), ruling 39 Q10): no enumerator,
    so an old saved value can only fall back, never select it. The switch's behaviour (Q7) is in ci/host/schema_jump.

Comments are stripped before every check, so a comment naming an enumerator neither satisfies nor fails it.
These are red until the implementation re-vendors and removes Canonical.
"""
import re
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
SOURCE = PLUGIN / "Source" / "SuperSLMUnreal"
VENDORED = PLUGIN / "Source" / "ThirdParty" / "SuperSLM"

_LEXEMES = re.compile(
    r'(?P<lit>(?:u8|u|U|L)?R"(?P<d>[^\s()\\]{0,16})\([\s\S]*?\)(?P=d)"'
    r'|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\')'
    r'|//[^\n]*|/\*[\s\S]*?\*/')


def code_of(path):
    text = path.read_text(encoding="utf-8")
    return _LEXEMES.sub(lambda m: m["lit"] if m["lit"] else " " * len(m[0]), text)


def enum_body(code, name):
    match = re.search(r"enum\s+class\s+" + name + r"\b[^{]*\{([^}]*)\}", code)
    assert match, "enum class %s not found" % name
    return match.group(1)


def enumerators(body):
    return [e.split("=")[0].strip() for e in body.split(",") if e.strip()]


def test_plugin_version_is_1_4():
    uplugin = next(PLUGIN.glob("*.uplugin")).read_text(encoding="utf-8")
    match = re.search(r'"VersionName"\s*:\s*"([^"]+)"', uplugin)
    version = match and tuple(int(p) for p in re.match(r"(\d+)\.(\d+)", match.group(1)).groups())
    assert version and version >= (1, 4), "VersionName %s, want 1.4.x or later" % (match and match.group(1))


R17_VENDOR_COMMIT = "a7d6ac22d0078809b58aceb9a499a24ddee1672d"  # ruling 39: MR6's vendor until the 1.18 tag (re-vendored for rulings 46/46a)


def test_vendored_engine_is_1_18():
    text = (VENDORED / "VENDORED_VERSION.txt").read_text(encoding="utf-8")
    tag = re.search(r"^Tag:\s*v(\d+)\.(\d+)\.(\d+)", text, re.M)
    commit = re.search(r"^Commit:\s*([0-9a-f]{40})", text, re.M)
    on_tag = tag and (int(tag.group(1)), int(tag.group(2))) >= (1, 18)
    on_r17 = commit and commit.group(1) == R17_VENDOR_COMMIT
    assert on_tag or on_r17, "vendored %s at %s; want the v1.18 tag or a later one, or %s until it" % (
        tag and "v%s.%s.%s" % tag.groups(), commit and commit.group(1), R17_VENDOR_COMMIT)


def test_vendored_cost_table_is_measured():
    table = VENDORED / "include" / "superslm" / "gpu_rows_cost_table.h"
    assert table.is_file(), "the vendored engine has no gpu_rows_cost_table.h"
    code = code_of(table)
    match = re.search(r"\bkGpuRowsCostTableMeasured\s*=\s*(true|false)\b", code)
    assert match, "kGpuRowsCostTableMeasured not defined"
    assert match.group(1) == "true", "the vendored cost table is the estimate (N4)"


def test_setting_has_no_canonical():
    body = enum_body(code_of(SOURCE / "Public" / "SuperSLMSchemaJumpTypes.h"), "ESuperSLMSchemaJumpForward")
    assert enumerators(body) == ["Off", "Exact"], "ESuperSLMSchemaJumpForward is %s, want Off, Exact" % enumerators(body)
    # The surviving values keep their numbers: an old saved 0 or 1 still means what it meant.
    assert re.search(r"\bOff\s*=\s*0\b", body) and re.search(r"\bExact\s*=\s*1\b", body)


def test_core_mode_has_no_canonical():
    body = enum_body(code_of(SOURCE / "Private" / "Core" / "SuperSLMSchemaJumpCore.h"), "ESchemaJumpMode")
    assert enumerators(body) == ["Off", "Exact"], "ESchemaJumpMode is %s, want Off, Exact" % enumerators(body)


def test_no_code_names_the_canonical_enumerator():
    hits = []
    for path in sorted(PLUGIN.joinpath("Source").rglob("*")):
        if path.suffix not in (".h", ".cpp", ".inl") or "ThirdParty" in path.parts:
            continue
        code = code_of(path)
        for m in re.finditer(r"\b(?:ESuperSLMSchemaJumpForward|ESchemaJumpMode)::Canonical\b", code):
            hits.append("%s:%d" % (path.relative_to(PLUGIN), code.count("\n", 0, m.start()) + 1))
    assert not hits, "Canonical is still named in code: " + ", ".join(hits[:10])


def _plugin_code():
    for path in sorted(PLUGIN.joinpath("Source").rglob("*")):
        if path.suffix in (".h", ".cpp", ".inl") and "ThirdParty" not in path.parts:
            yield path, code_of(path)


def test_chooser_reads_the_vendored_measured_table():
    # Rulings 37 and 39 Q11: the plugin fills FChooserInput from the vendored engine's merged arrays
    # (superslm_gpu::kGpuRowsCostRatioPerMille, kGpuRowsLogitsRatioPerMille) and kGpuRowsCostTableMeasured, never from
    # a copy, and never from the per-model arrays (information only).
    names = {
        "cost": r"\bkGpuRowsCostRatioPerMille\b",
        "logits": r"\bkGpuRowsLogitsRatioPerMille\b",
        "provenance": r"\bkGpuRowsCostTableMeasured\b",
    }
    found = {k: [] for k in names}
    per_model = []
    for path, code in _plugin_code():
        for k, pat in names.items():
            if re.search(pat, code):
                found[k].append(path.name)
        if re.search(r"\bkGpuRows(?:Cost|Logits)RatioPerMille(?:05B|15B)\b", code):
            per_model.append(path.name)
    missing = [k for k, v in found.items() if not v]
    assert not missing, "no plugin code reads the vendored table's %s" % ", ".join(missing)
    assert not per_model, "the per-model arrays are information only (ruling 39 Q11): " + ", ".join(per_model)


def _config_code():
    return code_of(SOURCE / "Public" / "SuperSLMGpuRuntimeConfig.h")


def test_config_has_the_margin_knob():
    m = re.search(r"\bint32\s+GpuRowsMarginPerMille\s*=\s*(\d+)\s*;", _config_code())
    assert m, "FSuperSLMGpuRuntimeConfig has no `int32 GpuRowsMarginPerMille = <default>;` (ruling 39 Q9)"
    assert int(m.group(1)) == 150, "GpuRowsMarginPerMille defaults to %s, want §5.1's 150" % m.group(1)


def test_config_has_the_multirow_switch():
    m = re.search(r"\bbool\s+bGpuMultiRow\s*=\s*(true|false)\s*;", _config_code())
    assert m, "FSuperSLMGpuRuntimeConfig has no `bool bGpuMultiRow = <default>;` (ruling 39 Q9, §5.4)"
    assert m.group(1) == "true", "bGpuMultiRow defaults to false, want true (MR-D16)"


def test_config_has_the_calibration_switch():
    # Ruling 48 (E-13): load-time row-cost calibration, on by default.
    m = re.search(r"\bbool\s+bCalibrateGpuRowCost\s*=\s*(true|false)\s*;", _config_code())
    assert m, "FSuperSLMGpuRuntimeConfig has no `bool bCalibrateGpuRowCost = <default>;` (ruling 48)"
    assert m.group(1) == "true", "bCalibrateGpuRowCost defaults to false, want true (ruling 48)"


def test_config_row_cost_is_unset_by_default():
    # Ruling 48a (1): GpuRowCostMs 0 is unset (calibrate, or 6.2 ms with the switch off); above 0 is the game's value.
    m = re.search(r"\bdouble\s+GpuRowCostMs\s*=\s*([0-9.]+)\s*;", _config_code())
    assert m, "FSuperSLMGpuRuntimeConfig has no `double GpuRowCostMs = <default>;`"
    assert float(m.group(1)) == 0.0, "GpuRowCostMs defaults to %s, want 0 (unset, ruling 48a)" % m.group(1)


def test_subsystem_exposes_the_row_cost_in_use():
    # Ruling 48 (E-13): a public getter for the row cost in use (game value, calibrated price or 6.2 ms), in ms.
    code = code_of(SOURCE / "Public" / "SuperSLMGpuSubsystem.h")
    assert re.search(r"\bdouble\s+GetGpuRowCostMs\s*\(\s*\)\s*const\b", code), \
        "USuperSLMGpuSubsystem declares no `double GetGpuRowCostMs() const` (ruling 48)"


def test_config_keeps_the_millisecond_fields_double():
    code = _config_code()
    for name in ("SchemaJumpGpuFrameBudgetMs", "GpuRowCostMs", "SceneGpuShareMs", "NominalTickMs"):
        assert re.search(r"\bdouble\s+" + name + r"\b", code), "%s is no longer double (ruling 39 Q9)" % name


def test_trace_counters_exist():
    # §5.5 and ruling 39 Q4, Q6: the counters are named in plugin code (as trace counter names, so in literals).
    # Ruling 42a: an Exact step that falls back (no draft, or a failed open) is counted as ExactStepFellBack.
    # Ruling 48 (E-13): the calibrated row cost is the trace counter CalibratedGpuRowCostUs.
    text = "\n".join(code for _, code in _plugin_code())
    missing = [c for c in ("RowsPerChunk", "RowsSliceMs", "HostFinishUs", "ExactFellBackToOff", "ExactStepFellBack",
                                 "CalibratedGpuRowCostUs") if c not in text]
    assert not missing, "trace counters not named in plugin code: " + ", ".join(missing)


def test_no_copy_of_a_cost_table_in_plugin_code():
    # The estimates of MR4 and the measured arrays must not be pasted into the plugin: a copy goes stale on re-vendor.
    copies = [r"1000\s*,\s*1275\s*,\s*1565", r"1000\s*,\s*1140\s*,\s*1604", r"1000\s*,\s*1004\s*,\s*1221",
              r"250\s*,\s*319\s*,\s*392", r"48\s*,\s*95\s*,\s*122", r"39\s*,\s*81\s*,\s*122"]
    hits = [path.name for path, code in _plugin_code() if any(re.search(c, code) for c in copies)]
    assert not hits, "a cost table is copied into: " + ", ".join(hits)


def test_host_finish_is_not_a_chooser_input():
    # Ruling 39 Q6: host_finish is a trace counter only; the chooser takes no host-finish input, so it cannot enter
    # the GPU-frame plan.
    code = code_of(SOURCE / "Private" / "Core" / "SuperSLMGpuRowsChooser.h")
    assert not re.search(r"HostFinish", code, re.I), "the chooser names a host-finish input (ruling 39 Q6)"


def test_switch_refusal_logs_that_canonical_was_removed():
    # Ruling 39 Q7: `canonical` and `2` are refused with a log line saying Canonical was removed in 1.4. The line is a
    # string literal in plugin code outside the tests that names both.
    hits = []
    for path, code in _plugin_code():
        if "Tests" in path.parts:
            continue
        for lit in re.findall(r'"(?:\\.|[^"\\])*"', code):
            if re.search(r"canonical", lit, re.I) and re.search(r"removed", lit, re.I) and "1.4" in lit:
                hits.append(path.name)
    assert hits, "no log line says Canonical was removed in 1.4 (ruling 39 Q7)"


# Ruling 45 / ruling 49: the per-tick record's state is test-only everywhere; the engine's cumulative counter is read
# by product code only in E-13's calibration (FSuperSLMGpuSubsystemState::RunCalibrationStep, SuperSLMGpuSubsystem.cpp).
_PER_TICK = re.compile(r"\b(?:TickGpuRecords|TickGpuLock|bTickGpu\w*|TickGpuStart\w*|TestTickGpu\w*|bTestTickGpu\w*)\b")
_COUNTER = re.compile(r"\bCumulativeGpuBusyUs\b")
_CALIBRATION_FILE = "SuperSLMGpuSubsystem.cpp"
_CALIBRATION_STATE = "FSuperSLMGpuSubsystemState"
_CALIBRATION_FN = "RunCalibrationStep"
# Ruling 49 as amended by 59b point 4: the cold-start guard's feedback reads the counter around a tick job, for layer
# sizing only, and only in this named member.
_FEEDBACK_FN = "MeasureTickJobForLayerSizing"


def _code_keeping_lines(text):
    """code_of() for text, but a comment keeps its newlines, so line numbers and offsets stay the file's."""
    return _LEXEMES.sub(lambda m: m["lit"] if m["lit"] else re.sub(r"[^\n]", " ", m[0]), text)


def _outside_dev_tests(code):
    """(line number, line, offset of the line) for each line a build without WITH_DEV_AUTOMATION_TESTS compiles, by
    #if nesting: a level is test-only when its #if/#elif names WITH_DEV_AUTOMATION_TESTS positively (not negated), and
    its #else is not."""
    stack, out, offset = [], [], 0
    for n, line in enumerate(code.split("\n"), 1):
        at = offset
        offset += len(line) + 1
        d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if d:
            kind, cond = d.group(1), d.group(2)
            pos = "WITH_DEV_AUTOMATION_TESTS" in cond and not re.search(r"!\s*(?:defined\s*\(?\s*)?WITH_DEV_AUTOMATION_TESTS", cond)
            if kind in ("if", "ifdef"):
                stack.append(pos)
            elif kind == "ifndef":
                stack.append(False)
            elif kind == "elif" and stack:
                stack[-1] = pos
            elif kind == "else" and stack:
                stack[-1] = False
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if not any(stack):
            out.append((n, line, at))
    return out


def _brace_span(code, open_at):
    """[open_at, close + 1) of the brace block opening at open_at (strings and comments already blanked)."""
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return open_at, i + 1
    return open_at, len(code)


def _calibration_bodies(code):
    """The body spans of FSuperSLMGpuSubsystemState::RunCalibrationStep's (and, ruling 59b, MeasureTickJobForLayerSizing's)
    in the struct's own body (unqualified, inside `struct FSuperSLMGpuSubsystemState { ... }`)."""
    structs = []
    for m in re.finditer(r"\bstruct\s+" + _CALIBRATION_STATE + r"\b[^;{]*\{", code):
        structs.append(_brace_span(code, m.end() - 1))
    spans = []
    for m in re.finditer(r"(\b" + _CALIBRATION_STATE + r"\s*::\s*)?\b(?:" + _CALIBRATION_FN + "|" + _FEEDBACK_FN + r")\s*\(", code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            depth += {"(": 1, ")": -1}.get(code[i], 0)
            i += 1
        tail = re.match(r"[^;{}]*?\{", code[i:])  # qualifiers, then the body; a `;` first is a declaration or a call
        if not tail or re.search(r"[;=]", tail.group(0)):
            continue
        in_struct = any(a <= m.start() < b for a, b in structs)
        if m.group(1) or in_struct:
            spans.append(_brace_span(code, i + tail.end() - 1))
    return spans


def gpu_busy_hits(name, text):
    """Ruling 45 with ruling 49: the places in one source file where a non-test build reads the GPU-busy sum it may
    not. Per-tick record names: anywhere outside WITH_DEV_AUTOMATION_TESTS. CumulativeGpuBusyUs: outside it, except
    within RunCalibrationStep's body in SuperSLMGpuSubsystem.cpp."""
    code = _code_keeping_lines(text)
    allowed = _calibration_bodies(code) if name == _CALIBRATION_FILE else []
    hits = []
    for n, line, at in _outside_dev_tests(code):
        if _PER_TICK.search(line):
            hits.append("%s:%d (per-tick record)" % (name, n))
        for m in _COUNTER.finditer(line):
            if not any(a <= at + m.start() < b for a, b in allowed):
                hits.append("%s:%d (CumulativeGpuBusyUs outside %s::%s)" % (name, n, _CALIBRATION_STATE, _CALIBRATION_FN))
    return hits


def test_tick_gpu_busy_is_compiled_out_of_non_test_builds():
    # Ruling 45: the per-tick GPU-busy sum is test-only: every plugin-runtime line that keeps the per-tick records is
    # inside #if WITH_DEV_AUTOMATION_TESTS, so a shipping build neither computes it nor changes behaviour; the seam's
    # not-built value is the stub's (false, no records). Ruling 49: E-13's calibration reads the engine's cumulative
    # counter in product code, and only in FSuperSLMGpuSubsystemState::RunCalibrationStep's body -- and (ruling 59b point 4)
    hits = []
    for path in sorted((SOURCE / "Private").rglob("*")) + sorted((SOURCE / "Public").rglob("*")):
        if path.suffix not in (".cpp", ".h") or "Tests" in path.parts:
            continue
        hits += gpu_busy_hits(path.name, path.read_text(encoding="utf-8"))
    assert not hits, "the GPU-busy sum is compiled outside WITH_DEV_AUTOMATION_TESTS at: " + ", ".join(hits[:20])


_CONTROL = """
struct FSuperSLMGpuSubsystemState
{
	double RunCalibrationStep(int32 Slice) const
	{
		const double C0 = superslm_gpu::CumulativeGpuBusyUs(); // in the body: allowed
		if (Slice > 0) { return superslm_gpu::CumulativeGpuBusyUs() - C0; }
		return 0.0;
	}
	double OtherStep() { return superslm_gpu::CumulativeGpuBusyUs(); }
	void MeasureTickJobForLayerSizing(int32& Job) { Job = static_cast<int32>(superslm_gpu::CumulativeGpuBusyUs()); }
	void Plan() { RunCalibrationStep(1); }
};
double FSuperSLMGpuSubsystemState::OutOfLine() { return superslm_gpu::CumulativeGpuBusyUs(); }
#if WITH_DEV_AUTOMATION_TESTS
double TestOnly() { return superslm_gpu::CumulativeGpuBusyUs() + TickGpuRecords.Num(); }
#endif
"""


def test_tick_gpu_busy_check_control():
    # Ruling 49's control: the check passes a read inside RunCalibrationStep's body and a test-only read, and fails a
    # read in another member, a read out of line, a read in RunCalibrationStep elsewhere than SuperSLMGpuSubsystem.cpp,
    # a per-tick name even inside RunCalibrationStep, and an out-of-line RunCalibrationStep of another type.
    hits = gpu_busy_hits("SuperSLMGpuSubsystem.cpp", _CONTROL)
    lines = sorted(int(h.split(":")[1].split()[0]) for h in hits)
    assert lines == [10, 14], "the control's reads outside RunCalibrationStep and MeasureTickJobForLayerSizing: want lines 10 and 14 red, got %s" % hits
    assert len(gpu_busy_hits("SuperSLMQuery.cpp", _CONTROL)) == 5, "RunCalibrationStep's or MeasureTickJobForLayerSizing's reads are allowed in another file"
    per_tick = _CONTROL.replace("const double C0 = superslm_gpu::CumulativeGpuBusyUs();", "const double C0 = TestTickGpuMarkUs;")
    assert any("per-tick" in h for h in gpu_busy_hits("SuperSLMGpuSubsystem.cpp", per_tick)), "a per-tick name in RunCalibrationStep passed"
    other = "double FOther::RunCalibrationStep() { return superslm_gpu::CumulativeGpuBusyUs(); }\n"
    assert gpu_busy_hits("SuperSLMGpuSubsystem.cpp", other), "another type's RunCalibrationStep passed"
    qualified = "double FSuperSLMGpuSubsystemState::RunCalibrationStep(int32 S)\n{\n\treturn superslm_gpu::CumulativeGpuBusyUs();\n}\n"
    assert not gpu_busy_hits("SuperSLMGpuSubsystem.cpp", qualified), "the out-of-line RunCalibrationStep's read failed"
