#!/usr/bin/env python3
"""Vendored-source coherence: the committed SuperSLM tree must be the pinned upstream tag.

The plugin compiles SuperSLM's sources from a vendored copy. That copy is only trustworthy if it
is byte-for-byte the release tag it claims to be. This script checks three things, reading the
COMMITTED tree (git blobs), never the working directory, so line-ending conversion on checkout
cannot make a correct tree look wrong or a wrong one look right:

  1. VENDORED_VERSION.txt names a tag and a commit, and the upstream tag resolves to that commit.
  2. Every file of the upstream tag (except `.github/`, which is upstream CI and not vendored) is
     present in the vendored directory with the identical blob id, and the vendored directory
     holds nothing else apart from VENDORED_VERSION.txt and VENDORED_MANIFEST.sha256.
  3. The vendored tree, exported raw from git, matches its own VENDORED_MANIFEST.sha256
     (the plugin's own checker, run against the export).

Usage:
    python check_vendored_against_tag.py --vendored-dir <repo-relative dir> \
        [--upstream https://github.com/dansupergameprogrammer/SuperSLM.git] \
        [--manifest-checker <path to check_vendored_tree_matches_manifest.py>]

The vendored directory is a parameter, not an assumption: the same check applies wherever the
vendored tree lives.

Exit 0 when every check passes; non-zero with every problem listed otherwise.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

NOT_UPSTREAM = {"VENDORED_VERSION.txt", "VENDORED_MANIFEST.sha256"}
UPSTREAM_EXCLUDED_PREFIXES = (".github/",)


def git(*args, cwd=None, binary=False):
    out = subprocess.run(["git", *args], cwd=cwd, capture_output=True, check=True)
    return out.stdout if binary else out.stdout.decode("utf-8")


def read_pin(vendored_dir: str):
    text = git("show", f"HEAD:{vendored_dir}/VENDORED_VERSION.txt")
    tag = commit = None
    for line in text.splitlines():
        s = line.strip()
        if tag is None and s.startswith("Tag:"):
            tag = s[4:].strip()
        elif commit is None and s.startswith("Commit:"):
            commit = s[7:].strip()
    return tag, commit


def ls_tree(treeish: str, prefix: str = "", cwd=None):
    """{path relative to prefix: blob id} for every blob under prefix at treeish."""
    args = ["ls-tree", "-r", "-z", "--full-tree", treeish]
    if prefix:
        args += ["--", prefix]
    raw = git(*args, cwd=cwd, binary=True)
    entries = {}
    for rec in raw.split(b"\0"):
        if not rec:
            continue
        meta, _, path = rec.partition(b"\t")
        _mode, kind, oid = meta.decode().split(" ")
        if kind != "blob":
            continue
        p = path.decode("utf-8")
        if prefix:
            p = p[len(prefix) + 1:]
        entries[p] = oid
    return entries


def export_raw(vendored_dir: str, dest: Path):
    """Writes every committed blob under vendored_dir, raw (no eol/smudge), into dest."""
    entries = ls_tree("HEAD", vendored_dir)
    for rel, oid in entries.items():
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(git("cat-file", "blob", oid, binary=True))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vendored-dir", required=True)
    ap.add_argument("--upstream", default="https://github.com/dansupergameprogrammer/SuperSLM.git")
    ap.add_argument("--manifest-checker", default=None)
    a = ap.parse_args()
    vdir = a.vendored_dir.rstrip("/")

    problems = []
    tag, commit = read_pin(vdir)
    if not tag or not commit:
        print(f"FAIL: could not read 'Tag:' and 'Commit:' from {vdir}/VENDORED_VERSION.txt")
        return 1
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        problems.append(f"Commit '{commit}' is not a full 40-hex commit id")

    with tempfile.TemporaryDirectory() as tmp:
        up = Path(tmp) / "upstream"
        git("init", "-q", "--bare", str(up))
        git("fetch", "-q", "--depth", "1", a.upstream, f"refs/tags/{tag}:refs/tags/{tag}", cwd=up)
        resolved = git("rev-parse", f"{tag}^{{commit}}", cwd=up).strip()
        if resolved != commit:
            problems.append(f"upstream {tag} is {resolved}, VENDORED_VERSION.txt says {commit}")

        upstream = {
            p: oid for p, oid in ls_tree(tag, cwd=up).items()
            if not p.startswith(UPSTREAM_EXCLUDED_PREFIXES)
        }
        vendored = {p: oid for p, oid in ls_tree("HEAD", vdir).items() if p not in NOT_UPSTREAM}

        for p in sorted(set(upstream) - set(vendored)):
            problems.append(f"MISSING from vendored tree: {p}")
        for p in sorted(set(vendored) - set(upstream)):
            problems.append(f"NOT IN {tag}: {p}")
        for p in sorted(set(upstream) & set(vendored)):
            if upstream[p] != vendored[p]:
                problems.append(f"DIFFERS from {tag}: {p} (tag {upstream[p]}, vendored {vendored[p]})")
        print(f"{tag} ({commit}): {len(upstream)} upstream files, {len(vendored)} vendored files compared")

        if a.manifest_checker:
            export = Path(tmp) / "export"
            export_raw(vdir, export)
            r = subprocess.run(
                [sys.executable, a.manifest_checker, "--thirdparty-root", str(export)],
                capture_output=True, text=True,
            )
            sys.stdout.write(r.stdout)
            if r.returncode != 0:
                problems.append("VENDORED_MANIFEST.sha256 does not match the committed tree (see above)")

    for p in problems:
        print(f"FAIL: {p}")
    if not problems:
        print("OK: the vendored tree is the pinned tag, byte for byte, and matches its manifest")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
