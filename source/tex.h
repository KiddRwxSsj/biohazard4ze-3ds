#pragma once
#include <3ds.h>
#include <stdbool.h>

void tex_atc(u16 *dst, int dw, int dh, const u8 *src, int sw, int sh, bool alpha);
int  tex_palette(u16 *dst, int dw, int dh, const u8 *src, int sw, int sh, u32 fmt, bool *has_alpha);
int  tex_raw(void *dst, int dw, int dh, const u8 *src, int sw, int sh, u32 fmt, u32 type, bool *has_alpha);
int  tex_bpp(int fmt);
void tex_unswap_rb(u16 *px, int n);
