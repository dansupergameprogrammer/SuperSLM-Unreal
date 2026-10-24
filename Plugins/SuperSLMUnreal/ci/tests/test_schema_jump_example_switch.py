"""Schema fast-decode plan P1.9 / mutant XD7 / code review round 1, finding 3: the example's scratch
switch -SuperSLMExampleJumpForward is wired, end to end, in source.

The Unreal cells (SuperSLM.SchemaJump.P1.SwitchParserIsPublic, SuperSLMExample.Demo.SchemaJumpForwardSwitch)
hold the parser's behaviour; this host cell holds the wiring they cannot see:
  - the plugin's public parser is the core parser's production caller;
  - the example reads the switch with FParse::Value, through the plugin's public parser, and logs a
    Warning naming the switch when it refuses a value;
  - the scene applies it to its settings, and the extraction request carries the setting.
Comments are stripped before every check, so a comment naming a call does not satisfy it.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
PLUGIN_SOURCE = REPO / "Plugins" / "SuperSLMUnreal" / "Source" / "SuperSLMUnreal"
EXAMPLE_SOURCE = REPO / "RepoDrafts" / "SuperSLMUnreal" / "ExampleProject" / "Source" / "ExampleProject"

# Comments become spaces; string and character literals are kept (a switch name is a literal).
_LEXEMES = re.compile(
    r'(?P<lit>(?:u8|u|U|L)?R"(?P<d>[^\s()\\]{0,16})\([\s\S]*?\)(?P=d)"'
    r'|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\')'
    r'|//[^\n]*|/\*[\s\S]*?\*/')


def without_comments(source):
    return _LEXEMES.sub(lambda m: m["lit"] if m["lit"] else " " * len(m[0]), source)


def code_of(path):
    return without_comments(path.read_text(encoding="utf-8"))


def body_of(code, signature):
    """The brace-balanced body of the first definition whose text matches signature (a regex ending
    before the opening brace), or None."""
    match = re.search(signature + r"\s*\{", code)
    if not match:
        return None
    depth, position = 1, match.end()
    while depth and position < len(code):
        depth += {"{": 1, "}": -1}.get(code[position], 0)
        position += 1
    return code[match.end():position - 1]


def is_test_source(path):
    return path.name.endswith("Tests.cpp") or "Tests" in path.parts or "TestAccess" in path.name


def production_sources(root):
    return [p for p in sorted(root.rglob("*.cpp")) if not is_test_source(p)]


def test_the_public_parser_is_the_core_parsers_production_caller():
    callers = []
    for path in production_sources(PLUGIN_SOURCE):
        body = body_of(code_of(path), r"\bbool\s+ParseSuperSLMSchemaJumpForwardSwitch\s*\([^)]*\)")
        if body is not None and re.search(r"\bParseSchemaJumpSwitch\s*\(", body):
            callers.append(path.name)
    assert callers, (
        "No production definition of ParseSuperSLMSchemaJumpForwardSwitch calls "
        "SuperSLMCore::ParseSchemaJumpSwitch (the stub in a *TestAccess* file does not count)")


def example_switch_body():
    code = code_of(EXAMPLE_SOURCE / "SuperSLMExampleDemo.cpp")
    body = body_of(code, r"\bbool\s+SchemaJumpForwardFromCommandLine\s*\([^)]*\)")
    assert body is not None, "SuperSLMExampleDemo.cpp defines no SchemaJumpForwardFromCommandLine"
    return body


def test_the_example_reads_the_switch_with_fparse_value():
    body = example_switch_body()
    assert re.search(r'FParse::Value\s*\([^;]*TEXT\s*\(\s*"SuperSLMExampleJumpForward="\s*\)', body), (
        'SchemaJumpForwardFromCommandLine does not read "SuperSLMExampleJumpForward=" with FParse::Value')


def test_the_example_parses_through_the_plugins_public_parser():
    assert re.search(r"\bParseSuperSLMSchemaJumpForwardSwitch\s*\(", example_switch_body()), (
        "SchemaJumpForwardFromCommandLine does not call the plugin's ParseSuperSLMSchemaJumpForwardSwitch")


def test_a_refused_value_logs_a_warning_naming_the_switch():
    body = example_switch_body()
    logs = re.findall(r"UE_LOG\s*\(\s*\w+\s*,\s*Warning\s*,([^;]*)\)\s*;", body)
    assert any("SuperSLMExampleJumpForward" in log for log in logs), (
        "SchemaJumpForwardFromCommandLine has no UE_LOG(..., Warning, ...) naming -SuperSLMExampleJumpForward")


def test_the_scene_applies_the_switch_to_its_settings():
    hits = []
    for path in production_sources(EXAMPLE_SOURCE):
        code = code_of(path)
        if re.search(r"\bSchemaJumpForwardFromCommandLine\s*\(\s*FCommandLine::Get\s*\(\s*\)\s*,[^;]*SchemaJumpForward\s*\)", code):
            hits.append(path.name)
    assert hits, ("No production source calls SchemaJumpForwardFromCommandLine(FCommandLine::Get(), "
                  "<settings>.SchemaJumpForward)")


def test_the_extraction_request_carries_the_setting():
    code = code_of(EXAMPLE_SOURCE / "SuperSLMExampleDemo.cpp")
    body = body_of(code, r"\bbool\s+FSuperSLMExampleDemo::TryStartExtraction\s*\([^)]*\)")
    assert body is not None, "FSuperSLMExampleDemo::TryStartExtraction not found"
    assert re.search(r"\bRequest\.SchemaJumpForward\s*=\s*[^;]*\bSchemaJumpForward\b", body), (
        "TryStartExtraction's FSuperSLMGenerationRequest never sets SchemaJumpForward from the settings")
