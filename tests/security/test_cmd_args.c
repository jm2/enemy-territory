/* Cmd_Args and Cmd_ArgsFrom (src/qcommon/cmd.c) joined the tokens with
 * strcat into static buffers of MAX_STRING_CHARS and BIG_INFO_STRING, but
 * Cmd_TokenizeString takes up to BIG_INFO_STRING, so a long line overran
 * them (CVE-2019-1010043; Quake III #573). SV_ConSay_f (sv_ccmds.c) put
 * "console: " and Cmd_Args() into 1024 bytes the same way, and CL_Setenv_f
 * (cl_main.c) joined its arguments unbounded into 1024. Cvar_Set_f
 * (cvar.c) undercounted its words into combined[MAX_STRING_TOKENS], and
 * CL_Rcon_f (cl_main.c) appended the password and the line into 1024. The
 * real functions are extracted by run.sh into CMD_ARGS_INC. */
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct client_s client_t;
static cvar_t running;
cvar_t *com_sv_running = &running;
static char sent[BIG_INFO_STRING * 2];
static int sends;
static char env[BIG_INFO_STRING * 2];

#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_Error( int level, const char *fmt, ... ) {
	(void)level; fprintf( stderr, "Com_Error: %s\n", fmt ); exit( 1 );
}
void QDECL SV_SendServerCommand( client_t *cl, const char *fmt, ... ) {
	va_list ap;
	(void)cl;
	va_start( ap, fmt );
	vsnprintf( sent, sizeof( sent ), fmt, ap );
	va_end( ap );
	sends++;
}
static int TestPutenv( const char *str ) {
	Q_strncpyz( env, str, sizeof( env ) );
	return 0;
}
#undef Q_putenv
#define Q_putenv TestPutenv

static char setValue[BIG_INFO_STRING * 2];
cvar_t *com_crashed;
cvar_t *Cvar_Set2( const char *var_name, const char *value, qboolean force ) {
	(void)var_name; (void)force;
	Q_strncpyz( setValue, value, sizeof( setValue ) );
	return NULL;
}
static cvar_t rconPassword, rconAddr;
cvar_t *rcon_client_password = &rconPassword, *rconAddress = &rconAddr;
netadr_t cl_rconTarget;
static struct { connstate_t state; } cls;
static struct { struct { netadr_t remoteAddress; } netchan; } clc;
static char packet[BIG_INFO_STRING * 2];
static int packetLength;
void NET_SendPacket( netsrc_t sock, int length, const void *data, netadr_t to ) {
	(void)sock; (void)to;
	packetLength = length;
	memcpy( packet, data, length < (int)sizeof( packet ) ? length : (int)sizeof( packet ) );
}
qboolean NET_StringToAdr( const char *s, netadr_t *a ) { (void)s; memset( a, 0, sizeof( *a ) ); return qtrue; }

typedef struct cmd_function_s cmd_function_t;
#include CMD_ARGS_INC

static int failures;
static void Check( int ok, const char *what ) {
	if ( !ok ) {
		fprintf( stderr, "cmd args regression failed: %s\n", what );
		failures++;
	}
}

/* "cmd" then words of 'x' separated by single spaces, total length n */
static void LongLine( char *line, int size, const char *cmd, int n ) {
	int i;

	Q_strncpyz( line, cmd, size );
	for ( i = strlen( line ) ; i < n && i < size - 1 ; i++ ) {
		line[i] = ( i % 64 == 63 ) ? ' ' : 'x';
	}
	line[i] = 0;
}

int main( void ) {
	static char line[BIG_INFO_STRING + 16];
	char *args;

	/* short lines are unchanged */
	Cmd_TokenizeString( "say hello  there" );
	Check( !strcmp( Cmd_Args(), "hello there" ), "Cmd_Args of a short line" );
	Check( !strcmp( Cmd_ArgsFrom( 2 ), "there" ), "Cmd_ArgsFrom of a short line" );

	/* the longest line the tokenizer takes: Cmd_Args stops at its buffer */
	LongLine( line, sizeof( line ), "cmd ", BIG_INFO_STRING - 1 );
	Cmd_TokenizeString( line );
	Check( Cmd_Argc() > 2, "the long line tokenizes" );
	args = Cmd_Args();
	Check( strlen( args ) == MAX_STRING_CHARS - 1, "Cmd_Args stops at MAX_STRING_CHARS - 1" );
	Check( !strncmp( args, "xxxx", 4 ), "and keeps its start" );
	args = Cmd_ArgsFrom( 0 );
	Check( strlen( args ) < BIG_INFO_STRING, "Cmd_ArgsFrom stays in BIG_INFO_STRING" );
	Check( !strncmp( args, "cmd xxx", 7 ), "Cmd_ArgsFrom keeps its start" );

	/* SV_ConSay_f */
	running.integer = 1;
	sends = 0;
	Cmd_TokenizeString( "say hello there" );
	SV_ConSay_f();
	Check( sends == 1 && !strcmp( sent, "chat \"console: hello there\"" ), "a short say is unchanged" );
	Cmd_TokenizeString( "say \"quoted words\"" );
	SV_ConSay_f();
	Check( !strcmp( sent, "chat \"console: quoted words\"" ), "a quoted say loses its quotes" );
	LongLine( line, sizeof( line ), "say ", 4 + MAX_STRING_CHARS - 1 );
	Cmd_TokenizeString( line );
	SV_ConSay_f();
	Check( strlen( sent ) == strlen( "chat \"\"" ) + 1023, "a long say stops at the 1024-byte buffer" );
	Check( !strncmp( sent, "chat \"console: xxx", 18 ), "and keeps its start" );

	/* CL_Setenv_f */
	Cmd_TokenizeString( "setenv NAME a b" );
	CL_Setenv_f();
	Check( !strcmp( env, "NAME=a b " ), "a short setenv is unchanged" );
	LongLine( line, sizeof( line ), "setenv NAME ", BIG_INFO_STRING - 1 );
	Cmd_TokenizeString( line );
	CL_Setenv_f();
	Check( strlen( env ) == 1023 && !strncmp( env, "NAME=xxx", 8 ), "a long setenv stops at its 1024-byte buffer" );

	/* Cvar_Set_f */
	Cmd_TokenizeString( "set v a b  c" );
	Cvar_Set_f();
	Check( !strcmp( setValue, "a b c" ), "a short set is unchanged" );
	{
		int i, n = strlen( "set v" );
		Q_strncpyz( line, "set v", sizeof( line ) );
		for ( i = 0 ; i < 254 ; i++ ) {
			line[n++] = ' ';
			line[n++] = 'a';
		}
		line[n] = 0;
	}
	Cmd_TokenizeString( line );
	Cvar_Set_f();
	Check( strlen( setValue ) < MAX_STRING_TOKENS && !strncmp( setValue, "a a a", 5 ), "a set of 254 short words stays in combined" );

	/* CL_Rcon_f */
	cls.state = CA_CONNECTED;
	rconPassword.string = "secret";
	Cmd_TokenizeString( "rcon status" );
	CL_Rcon_f();
	Check( packetLength == 4 + (int)strlen( "rcon secret status" ) + 1 && !memcmp( packet + 4, "rcon secret status", 19 ), "a short rcon is unchanged" );
	LongLine( line, sizeof( line ), "rcon ", MAX_STRING_CHARS - 1 );
	Cmd_TokenizeString( line );
	CL_Rcon_f();
	Check( packetLength == 1024 && !memcmp( packet + 4, "rcon secret xxx", 15 ), "a long rcon stops at its 1024-byte message" );

	if ( failures ) {
		return 1;
	}
	puts( "cmd args: passed" );
	return 0;
}
