/* 入力: XInput パッド -> キーボード入力 (DirectInput / GetAsyncKeyState / ウインドウメッセージ) */
#include "mkw.h"
#include <dinput.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

extern const char *g_button_names[];

/* ---- XInput ---- */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XSTATE *);
static XInputGetState_t pXInputGetState;
static int g_xinput_tried;

/* ボタン番号 (g_button_names と同じ並び) */
enum { B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT, B_START, B_BACK, B_LTHUMB, B_RTHUMB, B_LB, B_RB, B_GUIDE, B_RES, B_A, B_B, B_X, B_Y,
       B_LT, B_RT, B_LSU, B_LSD, B_LSL, B_LSR, B_RSU, B_RSD, B_RSL, B_RSR, B_COUNT };

static int g_map_vk[B_COUNT][4];          /* ボタンごとに最大4キー */
static volatile BYTE g_vk_down[256];      /* パッドで押下中の VK */
static volatile BYTE g_dik_down[256];     /* パッドで押下中の DIK */
static int g_pause_buttons[4], g_npause;
static double g_last_poll;
static CRITICAL_SECTION g_cs;

/* ---- キー名 ---- */
static const struct { const char *n; int vk; } g_keys[] = {
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN }, { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE },
    { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "BS", VK_BACK },
    { "SHIFT", VK_SHIFT }, { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT },
    { "CTRL", VK_CONTROL }, { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL },
    { "ALT", VK_MENU }, { "LALT", VK_LMENU }, { "RALT", VK_RMENU },
    { "INSERT", VK_INSERT }, { "INS", VK_INSERT }, { "DELETE", VK_DELETE }, { "DEL", VK_DELETE },
    { "HOME", VK_HOME }, { "END", VK_END }, { "PGUP", VK_PRIOR }, { "PAGEUP", VK_PRIOR }, { "PGDN", VK_NEXT }, { "PAGEDOWN", VK_NEXT },
    { "PAUSE", VK_PAUSE }, { "CAPSLOCK", VK_CAPITAL },
    { "MULTIPLY", VK_MULTIPLY }, { "ADD", VK_ADD }, { "SUBTRACT", VK_SUBTRACT }, { "DECIMAL", VK_DECIMAL }, { "DIVIDE", VK_DIVIDE },
    { "MINUS", VK_OEM_MINUS }, { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "SLASH", VK_OEM_2 },
    { "SEMICOLON", VK_OEM_PLUS }, { "COLON", VK_OEM_1 }, { "AT", VK_OEM_3 }, { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 },
    { "CARET", VK_OEM_7 }, { "YEN", VK_OEM_5 }, { "BACKSLASH", VK_OEM_102 },
    { NULL, 0 }
};

int keyname_to_vk(const char *name)
{
    char u[64]; int i;
    if (!name || !name[0]) return 0;
    for (i = 0; name[i] && i < 63; i++) u[i] = (char)toupper((unsigned char)name[i]);
    u[i] = 0;
    if (u[0] == '0' && u[1] == 'X') return (int)strtol(u, NULL, 16);
    if (!u[1] && ((u[0] >= 'A' && u[0] <= 'Z') || (u[0] >= '0' && u[0] <= '9'))) return u[0];
    if (u[0] == 'F' && u[1] >= '1' && u[1] <= '9') { int n = atoi(u + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (!strncmp(u, "NUMPAD", 6) && u[6] >= '0' && u[6] <= '9' && !u[7]) return VK_NUMPAD0 + (u[6] - '0');
    if (!strncmp(u, "NUM", 3) && u[3] >= '0' && u[3] <= '9' && !u[4]) return VK_NUMPAD0 + (u[3] - '0');
    for (i = 0; g_keys[i].n; i++) if (!strcmp(u, g_keys[i].n)) return g_keys[i].vk;
    return 0;
}

int vk_to_dik(int vk)
{
    switch (vk) {
    case VK_UP: return DIK_UP; case VK_DOWN: return DIK_DOWN; case VK_LEFT: return DIK_LEFT; case VK_RIGHT: return DIK_RIGHT;
    case VK_INSERT: return DIK_INSERT; case VK_DELETE: return DIK_DELETE; case VK_HOME: return DIK_HOME; case VK_END: return DIK_END;
    case VK_PRIOR: return DIK_PRIOR; case VK_NEXT: return DIK_NEXT; case VK_RCONTROL: return DIK_RCONTROL; case VK_RMENU: return DIK_RMENU;
    case VK_DIVIDE: return DIK_DIVIDE; case VK_PAUSE: return DIK_PAUSE;
    case VK_SHIFT: case VK_LSHIFT: return DIK_LSHIFT; case VK_RSHIFT: return DIK_RSHIFT;
    case VK_CONTROL: case VK_LCONTROL: return DIK_LCONTROL; case VK_MENU: case VK_LMENU: return DIK_LMENU;
    }
    return (int)MapVirtualKeyA(vk, 0 /* MAPVK_VK_TO_VSC */) & 0xFF;
}

static int parse_keys(const char *s, int *out, int max)
{
    char buf[64], *t, *ctx = NULL; int n = 0;
    strncpy(buf, s, 63); buf[63] = 0;
    for (t = strtok_r(buf, "+ ", &ctx); t && n < max; t = strtok_r(NULL, "+ ", &ctx)) {
        int vk = keyname_to_vk(t);
        if (vk) out[n++] = vk;
    }
    return n;
}

static int button_index(const char *name)
{
    int i;
    for (i = 0; g_button_names[i]; i++) if (g_button_names[i][0] && !_stricmp(name, g_button_names[i])) return i;
    return -1;
}

/* ---- ポーリング ---- */
static int g_btn_prev[B_COUNT];
static HWND g_msg_target(void) { vwin_t *w = vwin_main(); return w ? w->hwnd : NULL; }

static void post_key(int vk, int down)
{
    HWND h = g_msg_target();
    UINT sc = MapVirtualKeyA(vk, 0);
    LPARAM lp = 1 | (sc << 16);
    if (!h) return;
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) lp |= 1 << 24;
    if (!down) lp |= (1u << 30) | (1u << 31);
    PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
}

static int window_active(void)
{
    vwin_t *w = vwin_main();
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (g_cfg.background_input) return 1;
    if (w && fg == w->hwnd) return 1;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* ---- 割り当ての学習 (未知のゲーム用) ----
 * Ctrl+F12、またはパッドの LB+RB+Back を 1.5 秒押し続けると学習モード。
 * パッドのボタンを押す → そのボタンに割り当てるキーボードのキーを押す、を繰り返す (ゲームは一時停止)。
 * 結果はすぐ mkwrap.ini の [Input] に書き込まれる。もう一度 Ctrl+F12 / LB+RB+Back 長押しで終了。 */
static volatile int g_learn;          /* 0=なし 1=ボタン待ち 2=キー待ち */
static int g_learn_btn, g_learn_saved_pause;
static double g_learn_t, g_hint_until;
static wchar_t g_learn_msg[160];
static BYTE g_key_prev[256];

static const wchar_t *btn_label(int b)
{
    static const wchar_t *n[B_COUNT] = { L"十字キー↑", L"十字キー↓", L"十字キー←", L"十字キー→", L"Start", L"Back", L"左スティック押し込み", L"右スティック押し込み",
        L"LB", L"RB", L"Guide", L"", L"A", L"B", L"X", L"Y", L"LT", L"RT", L"左スティック↑", L"左スティック↓", L"左スティック←", L"左スティック→",
        L"右スティック↑", L"右スティック↓", L"右スティック←", L"右スティック→" };
    return (b >= 0 && b < B_COUNT) ? n[b] : L"";
}
static void vk_to_name(int vk, char *out, int n)
{
    int i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { out[0] = (char)vk; out[1] = 0; return; }
    if (vk >= VK_F1 && vk <= VK_F24) { _snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { _snprintf(out, n, "NUM%d", vk - VK_NUMPAD0); return; }
    for (i = 0; g_keys[i].n; i++) if (g_keys[i].vk == vk) { _snprintf(out, n, "%s", g_keys[i].n); return; }
    _snprintf(out, n, "0x%02X", vk);
}
static SHORT (WINAPI *r_GetAsyncKeyState)(int);
static int real_key(int vk) { return r_GetAsyncKeyState ? (r_GetAsyncKeyState(vk) & 0x8000) != 0 : (GetAsyncKeyState(vk) & 0x8000) != 0; }

const wchar_t *input_overlay_text(void)
{
    if (g_learn) return g_learn_msg;
    if (g_hint_until > 0 && mkw_now() < g_hint_until) return L"パッドの割り当て: Ctrl+F12 または LB+RB+Back 長押し";
    return NULL;
}

static DWORD g_poll_tid;
static void learn_set_msg(void)
{
    if (g_learn == 1) _snwprintf(g_learn_msg, 160, L"[割り当て] パッドのボタンを押す (終了: Ctrl+F12)");
    else _snwprintf(g_learn_msg, 160, L"[割り当て] %ls に割り当てるキーを押す (同じボタン=解除)", btn_label(g_learn_btn));
}
static void learn_toggle(void)
{
    if (!g_learn) {
        g_learn = 1; g_learn_saved_pause = g_paused; g_paused = 1; g_hint_until = 0;
        { int i; for (i = 0; i < 256; i++) g_key_prev[i] = (BYTE)real_key(i); }
        learn_set_msg();
        mkw_log("割り当ての学習を開始\n");
    } else {
        g_learn = 0; g_paused = g_learn_saved_pause;
        mkw_log("割り当ての学習を終了\n");
    }
    { vwin_t *w = vwin_main(); if (w) { w->dirty = 1; vwin_present(w, 0); } }
}
static void learn_assign(int b, int vk)
{
    char name[16] = "", line[64]; wchar_t wb[32], wv[64];
    int pair = -1;
    if (vk) vk_to_name(vk, name, sizeof name);
    /* 十字キーと左スティックは同じ方向に同じキーを割り当てる */
    if (b >= B_DUP && b <= B_DRIGHT) pair = B_LSU + (b - B_DUP);
    else if (b >= B_LSU && b <= B_LSR) pair = B_DUP + (b - B_LSU);
    EnterCriticalSection(&g_cs);
    memset(g_map_vk[b], 0, sizeof g_map_vk[b]);
    if (vk) g_map_vk[b][0] = vk;
    if (pair >= 0) { memset(g_map_vk[pair], 0, sizeof g_map_vk[pair]); if (vk) g_map_vk[pair][0] = vk; }
    LeaveCriticalSection(&g_cs);
    _snprintf(line, sizeof line, "%s", name);
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wv, 64);
    MultiByteToWideChar(CP_UTF8, 0, g_button_names[b], -1, wb, 32);
    WritePrivateProfileStringW(L"Input", wb, wv, g_ini);
    if (pair >= 0) { MultiByteToWideChar(CP_UTF8, 0, g_button_names[pair], -1, wb, 32); WritePrivateProfileStringW(L"Input", wb, wv, g_ini); }
    strncpy(g_cfg.map[b], name, sizeof g_cfg.map[b] - 1);
    mkw_log("割り当て: %s = %s\n", g_button_names[b], vk ? name : "(なし)");
}
/* 学習中は呼び出し元でパッド入力をゲームに渡さない */
static void learn_poll(const int *btn, int active)
{
    static int prev[B_COUNT]; static double combo_t; static int ctrl_f12_prev;
    int i, combo = btn[B_LB] && btn[B_RB] && btn[B_BACK];
    int cf = active && real_key(VK_CONTROL) && real_key(VK_F12);
    double now = mkw_now();
    /* 開始・終了 */
    if (cf && !ctrl_f12_prev) learn_toggle();
    ctrl_f12_prev = cf;
    if (combo) { if (combo_t == 0) combo_t = now; else if (combo_t > 0 && now - combo_t > 1.5) { combo_t = -1; learn_toggle(); } }
    else combo_t = 0;
    if (g_learn && !combo && active) {
        if (g_learn == 1) {
            for (i = 0; i < B_COUNT; i++) if (btn[i] && !prev[i] && i != B_RES && i != B_GUIDE) {
                g_learn_btn = i; g_learn = 2; g_learn_t = now;
                for (int k = 0; k < 256; k++) g_key_prev[k] = (BYTE)real_key(k);
                learn_set_msg(); break;
            }
        } else if (g_learn == 2) {
            if (btn[g_learn_btn] && !prev[g_learn_btn] && now - g_learn_t > 0.2) {
                learn_assign(g_learn_btn, 0); g_learn = 1; learn_set_msg();
            } else {
                for (i = 8; i < 256; i++) {
                    int d;
                    if (i == VK_SHIFT || i == VK_CONTROL || i == VK_MENU || i == VK_F12) continue;
                    d = real_key(i);
                    if (d && !g_key_prev[i]) {
                        learn_assign(g_learn_btn, i); g_learn = 1; learn_set_msg(); break;
                    }
                    g_key_prev[i] = (BYTE)d;
                }
                if (g_learn == 2 && now - g_learn_t > 15) { g_learn = 1; learn_set_msg(); }
            }
        }
        { vwin_t *w = vwin_main(); if (w) w->dirty = 1; }
    }
    memcpy(prev, btn, sizeof prev);
}

void input_poll(void)
{
    XSTATE st; int btn[B_COUNT]; BYTE vk[256], dik[256]; int i, k, pause_now;
    double now = mkw_now();
    static int pause_prev;
    if (!g_cfg.input_enable) return;
    if (now - g_last_poll < 0.002) return;
    g_last_poll = now;
    if (!pXInputGetState) {
        if (g_xinput_tried) return;
        g_xinput_tried = 1;
        {
            const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };
            for (i = 0; i < 5 && !pXInputGetState; i++) {
                HMODULE m = LoadLibraryA(dlls[i]);
                if (m) { pXInputGetState = (XInputGetState_t)GetProcAddress(m, (LPCSTR)100); if (!pXInputGetState) pXInputGetState = (XInputGetState_t)GetProcAddress(m, "XInputGetState"); }
                if (pXInputGetState) mkw_log("XInput: %s\n", dlls[i]);
            }
        }
        if (!pXInputGetState) { mkw_log("XInput が見つかりません\n"); return; }
    }
    memset(btn, 0, sizeof btn);
    memset(&st, 0, sizeof st);
    if (pXInputGetState(g_cfg.pad_index, &st) == ERROR_SUCCESS && window_active()) {
        WORD b = st.Gamepad.wButtons;
        double dz = g_cfg.deadzone / 100.0;
        static const WORD bits[16] = { 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0, 0x1000, 0x2000, 0x4000, 0x8000 };
        for (i = 0; i < 16; i++) if (bits[i] && (b & bits[i])) btn[i] = 1;
        btn[B_LT] = st.Gamepad.bLeftTrigger > 64;
        btn[B_RT] = st.Gamepad.bRightTrigger > 64;
        for (k = 0; k < 2; k++) {
            double x = (k ? st.Gamepad.sThumbRX : st.Gamepad.sThumbLX) / 32767.0;
            double y = (k ? st.Gamepad.sThumbRY : st.Gamepad.sThumbLY) / 32767.0;
            double mag = sqrt(x * x + y * y);
            int base = k ? B_RSU : B_LSU;
            if (mag > dz) {
                double ang = atan2(y, x) * 180.0 / 3.14159265358979;  /* 0=右, 90=上 */
                if (ang < 0) ang += 360;
                if (g_cfg.stick_dirs == 4) {
                    int sec = (int)((ang + 45) / 90) % 4;
                    if (sec == 0) btn[base + 3] = 1; else if (sec == 1) btn[base] = 1; else if (sec == 2) btn[base + 2] = 1; else btn[base + 1] = 1;
                } else {
                    int sec = (int)((ang + 22.5) / 45) % 8;
                    if (sec == 7 || sec == 0 || sec == 1) btn[base + 3] = 1;
                    if (sec >= 1 && sec <= 3) btn[base] = 1;
                    if (sec >= 3 && sec <= 5) btn[base + 2] = 1;
                    if (sec >= 5 && sec <= 7) btn[base + 1] = 1;
                }
            }
        }
    }
    {   /* 調査用: [Debug] TestButtons=A@10-10.2,... (パッドのボタンを押したことにする) */
        static char tb[512]; static int loaded;
        if (!loaded) { wchar_t w[512]; loaded = 1; GetPrivateProfileStringW(L"Debug", L"TestButtons", L"", w, 512, g_ini); WideCharToMultiByte(CP_UTF8, 0, w, -1, tb, 512, NULL, NULL); }
        if (tb[0]) {
            char buf[512], *t, *ctx = NULL; strcpy(buf, tb);
            for (t = strtok_r(buf, ",", &ctx); t; t = strtok_r(NULL, ",", &ctx)) {
                char *at = strchr(t, '@'); double a, b2; int bi;
                if (!at) continue;
                *at = 0; a = atof(at + 1); { char *d = strchr(at + 1, '-'); b2 = d ? atof(d + 1) : a + 0.2; }
                bi = button_index(t);
                if (bi >= 0 && now >= a && now < b2) btn[bi] = 1;
            }
        }
    }
    if (GetCurrentThreadId() == g_poll_tid) learn_poll(btn, window_active());
    if (g_learn || (btn[B_LB] && btn[B_RB] && btn[B_BACK])) memset(btn, 0, sizeof btn);
    /* 一時停止ボタン (同時押し可) */
    pause_now = g_npause > 0;
    for (i = 0; i < g_npause; i++) if (!btn[g_pause_buttons[i]]) pause_now = 0;
    if (pause_now && !pause_prev) g_paused = !g_paused;
    pause_prev = pause_now;
    if (pause_now) for (i = 0; i < g_npause; i++) btn[g_pause_buttons[i]] = 0;

    memset(vk, 0, sizeof vk); memset(dik, 0, sizeof dik);
    for (i = 0; i < B_COUNT; i++) {
        if (!btn[i]) continue;
        for (k = 0; k < 4 && g_map_vk[i][k]; k++) {
            int v = g_map_vk[i][k];
            vk[v & 255] = 1;
            if (v == VK_LSHIFT || v == VK_RSHIFT) vk[VK_SHIFT] = 1;
            if (v == VK_LCONTROL || v == VK_RCONTROL) vk[VK_CONTROL] = 1;
            if (v == VK_LMENU || v == VK_RMENU) vk[VK_MENU] = 1;
            if (v == VK_SHIFT) vk[VK_LSHIFT] = 1;
            if (v == VK_CONTROL) vk[VK_LCONTROL] = 1;
            dik[vk_to_dik(v) & 255] = 1;
        }
    }
    /* 調査用: [Debug] TestInput=キー@開始-終了,... */
    {
        static char ti[512]; static int loaded;
        if (!loaded) { wchar_t w[512]; loaded = 1; GetPrivateProfileStringW(L"Debug", L"TestInput", L"", w, 512, g_ini); WideCharToMultiByte(CP_UTF8, 0, w, -1, ti, 512, NULL, NULL); }
        if (ti[0]) {
            char buf[512], *t, *ctx = NULL; strcpy(buf, ti);
            for (t = strtok_r(buf, ",", &ctx); t; t = strtok_r(NULL, ",", &ctx)) {
                char *at = strchr(t, '@'); double a, b; int v;
                if (!at) continue;
                *at = 0; a = atof(at + 1); { char *d = strchr(at + 1, '-'); b = d ? atof(d + 1) : a + 0.2; }
                v = keyname_to_vk(t);
                if (v && now >= a && now < b) { vk[v] = 1; dik[vk_to_dik(v)] = 1; }
            }
        }
    }
    EnterCriticalSection(&g_cs);
    if (g_cfg.post_messages == 1 || g_cfg.post_messages == 2) {
        for (i = 1; i < 256; i++) {
            if (i == VK_SHIFT || i == VK_CONTROL || i == VK_MENU) continue;
            if (g_cfg.post_messages == 2 && !(i >= VK_F1 && i <= VK_F24)) continue; /* 2: ファンクションキーのみ */
            if (vk[i] != g_vk_down[i]) post_key(i, vk[i]);
        }
    }
    memcpy((void *)g_vk_down, vk, 256);
    memcpy((void *)g_dik_down, dik, 256);
    LeaveCriticalSection(&g_cs);
    memcpy(g_btn_prev, btn, sizeof btn);
}

int input_vk_down(int vk) { input_poll(); return vk > 0 && vk < 256 && g_vk_down[vk]; }
int input_dik_down(int dik) { return dik > 0 && dik < 256 && g_dik_down[dik]; }

static DWORD WINAPI poll_thread(LPVOID p)
{
    (void)p;
    g_poll_tid = GetCurrentThreadId();
    for (;;) {
        Sleep(4);
        input_poll();
    }
    return 0;
}

void input_init(void)
{
    int i;
    InitializeCriticalSection(&g_cs);
    for (i = 0; g_button_names[i] && i < B_COUNT; i++) {
        if (g_cfg.map[i][0]) parse_keys(g_cfg.map[i], g_map_vk[i], 4);
    }
    /* 一時停止ボタン "Back+Start" など */
    if (g_cfg.pause_button[0]) {
        char buf[64], *t, *ctx = NULL;
        strncpy(buf, g_cfg.pause_button, 63); buf[63] = 0;
        for (t = strtok_r(buf, "+ ", &ctx); t && g_npause < 4; t = strtok_r(NULL, "+ ", &ctx)) {
            int b = button_index(t);
            if (b >= 0) g_pause_buttons[g_npause++] = b;
        }
    }
    if (GetPrivateProfileIntW(L"Input", L"LearnHint", 0, g_ini)) g_hint_until = mkw_now() + 8;
    if (g_cfg.input_enable || 1) CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
}

/* ---- Win32 キー状態 API ---- */
static SHORT (WINAPI *r_GetKeyState)(int);
static BOOL (WINAPI *r_GetKeyboardState)(PBYTE);

static SHORT WINAPI hk_GetAsyncKeyState(int vk)
{
    SHORT r = r_GetAsyncKeyState(vk);
    if (input_vk_down(vk & 255)) r |= (SHORT)0x8000;
    return r;
}
static SHORT WINAPI hk_GetKeyState(int vk)
{
    SHORT r = r_GetKeyState(vk);
    if (input_vk_down(vk & 255)) r |= (SHORT)0xFF80;
    return r;
}
static BOOL WINAPI hk_GetKeyboardState(PBYTE ks)
{
    BOOL r = r_GetKeyboardState(ks);
    int i;
    if (r && ks) { input_poll(); for (i = 0; i < 256; i++) if (g_vk_down[i]) ks[i] |= 0x80; }
    return r;
}

/* ---- DirectInput (vtable 書き換え) ---- */
typedef HRESULT (WINAPI *CreateDevice_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *EnumDevices_t)(void *, DWORD, void *, void *, DWORD);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *QI_t)(void *, REFIID, void **);
typedef HRESULT (WINAPI *SetCoop_t)(void *, HWND, DWORD);

#define MAXVT 4
static struct { void **vt; CreateDevice_t cd; EnumDevices_t ed; QI_t qi; } g_divt[MAXVT];
static struct { void **vt; GetDeviceState_t gs; GetDeviceData_t gd; QI_t qi; SetCoop_t sc; } g_devvt[MAXVT];
#define MAXKB 8
static struct { void *dev; BYTE sent[256]; DWORD seq; } g_kb[MAXKB];

static int kb_index(void *dev) { int i; for (i = 0; i < MAXKB; i++) if (g_kb[i].dev == dev) return i; return -1; }

static int find_dev_vt(void *obj) { int i; void **vt = *(void ***)obj; for (i = 0; i < MAXVT; i++) if (g_devvt[i].vt == vt) return i; return -1; }
static int find_di_vt(void *obj) { int i; void **vt = *(void ***)obj; for (i = 0; i < MAXVT; i++) if (g_divt[i].vt == vt) return i; return -1; }

static HRESULT WINAPI hk_dev_GetDeviceState(void *This, DWORD cb, void *data)
{
    int v = find_dev_vt(This);
    HRESULT hr = g_devvt[v].gs(This, cb, data);
    if (kb_index(This) >= 0 && cb >= 256 && data) {
        int i;
        input_poll();
        if (FAILED(hr)) {
            /* フォーカス外などで失敗した場合は注入しない */
            return hr;
        }
        for (i = 1; i < 256; i++) if (g_dik_down[i]) ((BYTE *)data)[i] |= 0x80;
        /* Alt+Enter (擬似フルスクリーン切り替え) の Enter はゲームに渡さない */
        if (g_cfg.fs_key_vk == VK_RETURN && ((((BYTE *)data)[DIK_LMENU] | ((BYTE *)data)[DIK_RMENU]) & 0x80))
            ((BYTE *)data)[DIK_RETURN] = 0;
    }
    return hr;
}

static HRESULT WINAPI hk_dev_GetDeviceData(void *This, DWORD cbo, void *rg, DWORD *inout, DWORD fl)
{
    int v = find_dev_vt(This), k = kb_index(This);
    DWORD cap = inout ? *inout : 0;
    HRESULT hr = g_devvt[v].gd(This, cbo, rg, inout, fl);
    if (k >= 0 && inout && SUCCEEDED(hr) && cbo >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
        DWORD n = *inout; int i;
        BYTE cur[256];
        input_poll();
        memcpy(cur, (const void *)g_dik_down, 256);
        if (!rg) {
            /* バッファの破棄 (INFINITE) */
            if (!(fl & DIGDD_PEEK)) memcpy(g_kb[k].sent, cur, 256);
            return hr;
        }
        for (i = 1; i < 256 && n < cap; i++) {
            if (cur[i] != g_kb[k].sent[i]) {
                DIDEVICEOBJECTDATA_DX3 *d = (DIDEVICEOBJECTDATA_DX3 *)((BYTE *)rg + (size_t)n * cbo);
                memset(d, 0, cbo);
                d->dwOfs = i; d->dwData = cur[i] ? 0x80 : 0; d->dwTimeStamp = GetTickCount(); d->dwSequence = 0x40000000 + ++g_kb[k].seq;
                if (!(fl & DIGDD_PEEK)) g_kb[k].sent[i] = cur[i];
                n++;
            }
        }
        *inout = n;
    }
    return hr;
}

/* キーボードの排他モードは非排他にする。
 * 排他モードでは DirectInput がキー入力を横取りし、窓にキーのメッセージが届かないため、
 * Alt+Enter (擬似フルスクリーン) や一時停止キーが効かなくなる */
static HRESULT WINAPI hk_dev_SetCooperativeLevel(void *This, HWND h, DWORD fl)
{
    int v = find_dev_vt(This);
    if (kb_index(This) >= 0) mkw_log("DirectInput: キーボード SetCooperativeLevel hwnd=%p flags=%lx\n", (void *)h, (unsigned long)fl);
    if (kb_index(This) >= 0 && (fl & DISCL_EXCLUSIVE) && !GetPrivateProfileIntW(L"Input", L"KeyboardExclusive", 0, g_ini)) {
        mkw_log("DirectInput: キーボードの排他モードを非排他に変更 (flags=%lx)\n", (unsigned long)fl);
        fl = (fl & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
    }
    return g_devvt[v].sc(This, h, fl);
}

static void patch_device(void *dev);
static HRESULT WINAPI hk_dev_QueryInterface(void *This, REFIID iid, void **out)
{
    int v = find_dev_vt(This);
    HRESULT hr = g_devvt[v].qi(This, iid, out);
    if (SUCCEEDED(hr) && out && *out) {
        int k = kb_index(This);
        if (k >= 0 && kb_index(*out) < 0) {
            int j; for (j = 0; j < MAXKB; j++) if (!g_kb[j].dev) { g_kb[j].dev = *out; break; }
        }
        patch_device(*out);
    }
    return hr;
}

static void patch_device(void *dev)
{
    void **vt = *(void ***)dev; int i; DWORD old;
    for (i = 0; i < MAXVT; i++) if (g_devvt[i].vt == vt) return;
    for (i = 0; i < MAXVT; i++) if (!g_devvt[i].vt) break;
    if (i == MAXVT) return;
    g_devvt[i].vt = vt;
    g_devvt[i].qi = (QI_t)vt[0];
    g_devvt[i].gs = (GetDeviceState_t)vt[9];
    g_devvt[i].gd = (GetDeviceData_t)vt[10];
    g_devvt[i].sc = (SetCoop_t)vt[13];
    VirtualProtect(vt, 14 * sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    vt[0] = (void *)hk_dev_QueryInterface;
    vt[9] = (void *)hk_dev_GetDeviceState;
    vt[10] = (void *)hk_dev_GetDeviceData;
    vt[13] = (void *)hk_dev_SetCooperativeLevel;
    VirtualProtect(vt, 14 * sizeof(void *), old, &old);
}

static HRESULT WINAPI hk_di_CreateDevice(void *This, REFGUID g, void **out, void *outer)
{
    int v = find_di_vt(This);
    HRESULT hr;
    if (g_cfg.hide_joysticks && !IsEqualGUID(g, &GUID_SysKeyboard) && !IsEqualGUID(g, &GUID_SysMouse)) {
        mkw_log("DirectInput: ジョイスティックの作成を拒否 (HideJoysticks)\n");
        return DIERR_DEVICENOTREG;
    }
    hr = g_divt[v].cd(This, g, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        if (IsEqualGUID(g, &GUID_SysKeyboard)) {
            int j; for (j = 0; j < MAXKB; j++) if (!g_kb[j].dev) { g_kb[j].dev = *out; break; }
            mkw_log("DirectInput: キーボードデバイス作成\n");
        }
        patch_device(*out);
    }
    return hr;
}

typedef struct { LPDIENUMDEVICESCALLBACKA cb; void *ref; } enumctx_t;
static BOOL CALLBACK enum_filter(LPCDIDEVICEINSTANCEA inst, LPVOID p)
{
    enumctx_t *c = (enumctx_t *)p;
    if (GET_DIDEVICE_TYPE(inst->dwDevType) == DIDEVTYPE_JOYSTICK) return DIENUM_CONTINUE;
    return c->cb(inst, c->ref);
}
static HRESULT WINAPI hk_di_EnumDevices(void *This, DWORD type, void *cb, void *ref, DWORD fl)
{
    int v = find_di_vt(This);
    if (g_cfg.hide_joysticks) {
        enumctx_t c; c.cb = (LPDIENUMDEVICESCALLBACKA)cb; c.ref = ref;
        if (type == DIDEVTYPE_JOYSTICK) return DI_OK;
        return g_divt[v].ed(This, type, (void *)enum_filter, &c, fl);
    }
    return g_divt[v].ed(This, type, cb, ref, fl);
}

static HRESULT WINAPI hk_di_QueryInterface(void *This, REFIID iid, void **out)
{
    int v = find_di_vt(This);
    HRESULT hr = g_divt[v].qi(This, iid, out);
    if (SUCCEEDED(hr) && out && *out) dinput_wrap_object(*out, 0);
    return hr;
}

void dinput_wrap_object(void *pDI, int unicode)
{
    void **vt = *(void ***)pDI; int i; DWORD old;
    (void)unicode;
    for (i = 0; i < MAXVT; i++) if (g_divt[i].vt == vt) return;
    for (i = 0; i < MAXVT; i++) if (!g_divt[i].vt) break;
    if (i == MAXVT) return;
    g_divt[i].vt = vt;
    g_divt[i].qi = (QI_t)vt[0];
    g_divt[i].cd = (CreateDevice_t)vt[3];
    g_divt[i].ed = (EnumDevices_t)vt[4];
    VirtualProtect(vt, 5 * sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    vt[0] = (void *)hk_di_QueryInterface;
    vt[3] = (void *)hk_di_CreateDevice;
    vt[4] = (void *)hk_di_EnumDevices;
    VirtualProtect(vt, 5 * sizeof(void *), old, &old);
}

/* winmm のジョイスティック API を隠す (HideJoysticks) */
static UINT WINAPI hk_joyGetNumDevs(void) { return 0; }
static UINT WINAPI hk_joyGetPos(UINT id, void *p) { (void)id; (void)p; return 167 /* JOYERR_UNPLUGGED */; }
static UINT WINAPI hk_joyGetPosEx(UINT id, void *p) { (void)id; (void)p; return 167; }
static UINT WINAPI hk_joyGetDevCapsA(UINT_PTR id, void *p, UINT n) { (void)id; (void)p; (void)n; return 165 /* MMSYSERR_NODRIVER(6)? JOYERR_PARMS */; }

void input_hooks_install(void)
{
    if (g_cfg.hide_joysticks) {
        iat_hook(g_exe, "winmm.dll", "joyGetNumDevs", (void *)hk_joyGetNumDevs);
        iat_hook(g_exe, "winmm.dll", "joyGetPos", (void *)hk_joyGetPos);
        iat_hook(g_exe, "winmm.dll", "joyGetPosEx", (void *)hk_joyGetPosEx);
        iat_hook(g_exe, "winmm.dll", "joyGetDevCapsA", (void *)hk_joyGetDevCapsA);
    }
    HMODULE u = GetModuleHandleA("user32.dll");
    r_GetAsyncKeyState = (void *)GetProcAddress(u, "GetAsyncKeyState");
    r_GetKeyState = (void *)GetProcAddress(u, "GetKeyState");
    r_GetKeyboardState = (void *)GetProcAddress(u, "GetKeyboardState");
    iat_hook(g_exe, "user32.dll", "GetAsyncKeyState", (void *)hk_GetAsyncKeyState);
    iat_hook(g_exe, "user32.dll", "GetKeyState", (void *)hk_GetKeyState);
    iat_hook(g_exe, "user32.dll", "GetKeyboardState", (void *)hk_GetKeyboardState);
    iat_hook(g_exe, "dinput.dll", "DirectInputCreateA", (void *)mk_DirectInputCreateA);
}
