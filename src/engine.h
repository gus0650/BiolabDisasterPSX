// Biolab Disaster - Mega Drive port
// Small re-implementation of the parts of the high_impact engine used by the
// game, using fixed point math instead of floats.
//
// Units:
//   position     s32, 1/256 pixel ("subpixel")
//   velocity     s16, subpixels per frame (logic always runs at 60 Hz)
//   acceleration s16, subpixels per frame^2
//   friction     u16, fraction of velocity removed per frame, Q12
//   time         1/600 seconds, one frame = 10 units

#ifndef ENGINE_H
#define ENGINE_H

#include "types.h"
#include <string.h>
#include "gen/gfx.h"

#define TICK 10
#define T(SECONDS) ((s16)((SECONDS) * 600.0 + 0.5))
#define VEL(PX_PER_S) ((s16)((PX_PER_S) * 256.0 / 60.0 + ((PX_PER_S) >= 0 ? 0.5 : -0.5)))
#define ACC(PX_PER_S2) ((s16)((PX_PER_S2) * 256.0 / 3600.0 + 0.5))
#define FRICTION(F) ((u16)((F) * 4096.0 / 60.0 + 0.5))
#define FP(PX) ((s32)(PX) << 8)
#define PX(FPV) ((s16)((FPV) >> 8))
#define ANGLE_PI 128

#define SCREEN_W 320
#define SCREEN_H 240
#define MIN_BOUNCE VEL(10)

#define ENTITIES_MAX 250
#define PARTICLES_MAX 120

// --- Animation ---------------------------------------------------------------

typedef struct {
	const SprSheet *sheet;
	u16 frame_time;
	u16 total_time;
	u8 len;
	u8 loop;
	const u8 *seq;
} AnimDef;

#define ANIM(NAME, SHEET, FT, ...) \
	static const u8 NAME##_seq[] = {__VA_ARGS__}; \
	static const AnimDef NAME = {&SHEET, T(FT), (u16)(T(FT) * sizeof(NAME##_seq)), sizeof(NAME##_seq), 1, NAME##_seq}

// Non-looping animation. Mirrors high_impact's ANIM_STOP handling, which drops
// the last frame before the stop marker.
#define ANIM_ONCE(NAME, SHEET, FT, ...) \
	static const u8 NAME##_seq[] = {__VA_ARGS__}; \
	static const AnimDef NAME = {&SHEET, T(FT), (u16)(T(FT) * (sizeof(NAME##_seq) - 1)), sizeof(NAME##_seq) - 1, 0, NAME##_seq}

typedef struct {
	const AnimDef *def;
	s32 start_time;
	u8 flip_x;
	u8 flip_y;
	u8 tile_offset;
	u8 alpha; // 255 = opaque; low values flicker
} anim_t;

anim_t anim(const AnimDef *def);
void anim_goto_rand(anim_t *a);
u16 anim_looped(anim_t *a);

// --- Entities ----------------------------------------------------------------

enum {
	ENTITY_TYPE_NONE = 0,
	ENTITY_TYPE_BLOB,
	ENTITY_TYPE_CRATE,
	ENTITY_TYPE_DEBRIS,
	ENTITY_TYPE_DELAY,
	ENTITY_TYPE_DROPPER,
	ENTITY_TYPE_EARTHQUAKE,
	ENTITY_TYPE_END_HUB,
	ENTITY_TYPE_END_HUB_PLASMA,
	ENTITY_TYPE_END_HUB_FADE,
	ENTITY_TYPE_GLASS_DOME,
	ENTITY_TYPE_GRUNT,
	ENTITY_TYPE_HURT,
	ENTITY_TYPE_LEVEL_CHANGE,
	ENTITY_TYPE_MINE,
	ENTITY_TYPE_MOVER,
	ENTITY_TYPE_PARTICLE,
	ENTITY_TYPE_PLAYER,
	ENTITY_TYPE_PROJECTILE,
	ENTITY_TYPE_RESPAWN_POD,
	ENTITY_TYPE_SPIKE,
	ENTITY_TYPE_SPEWER,
	ENTITY_TYPE_SPEWER_SHOT,
	ENTITY_TYPE_TEST_TUBE,
	ENTITY_TYPE_TRIGGER,
	ENTITY_TYPE_VOID,
	ENTITY_TYPES_COUNT
};

enum {
	GROUP_NONE = 0,
	GROUP_PLAYER = 1 << 0,
	GROUP_NPC = 1 << 1,
	GROUP_ENEMY = 1 << 2,
	GROUP_ITEM = 1 << 3,
	GROUP_PROJECTILE = 1 << 4,
	GROUP_PICKUP = 1 << 5,
	GROUP_BREAKABLE = 1 << 6,
};

#define COLLIDES_WORLD   (1 << 1)
#define COLLIDES_LITE    (1 << 4)
#define COLLIDES_PASSIVE (1 << 5)
#define COLLIDES_ACTIVE  (1 << 6)
#define COLLIDES_FIXED   (1 << 7)

#define PHYSICS_NONE    0
#define PHYSICS_MOVE    1
#define PHYSICS_WORLD   (PHYSICS_MOVE | COLLIDES_WORLD)
#define PHYSICS_LITE    (PHYSICS_WORLD | COLLIDES_LITE)
#define PHYSICS_PASSIVE (PHYSICS_WORLD | COLLIDES_PASSIVE)
#define PHYSICS_ACTIVE  (PHYSICS_WORLD | COLLIDES_ACTIVE)
#define PHYSICS_FIXED   (PHYSICS_WORLD | COLLIDES_FIXED)

typedef struct {
	u16 id;
	u16 index;
} entity_ref_t;

#define MAX_TARGETS 6
typedef struct {
	u8 len;
	entity_ref_t e[MAX_TARGETS];
} entity_list_t;

typedef struct entity_t entity_t;

struct entity_t {
	u16 id;
	u8 slot;         // index in entity storage
	u8 is_alive;
	u8 on_ground;
	s16 draw_order;
	u8 type;
	u8 physics;
	u8 group;
	u8 check_against;
	s32 px, py;      // position (subpixels)
	s16 sx, sy;      // size (pixels)
	s16 vx, vy;      // velocity
	s16 ax, ay;      // acceleration
	u16 fx, fy;      // friction (Q12)
	s8 ox, oy;       // draw offset
	s16 health;
	u8 gravity;      // 0 or 1
	u8 restitution;  // Q8
	u16 mass;        // Q8
	anim_t anim;
	union {
		struct { u8 in_jump, seen_player; s16 jump_timer; } blob;
		struct { s16 count, duration, duration_time, emit_time; } debris;
		struct { entity_list_t targets; entity_ref_t triggered_by; u8 fire; s16 delay, delay_time; } delay;
		struct { s16 shoot_wait_time, shoot_time; u8 can_shoot; } dropper;
		struct { s16 duration; s16 strength; s16 time; } earthquake;
		struct { s16 stage; } end_hub;
		struct { s16 time; s16 index; } end_hub_plasma;
		struct { s16 time; } end_hub_fade;
		struct { s16 shoot_time; u8 flip, seen_player; } grunt;
		struct { s16 damage; } hurt;
		struct { s16 level; } level_change;
		struct { entity_list_t targets; s16 current_target; s16 speed; u8 dir_set; } mover;
		struct { s16 life_time, fade_time; } particle;
		struct { s16 high_jump_time, idle_time; u8 flip, can_jump, is_idle; } player;
		struct { const AnimDef *anim_hit; u8 has_hit, flip; } projectile;
		struct { u8 activated; } respawn_pod;
		struct { s16 shoot_wait_time, shoot_time; u8 can_shoot, flip; } spike;
		struct { s16 shoot_wait_time, shoot_time; u8 can_shoot; } spewer;
		struct { s16 bounce_count; } spewer_shot;
		struct { entity_list_t targets; s16 delay, delay_time; u8 can_fire; } trigger;
	};
};

typedef struct {
	void (*init)(entity_t *self);
	void (*settings)(entity_t *self, const LevelEntity *def);
	void (*update)(entity_t *self);
	void (*draw)(entity_t *self);
	void (*kill)(entity_t *self);
	void (*touch)(entity_t *self, entity_t *other);
	void (*collide)(entity_t *self, s16 nx, s16 ny, bool world);
	void (*damage)(entity_t *self, entity_t *other, s16 damage);
	void (*trigger)(entity_t *self, entity_t *other);
	void (*message)(entity_t *self, u16 message, void *data);
} entity_vtab_t;

extern entity_vtab_t entity_vtab[ENTITY_TYPES_COUNT];

#define EM_ACTIVATE 1

#define entity_update(E)          entity_vtab[(E)->type].update(E)
#define entity_kill(E)            ((E)->is_alive = false, entity_vtab[(E)->type].kill(E))
#define entity_touch(E, O)        entity_vtab[(E)->type].touch(E, O)
#define entity_collide(E, X, Y, W) entity_vtab[(E)->type].collide(E, X, Y, W)
#define entity_damage(E, O, D)    entity_vtab[(E)->type].damage(E, O, D)
#define entity_trigger(E, O)      entity_vtab[(E)->type].trigger(E, O)
#define entity_message(E, M, D)   entity_vtab[(E)->type].message(E, M, D)

void entities_init(void);
void entities_reset(void);
void entities_update(void);
void entities_draw(void);
entity_t *entity_spawn(u8 type, s32 px, s32 py);
entity_ref_t entity_ref(entity_t *e);
entity_t *entity_by_ref(entity_ref_t ref);
#define entity_ref_none() ((entity_ref_t){0, 0})
void entity_base_update(entity_t *self);
void entity_base_draw(entity_t *self);
void entity_base_draw_none(entity_t *self);
void entity_base_damage(entity_t *self, entity_t *other, s16 damage);
u16 entities_count_type(u8 type);
entity_t *entity_first_of_type(u8 type);
u16 particles_count(void);

// Center of an entity in subpixels
static inline s32 entity_cx(entity_t *e) { return e->px + ((s32)e->sx << 7); }
static inline s32 entity_cy(entity_t *e) { return e->py + ((s32)e->sy << 7); }

// --- Engine --------------------------------------------------------------------

typedef struct {
	s32 time;
	u32 frame;
	s32 vx, vy;         // viewport (subpixels)
	s16 gravity;        // per-frame velocity change
	const LevelDef *level;
	u8 *coll;
	u16 map_w, map_h;   // tiles
} engine_t;

extern engine_t engine;

typedef struct {
	void (*init)(void);
	void (*update)(void);
	void (*draw)(void);
	void (*cleanup)(void);
} scene_t;

void engine_set_scene(const scene_t *s);

// Map / collision
u8 map_tile_at_px(s16 x, s16 y);
void map_load(const LevelDef *def);
void map_draw(void);
void map_unload(void);

// Camera
typedef struct {
	entity_ref_t follow;
	s32 px, py;
	s16 offset_x, offset_y;
} camera_t;
void camera_follow(camera_t *cam, entity_ref_t follow, bool snap);
void camera_update(camera_t *cam);

// Rendering (GPU primitives are drawn in submission order)
void gfx_begin_frame(void);
void gfx_end_frame(void);
// sheet frame at screen position; alpha < 255 dims the sprite (fade)
void sprite_draw_frame(const SprSheet *sheet, u16 frame, s16 x, s16 y, u8 flip_x, u8 flip_y);
void sprite_draw_frame_alpha(const SprSheet *sheet, u16 frame, s16 x, s16 y, u8 flip_x, u8 flip_y, u8 alpha);
void gfx_draw_image(const TexRef *t, s16 x, s16 y, s16 w, s16 h);
void gfx_fill(s16 x, s16 y, s16 w, s16 h, u8 r, u8 g, u8 b);
// additive white overlay over the whole screen, amount 0..256
void gfx_flash_white(u16 amount);

// Input
#define BTN_LEFT  1
#define BTN_RIGHT 2
#define BTN_JUMP  4
#define BTN_SHOOT 8
#define BTN_START 16
u16 input_state(u16 b);
u16 input_pressed(u16 b);
void input_update(void);
void input_consume(void);
void engine_run(const scene_t *first);

// Text
#define ALIGN_LEFT 0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT 2
void text_draw(s16 x, s16 y, const char *str, u8 align);
s16 text_width(const char *str);

// Sound
void sound_init(void);
void sound_play_id(u16 id);
#define SOUND(NAME) sound_play_id(NAME)
void music_play(void);
void music_stop(void);
void music_set_volume(u16 vol); // 0..256
void music_update(void);

// Math helpers
static inline s32 muls16(s16 a, s16 b) { return (s32)a * b; }
static inline u32 mulu16(u16 a, u16 b) { return (u32)a * b; }
static inline u16 divu16(u32 a, u16 b) { u32 q = a / b; return q > 0xFFFF ? 0xFFFF : (u16)q; }
static inline u16 modu16(u32 a, u16 b) { return (u16)(a % b); }
s16 rand_int(s16 min, s16 max);
s16 rand_range(s16 min, s16 max); // alias
static inline s16 sin_q14(u8 a) { return sin_table[a]; }
static inline s16 cos_q14(u8 a) { return sin_table[(u8)(a + 64)]; }
u8 atan2_256(s16 y, s16 x);
u32 isqrt32(u32 v);
static inline s32 abs32(s32 v) { return v < 0 ? -v : v; }
// true if distance between two points (subpixels) is below limit (pixels)
bool dist_lt(s32 dx, s32 dy, s16 limit_px);

#endif
