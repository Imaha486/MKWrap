/* DirectDraw ソフトウェアエミュレーション
 * サーフェスはすべて DIB セクションで持ち、プライマリへの転送を窓のキャンバスへ流して拡大表示する。
 * IDirectDraw/2/4/7, IDirectDrawSurface/2/3/4/7, IDirectDrawPalette, IDirectDrawClipper を実装 */
#include "mkw.h"
#include <ddraw.h>
#include <string.h>
#include <stdlib.h>

BOOL mkw_real_GetClientRect(HWND h, LPRECT r);
BOOL mkw_real_ClientToScreen(HWND h, LPPOINT p);
void mode_change_fake(int w, int h, int bpp);
void mode_change_fake2(int w, int h, int bpp, HWND hwnd);
void vwin_register_ext(HWND h);

typedef struct Surf Surf;
typedef struct DDraw DDraw;
typedef struct Pal Pal;
typedef struct Clip Clip;

typedef struct { void **vtbl; void *self; int ver; } IFACE;
#define SELF(T, p) ((T *)((IFACE *)(p))->self)

struct DDraw {
    IFACE i[4];          /* 1,2,4,7 */
    LONG ref;
    HWND hwnd;
    DWORD coop;
    int mode_w, mode_h, mode_bpp;
    Surf *primary;
};

struct Pal {
    IFACE i;
    LONG ref;
    DWORD caps;
    PALETTEENTRY e[256];
    DDraw *dd;
};

struct Clip {
    IFACE i;
    LONG ref;
    HWND hwnd;
};

struct Surf {
    IFACE i[5];          /* 1,2,3,4,7 */
    LONG ref;
    DDraw *dd;
    DWORD caps;
    int w, h, bpp, pitch;
    DWORD rmask, gmask, bmask;
    HDC dc; HBITMAP bmp, oldbmp; BYTE *bits;
    int primary;          /* プライマリサーフェス */
    int virt;             /* 窓モードのプライマリ (実体はキャンバス) */
    Surf *back;           /* フリップチェーンの次 */
    Surf *front;          /* バックバッファの場合の親 */
    Pal *pal;
    Clip *clip;
    DDCOLORKEY ck_src, ck_dst;
    DWORD ckflags;
    int dc_out;
    BYTE *shadow; int sh_w, sh_h, sh_pitch; /* 窓モードプライマリの Lock 用 */
    DWORD uniq;
    double last_present;
    int ext;              /* SetSurfaceDesc でアプリのメモリを使っている (DIB を持たない) */
    HDC xdc; HBITMAP xbmp, xold; BYTE *xbits; /* ext 用 GetDC の一時 DIB */
};

static void **vt_dd[4], **vt_s[5], **vt_pal, **vt_clip;
static DDraw *g_dd;

/* ---------------- 共通ヘルパ ---------------- */
static int ver_index_dd(int v) { return v == 1 ? 0 : v == 2 ? 1 : v == 4 ? 2 : 3; }
static int ver_index_s(int v) { return v == 1 ? 0 : v == 2 ? 1 : v == 3 ? 2 : v == 4 ? 3 : 4; }
static void *dd_iface(DDraw *d, int v) { return &d->i[ver_index_dd(v)]; }
static void *s_iface(Surf *s, int v) { return &s->i[ver_index_s(v)]; }

static void fill_pf(DDPIXELFORMAT *pf, int bpp, DWORD r, DWORD g, DWORD b)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    if (bpp == 8) { pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8; pf->dwRGBBitCount = 8; return; }
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = bpp;
    pf->dwRBitMask = r; pf->dwGBitMask = g; pf->dwBBitMask = b;
}
static void default_masks(int bpp, DWORD *r, DWORD *g, DWORD *b)
{
    if (bpp == 16) { *r = 0xF800; *g = 0x07E0; *b = 0x001F; }
    else if (bpp == 15) { *r = 0x7C00; *g = 0x03E0; *b = 0x001F; }
    else if (bpp == 24 || bpp == 32) { *r = 0xFF0000; *g = 0x00FF00; *b = 0x0000FF; }
    else { *r = *g = *b = 0; }
}
static int display_bpp(DDraw *d)
{
    if (d && d->mode_bpp) return d->mode_bpp;
    if (g_fake_mode_bpp >= 8) return g_fake_mode_bpp;   /* ChangeDisplaySettings で色数を変えたつもりのゲーム */
    return g_cfg.desktop_bpp;
}

static vwin_t *surf_win(Surf *s)
{
    vwin_t *w = NULL;
    if (s->clip && s->clip->hwnd) w = vwin_find(s->clip->hwnd);
    if (!w && s->dd && s->dd->hwnd) w = vwin_find(s->dd->hwnd);
    if (!w) w = vwin_main();
    return w;
}

/* DIB セクションを確保 */
static int surf_alloc(Surf *s, int w, int h, int bpp, DWORD rm, DWORD gm, DWORD bm)
{
    struct { BITMAPINFOHEADER h; DWORD m[256]; } bi;
    HDC sdc;
    int realbpp = bpp == 15 ? 16 : bpp;
    memset(&bi, 0, sizeof bi);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)realbpp;
    if (realbpp == 16 || realbpp == 32) {
        bi.h.biCompression = BI_BITFIELDS;
        bi.m[0] = rm; bi.m[1] = gm; bi.m[2] = bm;
    } else if (realbpp == 8) {
        int i;
        bi.h.biCompression = BI_RGB; bi.h.biClrUsed = 256;
        for (i = 0; i < 256; i++) bi.m[i] = (DWORD)((i & 0xE0) << 16 | (i & 0x1C) << 11 | (i & 3) << 6);
    } else bi.h.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    s->dc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    s->bmp = CreateDIBSection(s->dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&s->bits, NULL, 0);
    if (!s->bmp) { DeleteDC(s->dc); s->dc = NULL; return 0; }
    s->oldbmp = (HBITMAP)SelectObject(s->dc, s->bmp);
    s->w = w; s->h = h; s->bpp = realbpp;
    s->rmask = rm; s->gmask = gm; s->bmask = bm;
    s->pitch = ((w * realbpp + 31) / 32) * 4;
    memset(s->bits, 0, (size_t)s->pitch * h);
    return 1;
}

static void surf_sync_palette(Surf *s)
{
    RGBQUAD q[256]; int i;
    Pal *p = s->pal;
    if (s->bpp != 8 || !s->dc) return;
    if (!p && s->dd && s->dd->primary) p = s->dd->primary->pal;
    if (!p) return;
    for (i = 0; i < 256; i++) { q[i].rgbRed = p->e[i].peRed; q[i].rgbGreen = p->e[i].peGreen; q[i].rgbBlue = p->e[i].peBlue; q[i].rgbReserved = 0; }
    SetDIBColorTable(s->dc, 0, 256, q);
}

/* ---- 画素変換 ---- */
static int mask_shift(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

typedef struct {
    int bpp; DWORD rm, gm, bm; int rs, gs, bs, rb, gb, bb;
    const PALETTEENTRY *pal;
} fmt_t;

static void make_fmt(fmt_t *f, int bpp, DWORD rm, DWORD gm, DWORD bm, const PALETTEENTRY *pal)
{
    f->bpp = bpp; f->rm = rm; f->gm = gm; f->bm = bm;
    f->rs = mask_shift(rm); f->gs = mask_shift(gm); f->bs = mask_shift(bm);
    f->rb = mask_bits(rm); f->gb = mask_bits(gm); f->bb = mask_bits(bm);
    f->pal = pal;
}
static const PALETTEENTRY *surf_pal(Surf *s)
{
    if (s->pal) return s->pal->e;
    if (s->dd && s->dd->primary && s->dd->primary->pal) return s->dd->primary->pal->e;
    return NULL;
}
static void surf_fmt(Surf *s, fmt_t *f) { make_fmt(f, s->bpp, s->rmask, s->gmask, s->bmask, surf_pal(s)); }

static inline DWORD expand(DWORD v, int bits)
{
    if (bits >= 8) return v >> (bits - 8);
    if (bits <= 0) return 0;
    v <<= (8 - bits);
    return v | (v >> bits);
}
static inline DWORD raw_to_rgb(const fmt_t *f, DWORD p)
{
    if (f->bpp == 8) {
        if (f->pal) { const PALETTEENTRY *e = &f->pal[p & 255]; return (DWORD)e->peRed << 16 | (DWORD)e->peGreen << 8 | e->peBlue; }
        return p * 0x010101;
    }
    return expand((p & f->rm) >> f->rs, f->rb) << 16 | expand((p & f->gm) >> f->gs, f->gb) << 8 | expand((p & f->bm) >> f->bs, f->bb);
}
static inline DWORD rgb_to_raw(const fmt_t *f, DWORD c)
{
    DWORD r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (f->bpp == 8) {
        /* 最も近いパレット色 */
        int i, best = 0; long bd = 0x7fffffff;
        if (!f->pal) return (r + g + b) / 3;
        for (i = 0; i < 256; i++) {
            long dr = (long)f->pal[i].peRed - r, dg = (long)f->pal[i].peGreen - g, db = (long)f->pal[i].peBlue - b;
            long d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = i; if (!d) break; }
        }
        return best;
    }
    return ((r >> (8 - (f->rb > 8 ? 8 : f->rb))) << f->rs & f->rm) |
           ((g >> (8 - (f->gb > 8 ? 8 : f->gb))) << f->gs & f->gm) |
           ((b >> (8 - (f->bb > 8 ? 8 : f->bb))) << f->bs & f->bm);
}
static inline DWORD get_px(const BYTE *row, int x, int bpp)
{
    switch (bpp) {
    case 8: return row[x];
    case 16: return ((const WORD *)row)[x];
    case 24: { const BYTE *p = row + x * 3; return p[0] | p[1] << 8 | p[2] << 16; }
    default: return ((const DWORD *)row)[x];
    }
}
static inline void put_px(BYTE *row, int x, int bpp, DWORD v)
{
    switch (bpp) {
    case 8: row[x] = (BYTE)v; break;
    case 16: ((WORD *)row)[x] = (WORD)v; break;
    case 24: { BYTE *p = row + x * 3; p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); } break;
    default: ((DWORD *)row)[x] = v; break;
    }
}

/* 16bpp -> 32bpp 変換表 (マスクごとにキャッシュ) */
static struct { DWORD rm, gm, bm; DWORD *t; } g_lut16[4];
static const DWORD *lut16_get(const fmt_t *f)
{
    int i, k;
    for (k = 0; k < 4; k++) if (g_lut16[k].t && g_lut16[k].rm == f->rm && g_lut16[k].gm == f->gm && g_lut16[k].bm == f->bm) return g_lut16[k].t;
    for (k = 0; k < 4 && g_lut16[k].t; k++) {}
    if (k == 4) k = 0;
    if (!g_lut16[k].t) g_lut16[k].t = (DWORD *)malloc(65536 * 4);
    g_lut16[k].rm = f->rm; g_lut16[k].gm = f->gm; g_lut16[k].bm = f->bm;
    for (i = 0; i < 65536; i++) g_lut16[k].t[i] = raw_to_rgb(f, i);
    return g_lut16[k].t;
}

/* 転送対象のメモリ領域 */
typedef struct { BYTE *bits; int pitch, w, h, ox, oy; fmt_t f; vwin_t *win; } target_t;

static int surf_target(Surf *s, target_t *t)
{
    memset(t, 0, sizeof *t);
    if (s->virt) {
        vwin_t *w = surf_win(s);
        POINT o = { 0, 0 };
        if (!w) return 0;
        vwin_canvas_dc(w);
        if (!w->cbits) return 0;
        mkw_real_ClientToScreen(w->hwnd, &o);
        t->bits = (BYTE *)w->cbits; t->pitch = w->cw * 4; t->w = w->cw; t->h = w->ch;
        t->ox = -o.x; t->oy = -o.y; t->win = w;
        make_fmt(&t->f, 32, 0xFF0000, 0xFF00, 0xFF, NULL);
        return 1;
    }
    t->bits = s->bits; t->pitch = s->pitch; t->w = s->w; t->h = s->h;
    surf_fmt(s, &t->f);
    return t->bits != NULL;
}

static void surf_present(Surf *s, int frame);
static void surf_desc2(Surf *s, DDSURFACEDESC2 *d);
static void desc2_to_desc1(const DDSURFACEDESC2 *a, DDSURFACEDESC *b);

/* 汎用転送 (拡大縮小・カラーキー・形式変換) */
static HRESULT do_blt(Surf *dst, RECT *dr_in, Surf *src, RECT *sr_in, int keyed, DWORD klo, DWORD khi, int mirror_lr, int mirror_ud)
{
    target_t dt, st;
    RECT dr, sr;
    int dw, dh, sw, sh, x, y, same;
    if (!surf_target(dst, &dt) || !surf_target(src, &st)) return DDERR_GENERIC;
    if (dr_in) dr = *dr_in; else { dr.left = 0; dr.top = 0; dr.right = dst->virt ? dt.w - dt.ox : dt.w; dr.bottom = dst->virt ? dt.h - dt.oy : dt.h;
        if (dst->virt) { dr.left = -dt.ox; dr.top = -dt.oy; } }
    if (sr_in) sr = *sr_in; else { sr.left = 0; sr.top = 0; sr.right = src->virt ? st.w : st.w; sr.bottom = st.h;
        if (src->virt) { sr.left = -st.ox; sr.top = -st.oy; sr.right = sr.left + st.w; sr.bottom = sr.top + st.h; } }
    OffsetRect(&dr, dt.ox, dt.oy);
    OffsetRect(&sr, st.ox, st.oy);
    dw = dr.right - dr.left; dh = dr.bottom - dr.top;
    sw = sr.right - sr.left; sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return DD_OK;
    same = (dt.f.bpp == st.f.bpp && dt.f.rm == st.f.rm && dt.f.gm == st.f.gm && dt.f.bm == st.f.bm && (dt.f.bpp != 8 || 1));
    {
        int y0 = dr.top < 0 ? -dr.top : 0, y1 = dr.bottom > dt.h ? dh - (dr.bottom - dt.h) : dh;
        int x0 = dr.left < 0 ? -dr.left : 0, x1 = dr.right > dt.w ? dw - (dr.right - dt.w) : dw;
        int bpd = dt.f.bpp / 8;
        /* 同形式・等倍・キーなしは行コピー */
        if (same && dw == sw && dh == sh && !keyed && !mirror_lr && !mirror_ud) {
            for (y = y0; y < y1; y++) {
                int sy = sr.top + y, sx0 = sr.left + x0, n = x1 - x0;
                int cx0 = x0;
                if (sy < 0 || sy >= st.h) continue;
                if (sx0 < 0) { cx0 -= sx0; n += sx0; sx0 = 0; }
                if (sx0 + n > st.w) n = st.w - sx0;
                if (n <= 0) continue;
                memmove(dt.bits + (size_t)(dr.top + y) * dt.pitch + (size_t)(dr.left + cx0) * bpd,
                        st.bits + (size_t)sy * st.pitch + (size_t)sx0 * bpd, (size_t)n * bpd);
            }
        } else {
            int *xmap = (int *)malloc(sizeof(int) * (x1 > 0 ? x1 : 1));
            const DWORD *lut = NULL; DWORD lut8[256];
            int to32 = (dt.f.bpp == 32 && dt.f.rm == 0xFF0000 && dt.f.gm == 0xFF00 && dt.f.bm == 0xFF);
            for (x = x0; x < x1; x++) {
                int sx = sr.left + (int)((long long)(mirror_lr ? (dw - 1 - x) : x) * sw / dw);
                xmap[x] = (sx < 0 || sx >= st.w) ? -1 : sx;
            }
            if (to32 && !same) {
                if (st.f.bpp == 8) { int i; for (i = 0; i < 256; i++) lut8[i] = raw_to_rgb(&st.f, i); lut = lut8; }
                else if (st.f.bpp == 16) lut = lut16_get(&st.f);
            }
            for (y = y0; y < y1; y++) {
                int sy = sr.top + (int)((long long)(mirror_ud ? (dh - 1 - y) : y) * sh / dh);
                BYTE *drow; const BYTE *srow;
                if (sy < 0 || sy >= st.h) continue;
                drow = dt.bits + (size_t)(dr.top + y) * dt.pitch;
                srow = st.bits + (size_t)sy * st.pitch;
                if (to32 && lut) {
                    DWORD *d32 = (DWORD *)drow + dr.left;
                    if (st.f.bpp == 8) {
                        for (x = x0; x < x1; x++) { int sx = xmap[x]; DWORD p; if (sx < 0) continue; p = srow[sx]; if (keyed && p >= klo && p <= khi) continue; d32[x] = lut[p]; }
                    } else {
                        const WORD *s16 = (const WORD *)srow;
                        for (x = x0; x < x1; x++) { int sx = xmap[x]; DWORD p; if (sx < 0) continue; p = s16[sx]; if (keyed && p >= klo && p <= khi) continue; d32[x] = lut[p]; }
                    }
                } else if (same && (dt.f.bpp == 32 || dt.f.bpp == 16 || dt.f.bpp == 8)) {
                    if (dt.f.bpp == 32) { DWORD *d = (DWORD *)drow + dr.left; const DWORD *sp = (const DWORD *)srow;
                        for (x = x0; x < x1; x++) { int sx = xmap[x]; DWORD p; if (sx < 0) continue; p = sp[sx]; if (keyed && p >= klo && p <= khi) continue; d[x] = p; } }
                    else if (dt.f.bpp == 16) { WORD *d = (WORD *)drow + dr.left; const WORD *sp = (const WORD *)srow;
                        for (x = x0; x < x1; x++) { int sx = xmap[x]; DWORD p; if (sx < 0) continue; p = sp[sx]; if (keyed && p >= klo && p <= khi) continue; d[x] = (WORD)p; } }
                    else { BYTE *d = drow + dr.left;
                        for (x = x0; x < x1; x++) { int sx = xmap[x]; DWORD p; if (sx < 0) continue; p = srow[sx]; if (keyed && p >= klo && p <= khi) continue; d[x] = (BYTE)p; } }
                } else {
                    for (x = x0; x < x1; x++) {
                        int sx = xmap[x]; DWORD p;
                        if (sx < 0) continue;
                        p = get_px(srow, sx, st.f.bpp);
                        if (keyed && p >= klo && p <= khi) continue;
                        if (!same) p = rgb_to_raw(&dt.f, raw_to_rgb(&st.f, p));
                        put_px(drow, dr.left + x, dt.f.bpp, p);
                    }
                }
            }
            free(xmap);
        }
    }
    if (dst->primary) {
        long long area = (long long)dw * dh;
        surf_present(dst, area * 2 >= (long long)dt.w * dt.h);
    }
    return DD_OK;
}

static HRESULT do_fill(Surf *dst, RECT *dr_in, DWORD color)
{
    target_t dt; RECT dr; int x, y;
    if (!surf_target(dst, &dt)) return DDERR_GENERIC;
    if (dr_in) dr = *dr_in;
    else { dr.left = dst->virt ? -dt.ox : 0; dr.top = dst->virt ? -dt.oy : 0; dr.right = dr.left + dt.w; dr.bottom = dr.top + dt.h; }
    OffsetRect(&dr, dt.ox, dt.oy);
    if (dr.left < 0) dr.left = 0;
    if (dr.top < 0) dr.top = 0;
    if (dr.right > dt.w) dr.right = dt.w;
    if (dr.bottom > dt.h) dr.bottom = dt.h;
    if (dst->virt) { fmt_t pf; DWORD r, g, b; int bpp = display_bpp(dst->dd); default_masks(bpp, &r, &g, &b); make_fmt(&pf, bpp == 15 ? 16 : bpp, r, g, b, surf_pal(dst)); color = raw_to_rgb(&pf, color); }
    for (y = dr.top; y < dr.bottom; y++) {
        BYTE *row = dt.bits + (size_t)y * dt.pitch;
        if (dt.f.bpp == 8) memset(row + dr.left, (int)color, dr.right - dr.left);
        else for (x = dr.left; x < dr.right; x++) put_px(row, x, dt.f.bpp, color);
    }
    if (dst->primary) {
        long long area = (long long)(dr.right - dr.left) * (dr.bottom - dr.top);
        surf_present(dst, area * 2 >= (long long)dt.w * dt.h);
    }
    return DD_OK;
}

/* プライマリの内容を窓へ */
static void surf_present(Surf *s, int frame)
{
    vwin_t *w;
    double now = mkw_now();
    if (s->front) s = s->front;
    w = surf_win(s);
    if (!w) return;
    if (!s->virt) {
        /* 専用バッファ -> キャンバスへ変換 */
        fmt_t f; int x, y; DWORD lut8[256];
        vwin_ensure_canvas(w, s->w, s->h);
        if (!w->cbits) return;
        surf_fmt(s, &f);
        if (s->bpp == 8) { int i; for (i = 0; i < 256; i++) lut8[i] = raw_to_rgb(&f, i); }
        for (y = 0; y < s->h && y < w->ch; y++) {
            const BYTE *srow = s->bits + (size_t)y * s->pitch;
            DWORD *drow = (DWORD *)w->cbits + (size_t)y * w->cw;
            int n = s->w < w->cw ? s->w : w->cw;
            if (s->bpp == 32 && s->rmask == 0xFF0000 && s->bmask == 0xFF) memcpy(drow, srow, (size_t)n * 4);
            else if (s->bpp == 16) { const DWORD *t = lut16_get(&f); const WORD *s16 = (const WORD *)srow; for (x = 0; x < n; x++) drow[x] = t[s16[x]]; }
            else if (s->bpp == 8) { for (x = 0; x < n; x++) drow[x] = lut8[srow[x]]; }
            else for (x = 0; x < n; x++) drow[x] = raw_to_rgb(&f, get_px(srow, x, s->bpp));
        }
    }
    if (!frame && now - s->last_present < 0.008) { w->dirty = 1; return; }
    s->last_present = now;
    vwin_present(w, frame);
}

/* ================= IDirectDrawPalette ================= */
static HRESULT WINAPI pal_QueryInterface(void *This, REFIID iid, void **out)
{
    Pal *p = SELF(Pal, This);
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirectDrawPalette)) { *out = &p->i; p->ref++; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI pal_AddRef(void *This) { return ++SELF(Pal, This)->ref; }
static ULONG WINAPI pal_Release(void *This)
{
    Pal *p = SELF(Pal, This);
    LONG r = --p->ref;
    if (r <= 0) { free(p); return 0; }
    return r;
}
static HRESULT WINAPI pal_GetCaps(void *This, DWORD *caps) { *caps = SELF(Pal, This)->caps; return DD_OK; }
static HRESULT WINAPI pal_GetEntries(void *This, DWORD fl, DWORD start, DWORD n, PALETTEENTRY *e)
{
    Pal *p = SELF(Pal, This); (void)fl;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(e, &p->e[start], n * sizeof(PALETTEENTRY));
    return DD_OK;
}
static HRESULT WINAPI pal_Initialize(void *This, void *dd, DWORD fl, PALETTEENTRY *e) { (void)This; (void)dd; (void)fl; (void)e; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI pal_SetEntries(void *This, DWORD fl, DWORD start, DWORD n, PALETTEENTRY *e)
{
    Pal *p = SELF(Pal, This); (void)fl;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(&p->e[start], e, n * sizeof(PALETTEENTRY));
    if (p->dd && p->dd->primary && p->dd->primary->pal == p) {
        Surf *pr = p->dd->primary;
        surf_sync_palette(pr);
        if (!pr->virt) surf_present(pr, 0);
    }
    return DD_OK;
}
static void *pal_vt[] = { pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries };

/* ================= IDirectDrawClipper ================= */
static HRESULT WINAPI clip_QueryInterface(void *This, REFIID iid, void **out)
{
    Clip *c = SELF(Clip, This);
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirectDrawClipper)) { *out = &c->i; c->ref++; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI clip_AddRef(void *This) { return ++SELF(Clip, This)->ref; }
static ULONG WINAPI clip_Release(void *This)
{
    Clip *c = SELF(Clip, This);
    LONG r = --c->ref;
    if (r <= 0) { free(c); return 0; }
    return r;
}
static HRESULT WINAPI clip_GetClipList(void *This, RECT *r, RGNDATA *rd, DWORD *size)
{
    Clip *c = SELF(Clip, This);
    DWORD need = sizeof(RGNDATAHEADER) + sizeof(RECT);
    RECT cr; POINT o = { 0, 0 };
    vwin_t *w = c->hwnd ? vwin_find(c->hwnd) : NULL;
    if (c->hwnd) {
        if (w) { cr.left = 0; cr.top = 0; cr.right = w->lw; cr.bottom = w->lh; }
        else mkw_real_GetClientRect(c->hwnd, &cr);
        mkw_real_ClientToScreen(c->hwnd, &o);
        OffsetRect(&cr, o.x, o.y);
    } else SetRect(&cr, 0, 0, 640, 480);
    if (r) IntersectRect(&cr, &cr, r);
    if (!rd) { *size = need; return DD_OK; }
    if (*size < need) { *size = need; return DDERR_REGIONTOOSMALL; }
    rd->rdh.dwSize = sizeof(RGNDATAHEADER); rd->rdh.iType = RDH_RECTANGLES; rd->rdh.nCount = 1;
    rd->rdh.nRgnSize = sizeof(RECT); rd->rdh.rcBound = cr;
    memcpy(rd->Buffer, &cr, sizeof(RECT));
    *size = need;
    return DD_OK;
}
static HRESULT WINAPI clip_GetHWnd(void *This, HWND *h) { *h = SELF(Clip, This)->hwnd; return DD_OK; }
static HRESULT WINAPI clip_Initialize(void *This, void *dd, DWORD fl) { (void)This; (void)dd; (void)fl; return DD_OK; }
static HRESULT WINAPI clip_IsClipListChanged(void *This, BOOL *b) { (void)This; *b = FALSE; return DD_OK; }
static HRESULT WINAPI clip_SetClipList(void *This, RGNDATA *rd, DWORD fl) { (void)This; (void)rd; (void)fl; return DD_OK; }
static HRESULT WINAPI clip_SetHWnd(void *This, DWORD fl, HWND h) { (void)fl; SELF(Clip, This)->hwnd = h; mkw_log("Clipper SetHWnd %p\n", (void *)h); return DD_OK; }
static void *clip_vt[] = { clip_QueryInterface, clip_AddRef, clip_Release, clip_GetClipList, clip_GetHWnd, clip_Initialize, clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd };

static Clip *new_clip(void)
{
    Clip *c = (Clip *)calloc(1, sizeof *c);
    c->i.vtbl = clip_vt; c->i.self = c; c->ref = 1;
    return c;
}

/* ================= IDirectDrawSurface ================= */
static Surf *new_surf(DDraw *dd)
{
    Surf *s = (Surf *)calloc(1, sizeof *s);
    int i, v[5] = { 1, 2, 3, 4, 7 };
    for (i = 0; i < 5; i++) { s->i[i].vtbl = vt_s[i]; s->i[i].self = s; s->i[i].ver = v[i]; }
    s->ref = 1; s->dd = dd; s->uniq = 1;
    return s;
}
static void free_surf(Surf *s)
{
    if (s->back && s->back != s && s->back->front == s) { Surf *b = s->back; s->back = NULL; b->front = NULL; free_surf(b); }
    if (s->dc) { SelectObject(s->dc, s->oldbmp); DeleteDC(s->dc); }
    if (s->bmp) DeleteObject(s->bmp);
    if (s->xdc) { SelectObject(s->xdc, s->xold); DeleteDC(s->xdc); }
    if (s->xbmp) DeleteObject(s->xbmp);
    if (s->pal) s->pal->ref--;
    if (s->clip) s->clip->ref--;
    free(s->shadow);
    if (s->dd && s->dd->primary == s) s->dd->primary = NULL;
    free(s);
}

static HRESULT WINAPI s_QueryInterface(void *This, REFIID iid, void **out)
{
    Surf *s = SELF(Surf, This);
    int v = 0;
    if (IsEqualGUID(iid, &IID_IUnknown)) v = ((IFACE *)This)->ver;
    else if (IsEqualGUID(iid, &IID_IDirectDrawSurface)) v = 1;
    else if (IsEqualGUID(iid, &IID_IDirectDrawSurface2)) v = 2;
    else if (IsEqualGUID(iid, &IID_IDirectDrawSurface3)) v = 3;
    else if (IsEqualGUID(iid, &IID_IDirectDrawSurface4)) v = 4;
    else if (IsEqualGUID(iid, &IID_IDirectDrawSurface7)) v = 7;
    if (!v) { *out = NULL; return E_NOINTERFACE; }
    s->ref++;
    *out = s_iface(s, v);
    return S_OK;
}
static ULONG WINAPI s_AddRef(void *This) { return ++SELF(Surf, This)->ref; }
static ULONG WINAPI s_Release(void *This)
{
    Surf *s = SELF(Surf, This);
    LONG r = --s->ref;
    if (r <= 0) {
        if (s->front) return 0; /* バックバッファはフロントと一緒に解放 */
        free_surf(s); return 0;
    }
    return r;
}
static HRESULT WINAPI s_AddAttachedSurface(void *This, void *att)
{
    Surf *s = SELF(Surf, This), *a = att ? SELF(Surf, att) : NULL;
    if (!a) return DDERR_INVALIDPARAMS;
    if (!s->back) { s->back = a; a->front = s; a->ref++; }
    return DD_OK;
}
static HRESULT WINAPI s_Unsupported0(void *This) { (void)This; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_AddOverlayDirtyRect(void *This, RECT *r) { (void)This; (void)r; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI s_Blt(void *This, RECT *dr, void *srcif, RECT *sr, DWORD fl, DDBLTFX *fx)
{
    { static int n; if (g_cfg.log && n < 20) { n++; mkw_log("Blt dst=%p(primary=%d) src=%p fl=%lx dr=%ld,%ld-%ld,%ld\n", This, SELF(Surf, This)->primary, srcif, (unsigned long)fl, dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1); } }
    Surf *d = SELF(Surf, This), *s = srcif ? SELF(Surf, srcif) : NULL;
    if (fl & DDBLT_COLORFILL) return do_fill(d, dr, fx ? fx->dwFillColor : 0);
    if (fl & DDBLT_DEPTHFILL) return DD_OK;
    if (!s) {
        if (fl & DDBLT_ROP && fx && fx->dwROP == BLACKNESS) return do_fill(d, dr, 0);
        return DD_OK;
    }
    {
        int keyed = 0; DWORD lo = 0, hi = 0;
        if (fl & DDBLT_KEYSRCOVERRIDE && fx) { keyed = 1; lo = fx->ddckSrcColorkey.dwColorSpaceLowValue; hi = fx->ddckSrcColorkey.dwColorSpaceHighValue; }
        else if (fl & DDBLT_KEYSRC && (s->ckflags & DDCKEY_SRCBLT)) { keyed = 1; lo = s->ck_src.dwColorSpaceLowValue; hi = s->ck_src.dwColorSpaceHighValue; }
        if (keyed && hi < lo) hi = lo;
        return do_blt(d, dr, s, sr, keyed, lo, hi,
                      (fl & DDBLT_DDFX) && fx && (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT),
                      (fl & DDBLT_DDFX) && fx && (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN));
    }
}
static HRESULT WINAPI s_BltBatch(void *This, void *b, DWORD n, DWORD fl) { (void)This; (void)b; (void)n; (void)fl; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_BltFast(void *This, DWORD x, DWORD y, void *srcif, RECT *sr, DWORD trans)
{
    Surf *d = SELF(Surf, This), *s = srcif ? SELF(Surf, srcif) : NULL;
    RECT dr, r;
    int keyed = 0; DWORD lo = 0, hi = 0;
    if (!s) return DDERR_INVALIDPARAMS;
    if (sr) r = *sr; else SetRect(&r, 0, 0, s->w, s->h);
    SetRect(&dr, x, y, x + (r.right - r.left), y + (r.bottom - r.top));
    if ((trans & DDBLTFAST_SRCCOLORKEY) && (s->ckflags & DDCKEY_SRCBLT)) { keyed = 1; lo = s->ck_src.dwColorSpaceLowValue; hi = s->ck_src.dwColorSpaceHighValue; if (hi < lo) hi = lo; }
    return do_blt(d, &dr, s, &r, keyed, lo, hi, 0, 0);
}
static HRESULT WINAPI s_DeleteAttachedSurface(void *This, DWORD fl, void *att)
{
    Surf *s = SELF(Surf, This); (void)fl;
    if (att && s->back == SELF(Surf, att)) { s->back->front = NULL; s->back->ref--; s->back = NULL; }
    return DD_OK;
}
static HRESULT WINAPI s_EnumAttachedSurfaces(void *This, void *ctx, void *cb)
{
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    if (s->back && s->back != s) {
        Surf *b = s->back;
        b->ref++;
        if (v >= 4) {
            DDSURFACEDESC2 d; memset(&d, 0, sizeof d); d.dwSize = sizeof d;
            surf_desc2(b, &d);
            ((LPDDENUMSURFACESCALLBACK2)cb)(s_iface(b, v), &d, ctx);
        } else {
            DDSURFACEDESC2 d2; DDSURFACEDESC d; memset(&d, 0, sizeof d); d.dwSize = sizeof d; d2.dwSize = sizeof d2;
            surf_desc2(b, &d2); desc2_to_desc1(&d2, &d);
            ((LPDDENUMSURFACESCALLBACK)cb)(s_iface(b, v), &d, ctx);
        }
    }
    return DD_OK;
}
static HRESULT WINAPI s_EnumOverlayZOrders(void *This, DWORD fl, void *ctx, void *cb) { (void)This; (void)fl; (void)ctx; (void)cb; return DD_OK; }

static void swap_buffers(Surf *a, Surf *b)
{
    HDC dc = a->dc; HBITMAP bm = a->bmp, ob = a->oldbmp; BYTE *bits = a->bits; int pitch = a->pitch, ext = a->ext;
    a->dc = b->dc; a->bmp = b->bmp; a->oldbmp = b->oldbmp; a->bits = b->bits; a->pitch = b->pitch; a->ext = b->ext;
    b->dc = dc; b->bmp = bm; b->oldbmp = ob; b->bits = bits; b->pitch = pitch; b->ext = ext;
}
static HRESULT WINAPI s_Flip(void *This, void *target, DWORD fl)
{
    { static int n; if (g_cfg.log && n < 5) { n++; mkw_log("Flip\n"); } }
    Surf *s = SELF(Surf, This);
    (void)fl;
    if (!s->primary) return DDERR_NOTFLIPPABLE;
    if (s->back && !s->virt) {
        Surf *t = target ? SELF(Surf, target) : s->back;
        /* 3重以上のチェーン: 前方へ回転 */
        Surf *last = s->back;
        if (t == s->back) {
            while (last->back && last->back != s) last = last->back;
            if (last == s->back) swap_buffers(s, s->back);
            else {
                Surf *p = s; Surf *n;
                /* front <- back1 <- back2 ... */
                for (n = s->back; n; p = n, n = (n->back != s ? n->back : NULL)) swap_buffers(p, n);
            }
        } else swap_buffers(s, t);
    }
    surf_present(s, 1);
    return DD_OK;
}
static HRESULT WINAPI s_GetAttachedSurface(void *This, DDSCAPS *caps, void **out)
{
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    (void)caps;
    if (!s->back) { *out = NULL; return DDERR_NOTFOUND; }
    s->back->ref++;
    *out = s_iface(s->back, v);
    return DD_OK;
}
static HRESULT WINAPI s_GetBltStatus(void *This, DWORD fl) { (void)This; (void)fl; return DD_OK; }
static HRESULT WINAPI s_GetCaps(void *This, DDSCAPS2 *caps)
{
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    if (v >= 4) { memset(caps, 0, sizeof(DDSCAPS2)); caps->dwCaps = s->caps; }
    else ((DDSCAPS *)caps)->dwCaps = s->caps;
    return DD_OK;
}
static HRESULT WINAPI s_GetClipper(void *This, void **out)
{
    Surf *s = SELF(Surf, This);
    if (!s->clip) { *out = NULL; return DDERR_NOCLIPPERATTACHED; }
    s->clip->ref++; *out = &s->clip->i; return DD_OK;
}
static HRESULT WINAPI s_GetColorKey(void *This, DWORD fl, DDCOLORKEY *ck)
{
    Surf *s = SELF(Surf, This);
    if (fl & DDCKEY_SRCBLT) { if (!(s->ckflags & DDCKEY_SRCBLT)) return DDERR_NOCOLORKEY; *ck = s->ck_src; return DD_OK; }
    if (fl & DDCKEY_DESTBLT) { if (!(s->ckflags & DDCKEY_DESTBLT)) return DDERR_NOCOLORKEY; *ck = s->ck_dst; return DD_OK; }
    return DDERR_INVALIDPARAMS;
}
static HRESULT WINAPI s_GetDC(void *This, HDC *out)
{
    { static int n; if (g_cfg.log && n < 10) { n++; mkw_log("GetDC %p primary=%d\n", This, SELF(Surf, This)->primary); } }
    Surf *s = SELF(Surf, This);
    if (s->virt) {
        vwin_t *w = surf_win(s);
        if (!w) return DDERR_GENERIC;
        *out = vwin_canvas_dc(w); s->dc_out = 1; return DD_OK;
    }
    if (s->ext) {
        /* アプリのメモリを使うサーフェス: 一時 DIB にコピーして渡し、ReleaseDC で書き戻す */
        int y, row = ((s->w * s->bpp + 31) / 32) * 4;
        if (!s->xdc) {
            struct { BITMAPINFOHEADER h; DWORD m[256]; } bi; HDC sdc;
            memset(&bi, 0, sizeof bi);
            bi.h.biSize = sizeof bi.h; bi.h.biWidth = s->w; bi.h.biHeight = -s->h; bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)s->bpp;
            if (s->bpp == 16 || s->bpp == 32) { bi.h.biCompression = BI_BITFIELDS; bi.m[0] = s->rmask; bi.m[1] = s->gmask; bi.m[2] = s->bmask; }
            sdc = GetDC(NULL); s->xdc = CreateCompatibleDC(sdc); ReleaseDC(NULL, sdc);
            s->xbmp = CreateDIBSection(s->xdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&s->xbits, NULL, 0);
            if (!s->xbmp) { DeleteDC(s->xdc); s->xdc = NULL; return DDERR_GENERIC; }
            s->xold = (HBITMAP)SelectObject(s->xdc, s->xbmp);
        }
        GdiFlush();
        for (y = 0; y < s->h; y++) memcpy(s->xbits + (size_t)y * row, s->bits + (size_t)y * s->pitch, (size_t)(s->w * s->bpp / 8));
        *out = s->xdc; s->dc_out = 1;
        return DD_OK;
    }
    if (!s->dc) return DDERR_GENERIC;
    surf_sync_palette(s);
    GdiFlush();
    *out = s->dc; s->dc_out = 1;
    return DD_OK;
}
static HRESULT WINAPI s_ReleaseDC(void *This, HDC dc)
{
    Surf *s = SELF(Surf, This);
    (void)dc;
    GdiFlush();
    if (s->ext && s->xdc && s->dc_out) {
        int y, row = ((s->w * s->bpp + 31) / 32) * 4;
        for (y = 0; y < s->h; y++) memcpy(s->bits + (size_t)y * s->pitch, s->xbits + (size_t)y * row, (size_t)(s->w * s->bpp / 8));
    }
    s->dc_out = 0;
    if (s->primary) surf_present(s, 0);
    return DD_OK;
}
static HRESULT WINAPI s_GetFlipStatus(void *This, DWORD fl) { (void)This; (void)fl; return DD_OK; }
static HRESULT WINAPI s_GetOverlayPosition(void *This, LONG *x, LONG *y) { (void)This; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_GetPalette(void *This, void **out)
{
    Surf *s = SELF(Surf, This);
    if (!s->pal) { *out = NULL; return DDERR_NOPALETTEATTACHED; }
    s->pal->ref++; *out = &s->pal->i; return DD_OK;
}
static void surf_pf(Surf *s, DDPIXELFORMAT *pf)
{
    if (s->virt) {
        DWORD r, g, b; int bpp = display_bpp(s->dd);
        default_masks(bpp, &r, &g, &b);
        fill_pf(pf, bpp == 15 ? 16 : bpp, r, g, b);
    } else fill_pf(pf, s->bpp, s->rmask, s->gmask, s->bmask);
}
static HRESULT WINAPI s_GetPixelFormat(void *This, DDPIXELFORMAT *pf) { surf_pf(SELF(Surf, This), pf); return DD_OK; }

static void surf_desc2(Surf *s, DDSURFACEDESC2 *d)
{
    DWORD sz = d->dwSize;
    memset(d, 0, sizeof *d);
    d->dwSize = sz;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    if (s->virt) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int bpp = display_bpp(s->dd);
        d->dwWidth = sw; d->dwHeight = sh; d->lPitch = sw * ((bpp == 15 ? 16 : bpp) / 8);
    } else { d->dwWidth = s->w; d->dwHeight = s->h; d->lPitch = s->pitch; }
    d->ddsCaps.dwCaps = s->caps;
    surf_pf(s, &d->ddpfPixelFormat);
    if (s->ckflags & DDCKEY_SRCBLT) { d->dwFlags |= DDSD_CKSRCBLT; d->ddckCKSrcBlt = s->ck_src; }
    if (s->ckflags & DDCKEY_DESTBLT) { d->dwFlags |= DDSD_CKDESTBLT; d->ddckCKDestBlt = s->ck_dst; }
    if (s->back && s->primary) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; { Surf *b = s->back; int n = 0; while (b && b != s && n < 8) { n++; b = b->back; } d->dwBackBufferCount = n; } }
}
static void desc2_to_desc1(const DDSURFACEDESC2 *a, DDSURFACEDESC *b)
{
    DWORD sz = b->dwSize;
    memset(b, 0, sizeof *b);
    b->dwSize = sz ? sz : sizeof *b;
    b->dwFlags = a->dwFlags; b->dwHeight = a->dwHeight; b->dwWidth = a->dwWidth; b->lPitch = a->lPitch;
    b->dwBackBufferCount = a->dwBackBufferCount; b->lpSurface = a->lpSurface;
    b->ddckCKDestBlt = a->ddckCKDestBlt; b->ddckCKSrcBlt = a->ddckCKSrcBlt;
    b->ddpfPixelFormat = a->ddpfPixelFormat; b->ddsCaps.dwCaps = a->ddsCaps.dwCaps;
}
static void desc1_to_desc2(const DDSURFACEDESC *a, DDSURFACEDESC2 *b)
{
    memset(b, 0, sizeof *b);
    b->dwSize = sizeof *b;
    b->dwFlags = a->dwFlags; b->dwHeight = a->dwHeight; b->dwWidth = a->dwWidth; b->lPitch = a->lPitch;
    b->dwBackBufferCount = a->dwBackBufferCount; b->lpSurface = a->lpSurface;
    b->ddckCKDestBlt = a->ddckCKDestBlt; b->ddckCKSrcBlt = a->ddckCKSrcBlt;
    b->ddpfPixelFormat = a->ddpfPixelFormat; b->ddsCaps.dwCaps = a->ddsCaps.dwCaps;
}
static HRESULT WINAPI s_GetSurfaceDesc(void *This, void *desc)
{
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    DDSURFACEDESC2 d;
    if (!desc) return DDERR_INVALIDPARAMS;
    d.dwSize = sizeof d;
    surf_desc2(s, &d);
    if (v >= 4) { DWORD sz = ((DDSURFACEDESC2 *)desc)->dwSize; *(DDSURFACEDESC2 *)desc = d; ((DDSURFACEDESC2 *)desc)->dwSize = sz ? sz : sizeof d; }
    else desc2_to_desc1(&d, (DDSURFACEDESC *)desc);
    return DD_OK;
}
static HRESULT WINAPI s_Initialize(void *This, void *dd, void *desc) { (void)This; (void)dd; (void)desc; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI s_IsLost(void *This) { (void)This; return DD_OK; }
static HRESULT WINAPI s_Lock(void *This, RECT *r, void *desc, DWORD fl, HANDLE ev)
{
    { static int n; if (g_cfg.log && n < 10) { n++; mkw_log("Lock %p primary=%d\n", This, SELF(Surf, This)->primary); } }
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    DDSURFACEDESC2 d;
    BYTE *base; int pitch, bypp;
    (void)fl; (void)ev;
    if (!desc) return DDERR_INVALIDPARAMS;
    d.dwSize = sizeof d;
    surf_desc2(s, &d);
    if (s->virt) {
        /* 窓モードのプライマリ: 画面サイズの影バッファを用意し、Unlock 時にキャンバスへ反映 */
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int bpp = display_bpp(s->dd); bpp = bpp == 15 ? 16 : bpp;
        if (!s->shadow || s->sh_w != sw || s->sh_h != sh) {
            free(s->shadow);
            s->sh_w = sw; s->sh_h = sh; s->sh_pitch = sw * bpp / 8;
            s->shadow = (BYTE *)calloc((size_t)s->sh_pitch, sh);
        }
        base = s->shadow; pitch = s->sh_pitch; bypp = bpp / 8;
        /* 現在のキャンバス内容を影バッファへ */
        {
            target_t t; int x, y; fmt_t pf; DWORD rm, gm, bm;
            if (surf_target(s, &t)) {
                default_masks(display_bpp(s->dd), &rm, &gm, &bm);
                make_fmt(&pf, bpp, rm, gm, bm, surf_pal(s));
                for (y = 0; y < t.h; y++) {
                    int sy = y - t.oy; const DWORD *crow = (const DWORD *)(t.bits + (size_t)y * t.pitch);
                    if (sy < 0 || sy >= sh) continue;
                    for (x = 0; x < t.w; x++) {
                        int sx = x - t.ox;
                        if (sx < 0 || sx >= sw) continue;
                        put_px(base + (size_t)sy * pitch, sx, bpp, rgb_to_raw(&pf, crow[x]));
                    }
                }
            }
        }
    } else {
        GdiFlush();
        base = s->bits; pitch = s->pitch; bypp = s->bpp / 8;
    }
    d.lPitch = pitch;
    d.lpSurface = base + (r ? (size_t)r->top * pitch + (size_t)r->left * bypp : 0);
    d.dwFlags |= DDSD_LPSURFACE;
    if (v >= 4) { DWORD sz = ((DDSURFACEDESC2 *)desc)->dwSize; *(DDSURFACEDESC2 *)desc = d; ((DDSURFACEDESC2 *)desc)->dwSize = sz ? sz : sizeof d; }
    else desc2_to_desc1(&d, (DDSURFACEDESC *)desc);
    return DD_OK;
}
static HRESULT WINAPI s_Unlock(void *This, void *r)
{
    Surf *s = SELF(Surf, This);
    (void)r;
    if (s->virt && s->shadow) {
        target_t t; int x, y; fmt_t pf; DWORD rm, gm, bm; int bpp = display_bpp(s->dd); bpp = bpp == 15 ? 16 : bpp;
        if (surf_target(s, &t)) {
            default_masks(display_bpp(s->dd), &rm, &gm, &bm);
            make_fmt(&pf, bpp, rm, gm, bm, surf_pal(s));
            for (y = 0; y < t.h; y++) {
                int sy = y - t.oy; DWORD *crow = (DWORD *)(t.bits + (size_t)y * t.pitch);
                if (sy < 0 || sy >= s->sh_h) continue;
                for (x = 0; x < t.w; x++) {
                    int sx = x - t.ox;
                    if (sx < 0 || sx >= s->sh_w) continue;
                    crow[x] = raw_to_rgb(&pf, get_px(s->shadow + (size_t)sy * s->sh_pitch, sx, bpp));
                }
            }
        }
    }
    s->uniq++;
    if (s->primary) surf_present(s, 1);
    return DD_OK;
}
static HRESULT WINAPI s_Restore(void *This) { (void)This; return DD_OK; }
static HRESULT WINAPI s_SetClipper(void *This, void *cif)
{
    Surf *s = SELF(Surf, This);
    Clip *c = cif ? SELF(Clip, cif) : NULL;
    if (c) c->ref++;
    if (s->clip) s->clip->ref--;
    s->clip = c;
    return DD_OK;
}
static HRESULT WINAPI s_SetColorKey(void *This, DWORD fl, DDCOLORKEY *ck)
{
    Surf *s = SELF(Surf, This);
    DWORD which = fl & (DDCKEY_SRCBLT | DDCKEY_DESTBLT | DDCKEY_SRCOVERLAY | DDCKEY_DESTOVERLAY);
    if (which & DDCKEY_SRCBLT) {
        if (ck) { s->ck_src = *ck; if (!(fl & DDCKEY_COLORSPACE)) s->ck_src.dwColorSpaceHighValue = ck->dwColorSpaceLowValue; s->ckflags |= DDCKEY_SRCBLT; }
        else s->ckflags &= ~DDCKEY_SRCBLT;
    }
    if (which & DDCKEY_DESTBLT) {
        if (ck) { s->ck_dst = *ck; s->ckflags |= DDCKEY_DESTBLT; }
        else s->ckflags &= ~DDCKEY_DESTBLT;
    }
    return DD_OK;
}
static HRESULT WINAPI s_SetOverlayPosition(void *This, LONG x, LONG y) { (void)This; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_SetPalette(void *This, void *pif)
{
    mkw_log("SetPalette %p primary=%d\n", This, SELF(Surf, This)->primary);
    Surf *s = SELF(Surf, This);
    Pal *p = pif ? SELF(Pal, pif) : NULL;
    if (p) p->ref++;
    if (s->pal) s->pal->ref--;
    s->pal = p;
    surf_sync_palette(s);
    if (s->primary) {
        Surf *b = s->back; while (b && b != s) { surf_sync_palette(b); b = b->back; }
        if (!s->virt) surf_present(s, 0);
    }
    return DD_OK;
}
static HRESULT WINAPI s_UpdateOverlay(void *This, RECT *a, void *b, RECT *c, DWORD d, void *e) { (void)This; (void)a; (void)b; (void)c; (void)d; (void)e; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlayDisplay(void *This, DWORD fl) { (void)This; (void)fl; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlayZOrder(void *This, DWORD fl, void *r) { (void)This; (void)fl; (void)r; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_GetDDInterface(void *This, void **out)
{
    Surf *s = SELF(Surf, This);
    s->dd->ref++; *out = dd_iface(s->dd, 1); return DD_OK;
}
static HRESULT WINAPI s_PageLock(void *This, DWORD fl) { (void)This; (void)fl; return DD_OK; }
/* アプリが用意したメモリをサーフェスの実体にする (Gens 系エミュレータが使う) */
static HRESULT WINAPI s_SetSurfaceDesc(void *This, void *desc, DWORD fl)
{
    Surf *s = SELF(Surf, This);
    int v = ((IFACE *)This)->ver;
    DDSURFACEDESC2 d;
    int bpp = s->bpp; DWORD rm = s->rmask, gm = s->gmask, bm = s->bmask;
    (void)fl;
    if (!desc) return DDERR_INVALIDPARAMS;
    if (v >= 4) d = *(DDSURFACEDESC2 *)desc;
    else desc1_to_desc2((DDSURFACEDESC *)desc, &d);
    if (s->primary || s->virt) return DDERR_INVALIDSURFACETYPE;
    if (!(d.dwFlags & DDSD_LPSURFACE) || !d.lpSurface) return DDERR_INVALIDPARAMS;
    if (d.dwFlags & DDSD_PIXELFORMAT) {
        bpp = (int)d.ddpfPixelFormat.dwRGBBitCount;
        rm = d.ddpfPixelFormat.dwRBitMask; gm = d.ddpfPixelFormat.dwGBitMask; bm = d.ddpfPixelFormat.dwBBitMask;
        if (bpp == 16 && !rm) { rm = 0xF800; gm = 0x07E0; bm = 0x001F; }
        if (bpp == 32 && !rm) { rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; }
    }
    /* 自前の DIB を解放してアプリのメモリに切り替える */
    if (s->dc) { SelectObject(s->dc, s->oldbmp); DeleteDC(s->dc); s->dc = NULL; }
    if (s->bmp) { DeleteObject(s->bmp); s->bmp = NULL; }
    if (s->xdc) { SelectObject(s->xdc, s->xold); DeleteDC(s->xdc); s->xdc = NULL; }
    if (s->xbmp) { DeleteObject(s->xbmp); s->xbmp = NULL; }
    if (d.dwFlags & DDSD_PITCH) {
        /* ピッチが形式より狭い (本物の DirectDraw なら失敗する) 場合はピッチから推定 (16bpp = RGB565) */
        int w0 = (d.dwFlags & DDSD_WIDTH) ? (int)d.dwWidth : s->w;
        if (w0 > 0 && d.lPitch > 0 && d.lPitch < w0 * bpp / 8) {
            int nb = (int)(d.lPitch * 8 / w0);
            if (nb >= 16 && nb < 24) { bpp = 16; rm = 0xF800; gm = 0x07E0; bm = 0x001F; }
            else if (nb >= 8 && nb < 16) bpp = 8;
        }
    }
    if (d.dwFlags & DDSD_WIDTH) s->w = (int)d.dwWidth;
    if (d.dwFlags & DDSD_HEIGHT) s->h = (int)d.dwHeight;
    s->bpp = bpp; s->rmask = rm; s->gmask = gm; s->bmask = bm;
    s->pitch = (d.dwFlags & DDSD_PITCH) ? (int)d.lPitch : s->w * bpp / 8;
    s->bits = (BYTE *)d.lpSurface;
    s->ext = 1;
    s->uniq++;
    { static int n; if (g_cfg.log && n < 4) { n++; mkw_log("SetSurfaceDesc %dx%d %dbpp pitch=%d flags=%lx pf=%lx/%lu %lx (アプリのメモリ)\n", s->w, s->h, s->bpp, s->pitch, (unsigned long)d.dwFlags, (unsigned long)d.ddpfPixelFormat.dwFlags, (unsigned long)d.ddpfPixelFormat.dwRGBBitCount, (unsigned long)d.ddpfPixelFormat.dwRBitMask); } }
    return DD_OK;
}
static HRESULT WINAPI s_SetPrivateData(void *This, REFGUID g, void *d, DWORD n, DWORD fl) { (void)This; (void)g; (void)d; (void)n; (void)fl; return DD_OK; }
static HRESULT WINAPI s_GetPrivateData(void *This, REFGUID g, void *d, DWORD *n) { (void)This; (void)g; (void)d; (void)n; return DDERR_NOTFOUND; }
static HRESULT WINAPI s_FreePrivateData(void *This, REFGUID g) { (void)This; (void)g; return DD_OK; }
static HRESULT WINAPI s_GetUniquenessValue(void *This, DWORD *v) { *v = SELF(Surf, This)->uniq; return DD_OK; }
static HRESULT WINAPI s_ChangeUniquenessValue(void *This) { SELF(Surf, This)->uniq++; return DD_OK; }
static HRESULT WINAPI s_SetPriority(void *This, DWORD p) { (void)This; (void)p; return DD_OK; }
static HRESULT WINAPI s_GetPriority(void *This, DWORD *p) { (void)This; *p = 0; return DD_OK; }

static void *s_vt_all[] = {
    s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt, s_BltBatch, s_BltFast,
    s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip, s_GetAttachedSurface, s_GetBltStatus,
    s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus, s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat,
    s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
    s_SetOverlayPosition, s_SetPalette, s_Unlock, s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder,
    /* v2 */ s_GetDDInterface, s_PageLock, s_PageLock,
    /* v3 */ s_SetSurfaceDesc,
    /* v4 */ s_SetPrivateData, s_GetPrivateData, s_FreePrivateData, s_GetUniquenessValue, s_ChangeUniquenessValue,
    /* v7 */ s_SetPriority, s_GetPriority, s_SetPriority, s_GetPriority,
};

/* ================= IDirectDraw ================= */
static HRESULT create_surface(DDraw *dd, DDSURFACEDESC2 *d, Surf **out)
{
    Surf *s = new_surf(dd);
    DWORD caps = d->ddsCaps.dwCaps;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->primary = 1;
        s->caps = caps | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        if (dd->mode_w && (dd->coop & DDSCL_EXCLUSIVE)) {
            int bpp = dd->mode_bpp ? dd->mode_bpp : 16; DWORD r, g, b;
            default_masks(bpp, &r, &g, &b);
            if (!surf_alloc(s, dd->mode_w, dd->mode_h, bpp, r, g, b)) { free(s); return DDERR_OUTOFMEMORY; }
            if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount > 0) {
                Surf *prev = s; DWORD i;
                for (i = 0; i < d->dwBackBufferCount && i < 3; i++) {
                    Surf *b2 = new_surf(dd);
                    surf_alloc(b2, dd->mode_w, dd->mode_h, bpp, r, g, b);
                    b2->caps = DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
                    if (i > 0) b2->caps &= ~DDSCAPS_BACKBUFFER;
                    prev->back = b2; b2->front = s;
                    prev = b2;
                }
                s->caps |= DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            }
            vwin_t *w = surf_win(s);
            if (w) vwin_ensure_canvas(w, dd->mode_w, dd->mode_h);
        } else {
            s->virt = 1;
            s->w = GetSystemMetrics(SM_CXSCREEN); s->h = GetSystemMetrics(SM_CYSCREEN);
            s->bpp = display_bpp(dd) == 15 ? 16 : display_bpp(dd);
            if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount > 0) {
                /* 窓モードでフリップを要求された場合: 画面サイズのバックバッファを作る */
                Surf *b2 = new_surf(dd); DWORD r, g, b;
                default_masks(s->bpp, &r, &g, &b);
                surf_alloc(b2, s->w, s->h, s->bpp, r, g, b);
                b2->caps = DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
                s->back = b2; b2->front = s;
            }
        }
        dd->primary = s;
        mkw_log("CreateSurface primary %s %dx%d %dbpp back=%d\n", s->virt ? "(窓)" : "(全画面)", s->w, s->h, s->bpp, s->back != NULL);
    } else {
        int w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : 1, h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : 1;
        int bpp; DWORD r, g, b;
        if ((d->dwFlags & DDSD_PIXELFORMAT) && (d->ddpfPixelFormat.dwFlags & (DDPF_RGB | DDPF_PALETTEINDEXED8))) {
            bpp = d->ddpfPixelFormat.dwRGBBitCount;
            r = d->ddpfPixelFormat.dwRBitMask; g = d->ddpfPixelFormat.dwGBitMask; b = d->ddpfPixelFormat.dwBBitMask;
            if (d->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) bpp = 8;
            if (bpp != 8 && !r) default_masks(bpp, &r, &g, &b);
        } else {
            bpp = display_bpp(dd);
            default_masks(bpp, &r, &g, &b);
        }
        if (bpp == 15) bpp = 16;
        if (caps & (DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY)) { free(s); return DDERR_INVALIDCAPS; }
        if (!surf_alloc(s, w, h, bpp, r, g, b)) { free(s); return DDERR_OUTOFMEMORY; }
        s->caps = (caps ? caps : DDSCAPS_OFFSCREENPLAIN);
        if (!(s->caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) s->caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        mkw_log("CreateSurface %dx%d %dbpp caps=%lx\n", w, h, bpp, (unsigned long)s->caps);
    }
    if (d->dwFlags & DDSD_CKSRCBLT) { s->ck_src = d->ddckCKSrcBlt; s->ckflags |= DDCKEY_SRCBLT; }
    if (d->dwFlags & DDSD_CKDESTBLT) { s->ck_dst = d->ddckCKDestBlt; s->ckflags |= DDCKEY_DESTBLT; }
    *out = s;
    return DD_OK;
}

static HRESULT WINAPI dd_QueryInterface(void *This, REFIID iid, void **out)
{
    DDraw *d = SELF(DDraw, This);
    int v = 0;
    if (IsEqualGUID(iid, &IID_IUnknown)) v = ((IFACE *)This)->ver;
    else if (IsEqualGUID(iid, &IID_IDirectDraw)) v = 1;
    else if (IsEqualGUID(iid, &IID_IDirectDraw2)) v = 2;
    else if (IsEqualGUID(iid, &IID_IDirectDraw4)) v = 4;
    else if (IsEqualGUID(iid, &IID_IDirectDraw7)) v = 7;
    if (!v) { *out = NULL; mkw_log("IDirectDraw::QueryInterface 未対応 IID\n"); return E_NOINTERFACE; }
    d->ref++; *out = dd_iface(d, v);
    return S_OK;
}
static ULONG WINAPI dd_AddRef(void *This) { return ++SELF(DDraw, This)->ref; }
static ULONG WINAPI dd_Release(void *This)
{
    DDraw *d = SELF(DDraw, This);
    LONG r = --d->ref;
    if (r <= 0) {
        if (d->mode_w) mode_change_fake(0, 0, 0);
        if (g_dd == d) g_dd = NULL;
        free(d); return 0;
    }
    return r;
}
static HRESULT WINAPI dd_Compact(void *This) { (void)This; return DD_OK; }
static HRESULT WINAPI dd_CreateClipper(void *This, DWORD fl, void **out, IUnknown *o)
{
    (void)This; (void)fl; (void)o;
    *out = &new_clip()->i; return DD_OK;
}
static HRESULT WINAPI dd_CreatePalette(void *This, DWORD fl, PALETTEENTRY *e, void **out, IUnknown *o)
{
    Pal *p = (Pal *)calloc(1, sizeof *p);
    (void)o;
    p->i.vtbl = vt_pal; p->i.self = p; p->ref = 1; p->caps = fl; p->dd = SELF(DDraw, This);
    if (e) memcpy(p->e, e, ((fl & DDPCAPS_8BIT) ? 256 : (fl & DDPCAPS_4BIT) ? 16 : 256) * sizeof(PALETTEENTRY));
    *out = &p->i;
    return DD_OK;
}
static HRESULT WINAPI dd_CreateSurface(void *This, void *desc, void **out, IUnknown *o)
{
    DDraw *d = SELF(DDraw, This);
    int v = ((IFACE *)This)->ver;
    DDSURFACEDESC2 d2; Surf *s; HRESULT hr;
    (void)o;
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    if (v >= 4) { memset(&d2, 0, sizeof d2); memcpy(&d2, desc, ((DDSURFACEDESC2 *)desc)->dwSize < sizeof d2 ? ((DDSURFACEDESC2 *)desc)->dwSize : sizeof d2); }
    else desc1_to_desc2((DDSURFACEDESC *)desc, &d2);
    hr = create_surface(d, &d2, &s);
    if (FAILED(hr)) { *out = NULL; return hr; }
    *out = s_iface(s, v == 2 ? 1 : v); /* IDirectDraw2::CreateSurface は IDirectDrawSurface を返す */
    return DD_OK;
}
static HRESULT WINAPI dd_DuplicateSurface(void *This, void *src, void **out)
{
    DDraw *d = SELF(DDraw, This); Surf *s = SELF(Surf, src), *n;
    int v = ((IFACE *)src)->ver;
    if (s->primary) return DDERR_CANTDUPLICATE;
    n = new_surf(d);
    surf_alloc(n, s->w, s->h, s->bpp, s->rmask, s->gmask, s->bmask);
    memcpy(n->bits, s->bits, (size_t)s->pitch * s->h);
    n->caps = s->caps; n->ck_src = s->ck_src; n->ck_dst = s->ck_dst; n->ckflags = s->ckflags;
    if (s->pal) { n->pal = s->pal; n->pal->ref++; }
    *out = s_iface(n, v);
    return DD_OK;
}
static const int g_modes[][2] = { { 320, 200 }, { 320, 240 }, { 512, 384 }, { 640, 400 }, { 640, 480 }, { 800, 600 }, { 1024, 768 } };
static HRESULT WINAPI dd_EnumDisplayModes(void *This, DWORD fl, void *desc, void *ctx, void *cb)
{
    int v = ((IFACE *)This)->ver;
    int bpps[3] = { 8, 16, 32 }, i, j;
    (void)fl;
    for (j = 0; j < 3; j++) for (i = 0; i < (int)(sizeof g_modes / sizeof g_modes[0]); i++) {
        DDSURFACEDESC2 d; DWORD r, g, b;
        if (desc) {
            DDSURFACEDESC2 *f = (DDSURFACEDESC2 *)desc;
            if ((f->dwFlags & DDSD_WIDTH) && f->dwWidth != (DWORD)g_modes[i][0]) continue;
            if ((f->dwFlags & DDSD_HEIGHT) && f->dwHeight != (DWORD)g_modes[i][1]) continue;
            if ((f->dwFlags & DDSD_PIXELFORMAT) && f->ddpfPixelFormat.dwRGBBitCount != (DWORD)bpps[j]) continue;
        }
        memset(&d, 0, sizeof d);
        d.dwSize = v >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = g_modes[i][0]; d.dwHeight = g_modes[i][1]; d.lPitch = g_modes[i][0] * bpps[j] / 8;
        d.dwRefreshRate = 60;
        default_masks(bpps[j], &r, &g, &b);
        fill_pf(&d.ddpfPixelFormat, bpps[j], r, g, b);
        if (v >= 4) { if (((LPDDENUMMODESCALLBACK2)cb)(&d, ctx) == DDENUMRET_CANCEL) return DD_OK; }
        else { DDSURFACEDESC d1; d1.dwSize = sizeof d1; desc2_to_desc1(&d, &d1); d1.dwRefreshRate = 60; d1.dwFlags |= DDSD_REFRESHRATE;
               if (((LPDDENUMMODESCALLBACK)cb)(&d1, ctx) == DDENUMRET_CANCEL) return DD_OK; }
    }
    return DD_OK;
}
static HRESULT WINAPI dd_EnumSurfaces(void *This, DWORD fl, void *desc, void *ctx, void *cb) { (void)This; (void)fl; (void)desc; (void)ctx; (void)cb; return DD_OK; }
static HRESULT WINAPI dd_FlipToGDISurface(void *This) { (void)This; return DD_OK; }
static void fill_caps(DDCAPS *c)
{
    DWORD sz = c->dwSize;
    if (sz < sizeof(DWORD) || sz > sizeof(DDCAPS)) sz = sizeof(DDCAPS);
    memset(c, 0, sz);
    c->dwSize = sz;
    c->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_CANBLTSYSMEM | DDCAPS_BLTDEPTHFILL;
    c->dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED;
    c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    c->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN;
    c->dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
    c->dwVidMemTotal = 64 * 1024 * 1024; c->dwVidMemFree = 60 * 1024 * 1024;
    c->ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN |
                        DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_VISIBLE | DDSCAPS_LOCALVIDMEM;
}
static HRESULT WINAPI dd_GetCaps(void *This, DDCAPS *hal, DDCAPS *hel)
{
    (void)This;
    if (hal) fill_caps(hal);
    if (hel) fill_caps(hel);
    return DD_OK;
}
static HRESULT WINAPI dd_GetDisplayMode(void *This, void *desc)
{
    DDraw *d = SELF(DDraw, This);
    int v = ((IFACE *)This)->ver;
    DDSURFACEDESC2 s; DWORD r, g, b; int bpp = display_bpp(d) == 15 ? 16 : display_bpp(d);
    memset(&s, 0, sizeof s); s.dwSize = sizeof s;
    s.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    s.dwWidth = d->mode_w ? d->mode_w : GetSystemMetrics(SM_CXSCREEN);
    s.dwHeight = d->mode_h ? d->mode_h : GetSystemMetrics(SM_CYSCREEN);
    s.lPitch = s.dwWidth * bpp / 8; s.dwRefreshRate = 60;
    default_masks(display_bpp(d), &r, &g, &b);
    fill_pf(&s.ddpfPixelFormat, bpp, r, g, b);
    if (v >= 4) { *(DDSURFACEDESC2 *)desc = s; }
    else desc2_to_desc1(&s, (DDSURFACEDESC *)desc);
    return DD_OK;
}
static HRESULT WINAPI dd_GetFourCCCodes(void *This, DWORD *n, DWORD *codes) { (void)This; (void)codes; *n = 0; return DD_OK; }
static HRESULT WINAPI dd_GetGDISurface(void *This, void **out)
{
    DDraw *d = SELF(DDraw, This);
    int v = ((IFACE *)This)->ver;
    if (!d->primary) return DDERR_NOTFOUND;
    d->primary->ref++; *out = s_iface(d->primary, v >= 4 ? v : 1);
    return DD_OK;
}
static HRESULT WINAPI dd_GetMonitorFrequency(void *This, DWORD *f)
{
    (void)This;
    *f = g_cfg.refresh > 0 ? (DWORD)(g_cfg.refresh + 0.5) : 60;
    return DD_OK;
}
static double vb_period(void) { return 1.0 / (g_cfg.refresh > 0 ? g_cfg.refresh : 60.0); }
static HRESULT WINAPI dd_GetScanLine(void *This, DWORD *line)
{
    double t = mkw_now(), p = vb_period(), ph = t / p - (double)(long long)(t / p);
    (void)This;
    *line = (DWORD)(ph * 525);
    if (*line >= 480) return DDERR_VERTICALBLANKINPROGRESS;
    return DD_OK;
}
static HRESULT WINAPI dd_GetVerticalBlankStatus(void *This, BOOL *b)
{
    double t = mkw_now(), p = vb_period(), ph = t / p - (double)(long long)(t / p);
    (void)This;
    *b = ph > 480.0 / 525.0;
    return DD_OK;
}
static HRESULT WINAPI dd_Initialize(void *This, GUID *g) { (void)This; (void)g; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI dd_RestoreDisplayMode(void *This)
{
    DDraw *d = SELF(DDraw, This);
    if (d->mode_w) { d->mode_w = d->mode_h = d->mode_bpp = 0; mode_change_fake(0, 0, 0); }
    return DD_OK;
}
static HRESULT WINAPI dd_SetCooperativeLevel(void *This, HWND h, DWORD fl)
{
    DDraw *d = SELF(DDraw, This);
    mkw_log("SetCooperativeLevel %p %lx\n", (void *)h, (unsigned long)fl);
    d->hwnd = h; d->coop = fl;
    if ((fl & DDSCL_FULLSCREEN) && h) {
        HWND top = GetAncestor(h, GA_ROOT); if (!top) top = h;
        d->hwnd = top;
        vwin_register_ext(top);
    }
    return DD_OK;
}
static HRESULT set_mode(DDraw *d, DWORD w, DWORD h, DWORD bpp)
{
    mkw_log("SetDisplayMode %lux%lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    d->mode_w = w; d->mode_h = h; d->mode_bpp = bpp;
    mode_change_fake2(w, h, bpp, (d->coop & DDSCL_FULLSCREEN) ? d->hwnd : NULL);
    return DD_OK;
}
static HRESULT WINAPI dd_SetDisplayMode1(void *This, DWORD w, DWORD h, DWORD bpp) { return set_mode(SELF(DDraw, This), w, h, bpp); }
static HRESULT WINAPI dd_SetDisplayMode2(void *This, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD fl) { (void)rate; (void)fl; return set_mode(SELF(DDraw, This), w, h, bpp); }
static HRESULT WINAPI dd_WaitForVerticalBlank(void *This, DWORD fl, HANDLE ev)
{
    double p = vb_period(), t = mkw_now(), next;
    (void)This; (void)ev;
    if (fl == DDWAITVB_BLOCKBEGINEVENT) return DDERR_UNSUPPORTED;
    if (g_cfg.log) { static int n; static double t0; n++; if (t - t0 >= 5.0) { mkw_log("WaitForVerticalBlank %.2f/s (flags=%lx)\n", n / (t - t0), (unsigned long)fl); n = 0; t0 = t; } }
    next = ((long long)(t / p) + 1) * p;
    while (mkw_now() < next - 0.002) Sleep(1);
    while (mkw_now() < next) {}
    return DD_OK;
}
static HRESULT WINAPI dd_GetAvailableVidMem(void *This, void *caps, DWORD *total, DWORD *fr)
{
    (void)This; (void)caps;
    if (total) *total = 64 * 1024 * 1024;
    if (fr) *fr = 60 * 1024 * 1024;
    return DD_OK;
}
static HRESULT WINAPI dd_GetSurfaceFromDC(void *This, HDC dc, void **out) { (void)This; (void)dc; *out = NULL; return DDERR_NOTFOUND; }
static HRESULT WINAPI dd_RestoreAllSurfaces(void *This) { (void)This; return DD_OK; }
static HRESULT WINAPI dd_TestCooperativeLevel(void *This) { (void)This; return DD_OK; }
static HRESULT WINAPI dd_GetDeviceIdentifier(void *This, DDDEVICEIDENTIFIER2 *id, DWORD fl)
{
    (void)This; (void)fl;
    memset(id, 0, sizeof(DDDEVICEIDENTIFIER));
    strcpy(id->szDriver, "mkwrap"); strcpy(id->szDescription, "MKWrap DirectDraw Emulation");
    return DD_OK;
}
static HRESULT WINAPI dd_StartModeTest(void *This, void *a, DWORD b, DWORD c) { (void)This; (void)a; (void)b; (void)c; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dd_EvaluateMode(void *This, DWORD a, DWORD *b) { (void)This; (void)a; (void)b; return DDERR_UNSUPPORTED; }

static void *dd1_vt[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
    dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode,
    dd_SetCooperativeLevel, dd_SetDisplayMode1, dd_WaitForVerticalBlank };
static void *dd2_vt[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
    dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode,
    dd_SetCooperativeLevel, dd_SetDisplayMode2, dd_WaitForVerticalBlank, dd_GetAvailableVidMem };
static void *dd4_vt[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
    dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode,
    dd_SetCooperativeLevel, dd_SetDisplayMode2, dd_WaitForVerticalBlank, dd_GetAvailableVidMem,
    dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier };
static void *dd7_vt[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
    dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode,
    dd_SetCooperativeLevel, dd_SetDisplayMode2, dd_WaitForVerticalBlank, dd_GetAvailableVidMem,
    dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier, dd_StartModeTest, dd_EvaluateMode };

static void init_vtables(void)
{
    static int done;
    if (done) return;
    done = 1;
    vt_dd[0] = dd1_vt; vt_dd[1] = dd2_vt; vt_dd[2] = dd4_vt; vt_dd[3] = dd7_vt;
    vt_s[0] = vt_s[1] = vt_s[2] = vt_s[3] = vt_s[4] = s_vt_all; /* 後方拡張なので同じ表で足りる */
    vt_pal = pal_vt; vt_clip = clip_vt;
}

static DDraw *new_dd(void)
{
    DDraw *d = (DDraw *)calloc(1, sizeof *d);
    int i, v[4] = { 1, 2, 4, 7 };
    init_vtables();
    for (i = 0; i < 4; i++) { d->i[i].vtbl = vt_dd[i]; d->i[i].self = d; d->i[i].ver = v[i]; }
    d->ref = 1;
    g_dd = d;
    return d;
}

HRESULT WINAPI mk_DirectDrawCreate(GUID *g, void **pp, void *outer)
{
    (void)g; (void)outer;
    if (!pp) return DDERR_INVALIDPARAMS;
    *pp = dd_iface(new_dd(), 1);
    mkw_log("DirectDrawCreate -> エミュレーション\n");
    return DD_OK;
}
HRESULT WINAPI mk_DirectDrawCreateEx(GUID *g, void **pp, REFIID iid, void *outer)
{
    (void)g; (void)outer; (void)iid;
    if (!pp) return DDERR_INVALIDPARAMS;
    *pp = dd_iface(new_dd(), 7);
    mkw_log("DirectDrawCreateEx -> エミュレーション\n");
    return DD_OK;
}
HRESULT WINAPI mk_DirectDrawEnumerateA(void *cb, void *ctx)
{
    ((LPDDENUMCALLBACKA)cb)(NULL, "Primary Display Driver", "display", ctx);
    return DD_OK;
}
HRESULT WINAPI mk_DirectDrawEnumerateExA(void *cb, void *ctx, DWORD fl)
{
    (void)fl;
    ((LPDDENUMCALLBACKEXA)cb)(NULL, "Primary Display Driver", "display", ctx, NULL);
    return DD_OK;
}

/* ---- ddraw.dll として置かれた場合のエクスポート ---- */
HRESULT WINAPI exp_DirectDrawCreate(GUID *g, void **pp, void *o) { return mk_DirectDrawCreate(g, pp, o); }
HRESULT WINAPI exp_DirectDrawCreateEx(GUID *g, void **pp, REFIID iid, void *o) { return mk_DirectDrawCreateEx(g, pp, iid, o); }
HRESULT WINAPI exp_DirectDrawEnumerateA(void *cb, void *ctx) { return mk_DirectDrawEnumerateA(cb, ctx); }
HRESULT WINAPI exp_DirectDrawEnumerateExA(void *cb, void *ctx, DWORD fl) { return mk_DirectDrawEnumerateExA(cb, ctx, fl); }
HRESULT WINAPI exp_DirectDrawEnumerateW(void *cb, void *ctx)
{
    ((BOOL (WINAPI *)(GUID *, LPWSTR, LPWSTR, LPVOID))cb)(NULL, L"Primary Display Driver", L"display", ctx);
    return DD_OK;
}
HRESULT WINAPI exp_DirectDrawEnumerateExW(void *cb, void *ctx, DWORD fl)
{
    (void)fl;
    ((BOOL (WINAPI *)(GUID *, LPWSTR, LPWSTR, LPVOID, HMONITOR))cb)(NULL, L"Primary Display Driver", L"display", ctx, NULL);
    return DD_OK;
}
HRESULT WINAPI exp_DirectDrawCreateClipper(DWORD fl, void **out, void *o) { (void)fl; (void)o; *out = &new_clip()->i; return DD_OK; }
void WINAPI exp_AcquireDDThreadLock(void) {}
void WINAPI exp_ReleaseDDThreadLock(void) {}
