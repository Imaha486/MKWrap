#include "mkw.h"
#include <string.h>

/* フック表: 実行ファイルが GetProcAddress で後から関数を求めた場合 (UPX 等で圧縮された実行ファイル) にも使う */
#define MAXHOOKS 160
static struct { const char *dll; const char *func; WORD ord; void *fn; HMODULE mod; } g_hooks[MAXHOOKS];
static int g_nhooks;
static void hook_table_add(const char *dll, const char *func, WORD ord, void *fn)
{
    int i;
    for (i = 0; i < g_nhooks; i++)
        if (!_stricmp(g_hooks[i].dll, dll) && ((func && g_hooks[i].func && !strcmp(g_hooks[i].func, func)) || (!func && g_hooks[i].ord == ord))) { g_hooks[i].fn = fn; return; }
    if (g_nhooks >= MAXHOOKS) return;
    g_hooks[g_nhooks].dll = dll; g_hooks[g_nhooks].func = func; g_hooks[g_nhooks].ord = ord; g_hooks[g_nhooks].fn = fn;
    g_nhooks++;
}
static void *hook_table_find(HMODULE mod, LPCSTR name)
{
    int i;
    for (i = 0; i < g_nhooks; i++) {
        if (!g_hooks[i].mod) g_hooks[i].mod = GetModuleHandleA(g_hooks[i].dll);
        if (g_hooks[i].mod != mod) continue;
        if ((ULONG_PTR)name < 0x10000) { if (!g_hooks[i].func && g_hooks[i].ord == (WORD)(ULONG_PTR)name) return g_hooks[i].fn; }
        else if (g_hooks[i].func && !strcmp(g_hooks[i].func, name)) return g_hooks[i].fn;
    }
    return NULL;
}

/* 指定モジュールの IAT を書き換える。戻り値は元の関数 (見つからなければ NULL) */
void *iat_hook(HMODULE mod, const char *dll, const char *func, void *newfn)
{
    if (mod == g_exe) hook_table_add(dll, func, 0, newfn);
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;
    void *orig = NULL;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        const char *name = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *oft, *ft;
        if (_stricmp(name, dll) != 0) continue;
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        oft = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk) : NULL;
        if (!oft) {
            /* 名前テーブルが無い場合は実アドレスで比較 */
            HMODULE m = GetModuleHandleA(dll);
            FARPROC target = m ? GetProcAddress(m, func) : NULL;
            if (!target) continue;
            for (; ft->u1.Function; ft++) {
                if ((FARPROC)ft->u1.Function == target) {
                    DWORD old;
                    orig = (void *)ft->u1.Function;
                    VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
                    ft->u1.Function = (DWORD_PTR)newfn;
                    VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                }
            }
            continue;
        }
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) == 0) {
                DWORD old;
                orig = (void *)ft->u1.Function;
                VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
                ft->u1.Function = (DWORD_PTR)newfn;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            }
        }
    }
    return orig;
}

/* 序数インポートの IAT 書き換え */
void *iat_hook_ord(HMODULE mod, const char *dll, WORD ord, void *newfn)
{
    if (mod == g_exe) hook_table_add(dll, NULL, ord, newfn);
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt; IMAGE_IMPORT_DESCRIPTOR *imp; DWORD rva; void *orig = NULL;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (_stricmp((const char *)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        oft = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal) && IMAGE_ORDINAL(oft->u1.Ordinal) == ord) {
                DWORD old;
                orig = (void *)ft->u1.Function;
                VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
                ft->u1.Function = (DWORD_PTR)newfn;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            }
        }
    }
    return orig;
}

/* ---------------- GetProcAddress / LoadLibrary ---------------- */
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
static GetProcAddress_t real_GetProcAddress;

static int is_module_named(HMODULE m, const wchar_t *name)
{
    wchar_t path[MAX_PATH], *b;
    if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) return 0;
    b = wcsrchr(path, L'\\'); b = b ? b + 1 : path;
    return _wcsicmp(b, name) == 0;
}

void *mkw_get_proc_override(HMODULE mod, const char *name)
{
    if ((ULONG_PTR)name < 0x10000) return NULL;
    if (is_module_named(mod, L"ddraw.dll")) {
        if (!strcmp(name, "DirectDrawCreate")) return (void *)mk_DirectDrawCreate;
        if (!strcmp(name, "DirectDrawCreateEx")) return (void *)mk_DirectDrawCreateEx;
        if (!strcmp(name, "DirectDrawEnumerateA")) return (void *)mk_DirectDrawEnumerateA;
        if (!strcmp(name, "DirectDrawEnumerateExA")) return (void *)mk_DirectDrawEnumerateExA;
    }
    if (mod == g_self || is_module_named(mod, L"dinput.dll")) {
        if (!strcmp(name, "DirectInputCreateA")) return (void *)mk_DirectInputCreateA;
        if (!strcmp(name, "DirectInputCreateW")) return (void *)mk_DirectInputCreateW;
        if (!strcmp(name, "DirectInputCreateEx")) return (void *)mk_DirectInputCreateEx;
    }
    return NULL;
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE mod, LPCSTR name)
{
    void *o = mkw_get_proc_override(mod, name);
    if (g_cfg.log && (ULONG_PTR)name >= 0x10000 && strstr(name, "RequestInterrupt") && GetPrivateProfileIntW(L"Debug", L"MeasureIRQ", 0, g_ini)) {
        extern void *r_z80irq_raw_v; void debug_irq_stub(void); extern void *g_irq_stub;
        r_z80irq_raw_v = (void *)real_GetProcAddress(mod, name);
        mkw_log("GetProcAddress(%s) を計測\n", name);
        return (FARPROC)g_irq_stub;
    }
    if (o) { mkw_log("GetProcAddress(%s) -> MKWrap\n", name); return (FARPROC)o; }
    o = hook_table_find(mod, name);
    if (g_cfg.log >= 2) { char mn[MAX_PATH] = ""; GetModuleFileNameA(mod, mn, MAX_PATH); mkw_log("GetProcAddress %s!%s%s\n", strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn, (ULONG_PTR)name < 0x10000 ? "#" : name, o ? " (フック)" : ""); }
    if (o) return (FARPROC)o;
    return real_GetProcAddress(mod, name);
}

static int (WINAPI *r_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static int WINAPI hk_MessageBoxA(HWND h, LPCSTR t, LPCSTR c, UINT f)
{
    mkw_log("MessageBox: [%s] %s\n", c ? c : "", t ? t : "");
    return r_MessageBoxA(h, t, c, f);
}

void hooks_install(void)
{
    real_GetProcAddress = (GetProcAddress_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
    iat_hook(g_exe, "kernel32.dll", "GetProcAddress", (void *)hk_GetProcAddress);

    iat_hook(g_exe, "ddraw.dll", "DirectDrawCreate", (void *)mk_DirectDrawCreate);
    iat_hook(g_exe, "ddraw.dll", "DirectDrawCreateEx", (void *)mk_DirectDrawCreateEx);
    iat_hook(g_exe, "ddraw.dll", "DirectDrawEnumerateA", (void *)mk_DirectDrawEnumerateA);
    iat_hook(g_exe, "ddraw.dll", "DirectDrawEnumerateExA", (void *)mk_DirectDrawEnumerateExA);

    if (g_cfg.log) {
        r_MessageBoxA = (void *)GetProcAddress(LoadLibraryA("user32.dll"), "MessageBoxA");
        iat_hook(g_exe, "user32.dll", "MessageBoxA", (void *)hk_MessageBoxA);
    }
    if (g_cfg.virtualize_window) win_hooks_install();
    timing_hooks_install();
    input_hooks_install();
    reg_hooks_install();
    dsound_hooks_install();
    { void cd_hooks_install(void); cd_hooks_install(); }
}
