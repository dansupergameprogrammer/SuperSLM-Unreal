#!/usr/bin/env python3
"""Prints the body of the top entry of a CHANGELOG.md (the text between the first '## ' heading
and the next one), for use as GitHub release notes.

Usage:
    python changelog_notes.py CHANGELOG.md > notes.md

Exits non-zero if the file has no '## ' entry or the top entry's body is empty.
"""
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
    body, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = True
            continue
        if inside:
            body.append(line)
    text = "\n".join(body).strip()
    if not text:
        print(f"no top entry with a body in {sys.argv[1]}", file=sys.stderr)
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
