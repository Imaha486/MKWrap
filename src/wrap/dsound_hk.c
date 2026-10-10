/* DirectSound: 一時停止中に音を止めるため、作成されたバッファを追跡する */
#include "mkw.h"
#include <string.h>

typedef HRESULT (WINAPI *DSCreate_t)(const GUID *, void **, void *);
static DSCreate_t r_DirectSoundCreate;
typedef HRESULT (WINAPI *CSB_t)(void *, const void *, void **, void *);
typedef ULONG (WINAPI *Rel_t)(void *);
typedef HRESULT (WINAPI *GetStatus_t)(void *, DWORD *);
typedef HRESULT (WINAPI *Play_t)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *Stop_t)(void *);

#define MAXBUF 256
static void *g_bufs[MAXBUF];
static int g_buf_state[MAXBUF]; /* 一時停止で止めた: 1=単発 2=ループ */
static CRITICAL_SECTION g_cs;
static CSB_t r_CreateSoundBuffer;
static Rel_t r_BufRelease;

/* vtable の書き換え (同じ vtable を共有する全オブジェクトに効く) */
void patch_vtbl(void *obj, int idx, void *fn, void **orig)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (vt[idx] == fn) return;
    if (orig && !*orig) *orig = vt[idx];
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}

static ULONG WINAPI hk_BufRelease(void *This)
{
    ULONG r = r_BufRelease(This);
    if (r == 0) {
        int i;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < MAXBUF; i++) if (g_bufs[i] == This) { g_bufs[i] = NULL; g_buf_state[i] = 0; }
        LeaveCriticalSection(&g_cs);
    }
    return r;
}
static HRESULT WINAPI hk_CreateSoundBuffer(void *This, const void *desc, void **out, void *outer)
{
    HRESULT hr = r_CreateSoundBuffer(This, desc, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        int i;
        patch_vtbl(*out, 2, (void *)hk_BufRelease, (void **)&r_BufRelease);
        EnterCriticalSection(&g_cs);
        for (i = 0; i < MAXBUF; i++) if (!g_bufs[i]) { g_bufs[i] = *out; g_buf_state[i] = 0; break; }
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}
static HRESULT WINAPI hk_DirectSoundCreate(const GUID *g, void **out, void *outer)
{
    HRESULT hr = r_DirectSoundCreate(g, out, outer);
    if (SUCCEEDED(hr) && out && *out) patch_vtbl(*out, 3, (void *)hk_CreateSoundBuffer, (void **)&r_CreateSoundBuffer);
    mkw_log("DirectSoundCreate = %08lx\n", hr);
    return hr;
}

void dsound_pause(int pause)
{
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAXBUF; i++) {
        void *b = g_bufs[i]; void **vt;
        if (!b) continue;
        vt = *(void ***)b;
        if (pause) {
            DWORD st = 0;
            if (SUCCEEDED(((GetStatus_t)vt[9])(b, &st)) && (st & 1)) {
                g_buf_state[i] = (st & 4) ? 2 : 1;
                ((Stop_t)vt[18])(b);
            }
        } else if (g_buf_state[i]) {
            ((Play_t)vt[12])(b, 0, 0, g_buf_state[i] == 2 ? 1 : 0);
            g_buf_state[i] = 0;
        }
    }
    LeaveCriticalSection(&g_cs);
}

void dsound_hooks_install(void)
{
    HMODULE ds;
    InitializeCriticalSection(&g_cs);
    ds = LoadLibraryA("dsound.dll");
    if (!ds) return;
    r_DirectSoundCreate = (DSCreate_t)GetProcAddress(ds, "DirectSoundCreate");
    if (!r_DirectSoundCreate) return;
    iat_hook(g_exe, "dsound.dll", "DirectSoundCreate", (void *)hk_DirectSoundCreate);
    iat_hook_ord(g_exe, "dsound.dll", 1, (void *)hk_DirectSoundCreate);
}
