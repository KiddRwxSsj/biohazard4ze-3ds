#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "aee.h"

#define APP_CLSID 0x0108AF6C

void egl_init(void);
extern bool gl_wide;
extern volatile bool gl_dump;
int gl_probe_next(void);
extern bool gl_bottom_owned, gl_bottom_on;
void gl_set_console(PrintConsole *c);
void gl_bottom_toggle(void);
const char *gl_probe_name(int p);
void gl_resume(void);
extern u64 gl_busy_ticks;
extern u32 gl_swaps;

// once a second, even before the first frame: tells a stuck game apart from a stuck gpu
static void heartbeat(void)
{
    static u64 last;
    static u32 last_swaps;
    u64 now = osGetTime();
    if (now - last < 1000) return;
    last = now;
    const char *bgm = midi_core() < 0 ? "off" : midi_ready() ? "on" : "loading";
    printf("\x1b[18;0H%4lus frames %6lu +%2lu bgm %s c%d\x1b[K\n", aee_uptime() / 1000,
           gl_swaps, gl_swaps - last_swaps, bgm, midi_core());
    if (gl_swaps == last_swaps) dbg("heartbeat: no frame this second, total %lu", gl_swaps);
    last_swaps = gl_swaps;

    // console only presents its buffer once at init, under the homebrew launcher that
    // never made it to the lcd. flush and present it again every second. not once the game's
    // screens own the bottom
    if (gl_bottom_owned && gl_bottom_on) goto stats;
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    GSPGPU_FlushDataCache(fb, fw * fh * 2);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);

stats:;
    static int n;
    if (++n % 5 == 0)
        dbg("stats: %lus frames %lu vblank %lu/%lu heap free %luk linear free %luk", aee_uptime() / 1000,
            gl_swaps, C3D_FrameCounter(0), C3D_FrameCounter(1), mem_heap_free() >> 10, linearSpaceFree() >> 10);
}

typedef struct {
    int error;
    u32 cls;
    void *display;
    s16 rc[4];
    const char *args;
} app_start_t;

// linear only holds gpu stuff (ring, textures, cmdbuf, framebuffers) and sfx pcm. the default
// split capped the normal heap near 31MB, too small for the soundfont next to everything else
u32 __ctru_linear_heap_size = 16 << 20;

extern volatile bool gl_paused;

volatile const char *g_where = "boot";
volatile u32 g_beat;
static volatile bool wd_quit;
static Thread wd;

// separate thread, so a hang still says where it is
static void watchdog(void *arg)
{
    u32 last = g_beat, still = 0;
    bool told = false;
    while (!wd_quit) {
        svcSleepThread(500000000LL);
        if (g_beat != last) {
            last = g_beat;
            still = 0;
            if (told) { printf("\x1b[16;0H\x1b[K"); told = false; }
            continue;
        }
        if (++still < 3 || told) continue;
        const char *w = (const char *)g_where;
        printf("\x1b[16;0Hno progress 1.5s in: %.18s\x1b[K", w);
        dbg("watchdog: main thread made no progress for 1.5s in '%s' (frames %lu)", w, gl_swaps);
        told = true;
    }
}

static aptHookCookie apt_cookie;

static void apt_cb(APT_HookType type, void *p)
{
    // citro3d's own hook already parked the gpu by now, anything the game draws in its
    // suspend handler has to go nowhere or it waits forever
    if (type == APTHOOK_ONSUSPEND) {
        gl_paused = true;
        shell_event(EVT_APP_SUSPEND, 0, 0);
        dbg("apt: suspended");
    } else if (type == APTHOOK_ONRESTORE) {
        gl_resume();
        gl_paused = false;
        shell_event(EVT_APP_RESUME, 0, 0);
        dbg("apt: restored");
    } else if (type == APTHOOK_ONEXIT) {
        gl_paused = true;
        dbg("apt: exit requested");
    }
}

// replaces libctru's: that one unmaps both heaps, then calls APT_CloseApplication (via
// __system_retAddr) and waits there to be killed. the gsp event thread is still running off a
// heap stack at that point and data aborts on the next vblank (luma crash screen, hard reboot).
// a stuck ndsp or teardown thread would do the same. cia only, the kernel frees it all at exit
extern void (*__system_retAddr)(void);

void __attribute__((noreturn)) __libctru_exit(int rc)
{
    if (__system_retAddr) __system_retAddr();
    svcExitProcess();
}

static void fatal(const char *msg)
{
    printf("\n%s\n\npress START to quit\n", msg);
    dbg("fatal: %s", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

int main(void)
{
    gfxInitDefault();
    gl_set_console(consoleInit(GFX_BOTTOM, NULL));
    osSetSpeedupEnable(true);
    printf("re4z\n");

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    wd = threadCreate(watchdog, NULL, 8 * 1024, prio > 0x19 ? prio - 2 : 0x18, 0, false);

    // hold L at boot: no music, hold R: no audio at all. hid needs a few frames before
    // the first scan reports anything, so keep scanning for a moment
    u32 boot_keys = 0;
    for (int i = 0; i < 12; i++) {
        gspWaitForVBlank();
        hidScanInput();
        boot_keys |= hidKeysHeld();
    }
    bool audio = !(boot_keys & KEY_R), music = audio && !(boot_keys & KEY_L);
    printf("audio %s, music %s\n", audio ? "on" : "off", music ? "on" : "off");
    dbg("boot: keys %08lx audio %d music %d heap %luk linear %luk", boot_keys, audio, music,
        mem_heap_free() >> 10, linearSpaceFree() >> 10);

    WHERE("init files"); files_init();
    void *shell = shell_obj();
    egl_init();
    WHERE("init audio"); media_init(audio, music);
    WHERE("init gpu"); gl_init();
    void *helpers = helpers_table();

    if (boot_keys & (KEY_X | KEY_Y)) {
        gputest_run(boot_keys & KEY_Y);
        goto out;
    }
    WHERE("load mod");

    void *mod = NULL;
    if (mod_load(GAME_DIR "bio4_brew.mod", shell, helpers, &mod) || !mod) {
        fatal("could not load " GAME_DIR "bio4_brew.mod\ncheck the data files and luma3ds");
        goto out;
    }

    void *app = NULL;
    shell_creating(&app);
    int (*mod_create)(void *, void *, u32, void **) = ((aee_obj *)mod)->vt[2];
    WHERE("game create");
    int rc = mod_create(mod, shell, APP_CLSID, &app);
    dbg("app create -> %d, applet %p", rc, app);
    if (rc || !app) {
        fatal("game refused to start, see log.txt");
        goto out;
    }
    shell_set_app(app);

    void *disp = NULL;
    int (*sh_create)(void *, u32, void **) = ((aee_obj *)shell)->vt[2];
    sh_create(shell, 0x01001001, &disp);
    app_start_t st = { 0, APP_CLSID, disp, { 0, 0, ZW, ZH }, "" };
    aptHook(&apt_cookie, apt_cb, NULL);
    WHERE("game start");
    shell_event(EVT_APP_START, 0, (u32)&st);
    printf("running. select+start toggles wide/4:3\n");
    dbg("main: game started");

    while (aptMainLoop()) {
        g_beat++;
        WHERE("main loop");
        hid_poll();
        u32 down = hidKeysDown(), held = hidKeysHeld();
        // l+r+select+start quits through the same teardown as the home menu
        if ((held & (KEY_L | KEY_R | KEY_SELECT | KEY_START)) == (KEY_L | KEY_R | KEY_SELECT | KEY_START)) break;
        if ((held & KEY_SELECT) && (down & KEY_START) && !(held & (KEY_L | KEY_R))) gl_wide = !gl_wide;
        // hw debugging: l+r+y logs the next frame's draws, l+r+x cycles a state override
        if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) {
            if (down & KEY_Y) {
                gl_dump = true;
                printf("\x1b[14;0Hdraw dump -> log_cia.txt\x1b[K");
            }
            if (down & KEY_B) gl_bottom_toggle();
            if (down & KEY_A) {
                midi_capture_start();
                printf("\x1b[15;0Hbgm capture: 20s -> bgm_capture.raw\x1b[K");
            }
            if (down & KEY_X) {
                int p = gl_probe_next();
                printf("\x1b[13;0Hprobe %d: %s\x1b[K", p, gl_probe_name(p));
                dbg("probe %d: %s", p, gl_probe_name(p));
            }
        }

        u64 t = svcGetSystemTick();
        int ran = shell_pump();
        if (ran) gl_busy_ticks += svcGetSystemTick() - t;
        media_update();
        if (midi_capture_poll()) printf("\x1b[15;0Hbgm capture written\x1b[K");
        heartbeat();
        if (!ran) {
            // nothing due, nap until the next timer but keep polling input
            u32 due = shell_next_due(), now = aee_uptime();
            u32 wait = due > now ? due - now : 0;
            if (wait > 4) wait = 4;
            if (wait) svcSleepThread(wait * 1000000LL);
        }
    }
    // no EVT_APP_STOP: apt is already tearing us down, and the game's stop path would
    // try to draw and play sounds. it saves on its own at the typewriter anyway
    dbg("main: exiting");
    aptUnhook(&apt_cookie);

out:
    WHERE("exit audio");
    dbg("main: exit audio");
    // ndspExit polls the dsp until it answers, which it doesn't once the home menu put it to
    // sleep. give teardown a deadline and leave if it doesn't make it, exit kills the thread
    Thread t = threadCreate((ThreadFunc)media_exit, NULL, 16 * 1024, 0x30, 0, false);
    if (t && R_FAILED(threadJoin(t, 1500000000LL))) dbg("main: audio teardown stuck, leaving it");
    else if (!t) media_exit();
    // no C3D_Fini: closing a suspended app means the home menu holds the gpu and it waits on
    // our gx queue forever. gsp's event thread stays up, __libctru_exit above keeps its stack mapped
    wd_quit = true;
    if (wd) { threadJoin(wd, 1000000000LL); threadFree(wd); }
    dbg("main: bye");
    dbg_close();
    return 0;
}
