// Runs a loader on bytes in memory, with just enough of the refresh imports.
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/renderer/tr_public.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

refimport_t ri;
static const byte *testFile;
static int testLength;
static int testFiles;

static int H_ReadFile( const char *name, void **buffer ) {
	byte *copy;
	if ( !testFile ) { *buffer = NULL; return -1; }
	// exact size and no terminator, so that the sanitizer sees any read past the end
	copy = malloc( testLength ? testLength : 1 );
	memcpy( copy, testFile, testLength );
	*buffer = copy;
	testFiles++;
	return testLength;
}
static void H_FreeFile( void *buffer ) { free( buffer ); testFiles--; }
static void QDECL H_Printf( int level, const char *fmt, ... ) { (void)level; (void)fmt; }
static void QDECL H_Error( int level, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt ); vfprintf( stderr, fmt, ap ); va_end( ap );
	fprintf( stderr, "\nFAIL: loader raised an error\n" );
	exit( 1 );
}
static void *H_Zone( int size ) { void *p = calloc( 1, size ); if ( !p ) abort(); return p; }
static void H_Free( void *p ) { free( p ); }

#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_Error( int level, const char *fmt, ... ) { fprintf( stderr, "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }

static byte *imageBuffer;
void *R_TryImageBuffer( int size ) {
	free( imageBuffer );
	imageBuffer = malloc( size );
	return imageBuffer;
}
static void H_Init( void ) {
	ri.FS_ReadFile = (void *)H_ReadFile;
	ri.FS_FreeFile = H_FreeFile;
	ri.Printf = H_Printf;
	ri.Error = H_Error;
	ri.Z_Malloc = H_Zone;
	ri.Free = H_Free;
}
