// The key binding functions from cl_keys.c: a key number from a module or a
// cfg indexes keys[] only inside it. legacy's ui asks Key_IsDown for its
// K_RSHIFT, 300 in ET: Legacy's keycodes, and a one-byte key name past 0x7f is
// negative with -fsigned-char.
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/client/keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void QDECL Com_Error( int level, const char *fmt, ... ) { (void)level; fprintf( stderr, "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void Com_Memset( void *d, const int v, const size_t n ) { memset( d, v, n ); }
void Z_Free( void *p ) { free( p ); }
char *CopyString( const char *s ) { char *d = malloc( strlen( s ) + 1 ); strcpy( d, s ); return d; }
int cvar_modifiedFlags;
static int argc;
static const char *argv[4];
int Cmd_Argc( void ) { return argc; }
char *Cmd_Argv( int i ) { return (char *)( i < argc ? argv[i] : "" ); }

// keys[] in the middle, so a write past either end shows in a neighbour
static qkey_t before[MAX_KEYS];
qkey_t keys[MAX_KEYS];
static qkey_t after[MAX_KEYS];
typedef struct { char *name; int keynum; } keyname_t;
static keyname_t keynames[] = { { "SHIFT", K_SHIFT }, { NULL, 0 } };
#include KEYS_INC

static int fails;
static void check( int ok, const char *what ) {
	if ( !ok ) { fprintf( stderr, "FAIL: %s\n", what ); fails++; }
}
static void cmd( int n, const char *a0, const char *a1, const char *a2 ) {
	argc = n; argv[0] = a0; argv[1] = a1; argv[2] = a2;
}
static int untouched( void ) {
	static const qkey_t zero;
	int i;
	for ( i = 0; i < MAX_KEYS; i++ ) {
		if ( memcmp( &before[i], &zero, sizeof( zero ) ) || memcmp( &after[i], &zero, sizeof( zero ) ) ) {
			return 0;
		}
	}
	return 1;
}

int main( void ) {
	static const int bad[] = { 300, MAX_KEYS, -2, -23, -128, 0x7fffffff, -0x7fffffff - 1 };
	int i;

	keys['a'].down = qtrue;
	keys[K_SHIFT].down = qtrue;
	check( Key_IsDown( 'a' ) && Key_IsDown( K_SHIFT ) && !Key_IsDown( 'b' ) && !Key_IsDown( -1 ), "Key_IsDown in range" );
	for ( i = 0; i < (int)( sizeof( bad ) / sizeof( bad[0] ) ); i++ ) {
		check( !Key_IsDown( bad[i] ), "Key_IsDown out of range" );
		Key_SetBinding( bad[i], "+attack" );
		check( !strcmp( Key_GetBinding( bad[i] ), "" ), "Key_GetBinding out of range" );
	}
	check( untouched(), "Key_SetBinding out of range wrote past keys[]" );

	cmd( 3, "bind", "a", "+forward" );
	Key_Bind_f();
	check( keys['a'].binding && !strcmp( Key_GetBinding( 'a' ), "+forward" ), "bind a" );
	cmd( 3, "bind", "SHIFT", "+speed" );
	Key_Bind_f();
	check( !strcmp( Key_GetBinding( K_SHIFT ), "+speed" ), "bind SHIFT" );
	cmd( 3, "bind", "0xff", "+back" );
	Key_Bind_f();
	check( !strcmp( Key_GetBinding( 0xff ), "+back" ), "bind 0xff" );
	// a one-byte name past 0x7f: Key_StringToKeynum gives a negative number
	cmd( 3, "bind", "\xe9", "+attack" );
	Key_Bind_f();
	cmd( 2, "bind", "\xe9", NULL );
	Key_Bind_f();
	cmd( 2, "unbind", "\xe9", NULL );
	Key_Unbind_f();
	check( untouched(), "bind of a byte past 0x7f wrote past keys[]" );
	cmd( 2, "unbind", "a", NULL );
	Key_Unbind_f();
	check( !strcmp( Key_GetBinding( 'a' ), "" ), "unbind a" );
	if ( fails ) {
		return 1;
	}
	printf( "key binding tests passed\n" );
	return 0;
}
