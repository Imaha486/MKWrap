/* 窓の仮想化と拡大表示
 * ゲームには元のサイズ (論理サイズ) の窓に見せかけ、実際の窓は拡大する。
 * GDI の描画先 DC は論理サイズのキャンバス (メモリ DC) に差し替え、転送時に拡大表示する。 */
#include "mkw.h"
#include <string.h>
#include <stdlib.h>

#define MAXWIN 8
#define MKW_TIMER 0x4D4B
#define WM_MKW_PRESENT (WM_APP + 0x3D4B)
static vwin_t g_wins[MAXWIN];
static int g_nwins;
vwin_t *g_main;
int g_fake_mode_w, g_fake_mode_h, g_fake_mode_bpp;
static CRITICAL_SECTION g_cs;
static int g_mode_fs; /* 画面モード変更により擬似フルスクリーンにした */

/* 本物の API */
static HWND (WINAPI *r_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HWND (WINAPI *r_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *r_MoveWindow)(HWND, int, int, int, int, BOOL);
static BOOL (WINAPI *r_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static BOOL (WINAPI *r_GetClientRect)(HWND, LPRECT);
static BOOL (WINAPI *r_GetWindowRect)(HWND, LPRECT);
static HDC (WINAPI *r_GetDC)(HWND);
static HDC (WINAPI *r_GetDCEx)(HWND, HRGN, DWORD);
static int (WINAPI *r_ReleaseDC)(HWND, HDC);
static HDC (WINAPI *r_BeginPaint)(HWND, LPPAINTSTRUCT);
static BOOL (WINAPI *r_EndPaint)(HWND, const PAINTSTRUCT *);
static int (WINAPI *r_GetSystemMetrics)(int);
static LONG (WINAPI *r_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
static LONG (WINAPI *r_ChangeDisplaySettingsW)(DEVMODEW *, DWORD);
static BOOL (WINAPI *r_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA *);
static HMENU (WINAPI *r_GetMenu)(HWND);
static BOOL (WINAPI *r_SetMenu)(HWND, HMENU);
static LONG (WINAPI *r_SetWindowLongA)(HWND, int, LONG);
static LONG (WINAPI *r_GetWindowLongA)(HWND, int);
static BOOL (WINAPI *r_ShowWindow)(HWND, int);
static BOOL (WINAPI *r_InvalidateRect)(HWND, const RECT *, BOOL);
static BOOL (WINAPI *r_ClientToScreen)(HWND, LPPOINT);
static BOOL (WINAPI *r_ScreenToClient)(HWND, LPPOINT);
static BOOL (WINAPI *r_DrawMenuBar)(HWND);
static int (WINAPI *r_StretchDIBits)(HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *, UINT, DWORD);
static int (WINAPI *r_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void *, const BITMAPINFO *, UINT);
static BOOL (WINAPI *r_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BOOL (WINAPI *r_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);

/* ---------------- 窓の管理 ---------------- */
vwin_t *vwin_find(HWND h)
{
    int i;
    if (!h) return NULL;
    for (i = 0; i < g_nwins; i++) if (g_wins[i].hwnd == h) return &g_wins[i];
    return NULL;
}
vwin_t *vwin_main(void) { return g_main; }

int vwin_is_canvas_dc(HDC dc) { return vwin_from_canvas_dc(dc) != NULL; }
vwin_t *vwin_from_canvas_dc(HDC dc)
{
    int i;
    if (!dc) return NULL;
    for (i = 0; i < g_nwins; i++) if (g_wins[i].cdc == dc) return &g_wins[i];
    return NULL;
}

static void nc_size(HWND h, LONG style, LONG exstyle, int menu, int *ncw, int *nch, int *ox, int *oy)
{
    RECT r = { 0, 0, 100, 100 };
    (void)h;
    AdjustWindowRectEx(&r, style, menu, exstyle);
    *ncw = (r.right - r.left) - 100; *nch = (r.bottom - r.top) - 100;
    if (ox) *ox = -r.left;
    if (oy) *oy = -r.top;
}

void vwin_ensure_canvas(vwin_t *w, int cw, int ch)
{
    BITMAPINFO bi; HDC sdc;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (w->cdc && w->cw == cw && w->ch == ch) return;
    EnterCriticalSection(&g_cs);
    if (!w->cdc) {
        sdc = r_GetDC(NULL);
        w->cdc = CreateCompatibleDC(sdc);
        r_ReleaseDC(NULL, sdc);
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    {
        void *bits = NULL;
        HBITMAP nb = CreateDIBSection(w->cdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (nb) {
            HBITMAP old = (HBITMAP)SelectObject(w->cdc, nb);
            if (!w->cold) w->cold = old;
            if (w->cbmp) {
                /* 旧内容を左上に残す */
                HDC t = CreateCompatibleDC(w->cdc);
                HBITMAP to = (HBITMAP)SelectObject(t, w->cbmp);
                r_BitBlt(w->cdc, 0, 0, w->cw, w->ch, t, 0, 0, SRCCOPY);
                SelectObject(t, to); DeleteDC(t);
                DeleteObject(w->cbmp);
            } else {
                memset(bits, 0, (size_t)cw * ch * 4);
            }
            w->cbmp = nb; w->cbits = bits; w->cw = cw; w->ch = ch;
        }
    }
    LeaveCriticalSection(&g_cs);
    mkw_log("canvas %dx%d\n", cw, ch);
}

HDC vwin_canvas_dc(vwin_t *w)
{
    vwin_ensure_canvas(w, w->lw, w->lh);
    return w->cdc;
}

static double auto_scale(int lw, int lh)
{
    RECT wa; double s;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (lw <= 0 || lh <= 0) return 1;
    s = (double)((wa.right - wa.left) * 9 / 10) / lw;
    if ((double)((wa.bottom - wa.top) * 85 / 100) / lh < s) s = (double)((wa.bottom - wa.top) * 85 / 100) / lh;
    s = (double)(int)s;
    if (s < 1) s = 1;
    return s;
}

static double cur_scale(vwin_t *w)
{
    double s = w->uscale > 0 ? w->uscale : g_cfg.scale > 0 ? g_cfg.scale : auto_scale(w->lw, w->lh);
    return s;
}

/* 論理サイズ -> 実際の窓サイズを適用 */
static void apply_real_size(vwin_t *w, int x, int y, int use_pos, BOOL repaint)
{
    LONG style, ex; int ncw, nch, rw, rh; double s; RECT wa, rc;
    if (w->fullscreen) return;
    style = r_GetWindowLongA(w->hwnd, GWL_STYLE);
    ex = r_GetWindowLongA(w->hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) return;
    if (IsZoomed(w->hwnd)) return;   /* 最大化中はそのまま */
    /* 枠付きの窓をマウスで大きさ変更・最大化できるようにする。ゲームには元のスタイルを見せる */
    if (g_cfg.resizable && !w->framed && (style & WS_CAPTION) == WS_CAPTION &&
        (style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) != (WS_THICKFRAME | WS_MAXIMIZEBOX)) {
        w->saved_style = style; w->saved_exstyle = ex;
        w->framed = 1;
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        w->in_size++;
        r_SetWindowLongA(w->hwnd, GWL_STYLE, style);
        r_SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        w->in_size--;
        mkw_log("窓の大きさを変更可能にしました\n");
    }
    s = cur_scale(w);
    rw = (int)(w->lw * s + 0.5); rh = (int)(w->lh * s + 0.5);
    if (g_cfg.aspect > 0) rw = (int)(rh * g_cfg.aspect + 0.5);
    nc_size(w->hwnd, style, ex, r_GetMenu(w->hwnd) != NULL, &ncw, &nch, NULL, NULL);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    /* 画面に入らない場合は縮小 */
    if (rw + ncw > wa.right - wa.left || rh + nch > wa.bottom - wa.top) {
        double fx = (double)(wa.right - wa.left - ncw) / rw, fy = (double)(wa.bottom - wa.top - nch) / rh;
        double f = fx < fy ? fx : fy;
        rw = (int)(rw * f); rh = (int)(rh * f);
    }
    w->in_size++;
    if (use_pos) r_SetWindowPos(w->hwnd, NULL, x, y, rw + ncw, rh + nch, SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));
    else r_SetWindowPos(w->hwnd, NULL, 0, 0, rw + ncw, rh + nch, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));
    /* メニューバーが折り返した場合などの補正 */
    r_GetClientRect(w->hwnd, &rc);
    if (rc.bottom != rh && rc.bottom > 0) {
        RECT wr; r_GetWindowRect(w->hwnd, &wr);
        r_SetWindowPos(w->hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top + (rh - rc.bottom), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    /* 画面外に出ないように */
    {
        RECT wr; int nx, ny;
        r_GetWindowRect(w->hwnd, &wr);
        nx = wr.left; ny = wr.top;
        if (wr.right > wa.right) nx = wa.right - (wr.right - wr.left);
        if (wr.bottom > wa.bottom) ny = wa.bottom - (wr.bottom - wr.top);
        if (nx < wa.left) nx = wa.left;
        if (ny < wa.top) ny = wa.top;
        if (nx != wr.left || ny != wr.top) r_SetWindowPos(w->hwnd, NULL, nx, ny, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    w->in_size--;
}

static LRESULT CALLBACK vwin_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

static vwin_t *vwin_register(HWND h, int lw, int lh)
{
    vwin_t *w; RECT rc;
    if (g_nwins >= MAXWIN) return NULL;
    w = &g_wins[g_nwins++];
    memset(w, 0, sizeof *w);
    w->hwnd = h;
    if (lw > 0 && lh > 0) { w->lw = lw; w->lh = lh; }
    else { r_GetClientRect(h, &rc); w->lw = rc.right; w->lh = rc.bottom; }
    w->orig_proc = (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
    SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)vwin_proc);
    if (!g_main) g_main = w;
    w->tid = GetWindowThreadProcessId(h, NULL);
    SetTimer(h, MKW_TIMER, 15, NULL);
    mkw_log("窓を登録 %p 論理 %dx%d\n", (void *)h, w->lw, w->lh);
    return w;
}

/* ---------------- 表示 ---------------- */
static void ensure_timer(vwin_t *w) { (void)w; } /* タイマーは登録時に作成済み */
static void dest_rect(vwin_t *w, RECT *cl, RECT *d)
{
    int W = cl->right, H = cl->bottom, dw, dh;
    double ar = g_cfg.aspect > 0 ? g_cfg.aspect : (double)w->cw / w->ch;
    if (!w->fullscreen && !g_cfg.keep_aspect) { *d = *cl; return; }
    if (w->fullscreen && g_cfg.integer_scale) {
        int k = W / w->cw; if (H / w->ch < k) k = H / w->ch; if (k < 1) k = 1;
        dh = w->ch * k; dw = (int)(dh * ar + 0.5);
        if (dw > W) { dw = w->cw * k; }
    } else {
        dw = W; dh = (int)(W / ar + 0.5);
        if (dh > H) { dh = H; dw = (int)(H * ar + 0.5); }
    }
    d->left = (W - dw) / 2; d->top = (H - dh) / 2; d->right = d->left + dw; d->bottom = d->top + dh;
}

static void vwin_present_now(vwin_t *w);
void vwin_present(vwin_t *w, int frame)
{
    if (!w || !w->cdc) return;
    if (frame) {
        pause_wait_if_paused();
        if (g_cfg.log && w->cbits) {   /* 内容が変化したフレーム数を計測 */
            static DWORD lasthash; static int uniq, tot; static double t0;
            DWORD hsh = 2166136261u; int i, n = w->cw * w->ch; const DWORD *p = (const DWORD *)w->cbits;
            double now = mkw_now();
            for (i = 0; i < n; i += 7) hsh = (hsh ^ p[i]) * 16777619u;
            tot++; if (hsh != lasthash) uniq++;
            lasthash = hsh;
            if (now - t0 >= 5.0) {
                double dt = now - t0;
                LONG np = InterlockedExchange(&w->st_presents, 0), nq = InterlockedExchange(&w->st_posts, 0);
                double pt = w->st_ptime; w->st_ptime = 0;
                mkw_log("フレーム: 転送 %.2f/s, 内容変化 %.2f/s, 表示 %.1f/s, 依頼 %.1f/s (窓のスレッドの表示処理 %.0f%%)%s\n",
                        tot / dt, uniq / dt, np / dt, nq / dt, pt / dt * 100, IsWindowEnabled(w->hwnd) ? "" : " [ダイアログ表示中]");
                tot = uniq = 0; t0 = now;
            }
        }
        frame_limit();
    }
    /* 窓を作ったスレッド以外 (描画スレッドなど) からは表示しない。
     * 別スレッドから窓へ描画すると、窓のスレッドとの間でロック待ちが起き、
     * ダイアログを開いたときなどに固まる原因になるため、窓のスレッドへ依頼する */
    if (w->tid && GetCurrentThreadId() != w->tid) {
        w->dirty = 1;
        /* 依頼の間隔を空ける: 依頼が途切れずに届くと、窓のスレッドのメッセージ待ちで
         * 投稿メッセージが常に優先され、ダイアログの描画 (WM_PAINT) や入力が処理されなくなる。
         * 間引いた分は窓のタイマー (15ms) が表示する */
        if (mkw_now() < w->next_post) return;
        if (!InterlockedExchange(&w->post_pending, 1)) {
            InterlockedIncrement(&w->st_posts);
            PostMessageA(w->hwnd, WM_MKW_PRESENT, 0, 0);
        }
        return;
    }
    vwin_present_now(w);
}

/* 案内の文字をキャンバスに重ねる (表示の間だけ。戻すための元の画素を返す) */
static DWORD *overlay_draw(vwin_t *w, RECT *box)
{
    const wchar_t *t = input_overlay_text();
    DWORD *save; int y, fh; HFONT f, of; SIZE sz; RECT r;
    if (!t || !w->cbits || w->cw < 32 || w->ch < 16) return NULL;
    fh = w->ch / 16; if (fh < 10) fh = 10; if (fh > 24) fh = 24;
    f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, SHIFTJIS_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"MS Gothic");
    of = (HFONT)SelectObject(w->cdc, f);
    GetTextExtentPoint32W(w->cdc, t, (int)wcslen(t), &sz);
    r.left = 0; r.right = w->cw; r.top = 0; r.bottom = sz.cy + 4;
    if (sz.cx + 8 > w->cw) {   /* 収まらなければ折り返す */
        RECT m = { 4, 2, w->cw - 4, 2 };
        DrawTextW(w->cdc, t, -1, &m, DT_CALCRECT | DT_WORDBREAK);
        r.bottom = m.bottom + 4; if (r.bottom > w->ch) r.bottom = w->ch;
    }
    *box = r;
    save = (DWORD *)malloc((size_t)w->cw * r.bottom * 4);
    if (!save) { SelectObject(w->cdc, of); DeleteObject(f); return NULL; }
    GdiFlush();
    for (y = 0; y < r.bottom; y++) memcpy(save + (size_t)y * w->cw, (DWORD *)w->cbits + (size_t)y * w->cw, (size_t)w->cw * 4);
    FillRect(w->cdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(w->cdc, TRANSPARENT); SetTextColor(w->cdc, RGB(255, 255, 80));
    { RECT m = { 4, 2, w->cw - 4, r.bottom }; DrawTextW(w->cdc, t, -1, &m, DT_WORDBREAK | DT_NOPREFIX); }
    GdiFlush();
    SelectObject(w->cdc, of); DeleteObject(f);
    return save;
}
static void overlay_restore(vwin_t *w, DWORD *save, const RECT *box)
{
    int y;
    if (!save) return;
    GdiFlush();
    for (y = 0; y < box->bottom; y++) memcpy((DWORD *)w->cbits + (size_t)y * w->cw, save + (size_t)y * w->cw, (size_t)w->cw * 4);
    free(save);
}

static void vwin_present_now2(vwin_t *w);
static void vwin_present_now(vwin_t *w)
{
    RECT box; DWORD *save;
    if (!w || !w->cdc) return;
    EnterCriticalSection(&g_cs);
    save = overlay_draw(w, &box);
    vwin_present_now2(w);
    overlay_restore(w, save, &box);
    LeaveCriticalSection(&g_cs);
}
static void vwin_present_now2(vwin_t *w)
{
    HDC dc; RECT cl, d; HBRUSH bk;
    if (!w || !w->cdc) return;
    if (IsIconic(w->hwnd)) { w->dirty = 0; return; }
    {   /* モニタの2倍を超える頻度の転送は間引く (後でタイマーが表示) */
        static double last, minint;
        double now = mkw_now();
        if (minint == 0) {
            DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
            int hz = (r_EnumDisplaySettingsA && r_EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) ? (int)dm.dmDisplayFrequency : 60;
            minint = 1.0 / (hz * 2.0);
        }
        if (now - last < minint) { w->dirty = 1; ensure_timer(w); return; }
        last = now;
    }
    InterlockedIncrement(&w->st_presents);
    EnterCriticalSection(&g_cs);
    r_GetClientRect(w->hwnd, &cl);
    dest_rect(w, &cl, &d);
    if (g_cfg.renderer != 1 && d3d9_present(w, &cl, &d)) {
        w->dirty = 0;
        LeaveCriticalSection(&g_cs);
        return;
    }
    dc = r_GetDC(w->hwnd);
    if (dc) {
        GdiFlush();
        if (g_cfg.filter) { SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL); }
        else SetStretchBltMode(dc, COLORONCOLOR);
        r_StretchBlt(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, w->cdc, 0, 0, w->cw, w->ch, SRCCOPY);
        bk = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (d.top > 0) { RECT r = { 0, 0, cl.right, d.top }; FillRect(dc, &r, bk); }
        if (d.bottom < cl.bottom) { RECT r = { 0, d.bottom, cl.right, cl.bottom }; FillRect(dc, &r, bk); }
        if (d.left > 0) { RECT r = { 0, d.top, d.left, d.bottom }; FillRect(dc, &r, bk); }
        if (d.right < cl.right) { RECT r = { d.right, d.top, cl.right, d.bottom }; FillRect(dc, &r, bk); }
        r_ReleaseDC(w->hwnd, dc);
    }
    w->dirty = 0;
    LeaveCriticalSection(&g_cs);
}

/* 小さな部分描画: 8ms 以上経っていれば表示 (レート制限なし) */
void vwin_present_small(vwin_t *w)
{
    static double last;
    double now = mkw_now();
    w->dirty = 1;
    if (now - last < 0.008) return;
    last = now;
    vwin_present(w, 0);
}

/* ---------------- 擬似フルスクリーン ---------------- */
/* 枠なし (WS_POPUP) の窓に、表示用の枠を付ける。ゲームには元のスタイルに見せる */
static void vwin_apply_frame(vwin_t *w)
{
    MONITORINFO mi; RECT wr;
    LONG st = (w->saved_style & ~(WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    w->framed = 1;
    w->in_size++;
    r_SetWindowLongA(w->hwnd, GWL_STYLE, st);
    r_SetWindowLongA(w->hwnd, GWL_EXSTYLE, (w->saved_exstyle & ~WS_EX_TOPMOST) | WS_EX_WINDOWEDGE);
    r_SetWindowPos(w->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    w->in_size--;
    apply_real_size(w, 0, 0, 0, TRUE);
    /* モニタの中央へ */
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    r_GetWindowRect(w->hwnd, &wr);
    w->in_size++;
    r_SetWindowPos(w->hwnd, NULL, (mi.rcWork.left + mi.rcWork.right - (wr.right - wr.left)) / 2,
                   (mi.rcWork.top + mi.rcWork.bottom - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    w->in_size--;
    mkw_log("枠付きの窓で表示 %dx%d\n", w->lw, w->lh);
}

void vwin_set_fullscreen(vwin_t *w, int fs)
{
    MONITORINFO mi;
    if (!w || w->fullscreen == fs) return;
    w->in_size++;
    if (fs) {
        r_GetWindowRect(w->hwnd, &w->saved_rect);
        if (!w->framed) {   /* 枠付き表示中なら saved_style にはゲーム本来のスタイルが入っている */
            w->saved_style = r_GetWindowLongA(w->hwnd, GWL_STYLE);
            w->saved_exstyle = r_GetWindowLongA(w->hwnd, GWL_EXSTYLE);
        }
        w->menu = r_GetMenu(w->hwnd);
        w->fullscreen = 1;
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        if (g_cfg.hide_menu_fs && w->menu) r_SetMenu(w->hwnd, NULL);
        r_SetWindowLongA(w->hwnd, GWL_STYLE, (w->saved_style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE);
        r_SetWindowLongA(w->hwnd, GWL_EXSTYLE, w->saved_exstyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE));
        r_SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                       SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (g_cfg.hide_cursor_fs) while (ShowCursor(FALSE) >= 0) {}
    } else {
        int popup = !(w->saved_style & WS_CAPTION);
        w->fullscreen = 0;
        if (w->menu) r_SetMenu(w->hwnd, w->menu);
        if (g_cfg.hide_cursor_fs) while (ShowCursor(TRUE) < 0) {}
        if (popup && g_cfg.frame_popup) {
            /* 元が枠なし窓 (DirectDraw の全画面用など) → 枠を付けて窓表示 */
            w->in_size--;
            vwin_apply_frame(w);
            r_InvalidateRect(w->hwnd, NULL, TRUE);
            mkw_log("フルスクリーン 0 (枠付き)\n");
            return;
        }
        w->framed = 0;
        r_SetWindowLongA(w->hwnd, GWL_STYLE, w->saved_style);
        r_SetWindowLongA(w->hwnd, GWL_EXSTYLE, w->saved_exstyle);
        r_SetWindowPos(w->hwnd, NULL, w->saved_rect.left, w->saved_rect.top,
                       w->saved_rect.right - w->saved_rect.left, w->saved_rect.bottom - w->saved_rect.top,
                       SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        apply_real_size(w, 0, 0, 0, TRUE);
    }
    w->in_size--;
    r_InvalidateRect(w->hwnd, NULL, TRUE);
    mkw_log("フルスクリーン %d\n", fs);
}
void vwin_toggle_fullscreen(vwin_t *w) { if (w) vwin_set_fullscreen(w, !w->fullscreen); }

/* ---------------- サブクラス ---------------- */
static LRESULT CALLBACK vwin_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    vwin_t *w = vwin_find(h);
    WNDPROC op = w ? w->orig_proc : DefWindowProcA;
    LRESULT r;
    if (!w) return DefWindowProcA(h, m, wp, lp);
    switch (m) {
    case WM_SYSKEYDOWN:
        if (wp == (WPARAM)g_cfg.fs_key_vk && (lp & (1 << 29))) {
            if (!(lp & (1 << 30))) vwin_toggle_fullscreen(w);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == '\r' && g_cfg.fs_key_vk == VK_RETURN) return 0;
        break;
    case WM_KEYDOWN:
        if (wp == (WPARAM)g_cfg.pause_vk && g_cfg.pause_vk && !(lp & (1 << 30))) {
            g_paused = !g_paused;
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        w->user_sizing = 1;
        break;
    case WM_EXITSIZEMOVE:
        if (w->user_sizing && !w->fullscreen && !IsZoomed(h) && w->lw > 0 && w->lh > 0) {
            /* 利用者がマウスで大きさを変えた → 以後この倍率を使う (ゲームが窓の大きさを変えたときも保つ) */
            RECT cr; double sx, sy;
            GetClientRect(h, &cr);
            sx = (double)cr.right / w->lw; sy = (double)cr.bottom / w->lh;
            if (cr.right > 0 && cr.bottom > 0) w->uscale = sx < sy ? sx : sy;
        }
        w->user_sizing = 0;
        break;
    case WM_SIZING:
        /* 縦横比を保って大きさを変える */
        if (g_cfg.keep_aspect && !w->fullscreen && w->lw > 0 && w->lh > 0) {
            RECT *rc = (RECT *)lp, wr, cr; int ncw, nch, cw, ch;
            double ar = g_cfg.aspect > 0 ? g_cfg.aspect : (double)w->lw / w->lh;
            r_GetWindowRect(h, &wr); GetClientRect(h, &cr);   /* 実際の大きさ (フックを通さない) */
            ncw = (wr.right - wr.left) - cr.right; nch = (wr.bottom - wr.top) - cr.bottom;
            cw = rc->right - rc->left - ncw; ch = rc->bottom - rc->top - nch;
            if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) cw = (int)(ch * ar + 0.5);
            else ch = (int)(cw / ar + 0.5);
            if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) rc->right = rc->left + cw + ncw;
            else if (wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) rc->top = rc->bottom - ch - nch;
            else rc->bottom = rc->top + ch + nch;
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            lp = MAKELPARAM(w->lw, w->lh);
            r_InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_MKW_PRESENT: {
        double t0 = mkw_now(), t1, gap;
        if (w->cdc) vwin_present_now(w);
        t1 = mkw_now();
        /* 次の依頼までの間隔: 表示にかかった時間以上 (窓のスレッドの半分以上を表示に使わない)。
         * モーダルダイアログ表示中 (ゲーム窓が無効) は 1/30 秒以上空ける */
        gap = t1 - t0;
        if (gap < 0.004) gap = 0.004;
        if (!IsWindowEnabled(h) && gap < 1.0 / 30) gap = 1.0 / 30;
        w->next_post = t1 + gap;
        w->st_ptime += t1 - t0;
        InterlockedExchange(&w->post_pending, 0);
        return 0;
    }
    case WM_TIMER:
        if (wp == MKW_TIMER && g_cfg.log >= 2) {
            static int dumped;
            HMENU mm = r_GetMenu(h);
            if (!dumped && mm) {
                int i, j, n = GetMenuItemCount(mm);
                dumped = 1;
                for (i = 0; i < n; i++) {
                    char t[128] = ""; HMENU sub = GetSubMenu(mm, i); int k = sub ? GetMenuItemCount(sub) : 0;
                    GetMenuStringA(mm, i, t, 128, MF_BYPOSITION); mkw_log("MENU %s\n", t);
                    for (j = 0; j < k; j++) { t[0] = 0; GetMenuStringA(sub, j, t, 128, MF_BYPOSITION); mkw_log("MENU   %u %s\n", GetMenuItemID(sub, j), t); }
                }
            }
        }
        if (wp == MKW_TIMER) {
            /* モーダルダイアログ表示中は間引く (ダイアログの処理を優先) */
            if (w->dirty && w->cdc && (IsWindowEnabled(h) || mkw_now() >= w->next_post)) {
                vwin_present_now(w);
                if (!IsWindowEnabled(h)) w->next_post = mkw_now() + 1.0 / 30;
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        r = CallWindowProcA(op, h, m, wp, lp);
        mm->ptMaxTrackSize.x = 16384; mm->ptMaxTrackSize.y = 16384;
        mm->ptMinTrackSize.x = 64; mm->ptMinTrackSize.y = 64;
        return r;
    }
    case WM_PAINT:
        w->dirty = 1;
        r = CallWindowProcA(op, h, m, wp, lp);
        if (w->cdc) vwin_present(w, 0);
        return r;
    case WM_NCDESTROY:
        d3d9_release_window(h);
        r = CallWindowProcA(op, h, m, wp, lp);
        w->hwnd = NULL;
        return r;
    }
    return CallWindowProcA(op, h, m, wp, lp);
}

/* ---------------- フック関数 ---------------- */
static int should_virtualize(DWORD style, HWND parent, LPCSTR cls)
{
    if (style & WS_CHILD) return 0;
    if ((ULONG_PTR)cls == 32770) return 0;
    if ((ULONG_PTR)cls >= 0x10000 && !_stricmp(cls, "#32770")) return 0;
    (void)parent;
    return g_nwins < MAXWIN && g_main == NULL; /* 最初のトップレベル窓のみ */
}

/* CBT フックで WM_CREATE より前に窓を登録する (作成中の GetDC もキャンバスにするため) */
static HWND g_cbt_registered;
static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_CREATEWND && !g_main) {
        CBT_CREATEWNDA *c = (CBT_CREATEWNDA *)lp;
        CREATESTRUCTA *cs = c->lpcs;
        if (cs && should_virtualize(cs->style, cs->hwndParent, cs->lpszClass)) {
            int lw = 0, lh = 0, ncw, nch;
            if (cs->cx > 0 && cs->cx != CW_USEDEFAULT && cs->cy > 0 && cs->cy != CW_USEDEFAULT) {
                nc_size((HWND)wp, cs->style, cs->dwExStyle, cs->hMenu != NULL, &ncw, &nch, NULL, NULL);
                lw = cs->cx - ncw; lh = cs->cy - nch;
            }
            if (vwin_register((HWND)wp, lw, lh)) g_cbt_registered = (HWND)wp;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int cx, int cy,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HHOOK hk = NULL; HWND h;
    mkw_log("CreateWindowExA class=%s style=%08lx ex=%08lx %d,%d %dx%d parent=%p\n", (ULONG_PTR)cls >= 0x10000 ? cls : "#atom", (unsigned long)style, (unsigned long)ex, x, y, cx, cy, (void *)parent);
    if (!g_main) hk = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    g_cbt_registered = NULL;
    h = r_CreateWindowExA(ex, cls, name, style, x, y, cx, cy, parent, menu, inst, param);
    if (hk) UnhookWindowsHookEx(hk);
    if (h && g_cbt_registered == h) {
        vwin_t *w = vwin_find(h);
        if (w && (w->lw <= 0 || w->lh <= 0)) { RECT rc; r_GetClientRect(h, &rc); w->lw = rc.right; w->lh = rc.bottom; }
        if (w && w->lw > 0 && w->lh > 0) {
            apply_real_size(w, 0, 0, 0, FALSE);
            if (g_cfg.fullscreen && (style & WS_VISIBLE)) vwin_set_fullscreen(w, 1);
        }
    } else if (h && should_virtualize(style, parent, cls)) {
        vwin_t *w = vwin_register(h, 0, 0);
        if (w && w->lw > 0 && w->lh > 0) {
            apply_real_size(w, 0, 0, 0, FALSE);
            if (g_cfg.fullscreen && (style & WS_VISIBLE)) vwin_set_fullscreen(w, 1);
        }
    }
    return h;
}
static HWND WINAPI hk_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int cx, int cy,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND h = r_CreateWindowExW(ex, cls, name, style, x, y, cx, cy, parent, menu, inst, param);
    if (h && !(style & WS_CHILD) && !((ULONG_PTR)cls == 32770) && g_main == NULL) {
        vwin_t *w = vwin_register(h, 0, 0);
        if (w && w->lw > 0 && w->lh > 0) apply_real_size(w, 0, 0, 0, FALSE);
    }
    return h;
}

static void set_logical_from_outer(vwin_t *w, int ow, int oh)
{
    int ncw, nch; LONG style, ex;
    if (w->fullscreen || w->framed) { style = w->saved_style; ex = w->saved_exstyle; }
    else { style = r_GetWindowLongA(w->hwnd, GWL_STYLE); ex = r_GetWindowLongA(w->hwnd, GWL_EXSTYLE); }
    nc_size(w->hwnd, style, ex, (w->fullscreen ? w->menu : r_GetMenu(w->hwnd)) != NULL, &ncw, &nch, NULL, NULL);
    if (ow - ncw > 0 && oh - nch > 0) {
        w->lw = ow - ncw; w->lh = oh - nch;
        mkw_log("論理サイズ変更 %dx%d\n", w->lw, w->lh);
    }
}

static void logical_outer(vwin_t *w, int *ow, int *oh)
{
    int ncw, nch; LONG st, ex;
    if (w->fullscreen || w->framed) { st = w->saved_style; ex = w->saved_exstyle; }
    else { st = r_GetWindowLongA(w->hwnd, GWL_STYLE); ex = r_GetWindowLongA(w->hwnd, GWL_EXSTYLE); }
    nc_size(w->hwnd, st, ex, (w->fullscreen ? w->menu : r_GetMenu(w->hwnd)) != NULL, &ncw, &nch, NULL, NULL);
    *ow = w->lw + ncw; *oh = w->lh + nch;
}

/* ゲームが指定した論理位置と中心が一致するように実際の窓を置く */
static void place_center(vwin_t *w, int lx, int ly)
{
    RECT wr, wa; int ow, oh, cx, cy, nx, ny, rw, rh;
    logical_outer(w, &ow, &oh);
    r_GetWindowRect(w->hwnd, &wr);
    rw = wr.right - wr.left; rh = wr.bottom - wr.top;
    cx = lx + ow / 2; cy = ly + oh / 2;
    nx = cx - rw / 2; ny = cy - rh / 2;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (nx + rw > wa.right) nx = wa.right - rw;
    if (ny + rh > wa.bottom) ny = wa.bottom - rh;
    if (nx < wa.left) nx = wa.left;
    if (ny < wa.top) ny = wa.top;
    w->in_size++;
    r_SetWindowPos(w->hwnd, NULL, nx, ny, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    w->in_size--;
}

static BOOL WINAPI hk_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL rep)
{
    vwin_t *w = vwin_find(h);
    if (!w || w->in_size) return r_MoveWindow(h, x, y, cx, cy, rep);
    set_logical_from_outer(w, cx, cy);
    if (w->fullscreen) return TRUE;
    apply_real_size(w, 0, 0, 0, rep);
    place_center(w, x, y);
    CallWindowProcA(w->orig_proc, h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->lw, w->lh));
    return TRUE;
}

static BOOL WINAPI hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    vwin_t *w = vwin_find(h);
    BOOL ok;
    if (!w || w->in_size) return r_SetWindowPos(h, after, x, y, cx, cy, fl);
    if (!(fl & SWP_NOSIZE)) set_logical_from_outer(w, cx, cy);
    if (w->fullscreen) {
        /* 位置・サイズは維持し、表示状態や Z 順だけ反映 */
        return r_SetWindowPos(h, after, 0, 0, 0, 0, (fl | SWP_NOMOVE | SWP_NOSIZE) & ~SWP_FRAMECHANGED);
    }
    if (!(fl & SWP_NOSIZE)) apply_real_size(w, 0, 0, 0, !(fl & SWP_NOREDRAW));
    if (!(fl & SWP_NOMOVE)) place_center(w, x, y);
    ok = r_SetWindowPos(h, after, 0, 0, 0, 0, fl | SWP_NOMOVE | SWP_NOSIZE);
    if (!(fl & SWP_NOSIZE)) CallWindowProcA(w->orig_proc, h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->lw, w->lh));
    return ok;
}

static BOOL WINAPI hk_GetClientRect(HWND h, LPRECT r)
{
    vwin_t *w = vwin_find(h);
    if (!w || !r) return r_GetClientRect(h, r);
    r->left = r->top = 0; r->right = w->lw; r->bottom = w->lh;
    return TRUE;
}

static BOOL WINAPI hk_GetWindowRect(HWND h, LPRECT r)
{
    vwin_t *w = vwin_find(h);
    BOOL ok = r_GetWindowRect(h, r);
    if (ok && w && r) {
        int ow, oh, cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
        logical_outer(w, &ow, &oh);
        if (w->fullscreen) { r->left = 0; r->top = 0; }
        else { r->left = cx - ow / 2; r->top = cy - oh / 2; }
        r->right = r->left + ow; r->bottom = r->top + oh;
    }
    return ok;
}

static HDC WINAPI hk_GetDC(HWND h)
{
    vwin_t *w = vwin_find(h);
    if (w) { w->dirty = 1; return vwin_canvas_dc(w); }
    return r_GetDC(h);
}
static HDC WINAPI hk_GetDCEx(HWND h, HRGN rgn, DWORD fl)
{
    vwin_t *w = vwin_find(h);
    if (w && !(fl & DCX_WINDOW)) { w->dirty = 1; return vwin_canvas_dc(w); }
    return r_GetDCEx(h, rgn, fl);
}
static int WINAPI hk_ReleaseDC(HWND h, HDC dc)
{
    vwin_t *w = vwin_from_canvas_dc(dc);
    if (w) { if (w->dirty) vwin_present(w, 0); return 1; }
    return r_ReleaseDC(h, dc);
}
static HDC WINAPI hk_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    vwin_t *w = vwin_find(h);
    HDC dc = r_BeginPaint(h, ps);
    if (w && dc) {
        ps->hdc = vwin_canvas_dc(w);
        ps->rcPaint.left = 0; ps->rcPaint.top = 0; ps->rcPaint.right = w->lw; ps->rcPaint.bottom = w->lh;
        w->dirty = 1;
        SetPropA(h, "mkw_realdc", (HANDLE)dc);
        return ps->hdc;
    }
    return dc;
}
static BOOL WINAPI hk_EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    vwin_t *w = vwin_find(h);
    if (w && ps && ps->hdc == w->cdc) {
        PAINTSTRUCT p2 = *ps;
        p2.hdc = (HDC)GetPropA(h, "mkw_realdc");
        RemovePropA(h, "mkw_realdc");
        r_EndPaint(h, &p2);
        vwin_present(w, 0);
        return TRUE;
    }
    return r_EndPaint(h, ps);
}

/* 窓の実 DC (作成前に取得されたもの / CS_OWNDC) への描画をキャンバスへ向け直す */
static HDC redirect_dc(HDC dc)
{
    HWND h;
    if (!dc || vwin_from_canvas_dc(dc)) return dc;
    if (GetObjectType(dc) != OBJ_DC) return dc;
    h = WindowFromDC(dc);
    if (h) { vwin_t *w = vwin_find(h); if (w) return vwin_canvas_dc(w); }
    return dc;
}

/* 1フレーム分の転送と見なせるか (キャンバスの半分以上を覆う) */
static void after_blit(HDC dc, int dw, int dh)
{
    vwin_t *w = vwin_from_canvas_dc(dc);
    { static int n; if (g_cfg.log && n < 6) { n++; mkw_log("GDI転送 dc=%p canvas=%d %dx%d\n", (void *)dc, w != NULL, dw, dh); } }
    if (!w) return;
    if (dw < 0) dw = -dw;
    if (dh < 0) dh = -dh;
    if ((long long)dw * dh * 2 >= (long long)w->cw * w->ch) vwin_present(w, 1);
    else vwin_present_small(w);
}
static int WINAPI hk_StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                   const void *bits, const BITMAPINFO *bi, UINT use, DWORD rop)
{
    int r;
    dc = redirect_dc(dc);
    r = r_StretchDIBits(dc, x, y, w, h, sx, sy, sw, sh, bits, bi, use, rop);
    after_blit(dc, w, h);
    return r;
}
static int WINAPI hk_SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT st, UINT n,
                                       const void *bits, const BITMAPINFO *bi, UINT use)
{
    int r;
    dc = redirect_dc(dc);
    r = r_SetDIBitsToDevice(dc, x, y, w, h, sx, sy, st, n, bits, bi, use);
    after_blit(dc, (int)w, (int)h);
    return r;
}
static BOOL WINAPI hk_BitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    BOOL r;
    dc = redirect_dc(dc);
    r = r_BitBlt(dc, x, y, w, h, src, sx, sy, rop);
    after_blit(dc, w, h);
    return r;
}
static BOOL WINAPI hk_StretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    BOOL r;
    dc = redirect_dc(dc);
    r = r_StretchBlt(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
    after_blit(dc, w, h);
    return r;
}

/* ---- 画面モード ---- */
static int WINAPI hk_GetSystemMetrics(int i)
{
    if (g_fake_mode_w) {
        if (i == SM_CXSCREEN || i == SM_CXFULLSCREEN || i == SM_CXVIRTUALSCREEN) return g_fake_mode_w;
        if (i == SM_CYSCREEN || i == SM_CYFULLSCREEN || i == SM_CYVIRTUALSCREEN) return g_fake_mode_h;
    }
    return r_GetSystemMetrics(i);
}

static vwin_t *g_mode_win;
void mode_change_fake2(int w, int h, int bpp, HWND hwnd)
{
    vwin_t *v = hwnd ? vwin_find(hwnd) : NULL;
    if (!v) v = g_main;
    if (w > 0 && h > 0) {
        g_fake_mode_w = w; g_fake_mode_h = h; g_fake_mode_bpp = bpp;
        mkw_log("画面モード変更(仮想) %dx%d %dbpp\n", w, h, bpp);
        if (v && (v != g_main || hwnd)) { v->lw = w; v->lh = h; }  /* DirectDraw 排他モードの窓は画面モードの大きさとみなす */
        if (v && !v->fullscreen) {
            if (g_cfg.mode_fullscreen || (g_cfg.fullscreen && v == g_main)) { vwin_set_fullscreen(v, 1); g_mode_fs = 1; g_mode_win = v; }
            else if (!(r_GetWindowLongA(v->hwnd, GWL_STYLE) & WS_CAPTION) && g_cfg.frame_popup) {
                v->saved_style = r_GetWindowLongA(v->hwnd, GWL_STYLE);
                v->saved_exstyle = r_GetWindowLongA(v->hwnd, GWL_EXSTYLE);
                vwin_apply_frame(v);
            }
        }
    } else {
        g_fake_mode_w = g_fake_mode_h = g_fake_mode_bpp = 0;
        mkw_log("画面モード復元(仮想)\n");
        if (g_mode_win && g_mode_fs && !(g_cfg.fullscreen && g_mode_win == g_main)) vwin_set_fullscreen(g_mode_win, 0);
        g_mode_fs = 0; g_mode_win = NULL;
    }
}
void mode_change_fake(int w, int h, int bpp) { mode_change_fake2(w, h, bpp, NULL); }

/* DirectDraw の排他モード窓を後から登録 */
void vwin_register_ext(HWND h)
{
    if (!h || vwin_find(h)) return;
    if (vwin_register(h, 0, 0)) mkw_log("排他モード窓を登録 %p\n", (void *)h);
}

static LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD fl)
{
    if (!g_cfg.ignore_mode_change) return r_ChangeDisplaySettingsA(dm, fl);
    if (fl & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (!dm) mode_change_fake(0, 0, 0);
    else mode_change_fake((dm->dmFields & DM_PELSWIDTH) ? (int)dm->dmPelsWidth : 640,
                          (dm->dmFields & DM_PELSHEIGHT) ? (int)dm->dmPelsHeight : 480,
                          (dm->dmFields & DM_BITSPERPEL) ? (int)dm->dmBitsPerPel : 16);
    return DISP_CHANGE_SUCCESSFUL;
}
static LONG WINAPI hk_ChangeDisplaySettingsW(DEVMODEW *dm, DWORD fl)
{
    if (!g_cfg.ignore_mode_change) return r_ChangeDisplaySettingsW(dm, fl);
    if (fl & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (!dm) mode_change_fake(0, 0, 0);
    else mode_change_fake((int)dm->dmPelsWidth, (int)dm->dmPelsHeight, (int)dm->dmBitsPerPel);
    return DISP_CHANGE_SUCCESSFUL;
}
static BOOL WINAPI hk_EnumDisplaySettingsA(LPCSTR dev, DWORD n, DEVMODEA *dm)
{
    static const int modes[][2] = { { 320, 240 }, { 512, 384 }, { 640, 400 }, { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    static const int bpps[] = { 8, 16, 32 };
    static DWORD realcount = (DWORD)-1;
    BOOL r;
    if (n == ENUM_CURRENT_SETTINGS || n == ENUM_REGISTRY_SETTINGS || !g_cfg.ignore_mode_change) {
        r = r_EnumDisplaySettingsA(dev, n, dm);
        if (r && dm && g_fake_mode_w && n == ENUM_CURRENT_SETTINGS) {
            dm->dmPelsWidth = g_fake_mode_w; dm->dmPelsHeight = g_fake_mode_h;
            if (g_fake_mode_bpp) dm->dmBitsPerPel = g_fake_mode_bpp;
        }
        return r;
    }
    if (realcount == (DWORD)-1) {
        DEVMODEA t; DWORD i = 0; memset(&t, 0, sizeof t); t.dmSize = sizeof t;
        while (i < 2000 && r_EnumDisplaySettingsA(dev, i, &t)) i++;
        realcount = i;
    }
    if (n < realcount) return r_EnumDisplaySettingsA(dev, n, dm);
    n -= realcount;
    if (n >= 6 * 3 || !dm) return FALSE;
    {
        WORD sz = dm->dmSize;
        r_EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, dm);
        dm->dmSize = sz;
        dm->dmPelsWidth = modes[n % 6][0]; dm->dmPelsHeight = modes[n % 6][1]; dm->dmBitsPerPel = bpps[n / 6];
        dm->dmDisplayFrequency = 60;
        dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    }
    return TRUE;
}

/* ---- メニュー/スタイル ---- */
static HMENU WINAPI hk_GetMenu(HWND h)
{
    vwin_t *w = vwin_find(h);
    if (w && w->fullscreen) return w->menu;
    return r_GetMenu(h);
}
static BOOL WINAPI hk_SetMenu(HWND h, HMENU m)
{
    vwin_t *w = vwin_find(h);
    if (w && w->fullscreen) { w->menu = m; if (!g_cfg.hide_menu_fs) r_SetMenu(h, m); return TRUE; }
    if (w) {
        /* 本物の Windows と同じく、窓の外寸を保ったままクライアント領域がメニューの分だけ変わる */
        int ow, oh, had = r_GetMenu(h) != NULL;
        BOOL r;
        logical_outer(w, &ow, &oh);
        r = r_SetMenu(h, m);
        if (had != (m != NULL)) set_logical_from_outer(w, ow, oh);
        apply_real_size(w, 0, 0, 0, TRUE);
        return r;
    }
    return r_SetMenu(h, m);
}
/* ゲームのアクセラレータに Alt+Enter (ゲーム自身の全画面切り替え) があると、
 * キー入力が WM_COMMAND に変換されて MKWrap の擬似フルスクリーン切り替えに届かないので、先に横取りする */
static int (WINAPI *r_TranslateAcceleratorA)(HWND, HACCEL, LPMSG);
static int (WINAPI *r_TranslateAcceleratorW)(HWND, HACCEL, LPMSG);
static int is_fs_hotkey(const MSG *m)
{
    vwin_t *w;
    if (!m || (m->message != WM_SYSKEYDOWN && m->message != WM_SYSKEYUP && m->message != WM_SYSCHAR)) return 0;
    if (!(m->lParam & (1 << 29))) return 0;   /* Alt が押されていない */
    if (m->message == WM_SYSCHAR ? !(m->wParam == '\r' && g_cfg.fs_key_vk == VK_RETURN) : m->wParam != (WPARAM)g_cfg.fs_key_vk) return 0;
    w = vwin_find(m->hwnd);
    if (!w) w = vwin_find(GetAncestor(m->hwnd, GA_ROOT));
    return w != NULL;
}
static int WINAPI hk_TranslateAcceleratorA(HWND h, HACCEL a, LPMSG m)
{
    if (is_fs_hotkey(m)) return 0;
    return r_TranslateAcceleratorA(h, a, m);
}
static int WINAPI hk_TranslateAcceleratorW(HWND h, HACCEL a, LPMSG m)
{
    if (is_fs_hotkey(m)) return 0;
    return r_TranslateAcceleratorW(h, a, m);
}

static BOOL WINAPI hk_DrawMenuBar(HWND h)
{
    vwin_t *w = vwin_find(h);
    if (w && w->fullscreen && g_cfg.hide_menu_fs) return TRUE;
    return r_DrawMenuBar(h);
}
static LONG WINAPI hk_SetWindowLongA(HWND h, int idx, LONG v)
{
    vwin_t *w = vwin_find(h);
    if (w && idx == GWL_WNDPROC) {
        LONG old = (LONG)(LONG_PTR)w->orig_proc;
        w->orig_proc = (WNDPROC)(LONG_PTR)v;
        return old;
    }
    if (w && (w->fullscreen || w->framed) && (idx == GWL_STYLE || idx == GWL_EXSTYLE)) {
        LONG old;
        if (idx == GWL_STYLE) { old = w->saved_style; w->saved_style = v; }
        else { old = w->saved_exstyle; w->saved_exstyle = v; }
        return old;
    }
    return r_SetWindowLongA(h, idx, v);
}
static LONG WINAPI hk_GetWindowLongA(HWND h, int idx)
{
    vwin_t *w = vwin_find(h);
    if (w && idx == GWL_WNDPROC) return (LONG)(LONG_PTR)w->orig_proc;
    if (w && (w->fullscreen || w->framed)) {
        if (idx == GWL_STYLE) return w->saved_style;
        if (idx == GWL_EXSTYLE) return w->saved_exstyle;
    }
    return r_GetWindowLongA(h, idx);
}
static BOOL WINAPI hk_ShowWindow(HWND h, int cmd)
{
    vwin_t *w = vwin_find(h);
    BOOL r;
    if (w && w->fullscreen && (cmd == SW_MAXIMIZE || cmd == SW_RESTORE)) cmd = SW_SHOW;
    r = r_ShowWindow(h, cmd);
    if (w && g_cfg.fullscreen && !w->fullscreen && cmd != SW_HIDE && cmd != SW_MINIMIZE && cmd != SW_SHOWMINIMIZED && IsWindowVisible(h)) {
        static int once;
        if (!once) { once = 1; vwin_set_fullscreen(w, 1); }
    }
    return r;
}

#define GP(mod, name) (void *)GetProcAddress(GetModuleHandleA(mod), name)
void win_hooks_install(void)
{
    LoadLibraryA("user32.dll"); LoadLibraryA("gdi32.dll");
    InitializeCriticalSection(&g_cs);
    r_CreateWindowExA = GP("user32.dll", "CreateWindowExA");
    r_CreateWindowExW = GP("user32.dll", "CreateWindowExW");
    r_MoveWindow = GP("user32.dll", "MoveWindow");
    r_SetWindowPos = GP("user32.dll", "SetWindowPos");
    r_GetClientRect = GP("user32.dll", "GetClientRect");
    r_GetWindowRect = GP("user32.dll", "GetWindowRect");
    r_GetDC = GP("user32.dll", "GetDC");
    r_GetDCEx = GP("user32.dll", "GetDCEx");
    r_ReleaseDC = GP("user32.dll", "ReleaseDC");
    r_BeginPaint = GP("user32.dll", "BeginPaint");
    r_EndPaint = GP("user32.dll", "EndPaint");
    r_GetSystemMetrics = GP("user32.dll", "GetSystemMetrics");
    r_ChangeDisplaySettingsA = GP("user32.dll", "ChangeDisplaySettingsA");
    r_ChangeDisplaySettingsW = GP("user32.dll", "ChangeDisplaySettingsW");
    r_EnumDisplaySettingsA = GP("user32.dll", "EnumDisplaySettingsA");
    r_GetMenu = GP("user32.dll", "GetMenu");
    r_SetMenu = GP("user32.dll", "SetMenu");
    r_DrawMenuBar = GP("user32.dll", "DrawMenuBar");
    r_SetWindowLongA = GP("user32.dll", "SetWindowLongA");
    r_GetWindowLongA = GP("user32.dll", "GetWindowLongA");
    r_ShowWindow = GP("user32.dll", "ShowWindow");
    r_InvalidateRect = GP("user32.dll", "InvalidateRect");
    r_ClientToScreen = GP("user32.dll", "ClientToScreen");
    r_ScreenToClient = GP("user32.dll", "ScreenToClient");
    r_StretchDIBits = GP("gdi32.dll", "StretchDIBits");
    r_SetDIBitsToDevice = GP("gdi32.dll", "SetDIBitsToDevice");
    r_BitBlt = GP("gdi32.dll", "BitBlt");
    r_StretchBlt = GP("gdi32.dll", "StretchBlt");

#define H(dll, fn) iat_hook(g_exe, dll, #fn, (void *)hk_##fn)
    H("user32.dll", CreateWindowExA);
    H("user32.dll", CreateWindowExW);
    H("user32.dll", MoveWindow);
    H("user32.dll", SetWindowPos);
    H("user32.dll", GetClientRect);
    H("user32.dll", GetWindowRect);
    H("user32.dll", GetDC);
    H("user32.dll", GetDCEx);
    H("user32.dll", ReleaseDC);
    H("user32.dll", BeginPaint);
    H("user32.dll", EndPaint);
    H("user32.dll", GetSystemMetrics);
    H("user32.dll", ChangeDisplaySettingsA);
    H("user32.dll", ChangeDisplaySettingsW);
    H("user32.dll", EnumDisplaySettingsA);
    H("user32.dll", GetMenu);
    H("user32.dll", SetMenu);
    r_TranslateAcceleratorA = GP("user32.dll", "TranslateAcceleratorA");
    r_TranslateAcceleratorW = GP("user32.dll", "TranslateAcceleratorW");
    H("user32.dll", TranslateAcceleratorA);
    H("user32.dll", TranslateAcceleratorW);
    H("user32.dll", DrawMenuBar);
    H("user32.dll", SetWindowLongA);
    H("user32.dll", GetWindowLongA);
    H("user32.dll", ShowWindow);
    H("gdi32.dll", StretchDIBits);
    H("gdi32.dll", SetDIBitsToDevice);
    H("gdi32.dll", BitBlt);
    H("gdi32.dll", StretchBlt);
#undef H
}

/* DirectDraw エミュレーションから使う */
BOOL mkw_real_GetClientRect(HWND h, LPRECT r) { return r_GetClientRect ? r_GetClientRect(h, r) : GetClientRect(h, r); }
BOOL mkw_real_ClientToScreen(HWND h, LPPOINT p) { return r_ClientToScreen ? r_ClientToScreen(h, p) : ClientToScreen(h, p); }
