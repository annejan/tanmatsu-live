#include "sd_scale.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    char const* name;
    int8_t      steps[12];
    int8_t      size;
} scale_def_t;

static scale_def_t const scales[SD_SCALE_COUNT] = {
    [SD_SCALE_NONE]           = {"none", {0}, 0},
    [SD_SCALE_MAJOR]          = {"major", {0, 2, 4, 5, 7, 9, 11}, 7},
    [SD_SCALE_MINOR]          = {"minor", {0, 2, 3, 5, 7, 8, 10}, 7},
    [SD_SCALE_DORIAN]         = {"dorian", {0, 2, 3, 5, 7, 9, 10}, 7},
    [SD_SCALE_PHRYGIAN]       = {"phrygian", {0, 1, 3, 5, 7, 8, 10}, 7},
    [SD_SCALE_LYDIAN]         = {"lydian", {0, 2, 4, 6, 7, 9, 11}, 7},
    [SD_SCALE_MIXOLYDIAN]     = {"mixolydian", {0, 2, 4, 5, 7, 9, 10}, 7},
    [SD_SCALE_LOCRIAN]        = {"locrian", {0, 1, 3, 5, 6, 8, 10}, 7},
    [SD_SCALE_HARMONIC_MINOR] = {"harmonic", {0, 2, 3, 5, 7, 8, 11}, 7},
    [SD_SCALE_MAJOR_PENT]     = {"majpent", {0, 2, 4, 7, 9}, 5},
    [SD_SCALE_MINOR_PENT]     = {"minpent", {0, 3, 5, 7, 10}, 5},
    [SD_SCALE_BLUES]          = {"blues", {0, 3, 5, 6, 7, 10}, 6},
    [SD_SCALE_CHROMATIC]      = {"chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12},
};

sd_scale_t sd_scale_from_name(char const* name) {
    if (!name) {
        return SD_SCALE_NONE;
    }
    for (int i = 1; i < SD_SCALE_COUNT; i++) {
        if (strcmp(name, scales[i].name) == 0) {
            return (sd_scale_t)i;
        }
    }
    // The names people reach for that are the same thing
    if (strcmp(name, "aeolian") == 0) {
        return SD_SCALE_MINOR;
    }
    if (strcmp(name, "ionian") == 0) {
        return SD_SCALE_MAJOR;
    }
    if (strcmp(name, "pent") == 0) {
        return SD_SCALE_MINOR_PENT;
    }
    return SD_SCALE_NONE;
}

char const* sd_scale_name(sd_scale_t s) {
    return (s > SD_SCALE_NONE && s < SD_SCALE_COUNT) ? scales[s].name : "none";
}

int sd_scale_semitone(sd_scale_t s, int degree) {
    if (s <= SD_SCALE_NONE || s >= SD_SCALE_COUNT) {
        return degree;  // no scale means the number was already semitones
    }
    int size = scales[s].size;
    if (size <= 0) {
        return degree;
    }
    // Floor division, so degree -1 is the note below the root rather than the
    // root again
    int octave = degree / size;
    int idx    = degree % size;
    if (idx < 0) {
        idx += size;
        octave--;
    }
    return scales[s].steps[idx] + 12 * octave;
}
