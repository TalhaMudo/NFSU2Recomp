#!/usr/bin/env python3
"""Instruction-level profile of the lifted code: which x86 instruction kinds does the
game thread's time go to?

    python3 tools/perf/lift_profile.py <PERFLOG directory> [--src gen/prof] [--start 6400 --end 9400]

Requirements: the recording must be taken with a binary built from annotated shards
(COMMENTS=1) and with -gline-tables-only (see docs/devlog.md "Lifter profile").  Each
sample's leaf PC is mapped with atos -i to a shard line, and the line to the
preceding "/* address  instruction operands */" comment, i.e. to the x86 instruction.
Standard library + atos only.
"""
import bisect, collections, os, re, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import session_report as sr

INSN = re.compile(r'^\s*/\* ([0-9a-f]{8})  (\S+)(?: (.*?))? \*/\s*$')


def category(mn, ops):
    ops = ops or ''
    if mn.startswith('f') and mn not in ('femms',):
        if mn in ('fld', 'fild', 'fstp', 'fst', 'fistp', 'fist', 'fisttp', 'fldz', 'fld1', 'fxch', 'fnstsw', 'fstsw',
                  'fnstcw', 'fldcw', 'ffree', 'fdecstp', 'fincstp'):
            return 'x87 load/store/stack'
        if mn.startswith(('fcom', 'fucom', 'ftst')):
            return 'x87 compare'
        return 'x87 arithmetic'
    if re.match(r'(mov|add|sub|mul|div|min|max|sqrt|rsqrt|rcp|cvt|ucomi|comi|and|andn|or|xor|shuf|unpck)(ss|sd|ps|pd)$', mn) \
            or mn.startswith(('cvt', 'movaps', 'movups', 'movss', 'movsd', 'movhps', 'movlps')) or mn in ('pxor',):
        return 'SSE'
    if mn.startswith('p') and mn not in ('push', 'pushal', 'pushfd', 'pop', 'popal', 'popfd'):
        return 'MMX/SSE integer'
    if mn in ('call',):
        return 'call'
    if mn in ('ret', 'retn'):
        return 'return'
    if mn in ('push', 'pop', 'pushal', 'popal', 'pushfd', 'popfd'):
        return 'push/pop'
    if mn in ('jmp',) or (mn.startswith('j')):
        return 'branch'
    if mn in ('cmp', 'test'):
        return 'cmp/test (flags)'
    if mn.startswith(('rep', 'movs', 'stos', 'lods', 'scas', 'cmps')):
        return 'string (rep)'
    if mn in ('mov', 'movzx', 'movsx', 'lea', 'xchg', 'cdq', 'cwde', 'cbw') or mn.startswith('cmov') or mn.startswith('set'):
        mem = '[' in ops
        if mn == 'lea':
            return 'lea'
        return 'mov memory' if mem else 'mov register'
    if mn in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not', 'adc', 'sbb', 'shl', 'shr', 'sar', 'rol',
              'ror', 'sal', 'shld', 'shrd', 'imul', 'mul', 'idiv', 'div', 'bt', 'bts', 'btr', 'bsf', 'bsr', 'bswap'):
        return 'integer ALU' + (' (memory)' if '[' in ops else '')
    return 'other: ' + mn


def main():
    d = sys.argv[1]
    src = 'gen/prof'
    if '--src' in sys.argv:
        src = sys.argv[sys.argv.index('--src') + 1]
    m = sr.read_meta(d)
    binary = m['binary']
    lo, hi = (int(x, 16) for x in m['text'].split())
    samples = [o for o in sr.read_samples(d) if o[2] == 0]
    # race window: by guest frame (gframe)
    start = int(sys.argv[sys.argv.index('--start') + 1]) if '--start' in sys.argv else 0
    end = int(sys.argv[sys.argv.index('--end') + 1]) if '--end' in sys.argv else 1 << 31
    samples = [o for o in samples if start <= o[1] < end]
    sb = sr.Symbol(d, m)

    # wait samples (frame limiter sleep etc.) excluded: work only
    work = [o for o in samples if not sr.WAIT.search(sb(o[3][0]))]
    print(f'{len(samples)} samples, {len(work)} work (excluding waits)')

    # leaf PCs in the main binary -> atos -i
    pcs = sorted({o[3][0] for o in work if lo <= o[3][0] < hi})
    where = {}
    tmp = os.path.join(d, '_pcs.txt')
    with open(tmp, 'w') as f:
        for p in pcs:
            f.write('0x%x\n' % p)
    out = subprocess.run(['atos', '-i', '-o', binary, '-l', hex(lo), '-f', tmp], capture_output=True, text=True).stdout
    os.remove(tmp)
    # atos -i: one or more lines per address (nested frames), blank line between addresses
    groups = out.split('\n\n')
    if len(groups) < len(pcs):        # some versions don't put a blank line: one address per line
        groups = out.splitlines()
    for p, g in zip(pcs, groups):
        lines = [s for s in g.strip().splitlines() if s.strip()]
        shard = None
        for s in lines:          # the outermost (last) shard line = the lifted instruction
            mm = re.search(r'\((shard_\d+\.c):(\d+)\)', s)
            if mm:
                shard = (mm.group(1), int(mm.group(2)))
        inner = lines[0] if lines else ''
        helper = re.search(r'^(\S+) \(in .*?\) \((cpu\.h):(\d+)\)', inner)
        where[p] = (shard, helper.group(1) if helper else None)

    files = {}

    def insn(shard, line):
        if shard not in files:
            files[shard] = open(os.path.join(src, shard), errors='replace').read().split('\n')
        L = files[shard]
        for k in range(line - 1, max(-1, line - 60), -1):
            mm = INSN.match(L[k])
            if mm:
                return int(mm.group(1), 16), mm.group(2), mm.group(3)
            if L[k].startswith('void sub_') or L[k].startswith('L_'):
                break
        return None, '(function entry/block)', ''

    cat = collections.Counter(); mnem = collections.Counter(); helpers = collections.Counter()
    outside = collections.Counter(); addrs = collections.Counter()
    for o in work:
        p = o[3][0]
        if not (lo <= p < hi):
            outside[sb(p)] += 1
            continue
        shard, helper = where.get(p, (None, None))
        if not shard:
            outside['(runtime) ' + sb(p)] += 1
            continue
        a, mn, ops = insn(*shard)
        k = category(mn, ops) if a is not None else mn
        cat[k] += 1
        mnem[mn] += 1
        if a is not None:
            addrs[a] += 1
        if helper:
            helpers[helper] += 1
    N = len(work)
    lifted = sum(cat.values())
    print(f'\nin lifted code: {lifted} samples ({100 * lifted / N:.1f}% of work); outside {N - lifted}')
    print('\n== x86 instruction kind (share within lifted code) ==')
    for k, v in cat.most_common():
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== most common instructions ==')
    for k, v in mnem.most_common(25):
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== inline helpers (cpu.h) ==')
    for k, v in helpers.most_common(12):
        print(f'  {100 * v / lifted:5.1f}%  {k}')
    print('\n== outside lifted code (work samples) ==')
    for k, v in outside.most_common(15):
        print(f'  {100 * v / N:5.1f}%  {k}')
    print('\n== hottest x86 addresses ==')
    for a, v in addrs.most_common(15):
        print(f'  {100 * v / lifted:5.2f}%  0x{a:08x}')


if __name__ == '__main__':
    main()
