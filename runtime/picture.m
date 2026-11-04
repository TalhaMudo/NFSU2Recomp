/* picture.m - optional picture passes on the GPU (launcher "Picture").
 * Every setting is off by default; off draws exactly what the game draws.
 *
 * CONSOLEGAMMA=1  the brightness curve of the Xbox 360 version on the final
 *   image (HUD included, as on the console's display).  The PC version looks
 *   paler and flatter; the curve deepens the dark tones.  It is ThirteenAG's
 *   X360GammaApprox from NFSUnderground2.WidescreenFix / WidescreenFixesPack
 *   (MIT, includes/postfx/postfx.fx).  It runs as one extra draw over the
 *   finished frame that reads its pixel in place (tile memory), so the frame
 *   is not copied.
 *
 * EDGEAA=1  smooths jagged edges of the 3D scene before the HUD is drawn
 *   (the HUD stays crisp).  Only GEOMETRIC edges are touched: the scene's
 *   depth buffer is still intact at that point, and a pixel counts as an
 *   edge only where the depth is not planar around it.  1 - z is affine in
 *   screen space on any flat surface, so its second difference across the
 *   pixel is ~0 on a wall, a road or a sign however busy its texture is, and
 *   large on a silhouette (car, pole, wire, roof line, leaves).  There the
 *   pixel is blended along the edge, the direction coming from the luma
 *   gradient: the directional filter of Timothy Lottes' FXAA (console
 *   variant), written anew.  Textures (windows, road markings, signs) stay
 *   as sharp as the game draws them.  Without a usable depth buffer the
 *   filter falls back to luma only, with FXAA's stricter "high quality"
 *   thresholds.  It cannot recover sub-pixel detail like the HD engine's
 *   MSAA.
 *
 * SHARPEN=<percent>  contrast adaptive sharpening of the 3D scene, also
 *   before the HUD: each pixel is pushed away from its four neighbours by a
 *   weight that shrinks where the neighbourhood already has strong contrast,
 *   so flat areas sharpen and edges do not ring.  The idea and the weight
 *   formula follow AMD FidelityFX CAS (MIT); the shader is our own.  100 =
 *   CAS's strongest setting (-1/5), 50 = half of it.  Pixels that EDGEAA
 *   smoothed are not sharpened again.
 *
 * Both run in the same pass: metal.m finds the draw that copies the finished
 * scene to the screen (PICTURE OPTIONS) and lets it read the pass's output
 * instead; the game's own texture is not changed, so effects that read it in
 * the next frame see the game's image.
 *
 * Called from metal.m (PICTURE OPTIONS), on the encoding thread.
 *
 * HD engine only (NFS_ENHANCED): the original engine stays the 2004 picture,
 * so there these calls are empty and the variables are ignored.
 *
 * X360GammaApprox (the curve in pic_gamma below), from WidescreenFixesPack:
 * MIT License, Copyright (c) 2018 ThirteenAG
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following
 * conditions: The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS",
 * WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
 * OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if NFS_ENHANCED

static NSString *kPicSrc = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V pic_v(uint i [[vertex_id]]) {\n"
    "  float2 q = float2(float((i << 1) & 2), float(i & 2));\n"
    "  V o; o.p = float4(q * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
    /* ThirteenAG's X360GammaApprox (MIT) */
    "static float x360(float x) {\n"
    "  x = max(x, 0.0);\n"
    "  float f1 = 0.541901 * x;\n"
    "  float f2 = pow(x, 1.13465) * (1.0 - exp2(-13.53054 * x));\n"
    "  float f3 = saturate(x * 6.56649 + 0.311465);\n"
    "  return mix(f1, f2, f3); }\n"
    "fragment half4 pic_gamma(V in [[stage_in]], half4 d [[color(0)]]) {\n"
    "  float3 c = float3(d.rgb);\n"
    "  return half4(half3(float3(x360(c.r), x360(c.g), x360(c.b))), d.a); }\n"
    "struct PP { float2 px; float aa; float sharp; float hasz; float pad[3]; float4 mul; };\n"
    "static float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "static float q1(depth2d<float> z, int2 p) {\n"
    "  int2 m = int2(z.get_width(), z.get_height()) - 1;\n"
    "  return 1.0 - z.read(uint2(clamp(p, int2(0), m))); }\n"
    "fragment float4 pic_pre(V in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        depth2d<float> z [[texture(1)]],\n"
    "                        sampler s [[sampler(0)]], constant PP &P [[buffer(0)]]) {\n"
    "  float2 uv = in.p.xy * P.px;\n"
    "  float4 c0 = t.sample(s, uv);\n"
    "  float3 c = c0.rgb;\n"
    "  bool geo = true;\n"
    "  if (P.aa > 0.0 && P.hasz > 0.0) {\n"
    "    int2 ip = int2(in.p.xy);\n"
    "    float qc = q1(z, ip), ql = q1(z, ip + int2(-1, 0)), qr = q1(z, ip + int2(1, 0));\n"
    "    float qu = q1(z, ip + int2(0, -1)), qd = q1(z, ip + int2(0, 1));\n"
    "    float qm = max(qc, max(max(ql, qr), max(qu, qd)));\n"
    "    geo = max(abs(ql + qr - 2.0 * qc), abs(qu + qd - 2.0 * qc)) > 0.1 * qm + 1e-7;\n"
    "  }\n"
    "  if (P.aa > 0.0 && geo) {\n"
    "    float lnw = luma(t.sample(s, uv, int2(-1, -1)).rgb), lne = luma(t.sample(s, uv, int2(1, -1)).rgb);\n"
    "    float lsw = luma(t.sample(s, uv, int2(-1, 1)).rgb), lse = luma(t.sample(s, uv, int2(1, 1)).rgb);\n"
    "    float lm = luma(c);\n"
    "    float lo = min(lm, min(min(lnw, lne), min(lsw, lse)));\n"
    "    float hi = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "    if (hi - lo > (P.hasz > 0.0 ? max(1.0 / 32.0, hi * 0.125) : max(1.0 / 16.0, hi * 0.166))) {\n"
    "      float2 d = float2((lsw + lse) - (lnw + lne), (lnw + lsw) - (lne + lse));\n"
    "      float bias = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "      d = clamp(d / (min(abs(d.x), abs(d.y)) + bias), -8.0, 8.0) * P.px;\n"
    "      float3 a = 0.5 * (t.sample(s, uv - d * (1.0 / 6.0)).rgb + t.sample(s, uv + d * (1.0 / 6.0)).rgb);\n"
    "      float3 b = 0.5 * a + 0.25 * (t.sample(s, uv - d * 0.5).rgb + t.sample(s, uv + d * 0.5).rgb);\n"
    "      float lb = luma(b);\n"
    "      return float4((lb < lo || lb > hi) ? a : b, c0.a) * P.mul;\n"
    "    }\n"
    "  }\n"
    "  if (P.sharp > 0.0) {\n"
    "    float3 n = t.sample(s, uv, int2(0, -1)).rgb, so = t.sample(s, uv, int2(0, 1)).rgb;\n"
    "    float3 e = t.sample(s, uv, int2(1, 0)).rgb, w = t.sample(s, uv, int2(-1, 0)).rgb;\n"
    "    float3 mn = min(c, min(min(n, so), min(e, w))), mx = max(c, max(max(n, so), max(e, w)));\n"
    "    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1.0 / 256.0)));\n"
    "    float3 wt = -amp * (0.2 * P.sharp);\n"
    "    c = saturate((c + wt * (n + so + e + w)) / (1.0 + 4.0 * wt));\n"
    "  }\n"
    "  return float4(c, c0.a) * P.mul; }\n";

static int g_gamma = -1;
static id<MTLLibrary> g_lib;
static id<MTLRenderPipelineState> g_gamma_pso;
static MTLPixelFormat g_gamma_fmt;
static unsigned long g_gamma_n;

int pic_gamma_on(void){
    if (g_gamma < 0){ const char *e = getenv("CONSOLEGAMMA"); g_gamma = (e && *e == '1'); }
    return g_gamma;
}

static id<MTLFunction> pic_fn(id<MTLDevice> dev, NSString *name){
    if (!g_lib){
        NSError *e = nil;
        g_lib = [dev newLibraryWithSource:kPicSrc options:nil error:&e];
        if (!g_lib){ fprintf(stderr, "picture: shaders could not be compiled: %s\n",
                             e ? [[e localizedDescription] UTF8String] : "?");
                     g_gamma = 0; return nil; }
    }
    return [g_lib newFunctionWithName:name];
}

static id<MTLRenderPipelineState> pic_pso(id<MTLDevice> dev, NSString *frag, MTLPixelFormat fmt){
    id<MTLFunction> v = pic_fn(dev, @"pic_v"), f = pic_fn(dev, frag);
    if (!v || !f) return nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = v; pd.fragmentFunction = f;
    pd.colorAttachments[0].pixelFormat = fmt;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p) fprintf(stderr, "picture: pipeline %s failed: %s\n", [frag UTF8String],
                    e ? [[e localizedDescription] UTF8String] : "?");
    return p;
}

/* The finished frame in scr (single sampled): apply the console curve in place. */
void pic_gamma(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> scr){
    if (!pic_gamma_on() || !scr) return;
    if (!g_gamma_pso || g_gamma_fmt != scr.pixelFormat){
        g_gamma_pso = pic_pso(dev, @"pic_gamma", scr.pixelFormat); g_gamma_fmt = scr.pixelFormat;
        if (!g_gamma_pso){ g_gamma = 0; return; }
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = scr;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:g_gamma_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    g_gamma_n++;
}

static int g_aa = -1, g_sharp = -1;
static id<MTLRenderPipelineState> g_pre_pso;
static MTLPixelFormat g_pre_fmt;
static id<MTLSamplerState> g_pre_smp;
static id<MTLTexture> g_pre_out;
static unsigned long g_pre_n, g_pre_z, g_pre_noz;

int pic_pre_on(void){
    if (g_aa < 0){
        const char *e = getenv("EDGEAA"); g_aa = (e && *e == '1');
        e = getenv("SHARPEN"); int v = e ? atoi(e) : 0; g_sharp = v < 0 ? 0 : v > 100 ? 100 : v;
    }
    return g_aa || g_sharp;
}

/* The finished scene in src (a render target the size of the screen, about to
 * be copied to the screen) and its depth zt (nil if not usable): returns a
 * texture with smoothing / sharpening applied, for that copy to read
 * instead; nil = use src. */
id<MTLTexture> pic_pre(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> zt){
    if (!pic_pre_on() || !src || src.textureType != MTLTextureType2D) return nil;
    if (!g_pre_pso || g_pre_fmt != src.pixelFormat){
        g_pre_pso = pic_pso(dev, @"pic_pre", src.pixelFormat); g_pre_fmt = src.pixelFormat;
        if (!g_pre_pso){ g_aa = g_sharp = 0; return nil; }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_pre_smp = [dev newSamplerStateWithDescriptor:sd];
    }
    if (!g_pre_out || g_pre_out.width != src.width || g_pre_out.height != src.height ||
        g_pre_out.pixelFormat != src.pixelFormat){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
            width:src.width height:src.height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_pre_out = [dev newTextureWithDescriptor:td];
        if (!g_pre_out) return nil;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_pre_out;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:g_pre_pso];
    [en setFragmentTexture:src atIndex:0];
    int hasz = zt && zt.pixelFormat == MTLPixelFormatDepth32Float && (zt.usage & MTLTextureUsageShaderRead) &&
               zt.width == src.width && zt.height == src.height && zt.sampleCount == 1;
    if (hasz) [en setFragmentTexture:zt atIndex:1];
    if (g_aa){ if (hasz) g_pre_z++; else g_pre_noz++; }
    [en setFragmentSamplerState:g_pre_smp atIndex:0];
    struct { float px[2]; float aa, sharp, hasz, pad[3]; float mul[4]; } P = { { 1.f / (float)src.width, 1.f / (float)src.height },
        g_aa ? 1.f : 0.f, (float)g_sharp / 100.f, hasz ? 1.f : 0.f, { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f, 1.f } };
    [en setFragmentBytes:&P length:sizeof P atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    g_pre_n++;
    return g_pre_out;
}

/* ---- IN-PASS DRAWS (metal.m PICTURE OPTIONS, "folded") -----------------------
 * Measured at 3024x1898 (ABTEST=PICTURE, 2026-11-04): the scene pass cost 0.58 ms and
 * the gamma pass 0.30 ms a frame, mostly writing and reading whole frames.  Both now
 * run INSIDE a pass metal.m has open anyway:
 *  - the scene filter replaces the game's copy of the scene to the screen when that
 *    draw is a plain copy (vsemu.c ps_plain_copy, UVs 1:1): no scratch texture, no
 *    extra pass;
 *  - the gamma curve is the last draw of the last screen pass, reading its pixel
 *    from tile memory: no extra load and store of the frame.
 * The output is the same as before.  The pipelines follow the open pass's
 * attachments; the encoder's depth, cull and buffer state is changed, metal.m drops
 * its cached copies.  PICFOLD=0: the separate passes as before. */
int pic_fold_on(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("PICFOLD"); v = !(e && *e == '0'); }
    return v;
}
typedef struct { const char *frag; MTLPixelFormat cf, df; NSUInteger ns; id<MTLRenderPipelineState> pso; } PicPso;
static PicPso g_ip[16]; static int g_nip;
static id<MTLDepthStencilState> g_ds_off;
static unsigned long g_fold_pre, g_fold_gamma;
static id<MTLRenderPipelineState> pic_pso_in(id<MTLDevice> dev, const char *frag, MTLPixelFormat cf,
                                             MTLPixelFormat df, NSUInteger ns){
    for (int i = 0; i < g_nip; i++)
        if (g_ip[i].frag == frag && g_ip[i].cf == cf && g_ip[i].df == df && g_ip[i].ns == ns) return g_ip[i].pso;
    id<MTLFunction> v = pic_fn(dev, @"pic_v"), f = pic_fn(dev, [NSString stringWithUTF8String:frag]);
    id<MTLRenderPipelineState> p = nil;
    if (v && f){
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = v; pd.fragmentFunction = f;
        pd.colorAttachments[0].pixelFormat = cf;
        pd.depthAttachmentPixelFormat = df;
        if (df == MTLPixelFormatDepth32Float_Stencil8 || df == MTLPixelFormatDepth24Unorm_Stencil8)
            pd.stencilAttachmentPixelFormat = df;
        pd.rasterSampleCount = ns ? ns : 1;
        NSError *e = nil;
        p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!p) fprintf(stderr, "picture: in-pass pipeline %s failed: %s\n", frag,
                        e ? [[e localizedDescription] UTF8String] : "?");
    }
    if (g_nip < 16){ g_ip[g_nip].frag = frag; g_ip[g_nip].cf = cf; g_ip[g_nip].df = df;
                     g_ip[g_nip].ns = ns; g_ip[g_nip].pso = p; g_nip++; }
    return p;
}
static void pic_in_state(id<MTLDevice> dev, id<MTLRenderCommandEncoder> en, id<MTLRenderPipelineState> p){
    if (!g_ds_off){ MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO;
        g_ds_off = [dev newDepthStencilStateWithDescriptor:dd]; }
    [en setRenderPipelineState:p];
    [en setDepthStencilState:g_ds_off];
    [en setCullMode:MTLCullModeNone];
}
/* The scene filter as the draw that copies src to the screen, times mul (the copy's
 * c0, or 1).  zt: the scene's depth (nil if it can't be read in this pass).
 * 1 = drawn (skip the game's draw). */
int pic_pre_draw(id<MTLDevice> dev, id<MTLRenderCommandEncoder> en, id<MTLTexture> src, id<MTLTexture> zt,
                 MTLPixelFormat cf, MTLPixelFormat df, NSUInteger ns, const float mul[4]){
    if (!pic_pre_on() || !en || !src || src.textureType != MTLTextureType2D) return 0;
    const char *fn = "pic_pre";
    id<MTLRenderPipelineState> p = pic_pso_in(dev, fn, cf, df, ns);
    if (!p) return 0;
    if (!g_pre_smp){ MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_pre_smp = [dev newSamplerStateWithDescriptor:sd]; }
    pic_in_state(dev, en, p);
    [en setFragmentTexture:src atIndex:0];
    int hasz = zt && zt.pixelFormat == MTLPixelFormatDepth32Float && (zt.usage & MTLTextureUsageShaderRead) &&
               zt.width == src.width && zt.height == src.height && zt.sampleCount == 1;
    [en setFragmentTexture:hasz ? zt : nil atIndex:1];
    if (g_aa){ if (hasz) g_pre_z++; else g_pre_noz++; }
    [en setFragmentSamplerState:g_pre_smp atIndex:0];
    struct { float px[2]; float aa, sharp, hasz, pad[3]; float mul[4]; } P = { { 1.f / (float)src.width, 1.f / (float)src.height },
        g_aa ? 1.f : 0.f, (float)g_sharp / 100.f, hasz ? 1.f : 0.f, { 0.f, 0.f, 0.f }, { mul[0], mul[1], mul[2], mul[3] } };
    [en setFragmentBytes:&P length:sizeof P atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    g_pre_n++; g_fold_pre++;
    return 1;
}
/* The gamma curve as the last draw of the open screen pass.  In a multisampled pass
 * it runs per sample before the resolve (edges differ by rounding only). */
int pic_gamma_draw(id<MTLDevice> dev, id<MTLRenderCommandEncoder> en, MTLPixelFormat cf, MTLPixelFormat df, NSUInteger ns){
    if (!pic_gamma_on() || !en) return 0;
    static const char *fn = "pic_gamma";
    id<MTLRenderPipelineState> p = pic_pso_in(dev, fn, cf, df, ns);
    if (!p) return 0;
    pic_in_state(dev, en, p);
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    g_gamma_n++; g_fold_gamma++;
    return 1;
}

void pic_report(void){
    if (pic_gamma_on()) fprintf(stderr, "  console gamma    : %lu frames\n", g_gamma_n);
    if (pic_pre_on()) fprintf(stderr, "  scene pass       : edge smoothing %s (depth edges %lu frames, luma only %lu), sharpening %d%%, %lu frames\n",
                              g_aa ? "on" : "off", g_pre_z, g_pre_noz, g_sharp, g_pre_n);
    if (pic_gamma_on() || pic_pre_on())
        fprintf(stderr, "  picture folding  : %s, scene filter in the copy draw %lu frames, gamma in the last pass %lu frames\n",
                pic_fold_on() ? "on" : "off (PICFOLD=0)", g_fold_pre, g_fold_gamma);
}

#else   /* original engine: no picture options */
int pic_pre_on(void){ return 0; }
id<MTLTexture> pic_pre(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> zt){
    (void)dev; (void)cb; (void)src; (void)zt; return nil; }
void pic_gamma(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> scr){ (void)dev; (void)cb; (void)scr; }
void pic_report(void){}
int pic_fold_on(void){ return 0; }
int pic_pre_draw(id<MTLDevice> dev, id<MTLRenderCommandEncoder> en, id<MTLTexture> src, id<MTLTexture> zt,
                 MTLPixelFormat cf, MTLPixelFormat df, NSUInteger ns, const float mul[4]){
    (void)mul; (void)dev; (void)en; (void)src; (void)zt; (void)cf; (void)df; (void)ns; return 0; }
int pic_gamma_draw(id<MTLDevice> dev, id<MTLRenderCommandEncoder> en, MTLPixelFormat cf, MTLPixelFormat df, NSUInteger ns){
    (void)dev; (void)en; (void)cf; (void)df; (void)ns; return 0; }
#endif
