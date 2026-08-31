// Mini notation: the terse pattern syntax borrowed from TidalCycles.
//
//   bd sd            two events in a cycle
//   bd [sd sd]       brackets subdivide a step
//   <bd sd>          angle brackets pick one per cycle
//   bd*4             repeat faster within the step
//   bd/2             stretch over two cycles
//   bd(3,8)          euclidean rhythm, optional third argument rotates
//   bd!3             repeat as three separate steps
//   bd@3             take three times the width
//   bd ~             tilde is a rest, and _ extends the previous step
//   bd?  bd?0.3      randomly drop, optionally with a probability
//   bd:2             sample index
//   bd, hh*4         comma stacks layers
#pragma once

#include <stddef.h>

#include "sd_pattern.h"

// Parses src into a pattern allocated from a. Returns NULL and fills err on a
// syntax error. seed varies the random choices made by '?'.
sd_pat_t* sd_mini_parse(sd_arena_t* a, char const* src, uint32_t seed, char* err, size_t errlen);
