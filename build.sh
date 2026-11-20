#!/bin/sh
# Builds biolab.exe and the disc image biolab.bin/.cue
set -e
cd "$(dirname "$0")"
PSN00BSDK_ROOT="${PSN00BSDK_ROOT:-/home/claude/psn00b}"
python3 tools/convert.py
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PSN00BSDK_ROOT"/lib/libpsn00b/cmake/sdk.cmake \
	-DPSN00BSDK_TARGET=mipsel-linux-gnu -DCMAKE_BUILD_TYPE=Release "$@" > /dev/null
# The SDK runs elf2x after linking; it crashes with Ubuntu's mipsel toolchain
# ("buffer overflow detected"). That step is not needed (tools/elf2psx.py replaces
# it), so only real compile/link errors or a missing ELF count as a failure.
rm -f build/biolab.elf
cmake --build build > build/log.txt 2>&1 || true
if grep -qE "error:|undefined reference" build/log.txt || [ ! -f build/biolab.elf ]; then
	grep -E "error|undefined reference|FAILED" build/log.txt; exit 1
fi
grep -E "warning" build/log.txt || true
mkdir -p out
python3 tools/elf2psx.py build/biolab.elf out/BIOLAB.EXE
cd out && "$PSN00BSDK_ROOT"/bin/mkpsxiso -y iso.xml -o biolab.bin -c biolab.cue > /dev/null && cd ..
ls -la out/biolab.bin out/biolab.cue out/BIOLAB.EXE
