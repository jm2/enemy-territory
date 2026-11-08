/* Q_rsqrt (src/game/q_math.c) and the collision code's SquareRootFloat
 * (src/qcommon/cm_trace.c, for capsule traces) reinterpret a float's bits as
 * an integer. They read them through a long, which is 64 bits on LP64 hosts:
 * the read took four bytes past the float, and the result depended on them.
 * The Mac target's long is 32 bits, so the int read is bit-identical there.
 * One Newton step leaves a relative error under 0.18 %, two well under
 * 0.001 %. Run under the sanitizers: UBSan's object-size check reports the 8-byte read of a 4-byte float.
 * Adapted from Quake III's tests/q_rsqrt_regression.c (019a396b). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/game/q_shared.h"

void QDECL Com_Error( int level, const char *fmt, ... ) { fprintf( stderr, "Com_Error: %s\n", fmt ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

/* SquareRootFloat verbatim: cm_trace.c needs the whole collision map */
#include SQRTF_INC

int main( void ) {
	static const float values[] = { 1e-6f, 0.01f, 0.25f, 1.0f, 2.0f, 4.0f, 10.0f, 12345.0f, 1e6f, 3e30f };
	int i;

	for ( i = 0; i < (int)( sizeof( values ) / sizeof( values[0] ) ); i++ ) {
		volatile float x = values[i];
		double want = 1.0 / sqrt( x );
		double got = Q_rsqrt( x );
		if ( fabs( got - want ) > 0.0018 * want ) {
			printf( "FAIL: Q_rsqrt(%g) = %g, want %g\n", x, got, want );
			return 1;
		}
		want = sqrt( x );
		got = SquareRootFloat( x );
		if ( fabs( got - want ) > 0.00001 * want ) {
			printf( "FAIL: SquareRootFloat(%g) = %g, want %g\n", x, got, want );
			return 1;
		}
	}
	printf( "rsqrt: Q_rsqrt and SquareRootFloat read 32 bits\n" );
	return 0;
}
