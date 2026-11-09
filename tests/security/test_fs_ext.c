// FS_HasExtension from files.c: FS_FOpenFileRead's pure-server and reference
// checks compared a suffix from strlen( name ) - strlen( suffix ), before the
// start of a shorter name (legacy's cgame opens short HUD names on a pure server).
#include "../../src/game/q_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void QDECL Com_Error( int level, const char *fmt, ... ) { (void)level; fprintf( stderr, "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
#include FS_EXT_INC

static int fails;
static void check( const char *name, const char *ext, int want ) {
	// the name alone in its own block, so a read before it is a heap underflow
	char *copy = malloc( strlen( name ) + 1 );
	strcpy( copy, name );
	if ( FS_HasExtension( copy, ext ) != want ) {
		fprintf( stderr, "FAIL: \"%s\" ends in \"%s\" should be %d\n", name, ext, want );
		fails++;
	}
	free( copy );
}

int main( void ) {
	check( "etconfig.cfg", ".cfg", 1 );
	check( "ETCONFIG.CFG", ".cfg", 1 );
	check( ".cfg", ".cfg", 1 );
	check( "cfg", ".cfg", 0 );
	check( "", ".botents", 0 );
	check( "a.hud", ".botents", 0 );
	check( "x.bot", "bots.txt", 0 );
	check( "mybots.txt", "bots.txt", 1 );
	check( "demo.dm_84", ".dm_84", 1 );
	check( "maps/oasis.script", ".shader", 0 );
	if ( fails ) {
		return 1;
	}
	printf( "file extension tests passed\n" );
	return 0;
}
