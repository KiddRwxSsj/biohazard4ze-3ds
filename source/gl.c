// the gles 1.x subset bio4 uses, on citro3d.
// client arrays get copied per draw into a linear ring (game arrays live on the
// heap, the stack and in the mod image, none of it gpu visible). GL_FIXED stays
// fixed, the shader rebuilds it.
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "aee.h"
#include "gl.h"
#include "tex.h"
#include "vsh_shbin.h"

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140C

#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702

#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_ALPHA_TEST 0x0BC0
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_FOG 0x0B60
#define GL_SCISSOR_TEST 0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_LIGHTING 0x0B50

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900

#define GL_REPLACE 0x1E01
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_BLEND_ENV 0x0BE2
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201

#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_LINEAR 0x2601

#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_REPEAT 0x2901

#define CAP_BLEND   BIT(0)
#define CAP_ALPHA   BIT(1)
#define CAP_DEPTH   BIT(2)
#define CAP_CULL    BIT(3)
#define CAP_FOG     BIT(4)
#define CAP_SCISSOR BIT(5)
#define CAP_POFF    BIT(6)

#define CL_VERTEX BIT(0)
#define CL_COLOR  BIT(1)
#define CL_NORMAL BIT(2)
#define CL_TEX0   BIT(3)
#define CL_TEX1   BIT(4)

#define MAXTEX 2048
#define RING_SIZE (1 << 20)   // a busy frame uses ~120k

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { int size; GLenum type; int stride; const u8 *ptr; } arr_t;

typedef struct {
    C3D_Tex t;
    bool ok, alpha;
    GLenum minf, magf, ws, wt;
    u32 sig;    // fnv-1a of the uploaded data: names a texture across runs, gl ids change per level
} tex_t;

// texture fingerprints (fnv of the game's upload) that tell the screens apart
#define SIG_TITLE 0x19137379u    // title art, 512x256
#define SIG_FONT 0xba093d29u     // bitmap font, 512x512
// status inventory panels (256x128). the merchant and mission screens may share it, which is
// why gl_swap also wants to have come from gameplay before it calls a frame the inventory
static bool sig_menu(u32 sig) { return sig == 0x6b060e8cu; }
// ganado skin, both ch_gna_all.mcts variants. zeebo conversion swapped r/b on the skin
// (blue villagers), mobile has it in order. fixed at upload
static bool sig_gna(u32 sig) { return sig == 0x7082f053u || sig == 0x00fc34fau; }


static struct {
    C3D_Mtx st[3][24];
    int sp[3];
    int mode;
    bool mvp_dirty, tm_dirty;

    arr_t va, ca, na, ta[2];
    u32 client;
    int unit, cunit;

    u32 caps;
    bool tex2d[2];
    GLuint bound[2];
    GLenum env[2];
    u32 envc[2];

    float color[4];
    float clearc[4];
    float cleard;
    GLenum bs, bd, af, df, cull, front;
    float aref;
    bool dmask;
    u8 cmask;

    GLenum fogmode;
    float fogd, fogs, foge;
    u32 fogc;
    bool fog_dirty, fog_params;
    float poff;

    int vp[4], sc[4];
    bool fix_dirty;
    bool state_dirty;
    GLenum err;
} g;

static tex_t *texs[MAXTEX];
static GLuint next_name = 1;

static C3D_RenderTarget *target;
// bottom screen: the inventory (and the title art) render here at the game's 640x480 and get
// shrunk 2:1 by the display transfer, so the bitmap font comes out filtered, not decimated.
// not linked to a screen, gl_swap presents it by hand so it only updates on frames that belong
// there and otherwise keeps the last image
static C3D_RenderTarget *bot_target;
enum { ROUTE_TOP, ROUTE_BOT, ROUTE_BOTH };
enum { FC_GAME, FC_MENU, FC_TITLE, FC_OTHER };
static int route = ROUTE_TOP;   // where this frame's draws go, from the last frame's class
static int cur_tgt;             // 0 top, 1 bottom
static bool bot_drawn;
static bool fs_menu, fs_title, fs_world;   // what this frame drew, for gl_swap's verdict
static int last_class = FC_OTHER;
static bool inv_seen;           // an inventory frame was shown this session
static u16 *bot_img;            // copy of the bottom image, survives the console toggle
bool gl_bottom_owned;           // main.c moves the console off the bottom once this is set
bool gl_bottom_on = true;       // l+r+b gives the bottom back to the console
#define BOT_W 240
#define BOT_H 320
#define BOT_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))
static DVLB_s *vsh_dvlb;
static shaderProgram_s prog;
static int u_mvp, u_tmat, u_k;
static C3D_Mtx fix;
static C3D_FogLut foglut;

static tex_t *hw_tex = (tex_t *)-1;  // what C3D_TexBind last saw, reset on any tex change
static GLenum hw_env;
static bool hw_alpha;
static bool hw_white;
static bool tex_only;   // MODULATE by a constant white color: same as REPLACE, see draw()
static bool hw_eye;   // uploaded mvp leaves the modelview out, copy_fixed_eye applied it
static float fog_n, fog_f;
// on hw the first draw after the fog unit switches on, or after a fog lut upload, gets a garbage
// fog factor: fully fogged (white) or fog colored against the fog, i.e. gone. that was leon's
// hair, the first fogged draw of every frame (the clear and the hud run with fog off). azahar
// applies fog state immediately and never shows it. fog_prime() takes the hit with a draw that
// writes nothing
static bool hw_fog;        // fog unit on as of the last emitted draw
static bool lut_pending;   // a fog lut was bound since the last fogged draw

static u8 *ring[2];
static u32 ring_used;
static int ring_cur;
static bool in_frame;
static int frame_draws;

// hw debugging, both driven from main.c. gl_dump (L+R+Y) logs every draw of the next frame,
// gl_probe (L+R+X) overrides one piece of state so a hw-only bug can be bisected on the device
volatile bool gl_dump;
int gl_probe;
static bool dumping;
static float dump_z0, dump_z1;

enum { PROBE_OFF, PROBE_AT_NOFOG, PROBE_AT_NOTEST, PROBE_AT_TEXALPHA, PROBE_REBIND, PROBE_AT_DEPTH, PROBE_NOFOG,
       PROBE_NOPRIME, PROBES };
static const char *const probe_names[PROBES] = {
    "off", "alpha-tested: no fog", "alpha-tested: no alpha test", "alpha-tested: tex alpha only",
    "all: rebind tex+state", "alpha-tested: depth always", "all: no fog", "all: no fog priming",
};
int gl_probe_next(void) { gl_probe = (gl_probe + 1) % PROBES; return gl_probe; }

// back from the home menu: citro3d re-uploads the fog lut and the fog unit may have been
// reconfigured, so the next fogged draw gets primed again
void gl_resume(void) { hw_fog = false; lut_pending = true; }
const char *gl_probe_name(int p) { return probe_names[p]; }

volatile bool gl_paused;  // set while apt has us suspended, the gpu isn't ours then
bool gl_wide = true;   // stretch to 400x240, select+start drops to 4:3
u64 gl_busy_ticks;
u32 gl_swaps;  // time the game spent in its callbacks, main loop adds to it

static inline float fx(GLfixed v) { return v * (1.0f / 65536.0f); }

static void mtx_set(C3D_Mtx *m, int r, float a, float b, float c, float d)
{
    m->r[r].x = a; m->r[r].y = b; m->r[r].z = c; m->r[r].w = d;
}

static C3D_Mtx *cur(void) { return &g.st[g.mode][g.sp[g.mode]]; }

static void touch_mtx(void)
{
    if (g.mode == 2) g.tm_dirty = true;
    else g.mvp_dirty = true;
    if (g.mode == 1) g.fog_dirty = true;
}

static void *ring_alloc(u32 n)
{
    n = (n + 15) & ~15;
    if (ring_used + n > RING_SIZE) return NULL;
    void *p = ring[ring_cur] + ring_used;
    ring_used += n;
    return p;
}

// ------------------------------------------------------------------ bottom screen
static PrintConsole *con;
static u16 con_dummy[BOT_W * BOT_H];   // where console output goes while the game owns the bottom
#define INV_FILE GAME_DIR "inventory.bin"

void gl_set_console(PrintConsole *c) { con = c; }

static u16 *bot_fb(void) { return (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); }

// first bottom present: console output moves to a dummy buffer, and any dirty console lines are
// cleaned out of the cache now so they can't land on top of the gpu's image later
static void bot_take(void)
{
    if (con) con->frameBuffer = con_dummy;
    GSPGPU_FlushDataCache(bot_fb(), BOT_W * BOT_H * 2);
    gl_bottom_owned = true;
}

static void bot_keep(bool to_sd)
{
    u16 *fb = bot_fb();
    GSPGPU_InvalidateDataCache(fb, BOT_W * BOT_H * 2);
    memcpy(bot_img, fb, BOT_W * BOT_H * 2);
    if (!to_sd) return;
    FILE *f = fopen(INV_FILE, "wb");
    if (f) { fwrite(bot_img, 2, BOT_W * BOT_H, f); fclose(f); }
}

static void bot_show_img(void)
{
    u16 *fb = bot_fb();
    memcpy(fb, bot_img, BOT_W * BOT_H * 2);
    GSPGPU_FlushDataCache(fb, BOT_W * BOT_H * 2);
}

// gameplay before the inventory was opened this session: last session's inventory from sd.
// first run ever there's no file, the bottom keeps the title art
static void bot_placeholder(void)
{
    if (!gl_bottom_on || !gl_bottom_owned) return;
    FILE *f = fopen(INV_FILE, "rb");
    if (!f) return;
    bool ok = fread(bot_img, 2, BOT_W * BOT_H, f) == BOT_W * BOT_H;
    fclose(f);
    if (ok) bot_show_img();
}

// l+r+b: bottom back to the console and back again
void gl_bottom_toggle(void)
{
    gl_bottom_on = !gl_bottom_on;
    if (!con || !gl_bottom_owned) return;
    if (gl_bottom_on) {
        con->frameBuffer = con_dummy;
        bot_show_img();
    } else {
        con->frameBuffer = bot_fb();
        printf("\x1b[2J");
    }
}

// screen space of the bound target, landscape: top 400x240, bottom target 640x480
static float tgt_w(void) { return cur_tgt ? 640 : 400; }
static float tgt_h(void) { return cur_tgt ? 480 : 240; }

static void bind_target(int t)
{
    if (cur_tgt == t) return;
    C3D_FrameDrawOn(t ? bot_target : target);
    cur_tgt = t;
    // FrameDrawOn resets viewport and scissor, and the fix matrix is per target
    g.fix_dirty = g.mvp_dirty = true;
    if (t) bot_drawn = true;
}

// ------------------------------------------------------------------ frame
// no SYNCDRAW: the game paces itself, all we need is the gpu queue drained.
// returns false if the gpu didn't finish within timeout_ms (0 = wait forever)
static bool try_begin(u32 timeout_ms)
{
    if (in_frame) return true;
    const char *was = (const char *)g_where;
    WHERE("gpu FrameBegin");
    u64 t0 = osGetTime();
    bool told = false;
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        u64 dt = osGetTime() - t0;
        if (!told && dt > 1000) {
            dbg("gpu: last frame still not done after 1s (vblank %lu/%lu)", C3D_FrameCounter(0), C3D_FrameCounter(1));
            told = true;
        }
        if (timeout_ms && dt > timeout_ms) { WHERE(was); return false; }
        svcSleepThread(500000);
    }
    cur_tgt = -1;
    bot_drawn = false;
    bind_target(route == ROUTE_BOT ? 1 : 0);
    fs_menu = fs_title = fs_world = false;
    WHERE(was);
    in_frame = true;
    frame_draws = 0;
    ring_cur ^= 1;
    ring_used = 0;
    g.state_dirty = g.mvp_dirty = g.tm_dirty = g.fix_dirty = true;
    if (gl_dump) {
        gl_dump = false;
        dumping = true;
        C3D_Mtx *pj = &g.st[1][g.sp[1]];
        dbg("dump: frame %lu probe %d (%s) wide %d", gl_swaps, gl_probe, probe_names[gl_probe], gl_wide);
        dbg("dump: proj z [%.4f %.4f %.4f %.4f] w [%.4f %.4f %.4f %.4f]", pj->r[2].x, pj->r[2].y, pj->r[2].z, pj->r[2].w,
            pj->r[3].x, pj->r[3].y, pj->r[3].z, pj->r[3].w);
        dbg("dump: fog mode %lx start %.2f end %.2f dens %.4f color %08lx lut n %.3f f %.1f", g.fogmode, g.fogs, g.foge, g.fogd,
            g.fogc, fog_n, fog_f);
        dbg("dump: vp %d %d %d %d sc %d %d %d %d", g.vp[0], g.vp[1], g.vp[2], g.vp[3], g.sc[0], g.sc[1], g.sc[2], g.sc[3]);
    }
    return true;
}

static void begin_frame(void) { try_begin(0); }

static void flush_ring(void)
{
    if (ring_used) GSPGPU_FlushDataCache(ring[ring_cur], ring_used);
}

// once a second on the bottom screen: fps, frame ms, gpu cmdbuf and ring use
static void perf_tick(void)
{
    static u64 t_last;
    static int frames;
    static u32 ring_peak;
    static float cmd_peak;
    frames++;
    if (ring_used > ring_peak) ring_peak = ring_used;
    float cu = C3D_GetCmdBufUsage();
    if (cu > cmd_peak) cmd_peak = cu;
    u64 now = osGetTime();
    if (!t_last) t_last = now;
    if (now - t_last >= 1000) {
        int bgm = midi_load(now - t_last);
        printf("\x1b[20;0Hfps %2d  %5.1f ms  cpu %5.1f ms\ncmd %3d%%  ring %4luk  bgm %3d%%  \n", frames,
               (now - t_last) / (float)frames, gl_busy_ticks / CPU_TICKS_PER_MSEC / frames,
               (int)(cmd_peak * 100), ring_peak >> 10, bgm);
        static int n;
        if (++n % 5 == 0)
            dbg("perf: fps %d  %.1f ms/frame  cpu %.1f ms  cmd %d%%  ring %luk  bgm %d%%", frames,
                (now - t_last) / (float)frames, gl_busy_ticks / CPU_TICKS_PER_MSEC / frames,
                (int)(cmd_peak * 100), ring_peak >> 10, bgm);
        gl_busy_ticks = 0;
        frames = 0;
        ring_peak = 0;
        cmd_peak = 0;
        t_last = now;
    }
}

static void texenv_update(tex_t *t);

// quads in screen pixels (4 xy corners each, drawn as strips) in one flat color. caller sets
// scissor and depth state, this sets the rest and leaves everything dirty for the game path
static void px_quads(const s16 *xy, int n, const float *rgba, bool prime)
{
    // the caller already touched depth/scissor state, the game path has to redo it either way
    g.state_dirty = g.mvp_dirty = g.fix_dirty = true;
    s16 *v = ring_alloc(n * 8 * sizeof *xy);
    if (!v) return;
    memcpy(v, xy, n * 8 * sizeof *xy);

    C3D_Mtx m;
    // screen px -> clip, then the same tilt the game path gets
    Mtx_Zeros(&m);
    mtx_set(&m, 0, 0, 2.0f / tgt_h(), 0, -1);
    mtx_set(&m, 1, -2.0f / tgt_w(), 0, 0, 1);
    mtx_set(&m, 2, 0, 0, 0, -0.5f);
    mtx_set(&m, 3, 0, 0, 0, 1);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &m);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_k, 0, 1, 0, 0);

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    AttrInfo_Init(ai);
    BufInfo_Init(bi);
    AttrInfo_AddLoader(ai, 0, GPU_SHORT, 2);
    AttrInfo_AddFixed(ai, 1);
    AttrInfo_AddFixed(ai, 2);
    BufInfo_Add(bi, v, 4, 1, 0x0);
    C3D_FixedAttribSet(1, 0, 0, 0, 1);
    C3D_FixedAttribSet(2, rgba[0], rgba[1], rgba[2], rgba[3]);

    texenv_update(NULL);
    hw_tex = (tex_t *)-1;
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_CullFace(GPU_CULL_NONE);
    if (prime) {
        // fog on, blended ZERO/ONE: runs the whole fragment path and leaves the framebuffer alone
        C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
        C3D_DepthMap(true, -1.0f, 0.0f);
    } else {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    }
    for (int i = 0; i < n; i++) C3D_DrawArrays(GPU_TRIANGLE_STRIP, i * 4, 4);
    hw_fog = prime;
    if (prime) lut_pending = false;
}

static void fog_prime(void)
{
    static const s16 corner[] = { 0, 0, 8, 0, 0, 8, 8, 8 };
    static const float none[4] = { 0, 0, 0, 0 };
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    px_quads(corner, 1, none, true);
    if (dumping) dbg("   fog prime");
}

// paint the 4:3 bars black before showing the frame
static void draw_bars(void)
{
    if (gl_wide || route == ROUTE_BOT) return;
    bind_target(0);
    // [0,40] and [360,400], full height
    static const s16 bars[] = { 0, 0, 40, 0, 0, 240, 40, 240, 360, 0, 400, 0, 360, 240, 400, 240 };
    static const float black[4] = { 0, 0, 0, 1 };
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    px_quads(bars, 2, black, false);
}

void gl_swap(void)
{
    if (gl_paused) return;
    gl_swaps++;
    perf_tick();
    if (!in_frame) begin_frame();
    if (dumping) { dumping = false; dbg("dump: end, %d draws", frame_draws); }
#ifdef SLOWSWAP
    // test hook: burn SLOWSWAP ms inside the game's frame, to match o3ds cpu time in an emulator
    { u64 e = svcGetSystemTick() + (u64)(SLOWSWAP * CPU_TICKS_PER_MSEC); while (svcGetSystemTick() < e); }
#endif
    draw_bars();
    // the inventory: its panels, nothing fogged, and gameplay seen recently. the mission select
    // and merchant share the panel art but come from the title, so they stay on the top
    static bool in_level;
    static int other_run;
    int cls = fs_title ? FC_TITLE : fs_world ? FC_GAME : fs_menu && in_level ? FC_MENU : FC_OTHER;
    if (cls == FC_GAME) in_level = true;
    if (cls == FC_TITLE || (cls == FC_OTHER && ++other_run > 45)) in_level = false;
    if (cls != FC_OTHER) other_run = 0;
    bool show_bot = gl_bottom_on && bot_drawn && (cls == FC_MENU || cls == FC_TITLE);
    // inventory open: the top keeps showing the last gameplay frame
    if (cls == FC_MENU && gl_bottom_on) target->used = false;
    flush_ring();
    const char *was = (const char *)g_where;
    WHERE("gpu FrameEnd");
    if (show_bot) {
        if (!gl_bottom_owned) bot_take();
        // the frame's commands first, then the 2:1 shrink onto the lcd, both through the gx queue
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        GX_DisplayTransfer((u32 *)bot_target->frameBuf.colorBuf, GX_BUFFER_DIM(BOT_W * 2, BOT_H * 2),
                           (u32 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), GX_BUFFER_DIM(BOT_W * 2, BOT_H * 2),
                           BOT_TRANSFER_FLAGS);
        if (cls == FC_MENU) inv_seen = true;
    }
    // GX_CMDLIST_FLUSH: flush just the command list. without it citro3d flushes the whole
    // 16MB linear heap every frame, and we already flush the ring and textures ourselves
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    WHERE(was);
    in_frame = false;

    // the bottom keeps its last image once a screen leaves. keep a copy for the console toggle,
    // and the inventory on sd as the placeholder for the next session
    if (gl_bottom_on && gl_bottom_owned && cls != last_class) {
        if (last_class == FC_MENU) bot_keep(true);
        else if (last_class == FC_TITLE) bot_keep(false);
    }
    static bool placeholder_done;
    if (cls == FC_GAME && !inv_seen && !placeholder_done) {
        placeholder_done = true;
        bot_placeholder();
    }
    last_class = cls;
    route = !gl_bottom_on ? ROUTE_TOP : cls == FC_MENU ? ROUTE_BOT : cls == FC_TITLE ? ROUTE_BOTH : ROUTE_TOP;
}

// game viewport (640x480, y up) -> top screen box, rotated for the portrait framebuffer
static void fix_update(void)
{
    float W = tgt_w(), H = tgt_h();
    float bw = cur_tgt ? 640 : gl_wide ? 400 : 320, bx = cur_tgt || gl_wide ? 0 : 40;
    float sx0 = bx + g.vp[0] * bw / ZW, sw = g.vp[2] * bw / ZW;
    float sy0 = g.vp[1] * H / ZH, sh = g.vp[3] * H / ZH;
    float ax = sw / W, bxo = (2 * sx0 + sw) / W - 1;
    float ay = sh / H, byo = (2 * sy0 + sh) / H - 1;
    // x'' = y', y'' = -x' (tilt), z' = -z/2 - w/2 so gl [-w,w] lands in pica [-w,0]
    Mtx_Zeros(&fix);
    mtx_set(&fix, 0, 0, ay, 0, byo);
    mtx_set(&fix, 1, -ax, 0, 0, -bxo);
    mtx_set(&fix, 2, 0, 0, -0.5f, -0.5f);
    mtx_set(&fix, 3, 0, 0, 0, 1);

    // scissor: game viewport intersected with glScissor, in framebuffer pixels
    float x0 = sx0, y0 = sy0, x1 = sx0 + sw, y1 = sy0 + sh;
    if (g.caps & CAP_SCISSOR) {
        float cx0 = bx + g.sc[0] * bw / ZW, cx1 = bx + (g.sc[0] + g.sc[2]) * bw / ZW;
        float cy0 = g.sc[1] * H / ZH, cy1 = (g.sc[1] + g.sc[3]) * H / ZH;
        if (cx0 > x0) x0 = cx0;
        if (cx1 < x1) x1 = cx1;
        if (cy0 > y0) y0 = cy0;
        if (cy1 < y1) y1 = cy1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    // fb is H wide (screen y), W tall (screen x, flipped)
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)y0, (u32)(W - x1), (u32)y1, (u32)(W - x0));
    g.mvp_dirty = true;
    g.fix_dirty = false;
}

// ------------------------------------------------------------------ state flush
static GPU_TESTFUNC testfunc(GLenum f)
{
    static const u8 t[8] = { GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS };
    return (f >= 0x200 && f <= 0x207) ? t[f - 0x200] : GPU_ALWAYS;
}

static GPU_BLENDFACTOR blendf(GLenum f)
{
    switch (f) {
    case 0: return GPU_ZERO;
    case 1: return GPU_ONE;
    case 0x300: return GPU_SRC_COLOR;
    case 0x301: return GPU_ONE_MINUS_SRC_COLOR;
    case 0x302: return GPU_SRC_ALPHA;
    case 0x303: return GPU_ONE_MINUS_SRC_ALPHA;
    case 0x304: return GPU_DST_ALPHA;
    case 0x305: return GPU_ONE_MINUS_DST_ALPHA;
    case 0x306: return GPU_DST_COLOR;
    case 0x307: return GPU_ONE_MINUS_DST_COLOR;
    case 0x308: return GPU_SRC_ALPHA_SATURATE;
    }
    return GPU_ONE;
}

static void fog_update(void)
{
    // pull near/far back out of the projection, gl style
    C3D_Mtx *p = &g.st[1][g.sp[1]];
    float A = p->r[2].z, B = p->r[2].w;
    float n = B / (A - 1), f = B / (A + 1);
    if (!(n > 0 && f > n)) { n = 1; f = 1000; }
    g.fog_dirty = false;
    // projection gets reloaded constantly, only rebuild when it really moved
    if (n == fog_n && f == fog_f && !g.fog_params) return;
    fog_n = n; fog_f = f; g.fog_params = false;
    float data[256];
    float prev = 0;
    for (int i = 0; i <= 128; i++) {
        float d = i / 128.0f;
        float z = f * n / (f - d * (f - n));
        float v;
        switch (g.fogmode) {
        case GL_EXP: v = expf(-g.fogd * z); break;
        case GL_EXP2: v = expf(-(g.fogd * z) * (g.fogd * z)); break;
        default: v = (g.foge - z) / (g.foge - g.fogs); break;
        }
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        if (i < 128) data[i] = v;
        if (i > 0) data[i + 127] = v - prev;
        prev = v;
    }
    FogLut_FromArray(&foglut, data);
    C3D_FogLutBind(&foglut);
    C3D_FogColor(g.fogc);
    lut_pending = true;
}

static void texenv_update(tex_t *t)
{
    C3D_TexEnv *e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    if (!t) {
        C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        return;
    }
    switch (g.env[0]) {
    case GL_REPLACE:
        C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
        // rgb only textures keep the fragment alpha
        C3D_TexEnvSrc(e, C3D_Alpha, t->alpha ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
        break;
    case GL_DECAL:
        C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
        C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(e, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
        break;
    case GL_ADD:
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD);
        C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
        break;
    case GL_BLEND_ENV:
        C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
        C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvColor(e, g.envc[0]);
        break;
    default:
        if (tex_only) {
            // rgb only textures keep the fragment alpha, which is ~1 here anyway
            C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(e, C3D_Alpha, t->alpha ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
            break;
        }
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
        break;
    }
    if (gl_probe == PROBE_AT_TEXALPHA && (g.caps & CAP_ALPHA)) {
        C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
    }
}

static bool want_fog(void)
{
    if (!(g.caps & CAP_FOG) || gl_probe == PROBE_NOFOG) return false;
    return !(gl_probe == PROBE_AT_NOFOG && (g.caps & CAP_ALPHA));
}

static void state_update(void)
{
    if (g.caps & CAP_BLEND)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blendf(g.bs), blendf(g.bd), blendf(g.bs), blendf(g.bd));
    else
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);

#ifdef NOALPHA
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
#else
    C3D_AlphaTest(g.caps & CAP_ALPHA, testfunc(g.af), (int)(g.aref * 255));
#endif

    bool dt = g.caps & CAP_DEPTH;
    C3D_DepthTest(true, dt ? testfunc(g.df) : GPU_ALWAYS, g.cmask | (dt && g.dmask ? GPU_WRITE_DEPTH : 0));

    GPU_CULLMODE cm = GPU_CULL_NONE;
    if (g.caps & CAP_CULL) {
        bool back = g.cull == GL_BACK;
        if (g.front == GL_CW) back = !back;
        cm = back ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW;
    }
    C3D_CullFace(cm);

    C3D_FogGasMode(g.caps & CAP_FOG ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_DepthMap(true, -1.0f, g.caps & CAP_POFF ? g.poff : 0.0f);

    bool at = g.caps & CAP_ALPHA;
    if (gl_probe == PROBE_NOFOG || (gl_probe == PROBE_AT_NOFOG && at))
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    hw_fog = want_fog();
    if (gl_probe == PROBE_AT_NOTEST && at) C3D_AlphaTest(false, GPU_ALWAYS, 0);
    if (gl_probe == PROBE_AT_DEPTH && at) C3D_DepthTest(true, GPU_ALWAYS, g.cmask);
    g.state_dirty = false;
}

// ------------------------------------------------------------------ draw
static int tsize(GLenum t) { return (t == GL_BYTE || t == GL_UNSIGNED_BYTE) ? 1 : (t == GL_SHORT || t == GL_UNSIGNED_SHORT) ? 2 : 4; }

static const u8 *copy_arr(const arr_t *a, int lo, int n, int *stride)
{
    int es = a->size * tsize(a->type);
    int st = a->stride ? a->stride : es;
    u32 len = (n - 1) * st + es;
    u8 *d = ring_alloc(len);
    if (!d) return NULL;
    memcpy(d, a->ptr + lo * st, len);
    *stride = st;
    return d;
}

// GL_FIXED has no pica format, turn it into packed floats while it goes into the ring
static const u8 *copy_fixed(const arr_t *a, int lo, int n, int comps, int *stride)
{
    int st = a->stride ? a->stride : a->size * 4;
    float *d = ring_alloc(n * comps * 4);
    if (!d) return NULL;
    const u8 *s = a->ptr + lo * st;
    for (int i = 0; i < n; i++, s += st) {
        const s32 *v = (const s32 *)s;
        for (int c = 0; c < comps; c++) d[i * comps + c] = v[c] * (1.0f / 65536.0f);
    }
    *stride = comps * 4;
    return (const u8 *)d;
}

// fixed positions go to eye space here, in f32. the game hands world coordinates (hundreds to
// thousands of units) and the pica loads attributes as f24 and runs dp4 in f24: the cm-scale gap
// between layered meshes (leon's hair over his scalp) drowns in rounding and the scalp wins
static const u8 *copy_fixed_eye(const arr_t *a, int lo, int n, const C3D_Mtx *mv, int *stride)
{
    int st = a->stride ? a->stride : a->size * 4;
    float *out = ring_alloc(n * 12);
    if (!out) return NULL;
    // w_eye is the constant r[3].w here, scale it out so the result is the same projective point
    C3D_Mtx m = *mv;
    float iw = 1.0f / m.r[3].w;
    for (int k = 0; k < 3; k++) { m.r[k].x *= iw; m.r[k].y *= iw; m.r[k].z *= iw; m.r[k].w *= iw; }
    const C3D_FVec *r = m.r;
    const u8 *s = a->ptr + lo * st;
    bool has_z = a->size > 2;
    float *d = out;
    for (int i = 0; i < n; i++, s += st, d += 3) {
        const s32 *v = (const s32 *)s;
        float x = v[0] * (1.0f / 65536.0f), y = v[1] * (1.0f / 65536.0f);
        float z = has_z ? v[2] * (1.0f / 65536.0f) : 0;
        d[0] = r[0].x * x + r[0].y * y + r[0].z * z + r[0].w;
        d[1] = r[1].x * x + r[1].y * y + r[1].z * z + r[1].w;
        d[2] = r[2].x * x + r[2].y * y + r[2].z * z + r[2].w;
    }
    if (dumping) {
        dump_z0 = 1e9f; dump_z1 = -1e9f;
        for (int i = 0; i < n; i++) {
            float z = out[i * 3 + 2];
            if (z < dump_z0) dump_z0 = z;
            if (z > dump_z1) dump_z1 = z;
        }
    }
    *stride = 12;
    return (const u8 *)out;
}

static GPU_FORMATS pfmt(GLenum t)
{
    switch (t) {
    case GL_BYTE: return GPU_BYTE;
    case GL_UNSIGNED_BYTE: return GPU_UNSIGNED_BYTE;
    case GL_FLOAT: return GPU_FLOAT;
    default: return GPU_SHORT;
    }
}

// texcoord range after the texture matrix, over the vertices this draw uses
static void uv_range(int lo, int n, float r[4])
{
    const arr_t *a = &g.ta[0];
    int es = a->size * tsize(a->type), st = a->stride ? a->stride : es;
    const C3D_Mtx *m = &g.st[2][g.sp[2]];
    r[0] = r[2] = 1e9f; r[1] = r[3] = -1e9f;
    for (int i = 0; i < n; i++) {
        const u8 *p = a->ptr + (lo + i) * st;
        float s0, t0;
        switch (a->type) {
        case GL_FIXED: s0 = fx(((const s32 *)p)[0]); t0 = fx(((const s32 *)p)[1]); break;
        case GL_FLOAT: s0 = ((const float *)p)[0]; t0 = ((const float *)p)[1]; break;
        case GL_SHORT: s0 = ((const s16 *)p)[0]; t0 = ((const s16 *)p)[1]; break;
        case GL_BYTE: s0 = ((const s8 *)p)[0]; t0 = ((const s8 *)p)[1]; break;
        default: s0 = p[0]; t0 = p[1]; break;
        }
        float u = m->r[0].x * s0 + m->r[0].y * t0 + m->r[0].w, v = m->r[1].x * s0 + m->r[1].y * t0 + m->r[1].w;
        if (u < r[0]) r[0] = u;
        if (u > r[1]) r[1] = u;
        if (v < r[2]) r[2] = v;
        if (v > r[3]) r[3] = v;
    }
}

static float fog_factor(float c)
{
    float f = g.fogmode == GL_EXP ? expf(-g.fogd * c) : g.fogmode == GL_EXP2 ? expf(-(g.fogd * c) * (g.fogd * c))
              : (g.foge - c) / (g.foge - g.fogs);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void dump_draw(GLenum mode, int count, int lo, int n, tex_t *t, bool eye, float cscale)
{
    float uv[4] = { 0, 0, 0, 0 };
    bool tc = t && (g.client & CL_TEX0) && g.ta[0].ptr;
    if (tc) uv_range(lo, n, uv);
    dbg("d%d m%lx n%d lo%d nv%d va %d/%lx/%d eye%d col%d tc %d/%lx uv %.3f..%.3f %.3f..%.3f", frame_draws, mode, count, lo, n,
        g.va.size, g.va.type, g.va.stride, eye, (g.client & CL_COLOR) && g.ca.ptr, tc, g.ta[0].type, uv[0], uv[1], uv[2], uv[3]);
    dbg("   tex %lu %08lx %dx%d f%d a%d filt %lx/%lx wrap %lx/%lx env %lx%s caps %02lx af %lx/%d df %lx dm%d cm%x cull %lx/%lx bl %lx/%lx",
        g.tex2d[0] ? g.bound[0] : 0, t ? t->sig : 0, t ? t->t.width : 0, t ? t->t.height : 0, t ? t->t.fmt : -1,
        t ? t->alpha : 0, t ? t->minf : 0, t ? t->magf : 0, t ? t->ws : 0, t ? t->wt : 0, g.env[0], tex_only ? "/tex" : "", g.caps, g.af,
        (int)(g.aref * 255), g.df, g.dmask, g.cmask, g.cull, g.front, g.bs, g.bd);
    if (eye)
        dbg("   color %.2f %.2f %.2f %.2f k %.4f eye z %.2f..%.2f gl fog %.2f..%.2f", g.color[0], g.color[1], g.color[2],
            g.color[3], cscale, dump_z0, dump_z1, fog_factor(-dump_z1), fog_factor(-dump_z0));
    else
        dbg("   color %.2f %.2f %.2f %.2f k %.4f", g.color[0], g.color[1], g.color[2], g.color[3], cscale);
}

static void upload_mvp(bool eye, const C3D_Mtx *mv)
{
    // eye space positions only need the projection
    C3D_Mtx pm, mvp;
    Mtx_Multiply(&pm, &fix, &g.st[1][g.sp[1]]);
    if (eye) mvp = pm;
    else Mtx_Multiply(&mvp, &pm, mv);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &mvp);
    g.mvp_dirty = false;
    hw_eye = eye;
}

static void draw(GLenum mode, int first, int count, const void *idx, GLenum itype)
{
    static bool warned_prim;
    if (gl_paused || count <= 0 || !(g.client & CL_VERTEX) || !g.va.ptr) return;
    GPU_Primitive_t prim;
    switch (mode) {
    case GL_TRIANGLES: prim = GPU_TRIANGLES; break;
    case GL_TRIANGLE_STRIP: prim = GPU_TRIANGLE_STRIP; break;
    case GL_TRIANGLE_FAN: prim = GPU_TRIANGLE_FAN; break;
    default:
        if (!warned_prim) { dbg("gl: prim %lx skipped", mode); warned_prim = true; }
        return;
    }
    if ((g.caps & CAP_CULL) && g.cull == GL_FRONT_AND_BACK) return;

    begin_frame();
    if (want_fog()) {
        if (g.fog_dirty) fog_update();
        if ((!hw_fog || lut_pending) && gl_probe != PROBE_NOPRIME) fog_prime();
    }

    int lo = first, hi = first + count - 1;
    if (idx) {
        lo = 0xffff; hi = 0;
        if (itype == GL_UNSIGNED_SHORT) {
            const u16 *s = idx;
            for (int i = 0; i < count; i++) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        } else {
            const u8 *s = idx;
            for (int i = 0; i < count; i++) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        }
    }
    int n = hi - lo + 1;

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    AttrInfo_Init(ai);
    BufInfo_Init(bi);

    int st;
    const u8 *p;
    const C3D_Mtx *mv = &g.st[0][g.sp[0]];
    // the game's fixed matrices carry 0xffff in m[15], not 1.0: affine within rounding is enough
    bool eye = g.va.type == GL_FIXED && !mv->r[3].x && !mv->r[3].y && !mv->r[3].z && fabsf(mv->r[3].w - 1) < 1e-3f;
    if (eye) {
        p = copy_fixed_eye(&g.va, lo, n, mv, &st);
        if (!p) goto full;
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    } else if (g.va.type == GL_FIXED) {
        int comps = g.va.size > 3 ? 3 : g.va.size;
        p = copy_fixed(&g.va, lo, n, comps, &st);
        if (!p) goto full;
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, comps);
    } else {
        p = copy_arr(&g.va, lo, n, &st);
        if (!p) goto full;
        AttrInfo_AddLoader(ai, 0, pfmt(g.va.type), g.va.size);
    }
    BufInfo_Add(bi, p, st, 1, 0x0);

    tex_t *t = NULL;
    if (g.tex2d[0] && g.bound[0] < MAXTEX && texs[g.bound[0]] && texs[g.bound[0]]->ok)
        t = texs[g.bound[0]];
    if (t && (g.client & CL_TEX0) && g.ta[0].ptr) {
        if (g.ta[0].type == GL_FIXED) {
            p = copy_fixed(&g.ta[0], lo, n, 2, &st);
            if (!p) goto full;
            AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
        } else {
            p = copy_arr(&g.ta[0], lo, n, &st);
            if (!p) goto full;
            AttrInfo_AddLoader(ai, 1, pfmt(g.ta[0].type), 2);
        }
        BufInfo_Add(bi, p, st, 1, 0x1);
    } else {
        AttrInfo_AddFixed(ai, 1);
        C3D_FixedAttribSet(1, 0, 0, 0, 1);
    }

    // no color array: the current color goes in as a constant attribute
    float cscale = 1.0f;
    if ((g.client & CL_COLOR) && g.ca.ptr) {
        if (g.ca.type == GL_FIXED) {
            p = copy_fixed(&g.ca, lo, n, 4, &st);
            if (!p) goto full;
            AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4);
        } else {
            p = copy_arr(&g.ca, lo, n, &st);
            if (!p) goto full;
            if (g.ca.type == GL_UNSIGNED_BYTE) cscale = 1.0f / 255;
            AttrInfo_AddLoader(ai, 2, pfmt(g.ca.type), g.ca.size);
        }
        BufInfo_Add(bi, p, st, 1, 0x2);
    } else {
        AttrInfo_AddFixed(ai, 2);
        C3D_FixedAttribSet(2, g.color[0], g.color[1], g.color[2], g.color[3]);
    }

    const void *ib = NULL;
    if (idx) {
        u16 *d = ring_alloc(count * 2);
        if (!d) goto full;
        if (itype == GL_UNSIGNED_SHORT) {
            const u16 *s = idx;
            for (int i = 0; i < count; i++) d[i] = s[i] - lo;
        } else {
            const u8 *s = idx;
            for (int i = 0; i < count; i++) d[i] = s[i] - lo;
        }
        ib = d;
    }

    if (gl_probe) { g.state_dirty = true; hw_tex = (tex_t *)-1; }
    if (g.fix_dirty) fix_update();
    if (g.state_dirty) state_update();
    if ((g.caps & CAP_FOG) && g.fog_dirty) fog_update();
    if (hw_fog) lut_pending = false;

    // GL_MODULATE with no color array and a white current color is just the texture. leon's hair
    // (alpha tested, constant color, fog) is the one draw that goes missing on hw only; this takes
    // the constant color attribute out of its fragment path without changing what gl would draw
    tex_only = !((g.client & CL_COLOR) && g.ca.ptr) && g.color[0] > 0.999f && g.color[1] > 0.999f &&
               g.color[2] > 0.999f && g.color[3] > 0.999f;
    if (t != hw_tex || g.env[0] != hw_env || (t && t->alpha != hw_alpha) || tex_only != hw_white) {
        // untextured draws drop the unit too, otherwise citro3d keeps the last texture's
        // address around and re-emits it later, even after the game deleted that texture
        C3D_TexBind(0, t ? &t->t : NULL);
        texenv_update(t);
        hw_tex = t;
        hw_env = g.env[0];
        hw_alpha = t ? t->alpha : false;
        hw_white = tex_only;
    }

    if (g.mvp_dirty || eye != hw_eye) upload_mvp(eye, mv);
    if (g.tm_dirty) {
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_tmat, &g.st[2][g.sp[2]]);
        g.tm_dirty = false;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_k, 0, cscale, 0, 0);

    if (dumping) dump_draw(mode, count, lo, n, t, eye, cscale);
    if (ib) C3D_DrawElements(prim, count, C3D_UNSIGNED_SHORT, ib);
    else C3D_DrawArrays(prim, 0, count);
    frame_draws++;

    if (g.caps & CAP_FOG) fs_world = true;
    if (t && sig_menu(t->sig)) fs_menu = true;
    if (t && t->sig == SIG_TITLE) fs_title = true;
    // title: the art again on the bottom, at full strength. the menu text (font atlas) stays on
    // the top, and so do the fades: the title darkens its own draws on the way out, and the
    // bottom has to keep the clean art afterwards
    if (route == ROUTE_BOTH && t && t->sig == SIG_TITLE) {
        bind_target(1);
        fix_update();
        upload_mvp(eye, mv);
        C3D_TexEnv *e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        if (ib) C3D_DrawElements(prim, count, C3D_UNSIGNED_SHORT, ib);
        else C3D_DrawArrays(prim, 0, count);
        hw_tex = (tex_t *)-1;
        g.state_dirty = true;
        bind_target(0);
    }
    return;

full:
    {
        static bool warned;
        if (!warned) { dbg("gl: ring full, dropping draws"); warned = true; }
    }
}

void gl_DrawArrays(GLenum mode, GLint first, GLsizei count) { draw(mode, first, count, NULL, 0); }
void gl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) { draw(mode, 0, count, idx, type); }

// ------------------------------------------------------------------ simple state
void gl_ActiveTexture(GLenum u) { g.unit = (u - 0x84C0) & 1; }
void gl_ClientActiveTexture(GLenum u) { g.cunit = (u - 0x84C0) & 1; }
void gl_AlphaFuncx(GLenum f, GLfixed r) { g.af = f; g.aref = fx(r); g.state_dirty = true; }
void gl_BlendFunc(GLenum s, GLenum d) { g.bs = s; g.bd = d; g.state_dirty = true; }
void gl_DepthFunc(GLenum f) { g.df = f; g.state_dirty = true; }
void gl_DepthMask(GLboolean m) { g.dmask = m; g.state_dirty = true; }
void gl_CullFace(GLenum m) { g.cull = m; g.state_dirty = true; }
void gl_FrontFace(GLenum m) { g.front = m; g.state_dirty = true; }
void gl_Hint(GLenum t, GLenum m) {}
void gl_ShadeModel(GLenum m) {}
void gl_LightModelx(GLenum p, GLfixed v) {}
void gl_PixelStorei(GLenum p, GLint v) {}
void gl_Finish(void) {}
void gl_Flush(void) {}
GLenum gl_GetError(void) { GLenum e = g.err; g.err = 0; return e; }

void gl_ColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a)
{
    g.cmask = (r ? GPU_WRITE_RED : 0) | (gg ? GPU_WRITE_GREEN : 0) | (b ? GPU_WRITE_BLUE : 0) | (a ? GPU_WRITE_ALPHA : 0);
    g.state_dirty = true;
}

void gl_PolygonOffsetx(GLfixed f, GLfixed u)
{
    // pica only has a constant depth bias. units in 24 bit steps, scaled up a bit
    g.poff = fx(u) * (1.0f / (1 << 20));
    g.state_dirty = true;
}

void gl_Color4x(GLfixed r, GLfixed gg, GLfixed b, GLfixed a)
{
    g.color[0] = fx(r); g.color[1] = fx(gg); g.color[2] = fx(b); g.color[3] = fx(a);
}

void gl_Color4ub(GLubyte r, GLubyte gg, GLubyte b, GLubyte a)
{
    g.color[0] = r / 255.0f; g.color[1] = gg / 255.0f; g.color[2] = b / 255.0f; g.color[3] = a / 255.0f;
}

static u8 c8(GLfixed v) { int i = v >> 8; return i < 0 ? 0 : i > 255 ? 255 : i; }

void gl_ClearColorx(GLfixed r, GLfixed gg, GLfixed b, GLfixed a)
{
    g.clearc[0] = c8(r) / 255.0f; g.clearc[1] = c8(gg) / 255.0f;
    g.clearc[2] = c8(b) / 255.0f; g.clearc[3] = c8(a) / 255.0f;
}

void gl_ClearDepthx(GLfixed d) { g.cleard = fx(d); }

// a quad in the command list, not a gx memory fill. the fill runs outside the command list and
// on hw the first draw after it (leon's hair) was sporadically wiped by it. as a draw it also
// gets gl's rules: scissor and color/depth write masks apply, the viewport doesn't
static void clear_bound(u32 wm)
{
    float W = tgt_w(), H = tgt_h();
    if (g.caps & CAP_SCISSOR) {
        float bw = cur_tgt ? 640 : gl_wide ? 400 : 320, bx = cur_tgt || gl_wide ? 0 : 40;
        float x0 = bx + g.sc[0] * bw / ZW, x1 = bx + (g.sc[0] + g.sc[2]) * bw / ZW;
        float y0 = g.sc[1] * H / ZH, y1 = (g.sc[1] + g.sc[3]) * H / ZH;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)y0, (u32)(W - x1), (u32)y1, (u32)(W - x0));
    } else {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    }
    float d = g.cleard < 0 ? 0 : g.cleard > 1 ? 1 : g.cleard;
    // depth test always passes, every fragment lands at the clear depth
    C3D_DepthTest(true, GPU_ALWAYS, wm);
    C3D_DepthMap(true, 0.0f, d);
    s16 full[] = { 0, 0, (s16)W, 0, 0, (s16)H, (s16)W, (s16)H };
    px_quads(full, 1, g.clearc, false);
}

// a quad in the command list, not a gx memory fill. the fill runs outside the command list and
// on hw the first draw after it (leon's hair) was sporadically wiped by it. as a draw it also
// gets gl's rules: scissor and color/depth write masks apply, the viewport doesn't
void gl_Clear(GLbitfield m)
{
    if (gl_paused) return;
    u32 wm = (m & 0x4000 ? g.cmask : 0) | (m & 0x100 && g.dmask ? GPU_WRITE_DEPTH : 0);
    if (!wm) return;
    begin_frame();
    clear_bound(wm);
    if (route == ROUTE_BOTH) {
        bind_target(1);
        clear_bound(wm);
        bind_target(0);
    }
}

static u32 cap_bit(GLenum c)
{
    switch (c) {
    case GL_BLEND: return CAP_BLEND;
    case GL_ALPHA_TEST: return CAP_ALPHA;
    case GL_DEPTH_TEST: return CAP_DEPTH;
    case GL_CULL_FACE: return CAP_CULL;
    case GL_FOG: return CAP_FOG;
    case GL_SCISSOR_TEST: return CAP_SCISSOR;
    case GL_POLYGON_OFFSET_FILL: return CAP_POFF;
    }
    return 0;
}

void gl_Enable(GLenum c)
{
    if (c == GL_TEXTURE_2D) { g.tex2d[g.unit] = true; return; }
    u32 b = cap_bit(c);
    if (!b || (g.caps & b)) return;
    g.caps |= b;
    g.state_dirty = true;
    if (b == CAP_SCISSOR) g.fix_dirty = true;
    if (b == CAP_FOG) g.fog_dirty = true;
}

void gl_Disable(GLenum c)
{
    if (c == GL_TEXTURE_2D) { g.tex2d[g.unit] = false; return; }
    u32 b = cap_bit(c);
    if (!b || !(g.caps & b)) return;
    g.caps &= ~b;
    g.state_dirty = true;
    if (b == CAP_SCISSOR) g.fix_dirty = true;
}

GLboolean gl_IsEnabled(GLenum c)
{
    if (c == GL_TEXTURE_2D) return g.tex2d[g.unit];
    return (g.caps & cap_bit(c)) != 0;
}

static u32 cl_bit(GLenum a)
{
    switch (a) {
    case GL_VERTEX_ARRAY: return CL_VERTEX;
    case GL_COLOR_ARRAY: return CL_COLOR;
    case GL_NORMAL_ARRAY: return CL_NORMAL;
    case GL_TEXTURE_COORD_ARRAY: return g.cunit ? CL_TEX1 : CL_TEX0;
    }
    return 0;
}

void gl_EnableClientState(GLenum a) { g.client |= cl_bit(a); }
void gl_DisableClientState(GLenum a) { g.client &= ~cl_bit(a); }

void gl_VertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { g.va = (arr_t){ s, t, st, p }; }
void gl_ColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { g.ca = (arr_t){ s, t, st, p }; }
void gl_NormalPointer(GLenum t, GLsizei st, const void *p) { g.na = (arr_t){ 3, t, st, p }; }
void gl_TexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) { g.ta[g.cunit] = (arr_t){ s, t, st, p }; }

void gl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    g.vp[0] = x; g.vp[1] = y; g.vp[2] = w; g.vp[3] = h;
    g.fix_dirty = true;
}

void gl_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    g.sc[0] = x; g.sc[1] = y; g.sc[2] = w; g.sc[3] = h;
    g.fix_dirty = true;
}

// the game sets GL_FOG_MODE every frame. only a real change rebuilds the lut, every upload costs
// a fog_prime and the upload itself
void gl_Fogx(GLenum p, GLfixed v)
{
    GLenum mode = g.fogmode;
    float d = g.fogd, st = g.fogs, en = g.foge;
    switch (p) {
    case GL_FOG_MODE: mode = v; break;
    case GL_FOG_DENSITY: d = fx(v); break;
    case GL_FOG_START: st = fx(v); break;
    case GL_FOG_END: en = fx(v); break;
    default: return;
    }
    if (mode == g.fogmode && d == g.fogd && st == g.fogs && en == g.foge) return;
    g.fogmode = mode; g.fogd = d; g.fogs = st; g.foge = en;
    g.fog_dirty = g.fog_params = true;
}

void gl_Fogxv(GLenum p, const GLfixed *v)
{
    if (p != GL_FOG_COLOR) { gl_Fogx(p, v[0]); return; }
    // fog color register is abgr
    u32 c = c8(v[3]) << 24 | c8(v[2]) << 16 | c8(v[1]) << 8 | c8(v[0]);
    if (c == g.fogc) return;
    g.fogc = c;
    g.fog_dirty = g.fog_params = true;
}

void gl_TexEnvx(GLenum t, GLenum p, GLfixed v)
{
    if (p == GL_TEXTURE_ENV_MODE) g.env[g.unit] = v;
}

void gl_TexEnvxv(GLenum t, GLenum p, const GLfixed *v)
{
    if (p == GL_TEXTURE_ENV_COLOR)
        g.envc[g.unit] = c8(v[3]) << 24 | c8(v[2]) << 16 | c8(v[1]) << 8 | c8(v[0]);
    else
        gl_TexEnvx(t, p, v[0]);
}

void gl_GetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case 0x0D33: *v = 1024; break;   // max texture size
    case 0x84E2: *v = 2; break;      // texture units
    case 0x0D32: *v = 24; break;     // modelview stack depth
    case 0x0D38: *v = 4; break;      // projection stack depth
    case 0x86A2: *v = 0; break;      // compressed format count
    case 0x0D56: *v = 16; break;     // depth bits
    default: *v = 0; break;
    }
}

const GLubyte *gl_GetString(GLenum n)
{
    switch (n) {
    case 0x1F00: return (const GLubyte *)"Qualcomm";
    case 0x1F01: return (const GLubyte *)"Adreno";
    case 0x1F02: return (const GLubyte *)"OpenGL ES-CM 1.0";
    }
    return (const GLubyte *)"";
}

// ------------------------------------------------------------------ matrices
void gl_MatrixMode(GLenum m) { g.mode = m == GL_PROJECTION ? 1 : m == GL_TEXTURE ? 2 : 0; }
void gl_LoadIdentity(void) { Mtx_Identity(cur()); touch_mtx(); }

void gl_LoadMatrixx(const GLfixed *m)
{
    C3D_Mtx *d = cur();
    for (int r = 0; r < 4; r++) mtx_set(d, r, fx(m[r]), fx(m[4 + r]), fx(m[8 + r]), fx(m[12 + r]));
    touch_mtx();
}

void gl_LoadMatrixf(const GLfloat *m)
{
    C3D_Mtx *d = cur();
    for (int r = 0; r < 4; r++) mtx_set(d, r, m[r], m[4 + r], m[8 + r], m[12 + r]);
    touch_mtx();
}

static void mul_cur(const C3D_Mtx *b)
{
    C3D_Mtx t;
    Mtx_Multiply(&t, cur(), b);
    *cur() = t;
    touch_mtx();
}

void gl_MultMatrixx(const GLfixed *m)
{
    C3D_Mtx b;
    for (int r = 0; r < 4; r++) mtx_set(&b, r, fx(m[r]), fx(m[4 + r]), fx(m[8 + r]), fx(m[12 + r]));
    mul_cur(&b);
}

void gl_MultMatrixf(const GLfloat *m)
{
    C3D_Mtx b;
    for (int r = 0; r < 4; r++) mtx_set(&b, r, m[r], m[4 + r], m[8 + r], m[12 + r]);
    mul_cur(&b);
}

void gl_PushMatrix(void)
{
    int *sp = &g.sp[g.mode];
    if (*sp == 23) { g.err = 0x0503; return; }
    g.st[g.mode][*sp + 1] = g.st[g.mode][*sp];
    (*sp)++;
}

void gl_PopMatrix(void)
{
    if (!g.sp[g.mode]) { g.err = 0x0504; return; }
    g.sp[g.mode]--;
    touch_mtx();
}

void gl_Translatex(GLfixed x, GLfixed y, GLfixed z)
{
    C3D_Mtx b;
    Mtx_Identity(&b);
    b.r[0].w = fx(x); b.r[1].w = fx(y); b.r[2].w = fx(z);
    mul_cur(&b);
}

void gl_Scalex(GLfixed x, GLfixed y, GLfixed z)
{
    C3D_Mtx b;
    Mtx_Identity(&b);
    b.r[0].x = fx(x); b.r[1].y = fx(y); b.r[2].z = fx(z);
    mul_cur(&b);
}

void gl_Rotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z)
{
    float ang = fx(a) * (float)M_PI / 180, X = fx(x), Y = fx(y), Z = fx(z);
    float l = sqrtf(X * X + Y * Y + Z * Z);
    if (l == 0) return;
    X /= l; Y /= l; Z /= l;
    float c = cosf(ang), s = sinf(ang), k = 1 - c;
    C3D_Mtx b;
    Mtx_Zeros(&b);
    mtx_set(&b, 0, X * X * k + c, X * Y * k - Z * s, X * Z * k + Y * s, 0);
    mtx_set(&b, 1, Y * X * k + Z * s, Y * Y * k + c, Y * Z * k - X * s, 0);
    mtx_set(&b, 2, X * Z * k - Y * s, Y * Z * k + X * s, Z * Z * k + c, 0);
    mtx_set(&b, 3, 0, 0, 0, 1);
    mul_cur(&b);
}

void gl_Orthox(GLfixed l_, GLfixed r_, GLfixed b_, GLfixed t_, GLfixed n_, GLfixed f_)
{
    float l = fx(l_), r = fx(r_), b = fx(b_), t = fx(t_), n = fx(n_), f = fx(f_);
    C3D_Mtx m;
    Mtx_Zeros(&m);
    mtx_set(&m, 0, 2 / (r - l), 0, 0, -(r + l) / (r - l));
    mtx_set(&m, 1, 0, 2 / (t - b), 0, -(t + b) / (t - b));
    mtx_set(&m, 2, 0, 0, -2 / (f - n), -(f + n) / (f - n));
    mtx_set(&m, 3, 0, 0, 0, 1);
    mul_cur(&m);
}

void gl_Frustumx(GLfixed l_, GLfixed r_, GLfixed b_, GLfixed t_, GLfixed n_, GLfixed f_)
{
    float l = fx(l_), r = fx(r_), b = fx(b_), t = fx(t_), n = fx(n_), f = fx(f_);
    C3D_Mtx m;
    Mtx_Zeros(&m);
    mtx_set(&m, 0, 2 * n / (r - l), 0, (r + l) / (r - l), 0);
    mtx_set(&m, 1, 0, 2 * n / (t - b), (t + b) / (t - b), 0);
    mtx_set(&m, 2, 0, 0, -(f + n) / (f - n), -2 * f * n / (f - n));
    mtx_set(&m, 3, 0, 0, -1, 0);
    mul_cur(&m);
    g.fog_dirty = true;
}

// ------------------------------------------------------------------ textures
void gl_GenTextures(GLsizei n, GLuint *names)
{
    for (int i = 0; i < n; i++) {
        // reuse freed slots so long sessions dont run out
        GLuint id = 0;
        for (GLuint k = next_name; k < MAXTEX; k++) if (!texs[k]) { id = k; break; }
        if (!id) for (GLuint k = 1; k < next_name; k++) if (!texs[k]) { id = k; break; }
        if (!id) { g.err = 0x0505; names[i] = 0; continue; }
        texs[id] = calloc(1, sizeof(tex_t));
        texs[id]->minf = texs[id]->magf = GL_LINEAR;
        texs[id]->ws = texs[id]->wt = GL_REPEAT;
        next_name = id + 1;
        names[i] = id;
    }
}

void gl_DeleteTextures(GLsizei n, const GLuint *names)
{
    for (int i = 0; i < n; i++) {
        GLuint id = names[i];
        if (!id || id >= MAXTEX || !texs[id]) continue;
        if (texs[id] == hw_tex) C3D_TexBind(0, NULL);
        if (texs[id]->ok) C3D_TexDelete(&texs[id]->t);
        free(texs[id]);
        texs[id] = NULL;
        hw_tex = (tex_t *)-1;
        for (int u = 0; u < 2; u++) if (g.bound[u] == id) g.bound[u] = 0;
    }
}

GLboolean gl_IsTexture(GLuint t) { return t && t < MAXTEX && texs[t]; }

static tex_t *bound_tex(void)
{
    GLuint id = g.bound[g.unit];
    if (!id || id >= MAXTEX) return NULL;
    if (!texs[id]) {
        texs[id] = calloc(1, sizeof(tex_t));
        texs[id]->minf = texs[id]->magf = GL_LINEAR;
        texs[id]->ws = texs[id]->wt = GL_REPEAT;
    }
    return texs[id];
}

void gl_BindTexture(GLenum t, GLuint name) { g.bound[g.unit] = name; }

static void tex_params(tex_t *t)
{
    hw_tex = (tex_t *)-1;
    if (!t->ok) return;
    C3D_TexSetFilter(&t->t, t->magf == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR,
                     (t->minf == GL_NEAREST || t->minf == 0x2700 || t->minf == 0x2702) ? GPU_NEAREST : GPU_LINEAR);
    C3D_TexSetWrap(&t->t, t->ws == GL_REPEAT ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                   t->wt == GL_REPEAT ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
}

void gl_TexParameterx(GLenum tg, GLenum p, GLfixed v)
{
    tex_t *t = bound_tex();
    if (!t) return;
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: if (t->minf == (GLenum)v) return; t->minf = v; break;
    case GL_TEXTURE_MAG_FILTER: if (t->magf == (GLenum)v) return; t->magf = v; break;
    case GL_TEXTURE_WRAP_S: if (t->ws == (GLenum)v) return; t->ws = v; break;
    case GL_TEXTURE_WRAP_T: if (t->wt == (GLenum)v) return; t->wt = v; break;
    default: return;
    }
    tex_params(t);
}

static int pot8(int v)
{
    int p = 8;
    while (p < v && p < 1024) p <<= 1;
    return p;
}

static bool tex_alloc(tex_t *t, int w, int h, int fmt)
{
    if (t->ok) {
        if (t->t.width == w && t->t.height == h && t->t.fmt == fmt) return true;
        C3D_TexDelete(&t->t);
        t->ok = false;
    }
    if (!C3D_TexInit(&t->t, w, h, fmt)) { dbg("gl: tex alloc %dx%d fmt %d failed", w, h, fmt); return false; }
    t->ok = true;
    return true;
}

static u32 fnv(const void *d, u32 n)
{
    const u8 *p = d;
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void tex_done(tex_t *t)
{
    GSPGPU_FlushDataCache(t->t.data, t->t.size);
    tex_params(t);
}

void gl_CompressedTexImage2D(GLenum tg, GLint lvl, GLenum fmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d)
{
    tex_t *t = bound_tex();
    // mips are ignored for now, level 0 is all the game needs to look right
    if (!t || lvl) return;
    int tw = pot8(w), th = pot8(h);
    t->sig = fnv(d, sz);
    if (fmt == 0x8C92 || fmt == 0x8C93 || fmt == 0x87EE) {
        bool alpha = fmt != 0x8C92;
        if (fmt == 0x87EE) { dbg("gl: interpolated alpha atc not done"); alpha = true; }
        if (!tex_alloc(t, tw, th, alpha ? GPU_RGBA4 : GPU_RGB565)) return;
        if (tw != w || th != h) memset(t->t.data, 0, t->t.size);
        tex_atc(t->t.data, tw, th, d, w, h, alpha);
        if (!alpha && sig_gna(t->sig)) tex_unswap_rb(t->t.data, tw * th);
        t->alpha = alpha;
    } else if (fmt >= 0x8B90 && fmt <= 0x8B99) {
        // decode into scratch first, the pica format depends on the palette kind
        bool kind8 = fmt >= 0x8B95;
        int k = fmt - (kind8 ? 0x8B95 : 0x8B90);
        int pf = k == 4 ? GPU_RGBA5551 : (k == 1 || k == 3) ? GPU_RGBA4 : GPU_RGB565;
        if (!tex_alloc(t, tw, th, pf)) return;
        if (tw != w || th != h) memset(t->t.data, 0, t->t.size);
        bool a;
        tex_palette(t->t.data, tw, th, d, w, h, fmt, &a);
        t->alpha = a;
    } else {
        dbg("gl: compressed fmt %lx not done", fmt);
        return;
    }
    tex_done(t);
}

static int raw_fmt(GLenum fmt, GLenum type)
{
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

void gl_TexImage2D(GLenum tg, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, const void *d)
{
    tex_t *t = bound_tex();
    if (!t || lvl) return;
    int pf = raw_fmt(fmt, type);
    if (pf < 0) { dbg("gl: teximage fmt %lx type %lx not done", fmt, type); return; }
    int tw = pot8(w), th = pot8(h);
    if (!tex_alloc(t, tw, th, pf)) return;
    memset(t->t.data, 0, t->t.size);
    t->sig = d ? fnv(d, w * h * (type == 0x1401 ? (fmt == 0x1908 ? 4 : fmt == 0x1907 ? 3 : fmt == 0x190A ? 2 : 1) : 2)) : 0;
    if (d) {
        bool a;
        tex_raw(t->t.data, tw, th, d, w, h, fmt, type, &a);
        t->alpha = a;
    }
    tex_done(t);
}

void gl_TexSubImage2D(GLenum tg, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *d)
{
    static bool warned;
    if (!warned) { dbg("gl: TexSubImage2D not done (%ldx%ld)", w, h); warned = true; }
}

// ------------------------------------------------------------------ init
void gl_init(void)
{
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    bot_target = C3D_RenderTargetCreate(BOT_W * 2, BOT_H * 2, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    bot_img = calloc(BOT_W * BOT_H, 2);
    if (!bot_target || !bot_img) { dbg("gl: no bottom target, bottom screen stays the console"); gl_bottom_on = false; }
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    vsh_dvlb = DVLB_ParseFile((u32 *)vsh_shbin, vsh_shbin_size);
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &vsh_dvlb->DVLE[0]);
    C3D_BindProgram(&prog);
    u_mvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
    u_tmat = shaderInstanceGetUniformLocation(prog.vertexShader, "tmat");
    u_k = shaderInstanceGetUniformLocation(prog.vertexShader, "k");

    for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));

    ring[0] = linearAlloc(RING_SIZE);
    ring[1] = linearAlloc(RING_SIZE);

    for (int m = 0; m < 3; m++) Mtx_Identity(&g.st[m][0]);
    g.color[0] = g.color[1] = g.color[2] = g.color[3] = 1;
    g.bs = 1; g.bd = 0; g.af = 0x207; g.df = 0x201;
    g.cull = GL_BACK; g.front = 0x0901;
    g.dmask = true; g.cmask = GPU_WRITE_COLOR;
    g.env[0] = g.env[1] = GL_MODULATE;
    g.fogmode = GL_EXP; g.fogd = 1; g.fogs = 0; g.foge = 1;
    g.vp[2] = ZW; g.vp[3] = ZH;
    g.sc[2] = ZW; g.sc[3] = ZH;
    g.cleard = 1;
    g.state_dirty = g.mvp_dirty = g.tm_dirty = g.fix_dirty = g.fog_dirty = g.fog_params = true;
    C3D_SetViewport(0, 0, 240, 400);
}

void gl_exit(void)
{
    for (int i = 0; i < MAXTEX; i++) if (texs[i]) { if (texs[i]->ok) C3D_TexDelete(&texs[i]->t); free(texs[i]); }
    linearFree(ring[0]);
    linearFree(ring[1]);
    shaderProgramFree(&prog);
    DVLB_Free(vsh_dvlb);
    C3D_RenderTargetDelete(target);
    C3D_Fini();
}

// ------------------------------------------------------------------ gpu self test
// walks from a stock citro3d draw up to this file's full path, one feature per stage.
// stage numbers are what the log and the bottom screen report.
#include "stock_shbin.h"

static DVLB_s *stock_dvlb;
static shaderProgram_s stock_prog;
static C3D_Tex test_tex;

static void *ring_put(const void *src, u32 n)
{
    void *d = ring_alloc(n);
    if (d) memcpy(d, src, n);
    return d;
}

bool gl_selftest_frame(int stage, int f)
{
    if (!try_begin(2000)) return false;
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x202040FF, 0xFFFFFF);

    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0, 400, 0, 240, -1, 1, true);
    float x = 100 + (f % 10) * 10;

    if (stage == 1) {
        // stock shader, float position + float color in one interleaved buffer
        if (!stock_dvlb) {
            stock_dvlb = DVLB_ParseFile((u32 *)stock_shbin, stock_shbin_size);
            shaderProgramInit(&stock_prog);
            shaderProgramSetVsh(&stock_prog, &stock_dvlb->DVLE[0]);
        }
        C3D_BindProgram(&stock_prog);
        int u = shaderInstanceGetUniformLocation(stock_prog.vertexShader, "proj");
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u, &proj);
        float v[] = { x, 40, 0.5f, 1, 0, 0, 1,   x + 200, 40, 0.5f, 0, 1, 0, 1,   x + 100, 200, 0.5f, 0, 0, 1, 1 };
        void *p = ring_put(v, sizeof v);
        C3D_AttrInfo *ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);
        C3D_BufInfo *bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, p, 28, 2, 0x10);
        texenv_update(NULL);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        hw_tex = (tex_t *)-1;
        return true;
    }

    C3D_BindProgram(&prog);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mvp, &proj);
    C3D_Mtx id;
    Mtx_Identity(&id);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_tmat, &id);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_k, 0, 1, 0, 0);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    AttrInfo_Init(ai);
    BufInfo_Init(bi);

    if (stage == 2) {
        // our shader, position + texcoord + color all from one float buffer
        float v[] = { x, 40, 0.5f, 0, 0, 1, 0, 0, 1,
                      x + 200, 40, 0.5f, 0, 0, 0, 1, 0, 1,
                      x + 100, 200, 0.5f, 0, 0, 0, 0, 1, 1 };
        void *p = ring_put(v, sizeof v);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
        AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4);
        BufInfo_Add(bi, p, 36, 3, 0x210);
        texenv_update(NULL);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        hw_tex = (tex_t *)-1;
        return true;
    }

    if (stage == 3) {
        // our shader, float position, texcoord + color as constant attributes like the gl path
        float v[] = { x, 40, 0.5f,  x + 200, 40, 0.5f,  x + 100, 200, 0.5f };
        void *p = ring_put(v, sizeof v);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        AttrInfo_AddFixed(ai, 1);
        AttrInfo_AddFixed(ai, 2);
        BufInfo_Add(bi, p, 12, 1, 0x0);
        C3D_FixedAttribSet(1, 0, 0, 0, 1);
        C3D_FixedAttribSet(2, 1, 0.5f, 0.2f, 1);
        texenv_update(NULL);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        hw_tex = (tex_t *)-1;
        return true;
    }

    // stage 4+: short positions like the game's 2d, then texture, then gl state
    s16 v[] = { (s16)x, 40, (s16)x, 200, (s16)(x + 160), 40, (s16)(x + 160), 200 };
    void *p = ring_put(v, sizeof v);
    AttrInfo_AddLoader(ai, 0, GPU_SHORT, 2);
    BufInfo_Add(bi, p, 4, 1, 0x0);
    if (stage >= 5) {
        if (!test_tex.data) {
            C3D_TexInit(&test_tex, 64, 64, GPU_RGBA8);
            u32 *px = test_tex.data;
            for (int i = 0; i < 64 * 64; i++) px[i] = (i >> 5) & 1 ? 0xFFC020FF : 0x202020FF;
            GSPGPU_FlushDataCache(test_tex.data, test_tex.size);
            C3D_TexSetFilter(&test_tex, GPU_LINEAR, GPU_LINEAR);
        }
        // float texcoords, which is what fixed point uv turns into now
        float uv[] = { 0, 1, 0, 0, 1, 1, 1, 0 };
        void *q = ring_put(uv, sizeof uv);
        AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
        BufInfo_Add(bi, q, 8, 1, 0x1);
        C3D_TexBind(0, &test_tex);
        C3D_TexEnv *e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
    } else {
        AttrInfo_AddFixed(ai, 1);
        C3D_FixedAttribSet(1, 0, 0, 0, 1);
        texenv_update(NULL);
    }
    AttrInfo_AddFixed(ai, 2);
    C3D_FixedAttribSet(2, 1, 1, 1, 1);
    if (stage >= 6) {
        // the fragment state the gl path sets on every frame
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 240, 400);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_DepthMap(true, -1.0f, 0.0f);
    }
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    hw_tex = (tex_t *)-1;
    return true;
}

void gl_selftest_end(void)
{
    flush_ring();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    in_frame = false;
    C3D_TexBind(0, NULL);
    C3D_BindProgram(&prog);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    hw_tex = (tex_t *)-1;
    hw_fog = false;
    g.state_dirty = g.mvp_dirty = g.tm_dirty = g.fix_dirty = true;
}

bool gl_selftest_wait(void)
{
    if (!try_begin(2000)) return false;
    // leave it open, the next stage or the game picks it up
    return true;
}
