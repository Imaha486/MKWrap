/* DPLAY.dll の代わり (MKSetup が必要なときだけゲームのフォルダに置く)
 * Windows 10/11 は DirectPlay が「レガシ コンポーネント」として既定で無効になっており、
 * DPLAY.dll を読み込むゲーム (通信対戦用の部品) はそれだけで起動できない。
 * ここでは本物の DPLAY.dll がシステムにあればそれに処理を渡し、無ければ「DirectPlay は使えない」と返す
 * (通信対戦以外は普通に遊べる)。 */
#include <windows.h>

typedef HRESULT (WINAPI *create_t)(void *, void **, void *);
typedef HRESULT (WINAPI *enum_t)(void *, void *);
#define DPERR_UNAVAILABLE ((HRESULT)0x887703E8L)

static HMODULE real(void)
{
    static HMODULE m; static int tried;
    if (!tried) {
        char p[MAX_PATH];
        tried = 1;
        if (GetSystemDirectoryA(p, MAX_PATH - 16)) {
            lstrcatA(p, "\\dplay.dll");
            if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) m = LoadLibraryA(p);
        }
    }
    return m;
}

HRESULT WINAPI DirectPlayCreate(void *guid, void **dp, void *outer)
{
    HMODULE m = real();
    create_t f = m ? (create_t)GetProcAddress(m, "DirectPlayCreate") : NULL;
    if (f) return f(guid, dp, outer);
    if (dp) *dp = NULL;
    return DPERR_UNAVAILABLE;
}
HRESULT WINAPI DirectPlayEnumerate(void *cb, void *ctx)
{
    HMODULE m = real();
    enum_t f = m ? (enum_t)GetProcAddress(m, "DirectPlayEnumerate") : NULL;
    if (f) return f(cb, ctx);
    return 0;   /* DP_OK (通信方式は 1 つも無い) */
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
