# NFSU2 → macOS ARM64 Native Port (static recompilation)

A project that lifts the machine code of `SPEED2.EXE` (32-bit x86 Windows PE) to
C and recompiles it as a **native ARM64 Mach-O**. No Wine, no emulation, no
Windows.

Method: the static recompilation technique used in the N64Recomp (Zelda MM) and
XenonRecomp (Sonic Unleashed) ports, adapted for x86.

> **This document is a chronological engineering log.** The current summary,
> build recipe and related work are in `README.md`. Numbers in older sections
> (test counts, trap counts, source size) reflect the state on the day they
> were written. Current values (2026-10-12): 23,278 functions, 132.5 MB of
> generated C, 400 static traps, 169 ISA tests / 439 checks.

## Target binary

| | |
|---|---|
| SHA-256 | `8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297` |
| Format | PE32, Intel 80386, GUI, ImageBase `0x400000` |
| Entry | `0x0075bcc7` |
| `.text` | 3,676,913 bytes (~1.13 M instructions, 337 distinct opcodes) |
| Packed/protected | **No** — the import table is complete and readable |

### Key discovery: the relocation table is intact
The PE's `BASERELOC` directory entry is zeroed and the `RELOCS_STRIPPED` flag
is set (typical for no-CD exes), **but the data is still physically there** — in
the unnamed 5th section (`0x8f5000`, 240,664 bytes). Parsed by hand:

* 999 blocks, 91,616 entries, **91,106 HIGHLOW absolute addresses**
* Validation: **91,106/91,106 point to a valid image address (100%)**
* Distribution: `.text` 71,094 · `.rdata` 14,601 · `.data` 5,411

This solves the biggest problem of x86 static recompilation: *which 4 bytes are
a pointer and which are a number?* RISC ISAs (MIPS/PPC) don't have this problem
thanks to fixed 4-byte instructions; with x86's variable-length (1–15 byte)
instructions an exact solution is undecidable in general. The reloc table gets
around it.

## Measured state

| Metric | Value |
|---|---|
| `.text` code coverage (reloc-assisted recursive descent) | **96.08%** |
| Total explained (code + alignment + jump tables + data) | **98.71%** |
| True uncertainty | **1.29%** (47,451 bytes / 1,546 blocks) |
| Functions found | **23,274** |
| **Fully lifted functions** | **23,046 / 23,274 = 99.0%** |
| **Lift coverage by instruction** | **98.5%** |
| Unsupported-instruction traps (static) | 914 (initially 30,783) |
| Indirect call sites (`call reg/[mem]`) | 6,279 |
| Indirect jumps | 763 (446 jump tables resolved) |

### Instruction mix
`mov` 27.6% · `push` 13.6% · `call` 6.1% · **x87 9.86%** · SSE/MMX 1.25%

## Runtime architecture

**Memory:** the guest 32-bit address space is reserved with `mmap`; guest
address → host address translation uses ARM64's `uxtw` addressing mode, **in a
single instruction, at zero extra cost**:

```asm
ldr w8, [x_base, w_addr, uxtw]     ; MR32(guest_addr)
```

> Note: mapping the guest directly at its own address (`0x400000`) was tried —
> even with `__PAGEZERO` shrunk to 0x4000, macOS ARM64 SIGKILLs the process.
> Since the base+offset approach is free anyway, this is no loss.

**CPU state:** `runtime/cpu.h` — 8 GPRs, flags in separate `uint32_t` fields
(clang's DCE eliminates flag computations not used within a function), an
8-entry x87 stack, 8 XMM registers.

## Files

```
tools/analyze.py    PE sections, recursive descent, coverage
tools/reloc2.py     parse the zeroed reloc table by hand  -> out/relocs.txt
tools/profile.py    instruction histogram, x87/SSE ratio
tools/lift.py       x86-32 -> C lifter (core)
tools/liftall.py    try all 23k functions, report missing instructions
runtime/cpu.h       CPU state, memory macros, flag/x87/string helpers
runtime/mem.c       loads PE sections into guest memory
tests/harness.c     integer tests (22 tests)
tests/fptest.c      x87 tests (30 tests)
```

Setup: `python3 -m venv .venv && PIP_USER=0 .venv/bin/pip install capstone pefile`

```bash
./.venv/bin/python tools/lift.py <SPEED2.EXE> 48b1e0 631d30 > gen/funcs.c
clang -O2 -Iruntime -o out/test tests/harness.c gen/funcs.c runtime/mem.c -lm
```

## Verified (52/52 tests pass, native arm64)

* `sub_0048b1e0` — array indexing, `imul`+`add`, 6 cases
* `sub_0075dc4b` — `_finite(double)`; **bit-exact** against C `isfinite()`
  (8 cases including inf, -inf, nan, subnormal)
* `sub_00631d30` — 6-block CFG, conditional branches, `ret 8` stdcall cleanup,
  sub-function call, stack balance
* `sub_0077b970` / `sub_0077b950` — `fld` + 3× `fadd`/`fsub` + `fstp`,
  with the real constants from the image, float result bit-exact
* `sub_006e1c00` — `fild` int→float, 7 cases including `INT32_MIN`/`INT32_MAX`
* `sub_0040ffd0` — float copy, inf/nan roundtrip


## Phase 2 results: indirect dispatch + full build

### Indirect call dispatch
A direct index table the size of `.text` (14 MB of `uint32`), guest address →
native function pointer, **O(1)**. 23,278 functions registered, no collisions or
out-of-range entries. Verified: the same function gives **exactly the same
result** whether called directly or dispatched via its guest address. Unresolved
indirect calls in the boot run: **0**.

### Full build
| | |
|---|---|
| Functions generated | **23,278** |
| C source | **115.6 MB** (128 shards) |
| Generation time | 21 s (10 cores) |
| Compilation | **130/130 shards, error-free**, 66 s (571 CPU-s, `-O2`) |
| Link | 43 MB **Mach-O 64-bit arm64** |
| Truncated branch targets | 0 |
| Missing symbols (stubs) | 4 |
| Unsupported-instruction traps | 30,783 (MMX/SSE/3DNow only) |

Three lifter bugs found and fixed during generation:
1. `call` outside `.text` → nonexistent `sub_XXXX` symbol (now goes to dispatch)
2. Conditional branch outside `.text` → nonexistent label (now a trap)
3. Branch targets not generated because of truncation → stub label

### fs:/gs: segment support (correctness fix)
A static scan showed the `fs:` prefix used **3,698 times** (3,613 of them `mov`)
— the SEH chain `fs:[0]` and TLS `fs:[0x2c]`. The lifter was silently ignoring
them, so `mov eax, fs:[0]` was translated wrongly. Now `c->fs_base` is added.
`es:/ds:/ss:` keep being ignored since their base is 0 in the flat memory model
(all 4,097 `es:` accesses are the implicit segment of string instructions).

### Guest memory model
The 4 GB address space is reserved as `PROT_NONE` and the regions in use are
committed. This way a bad guest access is diagnosed as `SIGSEGV` → guest
address instead of crashing the host. During boot a stack overflow was caught at
exactly `0x00cfffec` (20 bytes below the stack limit).

## Phase 3 seed: Win32 layer and boot exploration

252 IAT entries, 242 of them called, **613 call sites**:
KERNEL32 469 · USER32 62 · WS2_32 22 · ADVAPI32 17 · GDI32 14 · WINMM 11 · TAPI32 9

**stdcall argument counts:** inferring them from `push` sequences at call sites
was tried — only **74.9% correct** against 183 known functions. That is not good
enough, since every wrong count shifts `esp` and corrupts the stack; an
authoritative table was entered by hand for 183 functions (`tools/argcount.py`),
the remaining ~59 use the inference.

**Boot state:** starting from the entry point `0x0075bcc7`, the MSVC CRT startup
runs correctly up to a depth of **73 Win32 calls**:

```
GetVersionExA → HeapCreate → 17× InitializeCriticalSection → TlsAlloc/TlsSetValue
→ GetStartupInfoA → stdio handles → GetCommandLineA → environment variables
→ GetACP/GetCPInfo → GetStringTypeW → MultiByteToWideChar → LCMapStringW
→ WideCharToMultiByte   (locale/codepage initialization COMPLETE)
```

Implemented: heap (bump allocator), TLS, critical sections, time
(`QueryPerformanceCounter`, `GetTickCount`), `GetStringTypeW` (C1_* flags),
`LCMapStringW/A` (upper/lower case), `GetLocaleInfoA/W`, `MultiByteToWideChar`,
`WideCharToMultiByte`, `GetProcAddress` (resolution by name), `MessageBoxA`.

**Current blocker:** the CRT `/GS` stack security cookie check fails and the
game shows its own error box:

> *Buffer overrun detected! — Microsoft Visual C++ Runtime Library*

Diagnosed so far:
* The cookie global is at **guest `0x81a1d0`**, value `0xBB40E64E` (the MSVC
  link-time default) — it never changes during the run, so
  `__security_init_cookie` did not run. Since the global is constant, the
  inconsistency is in the **stack copy**.
* `__security_check_cookie` is at `0x76115e`; `__report_gsfailure` is at
  `0x76112d` and has **a single reference** (the `jmp` at `0x761167`).
* **All** 27 imports on the boot path have authoritative argument counts, so the
  source of the corruption is **not** wrong stdcall cleanup — it is in the lifted
  code itself.

### Structural finding: basic blocks are duplicated across functions
When a function passes to another function with `jmp` (tail call), the lifter
copies the target blocks into the caller's body. The `__security_check_cookie`
blocks exist separately in both `sub_0076115e` and `sub_00743cd0`. Semantically
correct, but **symbol-based intervention cannot catch every path** — this has to
be taken into account when debugging and when patching functions later.

### New tools
```
tools/genall.py     generate 23k functions in parallel, split into shards, generate fntab
tools/imports.py    extract the IAT, count call sites per import
tools/argcount.py   stdcall argument counts (authoritative table + inference)
tools/disasm.py     disassemble an address range
runtime/dispatch.c  O(1) indirect call table + guest stack tracer
runtime/win32.c     Win32 API layer (boot subset)
build.sh            parallel shard compilation
tests/main_all.c    full-build verification suite (15 tests)
tests/boot.c        entry point execution + tracing
```

```bash
./.venv/bin/python tools/genall.py <SPEED2.EXE> 128 && OPT=-O2 ./build.sh
```

### Next steps
1. Find the source of the `/GS` cookie corruption — most likely a bug in the
   lifter's flag/stack semantics. Suspects: the `inc`/`dec` CF-preservation
   trick, shift flags, `rep` string instructions, 16/8-bit partial register
   writes. Method: write a tracer (watchpoint) that steps through guest
   execution and finds the instruction at which the stack cookie changes.
2. Add MMX/SSE/3DNow → bring the 30,783 traps to zero.
3. Authoritative argument counts for the remaining ~59 imports.

## Roadmap

**Phase 1 — complete the lifter** *(done)*
- [x] Integer core, flags, CFG, `call`/`ret`
- [x] x87 FPU stack + 80-bit `tbyte` conversion
- [x] String instructions (`rep movs/stos/lods/scas/cmps`, `df` direction)
- [x] MMX (packed integer, saturation, shifts, punpck, pack, pcmpeq/gt, pmaddwd, pmulhw)
- [x] SSE/SSE2 (movaps/movups/movdqa, movss/movsd, add/sub/mul/div ps+ss+sd,
      shufps, unpck, sqrt/max/min, cvt*, ucomis*, movhlps/movlhps)
- [x] 3DNow! (pfadd/pfsub/pfmul/pfcmp*/pi2fd/pf2id)
- [x] `pushal`/`popal`, `rcl`/`rcr`, `bt` family, `pushfd`/`popfd`, `sahf`/`lahf`
- [x] Segment registers (fixed selector in the flat model), privileged instructions (no-ops)
- [ ] Remaining 1.5%: some operand forms of `pandn`, `packssdw`, `movq2dq` (9 functions)

**Phase 2 — runnability**
- [x] Indirect call dispatch table (O(1), 23,278 functions, verified)
- [ ] SEH (MSVC `__try/__except`) analysis and support
- [x] fs:/gs: segment base (TEB/TLS)
- [ ] Threads → pthreads
- [x] Generate + compile + link all 23k functions (43 MB arm64 Mach-O, 66 s)

**Phase 3 — Win32 layer** *(230 imports)*
- [ ] KERNEL32 (152) — file I/O → POSIX, virtual memory, threads, TLS, time
- [ ] USER32 (38) — window/message loop → Cocoa or SDL3
- [ ] GDI32 (12), ADVAPI32 (6, registry → plist), WINMM (6)
- [ ] WS2_32 → BSD sockets · SHELL32/SHFOLDER → path mapping
- [ ] TAPI32 (9) / NETAPI32 (1) → most likely stubs

**Phase 4 — COM/graphics** *(the single biggest item)*
- [ ] **D3D9 → Metal**: `Direct3DCreate9` is the only import, the rest goes through vtables.
      IDirect3DDevice9 + resource types, Shader Model 1.1/2.0 → MSL translation.
- [ ] DirectInput8 → GameController / IOKit
- [ ] DirectSound → CoreAudio
- [ ] DirectDraw (`DirectDrawCreate` is a single call, probably GPU detection)

**Phase 5 — correctness**
- [ ] x87 80-bit ↔ ARM64 64-bit deviation: the effect on physics/determinism must be measured
- [ ] Reference-comparison testing (same input → same output)

## Honest scope assessment

Phases 1–2 are mechanical and predictable. **Phase 4 is the project's center of
gravity** — D3D9→Metal translation is a big job on its own; DXVK took years,
but the SM2.0 subset a single game uses is much narrower.

A realistic estimate for one person: **12–24 months of serious part-time work**,
6–9 months full time. Static recompilation of x86 is not new and this project is
not the first example: [M-HT/SR](https://github.com/M-HT/SR) translates
DOS/Windows games (Septerra Core, Battle Isle 3) at assembly level and brings
them to many targets including macOS arm64;
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) lifts original
Xbox (x86) games with the same Python + Capstone → C method (Burnout 3, 22,097
functions). On the academic side, IR-based binary translation work such as
McSema, rev.ng, RetDec and BinRec has existed for years.

This project's own contributions are narrower:
(a) recovering a reloc table whose directory entry was zeroed but which is
physically still present, and using it to separate pointers from constants,
(b) a differential ISA fuzzer that uses Rosetta 2 as the oracle
(`tools/fuzzisa.py`), (c) a hybrid interpreter for the x86 code the game writes
to its stack at run time (`runtime/genexec.c`), (d) an end-to-end playable port
from a Win32 + Direct3D 9 target to macOS ARM64 / Metal. The reloc table turning
out intact is what moved this project from "research problem" to "very hard but
solvable engineering".

---

# Phases 3–4: Win32 + D3D9 layers, the game reached `main()`

## Where we stand

The game now **runs**: CRT startup, C++ global constructors, `main()`, the
window/registry/file layer, 6 guest threads, **the Direct3D9 device is created**,
the first frame is `Present`ed and the embedded shader effects (`IDI_WORLD_FX`
etc.) are loaded.

The only remaining blocker: **the D3DX effect parameter tree is parsed as a
cycle** (below).

| Stage | State |
|---|---|
| CRT startup (heap, TLS, locale, stdio) | ✅ complete |
| C++ static constructors (`_initterm`) | ✅ complete |
| `main()` / game init | ✅ working |
| Registry / window / synchronization / threads | ✅ working |
| File I/O + PE resources | ✅ working |
| Direct3D9 device + state blocks + shader objects | ✅ working |
| Effect (.efx) parameter tree | ❌ **cyclic — blocker** |
| Real drawing with Metal | ⬜ not started |

## Verification infrastructure

### ISA semantics test suite — `tools/isatest.py`
Hand-encoded x86 machine code + expected results derived from the Intel spec.
**158 cases, 417 checks, all passing.** Coverage: shifts/rotates and their flags,
bit tests, 8/16/32-bit arithmetic and flags, multiply/divide, sign extension,
all `setcc` conditions, memory addressing modes, stack operations, string
instructions (`rep movs/stos/cmps/scas/lods`, including the `df` direction), x87
(load/store/arithmetic/compare/`fnstsw`/`fistp` rounding), MMX/SSE moves.

This suite **found five real lifter bugs** (below). It runs automatically on
every build.

### Lifter bugs found and fixed

1. **`PUSH32` evaluation order** — the macro read the operand *after*
   decrementing `esp`. `push [esp+8]` was off by one slot. This is the basic
   pattern of MSVC argument passing; it affected everything.
2. **Indirect `call` operand order** — the target of `call [esp+0x24]` must be
   read before the return address is pushed. Callbacks such as `qsort` were
   going to the wrong address.
3. **Entry block ordering** — because blocks were emitted by address, **25.2% of
   functions whose entry block is not the lowest-addressed one started in the
   wrong place**. `__security_check_cookie` fell straight into
   `__report_gsfailure` and the game reported *"Buffer overrun detected!"*.
4. **`shl` OF flag** — the MSB of the original was used instead of the MSB of
   the result.
5. **`fxch` never swapped** — capstone gives `(st(0), st(i))` for this
   instruction; taking the first operand meant `ST(0)` was swapped with itself.
   x87 calculations
   were being silently corrupted. Capstone also names `fcomip` as `fcompi`, so
   that branch never matched.

Additional fixes: `sar` 8/16-bit sign extension, `rol`/`ror` operand size,
`xchg` evaluation order, `call`/`jcc` outside `.text`.

### Lines cutting across the screen — the sampling coordinate must be the interpolant itself

Every quad had a 2-pixel line around its frame and along the diagonal shared by
its two triangles (corner to corner on the splash screen, in the sky during
races, most visible over letters).

**Measurement chain:** turning mips off (`MIPOFF=1`) made the line disappear
completely → `lodMaxClamp=1` also removed it, so at the edge the LOD shoots
**far above 1**. The mip contents are complete (`mip levels: 0 MISSING`), the
trilinear filter (`MIPLIN`) and anisotropy (`NOANISO`) have no effect, the
half-pixel offset (`HALFPX`) has no effect.

**Root cause:** in the pixel shader the coordinate was produced by the
`TCRD(k)` macro — a four-way ternary chain plus a conditional division
`abs(w)<1e-6 ? 1.0 : w`. When the derivative quad spilled over a triangle edge,
the helper pixels' coordinates jumped in this condition and shifted by six
orders of magnitude, the LOD blew up and a very coarse mip was selected along
the edge.

**Fix:** the `D3DTSS_TEXCOORDINDEX` selection was moved **into the vertex
shader** (selector in `VSPar.tsel`, 2 bits each). On the pixel side stage k now
reads `i.uvk` directly; the projective division is single and continuous. It is
computed only for stages that actually sample (producing all four made the race
frame go from 15.7 → 18.7 ms). The CPU vertex path applies the same permutation.

### Fog (D3DRS_FOGENABLE) — had never been implemented

`RSLOG=1` measured it: the game sets `FOGENABLE` 5.3 million times and
`FOGCOLOR` 1.9 million times; **6 of the 18 vertex shaders write `oFog`**, but in
the MSL translation `oFog` went to `dummy`. Distant geometry never faded.
Now `oFog` is carried as a single float varying and
`mix(fogColor, color, saturate(fog))` is applied at the pixel output (`NOFOG=1`
turns it off). Note: `D3DRS_DEPTHBIAS` was measured — the game **never sets it**,
so implementing it is unnecessary.

### StretchRect mosaic — must not sample from the drawable

There was a rectangular mosaic on a 32/64-pixel grid in the sky and on glass
facades. Root cause: the scene copy was done with a fullscreen quad **sampling
from the drawable**; stale content came through at tile boundaries (the block
edges were measured at exact multiples of 32/64 — the Apple GPU tile size). 1:1
copies now use `MTLBlitCommandEncoder copyFromTexture`; for a scaled copy the
source is first taken into an intermediate texture with a real copy.
`BLITQUAD=1` brings back the old path.

### Z surfaces — lights and cars were visible through walls (2026-09-24)

From the very beginning the glows of the city lights (and some cars) were
visible through walls. The game NEVER uses occlusion queries (`CreateQuery`
0 times); glows are hidden by the ordinary Z test. The root cause was in the
Metal layer: there was **a single Z texture**, recreated every time the target
size changed. One race frame (measured with `ZSEQ=<frame>`): a 320x240 light
target (its own Z) → 6 x 128x128 reflection cube faces (each with its own Z) →
screen → back and forth between 640x480 blur passes (no Z) and the screen. On
every return to the screen Z was loaded from a new, undefined texture; glows
drawn after the blur were tested against garbage Z.

The fix is D3D9 semantics:
* `CreateDepthStencilSurface` gives the surface an id (+52, marker +56);
  1 = the implicit surface, `GetDepthStencilSurface` returns the bound one.
* `SetDepthStencilSurface` selects the bound surface; NULL → Z test/write off.
* If the `Clear` flags contain ZBUFFER (2), that surface is cleared to the given
  Z value in the next pass; the content is preserved across frames (previously
  it was cleared unconditionally on the target's first use in a frame).
* `metal.m`: `g_ztex[zid]`, bits 8+ of the draw record's `depth` field = zid;
  the pass is reopened when (target, Z) changes. On a size mismatch, a scratch
  texture + Z off (counter in the report: `Z surface: ... size mismatch`,
  measured 0).
Performance unchanged (race 8.34 → 8.34 ms/frame).

### Resolution from the settings menu (2026-09-27)

The game's Display → Resolution row now offers six real options: `720p`,
`1080p`, `1440p`, the view's own pixel count (e.g. `3024x1836`), `4K`,
`1600x1200` (the original 4:3). Measured:
* The menu text comes from ready-made strings in `.rdata` (`sub_0049b690`,
  row `[0x870d1c]` → `0x78fa1c`..`0x78f9e4`; display only). The strings are
  rewritten in memory; the first two slots are 7 characters, hence "720p".
* The `EnumAdapterModes` list only decides SELECTABILITY (the game looks up the
  table value in the list) → the list must stay the game's own table. Since the
  desktop mode was reported as 1920x1080, the 1600x1200 row (the old "1080p")
  could never be selected; now it is ≥1600x1200.
* The rows are generated from the VIEW's aspect ratio ("1080p" = 1080 lines at
  the window's aspect ratio), the window size is fixed; the resolution only
  changes quality. The window is sized once on first launch to the available
  area; when resized by hand the device is rebuilt.
* On Reset the old render targets (in D3D9 the game must release them all) are
  released on the Metal side 3 frames later; previously every change leaked
  ~6 RTs (>100 MB at 4K). Diagnostic counter: if a released RT is used later a
  `WARNING` lands in the report (measured 0).

### Fullscreen (2026-09-24)

The green button / `Cmd+F` / "Full Screen" in the menu is now real fullscreen:
the drawing surface goes to the screen's **pixel** size (14" MBP, below the
notch: 3024x1898), the game's 4:3 request is corrected with Hor+ (aspfix
0.837), and the UI sits in the centered 4:3 area (HUDFIX). Previously the
surface stayed 4:3 and was scaled ~1.03x with bars at the sides.

**How the resolution changes at run time:** the game's render targets are tied
to the device size; only the game itself can change the size, with `Reset`. The
alt-tab path from Windows is used: on the transition `com_request_reset()` →
`Present` returns `D3DERR_DEVICELOST` and `TestCooperativeLevel` returns
`D3DERR_DEVICENOTRESET`; the game (which calls TCL every frame) releases its
resources and calls `Reset`, and `res_pick` → `metal_native_size` picks the
screen size in fullscreen. Measured in the menu and in a race (`RESETAT`,
`FSTOGGLE`): the game does two Resets and carries on as before. The mouse is
mapped to the UI's narrowed area (`metal_set_ui_aspect`). The preference is
remembered in `NSUserDefaults` as `NFSUFullScreen`.

**Input (2026-09-27 fix):** the event loop swallowed every left click into the
game; the green/red/yellow buttons in the title bar and window dragging never
worked. Now mouse events outside the game area go to AppKit. `fn/Globe+F`
(which went to the game as "F" because it carries no Command) is caught
directly, and there is a hidden menu equivalent for `Ctrl+Cmd+F` (`Cmd+F`
remains too). All three paths verified with `FSSIM`.
Performance: race 8.35 (windowed) / 8.40 ms (fullscreen), GPU 4.9 → 5.1 ms.

### Two cursors / offset (2026-09-27)

The game cursor comes from `GetCursorPos`, not DirectInput; the game scales the
position into its own UI space based on `GetClientRect`. These calls returned a
fixed 1280x720; at 1280x960 the cursor drifted vertically by 1.33x (measured
with `MOUSEAT`: (1100,800) went off screen). Now the game's size is returned and
the macOS cursor is hidden over the game area.

### 4K

`NFSRES=3840x2160` works (clean image). M1 Pro race: 8.36 → 8.82 ms/frame,
GPU 4.9 → 6.0 ms. In fullscreen the size equals the screen pixels, but the
`NFSMAXPIX` default is 6 MP; on a 4K monitor fullscreen drops to 3266x1837.

### 1080p / widescreen — `NFSRES`

The game's own resolution table has 6 rows, all 4:3; 1080p cannot be selected
from the menu (index 6+ overflows the array). `NFSRES=1920x1080` (or
`./run.sh 1080p`) opens the device at the requested size. If the device aspect
ratio differs from the game's, the image **would be stretched horizontally**
(measured: the 1080p frame is a pure stretch of the 4:3 frame — difference
2.03; vertical-crop hypothesis 34.2), therefore:

* **Hor+ correction:** in perspective draws, clip-space x is scaled by
  `game_aspect / device_aspect`. Proportions are fixed and more of the scene is
  visible horizontally.
* **Perspective/UI split:** the constant register of the position matrix is
  decoded per shader from the bytecode (`vs_pos_creg`) — the assumption that it
  is in c0..c3 is WRONG (c10..c13 in vs_9). The split uses `c[cw].xyz == 0`;
  `c[cw].w` is the camera distance, not zero (measured: 5.79 / 222.0 / −86.5).
* **UI:** scaled about the center by the same ratio, circles stay circular
  (`HUDFIX=0` turns it off).
* **Render targets** are pulled to the device size, otherwise the post-process
  chain stayed at 1280x1024 and turned into a scaled blit and a mosaic.

Measured: 1920x1080 and 1280x1024 run **at the same speed** (15.73 vs 15.72 ms)
— the bottleneck is not the GPU but the recompiled x86 code.

### Lazy EFLAGS (flag liveness analysis) — `tools/lift.py`

x86's 6 arithmetic flags were computed eagerly per instruction, even though
most are never read. `flag_liveness()` solves **backward liveness** over each
function's CFG and derives, for every instruction, the mask of "which flags are
live after this". The `FLAGS_*_M` / `SETZSP_M` macros in `cpu.h` take the mask
and write only the flags that are needed.

**Measurement:** across 385,701 flag-writing sites, 2.66 flags on average
instead of 6 (**56% fewer**), and 24% of the sites compute no flags at all.
Heavy scene frame time **18.68 → 15.66 ms (53.5 → 63.9 fps, 19%)**.
It can be turned off with `FLAGOPT=0` for A/B testing.

**Pitfalls — every one of these caused a real crash, all handled in `_flag_defuse()`:**

* **Capstone reuses the `eflags` field for the FPU status bits (C0–C3) on
  x87** — `insn.eflags` and `insn.fpu_flags` are the same field. `fld` returns
  0xd002, `fcom` 0x2d; taken at face value, every `fld` looks like it "wrote
  CF" and every `fcom` like it "wrote ZF/SF/AF/OF", so the real computation of
  those flags above is skipped → stale values. Since the game is full of x87,
  this alone caused a crash within 12 seconds. All mnemonics starting with `f`
  are skipped (capstone's `fpu` group is unreliable: `fldln2` and
  `fstp st(0)` show up without a group). The only exception is the
  `fcomi`/`fucomi` family — they really do write all 6 flags.
* **Shifts/rotates with a count of 0 don't touch the flags** (`shl/shr/sar`,
  `rol/ror`, `shld/shrd`; in the lift the body is already guarded by
  `if(_s){...}`). Capstone still says "writes". Trusted if the count is a
  compile-time constant and non-zero, otherwise `def=0`.
* **`cmps`/`scas` with a `rep` prefix never execute when ECX=0** → `def=0`.
  Also, since the loop condition `if(!c->zf) break;` reads the ZF **it wrote
  itself**, ZF is forced into the mask (`|F_ZF`).
* **Capstone doesn't report some flag READS at all**: `pushfd` (all 6),
  `lahf`, `salc`, `rcl`/`rcr`, `cmc`, `daa`/`das`/`aaa`/`aas` → `_EXTRA_USE`.
* **If the lifter writes less than capstone claims**, the flag is killed above
  and not written here either: `imul`/`mul` only write CF+OF → `_DEF_CLAMP`.
* `UNDEFINED_*` **does not count as "writes"** — if it did we would treat the
  flag as dead and skip the real computation, but the lift might not write that
  flag.
* Unknown jump target / `ret` → `live_out = F_ALL` (conservative).

**Audit tool:** `tools/flagaudit.py` — lists every instruction form that
capstone says "writes" but the lift doesn't actually write. This class must be
empty (except `call`/`ret`, which are opaque on purpose). This was the only tool
that found every lazy-flag bug; blind bisection kept moving the target.

### Debugging tools
* **Instruction tracer** — the last 64K guest instruction addresses; "how did we get here" at crash time.
* **Watchpoint** — catches the instruction that writes to a guest address (`WATCH_ADDR=0x...`).
* **Guest stack trace** — resolves return addresses in the `.text` range as function+offset.
* **Instruction hooks** — `HOOKS=addr,...` hooks the requested addresses at generation time.
* **Function override** — `OVERRIDE=addr,...` generates the function as `sub_XXXX_orig`,
  the runtime provides a `sub_XXXX` wrapper (loop breaking / targeted patching).
* **Commit bitmap** — mapping tracking at 16 KB page granularity; prevents the debugger
  from touching unmapped memory and crashing a second time.

## Runtime architecture (additions)

**Guest memory map**
```
0x00400000-0x00930000  image            0x10000000-0x2F000000  CRT heap (496 MB, with headers)
0x00900000+            file handles     0x30000000-0x38000000  COM objects
0x00A00000+            object handles   0x38000000-0x60000000  guest buffers (texture/VB)
0x00C00000-0x00D00000  TEBs             0x60000000-0x70000000  thread stacks (1 MB/thread)
                                        0x7F000000-0x7FF00000  main stack (15 MB)
```
4 GB reserved as `PROT_NONE`, the regions in use are committed.

**Big guest lock (GIL)** — guest code runs on only one thread at a time; the
lock is released only in blocking calls (`Sleep`, `SleepEx`, `WaitFor*`). This
keeps the memory/allocators shared by the lifted code safe and the instruction
trace consistent.

**Guest threads are real pthreads** — each with its own CPU state, guest stack
and TEB. Since the lifted code is reentrant this works directly. Because the
guest call chain is carried on the host C stack, the main guest thread runs
with **a 1 GB stack**; every thread sets up its own `sigaltstack`.

## Win32 layer (`runtime/win32.c`, `win32_ext.c`, `fileio.c`, `res.c`)

251/251 imports have **exact stdcall argument counts**. (Inference from the
`push` sequence at the call site was tried — only **74–77% correct** against
known functions; since every wrong count shifts `esp` and corrupts the stack,
an authoritative table was entered by hand. cdecl variadics such as
`wsprintfA` are marked as 0.)

Implemented: heap (with headers, correct `HeapSize`/`ReAlloc`), virtual memory,
TLS, critical sections, time, locale/code page (`GetStringTypeW` C1_* flags,
`LCMapStringW`, `GetLocaleInfo`), `MultiByteToWideChar`/`WideCharToMultiByte`,
`GetProcAddress` (resolution by name), registry (in memory, "not found" default
→ the game uses its own defaults), window/cursor/message loop,
event/mutex/semaphore, `CreateThread`, **real file I/O** (Windows paths → game
directory, case-insensitive resolution, directory enumeration), **PE resource
directory** (`FindResource`/`LoadResource`/`LockResource` — the embedded `.efx`
shader effects come from here).

## COM / D3D9 layer (`runtime/com.c`, `d3d9_tables.h`)

A bridge that offers the guest COM objects with native implementations: the
object's vtable is filled in guest memory with the magic addresses
`0xE0000000|(interface<<16)|method`, and `dispatch_indirect` routes them to
`com_call`. Interfaces and **full method tables** (name + argument count):
`IDirect3D9` (17), `IDirect3DDevice9` (119), `Texture9`, `VertexBuffer9`,
`IndexBuffer9`, `Surface9`, `VertexShader9`, `VertexDeclaration9`,
`StateBlock9`, `SwapChain9`, `Query9`.

The path the game actually uses was verified: `Direct3DCreate9` →
`GetDeviceCaps` → `GetAdapterIdentifier` → `EnumAdapterModes` →
`CheckDeviceMultiSampleType` → **`CreateDevice`** →
`BeginScene`/`Clear`/`EndScene`/**`Present`** → `CreateVertexDeclaration` →
`CreateVertexShader`/`CreatePixelShader` →
`BeginStateBlock`/`SetVertexShader`/`EndStateBlock` → `StateBlock::Apply`.

Shader objects keep the real bytecode (scanning the token stream up to the
`0x0000FFFF` END token) and `GetFunction` returns it.

## OPEN ISSUE: the effect parameter tree is cyclic

`sub_0065ae35` walks a parameter tree (arg3 = ±1, like a reference counter).
The tree is **cyclic**: node 6's children are 4,5,6,7,8 — i.e. itself. Result:
unbounded recursion, exhausting the 15 MB guest stack after 286,134 calls.

Ruled out for certain:
* The effect data is intact — `IDI_WORLD_FX` signature `0xFEFF0901`, header 14 parameters / 2 techniques / 9 objects.
* It doesn't depend on shader capabilities — **exactly the same** 286,134 calls with `D3D_SM=0/11/14`.
* Resource loading is correct, file I/O is correct.
* ISA semantics verified with 367 tests.
* No function truncation (the `discover` limit truncates no function, 0 "truncated" stubs).
* 0 unresolved indirect calls, 0 unsupported-instruction traps.

The traced chain: `edi = [eax+0x48] + [esi]`, `[edi]` = child base index.
`[eax+0x48]` is written in the parser in `sub_00667d61` at `0x6688cc`
(`esi = [ebp-0x40] + word[edi+6]*4`), and `[ebp-0x40]` is set up at `0x6682e9`.
At the problem node all counters are 0 → all offsets are 0 → `[edi]` points to
the start of the blob. The value is written into the blob at `0x6688de`:
`blob[[edx+0x48]] = word[eax+0x44] + 1`.

**Temporary measure:** `runtime/overrides.c` applies two guards — recursion
depth is capped at 32, and nodes whose index array is NULL while the counter is
>0 are skipped safely. This prevents the crash, but since the calling function
retries forever the game can't get past this point (the guard exits cleanly at
a ceiling of 200,000).

### Standalone effect parser — `tools/efxparse.py`
Parses the D3DX9 `.efx` binary format in Python (a reference for checking
locations). Output for `IDI_WORLD_FX`:

```
parameters=14 techniques=2 objects=9
  0 SCALAR  INT    5 elements BlendState :BLENDSTATE      <- the ONLY array parameter
  1 SCALAR  INT    0          BaseMinTextureFilter
  ...
  6 MATRIX_ROWS FLOAT 0       TextureOffsetMatrix         <- the node blamed for the cycle
  ...
 13 OBJECT  SAMPLER 0         samp
```

**No parameter is a STRUCT; only parameter 0 is an array (5 elements).** So in a
correct parse node 6 should have no children at all. The `[edi]=5, childCount=5`
the walker reads is exactly parameter 0's element count — i.e. it is looking at
the wrong entry.

### The last link of the trace (found)
The child base index chain was followed to the end:

```
0x0066fbf8  mov ax, word [ebx+0x30]      ; ebx = effect object, +0x30 = allocation counter
0x0066fbfc  mov word [edi+0x44], ax      ; "my children start here" for every node
0x006688d3  mov cx, word [eax+0x44]
0x006688de  mov word [blob + offset], cx+1
0x0065aecd  movzx ecx, word [edi]        ; the walker reads this
0x0065aed3  lea eax, [ecx+eax-1]         ; child = [edi] + i - 1
```

Measurement: the `[ebx+0x30]` counter really does increase (0→1→2→3→4→…→8,
incremented by the instructions at `0x0066b7cb`, `0x006655df`, `0x00665305`),
**but the writes to all 14 nodes happen while the counter is at 3.** So every
node says "my children start at 3"; node 6 also counts the range 4..8 as its
children, and the cycle forms.

### Mechanism solved (but the bug is still open)

Functions involved:

| Address | Function | Role |
|---|---|---|
| `0x0066fbfc` | `sub_0066f6ac` | `[edi+0x44] = [ebx+0x30]` — writes "my children start here" to the node |
| `0x0066fc10` | → `sub_0066549b` | allocates the children right after, advances the counter |
| `0x006655df` | `sub_0066549b` | `add word [edi+0x30], si` — si = number of children |
| `0x00665305` | `sub_0066521d` | `inc word [esi+0x30]` |
| `0x0066b7cb` | `sub_0066b61d` | `inc word [ebx+0x30]` |

The design is **correct**: first the current base is saved, then the children
are allocated and the counter is advanced. The measurement confirms it too — the
counter goes from 3 to 8, exactly as many as `BlendState`'s **5 elements** (+1 at
`0x006655df`, +1 four times at `0x00665305`). For the 13 parameters without
elements `si=0`, so the counter doesn't advance; that is also correct.

The counter reaches 8 and the effect has **9 objects** — so `[ebx+0x30]` is the
object/element counter and `[this+0x2c]` the 9-entry object array. The index
range the walker visits (0..8) fits this.

**Still open:** nodes without children also keep a base of 3 (or 4) written to
them, and when `childCount==0` the walker **forces it to 1**
(`0x0065aeb8`–`0x0065aebe`), then recurses with `child = base + i - 1`. So even
a node without elements descends into a child. In correct behavior these nodes'
`[edi]` entry would be 0 and they would be skipped by the
`cmp word [edi], dx; je` at `0x0065aea4` — but because the blob entries overlap,
5 is read instead of 0.

**Next step (clear):** find out why the blob entries overlap. The `[edx+0x48]`
offsets should be separate per node; in the measurement the values 0 and 4 are
written to **two different nodes in the same blob**, but a third node reads
offset 0 again. We need to match which branch of the 4-way type switch in
`sub_00667d61` (after `0x0066888e`: `0x668b62`/`0x668ab7`/`0x668934`/fall-through)
runs for which parameter class against the output of `tools/efxparse.py` — the
classes of the 14 parameters are known (SCALAR/VECTOR/MATRIX_ROWS/OBJECT), so the
expected branch sequence can be computed and compared with the real one.

## Commands

**Versions:** the `VERSION` file at the repository root (currently `2`). The
package writes it into its version info ("2.0" in Finder), the binary writes it
to the log at startup, and session recordings are labelled
`~/nfs_sessions/<date>_v2`.
- **v1:** the base translator, no `-fno-strict-aliasing`. Archive: `arsiv/v1/`
  (outside git, contents described in `arsiv/v1/OKUBENI.txt`).
- **v2 (2026-10-18):** FLAGABI + X87OPT + RETEIP=0 + MEMARG=1, everything with
  `-fno-strict-aliasing`. Game code −22% CPU time per frame
  (`LIFTER_EFFICIENCY.md`).

A new lifter step is first tried in a separate `gen/<name>` directory. When it
is promoted to the main version, the old version is moved under `arsiv/vN/` and
`VERSION` is incremented by one.

### Running
```bash
./run.sh window      # windowed (Metal)
./run.sh headless    # no window, 60 s
./run.sh profile     # sampling profiler
./run.sh test        # 439 ISA + 15 function tests
./run.sh hd          # HD build: graphics enhancements (NFSHD=1 works with every mode)
```

**Two builds:** `out/nfsu2_boot` is the game's original look, `out/nfsu2_hd_boot`
has graphics enhancements. The lifted shards are shared; only the runtime is
compiled a second time with `-DNFS_ENHANCED=1` (`./link.sh [orig|hd]`). The
enhancement code is not compiled into the original build. Package:
`./pack.sh --hd` → `NFSU2 HD.app`. Details and results: `GRAPHICS_RESEARCH.md`
§6 (reflection resolution), §7 (MSAA), §8 (SSR + reflection separation), §10
(ray-traced reflections + static scene cache).

```bash
# full regeneration + build (~2.5 min).  Run from the repository root.
# 1) reloc table -> out/relocs.txt  (genall.py and argcount.py read it)
./.venv/bin/python tools/reloc2.py <SPEED2.EXE>
# 2) v2: flag liveness + translate 23k functions to C -> gen/all/, import
#    argument table -> gen/all/impargs.c.  All in one script; the recipe
#    reproduces gen/all exactly (verified 2026-10-18: 131 files, 0 differences).
# CAUTION: the OVERRIDE list in the script must be the SAME as runtime/overrides.c
# + runtime/d3dx_hle.c, otherwise the sub_XXXX_orig symbols aren't generated and
# the link breaks. d3dx_hle.c generates its wrappers with a macro (grep won't find
# them); the current list is in the "Regeneration" section of D3DX_HLE.md.
tools/gen_v2.sh <SPEED2.EXE>
# 3) compile + link
./build.sh && ./link.sh          # OPT: -O3 -mcpu=apple-m1 -fno-strict-aliasing

# if only the runtime changed (~10 s)
./link.sh

# run / diagnose
./diag.sh                          # find the crash address + disassemble around it
ALARM=60 ./out/nfsu2_boot <EXE>    # run with a timeout
ITRACE=40 ./out/nfsu2_boot <EXE>   # last 40 guest instructions
WATCH_ADDR=0x... ./out/nfsu2_boot  # memory watchpoint
COMTRACE=1 ./out/nfsu2_boot        # D3D9 call trace
./out/isatest                      # 367 ISA semantics tests
./out/nfsu2_all <EXE>              # 15 function verification tests
```

---

# MILESTONE: the game runs and produces frames

As of the morning of 25 August 2026 **the NFSU2 engine runs as native ARM64
code**: CRT → static constructors → `main()` → D3D9 device → effect system →
data loading → **game loop and front end (menu)**.

## Measured run state (60 s run)

| | |
|---|---|
| **Frames rendered (`Present`)** | **373** |
| **Draw calls (`Draw*`)** | **69,540** |
| Total COM/D3D9 calls | **1,729,435** |
| Total Win32 calls | 54,369 |
| Data files opened | **64** (12.7 MB read) |
| Guest threads | 9 started, 5 terminated |
| Guest instructions executed | ~1.1 billion |
| Unresolved indirect calls | 0 |
| Unsupported-instruction traps | 0 |

Real game data loaded: `GLOBAL/GLOBALA.BUN`, `GLOBAL/GLOBALB.LZC`,
`GLOBAL/GLOBALTEXTURESROW.BIN`, **`FRONTEND/FRONTB.LZC`** (menu),
`LANGUAGES/ENGLISH.BIN`, `memcard/LOCALE_ENGLISH.loc`, `sdata/sdat.viv` (sound),
`Global/FrontEndMemoryFile.bin`, `Global/CarHeadersMemoryFile.bin`.

CPU usage 2% — the game is idling in its draw loop waiting for input, i.e. it is
sitting in the menu.

## Bugs found and fixed at this stage

**Lifter (real bugs no. 6 and 7):**
6. **`int3` was taken for a block terminator.** The epilogue after a single
   `int3` used for assertions was never generated and execution fell into the
   body of the next function. Now a single `int3` is treated as a no-op and
   execution continues; a run of consecutive `0xCC` (padding between functions)
   is still a block end.

**Runtime:**
* **`ExitThread` didn't really terminate.** `_endthreadex` calls `ExitThread` at
  the end; when the stub simply returned, execution **fell** into the
  `_threadstartex` prologue and the thread restarted itself. It now terminates
  with `pthread_exit`.
* **TLS wasn't per-thread.** `TlsGetValue`/`TlsSetValue` used a global array;
  now the per-thread array in the TEB (`fs:[0x2c]`) is used.
* **`CREATE_SUSPENDED` was ignored.** Now the thread waits until `ResumeThread`.
* **The big guest lock (GIL) caused starvation.** The game was in a wait loop
  spinning on `EnterCriticalSection`; since the lock was only released in
  `Sleep`/`Wait` calls, the awaited thread could never run (188 million empty
  calls). Now the lock is released fairly once every 1024 Win32 calls.
* **`GetCurrentThreadId` returned 1 for all threads** — it is now read from the TEB.

**New layers:** `IDirectInput8` + `IDirectInputDevice8` (11 + 32 methods),
`IDirectSound` + `IDirectSoundBuffer` (11 + 21 methods), real file I/O, PE
resources.

## New tools

* **Sampling profiler** (`SAMPLE=1`) — a separate thread periodically samples
  the running guest instruction. Finds infinite loops and hot spots within
  seconds; this is how we caught the GIL starvation.
* **Run-time patch table** (`dispatch_patch`) — replaces an indirectly called
  guest function with a C wrapper **without regenerating/recompiling**. Cut the
  iteration time from 5 minutes to 10 seconds.
* **Heap block bounds check** — finds the heap block containing a guest address
  from its header; detects array overruns with certainty.

## OPEN: the D3DX effect tree is cyclic (bypassed with a temporary guard)

The tree loops back on itself as node 0 → 6 → 4..8 → 6, and index 8 is requested
while the node array capacity is 8. `runtime/overrides.c` applies two guards:
the array bound and a **set of visited nodes** (each node once per top-level
walk). In the 60 s run the guard kicks in only 1,295 times, so it is bounded and
the game progresses. The root cause is still open — the detailed trace is in the
section above.

## NEXT PHASE: D3D9 → Metal

The D3D9 surface the game **actually uses** was measured (60 s, a single menu
screen):

| Method | Calls |
|---|---|
| `SetRenderState` | 646,609 |
| `SetSamplerState` | 360,874 |
| `SetTextureStageState` | 243,416 |
| `SetTexture` | 69,779 |
| `SetVertexShader` / `SetPixelShader` | ~69,700 |
| **`DrawPrimitiveUP`** | **69,526** |
| `SetVertexShaderConstantF` | 69,257 |
| `StateBlock::Capture` / `Apply` | ~11,300 |
| `SetRenderTarget` / `SetDepthStencilSurface` | 1,386 |
| `CreateTexture` + `LockRect`/`UnlockRect` | 776 / 1,205 |
| `SetViewport`, `SetTransform` | 1,064 / 966 |
| `Present` / `BeginScene` | 372 |

**Key simplification:** the main draw path is `DrawPrimitiveUP` — vertex data
comes straight from a memory pointer, there is no vertex buffer management. In
Metal it is enough to upload the buffer and draw on every draw call.

Required order:
1. ✅ **Real `NSWindow` + `CAMetalLayer`** — `runtime/metal.m`. The main thread
   drives the NSApplication loop, the guest runs on a separate thread.
2. ✅ **`Clear` + `Present` → Metal render pass.** The game's `Present()` calls
   drive real `CAMetalDrawable`s: 456 game frames in 30 s, **138 Metal
   presentations** (Apple M1 Pro). The window currently shows only the game's
   `Clear` color.
3. ✅ **`CreateTexture`/`LockRect`/`UnlockRect` → `MTLTexture`.**
   A8R8G8B8→BGRA8Unorm, DXT1→BC1, DXT3→BC2, DXT5→BC3 (Apple Silicon supports BC).
   Including mip levels and the block-compressed pitch calculation.
   **767 textures created, 1,186 uploads** in 30 s.
4. ✅ **`DrawPrimitiveUP` → Metal draw path.** The vertex layout was derived by
   measurement: `POSITION float3 @0, COLOR D3DCOLOR @12, TEXCOORD float2 @16`,
   stride 32 bytes, `FVF=0x142` (XYZ|DIFFUSE|TEX1). **The vertices are already
   in NDC (-1..1)**, so the geometry can be drawn directly without translating
   the vertex shader. Since Metal has no triangle fans, fan/strip/list are
   converted to triangle lists. **41,126 triangles** drawn to the screen in 30 s.
5. ⬜ Shader Model 2.0 bytecode → MSL translation (the biggest remaining item).
   For now a fixed vertex/fragment shader pair is used: position pass-through,
   `color × texture` with alpha blending.
6. ⬜ `SetRenderState`/`SetSamplerState` → `MTLRenderPipelineState` /
   `MTLSamplerState` cache (call counts are high, a cache is a must).
7. ✅ **Input: `NSEvent` → DirectInput keyboard.** macOS virtual key codes are
   mapped to DirectInput `DIK_*` scan codes (letters, digits, arrows,
   Enter/Space/Esc/Tab/Shift/Ctrl). The game calls `GetDeviceState` **every
   frame** (399 times in 30 s) and reads the 256-byte table.
   Cmd+Esc closes the window.

### Measured DirectInput/DirectSound usage
```
IDirectInput8        CreateDevice 1, EnumDevices 1, EnumDevicesBySemantics 1
IDirectInputDevice8  SetDataFormat 1, SetCooperativeLevel 1, Acquire 1,
                     GetDeviceState 399      <- keyboard poll every frame
IDirectSound         CreateSoundBuffer 7, GetCaps 1, SetCooperativeLevel 2
```

### Render architecture
macOS wants the window on the main thread; the guest already runs on a separate
thread. The guest thread accumulates draws into a double-buffered command queue
(`BeginScene` resets, `Present` hands the buffer over to the main thread), and
the main thread encodes onto the `CAMetalDrawable`. The lock is held only during
the queue handover.

---

# Performance: 60.3 → 98.8 fps (race frame 16.59 → 10.12 ms)

**The end-to-end figure** was measured alternating A/B/A/B (old = `PACE60=1`,
new = default), so thermal drift was spread evenly over both configurations:
paired frame time **−39.2%**. Virtual/real clock ratio 0.998 vs 0.999 — **game
speed doesn't change**, only smoothness improves.

> Beware of back-to-back measurements: when the machine stays under load for
> hours, thermal drift reaches 3–4%. The step-by-step table below comes from
> runs spread over hours; the *order* and *order of magnitude* of the steps are
> reliable, but don't compare their absolute values with the alternating
> measurement.

Measurement window: the middle of the race, frames with `draws > 400`, paired
by real-time buckets (see "Measurement method"). In the order applied:

| step | ms | fps |
|---|---|---|
| baseline (60 Hz limiter on) | 15.70 | 63.7 |
| frame limiter removed | 12.60 | 79.4 |
| COM dispatch name → numeric id | 11.06 | 90.4 |
| single-block vertex copy for triangle lists | 10.84 | 92.3 |
| chain order + cube texture condition order | 10.63 | 94.1 |
| `restrict`, encoding lock, `rs_apply` dirty flag | 9.82 | **101.8** |

## 1. Frame limiter: 61% of the frame was sleep

`frame_pace()` (`runtime/win32_ext.c`) locked the frame to 60 Hz. The rationale
was: since the virtual clock advanced EXACTLY 16.667 ms per frame, game speed =
(real fps / 60). **That rationale no longer holds** — since the virtual clock
switched to `VCLOCK=real` mode, the game flows in real time.

The profile showed it directly: **11438** of the guest thread's 18671 samples
(61%) were `frame_pace` → `nanosleep` → `__semwait_signal`.

The limiter is now **off by default**; `PACE60=1` brings back the old one. The
game doesn't speed up: the virtual/real ratio is **0.999** at both 63.7 fps and
101.8 fps. Runaway fps is cut by vsync (120 fps in the menus = the display
refresh limit), and the virtual clock's per-frame increment is floored at 4 ms
(250 fps).

> This also explains all the "the optimization changed nothing" results in the
> first half of the session: since 61% of the frame was sleep, every CPU saving
> was swallowed by the limiter. **You can't measure CPU with the limiter on.**

## 2. COM dispatch: from name comparison to numeric ids

A race run has **377 million `com_call`s** (~43 per frame, ~43 per draw). The
dispatch compared the method NAME. First `NMEQ` (first-letter check + `strcmp`)
was tried, and it **gained nothing**: almost all D3D9 device methods start with
`Set`/`Get`/`Draw`, so the first-letter filter doesn't eliminate anything and a
full `strcmp` still runs at every step. In the profile 11% of the guest thread
was `strcmp`, 2335 samples from `com_call`.

Solution: a `MID_*` constant per method; the id is resolved **once** per method
with an `(iface, slot) → id` cache, and the chain is pure integer comparisons.
The weighted chain depth was 26.4 comparisons/call, **9.9 billion** comparisons
in total.

Then the chain was reordered by call frequency (the 11 hottest segments to the
front). Safety criterion: the `MID` tests are mutually exclusive; for a segment
to be movable it must not jump over the `strncmp(nm,"Draw",4)` /
`strncmp(nm,"Get",3)` prefix blocks — the `Draw*` block was moved as a whole.

## 3. Measurement method (turned out to matter this much)

Single-run comparisons were **±7% noisy** — a change was seen to "drop" from
94.1 to 87.6 even though that change could only have helped. Cause: scene
differences between runs.

The path tried and **rejected**: pairing by frame index with `VCLOCK=fixed`.
With a fixed clock the game runs 2× faster and the AUTOKEY sequence never
reaches the race (83 draws per frame = menu).

The path used: with the real clock the game state depends on **elapsed real
time**, so the same second = the same scene. Frames are put into 0.5 s buckets,
the median is taken per bucket and the two builds are paired bucket by bucket.
Run repeatability: **10.32 / 10.33 ms** (0.1%). Scripts:
`scratchpad/bench.sh`, `bcmp.py`.

## 4. Small but real ones

- **Single-block vertex copy** (`metal_draw_raw`): for triangle lists the source
  is already contiguous, yet `memcpy` was called PER VERTEX (~112 thousand
  small calls/frame). `_platform_memmove` 4.6% → 2.8%.
- **`restrict`** (`tools/lift.py` now emits `CPU *restrict c`): the guest
  registers are in the `CPU` struct, and `MR32()` writes the SAME type
  (`uint32_t`) into `g_mem`; so after every guest memory write the compiler had
  to reload fields like `c->eax`. Since the CPU struct is not inside `g_mem`,
  `restrict` is correct. −1.7%.
- **Encoding lock** (`runtime/metal.m`): the main thread held `g_qlock` for the
  WHOLE frame encoding (0.4–2 ms); since `metal_draw_raw` takes the same lock per
  draw, the guest thread blocked on every draw in that window — and that overlap
  was exactly the point of double buffering. The lock is now only for reading
  and writing `g_ready`. Lock wait 2.2% → 1.2%.
- **`rs_apply` dirty flag**: `SetRenderState` is called 110M times in a run and
  **96.5% of the calls rewrite the same value**. Since `rs_apply` derives
  entirely from `g_rs[]`, skipping it gives exactly the same result; 110M →
  3.85M applications. (The flag is sticky: changes made while Metal isn't ready
  are still applied when the device arrives.)
- **`is_cube_tex` condition order**: this test, which reads guest MEMORY, ran on
  every texture call; `IDirect3DTexture9` AddRef+Release alone is 96.8M calls.
  The cheap method-id test was moved to the front.
- **`ENVC()` / `mem_is_committed` inline**: `getenv` and `strcmp` disappeared
  from the profile completely.

## 5. Measured and REJECTED (don't try again)

- **PGO** (`-fprofile-generate` → `llvm-profdata` → `-fprofile-use`): only
  **0.9%** on top of `restrict`. A two-stage build + a profile file that goes
  stale on regeneration isn't worth it. The infrastructure remains: `link.sh`
  takes `$LDOPT`, `QUITFRAMES=<n>` gives a clean exit (the ALARM path calls
  `_exit` from the signal handler and skips `atexit`), `tests/boot.c` writes the
  profile via `dlsym` (`weak_import` gives a link error on Mach-O).
- **`NOVSYNC=1`**: 15.71 → 15.95 ms, i.e. nothing. Vsync doesn't constrain us at
  all (`nextDrawable` 0.02 ms/frame).
- **`dispatch_indirect` patch table order** and **inline `mem_is_committed`**:
  +0.3%, within noise. `g_patch_n` is actually 1; the removed scan was two
  instructions. (The changes are correct as code and remain.)

- **Metal system threads (2026-10-18, v2).** In a race ~30% of the process's CPU
  is in macOS's Metal/CoreAnimation GCD threads (0.34–0.39 cores, 90% kernel
  mode; `sample`: `IOGPUCommandQueueSubmitCommandBuffers`). Tried:
  - **Persistent residency set** (`MTLResidencySet`, all textures and buffers):
    no effect (0.384/0.389 → 0.380/0.362, alternating 2+2).
  - **+20/+40 empty passes per frame:** +0.04 / +0.01 cores, so the submission
    cost per pass is small.
  - **Micro-benchmark** (the game's pattern: 1400 draws, 25 passes, 200 textures,
    548 B of inline constants, fullscreen target + depth, 120 Hz `CAMetalLayer`
    presentation): 0.39–0.41 cores. This is very close to the ~0.45 in the game
    (render + GCD); even presenting an empty frame at 120 Hz costs 0.15 cores. So
    nothing abnormal; this is the cost of the pattern.
  - **`setVertexBufferOffset` + supplying constants from a frame buffer:** at
    best −0.02 cores (~0.05 W), at the noise limit; not done.

  The only real pollution found was the measurement tool: the PERFLOG screenshot
  read the GPU-compressed texture row by row with `getBytes` (~0.08 cores, only
  while recording). It is now copied to a linear `MTLBuffer`.

- **PASS LOAD and Z STORE (2026-10-18):** cutting color/Z load-store that looks
  unnecessary in render passes. In an in-run A/B, GPU energy and DRAM traffic
  didn't change (the Apple GPU already skips them); dropping the Z store violates
  the D3D9 contract (the game preserves the implicit Z across frames). Details:
  "GPU energy and memory traffic".

## 6. Current profile (guest thread, race)

```
79.1%  lifted x86 (the game itself)
 8.5%  com_call
 3.4%  _platform_memmove
 3.0%  dispatch_indirect
 1.2%  __psynch_cvwait
```

Guest thread wait 0.29 ms/frame, the main thread waits IDLE 9.88 ms/frame for a
frame to arrive, GPU 2.19 ms, CPU encoding 0.60 ms. **The bottleneck is entirely
the guest thread**; the graphics layer is free. The next real lever is the
remaining body of `com_call` (8.5%, 377M calls) or the lifter's code generation.

## 6b. The D3DX effect library: 47% of guest time (2026-09-23)

In the new profile most of the translated code turned out to be not the game's
own logic but the **D3DX9 effect framework** statically linked into the game:
`ID3DXEffect::BeginPass` (vtable 0x7b6fe8, slot 66) with its subcalls is 48% of
the guest thread. The game calls BeginPass on every draw (1398/frame), and D3DX
recomputes ~20 states and ~8.5 preshader expressions every time. Yet 95.8% of
the expression inputs are the same as in the previous evaluation.

The first HLE step (state value memo, `runtime/d3dx_hle.c`) cut guest CPU time
in a race from **10.9–12.0 → 8.1 ms/frame (−29%)**; with an idle GPU the race
went 90 → 118.9 fps (the 120 Hz limit). In memory-comparison verification 2.4M+
checks, 0 differences.

Stage 2 (state assignment memo) relies on D3DX's ~27 device calls per pass; 96%
of them are redundant. If the recorded calls currently have no effect, the state
is skipped. In a measurement locked to 60 fps it cut guest CPU by another 5%; at
120 Hz the race is already at the limit, so fps didn't change. 6.6M+
verification checks, 0 differences.

Stage 3 (pass-level skipping with parameter versions) turned out negative.
Textures come not from parameter data but from the effect's object table, and
the game changes textures on every draw; in verification 78% of the predicted
hits were wrong. The code remains, off by default.

Map, data structures, stages and progress log: **`D3DX_HLE.md`**.

## 7. The `sub_0050d510` error: NOT a game bug, a shutdown race

The error seen in some of the runs

```
*** FAULTING GUEST FUNCTION: sub_0050d510
*** HOST SEGV: 0x... (outside guest space)
```

happens **not during the game but at the `ALARM` shutdown**. Evidence: the
report dump comes right after the error output, and the run that crashes is the
one that produced the MOST frames. `all_reports()` is called from the `on_alarm`
signal handler and walks COM/report structures while guest threads are still
running.

Measurement: with a clean exit (`QUITFRAMES=<n>`) **5/5 runs, 0 errors** (three of
them exactly 10301 frames); with `ALARM`, 2 of 7 runs crash. The reported
function name is misleading too — `sub_0050d510` is a C override in
`overrides.c`, and when the host PC falls into the functions after it, the crash
is attributed to it.

**Conclusion:** use `QUITFRAMES` instead of `ALARM` in measurements and automated
runs. (Also in this session the stack of the guest worker threads was raised from
512 KB → 256 MB; `CreateThread` got the macOS default via
`pthread_create(...,0,...)`, whereas the main guest thread gets 1 GB. A correct
fix, but it wasn't the cause of this error.)

## 8. 1920x1080 in the menu

Vanilla NFSU2's own resolution table has 6 rows, all 4:3. Advertising a **7th**
mode with `EnumAdapterModes` makes the menu able to select that row, but the game
overflows its own array and produces a garbage size (exactly
`8126268x268571776` every time). The garbage doesn't only reach `CreateDevice` —
the game **stores** it and tries to create its render targets at that size too
(Metal texture validation blows up).

Patching the table in guest memory would have been the clean solution; **it
wasn't found** (both a 12-uint32 array search and the overflow signature
`1600,1200,0x007C08FC,0x10021C00` were searched for, neither matched — the table
isn't stored as a contiguous `{w,h}` array).

The applied solution: the overflow is **never triggered**. The 6th row in the
menu appears to the game as `1600x1200` and to the user as `1920x1080`; since
the game keeps reading a valid 4:3 value from its own table, its internal state
stays consistent, and we set the device up at 1920x1080 and compensate for the
difference in clip space — the same as the `NFSRES` path. `NOWIDE=1` goes back
to a real 1600x1200. Measurement: 97.6 vs 99.1 fps, i.e. free.

## 9. Measurement trap: `-O0` if `$OPT` is empty

`build.sh` builds the compile command with `xargs -I{} sh -c "..."`, and when a
long flag is added to `$OPT` it fails with "command line cannot be assembled,
too long"; so a wrapper script (`scratchpad/cc1.sh`) was written. The script used
`$OPT` **directly**, so when run with `unset OPT` the shards were silently
compiled at **`-O0`**: race frame 37 ms instead of 10 ms (3.7x), and an hour of
measurements went in the bin.

The symptom is clear in the profile: if `parity8`, `x87_r2`, `fcompare` appear
**as separate functions**, the shards are not optimized (when optimized they are
inlined). The script now uses `${OPT:--O3 -mcpu=native}`.

## 10. Flag liveness: measured, NO GAIN

In the generated code **37.8% of `FLAGS_*` calls compute all six flags** with the
`0x3f` mask, and only 25.3% are free. Reason: the liveness analysis counts
`call` as "reads all flags", so the flags stay live at every call boundary.

Making `call` "reads no flags" fixes the static distribution considerably (free
25.3% → 35.6%, six-flag 37.8% → 16.8%) but **has no effect on frame time**:
10.02 → 10.24 ms. Reason: clang already eliminates unread flag writes by itself;
the lifter's mask largely duplicates that.

> The first attempt showed it slowing things down 3.3x — that measurement was the
> `-O0` accident (see above), the result was invalid. With a correct build the
> gain is zero.

**Conclusion:** pruning flag emulation more aggressively was removed from the
roadmap. The remaining cost is inherent to the translation: guest registers live
in memory, guest memory access is `g_mem + offset`, and the game's own very
chatty renderer (~1250 draws, ~62 thousand COM calls per frame).

## 11. New environment switches

| switch | function |
|---|---|
| `PACE60=1` | brings back the old 60 Hz frame limiter (default: no limiter) |
| `SPINSLEEP=<µs>` | the game's 120 fps frame limiter (`sub_0057eae0`) sleeps instead of spinning, the final margin (default 200 µs) by spinning, sleep via `kevent` `NOTE_CRITICAL`; `0` turns it off. −1.2 W in a race (see `D3DX_HLE.md`) |
| `QPCWHO=1` | dump of QueryPerformanceCounter callers (diagnostic) |
| `PERFLOG=<dir>` | real game session recording (while the user plays); see "Real session recording" |
| `NOVSYNC=1` | turns vsync off — ONLY for measurement, the image tears |
| `QUITFRAMES=<n>` | exits CLEANLY after n frames (for PGO/measurement) |
| `LDOPT=...` | appended to `link.sh`'s final link line |
| `NOWIDE=1` | makes the 1920x1080 row in the menu a real 1600x1200 |
| `ZSEQ=<frame>` | prints the sequence of SetRenderTarget / SetDepthStencilSurface / Clear / Z state in that frame |
| `NFSFULL=1/0` | start fullscreen / windowed (overrides the remembered preference without changing it) |
| `FSTOGGLE=<frame>[,...]` | fullscreen toggle at those frames (test) |
| `FSSIM=<frame>:<k\|m\|c>[,...]` | synthetic input at that frame: `k` fn+F, `m` Ctrl+Cmd+F, `c` click on the green button (goes through the event loop) |
| `RESETAT=<frame>` | produce a device loss at that frame; tests the game's Reset path |
| `MOUSEAT=x,y[@frame,x,y]` | pin the cursor position the game sees (cursor mapping test) |
| `NOFOCUS=1` | open the window in the background, don't steal focus (testing while the user works) |
| `NOSOUND=1` | no sound output |
| `NFSU2_SLEEP_OLD=1` | the game's `Sleep`/timer back on `nanosleep` at default QoS, and every sound fix below off (issue #12 A/B) |
| `NFSU2_CATCHUP=0` | no timer catch-up / counted timer wake-ups / sleep debt |
| `NFSU2_PACE=0` | the sound loops read the virtual clock again (they stand still in long frames) |
| `NFSU2_GILPRIO=0` | no big-lock priority for the sound threads |
| `NFSU2_IOYIELD=0` | large file reads keep the big lock |
| `SLEEPSLACK=<us>` | measurement: every game `Sleep` / timer sleep wakes this much later |
| `GILHOG=<ms>:<ms>` | measurement: the game thread keeps the lock for the first value every second value, re-taking it every 50 us |
| `READSLOW=<us>` | measurement: every file read of 16 KB or more takes this much longer |
| `NFSU2_TEXFREE=0` | Metal textures are never released and ids never reused (before issue #49) |
| `NFSU2_OBJREUSE=0` | COM objects get a vtable each and are never reused (before 2026-11-04) |
| `NFSU2_POOLMERGE=0` | vertex/index pool free blocks are not merged, at most 4096 kept |
| `SNDREUSE=0` | sound buffer slots are never given back |
| `NFSU2_HANDLEFREE=0` | CloseHandle / RegCloseKey / DeleteCriticalSection free nothing |
| `EVENTLOG=1` | the Win32 event table every 400 waits (was always on) |
| `MATLOG=1` | the `[matrix]` totals every 20,000 draws (was always on) |
| `NORTFREE=1` | turn off releasing old render targets on Reset (diagnostic) |
| `NORTRESET=1` | don't return the target/Z to the back buffer on Reset (old behavior; the car breaks) |
| `NORSRESET=1` | don't reset render/sampler/stage states to defaults on Reset (old behavior) |
| `NOTEXZERO=1` | clear only level 0 of new textures, never clear cubes (old behavior) |
| `FSFAKE=<frame>[,...]` | imitate a fullscreen transition from the game's point of view without touching the window (test) |

### Inside-out car after Reset (2026-09-27) — SOLVED

Symptom: after entering and leaving fullscreen (or changing resolution) the car
in the main menu was inside-out/transparent (the brake discs visible through the
body), the turntable black. Sometimes it happened, sometimes it didn't.

**Root cause:** D3D9 `Reset` returns render target 0 to the back buffer and the
Z surface to the implicit Z; the game relies on this and never sets them again.
On our Metal side the last target/Z stayed as they were. The fullscreen
transition catches Reset **mid-frame** through the device-loss path: at that
moment a draw to texture 7 without Z (NULL) was in progress (measured,
`com: Reset: bound ...` lines). Afterwards the main scene was drawn without a Z
test.

**Measurement:** the same fullscreen + 8 resolution changes scenario; the old
behavior (`NORTRESET=1`) broken 3/3 (brightness 29.97), the fix intact 7/7
(37.2). Bisection: only the target/Z reset is decisive.

**Trap (cost time):** the Bash tool's shell is zsh; an unquoted `$E` is **not
split** into words. `E="NFSFULL=1 NORTFREE=1"` became a single argument,
`NFSFULL="1 NORTFREE=1"`, so the "comparison" runs were actually the same
configuration and the bug was thought to be random. Pass switches as separate
arguments.

### Crash on the 32nd resolution change (2026-09-27) — SOLVED

When the user switched back and forth between 720p↔4K many times in free roam,
the game crashed (`sub_005b9920`, address 0x80). The log had exactly **64 Resets**.

**Root cause:** the cube texture registry (`g_cube_obj`) was limited to 64
entries, and every change (two Resets) recreates 2 cube textures: 2 + 32×2 = 66.
A cube that didn't make it into the registry is taken for "2D",
`GetCubeMapSurface` returns with the 2D argument count, the stack shifts → crash.
It shows up in a race because the environment map is drawn every frame there; in
the menu the cube is never drawn, so it didn't show.

**Fix:** a registry bitmap (COM objects are never released, addresses are never
reused → unbounded and O(1)). `MAXTEX` 4096 → 16384 (a user session had created
2504 textures).

**Reproduction:** the Quick Race recipe + 35 device losses in the race with
`RESETAT` (every 40 frames from 6000). Old: crashed after the 64th Reset with the
same function/address/registers. New: 90 Resets, no crash, the image identical to
before the Reset. Lesson: a fixed-size table for anything recreated per Reset = a
time bomb; do long-session testing with a `RESETAT` list.

Two more D3D9 rules fixed along the same path (no visible difference measured):
- Reset returns all render/sampler/stage states to their defaults.
- New texture contents are undefined: now **all** mip levels and the cube's 6
  faces are zeroed. In the menu the car environment map (256/128 cube RT) is
  never drawn, but the body samples it (ps 265, stage 0); previously the cube was
  never cleared and could carry garbage if memory was reused (verified with
  deliberate garbage: noise on the body).

---

# Car reflections (cube textures) — SOLVED

`g_CarEnvironmentMapEnable=3` was already ON in the user's saved settings; the
reflections were requested but came out wrong. The "crashes when set to 1"
situation in older notes was closed by the cube-vtable fixes of earlier sessions
(2959 frames ran without problems in this session).

**Measured actual state:** the game **draws the environment map to the 6 cube
faces separately** (`SetRenderTarget` with 6 different cube surfaces), then binds
the texture to stage 0 and samples it. On our side:

1. `CreateCubeTexture` created a **flat 2D** Metal texture → the six faces were
   written on top of each other to the same surface.
2. In the pixel shader all slots were declared as `texture2d<float>` → when a
   cube texture was bound, Metal rejected the draw
   (`validateCommonDrawErrors`).

**What was done:**
- `metal_create_texture_cube()` — a real `MTLTextureTypeCube`.
- The render target is now `texture | ((face+1) << 16)`; the render pass sets
  `colorAttachments[0].slice`. `touched[]` (the clear/load decision) is now keyed
  on the `(texture, face)` pair.
- `GetCubeMapSurface` stores the face index in the surface object (+44).
- `metal_upload_texture_face()` — `replaceRegion` on a cube texture needs a
  **slice**; also the storage can't be `Private` (the game also writes to the
  faces with `LockRect` → `_validateReplaceRegion` assert, crash on frame 9).
- **Cube shader variant:** in D3D9 `ps_1_x` doesn't declare the texture TYPE,
  the type comes from the binding. So every ps is generated twice: normal and
  `psid + PSCUBE` (256). At draw time the variant is selected if the bound
  texture is a cube. `ps_to_msl_ex(..., cubemask)` declares slots as
  `texturecube<float>` and the `SMP<k>(uv2, uv3)` macro samples `.xy` in 2D and
  `.xyz` for cubes.
- Along the way: **`com_new` now zeroes the extra area.** Object fields are
  always read assuming "0 = not set up"; reading garbage values was a class of
  bug (the cube face index was the first to catch it).

**Cost: zero.** Alternating measurement (against flat 2D with NOCUBE=1):
9.90 vs 9.89 ms, paired +0.0%.

The `dcl` sampler type is now read too (`p->smptype[]`) — measured: **none** of
the game's 34 pixel shaders declares a cube, they are all `ps_1_x`. Trap: the
type is in bits 27-30 of the **DCL token**, not the dst token (in SM2+ bits 28-30
of dst are the high bits of the register type; reading from there gives "4" for
everything).

| switch | function |
|---|---|
| `NOCUBE=1` | makes cube textures flat 2D (old behavior, for comparison) |
| `CUBELOG=1` | prints cube surface render targets and cube texture bindings |
| `SMPTYPE=1` | prints the `dcl` sampler types for every ps |

## Remaining graphics items: what could be measured and what could not

Car reflections were solved in this session (see above). For the other four
items **no objective signal was found** at the time. 2026-10-17 update: after the
Windows reference arrived (below, "Comparison with the Windows reference") the
HUD seams and light streaks were closed. Crowds and TRAX are still open.

The run report is completely clean in this scene: 0 unsupported instructions, 0
unresolved indirect calls, `[matrix] broken=0`, `mip levels: 0 MISSING`,
`vertex shader: 0 unknown`, 0 dropped frames, the Metal validation layer silent.

- **HUD row seams — CLOSED (2026-10-17): by design, not a bug.**
  The Windows reference frame (NFSRef, frame 5800, 1280x960) and ours were put
  side by side with the lap list scaled 2x: the horizontal row separators and the
  vertical seam between the number/name columns exist on Windows too, in the same
  place and with the same intensity.
- **Crowds / characters are flat sheets.** Requires skinned meshes; in this scene
  the crowd shader is NEVER loaded (no BLENDINDICES/BLENDWEIGHT in the `VSDUMP`
  output, only 2 vertex shaders are dumped). The automatic key driver can't move
  the car (the throttle key comes from the DirectInput action mapping, DIK_UP
  doesn't work), so the place where the crowd is can't be reached
  programmatically. **It has to be reached by playing.**
- **Over-coverage of light streaks — CLOSED (2026-10-17): no difference.**
  The lamp regions were put side by side with the Windows race frame (5800). Halo
  and streak length/intensity are the same: pixel counts above 200 brightness are
  664 vs 695 and 146 vs 119 in the two regions. Whether the traffic light is lit
  or not is a phase difference (it varies between Windows's own two runs too).
- **TRAX ghost rectangle — still OPEN.** The box appears in the countdown when
  the music starts (~frame 4100–4700). The Windows reference set has no frames
  between 3050 and 5000, and at frame 5000 there is no box in either run. Also the
  port runs use `NOSOUND=1`, so the music and TRAX may not be triggered. Verifying
  it needs a reference frame in that range and with sound.

**The most productive path for the next session:** screenshots of the same scenes
on Windows from the user, or driving the game to that point and taking a frame
with `DUMPFRAME`.

## Comparison with the Windows reference (NFSRef, 2026-10-17)

`tools/nfsref/` is an ASI plugin placed next to the game on Windows (Ultimate
ASI Loader / `dinput8.dll`). It does a virtual clock (frame x 16667 µs), key
synthesis through the DirectInput action mapping, and `frame_NNNNN.bmp` dumps.

The port is run under the same conditions: `NFSRES=1280x960 VCLOCK=fixed
PACE60=1`, the same `AUTOSEQ`, the same `DUMPFRAME` list. The Windows menus are
deterministic. Race frames vary between Windows's **own** two runs too (loading
time, RNG): brightness Y 74.8 vs 67.1. In races, compare the presence of visual
elements, not pixel differences.

Real bugs found and fixed:

1. **Cube texture face upload** (`com.c`, commit dbd21e0). `UnlockRect` ignored
   the face on a cube texture and wrote everything to face 0.
   `GetCubeMapSurface` didn't record the face index on the surface. This is why
   the car reflection in the menu was wrong.
2. **Offscreen targets were cleared on their first use in a frame** (`metal.m`,
   the load action of the main pass). In D3D9 a render target is **preserved**
   across frames; only an explicit `Clear` clears it. The game's glow chain is a
   feedback loop: RT4 is multiplied by the tint (0.47, 0.35, 0.67) every frame
   and added on top. Since the port gave `MTLLoadActionClear` every frame, the
   feedback was reset. Result: the pink/purple halos and lavender reflections
   were missing. Now a target is cleared only if this draw is an explicit color
   `Clear`, and with its color; otherwise `Load`. `OFFCLEAR=1` brings back the
   old behavior (for A/B).
3. **Half pixel: the D3D9 pixel center is at the integer, in Metal at +0.5**
   (`metal.m`, `setViewport` in the main pass encoder). The game offsets its
   fullscreen quads by −0.5 according to the D3D9 rule. Without an offset, in
   Metal this had two consequences:
   - Every post-process pass sampled **between** texels and the bilinear filter
     added half a texel of blur in every pass. There are several passes in the
     chain; the result is a softening of the whole image.
   - The last row and column were never written. In the quarter-resolution glow
     target this was a 4-pixel darkening at the bottom and right edges after
     upscaling (in a race the bottom row 96 vs 49).

   Fix: viewport origin (0.5, 0.5). This gives the D3D9 coverage and
   interpolation rule exactly without adding a shifter. The port's own blit
   quads are in a separate encoder and are not shifted. `HALFPX=0` is the old
   behavior.

   The measurement shows the sharpness (mean |gradient|) of frames with edges
   and a post-process chain now matches Windows:

   | frame | Windows | port old | port new | pixel difference old → new |
   |---|---|---|---|---|
   | 1300 (2D, no chain) | 1.31 | 1.31 | 1.31 | 1.09 → 0.10 |
   | 1650 menu | 3.22 | 2.51 | 3.20 | 6.79 → **0.70** |
   | 2000 menu | 2.98 | 2.27 | 2.98 | 6.62 → **0.31** |
   | 3050 menu | 3.16 | 2.57 | 3.14 | 6.75 → **0.44** |
   | 5800 race | 4.39 | 2.96 | 4.39 | 19.0 → 18.3 |

   Earlier I had thought the ~6.7 difference in the menu was "the angle of the
   car on the turntable". **That was wrong**: the whole difference was this blur.
   The difference remaining in races is of the same kind as the difference
   between Windows's own two runs (different opponents, a different moment of
   lighting); the sharpness matches exactly there too.
   (Note: the name `HALFPX` had been tried earlier for "lines cutting across the
   screen" and found to have no effect. That bug was something else; this switch
   was added again and is on by default.)

As a result the menu mean colors match exactly (R G B):

| frame | Windows | port |
|---|---|---|
| 1650 | 45 43 39 | 45 43 39 |
| 2000 | 44 42 37 | 44 42 38 |
| 3050 | 43 41 40 | 43 41 40 |

**Measurement trap — `DUMPTEX` alpha blending:** the old texture dump drew the
texture with the alpha-blended `g_pso`. Targets with alpha < 1 (RT4/RT5)
therefore looked **black**, which led to a wrong "t2 is black" theory. The dump
now takes a raw blit copy and also prints alpha statistics. `DUMPOLD=1` gives the
old path.

New diagnostic switches:

| switch | function |
|---|---|
| `PSMSL=<id>[,..]` | prints the ps disassembly + generated MSL (cube variant `id+256`) |
| `DUMPFACE=<0..5>` | which face `DUMPTEX` should dump for a cube texture |
| `SKIPDRAW=a-b,n` | draw skipping now accepts ranges and lists |
| `OFFCLEAR=1` | clear offscreen targets every frame (old, wrong behavior) |
| `HALFPX=0` | turn off the D3D9 half-pixel offset (old, blurry behavior) |
| `DUMPOLD=1` | old (alpha-blended) texture dump |

## macOS application bundle (`./pack.sh`)

The port is now packaged as a double-clickable `.app`: `./pack.sh` produces
`out/NFSU2.app` (44 MB), `out/NFSU2-dist/`, and with `--zip` a 15 MB
`out/NFSU2.zip`. `run.sh` stays as it is as the development path.

**`runtime/bundle.m`** gathers the in-bundle behavior. Every path first checks
"are we running from a bundle" (`_NSGetExecutablePath` ->
`.../X.app/Contents/MacOS`) and returns immediately if not — running from the
terminal didn't change in any way.

* **Settings.** The variables `run.sh` exports (`WINDOW`, `FIXMAT=1`,
  `FIXMAT_MODE=3`, `NOMOVIES`, `SNDSYNC`, `GPUVS`) are set automatically inside
  the bundle. `ALARM` is deliberately **0**: `boot.c`'s 20 s diagnostic timeout
  would take the game out of the user's hands.
* **Game data.** In order: `NFSU2_DATA`, a previously selected path
  (NSUserDefaults), inside the bundle, next to the `.app` and its parent
  directory, a few candidates in the home directory; if not found the user is
  asked with an NSOpenPanel and the choice is stored.
* **Log.** When opened from Finder stderr goes to `/dev/null` and no
  diagnostics are left at all; it is written to `~/Library/Logs/NFSU2/nfsu2.log`
  (the previous run is `nfsu2-previous.log`). If stderr is a tty it is left
  alone.

### Distribution: two silent blockers

Both are of the kind that look like they "work" but never open the game on
another machine:

1. **`minos 26.0`.** If `-mmacosx-version-min` isn't given, the binary is stamped
   with the OS version of the machine that compiled it. On anything older than
   macOS 26 the application never starts ("requires a newer version"). The floor
   was lowered to **11.0**; none of the APIs used produce an availability
   warning.
2. **`-mcpu=native`.** Since this machine is an M1 Pro it resolves to
   `apple-m1` and no problem was visible; if the port is compiled on a newer Mac,
   the binary produced crashes with an illegal instruction on an M1. The `MCPU`
   default was made **`apple-m1`** (measured difference on M1 is zero; the old
   behavior with `MCPU=native`).

Both are defaults in `build.sh` and `link.sh`. Since `$OPT` got longer,
`xargs -I{}` in `build.sh` gave "command line too long" again; `$OPT` is no longer
embedded in the command line, it is passed through the environment and expanded
in the inner shell.

**Gatekeeper.** An ad-hoc signature is not notarization: `spctl` gives
**rejected** for a downloaded bundle (measured). The user needs to run
`xattr -dr com.apple.quarantine`; `README.txt` gives this as the first step. With
an Apple Developer account, `SIGN_ID="Developer ID Application: ..." ./pack.sh`
signs with a real certificate.

**Data NEXT TO the bundle, not INSIDE it.** `--with-data` copies the game into
the folder next to the `.app`. Putting it inside the bundle would break two
things: saved games (`memcard/`) can't be written into a signed bundle, and
writing invalidates the signature. The application already scans next to the
`.app`.

### Personal single-piece app (`./pack.sh --embed`)

`./pack.sh --embed` (and `--embed --hd`) embeds the game data INSIDE the bundle
(`Contents/Resources/Game`) and installs it under `out/personal/` (~1.5 GB;
`cp -Rc` APFS clone, takes no extra space on the same disk). **It cannot be
distributed because it contains the game itself**; the script refuses to run
together with `--zip` / `--with-data` and doesn't produce a distribution folder.
MOVIES (no decoder) and the Windows plugins (SCRIPTS, dinput8.dll, server.*) are
not embedded.

Writing inside a signed bundle breaks the signature, so a **write root** was
added to `fileio.c` (`fileio_set_write_root`): writes / creates / deletes go to
`~/Library/Application Support/NFSU2 Saves` (until 2026-10-25 it was called
"NFSU2 Kayitlar"; the runtime renames the old folder once on first start, so
existing saves keep working), reads look
there first and then in the game root, and directory listings are the union of
both. Active only when the data is found inside the bundle (or when
`NFSU2_SAVEDIR` is given); running from the terminal and a bundle with its data
outside stay on a single root as before. Embedded data comes before the saved
folder preference.

Verification: the bundle was copied outside the project tree and driven into a
race with `AUTOSEQ` (6000 frames, 0 unresolved calls; the 12 files that couldn't
be opened are the same as in a normal run: movies and the game's own probes).
The game created its save folder in the external directory; after the run
`codesign --verify` is valid, no file inside the bundle changed. Real profile
save/load was not tried.

### Behaviors needed to be an `.app`

Since `metal_run_main_loop` drives the event loop by hand, the work AppKit
normally does inside `[NSApp run]` wasn't being done:

* **There was no menu bar** -> Cmd-Q, Cmd-H, Cmd-M didn't work. A menu was
  added; also, keys with Cmd now go to `[NSApp sendEvent:]` instead of the game.
* **`[NSApp finishLaunching]` wasn't called** -> AppleEvent handlers were never
  registered, the application didn't respond to the "Quit" command (Dock > Quit,
  logging out), and the process hung.
* **The run loop wasn't pumped** -> only `nextEventMatchingMask` was called;
  AppleEvents come from run loop sources, not the event queue.

`applicationShouldTerminate:` returns **`NSTerminateCancel`**: `g_should_quit` is
set, the draw loop exits, `main()` writes the report and terminates.
**`NSTerminateLater` was tried and is unusable** — while waiting for the reply
AppKit drives its own run loop, our loop never returns, and shutdown hangs. The
only side effect of Cancel is a warning on logout; the application quits by
itself right after anyway.

A real bug that came up along the way: **the mouse Y was hard-coded as
`720.0 - pt.y`.** When 1080p was selected (since it became selectable from the
menu), the cursor fell off the screen. It is now tied to the window's content
height.

Also: without `NSHighResolutionCapable` the window would be drawn at 1x and
scaled up by WindowServer; in fullscreen `kCAGravityResizeAspect` keeps the
aspect ratio (a 5:4 image isn't stretched on a widescreen).

**Measurement.** After a full rebuild with portable flags, 900 frames 7.91 s —
the same as before the change (7.9 s). On the title screen 2077 frames / 17.6 s
(118 fps), virtual clock 0.998, 0 unsupported instructions, 0 unresolved
indirect calls. 439 ISA + 15 function tests pass.

## Retina resolution: blurriness and `NFSNATIVE`

**Symptom.** With the game running at its highest mode (1280x960) the image was
noticeably blurry compared to Windows screenshots; most of all in text,
headlights and wheel spokes.

**Measured root cause.** The window content was made as many POINTS as the game
resolution, and `CAMetalLayer.drawableSize` the same number of PIXELS;
`contentsScale` stayed 1.0. Measured with a separate test program on a 14"
MacBook Pro (1512x982 points, 2x): since 1280x960 points of content don't fit on
the screen it is cropped to 1280x918, the backing area becomes 2560x1836 pixels
and the 1280x960 surface is bilinearly upscaled ~1.91x into it — every pixel was
resampled and softened. (Running from the build or from the `.app` made no
difference; the bare binary also gets a 2x-scaled window.)

**Solution.**
* `win_apply_size` (metal.m): window content = pixels / screen scale,
  `contentsScale` is set explicitly. If it doesn't fit on the screen it is SHRUNK
  keeping the aspect ratio (never enlarged). Log line:
  `drawing WxH pixels, window wxh points -> 1:1, NO resampling`.
* `metal_native_size` + `res_pick` (com.c): the resolution the game asks for is
  raised to the screen's real pixel count by choosing an EXACT multiple of the
  reduced ratio. On this Mac 1280x960 -> **2448x1836**, `aspfix=1.0000`; the
  clip-space correction doesn't kick in and geometry isn't touched. The
  post-process render targets go to the same size through the existing NFSRES
  path. `NFSNATIVE=0` turns it off, `NFSRES` pins it, `NFSMAXPIX` (default 6 MP)
  is the upper limit on 5K/6K screens.
* CreateDevice and **Reset use the same rule**; Reset used to pass the raw value
  through (when the resolution was changed in-game the correction was undone). If
  there is no override, `g_req`/`aspfix` are reset.
* `GetDisplayMode` reports the size the game chose, not the real size: the
  settings screen looks this value up in its own table, and 2448x1836 isn't
  there.
* Mouse: the front-end cursor is driven by DirectInput RELATIVE motion
  (`GetCursorPos` is never called, `GetClientRect` is a fixed 1280x720). Since
  the window is now independent of the game resolution, the deltas are scaled to
  the area the image covers; the absolute mapping was also converted to the
  game's space. Not tested with a real mouse.
* The placeholder window (before CreateDevice) opens natively at the 1280x960
  aspect ratio; in the most common case the window doesn't jump.

**Trap: the title bar estimate.** In the first version the usable height was
computed with an estimate (menu + 28-point title); on a notched screen the title
bar is **32 points**. The 3-point difference cropped the window 921 -> 918 and
resampling came back. Now the area is measured on the main thread with AppKit
(`scr_cache`; `metal_early_init` is called before the guest thread).

**Measurement (M1 Pro, 120 Hz screen, highest graphics settings).** Quick Race
(Resort Loop) was entered with `AUTOSEQ`; the flow is deterministic, the two runs
go through the same screens frame by frame:

| section | native off (1280x960) | native on (2448x1836) |
|---|---|---|
| menus, frames 3000-3499 | mean 8.34 ms, p95 8.48 | mean 8.34 ms, p95 8.50 |
| race, frames 5000-6999 (~1300 draws/frame) | mean 10.47-10.64 ms, p95 11.5-11.8 | mean 10.56-10.77 ms, p95 11.6-11.9 |
| GPU busy (whole run) | mean 2.00 ms, max 3.95 | mean 4.88 ms, max 9.35 |

The frame time is set by the CPU (lifted code); 3.66x the pixels didn't lengthen
the frame. **Measurement trap:** in the first comparison 170-220 ms spikes were
seen in native — all of them coincided with `DUMPFRAME` frames (PNG writing,
scales with the pixel count). Leave dump frames out of measurements.

**Limits.** The UI fonts are fixed-resolution textures: they get sharper since
the second resampling is gone, but they can't exceed the texture's own detail.
Not measured on Macs with weaker GPUs; if GPU time exceeds the frame budget it
can be lowered with `NFSNATIVE=0` or `NFSMAXPIX`.

**Test note.** Test windows take focus; the game's settings are in
`~/.nfsu2_registry`. During this work `g_RacingResolution` once slipped from 3
(1280x960) to 4 (1280x1024) and couldn't be reproduced in controlled runs (most
likely cause: real keyboard input reaching the test window). Back up the file
before multi-run tests.

## Real session recording (`PERFLOG`, `tools/session.sh`)

Automated runs (the `AUTOSEQ` race) are for exact A/B; this recording is for
seeing **where and when** load builds up in a session the user plays themselves
(fast driving, effects, crashes, menus).

```bash
./tools/session.sh            # standard build; ./tools/session.sh hd
python3 tools/session_report.py ~/nfs_sessions/<date>   # -> <dir>/report.md
```

The script starts powermetrics (CPU/GPU watts, thermal pressure; password once)
and the game with `open --env PERFLOG=<dir>`, and stops it when the game exits.
`runtime/perflog.c` writes:

| file | contents |
|---|---|
| `guest.csv` | per guest frame: wall/CPU time, frame limiter sleep (idle margin), Present wait, COM/Win32 calls, VS/PS/PSO compiles, texture creates/uploads |
| `render.csv` | main thread frame interval, `nextDrawable` wait, encoding time, draws/vertices, dropped frames |
| `gpu.csv` | GPU time per frame |
| `seconds.csv` | process energy (`ri_energy_nj`, P cores), instructions/cycle (IPC), thermal state, memory |
| `thread.csv` | per-thread user/system time once a second |
| `samples.bin` | guest every 1 ms, main thread every 4 ms: PC + frame chain (`thread_suspend` → state → `resume`; no locks/malloc while suspended) |
| `screens/` | a ~480 px JPEG every 2 s (GPU copy, written in the background) |
| `temperature.csv` | CPU/GPU temperature, both fan speeds, SMC system power once a second (`tools/smcread.c`, no sudo needed; `session.sh` compiles it) |

Overhead (120 fps race, `ps -M`): guest thread +1.6%, process +5% (mostly the
sampler itself); fps, the longest frame and the variance don't change.
`task_gpu_utilisation` returns 0: GPU watts come only from powermetrics.


## GPU energy and memory traffic (2026-10-18)

Goal: lower watts while keeping 120 fps and the original image. In a race
(120 fps) the GPU is ~4.9 W, more than half of the system's ~8.8 W.

### Measurement: why a new method was needed

- **GPU ms/frame is not a measure of work.** macOS adjusts the GPU clock to the
  load (DVFS): when work drops the clock drops, and the same frame can take
  LONGER (5.3 → 5.8 / 7.6 ms was measured for a change that cut memory traffic).
- **The energy of separate runs can't be compared.** Three runs with the same
  settings gave 30.8 / 34.9 / 40.0 mJ/frame (DVFS state + the rest of the
  system).
- **Counters without sudo** (IOReport, `runtime/metal.m` `ior_read`):
  - `Energy Model / GPU Energy` (nJ): the modelled energy of the GPU core
    (powermetrics's "GPU Power"). **Does not include DRAM.**
  - `AMC Stats / Perf Counters`: bytes per agent at the memory controller.
    `GFX RD/WR` = the GPU's reads/writes to the memory system, `GFX DCS RD/WR` =
    the part of that going to DRAM. A measure of work independent of DVFS.
  - SMC `PSTR` (total system power) can also be read without sudo, but it updates
    once a second; coarse for A/B (tool: scratch `smc.c`, not permanent).
- **In-run A/B: `ABTEST=NAME[,N[,SKIP]]`** (in the `GPUWIN` window). The window
  is split into N-frame slices (default 120; short slices pair neighboring slices
  better, `32,4` was used in the measurements). The feature is on in even slices
  and off in odd ones. The first SKIP frames of a slice are dropped (the previous
  mode's work in the queue). The difference of neighboring slice pairs is
  reported as mean ± standard error. A feature asks for its own switch with
  `abt_off("NAME")` (`SCRREDIR`, `ALFAVAR`). The `GPUWIN` report now also gives
  energy and MB/frame.

```bash
# race, real resolution, locked 60 Hz (AUTOSEQ: the Quick Race recipe, throttle held)
env FIXMAT=1 FIXMAT_MODE=3 NOMOVIES=1 SNDSYNC=1 GPUVS=1 WINDOW=1 ALARM=0 \
    NOFOCUS=2 NOSOUND=1 NFSFULL=0 MOUSEAT=5,5 PACE60=1 \
    AUTOSEQ="1000:0x1C,1350:0x1C,1700:0x1C,2050:0x1C,2400:0x1C,2750:0x1C,3100:0x1C,3700:0x1C,4000:0x1C,4700:0xC8:5300" \
    QUITFRAMES=10000 GPUWIN=6200,9800 ABTEST=ALFAVAR,32,4 \
    out/nfsu2_boot "<SPEED2.EXE>" 2>&1 | grep -a "ABTEST\|WINDOW"
```

### Results (standard build, race, 3024x1836, 60 fps, 56 slice pairs)

| feature | GPU energy mJ/frame | GPU read / write MB/frame | DRAM read / write MB/frame | decision |
|---|---|---|---|---|
| ALPHA TEST VARIANT (`ALFAVAR`) | **−6.32 ± 0.48 (−17%)** | −2.8 ± 1.1 / −0.3 ± 0.8 | −1.8 ± 1.7 / +0.8 ± 1.7 | on by default (HD: −27%) |
| SCREEN REDIRECT (`SCRREDIR`) | −0.02 ± 0.88 | −10.7 ± 1.1 / −10.1 ± 0.8 | **−20.4 ± 1.9 / −21.0 ± 1.8** | on by default |
| PASS LOAD (DontCare / scratch Z) | +0.6 ± 0.8 | +0.9 ± 1.1 / +0.6 ± 0.6 | +1.6 ± 1.8 / +1.2 ± 1.4 | rejected, code removed |
| POST-PROCESS CHAIN (`FBZINCIR`), 2 runs | −1.07 ± 0.53 / +2.39 ± 1.44 (mean ≈ −0.7 ± 0.5) | −11.1 / −9.6 read, −5.8 / −4.2 write | **−12.6 / −9.8 read, −12.9 / −10.8 write** | on by default |

GPU time (with DVFS, for information only): `ALFAVAR` on 5.28 / off 6.08 ms/frame.

### ALPHA TEST VARIANT (`ALFAVAR`)

Every translated fragment shader did the alpha test itself:
`if (res.a < P.aref) discard_fragment()`. With the test off (aref 0) no fragment
is discarded, but because the shader contains `discard`, the Apple GPU doesn't
treat the draw as opaque and **hidden surface removal (HSR) turns off**: fragments
that will be covered later get shaded too. Now every ps (and the fixed-function
`fmain`) is specialized twice with a function constant (`kAT`); a draw with aref
0 is drawn with the variant without `discard` (bit 52 in the pipeline key). A
difference could only arise for a fragment producing `res.a < 0`; in D3D9 that
fragment is written anyway when the alpha test is off, so the new variant is more
faithful to D3D9.

Verification: `ALFAVAR=2` counts the fragments that "would be discarded" in the
test-free variant with a counter on the GPU (fragment buffer 6) and paints them
magenta. Result over a whole run (9000 frames, menu + race): **0 fragments**.
`ALFAVAR=3`, the positive control (alpha-tested draws also with the diagnostic
variant), counted 2.98 billion fragments.

**HD build:** the gain is larger (−30.1 ± 1.7 mJ/frame, −27%). In HD the
`ALFAVAR=2` counter first found a difference of 12.6 million fragments. Counting
per shader (a counter array indexed by ps id) showed the source: almost all of it
the fixed-function `fmain`, caused by negative vertex color alpha. D3D9 clamps the
vertex color to [0,1] (below, COLOR CLAMPING); with clamping, HD gives **0
fragments** as well, on in both builds.

### COLOR CLAMPING (`RENKSAT`)

In D3D9, ps_1_1 – ps_2_0 see the vertex color (v0/v1) clamped to [0,1]. The
Microsoft documentation says "8 bit unsigned, range (0, +1)" and "saturated
before use by the shader". Our vertex shader passed oD0/oD1 through without
clamping. A diagnostic counter showed **1.18 billion fragments** in race + menu
arriving with colors outside [0,1]; mostly car paint (ps 265, cube variant), then
ps 7 and `fmain`.

Clamping is done in two places:
- per vertex: `vsemu.c` `vs_to_msl` and the CPU path in `com.c`, vs < 3.0. The
  color interpolators of the hardware of the time already carried [0,1].
- per fragment: the `kCS` function constant; for floating-point overflows and
  missed paths.

Windows reference (1280x960, same key sequence): menu frames unchanged, the
distance to Windows the same as before (0.1 – 0.5). Since race frames vary from
run to run on Windows, there is no comparison there. This is a fidelity fix: it
follows the D3D9 contract and made the alpha variant exact in HD. `RENKSAT=0` is
the old behavior.

**Looking for magenta in the image is blind in races:** the post-process color
table (ps 26/28, LUT) turns magenta into another color; magenta never showed up
outside the menu frame. That is why the counter exists.

### SCREEN REDIRECT (`SCRREDIR`)

The race (and menu) frame draws the scene to the screen, then copies it at the
same size to #2 (the post-process reads #2), and at the very end rewrites the
screen from scratch with a fullscreen quad. The copy is 22 MB read + 22 MB write,
0.59 ms/frame (PASSPROF). When the conditions are met, the scene is drawn
directly into #2 and the copy is skipped. Conditions: the copy is the first blit
reading the screen; its target has the same size and format as the screen;
before the copy #2 is neither sampled nor a target; after the copy the first draw
touching the screen completely overwrites it (the FULLSCREEN COVERAGE rule). In
this case #2 receives exactly the content the copy would give: the same draws,
size, format, Z.

Verification: `SCRREDIR=2` clears the first screen pass after the copy with
magenta. If the coverage is wrong, magenta leaks into the final image; this test
isn't blind because it checks what is written to the screen AFTER the
post-process. 0 magenta in 16 frames. The image was also checked by eye. In HD
it is off, since there the screen is multisampled and has a G-buffer.

The gain is in DRAM: −41 MB per frame (−4.9 GB/s at 120 fps). GPU core energy
didn't change: the copy is memory work, not shader work.

### UV SCALE (`UVFIX`) — post-process blur at native resolution (2026-10-19)

With NFSRES / Retina the game draws believing it is at 1280x960, while the
fullscreen targets (#2/#3/#4, screen) are 3024x1836. The post-process quads carry
the mapping `u = (x + 0.5) / 1280` from the D3D9 half-texel rule (measured: u
0.0003906 .. 1.0003906 = 0.5/1280, v 0.0005208 .. 1.0005208 = 0.5/960). On the
real target this shifts the pixel center from the texel center by (0.68, 0.46)
texels. Every full-resolution pass shifted the image and softened it with
bilinear filtering; the main image goes through 4 passes (the color table chain
+ the copy to the screen). At 1280x960 (the Windows comparison resolution) the
mapping is exact, which is why this never showed up in any comparison.

Fix: the mapping the game intends is set up at the real size. The vertex shader
adds `0.5/Wreal − 0.5/Wgame` to the coordinate set matching the pattern, i.e.
`u = (x + 0.5) / Wreal`. Conditions:
- the quad covers the target completely (NDC ±1);
- the target is scaled (game size ≠ real);
- the set's vertex UVs follow exactly this pattern.

Sets that don't match the pattern (multi-tap blur offsets, 640x480 targets) are
left alone. ~2.5 draws per frame.

Result (same key sequence, native resolution; sharpness = mean brightness
gradient):

| frame | before | after |
|---|---|---|
| 1650 (3D menu, car) | 1.367 | 1.637 (+20%) |
| 2000 | 1.259 | 1.542 (+22%) |
| 3050 | 1.359 | 1.590 (+17%) |
| 5000 / 6200 (race) | 2.33 / 2.19 | 3.17 / 2.99 |
| 1300 (2D menu, no post-process) | 0.569 | 0.569 |

By eye: the carbon texture of the hood, the wheel spokes and the brake disc holes
are now sharp. `UVFIX=0` is the old (blurry) behavior.

### ORDERED TEXTURE UPLOADS (`TEXORD`) — a texture race (2026-10-19)

The game rewrites some textures every frame: the post-process color tables #8 and
#9, 5467 uploads in 5467 frames (`TEXUPSAY=1` counts them). The upload went to
the texture immediately with `replaceRegion`. While the guest thread records
frame N+1, the main thread is encoding frame N or the GPU is drawing it. So frame
N sometimes saw frame N+1's table, or even a half-written table. In D3D9 every
draw sees the content as of the moment it was submitted.

The symptom was: even encoding the same draw twice in the same frame gave
different results (0.6 M pixels in the post-process chain verification, at most
2/255).

Fix: the second and later uploads of a texture go into the draw list as ordered
records. The data is copied into the frame's upload area (double buffered, 8 MB),
and during encoding it becomes a blit (buffer to texture) ordered with the draws.
The first upload (loading screen, a texture not used yet) is immediate as before.
The pending uploads of a skipped (unencoded) frame are applied immediately before
the buffer is reused. ~2.3 uploads per frame, 0 overflows. After that, encoding
the same draw twice gives **0 pixel difference**. `TEXORD=0` is the old behavior.

### POST-PROCESS CHAIN (`FBZINCIR`) — three passes in one (2026-10-19)

The post-process applies a color table with three fullscreen passes:
`#3 = ps28(#2)`, `#2 = ps27(#3)`, `#3 = ps26(#2)`. Each step's input is the
previous step's output at the same pixel. Measured (`ABTEST=SKIPPS` +
`SKIPPS=26,27,28`): the three passes are ~16% of GPU energy and ~33 MB/frame of
DRAM.

The chain is now drawn in a single pass that binds both targets at once. Each step
reads its input from tile memory instead of a texture (`[[color(n)]]`, in draw
order on the Apple GPU). A shader variant (`vsemu.c` `ps_to_msl_fb`) replaces the
stage 0 sample with the tile memory input; it is compiled in the background, and
the old path is used until it is ready.

Conditions (per record):
- a single quad covering the target completely;
- no Z, blending or alpha test;
- the stage 0 texture is the other chain target, with the same size and format;
- UV 1:1 to the texel center (after UV SCALE);
- the ps samples stage 0 in a single place with `TCRD(0)`;
- the other stages don't read the chain targets;
- consecutive records alternate between the two targets.

Verification: `FBZINCIR=2` draws the chain through the old path, draws the merged
path with the same input into scratch textures, and compares the two pixel by
pixel on the GPU. Result: **0 differences** in 5454 chains, both in the final
output and in the intermediate target. `FBZCTRL=1` runs the helper with the normal
pipeline, `FBZCTRL=2` runs it twice and compares it with itself; these two are the
determinism test. Before the texture race was fixed, the result showed differences
of 1–3/255. The source of those differences was not the chain but the race above.

Gain: DRAM −23 MB/frame (two runs); GPU core energy within measurement noise
(mean ≈ −0.7 ± 0.5 mJ/frame). ~1.3 chains per frame (race + menu).

### Rejected

- **PASS LOAD:** in a pass that starts with a draw covering the whole screen,
  color `Load` → `DontCare`; in a pass that doesn't use Z, a scratch depth instead
  of the real Z. It was correct by the poison test, but it has no effect on any
  measure (table). The Apple GPU already skips the load of a fully covered tile
  with HSR, and doesn't move untouched Z either. The code was removed; the
  coverage computation (`com.c` → `metal_set_next_cov_clip`) remains for SCREEN
  REDIRECT.
- **Z STORE:** not storing Z with `DontCare` after its last use. The game opens
  the device without `D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL` (presentation
  parameters: swap 1, Z D24S8, flags 0; now logged at startup). So the implicit Z
  must be preserved across frames; Z cleared later within the frame came out
  0/frame. Not applicable, code removed.

New switches:

| switch | function |
|---|---|
| `ABTEST=NAME[,N[,SKIP]]` | in-run A/B (above) |
| `SCRREDIR=0/2` | turn off the screen redirect / poison test |
| `ALFAVAR=0/2/3` | turn off the test-free variant / diagnostic with counter (per ps; also counts vertex colors outside [0,1]) / positive control of the diagnostic |
| `RENKSAT=0` | turn off vertex color clamping (old, violates D3D9) |
| `UVFIX=0` | turn off the half-texel fix on scaled targets (old, blurry) |
| `TEXORD=0` | write re-uploaded textures immediately (old, racy) |
| `TEXUPSAY=1` | upload count per texture at exit (at most 12) |
| `FBZINCIR=0/2` | turn off the post-process chain merge / exactness check |
| `FBZCTRL=1/2` | (with `FBZINCIR=2`) run the helper with the normal pipeline / compare it with itself |
| `SKIPPS=…` + `ABTEST=SKIPPS` | measure the cost of those ps within a run (skips only in OFF slices) |


## 2026-10-20 – 10-24: heat, audio pitch, career saves

### Capacity measurement: the GPU's real share

In sessions the GPU looked "98% full", but in a capped (120 fps) run this is a
utilization that includes the DVFS clock. Three new switches for deterministic
measurement: `VCLOCK=8333` (game clock fixed at 8.333 ms per frame), `NODRAW=1`
(doesn't present to the screen, doesn't wait for vsync), `LIMFREE=<frame>` (the
game's 120 fps limiter is off after that frame; don't use it in menus). The GPU
runs 100% at its highest clock, and busy time per frame repeats within ±0.2%. The
`WINDOW` line writes the GPU utilization from IOReport and the "ms/frame @1296 MHz"
value; `PASSPROF_CSV=<path>` gives per-frame pass times. A feature's cost is
measured within the same run with `ABTEST=<NAME>,32,4` (neighboring slice pairs).
Trap: in ~1/3 of Quick Race capacity runs the car stays slow (the
`mid-scene copy` count should be ~5500 instead of ~1700) — those runs are invalid.

### HD: why it is always hot (86 °C)

Temperature and fans are now measured (`temperature.csv`). Finding: under load the
fans stay fixed at ~2300/2500 rpm, and the temperature follows the GPU power
within seconds; as long as the GPU is at its highest clock for 50-80% of the
drive, 86-90 °C. Quick Race capacity breakdown (8.40 ms/frame at the highest
clock, limit 8.33):

| item | ms/frame |
|---|---|
| MSAA 2x (cost without G-buffer 0.9) | ~3.0 |
| ↳ motion blur split (MS color+Z+G store/load, +176 MB/frame) | ~1.7 |
| ray tracing (kernel 2.0) | ~2.7 |
| the game's own full-resolution post-process (present in the standard build too) | ~2.3 |

The standard build is 3.0 ms/frame in the same scene; in a real session, racing,
CPU 2.82 + GPU 3.62 = **6.45 W** (16 October v1: 9.17 W), max 70 °C, the fans
never spun. The heat is the cost of the MSAA + ray tracing HD adds; the user chose
not to give up image quality (post-process anti-aliasing instead of MSAA was
rejected).

### HD: ray tracing static store, spatial groups

The static store had moved from a single structure (a +5 ms spike every 30
frames) to 8 time-ordered slots: the slots overlapped in the same region, the ray
kernel +48%, frames over 12 ms in a session 1.5% → 5.1% (regression). Now
(`metal.m` "STATIC STORE: SPATIAL GROUPS + NEW"): the store has two halves
(ping-pong), the live ones are split into at most 12 spatial groups (~16K
triangles); on merging, a GPU copy + ONE group build per frame, and a switch in a
single frame once all are done. No merge spike. Also a **position check**: when
copies of the same model (poles, palms) were drawn in a different order, the
refit of the instant structure broke the tree (kernel 1.5 → 15-25 ms); the world
translation from the draw's VS matrix is compared with the last full build, and
if it moves more than 4 units, a rebuild from scratch (`HD_RT_REFIT_MOVE`).

### HD: cars lost MSAA during motion blur

When driving fast the game draws the distant scene, copies the screen, blurs and
blends it, and THEN draws the cars and the near scene. HD counted this first copy
as the end of the scene: cars without MSAA and without ray tracing. `metal.m`
"MID-SCENE COPY": on such a copy the scene continues multisampled (`HD_MBFIX=0`
is the old behavior). It costs ~+1.5-2 W of GPU when driving fast; the user chose
to keep the fix. Tried and rejected: merging the reflections early at the split
and not carrying G (`ABTEST`: +7.5% work).

### HD: separate G-buffer pass (`HD_GSEP`)

The scene MSAA pass is now without a G-buffer; reflective draws there use the
game's own shader (exactly the same as the variant's c0). When the scene ends, a
single-sampled G pass redraws only the opaque reflection variants (Z twin, `<=`
test, slope bias -1). `ABTEST=GSEP`: GPU work -5.5%, energy -5.9%, DRAM -28
MB/frame; the validation layer is clean, the difference in the menu ≤ 4/255. The
only difference: on-road decals (the start line) no longer get ray-traced
reflections. Measured and left OFF by default: `HD_RT_FAR` (ray distance limit,
-0.1 ms) and `HD_RT_BMIN` (reflection weight threshold, -0.8% at an invisible
threshold).

### Audio: music played 3% slow/fast for 40-50 s (2026-10-22)

The user: "every now and then something happens, and from that moment the music
flows slower" (on the speakers too). `SNDMUTE=1` (new: mixing and the servo run,
silence goes to the device) + `SNDCSV` over a 3-minute Quick Race: on every
load/stutter the game's write lead settles at a new level, and the write cursor
servo in `audio.m` took this for an error and pushed the rate to the clamp (±3%),
staying there for 40-50 s; in 95% of the calls the rate deviated by more than ±1%.
Why it didn't close: the game chooses where it writes based on the REAL playback
cursor we report (`GetCurrentPosition` → `audio_cursors`), so when the playback
rate changes, production changes too. The servo is OFF by default (`SNDPLL=1` is
the old behavior): deviation 0.56% (only at stutter moments), silent reads the
same, cursor overruns 4 → 0.

### Career: profile name and saving (2026-10-24)

Until then no profile could ever be saved; the chain was five separate Win32 gaps:
text input arrives with `WM_CHAR` (we didn't generate it; `metal.m` character ring
→ `PeekMessageA`), `GetFullPathNameA` was missing, `FindFirstFile` always reported
the attribute as a file (profiles are looked up as FOLDERS; the wildcard pattern
was ignored too), `FileTimeToLocalFileTime`/`FileTimeToSystemTime` were missing (a
crash when a save existed), `RemoveDirectoryA`/`GetTimeZoneInformation` were
added. Saves are in `~/Library/Application Support/NFSU2 Saves` for every
bundle; in tests ALWAYS use `NFSU2_SAVEDIR=<scratch>`. Test:
`AUTOTEXT='1650:\b\b\b\bTalha\n'` types a name, `FILEOPLOG=1` logs file operations.

### System shortcuts and the Space switch freeze

The occasional 0.54 s freezes aren't the game's: Ctrl+arrow switches the macOS
desktop (`FullscreenGameMode=0`, visible in the system log). While the game is in
front the Space / Mission Control shortcuts are disabled
(`SLSSetSymbolicHotKeyEnabled`), and re-enabled when it goes to the background and
at exit; since this is global and stays disabled after a SIGKILL, a guard process
spawned from the same binary re-enables them on a crash (`tests/boot.c`).
Cmd+Tab, Spotlight and the screenshot shortcuts are left alone. `NFSKISAYOL=0`
turns it off.

### Capture shortcut

In game, **Cmd+Shift+D**: the next frame as PNG + `DRAWLIST` + `DRAWSUM` (to the
`PERFLOG` directory, otherwise `~/Library/Logs/NFSU2/`). For scenes that automated
tests can't produce (the end of a career race). Test hook `YAKALAKARE=<frame>`.
Open item: in career mode the post-race scene (behind the results panel) stays
black for the user; after a Quick Race drag loss the same scene is correct.


## 2026-10-25: first public feedback (Reddit)

Five reports from a player on another Mac. Findings and fixes:

### First-run settings were all at minimum (pop-in, no crowds)

On an empty registry the game runs its own hardware detection once
(`sub_005ced70` → `sub_005ceca0` → `sub_005cde50`): score = CPU MHz bracket × 0.8
+ GPU table score × 0.2, truncated → `g_PerformanceLevel`, which picks the preset
for every graphics option. CPU detection (`sub_005cdd00`) first checks for CPUID
the classic way (pushfd, flip EFLAGS bit 21, popfd, pushfd, compare).
`eflags_get/eflags_set` (`cpu.h`) did not keep bit 21, so the test always said
"no CPUID", the MHz stayed 0, the level came out 0 and EVERY option was saved at
its minimum (world detail 0 = visible building of the world ahead, no crowds,
reflections, shadows). `NotFirstTime=1` was saved too, so it never detected again.

- `g_eflags_hi` (`mem.c`) now carries the AC/ID bits through pushfd/popfd.
- `rdtsc_shim` counts real time at a nominal 3.0 GHz (it used to add a fixed
  3000 per call; the MHz measurement times RDTSC over 50 ms). `RDTSC=fixed`: old.
- `GetAdapterIdentifier` fills the whole structure: NVIDIA 10DE/0040 (a GeForce
  6800 Ultra, score 5.0 in the game's table). `ADAPTERID=ven:dev` overrides.
- The `NotFirstTime` fake is gone (`REGFIRST=1` restores it).

Result on an empty registry: level 4 (4.0 × 0.8 + 5.0 × 0.2 = 4.2; level 4 is the
highest the game ever picks by itself, 5 is manual only), world LOD 2, crowds on,
1080p row. Registries saved by the buggy detection (performance level, world
LOD, car LOD and crowds all 0, no `PortSettingsRepaired` marker) are repaired
once at load: `NotFirstTime` and the `g_*` values are dropped, so the game detects
again.

**Side effect, fixed:** the same CPUID test is in ten functions. Now that it
passes, a library at 0x72d369 picks its SSE variant (0x7337d0 instead of
0x733700), which uses `cvtpi2ps` and `shufps xmm, m128`; the lifter had neither
(9.3 million traps in one Quick Race). Both were added to `lift.py`, with a
`fuzzisa.py` template (all cases match real x86 under Rosetta), and `gen/all`
was regenerated: the only code differences were these instructions. Traps per
race: 0. The CRT's `stmxcsr` path stays unreachable (it needs SSE2, which our
CPUID does not report).

### Cinematic letterbox bars stopped at the 4:3 edges

With `HUDFIX` every orthographic draw is narrowed to the centre so the HUD keeps
its shape; the NIS letterbox bars were narrowed too and the 3D scene showed at the
sides. Orthographic draws that span the whole width but only part of the height
(the bars: y 0.75..1 and -1..-0.75) are now left at full width; full-screen
pictures (splash, loading screen) keep their 4:3 shape. `HUDWIDE=0`: old.
`ASPLOG=<frame>` logs each kind of full-width draw once.

### Quit froze the app

`ExitProcess` from a guest thread other than the main one parked that thread
and nothing told the window to close. `guest_exit_now()` now asks the Metal side
to quit first.

### Menu music "skipping back" for a moment

When the game stalls (first-launch shader compiles, loading), DirectSound keeps
looping its 0.5 s buffer and replays old data. The mixer now tracks which 256-byte
blocks were rewritten since playback last passed them and plays silence for
stale ones (`SNDSTALE=0`: old). `STALL=<present>:<ms>` reproduces a stall: 2 s
stall → 1.8 s muted instead of a replay; a normal race mutes ~85 ms.

### Unsupported executables

`SPEED2.EXE` is checked by SHA-256 at startup; any other build (the US retail
executable, for one) gets a clear message instead of a crash.
`NFSU2_ANY_EXE=1` skips the check.

## 2026-10-26: launcher window and portable copy

Reddit asked for a GUI and for a single app with the game inside, made without a
terminal. `launcher/*.swift` (Swift 5 mode, AppKit + SwiftUI, macOS 11 target)
is now the bundle's executable; `pack.sh` builds it with `swiftc` and puts both
engines next to it:

    Contents/MacOS/NFSU2              launcher
    Contents/MacOS/nfsu2-engine       out/nfsu2_boot
    Contents/MacOS/nfsu2-engine-hd    out/nfsu2_hd_boot

One download (~67 MB) instead of two apps. `./pack.sh --hd` still makes the old
HD-only "NFSU2 HD.app"; `--original-only` leaves the HD engine out; without
`swiftc` the engine becomes the executable as before (no window).

- **Game files:** same search order as `app_find_game`; the exe is checked
  (size, SHA-256) before Play; the engine gets the checked path in `NFSU2_DATA`.
  States: found / not found / different version / inside this app.
- **Graphics:** Original or HD (`LauncherEngine`), full screen (`NFSUFullScreen`,
  the key the engine already uses).
- **Make Copy…:** builds "NFSU2 Portable.app" (save panel, /Applications by
  default): copies this bundle plus the game folder into
  `Contents/Resources/Game` (MOVIES and the Windows plugins left out, as in
  `pack.sh --embed`), `copyfile(COPYFILE_CLONE)` per file (instant on the same
  APFS disk: measured 6 s for 1.5 GB), removes the quarantine tag, signs ad hoc
  again, then renames from a hidden `.partial` name. The running app is never
  written to (signature; App Translocation makes it read-only anyway). A copy
  being replaced goes to the Trash.
- **Tools:** Saves, Logs, Reset Graphics Settings (drops `g_*` and
  `NotFirstTime` from `~/.nfsu2_registry`: the game detects again), Report a
  Problem (GitHub issues).
- **Show this window when the app opens:** off = straight into the game; hold
  Option to see the window.

**The engine is a child process, not exec().** A game folder under Downloads /
Documents / Desktop triggers macOS's folder permission prompt. A child process
inherits the launcher as its "responsible" process, so the permission granted to
the launcher covers the engine; after exec() the ad-hoc signed engine would be a
different code identity and could be asked again. The launcher hides its window,
drops its Dock icon (accessory) and exits with the engine's exit status, so
`open -W` still waits for the game.

Test hooks (no visible window): `NFSU2_LAUNCHER_SNAPSHOT=<png>` draws the window
(`NFSU2_LAUNCHER_STATE=notfound|unsupported|embedded|copying|done`),
`NFSU2_LAUNCHER_TEST=play` presses Play off screen,
`NFSU2_LAUNCHER_TEST=portable:<game dir>:<dest.app>` builds a portable copy.
In automated runs keep the game outside Downloads/Documents/Desktop (or pass
`NFSU2_DATA`): the permission prompt otherwise waits for a click.
Verified: Original and HD started through `open` + launcher, 1200–1500 frames,
clean exit, 0 traps; portable copy signed (`codesign --verify --strict`), no
quarantine, runs from its embedded data; user prefs and registry unchanged.
`tools/session.sh` opens NFSU2.app with `NFSU2_SKIP_LAUNCHER=1 NFSU2_ENGINE=…`.

## 2026-10-26: HD black screen at start, Spotlight, menu car

### HD: ~8 s black screen on the first start of a build (fixed)

The game creates all its pixel shaders while it boots; each was translated and
compiled from MSL source on the spot, one after another. With a cold Metal cache
(the first start of every new build, or after macOS clears the cache) that held
the first frames back: measured, the first 150 frames took 10.0 s in the HD build
(more variants per shader) against 2.4 s with a warm cache. Now `metal_register_ps`
starts each compile in the background (up to 8 at once: the Metal compiler runs
them in parallel), and a draw waits only for the shader it needs; a draw that
needs a shader still waiting in the queue compiles it itself right away
(`ps_wait` / `ps_run`). Cold HD start: 150 frames in 3.1 s. Menu frames differ
from the old path no more than two runs of the old path differ from each other.
`PSASYNC=0`: old behaviour. Note for measurements: total process time is
misleading on a cold cache, Metal writes its cache at exit; time the frames.

### Spotlight opens 3-7 s late while the game runs (open)

Measured (user at the keyboard, ~15 runs): Spotlight's UI process ("Siri AI" on
macOS 27) is backgrounded (darwinbg, priority 4) while hidden and gets the
Cmd+Space ~5 s late; once it has it, it opens fast. Not the cause: the system
shortcut block (`NFSKISAYOL=0` was worse), Game Mode (`GCSupportsGameMode=false`
kept it off, still slow), GPU load (menu ~9 %), `framebufferOnly`
(`FBONLY=1`: readable middle texture + one copy pass, validated with the Metal
debug layer), disk I/O (a few KB/s), CPU cluster load (E-cluster ~62 %). A
control app presenting 120 Hz full screen with the same layer settings does NOT
slow Spotlight, so it is something in the engine; `FPSCAP=10` makes it fast,
`FPSCAP=60` does not. Next: grow the control app towards the engine (threads,
wakeups ~1160/s, GPU work) until Spotlight slows. Test switches: `FPSCAP=<n>`,
`BGFPS=<n>` (cap only while another app is in front), `FBONLY=1`. The layer is
now explicitly opaque.

### Menu car headlights

The main menu's 350Z (no profile) shows a large pale lilac highlight over the
hood front and headlights from the first frame on, and it stays after visiting
car select. Isolated with `SKIPPS`/`SKIPDRAW`: car = ps 265 (draws 167-285 in
frame 1650); the headlight lenses are draws 213-222 (light atlas texture 813,
64x64, complete with 4 mips; 807 = tail lights); the highlight itself is the
scene's reflection on the body panels, not a missing texture and not post
processing. Not decided whether anything is wrong without a reference image.

## 2026-11-02 – 11-03: audio dropouts (issue #12)

Reports: the sound "is gone for a second" every few seconds, on several Macs, not
reproducible here. The reporter's log (M6, 60 Hz, NA HD): the sound service timer
ran at 32 ticks a second instead of 100, the music buffer got 130 KB/s for 176 KB/s
played, 28% of the output was silence, 99 cursor overruns, 8 s of silence while a
race loaded. The game's sound hangs on two 100 Hz loops: the DirectSound mixer thread
(`0x7263c7`, `THREAD_PRIORITY_TIME_CRITICAL`: `next += 10`, `Sleep(next - now)`) and
the sound service timer (`0x6f5b4d`, a one-shot `timeSetEvent` that re-arms itself
and `SetEvent`s the service thread, `THREAD_PRIORITY_HIGHEST`). Four causes, all in
`runtime/win32_ext.c` (+ `win32.c`, `fileio.c`, `dispatch.c`, `com.c`):

1. **Sleeps.** `Sleep` and the timer thread used `nanosleep`, which macOS coalesces,
   and the game's threads ran at `QOS_CLASS_DEFAULT` (`pthread_create` does not
   inherit QoS). Now `kevent NOTE_CRITICAL` and `USER_INTERACTIVE`. Under full CPU
   load: late wake-ups 0.9 ms average / 25 ms max -> 0.03 / 2 ms.
2. **Late wake-ups lost ticks.** A timer re-armed for <= 1 ms (behind schedule)
   fires again at once; a `SetEvent` from the timer thread counts (up to 16);
   `Sleep` repays lateness (sleep debt). `SLEEPSLACK=55000`: 17 -> 100 ticks a
   second, silence 9.7% -> 2.9% (2.7% without the slack).
3. **The virtual clock stood still in long frames** (50 ms cap), and so did both
   loops. Their 7 clock reads (return addresses) now see real time and the timers
   are due in real time (SOUND PACE CLOCK, `win32.c`). Large file reads also give
   the big lock up.
4. **The big lock.** The macOS mutex is unfair; the game thread re-takes it thousands
   of times a second. The sound threads (game priority >= 2) and the timer thread now
   come first: others step back, the holder hands over at its next Win32, COM or
   indirect call. Reproduced here: a 656 ms hold while a race loaded, 950 ms muted,
   20 overruns (2 of ~12 runs); with priority the sound threads waited at most
   10-24 ms, no dropouts in 5 runs, frame times unchanged.

The log now carries `[timer]` lines every 30 s (tick rate, late wake-ups, lock waits,
catch-up and lost ticks). Note for measurements: "sound data gaps" also counts the
silence of loading screens; use the stale guard and cursor overrun lines. Left: a
230-330 ms hold at start-up, before any sound plays (inaudible).

## 2026-11-04: textures were never released (issue #49)

Report: long loads and stutter on a MacBook Air M1 with 8 GB. Its 2-hour log:
34,111 textures created, **34,089 still alive at the end, 1,875 MB**, while the game
itself held only 6,994 live objects (217,930 last releases). `obj_release` (com.c)
gave the game's buffer back on the last Release but never the Metal texture, and
texture ids were never reused (the table had been grown from 16384 to 65536 when it
first ran out). Every track or menu change left its textures behind: memory grew
~0.9 GB an hour, which on 8 GB means swapping and long loads, and the id table would
fill after ~4 hours (every later texture white).

Now (metal.m RELEASING TEXTURES): the last Release queues the id with its owner
object; 3 drawn frames later the main thread drops the texture, clears what is kept
per id (cube flag, uploaded-level mask, HD road-blur marks, ray-tracing slot stamp
and static reflection materials pointing at it) and puts the id on a free list. A
texture still bound to a stage waits (D3D9's device holds a reference). The object's
id is zeroed, so a stale state block binds nothing rather than a reused id. Reset
releases render targets through the same queue; the owner check stops a late
Release from dropping a texture that got a Reset-freed id.

Measured (HD, Quick Race, quit to the main menu and race again, 4 loads, 27,000
frames): textures alive at the end 1,551 / 109 MB (before: 7,893 / 354 MB), process
memory 491 MB flat (before: 378 -> 838 MB and climbing), 6,327 released / 5,778 ids
reused, 0 draws of a released texture, frame times unchanged. Frames before and after
are the same apart from run-to-run motion. EU and NA, original and HD. The log
prints `[tex]` every 2 minutes (textures alive, MB) and a release summary at the end.

Long-session audit done alongside (2-hour log + 4-load runs; what grows per load and
what its limit does):

| Table | Growth | Limit | When full |
|---|---|---|---|
| Guest COM heap (com_new: object + a new vtable each, never freed) | ~1.1 MB a load (32.8 MB in the 2-hour log) | 128 MB | `com_alloc` returns 0 -> crash, ~8 hours of play |
| Vertex/index pool (free blocks never merged; `PF_N` 4096 entries, extra frees dropped) | 115.8 MB top after 2 hours (4 MB in a short run) | 768 MB | new mirrors fail -> slower fallback path, ~13 hours |
| Sound buffers (`audio_register` slots never reused) | +1 a load (24 in 2 hours) | 128 | new streams silent, ~10 hours |
| Z surface textures (kept until the id wraps at 62) | ~4 per Reset | 62 ids | bounded, up to ~62 full-screen depth textures kept |
| Win32 objects (`CloseHandle` frees nothing) | +4 per menu -> race -> menu round trip (22 -> 26) | 511 | `Create*` returns 0, ~120 round trips |
| Critical sections (`DeleteCriticalSection` frees nothing) | +2 per round trip (43 -> 45) | 512 | locking silently off, ~235 round trips |

All six were then fixed (same day):

- **COM objects** (com.c COM OBJECT RECYCLING): one vtable per interface; a texture /
  vertex buffer / index buffer object whose last reference went is reused for a new
  object of the same kind and size after the data's quarantine (1500 presents),
  unless it is bound (texture stages, stream 0, indices) or a live state block names
  it; its pool mirrors are dropped at the release and again at the reuse.
- **Vertex/index pool** (metal.m): free blocks sorted by offset and merged, a free
  block at the top lowers it, the list has no size limit.
- **Sound buffers** (audio.m `audio_unregister`): the slot goes back at the game's
  Release; a duplicated buffer gets its own stale-data map (it shared the source's).
- **Depth textures**: created ones that are live at Reset are dropped 3 frames later.
- **Win32 objects**: CloseHandle / RegCloseKey mark the object closed; its slot is
  reused when nobody waits on it, a thread has ended and 2 s have passed;
  DuplicateHandle counts copies. DeleteCriticalSection frees the section (tombstone
  in the probe chain, only if nobody holds it).
- **Small ones**: the ordered-upload area grows up to 64 MB instead of overflowing;
  the multi-object wait polls with the precise sleep; the event table dump and the
  `[matrix]` totals are behind `EVENTLOG=1` / `MATLOG=1`.

Measured (HD, Quick Race, 3 round trips = 4 loads, 27,000 frames; all fixes vs all
switched off): COM heap 645 KB with 22,839 objects reused vs 4,539 KB; pool top
2.9 vs 5.0 MB; textures alive 1,552 vs 7,822; 8 sound slots given back; Win32 slots
21 (4 reused) vs 25, 10 critical sections deleted; process memory flat at ~560 MB
vs 376 -> 775 MB; no sound gaps in either; frame times the same (GPU 5.25 / CPU
0.86 vs 0.88 ms). A Reset storm (`RESETAT` every 100 frames in a race, 42 Resets):
231 render targets and 147 depth textures released, 8 depth textures alive at the
end, the race frame after it the same as before, no draw of a released texture.

Also seen: one 62 ms stretch of game code without any Win32/COM/indirect call at
`0x5c587d` (11.6 s in), where the sound threads cannot get the big lock; once in ~12
runs it lasted long enough for an 88 ms gap. Ordered texture uploads overflowed 97
times in the 2-hour log (those uploads fall back to immediate).
