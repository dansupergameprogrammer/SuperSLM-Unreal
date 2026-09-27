"""T-2226 -- L2-S0 red suite. Commissions guard G3 (the plan
§9 dim 11): "the plugin's own greedy-only structural check ... the check runs in CI over the
plugin's whole source tree ... Mutation: plant a `sslm_decode_params_init` call site passing
`SSLM_DECODE_MODE_DAMPED_GREEDY` -- CI must go red." Load-bearing because G3 is the single
guard standing between §12 RULING D-SLM3826 (runtime ships greedy mode only) and a silent
violation of §4's caller-owned-memory promise -- damped greedy grows a content-dependent
anti-LM state Layer 1 does not represent as caller workspace, so the promise is false the
moment damped greedy is reachable.

Per the commissioning rule (a deciding instrument is validated
against a must-accept AND a must-reject BEFORE its verdicts are trusted), this test drives
`ci/check_no_damped_greedy_calls.py` -- a build-time/CI-time source scanner L2-S0's build
delivers -- against two committed fixtures (fixtures/g3_clean, fixtures/g3_violation) and
requires it to accept the first and reject the second. "A check that has never rejected
anything is indistinguishable from a check that cannot" (plan §9 dim 11, G3's own text) --
the violation fixture is what makes this a real commissioning rather than a happy-path smoke
test.

Checker contract this test expects (specified here because the checker does not exist yet):
    python ci/check_no_damped_greedy_calls.py --source-root <dir>
    exit 0   -- no call site in <dir> passes SSLM_DECODE_MODE_DAMPED_GREEDY to
                sslm_decode_params_init
    exit !=0 -- at least one such call site found; stdout/stderr names the file and the
                literal SSLM_DECODE_MODE_DAMPED_GREEDY token (so a human/CI reader can find
                the offending line without re-running the scan by hand)

RED STATE (2026-08-22): ci/check_no_damped_greedy_calls.py does not exist in this tree.
Running this file under pytest now fails both cases with the same reason -- the checker
script is not found -- confirmed by executing it (see the test-design artifact's Execution
section for the captured run).
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
CI_ROOT = HERE.parent
CHECKER = CI_ROOT / "check_no_damped_greedy_calls.py"


def _run_checker(source_root: Path):
    if not CHECKER.exists():
        # The checker is L2-S0's own deliverable (§9 dim 11 G3), not this suite's -- the test suite
        # never implements the guard it is commissioning ("Do not implement").
        # Failing with a clear, explicit reason (rather than letting subprocess raise
        # FileNotFoundError uncaught) is what keeps this a red-unimplemented result rather
        # than a red-mis-stated-assertion result.
        return None, "ci/check_no_damped_greedy_calls.py does not exist yet (L2-S0 deliverable)"
    proc = subprocess.run(
        [sys.executable, str(CHECKER), "--source-root", str(source_root)],
        capture_output=True, text=True,
    )
    return proc.returncode, proc.stdout + proc.stderr


def test_must_accept_clean_call_site():
    """Must-accept: a call site using SSLM_DECODE_MODE_GREEDY,
    the only mode §2.3 RULING D-SLM3825/D-SLM3826 permits, is accepted (exit 0)."""
    returncode, output = _run_checker(HERE / "fixtures" / "g3_clean")
    assert returncode == 0, (
        f"expected the clean fixture to be accepted (exit 0); "
        f"got returncode={returncode!r}, output={output!r}"
    )


def test_must_reject_damped_greedy_call_site():
    """Must-reject, and the cell that makes G3 non-vacuous: a
    call site passing SSLM_DECODE_MODE_DAMPED_GREEDY is rejected (exit != 0), and the
    checker's own output names the offending token so the rejection is actionable."""
    returncode, output = _run_checker(HERE / "fixtures" / "g3_violation")
    assert returncode is not None and returncode != 0, (
        f"expected the violation fixture to be REJECTED (exit != 0); "
        f"got returncode={returncode!r}, output={output!r}"
    )
    assert "SSLM_DECODE_MODE_DAMPED_GREEDY" in output, (
        "rejection must name the offending mode, not just fail silently; "
        f"got output={output!r}"
    )


if __name__ == "__main__":
    # Executed at authoring time (2026-08-22) to confirm red for the right reason -- see the
    # test-design artifact's Execution section for the captured transcript.
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print(f"{name}: PASS (unexpected before L2-S0 lands)")
            except AssertionError as e:
                print(f"{name}: RED -- {e}")
