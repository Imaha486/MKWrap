/* 調査用: 異常終了・応答なしの記録
 *
 * - 例外で落ちたとき: mkwrap_crash.log (例外の種類・場所・全スレッドの呼び出し履歴) と mkwrap_crash.dmp
 * - 窓が応答しなくなったとき (watchdog.c): mkwrap_hang.log と mkwrap_hang.dmp
 *
 * 記録は別プロセス (rundll32.exe で このDLL の MkwDump を実行) で行う。
 * 落ちかけ・固まりかけのプロセスの中で全スレッドを止めて書き出すと、止めたスレッドが持っている
 * ロック (ヒープ・ローダー・CRT) 待ちで記録処理自体が固まったり落ちたりするため。
 * ダンプファイルは WinDbg / Visual Studio で開ける (MiniDumpWriteDump)。
 *
 * あわせて、ゲームの ExitProcess / TerminateProcess の呼び出し元と、ダイアログ API の出入りを mkwrap.log に記録する。 */
#include "mkw.h"
#include <tlhelp32.h>
#include <string.h>

/* ---------------- 直前の例外 (first chance) の記録 ---------------- */
#define RING 32
typedef struct { DWORD code, addr, tid, tick; } ring_ent_t;
typedef struct {
    char       magic[8];      /* "MKWDIAG" */
    volatile LONG pos;
    ring_ent_t ent[RING];
} diag_ring_t;
static diag_ring_t g_ring = { "MKWDIAG", 0, {{0}} };

static volatile LONG g_crashing;
static HANDLE g_evt_go, g_evt_done;
static EXCEPTION_POINTERS *g_cep;
static DWORD g_ctid;
static LPTOP_LEVEL_EXCEPTION_FILTER g_game_filter;
static int g_enabled;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    LONG i;
    if (code == 0x406D1388 /* スレッド名 */ || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    i = InterlockedIncrement(&g_ring.pos) - 1;
    g_ring.ent[i % RING].code = code;
    g_ring.ent[i % RING].addr = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    g_ring.ent[i % RING].tid = GetCurrentThreadId();
    g_ring.ent[i % RING].tick = GetTickCount();
    /* スタック溢れは通常の例外処理まで進めないことがあるので、ここで記録する */
    if (code == EXCEPTION_STACK_OVERFLOW && g_enabled && !InterlockedExchange(&g_crashing, 1)) {
        g_cep = ep; g_ctid = GetCurrentThreadId();
        SetEvent(g_evt_go);
        WaitForSingleObject(g_evt_done, 90000);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    if (g_enabled && !InterlockedExchange(&g_crashing, 1)) {
        g_cep = ep; g_ctid = GetCurrentThreadId();
        SetEvent(g_evt_go);
        WaitForSingleObject(g_evt_done, 90000);
    }
    if (g_game_filter) return g_game_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ゲームが自分のフィルタを設定しても、記録用のフィルタを先に通す */
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_game_filter;
    g_game_filter = f;
    return old;
}

static DWORD WINAPI reporter_thread(LPVOID p)
{
    (void)p;
    WaitForSingleObject(g_evt_go, INFINITE);
    diag_write_header(L"mkwrap_crash.log", "異常終了 (例外)");
    diag_launch_helper("crash", g_ctid, g_cep);
    SetEvent(g_evt_done);
    return 0;
}

/* ---------------- 記録用ヘルパーの起動 ---------------- */
static int is_wow64(void)
{
    BOOL (WINAPI *f)(HANDLE, PBOOL) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    BOOL b = FALSE;
    return f && f(GetCurrentProcess(), &b) && b;
}

void diag_write_header(const wchar_t *file, const char *title)
{
    wchar_t path[MAX_PATH]; HANDLE f; char buf[512]; DWORD n; SYSTEMTIME st; OSVERSIONINFOA ov;
    char exe[MAX_PATH] = "";
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", g_gamedir, file);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&st);
    memset(&ov, 0, sizeof ov); ov.dwOSVersionInfoSize = sizeof ov; GetVersionExA(&ov);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    n = wsprintfA(buf, "===== MKWrap " MKW_VERSION " %s %04d-%02d-%02d %02d:%02d:%02d.%03d =====\r\n%s\r\nWindows %lu.%lu.%lu%s\r\n",
                  title, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, exe,
                  ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, is_wow64() ? " (64bit)" : "");
    WriteFile(f, buf, n, &n, NULL);
    CloseHandle(f);
}

void diag_launch_helper(const char *kind, DWORD tid, void *ep)
{
    wchar_t sys[MAX_PATH], dll[MAX_PATH], cmd[1024];
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    if (!GetSystemWow64DirectoryW(sys, MAX_PATH)) GetSystemDirectoryW(sys, MAX_PATH);
    GetModuleFileNameW(g_self, dll, MAX_PATH);
    _snwprintf(cmd, 1024, L"\"%ls\\rundll32.exe\" \"%ls\",MkwDump %lu %lu %lu %lu %hs",
               sys, dll, (unsigned long)GetCurrentProcessId(), (unsigned long)tid,
               (unsigned long)(ULONG_PTR)ep, (unsigned long)(ULONG_PTR)&g_ring, kind);
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_gamedir, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

/* ---------------- 終了・ダイアログの記録 ---------------- */
static char *addr_name(DWORD a, char *out, int n)
{
    MEMORY_BASIC_INFORMATION mbi; char path[MAX_PATH], *b;
    if (VirtualQuery((void *)(ULONG_PTR)a, &mbi, sizeof mbi) && mbi.AllocationBase &&
        GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
        b = strrchr(path, '\\'); b = b ? b + 1 : path;
        _snprintf(out, n, "%s+%lx", b, (unsigned long)(a - (DWORD)(ULONG_PTR)mbi.AllocationBase));
    } else _snprintf(out, n, "%08lx", (unsigned long)a);
    out[n - 1] = 0;
    return out;
}
#define CALLER() ((DWORD)(ULONG_PTR)__builtin_return_address(0))

static void (WINAPI *r_ExitProcess)(UINT);
static BOOL (WINAPI *r_TerminateProcess)(HANDLE, UINT);
static void (WINAPI *r_FatalAppExitA)(UINT, LPCSTR);
static void WINAPI hk_ExitProcess(UINT c)
{
    char a[96];
    mkw_log("ExitProcess(%u) 呼出元 %s\n", c, addr_name(CALLER(), a, sizeof a));
    r_ExitProcess(c);
}
static BOOL WINAPI hk_TerminateProcess(HANDLE h, UINT c)
{
    char a[96];
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        mkw_log("TerminateProcess(%u) 呼出元 %s\n", c, addr_name(CALLER(), a, sizeof a));
        diag_write_header(L"mkwrap_crash.log", "TerminateProcess による終了");
    }
    return r_TerminateProcess(h, c);
}
static void WINAPI hk_FatalAppExitA(UINT u, LPCSTR m)
{
    char a[96];
    mkw_log("FatalAppExit: %s 呼出元 %s\n", m ? m : "", addr_name(CALLER(), a, sizeof a));
    diag_write_header(L"mkwrap_crash.log", "FatalAppExit による終了");
    r_FatalAppExitA(u, m);
}

/* ダイアログは DPI 非対応として作る (Windows が拡大表示する。1998 年当時の作りのまま動かすため) */
static HANDLE (WINAPI *p_SetThreadDpiAwarenessContext)(HANDLE);
static int g_proc_dpi_aware;
static HANDLE dpi_enter(void)
{
    HANDLE o;
    if (!g_cfg.dialog_dpi || !g_proc_dpi_aware || !p_SetThreadDpiAwarenessContext) return NULL;
    o = p_SetThreadDpiAwarenessContext((HANDLE)-5 /* UNAWARE_GDISCALED */);
    if (!o) o = p_SetThreadDpiAwarenessContext((HANDLE)-1 /* UNAWARE */);
    return o;
}
static void dpi_leave(HANDLE o) { if (o) p_SetThreadDpiAwarenessContext(o); }

typedef HWND (WINAPI *CDIP_t)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
typedef HWND (WINAPI *CDP_t)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef INT_PTR (WINAPI *DBP_t)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef INT_PTR (WINAPI *DBIP_t)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
typedef INT_PTR (WINAPI *PS_t)(void *);
static CDIP_t r_CreateDialogIndirectParamA;
static CDP_t r_CreateDialogParamA;
static DBP_t r_DialogBoxParamA;
static DBIP_t r_DialogBoxIndirectParamA;
static PS_t r_PropertySheetA;

static HWND WINAPI hk_CreateDialogIndirectParamA(HINSTANCE i, LPCDLGTEMPLATEA t, HWND o, DLGPROC p, LPARAM l)
{
    HANDLE c = dpi_enter(); HWND r;
    mkw_log("CreateDialogIndirectParam 開始 owner=%p%s\n", (void *)o, c ? " (DPI 非対応で作成)" : "");
    r = r_CreateDialogIndirectParamA(i, t, o, p, l);
    dpi_leave(c);
    mkw_log("CreateDialogIndirectParam 終了 -> %p\n", (void *)r);
    return r;
}
static HWND WINAPI hk_CreateDialogParamA(HINSTANCE i, LPCSTR t, HWND o, DLGPROC p, LPARAM l)
{
    HANDLE c = dpi_enter(); HWND r;
    mkw_log("CreateDialogParam 開始 owner=%p%s\n", (void *)o, c ? " (DPI 非対応で作成)" : "");
    r = r_CreateDialogParamA(i, t, o, p, l);
    dpi_leave(c);
    mkw_log("CreateDialogParam 終了 -> %p\n", (void *)r);
    return r;
}
/* DialogBox はモーダルループの間も DPI の設定が続くが、作成後に作られる窓もダイアログの子なので問題ない */
static INT_PTR WINAPI hk_DialogBoxParamA(HINSTANCE i, LPCSTR t, HWND o, DLGPROC p, LPARAM l)
{
    HANDLE c = dpi_enter(); INT_PTR r;
    mkw_log("DialogBoxParam 開始 owner=%p\n", (void *)o);
    r = r_DialogBoxParamA(i, t, o, p, l);
    dpi_leave(c);
    mkw_log("DialogBoxParam 終了 -> %ld\n", (long)r);
    return r;
}
static INT_PTR WINAPI hk_DialogBoxIndirectParamA(HINSTANCE i, LPCDLGTEMPLATEA t, HWND o, DLGPROC p, LPARAM l)
{
    HANDLE c = dpi_enter(); INT_PTR r;
    mkw_log("DialogBoxIndirectParam 開始 owner=%p\n", (void *)o);
    r = r_DialogBoxIndirectParamA(i, t, o, p, l);
    dpi_leave(c);
    mkw_log("DialogBoxIndirectParam 終了 -> %ld\n", (long)r);
    return r;
}
static INT_PTR WINAPI hk_PropertySheetA(void *psh)
{
    HANDLE c = dpi_enter(); INT_PTR r;
    mkw_log("PropertySheet 開始%s\n", c ? " (DPI 非対応で作成)" : "");
    r = r_PropertySheetA(psh);
    dpi_leave(c);
    mkw_log("PropertySheet 終了 -> %lx\n", (long)r);
    return r;
}

/* Windows 11 対策: 窓ハンドル NULL での DefWindowProcA
 * MFC 製のゲーム (リブルラブル / ドラゴンバスター / 妖怪道中記) の設定ダイアログは、WM_CTLCOLOR の既定処理を
 * 窓を持たない CWnd オブジェクトに転送しており、結果として DefWindowProcA(NULL, WM_CTLCOLORDLG, ...) を呼ぶ。
 * 以前の Windows では何もせず 0 を返していたが、Windows 11 の USER32 はここで NULL を参照して落ちる。
 * NULL の場合、WM_CTLCOLOR* なら対象のコントロール (lParam) の窓で既定処理を行い、それ以外は 0 を返す。 */
static LRESULT (WINAPI *r_DefWindowProcA)(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI hk_DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (!h) {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 3) {
            char a[96];
            mkw_log("DefWindowProcA(NULL, %04x) を補正 呼出元 %s\n", m, addr_name(CALLER(), a, sizeof a));
        }
        if (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC && lp && IsWindow((HWND)lp))
            return r_DefWindowProcA((HWND)lp, m, wp, lp);
        return 0;
    }
    return r_DefWindowProcA(h, m, wp, lp);
}

static void *getp(const char *dll, const char *fn)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? (void *)GetProcAddress(m, fn) : NULL;
}

void crash_hooks_install(void)
{
    r_ExitProcess = getp("kernel32.dll", "ExitProcess");
    r_TerminateProcess = getp("kernel32.dll", "TerminateProcess");
    r_FatalAppExitA = getp("kernel32.dll", "FatalAppExitA");
    iat_hook(g_exe, "kernel32.dll", "ExitProcess", (void *)hk_ExitProcess);
    iat_hook(g_exe, "kernel32.dll", "TerminateProcess", (void *)hk_TerminateProcess);
    iat_hook(g_exe, "kernel32.dll", "FatalAppExitA", (void *)hk_FatalAppExitA);
    if (g_enabled) iat_hook(g_exe, "kernel32.dll", "SetUnhandledExceptionFilter", (void *)hk_SetUnhandledExceptionFilter);

    {   /* 現在のプロセスが DPI 対応かどうか */
        HANDLE (WINAPI *gt)(void) = getp("user32.dll", "GetThreadDpiAwarenessContext");
        int (WINAPI *ga)(HANDLE) = getp("user32.dll", "GetAwarenessFromDpiAwarenessContext");
        p_SetThreadDpiAwarenessContext = getp("user32.dll", "SetThreadDpiAwarenessContext");
        g_proc_dpi_aware = gt && ga && ga(gt()) != 0;
    }
    r_DefWindowProcA = getp("user32.dll", "DefWindowProcA");
    if (GetPrivateProfileIntW(L"Patch", L"FixNullDefWindowProc", 1, g_ini))
        iat_hook(g_exe, "user32.dll", "DefWindowProcA", (void *)hk_DefWindowProcA);
    r_CreateDialogIndirectParamA = getp("user32.dll", "CreateDialogIndirectParamA");
    r_CreateDialogParamA = getp("user32.dll", "CreateDialogParamA");
    r_DialogBoxParamA = getp("user32.dll", "DialogBoxParamA");
    r_DialogBoxIndirectParamA = getp("user32.dll", "DialogBoxIndirectParamA");
    iat_hook(g_exe, "user32.dll", "CreateDialogIndirectParamA", (void *)hk_CreateDialogIndirectParamA);
    iat_hook(g_exe, "user32.dll", "CreateDialogParamA", (void *)hk_CreateDialogParamA);
    iat_hook(g_exe, "user32.dll", "DialogBoxParamA", (void *)hk_DialogBoxParamA);
    iat_hook(g_exe, "user32.dll", "DialogBoxIndirectParamA", (void *)hk_DialogBoxIndirectParamA);
    r_PropertySheetA = getp("comctl32.dll", "PropertySheetA");
    if (r_PropertySheetA) iat_hook(g_exe, "comctl32.dll", "PropertySheetA", (void *)hk_PropertySheetA);
}

void crash_init(void)
{
    g_enabled = GetPrivateProfileIntW(L"Debug", L"CrashReport", 1, g_ini) != 0;
    AddVectoredExceptionHandler(1, veh);
    if (!g_enabled) return;
    g_evt_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_evt_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, reporter_thread, NULL, 0, NULL));
    SetUnhandledExceptionFilter(crash_filter);
}
