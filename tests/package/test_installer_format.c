// The installer's parser against damaged payloads. Anything it accepts must lie inside the file.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "et_installer_format.h"

#define ENTRIES 3
#define FORKS 300

static void Put( unsigned char *p, unsigned long v ) {
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// [ header | forks | trailer ], with the header at offset 0
static int Make( unsigned char *b ) {
	int header = ET_HEADER_BYTES + ENTRIES * ET_ENTRY_BYTES, i;
	unsigned char *e;

	memset( b, 0, header + FORKS + ET_TRAILER_BYTES );
	memcpy( b, "ETIN", 4 ); Put( b + 4, ET_FORMAT_VERSION ); Put( b + 8, ENTRIES );
	for ( i = 0; i < ENTRIES; i++ ) {
		e = b + ET_HEADER_BYTES + i * ET_ENTRY_BYTES;
		e[0] = 1; e[1] = 'a' + i;
		Put( e + 32, i == 2 ? 1 : 0xffffffffu );
		if ( i == 1 ) {
			memcpy( e + 36, "fold", 4 );
		} else {
			memcpy( e + 36, "TEXT", 4 );
			Put( e + 48, i == 0 ? 100 : 150 );
			Put( e + 52, i == 0 ? 50 : 0 );
		}
	}
	e = b + header + FORKS;
	memcpy( e, "ETIT", 4 ); Put( e + 4, 0 ); Put( e + 8, header ); Put( e + 12, ET_Crc( 0, b, header ) );
	return header + FORKS + ET_TRAILER_BYTES;
}

static int Parse( const unsigned char *b, int n, etEntry_t *entries ) {
	unsigned char *copy = malloc( n ? n : 1 );      // exact size, for the sanitizer
	etTrailer_t t;
	int count = -1;

	memcpy( copy, b, n );
	if ( n >= ET_TRAILER_BYTES && ET_ParseTrailer( copy + n - ET_TRAILER_BYTES, n, &t ) == 0 ) {
		unsigned char *header = malloc( t.headerLength );

		memcpy( header, copy + t.headerOffset, t.headerLength );
		count = ET_ParseHeader( header, &t, n, entries );
		free( header );
		if ( count >= 0 ) {
			long long sum = 0;
			int i;

			for ( i = 0; i < count; i++ ) {
				sum += (long long)entries[i].dataLength + entries[i].rsrcLength;
				if ( entries[i].parent >= i || entries[i].name[0] > 31 ) {
					fprintf( stderr, "FAIL: entry %d out of range\n", i ); exit( 1 );
				}
			}
			if ( sum + t.headerOffset + t.headerLength + ET_TRAILER_BYTES != n ) {
				fprintf( stderr, "FAIL: forks do not fill the file\n" ); exit( 1 );
			}
		}
	}
	free( copy );
	return count;
}

// makes the header CRC right again, so that the checks behind it are reached
static void Reseal( unsigned char *b, int n ) {
	int header = ET_HEADER_BYTES + ENTRIES * ET_ENTRY_BYTES;

	Put( b + n - 4, ET_Crc( 0, b, header ) );
}

static void Refused( const char *what, const unsigned char *b, int n ) {
	etEntry_t entries[ET_MAX_ENTRIES];

	if ( Parse( b, n, entries ) >= 0 ) {
		fprintf( stderr, "FAIL: %s was accepted\n", what ); exit( 1 );
	}
}

// a data fork from scripts/installer_pack.py
static void ReadPacked( const char *path, int expected ) {
	etEntry_t entries[ET_MAX_ENTRIES];
	unsigned char *data;
	FILE *f = fopen( path, "rb" );
	long n;

	if ( !f || fseek( f, 0, SEEK_END ) || ( n = ftell( f ) ) <= 0 ) {
		fprintf( stderr, "FAIL: cannot read %s\n", path ); exit( 1 );
	}
	rewind( f );
	data = malloc( n );
	if ( fread( data, 1, n, f ) != (size_t)n ) {
		fprintf( stderr, "FAIL: short read\n" ); exit( 1 );
	}
	fclose( f );
	if ( Parse( data, n, entries ) != expected ) {
		fprintf( stderr, "FAIL: the parser refused the packer's output\n" ); exit( 1 );
	}
	free( data );
}

int main( int argc, char **argv ) {
	static unsigned char b[1024], m[1024];
	etEntry_t entries[ET_MAX_ENTRIES];
	int n = Make( b ), i, r, k, header = ET_HEADER_BYTES + ENTRIES * ET_ENTRY_BYTES;

	if ( argc == 3 ) {
		ReadPacked( argv[1], atoi( argv[2] ) );
		printf( "parser reads the packer's output\n" );
		return 0;
	}
	if ( ET_Crc( 0, (const unsigned char *)"123456789", 9 ) != 0xcbf43926ul ) {
		fprintf( stderr, "FAIL: CRC\n" ); return 1;
	}
	if ( Parse( b, n, entries ) != ENTRIES || entries[2].parent != 1 || entries[0].rsrcLength != 50 ) {
		fprintf( stderr, "FAIL: good payload\n" ); return 1;
	}
	for ( i = 0; i < n; i++ ) {
		Refused( "a truncated file", b, i );     // the trailer has gone
	}
	memcpy( m, b, n ); m[3] ^= 1; Refused( "a damaged header", m, n );
	memcpy( m, b, n ); Put( m + ET_HEADER_BYTES + 48, 0xffffffffu ); Reseal( m, n ); Refused( "a huge fork", m, n );
	memcpy( m, b, n ); Put( m + ET_HEADER_BYTES + 32, 0 ); Reseal( m, n ); Refused( "a parent of itself", m, n );
	memcpy( m, b, n ); Put( m + ET_HEADER_BYTES + 2 * ET_ENTRY_BYTES + 32, 0 ); Reseal( m, n ); Refused( "a parent that is a file", m, n );
	memcpy( m, b, n ); m[ET_HEADER_BYTES + 1] = ':'; Reseal( m, n ); Refused( "a colon", m, n );
	memcpy( m, b, n ); m[ET_HEADER_BYTES] = 32; Reseal( m, n ); Refused( "a long name", m, n );
	memcpy( m, b, n ); Put( m + ET_HEADER_BYTES + ET_ENTRY_BYTES + 48, 1 ); Reseal( m, n ); Refused( "a folder with a fork", m, n );
	memcpy( m, b, n ); Put( m + 8, ET_MAX_ENTRIES + 1 ); Reseal( m, n ); Refused( "too many entries", m, n );
	srand( 7 );
	for ( r = 0; r < 300000; r++ ) {
		memcpy( m, b, n );
		for ( k = 1 + rand() % 4; k > 0; k-- ) {
			int at = rand() % 2 ? rand() % header : n - ET_TRAILER_BYTES + rand() % ET_TRAILER_BYTES;

			m[at] = rand() & 1 ? rand() : 0xff;
		}
		if ( rand() & 1 ) {
			Reseal( m, n );
		}
		Parse( m, rand() & 7 ? n : rand() % ( n + 1 ), entries );
	}
	printf( "installer format tests passed\n" );
	return 0;
}
