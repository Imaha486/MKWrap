#ifndef MKX_COMMON_H
#define MKX_COMMON_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* ログ出力（GUI/コンソール共通） */
void mk_log(const wchar_t *fmt, ...);

/* 文字コード変換 */
void mk_utf8_to_w(const char *s, wchar_t *out, int outlen);
void mk_w_to_utf8(const wchar_t *s, char *out, int outlen);
void mk_sjis_to_w(const char *s, wchar_t *out, int outlen);

/* パス操作 */
void mk_path_join(wchar_t *out, int outlen, const wchar_t *a, const wchar_t *b);
int  mk_mkdirs(const wchar_t *dir);           /* 再帰的にフォルダ作成 */
int  mk_mkdirs_for_file(const wchar_t *file); /* ファイルの親フォルダを作成 */
int  mk_file_exists(const wchar_t *p);
int  mk_dir_exists(const wchar_t *p);
void mk_sanitize_rel(wchar_t *rel);           /* \/ 統一、..除去、禁止文字置換 */

/* 各形式 */
int is3_is_archive(const wchar_t *path);
int is3_extract(const wchar_t *zpath, const wchar_t *outdir, int *nfiles);
int is6_extract(const wchar_t *hdrpath, const wchar_t *outdir, int *nfiles, const wchar_t *srcroot);

#endif
