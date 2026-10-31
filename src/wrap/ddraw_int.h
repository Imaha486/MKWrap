/* DirectDraw エミュレーションの内部構造 (ddraw_emu.c と d3d6.c で共有) */
#ifndef DDRAW_INT_H
#define DDRAW_INT_H
#include "mkw.h"
#include <ddraw.h>

typedef struct Surf Surf;
typedef struct DDraw DDraw;
typedef struct Pal Pal;
typedef struct Clip Clip;

typedef struct { void **vtbl; void *self; int ver; } IFACE;
#define SELF(T, p) ((T *)((IFACE *)(p))->self)

struct DDraw {
    IFACE i[4];          /* 1,2,4,7 */
    LONG ref;
    void *d3d;           /* Direct3D (d3d6.c) */
    HWND hwnd;
    DWORD coop;
    int mode_w, mode_h, mode_bpp;
    Surf *primary;
};

struct Pal {
    IFACE i;
    LONG ref;
    DWORD caps;
    PALETTEENTRY e[256];
    DDraw *dd;
};

struct Clip {
    IFACE i;
    LONG ref;
    HWND hwnd;
};

struct Surf {
    IFACE i[5];          /* 1,2,3,4,7 */
    LONG ref;
    DDraw *dd;
    DWORD caps;
    int w, h, bpp, pitch;
    DWORD rmask, gmask, bmask;
    HDC dc; HBITMAP bmp, oldbmp; BYTE *bits;
    int primary;          /* プライマリサーフェス */
    int virt;             /* 窓モードのプライマリ (実体はキャンバス) */
    Surf *back;           /* フリップチェーンの次 */
    Surf *front;          /* バックバッファの場合の親 */
    Pal *pal;
    Clip *clip;
    DDCOLORKEY ck_src, ck_dst;
    DWORD ckflags;
    int dc_out;
    BYTE *shadow; int sh_w, sh_h, sh_pitch; /* 窓モードプライマリの Lock 用 */
    DWORD uniq;
    double last_present;
    int ext;              /* SetSurfaceDesc でアプリのメモリを使っている (DIB を持たない) */
    HDC xdc; HBITMAP xbmp, xold; BYTE *xbits; /* ext 用 GetDC の一時 DIB */
    /* Direct3D (d3d6.c) */
    DWORD amask;          /* アルファのマスク (テクスチャ) */
    int zbuf;             /* Z バッファ */
    Surf *zatt;           /* 付けられた Z バッファ */
    IFACE tex[2];         /* IDirect3DTexture / IDirect3DTexture2 */
    void *be_rt, *be_tex; /* Direct3D9 側の実体 (描画先 / テクスチャ) */
    int gpu_newer;        /* GPU 側 (描画先) の内容の方が新しい */
    int cpu_dirty;        /* CPU 側 (DIB) の内容が変わった */
};


void *mkw_s_iface(Surf *s, int v);
void mkw_surf_pf(Surf *s, DDPIXELFORMAT *pf);

/* d3d6.c: Direct3D (DirectX 6 世代) */
HRESULT d3d6_query_d3d(DDraw *dd, REFIID iid, void **out);
HRESULT d3d6_query_texture(Surf *s, REFIID iid, void **out);
void d3d6_init_surface(Surf *s);
void d3d6_cpu_access(Surf *s);     /* CPU で読み書きする前 (GPU の描画結果を DIB へ) */
void d3d6_cpu_modified(Surf *s);   /* CPU で書き換えた後 */
void d3d6_surface_freed(Surf *s);
void d3d6_flip(Surf *front, Surf *back);
void d3d6_ddraw_freed(DDraw *dd);

#endif
