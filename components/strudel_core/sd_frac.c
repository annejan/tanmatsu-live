#include "sd_frac.h"

static int64_t gcd64(int64_t a, int64_t b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b) {
        int64_t t = a % b;
        a         = b;
        b         = t;
    }
    return a ? a : 1;
}

sd_frac_t sd_frac(int64_t num, int64_t den) {
    if (den == 0) {
        return (sd_frac_t){0, 1};
    }
    if (den < 0) {
        num = -num;
        den = -den;
    }
    int64_t g = gcd64(num, den);
    return (sd_frac_t){num / g, den / g};
}

sd_frac_t sd_add(sd_frac_t a, sd_frac_t b) {
    return sd_frac(a.n * b.d + b.n * a.d, a.d * b.d);
}

sd_frac_t sd_sub(sd_frac_t a, sd_frac_t b) {
    return sd_frac(a.n * b.d - b.n * a.d, a.d * b.d);
}

sd_frac_t sd_mul(sd_frac_t a, sd_frac_t b) {
    // Cross reduce before multiplying, which keeps the products far away from
    // overflowing on the deeply nested patterns that mini notation produces.
    sd_frac_t x = sd_frac(a.n, b.d);
    sd_frac_t y = sd_frac(b.n, a.d);
    return sd_frac(x.n * y.n, x.d * y.d);
}

sd_frac_t sd_div(sd_frac_t a, sd_frac_t b) {
    if (b.n == 0) {
        return (sd_frac_t){0, 1};
    }
    return sd_mul(a, (sd_frac_t){b.d, b.n});
}

int sd_cmp(sd_frac_t a, sd_frac_t b) {
    // No 128 bit integers on a 32 bit RISC-V target, so cross multiply in 64
    // bits and only fall back when that would overflow. Both fractions are
    // normalized with a positive denominator, so the comparison is exact.
    if (a.d == b.d) {
        return a.n < b.n ? -1 : (a.n > b.n ? 1 : 0);
    }
    int64_t l, r;
    if (!__builtin_mul_overflow(a.n, b.d, &l) && !__builtin_mul_overflow(b.n, a.d, &r)) {
        return l < r ? -1 : (l > r ? 1 : 0);
    }
    double x = (double)a.n / (double)a.d;
    double y = (double)b.n / (double)b.d;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int64_t sd_floori(sd_frac_t a) {
    int64_t q = a.n / a.d;
    if (a.n % a.d != 0 && a.n < 0) {
        q--;  // C truncates towards zero, floor has to go the other way
    }
    return q;
}

sd_frac_t sd_floor(sd_frac_t a) {
    return sd_int(sd_floori(a));
}

sd_frac_t sd_cyclepos(sd_frac_t a) {
    return sd_sub(a, sd_floor(a));
}

sd_frac_t sd_min(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) <= 0 ? a : b;
}

sd_frac_t sd_max(sd_frac_t a, sd_frac_t b) {
    return sd_cmp(a, b) >= 0 ? a : b;
}
