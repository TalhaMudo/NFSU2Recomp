#!/usr/bin/env python3
"""Fonksiyonlar arasi bayrak canliligi (ABI): cagri ve donus noktalarinda bayraklar
gercekten canli mi?

MSVC'nin urettigi kod bir cagridan donen bayragi okumaz, cagrilan da girişte
cagiranin bayragini okumaz.  Lifter ise cagri ve ret'te tum bayraklari canli
sayiyor (muhafazakar): cagridan onceki son bayrak ureten komutun alti bayragi
hep hesaplanip bellege yaziliyor.  Bu arac varsayimi KANITLAR:

  giris_canli[f] : f'nin girisinde (tanimlanmadan once) okunan bayraklar
  bayrak_donen   : cagri sonrasinda bayrak okunan dogrudan cagrilarin hedefleri
  dolayli_okuyan : dolayli cagri sonrasinda bayrak okunan cagri noktalari

Sabit nokta: cagri T -> tanim F_ALL, kullanim giris_canli[T] (dolayli: F_ALL);
ret -> canli 0, f bayrak_donen ise F_ALL.

    ./.venv/bin/python tools/flagabi.py <SPEED2.EXE> [cikti.json]

cikti.json genall.py'ye FLAGABI=<json> ile verilir (bkz. lift.flag_liveness).
"""
import sys, os, json, collections, time, multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lift import Lifter, abi_cfg, abi_liveness, F_ALL
from capstone.x86 import X86_OP_IMM

EXE = sys.argv[1]
_L = None

def _init():
    global _L
    _L = Lifter(EXE, comments=False)

def ozet(a):
    try:
        blocks, calls = _L.discover(a)
    except Exception:
        return a, None
    return a, abi_cfg(blocks, _L.in_text)


def analiz(G):
    """sabit nokta -> (giris, donen, dolayli)"""
    giris = {a: 0 for a in G}
    donen = set()
    for tur_no in range(16):
        degisti = False
        yeni = set(); dolayli = []
        for a, g in G.items():
            ent, _, sonra = abi_liveness(g, a, giris, a in donen)
            if ent != giris[a]:
                giris[a] = ent; degisti = True
            for adr, (cur, tur, hedef) in sonra.items():
                if cur:
                    if tur == 1: yeni.add(hedef)
                    else: dolayli.append((a, adr, cur))
        if yeni - donen:
            donen |= yeni; degisti = True
        print(f'  tur {tur_no}: girisi canli {sum(1 for v in giris.values() if v)}, '
              f'bayrak donen {len(donen)}, dolayli cagri sonrasi okuyan {len(dolayli)}')
        if not degisti: break
    return giris, donen, dolayli


if __name__ == '__main__':
    cikti = sys.argv[2] if len(sys.argv) > 2 else 'out/flagabi.json'
    t0 = time.time()
    sys.argv = sys.argv[:2]            # genall argv[2]=NSHARD bekliyor
    import genall                      # discover_all: genall.EXE = sys.argv[1]
    funcs = genall.discover_all()
    print(f'{len(funcs)} fonksiyon ({time.time()-t0:.0f}s)')
    with mp.Pool(mp.cpu_count(), initializer=_init) as pool:
        G = dict(pool.map(ozet, funcs, chunksize=64))
    G = {a: g for a, g in G.items() if g}
    print(f'ozet: {len(G)} fonksiyon ({time.time()-t0:.0f}s)')
    giris, donen, dolayli = analiz(G)
    json.dump({'giris': {hex(a): v for a, v in giris.items() if v}, 'donen': sorted(hex(a) for a in donen),
               'dolayli': [[hex(a), hex(adr), cur] for a, adr, cur in dolayli]}, open(cikti, 'w'), indent=0)
    print(f'yazildi: {cikti}  ({time.time()-t0:.0f}s)')
    for a, adr, cur in dolayli[:20]:
        print(f'  dolayli cagri sonrasi bayrak okuyan: fonksiyon {a:#x} cagri {adr:#x} maske {cur:#x}')
    for a in sorted(donen)[:30]:
        print(f'  bayrak donen: {a:#x}')
    for a, v in sorted(giris.items())[:0]: pass
    print('girisi canli:', ' '.join(f'{a:#x}:{v:#x}' for a, v in sorted(giris.items()) if v)[:2000])
