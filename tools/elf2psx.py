#!/usr/bin/env python3
"""Convert a MIPS ELF into a PlayStation PS-X EXE (only loadable sections)."""
import struct, subprocess, sys, tempfile, os

def main(elf, out):
	# loadable sections in the KSEG0 address range
	hdr = subprocess.check_output(['mipsel-linux-gnu-readelf', '-SW', elf]).decode()
	secs = []
	for line in hdr.splitlines():
		line = line.strip()
		if not line.startswith('['):
			continue
		parts = line[line.index(']') + 1:].split()
		if len(parts) < 6 or parts[0] == 'Name':
			continue
		try:
			name, typ, addr, off, size = parts[0], parts[1], int(parts[2], 16), int(parts[3], 16), int(parts[4], 16)
		except ValueError:
			continue
		if typ == 'PROGBITS' and addr >= 0x80000000 and size:
			secs.append((name, addr, size))
	start = min(a for _, a, _ in secs)
	end = max(a + s for _, a, s in secs)
	with tempfile.TemporaryDirectory() as td:
		raw = os.path.join(td, 'raw.bin')
		cmd = ['mipsel-linux-gnu-objcopy', '-O', 'binary']
		for n, _, _ in secs:
			cmd += ['-j', n]
		subprocess.check_call(cmd + [elf, raw])
		data = open(raw, 'rb').read()
	assert len(data) == end - start, (len(data), end - start)
	data += b'\0' * ((-len(data)) % 2048)
	syms = subprocess.check_output(['mipsel-linux-gnu-nm', elf]).decode()
	entry = int(subprocess.check_output(['mipsel-linux-gnu-readelf', '-h', elf]).decode().split('Entry point address:')[1].split()[0], 16)
	gp = 0
	for l in syms.splitlines():
		p = l.split()
		if len(p) == 3 and p[2] == '_gp':
			gp = int(p[0], 16)
	h = bytearray(2048)
	h[0:8] = b'PS-X EXE'
	struct.pack_into('<IIIIIIIIIIII', h, 0x10, entry, gp, start, len(data), 0, 0, 0, 0, 0x801FFFF0, 0, 0, 0)
	h[0x4C:0x4C + 46] = b'Sony Computer Entertainment Inc. for Europe area'[:46]
	open(out, 'wb').write(bytes(h) + data)
	print('%s: entry %08X load %08X size %d gp %08X' % (out, entry, start, len(data), gp))

if __name__ == '__main__':
	main(sys.argv[1], sys.argv[2])
