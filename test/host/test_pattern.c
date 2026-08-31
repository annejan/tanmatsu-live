// Correctness tests for the pattern engine. The query model is subtle enough
// that every combinator gets pinned to exact rational positions here.
#include <stdio.h>
#include <string.h>

#include "sd_pattern.h"

static int failures = 0;
static int checks   = 0;

static void ok(int cond, char const* what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static uint8_t  arena_buf[64 * 1024];
static sd_arena_t arena;
static sd_hap_t haps_buf[256];

static sd_haps_t query(sd_pat_t* p, int cyc_from, int cyc_to) {
    static sd_haps_t out;
    out.haps     = haps_buf;
    out.cap      = (int)(sizeof(haps_buf) / sizeof(haps_buf[0]));
    out.n        = 0;
    out.overflow = false;
    sd_span_t s  = {sd_int(cyc_from), sd_int(cyc_to)};
    sd_query(p, s, &out);
    return out;
}

// Is there an onset at exactly num/den carrying this word?
static bool onset_at(sd_haps_t const* h, int num, int den, char const* word) {
    sd_frac_t want = sd_frac(num, den);
    for (int i = 0; i < h->n; i++) {
        if (!sd_hap_onset(&h->haps[i])) {
            continue;
        }
        if (!sd_eq(h->haps[i].whole.b, want)) {
            continue;
        }
        if (!word) {
            return true;
        }
        if (strcmp(h->haps[i].v.bare.word, word) == 0 || strcmp(h->haps[i].v.s, word) == 0) {
            return true;
        }
    }
    return false;
}

static int onset_count(sd_haps_t const* h) {
    int n = 0;
    for (int i = 0; i < h->n; i++) {
        if (sd_hap_onset(&h->haps[i])) {
            n++;
        }
    }
    return n;
}

int main(void) {
    // ---- fractions -------------------------------------------------------
    ok(sd_floori(sd_frac(-1, 2)) == -1, "floor of -1/2 is -1");
    ok(sd_floori(sd_frac(3, 2)) == 1, "floor of 3/2 is 1");
    ok(sd_eq(sd_cyclepos(sd_frac(-1, 2)), sd_frac(1, 2)), "cyclepos of -1/2 is 1/2");
    ok(sd_eq(sd_add(sd_frac(1, 3), sd_frac(1, 6)), sd_frac(1, 2)), "1/3 + 1/6 = 1/2");
    ok(sd_eq(sd_mul(sd_frac(2, 3), sd_frac(3, 4)), sd_frac(1, 2)), "2/3 * 3/4 = 1/2");

    sd_arena_init(&arena, arena_buf, sizeof(arena_buf));

    // ---- pure ------------------------------------------------------------
    {
        sd_pat_t*  p = sd_pure_word(&arena, "bd", -1);
        sd_haps_t  h = query(p, 0, 1);
        ok(h.n == 1, "pure gives one event per cycle");
        ok(onset_at(&h, 0, 1, "bd"), "pure onset at 0");
        h = query(p, 0, 3);
        ok(onset_count(&h) == 3, "pure repeats every cycle");
    }

    // ---- fastcat: "bd sd" ------------------------------------------------
    {
        sd_pat_t* k[2] = {sd_pure_word(&arena, "bd", -1), sd_pure_word(&arena, "sd", -1)};
        sd_pat_t* p    = sd_fastcat(&arena, k, 2);
        sd_haps_t h    = query(p, 0, 1);
        ok(onset_count(&h) == 2, "[bd sd] has two onsets");
        ok(onset_at(&h, 0, 1, "bd"), "bd at 0");
        ok(onset_at(&h, 1, 2, "sd"), "sd at 1/2");
    }

    // ---- nested: "bd [sd sd]" -------------------------------------------
    {
        sd_pat_t* inner[2] = {sd_pure_word(&arena, "sd", -1), sd_pure_word(&arena, "sd", -1)};
        sd_pat_t* k[2]     = {sd_pure_word(&arena, "bd", -1), sd_fastcat(&arena, inner, 2)};
        sd_pat_t* p        = sd_fastcat(&arena, k, 2);
        sd_haps_t h        = query(p, 0, 1);
        ok(onset_count(&h) == 3, "bd [sd sd] has three onsets");
        ok(onset_at(&h, 0, 1, "bd"), "bd at 0");
        ok(onset_at(&h, 1, 2, "sd"), "sd at 1/2");
        ok(onset_at(&h, 3, 4, "sd"), "sd at 3/4");
    }

    // ---- weights: "bd@3 sd" ---------------------------------------------
    {
        sd_pat_t* k[2]  = {sd_pure_word(&arena, "bd", -1), sd_pure_word(&arena, "sd", -1)};
        sd_frac_t w[2]  = {sd_int(3), sd_int(1)};
        sd_pat_t* p     = sd_timecat(&arena, k, w, 2);
        sd_haps_t h     = query(p, 0, 1);
        ok(onset_count(&h) == 2, "bd@3 sd has two onsets");
        ok(onset_at(&h, 0, 1, "bd"), "weighted bd at 0");
        ok(onset_at(&h, 3, 4, "sd"), "weighted sd at 3/4");
    }

    // ---- slowcat: "<bd sd>" ---------------------------------------------
    {
        sd_pat_t* k[2] = {sd_pure_word(&arena, "bd", -1), sd_pure_word(&arena, "sd", -1)};
        sd_pat_t* p    = sd_slowcat(&arena, k, 2);
        sd_haps_t h    = query(p, 0, 1);
        ok(onset_count(&h) == 1 && onset_at(&h, 0, 1, "bd"), "<bd sd> cycle 0 is bd");
        h = query(p, 1, 2);
        ok(onset_count(&h) == 1 && onset_at(&h, 1, 1, "sd"), "<bd sd> cycle 1 is sd");
        h = query(p, 2, 3);
        ok(onset_count(&h) == 1 && onset_at(&h, 2, 1, "bd"), "<bd sd> cycle 2 is bd again");
    }

    // ---- fast and slow ---------------------------------------------------
    {
        sd_pat_t* p = sd_fast(&arena, sd_int(4), sd_pure_word(&arena, "hh", -1));
        sd_haps_t h = query(p, 0, 1);
        ok(onset_count(&h) == 4, "hh*4 has four onsets");
        ok(onset_at(&h, 1, 4, "hh") && onset_at(&h, 3, 4, "hh"), "hh*4 lands on quarters");

        sd_pat_t* q = sd_slow(&arena, sd_int(2), sd_pure_word(&arena, "bd", -1));
        h           = query(q, 0, 2);
        ok(onset_count(&h) == 1, "bd/2 sounds once every two cycles");
    }

    // ---- euclid ----------------------------------------------------------
    {
        sd_pat_t* p = sd_euclid(&arena, 3, 8, 0, sd_pure_word(&arena, "bd", -1));
        sd_haps_t h = query(p, 0, 1);
        ok(onset_count(&h) == 3, "bd(3,8) has three onsets");
        ok(onset_at(&h, 0, 1, "bd"), "euclid onset at 0");
        ok(onset_at(&h, 3, 8, "bd"), "euclid onset at 3/8");
        ok(onset_at(&h, 6, 8, "bd"), "euclid onset at 6/8");

        bool o[16];
        sd_bjorklund(5, 8, o, 16);
        int c = 0;
        for (int i = 0; i < 8; i++) {
            c += o[i] ? 1 : 0;
        }
        ok(c == 5, "bjorklund(5,8) has five onsets");
    }

    // ---- stack -----------------------------------------------------------
    {
        sd_pat_t* k[2] = {sd_pure_word(&arena, "bd", -1),
                          sd_fast(&arena, sd_int(2), sd_pure_word(&arena, "hh", -1))};
        sd_pat_t* p    = sd_stack(&arena, k, 2);
        sd_haps_t h    = query(p, 0, 1);
        ok(onset_count(&h) == 3, "stack merges both layers");
    }

    // ---- rev -------------------------------------------------------------
    {
        sd_pat_t* k[2] = {sd_pure_word(&arena, "bd", -1), sd_pure_word(&arena, "sd", -1)};
        sd_pat_t* p    = sd_rev(&arena, sd_fastcat(&arena, k, 2));
        sd_haps_t h    = query(p, 0, 1);
        ok(onset_at(&h, 0, 1, "sd"), "rev puts sd first");
        ok(onset_at(&h, 1, 2, "bd"), "rev puts bd second");
    }

    // ---- controls --------------------------------------------------------
    {
        sd_pat_t* p = sd_ctrl(&arena, SD_F_S, sd_pure_word(&arena, "bd", -1));
        sd_haps_t h = query(p, 0, 1);
        ok(h.n == 1 && sd_has(&h.haps[0].v, SD_F_S), "s() sets the sound field");
        ok(strcmp(h.haps[0].v.s, "bd") == 0, "s() carries the word");

        sd_pat_t* q = sd_ctrl(&arena, SD_F_NOTE, sd_pure_word(&arena, "c3", -1));
        h           = query(q, 0, 1);
        ok(h.n == 1 && h.haps[0].v.f[SD_F_NOTE] == 48.0f, "note(c3) is midi 48");
    }

    // ---- combining: s("bd") # gain(0.8) ---------------------------------
    {
        sd_pat_t* left  = sd_ctrl(&arena, SD_F_S, sd_pure_word(&arena, "bd", -1));
        sd_pat_t* right = sd_ctrl(&arena, SD_F_GAIN, sd_pure_num(&arena, 0.8));
        sd_pat_t* p     = sd_op(&arena, SD_OP_SET, left, right);
        sd_haps_t h     = query(p, 0, 1);
        ok(h.n == 1, "op takes structure from the left");
        ok(sd_has(&h.haps[0].v, SD_F_S) && sd_has(&h.haps[0].v, SD_F_GAIN), "op merges both fields");
        ok(h.haps[0].v.f[SD_F_GAIN] > 0.79f && h.haps[0].v.f[SD_F_GAIN] < 0.81f, "gain came through");
    }

    // structure comes from the left even when the right subdivides
    {
        sd_pat_t* gk[2] = {sd_pure_num(&arena, 0.2), sd_pure_num(&arena, 0.9)};
        sd_pat_t* left  = sd_ctrl(&arena, SD_F_S, sd_fast(&arena, sd_int(4), sd_pure_word(&arena, "hh", -1)));
        sd_pat_t* right = sd_ctrl(&arena, SD_F_GAIN, sd_fastcat(&arena, gk, 2));
        sd_haps_t h     = query(sd_op(&arena, SD_OP_SET, left, right), 0, 1);
        ok(onset_count(&h) == 4, "four hats keep their structure");
        int quiet = 0, loud = 0;
        for (int i = 0; i < h.n; i++) {
            if (h.haps[i].v.f[SD_F_GAIN] < 0.5f) {
                quiet++;
            } else {
                loud++;
            }
        }
        ok(quiet == 2 && loud == 2, "gain pattern splits the hats two and two");
    }

    // ---- degrade is deterministic ---------------------------------------
    {
        sd_pat_t* p  = sd_degrade(&arena, 0.5, 1234, sd_fast(&arena, sd_int(16), sd_pure_word(&arena, "hh", -1)));
        sd_haps_t h1 = query(p, 0, 4);
        int       a  = onset_count(&h1);
        sd_haps_t h2 = query(p, 0, 4);
        ok(a == onset_count(&h2), "degrade gives the same answer twice");
        ok(a > 10 && a < 54, "degrade drops roughly half of 64");
    }

    // ---- partial windows, which is how the scheduler actually queries ----
    {
        sd_pat_t* p     = sd_fast(&arena, sd_int(4), sd_pure_word(&arena, "hh", -1));
        int       total = 0;
        for (int i = 0; i < 8; i++) {
            // eighth-cycle windows, as the audio task would ask for
            static sd_haps_t out;
            out.haps    = haps_buf;
            out.cap     = 256;
            out.n       = 0;
            sd_span_t s = {sd_frac(i, 8), sd_frac(i + 1, 8)};
            sd_query(p, s, &out);
            total += onset_count(&out);
        }
        ok(total == 4, "an onset is reported exactly once across adjacent windows");
    }

    // ---- iter ------------------------------------------------------------
    {
        sd_pat_t* k[4] = {sd_pure_word(&arena, "a", -1), sd_pure_word(&arena, "b", -1),
                          sd_pure_word(&arena, "c", -1), sd_pure_word(&arena, "d", -1)};
        sd_pat_t* p    = sd_iter(&arena, 4, sd_fastcat(&arena, k, 4));
        sd_haps_t h    = query(p, 0, 1);
        ok(onset_at(&h, 0, 1, "a"), "iter cycle 0 starts on a");
        h = query(p, 1, 2);
        ok(onset_at(&h, 1, 1, "b"), "iter cycle 1 starts on b");
    }

    ok(!arena.exhausted, "arena had room");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
