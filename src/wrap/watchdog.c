/* 固まった時の調査用: 窓のスレッドが応答しなくなったら、全スレッドの状態を mkwrap_hang.log と
 * mkwrap_hang.dmp に書き出す (記録は別プロセスで行う。crash.c / diag_helper.c)。
 * [Debug] HangReport=0 で無効 */
#include "mkw.h"

/* ---- ファイルを開く処理の監視 (Log=1): 失敗した・時間がかかった CreateFileA を記録 ---- */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static volatile LONG g_fm_calls, g_fm_fail;
static char g_fm_last[MAX_PATH];
static volatile double g_fm_start;
static volatile int g_fm_inside;
static DWORD g_fm_lasterr;
static int g_fm_nlog;
/* 作業フォルダの見張り: ドライバなど (ゲーム以外) が作業フォルダを書き換えると、
 * 相対パスでデータを開くゲームが「ファイルが見つからない」まま止まるため、起動時の作業フォルダに戻す */
static wchar_t g_start_cwd[MAX_PATH];
void mkw_fix_cwd(const char *where)
{
    wchar_t cur[MAX_PATH];
    if (!g_start_cwd[0]) return;
    if (!GetCurrentDirectoryW(MAX_PATH, cur)) return;
    if (_wcsicmp(cur, g_start_cwd) == 0) return;
    SetCurrentDirectoryW(g_start_cwd);
    {
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, cur, -1, a, sizeof a, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, g_start_cwd, -1, b, sizeof b, NULL, NULL);
        mkw_log("作業フォルダが %s に変わっていたので %s に戻しました (%s)\n", a, b, where);
    }
}
static int is_relative(const char *p)
{
    if (!p || !p[0]) return 0;
    if (p[0] == '\\' || p[0] == '/') return 0;
    if (p[1] == ':') return 0;
    return 1;
}
/* "DAT\INIT_DAT.BIN" のようなフォルダ付きの相対パスが見つからないとき、ゲームのフォルダ直下に
 * 同じ名前のファイルがあればそれを開く (古い MKSetup などでフォルダ無しで展開されていた場合の救済) */
static HANDLE flat_fallback(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t, HANDLE h)
{
    DWORD err = GetLastError(); const char *b; char alt[MAX_PATH * 2], dir[MAX_PATH * 2]; HANDLE h2;
    static int nlog;
    if ((err != ERROR_PATH_NOT_FOUND && err != ERROR_FILE_NOT_FOUND) || !is_relative(p) || (c != OPEN_EXISTING && c != OPEN_ALWAYS)) return h;
    b = strrchr(p, '\\'); if (!b) b = strrchr(p, '/');
    if (!b || !b[1]) return h;
    if (!WideCharToMultiByte(CP_ACP, 0, g_gamedir, -1, dir, sizeof dir, NULL, NULL)) return h;
    _snprintf(alt, sizeof alt, "%s\\%s", dir, b + 1); alt[sizeof alt - 1] = 0;
    h2 = o_CreateFileA(alt, a, s, sa, c, f, t);
    if (h2 == INVALID_HANDLE_VALUE) { SetLastError(err); return h; }
    if (nlog < 5) { nlog++; mkw_log("%s が見つからないため、ゲームのフォルダ直下の %s を開きました (フォルダ無しで展開されています。MKSetup 1.6.5 以降で展開し直すと解消します)\n", p, b + 1); }
    return h2;
}
static HANDLE WINAPI fm_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    HANDLE h; DWORD err; double t0 = mkw_now(), dt;
    if (is_relative(p)) mkw_fix_cwd(p);
    if (!g_cfg.log) {
        h = o_CreateFileA(p, a, s, sa, c, f, t);
        if (h == INVALID_HANDLE_VALUE) h = flat_fallback(p, a, s, sa, c, f, t, h);
        return h;
    }
    if (p) lstrcpynA(g_fm_last, p, MAX_PATH); else g_fm_last[0] = 0;
    g_fm_start = t0; g_fm_inside = 1;
    h = o_CreateFileA(p, a, s, sa, c, f, t);
    err = GetLastError();
    if (h == INVALID_HANDLE_VALUE) { h = flat_fallback(p, a, s, sa, c, f, t, h); if (h != INVALID_HANDLE_VALUE) err = 0; }
    g_fm_inside = 0;
    dt = mkw_now() - t0;
    InterlockedIncrement(&g_fm_calls);
    if (h == INVALID_HANDLE_VALUE) { InterlockedIncrement(&g_fm_fail); g_fm_lasterr = err; }
    if ((h == INVALID_HANDLE_VALUE || dt > 0.1 || g_cfg.log >= 2) && g_fm_nlog < 200) {
        g_fm_nlog++;
        mkw_log("CreateFileA \"%s\" access=%lx share=%lx disp=%lu → %s (エラー %lu, %.0f ms)\n", p ? p : "(null)",
                (unsigned long)a, (unsigned long)s, (unsigned long)c, h == INVALID_HANDLE_VALUE ? "失敗" : "成功", (unsigned long)err, dt * 1000);
    }
    SetLastError(err);
    return h;
}
/* 調査用 (Log=1): MsgWaitForMultipleObjects の結果を数える (DirectSound の通知などで使われる) */
typedef DWORD (WINAPI *MWFMO_t)(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
static MWFMO_t o_MsgWait;
static volatile LONG g_mw_calls, g_mw_obj, g_mw_input, g_mw_timeout;
/* Windows の MsgWaitForMultipleObjects は、窓のメッセージ (タイマーや再描画など) が来ていると待ち対象より先に
 * 「入力あり」を返す。ロックマン DASH は毎フレーム時間 0 で呼んで DirectSound の通知 (ストリーミング再生の
 * 次の部分の読み込み) を調べるため、MKWrap の表示用タイマーなどで常に「入力あり」になると読み込みが行われず、
 * 数秒分の音声が繰り返し流れていた。待ち対象を先に調べる */
static DWORD WINAPI fm_MsgWait(DWORD n, const HANDLE *h, BOOL all, DWORD ms, DWORD mask)
{
    DWORD r;
    if (n > 0 && h && !all) {
        r = WaitForMultipleObjects(n, h, FALSE, 0);
        if (r < WAIT_OBJECT_0 + n) goto done;
    }
    r = o_MsgWait(n, h, all, ms, mask);
done:
    InterlockedIncrement(&g_mw_calls);
    if (r < n) InterlockedIncrement(&g_mw_obj);
    else if (r == n) InterlockedIncrement(&g_mw_input);
    else if (r == WAIT_TIMEOUT) InterlockedIncrement(&g_mw_timeout);
    return r;
}
void msgwait_report(void)
{
    static LONG lc, lo, li, lt;
    if (!o_MsgWait || !g_cfg.log || g_mw_calls == lc) return;
    mkw_log("MsgWaitForMultipleObjects: %ld 回 (待ち対象が通知 %ld / 入力あり %ld / 時間切れ %ld)\n",
            g_mw_calls - lc, g_mw_obj - lo, g_mw_input - li, g_mw_timeout - lt);
    lc = g_mw_calls; lo = g_mw_obj; li = g_mw_input; lt = g_mw_timeout;
}
void filemon_install(void)
{
    void *o;
    GetCurrentDirectoryW(MAX_PATH, g_start_cwd);
    o = iat_hook(g_exe, "kernel32.dll", "CreateFileA", (void *)fm_CreateFileA);
    {
        void *m = iat_hook(g_exe, "user32.dll", "MsgWaitForMultipleObjects", (void *)fm_MsgWait);
        o_MsgWait = m ? (MWFMO_t)m : (MWFMO_t)GetProcAddress(GetModuleHandleA("user32.dll"), "MsgWaitForMultipleObjects");
    }
    o_CreateFileA = o ? (CreateFileA_t)o : (CreateFileA_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
}
/* 調査用: ゲームの実行ファイルの書き込み可能な領域 (変数) をそのまま書き出す (ミニダンプに入らないため) */
static void dump_exe_data(void)
{
    BYTE *base = (BYTE *)g_exe; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt); int i; wchar_t p[MAX_PATH]; FILE *f;
    _snwprintf(p, MAX_PATH, L"%ls\\mkwrap_hang_data.bin", g_gamedir);
    f = _wfopen(p, L"wb");
    if (!f) return;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD va = sec[i].VirtualAddress, sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        DWORD hdr[2]; hdr[0] = (DWORD)(base + va); hdr[1] = sz;
        fwrite(hdr, 4, 2, f); fwrite(base + va, 1, sz, f);
    }
    fclose(f);
    mkw_log("ゲームの変数の領域を mkwrap_hang_data.bin に書き出しました\n");
}
static void filemon_report(void)
{
    static LONG last_calls, last_fail;
    if (!o_CreateFileA || !g_cfg.log) return;
    mkw_log("  直近の CreateFileA: %ld 回 (うち失敗 %ld 回、最後のエラー %lu)、最後のファイル \"%s\"%s\n",
            g_fm_calls - last_calls, g_fm_fail - last_fail, (unsigned long)g_fm_lasterr, g_fm_last, g_fm_inside ? "" : "");
    if (g_fm_inside) mkw_log("  → このファイルを開く処理が %.1f 秒戻ってきていません\n", mkw_now() - g_fm_start);
    last_calls = g_fm_calls; last_fail = g_fm_fail;
}

static DWORD WINAPI watchdog_thread(LPVOID p)
{
    int dumped = 0, bad = 0, total = 0, still = 0, stall_done = 0;
    LONG last_frames = 0;
    (void)p;
    for (;;) {
        vwin_t *w;
        DWORD_PTR res;
        Sleep(1000);
        w = vwin_main();
        if (!w || !w->hwnd || !IsWindow(w->hwnd) || g_paused) { bad = 0; still = 0; continue; }
        if (SendMessageTimeoutA(w->hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 2000, &res)) {
            if (dumped) mkw_log("応答が戻りました\n");
            bad = 0; dumped = 0;
            /* 窓は応答しているのに、ゲームが画面を更新しなくなった (Log=1 のとき 1 回だけ記録) */
            if ((g_cfg.log || g_cfg.keep_active) && !stall_done && g_mkw_frames > 0 && !IsIconic(w->hwnd)) {
                if (g_mkw_frames == last_frames) {
                    if (++still == 8 && g_cfg.log) {
                        stall_done = 1;
                        filemon_report();
                        mkw_log("8 秒間画面の更新がありません。調査用に全スレッドの状態を mkwrap_hang.log / mkwrap_hang.dmp に記録します\n");
                        diag_write_header(L"mkwrap_hang.log", "画面の更新が止まった (窓は応答している)");
                        dump_exe_data();
                        diag_launch_helper("hang", w->tid, NULL);
                    }
                } else still = 0;
                if (still == 4) filemon_report();   /* 止まり始めの様子 */
                if (still == 3 && g_cfg.keep_active) {
                    /* 非アクティブのつもりで止まっているゲームのために、アクティブになったことを伝え直す */
                    mkw_log("画面の更新が止まっているため、ゲームにアクティブ (WM_ACTIVATEAPP 1) を伝え直します\n");
                    PostMessageA(w->hwnd, WM_ACTIVATEAPP, TRUE, 0);
                }
            }
            last_frames = g_mkw_frames;
            continue;
        }
        if (++bad >= 2 && !dumped && total < 3) {
            dumped = 1; total++;
            mkw_log("応答なしを検出しました。mkwrap_hang.log と mkwrap_hang.dmp に記録します\n");
            diag_write_header(L"mkwrap_hang.log", "応答なし");
            diag_launch_helper("hang", w->tid, NULL);
            mkw_log("記録しました\n");
        }
    }
    return 0;
}

void watchdog_start(void)
{
    if (!GetPrivateProfileIntW(L"Debug", L"HangReport", 1, g_ini)) return;
    CloseHandle(CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL));
}
