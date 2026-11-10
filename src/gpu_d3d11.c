/*
 * Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * This game's statically linked XDK D3D writes NV2A pushbuffers directly, so
 * xboxrecomp's D3D8 API layer never sees its draws. The toolkit's pushbuffer
 * executor decodes them; by default it also rasterises them on the CPU. This
 * file registers an nv2a_backend.h back end instead, so pixels are drawn on
 * the GPU:
 *
 *   - one D3D11 render target per guest colour surface address, one depth/
 *     stencil buffer per depth surface, at real (anti-aliased) pixels times
 *     GZ_RENDER_SCALE;
 *   - vertices arrive in surface pixels with 1/w; the vertex shader turns them
 *     into clip space with w = 1/rhw (perspective-correct colour and UVs) and
 *     z divided by the depth format's maximum (z is compared as given);
 *   - texture x diffuse (stage 0; the executor passes no combiner state yet),
 *     alpha test and the SET_CLIP_MIN/MAX depth range in the pixel shader;
 *   - blend, depth, stencil and colour-mask state mapped from NV2A's GL enums;
 *   - textures decoded by nv2a_backend_decode_texture, cached by address, size,
 *     format and a sampled content hash; a texture at a render target's address
 *     samples that render target (render-to-texture, the AA downsample);
 *   - flip presents the most recently drawn full-size surface to a window.
 *
 * Switches: GZ_GPU=0 (CPU rasteriser instead), GZ_RENDER_SCALE=1..4,
 * GZ_WINDOW_SCALE=1..4 (default 2), GZ_VSYNC=1, GZ_GPU_CULL=1 (per-triangle
 * culling from screen positions; off matches the CPU rasteriser),
 * GZ_GPU_TRACE=1 (first batches and periodic stats).
 *
 * Every call arrives on the NV2A poll thread, one at a time, so the device and
 * window are created lazily there and messages are pumped from flip().
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../../xboxrecomp/src/kernel/nv2a_backend.h"
#include "../../xboxrecomp/src/kernel/nv2a_vsh_interp.h"

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST_PTR(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* ── Settings ─────────────────────────────────────────────── */

static int s_rcdbg;
static int s_scale = 1, s_win_scale = 2, s_vsync, s_cull, s_trace, s_readback;
/* GZ_PACE_LOG: where a slow frame's time went on this thread (microseconds). */
static LONGLONG stat_qpc_us(void);
static LONGLONG s_h_map, s_h_map_max, s_h_present, s_h_last_flip, s_h_tex;
static UINT     s_h_discards;
static int s_aniso = 16;   /* GZ_ANISO: >1 = mip chain + anisotropic filtering on linear textures */
#define GZ_SWAP_ALLOW_TEARING    2048u   /* DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING */
#define GZ_PRESENT_ALLOW_TEARING 0x200u  /* DXGI_PRESENT_ALLOW_TEARING */
static int s_tearing;
static int s_fullscreen;   /* GZ_FULLSCREEN=1: borderless window over the primary monitor */
/* Widescreen (Options > aspect, GZ_WIDE): the game renders its 640x480 frame
 * with a 16:9 camera (recomp_manual.c widens the projection), so the frame is
 * anamorphic. Here it gets a 4/3 wider target, is shown at 16:9, and 2D
 * vertices (w exactly 1: HUD, menus, text) are pulled toward the centre by
 * 3/4 so they keep their shape. s_draw_squeeze is that factor for the draw
 * being set up, 0 for none. */
int gz_opt_wide(void);
static int   s_wide;
static float s_draw_squeeze, s_draw_anchor, s_draw_yscale = 1.0f;
/* The fight HUD (top band, while a fight is on screen). With the fight
 * camera widened the game itself already draws the HUD 3/4 narrower about
 * the centre (positions and sizes), which keeps its shape at 16:9 - so the
 * timer gets no further squeeze. The players' pieces are spread back out by
 * 4/3 about the centre (their exact 4:3 place and share of the width) and
 * grown 4/3 in height from the top edge to keep their shape: corners, same
 * share of the screen as in 4:3. GZ_WIDE_HUD=1 leaves them at that smaller
 * size in the middle. */
int gz_wide_fight_active(void);
static void wide_hud_scale(float anchor, float w, float ymx, float h)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("GZ_WIDE_HUD"); mode = (e && *e) ? atoi(e) : 2; }
    if (mode < 2 || ymx > h * 0.30f || !gz_wide_fight_active()) return;
    if (anchor == w * 0.5f) { s_draw_squeeze = 1.0f; return; }          /* timer: as the game drew it */
    s_draw_squeeze = 4.0f / 3.0f; s_draw_anchor = w * 0.5f; s_draw_yscale = 4.0f / 3.0f;
}
/* Where a 2D draw is squeezed toward: the centre, or - for HUD pieces in
 * the top or bottom band that sit wholly in one half (health bars, player
 * names, the portraits) - that side's edge, so the HUD spreads out to the
 * corners of the wide picture. GZ_WIDE_HUD=0 keeps everything centred. */
int gz_wide_fight_active(void);   /* recomp_manual.c */
static float wide_anchor(float mn, float mx, float ymn, float ymx, float w, float h)
{
    static int hud = -1;
    if (hud < 0) hud = !(getenv("GZ_WIDE_HUD") && getenv("GZ_WIDE_HUD")[0] == '0');
    if (hud && ymx <= h * 0.30f && gz_wide_fight_active()) {   /* fight HUD (top band) only */
        /* By the piece's centre: every piece of a player's HUD (portrait,
         * ring, bar, name) moves with its side; the timer (digits centred
         * between about 298 and 342 of 640) stays in the middle. */
        float c = (mn + mx) * 0.5f;
        if (c < w * 0.465f) return 0.0f;
        if (c > w * 0.535f) return w;
    }
    return w * 0.5f;
}
#define WIDE_SURF(w, h) (s_wide && (w) == 640u && (h) == 480u)
#define PX_W(w, h) (WIDE_SURF(w, h) ? ((w) * (UINT)s_scale * 4u + 2u) / 3u : (w) * (UINT)s_scale)

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v = (e && *e) ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

/* ── Device, window ───────────────────────────────────────── */

static HWND                    s_hwnd;
static ID3D11Device           *s_dev;
static ID3D11DeviceContext    *s_ctx;
static IDXGISwapChain         *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static UINT                    s_win_w, s_win_h;
static int                     s_failed, s_ready;

static ID3D11VertexShader *s_vs, *s_vs_blit;
static ID3D11PixelShader  *s_ps, *s_ps_blit, *s_ps_one;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_vb, *s_cb;
/* GPU vertex programs (be_draw_program). */
static ID3D11VertexShader *s_vs_prog;
static ID3D11InputLayout  *s_layout_prog;
static ID3DBlob           *s_prog_blob;
static ID3D11Buffer       *s_cb_prog, *s_cb_const, *s_cb_draw, *s_vb2, *s_ib;
static UINT                s_vb2_size, s_vb2_pos, s_ib_size, s_ib_pos;
static uint32_t            s_prog_rev_up = 0xFFFFFFFFu, s_const_rev_up = 0xFFFFFFFFu;
static int                 s_prog_ok;
static uint32_t            s_st_prog_draws;
static UINT                s_vb_size, s_vb_pos;
static ID3D11RasterizerState *s_rs;
static ID3D11SamplerState *s_blit_sampler;
static ID3D11DepthStencilState *s_ds_off;
static ID3D11BlendState   *s_blend_off;

/* ── Caches ───────────────────────────────────────────────── */

typedef struct {
    uint32_t va, w, h;                  /* guest address, real pixels */
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t pitch, bpp;
    uint32_t last_frame;
} Rt;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
} Ds;

typedef struct {
    uint32_t offset, w, h, color, hash, frame, used, cube;
    uint32_t tick;                      /* GetTickCount() of the last content hash */
    unsigned gen;                       /* gz_opt_generation() it was decoded under */
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
} Tex;

#define MAX_RT  24
#define MAX_DS  12
#define MAX_TEX 1536
#define MAX_STATE 512

static Rt  s_rts[MAX_RT];
static Ds  s_dss[MAX_DS];
static Tex s_texs[MAX_TEX];

typedef struct { uint64_t key; void *obj; } StateEnt;
static StateEnt s_blends[MAX_STATE], s_depths[MAX_STATE], s_samplers[MAX_STATE];
static int s_nblends, s_ndepths, s_nsamplers;

static uint32_t s_frame = 1;
static Rt      *s_present_rt;           /* most recent full-size surface drawn */

/* Stats */
static uint32_t s_st_draws, s_st_tris, s_st_uploads, s_st_presents, s_st_culled;

/* GZ_GPU_TRACE_FILE=<path>: when that file appears, the next frame is traced
 * draw by draw (combiner words, stages, vertex colours) and the file deleted. */
static uint32_t s_trace_req_frame, s_trace_req_n;
static int s_nodot;      /* F10 toggles the dot-product texture modes off */
static DWORD    s_st_t0;

/* ── Shaders ──────────────────────────────────────────────── */

/* The pixel shader ports xboxrecomp's nv2a_combiner.c (itself following
 * xemu's psh.c): register file R[16] = zero, c0, c1, fog, v0, v1, -, -,
 * t0..t3, r0, r1, v1+r0 sum, E*F; up to 8 general stages from the ICW/OCW
 * words, then the final combiner. Stage textures are fetched per the shader
 * stage program (none / project 2D-3D / cube as 2D / pass-through / clip). */
static const char s_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  float2 inv_size; float2 tex0_inv;\n"
"  float zscale; float alpha_ref; float alpha_func; float zclip_on;\n"
"  float zmin; float zmax; float rc_on; float tex0_on;\n"
"  uint4 fin;      /* final0, final1, control, stage_program */\n"
"  uint4 misc;     /* clip_plane_mode */\n"
"  float4 fog_color; float4 final_c0; float4 final_c1;\n"
"  float4 tex_scale[4];  /* sx, sy, bound, normalized */\n"
"  uint4 stage[8];       /* color_icw, alpha_icw, color_ocw, alpha_ocw */\n"
"  float4 factor0[8]; float4 factor1[8];\n"
"  float4 wide;         /* 2D in widescreen: x squeeze (0 = off) about x = y; height scale z about y = w */\n"
"};\n"
"/* SPEC: a pixel shader compiled for one combiner setup. Everything that\n"
" * steers control flow is a constant, so the interpreter below folds into\n"
" * straight code (ps_variant in the C source). */\n"
"#ifdef SPEC\n"
"static const uint4 k_fin = uint4(SPEC_FIN);\n"
"static const uint4 k_misc = uint4(SPEC_MISC);\n"
"static const uint4 k_stage[8] = { SPEC_STAGE };\n"
"static const float k_bound[4] = { SPEC_BOUND };\n"
"#define fin k_fin\n"
"#define misc k_misc\n"
"#define stage k_stage\n"
"#define rc_on SPEC_RC\n"
"#define tex0_on SPEC_T0\n"
"#define alpha_func SPEC_AF\n"
"#define zclip_on SPEC_ZC\n"
"#define TS(n) float4(tex_scale[n].xy, k_bound[n], tex_scale[n].w)\n"
"#define RC_LOOP [unroll(8)]\n"
"#else\n"
"#define TS(n) tex_scale[n]\n"
"#define RC_LOOP [loop]\n"
"#endif\n"
"struct VI { float3 p : POSITION; float rhw : TEXCOORD1; float4 c : COLOR0; float2 uv : TEXCOORD0;\n"
"            float4 spec : COLOR1; float fog : TEXCOORD2;\n"
"            float4 tc0 : TEXCOORD3; float4 tc1 : TEXCOORD4; float4 tc2 : TEXCOORD5; float4 tc3 : TEXCOORD6; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; float2 zr : TEXCOORD1;\n"
"            float4 spec : COLOR1; float fog : TEXCOORD2;\n"
"            float4 tc0 : TEXCOORD3; float4 tc1 : TEXCOORD4; float4 tc2 : TEXCOORD5; float4 tc3 : TEXCOORD6; };\n"
"VO vs_main(VI i) {\n"
"  VO o; float w = (i.rhw > 1e-8) ? 1.0 / i.rhw : 1.0;\n"
"  float px = i.p.x;\n"
"  if (wide.x > 0.0 && abs(w - 1.0) < 1e-3) px = wide.y + (px - wide.y) * wide.x;\n"
"  float x = px * inv_size.x * 2.0 - 1.0;\n"
"  float py = i.p.y;\n"
"  if (wide.x > 0.0 && abs(w - 1.0) < 1e-3) py = wide.w + (py - wide.w) * wide.z;\n"
"  float y = 1.0 - py * inv_size.y * 2.0;\n"
"  float z = saturate(i.p.z * zscale);\n"
"  o.p = float4(x * w, y * w, z * w, w); o.c = i.c; o.uv = i.uv * tex0_inv; o.zr = float2(i.p.z * w, w);\n"
"  o.spec = i.spec; o.fog = i.fog; o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.tc3 = i.tc3;\n"
"  return o;\n"
"}\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
"Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"TextureCube cx0 : register(t4); TextureCube cx1 : register(t5);\n"
"TextureCube cx2 : register(t6); TextureCube cx3 : register(t7);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1);\n"
"SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"static float4 R[16];\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1.0 - saturate(x);\n"
"  if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return -2.0 * p + 1.0;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float map_out(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;\n"
"  if (m == 0x10) return x * 2.0;\n"
"  if (m == 0x18) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20) return x * 4.0;\n"
"  if (m == 0x30) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 map_out3(float3 v, uint m) { return float3(map_out(v.x, m), map_out(v.y, m), map_out(v.z, m)); }\n"
"float3 rd_rgb(uint b) {\n"
"  float4 r = R[b & 15]; float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float rd_a(uint b) { float4 r = R[b & 15]; return map_in((b & 0x10) ? r.a : r.b, b); }\n"
"float4 fetch(uint mode, float4 tc, float4 sc, Texture2D t, TextureCube tq, SamplerState s, uint st) {\n"
"  if (mode == 3 && sc.z > 1.5) return tq.Sample(s, tc.xyz);\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2 || mode == 3) {\n"
"    float q = (mode != 3 && tc.w != 0.0) ? tc.w : 1.0;\n"
"    if (sc.z < 0.5) return float4(1, 1, 1, 1);\n"
"    return t.Sample(s, tc.xy / q * sc.xy);\n"
"  }\n"
"  if (mode == 4) return saturate(tc);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      uint ge = (misc.x >> (st * 4 + j)) & 1;\n"
"      if (ge ? tc[j] >= 0.0 : tc[j] < 0.0) discard;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"float4 combine(float4 v0, float4 v1, float fogf, float4 t0, float4 t1, float4 t2, float4 t3) {\n"
"  uint n = min(fin.z & 255, 8), flags = fin.z >> 8, s, d;\n"
"  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fog_color.rgb, fogf); R[4] = v0; R[5] = v1;\n"
"  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"  R[12].a = (fin.w & 31) ? t0.a : 1.0;\n"
"  RC_LOOP for (s = 0; s < n; s++) {\n"
"    uint4 sw = stage[s]; uint icw = sw.x, aicw = sw.y, ocw = sw.z, aocw = sw.w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12, om = fl & 0x38, aom = afl & 0x38;\n"
"    R[1] = factor0[(flags & 0x10) ? s : 0]; R[2] = factor1[(flags & 0x100) ? s : 0];\n"
"    bool mux = (flags & 1) ? (R[12].a >= 0.5) : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"    float3 A = rd_rgb(icw >> 24), B = rd_rgb(icw >> 16), C = rd_rgb(icw >> 8), D = rd_rgb(icw);\n"
"    float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16), aC = rd_a(aicw >> 8), aD = rd_a(aicw);\n"
"    float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"    float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = clamp(map_out3(pab, om), -1.0, 1.0);\n"
"    float3 cd = clamp(map_out3(pcd, om), -1.0, 1.0);\n"
"    float3 ms = clamp(map_out3(sum, om), -1.0, 1.0);\n"
"    float apab = aA * aB, apcd = aC * aD;\n"
"    float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(map_out(apab, aom), -1.0, 1.0);\n"
"    float acd = clamp(map_out(apcd, aom), -1.0, 1.0);\n"
"    float ams = clamp(map_out(asum, aom), -1.0, 1.0);\n"
"    d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"    d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"    d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"    d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"    d = aocw & 15;        if (d) R[d].a = acd;\n"
"    d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 o;\n"
"  if (fin.x != 0 || fin.y != 0) {\n"
"    uint f0 = fin.x, f1 = fin.y, ff = f1 & 255;\n"
"    R[1] = final_c0; R[2] = final_c1;\n"
"    float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"    float3 E = rd_rgb(f1 >> 24), F = rd_rgb(f1 >> 16);\n"
"    R[15] = float4(E * F, 0.0);\n"
"    float3 A = rd_rgb(f0 >> 24), B = rd_rgb(f0 >> 16), C = rd_rgb(f0 >> 8), D = rd_rgb(f0);\n"
"    o.rgb = D + C * (1.0 - A) + B * A;\n"
"    o.a = rd_a(f1 >> 8);\n"
"  } else {\n"
"    o = R[12];\n"
"  }\n"
"  return saturate(o);\n"
"}\n"
"float3 dotmap(float3 c, uint m) {\n"
"  if (m == 1) return (c * 255.0 - 128.0) / 127.0;   /* MINUS1_TO_1_D3D */\n"
"  if (m == 2 || m == 3) return c * 2.0 - 1.0;       /* MINUS1_TO_1 (GL) */\n"
"  return c;                                         /* ZERO_TO_ONE; HILO: not modelled */\n"
"}\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  float4 c;\n"
"  float zr = i.zr.x / i.zr.y;   /* (z*w, w) are linear in clip space; z itself is not */\n"
"  if (zclip_on > 0.5 && (zr < zmin || zr > zmax)) discard;\n"
"  if (rc_on > 0.5) {\n"
"    uint sp = fin.w;\n"
"    float4 t0 = fetch(sp & 31, i.tc0, TS(0), tx0, cx0, sm0, 0);\n"
"    /* Dot-product stage modes (xemu psh.c): 17 DOT_PRODUCT feeds a later\n"
"     * stage; 9 DOT_ST (2D lookup), 11 DOT_REFLECT_DIFFUSE, 12\n"
"     * DOT_REFLECT_SPECULAR and 14 DOT_STR_CUBE look the last stage's\n"
"     * texture up with the three dots. Godzilla's water is 2D normal map,\n"
"     * DOT, DOT, DOT_REFLECT_SPECULAR into a cube map. */\n"
"    uint m1 = (sp >> 5) & 31, m2 = (sp >> 10) & 31, m3 = (sp >> 15) & 31;\n"
"    if (misc.w >> 31) {   /* F10: dot-product modes off (old behaviour) */\n"
"      if (m1 == 17) m1 = 31; if (m2 == 17 || m2 == 9) m2 = 31;\n"
"      if (m3 == 11 || m3 == 12 || m3 == 14) m3 = 31;\n"
"    }\n"
"    float4 t1, t2, t3;\n"
"    float d1 = 0.0, d2 = 0.0, d3 = 0.0;\n"
"    if (m1 == 17) { d1 = dot(i.tc1.xyz, dotmap(t0.rgb, misc.z & 15)); t1 = float4(0, 0, 0, 0); }\n"
"    else t1 = fetch(m1, i.tc1, TS(1), tx1, cx1, sm1, 1);\n"
"    float4 in2 = (((misc.w >> 16) & 15) == 1) ? t1 : t0;\n"
"    if (m2 == 17) { d2 = dot(i.tc2.xyz, dotmap(in2.rgb, (misc.z >> 4) & 15)); t2 = float4(0, 0, 0, 0); }\n"
"    else if (m2 == 9) {\n"
"      d2 = dot(i.tc2.xyz, dotmap(in2.rgb, (misc.z >> 4) & 15));\n"
"      t2 = TS(2).z > 0.5 ? tx2.Sample(sm2, float2(d1, d2)) : float4(1, 1, 1, 1);\n"
"    }\n"
"    else t2 = fetch(m2, i.tc2, TS(2), tx2, cx2, sm2, 2);\n"
"    uint o3 = (misc.w >> 20) & 15;\n"
"    float4 in3 = (o3 == 2) ? t2 : ((o3 == 1) ? t1 : t0);\n"
"    if (m3 == 11 || m3 == 12 || m3 == 14) {\n"
"      d3 = dot(i.tc3.xyz, dotmap(in3.rgb, (misc.z >> 8) & 15));\n"
"      float3 nrm = float3(d1, d2, d3), dir = nrm;\n"
"      if (m3 == 12) {\n"
"        float3 eye = float3(i.tc1.w, i.tc2.w, i.tc3.w);\n"
"        dir = 2.0 * dot(nrm, eye) / max(dot(nrm, nrm), 1e-12) * nrm - eye;\n"
"      }\n"
"      if (TS(3).z > 1.5) t3 = cx3.Sample(sm3, dir);\n"
"      else t3 = float4(1, 1, 1, 1);\n"
"    }\n"
"    else t3 = fetch(m3, i.tc3, TS(3), tx3, cx3, sm3, 3);\n"
"    c = combine(i.c, i.spec, saturate(i.fog), t0, t1, t2, t3);\n"
"    if (misc.y == 1) c = t0; else if (misc.y == 2) c = i.c; else if (misc.y == 3) c = float4(i.fog.xxx, 1);\n"
"    else if (misc.y == 4) c = float4(R[12].rgb, 1);\n"
"  } else {\n"
"    c = i.c;\n"
"    if (tex0_on > 0.5) c *= tx0.Sample(sm0, i.uv);\n"
"  }\n"
"  int f = (int)alpha_func; float a = c.a, r = alpha_ref; bool ok = true;\n"
"  if (f == 0) ok = false;\n"
"  else if (f == 1) ok = a < r;\n"
"  else if (f == 2) ok = abs(a - r) < 0.5 / 255.0;\n"
"  else if (f == 3) ok = a <= r;\n"
"  else if (f == 4) ok = a > r;\n"
"  else if (f == 5) ok = abs(a - r) >= 0.5 / 255.0;\n"
"  else if (f == 6) ok = a >= r;\n"
"  if (!ok) discard;\n"
"  return c;\n"
"}\n"
"struct BO { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"BO vs_blit(uint id : SV_VertexID) {\n"
"  BO o; float2 t = float2((id << 1) & 2, id & 2);\n"
"  o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;\n"
"}\n"
"float4 ps_blit(BO i) : SV_Target { float4 c = tx0.Sample(sm0, i.uv); return float4(c.rgb, 1); }\n"
"float4 ps_one(BO i) : SV_Target { return float4(1, 1, 1, 1); }\n"
"/* ---- vertex programs on the GPU (nv2a_backend.h Nv2aProgramDraw) ---- */\n"
"cbuffer VP : register(b1) { uint4 prog[272]; };\n"
"cbuffer VC : register(b2) { float4 cst[192]; };\n"
"cbuffer VD : register(b3) { uint4 vinfo; float4 vparam; float4 vfog; };\n"
"struct VPI {\n"
"  float4 a0 : ATTR0;  float4 a1 : ATTR1;  float4 a2 : ATTR2;  float4 a3 : ATTR3;\n"
"  float4 a4 : ATTR4;  float4 a5 : ATTR5;  float4 a6 : ATTR6;  float4 a7 : ATTR7;\n"
"  float4 a8 : ATTR8;  float4 a9 : ATTR9;  float4 a10 : ATTR10; float4 a11 : ATTR11;\n"
"  float4 a12 : ATTR12; float4 a13 : ATTR13; float4 a14 : ATTR14; float4 a15 : ATTR15;\n"
"};\n"
"struct VPO { float4 p : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; float2 zr : TEXCOORD1;\n"
"             float4 spec : COLOR1; float fog : TEXCOORD2;\n"
"             float4 tc0 : TEXCOORD3; float4 tc1 : TEXCOORD4; float4 tc2 : TEXCOORD5; float4 tc3 : TEXCOORD6;\n"
"             float2 cd : SV_ClipDistance0; };\n"
"static float4 VIN[16];\n"
"static float4 VR[13];\n"
"static float4 VOR[13];\n"
"static int VA0;\n"
"float4 vsrc(uint s) {\n"
"  uint mux = s & 3, idx = (s >> 2) & 15;\n"
"  float4 v;\n"
"  if (mux == 1) v = VR[min(idx, 12)];\n"
"  else if (mux == 2) v = VIN[idx];\n"
"  else {\n"
"    int ci = (int)((s >> 16) & 255);\n"
"    if ((s >> 7) & 1) ci += VA0;\n"
"    if (ci < 0 || ci >= 192) ci = 0;\n"
"    v = cst[ci];\n"
"  }\n"
"  uint sw = (s >> 8) & 255;\n"
"  float4 r = float4(v[(sw >> 6) & 3], v[(sw >> 4) & 3], v[(sw >> 2) & 3], v[sw & 3]);\n"
"  return ((s >> 6) & 1) ? -r : r;\n"
"}\n"
"float4 vwm(float4 d, float4 v, uint m) {\n"
"  return float4((m & 8) ? v.x : d.x, (m & 4) ? v.y : d.y, (m & 2) ? v.z : d.z, (m & 1) ? v.w : d.w);\n"
"}\n"
"float4 vmac(uint op, float4 a, float4 b, float4 c) {\n"
"  if (op == 2) return a * b;\n"
"  if (op == 3) return a + c;\n"
"  if (op == 4) return a * b + c;\n"
"  if (op == 5) return dot(a.xyz, b.xyz).xxxx;\n"
"  if (op == 6) return (dot(a.xyz, b.xyz) + b.w).xxxx;\n"
"  if (op == 7) return dot(a, b).xxxx;\n"
"  if (op == 8) return float4(1.0, a.y * b.y, a.z, b.w);\n"
"  if (op == 9) return (a < b) ? a : b;\n"
"  if (op == 10) return (a >= b) ? a : b;\n"
"  if (op == 11) return (a < b) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);\n"
"  if (op == 12) return (a >= b) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);\n"
"  if (op == 13) VA0 = (int)floor(a.x + 0.001);\n"
"  return a;\n"
"}\n"
"float4 vilu(uint op, float4 c) {\n"
"  float x = c.x;\n"
"  if (op == 2) return (x == 0.0 ? asfloat(0x7F800000) : 1.0 / x).xxxx;\n"
"  if (op == 3) {\n"
"    float f = 1.0 / x, af = clamp(abs(f), 5.42101e-20, 1.884467e19);\n"
"    return (f < 0.0 ? -af : af).xxxx;\n"
"  }\n"
"  if (op == 4) return rsqrt(abs(x)).xxxx;\n"
"  if (op == 5) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"  if (op == 6) {\n"
"    float ax = abs(x);\n"
"    if (ax == 0.0) return float4(asfloat(0xFF800000), 1.0, asfloat(0xFF800000), 1.0);\n"
"    float e = floor(log2(ax));\n"
"    return float4(e, ax / exp2(e), log2(ax), 1.0);\n"
"  }\n"
"  if (op == 7) {\n"
"    float nl = max(c.x, 0.0), nh = max(c.y, 0.0), pw = clamp(c.w, -127.9961, 127.9961);\n"
"    return float4(1.0, nl, c.x > 0.0 ? pow(nh, pw) : 0.0, 1.0);\n"
"  }\n"
"  return c;\n"
"}\n"
"VPO vs_prog(VPI i) {\n"
"  VPO o;\n"
"  uint n, k;\n"
"  VIN[0] = i.a0; VIN[1] = i.a1; VIN[2] = i.a2; VIN[3] = i.a3;\n"
"  VIN[4] = i.a4; VIN[5] = i.a5; VIN[6] = i.a6; VIN[7] = i.a7;\n"
"  VIN[8] = i.a8; VIN[9] = i.a9; VIN[10] = i.a10; VIN[11] = i.a11;\n"
"  VIN[12] = i.a12; VIN[13] = i.a13; VIN[14] = i.a14; VIN[15] = i.a15;\n"
"  [unroll] for (k = 0; k < 13; k++) { VR[k] = float4(0, 0, 0, 0); VOR[k] = float4(0, 0, 0, 0); }\n"
"  VOR[3].w = 1.0; VOR[4].w = 1.0;\n"
"  VOR[9].w = 1.0; VOR[10].w = 1.0; VOR[11].w = 1.0; VOR[12].w = 1.0;\n"
"  VA0 = 0;\n"
"  [loop] for (n = 0; n < vinfo.x; n++) {\n"
"    uint4 w = prog[n * 2];\n"
"    uint w4 = prog[n * 2 + 1].x;\n"
"    uint mop = w.x & 15, iop = (w.x >> 4) & 7, mm = (w.x >> 8) & 15, im = (w.x >> 12) & 15;\n"
"    uint md = (w.x >> 16) & 255, idd = (w.x >> 24) & 255;\n"
"    uint om = w.y & 15, oa = (w.y >> 4) & 255;\n"
"    float4 a = vsrc(w.z), b = vsrc(w.w), c = vsrc(w4);\n"
"    float4 mr = float4(0, 0, 0, 0), ir = float4(0, 0, 0, 0);\n"
"    if (mop) mr = vmac(mop, a, b, c);\n"
"    if (iop) ir = vilu(iop, c);\n"
"    if (mm && md < 13) VR[md] = vwm(VR[md], mr, mm);\n"
"    if (im && idd < 13) VR[idd] = vwm(VR[idd], ir, im);\n"
"    if (om && ((w.y >> 12) & 1)) {\n"
"      float4 sv = ((w.y >> 13) & 1) ? ir : mr;\n"
"      if (oa == 0) VR[12] = vwm(VR[12], sv, om);\n"
"      else if (oa < 13) VOR[oa] = vwm(VOR[oa], sv, om);\n"
"    }\n"
"    if ((w.y >> 14) & 1) break;\n"
"  }\n"
"  float4 pos = VR[12];\n"
"  if (pos.w == 0.0 && !(asuint(pos.w) & 0x80000000)) pos.w = 1.0;\n"
"  pos.xy *= vparam.xy;\n"
"  float pw = pos.w;\n"
"  if (wide.x > 0.0 && abs(pw - 1.0) < 1e-3) pos.x = wide.y + (pos.x - wide.y) * wide.x;\n"
"  float x = pos.x * inv_size.x * 2.0 - 1.0;\n"
"  if (wide.x > 0.0 && abs(pw - 1.0) < 1e-3) pos.y = wide.w + (pos.y - wide.w) * wide.z;\n"
"  float y = 1.0 - pos.y * inv_size.y * 2.0;\n"
"  /* Depth is not clamped here: clamping a vertex's z before the GPU\n"
"   * interpolates it bends the depth across the whole triangle (a water\n"
"   * vertex just in front of the camera made the near water fail the depth\n"
"   * test and the riverbed show through, as a green flash). With depth clip\n"
"   * off, the rasterizer clamps each pixel's depth to [0, 1] instead. */\n"
"  o.p = float4(x * pw, y * pw, pos.z * zscale * pw, pw);\n"
"  o.zr = float2(pos.z * pw, pw);\n"
"  o.cd = float2(pw - vparam.z, vinfo.y ? (pos.z - cst[59].z) * pw : 1.0);\n"
"  /* The CPU path drops a triangle with any non-finite vertex; a huge\n"
"   * negative clip distance on that vertex clips its triangles away. */\n"
"  float3 cpos = pos.xyz * pw;\n"
"  if (any(isnan(pos)) || any(isinf(pos)) || any(isnan(cpos)) || any(isinf(cpos))) {\n"
"    o.p = float4(0, 0, 0, 1); o.cd = float2(-1e30, -1e30); o.zr = float2(0.0, 1.0);\n"
"  }\n"
"  o.c = saturate(VOR[3]);\n"
"  o.spec = saturate(VOR[4]);\n"
"  float fd = VOR[5].x, f = 1.0;\n"
"  if (vinfo.w) {\n"
"    uint m = vinfo.z;\n"
"    float px = vfog.x, py = vfog.y;\n"
"    if (m == 0x800 || m == 0x802) f = px + exp2(fd * py * 16.0) - 1.5;\n"
"    else if (m == 0x801 || m == 0x803) f = px + exp2(-fd * fd * py * py * 32.0) - 1.5;\n"
"    else f = px + fd * py - 1.0;\n"
"    if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
"    if (isnan(f)) f = 1.0;\n"
"    f = clamp(f, -1e4, 1e4);   /* saturated per pixel in ps_main */\n"
"  }\n"
"  o.fog = f;\n"
"  o.tc0 = VOR[9]; o.tc1 = VOR[10]; o.tc2 = VOR[11]; o.tc3 = VOR[12];\n"
"  o.uv = vparam.w > 0.5 ? VOR[9].xy : VOR[9].xy * tex0_inv;\n"
"  return o;\n"
"}\n";

/* Matches cbuffer CB in s_hlsl, row for row (16-byte registers). */
typedef struct {
    float    inv_size[2], tex0_inv[2];
    float    zscale, alpha_ref, alpha_func, zclip_on;
    float    zmin, zmax, rc_on, tex0_on;
    uint32_t fin[4];            /* final0, final1, control, stage_program */
    uint32_t misc[4];           /* clip_plane_mode */
    float    fog_color[4], final_c0[4], final_c1[4];
    float    tex_scale[4][4];   /* sx, sy, bound, normalized */
    uint32_t stage[8][4];       /* color_icw, alpha_icw, color_ocw, alpha_ocw */
    float    factor0[8][4], factor1[8][4];
    float    wide[4];           /* 2D in widescreen: x squeeze (0 = off) about x = [1]; height x [2] about y = [3] */
} Consts;

/* ---- Pixel shader variants ------------------------------------------------
 * ps_main interprets the combiner per pixel (register file, up to 8 stages,
 * four texture modes): fine at 60 fps, the limit at 240 fps and high internal
 * resolutions. Each distinct setup (PsKey) is also compiled as its own
 * shader with that setup baked in; a worker thread compiles (or loads from
 * shader_cache/) while the generic shader draws, so nothing waits for it.
 * GZ_PS_SPEC=0: generic shader only. */
typedef struct {
    uint32_t rc_on, tex0_on, alpha_func, zclip_on;
    uint32_t fin[4], misc[4], stage[8][4], bound[4];
} PsKey;
typedef struct {
    PsKey key;
    uint64_t hash;
    volatile LONG state;                /* 0 empty, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *ps;
} PsVar;
#define PS_VARS 1024
static PsVar  s_psv[PS_VARS];
static PsKey  s_ps_key;                 /* of the draw being set up (set_consts) */
static int    s_ps_spec = -1;
static HANDLE s_ps_event;
static volatile LONG s_ps_queue[PS_VARS], s_ps_qhead, s_ps_qtail;
static uint64_t s_ps_src_hash;

static uint64_t fnv64(const void *d, size_t n, uint64_t h)
{
    const uint8_t *p = (const uint8_t *)d;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}

static void ps_compile(PsVar *v)
{
    char path[MAX_PATH], fin[96], misc[96], stage[8 * 64], bound[64], rc[8], t0[8], af[16], zc[8];
    D3D_SHADER_MACRO mac[10];
    ID3DBlob *blob = NULL, *err = NULL;
    const PsKey *k = &v->key;
    void *code = NULL;
    size_t size = 0;
    FILE *f;
    int i, n = 0;

    snprintf(path, sizeof path, "shader_cache/%016llX.cso", (unsigned long long)v->hash);
    if ((f = fopen(path, "rb")) != NULL) {
        fseek(f, 0, SEEK_END); size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (1u << 20) && (code = malloc(size)) != NULL && fread(code, 1, size, f) != size) {
            free(code); code = NULL;
        }
        fclose(f);
    }
    if (!code) {
        snprintf(fin, sizeof fin, "%uu,%uu,%uu,%uu", k->fin[0], k->fin[1], k->fin[2], k->fin[3]);
        snprintf(misc, sizeof misc, "%uu,%uu,%uu,%uu", k->misc[0], k->misc[1], k->misc[2], k->misc[3]);
        for (i = 0; i < 8; i++)
            n += snprintf(stage + n, sizeof stage - (size_t)n, "%suint4(%uu,%uu,%uu,%uu)", i ? "," : "",
                          k->stage[i][0], k->stage[i][1], k->stage[i][2], k->stage[i][3]);
        snprintf(bound, sizeof bound, "%u.0,%u.0,%u.0,%u.0", k->bound[0], k->bound[1], k->bound[2], k->bound[3]);
        snprintf(rc, sizeof rc, "%u.0", k->rc_on); snprintf(t0, sizeof t0, "%u.0", k->tex0_on);
        snprintf(af, sizeof af, "%u.0", k->alpha_func); snprintf(zc, sizeof zc, "%u.0", k->zclip_on);
        mac[0].Name = "SPEC"; mac[0].Definition = "1";
        mac[1].Name = "SPEC_FIN"; mac[1].Definition = fin;
        mac[2].Name = "SPEC_MISC"; mac[2].Definition = misc;
        mac[3].Name = "SPEC_STAGE"; mac[3].Definition = stage;
        mac[4].Name = "SPEC_BOUND"; mac[4].Definition = bound;
        mac[5].Name = "SPEC_RC"; mac[5].Definition = rc;
        mac[6].Name = "SPEC_T0"; mac[6].Definition = t0;
        mac[7].Name = "SPEC_AF"; mac[7].Definition = af;
        mac[8].Name = "SPEC_ZC"; mac[8].Definition = zc;
        mac[9].Name = NULL; mac[9].Definition = NULL;
        if (FAILED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gz_gpu_spec", mac, NULL, "ps_main", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
            static LONG shown;
            if (InterlockedIncrement(&shown) <= 4)
                fprintf(stderr, "[GPU-D3D11] shader variant %016llX failed: %s\n", (unsigned long long)v->hash,
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            if (err) ID3D10Blob_Release(err);
            InterlockedExchange(&v->state, 3);
            return;
        }
        if (err) ID3D10Blob_Release(err);
        CreateDirectoryA("shader_cache", NULL);
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(ID3D10Blob_GetBufferPointer(blob), 1, ID3D10Blob_GetBufferSize(blob), f);
            fclose(f);
        }
    }
    if (SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, code ? code : ID3D10Blob_GetBufferPointer(blob),
                                                 code ? size : ID3D10Blob_GetBufferSize(blob), NULL, &v->ps)))
        InterlockedExchange(&v->state, 2);
    else
        InterlockedExchange(&v->state, 3);
    if (blob) ID3D10Blob_Release(blob);
    free(code);
}

static DWORD WINAPI ps_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(s_ps_event, INFINITE);
        while (s_ps_qtail != s_ps_qhead) {
            LONG slot = s_ps_queue[s_ps_qtail % PS_VARS];
            ps_compile(&s_psv[slot]);
            InterlockedIncrement(&s_ps_qtail);
        }
    }
}

/* The shader for the draw whose constants were just set: its variant once
 * compiled, the generic interpreter until then. */
static ID3D11PixelShader *ps_variant(void)
{
    uint64_t h;
    uint32_t i, at;
    if (s_ps_spec < 0) {
        s_ps_spec = env_int("GZ_PS_SPEC", 1, 0, 1);
        if (s_ps_spec) {
            s_ps_src_hash = fnv64(s_hlsl, sizeof s_hlsl - 1, 14695981039346656037ull);
            s_ps_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            if (!s_ps_event || !CreateThread(NULL, 0, ps_worker, NULL, 0, NULL)) s_ps_spec = 0;
        }
    }
    if (!s_ps_spec) return s_ps;
    h = fnv64(&s_ps_key, sizeof s_ps_key, s_ps_src_hash);
    at = (uint32_t)(h ^ (h >> 32)) % PS_VARS;
    for (i = 0; i < 16; i++, at = (at + 1) % PS_VARS) {
        PsVar *v = &s_psv[at];
        if (v->state == 0) {
            if (s_ps_qhead - s_ps_qtail >= PS_VARS - 1) return s_ps;      /* queue full: later */
            v->key = s_ps_key; v->hash = h;
            InterlockedExchange(&v->state, 1);
            s_ps_queue[s_ps_qhead % PS_VARS] = (LONG)at;
            InterlockedIncrement(&s_ps_qhead);
            SetEvent(s_ps_event);
            return s_ps;
        }
        if (v->hash == h && !memcmp(&v->key, &s_ps_key, sizeof s_ps_key))
            return v->state == 2 ? v->ps : s_ps;
    }
    return s_ps;
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static int compile(const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gz_gpu", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

/* ── Window ───────────────────────────────────────────────── */

static volatile LONG s_quit;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:   InterlockedExchange(&s_quit, 1); return 0;
    case WM_DESTROY: InterlockedExchange(&s_quit, 1); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void create_back_rtv(void)
{
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
        ID3D11Texture2D_Release(bb);
    }
}

static int init_device(void)
{
    WNDCLASSW wc;
    RECT r;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    ID3DBlob *vsb = NULL, *psb = NULL, *vbb = NULL, *pbb = NULL;
    HRESULT hr;

    /* GZ_RENDER_SCALE: internal resolution, multiples of 640x480 (1..6).
     * 0 = auto: enough to cover the window's height. */
    s_scale     = env_int("GZ_RENDER_SCALE", 0, 0, 6);
    s_win_scale = env_int("GZ_WINDOW_SCALE", 2, 1, 4);
    s_fullscreen = env_int("GZ_FULLSCREEN", 0, 0, 1);
    s_aniso     = env_int("GZ_ANISO", 16, 1, 16);
    s_vsync     = env_int("GZ_VSYNC", 0, 0, 1);
    s_cull      = env_int("GZ_GPU_CULL", 0, 0, 1);
    s_trace     = env_int("GZ_GPU_TRACE", 0, 0, 1);
    s_rcdbg     = env_int("GZ_GPU_RCDBG", 0, 0, 4);
    s_readback  = getenv("RECOMP_FB_DUMP") != NULL;

    SetProcessDPIAware();        /* real pixels: fullscreen covers the monitor at its native size */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"GodzillaSTERecomp";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    s_wide = gz_opt_wide();
    s_win_h = 480u * (UINT)s_win_scale;
    s_win_w = s_wide ? (s_win_h * 16u + 8u) / 9u : 640u * (UINT)s_win_scale;
    if (s_fullscreen) {
        /* Borderless over the primary monitor; the 4:3 frame is letterboxed. */
        s_win_w = (UINT)GetSystemMetrics(SM_CXSCREEN);
        s_win_h = (UINT)GetSystemMetrics(SM_CYSCREEN);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Godzilla: Save the Earth (recomp, D3D11)",
                                 WS_POPUP | WS_VISIBLE, 0, 0, (int)s_win_w, (int)s_win_h,
                                 NULL, NULL, wc.hInstance, NULL);
    } else {
        r.left = 0; r.top = 0; r.right = (LONG)s_win_w; r.bottom = (LONG)s_win_h;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Godzilla: Save the Earth (recomp, D3D11)",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    }
    if (s_scale == 0) {
        s_scale = (int)((s_win_h + 479u) / 480u);
        if (s_readback) s_scale = 1;     /* frame dumps read the target back at guest size */
        if (s_scale < 1) s_scale = 1;
        if (s_scale > 6) s_scale = 6;
    }
    if (!s_hwnd) {
        fprintf(stderr, "[GPU-D3D11] CreateWindow failed (%lu)\n", GetLastError());
        return 0;
    }

    memset(&sd, 0, sizeof sd);
    sd.BufferCount       = 3;
    sd.BufferDesc.Width  = s_win_w;
    sd.BufferDesc.Height = s_win_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = s_hwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    /* Tearing-capable swap chain: without it a flip-model present with
     * vsync off still waits for the compositor once it has a frame queued,
     * which stalled presents for 6-28 ms several times a second above the
     * display's refresh rate. GZ_TEARING=0 creates it the old way. */
    s_tearing = env_int("GZ_TEARING", 1, 0, 1);
    if (s_tearing) sd.Flags = GZ_SWAP_ALLOW_TEARING;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                       D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, &got, &s_ctx);
    if (FAILED(hr) && s_tearing) {
        s_tearing = 0; sd.Flags = 0;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, &got, &s_ctx);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] D3D11CreateDeviceAndSwapChain failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    create_back_rtv();

    if (!compile("vs_main", "vs_5_0", &vsb) || !compile("ps_main", "ps_5_0", &psb)
        || !compile("vs_blit", "vs_5_0", &vbb) || !compile("ps_blit", "ps_5_0", &pbb))
        return 0;
    {
        ID3DBlob *ob = NULL;
        if (compile("ps_one", "ps_5_0", &ob)) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ob), ID3D10Blob_GetBufferSize(ob), NULL, &s_ps_one);
            ID3D10Blob_Release(ob);
        }
    }
    {
        /* GPU vertex programs: vs_prog, 16 float4 inputs at a 256-byte
         * stride, and constant buffers for the program, c[] and per draw. */
        ID3DBlob *pb = NULL;
        if (compile("vs_prog", "vs_5_0", &pb)) {
            D3D11_INPUT_ELEMENT_DESC el[16];
            D3D11_BUFFER_DESC bd;
            int k, ok = 1;
            for (k = 0; k < 16; k++) {
                el[k].SemanticName = "ATTR"; el[k].SemanticIndex = (UINT)k;
                el[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[k].InputSlot = 0;
                el[k].AlignedByteOffset = (UINT)(16 * k);
                el[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA; el[k].InstanceDataStepRate = 0;
            }
            ok &= SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(pb),
                                                            ID3D10Blob_GetBufferSize(pb), NULL, &s_vs_prog));
            ok &= SUCCEEDED(ID3D11Device_CreateInputLayout(s_dev, el, 16, ID3D10Blob_GetBufferPointer(pb),
                                                           ID3D10Blob_GetBufferSize(pb), &s_layout_prog));
            s_prog_blob = pb;               /* kept: layouts per packed mask are made on demand */
            memset(&bd, 0, sizeof bd);
            bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.ByteWidth = 272 * 16;
            ok &= SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_prog));
            bd.ByteWidth = 192 * 16;
            ok &= SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_const));
            bd.ByteWidth = 3 * 16;
            ok &= SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_draw));
            s_prog_ok = ok && env_int("GZ_GPU_VSH", 1, 0, 1);
        }
        fprintf(stderr, "[GPU-D3D11] vertex programs on the GPU: %s\n", s_prog_ok ? "on" : "off");
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vbb), ID3D10Blob_GetBufferSize(vbb), NULL, &s_vs_blit);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pbb), ID3D10Blob_GetBufferSize(pbb), NULL, &s_ps_blit);
    {
        /* Nv2aVertex: x y z rhw diffuse u v */
        D3D11_INPUT_ELEMENT_DESC el[10] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(Nv2aVertex, x),        D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,          0, offsetof(Nv2aVertex, rhw),      D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, offsetof(Nv2aVertex, diffuse),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Nv2aVertex, u),        D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_B8G8R8A8_UNORM,     0, offsetof(Nv2aVertex, specular), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, offsetof(Nv2aVertex, fog),      D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[0]),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[1]),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[2]),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[3]),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = ID3D11Device_CreateInputLayout(s_dev, el, 10, ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &s_layout);
        if (FAILED(hr)) { fprintf(stderr, "[GPU-D3D11] input layout failed\n"); return 0; }
    }
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); ID3D10Blob_Release(vbb); ID3D10Blob_Release(pbb);

    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(Consts); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;          /* culling decided per triangle, see draw() */
        rd.DepthClipEnable = FALSE;             /* window z is compared as given */
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    }
    {
        D3D11_SAMPLER_DESC sm;
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sm, &s_blit_sampler);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        D3D11_BLEND_DESC bd;
        memset(&dd, 0, sizeof dd);
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds_off);
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_blend_off);
    }

    fprintf(stderr, "[GPU-D3D11] device up: window %ux%u, render scale %d, vsync %d, cull %d, readback %d\n",
            s_win_w, s_win_h, s_scale, s_vsync, s_cull, s_readback);
    s_st_t0 = GetTickCount();
    return 1;
}

static int ready(void)
{
    if (s_ready) return 1;
    if (s_failed) return 0;
    if (!init_device()) {
        s_failed = 1;
        fprintf(stderr, "[GPU-D3D11] init failed; nothing will be drawn (run with GZ_GPU=0 for the CPU rasteriser)\n");
        return 0;
    }
    s_ready = 1;
    return 1;
}

/* ── Render targets and depth buffers ─────────────────────── */

static Rt *s_bound_rt;
static Ds *s_bound_ds;

static void rt_release(Rt *r)
{
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof *r);
}

static Rt *rt_get(const Nv2aSurface *s)
{
    int i, oldest = 0;
    Rt *r;
    D3D11_TEXTURE2D_DESC td;

    /* One target per address. The size arriving here is the clip rectangle,
     * which a title narrows and widens on the same surface within a frame
     * (Godzilla's shadow passes): a narrower clip reuses the target, a wider
     * one grows it with its contents kept. Recreating on every change threw
     * away the finished fight scene mid-frame. */
    for (i = 0; i < MAX_RT; i++) {
        r = &s_rts[i];
        if (r->tex && r->va == s->color_va) {
            if (s->width <= r->w && s->height <= r->h) {
                r->last_frame = s_frame;
                r->pitch = s->pitch; r->bpp = s->bytes_per_pixel;
                return r;
            }
            {
                Rt old = *r;
                uint32_t w = s->width > old.w ? s->width : old.w;
                uint32_t h = s->height > old.h ? s->height : old.h;
                memset(&td, 0, sizeof td);
                td.Width = PX_W(w, h); td.Height = h * (UINT)s_scale;
                td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                memset(r, 0, sizeof *r);
                if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex))) { *r = old; return r; }
                ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
                ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
                {
                    float black[4] = { 0, 0, 0, 1 };
                    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, black);
                }
                ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)r->tex, 0, 0, 0, 0,
                                                          (ID3D11Resource *)old.tex, 0, NULL);
                r->va = old.va; r->w = w; r->h = h;
                r->pitch = s->pitch; r->bpp = s->bytes_per_pixel; r->last_frame = s_frame;
                if (s_bound_rt == r) s_bound_rt = NULL;     /* rebind: new views */
                rt_release(&old);
                return r;
            }
        }
    }
    for (i = 0; i < MAX_RT; i++)
        if (!s_rts[i].tex) { oldest = i; goto create; }
    for (i = 1; i < MAX_RT; i++)
        if (s_rts[i].last_frame < s_rts[oldest].last_frame) oldest = i;
    if (s_present_rt == &s_rts[oldest]) s_present_rt = NULL;
    rt_release(&s_rts[oldest]);
create:
    r = &s_rts[oldest];
    memset(&td, 0, sizeof td);
    td.Width = PX_W(s->width, s->height); td.Height = s->height * (UINT)s_scale;
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex))) return NULL;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    r->va = s->color_va; r->w = s->width; r->h = s->height;
    r->pitch = s->pitch; r->bpp = s->bytes_per_pixel; r->last_frame = s_frame;
    {
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, black);
    }
    if (s_trace)
        fprintf(stderr, "[GPU-D3D11] render target %08X %ux%u (%u bpp, aa %ux%u)\n",
                s->color_va, s->width, s->height, s->bytes_per_pixel * 8, s->aa_sx, s->aa_sy);
    return r;
}

static Ds *ds_get(uint32_t va, uint32_t w, uint32_t h)
{
    int i, slot = -1;
    Ds *d;
    D3D11_TEXTURE2D_DESC td;

    ID3D11Texture2D *keep = NULL;

    if (!va) return NULL;
    /* Like render targets: one per address, grown (contents kept), never
     * recreated for a narrower clip. */
    for (i = 0; i < MAX_DS; i++) {
        d = &s_dss[i];
        if (d->tex && d->va == va) {
            if (w <= d->w && h <= d->h) return d;
            if (w < d->w) w = d->w;
            if (h < d->h) h = d->h;
            slot = i;
            break;
        }
    }
    if (slot < 0)
        for (i = 0; i < MAX_DS; i++)
            if (!s_dss[i].tex) { slot = i; break; }
    if (slot < 0) slot = (int)(s_frame % MAX_DS);
    d = &s_dss[slot];
    if (d == s_bound_ds) s_bound_ds = NULL;
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex && d->va == va) keep = d->tex;
    else if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);
    memset(&td, 0, sizeof td);
    td.Width = PX_W(w, h); td.Height = h * (UINT)s_scale;
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    if (keep) {
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)d->tex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)keep, 0, NULL);
        ID3D11Texture2D_Release(keep);
    }
    d->va = va; d->w = w; d->h = h;
    return d;
}

static void bind_targets(Rt *r, Ds *d)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (r == s_bound_rt && d == s_bound_ds) return;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &null_srv);  /* RT may have been sampled */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (FLOAT)PX_W(r->w, r->h); vp.Height = (FLOAT)(r->h * (UINT)s_scale);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    s_bound_rt = r; s_bound_ds = d;
}

static void note_present_candidate(Rt *r, const Nv2aSurface *s)
{
    /* The frame to show: the most recent surface at least the display's size
     * (640x480 logical). Smaller targets are render-to-texture passes. */
    if (s->width / (s->aa_sx ? s->aa_sx : 1) >= 640 && s->height / (s->aa_sy ? s->aa_sy : 1) >= 480)
        s_present_rt = r;
}

/* ── State objects (cached) ───────────────────────────────── */

static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

static void *state_find(StateEnt *t, int n, uint64_t key)
{
    int i;
    for (i = 0; i < n; i++) if (t[i].key == key) return t[i].obj;
    return NULL;
}

static D3D11_BLEND map_blend(uint32_t f, int alpha_slot)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return alpha_slot ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301:  return alpha_slot ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return alpha_slot ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307:  return alpha_slot ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return alpha_slot ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND_OP map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

static UINT8 map_mask(uint32_t m)
{
    UINT8 w = 0;
    if (m & 0x00010000u) w |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (m & 0x00000100u) w |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (m & 0x00000001u) w |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (m & 0x01000000u) w |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return w;
}

static ID3D11BlendState *blend_state(const Nv2aRenderState *rs)
{
    uint64_t key = 0;
    ID3D11BlendState *b;
    D3D11_BLEND_DESC bd;

    key = mix64(key, rs->blend_enable); key = mix64(key, rs->blend_src);
    key = mix64(key, rs->blend_dst);    key = mix64(key, rs->blend_eq);
    key = mix64(key, rs->color_mask);
    if ((b = (ID3D11BlendState *)state_find(s_blends, s_nblends, key))) return b;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable    = rs->blend_enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend       = map_blend(rs->blend_src, 0);
    bd.RenderTarget[0].DestBlend      = map_blend(rs->blend_dst, 0);
    bd.RenderTarget[0].BlendOp        = map_blend_op(rs->blend_eq);
    bd.RenderTarget[0].SrcBlendAlpha  = map_blend(rs->blend_src, 1);
    bd.RenderTarget[0].DestBlendAlpha = map_blend(rs->blend_dst, 1);
    bd.RenderTarget[0].BlendOpAlpha   = map_blend_op(rs->blend_eq);
    bd.RenderTarget[0].RenderTargetWriteMask = map_mask(rs->color_mask);
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &b))) return s_blend_off;
    if (s_nblends < MAX_STATE) { s_blends[s_nblends].key = key; s_blends[s_nblends].obj = b; s_nblends++; }
    return b;
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207) return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP map_sop(uint32_t o)
{
    switch (o) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *rs, int has_ds)
{
    uint64_t key = 0;
    ID3D11DepthStencilState *d;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (!has_ds) return s_ds_off;
    key = mix64(key, rs->depth_test_enable); key = mix64(key, rs->depth_func);
    key = mix64(key, rs->depth_write);       key = mix64(key, rs->stencil_enable);
    key = mix64(key, rs->stencil_func);      key = mix64(key, rs->stencil_func_mask & 0xFF);
    key = mix64(key, rs->stencil_write_mask & 0xFF);
    key = mix64(key, rs->stencil_fail);      key = mix64(key, rs->stencil_zfail);
    key = mix64(key, rs->stencil_zpass);
    if ((d = (ID3D11DepthStencilState *)state_find(s_depths, s_ndepths, key))) return d;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable    = rs->depth_test_enable ? TRUE : FALSE;
    dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = map_cmp(rs->depth_func);
    dd.StencilEnable  = rs->stencil_enable ? TRUE : FALSE;
    dd.StencilReadMask  = (UINT8)(rs->stencil_func_mask & 0xFF);
    dd.StencilWriteMask = (UINT8)(rs->stencil_write_mask & 0xFF);
    dd.FrontFace.StencilFunc        = map_cmp(rs->stencil_func);
    dd.FrontFace.StencilFailOp      = map_sop(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = map_sop(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp      = map_sop(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &d))) return s_ds_off;
    if (s_ndepths < MAX_STATE) { s_depths[s_ndepths].key = key; s_depths[s_ndepths].obj = d; s_ndepths++; }
    return d;
}

static D3D11_TEXTURE_ADDRESS_MODE map_addr(uint32_t a)
{
    switch (a) {
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

static ID3D11SamplerState *sampler_state(const Nv2aTexture *t)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int linear = (mag == 2) || (min != 1 && min != 0);
    uint64_t key = mix64(mix64(mix64(0, t->addr_u), t->addr_v), (uint64_t)linear);
    int aniso = linear && s_aniso > 1 && !t->cube;
    ID3D11SamplerState *s;
    D3D11_SAMPLER_DESC sm;

    key = mix64(key, (uint64_t)aniso);
    if ((s = (ID3D11SamplerState *)state_find(s_samplers, s_nsamplers, key))) return s;
    memset(&sm, 0, sizeof sm);
    sm.Filter   = aniso ? D3D11_FILTER_ANISOTROPIC
                : linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.MaxAnisotropy = aniso ? (UINT)s_aniso : 1;
    sm.AddressU = map_addr(t->addr_u);
    sm.AddressV = map_addr(t->addr_v);
    sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sm.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &sm, &s))) return s_blit_sampler;
    if (s_nsamplers < MAX_STATE) { s_samplers[s_nsamplers].key = key; s_samplers[s_nsamplers].obj = s; s_nsamplers++; }
    return s;
}

/* ── Textures ─────────────────────────────────────────────── */

/* Cheap content hash: about 2,000 dwords spread over the texture's bytes, so
 * a texture the title rewrites (a movie, a dynamic texture) is noticed. */
static uint32_t tex_hash(const Nv2aTexture *t)
{
    uint32_t bytes = t->width * t->height * 4u, step, h = 2166136261u, i;
    const uint8_t *p = GUEST_PTR(t->offset);

    if (bytes > (4u << 20)) bytes = 4u << 20;
    step = bytes / 2048u;
    if (step < 16) step = 16;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step)
        h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    if (t->palette && t->color == 0x0B) {   /* SZ_I8 only: a palette swap changes the texture. Other
                                             * formats carry a stale palette pointer, which made them
                                             * look rewritten (a decode hitch about once a second). */
        const uint32_t *pal = (const uint32_t *)GUEST_PTR(t->palette);
        h = (h ^ t->palette) * 16777619u;
        for (i = 0; i < 256; i += 4)
            h = (h ^ pal[i]) * 16777619u;
    }
    return h;
}

static uint32_t *s_decode_buf;
static LONGLONG  s_decode_t0;
static size_t    s_decode_cap;

/* -- Texture dump / replacement ------------------------------------------
 * GZ_TEX_DUMP=<dir>: every decoded 2D texture is written once as
 * <dir>/<hash>_<w>x<h>.bmp (32-bit BGRA). A file textures/<hash>*.bmp (same
 * naming; GZ_TEX_DIR overrides the folder) replaces the texture with that
 * hash: any size, 32-bit BGRA BMP. Used for the PlayStation button glyphs and
 * open to texture packs. The hash is FNV-1a over the decoded BGRA pixels. */
int      gz_opt_ps_prompts(void);     /* gz_settings.c: Options > button icons */
unsigned gz_opt_generation(void);
static uint32_t s_repl_hash[512];
static char     s_repl_file[512][64];
static int      s_repl_n = -1;
static const char *s_repl_dir, *s_dump_dir;

static void repl_scan(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH];
    s_repl_n = 0;
    s_dump_dir = getenv("GZ_TEX_DUMP");
    s_repl_dir = getenv("GZ_TEX_DIR");
    if (!s_repl_dir || !*s_repl_dir) s_repl_dir = "textures_ps";
    if (getenv("GZ_TEX_REPLACE") && getenv("GZ_TEX_REPLACE")[0] == '0') return;
    snprintf(pat, sizeof pat, "%s/*.bmp", s_repl_dir);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        char *end;
        unsigned long v = strtoul(fd.cFileName, &end, 16);
        if (end - fd.cFileName == 8 && s_repl_n < 512 && strlen(fd.cFileName) < 64) {
            s_repl_hash[s_repl_n] = (uint32_t)v;
            strcpy(s_repl_file[s_repl_n++], fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    fprintf(stderr, "[GPU-D3D11] %d replacement texture(s) in %s\n", s_repl_n, s_repl_dir);
}

static void tex_dump(uint32_t hash, uint32_t w, uint32_t h, const uint32_t *px)
{
    char path[MAX_PATH];
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v, y;
    FILE *f;
    snprintf(path, sizeof path, "%s/%08X_%ux%u.bmp", s_dump_dir, hash, w, h);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || !(f = fopen(path, "wb"))) return;
    v = 54 + w * h * 4; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (y = 0; y < h; y++) fwrite(px + (size_t)(h - 1 - y) * w, 4, w, f);
    fclose(f);
}

/* Replacement pixels for `hash` (malloc'd, top-down BGRA) or NULL. */
static uint32_t *repl_load(uint32_t hash, uint32_t *w, uint32_t *h)
{
    char path[MAX_PATH];
    uint8_t hdr[54];
    uint32_t off, y, *px;
    int32_t bw, bh, i;
    FILE *f;
    for (i = 0; i < s_repl_n; i++) if (s_repl_hash[i] == hash) break;
    if (i >= s_repl_n) return NULL;
    snprintf(path, sizeof path, "%s/%s", s_repl_dir, s_repl_file[i]);
    if (!(f = fopen(path, "rb"))) return NULL;
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[28] != 32) { fclose(f); return NULL; }
    memcpy(&off, hdr + 10, 4); memcpy(&bw, hdr + 18, 4); memcpy(&bh, hdr + 22, 4);
    if (bw <= 0 || bw > 4096 || bh == 0 || bh > 4096 || bh < -4096) { fclose(f); return NULL; }
    *w = (uint32_t)bw; *h = (uint32_t)(bh < 0 ? -bh : bh);
    px = (uint32_t *)malloc((size_t)*w * *h * 4);
    fseek(f, (long)off, SEEK_SET);
    for (y = 0; px && y < *h; y++)
        if (fread(px + (size_t)(bh < 0 ? y : *h - 1 - y) * *w, 4, *w, f) != *w) { free(px); px = NULL; }
    fclose(f);
    return px;
}

/* GZ_WATER_LOG: where the last texture_srv() answer came from: 1 a render
 * target drawn in the last 4 frames (s_tex_src_age frames ago), 2 a stale
 * target skipped for memory, 0 guest memory. */
static int s_tex_src, s_tex_src_age;

static uint16_t *s_tex_ix_slot;
/* For the hitch log (GZ_PACE_LOG): time spent decoding textures, and how many. */
volatile LONG gz_stat_decode_us, gz_stat_decode_n, gz_stat_decode_px;
static LONGLONG stat_qpc_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return q.QuadPart * 1000000 / f.QuadPart;
}
static uint32_t  s_frame_tick;          /* GetTickCount() at the last flip */
static ID3D11ShaderResourceView *texture_srv(const Nv2aTexture *t)
{
    uint32_t i, slot = MAX_TEX, oldest = 0, h;
    Tex *x;
    D3D11_TEXTURE2D_DESC td;
    size_t need;

    if (!t->offset || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;

    /* Render-to-texture: sample the GPU target the title drew into -- but only
     * one drawn recently. Targets are cached by address forever, and a level
     * load reuses old target memory for ordinary textures: the menus' targets
     * then shadowed the fight's sky and monster skins with black. */
    if (!t->cube)
        for (i = 0; i < MAX_RT; i++)
            if (s_rts[i].tex && s_rts[i].va == t->offset) {
                if (s_rts[i].last_frame + 4 >= s_frame) {
                    s_tex_src = 1; s_tex_src_age = (int)(s_frame - s_rts[i].last_frame);
                    return s_rts[i].srv;
                }
                s_tex_src = 2; s_tex_src_age = (int)(s_frame - s_rts[i].last_frame);
                {
                    static int logged;
                    if (logged++ < 8)
                        fprintf(stderr, "[GPU-D3D11] texture %08X: stale render target (last drawn frame %u, now %u); using texture data\n",
                                t->offset, s_rts[i].last_frame, s_frame);
                }
                break;
            }

    if (t->cube || i >= MAX_RT) s_tex_src = 0;
    {
        /* Most binds are of a texture bound before: look in a small index
         * first (the table has 1,536 slots, searched per bind otherwise). */
        static uint16_t ix[4096];
        uint32_t k = ((t->offset >> 5) ^ (t->offset >> 17) ^ t->color) & 4095u, c = ix[k];
        i = 0;
        if (c) {
            x = &s_texs[c - 1];
            if (x->srv && x->offset == t->offset && x->w == t->width && x->h == t->height
                    && x->color == t->color && x->cube == (t->cube ? 1u : 0u))
                i = c - 1;                  /* the loop below starts on the match */
        }
        s_tex_ix_slot = &ix[k];
    }
    for (; i < MAX_TEX; i++) {
        x = &s_texs[i];
        if (x->srv && x->offset == t->offset && x->w == t->width && x->h == t->height
                && x->color == t->color && x->cube == (t->cube ? 1u : 0u)) {
            *s_tex_ix_slot = (uint16_t)(i + 1);
            if (x->gen != gz_opt_generation()) {
                /* A setting changed (button pictures): decode again. */
                x->gen = gz_opt_generation();
                x->hash = tex_hash(t); x->frame = s_frame; x->used = s_frame;
                slot = i;
                goto decode;
            }
            if (x->frame == s_frame) { x->used = s_frame; return x->srv; }
            /* Check for rewritten contents once per timer tick (~64 times a
             * second), not once per frame: at 240 fps the hashing alone was
             * a visible share of the frame. */
            x->frame = s_frame; x->used = s_frame;
            if (x->tick == s_frame_tick) return x->srv;
            x->tick = s_frame_tick;
            h = tex_hash(t);
            if (h == x->hash) return x->srv;
            x->hash = h;
            slot = i;                       /* contents changed: re-decode below */
            goto decode;
        }
        if (!x->srv) { if (slot == MAX_TEX) slot = i; }
        else if (x->used < s_texs[oldest].used) oldest = i;
    }
    if (slot == MAX_TEX) slot = oldest;
    x = &s_texs[slot];
    if (x->srv) ID3D11ShaderResourceView_Release(x->srv);
    if (x->tex) ID3D11Texture2D_Release(x->tex);
    memset(x, 0, sizeof *x);
    x->offset = t->offset; x->w = t->width; x->h = t->height; x->color = t->color;
    x->cube = t->cube ? 1u : 0u;
    x->hash = tex_hash(t); x->frame = s_frame; x->used = s_frame;
    x->gen = gz_opt_generation();
decode:
    {
        /* GZ_TEX_LOG=1: which textures get decoded again and again. */
        static int tlog = -1, shown;
        if (tlog < 0) tlog = getenv("GZ_TEX_LOG") ? atoi(getenv("GZ_TEX_LOG")) : 0;
        if (tlog && s_frame > (uint32_t)tlog && shown < 4000) {
            shown++;
            fprintf(stderr, "[TEXDEC] frame %u %08X %ux%u fmt %02X cube %u pal %08X\n", s_frame, t->offset, t->width, t->height, t->color, t->cube, t->palette);
        }
    }
    x = &s_texs[slot];
    need = (size_t)t->width * t->height * (t->cube ? 6u : 1u);
    s_decode_t0 = stat_qpc_us();
    InterlockedIncrement(&gz_stat_decode_n);
    InterlockedExchangeAdd(&gz_stat_decode_px, (LONG)need);
    if (need > s_decode_cap) {
        free(s_decode_buf);
        s_decode_buf = (uint32_t *)malloc(need * 4);
        s_decode_cap = s_decode_buf ? need : 0;
        if (!s_decode_buf) return NULL;
    }
    {
        /* A cube map is six faces, each a whole texture (all its mips) apart. */
        uint32_t faces = t->cube ? 6u : 1u, f;
        uint32_t stride = t->cube ? nv2a_backend_texture_face_stride(t) : 0;
        size_t face_px = (size_t)t->width * t->height;
        for (f = 0; f < faces; f++) {
            Nv2aTexture ft = *t;
            ft.offset = t->offset + f * stride; ft.cube = 0;
            if (!nv2a_backend_decode_texture(&ft, s_decode_buf + f * face_px)) {
                static int warned;
                if (warned++ < 8)
                    fprintf(stderr, "[GPU-D3D11] texture format 0x%02X (%ux%u at %08X) not decodable\n",
                            t->color, t->width, t->height, t->offset);
                return NULL;
            }
        }
        /* GZ_ANISO > 1: 2D textures get a generated mip chain (only level 0 of
         * the guest data is decoded), which anisotropic filtering needs to
         * stay sharp at a distance without shimmering. */
        int mips = s_aniso > 1 && !t->cube && (t->width > 1 || t->height > 1);
        uint32_t uw = t->width, uh = t->height, *rp = NULL;
        const uint32_t *px = s_decode_buf;
        if (s_repl_n < 0) repl_scan();
        if (!t->cube && (s_dump_dir || s_repl_n)) {
            /* Identify the texture by its decoded pixels: tex_hash() mixes in
             * the palette's guest address for palettized textures, which is
             * not the same from run to run. */
            uint32_t ch = 2166136261u, n = t->width * t->height, k;
            for (k = 0; k < n; k++) ch = (ch ^ s_decode_buf[k]) * 16777619u;
            ch = (ch ^ t->width) * 16777619u;
            if (s_dump_dir) tex_dump(ch, t->width, t->height, s_decode_buf);
            if (s_repl_n && gz_opt_ps_prompts() && (rp = repl_load(ch, &uw, &uh)) != NULL) px = rp;
        }
        if (x->srv && x->tex) {
            D3D11_TEXTURE2D_DESC cur;
            ID3D11Texture2D_GetDesc(x->tex, &cur);
            if (cur.Width != uw || cur.Height != uh) {      /* replacement of another size */
                ID3D11ShaderResourceView_Release(x->srv); x->srv = NULL;
                ID3D11Texture2D_Release(x->tex); x->tex = NULL;
            } else {
                /* Same size: update in place. */
                for (f = 0; f < faces; f++)
                    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)x->tex, f * cur.MipLevels, NULL,
                                                          px + f * face_px, uw * 4, 0);
                if (cur.MipLevels > 1) ID3D11DeviceContext_GenerateMips(s_ctx, x->srv);
            }
        }
        if (x->srv && x->tex) {
            /* updated above */
        } else if (mips) {
            memset(&td, 0, sizeof td);
            td.Width = uw; td.Height = uh; td.MipLevels = 0; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &x->tex))) { free(rp); return NULL; }
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)x->tex, NULL, &x->srv);
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)x->tex, 0, NULL, px, uw * 4, 0);
            if (x->srv) ID3D11DeviceContext_GenerateMips(s_ctx, x->srv);
        } else {
            D3D11_SUBRESOURCE_DATA sds[6];
            memset(&td, 0, sizeof td);
            td.Width = uw; td.Height = uh; td.MipLevels = 1; td.ArraySize = faces;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
            for (f = 0; f < faces; f++) {
                sds[f].pSysMem = px + f * face_px;
                sds[f].SysMemPitch = uw * 4; sds[f].SysMemSlicePitch = 0;
            }
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, sds, &x->tex))) { free(rp); return NULL; }
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)x->tex, NULL, &x->srv);
        }
        free(rp);
    }
    s_st_uploads++;
    InterlockedExchangeAdd(&gz_stat_decode_us, (LONG)(stat_qpc_us() - s_decode_t0));
    s_h_tex += stat_qpc_us() - s_decode_t0;
    return x->srv;
}

/* ── Constants and vertex upload ──────────────────────────── */

static void set_consts(const Nv2aSurface *s, const Nv2aBatch *b, int tex0_on,
                       const int bound[4])
{
    D3D11_MAPPED_SUBRESOURCE m;
    Consts c;
    const Nv2aRenderState *rs = b->state;
    const Nv2aCombiner *rc = b->combiner;
    float zmax_units = rs->zeta_bits == 16 ? 65535.0f : 16777215.0f;
    int st;

    memset(&c, 0, sizeof c);
    c.wide[0] = s_draw_squeeze; c.wide[1] = s_draw_anchor; c.wide[2] = s_draw_yscale; c.wide[3] = 0.0f;
    {
        static int wl = -1; static unsigned nsq, nanc, ntot, last;
        if (wl < 0) wl = getenv("GZ_WIDE_LOG") != NULL;
        if (wl) {
            ntot++; if (s_draw_squeeze > 0.0f) nsq++; if (s_draw_squeeze > 0.0f && s_draw_anchor != 320.0f) nanc++;
            if (s_frame - last >= 60) { { extern ptrdiff_t g_xbox_mem_offset; const float *pm = (const float *)((uintptr_t)0x00433430u + g_xbox_mem_offset); fprintf(stderr, "[WIDE] draws %u, squeeze set on %u, edge-anchored %u, renderer projection x %.4g y %.4g ratio %.4g\n", ntot, nsq, nanc, pm[0], pm[5], pm[0] != 0.0f ? pm[5] / pm[0] : 0.0f); } ntot = nsq = nanc = 0; last = s_frame; }
        }
    }
    c.inv_size[0] = 1.0f / (float)(s_bound_rt ? s_bound_rt->w : s->width);   /* target size, not clip */
    c.inv_size[1] = 1.0f / (float)(s_bound_rt ? s_bound_rt->h : s->height);
    if (b->texture && b->texture->width && b->texture->height) {
        c.tex0_inv[0] = 1.0f / (float)b->texture->width;
        c.tex0_inv[1] = 1.0f / (float)b->texture->height;
    }
    c.zscale     = 1.0f / zmax_units;
    c.tex0_on    = tex0_on ? 1.0f : 0.0f;
    if (rc) {
        c.rc_on = 1.0f;
        c.fin[0] = rc->final0; c.fin[1] = rc->final1;
        c.fin[2] = rc->control; c.fin[3] = rc->stage_program;
        c.misc[0] = b->clip_plane_mode;
        c.misc[2] = b->dot_mapping;
        c.misc[3] = (b->other_stage_input & 0x7FFFFFFFu) | (s_nodot ? 0x80000000u : 0);
        c.misc[1] = s_trace_req_frame ? (uint32_t)s_rcdbg : 0;  /* debug view once a trace is requested */
        unpack_argb(b->fog_color, c.fog_color);
        unpack_argb(rc->final_c0, c.final_c0);
        unpack_argb(rc->final_c1, c.final_c1);
        for (st = 0; st < 8; st++) {
            c.stage[st][0] = rc->color_icw[st]; c.stage[st][1] = rc->alpha_icw[st];
            c.stage[st][2] = rc->color_ocw[st]; c.stage[st][3] = rc->alpha_ocw[st];
            unpack_argb(rc->factor0[st], c.factor0[st]);
            unpack_argb(rc->factor1[st], c.factor1[st]);
        }
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = b->textures[st];
            c.tex_scale[st][0] = c.tex_scale[st][1] = 1.0f;
            if (t && !t->normalized && t->width && t->height) {
                c.tex_scale[st][0] = 1.0f / (float)t->width;     /* rect: texels */
                c.tex_scale[st][1] = 1.0f / (float)t->height;
            }
            c.tex_scale[st][2] = bound[st] ? (t && t->cube ? 2.0f : 1.0f) : 0.0f;  /* 2: cube */
            c.tex_scale[st][3] = (t && t->normalized) ? 1.0f : 0.0f;
        }
    }
    c.alpha_ref  = (float)(rs->alpha_ref & 0xFF) / 255.0f;
    c.alpha_func = rs->alpha_test_enable ? (float)((rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207)
                                                   ? rs->alpha_func - 0x200 : 7) : 7.0f;
    /* SET_CLIP_MIN/MAX only clip; skip when unset or degenerate. */
    c.zmin = rs->depth_min; c.zmax = rs->depth_max;
    c.zclip_on = (rs->depth_max > rs->depth_min && rs->depth_max > 0.0f) ? 1.0f : 0.0f;
    {
        /* What a specialized shader bakes in (ps_variant). */
        PsKey *k = &s_ps_key;
        memset(k, 0, sizeof *k);
        k->alpha_func = (uint32_t)c.alpha_func;
        k->zclip_on = c.zclip_on > 0.5f;
        if (rc) {
            uint32_t n = c.fin[2] & 255u;
            if (n > 8) n = 8;
            k->rc_on = 1;
            memcpy(k->fin, c.fin, sizeof k->fin);
            memcpy(k->misc, c.misc, sizeof k->misc);
            memcpy(k->stage, c.stage, n * sizeof k->stage[0]);
            for (st = 0; st < 4; st++) k->bound[st] = (uint32_t)c.tex_scale[st][2];
        } else {
            k->tex0_on = tex0_on ? 1u : 0u;
        }
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }
}

static int upload(const Nv2aVertex *v, UINT count, UINT *first)
{
    UINT bytes = count * (UINT)sizeof(Nv2aVertex);
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;

    if (!s_vb || bytes > s_vb_size) {
        D3D11_BUFFER_DESC bd;
        UINT size = s_vb_size ? s_vb_size : (8u << 20);
        while (size < bytes) size *= 2;
        if (s_vb) ID3D11Buffer_Release(s_vb);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = size; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) { s_vb = NULL; s_vb_size = 0; return 0; }
        s_vb_size = size; s_vb_pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (s_vb_pos + bytes > s_vb_size) { s_vb_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mode, 0, &m))) return 0;
    memcpy((uint8_t *)m.pData + s_vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    *first = s_vb_pos / (UINT)sizeof(Nv2aVertex);
    s_vb_pos += bytes;
    return 1;
}

/* Per-triangle culling from projected screen positions (y down). */
static Nv2aVertex *s_cull_buf;
static UINT        s_cull_cap;

static const Nv2aVertex *cull_tris(const Nv2aBatch *b, UINT *count)
{
    const Nv2aRenderState *rs = b->state;
    UINT i, n = 0;

    if (!s_cull || !rs->cull_enable) { *count = b->count; return b->vertices; }
    if (b->count > s_cull_cap) {
        free(s_cull_buf);
        s_cull_buf = (Nv2aVertex *)malloc(sizeof(Nv2aVertex) * b->count);
        s_cull_cap = s_cull_buf ? b->count : 0;
        if (!s_cull_buf) { *count = b->count; return b->vertices; }
    }
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *a = &b->vertices[i], *c = &b->vertices[i + 1], *d = &b->vertices[i + 2];
        float area = (c->x - a->x) * (d->y - a->y) - (d->x - a->x) * (c->y - a->y);
        int cw_on_screen = area > 0.0f;                     /* y down */
        int front = rs->front_face == 0x900 ? cw_on_screen : !cw_on_screen;
        int drop = rs->cull_face == 0x408 || (rs->cull_face == 0x405 && !front)
                || (rs->cull_face == 0x404 && front);
        if (drop) { s_st_culled++; continue; }
        s_cull_buf[n++] = *a; s_cull_buf[n++] = *c; s_cull_buf[n++] = *d;
    }
    *count = n;
    return s_cull_buf;
}

/* ── Back-end callbacks ───────────────────────────────────── */

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Rt *r;
    Ds *d;

    if (!ready() || !s->color_va || !s->width || !s->height) return;
    if (!(r = rt_get(s))) return;
    d = ds_get(rs->zeta_va, r->w, r->h);
    bind_targets(r, d);
    if (s_trace_req_frame && s_frame == s_trace_req_frame && s_trace_req_n < 4000)
        fprintf(stderr, "[TRACE] clear after d%u: surf %08X %ux%u (target %ux%u) flags %02X argb %08X zeta %08X\n",
                s_trace_req_n, s->color_va, s->width, s->height, r->w, r->h, flags, argb, rs->zeta_va);
    if (flags & 0xF0) {
        float c[4];
        c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (float)(argb & 0xFF) / 255.0f;
        c[3] = (float)(argb >> 24) / 255.0f;
        if ((flags & 0xF0) == 0xF0 && s->width >= r->w && s->height >= r->h) {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        } else {
            /* CLEAR_SURFACE clears per channel (R 0x10, G 0x20, B 0x40,
             * A 0x80) within the clip, which ClearRenderTargetView cannot.
             * Godzilla clears only alpha on its finished scene before a
             * destination-alpha pass; clearing all four wiped the fight to
             * black. Draw a full-viewport triangle of the clear colour
             * (blend factor x 1) through a write mask instead. */
            static ID3D11BlendState *mask_bs[16];
            UINT8 m = (UINT8)(((flags & 0x10) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                            | ((flags & 0x20) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                            | ((flags & 0x40) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                            | ((flags & 0x80) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
            if (!mask_bs[m]) {
                D3D11_BLEND_DESC bd;
                memset(&bd, 0, sizeof bd);
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
                bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
                bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].RenderTargetWriteMask = m;
                ID3D11Device_CreateBlendState(s_dev, &bd, &mask_bs[m]);
            }
            if (mask_bs[m] && s_ps_one) {
                D3D11_VIEWPORT vp = { 0, 0, (FLOAT)PX_W(s->width, s->height), (FLOAT)(s->height * (UINT)s_scale), 0, 1 };
                ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
                ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
                ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_blit, NULL, 0);
                ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_one, NULL, 0);
                ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
                ID3D11DeviceContext_OMSetBlendState(s_ctx, mask_bs[m], c, 0xFFFFFFFFu);
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
                ID3D11DeviceContext_Draw(s_ctx, 3, 0);
                s_bound_rt = NULL;          /* restore the full viewport on the next bind */
            }
        }
        note_present_candidate(r, s);
    }
    if (d && (flags & 0x3)) {
        UINT cf = 0;
        float depth;
        UINT8 stencil;
        if (rs->zeta_bits == 16) { depth = (float)(zstencil & 0xFFFF) / 65535.0f; stencil = 0; }
        else { depth = (float)(zstencil >> 8) / 16777215.0f; stencil = (UINT8)(zstencil & 0xFF); }
        if (flags & 0x1) cf |= D3D11_CLEAR_DEPTH;
        if (flags & 0x2) cf |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, cf, depth, stencil);
    }
}

/* Trace frames: write each stage texture, as decoded, next to the trace
 * trigger file (tex_<addr>_<fmt>.bmp, first face, once per address). */
static void trace_dump_tex(const Nv2aTexture *t)
{
    static uint32_t done[128];
    static int ndone;
    char path[600];
    const char *req = getenv("GZ_GPU_TRACE_FILE");
    const char *slash;
    uint32_t *px, hdr[13];
    size_t n;
    FILE *f;
    int i;

    if (!req || !t->width || !t->height || t->width > 2048 || t->height > 2048) return;
    for (i = 0; i < ndone; i++) if (done[i] == t->offset) return;
    if (ndone == 128) return;
    done[ndone++] = t->offset;
    n = (size_t)t->width * t->height;
    if (!(px = (uint32_t *)malloc(n * 4))) return;
    {
        Nv2aTexture ft = *t;
        ft.cube = 0;
        if (!nv2a_backend_decode_texture(&ft, px)) { free(px); return; }
    }
    slash = strrchr(req, '/');
    if (!slash || strrchr(req, '\\') > slash) slash = strrchr(req, '\\');
    snprintf(path, sizeof path, "%.*stex_%08X_%02X.bmp", slash ? (int)(slash - req + 1) : 0, req,
             t->offset, t->color);
    if ((f = fopen(path, "wb"))) {
        /* BITMAPFILEHEADER (from bfSize) + BITMAPINFOHEADER, 32 bpp, top-down. */
        memset(hdr, 0, sizeof hdr);
        hdr[0] = (uint32_t)(54 + n * 4); hdr[2] = 54; hdr[3] = 40;
        hdr[4] = t->width; hdr[5] = (uint32_t)-(int32_t)t->height;
        hdr[6] = 1 | (32u << 16);
        fwrite("BM", 1, 2, f);
        fwrite(hdr, 4, 13, f);
        fwrite(px, 4, n, f);
        fclose(f);
    }
    free(px);
}

/* Targets are bound; set everything the pixel side needs for batch b:
 * textures and samplers, the combiner constants, pixel shader, rasteriser,
 * blend and depth/stencil state. Shared by both draw paths. */
/* GZ_WATER_LOG: one pixel of a target (fractions of its size), read back
 * through a 1x1 staging texture. GZ_WATER_PROBE=x,y picks it. */
static ID3D11Texture2D *s_probe_stg;
static int s_water_pending;
static uint32_t probe1(Rt *r)
{
    static float fx = -1.0f, fy;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t v = 0;
    if (fx < 0.0f) {
        const char *e = getenv("GZ_WATER_PROBE");
        if (!e || sscanf(e, "%f,%f", &fx, &fy) != 2) { fx = 0.3f; fy = 0.92f; }
    }
    if (!r || !r->tex) return 0;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    if (!s_probe_stg) {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &sd, NULL, &s_probe_stg))) return 0;
    }
    box.left = (UINT)(fx * td.Width); box.top = (UINT)(fy * td.Height);
    box.right = box.left + 1; box.bottom = box.top + 1; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_probe_stg, 0, 0, 0, 0,
                                              (ID3D11Resource *)r->tex, 0, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_probe_stg, 0, D3D11_MAP_READ, 0, &m))) {
        v = *(const uint32_t *)m.pData;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_probe_stg, 0);
    }
    return v;
}

static void draw_state(const Nv2aSurface *s, const Nv2aBatch *b, Rt *r, Ds *d)
{
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *smp = s_blit_sampler;
    float bf[4];
    uint32_t bc;

    {
        ID3D11ShaderResourceView *srvs[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        ID3D11SamplerState *smps[4] = { s_blit_sampler, s_blit_sampler, s_blit_sampler, s_blit_sampler };
        int bound[4] = { 0, 0, 0, 0 }, st;

        if (b->combiner) {
            /* Combiner path: every stage the shader program enables. */
            for (st = 0; st < 4; st++) {
                if (!b->textures[st]) continue;
                srvs[st] = texture_srv(b->textures[st]);
                if (srvs[st] == r->srv) srvs[st] = NULL;   /* can't sample the target being drawn */
                smps[st] = sampler_state(b->textures[st]);
                bound[st] = srvs[st] != NULL;
                if (b->textures[st]->cube) { srvs[st + 4] = srvs[st]; srvs[st] = NULL; }  /* cubes: t4-t7 */
            }
            srv = srvs[0];
        } else if (b->texture) {
            srv = texture_srv(b->texture);
            if (srv == r->srv) srv = NULL;
            smp = sampler_state(b->texture);
            srvs[0] = srv; smps[0] = smp; bound[0] = srv != NULL;
        }
        {
            static int wlog = -1;
            if (wlog < 0) wlog = getenv("GZ_WATER_LOG") != NULL;
            if (wlog && b->combiner && b->combiner->stage_program != 0 && b->combiner->stage_program != 1
                    && b->combiner->stage_program != 0x21) {
                fprintf(stderr, "[WATER] f%u before %06X surf %08X sp %08X n%u blend %u(%X,%X) z %u/%u fogc %08X fin %08X/%08X",
                        s_frame, probe1(r) & 0xFFFFFFu, s->color_va, b->combiner->stage_program, b->count / 3,
                        b->state->blend_enable, b->state->blend_src, b->state->blend_dst,
                        b->state->depth_test_enable, b->state->depth_write, b->fog_color,
                        b->combiner->final0, b->combiner->final1);
                for (st = 0; st < 4; st++) {
                    const Nv2aTexture *t = b->textures[st];
                    if (!t) continue;
                    s_tex_src = 0; s_tex_src_age = 0;
                    texture_srv(t);
                    fprintf(stderr, " | t%d %08X fmt %02X %ux%u%s src %d age %d", st, t->offset, t->color,
                            t->width, t->height, t->cube ? " cube" : "", s_tex_src, s_tex_src_age);
                }
                s_water_pending = 1;
            }
        }
        set_consts(s, b, srv != NULL, bound);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srvs);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smps);
    }

    ID3D11DeviceContext_PSSetShader(s_ctx, ps_variant(), NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    bc = b->state->blend_color;
    bf[0] = (float)((bc >> 16) & 0xFF) / 255.0f; bf[1] = (float)((bc >> 8) & 0xFF) / 255.0f;
    bf[2] = (float)(bc & 0xFF) / 255.0f;         bf[3] = (float)(bc >> 24) / 255.0f;
    if (b->state->blend_src == 0x8003 || b->state->blend_src == 0x8004
        || b->state->blend_dst == 0x8003 || b->state->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];        /* constant alpha */
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(b->state), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_state(b->state, d != NULL),
                                               b->state->stencil_ref & 0xFF);
}

/* One [PROBE] line: grid (or GZ_GPU_PROBE) pixels of target r after the
 * draw just issued, numbered with the current trace draw. */
static void save_rt(Rt *r, const char *path);
static void trace_probe(const Nv2aSurface *s, Rt *r)
{
    /* Probe pixels of the target after this draw: which draw sets
     * them. GZ_GPU_PROBE=x,y;x,y;x,y (surface fractions) picks three
     * points; otherwise an 8x6 grid, row by row from the top left
     * (point i is at ((i%8)+0.5)/8, ((i/8)+0.5)/6). */
    static float pf[48][2];
    static int npf;
    D3D11_TEXTURE2D_DESC rd, sd2;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE mp;
    int k;
    if (!npf) {
        const char *e = getenv("GZ_GPU_PROBE");
        if (e && sscanf(e, "%f,%f;%f,%f;%f,%f", &pf[0][0], &pf[0][1], &pf[1][0], &pf[1][1],
                        &pf[2][0], &pf[2][1]) == 6)
            npf = 3;
        else {
            for (k = 0; k < 48; k++) {
                pf[k][0] = ((float)(k % 8) + 0.5f) / 8.0f;
                pf[k][1] = ((float)(k / 8) + 0.5f) / 6.0f;
            }
            npf = 48;
        }
    }
    ID3D11Texture2D_GetDesc(r->tex, &rd);
    sd2 = rd; sd2.MipLevels = 1; sd2.ArraySize = 1;
    sd2.SampleDesc.Count = 1; sd2.SampleDesc.Quality = 0;
    sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.MiscFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    fprintf(stderr, "[PROBE] d%u:", s_trace_req_n);
    if (rd.SampleDesc.Count == 1 && SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &sd2, NULL, &stg))) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)r->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &mp))) {
            for (k = 0; k < npf; k++) {
                UINT x = (UINT)(pf[k][0] * rd.Width), y = (UINT)(pf[k][1] * rd.Height);
                if (x >= rd.Width) x = rd.Width - 1;
                if (y >= rd.Height) y = rd.Height - 1;
                fprintf(stderr, " %08X", ((const uint32_t *)((const uint8_t *)mp.pData + (size_t)y * mp.RowPitch))[x]);
            }
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
        }
        ID3D11Texture2D_Release(stg);
    }
    fprintf(stderr, " surf %08X\n", s->color_va);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Rt *r;
    Ds *d;
    const Nv2aVertex *verts;
    UINT count, first, stride = sizeof(Nv2aVertex), offset = 0;

    if (!ready() || !s->color_va || !s->width || !s->height || b->count < 3) return;
    if (!(r = rt_get(s))) return;
    d = ds_get(b->state->zeta_va, r->w, r->h);
    bind_targets(r, d);
    note_present_candidate(r, s);

    verts = cull_tris(b, &count);
    if (count < 3 || !upload(verts, count, &first)) return;

    s_draw_squeeze = 0.0f;
    if (WIDE_SURF(r->w, r->h)) {
        /* 2D vertices are squeezed in vs_main. The exception is a quad with
         * no picture on it that spans the screen (a fade, a hit flash, a
         * letterbox bar): that must still cover the whole width. */
        s_draw_squeeze = 0.75f;
        s_draw_anchor = (float)r->w * 0.5f;
        s_draw_yscale = 1.0f;
        if (count <= 96) {
            float mn = 1e9f, mx = -1e9f, ymn = 1e9f, ymx = -1e9f;
            UINT k;
            int flat = 1;
            for (k = 0; k < count; k++) {
                if (verts[k].x < mn) mn = verts[k].x;
                if (verts[k].x > mx) mx = verts[k].x;
                if (verts[k].y < ymn) ymn = verts[k].y;
                if (verts[k].y > ymx) ymx = verts[k].y;
                if (verts[k].rhw < 0.99999f || verts[k].rhw > 1.00001f) flat = 0;
            }
            if (flat && count <= 6 && !b->texture && !b->textures[0] && mn <= 2.0f && mx >= (float)r->w - 2.0f)
                s_draw_squeeze = 0.0f;
            else if (flat)
            {
                s_draw_anchor = wide_anchor(mn, mx, ymn, ymx, (float)r->w, (float)r->h);
                wide_hud_scale(s_draw_anchor, (float)r->w, ymx, (float)r->h);
            }
        }
    }
    draw_state(s, b, r, d);

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_Draw(s_ctx, count, first);
    if (s_water_pending) { s_water_pending = 0; fprintf(stderr, " | after %06X\n", probe1(r) & 0xFFFFFFu); }

    s_st_draws++;
    s_st_tris += count / 3;
    if (s_trace_req_frame && s_frame == s_trace_req_frame && s_trace_req_n < 4000) {
        /* GZ_GPU_TRACE_FILE: a full dump of one requested frame. */
        const Nv2aCombiner *rc = b->combiner;
        int st;
        s_trace_req_n++;
        {
            trace_probe(s, r);
        }
        if (s->width <= 512 && getenv("GZ_GPU_TRACE_FILE")) {
            /* An offscreen target (light map, glint): keep its content after this draw. */
            static int nrt;
            const char *req = getenv("GZ_GPU_TRACE_FILE"), *sl = strrchr(req, '/');
            char path[600];
            if (nrt++ < 60) {
                snprintf(path, sizeof path, "%.*srt_%08X_f%u_d%u.bmp", sl ? (int)(sl - req + 1) : 0, req,
                         s->color_va, s_frame, s_trace_req_n);
                save_rt(r, path);
            }
        }
        fprintf(stderr, "[TRACE] d%u: %u tris surf %08X | v0 dif %08X spec %08X fog %.2f rhw %.4f z %.0f | "
                "blend %u(%X,%X,%X) atest %u(%X,%u) z %u/%X/%u stencil %u | ",
                s_trace_req_n, count / 3, s->color_va, verts[0].diffuse, verts[0].specular, verts[0].fog,
                verts[0].rhw, verts[0].z, b->state->blend_enable, b->state->blend_src, b->state->blend_dst,
                b->state->blend_eq, b->state->alpha_test_enable, b->state->alpha_func, b->state->alpha_ref,
                b->state->depth_test_enable, b->state->depth_func, b->state->depth_write,
                b->state->stencil_enable);
        if (rc) {
            fprintf(stderr, "rc ctl %08X sp %08X fin %08X/%08X fc %08X/%08X fogc %08X |", rc->control, rc->stage_program,
                    rc->final0, rc->final1, rc->final_c0, rc->final_c1, b->fog_color);
            for (st = 0; st < (int)(rc->control & 0xFF) && st < 8; st++)
                fprintf(stderr, " s%d c%08X a%08X oc%08X oa%08X f0 %08X f1 %08X", st, rc->color_icw[st],
                        rc->alpha_icw[st], rc->color_ocw[st], rc->alpha_ocw[st], rc->factor0[st], rc->factor1[st]);
            for (st = 0; st < 4; st++) {
                const Nv2aTexture *t = b->textures[st];
                if (t) trace_dump_tex(t);
                if (t) fprintf(stderr, " | t%d %08X fmt %02X %ux%u n%u c%u tc %.2f,%.2f,%.2f,%.2f", st, t->offset,
                               t->color, t->width, t->height, t->normalized, t->cube,
                               verts[0].tc[st][0], verts[0].tc[st][1], verts[0].tc[st][2], verts[0].tc[st][3]);
            }
        } else {
            fprintf(stderr, "no rc, tex %08X", b->texture ? b->texture->offset : 0);
        }
        fprintf(stderr, "\n");
    }
    {
        /* GZ_GPU_TRACE=1 prints the first 40 draws; GZ_GPU_TRACE_AT=<frame>
         * prints the first 60 draws of that frame instead (e.g. a menu). */
        static int at = -2;
        static uint32_t printed;
        int show;
        if (at == -2) { const char *e = getenv("GZ_GPU_TRACE_AT"); at = (e && *e) ? atoi(e) : -1; }
        show = (at < 0) ? (s_trace && s_st_draws <= 40) : ((int)s_frame == at && printed < 60);
        if (show) {
            printed++;
            fprintf(stderr, "[GPU-D3D11] f%u draw: %u tris surf %08X %ux%u tex %s%08X fmt %02X %ux%u "
                    "blend %u(%X,%X) atest %u(%X,%u) z %u/%X/%u | v0 diffuse %08X uv %.1f,%.1f xyz %.1f,%.1f,%.0f rhw %.4f\n",
                    s_frame, count / 3, s->color_va, s->width, s->height,
                    b->texture ? "" : "(none) ", b->texture ? b->texture->offset : 0,
                    b->texture ? b->texture->color : 0, b->texture ? b->texture->width : 0,
                    b->texture ? b->texture->height : 0, b->state->blend_enable, b->state->blend_src,
                    b->state->blend_dst, b->state->alpha_test_enable, b->state->alpha_func,
                    b->state->alpha_ref, b->state->depth_test_enable, b->state->depth_func,
                    b->state->depth_write, verts[0].diffuse, verts[0].u, verts[0].v,
                    verts[0].x, verts[0].y, verts[0].z, verts[0].rhw);
        }
    }
}

/* Copy the presented surface back to guest memory, so the toolkit's frame
 * dump and framebuffer window see what the GPU drew. Only with RECOMP_FB_DUMP
 * (it stalls the pipeline), 32-bit surfaces, render scale 1. */
static void be_present_sync(void)
{
    Rt *r = s_present_rt;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y;

    if (!s_ready || !s_readback || !r || s_scale != 1 || r->bpp != 4 || !r->pitch) return;
    {
        /* RECOMP_FB_DUMP_FLIPS=<file> dumps only while the file exists; skip
         * the readback (a CPU-GPU sync every flip) when no dump will be
         * written, so a test can measure frame rate with dumping paused. */
        static const char *gate = (const char *)-1;
        static int on = 1;
        if (gate == (const char *)-1) gate = getenv("RECOMP_FB_DUMP_FLIPS");
        if (gate && strcmp(gate, "1") != 0) {
            if ((s_frame & 7) == 0) {
                FILE *f = fopen(gate, "rb");
                on = f != NULL;
                if (f) fclose(f);
            }
            if (!on) return;
        }
    }
    ID3D11Texture2D_GetDesc(r->tex, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < r->h; y++)
            memcpy(GUEST_PTR(r->va + y * r->pitch), (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (r->pitch && r->pitch < r->w * 4) ? r->pitch : r->w * 4);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* Save the presented frame at its real (GZ_RENDER_SCALE) size as a 24-bit BMP. */
static void save_rt(Rt *r, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;

    if (!s_ready || !r || !r->tex) return;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        if ((f = fopen(path, "wb")) != NULL) {
            uint32_t w = td.Width, h = td.Height, row = (w * 3 + 3) & ~3u, x, y;
            uint8_t hdr[54] = { 'B', 'M' }, *line = (uint8_t *)calloc(1, row);
            uint32_t v;
            v = 54 + row * h; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
            hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            for (y = 0; line && y < h; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)(h - 1 - y) * m.RowPitch;
                for (x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
                fwrite(line, 1, row, f);
            }
            free(line);
            fclose(f);
            fprintf(stderr, "[GPU] screenshot %s (%ux%u)\n", path, w, h);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void save_shot(const char *path) { save_rt(s_present_rt, path); }

/* Burst capture without stalling the GPU: each frame is copied into one of
 * BURST_RING staging textures and written to disk BURST_RING - 1 frames
 * later, when the GPU has long finished it. (save_shot maps at once, which
 * makes the CPU wait for the GPU every frame and can hide timing bugs.) */
#define BURST_RING 4
static ID3D11Texture2D *s_burst_stg[BURST_RING];
static char s_burst_name[BURST_RING][600];

static void burst_write(int slot)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    ID3D11Texture2D *st = s_burst_stg[slot];
    if (!st || !s_burst_name[slot][0]) return;
    ID3D11Texture2D_GetDesc(st, &td);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        if ((f = fopen(s_burst_name[slot], "wb")) != NULL) {
            uint32_t w = td.Width, h = td.Height, row = (w * 3 + 3) & ~3u, x, y, v;
            uint8_t hdr[54] = { 'B', 'M' }, *line = (uint8_t *)calloc(1, row);
            v = 54 + row * h; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
            hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            for (y = 0; line && y < h; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)(h - 1 - y) * m.RowPitch;
                for (x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
                fwrite(line, 1, row, f);
            }
            free(line);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    s_burst_name[slot][0] = 0;
}

static void burst_copy(int k, const char *path)
{
    int slot = k % BURST_RING;
    D3D11_TEXTURE2D_DESC td, have;
    if (!s_present_rt || !s_present_rt->tex) return;
    burst_write(slot);                                  /* anything still queued there */
    ID3D11Texture2D_GetDesc(s_present_rt->tex, &td);
    if (s_burst_stg[slot]) {
        ID3D11Texture2D_GetDesc(s_burst_stg[slot], &have);
        if (have.Width != td.Width || have.Height != td.Height) {
            ID3D11Texture2D_Release(s_burst_stg[slot]);
            s_burst_stg[slot] = NULL;
        }
    }
    if (!s_burst_stg[slot]) {
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_burst_stg[slot]))) return;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_burst_stg[slot], (ID3D11Resource *)s_present_rt->tex);
    snprintf(s_burst_name[slot], sizeof s_burst_name[slot], "%s", path);
    if (getenv("GZ_WATER_LOG")) fprintf(stderr, "[BURST] %d = frame %u\n", k, s_frame);
    burst_write((k + 1) % BURST_RING);                  /* the oldest copy: done by now */
}

static void be_flip(void)
{
    MSG msg;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *null_srv = NULL;
    float black[4] = { 0, 0, 0, 1 };

    if (!s_ready) return;
    {
        /* GZ_PROFILE: sample the process 30 frames after a trace
         * request, once the traced frame's readbacks are out of the way. */
        extern void gz_profile_start(void);
        if (s_trace_req_frame && s_frame == s_trace_req_frame + 30)
            gz_profile_start();
        {
            /* GZ_PROFILE_AT=<seconds>: or start it that long after the first
             * frame, hands-free (a player heads straight into a fight). */
            static ULONGLONG first;
            static int at = -1;
            if (at < 0) {
                const char *e = getenv("GZ_PROFILE_AT");
                at = e ? atoi(e) : 0;
            }
            if (at > 0) {
                ULONGLONG now = GetTickCount64();
                if (!first) first = now;
                if (now - first >= (ULONGLONG)at * 1000u)
                    gz_profile_start();     /* starts once */
            }
        }
    }
    {
        /* GZ_RAMDUMP=<prefix>: when <prefix>.req exists, write the game's
         * .data section (VA 0x352C40-0x4BE4F0) to <prefix>_<n>.bin and
         * delete the request -- snapshots for finding game state. */
        static const char *rd = (const char *)-1;
        static int rd_n;
        if (rd == (const char *)-1) rd = getenv("GZ_RAMDUMP");
        if (rd && (s_frame & 7) == 0) {
            char req[600], out[600];
            FILE *f;
            snprintf(req, sizeof req, "%s.req", rd);
            if ((f = fopen(req, "rb"))) {
                fclose(f);
                remove(req);
                snprintf(out, sizeof out, "%s_%d.bin", rd, rd_n++);
                if ((f = fopen(out, "wb"))) {
                    fwrite(GUEST_PTR(0x352C40u), 1, 0x4BE4F0u - 0x352C40u, f);
                    fclose(f);
                    fprintf(stderr, "[RAMDUMP] %s (frame %u)\n", out, s_frame);
                }
            }
        }
    }
    {
        /* Live fps in the window title (and so the taskbar), twice a second:
         * the current half-second, then min / max of those readings and the
         * average over the last 10 s (20 buckets). */
        static ULONGLONG t0;
        static uint32_t n, b_frames[20], b_ms[20], nb, bi;
        ULONGLONG now = GetTickCount64();
        n++;
        if (!t0) t0 = now;
        else if (now - t0 >= 500) {
            wchar_t title[160];
            uint32_t k, frames = 0, ms = 0;
            double cur = n * 1000.0 / (double)(now - t0), mn = 1e9, mx = 0.0;
            b_frames[bi] = n; b_ms[bi] = (uint32_t)(now - t0);
            bi = (bi + 1) % 20;
            if (nb < 20) nb++;
            for (k = 0; k < nb; k++) {
                double f = b_frames[k] * 1000.0 / (double)(b_ms[k] ? b_ms[k] : 1);
                if (f < mn) mn = f;
                if (f > mx) mx = f;
                frames += b_frames[k];
                ms += b_ms[k];
            }
            swprintf(title, 160, L"Godzilla: Save the Earth (recomp, D3D11) - %.0f fps | 10 s: min %.0f  max %.0f  avg %.0f",
                     cur, mn, mx, frames * 1000.0 / (double)(ms ? ms : 1));
            SetWindowTextW(s_hwnd, title);
            t0 = now;
            n = 0;
        }
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (s_quit) {
        fprintf(stderr, "[GPU-D3D11] window closed; exiting\n");
        ExitProcess(0);
    }

    if (s_present_rt && s_back_rtv) {
        /* Letterbox the frame (4:3, or 16:9 in widescreen) into the window. */
        float aspect = WIDE_SURF(s_present_rt->w, s_present_rt->h) ? 16.0f / 9.0f : 4.0f / 3.0f;
        float w = (float)s_win_w, h = (float)s_win_h, fw = w, fh = w / aspect;
        if (fh > h) { fh = h; fw = h * aspect; }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
        vp.TopLeftX = (w - fw) * 0.5f; vp.TopLeftY = (h - fh) * 0.5f;
        vp.Width = fw; vp.Height = fh; vp.MinDepth = 0; vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_off, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_blit, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_blit, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_present_rt->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_sampler);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &null_srv);
        {
            static int hlog = -1;
            LONGLONG p0 = stat_qpc_us(), p1;
            IDXGISwapChain_Present(s_swap, (UINT)s_vsync, (!s_vsync && s_tearing) ? GZ_PRESENT_ALLOW_TEARING : 0);
            p1 = stat_qpc_us();
            s_h_present = p1 - p0;
            if (hlog < 0) hlog = getenv("GZ_PACE_LOG") != NULL;
            if (hlog && s_h_last_flip && p1 - s_h_last_flip > 10000)
                fprintf(stderr, "[GPUHITCH] flip to flip %.1f ms: present %.1f, buffer maps %.1f (longest %.1f, %u discards), texture decodes %.1f\n",
                        (p1 - s_h_last_flip) / 1000.0, s_h_present / 1000.0, s_h_map / 1000.0, s_h_map_max / 1000.0,
                        s_h_discards, s_h_tex / 1000.0);
            s_h_last_flip = p1; s_h_map = s_h_map_max = 0; s_h_discards = 0; s_h_tex = 0;
        }
        s_st_presents++;
        s_bound_rt = NULL; s_bound_ds = NULL;       /* rebind on the next draw */
    }
    {
        /* Options > aspect changed: the frame-size targets have the wrong
         * width now. Drop every target; they come back at the next draw. */
        int wide = gz_opt_wide(), k;
        if (wide != s_wide) {
            s_wide = wide;
            for (k = 0; k < MAX_RT; k++) if (s_rts[k].tex) rt_release(&s_rts[k]);
            for (k = 0; k < MAX_DS; k++) {
                Ds *d = &s_dss[k];
                if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
                if (d->tex) ID3D11Texture2D_Release(d->tex);
                memset(d, 0, sizeof *d);
            }
            s_present_rt = NULL; s_bound_rt = NULL; s_bound_ds = NULL;
            fprintf(stderr, "[GPU-D3D11] aspect %s\n", wide ? "16:9" : "4:3");
            if (!s_fullscreen && s_hwnd) {
                /* A window opened at 4:3 letterboxes a 16:9 picture (and the
                 * other way round): give the window the new shape, same
                 * height, and the swap chain buffers to match. */
                RECT rc;
                UINT nw = wide ? (s_win_h * 16u + 8u) / 9u : (s_win_h * 4u + 2u) / 3u;
                rc.left = 0; rc.top = 0; rc.right = (LONG)nw; rc.bottom = (LONG)s_win_h;
                AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
                if (!IsZoomed(s_hwnd))
                    SetWindowPos(s_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                s_win_w = nw;
                ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
                if (s_back_rtv) { ID3D11RenderTargetView_Release(s_back_rtv); s_back_rtv = NULL; }
                IDXGISwapChain_ResizeBuffers(s_swap, 0, s_win_w, s_win_h, DXGI_FORMAT_UNKNOWN, s_tearing ? GZ_SWAP_ALLOW_TEARING : 0);
                create_back_rtv();
            }
        }
    }
    s_frame++;
    s_frame_tick = GetTickCount();
    {
        /* Screenshots: F12 writes screenshots\shot_<n>.bmp; GZ_SHOT_FILE=<path>
         * (tests) writes <path>.bmp whenever the file <path> exists, then
         * deletes it. */
        static const char *req = (const char *)-1;
        static int f12_was, n, burst_left, burst_i;
        int f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        char path[600];
        if (req == (const char *)-1) req = getenv("GZ_SHOT_FILE");
        if (req && (s_frame & 7) == 0 && GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) {
            snprintf(path, sizeof path, "%s.bmp", req);
            save_shot(path);
            DeleteFileA(req);
            burst_left = env_int("GZ_SHOT_BURST", 0, 0, 3000);
            burst_i = 0;
        } else if (req && burst_left > 0) {
            /* GZ_SHOT_BURST=<n>: the n frames after a requested screenshot
             * too, as <path>_<i>.bmp (hunting flicker). */
            snprintf(path, sizeof path, "%s_%03d.bmp", req, burst_i);
            burst_copy(burst_i++, path);
            if (--burst_left == 0) {
                int k;
                for (k = 0; k < BURST_RING; k++) burst_write(k);
            }
        }
        if (f12 && !f12_was && GetForegroundWindow() == s_hwnd) {
            CreateDirectoryA("screenshots", NULL);
            do snprintf(path, sizeof path, "screenshots\\shot_%03d.bmp", n++);
            while (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && n < 1000);
            save_shot(path);
        }
        f12_was = f12;
    }
    {
        /* Hotkeys while playing. F9: capture - trace the next frame into the
         * log and, when RECOMP_FB_DUMP + RECOMP_FB_DUMP_FLIPS are set, dump
         * about 20 frames by creating the gate file briefly. F10: toggle the
         * dot-product texture modes (water reflection) for an A/B look. */
        static int f9_was, f10_was, gate_left;
        int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const char *gate = getenv("RECOMP_FB_DUMP_FLIPS");
        if (f9 && !f9_was) {
            s_trace_req_frame = s_frame + 1;
            s_trace_req_n = 0;
            fprintf(stderr, "[TRACE] frame %u requested (F9)\n", s_trace_req_frame);
            if (gate) {
                HANDLE h = CreateFileA(gate, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                gate_left = 20;
            }
        }
        if (gate_left > 0 && --gate_left == 0 && gate) DeleteFileA(gate);
        if (f10 && !f10_was) {
            s_nodot = !s_nodot;
            fprintf(stderr, "[GPU] F10: dot-product texture modes %s (frame %u)\n", s_nodot ? "OFF" : "on", s_frame);
        }
        f9_was = f9; f10_was = f10;
    }
    {
        static const char *req = (const char *)-1;
        if (req == (const char *)-1) req = getenv("GZ_GPU_TRACE_FILE");
        if (req && (s_frame & 15) == 0 && GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(req);
            s_trace_req_frame = s_frame + 1;
            s_trace_req_n = 0;
            fprintf(stderr, "[TRACE] frame %u requested\n", s_trace_req_frame);
        }
    }

    if (s_trace) {
        DWORD now = GetTickCount();
        if (now - s_st_t0 >= 5000) {
            fprintf(stderr, "[GPU-D3D11] %.1f presents/s, %u draws, %u tris, %u texture uploads, %u culled\n",
                    s_st_presents * 1000.0 / (double)(now - s_st_t0), s_st_draws, s_st_tris,
                    s_st_uploads, s_st_culled);
            s_st_t0 = now; s_st_presents = 0; s_st_draws = 0; s_st_tris = 0;
            s_st_uploads = 0; s_st_culled = 0;
        }
    }
}


/* Ring upload into a dynamic buffer (vertex or index), growing it as needed.
 * Returns the byte offset written at, or (UINT)-1. */
static UINT ring_upload(ID3D11Buffer **buf, UINT *size, UINT *pos, UINT bind,
                        const void *data, UINT bytes)
{
    LONGLONG h0 = stat_qpc_us(), h1;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at;

    if (!*buf || bytes > *size) {
        D3D11_BUFFER_DESC bd;
        UINT sz = *size ? *size : (16u << 20);
        while (sz < bytes) sz *= 2;
        if (*buf) ID3D11Buffer_Release(*buf);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sz; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = bind; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, buf))) { *buf = NULL; *size = 0; return (UINT)-1; }
        *size = sz; *pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (*pos + bytes > *size) { *pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    if (mode == D3D11_MAP_WRITE_DISCARD) s_h_discards++;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)*buf, 0, mode, 0, &m))) return (UINT)-1;
    memcpy((uint8_t *)m.pData + *pos, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)*buf, 0);
    at = *pos;
    *pos += (bytes + 255u) & ~255u;
    h1 = stat_qpc_us() - h0; s_h_map += h1; if (h1 > s_h_map_max) s_h_map_max = h1;
    return at;
}

static int cb_write(ID3D11Buffer *cb, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cb, 0);
    return 1;
}

/* A vertex-program batch: the program runs in vs_prog. */
/* Program draws arrive packed (Nv2aProgramDraw.packed): slot 0 carries the
 * inputs named in the mask, 16 bytes each, per vertex; slot 1 carries all 16
 * inputs once (per instance, one instance) for the rest. One layout per mask. */
static ID3D11InputLayout *prog_layout(uint32_t mask)
{
    static struct { uint32_t mask; ID3D11InputLayout *l; } tab[64];
    static int n;
    D3D11_INPUT_ELEMENT_DESC el[16];
    int k, rank = 0;
    for (k = 0; k < n; k++) if (tab[k].mask == mask) return tab[k].l;
    if (n >= 64 || !s_prog_blob) return NULL;
    for (k = 0; k < 16; k++) {
        int per_vertex = (mask >> k) & 1;
        el[k].SemanticName = "ATTR"; el[k].SemanticIndex = (UINT)k;
        el[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        el[k].InputSlot = per_vertex ? 0 : 1;
        el[k].AlignedByteOffset = (UINT)(16 * (per_vertex ? rank++ : k));
        el[k].InputSlotClass = per_vertex ? D3D11_INPUT_PER_VERTEX_DATA : D3D11_INPUT_PER_INSTANCE_DATA;
        el[k].InstanceDataStepRate = per_vertex ? 0 : 1;
    }
    tab[n].mask = mask; tab[n].l = NULL;
    ID3D11Device_CreateInputLayout(s_dev, el, 16, ID3D10Blob_GetBufferPointer(s_prog_blob),
                                   ID3D10Blob_GetBufferSize(s_prog_blob), &tab[n].l);
    return tab[n++].l;
}

static uint32_t prog_nact(uint32_t mask)
{
    uint32_t n = 0;
    for (mask &= 0xFFFFu; mask; mask &= mask - 1) n++;
    return n;
}

/* Vertex k of a packed draw as the 16 x 4 floats nv2a_vsh_run takes. */
static const float (*pv(const Nv2aProgramDraw *p, uint32_t k))[4]
{
    static float v[16][4];
    const float *src = p->packed + 64 + (size_t)k * prog_nact(p->packed_mask) * 4;
    int a;
    memcpy(v, p->packed, sizeof v);
    for (a = 0; a < 16; a++)
        if ((p->packed_mask >> a) & 1) { memcpy(v[a], src, 16); src += 4; }
    return (const float (*)[4])v;
}

static int be_draw_program(const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aProgramDraw *p)
{
    Rt *r;
    Ds *d;
    UINT vat, iat, strides[2], offsets[2];
    ID3D11Buffer *vbs[2];
    ID3D11InputLayout *layout;
    uint32_t nact;
    ID3D11Buffer *cbs[4];
    struct { uint32_t info[4]; float param[4]; float fog[4]; } dc;

    if (!s_prog_ok || p->ninstr == 0 || p->ninstr > 136) return 0;
    if (!ready()) return 0;
    if (!s->color_va || !s->width || !s->height || p->nindices < 3) return 1;
    if (!p->packed || !(layout = prog_layout(p->packed_mask))) return 0;
    nact = prog_nact(p->packed_mask);
    if (!(r = rt_get(s))) return 1;
    d = ds_get(b->state->zeta_va, r->w, r->h);
    bind_targets(r, d);
    note_present_candidate(r, s);

    vat = ring_upload(&s_vb2, &s_vb2_size, &s_vb2_pos, D3D11_BIND_VERTEX_BUFFER,
                      p->packed, 256u + p->nverts * nact * 16u);
    iat = ring_upload(&s_ib, &s_ib_size, &s_ib_pos, D3D11_BIND_INDEX_BUFFER,
                      p->indices, p->nindices * 4u);
    if (vat == (UINT)-1 || iat == (UINT)-1) return 1;
    if (p->program_rev != s_prog_rev_up) {
        if (!cb_write(s_cb_prog, p->program, p->ninstr * 32u)) return 1;
        s_prog_rev_up = p->program_rev;
    }
    if (p->constants_rev != s_const_rev_up) {
        if (!cb_write(s_cb_const, p->constants, 192 * 16)) return 1;
        s_const_rev_up = p->constants_rev;
    }
    memset(&dc, 0, sizeof dc);
    dc.info[0] = p->ninstr; dc.info[1] = p->near_z;
    dc.info[2] = p->fog_mode; dc.info[3] = p->fog_enable;
    dc.param[0] = p->aa_sx; dc.param[1] = p->aa_sy; dc.param[2] = p->clip_w;
    dc.param[3] = (b->texture && b->texture->normalized) ? 1.0f : 0.0f;
    dc.fog[0] = p->fog_param[0]; dc.fog[1] = p->fog_param[1];
    if (!cb_write(s_cb_draw, &dc, sizeof dc)) return 1;

    s_draw_squeeze = 0.0f;
    if (WIDE_SURF(r->w, r->h)) {
        /* As in be_draw; the positions come from running the program on the
         * CPU for the few vertices of a quad. */
        s_draw_squeeze = 0.75f;
        s_draw_anchor = (float)r->w * 0.5f;
        s_draw_yscale = 1.0f;
        if (p->nverts <= 64) {
            float mn = 1e9f, mx = -1e9f, ymn = 1e9f, ymx = -1e9f;
            uint32_t k;
            int flat = 1;
            for (k = 0; k < p->nverts && flat; k++) {
                Nv2aVshOutput o;
                if (!nv2a_vsh_run(pv(p, k), &o)) { flat = 0; break; }
                if (o.pos[0] < mn) mn = o.pos[0];
                if (o.pos[0] > mx) mx = o.pos[0];
                if (o.pos[1] < ymn) ymn = o.pos[1];
                if (o.pos[1] > ymx) ymx = o.pos[1];
                if (o.pos[3] < 0.99999f || o.pos[3] > 1.00001f) flat = 0;
            }
            if (flat && p->nverts <= 6 && !b->texture && !b->textures[0] && mn <= 2.0f && mx >= (float)r->w - 2.0f)
                s_draw_squeeze = 0.0f;
            else if (flat)
            {
                s_draw_anchor = wide_anchor(mn, mx, ymn, ymx, (float)r->w, (float)r->h);
                wide_hud_scale(s_draw_anchor, (float)r->w, ymx, (float)r->h);
            }
        }
    }
    draw_state(s, b, r, d);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    vbs[0] = s_vb2; vbs[1] = s_vb2;
    strides[0] = nact ? nact * 16u : 16u; strides[1] = 256u;
    offsets[0] = vat + 256u; offsets[1] = vat;
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 2, vbs, strides, offsets);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_prog, NULL, 0);
    cbs[0] = s_cb; cbs[1] = s_cb_prog; cbs[2] = s_cb_const; cbs[3] = s_cb_draw;
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 4, cbs);
    ID3D11DeviceContext_DrawIndexedInstanced(s_ctx, p->nindices, 1, iat / 4u, 0, 0);
    if (s_water_pending) {
        /* + the CPU interpreter's outputs for the first vertex: colours, fog, texcoords */
        Nv2aVshOutput o;
        s_water_pending = 0;
        fprintf(stderr, " | after %06X", probe1(r) & 0xFFFFFFu);
        if (nv2a_vsh_run(pv(p, 0), &o))
            fprintf(stderr, " | v0 d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f %.2f fog %.3g tc0 %.3g %.3g %.3g %.3g tc1 %.3g %.3g %.3g %.3g tc2 %.3g %.3g %.3g %.3g tc3 %.3g %.3g %.3g %.3g",
                    o.d0[0], o.d0[1], o.d0[2], o.d0[3], o.d1[0], o.d1[1], o.d1[2], o.d1[3], o.fog[0],
                    o.tex[0][0], o.tex[0][1], o.tex[0][2], o.tex[0][3], o.tex[1][0], o.tex[1][1], o.tex[1][2], o.tex[1][3],
                    o.tex[2][0], o.tex[2][1], o.tex[2][2], o.tex[2][3], o.tex[3][0], o.tex[3][1], o.tex[3][2], o.tex[3][3]);
        {
            uint32_t k;
            fprintf(stderr, " | nv %u ni %u pos", p->nverts, p->nindices);
            {
                float wmin = 1e30f, ymax = -1e30f, ymin = 1e30f, zmax = -1e30f, zmin = 1e30f;
                uint32_t bad = 0, kmin = 0, kymax = 0;
                for (k = 0; k < p->nverts; k++)
                    if (nv2a_vsh_run(pv(p, k), &o)) {
                        int j, fin = 1;
                        for (j = 0; j < 4; j++) if (!(o.pos[j] == o.pos[j]) || o.pos[j] > 1e30f || o.pos[j] < -1e30f) fin = 0;
                        if (!fin) { bad++; continue; }
                        if (o.pos[3] < wmin) { wmin = o.pos[3]; kmin = k; }
                        if (o.pos[1] > ymax) { ymax = o.pos[1]; kymax = k; }
                        if (o.pos[1] < ymin) ymin = o.pos[1];
                        if (o.pos[2] > zmax) zmax = o.pos[2];
                        if (o.pos[2] < zmin) zmin = o.pos[2];
                    }
                fprintf(stderr, " nonfinite %u wmin %.4g (v%u) y %.4g..%.4g (max at v%u) z %.6g..%.6g", bad, wmin, kmin, ymin, ymax, kymax, zmin, zmax);
                if (nv2a_vsh_run(pv(p, kymax), &o))
                    fprintf(stderr, " lowest [%.4g %.4g %.6g %.4g]", o.pos[0], o.pos[1], o.pos[2], o.pos[3]);
                fprintf(stderr, " c59 %.6g clipw %.4g nearz %u", p->constants[59][2], p->clip_w, p->near_z);
            }
        }
        fprintf(stderr, "\n");
    }
    if (s_trace_req_frame && s_frame == s_trace_req_frame && s_trace_req_n < 4000) {
        /* Program draws in the trace: what was drawn, plus the CPU
         * interpreter's result for the first vertex (nv2a_vsh_run) to set
         * against what the GPU path should have made of it. */
        Nv2aVshOutput cpu;
        int st, ok;
        s_trace_req_n++;
        trace_probe(s, r);
        ok = nv2a_vsh_run(pv(p, 0), &cpu);
        fprintf(stderr, "[TRACE] d%u: PROG %u tris %u verts ninstr %u rev %u surf %08X | blend %u(%X,%X,%X) atest %u z %u/%X/%u"
                " | sp %08X fin %08X fogc %08X fog %u/%X | v0 in0 %.3g %.3g %.3g %.3g",
                s_trace_req_n, p->nindices / 3, p->nverts, p->ninstr, p->program_rev, s->color_va,
                b->state->blend_enable, b->state->blend_src, b->state->blend_dst, b->state->blend_eq,
                b->state->alpha_test_enable, b->state->depth_test_enable, b->state->depth_func,
                b->state->depth_write, b->combiner ? b->combiner->stage_program : 0,
                b->combiner ? b->combiner->final0 : 0, b->fog_color, p->fog_enable, p->fog_mode,
                pv(p, 0)[0][0], pv(p, 0)[0][1], pv(p, 0)[0][2], pv(p, 0)[0][3]);
        if (ok)
            fprintf(stderr, " | cpu pos %.1f %.1f %.4g w %.4g d0 %.2f %.2f %.2f %.2f fog %.3g tc0 %.3g %.3g %.3g %.3g",
                    cpu.pos[0], cpu.pos[1], cpu.pos[2], cpu.pos[3], cpu.d0[0], cpu.d0[1], cpu.d0[2], cpu.d0[3],
                    cpu.fog[0], cpu.tex[0][0], cpu.tex[0][1], cpu.tex[0][2], cpu.tex[0][3]);
        if (ok && p->nverts <= 8) {
            /* Small draws (sky box faces): every vertex's CPU position and
             * the triangle list, to reconstruct the coverage offline. */
            uint32_t v;
            fprintf(stderr, " | c59 %.6g %.6g %.6g | all", p->constants[59][0], p->constants[59][1], p->constants[59][2]);
            for (v = 0; v < p->nverts; v++) {
                Nv2aVshOutput o;
                if (nv2a_vsh_run(pv(p, v), &o))
                    fprintf(stderr, " [%.6g %.6g %.6g %.6g]", o.pos[0] * p->aa_sx, o.pos[1] * p->aa_sy, o.pos[2], o.pos[3]);
            }
            fprintf(stderr, " idx");
            for (v = 0; v < p->nindices && v < 24; v++)
                fprintf(stderr, " %u", p->indices[v]);
        }
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = b->textures[st] ? b->textures[st] : (st == 0 ? b->texture : NULL);
            if (t) fprintf(stderr, " | t%d %08X fmt %02X %ux%u c%u addr %u/%u", st, t->offset, t->color, t->width, t->height, t->cube, t->addr_u, t->addr_v);
        }
        fprintf(stderr, "\n");
    }
    s_st_draws++;
    s_st_prog_draws++;
    s_st_tris += p->nindices / 3;
    return 1;
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_draw_program };

/* Called from main() before the title starts. GZ_GPU=0 keeps the CPU
 * rasteriser. Needs RECOMP_PB_EXEC (the executor is what calls us). */
int gz_gpu_register(void)
{
    const char *e = getenv("GZ_GPU");

    if (e && *e == '0') {
        fprintf(stderr, "[GPU-D3D11] disabled (GZ_GPU=0): CPU rasteriser\n");
        return 0;
    }
    if (!getenv("RECOMP_PB_EXEC")) {
        fprintf(stderr, "[GPU-D3D11] RECOMP_PB_EXEC is not set, so nothing would call the back end; not registering\n");
        return 0;
    }
    nv2a_backend_register_with_present_sync(&s_backend, be_present_sync);
    fprintf(stderr, "[GPU-D3D11] back end registered (device is created on the first draw)\n");
    return 1;
}
