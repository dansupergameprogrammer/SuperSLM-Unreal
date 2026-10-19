#!/usr/bin/env python3
"""G4: plugin-owned definitions must leave Layer-1 CPU tier dispatch automatic.

Check PublicDefinitions/PrivateDefinitions Add and literal-array AddRange initializers
in Build.cs, plus real #define directives in C/C++ sources/headers, excluding
ThirdParty. Defined-as0 still forces matmul: Layer1 tests defined(...).
Comments and opaque literals outside build-definition arguments are allowed.
Literal prefixes/concatenations are supported; unknown build-definition names
fail closed. This is lexical policy, not arbitrary C#/C++ evaluation or an
interprocedural include/preprocessor evaluator; conditional source defines are
also refused. Exit0 clean, exit1 violation, exit2 invalid/unreadable input.
"""
import argparse
from bisect import bisect_right
import json
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".inl"}
SYMBOL = re.compile(r"SUPERSLM_FORCE_[A-Za-z0-9_]+")
LEXEMES = re.compile(
    r'(?P<literal>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|@"(?:""|[^"])*"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\')'
    r'|(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)')
DEFINITION_CALL = re.compile(r"\b(?:Public|Private)Definitions\s*\.\s*(?P<method>Add|AddRange)\s*\(")
ARRAY_PREFIX = re.compile(r"new\s*(?:\[\s*\]|(?:string|(?:global::)?System\s*\.\s*String)\s*\[\s*\])")
DEFINE = re.compile(r"(?m)^[ \t]*#[ \t]*define[ \t]+(?P<name>SUPERSLM_FORCE_[A-Za-z0-9_]+)\b")


def _source(raw):
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
    blank = lambda match: "".join("\n" if c == "\n" else " " for c in match[0])
    code = LEXEMES.sub(blank, text)
    comments_removed = LEXEMES.sub(lambda m: blank(m) if m["comment"] is not None else m[0], text)
    return code, comments_removed, line


def _closing(code, opening):
    stack = []
    for position in range(opening, len(code)):
        char = code[position]
        if char in "([{":
            stack.append({"(": ")", "[": "]", "{": "}"}[char])
        elif char in ")]}":
            if not stack or char != stack.pop():
                return None
            if not stack:
                return position
    return None


def _definition(expression):
    """Return macro name from a literal prefix; None means not provable."""
    expression = expression.strip()
    prefix = ""
    while expression:
        match = LEXEMES.match(expression)
        if match is None or match["literal"] is None:
            break
        literal = match[0]
        if literal.startswith('@"'):
            prefix += literal[2:-1].replace('""', '"')
        elif literal.startswith('"'):
            try:
                prefix += json.loads(literal)
            except ValueError:
                return None
        else:
            return None
        expression = expression[match.end():].strip()
        if not expression.startswith("+"):
            if expression:
                # A method/indexer/operator can change the entire literal,
                # including its name; only concatenation preserves a prefix.
                return None
            break
        expression = expression[1:].strip()
    if "=" in prefix:
        name = prefix.split("=", 1)[0].strip()
    elif not expression:
        name = prefix.strip()
    else:
        return None
    return name if re.fullmatch(r"[A-Za-z_]\w*", name) else None


def _build_definitions(code, text):
    for match in DEFINITION_CALL.finditer(code):
        end = _closing(code, match.end() - 1)
        if end is None:
            yield match.start(), "unresolved SUPERSLM_FORCE_* build definition"
            continue
        low, high = match.end(), end
        if match["method"] == "AddRange":
            brace = code.find("{", low, high)
            closing = _closing(code, brace) if brace != -1 else None
            if (closing is None or closing >= high
                    or ARRAY_PREFIX.fullmatch(text[low:brace].strip()) is None
                    or text[closing + 1:high].strip()):
                # A wrapper or LINQ/container transform can change every
                # definition. Only a bare literal array preserves these items.
                yield match.start(), "unresolved SUPERSLM_FORCE_* build definition"
                continue
            low, high = brace + 1, closing
        start, depth, items = low, [], []
        for position in range(low, high):
            char = code[position]
            if char in "([{":
                depth.append(char)
            elif char in ")]}":
                if depth:
                    depth.pop()
            elif char == "," and not depth:
                items.append((start, position))
                start = position + 1
        items.append((start, high))
        for start, stop in items:
            if not text[start:stop].strip():
                continue
            name = _definition(text[start:stop])
            if name is None or SYMBOL.fullmatch(name):
                yield start, name or "unresolved SUPERSLM_FORCE_* build definition"


def scan(source_root: Path):
    violations = []
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or "ThirdParty" in path.relative_to(source_root).parts[:-1]:
            continue
        is_build = path.name.endswith(".Build.cs")
        if not is_build and path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        code, text, line = _source(path.read_text(encoding="utf-8-sig"))
        if is_build:
            violations.extend((path, line(position), name) for position, name in _build_definitions(code, text))
        else:
            violations.extend((path, line(m.start()), m["name"]) for m in DEFINE.finditer(code))
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
    for path, line_no, symbol in violations:
        print(f"{path}:{line_no}: {symbol} defined or not provably absent -- this plugin must never force a Layer-1 "
              "matmul SIMD tier (§2.1, D-SLM3833; leave runtime CPUID dispatch automatic)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
