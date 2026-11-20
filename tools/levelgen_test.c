// Host test harness: dump generated levels for preview rendering.
// gcc -I src -I src/gen tools/levelgen_test.c src/levelgen.c src/gen/gfx.c -o levelgen_test
#include <stdio.h>
#include <stdlib.h>
#include "levelgen.h"

int main(int argc, char **argv) {
	u32 seed = argc > 1 ? strtoul(argv[1], 0, 10) : 1;
	u16 diff = argc > 2 ? atoi(argv[2]) : 0;
	const LevelDef *d = levelgen_generate(seed, diff);
	FILE *f = fopen(argc > 3 ? argv[3] : "level.bin", "wb");
	fwrite(&d->w, 2, 1, f);
	fwrite(&d->h, 2, 1, f);
	fwrite(d->main, 1, d->w * d->h, f);
	fwrite(d->coll, 1, d->w * d->h, f);
	fwrite(&d->nents, 2, 1, f);
	for (int i = 0; i < d->nents; i++) {
		const LevelEntity *e = &d->ents[i];
		short v[6] = {e->type, e->x, e->y, e->sx, e->sy, e->a};
		fwrite(v, 2, 6, f);
	}
	fclose(f);
	return 0;
}
