// bgm: the game's smf files through tinysoundfont, rendered on core 1 and
// streamed into one ndsp channel. two slots so a jingle can sit on top of a loop.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "aee.h"

#pragma GCC optimize("O3")
// float libm for the per-voice, per-block modulation math (pitch, filter, gain): the double
// pow and tan tsf uses by default are several times slower on the vfp11, and the synth sits
// near real time on o3ds core 1
#include <math.h>
#define TSF_POW(a, b) powf((a), (b))
#define TSF_POWF powf
#define TSF_EXPF expf
#define TSF_LOG logf
#define TSF_TAN tanf
#define TSF_LOG10 log10f
#define TSF_SQRT sqrtf
#define TSF_SQRTF sqrtf
#define TSF_SAMPLES_SHORT
#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "tsf/tsf.h"
#include "tsf/tml.h"

#define RATE      22050
#define CHUNK     1024          // frames per wavebuf, ~46ms
#define NBUF      8           // ~370ms queued, rides out core1 hiccups
#define CHAN      23
#define MAXVOICES 32
#define GAIN_DB   -8.0f
#define NSLOT     2

typedef struct {
    tsf *synth;
    tml_message *song, *cur;
    double t;                   // ms into the song
    float vol;
    bool playing;
    int owner;
    u16 drums;                  // channels on a drum kit, bit per channel
} slot_t;

// the game thread never touches slots: it queues commands and reads back finished handles,
// both under a lock held for a handful of instructions. rendering runs with no lock held.
enum { CMD_PLAY, CMD_STOP, CMD_VOL };
typedef struct { int op, h; tml_message *song; float vol; } cmd_t;

static tsf *font;
static slot_t slots[NSLOT];
static int next_owner = 1;
static LightLock qlock;
static cmd_t cmdq[32];
static int cq_head, cq_tail;
static int donev[16], ndone;
static LightEvent wake;
static Thread th;
static volatile bool quit, failed;
static volatile int maxvoices = MAXVOICES;
static ndspWaveBuf wbuf[NBUF];
static s16 *pcm;
static u64 busy_ticks;
static float gain;

// hw debugging (L+R+A): CAP_CHUNKS of the exact pcm we queue, plus what the dsp reported at each
// queue, written to sd by the main thread once full
#define CAP_CHUNKS 431          // ~20s
typedef struct { u32 ms; u16 our_seq, dsp_seq; u32 dsp_pos; u8 done, voices; } cap_info_t;
static volatile int cap_state;  // 0 idle, 1 recording, 2 full
static s16 *cap_pcm;
static cap_info_t *cap_info;
static int cap_n;
static int on_core = -1;

// the songs use the mobile bank convention: bank msb 124 melodic, 125 drum kit on any channel
// (gm only knows channel 10). 92 bowed pad is a near white noise sample in GeneralUser GS's top
// layer, it hisses like static on the level select and in a few levels, so it plays warm pad
static void apply(slot_t *sl, const tml_message *m)
{
    tsf *s = sl->synth;
    int ch = m->channel;
    bool drum = sl->drums >> ch & 1;
    switch (m->type) {
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(s, ch, !drum && m->program == 92 ? 89 : m->program, drum);
        break;
    case TML_NOTE_ON: tsf_channel_note_on(s, ch, m->key, m->velocity / 127.0f); break;
    case TML_NOTE_OFF: tsf_channel_note_off(s, ch, m->key); break;
    case TML_PITCH_BEND: tsf_channel_set_pitchwheel(s, ch, m->pitch_bend); break;
    case TML_CONTROL_CHANGE:
        if (m->control == 0 && (m->control_value == 124 || m->control_value == 125))
            sl->drums = (sl->drums & ~(1 << ch)) | (m->control_value == 125) << ch;
        tsf_channel_midi_control(s, ch, m->control, m->control_value);
        break;
    }
}

// tsf only treats maxVoiceNum as a flag, the pool stays at MAXVOICES. when the synth falls
// behind, drop the quietest voices until the cap holds
static void cap_voices(tsf *s, int cap)
{
    for (int n = tsf_active_voice_count(s); n > cap; n--) {
        struct tsf_voice *q = NULL;
        for (struct tsf_voice *v = s->voices, *e = v + s->voiceNum; v != e; v++)
            if (v->playingPreset != -1 && (!q || v->ampenv.level < q->ampenv.level)) q = v;
        if (!q) break;
        tsf_voice_kill(q);
    }
}

static slot_t *by_owner(int h)
{
    for (int i = 0; i < NSLOT; i++) if (h && slots[i].owner == h) return &slots[i];
    return NULL;
}

static void drain_cmds(void)
{
    cmd_t c[32];
    int n = 0;
    LightLock_Lock(&qlock);
    while (cq_head != cq_tail) { c[n++] = cmdq[cq_head]; cq_head = (cq_head + 1) & 31; }
    LightLock_Unlock(&qlock);

    for (int i = 0; i < n; i++) {
        slot_t *s = c[i].op == CMD_PLAY ? NULL : by_owner(c[i].h);
        if (c[i].op == CMD_PLAY) {
            // a free slot, else take over the first one
            for (int k = 0; k < NSLOT && !s; k++) if (!slots[k].playing) s = &slots[k];
            if (!s) s = &slots[0];
            if (s->song) tml_free(s->song);
            if (s->synth) {
                // let the old song ring out, the new one sets its own programs
                tsf_note_off_all(s->synth);
                for (int ch = 0; ch < 16; ch++) tsf_channel_midi_control(s->synth, ch, 121, 0);
                tsf_channel_set_bank_preset(s->synth, 9, 128, 0);
            }
            s->song = s->cur = c[i].song;
            s->t = 0;
            s->vol = c[i].vol;
            s->playing = true;
            s->owner = c[i].h;
            s->drums = 1 << 9;
            slot_t *o = &slots[s == &slots[0]];
            dbg("midi: h%d -> slot %d, other slot %s h%d", c[i].h, (int)(s - slots), o->playing ? "playing" : "idle", o->owner);
        } else if (c[i].op == CMD_STOP && !s) {
            dbg("midi: stop h%d, no slot", c[i].h);
        } else if (s && c[i].op == CMD_STOP) {
            dbg("midi: stop h%d slot %d", c[i].h, (int)(s - slots));
            s->playing = false;
            s->owner = 0;
            if (s->synth) tsf_note_off_all(s->synth);
        } else if (s && c[i].op == CMD_VOL) {
            s->vol = c[i].vol;
        }
    }
}

static void post_done(int h)
{
    LightLock_Lock(&qlock);
    if (ndone < 16) donev[ndone++] = h;
    LightLock_Unlock(&qlock);
}

static void render(s16 *out)
{
    drain_cmds();
    memset(out, 0, CHUNK * 4);
    for (int i = 0; i < NSLOT; i++) {
        slot_t *s = &slots[i];
        if (!s->synth) {
            if (!s->playing) continue;
            // copies share the samples but start with no voices, give them a pool
            s->synth = tsf_copy(font);
            if (!s->synth) continue;
            tsf_set_max_voices(s->synth, MAXVOICES);
            tsf_channel_set_bank_preset(s->synth, 9, 128, 0);
        }
        cap_voices(s->synth, maxvoices);
        // set_volume replaces the output gain, so fold it back in
        tsf_set_volume(s->synth, s->vol * gain);
        // events land on their own sample: render up to each one, apply it, go on. applying a
        // whole chunk's events up front put every note up to 46ms early on a 46ms grid, drums
        // and bass smeared against each other like two copies of the song out of sync
        int pos = 0;
        if (s->playing) {
            double t0 = s->t;
            s->t += CHUNK * 1000.0 / RATE;
            for (; s->cur && s->cur->time <= s->t; s->cur = s->cur->next) {
                int at = (int)((s->cur->time - t0) * RATE / 1000.0);
                if (at > CHUNK) at = CHUNK;
                if (at > pos) {
                    tsf_render_short(s->synth, out + pos * 2, at - pos, 1);
                    pos = at;
                }
                apply(s, s->cur);
            }
            if (!s->cur) {
                s->playing = false;
                dbg("midi: h%d done", s->owner);
                post_done(s->owner);
                s->owner = 0;
            }
        }
        if (pos == 0 && !s->playing && !tsf_active_voice_count(s->synth)) continue;
        if (pos < CHUNK) tsf_render_short(s->synth, out + pos * 2, CHUNK - pos, 1);
    }
}

static void dsp_cb(void *p) { LightEvent_Signal(&wake); }

static void thread_main(void *arg)
{
    // runfast: flush denormals, release tails decay into them and vfp11 traps on each one
    __asm__ volatile("vmsr fpscr, %0" :: "r"(0x03000000));
    struct stat st;
    if (stat(GAME_DIR "bgm.sf2", &st)) { dbg("midi: no " GAME_DIR "bgm.sf2, music off"); failed = true; return; }
    dbg("midi: loading bgm.sf2 (%ld bytes), heap free %luk", (long)st.st_size, mem_heap_free() >> 10);
    font = tsf_load_filename(GAME_DIR "bgm.sf2");
    if (!font) {
        dbg("midi: bgm.sf2 failed to load, heap free %luk. out of memory? music off", mem_heap_free() >> 10);
        failed = true;
        return;
    }
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, RATE, 0);
    gain = powf(10.0f, GAIN_DB / 20.0f);
    dbg("midi: soundfont loaded, %d presets", tsf_get_presetcount(font));

    ndspChnReset(CHAN);
    ndspChnSetInterp(CHAN, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHAN, RATE);
    ndspChnSetFormat(CHAN, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1, 1 };
    ndspChnSetMix(CHAN, mix);
    for (int i = 0; i < NBUF; i++) {
        memset(&wbuf[i], 0, sizeof wbuf[i]);
        wbuf[i].data_pcm16 = pcm + i * CHUNK * 2;
        wbuf[i].nsamples = CHUNK;
        wbuf[i].status = NDSP_WBUF_DONE;
    }
    // a chunk is ~46ms of audio. if rendering eats more than 85% of that over a
    // window, drop the voice cap a notch so the music keeps up instead of stuttering
    const u64 chunk_ticks = (u64)(CHUNK * 1000.0 / RATE * CPU_TICKS_PER_MSEC);
    u64 win_ticks = 0;
    int win_n = 0;

    u32 dry = 0;
    u64 dry_told = 0;
    bool started = false;   // all buffers start out DONE
    while (!quit) {
        // every buffer back means the dsp ran out of music: a gap, or stale audio on hw
        int done = 0;
        for (int i = 0; i < NBUF; i++) done += wbuf[i].status == NDSP_WBUF_DONE;
        if (done == NBUF && started) {
            dry++;
            u64 now = osGetTime();
            if (now - dry_told > 5000) { dbg("midi: dsp ran dry, %lu times so far", dry); dry_told = now; }
        }
        started = true;
        for (int i = 0; i < NBUF && !quit; i++) {
            if (wbuf[i].status != NDSP_WBUF_DONE) continue;
            u64 t0 = svcGetSystemTick();
            render(wbuf[i].data_pcm16);
#ifdef SLOWSYNTH
            // test hook: pretend the synth is slower than real time
            { u64 e = svcGetSystemTick() + (u64)(SLOWSYNTH * CPU_TICKS_PER_MSEC); while (svcGetSystemTick() < e); }
#endif
            DSP_FlushDataCache(wbuf[i].data_pcm16, CHUNK * 4);
            ndspChnWaveBufAdd(CHAN, &wbuf[i]);
            if (cap_state == 1) {
                memcpy(cap_pcm + cap_n * CHUNK * 2, wbuf[i].data_pcm16, CHUNK * 4);
                cap_info_t *ci = &cap_info[cap_n];
                ci->ms = osGetTime();
                ci->our_seq = wbuf[i].sequence_id;
                ci->dsp_seq = ndspChnGetWaveBufSeq(CHAN);
                ci->dsp_pos = ndspChnGetSamplePos(CHAN);
                ci->done = done;
                ci->voices = slots[0].synth ? tsf_active_voice_count(slots[0].synth) : 0;
                if (++cap_n == CAP_CHUNKS) cap_state = 2;
            }
            u64 dt = svcGetSystemTick() - t0;
            busy_ticks += dt;
            win_ticks += dt;
            if (++win_n == 16) {
                if (win_ticks > chunk_ticks * 16 * 85 / 100 && maxvoices > 12) {
                    maxvoices -= 4;
                    dbg("midi: behind real time, voices down to %d", maxvoices);
                }
                win_ticks = 0;
                win_n = 0;
            }
            // give the dsp and everything else on this core a breath between chunks
            svcSleepThread(1000000);
        }
        // timed wait: once apt starts closing us the dsp stops calling back
        LightEvent_WaitTimeout(&wake, 50000000LL);
    }
}

void midi_init(void)
{
    LightLock_Init(&qlock);
    LightEvent_Init(&wake, RESET_ONESHOT);
    pcm = linearAlloc(NBUF * CHUNK * 4);
    if (!pcm) { failed = true; return; }
    ndspSetCallback(dsp_cb, NULL);

    // synth lives on the syscore when we can get a slice of it. on core 0 it has to sit
    // below the game thread, otherwise a slow render starves the game
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // the allowed max differs between setups, walk down until one sticks
    static const u32 limits[] = { 70, 50, 40, 30, 25 };
    Result r = -1;
    for (unsigned i = 0; i < sizeof limits / sizeof *limits && R_FAILED(r); i++) {
        r = APT_SetAppCpuTimeLimit(limits[i]);
        if (R_SUCCEEDED(r)) dbg("midi: core 1 slice %lu%%", limits[i]);
    }
    if (R_SUCCEEDED(r)) {
        th = threadCreate(thread_main, NULL, 64 * 1024, prio - 1, 1, false);
        if (th) on_core = 1;
    }
    if (!th) {
        dbg("midi: no core 1 (%08lx), synth on core 0 below the game", r);
        th = threadCreate(thread_main, NULL, 64 * 1024, prio + 1, 0, false);
        if (th) on_core = 0;
    }
    if (!th) failed = true;
}

int midi_core(void) { return on_core; }

void midi_capture_start(void)
{
    if (cap_state || !th) return;
    cap_pcm = malloc(CAP_CHUNKS * CHUNK * 4);
    cap_info = malloc(CAP_CHUNKS * sizeof *cap_info);
    if (!cap_pcm || !cap_info) { free(cap_pcm); free(cap_info); dbg("midi: capture, no memory"); return; }
    cap_n = 0;
    dbg("midi: capture start");
    cap_state = 1;
}

// main thread: write a finished capture out. true once written
bool midi_capture_poll(void)
{
    if (cap_state != 2) return false;
    FILE *f = fopen(GAME_DIR "bgm_capture.raw", "wb");
    if (f) { fwrite(cap_pcm, CHUNK * 4, CAP_CHUNKS, f); fclose(f); }
    f = fopen(GAME_DIR "bgm_capture.txt", "w");
    if (f) {
        fprintf(f, "# s16 stereo %d Hz, %d frames per chunk. ms our_seq dsp_seq dsp_pos done voices\n", RATE, CHUNK);
        for (int i = 0; i < CAP_CHUNKS; i++)
            fprintf(f, "%lu %u %u %lu %u %u\n", cap_info[i].ms, cap_info[i].our_seq, cap_info[i].dsp_seq,
                    cap_info[i].dsp_pos, cap_info[i].done, cap_info[i].voices);
        fclose(f);
    }
    free(cap_pcm); free(cap_info);
    cap_pcm = NULL; cap_info = NULL;
    cap_state = 0;
    dbg("midi: capture written");
    return true;
}
bool midi_ready(void) { return font != NULL; }

void midi_exit(void)
{
    if (!th) return;
    quit = true;
    LightEvent_Signal(&wake);
    // still busy loading the soundfont? then leave it be, the process is going away anyway
    if (R_FAILED(threadJoin(th, 500000000LL))) { dbg("midi: synth thread didnt stop, leaving it"); return; }
    threadFree(th);
    ndspChnReset(CHAN);
    for (int i = 0; i < NSLOT; i++) {
        if (slots[i].song) tml_free(slots[i].song);
        if (slots[i].synth) tsf_close(slots[i].synth);
    }
    if (font) tsf_close(font);
    linearFree(pcm);
}

static void push(cmd_t c)
{
    LightLock_Lock(&qlock);
    int nt = (cq_tail + 1) & 31;
    if (nt != cq_head) { cmdq[cq_tail] = c; cq_tail = nt; }
    else if (c.song) tml_free(c.song);
    LightLock_Unlock(&qlock);
}

// returns a handle, 0 if music is off
int midi_play(const void *data, u32 size, float vol)
{
    if (failed || !th) return 0;
    tml_message *song = tml_load_memory(data, size);
    if (!song) return 0;
    int h = next_owner++;
    push((cmd_t){ CMD_PLAY, h, song, vol });
    dbg("midi: play %lu bytes vol %.2f -> h%d", size, vol, h);
    return h;
}

void midi_stop(int h) { if (h) push((cmd_t){ CMD_STOP, h, NULL, 0 }); }
void midi_volume(int h, float vol) { if (h) push((cmd_t){ CMD_VOL, h, NULL, vol }); }

// true once the last event of the song went out
bool midi_finished(int h)
{
    bool f = false;
    LightLock_Lock(&qlock);
    for (int i = 0; i < ndone; i++)
        if (donev[i] == h) { donev[i] = donev[--ndone]; f = true; break; }
    LightLock_Unlock(&qlock);
    return f;
}

// synth thread load in percent of real time, for the overlay
int midi_load(u32 elapsed_ms)
{
    u64 t = busy_ticks;
    busy_ticks = 0;
    return elapsed_ms ? (int)(t / CPU_TICKS_PER_MSEC * 100 / elapsed_ms) : 0;
}
