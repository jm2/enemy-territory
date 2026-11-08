/* files.c, extracted: FS_ListFilteredFiles with an empty path, and the key files
 * (etkey, rtcwkey) refused by name in any letter case to the read guard, the
 * existence probe, FS_FOpenFileWrite, FS_FOpenFileAppend and FS_Rename, while
 * other names still pass. HFS opens "ETKEY" as etkey. Run under ASan.
 * An argument runs one case only. argv[2] is a scratch directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/qcommon/unzip.h"

void QDECL Com_Error( int code, const char *fmt, ... ) { fprintf( stderr, "FAIL: unexpected Com_Error: %s\n", fmt ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

qboolean com_fullyInitialized;
gameInfo_t com_gameInfo;
static char home[MAX_OSPATH];
static cvar_t debugVar, homeVar, restrictVar;
static cvar_t *fs_debug = &debugVar, *fs_homepath = &homeVar, *fs_restrict = &restrictVar;
char fs_gamedir[MAX_OSPATH] = "etmain";
static int fs_numServerPaks;
static int copies;

char *FS_BuildOSPath( const char *base, const char *game, const char *qpath ) {
	static char ospath[2][MAX_OSPATH];
	static int toggle;

	toggle ^= 1;
	Com_sprintf( ospath[toggle], sizeof( ospath[0] ), "%s/%s/%s", base, game && game[0] ? game : fs_gamedir, qpath );
	return ospath[toggle];
}
int FS_CreatePath( const char *p ) { return 0; }
void FS_CopyFile( char *from, char *to ) { copies++; }
static void FS_Remove( const char *p ) { remove( p ); }
char *CopyString( const char *in ) { return strcpy( malloc( strlen( in ) + 1 ), in ); }
#ifdef ZONE_DEBUG
void *Z_MallocDebug( int size, char *label, char *file, int line ) { return calloc( 1, size ); }
#else
void *Z_Malloc( int size ) { return calloc( 1, size ); }
#endif
int Com_FilterPath( char *filter, char *name, int casesensitive ) { return 0; }
#include FS_TYPES_INC
static searchpath_t *fs_searchpaths;
qboolean FS_PakIsPure( pack_t *p ) { return qtrue; }
char **Sys_ListFiles( const char *d, const char *e, char *f, int *n, qboolean w ) { *n = 0; return NULL; }
void Sys_FreeFileList( char **list ) {}

#include FS_TEXT_INC

static void Check( int ok, const char *what, const char *name ) {
	if ( !ok ) {
		printf( "FAIL: %s: \"%s\"\n", what, name );
		exit( 1 );
	}
}

static int Exists( const char *name ) {
	char p[MAX_OSPATH * 2];
	struct stat st;

	snprintf( p, sizeof( p ), "%s/etmain/%s", home, name );
	return !stat( p, &st );
}

static void ListEmpty( void ) {
	static fileInPack_t files[2] = { { "scripts/a.shader" }, { "b.cfg" } };
	static pack_t pak;
	static directory_t dir;
	static searchpath_t dirPath = { NULL, NULL, &dir }, pakPath = { &dirPath, &pak, NULL };
	char *empty = calloc( 1, 1 ), **list;  /* heap, so ASan sees path[-1] */
	int n, i;

	pak.numfiles = 2;
	pak.buildBuffer = files;
	fs_searchpaths = &pakPath;
	list = FS_ListFilteredFiles( empty, "cfg", NULL, &n );
	Check( n == 1 && !strcmp( list[0], "b.cfg" ), "empty path lists the top level", "" );
	for ( i = 0; i < n; i++ ) {
		free( list[i] );
	}
	free( list );
	list = FS_ListFilteredFiles( "scripts/", "shader", NULL, &n );
	Check( n == 1 && !strcmp( list[0], "a.shader" ), "trailing slash still trimmed", "scripts/" );
	free( list[0] );
	free( list );
	free( empty );
	printf( "list empty path: passed\n" );
}

static const char *keys[] = {
	"etkey", "ETKEY", "EtKey", "eTkEy", "etmain/etkey", "etmain/ETKEY", "profiles/x/EtKey",
	"rtcwkey", "RTCWKEY", "etmain/RtCwKey",
};
static const char *others[] = {
	"etconfig.cfg", "scripts/legacy_gfx_2d.shader", "maps/oasis.bsp", "profiles/x/etconfig.cfg", "et_key",
};
#define N( a ) (int)( sizeof( a ) / sizeof( a[0] ) )

static void KeyRead( void ) {
	int i;

	com_fullyInitialized = qtrue;
	for ( i = 0; i < N( keys ); i++ ) {
		Check( ReadGuard( keys[i] ), "read guard lets a key file through", keys[i] );
	}
	for ( i = 0; i < N( keys ); i++ ) {
		Check( ProbeGuard( keys[i] ), "existence probe lets a key file through", keys[i] );
	}
	for ( i = 0; i < N( others ); i++ ) {
		Check( !ReadGuard( others[i] ) && !ProbeGuard( others[i] ), "read guard refuses an ordinary file", others[i] );
	}
	com_fullyInitialized = qfalse;
	for ( i = 0; i < N( keys ); i++ ) {
		Check( !ReadGuard( keys[i] ) && !ProbeGuard( keys[i] ), "start-up read of the key refused", keys[i] );
	}
	printf( "key read guard: passed\n" );
}

static void KeyWrite( void ) {
	static searchpath_t none;
	fileHandle_t f;
	FILE *o;
	int i;
	char p[MAX_OSPATH * 2];

	fs_searchpaths = &none;
	for ( i = 0; i < N( keys ); i++ ) {
		if ( strchr( keys[i], '/' ) ) {
			continue;
		}
		f = FS_FOpenFileWrite( keys[i] );
		Check( !f && !Exists( keys[i] ), "write to a key file", keys[i] );
		f = FS_FOpenFileAppend( keys[i] );
		Check( !f && !Exists( keys[i] ), "append to a key file", keys[i] );
	}
	f = FS_FOpenFileWrite( "notes.txt" );
	Check( f > 0 && Exists( "notes.txt" ), "ordinary write", "notes.txt" );
	fclose( fsh[f].handleFiles.file.o );
	memset( &fsh[f], 0, sizeof( fsh[f] ) );
	f = FS_FOpenFileAppend( "notes.txt" );
	Check( f > 0, "ordinary append", "notes.txt" );
	fclose( fsh[f].handleFiles.file.o );
	memset( &fsh[f], 0, sizeof( fsh[f] ) );
	FS_Rename( "notes.txt", "EtKey" );
	Check( Exists( "notes.txt" ) && !Exists( "EtKey" ), "rename onto a key file", "EtKey" );
	snprintf( p, sizeof( p ), "%s/etmain/etkey", home );
	o = fopen( p, "wb" );
	fputs( "0000001002", o );
	fclose( o );
	/* stands in for the etkey a case-insensitive HFS volume would open */
	snprintf( p, sizeof( p ), "%s/etmain/ETKEY", home );
	o = fopen( p, "wb" );
	fputs( "0000001002", o );
	fclose( o );
	FS_Rename( "ETKEY", "stolen.txt" );
	FS_Rename( "etkey", "stolen.txt" );
	Check( Exists( "etkey" ) && Exists( "ETKEY" ) && !Exists( "stolen.txt" ), "rename of a key file away", "ETKEY" );
	FS_Rename( "notes.txt", "notes2.txt" );
	Check( !Exists( "notes.txt" ) && Exists( "notes2.txt" ), "ordinary rename", "notes2.txt" );
	printf( "key write guard: passed\n" );
}

int main( int argc, char **argv ) {
	static const struct { const char *name; void ( *run )( void ); } cases[] = {
		{ "list", ListEmpty }, { "keyread", KeyRead }, { "keywrite", KeyWrite },
	};
	char p[MAX_OSPATH * 2];
	int i;

	if ( argc < 3 || strlen( argv[2] ) >= sizeof( home ) - 16 ) {
		printf( "usage: test_fs_text case|all scratchdir\n" );
		return 2;
	}
	strcpy( home, argv[2] );
	homeVar.string = home;
	snprintf( p, sizeof( p ), "%s/etmain", home );
	mkdir( p, 0755 );
	for ( i = 0; i < N( cases ); i++ ) {
		if ( !strcmp( argv[1], "all" ) || !strcmp( argv[1], cases[i].name ) ) {
			cases[i].run();
		}
	}
	return 0;
}
