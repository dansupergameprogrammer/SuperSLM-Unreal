"""T-3102: independent recomputation for the D4 user calibration estimate (plan §10.3.1 item 5b,
D-SLM8071/D-SLM8074).

Not a pytest module: every subcommand reads the output of a real run (an Unreal log and the
estimate's retained per-call CSVs, or the T-3050d capture), so the box driver runs it by hand.
Exit 0 means every check held, 1 means at least one failed (each failure is printed), 2 means
the inputs could not be read.

Subcommands
  report     Structure of one estimate run in a log: the label first, 22 arm/depth rows and 13
             suggestions per mode, the four load summaries, coverage, lock, files, status and the
             disclosures; no verified/idleness/refusal wording. --min-workload-foreign PCT also
             requires both workload load medians to reach PCT (the elevated-load cells).
             --expect-write / --expect-no-write pin the opt-in.
  recompute  Arithmetic of one estimate run: every row's call count, median and IQR, and all 26
             suggested fields, recomputed from the run's own mode0/mode1 .calls.csv by the
             documented reductions and fits -- never by calling the plugin.
  fresh      Two or more runs in one log (one editor session): distinct run ids and directories,
             matching CSV block identities, calls and samples numbered from zero, finite ordered times inside
             that run's independent timestamped header-to-completion wall-time envelope.
  provenance The one-time profile table: recomputed from the six T-3050d blocks (median within
             each block, median of the three block medians, the fits, rounded upward at the
             displayed precision), compared with the committed fixture and, with --plan, with the
             plan's own table. The six .calls.csv files are first checked against SHA256SUMS.txt.

Definitions (stated once, used by both recompute and provenance)
  median      the conventional median: the middle value, or the mean of the two middle values
              (statistics.median), as the T-3050d analysis computed it.
  IQR         p75 - p25 with linear interpolation between order statistics (the T-3050d
              analysis's percentile()).
  chunk-1 fit over depths d_i and medians m_i (layer, prompt_1): slope = max(0, (m_last - m_first)
              / (d_last - d_first)); intercept = m_first, raised by the largest m_i - (intercept +
              slope * d_i) when positive (the intercept is anchored at the depth-1 median).
  batched fit per-token medians t_i = median(call ms) / 8 at positions p_i = d_i + 3.5: slope as
              above over p; intercept = t_first - slope * p_first, raised by the largest residual.
  printed comparison (plan item 5b contract item 13, D-SLM8147): a suggestion prints exactly
              d decimals (3 ms, 6 per position) and matches its recomputation x when
              x - 1e-8 <= printed < x + 10^-d + 1e-8 (the round-up rule); a row median or IQR
              prints at least 3 decimals and matches when |printed - x| <= 0.5 * 10^-d + 1e-8.
  rounding    the one-time table: ceil(x * 10^d - 1e-9) / 10^d (D-SLM8113).
  samples     report also applies D-SLM8142 to samples.csv: the subtraction identity within 2e-6,
              the workload own-share floor 0.5 * 100 / N, the four lock_state tokens, and (with
              --require-own-lock) "own" on every sample.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import statistics
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path

PREFIX = "SuperSLM.EstimateCosts: "
HEADER = PREFIX + "CALIBRATION ESTIMATE — not verified"
FORBIDDEN = ("verified-idle", "MEETS the criterion", "idleness criterion", "REFUSED", "ceiling")
CALLS_HEADER = ("call_id,block,mode,arm,depth,rep,layer,start_s,end_s,elapsed_ms,foreign_pct,"
                "foreign_peak_pct,foreign_coverage,first_sample,last_sample")
FIELDS = ("LayerCostMs", "LayerCostPerPositionMs", "PromptTokenCostMs", "PromptTokenCostPerPositionMs",
          "BatchedPromptTokenCostMs", "BatchedPromptTokenCostPerPositionMs", "FinishCostMs", "ResetCostMs",
          "AdoptCostMs", "SaveCostMs", "RestoreCostMs", "PrefixBeginCostMs", "PrefixReleaseCostMs")
SCALAR_ARMS = {"FinishCostMs": ("finish", 1), "ResetCostMs": ("reset", -1), "AdoptCostMs": ("adopt", 1),
               "SaveCostMs": ("save", 0), "RestoreCostMs": ("restore", 0),
               "PrefixBeginCostMs": ("prefix_begin", -1), "PrefixReleaseCostMs": ("prefix_release", -1)}
CHUNK1_DEPTHS = (1, 1024, 2048, 3072, 4095)
BATCHED_DEPTHS = (1, 1024, 2048, 3072, 4088)
REPS = 12
NUM_LAYERS = 24  # A-EX (Qwen2.5-0.5B): the layer arm times layers 1 .. 22 of each repetition
BLOCKS = {0: ("m0a", "m0b", "m0c"), 1: ("m1a", "m1b", "m1c")}


class Checks:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.count = 0

    def that(self, condition: bool, what: str) -> bool:
        self.count += 1
        if not condition:
            self.failures.append(what)
            print(f"FAIL: {what}")
        return condition

    def finish(self, name: str) -> int:
        print(f"{name}: {self.count - len(self.failures)}/{self.count} checks held")
        return 1 if self.failures else 0


def expected_groups() -> list[tuple[str, int]]:
    groups = [(arm, depth) for arm, depth in SCALAR_ARMS.values()]
    for d in CHUNK1_DEPTHS:
        groups += [("prompt_1", d), ("layer", d)]
    groups += [("prompt_8", d) for d in BATCHED_DEPTHS]
    return groups


def expected_calls(arm: str) -> int:
    return REPS * (NUM_LAYERS - 2) if arm == "layer" else REPS


def percentile(values: list[float], q: float) -> float:
    x = sorted(values)
    p = (len(x) - 1) * q
    lo = math.floor(p)
    return x[lo] + (x[math.ceil(p)] - x[lo]) * (p - lo)


def chunk1_fit(depths: list[float], medians: list[float]) -> tuple[float, float]:
    span = depths[-1] - depths[0]
    slope = max(0.0, (medians[-1] - medians[0]) / span) if span > 0 else 0.0
    intercept = medians[0]
    raise_by = max([0.0] + [m - (intercept + slope * d) for d, m in zip(depths, medians)])
    return intercept + raise_by, slope


def batched_fit(depths: list[float], call_medians: list[float]) -> tuple[float, float]:
    positions = [d + 3.5 for d in depths]
    per_token = [m / 8.0 for m in call_medians]
    span = positions[-1] - positions[0]
    slope = max(0.0, (per_token[-1] - per_token[0]) / span) if span > 0 else 0.0
    intercept = per_token[0] - slope * positions[0]
    raise_by = max([0.0] + [t - (intercept + slope * p) for p, t in zip(positions, per_token)])
    return intercept + raise_by, slope


def profile_from_medians(med: dict[tuple[str, int], float]) -> dict[str, float]:
    out: dict[str, float] = {}
    out["LayerCostMs"], out["LayerCostPerPositionMs"] = chunk1_fit(
        list(CHUNK1_DEPTHS), [med[("layer", d)] for d in CHUNK1_DEPTHS])
    out["PromptTokenCostMs"], out["PromptTokenCostPerPositionMs"] = chunk1_fit(
        list(CHUNK1_DEPTHS), [med[("prompt_1", d)] for d in CHUNK1_DEPTHS])
    out["BatchedPromptTokenCostMs"], out["BatchedPromptTokenCostPerPositionMs"] = batched_fit(
        list(BATCHED_DEPTHS), [med[("prompt_8", d)] for d in BATCHED_DEPTHS])
    for field, key in SCALAR_ARMS.items():
        out[field] = med[key]
    return out


def decimals(text: str) -> int:
    return len(text.split(".", 1)[1]) if "." in text else 0


SLACK_MS = 1e-8  # contract item 13 (D-SLM8147): CSV times print at 9 decimals; reasoned, not executed


def suggestion_matches(text: str, exact: float, field: str) -> tuple[bool, str]:
    """A suggestion prints EXACTLY d decimals (3 ms, 6 per position) and is the round-up of its
    recomputation x: x - 1e-8 <= printed < x + 10^-d + 1e-8 (D-SLM8113, D-SLM8147)."""
    try:
        value = float(text)
    except ValueError:
        return False, f"'{text}' is not a number"
    want = 6 if "PerPosition" in field else 3
    d = decimals(text)
    if d != want:
        return False, f"'{text}' prints {d} decimals, not exactly {want}"
    ok = exact - SLACK_MS <= value < exact + 10.0 ** -d + SLACK_MS
    return ok, f"printed {text}, recomputed {exact:.12f}, difference {value - exact:.3e}"


def row_matches(text: str, exact: float) -> tuple[bool, str]:
    """A row median or IQR prints at least 3 decimals, round-to-nearest:
    |printed - x| <= 0.5 * 10^-d + 1e-8 (contract item 13, symmetric)."""
    try:
        value = float(text)
    except ValueError:
        return False, f"'{text}' is not a number"
    d = decimals(text)
    if d < 3:
        return False, f"'{text}' prints {d} decimals, fewer than 3"
    ok = abs(value - exact) <= 0.5 * 10.0 ** -d + SLACK_MS
    return ok, f"printed {text}, recomputed {exact:.12f}, difference {value - exact:.3e}"


def ceil_at(value: float, d: int) -> float:
    return math.ceil(value * 10 ** d - 1e-9) / 10 ** d


# --- Unreal log --------------------------------------------------------------------------------

def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        return data.decode("utf-16")
    return data.decode("utf-8-sig", errors="replace")


def estimate_runs(log: Path) -> list[list[str]]:
    """Each run is the report lines from one header to the line before the next header."""
    runs: list[list[str]] = []
    for line in read_text(log).splitlines():
        # Failed automation cells echo captured log lines as failure diagnostics. They are
        # copies of a run, not another command invocation or another evidence directory.
        if "LogAutomationController:" in line:
            continue
        at = line.find(PREFIX)
        if at < 0:
            continue
        body = line[at:]
        if body.startswith(HEADER):
            runs.append([])
        if runs:
            runs[-1].append(body)
    return runs


def estimate_run_time_bounds(log: Path) -> list[float | None]:
    """Wall-time upper bounds independent of the CSV's shared monotonic-clock origin."""
    runs = []
    timestamp = re.compile(r"\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]")
    for line in read_text(log).splitlines():
        if "LogAutomationController:" in line or PREFIX not in line:
            continue
        body = line[line.index(PREFIX):]
        match = timestamp.search(line[:line.index(PREFIX)])
        try:
            time = datetime.strptime(match[1], "%Y.%m.%d-%H.%M.%S:%f") if match else None
        except ValueError:
            time = None
        if body.startswith(HEADER):
            runs.append({"start": time, "ends": []})
        elif (runs and body.startswith(PREFIX + "run status=")
              and key_values(body[len(PREFIX):]).get("status") == "complete"):
            runs[-1]["ends"].append(time)
    bounds = []
    for run in runs:
        start, ends = run["start"], run["ends"]
        valid = start is not None and len(ends) == 1 and ends[0] is not None and ends[0] >= start
        # Original Unreal log timestamps resolve to 1 ms; CSV coordinates print 9 decimals.
        # This serialization allowance is independent of candidate timing measurements.
        bounds.append((ends[0] - start).total_seconds() + 0.001000001 if valid else None)
    return bounds


def intervals_within_run(rows: list[dict], bound: float | None) -> bool:
    if not rows or bound is None:
        return False
    try:
        intervals = [(float(row["start_s"]), float(row["end_s"])) for row in rows]
    except (KeyError, TypeError, ValueError):
        return False
    return all(math.isfinite(start) and math.isfinite(end) and 0.0 <= start <= end <= bound
               for start, end in intervals)


def key_values(text: str) -> dict[str, str]:
    return dict(token.split("=", 1) for token in text.split() if "=" in token)


def parse_run(lines: list[str]) -> dict:
    run: dict = {"rows": {}, "suggested": {0: {}, 1: {}}, "loads": {}, "lines": lines, "progress": set()}
    for index, line in enumerate(lines):
        body = line[len(PREFIX):]
        kv = key_values(body)
        if body.startswith("run id="):
            run["run"] = kv
        elif body.startswith("run status="):
            run["status"] = kv.get("status")
            run["status_index"] = index
        elif body.startswith("progress "):
            run["progress"].add(kv.get("mode"))
            if kv.get("phase") == "warm-up":
                run.setdefault("warmup", {}).setdefault(kv.get("mode"), (index, kv))
            if kv.get("phase") == "timed":
                run.setdefault("timed", {}).setdefault(kv.get("mode"), index)
        elif body.startswith("row "):
            run["rows"][(int(kv["mode"]), kv["arm"], int(kv["depth"]))] = kv
        elif body.startswith("suggested "):
            mode = int(kv.pop("mode"))
            run["suggested"][mode].update(kv)
        elif body.startswith("load "):
            run["loads"][(kv.get("phase"), kv.get("mode"))] = kv
            if kv.get("phase") == "post":
                run["post_index"] = index
        elif body.startswith("coverage "):
            run["coverage"] = kv
        elif body.startswith("lock "):
            run["lock"] = kv.get("state")
        elif body.startswith("files "):
            run["files"] = body[len("files "):].strip().strip('"')
        elif body.startswith("wrote "):
            run["wrote_index"] = index
    return run


def select_run(log: Path, index: int) -> dict:
    runs = estimate_runs(log)
    if not runs:
        raise SystemExit(f"no estimate run (no '{HEADER}' line) in {log}")
    if not -len(runs) <= index < len(runs):
        raise SystemExit(f"{log} holds {len(runs)} estimate run(s); --run-index {index} is out of range")
    return parse_run(runs[index])


def read_calls(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        header = handle.readline().strip()
        if header != CALLS_HEADER:
            raise ValueError(f"{path} header is '{header}'")
        return list(csv.DictReader(handle, fieldnames=CALLS_HEADER.split(",")))


def group_calls(rows: list[dict]) -> dict[tuple[str, int], list[float]]:
    groups: dict[tuple[str, int], list[float]] = defaultdict(list)
    for row in rows:
        groups[(row["arm"], int(row["depth"]))].append(float(row["elapsed_ms"]))
    return groups


# --- Subcommands -------------------------------------------------------------------------------

LOCK_TOKENS = ("absent", "own", "foreign", "not-configured")


def check_samples(c: "Checks", path: Path, logical_processors: int, require_own_lock: bool) -> None:
    """D-SLM8142 over the retained samples.csv: foreign_pct = max(0, box_pct - own_pct) within 2e-6
    on every sample; each mode's workload-phase median own_pct >= 0.5 * 100 / N (N from the box,
    never from a reading); lock_state uses the report's four tokens, and reads "own" on every
    sample under the box driver's held build lock."""
    try:
        with path.open(newline="", encoding="utf-8-sig") as handle:
            samples = list(csv.DictReader(handle))
    except OSError as error:
        c.that(False, f"samples.csv readable: {error}")
        return
    needed = {"mode", "phase", "box_pct", "own_pct", "foreign_pct", "lock_state"}
    if not c.that(bool(samples) and needed <= set(samples[0]), f"samples.csv carries {sorted(needed)}"):
        return
    worst = max(abs(float(s["foreign_pct"]) - max(0.0, float(s["box_pct"]) - float(s["own_pct"]))) for s in samples)
    c.that(worst <= 2e-6, f"every sample's foreign_pct = max(0, box_pct - own_pct) within 2e-6 (worst {worst:.3g})")
    bad_tokens = sorted({s["lock_state"] for s in samples} - set(LOCK_TOKENS))
    c.that(not bad_tokens, f"lock_state uses only {LOCK_TOKENS} (found {bad_tokens})")
    if require_own_lock:
        not_own = sum(1 for s in samples if s["lock_state"] != "own")
        c.that(not_own == 0, f"under the held build lock every sample's lock_state is own ({not_own} not)")
    floor = 0.5 * 100.0 / logical_processors
    for mode in ("0", "1"):
        own = [float(s["own_pct"]) for s in samples if s["phase"] == "workload" and s["mode"] == mode]
        median = statistics.median(own) if own else -1.0
        c.that(bool(own) and median >= floor,
               f"mode {mode} workload median own_pct {median:.4f}% >= 0.5 x 100/N = {floor:.4f}% (N = {logical_processors})")


def cmd_report(args: argparse.Namespace) -> int:
    c = Checks()
    run = select_run(Path(args.log), args.run_index)
    lines = run["lines"]
    c.that(lines[0].startswith(HEADER), "the estimate label is the run's first report line")
    for line in lines:
        for word in FORBIDDEN:
            c.that(word not in line, f"no '{word}' in the estimate path: {line[:120]}")
    meta = run.get("run", {})
    c.that(re.fullmatch(r"[0-9a-f]{64}", meta.get("artifact_hash", "")) is not None, "run reports a 64-hex artifact hash")
    if args.artifact_hash:
        c.that(meta.get("artifact_hash") == args.artifact_hash.lower(), "run reports the expected artifact hash")
    c.that(meta.get("FinishParallelTasks") == str(args.tasks), f"run reports FinishParallelTasks={args.tasks}")
    for key in ("id", "engine", "plugin"):
        c.that(bool(meta.get(key)), f"run reports {key}")
    c.that({"0", "1"} <= run["progress"], "progress is printed for both modes")
    c.that(run.get("status") == "complete", "run status is complete")
    joined = "\n".join(lines)
    c.that("not a confidence interval" in joined, "IQR disclosure present")
    c.that("250 ms interval averages" in joined, "foreign-load disclosure present")
    for mode in (0, 1):
        c.that(sum(1 for key in run["rows"] if key[0] == mode) == len(expected_groups()), f"mode {mode} has exactly 22 rows")
        for arm, depth in expected_groups():
            row = run["rows"].get((mode, arm, depth))
            ok = row is not None and row.get("calls") == str(expected_calls(arm))
            c.that(ok, f"mode {mode} {arm}@{depth} row present with {expected_calls(arm)} calls")
        c.that(set(run["suggested"][mode]) == set(FIELDS), f"mode {mode} suggests exactly the thirteen fields")
        for field in FIELDS:
            want = 6 if "PerPosition" in field else 3
            c.that(decimals(run["suggested"][mode].get(field, "")) == want,
                   f"mode {mode} suggested {field} prints exactly {want} decimals")
        try:
            c.that(float(run["suggested"][mode]["BatchedPromptTokenCostMs"]) <= float(run["suggested"][mode]["PromptTokenCostMs"]),
                   f"mode {mode} suggested BatchedPromptTokenCostMs does not exceed PromptTokenCostMs")
        except (KeyError, ValueError):
            c.that(False, f"mode {mode} suggests both prompt intercepts")
        timed_calls = sum(expected_calls(arm) for arm, _ in expected_groups())
        warm = run.get("warmup", {}).get(str(mode))
        timed_at = run.get("timed", {}).get(str(mode))
        c.that(warm is not None and warm[1].get("reps") == "5" and warm[1].get("calls") == str(5 * timed_calls // 12)
               and timed_at is not None and warm[0] < timed_at,
               f"mode {mode} warm-up line reps=5 calls={5 * timed_calls // 12} precedes its first timed line")
    for key, name in ((("pre", None), "pre"), (("workload", "0"), "mode-0 workload"),
                      (("workload", "1"), "mode-1 workload"), (("post", None), "post")):
        load = run["loads"].get(key)
        ok = False
        if load:
            try:
                med, p95, mx = float(load["median_pct"]), float(load["p95_pct"]), float(load["max_pct"])
                ok = int(load["samples"]) > 0 and 0 <= med <= p95 <= mx <= 100
            except (KeyError, ValueError):
                ok = False
        c.that(ok, f"{name} load summary reports samples and ordered median <= p95 <= max")
        if args.min_workload_foreign is not None and key[0] == "workload" and load:
            c.that(float(load.get("median_pct", -1)) >= args.min_workload_foreign,
                   f"{name} discloses the elevated foreign load (median {load.get('median_pct')} >= {args.min_workload_foreign})")
    c.that(run.get("coverage", {}).get("uncovered_calls") == "0", "no uncovered timed call")
    c.that(run.get("lock") in ("absent", "own", "foreign", "not-configured"), "lock observation reported")
    files = Path(run.get("files", ""))
    c.that(files.is_dir(), f"diagnostic directory exists: {files}")
    check_samples(c, files / "samples.csv", args.logical_processors, args.require_own_lock)
    if args.expect_write:
        c.that("wrote_index" in run and run["wrote_index"] > run.get("post_index", 1 << 30), "the opt-in write is reported after the post window")
    if args.expect_no_write:
        c.that("wrote_index" not in run, "nothing is written without --write-ini")
    return c.finish("report")


def cmd_recompute(args: argparse.Namespace) -> int:
    c = Checks()
    run = select_run(Path(args.log), args.run_index)
    files = Path(run.get("files", ""))
    for mode in (0, 1):
        csv_path = files / f"mode{mode}.calls.csv"
        try:
            calls = read_calls(csv_path)
        except (OSError, ValueError) as error:
            print(f"cannot read {csv_path}: {error}")
            return 2
        c.that(all(row["mode"] == str(mode) for row in calls), f"every call in {csv_path.name} is labelled mode {mode}")
        c.that(all(math.isfinite(float(r["elapsed_ms"])) and float(r["elapsed_ms"]) > 0 for r in calls),
               f"every retained mode-{mode} call has a finite positive duration")
        groups = group_calls(calls)
        c.that(set(groups) == set(expected_groups()), f"mode {mode} CSV holds exactly the 22 arm/depth groups")
        medians: dict[tuple[str, int], float] = {}
        for arm, depth in expected_groups():
            values = groups.get((arm, depth), [])
            row = run["rows"].get((mode, arm, depth), {})
            c.that(len(values) == expected_calls(arm) and row.get("calls") == str(len(values)),
                   f"mode {mode} {arm}@{depth}: the row's call count equals the retained calls ({len(values)})")
            if not values:
                continue
            medians[(arm, depth)] = statistics.median(values)
            ok, why = row_matches(row.get("median_ms", "x"), medians[(arm, depth)])
            c.that(ok, f"mode {mode} {arm}@{depth} median: {why}")
            ok, why = row_matches(row.get("iqr_ms", "x"), percentile(values, 0.75) - percentile(values, 0.25))
            c.that(ok, f"mode {mode} {arm}@{depth} IQR: {why}")
        if len(medians) != len(expected_groups()):
            continue
        exact = profile_from_medians(medians)
        for field in FIELDS:
            ok, why = suggestion_matches(run["suggested"][mode].get(field, "x"), exact[field], field)
            c.that(ok, f"mode {mode} suggested {field}: {why}")
    return c.finish("recompute")


def cmd_fresh(args: argparse.Namespace) -> int:
    c = Checks()
    runs = [parse_run(lines) for lines in estimate_runs(Path(args.log))]
    c.that(len(runs) >= 2, f"the log holds two or more estimate runs ({len(runs)})")
    ids = [r.get("run", {}).get("id") for r in runs]
    dirs = [r.get("files") for r in runs]
    bounds = estimate_run_time_bounds(Path(args.log))
    c.that(len(bounds) == len(runs), "each parsed run has its own wall-time envelope")
    c.that(len(set(ids)) == len(ids) and None not in ids, f"run ids are distinct: {ids}")
    c.that(len(set(dirs)) == len(dirs) and None not in dirs, f"diagnostic directories are distinct: {dirs}")
    for index, run in enumerate(runs):
        bound = bounds[index] if index < len(bounds) else None
        c.that(run.get("status") == "complete", f"run {index} status is exactly complete")
        c.that(bound is not None, f"run {index} has one timestamped completion after its timestamped header")
        files = Path(run.get("files", ""))
        for mode in (0, 1):
            try:
                calls = read_calls(files / f"mode{mode}.calls.csv")
            except (OSError, ValueError) as error:
                c.that(False, f"run {index} mode {mode} calls readable: {error}")
                continue
            c.that([int(r["call_id"]) for r in calls] == list(range(len(calls))), f"run {index} mode {mode} call ids number from zero")
            expected_block = str(run.get("run", {}).get("id", "")) + f"-m{mode}"
            c.that(bool(calls) and all(row.get("block") == expected_block for row in calls),
                   f"run {index} mode {mode} calls carry the current run/mode block identity")
            c.that(intervals_within_run(calls, bound), f"run {index} mode {mode} call intervals are finite, ordered and inside its own wall-time envelope")
        try:
            with (files / "samples.csv").open(newline="", encoding="utf-8-sig") as handle:
                samples = list(csv.DictReader(handle))
            c.that([int(s["sample_id"]) for s in samples] == list(range(len(samples))), f"run {index} samples number from zero")
            c.that(str(len(samples)) == run.get("coverage", {}).get("samples"), f"run {index} retains exactly the samples it reports")
            c.that(bool(samples) and all(row.get("block") == run.get("run", {}).get("id") for row in samples),
                   f"run {index} samples carry the current run block identity")
            c.that(intervals_within_run(samples, bound), f"run {index} sample intervals are finite, ordered and inside its own wall-time envelope")
        except (OSError, KeyError, ValueError) as error:
            c.that(False, f"run {index} samples readable: {error}")
    return c.finish("fresh")


def read_plan_table(plan: Path) -> dict[int, dict[str, float]]:
    table: dict[int, dict[str, float]] = {0: {}, 1: {}}
    pattern = re.compile(r"^\s*\|\s*`(\w+)`\s*\|\s*([0-9.]+)\s*\|\s*([0-9.]+)\s*\|")
    for line in read_text(plan).splitlines():
        match = pattern.match(line)
        if match and match.group(1) in FIELDS and match.group(1) not in table[0]:
            table[0][match.group(1)] = float(match.group(2))
            table[1][match.group(1)] = float(match.group(3))
    return table


def cmd_provenance(args: argparse.Namespace) -> int:
    c = Checks()
    root = Path(args.artifact_dir)
    sums: dict[str, str] = {}
    try:
        for line in (root / "SHA256SUMS.txt").read_text(encoding="utf-8-sig").splitlines():
            parts = line.strip().split()
            if len(parts) >= 2:
                sums[parts[-1].lstrip("*").replace("\\", "/").split("/")[-1]] = parts[0].lower()
    except OSError as error:
        print(f"cannot read SHA256SUMS.txt: {error}")
        return 2
    blocks: dict[str, list[dict]] = {}
    for mode, names in BLOCKS.items():
        for name in names:
            path = root / f"{name}.calls.csv"
            digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else ""
            c.that(sums.get(path.name) == digest and digest != "", f"{path.name} matches SHA256SUMS.txt")
            try:
                blocks[name] = read_calls(path)
            except (OSError, ValueError) as error:
                print(f"cannot read {path}: {error}")
                return 2
    fixture = json.loads(Path(args.profiles).read_text(encoding="utf-8"))["profiles"]
    plan = read_plan_table(Path(args.plan)) if args.plan else None
    for mode, names in BLOCKS.items():
        per_block = {name: group_calls(blocks[name]) for name in names}
        medians: dict[tuple[str, int], float] = {}
        for key in expected_groups():
            block_medians = [statistics.median(per_block[name][key]) for name in names if per_block[name].get(key)]
            c.that(len(block_medians) == 3, f"mode {mode} {key[0]}@{key[1]} has a median in each of the three blocks")
            if block_medians:
                medians[key] = statistics.median(block_medians)
        if len(medians) != len(expected_groups()):
            continue
        exact = profile_from_medians(medians)
        for field in FIELDS:
            digits = 6 if "PerPosition" in field else 3
            recomputed = ceil_at(exact[field], digits)
            pinned = float(fixture[str(mode)][field])
            c.that(abs(recomputed - pinned) < 1e-12,
                   f"mode {mode} {field}: recomputed {recomputed:.{digits}f} (exact {exact[field]:.9f}), fixture {pinned}")
            if plan is not None:
                c.that(abs(plan[mode].get(field, float("nan")) - pinned) < 1e-12,
                       f"mode {mode} {field}: plan table {plan[mode].get(field)} equals the fixture {pinned}")
    return c.finish("provenance")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    report = sub.add_parser("report")
    report.add_argument("--log", required=True)
    report.add_argument("--run-index", type=int, default=-1)
    report.add_argument("--artifact-hash")
    report.add_argument("--min-workload-foreign", type=float)
    report.add_argument("--expect-write", action="store_true")
    report.add_argument("--expect-no-write", action="store_true")
    report.add_argument("--tasks", type=int, default=4, help="the FinishParallelTasks the run must report")
    report.add_argument("--logical-processors", type=int, default=os.cpu_count() or 1,
                        help="N for the own-share floor (Windows: GetActiveProcessorCount over all groups)")
    report.add_argument("--require-own-lock", action="store_true",
                        help="the run was made under the box driver's held SSU build lock")
    recompute = sub.add_parser("recompute")
    recompute.add_argument("--log", required=True)
    recompute.add_argument("--run-index", type=int, default=-1)
    fresh = sub.add_parser("fresh")
    fresh.add_argument("--log", required=True)
    provenance = sub.add_parser("provenance")
    provenance.add_argument("--artifact-dir", required=True)
    provenance.add_argument("--profiles", required=True)
    provenance.add_argument("--plan")
    args = parser.parse_args(argv)
    return {"report": cmd_report, "recompute": cmd_recompute, "fresh": cmd_fresh,
            "provenance": cmd_provenance}[args.command](args)


if __name__ == "__main__":
    # A malformed report line or file is an unreadable input (exit 2), never a quiet pass and
    # never confused with a check that ran and failed (exit 1).
    try:
        sys.exit(main(sys.argv[1:]))
    except (KeyError, ValueError, OSError, IndexError) as error:
        print(f"unreadable input: {type(error).__name__}: {error}")
        sys.exit(2)
