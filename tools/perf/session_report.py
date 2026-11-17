#!/usr/bin/env python3
"""Analyze a real game session recording (PERFLOG) -> <dir>/report.md

    python3 tools/perf/session_report.py <dir> [--window 5]

Input: the files runtime/perflog.c writes (+ the powermetrics output power.txt
if present).  Standard library + `nm` only.  Recordings made before the English
rename (konuk.csv, saniye.csv, Turkish column names, ...) are read too.
"""
import bisect, collections, csv, datetime, glob, os, re, statistics as st, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Old (Turkish) names -> current names, so older recordings still work.
OLD_COLS = {
    'gkare': 'gframe', 'duvar_us': 'wall_us', 'uyku_us': 'sleep_us', 'present_bekle_us': 'present_wait_us',
    'derleme_us': 'compile_us', 'doku_n': 'tex_n', 'doku_kb': 'tex_kb', 'doku_us': 'tex_us',
    'kare': 'frame', 'aralik_us': 'interval_us', 'drawable_bekle_us': 'drawable_wait_us', 'kodlama_us': 'encode_us',
    'cizim': 'draws', 'kose': 'verts', 'dusen': 'dropped', 'doku_farkli': 'tex_distinct',
    'enerji_nj': 'energy_nj', 'penerji_nj': 'penergy_nj', 'komut': 'instr', 'cevrim': 'cycles',
    'pkomut': 'pinstr', 'pcevrim': 'pcycles', 'kull_t': 'user_t', 'sis_t': 'sys_t', 'pkull_t': 'puser_t',
    'psis_t': 'psys_t', 'gpu_kullanim': 'gpu_util', 'termal': 'thermal', 'bellek': 'memory',
    'rol': 'role', 'ad': 'name', 'kull_ns': 'user_ns', 'sis_ns': 'sys_ns',
    'cpu_ort': 'cpu_avg', 'gpu_ort': 'gpu_avg', 'sistem_w': 'system_w',
}
OLD_VALUES = {'KONUK': 'GUEST', 'ANA': 'MAIN'}
OLD_META = {'ikili': 'binary', 'kaydirma': 'slide', 'metin': 'text', 'zaman_tabani': 'timebase',
            'surum': 'version', 'baslangic_epoch_us': 'start_epoch_us'}


def path_of(d, *names):
    """The first of the given names that exists in d (current name first, then old ones)."""
    for n in names:
        p = os.path.join(d, n)
        if os.path.exists(p):
            return p
    return os.path.join(d, names[0])


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        r = csv.reader(f)
        head = next(r, None)
        if not head:
            return []
        head = [OLD_COLS.get(h, h) for h in head]
        out = []
        for row in r:
            if len(row) != len(head):
                continue
            d = {}
            for k, v in zip(head, row):
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = OLD_VALUES.get(v, v)
            out.append(d)
        return out


def read_meta(d):
    m = {}
    for l in open(os.path.join(d, 'meta.txt')):
        k, _, v = l.strip().partition(' ')
        m.setdefault(OLD_META.get(k, k), v)
    return m


# ---- symbols ----
def source_map():
    """runtime/*.c|m function definitions -> file name (for categories)"""
    h = {}
    defn = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
    for path in glob.glob(os.path.join(ROOT, 'runtime', '*.[cm]')):
        name = os.path.basename(path)
        for l in open(path, errors='replace'):
            if l.startswith((' ', '\t', '#', '/', '}', '{')):
                continue
            m = defn.match(l)
            if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'sizeof'):
                h.setdefault(m.group(1), name)
    return h


class Symbol:
    def __init__(self, d, m):
        self.slide = int(m.get('slide', '0'))
        lo, hi = m.get('text', '0 0').split()
        self.lo, self.hi = int(lo, 16), int(hi, 16)
        binary = m['binary']
        out = subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout
        self.addr, self.name = [], []
        for l in out.splitlines():
            p = l.split()
            if len(p) >= 3 and p[1] in 'tT':
                self.addr.append(int(p[0], 16) + self.slide)
                self.name.append(p[2][1:] if p[2].startswith('_') else p[2])
        self.sys = {}
        sy = path_of(d, 'symbols.txt', 'sembol.txt')
        if os.path.exists(sy):
            for l in open(sy):
                p = l.split()
                if len(p) >= 2:
                    self.sys[int(p[0], 16)] = p[1]
        self.cache = {}

    def __call__(self, pc):
        r = self.cache.get(pc)
        if r is not None:
            return r
        if self.lo <= pc < self.hi:
            i = bisect.bisect_right(self.addr, pc) - 1
            r = self.name[i] if i >= 0 else '?'
        else:
            r = self.sys.get(pc, '?system')
        self.cache[pc] = r
        return r


WAIT = re.compile(r'kevent|psynch|mach_msg|semwait|nanosleep|__ulock|swtch|__select|workq|__wait4|cond_wait')


def category(name, src):
    m = re.match(r'sub_00([0-9a-f]{6})', name)
    if m:
        a = int(m.group(1), 16)
        return 'D3DX (lift)' if 0x650000 <= a < 0x690000 else 'game (lift)'
    if WAIT.search(name):
        return 'wait/sleep'
    if re.match(r'(n[4-8]_|dxn|nat_|pl_plan|efx_)', name):
        return 'D3DX (native)'
    k = src.get(name)
    if k:
        return {'com.c': 'D3D9 layer (com.c)', 'metal.m': 'Metal (metal.m)', 'd3dx_hle.c': 'D3DX (native)',
                'win32.c': 'Win32', 'win32_ext.c': 'Win32', 'vsemu.c': 'vsemu', 'dispatch.c': 'dispatch',
                'genexec.c': 'dispatch', 'mem.c': 'memory', 'enhance.c': 'HD (enhance)',
                'audio.m': 'audio', 'perflog.c': 'perflog'}.get(k, k)
    if name.startswith(('x87_', 'fcompare', 'fsw_', 'FLAGS', 'parity')):
        return 'game (lift)'
    if name in ('?', '?system'):
        return 'system (?)'
    if name.startswith(('objc_', '-[', '+[', 'MTL', 'IOGPU', 'AGX', 'CA', 'CF', '_dispatch', 'dispatch')):
        return 'system: Metal/ObjC'
    if name.startswith(('_platform_mem', 'memcpy', 'memmove', 'memset', 'bzero', '_platform_strcmp', 'strcmp')):
        return 'system: mem/str'
    return 'system: other'


def read_samples(d):
    path = path_of(d, 'samples.bin', 'ornek.bin')
    data = open(path, 'rb').read() if os.path.exists(path) else b''
    i, out = 0, []
    while i + 16 <= len(data):
        t, gf, role, n, _ = struct.unpack_from('<QIBBH', data, i)
        i += 16
        if i + 8 * n > len(data):
            break
        pcs = struct.unpack_from('<%dQ' % n, data, i)
        i += 8 * n
        out.append((t / 1e3, gf, role, pcs))
    return out


# ---- powermetrics ----
def read_power(path):
    if not os.path.exists(path):
        return []
    samples, cur = [], None
    for l in open(path, errors='replace'):
        m = re.match(r'\*\*\* Sampled system activity \((.+?) [+-]\d{4}\) \(([\d.]+)ms elapsed\)', l)
        if m:
            t = datetime.datetime.strptime(m.group(1), '%a %b %d %H:%M:%S %Y').timestamp()
            cur = {'t': t, 'dt': float(m.group(2)) / 1000}
            samples.append(cur)
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
            cur['pressure'] = m.group(1)
    return [o for o in samples if 'top' in o]


def mean(xs):
    xs = [x for x in xs if x is not None]
    return st.mean(xs) if xs else float('nan')


def main():
    d = sys.argv[1]
    win_s = 5.0
    for flag in ('--window', '--pencere'):
        if flag in sys.argv:
            win_s = float(sys.argv[sys.argv.index(flag) + 1])
    m = read_meta(d)
    epoch0 = int(m.get('start_epoch_us', '0')) / 1e6
    guest = read_csv(path_of(d, 'guest.csv', 'konuk.csv'))
    render = read_csv(os.path.join(d, 'render.csv'))
    gpu = {int(r['frame']): r['gpu_us'] for r in read_csv(os.path.join(d, 'gpu.csv'))}
    sec = read_csv(path_of(d, 'seconds.csv', 'saniye.csv'))
    thr = read_csv(os.path.join(d, 'thread.csv'))
    power = read_power(path_of(d, 'power.txt', 'guc.txt'))
    for g in power:
        g['tms'] = (g['t'] - g['dt'] / 2 - epoch0) * 1e3   # middle of the sample, in session time
    # session.sh + tools/smcread: SMC temperature / fan (1 s), empty if missing
    temp = read_csv(path_of(d, 'temperature.csv', 'sicaklik.csv'))
    for c in temp:
        c['tms'] = (c['epoch_s'] - epoch0) * 1e3
    screens = sorted(glob.glob(os.path.join(d, 'screens', '*.jpg')) or glob.glob(os.path.join(d, 'ekran', '*.jpg')))
    screen_t = [(float(os.path.basename(e).split('_')[0]), e) for e in screens]

    sb = Symbol(d, m)
    src = source_map()
    samples = read_samples(d)

    R = []
    out = R.append
    duration = (render[-1]['t_ms'] - render[0]['t_ms']) / 1e3 if len(render) > 1 else 0
    out('# Session report: %s\n' % os.path.basename(os.path.normpath(d)))
    out('version **%s**, build **%s**, binary `%s`, duration %.0f s, %d frames (render), %d guest frames, %d samples, %d screenshots\n'
        % (('v' + m['version']) if m.get('version') else 'untagged', m.get('build', '?'), os.path.basename(m.get('binary', '?')),
           duration, len(render), len(guest), len(samples), len(screens)))

    # ---- overview ----
    iv = [r['interval_us'] / 1e3 for r in render if r['interval_us'] > 0]
    if iv:
        fps = 1000 / st.mean(iv)
        out('## Overview\n')
        out('| measure | value |\n|---|---|')
        out('| mean fps | %.1f |' % fps)
        for thr_ms in (9, 12, 20, 34):
            n = sum(1 for x in iv if x > thr_ms)
            out('| frames > %d ms | %d (%.2f%%) |' % (thr_ms, n, 100 * n / len(iv)))
        out('| longest frame | %.1f ms |' % max(iv))
        if guest:
            out('| guest CPU / frame | %.2f ms |' % mean(k['cpu_us'] / 1e3 for k in guest))
            out('| frame limiter sleep (idle margin) / frame | %.2f ms |' % mean(k['sleep_us'] / 1e3 for k in guest))
        out('| main thread encoding / frame | %.2f ms |' % mean(r['encode_us'] / 1e3 for r in render))
        if gpu:
            out('| GPU / frame | %.2f ms |' % mean(v / 1e3 for v in gpu.values()))
        out('| draws / frame | %.0f |' % mean(r['draws'] for r in render))
        if len(sec) > 1:
            dt = (sec[-1]['t_ms'] - sec[0]['t_ms']) / 1e3
            out('| process CPU energy (rusage) | %.2f W (P cores %.2f W) |'
                % ((sec[-1]['energy_nj'] - sec[0]['energy_nj']) / 1e9 / dt,
                   (sec[-1]['penergy_nj'] - sec[0]['penergy_nj']) / 1e9 / dt))
            cyc = sec[-1]['cycles'] - sec[0]['cycles']
            ins = sec[-1]['instr'] - sec[0]['instr']
            if cyc > 0:
                out('| IPC | %.2f |' % (ins / cyc))
        if power:
            out('| powermetrics total / CPU / GPU | %.2f / %.2f / %.2f W |'
                % (mean(g['top'] for g in power), mean(g['cpu'] for g in power), mean(g['gpu'] for g in power)))
            out('| GPU frequency / P0 frequency | %.0f / %.0f MHz |' % (mean(g.get('gf') for g in power), mean(g.get('pf') for g in power)))
        if temp:
            ok = lambda k: [c[k] for c in temp if c[k] > 0]
            out('| temperature CPU / GPU (mean / max) | %.1f / %.1f C, %.1f / %.1f C |'
                % (mean(ok('cpu_avg')), max(ok('cpu_max') or [0]), mean(ok('gpu_avg')), max(ok('gpu_max') or [0])))
            fv = [c[k] for c in temp for k in ('fan0', 'fan1') if c[k] >= 0]   # 0 = fan stopped (valid)
            out('| fan (mean / max) | %.0f / %.0f rpm%s |' % (mean(fv), max(fv or [0]), ' (fans never spun)' if fv and max(fv) == 0 else ''))
            out('| system power (SMC PSTR, display included) mean / max | %.1f / %.1f W |' % (mean(ok('system_w')), max(ok('system_w') or [0])))
        # session.sh: the real display refresh rate (DLINK=2) and battery/power mode.
        # Tells at a glance whether a drop comes from the game or the system.
        tl = path_of(d, 'game_full.log', 'oyun_tam.log')
        if os.path.exists(tl):
            hz = [float(x) for x in re.findall(r'^\[(?:display|ekran)\] ([\d.]+) Hz', open(tl, errors='replace').read(), re.M)]
            if hz:
                out('| display refresh (min / mean / max) | %.0f / %.0f / %.0f Hz |' % (min(hz), st.mean(hz), max(hz)))
        for names, label in ((('battery_start.txt', 'pil_bas.txt'), 'battery / power mode (start)'),
                             (('battery_end.txt', 'pil_son.txt'), 'battery / power mode (end)')):
            pp = path_of(d, *names)
            if os.path.exists(pp):
                t = open(pp, errors='replace').read()
                p = re.search(r'(\d+)%; ([^;]+);', t); lp = re.search(r'lowpowermode\s+(\d)', t)
                src_kind = 'charger' if 'AC Power' in t else 'battery'
                out('| %s | %s%s, %s (%s), Low Power Mode %s |' % (label, p.group(1) if p else '?', '%' if p else '',
                    src_kind, (p.group(2) if p else '?'), ('on' if lp and lp.group(1) == '1' else 'off')))
        out('')

    # ---- events ----
    if guest:
        total = lambda k: sum(x[k] for x in guest)
        out('## Events (session total)\n')
        out('| event | count | time |\n|---|---|---|')
        out('| VS compiles | %d | |' % total('vs_n'))
        out('| PS compiles | %d | |' % total('ps_n'))
        out('| PSO creations | %d | |' % total('pso_n'))
        out('| total compile time | | %.1f ms |' % (total('compile_us') / 1e3))
        out('| texture creations | %d | |' % total('tex_n'))
        out('| texture uploads | %.1f MB | %.1f ms |' % (total('tex_kb') / 1024, total('tex_us') / 1e3))
        out('')

    # ---- timeline ----
    t_start = render[0]['t_ms'] if render else 0
    t_end = render[-1]['t_ms'] if render else 0
    windows = []
    t = t_start
    while t < t_end:
        a, b = t, t + win_s * 1000
        rr = [r for r in render if a <= r['t_ms'] < b]
        kk = [k for k in guest if a <= k['t_ms'] < b]
        gg = [g for g in power if a <= g['tms'] < b]
        cc = [c for c in temp if a <= c['tms'] < b]
        w = {'a': a, 'b': b, 'n': len(rr)}
        if rr:
            w['fps'] = len(rr) / (win_s)
            w['encode'] = mean(r['encode_us'] / 1e3 for r in rr)
            w['draws'] = mean(r['draws'] for r in rr)
            w['gpu'] = mean(gpu.get(int(r['frame'])) for r in rr) / 1e3 if gpu else float('nan')
            w['max'] = max(r['interval_us'] for r in rr) / 1e3
        if kk:
            w['gcpu'] = mean(k['cpu_us'] / 1e3 for k in kk)
            w['sleep'] = mean(k['sleep_us'] / 1e3 for k in kk)
            w['compiles'] = sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in kk)
            w['tex_mb'] = sum(k['tex_kb'] for k in kk) / 1024
        ss2 = [s for s in sec if a <= s['t_ms'] <= b]
        if len(ss2) >= 2:
            dt = (ss2[-1]['t_ms'] - ss2[0]['t_ms']) / 1e3
            w['cpuw'] = (ss2[-1]['energy_nj'] - ss2[0]['energy_nj']) / 1e9 / dt
            w['thermal'] = max(s['thermal'] for s in ss2)
        if gg:
            w['pm_cpu'] = mean(g['cpu'] for g in gg)
            w['pm_gpu'] = mean(g['gpu'] for g in gg)
            w['pm_gf'] = mean(g.get('gf') for g in gg)
        if cc:
            w['gpu_c'] = max(c['gpu_max'] for c in cc)
            w['fan'] = mean([c[k] for c in cc for k in ('fan0', 'fan1') if c[k] >= 0])
        e = [x for x in screen_t if a <= x[0] < b]
        w['screen'] = os.path.relpath(e[len(e) // 2][1], d) if e else ''
        windows.append(w)
        t = b

    def f(w, k, fmt='%.2f'):
        v = w.get(k)
        return '' if v is None or v != v else fmt % v

    out('## Timeline (%.0f s windows)\n' % win_s)
    out('Guest CPU = the game thread\'s real CPU time per frame; sleep = the frame limiter\'s '
        'idle margin (high = relaxed).  CPU W = the process\'s own energy counter (no sudo needed); pm = powermetrics.\n')
    out('| t (s) | fps | guest CPU ms | sleep ms | encode ms | GPU ms | draws | longest ms | CPU W | pm CPU W | pm GPU W | GPU MHz | thermal | GPU C | fan | compiles | tex MB | screen |')
    out('|' + '---|' * 18)
    for w in windows:
        out('| %.0f | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            (w['a'] - t_start) / 1e3, f(w, 'fps', '%.1f'), f(w, 'gcpu'), f(w, 'sleep'), f(w, 'encode'), f(w, 'gpu'),
            f(w, 'draws', '%.0f'), f(w, 'max', '%.1f'), f(w, 'cpuw'), f(w, 'pm_cpu'), f(w, 'pm_gpu'),
            f(w, 'pm_gf', '%.0f'), f(w, 'thermal', '%.0f'), f(w, 'gpu_c', '%.0f'), f(w, 'fan', '%.0f'), f(w, 'compiles', '%.0f'), f(w, 'tex_mb', '%.1f'),
            ('![](%s)' % w['screen']) if w['screen'] else ''))
    out('')

    # ---- sample profile ----
    def profile(sel, title, n_self=30, n_incl=25):
        if not sel:
            return
        self_c, incl_c, cat_c = collections.Counter(), collections.Counter(), collections.Counter()
        for _, _, _, pcs in sel:
            names = [sb(p) for p in pcs]
            self_c[names[0]] += 1
            cat_c[category(names[0], src)] += 1
            for a in set(names):
                if a not in ('?', '?system'):
                    incl_c[a] += 1
        N = len(sel)
        out('### %s (%d samples)\n' % (title, N))
        out('| category (self) | share |\n|---|---|')
        for k, v in cat_c.most_common():
            out('| %s | %.1f%% |' % (k, 100 * v / N))
        out('\n| function (self) | share | category |\n|---|---|---|')
        for k, v in self_c.most_common(n_self):
            out('| `%s` | %.2f%% | %s |' % (k, 100 * v / N, category(k, src)))
        out('\n| function (inclusive) | share |\n|---|---|')
        for k, v in incl_c.most_common(n_incl):
            out('| `%s` | %.1f%% |' % (k, 100 * v / N))
        out('')

    guest_s = [o for o in samples if o[2] == 0]
    main_s = [o for o in samples if o[2] == 1]
    out('## Profile: game thread (1 ms sampling, wall clock)\n')
    out('Share = sample ratio; wait/sleep included.  For ms/frame: share x frame time (~8.33 ms).\n')
    profile(guest_s, 'Whole session')

    # heaviest windows: by guest CPU
    heavy = sorted([w for w in windows if w.get('gcpu') == w.get('gcpu') and w.get('n', 0) > 50],
                   key=lambda w: -w['gcpu'])[:3]
    for w in heavy:
        sel = [o for o in guest_s if w['a'] <= o[0] < w['b']]
        profile(sel, 'Heaviest window: t=%.0f-%.0f s, guest CPU %.2f ms/frame, GPU %s ms %s'
                % ((w['a'] - t_start) / 1e3, (w['b'] - t_start) / 1e3, w['gcpu'], f(w, 'gpu'),
                   ('![](%s)' % w['screen']) if w['screen'] else ''), 20, 0)
    out('## Profile: main (render) thread (4 ms sampling)\n')
    profile(main_s, 'Whole session', 20, 15)

    # ---- hitches ----
    out('## Hitches (frame interval > 12 ms)\n')
    hitches = [r for r in render if r['interval_us'] > 12000]
    if not hitches:
        out('none\n')
    else:
        out('| t (s) | frame | interval ms | nearby guest CPU ms (max) | compiles | tex KB | GPU ms | screen |\n|---|---|---|---|---|---|---|---|')
        for r in hitches[:60]:
            kk = [k for k in guest if r['t_ms'] - 60 <= k['t_ms'] <= r['t_ms'] + 20]
            e = min(screen_t, key=lambda x: abs(x[0] - r['t_ms'])) if screen_t else None
            out('| %.2f | %d | %.1f | %s | %d | %d | %s | %s |' % (
                (r['t_ms'] - t_start) / 1e3, r['frame'], r['interval_us'] / 1e3,
                '%.1f' % max(k['cpu_us'] / 1e3 for k in kk) if kk else '',
                sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in kk), sum(k['tex_kb'] for k in kk),
                '%.1f' % (gpu[int(r['frame'])] / 1e3) if int(r['frame']) in gpu else '',
                ('![](%s)' % os.path.relpath(e[1], d)) if e and abs(e[0] - r['t_ms']) < 1500 else ''))
        if len(hitches) > 60:
            out('\n... %d hitches in total' % len(hitches))
        out('')

    # ---- threads ----
    if thr:
        first, last = {}, {}
        for r in thr:
            tid = int(r['tid'])
            first.setdefault(tid, r)
            last[tid] = r
        dt = (thr[-1]['t_ms'] - thr[0]['t_ms']) / 1e3
        rows = []
        for tid, r in last.items():
            a = first[tid]
            us = (r['user_ns'] - a['user_ns']) / 1e9 / dt
            sy = (r['sys_ns'] - a['sys_ns']) / 1e9 / dt
            rows.append((us + sy, us, sy, r.get('role', ''), r.get('name', ''), tid))
        rows.sort(reverse=True)
        out('## Threads (cores = CPU s / wall s)\n')
        # samples of the other threads (role 2, 10 ms): most common PC < LR per thread
        other = collections.defaultdict(collections.Counter)
        for _, tid32, role, pcs in samples:
            if role == 2:
                other[tid32][' < '.join(sb(p) for p in pcs[:2])] += 1
        out('| role | name | total | user | system | most common (PC < LR, 10 ms samples) |\n|---|---|---|---|---|---|')
        for t_, us, sy, role, name, tid in rows[:16]:
            c = other.get(tid & 0xFFFFFFFF)
            top = ''
            if c:
                N = sum(c.values())
                top = '; '.join('`%s` %.0f%%' % (k, 100 * v / N) for k, v in c.most_common(3))
            out('| %s | %s | %.3f | %.3f | %.3f | %s |' % (role, name or tid, t_, us, sy, top))
        out('| | **total** | **%.3f** | %.3f | %.3f | |' % (sum(x[0] for x in rows), sum(x[1] for x in rows), sum(x[2] for x in rows)))
        out('')

    open(os.path.join(d, 'report.md'), 'w').write('\n'.join(R) + '\n')
    print('\n'.join(R[:40]))
    print('...\nreport: %s' % os.path.join(d, 'report.md'))


if __name__ == '__main__':
    main()
