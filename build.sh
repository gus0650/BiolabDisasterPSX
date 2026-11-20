#!/bin/sh
# Builds biolab.exe and the disc image biolab.bin/.cue
set -e
cd "$(dirname "$0")"
PSN00BSDK_ROOT="${PSN00BSDK_ROOT:-/home/claude/psn00b}"
python3 tools/convert.py
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PSN00BSDK_ROOT"/lib/libpsn00b/cmake/sdk.cmake \
	-DPSN00BSDK_TARGET=mipsel-linux-gnu -DCMAKE_BUILD_TYPE=Release "$@" > /dev/null
cmake --build build > build/log.txt 2>&1 || { grep -E "error" build/log.txt; exit 1; }
grep -E "warning" build/log.txt || true
mkdir -p out
python3 tools/elf2psx.py build/biolab.elf out/BIOLAB.EXE
cd out && "$PSN00BSDK_ROOT"/bin/mkpsxiso -y iso.xml -o biolab.bin -c biolab.cue > /dev/null && cd ..
ls -la out/biolab.bin out/biolab.cue out/BIOLAB.EXE
