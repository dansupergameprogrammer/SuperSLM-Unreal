"""Unreal executes report-tree leaves; a test cannot also name a parent node."""
import json
import re
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[2] / "Source"
LEXEMES = re.compile(
    r'(?:u8|u|U|L)?R"(?P<d>[^\s()\\]{0,16})\([\s\S]*?\)(?P=d)"'
    r'|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    r'|//[^\n]*|/\*[\s\S]*?\*/')
MACRO = re.compile(r"\bIMPLEMENT_(CUSTOM_SIMPLE|SIMPLE)_AUTOMATION_TEST\s*\(")


def arguments_at(source, position):
    start, depth, arguments = position, 1, []
    while depth:
        assert position < len(source), "Unterminated automation registration"
        token = LEXEMES.match(source, position)
        if token:
            position = token.end()
            continue
        char = source[position]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        if (char == "," and depth == 1) or not depth:
            arguments.append(source[start:position].strip())
            start = position + 1
        position += 1
    return arguments, position


def code_only(source):
    return LEXEMES.sub(lambda m: " " * len(m[0]), source)


def expand_registration_factories(source):
    definitions = list(re.finditer(r"^[ \t]*#define[ \t]+(\w+)\(([^)]*)\)[ \t]*(.*)$", source, re.M))
    factories = {m[1]: (m[2].split(","), m[3]) for m in definitions if MACRO.search(m[3])}
    if not factories:
        return source
    for match in reversed(definitions):
        if match[1] in factories:
            source = source[:match.start()] + " " * len(match[0]) + source[match.end():]
    call = re.compile(r"\b(" + "|".join(map(re.escape, factories)) + r")\s*\(")
    while (match := call.search(code_only(source))) is not None:
        actual, end = arguments_at(source, match.end())
        parameters, body = factories[match[1]]
        assert len(actual) == len(parameters), "Registration factory argument mismatch"
        values = dict(zip((parameter.strip() for parameter in parameters), actual))

        def substitute(fragment):
            def replacement(match):
                if match[1] is not None:
                    return json.dumps(values[match[1]]) if match[1] in values else match[0]
                return values.get(match[0], match[0])

            return re.sub(r"(?<!#)#\s*([A-Za-z_]\w*)|\b[A-Za-z_]\w*\b", replacement, fragment)

        chunks, start = [], 0
        for literal in LEXEMES.finditer(body):
            chunks.extend((substitute(body[start:literal.start()]), literal[0]))
            start = literal.end()
        chunks.append(substitute(body[start:]))
        expanded = re.sub(r"\s*##\s*", "", "".join(chunks))
        source = source[:match.start()] + expanded + source[end:]
    return source


def registrations(source):
    source = re.sub(r"\\\r?\n", "", source)
    source = LEXEMES.sub(lambda m: " " * len(m[0]) if m[0].startswith(("//", "/*")) else m[0], source)
    source = expand_registration_factories(source)
    for match in MACRO.finditer(code_only(source)):
        arguments, _ = arguments_at(source, match.end())
        index = 2 if match[1] == "CUSTOM_SIMPLE" else 1
        literals = re.findall(r'"(?:\\.|[^"\\])*"', arguments[index])
        assert literals, "Automation registration needs an auditable literal name"
        residue = re.sub(r'"(?:\\.|[^"\\])*"|\s+', "", arguments[index])
        assert residue in {"", "TEXT()"}, "Unexpanded registration name: " + arguments[index]
        name = "".join(json.loads(value) for value in literals)
        flags = arguments[-1]
        channel = "Product" if "ProductFilter" in flags else "Stress" if "StressFilter" in flags else "Other"
        context = "Editor" if "EditorContext" in flags else "Client" if "ClientContext" in flags else "Other"
        yield name, channel, context


def parent_collisions(entries):
    names = set(entries)
    return sorted((parent, child) for parent, channel, context in names
                  for child, other_channel, other_context in names
                  if child.startswith(parent + "."))


def test_registered_editor_tests_remain_executable_leaves():
    entries = []
    roots = [SOURCE]
    mcp = SOURCE.parents[1] / "SuperSLMUnrealMCP/Source"
    if mcp.is_dir():
        roots.append(mcp)
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() in {".cpp", ".h", ".hpp", ".inl"} and "ThirdParty" not in path.parts:
                entries.extend(registrations(path.read_text(encoding="utf-8-sig")))
    assert entries, "Automation registration inputs are missing"
    assert not parent_collisions(entries), "Unreal hides non-leaf tests: " + repr(parent_collisions(entries))
