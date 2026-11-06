#!/usr/bin/env bash
# Vendor the GPL `legacy` mod sources from an ET: Legacy checkout at a release tag.
# usage: scripts/sync_legacy.sh [etlegacy-checkout] [tag]
# A client's built-in cgame must match the server's mod release, so each ETL
# release is a re-run of this script, then scripts/gen_static_renames.py.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-$HOME/etlegacy}
TAG=${2:-v2.86.2}
LIBS=${3:-$HOME/etlegacy-libs}   # clone of etlegacy-libs; cJSON comes from the commit the tag pins
[ "$(git -C "$SRC" describe --tags --exact-match HEAD)" = "$TAG" ] || { echo "$SRC is not at $TAG" >&2; exit 1; }
[ -z "$(git -C "$SRC" status --porcelain -- src etmain/ui vendor/sha-1)" ] || { echo "$SRC is dirty" >&2; exit 1; }
OUT=$ROOT/legacy
rm -rf "$OUT/src" "$OUT/etmain" "$OUT/vendor"
mkdir -p "$OUT/src/qcommon" "$OUT/src/renderercommon" "$OUT/etmain/ui" "$OUT/vendor/sha-1"
cp -r "$SRC/src/game" "$SRC/src/cgame" "$SRC/src/ui" "$OUT/src/"
for f in i18n.c q_math.c q_math.h q_oss.h q_platform.h q_primitives.h q_shared.c q_shared.h q_unicode.c q_unicode.h \
         keycodes.h version.c version.h vm_ext.h; do cp "$SRC/src/qcommon/$f" "$OUT/src/qcommon/"; done
cp "$SRC/src/renderercommon/tr_types.h" "$OUT/src/renderercommon/"
cp "$SRC/etmain/ui/menudef.h" "$OUT/etmain/ui/"
cp "$SRC"/vendor/sha-1/sha1.[ch] "$OUT/vendor/sha-1/"
cp "$SRC/COPYING.txt" "$OUT/"
mkdir -p "$OUT/src/qcommon/json" "$OUT/vendor/cjson"
cp "$SRC"/src/qcommon/json/json.[ch] "$OUT/src/qcommon/json/"
LIBS_COMMIT=$(git -C "$SRC" ls-tree HEAD libs | awk '{print $3}')
for f in cJSON.c cJSON.h LICENSE; do git -C "$LIBS" show "$LIBS_COMMIT:cjson/$f" > "$OUT/vendor/cjson/$f"; done
# The one local change to the mod. Each slot is 1.1 MB in both cgame and qagame, a
# Mac has a fixed partition and needs its RAM for GL's texture copies, and every
# stock character shares a single slot. Too few is a clean error, and it is local
# to each module, never on the wire.
sed -i 's/^#define MAX_ANIMSCRIPT_MODELS               32$/#define MAX_ANIMSCRIPT_MODELS               8/' "$OUT/src/game/bg_public.h"
grep -q 'MAX_ANIMSCRIPT_MODELS               8$' "$OUT/src/game/bg_public.h"

printf 'tag=%s\ncommit=%s\nlibs=%s\n' "$TAG" "$(git -C "$SRC" rev-parse HEAD)" "$LIBS_COMMIT" > "$OUT/VERSION.vendored"
