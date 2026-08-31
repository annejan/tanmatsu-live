#include "sd_pattern.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

void sd_arena_init(sd_arena_t* a, void* buf, size_t cap) {
    a->buf       = (uint8_t*)buf;
    a->cap       = cap;
    a->used      = 0;
    a->exhausted = false;
}

void sd_arena_reset(sd_arena_t* a) {
    a->used      = 0;
    a->exhausted = false;
}

void* sd_arena_alloc(sd_arena_t* a, size_t bytes) {
    size_t aligned = (bytes + 7u) & ~(size_t)7u;
    if (a->used + aligned > a->cap) {
        a->exhausted = true;
        return NULL;
    }
    void* p  = a->buf + a->used;
    a->used += aligned;
    memset(p, 0, aligned);
    return p;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

typedef enum {
    P_SILENCE = 0,
    P_PURE,
    P_STACK,
    P_TIMECAT,
    P_SLOWCAT,
    P_FAST,
    P_LATE,
    P_REV,
    P_ITER,
    P_SEGMENT,
    P_EUCLID,
    P_DEGRADE,
    P_CTRL,
    P_OP,
} ptype_t;

struct sd_pat {
    ptype_t    type;
    sd_pat_t** kids;
    sd_frac_t* w;
    int        nkids;
    sd_frac_t  a;           // fast factor, or shift amount
    int        i0, i1, i2;  // euclid k/n/rot, iter n, segment n, ctrl field
    double     d0;          // degrade probability
    uint32_t   seed;
    // PURE only. Storing the bare value rather than a whole control map saves
    // about a hundred bytes per node, which is the difference between a full
    // set of sixteen parts fitting the arena and not.
    sd_val_t*  val;
    sd_op_t    op;
};

static sd_pat_t* node(sd_arena_t* a, ptype_t t) {
    sd_pat_t* p = sd_arena_alloc(a, sizeof(sd_pat_t));
    if (p) {
        p->type = t;
    }
    return p;
}

static sd_pat_t** copy_kids(sd_arena_t* a, sd_pat_t** kids, int n) {
    sd_pat_t** out = sd_arena_alloc(a, sizeof(sd_pat_t*) * (size_t)n);
    if (out) {
        memcpy(out, kids, sizeof(sd_pat_t*) * (size_t)n);
    }
    return out;
}

sd_pat_t* sd_silence(sd_arena_t* a) {
    return node(a, P_SILENCE);
}

sd_pat_t* sd_pure(sd_arena_t* a, sd_val_t v) {
    sd_pat_t* p = node(a, P_PURE);
    if (!p) {
        return NULL;
    }
    p->val = sd_arena_alloc(a, sizeof(sd_val_t));
    if (!p->val) {
        return NULL;
    }
    *p->val = v;
    return p;
}

sd_pat_t* sd_pure_word(sd_arena_t* a, char const* word, int idx) {
    sd_val_t v = {0};
    v.type     = SD_V_WORD;
    v.idx      = idx;
    snprintf(v.word, sizeof(v.word), "%s", word ? word : "");
    return sd_pure(a, v);
}

sd_pat_t* sd_pure_num(sd_arena_t* a, double num) {
    sd_val_t v = {0};
    v.type     = SD_V_NUM;
    v.num      = num;
    v.idx      = -1;
    return sd_pure(a, v);
}

sd_pat_t* sd_timecat(sd_arena_t* a, sd_pat_t** kids, sd_frac_t const* weights, int n) {
    if (n <= 0) {
        return sd_silence(a);
    }
    sd_pat_t* p = node(a, P_TIMECAT);
    if (!p) {
        return NULL;
    }
    p->kids  = copy_kids(a, kids, n);
    p->nkids = n;
    if (!p->kids) {
        return NULL;
    }
    // Equal weights are the overwhelmingly common case and need no array: a
    // sixteen step grid was spending 256 bytes storing the number one.
    bool uniform = true;
    if (weights) {
        for (int i = 0; i < n && uniform; i++) {
            uniform = sd_eq(weights[i], sd_int(1));
        }
    }
    if (uniform) {
        p->w = NULL;
        return p;
    }
    p->w = sd_arena_alloc(a, sizeof(sd_frac_t) * (size_t)n);
    if (!p->w) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        p->w[i] = weights[i];
    }
    return p;
}

sd_pat_t* sd_fastcat(sd_arena_t* a, sd_pat_t** kids, int n) {
    return sd_timecat(a, kids, NULL, n);
}

static sd_pat_t* kids_node(sd_arena_t* a, ptype_t t, sd_pat_t** kids, int n) {
    if (n <= 0) {
        return sd_silence(a);
    }
    if (n == 1) {
        return kids[0];
    }
    sd_pat_t* p = node(a, t);
    if (!p) {
        return NULL;
    }
    p->kids  = copy_kids(a, kids, n);
    p->nkids = n;
    return p->kids ? p : NULL;
}

sd_pat_t* sd_slowcat(sd_arena_t* a, sd_pat_t** kids, int n) {
    return kids_node(a, P_SLOWCAT, kids, n);
}

sd_pat_t* sd_stack(sd_arena_t* a, sd_pat_t** kids, int n) {
    return kids_node(a, P_STACK, kids, n);
}

static sd_pat_t* unary(sd_arena_t* a, ptype_t t, sd_pat_t* p) {
    if (!p) {
        return NULL;
    }
    sd_pat_t* q = node(a, t);
    if (!q) {
        return NULL;
    }
    q->kids  = copy_kids(a, &p, 1);
    q->nkids = 1;
    return q->kids ? q : NULL;
}

sd_pat_t* sd_fast(sd_arena_t* a, sd_frac_t factor, sd_pat_t* p) {
    if (factor.n <= 0) {
        return sd_silence(a);
    }
    sd_pat_t* q = unary(a, P_FAST, p);
    if (q) {
        q->a = factor;
    }
    return q;
}

sd_pat_t* sd_slow(sd_arena_t* a, sd_frac_t factor, sd_pat_t* p) {
    if (factor.n <= 0) {
        return sd_silence(a);
    }
    return sd_fast(a, sd_div(sd_int(1), factor), p);
}

sd_pat_t* sd_late(sd_arena_t* a, sd_frac_t amount, sd_pat_t* p) {
    sd_pat_t* q = unary(a, P_LATE, p);
    if (q) {
        q->a = amount;
    }
    return q;
}

sd_pat_t* sd_early(sd_arena_t* a, sd_frac_t amount, sd_pat_t* p) {
    return sd_late(a, sd_sub(sd_int(0), amount), p);
}

sd_pat_t* sd_rev(sd_arena_t* a, sd_pat_t* p) {
    return unary(a, P_REV, p);
}

sd_pat_t* sd_iter(sd_arena_t* a, int n, sd_pat_t* p) {
    if (n <= 1) {
        return p;
    }
    sd_pat_t* q = unary(a, P_ITER, p);
    if (q) {
        q->i0 = n;
    }
    return q;
}

sd_pat_t* sd_segment(sd_arena_t* a, int n, sd_pat_t* p) {
    if (n <= 0) {
        return p;
    }
    sd_pat_t* q = unary(a, P_SEGMENT, p);
    if (q) {
        q->i0 = n;
    }
    return q;
}

sd_pat_t* sd_euclid(sd_arena_t* a, int k, int n, int rot, sd_pat_t* p) {
    if (n <= 0) {
        return sd_silence(a);
    }
    sd_pat_t* q = unary(a, P_EUCLID, p);
    if (q) {
        q->i0 = k;
        q->i1 = n;
        q->i2 = rot;
    }
    return q;
}

sd_pat_t* sd_degrade(sd_arena_t* a, double prob, uint32_t seed, sd_pat_t* p) {
    sd_pat_t* q = unary(a, P_DEGRADE, p);
    if (q) {
        q->d0   = prob;
        q->seed = seed;
    }
    return q;
}

sd_pat_t* sd_ctrl(sd_arena_t* a, sd_field_t field, sd_pat_t* p) {
    sd_pat_t* q = unary(a, P_CTRL, p);
    if (q) {
        q->i0 = (int)field;
    }
    return q;
}

sd_pat_t* sd_op(sd_arena_t* a, sd_op_t op, sd_pat_t* left, sd_pat_t* right) {
    if (!left || !right) {
        return left ? left : right;
    }
    sd_pat_t* kids[2] = {left, right};
    sd_pat_t* q       = node(a, P_OP);
    if (!q) {
        return NULL;
    }
    q->kids  = copy_kids(a, kids, 2);
    q->nkids = 2;
    q->op    = op;
    return q->kids ? q : NULL;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void haps_push(sd_haps_t* out, sd_hap_t const* h) {
    if (out->n >= out->cap) {
        out->overflow = true;
        return;
    }
    out->haps[out->n++] = *h;
}

static sd_span_t span_sect(sd_span_t a, sd_span_t b) {
    sd_span_t r = {sd_max(a.b, b.b), sd_min(a.e, b.e)};
    return r;
}

static bool span_valid(sd_span_t s) {
    return sd_lte(s.b, s.e);
}

static int64_t floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static double hash01(sd_frac_t t, uint32_t seed) {
    uint64_t x  = (uint64_t)t.n * 0x9E3779B97F4A7C15ull;
    x          ^= (uint64_t)t.d << 32;
    x          ^= (uint64_t)seed * 0xBF58476D1CE4E5B9ull;
    x          ^= x >> 30;
    x          *= 0xBF58476D1CE4E5B9ull;
    x          ^= x >> 27;
    x          *= 0x94D049BB133111EBull;
    x          ^= x >> 31;
    return (double)(x >> 11) / 9007199254740992.0;
}

int sd_bjorklund(int k, int n, bool* onsets, int max) {
    if (n <= 0 || n > max) {
        return 0;
    }
    if (k <= 0) {
        for (int i = 0; i < n; i++) {
            onsets[i] = false;
        }
        return n;
    }
    if (k >= n) {
        for (int i = 0; i < n; i++) {
            onsets[i] = true;
        }
        return n;
    }
    // The Bresenham form of the euclidean rhythm, which agrees with Tidal's
    // bjorklund for every (k,n): E(3,8) is x..x..x.
    int prev = -1;
    for (int i = 0; i < n; i++) {
        int cur   = (i * k) / n;
        onsets[i] = (cur != prev);
        prev      = cur;
    }
    return n;
}

static void note_from_word(char const* w, float* out) {
    static int8_t const semis[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    size_t              len      = strlen(w);
    if (len == 0) {
        return;
    }
    char c = (char)(w[0] | 0x20);
    if (c < 'a' || c > 'g') {
        return;
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
    *out = (float)(midi + 12 * (oct + 1));  // c3 is midi 48
}

static void hval_merge(sd_hval_t* dst, sd_hval_t const* src, sd_op_t op) {
    for (int f = 0; f < SD_F_COUNT; f++) {
        if (!(src->set & (1u << f))) {
            continue;
        }
        if (f == SD_F_S) {
            memcpy(dst->s, src->s, sizeof(dst->s));
            dst->sidx = src->sidx;
        } else if (!(dst->set & (1u << f)) || op == SD_OP_SET) {
            dst->f[f] = src->f[f];
        } else if (op == SD_OP_ADD) {
            dst->f[f] += src->f[f];
        } else {
            dst->f[f] *= src->f[f];
        }
        dst->set |= (1u << f);
    }
    if (dst->bare.type == SD_V_NONE) {
        dst->bare = src->bare;
    }
}

// Maps every hap appended since mark through a linear time transform,
// t -> t * mul + add, and clips the part to a limit when one is given.
static void remap(sd_haps_t* out, int mark, sd_frac_t mul, sd_frac_t add, sd_span_t const* clip) {
    int w = mark;
    for (int i = mark; i < out->n; i++) {
        sd_hap_t h = out->haps[i];
        h.part.b   = sd_add(sd_mul(h.part.b, mul), add);
        h.part.e   = sd_add(sd_mul(h.part.e, mul), add);
        if (h.has_whole) {
            h.whole.b = sd_add(sd_mul(h.whole.b, mul), add);
            h.whole.e = sd_add(sd_mul(h.whole.e, mul), add);
        }
        if (clip) {
            h.part = span_sect(h.part, *clip);
            if (!span_valid(h.part) || sd_eq(h.part.b, h.part.e)) {
                continue;  // fell entirely outside the slot
            }
        }
        out->haps[w++] = h;
    }
    out->n = w;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

static void query_slotted(sd_pat_t const* p, sd_span_t piece, sd_haps_t* out, sd_frac_t c, sd_frac_t start,
                          sd_frac_t len, sd_pat_t const* kid) {
    if (len.n <= 0 || !kid) {
        return;
    }
    sd_span_t slot  = {sd_add(c, start), sd_add(c, sd_add(start, len))};
    sd_span_t inter = span_sect(piece, slot);
    if (!span_valid(inter) || (sd_eq(inter.b, inter.e) && !sd_eq(piece.b, piece.e))) {
        return;
    }
    // Child time runs a full cycle across the slot
    sd_frac_t off = sd_add(c, start);
    sd_span_t q   = {sd_add(c, sd_div(sd_sub(inter.b, off), len)), sd_add(c, sd_div(sd_sub(inter.e, off), len))};

    int mark = out->n;
    sd_query(kid, q, out);
    // t = off + (child - c) * len
    remap(out, mark, len, sd_sub(off, sd_mul(c, len)), &slot);
    (void)p;
}

void sd_query(sd_pat_t const* p, sd_span_t span, sd_haps_t* out) {
    if (!p || out->n >= out->cap) {
        return;
    }
    if (!span_valid(span)) {
        return;
    }

    switch (p->type) {
        case P_SILENCE:
            return;

        case P_STACK:
            for (int i = 0; i < p->nkids; i++) {
                sd_query(p->kids[i], span, out);
            }
            return;

        case P_FAST: {
            int       mark = out->n;
            sd_span_t q    = {sd_mul(span.b, p->a), sd_mul(span.e, p->a)};
            sd_query(p->kids[0], q, out);
            remap(out, mark, sd_div(sd_int(1), p->a), sd_int(0), NULL);
            return;
        }

        case P_LATE: {
            int       mark = out->n;
            sd_span_t q    = {sd_sub(span.b, p->a), sd_sub(span.e, p->a)};
            sd_query(p->kids[0], q, out);
            remap(out, mark, sd_int(1), p->a, NULL);
            return;
        }

        case P_CTRL: {
            int mark = out->n;
            sd_query(p->kids[0], span, out);
            sd_field_t f = (sd_field_t)p->i0;
            for (int i = mark; i < out->n; i++) {
                sd_hval_t* v = &out->haps[i].v;
                if (f == SD_F_S) {
                    if (v->bare.type == SD_V_WORD) {
                        memcpy(v->s, v->bare.word, sizeof(v->s));
                        v->sidx  = v->bare.idx;
                        v->set  |= (1u << SD_F_S);
                    }
                } else {
                    float x = 0.0f;
                    if (v->bare.type == SD_V_NUM) {
                        x = (float)v->bare.num;
                    } else if (v->bare.type == SD_V_WORD) {
                        if (f == SD_F_NOTE) {
                            note_from_word(v->bare.word, &x);
                        } else {
                            continue;  // a word means nothing to a numeric control
                        }
                    } else {
                        continue;
                    }
                    v->f[f]  = x;
                    v->set  |= (1u << f);
                }
            }
            return;
        }

        case P_OP: {
            int mark = out->n;
            sd_query(p->kids[0], span, out);
            int left_end = out->n;
            for (int i = mark; i < left_end; i++) {
                sd_span_t probe = out->haps[i].part;
                int       tmp   = out->n;
                sd_query(p->kids[1], probe, out);
                for (int j = tmp; j < out->n; j++) {
                    sd_span_t x = span_sect(out->haps[i].part, out->haps[j].part);
                    if (span_valid(x)) {
                        hval_merge(&out->haps[i].v, &out->haps[j].v, p->op);
                        break;
                    }
                }
                out->n = tmp;  // drop the right hand haps, structure is the left's
            }
            return;
        }

        case P_DEGRADE: {
            int mark = out->n;
            sd_query(p->kids[0], span, out);
            int w = mark;
            for (int i = mark; i < out->n; i++) {
                sd_frac_t at = out->haps[i].has_whole ? out->haps[i].whole.b : out->haps[i].part.b;
                if (hash01(at, p->seed) >= p->d0) {
                    out->haps[w++] = out->haps[i];
                }
            }
            out->n = w;
            return;
        }

        default:
            break;
    }

    // Everything below reasons one cycle at a time
    sd_frac_t b          = span.b;
    bool      zero_width = sd_eq(span.b, span.e);
    do {
        sd_frac_t nb    = sd_add(sd_floor(b), sd_int(1));
        sd_frac_t pe    = zero_width ? b : sd_min(nb, span.e);
        sd_span_t piece = {b, pe};
        sd_frac_t c     = sd_floor(b);

        switch (p->type) {
            case P_PURE: {
                sd_hap_t h  = {0};
                h.has_whole = true;
                h.whole.b   = c;
                h.whole.e   = sd_add(c, sd_int(1));
                h.part      = piece;
                h.v.bare    = *p->val;
                h.v.sidx    = p->val->idx;
                haps_push(out, &h);
                break;
            }

            case P_TIMECAT: {
                sd_frac_t total = sd_int(0);
                for (int i = 0; i < p->nkids; i++) {
                    total = sd_add(total, p->w ? p->w[i] : sd_int(1));
                }
                if (total.n <= 0) {
                    break;
                }
                sd_frac_t acc = sd_int(0);
                for (int i = 0; i < p->nkids; i++) {
                    sd_frac_t wi    = p->w ? p->w[i] : sd_int(1);
                    sd_frac_t start = sd_div(acc, total);
                    sd_frac_t len   = sd_div(wi, total);
                    query_slotted(p, piece, out, c, start, len, p->kids[i]);
                    acc = sd_add(acc, wi);
                }
                break;
            }

            case P_EUCLID: {
                bool onsets[64];
                int  n = sd_bjorklund(p->i0, p->i1, onsets, 64);
                if (n <= 0) {
                    break;
                }
                sd_frac_t len = sd_frac(1, n);
                for (int i = 0; i < n; i++) {
                    int j = ((i - p->i2) % n + n) % n;
                    if (!onsets[j]) {
                        continue;
                    }
                    query_slotted(p, piece, out, c, sd_frac(i, n), len, p->kids[0]);
                }
                break;
            }

            case P_SLOWCAT: {
                int64_t   ci     = sd_floori(b);
                int       n      = p->nkids;
                int       idx    = (int)(((ci % n) + n) % n);
                int64_t   offset = ci - floordiv(ci, n);
                sd_frac_t o      = sd_int(offset);
                sd_span_t q      = {sd_sub(piece.b, o), sd_sub(piece.e, o)};
                int       mark   = out->n;
                sd_query(p->kids[idx], q, out);
                remap(out, mark, sd_int(1), o, NULL);
                break;
            }

            case P_REV: {
                // Reflect the cycle: t -> 2c + 1 - t
                sd_frac_t pivot = sd_add(sd_mul(c, sd_int(2)), sd_int(1));
                sd_span_t q     = {sd_sub(pivot, piece.e), sd_sub(pivot, piece.b)};
                int       mark  = out->n;
                sd_query(p->kids[0], q, out);
                for (int i = mark; i < out->n; i++) {
                    sd_hap_t* h  = &out->haps[i];
                    sd_frac_t pb = sd_sub(pivot, h->part.e), pe2 = sd_sub(pivot, h->part.b);
                    h->part.b = pb;
                    h->part.e = pe2;
                    if (h->has_whole) {
                        sd_frac_t wb = sd_sub(pivot, h->whole.e), we = sd_sub(pivot, h->whole.b);
                        h->whole.b = wb;
                        h->whole.e = we;
                    }
                }
                break;
            }

            case P_ITER: {
                int64_t   ci    = sd_floori(b);
                int       n     = p->i0;
                sd_frac_t shift = sd_frac(((ci % n) + n) % n, n);
                sd_span_t q     = {sd_add(piece.b, shift), sd_add(piece.e, shift)};
                int       mark  = out->n;
                sd_query(p->kids[0], q, out);
                remap(out, mark, sd_int(1), sd_sub(sd_int(0), shift), NULL);
                break;
            }

            case P_SEGMENT: {
                int n = p->i0;
                for (int i = 0; i < n; i++) {
                    sd_span_t slot  = {sd_add(c, sd_frac(i, n)), sd_add(c, sd_frac(i + 1, n))};
                    sd_span_t inter = span_sect(piece, slot);
                    if (!span_valid(inter) || (sd_eq(inter.b, inter.e) && !zero_width)) {
                        continue;
                    }
                    int tmp = out->n;
                    sd_query(p->kids[0], slot, out);
                    if (out->n > tmp) {
                        sd_hval_t v = out->haps[tmp].v;
                        out->n      = tmp;
                        sd_hap_t h  = {0};
                        h.has_whole = true;
                        h.whole     = slot;
                        h.part      = inter;
                        h.v         = v;
                        haps_push(out, &h);
                    } else {
                        out->n = tmp;
                    }
                }
                break;
            }

            default:
                break;
        }

        b = pe;
        if (zero_width) {
            break;
        }
    } while (sd_lt(b, span.e));
}

// ---------------------------------------------------------------------------
// Field names
// ---------------------------------------------------------------------------

static char const* const field_names[SD_F_COUNT] = {
    "s",     "n",       "note",    "gain", "pan",   "speed", "cutoff", "resonance", "attack",
    "decay", "sustain", "release", "room", "delay", "shape", "orbit",  "legato",
};

char const* sd_field_name(sd_field_t f) {
    return (f >= 0 && f < SD_F_COUNT) ? field_names[f] : "";
}

sd_field_t sd_field_from_name(char const* name) {
    if (!name) {
        return SD_F_COUNT;
    }
    for (int i = 0; i < SD_F_COUNT; i++) {
        if (strcmp(name, field_names[i]) == 0) {
            return (sd_field_t)i;
        }
    }
    // Short aliases, because a line is 95 columns and a control clause should
    // not eat a tenth of it spelling out "resonance".
    static struct {
        char const* alias;
        sd_field_t  field;
    } const aliases[] = {
        {"sound", SD_F_S},        {"lpf", SD_F_CUTOFF},   {"res", SD_F_RESONANCE},
        {"g", SD_F_GAIN},         {"p", SD_F_PAN},        {"c", SD_F_CUTOFF},
        {"q", SD_F_RESONANCE},    {"nt", SD_F_NOTE},      {"sp", SD_F_SPEED},
        {"lg", SD_F_LEGATO},      {"atk", SD_F_ATTACK},   {"dec", SD_F_DECAY},
        {"rel", SD_F_RELEASE},    {"sus", SD_F_SUSTAIN},  {"shp", SD_F_SHAPE},
        {"rm", SD_F_ROOM},        {"dl", SD_F_DELAY},     {"ob", SD_F_ORBIT},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].alias) == 0) {
            return aliases[i].field;
        }
    }
    return SD_F_COUNT;
}
