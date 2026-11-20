#ifndef LEVELGEN_H
#define LEVELGEN_H

#include "types.h"
#include "gfx.h"

#define LEVELGEN_MAX_W 1024
#define LEVELGEN_ROWS 40
#define LEVELGEN_MAX_ENTS 200

// level_change entity value meaning "continue with the next random level"
#define LEVEL_NEXT_RANDOM 255

// Generates a level from a seed. difficulty: 0 = first random level, grows
// with every completed random level. The returned LevelDef points to static
// buffers that stay valid until the next call.
const LevelDef *levelgen_generate(u32 seed, u16 difficulty);

#endif
