#!/usr/bin/env python3
"""Build hudhd.pak: high-resolution HUD and menu textures, keyed by the game's own
texture name hash, for the engine's "Sharper HUD and menus" option (runtime/hudhd.c).

    tools/hudhd_pack.py <source> <out.pak>

<source>: LunicAura106's NFSU2 Upscaled HUD (nfsmods.xyz/mod/4319), the .7z as
released or a folder it was unpacked into.  Its textures are DDS files named after
the game's textures (FRONTB/FRONTENDTEXTURES/CARSELECT_MANUFACTURER_HONDA.dds); the
name's bStringHash (h = h*33 + c from 0xFFFFFFFF) is the hash the game keeps in each
texture's info (+0x24) when it creates the texture, so no matching by content is
needed.  The optional Unlimiter part textures (UnlimiterTextures/) are left out.

Pak layout (little endian):
    "NHHD"  u32 version (1)  u32 count  u32 0
    count x { u32 hash, u32 d3dfmt, u16 w, u16 h, u16 levels, u16 0,
              u32 offset, u32 packed size, u32 size }       sorted by hash
    data: per texture, zlib of every mip level in turn, each tightly packed
"""
import os, struct, subprocess, sys, tempfile, zlib

FOURCC = {b'DXT1': (0x31545844, 8), b'DXT3': (0x33545844, 16), b'DXT5': (0x35545844, 16)}
SKIP_DIRS = {'unlimitertextures', 'scripts'}


def bstring_hash(name):
    h = 0xFFFFFFFF
    for ch in name.encode('ascii'):
        h = (h * 33 + ch) & 0xFFFFFFFF
    return h


def read_dds(path):
    """(d3dfmt, w, h, levels, data of every level) or a reason it is not taken."""
    b = open(path, 'rb').read()
    if len(b) < 128 or b[:4] != b'DDS ':
        return 'not a DDS file'
    h, w = struct.unpack_from('<II', b, 12)
    levels = max(1, struct.unpack_from('<I', b, 28)[0])
    pf_flags = struct.unpack_from('<I', b, 80)[0]
    fcc = b[84:88]
    if not (pf_flags & 4) or fcc not in FOURCC:
        return 'format %r is not DXT1/3/5' % fcc
    d3dfmt, bpb = FOURCC[fcc]
    size, lw, lh = 0, w, h
    for _ in range(levels):
        size += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bpb
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    data = b[128:128 + size]
    if len(data) != size:
        return 'truncated (%d of %d bytes)' % (len(data), size)
    return d3dfmt, w, h, levels, data


def collect(root):
    tex, notes = {}, []
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x.lower() not in SKIP_DIRS)
        for f in sorted(files):
            if not f.lower().endswith('.dds'):
                continue
            name = os.path.splitext(f)[0].upper()
            r = read_dds(os.path.join(d, f))
            if isinstance(r, str):
                notes.append('%s: %s' % (os.path.join(d, f), r)); continue
            hs = bstring_hash(name)
            if hs in tex and tex[hs][1] != r:
                notes.append('%s: another texture of this name was taken first' % name); continue
            tex[hs] = (name, r)
    return tex, notes


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as tmp:
        if os.path.isfile(src):
            seven = next((p for p in ('/opt/homebrew/bin/7zz', '/usr/local/bin/7zz', '/opt/homebrew/bin/7z')
                          if os.path.exists(p)), None)
            cmd = [seven, 'x', '-y', '-o' + tmp, src] if seven else ['bsdtar', '-xf', src, '-C', tmp]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            src = tmp
        tex, notes = collect(src)
        if not tex:
            sys.exit('no textures found in ' + sys.argv[1])
        entries, blob = [], bytearray()
        for hs in sorted(tex):
            name, (fmt, w, h, levels, data) = tex[hs]
            z = zlib.compress(data, 9)
            entries.append(struct.pack('<IIHHHHIII', hs, fmt, w, h, levels, 0, len(blob), len(z), len(data)))
            blob += z
        head = struct.pack('<4sIII', b'NHHD', 1, len(entries), 0)
        base = len(head) + 28 * len(entries)
        entries = [e[:16] + struct.pack('<I', struct.unpack_from('<I', e, 16)[0] + base) + e[20:] for e in entries]
        tmp_out = out + '.partial'
        with open(tmp_out, 'wb') as f:
            f.write(head); f.write(b''.join(entries)); f.write(blob)
        os.replace(tmp_out, out)
    raw = sum(len(r[4]) for _, r in tex.values())
    print('hudhd.pak: %d textures, %.1f MB of texture data in %.1f MB' %
          (len(tex), raw / 1e6, os.path.getsize(out) / 1e6))
    for n in notes:
        print('  skipped ' + n)


if __name__ == '__main__':
    main()
