// IMedia. sfx are ima adpcm wavs, decoded once and kept in linear ram.
// bgm is smf midi, no synth yet so it plays silent but still acts like it plays.
#include <stdlib.h>
#include <string.h>
#include "aee.h"

#define MM_PARM_MEDIA_DATA 1
#define MM_PARM_VOLUME 4
#define MM_PARM_PLAY_REPEAT 11
#define MM_CMD_PLAY 4
#define MM_STATUS_START 1
#define MM_STATUS_DONE 2
#define MM_STATE_IDLE 1
#define MM_STATE_PLAY 4

typedef struct {
    const void *src;
    u32 srclen;
    u32 sum;
    s16 *pcm;
    u32 frames;
    int rate;
    u32 used;
} sample_t;

static sample_t samples[96];
static int nsamples;

typedef struct media {
    void *const *vt;
    u32 cls;
    const u8 *data;
    u32 size;
    void (*notify)(void *, void *);
    void *user;
    int ch;
    sample_t *s;
    int repeat;
    int song;
    float vol;
    bool playing, midi;
    ndspWaveBuf wb;
} media_t;

typedef struct { media_t *m; u32 status; } note_t;
static note_t notes[32];
static int nnotes;
static note_t *running;    // batch being delivered, release has to null it out too
static int nrunning;
static bool dsp_ok;
static u32 notify_blk[7];

static void *vt_media[14];
static media_t *midis[16];
static int nmidi;

static void post(media_t *m, u32 status)
{
    if (!m->notify || nnotes == 32) return;
    notes[nnotes++] = (note_t){ m, status };
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }

static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int ima_idx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline s16 ima_nib(int n, int *pred, int *idx)
{
    int step = ima_step[*idx];
    int d = step >> 3;
    if (n & 1) d += step >> 2;
    if (n & 2) d += step >> 1;
    if (n & 4) d += step;
    *pred += n & 8 ? -d : d;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_idx[n];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return *pred;
}

// mono only, that's all the game ships. stereo gets downmixed by just taking left
static bool wav_decode(const u8 *d, u32 len, sample_t *s)
{
    if (len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    int fmt = 0, chans = 1, align = 0;
    const u8 *data = NULL;
    u32 dlen = 0;
    for (u32 o = 12; o + 8 <= len;) {
        u32 csz = rd32(d + o + 4);
        if (!memcmp(d + o, "fmt ", 4)) {
            fmt = rd16(d + o + 8);
            chans = rd16(d + o + 10);
            s->rate = rd32(d + o + 12);
            align = rd16(d + o + 20);
        } else if (!memcmp(d + o, "data", 4)) {
            data = d + o + 8;
            dlen = csz > len - o - 8 ? len - o - 8 : csz;
        }
        o += 8 + csz + (csz & 1);
    }
    if (!data || !s->rate) return false;

    if (fmt == 1) {
        s->frames = dlen / (2 * chans);
        s->pcm = linearAlloc(s->frames * 2);
        if (!s->pcm) return false;
        for (u32 i = 0; i < s->frames; i++) s->pcm[i] = rd16(data + i * 2 * chans);
        return true;
    }
    if (fmt != 0x11 || !align) return false;

    int per_block = (align - 4 * chans) * 2 / chans + 1;
    u32 blocks = dlen / align;
    s->frames = blocks * per_block;
    s->pcm = linearAlloc(s->frames * 2);
    if (!s->pcm) return false;
    s16 *o = s->pcm;
    for (u32 b = 0; b < blocks; b++) {
        const u8 *p = data + b * align;
        int pred = (s16)rd16(p), idx = p[2];
        if (idx > 88) idx = 88;
        *o++ = pred;
        const u8 *n = p + 4 * chans;
        for (int i = 0; i < per_block - 1; i += 2) {
            u8 byte = n[i / 2 * chans];
            *o++ = ima_nib(byte & 15, &pred, &idx);
            *o++ = ima_nib(byte >> 4, &pred, &idx);
        }
    }
    return true;
}

static media_t *chans[24];
static u32 use_tick;

static bool sample_playing(const sample_t *s)
{
    for (int i = 0; i < 23; i++)
        if (chans[i] && chans[i]->wb.data_pcm16 == s->pcm) return true;
    return false;
}

static sample_t *sample_get(const u8 *d, u32 len)
{
    // the game keeps reusing the same buffers, key on pointer, length and a few bytes
    u32 sum = len;
    for (u32 i = 0; i < len && i < 256; i += 4) sum = sum * 31 + rd32(d + i);
    for (int i = 0; i < nsamples; i++)
        if (samples[i].src == d && samples[i].srclen == len && samples[i].sum == sum) {
            samples[i].used = ++use_tick;
            return &samples[i];
        }
    sample_t *s = &samples[nsamples];
    if (nsamples == 96) {
        // full: reuse the least recently played slot in place. never one a channel is still
        // reading (the dsp would play whatever gets decoded into that memory next)
        s = NULL;
        for (int i = 0; i < 96; i++)
            if (!sample_playing(&samples[i]) && (!s || samples[i].used < s->used)) s = &samples[i];
        if (!s) return NULL;
        static bool told;
        if (!told) { dbg("media: sample cache full, recycling slots"); told = true; }
        linearFree(s->pcm);
    }
    memset(s, 0, sizeof *s);
    if (!wav_decode(d, len, s)) {
        dbg("media: cant decode %lu bytes", len);
        return NULL;
    }
    if (s == &samples[nsamples]) nsamples++;
    s->src = d; s->srclen = len; s->sum = sum; s->used = ++use_tick;
    GSPGPU_FlushDataCache(s->pcm, s->frames * 2);
    return s;
}

static void chan_stop(media_t *m)
{
    if (m->song) { midi_stop(m->song); m->song = 0; }
    if (m->ch < 0) return;
    ndspChnWaveBufClear(m->ch);
    chans[m->ch] = NULL;
    m->ch = -1;
}

static int m_addref(media_t *m) { return 1; }

static int m_release(media_t *m)
{
    if (m->midi) dbg("media: %p release midi h%d", m, m->song);
    chan_stop(m);
    for (int i = 0; i < nnotes; i++) if (notes[i].m == m) notes[i--] = notes[--nnotes];
    for (int i = 0; i < nrunning; i++) if (running[i].m == m) running[i].m = NULL;
    for (int i = 0; i < nmidi; i++) if (midis[i] == m) { midis[i] = midis[--nmidi]; break; }
    free(m);
    return 0;
}

static int m_qi(media_t *m, u32 iid, void **out) { *out = m; return SUCCESS; }

static int m_regnotify(media_t *m, void *fn, void *user)
{
    m->notify = fn;
    m->user = user;
    return SUCCESS;
}

static int m_setparm(media_t *m, int parm, u32 p1, u32 p2)
{
    switch (parm) {
    case MM_PARM_MEDIA_DATA: {
        const u32 *md = (const u32 *)p1;
        m->data = (const u8 *)md[1];
        m->size = md[2];
        m->midi = m->size >= 4 && !memcmp(m->data, "MThd", 4);
        m->s = NULL;
        return SUCCESS;
    }
    case MM_PARM_VOLUME:
        m->vol = p1 / 100.0f;
        if (m->vol > 1) m->vol = 1;
        if (m->ch >= 0) { float mix[12] = { m->vol, m->vol }; ndspChnSetMix(m->ch, mix); }
        if (m->song) midi_volume(m->song, m->vol);
        return SUCCESS;
    case MM_PARM_PLAY_REPEAT:
        m->repeat = p1;
        return SUCCESS;
    }
    return SUCCESS;
}

static int m_getparm(media_t *m, int parm, u32 *p1, u32 *p2)
{
    if (parm == MM_PARM_VOLUME && p1) *p1 = m->vol * 100;
    return SUCCESS;
}

static int m_play(media_t *m)
{
    if (!m->data) return EFAILED;
    chan_stop(m);
    m->playing = true;
    post(m, MM_STATUS_START);
    if (!dsp_ok) return SUCCESS;
    // no soundfont means song 0: plays silent and never reports done, like before
    if (m->midi) { m->song = midi_play(m->data, m->size, m->vol); return SUCCESS; }

    m->s = sample_get(m->data, m->size);
    if (!m->s) { post(m, MM_STATUS_DONE); m->playing = false; return SUCCESS; }

    int ch = -1;
    // 23 belongs to the music stream
    for (int i = 0; i < 23; i++) if (!chans[i]) { ch = i; break; }
    if (ch < 0) return SUCCESS;
    chans[ch] = m;
    m->ch = ch;
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, m->s->rate);
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { m->vol, m->vol };
    ndspChnSetMix(ch, mix);
    memset(&m->wb, 0, sizeof m->wb);
    m->wb.data_pcm16 = m->s->pcm;
    m->wb.nsamples = m->s->frames;
    ndspChnWaveBufAdd(ch, &m->wb);
    return SUCCESS;
}

static int m_stop(media_t *m)
{
    if (m->midi) dbg("media: %p stop midi h%d", m, m->song);
    chan_stop(m);
    if (m->playing) post(m, MM_STATUS_DONE);
    m->playing = false;
    return SUCCESS;
}

static int m_pause(media_t *m) { if (m->ch >= 0) ndspChnSetPaused(m->ch, true); return SUCCESS; }
static int m_resume(media_t *m) { if (m->ch >= 0) ndspChnSetPaused(m->ch, false); return SUCCESS; }
static int m_state(media_t *m, bool *changing)
{
    if (changing) *changing = false;
    return m->playing ? MM_STATE_PLAY : MM_STATE_IDLE;
}
static int m_total(media_t *m) { return m->s ? m->s->frames * 1000 / m->s->rate : 0; }

void *media_new(u32 cls)
{
    media_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (nmidi < 16) midis[nmidi++] = m;
    m->vt = vt_media;
    m->cls = cls;
    m->ch = -1;
    m->vol = 1;
    return m;
}

void media_init(bool audio, bool music)
{
    void **v = vt_media;
    vt_fill(v, 14);
    v[0] = m_addref; v[1] = m_release; v[2] = m_qi; v[3] = m_regnotify; v[4] = m_setparm;
    v[5] = m_getparm; v[6] = m_play; v[8] = m_stop; v[10] = m_pause; v[11] = m_resume;
    v[12] = m_total; v[13] = m_state;
    if (!audio) { dbg("media: audio off (R held at boot)"); return; }
    dsp_ok = R_SUCCEEDED(ndspInit());
    if (!dsp_ok) dbg("media: ndspInit failed, no dspfirm.cdc? sound off");
    else {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        if (music) midi_init();
        else dbg("media: music off (L held at boot)");
    }
}

void media_exit(void)
{
    if (dsp_ok) {
        midi_exit();
        dbg("media: ndspExit");
        ndspExit();
        dbg("media: audio down");
    }
    for (int i = 0; i < nsamples; i++) linearFree(samples[i].pcm);
}

void media_update(void)
{
    for (int i = 0; i < nmidi; i++) {
        media_t *m = midis[i];
        if (m->song && midi_finished(m->song)) { m->song = 0; m->playing = false; post(m, MM_STATUS_DONE); }
    }
    for (int i = 0; i < 24; i++) {
        media_t *m = chans[i];
        if (!m || m->wb.status != NDSP_WBUF_DONE) continue;
        chans[i] = NULL;
        m->ch = -1;
        m->playing = false;
        post(m, MM_STATUS_DONE);
    }
    // callbacks can stop/release media, so copy the batch out first
    static note_t batch[32];
    int n = nnotes;
    memcpy(batch, notes, n * sizeof *batch);
    nnotes = 0;
    running = batch;
    nrunning = n;
    for (int i = 0; i < n; i++) {
        media_t *m = batch[i].m;
        if (!m) continue;
        // clsMedia, pIMedia, nCmd, nSubCmd, nStatus, pCmdData, dwSize
        notify_blk[0] = m->cls; notify_blk[1] = (u32)m; notify_blk[2] = MM_CMD_PLAY;
        notify_blk[3] = 0; notify_blk[4] = batch[i].status; notify_blk[5] = 0; notify_blk[6] = 0;
        if (m->notify) m->notify(m->user, notify_blk);
    }
    nrunning = 0;
}
