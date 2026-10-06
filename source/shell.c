// IShell plus the small fry: heap, display, bitmap, signals, class factory, timers
#include <stdlib.h>
#include <string.h>
#include "aee.h"

#define NSLOTS 160

// unknown vtable slot: log index and caller once per call site
#define VS(n) static int vs_##n(void) { vstub_hit(n, __builtin_return_address(0)); return 0; }
static void vstub_hit(int n, void *lr)
{
    static void *seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == lr) return;
    if (nseen < 64) seen[nseen++] = lr;
    dbg("vtable slot %d not done, lr %p", n, lr);
}
#define VS10(a) VS(a##0) VS(a##1) VS(a##2) VS(a##3) VS(a##4) VS(a##5) VS(a##6) VS(a##7) VS(a##8) VS(a##9)
VS(0) VS(1) VS(2) VS(3) VS(4) VS(5) VS(6) VS(7) VS(8) VS(9)
VS10(1) VS10(2) VS10(3) VS10(4) VS10(5) VS10(6) VS10(7) VS10(8) VS10(9) VS10(10)
VS10(11) VS10(12) VS10(13) VS10(14) VS10(15)
#define V10(a) vs_##a##0, vs_##a##1, vs_##a##2, vs_##a##3, vs_##a##4, vs_##a##5, vs_##a##6, vs_##a##7, vs_##a##8, vs_##a##9
static void *const vstubs[NSLOTS] = {
    vs_0, vs_1, vs_2, vs_3, vs_4, vs_5, vs_6, vs_7, vs_8, vs_9,
    V10(1), V10(2), V10(3), V10(4), V10(5), V10(6), V10(7), V10(8), V10(9), V10(10),
    V10(11), V10(12), V10(13), V10(14), V10(15),
};

void vt_fill(void **vt, int n)
{
    memcpy(vt, vstubs, n * sizeof(void *));
}

static int one(void) { return 1; }
static int zero(void) { return 0; }

// ---------------------------------------------------------------- applet
static void *app;
static void *pmod_out_slot;

void *shell_app(void) { return app ? app : (pmod_out_slot ? *(void **)pmod_out_slot : NULL); }
void shell_set_app(void *a) { app = a; }
void shell_creating(void **slot) { pmod_out_slot = slot; }

void shell_event(uint16_t evt, uint16_t w, uint32_t dw)
{
    if (!app) return;
    int (*he)(void *, int, int, uint32_t) = ((aee_obj *)app)->vt[2];
    he(app, evt, w, dw);
}

// ---------------------------------------------------------------- timers / callbacks
typedef struct { uint32_t due; PFNNOTIFY fn; void *user; } timer_t_;
static timer_t_ timers[32];
static int ntimers;
static AEECallback *resumes[32];
static int nresumes;

static int sh_settimer(void *s, int32_t ms, PFNNOTIFY fn, void *user)
{
    if (!fn) return EBADPARM;
    // SetTimerEx hands the AEECallback in both slots
    if ((void *)fn == user) {
        AEECallback *cb = user;
        fn = cb->notify;
        user = cb->notify_data;
    }
    static int logged;
    if (logged < 12) { logged++; dbg("SetTimer %ld ms fn %p", ms, fn); }
    uint32_t due = aee_uptime() + (ms > 0 ? ms : 0);
    for (int i = 0; i < ntimers; i++)
        if (timers[i].fn == fn && timers[i].user == user) { timers[i].due = due; return SUCCESS; }
    if (ntimers == 32) { dbg("timer table full"); return ENOMEMORY; }
    timers[ntimers++] = (timer_t_){ due, fn, user };
    return SUCCESS;
}

static int sh_canceltimer(void *s, PFNNOTIFY fn, void *user)
{
    for (int i = 0; i < ntimers; i++)
        if ((!fn || timers[i].fn == fn) && (!user || timers[i].user == user))
            timers[i--] = timers[--ntimers];
    return SUCCESS;
}

static int sh_timerexp(void *s, PFNNOTIFY fn, void *user)
{
    uint32_t now = aee_uptime();
    for (int i = 0; i < ntimers; i++)
        if (timers[i].fn == fn && timers[i].user == user)
            return timers[i].due > now ? timers[i].due - now : 0;
    return 0;
}

static void cb_cancel(AEECallback *cb)
{
    for (int i = 0; i < nresumes; i++)
        if (resumes[i] == cb) { memmove(resumes + i, resumes + i + 1, (--nresumes - i) * sizeof *resumes); break; }
    cb->cancel = NULL;
}

static int sh_resume(void *s, AEECallback *cb)
{
    if (cb->cancel) cb->cancel(cb);
    if (nresumes == 32) return ENOMEMORY;
    resumes[nresumes++] = cb;
    cb->cancel = cb_cancel;
    cb->cancel_data = cb;
    return SUCCESS;
}

uint32_t shell_next_due(void)
{
    uint32_t best = 0xffffffff;
    for (int i = 0; i < ntimers; i++) if (timers[i].due < best) best = timers[i].due;
    return best;
}

// ---------------------------------------------------------------- signals
struct sig {
    void *const *vt;
    PFNNOTIFY fn;
    void *cx;
    bool queued;
};

static asig_t *sigq[16];
static int nsigq;

void sig_raise(asig_t *s)
{
    if (!s || s->queued || nsigq == 16) return;
    s->queued = true;
    sigq[nsigq++] = s;
}

static int sig_set(asig_t *s) { sig_raise(s); return SUCCESS; }
static int sig_detach(asig_t *s) { s->fn = NULL; return SUCCESS; }
static int sig_qi(asig_t *s, uint32_t iid, void **out) { *out = s; return SUCCESS; }

static void *vt_sig[6];

static int sigf_create(void *self, PFNNOTIFY fn, void *cx, asig_t **ps, asig_t **pctl)
{
    asig_t *s = calloc(1, sizeof *s);
    if (!s) return ENOMEMORY;
    s->vt = vt_sig;
    s->fn = fn;
    s->cx = cx;
    if (ps) *ps = s;
    if (pctl) *pctl = s;
    return SUCCESS;
}

static void *vt_sigf[4];
static aee_obj sigf_obj = { vt_sigf };

// ---------------------------------------------------------------- heap
static int heap_avail(void *h, uint32_t n) { return 1; }
static void *heap_malloc(void *h, uint32_t n) { return aee_malloc(n); }
static void heap_free(void *h, void *p) { aee_free(p); }
static void *heap_realloc(void *h, void *p, uint32_t n) { return realloc(p, n); }
static int heap_stats(void *h, uint32_t *total, uint32_t *used)
{
    if (total) *total = 16 << 20;
    if (used) *used = 4 << 20;
    return SUCCESS;
}
static void *vt_heap[9];
static aee_obj heap_obj = { vt_heap };

// ---------------------------------------------------------------- display + bitmap
// IDIB layout, the game reads pBmp/cx/cy/pitch straight out of it
typedef struct {
    void *const *vt;
    void *palmap;
    u8 *bmp;
    u32 *rgb;
    u32 transp;
    u16 cx, cy;
    s16 pitch;
    u16 nrgb;
    u8 depth, scheme;
    u8 pad[6];
} dib_t;

static void *vt_bmp[16];
static dib_t devbmp = { vt_bmp };

static int bmp_qi(dib_t *b, uint32_t iid, void **out)
{
    // IBitmap, IDIB, IDIB 2.0
    if (iid == 0x01001021 || iid == 0x01001045 || iid == 0x0100102c) { *out = b; return SUCCESS; }
    *out = NULL;
    return ECLASSNOTSUPPORT;
}

static int bmp_info(dib_t *b, u32 *info, int sz)
{
    info[0] = b->cx; info[1] = b->cy; info[2] = b->depth;
    return SUCCESS;
}

static int disp_devbmp(void *d, void **out)
{
    if (!devbmp.bmp) {
        devbmp.bmp = calloc(ZW * ZH, 2);
        devbmp.cx = ZW; devbmp.cy = ZH; devbmp.pitch = ZW * 2;
        devbmp.depth = 16; devbmp.scheme = 16;
    }
    *out = &devbmp;
    return SUCCESS;
}

static void *vt_disp[26];
static aee_obj disp_obj = { vt_disp };

// ---------------------------------------------------------------- shell
static int sh_create(void *s, uint32_t cls, void **out)
{
    const char *was = (const char *)g_where;
    WHERE("CreateInstance");
    void *o = NULL;
    switch (cls) {
    case 0x01001001: case 0x010127d4: o = &disp_obj; break;
    case 0x01001002: o = &heap_obj; break;
    case 0x01001003: o = filemgr_new(); break;
    case 0x0106c411: o = hid_new(); break;
    case 0x01041207: o = &sigf_obj; break;
    case 0x0103d8ec: o = qegl_new(); break;
    case 0x01005500: case 0x01005501: case 0x01005502: case 0x0100550a: case 0x01005511:
        o = media_new(cls); break;
    }
    if (out) *out = o;
    if (!o) {
        dbg("CreateInstance %08lx not supported", cls);
        WHERE(was);
        return ECLASSNOTSUPPORT;
    }
    WHERE(was);
    return SUCCESS;
}

static int sh_devinfo(void *s, u8 *p)
{
    u16 head[8] = { ZW, ZH, 0, 0, 8, 0, 0, 16 };
    memset(p, 0, 44);
    memcpy(p, head, sizeof head);
    *(u32 *)(p + 24) = 16 << 20;
    return SUCCESS;
}

static int sh_sendevent(void *s, int pri, uint32_t cls, uint16_t evt, uint16_t w, uint32_t dw)
{
    shell_event(evt, w, dw);
    return 1;
}

static void *vt_shell[52];
static aee_obj shell = { vt_shell };

void *shell_obj(void)
{
    void **v;

    v = vt_shell; vt_fill(v, 52);
    v[0] = one; v[1] = zero; v[2] = sh_create; v[4] = sh_devinfo;
    v[11] = sh_settimer; v[12] = sh_canceltimer; v[13] = sh_timerexp;
    v[21] = sh_sendevent; v[36] = sh_resume;

    v = vt_sig; vt_fill(v, 6);
    v[0] = one; v[1] = zero; v[2] = sig_qi; v[3] = sig_set; v[4] = sig_detach; v[5] = zero;

    v = vt_sigf; vt_fill(v, 4);
    v[0] = one; v[1] = zero; v[3] = sigf_create;

    v = vt_heap; vt_fill(v, 9);
    v[0] = one; v[1] = zero; v[2] = heap_malloc; v[3] = heap_realloc; v[4] = heap_free;
    v[6] = heap_avail; v[7] = heap_stats;

    v = vt_bmp; vt_fill(v, 16);
    v[0] = one; v[1] = zero; v[2] = bmp_qi; v[12] = bmp_info;

    // the game only touches display on its error screens, keep it quiet
    v = vt_disp; vt_fill(v, 26);
    for (int i = 0; i < 26; i++) v[i] = zero;
    v[0] = one; v[16] = disp_devbmp;

    return &shell;
}

// one pass of the brew scheduler, returns how much ran
int shell_pump(void)
{
    int ran = 0;
    uint32_t now = aee_uptime();

    // pull the due ones out first, callbacks rearm themselves while we loop
    timer_t_ due[32];
    int nd = 0;
    for (int i = 0; i < ntimers; i++)
        if (timers[i].due <= now) { due[nd++] = timers[i]; timers[i--] = timers[--ntimers]; }
    for (int i = 0; i < nd; i++) {
        WHERE("game timer");
        due[i].fn(due[i].user);
        ran++;
    }

    int n = nresumes;
    AEECallback *batch[32];
    memcpy(batch, resumes, n * sizeof *batch);
    nresumes = 0;
    for (int i = 0; i < n; i++) {
        batch[i]->cancel = NULL;
        WHERE("game callback");
        if (batch[i]->notify) batch[i]->notify(batch[i]->notify_data);
        ran++;
    }

    n = nsigq;
    asig_t *sb[16];
    memcpy(sb, sigq, n * sizeof *sb);
    nsigq = 0;
    for (int i = 0; i < n; i++) {
        sb[i]->queued = false;
        WHERE("game signal");
        if (sb[i]->fn) sb[i]->fn(sb[i]->cx);
        ran++;
    }
    return ran;
}
