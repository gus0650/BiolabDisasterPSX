// All entity types of Biolab Disaster, ported from src/entities/*.c

#include "game.h"
#include "levelgen.h"

#define PLAYER() entity_by_ref(g.player)
#define DEC(V) do { (V) -= TICK; if ((V) < -30000) (V) = -30000; } while (0)

static s16 dist_px(s32 a, s32 b) {
	s32 d = (a - b) >> 8;
	return (s16)(d < 0 ? -d : d);
}

// Player position relative values used by the enemies
#define PDX(SELF, P) dist_px((P)->px, (SELF)->px)
#define PDY(SELF, P) dist_px((P)->py, (SELF)->py)
#define PDIR(SELF, P) (((P)->px - (SELF)->px) < 0 ? -1 : 1)

static void spawn_gibs(entity_t *self, entity_t *other, s16 count, const AnimDef *def) {
	u8 a = atan2_256(other->vy, other->vx);
	for (s16 i = 0; i < count; i++) {
		game_spawn_particle(self->px, self->py, 120, 30, a, 32, def);
	}
}

// ---------------------------------------------------------------------------
// Blob

ANIM(blob_idle, sheet_blob, 0.5, 1,2,2,2,2,2,2,2,2,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2);
ANIM(blob_crawl, sheet_blob, 0.1, 0, 1);
ANIM(blob_jump, sheet_blob, 0.2, 2, 3, 4);
ANIM(blob_hit, sheet_blob, 0.1, 5);
ANIM(blob_gib, sheet_blob_gib, 10, 0, 1, 2);

static void blob_init(entity_t *self) {
	self->group = GROUP_ENEMY;
	self->check_against = GROUP_PLAYER;
	self->sx = 10; self->sy = 13;
	self->ox = 3; self->oy = 3;
	self->health = 20;
	self->anim = anim(&blob_idle);
	anim_goto_rand(&self->anim);
	self->anim.flip_x = rand_int(0, 1);
	self->physics = PHYSICS_PASSIVE;
}

static void blob_update(entity_t *self) {
	entity_t *p = PLAYER();
	if (p) {
		s16 dx = PDX(self, p), dy = PDY(self, p);
		s16 dir = PDIR(self, p);
		if (!self->blob.seen_player) {
			if (dx < 64 && dy < 20) {
				self->blob.seen_player = true;
			}
		}
		else if (self->on_ground && self->anim.def != &blob_hit) {
			if (self->blob.jump_timer < 30000) self->blob.jump_timer += TICK;
			if (
				self->anim.def != &blob_jump && self->blob.jump_timer > T(0.5) && (
					(dx < 40 && dy > 20) ||
					map_tile_at_px(PX(self->px) + self->sx * dir, PX(self->py) + self->sy) == 1
				)
			) {
				self->anim = anim(&blob_jump);
				self->anim.flip_x = dir < 0;
				self->vx = 0;
			}
			else if (self->anim.def == &blob_jump && anim_looped(&self->anim)) {
				self->vy = VEL(-70);
				self->vx = self->anim.flip_x ? VEL(-60) : VEL(60);
				self->blob.in_jump = true;
			}
			else if (self->anim.def != &blob_jump && self->blob.jump_timer > T(0.2)) {
				self->anim = anim(&blob_crawl);
				self->anim.flip_x = dir < 0;
				self->vx = VEL(20) * dir;
			}
		}
	}
	if (self->anim.def == &blob_hit && anim_looped(&self->anim)) {
		self->anim = anim(&blob_idle);
	}
	if (self->blob.in_jump && self->vx == 0 && self->anim.def != &blob_hit) {
		self->vx = self->anim.flip_x ? VEL(-30) : VEL(30);
	}
	bool was_on_ground = self->on_ground;
	entity_base_update(self);
	if (self->on_ground && !was_on_ground && self->anim.def != &blob_hit) {
		self->blob.in_jump = false;
		self->blob.jump_timer = 0;
		u8 flip = self->anim.flip_x;
		self->anim = anim(&blob_idle);
		self->anim.flip_x = flip;
		self->vx = 0;
	}
}

static void blob_damage(entity_t *self, entity_t *other, s16 damage) {
	self->anim = anim(&blob_hit);
	self->blob.seen_player = true;
	self->blob.in_jump = false;
	self->vx = other->vx > 0 ? VEL(50) : VEL(-50);
	spawn_gibs(self, other, self->health <= damage ? 30 : 5, &blob_gib);
	SOUND(SND_WETGIB);
	entity_base_damage(self, other, damage);
}

static void enemy_touch(entity_t *self, entity_t *other) {
	entity_damage(other, self, 10);
}

// ---------------------------------------------------------------------------
// Crate

ANIM(crate_idle, sheet_crate, 1.0, 0);
ANIM(crate_debris, sheet_crate_debris, 5.0, 2, 3, 6, 7);

static void crate_init(entity_t *self) {
	self->anim = anim(&crate_idle);
	self->sx = 8; self->sy = 8;
	self->fx = FRICTION(4);
	self->health = 5;
	self->restitution = 102; // 0.4
	self->mass = 64;         // 0.25
	self->group = GROUP_BREAKABLE;
	self->check_against = GROUP_NONE;
	self->physics = PHYSICS_ACTIVE;
}

static void crate_kill(entity_t *self) {
	SOUND(SND_CRACK);
	for (s16 i = 0; i < 10; i++) {
		game_spawn_particle(self->px, self->py, 120, 30, 0, 128, &crate_debris);
	}
}

// ---------------------------------------------------------------------------
// Debris

ANIM(debris_idle, sheet_debris, 5.0, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14);

static void debris_settings(entity_t *self, const LevelEntity *def) {
	self->debris.count = def->a;
	self->debris.duration = def->b;
	self->sx = def->sx;
	self->sy = def->sy;
}

static void debris_trigger(entity_t *self, entity_t *other) {
	self->debris.duration_time = self->debris.duration;
}

static void debris_update(entity_t *self) {
	if (self->debris.duration_time <= 0) {
		return;
	}
	self->debris.emit_time -= TICK;
	self->debris.duration_time -= TICK;
	if (self->debris.emit_time < 0) {
		self->debris.emit_time = self->debris.count ? self->debris.duration / self->debris.count : 600;
		s32 x = self->px + FP(rand_int(0, self->sx));
		s32 y = self->py + FP(rand_int(0, self->sy));
		entity_t *p = game_spawn_particle(x, y, 30, 10, 0, 128, &debris_idle);
		if (p) {
			p->particle.life_time = T(2);
			p->particle.fade_time = T(1);
			p->restitution = 154; // 0.6
		}
	}
}

// ---------------------------------------------------------------------------
// Delay

static void delay_settings(entity_t *self, const LevelEntity *def) {
	self->delay.targets = game_targets(def);
	self->delay.delay = def->a;
}

static void delay_update(entity_t *self) {
	self->delay.delay_time -= TICK;
	if (!self->delay.fire || self->delay.delay_time > 0) {
		return;
	}
	self->delay.fire = false;
	for (u16 i = 0; i < self->delay.targets.len; i++) {
		entity_t *t = entity_by_ref(self->delay.targets.e[i]);
		if (t) {
			entity_t *by = entity_by_ref(self->delay.triggered_by);
			entity_trigger(t, by ? by : self);
		}
	}
}

static void delay_trigger(entity_t *self, entity_t *other) {
	self->delay.fire = true;
	self->delay.delay_time = self->delay.delay;
	self->delay.triggered_by = entity_ref(other);
}

// ---------------------------------------------------------------------------
// Dropper

ANIM(dropper_idle, sheet_dropper, 1.0, 0);
ANIM(dropper_shoot, sheet_dropper, 0.2, 1, 2, 2, 1);
ANIM(dropper_hit, sheet_dropper, 0.2, 3);
ANIM(dropper_shot_idle, sheet_dropper_shot, 1.0, 8);
ANIM(dropper_shot_hit, sheet_dropper_shot, 0.1, 9, 10, 11);

static void dropper_init(entity_t *self) {
	self->anim = anim(&dropper_idle);
	self->sx = 14; self->sy = 8;
	self->ox = 1; self->oy = 0;
	self->group = GROUP_ENEMY;
	self->check_against = GROUP_PLAYER;
	self->health = 80;
	self->dropper.shoot_wait_time = T(1);
	self->dropper.shoot_time = T(10);
	self->dropper.can_shoot = false;
}

static void dropper_update(entity_t *self) {
	entity_t *p = PLAYER();
	DEC(self->dropper.shoot_wait_time);
	DEC(self->dropper.shoot_time);
	if (self->anim.def == &dropper_hit && anim_looped(&self->anim)) {
		self->anim.def = &dropper_idle;
		self->dropper.shoot_wait_time = T(0.5);
	}
	else if (
		p && self->anim.def == &dropper_idle &&
		self->dropper.shoot_wait_time < 0 &&
		dist_lt(p->px - self->px, p->py - self->py, 128)
	) {
		self->anim = anim(&dropper_shoot);
		self->dropper.shoot_time = T(0.8);
		self->dropper.can_shoot = true;
	}
	else if (self->anim.def == &dropper_shoot && self->dropper.can_shoot && self->dropper.shoot_time < 0) {
		self->dropper.can_shoot = false;
		entity_t *drop = entity_spawn(ENTITY_TYPE_PROJECTILE, self->px + FP(5), self->py + FP(6));
		if (drop) {
			drop->sx = 4; drop->sy = 4;
			drop->gravity = 1;
			drop->ox = 2; drop->oy = 4;
			drop->check_against = GROUP_PLAYER | GROUP_BREAKABLE;
			drop->anim = anim(&dropper_shot_idle);
			drop->projectile.anim_hit = &dropper_shot_hit;
			drop->vx = 0; drop->vy = 0;
		}
	}
	if (self->anim.def == &dropper_shoot && anim_looped(&self->anim)) {
		self->anim = anim(&dropper_idle);
		self->dropper.shoot_wait_time = T(0.5);
	}
	entity_base_update(self);
}

static void dropper_damage(entity_t *self, entity_t *other, s16 damage) {
	self->anim = anim(&dropper_hit);
	self->vx = other->vx > 0 ? VEL(50) : VEL(-50);
	spawn_gibs(self, other, self->health <= damage ? 20 : 3, &blob_gib);
	SOUND(SND_WETGIB);
	entity_base_damage(self, other, damage);
}

// ---------------------------------------------------------------------------
// Earthquake

static void earthquake_settings(entity_t *self, const LevelEntity *def) {
	self->earthquake.strength = def->a; // Q8 pixels
	self->earthquake.duration = def->b;
}

static void earthquake_update(entity_t *self) {
	if (self->earthquake.time <= 0) {
		return;
	}
	self->earthquake.time -= TICK;
	if (self->earthquake.time < 0) {
		self->earthquake.time = 0;
	}
	// shake = strength * p * 0.5 (Q8 pixels == subpixels)
	s32 shake = (s32)self->earthquake.strength * self->earthquake.time / self->earthquake.duration / 2;
	if (shake > 128) {
		engine.vx += (s32)rand_int(-shake, shake);
		engine.vy += (s32)rand_int(-shake, shake);
	}
}

static void earthquake_trigger(entity_t *self, entity_t *other) {
	SOUND(SND_EARTHQUAKE);
	self->earthquake.time = self->earthquake.duration;
}

// ---------------------------------------------------------------------------
// End hub

ANIM(endhub_idle, sheet_endhub, 0.5, 0, 1);
ANIM(endhub_activated, sheet_endhub, 0.5, 2, 3);

static void end_hub_init(entity_t *self) {
	self->anim = anim(&endhub_idle);
	self->sx = 24; self->sy = 24;
	self->draw_order = -1;
}

#define PLASMA_COUNT 100

static void end_hub_update(entity_t *self) {
	if (self->end_hub.stage >= 1) {
		// fade out the music: volume -= tick * 0.25
		if (g.music_volume > 0) {
			g.music_volume -= 1; // 256 * 0.25 / 60 per frame ~= 1
			music_set_volume(g.music_volume);
		}
	}
}

static void end_hub_trigger(entity_t *self, entity_t *other) {
	self->end_hub.stage++;
	if (self->end_hub.stage == 1) {
		SOUND(SND_RESPAWN);
		self->anim = anim(&endhub_activated);
	}
	else if (self->end_hub.stage == 2) {
		SOUND(SND_THEEND);
		// one entity draws all plasma sprites
		entity_spawn(ENTITY_TYPE_END_HUB_PLASMA, self->px, self->py - FP(24));
	}
	else if (self->end_hub.stage == 3) {
		entity_spawn(ENTITY_TYPE_END_HUB_FADE, self->px, self->py);
	}
	else if (self->end_hub.stage == 4) {
		engine_set_scene(&scene_credits);
	}
}

// Plasma: the original uses 100 sprites moved by 2D noise; we use fewer
// sprites moved by sums of sines with incommensurate frequencies.
static s16 wave(s16 i, s32 t, u16 fi, u16 ft) {
	u8 a = (u8)((i * fi) + (t * ft >> 8));
	return sin_q14(a);
}

static void end_hub_plasma_draw(entity_t *self) {
	self->end_hub_plasma.time += TICK;
	self->draw_order = 8;
	s32 d = self->end_hub_plasma.time; // 1/600 s
	s32 t = d + 1600;
	// spread = clamp(80 / (d*d*0.7), 0, 1000), Q8, clamped at 128x (off screen)
	s32 d2 = (d * d) >> 8;
	s32 sp = d2 > 0 ? 41142857L / d2 : 32767;
	s16 spread = sp > 32767 ? 32767 : (s16)sp;
	s16 bx = (s16)((self->px - engine.vx) >> 8) - 10;
	s16 by = (s16)((self->py - engine.vy) >> 8);
	for (s16 n = 0; n < PLASMA_COUNT; n++) {
		s16 i = n * (100 / PLASMA_COUNT);
		s32 xn = wave(i, t, 26, 29) + 2 * (s32)wave(i, t, 62, 82) + wave(i, t + 77, 197, 288) / 2;
		s32 yn = wave(i + 31, t, 26, 36) + 2 * (s32)wave(i + 7, t, 62, 69) + wave(i + 3, t, 197, 263) / 2;
		s32 ox = ((s32)(s16)((xn * 40) >> 14) * spread) >> 8;
		s32 oy = ((s32)(s16)((yn * 30) >> 14) * spread) >> 8;
		if (abs32(ox) > 400 || abs32(oy) > 400) {
			continue;
		}
		sprite_draw_frame(&sheet_plasma, 0, bx + ox, by + oy, 0, 0);
	}
}

static void end_hub_fade_draw(entity_t *self) {
	self->draw_order = 64;
	self->end_hub_fade.time += TICK;
	s32 a = (s32)self->end_hub_fade.time * 256 / 600;
	game_fade_white(a > 256 ? 256 : a);
}

// ---------------------------------------------------------------------------
// Glass dome

ANIM(glass_dome_idle, sheet_glass_dome, 1.0, 0);
ANIM(glass_shards, sheet_glass_shards, 5.0, 0, 1, 2, 3);

static void glass_dome_init(entity_t *self) {
	self->anim = anim(&glass_dome_idle);
	self->sx = 40; self->sy = 32;
	self->health = 80;
	self->group = GROUP_BREAKABLE;
	self->check_against = GROUP_NONE;
	self->physics = PHYSICS_FIXED;
}

static void glass_dome_damage(entity_t *self, entity_t *other, s16 damage) {
	SOUND(SND_GLASS_IMPACT);
	for (s16 i = 0; i < 3; i++) {
		game_spawn_particle(other->px, other->py, 120, 30, 0, 128, &glass_shards);
	}
	entity_base_damage(self, other, damage);
}

static void glass_dome_kill(entity_t *self) {
	SOUND(SND_GLASS_SHATTER);
	for (s16 i = 0; i < 100; i++) {
		s32 x = self->px + FP(rand_int(0, self->sx));
		s32 y = self->py + FP(rand_int(0, self->sy));
		game_spawn_particle(x, y, 120, 30, 0, 128, &glass_shards);
	}
}

// ---------------------------------------------------------------------------
// Grunt

ANIM(grunt_idle, sheet_grunt, 0.5, 0,0,0,0,1,0,0,0,1,1,1);
ANIM(grunt_walk, sheet_grunt, 0.1, 6,7,8,9,10,11);
ANIM(grunt_shoot, sheet_grunt, 0.2, 2);
ANIM(grunt_hit, sheet_grunt, 0.1, 3);
ANIM(grunt_gib, sheet_grunt_gib, 5.0, 16, 17, 40, 41);
ANIM(grunt_gun, sheet_grunt_gun, 1.0, 11);
ANIM(grunt_shot_idle, sheet_grunt_shot, 1.0, 0);
ANIM(grunt_shot_hit, sheet_grunt_shot, 0.1, 0,1,2,3,4,5);

static void grunt_init(entity_t *self) {
	self->group = GROUP_ENEMY;
	self->check_against = GROUP_PLAYER;
	self->sx = 10; self->sy = 13;
	self->ox = 3; self->oy = 3;
	self->fx = FRICTION(6);
	self->health = 20;
	self->anim = anim(&grunt_idle);
	self->grunt.flip = rand_int(0, 1);
	self->physics = PHYSICS_PASSIVE;
}

static void grunt_update(entity_t *self) {
	entity_t *p = PLAYER();
	DEC(self->grunt.shoot_time);
	if (p) {
		s16 dx = PDX(self, p), dy = PDY(self, p);
		s16 dir = PDIR(self, p);
		if (!self->grunt.seen_player) {
			if (dx < 160 && dy < 32) {
				self->grunt.seen_player = true;
				self->grunt.shoot_time = T(1.5);
			}
		}
		else if (self->on_ground && self->anim.def != &grunt_hit) {
			if ((dx > 160 || (dx > 96 && self->anim.def == &grunt_walk)) && self->grunt.shoot_time < 0) {
				if (self->anim.def != &grunt_walk) {
					self->anim = anim(&grunt_walk);
				}
				self->vx = VEL(30) * dir;
			}
			else if (self->anim.def == &grunt_walk) {
				self->anim = anim(&grunt_idle);
				self->vx = 0;
				self->grunt.shoot_time = T(1.0);
			}
			else if (dy < 64 && self->grunt.shoot_time < 0) {
				entity_t *plasma = entity_spawn(ENTITY_TYPE_PROJECTILE,
					self->px + FP(self->grunt.flip ? -3 : 5), self->py + FP(6));
				if (plasma) {
					plasma->check_against = GROUP_PLAYER | GROUP_BREAKABLE;
					plasma->anim = anim(&grunt_shot_idle);
					plasma->projectile.flip = self->grunt.flip;
					plasma->anim.flip_x = self->grunt.flip;
					plasma->projectile.anim_hit = &grunt_shot_hit;
					plasma->vx = self->grunt.flip ? VEL(-120) : VEL(120);
					plasma->vy = 0;
				}
				SOUND(SND_GRUNT_PLASMA);
				self->grunt.shoot_time = T(2.0);
				if (self->anim.def != &grunt_idle) {
					self->anim = anim(&grunt_idle);
				}
			}
			if (self->anim.def == &grunt_idle && self->grunt.shoot_time < T(0.5)) {
				self->anim = anim(&grunt_shoot);
			}
			self->grunt.flip = dir < 0;
		}
	}
	if (self->anim.def == &grunt_hit && anim_looped(&self->anim)) {
		self->anim = anim(&grunt_idle);
	}
	entity_base_update(self);
	self->anim.flip_x = self->grunt.flip;
	self->anim.tile_offset = self->grunt.flip ? 12 : 0;
}

static void grunt_damage(entity_t *self, entity_t *other, s16 damage) {
	self->anim = anim(&grunt_hit);
	self->vx = other->vx > 0 ? VEL(60) : VEL(-60);
	if (self->grunt.shoot_time > T(-0.3)) {
		self->grunt.shoot_time = T(0.7);
	}
	self->grunt.seen_player = true;
	spawn_gibs(self, other, self->health <= damage ? 10 : 3, &grunt_gib);
	if (self->health <= damage) {
		game_spawn_particle(self->px, self->py, 60, 10, atan2_256(other->vy, other->vx), 32, &grunt_gun);
	}
	SOUND(SND_DRYGIB);
	entity_base_damage(self, other, damage);
}

// ---------------------------------------------------------------------------
// Hurt / level change / void

static void hurt_settings(entity_t *self, const LevelEntity *def) {
	self->hurt.damage = def->a ? def->a : 10;
}

static void hurt_trigger(entity_t *self, entity_t *other) {
	entity_damage(other, self, self->hurt.damage);
}

static void level_change_settings(entity_t *self, const LevelEntity *def) {
	self->level_change.level = def->a;
}

static void level_change_trigger(entity_t *self, entity_t *other) {
	if (self->level_change.level == LEVEL_NEXT_RANDOM) {
		g.random_count++;
	}
	else {
		game_set_level(self->level_change.level);
	}
	engine_set_scene(&scene_stats);
}

// ---------------------------------------------------------------------------
// Mine

ANIM(mine_idle, sheet_mine, 0.17, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,3);
ANIM(mine_gib, sheet_mine_gib, 1, 0);

static void mine_init(entity_t *self) {
	self->sx = 8; self->sy = 5;
	self->ox = 4; self->oy = 3;
	self->check_against = GROUP_PLAYER;
	self->anim = anim(&mine_idle);
	anim_goto_rand(&self->anim);
}

static void mine_kill(entity_t *self) {
	for (s16 i = 0; i < 10; i++) {
		entity_t *p = game_spawn_particle(self->px, self->py, 120, 30, 192, 64, &mine_gib);
		if (p) {
			p->physics = PHYSICS_WORLD;
		}
	}
	SOUND(SND_MINE);
}

static void mine_touch(entity_t *self, entity_t *other) {
	entity_kill(self);
	entity_damage(other, self, 10);
}

// ---------------------------------------------------------------------------
// Mover (elevator)

ANIM(elevator_idle, sheet_elevator, 1.0, 0);

static void mover_init(entity_t *self) {
	self->anim = anim(&elevator_idle);
	self->sx = 24; self->sy = 8;
	self->physics = PHYSICS_FIXED;
	self->gravity = 0;
	self->mover.speed = 20;
}

static void mover_settings(entity_t *self, const LevelEntity *def) {
	self->mover.targets = game_targets(def);
	self->mover.speed = def->a ? def->a : 20;
}

// squared distance between entity centers in (1/16 px)^2, using 16x16 bit
// multiplies (waypoints are close to each other)
static s32 center_dist2(entity_t *a, entity_t *b, s16 *dx, s16 *dy) {
	s32 x = (entity_cx(b) - entity_cx(a)) >> 4;
	s32 y = (entity_cy(b) - entity_cy(a)) >> 4;
	if (x > 32767) x = 32767; else if (x < -32767) x = -32767;
	if (y > 32767) y = 32767; else if (y < -32767) y = -32767;
	*dx = (s16)x;
	*dy = (s16)y;
	return (s32)*dx * *dx + (s32)*dy * *dy;
}

static void mover_update(entity_t *self) {
	if (self->mover.targets.len == 0) {
		return;
	}
	entity_t *target = entity_by_ref(self->mover.targets.e[self->mover.current_target]);
	if (!target) {
		entity_base_update(self);
		return;
	}
	s16 dx, dy;
	s32 prev = center_dist2(self, target, &dx, &dy);
	// direction toward the target is constant along the way; compute once
	if (!self->mover.dir_set) {
		self->mover.dir_set = 1;
		u32 len = isqrt32(prev);
		if (len > 0) {
			s32 sp = (s32)self->mover.speed * 256 / 60; // subpixels per frame
			self->vx = (s16)((s32)dx * sp / (s32)len);
			self->vy = (s16)((s32)dy * sp / (s32)len);
		}
		else {
			self->vx = self->vy = 0;
		}
	}
	entity_base_update(self);
	s32 cur = center_dist2(self, target, &dx, &dy);
	if (cur > prev || cur < 64) { // 0.5 px == 8 units of 1/16 px
		self->px = entity_cx(target) - ((s32)self->sx << 7);
		self->py = entity_cy(target) - ((s32)self->sy << 7);
		self->mover.current_target = (self->mover.current_target + 1) % self->mover.targets.len;
		self->mover.dir_set = 0;
	}
}

// ---------------------------------------------------------------------------
// Particle

static void particle_init(entity_t *self) {
	self->sx = 4; self->sy = 4;
	self->ox = 0; self->oy = 0;
	self->physics = PHYSICS_LITE;
	self->fx = FRICTION(0.25);
	self->restitution = rand_int(128, 205);
	self->particle.life_time = rand_int(T(2.0), T(5.0));
	self->particle.fade_time = T(6);
}

static void particle_update(entity_t *self) {
	self->particle.life_time -= TICK;
	if (self->particle.life_time < 0) {
		entity_kill(self);
		return;
	}
	s32 a = self->particle.life_time >= self->particle.fade_time ? 255 :
		divu16((u32)self->particle.life_time * 255, self->particle.fade_time);
	self->anim.alpha = a > 255 ? 255 : (a < 0 ? 0 : a);
	entity_base_update(self);
}

// ---------------------------------------------------------------------------
// Player

#define JUMP_INITIAL_VEL VEL(30)
#define JUMP_HIGH_TIME T(0.14)
#define JUMP_HIGH_ACCEL 780
#define ACCEL_GROUND ACC(600)
#define ACCEL_AIR ACC(300)
#define FRICTION_GROUND FRICTION(10)
#define FRICTION_AIR FRICTION(5)

ANIM(player_idle, sheet_player, 1.0, 0);
ANIM_ONCE(player_scratch, sheet_player, 0.3, 2,1,2,1,2);
ANIM_ONCE(player_shrug, sheet_player, 0.3, 3,3,3,3,3,3,4,3,3);
ANIM(player_run, sheet_player, 0.07, 6,7,8,9,10,11);
ANIM(player_jump, sheet_player, 1.0, 15);
ANIM(player_fall, sheet_player, 0.4, 12, 13);
ANIM(player_land, sheet_player, 0.15, 14);
ANIM(player_die, sheet_player, 0.07, 18,19,20,21,22,23,16,16,16);
ANIM(player_spawn, sheet_player, 0.07, 16,16,16,23,22,21,20,19,18);
ANIM(player_gib, sheet_player_gib, 10, 82, 94);
ANIM(player_gib_gun, sheet_player_gib, 10, 11);
ANIM(player_plasma_idle, sheet_projectile, 1.0, 0);
ANIM_ONCE(player_plasma_hit, sheet_projectile, 0.1, 0,1,2,3,4,5);

static void player_init(entity_t *self) {
	self->anim = anim(&player_spawn);
	self->physics = PHYSICS_ACTIVE;
	self->ox = 4; self->oy = 2;
	self->sx = 8; self->sy = 14;
	self->group = GROUP_PLAYER;
}

static void player_update(entity_t *self) {
	if (self->anim.def == &player_spawn) {
		if (anim_looped(&self->anim)) {
			self->anim = anim(&player_idle);
		}
		else {
			return;
		}
	}
	if (self->anim.def == &player_die) {
		if (anim_looped(&self->anim)) {
			entity_kill(self);
		}
		return;
	}

	self->fx = self->on_ground ? FRICTION_GROUND : FRICTION_AIR;
	bool did_move = false;
	if (input_state(BTN_LEFT)) {
		self->ax = self->on_ground ? -ACCEL_GROUND : -ACCEL_AIR;
		self->player.flip = true;
		did_move = true;
	}
	else if (input_state(BTN_RIGHT)) {
		self->ax = self->on_ground ? ACCEL_GROUND : ACCEL_AIR;
		self->player.flip = false;
		did_move = true;
	}
	else {
		self->ax = 0;
	}

	if (input_state(BTN_JUMP)) {
		if (self->on_ground && self->player.can_jump) {
			self->vy = -JUMP_INITIAL_VEL;
			self->player.can_jump = false;
			self->player.high_jump_time = JUMP_HIGH_TIME;
		}
		else if (self->player.high_jump_time > 0) {
			self->player.high_jump_time -= TICK;
			s16 d = self->player.high_jump_time;
			s16 f = d < 0 ? TICK + d : TICK;
			if (f < 0) {
				f = 0;
			}
			// vel.y -= 780 * f(seconds) in subpixels/frame
			self->vy -= (s16)((s32)JUMP_HIGH_ACCEL * 256 * f / 36000);
		}
	}
	else {
		self->player.high_jump_time = 0;
		self->player.can_jump = true;
	}

	if (input_pressed(BTN_SHOOT)) {
		entity_t *plasma = entity_spawn(ENTITY_TYPE_PROJECTILE,
			self->px + FP(self->player.flip ? -3 : 5), self->py + FP(6));
		if (plasma) {
			plasma->vx = self->player.flip ? VEL(-200) : VEL(200);
			plasma->vy = 0;
			plasma->check_against = GROUP_ENEMY | GROUP_BREAKABLE;
			plasma->anim = anim(&player_plasma_idle);
			plasma->projectile.flip = self->player.flip;
			plasma->anim.flip_x = self->player.flip;
			plasma->projectile.anim_hit = &player_plasma_hit;
		}
		SOUND(SND_PLASMA);
	}

	bool was_on_ground = self->on_ground;
	entity_base_update(self);

	if (!was_on_ground && self->on_ground) {
		self->anim = anim(&player_land);
	}
	else if (was_on_ground && (self->anim.def != &player_land || anim_looped(&self->anim))) {
		if (did_move) {
			if (self->anim.def != &player_run) {
				self->anim = anim(&player_run);
			}
			self->player.idle_time = 0;
			self->player.is_idle = false;
		}
		else {
			if (!self->player.is_idle || (!self->anim.def->loop && anim_looped(&self->anim))) {
				self->player.is_idle = true;
				self->player.idle_time = rand_int(T(3), T(7));
				self->anim = anim(&player_idle);
			}
			self->player.idle_time -= TICK;
			if (self->player.is_idle && self->player.idle_time < 0) {
				self->player.idle_time = rand_int(T(3), T(7));
				self->anim = rand_int(0, 1) == 0 ? anim(&player_scratch) : anim(&player_shrug);
			}
		}
	}
	else if (!was_on_ground) {
		if (self->vy < 0) {
			if (self->anim.def != &player_jump) {
				self->anim = anim(&player_jump);
			}
		}
		else if (self->anim.def != &player_fall) {
			self->anim = anim(&player_fall);
		}
		self->player.is_idle = false;
	}
	self->anim.flip_x = self->player.flip;
	self->anim.tile_offset = self->player.flip ? 24 : 0;
}

static void player_damage(entity_t *self, entity_t *other, s16 damage) {
	if (self->anim.def != &player_die) {
		self->anim = anim(&player_die);
		self->anim.flip_x = self->player.flip;
		self->anim.tile_offset = self->player.flip ? 24 : 0;
		for (s16 i = 0; i < 5; i++) {
			entity_t *p = game_spawn_particle(self->px, self->py, 70, 30, 192, 64, &player_gib);
			if (p) {
				p->physics = PHYSICS_MOVE;
				p->particle.life_time = T(0.5);
				p->particle.fade_time = T(0.5);
			}
		}
		entity_t *gun = game_spawn_particle(self->px, self->py, 60, 10, 192, 64, &player_gib_gun);
		if (gun) {
			gun->sx = 8; gun->sy = 8;
		}
		SOUND(SND_DIE);
	}
}

static void player_kill(entity_t *self) {
	game_respawn();
}

// ---------------------------------------------------------------------------
// Projectile

static void projectile_init(entity_t *self) {
	self->sx = 6; self->sy = 3;
	self->ox = 1; self->oy = 2;
	self->gravity = 0;
	self->physics = PHYSICS_WORLD;
}

static void projectile_set_hit(entity_t *self) {
	self->projectile.has_hit = true;
	self->vx = 0;
	if (self->projectile.anim_hit) {
		self->anim = anim(self->projectile.anim_hit);
		self->anim.flip_x = self->projectile.flip;
	}
}

static void projectile_update(entity_t *self) {
	entity_base_update(self);
	if (self->projectile.has_hit && anim_looped(&self->anim)) {
		entity_kill(self);
	}
}

static void projectile_collide(entity_t *self, s16 nx, s16 ny, bool world) {
	if (self->projectile.has_hit) {
		return;
	}
	projectile_set_hit(self);
}

static void projectile_touch(entity_t *self, entity_t *other) {
	if (self->projectile.has_hit) {
		return;
	}
	entity_damage(other, self, 10);
	projectile_set_hit(self);
}

// ---------------------------------------------------------------------------
// Respawn pod

ANIM(pod_idle, sheet_respawn_pod, 0.5, 0, 1);
ANIM(pod_activated, sheet_respawn_pod, 0.5, 2, 3);
ANIM(pod_respawn, sheet_respawn_pod, 0.3, 0, 4);

static void respawn_pod_init(entity_t *self) {
	self->check_against = GROUP_PLAYER;
	self->anim = anim(&pod_idle);
	self->sx = 18; self->sy = 16;
	self->draw_order = -1;
}

static void respawn_pod_update(entity_t *self) {
	if (self->anim.def == &pod_respawn && anim_looped(&self->anim) > 4) {
		self->anim = anim(&pod_activated);
	}
	entity_base_update(self);
}

static void respawn_pod_touch(entity_t *self, entity_t *other) {
	if (self->respawn_pod.activated) {
		return;
	}
	self->respawn_pod.activated = true;
	self->anim = anim(&pod_activated);
	game_set_checkpoint(entity_ref(self));
	SOUND(SND_RESPAWN);
}

static void respawn_pod_message(entity_t *self, u16 message, void *data) {
	if (message == EM_ACTIVATE) {
		self->anim = anim(&pod_respawn);
	}
}

// ---------------------------------------------------------------------------
// Spewer

ANIM(spewer_idle, sheet_spewer, 0.5, 0,0,0,0,0,0,0,0,1);
ANIM(spewer_spew, sheet_spewer, 0.1, 1,2,2,1,1);
ANIM(spewer_hit, sheet_spewer, 0.2, 3);
ANIM(spewer_gib, sheet_spewer_gib, 1.0, 16);

static void spewer_init(entity_t *self) {
	self->group = GROUP_ENEMY;
	self->check_against = GROUP_PLAYER;
	self->sx = 16; self->sy = 8;
	self->health = 20;
	self->anim = anim(&spewer_idle);
	self->spewer.shoot_wait_time = T(1);
	self->spewer.shoot_time = T(10);
}

static void spewer_update(entity_t *self) {
	DEC(self->spewer.shoot_time);
	DEC(self->spewer.shoot_wait_time);
	entity_t *p = PLAYER();
	if (self->anim.def == &spewer_hit && anim_looped(&self->anim)) {
		self->anim = anim(&spewer_idle);
		self->spewer.shoot_wait_time = T(0.5);
	}
	else if (
		p && self->anim.def == &spewer_idle &&
		self->spewer.shoot_wait_time < 0 &&
		dist_lt(p->px - self->px, p->py - self->py, 80)
	) {
		self->anim = anim(&spewer_spew);
		self->spewer.shoot_time = T(0.45);
		self->spewer.can_shoot = true;
	}
	else if (self->anim.def == &spewer_spew && self->spewer.can_shoot && self->spewer.shoot_time < 0) {
		self->spewer.can_shoot = false;
		entity_spawn(ENTITY_TYPE_SPEWER_SHOT, self->px + FP(4), self->py + FP(4));
	}
	if (self->anim.def == &spewer_spew && anim_looped(&self->anim)) {
		self->anim = anim(&spewer_idle);
		self->spewer.shoot_wait_time = T(1.5);
	}
	if (p) {
		self->anim.flip_x = (self->px - p->px) < 0;
	}
	entity_base_update(self);
}

static void spewer_damage(entity_t *self, entity_t *other, s16 damage) {
	self->anim = anim(&spewer_hit);
	s16 n = self->health <= damage ? 10 : 3;
	u8 a = atan2_256(other->vy, other->vx);
	for (s16 i = 0; i < n; i++) {
		game_spawn_particle(entity_cx(self), entity_cy(self), 120, 30, a, 32, &spewer_gib);
	}
	SOUND(SND_DRYGIB);
	entity_base_damage(self, other, damage);
}

ANIM(spewer_shot_idle, sheet_spewer_gib, 5.0, 16);

static void spewer_shot_init(entity_t *self) {
	self->check_against = GROUP_PLAYER;
	self->sx = 4; self->sy = 4;
	self->fx = FRICTION(1);
	self->anim = anim(&spewer_shot_idle);
	self->restitution = 179; // 0.7
	self->physics = PHYSICS_LITE;
	entity_t *p = PLAYER();
	s16 dir = (p && self->px - p->px > 0) ? -1 : 1;
	self->vx = rand_int(VEL(40), VEL(120)) * dir;
	self->vy = VEL(-100);
}

static void spewer_shot_collide(entity_t *self, s16 nx, s16 ny, bool world) {
	self->spewer_shot.bounce_count++;
	if (self->spewer_shot.bounce_count >= 3) {
		entity_kill(self);
	}
}

static void spewer_shot_touch(entity_t *self, entity_t *other) {
	entity_damage(other, self, 10);
	entity_kill(self);
}

// ---------------------------------------------------------------------------
// Spike

ANIM(spike_crawl, sheet_spike, 0.08, 5, 6, 7);
ANIM(spike_shoot, sheet_spike, 0.15, 3,3,0,1,2,2,2,1,3,3,3);
ANIM(spike_hit, sheet_spike, 0.1, 8);
ANIM(spike_gib, sheet_spike_small, 5.0, 28, 29, 38, 39);
ANIM(spike_shot_idle, sheet_spike_small, 0.1, 8, 9);
ANIM(spike_shot_hit, sheet_spike_small, 0.1, 18, 19);

static void spike_init(entity_t *self) {
	self->group = GROUP_ENEMY;
	self->check_against = GROUP_PLAYER;
	self->sx = 16; self->sy = 9;
	self->ox = 0; self->oy = 7;
	self->health = 30;
	self->anim = anim(&spike_crawl);
	self->physics = PHYSICS_PASSIVE;
	self->spike.shoot_wait_time = T(10);
}

static void spike_shoot_one(s32 x, s32 y, s16 vx, s16 vy) {
	entity_t *s = entity_spawn(ENTITY_TYPE_PROJECTILE, x, y);
	if (s) {
		s->sx = 4; s->sy = 4;
		s->ox = 2; s->oy = 2;
		s->check_against = GROUP_PLAYER | GROUP_BREAKABLE;
		s->anim = anim(&spike_shot_idle);
		s->projectile.anim_hit = &spike_shot_hit;
		s->vx = vx;
		s->vy = vy;
	}
}

static void spike_update(entity_t *self) {
	entity_t *p = PLAYER();
	if (self->anim.def == &spike_shoot) {
		if (anim_looped(&self->anim)) {
			self->anim = anim(&spike_crawl);
		}
		DEC(self->spike.shoot_time);
		if (self->spike.can_shoot && self->spike.shoot_time < 0) {
			self->spike.can_shoot = false;
			spike_shoot_one(self->px + FP(6), self->py - FP(4), 0, VEL(-60));
			spike_shoot_one(self->px + FP(1), self->py - FP(2), VEL(-60), 0);
			spike_shoot_one(self->px + FP(10), self->py - FP(2), VEL(60), 0);
		}
	}
	else if (self->anim.def == &spike_crawl) {
		DEC(self->spike.shoot_wait_time);
		if (p && self->spike.shoot_wait_time < 0 && dist_lt(p->px - self->px, p->py - self->py, 160)) {
			self->anim = anim(&spike_shoot);
			self->spike.shoot_wait_time = T(5);
			self->spike.shoot_time = T(1.2);
			self->spike.can_shoot = true;
			self->vx = 0;
		}
		else {
			s16 cx = PX(self->px) + (self->spike.flip ? 4 : self->sx - 4);
			s16 cy = PX(self->py) + self->sy + 1;
			if (map_tile_at_px(cx, cy) == 0) {
				self->spike.flip = !self->spike.flip;
			}
			self->vx = self->spike.flip ? VEL(-14) : VEL(14);
		}
	}
	else if (self->anim.def == &spike_hit && anim_looped(&self->anim)) {
		self->anim = anim(&spike_crawl);
	}
	entity_base_update(self);
}

static void spike_damage(entity_t *self, entity_t *other, s16 damage) {
	self->anim = anim(&spike_hit);
	self->vx = other->vx > 0 ? VEL(50) : VEL(-50);
	s16 n = self->health <= damage ? 15 : 3;
	u8 a = atan2_256(other->vy, other->vx);
	for (s16 i = 0; i < n; i++) {
		entity_t *pt = game_spawn_particle(entity_cx(self), entity_cy(self), 120, 30, a, 32, &spike_gib);
		if (pt) {
			pt->ox = 2; pt->oy = 2;
		}
	}
	SOUND(SND_DRYGIB);
	entity_base_damage(self, other, damage);
}

static void spike_collide(entity_t *self, s16 nx, s16 ny, bool world) {
	if (nx != 0) {
		self->spike.flip = !self->spike.flip;
	}
}

// ---------------------------------------------------------------------------
// Test tube

ANIM(tube_idle, sheet_test_tube, 0.1, 0,0,0,1,2,3,0,0,0,2,0,0,1,0,0);

static void test_tube_init(entity_t *self) {
	self->anim = anim(&tube_idle);
	anim_goto_rand(&self->anim);
	self->sx = 8; self->sy = 10;
	self->check_against = GROUP_PLAYER;
}

static void test_tube_touch(entity_t *self, entity_t *other) {
	entity_kill(self);
	SOUND(SND_COLLECT);
	g.tubes_collected++;
}

// ---------------------------------------------------------------------------
// Trigger

static void trigger_settings(entity_t *self, const LevelEntity *def) {
	self->trigger.targets = game_targets(def);
	self->sx = def->sx;
	self->sy = def->sy;
	self->trigger.delay = def->a;
	self->trigger.can_fire = true;
	self->check_against |= def->checks;
}

static void trigger_update(entity_t *self) {
	self->trigger.delay_time -= TICK;
	if (self->trigger.delay_time < -30000) {
		self->trigger.delay_time = -30000;
	}
}

static void trigger_touch(entity_t *self, entity_t *other) {
	if (!self->trigger.can_fire || self->trigger.delay_time > 0) {
		return;
	}
	for (u16 i = 0; i < self->trigger.targets.len; i++) {
		entity_t *t = entity_by_ref(self->trigger.targets.e[i]);
		if (t) {
			entity_trigger(t, other);
		}
	}
	if (self->trigger.delay == -1) {
		self->trigger.can_fire = false;
	}
	else {
		self->trigger.delay_time = self->trigger.delay;
	}
}

// Fix up timers that would overflow s16 over long play sessions
static void timer_clamp_update(entity_t *self) {
	entity_base_update(self);
}

static void delay_update_clamped(entity_t *self) {
	delay_update(self);
	if (self->delay.delay_time < -30000) {
		self->delay.delay_time = -30000;
	}
}

static void no_update(entity_t *self) {}
#define no_draw entity_base_draw_none

// ---------------------------------------------------------------------------

void entities_register(void) {
	entity_vtab[ENTITY_TYPE_BLOB] = (entity_vtab_t){
		.init = blob_init, .update = blob_update, .damage = blob_damage, .touch = enemy_touch};
	entity_vtab[ENTITY_TYPE_CRATE] = (entity_vtab_t){
		.init = crate_init, .kill = crate_kill};
	entity_vtab[ENTITY_TYPE_DEBRIS] = (entity_vtab_t){
		.settings = debris_settings, .trigger = debris_trigger, .update = debris_update, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_DELAY] = (entity_vtab_t){
		.settings = delay_settings, .update = delay_update_clamped, .trigger = delay_trigger, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_DROPPER] = (entity_vtab_t){
		.init = dropper_init, .update = dropper_update, .damage = dropper_damage, .touch = enemy_touch};
	entity_vtab[ENTITY_TYPE_EARTHQUAKE] = (entity_vtab_t){
		.settings = earthquake_settings, .update = earthquake_update, .trigger = earthquake_trigger, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_END_HUB] = (entity_vtab_t){
		.init = end_hub_init, .trigger = end_hub_trigger, .update = end_hub_update};
	entity_vtab[ENTITY_TYPE_END_HUB_PLASMA] = (entity_vtab_t){
		.draw = end_hub_plasma_draw, .update = no_update};
	entity_vtab[ENTITY_TYPE_END_HUB_FADE] = (entity_vtab_t){
		.draw = end_hub_fade_draw, .update = no_update};
	entity_vtab[ENTITY_TYPE_GLASS_DOME] = (entity_vtab_t){
		.init = glass_dome_init, .damage = glass_dome_damage, .kill = glass_dome_kill};
	entity_vtab[ENTITY_TYPE_GRUNT] = (entity_vtab_t){
		.init = grunt_init, .update = grunt_update, .damage = grunt_damage, .touch = enemy_touch};
	entity_vtab[ENTITY_TYPE_HURT] = (entity_vtab_t){
		.settings = hurt_settings, .trigger = hurt_trigger, .update = no_update, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_LEVEL_CHANGE] = (entity_vtab_t){
		.settings = level_change_settings, .trigger = level_change_trigger, .update = no_update, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_MINE] = (entity_vtab_t){
		.init = mine_init, .kill = mine_kill, .touch = mine_touch, .update = no_update};
	entity_vtab[ENTITY_TYPE_MOVER] = (entity_vtab_t){
		.init = mover_init, .settings = mover_settings, .update = mover_update};
	entity_vtab[ENTITY_TYPE_PARTICLE] = (entity_vtab_t){
		.init = particle_init, .update = particle_update};
	entity_vtab[ENTITY_TYPE_PLAYER] = (entity_vtab_t){
		.init = player_init, .update = player_update, .damage = player_damage, .kill = player_kill};
	entity_vtab[ENTITY_TYPE_PROJECTILE] = (entity_vtab_t){
		.init = projectile_init, .update = projectile_update, .collide = projectile_collide, .touch = projectile_touch};
	entity_vtab[ENTITY_TYPE_RESPAWN_POD] = (entity_vtab_t){
		.init = respawn_pod_init, .update = respawn_pod_update, .touch = respawn_pod_touch, .message = respawn_pod_message};
	entity_vtab[ENTITY_TYPE_SPIKE] = (entity_vtab_t){
		.init = spike_init, .update = spike_update, .damage = spike_damage, .collide = spike_collide, .touch = enemy_touch};
	entity_vtab[ENTITY_TYPE_SPEWER] = (entity_vtab_t){
		.init = spewer_init, .update = spewer_update, .damage = spewer_damage, .touch = enemy_touch};
	entity_vtab[ENTITY_TYPE_SPEWER_SHOT] = (entity_vtab_t){
		.init = spewer_shot_init, .collide = spewer_shot_collide, .touch = spewer_shot_touch};
	entity_vtab[ENTITY_TYPE_TEST_TUBE] = (entity_vtab_t){
		.init = test_tube_init, .touch = test_tube_touch, .update = no_update};
	entity_vtab[ENTITY_TYPE_TRIGGER] = (entity_vtab_t){
		.settings = trigger_settings, .update = trigger_update, .touch = trigger_touch, .draw = no_draw};
	entity_vtab[ENTITY_TYPE_VOID] = (entity_vtab_t){
		.update = no_update, .draw = no_draw};
	(void)timer_clamp_update;
	entities_init();
}
