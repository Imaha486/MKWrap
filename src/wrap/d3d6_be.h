/* d3d6.c と d3d6_be.c (Direct3D9 側) の間の取り決め。d3d9.h / d3d.h のどちらにも依存しない */
#ifndef D3D6_BE_H
#define D3D6_BE_H
#include <windows.h>

typedef struct {
    DWORD type;           /* D3DLIGHTTYPE (DirectX 6) */
    float color[4];       /* r g b a */
    float pos[3], dir[3];
    float range, falloff, att[3], theta, phi;
} be_light_t;

int be_init(void);
void be_set_window(HWND h);
int be_check(void);   /* 0 = 正常 / 1 = デバイスを作り直した / -1 = 消失中 */
void *be_rt_new(int w, int h);
void be_rt_free(void *rt);
void be_rt_read(void *rt, BYTE *bits, int pitch, int bpp, DWORD rm, DWORD gm, DWORD bm);
void be_rt_write(void *rt, const BYTE *bits, int pitch, int bpp, DWORD rm, DWORD gm, DWORD bm);
void *be_tex_new(int w, int h);
void be_tex_free(void *t);
void be_tex_write(void *t, const DWORD *argb);
int be_target(void *rt);   /* 戻り値: Z バッファがあるか */
void be_rs(DWORD s, DWORD v);
void be_tss(DWORD stage, DWORD t, DWORD v);
void be_samp(DWORD stage, DWORD t, DWORD v);
void be_settex(DWORD stage, void *t);
void be_viewport(DWORD x, DWORD y, DWORD w, DWORD h, float mn, float mx);
void be_clear(DWORD n, const LONG *rects, DWORD flags, DWORD color, float z, DWORD stencil);
void be_scene(int begin);
void be_transform(DWORD st9, const float *m16);
void be_material(const float *m17);
void be_light(DWORD i, const be_light_t *l);
void be_draw(DWORD prim, DWORD fvf, DWORD stride, const void *v, DWORD nv, const WORD *idx, DWORD ni);

/* Direct3D9 のステート番号 (d3d9types.h と同じ値) */
#define BE_SAMP_ADDRESSU 1
#define BE_SAMP_ADDRESSV 2
#define BE_SAMP_BORDERCOLOR 4
#define BE_SAMP_MAGFILTER 5
#define BE_SAMP_MINFILTER 6
#define BE_SAMP_MIPFILTER 7
#define BE_SAMP_MIPMAPLODBIAS 8
#define BE_SAMP_MAXANISOTROPY 10
#define BE_RS_LIGHTING 137
#define BE_RS_DEPTHBIAS 195
#endif
