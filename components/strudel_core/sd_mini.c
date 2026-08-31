#include "sd_mini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TERMS 24
#define MAX_DEPTH 6

typedef struct {
    char const* s;
    size_t      i;
    size_t      len;
    sd_arena_t* a;
    char*       err;
    size_t      errlen;
    bool        failed;
    int         depth;
    uint32_t    seed;
    uint32_t    rnd;  // bumped per '?' so separate ones decorrelate
} P;

static sd_pat_t* parse_stack(P* p, char close);

static void fail(P* p, char const* msg) {
    if (!p->failed) {
        p->failed = true;
        if (p->err && p->errlen) {
            snprintf(p->err, p->errlen, "%s at %u", msg, (unsigned)p->i);
        }
    }
}

static void skip_ws(P* p) {
    while (p->i < p->len && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) {
        p->i++;
    }
}

static char peek(P* p) {
    return p->i < p->len ? p->s[p->i] : '\0';
}

static bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#' || c == '.' ||
           c == '-';
}

// A bare number, used for the arguments of * / @ ! ( ) and ?
static bool parse_num(P* p, double* out) {
    skip_ws(p);
    size_t start = p->i;
    if (p->i < p->len && (p->s[p->i] == '-' || p->s[p->i] == '+')) {
        p->i++;
    }
    while (p->i < p->len && ((p->s[p->i] >= '0' && p->s[p->i] <= '9') || p->s[p->i] == '.')) {
        p->i++;
    }
    if (p->i == start) {
        return false;
    }
    char   buf[24];
    size_t n = p->i - start;
    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, p->s + start, n);
    buf[n] = 0;
    *out   = strtod(buf, NULL);
    return true;
}

static sd_frac_t frac_from_double(double v) {
    // Musical factors are small and simple, so a denominator of 48 covers
    // triplets, quintuplets and the usual dotted values exactly enough.
    if (v < 0) {
        v = 0;
    }
    return sd_frac((int64_t)(v * 48.0 + 0.5), 48);
}

static sd_pat_t* parse_atom(P* p) {
    skip_ws(p);
    char c = peek(p);

    if (c == '[') {
        p->i++;
        sd_pat_t* inner = parse_stack(p, ']');
        skip_ws(p);
        if (peek(p) != ']') {
            fail(p, "missing ]");
            return NULL;
        }
        p->i++;
        return inner;
    }
    if (c == '<') {
        p->i++;
        // Angle brackets are a slowcat over the terms, one per cycle
        p->depth++;
        if (p->depth > MAX_DEPTH) {
            fail(p, "nested too deep");
            return NULL;
        }
        sd_pat_t* kids[MAX_TERMS];
        int       n = 0;
        while (!p->failed) {
            skip_ws(p);
            if (peek(p) == '>' || peek(p) == '\0') {
                break;
            }
            if (n >= MAX_TERMS) {
                fail(p, "too many alternatives");
                break;
            }
            sd_pat_t* t = parse_atom(p);
            if (!t) {
                break;
            }
            kids[n++] = t;
        }
        p->depth--;
        if (peek(p) != '>') {
            fail(p, "missing >");
            return NULL;
        }
        p->i++;
        return p->failed ? NULL : sd_slowcat(p->a, kids, n);
    }
    if (c == '~') {
        p->i++;
        return sd_silence(p->a);
    }
    if (word_char(c)) {
        size_t start = p->i;
        while (p->i < p->len && word_char(p->s[p->i])) {
            p->i++;
        }
        size_t n = p->i - start;
        char   buf[SD_WORD_LEN];
        if (n >= sizeof(buf)) {
            n = sizeof(buf) - 1;
        }
        memcpy(buf, p->s + start, n);
        buf[n] = 0;

        // A lone dot or dash reads as a rest, which is what people type out of
        // habit from step grids.
        if (strcmp(buf, ".") == 0 || strcmp(buf, "-") == 0) {
            return sd_silence(p->a);
        }

        int idx = -1;
        if (peek(p) == ':') {
            p->i++;
            double d = 0;
            if (parse_num(p, &d)) {
                idx = (int)d;
            }
        }
        // A token that is purely numeric becomes a number, so gain patterns
        // and note offsets work without quoting.
        char*  end = NULL;
        double dv  = strtod(buf, &end);
        if (end && *end == 0 && n > 0) {
            return sd_pure_num(p->a, dv);
        }
        return sd_pure_word(p->a, buf, idx);
    }

    fail(p, "unexpected character");
    return NULL;
}

// An atom plus any trailing modifiers. weight is written when @ or _ applies.
static sd_pat_t* parse_term(P* p, sd_frac_t* weight, int* repeat) {
    *weight     = sd_int(1);
    *repeat     = 1;
    sd_pat_t* t = parse_atom(p);
    if (!t) {
        return NULL;
    }

    while (!p->failed && p->i < p->len) {
        char c = peek(p);
        if (c == '*' || c == '/') {
            p->i++;
            double v = 0;
            if (!parse_num(p, &v) || v <= 0) {
                fail(p, "expected a factor");
                return NULL;
            }
            sd_frac_t f = frac_from_double(v);
            t           = (c == '*') ? sd_fast(p->a, f, t) : sd_slow(p->a, f, t);
        } else if (c == '(') {
            p->i++;
            double k = 0, n = 0, rot = 0;
            if (!parse_num(p, &k)) {
                fail(p, "expected pulses");
                return NULL;
            }
            skip_ws(p);
            if (peek(p) != ',') {
                fail(p, "expected , in euclid");
                return NULL;
            }
            p->i++;
            if (!parse_num(p, &n)) {
                fail(p, "expected steps");
                return NULL;
            }
            skip_ws(p);
            if (peek(p) == ',') {
                p->i++;
                parse_num(p, &rot);
            }
            skip_ws(p);
            if (peek(p) != ')') {
                fail(p, "missing )");
                return NULL;
            }
            p->i++;
            t = sd_euclid(p->a, (int)k, (int)n, (int)rot, t);
        } else if (c == '?') {
            p->i++;
            double prob = 0.5;
            parse_num(p, &prob);
            t = sd_degrade(p->a, prob, p->seed + (p->rnd++ * 2654435761u), t);
        } else if (c == '@') {
            p->i++;
            double w = 1;
            if (!parse_num(p, &w) || w <= 0) {
                fail(p, "expected a weight");
                return NULL;
            }
            *weight = frac_from_double(w);
        } else if (c == '!') {
            p->i++;
            double r = 2;
            parse_num(p, &r);
            *repeat = (int)r < 1 ? 1 : (int)r;
        } else {
            break;
        }
    }
    return t;
}

// A whitespace separated run of terms, laid out across one cycle.
static sd_pat_t* parse_seq(P* p, char close) {
    p->depth++;
    if (p->depth > MAX_DEPTH) {
        fail(p, "nested too deep");
        p->depth--;
        return NULL;
    }
    sd_pat_t* kids[MAX_TERMS];
    sd_frac_t w[MAX_TERMS];
    int       n = 0;

    while (!p->failed) {
        skip_ws(p);
        char c = peek(p);
        if (c == '\0' || c == ',' || c == close) {
            break;
        }
        if (c == '_') {
            // Extend the previous step rather than starting a new one
            p->i++;
            if (n > 0) {
                w[n - 1] = sd_add(w[n - 1], sd_int(1));
            }
            continue;
        }
        sd_frac_t tw;
        int       rep;
        sd_pat_t* t = parse_term(p, &tw, &rep);
        if (!t) {
            break;
        }
        for (int r = 0; r < rep; r++) {
            if (n >= MAX_TERMS) {
                fail(p, "too many steps");
                break;
            }
            kids[n] = t;
            w[n]    = tw;
            n++;
        }
    }
    p->depth--;
    if (p->failed) {
        return NULL;
    }
    if (n == 0) {
        return sd_silence(p->a);
    }
    if (n == 1 && sd_eq(w[0], sd_int(1))) {
        return kids[0];
    }
    return sd_timecat(p->a, kids, w, n);
}

// Comma separated sequences played on top of each other.
static sd_pat_t* parse_stack(P* p, char close) {
    sd_pat_t* layers[MAX_TERMS];
    int       n = 0;
    while (!p->failed) {
        if (n >= MAX_TERMS) {
            fail(p, "too many layers");
            break;
        }
        sd_pat_t* l = parse_seq(p, close);
        if (!l) {
            break;
        }
        layers[n++] = l;
        skip_ws(p);
        if (peek(p) == ',') {
            p->i++;
            continue;
        }
        break;
    }
    if (p->failed) {
        return NULL;
    }
    return n == 1 ? layers[0] : sd_stack(p->a, layers, n);
}

sd_pat_t* sd_mini_parse(sd_arena_t* a, char const* src, uint32_t seed, char* err, size_t errlen) {
    if (err && errlen) {
        err[0] = 0;
    }
    if (!src) {
        return sd_silence(a);
    }
    P         p = {.s = src, .i = 0, .len = strlen(src), .a = a, .err = err, .errlen = errlen, .seed = seed};
    sd_pat_t* r = parse_stack(&p, '\0');
    skip_ws(&p);
    if (!p.failed && p.i < p.len) {
        fail(&p, "unexpected trailing input");
    }
    if (p.failed) {
        return NULL;
    }
    if (a->exhausted) {
        if (err && errlen) {
            snprintf(err, errlen, "pattern too complex");
        }
        return NULL;
    }
    return r;
}
