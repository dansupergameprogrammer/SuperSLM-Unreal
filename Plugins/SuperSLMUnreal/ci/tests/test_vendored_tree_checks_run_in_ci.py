"""T-2799 S2 (code review, the code review record).

`ci/check_vendored_tree_matches_manifest.py` and `ci/check_vendored_wrappers_complete.py` had
exactly one caller each, a CI workflow removed at U0 (D-SLM7233). Neither checker is in
`ci/tests`, so nothing in this suite -- and nothing the U0 gate's "`ci/tests` is green" claim
covered -- ever ran them again. That gap is why S1 (three `v1.5.0` release docs silently dropped
by an un-anchored `.gitignore` rule) and O1 (three plugin-authored files placed inside the
vendored directory) both survived into a green U0 build: the manifest checker would have caught
either one, and nothing invoked it.

**Why this reads the COMMITTED tree, never the working directory (code review T-2799, "Why S1 went
unseen"):** every one of S1's four near-misses happened because a check read the filesystem.
`git status --short` hides ignored files; staging with `git add` skips ignored files by design;
a build that mirrors files in and then runs `git clean -fd` leaves ignored files sitting on disk,
where a filesystem-reading check would pass against them. A checker that reads a working
directory inherits every one of those blind spots. Reading `git show HEAD:<path>` for every path
`git ls-tree` reports under `HEAD` reads exactly what is committed and nothing else -- an
ignored-and-therefore-absent file is simply absent from `git ls-tree`, and a hand-placed extra
that was never `git add`-ed is simply not there either.

**Why `git show`/`git cat-file`, never `git archive`:** `git archive` applies `.gitattributes`'
`text`/`eol=crlf` conversion exactly as a normal checkout would (`VENDORED_VERSION.txt` notes it
on Windows), which would corrupt a byte-exact manifest comparison with line-ending noise having
nothing to do with content. `git cat-file`/`git show` return the raw blob, unconditionally, on
every platform -- the same method the re-vendor itself uses.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

PLUGIN_ROOT = Path(__file__).resolve().parents[2]  # .../Plugins/SuperSLMUnreal (file is ci/tests/<this>.py)
CHECK_MANIFEST = PLUGIN_ROOT / "ci" / "check_vendored_tree_matches_manifest.py"
CHECK_WRAPPERS = PLUGIN_ROOT / "ci" / "check_vendored_wrappers_complete.py"


def _repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=PLUGIN_ROOT,
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    return Path(out)


def _plugin_rel_posix(repo_root: Path) -> str:
    return PLUGIN_ROOT.resolve().relative_to(repo_root.resolve()).as_posix()


def _export_head_tree(repo_root: Path, rel_root: str, dest: Path) -> list[str]:
    """Writes every path `git ls-tree` reports under `rel_root` at HEAD, raw, into `dest`,
    preserving `rel_root`'s own path prefix. One `git cat-file --batch` call reads every blob,
    rather than one `git show` subprocess per file, which is the same correctness (raw blob,
    no smudge/eol conversion) at a fraction of the process-spawn cost for a ~700-file tree.
    """
    ls = subprocess.run(
        ["git", "ls-tree", "-r", "-z", "--full-tree", "HEAD", "--", rel_root],
        cwd=repo_root, capture_output=True, check=True,
    ).stdout
    entries = [e for e in ls.split(b"\0") if e]
    assert entries, f"git ls-tree reported nothing under {rel_root!r} at HEAD"

    shas = []
    rel_paths = []
    for entry in entries:
        meta, _, rel_path = entry.partition(b"\t")
        _mode, kind, sha = meta.split()
        if kind != b"blob":
            continue
        shas.append(sha)
        rel_paths.append(rel_path.decode("utf-8"))

    batch_input = b"\n".join(shas) + b"\n"
    batch_out = subprocess.run(
        ["git", "cat-file", "--batch"], cwd=repo_root,
        input=batch_input, capture_output=True, check=True,
    ).stdout

    pos = 0
    for sha, rel_path in zip(shas, rel_paths):
        header_end = batch_out.index(b"\n", pos)
        header = batch_out[pos:header_end].decode("utf-8")
        out_sha, out_kind, size_str = header.split()
        assert out_sha == sha.decode("ascii") and out_kind == "blob", (
            f"cat-file --batch desynchronized at {rel_path!r}: expected blob {sha!r}, got {header!r}"
        )
        size = int(size_str)
        content_start = header_end + 1
        content = batch_out[content_start:content_start + size]
        pos = content_start + size + 1  # +1 for the trailing newline cat-file appends

        out_path = dest / rel_path
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(content)

    return rel_paths


@pytest.fixture(scope="module")
def committed_plugin_tree():
    """The plugin's own tree at HEAD, exported raw into a scratch directory once per test
    module run, and reused by every test below -- one `git cat-file --batch` call rather than
    one per test.
    """
    repo_root = _repo_root()
    plugin_rel = _plugin_rel_posix(repo_root)
    with tempfile.TemporaryDirectory(prefix="t2799_committed_tree_") as tmp:
        dest = Path(tmp)
        _export_head_tree(repo_root, plugin_rel, dest)
        yield dest / plugin_rel


def test_manifest_checker_passes_against_committed_tree(committed_plugin_tree):
    thirdparty_root = committed_plugin_tree / "Source" / "ThirdParty" / "SuperSLM"
    assert thirdparty_root.is_dir(), (
        f"{thirdparty_root} is missing from the committed-tree export -- either HEAD does not "
        "vendor Layer 1 at this path, or the export itself is broken"
    )
    result = subprocess.run(
        [sys.executable, str(CHECK_MANIFEST), "--thirdparty-root", str(thirdparty_root)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0, (
        "the vendored-tree manifest check failed against the COMMITTED tree (HEAD) -- this is "
        "exactly the check the deleted CI workflow used to run, and exactly the gap that let "
        "T-2799's S1 (three files dropped by an un-anchored .gitignore rule) and O1 (three "
        "plugin-authored files inside the vendored directory) both reach a green U0 build:\n"
        f"{result.stdout}{result.stderr}"
    )


def test_wrapper_completeness_checker_passes_against_committed_tree(committed_plugin_tree):
    result = subprocess.run(
        [sys.executable, str(CHECK_WRAPPERS), "--plugin-root", str(committed_plugin_tree)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0, (
        "the vendored-wrapper completeness check failed against the COMMITTED tree (HEAD):\n"
        f"{result.stdout}{result.stderr}"
    )
