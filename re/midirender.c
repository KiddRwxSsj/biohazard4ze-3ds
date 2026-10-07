// host test: render a midi with the vendored tsf to a 16 bit wav
#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "tsf.h"
#include "tml.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    int rate = 22050;
    tsf *f = tsf_load_filename(argv[1]);
    tml_message *song = tml_load_filename(argv[2]);
    if (!f || !song) { printf("load fail\n"); return 1; }
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, rate, argc > 4 ? atof(argv[4]) : 0.0f);
    tsf_channel_set_bank_preset(f, 9, 128, 0);
    if (argc > 5) tsf_set_max_voices(f, atoi(argv[5]));
    FILE *o = fopen(argv[3], "wb");
    unsigned int len; tml_get_info(song, 0, 0, 0, 0, &len);
    int total = (int)((len / 1000.0 + 2) * rate);
    fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\2\0", 1, 24, o);
    int r = rate, br = rate * 4; short ba = 4, bits = 16;
    fwrite(&r, 4, 1, o); fwrite(&br, 4, 1, o); fwrite(&ba, 2, 1, o); fwrite(&bits, 2, 1, o);
    fwrite("data", 1, 4, o); int ds = total * 4; fwrite(&ds, 4, 1, o);
    short buf[2 * 64];
    double t = 0, sum = 0; int peak = 0, maxv = 0;
    tml_message *m = song;
    for (int done = 0; done < total; done += 64) {
        for (t += 64 * 1000.0 / rate; m && m->time <= t; m = m->next) {
            switch (m->type) {
            case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(f, m->channel, m->program, m->channel == 9); break;
            case TML_NOTE_ON: tsf_channel_note_on(f, m->channel, m->key, m->velocity / 127.0f); break;
            case TML_NOTE_OFF: tsf_channel_note_off(f, m->channel, m->key); break;
            case TML_PITCH_BEND: tsf_channel_set_pitchwheel(f, m->channel, m->pitch_bend); break;
            case TML_CONTROL_CHANGE: tsf_channel_midi_control(f, m->channel, m->control, m->control_value); break;
            }
        }
        tsf_render_short(f, buf, 64, 0);
        { int a = tsf_active_voice_count(f); if (a > maxv) maxv = a; }
        for (int i = 0; i < 128; i++) { sum += buf[i] * (double)buf[i]; if (abs(buf[i]) > peak) peak = abs(buf[i]); }
        fwrite(buf, 2, 128, o);
    }
    fclose(o);
    printf("%s: %.1fs rms %.0f peak %d voices %d\n", argv[2], len / 1000.0, sqrt(sum / (total * 2.0)), peak, maxv);
    return 0;
}
