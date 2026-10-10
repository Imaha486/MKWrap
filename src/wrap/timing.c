/* タイミング制御: 画面転送のレート制限、時計の倍率、timeSetEvent の置き換え、一時停止 */
#include "mkw.h"
#include <mmsystem.h>
#include <string.h>

volatile int g_paused;
static LARGE_INTEGER g_freq, g_t0;
static double g_next_frame;
static int g_frames; static double g_fps_t0;
static volatile LONG g_cnt_tgt, g_cnt_gtc;
static struct { void *ra; LONG n; } g_callers[16];
static void note_caller(void *ra)
{
    int i;
    if (!g_cfg.log) return;
    for (i = 0; i < 16; i++) { if (g_callers[i].ra == ra) { g_callers[i].n++; return; } if (!g_callers[i].ra) { g_callers[i].ra = ra; g_callers[i].n = 1; return; } }
}

double mkw_now(void)
{
    LARGE_INTEGER t;
    if (!g_freq.QuadPart) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
}

static void wait_until(double t)
{
    double now;
    while ((now = mkw_now()) < t - 0.0025) Sleep(1);
    while (mkw_now() < t) YieldProcessor();
}

void frame_limit(void)
{
    double now = mkw_now();
    g_frames++;
    if (g_cfg.log && now - g_fps_t0 >= 5.0) {
        mkw_log("画面転送 %.3f fps / GetTickCount %.1f/s timeGetTime %.1f/s\n", g_frames / (now - g_fps_t0), g_cnt_gtc / (now - g_fps_t0), g_cnt_tgt / (now - g_fps_t0));
        g_cnt_gtc = g_cnt_tgt = 0;
        { int i; for (i = 0; i < 16 && g_callers[i].ra; i++) { mkw_log("  呼出元 %p: %.1f/s\n", g_callers[i].ra, g_callers[i].n / 5.0); g_callers[i].n = 0; } }
        g_frames = 0; g_fps_t0 = now;
    }
    if (g_cfg.refresh <= 0) return;
    {
        double period = 1.0 / g_cfg.refresh;
        if (g_next_frame == 0 || now - g_next_frame > period * 2) g_next_frame = now;
        g_next_frame += period;
        if (g_next_frame > now) wait_until(g_next_frame);
    }
}

/* ---------------- 一時停止 ---------------- */
void dsound_pause(int pause);
void title_pause(int on);
static void set_title_pause(int on) { title_pause(on); }
void title_pause(int on)
{
    vwin_t *w = vwin_main();
    char t[512]; int n;
    if (!w || !w->hwnd) return;
    n = GetWindowTextA(w->hwnd, t, sizeof t - 16);
    if (on) { if (n < 8 || strcmp(t + n - 8, " - Pause")) { strcat(t, " - Pause"); SetWindowTextA(w->hwnd, t); } }
    else if (n >= 8 && !strcmp(t + n - 8, " - Pause")) { t[n - 8] = 0; SetWindowTextA(w->hwnd, t); }
}

static volatile LONG g_in_pause;
void pause_wait_if_paused(void)
{
    MSG m;
    if (!g_paused || InterlockedExchange(&g_in_pause, 1)) return;
    mkw_log("一時停止\n");
    set_title_pause(1);
    dsound_pause(1);
    while (g_paused) {
        input_poll();
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); g_paused = 0; break; }
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (g_main && g_main->cdc) vwin_present(g_main, 0);
        MsgWaitForMultipleObjects(0, NULL, FALSE, 15, QS_ALLINPUT);
    }
    dsound_pause(0);
    set_title_pause(0);
    g_next_frame = 0;
    mkw_log("再開\n");
    InterlockedExchange(&g_in_pause, 0);
}

/* ---------------- 時計 ---------------- */
static DWORD (WINAPI *r_timeGetTime)(void);
static DWORD (WINAPI *r_GetTickCount)(void);
static BOOL (WINAPI *r_QueryPerformanceCounter)(LARGE_INTEGER *);
static MMRESULT (WINAPI *r_timeSetEvent)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
static MMRESULT (WINAPI *r_timeKillEvent)(UINT);

/* QPC を元にした高精度・倍率付きのミリ秒時計 */
static DWORD g_tick_base;
static DWORD virt_ms(void)
{
    return g_tick_base + (DWORD)(LONGLONG)(mkw_now() * 1000.0 * g_cfg.time_scale);
}
static DWORD WINAPI hk_timeGetTime(void) { g_cnt_tgt++; note_caller(__builtin_return_address(0)); return virt_ms(); }
static DWORD WINAPI hk_GetTickCount(void) { g_cnt_gtc++; note_caller(__builtin_return_address(0)); return virt_ms(); }
static LONGLONG g_qpc_br, g_qpc_bv;
static BOOL WINAPI hk_QueryPerformanceCounter(LARGE_INTEGER *c)
{
    BOOL r = r_QueryPerformanceCounter(c);
    if (r && c) {
        if (!g_qpc_br) { g_qpc_br = c->QuadPart; g_qpc_bv = c->QuadPart; }
        c->QuadPart = g_qpc_bv + (LONGLONG)((double)(c->QuadPart - g_qpc_br) * g_cfg.time_scale);
    }
    return r;
}

/* timeSetEvent を高精度タイマースレッドに置き換え */
#define MAXTIMERS 8
typedef struct { volatile int active; UINT id; LPTIMECALLBACK cb; DWORD_PTR user; UINT flags; double period; HANDLE th; } mtimer_t;
static mtimer_t g_timers[MAXTIMERS];

static DWORD WINAPI timer_thread(LPVOID p)
{
    mtimer_t *t = (mtimer_t *)p;
    double next = mkw_now();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (t->active) {
        next += t->period;
        if (mkw_now() - next > t->period * 4) next = mkw_now();
        wait_until(next);
        if (!t->active) break;
        if (g_paused) continue;
        if (t->flags & TIME_CALLBACK_EVENT_SET) SetEvent((HANDLE)t->cb);
        else if (t->flags & TIME_CALLBACK_EVENT_PULSE) PulseEvent((HANDLE)t->cb);
        else t->cb(t->id, 0, t->user, 0, 0);
        if (!(t->flags & TIME_PERIODIC)) { t->active = 0; break; }
    }
    return 0;
}

static MMRESULT WINAPI hk_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags)
{
    int i;
    mkw_log("timeSetEvent(delay=%u res=%u flags=%x)\n", delay, res, flags);
    if (g_cfg.timer_hz <= 0 || !(flags & TIME_PERIODIC)) return r_timeSetEvent(delay, res, cb, user, flags);
    for (i = 0; i < MAXTIMERS; i++) {
        if (!g_timers[i].active && !g_timers[i].th) {
            mtimer_t *t = &g_timers[i];
            t->id = 0x4D4B0000 + i; t->cb = cb; t->user = user; t->flags = flags;
            t->period = 1.0 / g_cfg.timer_hz;
            t->active = 1;
            t->th = CreateThread(NULL, 0, timer_thread, t, 0, NULL);
            mkw_log(" -> %.3f Hz の内部タイマーで置き換え\n", g_cfg.timer_hz);
            return t->id;
        }
    }
    return r_timeSetEvent(delay, res, cb, user, flags);
}
static MMRESULT WINAPI hk_timeKillEvent(UINT id)
{
    if ((id & 0xFFFF0000) == 0x4D4B0000 && (id & 0xFFFF) < MAXTIMERS) {
        mtimer_t *t = &g_timers[id & 0xFFFF];
        t->active = 0;
        if (t->th) {
            if (GetThreadId(t->th) != GetCurrentThreadId()) WaitForSingleObject(t->th, 500);
            CloseHandle(t->th); t->th = NULL;
        }
        return TIMERR_NOERROR;
    }
    return r_timeKillEvent(id);
}

void timing_hooks_install(void)
{
    HMODULE wm = LoadLibraryA("winmm.dll");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    mkw_now();
    timeBeginPeriod(1);
    r_timeGetTime = (void *)GetProcAddress(wm, "timeGetTime");
    r_timeSetEvent = (void *)GetProcAddress(wm, "timeSetEvent");
    r_timeKillEvent = (void *)GetProcAddress(wm, "timeKillEvent");
    r_GetTickCount = (void *)GetProcAddress(k32, "GetTickCount");
    r_QueryPerformanceCounter = (void *)GetProcAddress(k32, "QueryPerformanceCounter");
    g_tick_base = r_GetTickCount() - (DWORD)(mkw_now() * 1000.0);
    if (g_cfg.time_scale != 1.0 || g_cfg.precise_timer) {
        iat_hook(g_exe, "winmm.dll", "timeGetTime", (void *)hk_timeGetTime);
        iat_hook(g_exe, "kernel32.dll", "GetTickCount", (void *)hk_GetTickCount);
    }
    if (g_cfg.time_scale != 1.0)
        iat_hook(g_exe, "kernel32.dll", "QueryPerformanceCounter", (void *)hk_QueryPerformanceCounter);
    iat_hook(g_exe, "winmm.dll", "timeSetEvent", (void *)hk_timeSetEvent);
    iat_hook(g_exe, "winmm.dll", "timeKillEvent", (void *)hk_timeKillEvent);
    { void debug_irq_hooks_install(void); debug_irq_hooks_install(); }
}

/* ---- 調査用: CPU エミュレータ DLL の割り込み要求回数を数える (Log=1 のとき) ---- */
static void (__stdcall *r_z80irq)(void *, int, int);
static void (__fastcall *r_6809irq)(void *, int, int);
static volatile LONG g_irq_cnt;
static double g_irq_t0;
static void irq_note(void)
{
    double now = mkw_now();
    g_irq_cnt++;
    if (now - g_irq_t0 >= 5.0) { mkw_log("CPU割り込み要求 %.2f/s\n", g_irq_cnt / (now - g_irq_t0)); g_irq_cnt = 0; g_irq_t0 = now; }
}
static void *r_z80irq_raw;
__attribute__((naked)) static void hk_z80irq(void)
{
    /* 呼出規約に依存しないよう、レジスタとスタックを保ったまま転送 */
    __asm__ volatile(
        "pushal\n\t"
        "push %ecx\n\t"
        "push 40(%esp)\n\t"
        "push 40(%esp)\n\t"
        "call _irq_note_c\n\t"
        "add $12, %esp\n\t"
        "popal\n\t"
        "jmp *_r_z80irq_raw_v\n\t");
}
static struct { DWORD a, b, c; LONG n; } g_irqh[16];
void __cdecl irq_note_c(DWORD a1, DWORD a2, DWORD ecx)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (g_irqh[i].n && g_irqh[i].a == a1 && g_irqh[i].b == a2 && g_irqh[i].c == ecx) { g_irqh[i].n++; break; }
        if (!g_irqh[i].n) { g_irqh[i].a = a1; g_irqh[i].b = a2; g_irqh[i].c = ecx; g_irqh[i].n = 1; break; }
    }
    if (mkw_now() - g_irq_t0 >= 5.0) for (i = 0; i < 16 && g_irqh[i].n; i++) { mkw_log("  irq args %08lx %08lx ecx=%08lx : %.1f/s\n", g_irqh[i].a, g_irqh[i].b, g_irqh[i].c, g_irqh[i].n / 5.0); g_irqh[i].n = 0; }
    irq_note();
}
void *g_irq_stub = (void *)hk_z80irq;
void *r_z80irq_raw_v;
static void *r_6809irq_raw_v_dummy;
void debug_irq_hooks_install(void)
{
    void *o;
    (void)r_z80irq; (void)r_6809irq; (void)r_z80irq_raw; (void)r_6809irq_raw_v_dummy;
    if (!g_cfg.log || !GetPrivateProfileIntW(L"Debug", L"MeasureIRQ", 0, g_ini)) return;
    o = iat_hook(g_exe, "Z80EMU.dll", "Z80EMU_RequestInterrupt", (void *)hk_z80irq);
    if (o) { r_z80irq_raw_v = o; mkw_log("Z80EMU_RequestInterrupt を計測\n"); return; }
    o = iat_hook(g_exe, "DLL6809.dll", "?MC6809EMU_RequestInterrupt@@YIXPAUMC6809EMU@@HH@Z", (void *)hk_z80irq);
    if (o) { r_z80irq_raw_v = o; mkw_log("MC6809EMU_RequestInterrupt を計測\n"); }
}
