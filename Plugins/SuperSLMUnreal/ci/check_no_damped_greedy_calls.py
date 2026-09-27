#!/usr/bin/env python3
"""L2-S0 guard G3 (the plan §9 dim 11; commissioned by
ci/tests/test_guard_g3_no_damped_greedy.py).

"The plugin's own greedy-only structural check ... the check runs in CI over the plugin's
whole source tree ... Mutation: plant a `sslm_decode_params_init` call site passing
`SSLM_DECODE_MODE_DAMPED_GREEDY` -- CI must go red."

Scans every source file under --source-root for a call to sslm_decode_params_init whose
argument list names SSLM_DECODE_MODE_DAMPED_GREEDY. This is a textual scan (matching the
mutation the plan's own gate text names, which is itself textual: "a call site passing
SSLM_DECODE_MODE_DAMPED_GREEDY"), not a C++ parse -- the plugin ships greedy mode only
(§2.3 RULING D-SLM3826), so a real call site naming the damped-greedy mode constant is
always a violation regardless of surrounding syntax.

Usage:
    python check_no_damped_greedy_calls.py --source-root <dir>

Exit 0   -- no violation found.
Exit !=0 -- at least one violation found; stdout names every offending file and line.
"""
import argparse
import re
import sys
from pathlib import Path

# A call to sslm_decode_params_init whose argument list contains
# SSLM_DECODE_MODE_DAMPED_GREEDY, anywhere between the opening paren of the call and the
# next statement terminator. Deliberately generous (matches across a line break) because the
# violation this guard exists to catch is the TOKEN appearing in a real call site's argument
# list, not a specific formatting.
VIOLATION_PATTERN = re.compile(
    r"sslm_decode_params_init\s*\([^;]*?SSLM_DECODE_MODE_DAMPED_GREEDY", re.DOTALL
)

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".inl"}

# T-2241 review C3: run over the plugin's real Source tree ("the check runs in CI over the
# plugin's whole source tree", plan §9 dim 11 G3), that tree now also contains the vendored
# Layer 1 source at Source/ThirdParty/SuperSLM -- including Layer 1's OWN damped-greedy red
# suite, which legitimately passes SSLM_DECODE_MODE_DAMPED_GREEDY as the thing under test.
# Layer 1's own call sites are not this plugin's call sites: G3 exists to guard THIS plugin's
# scheduler (§2.3 RULING D-SLM3826, greedy-only), never to re-police vendored third-party
# source the plugin does not author and never calls into for this purpose. Any path component
# literally named "ThirdParty" is excluded -- matching the plugin's own vendoring convention
# (Source/ThirdParty/<name>), not a narrower Layer-1-specific name, so the exclusion still
# holds if a second vendored dependency is ever added.
EXCLUDED_DIR_NAME = "ThirdParty"


def _is_excluded(path: Path, source_root: Path) -> bool:
    return EXCLUDED_DIR_NAME in path.relative_to(source_root).parts[:-1]


def scan(source_root: Path):
    violations = []
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        if _is_excluded(path, source_root):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in VIOLATION_PATTERN.finditer(text):
            line_no = text.count("\n", 0, match.start()) + 1
            violations.append((path, line_no))
    return violations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    args = parser.parse_args()

    if not args.source_root.is_dir():
        print(f"error: --source-root {args.source_root} is not a directory", file=sys.stderr)
        return 2

    violations = scan(args.source_root)
    if not violations:
        return 0

    for path, line_no in violations:
        print(f"{path}:{line_no}: call site passes SSLM_DECODE_MODE_DAMPED_GREEDY to "
              f"sslm_decode_params_init -- the plugin ships SSLM_DECODE_MODE_GREEDY only "
              f"(§2.3 RULING D-SLM3826)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
