// Random level generator for Biolab Disaster.
//
// Layout: the level is a winding corridor built from segments (flat runs,
// steps, small pits, climbing shafts with platforms, drops, and platform
// crossings over deadly pits). Every segment is constructed so that it can
// be passed with the player's jump (up to 3 tiles high, 4 tiles across).
//
// Looks: tiles are chosen by a model learned from the three original levels
// (see gen_tile_model() in tools/convert.py): for each cell the 3x3 pattern
// of solid/empty cells plus the tiles already placed to the left and above
// select a weighted list of tiles. Deep solid areas get noise-based rock.

#include "levelgen.h"
#include "engine.h"
#ifdef LG_DEBUG
#include <stdio.h>
#endif

#define H LEVELGEN_ROWS

static u8 g_main[LEVELGEN_MAX_W * LEVELGEN_ROWS];
static u8 g_coll[LEVELGEN_MAX_W * LEVELGEN_ROWS];
static LevelEntity g_ents[LEVELGEN_MAX_ENTS];
static u16 g_nents;
static LevelDef g_def;
static u16 W;

// --- RNG (xorshift32) ----------------------------------------------------------

static u32 rng;

static u32 rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static s16 rint_(s16 lo, s16 hi) {
	if (hi <= lo) {
		return lo;
	}
	return lo + (s16)(rnd() % (u32)(hi - lo + 1));
}

static bool chance(u16 percent) {
	return (rnd() % 100) < percent;
}

// --- Grid helpers --------------------------------------------------------------

static inline bool solid(s16 x, s16 y) {
	if (x < 0 || y < 0 || x >= (s16)W || y >= H) {
		return true;
	}
	return g_coll[y * W + x] != 0;
}

static void set_empty(s16 x, s16 y) {
	if (x > 0 && y > 0 && x < (s16)W - 1 && y < H - 1) {
		g_coll[y * W + x] = 0;
	}
}

static void set_solid(s16 x, s16 y) {
	if (x >= 0 && y >= 0 && x < (s16)W && y < H) {
		g_coll[y * W + x] = 1;
	}
}

// carve column x empty between rows top..bottom-1
static void carve(s16 x, s16 top, s16 bottom) {
	for (s16 y = top; y < bottom; y++) {
		set_empty(x, y);
	}
}

// --- Entities ------------------------------------------------------------------

static LevelEntity *add_ent(u8 type, s16 x, s16 y) {
	if (g_nents >= LEVELGEN_MAX_ENTS) {
		return NULL;
	}
	LevelEntity *e = &g_ents[g_nents++];
	memset(e, 0, sizeof(*e));
	e->type = type;
	e->x = x;
	e->y = y;
	return e;
}

static u16 ent_index(LevelEntity *e) {
	return (u16)(e - g_ents);
}

// trigger box that kills the player (hurt with a large damage)
static void add_kill_zone(s16 px, s16 py, s16 w, s16 h) {
	if (g_nents + 2 > LEVELGEN_MAX_ENTS) {
		return;
	}
	LevelEntity *hurt = add_ent(ENTITY_TYPE_HURT, px, py);
	hurt->a = 1000;
	LevelEntity *t = add_ent(ENTITY_TYPE_TRIGGER, px, py);
	t->sx = w;
	t->sy = h;
	t->has_size = 1;
	t->a = 0; // can fire again immediately (delay 0)
	t->checks = 1; // player
	t->ntargets = 1;
	t->targets[0] = ent_index(hurt);
}

// --- Enemy placement -------------------------------------------------------------

static u16 difficulty;
static s16 enemy_cols[LEVELGEN_MAX_ENTS];
static u16 n_enemy_cols;

static bool column_free(s16 x, s16 dist) {
	for (u16 i = 0; i < n_enemy_cols; i++) {
		s16 d = enemy_cols[i] - x;
		if (d < dist && d > -dist) {
			return false;
		}
	}
	return true;
}
static s16 tubes_placed;
static s16 max_tubes;

typedef struct {
	s16 x0, x1;   // flat floor columns [x0, x1)
	s16 floor;    // floor row
	s16 top;      // ceiling row (first empty row)
} flat_run_t;

#define MAX_RUNS 256
static flat_run_t runs[MAX_RUNS];
static u16 nruns;

static void note_run(s16 x0, s16 x1, s16 floor, s16 top) {
	if (nruns < MAX_RUNS && x1 - x0 >= 3) {
		runs[nruns++] = (flat_run_t){x0, x1, floor, top};
	}
}

static void place_enemy_on_run(flat_run_t *r) {
	s16 len = r->x1 - r->x0;
	s16 x = rint_(r->x0 + 1, r->x1 - 2);
	if (x < 24 || !column_free(x, 5)) {
		return;
	}
	s16 fy = r->floor * 8;
	s16 roll = rint_(0, 99);
	s16 hard = difficulty > 6 ? 6 : difficulty;
	if (roll < 22) {
		add_ent(ENTITY_TYPE_BLOB, x * 8, fy - 13);
	}
	else if (roll < 42 && len >= 6) {
		add_ent(ENTITY_TYPE_GRUNT, x * 8, fy - 13);
	}
	else if (roll < 56 && len >= 5) {
		add_ent(ENTITY_TYPE_SPIKE, x * 8, fy - 9);
	}
	else if (roll < 68) {
		add_ent(ENTITY_TYPE_MINE, x * 8 + 4, fy - 5);
	}
	else if (roll < 80 && r->floor - r->top >= 5) {
		// dropper hangs from the ceiling
		add_ent(ENTITY_TYPE_DROPPER, x * 8, r->top * 8);
	}
	else if (roll < 88 + hard) {
		add_ent(ENTITY_TYPE_SPEWER, x * 8, fy - 8);
	}
	else {
		// a few crates to play with
		s16 n = rint_(1, 3);
		for (s16 i = 0; i < n; i++) {
			add_ent(ENTITY_TYPE_CRATE, (x + i) * 8, fy - 8);
		}
	}
	enemy_cols[n_enemy_cols++] = x;
}

// --- Layout segments -----------------------------------------------------------

static s16 cx;        // current column
static s16 cfloor;    // current floor row
static s16 cheight;   // current corridor height

#define MIN_FLOOR 12
#define MAX_FLOOR (H - 3)

static s16 vary_height(void) {
	cheight += rint_(-1, 1);
	if (cheight < 6) cheight = 6;
	if (cheight > 11) cheight = 11;
	if (cfloor - cheight < 2) cheight = cfloor - 2;
	return cheight;
}

static void seg_flat(s16 len) {
	s16 x0 = cx;
	s16 min_top = cfloor;
	for (s16 i = 0; i < len && cx < (s16)W - 2; i++, cx++) {
		if ((i % 3) == 0) {
			vary_height();
		}
		carve(cx, cfloor - cheight, cfloor);
		if (cfloor - cheight < min_top) min_top = cfloor - cheight;
	}
	note_run(x0, cx, cfloor, min_top);
	// a floating platform with a test tube above long, tall stretches
	if (len >= 8 && cheight >= 8 && tubes_placed < max_tubes && chance(35)) {
		s16 px = x0 + rint_(2, len - 5);
		s16 py = cfloor - 3;
		for (s16 i = 0; i < 3; i++) {
			set_solid(px + i, py);
		}
		add_ent(ENTITY_TYPE_TEST_TUBE, (px + 1) * 8, py * 8 - 10);
		tubes_placed++;
	}
}

static void seg_steps(void) {
	s16 n = rint_(2, 4);
	for (s16 i = 0; i < n; i++) {
		s16 d = rint_(1, 3) * (chance(50) ? 1 : -1);
		s16 nf = cfloor + d;
		if (nf < MIN_FLOOR || nf > MAX_FLOOR) {
			nf = cfloor - d;
		}
		if (nf < MIN_FLOOR || nf > MAX_FLOOR) {
			nf = cfloor;
		}
		cfloor = nf;
		seg_flat(rint_(3, 6));
	}
}

static void seg_small_pit(void) {
	seg_flat(2);
	s16 w = rint_(2, 4);
	s16 depth = rint_(2, 3);
	if (cfloor + depth > MAX_FLOOR) {
		depth = MAX_FLOOR - cfloor;
	}
	for (s16 i = 0; i < w; i++, cx++) {
		carve(cx, cfloor - cheight, cfloor + depth);
	}
	if (depth >= 2 && tubes_placed < max_tubes && chance(30)) {
		// a tube at the bottom of the pit
		add_ent(ENTITY_TYPE_TEST_TUBE, (cx - 1) * 8, (cfloor + depth) * 8 - 10);
		tubes_placed++;
	}
	seg_flat(2);
}

// climb up a shaft with alternating platforms (3 rows apart)
static void seg_climb(void) {
	s16 rise = rint_(6, 15);
	s16 nf = cfloor - rise;
	if (nf - 7 < 2) {
		nf = 9;
		rise = cfloor - nf;
	}
	if (rise < 6) {
		seg_steps();
		return;
	}
	s16 cw = rint_(8, 11);
	s16 x0 = cx;
	s16 top = nf - rint_(6, 9);
	if (top < 2) top = 2;
	for (s16 i = 0; i < cw; i++) {
		carve(x0 + i, top, cfloor);
	}
	// platform rows from the floor upward; the last one must be on the right
	s16 rows[8], nrows = 0;
	for (s16 r = cfloor - 3; r > nf && nrows < 8; r -= 3) {
		rows[nrows++] = r;
	}
	for (s16 k = 0; k < nrows; k++) {
		// count from the top: top-most platform is on the right side
		bool right = ((nrows - 1 - k) % 2) == 0;
		s16 r = rows[k];
		s16 px = right ? x0 + cw - 4 : x0;
		for (s16 i = 0; i < 4; i++) {
			set_solid(px + i, r);
		}
		if (k == nrows / 2 && tubes_placed < max_tubes && chance(40)) {
			add_ent(ENTITY_TYPE_TEST_TUBE, (px + 1) * 8, r * 8 - 10);
			tubes_placed++;
		}
	}
	cx = x0 + cw;
	cfloor = nf;
	cheight = 7;
	seg_flat(rint_(3, 5));
}

// drop down into a lower area
static void seg_drop(void) {
	s16 d = rint_(5, 14);
	s16 nf = cfloor + d;
	if (nf > MAX_FLOOR) {
		nf = MAX_FLOOR;
	}
	if (nf - cfloor < 4) {
		seg_steps();
		return;
	}
	s16 cw = rint_(4, 7);
	s16 top = cfloor - cheight;
	for (s16 i = 0; i < cw; i++, cx++) {
		carve(cx, top, nf);
	}
	cfloor = nf;
	seg_flat(rint_(4, 8));
}

// crossing over a deadly pit on small platforms
static void seg_gap(void) {
	if (cfloor > H - 9) {
		seg_climb();
		return;
	}
	seg_flat(2);
	s16 len = rint_(12, 22);
	s16 x0 = cx;
	s16 bottom = H - 2;
	s16 x = x0;
	s16 plat_row = cfloor;
	s16 top = cfloor - cheight;
	for (s16 i = 0; i < len; i++) {
		carve(x0 + i, top, bottom);
	}
	// platforms: gaps of 2..4 columns, width 2..4, height change of -2..+1
	// pos: first column after the last solid footing; jumps cover at most
	// 3 empty columns so the next footing is always within reach
	s16 pos = x0;
	(void)x;
	while (x0 + len - pos > 3) {
		s16 remaining = x0 + len - pos;
		s16 gap = rint_(2, remaining - 2 < 3 ? remaining - 2 : 3);
		s16 start = pos + gap;
		s16 pw = rint_(2, 4);
		if (start + pw > x0 + len - 2) pw = x0 + len - 2 - start;
		if (pw < 1) pw = 1;
		s16 nr = plat_row + rint_(-2, 1);
		if (nr > cfloor + 2) nr = cfloor + 2;
		if (nr < cfloor - 3) nr = cfloor - 3;
		if (nr - top < 4) nr = top + 4;
		plat_row = nr;
		for (s16 i = 0; i < pw; i++) {
			set_solid(start + i, plat_row);
		}
		pos = start + pw;
	}
	// make sure the landing on the far side is reachable from the last platform
	cx = x0 + len;
	if (cfloor > plat_row + 3) cfloor = plat_row + 3;
	if (cfloor < plat_row - 3) cfloor = plat_row - 3;
	add_kill_zone(x0 * 8, (bottom - 2) * 8, len * 8, 16);
	seg_flat(3);
}

// --- Tile synthesis --------------------------------------------------------------

static u16 mask9(s16 x, s16 y) {
	u16 m = 0;
	for (s16 dy = -1; dy <= 1; dy++) {
		for (s16 dx = -1; dx <= 1; dx++) {
			m = (m << 1) | (solid(x + dx, y + dy) ? 1 : 0);
		}
	}
	return m;
}

static const TileCtx *find_ctx(u32 key) {
	s32 lo = 0, hi = TILE_CTX_COUNT - 1;
	while (lo <= hi) {
		s32 mid = (lo + hi) >> 1;
		u32 k = tile_ctx[mid].key;
		if (k == key) {
			return &tile_ctx[mid];
		}
		if (k < key) {
			lo = mid + 1;
		}
		else {
			hi = mid - 1;
		}
	}
	return NULL;
}

static u8 pick(const TileCtx *c) {
	u32 total = 0;
	for (u16 i = 0; i < c->count; i++) {
		total += tile_choice[c->first + i].weight;
	}
	u32 r = rnd() % total;
	for (u16 i = 0; i < c->count; i++) {
		u16 w = tile_choice[c->first + i].weight;
		if (r < w) {
			return tile_choice[c->first + i].tile;
		}
		r -= w;
	}
	return tile_choice[c->first].tile;
}

// smooth value noise, 0..255
static u32 hash2(s32 x, s32 y, u32 seed) {
	u32 h = (u32)x * 0x27d4eb2dU ^ (u32)y * 0x165667b1U ^ seed * 0x9e3779b9U;
	h ^= h >> 15;
	h *= 0x85ebca6bU;
	h ^= h >> 13;
	return h & 0xFF;
}

static u16 noise(s16 x, s16 y, u32 seed) {
	s32 fx = x * 256 / 3, fy = y * 256 / 3;
	s32 ix = fx >> 8, iy = fy >> 8;
	s32 u = fx & 255, v = fy & 255;
	s32 a = hash2(ix, iy, seed) * (256 - u) + hash2(ix + 1, iy, seed) * u;
	s32 b = hash2(ix, iy + 1, seed) * (256 - u) + hash2(ix + 1, iy + 1, seed) * u;
	return (u16)((a * (256 - v) + b * v) >> 16);
}

static bool deep_solid(s16 x, s16 y) {
	for (s16 dy = -2; dy <= 2; dy++) {
		for (s16 dx = -2; dx <= 2; dx++) {
			if (!solid(x + dx, y + dy)) {
				return false;
			}
		}
	}
	return true;
}

static void synthesize_tiles(u32 seed) {
	static const u8 rock_dark[] = {77, 77, 77, 77, 78, 78, 79};
	static const u8 rock_orange[] = {57, 58, 59, 60, 61, 62, 63, 63};
	for (s16 y = 0; y < H; y++) {
		for (s16 x = 0; x < (s16)W; x++) {
			u8 *out = &g_main[y * W + x];
			bool s = solid(x, y);
			u16 k = mask9(x, y);
			u8 left = x > 0 ? g_main[y * W + x - 1] : 255;
			u8 up = y > 0 ? g_main[(y - 1) * W + x] : 255;
			if (s && deep_solid(x, y)) {
				if (noise(x, y, seed) > 160) {
					*out = rock_orange[rnd() % sizeof(rock_orange)];
				}
				else {
					*out = rock_dark[rnd() % sizeof(rock_dark)];
				}
				continue;
			}
			// decorations in open air only continue from above (pipes etc.)
			if (!s && k == 0 && (up == 0 || up == 255)) {
				*out = 0;
				continue;
			}
			const TileCtx *c = find_ctx(((u32)k << 16) | ((u32)left << 8) | up);
			if (!c) c = find_ctx(((u32)k << 16) | ((u32)left << 8) | 254);
			if (!c) c = find_ctx(((u32)k << 16) | (253 << 8) | up);
			if (!c) c = find_ctx(((u32)k << 16) | (252 << 8) | 252);
			if (c) {
				*out = pick(c);
			}
			else {
				*out = s ? 77 : 0;
			}
			// never leave a solid cell invisible or an empty cell looking solid
			if (s && *out == 0) {
				*out = 77;
			}
		}
	}
}

// --- Main -----------------------------------------------------------------------

const LevelDef *levelgen_generate(u32 seed, u16 diff) {
	rng = seed * 2654435761U + 0x1234567U;
	if (rng == 0) rng = 1;
	for (int i = 0; i < 8; i++) rnd();
	difficulty = diff;
	// levels grow by 32 tiles (one screen) per completed level, up to 1024
	{
		u32 w = 160 + (u32)diff * 32;
		W = w > LEVELGEN_MAX_W ? LEVELGEN_MAX_W : (u16)w;
	}
	max_tubes = 6 + (W - 160) / 128;
	memset(g_coll, 1, sizeof(g_coll));
	memset(g_main, 0, sizeof(g_main));
	g_nents = 0;
	nruns = 0;
	tubes_placed = 0;
	n_enemy_cols = 0;

	// start area
	cx = 2;
	cfloor = rint_(20, 30);
	cheight = 8;
	add_ent(ENTITY_TYPE_PLAYER, 4 * 8, cfloor * 8 - 14);
	seg_flat(14);

	s16 next_pod = 50;
	s16 seg_count = 0;
	while (cx < (s16)W - 30) {
		if (cx >= next_pod) {
			// checkpoint on a short flat run
			seg_flat(3);
			add_ent(ENTITY_TYPE_RESPAWN_POD, (cx - 3) * 8, cfloor * 8 - 16);
			enemy_cols[n_enemy_cols++] = cx - 3;
			seg_flat(4);
			next_pod += 45 + rint_(0, 15);
			continue;
		}
		s16 r = rint_(0, 99);
		if (r < 25) seg_flat(rint_(6, 14));
		else if (r < 45) seg_steps();
		else if (r < 57) seg_small_pit();
		else if (r < 72) seg_climb();
		else if (r < 84) seg_drop();
		else seg_gap();
		seg_count++;
	}
	// final stretch and exit
	seg_flat((s16)W - 2 - cx);
	s16 exit_x = (s16)W - 6;
	LevelEntity *lc = add_ent(ENTITY_TYPE_LEVEL_CHANGE, exit_x * 8, (cfloor - 4) * 8);
	if (lc) {
		lc->a = LEVEL_NEXT_RANDOM;
		u16 lci = ent_index(lc);
		LevelEntity *t = add_ent(ENTITY_TYPE_TRIGGER, exit_x * 8, (cfloor - 6) * 8);
		if (t) {
			t->sx = 32;
			t->sy = 48;
			t->has_size = 1;
			t->a = -1;
			t->checks = 1;
			t->ntargets = 1;
			t->targets[0] = lci;
		}
	}

	// enemies on flat runs: more with every level
	// enemy density grows from 6 to 10 per 100 tiles over the first 20 levels
	u16 density = 6 + (diff > 20 ? 20 : diff) / 5;
	u16 want = (u16)((u32)W * density / 100);
	if (want > 110) want = 110;
	u16 attempts = want * 8;
	for (u16 i = 0; i < attempts; i++) {
		if (nruns == 0) break;
		u16 before = g_nents;
		place_enemy_on_run(&runs[rnd() % nruns]);
		if (g_nents != before && --want == 0) {
			break;
		}
	}
	// a few more test tubes on random flat runs
	for (u16 i = 0; i < 20 && tubes_placed < max_tubes && nruns; i++) {
		flat_run_t *r = &runs[rnd() % nruns];
		s16 x = rint_(r->x0, r->x1 - 1);
		if (x > 8) {
			add_ent(ENTITY_TYPE_TEST_TUBE, x * 8, r->floor * 8 - 10);
			tubes_placed++;
		}
	}

	synthesize_tiles(seed);

	const LevelDef *bgsrc = levels[1];
	g_def.w = W;
	g_def.h = H;
	g_def.bw = bgsrc->bw;
	g_def.bh = bgsrc->bh;
	g_def.main = g_main;
	g_def.bg = bgsrc->bg;
	g_def.coll = g_coll;
	g_def.nents = g_nents;
	g_def.ents = g_ents;
	// the maps are W wide: repack rows if W < LEVELGEN_MAX_W is not needed,
	// rows are already stored with stride W.
	return &g_def;
}
