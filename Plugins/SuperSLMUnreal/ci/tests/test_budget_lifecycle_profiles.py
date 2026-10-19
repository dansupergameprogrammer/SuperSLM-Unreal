"""Check the committed K4 fixture against every original call, independently of the resolver.

Decimal order statistics avoid using either the production constants or the fixture producer's
floating-point reduction. The native 60-configuration cell grades actual delivered-job selection.
"""
import csv
import hashlib
import json
from collections import defaultdict
from decimal import Decimal, ROUND_CEILING
from pathlib import Path


FIXTURES = (Path(__file__).resolve().parents[2] / "Source" / "SuperSLMUnrealEditor"
            / "Private" / "Tests" / "Fixtures" / "Paging")
FIELDS = {"adopt_1000": "AdoptCostMs", "restore_full": "RestoreCostMs",
          "prefix_begin_full_budget": "PrefixBeginCostMs"}


def middle(values):
    ordered = sorted(values)
    size = len(ordered)
    assert size > 0
    return (ordered[(size - 1) // 2] + ordered[size // 2]) / 2


def test_budget_fixture_matches_all_actual_raw_calls():
    fixture = json.loads((FIXTURES / "budget_lifecycle_cost_profiles.json").read_text(encoding="utf-8-sig"),
                        parse_float=Decimal)
    assert set(fixture["profiles"]) == {"4096", "32768"}
    for label, cap, artifact in (
        ("05b", 4096, "83e96aa46e1404c824e2bfea22e1511a528298a2edd32299e69a77f414e2007a"),
        ("15b", 32768, "811978f69bdb3aab7e9c89e03becd399c8549990c817465e596e1046d6cc113e"),
    ):
        raw = FIXTURES / "LifecyclePricing" / label
        meta = json.loads((raw / "metadata.json").read_text(encoding="utf-8-sig"))
        assert meta["cap"] == cap and meta["artifact_hash"] == artifact
        assert meta["tasks"] == 4 and meta["blocks_per_mode"] == 3
        assert meta["warmup_per_arm_mode"] == meta["timed_per_arm_mode"] == 50
        assert meta["elapsed_s"] > 0 and meta["timer_ms_per_cycle"] > 0
        assert abs(meta["whole_run_foreign_pct"] - max(
            0, meta["whole_run_box_busy_pct"] - meta["whole_run_own_pct"])) < 1e-8
        for source in fixture["evidence"][str(cap)]["inputs"]:
            path = raw / Path(source["path"].replace("\\", "/")).name
            assert path.stat().st_size == source["bytes"]
            assert hashlib.sha256(path.read_bytes()).hexdigest() == source["sha256"]

        with (raw / "calls.csv").open(encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == 1800
        population = set()
        timed = defaultdict(list)
        for row in rows:
            block, mode, repeat = (int(row[key]) for key in ("block", "mode", "rep"))
            arm, phase = row["arm"], row["phase"]
            identity = block, mode, arm, phase, repeat
            assert block in range(3) and mode in (0, 1) and repeat in range(50)
            assert arm in FIELDS and phase in ("warmup", "timed")
            assert identity not in population
            population.add(identity)
            duration = Decimal(row["elapsed_ms"])
            assert duration.is_finite() and duration > 0 and int(row["cap"]) == cap
            expected = {"adopt_1000": (512, 1000, 1000, 512, 1512),
                        "restore_full": (cap, cap, 0, cap, cap),
                        "prefix_begin_full_budget": (cap, 0, 0, 512, 512)}[arm]
            assert tuple(int(row[key]) for key in (
                "call_budget", "observed_context", "observed_origin", "observed_budget", "observed_limit")) == expected
            if phase == "timed":
                timed[mode, arm, block].append(duration)

        profiles = fixture["profiles"][str(cap)]
        assert set(profiles) == {"0", "1"}
        for mode in (0, 1):
            assert set(profiles[str(mode)]) == set(FIELDS.values())
            for arm, field in FIELDS.items():
                blocks = [timed[mode, arm, block] for block in range(3)]
                assert all(len(block) == 50 for block in blocks)
                estimate = middle([middle(block) for block in blocks])
                rounded = estimate.quantize(Decimal(".001"), rounding=ROUND_CEILING)
                assert rounded > 0 and profiles[str(mode)][field] == rounded
