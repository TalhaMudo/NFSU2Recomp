import sys, re, random, collections; sys.path.insert(0,'tools')
from lift import Lifter, _flag_defuse, F_ALL, F_CF,F_PF,F_AF,F_ZF,F_SF,F_OF
EXE=sys.argv[1]
L=Lifter(EXE, comments=False, trap=True)
L.cur_fmask = F_ALL

def written_flags(code):
    """Yayilan C metninde HANGI bayraklar yazilabiliyor."""
    w = 0
    if 'SETZSP' in code: w |= F_ZF|F_SF|F_PF
    for mac,bits in (('FLAGS_ADD',F_ALL),('FLAGS_SUB',F_ALL),('FLAGS_ADC',F_ALL),
                     ('FLAGS_SBB',F_ALL),('FLAGS_LOG',F_ALL)):
        if mac in code: w |= bits
    for nm,bit in (('cf',F_CF),('pf',F_PF),('af',F_AF),('zf',F_ZF),('sf',F_SF),('of',F_OF)):
        if re.search(r'c->%s\s*=[^=]'%nm, code): w |= bit
    if 'eflags_set' in code: w |= F_ALL
    return w

nm=lambda m: ''.join(k for k,v in (('C',F_CF),('P',F_PF),('A',F_AF),('Z',F_ZF),('S',F_SF),('O',F_OF)) if m&v) or '-'
fns=[int(x,16) for x in re.findall(r'void sub_([0-9a-f]{8})\(CPU\*\);', open('gen/all/decls.h').read())]
random.seed(11); samp=random.sample(fns, 4000)
bad=collections.Counter(); ex={}; seen=set(); ninsn=0
for a in samp:
    try: blocks,_=L.discover(a)
    except Exception: continue
    for ba in sorted(blocks):
        for i in blocks[ba]:
            key=(i.mnemonic, tuple(o.type for o in i.operands))
            if key in seen: continue
            seen.add(key); ninsn+=1
            d,u=_flag_defuse(i)
            if not d: continue
            try: code=' '.join(L.insn(i, set()))
            except Exception: continue
            w=written_flags(code)
            miss = d & ~w
            if miss:
                bad[(i.mnemonic, nm(miss))]+=1
                ex.setdefault((i.mnemonic, nm(miss)), (i.op_str, code[:150]))
print("incelenen farkli komut bicimi:", ninsn)
print("--- capstone 'yazar' diyor ama lift YAZMIYOR ---")
for (mn,ms),c in bad.most_common(30):
    ops,code = ex[(mn,ms)]
    print("  %-12s eksik=%-6s  | %s" % (mn, ms, ops[:40]))
    print("      -> %s" % code.replace('\n',' ')[:130])
