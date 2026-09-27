#!/usr/bin/env python3
"""T-2241 review S4: "Nothing verifies the vendored tree is the pinned tag; a hand-edit passes
every cell." The prior state: test_thirdparty_vendoring.py's cell (a) checks that the
directory exists, that VENDORED_VERSION.txt names the right tag/commit strings, and that a
CMakeLists.txt is present -- never a single byte of the tree itself.

This script verifies every file listed in Source/ThirdParty/SuperSLM/VENDORED_MANIFEST.sha256
(generated at vendoring time from `git show <tag>:<path>` for every path in
`git ls-tree -r --name-only <tag>` -- the tag VENDORED_VERSION.txt and the manifest's own header
name, v1.9.0 at 1.0) still hashes to the recorded value, and that the manifest's own file set
matches what is actually on disk (an added, removed, or renamed file is caught the same as a
content edit).

Usage:
    python check_vendored_tree_matches_manifest.py --thirdparty-root <dir>

Exit 0   -- every manifested file present with a matching hash, and no extra/missing files.
Exit !=0 -- at least one mismatch; stdout names every offending file and the reason
            (content-changed / missing / unlisted-extra).
"""
import argparse
import hashlib
import sys
from pathlib import Path

MANIFEST_NAME = "VENDORED_MANIFEST.sha256"
EXCLUDED_FROM_TREE_SCAN = {"VENDORED_VERSION.txt", MANIFEST_NAME}


def _load_manifest(manifest_path: Path):
    entries = {}
    for line in manifest_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, _, rel = line.partition("  ")
        if not digest or not rel:
            continue
        entries[rel] = digest
    return entries


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def check(thirdparty_root: Path):
    manifest_path = thirdparty_root / MANIFEST_NAME
    if not manifest_path.is_file():
        return [f"{manifest_path} does not exist -- cannot verify the vendored tree against anything"]

    manifest = _load_manifest(manifest_path)
    on_disk = {
        p.relative_to(thirdparty_root).as_posix()
        for p in thirdparty_root.rglob("*")
        if p.is_file() and p.relative_to(thirdparty_root).as_posix() not in EXCLUDED_FROM_TREE_SCAN
    }

    problems = []
    for rel, expected_hash in sorted(manifest.items()):
        full = thirdparty_root / rel
        if not full.is_file():
            problems.append(f"MISSING: {rel} (listed in manifest, absent on disk)")
            continue
        actual_hash = _sha256(full)
        if actual_hash != expected_hash:
            problems.append(
                f"CONTENT CHANGED: {rel} (manifest {expected_hash}, on-disk {actual_hash})"
            )

    extra = sorted(on_disk - set(manifest.keys()))
    for rel in extra:
        problems.append(f"UNLISTED EXTRA: {rel} (present on disk, not in manifest -- re-vendor and regenerate the manifest)")

    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--thirdparty-root", required=True, type=Path)
    args = parser.parse_args()

    if not args.thirdparty_root.is_dir():
        print(f"error: --thirdparty-root {args.thirdparty_root} is not a directory", file=sys.stderr)
        return 2

    problems = check(args.thirdparty_root)
    if not problems:
        print(f"OK: vendored tree matches {MANIFEST_NAME}")
        return 0

    for p in problems:
        print(p)
    return 1


if __name__ == "__main__":
    sys.exit(main())
