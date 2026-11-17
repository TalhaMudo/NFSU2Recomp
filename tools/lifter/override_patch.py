#!/usr/bin/env python3
"""Apply an OVERRIDE to the existing gen/all shards (without a full regeneration).

The same as the output genall.py would produce with OVERRIDE=...: the chosen
function's definition is renamed to `sub_X_orig`, its declaration is added to
decls.h.  Prints the affected shard names; recompiling only those is enough:

    .venv/bin/python tools/lifter/override_patch.py 66ace2,6691b1
    -> shard_0085 shard_0101 ...

Ones already _orig are skipped (safe to run again).
On a full regeneration the same addresses must go into the genall.py OVERRIDE list.
"""
import sys, os, re, glob

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, 'gen', 'all')

def main():
    addrs = [int(x, 16) for x in sys.argv[1].replace('0x', '').split(',') if x.strip()]
    want = {f"void sub_{a:08x}(CPU *restrict c) {{": a for a in addrs}
    found = {}
    for path in sorted(glob.glob(os.path.join(GEN, 'shard_*.c'))):
        src = open(path).read()
        hit = [h for h in want if h in src]
        if not hit: continue
        for h in hit:
            a = want[h]
            src = src.replace(h, f"void sub_{a:08x}_orig(CPU *restrict c) {{", 1)
            found[a] = os.path.basename(path)[:-2]
        open(path, 'w').write(src)
    decls = os.path.join(GEN, 'decls.h')
    d = open(decls).read()
    add = [f"void sub_{a:08x}_orig(CPU*);\n" for a in addrs
           if f"void sub_{a:08x}_orig(CPU*);" not in d]
    if add:
        d = d.replace("#endif\n", "".join(add) + "#endif\n")
        open(decls, 'w').write(d)
    for a in addrs:
        if a not in found:
            print(f"  (skipped / already overridden: {a:08x})", file=sys.stderr)
    print(" ".join(sorted(set(found.values()))))

if __name__ == '__main__':
    main()
