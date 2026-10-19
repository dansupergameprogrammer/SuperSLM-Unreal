#!/usr/bin/env python3
"""G3: plugin-owned CPU decode parameters must remain greedy.

Scan C/C++ source and headers, excluding ThirdParty. Recognize the current
sslm_decode_params_init ABI, visible local const/constexpr mode aliases, and
mode writes on declared/initialized sslm_decode_params objects. Comments,
strings and raw strings are opaque; preprocessing line splices are removed.
Unknown initializer modes or parameter-mode writes fail closed. This is a
lexical/local-constant policy check, not interprocedural C++ data-flow analysis
or conditional-preprocessor evaluation. Refusal/status references are allowed.

Usage: python check_no_damped_greedy_calls.py --source-root <dir>
Exit0: clean; exit1: policy violation; exit2: invalid/unreadable input.
"""
import argparse
from bisect import bisect_right
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".inl"}
EXCLUDED_DIR_NAME = "ThirdParty"
LEXEMES = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    r'|//[^\n]*|/\*[\s\S]*?\*/')
INIT = re.compile(r"\bsslm_decode_params_init\s*\(")
CONSTANT = re.compile(
    r"\b(?:constexpr|const)\s+(?:[A-Za-z_]\w*(?:::\w+)*(?:\s*[*&])?\s+)+"
    r"(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<value>[^;{}]+);")
# A declaration of any local type can shadow a visible constant. Resolve only
# const/constexpr values, but block mutable/typedef/auto shadows as unknown.
LOCAL = re.compile(
    r"\b(?P<type>[A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;{}()]+>)?|decltype\s*\([^;{}]+\))"
    r"(?:\s+[*&]?\s*|\s*[*&]\s*)(?P<name>[A-Za-z_]\w*)\s*(?=[=;,{)])")
NON_TYPES = {"return", "co_return", "throw", "co_await", "delete", "sizeof", "alignof",
             "goto", "namespace", "class", "struct", "enum", "union", "using"}
PARAMETER = re.compile(r"\bsslm_decode_params\b\s*(?:(?:const|volatile)\s+)*[*&]?\s*([A-Za-z_]\w*)")
MODE_WRITE = re.compile(
    r"\b(?P<object>[A-Za-z_]\w*(?:(?:\s*\.|\s*->)\s*[A-Za-z_]\w*)*)"
    r"\s*(?:\.|->)\s*mode\s*(?P<op>=(?!=)|[+*/%|&^\-]=|<<=|>>=|\+\+|--)\s*(?P<value>[^;]*);")
PREFIX_MODE_WRITE = re.compile(
    r"(?:\+\+|--)\s*(?P<object>[A-Za-z_]\w*(?:(?:\s*\.|\s*->)\s*[A-Za-z_]\w*)*)"
    r"\s*(?:\.|->)\s*mode\b")


def _source(raw):
    """Splice first, retaining original physical line numbers for diagnostics."""
    chunks, positions, removed, cursor, length = [], [], 0, 0, 0
    for match in re.finditer(r"\\\r?\n", raw):
        part = raw[cursor:match.start()]
        chunks.append(part)
        length += len(part)
        removed += len(match[0])
        positions.append((length, removed))
        cursor = match.end()
    chunks.append(raw[cursor:])
    starts = [position for position, _ in positions]
    newlines = [i for i, char in enumerate(raw) if char == "\n"]

    def line(position):
        index = bisect_right(starts, position)
        original = position + (positions[index - 1][1] if index else 0)
        return bisect_right(newlines, original) + 1

    text = "".join(chunks)
    code = LEXEMES.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), text)
    return code, line


def _arguments(code, opening):
    stack, start, result = [")"], opening + 1, []
    for position in range(start, len(code)):
        char = code[position]
        if char in "([{":
            stack.append({"(": ")", "[": "]", "{": "}"}[char])
        elif char in ")]}":
            if not stack or char != stack.pop():
                return None
            if not stack:
                result.append(code[start:position].strip())
                return result
        elif char == "," and len(stack) == 1:
            result.append(code[start:position].strip())
            start = position + 1
    return None


def _scope_lookup(code):
    """Index brace scopes once: whole-source scanning must stay linear/log-linear."""
    stack, positions, contexts = [], [0], [()]
    for index, char in enumerate(code):
        if char == "{":
            stack.append(index)
        elif char == "}" and stack:
            stack.pop()
        else:
            continue
        positions.append(index + 1)
        contexts.append(tuple(stack))
    return lambda position: contexts[bisect_right(positions, position) - 1]


def _visible(declaration, use):
    return use[:len(declaration)] == declaration


def _unparenthesize(expression):
    expression = expression.strip()
    while expression.startswith("(") and expression.endswith(")"):
        arguments = _arguments(expression, 0)
        if arguments is None or len(arguments) != 1 or arguments[0] != expression[1:-1].strip():
            break
        expression = expression[1:-1].strip()
    return expression


def _violations(code):
    scope_at = _scope_lookup(code)
    bindings = [(m["name"], None, m.start("name"), scope_at(m.start("name")))
                for m in LOCAL.finditer(code) if m["type"] not in NON_TYPES]
    for match in CONSTANT.finditer(code):
        position = match.start("name")
        bindings = [b for b in bindings if b[2] != position]
        bindings.append((match["name"], match["value"], position, scope_at(position)))
    bindings.sort(key=lambda b: b[2])

    def mode(expression, position, seen=()):
        expression = _unparenthesize(expression)
        if expression == "SSLM_DECODE_MODE_GREEDY":
            return 0
        if expression == "SSLM_DECODE_MODE_DAMPED_GREEDY":
            return 1
        if re.fullmatch(r"0[uUlL]*", expression):
            return 0
        cast = re.fullmatch(r"static_cast\s*<[^<>]+>\s*\((.*)\)", expression, re.DOTALL)
        if cast:
            return mode(cast[1], position, seen)
        cast = re.fullmatch(r"\(\s*(?:sslm_decode_mode|int|int32_t|uint32_t)\s*\)\s*(.+)", expression, re.DOTALL)
        if cast:
            return mode(cast[1], position, seen)
        if re.fullmatch(r"[A-Za-z_]\w*", expression) and expression not in seen:
            context = scope_at(position)
            for name, value, declared, scope in reversed(bindings):
                if name == expression and declared < position and _visible(scope, context):
                    return mode(value, declared, seen + (name,)) if value is not None else None
        return None

    bad = set()
    parameters = [(m[1], m.start(), scope_at(m.start())) for m in PARAMETER.finditer(code)]
    for match in INIT.finditer(code):
        arguments = _arguments(code, match.end() - 1)
        if arguments is None or len(arguments) != 4 or mode(arguments[1], match.start()) != 0:
            bad.add(match.start())
        if arguments and len(arguments) == 4:
            target = _unparenthesize(arguments[3])
            if target.startswith("&"):
                target = re.sub(r"\s+", "", _unparenthesize(target[1:]))
                parameters.append((target, match.start(), scope_at(match.start())))
    for match in MODE_WRITE.finditer(code):
        target = re.sub(r"\s+", "", match["object"])
        context = scope_at(match.start())
        if any(name == target and declared < match.start() and _visible(scope, context)
               for name, declared, scope in parameters):
            if match["op"] != "=" or mode(match["value"], match.start()) != 0:
                bad.add(match.start())
    for match in PREFIX_MODE_WRITE.finditer(code):
        target = re.sub(r"\s+", "", match["object"])
        context = scope_at(match.start())
        if any(name == target and declared < match.start() and _visible(scope, context)
               for name, declared, scope in parameters):
            bad.add(match.start())
    return sorted(bad)


def scan(source_root: Path):
    violations = []
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if EXCLUDED_DIR_NAME in path.relative_to(source_root).parts[:-1]:
            continue
        code, line = _source(path.read_text(encoding="utf-8-sig"))
        violations.extend((path, line(position)) for position in _violations(code))
    return violations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    args = parser.parse_args()
    if not args.source_root.is_dir():
        print(f"error: --source-root {args.source_root} is not a directory", file=sys.stderr)
        return 2
    try:
        violations = scan(args.source_root)
    except (OSError, UnicodeError) as error:
        print(f"error reading source: {error}", file=sys.stderr)
        return 2
    for path, line_no in violations:
        print(f"{path}:{line_no}: decode mode is not provably SSLM_DECODE_MODE_GREEDY; "
              "SSLM_DECODE_MODE_DAMPED_GREEDY is prohibited by §2.3 RULING D-SLM3826")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
