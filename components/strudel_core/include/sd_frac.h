// Exact rational time.
//
// Cycle positions have to be exact: a pattern like [a b c] divides a cycle in
// three, and accumulating that in floating point drifts until events land on
// the wrong side of a boundary. Everything in the pattern engine is a fraction
// and only becomes a float at the point a note is handed to the synth.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t n;  // numerator
    int64_t d;  // denominator, always > 0
} sd_frac_t;

// Normalizes sign and reduces by the greatest common divisor.
sd_frac_t sd_frac(int64_t num, int64_t den);

static inline sd_frac_t sd_int(int64_t v) {
    return (sd_frac_t){v, 1};
}

sd_frac_t sd_add(sd_frac_t a, sd_frac_t b);
sd_frac_t sd_sub(sd_frac_t a, sd_frac_t b);
sd_frac_t sd_mul(sd_frac_t a, sd_frac_t b);
sd_frac_t sd_div(sd_frac_t a, sd_frac_t b);

// -1, 0 or 1
int sd_cmp(sd_frac_t a, sd_frac_t b);

static inline bool sd_eq(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) == 0;
}
static inline bool sd_lt(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) < 0;
}
static inline bool sd_lte(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) <= 0;
}
static inline bool sd_gt(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) > 0;
}
static inline bool sd_gte(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) >= 0;
}

// Largest integer not greater than a, as an integer and as a fraction.
int64_t   sd_floori(sd_frac_t a);
sd_frac_t sd_floor(sd_frac_t a);
// Position within the current cycle, always in [0,1).
sd_frac_t sd_cyclepos(sd_frac_t a);

sd_frac_t sd_min(sd_frac_t a, sd_frac_t b);
sd_frac_t sd_max(sd_frac_t a, sd_frac_t b);

static inline double sd_todouble(sd_frac_t a) {
    return (double)a.n / (double)a.d;
}
