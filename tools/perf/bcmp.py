#!/usr/bin/env python3
"""Compare two FPSCSV runs by matching them on real-time buckets.

With the real clock the game state depends on the real time elapsed: same second = same scene.
Frames go into 0.5 s buckets (cumulative ms), the median is taken per bucket,
the two runs are matched bucket by bucket.  Race frames only (draws > 400).

    tools/perf/bcmp.py a1.csv,a2.csv b1.csv,b2.csv     (groups A and B, comma-separated)
"""
import csv, sys, statistics as st

def buckets(path, w=0.5):
    t = 0.0; b = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            ms = float(r['ms']); t += ms / 1000.0
            if int(r.get('draws') or r['cizim']) <= 400 or ms > 100: continue   # menu / dump frame
            b.setdefault(int(t / w), []).append(ms)
    return {k: st.median(v) for k, v in b.items() if len(v) >= 3}

def group(paths):
    bs = [buckets(p) for p in paths]
    keys = set.intersection(*[set(x) for x in bs])
    return {k: st.median([x[k] for x in bs]) for k in keys}

a = group(sys.argv[1].split(',')); b = group(sys.argv[2].split(','))
keys = sorted(set(a) & set(b))
if not keys: sys.exit("no common race bucket")
ratios = [b[k] / a[k] for k in keys]
ma = st.median([a[k] for k in keys]); mb = st.median([b[k] for k in keys])
print(f"matched buckets: {len(keys)}")
print(f"A median {ma:.2f} ms ({1000/ma:.1f} fps)   B median {mb:.2f} ms ({1000/mb:.1f} fps)")
print(f"B/A matched ratio (median): {st.median(ratios):.4f}  ->  {100*(st.median(ratios)-1):+.1f}%")
