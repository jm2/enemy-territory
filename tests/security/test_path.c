/* Names a server can send as paths, under '/' and HFS ':' rules. */
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

#define YES( f, s ) do { if ( !f( s ) ) { printf( "FAIL: " #f " refused \"%s\"\n", s ); exit( 1 ); } } while ( 0 )
#define NO( f, s ) do { if ( f( s ) ) { printf( "FAIL: " #f " allowed \"%s\"\n", s ); exit( 1 ); } } while ( 0 )

int main( void ) {
	/* retail and mod content must still pass */
	YES( FS_CheckQPath, "maps/oasis.bsp" );
	YES( FS_CheckQPath, "scripts/legacy_gfx_2d.shader" );
	YES( FS_CheckQPath, "etkey" );
	YES( FS_CheckQPath, "cgame.mp.i386.so" );
	YES( FS_CheckQPath, "legacy/legacy_v2.86.2.pk3.tmp" );
	YES( FS_CheckPakName, "legacy/legacy_v2.86.2" );
	YES( FS_CheckPakName, "etmain/pak0" );
	YES( FS_CheckPakName, "etmain/mp_custom-b3_final" );
	YES( FS_CheckDownloadName, "legacy/legacy_v2.86.2.pk3" );
	YES( FS_CheckDownloadName, "etmain/map.0badc0de.pk3" );
	YES( FS_CheckGameDir, "legacy" );

	NO( FS_CheckQPath, "../etkey" );
	NO( FS_CheckQPath, "a/../../etkey" );
	NO( FS_CheckQPath, "..\\etkey" );
	NO( FS_CheckQPath, ":etkey" );
	NO( FS_CheckQPath, "a::b" );
	NO( FS_CheckQPath, "C:\\windows" );
	NO( FS_CheckPakName, "" );
	NO( FS_CheckPakName, "/pak0" );
	NO( FS_CheckPakName, "etmain/" );
	NO( FS_CheckPakName, "pak0" );
	NO( FS_CheckPakName, "../pak0" );
	NO( FS_CheckPakName, "etmain/../../pak0" );
	NO( FS_CheckPakName, "etmain/sub/pak0" );
	NO( FS_CheckPakName, "etmain\\pak0" );
	NO( FS_CheckPakName, "etmain:pak0" );
	NO( FS_CheckPakName, "etmain//pak0" );
	NO( FS_CheckPakName, "etmain/a@b" );
	NO( FS_CheckDownloadName, "etmain/pak0" );
	NO( FS_CheckDownloadName, "etmain/pak0.pk" );
	NO( FS_CheckDownloadName, ".pk3" );
	NO( FS_CheckDownloadName, "etmain/autoexec.cfg" );
	NO( FS_CheckGameDir, "" );
	NO( FS_CheckGameDir, "." );
	NO( FS_CheckGameDir, ".." );
	NO( FS_CheckGameDir, "a/b" );
	NO( FS_CheckGameDir, "a:b" );
#if ET_PATH_SEP == ':'
	NO( FS_CheckQPath, "/etkey" );
	NO( FS_CheckQPath, "a//b" );
	NO( FS_CheckQPath, "a/\\b" );
	printf( "paths: all passed with HFS separators\n" );
#else
	printf( "paths: all passed\n" );
#endif
	return 0;
}
