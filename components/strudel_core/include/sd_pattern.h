// Pattern algebra, in the TidalCycles and Strudel sense.
//
// A pattern is a pure function from a stretch of time to the events that occur
// in it. Nothing is precomputed and nothing is stored per event, so a pattern
// that repeats forever costs the same as one that plays once, and asking for
// cycle 10000 is as cheap as asking for cycle 0.
//
// Patterns are built as a small tree of nodes in an arena, so re-evaluating an
// editor buffer is a reset and a rebuild rather than a pile of frees. Querying
// allocates nothing at all: it appends into a caller supplied buffer, which
// makes it safe to call from the audio task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd_frac.h"

#define SD_WORD_LEN 16

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

typedef enum {
    SD_V_NONE = 0,
    SD_V_NUM,
    SD_V_WORD,
} sd_vtype_t;

typedef struct {
    sd_vtype_t type;
    double     num;
    char       word[SD_WORD_LEN];
    int        idx;  // the n in "bd:3", -1 when absent
} sd_val_t;

// Control fields, the named parameters a pattern can carry. Kept under 32 so
// the presence set is a single word.
typedef enum {
    SD_F_S = 0,
    SD_F_N,
    SD_F_NOTE,
    SD_F_GAIN,
    SD_F_PAN,
    SD_F_SPEED,
    SD_F_CUTOFF,
    SD_F_RESONANCE,
    SD_F_ATTACK,
    SD_F_DECAY,
    SD_F_SUSTAIN,
    SD_F_RELEASE,
    SD_F_ROOM,
    SD_F_DELAY,
    SD_F_SHAPE,
    SD_F_ORBIT,
    SD_F_LEGATO,
    SD_F_COUNT,
} sd_field_t;

// A hap's value: any set of control fields, plus the bare value it came from
// before a control was applied to it.
typedef struct {
    uint32_t set;  // bit per sd_field_t
    float    f[SD_F_COUNT];
    char     s[SD_WORD_LEN];
    int      sidx;
    sd_val_t bare;
} sd_hval_t;

static inline bool sd_has(sd_hval_t const* v, sd_field_t f) {
    return (v->set & (1u << f)) != 0;
}

// ---------------------------------------------------------------------------
// Time spans and events
// ---------------------------------------------------------------------------

typedef struct {
    sd_frac_t b, e;
} sd_span_t;

// whole is the event's full extent, part is the piece falling inside the query.
// An event with no whole is a continuous signal sampled over part.
typedef struct {
    sd_span_t whole;
    bool      has_whole;
    sd_span_t part;
    sd_hval_t v;
} sd_hap_t;

// True when this hap carries the start of its event rather than a fragment of
// one that began earlier. Only these should trigger a note.
static inline bool sd_hap_onset(sd_hap_t const* h) {
    return h->has_whole && sd_eq(h->whole.b, h->part.b);
}

typedef struct {
    sd_hap_t* haps;
    int       n;
    int       cap;
    bool      overflow;  // set when cap was reached, so callers can notice
} sd_haps_t;

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   used;
    bool     exhausted;
} sd_arena_t;

void  sd_arena_init(sd_arena_t* a, void* buf, size_t cap);
void  sd_arena_reset(sd_arena_t* a);
void* sd_arena_alloc(sd_arena_t* a, size_t bytes);

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

typedef struct sd_pat sd_pat_t;

typedef enum {
    SD_OP_SET = 0,  // right wins
    SD_OP_ADD,
    SD_OP_MUL,
} sd_op_t;

// Builders. Every one takes the arena and returns NULL if it is exhausted;
// NULL is treated as silence everywhere, so a failed build degrades rather
// than crashes.
sd_pat_t* sd_silence(sd_arena_t* a);
sd_pat_t* sd_pure(sd_arena_t* a, sd_val_t v);
sd_pat_t* sd_pure_word(sd_arena_t* a, char const* word, int idx);
sd_pat_t* sd_pure_num(sd_arena_t* a, double num);

// fastcat with per child weights, which is what "a b@3 c" needs. Pass NULL for
// weights to divide the cycle evenly.
sd_pat_t* sd_timecat(sd_arena_t* a, sd_pat_t** kids, sd_frac_t const* weights, int n);
sd_pat_t* sd_fastcat(sd_arena_t* a, sd_pat_t** kids, int n);
sd_pat_t* sd_slowcat(sd_arena_t* a, sd_pat_t** kids, int n);
sd_pat_t* sd_stack(sd_arena_t* a, sd_pat_t** kids, int n);

sd_pat_t* sd_fast(sd_arena_t* a, sd_frac_t factor, sd_pat_t* p);
sd_pat_t* sd_slow(sd_arena_t* a, sd_frac_t factor, sd_pat_t* p);
sd_pat_t* sd_late(sd_arena_t* a, sd_frac_t amount, sd_pat_t* p);
sd_pat_t* sd_early(sd_arena_t* a, sd_frac_t amount, sd_pat_t* p);
sd_pat_t* sd_rev(sd_arena_t* a, sd_pat_t* p);
sd_pat_t* sd_iter(sd_arena_t* a, int n, sd_pat_t* p);
sd_pat_t* sd_segment(sd_arena_t* a, int n, sd_pat_t* p);
sd_pat_t* sd_euclid(sd_arena_t* a, int k, int n, int rot, sd_pat_t* p);
sd_pat_t* sd_degrade(sd_arena_t* a, double prob, uint32_t seed, sd_pat_t* p);

// Moves each hap's bare value into a named control field.
sd_pat_t* sd_ctrl(sd_arena_t* a, sd_field_t field, sd_pat_t* p);
// Combines two patterns, taking structure from the left.
sd_pat_t* sd_op(sd_arena_t* a, sd_op_t op, sd_pat_t* left, sd_pat_t* right);

// Appends every event overlapping span to out. Allocates nothing.
void sd_query(sd_pat_t const* p, sd_span_t span, sd_haps_t* out);

// Fills bjorklund's euclidean rhythm into onsets, returns n.
int sd_bjorklund(int k, int n, bool* onsets, int max);

char const* sd_field_name(sd_field_t f);
// Looks up a control name, returns SD_F_COUNT when unknown.
sd_field_t sd_field_from_name(char const* name);
