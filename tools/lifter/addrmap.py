#!/usr/bin/env python3
"""Map addresses of the European SPEED2.EXE (the reference build) to another
build of the same game, such as the North American v1.2 executable.

The two builds come from the same source and compiler; the code is mostly
shifted by a few bytes. Functions are matched by a byte signature in which the
parts that move between builds are masked (relocated dwords, rel32 call/jump
operands), and the function lists are aligned in order (patience diff, the
linker keeps the order). Inside a matched function every address keeps its
offset. Data addresses come from the relocated operands of matched functions:
the same instruction refers to address A in one build and B in the other.

  addrmap.py build  <reference.exe> <other.exe> <map.json>
  addrmap.py lookup <map.json> <address>...
  addrmap.py header <map.json> <out.h> <source>...
        For every GA(<literal>) in the sources: #define GA_<literal> <mapped>,
        for every sub_XXXXXXXX name: #define sub_XXXXXXXX sub_YYYYYYYY (and the
        _orig/_m variants). Exits with an error if an address cannot be mapped.
  addrmap.py list   <map.json> <hex,hex,...>   function starts, mapped, same format
"""
import sys, os, re, json, bisect, struct, hashlib, collections, difflib
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class Exe:
    def __init__(self, path):
        self.path = path
        pe = pefile.PE(path, fast_load=True)
        self.ib = ib = pe.OPTIONAL_HEADER.ImageBase
        self.ep = ib + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.secs = []
        for s in pe.sections:
            nm = s.Name.rstrip(b'\0').decode('latin1').strip() or '<none>'
            self.secs.append((nm, ib + s.VirtualAddress, s.Misc_VirtualSize, s.get_data()))
        t = self.secs[0]
        assert t[0] == '.text', t[0]
        self.tva, self.tsz, self.text = t[1], t[2], t[3]
        self.relocs = self._relocs()

    def _relocs(self):
        blob = next(d for nm, va, vs, d in self.secs if nm == '<none>')
        out = []; off = 0
        while off + 8 <= len(blob):
            page, size = struct.unpack_from('<II', blob, off)
            if size < 8: break
            for i in range((min(size, len(blob) - off) - 8) // 2):
                (w,) = struct.unpack_from('<H', blob, off + 8 + i * 2)
                if w >> 12 == 3: out.append(self.ib + page + (w & 0xfff))
            off += size
        return sorted(out)

    def in_text(self, a): return self.tva <= a < self.tva + self.tsz

    def rd32(self, a):
        for nm, va, vs, d in self.secs:
            if va <= a < va + len(d) - 3: return struct.unpack_from('<I', d, a - va)[0]
        return None

    def section_of(self, a):
        for nm, va, vs, d in self.secs:
            if va <= a < va + max(vs, len(d)): return nm
        return None

    def discover(self):
        """Function starts: entry, relocated pointers into .text, call targets."""
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        funcs = {self.ep}
        for a in self.relocs:
            v = self.rd32(a)
            if v is not None and self.in_text(v): funcs.add(v)
        cov = bytearray(self.tsz); q = collections.deque(funcs); seen = set(funcs)
        while q:
            va = q.popleft()
            while self.in_text(va):
                off = va - self.tva
                if cov[off]: break
                ins = next(md.disasm_lite(self.text[off:off + 16], va), None)
                if ins is None: break
                addr, size, m, ops = ins
                for k in range(size):
                    if off + k < self.tsz: cov[off + k] = 1
                tgt = None
                if ops.startswith('0x'):
                    try: tgt = int(ops, 16)
                    except ValueError: tgt = None
                if m == 'call':
                    if tgt is not None and self.in_text(tgt) and tgt not in seen:
                        seen.add(tgt); funcs.add(tgt); q.append(tgt)
                elif m == 'jmp':
                    if tgt is None: break
                    va = tgt; continue
                elif m.startswith('j'):
                    if tgt is not None and tgt not in seen: seen.add(tgt); q.append(tgt)
                elif m in ('ret', 'retf', 'int3', 'hlt', 'ud2'):
                    break
                va = addr + size
        return sorted(funcs)

    def signatures(self, starts):
        """(start, length, hash, reloc offsets) per function; moving bytes masked."""
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        rel = self.relocs; out = []
        end_text = self.tva + self.tsz
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else end_text
            body = bytearray(self.text[s - self.tva:e - self.tva])
            roffs = []
            k = bisect.bisect_left(rel, s)
            while k < len(rel) and rel[k] < e:
                o = rel[k] - s; roffs.append(o)
                body[o:o + 4] = b'\0\0\0\0'; k += 1
            for addr, size, m, ops in md.disasm_lite(bytes(body), s):
                if m == 'call' or m.startswith('j'):
                    o = addr - s
                    if size == 5 and body[o] in (0xE8, 0xE9): body[o + 1:o + 5] = b'\0\0\0\0'
                    elif size == 6 and body[o] == 0x0F: body[o + 2:o + 6] = b'\0\0\0\0'
            h = hashlib.blake2b(bytes(body), digest_size=12).hexdigest() + f':{len(body)}'
            out.append((s, len(body), h, roffs))
        return out


def patience(a, b):
    """Align two hash lists; returns matched index pairs (i, j)."""
    def rec(a0, a1, b0, b1, out):
        if a0 >= a1 or b0 >= b1: return
        ca = collections.Counter(a[a0:a1]); cb = collections.Counter(b[b0:b1])
        ua = {a[i]: i for i in range(a0, a1) if ca[a[i]] == 1 and cb.get(a[i]) == 1}
        anchors = sorted((ua[b[j]], j) for j in range(b0, b1) if b[j] in ua)
        if not anchors:
            sm = difflib.SequenceMatcher(None, a[a0:a1], b[b0:b1], autojunk=False)
            for blk in sm.get_matching_blocks():
                for k in range(blk.size): out.append((a0 + blk.a + k, b0 + blk.b + k))
            return
        # longest increasing subsequence on j
        tails = []; prev = [None] * len(anchors); idx = []
        for n, (i, j) in enumerate(anchors):
            p = bisect.bisect_left(tails, j)
            if p == len(tails): tails.append(j); idx.append(n)
            else: tails[p] = j; idx[p] = n
            prev[n] = idx[p - 1] if p > 0 else None
        chain = []; n = idx[-1] if idx else None
        while n is not None: chain.append(anchors[n]); n = prev[n]
        chain.reverse()
        pa, pb = a0, b0
        for i, j in chain:
            rec(pa, i, pb, j, out); out.append((i, j)); pa, pb = i + 1, j + 1
        rec(pa, a1, pb, b1, out)
    out = []
    sys.setrecursionlimit(100000)
    rec(0, len(a), 0, len(b), out)
    return out


def build(ref_path, oth_path, out_path):
    R, O = Exe(ref_path), Exe(oth_path)
    print(f"relocations: reference {len(R.relocs)}, other {len(O.relocs)}")
    fr, fo = R.discover(), O.discover()
    print(f"functions:   reference {len(fr)}, other {len(fo)}")
    sr, so = R.signatures(fr), O.signatures(fo)
    pairs = patience([x[2] for x in sr], [x[2] for x in so])
    fmap = {}
    for i, j in pairs:
        fmap[sr[i][0]] = so[j][0]
    # data/code pairs from relocated operands at the same offsets
    dpairs = collections.defaultdict(collections.Counter)
    for i, j in pairs:
        s, n, h, roffs = sr[i]; s2 = so[j][0]
        for o in roffs:
            v, v2 = R.rd32(s + o), O.rd32(s2 + o)
            if v is not None and v2 is not None: dpairs[v][v2] += 1
    # rel32 call targets of matched functions confirm/extend function pairs
    md = Cs(CS_ARCH_X86, CS_MODE_32); extra = 0; conflict = 0
    for i, j in pairs:
        s, n, h, roffs = sr[i]; s2 = so[j][0]
        for addr, size, m, ops in md.disasm_lite(R.text[s - R.tva:s - R.tva + n], s):
            if m == 'call' and size == 5 and ops.startswith('0x'):
                t = int(ops, 16); o = addr - s
                t2 = (s2 + o + 5 + struct.unpack_from('<i', O.text, s2 + o + 1 - O.tva)[0]) & 0xffffffff
                if t in fmap:
                    if fmap[t] != t2: conflict += 1
                elif R.in_text(t) and O.in_text(t2):
                    fmap[t] = t2; extra += 1
    dmap = {}
    ambiguous = 0
    for v, cnt in dpairs.items():
        if len(cnt) == 1: dmap[v] = next(iter(cnt))
        else: ambiguous += 1
    lens_r = {s: n for s, n, h, r in sr}; lens_o = {s: n for s, n, h, r in so}
    same = {s: t for s, t in fmap.items() if lens_r.get(s) is not None and lens_r.get(s) == lens_o.get(t)}
    unmatched = [s for s, n, h, r in sr if s not in fmap]
    print(f"matched functions: {len(pairs)} by signature, {extra} more by call targets "
          f"({conflict} conflicting); {len(unmatched)} reference functions unmatched")
    print(f"data pairs: {len(dmap)} consistent, {ambiguous} ambiguous")
    json.dump({
        'reference': os.path.basename(ref_path), 'other': os.path.basename(oth_path),
        'ref_sections': [(nm, va, vs) for nm, va, vs, d in R.secs],
        'functions': {f'{k:08x}': f'{v:08x}' for k, v in sorted(fmap.items())},
        'same_length': sorted(f'{k:08x}' for k in same),
        'lengths': {f'{s:08x}': n for s, n, h, r in sr},
        'data': {f'{k:08x}': f'{v:08x}' for k, v in sorted(dmap.items())},
        'unmatched': [f'{s:08x}' for s in unmatched],
    }, open(out_path, 'w'), indent=0)
    print(f"-> {out_path}")


class Map:
    def __init__(self, path):
        j = json.load(open(path))
        self.fn = {int(k, 16): int(v, 16) for k, v in j['functions'].items()}
        self.same = {int(k, 16) for k in j['same_length']}
        self.len = {int(k, 16): v for k, v in j['lengths'].items()}
        self.data = {int(k, 16): int(v, 16) for k, v in j['data'].items()}
        self.starts = sorted(self.len)
        self.dkeys = sorted(self.data)
        self.secs = j['ref_sections']

    def section(self, a):
        for nm, va, vs in self.secs:
            if va <= a < va + max(vs, 0x1000): return nm
        return None

    def lookup(self, a):
        """-> (mapped address, how) or (None, reason)"""
        if a in self.data: return self.data[a], 'operand pair'
        if self.section(a) == '.text':
            k = bisect.bisect_right(self.starts, a) - 1
            if k < 0: return None, 'before the first function'
            s = self.starts[k]
            if a - s >= self.len[s]: return None, 'outside any function'
            if s not in self.fn: return None, f'function {s:08x} is not matched'
            if a != s and s not in self.same: return None, f'function {s:08x} changed length'
            return self.fn[s] + (a - s), f'function {s:08x} + {a - s:#x}'
        # data: both nearest pairs in the same section must agree on the delta
        k = bisect.bisect_left(self.dkeys, a)
        lo = self.dkeys[k - 1] if k > 0 else None
        hi = self.dkeys[k] if k < len(self.dkeys) else None
        if lo is None or hi is None: return None, 'no data pairs around it'
        d1, d2 = self.data[lo] - lo, self.data[hi] - hi
        sec = self.section(a)
        if d1 == d2 and self.section(lo) == sec == self.section(hi):
            return a + d1, f'between pairs {lo:08x} and {hi:08x} (delta {d1:+#x})'
        return None, f'neighbours disagree: {lo:08x}{d1:+#x}, {hi:08x}{d2:+#x}'


GA_RE = re.compile(r'\bGA\(\s*(0x[0-9a-fA-F]+u?)\s*\)')
SUB_RE = re.compile(r'\bsub_([0-9a-f]{8})(?:_orig)?(?:_m)?\b')
COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def header(map_path, out_path, sources):
    M = Map(map_path); lits = {}; names = set(); bad = []
    for src in sources:
        # skip generated address headers (this one included)
        if os.path.abspath(src) == os.path.abspath(out_path) or os.path.basename(src).startswith('build_'):
            continue
        text = open(src, encoding='utf-8', errors='replace').read()
        code = COMMENT_RE.sub(' ', text)
        for m in GA_RE.finditer(code):
            lits.setdefault(m.group(1), src)
        for m in SUB_RE.finditer(code):
            names.add(int(m.group(1), 16))
    lines = ['/* Generated by tools/lifter/addrmap.py header -- do not edit.',
             f' * Reference-build addresses used by the runtime, mapped to this build. */',
             '#pragma once', '#define GA(x) GA_##x']
    for lit, src in sorted(lits.items()):
        a = int(lit.rstrip('uU'), 16)
        v, how = M.lookup(a)
        if v is None: bad.append(f'{lit} ({os.path.basename(src)}): {how}'); continue
        lines.append(f'#define GA_{lit} 0x{v:08x}u   /* {how} */')
    for a in sorted(names):
        if a not in M.fn or a not in M.len:
            bad.append(f'sub_{a:08x}: not a matched function'); continue
        b = M.fn[a]
        for suf in ('', '_orig', '_m', '_orig_m'):
            lines.append(f'#define sub_{a:08x}{suf} sub_{b:08x}{suf}')
    if bad:
        print('UNMAPPED:\n  ' + '\n  '.join(bad)); sys.exit(1)
    open(out_path, 'w').write('\n'.join(lines) + '\n')
    print(f"-> {out_path}: {len(lits)} addresses, {len(names)} function names")


def main():
    if len(sys.argv) < 3: print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == 'build': build(*sys.argv[2:5])
    elif cmd == 'lookup':
        M = Map(sys.argv[2])
        for x in sys.argv[3:]:
            a = int(x, 16); v, how = M.lookup(a)
            print(f"{a:08x} -> {v:08x}  {how}" if v is not None else f"{a:08x} -> ?  {how}")
    elif cmd == 'header': header(sys.argv[2], sys.argv[3], sys.argv[4:])
    elif cmd == 'list':
        M = Map(sys.argv[2]); out = []; bad = []
        for x in sys.argv[3].split(','):
            if not x.strip(): continue
            a = int(x, 16)
            if a in M.fn and a in M.len: out.append(f'{M.fn[a]:x}')
            else: bad.append(x)
        if bad: print('UNMAPPED: ' + ','.join(bad), file=sys.stderr); sys.exit(1)
        print(','.join(out))
    else: print(__doc__); sys.exit(1)


if __name__ == '__main__':
    main()
