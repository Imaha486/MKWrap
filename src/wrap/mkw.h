/* MKWrap - メディアカイト系アーケード移植ソフト用 汎用ラッパー (dinput.dll プロキシ) */
#ifndef MKW_H
#define MKW_H
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <stdio.h>

#define MKW_VERSION "1.3.1"

/* ---------------- 設定 ---------------- */
typedef struct {
    /* [Display] */
    double scale;          /* 窓表示倍率 (0=自動) */
    int    fullscreen;     /* 起動時に擬似フルスクリーン */
    int    filter;         /* 0=ニアレスト 1=スムーズ */
    int    keep_aspect;
    int    integer_scale;  /* フルスクリーン時に整数倍のみ */
    int    hide_menu_fs;   /* フルスクリーン時メニューバーを隠す */
    int    desktop_bpp;    /* DirectDraw へ報告する画面色数 */
    int    fs_key_vk;      /* Alt+この仮想キーで切替 (既定 VK_RETURN) */
    int    hide_cursor_fs;
    int    ignore_mode_change;
    int    virtualize_window; /* 0にすると窓の仮想化を行わない */
    double aspect;         /* 表示アスペクト補正 (0=画素等倍) */
    int    renderer;       /* 0=自動(D3D9) 1=GDI */
    int    mode_fullscreen; /* ゲームが全画面にしたとき擬似フルスクリーンにする */
    int    frame_popup;     /* 枠なし窓を窓表示するときは枠を付ける */
    int    vsync;
    int    resizable;       /* 窓の大きさをマウスで変えられるようにする */
    int    dialog_dpi;      /* 0=ゲームと同じ 1=ダイアログは DPI 非対応として作る (Windows が拡大) */
    /* [Timing] */
    double refresh;        /* 画面転送の上限レート Hz (0=無制限) */
    double timer_hz;       /* timeSetEvent 周期を置き換え (0=無効) */
    double time_scale;     /* timeGetTime 等の進み方の倍率 (1=等速) */
    int    precise_timer;  /* GetTickCount/timeGetTime を高精度化 */
    double framewait_hz;   /* [Patch] framewait の待ち間隔 */
    /* [Input] */
    int    input_enable;
    int    pad_index;
    int    deadzone;       /* % */
    int    stick_dirs;     /* 4 or 8 */
    int    hide_joysticks;
    int    background_input;
    int    post_messages;  /* WM_KEYDOWN/UP も送る */
    char   map[32][64];    /* ボタン -> キー名 */
    int    pause_vk;       /* キーボードのポーズキー */
    char   pause_button[64];
    /* [Registry] */
    int    reg_virtualize;
    /* [Debug] */
    int    log;
} mkw_config_t;

extern mkw_config_t g_cfg;
extern HMODULE g_self;
extern HMODULE g_exe;
extern wchar_t g_gamedir[MAX_PATH];
extern wchar_t g_ini[MAX_PATH];

void cfg_load(void);
void mkw_log(const char *fmt, ...);

/* ---------------- フック ---------------- */
void *iat_hook(HMODULE mod, const char *dll, const char *func, void *newfn);
void *iat_hook_ord(HMODULE mod, const char *dll, WORD ord, void *newfn);
void dsound_hooks_install(void);
void dsound_pause(int pause);
void patch_vtbl(void *obj, int idx, void *fn, void **orig);
void hooks_install(void);
void patches_apply(void);
void *mkw_get_proc_override(HMODULE mod, const char *name); /* GetProcAddress 差し替え */

/* ---------------- 窓の仮想化と表示 ---------------- */
typedef struct {
    HWND   hwnd;
    int    lw, lh;          /* ゲームから見えるクライアントサイズ */
    int    fullscreen;
    RECT   saved_rect;       /* 窓モード時の位置 */
    LONG   saved_style, saved_exstyle;
    HMENU  menu;
    WNDPROC orig_proc;
    /* キャンバス */
    HDC    cdc;
    HBITMAP cbmp, cold;
    void  *cbits;
    int    cw, ch;          /* キャンバスサイズ */
    volatile LONG dirty;
    int    in_size;
    int    timer;
    DWORD  tid;            /* 窓を作ったスレッド */
    volatile LONG post_pending;
    int    framed;          /* 枠なし窓に枠を付けて窓表示している */
    volatile double next_post; /* 次に表示を依頼してよい時刻 (窓のスレッドを表示処理で埋めないため) */
    volatile LONG st_posts, st_presents; /* 統計: 依頼回数 / 実際の表示回数 */
    double st_ptime;
    double uscale;
    int    user_sizing;          /* 利用者が窓の大きさを変えたときの倍率 (0=自動/指定どおり) */        /* 統計: 表示にかかった時間の合計 */
} vwin_t;

extern vwin_t *g_main;
vwin_t *vwin_find(HWND h);
vwin_t *vwin_main(void);
void    vwin_ensure_canvas(vwin_t *w, int cw, int ch);
void    vwin_present(vwin_t *w, int frame);
void    vwin_present_small(vwin_t *w);  /* frame=1: 1フレーム分の転送 (レート制限対象) */
HDC     vwin_canvas_dc(vwin_t *w);
void    vwin_toggle_fullscreen(vwin_t *w);
void    vwin_set_fullscreen(vwin_t *w, int fs);
int     vwin_is_canvas_dc(HDC dc);
vwin_t *vwin_from_canvas_dc(HDC dc);
void    win_hooks_install(void);
int     d3d9_present(vwin_t *w, const RECT *cl, const RECT *d);
void    d3d9_release_window(HWND h);
extern int g_fake_mode_w, g_fake_mode_h, g_fake_mode_bpp; /* ゲームが設定したつもりの画面モード */

/* ---------------- タイミング ---------------- */
void frame_limit(void);
void timing_hooks_install(void);
double mkw_now(void);
void pause_wait_if_paused(void);
extern volatile int g_paused;

/* ---------------- 入力 ---------------- */
void input_init(void);
void input_hooks_install(void);
int  input_vk_down(int vk);     /* パッドで押されている仮想キー */
int  input_dik_down(int dik);   /* パッドで押されている DIK */
void input_poll(void);
HRESULT WINAPI mk_DirectInputCreateA(HINSTANCE, DWORD, void **, void *);
HRESULT WINAPI mk_DirectInputCreateW(HINSTANCE, DWORD, void **, void *);
HRESULT WINAPI mk_DirectInputCreateEx(HINSTANCE, DWORD, REFIID, void **, void *);
void dinput_wrap_object(void *pDI, int unicode);

/* ---------------- DirectDraw ---------------- */
HRESULT WINAPI mk_DirectDrawCreate(GUID *g, void **pp, void *outer);
HRESULT WINAPI mk_DirectDrawCreateEx(GUID *g, void **pp, REFIID iid, void *outer);
HRESULT WINAPI mk_DirectDrawEnumerateA(void *cb, void *ctx);
HRESULT WINAPI mk_DirectDrawEnumerateExA(void *cb, void *ctx, DWORD flags);

/* ---------------- レジストリ ---------------- */
void reg_hooks_install(void);

/* ---------------- 調査用 ---------------- */
void crash_init(void);
void crash_hooks_install(void);
void diag_launch_helper(const char *kind, DWORD tid, void *ep);
void diag_write_header(const wchar_t *file, const char *title);

/* キー名 */
int keyname_to_vk(const char *name);
int vk_to_dik(int vk);

#endif
