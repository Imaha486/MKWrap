/* Direct3D9 による拡大表示 (GPU で拡大するので高解像度でも軽い)
 * 失敗した場合は呼び出し側が GDI 表示に切り替える */
#define COBJMACROS
#include "mkw.h"
#include <d3d9.h>
#include <string.h>

typedef IDirect3D9 *(WINAPI *Create9_t)(UINT);

typedef struct {
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev;
    IDirect3DTexture9 *tex;
    int tw, th;          /* テクスチャサイズ */
    int bw, bh;          /* バックバッファサイズ */
    int failed;
    D3DPRESENT_PARAMETERS pp;
} d3dctx_t;

static d3dctx_t g_ctx[4];
static CRITICAL_SECTION g_cs;
static int g_cs_init;

typedef struct { float x, y, z, rhw, u, v; } vtx_t;
#define FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static d3dctx_t *ctx_for(HWND h)
{
    int i;
    for (i = 0; i < 4; i++) if (g_ctx[i].hwnd == h) return &g_ctx[i];
    for (i = 0; i < 4; i++) if (!g_ctx[i].hwnd) { memset(&g_ctx[i], 0, sizeof g_ctx[i]); g_ctx[i].hwnd = h; return &g_ctx[i]; }
    return NULL;
}

static void release_tex(d3dctx_t *c) { if (c->tex) { IDirect3DTexture9_Release(c->tex); c->tex = NULL; } c->tw = c->th = 0; }

static int create_device(d3dctx_t *c, int w, int h)
{
    HRESULT hr;
    if (!c->d3d) {
        HMODULE m = LoadLibraryA("d3d9.dll");
        Create9_t f = m ? (Create9_t)GetProcAddress(m, "Direct3DCreate9") : NULL;
        c->d3d = f ? f(D3D_SDK_VERSION) : NULL;
        if (!c->d3d) { mkw_log("D3D9: Direct3DCreate9 失敗\n"); return 0; }
    }
    memset(&c->pp, 0, sizeof c->pp);
    c->pp.Windowed = TRUE;
    c->pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    c->pp.BackBufferFormat = D3DFMT_UNKNOWN;
    c->pp.BackBufferWidth = w; c->pp.BackBufferHeight = h;
    c->pp.hDeviceWindow = c->hwnd;
    c->pp.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(c->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, c->hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &c->pp, &c->dev);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(c->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, c->hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &c->pp, &c->dev);
    if (FAILED(hr)) { mkw_log("D3D9: CreateDevice 失敗 %08lx\n", hr); c->dev = NULL; return 0; }
    c->bw = w; c->bh = h;
    mkw_log("D3D9: デバイス作成 %dx%d\n", w, h);
    return 1;
}

static int reset_device(d3dctx_t *c, int w, int h)
{
    HRESULT hr;
    release_tex(c);
    c->pp.BackBufferWidth = w; c->pp.BackBufferHeight = h;
    hr = IDirect3DDevice9_Reset(c->dev, &c->pp);
    if (FAILED(hr)) return 0;
    c->bw = w; c->bh = h;
    return 1;
}

/* 戻り値 1=表示した 0=失敗 (GDI に切り替える) */
int d3d9_present(vwin_t *w, const RECT *cl, const RECT *d)
{
    d3dctx_t *c; HRESULT hr; D3DLOCKED_RECT lr; int y, cw, ch;
    vtx_t v[4];
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; }
    EnterCriticalSection(&g_cs);
    c = ctx_for(w->hwnd);
    if (!c || c->failed) { LeaveCriticalSection(&g_cs); return 0; }
    cw = cl->right > 0 ? cl->right : 1; ch = cl->bottom > 0 ? cl->bottom : 1;
    { static int n; if (g_cfg.log && n < 40 && (cw != c->bw || ch != c->bh || !c->dev)) { n++; mkw_log("D3D9 present hwnd=%p client=%dx%d dest=%ld,%ld-%ld,%ld canvas=%dx%d\n", (void *)w->hwnd, cw, ch, d->left, d->top, d->right, d->bottom, w->cw, w->ch); } }
    if (!c->dev && !create_device(c, cw, ch)) { c->failed = 1; LeaveCriticalSection(&g_cs); return 0; }
    hr = IDirect3DDevice9_TestCooperativeLevel(c->dev);
    if (hr == D3DERR_DEVICELOST) { LeaveCriticalSection(&g_cs); return 1; }
    if (hr == D3DERR_DEVICENOTRESET || c->bw != cw || c->bh != ch) {
        if (!reset_device(c, cw, ch)) { LeaveCriticalSection(&g_cs); return 1; }
    }
    if (!c->tex || c->tw != w->cw || c->th != w->ch) {
        release_tex(c);
        hr = IDirect3DDevice9_CreateTexture(c->dev, w->cw, w->ch, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &c->tex, NULL);
        if (FAILED(hr)) { mkw_log("D3D9: テクスチャ作成失敗 %08lx\n", hr); c->failed = 1; LeaveCriticalSection(&g_cs); return 0; }
        c->tw = w->cw; c->th = w->ch;
    }
    GdiFlush();
    if (SUCCEEDED(IDirect3DTexture9_LockRect(c->tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (y = 0; y < w->ch; y++)
            memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, (const BYTE *)w->cbits + (size_t)y * w->cw * 4, (size_t)w->cw * 4);
        IDirect3DTexture9_UnlockRect(c->tex, 0);
    }
    IDirect3DDevice9_Clear(c->dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(c->dev))) {
        float l = (float)d->left - 0.5f, t = (float)d->top - 0.5f, r = (float)d->right - 0.5f, b = (float)d->bottom - 0.5f;
        D3DTEXTUREFILTERTYPE ft = g_cfg.filter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        v[0].x = l; v[0].y = t; v[0].u = 0; v[0].v = 0;
        v[1].x = r; v[1].y = t; v[1].u = 1; v[1].v = 0;
        v[2].x = l; v[2].y = b; v[2].u = 0; v[2].v = 1;
        v[3].x = r; v[3].y = b; v[3].u = 1; v[3].v = 1;
        for (y = 0; y < 4; y++) { v[y].z = 0; v[y].rhw = 1; }
        IDirect3DDevice9_SetFVF(c->dev, FVF);
        IDirect3DDevice9_SetTexture(c->dev, 0, (IDirect3DBaseTexture9 *)c->tex);
        IDirect3DDevice9_SetSamplerState(c->dev, 0, D3DSAMP_MINFILTER, ft);
        IDirect3DDevice9_SetSamplerState(c->dev, 0, D3DSAMP_MAGFILTER, ft);
        IDirect3DDevice9_SetSamplerState(c->dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(c->dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetRenderState(c->dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(c->dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(c->dev, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_DrawPrimitiveUP(c->dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(vtx_t));
        IDirect3DDevice9_EndScene(c->dev);
    }
    hr = IDirect3DDevice9_Present(c->dev, NULL, NULL, NULL, NULL);
    LeaveCriticalSection(&g_cs);
    return 1;
}

void d3d9_release_window(HWND h)
{
    int i;
    if (!g_cs_init) return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < 4; i++) if (g_ctx[i].hwnd == h) {
        release_tex(&g_ctx[i]);
        if (g_ctx[i].dev) IDirect3DDevice9_Release(g_ctx[i].dev);
        if (g_ctx[i].d3d) IDirect3D9_Release(g_ctx[i].d3d);
        memset(&g_ctx[i], 0, sizeof g_ctx[i]);
    }
    LeaveCriticalSection(&g_cs);
}
