#!/usr/bin/env python3
"""Lift edilmis kodun komut duzeyinde profili: oyun thread'i zamani hangi x86
komut turlerine gidiyor?

    python3 tools/lift_profil.py <PERFLOG dizini> [--src gen/prof] [--bas 6400 --son 9400]

Gerekenler: kayit, aciklamali shard'larla (COMMENTS=1) ve -gline-tables-only ile
derlenmis ikiliyle alinmis olmali (bkz. PROJECT.md "Lifter profili").  Her ornegin
yaprak PC'si atos -i ile shard satirina, satir da bir onceki
"/* adres  komut islenenler */" yorumuna, yani x86 komutuna eslenir.
Yalniz standart kutuphane + atos.
"""
import bisect, collections, os, re, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oturum_analiz as oa

KOMUT = re.compile(r'^\s*/\* ([0-9a-f]{8})  (\S+)(?: (.*?))? \*/\s*$')


def kategori(mn, ops):
    ops = ops or ''
    if mn.startswith('f') and mn not in ('femms',):
        if mn in ('fld', 'fild', 'fstp', 'fst', 'fistp', 'fist', 'fisttp', 'fldz', 'fld1', 'fxch', 'fnstsw', 'fstsw',
                  'fnstcw', 'fldcw', 'ffree', 'fdecstp', 'fincstp'):
            return 'x87 yukle/sakla/yigin'
        if mn.startswith(('fcom', 'fucom', 'ftst')):
            return 'x87 karsilastirma'
        return 'x87 aritmetik'
    if re.match(r'(mov|add|sub|mul|div|min|max|sqrt|rsqrt|rcp|cvt|ucomi|comi|and|andn|or|xor|shuf|unpck)(ss|sd|ps|pd)$', mn) \
            or mn.startswith(('cvt', 'movaps', 'movups', 'movss', 'movsd', 'movhps', 'movlps')) or mn in ('pxor',):
        return 'SSE'
    if mn.startswith('p') and mn not in ('push', 'pushal', 'pushfd', 'pop', 'popal', 'popfd'):
        return 'MMX/SSE tamsayi'
    if mn in ('call',):
        return 'cagri'
    if mn in ('ret', 'retn'):
        return 'donus'
    if mn in ('push', 'pop', 'pushal', 'popal', 'pushfd', 'popfd'):
        return 'push/pop'
    if mn in ('jmp',) or (mn.startswith('j')):
        return 'dal'
    if mn in ('cmp', 'test'):
        return 'cmp/test (bayrak)'
    if mn.startswith(('rep', 'movs', 'stos', 'lods', 'scas', 'cmps')):
        return 'dizgi (rep)'
    if mn in ('mov', 'movzx', 'movsx', 'lea', 'xchg', 'cdq', 'cwde', 'cbw') or mn.startswith('cmov') or mn.startswith('set'):
        bel = '[' in ops
        if mn == 'lea':
            return 'lea'
        return 'mov bellek' if bel else 'mov yazmac'
    if mn in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not', 'adc', 'sbb', 'shl', 'shr', 'sar', 'rol',
              'ror', 'sal', 'shld', 'shrd', 'imul', 'mul', 'idiv', 'div', 'bt', 'bts', 'btr', 'bsf', 'bsr', 'bswap'):
        return 'tamsayi ALU' + (' (bellek)' if '[' in ops else '')
    return 'diger: ' + mn


def main():
    d = sys.argv[1]
    src = 'gen/prof'
    if '--src' in sys.argv:
        src = sys.argv[sys.argv.index('--src') + 1]
    m = oa.meta_oku(d)
    ikili = m['ikili']
    lo, hi = (int(x, 16) for x in m['metin'].split())
    orn = [o for o in oa.ornekleri_oku(d) if o[2] == 0]
    # yaris penceresi: konuk karesi (gframe) ile
    bas = int(sys.argv[sys.argv.index('--bas') + 1]) if '--bas' in sys.argv else 0
    son = int(sys.argv[sys.argv.index('--son') + 1]) if '--son' in sys.argv else 1 << 31
    orn = [o for o in orn if bas <= o[1] < son]
    sb = oa.Sembol(d, m)

    # bekleme ornekleri (kare sinirlayici uykusu vb.) disarida: yalniz is
    is_orn = [o for o in orn if not oa.BEKLEME.search(sb(o[3][0]))]
    print(f'{len(orn)} ornek, {len(is_orn)} is (bekleme disi)')

    # ana ikilideki yaprak PC'ler -> atos -i
    pcs = sorted({o[3][0] for o in is_orn if lo <= o[3][0] < hi})
    yer = {}
    tmp = os.path.join(d, '_pcs.txt')
    with open(tmp, 'w') as f:
        for p in pcs:
            f.write('0x%x\n' % p)
    out = subprocess.run(['atos', '-i', '-o', ikili, '-l', hex(lo), '-f', tmp], capture_output=True, text=True).stdout
    os.remove(tmp)
    # atos -i: her adres icin bir veya daha fazla satir (ic ice cerceveler), adresler arasi bos satir
    gruplar = out.split('\n\n')
    if len(gruplar) < len(pcs):        # bazi surumler bos satir koymaz: satir basina bir adres
        gruplar = out.splitlines()
    for p, g in zip(pcs, gruplar):
        satirlar = [s for s in g.strip().splitlines() if s.strip()]
        shard = None
        for s in satirlar:          # en distaki (son) shard satiri = lift edilmis komut
            mm = re.search(r'\((shard_\d+\.c):(\d+)\)', s)
            if mm:
                shard = (mm.group(1), int(mm.group(2)))
        ic = satirlar[0] if satirlar else ''
        yardimci = re.search(r'^(\S+) \(in .*?\) \((cpu\.h):(\d+)\)', ic)
        yer[p] = (shard, yardimci.group(1) if yardimci else None)

    dosya = {}

    def komut(shard, satir):
        if shard not in dosya:
            dosya[shard] = open(os.path.join(src, shard), errors='replace').read().split('\n')
        L = dosya[shard]
        for k in range(satir - 1, max(-1, satir - 60), -1):
            mm = KOMUT.match(L[k])
            if mm:
                return int(mm.group(1), 16), mm.group(2), mm.group(3)
            if L[k].startswith('void sub_') or L[k].startswith('L_'):
                break
        return None, '(fonksiyon girisi/blok)', ''

    kat = collections.Counter(); mnem = collections.Counter(); yard = collections.Counter()
    disari = collections.Counter(); adres = collections.Counter()
    for o in is_orn:
        p = o[3][0]
        if not (lo <= p < hi):
            disari[sb(p)] += 1
            continue
        shard, yardimci = yer.get(p, (None, None))
        if not shard:
            disari['(runtime) ' + sb(p)] += 1
            continue
        a, mn, ops = komut(*shard)
        k = kategori(mn, ops) if a is not None else mn
        kat[k] += 1
        mnem[mn] += 1
        if a is not None:
            adres[a] += 1
        if yardimci:
            yard[yardimci] += 1
    N = len(is_orn)
    lifted = sum(kat.values())
    print(f'\nlift edilmis kodda: {lifted} ornek ({100 * lifted / N:.1f}% of is); disarida {N - lifted}')
    print('\n== x86 komut turu (lift edilmis kod icinde pay) ==')
    for k, v in kat.most_common():
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== en sik komutlar ==')
    for k, v in mnem.most_common(25):
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== satir ici yardimcilar (cpu.h) ==')
    for k, v in yard.most_common(12):
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== lift edilmis kod disi (is ornekleri) ==')
    for k, v in disari.most_common(15):
        print(f'  {100 * v / N:5.1f}%  {k}')
    print('\n== en sicak x86 adresleri ==')
    for a, v in adres.most_common(15):
        print(f'  {100 * v / lifted:5.2f}%  0x{a:08x}')


if __name__ == '__main__':
    main()
