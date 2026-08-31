#include "sd_line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd_mini.h"

static bool is_ws(char c) {
    return c == ' ' || c == '\t';
}

static bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void trim(char const** s, size_t* len) {
    while (*len && is_ws(**s)) {
        (*s)++;
        (*len)--;
    }
    while (*len && (is_ws((*s)[*len - 1]) || (*s)[*len - 1] == '\r')) {
        (*len)--;
    }
}

bool sd_line_split(char const* src, size_t len, sd_line_t* out, char* err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    if (err && errlen) {
        err[0] = 0;
    }
    trim(&src, &len);
    if (len == 0) {
        return false;
    }

    // Head is the first whitespace delimited token
    size_t i = 0;
    while (i < len && !is_ws(src[i])) {
        i++;
    }
    out->head     = src;
    out->head_len = i;

    char const* body     = src + i;
    size_t      body_len = len - i;
    trim(&body, &body_len);
    if (body_len == 0) {
        out->structure     = body;
        out->structure_len = 0;
        return true;
    }

    // Find every clause cut, tracking bracket depth so an = inside a group
    // could never be mistaken for one (mini notation has no = at all).
    size_t cuts[SD_LINE_MAX_CLAUSES + 1];
    int    ncuts = 0;
    int    depth = 0;

    for (size_t k = 0; k < body_len; k++) {
        char c = body[k];
        if (c == '[' || c == '<' || c == '(') {
            depth++;
        } else if (c == ']' || c == '>' || c == ')') {
            if (depth > 0) {
                depth--;
            }
        } else if (c == '=' && depth == 0) {
            // Walk back over an optional op character then the field letters
            size_t j = k;
            if (j > 0 && (body[j - 1] == '+' || body[j - 1] == '*')) {
                j--;
            }
            size_t letters_end = j;
            while (j > 0 && is_letter(body[j - 1])) {
                j--;
            }
            if (j == letters_end) {
                if (err && errlen) {
                    snprintf(err, errlen, "stray = at column %u", (unsigned)k);
                }
                return false;
            }
            if (j != 0 && !is_ws(body[j - 1])) {
                if (err && errlen) {
                    snprintf(err, errlen, "stray = at column %u", (unsigned)k);
                }
                return false;
            }
            if (ncuts >= SD_LINE_MAX_CLAUSES) {
                if (err && errlen) {
                    snprintf(err, errlen, "too many controls");
                }
                return false;
            }
            cuts[ncuts++] = j;
        }
    }

    // Everything before the first cut is the structure
    size_t struct_len   = ncuts ? cuts[0] : body_len;
    out->structure      = body;
    out->structure_len  = struct_len;
    while (out->structure_len && is_ws(out->structure[out->structure_len - 1])) {
        out->structure_len--;
    }

    for (int c = 0; c < ncuts; c++) {
        size_t start = cuts[c];
        size_t end   = (c + 1 < ncuts) ? cuts[c + 1] : body_len;

        size_t j = start;
        size_t f = 0;
        while (j < end && is_letter(body[j])) {
            if (f + 1 < SD_LINE_FIELD_LEN) {
                out->clause[c].field[f++] = body[j];
            }
            j++;
        }
        out->clause[c].field[f] = 0;

        sd_line_op_t op = SD_LINE_SET;
        if (j < end && body[j] == '+') {
            op = SD_LINE_ADD;
            j++;
        } else if (j < end && body[j] == '*') {
            op = SD_LINE_MUL;
            j++;
        }
        if (j >= end || body[j] != '=') {
            if (err && errlen) {
                snprintf(err, errlen, "malformed control '%s'", out->clause[c].field);
            }
            return false;
        }
        j++;  // past the =

        char const* v    = body + j;
        size_t      vlen = end - j;
        trim(&v, &vlen);
        if (vlen == 0) {
            if (err && errlen) {
                snprintf(err, errlen, "control '%s' has no value", out->clause[c].field);
            }
            return false;
        }
        out->clause[c].op        = op;
        out->clause[c].value     = v;
        out->clause[c].value_len = vlen;
    }
    out->nclauses = ncuts;
    return true;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// A compact step grid, "x...x...", is a shorter way of writing a sequence, so
// it becomes the same pattern a written out sequence would.
static bool looks_like_grid(char const* s, size_t len) {
    if (len < 2) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (is_ws(c)) {
            return false;
        }
        bool grid = c == 'x' || c == 'X' || c == '.' || c == '~' || c == '-' || c == '_' || (c >= '0' && c <= '9');
        if (!grid) {
            return false;
        }
    }
    return true;
}

// A grid has at most a handful of distinct steps, and a pattern never mutates,
// so one node per distinct character is shared across every step that uses it.
static sd_pat_t* grid_pattern(sd_arena_t* a, char const* s, size_t len) {
    sd_pat_t* kids[64];
    sd_pat_t* cache[128] = {0};
    int       n          = 0;
    for (size_t i = 0; i < len && n < 64; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128) {
            continue;
        }
        if (!cache[c]) {
            if (c == '.' || c == '~' || c == '-' || c == '_') {
                cache[c] = sd_silence(a);
            } else if (c >= '1' && c <= '9') {
                cache[c] = sd_pure_num(a, (double)(c - '0'));
            } else {
                char w[2] = {(char)c, 0};
                cache[c]  = sd_pure_word(a, w, -1);
            }
        }
        kids[n++] = cache[c];
    }
    return n ? sd_fastcat(a, kids, n) : sd_silence(a);
}

// gain, pan and friends run 0..1, which is what makes a per step digit grid
// worth having: 9 is full, 0 is nothing, and a dot leaves the step alone.
static bool is_unit_field(sd_field_t f) {
    return f == SD_F_GAIN || f == SD_F_PAN || f == SD_F_RESONANCE || f == SD_F_SUSTAIN || f == SD_F_SHAPE ||
           f == SD_F_ROOM || f == SD_F_DELAY;
}

static sd_pat_t* ninths_pattern(sd_arena_t* a, char const* s, size_t len) {
    sd_pat_t* kids[64];
    sd_pat_t* cache[11] = {0};  // ten digits and one rest
    int       n         = 0;
    for (size_t i = 0; i < len && n < 64; i++) {
        char c   = s[i];
        int  idx = (c >= '0' && c <= '9') ? c - '0' : 10;
        if (!cache[idx]) {
            cache[idx] = idx == 10 ? sd_silence(a) : sd_pure_num(a, (double)idx / 9.0);
        }
        kids[n++] = cache[idx];
    }
    return n ? sd_fastcat(a, kids, n) : sd_silence(a);
}

static sd_pat_t* clause_pattern(sd_arena_t* a, sd_field_t f, char const* v, size_t len, uint32_t seed, char* err,
                                size_t errlen) {
    char   buf[128];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n);
    buf[n] = 0;

    bool all_grid = n >= 2, has_digit = false, all_digits = n >= 2;
    for (size_t i = 0; i < n; i++) {
        char c     = buf[i];
        bool digit = c >= '0' && c <= '9';
        bool rest  = c == '.' || c == '~' || c == '-' || c == '_';
        has_digit  = has_digit || digit;
        all_digits = all_digits && digit;
        all_grid   = all_grid && (digit || rest);
    }

    char*  end          = NULL;
    double dv           = strtod(buf, &end);
    bool   whole_number = end && *end == 0 && end != buf;

    if (is_unit_field(f) && all_grid && has_digit && (!whole_number || all_digits)) {
        return ninths_pattern(a, buf, n);
    }
    if (whole_number) {
        return sd_pure_num(a, dv);
    }
    return sd_mini_parse(a, buf, seed, err, errlen);
}

// Every failure path has to leave a message. An allocation that returns NULL
// because the arena filled is not a parse error and sets nothing by itself, so
// it would otherwise surface as a silent refusal to play.
static sd_pat_t* fail_out(sd_arena_t* a, char* err, size_t errlen) {
    if (err && errlen && err[0] == 0) {
        snprintf(err, errlen, a->exhausted ? "pattern too complex" : "could not build pattern");
    }
    return NULL;
}

sd_pat_t* sd_line_pattern(sd_arena_t* a, sd_line_t const* line, uint32_t seed, float const* defaults, char* err,
                          size_t errlen) {
    if (err && errlen) {
        err[0] = 0;
    }
    if (line->structure_len == 0) {
        snprintf(err, errlen, "no pattern");
        return NULL;
    }

    sd_pat_t* pat = NULL;
    if (looks_like_grid(line->structure, line->structure_len)) {
        pat = grid_pattern(a, line->structure, line->structure_len);
    } else {
        char   buf[192];
        size_t n = line->structure_len < sizeof(buf) - 1 ? line->structure_len : sizeof(buf) - 1;
        memcpy(buf, line->structure, n);
        buf[n] = 0;
        pat    = sd_mini_parse(a, buf, seed, err, errlen);
        if (!pat) {
            return fail_out(a, err, errlen);
        }
    }
    if (!pat) {
        return fail_out(a, err, errlen);
    }

    uint32_t seeded = 0;
    for (int i = 0; i < line->nclauses; i++) {
        sd_field_t f = sd_field_from_name(line->clause[i].field);
        if (f == SD_F_COUNT) {
            snprintf(err, errlen, "unknown control '%s'", line->clause[i].field);
            return NULL;
        }
        sd_pat_t* vp =
            clause_pattern(a, f, line->clause[i].value, line->clause[i].value_len, seed + (uint32_t)i * 2654435761u,
                           err, errlen);
        if (!vp) {
            return fail_out(a, err, errlen);
        }
        sd_op_t op = line->clause[i].op == SD_LINE_ADD   ? SD_OP_ADD
                     : line->clause[i].op == SD_LINE_MUL ? SD_OP_MUL
                                                         : SD_OP_SET;

        // Adding to or scaling a field needs something already there, so its
        // default is set once before the first such clause touches it.
        if (op != SD_OP_SET && !(seeded & (1u << f))) {
            sd_pat_t* seedp = sd_ctrl(a, f, sd_pure_num(a, defaults ? defaults[f] : 0.0f));
            if (!seedp) {
                return fail_out(a, err, errlen);
            }
            pat = sd_op(a, SD_OP_SET, pat, seedp);
        }
        seeded |= (1u << f);

        sd_pat_t* ctrl = sd_ctrl(a, f, vp);
        if (!ctrl || !pat) {
            return fail_out(a, err, errlen);
        }
        pat = sd_op(a, op, pat, ctrl);
    }

    if (!pat || a->exhausted) {
        return fail_out(a, err, errlen);
    }
    return pat;
}
