# Mod support (branch `mods-support`, work in progress)

## What "a mod" means for NFSU2

| Kind | Examples | How it works on Windows | Here |
|---|---|---|---|
| Data | Redux, UG2 SKIN, texture packs, the data half of underground2.net | Replaced/added files in the game folder, installed with *Binary*/*Binarius* | Files are read from the game folder (`runtime/fileio.c`, case-insensitive), so they load. |
| ASI plugin | NFSU2 Unlimiter, Extra Options, HD Reflections, Widescreen Fix | A DLL loaded by an ASI loader that patches the running exe | **Unlimiter: being added** (below). Other plugins: not loaded; the log names them as ignored. |
| Patched exe | no-CD, 4GB patch, IPS patches | A modified `speed2.exe` | Not usable: the engines are lifted from one specific exe. |

underground2.net needs Unlimiter and Extra Options plus its data.

## How an ASI plugin runs here

Both halves of a plugin are lifted ahead of time, like the exe:

1. **The plugin's own code.** `tools/mods/asi_rebase.py` moves the DLL to
   `0x0A000000` (the plugin's preferred `0x10000000` is our CRT heap) using
   its relocation table. `tools/genall.py` lifts it into `gen/ul` with its own
   function table (`FNTAB_PREFIX=ul_`). `PUSHRET=1` makes `push imm; ret` a
   jump: the plugin's code caves return into game functions that way.
   Unlimiter v4.0.0.802: 5,294 functions, 202 unsupported instructions, all in
   CPU-dispatched CRT paths (AVX/SSE4.2) that the plugin is told not to use.
2. **Where caves come back.** `tools/mods/asi_resume.py` finds every game
   address the plugin jumps back to (`push imm; ret`,
   `jmp [DWORD = 0x625401]`). There are 38. They become extra entry points of
   the game engine (`genall.py EXTRA_FUNCS`), so a cave's tail jump runs the
   rest of the game function, the way switch-table targets already do.
3. **The plugin's patches to game code.** These are recorded once from a real
   DllMain run (`ASIREC=<file>`, see below). `tools/mods/asi_sites.py` turns
   every changed range into an xpatch site (`tools/xpatch.py`, kind `w`). The
   NA engine is regenerated with them as switches, off by default. A patched
   `jmp` out of the function is a tail call (`XPATCH_TAILJMP=1`).

At start-up `runtime/asi.c` does what the ASI loader does on Windows:

* finds `scripts\*.asi` and checks the one with lifted code by sha256;
* maps it, relocates it, binds its 102 imports through the Win32 layer, sets
  up its implicit TLS (the CRT's thread-safe statics) for every thread;
* runs DllMain on the guest main thread before the game's entry point, with
  the working directory at `scripts\`;
* compares the game's `.text` with the original. Every changed range must
  match a recorded site byte for byte; matching sites switch on. Anything else
  is reported: `asi: code patch at 0x... NOT HONOURED`.

Data patches (pool sizes, tables, vtables) need nothing: they land in guest
memory, which the lifted code reads.

New Win32 functions for the plugin's CRT (Visual Studio 2022) are in
`runtime/win32_asi.c`: Fls\*, SRW locks (on the real critical-section locks;
the GIL is handed over every 1,024 calls), the W file API handed to the A
versions in `fileio.c`, and the module queries. `GetModuleHandleA("X.asi")` now
says "not loaded" for plugins that are not, so Unlimiter does not think Extra
Options is present. The same file also covers the exe's own late stubs
(`VirtualProtect`, `SetThreadPriority`, `DeleteObject`, `Begin/EndPaint`).

The stock behaviour is unchanged without a plugin: the switches are off, the
extra entry points only add functions, and `NFSU2_ASI=0` skips loading.

## Building

```bash
# plugin code + resume points + sites from a record, NA engine regenerated, all compiled
tools/mods/build_ul.sh <NFSU2Unlimiter.asi> <NA SPEED2.EXE> tools/mods/ul_record.txt
BUILD=na ./link.sh
```

Re-recording, for example after a new Unlimiter version, takes two passes.
First build with no record (`-`), so the sites are off. Then run once with
`ASIREC=<file>` and build again with that file.

The lifter splits a straight-line block at 4,000 instructions and goes on with
the rest as a block of its own (`tools/lift.py`, `discover`). Before, the rest
was never discovered: Unlimiter's static initialiser `sub_0a037170` (44,550
instructions, thousands of `std::string` constructions, no branch) ended after
4,001, the C function fell off its end with the guest stack 0xA500 bytes deep,
and the game faulted at start-up in `sub_00577de0` (`mov al, [esi+0x10]` with
esi = 8). The exe has no such block (longest 1,757 lines in gen/all and gen/na),
so its engines are generated as before.

## Installing Binary / Binarius mods on macOS (modinstall)

Most PC mods for the game's data come with a script for nlgxzef's Binary or its
fork Binarius: a launch file (`install.end`, `*.endlauncher`: `[VERSN1]` + JSON,
or the JSON alone) naming an endscript and the game files it edits. Unlimiter's
data half is one too (`_NFSU2_Unlimiter-Binarius.endlauncher`: 3,430 strings,
296 texture updates, FNG imports, slot types, car parts). Here the same
libraries run without the window and without Wine:

```bash
tools/mods/binarius/build.sh            # once: clones, patches, builds out/binarius/modinstall
cp -cR "<game folder>" <copy>           # the install CHANGES the folder, as Binarius does
tools/mods/ul_install.sh <extracted NFSU2-ULv4.0.0.802> <copy>
out/binarius/modinstall/modinstall <mod>/install.end <copy> [--choose "question=answer"] [--report r.json]
```

The launcher's Mods tab runs it for the player (next section); these commands
are for testing by hand.

`build.sh` clones Endscript, Darius (= Nikki) and CoreExtensions (MIT; Binarius
itself, GPL, only for its key list) at fixed commits into `out/binarius/src`,
applies `tools/mods/binarius/patches/*.patch` and builds
`tools/mods/binarius/modinstall` (the User Mode flow of Binarius' IntroUI, with
the questions answered from the command line), plus a self-contained single-file
copy for the app (`out/binarius/modinstall-app`, .NET inside, ~36 MB, with the
three MIT licence texts). Nothing third-party is in the repo.

Questions (comboboxes, checkboxes) take `--choose`, else the script's first
option / "yes". One rule on top: when a lookup in the game's data fails
("Collection named X does not exist") inside the branch of an option nobody
chose, the other options of that question are tried and the one that fits is
kept. Carbon 4K needs it: its "Default" names a texture pack `DEFAULT`, which no
tool finds in the stock GLOBALB (internal name `GLOBAL`, file name
`GlobalTextures.tpk`, which Binarius uses); its "Unlimiter" option names that
one. Other errors (a file of the mod missing) never cause a retry, so a
"New Installation" is never swapped for an "Upgrade".

The patches, macOS ones first:

* **Compression.** Nikki calls LZCompressLib.dll (Windows). JDLZ and RAWW are
  in C# now (byte-identical to the game's own decoder on FrontB and GlobalB);
  HUFF (6 FNGs in FrontB, textures in CARS/TEXTURES.BIN) goes to
  `out/binarius/libhuff.dylib`: the game's own decoder, `sub_00573840`, taken
  from the lifted NA engine (`gen/na`, so it stays in `out/`). Writes are RAWW,
  which the game reads too.
* **Paths.** The scripts are rewritten with `/` in a temporary copy;
  `unlock_memory`/`speedreflect` use `Path.Combine`; a TPK's file name in the
  data (`Global\Pipeline\FrontEndTextures.tpk`) is split on `\`; the car
  geometry paths Nikki writes keep the game's `\`.
* **Mip levels.** A .dds with more than 8 levels keeps its 8 largest: the
  game uploads at most 8 (the most any of its own textures has). Carbon 4K's
  4096-pixel CARBONFIBRE brings 13; levels 9-13 were never filled
  (`check.sh`: "missing mip levels = 1"). The layout is told from the size
  (DXT1, DXT3/5, 4/2/1 bytes a pixel); a size that fits none is left alone.
* **Culture.** `modinstall` runs in en-US, as Binarius does. With a Turkish
  locale `ToUpper("i")` is "İ" and decimals parse differently: GlobalB came
  out different.
* **Data Nikki lost on every save** (the same on Windows, found by a no-op
  load/save of GlobalB and a chunk-by-chunk diff):
  - `Stage` (career race stage): only bit 0 of the direction byte was kept.
    Bit 1 marks the races whose tracks Quick Race offers from the start;
    without it every Quick Race track was locked.
  - `GCareerRace`: the 4 bytes of the unlock condition (the stage byte of
    every condition, all of AtStageStart) were written as zeros.
  - `CarTypeInfo`: the street/pro engine and street turbo block
    (0x3E0-0x450) and the street/pro RPM refresh slots were replaced by
    TOP / 3 and TOP * 2 / 3: every car's engine upgrades changed.
  Kept as Nikki does it (judged harmless): text after the terminator of fixed
  string fields (EA tool garbage), opponents beyond NumOpponents, the second
  copy of track 4400 in the track table, the 0x16 bytes Nikki skips in
  AcidEffects/AcidEmitters.

Answers given: New Installation, Enhanced paint groups and rim brands, fix the
wheel textures, no tire mod (`UL_CHOOSE` overrides, e.g. "rim brands=UG2.net").
The one error left is `speedreflect auto`: SpeedReflect.asi is Binarius' own
reflection plugin and is not used here.

Checked (2026-11-07, NA engine, scripted with AUTOSEQ, no profile): on a fresh
install the stock engine and the Unlimiter engine both reach the main menu,
Quick Race track select (Resort Loop open), car select and a running race
(lap 1/3), no faults. A no-op load/save of GlobalB gives every car and career
record back byte for byte except the cases listed above as harmless.
Test pitfall: install into a fresh copy every time; running the script twice
on one folder is not what Binarius users do and gave a locked track list.

Checked again (2026-11-08, after bringing the branch up to main 15ba922):

* `./run.sh test`: ISA tests 474/474, function tests 15/15.
* `tools/check.sh` on the EU engines built from this branch (plugin support
  linked in, no plugin): standard and HD, all checks pass. The plugin code
  does not change the plain game.
* The same checks on the NA engines (a copy of check.sh that runs
  `out/nfsu2_na_boot` / `out/nfsu2_na_hd_boot`): NA data without a plugin,
  NA + Unlimiter standard and NA + Unlimiter HD, all pass, including
  "missing mip levels = 0" (Unlimiter's 296 texture updates stay within the
  8 levels the engine uploads). The plugin log says 307 of 308 patches on,
  1 data, 0 not honoured.
* Unlimiter's features in the menus, A/B against the plain NA engine on the
  same key sequence (Customize, Peugeot 206, Body Shop, Rims): entering Rims
  asks "Which wheels would you like to customize? Front / Rear / All" (the
  plugin's split front/rear rims); the brand list starts with Stock and Same
  As Rear and has Advan and Oasis (the "Enhanced rim brands" install choice),
  with their logos from the installed data. The Body Shop categories are the
  same as stock for the 206; Unlimiter adds categories per car through
  `UnlimiterData/<CAR>.ini`, which the release does not ship (the plugin asks
  for them and falls back to its defaults, as it does on Windows).

## Settings › Mods in the launcher

`launcher/Mods.swift`, the Mods tab of the Settings window (`Settings.swift`):

* **NFSU2 Unlimiter** has a card of its own: Download (the 4.0.0.802 release
  from GitHub, 4.6 MB, checked against its SHA-256) or Add File…, then a switch
  and the questions its script asked (paint groups, rim brands, wheel texture
  fix, tire mod; defaults as its Read Me recommends; "New Installation" is
  fixed, the launcher always installs onto an untouched copy). It works with
  the North American v1.2 game only; with another the row says so.
* **Add Mod…** takes any mod's archive (zip, 7z, rar: macOS's bsdtar) or folder.
  A launch file makes it a script mod; otherwise folders such as TRACKS, GLOBAL,
  CARS make it a plain-file mod (only those folders are used, a Readme next to
  them stays out). A mod with a symbolic link anywhere is refused, and so is a
  mod with a plugin (.asi) the engines have no code for ("plugins are Windows
  code and each one has to be translated for the Mac first"). Each mod gets a
  switch and the questions its script asked, with the answers used; another
  answer installs it again. Right click: Show in Finder, Remove.
* **Applying** (on every change, and before Play when out of date): the game
  folder is cloned into a temporary folder on its own volume
  (`.itemReplacementDirectory`; APFS: instant, no space), Unlimiter is
  installed into the clone first, then the other mods in the order they were
  added (a script mod from a fresh copy of itself, since scripts move their
  files out), and the files whose size or date differ from the game's move to
  `~/Library/Application Support/NFSU2/Mods/Files`; game files no mod left are
  listed in `Files/.deleted`. A stamp (the mods, their answers, the game's
  SPEED2.EXE, the installer) says whether Files is up to date. All three
  nfsmods.xyz mods plus Unlimiter take ~13 s. A mod that fails is turned off
  again with the reason in its row, and the others are applied without it.
* **Off** is the original game: no mod files, `NFSU2_MODS` not set.
  Restore Original Game turns every mod off.
* Test hooks (no window, the same code): `NFSU2_LAUNCHER_TEST=mods:<game
  folder>:add=<file>;ul-download;on=<id>;off=<id>;choose=<id>|<question>|<answer>;remove=<id>;restore`,
  `NFSU2_MODS_STORE`, `NFSU2_MODINSTALL`, `NFSU2_UL_URL` (main.swift).

The engine side (`runtime/fileio.c`): `NFSU2_MODS=<folder>` is a read layer
between the save folder and the game folder. A file there is read instead of
the game's (each logged once: `[file] mod: TRACKS/STREAML4RA.BUN`); a folder
only the mods have (scripts\ for Unlimiter) is taken from there; a folder
both have is listed from both (FindFirstFile, three layers, no name twice);
paths in `.deleted` are not there. Nothing is ever written to it.
`runtime/asi.c` looks the plugin up as a file through these layers, so
Unlimiter loads from the mod files even when the game folder has a scripts\
folder of its own.

`pack.sh` puts the installer in `Contents/Resources/modinstall` and signs it
(with a real certificate: hardened runtime plus the JIT entitlements .NET
needs).

PR #41 (007hacky007) did the same for three mods with its own Swift subset of
Binary's commands; it could not install Unlimiter or mods that need it. This
tab replaces it with Binarius' own libraries; the read layer is the PR's idea,
written again here.

Checked (2026-11-08, three mods from nfsmods.xyz: Detailed Map, Upscaled
Freeway Signs, Carbon 4K, SHA-256 as in PR #41; Unlimiter 802):

* Launcher hooks: adding all three, then Unlimiter by Download (from a
  file:// copy), gives 121 / 510 changed files, nothing removed; off, on,
  Restore Original Game, another answer to a question (rim brands Vanilla and
  back) each rebuild; an Unlimiter added on the European game stays out of the
  files. Carbon 4K's question resolves to "Unlimiter" by itself.
* `check.sh` (a copy passing `NFSU2_MODS`): NA + Unlimiter + the three mods,
  standard and HD; EU + the three mods, standard and HD: all pass (54 checks),
  after the mip cap. The engine reads 7 files from the layer on EU, 381 with
  Unlimiter; the plugin loads from the layer (307 of 308 patches on).
* In the game: the same Quick Race moment with the mods off and on, Detailed
  Map's minimap (sharper roads, lanes); Unlimiter from the layer: the
  front/rear rims question, Stock, Advan.
* The single-file installer runs with no .NET on the machine.

## Limits and open points

* One recorded configuration. The ranges come from DllMain with the settings
  that were present when recording. A setting that patches different code is
  reported as not honoured until it is recorded too.
* One plugin build: another Unlimiter version needs its own lift (v4.0.0.803
  came out on 2026-11-04; this is .802).
* The test data set is the NA v1.2 archive plus the 35 traffic files it lacks,
  from the US discs; the plugin addresses are NA v1.2's.
* Seen in the menus: split front/rear rims, Stock / Same As Rear, Advan and
  Oasis. Not yet seen: per-car extra part categories (need per-car .ini files
  or add-on cars). Extra Options and underground2.net are not lifted yet.
* Mods tab: the order of mods is the order they were added (no reordering);
  Make Copy (portable app) doesn't take the mods along; the Carbon 4K texture
  on a car was not looked at (carbon parts are locked on a fresh profile; the
  mip check passed).
