// CL_AddReliableCommand from cl_main.c: no command may take the slot of one
// that is not acknowledged, and CL_Disconnect's guard must leave room for its own.
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
static struct {
	int reliableSequence, reliableAcknowledge;
	char reliableCommands[MAX_RELIABLE_COMMANDS][MAX_STRING_CHARS];
} clc;
static jmp_buf dropped;
void QDECL Com_Error( int level, const char *fmt, ... ) { (void)level; (void)fmt; longjmp( dropped, 1 ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void Com_Memset( void *d, const int v, const size_t n ) { memset( d, v, n ); }
#include RELIABLE_INC

int main( void ) {
	int i, ok;
	char name[32];

	clc.reliableSequence = clc.reliableAcknowledge = 1000;
	for ( i = 0; i < MAX_RELIABLE_COMMANDS; i++ ) {
		Com_sprintf( name, sizeof( name ), "cmd%d", i );
		if ( setjmp( dropped ) ) { fprintf( stderr, "FAIL: dropped at %d of %d\n", i + 1, MAX_RELIABLE_COMMANDS ); return 1; }
		CL_AddReliableCommand( name );
	}
	// every unacknowledged command is still in its own slot
	for ( i = clc.reliableAcknowledge + 1; i <= clc.reliableSequence; i++ ) {
		Com_sprintf( name, sizeof( name ), "cmd%d", i - clc.reliableAcknowledge - 1 );
		if ( strcmp( clc.reliableCommands[i & ( MAX_RELIABLE_COMMANDS - 1 )], name ) ) {
			fprintf( stderr, "FAIL: command %d overwritten\n", i ); return 1;
		}
	}
	// CL_Disconnect's own guard, taken from cl_main.c, says there is no room
	if ( DISCONNECT_GUARD ) { fprintf( stderr, "FAIL: CL_Disconnect would queue into a full queue\n" ); return 1; }
	ok = 0;
	if ( setjmp( dropped ) ) {
		ok = 1;
	} else {
		CL_AddReliableCommand( "one too many" );
	}
	if ( !ok ) { fprintf( stderr, "FAIL: command %d taken\n", MAX_RELIABLE_COMMANDS + 1 ); return 1; }
	if ( strcmp( clc.reliableCommands[( clc.reliableAcknowledge + 1 ) & ( MAX_RELIABLE_COMMANDS - 1 )], "cmd0" ) ) {
		fprintf( stderr, "FAIL: oldest overwritten\n" ); return 1;
	}
	// one acknowledged makes room for one, and CL_Disconnect sees it
	clc.reliableAcknowledge++;
	if ( !( DISCONNECT_GUARD ) ) { fprintf( stderr, "FAIL: CL_Disconnect would not queue with room\n" ); return 1; }
	if ( setjmp( dropped ) ) { fprintf( stderr, "FAIL: no room after an ack\n" ); return 1; }
	CL_AddReliableCommand( "after ack" );
	printf( "reliable command tests passed\n" );
	return 0;
}
