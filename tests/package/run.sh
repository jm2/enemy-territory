#!/usr/bin/env bash
# package_mac.sh must refuse missing or wrong game data before a long build starts.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
refuses() { # what, text to expect
    local out
    if out=$(ET_DATA_DIR=$T/data "$ROOT/scripts/package_mac.sh" --check 2>&1); then
        echo "FAIL: $1 was accepted"; fails=1
    elif ! grep -q "$2" <<<"$out"; then
        echo "FAIL: $1: $out"; fails=1
    fi
}
mkdir -p "$T/data/etmain" "$T/data/legacy"
refuses "an empty folder" "pak0.pk3 is missing"
# a demo pak has the right name and the wrong bytes
echo demo > "$T/data/etmain/pak0.pk3"
refuses "another pak0" "not the expected file"
# partition arithmetic against the sources
python3 "$ROOT/cmake/mac_partition.py" host --root "$ROOT" >/dev/null || { echo "FAIL: partition"; fails=1; }
sed 's/#define DEF_COMHUNKMEGS "72"/#define DEF_COMHUNKMEGS "120"/' "$ROOT/src/qcommon/common.c" > "$T/common.c"
mkdir -p "$T/tree/src/qcommon" "$T/tree/src/client" "$T/tree/src/mac9"
cp "$T/common.c" "$T/tree/src/qcommon/common.c"
cp "$ROOT/src/client/snd_mem.c" "$ROOT/src/client/snd_local.h" "$T/tree/src/client/"
cp "$ROOT/src/mac9/mac_resources.r" "$T/tree/src/mac9/"
if python3 "$ROOT/cmake/mac_partition.py" host --root "$T/tree" >/dev/null 2>&1; then
    echo "FAIL: a 120 MB hunk fits a partition sized for 72"; fails=1
fi
# the installer's parser, and a whole payload written then read back
gcc -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wno-unused-function -I "$ROOT/src/mac9" \
    "$ROOT/tests/package/test_installer_format.c" -o "$T/format" && "$T/format" || { echo "FAIL: installer format"; fails=1; }
python3 -I -B "$ROOT/tests/package/test_installer_pack.py" "$ROOT" "$T" || { echo "FAIL: installer pack"; fails=1; }
# the Mac's parser must read what the packer really writes, header after the PEF
"$T/format" "$T/pack/out" 4 || { echo "FAIL: parser against packer"; fails=1; }
# the install image Disk Copy mounts
python3 -B "$ROOT/tests/package/test_ndif_image.py" 2>&1 | tail -1 | grep -q '^OK' || { echo "FAIL: ndif image"; fails=1; }
[ "$fails" = 0 ] && echo "package checks passed"
exit "$fails"
