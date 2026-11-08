#!/usr/bin/env python3
"""Prepare an ASI plugin (a 32-bit Windows DLL) for lifting.

    asi_rebase.py <plugin.asi> <out-dir> [--base 0x0A000000]

Writes into <out-dir>:
  plugin.dll   the plugin with its relocations applied for --base, so the
               lifted code and the runtime agree on every absolute address
  relocs.txt   the patched locations (one hex address per line, as genall.py
               and the exe's own reloc2.py output)
  meta.json    sha256 of the input, base, entry, sections, imports (IAT slot,
               dll, name), TLS directory, exported names

The runtime (runtime/asi.c) maps the user's own .asi file at the same base and
applies the same relocations, after checking the sha256 recorded here: lifted
code belongs to one build of one plugin, as the engines belong to one exe.

Read-only on the input.
"""
import sys, os, json, hashlib, argparse, struct
import pefile

ap = argparse.ArgumentParser()
ap.add_argument('asi'); ap.add_argument('out')
ap.add_argument('--base', default='0x0A000000')
a = ap.parse_args()
base = int(a.base, 0)
raw = open(a.asi, 'rb').read()
sha = hashlib.sha256(raw).hexdigest()

pe = pefile.PE(data=raw)
if pe.FILE_HEADER.Machine != 0x14c: sys.exit("not a 32-bit x86 image")
if not (pe.FILE_HEADER.Characteristics & 0x2000): sys.exit("not a DLL")
old = pe.OPTIONAL_HEADER.ImageBase
if not hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'): sys.exit("no relocation table: cannot be moved")

relocs = []
for blk in pe.DIRECTORY_ENTRY_BASERELOC:
    for e in blk.entries:
        if e.type == 3: relocs.append(base + e.rva)          # HIGHLOW
        elif e.type != 0: sys.exit(f"relocation type {e.type} not supported")
pe.relocate_image(base)
os.makedirs(a.out, exist_ok=True)
pe.write(os.path.join(a.out, 'plugin.dll'))

imports = []
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in d.imports:
        imports.append({'iat': i.address, 'dll': d.dll.decode(),
                        'name': i.name.decode() if i.name else None,
                        'ordinal': None if i.name else i.ordinal})
tls = None
if hasattr(pe, 'DIRECTORY_ENTRY_TLS'):
    t = pe.DIRECTORY_ENTRY_TLS.struct
    cbs = []
    p = t.AddressOfCallBacks
    while p:
        v = pe.get_dword_at_rva(p - base)
        if not v: break
        cbs.append(v); p += 4
    tls = {'start': t.StartAddressOfRawData, 'end': t.EndAddressOfRawData,
           'index': t.AddressOfIndex, 'zerofill': t.SizeOfZeroFill, 'callbacks': cbs}
exports = []
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    exports = [{'name': e.name.decode() if e.name else None, 'addr': base + e.address}
               for e in pe.DIRECTORY_ENTRY_EXPORT.symbols]
meta = {
    'file': os.path.basename(a.asi), 'sha256': sha, 'size': len(raw),
    'preferred_base': old, 'base': base, 'image_size': pe.OPTIONAL_HEADER.SizeOfImage,
    'entry': base + pe.OPTIONAL_HEADER.AddressOfEntryPoint,
    'sections': [{'name': s.Name.rstrip(b'\0').decode(), 'va': base + s.VirtualAddress,
                  'vsize': s.Misc_VirtualSize, 'chars': s.Characteristics} for s in pe.sections],
    'imports': imports, 'tls': tls, 'exports': exports, 'relocs': len(relocs),
}
json.dump(meta, open(os.path.join(a.out, 'meta.json'), 'w'), indent=1)
open(os.path.join(a.out, 'relocs.txt'), 'w').write('\n'.join(f'{r:08x}' for r in sorted(relocs)))
print(f"{meta['file']}: sha256 {sha[:16]}..  {old:#x} -> {base:#x}, {len(relocs)} relocations, "
      f"{len(imports)} imports, entry {meta['entry']:#x}, tls {'yes' if tls else 'no'}"
      + (f" ({len(tls['callbacks'])} callbacks)" if tls else ""))
