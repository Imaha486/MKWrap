/* InstallShield 3.x の ".Z" アーカイブ展開 (シグネチャ 13 5D 65 8C)
 * 中身は PKWARE DCL implode 圧縮 -> zlib contrib/blast で伸長する */
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include "blast.h"

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

typedef struct { const unsigned char *p; unsigned left; } bin_t;
typedef struct { HANDLE h; unsigned long n; } bout_t;

static unsigned b_in(void *how, unsigned char **buf)
{
    bin_t *b = (bin_t *)how; unsigned n = b->left;
    *buf = (unsigned char *)b->p; b->p += n; b->left = 0; return n;
}
static int b_out(void *how, unsigned char *buf, unsigned len)
{
    bout_t *o = (bout_t *)how; DWORD w = 0;
    o->n += len;
    return !WriteFile(o->h, buf, len, &w, NULL) || w != len;
}

static unsigned char *read_all(const wchar_t *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *d; DWORD sz, rd = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    d = (unsigned char *)malloc(sz ? sz : 1);
    if (!d || !ReadFile(h, d, sz, &rd, NULL) || rd != sz) { CloseHandle(h); free(d); return NULL; }
    CloseHandle(h); *size = sz; return d;
}

int is3_is_archive(const wchar_t *path)
{
    unsigned char h[4]; DWORD rd = 0; int ok = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (ReadFile(f, h, 4, &rd, NULL) && rd == 4 && rd32(h) == 0x8C655D13u) ok = 1;
    CloseHandle(f); return ok;
}

/* DOS日付時刻を FILETIME に */
static void set_dos_time(HANDLE h, unsigned dt)
{
    FILETIME lft, ft;
    if (DosDateTimeToFileTime((WORD)(dt & 0xFFFF), (WORD)(dt >> 16), &lft) && LocalFileTimeToFileTime(&lft, &ft))
        SetFileTime(h, NULL, NULL, &ft);
}

int is3_extract(const wchar_t *zpath, const wchar_t *outdir, int *nfiles_out)
{
    DWORD sz = 0; unsigned char *d = read_all(zpath, &sz);
    unsigned nfiles, ndirs, toc, i, pos; wchar_t (*dirs)[MAX_PATH]; int errors = 0, done = 0;
    if (!d) { mk_log(L"  ファイルを開けません: %ls\r\n", zpath); return -1; }
    if (sz < 0x100 || rd32(d) != 0x8C655D13u) { free(d); return -1; }
    nfiles = rd16(d + 0x0C); toc = rd32(d + 0x29); ndirs = rd16(d + 0x31);
    if (toc >= sz) { free(d); mk_log(L"  目次が壊れています\r\n"); return -1; }
    dirs = calloc(ndirs ? ndirs : 1, sizeof(*dirs));
    pos = toc;
    for (i = 0; i < ndirs && pos + 6 <= sz; i++) {
        unsigned chunk = rd16(d + pos + 2), nl = rd16(d + pos + 4);
        char tmp[300];
        if (nl > 255) nl = 255;
        memcpy(tmp, d + pos + 6, nl); tmp[nl] = 0;
        mk_sjis_to_w(tmp, dirs[i], MAX_PATH);
        mk_sanitize_rel(dirs[i]);
        pos += chunk ? chunk : 6 + nl;
    }
    for (i = 0; i < nfiles && pos + 30 < sz; i++) {
        const unsigned char *e = d + pos;
        unsigned dir = rd16(e + 1), usize = rd32(e + 3), csize = rd32(e + 7), off = rd32(e + 11);
        unsigned dt = rd32(e + 15), chunk = rd16(e + 23), nl = e[29];
        char nameA[300]; wchar_t name[MAX_PATH], rel[MAX_PATH * 2], full[MAX_PATH * 3];
        bin_t bi; bout_t bo; int r;
        if (!chunk) break;
        memcpy(nameA, e + 30, nl); nameA[nl] = 0;
        mk_sjis_to_w(nameA, name, MAX_PATH);
        if (dir < ndirs && dirs[dir][0]) _snwprintf(rel, MAX_PATH * 2, L"%ls\\%ls", dirs[dir], name);
        else _snwprintf(rel, MAX_PATH * 2, L"%ls", name);
        mk_sanitize_rel(rel);
        mk_path_join(full, MAX_PATH * 3, outdir, rel);
        mk_mkdirs_for_file(full);
        pos += chunk;
        if ((unsigned long long)off + csize > sz) { mk_log(L"  破損: %ls\r\n", rel); errors++; continue; }
        bo.h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); bo.n = 0;
        if (bo.h == INVALID_HANDLE_VALUE) { mk_log(L"  書き込み失敗: %ls\r\n", full); errors++; continue; }
        bi.p = d + off; bi.left = csize;
        if (csize == 0 && usize == 0) r = 0;
        else r = blast(b_in, &bi, b_out, &bo, NULL, NULL);
        set_dos_time(bo.h, dt);
        CloseHandle(bo.h);
        if (r != 0 || bo.n != usize) { mk_log(L"  展開エラー(%d): %ls (%lu/%u)\r\n", r, rel, bo.n, usize); errors++; }
        else { mk_log(L"  %ls (%u bytes)\r\n", rel, usize); done++; }
    }
    free(dirs); free(d);
    if (nfiles_out) *nfiles_out += done;
    return errors ? -2 : 0;
}
