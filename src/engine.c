#include "engine.h"
#include <psxgpu.h>
#include <psxpad.h>
#include <psxapi.h>
#include <psxspu.h>
#include <psxcd.h>
#include <psxetc.h>
#include <hwregs_c.h>

u16 random(void);

engine_t engine;

// ---------------------------------------------------------------------------
// Math

s16 rand_int(s16 min, s16 max) {
	if (max <= min) {
		return min;
	}
	u16 r = random();
	return min + (s16)(r % (u16)(max - min + 1));
}

s16 rand_range(s16 min, s16 max) {
	return rand_int(min, max);
}

u32 isqrt32(u32 v) {
	u32 res = 0;
	u32 bit = 1UL << 30;
	while (bit > v) {
		bit >>= 2;
	}
	while (bit) {
		if (v >= res + bit) {
			v -= res + bit;
			res = (res >> 1) + bit;
		}
		else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

// Approximate atan2 with 256 steps per circle (good to ~1.5 degrees)
u8 atan2_256(s16 y, s16 x) {
	if (x == 0 && y == 0) {
		return 0;
	}
	s32 ax = x < 0 ? -x : x;
	s32 ay = y < 0 ? -y : y;
	u8 a;
	// first octant angle 0..32 via t = min/max, atan(t) ~ t*(pi/4) + 0.273*t*(1-t)
	s32 mn = ax < ay ? ax : ay;
	s32 mx = ax < ay ? ay : ax;
	s32 t = (mn << 12) / mx; // Q12
	s32 ang = (t * 32 + ((t * (4096 - t)) >> 12) * 11 / 1) >> 12; // ~ in 1/256 circle
	if (ang > 32) {
		ang = 32;
	}
	if (ay > ax) {
		ang = 64 - ang;
	}
	a = (u8)ang;
	if (x < 0) {
		a = 128 - a;
	}
	if (y < 0) {
		a = (u8)(256 - a);
	}
	return a;
}

bool dist_lt(s32 dx, s32 dy, s16 limit_px) {
	dx = abs32(dx) >> 4;
	dy = abs32(dy) >> 4;
	s32 lim = (s32)limit_px << 4;
	if (dx >= lim || dy >= lim) {
		return false;
	}
	return dx * dx + dy * dy < lim * lim;
}

// ---------------------------------------------------------------------------
// Animation

anim_t anim(const AnimDef *def) {
	anim_t a = {def, engine.time, 0, 0, 0, 255};
	return a;
}

void anim_goto_rand(anim_t *a) {
	// note: high_impact adds the offset (delaying the animation instead of
	// jumping ahead); we use the intended random start frame.
	a->start_time = engine.time - (s32)rand_int(0, a->def->len - 1) * a->def->frame_time;
}

u16 anim_looped(anim_t *a) {
	s32 diff = engine.time - a->start_time;
	if (diff <= 0) {
		return 0;
	}
	return divu16((u32)diff, a->def->total_time);
}

static u16 anim_frame(anim_t *a) {
	const AnimDef *def = a->def;
	s32 diff = engine.time - a->start_time;
	if (diff < 0) {
		diff = 0;
	}
	u16 f;
	if (!def->loop && (u32)diff >= def->total_time) {
		f = def->len - 1;
	}
	else {
		f = divu16(modu16((u32)diff, def->total_time), def->frame_time);
		if (f >= def->len) {
			f = def->len - 1;
		}
	}
	return def->seq[f] + a->tile_offset;
}

static u32 rng_state = 0x12345678;
u16 random(void) {
	rng_state = rng_state * 1103515245u + 12345u;
	return (u16)(rng_state >> 16);
}

// ---------------------------------------------------------------------------
// GPU: double-buffered display, primitives chained in submission order

#define PRIM_BUF_SIZE (96 * 1024)

typedef struct {
	DISPENV disp;
	DRAWENV draw;
	u32 head[2];            // dummy chain head (one empty primitive)
	u8 prims[PRIM_BUF_SIZE];
} framebuf_t;

static framebuf_t fb[2];
static int fb_cur;
static u8 *prim_ptr;
static u32 *prim_tail;
static bool prim_overflow;

static void *prim_alloc(u32 size, u32 words) {
	framebuf_t *f = &fb[fb_cur];
	if (prim_ptr + size > f->prims + PRIM_BUF_SIZE) {
		prim_overflow = true;
		return NULL;
	}
	u32 *p = (u32 *)prim_ptr;
	prim_ptr += size;
	// link the previous primitive to this one
	*prim_tail = (*prim_tail & 0xFF000000) | ((u32)p & 0x00FFFFFF);
	*p = (words << 24) | 0x00FFFFFF;
	prim_tail = p;
	return p;
}

static void gfx_init(void) {
	ResetGraph(0);
	SetDefDispEnv(&fb[0].disp, 0, 0, SCREEN_W, SCREEN_H);
	SetDefDrawEnv(&fb[0].draw, 0, 240, SCREEN_W, SCREEN_H);
	SetDefDispEnv(&fb[1].disp, 0, 240, SCREEN_W, SCREEN_H);
	SetDefDrawEnv(&fb[1].draw, 0, 0, SCREEN_W, SCREEN_H);
	for (int i = 0; i < 2; i++) {
		setRGB0(&fb[i].draw, 0, 0, 0);
		fb[i].draw.isbg = 1;
		fb[i].draw.dtd = 0;
	}
	// upload all textures and CLUTs
	for (int i = 0; i < VRAM_UPLOAD_COUNT; i++) {
		const VramUpload *u = &vram_uploads[i];
		RECT r = {u->x, u->y, u->w, u->h};
		LoadImage(&r, (const u32 *)u->data);
		DrawSync(0);
	}
	fb_cur = 0;
	PutDispEnv(&fb[1].disp);
	PutDrawEnv(&fb[0].draw);
	SetDispMask(1);
}

void gfx_begin_frame(void) {
	framebuf_t *f = &fb[fb_cur];
	f->head[0] = 0x00FFFFFF;
	prim_tail = &f->head[0];
	prim_ptr = f->prims;
	prim_overflow = false;
}

void gfx_end_frame(void) {
	framebuf_t *f = &fb[fb_cur];
	DrawSync(0);
	VSync(0);
	PutDispEnv(&f->disp);
	PutDrawEnv(&f->draw);
	DrawOTag(&f->head[0]);
	fb_cur ^= 1;
}

static inline u16 tpage_for(const TexRef *t, u8 abr) {
	return getTPage(1, abr, t->px, t->py);
}

static inline u16 clut_for(const TexRef *t) {
	return getClut(t->cx, t->cy);
}

// textured quad (POLY_FT4: carries its own tpage and clut)
static void draw_tex(const TexRef *t, s16 u, s16 v, s16 w, s16 h, s16 x, s16 y, u8 abr, bool semi, u8 bright) {
	if (x >= SCREEN_W || y >= SCREEN_H || x + w <= 0 || y + h <= 0) {
		return;
	}
	POLY_FT4 *p = prim_alloc(sizeof(POLY_FT4), 9);
	if (!p) {
		return;
	}
	setPolyFT4(p);
	setSemiTrans(p, semi);
	setRGB0(p, bright, bright, bright);
	if (bright == 128) {
		setShadeTex(p, 1);
	}
	setXYWH(p, x, y, w, h);
	u8 u0 = t->u + u, v0 = t->v + v;
	setUVWH(p, u0, v0, w, h);
	p->tpage = tpage_for(t, abr);
	p->clut = clut_for(t);
	*(u32 *)p = (*(u32 *)p & 0x00FFFFFF) | (9 << 24);
}

void sprite_draw_frame_alpha(const SprSheet *sheet, u16 frame, s16 x, s16 y, u8 flip_x, u8 flip_y, u8 alpha) {
	if (frame >= sheet->nframes) {
		return;
	}
	u8 cx = frame % sheet->cols;
	u8 cy = frame / sheet->cols;
	u8 var = (flip_x ? 1 : 0) | (flip_y ? 2 : 0);
	if (flip_x) {
		cx = sheet->cols - 1 - cx;
	}
	if (flip_y) {
		cy = sheet->rows - 1 - cy;
	}
	const TexRef *t = &sheet->var[var];
	// blend: 0 = opaque, 1 = 50% for semi pixels (STP), 2 = additive
	u8 abr = sheet->blend == 2 ? 1 : 0;
	bool semi = sheet->blend != 0;
	u8 bright = alpha >= 255 ? 128 : (u8)((alpha + 1) >> 1);
	draw_tex(t, cx * sheet->w, cy * sheet->h, sheet->w, sheet->h, x, y, abr, semi, bright);
}

void sprite_draw_frame(const SprSheet *sheet, u16 frame, s16 x, s16 y, u8 flip_x, u8 flip_y) {
	sprite_draw_frame_alpha(sheet, frame, x, y, flip_x, flip_y, 255);
}

void gfx_draw_image(const TexRef *t, s16 x, s16 y, s16 w, s16 h) {
	draw_tex(t, 0, 0, w, h, x, y, 0, false, 128);
}

void gfx_fill(s16 x, s16 y, s16 w, s16 h, u8 r, u8 g, u8 b) {
	TILE *p = prim_alloc(sizeof(TILE), 3);
	if (!p) {
		return;
	}
	setTile(p);
	setXY0(p, x, y);
	setWH(p, w, h);
	setRGB0(p, r, g, b);
	*(u32 *)p = (*(u32 *)p & 0x00FFFFFF) | (3 << 24);
}

void gfx_flash_white(u16 amount) {
	if (amount == 0) {
		return;
	}
	if (amount >= 256) {
		gfx_fill(0, 0, SCREEN_W, SCREEN_H, 255, 255, 255);
		return;
	}
	// additive (B+F) white tile
	DR_TPAGE *tp = prim_alloc(sizeof(DR_TPAGE), 1);
	if (!tp) {
		return;
	}
	setDrawTPage(tp, 1, 0, getTPage(0, 1, 0, 0));
	*(u32 *)tp = (*(u32 *)tp & 0x00FFFFFF) | (1 << 24);
	TILE *p = prim_alloc(sizeof(TILE), 3);
	if (!p) {
		return;
	}
	setTile(p);
	setSemiTrans(p, 1);
	setXY0(p, 0, 0);
	setWH(p, SCREEN_W, SCREEN_H);
	u8 c = (u8)(amount - (amount >> 8));
	setRGB0(p, c, c, c);
	*(u32 *)p = (*(u32 *)p & 0x00FFFFFF) | (3 << 24);
}

// ---------------------------------------------------------------------------
// Text (font texture, drawn per glyph)

s16 text_width(const char *str) {
	s16 w = 0;
	for (const char *c = str; *c && *c != '\n'; c++) {
		if (*c >= FONT_FIRST_CHAR && *c < FONT_LAST_CHAR) {
			w += font_glyphs[*c - FONT_FIRST_CHAR].advance;
		}
	}
	return w;
}

void text_draw(s16 x, s16 y, const char *str, u8 align) {
	const char *c = str;
	while (*c) {
		while (*c == '\n') {
			y += FONT_LINE_HEIGHT;
			c++;
		}
		if (!*c) {
			break;
		}
		s16 lx = x;
		if (align != ALIGN_LEFT) {
			s16 w = text_width(c);
			lx -= align == ALIGN_CENTER ? w / 2 : w;
		}
		for (; *c && *c != '\n'; c++) {
			if (*c < FONT_FIRST_CHAR || *c >= FONT_LAST_CHAR) {
				continue;
			}
			const FontGlyph *g = &font_glyphs[*c - FONT_FIRST_CHAR];
			if (g->w && g->h && y + g->oy < SCREEN_H && y + g->oy + g->h > 0) {
				draw_tex(&tex_font_ref, g->x, g->y, g->w, g->h, lx + g->ox, y + g->oy, 0, false, 128);
			}
			lx += g->advance;
		}
	}
}

// ---------------------------------------------------------------------------
// Input

static u8 pad_buf[2][34];
static u16 in_state, in_prev;

static void input_init(void) {
	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	StartPAD();
	ChangeClearPAD(0);
}

void input_update(void) {
	PADTYPE *pad = (PADTYPE *)pad_buf[0];
	u16 s = 0;
	if (pad->stat == 0) {
		u16 b = ~pad->btn;
		if (b & PAD_LEFT) s |= BTN_LEFT;
		if (b & PAD_RIGHT) s |= BTN_RIGHT;
		if (b & (PAD_CROSS | PAD_UP)) s |= BTN_JUMP;
		if (b & (PAD_SQUARE | PAD_CIRCLE)) s |= BTN_SHOOT;
		if (b & PAD_START) s |= BTN_START;
		if (b & PAD_UP) s |= BTN_UP;
		if (b & PAD_DOWN) s |= BTN_DOWN;
	}
	in_prev = in_state;
	in_state = s;
}

void input_consume(void) {
	in_prev = in_state;
}

u16 input_state(u16 b) {
	return in_state & b;
}

u16 input_pressed(u16 b) {
	return in_state & ~in_prev & b;
}

// ---------------------------------------------------------------------------
// Sound effects (SPU ADPCM) and music (CD-DA track 2)

#define SPU_ALLOC_START 0x1010
#define SFX_VOICES 22
static u32 sound_addr[SND_COUNT];
static u16 next_voice;

void sound_init(void) {
	SpuInit();
	u32 addr = SPU_ALLOC_START;
	for (int i = 0; i < SND_COUNT; i++) {
		sound_addr[i] = addr;
		SpuSetTransferMode(SPU_TRANSFER_BY_IO);
		SpuSetTransferStartAddr(addr);
		SpuWrite((const uint32_t *)sounds[i].data, (sounds[i].size + 63) & ~63);
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
		addr += (sounds[i].size + 63) & ~63;
	}
	SPU_MASTER_VOL_L = 0x3fff;
	SPU_MASTER_VOL_R = 0x3fff;
	SPU_CD_VOL_L = 0x6000;
	SPU_CD_VOL_R = 0x6000;
}

void sound_play_id(u16 id) {
	int ch = next_voice;
	next_voice = (next_voice + 1) % SFX_VOICES;
	SpuSetKey(0, 1 << ch);
	SPU_CH_FREQ(ch) = getSPUSampleRate(sounds[id].rate);
	SPU_CH_ADDR(ch) = getSPUAddr(sound_addr[id]);
	// the original uses a global volume of 0.75
	SPU_CH_VOL_L(ch) = 0x2fff;
	SPU_CH_VOL_R(ch) = 0x2fff;
	SPU_CH_ADSR1(ch) = 0x00ff;
	SPU_CH_ADSR2(ch) = 0x0000;
	SpuSetKey(1, 1 << ch);
}

#define MUSIC_TRACK 2
static volatile bool music_ended;
static bool music_playing;
static bool cd_ok;

static void music_autopause_cb(CdlIntrResult r, uint8_t *res) {
	music_ended = true;
}

static void music_start_track(void) {
	u8 mode = CdlModeDA | CdlModeAP;
	CdControlB(CdlSetmode, &mode, 0);
	u8 track = MUSIC_TRACK;
	CdControlF(CdlPlay, &track);
}

void music_play(void) {
	if (!cd_ok) {
		return;
	}
	music_ended = false;
	music_playing = true;
	music_start_track();
	music_set_volume(256);
}

void music_stop(void) {
	if (!cd_ok) {
		return;
	}
	music_playing = false;
	CdControlF(CdlPause, 0);
}

void music_set_volume(u16 vol) {
	if (vol > 256) vol = 256;
	u16 v = (u16)((0x6000 * (u32)vol) >> 8);
	SPU_CD_VOL_L = v;
	SPU_CD_VOL_R = v;
}

void music_update(void) {
	// loop: the drive pauses at the end of the track (CdlModeAP)
	if (music_playing && music_ended) {
		music_ended = false;
		music_start_track();
	}
}

static void cd_init(void) {
	cd_ok = CdInit() ? true : false;
	if (cd_ok) {
		CdAutoPauseCallback(music_autopause_cb);
	}
}


// ---------------------------------------------------------------------------
// Map: collision and drawing (8x8 tiles as SPRT_8 primitives)

static s16 anim_tile[TILESET_COUNT];

typedef struct { u16 tile; u16 frame_time; u8 seq[8]; } tile_anim_t;
static const tile_anim_t tile_anims[4] = {
	{80, T(0.13), {80, 81, 82, 83, 84, 85, 86, 87}},
	{81, T(0.17), {84, 83, 82, 81, 80, 87, 86, 85}},
	{88, T(0.23), {88, 89, 90, 91, 92, 93, 94, 95}},
	{89, T(0.19), {95, 94, 93, 92, 91, 90, 89, 88}},
};

u8 map_tile_at_px(s16 x, s16 y) {
	if (x < 0 || y < 0) {
		// high_impact truncates toward zero: -7..-1 map to tile 0
		if (x <= -8 || y <= -8) {
			return 0;
		}
		if (x < 0) x = 0;
		if (y < 0) y = 0;
	}
	u16 tx = x >> 3, ty = y >> 3;
	if (tx >= engine.map_w || ty >= engine.map_h) {
		return 0;
	}
	return engine.coll[ty * engine.map_w + tx];
}

static inline u8 coll_at(s16 tx, s16 ty) {
	if ((u16)tx >= engine.map_w || (u16)ty >= engine.map_h) {
		return 0;
	}
	return engine.coll[(u16)ty * engine.map_w + (u16)tx];
}

void map_load(const LevelDef *def) {
	engine.level = def;
	engine.coll = (u8 *)def->coll;
	engine.map_w = def->w;
	engine.map_h = def->h;
}

void map_unload(void) {
	engine.level = NULL;
	engine.coll = NULL;
	engine.map_w = engine.map_h = 0;
}

static void draw_layer(const u8 *data, u16 w, u16 h, bool repeat, s16 ox, s16 oy, s16 clip_y0, s16 clip_y1) {
	DR_TPAGE *tp = prim_alloc(sizeof(DR_TPAGE), 1);
	if (!tp) {
		return;
	}
	setDrawTPage(tp, 0, 0, tpage_for(&tex_tileset_ref, 0));
	*(u32 *)tp = (*(u32 *)tp & 0x00FFFFFF) | (1 << 24);
	u16 clut = clut_for(&tex_tileset_ref);
	s16 tx0 = ox >> 3, ty0 = oy >> 3;
	s16 px0 = -(ox & 7), py0 = -(oy & 7);
	for (s16 ty = 0; py0 + ty * 8 < SCREEN_H; ty++) {
		s16 sy = py0 + ty * 8;
		if (sy + 8 <= clip_y0 || sy >= clip_y1) {
			continue;
		}
		s16 my = ty0 + ty;
		if (repeat) {
			my %= (s16)h;
			if (my < 0) my += h;
		}
		else if (my < 0 || my >= (s16)h) {
			continue;
		}
		const u8 *row = data + my * w;
		for (s16 tx = 0; px0 + tx * 8 < SCREEN_W; tx++) {
			s16 mx = tx0 + tx;
			if (repeat) {
				mx %= (s16)w;
				if (mx < 0) mx += w;
			}
			else if (mx < 0 || mx >= (s16)w) {
				continue;
			}
			u8 t = row[mx];
			if (!t) {
				continue;
			}
			s16 tile = anim_tile[t - 1];
			SPRT_8 *p = prim_alloc(sizeof(SPRT_8), 3);
			if (!p) {
				return;
			}
			setSprt8(p);
			setXY0(p, px0 + tx * 8, sy);
			setUV0(p, tex_tileset_ref.u + (tile % TILESET_COLS) * 8, tex_tileset_ref.v + (tile / TILESET_COLS) * 8);
			setRGB0(p, 128, 128, 128);
			p->clut = clut;
			*(u32 *)p = (*(u32 *)p & 0x00FFFFFF) | (3 << 24);
		}
	}
}

void map_draw(void) {
	if (!engine.level) {
		return;
	}
	for (s16 i = 0; i < TILESET_COUNT; i++) {
		anim_tile[i] = i;
	}
	for (u16 i = 0; i < 4; i++) {
		const tile_anim_t *a = &tile_anims[i];
		anim_tile[a->tile] = a->seq[((u32)engine.time / a->frame_time) & 7];
	}
	const LevelDef *l = engine.level;
	s16 vx = (engine.vx + 128) >> 8;
	s16 vy = (engine.vy + 128) >> 8;
	// maps shorter than the screen are centered with black bars
	s16 y0 = 0, y1 = SCREEN_H;
	if (l->h * 8 < SCREEN_H) {
		y0 = -vy;
		y1 = -vy + l->h * 8;
	}
	draw_layer(l->bg, l->bw, l->bh, true, vx >> 1, vy >> 1, y0, y1);
	draw_layer(l->main, l->w, l->h, false, vx, vy, y0, y1);
}

// ---------------------------------------------------------------------------
// Camera

static void camera_target(camera_t *cam, s32 *tx, s32 *ty) {
	s32 x = cam->px - FP(SCREEN_W / 2) + FP(cam->offset_x);
	s32 y = cam->py - FP(SCREEN_H / 2) + FP(cam->offset_y);
	if (engine.level) {
		s32 bw = FP(engine.map_w * 8), bh = FP(engine.map_h * 8);
		if (x < 0) x = 0;
		if (x > bw - FP(SCREEN_W)) x = bw - FP(SCREEN_W);
		if (bh < FP(SCREEN_H)) {
			y = (bh - FP(SCREEN_H)) / 2;
		}
		else {
			if (y < 0) y = 0;
			if (y > bh - FP(SCREEN_H)) y = bh - FP(SCREEN_H);
		}
	}
	*tx = x;
	*ty = y;
}

void camera_update(camera_t *cam) {
	entity_t *f = entity_by_ref(cam->follow);
	if (f) {
		// deadzone and look-ahead are zero in Biolab: the camera tracks the
		// followed entity's position directly
		cam->px = f->px;
		cam->py = f->py;
	}
	s32 tx, ty;
	camera_target(cam, &tx, &ty);
	s32 dx = tx - engine.vx;
	s32 dy = ty - engine.vy;
	// vel = diff * speed(3); moves only if |vel.x|+|vel.y| > min_vel(5)
	if ((abs32(dx) + abs32(dy)) * 3 > FP(5)) {
		// diff * 3 / 60 == diff / 20 ~= diff * 13 / 256
		engine.vx += (dx * 13) >> 8;
		engine.vy += (dy * 13) >> 8;
	}
}

void camera_follow(camera_t *cam, entity_ref_t follow, bool snap) {
	cam->follow = follow;
	if (snap) {
		camera_update(cam);
		s32 tx, ty;
		camera_target(cam, &tx, &ty);
		engine.vx = tx;
		engine.vy = ty;
	}
}

// ---------------------------------------------------------------------------
// Entities

entity_vtab_t entity_vtab[ENTITY_TYPES_COUNT];

static u16 entities_len;
static u16 entity_unique_id;
static entity_t *entities[ENTITIES_MAX];
static entity_t entities_storage[ENTITIES_MAX];
static u16 particle_count;

static void noop_init(entity_t *self) {}
static void noop_settings(entity_t *self, const LevelEntity *def) {}
static void noop_kill(entity_t *self) {}
static void noop_touch(entity_t *self, entity_t *other) {}
static void noop_collide(entity_t *self, s16 nx, s16 ny, bool world) {}
static void noop_trigger(entity_t *self, entity_t *other) {}
static void noop_message(entity_t *self, u16 m, void *d) {}

void entities_init(void) {
	for (u16 i = 0; i < ENTITY_TYPES_COUNT; i++) {
		entity_vtab_t *v = &entity_vtab[i];
		if (!v->init) v->init = noop_init;
		if (!v->settings) v->settings = noop_settings;
		if (!v->update) v->update = entity_base_update;
		if (!v->draw) v->draw = entity_base_draw;
		if (!v->kill) v->kill = noop_kill;
		if (!v->touch) v->touch = noop_touch;
		if (!v->collide) v->collide = noop_collide;
		if (!v->damage) v->damage = entity_base_damage;
		if (!v->trigger) v->trigger = noop_trigger;
		if (!v->message) v->message = noop_message;
	}
	entities_reset();
}

void entities_reset(void) {
	for (u16 i = 0; i < ENTITIES_MAX; i++) {
		entities[i] = &entities_storage[i];
		entities_storage[i].is_alive = false;
		entities_storage[i].slot = i;
	}
	entities_len = 0;
	particle_count = 0;
}

u16 particles_count(void) {
	return particle_count;
}

entity_t *entity_spawn(u8 type, s32 px, s32 py) {
	if (entities_len >= ENTITIES_MAX) {
		return NULL;
	}
	entity_t *e = entities[entities_len++];
	entity_unique_id++;
	if (entity_unique_id == 0) {
		entity_unique_id = 1;
	}
	u16 slot = e->slot;
	memset(e, 0, sizeof(entity_t));
	e->slot = slot;
	e->type = type;
	e->id = entity_unique_id;
	e->is_alive = true;
	e->px = px;
	e->py = py;
	e->gravity = 1;
	e->mass = 256;
	e->sx = 8;
	e->sy = 8;
	if (type == ENTITY_TYPE_PARTICLE) {
		particle_count++;
	}
	entity_vtab[type].init(e);
	return e;
}

entity_ref_t entity_ref(entity_t *e) {
	if (!e) {
		return entity_ref_none();
	}
	return (entity_ref_t){e->id, e->slot};
}

entity_t *entity_by_ref(entity_ref_t ref) {
	if (ref.index >= ENTITIES_MAX) {
		return NULL;
	}
	entity_t *e = &entities_storage[ref.index];
	if (e->is_alive && e->id == ref.id) {
		return e;
	}
	return NULL;
}

u16 entities_count_type(u8 type) {
	u16 n = 0;
	for (u16 i = 0; i < entities_len; i++) {
		if (entities[i]->type == type && entities[i]->is_alive) {
			n++;
		}
	}
	return n;
}

entity_t *entity_first_of_type(u8 type) {
	for (u16 i = 0; i < entities_len; i++) {
		if (entities[i]->type == type && entities[i]->is_alive) {
			return entities[i];
		}
	}
	return NULL;
}

void entity_base_damage(entity_t *self, entity_t *other, s16 damage) {
	self->health -= damage;
	if (self->health <= 0 && self->is_alive) {
		entity_kill(self);
	}
}

void entity_base_draw_none(entity_t *self) {}

void entity_base_draw(entity_t *self) {
	if (!self->anim.def) {
		return;
	}
	s16 x = (s16)((self->px - engine.vx + 128) >> 8) - self->ox;
	s16 y = (s16)((self->py - engine.vy + 128) >> 8) - self->oy;
	const SprSheet *sh = self->anim.def->sheet;
	if (x > SCREEN_W || y > SCREEN_H || x + (s16)sh->w < 0 || y + (s16)sh->h < 0) {
		return;
	}
	sprite_draw_frame_alpha(sh, anim_frame(&self->anim), x, y, self->anim.flip_x, self->anim.flip_y, self->anim.alpha);
}

// --- World collision ---------------------------------------------------------

static void handle_trace(entity_t *self, s16 nx, s16 ny) {
	entity_collide(self, nx, ny, true);
	if (self->restitution) {
		s16 against = nx ? self->vx * nx : self->vy * ny;
		s16 aa = against < 0 ? -against : against;
		if ((mulu16(aa, self->restitution) >> 8) > MIN_BOUNCE) {
			s16 vx = self->vx, vy = self->vy;
			if (nx) {
				vx -= 2 * against * nx;
			}
			else {
				vy -= 2 * against * ny;
			}
			self->vx = (s16)(muls16(vx, self->restitution) >> 8);
			self->vy = (s16)(muls16(vy, self->restitution) >> 8);
			return;
		}
	}
	if (engine.gravity && ny < 0) {
		self->on_ground = true;
	}
	if (nx) {
		self->vx = 0;
	}
	else {
		self->vy = 0;
	}
}

static void move_x(entity_t *self, s32 dx) {
	s32 nx = self->px + dx;
	s32 w = FP(self->sx);
	s16 c0, c1;
	if (dx > 0) {
		c0 = (s16)((self->px + w + 2047) >> 11);
		c1 = (s16)((nx + w - 1) >> 11);
		if (c0 > c1) {
			self->px = nx;
			return;
		}
	}
	else {
		c0 = (s16)((self->px >> 11) - 1);
		c1 = (s16)(nx >> 11);
		if (c0 < c1) {
			self->px = nx;
			return;
		}
	}
	s16 ty0 = (s16)(self->py >> 11);
	s16 ty1 = (s16)((self->py + FP(self->sy) - 1) >> 11);
	if (dx > 0) {
		for (s16 c = c0; c <= c1; c++) {
			for (s16 ty = ty0; ty <= ty1; ty++) {
				if (coll_at(c, ty) == 1) {
					self->px = ((s32)c << 11) - w;
					handle_trace(self, -1, 0);
					return;
				}
			}
		}
	}
	else {
		for (s16 c = c0; c >= c1; c--) {
			for (s16 ty = ty0; ty <= ty1; ty++) {
				if (coll_at(c, ty) == 1) {
					self->px = (s32)(c + 1) << 11;
					handle_trace(self, 1, 0);
					return;
				}
			}
		}
	}
	self->px = nx;
}

static void move_y(entity_t *self, s32 dy) {
	s32 ny = self->py + dy;
	s32 h = FP(self->sy);
	s16 r0, r1;
	if (dy > 0) {
		r0 = (s16)((self->py + h + 2047) >> 11);
		r1 = (s16)((ny + h - 1) >> 11);
		if (r0 > r1) {
			self->py = ny;
			return;
		}
	}
	else {
		r0 = (s16)((self->py >> 11) - 1);
		r1 = (s16)(ny >> 11);
		if (r0 < r1) {
			self->py = ny;
			return;
		}
	}
	s16 tx0 = (s16)(self->px >> 11);
	s16 tx1 = (s16)((self->px + FP(self->sx) - 1) >> 11);
	if (dy > 0) {
		for (s16 r = r0; r <= r1; r++) {
			for (s16 tx = tx0; tx <= tx1; tx++) {
				if (coll_at(tx, r) == 1) {
					self->py = ((s32)r << 11) - h;
					handle_trace(self, 0, -1);
					return;
				}
			}
		}
	}
	else {
		for (s16 r = r0; r >= r1; r--) {
			for (s16 tx = tx0; tx <= tx1; tx++) {
				if (coll_at(tx, r) == 1) {
					self->py = (s32)(r + 1) << 11;
					handle_trace(self, 0, 1);
					return;
				}
			}
		}
	}
	self->py = ny;
}

static void entity_move(entity_t *self, s32 dx, s32 dy) {
	if ((self->physics & COLLIDES_WORLD) && engine.coll) {
		if (dx) {
			move_x(self, dx);
		}
		if (dy && self->is_alive) {
			move_y(self, dy);
		}
	}
	else {
		self->px += dx;
		self->py += dy;
	}
}

static inline s16 fric(s16 v, u16 f) {
	if (!f) {
		return 0;
	}
	if (f >= 4096) {
		return v;
	}
	if (v >= 0) {
		return (s16)(mulu16((u16)v, f) >> 12);
	}
	return -(s16)(mulu16((u16)(-v), f) >> 12);
}

void entity_base_update(entity_t *self) {
	if (!(self->physics & PHYSICS_MOVE)) {
		return;
	}
	s16 ovx = self->vx, ovy = self->vy;
	if (self->gravity) {
		self->vy += engine.gravity;
	}
	self->vx += self->ax - fric(self->vx, self->fx);
	self->vy += self->ay - fric(self->vy, self->fy);
	s32 dx = ((s32)ovx + self->vx) >> 1;
	s32 dy = ((s32)ovy + self->vy) >> 1;
	self->on_ground = false;
	entity_move(self, dx, dy);
}

// --- Entity vs entity ----------------------------------------------------------

static void separate_x(entity_t *left, entity_t *right, u16 lm, u16 rm, s32 overlap) {
	s16 impact = left->vx - right->vx;
	if (lm > 0) {
		left->vx = (s16)((muls16(right->vx, lm) + muls16(left->vx, rm)) >> 8);
		s16 bounce = (s16)(muls16(impact, left->restitution) >> 8);
		if (bounce > MIN_BOUNCE) {
			left->vx -= bounce;
		}
		entity_move(left, -(muls16((s16)overlap, lm) >> 8), 0);
	}
	if (rm > 0) {
		right->vx = (s16)((muls16(left->vx, rm) + muls16(right->vx, lm)) >> 8);
		s16 bounce = (s16)(muls16(impact, right->restitution) >> 8);
		if (bounce > MIN_BOUNCE) {
			right->vx += bounce;
		}
		entity_move(right, muls16((s16)overlap, rm) >> 8, 0);
	}
}

static void separate_y(entity_t *top, entity_t *bottom, u16 tm, u16 bm, s32 overlap) {
	if (bottom->on_ground && tm > 0) {
		tm = 256;
		bm = 0;
	}
	s16 impact = top->vy - bottom->vy;
	s16 top_vy = top->vy;
	if (tm > 0) {
		top->vy = (s16)((muls16(top->vy, bm) + muls16(bottom->vy, tm)) >> 8);
		s32 move_x = 0;
		s16 bounce = (s16)(muls16(impact, top->restitution) >> 8);
		if (bounce > MIN_BOUNCE) {
			top->vy -= bounce;
		}
		else {
			top->on_ground = true;
			move_x = bottom->vx;
		}
		entity_move(top, move_x, -(muls16((s16)overlap, tm) >> 8));
	}
	if (bm > 0) {
		bottom->vy = (s16)((muls16(bottom->vy, tm) + muls16(top_vy, bm)) >> 8);
		s16 bounce = (s16)(muls16(impact, bottom->restitution) >> 8);
		if (bounce > MIN_BOUNCE) {
			bottom->vy += bounce;
		}
		entity_move(bottom, 0, muls16((s16)overlap, bm) >> 8);
	}
}

static void resolve_collision(entity_t *a, entity_t *b) {
	s32 ox = a->px < b->px ? a->px + FP(a->sx) - b->px : b->px + FP(b->sx) - a->px;
	s32 oy = a->py < b->py ? a->py + FP(a->sy) - b->py : b->py + FP(b->sy) - a->py;
	u16 am, bm;
	if ((a->physics & COLLIDES_LITE) || (b->physics & COLLIDES_FIXED)) {
		am = 256;
		bm = 0;
	}
	else if ((a->physics & COLLIDES_FIXED) || (b->physics & COLLIDES_LITE)) {
		am = 0;
		bm = 256;
	}
	else {
		u16 total = a->mass + b->mass;
		am = (u16)(((u32)b->mass << 8) / total);
		bm = 256 - am;
	}
	if (oy > ox) {
		if (a->px < b->px) {
			separate_x(a, b, am, bm, ox);
			entity_collide(a, -1, 0, false);
			entity_collide(b, 1, 0, false);
		}
		else {
			separate_x(b, a, bm, am, ox);
			entity_collide(a, 1, 0, false);
			entity_collide(b, -1, 0, false);
		}
	}
	else {
		if (a->py < b->py) {
			separate_y(a, b, am, bm, oy);
			entity_collide(a, 0, -1, false);
			entity_collide(b, 0, 1, false);
		}
		else {
			separate_y(b, a, bm, am, oy);
			entity_collide(a, 0, 1, false);
			entity_collide(b, 0, -1, false);
		}
	}
}

static inline bool is_touching(entity_t *a, entity_t *b) {
	return !(
		a->px >= b->px + FP(b->sx) ||
		a->px + FP(a->sx) <= b->px ||
		a->py >= b->py + FP(b->sy) ||
		a->py + FP(a->sy) <= b->py
	);
}

// Entities far away from the screen are frozen to save CPU time
// (the original updates everything; enemies only act near the player anyway).
static inline bool is_far(entity_t *e) {
	return false;
	switch (e->type) {
		case ENTITY_TYPE_BLOB: case ENTITY_TYPE_GRUNT: case ENTITY_TYPE_SPIKE:
		case ENTITY_TYPE_DROPPER: case ENTITY_TYPE_SPEWER: case ENTITY_TYPE_CRATE:
		case ENTITY_TYPE_PARTICLE: case ENTITY_TYPE_PROJECTILE: case ENTITY_TYPE_SPEWER_SHOT:
		case ENTITY_TYPE_MINE: case ENTITY_TYPE_TEST_TUBE: case ENTITY_TYPE_MOVER: {
			s16 dx = (s16)((e->px - engine.vx) >> 8);
			s16 dy = (s16)((e->py - engine.vy) >> 8);
			return dx < -160 || dx > SCREEN_W + 160 || dy < -160 || dy > SCREEN_H + 160;
		}
		default:
			return false;
	}
}

void entities_update(void) {
	for (u16 i = 0; i < entities_len; i++) {
		entity_t *e = entities[i];
		if (is_far(e)) {
			if (e->type == ENTITY_TYPE_PARTICLE || e->type == ENTITY_TYPE_PROJECTILE || e->type == ENTITY_TYPE_SPEWER_SHOT) {
				e->is_alive = false; // silently discard
			}
		}
		else {
			entity_update(e);
		}
		if (!e->is_alive) {
			if (e->type == ENTITY_TYPE_PARTICLE && particle_count) {
				particle_count--;
			}
			entities_len--;
			if (i < entities_len) {
				entity_t *last = entities[entities_len];
				entities[entities_len] = entities[i];
				entities[i] = last;
				i--;
			}
		}
	}

	// insertion sort by x (nearly sorted from the previous frame)
	for (u16 i = 1; i < entities_len; i++) {
		entity_t *e = entities[i];
		s16 j = i - 1;
		while (j >= 0 && entities[j]->px > e->px) {
			entities[j + 1] = entities[j];
			j--;
		}
		entities[j + 1] = e;
	}

	// Only entities overlapping an area around the screen take part in
	// collision checks (far away entities are frozen anyway).
	s32 cmin = -FP(30000), cmax = FP(30000);
	for (u16 i = 0; i < entities_len; i++) {
		entity_t *e1 = entities[i];
		if (e1->px > cmax) {
			break;
		}
		if (e1->px + FP(e1->sx) < cmin) {
			continue;
		}
		if (e1->check_against || e1->group || e1->physics > COLLIDES_LITE) {
			s32 max_pos = e1->px + FP(e1->sx);
			for (u16 j = i + 1; j < entities_len && entities[j]->px < max_pos; j++) {
				entity_t *e2 = entities[j];
				if (is_touching(e1, e2)) {
					if (e1->check_against & e2->group) {
						entity_touch(e1, e2);
					}
					if (e1->group & e2->check_against) {
						entity_touch(e2, e1);
					}
					if (
						e1->physics >= COLLIDES_LITE &&
						e2->physics >= COLLIDES_LITE &&
						(u16)e1->physics + e2->physics >= (COLLIDES_ACTIVE | COLLIDES_LITE) &&
						e1->mass + e2->mass > 0
					) {
						resolve_collision(e1, e2);
					}
				}
			}
		}
	}
}

void entities_draw(void) {
	// painter's order: draw_order ascending (stable), later primitives on top
	static entity_t *sorted[ENTITIES_MAX];
	u16 n = 0;
	s32 vx0 = engine.vx - FP(64), vy0 = engine.vy - FP(64);
	s32 vx1 = engine.vx + FP(SCREEN_W + 16), vy1 = engine.vy + FP(SCREEN_H + 16);
	for (u16 i = 0; i < entities_len; i++) {
		entity_t *e = entities[i];
		if (!e->is_alive || entity_vtab[e->type].draw == entity_base_draw_none) {
			continue;
		}
		bool always = e->type == ENTITY_TYPE_END_HUB_PLASMA || e->type == ENTITY_TYPE_END_HUB_FADE;
		if (!always && (e->px < vx0 || e->px > vx1 || e->py < vy0 || e->py > vy1)) {
			continue;
		}
		s16 j = n - 1;
		while (j >= 0 && sorted[j]->draw_order > e->draw_order) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = e;
		n++;
	}
	for (u16 i = 0; i < n; i++) {
		entity_vtab[sorted[i]->type].draw(sorted[i]);
	}
}

// ---------------------------------------------------------------------------
// Scenes and main loop

static const scene_t *scene_cur;
static const scene_t *scene_next;

void engine_set_scene(const scene_t *s) {
	scene_next = s;
}

static void scene_switch(void) {
	if (scene_cur && scene_cur->cleanup) {
		scene_cur->cleanup();
	}
	entities_reset();
	map_unload();
	engine.time = 0;
	engine.frame = 0;
	engine.vx = engine.vy = 0;
	engine.gravity = 0;
	scene_cur = scene_next;
	scene_next = NULL;
	if (scene_cur->init) {
		scene_cur->init();
	}
}

static void engine_step(void) {
	if (scene_next) {
		scene_switch();
	}
	engine.time += TICK;
	engine.frame++;
	if (scene_cur->update) {
		scene_cur->update();
	}
}

void engine_run(const scene_t *first) {
	gfx_init();
	input_init();
	sound_init();
	cd_init();
	scene_next = first;
	// PAL consoles: run 6 logic steps per 5 frames to keep 60 Hz game speed
	bool pal = GetVideoMode() == MODE_PAL;
	u16 pal_acc = 0;
	while (1) {
		input_update();
		engine_step();
		if (pal && ++pal_acc >= 5) {
			pal_acc = 0;
			input_consume();
			engine_step();
		}
		music_update();
		gfx_begin_frame();
		if (scene_cur->draw) {
			scene_cur->draw();
		}
		gfx_end_frame();
	}
}
