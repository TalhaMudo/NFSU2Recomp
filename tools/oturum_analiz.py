#!/usr/bin/env python3
"""Gercek oyun oturumu kaydini (PERFLOG) cozumle -> <dizin>/rapor.md

    python3 tools/oturum_analiz.py <dizin> [--pencere 5]

Girdi: runtime/perflog.c'nin yazdigi dosyalar (+ varsa powermetrics ciktisi
guc.txt).  Yalnizca standart kutuphane + `nm`.
"""
import bisect, collections, csv, datetime, glob, os, re, statistics as st, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def oku_csv(yol):
    if not os.path.exists(yol):
        return []
    with open(yol, newline='') as f:
        r = csv.reader(f)
        bas = next(r, None)
        if not bas:
            return []
        out = []
        for satir in r:
            if len(satir) != len(bas):
                continue
            d = {}
            for k, v in zip(bas, satir):
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
            out.append(d)
        return out


def meta_oku(d):
    m = {}
    for l in open(os.path.join(d, 'meta.txt')):
        k, _, v = l.strip().partition(' ')
        m.setdefault(k, v)
    return m


# ---- semboller ----
def kaynak_haritasi():
    """runtime/*.c|m fonksiyon tanimlari -> dosya adi (kategori icin)"""
    h = {}
    tanim = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
    for yol in glob.glob(os.path.join(ROOT, 'runtime', '*.[cm]')):
        ad = os.path.basename(yol)
        for l in open(yol, errors='replace'):
            if l.startswith((' ', '\t', '#', '/', '}', '{')):
                continue
            m = tanim.match(l)
            if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'sizeof'):
                h.setdefault(m.group(1), ad)
    return h


class Sembol:
    def __init__(self, d, m):
        self.slide = int(m.get('kaydirma', '0'))
        lo, hi = m.get('metin', '0 0').split()
        self.lo, self.hi = int(lo, 16), int(hi, 16)
        ikili = m['ikili']
        out = subprocess.run(['nm', '-n', ikili], capture_output=True, text=True).stdout
        self.adr, self.ad = [], []
        for l in out.splitlines():
            p = l.split()
            if len(p) >= 3 and p[1] in 'tT':
                self.adr.append(int(p[0], 16) + self.slide)
                self.ad.append(p[2][1:] if p[2].startswith('_') else p[2])
        self.sis = {}
        sy = os.path.join(d, 'sembol.txt')
        if os.path.exists(sy):
            for l in open(sy):
                p = l.split()
                if len(p) >= 2:
                    self.sis[int(p[0], 16)] = p[1] + ('' if len(p) < 3 else '')
        self.onbellek = {}

    def __call__(self, pc):
        r = self.onbellek.get(pc)
        if r is not None:
            return r
        if self.lo <= pc < self.hi:
            i = bisect.bisect_right(self.adr, pc) - 1
            r = self.ad[i] if i >= 0 else '?'
        else:
            r = self.sis.get(pc, '?sistem')
        self.onbellek[pc] = r
        return r


BEKLEME = re.compile(r'kevent|psynch|mach_msg|semwait|nanosleep|__ulock|swtch|__select|workq|__wait4|cond_wait')


def kategori(ad, kaynak):
    m = re.match(r'sub_00([0-9a-f]{6})', ad)
    if m:
        a = int(m.group(1), 16)
        return 'D3DX (lift)' if 0x650000 <= a < 0x690000 else 'oyun (lift)'
    if BEKLEME.search(ad):
        return 'bekleme/uyku'
    if re.match(r'(n[4-8]_|dxn|nat_|pl_plan|efx_)', ad):
        return 'D3DX (yerel)'
    k = kaynak.get(ad)
    if k:
        return {'com.c': 'D3D9 katmani (com.c)', 'metal.m': 'Metal (metal.m)', 'd3dx_hle.c': 'D3DX (yerel)',
                'win32.c': 'Win32', 'win32_ext.c': 'Win32', 'vsemu.c': 'vsemu', 'dispatch.c': 'dagitim',
                'genexec.c': 'dagitim', 'mem.c': 'bellek', 'enhance.c': 'HD (enhance)',
                'audio.m': 'ses', 'perflog.c': 'perflog'}.get(k, k)
    if ad.startswith(('x87_', 'fcompare', 'fsw_', 'FLAGS', 'parity')):
        return 'oyun (lift)'
    if ad in ('?', '?sistem'):
        return 'sistem (?)'
    if ad.startswith(('objc_', '-[', '+[', 'MTL', 'IOGPU', 'AGX', 'CA', 'CF', '_dispatch', 'dispatch')):
        return 'sistem: Metal/ObjC'
    if ad.startswith(('_platform_mem', 'memcpy', 'memmove', 'memset', 'bzero', '_platform_strcmp', 'strcmp')):
        return 'sistem: mem/str'
    return 'sistem: diger'


def ornekleri_oku(d):
    yol = os.path.join(d, 'ornek.bin')
    veri = open(yol, 'rb').read() if os.path.exists(yol) else b''
    i, out = 0, []
    while i + 16 <= len(veri):
        t, gf, rol, n, _ = struct.unpack_from('<QIBBH', veri, i)
        i += 16
        if i + 8 * n > len(veri):
            break
        pcs = struct.unpack_from('<%dQ' % n, veri, i)
        i += 8 * n
        out.append((t / 1e3, gf, rol, pcs))
    return out


# ---- powermetrics ----
def guc_oku(yol):
    if not os.path.exists(yol):
        return []
    ornek, cur = [], None
    for l in open(yol, errors='replace'):
        m = re.match(r'\*\*\* Sampled system activity \((.+?) [+-]\d{4}\) \(([\d.]+)ms elapsed\)', l)
        if m:
            t = datetime.datetime.strptime(m.group(1), '%a %b %d %H:%M:%S %Y').timestamp()
            cur = {'t': t, 'dt': float(m.group(2)) / 1000}
            ornek.append(cur)
            continue
        if cur is None:
            continue
        for k, nm in (('cpu', 'CPU Power'), ('gpu', 'GPU Power'), ('top', 'Combined Power (CPU + GPU + ANE)')):
            if l.startswith(nm + ':') and k not in cur:
                cur[k] = float(l.split(':')[1].split()[0]) / 1000
        m = re.match(r'GPU HW active frequency: (\d+) MHz', l)
        if m and 'gf' not in cur:
            cur['gf'] = float(m.group(1))
        m = re.match(r'P0-Cluster HW active frequency: (\d+) MHz', l)
        if m and 'pf' not in cur:
            cur['pf'] = float(m.group(1))
        m = re.match(r'Current pressure level: (\w+)', l)
        if m:
            cur['basinc'] = m.group(1)
    return [o for o in ornek if 'top' in o]


def ort(xs):
    xs = [x for x in xs if x is not None]
    return st.mean(xs) if xs else float('nan')


def main():
    d = sys.argv[1]
    pen_s = 5.0
    if '--pencere' in sys.argv:
        pen_s = float(sys.argv[sys.argv.index('--pencere') + 1])
    m = meta_oku(d)
    epoch0 = int(m.get('baslangic_epoch_us', '0')) / 1e6
    konuk = oku_csv(os.path.join(d, 'konuk.csv'))
    render = oku_csv(os.path.join(d, 'render.csv'))
    gpu = {int(r['kare']): r['gpu_us'] for r in oku_csv(os.path.join(d, 'gpu.csv'))}
    sec = oku_csv(os.path.join(d, 'saniye.csv'))
    thr = oku_csv(os.path.join(d, 'thread.csv'))
    guc = guc_oku(os.path.join(d, 'guc.txt'))
    for g in guc:
        g['tms'] = (g['t'] - g['dt'] / 2 - epoch0) * 1e3   # ornek ortasi, oturum zamaninda
    # oturum.sh + tools/smcoku: SMC sicaklik / fan (1 sn), yoksa bos
    sicak = oku_csv(os.path.join(d, 'sicaklik.csv')) if os.path.exists(os.path.join(d, 'sicaklik.csv')) else []
    for c in sicak:
        c['tms'] = (c['epoch_s'] - epoch0) * 1e3
    ekranlar = sorted(glob.glob(os.path.join(d, 'ekran', '*.jpg')))
    ekran_t = [(float(os.path.basename(e).split('_')[0]), e) for e in ekranlar]

    sb = Sembol(d, m)
    kaynak = kaynak_haritasi()
    orn = ornekleri_oku(d)

    R = []
    yaz = R.append
    sure = (render[-1]['t_ms'] - render[0]['t_ms']) / 1e3 if len(render) > 1 else 0
    yaz('# Oturum raporu: %s\n' % os.path.basename(os.path.normpath(d)))
    yaz('surum **%s**, build **%s**, ikili `%s`, sure %.0f sn, %d kare (render), %d konuk karesi, %d ornek, %d ekran goruntusu\n'
        % (('v' + m['surum']) if m.get('surum') else 'etiketsiz', m.get('build', '?'), os.path.basename(m.get('ikili', '?')), sure, len(render), len(konuk), len(orn), len(ekranlar)))

    # ---- genel ----
    ar = [r['aralik_us'] / 1e3 for r in render if r['aralik_us'] > 0]
    if ar:
        fps = 1000 / st.mean(ar)
        yaz('## Genel\n')
        yaz('| olcu | deger |\n|---|---|')
        yaz('| ortalama fps | %.1f |' % fps)
        for esik in (9, 12, 20, 34):
            n = sum(1 for x in ar if x > esik)
            yaz('| kare > %d ms | %d (%.2f%%) |' % (esik, n, 100 * n / len(ar)))
        yaz('| en uzun kare | %.1f ms |' % max(ar))
        if konuk:
            yaz('| konuk CPU / kare | %.2f ms |' % ort(k['cpu_us'] / 1e3 for k in konuk))
            yaz('| kare sinirlayici uykusu (bos pay) / kare | %.2f ms |' % ort(k['uyku_us'] / 1e3 for k in konuk))
        yaz('| ana thread kodlama / kare | %.2f ms |' % ort(r['kodlama_us'] / 1e3 for r in render))
        if gpu:
            yaz('| GPU / kare | %.2f ms |' % ort(v / 1e3 for v in gpu.values()))
        yaz('| cizim / kare | %.0f |' % ort(r['cizim'] for r in render))
        if len(sec) > 1:
            dt = (sec[-1]['t_ms'] - sec[0]['t_ms']) / 1e3
            yaz('| surec CPU enerjisi (rusage) | %.2f W (P cekirdek %.2f W) |'
                % ((sec[-1]['enerji_nj'] - sec[0]['enerji_nj']) / 1e9 / dt,
                   (sec[-1]['penerji_nj'] - sec[0]['penerji_nj']) / 1e9 / dt))
            cyc = sec[-1]['cevrim'] - sec[0]['cevrim']
            ins = sec[-1]['komut'] - sec[0]['komut']
            if cyc > 0:
                yaz('| IPC | %.2f |' % (ins / cyc))
        if guc:
            yaz('| powermetrics toplam / CPU / GPU | %.2f / %.2f / %.2f W |'
                % (ort(g['top'] for g in guc), ort(g['cpu'] for g in guc), ort(g['gpu'] for g in guc)))
            yaz('| GPU frekansi / P0 frekansi | %.0f / %.0f MHz |' % (ort(g.get('gf') for g in guc), ort(g.get('pf') for g in guc)))
        if sicak:
            ok = lambda k: [c[k] for c in sicak if c[k] > 0]
            yaz('| sicaklik CPU / GPU (ort / en yuksek) | %.1f / %.1f C, %.1f / %.1f C |'
                % (ort(ok('cpu_ort')), max(ok('cpu_max') or [0]), ort(ok('gpu_ort')), max(ok('gpu_max') or [0])))
            fv = [c[k] for c in sicak for k in ('fan0', 'fan1') if c[k] >= 0]   # 0 = fan durdu (gecerli)
            yaz('| fan (ort / en yuksek) | %.0f / %.0f dev/dk%s |' % (ort(fv), max(fv or [0]), ' (fanlar hic donmedi)' if fv and max(fv) == 0 else ''))
            yaz('| sistem gucu (SMC PSTR, ekran dahil) ort / en yuksek | %.1f / %.1f W |' % (ort(ok('sistem_w')), max(ok('sistem_w') or [0])))
        # oturum.sh: gercek ekran tazeleme hizi (DLINK=2) ve pil/guc modu.  Dusus
        # oyundan mi sistemden mi, ilk bakista buradan ayrilir.
        tl = os.path.join(d, 'oyun_tam.log')
        if os.path.exists(tl):
            hz = [float(x) for x in re.findall(r'^\[ekran\] ([\d.]+) Hz', open(tl, errors='replace').read(), re.M)]
            if hz:
                yaz('| ekran tazeleme (en dusuk / ort / en yuksek) | %.0f / %.0f / %.0f Hz |' % (min(hz), st.mean(hz), max(hz)))
        for ad, et in (('pil_bas.txt', 'pil / guc modu (bas)'), ('pil_son.txt', 'pil / guc modu (son)')):
            pp = os.path.join(d, ad)
            if os.path.exists(pp):
                t = open(pp, errors='replace').read()
                p = re.search(r'(\d+)%; ([^;]+);', t); lp = re.search(r'lowpowermode\s+(\d)', t)
                gk = 'sarj' if 'AC Power' in t else 'pil'
                yaz('| %s | %s%s, %s (%s), Dusuk Guc Modu %s |' % (et, p.group(1) if p else '?', '%' if p else '',
                    gk, (p.group(2) if p else '?'), ('acik' if lp and lp.group(1) == '1' else 'kapali')))
        yaz('')

    # ---- olaylar ----
    if konuk:
        top = lambda k: sum(x[k] for x in konuk)
        yaz('## Olaylar (oturum toplami)\n')
        yaz('| olay | sayi | sure |\n|---|---|---|')
        yaz('| VS derleme | %d | |' % top('vs_n'))
        yaz('| PS derleme | %d | |' % top('ps_n'))
        yaz('| PSO yaratma | %d | |' % top('pso_n'))
        yaz('| derleme toplam sure | | %.1f ms |' % (top('derleme_us') / 1e3))
        yaz('| doku yaratma | %d | |' % top('doku_n'))
        yaz('| doku yukleme | %.1f MB | %.1f ms |' % (top('doku_kb') / 1024, top('doku_us') / 1e3))
        yaz('')

    # ---- zaman cizelgesi ----
    t_bas = render[0]['t_ms'] if render else 0
    t_son = render[-1]['t_ms'] if render else 0
    pencereler = []
    t = t_bas
    while t < t_son:
        a, b = t, t + pen_s * 1000
        rr = [r for r in render if a <= r['t_ms'] < b]
        kk = [k for k in konuk if a <= k['t_ms'] < b]
        ss = [s for s in sec if a - 1000 <= s['t_ms'] < b + 1000]
        gg = [g for g in guc if a <= g['tms'] < b]
        cc = [c for c in sicak if a <= c['tms'] < b]
        w = {'a': a, 'b': b, 'n': len(rr)}
        if rr:
            w['fps'] = len(rr) / (pen_s)
            w['kodlama'] = ort(r['kodlama_us'] / 1e3 for r in rr)
            w['cizim'] = ort(r['cizim'] for r in rr)
            w['gpu'] = ort(gpu.get(int(r['kare'])) for r in rr) / 1e3 if gpu else float('nan')
            w['maks'] = max(r['aralik_us'] for r in rr) / 1e3
        if kk:
            w['kcpu'] = ort(k['cpu_us'] / 1e3 for k in kk)
            w['uyku'] = ort(k['uyku_us'] / 1e3 for k in kk)
            w['derleme'] = sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in kk)
            w['doku_mb'] = sum(k['doku_kb'] for k in kk) / 1024
        ss2 = [s for s in sec if a <= s['t_ms'] <= b]
        if len(ss2) >= 2:
            dt = (ss2[-1]['t_ms'] - ss2[0]['t_ms']) / 1e3
            w['cpuw'] = (ss2[-1]['enerji_nj'] - ss2[0]['enerji_nj']) / 1e9 / dt
            w['termal'] = max(s['termal'] for s in ss2)
        if gg:
            w['pm_cpu'] = ort(g['cpu'] for g in gg)
            w['pm_gpu'] = ort(g['gpu'] for g in gg)
            w['pm_gf'] = ort(g.get('gf') for g in gg)
        if cc:
            w['gpu_c'] = max(c['gpu_max'] for c in cc)
            w['fan'] = ort([c[k] for c in cc for k in ('fan0', 'fan1') if c[k] >= 0])
        e = [x for x in ekran_t if a <= x[0] < b]
        w['ekran'] = os.path.relpath(e[len(e) // 2][1], d) if e else ''
        pencereler.append(w)
        t = b

    def f(w, k, fmt='%.2f'):
        v = w.get(k)
        return '' if v is None or v != v else fmt % v

    yaz('## Zaman cizelgesi (%.0f sn pencereler)\n' % pen_s)
    yaz('Konuk CPU = oyun thread\'inin kare basina gercek CPU suresi; uyku = kare sinirlayicinin '
        'bos payi (yuksek = rahat).  CPU W = surecin kendi enerji sayaci (sudo gerekmez); pm = powermetrics.\n')
    yaz('| t (sn) | fps | konuk CPU ms | uyku ms | kodlama ms | GPU ms | cizim | en uzun ms | CPU W | pm CPU W | pm GPU W | GPU MHz | termal | GPU C | fan | derleme | doku MB | ekran |')
    yaz('|' + '---|' * 18)
    for w in pencereler:
        yaz('| %.0f | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            (w['a'] - t_bas) / 1e3, f(w, 'fps', '%.1f'), f(w, 'kcpu'), f(w, 'uyku'), f(w, 'kodlama'), f(w, 'gpu'),
            f(w, 'cizim', '%.0f'), f(w, 'maks', '%.1f'), f(w, 'cpuw'), f(w, 'pm_cpu'), f(w, 'pm_gpu'),
            f(w, 'pm_gf', '%.0f'), f(w, 'termal', '%.0f'), f(w, 'gpu_c', '%.0f'), f(w, 'fan', '%.0f'), f(w, 'derleme', '%.0f'), f(w, 'doku_mb', '%.1f'),
            ('![](%s)' % w['ekran']) if w['ekran'] else ''))
    yaz('')

    # ---- ornek profili ----
    def profil(secim, baslik, n_self=30, n_incl=25):
        if not secim:
            return
        self_c, incl_c, kat_c = collections.Counter(), collections.Counter(), collections.Counter()
        for _, _, _, pcs in secim:
            adlar = [sb(p) for p in pcs]
            self_c[adlar[0]] += 1
            kat_c[kategori(adlar[0], kaynak)] += 1
            for a in set(adlar):
                if a not in ('?', '?sistem'):
                    incl_c[a] += 1
        N = len(secim)
        yaz('### %s (%d ornek)\n' % (baslik, N))
        yaz('| kategori (oz) | pay |\n|---|---|')
        for k, v in kat_c.most_common():
            yaz('| %s | %.1f%% |' % (k, 100 * v / N))
        yaz('\n| fonksiyon (oz) | pay | kategori |\n|---|---|---|')
        for k, v in self_c.most_common(n_self):
            yaz('| `%s` | %.2f%% | %s |' % (k, 100 * v / N, kategori(k, kaynak)))
        yaz('\n| fonksiyon (kapsayici) | pay |\n|---|---|')
        for k, v in incl_c.most_common(n_incl):
            yaz('| `%s` | %.1f%% |' % (k, 100 * v / N))
        yaz('')

    konuk_orn = [o for o in orn if o[2] == 0]
    ana_orn = [o for o in orn if o[2] == 1]
    yaz('## Profil: oyun thread\'i (1 ms ornekleme, duvar saati)\n')
    yaz('Pay = ornek orani; bekleme/uyku dahil.  ms/kare icin pay x kare suresi (~8.33 ms).\n')
    profil(konuk_orn, 'Tum oturum')

    # en agir pencereler: konuk CPU'ya gore
    agir = sorted([w for w in pencereler if w.get('kcpu') == w.get('kcpu') and w.get('n', 0) > 50],
                  key=lambda w: -w['kcpu'])[:3]
    for w in agir:
        sec_o = [o for o in konuk_orn if w['a'] <= o[0] < w['b']]
        profil(sec_o, 'En agir pencere: t=%.0f-%.0f sn, konuk CPU %.2f ms/kare, GPU %s ms %s'
               % ((w['a'] - t_bas) / 1e3, (w['b'] - t_bas) / 1e3, w['kcpu'], f(w, 'gpu'),
                  ('![](%s)' % w['ekran']) if w['ekran'] else ''), 20, 0)
    yaz('## Profil: ana (render) thread\'i (4 ms ornekleme)\n')
    profil(ana_orn, 'Tum oturum', 20, 15)

    # ---- takilmalar ----
    yaz('## Takilmalar (kare araligi > 12 ms)\n')
    tak = [r for r in render if r['aralik_us'] > 12000]
    if not tak:
        yaz('yok\n')
    else:
        yaz('| t (sn) | kare | aralik ms | yakin konuk CPU ms (maks) | derleme | doku KB | GPU ms | ekran |\n|---|---|---|---|---|---|---|---|')
        for r in tak[:60]:
            kk = [k for k in konuk if r['t_ms'] - 60 <= k['t_ms'] <= r['t_ms'] + 20]
            e = min(ekran_t, key=lambda x: abs(x[0] - r['t_ms'])) if ekran_t else None
            yaz('| %.2f | %d | %.1f | %s | %d | %d | %s | %s |' % (
                (r['t_ms'] - t_bas) / 1e3, r['kare'], r['aralik_us'] / 1e3,
                '%.1f' % max(k['cpu_us'] / 1e3 for k in kk) if kk else '',
                sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in kk), sum(k['doku_kb'] for k in kk),
                '%.1f' % (gpu[int(r['kare'])] / 1e3) if int(r['kare']) in gpu else '',
                ('![](%s)' % os.path.relpath(e[1], d)) if e and abs(e[0] - r['t_ms']) < 1500 else ''))
        if len(tak) > 60:
            yaz('\n... toplam %d takilma' % len(tak))
        yaz('')

    # ---- thread'ler ----
    if thr:
        ilk, son = {}, {}
        for r in thr:
            tid = int(r['tid'])
            ilk.setdefault(tid, r)
            son[tid] = r
        dt = (thr[-1]['t_ms'] - thr[0]['t_ms']) / 1e3
        satir = []
        for tid, r in son.items():
            a = ilk[tid]
            ku = (r['kull_ns'] - a['kull_ns']) / 1e9 / dt
            si = (r['sis_ns'] - a['sis_ns']) / 1e9 / dt
            satir.append((ku + si, ku, si, r.get('rol', ''), r.get('ad', ''), tid))
        satir.sort(reverse=True)
        yaz('## Thread\'ler (cekirdek = CPU sn / duvar sn)\n')
        # diger thread'lerin ornekleri (rol 2, 10 ms): thread basina en sik PC < LR
        diger = collections.defaultdict(collections.Counter)
        for _, tid32, rol, pcs in orn:
            if rol == 2:
                diger[tid32][' < '.join(sb(p) for p in pcs[:2])] += 1
        yaz('| rol | ad | toplam | kullanici | sistem | en sik (PC < LR, 10 ms ornek) |\n|---|---|---|---|---|---|')
        for t_, ku, si, rol, ad, tid in satir[:16]:
            c = diger.get(tid & 0xFFFFFFFF)
            ne = ''
            if c:
                N = sum(c.values())
                ne = '; '.join('`%s` %.0f%%' % (k, 100 * v / N) for k, v in c.most_common(3))
            yaz('| %s | %s | %.3f | %.3f | %.3f | %s |' % (rol, ad or tid, t_, ku, si, ne))
        yaz('| | **toplam** | **%.3f** | %.3f | %.3f | |' % (sum(x[0] for x in satir), sum(x[1] for x in satir), sum(x[2] for x in satir)))
        yaz('')

    open(os.path.join(d, 'rapor.md'), 'w').write('\n'.join(R) + '\n')
    print('\n'.join(R[:40]))
    print('...\nrapor: %s' % os.path.join(d, 'rapor.md'))


if __name__ == '__main__':
    main()
