#!/usr/bin/env python3
"""Full parser for the D3DX9 fx_2_0 effect binary (0xFEFF0901).

Usage:  efxdump.py <blob.efx> [...]        (blob: extracted from the PE resource)
        efxdump.py --exe SPEED2.EXE         (all the embedded *_FX resources)

Reference for the native ID3DXEffect (runtime/fxnative.c): parameters,
techniques, passes, states, objects and resources (shader bytecode,
parameter name, preshader expression).  Offsets are relative to blob+8 (the
data region).  See docs/d3dx-hle.md, Stage 4.
"""
import sys, struct

CLASS = {0:'SCALAR',1:'VECTOR',2:'MATRIX_ROWS',3:'MATRIX_COLS',4:'OBJECT',5:'STRUCT'}
TYPE  = {0:'VOID',1:'BOOL',2:'INT',3:'FLOAT',4:'STRING',5:'TEXTURE',6:'TEXTURE1D',
         7:'TEXTURE2D',8:'TEXTURE3D',9:'TEXTURECUBE',10:'SAMPLER',11:'SAMPLER1D',
         12:'SAMPLER2D',13:'SAMPLER3D',14:'SAMPLERCUBE',15:'PIXELSHADER',
         16:'VERTEXSHADER',17:'PIXELFRAGMENT',18:'VERTEXFRAGMENT'}
OBJ_TYPES = {4,5,6,7,8,9,15,16}          # types that carry an object id
SAMPLERS  = {10,11,12,13,14}


class Rd:
    def __init__(s, b, p): s.b, s.p = b, p
    def u32(s):
        v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v


class Effect:
    def __init__(s, blob):
        tag, start = struct.unpack_from('<II', blob, 0)
        assert tag == 0xFEFF0901, hex(tag)
        s.data = blob[8:]
        s.objects = {}
        r = Rd(s.data, start)
        s.nparam, s.ntech, s.unk, s.nobj = r.u32(), r.u32(), r.u32(), r.u32()
        s.params = [s.parameter(r) for _ in range(s.nparam)]
        s.techs = [s.technique(r) for _ in range(s.ntech)]
        nstr, nres = r.u32(), r.u32()
        s.strings = {}
        for _ in range(nstr):
            oid = r.u32(); sz = r.u32()
            s.strings[oid] = s.data[r.p:r.p+sz]; r.p += (sz + 3) & ~3
        s.resources = []
        for _ in range(nres):
            ti, idx, ei, si, usage = r.u32(), r.u32(), r.u32(), r.u32(), r.u32()
            sz = r.u32(); d = s.data[r.p:r.p+sz]; r.p += (sz + 3) & ~3
            s.resources.append(dict(tech=ti, idx=idx, elem=ei, state=si, usage=usage, data=d))
        s.end = r.p

    def name(s, off):
        if off == 0: return ''
        n = struct.unpack_from('<I', s.data, off)[0]
        return s.data[off+4:off+4+n].rstrip(b'\0').decode('latin1')

    def typedef(s, r, parent=None):
        t = dict(type=r.u32(), cls=r.u32())
        t['name'] = s.name(r.u32()); t['sem'] = s.name(r.u32()); t['elems'] = r.u32()
        t['rows'] = t['cols'] = 0; t['members'] = []
        c = t['cls']
        if c == 1: t['cols'], t['rows'] = r.u32(), r.u32()
        elif c in (0, 2, 3): t['rows'], t['cols'] = r.u32(), r.u32()
        elif c == 5:
            n = r.u32(); t['members'] = [s.typedef(r) for _ in range(n)]
        return t

    def value(s, t, r, top=True):
        """First value; a list for element arrays."""
        if top and t['elems']:
            return [s.value(t, r, False) for _ in range(t['elems'])]
        c, ty = t['cls'], t['type']
        if c in (0, 1, 2, 3):
            n = t['rows'] * t['cols']
            raw = [r.u32() for _ in range(n)]
            return ('num', raw)
        if c == 5:
            return ('struct', [s.value(m, r) for m in t['members']])
        if c == 4:
            if ty in SAMPLERS:
                n = r.u32()
                return ('sampler', [s.state(r) for _ in range(n)])
            oid = r.u32(); s.objects[oid] = ty
            return ('obj', oid)
        return ('?', None)

    def param_like(s, r, with_flags=True):
        toff = r.u32(); voff = r.u32()
        flags = r.u32() if with_flags else 0
        nann = r.u32() if with_flags else 0
        t = s.typedef(Rd(s.data, toff))
        v = s.value(t, Rd(s.data, voff))
        anns = [s.annotation(r) for _ in range(nann)]
        return dict(t=t, v=v, flags=flags, anns=anns, toff=toff, voff=voff)

    def annotation(s, r):
        toff = r.u32(); voff = r.u32()
        t = s.typedef(Rd(s.data, toff))
        return dict(t=t, v=s.value(t, Rd(s.data, voff)))

    def parameter(s, r):
        return s.param_like(r)

    def state(s, r):
        op = r.u32(); idx = r.u32(); toff = r.u32(); voff = r.u32()
        t = s.typedef(Rd(s.data, toff))
        v = s.value(t, Rd(s.data, voff))
        return dict(op=op, idx=idx, t=t, v=v)

    def technique(s, r):
        nm = s.name(r.u32()); nann = r.u32(); npass = r.u32()
        anns = [s.annotation(r) for _ in range(nann)]
        passes = []
        for _ in range(npass):
            pn = s.name(r.u32()); pa = r.u32(); ns = r.u32()
            panns = [s.annotation(r) for _ in range(pa)]
            passes.append(dict(name=pn, anns=panns, states=[s.state(r) for _ in range(ns)]))
        return dict(name=nm, anns=anns, passes=passes)


def fmtv(v):
    if isinstance(v, list): return '[' + ', '.join(fmtv(x) for x in v) + ']'
    k, d = v
    if k == 'num':
        fl = [struct.unpack('<f', struct.pack('<I', x))[0] for x in d]
        if all(x < 0x10000 for x in d): return 'i' + str(d)
        return 'f[' + ', '.join(f'{x:g}' for x in fl) + ']'
    if k == 'obj': return f'obj#{d}'
    if k == 'sampler': return 'sampler{' + '; '.join(f"op{st['op']}[{st['idx']}]={fmtv(st['v'])}" for st in d) + '}'
    return str(v)


REGSET = {0:'b', 1:'i', 2:'c', 3:'s'}


def ctab(code):
    """CTAB interpretation of shader/expression bytecode: [(name, set, first, count, class, type, rows, cols, elements)]."""
    n = len(code) // 4
    w = struct.unpack('<%dI' % n, code[:n*4])
    i = 1
    while i < n:
        t = w[i]
        if t & 0xffff == 0xfffe:
            L = t >> 16
            if w[i+1] == 0x42415443:                       # 'CTAB'
                b = code[(i+2)*4:(i+1+L)*4]
                _, _, _, nc, coff = struct.unpack_from('<5I', b, 0)
                out = []
                for k in range(nc):
                    noff, rs, ri, rc, _, toff, doff = struct.unpack_from('<IHHHHII', b, coff + 20*k)
                    nm = b[noff:b.index(b'\0', noff)].decode('latin1')
                    cl, ty, ro, co, el, nm_, mo = struct.unpack_from('<HHHHHHI', b, toff)
                    out.append((nm, REGSET.get(rs, rs), ri, rc, cl, ty, ro, co, el))
                return out
            i += 1 + L
        elif t == 0xffff:
            break
        else:
            i += 1
    return []


def tstr(t):
    s = f"{CLASS.get(t['cls'],t['cls'])} {TYPE.get(t['type'],t['type'])}"
    if t['cls'] in (0,1,2,3): s += f" {t['rows']}x{t['cols']}"
    if t['elems']: s += f" [{t['elems']}]"
    return s


def dump(nm, blob):
    e = Effect(blob)
    print(f"=== {nm}: {len(blob)} bytes, parameters {e.nparam}, techniques {e.ntech}, objects {e.nobj}, "
          f"unknown {e.unk:#x}, read {e.end+8}/{len(blob)}")
    for i, p in enumerate(e.params):
        t = p['t']
        print(f"  P{i:<2} {tstr(t):26s} {t['name']}" + (f" :{t['sem']}" if t['sem'] else '')
              + f"  flags={p['flags']:#x} = {fmtv(p['v'])}" + (f"  ann={len(p['anns'])}" if p['anns'] else ''))
    for ti, t in enumerate(e.techs):
        print(f"  T{ti} {t['name']}  ({len(t['passes'])} passes)")
        for pi, ps in enumerate(t['passes']):
            print(f"    pass {pi} {ps['name']}: {len(ps['states'])} states")
            for si, st in enumerate(ps['states']):
                print(f"      S{si:<2} op={st['op']:#05x} idx={st['idx']:<2} {tstr(st['t']):22s} {fmtv(st['v'])}")
    for oid in sorted(e.strings):
        print(f"  string #{oid}: {e.strings[oid][:60]!r}")
    for r in e.resources:
        d = r['data']; kind = {0:'expression/shader', 1:'parameter', 2:'array selector'}.get(r['usage'], '?')
        head = d[:4].hex()
        txt = d.rstrip(b'\0').decode('latin1') if r['usage'] == 1 else f"{len(d)} bytes {head}"
        if r['usage'] == 0 and len(d) >= 4:
            ver = struct.unpack_from('<I', d, 0)[0]
            kind2 = {0xfffe: 'VS', 0xffff: 'PS', 0x4658: 'expression'}.get(ver >> 16, '?')
            cs = ctab(d)
            txt = f"{kind2} {ver & 0xffff:#06x}, {len(d)} bytes; constants: " + ', '.join(
                f"{c[0]}->{c[1]}{c[2]}" + (f"..{c[2]+c[3]-1}" if c[3] > 1 else '') + f"({CLASS.get(c[4],c[4])[:6]})"
                for c in cs)
        where = (f"param P{r['idx']}" + (f"[{r['elem']}]" if r['elem'] != 0xffffffff else '') if r['tech'] == 0xffffffff
                 else f"T{r['tech']} pass {r['idx']}")
        print(f"  resource {where} state S{r['state']}: {kind}: {txt}")
    return e


def main():
    a = sys.argv[1:]
    if a and a[0] == '--exe':
        import pefile
        pe = pefile.PE(a[1])
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            for en in t.directory.entries:
                if en.name and str(en.name).endswith('_FX'):
                    for l in en.directory.entries:
                        d = l.data.struct
                        dump(str(en.name), pe.get_data(d.OffsetToData, d.Size))
        return
    for f in a:
        dump(f, open(f, 'rb').read())


if __name__ == '__main__':
    main()
