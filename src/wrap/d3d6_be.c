/* Direct3D (DirectX 6 世代) エミュレーションの描画側: Direct3D 9 で描く
 * d3d6.c からだけ使う。ddraw.h / d3d.h と d3d9.h は同時に include できないので分けている */
#define COBJMACROS
#include "mkw.h"
#include <d3d9.h>
#include <string.h>
#include "d3d6_be.h"

typedef IDirect3D9 *(WINAPI *Create9_t)(UINT);

typedef struct { IDirect3DSurface9 *col, *z; int w, h; } rt_t;
typedef struct { IDirect3DTexture9 *t; int w, h; } tex_t;

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static HWND g_hwnd;
static int g_failed;
static D3DFORMAT g_zfmt = D3DFMT_D24X8;
static rt_t *g_cur;
static int g_in_scene;
static D3DPRESENT_PARAMETERS g_pp;
static rt_t *g_rts[64];
static D3DVIEWPORT9 g_vp; static int g_vp_set;

static LRESULT CALLBACK be_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

/* 描画用のデバイスはゲームの窓に結び付ける (Present はしないので窓の表示には影響しない) */
void be_set_window(HWND h) { if (!g_dev && h && IsWindow(h)) g_hwnd = h; }

int be_init(void)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    if (g_dev) return 1;
    if (g_failed) return 0;
    g_failed = 1;
    {
        HMODULE m = LoadLibraryA("d3d9.dll");
        Create9_t f = m ? (Create9_t)GetProcAddress(m, "Direct3DCreate9") : NULL;
        g_d3d = f ? f(D3D_SDK_VERSION) : NULL;
        if (!g_d3d) { mkw_log("D3D6: Direct3DCreate9 失敗\n"); return 0; }
    }
    if (!g_hwnd) {   /* ゲームの窓が分からない場合だけ、見えない窓を作る */
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = be_wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MKWrapD3D6";
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MKWrapD3D6", L"", WS_POPUP, -32000, -32000, 16, 16, NULL, NULL, wc.hInstance, NULL);
    }
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 16; pp.BackBufferHeight = 16; pp.hDeviceWindow = g_hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr)) { mkw_log("D3D6: CreateDevice 失敗 %08lx\n", (unsigned long)hr); g_dev = NULL; return 0; }
    g_pp = pp;
    if (FAILED(IDirect3D9_CheckDeviceFormat(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24X8)))
        g_zfmt = D3DFMT_D16;
    /* DirectX 6 の既定値に合わせる */
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DITHERENABLE, FALSE);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_failed = 0;
    mkw_fix_cwd("Direct3D (DirectX 6) の初期化の後");
    mkw_log("D3D6: Direct3D9 で描画します (Z=%s)\n", g_zfmt == D3DFMT_D16 ? "D16" : "D24X8");
    return 1;
}

/* ---- 描画先 ---- */
void *be_rt_new(int w, int h)
{
    rt_t *r;
    if (!be_init()) return NULL;
    r = (rt_t *)calloc(1, sizeof *r);
    r->w = w; r->h = h;
    if (FAILED(IDirect3DDevice9_CreateRenderTarget(g_dev, w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &r->col, NULL))) {
        mkw_log("D3D6: CreateRenderTarget %dx%d 失敗\n", w, h); free(r); return NULL;
    }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, w, h, g_zfmt, D3DMULTISAMPLE_NONE, 0, FALSE, &r->z, NULL))) r->z = NULL;
    IDirect3DDevice9_ColorFill(g_dev, r->col, NULL, 0);
    { int i; for (i = 0; i < 64; i++) if (!g_rts[i]) { g_rts[i] = r; break; } }
    return r;
}
void be_rt_free(void *p)
{
    rt_t *r = (rt_t *)p;
    if (!r) return;
    if (g_cur == r) {
        IDirect3DSurface9 *bb = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb); IDirect3DSurface9_Release(bb); }
        IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
        g_cur = NULL;
    }
    if (r->col) IDirect3DSurface9_Release(r->col);
    if (r->z) IDirect3DSurface9_Release(r->z);
    { int i; for (i = 0; i < 64; i++) if (g_rts[i] == r) g_rts[i] = NULL; }
    free(r);
}

static int msh(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mbits(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

/* 描画結果 (X8R8G8B8) をサーフェスの形式で bits へ */
void be_rt_read(void *p, BYTE *bits, int pitch, int bpp, DWORD rm, DWORD gm, DWORD bm)
{
    rt_t *r = (rt_t *)p; D3DLOCKED_RECT lr; int x, y;
    if (!r || !bits || !r->col) return;
    if (FAILED(IDirect3DSurface9_LockRect(r->col, &lr, NULL, D3DLOCK_READONLY))) return;
    if (bpp == 32 && rm == 0xFF0000 && gm == 0xFF00 && bm == 0xFF) {
        for (y = 0; y < r->h; y++) memcpy(bits + (size_t)y * pitch, (BYTE *)lr.pBits + (size_t)y * lr.Pitch, (size_t)r->w * 4);
    } else if (bpp == 16 || bpp == 32) {
        int rs = msh(rm), gs = msh(gm), bs = msh(bm), rb = mbits(rm), gb = mbits(gm), bb = mbits(bm);
        for (y = 0; y < r->h; y++) {
            const DWORD *s = (const DWORD *)((BYTE *)lr.pBits + (size_t)y * lr.Pitch);
            if (bpp == 16) {
                WORD *d = (WORD *)(bits + (size_t)y * pitch);
                if (rm == 0xF800 && gm == 0x7E0 && bm == 0x1F)
                    for (x = 0; x < r->w; x++) { DWORD c = s[x]; d[x] = (WORD)((c >> 8 & 0xF800) | (c >> 5 & 0x7E0) | (c >> 3 & 0x1F)); }
                else
                    for (x = 0; x < r->w; x++) { DWORD c = s[x];
                        d[x] = (WORD)(((c >> 16 & 0xFF) >> (8 - rb) << rs) | ((c >> 8 & 0xFF) >> (8 - gb) << gs) | ((c & 0xFF) >> (8 - bb) << bs)); }
            } else {
                DWORD *d = (DWORD *)(bits + (size_t)y * pitch);
                for (x = 0; x < r->w; x++) { DWORD c = s[x];
                    d[x] = ((c >> 16 & 0xFF) >> (8 - rb) << rs) | ((c >> 8 & 0xFF) >> (8 - gb) << gs) | ((c & 0xFF) >> (8 - bb) << bs); }
            }
        }
    }
    IDirect3DSurface9_UnlockRect(r->col);
}
/* CPU で書き換えられたサーフェスの内容を描画先へ */
void be_rt_write(void *p, const BYTE *bits, int pitch, int bpp, DWORD rm, DWORD gm, DWORD bm)
{
    rt_t *r = (rt_t *)p; D3DLOCKED_RECT lr; int x, y;
    if (!r || !bits || !r->col) return;
    if (g_in_scene) { IDirect3DDevice9_EndScene(g_dev); }
    if (FAILED(IDirect3DSurface9_LockRect(r->col, &lr, NULL, 0))) { if (g_in_scene) IDirect3DDevice9_BeginScene(g_dev); return; }
    {
        int rs = msh(rm), gs = msh(gm), bs = msh(bm), rb = mbits(rm), gb = mbits(gm), bb = mbits(bm);
        for (y = 0; y < r->h; y++) {
            DWORD *d = (DWORD *)((BYTE *)lr.pBits + (size_t)y * lr.Pitch);
            for (x = 0; x < r->w; x++) {
                DWORD c = bpp == 16 ? ((const WORD *)(bits + (size_t)y * pitch))[x] : ((const DWORD *)(bits + (size_t)y * pitch))[x];
                DWORD R = rb ? (c & rm) >> rs << (8 - rb) : 0, G = gb ? (c & gm) >> gs << (8 - gb) : 0, B = bb ? (c & bm) >> bs << (8 - bb) : 0;
                if (rb && rb < 8) R |= R >> rb;
                if (gb && gb < 8) G |= G >> gb;
                if (bb && bb < 8) B |= B >> bb;
                d[x] = 0xFF000000 | R << 16 | G << 8 | B;
            }
        }
    }
    IDirect3DSurface9_UnlockRect(r->col);
    if (g_in_scene) IDirect3DDevice9_BeginScene(g_dev);
}

/* ---- テクスチャ ---- */
void *be_tex_new(int w, int h)
{
    tex_t *t;
    if (!be_init()) return NULL;
    t = (tex_t *)calloc(1, sizeof *t);
    t->w = w; t->h = h;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t->t, NULL))) {
        mkw_log("D3D6: CreateTexture %dx%d 失敗\n", w, h); free(t); return NULL;
    }
    return t;
}
void be_tex_free(void *p)
{
    tex_t *t = (tex_t *)p;
    if (!t) return;
    IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
    IDirect3DDevice9_SetTexture(g_dev, 1, NULL);
    IDirect3DTexture9_Release(t->t);
    free(t);
}
void be_tex_write(void *p, const DWORD *argb)
{
    tex_t *t = (tex_t *)p; D3DLOCKED_RECT lr; int y;
    if (!t) return;
    if (FAILED(IDirect3DTexture9_LockRect(t->t, 0, &lr, NULL, 0))) return;
    for (y = 0; y < t->h; y++) memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, argb + (size_t)y * t->w, (size_t)t->w * 4);
    IDirect3DTexture9_UnlockRect(t->t, 0);
}

/* ---- 状態 ---- */
void be_viewport(DWORD x, DWORD y, DWORD w, DWORD h, float mn, float mx);
int be_target(void *p)
{
    rt_t *r = (rt_t *)p;
    if (!g_dev || !r || !r->col) return 0;
    if (g_cur != r) {
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, r->col);
        IDirect3DDevice9_SetDepthStencilSurface(g_dev, r->z);
        g_cur = r;
        /* SetRenderTarget でビューポートが全体に戻るので設定し直す */
        if (g_vp_set) be_viewport(g_vp.X, g_vp.Y, g_vp.Width, g_vp.Height, g_vp.MinZ, g_vp.MaxZ);
    }
    return r->z != NULL;
}
void be_rs(DWORD s, DWORD v) { if (g_dev) IDirect3DDevice9_SetRenderState(g_dev, (D3DRENDERSTATETYPE)s, v); }
void be_tss(DWORD st, DWORD t, DWORD v) { if (g_dev) IDirect3DDevice9_SetTextureStageState(g_dev, st, (D3DTEXTURESTAGESTATETYPE)t, v); }
void be_samp(DWORD st, DWORD t, DWORD v) { if (g_dev) IDirect3DDevice9_SetSamplerState(g_dev, st, (D3DSAMPLERSTATETYPE)t, v); }
void be_settex(DWORD st, void *p) { if (g_dev) IDirect3DDevice9_SetTexture(g_dev, st, p ? (IDirect3DBaseTexture9 *)((tex_t *)p)->t : NULL); }
void be_viewport(DWORD x, DWORD y, DWORD w, DWORD h, float mn, float mx)
{
    D3DVIEWPORT9 v;
    if (!g_dev) return;
    if (g_cur) {
        if ((int)x >= g_cur->w) x = 0;
        if ((int)y >= g_cur->h) y = 0;
        if ((int)(x + w) > g_cur->w) w = g_cur->w - x;
        if ((int)(y + h) > g_cur->h) h = g_cur->h - y;
    }
    v.X = x; v.Y = y; v.Width = w; v.Height = h; v.MinZ = mn; v.MaxZ = mx;
    g_vp = v; g_vp_set = 1;
    if (g_cur) IDirect3DDevice9_SetViewport(g_dev, &v);
}
void be_clear(DWORD n, const LONG *rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
    DWORD f = 0;
    if (!g_dev || !g_cur) return;
    if (flags & 1) f |= D3DCLEAR_TARGET;
    if ((flags & 2) && g_cur->z) f |= D3DCLEAR_ZBUFFER;
    if ((flags & 4) && g_cur->z && g_zfmt != D3DFMT_D16 && 0) f |= D3DCLEAR_STENCIL;
    if (!f) return;
    IDirect3DDevice9_Clear(g_dev, n, (const D3DRECT *)rects, f, color, z, stencil);
}
void be_scene(int begin)
{
    if (!g_dev) return;
    if (begin && !g_in_scene) { IDirect3DDevice9_BeginScene(g_dev); g_in_scene = 1; }
    else if (!begin && g_in_scene) { IDirect3DDevice9_EndScene(g_dev); g_in_scene = 0; }
}
void be_transform(DWORD st, const float *m) { if (g_dev) IDirect3DDevice9_SetTransform(g_dev, (D3DTRANSFORMSTATETYPE)st, (const D3DMATRIX *)m); }
void be_material(const float *m) { if (g_dev) IDirect3DDevice9_SetMaterial(g_dev, (const D3DMATERIAL9 *)m); }
void be_light(DWORD i, const be_light_t *l)
{
    D3DLIGHT9 L;
    if (!g_dev) return;
    if (!l) { IDirect3DDevice9_LightEnable(g_dev, i, FALSE); return; }
    memset(&L, 0, sizeof L);
    L.Type = l->type == 2 || l->type == 5 ? D3DLIGHT_SPOT : l->type == 3 ? D3DLIGHT_DIRECTIONAL : D3DLIGHT_POINT;
    memcpy(&L.Diffuse, l->color, 16); memcpy(&L.Specular, l->color, 16);
    memcpy(&L.Position, l->pos, 12); memcpy(&L.Direction, l->dir, 12);
    L.Range = l->range; L.Falloff = l->falloff; L.Attenuation0 = l->att[0]; L.Attenuation1 = l->att[1]; L.Attenuation2 = l->att[2];
    L.Theta = l->theta; L.Phi = l->phi;
    if (L.Type != D3DLIGHT_DIRECTIONAL && L.Range <= 0) L.Range = 1e10f;
    if (L.Type != D3DLIGHT_DIRECTIONAL && L.Attenuation0 == 0 && L.Attenuation1 == 0 && L.Attenuation2 == 0) L.Attenuation0 = 1;
    IDirect3DDevice9_SetLight(g_dev, i, &L);
    IDirect3DDevice9_LightEnable(g_dev, i, TRUE);
}

static UINT prim_count(DWORD prim, DWORD n)
{
    switch (prim) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n / 2;
    case D3DPT_LINESTRIP: return n > 1 ? n - 1 : 0;
    case D3DPT_TRIANGLELIST: return n / 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n > 2 ? n - 2 : 0;
    }
    return 0;
}
void be_draw(DWORD prim, DWORD fvf, DWORD stride, const void *v, DWORD nv, const WORD *idx, DWORD ni)
{
    UINT pc;
    if (!g_dev || !g_cur) return;
    pc = prim_count(prim, idx ? ni : nv);
    if (!pc) return;
    if (!g_in_scene) { IDirect3DDevice9_BeginScene(g_dev); g_in_scene = 1; }
    IDirect3DDevice9_SetFVF(g_dev, fvf);
    if (idx) IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, (D3DPRIMITIVETYPE)prim, 0, nv, pc, idx, D3DFMT_INDEX16, v, stride);
    else IDirect3DDevice9_DrawPrimitiveUP(g_dev, (D3DPRIMITIVETYPE)prim, pc, v, stride);
}

/* デバイスの消失 (画面のロック・解像度変更など) からの復帰
 * 戻り値: 0 = 正常 / 1 = 作り直した (ステートを設定し直す必要あり) / -1 = 消失中 (描けない) */
int be_check(void)
{
    HRESULT hr; int i;
    if (!g_dev) return -1;
    hr = IDirect3DDevice9_TestCooperativeLevel(g_dev);
    if (hr == D3D_OK) return 0;
    if (hr == D3DERR_DEVICELOST) return -1;
    if (hr != D3DERR_DEVICENOTRESET) return -1;
    if (g_in_scene) { IDirect3DDevice9_EndScene(g_dev); g_in_scene = 0; }
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, NULL);
    for (i = 0; i < 64; i++) if (g_rts[i]) {
        rt_t *r = g_rts[i];
        if (r->col) { IDirect3DSurface9_Release(r->col); r->col = NULL; }
        if (r->z) { IDirect3DSurface9_Release(r->z); r->z = NULL; }
    }
    g_cur = NULL;
    hr = IDirect3DDevice9_Reset(g_dev, &g_pp);
    if (FAILED(hr)) { mkw_log("D3D6: Reset 失敗 %08lx\n", (unsigned long)hr); return -1; }
    for (i = 0; i < 64; i++) if (g_rts[i]) {
        rt_t *r = g_rts[i];
        if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(g_dev, r->w, r->h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &r->col, NULL)))
            IDirect3DDevice9_ColorFill(g_dev, r->col, NULL, 0);
        else r->col = NULL;
        if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, r->w, r->h, g_zfmt, D3DMULTISAMPLE_NONE, 0, FALSE, &r->z, NULL))) r->z = NULL;
    }
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    mkw_log("D3D6: デバイスを作り直しました\n");
    return 1;
}
