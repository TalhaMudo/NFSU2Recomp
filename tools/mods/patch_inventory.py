#!/usr/bin/env python3
"""Inventory of the in-process patches an ASI mod (e.g. NFSU2 Unlimiter) applies.

An ASI plugin patches the running x86 exe: `injector::WriteMemory`, `MakeCALL`,
`MakeJMP`, `MakeNOP`, `MakeRangedNOP`.  Our engines are statically recompiled, so
a patch only takes effect on data; a patch inside code needs an explicit
equivalent.  This script finds every patch site in the plugin's source and sorts
it by what it would take to honour it here.

    patch_inventory.py <plugin-source-dir> <gen/na/fntab.c> [--json out.json]

The addresses a mod uses are those of the exe it targets.  Unlimiter and the
other mods target the North American v1.2 exe (4,800,512 bytes), so `fntab.c`
must come from the NA engine (`gen/na`), not the European one.

Read-only: it never writes inside the plugin source or the engine folders.
"""
import bisect, collections, json, pathlib, re, sys

KINDS = ("WriteMemory", "ReadMemory", "MakeCALL", "MakeJMP", "MakeNOP", "MakeRangedNOP")
CALL_RE = re.compile(r"injector::(%s)\s*(?:<([^>]+)>)?\s*\(([^;]*?)\)\s*;(?:[ \t]*//([^\n]*))?" % "|".join(KINDS))
ADDR_RE = re.compile(r"^\s*(0x[0-9A-Fa-f]+)\b")

# EU layout; NA is within a few KB of these, enough to tell code from data.
TEXT_END = 0x782000
RDATA_END = 0x7E8000


def load_functions(fntab):
    text = pathlib.Path(fntab).read_text()
    block = text.split("g_fn_addr[] = {", 1)[1].split("};", 1)[0]
    return sorted(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)u", block))


def region(a):
    if a < 0x401000:
        return "below-image"
    if a < TEXT_END:
        return "code"
    if a < RDATA_END:
        return "rdata"
    return "data"


def scan(src):
    sites = []
    for p in sorted(pathlib.Path(src).glob("*.h")) + sorted(pathlib.Path(src).glob("*.cpp")):
        text = p.read_text(errors="replace")
        for m in CALL_RE.finditer(text):
            kind, width, args, note = m.groups()
            am = ADDR_RE.match(args)
            line = text.count("\n", 0, m.start()) + 1
            sites.append({
                "file": p.name, "line": line, "kind": kind, "width": (width or "").strip(),
                "addr": int(am.group(1), 16) if am else None,
                "args": " ".join(args.split())[:120], "note": (note or "").strip(),
            })
    return sites


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sites = scan(sys.argv[1])
    funcs = load_functions(sys.argv[2])
    out = None
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]

    for s in sites:
        a = s["addr"]
        if a is None:
            s["region"] = "unresolved"; s["fn"] = None
            continue
        s["region"] = region(a)
        if s["region"] == "code":
            i = bisect.bisect_right(funcs, a) - 1
            s["fn"] = funcs[i] if i >= 0 else None
        else:
            s["fn"] = None

    by = collections.Counter((s["kind"], s["region"]) for s in sites)
    print("patch sites: %d in %d files" % (len(sites), len({s["file"] for s in sites})))
    print("\n%-14s %-12s %5s" % ("kind", "region", "sites"))
    for (k, r), n in sorted(by.items()):
        print("%-14s %-12s %5d" % (k, r, n))

    code = [s for s in sites if s["region"] == "code" and s["kind"] != "ReadMemory"]
    fns = {s["fn"] for s in code}
    print("\nwrites into code: %d sites in %d distinct lifted functions" % (len(code), len(fns)))
    data = [s for s in sites if s["region"] in ("rdata", "data") and s["kind"] == "WriteMemory"]
    print("writes to data:   %d sites (honoured by writing guest memory before start)" % len(data))
    print("unresolved addr:  %d sites (address is an expression; needs reading by hand)"
          % sum(1 for s in sites if s["region"] == "unresolved"))

    per_file = collections.Counter(s["file"] for s in code)
    print("\ncode patches per file:")
    for f, n in per_file.most_common(12):
        print("  %-28s %4d" % (f, n))
    if out:
        pathlib.Path(out).write_text(json.dumps(sites, indent=1))
        print("\nwrote", out)


if __name__ == "__main__":
    main()
