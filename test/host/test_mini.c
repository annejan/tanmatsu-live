// Mini notation parser tests. Each case pins the events to exact positions.
#include <stdio.h>
#include <string.h>

#include "sd_mini.h"

static int failures = 0;
static int checks   = 0;

static void ok(int cond, char const* what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static uint8_t    arena_buf[64 * 1024];
static sd_arena_t arena;
static sd_hap_t   haps_buf[256];
static sd_haps_t  out;

static sd_haps_t* run(char const* src, int cyc) {
    sd_arena_reset(&arena);
    char      err[96] = "";
    sd_pat_t* p       = sd_mini_parse(&arena, src, 1, err, sizeof(err));
    out.haps          = haps_buf;
    out.cap           = 256;
    out.n             = 0;
    if (!p) {
        printf("  parse error for \"%s\": %s\n", src, err);
        return &out;
    }
    sd_span_t s = {sd_int(cyc), sd_int(cyc + 1)};
    sd_query(p, s, &out);
    return &out;
}

static int onsets(sd_haps_t const* h) {
    int n = 0;
    for (int i = 0; i < h->n; i++) {
        if (sd_hap_onset(&h->haps[i])) {
            n++;
        }
    }
    return n;
}

static bool at(sd_haps_t const* h, int num, int den, char const* word) {
    sd_frac_t want = sd_frac(num, den);
    for (int i = 0; i < h->n; i++) {
        if (sd_hap_onset(&h->haps[i]) && sd_eq(h->haps[i].whole.b, want)) {
            if (!word) {
                return true;
            }
            if (strcmp(h->haps[i].v.bare.word, word) == 0) {
                return true;
            }
        }
    }
    return false;
}

int main(void) {
    sd_arena_init(&arena, arena_buf, sizeof(arena_buf));

    ok(onsets(run("bd sd", 0)) == 2, "\"bd sd\" is two steps");
    ok(at(run("bd sd", 0), 1, 2, "sd"), "sd lands on the half");

    ok(onsets(run("bd [sd sd]", 0)) == 3, "brackets subdivide");
    ok(at(run("bd [sd sd]", 0), 3, 4, "sd"), "subdivision lands on 3/4");

    ok(onsets(run("bd*4", 0)) == 4, "* repeats within the step");
    ok(onsets(run("bd/2", 0)) + onsets(run("bd/2", 1)) == 1, "/ stretches over two cycles");

    ok(at(run("<bd sd>", 0), 0, 1, "bd"), "<> picks bd on cycle 0");
    ok(at(run("<bd sd>", 1), 1, 1, "sd"), "<> picks sd on cycle 1");

    {
        sd_haps_t* h = run("bd(3,8)", 0);
        ok(onsets(h) == 3, "bd(3,8) is three onsets");
        ok(at(h, 0, 1, "bd") && at(h, 3, 8, "bd") && at(h, 6, 8, "bd"), "euclid positions");
    }
    ok(onsets(run("bd(5,8)", 0)) == 5, "bd(5,8) is five onsets");
    ok(onsets(run("bd(3,8,2)", 0)) == 3, "euclid accepts a rotation");

    ok(onsets(run("bd!3", 0)) == 3, "! repeats as separate steps");
    ok(at(run("bd@3 sd", 0), 3, 4, "sd"), "@ weights the step");
    ok(at(run("bd _ sd", 0), 2, 3, "sd"), "_ extends the previous step");

    ok(onsets(run("bd, hh*4", 0)) == 5, ", stacks layers");
    ok(onsets(run("~ sd", 0)) == 1, "~ is a rest");
    ok(onsets(run("bd . sd", 0)) == 2, "a lone dot is a rest too");

    {
        sd_haps_t* h = run("bd:3", 0);
        ok(h->n == 1 && h->haps[0].v.bare.idx == 3, "bd:3 carries the sample index");
    }
    {
        sd_haps_t* h = run("0.5 1", 0);
        ok(h->n == 2 && h->haps[0].v.bare.type == SD_V_NUM, "bare numbers parse as numbers");
        ok(h->haps[0].v.bare.num > 0.49 && h->haps[0].v.bare.num < 0.51, "0.5 survives");
    }

    // degrade is random but bounded
    {
        int total = 0;
        for (int c = 0; c < 8; c++) {
            total += onsets(run("hh*8?", c));
        }
        ok(total > 8 && total < 60, "? drops some but not all");
    }

    // combinations that should not explode
    ok(onsets(run("[bd sd]*2, hh(5,8), <c2 eb2>", 0)) > 0, "a busy line parses");
    ok(onsets(run("bd [sd [hh hh]]", 0)) == 4, "nesting two deep");

    // errors are reported rather than crashing
    {
        char      err[96] = "";
        sd_arena_reset(&arena);
        ok(sd_mini_parse(&arena, "bd [sd", 1, err, sizeof(err)) == NULL && err[0], "unclosed bracket errors");
        sd_arena_reset(&arena);
        ok(sd_mini_parse(&arena, "bd <sd", 1, err, sizeof(err)) == NULL && err[0], "unclosed angle errors");
        sd_arena_reset(&arena);
        ok(sd_mini_parse(&arena, "bd*", 1, err, sizeof(err)) == NULL && err[0], "missing factor errors");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
