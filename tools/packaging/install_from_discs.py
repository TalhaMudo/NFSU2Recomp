#!/usr/bin/env python3
"""Install NFSU2 from its PC discs on a Mac, without running the Windows installer.

The PC discs hold the game in plain zip files (compressed.zip, with Windows
backslash paths) plus a few loose folders, so the installer is not needed:

    python3 tools/packaging/install_from_discs.py "<output folder>" <disc 1> <disc 2>

Each disc can be a .cue, a raw .bin (2352-byte sectors), an .iso, or a folder
(a mounted or copied disc).  Needs 7-Zip (`brew install sevenzip`) for .bin/.iso
images.  The result is a normal game folder; the launcher still checks that its
SPEED2.EXE is the supported build (see README, "Requirements").
"""
import os, posixpath, shutil, subprocess, sys, tempfile, zipfile

# Disc content that belongs to the installer or the copy protection, not the game.
SKIP_TOP = {'autorun', 'autorun.exe', 'autorun.inf', 'autorungui.dll', 'bin.dat',
            'common_filelist.txt', 'compressed.zip', 'directx', 'eauninstall.exe',
            'rungame.exe', 'setup.exe', 'support'}
SKIP_EXT = ('.tmp', '.016', '.256')


def bin_to_iso(src, dst):
    """Raw MODE1/2352 image -> plain 2048-byte-sector ISO."""
    if os.path.getsize(src) % 2352:
        raise SystemExit(f"{src}: not a raw 2352-byte-sector image")
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        while True:
            s = f.read(2352 * 512)
            if not s:
                break
            for i in range(0, len(s), 2352):
                o.write(s[i + 16:i + 16 + 2048])


def cue_bin(cue):
    with open(cue, errors='replace') as f:
        for line in f:
            if line.strip().upper().startswith('FILE'):
                name = line.split('"')[1] if '"' in line else line.split()[1]
                return os.path.join(os.path.dirname(cue), name)
    raise SystemExit(f"{cue}: no FILE line")


def disc_folder(path, work):
    """Return a folder with the disc's files."""
    if os.path.isdir(path):
        return path
    low = path.lower()
    if low.endswith('.cue'):
        path, low = cue_bin(path), cue_bin(path).lower()
    if low.endswith('.bin'):
        iso = os.path.join(work, 'disc.iso')
        print(f"  converting {os.path.basename(path)} to ISO")
        bin_to_iso(path, iso)
        path = iso
    seven = shutil.which('7zz') or shutil.which('7z')
    if not seven:
        raise SystemExit("7-Zip is needed to read disc images: brew install sevenzip")
    out = os.path.join(work, 'files')
    subprocess.run([seven, 'x', '-y', '-bso0', '-bsp0', f'-o{out}', path], check=True)
    return out


def unzip_backslash(zpath, out):
    """Extract a zip whose member names use Windows backslashes."""
    n = 0
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            name = posixpath.normpath(info.filename.replace('\\', '/'))
            if name.startswith(('/', '..')):
                raise SystemExit(f"{zpath}: unsafe path {info.filename}")
            dst = os.path.join(out, name)
            if info.filename.endswith(('\\', '/')):
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(info) as src, open(dst, 'wb') as f:
                shutil.copyfileobj(src, f, 1 << 20)
            n += 1
    return n


def install_disc(folder, out):
    for entry in sorted(os.listdir(folder)):
        src = os.path.join(folder, entry)
        low = entry.lower()
        if low == 'compressed.zip':
            print(f"  compressed.zip: {unzip_backslash(src, out)} files")
        elif low in SKIP_TOP or low.endswith(SKIP_EXT) or low.startswith('.'):
            continue
        elif os.path.isdir(src):
            shutil.copytree(src, os.path.join(out, entry), dirs_exist_ok=True)
            print(f"  {entry}/")
        else:
            dst = 'SPEED2.EXE' if low == 'speed2.exe' else entry
            shutil.copy2(src, os.path.join(out, dst))
            print(f"  {dst}")


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        sys.exit(1)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    for i, disc in enumerate(sys.argv[2:], 1):
        print(f"disc {i}: {disc}")
        with tempfile.TemporaryDirectory(dir=out, prefix='.disc') as work:
            install_disc(disc_folder(os.path.abspath(disc), work), out)
    exe = os.path.join(out, 'SPEED2.EXE')
    print(f"\ninstalled to {out}")
    if not os.path.exists(exe):
        print("no SPEED2.EXE found on the discs")
    elif os.path.getsize(exe) == 5987981:
        print("SPEED2.EXE is the US v1.0 retail executable. It is SafeDisc-protected (its code\n"
              "is encrypted) and cannot be recompiled; the game data is usable. See README.")


if __name__ == '__main__':
    main()
