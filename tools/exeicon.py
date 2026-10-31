#!/usr/bin/env python3
"""Build the app icon (.icns) from SPEED2.EXE's own icon resource.

The exe carries one icon group, IDI_ICON1, with a single 32x32 24-bit image
(the grey NFS logo; the NFSU_icon.ico next to it is the same picture).  The
logo is white on grey, every other shade is anti-aliasing, so it is traced
back to vectors: the edge is taken where a smooth enlargement of the
coverage crosses 50 %, fitted with straight lines, and the lines are snapped
to the logo's own angles (level and its italic slant) so the corners come
out sharp.  Other icons are enlarged in nearest-neighbour steps.  The result
is cut to the macOS icon shape (an 824 px rounded tile on the 1024 grid,
with a soft shadow) and written in every size iconutil wants.  For personal
use: the artwork is EA's.

Usage:  tools/exeicon.py <SPEED2.EXE> <out.icns> [<preview.png>]
"""
import io, math, os, struct, subprocess, sys, tempfile
import pefile
from PIL import Image, ImageChops, ImageDraw, ImageFilter

RT_ICON, RT_GROUP_ICON = 3, 14
CANVAS, TILE, RADIUS = 1024, 824, 185          # Big Sur+ icon grid


def exe_icons(path):
    """All images of the exe's first icon group, as (w, h, bpp, bytes)."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    icons, group = {}, None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for e in t.directory.entries:
            for lang in e.directory.entries:
                d = lang.data.struct
                raw = pe.get_data(d.OffsetToData, d.Size)
                if t.id == RT_ICON:
                    icons[e.id] = raw
                elif t.id == RT_GROUP_ICON and group is None:
                    group = raw
    if group is None:
        sys.exit("exeicon: no icon group in %s" % path)
    _, _, count = struct.unpack_from('<HHH', group)
    out = []
    for i in range(count):
        w, h, _, _, _, bpp, size, iid = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
        if iid in icons:
            out.append((w or 256, h or 256, bpp, icons[iid]))
    return out


def decode(w, h, bpp, raw):
    """One RT_ICON image (PNG or BMP DIB with AND mask) -> RGBA."""
    if raw[:8] == b'\x89PNG\r\n\x1a\n':
        return Image.open(io.BytesIO(raw)).convert('RGBA')
    # Wrap the DIB in a one-image .ico and let Pillow apply the AND mask.
    ico = struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', w & 255, h & 255, 0, 0, 1, bpp,
                                                    len(raw), 22) + raw
    im = Image.open(io.BytesIO(ico))
    im.size = (w, h)
    return im.convert('RGBA')


def pixels(im):
    p = im.convert('RGB').load()
    return [p[x, y] for y in range(im.height) for x in range(im.width)]


def two_tone(art):
    """(background, foreground) if art is one colour on another plus blends of the two."""
    px = pixels(art)
    bg = max(set(px), key=px.count)
    fg = max(px, key=lambda c: sum((a - b) ** 2 for a, b in zip(c, bg)))
    span = sum((a - b) ** 2 for a, b in zip(fg, bg))
    if span < 64 ** 2:
        return None
    for c in set(px):
        t = sum((a - b) * (f - b) for a, b, f in zip(c, bg, fg)) / span
        if sum((a - (b + t * (f - b))) ** 2 for a, b, f in zip(c, bg, fg)) > 12 ** 2:
            return None
    return bg, fg


def contours(f, w, h):
    """Marching squares at 0.5 on f[y][x] (0 outside): closed loops of points."""
    def v(x, y):
        return f[y][x] if 0 <= x < w and 0 <= y < h else 0.0

    def pt(e):
        k, x, y = e
        a, b = (v(x, y), v(x + 1, y)) if k == 'h' else (v(x, y), v(x, y + 1))
        t = (0.5 - a) / (b - a)
        return (x + t, y) if k == 'h' else (x, y + t)
    adj = {}

    def link(p, q):
        adj.setdefault(p, []).append(q)
        adj.setdefault(q, []).append(p)
    for y in range(-1, h):
        for x in range(-1, w):
            a, b, c, d = v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y + 1)
            T, R, B, L = ('h', x, y), ('v', x + 1, y), ('h', x, y + 1), ('v', x, y)
            s = (a >= .5) | (b >= .5) << 1 | (c >= .5) << 2 | (d >= .5) << 3
            if s in (0, 15):
                continue
            if s in (5, 10):                       # saddle: the centre decides
                if (s == 5) == ((a + b + c + d) / 4 >= .5):
                    link(T, R); link(B, L)
                else:
                    link(T, L); link(B, R)
                continue
            es = [e for e, p, q in ((T, a, b), (R, b, c), (B, d, c), (L, a, d)) if (p >= .5) != (q >= .5)]
            link(es[0], es[1])
    loops, seen = [], set()
    for e in adj:
        loop, prev = [], None
        while e not in seen:
            seen.add(e)
            loop.append(pt(e))
            nxt = [q for q in adj[e] if q != prev and q not in seen] or [q for q in adj[e] if q != prev]
            prev, e = e, nxt[0]
        if loop:
            loops.append(loop)
    return loops


def corners(pts, a, b, tol, out):
    """Douglas-Peucker: indices in pts[a:b] where the outline turns."""
    (x0, y0), (x1, y1) = pts[a], pts[b]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy) or 1e-9
    i, dm = a, -1.0
    for k in range(a + 1, b):
        d = abs(dy * (pts[k][0] - x0) - dx * (pts[k][1] - y0)) / n
        if d > dm:
            i, dm = k, d
    if dm > tol:
        corners(pts, a, i, tol, out)
        corners(pts, i, b, tol, out)
    else:
        out.append(a)


def fit(pts):
    """Least-squares line through pts: [centre, angle, length]."""
    n = len(pts)
    cx, cy = sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n
    sxx = sum((p[0] - cx) ** 2 for p in pts)
    syy = sum((p[1] - cy) ** 2 for p in pts)
    sxy = sum((p[0] - cx) * (p[1] - cy) for p in pts)
    return [(cx, cy), 0.5 * math.atan2(2 * sxy, sxx - syy), math.dist(pts[0], pts[-1])]


def meet(l1, l2):
    (x1, y1), a1, _ = l1
    (x2, y2), a2, _ = l2
    den = math.cos(a1) * math.sin(a2) - math.sin(a1) * math.cos(a2)
    if abs(den) < 0.05:
        return None
    t = ((x2 - x1) * math.sin(a2) - (y2 - y1) * math.cos(a2)) / den
    return (x1 + t * math.cos(a1), y1 + t * math.sin(a1))


def trace(art, bg, fg, up=8, tol=0.25, snap=10):
    """The foreground of a two-tone image as polygons, in source pixels."""
    w, h = art.size
    lum = lambda c: sum(c[:3])
    lb, lf = lum(bg), lum(fg)
    cov = Image.new('L', art.size)
    cov.putdata([max(0, min(255, round((lum(c) - lb) * 255 / (lf - lb)))) for c in pixels(art)])
    px = cov.resize((w * up, h * up), Image.BICUBIC).load()
    f = [[px[x, y] / 255 for x in range(w * up)] for y in range(h * up)]
    outlines = []
    for loop in contours(f, w * up, h * up):
        if len(loop) < 8:
            continue
        pts = [((x + .5) / up, (y + .5) / up) for x, y in loop]
        n = len(pts)
        far = max(range(n), key=lambda k: math.dist(pts[k], pts[0]))
        ring, idx = pts + [pts[0]], []
        corners(ring, 0, far, tol, idx)
        corners(ring, far, n, tol, idx)
        outlines.append([fit(ring[a:b + 1]) for a, b in zip(idx, idx[1:] + [n])])
    # The logo's own directions: level, plus the strongest angles by edge length.
    hist = [0.0] * 180
    for lines in outlines:
        for l in lines:
            hist[round(math.degrees(l[1])) % 180] += l[2]
    hist = [sum(hist[(i + k) % 180] for k in range(-2, 3)) for i in range(180)]
    gap = lambda a, b: min(abs(a - b) % 180, 180 - abs(a - b) % 180)
    dirs = [0]
    for i in sorted(range(180), key=lambda i: -hist[i]):
        if hist[i] < 0.08 * max(hist):
            break
        if all(gap(i, d) > snap for d in dirs):
            dirs.append(i)
    polys = []
    for lines in outlines:
        for l in lines:
            deg = math.degrees(l[1]) % 180
            d = min(dirs, key=lambda d: gap(deg, d))
            if gap(deg, d) <= snap:
                l[1] = math.radians(d)
        # merge neighbours left on the same line, drop slivers
        off = lambda l: -math.sin(l[1]) * l[0][0] + math.cos(l[1]) * l[0][1]
        k = 0
        while k < len(lines) and len(lines) > 3:
            a, b = lines[k - 1], lines[k]
            if abs(a[1] - b[1]) < 1e-6 and abs(off(a) - off(b)) < 0.3:
                s = a[2] + b[2]
                a[0] = ((a[0][0] * a[2] + b[0][0] * b[2]) / s, (a[0][1] * a[2] + b[0][1] * b[2]) / s)
                a[2] = s
            elif b[2] >= 0.35:
                k += 1
                continue
            del lines[k]
            k = 0
        polys.append([meet(lines[k - 1], lines[k]) or lines[k][0] for k in range(len(lines))])
    return polys


def draw(polys, size, bg, fg, fill=0.8, ss=4):
    """Polygons (even-odd) filled with fg on bg, anti-aliased, centred to fill
    that share of the width (the icon's art runs into its own left edge)."""
    xs, ys = [x for p in polys for x, _ in p], [y for p in polys for _, y in p]
    k = size * ss * fill / (max(xs) - min(xs))
    ox = size * ss / 2 - k * (min(xs) + max(xs)) / 2
    oy = size * ss / 2 - k * (min(ys) + max(ys)) / 2
    m = Image.new('1', (size * ss, size * ss), 0)
    for p in polys:
        one = Image.new('1', m.size, 0)
        ImageDraw.Draw(one).polygon([(ox + x * k, oy + y * k) for x, y in p], fill=1)
        m = ImageChops.logical_xor(m, one)
    m = m.convert('L').resize((size, size), Image.BOX)
    return Image.composite(Image.new('RGBA', m.size, fg + (255,)), Image.new('RGBA', m.size, bg + (255,)), m)


def master(art):
    """The 1024x1024 icon: art enlarged to the tile, rounded, with a shadow."""
    tones = two_tone(art) if art.getextrema()[3][0] == 255 and art.width == art.height else None
    if tones:
        tile = draw(trace(art, *tones), TILE, *tones)
    else:
        k = -(-TILE // art.width)                  # integer factor >= tile
        big = art.resize((art.width * k, art.height * k), Image.NEAREST)
        tile = big.resize((TILE, TILE), Image.LANCZOS)
    mask = Image.new('L', (TILE * 4, TILE * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, TILE * 4 - 1, TILE * 4 - 1], radius=RADIUS * 4, fill=255)
    mask = mask.resize((TILE, TILE), Image.LANCZOS)
    tile.putalpha(Image.composite(tile.getchannel('A'), Image.new('L', tile.size, 0), mask))

    off = (CANVAS - TILE) // 2
    shadow = Image.new('L', (CANVAS, CANVAS), 0)
    shadow.paste(mask.point(lambda v: v * 0.45), (off, off + 12))
    shadow = shadow.filter(ImageFilter.GaussianBlur(14))
    img = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    img.putalpha(shadow)
    img.alpha_composite(tile, (off, off))
    return img


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    exe, out = sys.argv[1], sys.argv[2]
    imgs = exe_icons(exe)
    w, h, bpp, raw = max(imgs, key=lambda t: (t[0] * t[1], t[2]))
    art = decode(w, h, bpp, raw)
    img = master(art)
    if len(sys.argv) > 3:
        img.save(sys.argv[3])
    with tempfile.TemporaryDirectory() as td:
        iconset = os.path.join(td, 'AppIcon.iconset')
        os.mkdir(iconset)
        for px in (16, 32, 128, 256, 512):
            img.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, 'icon_%dx%d.png' % (px, px)))
            img.resize((px * 2, px * 2), Image.LANCZOS).save(os.path.join(iconset, 'icon_%dx%d@2x.png' % (px, px)))
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', out], check=True)
    print('icon: %s (from %s: %dx%d, %d-bit, IDI_ICON1)' % (out, os.path.basename(exe), w, h, bpp))


if __name__ == '__main__':
    main()
