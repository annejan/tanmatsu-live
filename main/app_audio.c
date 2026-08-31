#include "app_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_mini.h"
#include "sd_pattern.h"
#include "sd_synth.h"

static char const TAG[] = "audio";

#define SAMPLE_RATE 48000
#define BLOCK       256  // 5.3 ms, short enough that a keystroke feels instant
#define ARENA_BYTES (28 * 1024)
#define MAX_EVENTS  64
#define MAX_HAPS    48
#define LANE_STEPS  16

// A cycle is one bar of four beats, so cycles per second is bpm/240. Keeping
// bpm an integer makes the whole time base exact rational arithmetic.
#define CYCLE_DEN ((int64_t)240 * SAMPLE_RATE)

typedef struct {
    int       editor_line;
    char      name[24];
    sd_drum_t drum;
    sd_wave_t wave;
    bool      melodic;
    float     gain, cutoff, resonance;
    int       orbit;
    sd_pat_t* pat;
    uint16_t  mask;  // which of 16 slots have an onset, for the UI lanes
} seq_line_t;

typedef struct {
    seq_line_t line[APP_SEQ_MAX_LINES];
    int        nlines;
} seq_prog_t;

static sd_synth_t*       synth      = NULL;
static i2s_chan_handle_t i2s        = NULL;
static SemaphoreHandle_t prog_mutex = NULL;

// Patterns live in an arena. Parsing fills the spare one and the two are
// swapped under the mutex, so the audio task never reads a half built tree.
static uint8_t    arena_mem[2][ARENA_BYTES];
static sd_arena_t arenas[2];
static int        arena_live = 0;

static seq_prog_t prog;
static seq_prog_t prog_scratch;

static volatile bool    playing  = false;
static volatile int     bpm_i    = 124;
static volatile float   master   = 0.8f;
static volatile int64_t cur_step = 0;
static volatile int     voices   = 0;
static volatile float   load     = 0.0f;

static char parse_error[96] = "";

static float   fbuf[BLOCK * 2];
static int16_t ibuf[BLOCK * 2];

// Audio task only, so file scope rather than a large stack frame
static sd_hap_t query_haps[MAX_HAPS];
static struct {
    uint32_t off;  // samples into the block
    int16_t  line;
    sd_val_t bare;
} events[MAX_EVENTS];

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static void trim(char const** s, size_t* len) {
    while (*len && (**s == ' ' || **s == '\t')) {
        (*s)++;
        (*len)--;
    }
    while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' || (*s)[*len - 1] == '\r')) {
        (*len)--;
    }
}

// The old step grid, "x...x...", stays valid: it is just a compact way of
// writing a sequence, so it is turned into the same pattern a sequence makes.
static bool looks_like_grid(char const* s, size_t len) {
    if (len < 2) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            return false;
        }
        bool grid = c == 'x' || c == 'X' || c == '.' || c == '~' || c == '-' || c == '_' || (c >= '0' && c <= '9');
        if (!grid) {
            return false;
        }
    }
    return true;
}

static sd_pat_t* grid_to_pattern(sd_arena_t* a, char const* s, size_t len) {
    sd_pat_t* kids[64];
    int       n = 0;
    for (size_t i = 0; i < len && n < 64; i++) {
        char c = s[i];
        if (c == '.' || c == '~' || c == '-' || c == '_') {
            kids[n++] = sd_silence(a);
        } else if (c >= '1' && c <= '9') {
            kids[n++] = sd_pure_num(a, (double)(c - '0'));
        } else {
            char w[2] = {c, 0};
            kids[n++] = sd_pure_word(a, w, -1);
        }
    }
    return n ? sd_fastcat(a, kids, n) : sd_silence(a);
}

static bool parse_line(char const* src, size_t len, int editor_line, sd_arena_t* a, seq_line_t* out, char* err,
                       size_t errlen) {
    trim(&src, &len);
    if (len == 0 || src[0] == '#') {
        return false;
    }

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
        int v = (int)strtol(rest, NULL, 10);
        if (v >= 20 && v <= 400) {
            bpm_i = v;
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

    out->gain = 0.8f;
    char* colon = strchr(head, ':');
    if (colon) {
        *colon  = 0;
        float g = 0, c = 0, r = 0;
        int   n = sscanf(colon + 1, "%f:%f:%f", &g, &c, &r);
        if (n >= 1) {
            out->gain = g;
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
    out->wave = SD_WAVE_COUNT;
    if (out->drum == SD_DRUM_NONE) {
        out->wave = sd_wave_from_name(head);
        if (out->wave == SD_WAVE_COUNT) {
            snprintf(err, errlen, "line %d: unknown sound '%s'", editor_line + 1, head);
            return false;
        }
        out->melodic = true;
        if (out->cutoff <= 0.0f) {
            out->cutoff = 2500.0f;
        }
    }
    out->orbit = out->melodic ? 1 : 0;

    if (restlen == 0) {
        snprintf(err, errlen, "line %d: no pattern", editor_line + 1);
        return false;
    }

    if (looks_like_grid(rest, restlen)) {
        out->pat = grid_to_pattern(a, rest, restlen);
    } else {
        char buf[192];
        size_t n = restlen < sizeof(buf) - 1 ? restlen : sizeof(buf) - 1;
        memcpy(buf, rest, n);
        buf[n] = 0;
        char perr[64] = "";
        out->pat = sd_mini_parse(a, buf, (uint32_t)(editor_line * 7919 + 13), perr, sizeof(perr));
        if (!out->pat) {
            snprintf(err, errlen, "line %d: %s", editor_line + 1, perr);
            return false;
        }
    }
    return out->pat != NULL;
}

// A 16 slot summary of one cycle, purely so the UI can draw a lane.
static uint16_t pattern_mask(sd_pat_t* p) {
    uint16_t  mask = 0;
    sd_haps_t out  = {.haps = query_haps, .cap = MAX_HAPS, .n = 0};
    sd_span_t s    = {sd_int(0), sd_int(1)};
    sd_query(p, s, &out);
    for (int i = 0; i < out.n; i++) {
        if (!sd_hap_onset(&out.haps[i])) {
            continue;
        }
        double pos = sd_todouble(out.haps[i].whole.b);
        int    idx = (int)(pos * LANE_STEPS + 0.0001);
        if (idx >= 0 && idx < LANE_STEPS) {
            mask |= (uint16_t)(1u << idx);
        }
    }
    return mask;
}

void app_audio_eval(char const* text) {
    if (!synth) {
        return;
    }
    char err[sizeof(parse_error)] = "";
    int  spare                    = 1 - arena_live;
    sd_arena_reset(&arenas[spare]);
    memset(&prog_scratch, 0, sizeof(prog_scratch));

    char const* p           = text;
    int         editor_line = 0;
    while (*p && prog_scratch.nlines < APP_SEQ_MAX_LINES) {
        char const* eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);
        char        buf[256];
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, p, len);
        buf[len] = 0;

        seq_line_t line = {0};
        if (parse_line(buf, len, editor_line, &arenas[spare], &line, err, sizeof(err))) {
            line.mask                                = pattern_mask(line.pat);
            prog_scratch.line[prog_scratch.nlines++] = line;
        }
        editor_line++;
        if (!eol) {
            break;
        }
        p = eol + 1;
    }

    if (arenas[spare].exhausted && !err[0]) {
        snprintf(err, sizeof(err), "too many patterns to fit");
    }

    xSemaphoreTake(prog_mutex, portMAX_DELAY);
    prog       = prog_scratch;
    arena_live = spare;
    xSemaphoreGive(prog_mutex);

    strncpy(parse_error, err, sizeof(parse_error) - 1);
    parse_error[sizeof(parse_error) - 1] = 0;
    ESP_LOGI(TAG, "eval: %d parts%s%s", prog.nlines, err[0] ? ", " : "", err);
}

// ---------------------------------------------------------------------------
// Triggering
// ---------------------------------------------------------------------------

static float note_hz(int midi) {
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

static bool word_to_note(char const* w, float* out_hz) {
    static int8_t const semis[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    size_t              len      = strlen(w);
    if (len == 0) {
        return false;
    }
    char c = (char)(w[0] | 0x20);
    if (c < 'a' || c > 'g') {
        return false;
    }
    int    midi = semis[c - 'a'];
    size_t i    = 1;
    if (i < len && (w[i] == '#' || w[i] == 's')) {
        midi++;
        i++;
    } else if (i < len && w[i] == 'b') {
        midi--;
        i++;
    }
    int oct = 3;
    if (i < len && w[i] >= '0' && w[i] <= '9') {
        oct = w[i] - '0';
        i++;
    }
    if (i != len) {
        return false;
    }
    *out_hz = note_hz(midi + 12 * (oct + 1));
    return true;
}

static void trigger(seq_line_t const* ln, sd_val_t const* bare, float step_seconds) {
    float const base_hz = 65.406f;  // c2

    sd_note_t n = {0};
    n.drum      = ln->drum;
    n.wave      = ln->wave == SD_WAVE_COUNT ? SD_WAVE_SINE : ln->wave;
    n.gain      = ln->gain;
    n.pan       = 0.5f;
    n.orbit     = ln->orbit;
    n.freq      = base_hz;

    if (bare->type == SD_V_WORD) {
        if (bare->word[0] == 'X') {
            n.gain *= 1.3f;
        } else if (ln->melodic) {
            float hz;
            if (word_to_note(bare->word, &hz)) {
                n.freq = hz;
            }
        }
    } else if (bare->type == SD_V_NUM) {
        if (ln->melodic) {
            n.freq = base_hz * powf(2.0f, (float)bare->num / 12.0f);
        } else {
            n.gain *= (float)bare->num / 9.0f;  // 1..9 is a velocity
        }
    }

    if (ln->drum == SD_DRUM_NONE) {
        n.attack    = 0.004f;
        n.decay     = 0.09f;
        n.sustain   = 0.6f;
        n.release   = 0.14f;
        n.dur       = step_seconds * 0.85f;
        n.cutoff    = ln->cutoff;
        n.resonance = ln->resonance > 0.0f ? ln->resonance : 0.35f;
    }
    sd_synth_note_on(synth, &n);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

static sd_frac_t cycle_at(int64_t pos, int bpm) {
    return sd_frac(pos * bpm, CYCLE_DEN);
}

static int gather(sd_frac_t c0, sd_frac_t c1, int bpm) {
    int    nev            = 0;
    double samples_per_cy = (double)CYCLE_DEN / (double)bpm;

    for (int l = 0; l < prog.nlines && nev < MAX_EVENTS; l++) {
        sd_pat_t* pat = prog.line[l].pat;
        if (!pat) {
            continue;
        }
        sd_haps_t out = {.haps = query_haps, .cap = MAX_HAPS, .n = 0};
        sd_span_t s   = {c0, c1};
        sd_query(pat, s, &out);

        for (int i = 0; i < out.n && nev < MAX_EVENTS; i++) {
            if (!sd_hap_onset(&out.haps[i])) {
                continue;
            }
            double dt  = sd_todouble(sd_sub(out.haps[i].whole.b, c0));
            long   off = lround(dt * samples_per_cy);
            if (off < 0) {
                off = 0;
            }
            if (off >= BLOCK) {
                off = BLOCK - 1;
            }
            events[nev].off  = (uint32_t)off;
            events[nev].line = (int16_t)l;
            events[nev].bare = out.haps[i].v.bare;
            nev++;
        }
    }

    // Small n, and almost always nearly sorted already
    for (int i = 1; i < nev; i++) {
        for (int j = i; j > 0 && events[j - 1].off > events[j].off; j--) {
            typeof(events[0]) t = events[j - 1];
            events[j - 1]       = events[j];
            events[j]           = t;
        }
    }
    return nev;
}

static void audio_task(void* arg) {
    int64_t pos = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        xSemaphoreTake(prog_mutex, portMAX_DELAY);
        int   bpm          = bpm_i < 20 ? 20 : bpm_i;
        bool  play         = playing;
        float step_seconds = (float)(CYCLE_DEN / (double)bpm / SAMPLE_RATE / LANE_STEPS);

        int nev = 0;
        if (play) {
            sd_frac_t c0 = cycle_at(pos, bpm);
            sd_frac_t c1 = cycle_at(pos + BLOCK, bpm);
            nev          = gather(c0, c1, bpm);
            cur_step     = (int64_t)(sd_todouble(c0) * LANE_STEPS);
        }

        uint32_t done = 0;
        int      ev   = 0;
        while (done < BLOCK) {
            while (ev < nev && events[ev].off <= done) {
                trigger(&prog.line[events[ev].line], &events[ev].bare, step_seconds);
                ev++;
            }
            uint32_t next = (ev < nev) ? events[ev].off : BLOCK;
            if (next <= done) {
                next = done + 1;
            }
            uint32_t chunk = next - done;
            sd_synth_render(synth, fbuf + done * 2, chunk);
            done += chunk;
        }
        if (play) {
            pos += BLOCK;
        } else {
            pos      = 0;
            cur_step = 0;
        }
        xSemaphoreGive(prog_mutex);

        for (uint32_t i = 0; i < BLOCK * 2; i++) {
            float x = fbuf[i];
            ibuf[i] = (int16_t)((x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x)) * 32767.0f);
        }

        voices        = sd_synth_active_voices(synth);
        int64_t spent = esp_timer_get_time() - t0;
        load          = (float)spent / (1000000.0f * (float)BLOCK / (float)SAMPLE_RATE);

        size_t written = 0;
        i2s_channel_write(i2s, ibuf, sizeof(ibuf), &written, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

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

    for (int i = 0; i < 2; i++) {
        sd_arena_init(&arenas[i], arena_mem[i], ARENA_BYTES);
    }

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

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 1);
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
void  app_audio_set_bpm(float v) { bpm_i = v < 20.0f ? 20 : (v > 400.0f ? 400 : (int)(v + 0.5f)); }
float app_audio_get_bpm(void) { return (float)bpm_i; }

void app_audio_set_master(float g) {
    master = g < 0.0f ? 0.0f : (g > 2.0f ? 2.0f : g);
    sd_synth_set_master(synth, master);
}

float       app_audio_get_master(void) { return master; }
int64_t     app_audio_get_step(void) { return cur_step; }
int         app_audio_get_voices(void) { return voices; }
float       app_audio_get_load(void) { return load; }
char const* app_audio_get_error(void) { return parse_error; }
int         app_audio_get_part_count(void) { return prog.nlines; }

int app_audio_get_part_steps(int part) {
    return (part < 0 || part >= prog.nlines) ? 0 : LANE_STEPS;
}

bool app_audio_get_part_step_on(int part, int step) {
    if (part < 0 || part >= prog.nlines || step < 0 || step >= LANE_STEPS) {
        return false;
    }
    return (prog.line[part].mask & (1u << step)) != 0;
}

int app_audio_get_part_editor_line(int part) {
    return (part < 0 || part >= prog.nlines) ? -1 : prog.line[part].editor_line;
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
            return LANE_STEPS;
        }
    }
    return 0;
}
