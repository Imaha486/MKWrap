/* 固まった時の調査用: 窓のスレッドが応答しなくなったら、全スレッドの状態を mkwrap_hang.log と
 * mkwrap_hang.dmp に書き出す (記録は別プロセスで行う。crash.c / diag_helper.c)。
 * [Debug] HangReport=0 で無効 */
#include "mkw.h"

static DWORD WINAPI watchdog_thread(LPVOID p)
{
    int dumped = 0, bad = 0, total = 0;
    (void)p;
    for (;;) {
        vwin_t *w;
        DWORD_PTR res;
        Sleep(1000);
        w = vwin_main();
        if (!w || !w->hwnd || !IsWindow(w->hwnd) || g_paused) { bad = 0; continue; }
        if (SendMessageTimeoutA(w->hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 2000, &res)) {
            if (dumped) mkw_log("応答が戻りました\n");
            bad = 0; dumped = 0; continue;
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
