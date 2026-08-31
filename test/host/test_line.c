// The clause cut is the one genuinely tricky piece of the line syntax, so it
// gets pinned here: an = inside brackets is not a clause, a value runs up to
// the next field name, and a bare = is an error rather than a surprise.
#include <stdio.h>
#include <string.h>

#include "sd_line.h"

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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
