#!/usr/bin/env bash
# Build the Classic application, and with "package" the files that install it.
# --bringup builds at -O0, as Quake III's does, and with "package" adds the stage 1 files.
# See docs/building-mac-os9.md. Usage: ./build_mac.sh [package] [--bringup] [cmake options], in any order
set -euo pipefail
cd "$(dirname "$0")"
PACKAGE=0
BRINGUP=
OPT=-O2
USAGE="Usage: ./build_mac.sh [package] [--bringup] [cmake options]"
CMAKE_OPTIONS=()
for option in "$@"; do
    case "$option" in
        package)
            [ "$PACKAGE" = 0 ] || { echo "build_mac.sh: package given twice. $USAGE" >&2; exit 1; }
            PACKAGE=1 ;;
        --bringup)
            [ -z "$BRINGUP" ] || { echo "build_mac.sh: --bringup given twice. $USAGE" >&2; exit 1; }
            BRINGUP=--bringup
            OPT=-O0 ;;
        -*) CMAKE_OPTIONS+=("$option") ;;
        # cmake takes any other word as a path, ignores it and succeeds
        *) echo "build_mac.sh: $option is not a cmake option. $USAGE" >&2; exit 1 ;;
    esac
done
if [ "$PACKAGE" = 1 ]; then
    # refuse before a long build, not after it
    scripts/package_mac.sh --check
fi
# set on every run, or the cache would keep the last one; an -DET_OPT among the options wins
cmake -S . -B build-mac -DET_TARGET=mac -DCMAKE_TOOLCHAIN_FILE=cmake/Retro68.toolchain.cmake -DET_OPT="$OPT" ${CMAKE_OPTIONS[@]+"${CMAKE_OPTIONS[@]}"} >/dev/null
cmake --build build-mac -j"$(nproc)" --target ET_APPL ET_INSTALLER_APPL
head -c 8 build-mac/ET.pef | grep -aq 'Joy!peff' || { echo "ET.pef is not a PEF" >&2; exit 1; }
python3 -B mac_app.py verify --creator WfET --pef build-mac/ET.pef build-mac/ET.bin build-mac/%ET.ad
# the manifest must describe the PEF that was just built
recorded=$(sed -n 's/^pef_sha256=//p' build-mac/ET.manifest.txt)
actual=$(sha256sum build-mac/ET.pef | cut -d' ' -f1)
if [ "$recorded" != "$actual" ]; then
    echo "ET.manifest.txt records pef_sha256=$recorded, but ET.pef is $actual" >&2
    exit 1
fi
ls -l build-mac/ET.pef build-mac/ET.bin
sed -n -E 's/^(pef_sha256|gcc_version|retro68_commit|retro68_pinned)=/  &/p' build-mac/ET.manifest.txt
if [ "$PACKAGE" = 1 ]; then
    scripts/package_mac.sh --from-build $BRINGUP
fi
