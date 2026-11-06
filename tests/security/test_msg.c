/* Bounds of the message layer against truncated and hostile input. Run under
 * ASan and UBSan by tests/security/run.sh: an overread fails the sanitizer, a
 * wrong value fails an assert. */
#include "msg_stubs.h"
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"

jmp_buf test_drop;
int test_drops;
cvar_t *cl_shownet;

void QDECL Com_Error( int code, const char *fmt, ... ) {
	test_drops++;
	longjmp( test_drop, 1 );
}
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
qboolean CL_CheckPaused( void ) { return qfalse; }
cvar_t *Cvar_Get( const char *a, const char *b, int c ) { return NULL; }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

#define CHECK( x ) do { if ( !( x ) ) { printf( "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); exit( 1 ); } } while ( 0 )

/* heap buffers of exact size, so ASan sees one byte past the end */
static void msg_of( msg_t *m, byte *d, int size ) {
	MSG_Init( m, d, size );
	m->cursize = size;
	MSG_BeginReading( m );
}

static void test_oob_truncated( void ) {
	int len;

	for ( len = 0; len < 6; len++ ) {
		byte *d = malloc( len ? len : 1 );
		msg_t m;
		int v;

		memset( d, 0x41, len ? len : 1 );
		MSG_Init( &m, d, len );
		m.cursize = len;
		MSG_BeginReadingOOB( &m );
		v = MSG_ReadLong( &m );
		CHECK( len >= 4 ? v == 0x41414141 : v == -1 );
		v = MSG_ReadShort( &m );
		CHECK( len >= 6 ? v == 0x4141 : v == -1 );
		free( d );
	}
}

static void test_oob_values( void ) {
	byte d[8] = { 0x80, 0xff, 0x01, 0x02, 0x03, 0x84, 0, 0 };
	msg_t m;

	MSG_Init( &m, d, sizeof( d ) );
	m.cursize = sizeof( d );
	MSG_BeginReadingOOB( &m );
	CHECK( MSG_ReadShort( &m ) == (short)0xff80 );
	CHECK( (unsigned int)MSG_ReadLong( &m ) == 0x84030201u );
}

static void test_huff_truncated_everywhere( void ) {
	int len, seed;

	/* every short random buffer through every reader: no overread, ever */
	for ( len = 1; len < 64; len++ ) {
		for ( seed = 0; seed < 200; seed++ ) {
			byte *d = malloc( len );
			msg_t m;
			int i;

			srand( len * 1000 + seed );
			for ( i = 0; i < len; i++ ) {
				d[i] = rand();
			}
			msg_of( &m, d, len );
			if ( !setjmp( test_drop ) ) {
				for ( i = 0; i < 400 && m.readcount <= m.cursize; i++ ) {
					switch ( i % 4 ) {
					case 0: MSG_ReadBits( &m, 7 ); break;
					case 1: MSG_ReadLong( &m ); break;
					case 2: MSG_ReadString( &m ); break;
					case 3: MSG_ReadByte( &m ); break;
					}
				}
			}
			CHECK( MSG_ReadByte( &m ) == -1 || m.readcount <= m.cursize );
			free( d );
		}
	}
}

static void test_write_exact_capacity( void ) {
	int size;

	for ( size = 1; size < 64; size++ ) {
		byte *d = malloc( size );
		msg_t m;
		int i;

		MSG_Init( &m, d, size );
		for ( i = 0; i < 200; i++ ) {
			MSG_WriteLong( &m, 0x12345678 + i );
		}
		CHECK( m.overflowed );
		free( d );

		d = malloc( size );
		MSG_InitOOB( &m, d, size );
		for ( i = 0; i < 200; i++ ) {
			MSG_WriteLong( &m, i );
		}
		CHECK( m.overflowed || size % 4 == 0 );
		CHECK( m.cursize <= size );
		free( d );
	}
}

static void test_roundtrip( void ) {
	static byte d[4096];
	msg_t m;
	int i, n;

	MSG_Init( &m, d, sizeof( d ) );
	MSG_WriteBits( &m, 5, 3 );
	MSG_WriteBits( &m, -3, -16 );
	MSG_WriteLong( &m, -123456 );
	MSG_WriteShort( &m, -2 );
	MSG_WriteString( &m, "cs 12 \"hi\"" );
	for ( i = 0; i < 300; i++ ) {
		MSG_WriteByte( &m, i & 0xff );
	}
	CHECK( !m.overflowed );
	n = m.cursize;
	MSG_BeginReading( &m );
	m.cursize = n;
	CHECK( MSG_ReadBits( &m, 3 ) == 5 );
	CHECK( MSG_ReadBits( &m, -16 ) == -3 );
	CHECK( MSG_ReadLong( &m ) == -123456 );
	CHECK( MSG_ReadShort( &m ) == -2 );
	CHECK( !strcmp( MSG_ReadString( &m ), "cs 12 \"hi\"" ) );
	for ( i = 0; i < 300; i++ ) {
		CHECK( MSG_ReadByte( &m ) == ( i & 0xff ) );
	}
}

static void test_bad_field_counts( void ) {
	static byte d[256];
	msg_t m;
	entityState_t from, to;
	playerState_t pfrom, pto;
	int drops;

	memset( &from, 0, sizeof( from ) );
	memset( &pfrom, 0, sizeof( pfrom ) );

	/* field count past the table */
	MSG_Init( &m, d, sizeof( d ) );
	MSG_WriteBits( &m, 0, 1 );
	MSG_WriteBits( &m, 1, 1 );
	MSG_WriteByte( &m, 255 );
	MSG_BeginReading( &m );
	drops = test_drops;
	if ( !setjmp( test_drop ) ) {
		MSG_ReadDeltaEntity( &m, &from, &to, 5 );
	}
	CHECK( test_drops == drops + 1 );

	/* field count missing: the read returns -1 */
	MSG_Init( &m, d, sizeof( d ) );
	MSG_WriteBits( &m, 0, 1 );
	MSG_WriteBits( &m, 1, 1 );
	m.cursize = 1;
	MSG_BeginReading( &m );
	m.cursize = 1;
	drops = test_drops;
	if ( !setjmp( test_drop ) ) {
		MSG_ReadDeltaEntity( &m, &from, &to, 5 );
	}
	CHECK( test_drops == drops + 1 );

	MSG_Init( &m, d, sizeof( d ) );
	MSG_WriteByte( &m, 255 );
	MSG_BeginReading( &m );
	drops = test_drops;
	if ( !setjmp( test_drop ) ) {
		MSG_ReadDeltaPlayerstate( &m, &pfrom, &pto );
	}
	CHECK( test_drops == drops + 1 );
}

int main( void ) {
	Swap_Init();
	MSG_initHuffman();
	test_oob_values();
	test_oob_truncated();
	test_roundtrip();
	test_write_exact_capacity();
	test_huff_truncated_everywhere();
	test_bad_field_counts();
	printf( "message bounds: all passed\n" );
	return 0;
}
