#!/usr/bin/env python3
"""Generate the app icon for NFSU2.app.

The game's own NFSU_icon.ico is only 32x32 -- too small for a modern icon (up to
1024), it comes out blocky when scaled up.  Instead we draw the motif as vectors:
a night / neon speedometer.

Usage:  python3 tools/packaging/mkicon.py <output.icns>
"""
import math, subprocess, sys, tempfile, os
from PIL import Image, ImageDraw

S = 1024
SS = 4                      # draw at 4x first, then shrink: anti-aliasing


def squircle_mask(size, r_frac=0.2237):
    """macOS's corner rounding ratio ~0.2237 * edge."""
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=int(size * r_frac), fill=255)
    return m


def vgrad(size, top, bot):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size), Image.BILINEAR)


def arc_gradient(d, box, a0, a1, width, stops, steps=180):
    """An arc whose color changes with the angle: split it into small pieces and draw each separately."""
    for i in range(steps):
        t0, t1 = i / steps, (i + 1) / steps
        t = (t0 + t1) / 2
        # linear transition between color stops
        n = len(stops) - 1
        k = min(int(t * n), n - 1)
        f = t * n - k
        c = tuple(int(stops[k][j] + (stops[k + 1][j] - stops[k][j]) * f) for j in range(3))
        d.arc(box, a0 + (a1 - a0) * t0 - 0.6, a0 + (a1 - a0) * t1 + 0.6,
              fill=c, width=width)


def build():
    n = S * SS
    img = Image.new("RGB", (n, n))
    # background: night asphalt, a light purple at the top
    img.paste(vgrad(n, (34, 30, 48), (10, 10, 14)), (0, 0))
    d = ImageDraw.Draw(img, "RGBA")

    cx = cy = n // 2
    R = int(n * 0.335)
    box = [cx - R, cy - R, cx + R, cy + R]
    A0, A1 = 145, 395                      # an arc running from bottom-left to bottom-right

    # 1) soft neon glow outside the arc (a few wide, transparent passes)
    for w, a in ((int(n * 0.115), 26), (int(n * 0.075), 38), (int(n * 0.05), 55)):
        d.arc(box, A0, A1, fill=(255, 60, 170, a), width=w)

    # 2) dim dial track
    d.arc(box, A0, A1, fill=(255, 255, 255, 26), width=int(n * 0.045))

    # 3) main arc: cyan -> purple -> magenta
    arc_gradient(d, box, A0, A1, int(n * 0.042),
                 [(0, 224, 255), (120, 110, 255), (255, 46, 150)])

    # 4) dial ticks
    for i in range(11):
        a = math.radians(A0 + (A1 - A0) * i / 10)
        r0 = R - int(n * 0.075)
        r1 = R - int(n * 0.032) if i % 5 else R - int(n * 0.012)
        w = int(n * 0.009) if i % 5 else int(n * 0.016)
        col = (255, 255, 255, 150) if i % 5 else (255, 255, 255, 235)
        d.line([cx + r0 * math.cos(a), cy + r0 * math.sin(a),
                cx + r1 * math.cos(a), cy + r1 * math.sin(a)], fill=col, width=w)

    # 5) needle: toward the red zone
    a = math.radians(A0 + (A1 - A0) * 0.78)
    tip = R - int(n * 0.055)
    tail = int(n * 0.055)
    px, py = -math.sin(a), math.cos(a)
    bw = n * 0.026
    d.polygon([(cx + tip * math.cos(a), cy + tip * math.sin(a)),
               (cx - tail * math.cos(a) + px * bw, cy - tail * math.sin(a) + py * bw),
               (cx - tail * math.cos(a) - px * bw, cy - tail * math.sin(a) - py * bw)],
              fill=(255, 255, 255, 245))
    # needle hub
    r = int(n * 0.052)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(18, 16, 24, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 46, 150, 230),
              width=int(n * 0.011))

    # 6) glass glare from the top
    gl = Image.new("L", (n, n), 0)
    ImageDraw.Draw(gl).ellipse([-n * 0.35, -n * 0.95, n * 1.35, n * 0.42], fill=42)
    img = Image.composite(Image.new("RGB", (n, n), (255, 255, 255)), img, gl)

    img = img.resize((S, S), Image.LANCZOS).convert("RGBA")
    img.putalpha(squircle_mask(S))
    return img


def main(out):
    img = build()
    with tempfile.TemporaryDirectory() as td:
        ic = os.path.join(td, "icon.iconset")
        os.mkdir(ic)
        for px in (16, 32, 64, 128, 256, 512, 1024):
            r = img.resize((px, px), Image.LANCZOS)
            if px <= 512:
                r.save(os.path.join(ic, f"icon_{px}x{px}.png"))
            if px >= 32:
                r.save(os.path.join(ic, f"icon_{px//2}x{px//2}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", ic, "-o", out], check=True)
    print(f"icon: {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "AppIcon.icns")
