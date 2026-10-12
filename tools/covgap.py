#!/usr/bin/env python3
"""Kapsama sinirini bul: CALISAN koddan cagrilan ama HIC calismayan fonksiyonlar.

Belirti kovalamak yerine "hangi kapi hic acilmadi" sorusunu cevaplar."""
import sys, struct, re, collections

EXE = "Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
covfile = sys.argv[1]

d = open(EXE,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
optsz = struct.unpack_from('<H', d, pe+20)[0]
imgbase = struct.unpack_from('<I', d, pe+24+28)[0]
text = None
for i in range(nsec):
    o = pe+24+optsz+40*i
    nm = d[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o+8)
    if nm == '.text': text = (imgbase+va, vsz, ra, rsz)
sva, vsz, ra, rsz = text

# fonksiyon adresleri
fn = []
for m in re.finditer(r'0x([0-9a-f]{8})u', open('gen/all/fntab.c').read()):
    fn.append(int(m.group(1), 16))
fn = sorted(set(a for a in fn if sva <= a < sva+vsz))
import bisect
def owner(va):
    i = bisect.bisect_right(fn, va) - 1
    return fn[i] if i >= 0 else None

# bicim: '<adres>' ya da '<adres> <sayac>'
hit = set(int(l.split()[0], 16) for l in open(covfile) if l.strip())

# dogrudan call'lari tara
callers = collections.defaultdict(set)
for off in range(ra, ra+rsz-5):
    if d[off] != 0xE8: continue
    disp = struct.unpack_from('<i', d, off+1)[0]
    site = sva + (off - ra)
    tgt = site + 5 + disp
    if not (sva <= tgt < sva+vsz): continue
    ow = owner(site)
    if ow is None: continue
    callers[tgt].add(ow)

# calisan cagiranlardan cagrilan ama calismayan hedefler
gap = []
for tgt, cs in callers.items():
    if tgt in hit: continue
    live = cs & hit
    if live: gap.append((len(live), tgt, sorted(live)[:4]))
gap.sort(reverse=True)

print("calisan fonksiyon: %d / %d" % (len(hit), len(fn)))
print("calisan koddan cagrilan ama HIC calismayan hedef: %d\n" % len(gap))
print("  %-12s %-8s %s" % ("hedef", "cagiran", "ornek cagiranlar"))
for n, tgt, cs in gap[:40]:
    print("  0x%08x  %-8d %s" % (tgt, n, " ".join("0x%08x" % c for c in cs)))
