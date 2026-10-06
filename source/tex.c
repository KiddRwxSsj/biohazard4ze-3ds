// texture decoders straight into pica tiled layout (8x8 tiles, morton inside)
// atc rule: c0 is 555 with a mode bit on top, c1 is 565.
// mode 0: c0, 5/8 c0 + 3/8 c1, 3/8 c0 + 5/8 c1, c1. mode 1: black, c0 - c1/4, c0, c1
#include <string.h>
#include "tex.h"

// gl row 0 is t=0 but pica samples v=0 from the last row, so rows go in bottom up
#ifndef TEX_FLIP
#define TEX_FLIP 1
#endif

static inline u32 tile_off(int x, int y, int w)
{
    u32 m = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m;
}

static inline int row(int y, int h) { return TEX_FLIP ? h - 1 - y : y; }

static inline u16 to565(int r, int g, int b) { return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); }
static inline u16 to4444(int r, int g, int b, int a) { return (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4); }

static void atc_palette(u16 c0, u16 c1, u8 pal[4][3])
{
    int a[3] = { (c0 >> 10 & 31) * 255 / 31, (c0 >> 5 & 31) * 255 / 31, (c0 & 31) * 255 / 31 };
    int b[3] = { (c1 >> 11) * 255 / 31, (c1 >> 5 & 63) * 255 / 63, (c1 & 31) * 255 / 31 };
    for (int i = 0; i < 3; i++) {
        if (c0 & 0x8000) {
            int d = a[i] - b[i] / 4;
            pal[0][i] = 0;
            pal[1][i] = d < 0 ? 0 : d;
            pal[2][i] = a[i];
        } else {
            pal[0][i] = a[i];
            pal[1][i] = (5 * a[i] + 3 * b[i]) / 8;
            pal[2][i] = (3 * a[i] + 5 * b[i]) / 8;
        }
        pal[3][i] = b[i];
    }
}

// dst w/h are the pica texture size (>= 8), sw/sh the source size
void tex_atc(u16 *dst, int dw, int dh, const u8 *src, int sw, int sh, bool alpha)
{
    int bw = (sw + 3) / 4, bh = (sh + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const u8 *blk = src + (by * bw + bx) * (alpha ? 16 : 8);
            const u8 *ab = NULL;
            if (alpha) { ab = blk; blk += 8; }
            u16 c0 = blk[0] | blk[1] << 8, c1 = blk[2] | blk[3] << 8;
            u32 idx = blk[4] | blk[5] << 8 | blk[6] << 16 | (u32)blk[7] << 24;
            u8 pal[4][3];
            atc_palette(c0, c1, pal);
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= dw || y >= dh) continue;
                const u8 *c = pal[idx >> (2 * i) & 3];
                u16 px;
                if (alpha) {
                    int a = (ab[i >> 1] >> ((i & 1) * 4)) & 15;
                    px = (c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 | a;
                } else {
                    px = to565(c[0], c[1], c[2]);
                }
                dst[tile_off(x, row(y, dh), dw)] = px;
            }
        }
}

// GL_PALETTEn_* : palette first, then 8 or 4 bit indices
int tex_palette(u16 *dst, int dw, int dh, const u8 *src, int sw, int sh, u32 fmt, bool *has_alpha)
{
    bool p8 = fmt >= 0x8B95;
    int kind = fmt - (p8 ? 0x8B95 : 0x8B90);
    static const u8 esz[5] = { 3, 4, 2, 2, 2 };
    int n = p8 ? 256 : 16;
    u16 pal[256];
    for (int i = 0; i < n; i++) {
        const u8 *e = src + i * esz[kind];
        u16 v = e[0] | e[1] << 8;
        switch (kind) {
        case 0: pal[i] = to565(e[0], e[1], e[2]); break;
        case 1: pal[i] = to4444(e[0], e[1], e[2], e[3]); break;
        case 2: pal[i] = v; break;
        case 3: pal[i] = v; break;
        case 4: pal[i] = v; break;
        }
    }
    *has_alpha = kind == 1 || kind == 3 || kind == 4;
    const u8 *ix = src + n * esz[kind];
    for (int y = 0; y < sh && y < dh; y++)
        for (int x = 0; x < sw && x < dw; x++) {
            int i = y * sw + x;
            int k = p8 ? ix[i] : (i & 1 ? ix[i >> 1] & 15 : ix[i >> 1] >> 4);
            dst[tile_off(x, row(y, dh), dw)] = pal[k];
        }
    // pica format to use
    if (kind == 4) return GPU_RGBA5551;
    return *has_alpha ? GPU_RGBA4 : GPU_RGB565;
}

// plain glTexImage2D data. returns the pica format, -1 if we dont know it
int tex_raw(void *dstv, int dw, int dh, const u8 *src, int sw, int sh, u32 fmt, u32 type, bool *has_alpha)
{
    u8 *d8 = dstv;
    u16 *d16 = dstv;
    u32 *d32 = dstv;
    *has_alpha = fmt == 0x1908 || fmt == 0x1906 || fmt == 0x190A;
    for (int y = 0; y < sh && y < dh; y++)
        for (int x = 0; x < sw && x < dw; x++) {
            u32 o = tile_off(x, row(y, dh), dw);
            int i = y * sw + x;
            if (type == 0x1401) {
                const u8 *p;
                switch (fmt) {
                case 0x1908: p = src + i * 4; d32[o] = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; break;
                case 0x1907: p = src + i * 3; d8[o * 3] = p[2]; d8[o * 3 + 1] = p[1]; d8[o * 3 + 2] = p[0]; break;
                case 0x1909: d8[o] = src[i]; break;
                case 0x1906: d8[o] = src[i]; break;
                case 0x190A: p = src + i * 2; d16[o] = p[0] << 8 | p[1]; break;
                default: return -1;
                }
            } else if (type == 0x8363 || type == 0x8033 || type == 0x8034) {
                d16[o] = ((const u16 *)src)[i];
            } else {
                return -1;
            }
        }
    if (type == 0x8363) return GPU_RGB565;
    if (type == 0x8033) return GPU_RGBA4;
    if (type == 0x8034) return GPU_RGBA5551;
    switch (fmt) {
    case 0x1908: return GPU_RGBA8;
    case 0x1907: return GPU_RGB8;
    case 0x1909: return GPU_L8;
    case 0x1906: return GPU_A8;
    case 0x190A: return GPU_LA8;
    }
    return -1;
}

// rgb565 in place: swap r/b back where b leads r by > LO, ramped over LO..HI so the skin
// edge doesn't band. fitted vs the mobile build (same ganado art, channels in order)
void tex_unswap_rb(u16 *px, int n)
{
    enum { LO = 4, HI = 20 };
    for (int i = 0; i < n; i++) {
        u16 p = px[i];
        int r = (p >> 11) << 3 | p >> 13, b = (p & 31) << 3 | (p & 31) >> 2;
        int d = b - r;
        if (d <= LO) continue;
        int w = d >= HI ? 256 : (d - LO) * 256 / (HI - LO);
        int nr = r + (d * w >> 8), nb = b - (d * w >> 8);
        px[i] = (nr >> 3) << 11 | (p & 0x07e0) | nb >> 3;
    }
}

int tex_bpp(int fmt)
{
    switch (fmt) {
    case GPU_RGBA8: return 32;
    case GPU_RGB8: return 24;
    case GPU_L8: case GPU_A8: return 8;
    default: return 16;
    }
}
