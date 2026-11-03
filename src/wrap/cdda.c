/* CD-DA (MCI の cdaudio) のエミュレーション
 * 仮想 CD ([CD] Emulate=1) のとき、ゲームが mciSendCommand / mciSendString で開く cdaudio を MKWrap が受け持つ。
 * 音楽は CD のトラックを取り込んだファイル (Track02.wav / Track02.mp3 など) があればそれを再生し、
 * 無ければ無音で「再生中」を演じて、曲の長さが過ぎたら再生終了の通知 (MM_MCINOTIFY) を送る。
 *   [CD] Tracks=19                 トラック数 (1 はデータ)
 *   [CD] TrackLengths=0:00,11:00,..  各トラックの長さ (分:秒)。ファイルがあればファイルの長さを使う
 *   [CD] AudioPath=CDDA             取り込んだ音楽ファイルのフォルダ (既定: CDDA フォルダ → ゲームのフォルダ)
 *   [CD] Volume=100 */
#include "mkw.h"
#include <mmsystem.h>
#include <string.h>
#include <stdlib.h>

#define FAKE_ID 0x6A1D
#define MAXTR 99

static int g_on, g_ntr = 20;
static DWORD g_len[MAXTR + 1];          /* ms */
static char g_file[MAXTR + 1][MAX_PATH];
static char g_alias[64];
static DWORD g_tfmt = MCI_FORMAT_MSF;
static char g_vletter;

/* 再生状態 (worker スレッドと共有) */
static CRITICAL_SECTION g_cs;
static HANDLE g_ev;
static HANDLE g_ready;                  /* 音楽ファイルの長さを調べ終えた */
static volatile int g_cmd;              /* 0 なし 1 再生 2 停止 3 一時停止 4 再開 */
static int g_play_tr;
static DWORD g_play_off;                /* トラック内の開始位置 ms */
static HWND g_notify_hwnd;
static int g_playing, g_cdpaused;
static double g_t0;                     /* 再生開始時刻 (トラック内位置 0 の時刻) */
static double g_pause_at;
static int g_cur_tr = 1;
static DWORD g_end_ms;                  /* 再生終了位置 (トラック内 ms) */

typedef DWORD (WINAPI *mciSendStringA_t)(LPCSTR, LPSTR, UINT, HWND);
static mciSendStringA_t p_mciSendStringA;

static DWORD tr_start(int t) { DWORD s = 2000; int i; for (i = 1; i < t && i <= g_ntr; i++) s += g_len[i]; return s; }  /* 先頭の 2 秒 (150 フレーム) */
static DWORD fmt_out(DWORD ms_abs, int track_rel_tr, int as_length)
{
    DWORD m, s, f;
    if (g_tfmt == MCI_FORMAT_MILLISECONDS) return ms_abs;
    m = ms_abs / 60000; s = (ms_abs / 1000) % 60; f = (ms_abs % 1000) * 75 / 1000;
    if (g_tfmt == MCI_FORMAT_TMSF && !as_length) return MCI_MAKE_TMSF(track_rel_tr, m, s, f);
    return MCI_MAKE_MSF(m, s, f);
}
/* 位置 (現在の時間形式) → トラック番号とトラック内 ms */
static void pos_in(DWORD v, int *tr, DWORD *off)
{
    DWORD ms; int t;
    if (g_tfmt == MCI_FORMAT_TMSF) {
        *tr = MCI_TMSF_TRACK(v); if (*tr < 1) *tr = 1;
        *off = MCI_TMSF_MINUTE(v) * 60000 + MCI_TMSF_SECOND(v) * 1000 + MCI_TMSF_FRAME(v) * 1000 / 75;
        return;
    }
    ms = g_tfmt == MCI_FORMAT_MILLISECONDS ? v : MCI_MSF_MINUTE(v) * 60000 + MCI_MSF_SECOND(v) * 1000 + MCI_MSF_FRAME(v) * 1000 / 75;
    for (t = 1; t < g_ntr; t++) if (ms < tr_start(t + 1)) break;
    *tr = t; *off = ms > tr_start(t) ? ms - tr_start(t) : 0;
}

static void notify(WPARAM code)
{
    if (g_notify_hwnd) PostMessageA(g_notify_hwnd, MM_MCINOTIFY, code, FAKE_ID);
    g_notify_hwnd = NULL;
}

static DWORD file_len_ms(const char *path);
/* 音楽ファイルの再生は専用スレッドで行う (MCI の装置は開いたスレッドに結び付くため) */
static DWORD WINAPI worker(LPVOID p)
{
    int opened = 0;
    char cmd[MAX_PATH + 64], ret[64];
    (void)p;
    {   /* 取り込んだ音楽ファイルの長さ */
        int t;
        for (t = 2; t <= g_ntr; t++) if (g_file[t][0]) { DWORD l = file_len_ms(g_file[t]); if (l) g_len[t] = l; else mkw_log("CD-DA: %s の長さを調べられません\n", g_file[t]); }
        SetEvent(g_ready);
    }
    for (;;) {
        int c;
        MSG m;
        WaitForSingleObject(g_ev, 100);
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        EnterCriticalSection(&g_cs);
        c = g_cmd; g_cmd = 0;
        LeaveCriticalSection(&g_cs);
        if (c == 1 || c == 2) {
            if (opened) { p_mciSendStringA("close mkwcdda", NULL, 0, NULL); opened = 0; }
        }
        if (c == 1 && g_file[g_play_tr][0] && p_mciSendStringA) {
            _snprintf(cmd, sizeof cmd, "open \"%s\" type mpegvideo alias mkwcdda", g_file[g_play_tr]); cmd[sizeof cmd - 1] = 0;
            if (p_mciSendStringA(cmd, NULL, 0, NULL) == 0) {
                int vol = GetPrivateProfileIntW(L"CD", L"Volume", 100, g_ini);
                opened = 1;
                p_mciSendStringA("set mkwcdda time format milliseconds", NULL, 0, NULL);
                _snprintf(cmd, sizeof cmd, "setaudio mkwcdda volume to %d", vol * 10); p_mciSendStringA(cmd, NULL, 0, NULL);
                _snprintf(cmd, sizeof cmd, "play mkwcdda from %lu", (unsigned long)g_play_off); p_mciSendStringA(cmd, NULL, 0, NULL);
            } else mkw_log("CD-DA: %s を開けません\n", g_file[g_play_tr]);
        }
        if (c == 3 && opened) p_mciSendStringA("pause mkwcdda", NULL, 0, NULL);
        if (c == 4 && opened) p_mciSendStringA("resume mkwcdda", NULL, 0, NULL);
        /* 再生の終了 */
        EnterCriticalSection(&g_cs);
        if (g_playing && !g_cdpaused) {
            double pos = (mkw_now() - g_t0) * 1000.0;
            int end = pos >= (double)g_end_ms;
            if (opened && !end) {
                ret[0] = 0;
                if (p_mciSendStringA("status mkwcdda mode", ret, sizeof ret, NULL) == 0 && !_stricmp(ret, "stopped") && pos > 500) end = 1;
            }
            if (end) {
                g_playing = 0;
                if (opened) { p_mciSendStringA("close mkwcdda", NULL, 0, NULL); opened = 0; }
                notify(MCI_NOTIFY_SUCCESSFUL);
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

static DWORD file_len_ms(const char *path)
{
    char cmd[MAX_PATH + 64], ret[32]; DWORD len = 0;
    if (!p_mciSendStringA) return 0;
    _snprintf(cmd, sizeof cmd, "open \"%s\" type mpegvideo alias mkwcdlen", path); cmd[sizeof cmd - 1] = 0;
    if (p_mciSendStringA(cmd, NULL, 0, NULL) != 0) return 0;
    p_mciSendStringA("set mkwcdlen time format milliseconds", NULL, 0, NULL);
    if (p_mciSendStringA("status mkwcdlen length", ret, sizeof ret, NULL) == 0) len = (DWORD)atol(ret);
    p_mciSendStringA("close mkwcdlen", NULL, 0, NULL);
    return len;
}

static void find_files(const char *dir)
{
    static const char *pat[] = { "Track%02d.%s", "Track %02d.%s", "track%02d.%s", "%02d.%s", "Track%d.%s", NULL };
    static const char *ext[] = { "wav", "mp3", "wma", NULL };
    int t, i, e;
    for (t = 2; t <= g_ntr; t++) {
        if (g_file[t][0]) continue;
        for (i = 0; pat[i] && !g_file[t][0]; i++) for (e = 0; ext[e] && !g_file[t][0]; e++) {
            char name[64], full[MAX_PATH];
            _snprintf(name, sizeof name, pat[i], t, ext[e]);
            _snprintf(full, sizeof full, "%s\\%s", dir, name); full[MAX_PATH - 1] = 0;
            if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) strcpy(g_file[t], full);
        }
    }
}

void cdda_init(char letter, const char *cdpath)
{
    wchar_t w[1024]; char a[1024], gd[MAX_PATH]; int t, nfile = 0;
    g_vletter = letter;
    InitializeCriticalSection(&g_cs);
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    p_mciSendStringA = (mciSendStringA_t)GetProcAddress(LoadLibraryA("winmm.dll"), "mciSendStringA");
    g_ntr = GetPrivateProfileIntW(L"CD", L"Tracks", 20, g_ini);
    if (g_ntr < 1) g_ntr = 1;
    if (g_ntr > MAXTR) g_ntr = MAXTR;
    for (t = 1; t <= g_ntr; t++) g_len[t] = t == 1 ? 60000 : 180000;
    GetPrivateProfileStringW(L"CD", L"TrackLengths", L"", w, 1024, g_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof a, NULL, NULL);
    if (a[0]) {
        char *s = a, *tok; t = 1;
        while ((tok = strtok(s, ", ")) && t <= g_ntr) {
            int m = 0, sec = 0; s = NULL;
            if (sscanf(tok, "%d:%d", &m, &sec) == 2) g_len[t] = (DWORD)(m * 60 + sec) * 1000;
            else g_len[t] = (DWORD)atoi(tok) * 1000;
            if (!g_len[t]) g_len[t] = 1000;
            t++;
        }
    }
    /* 取り込んだ音楽ファイル */
    WideCharToMultiByte(CP_ACP, 0, g_gamedir, -1, gd, sizeof gd, NULL, NULL);
    GetPrivateProfileStringW(L"CD", L"AudioPath", L"", w, MAX_PATH, g_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, a, MAX_PATH, NULL, NULL);
    if (a[0]) {
        char d[MAX_PATH];
        if (a[1] == ':' || (a[0] == '\\' && a[1] == '\\')) strcpy(d, a); else _snprintf(d, sizeof d, "%s\\%s", gd, a);
        find_files(d);
    }
    { char d[MAX_PATH]; _snprintf(d, sizeof d, "%s\\CDDA", gd); find_files(d); }
    find_files(cdpath);
    find_files(gd);
    for (t = 2; t <= g_ntr; t++) if (g_file[t][0]) nfile++;
    /* ファイルの長さは worker スレッドで調べる (ここは DLL の初期化中で MCI を使えない) */
    g_ready = CreateEventA(NULL, TRUE, nfile ? FALSE : TRUE, NULL);
    g_on = 1;
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    mkw_log("CD-DA: %d トラック、音楽ファイル %d 個%s\n", g_ntr, nfile, nfile ? "" : " (無音で再生したことにします)");
}

static void start_play(int tr, DWORD off, int to_tr, DWORD to_off, int has_to, HWND cb)
{
    EnterCriticalSection(&g_cs);
    if (tr < 2) tr = 2;   /* トラック 1 はデータ */
    if (tr > g_ntr) tr = g_ntr;
    g_play_tr = tr; g_play_off = off; g_cur_tr = tr;
    g_end_ms = g_len[tr];
    if (has_to && to_tr == tr && to_off > off) g_end_ms = to_off;
    g_t0 = mkw_now() - off / 1000.0;
    g_playing = 1; g_cdpaused = 0;
    g_notify_hwnd = cb;
    g_cmd = 1;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_ev);
    mkw_log("CD-DA: トラック %d を再生%s\n", tr, g_file[tr][0] ? "" : " (音楽ファイルなし)");
    (void)to_tr;
}
static void stop_play(int code)
{
    EnterCriticalSection(&g_cs);
    if (g_playing && code) notify(code);
    g_playing = 0; g_cdpaused = 0; g_cmd = 2;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_ev);
}
static DWORD cur_pos_abs(void)
{
    double ms;
    if (!g_playing) return tr_start(g_cur_tr);
    ms = ((g_cdpaused ? g_pause_at : mkw_now()) - g_t0) * 1000.0;
    if (ms < 0) ms = 0;
    return tr_start(g_cur_tr) + (DWORD)ms;
}

static int is_cd_type(LPCSTR s)
{
    return s && !IS_INTRESOURCE(s) && (!_stricmp(s, "cdaudio") || (((s[0] & ~0x20) == g_vletter) && s[1] == ':'));
}

/* mciSendCommandA: 受け持った場合は 1 を返す */
int cdda_command(MCIDEVICEID id, UINT msg, DWORD_PTR fl, DWORD_PTR parm, MCIERROR *res)
{
    if (!g_on) return 0;
    if (g_ready) WaitForSingleObject(g_ready, 5000);
    if (msg == MCI_OPEN) {
        MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)parm;
        int cd = 0;
        if (!o) return 0;
        if ((fl & MCI_OPEN_TYPE) && (fl & MCI_OPEN_TYPE_ID)) cd = LOWORD((DWORD_PTR)o->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO;
        else if (fl & MCI_OPEN_TYPE) cd = is_cd_type(o->lpstrDeviceType);
        if (!cd && (fl & MCI_OPEN_ELEMENT) && !(fl & MCI_OPEN_ELEMENT_ID)) cd = is_cd_type(o->lpstrElementName) && o->lpstrElementName[2] == 0;
        if (!cd) return 0;
        o->wDeviceID = FAKE_ID;
        if ((fl & MCI_OPEN_ALIAS) && o->lpstrAlias) lstrcpynA(g_alias, o->lpstrAlias, sizeof g_alias);
        g_tfmt = MCI_FORMAT_MSF;
        mkw_log("CD-DA: cdaudio を開きました (MKWrap が受け持ちます)\n");
        *res = 0;
        if ((fl & MCI_NOTIFY) && o->dwCallback) PostMessageA((HWND)o->dwCallback, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, FAKE_ID);
        return 1;
    }
    if (id != FAKE_ID && !(id == MCI_ALL_DEVICE_ID && (msg == MCI_CLOSE || msg == MCI_STOP))) return 0;
    *res = 0;
    switch (msg) {
    case MCI_CLOSE: stop_play(0); if (id == MCI_ALL_DEVICE_ID) return 0; break;
    case MCI_STOP: stop_play(MCI_NOTIFY_ABORTED); break;
    case MCI_PAUSE:
        EnterCriticalSection(&g_cs);
        if (g_playing && !g_cdpaused) { g_cdpaused = 1; g_pause_at = mkw_now(); g_cmd = 3; }
        LeaveCriticalSection(&g_cs); SetEvent(g_ev); break;
    case MCI_RESUME:
        EnterCriticalSection(&g_cs);
        if (g_playing && g_cdpaused) { g_t0 += mkw_now() - g_pause_at; g_cdpaused = 0; g_cmd = 4; }
        LeaveCriticalSection(&g_cs); SetEvent(g_ev); break;
    case MCI_SET: {
        MCI_SET_PARMS *s = (MCI_SET_PARMS *)parm;
        if ((fl & MCI_SET_TIME_FORMAT) && s) g_tfmt = s->dwTimeFormat;
        break; }
    case MCI_SEEK: {
        MCI_SEEK_PARMS *s = (MCI_SEEK_PARMS *)parm;
        int tr = 2; DWORD off = 0;
        stop_play(0);
        if ((fl & MCI_TO) && s) pos_in(s->dwTo, &tr, &off);
        else if (fl & MCI_SEEK_TO_END) tr = g_ntr;
        g_cur_tr = tr < 1 ? 1 : tr;
        break; }
    case MCI_PLAY: {
        MCI_PLAY_PARMS *p = (MCI_PLAY_PARMS *)parm;
        int tr = g_cur_tr, tt = 0; DWORD off = 0, toff = 0;
        if (g_playing) stop_play(MCI_NOTIFY_SUPERSEDED);
        if ((fl & MCI_FROM) && p) pos_in(p->dwFrom, &tr, &off);
        if ((fl & MCI_TO) && p) pos_in(p->dwTo, &tt, &toff);
        start_play(tr, off, tt, toff, (fl & MCI_TO) != 0, (fl & MCI_NOTIFY) && p ? (HWND)p->dwCallback : NULL);
        return 1; }
    case MCI_STATUS: {
        MCI_STATUS_PARMS *s = (MCI_STATUS_PARMS *)parm;
        int tr;
        if (!s || !(fl & MCI_STATUS_ITEM)) { *res = MCIERR_MISSING_PARAMETER; break; }
        tr = (fl & MCI_TRACK) ? (int)s->dwTrack : g_cur_tr;
        if (tr < 1 || tr > g_ntr) { if (fl & MCI_TRACK) { *res = MCIERR_OUTOFRANGE; break; } tr = 1; }
        switch (s->dwItem) {
        case MCI_STATUS_LENGTH:
            if (fl & MCI_TRACK) s->dwReturn = fmt_out(g_len[tr], tr, 1);
            else s->dwReturn = fmt_out(tr_start(g_ntr + 1), 1, 1);
            if (g_tfmt == MCI_FORMAT_TMSF) s->dwReturn = fmt_out((fl & MCI_TRACK) ? g_len[tr] : tr_start(g_ntr + 1), tr, 1);
            break;
        case MCI_STATUS_POSITION:
            if (fl & MCI_STATUS_START) s->dwReturn = g_tfmt == MCI_FORMAT_TMSF ? MCI_MAKE_TMSF(1, 0, 0, 0) : fmt_out(tr_start(1), 1, 0);
            else if (fl & MCI_TRACK) s->dwReturn = g_tfmt == MCI_FORMAT_TMSF ? MCI_MAKE_TMSF(tr, 0, 0, 0) : fmt_out(tr_start(tr), tr, 0);
            else {
                DWORD a = cur_pos_abs();
                if (g_tfmt == MCI_FORMAT_TMSF) { DWORD r = a - tr_start(g_cur_tr); s->dwReturn = MCI_MAKE_TMSF(g_cur_tr, r / 60000, (r / 1000) % 60, (r % 1000) * 75 / 1000); }
                else s->dwReturn = fmt_out(a, g_cur_tr, 0);
            }
            break;
        case MCI_STATUS_NUMBER_OF_TRACKS: s->dwReturn = g_ntr; break;
        case MCI_STATUS_MODE: s->dwReturn = g_playing ? (g_cdpaused ? MCI_MODE_PAUSE : MCI_MODE_PLAY) : MCI_MODE_STOP; break;
        case MCI_STATUS_MEDIA_PRESENT: case MCI_STATUS_READY: s->dwReturn = TRUE; break;
        case MCI_STATUS_TIME_FORMAT: s->dwReturn = g_tfmt; break;
        case MCI_STATUS_CURRENT_TRACK: s->dwReturn = g_cur_tr; break;
        case MCI_CDA_STATUS_TYPE_TRACK: s->dwReturn = tr == 1 ? MCI_CDA_TRACK_OTHER : MCI_CDA_TRACK_AUDIO; break;
        default: s->dwReturn = 0; break;
        }
        break; }
    case MCI_GETDEVCAPS: {
        MCI_GETDEVCAPS_PARMS *g = (MCI_GETDEVCAPS_PARMS *)parm;
        if (g) g->dwReturn = g->dwItem == MCI_GETDEVCAPS_DEVICE_TYPE ? MCI_DEVTYPE_CD_AUDIO : TRUE;
        break; }
    case MCI_INFO: {
        MCI_INFO_PARMSA *i = (MCI_INFO_PARMSA *)parm;
        if (i && i->lpstrReturn && i->dwRetSize) lstrcpynA(i->lpstrReturn, "MKWrap CD Audio", i->dwRetSize);
        break; }
    default: break;
    }
    if ((fl & MCI_NOTIFY) && parm && msg != MCI_PLAY && msg != MCI_CLOSE) {
        DWORD_PTR cb = *(DWORD_PTR *)parm;
        if (cb) PostMessageA((HWND)cb, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, FAKE_ID);
    }
    return 1;
}

int cdda_device_id(LPCSTR name, MCIDEVICEID *out)
{
    if (!g_on || !name) return 0;
    if ((g_alias[0] && !_stricmp(name, g_alias)) || !_stricmp(name, "cdaudio")) { *out = FAKE_ID; return 1; }
    return 0;
}
int cdda_enabled(void) { return g_on; }
