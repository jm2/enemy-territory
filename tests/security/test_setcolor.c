// RB_SetColor from tr_backend.c: a module's color, any float, becomes a byte
// without an undefined conversion, and the bytes both targets gave are kept.
#include <math.h>
#include <stdio.h>
typedef unsigned char byte;
typedef struct { int commandId; float color[4]; } setColorCommand_t;
static struct { byte color2D[4]; } backEnd;
#include SETCOLOR_INC

static int fails;
static void check( float c, int want ) {
	setColorCommand_t cmd = { 0, { c, c, c, c } };
	const void *next = RB_SetColor( &cmd );
	if ( next != &cmd + 1 || backEnd.color2D[0] != want || backEnd.color2D[3] != want ) {
		fprintf( stderr, "FAIL: %g gave %d, want %d\n", c, backEnd.color2D[0], want );
		fails++;
	}
}

int main( void ) {
	check( 0, 0 );
	check( 0.5f, 127 );
	check( 1, 255 );
	check( 254.84f, 216 );      // legacy's health bar: 64984 kept to its low byte
	check( -0.5f, 129 );        // -127 kept to its low byte
	check( 8421504.0f, 128 );   // 2147483520 * 255 / 255, still an int
	check( 1e30f, 255 );
	check( -1e30f, 0 );
	check( INFINITY, 255 );
	check( -INFINITY, 0 );
	check( NAN, 0 );
	if ( fails ) {
		return 1;
	}
	printf( "set color tests passed\n" );
	return 0;
}
