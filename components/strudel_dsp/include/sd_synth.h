// Polyphonic voice engine for the Tanmatsu live coding instrument.
//
// Pure C, no ESP-IDF dependencies, so it can be compiled and unit tested on a
// host. Everything is single precision float; the ESP32-P4 has a hardware FPU.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SD_MAX_VOICES 24
#define SD_ORBITS     4

typedef enum {
    SD_WAVE_SINE = 0,
    SD_WAVE_SAW,
    SD_WAVE_SQUARE,
    SD_WAVE_TRI,
    SD_WAVE_NOISE,
    SD_WAVE_COUNT,
} sd_wave_t;

// Drum voices are synthesized, so a beat plays without any samples on the SD
// card. SD_DRUM_NONE means the oscillator or sample player is used instead.
typedef enum {
    SD_DRUM_NONE = 0,
    SD_DRUM_BD,   // kick
    SD_DRUM_SD,   // snare
    SD_DRUM_HH,   // closed hat
    SD_DRUM_OH,   // open hat
    SD_DRUM_CP,   // clap
    SD_DRUM_RIM,  // rimshot
    SD_DRUM_TOM,  // tom
    SD_DRUM_COUNT,
} sd_drum_t;

// One note-on request. Units are absolute: the pattern layer resolves musical
// values (note names, cycles) into Hz and seconds before getting here.
typedef struct {
    sd_wave_t wave;
    sd_drum_t drum;
    float     freq;                             // Hz
    float     gain;                             // linear, 0..1
    float     pan;                              // 0 = left, 0.5 = centre, 1 = right
    float     attack, decay, sustain, release;  // seconds; sustain is a level
    float     dur;                              // seconds of gate before release
    float     cutoff;                           // Hz; <= 0 bypasses the filter
    float     resonance;                        // 0..1, maps to Q 0.5..12
    float     shape;                            // 0..1 waveshaper drive
    int       orbit;                            // which effect bus, 0..SD_ORBITS-1
    // How much of this voice goes to the delay and to the reverb. Negative
    // means take the orbit's own send level, which is the usual case.
    float     delay_send;
    float     room_send;

    // Sample playback. When smp is non-NULL it replaces the oscillator.
    int16_t const* smp;
    uint32_t       smp_len;    // frames (mono)
    float          smp_speed;  // 1.0 = original pitch
} sd_note_t;

// A note with the sends set to inherit and nothing else assumed. Zeroing a
// sd_note_t yourself would silently mean "send this voice nowhere".
sd_note_t sd_note_default(void);

typedef struct sd_synth sd_synth_t;

// Zeroing allocator. hot marks small buffers the render loop touches every
// sample, which on a badge with PSRAM belong in internal RAM; bulk buffers can
// live in slower external memory.
typedef void* (*sd_alloc_fn)(size_t bytes, bool hot);
typedef void (*sd_free_fn)(void* ptr);

// Allocates the synth. Delay lines are NOT allocated here, only when a delay
// is actually given a non-zero mix, so an unused orbit costs nothing.
sd_synth_t* sd_synth_create(uint32_t sample_rate);
sd_synth_t* sd_synth_create_ex(uint32_t sample_rate, sd_alloc_fn alloc, sd_free_fn release);
void        sd_synth_destroy(sd_synth_t* s);

// Starts a note. Safe to call from a different task than sd_synth_render as
// long as the caller serializes its own calls; voice stealing takes the
// quietest voice when all are busy.
void sd_synth_note_on(sd_synth_t* s, sd_note_t const* n);

// Renders interleaved stereo, frames per-channel samples, values in -1..1.
void sd_synth_render(sd_synth_t* s, float* out_lr, uint32_t frames);

// Per-orbit delay. time_s is clamped to the allocated line length.
void sd_synth_set_delay(sd_synth_t* s, int orbit, float time_s, float feedback, float mix);

// One shared reverb, fed by a per orbit send. A single tank costs a fraction
// of a reverb per orbit and is how a mixing desk would do it anyway.
// size 0..1 is the tail length, damping 0..1 rolls off the highs in the tail,
// mix 0..1 is how much of the tank reaches the output.
void sd_synth_set_reverb(sd_synth_t* s, float size, float damping, float mix);
void sd_synth_set_room(sd_synth_t* s, int orbit, float send);

void sd_synth_set_master(sd_synth_t* s, float gain);
int  sd_synth_active_voices(sd_synth_t const* s);
void sd_synth_panic(sd_synth_t* s);  // silence everything immediately

// Maps a drum name ("bd", "sd", ...) to its enum, SD_DRUM_NONE if unknown.
sd_drum_t sd_drum_from_name(char const* name);
// Maps a waveform name ("sine", "saw", ...), SD_WAVE_COUNT if unknown.
sd_wave_t sd_wave_from_name(char const* name);
