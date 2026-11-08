/* Server client-state handling, with the real functions run.sh extracts from
 * sv_client.c, sv_init.c and sv_main.c (SERVER_INC):
 * - configstring changes for a client still loading are marked, not queued,
 *   and sent once at their newest value when it enters the world (csUpdated);
 * - nextdl for a block the server has not read, or with no download, is ignored;
 * - donedl from a client in the world sends no gamestate, unless map_restart
 *   made it active while it was still downloading (gamestateOwed);
 *   and map_restart's loop, a packet on the current serverId, and a usercmd
 *   after "vdr" keep to that (SV_MapRestart_f, SV_ExecuteClientMessage, SV_UserMove);
 * - the engine's copy of a name keeps no control characters.
 * Run under ASan and UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../src/server/server.h"

#define CHECK( x ) do { if ( !( x ) ) { printf( "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); exit( 1 ); } } while ( 0 )

serverStatic_t svs;
server_t sv;
vm_t *gvm;
cvar_t *sv_maxclients, *com_dedicated, *sv_lanForceRate, *cl_shownet, *sv_pure, *com_sv_running;
static cvar_t maxclients, dedicated, lanForceRate, pure, svRunning;
int com_frameTime;
gameInfo_t com_gameInfo;

void QDECL Com_Error( int code, const char *fmt, ... ) { printf( "FAIL: Com_Error %s\n", fmt ); exit( 1 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void QDECL Com_DPrintf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
qboolean CL_CheckPaused( void ) { return qfalse; }
cvar_t *Cvar_Get( const char *a, const char *b, int c ) { return NULL; }
char *CopyString( const char *in ) { return strdup( in ); }
void Z_Free( void *p ) { free( p ); }
void FS_FCloseFile( fileHandle_t f ) {}
qboolean Sys_IsLANAddress( netadr_t adr ) { return qfalse; }
qboolean NET_IsLocalAddress( netadr_t adr ) { return qtrue; }
const char *NET_AdrToString( netadr_t a ) { return "localhost"; }
qboolean SV_GameIsSinglePlayer( void ) { return qfalse; }
qboolean SV_GameIsCoop( void ) { return qfalse; }

static const char *argv1 = "";
static int argc = 2;
char *Cmd_Argv( int arg ) { return arg == 1 ? (char *)argv1 : "nextdl"; }
int Cmd_Argc( void ) { return argc; }
int Com_HashKey( char *string, int maxlen ) { return 0; }
void Cvar_Set( const char *name, const char *value ) {}
char *Cvar_VariableString( const char *name ) { return ""; }
int Cvar_VariableIntegerValue( const char *name ) { return 0; }
int FS_ReadFile( const char *qpath, void **buffer ) { return -1; }
void Hunk_FreeTempMemory( void *buf ) {}
void SV_SpawnServer( char *server, qboolean killBots ) { printf( "FAIL: SV_SpawnServer\n" ); exit( 1 ); }
void *VM_ExplicitArgPtr( vm_t *vm, int intValue ) { return NULL; }
static int thinks;
void SV_ClientThink( client_t *cl, usercmd_t *cmd ) { thinks++; }
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg, qboolean premaprestart ) {
	MSG_ReadLong( msg );
	MSG_ReadString( msg );
	return qtrue;
}
static void SV_ParseBinaryMessage( client_t *cl, msg_t *msg ) {}
static qboolean SV_TransitionGameState( gamestate_t new_gs, gamestate_t old_gs, int delay ) { return qtrue; }

static const char *dropReason[MAX_CLIENTS];
void SV_DropClient( client_t *drop, const char *reason ) {
	dropReason[drop - svs.clients] = reason;
	drop->state = CS_ZOMBIE;
}
static int gamestates;
void SV_UpdateServerCommandsToClient( client_t *client, msg_t *msg ) {}
void SV_SendMessageToClient( msg_t *msg, client_t *client ) {
	gamestates++;
	client->netchan.outgoingSequence++;
}
static sharedEntity_t entities[MAX_CLIENTS];
sharedEntity_t *SV_GentityNum( int num ) { return &entities[num]; }
void SV_SetConfigstring( int index, const char *val );
void SV_RestartGameProgs( void ) {
	/* the game sets its warmup configstring as it restarts */
	SV_SetConfigstring( CS_WARMUP, va( "%i", com_frameTime ) );
}
static int begins[MAX_CLIENTS];
int QDECL VM_Call( vm_t *vm, int callNum, ... ) {
	va_list ap;
	int n;
	va_start( ap, callNum );
	n = va_arg( ap, int );
	va_end( ap );
	if ( callNum == GAME_CLIENT_BEGIN ) {
		begins[n]++;
	}
	return 0;
}

#include SERVER_INC

#define CLIENTS 3
enum { ACTIVE, PRIMED, CONNECTED };

static void Reset( void ) {
	int i;
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		free( sv.configstrings[i] );
		sv.configstrings[i] = CopyString( "" );
		sv.configstringsmodified[i] = qfalse;
	}
	memset( svs.clients, 0, sizeof( client_t ) * CLIENTS );
	memset( entities, 0, sizeof( entities ) );
	memset( begins, 0, sizeof( begins ) );
	memset( dropReason, 0, sizeof( dropReason ) );
	svs.clients[ACTIVE].state = CS_ACTIVE;
	svs.clients[ACTIVE].gentity = &entities[ACTIVE];
	svs.clients[PRIMED].state = CS_PRIMED;
	svs.clients[CONNECTED].state = CS_CONNECTED;
	sv.state = SS_GAME;
	sv.restarting = qfalse;
	gamestates = 0;
}

static const char *Cmd( client_t *cl, int seq ) {
	return cl->reliableCommands[seq & ( MAX_RELIABLE_COMMANDS - 1 )];
}

/* Changes while a client loads: no overflow, each sent once on entering the world. */
static void Configstrings( void ) {
	client_t *loading = &svs.clients[PRIMED], *active = &svs.clients[ACTIVE];
	usercmd_t cmd;
	char expected[64], big[2600];
	int i, k, last = 2 * MAX_RELIABLE_COMMANDS;

	Reset();
	for ( k = 0; k <= last; k++ ) {
		SV_SetConfigstring( CS_PLAYERS + k % 3, va( "n\\player %i", k ) );
		SV_UpdateConfigStrings();
		active->reliableAcknowledge = active->reliableSequence;    /* a player in the game acks */
	}
	CHECK( !dropReason[PRIMED] );    /* master: "Server command overflow" */
	CHECK( loading->state == CS_PRIMED && loading->reliableSequence == 0 );
	CHECK( loading->csUpdated[CS_PLAYERS] && loading->csUpdated[CS_PLAYERS + 1] && loading->csUpdated[CS_PLAYERS + 2] );
	CHECK( !loading->csUpdated[CS_PLAYERS + 3] );
	CHECK( active->reliableSequence == last + 1 && !dropReason[ACTIVE] );
	CHECK( svs.clients[CONNECTED].reliableSequence == 0 && !svs.clients[CONNECTED].csUpdated[CS_PLAYERS] );

	memset( &cmd, 0, sizeof( cmd ) );
	SV_ClientEnterWorld( loading, &cmd );
	CHECK( loading->state == CS_ACTIVE && begins[PRIMED] == 1 && loading->reliableSequence == 3 );
	for ( i = 0; i < 3; i++ ) {
		/* in index order, each at its newest value */
		Com_sprintf( expected, sizeof( expected ), "cs %i \"n\\player %i\"\n", CS_PLAYERS + i, last - ( last - i ) % 3 );
		CHECK( !strcmp( Cmd( loading, i + 1 ), expected ) );
	}
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		CHECK( !loading->csUpdated[i] );
	}

	/* a big configstring goes in bcs chunks, the same way to both */
	Reset();
	for ( i = 0; i < (int)sizeof( big ) - 1; i++ ) {
		big[i] = 'a' + i % 26;
	}
	big[sizeof( big ) - 1] = 0;
	SV_SetConfigstring( CS_PLAYERS, big );
	SV_UpdateConfigStrings();
	CHECK( loading->reliableSequence == 0 && loading->csUpdated[CS_PLAYERS] );
	SV_ClientEnterWorld( loading, &cmd );
	CHECK( active->reliableSequence == 3 && loading->reliableSequence == 3 );
	CHECK( !strncmp( Cmd( loading, 1 ), "bcs0 ", 5 ) && !strncmp( Cmd( loading, 2 ), "bcs1 ", 5 ) && !strncmp( Cmd( loading, 3 ), "bcs2 ", 5 ) );
	for ( i = 1; i <= 3; i++ ) {
		CHECK( !strcmp( Cmd( loading, i ), Cmd( active, i ) ) );
	}

	/* SVF_NOSERVERINFO still holds for the deferred serverinfo */
	Reset();
	entities[PRIMED].r.svFlags = SVF_NOSERVERINFO;
	loading->gentity = &entities[PRIMED];
	SV_SetConfigstring( CS_SERVERINFO, "\\sv_hostname\\x" );
	SV_UpdateConfigStrings();
	CHECK( active->reliableSequence == 1 && loading->csUpdated[CS_SERVERINFO] );
	SV_ClientEnterWorld( loading, &cmd );
	CHECK( loading->reliableSequence == 0 && !loading->csUpdated[CS_SERVERINFO] );

	/* a gamestate holds every configstring as it is, so it clears the marks */
	Reset();
	SV_SetConfigstring( CS_PLAYERS, "n\\a" );
	SV_UpdateConfigStrings();
	CHECK( loading->csUpdated[CS_PLAYERS] );
	SV_SendClientGameState( loading );
	CHECK( !loading->csUpdated[CS_PLAYERS] && gamestates == 1 );
	SV_ClientEnterWorld( loading, &cmd );
	CHECK( loading->reliableSequence == 0 );
	puts( "server state: configstrings for a loading client deferred" );
}

static void Nextdl( client_t *cl, int block ) {
	char buf[16];
	Com_sprintf( buf, sizeof( buf ), "%i", block );
	argv1 = buf;
	SV_NextDownload_f( cl );
}

/* Acknowledges stop at the blocks the server has read. */
static void Download( void ) {
	client_t *cl = &svs.clients[CONNECTED];
	int i;

	Reset();
	Q_strncpyz( cl->downloadName, "maps.pk3", sizeof( cl->downloadName ) );
	for ( i = 0; i < MAX_DOWNLOAD_WINDOW; i++ ) {
		cl->downloadBlockSize[i] = MAX_DOWNLOAD_BLKSIZE;
	}
	cl->downloadCurrentBlock = 2;    /* blocks 0 and 1 read and sent */
	for ( i = 0; i < 100; i++ ) {
		Nextdl( cl, i + 2 );        /* chained acks ahead of what was read */
	}
	Nextdl( cl, 2 );
	CHECK( cl->downloadClientBlock == 0 && !dropReason[CONNECTED] );    /* master: broken download */
	Nextdl( cl, 0 );
	Nextdl( cl, 1 );
	CHECK( cl->downloadClientBlock == 2 && cl->state == CS_CONNECTED );
	Nextdl( cl, 2 );
	CHECK( cl->downloadClientBlock == 2 && cl->state == CS_CONNECTED );
	/* the empty end-of-file block, once read, ends the download */
	cl->downloadBlockSize[2] = 0;
	cl->downloadCurrentBlock = 3;
	Nextdl( cl, 2 );
	CHECK( !cl->downloadName[0] && cl->state == CS_CONNECTED );
	/* no download: ignored */
	Nextdl( cl, 2 );
	Nextdl( cl, 0 );
	CHECK( cl->state == CS_CONNECTED && !dropReason[CONNECTED] );
	/* an ack for a block already acknowledged still drops */
	Q_strncpyz( cl->downloadName, "maps.pk3", sizeof( cl->downloadName ) );
	cl->downloadClientBlock = 1;
	cl->downloadCurrentBlock = 2;
	Nextdl( cl, 0 );
	CHECK( cl->state == CS_ZOMBIE && dropReason[CONNECTED] && !strcmp( dropReason[CONNECTED], "broken download" ) );
	puts( "server state: nextdl past the blocks read ignored" );
}

/* donedl gets a gamestate only to a client not yet in the world. */
static void Donedl( void ) {
	client_t *cl = &svs.clients[ACTIVE];
	usercmd_t cmd;
	int before;

	memset( &cmd, 0, sizeof( cmd ) );
	Reset();
	cl->netchan.outgoingSequence = 10;
	cl->gamestateMessageNum = 5;
	SV_DoneDownload_f( cl );
	CHECK( cl->state == CS_ACTIVE && gamestates == 0 && cl->gamestateMessageNum == 5 );    /* master: a gamestate */

	/* a client still downloading gets it */
	SV_DoneDownload_f( &svs.clients[CONNECTED] );
	CHECK( svs.clients[CONNECTED].state == CS_PRIMED && gamestates == 1 );

	/* map_restart made it active while it was downloading: it is owed one */
	cl = &svs.clients[CONNECTED];
	cl->state = CS_CONNECTED;
	SV_ClientMapRestarted( cl );
	CHECK( cl->state == CS_ACTIVE && cl->gamestateOwed );
	before = cl->netchan.outgoingSequence;
	SV_DoneDownload_f( cl );
	CHECK( cl->state == CS_PRIMED && gamestates == 2 && cl->gamestateMessageNum == before && !cl->gamestateOwed );
	SV_ClientEnterWorld( cl, &cmd );
	SV_DoneDownload_f( cl );
	CHECK( cl->state == CS_ACTIVE && gamestates == 2 );

	/* a client already in the world at map_restart owes nothing */
	cl = &svs.clients[ACTIVE];
	SV_ClientMapRestarted( cl );
	CHECK( cl->state == CS_ACTIVE && !cl->gamestateOwed );
	SV_DoneDownload_f( cl );
	CHECK( cl->state == CS_ACTIVE && gamestates == 2 );
	puts( "server state: in-game donedl ignored" );
}

/* The engine's copy of a name holds no control characters; color codes and high bytes stay. */
static void Names( void ) {
	client_t *cl = &svs.clients[ACTIVE];
	char raw[200];
	int i;

	Reset();
	Q_strncpyz( cl->userinfo, "\\name\\Two\nLines\r^3Admin\x7f\tX\xe9\\rate\\25000", sizeof( cl->userinfo ) );
	SV_UserinfoChanged( cl );
	CHECK( !strcmp( cl->name, "TwoLines^3AdminX\xe9" ) );    /* master: the raw name */
	CHECK( cl->rate == 25000 );

	SV_SetUserinfo( PRIMED, "\\name\\a\nb\\rate\\1" );
	CHECK( !strcmp( svs.clients[PRIMED].name, "ab" ) );

	/* bounded by the field, after filtering */
	Q_strncpyz( raw, "\\name\\", sizeof( raw ) );
	for ( i = 0; i < 100; i++ ) {
		Q_strcat( raw, sizeof( raw ), i & 1 ? "N" : "\n" );
	}
	Q_strncpyz( cl->userinfo, raw, sizeof( cl->userinfo ) );
	SV_UserinfoChanged( cl );
	CHECK( strlen( cl->name ) == sizeof( cl->name ) - 1 && !strchr( cl->name, '\n' ) );

	/* an ordinary ET name is unchanged */
	Q_strncpyz( cl->userinfo, "\\name\\^1E^7T ^bPlayer [x]", sizeof( cl->userinfo ) );
	SV_UserinfoChanged( cl );
	CHECK( !strcmp( cl->name, "^1E^7T ^bPlayer [x]" ) );
	puts( "server state: names without control characters" );
}

static qboolean IsCs( const char *cmd, int index ) {
	const char *prefix = va( "cs %i ", index );
	return !strncmp( cmd, prefix, strlen( prefix ) );
}

/* A client packet as CL_WritePacket builds it: header, then one usercmd. */
static void Packet( client_t *cl, int serverId, qboolean move ) {
	static byte data[MAX_MSGLEN];
	msg_t msg;
	usercmd_t from, to;
	MSG_Init( &msg, data, sizeof( data ) );
	MSG_Bitstream( &msg );
	MSG_WriteLong( &msg, serverId );
	MSG_WriteLong( &msg, cl->netchan.outgoingSequence - 1 );
	MSG_WriteLong( &msg, cl->reliableSequence );
	if ( move ) {
		memset( &from, 0, sizeof( from ) );
		memset( &to, 0, sizeof( to ) );
		to.serverTime = ++svs.time;
		MSG_WriteByte( &msg, clc_moveNoDelta );
		MSG_WriteByte( &msg, 1 );
		MSG_WriteDeltaUsercmdKey( &msg, sv.checksumFeed ^ ( cl->netchan.outgoingSequence - 1 ), &from, &to );
	}
	MSG_WriteByte( &msg, clc_EOF );
	MSG_BeginReading( &msg );
	SV_ExecuteClientMessage( cl, &msg );
}

static void MapRestart( void ) {
	argc = 1;
	com_frameTime += 100;
	SV_MapRestart_f();
}

/* map_restart while a client downloads or loads, and vdr from a client in the world. */
static void Restarts( void ) {
	client_t *cl = &svs.clients[CONNECTED], *loading = &svs.clients[PRIMED];
	int i, first;

	Reset();
	pure.integer = 1;
	svRunning.integer = 1;
	sv.serverId = sv.restartedServerId = com_frameTime = 1000;
	Q_strncpyz( cl->downloadName, "maps.pk3", sizeof( cl->downloadName ) );

	/* two restarts during one download: the debt stays */
	MapRestart();
	CHECK( cl->state == CS_ACTIVE && cl->gamestateOwed );
	MapRestart();
	CHECK( cl->state == CS_ACTIVE && cl->gamestateOwed );    /* Q3A be418fe7 alone: cleared */
	CHECK( !svs.clients[ACTIVE].gamestateOwed );
	cl->downloadName[0] = 0;
	SV_DoneDownload_f( cl );
	CHECK( cl->state == CS_PRIMED && !cl->gamestateOwed );

	/* a client loading at the restart gets its marks ahead of map_restart */
	Reset();
	sv.serverId = sv.restartedServerId = com_frameTime;
	SV_SetConfigstring( CS_PLAYERS, "n\\early" );
	SV_UpdateConfigStrings();
	CHECK( loading->csUpdated[CS_PLAYERS] );
	first = loading->reliableSequence + 1;
	MapRestart();
	CHECK( loading->state == CS_ACTIVE && loading->gamestateOwed );
	CHECK( IsCs( Cmd( loading, first ), CS_PLAYERS ) );    /* fdb89cac */
	CHECK( !strcmp( Cmd( loading, first + 1 ), "map_restart\n" ) );
	/* the restart's own changes reach it, like everyone, at the end of the frame */
	SV_UpdateConfigStrings();
	CHECK( IsCs( Cmd( loading, loading->reliableSequence ), CS_WARMUP ) );
	CHECK( IsCs( Cmd( &svs.clients[ACTIVE], svs.clients[ACTIVE].reliableSequence ), CS_WARMUP ) );

	/* once it is on the current serverId it is owed nothing */
	loading->netchan.outgoingSequence = 50;
	loading->gotCP = qtrue;
	loading->pureAuthentic = 1;
	Packet( loading, sv.serverId - 50, qfalse );    /* still on the old one: pre map_restart */
	CHECK( loading->gamestateOwed );
	Packet( loading, sv.serverId, qfalse );
	CHECK( !loading->gamestateOwed && loading->state == CS_ACTIVE );
	i = gamestates;
	SV_DoneDownload_f( loading );
	CHECK( gamestates == i && loading->state == CS_ACTIVE );    /* Q3A be418fe7 alone: a gamestate */

	/* nor once it plays, even on an old serverId that a "nextdl" let past the gate */
	loading->gamestateOwed = qtrue;
	Q_strncpyz( loading->lastClientCommandString, "nextdl 0", sizeof( loading->lastClientCommandString ) );
	thinks = 0;
	Packet( loading, sv.serverId - 50, qtrue );
	CHECK( thinks == 1 && !loading->gamestateOwed );    /* 68d559bc */
	SV_DoneDownload_f( loading );
	CHECK( gamestates == i && loading->state == CS_ACTIVE );
	loading->lastClientCommandString[0] = 0;

	/* an old serverId from a client in the world does not resend the gamestate */
	cl = &svs.clients[ACTIVE];
	cl->netchan.outgoingSequence = 80;
	cl->gamestateMessageNum = 10;
	cl->gotCP = qtrue;
	cl->pureAuthentic = 1;
	i = gamestates;
	Packet( cl, sv.restartedServerId - 1, qtrue );
	CHECK( gamestates == i && cl->state == CS_ACTIVE );    /* master: a gamestate */

	/* unless map_restart made it active while it still needed one: then a
	   dropped gamestate is resent */
	cl->gamestateOwed = qtrue;
	Packet( cl, sv.restartedServerId - 1, qfalse );
	CHECK( gamestates == i + 1 && cl->state == CS_PRIMED && !cl->gamestateOwed );
	SV_ClientEnterWorld( cl, &cl->lastUsercmd );

	/* vdr then cp, as vid_restart sends them, plays on */
	SV_ResetPureClient_f( cl );
	CHECK( !cl->gotCP );
	cl->gotCP = qtrue;    /* SV_VerifyPaks_f */
	cl->pureAuthentic = 1;
	i = gamestates;
	first = begins[ACTIVE];
	thinks = 0;
	Packet( cl, sv.serverId, qtrue );
	CHECK( thinks == 1 && cl->state == CS_ACTIVE && begins[ACTIVE] == first && gamestates == i );

	/* vdr with no cp: no gamestate (master: a second ClientBegin), and the
	   client is dropped as ETL does instead of left frozen */
	SV_ResetPureClient_f( cl );
	thinks = 0;
	Packet( cl, sv.serverId, qtrue );
	CHECK( gamestates == i && begins[ACTIVE] == first && !thinks );
	CHECK( cl->state == CS_ZOMBIE && dropReason[ACTIVE] && !strcmp( dropReason[ACTIVE], "Cannot validate pure client!" ) );
	pure.integer = 0;
	puts( "server state: map_restart, serverId and vdr keep the gamestate debt" );
}

/* A www download that fails or has a bad checksum falls back with a new
   gamestate, but only for a client not yet in the world. */
static void Wwwdl( const char *how ) {
	client_t *cl;
	int i;

	Reset();
	/* while connecting: the fallback gets its gamestate */
	cl = &svs.clients[PRIMED];
	cl->bWWWDl = qtrue;
	argv1 = "ack";
	SV_WWWDownload_f( cl );
	argv1 = how;
	SV_WWWDownload_f( cl );
	CHECK( gamestates == 1 && cl->state == CS_PRIMED && cl->bFallback && !cl->bWWWing );

	/* in the world, having asked for a download: none */
	cl = &svs.clients[ACTIVE];
	cl->bWWWDl = qtrue;
	argv1 = "ack";
	SV_WWWDownload_f( cl );
	argv1 = how;
	SV_WWWDownload_f( cl );
	CHECK( gamestates == 1 && cl->state == CS_ACTIVE && !dropReason[ACTIVE] );    /* master: a gamestate */

	/* made active by map_restart mid-download: owed one */
	SV_ClientMapRestarted( &svs.clients[CONNECTED] );
	cl = &svs.clients[CONNECTED];
	CHECK( cl->state == CS_ACTIVE && cl->gamestateOwed );
	cl->bWWWDl = qtrue;
	argv1 = "ack";
	SV_WWWDownload_f( cl );
	argv1 = how;
	SV_WWWDownload_f( cl );
	CHECK( gamestates == 2 && cl->state == CS_PRIMED && !cl->gamestateOwed );
	argv1 = "";
	for ( i = 0; i < CLIENTS; i++ ) {
		svs.clients[i].bWWWDl = svs.clients[i].bFallback = qfalse;
	}
	printf( "server state: wwwdl %s falls back with a gamestate only before the world\n", how );
}

/* A bot takes a slot a loading client left without clearing it. */
static void Bots( void ) {
	client_t *cl = &svs.clients[CONNECTED];
	Reset();
	cl->state = CS_FREE;
	cl->gamestateOwed = qtrue;
	cl->csUpdated[CS_PLAYERS] = 1;
	CHECK( SV_BotAllocateClient( 0 ) == CONNECTED );
	CHECK( cl->state == CS_ACTIVE && !cl->gamestateOwed && !cl->csUpdated[CS_PLAYERS] );
	puts( "server state: a bot's slot keeps no gamestate debt or marks" );
}

int main( void ) {
	maxclients.integer = CLIENTS;
	sv_maxclients = &maxclients;
	com_dedicated = &dedicated;
	sv_lanForceRate = &lanForceRate;
	sv_pure = &pure;
	com_sv_running = &svRunning;
	svs.clients = calloc( CLIENTS, sizeof( client_t ) );
	Configstrings();
	Download();
	Donedl();
	Names();
	Restarts();
	Wwwdl( "fail" );
	Wwwdl( "chkfail" );
	Bots();
	puts( "server state: all passed" );
	return 0;
}
