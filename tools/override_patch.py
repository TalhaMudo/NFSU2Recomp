#!/usr/bin/env python3
"""Mevcut gen/all shard'larina OVERRIDE uygula (tam yeniden uretim olmadan).

genall.py'nin OVERRIDE=... ile uretecegi ciktinin aynisi: secilen
fonksiyonun tanimi `sub_X_orig` olarak yeniden adlandirilir, decls.h'e
bildirimi eklenir.  Etkilenen shard adlarini basar; yalnizca onlari
yeniden derlemek yeter:

    .venv/bin/python tools/override_patch.py 66ace2,6691b1
    -> shard_0085 shard_0101 ...

Zaten _orig olanlar atlanir (tekrar calistirmak guvenli).
Tam yeniden uretimde ayni adresler genall.py OVERRIDE listesine girmeli.
"""
import sys, os, re, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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
            print(f"  (atlandi / zaten override: {a:08x})", file=sys.stderr)
    print(" ".join(sorted(set(found.values()))))

if __name__ == '__main__':
    main()
