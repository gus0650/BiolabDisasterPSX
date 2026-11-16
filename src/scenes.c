#include "game.h"
#include <stdio.h>

global_t g;

// The original renders at 240x160; we center that area on the 320x224 screen
// for the menus.
#define OX 40
#define OY 40

// ---------------------------------------------------------------------------
// Game

static entity_ref_t last_checkpoint;
static s32 initial_spawn_x, initial_spawn_y;
static camera_t camera;
static entity_t *level_spawned[96];

void game_set_level(s16 level) {
	g.level = level;
}

void game_set_checkpoint(entity_ref_t checkpoint) {
	last_checkpoint = checkpoint;
}

void game_fade_white(u16 amount) {
	g.fade_white = amount;
}

entity_list_t game_targets(const LevelEntity *def) {
	entity_list_t list;
	list.len = 0;
	for (u16 i = 0; i < def->ntargets; i++) {
		entity_t *e = level_spawned[def->targets[i]];
		if (e && e->is_alive) {
			list.e[list.len++] = entity_ref(e);
		}
	}
	return list;
}

entity_t *game_spawn_particle(s32 px, s32 py, s16 vel, s16 vel_var, u8 angle, u8 angle_var, const AnimDef *def) {
	if (particles_count() >= PARTICLES_MAX) {
		return NULL;
	}
	entity_t *p = entity_spawn(ENTITY_TYPE_PARTICLE, px, py);
	if (!p) {
		return NULL;
	}
	p->anim = anim(def);
	anim_goto_rand(&p->anim);
	p->anim.flip_x = rand_int(0, 1);
	p->anim.flip_y = rand_int(0, 1);
	u8 a = angle + (s8)rand_int(-angle_var, angle_var);
	s32 v = (s32)rand_int(vel - vel_var, vel + vel_var) * 256 / 60; // subpixels/frame
	p->vx = (s16)((v * cos_q14(a)) >> 14);
	p->vy = (s16)((v * sin_q14(a)) >> 14);
	return p;
}

void game_respawn(void) {
	s32 x = initial_spawn_x, y = initial_spawn_y;
	entity_t *pod = entity_by_ref(last_checkpoint);
	if (pod) {
		x = pod->px + FP(11);
		y = pod->py;
		entity_message(pod, EM_ACTIVATE, NULL);
	}
	entity_t *player = entity_spawn(ENTITY_TYPE_PLAYER, x, y);
	g.player = entity_ref(player);
	camera_follow(&camera, g.player, false);
	g.death_count++;
}

static void game_init(void) {
	const LevelDef *l = levels[g.level];
	map_load(l);
	engine.gravity = ACC(240);
	g.fade_white = 0;

	for (u16 i = 0; i < l->nents && i < 96; i++) {
		const LevelEntity *d = &l->ents[i];
		level_spawned[i] = entity_spawn(d->type, FP(d->x), FP(d->y));
	}
	for (u16 i = 0; i < l->nents && i < 96; i++) {
		if (level_spawned[i]) {
			entity_vtab[level_spawned[i]->type].settings(level_spawned[i], &l->ents[i]);
		}
	}

	camera.offset_x = 32;
	camera.offset_y = 0;
	last_checkpoint = entity_ref_none();
	g.player = entity_ref_none();

	entity_t *p = entity_first_of_type(ENTITY_TYPE_PLAYER);
#ifdef DEBUG_X
	if (p) {
		static u8 debug_used = 0;
		if (!debug_used && DEBUG_X >= 0) {
			debug_used = 1;
			p->px = FP(DEBUG_X);
			p->py = FP(DEBUG_Y);
		}
	}
#endif
	if (p) {
		g.player = entity_ref(p);
		camera_follow(&camera, g.player, true);
		initial_spawn_x = p->px;
		initial_spawn_y = p->py;
	}
	g.tubes_collected = 0;
	g.death_count = 0;
	g.tubes_total = entities_count_type(ENTITY_TYPE_TEST_TUBE);
	map_draw();
}

static void game_cleanup(void) {
	g.level_time = engine.time;
}

static void game_update(void) {
	entities_update();
	camera_update(&camera);
}

static void game_draw(void) {
	map_draw();
	entities_draw();
	gfx_flash_white(g.fade_white);
}

const scene_t scene_game = {game_init, game_update, game_draw, game_cleanup};

// ---------------------------------------------------------------------------
// Title

static u8 intro_sound_played;

static void title_init(void) {
	intro_sound_played = false;
}

static void title_update(void) {
	if (engine.time > T(0.5) && !intro_sound_played) {
		SOUND(SND_INTRO);
		intro_sound_played = true;
	}
	if (input_pressed(BTN_SHOOT | BTN_JUMP | BTN_START)) {
#ifdef DEBUG_LEVEL
		game_set_level(DEBUG_LEVEL);
#else
		game_set_level(0);
#endif
		g.music_volume = 256;
		music_play();
		engine_set_scene(&scene_game);
	}
}

static void title_draw(void) {
	s32 d = engine.time - T(1);
	// v = clamp(-d^3, 0, 1) in Q8
	s32 v = 0;
	if (d < 0) {
		s32 nd = -d;
		v = ((nd * nd / 600) * nd / 600) * 256 / 600;
		if (v > 256) v = 256;
	}
	s16 x_logo = 44 - (s16)(v * 204 / 256) + OX;
	s16 x_player = 166 + (s16)(v * 148 / 256) + OX;
	gfx_draw_image(&img_title_biolab, x_logo, 26 + OY, TITLE_BIOLAB_W, TITLE_BIOLAB_H);
	gfx_draw_image(&img_title_disaster, x_logo, 70 + OY, TITLE_DISASTER_W, TITLE_DISASTER_H);
	gfx_draw_image(&img_title_player, x_player, 56 + OY, TITLE_PLAYER_W, TITLE_PLAYER_H);
	s32 k = d * 2 / 600;
	if (d > T(2) || (k % 2) == 0) {
		text_draw(120 + OX, 140 + OY, "Press START to Play", ALIGN_CENTER);
	}
}

const scene_t scene_title = {title_init, title_update, title_draw, NULL};

// ---------------------------------------------------------------------------
// Stats

static char s_time[24], s_tubes[24], s_deaths[24];

static void stats_init(void) {
	u32 t = g.level_time; // 1/600 s
	sprintf(s_time, "%u.%02us", (unsigned)(t / 600), (unsigned)((t % 600) / 6));
	sprintf(s_tubes, "%d/%d", g.tubes_collected, g.tubes_total);
	sprintf(s_deaths, "%d", g.death_count);
}

static void stats_update(void) {
	if (input_pressed(BTN_SHOOT | BTN_JUMP | BTN_START)) {
		engine_set_scene(&scene_game);
	}
}

static void stats_draw(void) {
	text_draw(120 + OX, 20 + OY, "Level Complete!", ALIGN_CENTER);
	text_draw(98 + OX, 56 + OY, "Time:", ALIGN_RIGHT);
	text_draw(104 + OX, 56 + OY, s_time, ALIGN_LEFT);
	text_draw(98 + OX, 68 + OY, "Tubes Collected:", ALIGN_RIGHT);
	text_draw(104 + OX, 68 + OY, s_tubes, ALIGN_LEFT);
	text_draw(98 + OX, 80 + OY, "Deaths:", ALIGN_RIGHT);
	text_draw(104 + OX, 80 + OY, s_deaths, ALIGN_LEFT);
	text_draw(120 + OX, 140 + OY, "Press START to Proceed", ALIGN_CENTER);
}

const scene_t scene_stats = {stats_init, stats_update, stats_draw, NULL};

// ---------------------------------------------------------------------------
// Credits

static const char *credits =
	"          Thanks for Playing!\n"
	"\n"
	"\n"
	"Concept Graphics & Programming\n"
	"    Dominic Szablewski\n"
	"\n"
	"Music\n"
	"    Andreas Loesch\n"
	"\n"
	"Beta Testing\n"
	"    Sebastian Gerhard\n"
	"    Benjamin Hartmann\n"
	"    Jos Hirth\n"
	"    David Jacovangelo\n"
	"    Tim Juraschka\n"
	"    Christopher Klink\n"
	"    Mike Neumann\n"
	"\n"
	"\n"
	"\n"
	"\n"
	"Made with high_impact\n"
	"github.com/phoboslab/high_impact\n"
	"\n"
	"PlayStation port made with PSn00bSDK\n";

static void credits_init(void) {
}

static void credits_update(void) {
	if (input_pressed(BTN_SHOOT | BTN_JUMP | BTN_START) || engine.time > T(44)) {
		engine_set_scene(&scene_title);
	}
}

static void credits_draw(void) {
	// y = height - (time - 3.5) * 10
	s32 y = SCREEN_H - (engine.time - T(3.5)) / 60;
	text_draw(32 + OX, (s16)y, credits, ALIGN_LEFT);
	// fade in from white over one second
	s32 fade = 256 - engine.time * 256 / 600;
	if (fade > 0) {
		gfx_flash_white((u16)fade);
	}
}

const scene_t scene_credits = {credits_init, credits_update, credits_draw, NULL};
