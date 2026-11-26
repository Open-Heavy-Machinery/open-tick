#!/usr/bin/env python3
"""Analysis of an M2 (sync) CSV: round trip, time inside the bridge, and the
clock mapping through the lowest-delay samples at three thresholds.

    m2_analysis.py m2_sync.csv > m2_sync_analysis.txt

Input columns: t1_ns, t4_ns (host CLOCK_MONOTONIC), t_rx_tick, t_tx_tick
(bridge ticks at 27 MHz: T2 and T3). For each sample:
    delay  = (T4 - T1) - (T3 - T2)
    offset = ((T2 - T1) + (T3 - T4)) / 2      (bridge time minus host time)
For each threshold, a least-squares line offset(t1) through the samples whose
delay is below it gives the drift (ppm) and the residuals. Percentiles are
nearest-rank, as in pingpong_bench.py. Standard library only.
"""
import csv
import math
import sys

F_TICK = 27_000_000
THRESHOLDS_US = (300, 500, 1000)


def pct(vals, p):
    s = sorted(vals)
    return s[max(1, math.ceil(p / 100.0 * len(s))) - 1] if s else float("nan")


def main(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            if not r["t_rx_tick"]:
                continue  # lost exchange
            t1, t4 = int(r["t1_ns"]), int(r["t4_ns"])
            t2 = int(r["t_rx_tick"]) * 1e9 / F_TICK
            t3 = int(r["t_tx_tick"]) * 1e9 / F_TICK
            rows.append((t1, t4 - t1, t3 - t2, (t4 - t1) - (t3 - t2), ((t2 - t1) + (t3 - t4)) / 2))
    print(f"samples {len(rows)}")
    rtt = [r[1] / 1000 for r in rows]
    inside = [r[2] / 1000 for r in rows]
    print(f"round trip T4-T1 us: p1 {pct(rtt, 1):.0f} p50 {pct(rtt, 50):.0f} p99 {pct(rtt, 99):.0f}")
    print(f"inside the bridge T3-T2 us (first request byte -> reply queued; request 8 B = 27 us on the line): "
          f"p1 {pct(inside, 1):.0f} p50 {pct(inside, 50):.0f} p99 {pct(inside, 99):.0f} max {max(inside):.0f}")
    for thr in THRESHOLDS_US:
        sel = [(t1, off) for t1, _, _, d, off in rows if d < thr * 1000]
        if len(sel) < 3:
            print(f"delay < {thr} us: {len(sel)} samples, too few for a line")
            continue
        gap = max((b[0] - a[0]) / 1e9 for a, b in zip(sel, sel[1:]))
        n = len(sel)
        mx = sum(t for t, _ in sel) / n
        my = sum(o for _, o in sel) / n
        sxx = sum((t - mx) ** 2 for t, _ in sel)
        slope = sum((t - mx) * (o - my) for t, o in sel) / sxx
        res = [abs((o - my) - slope * (t - mx)) / 1000 for t, o in sel]
        print(f"delay < {thr} us: {n} samples ({n / len(rows) * 100:.1f} %), longest gap between them {gap:.2f} s; "
              f"drift {slope * 1e6:+.2f} ppm; |offset - line| us p50 {pct(res, 50):.0f} p99 {pct(res, 99):.0f} "
              f"max {max(res):.0f}")


if __name__ == "__main__":
    main(sys.argv[1])
