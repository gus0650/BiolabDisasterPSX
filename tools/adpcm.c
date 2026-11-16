// PS1 SPU ADPCM encoder: adpcm in.raw(s16 mono) out.bin
// Brute-force best filter/shift per 28-sample block, with error feedback.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const int F0[5] = {0, 60, 115, 98, 122};
static const int F1[5] = {0, 0, -52, -55, -60};

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// encode one block; returns squared error, writes nibbles and new history
static long long enc(const int16_t *s, int f, int sh, int *h1, int *h2, uint8_t *nib) {
	long long err = 0;
	int p1 = *h1, p2 = *h2;
	for (int i = 0; i < 28; i++) {
		int pred = (p1 * F0[f] + p2 * F1[f] + 32) >> 6;
		int d = s[i] - pred;
		// quantize: value = (n << 12) >> sh ; n in -8..7
		int q = (d * (1 << sh) + (d >= 0 ? 2048 : -2048)) / 4096;
		if (q > 7) q = 7;
		if (q < -8) q = -8;
		int dec = clamp16(((q << 12) >> sh) + pred);
		int e = dec - s[i];
		err += (long long)e * e;
		nib[i] = q & 15;
		p2 = p1;
		p1 = dec;
	}
	*h1 = p1;
	*h2 = p2;
	return err;
}

int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	long n = ftell(f) / 2;
	fseek(f, 0, SEEK_SET);
	long nb = (n + 27) / 28;
	int16_t *s = calloc(nb * 28 + 28, 2);
	fread(s, 2, n, f);
	fclose(f);
	FILE *o = fopen(argv[2], "wb");
	int h1 = 0, h2 = 0;
	for (long b = 0; b < nb; b++) {
		long long best = -1;
		int bf = 0, bs = 0;
		uint8_t nib[28], bn[28];
		for (int fi = 0; fi < 5; fi++) {
			for (int sh = 0; sh <= 12; sh++) {
				int a = h1, c = h2;
				long long e = enc(s + b * 28, fi, sh, &a, &c, nib);
				if (best < 0 || e < best) {
					best = e;
					bf = fi;
					bs = sh;
					memcpy(bn, nib, 28);
				}
			}
		}
		enc(s + b * 28, bf, bs, &h1, &h2, nib);
		uint8_t blk[16];
		blk[0] = (bf << 4) | bs;
		blk[1] = (b == 0) ? 0x04 : 0x00;
		if (b == nb - 1) {
			blk[1] |= 0x01; // end: stop the voice
		}
		for (int i = 0; i < 14; i++) {
			blk[2 + i] = nib[i * 2] | (nib[i * 2 + 1] << 4);
		}
		fwrite(blk, 1, 16, o);
	}
	// trailing silent block (end + mute), some hardware reads one block ahead
	uint8_t z[16] = {0, 0x01};
	fwrite(z, 1, 16, o);
	fclose(o);
	return 0;
}
