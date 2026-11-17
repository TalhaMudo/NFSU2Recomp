# The D3DX9 effect library — HLE work

Work on moving the D3DX9 effect framework (ID3DXEffect) statically linked into
NFSU2 to native code / short-circuiting it, step by step. Code:
`runtime/d3dx_hle.c`. General performance context: `docs/devlog.md` → "Performance".

---

## Why

Measurement of 2026-09-23 (M1 Pro, highest settings, AUTOSEQ Quick Race,
mid-race, `sample` 8204 samples, guest thread self time):

| item | share |
|---|---|
| **D3DX9 effect library** (33 functions with a `mov edi,edi` prologue, mostly 0x0066xxxx) | **47%** |
| the game's own code | 32% |
| our D3D9 layer (`com_call`, `memmove`, `dispatch_indirect`) | ~15% |

Including its subcalls, **a single API entry, `ID3DXEffect::BeginPass`, is 48% of
the guest thread**. The real D3D calls inside it are only 3.4%; the rest is D3DX
re-interpreting all state expressions (preshaders) on the CPU on every pass —
in our case through translated x86.

---

## Map

### ID3DXEffect vtable — `0x7b6fe8`

The mapping was verified with `ret N` signatures (every `Set*` 0xc/0x10, `Begin`
0xc, `BeginPass` 8, `CommitChanges`/`EndPass`/`End` 4 ...). The layout is the
transitional (2004) header that has both Set and Get for pixel/vertex shaders:

| slot | method | address | | slot | method | address |
|---|---|---|---|---|---|---|
| 3 | GetDesc | 0065cb66 | | 58 | SetArrayRange | 0066c9cd |
| 20 | SetValue | 0066c905 | | 59 | GetPool | 0065cb08 |
| 26 | SetInt | 0066dd7f | | 60 | SetTechnique | 00661e8c |
| 28 | SetIntArray | 0066e8a9 | | 62 | ValidateTechnique | 0066074e |
| 30 | SetFloat | 0066f501 | | 65 | **Begin** | 00664792 |
| 32 | SetFloatArray | 0066eec8 | | 66 | **BeginPass** | **0066ace2** |
| 34 | SetVector | 0066d303 | | 67 | CommitChanges | 0066ac27 |
| 36 | SetVectorArray | 0066ca38 | | 68 | EndPass | 0065addb |
| 38 | SetMatrix | 0066d13a | | 69 | End | 0065acf5 |
| 52 | SetTexture | 0066d7e9 | | 70 | GetDevice | 0065cb37 |
| 54–57 | Set/Get Pixel/VertexShader | | | 71/72 | OnLostDevice / OnResetDevice | 0065c89d / 0065e593 |
| | | | | 78 | CloneEffect | 0067065b |

### Hot call tree (profile share, including subcalls)

```
48.1%  BeginPass            0066ace2   (game: sub_005cf430 draw loop, 38.6%)
47.1%   apply pass           006691b1
33.3%    set state           00669296   type>>24 switch, 100-byte state record
26.0%     get state value    00664a2f   <- Stage 1 memoizes here
17.2%      evaluate expr     00663a42   FPU CW setup + gather inputs + VM
 5.3%       preshader VM     00688e89   x87, double register file
 4.8%       gather inputs    00662edc   (kind 1/2)   / 00663275 (kind 0)
 6.5%      process result    00660a52
 8.2%    (other path)        00669f40 → 00668e0d → 00668cd0 → 00664a2f
```

### Call counts (race, per frame, `D3DXSTAT`)

| | calls/frame |
|---|---|
| Begin / End | 80 / 66 |
| BeginPass / CommitChanges / EndPass | 1398 |
| set state (`669296`) | 28,338 (~20 per pass) |
| get state value / evaluate expression / VM | 11,838 (~8.5 per pass) |
| total Set* (Matrix 1731, Vector 1512, Texture 576, IntArray 501, Float 233, VectorArray 130) | ~4,700 (~3.4 per pass) |

The game calls BeginPass on every draw. While ~3 parameters change per pass,
D3DX recomputes and applies all ~20 states.

### Known data structures

Effect object (`this`, `ecx`):

| offset | meaning |
|---|---|
| +0x2c | array of parameter/pass records, 0x64 bytes/record |
| +0x3c | Begin state (0 = outside Begin → `D3DERR_INVALIDCALL` 0x8876086c) |
| +0x40 / +0x42 | technique indices |
| +0x44 | active pass (0xffff = none) |
| +0xa4 | technique array, 0xb4 bytes/technique; passes at `[technique+0x38]`, 0x38 bytes, pass param index `+0x34` |
| +0xb0 | object table |
| +0x114 | saved FPU control word |
| +0x118 | flags: bit0 = CW saved, 0x100 = inside a pass, 8 = … |

Parameter/pass record `P = idx*0x64 + [this+0x2c]`: `+0` data block pointer,
`+4` sub-record (state) array, `+8` flags, `+0x36` state count.

State record `R = b*0x64 + [P+4]`:

| offset | meaning |
|---|---|
| +0x08 | value buffer pointer |
| +0x30 | flags; &4 = constant value (`[R+0x60]` bytes are copied from `[R+0x5c]`) |
| +0x34 / +0x4e | input records (32 bytes) / count; valid if `[R+0x3c]` != 0 |
| +0x38 | program container (vtable 0x7b7124, slot 3 = getter returning `[+0xc]`) |
| +0x40 / +0x44 | VM program pointers |
| +0x48 | offset of the target within `P.data` |
| +0x52 | state type → table at 0x7b5834 (0x1c bytes/entry) |

Input record: `+0` dirty flag (always set, re-set with `[+0] = [+4]`, so it is
useless), `+0xc` → {data pointer, definition}, `+0x10` → register definition
(`+4` set 0/1/2, `+6` index, `+8` count). The gatherers convert float/int/bool
data into the VM's **double** registers (32 bytes per float4).

---

## Stages

### Stage 0 — measurement infrastructure ✅ (2026-09-23)

* `runtime/d3dx_hle.c`: counter wrappers for 20 hot D3DX functions.
* `tools/lifter/override_patch.py`: applies `OVERRIDE` to the existing shards without a
  full regeneration; only the affected shards are recompiled.
* `CPUWIN`: guest thread CPU time / frame (a measure independent of the GPU).

### Stage 1 — state value memo ✅ (2026-09-23)

`664a2f` (get state value) depends, for expression states, only on the data of a
single input. `D3DXANALYZE` measurement: 111 distinct (effect, pass, state) keys
in a race, and in **95.8% of evaluations the input data is byte-for-byte the same
as in the previous evaluation of the same state**.

Memo key `(this, a, b)`. The record keeps a **full copy** of the input (`this`,
`R`, `[R+0x38]`, `[P]`, `[R+0x48]`, `[R+8]` plus each input's dirty flag, data
pointer and data bytes; at most 256 bytes) and compares it with `memcmp`. If it
is the same as the previous copy, and the target dword and the first dword of the
value buffer haven't changed since the last real computation either, the call is
skipped. (The first version used a byte-wise FNV hash; in the profile it took 8%
by itself. Switching to a full copy made it both faster and removed the collision
risk.)

**Verification (`D3DXVERIFY=n`):** on 1/n of the predicted hits the original is
run anyway; ±256 KB of the effect object (mapped pages) and `.data` are compared
before/after.
* The only difference is `this+0x114/0x118`: the saved FPU CW. Harmless, since
  the skipped evaluation never touches the CW and the end of BeginPass doesn't
  restore it either. Excluded from the comparison.
* **965,299** checks with the memo off, **1,003,104** with the memo on (hashed
  version) and **501,888** (full-copy version): **0 differences**, 0 guard
  aborts.
* Visual: car selection (frame 3050) exactly the same; in race frames only
  timing-dependent counters differ.

**Result** (four alternating runs, 2448x1836, race frames 5900–8990):

| | guest CPU ms/frame | race mean | median | p95 |
|---|---|---|---|---|
| memo off | 11.98 / 10.89 | 78.6 / 90.0 fps | 12.13 / 11.05 ms | 15.30 / 12.33 ms |
| memo on | 8.10 / 8.11 | 111.2 / **118.9 fps** | 8.36 / **8.33 ms** | 16.58 / 8.72 ms |

Guest CPU **−29%**. In the pair with an idle GPU the race hit the display's
120 Hz limit (median 8.33 ms).

`D3DXMEMO=0` turns the memo off (on by default).

### Stage 2 — state assignment memo ✅ (2026-09-24)

**Measurement (`COMSTAT`, race, per frame):** ~36,000 device calls come from
inside BeginPass/CommitChanges, and **96% of them are redundant** (they rewrite
the value already on the device). Only ~7,000 come from outside.

| method | inside BeginPass | redundant | outside | redundant |
|---|---|---|---|---|
| SetRenderState | 12,598 | 97.9% | 1,578 | 85.0% |
| SetSamplerState | 11,745 | 97.4% | 1,212 | 75.2% |
| SetTextureStageState | 5,325 | 99.3% | 486 | 93.4% |
| SetTexture | 2,189 | 74.1% | 194 | 63.4% |
| SetVertexShader / SetPixelShader | 1,340 | 95.9% | 66 | 25.8% |
| SetVertexShaderConstantF | 1,317 | 50.4% | 57 | 10.5% |
| SetPixelShaderConstantF | 458 | 76.2% | 16 | 0% |

**Implementation:** a wrapper for `sub_00669296` (set state) + `runtime/dxrec.h`.
* During a real run, the device calls the function makes (with arguments and
  constant data) are recorded in `com.c` (`dxrec_begin/end`).
* The question "would this call have no effect if made now" is answered in
  `com.c` by comparing the value the handler would write with the current state
  (`dxrec_redundant`). 8 supported methods: RenderState, SamplerState,
  TextureStageState, Texture (including the derived Metal id), Vertex/Pixel
  Shader (including the derived id), VS/PS ConstantF (data bytes). Any other
  device call, a nested `669296`, state block recording or a record overflow
  invalidates it.
* Snapshot: 11 fixed fields (`this` flags, P), the whole state record
  (0x64 bytes), the target value (16), the value buffer (16), the object table
  entry, and for expression states the Stage 1 inputs. It is compared **in place**
  against the record slot's buffer and only copied after the first difference
  (8-byte inline word comparison).
* Hit = same snapshot **and** every recorded call currently has no effect.

**Verification (`D3DXVERIFY2=n`):** on sampled hits the original is run with
recording. The memory window must be the same and every call it makes must have
no effect at that moment.
* In the first run two state types came out different. **Type 9** reads
  indirectly from the texture table; **types 14/15** select an object from an
  array with an expression and apply it nested. The snapshot doesn't cover these
  inputs. **Only types 1–8 are memoized** (86% of calls).
* After that, 3,286,996 + 3,304,796 checks, **0 memory / 0 device / 0 eax
  differences**. Also clean with the Stage 1 verification on at the same time.
* Visual: car selection 0.07% pixel difference (the cursor), in races only the
  counters.

**Result:** 82% hits (92–99% for types 1–8).

| measurement | Stage 2 off | on |
|---|---|---|
| guest CPU, `PACE60=1` (fixed 60 fps) | 7.94 / 7.80 ms | 7.50 / 7.45 ms (**−5%**) |
| race fps (this machine, 120 Hz) | 119.8 | 119.9 (already at the limit) |

In the 60 fps profile BeginPass is ~26% of busy time. The wrapper's own check is
~12% (~34 ns per state, the original ~100 ns). That is why the gain stays small:
~300 bytes of input are read and compared for every state.

`D3DXMEMO2=0` turns it off (on by default).

**Measurement lesson:** after Stage 1 the race is at the 120 Hz limit. There the
guest CPU/frame comes out at 8.15 ms in both configurations: the game's work
stretches a little with the frame rate (7.9 ms at 60 fps). Measure CPU changes
with **`PACE60=1` + `CPUWIN`**. `NOVSYNC=1` doesn't lift this limit (in windowed
mode the compositor runs at 120 Hz).

### Stage 2b — what's left (NEXT)

1. **Make the check cheaper:** the 100 bytes of the state record are D3DX's
   static structure. Settling for only the fields that are read (+0, +4, +8,
   +0x1c, +0x30, +0x34, +0x38, +0x3c, +0x48, +0x4e, +0x52..+0x60) cuts the
   snapshot by ~half. Must be verified with `D3DXVERIFY2` first. The slot table
   (8192 × ~1.1 KB) may also not fit in the cache.
2. **Types 9 and 14/15:** add their indirect inputs to the snapshot (type 9: the
   `[this+0x18]` texture table entry; type 14: the selected object's own states).
   Type 14 evaluates its own expression directly with `663a42`, so the Stage 1
   memo doesn't cover it.
3. **The `669f40 → 668e0d → 668cd0` path** (~7% of busy time at 60 fps): looks
   like a sampler object application; the same record/check can be done per
   sub-state.

### Stage 3 — parameter versions + pass-level skipping ❌ (2026-09-24, negative result)

**Idea:** make "did the inputs change" O(1) with a version counter per parameter
and skip the whole pass with a single check per BeginPass.

**Infrastructure built (still in the code, OFF by default):**
* All `Set*` slots (20–56 and CloneEffect, 20 call sites) call `0x66056c`, which
  in turn (for every effect in the effect pool) calls
  **`0x65ab24(effect, slot)`**. The parameter block path (`0x67054b`) comes
  straight to `65ab24`. The `65ab24` hook writes a global counter to the 64-byte
  lines of the changed data. Since the effect pool shares data between effects,
  the key is the data **address** (the same data address was seen in different
  effects).
* Parameter slot: `+0` data, `+4` definition, `+0x14` parameter index.
  Definition: `+0` type (2 int, 3 float, 5 texture), `+4` class (0 scalar, 1
  vector, 2 matrix, 4 object), `+0x10` elements, `+0x14` rows, `+0x18` columns.
  The `+0xc` field of the expression input record points to the same slot
  structure.
* `6691b1` pass wrapper: fingerprint (effect fields + pass record + technique
  `+0x8c..+0xab`), dependency lines, no-effect check of the recorded calls.
  `dxrec` now supports a record kind (pass/state) and `dxrec_append`. The calls of
  states that Stage 2 skipped are appended to the open pass record.
* Flag 8 path (`66404c`/`6641a7`): compares the pass's shader constant register
  count with the technique's largest, and only does work when it grows (no calls
  after warm-up). Deemed acceptable by adding the technique fields to the
  fingerprint.

**Result:** 86% predicted hits, but `D3DXVERIFY3` showed that in **78%** of the
hits the pass actually produced different calls (713,519 device and 523,606
memory differences in 919,133 checks). Reasons:
1. **Textures don't come from parameter data.** The sampler/texture state looks
   at the parameter through an object index; `SetTexture` writes the new pointer
   into the effect's object table (`[this+0x18] + index*24 + 4`), and the
   parameter data (the index) doesn't change.
2. The game changes textures on **almost every draw** (576 SetTexture per frame).
   Even if the texture dependency were added correctly, cases where a whole pass
   stays the same are ~19%.

So in this game the right granularity is not the pass but the **state**
(Stage 2). `D3DXMEMO3` is off by default; the version table is only maintained
when it is on. Stages 1–2 were re-verified after these changes: 3,541,437 +
81,950 checks, 0 differences.

**Side finding, the identity of `668e0d`:** the shader-based pass application.
It binds the VS/PS (`+0x170`/`+0x1ac`), fills the constant blocks from the
parameters (`6623ea`/`6627a5`/`662bab`) and uploads them with
`SetVertex/PixelShaderConstantF/I/B` (`+0x178/+0x1b4`, `+0x180/+0x1bc`,
`+0x188/+0x1c4`). At the end it applies the sampler states with `668cd0`
(type 3 → `SetSamplerState`, type 9 → `SetTexture`, from the texture object
table). The constant uploads really do change per draw. Savings exist only in the
shader/sampler part.

### Stage 3b — next candidates

1. **A state-level memo for `668cd0` (sampler sub-states):** Stage 2's method,
   adding the object table entry (`[this+0x18]`) for textures to the snapshot. At
   60 fps the `668e0d` path is ~7.5% of busy time.
2. **Make the Stage 2 check cheaper** (the item in Stage 2b).
3. Use parameter versions **at the state level** (instead of byte comparison of
   expression inputs). Expected gain small: most of the snapshot is the static
   state record.

### Stage 3c — native helpers ✅ / a cheaper Stage 2 check ❌ (2026-10-15)

* **Native helpers** (bit-identical C equivalents of the lifted x87/flag code):
  `67193e` D3DXMatrixMultiply (a fast path without NaN checks if all 32 inputs are
  finite, FMA off; the original otherwise or on partial overlap), `66056c` pool
  notification (register and stack layout exact), `65cf44` SetMatrix transpose
  (types 3/1; type 2 `__ftol` in the original). `D3DXNVERIFY=n`: ~1 million
  comparisons, 0 differences. D3DX self time −12.5% (profile), game thread ~−5%.
* **Narrowing the Stage 2 check** (only the fields of the state record that are
  read, a hot/cold slot split): verification clean, but the time in the profile
  **didn't change** (1070 → 1065 samples) — the cost isn't in the byte
  comparison. Reverted.

### Stage 4 — moving D3DX's hot path to native C (2026-10-15, ongoing)

**Inventory** (`EFXSTAT=<frame>`; the `D3DXCreateEffectFromResourceA` = `640c12`
wrapper also logs creation):
* 23 effects, all at startup, with a single shared pool (`[0x870914]`), from the
  compiled fx_2_0 binaries in the PE resources (`RT_RCDATA`, `IDI_*_FX`).
* 25 of the 82 methods are used; per frame only 8 Set* + Begin / BeginPass /
  CommitChanges / EndPass / End. No string handles at all.
* 13 effects, 16 passes in a race; 33 across menu + race.
* Handle = bitwise NOT of the record address: parameter
  `~([this+0x9c] + i*0x50)`, technique `~([this+0xa4] + i*0xb4)`, pass
  `~([technique+0x38] + i*0x38)`.

**Format** (`tools/analysis/efxdump.py`, all 23 blobs are consumed byte for byte): 325
parameters, 35 techniques, 47 passes, 871 states; 92 shaders (CTAB is parsed),
224 expressions — **all single-component MOVs** (no constant/relative
addressing), and the `PRES` markers in the shaders are empty. The operation table
follows D3D9 order: 0..0x66 render state (WRAP0–15 together), 0x67.. texture
stage, 0x7a FVF, 0x7b–0x7e transform, 0x92/0x93 VS/PS, 0xa4 Texture, 0xa5..
sampler state. D3DX's own state table is at `0x7b5834` (28 bytes per operation).

**D3DX mechanics** (`FXTRACE=<frame>,...` device + effect call trace; for raw
D3DX use `D3DXMEMO=0 D3DXMEMO2=0`):
* Begin(0) captures ~5 state blocks every time, and End applies them (dozens of
  device calls even with no pass).
* `669296`: if not dirty (`R+0==0`) the value isn't refreshed, but the device
  call **is made on every BeginPass**; its end is the `0x66a1f7` jump table
  (type 1 RS, 2 TSS, 3 SS, 9 texture, 14 shader object, 15 sampler object …).
* Set* → `65ab24`: outside Begin it marks the dependent states dirty; inside
  Begin it also adds them **from all passes** to the dirty list
  (`[this+0x108]`). CommitChanges applies the records from the list whose
  `[P+0x3c]` (activity counter, `65ae35` ±1, propagated to nested records) is
  greater than zero — which is why the PS constant block of an inactive pass can
  get uploaded too. EndPass clears the list without applying it. Since this
  ledger lives in D3DX memory, the port was done without disturbing it.

**Ported** (exactly, on D3DX's own memory structures; every unsupported path
falls back to the original per call):

| step | function | scope | verification |
|---|---|---|---|
| 4a | `669296` types 1/2/3 (+ expression memo) | 86% of state calls; `664a2f` if dirty, a small memo per state (input data + target/value buffer guard) | 2.9 M, 0 differences |
| 4b | `6691b1` pass loop | prologue, flag 8 / P&8 / P&1 paths, two error rules | via 4a |
| 4c | `668cd0` samplers | state + texture per sampler record | 4 thousand, 0 differences |
| 4d | `668e0d` shader | VS/PS + raw-copy constant blocks + upload; the fillers stay guest | 316 thousand, 0 differences |
| 4e | `66d7e9` SetTexture + `65ccd8` | object table + pool propagation | run + image |

Common rules: if a device call matches the `com.c` shadow state it isn't made at
all (types 1–3 inline; always made during state block recording/replay, an open
`dxrec` record, or a trace frame). Texture AddRef/Release are skipped since they
are no-ops in `com.c` (~3900 dispatches per frame). The verifier (`nat_verify`):
the native code does a dry run (device calls are recorded), memory is rolled back,
the original runs with `dxrec` recording; the memory window + extra records, the
call sequence and eax/esp are compared. Paths that need a guest subcall can't be
verified dry.

**Measurement** (`PACE60=1 CPUWIN=6200,8800`, in the same session against the
baseline `k21` binary, mean of two runs):

| state | baseline | new | difference |
|---|---|---|---|
| native helpers | 6.76 | 6.68 | −1.2% |
| + 4a | 6.68 | 6.11 | −8.5% |
| + expression memo, 4b | 6.70 | 5.64 | −15.8% |
| + 4c, 4d | 6.58 | 5.68 | −13.8% (within noise) |
| + 4e, inline check | 6.97 | 5.34 | **−23.4%** |

Image: race frame 6600 the same side by side (differences only from the timer /
frame moment). In the profile D3DX + native code are still ~29% of the game
thread; the biggest items are the expression memo (`n4_eval`), the state core,
expression evaluation misses (`663a42`/`660a52`/`688e89`), and the Set* path
(`66056c`, `66d13a`, `66d303`, `65ab24`).

**Next:** a compiled state list per pass (for non-dirty states only a value +
shadow comparison), the Set* path (`65ab24` mode A), a MOV-specific native
equivalent of expression evaluation, applying the Begin/End state blocks filtered
through the shadow.

### Power at 120 fps: the game's frame limiter was spinning ✅ (2026-10-16)

**Problem:** Stage 4's −23% CPU gain didn't show up in watts at all at 120 fps
(`powermetrics`, race 3024x1964: baseline 8.33 W, new 8.31 W). The P0 cluster was
at 3.2 GHz with the same activity in both versions. Real per-thread CPU with
`ps -M`: the game thread was at **8.2 ms/frame** in both versions (frame 8.34 ms →
98% busy). The time won was burning in a wait loop.

**Source:** QueryPerformanceCounter is called ~250 million times per run (~19
thousand per frame in a race; ~2 thousand with `PACE60`). The caller according to
`QPCWHO=1`: `sub_0043bdf0` (GetTicks = QPC >> `[0x8284e4]`), called from
`0x57eb79`, i.e. `sub_0057eae0` — NFSU2's own frame limiter:

```
X = [0x865558] = 1/120 s (frame target), dt = this frame's duration
if dt < X:  start = GetTicks(); target = (X - dt) * [0x784550] (=1000, ms)
            do { elapsed = Diff(start, GetTicks()); } while (elapsed < target);
Diff = sub_0043be20 = (now - start) * 2^S * [0x828028] (=0.001, ms)
```

The loop only looks at the clock; it spins without sleeping. Every ms won in D3DX
(or anywhere else) went into this loop.

**Fix** (`runtime/win32.c`, `qpc_spin_sleep`): if the QPC call comes from this
loop (return `0x43bdfe`, `[esp+0xc]` = `0x57eb79`), the remaining time is
computed with the game's own formula: start = EBX, target = `[esp+0x20]` (float,
ms). Then it sleeps (remaining − margin) while releasing the big lock; the final
margin is still waited out by spinning. Since the exit condition and the clock
stay the same, timing and image don't change. `SPINSLEEP=<margin µs>` (default
200), `SPINSLEEP=0` turns it off.

The sleep is not `nanosleep` but a `kevent` timer + `NOTE_CRITICAL`
(`win32_ext.c`, `guest_sleep_ns`). macOS wakes `nanosleep` with a timer
coalescing margin: in the game a 3 ms request is on average 447 µs late, 12% more
than 800 µs. Because of this, with a 400 µs margin the frame deviation rose from
0.44 → 0.50 ms, and with a 150 µs margin fps dropped to 116.6. With `kevent` the
mean latency is 41 µs (98% ≤ 200 µs), lock wait 2–6 µs; with 150 and 400 µs
margins fps is 119.9–120.0, deviation 0.43 ms (the same as the spinning version),
longest frame 9.5 ms. Independent measurement (QoS makes no difference):
`nanosleep` and `mach_wait_until` median ~770 µs late, `kevent` `NOTE_CRITICAL`
median ~45 µs late.

**Measurement** (120 fps race, frames 6400–11600, `powermetrics`, mean of two
runs):

| version | total | CPU | GPU | game thread (`ps -M`) | fps | longest frame |
|---|---|---|---|---|---|---|
| A baseline (`k21`) | 8.83 W | 5.18 W | 3.65 W | 8.26 ms/frame | 119.9 | 14.9 ms |
| B Stage 4, `SPINSLEEP=0` | 8.83 W | 5.10 W | 3.74 W | 8.22 ms/frame | 119.9 | 14.7 ms |
| C Stage 4 + sleep | **7.64 W** | **3.96 W** | 3.68 W | **5.25 ms/frame** | 119.9 | 9.5 ms |

C is 1.19 W lower than A in both runs: total −13.5%, CPU −24%. Process total
1.40 → 1.05 cores, QPC calls 269 M → 4.2 M. (This measurement was with
`nanosleep` + a 400 µs margin; the `kevent` version also fixed the deviation, see
above.) The same in the HD build: game thread 8.11 → 5.32 ms/frame, process
1.43 → 1.17 cores, 119.8 fps.

**Measurement lesson:** at 120 fps the guest thread's CPU (including `CPUWIN`)
includes the frame limiter's spinning. Measure CPU gains with `PACE60=1`, and
power gains only with `powermetrics`. Real per-thread CPU: `ps -M -p <pid>` at the
start and end of the race window.

---

## Environment variables

| variable | effect |
|---|---|
| `D3DXMEMO=0` | turns off the Stage 1 memo (on by default) |
| `D3DXVERIFY=<n>` | on 1/n of the predicted hits runs the original and compares memory (slow) |
| `D3DXSTAT=<frame>` | counter/time report from that frame on (the timers add ~5 ms per frame; take times from the profile) |
| `D3DXANALYZE=<frame>` | change rate of expression evaluation inputs |
| `CPUWIN=<start>,<end>` | guest thread CPU ms/frame (together with `PACE60=1` at the 120 Hz limit) |
| `D3DXMEMO2=0` | turns off the Stage 2 memo (on by default) |
| `D3DXVERIFY2=<n>` | on 1/n of Stage 2 hits runs the original with recording and compares memory + device |
| `D3DXMEMO3=1` | Stage 3 pass memo (research; OFF by default, came out wrong in verification) |
| `D3DXVERIFY3=<n>` | on 1/n of Stage 3 hits runs the original and compares, writes the effective calls |
| `COMSTAT=<frame>` | device method counts, inside/outside BeginPass, redundant ratio (together with `D3DXSTAT`) |
| `D3DXNATIVE=0` | turns off the native helpers (`67193e`, `66056c`, `65cf44`) |
| `D3DXNVERIFY=<n>` | compares against the original on every nth call of the native helpers |
| `D3DXNATIVE4=0` | turns off the native state path (4a) and the pass loop (4b) |
| `D3DXN4MEMO=0` | turns off the per-state expression memo |
| `D3DXN4VERIFY=<n>` | dry native + original comparison on the 4a/4c/4d paths (with `D3DXMEMO=0`) |
| `D3DXNATIVE5/6/7/8=0` | turn off the native pass loop, sampler, shader and SetTexture paths respectively |
| `EFXSTAT=<frame>` | ID3DXEffect vtable inventory + effect creation lines |
| `FXTRACE=<frame>[,…]` | every device call (argument/constant data) and effect method in order at those frames |
| `SPINSLEEP=<µs>` | sleep instead of spinning in the game's frame limiter; the final margin (default 200) is waited out by spinning. `0` turns it off |
| `QPCWHO=1` | QueryPerformanceCounter callers (thread, return, parent frame) + a frame stamp every 1 M calls + a `SPINSLEEP` summary (sleep count, late wake-up distribution, lock wait) |

All of these are for measurement/diagnostics; `run.sh` doesn't clear them. The
recipe for measurements: `AUTOSEQ` Quick Race + `QUITFRAMES=9000`, window
5900–8990. See `docs/devlog.md` "Measurement method".

**Measurement trap:** frame time is max(CPU, GPU). When the GPU is shared with
another application (on the night of 2026-09-23 the GPU was 20% busy with the
game closed, 5 → 18 ms per frame in game) all old and new runs slow down and the
CPU gain disappears. Measure CPU changes with `CPUWIN`, frame time with
`NFSNATIVE=0`.

## Regeneration

Every wrapped function must be generated as `sub_XXXX_orig`. The full OVERRIDE
list (overrides.c + d3dx_hle.c; 35 addresses, must be the same as in
`docs/devlog.md` → "Commands"). The first three are the rain / time-of-day wrappers
in `overrides.c`; if one is missing, `sub_XXXX_orig` isn't generated and the link
breaks:

```
60fab0,61e390,61f130,5c7ed0,50d510,65ae35,6623ea,66aa3f,66ace2,6691b1,669296,664a2f,663a42,688e89,663275,662edc,660a52,668e0d,664792,66ac27,65addb,65acf5,66d13a,66d303,66d7e9,66e8a9,66f501,66ca38,65ab24,67193e,66056c,65cf44,640c12,668cd0,65ccd8
```

To apply to the existing shards instead of a full generation:

```bash
.venv/bin/python tools/lifter/override_patch.py 66ace2,6691b1   # prints the names of the affected shards
# compile only those shards (same flags as build.sh), then ./link.sh
```

---

## Progress log

| date | stage | change | guest CPU ms/frame (race) | note |
|---|---|---|---|---|
| 2026-09-23 | 0 | measurement infrastructure, vtable map | 10.9–12.3 | reference (memo off) |
| 2026-09-23 | 1 | state value memo (`664a2f`), hashed | 8.40–9.22 | −22–25%, 0 verification differences |
| 2026-09-24 | 1 | memo full copy + `memcmp` | 8.10–8.11 | −29% (reference the same night 10.9–12.0), race 118.9 fps |
| 2026-09-24 | 2 | state assignment memo (`669296`), call recording + no-effect check | 7.45–7.50 (`PACE60`) | −5% (`PACE60` reference 7.80–7.94); no difference at 120 Hz, already at the limit |
| 2026-09-24 | 3 | parameter versions + pass memo | — | negative: in verification 78% of the hits were wrong (texture object table, texture change per draw); off by default |
| 2026-10-15 | 3c | native helpers; cheaper Stage 2 check | 6.76 → 6.68 (`PACE60`) | helpers permanent; Stage 2 narrowing gained nothing, reverted |
| 2026-10-15 | 4a–4e | `669296`/`6691b1`/`668cd0`/`668e0d`/SetTexture native | 6.97 → 5.34 (`PACE60`, same session) | −23%; all verifications 0 differences, image the same |
| 2026-10-16 | power | frame limiter (`sub_0057eae0`) spin → sleep | 8.26 → 5.25 (120 fps, `ps -M`) | 8.83 → 7.64 W (−13.5%), 119.9 fps kept; Stage 4 alone was 0 W |
| | | | | |
