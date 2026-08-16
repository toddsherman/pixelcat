#!/usr/bin/env python3
"""Check the AXP2101 fuel gauge against physics.

Feed it a serial capture containing a --BATT-BEGIN/--BATT-END block (or the
raw battery.csv):

    python3 tools/batt_report.py capture.txt

The useful trick is the charge runs. While the PMU is in constant-current
mode it pushes a fixed current into the cell, so *true* state of charge rises
linearly with time. Any curve in percent-vs-time during CC is therefore the
gauge's own distortion, not the battery's. Discharge runs get the same
treatment against wall-clock time, which is only fair if the load is roughly
steady -- asleep it is.

Reports seconds-per-percent by 10% band. Flat means honest; a band that costs
far fewer seconds per percent is one the gauge races through.
"""
import re
import sys
from collections import defaultdict

CHG = {0: "tri", 1: "pre", 2: "CC", 3: "CV", 4: "done", 5: "idle"}


def parse(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r'^(\d+),(\d+),(-?\d+),(-?\d+),([0-9A-Fa-f]{2}),'
                     r'([0-9A-Fa-f]{2}),(\d+)$', line)
        if m:
            rows.append({
                'epoch': int(m.group(1)), 'up': int(m.group(2)),
                'pct': int(m.group(3)), 'mv': int(m.group(4)),
                'st1': int(m.group(5), 16), 'st2': int(m.group(6), 16),
                'chg': int(m.group(7)),
            })
    return rows


def split_runs(rows):
    """Break at reboots (uptime resets) and at changes of charge state."""
    runs, cur = [], []
    for r in rows:
        if cur:
            prev = cur[-1]
            restarted = r['up'] < prev['up']
            changed = r['chg'] != prev['chg']
            if restarted or changed:
                if len(cur) > 1:
                    runs.append(cur)
                cur = []
        cur.append(r)
    if len(cur) > 1:
        runs.append(cur)
    return runs


def band_table(run):
    """Seconds of elapsed time spent per percentage point, by 10% band."""
    secs = defaultdict(float)
    pts = defaultdict(int)
    for a, b in zip(run, run[1:]):
        dp = b['pct'] - a['pct']
        dt = b['epoch'] - a['epoch']
        if dt <= 0 or dt > 3600 or dp == 0:
            continue
        band = (min(a['pct'], b['pct']) // 10) * 10
        secs[band] += dt
        pts[band] += abs(dp)
    return {k: secs[k] / pts[k] for k in secs if pts[k]}


def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    rows = parse(text)
    if not rows:
        print("no battery samples found")
        return
    print(f"{len(rows)} samples\n")

    for run in split_runs(rows):
        kind = CHG.get(run[0]['chg'], '?')
        span = (run[-1]['epoch'] - run[0]['epoch']) / 60.0
        print(f"--- {kind} run: {len(run)} samples, {span:.0f} min, "
              f"{run[0]['pct']}% -> {run[-1]['pct']}%, "
              f"{run[0]['mv']} -> {run[-1]['mv']} mV")
        if run[0]['chg'] == 2:
            print("    (constant current: true charge rises linearly with "
                  "time, so uneven bands below are the gauge distorting)")
        bands = band_table(run)
        if not bands:
            print("    not enough movement to judge\n")
            continue
        vals = list(bands.values())
        for band in sorted(bands):
            s = bands[band]
            bar = '#' * max(1, int(round(s / max(vals) * 40)))
            print(f"    {band:3d}-{band+9:3d}%  {s:6.0f} s/%  {bar}")
        lo = [v for k, v in bands.items() if k < 50]
        hi = [v for k, v in bands.items() if k >= 50]
        if lo and hi:
            a, b = sum(lo) / len(lo), sum(hi) / len(hi)
            print(f"    below 50%: {a:.0f} s/%   at/above 50%: {b:.0f} s/%"
                  f"   ratio {a / b:.2f}x")
            if a < b * 0.7:
                print("    -> the gauge moves FASTER below 50% "
                      "(each low percent is worth less real charge)")
            elif a > b * 1.4:
                print("    -> the gauge moves SLOWER below 50% "
                      "(it stalls at the bottom)")
            else:
                print("    -> roughly even across the range")
        else:
            print("    need samples on both sides of 50% to compare")
        print()


if __name__ == '__main__':
    main()
