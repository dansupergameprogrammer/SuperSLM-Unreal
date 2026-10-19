"""Routine coverage keeps new cells and fails on stale or empty selections."""
import importlib.util
import json
from pathlib import Path
import re
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pkv_suites", ROOT / "tools/ci/run_paged_kv_suites.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def test_new_cells_run_without_an_allowlist_edit():
    assert driver.select_cells(["short", "stress", "new"], {"stress": 100}, "routine") == ["short", "new"]


def test_full_runs_every_cell_in_registration_order():
    assert driver.select_cells(["stress", "short"], {"stress": 100}, "full") == ["stress", "short"]


@pytest.mark.parametrize("registered,extended", [([], {}), (["x", "x"], {}), (["x"], {"gone": 100}), (["x"], {"x": 100}), (["bad id"], {})])
def test_invalid_routine_selection_fails(registered, extended):
    with pytest.raises(ValueError):
        driver.select_cells(registered, extended, "routine")


def test_full_mode_still_rejects_stale_exclusions():
    with pytest.raises(ValueError, match="absent"):
        driver.select_cells(["new"], {"old": 100}, "full")


def test_manifest_contains_only_measured_extended_cells():
    manifest = json.loads(driver.EXTENDED.read_text())
    assert {key for key in manifest if not key.startswith("_")} == set(driver.SUITES)
    assert all(seconds > 90 for suite in driver.SUITES for seconds in manifest[suite].values())
    for suite, dependents in manifest["_shared_work"].items():
        assert suite in driver.SUITES
        for cell, dependencies in dependents.items():
            assert cell not in manifest[suite]
            assert dependencies and set(dependencies) <= set(manifest[suite])


def test_failed_suite_fails_driver_and_selection_is_recorded(tmp_path, monkeypatch):
    for name in ("pkv_def", "pkv_qk", "pkv_odd", "pkv_32k"):
        (tmp_path / f"{name}.sslm").touch()
    manifest = tmp_path / "extended.json"
    manifest.write_text(json.dumps({suite: {"stress": 100} for suite in driver.SUITES}))
    monkeypatch.setattr(driver, "EXTENDED", manifest)
    commands = []

    def run(command, **kwargs):
        if command[-1] == "--list":
            return SimpleNamespace(stdout="new\nstress\nkept\n", returncode=0)
        commands.append(command)
        return SimpleNamespace(returncode=9 if "pkv_c3" in command[0] else 0)

    monkeypatch.setattr(driver.subprocess, "run", run)
    receipt = tmp_path / "receipt.json"
    monkeypatch.setattr("sys.argv", ["driver", "--binary-dir", str(tmp_path), "--mode", "routine",
                                    "--fixture-dir", str(tmp_path), "--receipt", str(receipt)])
    assert driver.main() == 1
    assert len(commands) == 5
    assert all(command[1:] == ["=new", "=kept"] for command in commands)
    rows = json.loads(receipt.read_text())["suites"]
    assert all(row["omitted"] == ["stress"] for row in rows)
    assert next(row for row in rows if row["suite"] == "c3")["exit_code"] == 9


def test_manual_modes_do_not_start_the_platform_matrix():
    workflow = (ROOT / ".github/workflows/tests.yml").read_text()
    body = workflow.split("\njobs:\n", 1)[1]
    sections = re.split(r"(?m)^  ([a-zA-Z0-9_-]+):\s*$", body)
    jobs = dict(zip(sections[1::2], sections[2::2]))

    def selected(event, suite):
        result = set()
        for name, section in jobs.items():
            found = re.search(r"(?m)^    if: (.+)$", section)
            condition = found.group(1) if found else "True"
            expression = condition.replace("github.event_name", "event").replace("inputs.suite", "suite")
            expression = expression.replace("&&", " and ").replace("||", " or ")
            if eval(expression, {"__builtins__": {}}, {"event": event, "suite": suite}):
                result.add(name)
        return result

    regular = set(jobs) - {"paged-kv-full"}
    assert selected("push", "") == regular
    assert selected("pull_request", "") == regular
    assert selected("workflow_dispatch", "all") == regular
    assert selected("workflow_dispatch", "coverage") == {"branch-coverage"}
    assert selected("workflow_dispatch", "paged-kv-full") == {"paged-kv-full"}
