/* MKSetup - メディアカイト系 CD から「展開だけ」を行い、汎用ラッパーと INI を配置する
 *  工程1: CD (またはフォルダ) のインストーラ情報を読み、指定フォルダにファイル一式を展開する
 *         (インストーラは実行しない / レジストリ・スタートメニューは触らない)
 *  工程2: 展開先 (インストーラが無い場合は指定フォルダ自身) に dinput.dll と mkwrap.ini を置く
 *  コマンドライン: MKSetup.exe <展開元> <展開先> [/nowrap]
 *                  MKSetup.exe /wrap <ゲームフォルダ>      (工程2のみ) */
#include "common.h"
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "profiles.h"

#define IDR_WRAPDLL 101
#define APP_TITLE L"MKSetup - メディアカイト CD 展開ツール"

static HWND g_hwnd, g_log, g_src, g_dst, g_run, g_chk;
static HANDLE g_conout;
static FILE *g_logfile;
static volatile LONG g_busy;

/* ---------------- 共通ヘルパ ---------------- */
void mk_log(const wchar_t *fmt, ...)
{
    wchar_t buf[2048]; va_list a;
    va_start(a, fmt); _vsnwprintf(buf, 2047, fmt, a); va_end(a);
    buf[2047] = 0;
    if (g_logfile) { fputws(buf, g_logfile); fflush(g_logfile); }
    if (g_conout) {
        char u[4096]; DWORD w; int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, sizeof u, NULL, NULL);
        WriteFile(g_conout, u, n > 0 ? n - 1 : 0, &w, NULL);
    }
    if (g_log) {
        wchar_t *copy = _wcsdup(buf);
        PostMessageW(g_hwnd, WM_APP + 1, 0, (LPARAM)copy);
    }
}
void mk_utf8_to_w(const char *s, wchar_t *out, int outlen) { if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outlen)) out[0] = 0; }
void mk_w_to_utf8(const wchar_t *s, char *out, int outlen) { if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outlen, NULL, NULL)) out[0] = 0; }
void mk_sjis_to_w(const char *s, wchar_t *out, int outlen) { if (!MultiByteToWideChar(932, 0, s, -1, out, outlen)) MultiByteToWideChar(CP_ACP, 0, s, -1, out, outlen); }

void mk_path_join(wchar_t *out, int outlen, const wchar_t *a, const wchar_t *b)
{
    size_t l = wcslen(a);
    if (l && (a[l - 1] == L'\\' || a[l - 1] == L'/')) _snwprintf(out, outlen, L"%ls%ls", a, b);
    else _snwprintf(out, outlen, L"%ls\\%ls", a, b);
    out[outlen - 1] = 0;
}
int mk_file_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
int mk_dir_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
int mk_mkdirs(const wchar_t *dir)
{
    wchar_t tmp[MAX_PATH * 3], *p;
    wcsncpy(tmp, dir, MAX_PATH * 3 - 1); tmp[MAX_PATH * 3 - 1] = 0;
    for (p = tmp; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p > tmp && p[-1] != L':' && p != tmp + 1) {
            wchar_t c = *p; *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
    return mk_dir_exists(tmp);
}
int mk_mkdirs_for_file(const wchar_t *file)
{
    wchar_t tmp[MAX_PATH * 3], *p;
    wcsncpy(tmp, file, MAX_PATH * 3 - 1); tmp[MAX_PATH * 3 - 1] = 0;
    p = wcsrchr(tmp, L'\\');
    if (!p) return 1;
    *p = 0;
    return mk_mkdirs(tmp);
}
void mk_sanitize_rel(wchar_t *rel)
{
    wchar_t *s = rel, *d = rel;
    for (; *s; s++) {
        wchar_t c = *s;
        if (c == L'/') c = L'\\';
        if (c == L'\\' && (d == rel || d[-1] == L'\\')) continue;
        if (c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32) c = L'_';
        *d++ = c;
    }
    *d = 0;
    /* ".." 要素を除去 */
    while ((s = wcsstr(rel, L"..\\")) != NULL) memmove(s, s + 3, (wcslen(s + 3) + 1) * sizeof(wchar_t));
    if (!wcscmp(rel, L"..")) rel[0] = 0;
}

static int ends_with_i(const wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && !_wcsicmp(s + a - b, suf);
}

/* ---------------- インストーラ検出 ---------------- */
typedef struct {
    int type;              /* 0=なし 3=IS3 6=IS5/6 9=MSI */
    wchar_t dir[MAX_PATH * 2];
    wchar_t file[MAX_PATH * 2];
} found_t;

static int find_installer_in(const wchar_t *dir, found_t *f)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2];
    int has_ins = 0;
    mk_path_join(pat, MAX_PATH * 2, dir, L"setup.ins");
    has_ins = mk_file_exists(pat);
    /* IS5/6 */
    mk_path_join(pat, MAX_PATH * 2, dir, L"data1.hdr");
    if (mk_file_exists(pat)) { f->type = 6; wcscpy(f->dir, dir); wcscpy(f->file, pat); return 1; }
    /* IS5 初期 (data1.cab にヘッダを含む) */
    mk_path_join(pat, MAX_PATH * 2, dir, L"data1.cab");
    if (mk_file_exists(pat)) {
        HANDLE fh = CreateFileW(pat, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char sig[4] = { 0 }; DWORD rd = 0;
        if (fh != INVALID_HANDLE_VALUE) { ReadFile(fh, sig, 4, &rd, NULL); CloseHandle(fh); }
        if (rd == 4 && !memcmp(sig, "ISc(", 4)) { f->type = 6; wcscpy(f->dir, dir); wcscpy(f->file, pat); return 1; }
    }
    /* IS3: SETUP.INS と同じ場所にある .Z */
    if (has_ins) {
        mk_path_join(pat, MAX_PATH * 2, dir, L"*.z");
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t full[MAX_PATH * 2];
                if (fd.cFileName[0] == L'_' || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                mk_path_join(full, MAX_PATH * 2, dir, fd.cFileName);
                if (is3_is_archive(full)) { FindClose(h); f->type = 3; wcscpy(f->dir, dir); wcscpy(f->file, full); return 1; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    /* MSI */
    mk_path_join(pat, MAX_PATH * 2, dir, L"*.msi");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        f->type = 9; wcscpy(f->dir, dir); mk_path_join(f->file, MAX_PATH * 2, dir, fd.cFileName);
        return 1;
    }
    return 0;
}

static int find_installer(const wchar_t *src, found_t *f, int depth)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2];
    if (find_installer_in(src, f)) return 1;
    if (depth <= 0) return 0;
    mk_path_join(pat, MAX_PATH * 2, src, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t sub[MAX_PATH * 2];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (!_wcsicmp(fd.cFileName, L"directx") || !_wcsnicmp(fd.cFileName, L"directx", 7)) continue;
        mk_path_join(sub, MAX_PATH * 2, src, fd.cFileName);
        if (find_installer(sub, f, depth - 1)) { FindClose(h); return 1; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

/* ---------------- 展開処理 ---------------- */
static int extract_is3(found_t *f, const wchar_t *dst)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2]; int total = 0, err = 0;
    mk_path_join(pat, MAX_PATH * 2, f->dir, L"*.z");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        wchar_t full[MAX_PATH * 2];
        if (fd.cFileName[0] == L'_') continue; /* _SETUP.LIB などはインストーラ自身 */
        mk_path_join(full, MAX_PATH * 2, f->dir, fd.cFileName);
        if (!is3_is_archive(full)) continue;
        mk_log(L"[InstallShield 3 アーカイブ] %ls\r\n", fd.cFileName);
        if (is3_extract(full, dst, &total) != 0) err++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    mk_log(L"%d 個のファイルを展開しました。\r\n", total);
    return err ? -1 : total;
}

static const wchar_t *g_srcroot;
static int extract_is6(found_t *f, const wchar_t *dst)
{
    int total = 0, r;
    mk_log(L"[InstallShield 5/6 キャビネット] %ls\r\n", f->file);
    r = is6_extract(f->file, dst, &total, g_srcroot);
    mk_log(L"%d 個のファイルを展開しました。\r\n", total);
    return r ? -1 : total;
}

static const wchar_t *g_msi_skip[] = { L"setup.exe", L"setup.ini", L"instmsia.exe", L"instmsiw.exe", L"autorun.inf", L"autorun.exe", L"setup.bmp", NULL };
static int msi_skip(const wchar_t *n)
{
    int i;
    if (ends_with_i(n, L".msi")) return 1;
    for (i = 0; g_msi_skip[i]; i++) if (!_wcsicmp(n, g_msi_skip[i])) return 1;
    return 0;
}
static int copy_tree(const wchar_t *src, const wchar_t *dst, int top, int *count)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2]; int err = 0;
    mk_mkdirs(dst);
    mk_path_join(pat, MAX_PATH * 2, src, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        wchar_t s2[MAX_PATH * 2], d2[MAX_PATH * 2];
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (top && msi_skip(fd.cFileName)) continue;
        mk_path_join(s2, MAX_PATH * 2, src, fd.cFileName);
        mk_path_join(d2, MAX_PATH * 2, dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (copy_tree(s2, d2, 0, count)) err++; }
        else {
            if (CopyFileW(s2, d2, FALSE)) {
                SetFileAttributesW(d2, GetFileAttributesW(d2) & ~FILE_ATTRIBUTE_READONLY); /* CD からのコピーは読み取り専用になるため */
                (*count)++;
                mk_log(L"  %ls\r\n", fd.cFileName);
            } else { mk_log(L"  コピー失敗: %ls\r\n", s2); err++; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return err ? -1 : 0;
}

static int msi_has_loose_files(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2]; int n = 0;
    mk_path_join(pat, MAX_PATH * 2, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (msi_skip(fd.cFileName) || ends_with_i(fd.cFileName, L".cab")) continue;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n > 0;
}

static int extract_msi(found_t *f, const wchar_t *dst)
{
    int count = 0;
    if (msi_has_loose_files(f->dir)) {
        mk_log(L"[Windows Installer (非圧縮)] %ls のファイルをコピーします\r\n", f->dir);
        copy_tree(f->dir, dst, 1, &count);
        mk_log(L"%d 個のファイルをコピーしました。\r\n", count);
        return count;
    } else {
        /* 圧縮された MSI は「管理者用インストール」(展開のみ・登録なし) を使う */
        wchar_t cmd[MAX_PATH * 6]; STARTUPINFOW si; PROCESS_INFORMATION pi; DWORD ec = 1;
        _snwprintf(cmd, MAX_PATH * 6, L"msiexec.exe /a \"%ls\" TARGETDIR=\"%ls\" /qb", f->file, dst);
        mk_log(L"[Windows Installer] 管理者用インストールで展開します:\r\n  %ls\r\n", cmd);
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { mk_log(L"msiexec を起動できません\r\n"); return -1; }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        mk_log(L"msiexec 終了コード %lu\r\n", ec);
        return ec == 0 ? 1 : -1;
    }
}

/* ---------------- 工程2: ラッパー配置 ---------------- */
/* PE のインポートに指定 DLL があるか */
static int pe_imports(const wchar_t *path, const char *dllname)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE m; BYTE *b; int found = 0; DWORD size;
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    m = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) { CloseHandle(h); return 0; }
    b = (BYTE *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (b && size > 0x200) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) < size) {
            IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(b + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
                DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
                IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
                int i, ns = nt->FileHeader.NumberOfSections;
                #define RVA2OFS(r) ({ DWORD _o = 0; int _k; for (_k = 0; _k < ns; _k++) if ((r) >= sec[_k].VirtualAddress && (r) < sec[_k].VirtualAddress + (sec[_k].Misc.VirtualSize ? sec[_k].Misc.VirtualSize : sec[_k].SizeOfRawData)) { _o = (r) - sec[_k].VirtualAddress + sec[_k].PointerToRawData; break; } _o; })
                if (rva) {
                    DWORD ofs = RVA2OFS(rva);
                    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(b + ofs);
                    for (i = 0; ofs && (BYTE *)(imp + 1) <= b + size && imp->Name; imp++, i++) {
                        DWORD no = RVA2OFS(imp->Name);
                        if (no && no < size && !_stricmp((const char *)(b + no), dllname)) { found = 1; break; }
                    }
                }
            }
        }
    }
    if (b) UnmapViewOfFile(b);
    CloseHandle(m); CloseHandle(h);
    return found;
}

static int find_game_exe(const wchar_t *dir, wchar_t *out, int depth)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2]; DWORD best = 0;
    out[0] = 0;
    mk_path_join(pat, MAX_PATH * 2, dir, L"*.exe");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[MAX_PATH * 2];
            if (!_wcsicmp(fd.cFileName, L"uninst.exe") || !_wcsicmp(fd.cFileName, L"setup.exe") || !_wcsicmp(fd.cFileName, L"mksetup.exe")) continue;
            mk_path_join(full, MAX_PATH * 2, dir, fd.cFileName);
            if ((pe_imports(full, "dinput.dll") || pe_imports(full, "ddraw.dll")) && fd.nFileSizeLow > best) { best = fd.nFileSizeLow; wcscpy(out, full); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (out[0] || depth <= 0) return out[0] != 0;
    /* 1 階層下も探す */
    mk_path_join(pat, MAX_PATH * 2, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t sub[MAX_PATH * 2];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        mk_path_join(sub, MAX_PATH * 2, dir, fd.cFileName);
        if (find_game_exe(sub, out, 0)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out[0] != 0;
}

static const mk_profile_t *profile_for(const wchar_t *exe)
{
    const wchar_t *b = wcsrchr(exe, L'\\'); size_t i;
    b = b ? b + 1 : exe;
    for (i = 0; i < sizeof g_profiles / sizeof g_profiles[0]; i++) if (!_wcsicmp(b, g_profiles[i].exe)) return &g_profiles[i];
    return &g_generic_profile;
}

static void write_utf16(HANDLE h, const wchar_t *s) { DWORD w; WriteFile(h, s, (DWORD)(wcslen(s) * sizeof(wchar_t)), &w, NULL); }

static int write_ini(const wchar_t *path, const mk_profile_t *p, const wchar_t *exename)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    wchar_t buf[512];
    DWORD w;
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, "\xFF\xFE", 2, &w, NULL); /* UTF-16LE BOM (Windows の INI API がそのまま読める) */
    _snwprintf(buf, 512, L"; MKWrap 設定ファイル — %ls (%ls)\r\n", p->title, exename);
    write_utf16(h, buf);
    write_utf16(h,
        L"; 同じフォルダの dinput.dll (または ddraw.dll) が MKWrap 本体です。表示・入力・速度を補正します。\r\n"
        L"; 使わない場合はその DLL を削除するだけで元に戻ります。\r\n");
    if (p->note) write_utf16(h, p->note);
    write_utf16(h,
        L"\r\n[Display]\r\n"
        L"; 窓表示の倍率 (小数可)。0 = 画面に収まる最大の整数倍\r\n"
        L"Scale=0\r\n"
        L"; 1 = 起動時から擬似フルスクリーン (Alt+Enter でいつでも切り替え)\r\n"
        L"Fullscreen=0\r\n"
        L"; 拡大方法 0 = くっきり (ニアレストネイバー) / 1 = なめらか\r\n"
        L"Filter=0\r\n"
        L"; 1 = 縦横比を保つ (左右または上下に黒帯)\r\n"
        L"KeepAspect=1\r\n"
        L"; 1 = フルスクリーン時に整数倍だけで拡大する\r\n"
        L"IntegerScale=0\r\n"
        L"; 表示の横縦比を指定 (例 1.3333 = 4:3)。0 = ゲームの画素比のまま\r\n"
        L"Aspect=0\r\n"
        L"; フルスクリーン時にメニューバーを隠す\r\n"
        L"HideMenuInFullscreen=1\r\n"
        L"; フルスクリーン切り替えキー (Alt + このキー)\r\n"
        L"FullscreenKey=ENTER\r\n"
        L"; ゲームによる解像度変更を実際には行わず擬似フルスクリーンにする\r\n"
        L"IgnoreModeChange=1\r\n"
        L"; ゲームが全画面に切り替えたとき 1 = 擬似フルスクリーン / 0 = 窓のまま表示\r\n");
    write_utf16(h, p->display ? p->display : L"ModeChangeFullscreen=1\r\n");
    write_utf16(h,
        L"; 枠のない窓を窓表示するときに、タイトルバーと枠を付ける\r\n"
        L"FramePopupWindow=1\r\n"
        L"; 1 = 窓の大きさをマウスで変えられるようにする (最大化も可。KeepAspect=1 なら縦横比を保つ)\r\n"
        L"Resizable=1\r\n"
        L"; 1 = 高DPI対応として動かす (ぼやけ防止)\r\n"
        L"DpiAware=1\r\n"
        L"; DirectDraw に知らせる画面の色数 (32 または 16)\r\n"
        L"DesktopBpp=32\r\n"
        L"\r\n[Timing]\r\n"
        L"; GameHz = ゲームが本来動いている周期, TargetHz = 合わせたい周期 (アーケードの多くは 60.606)\r\n"
        L"; RefreshRate = 画面転送の上限 (0 = 制限しない)\r\n");
    write_utf16(h, p->timing);
    write_utf16(h,
        L"RefreshRate=0\r\n"
        L"; 1 = GetTickCount/timeGetTime を 1ms 精度にする (Windows 標準は約 16ms 刻み)\r\n"
        L"PreciseTimer=1\r\n"
        L"\r\n[Input]\r\n"
        L"; XInput コントローラ (Xbox 系) のボタンをキーボードのキーに割り当てます\r\n"
        L"; キー名: A～Z 0～9 F1～F12 UP DOWN LEFT RIGHT SPACE ENTER ESC TAB LSHIFT RSHIFT LCTRL RCTRL\r\n"
        L";         LALT INSERT DELETE HOME END PGUP PGDN NUM0～NUM9 など。「X+Z」で同時押し\r\n"
        L"; ボタン名: A B X Y LB RB LT RT Back Start LThumb RThumb Guide\r\n"
        L";           DPadUp/Down/Left/Right, LStickUp/Down/Left/Right, RStickUp/Down/Left/Right\r\n"
        L"Enable=1\r\n"
        L"; 使うコントローラ番号 (0～3)\r\n"
        L"Pad=0\r\n"
        L"; アナログスティックの遊び (%)。小さいほど敏感\r\n"
        L"Deadzone=35\r\n"
        L"; スティックの方向数 (8 または 4)\r\n"
        L"StickDirections=8\r\n"
        L"; 1 = ゲームから DirectInput のジョイスティックを隠す (二重入力の防止)\r\n"
        L"HideJoysticks=1\r\n"
        L"; キー入力メッセージも送る: 0 = 送らない / 1 = すべて / 2 = F1～F12 のみ (メニューのショートカット用)\r\n"
        L"PostKeyMessages=2\r\n"
        L"; 1 = 窓が非アクティブでもパッド入力を渡す\r\n"
        L"BackgroundInput=0\r\n"
        L"; 一時停止: キーボードのキーと、パッドのボタン (例 Back+Start, Guide)\r\n"
        L"PauseKey=PAUSE\r\n"
        L"PauseButton=\r\n");
    write_utf16(h, p->input);
    write_utf16(h,
        L"\r\n[Registry]\r\n"
        L"; ゲームが読み書きするレジストリを mkwrap_reg.ini に置き換える (実際のレジストリは使わない)\r\n"
        L"Virtualize=1\r\n"
        L"; ゲームが参照する値の初期値 (%GAMEDIR% はこのフォルダ)\r\n");
    write_utf16(h, p->registry);
    write_utf16(h,
        L"\r\n[Patch]\r\n"
        L"; タイトル別の実行ファイル修正。expect= のバイト列が一致しないときは適用されません\r\n");
    write_utf16(h, p->patch);
    if (p->extra && p->extra[0]) write_utf16(h, p->extra);
    write_utf16(h,
        L"\r\n[Debug]\r\n"
        L"; 1 = mkwrap.log に動作記録を出す\r\n"
        L"Log=0\r\n");
    CloseHandle(h);
    return 1;
}

static int deploy_wrapper(const wchar_t *dir)
{
    wchar_t exe[MAX_PATH * 2], gdir[MAX_PATH * 2], dll[MAX_PATH * 2], ini[MAX_PATH * 2], *p;
    HRSRC r; HGLOBAL g; DWORD sz, w; HANDLE h; const mk_profile_t *prof;
    mk_log(L"\r\n=== 工程2: 汎用ラッパーの配置 ===\r\n");
    if (!find_game_exe(dir, exe, 1)) {
        mk_log(L"DirectInput / DirectDraw を使う実行ファイルが見つかりません。ラッパーは配置しません。\r\n");
        return -1;
    }
    wcscpy(gdir, exe);
    p = wcsrchr(gdir, L'\\'); if (p) *p = 0;
    prof = profile_for(exe);
    mk_log(L"ゲーム本体: %ls\r\nタイトル: %ls\r\n", exe, prof->title);
    r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_WRAPDLL), (LPCWSTR)RT_RCDATA);
    if (!r || !(g = LoadResource(NULL, r))) { mk_log(L"内蔵の dinput.dll を読み出せません\r\n"); return -1; }
    sz = SizeofResource(NULL, r);
    /* DirectInput を使うなら dinput.dll、使わず DirectDraw だけなら ddraw.dll として置く */
    {
        const wchar_t *dn = pe_imports(exe, "dinput.dll") ? L"dinput.dll" : L"ddraw.dll";
        mk_path_join(dll, MAX_PATH * 2, gdir, dn);
    }
    h = CreateFileW(dll, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, LockResource(g), sz, &w, NULL) || w != sz) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        mk_log(L"dinput.dll を書き込めません: %ls\r\n", dll); return -1;
    }
    CloseHandle(h);
    mk_log(L"配置: %ls\r\n", dll);
    mk_path_join(ini, MAX_PATH * 2, gdir, L"mkwrap.ini");
    if (mk_file_exists(ini)) {
        wchar_t ini2[MAX_PATH * 2];
        mk_path_join(ini2, MAX_PATH * 2, gdir, L"mkwrap.ini.new");
        DeleteFileW(ini2);
        write_ini(ini2, prof, wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe);
        mk_log(L"mkwrap.ini は既にあるため残しました (新しい既定値は mkwrap.ini.new)\r\n");
    } else if (write_ini(ini, prof, wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe)) mk_log(L"配置: %ls\r\n", ini);
    else mk_log(L"mkwrap.ini を書き込めません\r\n");
    return 0;
}

/* ---------------- 全体の流れ ---------------- */
static int run_job(const wchar_t *src, const wchar_t *dst, int wrap)
{
    found_t f; int r = 0;
    memset(&f, 0, sizeof f);
    mk_log(L"=== 工程1: 展開 ===\r\n展開元: %ls\r\n", src);
    if (!mk_dir_exists(src)) { mk_log(L"展開元が見つかりません。\r\n"); return -1; }
    g_srcroot = src;
    if (!find_installer(src, &f, 2)) {
        mk_log(L"インストーラが見つからないため、展開済みのゲームフォルダとして扱います。\r\n");
        return wrap ? deploy_wrapper(src) : 0;
    }
    if (!dst || !dst[0]) { mk_log(L"展開先を指定してください。\r\n"); return -1; }
    mk_log(L"展開先: %ls\r\n", dst);
    if (!mk_mkdirs(dst)) { mk_log(L"展開先フォルダを作成できません。\r\n"); return -1; }
    switch (f.type) {
    case 3: r = extract_is3(&f, dst); break;
    case 6: r = extract_is6(&f, dst); break;
    case 9: r = extract_msi(&f, dst); break;
    }
    if (r < 0) mk_log(L"※ 展開中にエラーがありました。\r\n");
    if (r == 0) { mk_log(L"展開されたファイルがありません。\r\n"); return -1; }
    mk_log(L"レジストリやスタートメニューは変更していません。\r\n");
    if (wrap) deploy_wrapper(dst);
    return r < 0 ? -1 : 0;
}

/* ---------------- GUI ---------------- */
enum { ID_SRC = 100, ID_SRCBTN, ID_DST, ID_DSTBTN, ID_CHK, ID_RUN, ID_LOG, ID_CLOSE };

static void browse(HWND edit, const wchar_t *title)
{
    BROWSEINFOW bi; LPITEMIDLIST il; wchar_t path[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd; bi.lpszTitle = title; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    il = SHBrowseForFolderW(&bi);
    if (il && SHGetPathFromIDListW(il, path)) SetWindowTextW(edit, path);
    if (il) CoTaskMemFree(il);
}

static void fill_drives(HWND combo)
{
    DWORD m = GetLogicalDrives(); int i;
    for (i = 0; i < 26; i++) {
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 }, label[MAX_PATH] = L"", item[MAX_PATH + 16];
        if (!(m & (1u << i)) || GetDriveTypeW(root) != DRIVE_CDROM) continue;
        GetVolumeInformationW(root, label, MAX_PATH, NULL, NULL, NULL, NULL, 0);
        _snwprintf(item, MAX_PATH + 16, L"%ls", root);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
        (void)label;
    }
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

typedef struct { wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2]; int wrap; } job_t;
static DWORD WINAPI job_thread(LPVOID p)
{
    job_t *j = (job_t *)p;
    int r = run_job(j->src, j->dst, j->wrap);
    mk_log(r == 0 ? L"\r\n完了しました。\r\n" : L"\r\n終了しました (エラーあり)。\r\n");
    PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
    free(j);
    return 0;
}

/* 展開先の既定値: ユーザーフォルダ\Games\MediaKite\<CD のボリューム名またはフォルダ名> */
static int g_dst_touched;
static void update_default_dst(void)
{
    wchar_t src[MAX_PATH * 2], base[MAX_PATH], name[MAX_PATH] = L"", out[MAX_PATH * 2];
    size_t l;
    if (g_dst_touched) return;
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, base) != S_OK) return;
    wcscat(base, L"\\Games\\MediaKite");
    GetWindowTextW(g_src, src, MAX_PATH * 2);
    l = wcslen(src);
    if (l >= 2 && l <= 3 && src[1] == L':') {
        wchar_t root[4] = { src[0], L':', L'\\', 0 };
        GetVolumeInformationW(root, name, MAX_PATH, NULL, NULL, NULL, NULL, 0);
    } else if (l) {
        wchar_t *p;
        while (l && (src[l - 1] == L'\\' || src[l - 1] == L'/')) src[--l] = 0;
        p = wcsrchr(src, L'\\');
        wcsncpy(name, p ? p + 1 : src, MAX_PATH - 1);
    }
    mk_sanitize_rel(name);
    { wchar_t *q; for (q = name; *q; q++) if (*q == L'\\') *q = L'_'; }
    if (name[0]) _snwprintf(out, MAX_PATH * 2, L"%ls\\%ls", base, name);
    else wcscpy(out, base);
    SetWindowTextW(g_dst, out);
    g_dst_touched = 0;
}

static HFONT g_font;
static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"COMBOBOX") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
        mk(L"STATIC", L"展開元 (CD ドライブ、またはフォルダ):", 0, 12, 10, 600, 20, 0);
        g_src = mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 12, 32, 520, 200, ID_SRC);
        mk(L"BUTTON", L"参照...", BS_PUSHBUTTON | WS_TABSTOP, 540, 31, 80, 26, ID_SRCBTN);
        mk(L"STATIC", L"展開先フォルダ:", 0, 12, 66, 600, 20, 0);
        g_dst = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 88, 520, 24, ID_DST);
        mk(L"BUTTON", L"参照...", BS_PUSHBUTTON | WS_TABSTOP, 540, 87, 80, 26, ID_DSTBTN);
        g_chk = mk(L"BUTTON", L"汎用ラッパー (dinput.dll / ddraw.dll) と設定ファイル (mkwrap.ini) を配置する", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 120, 600, 22, ID_CHK);
        SendMessageW(g_chk, BM_SETCHECK, BST_CHECKED, 0);
        g_run = mk(L"BUTTON", L"実行", BS_DEFPUSHBUTTON | WS_TABSTOP, 430, 148, 90, 30, ID_RUN);
        mk(L"BUTTON", L"閉じる", BS_PUSHBUTTON | WS_TABSTOP, 530, 148, 90, 30, ID_CLOSE);
        g_log = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, 12, 188, 608, 300, ID_LOG);
        SendMessageW(g_log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        fill_drives(g_src);
        update_default_dst();
        return 0;
    case WM_SIZE: {
        int W = LOWORD(lp), H = HIWORD(lp);
        MoveWindow(g_log, 12, 188, W - 24, H - 200, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SRC: if (HIWORD(wp) == CBN_SELCHANGE) { PostMessageW(h, WM_APP + 3, 0, 0); } break;
        case ID_SRCBTN: browse(g_src, L"展開元 (CD ドライブまたはフォルダ) を選んでください"); update_default_dst(); break;
        case ID_DSTBTN: browse(g_dst, L"展開先フォルダを選んでください"); g_dst_touched = 1; break;
        case ID_DST: if (HIWORD(wp) == EN_CHANGE && GetFocus() == g_dst) g_dst_touched = 1; break;
        case ID_CLOSE: if (!g_busy) DestroyWindow(h); break;
        case ID_RUN:
            if (!g_busy) {
                job_t *j = (job_t *)calloc(1, sizeof *j);
                GetWindowTextW(g_src, j->src, MAX_PATH * 2);
                GetWindowTextW(g_dst, j->dst, MAX_PATH * 2);
                j->wrap = SendMessageW(g_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!j->src[0]) { MessageBoxW(h, L"展開元を指定してください。", APP_TITLE, MB_ICONWARNING); free(j); break; }
                SetWindowTextW(g_log, L"");
                g_busy = 1; EnableWindow(g_run, FALSE);
                CloseHandle(CreateThread(NULL, 0, job_thread, j, 0, NULL));
            }
            break;
        }
        return 0;
    case WM_APP + 1: {
        wchar_t *s = (wchar_t *)lp;
        int n = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, n, n);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
        free(s);
        return 0;
    }
    case WM_APP + 3:
        update_default_dst();
        return 0;
    case WM_APP + 2:
        g_busy = 0; EnableWindow(g_run, TRUE);
        return 0;
    case WM_CLOSE:
        if (g_busy) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WNDCLASSW wc; MSG msg; HWND h;
    (void)prev; (void)cmdline;
    if (argc >= 3) {
        /* コマンドライン実行 */
        int r, wrap = 1, i;
        wchar_t lp[MAX_PATH * 2];
        if (AttachConsole(ATTACH_PARENT_PROCESS)) g_conout = GetStdHandle(STD_OUTPUT_HANDLE);
        for (i = 3; i < argc; i++) if (!_wcsicmp(argv[i], L"/nowrap")) wrap = 0;
        if (!_wcsicmp(argv[1], L"/wrap")) return deploy_wrapper(argv[2]) == 0 ? 0 : 1;
        mk_mkdirs(argv[2]);
        mk_path_join(lp, MAX_PATH * 2, argv[2], L"mksetup.log");
        g_logfile = _wfopen(lp, L"w, ccs=UTF-8");
        r = run_job(argv[1], argv[2], wrap);
        mk_log(r == 0 ? L"完了しました。\r\n" : L"終了しました (エラーあり)。\r\n");
        if (g_logfile) fclose(g_logfile);
        return r == 0 ? 0 : 1;
    }
    CoInitialize(NULL);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"MKSetupWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    RegisterClassW(&wc);
    h = CreateWindowW(L"MKSetupWnd", APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 650, 560, NULL, NULL, inst, NULL);
    ShowWindow(h, show);
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
