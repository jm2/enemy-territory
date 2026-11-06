// Hostile pk3 files: the reader must refuse them or read them within bounds.
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/qcommon/unzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zip_seed.h"
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_Error( int level, const char *fmt, ... ) { fprintf( stderr, "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }
void Com_Memset( void *d, const int v, const size_t n ) { memset( d, v, n ); }
void Com_Memcpy( void *d, const void *s, const size_t n ) { memcpy( d, s, n ); }
void *Z_Malloc( int size ) { void *p; if ( size < 0 ) { fprintf( stderr, "FAIL: negative alloc\n" ); exit( 1 ); } p = calloc( 1, size ? size : 1 ); if ( !p ) abort(); return p; }
void Z_Free( void *p ) { free( p ); }
#ifdef ZONE_DEBUG
#error
#endif

static char path[512];

// FS_LoadZipFile and what it needs, cut from files.c by run.sh
int fs_checksumFeed, fs_packFiles;
unsigned Com_BlockChecksum( const void *b, int n ) { (void)b; return n; }
unsigned Com_BlockChecksumKey( void *b, int n, int k ) { (void)b; (void)k; return n; }
#include ZIP_INC

// walk every entry as files.c does, and read each to its end
static int Walk( void ) {
	unzFile uf = unzOpen( path );
	unz_global_info gi;
	unz_file_info info;
	char name[256], buf[8192];
	unsigned long i;
	int total = 0, r, loops;

	if ( !uf ) return 0;
	if ( unzGetGlobalInfo( uf, &gi ) != UNZ_OK ) { unzClose( uf ); return 0; }
	if ( gi.number_entry && unzGoToFirstFile( uf ) == UNZ_OK ) {
		for ( i = 0; i < gi.number_entry && i < 64; i++ ) {
			if ( unzGetCurrentFileInfo( uf, &info, name, sizeof( name ), NULL, 0, NULL, 0 ) != UNZ_OK ) break;
			if ( unzOpenCurrentFile( uf ) == UNZ_OK ) {
				for ( loops = 0; loops < 4096; loops++ ) {
					r = unzReadCurrentFile( uf, buf, sizeof( buf ) );
					if ( r <= 0 ) break;
					total += r;
				}
				if ( loops == 4096 ) { fprintf( stderr, "FAIL: read does not end\n" ); exit( 1 ); }
				unzCloseCurrentFile( uf );
			}
			if ( unzGoToNextFile( uf ) != UNZ_OK ) break;
		}
	}
	unzClose( uf );
	return total;
}

// offset of a 4 byte signature, counted from the end when last is set
static int Find( const byte *d, int n, unsigned sig ) {
	int i;
	for ( i = 0; i + 4 <= n; i++ ) {
		if ( d[i] == ( sig & 255 ) && d[i + 1] == ( ( sig >> 8 ) & 255 ) && d[i + 2] == ( ( sig >> 16 ) & 255 ) && d[i + 3] == sig >> 24 ) return i;
	}
	fprintf( stderr, "FAIL: no signature\n" );
	exit( 1 );
}

static void Put( const byte *d, int n ) {
	FILE *f = fopen( path, "wb" );
	fwrite( d, 1, n, f );
	fclose( f );
}

int main( void ) {
	static byte m[1 << 20];
	int i, r, k, good;

	Swap_Init();
	snprintf( path, sizeof( path ), "%s/etzip%d.pk3", getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/var/tmp", (int)getpid() );
	Put( zip_seed, zip_seed_len );
	good = Walk();
	if ( good != ZIP_SEED_BYTES ) { fprintf( stderr, "FAIL: good file read %d of %d\n", good, ZIP_SEED_BYTES ); return 1; }

	for ( i = 0; i < zip_seed_len; i += 7 ) { Put( zip_seed, i ); Walk(); }

	// CVE-2002-0059: a tree error other than a data error freed the table twice
	Put( zip_bad_tree, zip_bad_tree_len );
	Walk();
	Walk();

	// a good file loads as a pack, with every name
	{
		pack_t *pack;
		Put( zip_seed, zip_seed_len );
		pack = FS_LoadZipFile( path, "test.pk3" );
		if ( !pack || pack->numfiles != 4 || !pack->buildBuffer[3].name[0] ) { fprintf( stderr, "FAIL: good pack\n" ); return 1; }
		unzClose( pack->handle );
		Z_Free( pack->buildBuffer );
		Z_Free( pack );
	}

	// a stored entry that declares more than it holds: the first entry is stored
	memcpy( m, zip_seed, zip_seed_len );
	i = Find( m, zip_seed_len, 0x02014b50 );
	m[i + 25]++;
	m[Find( m, zip_seed_len, 0x04034b50 ) + 23]++;
	Put( m, zip_seed_len );
	Walk();

	// a name longer than the loader's buffer is not left unterminated
	memcpy( m, zip_seed, zip_seed_len );
	i = Find( m, zip_seed_len, 0x02014b50 );
	m[i + 29] = 2;
	Put( m, zip_seed_len );
	if ( FS_LoadZipFile( path, "test.pk3" ) ) { fprintf( stderr, "FAIL: long name\n" ); return 1; }

	// more entries claimed than the directory holds
	memcpy( m, zip_seed, zip_seed_len );
	i = Find( m, zip_seed_len, 0x06054b50 );
	m[i + 8] = 200; m[i + 10] = 200;
	Put( m, zip_seed_len );
	if ( FS_LoadZipFile( path, "test.pk3" ) ) { fprintf( stderr, "FAIL: false entry count\n" ); return 1; }

	srand( 7 );
	for ( r = 0; r < 60000; r++ ) {
		int tail = zip_seed_len > 200 ? zip_seed_len - 200 : 0;
		memcpy( m, zip_seed, zip_seed_len );
		for ( k = 1 + rand() % 6; k > 0; k-- ) {
			int at = rand() & 1 ? rand() % zip_seed_len : tail + rand() % ( zip_seed_len - tail );
			m[at] = rand() & 1 ? rand() : 0xff;
		}
		Put( m, zip_seed_len );
		Walk();
		{
			pack_t *pack = FS_LoadZipFile( path, "test.pk3" );
			if ( pack ) {
				unzClose( pack->handle );
				Z_Free( pack->buildBuffer );
				Z_Free( pack );
			}
		}
	}
	unlink( path );
	printf( "zip tests passed\n" );
	return 0;
}
