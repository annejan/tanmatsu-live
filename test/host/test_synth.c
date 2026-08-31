// Host test for the voice engine: renders a short beat, checks the output is
// finite, in range, and actually loud enough to be audible, then writes a wav
// so it can be listened to.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd_synth.h"

#define SR     48000
#define BLOCK  256
#define FRAMES (SR * 4)

static int failures = 0;

static void check(int cond, char const* what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void write_wav(char const* path, float const* lr, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        return;
    }
    uint32_t data_bytes = frames * 2 * 2;
    uint32_t chunk      = 36 + data_bytes;
    uint16_t ch = 2, bits = 16, fmt = 1;
    uint32_t rate = SR, byte_rate = SR * 4;
    uint16_t align = 4;
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t sixteen = 16;
    fwrite(&sixteen, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    for (uint32_t i = 0; i < frames * 2; i++) {
        float   x = lr[i] > 1.0f ? 1.0f : (lr[i] < -1.0f ? -1.0f : lr[i]);
        int16_t s = (int16_t)lrintf(x * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static sd_note_t drum_note(sd_drum_t d, float gain) {
    sd_note_t n = {0};
    n.drum      = d;
    n.gain      = gain;
    n.pan       = 0.5f;
    return n;
}

int main(void) {
    check(sd_drum_from_name("bd") == SD_DRUM_BD, "drum name bd");
    check(sd_drum_from_name("nope") == SD_DRUM_NONE, "drum name unknown");
    check(sd_wave_from_name("saw") == SD_WAVE_SAW, "wave name saw");

    sd_synth_t* s = sd_synth_create(SR);
    check(s != NULL, "synth created");
    if (!s) {
        return 1;
    }
    sd_synth_set_delay(s, 1, 0.1875f, 0.45f, 0.35f);

    float* out = calloc(FRAMES * 2, sizeof(float));
    check(out != NULL, "output buffer");
    if (!out) {
        return 1;
    }

    // Four bars at 120 bpm: kick on every beat, hats on eighths, snare on 2/4,
    // plus a bass line on orbit 1 so the delay path is exercised.
    double  spb   = 0.5;  // seconds per beat
    int     step  = 0;
    int     peak_voices = 0;
    float const bass_hz[4] = {55.0f, 55.0f, 73.42f, 82.41f};

    for (uint32_t pos = 0; pos < FRAMES; pos += BLOCK) {
        double t_start = (double)pos / SR;
        double t_end   = (double)(pos + BLOCK) / SR;
        // Trigger every sixteenth that starts inside this block
        while ((double)step * spb / 4.0 < t_end) {
            double t = (double)step * spb / 4.0;
            if (t >= t_start) {
                int s16 = step % 16;
                if (s16 % 4 == 0) {
                    sd_note_t n = drum_note(SD_DRUM_BD, 0.9f);
                    sd_synth_note_on(s, &n);
                }
                if (s16 % 2 == 0) {
                    sd_note_t n = drum_note(SD_DRUM_HH, s16 % 4 == 0 ? 0.25f : 0.4f);
                    n.pan       = 0.65f;
                    sd_synth_note_on(s, &n);
                }
                if (s16 == 4 || s16 == 12) {
                    sd_note_t n = drum_note(SD_DRUM_SD, 0.7f);
                    sd_synth_note_on(s, &n);
                }
                if (s16 == 14) {
                    sd_note_t n = drum_note(SD_DRUM_CP, 0.5f);
                    n.pan       = 0.35f;
                    sd_synth_note_on(s, &n);
                }
                if (s16 % 8 == 0) {
                    sd_note_t n = {0};
                    n.wave      = SD_WAVE_SAW;
                    n.freq      = bass_hz[(step / 16) % 4];
                    n.gain      = 0.35f;
                    n.pan       = 0.5f;
                    n.attack    = 0.005f;
                    n.decay     = 0.08f;
                    n.sustain   = 0.5f;
                    n.release   = 0.12f;
                    n.dur       = 0.35f;
                    n.cutoff    = 700.0f;
                    n.resonance = 0.55f;
                    n.shape     = 0.2f;
                    n.orbit     = 1;
                    sd_synth_note_on(s, &n);
                }
            }
            step++;
        }
        uint32_t n = FRAMES - pos < BLOCK ? FRAMES - pos : BLOCK;
        sd_synth_render(s, out + (size_t)pos * 2, n);
        int active = sd_synth_active_voices(s);
        if (active > peak_voices) {
            peak_voices = active;
        }
    }

    float peak = 0.0f;
    double rms = 0.0;
    int    bad = 0;
    for (uint32_t i = 0; i < FRAMES * 2; i++) {
        float x = out[i];
        if (!isfinite(x)) {
            bad++;
            continue;
        }
        if (fabsf(x) > peak) {
            peak = fabsf(x);
        }
        rms += (double)x * x;
    }
    rms = sqrt(rms / (FRAMES * 2));

    check(bad == 0, "no NaN or inf in output");
    check(peak <= 1.0f, "output within -1..1");
    check(peak > 0.2f, "output actually audible");
    check(rms > 0.02f, "output has body");
    check(peak_voices <= SD_MAX_VOICES, "voice count within limit");

    printf("peak %.3f  rms %.4f  peak voices %d\n", peak, rms, peak_voices);
    write_wav("beat.wav", out, FRAMES);
    printf("wrote beat.wav (%d s)\n", FRAMES / SR);

    sd_synth_panic(s);
    sd_synth_render(s, out, BLOCK);
    float after = 0.0f;
    for (uint32_t i = 0; i < BLOCK * 2; i++) {
        if (fabsf(out[i]) > after) {
            after = fabsf(out[i]);
        }
    }
    check(after == 0.0f, "panic silences everything");
    check(sd_synth_active_voices(s) == 0, "panic frees all voices");

    free(out);
    sd_synth_destroy(s);
    printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
