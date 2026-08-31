// The clause cut is the one genuinely tricky piece of the line syntax, so it
// gets pinned here: an = inside brackets is not a clause, a value runs up to
// the next field name, and a bare = is an error rather than a surprise.
#include <stdio.h>
#include <string.h>

#include "sd_line.h"
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

static sd_line_t L;
static char      err[96];

static bool split(char const* s) {
    return sd_line_split(s, strlen(s), &L, err, sizeof(err));
}

static bool is(char const* a, size_t len, char const* want) {
    return strlen(want) == len && strncmp(a, want, len) == 0;
}

int main(void) {
    // A line with no clauses is what every existing set looks like
    ok(split("bd x...x...x...x..."), "plain line splits");
    ok(is(L.head, L.head_len, "bd"), "head is bd");
    ok(is(L.structure, L.structure_len, "x...x...x...x..."), "structure is the grid");
    ok(L.nclauses == 0, "no clauses");

    ok(split("hh:0.26 x*8?0.2"), "head with colons still splits");
    ok(is(L.head, L.head_len, "hh:0.26"), "colon shorthand stays in the head");

    // One clause
    ok(split("bd x*4 g=0.5"), "one clause");
    ok(is(L.structure, L.structure_len, "x*4"), "structure stops before the clause");
    ok(L.nclauses == 1 && strcmp(L.clause[0].field, "g") == 0, "field is g");
    ok(L.clause[0].op == SD_LINE_SET, "plain = is set");
    ok(is(L.clause[0].value, L.clause[0].value_len, "0.5"), "value is 0.5");

    // Several clauses, values that are themselves patterns
    ok(split("hh x*8 g=[.2 .9 .4 .9] p=<0 1> c=1200"), "three clauses");
    ok(L.nclauses == 3, "counted three");
    ok(is(L.structure, L.structure_len, "x*8"), "structure intact");
    ok(is(L.clause[0].value, L.clause[0].value_len, "[.2 .9 .4 .9]"), "bracketed value kept whole");
    ok(is(L.clause[1].value, L.clause[1].value_len, "<0 1>"), "angle value kept whole");
    ok(is(L.clause[2].value, L.clause[2].value_len, "1200"), "last value runs to the end");

    // Operators
    ok(split("saw c2 g*=0.5 note+=12"), "operators parse");
    ok(L.clause[0].op == SD_LINE_MUL, "*= is multiply");
    ok(L.clause[1].op == SD_LINE_ADD, "+= is add");
    ok(strcmp(L.clause[1].field, "note") == 0, "long field names work");

    // A structure containing brackets and euclid, then a clause
    ok(split("bd [x x](3,8) g=0.7"), "structure with brackets and euclid");
    ok(is(L.structure, L.structure_len, "[x x](3,8)"), "structure kept whole");
    ok(L.nclauses == 1, "still one clause");

    // Values that look like words
    ok(split("saw c2 eb2 s=sine"), "word valued clause");
    ok(is(L.structure, L.structure_len, "c2 eb2"), "multi step structure");
    ok(is(L.clause[0].value, L.clause[0].value_len, "sine"), "word value");

    // The ninths grid, which must survive as a single token
    ok(split("hh x*8 g=29492949"), "ninths grid value");
    ok(is(L.clause[0].value, L.clause[0].value_len, "29492949"), "grid kept whole");

    // Errors
    ok(!split("bd x*4 =0.5"), "a bare = is an error");
    ok(err[0] != 0, "and it says why");
    ok(!split("bd x*4 g="), "an empty value is an error");
    ok(!split("bd x*4 g=0.5 p="), "an empty trailing value is an error");

    // An = that is not preceded by whitespace is not a clause opener
    ok(!split("bd x*4x=1"), "= glued to the structure is rejected");

    // ---- end to end: text in, events with control values out --------------
    {
        static uint8_t    arena_buf[64 * 1024];
        static sd_arena_t arena;
        static sd_hap_t   haps[128];

        float defaults[SD_F_COUNT] = {0};
        defaults[SD_F_GAIN]        = 0.8f;
        defaults[SD_F_CUTOFF]      = 2500.0f;
        defaults[SD_F_PAN]         = 0.5f;

        // Returns the value of a field at position num/den, or a sentinel
        float (*at_field)(char const*, sd_field_t, int, int, int);
        (void)at_field;

        struct {
            char const* line;
            sd_field_t  field;
            int         num, den, cyc;
            float       want;
        } const cases[] = {
            // a per step ninths grid of gains
            {"hh x*8 g=29492949", SD_F_GAIN, 0, 1, 0, 2.0f / 9.0f},
            {"hh x*8 g=29492949", SD_F_GAIN, 1, 8, 0, 9.0f / 9.0f},
            {"hh x*8 g=29492949", SD_F_GAIN, 2, 8, 0, 4.0f / 9.0f},
            // a control that alternates per cycle
            {"saw c2 eb2 c=<620 1400>", SD_F_CUTOFF, 0, 1, 0, 620.0f},
            {"saw c2 eb2 c=<620 1400>", SD_F_CUTOFF, 0, 1, 1, 1400.0f},
            // a plain number applies to every step
            {"bd x*4 p=0.25", SD_F_PAN, 3, 4, 0, 0.25f},
            // multiply combines with the default rather than with zero
            {"hh x*4 g*=0.5", SD_F_GAIN, 0, 1, 0, 0.4f},
            // add likewise
            {"saw c3 nt+=12", SD_F_NOTE, 0, 1, 0, 12.0f},
            // a bracketed value keeps its own structure
            {"hh x*4 g=[.2 .9]", SD_F_GAIN, 0, 1, 0, 0.2f},
            {"hh x*4 g=[.2 .9]", SD_F_GAIN, 1, 2, 0, 0.9f},
        };

        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            sd_arena_reset(&arena);
            sd_arena_init(&arena, arena_buf, sizeof(arena_buf));

            sd_line_t line;
            char      e[80] = "";
            if (!sd_line_split(cases[ci].line, strlen(cases[ci].line), &line, e, sizeof(e))) {
                printf("FAIL: split \"%s\": %s\n", cases[ci].line, e);
                failures++;
                checks++;
                continue;
            }
            sd_pat_t* pat = sd_line_pattern(&arena, &line, 1, defaults, e, sizeof(e));
            if (!pat) {
                printf("FAIL: build \"%s\": %s\n", cases[ci].line, e);
                failures++;
                checks++;
                continue;
            }
            sd_haps_t out = {.haps = haps, .cap = 128, .n = 0};
            sd_span_t sp  = {sd_int(cases[ci].cyc), sd_int(cases[ci].cyc + 1)};
            sd_query(pat, sp, &out);

            sd_frac_t want_at = sd_add(sd_int(cases[ci].cyc), sd_frac(cases[ci].num, cases[ci].den));
            bool      found   = false;
            float     got     = 0.0f;
            for (int i = 0; i < out.n; i++) {
                if (sd_hap_onset(&out.haps[i]) && sd_eq(out.haps[i].whole.b, want_at)) {
                    found = sd_has(&out.haps[i].v, cases[ci].field);
                    got   = out.haps[i].v.f[cases[ci].field];
                    break;
                }
            }
            checks++;
            float diff = got - cases[ci].want;
            if (!found || diff > 0.001f || diff < -0.001f) {
                printf("FAIL: \"%s\" field %s at %d/%d cyc %d: got %.4f want %.4f%s\n", cases[ci].line,
                       sd_field_name(cases[ci].field), cases[ci].num, cases[ci].den, cases[ci].cyc, got,
                       cases[ci].want, found ? "" : " (field not set)");
                failures++;
            }
        }

        // The sets that actually ship must fit with room to spare, and running
        // out must be reported rather than being a crash or a silent truncation.
        char const* const real_set[] = {
            "bd      x ~ ~ x ~ ~ x ~",
            "sd      ~ ~ x ~ ~ ~ x ~",
            "hh      x*8?0.2 g=29492949",
            "oh:0.20 ~ ~ ~ ~ ~ ~ x ~",
            "cp:0.34 x(3,8,3)",
            "saw     <c2 g1 c2 bb1> ~ [~ c2] ~   g=.26 c=<620 900 620 1400> q=.55",
            "saw     [c3 eb3 g3]*2 . <bb2 ab2>   g=.11 c=1500 q=.35 p=<.35 .65>",
            "square  ~ <c5 eb5> ~ g4             g=.06 c=2600 nt=<0 0 12 0>",
            "bd      x...x...x...x...",
            "hh      x*16?0.12 g=2939293929392949",
            "saw     c1 ~ ~ [~ c1] ~ ~ eb1 ~     g=.30 c=<340 520 340 800> q=.6",
            "tri     <c3 ab2 bb2 g2>             g=.08 c=900 nt+=<0 12>",
        };
        int const nreal = (int)(sizeof(real_set) / sizeof(real_set[0]));

        sd_arena_init(&arena, arena_buf, 48 * 1024);
        int built = 0;
        for (int i = 0; i < nreal; i++) {
            sd_line_t line;
            char      e[80] = "";
            if (!sd_line_split(real_set[i], strlen(real_set[i]), &line, e, sizeof(e))) {
                break;
            }
            if (!sd_line_pattern(&arena, &line, (uint32_t)i, defaults, e, sizeof(e))) {
                break;
            }
            built++;
        }
        ok(built == nreal, "a real set fits the badge's arena");
        ok(arena.used < 48u * 1024u / 2u, "and leaves at least half of it free");
        printf("  arena: %u of %u bytes for a %d part set\n", (unsigned)arena.used, 48u * 1024u, nreal);

        // Overflow has to be an honest failure, not a crash
        {
            sd_arena_init(&arena, arena_buf, 2048);
            sd_line_t line;
            char      e[80] = "";
            char const* busy = "saw [c3 eb3 g3]*2 . <bb2 ab2>  g=.11 c=<1200 2400> q=.35 p=<.35 .65>";
            sd_line_split(busy, strlen(busy), &line, e, sizeof(e));
            sd_pat_t* p = sd_line_pattern(&arena, &line, 1, defaults, e, sizeof(e));
            ok(p == NULL, "a pattern too big for the arena returns NULL");
            ok(e[0] != 0, "and says so");

            // querying a NULL pattern must be silent rather than fatal
            sd_haps_t out = {.haps = haps, .cap = 128, .n = 0};
            sd_span_t sp  = {sd_int(0), sd_int(1)};
            sd_query(NULL, sp, &out);
            ok(out.n == 0, "querying nothing yields nothing");
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
