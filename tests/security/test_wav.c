// Hostile WAV headers. Everything GetWavinfo reports must lie inside the file.
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/client/snd_local.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void Com_Memset( void *d, const int v, const size_t n ) { memset( d, v, n ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_Error( int level, const char *fmt, ... ) { fprintf( stderr, "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }
#include WAV_INC

static int MakeWav( byte *b, int rate, int width, int samples ) {
	int bytes = samples * width, n = 44;
	memset( b, 0, 44 );
	memcpy( b, "RIFF", 4 ); memcpy( b + 8, "WAVEfmt ", 8 ); b[16] = 16; b[20] = 1; b[22] = 1;
	b[24] = rate; b[25] = rate >> 8; b[34] = width * 8;
	memcpy( b + 36, "data", 4 ); b[40] = bytes; b[41] = bytes >> 8;
	b[4] = ( n - 8 + bytes ) & 255; b[5] = ( n - 8 + bytes ) >> 8;
	memset( b + 44, 0x40, bytes );
	return n + bytes;
}

static wavinfo_t Parse( const byte *data, int length ) {
	// exact size, so the sanitizer sees any read past the end
	byte *copy = malloc( length ? length : 1 );
	wavinfo_t info;
	memcpy( copy, data, length );
	info = GetWavinfo( "test", copy, length );
	if ( info.samples && info.dataofs + (long long)info.samples * info.width > length ) {
		fprintf( stderr, "FAIL: data past the end of the file\n" );
		exit( 1 );
	}
	if ( info.samples && info.width ) {     // what the resamplers will read
		volatile byte sum = 0;
		int i;
		for ( i = 0; i < info.samples * info.width; i++ ) sum += copy[info.dataofs + i];
	}
	free( copy );
	return info;
}

int main( void ) {
	static byte b[1 << 16], m[1 << 16];
	wavinfo_t info;
	int n, i, r, k;

	n = MakeWav( b, 22050, 2, 100 );
	info = Parse( b, n );
	if ( info.samples != 100 || info.width != 2 || info.channels != 1 || info.rate != 22050 || info.dataofs != 44 ) {
		fprintf( stderr, "FAIL: good file\n" ); return 1;
	}
	// a data chunk claiming more than the file holds ends with the file
	b[43] = 0x7f;
	info = Parse( b, n );
	if ( info.samples != 100 ) { fprintf( stderr, "FAIL: long data chunk %d\n", info.samples ); return 1; }
	n = MakeWav( b, 22050, 2, 100 );
	// 4 bits a sample divided by zero
	b[34] = 4;
	info = Parse( b, n );
	if ( info.samples ) { fprintf( stderr, "FAIL: sub-byte width\n" ); return 1; }
	n = MakeWav( b, 22050, 2, 100 );
	for ( i = 0; i < n; i++ ) Parse( b, i );
	srand( 99 );
	for ( r = 0; r < 200000; r++ ) {
		memcpy( m, b, n );
		for ( k = 1 + rand() % 6; k > 0; k-- ) m[rand() % 60] = rand() & 1 ? rand() : 0xff;
		Parse( m, rand() & 3 ? n : rand() % ( n + 1 ) );
	}
	printf( "wav tests passed\n" );
	return 0;
}
