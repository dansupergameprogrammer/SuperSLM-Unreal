#!/usr/bin/env python3
"""L2-S0 guard G4 (the plan §9 dim 11; commissioned by
ci/tests/test_guard_g4_no_forced_simd_tier.py).

"The never-forced-SIMD-tier check ... D-SLM3833 -- the determinism guarantee the plugin
inherits is Layer 1's cross-tier bit-identity, which holds only while the plugin leaves the
runtime dispatch alone ... Mutation: plant a SUPERSLM_FORCE_AVX2_MATMUL definition in the
plugin's build -- CI must go red."

Scans every *.Build.cs under --source-root for a preprocessor definition (PublicDefinitions
or PrivateDefinitions) naming a SUPERSLM_FORCE_* symbol -- the family Layer 1's own
CMakeLists.txt uses to pin a matmul kernel tier for its own forced-tier test/digest binaries
(SUPERSLM_FORCE_SSE2_MATMUL / SUPERSLM_FORCE_AVX2_MATMUL / SUPERSLM_FORCE_AVX512_MATMUL).
This plugin must never define any of them: doing so would defeat the runtime CPUID+XGETBV
tier dispatch the plugin's own cross-tier determinism claim depends on (§2.1, D-SLM3833).

Usage:
    python check_no_forced_simd_tier.py --source-root <dir>

Exit 0   -- no *.Build.cs under <dir> defines a SUPERSLM_FORCE_* symbol.
Exit !=0 -- at least one such definition found; stdout names the file and the symbol.
"""
import argparse
import re
import sys
from pathlib import Path

VIOLATION_PATTERN = re.compile(r"SUPERSLM_FORCE_[A-Za-z0-9_]+")

# Strip // line comments and /* */ block comments before scanning, so a Build.cs whose
# COMMENTS discuss the SUPERSLM_FORCE_* family (as this guard's own clean fixture and the
# plugin's real Build.cs both do, documenting why the plugin never defines one) are not
# mistaken for the real preprocessor definition this guard exists to catch.
COMMENT_PATTERN = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def _strip_comments(text: str) -> str:
    # Replace comment characters with spaces (not empty string) so byte/line offsets used for
    # line-number reporting stay aligned with the original file.
    return COMMENT_PATTERN.sub(lambda m: " " * len(m.group(0)), text)


def scan(source_root: Path):
    violations = []
    for path in sorted(source_root.rglob("*.Build.cs")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        code_only = _strip_comments(text)
        for match in VIOLATION_PATTERN.finditer(code_only):
            line_no = code_only.count("\n", 0, match.start()) + 1
            violations.append((path, line_no, match.group(0)))
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

    for path, line_no, symbol in violations:
        print(f"{path}:{line_no}: {symbol} defined -- this plugin must never force a Layer-1 "
              f"matmul SIMD tier (§2.1, D-SLM3833: the cross-tier bit-identity determinism "
              f"claim holds only while the plugin leaves the runtime CPUID dispatch alone)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
