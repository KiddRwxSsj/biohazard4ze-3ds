// usage: gcc -O2 -Isource/tsf re/midiport.c -o midiport -lm
//        midiport bgm.sf2 song.mid out.raw 1   (1 = events on their sample, 0 = old per-chunk)
// host copy of midi.c's render loop: 1024 frame chunks, events either up front (old) or on their
// sample (new). same tsf build flags, voice pool, gain, bank 124/125 handling and 92->89 remap
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
#include "tsf.h"
#include "tml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RATE 22050
#define CHUNK 1024
static unsigned short drums;
static void apply(tsf *s, const tml_message *m)
{
    int ch = m->channel, drum = drums >> ch & 1;
    switch (m->type) {
    case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(s, ch, !drum && m->program == 92 ? 89 : m->program, drum); break;
    case TML_NOTE_ON: tsf_channel_note_on(s, ch, m->key, m->velocity / 127.0f); break;
    case TML_NOTE_OFF: tsf_channel_note_off(s, ch, m->key); break;
    case TML_PITCH_BEND: tsf_channel_set_pitchwheel(s, ch, m->pitch_bend); break;
    case TML_CONTROL_CHANGE:
        if (m->control == 0 && (m->control_value == 124 || m->control_value == 125))
            drums = (drums & ~(1 << ch)) | (m->control_value == 125) << ch;
        tsf_channel_midi_control(s, ch, m->control, m->control_value);
        break;
    }
}
int main(int argc, char **argv)
{
    tsf *font = tsf_load_filename(argv[1]);
    tml_message *song = tml_load_filename(argv[2]);
    int exact = atoi(argv[4]);
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, RATE, 0);
    tsf *s = tsf_copy(font);
    tsf_set_max_voices(s, 32);
    tsf_channel_set_bank_preset(s, 9, 128, 0);
    drums = 1 << 9;
    tsf_set_volume(s, powf(10.0f, -8.0f / 20));
    unsigned int len; tml_get_info(song, 0, 0, 0, 0, &len);
    FILE *o = fopen(argv[3], "wb");
    static short out[CHUNK * 2];
    double t = 0, err = 0, errmax = 0; int nev = 0;
    tml_message *cur = song;
    for (int c = 0; c < (int)(len / 1000.0 * RATE / CHUNK) + 1; c++) {
        memset(out, 0, sizeof out);
        double t0 = t; t += CHUNK * 1000.0 / RATE;
        int pos = 0;
        for (; cur && cur->time <= t; cur = cur->next) {
            int at = exact ? (int)((cur->time - t0) * RATE / 1000.0) : 0;
            if (at > CHUNK) at = CHUNK;
            if (at > pos) { tsf_render_short(s, out + pos * 2, at - pos, 1); pos = at; }
            if (cur->type == TML_NOTE_ON) { double e = cur->time - (t0 + at * 1000.0 / RATE); err += e; if (e > errmax) errmax = e; nev++; }
            apply(s, cur);
        }
        if (pos < CHUNK) tsf_render_short(s, out + pos * 2, CHUNK - pos, 1);
        fwrite(out, 4, CHUNK, o);
    }
    fclose(o);
    fprintf(stderr, "%s: note onsets early by avg %.1f ms, max %.1f ms\n", exact ? "on-sample" : "per-chunk", err / nev, errmax);
    return 0;
}
