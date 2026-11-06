#!/usr/bin/env bash
# Build the Classic application. See docs/building-mac-os9.md.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build-mac -DET_TARGET=mac -DCMAKE_TOOLCHAIN_FILE=cmake/Retro68.toolchain.cmake "$@" >/dev/null
cmake --build build-mac -j"$(nproc)" --target ET_APPL
head -c 8 build-mac/ET.pef | grep -aq 'Joy!peff' || { echo "ET.pef is not a PEF" >&2; exit 1; }
python3 mac_app.py verify --creator WfET --pef build-mac/ET.pef build-mac/ET.bin build-mac/ET.dsk build-mac/%ET.ad
ls -l build-mac/ET.pef build-mac/ET.bin
