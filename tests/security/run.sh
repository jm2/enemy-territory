#!/usr/bin/env bash
# Host sanitizer tests of the code that parses what a peer sends.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
FLAGS="-std=gnu99 -fgnu89-inline -fcommon -fpermissive -fsigned-char -g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -DC_ONLY -DBOTLIB"
cd "$ROOT"
for sep in "'/'" "':'"; do
    "$CC" $FLAGS "-DET_PATH_SEP=$sep" tests/security/test_path.c src/qcommon/fs_path.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/path"
    "$OUT/path"
done
# FS_VerifyOfficialPaks alone: its file needs the whole engine
python3 - "$OUT/official.inc" <<'P'
import re,sys
s=open('src/qcommon/files.c',encoding='latin-1').read()
a=s.index('#define MAX_OFFICIAL_PAKS'); b=s.index('\n}\n',s.index('qboolean FS_VerifyOfficialPaks( void ) {'))+3
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b])
P
"$CC" $FLAGS "-DOFFICIAL_INC=\"$OUT/official.inc\"" tests/security/test_official.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/official"
"$OUT/official"
for t in msg; do
    "$CC" $FLAGS tests/security/test_$t.c src/qcommon/msg.c src/qcommon/huffman.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/$t"
    "$OUT/$t"
done
