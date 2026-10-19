#!/usr/bin/env python3
"""Run routine coverage cells or the complete five-suite paged-KV CPU workload.

Routine mode omits only the explicitly measured extended cells. Inventory comes
from each executable, so new cells run by default and stale exclusions fail.
Full mode runs every registered cell, without changing race repetition counts.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time

SUITES = ("r0", "c2", "c3", "c4", "c5")
EXTENDED = Path(__file__).with_name("paged_kv_extended_cells.json")


def select_cells(registered: list[str], extended: dict[str, float], mode: str) -> list[str]:
    if not registered or len(set(registered)) != len(registered):
        raise ValueError("cell inventory must be nonempty and contain unique ids")
    if any(not cell or any(c.isspace() for c in cell) for cell in registered):
        raise ValueError("cell inventory contains an invalid id")
    missing = set(extended) - set(registered)
    if missing:
        raise ValueError(f"extended cells absent from executable: {sorted(missing)}")
    if mode not in ("routine", "full"):
        raise ValueError(f"unknown mode: {mode}")
    selected = [cell for cell in registered if mode == "full" or cell not in extended]
    if not selected:
        raise ValueError("selection contains no cells")
    return selected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", required=True, type=Path)
    parser.add_argument("--mode", choices=("routine", "full"), required=True)
    parser.add_argument("--fixture-dir", type=Path)
    parser.add_argument("--profile-dir", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    extended = json.loads(EXTENDED.read_text(encoding="utf-8"))
    if {key for key in extended if not key.startswith("_")} != set(SUITES):
        parser.error("extended-cell manifest must name exactly the five CPU suites")
    shared = extended.get("_shared_work", {})
    if set(shared) - set(SUITES):
        parser.error("shared-work dependents must name a CPU suite")
    env = os.environ.copy()
    fixture = args.fixture_dir or Path(env.get("SUPERSLM_PAGED_KV_FIXTURE_DIR", ""))
    if not all((fixture / f"{name}.sslm").is_file() for name in ("pkv_def", "pkv_qk", "pkv_odd", "pkv_32k")):
        parser.error("all four generated paged-KV fixtures are required")
    env["SUPERSLM_PAGED_KV_FIXTURE_DIR"] = str(fixture.resolve())
    plans = []
    # Preflight every suite before executing any cells.
    for suite in SUITES:
        binary = args.binary_dir.resolve() / f"superslm_pkv_{suite}{'.exe' if os.name == 'nt' else ''}"
        inventory_env = dict(env, LLVM_PROFILE_FILE=os.devnull)
        inventory = subprocess.run([str(binary), "--list"], env=inventory_env, text=True, capture_output=True, check=True)
        registered = inventory.stdout.splitlines()
        omitted = dict(extended[suite])
        for cell, dependencies in shared.get(suite, {}).items():
            if not dependencies or not set(dependencies) <= set(extended[suite]):
                parser.error(f"{suite}/{cell}: shared work must name measured extended cells")
            omitted[cell] = 0.0
        selected = select_cells(registered, omitted, args.mode)
        plans.append((suite, binary, registered, selected))
    receipt = {"mode": args.mode, "suites": []}
    failed = False
    for suite, binary, registered, selected in plans:
        run_env = env.copy()
        if args.profile_dir:
            args.profile_dir.mkdir(parents=True, exist_ok=True)
            run_env["LLVM_PROFILE_FILE"] = str(args.profile_dir.resolve() / f"pkv_{suite}.profraw")
        omitted = [cell for cell in registered if cell not in selected]
        print(f"{suite}: running {len(selected)}/{len(registered)} cells ({args.mode}); extended omissions: {omitted}", flush=True)
        started = time.monotonic()
        result = subprocess.run([str(binary), *(f"={cell}" for cell in selected)], env=run_env)
        receipt["suites"].append({"suite": suite, "registered": registered, "selected": selected,
                                  "omitted": omitted, "exit_code": result.returncode,
                                  "elapsed_seconds": time.monotonic() - started})
        failed |= result.returncode != 0
        if args.receipt:
            args.receipt.parent.mkdir(parents=True, exist_ok=True)
            args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
