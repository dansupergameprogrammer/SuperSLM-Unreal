"""Run the repaired route-P drivers against the pinned engine fixture (the tag VENDORED_VERSION.txt names).

Usage: python d4_route_p_runtime_pin.py --engine-prefix <prefix> --artifact <model.sslm>
       --reference-json <independent-engine-code-digest.json> --reopening <record.json>
       --scratch <empty scratch directory>
The engine prefix must carry the normal provenance and pinned-library hash used by the driver.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


PLUGIN = Path(__file__).resolve().parents[2]
VENDOR = PLUGIN / "Source" / "ThirdParty" / "SuperSLM"
DRIVER = VENDOR / "tools" / "consumer_reach" / "route_p_hook" / "run_route_p_hook_witness.sh"
MERGE = DRIVER.with_name("run_merge_acceptance.sh")
VARIANTS = (
    "control", "H1_hex_zero", "H2_named_zero", "H3_reserved_deleted_Hook_value_init",
    "Hplus_prefill_bit", "Hbad_other_bit", "Hplus_named_bit",
    "Hk16_bit_only_at_k_ge_16", "Hk_lt16",
)
UNHOOKED = "not hooked"
REFUSED = "not hooked (engine refused the install; fail closed)"
EXPECTED_READINGS = {
    "control": (UNHOOKED, UNHOOKED, UNHOOKED),
    "H1_hex_zero": (UNHOOKED, UNHOOKED, UNHOOKED),
    "H2_named_zero": (UNHOOKED, UNHOOKED, UNHOOKED),
    "H3_reserved_deleted_Hook_value_init": (UNHOOKED, UNHOOKED, UNHOOKED),
    "Hplus_prefill_bit": (REFUSED, REFUSED, REFUSED),
    "Hbad_other_bit": (UNHOOKED, UNHOOKED, UNHOOKED),
    "Hplus_named_bit": (REFUSED, REFUSED, REFUSED),
    "Hk16_bit_only_at_k_ge_16": (UNHOOKED, UNHOOKED, REFUSED),
    "Hk_lt16": (REFUSED, REFUSED, UNHOOKED),
}


def linux_path(path: Path) -> str:
    path = path.resolve()
    if os.name == "nt":
        return f"/mnt/{path.drive[0].lower()}/{path.as_posix()[3:]}"
    return str(path)


def run_bash(script: Path, *args: str) -> subprocess.CompletedProcess[str]:
    command = (["wsl.exe", "-e", "env", "PYTHONDONTWRITEBYTECODE=1", "bash", linux_path(script)] if os.name == "nt"
               else ["bash", str(script)])
    return subprocess.run(command + list(args), capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("engine-prefix", "artifact", "reference-json", "reopening", "scratch"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    prefix = linux_path(args.engine_prefix)
    commit = re.search(r"^Commit: ([0-9a-f]{40})$", (VENDOR / "VENDORED_VERSION.txt").read_text(), re.M)
    assert commit, "vendored engine commit missing"
    spec = f"v113={prefix}={commit.group(1)}={linux_path(args.reference_json)}"
    source = linux_path(PLUGIN / "Source" / "SuperSLMUnreal")
    include = f"{prefix}/include"
    runtime_scratch = linux_path(args.scratch / "route-p")
    result = run_bash(DRIVER, source, include, runtime_scratch, linux_path(args.artifact), spec)
    print(result.stdout, end="")
    if result.returncode != 0 or "COMPILE FAIL" in result.stdout or "[v113] ok" not in result.stdout:
        print(result.stderr, file=sys.stderr)
        return 1
    for variant in VARIANTS:
        section = re.search(rf"^== {re.escape(variant)}\n(.*?)(?=^== |\Z)", result.stdout, re.M | re.S)
        if not section:
            print(f"missing compiled runtime variant {variant}", file=sys.stderr)
            return 1
        for k, expected in zip((2, 8, 256), EXPECTED_READINGS[variant]):
            line = rf"^   \[v113\] H\(k={k}\) = {re.escape(expected)}$"
            if not re.search(line, section.group(1), re.M):
                print(f"{variant} at k={k} did not read {expected}", file=sys.stderr)
                return 1
    dead = result.stdout.split("== dead-counter fixture", 1)[-1]
    if any(f"control    [v113, dead counter] H(k={k}) = VOID" not in dead for k in (2, 8, 256)):
        print("dead-counter control is not VOID at all three task counts", file=sys.stderr)
        return 1

    merge = run_bash(MERGE, source, linux_path(args.scratch / "merge"), linux_path(args.artifact),
                     linux_path(args.reopening), spec)
    print(merge.stdout, end="")
    if merge.returncode != 2 or "COMPILE FAIL" in merge.stdout or "=> REFUSED -- H(k=" not in merge.stdout:
        print(merge.stderr, file=sys.stderr)
        return 1
    print("T-3050 route-P runtime pin: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
