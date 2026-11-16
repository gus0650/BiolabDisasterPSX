# Biolab Disaster – PlayStation port

Port of phoboslab's *Biolab Disaster* to the Sony PlayStation (PS1), built with PSn00bSDK.

## Build outputs
- `biolab.bin` + `biolab.cue`: bootable disc image (game + the original music as a CD audio track)
- `BIOLAB.EXE`: the program alone, for emulators that load PS-EXE files directly (no music, because it needs the CD)

## Controls
- D-pad: move
- Cross (or Up): jump (hold to jump higher)
- Square or Circle: shoot
- START: start / continue

## Notes
- Original graphics in full color (one 256-color palette per sprite sheet). The glass dome uses real translucency, and the ending plasma uses additive blending with all 100 sprites.
- Music: the original `biochemie.wav` at full CD quality, looped by the CD drive.
- Sound effects: SPU ADPCM, 22 kHz (11 kHz for the long ones).
- Same fixed-point game engine as the Mega Drive port, at 320x240 and a full 60 fps.
- The disc has no Sony license data. Real consoles need a modchip or softmod (e.g. FreePSXBoot, Unirom); emulators boot it directly.

## Building
The game assets (graphics, sounds, music) are not part of this repository: they belong to their authors and have no license. `src/gen/` is generated from them at build time.
Needs PSn00bSDK, a mipsel GCC, mkpsxiso, Python 3 (numpy, Pillow, scipy).
1. Copy the `assets/` folder from https://github.com/phoboslab/high_biolab into this folder.
2. Build the ADPCM encoder: `gcc -O2 -o tools/adpcm tools/adpcm.c`
3. `./build.sh` runs `tools/convert.py` (generates `src/gen/`) and builds `out/BIOLAB.EXE` and the disc image `out/biolab.bin/.cue` (adjust the SDK paths in it). `tools/elf2psx.py` replaces the SDK's elf2x, which fails with Ubuntu's mipsel toolchain.
