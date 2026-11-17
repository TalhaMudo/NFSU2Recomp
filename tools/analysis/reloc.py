#!/usr/bin/env python3
import sys, os, struct, collections
import pefile
pe = pefile.PE(sys.argv[1])
IB = pe.OPTIONAL_HEADER.ImageBase
dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']]
print(f"BASERELOC DataDirectory: RVA 0x{dd.VirtualAddress:08x} (VA 0x{IB+dd.VirtualAddress:08x})  size {dd.Size}")
print(f"IMAGE_FILE_RELOCS_STRIPPED flag: {bool(pe.FILE_HEADER.Characteristics & 0x0001)}")
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
tot=0; types=collections.Counter(); addrs=[]
for b in getattr(pe,'DIRECTORY_ENTRY_BASERELOC',[]):
    for e in b.entries:
        types[e.type]+=1; tot+=1
        if e.type==3: addrs.append(IB+e.rva)
print(f"\nTOPLAM relocation: {tot}")
for t,c in types.items():
    print(f"  tip {t} ({'HIGHLOW/absolute address' if t==3 else 'ABSOLUTE/padding' if t==0 else '?'}): {c}")
# which section these addresses are in
secs=[(s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>', IB+s.VirtualAddress, s.Misc_VirtualSize) for s in pe.sections]
loc=collections.Counter()
for a in addrs:
    for nm,va,vs in secs:
        if va<=a<va+vs: loc[nm]+=1; break
    else: loc['<outside>']+=1
print("\nsections containing absolute-address patches:")
for k,v in loc.most_common(): print(f"  {k:10s} {v}")
os.makedirs('out', exist_ok=True)
open('out/relocs.txt','w').write('\n'.join(f'{a:08x}' for a in sorted(addrs)))
print(f"\n-> out/relocs.txt written ({len(addrs)} addresses)")
