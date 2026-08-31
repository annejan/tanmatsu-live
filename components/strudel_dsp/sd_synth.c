#include "sd_synth.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Keep the render loop out of flash. Instruction fetch goes over the same MSPI
// bus as PSRAM, so a cache miss here stalls behind the framebuffer traffic.
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define SD_HOT IRAM_ATTR
#else
#define SD_HOT
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Half a second of stereo float per orbit is 187 KB. These live in external
// memory and are read and written every sample, so they are allocated only for
// orbits that actually have a delay switched on.
#define SD_DELAY_MAX_S 0.5f

typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
} env_stage_t;

// Cytomic style topology preserving transform state variable filter. Cheap,
// stable when the cutoff is modulated, and does not blow up at high Q.
typedef struct {
    float g, k, a1, a2, a3;
    float ic1eq, ic2eq;
} svf_t;

typedef struct {
    bool     active;
    uint32_t age;  // note-on order, used to pick a victim when stealing

    // Oscillator
    sd_wave_t wave;
    float     phase, phase_inc;

    // Sample player
    int16_t const* smp;
    uint32_t       smp_len;
    float          smp_pos, smp_inc;

    // Drum. dec_* are running decay values, dk* their per sample multipliers,
    // which replaces an expf call per envelope per sample.
    sd_drum_t drum;
    float     drum_t;  // seconds since note-on, only the clap still needs it
    float     dec_a, dec_b, dec_c;
    float     dka, dkb, dkc;
    float     cp_slap;

    // Envelope
    env_stage_t stage;
    float       env, env_target;
    float       atk_rate, dec_rate, rel_rate, sustain;
    float       gate_left;  // seconds until release is triggered

    svf_t filter;
    bool  use_filter;

    float    gain, pan_l, pan_r, shape;
    float    delay_send, room_send;
    int      orbit;
    uint32_t rng;
} voice_t;

typedef struct {
    float*   buf;  // stereo interleaved
    uint32_t len;  // frames
    uint32_t wpos;
    float    time_s, feedback, mix;
    float    room_send;
} delay_t;

// A Schroeder tank: four damped combs in parallel into two allpass sections.
// Mono in, spread to stereo on the way out, which costs half the memory of a
// true stereo tank and is indistinguishable on a badge speaker.
#define RV_COMBS   4
#define RV_ALLPASS 2

typedef struct {
    float*   comb[RV_COMBS];
    uint32_t comb_len[RV_COMBS];
    uint32_t comb_pos[RV_COMBS];
    float    comb_store[RV_COMBS];

    float*   ap[RV_ALLPASS];
    uint32_t ap_len[RV_ALLPASS];
    uint32_t ap_pos[RV_ALLPASS];

    float feedback, damping, mix;
    bool  ready;
} reverb_t;

struct sd_synth {
    sd_alloc_fn alloc;
    sd_free_fn  release;
    uint32_t    sr;
    float       inv_sr;
    voice_t     v[SD_MAX_VOICES];
    uint32_t    age_counter;
    delay_t     delay[SD_ORBITS];
    reverb_t    reverb;
    float*      orbit_mix;
    // Send buses, so a single voice can be wetter than the rest of its orbit
    float*      delay_send;
    float*      room_send;  // scratch, stereo interleaved, one block
    uint32_t    orbit_mix_frames;
    float       master;
    uint32_t    rng;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline uint32_t xorshift(uint32_t* s) {
    uint32_t x  = *s;
    x          ^= x << 13;
    x          ^= x >> 17;
    x          ^= x << 5;
    *s          = x ? x : 0x1234567u;
    return *s;
}

static inline float white(uint32_t* s) {
    return (float)(int32_t)(xorshift(s) >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

static void svf_set(svf_t* f, float cutoff, float q, float sr) {
    if (cutoff < 20.0f) {
        cutoff = 20.0f;
    }
    float nyq = sr * 0.49f;
    if (cutoff > nyq) {
        cutoff = nyq;
    }
    if (q < 0.5f) {
        q = 0.5f;
    }
    f->g  = tanf((float)M_PI * cutoff / sr);
    f->k  = 1.0f / q;
    f->a1 = 1.0f / (1.0f + f->g * (f->g + f->k));
    f->a2 = f->g * f->a1;
    f->a3 = f->g * f->a2;
}

static inline float svf_lp(svf_t* f, float in) {
    float v3 = in - f->ic2eq;
    float v1 = f->a1 * f->ic1eq + f->a2 * v3;
    float v2 = f->ic2eq + f->a2 * f->ic1eq + f->a3 * v3;
    f->ic1eq = 2.0f * v1 - f->ic1eq;
    f->ic2eq = 2.0f * v2 - f->ic2eq;
    return v2;
}

static inline float svf_hp(svf_t* f, float in) {
    float v3 = in - f->ic2eq;
    float v1 = f->a1 * f->ic1eq + f->a2 * v3;
    float v2 = f->ic2eq + f->a2 * f->ic1eq + f->a3 * v3;
    f->ic1eq = 2.0f * v1 - f->ic1eq;
    f->ic2eq = 2.0f * v2 - f->ic2eq;
    return in - f->k * v1 - v2;
}

// sin(2*pi*p) for p in [0,1), accurate to about 0.1 percent. A parabola plus a
// correction term: no table, no libm call, and newlib's sinf on a 400 MHz
// in-order core is far more expensive than this handful of multiplies.
static inline float fast_sin01(float p) {
    float x = 2.0f * p - 1.0f;
    float y = 4.0f * x * (1.0f - fabsf(x));
    y       = y * (0.775f + 0.225f * fabsf(y));
    return -y;
}

// One pole exponential decay per sample, so the drum voices need no expf in
// the inner loop. The coefficient is computed once at note-on.
static inline float decay_coef(float rate_per_s, float inv_sr) {
    return expf(-rate_per_s * inv_sr);
}

// Antiderivative-free polynomial band limited step. Removes the worst of the
// aliasing from the saw and square without the cost of a wavetable per octave.
static inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float osc(voice_t* v) {
    float p  = v->phase;
    float dt = v->phase_inc;
    float out;
    switch (v->wave) {
        case SD_WAVE_SINE:
            out = fast_sin01(p);
            break;
        case SD_WAVE_SAW:
            out = 2.0f * p - 1.0f - polyblep(p, dt);
            break;
        case SD_WAVE_SQUARE: {
            float p2 = p + 0.5f;
            if (p2 >= 1.0f) {
                p2 -= 1.0f;
            }
            out = (p < 0.5f ? 1.0f : -1.0f) - polyblep(p, dt) + polyblep(p2, dt);
            break;
        }
        case SD_WAVE_TRI:
            out = 4.0f * fabsf(p - 0.5f) - 1.0f;
            break;
        case SD_WAVE_NOISE:
            out = white(&v->rng);
            break;
        default:
            out = 0.0f;
            break;
    }
    v->phase += dt;
    if (v->phase >= 1.0f) {
        v->phase -= 1.0f;
    }
    return out;
}

// Analog style drum voices. Each is a pitch swept oscillator, a noise burst, or
// both, with its own hardcoded envelope; the caller's ADSR still scales it.
static inline float drum(voice_t* v, float inv_sr) {
    float out = 0.0f;
    switch (v->drum) {
        case SD_DRUM_BD: {
            float f   = 48.0f + 110.0f * v->dec_a;
            v->phase += f * inv_sr;
            if (v->phase >= 1.0f) {
                v->phase -= 1.0f;
            }
            out = fast_sin01(v->phase) * v->dec_b + white(&v->rng) * v->dec_c * 0.4f;
            break;
        }
        case SD_DRUM_SD: {
            v->phase += 185.0f * inv_sr;
            if (v->phase >= 1.0f) {
                v->phase -= 1.0f;
            }
            out = fast_sin01(v->phase) * v->dec_b * 0.5f + svf_hp(&v->filter, white(&v->rng)) * v->dec_c * 0.9f;
            break;
        }
        case SD_DRUM_HH:
        case SD_DRUM_OH:
            out = svf_hp(&v->filter, white(&v->rng)) * v->dec_b;
            break;
        case SD_DRUM_CP: {
            // Three fast slaps then the body, which is what makes it a clap
            if (v->drum_t < 0.033f) {
                out         = svf_hp(&v->filter, white(&v->rng)) * v->dec_b;
                v->cp_slap += inv_sr;
                if (v->cp_slap >= 0.011f) {
                    v->cp_slap = 0.0f;
                    v->dec_b   = 1.0f;  // retrigger the slap
                }
            } else {
                out = svf_hp(&v->filter, white(&v->rng)) * v->dec_c;
            }
            break;
        }
        case SD_DRUM_RIM: {
            v->phase += 1700.0f * inv_sr;
            if (v->phase >= 1.0f) {
                v->phase -= 1.0f;
            }
            out = (fast_sin01(v->phase) * 0.7f + white(&v->rng) * 0.3f) * v->dec_b;
            break;
        }
        case SD_DRUM_TOM: {
            float f   = 90.0f + 60.0f * v->dec_a;
            v->phase += f * inv_sr;
            if (v->phase >= 1.0f) {
                v->phase -= 1.0f;
            }
            out = fast_sin01(v->phase) * v->dec_b;
            break;
        }
        default:
            break;
    }
    v->dec_a  *= v->dka;
    v->dec_b  *= v->dkb;
    v->dec_c  *= v->dkc;
    v->drum_t += inv_sr;
    return out;
}

// f(0)' = 1, f(+-1.5) = +-1 with zero slope, so it saturates without a corner
// and without a libm call.
static inline float soft_clip(float x) {
    if (x >= 1.5f) {
        return 1.0f;
    }
    if (x <= -1.5f) {
        return -1.0f;
    }
    return x - (4.0f / 27.0f) * x * x * x;
}

static inline float shaper(float x, float amount) {
    if (amount <= 0.0f) {
        return x;
    }
    return soft_clip(x * (1.0f + amount * 8.0f));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void* default_alloc(size_t bytes, bool hot) {
    (void)hot;
    return calloc(1, bytes);
}

static void default_free(void* p) {
    free(p);
}

sd_synth_t* sd_synth_create(uint32_t sample_rate) {
    return sd_synth_create_ex(sample_rate, default_alloc, default_free);
}

sd_synth_t* sd_synth_create_ex(uint32_t sample_rate, sd_alloc_fn alloc, sd_free_fn release) {
    if (sample_rate < 8000) {
        return NULL;
    }
    if (!alloc || !release) {
        alloc   = default_alloc;
        release = default_free;
    }
    sd_synth_t* s = alloc(sizeof(sd_synth_t), true);
    if (!s) {
        return NULL;
    }
    s->alloc   = alloc;
    s->release = release;
    s->sr      = sample_rate;
    s->inv_sr  = 1.0f / (float)sample_rate;
    s->master  = 0.8f;
    s->rng     = 0xC0FFEEu;

    for (int i = 0; i < SD_ORBITS; i++) {
        s->delay[i].len      = (uint32_t)(SD_DELAY_MAX_S * (float)sample_rate);
        s->delay[i].time_s   = 0.25f;
        s->delay[i].feedback = 0.4f;
        s->delay[i].mix      = 0.0f;
    }
    for (int i = 0; i < SD_MAX_VOICES; i++) {
        s->v[i].rng = 0x9E3779B9u + (uint32_t)i * 2654435761u;
    }
    return s;
}

// Delay lengths in samples at 48k, mutually prime so the tank does not ring on
// one pitch. Scaled from the classic 44.1k values.
static uint32_t const RV_COMB_LEN[RV_COMBS] = {1213, 1291, 1387, 1481};
static uint32_t const RV_AP_LEN[RV_ALLPASS] = {605, 481};

static bool reverb_alloc(sd_synth_t* s) {
    reverb_t* r = &s->reverb;
    if (r->ready) {
        return true;
    }
    float scale = (float)s->sr / 48000.0f;
    for (int i = 0; i < RV_COMBS; i++) {
        r->comb_len[i] = (uint32_t)(RV_COMB_LEN[i] * scale);
        r->comb[i]     = s->alloc((size_t)r->comb_len[i] * sizeof(float), true);
        if (!r->comb[i]) {
            return false;
        }
    }
    for (int i = 0; i < RV_ALLPASS; i++) {
        r->ap_len[i] = (uint32_t)(RV_AP_LEN[i] * scale);
        r->ap[i]     = s->alloc((size_t)r->ap_len[i] * sizeof(float), true);
        if (!r->ap[i]) {
            return false;
        }
    }
    r->ready = true;
    return true;
}

static inline float reverb_tick(reverb_t* r, float in) {
    float acc = 0.0f;
    for (int i = 0; i < RV_COMBS; i++) {
        float* b   = r->comb[i];
        uint32_t p = r->comb_pos[i];
        float  out = b[p];
        // One pole lowpass inside the feedback path is what makes a tail decay
        // rather than turn into noise
        r->comb_store[i] = out * (1.0f - r->damping) + r->comb_store[i] * r->damping;
        b[p]             = in + r->comb_store[i] * r->feedback;
        r->comb_pos[i]   = (p + 1 >= r->comb_len[i]) ? 0 : p + 1;
        acc += out;
    }
    acc *= 1.0f / RV_COMBS;

    for (int i = 0; i < RV_ALLPASS; i++) {
        float*   b = r->ap[i];
        uint32_t p = r->ap_pos[i];
        float    bufout = b[p];
        float    out    = bufout - acc;
        b[p]            = acc + bufout * 0.5f;
        r->ap_pos[i]    = (p + 1 >= r->ap_len[i]) ? 0 : p + 1;
        acc             = out;
    }
    return acc;
}

void sd_synth_set_reverb(sd_synth_t* s, float size, float damping, float mix) {
    reverb_t* r = &s->reverb;
    r->feedback = 0.7f + (size < 0.0f ? 0.0f : (size > 1.0f ? 1.0f : size)) * 0.28f;
    r->damping  = damping < 0.0f ? 0.0f : (damping > 0.95f ? 0.95f : damping);
    r->mix      = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    if (r->mix > 0.0f) {
        reverb_alloc(s);
    }
}

void sd_synth_set_room(sd_synth_t* s, int orbit, float send) {
    if (orbit < 0 || orbit >= SD_ORBITS) {
        return;
    }
    s->delay[orbit].room_send = send < 0.0f ? 0.0f : (send > 1.0f ? 1.0f : send);
    if (send > 0.0f) {
        reverb_alloc(s);
    }
}

void sd_synth_destroy(sd_synth_t* s) {
    if (!s) {
        return;
    }
    sd_free_fn release = s->release ? s->release : default_free;
    for (int i = 0; i < SD_ORBITS; i++) {
        if (s->delay[i].buf) {
            release(s->delay[i].buf);
        }
    }
    for (int i = 0; i < RV_COMBS; i++) {
        if (s->reverb.comb[i]) {
            release(s->reverb.comb[i]);
        }
    }
    for (int i = 0; i < RV_ALLPASS; i++) {
        if (s->reverb.ap[i]) {
            release(s->reverb.ap[i]);
        }
    }
    if (s->orbit_mix) {
        release(s->orbit_mix);
    }
    if (s->delay_send) {
        release(s->delay_send);
    }
    if (s->room_send) {
        release(s->room_send);
    }
    release(s);
}

void sd_synth_set_master(sd_synth_t* s, float gain) {
    s->master = gain < 0.0f ? 0.0f : (gain > 4.0f ? 4.0f : gain);
}

void sd_synth_set_delay(sd_synth_t* s, int orbit, float time_s, float feedback, float mix) {
    if (orbit < 0 || orbit >= SD_ORBITS) {
        return;
    }
    delay_t* d = &s->delay[orbit];
    if (mix > 0.0f && !d->buf) {
        d->buf = s->alloc((size_t)d->len * 2 * sizeof(float), false);
        if (!d->buf) {
            return;  // no line, no delay; the dry signal still plays
        }
    }
    float max   = (float)d->len * s->inv_sr - 0.001f;
    d->time_s   = time_s < 0.001f ? 0.001f : (time_s > max ? max : time_s);
    d->feedback = feedback < 0.0f ? 0.0f : (feedback > 0.95f ? 0.95f : feedback);
    d->mix      = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
}

int sd_synth_active_voices(sd_synth_t const* s) {
    int n = 0;
    for (int i = 0; i < SD_MAX_VOICES; i++) {
        if (s->v[i].active) {
            n++;
        }
    }
    return n;
}

static void reverb_clear(sd_synth_t* s) {
    reverb_t* r = &s->reverb;
    for (int i = 0; i < RV_COMBS; i++) {
        if (r->comb[i]) {
            memset(r->comb[i], 0, (size_t)r->comb_len[i] * sizeof(float));
        }
        r->comb_store[i] = 0.0f;
    }
    for (int i = 0; i < RV_ALLPASS; i++) {
        if (r->ap[i]) {
            memset(r->ap[i], 0, (size_t)r->ap_len[i] * sizeof(float));
        }
    }
}

void sd_synth_panic(sd_synth_t* s) {
    for (int i = 0; i < SD_MAX_VOICES; i++) {
        s->v[i].active = false;
        s->v[i].stage  = ENV_IDLE;
    }
    for (int i = 0; i < SD_ORBITS; i++) {
        if (s->delay[i].buf) {
            memset(s->delay[i].buf, 0, (size_t)s->delay[i].len * 2 * sizeof(float));
        }
    }
    reverb_clear(s);
}

// ---------------------------------------------------------------------------
// Note on
// ---------------------------------------------------------------------------

static float rate_from_time(float seconds, float inv_sr) {
    if (seconds <= 0.0f) {
        return 1.0f;  // instant
    }
    return inv_sr / seconds;
}

static voice_t* pick_voice(sd_synth_t* s) {
    voice_t* best = NULL;
    for (int i = 0; i < SD_MAX_VOICES; i++) {
        if (!s->v[i].active) {
            return &s->v[i];
        }
    }
    // All busy: steal the quietest, breaking ties towards the oldest.
    float best_score = 1e30f;
    for (int i = 0; i < SD_MAX_VOICES; i++) {
        float score = s->v[i].env * s->v[i].gain + (float)s->v[i].age * 1e-9f;
        if (score < best_score) {
            best_score = score;
            best       = &s->v[i];
        }
    }
    return best;
}

sd_note_t sd_note_default(void) {
    sd_note_t n   = {0};
    n.gain        = 0.8f;
    n.pan         = 0.5f;
    n.delay_send  = -1.0f;  // inherit the orbit
    n.room_send   = -1.0f;
    n.smp_speed   = 1.0f;
    return n;
}

void sd_synth_note_on(sd_synth_t* s, sd_note_t const* n) {
    voice_t* v = pick_voice(s);
    if (!v) {
        return;
    }
    uint32_t rng = v->rng;
    memset(v, 0, sizeof(*v));
    v->rng    = rng ? rng : 0x1234567u;
    v->active = true;
    v->age    = ++s->age_counter;

    v->wave = (n->wave >= 0 && n->wave < SD_WAVE_COUNT) ? n->wave : SD_WAVE_SINE;
    v->drum = (n->drum > SD_DRUM_NONE && n->drum < SD_DRUM_COUNT) ? n->drum : SD_DRUM_NONE;

    float freq   = n->freq > 0.0f ? n->freq : 220.0f;
    v->phase_inc = freq * s->inv_sr;
    if (v->phase_inc > 0.49f) {
        v->phase_inc = 0.49f;
    }

    if (n->smp && n->smp_len > 0) {
        v->smp     = n->smp;
        v->smp_len = n->smp_len;
        v->smp_pos = 0.0f;
        v->smp_inc = n->smp_speed > 0.0f ? n->smp_speed : 1.0f;
    }

    v->gain   = n->gain > 0.0f ? n->gain : 0.8f;
    v->shape  = n->shape;
    float pan = n->pan;
    if (pan < 0.0f) {
        pan = 0.0f;
    }
    if (pan > 1.0f) {
        pan = 1.0f;
    }
    // Constant power pan, so a centred voice is not louder than a hard panned one
    v->pan_l = cosf(pan * 0.5f * (float)M_PI);
    v->pan_r = sinf(pan * 0.5f * (float)M_PI);
    v->orbit      = (n->orbit >= 0 && n->orbit < SD_ORBITS) ? n->orbit : 0;
    v->delay_send = n->delay_send >= 0.0f ? n->delay_send : s->delay[v->orbit].mix;
    v->room_send  = n->room_send >= 0.0f ? n->room_send : s->delay[v->orbit].room_send;

    v->sustain   = n->sustain > 0.0f ? n->sustain : (v->drum ? 0.0f : 0.7f);
    v->atk_rate  = rate_from_time(n->attack, s->inv_sr);
    v->dec_rate  = rate_from_time(n->decay > 0.0f ? n->decay : 0.1f, s->inv_sr);
    v->rel_rate  = rate_from_time(n->release > 0.0f ? n->release : 0.05f, s->inv_sr);
    v->gate_left = n->dur > 0.0f ? n->dur : 0.1f;
    v->stage     = ENV_ATTACK;
    v->env       = 0.0f;

    // Drums carry their own amplitude shape, so the ADSR just opens and stays
    // open for the length of the hit.
    if (v->drum != SD_DRUM_NONE) {
        v->stage     = ENV_SUSTAIN;
        v->env       = 1.0f;
        v->sustain   = 1.0f;
        v->gate_left = 1.5f;
        // The noise based drums reuse the voice filter as a fixed high pass
        switch (v->drum) {
            case SD_DRUM_HH:
                svf_set(&v->filter, 7500.0f, 0.8f, (float)s->sr);
                break;
            case SD_DRUM_OH:
                svf_set(&v->filter, 6000.0f, 0.8f, (float)s->sr);
                break;
            case SD_DRUM_CP:
                svf_set(&v->filter, 1200.0f, 1.2f, (float)s->sr);
                break;
            case SD_DRUM_SD:
                svf_set(&v->filter, 900.0f, 0.9f, (float)s->sr);
                break;
            default:
                break;
        }
        v->use_filter = false;  // the drum code drives the filter itself

        float isr = s->inv_sr;
        v->dec_a = v->dec_b = v->dec_c = 1.0f;
        v->dka = v->dkb = v->dkc = 1.0f;
        switch (v->drum) {
            case SD_DRUM_BD:
                v->dka = decay_coef(55.0f, isr);
                v->dkb = decay_coef(9.0f, isr);
                v->dkc = decay_coef(400.0f, isr);
                break;
            case SD_DRUM_SD:
                v->dkb = decay_coef(30.0f, isr);
                v->dkc = decay_coef(22.0f, isr);
                break;
            case SD_DRUM_HH:
                v->dkb = decay_coef(130.0f, isr);
                break;
            case SD_DRUM_OH:
                v->dkb = decay_coef(14.0f, isr);
                break;
            case SD_DRUM_CP:
                v->dkb   = decay_coef(480.0f, isr);
                v->dkc   = decay_coef(26.0f, isr);
                // Pre-wound so the body starts at full level at t = 33 ms
                v->dec_c = expf(0.033f * 26.0f);
                break;
            case SD_DRUM_RIM:
                v->dkb = decay_coef(160.0f, isr);
                break;
            case SD_DRUM_TOM:
                v->dka = decay_coef(30.0f, isr);
                v->dkb = decay_coef(11.0f, isr);
                break;
            default:
                break;
        }
    } else if (n->cutoff > 0.0f) {
        float q = 0.5f + (n->resonance < 0.0f ? 0.0f : (n->resonance > 1.0f ? 1.0f : n->resonance)) * 11.5f;
        svf_set(&v->filter, n->cutoff, q, (float)s->sr);
        v->use_filter = true;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

static inline float env_step(voice_t* v, float inv_sr) {
    switch (v->stage) {
        case ENV_ATTACK:
            v->env += v->atk_rate;
            if (v->env >= 1.0f) {
                v->env   = 1.0f;
                v->stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            v->env -= v->dec_rate * (1.0f - v->sustain);
            if (v->env <= v->sustain) {
                v->env   = v->sustain;
                v->stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            v->env -= v->rel_rate;
            if (v->env <= 0.0f) {
                v->env    = 0.0f;
                v->stage  = ENV_IDLE;
                v->active = false;
            }
            break;
        default:
            v->active = false;
            break;
    }
    if (v->stage != ENV_RELEASE && v->stage != ENV_IDLE) {
        v->gate_left -= inv_sr;
        if (v->gate_left <= 0.0f) {
            v->stage = ENV_RELEASE;
        }
    }
    return v->env;
}

void SD_HOT sd_synth_render(sd_synth_t* s, float* out_lr, uint32_t frames) {
    memset(out_lr, 0, (size_t)frames * 2 * sizeof(float));

    // Effect buses are summed separately so a delay can be per orbit
    if (s->orbit_mix_frames < frames) {
        if (s->orbit_mix) {
            s->release(s->orbit_mix);
        }
        s->orbit_mix = s->alloc((size_t)frames * 2 * SD_ORBITS * sizeof(float), true);
        if (s->delay_send) {
            s->release(s->delay_send);
        }
        s->delay_send = s->alloc((size_t)frames * 2 * SD_ORBITS * sizeof(float), true);
        if (s->room_send) {
            s->release(s->room_send);
        }
        s->room_send = s->alloc((size_t)frames * sizeof(float), true);
        if (!s->orbit_mix || !s->delay_send || !s->room_send) {
            s->orbit_mix_frames = 0;
            return;
        }
        s->orbit_mix_frames = frames;
    }
    memset(s->orbit_mix, 0, (size_t)frames * 2 * SD_ORBITS * sizeof(float));
    memset(s->delay_send, 0, (size_t)frames * 2 * SD_ORBITS * sizeof(float));
    memset(s->room_send, 0, (size_t)frames * sizeof(float));

    for (int vi = 0; vi < SD_MAX_VOICES; vi++) {
        voice_t* v = &s->v[vi];
        if (!v->active) {
            continue;
        }
        float* bus  = s->orbit_mix + (size_t)v->orbit * frames * 2;
        float* dsnd = s->delay_send + (size_t)v->orbit * frames * 2;
        for (uint32_t i = 0; i < frames && v->active; i++) {
            float raw;
            if (v->drum != SD_DRUM_NONE) {
                raw = drum(v, s->inv_sr);
            } else if (v->smp) {
                uint32_t idx = (uint32_t)v->smp_pos;
                if (idx + 1 >= v->smp_len) {
                    v->active = false;
                    break;
                }
                float frac  = v->smp_pos - (float)idx;
                float a     = (float)v->smp[idx] * (1.0f / 32768.0f);
                float b     = (float)v->smp[idx + 1] * (1.0f / 32768.0f);
                raw         = a + (b - a) * frac;
                v->smp_pos += v->smp_inc;
            } else {
                raw = osc(v);
            }

            if (v->use_filter) {
                raw = svf_lp(&v->filter, raw);
            }
            if (v->shape > 0.0f) {
                raw = shaper(raw, v->shape);
            }

            float amp = env_step(v, s->inv_sr) * v->gain;
            float l   = raw * amp * v->pan_l;
            float r   = raw * amp * v->pan_r;
            bus[i * 2] += l;
            bus[i * 2 + 1] += r;
            if (v->delay_send > 0.0f) {
                dsnd[i * 2] += l * v->delay_send;
                dsnd[i * 2 + 1] += r * v->delay_send;
            }
            if (v->room_send > 0.0f) {
                s->room_send[i] += (l + r) * 0.5f * v->room_send;
            }
        }
    }

    for (int o = 0; o < SD_ORBITS; o++) {
        float*   bus = s->orbit_mix + (size_t)o * frames * 2;
        delay_t* d   = &s->delay[o];
        float const* dsnd = s->delay_send + (size_t)o * frames * 2;
        if (d->buf) {
            uint32_t dsamp = (uint32_t)(d->time_s * (float)s->sr);
            if (dsamp < 1) {
                dsamp = 1;
            }
            if (dsamp >= d->len) {
                dsamp = d->len - 1;
            }
            for (uint32_t i = 0; i < frames; i++) {
                uint32_t rpos            = (d->wpos + d->len - dsamp) % d->len;
                float    dl              = d->buf[rpos * 2];
                float    dr              = d->buf[rpos * 2 + 1];
                // The tank is fed by the send bus, so a voice can be wetter
                // than the rest of its orbit. The return is at unity because
                // the send level already decided how loud it is.
                d->buf[d->wpos * 2]     = dsnd[i * 2] + dl * d->feedback;
                d->buf[d->wpos * 2 + 1] = dsnd[i * 2 + 1] + dr * d->feedback;
                d->wpos                 = (d->wpos + 1) % d->len;
                bus[i * 2] += dl;
                bus[i * 2 + 1] += dr;
            }
        }
        for (uint32_t i = 0; i < frames * 2; i++) {
            out_lr[i] += bus[i];
        }
    }

    reverb_t* rv = &s->reverb;
    if (rv->ready && rv->mix > 0.0f) {
        for (uint32_t i = 0; i < frames; i++) {
            float wet = reverb_tick(rv, s->room_send[i]) * rv->mix;
            // A little decorrelation so the tail is not dead centre
            out_lr[i * 2] += wet;
            out_lr[i * 2 + 1] += wet * 0.92f;
        }
    }

    for (uint32_t i = 0; i < frames * 2; i++) {
        out_lr[i] = soft_clip(out_lr[i] * s->master);
    }
}

// ---------------------------------------------------------------------------
// Name lookup
// ---------------------------------------------------------------------------

sd_drum_t sd_drum_from_name(char const* name) {
    static char const* const names[SD_DRUM_COUNT] = {NULL, "bd", "sd", "hh", "oh", "cp", "rim", "tom"};
    if (!name) {
        return SD_DRUM_NONE;
    }
    for (int i = 1; i < SD_DRUM_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            return (sd_drum_t)i;
        }
    }
    return SD_DRUM_NONE;
}

sd_wave_t sd_wave_from_name(char const* name) {
    static char const* const names[SD_WAVE_COUNT] = {"sine", "saw", "square", "tri", "noise"};
    if (!name) {
        return SD_WAVE_COUNT;
    }
    for (int i = 0; i < SD_WAVE_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            return (sd_wave_t)i;
        }
    }
    return SD_WAVE_COUNT;
}
