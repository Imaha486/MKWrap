/* Direct3D (DirectX 6 世代: IDirect3D3 / IDirect3DDevice3) のエミュレーション
 * MKWrap の DirectDraw エミュレーションのサーフェス (DIB) の上で動き、描画は Direct3D 9 で行う。
 *   - 3D の描画先 (バックバッファ) は Direct3D9 のレンダーターゲットと対になり、
 *     Flip や CPU アクセス (Lock / Blt / GetDC) の前に描画結果を DIB へ読み戻す
 *   - CPU で書き換えられた描画先は、次の 3D 描画の前に Direct3D9 側へ書き込む
 *   - テクスチャは使う時点で内容が変わっていれば A8R8G8B8 に変換して転送する
 * こうして DIB 側にいつも最新の絵があるので、窓表示・拡大・擬似フルスクリーンは 2D ゲームと同じ仕組みで動く */
#include "ddraw_int.h"
#include <d3d.h>
#include <math.h>
#include "d3d6_be.h"

typedef struct Dev Dev;
typedef struct Vp Vp;

typedef struct { IFACE i; DDraw *dd; } D3DObj;
typedef struct { IFACE i; LONG ref; D3DMATERIAL m; DWORD h; } Mat;
typedef struct { IFACE i; LONG ref; D3DLIGHT2 l; Vp *vp; } Light;
struct Vp { IFACE i; LONG ref; Dev *dev; D3DVIEWPORT2 v; int has; DWORD bg; Light *lt[8]; int nl; };
typedef struct { IFACE i; LONG ref; D3DVERTEXBUFFERDESC d; BYTE *data; DWORD stride; } VB;
struct Dev {
    IFACE i; LONG ref;
    DDraw *dd;
    Surf *target;
    Vp *vp[16]; int nvp; Vp *cur;
    DWORD rs[256];
    DWORD tss[8][32];
    Surf *tex[8];
    DWORD ls[16];
    float tf[4][16];
    int null_ovr;          /* テクスチャなしのため stage0 を差し替え中 */
    int lights_dirty;
    /* Begin/Vertex/End */
    DWORD bprim, bfvf, bflags; BYTE *bbuf; DWORD bn, bcap; WORD *bidx; DWORD bni, bicap; int bindexed;
};

static void **vt_d3d, **vt_dev, **vt_vp, **vt_mat, **vt_light, **vt_tex1, **vt_tex2, **vt_vb;
static Dev *g_devs[8];
static Mat *g_mats[256];
static Surf *g_texh[1024];

#define RET_VOID(x) (void)(x)

/* ================= 補助 ================= */
static DWORD fvf_stride(DWORD fvf)
{
    static const int pos[8] = { 0, 12, 16, 16, 20, 24, 28, 32 };
    DWORD s = pos[(fvf >> 1) & 7], n, i;
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_RESERVED1) s += 4;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    n = (fvf >> 8) & 15;
    for (i = 0; i < n && i < 8; i++) {
        static const int sz[4] = { 8, 12, 16, 4 };
        s += sz[(fvf >> (16 + i * 2)) & 3];
    }
    return s;
}
static DWORD col_from_value(const D3DCOLORVALUE *c)
{
    float v[4] = { c->a, c->r, c->g, c->b }; DWORD o = 0; int i;
    for (i = 0; i < 4; i++) { int k = (int)(v[i] * 255.0f + 0.5f); if (k < 0) k = 0; if (k > 255) k = 255; o = o << 8 | (DWORD)k; }
    return o;
}
static int msh(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mbits(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }
static DWORD ext8(DWORD v, DWORD m)
{
    int b = mbits(m); if (!b) return 0;
    v = (v & m) >> msh(m);
    if (b >= 8) return v >> (b - 8);
    v <<= 8 - b;
    while (b < 8) { v |= v >> b; b *= 2; }
    return v & 0xFF;
}

/* テクスチャの内容を Direct3D9 へ */
static void tex_sync(Surf *s)
{
    int x, y, fresh = 0;
    DWORD *buf; const PALETTEENTRY *pal = NULL;
    if (!s->be_tex) { s->be_tex = be_tex_new(s->w, s->h); fresh = 1; if (!s->be_tex) return; }
    if (!fresh && !s->cpu_dirty) return;
    s->cpu_dirty = 0;
    if (s->ext || !s->bits) { if (!s->bits) return; }
    GdiFlush();
    buf = (DWORD *)malloc((size_t)s->w * s->h * 4);
    if (!buf) return;
    if (s->bpp == 8) pal = s->pal ? s->pal->e : (s->dd && s->dd->primary && s->dd->primary->pal ? s->dd->primary->pal->e : NULL);
    {
        int keyed = (s->ckflags & DDCKEY_SRCBLT) != 0;
        DWORD lo = s->ck_src.dwColorSpaceLowValue, hi = s->ck_src.dwColorSpaceHighValue;
        if (hi < lo) hi = lo;
        for (y = 0; y < s->h; y++) {
            const BYTE *row = s->bits + (size_t)y * s->pitch;
            DWORD *d = buf + (size_t)y * s->w;
            for (x = 0; x < s->w; x++) {
                DWORD p, c;
                switch (s->bpp) {
                case 8: p = row[x]; c = pal ? (0xFF000000 | (DWORD)pal[p].peRed << 16 | (DWORD)pal[p].peGreen << 8 | pal[p].peBlue) : (0xFF000000 | p * 0x010101); break;
                case 16: p = ((const WORD *)row)[x];
                    c = (s->amask ? ext8(p, s->amask) << 24 : 0xFF000000) | ext8(p, s->rmask) << 16 | ext8(p, s->gmask) << 8 | ext8(p, s->bmask); break;
                case 24: p = row[x * 3] | row[x * 3 + 1] << 8 | (DWORD)row[x * 3 + 2] << 16; c = 0xFF000000 | p; break;
                default: p = ((const DWORD *)row)[x];
                    c = (s->amask ? ext8(p, s->amask) << 24 : 0xFF000000) | ext8(p, s->rmask) << 16 | ext8(p, s->gmask) << 8 | ext8(p, s->bmask); break;
                }
                if (keyed && p >= lo && p <= hi) c = 0;
                d[x] = c;
            }
        }
    }
    be_tex_write(s->be_tex, buf);
    free(buf);
}

/* ================= DirectDraw 側からの呼び出し ================= */
void d3d6_cpu_access(Surf *s)
{
    if (s && s->gpu_newer && s->be_rt) {
        GdiFlush();
        be_rt_read(s->be_rt, s->bits, s->pitch, s->bpp, s->rmask, s->gmask, s->bmask);
        s->gpu_newer = 0;
    }
}
void d3d6_cpu_modified(Surf *s) { if (s) { s->cpu_dirty = 1; } }
void d3d6_flip(Surf *front, Surf *back)
{
    (void)front;
    if (back && back->be_rt) {
        be_scene(0);
        d3d6_cpu_access(back);
        back->cpu_dirty = 0;   /* 交換後の DIB は前のフロントの内容: 描画先は直前の絵を持ったままにする */
    }
}
void d3d6_surface_freed(Surf *s)
{
    int i, k;
    for (i = 0; i < 1024; i++) if (g_texh[i] == s) g_texh[i] = NULL;
    for (k = 0; k < 8; k++) if (g_devs[k]) {
        Dev *d = g_devs[k];
        for (i = 0; i < 8; i++) if (d->tex[i] == s) { d->tex[i] = NULL; be_settex(i, NULL); }
        if (d->target == s) d->target = NULL;
    }
    if (s->be_rt) { be_rt_free(s->be_rt); s->be_rt = NULL; }
    if (s->be_tex) { be_tex_free(s->be_tex); s->be_tex = NULL; }
}
void d3d6_ddraw_freed(DDraw *dd)
{
    if (dd->d3d) { ((D3DObj *)dd->d3d)->dd = NULL; free(dd->d3d); dd->d3d = NULL; }
}

/* ================= 性能情報 ================= */
static void fill_prim(D3DPRIMCAPS *p)
{
    memset(p, 0, sizeof *p);
    p->dwSize = sizeof *p;
    p->dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    p->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG;
    p->dwZCmpCaps = p->dwAlphaCmpCaps = 0xFF;
    p->dwSrcBlendCaps = p->dwDestBlendCaps = 0x1FFF;
    p->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_BORDER;
    p->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR |
                             D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR | D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                             D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    p->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    p->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
}
static void fill_desc(D3DDEVICEDESC *d, int hw)
{
    DWORD sz = d->dwSize ? d->dwSize : sizeof *d;
    D3DDEVICEDESC t;
    memset(&t, 0, sizeof t);
    t.dwSize = sizeof t;
    t.dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_BCLIPPING | D3DDD_LIGHTINGCAPS | D3DDD_LINECAPS | D3DDD_TRICAPS |
                D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    t.dcmColorModel = D3DCOLOR_RGB;
    t.dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                  D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
                  D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX |
                  (hw ? D3DDEVCAPS_HWRASTERIZATION : 0);
    t.dtcTransformCaps.dwSize = sizeof t.dtcTransformCaps; t.dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    t.bClipping = TRUE;
    t.dlcLightingCaps.dwSize = sizeof t.dlcLightingCaps;
    t.dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
    t.dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB; t.dlcLightingCaps.dwNumLights = 8;
    fill_prim(&t.dpcLineCaps); fill_prim(&t.dpcTriCaps);
    t.dwDeviceRenderBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    t.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    t.dwMaxBufferSize = 0; t.dwMaxVertexCount = 65535;
    t.dwMinTextureWidth = t.dwMinTextureHeight = 1;
    t.dwMaxTextureWidth = t.dwMaxTextureHeight = 2048;
    t.dwMaxTextureRepeat = 2048; t.dwMaxTextureAspectRatio = 2048; t.dwMaxAnisotropy = 1;
    t.dvGuardBandLeft = -2048; t.dvGuardBandTop = -2048; t.dvGuardBandRight = 2047; t.dvGuardBandBottom = 2047;
    t.dwStencilCaps = 0;
    t.dwFVFCaps = 8;
    t.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X |
                        D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
                        D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
                        D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA;
    t.wMaxTextureBlendStages = 8; t.wMaxSimultaneousTextures = 2;
    if (sz > sizeof t) sz = sizeof t;
    memcpy(d, &t, sz);
    d->dwSize = sz;
}

/* ================= IDirect3D3 ================= */
static HRESULT WINAPI d3d_QueryInterface(void *This, REFIID iid, void **out)
{
    D3DObj *o = SELF(D3DObj, This);
    if (IsEqualGUID(iid, &IID_IDirect3D3) || IsEqualGUID(iid, &IID_IUnknown)) { *out = This; ((ULONG (WINAPI *)(void *))o->dd->i[0].vtbl[1])(&o->dd->i[0]); return S_OK; }
    return ((HRESULT (WINAPI *)(void *, REFIID, void **))o->dd->i[0].vtbl[0])(&o->dd->i[0], iid, out);
}
static ULONG WINAPI d3d_AddRef(void *This) { D3DObj *o = SELF(D3DObj, This); return ((ULONG (WINAPI *)(void *))o->dd->i[0].vtbl[1])(&o->dd->i[0]); }
static ULONG WINAPI d3d_Release(void *This) { D3DObj *o = SELF(D3DObj, This); return ((ULONG (WINAPI *)(void *))o->dd->i[0].vtbl[2])(&o->dd->i[0]); }
static HRESULT WINAPI d3d_EnumDevices(void *This, LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    D3DDEVICEDESC hal, hel; GUID g;
    (void)This;
    if (!cb) return DDERR_INVALIDPARAMS;
    memset(&hal, 0, sizeof hal); memset(&hel, 0, sizeof hel);
    hal.dwSize = hel.dwSize = sizeof hal;
    fill_desc(&hel, 0);
    hal.dwSize = sizeof hal; hal.dcmColorModel = 0;
    g = IID_IDirect3DRGBDevice;
    if (cb(&g, (char *)"MKWrap RGB Emulation", (char *)"RGB Emulation", &hal, &hel, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
    memset(&hal, 0, sizeof hal); hal.dwSize = sizeof hal; fill_desc(&hal, 1);
    memset(&hel, 0, sizeof hel); hel.dwSize = sizeof hel; fill_desc(&hel, 1);
    g = IID_IDirect3DHALDevice;
    cb(&g, (char *)"MKWrap Direct3D HAL (Direct3D9)", (char *)"Direct3D HAL", &hal, &hel, ctx);
    return D3D_OK;
}
static HRESULT WINAPI d3d_CreateLight(void *This, void **out, IUnknown *o)
{
    Light *l = (Light *)calloc(1, sizeof *l);
    (void)This; (void)o;
    l->i.vtbl = vt_light; l->i.self = l; l->ref = 1; l->l.dwSize = sizeof l->l;
    *out = &l->i; return D3D_OK;
}
static HRESULT WINAPI d3d_CreateMaterial(void *This, void **out, IUnknown *o)
{
    Mat *m = (Mat *)calloc(1, sizeof *m);
    (void)This; (void)o;
    m->i.vtbl = vt_mat; m->i.self = m; m->ref = 1; m->m.dwSize = sizeof m->m;
    *out = &m->i; return D3D_OK;
}
static HRESULT WINAPI d3d_CreateViewport(void *This, void **out, IUnknown *o)
{
    Vp *v = (Vp *)calloc(1, sizeof *v);
    (void)This; (void)o;
    v->i.vtbl = vt_vp; v->i.self = v; v->ref = 1;
    *out = &v->i; return D3D_OK;
}
static HRESULT WINAPI d3d_FindDevice(void *This, D3DFINDDEVICESEARCH *s, D3DFINDDEVICERESULT *r)
{
    (void)This; (void)s;
    if (!r) return DDERR_INVALIDPARAMS;
    r->guid = IID_IDirect3DHALDevice;
    r->ddHwDesc.dwSize = sizeof r->ddHwDesc; fill_desc(&r->ddHwDesc, 1);
    r->ddSwDesc.dwSize = sizeof r->ddSwDesc; fill_desc(&r->ddSwDesc, 1);
    return D3D_OK;
}
static void dev_set_rs(Dev *d, DWORD st, DWORD v);
static void dev_set_tss(Dev *d, DWORD stage, DWORD t, DWORD v);
static HRESULT WINAPI d3d_CreateDevice(void *This, REFCLSID cls, void *surf, void **out, IUnknown *o)
{
    D3DObj *ob = SELF(D3DObj, This);
    Surf *s = surf ? SELF(Surf, surf) : NULL;
    Dev *d; int k, i;
    (void)cls; (void)o;
    if (!s || !out) return DDERR_INVALIDPARAMS;
    be_set_window(ob->dd ? ob->dd->hwnd : NULL);
    if (!be_init()) return DDERR_GENERIC;
    if (!s->be_rt) s->be_rt = be_rt_new(s->w, s->h);
    if (!s->be_rt) return DDERR_OUTOFMEMORY;
    d = (Dev *)calloc(1, sizeof *d);
    d->i.vtbl = vt_dev; d->i.self = d; d->ref = 1; d->dd = ob->dd; d->target = s;
    s->ref++;
    for (k = 0; k < 8; k++) if (!g_devs[k]) { g_devs[k] = d; break; }
    /* 既定値 (DirectX 6) */
    d->rs[D3DRENDERSTATE_ZENABLE] = s->zatt ? D3DZB_TRUE : D3DZB_FALSE;
    d->rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID; d->rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    d->rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE; d->rs[D3DRENDERSTATE_LASTPIXEL] = TRUE;
    d->rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE; d->rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    d->rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW; d->rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    d->rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS; d->rs[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    d->rs[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_NEAREST; d->rs[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_NEAREST;
    d->rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE; d->rs[D3DRENDERSTATE_SPECULARENABLE] = TRUE;
    d->rs[D3DRENDERSTATE_FOGEND] = 0x3F800000; d->rs[D3DRENDERSTATE_FOGDENSITY] = 0x3F800000;
    d->rs[D3DRENDERSTATE_STENCILMASK] = d->rs[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFF;
    d->rs[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
    d->rs[D3DRENDERSTATE_STENCILFAIL] = d->rs[D3DRENDERSTATE_STENCILZFAIL] = d->rs[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
    d->rs[D3DRENDERSTATE_TEXTUREADDRESS] = d->rs[D3DRENDERSTATE_TEXTUREADDRESSU] = d->rs[D3DRENDERSTATE_TEXTUREADDRESSV] = D3DTADDRESS_WRAP;
    d->rs[D3DRENDERSTATE_DITHERENABLE] = FALSE;
    for (i = 0; i < 8; i++) {
        d->tss[i][D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE; d->tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE; d->tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        d->tss[i][D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1; d->tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; d->tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        d->tss[i][D3DTSS_TEXCOORDINDEX] = i;
        d->tss[i][D3DTSS_ADDRESS] = d->tss[i][D3DTSS_ADDRESSU] = d->tss[i][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        d->tss[i][D3DTSS_MAGFILTER] = D3DTFG_POINT; d->tss[i][D3DTSS_MINFILTER] = D3DTFN_POINT; d->tss[i][D3DTSS_MIPFILTER] = D3DTFP_NONE;
    }
    for (i = 0; i < 4; i++) { int j; for (j = 0; j < 16; j++) d->tf[i][j] = (j % 5 == 0) ? 1.0f : 0.0f; }
    be_target(s->be_rt);
    be_viewport(0, 0, s->w, s->h, 0, 1);
    for (i = 1; i < 256; i++) if (d->rs[i]) dev_set_rs(d, i, d->rs[i]);
    be_rs(D3DRENDERSTATE_SPECULARENABLE, TRUE);
    mkw_log("D3D6: CreateDevice %s %dx%d %dbpp Z=%s\n", IsEqualGUID(cls, &IID_IDirect3DHALDevice) ? "HAL" : IsEqualGUID(cls, &IID_IDirect3DRGBDevice) ? "RGB" : "?", s->w, s->h, s->bpp, s->zatt ? "あり" : "なし");
    *out = &d->i;
    return D3D_OK;
}
static HRESULT WINAPI d3d_CreateVertexBuffer(void *This, D3DVERTEXBUFFERDESC *desc, void **out, DWORD fl, IUnknown *o)
{
    VB *v;
    (void)This; (void)fl; (void)o;
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    v = (VB *)calloc(1, sizeof *v);
    v->i.vtbl = vt_vb; v->i.self = v; v->ref = 1; v->d = *desc; v->d.dwSize = sizeof v->d;
    v->stride = fvf_stride(desc->dwFVF);
    v->data = (BYTE *)calloc(desc->dwNumVertices ? desc->dwNumVertices : 1, v->stride ? v->stride : 4);
    *out = &v->i; return D3D_OK;
}
static HRESULT WINAPI d3d_EnumZBufferFormats(void *This, REFCLSID cls, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    static const DWORD depth[3] = { 16, 24, 32 }, mask[3] = { 0xFFFF, 0xFFFFFF, 0xFFFFFFFF };
    int i; (void)This; (void)cls;
    if (!cb) return DDERR_INVALIDPARAMS;
    mkw_log("D3D6: EnumZBufferFormats\n");
    for (i = 0; i < 3; i++) {
        DDPIXELFORMAT pf; memset(&pf, 0, sizeof pf);
        pf.dwSize = sizeof pf; pf.dwFlags = DDPF_ZBUFFER; pf.dwZBufferBitDepth = depth[i]; pf.dwZBitMask = mask[i];
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}
static HRESULT WINAPI d3d_EvictManagedTextures(void *This) { (void)This; return D3D_OK; }
static void *d3d_vt[] = { d3d_QueryInterface, d3d_AddRef, d3d_Release, d3d_EnumDevices, d3d_CreateLight, d3d_CreateMaterial,
                          d3d_CreateViewport, d3d_FindDevice, d3d_CreateDevice, d3d_CreateVertexBuffer, d3d_EnumZBufferFormats, d3d_EvictManagedTextures };

/* ================= IDirect3DMaterial3 ================= */
static HRESULT WINAPI mat_QueryInterface(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DMaterial3) || IsEqualGUID(iid, &IID_IUnknown)) { SELF(Mat, This)->ref++; *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI mat_AddRef(void *This) { return ++SELF(Mat, This)->ref; }
static ULONG WINAPI mat_Release(void *This)
{
    Mat *m = SELF(Mat, This);
    if (--m->ref > 0) return m->ref;
    if (m->h && m->h < 256) g_mats[m->h] = NULL;
    free(m); return 0;
}
static HRESULT WINAPI mat_SetMaterial(void *This, D3DMATERIAL *m)
{
    Mat *o = SELF(Mat, This);
    int k;
    if (!m) return DDERR_INVALIDPARAMS;
    o->m = *m; o->m.dwSize = sizeof o->m;
    /* 使用中の材質なら反映 */
    for (k = 0; k < 8; k++) if (g_devs[k] && o->h && g_devs[k]->ls[D3DLIGHTSTATE_MATERIAL] == o->h) be_material(&o->m.diffuse.r);
    return D3D_OK;
}
static HRESULT WINAPI mat_GetMaterial(void *This, D3DMATERIAL *m)
{
    Mat *o = SELF(Mat, This);
    DWORD sz;
    if (!m) return DDERR_INVALIDPARAMS;
    sz = m->dwSize && m->dwSize < sizeof *m ? m->dwSize : sizeof *m;
    memcpy(m, &o->m, sz); m->dwSize = sz;
    return D3D_OK;
}
static HRESULT WINAPI mat_GetHandle(void *This, void *dev, D3DMATERIALHANDLE *h)
{
    Mat *o = SELF(Mat, This);
    (void)dev;
    if (!o->h) { DWORD i; for (i = 1; i < 256; i++) if (!g_mats[i]) { g_mats[i] = o; o->h = i; break; } }
    *h = o->h;
    return o->h ? D3D_OK : DDERR_OUTOFMEMORY;
}
static void *mat_vt[] = { mat_QueryInterface, mat_AddRef, mat_Release, mat_SetMaterial, mat_GetMaterial, mat_GetHandle };
static Mat *mat_from(DWORD h) { return h && h < 256 ? g_mats[h] : NULL; }

/* ================= IDirect3DLight ================= */
static HRESULT WINAPI light_QueryInterface(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DLight) || IsEqualGUID(iid, &IID_IUnknown)) { SELF(Light, This)->ref++; *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI light_AddRef(void *This) { return ++SELF(Light, This)->ref; }
static ULONG WINAPI light_Release(void *This) { Light *l = SELF(Light, This); if (--l->ref > 0) return l->ref; free(l); return 0; }
static HRESULT WINAPI light_Initialize(void *This, void *d3d) { (void)This; (void)d3d; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI light_SetLight(void *This, D3DLIGHT2 *l)
{
    Light *o = SELF(Light, This);
    DWORD sz;
    if (!l) return DDERR_INVALIDPARAMS;
    sz = l->dwSize && l->dwSize < sizeof *l ? l->dwSize : sizeof *l;
    memset(&o->l, 0, sizeof o->l);
    memcpy(&o->l, l, sz);
    if (sz < sizeof *l) o->l.dwFlags = D3DLIGHT_ACTIVE;   /* D3DLIGHT (旧形式) には dwFlags がない */
    if (o->vp && o->vp->dev) o->vp->dev->lights_dirty = 1;
    return D3D_OK;
}
static HRESULT WINAPI light_GetLight(void *This, D3DLIGHT2 *l)
{
    Light *o = SELF(Light, This);
    DWORD sz;
    if (!l) return DDERR_INVALIDPARAMS;
    sz = l->dwSize && l->dwSize < sizeof *l ? l->dwSize : sizeof *l;
    memcpy(l, &o->l, sz); l->dwSize = sz;
    return D3D_OK;
}
static void *light_vt[] = { light_QueryInterface, light_AddRef, light_Release, light_Initialize, light_SetLight, light_GetLight };

/* ================= IDirect3DViewport3 ================= */
static void vp_apply(Vp *v)
{
    float mn, mx;
    if (!v || !v->dev || !v->has) return;
    mn = v->v.dvMinZ; mx = v->v.dvMaxZ;
    if (!(mx > mn) || mn < 0 || mx > 1) { mn = 0; mx = 1; }
    be_viewport(v->v.dwX, v->v.dwY, v->v.dwWidth, v->v.dwHeight, mn, mx);
}
static HRESULT WINAPI vp_QueryInterface(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DViewport3) || IsEqualGUID(iid, &IID_IDirect3DViewport2) || IsEqualGUID(iid, &IID_IDirect3DViewport) ||
        IsEqualGUID(iid, &IID_IUnknown)) { SELF(Vp, This)->ref++; *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI vp_AddRef(void *This) { return ++SELF(Vp, This)->ref; }
static ULONG WINAPI vp_Release(void *This)
{
    Vp *v = SELF(Vp, This);
    int i;
    if (--v->ref > 0) return v->ref;
    for (i = 0; i < v->nl; i++) { v->lt[i]->vp = NULL; light_Release(&v->lt[i]->i); }
    free(v); return 0;
}
static HRESULT WINAPI vp_Initialize(void *This, void *d3d) { (void)This; (void)d3d; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI vp_GetViewport(void *This, D3DVIEWPORT *p)
{
    Vp *v = SELF(Vp, This);
    if (!p) return DDERR_INVALIDPARAMS;
    if (!v->has) return D3DERR_VIEWPORTDATANOTSET;
    memset(p, 0, sizeof *p); p->dwSize = sizeof *p;
    p->dwX = v->v.dwX; p->dwY = v->v.dwY; p->dwWidth = v->v.dwWidth; p->dwHeight = v->v.dwHeight;
    p->dvScaleX = v->v.dwWidth / 2.0f; p->dvScaleY = v->v.dwHeight / 2.0f; p->dvMaxX = 1; p->dvMaxY = 1;
    p->dvMinZ = v->v.dvMinZ; p->dvMaxZ = v->v.dvMaxZ;
    return D3D_OK;
}
static HRESULT WINAPI vp_SetViewport(void *This, D3DVIEWPORT *p)
{
    Vp *v = SELF(Vp, This);
    if (!p) return DDERR_INVALIDPARAMS;
    v->v.dwSize = sizeof v->v; v->v.dwX = p->dwX; v->v.dwY = p->dwY; v->v.dwWidth = p->dwWidth; v->v.dwHeight = p->dwHeight;
    v->v.dvClipX = -1; v->v.dvClipY = 1; v->v.dvClipWidth = 2; v->v.dvClipHeight = 2;
    v->v.dvMinZ = p->dvMinZ; v->v.dvMaxZ = p->dvMaxZ; v->has = 1;
    if (v->dev && v->dev->cur == v) vp_apply(v);
    return D3D_OK;
}
static HRESULT WINAPI vp_TransformVertices(void *This, DWORD n, void *d, DWORD fl, DWORD *off) { (void)This; (void)n; (void)d; (void)fl; if (off) *off = 0; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI vp_LightElements(void *This, DWORD n, void *d) { (void)This; (void)n; (void)d; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI vp_SetBackground(void *This, D3DMATERIALHANDLE h) { SELF(Vp, This)->bg = h; return D3D_OK; }
static HRESULT WINAPI vp_GetBackground(void *This, D3DMATERIALHANDLE *h, BOOL *valid)
{
    Vp *v = SELF(Vp, This);
    if (h) *h = v->bg;
    if (valid) *valid = v->bg != 0;
    return D3D_OK;
}
static HRESULT WINAPI vp_SetBackgroundDepth(void *This, void *s) { (void)This; (void)s; return D3D_OK; }
static HRESULT WINAPI vp_GetBackgroundDepth(void *This, void **s, BOOL *valid) { (void)This; if (s) *s = NULL; if (valid) *valid = FALSE; return D3D_OK; }
static int dev_prepare_target(Dev *d);
static HRESULT vp_clear_common(Vp *v, DWORD n, D3DRECT *r, DWORD fl, DWORD color, float z, DWORD st)
{
    Dev *d = v->dev;
    if (!d || !d->target) return D3DERR_VIEWPORTHASNODEVICE;
    if (!dev_prepare_target(d)) return D3D_OK;
    if (v != d->cur) vp_apply(v);
    be_clear(n, (const LONG *)r, fl, color, z, st);
    if (v != d->cur && d->cur) vp_apply(d->cur);
    d->target->gpu_newer = 1;
    return D3D_OK;
}
static HRESULT WINAPI vp_Clear(void *This, DWORD n, D3DRECT *r, DWORD fl)
{
    Vp *v = SELF(Vp, This);
    Mat *m = mat_from(v->bg);
    DWORD c = m ? col_from_value(&m->m.diffuse) : 0;
    return vp_clear_common(v, n, r, fl, c, 1.0f, 0);
}
static HRESULT WINAPI vp_AddLight(void *This, void *li)
{
    Vp *v = SELF(Vp, This);
    Light *l = li ? SELF(Light, li) : NULL;
    if (!l) return DDERR_INVALIDPARAMS;
    if (v->nl >= 8) return D3DERR_LIGHT_SET_FAILED;
    l->ref++; l->vp = v; v->lt[v->nl++] = l;
    if (v->dev) v->dev->lights_dirty = 1;
    return D3D_OK;
}
static HRESULT WINAPI vp_DeleteLight(void *This, void *li)
{
    Vp *v = SELF(Vp, This);
    Light *l = li ? SELF(Light, li) : NULL;
    int i;
    for (i = 0; i < v->nl; i++) if (v->lt[i] == l) {
        memmove(&v->lt[i], &v->lt[i + 1], sizeof(Light *) * (v->nl - i - 1)); v->nl--;
        l->vp = NULL; light_Release(&l->i);
        if (v->dev) v->dev->lights_dirty = 1;
        return D3D_OK;
    }
    return DDERR_INVALIDPARAMS;
}
static HRESULT WINAPI vp_NextLight(void *This, void *ref, void **out, DWORD fl)
{
    Vp *v = SELF(Vp, This);
    int i = -1, k;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (fl == D3DNEXT_HEAD) i = 0;
    else if (fl == D3DNEXT_TAIL) i = v->nl - 1;
    else for (k = 0; k < v->nl; k++) if (&v->lt[k]->i == ref) i = k + 1;
    if (i < 0 || i >= v->nl) return D3DERR_LIGHT_SET_FAILED;
    v->lt[i]->ref++; *out = &v->lt[i]->i; return D3D_OK;
}
static HRESULT WINAPI vp_GetViewport2(void *This, D3DVIEWPORT2 *p)
{
    Vp *v = SELF(Vp, This);
    if (!p) return DDERR_INVALIDPARAMS;
    if (!v->has) return D3DERR_VIEWPORTDATANOTSET;
    *p = v->v; return D3D_OK;
}
static HRESULT WINAPI vp_SetViewport2(void *This, D3DVIEWPORT2 *p)
{
    Vp *v = SELF(Vp, This);
    if (!p) return DDERR_INVALIDPARAMS;
    v->v = *p; v->v.dwSize = sizeof v->v; v->has = 1;
    if (v->dev && v->dev->cur == v) vp_apply(v);
    return D3D_OK;
}
static HRESULT WINAPI vp_SetBackgroundDepth2(void *This, void *s) { (void)This; (void)s; return D3D_OK; }
static HRESULT WINAPI vp_GetBackgroundDepth2(void *This, void **s, BOOL *valid) { (void)This; if (s) *s = NULL; if (valid) *valid = FALSE; return D3D_OK; }
static HRESULT WINAPI vp_Clear2(void *This, DWORD n, D3DRECT *r, DWORD fl, DWORD color, D3DVALUE z, DWORD st)
{
    return vp_clear_common(SELF(Vp, This), n, r, fl, color, z, st);
}
static void *vp_vt[] = { vp_QueryInterface, vp_AddRef, vp_Release, vp_Initialize, vp_GetViewport, vp_SetViewport, vp_TransformVertices,
                         vp_LightElements, vp_SetBackground, vp_GetBackground, vp_SetBackgroundDepth, vp_GetBackgroundDepth, vp_Clear,
                         vp_AddLight, vp_DeleteLight, vp_NextLight, vp_GetViewport2, vp_SetViewport2, vp_SetBackgroundDepth2,
                         vp_GetBackgroundDepth2, vp_Clear2 };

/* ================= IDirect3DTexture / IDirect3DTexture2 (サーフェスの別の顔) ================= */
#define TEXS(p) SELF(Surf, p)
static HRESULT WINAPI tex_QueryInterface(void *This, REFIID iid, void **out)
{
    Surf *s = TEXS(This);
    return ((HRESULT (WINAPI *)(void *, REFIID, void **))s->i[0].vtbl[0])(&s->i[0], iid, out);
}
static ULONG WINAPI tex_AddRef(void *This) { return ++TEXS(This)->ref; }
static ULONG WINAPI tex_Release(void *This) { Surf *s = TEXS(This); return ((ULONG (WINAPI *)(void *))s->i[0].vtbl[2])(&s->i[0]); }
static HRESULT WINAPI tex_Initialize(void *This, void *a, void *b) { (void)This; (void)a; (void)b; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI tex_GetHandle(void *This, void *dev, D3DTEXTUREHANDLE *h)
{
    Surf *s = TEXS(This);
    DWORD i;
    (void)dev;
    for (i = 1; i < 1024; i++) if (g_texh[i] == s) { *h = i; return D3D_OK; }
    for (i = 1; i < 1024; i++) if (!g_texh[i]) { g_texh[i] = s; *h = i; return D3D_OK; }
    return DDERR_OUTOFMEMORY;
}
static HRESULT WINAPI tex_PaletteChanged(void *This, DWORD a, DWORD b) { (void)a; (void)b; TEXS(This)->cpu_dirty = 1; return D3D_OK; }
static HRESULT WINAPI tex_Load(void *This, void *srcif)
{
    Surf *d = TEXS(This), *s = srcif ? TEXS(srcif) : NULL;
    int y;
    if (!s) return DDERR_INVALIDPARAMS;
    if (s == d) return D3D_OK;
    d3d6_cpu_access(s);
    if (!d->bits || !s->bits) return DDERR_GENERIC;
    GdiFlush();
    if (s->bpp == d->bpp && s->w == d->w && s->h == d->h) {
        for (y = 0; y < d->h; y++) memcpy(d->bits + (size_t)y * d->pitch, s->bits + (size_t)y * s->pitch, (size_t)d->w * d->bpp / 8);
    } else if (s->bpp == d->bpp) {
        int x, b = d->bpp / 8;
        for (y = 0; y < d->h; y++) for (x = 0; x < d->w; x++)
            memcpy(d->bits + (size_t)y * d->pitch + (size_t)x * b, s->bits + (size_t)(y * s->h / d->h) * s->pitch + (size_t)(x * s->w / d->w) * b, b);
    } else return DDERR_INVALIDPIXELFORMAT;
    if (s->ckflags & DDCKEY_SRCBLT) { d->ck_src = s->ck_src; d->ckflags |= DDCKEY_SRCBLT; }
    if (s->pal && s->pal != d->pal) { if (d->pal) d->pal->ref--; d->pal = s->pal; s->pal->ref++; }
    d->cpu_dirty = 1; d->uniq++;
    return D3D_OK;
}
static HRESULT WINAPI tex_Unload(void *This) { (void)This; return D3D_OK; }
static void *tex1_vt[] = { tex_QueryInterface, tex_AddRef, tex_Release, tex_Initialize, tex_GetHandle, tex_PaletteChanged, tex_Load, tex_Unload };
static void *tex2_vt[] = { tex_QueryInterface, tex_AddRef, tex_Release, tex_GetHandle, tex_PaletteChanged, tex_Load };

void d3d6_init_surface(Surf *s)
{
    s->tex[0].vtbl = tex1_vt; s->tex[0].self = s; s->tex[0].ver = 1;
    s->tex[1].vtbl = tex2_vt; s->tex[1].self = s; s->tex[1].ver = 2;
}
HRESULT d3d6_query_texture(Surf *s, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DTexture)) { s->ref++; *out = &s->tex[0]; return S_OK; }
    if (IsEqualGUID(iid, &IID_IDirect3DTexture2)) { s->ref++; *out = &s->tex[1]; return S_OK; }
    return E_NOINTERFACE;
}

/* ================= IDirect3DVertexBuffer ================= */
static HRESULT WINAPI vb_QueryInterface(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DVertexBuffer) || IsEqualGUID(iid, &IID_IUnknown)) { SELF(VB, This)->ref++; *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI vb_AddRef(void *This) { return ++SELF(VB, This)->ref; }
static ULONG WINAPI vb_Release(void *This) { VB *v = SELF(VB, This); if (--v->ref > 0) return v->ref; free(v->data); free(v); return 0; }
static HRESULT WINAPI vb_Lock(void *This, DWORD fl, void **p, DWORD *size)
{
    VB *v = SELF(VB, This); (void)fl;
    if (!p) return DDERR_INVALIDPARAMS;
    *p = v->data; if (size) *size = v->stride * v->d.dwNumVertices;
    return D3D_OK;
}
static HRESULT WINAPI vb_Unlock(void *This) { (void)This; return D3D_OK; }
static HRESULT WINAPI vb_ProcessVertices(void *This, DWORD op, DWORD di, DWORD n, void *src, DWORD si, void *dev, DWORD fl)
{ (void)This; (void)op; (void)di; (void)n; (void)src; (void)si; (void)dev; (void)fl; mkw_log("D3D6: ProcessVertices 未対応\n"); return DDERR_UNSUPPORTED; }
static HRESULT WINAPI vb_GetVertexBufferDesc(void *This, D3DVERTEXBUFFERDESC *d) { if (!d) return DDERR_INVALIDPARAMS; *d = SELF(VB, This)->d; return D3D_OK; }
static HRESULT WINAPI vb_Optimize(void *This, void *dev, DWORD fl) { (void)This; (void)dev; (void)fl; return D3D_OK; }
static void *vb_vt[] = { vb_QueryInterface, vb_AddRef, vb_Release, vb_Lock, vb_Unlock, vb_ProcessVertices, vb_GetVertexBufferDesc, vb_Optimize };

/* ================= IDirect3DDevice3 ================= */
static void dev_reapply(Dev *d);
static int g_lost;
/* フレームの始めにデバイスの消失を調べる */
static void dev_check_lost(void)
{
    int c = be_check(), k, i;
    g_lost = c < 0;
    if (c > 0) {
        for (k = 0; k < 8; k++) if (g_devs[k]) {
            dev_reapply(g_devs[k]);
            if (g_devs[k]->target) g_devs[k]->target->cpu_dirty = 1;   /* 作り直した描画先は空なので DIB の内容を戻す */
        }
        (void)i;
    }
}
static int dev_prepare_target(Dev *d)
{
    Surf *t = d->target;
    if (!t || !t->be_rt || g_lost) return 0;
    be_target(t->be_rt);
    if (t->cpu_dirty) {
        GdiFlush();
        be_rt_write(t->be_rt, t->bits, t->pitch, t->bpp, t->rmask, t->gmask, t->bmask);
        t->cpu_dirty = 0;
    }
    return 1;
}
static void apply_stage0(Dev *d)
{
    int i;
    for (i = D3DTSS_COLOROP; i <= D3DTSS_ALPHAARG2; i++) be_tss(0, i, d->tss[0][i]);
}
static void dev_set_tss(Dev *d, DWORD stage, DWORD t, DWORD v)
{
    if (stage >= 8 || t >= 32) return;
    d->tss[stage][t] = v;
    switch (t) {
    case D3DTSS_ADDRESS: be_samp(stage, BE_SAMP_ADDRESSU, v); be_samp(stage, BE_SAMP_ADDRESSV, v); break;
    case D3DTSS_ADDRESSU: be_samp(stage, BE_SAMP_ADDRESSU, v); break;
    case D3DTSS_ADDRESSV: be_samp(stage, BE_SAMP_ADDRESSV, v); break;
    case D3DTSS_BORDERCOLOR: be_samp(stage, BE_SAMP_BORDERCOLOR, v); break;
    case D3DTSS_MAGFILTER: be_samp(stage, BE_SAMP_MAGFILTER, v == D3DTFG_POINT ? 1 : v == D3DTFG_ANISOTROPIC ? 3 : 2); break;
    case D3DTSS_MINFILTER: be_samp(stage, BE_SAMP_MINFILTER, v == D3DTFN_POINT ? 1 : v == D3DTFN_ANISOTROPIC ? 3 : 2); break;
    case D3DTSS_MIPFILTER: be_samp(stage, BE_SAMP_MIPFILTER, v ? v - 1 : 0); break;
    case D3DTSS_MIPMAPLODBIAS: be_samp(stage, BE_SAMP_MIPMAPLODBIAS, v); break;
    case D3DTSS_MAXANISOTROPY: be_samp(stage, BE_SAMP_MAXANISOTROPY, v); break;
    case D3DTSS_MAXMIPLEVEL: break;
    default:
        if (stage == 0 && d->null_ovr && t >= D3DTSS_COLOROP && t <= D3DTSS_ALPHAARG2) break;  /* 描画時に反映 */
        be_tss(stage, t, v);
    }
}
static void dev_set_rs(Dev *d, DWORD st, DWORD v)
{
    if (st < 256) d->rs[st] = v;
    switch (st) {
    case D3DRENDERSTATE_TEXTUREHANDLE: d->tex[0] = v && v < 1024 ? g_texh[v] : NULL; break;
    case D3DRENDERSTATE_TEXTUREADDRESS: dev_set_tss(d, 0, D3DTSS_ADDRESS, v); break;
    case D3DRENDERSTATE_TEXTUREADDRESSU: dev_set_tss(d, 0, D3DTSS_ADDRESSU, v); break;
    case D3DRENDERSTATE_TEXTUREADDRESSV: dev_set_tss(d, 0, D3DTSS_ADDRESSV, v); break;
    case D3DRENDERSTATE_TEXTUREMAG: be_samp(0, BE_SAMP_MAGFILTER, v == D3DFILTER_NEAREST ? 1 : 2); break;
    case D3DRENDERSTATE_TEXTUREMIN:
        be_samp(0, BE_SAMP_MINFILTER, (v == D3DFILTER_NEAREST || v == D3DFILTER_MIPNEAREST || v == D3DFILTER_LINEARMIPNEAREST) ? 1 : 2);
        be_samp(0, BE_SAMP_MIPFILTER, 0); break;
    case D3DRENDERSTATE_BORDERCOLOR: be_samp(0, BE_SAMP_BORDERCOLOR, v); break;
    case D3DRENDERSTATE_MIPMAPLODBIAS: be_samp(0, BE_SAMP_MIPMAPLODBIAS, v); break;
    case D3DRENDERSTATE_ANISOTROPY: be_samp(0, BE_SAMP_MAXANISOTROPY, v); break;
    case D3DRENDERSTATE_ZBIAS: { float f = -(float)v * 0.00002f; DWORD fv; memcpy(&fv, &f, 4); be_rs(BE_RS_DEPTHBIAS, fv); } break;
    case D3DRENDERSTATE_TEXTUREMAPBLEND:
        switch (v) {
        case D3DTBLEND_DECAL: case D3DTBLEND_COPY:
            dev_set_tss(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev_set_tss(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev_set_tss(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev_set_tss(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); break;
        case D3DTBLEND_MODULATE:
            dev_set_tss(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE); dev_set_tss(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev_set_tss(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev_set_tss(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); break;
        case D3DTBLEND_DECALALPHA:
            dev_set_tss(d, 0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA); dev_set_tss(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev_set_tss(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); dev_set_tss(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); break;
        case D3DTBLEND_MODULATEALPHA: case D3DTBLEND_MODULATEMASK:
            dev_set_tss(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE); dev_set_tss(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev_set_tss(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); dev_set_tss(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); break;
        case D3DTBLEND_ADD:
            dev_set_tss(d, 0, D3DTSS_COLOROP, D3DTOP_ADD); dev_set_tss(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev_set_tss(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev_set_tss(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); dev_set_tss(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); break;
        }
        break;
    case D3DRENDERSTATE_ZENABLE: case D3DRENDERSTATE_ALPHATESTENABLE: case D3DRENDERSTATE_ALPHAREF: case D3DRENDERSTATE_ALPHAFUNC:
    case D3DRENDERSTATE_COLORKEYENABLE:
        break;   /* 描画時に反映 */
    default:
        if (st == 8 || st == 9 || st == 14 || st == 16 || st == 19 || st == 20 || (st >= 22 && st <= 23) || (st >= 26 && st <= 29) ||
            (st >= 34 && st <= 38) || st == 48 || (st >= 52 && st <= 60) || (st >= 128 && st <= 160))
            be_rs(st, v);
    }
}
static void dev_apply_lights(Dev *d)
{
    int i, n = 0;
    Vp *v = d->cur;
    if (v) for (i = 0; i < v->nl && n < 8; i++) {
        Light *l = v->lt[i];
        be_light_t L;
        if (!(l->l.dwFlags & D3DLIGHT_ACTIVE)) continue;
        memset(&L, 0, sizeof L);
        L.type = l->l.dltType;
        L.color[0] = l->l.dcvColor.r; L.color[1] = l->l.dcvColor.g; L.color[2] = l->l.dcvColor.b; L.color[3] = l->l.dcvColor.a;
        L.pos[0] = l->l.dvPosition.x; L.pos[1] = l->l.dvPosition.y; L.pos[2] = l->l.dvPosition.z;
        L.dir[0] = l->l.dvDirection.x; L.dir[1] = l->l.dvDirection.y; L.dir[2] = l->l.dvDirection.z;
        L.range = l->l.dvRange; L.falloff = l->l.dvFalloff;
        L.att[0] = l->l.dvAttenuation0; L.att[1] = l->l.dvAttenuation1; L.att[2] = l->l.dvAttenuation2;
        L.theta = l->l.dvTheta; L.phi = l->l.dvPhi;
        be_light(n++, &L);
    }
    for (i = n; i < 8; i++) be_light(i, NULL);
    d->lights_dirty = 0;
}
/* デバイスを作り直した後: ステートを全部設定し直す */
static void dev_reapply(Dev *d)
{
    DWORD i, st;
    for (i = 1; i < 256; i++) if (i != D3DRENDERSTATE_TEXTUREMAPBLEND && i != D3DRENDERSTATE_TEXTUREHANDLE) dev_set_rs(d, i, d->rs[i]);
    for (st = 0; st < 8; st++) for (i = 1; i < 32; i++) dev_set_tss(d, st, i, d->tss[st][i]);
    be_transform(256, d->tf[0]); be_transform(2, d->tf[1]); be_transform(3, d->tf[2]);
    { Mat *m = mat_from(d->ls[D3DLIGHTSTATE_MATERIAL]); if (m) be_material(&m->m.diffuse.r); }
    be_rs(139, d->ls[D3DLIGHTSTATE_AMBIENT]);
    d->lights_dirty = 1; d->null_ovr = 0;
    if (d->cur) vp_apply(d->cur);
}
/* 描画の直前: 描画先・テクスチャ・描画時に決まるステートを反映 */
static int dev_prepare(Dev *d, DWORD fvf, DWORD flags)
{
    int hasz, i, ck = 0, lit;
    Surf *t0;
    if (!dev_prepare_target(d)) return 0;
    hasz = d->target->zatt != NULL;
    be_rs(D3DRENDERSTATE_ZENABLE, hasz ? d->rs[D3DRENDERSTATE_ZENABLE] : 0);
    for (i = 0; i < 8; i++) {
        Surf *s = d->tex[i];
        if (s) { d3d6_cpu_access(s); tex_sync(s); be_settex(i, s->be_tex); }
        else be_settex(i, NULL);
    }
    t0 = d->tex[0];
    if (!t0 && !d->null_ovr) {
        /* テクスチャなし: DirectX 6 ではテクスチャ引数が頂点色になる */
        be_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); be_tss(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        be_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); be_tss(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        d->null_ovr = 1;
    } else if (t0 && d->null_ovr) { d->null_ovr = 0; apply_stage0(d); }
    /* カラーキー透過: キーの色はアルファ 0 にしてあるので、アルファテストで抜く */
    if (d->rs[D3DRENDERSTATE_COLORKEYENABLE] && t0 && (t0->ckflags & DDCKEY_SRCBLT)) ck = 1;
    if (ck && !d->rs[D3DRENDERSTATE_ALPHATESTENABLE]) {
        be_rs(D3DRENDERSTATE_ALPHATESTENABLE, TRUE); be_rs(D3DRENDERSTATE_ALPHAREF, 0); be_rs(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER);
    } else {
        be_rs(D3DRENDERSTATE_ALPHATESTENABLE, d->rs[D3DRENDERSTATE_ALPHATESTENABLE]);
        be_rs(D3DRENDERSTATE_ALPHAREF, d->rs[D3DRENDERSTATE_ALPHAREF]);
        be_rs(D3DRENDERSTATE_ALPHAFUNC, d->rs[D3DRENDERSTATE_ALPHAFUNC] ? d->rs[D3DRENDERSTATE_ALPHAFUNC] : D3DCMP_ALWAYS);
    }
    lit = !(fvf & D3DFVF_XYZRHW) && (fvf & D3DFVF_NORMAL) && !(flags & D3DDP_DONOTLIGHT);
    be_rs(BE_RS_LIGHTING, lit);
    if (lit && d->lights_dirty) dev_apply_lights(d);
    return 1;
}
static void dev_draw(Dev *d, DWORD prim, DWORD fvf, const void *v, DWORD nv, const WORD *idx, DWORD ni, DWORD flags)
{
    static int n;
    if (!v || !nv) return;
    if (!dev_prepare(d, fvf, flags)) return;
    if (g_cfg.log && n < 3) { n++; mkw_log("D3D6: Draw prim=%lu fvf=%lx n=%lu idx=%lu\n", (unsigned long)prim, (unsigned long)fvf, (unsigned long)nv, (unsigned long)ni); }
    be_draw(prim, fvf & ~0xF000u, fvf_stride(fvf), v, nv, idx, ni);
    d->target->gpu_newer = 1;
}

static HRESULT WINAPI dev_QueryInterface(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirect3DDevice3) || IsEqualGUID(iid, &IID_IUnknown)) { SELF(Dev, This)->ref++; *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI dev_AddRef(void *This) { return ++SELF(Dev, This)->ref; }
static ULONG WINAPI dev_Release(void *This)
{
    Dev *d = SELF(Dev, This);
    int k;
    if (--d->ref > 0) return d->ref;
    be_scene(0);
    for (k = 0; k < 8; k++) if (g_devs[k] == d) g_devs[k] = NULL;
    for (k = 0; k < d->nvp; k++) { d->vp[k]->dev = NULL; vp_Release(&d->vp[k]->i); }
    if (d->target) { Surf *t = d->target; d->target = NULL; ((ULONG (WINAPI *)(void *))t->i[0].vtbl[2])(&t->i[0]); }
    free(d->bbuf); free(d->bidx);
    free(d);
    return 0;
}
static HRESULT WINAPI dev_GetCaps(void *This, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel)
{
    (void)This;
    if (hal) fill_desc(hal, 1);
    if (hel) fill_desc(hel, 1);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetStats(void *This, D3DSTATS *s) { (void)This; if (s) { DWORD sz = s->dwSize; memset(s, 0, sizeof *s); s->dwSize = sz; } return D3D_OK; }
static HRESULT WINAPI dev_AddViewport(void *This, void *vi)
{
    Dev *d = SELF(Dev, This);
    Vp *v = vi ? SELF(Vp, vi) : NULL;
    if (!v) return DDERR_INVALIDPARAMS;
    if (v->dev == d) return D3D_OK;
    if (d->nvp >= 16) return DDERR_OUTOFMEMORY;
    v->ref++; v->dev = d; d->vp[d->nvp++] = v;
    return D3D_OK;
}
static HRESULT WINAPI dev_DeleteViewport(void *This, void *vi)
{
    Dev *d = SELF(Dev, This);
    Vp *v = vi ? SELF(Vp, vi) : NULL;
    int i;
    for (i = 0; i < d->nvp; i++) if (d->vp[i] == v) {
        memmove(&d->vp[i], &d->vp[i + 1], sizeof(Vp *) * (d->nvp - i - 1)); d->nvp--;
        if (d->cur == v) { d->cur = NULL; vp_Release(&v->i); }
        v->dev = NULL; vp_Release(&v->i);
        return D3D_OK;
    }
    return DDERR_INVALIDPARAMS;
}
static HRESULT WINAPI dev_NextViewport(void *This, void *ref, void **out, DWORD fl)
{
    Dev *d = SELF(Dev, This);
    int i = -1, k;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (fl == D3DNEXT_HEAD) i = 0;
    else if (fl == D3DNEXT_TAIL) i = d->nvp - 1;
    else for (k = 0; k < d->nvp; k++) if (&d->vp[k]->i == ref) i = k + 1;
    if (i < 0 || i >= d->nvp) return DDERR_INVALIDPARAMS;
    d->vp[i]->ref++; *out = &d->vp[i]->i; return D3D_OK;
}
static HRESULT WINAPI dev_EnumTextureFormats(void *This, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    static const DWORD f[][5] = {
        { 16, 0xF800, 0x07E0, 0x001F, 0 }, { 16, 0x7C00, 0x03E0, 0x001F, 0 }, { 16, 0x7C00, 0x03E0, 0x001F, 0x8000 },
        { 16, 0x0F00, 0x00F0, 0x000F, 0xF000 }, { 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000 }, { 32, 0xFF0000, 0xFF00, 0xFF, 0 },
    };
    int i; (void)This;
    if (!cb) return DDERR_INVALIDPARAMS;
    for (i = 0; i < (int)(sizeof f / sizeof f[0]); i++) {
        DDPIXELFORMAT pf; memset(&pf, 0, sizeof pf);
        pf.dwSize = sizeof pf; pf.dwFlags = DDPF_RGB | (f[i][4] ? DDPF_ALPHAPIXELS : 0);
        pf.dwRGBBitCount = f[i][0]; pf.dwRBitMask = f[i][1]; pf.dwGBitMask = f[i][2]; pf.dwBBitMask = f[i][3]; pf.dwRGBAlphaBitMask = f[i][4];
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}
static HRESULT WINAPI dev_BeginScene(void *This) { (void)This; dev_check_lost(); if (!g_lost) be_scene(1); return D3D_OK; }
static HRESULT WINAPI dev_EndScene(void *This) { (void)This; be_scene(0); return D3D_OK; }
static HRESULT WINAPI dev_GetDirect3D(void *This, void **out)
{
    Dev *d = SELF(Dev, This);
    D3DObj *o = d->dd ? (D3DObj *)d->dd->d3d : NULL;
    if (!o) { *out = NULL; return DDERR_GENERIC; }
    d3d_AddRef(&o->i); *out = &o->i; return D3D_OK;
}
static HRESULT WINAPI dev_SetCurrentViewport(void *This, void *vi)
{
    Dev *d = SELF(Dev, This);
    Vp *v = vi ? SELF(Vp, vi) : NULL;
    if (v && v->dev != d) return DDERR_INVALIDPARAMS;
    if (v) v->ref++;
    if (d->cur) vp_Release(&d->cur->i);
    d->cur = v;
    d->lights_dirty = 1;
    if (v) { dev_prepare_target(d); vp_apply(v); }
    return D3D_OK;
}
static HRESULT WINAPI dev_GetCurrentViewport(void *This, void **out)
{
    Dev *d = SELF(Dev, This);
    if (!out) return DDERR_INVALIDPARAMS;
    if (!d->cur) { *out = NULL; return D3DERR_NOCURRENTVIEWPORT; }
    d->cur->ref++; *out = &d->cur->i; return D3D_OK;
}
static HRESULT WINAPI dev_SetRenderTarget(void *This, void *surf, DWORD fl)
{
    Dev *d = SELF(Dev, This);
    Surf *s = surf ? SELF(Surf, surf) : NULL;
    (void)fl;
    if (!s) return DDERR_INVALIDPARAMS;
    if (s == d->target) return D3D_OK;
    if (!s->be_rt) s->be_rt = be_rt_new(s->w, s->h);
    if (!s->be_rt) return DDERR_OUTOFMEMORY;
    s->ref++;
    if (d->target) { Surf *t = d->target; ((ULONG (WINAPI *)(void *))t->i[0].vtbl[2])(&t->i[0]); }
    d->target = s;
    dev_prepare_target(d);
    if (d->cur) vp_apply(d->cur);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderTarget(void *This, void **out)
{
    Dev *d = SELF(Dev, This);
    if (!out) return DDERR_INVALIDPARAMS;
    if (!d->target) { *out = NULL; return DDERR_GENERIC; }
    d->target->ref++; *out = mkw_s_iface(d->target, 4); return D3D_OK;
}
static HRESULT WINAPI dev_Begin(void *This, D3DPRIMITIVETYPE p, DWORD fvf, DWORD fl)
{
    Dev *d = SELF(Dev, This);
    d->bprim = p; d->bfvf = fvf; d->bflags = fl; d->bn = 0; d->bni = 0; d->bindexed = 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_BeginIndexed(void *This, D3DPRIMITIVETYPE p, DWORD fvf, void *v, DWORD nv, DWORD fl)
{
    Dev *d = SELF(Dev, This);
    DWORD st = fvf_stride(fvf);
    d->bprim = p; d->bfvf = fvf; d->bflags = fl; d->bni = 0; d->bindexed = 1;
    if (d->bcap < nv * st) { d->bcap = nv * st; d->bbuf = (BYTE *)realloc(d->bbuf, d->bcap); }
    if (v && nv) memcpy(d->bbuf, v, nv * st);
    d->bn = nv;
    return D3D_OK;
}
static HRESULT WINAPI dev_Vertex(void *This, void *v)
{
    Dev *d = SELF(Dev, This);
    DWORD st = fvf_stride(d->bfvf);
    if (!v) return DDERR_INVALIDPARAMS;
    if (d->bcap < (d->bn + 1) * st) { d->bcap = (d->bn + 64) * st; d->bbuf = (BYTE *)realloc(d->bbuf, d->bcap); }
    memcpy(d->bbuf + d->bn * st, v, st); d->bn++;
    return D3D_OK;
}
static HRESULT WINAPI dev_Index(void *This, WORD i)
{
    Dev *d = SELF(Dev, This);
    if (d->bicap < d->bni + 1) { d->bicap = d->bni + 256; d->bidx = (WORD *)realloc(d->bidx, d->bicap * 2); }
    d->bidx[d->bni++] = i;
    return D3D_OK;
}
static HRESULT WINAPI dev_End(void *This, DWORD fl)
{
    Dev *d = SELF(Dev, This);
    (void)fl;
    dev_draw(d, d->bprim, d->bfvf, d->bbuf, d->bn, d->bindexed ? d->bidx : NULL, d->bni, d->bflags);
    d->bn = d->bni = 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderState(void *This, D3DRENDERSTATETYPE st, DWORD *v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    *v = (DWORD)st < 256 ? SELF(Dev, This)->rs[st] : 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetRenderState(void *This, D3DRENDERSTATETYPE st, DWORD v)
{
    dev_set_rs(SELF(Dev, This), (DWORD)st, v);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetLightState(void *This, D3DLIGHTSTATETYPE st, DWORD *v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    *v = (DWORD)st < 16 ? SELF(Dev, This)->ls[st] : 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetLightState(void *This, D3DLIGHTSTATETYPE st, DWORD v)
{
    Dev *d = SELF(Dev, This);
    if ((DWORD)st < 16) d->ls[st] = v;
    switch (st) {
    case D3DLIGHTSTATE_MATERIAL: { Mat *m = mat_from(v); if (m) be_material(&m->m.diffuse.r); } break;
    case D3DLIGHTSTATE_AMBIENT: be_rs(139, v); break;
    case D3DLIGHTSTATE_FOGMODE: be_rs(140, v); break;
    case D3DLIGHTSTATE_FOGSTART: be_rs(36, v); break;
    case D3DLIGHTSTATE_FOGEND: be_rs(37, v); break;
    case D3DLIGHTSTATE_FOGDENSITY: be_rs(38, v); break;
    case D3DLIGHTSTATE_COLORVERTEX: be_rs(141, v); break;
    default: break;
    }
    return D3D_OK;
}
static int tf_index(DWORD st) { return st == D3DTRANSFORMSTATE_WORLD ? 0 : st == D3DTRANSFORMSTATE_VIEW ? 1 : st == D3DTRANSFORMSTATE_PROJECTION ? 2 : -1; }
static DWORD tf_9(DWORD st) { return st == D3DTRANSFORMSTATE_WORLD ? 256 : st; }
static HRESULT WINAPI dev_SetTransform(void *This, D3DTRANSFORMSTATETYPE st, D3DMATRIX *m)
{
    Dev *d = SELF(Dev, This);
    int k = tf_index(st);
    if (!m) return DDERR_INVALIDPARAMS;
    if (k < 0) return D3D_OK;
    memcpy(d->tf[k], m, 64);
    be_transform(tf_9(st), d->tf[k]);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTransform(void *This, D3DTRANSFORMSTATETYPE st, D3DMATRIX *m)
{
    Dev *d = SELF(Dev, This);
    int k = tf_index(st);
    if (!m) return DDERR_INVALIDPARAMS;
    if (k < 0) return DDERR_INVALIDPARAMS;
    memcpy(m, d->tf[k], 64);
    return D3D_OK;
}
static HRESULT WINAPI dev_MultiplyTransform(void *This, D3DTRANSFORMSTATETYPE st, D3DMATRIX *m)
{
    Dev *d = SELF(Dev, This);
    int k = tf_index(st), i, j, n;
    float r[16], a[16];
    if (!m || k < 0) return DDERR_INVALIDPARAMS;
    memcpy(a, m, 64);
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) { float s = 0; for (n = 0; n < 4; n++) s += a[i * 4 + n] * d->tf[k][n * 4 + j]; r[i * 4 + j] = s; }
    memcpy(d->tf[k], r, 64);
    be_transform(tf_9(st), d->tf[k]);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawPrimitive(void *This, D3DPRIMITIVETYPE p, DWORD fvf, void *v, DWORD n, DWORD fl)
{
    dev_draw(SELF(Dev, This), p, fvf, v, n, NULL, 0, fl);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitive(void *This, D3DPRIMITIVETYPE p, DWORD fvf, void *v, DWORD nv, WORD *idx, DWORD ni, DWORD fl)
{
    if (!idx || !ni) return D3D_OK;
    dev_draw(SELF(Dev, This), p, fvf, v, nv, idx, ni, fl);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetClipStatus(void *This, D3DCLIPSTATUS *c) { (void)This; (void)c; return D3D_OK; }
static HRESULT WINAPI dev_GetClipStatus(void *This, D3DCLIPSTATUS *c)
{
    (void)This;
    if (!c) return DDERR_INVALIDPARAMS;
    memset(c, 0, sizeof *c); return D3D_OK;
}
/* 要素ごとに別配列の頂点を一つの配列に詰め直す */
static BYTE *strided_pack(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA *sd, DWORD n)
{
    DWORD st = fvf_stride(fvf), i, t, nt = (fvf >> 8) & 15;
    BYTE *buf = (BYTE *)malloc((size_t)st * (n ? n : 1));
    static const int pos[8] = { 0, 12, 16, 16, 20, 24, 28, 32 };
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        BYTE *o = buf + (size_t)i * st;
        int ps = pos[(fvf >> 1) & 7];
        if (ps) { memcpy(o, (const BYTE *)sd->position.lpvData + (size_t)i * sd->position.dwStride, ps); o += ps; }
        if (fvf & D3DFVF_NORMAL) { memcpy(o, (const BYTE *)sd->normal.lpvData + (size_t)i * sd->normal.dwStride, 12); o += 12; }
        if (fvf & D3DFVF_RESERVED1) { memset(o, 0, 4); o += 4; }
        if (fvf & D3DFVF_DIFFUSE) { memcpy(o, (const BYTE *)sd->diffuse.lpvData + (size_t)i * sd->diffuse.dwStride, 4); o += 4; }
        if (fvf & D3DFVF_SPECULAR) { memcpy(o, (const BYTE *)sd->specular.lpvData + (size_t)i * sd->specular.dwStride, 4); o += 4; }
        for (t = 0; t < nt && t < 8; t++) {
            static const int sz[4] = { 8, 12, 16, 4 };
            int z = sz[(fvf >> (16 + t * 2)) & 3];
            memcpy(o, (const BYTE *)sd->textureCoords[t].lpvData + (size_t)i * sd->textureCoords[t].dwStride, z); o += z;
        }
    }
    return buf;
}
static HRESULT WINAPI dev_DrawPrimitiveStrided(void *This, D3DPRIMITIVETYPE p, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *sd, DWORD n, DWORD fl)
{
    BYTE *b;
    if (!sd || !n) return D3D_OK;
    b = strided_pack(fvf, sd, n);
    if (b) { dev_draw(SELF(Dev, This), p, fvf, b, n, NULL, 0, fl); free(b); }
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitiveStrided(void *This, D3DPRIMITIVETYPE p, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *sd, DWORD nv, WORD *idx, DWORD ni, DWORD fl)
{
    BYTE *b;
    if (!sd || !nv || !idx || !ni) return D3D_OK;
    b = strided_pack(fvf, sd, nv);
    if (b) { dev_draw(SELF(Dev, This), p, fvf, b, nv, idx, ni, fl); free(b); }
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawPrimitiveVB(void *This, D3DPRIMITIVETYPE p, void *vbi, DWORD start, DWORD n, DWORD fl)
{
    VB *v = vbi ? SELF(VB, vbi) : NULL;
    if (!v) return DDERR_INVALIDPARAMS;
    if (start + n > v->d.dwNumVertices) return DDERR_INVALIDPARAMS;
    dev_draw(SELF(Dev, This), p, v->d.dwFVF, v->data + (size_t)start * v->stride, n, NULL, 0, fl);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitiveVB(void *This, D3DPRIMITIVETYPE p, void *vbi, WORD *idx, DWORD ni, DWORD fl)
{
    VB *v = vbi ? SELF(VB, vbi) : NULL;
    if (!v) return DDERR_INVALIDPARAMS;
    if (!idx || !ni) return D3D_OK;
    dev_draw(SELF(Dev, This), p, v->d.dwFVF, v->data, v->d.dwNumVertices, idx, ni, fl);
    return D3D_OK;
}
static HRESULT WINAPI dev_ComputeSphereVisibility(void *This, D3DVECTOR *c, D3DVALUE *r, DWORD n, DWORD fl, DWORD *res)
{
    DWORD i; (void)This; (void)c; (void)r; (void)fl;
    if (res) for (i = 0; i < n; i++) res[i] = D3DVIS_INTERSECT_FRUSTUM;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTexture(void *This, DWORD st, void **out)
{
    Dev *d = SELF(Dev, This);
    if (!out) return DDERR_INVALIDPARAMS;
    if (st >= 8 || !d->tex[st]) { *out = NULL; return D3D_OK; }
    d->tex[st]->ref++; *out = &d->tex[st]->tex[1]; return D3D_OK;
}
static HRESULT WINAPI dev_SetTexture(void *This, DWORD st, void *t)
{
    Dev *d = SELF(Dev, This);
    if (st >= 8) return DDERR_INVALIDPARAMS;
    d->tex[st] = t ? SELF(Surf, t) : NULL;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTextureStageState(void *This, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    *v = st < 8 && (DWORD)t < 32 ? SELF(Dev, This)->tss[st][t] : 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetTextureStageState(void *This, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    dev_set_tss(SELF(Dev, This), st, (DWORD)t, v);
    return D3D_OK;
}
static HRESULT WINAPI dev_ValidateDevice(void *This, DWORD *n) { (void)This; if (n) *n = 1; return D3D_OK; }
static void *dev_vt[] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCaps, dev_GetStats, dev_AddViewport, dev_DeleteViewport, dev_NextViewport,
    dev_EnumTextureFormats, dev_BeginScene, dev_EndScene, dev_GetDirect3D, dev_SetCurrentViewport, dev_GetCurrentViewport,
    dev_SetRenderTarget, dev_GetRenderTarget, dev_Begin, dev_BeginIndexed, dev_Vertex, dev_Index, dev_End, dev_GetRenderState,
    dev_SetRenderState, dev_GetLightState, dev_SetLightState, dev_SetTransform, dev_GetTransform, dev_MultiplyTransform,
    dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_SetClipStatus, dev_GetClipStatus, dev_DrawPrimitiveStrided,
    dev_DrawIndexedPrimitiveStrided, dev_DrawPrimitiveVB, dev_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility,
    dev_GetTexture, dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice,
};

/* ================= 入口 ================= */
static void init_vt(void)
{
    if (vt_d3d) return;
    vt_d3d = d3d_vt; vt_dev = dev_vt; vt_vp = vp_vt; vt_mat = mat_vt; vt_light = light_vt; vt_tex1 = tex1_vt; vt_tex2 = tex2_vt; vt_vb = vb_vt;
}
HRESULT d3d6_query_d3d(DDraw *dd, REFIID iid, void **out)
{
    D3DObj *o;
    init_vt();
    if (!IsEqualGUID(iid, &IID_IDirect3D3)) return E_NOINTERFACE;
    if (!dd->d3d) {
        o = (D3DObj *)calloc(1, sizeof *o);
        o->i.vtbl = vt_d3d; o->i.self = o; o->i.ver = 3; o->dd = dd;
        dd->d3d = o;
        mkw_log("D3D6: IDirect3D3 を提供します\n");
    }
    o = (D3DObj *)dd->d3d;
    d3d_AddRef(&o->i);
    *out = &o->i;
    return S_OK;
}
