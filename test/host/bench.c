// Throughput benchmark for the voice engine.
//
// Renders a dense arrangement and reports the realtime factor: how many
// seconds of audio come out per second of CPU. The Tanmatsu needs > 1.0 with
// enough margin left over for the display and the keyboard, so treat anything
// under about 4x here as too slow for the badge.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "sd_synth.h"

#define SR      48000
#define BLOCK   256
#define SECONDS 20

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static sd_note_t drum_note(sd_drum_t d, float gain, float pan) {
    sd_note_t n = {0};
    n.drum      = d;
    n.gain      = gain;
    n.pan       = pan;
    return n;
}

int main(void) {
    sd_synth_t* s = sd_synth_create(SR);
    if (!s) {
        return 1;
    }
    sd_synth_set_delay(s, 1, 0.1875f, 0.45f, 0.35f);

    float* out = calloc(BLOCK * 2, sizeof(float));
    if (!out) {
        return 1;
    }

    uint32_t const total   = SR * SECONDS;
    double const   spb     = 0.5;  // 120 bpm
    int            step    = 0;
    int            peak    = 0;
    long           notes   = 0;
    double         started = now_s();

    for (uint32_t pos = 0; pos < total; pos += BLOCK) {
        double t_start = (double)pos / SR;
        double t_end   = (double)(pos + BLOCK) / SR;
        while ((double)step * spb / 4.0 < t_end) {
            double t = (double)step * spb / 4.0;
            if (t >= t_start) {
                int s16 = step % 16;
                // A busy pattern: kick, hats, snare, clap and two synth parts
                if (s16 % 4 == 0) {
                    sd_note_t n = drum_note(SD_DRUM_BD, 0.9f, 0.5f);
                    sd_synth_note_on(s, &n);
                    notes++;
                }
                if (s16 % 2 == 0) {
                    sd_note_t n = drum_note(SD_DRUM_HH, 0.35f, 0.65f);
                    sd_synth_note_on(s, &n);
                    notes++;
                }
                if (s16 == 4 || s16 == 12) {
                    sd_note_t n = drum_note(SD_DRUM_SD, 0.7f, 0.5f);
                    sd_synth_note_on(s, &n);
                    notes++;
                }
                if (s16 == 14) {
                    sd_note_t n = drum_note(SD_DRUM_CP, 0.5f, 0.35f);
                    sd_synth_note_on(s, &n);
                    notes++;
                }
                if (s16 % 8 == 0) {
                    sd_note_t n = {0};
                    n.wave      = SD_WAVE_SAW;
                    n.freq      = 55.0f * (1.0f + 0.25f * (float)((step / 16) % 4));
                    n.gain      = 0.3f;
                    n.pan       = 0.5f;
                    n.attack    = 0.005f;
                    n.decay     = 0.08f;
                    n.sustain   = 0.6f;
                    n.release   = 0.12f;
                    n.dur       = 0.35f;
                    n.cutoff    = 700.0f;
                    n.resonance = 0.55f;
                    n.orbit     = 1;
                    sd_synth_note_on(s, &n);
                    notes++;
                }
                if (s16 % 4 == 2) {
                    sd_note_t n = {0};
                    n.wave      = SD_WAVE_SQUARE;
                    n.freq      = 440.0f;
                    n.gain      = 0.12f;
                    n.pan       = 0.4f;
                    n.attack    = 0.004f;
                    n.decay     = 0.06f;
                    n.sustain   = 0.4f;
                    n.release   = 0.10f;
                    n.dur       = 0.20f;
                    n.cutoff    = 2400.0f;
                    n.resonance = 0.3f;
                    n.orbit     = 1;
                    sd_synth_note_on(s, &n);
                    notes++;
                }
            }
            step++;
        }
        sd_synth_render(s, out, BLOCK);
        int active = sd_synth_active_voices(s);
        if (active > peak) {
            peak = active;
        }
    }

    double elapsed = now_s() - started;
    double rt      = (double)SECONDS / elapsed;
    // What one 256 frame block would cost against its own 5.33 ms of wall time
    double block_budget_pct = 100.0 / rt;

    printf("rendered %d s of audio in %.3f s cpu\n", SECONDS, elapsed);
    printf("realtime factor : %.2fx\n", rt);
    printf("block load      : %.1f%% of budget\n", block_budget_pct);
    printf("peak voices     : %d   notes: %ld\n", peak, notes);

    free(out);
    sd_synth_destroy(s);
    return 0;
}
