// Scales, so a part can be written in degrees rather than in semitones.
//
// Writing "0 2 4 6" and choosing a scale means every note is in key by
// construction, which matters far more on a badge than it does in a DAW: there
// is no piano roll to correct a wrong note in, and a set is played once.
#pragma once

#include <stdbool.h>

typedef enum {
    SD_SCALE_NONE = 0,
    SD_SCALE_MAJOR,
    SD_SCALE_MINOR,
    SD_SCALE_DORIAN,
    SD_SCALE_PHRYGIAN,
    SD_SCALE_LYDIAN,
    SD_SCALE_MIXOLYDIAN,
    SD_SCALE_LOCRIAN,
    SD_SCALE_HARMONIC_MINOR,
    SD_SCALE_MAJOR_PENT,
    SD_SCALE_MINOR_PENT,
    SD_SCALE_BLUES,
    SD_SCALE_CHROMATIC,
    SD_SCALE_COUNT,
} sd_scale_t;

// Looks up a scale by name, SD_SCALE_NONE when unknown.
sd_scale_t sd_scale_from_name(char const* name);
char const* sd_scale_name(sd_scale_t s);

// Semitones above the root for a degree. Degrees below zero and past the end
// of the scale keep going, wrapping into lower and higher octaves, so "0 2 4 7"
// and "-3 0 2" both mean something musical.
int sd_scale_semitone(sd_scale_t s, int degree);
