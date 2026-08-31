// Scale degrees are what let someone write a bass line as numbers and stay in
// key, so the wrapping into lower and higher octaves has to be right.
#include <stdio.h>
#include <string.h>

#include "sd_scale.h"

static int failures = 0;
static int checks   = 0;

static void eq(int got, int want, char const* what) {
    checks++;
    if (got != want) {
        printf("FAIL: %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

static void ok(int cond, char const* what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    ok(sd_scale_from_name("minor") == SD_SCALE_MINOR, "minor by name");
    ok(sd_scale_from_name("aeolian") == SD_SCALE_MINOR, "aeolian is minor");
    ok(sd_scale_from_name("nonsense") == SD_SCALE_NONE, "unknown name is none");

    // Degrees inside the first octave
    eq(sd_scale_semitone(SD_SCALE_MAJOR, 0), 0, "major degree 0");
    eq(sd_scale_semitone(SD_SCALE_MAJOR, 2), 4, "major degree 2 is a third");
    eq(sd_scale_semitone(SD_SCALE_MAJOR, 4), 7, "major degree 4 is a fifth");
    eq(sd_scale_semitone(SD_SCALE_MINOR, 2), 3, "minor third is flat");

    // Past the end of the scale keeps climbing
    eq(sd_scale_semitone(SD_SCALE_MAJOR, 7), 12, "degree 7 is the octave");
    eq(sd_scale_semitone(SD_SCALE_MAJOR, 9), 16, "degree 9 keeps going");
    eq(sd_scale_semitone(SD_SCALE_MINOR_PENT, 5), 12, "pentatonic octave is five degrees");

    // Below the root goes down rather than wrapping to the root
    eq(sd_scale_semitone(SD_SCALE_MAJOR, -1), -1, "degree -1 is the leading tone below");
    eq(sd_scale_semitone(SD_SCALE_MAJOR, -7), -12, "degree -7 is an octave down");
    eq(sd_scale_semitone(SD_SCALE_MINOR, -1), -2, "minor degree -1");

    // Chromatic is a passthrough, which is what no scale should feel like
    for (int d = -12; d <= 12; d++) {
        if (sd_scale_semitone(SD_SCALE_CHROMATIC, d) != d) {
            printf("FAIL: chromatic degree %d\n", d);
            failures++;
            break;
        }
    }
    checks++;

    // No scale leaves the number as semitones
    eq(sd_scale_semitone(SD_SCALE_NONE, 7), 7, "no scale is semitones");

    // Every scale is monotonic, or a run of degrees would not climb
    for (int s = 1; s < SD_SCALE_COUNT; s++) {
        int prev = sd_scale_semitone((sd_scale_t)s, -8);
        for (int d = -7; d <= 16; d++) {
            int cur = sd_scale_semitone((sd_scale_t)s, d);
            if (cur <= prev) {
                printf("FAIL: %s not monotonic at degree %d\n", sd_scale_name((sd_scale_t)s), d);
                failures++;
                break;
            }
            prev = cur;
        }
    }
    checks++;

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
