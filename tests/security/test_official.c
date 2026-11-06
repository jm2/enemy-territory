/* FS_VerifyOfficialPaks against a server's pak lists: more official names than the
 * table holds, and names missing for some checksums. Run under ASan. */
#include <stdio.h>
#include <stdlib.h>
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"

void QDECL Com_Error( int code, const char *fmt, ... ) { exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

typedef struct { char pakBasename[64], pakGamename[64]; int checksum; } pack_t;
typedef struct searchpath_s { struct searchpath_s *next; pack_t *pack; } searchpath_t;
static searchpath_t *fs_searchpaths;
static int fs_numServerPaks, fs_serverPaks[4096];
static char *fs_serverPakNames[4096];
qboolean FS_idPak( char *pak, char *base ) { return !strcmp( pak, "etmain/pak0" ); }

typedef struct {
	char pakname[MAX_QPATH];
	qboolean ok;
} officialpak_t;

#include OFFICIAL_INC

int main( void ) {
	int i;

	fs_numServerPaks = 1000;
	for ( i = 0; i < 1000; i++ ) {
		fs_serverPaks[i] = i + 1;
		fs_serverPakNames[i] = "etmain/pak0";
	}
	if ( FS_VerifyOfficialPaks() ) {
		printf( "FAIL: missing paks verified\n" );
		return 1;
	}
	for ( i = 0; i < 1000; i++ ) {
		fs_serverPakNames[i] = NULL;
	}
	FS_VerifyOfficialPaks();
	printf( "official paks: all passed\n" );
	return 0;
}
