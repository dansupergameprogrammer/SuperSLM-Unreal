"""T-2226 -- L2-S0 red suite. Commissions guard G4 (the plan
§9 dim 11): "the never-forced-SIMD-tier check ... D-SLM3833 -- the determinism guarantee the
plugin inherits is Layer 1's cross-tier bit-identity, which holds only while the plugin
leaves the runtime dispatch alone ... Mutation: plant a SUPERSLM_FORCE_AVX2_MATMUL
definition in the plugin's build -- CI must go red."

Same commissioning shape as test_guard_g3_no_damped_greedy.py (the commissioning rule:
must-accept + must-reject, both real and executed against the checker's real data path --
grepping the plugin's own Build.cs files, per the plan's own text for this guard).

Checker contract this test expects (specified here because the checker does not exist yet):
    python ci/check_no_forced_simd_tier.py --source-root <dir>
    exit 0   -- no *.Build.cs under <dir> defines a SUPERSLM_FORCE_* preprocessor symbol
    exit !=0 -- at least one such definition found; output names the file and the symbol

RED STATE (2026-08-22): ci/check_no_forced_simd_tier.py does not exist in this tree.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
CI_ROOT = HERE.parent
CHECKER = CI_ROOT / "check_no_forced_simd_tier.py"


def _run_checker(source_root: Path):
    if not CHECKER.exists():
        return None, "ci/check_no_forced_simd_tier.py does not exist yet (L2-S0 deliverable)"
    proc = subprocess.run(
        [sys.executable, str(CHECKER), "--source-root", str(source_root)],
        capture_output=True, text=True,
    )
    return proc.returncode, proc.stdout + proc.stderr


def test_must_accept_no_forced_tier():
    returncode, output = _run_checker(HERE / "fixtures" / "g4_clean")
    assert returncode == 0, (
        f"expected the clean fixture to be accepted (exit 0); "
        f"got returncode={returncode!r}, output={output!r}"
    )


def test_must_reject_forced_avx2_definition():
    returncode, output = _run_checker(HERE / "fixtures" / "g4_violation")
    assert returncode is not None and returncode != 0, (
        f"expected the violation fixture to be REJECTED (exit != 0); "
        f"got returncode={returncode!r}, output={output!r}"
    )
    assert "SUPERSLM_FORCE_AVX2_MATMUL" in output, (
        f"rejection must name the offending symbol; got output={output!r}"
    )


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print(f"{name}: PASS (unexpected before L2-S0 lands)")
            except AssertionError as e:
                print(f"{name}: RED -- {e}")
