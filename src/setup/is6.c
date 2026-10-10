/* InstallShield 5.x/6.x 形式 (data1.hdr / data1.cab) の展開 — unshield (MIT) を利用 */
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "libunshield.h"

/* ---- unshield 用 I/O コールバック (UTF-8 パス -> ワイド文字 API) ---- */
static void *cb_fopen(const char *fn, const char *modes, void *ud)
{
    wchar_t wfn[MAX_PATH * 2], wm[16];
    (void)ud;
    mk_utf8_to_w(fn, wfn, MAX_PATH * 2);
    mk_utf8_to_w(modes, wm, 16);
    return _wfopen(wfn, wm);
}
static int cb_fseek(void *f, long off, int wh, void *ud) { (void)ud; return fseek((FILE *)f, off, wh); }
static long cb_ftell(void *f, void *ud) { (void)ud; return ftell((FILE *)f); }
static size_t cb_fread(void *p, size_t s, size_t n, void *f, void *ud) { (void)ud; return fread(p, s, n, (FILE *)f); }
static size_t cb_fwrite(const void *p, size_t s, size_t n, void *f, void *ud) { (void)ud; return fwrite(p, s, n, (FILE *)f); }
static int cb_fclose(void *f, void *ud) { (void)ud; return fclose((FILE *)f); }

typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int first; struct dirent de; } mydir_t;
static void *cb_opendir(const char *name, void *ud)
{
    wchar_t w[MAX_PATH * 2]; mydir_t *d; size_t l;
    (void)ud;
    mk_utf8_to_w(name, w, MAX_PATH * 2 - 4);
    l = wcslen(w);
    if (l && w[l - 1] != L'\\' && w[l - 1] != L'/') wcscat(w, L"\\");
    wcscat(w, L"*");
    d = (mydir_t *)calloc(1, sizeof(*d));
    d->h = FindFirstFileW(w, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}
static struct dirent *cb_readdir(void *dir, void *ud)
{
    mydir_t *d = (mydir_t *)dir; (void)ud;
    if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
    d->first = 0;
    memset(&d->de, 0, sizeof(d->de));
    mk_w_to_utf8(d->fd.cFileName, d->de.d_name, sizeof(d->de.d_name));
    d->de.d_namlen = (unsigned short)strlen(d->de.d_name);
    return &d->de;
}
static int cb_closedir(void *dir, void *ud)
{
    mydir_t *d = (mydir_t *)dir; (void)ud;
    FindClose(d->h); free(d); return 0;
}
static const UnshieldIoCallbacks g_cb = { cb_fopen, cb_fseek, cb_ftell, cb_fread, cb_fwrite, cb_fclose, cb_opendir, cb_closedir, cb_readdir };

/* InstallShield 内部の文字列は ANSI(日本語版なら Shift_JIS) */
static void cab_str_to_w(Unshield *u, const char *s, wchar_t *out, int outlen)
{
    if (unshield_is_unicode(u)) mk_utf8_to_w(s, out, outlen);
    else mk_sjis_to_w(s, out, outlen);
}

/* キャビネットに中身が無いファイル (CD 上に非圧縮で置かれている) を探す */
static int find_loose(const wchar_t *dir, const wchar_t *name, unsigned long long size, wchar_t *out, int depth)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH * 2]; int ok = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[MAX_PATH * 2];
            if (depth <= 0 || !_wcsnicmp(fd.cFileName, L"directx", 7)) continue;
            _snwprintf(sub, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            if (find_loose(sub, name, size, out, depth - 1)) { ok = 1; break; }
        } else if (!_wcsicmp(fd.cFileName, name) && ((unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) == size) {
            _snwprintf(out, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            ok = 1; break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

int is6_extract(const wchar_t *hdrpath, const wchar_t *outdir, int *nfiles_out, const wchar_t *srcroot)
{
    char path8[MAX_PATH * 3]; Unshield *u; int g, ng, errors = 0, done = 0;
    unsigned char *seen; int nf;
    mk_w_to_utf8(hdrpath, path8, sizeof(path8));
    unshield_set_log_level(UNSHIELD_LOG_LEVEL_LOWEST);
    u = unshield_open2(path8, &g_cb, NULL);
    if (!u) { mk_log(L"  キャビネットを開けません: %ls\r\n", hdrpath); return -1; }
    nf = unshield_file_count(u);
    seen = (unsigned char *)calloc(nf > 0 ? nf : 1, 1);
    ng = unshield_file_group_count(u);
    for (g = 0; g < ng; g++) {
        UnshieldFileGroup *fg = unshield_file_group_get(u, g);
        int i; wchar_t gname[MAX_PATH];
        if (!fg || !fg->name) continue;
        cab_str_to_w(u, fg->name, gname, MAX_PATH);
        if (fg->name[0] == '<') { /* <Engine> <Support> などはインストーラ自身の部品 */
            continue;
        }
        mk_log(L" [ファイルグループ] %ls\r\n", gname);
        for (i = fg->first_file; i <= fg->last_file && i < nf; i++) {
            const char *fn; int di; wchar_t wfn[MAX_PATH], wdir[MAX_PATH], rel[MAX_PATH * 2], full[MAX_PATH * 3];
            char full8[MAX_PATH * 4];
            if (i < 0 || seen[i] || !unshield_file_is_valid(u, i)) continue; /* 無効な項目は名前も壊れている */
            seen[i] = 1;
            fn = unshield_file_name(u, i);
            if (!fn) continue;
            cab_str_to_w(u, fn, wfn, MAX_PATH);
            di = unshield_file_directory(u, i);
            wdir[0] = 0;
            if (di >= 0) { const char *dn = unshield_directory_name(u, di); if (dn) cab_str_to_w(u, dn, wdir, MAX_PATH); }
            if (wdir[0]) _snwprintf(rel, MAX_PATH * 2, L"%ls\\%ls", wdir, wfn);
            else _snwprintf(rel, MAX_PATH * 2, L"%ls", wfn);
            mk_sanitize_rel(rel);
            mk_path_join(full, MAX_PATH * 3, outdir, rel);
            mk_mkdirs_for_file(full);
            mk_w_to_utf8(full, full8, sizeof(full8));
            if (unshield_file_save(u, i, full8)) { mk_log(L"  %ls (%u bytes)\r\n", rel, (unsigned)unshield_file_size(u, i)); done++; }
            else {
                /* キャビネット外 (CD 上の非圧縮ファイル) を探してコピー */
                wchar_t loose[MAX_PATH * 2];
                if (srcroot && find_loose(srcroot, wfn, unshield_file_size(u, i), loose, 4) && CopyFileW(loose, full, FALSE)) {
                    SetFileAttributesW(full, GetFileAttributesW(full) & ~FILE_ATTRIBUTE_READONLY);
                    mk_log(L"  %ls (CD からコピー)\r\n", rel); done++;
                } else { mk_log(L"  展開エラー: %ls\r\n", rel); errors++; }
            }
        }
    }
    free(seen);
    unshield_close(u);
    if (nfiles_out) *nfiles_out += done;
    return errors ? -2 : 0;
}
