#include "game.h"

int main(int argc, const char **argv) {
	entities_register();
	g.player = entity_ref_none();
	engine_run(&scene_title);
	return 0;
}
