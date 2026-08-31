// Splitting a part line into its head, its structure and its control clauses.
//
// A part line is:
//
//     <head> <structure> [field=pattern] [field+=pattern] [field*=pattern] ...
//
// The clause cut is the only subtle part. An "=" at bracket depth zero opens a
// clause; walking backwards over an optional + or * and then the field letters
// gives the point where the previous value ends. "=" never appears in mini
// notation, so this needs no lookahead and no symbol table.
//
// This is pure string work with no ESP-IDF dependency so it can be tested on a
// host, which is where the awkward cases actually get found.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SD_LINE_MAX_CLAUSES 12
#define SD_LINE_FIELD_LEN   12

typedef enum {
    SD_LINE_SET = 0,
    SD_LINE_ADD,
    SD_LINE_MUL,
} sd_line_op_t;

typedef struct {
    char         field[SD_LINE_FIELD_LEN];
    sd_line_op_t op;
    char const*  value;  // points into the source
    size_t       value_len;
} sd_line_clause_t;

typedef struct {
    char const* head;
    size_t      head_len;
    char const* structure;
    size_t      structure_len;

    sd_line_clause_t clause[SD_LINE_MAX_CLAUSES];
    int              nclauses;
} sd_line_t;

// Splits src. Returns false and fills err on a malformed clause.
bool sd_line_split(char const* src, size_t len, sd_line_t* out, char* err, size_t errlen);
