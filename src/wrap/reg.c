/* レジストリの仮想化
 * HKCU\Software\*, HKLM\Software\* (Microsoft 等を除く) へのアクセスを
 * ゲームフォルダの mkwrap_reg.ini に置き換える。実際のレジストリには書き込まない。
 * mkwrap.ini の [Registry] にある "キー\値名=データ" は初期値として使われる。 */
#include "mkw.h"
#include <string.h>
#include <stdlib.h>

#define FAKE_BASE 0x4D4B0000u
#define MAXH 64
static char *g_hpath[MAXH];       /* 偽ハンドル -> キーのパス */
static wchar_t g_regini[MAX_PATH];
static CRITICAL_SECTION g_cs;

static LONG (WINAPI *r_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
static LONG (WINAPI *r_RegOpenKeyA)(HKEY, LPCSTR, PHKEY);
static LONG (WINAPI *r_RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
static LONG (WINAPI *r_RegCreateKeyA)(HKEY, LPCSTR, PHKEY);
static LONG (WINAPI *r_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static LONG (WINAPI *r_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
static LONG (WINAPI *r_RegCloseKey)(HKEY);
static LONG (WINAPI *r_RegDeleteValueA)(HKEY, LPCSTR);
static LONG (WINAPI *r_RegDeleteKeyA)(HKEY, LPCSTR);
static LONG (WINAPI *r_RegFlushKey)(HKEY);

static int is_fake(HKEY h) { return ((ULONG_PTR)h & 0xFFFF0000u) == FAKE_BASE && ((ULONG_PTR)h & 0xFFFF) < MAXH && g_hpath[(ULONG_PTR)h & 0xFFFF]; }

static const char *root_name(HKEY h)
{
    if (h == HKEY_CURRENT_USER) return "HKCU";
    if (h == HKEY_LOCAL_MACHINE) return "HKLM";
    if (h == HKEY_USERS) return "HKU";
    if (h == HKEY_CLASSES_ROOT) return "HKCR";
    return NULL;
}

/* 仮想化の対象か */
static int in_scope(const char *path)
{
    const char *p = strchr(path, '\\');
    if (!p) return 0;
    if (strncmp(path, "HKCU\\", 5) && strncmp(path, "HKLM\\", 5)) return 0;
    p++;
    if (_strnicmp(p, "SOFTWARE", 8) || (p[8] && p[8] != '\\')) return 0;
    if (!p[8]) return 1;
    p += 9;
    if (!_strnicmp(p, "Microsoft", 9) || !_strnicmp(p, "Classes", 7) || !_strnicmp(p, "Policies", 8) || !_strnicmp(p, "Wow6432Node", 11)) return 0;
    return 1;
}

static void norm_path(char *p)
{
    /* 区切りの重複/末尾を除去し、比較用に大文字化はしない */
    char *s = p, *d = p;
    while (*s) {
        if (*s == '/') *s = '\\';
        if (*s == '\\' && (d == p || d[-1] == '\\')) { s++; continue; }
        *d++ = *s++;
    }
    if (d > p && d[-1] == '\\') d--;
    *d = 0;
}

static int build_path(HKEY parent, LPCSTR sub, char *out, int outlen)
{
    const char *base;
    if (is_fake(parent)) base = g_hpath[(ULONG_PTR)parent & 0xFFFF];
    else base = root_name(parent);
    if (!base) return 0;
    if (sub && sub[0]) _snprintf(out, outlen, "%s\\%s", base, sub);
    else _snprintf(out, outlen, "%s", base);
    out[outlen - 1] = 0;
    norm_path(out);
    return 1;
}

static HKEY new_handle(const char *path)
{
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 1; i < MAXH; i++) if (!g_hpath[i]) { g_hpath[i] = _strdup(path); break; }
    LeaveCriticalSection(&g_cs);
    if (i == MAXH) return NULL;
    return (HKEY)(ULONG_PTR)(FAKE_BASE + i);
}

/* ---- 保存 (ANSI 文字列として INI に書く: キー=セクション, 値名=キー) ---- */
static void to_w(const char *s, wchar_t *w, int n) { MultiByteToWideChar(CP_ACP, 0, s, -1, w, n); }

static int key_exists(const char *path)
{
    wchar_t ws[512], buf[4096];
    DWORD n;
    to_w(path, ws, 512);
    /* 自分自身のセクション、または配下のセクションがあれば存在 */
    n = GetPrivateProfileSectionNamesW(buf, 4096, g_regini);
    {
        wchar_t *p = buf; size_t l = wcslen(ws);
        while (p < buf + n && *p) {
            if (!_wcsnicmp(p, ws, l) && (p[l] == 0 || p[l] == L'\\')) return 1;
            p += wcslen(p) + 1;
        }
    }
    /* 初期値 */
    n = GetPrivateProfileSectionW(L"Registry", buf, 4096, g_ini);
    {
        wchar_t *p = buf; size_t l = wcslen(ws);
        while (p < buf + n && *p) {
            if (!_wcsnicmp(p, ws, l) && p[l] == L'\\') return 1;
            p += wcslen(p) + 1;
        }
    }
    return 0;
}

/* 値の取得: 型とバイト列 */
static int value_get(const char *path, const char *name, DWORD *type, BYTE *data, DWORD *len)
{
    wchar_t ws[512], wn[256], buf[2048];
    DWORD t; int n;
    to_w(path, ws, 512);
    to_w(name && name[0] ? name : "@", wn, 256);
    GetPrivateProfileStringW(ws, wn, L"\x01", buf, 2048, g_regini);
    if (buf[0] == 1) {
        /* 初期値 [Registry] キー\値名=データ */
        wchar_t key[800];
        _snwprintf(key, 800, L"%ls\\%ls", ws, name && name[0] ? wn : L"");
        if (!(name && name[0])) key[wcslen(key) - 1] = 0;
        GetPrivateProfileStringW(L"Registry", key, L"\x01", buf, 2048, g_ini);
        if (buf[0] == 1) return 0;
        {   /* %GAMEDIR% 展開 */
            wchar_t *p = wcsstr(buf, L"%GAMEDIR%");
            if (p) { wchar_t rest[1024]; wcscpy(rest, p + 9); *p = 0; wcscat(buf, g_gamedir); wcscat(buf, rest); }
        }
        if (!wcsncmp(buf, L"dword:", 6)) { DWORD v = (DWORD)wcstoul(buf + 6, NULL, 0); *type = REG_DWORD; memcpy(data, &v, 4); *len = 4; return 1; }
        *type = REG_SZ;
        n = WideCharToMultiByte(CP_ACP, 0, buf, -1, (char *)data, (int)*len, NULL, NULL);
        *len = n;
        return 1;
    }
    if (!wcsncmp(buf, L"dword:", 6)) { DWORD v = (DWORD)wcstoul(buf + 6, NULL, 16); *type = REG_DWORD; memcpy(data, &v, 4); *len = 4; return 1; }
    if (!wcsncmp(buf, L"hex", 3)) {
        /* hex(t):xx,xx,... */
        wchar_t *p = buf + 3; DWORD cnt = 0;
        t = REG_BINARY;
        if (*p == L'(') { t = (DWORD)wcstoul(p + 1, &p, 16); if (*p == L')') p++; }
        if (*p == L':') p++;
        while (*p && cnt < *len) { data[cnt++] = (BYTE)wcstoul(p, &p, 16); if (*p == L',') p++; }
        *type = t; *len = cnt;
        return 1;
    }
    if (buf[0] == L'"') {
        size_t l = wcslen(buf);
        if (l >= 2 && buf[l - 1] == L'"') buf[l - 1] = 0;
        n = WideCharToMultiByte(CP_ACP, 0, buf + 1, -1, (char *)data, (int)*len, NULL, NULL);
        *type = REG_SZ; *len = n;
        return 1;
    }
    n = WideCharToMultiByte(CP_ACP, 0, buf, -1, (char *)data, (int)*len, NULL, NULL);
    *type = REG_SZ; *len = n;
    return 1;
}

static void value_set(const char *path, const char *name, DWORD type, const BYTE *data, DWORD len)
{
    wchar_t ws[512], wn[256], buf[8192];
    to_w(path, ws, 512);
    to_w(name && name[0] ? name : "@", wn, 256);
    if (type == REG_DWORD && len >= 4) _snwprintf(buf, 64, L"dword:%08lx", (unsigned long)*(const DWORD *)data);
    else if ((type == REG_SZ || type == REG_EXPAND_SZ) && len < 4000) {
        char tmp[4096]; wchar_t wt[4096];
        memcpy(tmp, data, len); tmp[len] = 0;
        MultiByteToWideChar(CP_ACP, 0, tmp, -1, wt, 4096);
        _snwprintf(buf, 8192, L"\"%ls\"", wt);
    } else {
        DWORD i; int pos;
        pos = _snwprintf(buf, 32, L"hex(%lx):", (unsigned long)type);
        for (i = 0; i < len && pos < 8180; i++) pos += _snwprintf(buf + pos, 8, i ? L",%02x" : L"%02x", data[i]);
    }
    buf[8191] = 0;
    WritePrivateProfileStringW(ws, wn, buf, g_regini);
}

/* ---- フック ---- */
static LONG open_common(HKEY parent, LPCSTR sub, PHKEY out, int create, LPDWORD disp)
{
    char path[1024];
    if (!out) return ERROR_INVALID_PARAMETER;
    if (!build_path(parent, sub, path, sizeof path)) return -1;
    if (!is_fake(parent) && !in_scope(path)) return -1;
    if (!create && !key_exists(path)) { mkw_log("Reg 仮想: 開けない %s\n", path); return ERROR_FILE_NOT_FOUND; }
    if (create && !key_exists(path)) {
        wchar_t ws[512]; to_w(path, ws, 512);
        WritePrivateProfileStringW(ws, L"@@", L"1", g_regini); /* キーの存在印 */
        if (disp) *disp = REG_CREATED_NEW_KEY;
    } else if (disp) *disp = REG_OPENED_EXISTING_KEY;
    *out = new_handle(path);
    mkw_log("Reg 仮想: %s %s\n", create ? "作成" : "開く", path);
    return *out ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
}

static LONG WINAPI hk_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD o, REGSAM sam, PHKEY out)
{
    LONG r = open_common(h, sub, out, 0, NULL);
    if (r != -1) return r;
    return r_RegOpenKeyExA(h, sub, o, sam, out);
}
static LONG WINAPI hk_RegOpenKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    LONG r = open_common(h, sub, out, 0, NULL);
    if (r != -1) return r;
    return r_RegOpenKeyA(h, sub, out);
}
static LONG WINAPI hk_RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam, LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    LONG r = open_common(h, sub, out, 1, disp);
    if (r != -1) return r;
    return r_RegCreateKeyExA(h, sub, res, cls, opt, sam, sa, out, disp);
}
static LONG WINAPI hk_RegCreateKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    LONG r = open_common(h, sub, out, 1, NULL);
    if (r != -1) return r;
    return r_RegCreateKeyA(h, sub, out);
}
static LONG WINAPI hk_RegCloseKey(HKEY h)
{
    if (is_fake(h)) {
        int i = (int)((ULONG_PTR)h & 0xFFFF);
        EnterCriticalSection(&g_cs);
        free(g_hpath[i]); g_hpath[i] = NULL;
        LeaveCriticalSection(&g_cs);
        return ERROR_SUCCESS;
    }
    return r_RegCloseKey(h);
}
static LONG WINAPI hk_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    BYTE tmp[8192]; DWORD t, len = sizeof tmp;
    (void)res;
    if (!is_fake(h)) return r_RegQueryValueExA(h, name, res, type, data, cb);
    if (!value_get(g_hpath[(ULONG_PTR)h & 0xFFFF], name, &t, tmp, &len)) return ERROR_FILE_NOT_FOUND;
    if (type) *type = t;
    if (!cb) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!data) { *cb = len; return ERROR_SUCCESS; }
    if (*cb < len) { *cb = len; return ERROR_MORE_DATA; }
    memcpy(data, tmp, len);
    *cb = len;
    return ERROR_SUCCESS;
}
static LONG WINAPI hk_RegSetValueExA(HKEY h, LPCSTR name, DWORD res, DWORD type, const BYTE *data, DWORD cb)
{
    if (!is_fake(h)) return r_RegSetValueExA(h, name, res, type, data, cb);
    value_set(g_hpath[(ULONG_PTR)h & 0xFFFF], name, type, data, cb);
    return ERROR_SUCCESS;
}
static LONG WINAPI hk_RegDeleteValueA(HKEY h, LPCSTR name)
{
    if (!is_fake(h)) return r_RegDeleteValueA(h, name);
    {
        wchar_t ws[512], wn[256];
        to_w(g_hpath[(ULONG_PTR)h & 0xFFFF], ws, 512);
        to_w(name && name[0] ? name : "@", wn, 256);
        WritePrivateProfileStringW(ws, wn, NULL, g_regini);
    }
    return ERROR_SUCCESS;
}
static LONG WINAPI hk_RegDeleteKeyA(HKEY h, LPCSTR sub)
{
    char path[1024];
    if (!is_fake(h) && !(build_path(h, sub, path, sizeof path) && in_scope(path))) return r_RegDeleteKeyA(h, sub);
    if (!build_path(h, sub, path, sizeof path)) return ERROR_INVALID_PARAMETER;
    { wchar_t ws[512]; to_w(path, ws, 512); WritePrivateProfileStringW(ws, NULL, NULL, g_regini); }
    return ERROR_SUCCESS;
}
static LONG WINAPI hk_RegFlushKey(HKEY h)
{
    if (is_fake(h)) return ERROR_SUCCESS;
    return r_RegFlushKey(h);
}

void reg_hooks_install(void)
{
    HMODULE a;
    if (!g_cfg.reg_virtualize) return;
    InitializeCriticalSection(&g_cs);
    _snwprintf(g_regini, MAX_PATH, L"%ls\\mkwrap_reg.ini", g_gamedir);
    a = LoadLibraryA("advapi32.dll");
    r_RegOpenKeyExA = (void *)GetProcAddress(a, "RegOpenKeyExA");
    r_RegOpenKeyA = (void *)GetProcAddress(a, "RegOpenKeyA");
    r_RegCreateKeyExA = (void *)GetProcAddress(a, "RegCreateKeyExA");
    r_RegCreateKeyA = (void *)GetProcAddress(a, "RegCreateKeyA");
    r_RegQueryValueExA = (void *)GetProcAddress(a, "RegQueryValueExA");
    r_RegSetValueExA = (void *)GetProcAddress(a, "RegSetValueExA");
    r_RegCloseKey = (void *)GetProcAddress(a, "RegCloseKey");
    r_RegDeleteValueA = (void *)GetProcAddress(a, "RegDeleteValueA");
    r_RegDeleteKeyA = (void *)GetProcAddress(a, "RegDeleteKeyA");
    r_RegFlushKey = (void *)GetProcAddress(a, "RegFlushKey");
#define H(fn) iat_hook(g_exe, "advapi32.dll", #fn, (void *)hk_##fn)
    H(RegOpenKeyExA); H(RegOpenKeyA); H(RegCreateKeyExA); H(RegCreateKeyA); H(RegQueryValueExA);
    H(RegSetValueExA); H(RegCloseKey); H(RegDeleteValueA); H(RegDeleteKeyA); H(RegFlushKey);
#undef H
}
