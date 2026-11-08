/* The client's handling of what a server sends: the real cl_parse.c, cvar.c,
 * msg.c and fs_path.c, with the rest of the engine stubbed. Run under ASan and
 * UBSan by tests/security/run.sh. CL_BeginDownload comes from cl_main.c by the
 * python block there, as BEGINDL_INC, built for the PATH_SEP in TEST_PATH_SEP. */
#include <setjmp.h>
#include "../../src/client/client.h"

void CL_ParseDownload( msg_t *msg );

#define CHECK( x ) do { if ( !( x ) ) { printf( "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); exit( 1 ); } } while ( 0 )

clientActive_t cl;
clientConnection_t clc;
clientStatic_t cls;
vm_t *cgvm;
qboolean sv_cheats;
cvar_t *cl_shownet, *cl_shownuments, *cl_autorecord, *com_developer, *com_sv_running, *com_crashed;

static jmp_buf test_drop;
static int drops;
static char reliable[64][MAX_STRING_CHARS];
static int numReliable, nextDownloads, binaryMessages, opens, writes;
static char renamedFrom[MAX_OSPATH], renamedTo[MAX_OSPATH];

void QDECL Com_Error( int code, const char *fmt, ... ) {
	drops++;
	longjmp( test_drop, 1 );
}
void QDECL Com_Printf( const char *fmt, ... ) {}
void QDECL Com_DPrintf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
qboolean CL_CheckPaused( void ) { return qfalse; }
char *CopyString( const char *in ) { return strdup( in ); }
void Z_Free( void *p ) { free( p ); }
void Cmd_AddCommand( const char *n, xcommand_t f ) {}
int Cmd_Argc( void ) { return 0; }
char *Cmd_Argv( int i ) { return ""; }
void Cmd_TokenizeString( const char *s ) {}
int Com_Filter( char *f, char *n, int c ) { return 0; }
void QDECL FS_Printf( fileHandle_t h, const char *fmt, ... ) {}
const char *CL_TranslateStringBuf( const char *s ) { return s; }
void CM_BoxTrace( trace_t *r, const vec3_t s, const vec3_t e, const vec3_t mi, const vec3_t ma, clipHandle_t m, int b, int c ) {
	memset( r, 0, sizeof( *r ) );
	r->fraction = 1;
}
void Cbuf_ExecuteText( int w, const char *t ) {}
void CL_AddReliableCommand( const char *c ) { Q_strncpyz( reliable[numReliable++ & 63], c, MAX_STRING_CHARS ); }
void CL_ClearState( void ) { memset( &cl, 0, sizeof( cl ) ); }
void CL_InitDownloads( void ) {}
void CL_NextDownload( void ) { nextDownloads++; }
void CL_PurgeCache( void ) {}
void CL_Record( const char *n ) {}
void CL_WritePacket( void ) {}
void CL_CGameBinaryMessageReceived( const char *b, int n, int t ) {
	CHECK( cgvm ); /* VM_Call( NULL ) is ERR_FATAL */
	binaryMessages++;
}
int Com_RealTime( qtime_t *t ) { memset( t, 0, sizeof( *t ) ); return 0; }
void Con_Close( void ) {}
void Sys_OpenURL( const char *u, qboolean q ) {}
/* as dl_main_stubs.c: there is no www download here */
int DL_BeginDownload( const char *l, const char *r, int d ) { return 0; }
char *FS_BuildOSPath( const char *base, const char *game, const char *qpath ) {
	static char p[MAX_OSPATH];
	Com_sprintf( p, sizeof( p ), "%s/%s/%s", base, game, qpath );
	return p;
}
qboolean FS_ConditionalRestart( int f ) { return qfalse; }
void FS_FCloseFile( fileHandle_t f ) {}
void FS_PureServerSetLoadedPaks( const char *a, const char *b ) {}
void FS_PureServerSetReferencedPaks( const char *a, const char *b ) {}
qboolean FS_VerifyOfficialPaks( void ) { return qtrue; }
fileHandle_t FS_SV_FOpenFileWrite( const char *n ) { opens++; return 1; }
int FS_Write( const void *b, int n, fileHandle_t f ) { writes += n; return n; }
void FS_SV_Rename( const char *from, const char *to ) {
	Q_strncpyz( renamedFrom, from, sizeof( renamedFrom ) );
	Q_strncpyz( renamedTo, to, sizeof( renamedTo ) );
}

#undef PATH_SEP
#define PATH_SEP TEST_PATH_SEP
#include BEGINDL_INC

static byte buf[MAX_MSGLEN];
static msg_t wm;

static void begin( void ) {
	MSG_Init( &wm, buf, sizeof( buf ) );
	MSG_Bitstream( &wm );
}

/* run f on what was written; qtrue if it dropped */
static qboolean run( void ( *f )( msg_t * ) ) {
	msg_t m = wm;

	MSG_BeginReading( &m );
	if ( setjmp( test_drop ) ) {
		return qtrue;
	}
	f( &m );
	return qfalse;
}

static void write_block( int block, int fileSize, int len ) {
	byte data[1024];

	memset( data, 'x', sizeof( data ) );
	MSG_WriteShort( &wm, block );
	if ( !block ) {
		MSG_WriteLong( &wm, fileSize );
	}
	MSG_WriteShort( &wm, len );
	MSG_WriteData( &wm, data, len );
}

static void write_redirect( const char *url, int flags ) {
	MSG_WriteShort( &wm, -1 );
	MSG_WriteString( &wm, url );
	MSG_WriteLong( &wm, 1000 );
	MSG_WriteLong( &wm, flags );
}

static qboolean sent( const char *c ) {
	int i;

	for ( i = 0; i < numReliable && i < 64; i++ ) {
		if ( !strcmp( reliable[i], c ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/* what CL_InitDownloads does with a new gamestate, then CL_BeginDownload */
static void new_download( const char *name ) {
	clc.bWWWDl = clc.bWWWDlAborting = qfalse;
	cls.downloadTempName[0] = cls.downloadName[0] = cls.originalDownloadName[0] = 0;
	CHECK( CL_BeginDownload( name, name ) );
	renamedFrom[0] = renamedTo[0] = 0;
	numReliable = nextDownloads = opens = writes = 0;
}

static void udp_download( void ) {
	begin();
	write_block( 0, 1500, 1000 );
	CHECK( !run( CL_ParseDownload ) );
	begin();
	write_block( 1, 0, 500 );
	CHECK( !run( CL_ParseDownload ) );
	begin();
	write_block( 2, 0, 0 );
	CHECK( !run( CL_ParseDownload ) );
}

static void test_udp_download( void ) {
	new_download( "etmain/map.pk3" );
	udp_download();
	CHECK( opens == 1 && writes == 1500 );
	CHECK( !strcmp( renamedFrom, "etmain/map.pk3.tmp" ) && !strcmp( renamedTo, "etmain/map.pk3" ) );
	CHECK( sent( "nextdl 0" ) && sent( "nextdl 2" ) && nextDownloads == 1 );
	CHECK( !cls.downloadName[0] && !cls.downloadTempName[0] );
}

static void test_redirect( void ) {
	static const char *urls[] = {
		"http://example.com/etmain/map.pk3",
		"../../../.bashrc",
		"etmain/autoexec.cfg",
	};
	int i;

	for ( i = 0; i < 3; i++ ) {
		new_download( "etmain/map.pk3" );
		begin();
		write_redirect( urls[i], 0 );
		CHECK( !run( CL_ParseDownload ) );
		/* the URL goes nowhere near the names the file is written under */
		CHECK( !strcmp( cls.downloadName, "etmain/map.pk3" ) );
		CHECK( !strcmp( cls.downloadTempName, "etmain/map.pk3.tmp" ) );
		CHECK( sent( "wwwdl ack" ) && sent( "wwwdl fail" ) );
		CHECK( clc.bWWWDl && clc.bWWWDlAborting );
		/* the repeated redirect is eaten */
		begin();
		write_redirect( urls[i], 0 );
		CHECK( !run( CL_ParseDownload ) );
		/* blocks before the server's new gamestate are not for this download */
		udp_download();
		CHECK( !opens && !writes && !renamedTo[0] );
		/* and after it the UDP fallback completes, as with a real server */
		new_download( "etmain/map.pk3" );
		udp_download();
		CHECK( !strcmp( renamedTo, "etmain/map.pk3" ) );
	}
	/* a message is consumed whole: what follows the ignored block still parses */
	new_download( "etmain/map.pk3" );
	clc.bWWWDl = qtrue;
	begin();
	write_block( 0, 1500, 1000 );
	MSG_WriteShort( &wm, 0x1234 );
	{
		msg_t m = wm;

		MSG_BeginReading( &m );
		CHECK( !setjmp( test_drop ) );
		CL_ParseDownload( &m );
		CHECK( MSG_ReadShort( &m ) == 0x1234 );
	}
}

static void test_eof_rename_checked( void ) {
	new_download( "etmain/map.pk3" );
	Q_strncpyz( cls.downloadName, "etmain/../../x.cfg", sizeof( cls.downloadName ) );
	udp_download();
	CHECK( !renamedTo[0] && nextDownloads == 1 );
}

static void test_begin_download_names( void ) {
	char longName[400];
	const char *tmp31 = TEST_PATH_SEP == ':' ? "etmain/abcdefghijklmnopqrstuvwxyz1.tmp" : "etmain/abcdefghijklmnopqrstuvwxyz1.pk3.tmp";

	CHECK( !CL_BeginDownload( "etmain/x.cfg", "etmain/x.pk3" ) );
	CHECK( !CL_BeginDownload( "../x.pk3", "etmain/x.pk3" ) );
	memset( longName, 'a', sizeof( longName ) );
	memcpy( longName, "etmain/", 7 );
	strcpy( longName + sizeof( longName ) - 5, ".pk3" );
	CHECK( !CL_BeginDownload( longName, "etmain/x.pk3" ) );
	CHECK( !CL_BeginDownload( "etmain/x.pk3", longName ) );
	/* 27-byte leaf: "<name>.tmp" fits HFS's 31 bytes */
	CHECK( CL_BeginDownload( "etmain/abcdefghijklmnopqrstuvw.pk3", "etmain/abcdefghijklmnopqrstuvw.pk3" ) );
	CHECK( !strcmp( cls.downloadTempName, "etmain/abcdefghijklmnopqrstuvw.pk3.tmp" ) );
	/* 31-byte leaf */
	CHECK( CL_BeginDownload( "etmain/abcdefghijklmnopqrstuvwxyz1.pk3", "etmain/abcdefghijklmnopqrstuvwxyz1.pk3" ) );
	CHECK( !strcmp( cls.downloadTempName, tmp31 ) );
	CHECK( !strcmp( cls.downloadName, "etmain/abcdefghijklmnopqrstuvwxyz1.pk3" ) );
}

static void test_binary_without_cgame( void ) {
	static const byte tail[] = "binary bytes after svc_EOF";

	begin();
	MSG_WriteLong( &wm, 0 );    /* reliable acknowledge */
	MSG_WriteByte( &wm, svc_EOF );
	memcpy( wm.data + wm.cursize, tail, sizeof( tail ) );
	wm.cursize += sizeof( tail );
	cgvm = NULL;
	binaryMessages = 0;
	CHECK( !run( CL_ParseServerMessage ) );
	CHECK( !binaryMessages );
	cgvm = (vm_t *)&cl;
	CHECK( !run( CL_ParseServerMessage ) );
	CHECK( binaryMessages == 1 );
	cgvm = NULL;
}

static void parse_entities( msg_t *m ) {
	clSnapshot_t snap;

	memset( &snap, 0, sizeof( snap ) );
	CL_ParsePacketEntities( m, NULL, &snap );
}

static void test_entity_count( void ) {
	entityState_t base;
	int i;

	memset( &base, 0, sizeof( base ) );
	memset( &cl, 0, sizeof( cl ) );
	/* the same baseline entity, again and again: one frame past the ring */
	begin();
	for ( i = 0; i < MAX_PARSE_ENTITIES + 8; i++ ) {
		MSG_WriteDeltaEntity( &wm, &base, &base, qtrue );
	}
	MSG_WriteBits( &wm, MAX_GENTITIES - 1, GENTITYNUM_BITS );
	drops = 0;
	CHECK( run( parse_entities ) );
	CHECK( cl.parseEntitiesNum == MAX_PARSE_ENTITIES );
	/* a full frame of distinct entities still parses */
	memset( &cl, 0, sizeof( cl ) );
	begin();
	for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
		base.number = i;
		MSG_WriteDeltaEntity( &wm, &base, &base, qtrue );
	}
	MSG_WriteBits( &wm, MAX_GENTITIES - 1, GENTITYNUM_BITS );
	CHECK( !run( parse_entities ) );
	CHECK( cl.parseEntitiesNum == MAX_GENTITIES - 1 );
}

static void set_systeminfo( const char *s ) {
	cl.gameState.stringOffsets[CS_SYSTEMINFO] = 1;
	Q_strncpyz( cl.gameState.stringData + 1, s, sizeof( cl.gameState.stringData ) - 1 );
}

static void test_systeminfo_cvars( void ) {
	vmCvar_t vc;

	memset( &cl, 0, sizeof( cl ) );
	clc.netchan.remoteAddress.type = NA_IP;
	Cvar_Get( "cl_guid", "mine", 0 );
	Cvar_Get( "sv_floodprotect", "1", CVAR_SYSTEMINFO );
	Cvar_Get( "pmove_fixed", "0", 0 );

	set_systeminfo( "\\sv_serverid\\7\\srv_made\\a\\cl_guid\\theirs\\sv_floodprotect\\0\\pmove_fixed\\1" );
	CHECK( !setjmp( test_drop ) );
	CL_SystemInfoChanged();
	CHECK( cl.serverId == 7 );
	CHECK( !strcmp( Cvar_VariableString( "cl_guid" ), "mine" ) );
	CHECK( !strcmp( Cvar_VariableString( "sv_floodprotect" ), "0" ) );
	CHECK( !strcmp( Cvar_VariableString( "pmove_fixed" ), "1" ) );
	CHECK( !strcmp( Cvar_VariableString( "srv_made" ), "a" ) );
	CHECK( ( Cvar_Flags( "srv_made" ) & ( CVAR_SERVER_CREATED | CVAR_ROM ) ) == ( CVAR_SERVER_CREATED | CVAR_ROM ) );

	/* the same or a later server may set what a server created */
	set_systeminfo( "\\sv_serverid\\8\\srv_made\\b" );
	CL_SystemInfoChanged();
	CHECK( !strcmp( Cvar_VariableString( "srv_made" ), "b" ) );
	/* the player may not, until something registers it */
	Cvar_Set2( "srv_made", "c", qfalse );
	CHECK( !strcmp( Cvar_VariableString( "srv_made" ), "b" ) );

	/* a module cannot claim CVAR_SERVER_CREATED */
	Cvar_Register( &vc, "mod_var", "1", CVAR_SERVER_CREATED | CVAR_ARCHIVE );
	CHECK( Cvar_Flags( "mod_var" ) == CVAR_ARCHIVE );
	set_systeminfo( "\\sv_serverid\\9\\mod_var\\2" );
	CL_SystemInfoChanged();
	CHECK( !strcmp( Cvar_VariableString( "mod_var" ), "1" ) );

	/* registration takes the cvar over: its flags decide from then on */
	Cvar_Register( &vc, "srv_made", "d", CVAR_ARCHIVE );
	CHECK( Cvar_Flags( "srv_made" ) == CVAR_ARCHIVE );
	Cvar_Set2( "srv_made", "e", qfalse );
	CHECK( !strcmp( Cvar_VariableString( "srv_made" ), "e" ) );
	set_systeminfo( "\\sv_serverid\\9\\srv_made\\f" );
	CL_SystemInfoChanged();
	CHECK( !strcmp( Cvar_VariableString( "srv_made" ), "e" ) );
}

int main( int argc, char **argv ) {
	Cvar_Init();
	cl_shownet = Cvar_Get( "cl_shownet", "0", 0 );
	cl_shownuments = Cvar_Get( "cl_shownuments", "0", 0 );
	cl_autorecord = Cvar_Get( "cl_autorecord", "0", 0 );
	com_developer = Cvar_Get( "developer", "0", 0 );
	com_sv_running = Cvar_Get( "sv_running", "0", 0 );
	Cvar_Get( "fs_homepath", "/home/player/.etwolf", 0 );

	/* "names": every pak name on stdin must still download, under a temp name
	 * that is not the pak's own and, with ':', fits HFS when the pak's does */
	if ( argc > 1 && !strcmp( argv[1], "names" ) ) {
		char line[1024];
		int n = 0;

		while ( fgets( line, sizeof( line ), stdin ) ) {
			const char *leaf, *pakLeaf;

			line[strcspn( line, "\r\n" )] = 0;
			CHECK( CL_BeginDownload( line, line ) );
			CHECK( strcmp( cls.downloadTempName, cls.downloadName ) );
			leaf = strrchr( cls.downloadTempName, '/' );
			pakLeaf = strrchr( line, '/' );
			CHECK( TEST_PATH_SEP != ':' || strlen( pakLeaf + 1 ) > 31 || strlen( leaf + 1 ) <= 31 );
			n++;
		}
		printf( "%d pak names accepted (PATH_SEP '%c')\n", n, TEST_PATH_SEP );
		return 0;
	}
	/* one test by name, or all */
#define T( name ) if ( argc < 2 || !strcmp( argv[1], #name ) ) { test_##name(); }
	T( udp_download )
	T( redirect )
	T( eof_rename_checked )
	T( begin_download_names )
	T( binary_without_cgame )
	T( entity_count )
	T( systeminfo_cvars )
	printf( "client parse (PATH_SEP '%c'): all passed\n", TEST_PATH_SEP );
	return 0;
}
