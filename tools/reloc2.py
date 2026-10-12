#!/usr/bin/env python3
"""Directory girisi sifirlanmis relocation tablosunu isimsiz bolumden elle ayristir."""
import sys, os, struct, collections
import pefile
pe = pefile.PE(sys.argv[1]); IB = pe.OPTIONAL_HEADER.ImageBase
secs=[]
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>'
    secs.append((nm, IB+s.VirtualAddress, s.Misc_VirtualSize, s.get_data()))
blob = next(d for nm,va,vs,d in secs if nm=='<none>')

off=0; tot=0; types=collections.Counter(); addrs=[]; blocks=0; bad=0
while off+8 <= len(blob):
    page, size = struct.unpack_from('<II', blob, off)
    if size==0: break
    if size<8 or off+size > len(blob)+8:
        bad+=1; break
    n=(min(size,len(blob)-off)-8)//2
    for i in range(n):
        (w,)=struct.unpack_from('<H', blob, off+8+i*2)
        t=w>>12; o=w&0xfff
        types[t]+=1; tot+=1
        if t==3: addrs.append(IB+page+o)
    blocks+=1; off+=size
print(f"ayristirilan blok: {blocks}   toplam giris: {tot}   hatali: {bad}")
for t,c in sorted(types.items()): print(f"  tip {t}: {c}")

def sec_of(a):
    for nm,va,vs,d in secs:
        if va<=a<va+vs: return nm
    return '<disarida>'
print("\nmutlak-adres yamalarinin bulundugu bolumler:")
for k,v in collections.Counter(map(sec_of,addrs)).most_common(): print(f"  {k:10s} {v}")

# Dogrulama: bu konumlardaki 4-baytlik degerler gercekten image icine mi isaret ediyor?
def rd32(a):
    for nm,va,vs,d in secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    return None
IMG_LO, IMG_HI = IB, IB+pe.OPTIONAL_HEADER.SizeOfImage
ok=sum(1 for a in addrs if (v:=rd32(a)) is not None and IMG_LO<=v<IMG_HI)
print(f"\nDOGRULAMA: {ok}/{len(addrs)} konumdaki deger image araligina ({IMG_LO:#x}-{IMG_HI:#x}) isaret ediyor = {100.0*ok/max(len(addrs),1):.2f}%")
out='out/relocs.txt'; os.makedirs('out', exist_ok=True)
open(out,'w').write('\n'.join(f'{a:08x}' for a in sorted(addrs)))
print(f"-> {out} ({len(addrs)} adres)")
