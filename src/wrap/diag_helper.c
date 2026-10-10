/* 記録用ヘルパー: rundll32.exe "dinput.dll",MkwDump <pid> <tid> <例外情報のアドレス> <例外履歴のアドレス> <crash|hang>
 *
 * ゲームのプロセスの外から、全スレッドの実行位置と呼び出し履歴をテキストで書き出し、
 * ミニダンプ (MiniDumpWriteDump) を作る。 */
#include "mkw.h"
#include <tlhelp32.h>
#include <dbghelp.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct { DWORD base, size; char name[64]; } modent_t;
static modent_t g_mods[512];
static int g_nmods;
static HANDLE g_proc, g_out;

static void out(const char *fmt, ...)
{
    char buf[1024]; va_list a; int n; DWORD w;
    va_start(a, fmt); n = _vsnprintf(buf, sizeof buf - 1, fmt, a); va_end(a);
    if (n < 0) n = sizeof buf - 1;
    buf[n] = 0;
    WriteFile(g_out, buf, n, &w, NULL);
}

static const modent_t *mod_of(DWORD a)
{
    int i;
    for (i = 0; i < g_nmods; i++) if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    return NULL;
}
static const char *aname(DWORD a)
{
    static char b[4][96]; static int k;
    const modent_t *m = mod_of(a);
    char *s = b[k++ & 3];
    if (m) _snprintf(s, 96, "%08lx %s+%lx", (unsigned long)a, m->name, (unsigned long)(a - m->base));
    else _snprintf(s, 96, "%08lx", (unsigned long)a);
    s[95] = 0;
    return s;
}
static int rd(DWORD a, void *buf, DWORD n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (void *)(ULONG_PTR)a, buf, n, &got) && got == n;
}
/* 直前が call 命令か (戻り番地らしさ) */
static int after_call(DWORD a)
{
    BYTE p[8];
    if (!rd(a - 7, p, 7)) return 0;
    /* p[7-k] が a-k */
    if (p[2] == 0xE8) return 1;                           /* call rel32 */
    if (p[1] == 0xFF && (p[2] & 0x38) == 0x10) return 1;  /* call [mem] */
    if (p[5] == 0xFF && (p[6] & 0x38) == 0x10) return 1;  /* call reg / [reg] */
    if (p[4] == 0xFF && (p[5] & 0x38) == 0x10) return 1;  /* call [reg+d8] */
    if (p[0] == 0xFF && (p[1] & 0x38) == 0x10) return 1;  /* call [reg+d32] */
    if (p[3] == 0xFF && (p[4] & 0x38) == 0x10) return 1;  /* call [reg+sib+d8] */
    return 0;
}
static int is_exec(DWORD a)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(g_proc, (void *)(ULONG_PTR)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void dump_stack(DWORD esp, DWORD ebp)
{
    DWORD buf[1024]; int i, n = 0, cnt;
    /* EBP の連鎖 */
    {
        DWORD f = ebp, fr[2]; int k;
        out("  EBP連鎖:");
        for (k = 0; k < 24 && f && f >= esp && rd(f, fr, 8); k++) {
            out(" %s", aname(fr[1]));
            if (fr[0] <= f) break;
            f = fr[0];
        }
        out("\n");
    }
    /* スタック上の戻り番地らしい値 */
    for (cnt = 0; cnt < 4; cnt++) {
        if (!rd(esp + cnt * 4096, buf, 4096)) break;
        for (i = 0; i < 1024 && n < 64; i++) {
            DWORD v = buf[i];
            if (v < 0x10000 || !mod_of(v) || !is_exec(v) || !after_call(v)) continue;
            out("  [esp+%04x] %s\n", cnt * 4096 + i * 4, aname(v));
            n++;
        }
    }
}

static void dump_threads(DWORD pid, DWORD ftid, CONTEXT *fctx)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        HANDLE h; CONTEXT c;
        if (te.th32OwnerProcessID != pid) continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) { out("\n--- スレッド %lu (開けません)\n", (unsigned long)te.th32ThreadID); continue; }
        SuspendThread(h);
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (te.th32ThreadID == ftid && fctx) c = *fctx;   /* 例外を起こしたスレッドは例外時点の状態 */
        else if (!GetThreadContext(h, &c)) { out("\n--- スレッド %lu (状態を読めません)\n", (unsigned long)te.th32ThreadID); ResumeThread(h); CloseHandle(h); continue; }
        out("\n--- スレッド %lu%s 優先度 %ld\n", (unsigned long)te.th32ThreadID, te.th32ThreadID == ftid ? " ★" : "", (long)te.tpBasePri);
        out("  EIP=%s\n  ESP=%08lx EBP=%08lx EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx\n",
            aname(c.Eip), c.Esp, c.Ebp, c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi);
        dump_stack(c.Esp, c.Ebp);
        ResumeThread(h);
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static const char *code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "アクセス違反";
    case EXCEPTION_STACK_OVERFLOW: return "スタック溢れ";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "不正な命令";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "0 での除算";
    case EXCEPTION_PRIV_INSTRUCTION: return "特権命令";
    case EXCEPTION_BREAKPOINT: return "ブレークポイント";
    case 0xE06D7363: return "C++ 例外";
    case 0xC0000374: return "ヒープ破損";
    case 0xC0000409: return "セキュリティチェック (バッファ溢れ)";
    }
    return "";
}

typedef BOOL (WINAPI *MDWD_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

void CALLBACK MkwDump(HWND hw, HINSTANCE hi, LPSTR cmd, int show)
{
    DWORD pid, tid, epa, ringa; char kind[16] = "crash", *p = cmd;
    wchar_t dir[MAX_PATH], path[MAX_PATH], *s;
    EXCEPTION_POINTERS ep; EXCEPTION_RECORD er; CONTEXT ctx; int have_ex = 0;
    (void)hw; (void)hi; (void)show;
    pid = strtoul(p, &p, 10); tid = strtoul(p, &p, 10); epa = strtoul(p, &p, 10); ringa = strtoul(p, &p, 10);
    while (*p == ' ') p++;
    if (*p) { strncpy(kind, p, sizeof kind - 1); kind[sizeof kind - 1] = 0; }
    if (strcmp(kind, "hang") && strcmp(kind, "crash")) strcpy(kind, "crash");

    GetModuleFileNameW(g_self, dir, MAX_PATH);
    s = wcsrchr(dir, L'\\'); if (s) *s = 0;
    g_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!g_proc) return;
    _snwprintf(path, MAX_PATH, L"%ls\\mkwrap_%hs.log", dir, kind);
    g_out = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_out == INVALID_HANDLE_VALUE) { CloseHandle(g_proc); return; }

    /* モジュール一覧 */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32 me;
        if (snap != INVALID_HANDLE_VALUE) {
            me.dwSize = sizeof me;
            if (Module32First(snap, &me)) do {
                if (g_nmods < 512) {
                    g_mods[g_nmods].base = (DWORD)(ULONG_PTR)me.modBaseAddr; g_mods[g_nmods].size = me.modBaseSize;
                    strncpy(g_mods[g_nmods].name, me.szModule, 63); g_mods[g_nmods].name[63] = 0;
                    g_nmods++;
                }
            } while (Module32Next(snap, &me));
            CloseHandle(snap);
        }
    }

    if (epa && rd(epa, &ep, sizeof ep) && rd((DWORD)(ULONG_PTR)ep.ExceptionRecord, &er, sizeof er) &&
        rd((DWORD)(ULONG_PTR)ep.ContextRecord, &ctx, sizeof ctx)) {
        DWORD i;
        have_ex = 1;
        out("例外 %08lx %s  場所 %s  スレッド %lu\n", (unsigned long)er.ExceptionCode, code_name(er.ExceptionCode),
            aname((DWORD)(ULONG_PTR)er.ExceptionAddress), (unsigned long)tid);
        if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2)
            out("  %s アドレス %08lx\n", er.ExceptionInformation[0] == 0 ? "読み込み" : er.ExceptionInformation[0] == 8 ? "実行" : "書き込み",
                (unsigned long)er.ExceptionInformation[1]);
        else for (i = 0; i < er.NumberParameters && i < 4; i++) out("  引数%lu %08lx\n", (unsigned long)i, (unsigned long)er.ExceptionInformation[i]);
        {   /* 例外位置の命令バイト */
            BYTE b[16]; int k;
            if (rd((DWORD)(ULONG_PTR)er.ExceptionAddress, b, 16)) { out("  命令:"); for (k = 0; k < 16; k++) out(" %02X", b[k]); out("\n"); }
        }
    } else if (!strcmp(kind, "hang")) {
        out("応答なし (窓のスレッド %lu)\n", (unsigned long)tid);
    }

    /* 直前の例外の履歴 (処理済みのものも含む) */
    if (ringa) {
        struct { char magic[8]; LONG pos; struct { DWORD code, addr, tid, tick; } ent[32]; } ring;
        if (rd(ringa, &ring, sizeof ring) && !memcmp(ring.magic, "MKWDIAG", 7) && ring.pos > 0) {
            LONG i, from = ring.pos > 32 ? ring.pos - 32 : 0;
            out("直前の例外 (全 %ld 件のうち最後の %ld 件):\n", (long)ring.pos, (long)(ring.pos - from));
            for (i = from; i < ring.pos; i++)
                out("  tick=%lu スレッド %lu 例外 %08lx 場所 %s\n", (unsigned long)ring.ent[i % 32].tick, (unsigned long)ring.ent[i % 32].tid,
                    (unsigned long)ring.ent[i % 32].code, aname(ring.ent[i % 32].addr));
        }
    }

    /* 全スレッド */
    dump_threads(pid, tid, have_ex ? &ctx : NULL);

    out("\nモジュール:\n");
    {
        int i;
        for (i = 0; i < g_nmods; i++) out("  %08lx %08lx %s\n", (unsigned long)g_mods[i].base, (unsigned long)g_mods[i].size, g_mods[i].name);
    }

    /* ミニダンプ */
    {
        HMODULE dh = LoadLibraryA("dbghelp.dll");
        MDWD_t mdwd = dh ? (MDWD_t)GetProcAddress(dh, "MiniDumpWriteDump") : NULL;
        if (mdwd) {
            HANDLE f;
            _snwprintf(path, MAX_PATH, L"%ls\\mkwrap_%hs.dmp", dir, kind);
            f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                BOOL ok;
                mei.ThreadId = tid; mei.ExceptionPointers = (PEXCEPTION_POINTERS)(ULONG_PTR)epa; mei.ClientPointers = TRUE;
                ok = mdwd(g_proc, pid, f, (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                          MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo),
                          have_ex ? &mei : NULL, NULL, NULL);
                CloseHandle(f);
                out("\nミニダンプ: mkwrap_%s.dmp %s\n", kind, ok ? "" : "(作成失敗)");
            }
        }
    }
    out("\n");
    CloseHandle(g_out);
    CloseHandle(g_proc);
}
