#include "app_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/audio.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_synth.h"

static char const TAG[] = "audio";

#define SAMPLE_RATE 48000
#define BLOCK       256  // 5.3 ms, short enough that a keystroke feels instant

typedef struct {
    uint8_t on;    // 0 = rest
    float   gain;  // per step, already includes the line gain
    float   freq;  // Hz, ignored for drum lines
} seq_step_t;

typedef struct {
    int        editor_line;
    char       name[24];
    sd_drum_t  drum;
    sd_wave_t  wave;
    float      cutoff, resonance, shape;
    int        orbit;
    int        nsteps;
    seq_step_t step[APP_SEQ_MAX_STEPS];
} seq_line_t;

typedef struct {
    seq_line_t line[APP_SEQ_MAX_LINES];
    int        nlines;
} seq_prog_t;

static sd_synth_t*      synth   = NULL;
static i2s_chan_handle_t i2s    = NULL;
static SemaphoreHandle_t prog_mutex = NULL;

static seq_prog_t prog;         // read by the audio task
static seq_prog_t prog_scratch;  // written by the UI task while parsing

static volatile bool  playing   = false;
static volatile float bpm       = 120.0f;
static volatile float master    = 0.8f;
static volatile int64_t cur_step = 0;
static volatile int   voices    = 0;
static volatile float load      = 0.0f;

static char parse_error[96] = "";

static float  fbuf[BLOCK * 2];
static int16_t ibuf[BLOCK * 2];

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static bool is_rest_char(char c) {
    return c == '.' || c == '~' || c == '-' || c == '_';
}

// Note names in the Strudel convention, where c3 is midi 48.
static bool parse_note(char const* tok, size_t len, float* out_hz) {
    if (len == 0) {
        return false;
    }
    static int8_t const semis[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    char                c        = tok[0] | 0x20;
    if (c < 'a' || c > 'g') {
        return false;
    }
    int    midi = semis[c - 'a'];
    size_t i    = 1;
    if (i < len && (tok[i] == '#' || tok[i] == 's')) {
        midi++;
        i++;
    } else if (i < len && tok[i] == 'b') {
        midi--;
        i++;
    }
    int octave = 3;
    if (i < len && tok[i] >= '0' && tok[i] <= '9') {
        octave = tok[i] - '0';
        i++;
    }
    if (i != len) {
        return false;
    }
    // c3 == midi 48, so octave n starts at 12*(n+1)
    midi += 12 * (octave + 1);
    *out_hz = 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
    return true;
}

// A single step, either one character of a grid or one whitespace separated
// token. Returns false for a rest.
static bool parse_step(char const* tok, size_t len, bool melodic, float base_hz, seq_step_t* out) {
    if (len == 0 || is_rest_char(tok[0])) {
        return false;
    }
    out->on   = 1;
    out->gain = 1.0f;
    out->freq = base_hz;

    if (!melodic) {
        // Drums: x is a hit, X is an accent, 1..9 sets the velocity
        if (tok[0] == 'X') {
            out->gain = 1.3f;
        } else if (tok[0] >= '1' && tok[0] <= '9') {
            out->gain = (float)(tok[0] - '0') / 9.0f;
        }
        return true;
    }

    if (tok[0] == 'x' || tok[0] == 'X') {
        out->gain = tok[0] == 'X' ? 1.3f : 1.0f;
        return true;
    }
    float hz;
    if (parse_note(tok, len, &hz)) {
        out->freq = hz;
        return true;
    }
    // A bare number is a semitone offset from the line's base note
    if (len == 1 && tok[0] >= '0' && tok[0] <= '9') {
        out->freq = base_hz * powf(2.0f, (float)(tok[0] - '0') / 12.0f);
        return true;
    }
    char buf[8];
    if (len < sizeof(buf)) {
        memcpy(buf, tok, len);
        buf[len] = 0;
        char* end = NULL;
        double d  = strtod(buf, &end);
        if (end && *end == 0) {
            out->freq = base_hz * powf(2.0f, (float)d / 12.0f);
            return true;
        }
    }
    out->on = 0;
    return false;
}

static void trim(char const** s, size_t* len) {
    while (*len && (**s == ' ' || **s == '\t')) {
        (*s)++;
        (*len)--;
    }
    while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' || (*s)[*len - 1] == '\r')) {
        (*len)--;
    }
}

// One editor line into one sequencer part. Returns false if the line is blank,
// a comment, or a directive that was handled here.
static bool parse_line(char const* src, size_t len, int editor_line, seq_line_t* out, char* err, size_t errlen) {
    trim(&src, &len);
    if (len == 0 || src[0] == '#') {
        return false;
    }

    // Split the head token from the rest
    size_t hlen = 0;
    while (hlen < len && src[hlen] != ' ' && src[hlen] != '\t') {
        hlen++;
    }
    char head[24];
    if (hlen >= sizeof(head)) {
        snprintf(err, errlen, "line %d: name too long", editor_line + 1);
        return false;
    }
    memcpy(head, src, hlen);
    head[hlen] = 0;
    char const* rest    = src + hlen;
    size_t      restlen = len - hlen;
    trim(&rest, &restlen);

    if (strcmp(head, "bpm") == 0) {
        float v = strtof(rest, NULL);
        if (v >= 20.0f && v <= 400.0f) {
            bpm = v;
        } else {
            snprintf(err, errlen, "line %d: bpm out of range", editor_line + 1);
        }
        return false;
    }
    if (strcmp(head, "gain") == 0) {
        float v = strtof(rest, NULL);
        if (v >= 0.0f && v <= 2.0f) {
            master = v;
            sd_synth_set_master(synth, v);
        }
        return false;
    }
    if (strcmp(head, "delay") == 0) {
        int   orbit = 1;
        float t = 0.1875f, fb = 0.4f, mix = 0.3f;
        sscanf(rest, "%d %f %f %f", &orbit, &t, &fb, &mix);
        sd_synth_set_delay(synth, orbit, t, fb, mix);
        return false;
    }

    // name[:gain[:cutoff[:resonance]]]
    float line_gain = 0.8f;
    char* colon     = strchr(head, ':');
    if (colon) {
        *colon = 0;
        float g = 0, c = 0, r = 0;
        int   n = sscanf(colon + 1, "%f:%f:%f", &g, &c, &r);
        if (n >= 1) {
            line_gain = g;
        }
        if (n >= 2) {
            out->cutoff = c;
        }
        if (n >= 3) {
            out->resonance = r;
        }
    }

    out->editor_line = editor_line;
    snprintf(out->name, sizeof(out->name), "%s", head);
    out->drum = sd_drum_from_name(head);
    out->wave        = SD_WAVE_COUNT;
    bool melodic     = false;
    if (out->drum == SD_DRUM_NONE) {
        out->wave = sd_wave_from_name(head);
        if (out->wave == SD_WAVE_COUNT) {
            snprintf(err, errlen, "line %d: unknown sound '%s'", editor_line + 1, head);
            return false;
        }
        melodic = true;
        if (out->cutoff <= 0.0f) {
            out->cutoff = 2500.0f;
        }
    }
    out->orbit = melodic ? 1 : 0;

    if (restlen == 0) {
        snprintf(err, errlen, "line %d: no steps", editor_line + 1);
        return false;
    }

    float base_hz = 65.406f;  // c2, a comfortable bass root
    bool  spaced  = memchr(rest, ' ', restlen) != NULL;
    out->nsteps   = 0;

    if (spaced) {
        size_t i = 0;
        while (i < restlen && out->nsteps < APP_SEQ_MAX_STEPS) {
            while (i < restlen && rest[i] == ' ') {
                i++;
            }
            size_t start = i;
            while (i < restlen && rest[i] != ' ') {
                i++;
            }
            if (i == start) {
                break;
            }
            seq_step_t st = {0};
            parse_step(rest + start, i - start, melodic, base_hz, &st);
            st.gain *= line_gain;
            out->step[out->nsteps++] = st;
        }
    } else {
        for (size_t i = 0; i < restlen && out->nsteps < APP_SEQ_MAX_STEPS; i++) {
            seq_step_t st = {0};
            parse_step(rest + i, 1, melodic, base_hz, &st);
            st.gain *= line_gain;
            out->step[out->nsteps++] = st;
        }
    }

    if (out->nsteps == 0) {
        snprintf(err, errlen, "line %d: no steps", editor_line + 1);
        return false;
    }
    return true;
}

void app_audio_eval(char const* text) {
    if (!synth) {
        return;
    }
    char err[sizeof(parse_error)] = "";
    memset(&prog_scratch, 0, sizeof(prog_scratch));

    char const* p           = text;
    int         editor_line = 0;
    while (*p && prog_scratch.nlines < APP_SEQ_MAX_LINES) {
        char const* eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);
        // Parsing works on a terminated copy, so sscanf and strtof can never
        // run past the end of the line into the next one.
        char buf[APP_SEQ_MAX_STEPS * 5 + 32];
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, p, len);
        buf[len] = 0;

        seq_line_t line = {0};
        if (parse_line(buf, len, editor_line, &line, err, sizeof(err))) {
            prog_scratch.line[prog_scratch.nlines++] = line;
        }
        editor_line++;
        if (!eol) {
            break;
        }
        p = eol + 1;
    }

    xSemaphoreTake(prog_mutex, portMAX_DELAY);
    prog = prog_scratch;
    xSemaphoreGive(prog_mutex);

    strncpy(parse_error, err, sizeof(parse_error) - 1);
    parse_error[sizeof(parse_error) - 1] = 0;
    ESP_LOGI(TAG, "eval: %d parts%s%s", prog.nlines, err[0] ? ", " : "", err);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

static void trigger_step(int64_t step, float step_seconds) {
    for (int l = 0; l < prog.nlines; l++) {
        seq_line_t const* ln = &prog.line[l];
        if (ln->nsteps <= 0) {
            continue;
        }
        int idx = (int)(((step % ln->nsteps) + ln->nsteps) % ln->nsteps);
        seq_step_t const* st = &ln->step[idx];
        if (!st->on) {
            continue;
        }
        sd_note_t n = {0};
        n.drum      = ln->drum;
        n.wave      = ln->wave == SD_WAVE_COUNT ? SD_WAVE_SINE : ln->wave;
        n.freq      = st->freq;
        n.gain      = st->gain;
        n.pan       = 0.5f;
        n.orbit     = ln->orbit;
        if (ln->drum == SD_DRUM_NONE) {
            n.attack    = 0.004f;
            n.decay     = 0.09f;
            n.sustain   = 0.6f;
            n.release   = 0.14f;
            n.dur       = step_seconds * 0.85f;
            n.cutoff    = ln->cutoff;
            n.resonance = ln->resonance > 0.0f ? ln->resonance : 0.35f;
            n.shape     = ln->shape;
        }
        sd_synth_note_on(synth, &n);
    }
}

static void audio_task(void* arg) {
    double samples_to_next = 0.0;
    int64_t step           = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        xSemaphoreTake(prog_mutex, portMAX_DELAY);
        float b = bpm;
        if (b < 20.0f) {
            b = 20.0f;
        }
        double samples_per_step = (double)SAMPLE_RATE * 60.0 / (b * 4.0);  // sixteenths
        float  step_seconds     = (float)(samples_per_step / SAMPLE_RATE);
        bool   play             = playing;

        uint32_t remaining = BLOCK;
        float*   dst       = fbuf;
        while (remaining > 0) {
            uint32_t chunk = remaining;
            if (play) {
                if (samples_to_next <= 0.0) {
                    trigger_step(step, step_seconds);
                    step++;
                    cur_step = step;
                    samples_to_next += samples_per_step;
                    continue;
                }
                uint32_t until = (uint32_t)samples_to_next;
                if (until == 0) {
                    until = 1;
                }
                if (until < chunk) {
                    chunk = until;
                }
            }
            sd_synth_render(synth, dst, chunk);
            if (play) {
                samples_to_next -= chunk;
            }
            dst += chunk * 2;
            remaining -= chunk;
        }
        if (!play) {
            samples_to_next = 0.0;
            step            = 0;
            cur_step        = 0;
        }
        xSemaphoreGive(prog_mutex);

        for (uint32_t i = 0; i < BLOCK * 2; i++) {
            float x = fbuf[i];
            if (x > 1.0f) {
                x = 1.0f;
            }
            if (x < -1.0f) {
                x = -1.0f;
            }
            ibuf[i] = (int16_t)(x * 32767.0f);
        }

        voices = sd_synth_active_voices(synth);
        // Budget is the wall clock time this block of audio will take to play
        int64_t spent = esp_timer_get_time() - t0;
        load          = (float)spent / (1000000.0f * (float)BLOCK / (float)SAMPLE_RATE);

        size_t written = 0;
        i2s_channel_write(i2s, ibuf, sizeof(ibuf), &written, portMAX_DELAY);
    }
}

// The voice state and the per block scratch are touched every sample, so they
// go in internal RAM. The delay lines are far too large for that and would
// crowd out the framebuffer, so they stay in PSRAM.
static void* audio_alloc(size_t bytes, bool hot) {
    void* p = NULL;
    if (hot) {
        p = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = calloc(1, bytes);
    }
    return p;
}

static void audio_free(void* p) {
    heap_caps_free(p);
}

esp_err_t app_audio_start(void) {
    synth = sd_synth_create_ex(SAMPLE_RATE, audio_alloc, audio_free);
    if (!synth) {
        ESP_LOGE(TAG, "Failed to allocate the synth");
        return ESP_ERR_NO_MEM;
    }
    sd_synth_set_master(synth, master);
    sd_synth_set_delay(synth, 1, 0.1875f, 0.4f, 0.25f);

    prog_mutex = xSemaphoreCreateMutex();
    if (!prog_mutex) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = bsp_audio_get_i2s_handle(&i2s);
    if (res != ESP_OK || !i2s) {
        ESP_LOGE(TAG, "No I2S handle: %d", res);
        return res == ESP_OK ? ESP_FAIL : res;
    }
    res = bsp_audio_set_rate(SAMPLE_RATE);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Could not set the sample rate: %d", res);
    }
    bsp_audio_set_volume(80.0f);
    bsp_audio_set_amplifier(true);

    // Core 1, above the UI, so a redraw can never starve the DMA
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 10, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the audio task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_audio_set_playing(bool p) {
    playing = p;
    if (!p) {
        sd_synth_panic(synth);
    }
}

bool  app_audio_is_playing(void) { return playing; }
void  app_audio_set_bpm(float v) { bpm = v < 20.0f ? 20.0f : (v > 400.0f ? 400.0f : v); }
float app_audio_get_bpm(void) { return bpm; }
void  app_audio_set_master(float g) {
    master = g;
    sd_synth_set_master(synth, g);
}
float app_audio_get_master(void) { return master; }
int64_t app_audio_get_step(void) { return cur_step; }
int   app_audio_get_voices(void) { return voices; }
float app_audio_get_load(void) { return load; }
char const* app_audio_get_error(void) { return parse_error; }
int app_audio_get_line_count(void) { return prog.nlines; }

int app_audio_get_part_count(void) {
    return prog.nlines;
}

int app_audio_get_part_steps(int part) {
    if (part < 0 || part >= prog.nlines) {
        return 0;
    }
    return prog.line[part].nsteps;
}

bool app_audio_get_part_step_on(int part, int step) {
    if (part < 0 || part >= prog.nlines) {
        return false;
    }
    seq_line_t const* ln = &prog.line[part];
    if (step < 0 || step >= ln->nsteps) {
        return false;
    }
    return ln->step[step].on != 0;
}

int app_audio_get_part_editor_line(int part) {
    if (part < 0 || part >= prog.nlines) {
        return -1;
    }
    return prog.line[part].editor_line;
}

void app_audio_get_part_name(int part, char* out, size_t len) {
    if (!out || len == 0) {
        return;
    }
    if (part < 0 || part >= prog.nlines) {
        out[0] = 0;
        return;
    }
    snprintf(out, len, "%s", prog.line[part].name);
}

int app_audio_get_line_steps(int editor_line) {
    for (int i = 0; i < prog.nlines; i++) {
        if (prog.line[i].editor_line == editor_line) {
            return prog.line[i].nsteps;
        }
    }
    return 0;
}
