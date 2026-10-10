/* INI の [Patch] による実行ファイルの書き換え (タイトル別の調整用)
 *   名前=アドレス,bytes,XX XX ...[,expect=XX XX ...]
 *   名前=アドレス,framewait,再開アドレス[,expect=XX XX ...]
 *       アドレスに「MKWrap のフレーム待ち関数呼び出し + 再開アドレスへのジャンプ」を書き込む */
#include "mkw.h"
#include <string.h>
#include <stdlib.h>

static double g_fw_next;

/* フレーム待ち: FrameWaitHz (既定は TargetHz → 60.606) の間隔で待つ。一時停止もここで行う */
void __cdecl mkw_framewait_c(void)
{
    double hz = g_cfg.framewait_hz > 0 ? g_cfg.framewait_hz : 60.606;
    double period = 1.0 / hz, now;
    if (g_paused) {
        vwin_t *w = vwin_main();
        int ui = w && GetWindowThreadProcessId(w->hwnd, NULL) == GetCurrentThreadId();
        if (ui) pause_wait_if_paused();
        else {
            void dsound_pause(int); void title_pause(int);
            title_pause(1); dsound_pause(1);
            while (g_paused) { input_poll(); Sleep(10); }
            dsound_pause(0); title_pause(0);
        }
        g_fw_next = 0;
    }
    now = mkw_now();
    if (g_cfg.log) {
        static int cnt; static double t0;
        cnt++;
        if (now - t0 >= 5.0) { mkw_log("framewait %.3f/s\n", cnt / (now - t0)); cnt = 0; t0 = now; }
    }
    if (g_fw_next == 0 || now - g_fw_next > 0.1) g_fw_next = now;
    g_fw_next += period;
    while (mkw_now() < g_fw_next - 0.0025) Sleep(1);
    while (mkw_now() < g_fw_next) YieldProcessor();
}

__attribute__((naked)) void mkw_framewait_stub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _mkw_framewait_c\n\t"
        "popfl\n\t"
        "popal\n\t"
        "ret\n\t");
}

static int parse_hex(const char *s, BYTE *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ',') break;
        out[n++] = (BYTE)strtoul(s, (char **)&s, 16);
    }
    return n;
}

static int write_mem(BYTE *addr, const BYTE *data, int n)
{
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(addr, data, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return 1;
}

static void apply_one(const char *name, const char *val)
{
    char buf[512], *p, *type, *arg, *exp = NULL;
    BYTE *addr; BYTE data[128]; int n = 0;
    strncpy(buf, val, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    p = strstr(buf, "expect=");
    if (p) { exp = p + 7; if (p > buf && p[-1] == ',') p[-1] = 0; else *p = 0; }
    addr = (BYTE *)strtoul(buf, &p, 16);
    if (*p != ',') { mkw_log("Patch %s: 書式エラー\n", name); return; }
    type = p + 1;
    arg = strchr(type, ',');
    if (arg) *arg++ = 0;
    if (IsBadReadPtr(addr, 1)) { mkw_log("Patch %s: アドレス不正 %p\n", name, (void *)addr); return; }
    if (exp) {
        BYTE e[64]; int en = parse_hex(exp, e, 64);
        if (IsBadReadPtr(addr, en) || memcmp(addr, e, en) != 0) { mkw_log("Patch %s: 元のバイト列が一致しないため適用しません\n", name); return; }
    }
    if (!_stricmp(type, "bytes") && arg) {
        n = parse_hex(arg, data, sizeof data);
    } else if (!_stricmp(type, "framewait") && arg) {
        BYTE *resume = (BYTE *)strtoul(arg, NULL, 16);
        LONG rel1 = (LONG)((BYTE *)mkw_framewait_stub - (addr + 5));
        data[0] = 0xE8; memcpy(data + 1, &rel1, 4);
        if (resume) {
            LONG rel2 = (LONG)(resume - (addr + 10));
            data[5] = 0xE9; memcpy(data + 6, &rel2, 4);
            n = 10;
        } else n = 5;
    } else { mkw_log("Patch %s: 種類 %s は不明\n", name, type); return; }
    if (n > 0 && write_mem(addr, data, n)) mkw_log("Patch %s: %p に %d バイト適用 (%s)\n", name, (void *)addr, n, type);
}

void patches_apply(void)
{
    wchar_t sec[8192]; wchar_t *p;
    DWORD n = GetPrivateProfileSectionW(L"Patch", sec, 8192, g_ini);
    for (p = sec; p < sec + n && *p; p += wcslen(p) + 1) {
        char line[1024], *eq, *c;
        WideCharToMultiByte(CP_UTF8, 0, p, -1, line, sizeof line, NULL, NULL);
        if (line[0] == ';' || line[0] == '#') continue;
        c = strchr(line, ';'); if (c) *c = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        apply_one(line, eq + 1);
    }
}
