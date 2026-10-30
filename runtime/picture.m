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
 * Called from metal.m (PICTURE OPTIONS), on the encoding thread.            */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    "  return half4(half3(float3(x360(c.r), x360(c.g), x360(c.b))), d.a); }\n";

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

void pic_report(void){
    if (pic_gamma_on()) fprintf(stderr, "  console gamma    : %lu frames\n", g_gamma_n);
}
