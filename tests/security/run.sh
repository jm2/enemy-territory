#!/usr/bin/env bash
# Host sanitizer tests of the code that parses what a peer sends.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export TMPDIR=${TMPDIR:-/var/tmp}
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
# CL_AddReliableCommand alone: its file needs the whole client
python3 - "$OUT/reliable.inc" <<'R'
import sys
s=open('src/client/cl_main.c',encoding='latin-1').read()
a=s.index('void CL_AddReliableCommand( const char *cmd ) {'); b=s.index('\n}\n',a)+3
# CL_Disconnect's guard, as the real code has it, for the test to evaluate
c=s.index('void CL_Disconnect( qboolean showMainMenu ) {'); d=s.index('\n}\n',c)
g=s.index('if ( clc.reliableSequence - clc.reliableAcknowledge',c)
assert g<d, 'CL_Disconnect has no command queue guard'
guard=s[g+len('if '):s.index(' {',g)]
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b]+'#define DISCONNECT_GUARD %s\n' % guard)
R
"$CC" $FLAGS "-DRELIABLE_INC=\"$OUT/reliable.inc\"" tests/security/test_reliable.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/reliable"
"$OUT/reliable"
# GetWavinfo alone: its file needs the sound system
python3 - "$OUT/wav.inc" <<'W'
import sys
s=open('src/client/snd_mem.c',encoding='latin-1').read()
a=s.index('static byte    *data_p;'); b=s.index('\n}\n',s.index('static wavinfo_t GetWavinfo('))+3
d=open('src/client/snd_dma.c',encoding='latin-1').read()
c=d.index('void S_ByteSwapRawSamples( int samples, int width, int s_channels, short *data ) {'); e=d.index('\n}\n',c)+3
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b]+d[c:e])
W
"$CC" $FLAGS -D__MACOS__ "-DWAV_INC=\"$OUT/wav.inc\"" tests/security/test_wav.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/wav"
"$OUT/wav"
# FS_LoadZipFile alone: its file needs the whole engine
python3 - "$OUT/zip.inc" <<'Z'
import sys
s=open('src/qcommon/files.c',encoding='latin-1').read()
a=s.index('#define MAX_ZPATH'); b=s.index('} pack_t;',a)+len('} pack_t;\n')
c=s.index('static long FS_HashFileName'); d=s.index('\n}\n',c)+3
e=s.index('static pack_t *FS_LoadZipFile('); f=s.index('\n}\n',e)+3
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b]+s[c:d]+s[e:f])
Z
"$CC" $FLAGS "-DZIP_INC=\"$OUT/zip.inc\"" tests/security/test_zip.c src/qcommon/unzip.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/zip"
"$OUT/zip"
# COM_ParseExt and Info_SetValueForKey from q_shared.c, and tab completion's FindMatches
# with the statics it works on, verbatim from common.c (adapted from Quake III 7ef6c86b)
python3 - "$OUT/findmatches.inc" <<'F'
import sys
s=open('src/qcommon/common.c',encoding='latin-1').read()
a=s.index('static const char *completionString;'); b=s.index('\n}\n',s.index('static void FindMatches( const char *s ) {'))+3
open(sys.argv[1],'w',encoding='latin-1').write('#line %d "src/qcommon/common.c"\n' % (s.count('\n',0,a)+1)+s[a:b])
F
"$CC" $FLAGS "-DFINDMATCHES_INC=\"$OUT/findmatches.inc\"" tests/security/test_text.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/text"
"$OUT/text"
# files.c's empty-path listing and key file guards: its file needs the whole engine
python3 - "$OUT/fs_types.inc" "$OUT/fs_text.inc" <<'K'
import re,sys
s=open('src/qcommon/files.c',encoding='latin-1').read()
def fn(sig, optional=False):
    if optional and sig not in s:
        return ''
    a=s.index(sig); return s[a:s.index('\n}\n',a)+3]
a=s.index('#define MAX_ZPATH'); b=s.index('} searchpath_t;',a)+len('} searchpath_t;\n')
c=s.index('typedef union qfile_gus'); d=s.index('static fileHandleData_t fsh[MAX_FILE_HANDLES];',c)
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b]+s[c:d]+'static fileHandleData_t fsh[MAX_FILE_HANDLES];\n')
out=[fn('static fileHandle_t FS_HandleForFile( void ) {'),
     fn('static qboolean FS_IsKeyFileName( const char *filename ) {',True),
     fn('static qboolean FS_CheckWriteIsNotKey( const char *filename, const char *function ) {',True),
     fn('void FS_Rename( const char *from, const char *to ) {'),
     fn('fileHandle_t FS_FOpenFileWrite( const char *filename ) {'),
     fn('fileHandle_t FS_FOpenFileAppend( const char *filename ) {')]
e=s.index('#define MAX_FOUND_FILES'); out.append(s[e:s.index('\n}\n',s.index('char **FS_ListFilteredFiles('))+3])
# FS_FOpenFileRead's two guards, as the real code has them: the read refusal and the existence probe
r=fn('int FS_FOpenFileRead( const char *filename, fileHandle_t *file, qboolean uniqueFILE ) {')
g=r.index('if ( com_fullyInitialized && ',r.index('make sure the q3key file is only readable'))
read=r[g+len('if '):r.index(' {',g)]
p=r[r.index('if ( file == NULL ) {'):r.index('for ( search = fs_searchpaths',r.index('if ( file == NULL ) {'))]
m=re.search(r'if \( (com_fullyInitialized && .*?) \) \{',p)
probe='(%s)' % m.group(1) if m else '0'
out.append('static int ReadGuard( const char *filename ) { return %s; }\n' % read)
out.append('static int ProbeGuard( const char *filename ) { return %s; }\n' % probe)
open(sys.argv[2],'w',encoding='latin-1').write(''.join(out))
K
"$CC" $FLAGS "-DFS_TYPES_INC=\"$OUT/fs_types.inc\"" "-DFS_TEXT_INC=\"$OUT/fs_text.inc\"" tests/security/test_fs_text.c src/qcommon/fs_path.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/fs_text"
mkdir "$OUT/fs_home"
"$OUT/fs_text" all "$OUT/fs_home"
JPEG=$(for f in jdapimin jdapistd jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdpostct jdsample jerror jmemmgr jmemnobs jutils jcomapi jidctflt; do echo src/jpeg-6/$f.c; done)
# libjpeg's Huffman reader shifts its bit buffer through the sign bit by design. Harmless: its
# memory errors are what matter, so only that check is off for it.
mkdir "$OUT/jpeg"
for f in $JPEG; do "$CC" $FLAGS -fno-sanitize=shift,signed-integer-overflow -c "$f" -o "$OUT/jpeg/$(basename "$f").o"; done
"$CC" $FLAGS tests/security/test_images.c src/renderer/tr_image_bmp.c src/renderer/tr_image_pcx.c src/renderer/tr_image_tga.c src/renderer/tr_image_jpeg.c "$OUT"/jpeg/*.o src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/images"
"$OUT/images"
# Shader and skin parsing: the tests include the real tr_shader.c and tr_image.c, and
# the linker drops the parts of the renderer they do not reach
for t in shader skin; do
    "$CC" $FLAGS -ffunction-sections -fdata-sections tests/security/test_$t.c src/game/q_shared.c src/game/q_math.c -Wl,--gc-sections -lm -o "$OUT/$t"
    ASAN_OPTIONS=detect_leaks=0 "$OUT/$t"
done
for t in msg; do
    "$CC" $FLAGS tests/security/test_$t.c src/qcommon/msg.c src/qcommon/huffman.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/$t"
    "$OUT/$t"
done
