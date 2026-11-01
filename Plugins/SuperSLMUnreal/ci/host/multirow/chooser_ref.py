"""Cell 10.6's independent reference: the multi-row chooser in exact rational arithmetic (multi-row GPU plan rev 1.3.2,
§5.1-§5.3, §4.7, MR-D11; red-suite-mr6.md).

The plugin's chooser (Source/SuperSLMUnreal/Private/Core/SuperSLMGpuRowsChooser.h) works in int64. This file works in
Python's fractions and integers, written from the plan's text and never from the chooser's code, so the chooser is
graded against an answer it did not compute (10.6: "checked against an independent exact-rational reference").

  python3 chooser_ref.py grid  > grid.txt     the 10.6 grid, one case per two lines (`in ...`, `out ...`)
  python3 chooser_ref.py check grid.txt       exit 0 when grid.txt equals what this file generates now

The float inputs are float32 values (the plan's f32, rev 1.3 F4): each is written with 9 significant digits, which
round-trips through strtof exactly.
"""
import math
import struct
import sys
from fractions import Fraction as Fr

MAX_ROWS = 8
MIN_EXACT_ROWS = 4


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def to_us(ms, is_cost):
    # §5.1 with ruling 39 Q9: v = ms x 1000 formed in double from the double field, n = llround(v); n when
    # |v - n| <= 0.01, else costs round up and B rounds down. Done in exact rationals on the double the C++ forms.
    v = Fr(float(ms) * 1000.0)
    n = math.floor(v + Fr(1, 2)) if v >= 0 else -math.floor(-v + Fr(1, 2))  # llround: half away from zero
    if abs(v - n) <= Fr(1, 100):
        return n
    return math.ceil(v) if is_cost else math.floor(v)


def plan(c):
    """c: dict of inputs. Returns the expected outputs (dict)."""
    L, m, lpt = c["L"], c["m"], c["lpt"]
    c1 = to_us(c["c1"], True)
    s = to_us(c["s"], True)
    knob = c["b"] != 0
    b = min(to_us(c["b"], False), to_us(c["nt"], False)) if knob else c1 + s
    C = [math.ceil(Fr(r * c1, 1000)) for r in c["r"]]
    G = [math.ceil(Fr(g * c1, 1000)) for g in c["g"]]
    room = b - s  # ruling 39 Q1: every l is computed against B_eff - Scene

    def layer_price(M, l):  # exact rational price of l layers of an M-row chunk, without the scene
        if M == 1:
            return Fr(l * C[0], L)  # Composed's unit count; l(1) itself is below (E-14: the margin with the knob set)
        return Fr(l * C[M - 1] * (1000 + m), L * 1000)

    def logits_price(M):
        return Fr(G[M - 1] * (1000 + m), 1000)

    out = {"c1us": c1, "sus": s, "bus": b, "C": C, "G": G, "below": 1 if room < c1 else 0}
    # Ruling 39 Q5: an estimated table in a shipping build, or bGpuMultiRow false (P4.4), is the one-row plan.
    status = 0 if c["meas"] else (2 if c["ship"] else 1)
    out["status"] = status
    multirow = c["mr"] == 1 and status != 2
    out["mron"] = 1 if multirow else 0
    # M = 1 (ruling 39 Q2; erratum E-14, ruling 53): with the knob set, l(1) = min(L, max(1, floor(L x room x 1000 /
    # (C(1) x (1000 + m))))), the margin M >= 2 carries. With the knob at 0 there is no budget to protect and l(1) = L
    # (1.17's whole token, P4.4): room = C(1) there, and the margin is not applied.
    if room <= 0:
        l1 = 1
    elif knob:
        l1 = min(L, max(1, math.floor(Fr(L * room * 1000, c1 * (1000 + m)))))
    else:
        l1 = min(L, max(1, math.floor(Fr(L * room, c1))))
    ls, Ds, Dxs = [l1], [-(-L // l1)], [0]
    for M in range(2, MAX_ROWS + 1):
        l = 0
        while l < L and layer_price(M, l + 1) <= room:
            l += 1
        D = -(-L // l) if l > 0 else 0
        Dx = 0
        if l > 0:
            last = L - (D - 1) * l
            if layer_price(M, last) + logits_price(M) <= room:
                Dx = D
            elif layer_price(M, 1) + logits_price(M) <= room:
                # Ruling 42 R2: the engine runs the logits stage in the `ready` that finishes the last layer, so §4.7
                # step 3's "tick of its own" is not available: the last slice shrinks to the k >= 1 layers that fit
                # with the logits stage, which is D + 1 ticks, and is runnable only when one layer plus logits fits.
                Dx = D + 1
        ls.append(l)
        Ds.append(D)
        Dxs.append(Dx)
    out["l"], out["D"], out["Dx"] = ls, Ds, Dxs
    # Composed's M = 1 slice: l <= l(1) with the knob set, l <= L without (one layer always admitted).
    cap1 = l1 if knob else L
    one = (1, min(lpt, cap1), min(lpt, cap1))
    if not multirow:
        out["mmax"], out["ml"] = 1, l1
        out["cr"], out["cl"], out["cu"] = one
        out["exon"], out["mexact"] = 0, 0
        out["reason"] = 0 if not c["ex"] else -1  # chosen under the one-row plan: the reason is not graded
        return out
    best = None
    for M in range(1, MAX_ROWS + 1):
        if ls[M - 1] == 0:
            continue
        val = Fr(M, Ds[M - 1])
        if best is None or val > best[0]:  # strict: ties go to fewer rows
            best = (val, M)
    out["mmax"] = best[1]
    out["ml"] = ls[best[1] - 1]
    cb = (one[1], 1, one[1], one[2])
    for M in range(2, MAX_ROWS + 1):
        for l in range(1, L + 1):
            units = math.ceil(Fr(l * C[M - 1] * (1000 + m), C[0] * 1000))
            if units > lpt:
                continue
            if knob and layer_price(M, l) > room:
                continue
            rl = M * l
            if rl > cb[0]:
                cb = (rl, M, l, units)
    out["cr"], out["cl"], out["cu"] = cb[1], cb[2], cb[3]
    # Exact (OneCall; the Composed path verifies at the same M_exact, ruling 39 Q3).
    if not c["ex"]:
        out["exon"], out["reason"], out["mexact"] = 0, 0, 0
    elif not c["head"]:
        out["exon"], out["reason"], out["mexact"] = 0, 2, 0
    else:
        eb = None
        for M in range(MIN_EXACT_ROWS, MAX_ROWS + 1):
            if Dxs[M - 1] == 0:
                continue
            val = Fr(M, Dxs[M - 1])
            if eb is None or val > eb[0]:
                eb = (val, M)
        if eb is None:  # ruling 39 Q4: M = 3, then 2, else Off
            eb = next(((None, M) for M in (3, 2) if Dxs[M - 1] > 0), None)
        out["exon"], out["reason"], out["mexact"] = (1, 1, eb[1]) if eb else (0, 3, 0)
    return out


# ------------------------------------------------------------------------------------------------ the grid
ESTIMATED_R = [1000, 1275, 1565, 1855, 2142, 2428, 2714, 3000]  # engine gpu_rows_cost_table.h at MR4 (estimates)
ESTIMATED_G = [250, 319, 392, 464, 536, 607, 679, 750]
SPIKE_R = [1000, 1275, 1855, 1855, 3000, 3000, 3000, 3000]  # §4.7's COST points held flat between them (knife edges)
G0 = [0] * 8
MEASURED_LIKE_G = [48, 48, 48, 48, 48, 48, 48, 48]  # ruling 37's g(1) at 9493b927 (0.5B), held flat
# Ruling 37's measured table (SuperSLM 277682d4, gpu_rows_cost_table.h, RX 7900 XT): the merged per-M maximum arrays,
# the only ones the plugin uses (ruling 39 Q11).
MEASURED_R = [1000, 1140, 1604, 1618, 2479, 2491, 2503, 2521]
MEASURED_G = [48, 95, 122, 162, 200, 238, 275, 313]
TABLES = [("estimated", ESTIMATED_R, ESTIMATED_G), ("spike-g0", SPIKE_R, G0), ("spike-g25", SPIKE_R, [250, 319, 464, 464, 750, 750, 750, 750]),
          ("est-g48", ESTIMATED_R, MEASURED_LIKE_G), ("measured", MEASURED_R, MEASURED_G)]
# Budgets in ms (double): 0 (one row); below one row (1.0, 2.0, 4.0: ruling 39 Q2); the float-configured inputs of rev
# 1.3 F4; NominalTickMs itself; one above it (clamped); and 6.2005 (rounds down).
BUDGETS = [0.0, 1.0, 2.0, 4.0, 6.2, 6.2005, 7.9, 11.5, 12.4, 16.7, 18.6]
SCENES = [0.0, 1.5]  # ruling 39 Q1: Room = B_eff - Scene (1.5 > B = 1.0: Room <= 0, still one layer)
MARGINS = [0, 150, 300, 2000]  # 2000: no M in 4..8 fits Exact's logits stage at most budgets (ruling 39 Q4)
LAYERS = [24, 28]
LPT = [1, 2, 4, 8]


def base_case(**kw):
    c = dict(table="estimated", r=ESTIMATED_R, g=ESTIMATED_G, meas=1, c1=6.2, b=0.0, s=0.0, nt=16.7, m=150, mr=1, L=24, lpt=4,
             dpl=24, path=0, ex=1, head=1, ship=0)
    c.update(kw)
    return c


def cases():
    for tname, r, g in TABLES:
        for L in LAYERS:
            for b in BUDGETS:
                for sc in SCENES:
                    for m in MARGINS:
                        for lpt in LPT:
                            # The plan reports both paths' choices from one configuration; path selects which one runs.
                            yield base_case(table=tname, r=r, g=g, b=b, s=sc, m=m, L=L, lpt=lpt)
    # N4 / ruling 39 Q5 and P4.4: provenance, build and the switch, over a slice of budgets and margins.
    for meas, ship, mr in ((0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 0), (0, 1, 0)):
        for b in (0.0, 2.0, 11.5, 16.7):
            for m in (0, 150):
                for ex, head in ((1, 1), (0, 1), (1, 0)):
                    yield base_case(table="n4-meas%d-ship%d-mr%d" % (meas, ship, mr), meas=meas, ship=ship, mr=mr, b=b, m=m, ex=ex, head=head)
    # A non-default GpuRowCostMs (6.6 ms, the plan's range) and a 7.11 (b)/(c) sweep on the measured table.
    for b in (0.0, 4.0, 16.7):
        for ex, head in ((0, 1), (1, 0), (1, 1)):
            yield base_case(table="measured-c6.6", r=MEASURED_R, g=MEASURED_G, c1=6.6, b=b, ex=ex, head=head)


def fmt_f(x):
    return repr(float(x))


def line_in(c):
    return ("in table=%s r=%s g=%s meas=%d c1=%s b=%s s=%s nt=%s m=%d mr=%d L=%d lpt=%d dpl=%d path=%d ex=%d head=%d ship=%d" % (
        c["table"], ",".join(map(str, c["r"])), ",".join(map(str, c["g"])), c["meas"], fmt_f(c["c1"]), fmt_f(c["b"]), fmt_f(c["s"]),
        fmt_f(c["nt"]), c["m"], c["mr"], c["L"], c["lpt"], c["dpl"], c["path"], c["ex"], c["head"], c["ship"]))


def line_out(o):
    j = lambda v: ",".join(map(str, v))
    return ("out status=%d mron=%d below=%d c1us=%d sus=%d bus=%d C=%s G=%s l=%s D=%s Dx=%s mmax=%d ml=%d cr=%d cl=%d cu=%d exon=%d reason=%d mexact=%d" % (
        o["status"], o["mron"], o["below"], o["c1us"], o["sus"], o["bus"], j(o["C"]), j(o["G"]), j(o["l"]), j(o["D"]), j(o["Dx"]), o["mmax"],
        o["ml"], o["cr"], o["cl"], o["cu"], o["exon"], o["reason"], o["mexact"]))


def grid_text():
    lines = ["# 10.6 grid (chooser_ref.py; red-suite-mr6.md; rulings 39, 42 and 53). Regenerate with: python3 chooser_ref.py grid > grid.txt"]
    for c in cases():
        lines.append(line_in(c))
        lines.append(line_out(plan(c)))
    return "\n".join(lines) + "\n"


def self_check():
    # The plan's own worked figures (§4.7's table, §5.2, §5.3), so the reference is checked against the plan's text.
    base = dict(r=[1000, 1275, 1855, 1855, 3000, 3000, 3000, 3000], meas=1, c1=6.2, b=0.0, s=0.0, nt=16.7, mr=1, L=24, lpt=4,
                dpl=24, path=0, ex=1, head=1, ship=0)
    spike = {2: 7900, 4: 11500, 8: 18600}
    def at(m, gamma):
        c = dict(base, m=m)
        # gamma = G / C(M): g(M) = gamma x r(M)
        c["g"] = [math.ceil(Fr(gamma) * x) for x in c["r"]]
        return plan(c)
    # C(M) in us equals COST's spike figures (r rounds up: 7,905 for 7,900 is the table's own rounding).
    o = at(0, 0)
    assert o["C"][0] == 6200 and o["C"][3] == 11501 and o["C"][7] == 18600, o["C"]
    # §4.7's table: (M, m, gamma) -> (l, D_x). Ruling 42 R2 changes one row: at M = 8, m = 150, gamma = 0.25 the
    # plan's 5 counted a logits-only tick; one layer plus logits (891.25 + 5,347.5 us) does not fit 6,200, so D_x = 0.
    want = {(2, 0, 0): (18, 2), (4, 0, 0): (12, 2), (8, 0, 0): (8, 3), (2, 0, .25): (18, 2), (4, 0, .25): (12, 3), (8, 0, .25): (8, 4),
            (2, 150, 0): (16, 2), (4, 150, 0): (11, 3), (8, 150, 0): (6, 4), (2, 150, .25): (16, 2), (4, 150, .25): (11, 3), (8, 150, .25): (6, 0)}
    for (M, m, gm), (l, dx) in want.items():
        o = at(m, Fr(gm).limit_denominator(4))
        assert (o["l"][M - 1], o["Dx"][M - 1]) == (l, dx), (M, m, gm, o["l"][M - 1], o["Dx"][M - 1])
    # §5.2: m = 150: M_max = 8 with D = 4; m = 0: D(8) = 3.
    assert at(150, 0)["mmax"] == 8 and at(150, 0)["D"][7] == 4
    assert at(0, 0)["D"][7] == 3
    # §5.3 (LayersPerTick 4): m = 150: M = 8, l = 1, 4 units; m = 0: the tie of 8 row-layers goes to M = 4, l = 2, 4 units.
    for m, want_c in ((150, (8, 1, 4)), (0, (4, 2, 4))):
        o = at(m, 0)
        assert (o["cr"], o["cl"], o["cu"]) == want_c, (m, o["cr"], o["cl"], o["cu"])
    # Rev 1.3 F4's conversions, applied to the double value (ruling 39 Q9); f32-configured values convert the same.
    for ms, us in ((6.2, 6200), (7.9, 7900), (11.5, 11500), (18.6, 18600)):
        assert to_us(ms, True) == us and to_us(ms, False) == us
        assert to_us(f32(ms), True) == us and to_us(f32(ms), False) == us
    assert to_us(6.2005, False) == 6200 and to_us(6.2005, True) == 6201
    # Ruling 39 Q2 with E-14's margin (ruling 53): B = 2 ms (below one row), m = 150: l(1) = floor(24 x 2000 x 1000 /
    # (6200 x 1150)) = 6, D(1) = 4; m = 0 keeps 39 Q2's 7; B below the scene: l(1) = 1.
    o = plan(dict(base, m=150, g=[0] * 8, b=2.0))
    assert (o["l"][0], o["D"][0], o["below"]) == (6, 4, 1), (o["l"][0], o["D"][0], o["below"])
    o = plan(dict(base, m=0, g=[0] * 8, b=2.0))
    assert (o["l"][0], o["D"][0]) == (7, 4), (o["l"][0], o["D"][0])
    # E-14: the knob at 0 keeps the whole token, margin or not (P4.4's 1.17 scheduling).
    o = plan(dict(base, m=150, g=[0] * 8, b=0.0))
    assert (o["l"][0], o["D"][0]) == (24, 1), (o["l"][0], o["D"][0])
    # Triage run 3's case: C(1) = 6.967 ms, B = 2 ms, m = 150: l(1) = floor(48,000,000,000 / 8,012,050) = 5, not 6.
    o = plan(dict(base, c1=6.967, m=150, g=[0] * 8, b=2.0))
    assert o["l"][0] == 5, o["l"][0]
    o = plan(dict(base, m=150, g=[0] * 8, b=1.0, s=1.5))
    assert (o["l"][0], o["D"][0], o["cr"], o["cl"]) == (1, 24, 1, 1)
    # Ruling 39 Q1: a scene share takes its time out of every l: B = 11.5, S = 1.5 leaves 10,000 us of room.
    o = plan(dict(base, m=0, g=[0] * 8, b=11.5, s=1.5))
    assert o["l"][3] == 24 * 10000 // 11501, o["l"][3]
    # Ruling 42 R2: a D + 1 schedule needs one layer plus logits to fit. The review's case (measured table, B = 0,
    # m = 2000): M = 8 has l = 2, D = 12 but one layer plus logits does not fit, so D_x(8) = 0 and Q4's ladder runs.
    o = plan(dict(base, r=MEASURED_R, g=MEASURED_G, m=2000))
    for M in range(2, MAX_ROWS + 1):
        if o["Dx"][M - 1] == o["D"][M - 1] + 1:
            assert Fr(o["C"][M - 1] * 3000, 24 * 1000) + Fr(o["G"][M - 1] * 3000, 1000) <= o["bus"], (M, o["Dx"])
    assert o["Dx"][7] == 0 and o["mexact"] != 8, (o["Dx"], o["mexact"])
    # Ruling 39 Q4: m = 2000 at one row's budget fits no M in 4..8; M = 3, then 2, else Off.
    o = plan(dict(base, r=ESTIMATED_R, g=ESTIMATED_G, m=2000))
    assert o["mexact"] in (0, 2, 3) and (o["reason"] == 3) == (o["mexact"] == 0), (o["Dx"], o["mexact"], o["reason"])


def main(argv):
    self_check()
    if len(argv) == 2 and argv[1] == "grid":
        sys.stdout.write(grid_text())
        return 0
    if len(argv) == 3 and argv[1] == "check":
        have = open(argv[2]).read()
        if have != grid_text():
            print("grid.txt differs from chooser_ref.py's output: regenerate it")
            return 1
        print("grid.txt matches chooser_ref.py (%d cases)" % (have.count("\nin ")))
        return 0
    if len(argv) == 2 and argv[1] == "selfcheck":
        print("chooser_ref.py: the plan's worked figures hold")
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
