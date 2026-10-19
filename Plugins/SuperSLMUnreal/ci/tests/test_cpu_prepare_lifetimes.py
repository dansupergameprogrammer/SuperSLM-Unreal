"""Bounded lexical lifetime guard: real source, original defect and 12 planted reads.

This is source-order/alias discrimination, not C++ compilation, execution or sanitizer
evidence. The guard covers immediate state-destruction/return expressions inside
PrepareCpuState, including the local AllocFail helper; it is not interprocedural analysis.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pytest


CI = Path(__file__).resolve().parents[1]
PLUGIN = CI.parent
GUARD = CI / "check_cpu_prepare_lifetimes.py"
SOURCE = PLUGIN / "Source/SuperSLMUnreal/Private/SuperSLMSubsystem.cpp"
ORIGINAL = Path(__file__).parent / "fixtures/cpu_prepare_original_a8729f.cpp"
ORIGINAL_FUNCTION_SHA256 = "da4c543b5b1fbe5eedb5e86a45bb3cc8f8e163503a3304b2203bbabd573aba3e"
spec = importlib.util.spec_from_file_location("cpu_prepare_lifetime_guard", GUARD)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def test_current_full_source_has_no_post_destroy_state_reads():
    result = guard.audit(SOURCE.read_text(encoding="utf-8-sig"))
    assert result["roots"] == ["NewState"]
    assert result["aliases"] == ["Lane", "NewState", "S"]
    assert len(result["sites"]) == 10
    assert result["rejected_sites"] == []


def test_original_function_reproduces_the_independent_finding():
    # Exact function extracted from a8729fdd51918fa0c355911af028e2806f10f78f;
    # no Git history or ignored review-scratch file is required to reproduce the defect.
    source = ORIGINAL.read_text(encoding="utf-8")
    assert hashlib.sha256(source.encode()).hexdigest() == ORIGINAL_FUNCTION_SHA256
    result = guard.audit(source)
    assert len(result["sites"]) == 10
    assert len(result["rejected_sites"]) == 1
    assert result["rejected_sites"][0]["state_reads"] == ["S"]
    assert "S.PagePositions" in result["rejected_sites"][0]["expression"]
    assert "S.PageBytes" in result["rejected_sites"][0]["expression"]


PLANTED_READS = [(site, "NewState->Model") for site in range(10)] + [
    (9, "S.PageBytes"), (9, "Lane.Workspace")]


@pytest.mark.parametrize("site_index,state_read", PLANTED_READS,
                         ids=[f"site-{site}-{read}" for site, read in PLANTED_READS])
def test_each_destroy_return_site_and_both_local_aliases_are_rejected(site_index, state_read):
    source = SOURCE.read_text(encoding="utf-8-sig")
    healthy = guard.audit(source)
    assert len(healthy["sites"]) == 10 and not healthy["rejected_sites"]
    site = healthy["sites"][site_index]
    # Locate by the enumerated line, then plant a read in that site's actual return
    # expression. No expected message/remedy text is used to identify the branch.
    clean = guard.mask_noncode(source)
    line_start = sum(len(line) for line in clean.splitlines(keepends=True)[:site["destroy_line"] - 1])
    destroyed = re.search(r"\b(?:TearDown\s*\(\s*\)|delete\s+NewState)\s*;", clean[line_start:])
    assert destroyed is not None
    after_destroy = line_start + destroyed.end()
    returned = re.match(r"\s*return\b([^;]*);", clean[after_destroy:])
    assert returned is not None
    begin, end = after_destroy + returned.start(1), after_destroy + returned.end(1)
    mutant = source[:begin] + f" ((void)({state_read})," + source[begin:end] + ")" + source[end:]
    # The AllocFail helper otherwise cannot name the owning pointer; make that
    # capture explicit in the planted construction, without changing its ordering.
    if "What" in site["expression"]:
        anchor = "auto AllocFail = [&TearDown, &Refuse]"
        assert mutant.count(anchor) == 1
        mutant = mutant.replace(anchor, "auto AllocFail = [&TearDown, &Refuse, NewState]")
    result = guard.audit(mutant)
    assert len(result["sites"]) == 10
    assert len(result["rejected_sites"]) == 1
    rejected = result["rejected_sites"][0]
    assert rejected["destroy_line"] == site["destroy_line"]
    assert rejected["state_reads"] == [state_read.split("->")[0].split(".")[0]]


@pytest.mark.parametrize("original,expected_exit", [(False, 0), (True, 1)],
                         ids=["current-full-source", "original-defect"])
def test_guard_cli_reports_real_acceptance_and_refusal(tmp_path, original, expected_exit):
    output = tmp_path / "lifetime-report.json"
    result = subprocess.run(
        [sys.executable, "-B", str(GUARD), str(ORIGINAL if original else SOURCE),
         "--output", str(output)],
        env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == expected_exit, result.stdout + result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert len(report["sites"]) == 10
    assert bool(report["rejected_sites"]) == original
