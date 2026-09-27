#!/usr/bin/env python3
"""Version identity: the plugin's version is stated once and agrees everywhere it appears.

Checks, reading the committed files at HEAD:

  1. The .uplugin `VersionName` and the top entry of CHANGELOG.md agree:
     - normally they are the same version;
     - between releases, a `VersionName` ending in `-dev` is allowed while the top CHANGELOG
       entry is marked Unreleased.
  2. Version tags on HEAD (`v<digits>...`): more than one distinct tag is always a failure. One tag
     must equal `v` + `VersionName`. No tag is a failure unless --allow-untagged is given.
  3. With --release: `VersionName` is not a `-dev` version, equals the top CHANGELOG version, and
     that entry is not marked Unreleased.
  4. Each --vendored-dir's VENDORED_VERSION.txt `Tag:` equals `v` + the `project(... VERSION x.y.z`
     in that directory's CMakeLists.txt, so the pin and the vendored sources name one release.
     (The plugin versions independently of SuperSLM; only the pin is checked, not equality.)

Usage:
    python check_version_identity.py --uplugin <path> --changelog <path> \
        [--vendored-dir <dir> ...] [--allow-untagged] [--release]
"""
import argparse
import json
import re
import subprocess
import sys

HEADING_RE = re.compile(r"^##\s+\[?v?(\d+\.\d+\.\d+[0-9A-Za-z.\-]*)\]?(.*)$")
TAG_RE = re.compile(r"^v\d")
CMAKE_VERSION_RE = re.compile(r"project\(\s*\w+\s+VERSION\s+(\d+\.\d+\.\d+)", re.IGNORECASE)


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def head_file(path):
    # The committed bytes; a working-copy edit that was never committed proves nothing.
    return git("show", f"HEAD:{path}")


def top_changelog_entry(text):
    for line in text.splitlines():
        m = HEADING_RE.match(line.strip())
        if m:
            return m.group(1), ("unreleased" in m.group(2).lower())
    return None, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uplugin", required=True)
    ap.add_argument("--changelog", required=True)
    ap.add_argument("--vendored-dir", action="append", default=[])
    ap.add_argument("--allow-untagged", action="store_true")
    ap.add_argument("--release", action="store_true")
    a = ap.parse_args()

    problems = []
    version = json.loads(head_file(a.uplugin))["VersionName"]
    top, unreleased = top_changelog_entry(head_file(a.changelog))
    print(f"VersionName {version}; CHANGELOG top {top}{' (Unreleased)' if unreleased else ''}")

    if top is None:
        problems.append(f"{a.changelog} has no '## <version>' entry")
    elif version != top and not (version.endswith("-dev") and unreleased):
        problems.append(f"VersionName {version} does not match the top CHANGELOG entry {top}")

    if a.release:
        if version.endswith("-dev"):
            problems.append(f"VersionName {version} is a development version; set the release version first")
        if unreleased:
            problems.append(f"the top CHANGELOG entry {top} is still marked Unreleased; date it first")

    tags = sorted({t for t in git("tag", "--points-at", "HEAD").split() if TAG_RE.match(t)})
    if len(tags) > 1:
        problems.append(f"HEAD carries more than one version tag: {', '.join(tags)}")
    elif len(tags) == 1:
        if tags[0] != f"v{version}":
            problems.append(f"HEAD is tagged {tags[0]} but VersionName is {version}")
        if unreleased:
            problems.append(f"HEAD is tagged {tags[0]} but the top CHANGELOG entry is marked Unreleased")
    elif not a.allow_untagged:
        problems.append("HEAD carries no version tag (pass --allow-untagged between releases)")

    for vdir in a.vendored_dir:
        vdir = vdir.rstrip("/")
        pin = None
        for line in head_file(f"{vdir}/VENDORED_VERSION.txt").splitlines():
            if line.strip().startswith("Tag:"):
                pin = line.strip()[4:].strip()
                break
        m = CMAKE_VERSION_RE.search(head_file(f"{vdir}/CMakeLists.txt"))
        if not pin or not m:
            problems.append(f"{vdir}: could not read the pinned tag or the CMake project version")
        elif pin != f"v{m.group(1)}":
            problems.append(f"{vdir}: VENDORED_VERSION.txt pins {pin} but the vendored CMakeLists.txt is {m.group(1)}")
        else:
            print(f"{vdir}: pin {pin} agrees with the vendored sources")

    for p in problems:
        print(f"FAIL: {p}")
    if not problems:
        print("OK: version identity holds")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
