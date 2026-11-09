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
# COM_ParseExt, Info_SetValueForKey and va from q_shared.c, and tab completion's FindMatches
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
# the linker drops the parts of the renderer they do not reach. float-cast-overflow is
# not in -fsanitize=undefined: it catches shader numbers converted to bytes and ints
for t in shader skin; do
    "$CC" $FLAGS -fsanitize=float-cast-overflow -fno-sanitize-recover=float-cast-overflow -ffunction-sections -fdata-sections tests/security/test_$t.c src/game/q_shared.c src/game/q_math.c -Wl,--gc-sections -lm -o "$OUT/$t"
    ASAN_OPTIONS=detect_leaks=0 "$OUT/$t"
done
# The RoQ decoder, cl_cin.c included whole. Its motion copies move unaligned doubles
# by design (odd-pixel vectors; fine on x86 and PPC), so only the alignment check is off.
"$CC" $FLAGS -fno-sanitize-recover=all -fno-sanitize=alignment tests/security/test_cin.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/cin"
"$OUT/cin"
# The server's client-state handling: the real functions alone, their files need the whole engine
python3 - "$OUT/server.inc" <<'S'
import sys
def fn(path,sigs):
    s=open(path,encoding='latin-1').read(); out=''
    for sig in sigs:
        a=s.index('\n'+sig)+1; out+=s[a:s.index('\n}\n',a)+3]
    return out
inc=fn('src/server/sv_main.c',['char    *SV_ExpandNewlines( char *in ) {','void SV_AddServerCommand( client_t *client, const char *cmd ) {',
    'void QDECL SV_SendServerCommand( client_t *cl, const char *fmt, ... ) {'])
inc+=fn('src/server/sv_init.c',['void SV_SetConfigstring( int index, const char *val ) {','static void SV_SendConfigstring( client_t *client, int index ) {',
    'void SV_UpdateConfigStrings( void ) {','void SV_UpdateClientConfigstrings( client_t *client ) {','void SV_SetUserinfo( int index, const char *val ) {'])
inc+=fn('src/server/sv_client.c',['void SV_SendClientGameState( client_t *client ) {','void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {',
    'static void SV_CloseDownload( client_t *cl ) {','void SV_DoneDownload_f( client_t *cl ) {','void SV_NextDownload_f( client_t *cl ) {',
    'void SV_CopyClientName( char *dest, const char *src, int destsize ) {','void SV_UserinfoChanged( client_t *cl ) {',
    'static void SV_ResetPureClient_f( client_t *cl ) {','static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {',
    'void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {','void SV_ClientMapRestarted( client_t *client ) {',
    'void SV_WWWDownload_f( client_t *cl ) {'])
inc+=fn('src/server/sv_bot.c',['int SV_BotAllocateClient( int clientNum ) {'])
inc+=fn('src/server/sv_ccmds.c',['static void SV_MapRestart_f( void ) {'])
open(sys.argv[1],'w',encoding='latin-1').write(inc)
S
"$CC" $FLAGS "-DSERVER_INC=\"$OUT/server.inc\"" tests/security/test_server_state.c src/qcommon/msg.c src/qcommon/huffman.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/server"
"$OUT/server"
for t in msg; do
    "$CC" $FLAGS tests/security/test_$t.c src/qcommon/msg.c src/qcommon/huffman.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/$t"
    "$OUT/$t"
done
# the real cl_parse.c and cvar.c, with CL_BeginDownload alone from cl_main.c
python3 - "$OUT/begindl.inc" <<'P'
import sys
s=open('src/client/cl_main.c',encoding='latin-1').read()
a=s.index('qboolean CL_BeginDownload( const char *localName, const char *remoteName ) {'); b=s.index('\n}\n',a)+3
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b])
P
for sep in "'/'" "':'"; do
    "$CC" $FLAGS "-DTEST_PATH_SEP=$sep" "-DBEGINDL_INC=\"$OUT/begindl.inc\"" tests/security/test_clparse.c src/client/cl_parse.c src/qcommon/cvar.c src/qcommon/fs_path.c src/qcommon/msg.c src/qcommon/huffman.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/clparse"
    "$OUT/clparse"
done
# Q_rsqrt and SquareRootFloat read a float's 32 bits, not a 64-bit host long.
# SquareRootFloat alone: cm_trace.c needs the whole collision map
python3 - "$OUT/sqrtf.inc" <<'R'
import sys
s=open('src/qcommon/cm_trace.c',encoding='latin-1').read()
a=s.index('float SquareRootFloat( float number ) {'); b=s.index('\n}\n',a)+3
open(sys.argv[1],'w',encoding='latin-1').write('#line %d "src/qcommon/cm_trace.c"\n'%(s.count('\n',0,a)+1)+s[a:b])
R
"$CC" $FLAGS -fno-sanitize-recover=all "-DSQRTF_INC=\"$OUT/sqrtf.inc\"" tests/security/test_rsqrt.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/rsqrt"
"$OUT/rsqrt"
# The model loaders and the bone, mesh and surface code that trusts them. The
# second build makes LittleLong and friends swap, as on the big-endian target;
# LongSwap shifts a byte into the sign bit by design.
MODELS="tests/security/test_models.c src/renderer/tr_model.c src/renderer/tr_animation_mds.c src/renderer/tr_animation_mdm.c src/renderer/tr_mesh.c src/renderer/tr_cmesh.c src/renderer/tr_surface.c src/game/q_math.c"
"$CC" $FLAGS -fno-sanitize-recover=all -Isrc/renderer $MODELS -lm -o "$OUT/models"
"$OUT/models"
"$CC" $FLAGS -fno-sanitize-recover=all -fno-sanitize=shift-base -DMODEL_TEST_SWAP -Isrc/renderer $MODELS -lm -o "$OUT/models_swapped"
"$OUT/models_swapped"
# BSP preflight: the real CM_LoadMap and RE_LoadWorldMap on corrupted maps. Set
# BSP_FILES to a list of .bsp files to also load, truncate and mutate each of them.
"$CC" $FLAGS tests/security/test_bsp_cm.c src/qcommon/cm_load.c src/qcommon/cm_patch.c src/qcommon/cm_test.c src/qcommon/cm_polylib.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/bsp_cm"
# collision patch plane, facet and border budgets: the test includes cm_patch.c for its statics
"$CC" $FLAGS tests/security/test_cm_patch.c src/qcommon/cm_polylib.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/cm_patch"
"$OUT/cm_patch"
# R_FindLightmap alone: tr_shader.c needs the whole renderer
python3 - "$OUT/lightmap.inc" <<'L'
import sys
s=open('src/renderer/tr_shader.c',encoding='latin-1').read()
a=s.index('#define EXTERNAL_LIGHTMAP'); b=s.index('\n}\n',s.index('void R_FindLightmap( int *lightmapIndex ) {'))+3
open(sys.argv[1],'w',encoding='latin-1').write(s[a:b])
L
"$CC" $FLAGS "-DLIGHTMAP_INC=\"$OUT/lightmap.inc\"" tests/security/test_bsp_tr.c src/renderer/tr_curve.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/bsp_tr"
# R_StitchPatches alone, its grid inserts stubbed, with exact-size LOD error arrays
"$CC" $FLAGS -fno-sanitize-recover=all -ffunction-sections -fdata-sections tests/security/test_bsp_stitch.c src/game/q_shared.c src/game/q_math.c -Wl,--gc-sections -lm -o "$OUT/bsp_stitch"
"$OUT/bsp_stitch"
# the world tree walks down a 65,536-node chain in a 256 KB thread stack: the
# collision ones with the real cm_*.c, the renderer ones extracted from their files
"$CC" $FLAGS -DWALK_STACKS -pthread tests/security/test_bsp_walk_cm.c src/qcommon/cm_load.c src/qcommon/cm_patch.c src/qcommon/cm_test.c src/qcommon/cm_trace.c src/qcommon/cm_polylib.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/bsp_walk_cm"
python3 - "$OUT/walk.inc" <<'X'
import sys
def fn(path, sig):
    s = open(path, encoding='latin-1').read()
    a = s.index(sig); b = s.index('\n}\n', a) + 3
    return '#line %d "%s"\n' % (s.count('\n', 0, a) + 1, path) + s[a:b]
out = [fn('src/renderer/tr_decals.c', 'qboolean R_TestDecalBoundingBox( decalProjector_t *dp, vec3_t mins, vec3_t maxs ) {'),
       fn('src/renderer/tr_main.c', 'void R_SetupFrustum( void ) {'),
       fn('src/renderer/tr_world.c', 'static void R_RecursiveWorldNode( mnode_t *node, int planeBits, int dlightBits, int decalBits ) {'),
       fn('src/renderer/tr_world.c', 'static mnode_t *R_PointInLeaf( const vec3_t p ) {'),
       fn('src/renderer/tr_world.c', 'static const byte *R_ClusterPVS( int cluster ) {'),
       fn('src/renderer/tr_world.c', 'static void R_MarkLeaves( void ) {'),
       fn('src/renderer/tr_marks.c', 'void R_BoxSurfaces_r( mnode_t *node, vec3_t mins, vec3_t maxs, surfaceType_t **list, int listsize, int *listlength, vec3_t dir ) {')]
open(sys.argv[1], 'w', encoding='latin-1').write(''.join(out))
X
"$CC" $FLAGS -DWALK_STACKS -pthread "-DWALK_INC=\"$OUT/walk.inc\"" tests/security/test_bsp_walk_tr.c src/renderer/tr_curve.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/bsp_walk_tr"
"$OUT/bsp_walk_cm"
"$OUT/bsp_walk_tr"
"$OUT/bsp_cm" | grep -v '^case: \|^    '
"$OUT/bsp_tr" | grep -v '^case: \|^    '
if [ -n "${BSP_FILES:-}" ]; then
    "$OUT/bsp_cm" $BSP_FILES
    "$OUT/bsp_tr" $BSP_FILES
    # tree depths, and digests of random traces, box queries and world walks
    "$OUT/bsp_walk_cm" $BSP_FILES
    "$OUT/bsp_walk_tr" $BSP_FILES
fi
# the botlib preprocessor that reads menus, characters and weapons from pk3s, on
# the real zone allocator: its file needs the whole engine
python3 - "$OUT/zone.inc" <<'B'
import sys
s=open('src/qcommon/common.c',encoding='latin-1').read()
a=s.index('#define ZONEID')
e='void *Z_TryTagMalloc( int size, int tag ) {' if 'void *Z_TryTagMalloc(' in s else 'void *Z_TagMalloc( int size, int tag ) {'
b=s.index('\n}\n',s.index(e))+3
h=('#define HAVE_Z_TRY 1\n' if 'Z_TryTagMalloc' in s[a:b] else '')+('#define HAVE_Z_AVAILABLE 1\n' if 'Z_AvailableMemory' in s[a:b] else '')
q=open('src/qcommon/qcommon.h',encoding='latin-1').read()
c=q.index('typedef enum {\n\tTAG_FREE,'); d=q.index('} memtag_t;',c)+len('} memtag_t;\n')
open(sys.argv[1],'w',encoding='latin-1').write(h+q[c:d]+s[a:b])
B
# the zone rounds blocks to 4 bytes, as the 32-bit target needs; a 64-bit host
# would flag its 8-byte pointers in them; float-cast-overflow, which
# -fsanitize=undefined leaves out, checks the number and $evalfloat conversions
"$CC" $FLAGS -fno-sanitize=alignment -fsanitize=float-cast-overflow -fno-sanitize-recover=float-cast-overflow "-DZONE_INC=\"$OUT/zone.inc\"" tests/security/test_botlib_precomp.c src/botlib/l_precomp.c src/botlib/l_script.c src/botlib/l_memory.c src/game/q_shared.c src/game/q_math.c -pthread -lm -o "$OUT/botlib_precomp"
"$OUT/botlib_precomp"
# Key_IsDown, the binding functions and bind/unbind alone: cl_keys.c needs the whole client
python3 - "$OUT/keys.inc" <<'K'
import sys
s=open('src/client/cl_keys.c',encoding='latin-1').read()
def fn(sig):
    a=s.index(sig); return '#line %d "src/client/cl_keys.c"\n'%(s.count('\n',0,a)+1)+s[a:s.index('\n}\n',a)+3]
h=s.index('#define BIND_HASH_SIZE')
out=[fn('int Key_StringToKeynum( char *str ) {'), fn('qboolean Key_IsDown( int keynum ) {'),
     s[h:s.index('\n}\n',h)+3], fn('void Key_SetBinding( int keynum, const char *binding ) {'),
     fn('char *Key_GetBinding( int keynum ) {'), fn('void Key_Unbind_f( void ) {'), fn('void Key_Bind_f( void ) {')]
open(sys.argv[1],'w',encoding='latin-1').write(''.join(out))
K
"$CC" $FLAGS -fno-sanitize-recover=all "-DKEYS_INC=\"$OUT/keys.inc\"" tests/security/test_keys.c src/game/q_shared.c src/game/q_math.c -lm -o "$OUT/keys"
"$OUT/keys"
