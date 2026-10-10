/* 仮想 CD-ROM ドライブ
 * CD を入れていないと起動しない/音楽を CD から読むタイトル向けに、
 * 空いているドライブ文字を「CD-ROM」として見せ、その中身をフォルダ (既定はゲームフォルダ) に置き換える。
 * [CD] Emulate=1 / Path= / Letter= / Label= */
#include "mkw.h"
#include <mmsystem.h>
#include <string.h>

static char g_letter;            /* 'Q' など */
static char g_cdpath[MAX_PATH];  /* 置き換え先 (末尾 \ なし, ANSI) */
static char g_label[64];

static DWORD (WINAPI *r_GetLogicalDrives)(void);
static DWORD (WINAPI *r_GetLogicalDriveStringsA)(DWORD, LPSTR);
static UINT (WINAPI *r_GetDriveTypeA)(LPCSTR);
static BOOL (WINAPI *r_GetVolumeInformationA)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD);
static HANDLE (WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *r_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static DWORD (WINAPI *r_GetFileAttributesA)(LPCSTR);
static BOOL (WINAPI *r_GetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static HMMIO (WINAPI *r_mmioOpenA)(LPSTR, LPMMIOINFO, DWORD);
static HFILE (WINAPI *r_OpenFile)(LPCSTR, LPOFSTRUCT, UINT);
static BOOL (WINAPI *r_SetCurrentDirectoryA)(LPCSTR);
static UINT (WINAPI *r_GetDiskFreeSpaceA_dummy)(void);

static int is_vdrive(LPCSTR p)
{
    return p && g_letter && (p[0] & ~0x20) == g_letter && p[1] == ':';
}
/* "Q:\xxx" -> "<path>\xxx" */
static LPCSTR map(LPCSTR p, char *buf)
{
    if (g_cfg.log >= 2 && p) mkw_log("CD? %s\n", p);
    if (!is_vdrive(p)) return p;
    _snprintf(buf, MAX_PATH * 2, "%s%s%s", g_cdpath, (p[2] == '\\' || p[2] == '/') ? "" : "\\", p + 2);
    buf[MAX_PATH * 2 - 1] = 0;
    return buf;
}

static DWORD WINAPI hk_GetLogicalDrives(void) { return r_GetLogicalDrives() | (1u << (g_letter - 'A')); }
static DWORD WINAPI hk_GetLogicalDriveStringsA(DWORD n, LPSTR buf)
{
    char tmp[512]; DWORD l = r_GetLogicalDriveStringsA(sizeof tmp - 8, tmp), need;
    if (l > sizeof tmp - 8) return r_GetLogicalDriveStringsA(n, buf);
    tmp[l] = g_letter; tmp[l + 1] = ':'; tmp[l + 2] = '\\'; tmp[l + 3] = 0; tmp[l + 4] = 0;
    need = l + 4;
    if (!buf || n < need + 1) return need + 1;
    memcpy(buf, tmp, need + 1);
    return need;
}
static UINT WINAPI hk_GetDriveTypeA(LPCSTR root)
{
    if (is_vdrive(root)) return DRIVE_CDROM;
    return r_GetDriveTypeA(root);
}
static BOOL WINAPI hk_GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD nn, LPDWORD serial, LPDWORD maxc, LPDWORD fl, LPSTR fs, DWORD fsn)
{
    if (is_vdrive(root)) {
        if (name && nn) { strncpy(name, g_label, nn - 1); name[nn - 1] = 0; }
        if (serial) *serial = 0x4D4B4344;
        if (maxc) *maxc = 255;
        if (fl) *fl = FILE_READ_ONLY_VOLUME | FILE_CASE_PRESERVED_NAMES;
        if (fs && fsn) { strncpy(fs, "CDFS", fsn - 1); fs[fsn - 1] = 0; }
        return TRUE;
    }
    return r_GetVolumeInformationA(root, name, nn, serial, maxc, fl, fs, fsn);
}
static HANDLE WINAPI hk_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    char b[MAX_PATH * 2];
    return r_CreateFileA(map(p, b), a, s, sa, c, f, t);
}
static HANDLE WINAPI hk_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA d)
{
    char b[MAX_PATH * 2];
    return r_FindFirstFileA(map(p, b), d);
}
static DWORD WINAPI hk_GetFileAttributesA(LPCSTR p)
{
    char b[MAX_PATH * 2];
    if (is_vdrive(p) && (!p[2] || ((p[2] == '\\' || p[2] == '/') && !p[3]))) return FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    return r_GetFileAttributesA(map(p, b));
}
static BOOL WINAPI hk_GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID d)
{
    char b[MAX_PATH * 2];
    return r_GetFileAttributesExA(map(p, b), l, d);
}
static HMMIO WINAPI hk_mmioOpenA(LPSTR p, LPMMIOINFO i, DWORD f)
{
    char b[MAX_PATH * 2];
    if (p && is_vdrive(p)) return r_mmioOpenA((LPSTR)map(p, b), i, f);
    return r_mmioOpenA(p, i, f);
}
static HFILE WINAPI hk_OpenFile(LPCSTR p, LPOFSTRUCT o, UINT st)
{
    char b[MAX_PATH * 2];
    return r_OpenFile(map(p, b), o, st);
}
static BOOL WINAPI hk_SetCurrentDirectoryA(LPCSTR p)
{
    char b[MAX_PATH * 2];
    return r_SetCurrentDirectoryA(map(p, b));
}

void cd_hooks_install(void)
{
    wchar_t w[MAX_PATH]; char tmp[16]; HMODULE k32, wm;
    if (!GetPrivateProfileIntW(L"CD", L"Emulate", 0, g_ini)) return;
    GetPrivateProfileStringW(L"CD", L"Path", L"", w, MAX_PATH, g_ini);
    if (!w[0] || !wcscmp(w, L".")) wcscpy(w, g_gamedir);
    else if (!(w[0] && w[1] == L':') && !(w[0] == L'\\' && w[1] == L'\\')) {
        wchar_t t2[MAX_PATH]; _snwprintf(t2, MAX_PATH, L"%ls\\%ls", g_gamedir, w); wcscpy(w, t2);
    }
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_cdpath, MAX_PATH, NULL, NULL);
    { size_t l = strlen(g_cdpath); while (l && (g_cdpath[l - 1] == '\\' || g_cdpath[l - 1] == '/')) g_cdpath[--l] = 0; }
    GetPrivateProfileStringW(L"CD", L"Label", L"", w, 64, g_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_label, sizeof g_label, NULL, NULL);
    GetPrivateProfileStringW(L"CD", L"Letter", L"", w, 8, g_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, tmp, sizeof tmp, NULL, NULL);
    k32 = GetModuleHandleA("kernel32.dll");
    r_GetLogicalDrives = (void *)GetProcAddress(k32, "GetLogicalDrives");
    if (tmp[0] && ((tmp[0] & ~0x20) >= 'A' && (tmp[0] & ~0x20) <= 'Z')) g_letter = (char)(tmp[0] & ~0x20);
    else {
        /* 空いているドライブ文字を後ろから探す */
        DWORD used = r_GetLogicalDrives(); char c;
        for (c = 'Z'; c >= 'D'; c--) if (!(used & (1u << (c - 'A')))) { g_letter = c; break; }
    }
    if (!g_letter) return;
    r_GetLogicalDriveStringsA = (void *)GetProcAddress(k32, "GetLogicalDriveStringsA");
    r_GetDriveTypeA = (void *)GetProcAddress(k32, "GetDriveTypeA");
    r_GetVolumeInformationA = (void *)GetProcAddress(k32, "GetVolumeInformationA");
    r_CreateFileA = (void *)GetProcAddress(k32, "CreateFileA");
    r_FindFirstFileA = (void *)GetProcAddress(k32, "FindFirstFileA");
    r_GetFileAttributesA = (void *)GetProcAddress(k32, "GetFileAttributesA");
    r_GetFileAttributesExA = (void *)GetProcAddress(k32, "GetFileAttributesExA");
    r_OpenFile = (void *)GetProcAddress(k32, "OpenFile");
    r_SetCurrentDirectoryA = (void *)GetProcAddress(k32, "SetCurrentDirectoryA");
    wm = LoadLibraryA("winmm.dll");
    r_mmioOpenA = (void *)GetProcAddress(wm, "mmioOpenA");
    (void)r_GetDiskFreeSpaceA_dummy;
#define H(dll, fn) iat_hook(g_exe, dll, #fn, (void *)hk_##fn)
    H("kernel32.dll", GetLogicalDrives); H("kernel32.dll", GetLogicalDriveStringsA); H("kernel32.dll", GetDriveTypeA);
    H("kernel32.dll", GetVolumeInformationA); H("kernel32.dll", CreateFileA); H("kernel32.dll", FindFirstFileA);
    H("kernel32.dll", GetFileAttributesA); H("kernel32.dll", GetFileAttributesExA); H("kernel32.dll", OpenFile);
    H("kernel32.dll", SetCurrentDirectoryA); H("winmm.dll", mmioOpenA);
#undef H
    mkw_log("仮想CDドライブ %c: -> %s (ラベル %s)\n", g_letter, g_cdpath, g_label);
}
