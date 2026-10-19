"""Builder regressions for G3/G4's lexical scope, through their actual CLIs.

These supplement the original commissioning fixtures; they are not an
independently authored commissioning population or a universal C++ parser test.
"""
from pathlib import Path
import subprocess
import sys

import pytest

CI = Path(__file__).resolve().parents[1]
GREEDY = "SSLM_DECODE_MODE_GREEDY"
DAMPED = "SSLM_DECODE_MODE_DAMPED_GREEDY"


def run_guard(tmp_path, guard, relative, source):
    path = tmp_path / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source, encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-B", str(CI / guard), "--source-root", str(tmp_path)],
        capture_output=True, text=True, timeout=30,
    )
    return path, result


@pytest.mark.parametrize("suffix", ["cpp", "h", "hpp", "inl", "cc", "cxx"])
@pytest.mark.parametrize("body", [
    f"sslm_decode_params_init /* separation */ (m, /* mode */ {DAMPED}, 1, &p);",
    f"sslm_decode_params_init(m, {GREEDY}, 1, &p); p.mode = {DAMPED};",
    f"constexpr int selected = {DAMPED}; sslm_decode_params_init(m, selected, 1, &p);",
])
def test_g3_rejects_real_call_or_mode_write_in_every_source_suffix(tmp_path, suffix, body):
    path, result = run_guard(tmp_path, "check_no_damped_greedy_calls.py", f"Runtime/Call.{suffix}",
                             "void f() { sslm_decode_params p{}; " + body + " }")
    assert result.returncode == 1, result.stdout + result.stderr
    assert str(path) in result.stdout and DAMPED in result.stdout


@pytest.mark.parametrize("body, expected", [
    (f"constexpr int outer = {GREEDY}; const auto inner = (outer); "
     "sslm_decode_params_init(m, static_cast<int>(inner), 1, &p);", 0),
    (f"constexpr int outer = {DAMPED}; const auto inner = outer; "
     "sslm_decode_params_init(m, inner, 1, &p);", 1),
    (f"constexpr int selected = {GREEDY}; {{ int selected = unknown; "
     "sslm_decode_params_init(m, selected, 1, &p); }", 1),
    (f"typedef int LocalMode; constexpr int selected = {GREEDY}; "
     f"{{ LocalMode selected = {DAMPED}; sslm_decode_params_init(m, selected, 1, &p); }}", 1),
    (f"constexpr int selected = {GREEDY}; "
     f"{{ unsigned long selected = {DAMPED}; sslm_decode_params_init(m, selected, 1, &p); }}", 1),
    (f"constexpr int selected = {GREEDY}; "
     f"{{ decltype(1) selected = {DAMPED}; sslm_decode_params_init(m, selected, 1, &p); }}", 1),
    (f"constexpr int selected = {GREEDY}; {{ constexpr int selected = {DAMPED}; }} "
     "sslm_decode_params_init(m, selected, 1, &p);", 0),
    ("sslm_decode_params_init(m, UnresolvedMode(), 1, &p);", 1),
    (f"sslm_decode_params_init(m, {GREEDY}, 1, &p); p.mode |= 1;", 1),
    (f"sslm_decode_params_init(m, {GREEDY}, 1, &p); p.mode++;", 1),
    (f"sslm_decode_params_init(m, {GREEDY}, 1, &p); ++p.mode;", 1),
    (f"sslm_decode_params_init(m, {GREEDY}, 1, &p); p.mode = GetMode();", 1),
    (f"sslm_decode_params_init(m, {GREEDY}, 1, &p); p.mode = {GREEDY};", 0),
    (f"sslm_decode_params* alias = &p; alias->mode = {DAMPED};", 1),
    (f"if (status == SSLM_UNSUPPORTED) Refuse({DAMPED});", 0),
])
def test_g3_local_aliases_scopes_and_unknowns(tmp_path, body, expected):
    _, result = run_guard(tmp_path, "check_no_damped_greedy_calls.py", "Runtime/Call.cpp",
                          "void f() { sslm_decode_params p{}; " + body + " }")
    assert result.returncode == expected, result.stdout + result.stderr


def test_g3_opaque_literals_comments_and_physical_line_after_splice(tmp_path):
    source = (f'const char* text = R"tag(sslm_decode_params_init(m, {DAMPED}, 1, &p);)tag";\n'
              f'// sslm_decode_params_init(m, {DAMPED}, 1, &p);\n'
              f'/* sslm_decode_params_init(m, {DAMPED}, 1, &p); */\n')
    _, result = run_guard(tmp_path, "check_no_damped_greedy_calls.py", "Runtime/Call.cpp", source)
    assert result.returncode == 0, result.stdout + result.stderr
    path, result = run_guard(tmp_path, "check_no_damped_greedy_calls.py", "Runtime/Call.cpp",
                             source + "void f() {\nsslm_decode_params_" + "\\\n" +
                             f"init(m, {DAMPED}, 1, &p);\n}}\n")
    assert result.returncode == 1 and f"{path}:5:" in result.stdout


@pytest.mark.parametrize("source", [
    '#define SUPERSLM_FORCE_SCALAR_MATMUL 1\n',
    '# define /* separation */ SUPERSLM_FORCE_AVX2_MATMUL 1\n',
    '#define SUPERSLM_FORCE_AVX2_' + '\\\n' + 'MATMUL 1\n',
    '#define SUPERSLM_FORCE_AVX512_MATMUL 0\n',
])
def test_g4_refuses_actual_wrapper_definitions_even_zero(tmp_path, source):
    _, result = run_guard(tmp_path, "check_no_forced_simd_tier.py", "Runtime/Vendored/Matmul.cpp",
                          source + '#include "../../../ThirdParty/SuperSLM/src/matmul.cpp"\n')
    assert result.returncode == 1, result.stdout + result.stderr
    assert "SUPERSLM_FORCE_" in result.stdout


@pytest.mark.parametrize("source, expected", [
    ('PublicDefinitions.Add("SUPERSLM_FORCE_AVX2_MATMUL=0");', 1),
    ('PrivateDefinitions /* gap */ . Add ("SUPERSLM_FORCE_" + "SSE2_MATMUL=1");', 1),
    ('PublicDefinitions.AddRange(new string[] { "NORMAL=1", "SUPERSLM_FORCE_SCALAR_MATMUL" });', 1),
    ('PublicDefinitions.AddRange(new[] { "NORMAL=1", "OTHER=" + DynamicValue() });', 0),
    ('PrivateDefinitions.Add("NORMAL=" + DynamicValue());', 0),
    ('PrivateDefinitions.Add(@"NORMAL=abc""def");', 0),
    ('PrivateDefinitions.Add("SUPERSLM_FORCE_" + DynamicName());', 1),
    ('PrivateDefinitions.Add(DynamicDefinition());', 1),
    ('PrivateDefinitions.Add("NORMAL=1".Replace("NORMAL", "SUPERSLM_FORCE_AVX2_MATMUL"));', 1),
    ('PublicDefinitions.AddRange(DynamicDefinitions());', 1),
    ('PublicDefinitions.AddRange(new[] { "UNRELATED=1" }.Select(x => "SUPERSLM_FORCE_AVX2_MATMUL=1"));', 1),
    ('PublicDefinitions.AddRange(Transform(new[] { "UNRELATED=1" }));', 1),
    ('// PublicDefinitions.Add("SUPERSLM_FORCE_AVX2_MATMUL=1");\n'
     'var refusal = @"SUPERSLM_FORCE_AVX2_MATMUL must remain undefined";\n'
     'PrivateDefinitions.Add("NORMAL=1");', 0),
])
def test_g4_build_definition_arguments_and_dynamic_name_refusal(tmp_path, source, expected):
    _, result = run_guard(tmp_path, "check_no_forced_simd_tier.py", "Editor/Test.Build.cs", source)
    assert result.returncode == expected, result.stdout + result.stderr


@pytest.mark.parametrize("guard", ["check_no_damped_greedy_calls.py", "check_no_forced_simd_tier.py"])
def test_opaque_references_and_thirdparty_are_not_symbol_bans(tmp_path, guard):
    (tmp_path / "ThirdParty").mkdir()
    (tmp_path / "ThirdParty/Core.cpp").write_text(
        f"#define SUPERSLM_FORCE_AVX2_MATMUL 1\nsslm_decode_params_init(m, {DAMPED}, 1, &p);",
        encoding="utf-8")
    _, result = run_guard(tmp_path, guard, "Runtime/Refusal.cpp",
                          f'const char* reason = R"(#define SUPERSLM_FORCE_AVX2_MATMUL 1\n'
                          f'sslm_decode_params_init(m, {DAMPED}, 1, &p);)";\n'
                          '// SUPERSLM_FORCE_AVX2_MATMUL is refused\n')
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("guard", ["check_no_damped_greedy_calls.py", "check_no_forced_simd_tier.py"])
def test_invalid_input_is_not_a_policy_rejection(tmp_path, guard):
    command = [sys.executable, "-B", str(CI / guard), "--source-root"]
    missing = subprocess.run(command + [str(tmp_path / "missing")], capture_output=True, text=True, timeout=30)
    assert missing.returncode == 2 and "not a directory" in missing.stderr
    (tmp_path / "Unreadable.cpp").write_bytes(b"\xff\xfe\xfd")
    invalid = subprocess.run(command + [str(tmp_path)], capture_output=True, text=True, timeout=30)
    assert invalid.returncode == 2 and "error reading source" in invalid.stderr
