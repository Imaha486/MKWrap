#include "mkw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

mkw_config_t g_cfg;
HMODULE g_self, g_exe;
wchar_t g_gamedir[MAX_PATH];
wchar_t g_ini[MAX_PATH];
static FILE *g_logf;
static CRITICAL_SECTION g_logcs;

void mkw_log(const char *fmt, ...)
{
    va_list a; SYSTEMTIME st;
    if (!g_cfg.log || !g_logf) return;
    EnterCriticalSection(&g_logcs);
    GetLocalTime(&st);
    fprintf(g_logf, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(a, fmt); vfprintf(g_logf, fmt, a); va_end(a);
    fflush(g_logf);
    LeaveCriticalSection(&g_logcs);
}

/* ---------------- INI 読み込み ---------------- */
static int ini_int(const wchar_t *sec, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, g_ini);
}
static double ini_dbl(const wchar_t *sec, const wchar_t *key, double def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_ini);
    if (!buf[0]) return def;
    return _wtof(buf);
}
static void ini_str(const wchar_t *sec, const wchar_t *key, const char *def, char *out, int outlen)
{
    wchar_t buf[256], wdef[64];
    MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 64);
    GetPrivateProfileStringW(sec, key, wdef, buf, 256, g_ini);
    /* コメント除去 */
    { wchar_t *c = wcschr(buf, L';'); if (c) *c = 0; }
    { int n = (int)wcslen(buf); while (n && (buf[n - 1] == L' ' || buf[n - 1] == L'\t')) buf[--n] = 0; }
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, outlen, NULL, NULL);
}

const char *g_button_names[] = {
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Start", "Back", "LThumb", "RThumb",
    "LB", "RB", "Guide", "", "A", "B", "X", "Y",
    "LT", "RT", "LStickUp", "LStickDown", "LStickLeft", "LStickRight",
    "RStickUp", "RStickDown", "RStickLeft", "RStickRight", NULL
};

void cfg_load(void)
{
    wchar_t *p; char tmp[64]; int i;
    GetModuleFileNameW(g_self, g_ini, MAX_PATH);
    p = wcsrchr(g_ini, L'\\');
    if (p) { p[1] = 0; } else g_ini[0] = 0;
    wcscpy(g_gamedir, g_ini);
    if (g_gamedir[0]) g_gamedir[wcslen(g_gamedir) - 1] = 0; /* 末尾の \ を除く */
    wcscat(g_ini, L"mkwrap.ini");

    g_cfg.scale = ini_dbl(L"Display", L"Scale", 0);
    g_cfg.fullscreen = ini_int(L"Display", L"Fullscreen", 0);
    g_cfg.filter = ini_int(L"Display", L"Filter", 0);
    g_cfg.keep_aspect = ini_int(L"Display", L"KeepAspect", 1);
    g_cfg.integer_scale = ini_int(L"Display", L"IntegerScale", 0);
    g_cfg.hide_menu_fs = ini_int(L"Display", L"HideMenuInFullscreen", 1);
    g_cfg.desktop_bpp = ini_int(L"Display", L"DesktopBpp", 32);
    g_cfg.hide_cursor_fs = ini_int(L"Display", L"HideCursorInFullscreen", 1);
    g_cfg.ignore_mode_change = ini_int(L"Display", L"IgnoreModeChange", 1);
    g_cfg.virtualize_window = ini_int(L"Display", L"VirtualizeWindow", 1);
    g_cfg.aspect = ini_dbl(L"Display", L"Aspect", 0);
    ini_str(L"Display", L"Renderer", "auto", tmp, sizeof tmp);
    g_cfg.renderer = !_stricmp(tmp, "gdi") ? 1 : 0;
    g_cfg.vsync = ini_int(L"Display", L"VSync", 0);
    /* 未指定なら Fullscreen に従う (古い INI でも Fullscreen=0 なら窓のまま) */
    g_cfg.mode_fullscreen = ini_int(L"Display", L"ModeChangeFullscreen", g_cfg.fullscreen);
    g_cfg.resizable = ini_int(L"Display", L"Resizable", 1);
    g_cfg.frame_popup = ini_int(L"Display", L"FramePopupWindow", 1);
    ini_str(L"Display", L"FullscreenKey", "ENTER", tmp, sizeof tmp);
    g_cfg.fs_key_vk = keyname_to_vk(tmp);

    g_cfg.refresh = ini_dbl(L"Timing", L"RefreshRate", 0);
    g_cfg.timer_hz = ini_dbl(L"Timing", L"TimerHz", 0);
    g_cfg.time_scale = ini_dbl(L"Timing", L"TimeScale", 1.0);
    if (g_cfg.time_scale <= 0.01) g_cfg.time_scale = 1.0;
    g_cfg.precise_timer = ini_int(L"Timing", L"PreciseTimer", 1);
    {   /* GameHz と TargetHz から倍率を求める指定方法 */
        double ghz = ini_dbl(L"Timing", L"GameHz", 0), thz = ini_dbl(L"Timing", L"TargetHz", 0);
        if (ghz > 0 && thz > 0) g_cfg.time_scale = thz / ghz;
        g_cfg.framewait_hz = ini_dbl(L"Timing", L"FrameWaitHz", thz > 0 ? thz : 60.606);
    }

    g_cfg.input_enable = ini_int(L"Input", L"Enable", 1);
    g_cfg.pad_index = ini_int(L"Input", L"Pad", 0);
    g_cfg.deadzone = ini_int(L"Input", L"Deadzone", 35);
    g_cfg.stick_dirs = ini_int(L"Input", L"StickDirections", 8);
    g_cfg.hide_joysticks = ini_int(L"Input", L"HideJoysticks", 1);
    g_cfg.background_input = ini_int(L"Input", L"BackgroundInput", 0);
    g_cfg.post_messages = ini_int(L"Input", L"PostKeyMessages", 2);
    ini_str(L"Input", L"PauseKey", "PAUSE", tmp, sizeof tmp);
    g_cfg.pause_vk = keyname_to_vk(tmp);
    ini_str(L"Input", L"PauseButton", "", g_cfg.pause_button, sizeof g_cfg.pause_button);
    for (i = 0; g_button_names[i]; i++) {
        wchar_t wk[32];
        if (!g_button_names[i][0]) continue;
        MultiByteToWideChar(CP_UTF8, 0, g_button_names[i], -1, wk, 32);
        ini_str(L"Input", wk, "", g_cfg.map[i], sizeof g_cfg.map[i]);
    }

    g_cfg.reg_virtualize = ini_int(L"Registry", L"Virtualize", 1);
    g_cfg.log = ini_int(L"Debug", L"Log", 0);
    g_cfg.dialog_dpi = ini_int(L"Display", L"DialogDpiUnaware", 1);
}

/* ---------------- 本物の dinput.dll ---------------- */
static HMODULE g_real_dinput;
static FARPROC real_proc(const char *name)
{
    if (!g_real_dinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat(path, L"\\dinput.dll");
        g_real_dinput = LoadLibraryW(path);
        if (!g_real_dinput) { mkw_log("system dinput.dll を読み込めません\n"); return NULL; }
    }
    return GetProcAddress(g_real_dinput, name);
}

typedef HRESULT (WINAPI *DICreateA_t)(HINSTANCE, DWORD, void **, void *);
typedef HRESULT (WINAPI *DICreateEx_t)(HINSTANCE, DWORD, REFIID, void **, void *);

HRESULT WINAPI mk_DirectInputCreateA(HINSTANCE h, DWORD ver, void **out, void *outer)
{
    DICreateA_t f = (DICreateA_t)real_proc("DirectInputCreateA");
    HRESULT hr;
    if (!f) return E_FAIL;
    hr = f(h, ver, out, outer);
    mkw_log("DirectInputCreateA(ver=%x) = %08lx\n", (unsigned)ver, hr);
    if (SUCCEEDED(hr) && out && *out) dinput_wrap_object(*out, 0);
    return hr;
}
HRESULT WINAPI mk_DirectInputCreateW(HINSTANCE h, DWORD ver, void **out, void *outer)
{
    DICreateA_t f = (DICreateA_t)real_proc("DirectInputCreateW");
    HRESULT hr;
    if (!f) return E_FAIL;
    hr = f(h, ver, out, outer);
    if (SUCCEEDED(hr) && out && *out) dinput_wrap_object(*out, 1);
    return hr;
}
HRESULT WINAPI mk_DirectInputCreateEx(HINSTANCE h, DWORD ver, REFIID iid, void **out, void *outer)
{
    DICreateEx_t f = (DICreateEx_t)real_proc("DirectInputCreateEx");
    HRESULT hr;
    if (!f) return E_FAIL;
    hr = f(h, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) dinput_wrap_object(*out, 0);
    return hr;
}

/* エクスポート (exports.def で別名を付ける) */
HRESULT WINAPI exp_DirectInputCreateA(HINSTANCE h, DWORD v, void **o, void *u) { return mk_DirectInputCreateA(h, v, o, u); }
HRESULT WINAPI exp_DirectInputCreateW(HINSTANCE h, DWORD v, void **o, void *u) { return mk_DirectInputCreateW(h, v, o, u); }
HRESULT WINAPI exp_DirectInputCreateEx(HINSTANCE h, DWORD v, REFIID i, void **o, void *u) { return mk_DirectInputCreateEx(h, v, i, o, u); }
HRESULT WINAPI exp_DllCanUnloadNow(void)
{
    typedef HRESULT (WINAPI *f_t)(void); f_t f = (f_t)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI exp_DllGetClassObject(REFCLSID c, REFIID i, void **o)
{
    typedef HRESULT (WINAPI *f_t)(REFCLSID, REFIID, void **); f_t f = (f_t)real_proc("DllGetClassObject");
    return f ? f(c, i, o) : E_FAIL;
}
HRESULT WINAPI exp_DllRegisterServer(void) { return E_NOTIMPL; }
HRESULT WINAPI exp_DllUnregisterServer(void) { return E_NOTIMPL; }

/* ---------------- 初期化 ---------------- */
static int g_inited;
static void mkw_init(void)
{
    char path[MAX_PATH];
    if (g_inited) return;
    g_inited = 1;
    InitializeCriticalSection(&g_logcs);
    g_exe = GetModuleHandleW(NULL);
    cfg_load();
    if (g_cfg.log) {
        wchar_t lp[MAX_PATH];
        _snwprintf(lp, MAX_PATH, L"%ls\\mkwrap.log", g_gamedir);
        g_logf = _wfopen(lp, L"w");
    }
    GetModuleFileNameA(NULL, path, MAX_PATH);
    mkw_log("MKWrap " MKW_VERSION " 起動: %s\n", path);
    mkw_log("Scale=%.2f Fullscreen=%d Refresh=%.3f TimerHz=%.3f TimeScale=%.3f\n",
            g_cfg.scale, g_cfg.fullscreen, g_cfg.refresh, g_cfg.timer_hz, g_cfg.time_scale);
    if (ini_int(L"Display", L"DpiAware", 1)) {
        /* 高DPI環境で Windows によるぼやけた拡大を避ける (ダイアログは Per-Monitor V2 で自動調整される) */
        typedef BOOL (WINAPI *ctx_t)(HANDLE);
        HMODULE u = GetModuleHandleA("user32.dll");
        ctx_t f = u ? (ctx_t)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
        if (!f || !f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) {
            BOOL (WINAPI *g)(void) = u ? (BOOL (WINAPI *)(void))GetProcAddress(u, "SetProcessDPIAware") : NULL;
            if (g) g();
        }
    }
    crash_init();
    hooks_install();
    crash_hooks_install();
    patches_apply();
    input_init();
    { void watchdog_start(void); watchdog_start(); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t exe[MAX_PATH], *b;
        g_self = h;
        DisableThreadLibraryCalls(h);
        /* 記録用ヘルパー (rundll32.exe から MkwDump を呼ぶ) として読み込まれた場合は何もしない */
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        b = wcsrchr(exe, L'\\'); b = b ? b + 1 : exe;
        if (!_wcsicmp(b, L"rundll32.exe")) {
            GetModuleFileNameW(h, g_gamedir, MAX_PATH);
            return TRUE;
        }
        mkw_init();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_logf) { fclose(g_logf); g_logf = NULL; }
    }
    return TRUE;
}
