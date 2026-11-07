/* Issues #263 and #10: classic Mac startup initializes the Toolbox itself and
 * keeps the Retro68 console window closed unless it is wanted.
 *
 * #263: Retro68's startup code initializes no Toolbox manager.  The port used
 * to rely on libRetroConsole, whose first printf ran InitGraf, InitFonts,
 * InitWindows and InitMenus.  main must now run id's InitMacStuff sequence
 * (MaxApplZone, MoreMasters, InitGraf, InitFonts, FlushEvents, InitWindows,
 * InitMenus, TEInit, InitDialogs, InitCursor) before it writes anything, calls
 * any Sys_* function or reads the keyboard with GetKeys.
 *
 * #10: the console window opens only when output is wanted (viewlog, the
 * Shift prompt at launch, or errors on stderr).  Default startup must not open
 * it, and its log lines must still reach the crash-dump ring.
 *
 * The follow-up to #499: Sys_Error writes the crash log before stderr can
 * create the window, releases a display DrawSprocket still holds, then shows
 * retail's Stop alert; opening the window mid-game restores the current port;
 * every write is gated, so viewlog 0 also stops output to a window the Shift
 * prompt opened; and a static module failure waits for Return.
 *
 * #5: Sys_Error closes the Sound Manager channel (SNDDMA_Shutdown) before
 * the alert, since a recursive error skips CL_Shutdown.
 *
 * The runner extracts the real main, Sys_InitToolbox, Sys_LogPrintf,
 * Sys_AppendStartupText, Sys_ReadStartupFile, Sys_StartupError, Sys_JoinHFSPath
 * and Sys_Error (mac_main.c) and Sys_ConsoleWanted, Sys_ShowConsole and Sys_Print
 * (mac_console.c), and links the real _consolewrite and _consoleread
 * (mac_consolehooks.cc, compiled as C++ against mac_console_window_fake.h).
 * The Toolbox calls are fakes that log their order, and stdio goes straight
 * to the real hooks.  The window may only be created after the Toolbox is up,
 * because mac_consolehooks.cc no longer initializes it. */
#include "../../src/game/q_shared.h"
#include "et_compat.h"
#include "../../src/qcommon/qcommon.h"
#undef Sys_LogPrintf	/* the Mac build has a real one (macintosh) */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "mac_console_window_fake.h"

/* ---- fake Toolbox ---- */

#define MAX_EVENTS	256
static const char	*events[MAX_EVENTS];
static int			numEvents;
static int			toolboxReady;	/* InitCursor has run */
static int			shiftHeld;

static int EventIndex( const char *name );

static void Event( const char *name ) {
	if ( numEvents < MAX_EVENTS ) {
		events[numEvents++] = name;
	}
}

typedef unsigned int	KeyMap[4];
typedef struct { short what; } EventRecord;
#define everyEvent		( (short)0xFFFF )
FakeQDGlobals	qd = { NULL, { { 0, 0, 480, 640 } } };

static struct FakeGrafPort { int unused; } gamePort, consolePort;
static GrafPtr	currentPort;

void GetPort( GrafPtr *port ) { *port = currentPort; }
void SetPort( GrafPtr port ) { currentPort = port; }
void InsetRect( Rect *r, short dh, short dv ) {
	r->left += dh;
	r->right -= dh;
	r->top += dv;
	r->bottom -= dv;
}

static void MaxApplZone( void ) { Event( "MaxApplZone" ); }
static void MoreMasters( void ) { Event( "MoreMasters" ); }
static void Check( int ok, const char *message );
static void InitGraf( void *globalPtr ) {
	Check( globalPtr == &qd.thePort, "InitGraf is passed &qd.thePort" );
	Event( "InitGraf" );
}
static void InitFonts( void ) { Event( "InitFonts" ); }
static void FlushEvents( short mask, short stop ) {
	Check( mask == everyEvent && stop == 0, "FlushEvents( everyEvent, 0 )" );
	Event( "FlushEvents" );
}
static void InitWindows( void ) { Event( "InitWindows" ); }
static void InitMenus( void ) { Event( "InitMenus" ); }
static void TEInit( void ) { Event( "TEInit" ); }
static void InitDialogs( void *ignored ) { Event( "InitDialogs" ); }
static void InitCursor( void ) { Event( "InitCursor" ); toolboxReady = 1; }
static int EventAvail( short mask, EventRecord *event ) {
	Event( "EventAvail" );
	event->what = 0;
	return 0;
}
static void GetKeys( KeyMap keys ) {
	Event( "GetKeys" );
	memset( keys, 0, sizeof( KeyMap ) );
	if ( shiftHeld ) {
		( (unsigned char *)keys )[7] = 0x01;	/* key code 0x38 */
	}
}

/* ---- the console window behind the real mac_consolehooks.cc ---- */

int Sys_ConsoleWanted( int fd );
ssize_t _consolewrite( int fd, const void *buf, size_t count );
ssize_t _consoleread( int fd, void *buf, size_t count );
void Sys_ConsoleDraw( GrafPtr window );	/* mac_consolehooks.cc */

static int			windowOpen;
static char			windowText[16384];
static char			ring[16384];		/* what Sys_LogRecord kept */
static int			outputBeforeToolbox;
static const char	*typed;				/* what is typed into the window */
static int			draining;

GrafPtr FakeConsoleWindowOpen( const Rect *r ) {
	Check( toolboxReady, "the console window is only created after the Toolbox is initialized" );
	Check( r->top == 45 && r->left == 5 && r->bottom == 475 && r->right == 635,
		"the window is where InitConsole put it" );
	Event( "console window" );
	windowOpen = 1;
	return &consolePort;
}

static int			windowDraws;
void FakeConsoleWindowDraw( void ) {
	Check( windowOpen, "only an open window is drawn" );
	windowDraws++;
}

void FakeConsoleWindowWrite( const char *s, int n ) {
	size_t	length = strlen( windowText );

	Check( windowOpen, "output only reaches an open window" );
	if ( length + n < sizeof( windowText ) ) {
		memcpy( windowText + length, s, n );
		windowText[length + n] = '\0';
	}
}

/* A line typed and ended with Return; Return alone once typed runs out. */
const char *FakeConsoleWindowReadLine( void ) {
	static char	line[2048];
	int			i;

	if ( draining ) {
		return "";
	}
	Event( "read" );
	for ( i = 0 ; i < (int)sizeof( line ) - 1 && typed && *typed ; ) {
		line[i++] = *typed;
		if ( *typed++ == '\n' ) {
			break;
		}
	}
	if ( i == 0 ) {
		line[i++] = '\n';
	}
	line[i] = '\0';
	return line;
}

/* Close the window between cases, dropping what a case left unread. */
static void CloseWindow( void ) {
	char	junk[4096];

	if ( windowOpen ) {
		draining = 1;
		while ( _consoleread( 0, junk, sizeof( junk ) ) > 0 ) {
		}
		draining = 0;
	}
	FakeConsoleWindowDelete();
	windowOpen = 0;
	windowText[0] = '\0';
}

static void ConsoleWrite( int fd, const char *text ) {
	if ( !toolboxReady ) {
		outputBeforeToolbox = 1;
	}
	Event( "output" );
	Check( _consolewrite( fd, text, strlen( text ) ) == (ssize_t)strlen( text ),
		"_consolewrite takes the whole write" );
}

static int FakePrintf( const char *fmt, ... ) {
	char	text[4096];
	va_list	argptr;

	va_start( argptr, fmt );
	vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	ConsoleWrite( 1, text );
	return strlen( text );
}

static int FakeFprintf( FILE *f, const char *fmt, ... ) {
	char	text[4096];
	va_list	argptr;

	va_start( argptr, fmt );
	vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	ConsoleWrite( f == stderr ? 2 : 1, text );
	return strlen( text );
}

static int FakeFflush( FILE *f ) {
	return 0;
}

static char *FakeFgets( char *buf, int size, FILE *f ) {
	int		i;
	char	c;

	Check( f == stdin, "fgets reads stdin" );
	for ( i = 0 ; i < size - 1 && _consoleread( 0, &c, 1 ) == 1 ; ) {
		buf[i++] = c;
		if ( c == '\n' ) {
			break;
		}
	}
	if ( i == 0 ) {
		return NULL;
	}
	buf[i] = '\0';
	return buf;
}

static int FakeGetchar( void ) {
	unsigned char	c;

	if ( _consoleread( 0, &c, 1 ) != 1 ) {
		return EOF;
	}
	return c;
}

/* ---- Sys_Error's world ---- */

static struct { qboolean isFullscreen; } glConfig;
static jmp_buf		exited;
static int			exitStatus;
static int			ringHadErrorAtDump;
static char			alertTitle[256], alertMessage[256];

void Com_DumpFlightRecord( const char *fileName ) {
	Event( "flight record" );
}

static void Sys_DumpRetroLogs( const char *fileName ) {
	Check( !strcmp( fileName, "retro68_console_crash.txt" ), "Sys_Error dumps retro68_console_crash.txt" );
	Event( "crash log" );
	ringHadErrorAtDump = strstr( ring, "Sys_Error: " ) != NULL;
}

static void Sys_ShutdownInput( void ) { Event( "Sys_ShutdownInput" ); }
static void Sys_ShutdownNetworking( void ) { Event( "Sys_ShutdownNetworking" ); }
static void SNDDMA_Shutdown( void ) { Event( "SNDDMA_Shutdown" ); }

static void PascalToC( char *out, const unsigned char *in ) {
	memcpy( out, in + 1, in[0] );
	out[in[0]] = '\0';
}

static void ParamText( ConstStr255Param p0, ConstStr255Param p1, ConstStr255Param p2,
		ConstStr255Param p3 ) {
	PascalToC( alertTitle, p0 );
	PascalToC( alertMessage, p1 );
	Check( !memcmp( p1, p2, p1[0] + 1 ) && !memcmp( p1, p3, p1[0] + 1 ),
		"^1, ^2 and ^3 are the message, as retail" );
}

static short StopAlert( short alertID, void *filter ) {
	Check( alertID == 128 && filter == NULL, "StopAlert( 128, NULL ), as retail" );
	Event( "alert" );
	return 1;
}

static void FakeExit( int status ) __attribute__(( noreturn ));
static void FakeExit( int status ) {
	exitStatus = status;
	longjmp( exited, 1 );
}

/* ---- the rest of main's world ---- */

static jmp_buf		comInitDone;
static int			comInitCalls;
static int			viewlog;
static const char	*staticModulesError;
static char			comInitCommandLine[MAX_STRING_CHARS];
static char			cwd[4096];		/* Sys_GetCwd: the application folder */

void Sys_LogRecord( const char *text ) {
	Q_strcat( ring, sizeof( ring ), text );
}

static const char *VM_InitStaticModules( void *modules, int count ) {
	Event( "VM_InitStaticModules" );
	return staticModulesError;
}
static int	sys_staticModules[1];
#define SYS_STATIC_MODULES	1

static char *Sys_GetCwd( void ) {
	Event( "Sys_GetCwd" );
	return cwd;
}

void Com_FlightRecord( const char *fmt, ... ) {
}

void Debug_Breadcrumb( int color ) {
}

void Com_Frame( void ) {
	Check( 0, "Com_Init returns to the test" );
}

void Sys_ShowConsole( int level, qboolean quitOnClose );
void Sys_LogPrintf( const char *fmt, ... );

/* Stands in for Com_Init: some startup logging, then CL_Init's
 * Sys_ShowConsole( viewlog ), then more console output. */
void Com_Init( char *commandLine ) {
	Event( "Com_Init" );
	comInitCalls++;
	Q_strncpyz( comInitCommandLine, commandLine, sizeof( comInitCommandLine ) );
	Sys_LogPrintf( "Com_Init: Sys_Init\n" );
	Sys_Print( "engine: before viewlog\n" );
	Sys_ShowConsole( viewlog, qfalse );
	Sys_Print( "engine: after viewlog\n" );
	Sys_LogPrintf( "Com_Init: after viewlog\n" );
	longjmp( comInitDone, 1 );
}

void QDECL Com_Error( int level, const char *fmt, ... ) {
	fprintf( stderr, "mac toolbox init regression: Com_Error: %s\n", fmt );
	exit( 1 );
}

void QDECL Com_Printf( const char *fmt, ... ) {
}

static qboolean	consoleDisplayed;	/* mac_console.c's */

/* The boot log (mac_bootlog.c has its own regression). */
static void Sys_StartBootLog( void ) { Event( "Sys_StartBootLog" ); }
static void Sys_Heartbeat( void ) { }
void Sys_BootLogFlush( void ) { Event( "Sys_BootLogFlush" ); }
void Sys_BootLogClose( void ) { Event( "Sys_BootLogClose" ); }
void Sys_InstallCrashHandler( void ) { Event( "Sys_InstallCrashHandler" ); }
void Sys_RemoveCrashHandler( void ) { Event( "Sys_RemoveCrashHandler" ); }
/* ET: mac_main calls Sys_InitStaticModules, which wraps VM_InitStaticModules (mac_et.c) */
const char *Sys_InitStaticModules( void ) { return VM_InitStaticModules( NULL, 0 ); }

/* mac_glimp.c's, which tests/mac_glimp_regression.c covers */
void GLimp_ReleaseDisplay( void ) {
	Event( "GLimp_ReleaseDisplay" );
	glConfig.isFullscreen = qfalse;
}

#define printf		FakePrintf
#define fprintf		FakeFprintf
#define fflush		FakeFflush
#define fgets		FakeFgets
#define getchar		FakeGetchar
#define exit		FakeExit
#define main		Sys_TestMacMain
#include "mac_main_extracted.c"
#include "mac_console_extracted.c"
#undef main
#undef exit
#undef printf
#undef fprintf
#undef fflush
#undef fgets
#undef getchar

/* ---- the tests ---- */

static const char *currentCase = "setup";

static void Check( int ok, const char *message ) {
	if ( !ok ) {
		int	i;

		fprintf( stderr, "mac toolbox init regression failed (%s): %s\nevents:", currentCase, message );
		for ( i = 0 ; i < numEvents ; i++ ) {
			fprintf( stderr, " %s", events[i] );
		}
		fprintf( stderr, "\n" );
		exit( 1 );
	}
}

static int EventIndex( const char *name ) {
	int	i;

	for ( i = 0 ; i < numEvents ; i++ ) {
		if ( !strcmp( events[i], name ) ) {
			return i;
		}
	}
	return -1;
}

static int EventCount( const char *name ) {
	int	i, count;

	for ( i = count = 0 ; i < numEvents ; i++ ) {
		if ( !strcmp( events[i], name ) ) {
			count++;
		}
	}
	return count;
}

static void WriteParms( const char *data ) {
	char	path[sizeof( cwd ) + 32];	/* cwd + ":METParms.txt" */
	FILE	*f;

	snprintf( path, sizeof( path ), "%s:METParms.txt", cwd );
	if ( !data ) {
		remove( path );
		return;
	}
	f = fopen( path, "wb" );
	Check( f != NULL, "write the parameters file" );
	fwrite( data, 1, strlen( data ), f );
	fclose( f );
}

static int RunMain( const char *name, int shift, const char *line, const char *parms, int log ) {
	static char	*argv[] = { "Quake3", NULL };
	int			result;

	currentCase = name;
	numEvents = 0;
	toolboxReady = 0;
	shiftHeld = shift;
	typed = line;
	viewlog = log;
	staticModulesError = NULL;
	CloseWindow();
	ring[0] = '\0';
	outputBeforeToolbox = 0;
	consoleDisplayed = qfalse;
	comInitCalls = 0;
	comInitCommandLine[0] = '\0';
	WriteParms( parms );

	if ( setjmp( comInitDone ) ) {
		return -1;	/* reached Com_Init */
	}
	result = Sys_TestMacMain( 1, argv );
	return result;
}

/* main's own Toolbox initialization comes first, in InitMacStuff's order */
static void CheckToolboxFirst( void ) {
	static const char	*order[] = {
		"MaxApplZone", "MoreMasters", "InitGraf", "InitFonts", "FlushEvents",
		"InitWindows", "InitMenus", "TEInit", "InitDialogs", "InitCursor"
	};
	int					i;

	Check( !outputBeforeToolbox, "no output before the Toolbox is initialized" );
	for ( i = 0 ; i < (int)( sizeof( order ) / sizeof( order[0] ) ) ; i++ ) {
		Check( numEvents > i && !strcmp( events[i], order[i] ),
			"main starts with MaxApplZone, MoreMasters, InitGraf, InitFonts, FlushEvents, "
			"InitWindows, InitMenus, TEInit, InitDialogs, InitCursor" );
	}
	Check( EventIndex( "EventAvail" ) > EventIndex( "InitCursor" ),
		"EventAvail brings the application to the front after the Toolbox is up" );
	Check( EventIndex( "VM_InitStaticModules" ) > EventIndex( "InitCursor" ),
		"VM_InitStaticModules runs after the Toolbox is initialized" );
}

static void RunSysError( const char *name, qboolean fullscreen, const char *fmt, ... ) {
	char	text[1024];
	va_list	argptr;

	currentCase = name;
	numEvents = 0;
	CloseWindow();
	ring[0] = '\0';
	consoleDisplayed = qfalse;
	glConfig.isFullscreen = fullscreen;
	ringHadErrorAtDump = 0;
	alertTitle[0] = alertMessage[0] = '\0';
	exitStatus = -1;
	va_start( argptr, fmt );
	vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	if ( !setjmp( exited ) ) {
		Sys_Error( "%s", text );
		Check( 0, "Sys_Error does not return" );
	}
	glConfig.isFullscreen = qfalse;
}

static void CheckGetKeysAfterToolbox( void ) {
	Check( EventIndex( "GetKeys" ) > EventIndex( "InitCursor" ), "GetKeys runs after the Toolbox is initialized" );
	Check( EventIndex( "Sys_StartBootLog" ) >= 0 && EventIndex( "Sys_StartBootLog" ) < EventIndex( "Com_Init" ),
		"the boot log is running before Com_Init can freeze" );
	Check( EventIndex( "Sys_InstallCrashHandler" ) >= 0 && EventIndex( "Sys_InstallCrashHandler" ) < EventIndex( "Com_Init" ),
		"crashes are caught from before Com_Init" );
	/* a real G4 froze here and left no log: GetKeys starts the parameters */
	Check( EventIndex( "Sys_StartBootLog" ) < EventIndex( "GetKeys" ),
		"the boot log is running before the parameters are read" );
	Check( EventIndex( "Sys_InstallCrashHandler" ) < EventIndex( "GetKeys" ),
		"crashes are caught from before the parameters are read" );
}

int main( int argc, char **argv ) {
	int		result;
	char	longLine[1200];
	char	longFile[1200];

	if ( argc != 2 ) {
		fprintf( stderr, "usage: %s <scratch directory>\n", argv[0] );
		return 2;
	}
	/* main joins HFS style, so the file is <dir>/app:METParms.txt */
	snprintf( cwd, sizeof( cwd ), "%s/app", argv[1] );

	/* default launch: no Shift, no parameters file, viewlog 0 */
	result = RunMain( "default launch", 0, NULL, NULL, 0 );
	Check( result == -1 && comInitCalls == 1, "Com_Init is reached" );
	CheckToolboxFirst();
	CheckGetKeysAfterToolbox();
	Check( EventIndex( "output" ) > EventIndex( "InitCursor" ), "there is startup output, after the Toolbox" );
	Check( !windowOpen, "no console window with viewlog 0 and no error" );
	Check( strstr( ring, "main: START\n" ) != NULL, "the crash ring has main: START" );
	Check( strstr( ring, "main: command line: \n" ) != NULL, "the crash ring has the command line" );
	Check( strstr( ring, "Com_Init: Sys_Init\n" ) != NULL, "the crash ring has Com_Init's log" );
	Check( strstr( ring, "engine: after viewlog\n" ) != NULL, "the crash ring has hidden engine output" );
	Check( comInitCommandLine[0] == '\0', "empty command line" );

	/* a parameters file changes nothing about the window */
	result = RunMain( "parameters file", 0, NULL, "+set fs_game mymod\r\nsafe\r\n", 0 );
	Check( result == -1, "Com_Init is reached" );
	CheckToolboxFirst();
	CheckGetKeysAfterToolbox();
	Check( !windowOpen, "no console window with a parameters file" );
	Check( !strcmp( comInitCommandLine, "+set fs_game mymod\nsafe\n" ), "the file reaches Com_Init" );
	Check( strstr( ring, "main: startup parameters read from" ) != NULL, "the crash ring has the file's path" );

	/* the log and crash handler start before the file, which can turn them off */
	Check( EventIndex( "Sys_BootLogClose" ) < 0 && EventIndex( "Sys_RemoveCrashHandler" ) < 0,
		"the log and crash handler stay on by default" );
	result = RunMain( "log and crash handler off", 0, NULL,
		"+set sys_bootlog 0\r+set sys_crashhandler 0\r", 0 );
	Check( result == -1, "Com_Init is reached" );
	CheckGetKeysAfterToolbox();
	Check( EventIndex( "Sys_BootLogClose" ) >= 0 && EventIndex( "Sys_BootLogClose" ) < EventIndex( "Com_Init" ),
		"sys_bootlog 0 closes the log before Com_Init" );
	Check( EventIndex( "Sys_RemoveCrashHandler" ) >= 0
		&& EventIndex( "Sys_RemoveCrashHandler" ) < EventIndex( "Com_Init" ),
		"sys_crashhandler 0 removes the handler before Com_Init" );

	/* viewlog 1 opens the window when Com_Init shows the console */
	result = RunMain( "viewlog 1", 0, NULL, NULL, 1 );
	Check( result == -1, "Com_Init is reached" );
	CheckToolboxFirst();
	Check( windowOpen, "viewlog 1 opens the console window" );
	Check( EventIndex( "console window" ) > EventIndex( "Com_Init" ), "the window opens at Sys_ShowConsole" );
	Check( strstr( windowText, "engine: after viewlog\n" ) != NULL, "engine output shows with viewlog 1" );
	Check( strstr( windowText, "engine: before viewlog\n" ) == NULL, "output before viewlog stays hidden" );
	Check( strstr( windowText, "Com_Init: after viewlog\n" ) != NULL, "Sys_LogPrintf shows with viewlog 1" );

	/* a console opened mid-game leaves the current port on the game window */
	result = RunMain( "console opened mid-game", 0, NULL, NULL, 0 );
	Check( result == -1 && !windowOpen, "no console window at launch" );
	SetPort( &gamePort );
	Sys_ShowConsole( 1, qfalse );	/* viewlog 1 at runtime */
	Check( windowOpen, "viewlog 1 at runtime opens the console window" );
	Check( currentPort == &gamePort, "opening the console window restores the current port" );
	Sys_Print( "engine: mid-game\n" );
	Check( strstr( windowText, "engine: mid-game\n" ) != NULL, "engine output reaches the window" );
	Check( currentPort == &gamePort, "writing to the console window keeps the current port" );
	/* #19: DoUpdate redraws the console window, and only that window */
	Sys_ConsoleDraw( &gamePort );
	Check( windowDraws == 0, "an update of the game window does not draw the console" );
	Sys_ConsoleDraw( &consolePort );
	Check( windowDraws == 1, "an update of the console window redraws it" );

	/* Shift held: the prompt is shown in the window and the typed line used */
	result = RunMain( "Shift prompt", 1, "+set s_initsound 1\n", "+set fs_game ignored\n", 0 );
	Check( result == -1, "Com_Init is reached" );
	CheckToolboxFirst();
	CheckGetKeysAfterToolbox();
	Check( windowOpen, "Shift opens the console window" );
	Check( EventIndex( "console window" ) > EventIndex( "GetKeys" ), "the window opens after GetKeys" );
	Check( strstr( windowText, "Enemy Territory startup parameters" ) != NULL, "the prompt is in the window" );
	Check( !strcmp( comInitCommandLine, "+set s_initsound 1" ), "the typed line reaches Com_Init" );
	/* viewlog 0 then hides the console again: the window stays, but its
	 * output stops, as retail's Sys_Print stopped printing */
	Check( strstr( windowText, "Com_Init: after viewlog\n" ) == NULL,
		"after viewlog 0, Sys_LogPrintf no longer reaches the Shift prompt's window" );
	Check( strstr( windowText, "engine: after viewlog\n" ) == NULL,
		"after viewlog 0, engine output no longer reaches the window" );
	Check( strstr( ring, "Com_Init: after viewlog\n" ) != NULL, "the crash ring still has it" );
	FakeFprintf( stderr, "engine: error after viewlog\n" );
	Check( strstr( windowText, "engine: error after viewlog\n" ) != NULL,
		"errors on stderr still reach the window after viewlog 0" );

	/* #24/#486: an overlong parameters file stops before Com_Init, in the window */
	memset( longFile, 'a', sizeof( longFile ) - 1 );
	longFile[sizeof( longFile ) - 1] = '\0';
	result = RunMain( "overlong parameters file", 0, NULL, longFile, 0 );
	Check( result == 1 && comInitCalls == 0, "main returns 1 before Com_Init" );
	CheckToolboxFirst();
	Check( windowOpen, "the error opens the console window" );
	Check( strstr( windowText, "exceeds 1023 bytes" ) != NULL, "the error is in the window" );
	Check( strstr( windowText, "Press Return to quit." ) != NULL, "the Return prompt is in the window" );

	/* the same for an overlong typed line */
	memset( longLine, 'b', sizeof( longLine ) - 2 );
	longLine[sizeof( longLine ) - 2] = '\n';
	longLine[sizeof( longLine ) - 1] = '\0';
	result = RunMain( "overlong typed line", 1, longLine, NULL, 0 );
	Check( result == 1 && comInitCalls == 0, "main returns 1 before Com_Init" );
	CheckToolboxFirst();
	Check( strstr( windowText, "startup parameters exceed 1023 bytes" ) != NULL, "the error is in the window" );
	Check( strstr( windowText, "Press Return to quit." ) != NULL, "the Return prompt is in the window" );
	/* #486 review: the rest of the overlong line is not the Return */
	Check( EventCount( "read" ) == 2 && !strcmp( events[numEvents - 1], "read" ),
		"main drops the rest of the typed line and waits for a new Return before it quits" );

	/* a static module failure is reported in the window, after the Toolbox */
	numEvents = 0;
	currentCase = "static module error";
	WriteParms( NULL );
	CloseWindow();
	toolboxReady = 0;
	typed = NULL;
	outputBeforeToolbox = 0;
	consoleDisplayed = qfalse;
	staticModulesError = "static module failure";
	{
		static char	*args[] = { "Quake3", NULL };

		if ( !setjmp( comInitDone ) ) {
			result = Sys_TestMacMain( 1, args );
		} else {
			result = -1;
		}
	}
	Check( result == 1, "main returns 1" );
	Check( !outputBeforeToolbox, "no output before the Toolbox is initialized" );
	Check( EventIndex( "VM_InitStaticModules" ) > EventIndex( "InitCursor" ), "the Toolbox is initialized first" );
	Check( windowOpen && strstr( windowText, "static module failure" ) != NULL, "the error is in the window" );
	Check( strstr( windowText, "Press Return to quit." ) != NULL, "the Return prompt is in the window" );
	Check( EventIndex( "read" ) > EventIndex( "console window" ), "main waits for Return before it quits" );
	Check( EventIndex( "GetKeys" ) < 0, "main stops before the rest of startup" );
	staticModulesError = NULL;

	/* Sys_Error with the console hidden: the crash log is written before
	 * stderr creates the window, then the Stop alert shows the message */
	RunSysError( "Sys_Error", qfalse, "fatal %d", 7 );
	Check( EventIndex( "crash log" ) >= 0 && ringHadErrorAtDump, "the crash log has the error" );
	Check( EventIndex( "console window" ) > EventIndex( "crash log" ),
		"the crash log is written before the console window is created" );
	Check( strstr( windowText, "Sys_Error: fatal 7\n" ) != NULL, "the error is in the console window" );
	Check( EventIndex( "alert" ) > EventIndex( "Sys_ShutdownInput" ), "the alert follows the input shutdown" );
	/* issue #5: a recursive error never reaches S_Shutdown, and the Sound
	 * Manager would loop the last mixed chunks under the alert */
	Check( EventIndex( "SNDDMA_Shutdown" ) >= 0 && EventIndex( "alert" ) > EventIndex( "SNDDMA_Shutdown" ),
		"Sys_Error closes the sound channel before the alert" );
	Check( !strcmp( alertTitle, "Enemy Territory Error:" ) && !strcmp( alertMessage, "fatal 7" ),
		"the alert shows the error, as retail" );
	Check( exitStatus == 1, "Sys_Error exits with 1" );
	Check( EventIndex( "Sys_BootLogFlush" ) >= 0 && EventIndex( "Sys_BootLogFlush" ) < EventIndex( "alert" ),
		"the boot log is on disk before the alert waits for the player" );
	Check( EventIndex( "Sys_BootLogClose" ) > EventIndex( "alert" ), "and closed when the game exits" );

	/* a message longer than a Pascal string is cut at 255 bytes */
	memset( longLine, 'c', 300 );
	longLine[300] = '\0';
	RunSysError( "Sys_Error long message", qfalse, "%s", longLine );
	Check( strlen( alertMessage ) == 255 && !strncmp( alertMessage, longLine, 255 ),
		"the alert shows the first 255 bytes" );

	/* a recursive error still holds the display: give it back, then show the alert */
	RunSysError( "Sys_Error fullscreen", qtrue, "recursive" );
	Check( EventIndex( "crash log" ) >= 0 && EventIndex( "crash log" ) < EventIndex( "GLimp_ReleaseDisplay" ),
		"the crash log is safe before the display is touched" );
	Check( EventIndex( "GLimp_ReleaseDisplay" ) >= 0 && EventIndex( "GLimp_ReleaseDisplay" ) < EventIndex( "alert" ) && exitStatus == 1,
		"Sys_Error releases a captured display, then shows the alert" );
	Check( EventIndex( "SNDDMA_Shutdown" ) >= 0, "Sys_Error closes the sound channel before it exits" );

	CloseWindow();

	printf( "mac toolbox init regression passed\n" );
	return 0;
}
