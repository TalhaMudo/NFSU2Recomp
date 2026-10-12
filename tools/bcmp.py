#!/usr/bin/env python3
"""Iki FPSCSV kosusunu gercek-zaman kovalarina gore eslestirip karsilastir.

Gercek saatte oyun durumu gecen gercek zamana bagli: ayni saniye = ayni sahne.
Kareler 0.5 sn'lik kovalara atilir (kumulatif ms), kova basina medyan alinir,
iki kosu kova kova eslenir.  Yalnizca yaris kareleri (cizim > 400).

    tools/bcmp.py a1.csv,a2.csv b1.csv,b2.csv     (A ve B gruplari, virgullu)
"""
import csv, sys, statistics as st

def buckets(path, w=0.5):
    t = 0.0; b = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            ms = float(r['ms']); t += ms / 1000.0
            if int(r['cizim']) <= 400 or ms > 100: continue   # menu / dokum karesi
            b.setdefault(int(t / w), []).append(ms)
    return {k: st.median(v) for k, v in b.items() if len(v) >= 3}

def group(paths):
    bs = [buckets(p) for p in paths]
    keys = set.intersection(*[set(x) for x in bs])
    return {k: st.median([x[k] for x in bs]) for k in keys}

a = group(sys.argv[1].split(',')); b = group(sys.argv[2].split(','))
keys = sorted(set(a) & set(b))
if not keys: sys.exit("ortak yaris kovasi yok")
ratios = [b[k] / a[k] for k in keys]
ma = st.median([a[k] for k in keys]); mb = st.median([b[k] for k in keys])
print(f"eslesen kova: {len(keys)}")
print(f"A medyan {ma:.2f} ms ({1000/ma:.1f} fps)   B medyan {mb:.2f} ms ({1000/mb:.1f} fps)")
print(f"B/A eslestirilmis oran (medyan): {st.median(ratios):.4f}  ->  {100*(st.median(ratios)-1):+.1f}%")
