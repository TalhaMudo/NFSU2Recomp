# NFSU2 graphics enhancement research (2026-10-04)

Question: how are projects that add new reflection/effect/engine improvements to
this game built, what has the community done, and what can we do as a native
port?

**Short answer.** The PC community uses four approaches: (1) memory/code patches
to the exe (ASI plugins), (2) a `d3d9.dll` wrapper (ReShade, dgVoodoo, the
Widescreen Fix's PostFX), (3) path tracing with RTX Remix, (4) data file mods
(textures/models). In our port (1) and (2) **don't work as DLLs**, but we can do
both in a more powerful way: the D3D9 layer is already ours, and the lifted code
is already ours. (4) works as is, because we read the same game files.

---

## 1. Community inventory

| Project | What it does | Technique | License | Applicability to us |
|---|---|---|---|---|
| **NFSU2 HD Reflections** (Aero_, later the Zintexiss fork) | Raises the resolution of the road reflection, car cube map and rear-view mirror; adds the sky, headlights and light trails to reflections; increases reflection LOD and distance; "Xbox 360 style" motion blur; chrome material; enlarges memory pools | ASI + fixed-address patching and code caves with `injector` | **GPL-3** | The knowledge (addresses, constants) can be used, the code can't be copied. This is the most valuable map. |
| **Widescreen Fix** (ThirteenAG) | Hor+ FOV, HUD, mirror/reflection aspect ratio, "Console Gamma", SMAA, hiding the MSAA edge bug, windowed mode | ASI + byte signatures (`hook::pattern`) + an `IDirect3DDevice9` EndScene hook | MIT | We already have Hor+ and the HUD (`NFSRES`, `HUDFIX`). Rewriting Console Gamma and SMAA in Metal is easy. |
| **Xbox Rain Droplets** (ThirteenAG) | Rain drops sticking to the camera (the effect from the Xbox version) | Intensity and "am I in a tunnel" are read from the game's rain object; the drops are drawn with D3D9 | MIT | Directly portable: we read guest memory and draw the drops in Metal. |
| **NFSU2 Extra Options** (ExOpts Team) | Constant rain, rain size/density/gravity, road reflection amount, headlight brightness, neon brightness, hidden cameras, debug camera | ASI, `.ini` | — | Writes to game variables; easy for us as environment variables or a settings menu. |
| **Underground 2 RTX Remix** (UncleBurrito; `adamplayer/Underground2RTX`) | Path tracing: all light sources re-placed, neons emit light, glossy car paint, new traffic cars, sky, NRC/froxel fog, DLSS/XeSS | RTX Remix captures D3D9 calls and rebuilds the scene; **an IPS patch to `speed2.exe` is needed to turn off camera-based culling** | — | Important as an idea. Problems: light culling flicker, 100+ lights, 30–50 fps on an RTX 4080. |
| **Redux 3.0, "UG2 SKIN", 8K Remaster** | HD textures, a new lighting atmosphere, ReShade presets | Replacing the game files (TPK/BIN) + ReShade | Varies | The texture/model part **works as is** for us. The ReShade part doesn't (there is no ReShade on Metal). |
| **ReShade presets** | SSAO, bloom, SMAA, sharpening, LUT | Screen-space post-processing in a d3d9 wrapper, reading the depth buffer | BSD (ReShade) | The equivalent can be done with our own Metal post-process pass. |
| **dgVoodoo2 / DXVK** | Forced MSAA/AF, API translation | D3D9 → D3D11/Vulkan | — | The counterpart of our D3D9→Metal layer. We can force MSAA too. |

## 2. The game's reflection pipeline (verified in our exe)

From the HD Reflections code and our own parsing:

| Reflection | How it is produced | Default | Location in our exe |
|---|---|---|---|
| **Car environment map** | 6 cube faces drawn separately (we already solved this, see the cube texture note) | 128 px/face | `.data` `0x7FEE6C` = **128** ✅ |
| **Rear-view mirror** | a separate eView (`ecx==3`) | 256×256 | `.data` `0x7FEE80/84` = **256/256** ✅ |
| **Road reflection** | a separate eView (`ecx==4`), the mirrored scene is drawn into a render target, then blurred | **320×240** | `CreateTexture(320,240,…,usage=RT)` three times: `0x5BA099`, `0x5BA0DB`, `0x5BA10F` ✅ |

eView ids (from the mods): `1` main view, `3` rear-view mirror, `4` road
reflection. The game's settings are already at the highest:
`g_RoadReflectionEnable=3`, `g_CarEnvironmentMapEnable=3`, `g_FSAALevel=-1` (we
have no MSAA at all).

On a 1920×1080 screen the road reflection goes from 320×240 → **upscaled 6×
horizontally and 4.5× vertically**. This is the real reason for the blurry,
blocky reflection on wet ground. It is also why the community's most downloaded
graphics mod exists.

### ⚠️ Key trap: our exe is not the exe the mods target

HD Reflections wants "v1.2 HOODLUM speed2.exe, **4,800,512 bytes**". Ours is
**4,788,224 bytes** (SHA `8f6a9ae2…`). Both have their entry point at
`0x75BCC7`, so even the mod's own version check can't tell them apart. Measured:

* **The `.data` addresses match:** `0x7FEE6C`=128, `0x7FEE80`=256, `0x7FEE84`=256.
* **The code addresses are shifted:** the road reflection immediates are at
  `0x5BA08A/8F` in the mod, `0x5BA09A/9F` in ours (**+0x10**). Other hook points
  are shifted by other amounts (e.g. `0x5CAEC3` is 5 bytes before a `call` in
  ours).

**Conclusion:** **never use** the code addresses from the mods directly. Find each
one again by byte signature (`hook::pattern`, as Widescreen Fix and Rain Droplets
do). `.data` addresses can be used once they've been verified by reading their
values.

## 3. How these techniques map to our port

| On PC | Here | Note |
|---|---|---|
| Writing to `.data`/`.rdata` | `MW32(address)=value` before the game starts | The cheapest path. Cube and mirror resolution are in this class. |
| Changing an immediate in code (`push 0x140`) | **Doesn't work**: in the lifted C the immediate has been compiled into a constant, writing to guest memory has no effect. Options: (a) catch it in the D3D layer, (b) apply a byte patch during lifting, (c) rewrite the function in C with `override_patch.py` | There is no byte patch mechanism yet (no PATCH in `lift.py`/`genall.py`). |
| Code cave (`MakeJMP`) | `override_patch.py` + `overrides.c` (`sub_X` → `sub_X_orig` + our wrapper) | This mechanism already exists and is in use. |
| d3d9 proxy / ReShade | `com.c` + `metal.m` themselves | We already have depth, all render targets, shader bytecode and VS constants. |
| RTX Remix scene capture | Our draw queue + the camera matrix directly from guest memory | We can read from the game's data structures the information Remix has to guess. |
| Texture pack (replacing TPKs) | No change needed | We read the same files. |

## 4. What we can do: suggested order

Context: the GPU is **20% busy** at 3.3 ms per frame, the bottleneck is the lifted
x86 code (see the frame budget note). So there is **~13 ms of free budget** on the
GPU side. Most of the effects below are effectively free.

### Phase A: quick wins (on the order of days, objectively verifiable)

1. **Road reflection resolution (the biggest visual gain).** In `CreateTexture`,
   `usage&RT && 320×240` → the device size (or a `REFLRES` multiplier). The same
   as what we do for fullscreen targets for `NFSRES`. In addition, while this
   target is bound, the `SetViewport`/`Clear` rectangles must be scaled by the
   same ratio. If sampling uses screen-space coordinates, nothing else is needed.
   **Verification:** wet ground with `DUMPFRAME`; the RT size is logged.
2. **Cube map 128 → 512, mirror 256 → screen/2.** A `.data` write, two lines.
   Since there are 6 cube faces, the cost is 6× the face size; there is GPU budget.
3. **MSAA (4×).** The game has a `g_FSAALevel` setting, but we have no sample
   count at all. The main target and Z become 4×, and the main target is
   `resolve`d before the game samples it (`StretchRect`, post-process). Widescreen
   Fix's "HideAABug" note (a 1 px edge bug with MSAA) can be expected here too.
4. **Forcing 16× anisotropic filtering.** The code exists (`maxAnisotropy`); it's
   enough to make the default 16 and verify it on road textures.
5. **Console Gamma / color mapping.** The PC version is paler than the consoles;
   WFP fixes this with a post-process shader (MIT). A single fullscreen pass
   before presentation.

### Phase B: improving the game's own effects (on the order of weeks)

6. **Bringing back missing content in reflections** (HD Reflections's
   RestoreSkybox / RestoreHeadlights / LOD features). Needs code patches: the
   addresses are found by signature and applied with `overrides.c`. GPL code is
   not copied, the behavior is rewritten.
7. **Better road reflection blur.** The game's blur is tuned for the low
   resolution ("doesn't scale well at high resolution"). A distance-dependent
   (roughness) Kawase blur at high resolution.
8. **Xbox rain droplets.** Port the Rain Droplets (MIT) logic: read the rain
   object's intensity (`+0x4D48`) and tunnel flag (`+0x4D04`), draw the drops in
   the post-process. Since the offsets are within a struct they probably match;
   the function addresses must be found by signature.
9. **Screen-space effects** (what ReShade does): SSAO (we have the Z texture; the
   projection from the `vs_pos_creg` constants), SMAA/FXAA, CAS sharpening, bloom.
   All of them can be toggled within a single frame (A/B measurement possible).
10. **HD texture replacement.** An XXH3 hash in `LockRect/UnlockRect` → if
    `textures/<hash>.dds` exists on disk, load it (the Dolphin/Remix method).
    Texture packs and AI-upscaled textures without modifying the game files.

### Phase C: engine level, new capabilities (on the order of months, research)

11. **MetalFX.** The spatial upscaler is easy (free sharpness on a Retina 5K). The
    temporal upscaler and **frame interpolation in macOS 26** need motion
    vectors. Since our bottleneck is the CPU, interpolation could double the
    perceived smoothness. Motion vectors need the previous frame's WVP stored per
    draw (matching draws across frames is hard but not impossible).
12. **Hybrid ray-traced reflections (Metal RT).** We do what Remix does,
    selectively: real reflections for car paint and wet roads. Needed: world-space
    geometry. The game sends a combined WVP, but if the camera ViewProj is read
    from the eView structure in guest memory, World = WVP·(VP)⁻¹ can be derived.
    Where Remix struggles to get this information, we have the advantage.
    **Constraint:** the M1 Pro has no hardware RT (M3+ does); software RT on M1
    would be slow. The culling problem Remix ran into exists for us too
    (offscreen objects are never submitted; they appear missing in reflections).
    Culling has to be relaxed with an override.
13. **Dynamic lights** (neons, street lamps, headlights as real lights). This is
    the most impressive part of the Remix mod, but it's content work that needs
    hand-placed lights. Item 12 first.

## 5. What we shouldn't do / risks

* **Copying GPL code.** HD Reflections is GPL-3. Address, constant and behavior
  knowledge can be used, the code must be rewritten. WFP and Rain Droplets are
  MIT (attribution is enough).
* **Using mod addresses directly.** The exe is different (see §2).
* **Starting with a subjective goal.** The graphics verification limit note
  applies. Every item has a defined objective signal: an RT size log, a
  `DUMPFRAME` comparison, a switch that can be toggled within a single run, the
  `metal_budget_report` cost.
* **Memory pool patches (ExpandMemoryPools).** The mod's address `0x8F5790` falls
  into the unnamed 5th section (reloc data) in our exe. This patch can't be
  applied here without verification.

## 6. Implemented: the HD build and reflection resolution (2026-10-04)

**Structure.** `link.sh` now produces two binaries: `out/nfsu2_boot` (original)
and `out/nfsu2_hd_boot` (HD). The shards are shared, the difference is only in
the runtime: `runtime/enhance.{h,c}` and hooks guarded by `NFS_ENHANCED`. The
original binary has no HD code (`strings | grep '[HD]'` → 0). `./run.sh hd`,
`./pack.sh --hd` → `NFSU2 HD.app` (separate bundle id, separate log
`nfsu2-hd.log`).

**Method.** The game's logic isn't touched: the targets are recognized by the
**return address** of `CreateTexture` / `CreateCubeTexture`, only the Metal
texture is enlarged, and the game keeps seeing the old size. Since we already set
the viewport to the whole target, the scene fills the large texture.

| Target | Call | Original | HD default | Env |
|---|---|---|---|---|
| road reflection (+ blur target) | `0x5BA0A7`, `0x5BA11D` | 320×240 | back buffer (1920×1080 at 1080p) | `HD_ROAD=1.0` |
| rear-view mirror | `0x5BA1DD` | 256×256 | 1024×1024 | `HD_MIRROR` |
| car cube map | `0x5BA559` | 128/face | 512/face | `HD_CUBE` |

`HDREFL=0` turns all of them off within the HD build (for A/B).

**Blur trap (measured).** The game blurs the road reflection in two passes with
ps 19 + vs 5 (13→14→13): 4 taps, offsets fixed in UV relative to 320 pixels. When
the texture grew, the taps landed 6 pixels apart and a **vertical striped
pattern** appeared. Since the same ps/vs pair is also used in the glow chain
(640×480 targets), the fix is only applied to "road target to road target" draws:
the road targets are created with a full mip chain, the source's mips are
generated before the blur draw (`generateMipmapsForTexture`), the shader reads
from level L and scales the tap offsets about the center by `s = 2^L/scale`
(P.pad0/pad1; 0 for every other draw). The tap spacing always stays ~1 texel.
`HD_BLUR`: 1 = the game's blur width (smoother), 0 = sharp, **default 0.5**.

**Verification** (Quick Race, 1080p, frame 6200, `DUMPTEX=13`): reflection texture
320×240 → 1920×1080, no striped pattern, the three `HD_BLUR` values order as
expected. Z size mismatches 0. GPU (run mean): 2.84 → 3.50 ms/frame.

**Still open:**
* Resort Loop is a dry track; in the final frame very little road reflection is
  blended in; the difference is clear in the reflection texture but small in the
  full frame. A visual comparison **on wet ground / in rain** is needed.
* The rear-view mirror isn't visible from this camera; the 1024 texture is
  created but its content wasn't verified.
* The cube map is sampled without mips at 512; there may be shimmering (aliasing)
  on small cars. If needed, a mip chain for the cube too.

## 7. Implemented: 4× MSAA (HD build, 2026-10-06)

`HD_MSAA=4` is the default (2/8 also work, 0/1 off). The code is in `metal.m`
("HD: MSAA for the screen").

* **Only screen passes are multisampled.** The scene and UI are drawn directly to
  the back buffer; the render targets (reflections, post-process, cubes) stay
  single-sampled. Every screen pass is resolved into the drawable with
  `StoreAndMultisampleResolve`, so the copy StretchRect reads from the back
  buffer is always the resolved image. Blits to the screen (0 in a race) are
  drawn into the multisampled texture as quads.
* **Bit 48 of the pipeline key = sample count.**
* **Z trap (measured).** Every Z surface has a 4× twin. In the first version the
  twins didn't share content, and the game's **depth-tested fullscreen quad**
  (ps 17 / vs 11, to a 1920×1080 target, with the screen's Z) couldn't see the
  scene's depth ~11 times per frame. Solution: the MSAA pass also **resolves**
  depth into the single-sampled Z (`depthResolveFilter = Sample0`;
  `HD_ZRESOLVE=0` turns it off). A pending `Clear(Z)` is consumed separately for
  the two twins. The opposite direction (a single-sampled pass writes Z and the
  screen reads it) is tracked with a counter: 0.
* **Verification.** The Metal validation layer is clean. The MSAA on/off
  difference is only at geometry edges; on flat areas the mean difference is
  0.9/765.
* **Cost** (A/B/A/B, `tools/perf/bcmp.py`, race, 1080p): frame time **+0.2%** (noise,
  ~119 fps); GPU busy ~+1.0 ms/frame.
* **Limit.** Alpha-tested leaves and fences (`discard` in the shader) aren't
  smoothed by MSAA. Next step: alpha-to-coverage + alpha sharpening for these
  draws.

## 8. Implemented: SSR + reflection separation infrastructure (HD build, 2026-10-07)

The goal is twofold: screen-space reflections (SSR) today, and ray-traced
reflections on the same infrastructure later. `HD_SSR=0` turns it off,
`HD_SSR_DEBUG=1` visualizes it (green hit, purple fallback to the game's flat
reflection, red edge fade). `HD_SSR_STEPS=48`, `HD_SSR_MAXD=120`.

**Camera (measured).** The game only gives `SetTransform` identity matrices; the
matrices are written directly into VS constants. The source is `sub_005c7ed0`
(per-view projection setup; thiscall, ecx = matrix structure, `[esp+4]` =
eView). `eView+4` is the id (1 main), `eView+0x40` the Camera* (`+0xbc` near,
`+0xc0` far, `+0xc4` FOV); `ecx+0` view, `+0x40` projection, `+0x80` product. The
`overrides.c` wrapper records the main view's matrices into the frame.
Verification: the product is the same as the `c0..c3` constants of the world
geometry draws (x column ×0.75 = our Hor+). Near 0.5; world up is +Z.

**Where the reflection is (measured).** Road: ps 5 / ps 6 sample the road
reflection texture at stage 1/3; car: ps 265 samples the cube map at stage 0. All
three are **linear** in the reflection sample, with no `saturate` in between:
`output = A + B · reflection` (for the road B = texture alpha × vertex alpha).

**Separation.** In HD these draws switch to a reflection variant (ps +
512·(1+stage), generated on demand). The body runs three times (real /
reflection=0 / reflection=1) and writes `B = o₂ − o₁` (including fog, RGBA16F)
and the original `R0` (RGBA8) to the G-buffer. Other opaque draws write zero (a
surface in front covers the road's data), blended ones don't write. When the main
scene ends (the first StretchRect from the screen), a composite pass marches a ray
through depth; on a hit `color + B·(R_new − R0)`, otherwise the game's flat
reflection stays (the correct fallback for offscreen content). **When ray tracing
arrives only the source of `R_new` will change.**

**Verification.** The Metal validation layer is clean; 1 composite and ~26
reflection draws per frame. In the visualization most of the road surface is a
hit; fallback when the ray goes under the car and at the screen edge.

### 8b. Car paint (2026-10-11)

`HD_SSR_CAR=0` turns it off, `HD_SSR_CARLOD=1.0` is the reflection softness (the
mip level of the scene copy).

* **Variant:** the cube variant of ps 9 (265) → 777. A third G-buffer target
  (RGBA16F) carries the cube sampling vector; this is the reflection direction the
  game computes in its vertex shader.
* **Direction space (measured, trap).** The cube map's six face cameras (views
  16–21) are aligned NOT to the world axes but to the camera/car frame. Cube
  vector: +x = right, +y = back, +z = up. Conversion to world uses the forward
  directions of the face cameras: +x → view 17, +y → view 18, +z → view 21 (from
  `sub_005c7ed0` every frame).
* **Measurement trap.** The visualization colors go through the game's
  post-process (tint, glow); reading numbers from colors is MISLEADING (in the
  first attempt I thought +z was down). Reliable test: `HD_SSR_DEBUG=3` — agreement
  between the normal derived from the reflection direction `normalize(R − V)` and
  the geometric normal from depth, in two colors (green agrees / red opposite).
  Result: 62% of the body agrees; the red bands are thin ridges (smoothed vertex
  normal ≠ surface normal).
* **Self-intersection:** the ray starts offset by 4 cm along
  `N = normalize(R − V)`; on the car the step is 8 cm, range 60 m.
* **Result.** Viewed from behind, the trunk and bumper reflect the road behind
  (hit); the roof and glass face the sky and the cube map stays.
* **Limit.** SSR can only reflect what's visible on screen; most of the car paint
  faces up/sideways. The real gain here will come with ray tracing — the same
  G-buffer (B, R0, direction) will be used directly.

## 9. Measured: how long ray tracing takes on this machine (2026-10-11)

`HD_RTPROBE=1` + `DUMPFRAME=<frame>`: runs once at the selected frame, prints a
report, writes two PNGs (`…99001` primary rays, `…99002` reflection rays). The
code is in `metal.m` "RAY TRACING TIME MEASUREMENT". Apple M1 Pro (no hardware
RT), 1920×1080, Quick Race frame 6200, two runs:

| Step | Time |
|---|---|
| Capture (CPU): 518 draws → **18,989 triangles**, world space | **0.20 ms** |
| Acceleration structure, default build | 2.6 ms |
| Acceleration structure, fast build (`PreferFastBuild`) | 1.7 ms |
| Acceleration structure, refit | **0.13 ms** |
| Primary rays, all pixels (2.07 M rays) | 3.05 ms (~680 M rays/s) |
| **Reflection rays, road + car pixels (852 thousand rays)** | **2.0 ms** |

**Conclusion.** In the worst case (a full rebuild every frame + reflection rays)
~4 ms of GPU. The frame is 8.35 ms (CPU-bound), the GPU is currently ~5.8 ms busy;
so ray-traced reflections stay far above 60 fps. The reason is the small scene:
a 2004 game, ~19 thousand triangles in view.

**Capture method.** The vertex shader isn't run: since `oPos = p·WVP`,
`world = p·(WVP·(VP′)⁻¹)`; WVP is the draw's `c[cx..cx+3]` constants
(`vs_pos_creg`), VP′ the camera with Hor+ applied. The product is in double
precision (for the near 0.5 / far ~16 km cancellation error). The primary ray
image overlaps the game's frame → the capture is correct.

**What this measurement does NOT cover (the real remaining work):**
* **Shading the hit point.** The measurement is intersection only. Color needs
  texture + UV + vertex color per triangle. The game's lighting is baked into the
  vertex color (ps 5: `2·texture·vertex color`), so no lighting computation is
  needed; the cost is a texture array + a few samples.
* **The road isn't in the structure.** vs_4 / vs_14 don't produce the position
  with the `c[cx..cx+3]` pattern (45 draws skipped); the position path for these
  two VSs must be solved separately.
* **No out-of-view geometry.** The game culls behind the camera; the scene behind
  the car stays missing in the reflection (Remix's problem too). Solution: relax
  the culling with an override, or the cube map as a fallback.
* **Alpha-tested geometry** (trees, fences) was counted as opaque; the right way
  is to read the texture alpha at the intersection (slower).
* 116 draws that don't write Z (light shafts, particles) are deliberately left out.
* The image of the measurement frame comes out broken: since the measurement waits
  for seconds, that frame's draw buffer gets overwritten (a diagnostic side effect
  only).

## 10. Implemented: ray-traced reflections, complete (HD build, 2026-10-11)

The measurement in §9 was intersection only and left four things missing (hit
shading, the road, out-of-view geometry, alpha testing). All of them were done;
ray tracing is now the **default** in the HD build (`HD_RT=0` turns it off and
falls back to SSR). Code: `metal.m` "RT kernel" + "STATIC SCENE CACHE", `vsemu.c`
`vs_to_msl` (the capture kernel), the G-buffer from §8.

### Pipeline (per frame)

1. **Capture (GPU).** The body of every vertex shader is generated a second time
   as a compute kernel `vsk_N`. The kernel reads the draw's raw vertex data and
   constants and runs the game's VS EXACTLY; from the output it writes the world
   position (`view = (x/P00′, y/P11, w)`, `world = view·V⁻¹`: doesn't hit the
   z−w cancellation) and the attributes (vertex color, 3 UV sets, fog). So the
   road (vs_4/vs_14), skinned models and UV-generating VSs need no special case:
   "whatever the VS produces".
2. **Material.** Per draw: 3 textures, PS class, alpha reference, PS constant c0,
   fog color, projection modes. Textures go into a 512-entry bindless (argument
   buffer) table per frame.
3. **Acceleration structure.** Two sub-structures + an instanced top-level
   structure: *instant* (fast build every frame; cars, skinned meshes, newly
   visible things) and the *static store* (below).
4. **Tracing (compute kernel `hd_rt_kern`).** A reflection ray for every pixel in
   the G-buffer carrying the road or car class: on the road the normal is +Z, on
   the car it comes from the game's cube lookup vector (`N = normalize(R − V)`).
   On a hit the game's PS is reconstructed: `2·texture·vertex color`, on surfaces
   lit by headlights (ps 4/6) `light texture × falloff ramp × c0 + vertex color`,
   then the game's fog formula. On an **alpha-tested** surface (trees, fences), if
   the texture alpha is below the reference the ray continues through the surface
   (up to 5 layers). The texture mip comes from the ray length.
5. **Composite.** The separation from §8: `color + B·(R_new − R0)`. For a ray that
   misses, the game's own reflection stays.

### Half resolution

The kernel alone takes 4.55 ms, but within the frame at full resolution the GPU
went up to ~15 ms and made the game GPU-bound (~74 fps). Rays are cast at half
resolution (`HD_RT_SCALE=2`; the default at 1080p, the scale grows automatically
at higher resolutions, see §11), with a **depth- and class-aware** 4-tap upscale
in the composite: the car edge doesn't bleed into the road. `HD_RT_SCALE=1` is
full resolution.

### Static scene cache: behind the camera

The game doesn't draw what the camera doesn't see; the car's rear window/trunk
reflect exactly what's behind the camera. The solution isn't to touch the game's
culling but to **remember** what was seen:

* Draw = (hash of the raw vertex data `gh`, world matrix `W`). If the same draw is
  seen in the same place for 12 **consecutive** frames it counts as static; its
  captured vertices are copied into the static store on the GPU (no read-back to
  the CPU) and it leaves the instant capture. The static sub-structure is rebuilt
  at most once every 30 frames.
* Cars (cube-mapped PS) and skinned meshes never go in.
* The key is `gh` + texture; **no VS id**: the headlight-lit (vs_13/vs_14 + ps
  4/6) and unlit draws of the same piece are a single entry.
* **Headlight lighting isn't frozen.** The static copy is stored as an unlit base.
  If the piece is drawn lit in that frame, the draw's light matrix (VS constants
  `c[K..K+3]`, found with `vs_light_creg`; object → light) is converted to world
  space with `W⁻¹` and written into the material; the kernel builds the headlight
  term at the hit point (`oT1 = p·L`, `oT2.x = 0.005·q.w + 0.5`). A piece not
  drawn has no headlight term. Fog color, PS constant etc. are also refreshed
  every frame they are seen.
* **Eviction rules:** (a) *far*: farther than 900 units and not seen for 30
  frames (anything farther than 800 is never promoted); (b) *near*: right in front
  of the camera (< 50 units), in the middle of the view and not drawn for 60
  frames = really gone; (c) *sibling*: the same geometry was drawn at the same
  point with a different texture or a different orientation (an animated texture,
  a tree billboard turning to the camera, a rotating object) → the old one is
  evicted.
* **Volatility:** every sibling change doubles the entry's promotion threshold
  (12, 24, … 768 frames); something that keeps changing stays in the instant
  capture.
* When dead ranges exceed 40% of the store, the live ones are compacted on the GPU
  into the other buffer pair (ping-pong; the store isn't reset).
* For static hits the fog is recomputed relative to the current camera, not the
  recorded one (the fog constant found with `vs_fog_creg`).

**Four bugs were found here, all four by measurement** (`HD_RT_LOG`,
`HD_RT_DUMPTAB`, the cache on/off image difference with `HD_SSR_DEBUG=4`):

1. *The "in view but not drawn → evict" rule was wrong.* The game also culls
   things inside the view (it doesn't draw a small object far away). While the
   camera roamed, 300+ unjustified evictions/re-promotions in 300 frames.
2. *Precision of W.* `W = M·(VP′)⁻¹` gives the homogeneous component from the
   `(z − Q·w)/(−Q·near)` cancellation with ~1e-4 error; multiplied by the camera
   position (hundreds of units), W's translation wobbled by 0.03–0.1 units
   depending on the camera. The same object counted as a separate entry at every
   camera angle and piled up in the store: **878 of 1771 entries were copies**.
   Fix: the same path as the capture kernel, homogeneous column exactly (0,0,0,1).
3. *Billboards turning to the camera.* Trees turn to face the camera; every time
   the camera stopped, the new orientation became a separate "static" (12 copies
   of one tree). Piles of alpha-tested copies made the ray restart over and over
   and pushed the GPU from **8 → 22 ms** (the game dropped to ~45 fps). The
   sibling rule was extended to cover orientation → 8 copies (all legitimate).
4. *Frozen headlight lighting.* The static copy carried the light UVs as they were
   at promotion time: after the opponents pulled away from the start line, the road
   stayed bright in the reflection (mean difference 10/255 in the car region
   against the cache-less reference). The first fix (re-capture lit draws every
   frame) didn't work: the game draws almost the whole near scene with the lit VS
   (255 draws at the race start, all the player's headlights: texture 204), GPU
   7.1 → 10.9 ms. The permanent solution is the world-space headlight term above:
   difference 0.8/255, GPU 7.0 ms.

### Measurement (M1 Pro, 1920×1080, 4× MSAA, Quick Race, 7500 frames)

| | GPU busy (mean) | race frame time (median) | triangles per frame | instant draws |
|---|---|---|---|---|
| RT off (`HD_RT=0`, SSR) | 5.87 ms | 8.3–8.4 ms | – | – |
| RT, cache off (`HD_RT_CACHE=0`) | 8.08 ms | 8.3–8.6 ms | 19,375 | 406 |
| **RT, cache on (default)** | **7.06 ms** | **8.3–8.4 ms** | 34,716 (8,692 instant + 26,024 static) | 134 (~110 in a race) |

The frame time is the same in all three (the game is CPU-bound, ~120 fps); the
cost of ray tracing is ~1.2 ms on the GPU. The cache both brings back what's
behind the camera and lowers the GPU load, since it cuts the draws captured every
frame from 406 to ~110 (8.08 → 7.06 ms). At the race start (camera still) churn is
zero: 0–5 promotions, 0–2 evictions in 300 frames; over the run 933 promotions, 66
evictions, 33 static builds, 0 resets. The validation layer
(`MTL_DEBUG_LAYER=1`) is clean; the compaction and re-hashing paths were tested in
the same run by forcing them with `HD_RT_STRESS=400` (14 times each).

**Does what's behind the camera really come through?** `HD_SSR_DEBUG=4`
(reflection only; miss = navy), frame 7400, the car body (280×290 pixels): missing
pixels 7,720 with the cache off, 6,848 with it on. So 11% of the misses now hit
geometry the camera doesn't see (the lower edge of the bumper, corners); the
remaining misses are the sky (not in the structure since it doesn't write Z → the
game's cube map). The gain is small in this scene, because most rear surfaces
reflect the road between the camera and the car; the real difference is expected
with buildings left behind while driving (below, "Limits").
Images: `docs/img/rt_on_off.png` (RT off/on),
`docs/img/rt_reflection_only_cache.png` (reflection only; top cache on, bottom
off), `docs/img/road_horplus_fix.png`.

### The road Hor+ bug (affects both builds)

Found during capture: `vs_pos_creg` didn't recognize VSs that build the position
through a temporary register (`dp4 rN.c, v0, cK` + `mov oPos, rN`: the road,
vs_4/vs_14) → the road didn't get the Hor+ correction and was drawn 33% too wide
horizontally at 16:9 (the start line checkers and lane lines were offset). It was
rewritten to track temporaries; the original build got fixed too.

### Limits (honestly)

* **Single bounce, sharp reflections.** No roughness; the road's blur comes from
  the game's own B term. No reflections within reflections (the paint of a car
  that's hit has no cube map, flat texture × vertex color).
* **The road normal is assumed to be +Z** (on sloped ramps the reflection shifts
  slightly).
* **No geometry that was never seen.** The cache only remembers what the camera
  has seen once; on a road driven for the first time, a building behind isn't in
  the reflection if the camera never looked at it (on a miss the game's cube map
  stays).
* **Draws that don't write Z** (light shafts, particles, halos) aren't in the
  structure.
* **No moving test.** AUTOSEQ can't drive the car; the measurements are based on
  the static camera at the race start + the pre-race camera fly-around (moving
  camera, ~480 promotions in 300 frames). Behavior while driving must be tried by
  hand.
* Requires macOS 13+ (the ray tracing API); on older systems it falls back to SSR.

## 11. Measured and fixed: where GPU time goes in the HD build (2026-10-11)

Question: "what is draw time spent on, there seems to be unnecessary work". The
answer was given not by guessing but with two tools (both exist in both builds):

* `GPUWIN=a,b`: the GPU/CPU-encoding mean only for frames `[a,b)` (so the menus
  don't dilute the mean). Race: `GPUWIN=6000,7300`.
* `PASSPROF=N` (+ `PASSPROF_MODE=1`): the GPU time of **every pass** on every Nth
  frame in the window, with the Metal counter sampler; vertex and fragment stages
  separately. The pass order and contents of the first profiled frame are dumped
  too.
* Helpers: `HD_COMP=0` (skip the composite), `HD_RT_AB=1` (toggle the ray tracing
  steps in 100-frame slices within the same run).

**Measurement traps (all of them happened):**
* Sampling compute/blit/acceleration passes (`PASSPROF_MODE=3`) slows the GPU
  down ~6×; only render passes (`MODE=1`) don't disturb the measurement.
* Counter samples are directly in ns on the Apple GPU; `sampleTimestamps` however
  are mach ticks (41.67 ns). Mixing them up makes times come out 41×.
* The GPU lowers its frequency under light load: the same pass "takes" longer in
  the original build than in HD. ms differences show a feature's cost SMALLER than
  it is.
* At 1080p the GPU sits idle (the frame is CPU-bound, 8.3 ms) and everything looks
  "free". The real cost shows at the screen's native resolution (3024×1964).
* The weather is random: a rainy run is a completely different load (below).

### Structure of the frame (race, HD default, before the fix)

| order | pass | note |
|---|---|---|
| 0 | SCREEN +MSAA +G | **Clear only** |
| 1–6 | road reflection (screen size) + blur + 2 mip generations | |
| 7–12 | cube map, 6 faces 512² | |
| 13 | SCREEN +MSAA +G | scene: 674 draws |
| 14 | offscreen, screen size | light glows: test the scene's Z, don't write Z |
| 15 | SCREEN +MSAA +G | **a single draw** |
| 16–20 | ray tracing (capture, structure, kernel) + composite | |
| 21–38 | post-process chain (640×480 and screen size) | |
| 39–40 | SCREEN +MSAA | post-process quad, UI |

EACH of the five multisampled screen passes stored and resolved color + 3
G-buffers + Z at 4×. In the 1080p profile the screen passes were 4.6 ms of 9.7 ms
(48%); ray tracing itself is smaller than that.

### Three fixes (`metal.m` "MSAA: unnecessary store / resolve")

1. **A screen pass containing only a Clear isn't opened** (the next screen pass
   clears on first use anyway).
2. **Lazy store/resolve.** The resolve decision is made when the pass is opened
   (Metal rule: if a resolve target is bound, the action must include a resolve),
   the store decision when it closes: if another multisampled screen pass follows,
   store only; if a copy reading the screen or the end of the frame follows,
   resolve only; the G-buffer is resolved only right before the composite. The
   screen passes after the first copy from the screen (post-process quad, UI) and
   the composite are single-sampled: there all samples carry the same value
   anyway. `HD_MSLAZY=0` is the old behavior.
3. **The intermediate pass that splits the scene is deferred** (`hd_reorder`). If
   the pattern is `screen | R (offscreen, doesn't write Z) | S (screen, doesn't
   write Z, doesn't sample R's target)`, R and S swap places: the result is
   identical pixel for pixel, and the scene is a single pass. `HD_REORDER=0`
   turns it off.

Multisampled screen passes per frame 5 → 1. The image is the same (old/new mean
difference ≤ 0.5/255 in menu and race frames, at the level of timing noise),
anti-aliasing is preserved, the validation layer is clean, and the counter of
passes where the prediction didn't hold is 0.

### Result

1080p, race window, GPU ms/frame (frame time 8.3–8.4 ms in all):

| | before | after |
|---|---|---|
| original build | 3.6–3.9 | (unchanged) |
| HD: reflection resolution + MSAA | 6.0–6.3 | 5.3 |
| HD: + SSR | 6.8 | 5.8 |
| HD: + ray tracing (default) | 9.3–9.9 | **6.2** |

The screen's native resolution 3024×1964, dry weather, frame rate:

| | before | after |
|---|---|---|
| original build | 120 fps | – |
| HD, ray tracing off | 56 fps | **median 120 fps** (mean 9.1 ms ≈ 110 fps) |
| HD, ray tracing on, ray scale 2 | 40 fps | 60 fps (on a 120 Hz screen, exceeding 8.3 ms rounds up to 16.7 ms) |
| HD, ray tracing on, **automatic scale** | – | **median 120 fps** (8.3 ms; mean 10.8 ms, p99 17.5 ms: drops to 60 now and then) |

### Automatic ray scale

Step by step within the same run with `HD_RT_AB=1` (3024×1964, dry, scale 2,
frame time median): without ray tracing 8.3 ms; with rays cast 12.8 ms; with the
composite 16.7 ms. The ray count grew with resolution (half resolution =
1512×982 = 1.48 M rays; 0.52 M at 1080p).

If `HD_RT_SCALE` isn't given, the scale is now chosen by resolution
(`(width + 480) / 960`, between 2 and 4): 2 at 1080p, 3 at 2560 and 3024, 4 at
4K. The ray buffer always stays ~1000 pixels wide. At 3024×1964 with scale 3 and
all steps on the frame is 8.2 ms; in the reflection-only image the mean
difference from scale 2 is 1.1/255 (thanks to the depth-aware upscale).

**Measurement note:** while the frame is below 8.3 ms, two command buffers are in
flight at the same time and `GPUEndTime − GPUStartTime` also counts the queue
wait (21 ms "GPU" in the scale 3 run but an 8.2 ms frame). Near the GPU limit the
right measure is the **frame time**.

### Solved: "17 fps in the rain" was actually a poisoned acceleration structure (2026-10-11, night)

The previous section said "~17 fps in rainy runs, reason unknown". It was NOT the
rain. The order of discovery (wrong leads included, so nobody falls for them
again):

* Rain was forced with `FORCERAIN=1` (below): 80 fps even in rain. The rain is
  innocent.
* Slow runs had an extra vertex shader registered (18–19 instead of 17). This was
  not the cause but the EFFECT: when a run is slow it lasts longer, the game clock
  reaches the 60th second and the sky transition (`skymorph.efx`, vs_18) starts.
  The transition was forced with `FORCETOD=1`: no slowdown.
* A real slow run was caught with `HD_RT_AB=1` (1 in 13 short runs): without ray
  tracing 8.3 ms; with **plain ray casting** (no shading) on, 58 ms; capture +
  structure build 8.3 ms. Draw/triangle counts normal → the acceleration
  structure itself is broken.
* **Root cause:** some of the captured vertices are NaN / infinite or absurdly
  far away. In the race scene ~450 triangles with NaNs + ~200 "farther than
  100 km" per frame; most from the car shader (vs_12), some from skinned models
  (vs_15), a few from world draws (vs_3/vs_13). These are draws that don't appear
  on screen (most likely hidden parts the game leaves with a broken/empty matrix;
  the rasterizer discarded them, the capture didn't).
* **Causality test** (`HD_RT_POISON=<value>`: the check off, a single vertex moved
  to x = value), 3024×1964, frame median: 1e7 → **71 ms**; 1e12 → 16.6 ms. So a
  single vertex ~10⁷ units away makes the structure's boxes useless (when the root
  box becomes gigantic, the precision of the node bounds runs out); much larger
  values are already treated as invalid by the builder. The randomness comes from
  here: it depends on the size of the garbage.

**Fix** (`metal.m` `hd_rt_fix`, on by default, `HD_RT_FIX=0` turns it off): right
after capture, before the structure is built and before copying into the static
store, a compute pass looks at every triangle; a triangle with a NaN/infinite
vertex, or one with any component beyond 100 km, is degenerated (all three
vertices = 0). The counters are in the report (`[HD] RT vertex check`) and per
shader in the `HD_RT_LOG` output.

**Verification status (honestly):** with the check on two runs were fast and the
counters work; the causality test proves the mechanism. But the claim "slow runs
no longer happen" needs statistics (the rate without the fix ~8%; ~40 runs for 95%
confidence) and this was NOT done (the computer was about to be shut down). Still
open: the real source of the broken vertices (which draws, whether the game gives
garbage or the capture kernel reads wrong) was not investigated; the check cuts
off the symptom.

**Old measurements deemed invalid:** the 3024×1964 "before" values in this section
(ray tracing on 40 fps, off 56 fps) happened to coincide with poisoned/long runs;
the real gain of the MSAA fix at that resolution must be re-measured. The 1080p
comparison (9.3–9.9 → 6.2 ms) is valid. The previous session's finding "878 copies
in the cache → 22 ms" may also have been partly mixed up with this effect (the
copies were real, but those runs also had 18 shaders).

### Forcing the weather (in both builds, `runtime/overrides.c`)

* `FORCERAIN=1` rain, `2` always dry: `Rain::Update` = `sub_0061f130`
  (`Rain+0x5094` target precipitation, `+0x1f0` intensity) and `sub_0061e390`,
  which computes the target, are wrapped. (Attempts `3`/`4` didn't change the sky.)
* `FORCETOD=1` (+ `FORCETOD_STATE=1..4`): the sky state machine `sub_0060fab0`;
  state `0x8a1c8c`, world counter `0x7fb718` (s), it switches to state 2 when
  `(int)counter % 300 == 60`.
* Setting variables: `g_RainEnable` = `0x870D10` (option only), table
  `0x870ccc…0x870d24`.

## 12. Current state and GPU breakdown (2026-10-21)

Details and measurements are in the "2026-10-20 – 10-24" section of `docs/devlog.md`;
a summary here.

* **MSAA now depends on resolution:** 2x at 1440 lines and above, 4x below
  (forced with `HD_MSAA`). 2x at native Retina resolution.
* **The ray tracing static store is in spatial groups**, with a position check on
  the instant structure's refit (a rebuild from scratch when copies of the same
  model swap places).
* **Motion blur:** when driving fast, the game's mid-scene screen copy is no longer
  counted as the end of the scene; cars have MSAA and reflections (`HD_MBFIX`).
* **Separate G-buffer pass:** the scene MSAA pass has no G, G is in a separate
  single-sampled pass (`HD_GSEP`); GPU work -5.5%.
* **Capacity breakdown** (Quick Race, 8.40 ms/frame at the highest clock, 120 fps
  limit 8.33): MSAA ~3.0 ms (0.9 without G; ~1.7 of it is the motion blur split),
  ray tracing ~2.7 (kernel 2.0), the game's own full-resolution post-process ~2.3.
  The standard build is 3.0 ms in the same scene. HD's heat (86 °C, ~18 W) is the
  natural cost of these features; options that would give up image quality
  (post-process anti-aliasing, a ray distance limit) were not applied, by the
  user's decision.

## Sources

* AeroWidescreen/NFSHDReflections: https://github.com/AeroWidescreen/NFSHDReflections (`SOURCE/NFSU2/dllmain.cpp`, `assembly.h`)
* Zintexiss/NFSU2HDReflections: https://github.com/Zintexiss/NFSU2HDReflections
* ThirteenAG/WidescreenFixesPack (NFSU2): https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/nfsu2
* ThirteenAG/XboxRainDroplets (NFSU2): https://github.com/ThirteenAG/XboxRainDroplets/releases/tag/nfsu2
* ExOptsTeam/NFSU2ExOpts: https://github.com/ExOptsTeam/NFSU2ExOpts
* adamplayer/Underground2RTX: https://github.com/adamplayer/Underground2RTX
* Underground 2 Remix (ModDB): https://www.moddb.com/mods/need-for-speed-underground-2-remix
* DSOGaming, Remix v0.4: https://www.dsogaming.com/mods/need-for-speed-underground-2-rtx-remix-mod-v0-4-released/
* PCGamesN, Remix first release: https://www.pcgamesn.com/nvidia/rtx-remix-need-for-speed-underground-2
* Redux 3.0: https://www.ktmx.pro/2024/06/need-for-speed-underground-2-redux-30.html
* ReShade presets: https://nfsmods.xyz/mod/5569
* Metal 4 / MetalFX frame interpolation: https://developer.apple.com/videos/play/wwdc2025/211/
