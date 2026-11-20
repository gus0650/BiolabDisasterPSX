#ifndef GAME_H
#define GAME_H

#include "engine.h"

typedef struct {
	entity_ref_t player;
	s16 death_count;
	s16 tubes_collected;
	s16 tubes_total;
	s32 level_time;
	s16 level;
	s16 music_volume;
	u8 random_mode;      // playing generated levels
	u16 random_count;    // completed random levels (difficulty)
	u32 random_seed;     // seed of the first random level
	u16 fade_white;
} global_t;

extern global_t g;

extern const scene_t scene_title;
extern const scene_t scene_game;
extern const scene_t scene_credits;
extern const scene_t scene_stats;

void game_set_level(s16 level);
void game_set_checkpoint(entity_ref_t checkpoint);
void game_respawn(void);
entity_t *game_spawn_particle(s32 px, s32 py, s16 vel, s16 vel_var, u8 angle, u8 angle_var, const AnimDef *def);
entity_list_t game_targets(const LevelEntity *def);
void game_fade_white(u16 amount);

void entities_register(void);

#endif
