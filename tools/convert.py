#!/usr/bin/env python3
"""Asset converter: Biolab Disaster -> PlayStation (PSn00bSDK).

Generates src/gen/gfx.c + gfx.h:
  - 8bpp textures with 15-bit CLUTs, packed into VRAM texture pages
    (sprite sheets also get pre-flipped variants: H, V, HV)
  - semi-transparent pixels use the STP bit (blended 50/50 by the GPU),
    additive sprites (plasma) are premultiplied for B+F blending
  - font, title images, levels, sound effects (SPU ADPCM), sine table
"""
import json, os, math, subprocess, struct, wave, tempfile
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = os.path.join(ROOT, 'assets')
OUT = os.path.join(ROOT, 'src', 'gen')
os.makedirs(OUT, exist_ok=True)

def load(path):
	return np.array(Image.open(os.path.join(A, path)).convert('RGBA')).astype(np.int32)

def fmt(vals, per=16, f='%d'):
	return '\n'.join('\t' + ', '.join(f % v for v in vals[i:i + per]) + ',' for i in range(0, len(vals), per))

# ---------------------------------------------------------------------------
# Color conversion

def psx_color(r, g, b, stp):
	c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
	if stp:
		c |= 0x8000
	elif c == 0:
		c = 0x8000  # opaque black must not be 0 (transparent)
	return c

def image_to_indexed(img, mode=None):
	"""Returns (indices HxW, clut list of u16). Index 0 = transparent."""
	a = img[:, :, 3]
	rgb = img[:, :, :3]
	if mode == 'additive':
		# premultiply: additive blending B+F with F = color * alpha
		rgb = (rgb * a[:, :, None] / 255).astype(np.int32)
		opaque = rgb.max(axis=2) >= 8
		semi = np.zeros_like(opaque)
	else:
		opaque = a >= 160
		semi = (a >= 40) & (a < 160)
	vis = opaque | semi
	keys = {}
	idx = np.zeros(a.shape, dtype=np.int32)
	cols = []
	h, w = a.shape
	px = []
	for y in range(h):
		for x in range(w):
			if vis[y, x]:
				r, g, b = [int(v) for v in rgb[y, x]]
				s = 1 if (semi[y, x] or mode == 'additive') else 0
				px.append(((r >> 3, g >> 3, b >> 3, s), y, x))
	uniq = sorted(set(k for k, _, _ in px))
	if len(uniq) > 255:
		# quantize the visible pixels down to 255 colors
		arr = np.array([[k[0] * 8, k[1] * 8, k[2] * 8] for k, _, _ in px], dtype=np.uint8).reshape(1, -1, 3)
		q = Image.fromarray(arr, 'RGB').quantize(255, method=Image.Quantize.MEDIANCUT)
		pal = q.getpalette()[:255 * 3]
		qi = np.array(q).ravel()
		mapping = {}
		new = []
		for i, (k, y, x) in enumerate(px):
			c = qi[i]
			key = (pal[c * 3] >> 3, pal[c * 3 + 1] >> 3, pal[c * 3 + 2] >> 3, k[3])
			new.append((key, y, x))
		px = new
		uniq = sorted(set(k for k, _, _ in px))[:255]
	lut = {k: i + 1 for i, k in enumerate(uniq)}
	for k, y, x in px:
		idx[y, x] = lut.get(k, 1)
	clut = [0] + [psx_color(k[0] << 3, k[1] << 3, k[2] << 3, k[3]) for k in uniq]
	clut += [0] * (256 - len(clut))
	return idx, clut

# ---------------------------------------------------------------------------
# VRAM packing: framebuffers at (0,0) and (0,240) 320x240 each.
# 8bpp texture pages: 256x256 texels = 128 halfwords; page x at 320+128*k.
# CLUTs: 256 halfwords wide rows at x=0, y=480..511.

class Packer:
	def __init__(self):
		self.pages = []  # (vram_x, vram_y, shelves)
		for py in (0, 256):
			for k in range(5):
				self.pages.append({'x': 320 + 128 * k, 'y': py, 'shelf_y': 0, 'shelf_h': 0, 'cur_x': 0})
		self.clut_row = 0

	def place(self, w, h):
		for p in self.pages:
			if p['cur_x'] + w <= 256 and p['shelf_y'] + h <= 256:
				x, y = p['cur_x'], p['shelf_y']
				p['cur_x'] += w + (w & 1)
				p['shelf_h'] = max(p['shelf_h'], h)
				return p, x, y
			if p['shelf_y'] + p['shelf_h'] + h <= 256 and w <= 256:
				p['shelf_y'] += p['shelf_h']
				p['shelf_h'] = h
				p['cur_x'] = w + (w & 1)
				return p, 0, p['shelf_y']
		raise Exception('VRAM full')

	def clut(self):
		r = self.clut_row
		self.clut_row += 1
		assert r < 32
		return 0, 480 + r

uploads = []  # (x, y, w_halfwords, h, name)
c_lines = ['#include "gfx.h"', '']
h_lines = ['#ifndef GFX_H', '#define GFX_H', '#include "types.h"', '',
	'typedef struct { u16 px, py; u8 u, v; u16 cx, cy; } TexRef; // page vram pos, uv in page, clut pos',
	'typedef struct { u16 x, y, w, h; const u16 *data; } VramUpload;',
	'typedef struct { u8 w, h; u8 cols, rows; u8 blend; u8 _pad; u16 nframes; TexRef var[4]; } SprSheet;',
]

packer = Packer()

def add_texture(name, idx, clut, clut_pos=None):
	h, w = idx.shape
	wp = w + (w & 1)
	pad = np.zeros((h, wp), dtype=np.int32)
	pad[:, :w] = idx
	page, u, v = packer.place(wp, h)
	vx = page['x'] + u // 2
	vy = page['y'] + v
	data = []
	for y in range(h):
		for x in range(0, wp, 2):
			data.append(int(pad[y, x]) | (int(pad[y, x + 1]) << 8))
	c_lines.append('static const u16 tex_%s[%d] = {' % (name, len(data)))
	c_lines.append(fmt(data, 16, '0x%04X'))
	c_lines.append('};')
	uploads.append((vx, vy, wp // 2, h, 'tex_' + name))
	if clut_pos:
		cx, cy = clut_pos
	else:
		cx, cy = packer.clut()
		c_lines.append('static const u16 clut_%s[256] = {' % name)
		c_lines.append(fmt(clut, 16, '0x%04X'))
		c_lines.append('};')
		uploads.append((cx, cy, 256, 1, 'clut_' + name))
	return (page['x'], page['y'], u, v, cx, cy)

# ---------------------------------------------------------------------------

SHEETS = [
	('player',        'sprites/player.png',          16, 16),
	('player_gib',    'sprites/player.png',           8,  8),
	('projectile',    'sprites/projectile.png',       8,  8),
	('blob',          'sprites/blob.png',            16, 16),
	('blob_gib',      'sprites/blob-gibs.png',        4,  4),
	('crate',         'sprites/crate.png',            8,  8),
	('crate_debris',  'sprites/crate.png',            4,  4),
	('debris',        'sprites/debris.png',           4,  4),
	('dropper',       'sprites/dropper.png',         16,  8),
	('dropper_shot',  'sprites/dropper.png',          8,  8),
	('endhub',        'sprites/endhub.png',          24, 24),
	('plasma',        'sprites/plasma.png',          20, 20),
	('glass_dome',    'sprites/glass-dome.png',      40, 32),
	('glass_shards',  'sprites/glass-shards.png',     4,  4),
	('grunt',         'sprites/grunt.png',           16, 16),
	('grunt_gib',     'sprites/grunt.png',            4,  4),
	('grunt_gun',     'sprites/grunt.png',            8,  8),
	('grunt_shot',    'sprites/grunt-projectile.png', 8,  8),
	('mine',          'sprites/mine.png',            16,  8),
	('mine_gib',      'sprites/mine.png',             4,  4),
	('elevator',      'sprites/elevator.png',        24,  8),
	('respawn_pod',   'sprites/respawn-pod.png',     18, 16),
	('spewer',        'sprites/spewer.png',          16,  8),
	('spewer_gib',    'sprites/spewer.png',           4,  4),
	('spike',         'sprites/spike.png',           16, 16),
	('spike_small',   'sprites/spike.png',            8,  8),
	('test_tube',     'sprites/test-tube.png',        8, 10),
]
BLEND = {'sprites/plasma.png': 2, 'sprites/glass-dome.png': 1, 'sprites/glass-shards.png': 1, 'sprites/mine.png': 1}

def gen_sprites():
	files = {}
	for name, f, fw, fh in SHEETS:
		files.setdefault(f, [])
	refs = {}
	for f in files:
		img = load(f)
		mode = 'additive' if BLEND.get(f) == 2 else None
		idx, clut = image_to_indexed(img, mode)
		base = os.path.splitext(os.path.basename(f))[0].replace('-', '_')
		variants = [idx, idx[:, ::-1], idx[::-1, :], idx[::-1, ::-1]]
		r0 = add_texture('%s_0' % base, variants[0], clut)
		refs[f] = [r0] + [add_texture('%s_%d' % (base, i), v, clut, (r0[4], r0[5])) for i, v in enumerate(variants) if i > 0]
	for name, f, fw, fh in SHEETS:
		img = load(f)
		cols = img.shape[1] // fw
		rows = img.shape[0] // fh
		vs = ', '.join('{%d, %d, %d, %d, %d, %d}' % r for r in refs[f])
		c_lines.append('const SprSheet sheet_%s = {%d, %d, %d, %d, %d, 0, %d, {%s}};' % (
			name, fw, fh, cols, rows, BLEND.get(f, 0), cols * rows, vs))
		h_lines.append('extern const SprSheet sheet_%s;' % name)

def gen_tileset():
	img = load('tiles/biolab.png')
	idx, clut = image_to_indexed(img)
	r = add_texture('tileset', idx, clut)
	c_lines.append('const TexRef tex_tileset_ref = {%d, %d, %d, %d, %d, %d};' % r)
	h_lines.append('extern const TexRef tex_tileset_ref;')
	h_lines.append('#define TILESET_COLS %d' % (img.shape[1] // 8))
	h_lines.append('#define TILESET_COUNT %d' % ((img.shape[1] // 8) * (img.shape[0] // 8)))

def gen_images():
	for name, f in [('title_biolab', 'title-biolab.png'), ('title_disaster', 'title-disaster.png'), ('title_player', 'title-player.png')]:
		img = load(f)
		idx, clut = image_to_indexed(img)
		r = add_texture(name, idx, clut)
		c_lines.append('const TexRef img_%s = {%d, %d, %d, %d, %d, %d};' % ((name,) + r))
		h_lines.append('extern const TexRef img_%s;' % name)
		h_lines.append('#define %s_W %d' % (name.upper(), img.shape[1]))
		h_lines.append('#define %s_H %d' % (name.upper(), img.shape[0]))

def gen_font():
	img = load('font_04b03.png')
	# opaque where alpha >= 128
	img2 = img.copy()
	img2[:, :, 3] = np.where(img[:, :, 3] >= 128, 255, 0)
	idx, clut = image_to_indexed(img2)
	r = add_texture('font', idx, clut)
	c_lines.append('const TexRef tex_font_ref = {%d, %d, %d, %d, %d, %d};' % r)
	h_lines.append('extern const TexRef tex_font_ref;')
	meta = json.load(open(os.path.join(A, 'font_04b03.json')))
	met = meta['metrics']
	n = meta['last_char'] - meta['first_char']
	h_lines.append('typedef struct { u8 x, y, w, h; s8 ox, oy; u8 advance; u8 _pad; } FontGlyph;')
	h_lines.append('#define FONT_FIRST_CHAR %d' % meta['first_char'])
	h_lines.append('#define FONT_LAST_CHAR %d' % meta['last_char'])
	h_lines.append('#define FONT_LINE_HEIGHT %d' % meta['height'])
	h_lines.append('extern const FontGlyph font_glyphs[%d];' % n)
	c_lines.append('const FontGlyph font_glyphs[%d] = {' % n)
	for i in range(n):
		c_lines.append('\t{%d, %d, %d, %d, %d, %d, %d, 0},' % tuple(met[i * 7:i * 7 + 7]))
	c_lines.append('};')

# ---------------------------------------------------------------------------
# Levels (same layout as the Mega Drive port; plane entries are tile+1)

ENTITY_TYPES = ['blob', 'crate', 'debris', 'delay', 'dropper', 'earthquake', 'end_hub', 'end_hub_plasma',
	'end_hub_fade', 'glass_dome', 'grunt', 'hurt', 'level_change', 'mine', 'mover', 'particle', 'player',
	'projectile', 'respawn_pod', 'spike', 'spewer', 'spewer_shot', 'test_tube', 'trigger', 'void']
LEVELS = ['biolab-1.json', 'biolab-2.json', 'biolab-3.json']
CHECKS = {'player': 1, 'npc': 2, 'enemy': 4, 'item': 8, 'projectile': 16, 'breakable': 64}

def T(s):
	return int(round(s * 600))

def gen_levels():
	h_lines.append('typedef struct { u8 type; u8 ntargets; s16 x, y; s16 sx, sy; s16 a, b; u8 checks; u8 has_size; u8 targets[6]; } LevelEntity;')
	h_lines.append('typedef struct { u16 w, h; u16 bw, bh; const u8 *main; const u8 *bg; const u8 *coll; u16 nents; const LevelEntity *ents; } LevelDef;')
	h_lines.append('#define LEVEL_COUNT %d' % len(LEVELS))
	h_lines.append('extern const LevelDef *const levels[%d];' % len(LEVELS))
	for li, lf in enumerate(LEVELS):
		d = json.load(open(os.path.join(A, 'levels', lf)))
		maps = {m['name']: m for m in d['maps']}
		mm, bg, col = maps['main'], maps['background'], maps['collision']
		for nm, m in (('main', mm), ('bg', bg)):
			vals = [t for row in m['data'] for t in row]
			assert max(vals) < 256
			c_lines.append('static const u8 l%d_%s[%d] = {' % (li, nm, len(vals)))
			c_lines.append(fmt(vals, 32))
			c_lines.append('};')
		cd = [1 if t == 1 else 0 for row in col['data'] for t in row]
		c_lines.append('static const u8 l%d_coll[%d] = {' % (li, len(cd)))
		c_lines.append(fmt(cd, 32))
		c_lines.append('};')
		ents = d['entities']
		names = {}
		for i, e in enumerate(ents):
			s = e.get('settings') or {}
			if isinstance(s, dict) and 'name' in s:
				names.setdefault(s['name'], i)
		rows = []
		for e in ents:
			t = e['type']
			s = e.get('settings') or {}
			if not isinstance(s, dict):
				s = {}
			tg = [names[s['target'][k]] for k in sorted(s.get('target', {}).keys(), key=int) if s['target'][k] in names]
			size = s.get('size')
			sx = int((size or {}).get('x', 0)); sy = int((size or {}).get('y', 0))
			a = b = 0
			checks = 0
			if t == 'debris':
				a = int(s.get('count', 0)); b = T(s.get('duration', 0))
			elif t == 'delay':
				a = T(s.get('delay', 0))
			elif t == 'earthquake':
				a = int(round(s.get('strength', 0) * 256)); b = T(s.get('duration', 0))
			elif t == 'hurt':
				a = int(s.get('damage', 0)) or 10
			elif t == 'level_change':
				a = LEVELS.index(s['level'].split('/')[-1])
			elif t == 'mover':
				a = int(s.get('speed', 0)) or 20
			elif t == 'trigger':
				a = T(s['delay']) if 'delay' in s else -1
				if 'checks' in s:
					for k, v in CHECKS.items():
						if k in s['checks']:
							checks |= v
				else:
					checks = 1
			rows.append('\t{%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, {%s}},' % (
				ENTITY_TYPES.index(t) + 1, len(tg), int(e['x']), int(e['y']), sx, sy, a, b, checks,
				1 if size is not None else 0, ', '.join(str(x) for x in (tg + [0] * (6 - len(tg))))))
		c_lines.append('static const LevelEntity l%d_ents[%d] = {' % (li, len(ents)))
		c_lines.extend(rows)
		c_lines.append('};')
		c_lines.append('static const LevelDef level%d = {%d, %d, %d, %d, l%d_main, l%d_bg, l%d_coll, %d, l%d_ents};' % (
			li, mm['width'], mm['height'], bg['width'], bg['height'], li, li, li, len(ents), li))
	c_lines.append('const LevelDef *const levels[%d] = {%s};' % (len(LEVELS), ', '.join('&level%d' % i for i in range(len(LEVELS)))))

# ---------------------------------------------------------------------------
# Sound effects -> SPU ADPCM

SOUNDS = [
	('collect', 22050), ('crack', 22050), ('die', 22050), ('drygib', 22050), ('earthquake', 11025),
	('glass_impact', 22050), ('glass_shatter', 22050), ('grunt_plasma', 22050), ('intro', 11025),
	('mine', 22050), ('plasma', 22050), ('respawn', 22050), ('theend', 11025), ('wetgib', 22050),
]
SOUND_FILES = {'die': 'die-respawn', 'respawn': 'respawn-activate', 'glass_impact': 'glass-impact',
	'glass_shatter': 'glass-shatter', 'grunt_plasma': 'grunt-plasma'}

def gen_sounds():
	from scipy.signal import resample_poly
	total = 0
	h_lines.append('typedef struct { const u8 *data; u32 size; u16 rate; } SoundDef;')
	h_lines.append('enum {')
	for name, rate in SOUNDS:
		h_lines.append('\tSND_%s,' % name.upper())
	h_lines.append('\tSND_COUNT\n};')
	h_lines.append('extern const SoundDef sounds[SND_COUNT];')
	defs = []
	for name, rate in SOUNDS:
		f = SOUND_FILES.get(name, name)
		w = wave.open(os.path.join(A, 'sounds', f + '.wav'))
		sr = w.getframerate()
		a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, w.getnchannels()).astype(np.float64).mean(1)
		g = math.gcd(rate, sr)
		a = resample_poly(a, rate // g, sr // g)
		a = np.clip(a, -32768, 32767).astype(np.int16)
		with tempfile.TemporaryDirectory() as td:
			raw = os.path.join(td, 'in.raw'); out = os.path.join(td, 'out.bin')
			a.tofile(raw)
			subprocess.check_call([os.path.join(ROOT, 'tools', 'adpcm'), raw, out])
			data = open(out, 'rb').read()
		total += len(data)
		c_lines.append('static const u8 snd_%s[%d] __attribute__((aligned(4))) = {' % (name, len(data)))
		c_lines.append(fmt(list(data), 32, '0x%02X'))
		c_lines.append('};')
		defs.append('\t{snd_%s, %d, %d},' % (name, len(data), rate))
	c_lines.append('const SoundDef sounds[SND_COUNT] = {')
	c_lines.extend(defs)
	c_lines.append('};')
	print('  SPU sound data: %d bytes' % total)
	assert total < 512 * 1024 - 0x1010 - 64

# ---------------------------------------------------------------------------
# Tile model for the random level generator: which main-layer tile the
# original levels use for a given 3x3 solid/empty pattern, given the tiles
# already placed to the left and above (with back-off to smaller contexts).

def gen_tile_model():
	import collections
	GRASS = set(range(81, 97))  # animated grass/slime, only used in level 3
	T3 = collections.defaultdict(collections.Counter)
	T2 = collections.defaultdict(collections.Counter)
	T2u = collections.defaultdict(collections.Counter)
	T1 = collections.defaultdict(collections.Counter)
	for lf in LEVELS:
		d = json.load(open(os.path.join(A, 'levels', lf)))
		maps = {m['name']: m for m in d['maps']}
		main = maps['main']['data']
		col = maps['collision']['data']
		h, w = len(col), len(col[0])
		def S(x, y):
			return 1 if (x < 0 or y < 0 or x >= w or y >= h) else (1 if col[y][x] == 1 else 0)
		for y in range(h):
			for x in range(w):
				k = 0
				for dy in (-1, 0, 1):
					for dx in (-1, 0, 1):
						k = (k << 1) | S(x + dx, y + dy)
				t = main[y][x]
				if t in GRASS:
					continue
				left = main[y][x - 1] if x > 0 else 255
				up = main[y - 1][x] if y > 0 else 255
				if left in GRASS: left = 0
				if up in GRASS: up = 0
				T3[(k << 16) | (left << 8) | up][t] += 1
				T2[(k << 16) | (left << 8) | 254][t] += 1
				T2u[(k << 16) | (253 << 8) | up][t] += 1
				T1[(k << 16) | (252 << 8) | 252][t] += 1
	keys = []
	items = []
	for tab in (T3, T2, T2u, T1):
		for key in tab:
			keys.append((key, len(items), len(tab[key])))
			for t, c in tab[key].most_common():
				items.append((t, min(c, 65535)))
	keys.sort()
	h_lines.append('typedef struct { u32 key; u16 first, count; } TileCtx;')
	h_lines.append('typedef struct { u8 tile; u8 _pad; u16 weight; } TileChoice;')
	h_lines.append('#define TILE_CTX_COUNT %d' % len(keys))
	h_lines.append('extern const TileCtx tile_ctx[%d];' % len(keys))
	h_lines.append('extern const TileChoice tile_choice[%d];' % len(items))
	c_lines.append('const TileCtx tile_ctx[%d] = {' % len(keys))
	c_lines.extend('\t{0x%08X, %d, %d},' % k for k in keys)
	c_lines.append('};')
	c_lines.append('const TileChoice tile_choice[%d] = {' % len(items))
	c_lines.append(fmt(['{%d, 0, %d}' % it for it in items], 12, '%s'))
	c_lines.append('};')
	print('  tile model: %d contexts, %d choices' % (len(keys), len(items)))

def gen_trig():
	vals = [int(round(math.sin(i * 2 * math.pi / 256) * 16384)) for i in range(256)]
	c_lines.append('const s16 sin_table[256] = {')
	c_lines.append(fmt(vals))
	c_lines.append('};')
	h_lines.append('extern const s16 sin_table[256];')

def main():
	gen_tileset()
	gen_sprites()
	gen_images()
	gen_font()
	c_lines.append('const VramUpload vram_uploads[%d] = {' % len(uploads))
	for (x, y, w, h, n) in uploads:
		c_lines.append('\t{%d, %d, %d, %d, %s},' % (x, y, w, h, n))
	c_lines.append('};')
	h_lines.append('#define VRAM_UPLOAD_COUNT %d' % len(uploads))
	h_lines.append('extern const VramUpload vram_uploads[%d];' % len(uploads))
	gen_levels()
	gen_tile_model()
	gen_sounds()
	gen_trig()
	h_lines.append('#endif')
	open(os.path.join(OUT, 'gfx.c'), 'w').write('\n'.join(c_lines) + '\n')
	open(os.path.join(OUT, 'gfx.h'), 'w').write('\n'.join(h_lines) + '\n')
	print('  textures:', len(uploads) // 2, 'clut rows used:', packer.clut_row)

if __name__ == '__main__':
	main()
