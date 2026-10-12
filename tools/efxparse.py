#!/usr/bin/env python3
"""D3DX9 efekt (.efx) ikili formatini bagimsiz ayristir.
   Amac: oyunun kurdugu parametre agaciyla karsilastirmak."""
import sys, struct
sys.path.insert(0,'tools')
from lift import Lifter

CLASS = {0:'SCALAR',1:'VECTOR',2:'MATRIX_ROWS',3:'MATRIX_COLS',4:'OBJECT',5:'STRUCT'}
TYPE  = {0:'VOID',1:'BOOL',2:'INT',3:'FLOAT',4:'STRING',5:'TEXTURE',6:'TEXTURE1D',
         7:'TEXTURE2D',8:'TEXTURE3D',9:'TEXTURECUBE',10:'SAMPLER',11:'SAMPLER1D',
         12:'SAMPLER2D',13:'SAMPLER3D',14:'SAMPLERCUBE',15:'PIXELSHADER',
         16:'VERTEXSHADER',17:'PIXELFRAGMENT',18:'VERTEXFRAGMENT'}

def main():
    L=Lifter(sys.argv[1])
    def rd(a,n):
        for nm,va,vs,d in L.secs:
            if va<=a<va+len(d): return d[a-va:a-va+n]
        return b''
    VA=int(sys.argv[2],16); SZ=int(sys.argv[3])
    b=rd(VA,SZ)
    ver, blobsz = struct.unpack_from('<II', b, 0)
    print(f"surum=0x{ver:08x}  blob={blobsz}  toplam={SZ}")
    base = 8                       # blob buradan baslar
    hdr  = 8 + blobsz
    nparam, ntech, unk, nobj = struct.unpack_from('<IIII', b, hdr)
    print(f"parametre={nparam} teknik={ntech} ?={unk} nesne={nobj}\n")

    def cstr(off):
        # blob'ta: DWORD uzunluk, sonra string
        if off==0 or base+off+4 > len(b): return ''
        n = struct.unpack_from('<I', b, base+off)[0]
        if n==0 or base+off+4+n > len(b): return ''
        return b[base+off+4 : base+off+4+n-1].decode('latin1','replace')

    def read_type(off, depth=0, out=None, idx=None):
        """type struct: Type, Class, NameOff, SemanticOff, Elements [, StructMembers]"""
        p = base+off
        if p+20 > len(b): return None
        t,cl,noff,soff,elems = struct.unpack_from('<IIIII', b, p)
        p += 20
        members = 0
        if cl == 5:                      # STRUCT
            members = struct.unpack_from('<I', b, p)[0]; p += 4
        elif cl in (1,2,3):              # VECTOR / MATRIX
            pass
        return dict(type=TYPE.get(t,str(t)), cls=CLASS.get(cl,str(cl)),
                    name=cstr(noff), sem=cstr(soff), elems=elems, members=members)

    p = hdr + 16
    print(f"{'#':>3} {'sinif':12} {'tip':14} {'eleman':>6} {'uye':>4}  ad")
    print("  " + "-"*62)
    for i in range(nparam):
        if p+16 > len(b): break
        toff, voff, flags, nann = struct.unpack_from('<IIII', b, p)
        p += 16
        ti = read_type(toff)
        if ti is None: print(f"{i:3} <okunamadi>"); continue
        print(f"{i:3} {ti['cls']:12} {ti['type']:14} {ti['elems']:6} {ti['members']:4}  {ti['name']}"
              + (f"  :{ti['sem']}" if ti['sem'] else ""))
        # tur/deger ofsetleri: oyunun kurdugu isaretcilerle karsilastirmak icin
        toff2, voff2 = toff, voff
        vb = b[base+voff2 : base+voff2+16]
        print(f"      turOfs=0x{toff2:04x} degerOfs=0x{voff2:04x} bayrak=0x{flags:x} "
              f"ann={nann}  deger[0:16]={vb.hex()}")
        # annotation'lari atla
        for _ in range(nann):
            if p+8 <= len(b): p += 8
    print()
main()
