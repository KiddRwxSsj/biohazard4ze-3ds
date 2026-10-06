// QEGL + IGLES. both take this first and hand results back through a trailing
// out pointer, the int return is an aee error code
#include <string.h>
#include "aee.h"
#include "gl.h"

#define EGL_TRUE 1
#define IID_EGL10 0x0103d8ed
#define IID_EGL11 0x0103d8ee
#define IID_GLES10 0x0103d8dd
#define IID_GLES11 0x0103d8ea

#define SOFT __attribute__((pcs("aapcs")))

static void *vt_egl[31];
static void *vt_gles[156];
static aee_obj egl = { vt_egl };
static aee_obj gles = { vt_gles };

static int one(void) { return 1; }
static int zero(void) { return 0; }

static int egl_qi(void *s, u32 iid, void **out)
{
    if (iid == IID_GLES10 || iid == IID_GLES11) { *out = &gles; return SUCCESS; }
    if (iid == IID_EGL10 || iid == IID_EGL11) { *out = &egl; return SUCCESS; }
    dbg("egl: QI %08lx declined", iid);
    *out = NULL;
    return ECLASSNOTSUPPORT;
}

#define OUT(p, v) do { if (p) *(u32 *)(p) = (u32)(v); } while (0)

static int e_geterror(void *s, u32 *o) { OUT(o, 0x3000); return SUCCESS; }
static int e_getdisplay(void *s, void *id, u32 *o) { OUT(o, 1); return SUCCESS; }
static int e_init(void *s, u32 d, int *maj, int *min, u32 *o)
{
    if (maj) *maj = 1;
    if (min) *min = 1;
    OUT(o, EGL_TRUE);
    return SUCCESS;
}
static int e_true1(void *s, u32 a, u32 *o) { OUT(o, EGL_TRUE); return SUCCESS; }
static int e_true2(void *s, u32 a, u32 b, u32 *o) { OUT(o, EGL_TRUE); return SUCCESS; }
static int e_true4(void *s, u32 a, u32 b, u32 c, u32 d, u32 *o) { OUT(o, EGL_TRUE); return SUCCESS; }

static int e_querystring(void *s, u32 d, int name, u32 *o)
{
    const char *r = name == 0x3053 ? "Qualcomm" : name == 0x3054 ? "1.1" : "";
    OUT(o, r);
    return SUCCESS;
}

static int e_getconfigs(void *s, u32 d, u32 *cfg, int size, int *num, u32 *o)
{
    if (cfg && size > 0) cfg[0] = 1;
    if (num) *num = 1;
    OUT(o, EGL_TRUE);
    return SUCCESS;
}

static int e_choose(void *s, u32 d, const int *attr, u32 *cfg, int size, int *num, u32 *o)
{
    return e_getconfigs(s, d, cfg, size, num, o);
}

static int cfg_attr(int a)
{
    switch (a) {
    case 0x3020: return 16;           // buffer size
    case 0x3021: return 0;            // alpha
    case 0x3022: return 5;            // blue
    case 0x3023: return 6;            // green
    case 0x3024: return 5;            // red
    case 0x3025: return 16;           // depth
    case 0x3026: return 0;            // stencil
    case 0x3028: return 1;            // config id
    case 0x3033: return 0x5;          // surface type: window|pbuffer
    case 0x3056: return ZH;           // height
    case 0x3057: return ZW;           // width
    }
    return 0;
}

static int e_cfgattr(void *s, u32 d, u32 cfg, int attr, int *val, u32 *o)
{
    if (val) *val = cfg_attr(attr);
    OUT(o, EGL_TRUE);
    return SUCCESS;
}

static int e_window(void *s, u32 d, u32 cfg, void *win, const int *attr, u32 *o) { OUT(o, 2); return SUCCESS; }
static int e_pbuffer(void *s, u32 d, u32 cfg, const int *attr, u32 *o) { OUT(o, 4); return SUCCESS; }
static int e_context(void *s, u32 d, u32 cfg, u32 share, const int *attr, u32 *o) { OUT(o, 3); return SUCCESS; }
static int e_querysurf(void *s, u32 d, u32 surf, int attr, int *val, u32 *o)
{
    if (val) *val = cfg_attr(attr);
    OUT(o, EGL_TRUE);
    return SUCCESS;
}
static int e_swap(void *s, u32 d, u32 surf, u32 *o)
{
    gl_swap();
    OUT(o, EGL_TRUE);
    return SUCCESS;
}
static int e_curctx(void *s, u32 *o) { OUT(o, 3); return SUCCESS; }
static int e_cursurf(void *s, int which, u32 *o) { OUT(o, 2); return SUCCESS; }
static int e_curdisp(void *s, u32 *o) { OUT(o, 1); return SUCCESS; }

void *qegl_new(void) { return &egl; }

// ------------------------------------------------------------------ gles thunks
static inline GLfixed X(float f) { return (GLfixed)(f * 65536.0f); }

static int g_qi(void *s, u32 iid, void **out) { *out = &gles; return SUCCESS; }

SOFT static int g_alphafunc(void *s, GLenum f, float r) { gl_AlphaFuncx(f, X(r)); return 0; }
SOFT static int g_clearcolor(void *s, float r, float gg, float b, float a) { gl_ClearColorx(X(r), X(gg), X(b), X(a)); return 0; }
SOFT static int g_cleardepthf(void *s, float d) { gl_ClearDepthx(X(d)); return 0; }
SOFT static int g_color4f(void *s, float r, float gg, float b, float a) { gl_Color4x(X(r), X(gg), X(b), X(a)); return 0; }
SOFT static int g_fogf(void *s, GLenum p, float v) { gl_Fogx(p, p == 0x0B65 ? (GLfixed)v : X(v)); return 0; }
static int g_fogfv(void *s, GLenum p, const float *v)
{
    GLfixed t[4] = { X(v[0]), X(v[1]), X(v[2]), X(v[3]) };
    if (p == 0x0B65) t[0] = (GLfixed)v[0];
    gl_Fogxv(p, t);
    return 0;
}
SOFT static int g_frustumf(void *s, float l, float r, float b, float t, float n, float f) { gl_Frustumx(X(l), X(r), X(b), X(t), X(n), X(f)); return 0; }
static int g_loadmatrixf(void *s, const float *m) { gl_LoadMatrixf(m); return 0; }
static int g_multmatrixf(void *s, const float *m) { gl_MultMatrixf(m); return 0; }
SOFT static int g_orthof(void *s, float l, float r, float b, float t, float n, float f) { gl_Orthox(X(l), X(r), X(b), X(t), X(n), X(f)); return 0; }
SOFT static int g_polyoff(void *s, float f, float u) { gl_PolygonOffsetx(X(f), X(u)); return 0; }
SOFT static int g_rotatef(void *s, float a, float x, float y, float z) { gl_Rotatex(X(a), X(x), X(y), X(z)); return 0; }
SOFT static int g_scalef(void *s, float x, float y, float z) { gl_Scalex(X(x), X(y), X(z)); return 0; }
SOFT static int g_texenvf(void *s, GLenum t, GLenum p, float v) { gl_TexEnvx(t, p, (GLfixed)v); return 0; }
SOFT static int g_texparamf(void *s, GLenum t, GLenum p, float v) { gl_TexParameterx(t, p, (GLfixed)v); return 0; }
SOFT static int g_translatef(void *s, float x, float y, float z) { gl_Translatex(X(x), X(y), X(z)); return 0; }

static int g_activetex(void *s, GLenum u) { gl_ActiveTexture(u); return 0; }
static int g_alphafuncx(void *s, GLenum f, GLfixed r) { gl_AlphaFuncx(f, r); return 0; }
static int g_bindtex(void *s, GLenum t, GLuint n) { gl_BindTexture(t, n); return 0; }
static int g_blendfunc(void *s, GLenum a, GLenum b) { gl_BlendFunc(a, b); return 0; }
static int g_clear(void *s, GLbitfield m) { gl_Clear(m); return 0; }
static int g_clearcolorx(void *s, GLfixed r, GLfixed gg, GLfixed b, GLfixed a) { gl_ClearColorx(r, gg, b, a); return 0; }
static int g_cleardepthx(void *s, GLfixed d) { gl_ClearDepthx(d); return 0; }
static int g_clientactive(void *s, GLenum u) { gl_ClientActiveTexture(u); return 0; }
static int g_color4x(void *s, GLfixed r, GLfixed gg, GLfixed b, GLfixed a) { gl_Color4x(r, gg, b, a); return 0; }
static int g_colormask(void *s, GLboolean r, GLboolean gg, GLboolean b, GLboolean a) { gl_ColorMask(r, gg, b, a); return 0; }
static int g_colorptr(void *s, GLint n, GLenum t, GLsizei st, const void *p) { gl_ColorPointer(n, t, st, p); return 0; }
static int g_ctex(void *s, GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d)
{
    gl_CompressedTexImage2D(t, l, f, w, h, b, sz, d);
    return 0;
}
static int g_cullface(void *s, GLenum m) { gl_CullFace(m); return 0; }
static int g_deltex(void *s, GLsizei n, const GLuint *t) { gl_DeleteTextures(n, t); return 0; }
static int g_depthfunc(void *s, GLenum f) { gl_DepthFunc(f); return 0; }
static int g_depthmask(void *s, GLboolean m) { gl_DepthMask(m); return 0; }
static int g_disable(void *s, GLenum c) { gl_Disable(c); return 0; }
static int g_disablecs(void *s, GLenum a) { gl_DisableClientState(a); return 0; }
static int g_drawarrays(void *s, GLenum m, GLint f, GLsizei c) { gl_DrawArrays(m, f, c); return 0; }
static int g_drawelements(void *s, GLenum m, GLsizei c, GLenum t, const void *i) { gl_DrawElements(m, c, t, i); return 0; }
static int g_enable(void *s, GLenum c) { gl_Enable(c); return 0; }
static int g_enablecs(void *s, GLenum a) { gl_EnableClientState(a); return 0; }
static int g_nop(void *s) { return 0; }
static int g_fogx(void *s, GLenum p, GLfixed v) { gl_Fogx(p, v); return 0; }
static int g_fogxv(void *s, GLenum p, const GLfixed *v) { gl_Fogxv(p, v); return 0; }
static int g_frontface(void *s, GLenum m) { gl_FrontFace(m); return 0; }
static int g_frustumx(void *s, GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) { gl_Frustumx(l, r, b, t, n, f); return 0; }
static int g_gentex(void *s, GLsizei n, GLuint *t) { gl_GenTextures(n, t); return 0; }
static int g_geterror(void *s, u32 *o) { OUT(o, gl_GetError()); return 0; }
static int g_getintegerv(void *s, GLenum p, GLint *v) { gl_GetIntegerv(p, v); return 0; }
static int g_getstring(void *s, GLenum n, u32 *o) { OUT(o, gl_GetString(n)); return 0; }
static int g_hint(void *s, GLenum t, GLenum m) { return 0; }
static int g_loadidentity(void *s) { gl_LoadIdentity(); return 0; }
static int g_loadmatrixx(void *s, const GLfixed *m) { gl_LoadMatrixx(m); return 0; }
static int g_matrixmode(void *s, GLenum m) { gl_MatrixMode(m); return 0; }
static int g_multmatrixx(void *s, const GLfixed *m) { gl_MultMatrixx(m); return 0; }
static int g_normalptr(void *s, GLenum t, GLsizei st, const void *p) { gl_NormalPointer(t, st, p); return 0; }
static int g_orthox(void *s, GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) { gl_Orthox(l, r, b, t, n, f); return 0; }
static int g_polyoffx(void *s, GLfixed f, GLfixed u) { gl_PolygonOffsetx(f, u); return 0; }
static int g_popmatrix(void *s) { gl_PopMatrix(); return 0; }
static int g_pushmatrix(void *s) { gl_PushMatrix(); return 0; }
static int g_rotatex(void *s, GLfixed a, GLfixed x, GLfixed y, GLfixed z) { gl_Rotatex(a, x, y, z); return 0; }
static int g_scalex(void *s, GLfixed x, GLfixed y, GLfixed z) { gl_Scalex(x, y, z); return 0; }
static int g_scissor(void *s, GLint x, GLint y, GLsizei w, GLsizei h) { gl_Scissor(x, y, w, h); return 0; }
static int g_texcoordptr(void *s, GLint n, GLenum t, GLsizei st, const void *p) { gl_TexCoordPointer(n, t, st, p); return 0; }
static int g_texenvx(void *s, GLenum t, GLenum p, GLfixed v) { gl_TexEnvx(t, p, v); return 0; }
static int g_texenvxv(void *s, GLenum t, GLenum p, const GLfixed *v) { gl_TexEnvxv(t, p, v); return 0; }
static int g_teximage(void *s, GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d)
{
    gl_TexImage2D(t, l, ifmt, w, h, b, f, ty, d);
    return 0;
}
static int g_texparamx(void *s, GLenum t, GLenum p, GLfixed v) { gl_TexParameterx(t, p, v); return 0; }
static int g_texsub(void *s, GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d)
{
    gl_TexSubImage2D(t, l, x, y, w, h, f, ty, d);
    return 0;
}
static int g_translatex(void *s, GLfixed x, GLfixed y, GLfixed z) { gl_Translatex(x, y, z); return 0; }
static int g_vertexptr(void *s, GLint n, GLenum t, GLsizei st, const void *p) { gl_VertexPointer(n, t, st, p); return 0; }
static int g_viewport(void *s, GLint x, GLint y, GLsizei w, GLsizei h) { gl_Viewport(x, y, w, h); return 0; }
static int g_color4ub(void *s, GLubyte r, GLubyte gg, GLubyte b, GLubyte a) { gl_Color4ub(r, gg, b, a); return 0; }
static int g_isenabled(void *s, GLenum c, u32 *o) { OUT(o, gl_IsEnabled(c)); return 0; }
static int g_istexture(void *s, GLuint t, u32 *o) { OUT(o, gl_IsTexture(t)); return 0; }
static int g_texenviv(void *s, GLenum t, GLenum p, const GLint *v) { gl_TexEnvxv(t, p, v); return 0; }

void egl_init(void)
{
    void **v = vt_egl;
    vt_fill(v, 31);
    v[0] = one; v[1] = zero; v[2] = egl_qi; v[3] = e_geterror; v[4] = e_getdisplay; v[5] = e_init;
    v[6] = e_true1; v[7] = e_querystring; v[8] = e_getconfigs; v[9] = e_choose; v[10] = e_cfgattr;
    v[11] = e_window; v[12] = e_window; v[13] = e_pbuffer; v[14] = e_true2; v[15] = e_querysurf;
    v[16] = e_context; v[17] = e_true2; v[18] = e_true4; v[19] = e_curctx; v[20] = e_cursurf;
    v[21] = e_curdisp; v[22] = e_querysurf; v[23] = e_true1; v[24] = e_true1; v[25] = e_swap;
    v[26] = e_true2; v[27] = e_querysurf; v[28] = e_true2; v[29] = e_true2; v[30] = e_true2;

    v = vt_gles;
    vt_fill(v, 156);
    v[0] = one; v[1] = zero; v[2] = g_qi;
    v[3] = g_alphafunc; v[4] = g_clearcolor; v[5] = g_cleardepthf; v[6] = g_color4f; v[8] = g_fogf;
    v[9] = g_fogfv; v[10] = g_frustumf; v[16] = g_loadmatrixf; v[19] = g_multmatrixf; v[22] = g_orthof;
    v[24] = g_polyoff; v[25] = g_rotatef; v[26] = g_scalef; v[27] = g_texenvf; v[29] = g_texparamf;
    v[30] = g_translatef;
    v[31] = g_activetex; v[32] = g_alphafuncx; v[33] = g_bindtex; v[34] = g_blendfunc; v[35] = g_clear;
    v[36] = g_clearcolorx; v[37] = g_cleardepthx; v[38] = g_nop; v[39] = g_clientactive; v[40] = g_color4x;
    v[41] = g_colormask; v[42] = g_colorptr; v[43] = g_ctex; v[47] = g_cullface; v[48] = g_deltex;
    v[49] = g_depthfunc; v[50] = g_depthmask; v[51] = g_nop; v[52] = g_disable; v[53] = g_disablecs;
    v[54] = g_drawarrays; v[55] = g_drawelements; v[56] = g_enable; v[57] = g_enablecs; v[58] = g_nop;
    v[59] = g_nop; v[60] = g_fogx; v[61] = g_fogxv; v[62] = g_frontface; v[63] = g_frustumx;
    v[64] = g_gentex; v[65] = g_geterror; v[66] = g_getintegerv; v[67] = g_getstring; v[68] = g_hint;
    v[69] = g_hint; v[70] = g_hint; v[71] = g_nop; v[72] = g_nop; v[73] = g_nop; v[74] = g_loadidentity;
    v[75] = g_loadmatrixx; v[76] = g_nop; v[77] = g_nop; v[78] = g_nop; v[79] = g_matrixmode;
    v[80] = g_multmatrixx; v[83] = g_normalptr; v[84] = g_orthox; v[85] = g_hint; v[86] = g_nop;
    v[87] = g_polyoffx; v[88] = g_popmatrix; v[89] = g_pushmatrix; v[91] = g_rotatex; v[94] = g_scalex;
    v[95] = g_scissor; v[96] = g_nop; v[97] = g_nop; v[98] = g_nop; v[99] = g_nop; v[100] = g_texcoordptr;
    v[101] = g_texenvx; v[102] = g_texenvxv; v[103] = g_teximage; v[104] = g_texparamx; v[105] = g_texsub;
    v[106] = g_translatex; v[107] = g_vertexptr; v[108] = g_viewport;
    v[123] = g_color4ub; v[138] = g_isenabled; v[139] = g_istexture;
    v[142] = g_texenvx; v[143] = g_texenviv; v[144] = g_texparamx;
}
