#pragma once
#include <3ds.h>
#include <stdint.h>
#include <stdbool.h>

#define SUCCESS             0
#define EFAILED             1
#define ENOMEMORY           2
#define ECLASSNOTSUPPORT    3
#define EBADPARM            14
#define EUNSUPPORTED        20

#define EVT_APP_START       0
#define EVT_APP_STOP        1
#define EVT_APP_SUSPEND     2
#define EVT_APP_RESUME      3

// game data + saves live here
#define GAME_DIR "sdmc:/re4z/"

// zeebo screen, what the game thinks it renders to
#define ZW 640
#define ZH 480

typedef uint16_t AECHAR;
typedef void (*PFNNOTIFY)(void *);

// every brew object is a pointer to its vtable first
typedef struct { void *const *vt; } aee_obj;

typedef struct AEECallback {
    struct AEECallback *next;
    void *mgr;
    void (*cancel)(struct AEECallback *);
    void *cancel_data;
    PFNNOTIFY notify;
    void *notify_data;
    void *reserved;
} AEECallback;

// main.c: where the main thread is, read by the watchdog when it stops moving
extern volatile const char *g_where;
extern volatile u32 g_beat;
#define WHERE(s) (g_where = (s))

// log.c
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dbg_close(void);
u32  mem_heap_free(void);

// mod.c
int mod_load(const char *path, void *shell, void *helpers, void **pmod);

// helpers.c
void *helpers_table(void);
void *aee_malloc(uint32_t n);
void aee_free(void *p);
uint32_t aee_uptime(void);

// shell.c
void vt_fill(void **vt, int n);
void shell_creating(void **slot);
void *shell_obj(void);
void *shell_app(void);
void shell_set_app(void *app);
int  shell_pump(void);
void shell_event(uint16_t evt, uint16_t w, uint32_t dw);
uint32_t shell_next_due(void);

typedef struct sig asig_t;
void sig_raise(asig_t *s);

// file.c
void *filemgr_new(void);
void files_init(void);

// lang.c
void lang_apply(const char *arc, u8 *d, u32 size);

// hid.c
void *hid_new(void);
void hid_poll(void);

// egl.c
void *qegl_new(void);

// media.c
void *media_new(uint32_t cls);
void media_init(bool audio, bool music);
void media_update(void);
void media_exit(void);

// midi.c
void midi_init(void);
void midi_exit(void);
int  midi_play(const void *data, u32 size, float vol);
void midi_stop(int h);
void midi_volume(int h, float vol);
bool midi_finished(int h);
int  midi_load(u32 elapsed_ms);
int  midi_core(void);
bool midi_ready(void);
void midi_capture_start(void);
bool midi_capture_poll(void);

// gputest.c
void gputest_run(bool draw);

// gl.c
void gl_init(void);
void gl_exit(void);
void gl_swap(void);
