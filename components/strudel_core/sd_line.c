#include "sd_line.h"

#include <stdio.h>
#include <string.h>

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
