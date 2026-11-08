/* The botlib preprocessor (src/botlib/l_precomp.c, l_script.c) reads every
   .menu, .char, .weap and hud file the UI, cgame and game load, from pk3s a
   server can supply.  Crafted files go through the REAL LoadSourceFile,
   PC_ReadToken and PC_*Handle code.  Cases adapted from Q3A's
   tests/bot_preprocessor_bounds_regression.c and bot_macro_regression.c.

   Each case runs in its own process, on a 1 MiB thread stack (the Mac OS 9
   stack has no guard page) under an alarm.  Memory comes from ET's real zone
   allocator (extracted from src/qcommon/common.c by run.sh) through botlib's
   real l_memory.c, in a zone the size of the Mac's (10 MB) of which a map
   holds 2 MB.  Running out, or finding no free block large enough in a
   fragmented zone, is a fatal Z_Malloc error, so the old code fails a case by
   a sanitizer report, a stack overflow, a hang, a fatal allocation or a
   missing refusal. */
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../../src/game/q_shared.h"
#include "../../src/game/botlib.h"
#include "../../src/botlib/l_script.h"
#include "../../src/botlib/l_precomp.h"
#include "../../src/botlib/l_memory.h"
#include ZONE_INC

#ifdef PC_OWNER_UI
#define LOAD( name ) PC_LoadSourceHandle( name, PC_OWNER_UI )
#else
#define LOAD( name ) PC_LoadSourceHandle( name )
#endif

#define CASE_STACK ( 1024 * 1024 )
#define MAX_FILES 160
#define MAX_TEXT ( 4 * 1024 * 1024 )
/* __MACOS__ DEF_COMZONEMEGS is 10; the stock maps use about 2 MB of it */
#define ZONE_BYTES ( 10 * 1024 * 1024 )
#define MAP_BYTES ( 2 * 1024 * 1024 )

botlib_import_t botimport;
extern int numtokens;
static int failures, errors, warnings, liveOwners;
static char lastError[1024];
static char *fileNames[MAX_FILES], *fileTexts[MAX_FILES];
static int numFiles;
static int openText[64];
static char text[MAX_TEXT];
static size_t used;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		printf( "    FAIL: %s (errors=%d last=\"%.120s\")\n", what, errors, lastError );
		failures++;
	}
}

/* botlib's allocations through the engine's imports, as sv_bot.c makes them */
static void *ZoneGetMemory( int size ) {
	void *p = Z_TagMalloc( size, TAG_BOTLIB );
	liveOwners++;
	return p;
}
#ifdef HAVE_Z_TRY
static void *ZoneTryGetMemory( int size ) {
	void *p = Z_TryTagMalloc( size, TAG_BOTLIB );
	if ( p ) {
		liveOwners++;
	}
	return p;
}
#endif
static void ZoneFreeMemory( void *p ) {
	Z_Free( p );
	liveOwners--;
}
static void *ZoneHunkAlloc( int size ) { (void)size; return NULL; }
void QDECL Log_Write( char *fmt, ... ) { (void)fmt; }
void QDECL Com_Error( int level, const char *fmt, ... ) {
	va_list a;
	(void)level;
	printf( "    FAIL: fatal error: " );
	va_start( a, fmt );
	vprintf( fmt, a );
	va_end( a );
	printf( "\n" );
	fflush( stdout );
	_exit( 3 );
}
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

static void QDECL Print( int level, char *fmt, ... ) {
	va_list a;
	if ( level == PRT_ERROR || level == PRT_FATAL ) {
		errors++;
		va_start( a, fmt );
		vsnprintf( lastError, sizeof( lastError ), fmt, a );
		va_end( a );
	} else {
		warnings++;
	}
}
static int FileOpen( const char *path, fileHandle_t *f, fsMode_t mode ) {
	int i, h;
	(void)mode;
	*f = 0;
	for ( i = 0; i < numFiles; i++ ) {
		if ( strcmp( fileNames[i], path ) ) {
			continue;
		}
		for ( h = 1; h < 64 && openText[h]; h++ ) ;
		if ( h >= 64 ) {
			return -1;
		}
		openText[h] = i + 1;
		*f = h;
		return (int)strlen( fileTexts[i] );
	}
	return -1;
}
static int FileRead( void *out, int len, fileHandle_t f ) { memcpy( out, fileTexts[openText[f] - 1], len ); return len; }
static void FileClose( fileHandle_t f ) { openText[f] = 0; }

static void AddFile( const char *name, const char *contents ) {
	if ( numFiles >= MAX_FILES ) {
		Check( 0, "fixture file table" );
		return;
	}
	fileNames[numFiles] = strdup( name );
	fileTexts[numFiles] = strdup( contents );
	numFiles++;
}
static void Begin( void ) { used = 0; text[0] = '\0'; }
static void Append( const char *fmt, ... ) {
	va_list a;
	int n;
	va_start( a, fmt );
	n = vsnprintf( text + used, sizeof( text ) - used, fmt, a );
	va_end( a );
	if ( n < 0 || (size_t)n >= sizeof( text ) - used ) {
		Check( 0, "fixture text fits" );
		return;
	}
	used += n;
}
static source_t *Load( const char *name ) {
	source_t *source;
	errors = warnings = 0;
	lastError[0] = '\0';
	PC_SetBaseFolder( "" );
	source = LoadSourceFile( name );
	Check( source != NULL, "crafted source loads" );
	return source;
}
static int Read( source_t *source, token_t *token ) {
	return source && PC_ReadToken( source, token );
}
static void Expect( source_t *source, int type, const char *string ) {
	token_t token;
	if ( !Read( source, &token ) ) {
		Check( 0, "expected token is read" );
		return;
	}
	if ( token.type != type || strcmp( token.string, string ) ) {
		printf( "    got type %d \"%.60s\", want type %d \"%.60s\"\n", token.type, token.string, type, string );
		Check( 0, "token matches the golden" );
	}
}
static void ExpectEnd( source_t *source ) {
	token_t token;
	Check( !Read( source, &token ), "source ends after the golden tokens" );
	Check( !errors, "golden source reports no error" );
}
static int ReadUntilFailure( source_t *source, int limit ) {
	token_t token;
	int n = 0;
	while ( n < limit && Read( source, &token ) ) n++;
	return n;
}
static void ExpectRefusal( source_t *source, int published, int limit, const char *diagnostic ) {
	Check( published < limit, "the read fails" );
	Check( errors == 1 && strstr( lastError, diagnostic ) != NULL, diagnostic );
	if ( source ) {
		Check( !source->tokens, "queued expansion is released on failure" );
	}
}
static void Finish( source_t *source ) {
	if ( source ) {
		FreeSource( source );
	}
	Check( !numtokens, "every token copy is released" );
	Check( !liveOwners, "every script, source and token allocation is released" );
}

/* --- source names: a CS_CHARACTERS configstring reaches trap_PC_LoadSource */

static void LongNames( void ) {
	char name[2100], include[256], out[MAX_QPATH];
	pc_token_t pc;
	int handle, line;

	/* the 63-character prefix is a real file; the old code opened it for the
	   longer name and then copied the long name into 64- and 1024-byte buffers */
	memset( name, 'c', sizeof( name ) );
	memcpy( name, "characters/", 11 );
	name[63] = '\0';
	AddFile( name, "characterDef { skin \"x\" }\n" );
	name[63] = 'c';
	name[100] = '\0';
	handle = LOAD( name );
	if ( handle ) {
		PC_SourceFileAndLine( handle, out, &line );
		PC_FreeSourceHandle( handle );
	}
	Check( !handle, "a 100-character name is refused, not opened as its 63-character prefix" );
	name[100] = 'c';
	name[2099] = '\0';
	handle = LOAD( name );
	if ( handle ) {
		PC_FreeSourceHandle( handle );
	}
	Check( !handle, "a 2099-character name is refused" );
	/* 63 characters still load, and the name fits a module's MAX_QPATH buffer */
	name[63] = '\0';
	handle = LOAD( name );
	Check( handle > 0, "a 63-character name loads" );
	if ( handle ) {
		Check( PC_ReadTokenHandle( handle, &pc ) && !strcmp( pc.string, "characterDef" ), "its first token" );
		Check( PC_SourceFileAndLine( handle, out, &line ) && !strcmp( out, name ), "its name fits MAX_QPATH" );
		PC_FreeSourceHandle( handle );
	}
	/* an #include of a long name is refused the same way */
	memset( include, 'i', sizeof( include ) );
	include[63] = '\0';
	AddFile( include, "inner\n" );
	include[63] = 'i';
	include[200] = '\0';
	Begin();
	Append( "#include \"%s\"\nafter\n", include );
	AddFile( "inc.menu", text );
	{
		source_t *source = Load( "inc.menu" );
		Check( !ReadUntilFailure( source, 5 ) && errors == 1 && strstr( lastError, "not found" ), "a long #include name is not found" );
		Finish( source );
	}
	Finish( NULL );
}

/* --- token pasting and stringizing (Q3A 08460fb3) --------------------------- */

static void Pasting( void ) {
	source_t *source;
	token_t *a, *b;
	int i;

	Begin();
	Append( "#define M(x, y) x ## y\n#define Q(x, y) x ## y\nmarker M(" );
	for ( i = 0; i < 800; i++ ) Append( "a" );
	Append( ", " );
	for ( i = 0; i < 800; i++ ) Append( "b" );
	Append( ") tail\n" );
	AddFile( "paste.menu", text );
	source = Load( "paste.menu" );
	Expect( source, TT_NAME, "marker" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "can't merge" );
	Finish( source );

	Begin();
	Append( "#define M(x, y) x ## y\nmarker M(\"" );
	for ( i = 0; i < 800; i++ ) Append( "a" );
	Append( "\", \"" );
	for ( i = 0; i < 800; i++ ) Append( "b" );
	Append( "\") tail\n" );
	AddFile( "pastes.menu", text );
	source = Load( "pastes.menu" );
	Expect( source, TT_NAME, "marker" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "can't merge" );
	Finish( source );

	/* goldens: retail name/name, name/number and string/string pastes */
	AddFile( "pasteok.menu", "#define M(x, y) x ## y\nM(native, Suffix) M(native, 123) M(\"a\", \"b\") M(\"\", \"b\") end\n" );
	source = Load( "pasteok.menu" );
	Expect( source, TT_NAME, "nativeSuffix" );
	Expect( source, TT_NAME, "native123" );
	Expect( source, TT_STRING, "\"abb\"" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );

	/* a quoteless string token must not index before its buffer */
	a = malloc( sizeof( token_t ) );
	b = malloc( sizeof( token_t ) );
	memset( a, 0, sizeof( *a ) );
	memset( b, 0, sizeof( *b ) );
	a->type = b->type = TT_STRING;
	strcpy( b->string, "\"b\"" );
	Check( !PC_MergeTokens( a, b ), "an empty string token is not merged" );
	free( a );
	free( b );
}

static void Stringizing( void ) {
	source_t *source;
	pc_token_t pc;
	int handle, i;

	/* 1100 one-character tokens stringize to more than a token holds; the old
	   code dropped the closing quote and left the string unterminated, and
	   the trap copy then overran the module's pc_token_t */
	Begin();
	Append( "#define S(x) #x\nmarker S(" );
	for ( i = 0; i < 1100; i++ ) Append( "a " );
	Append( ") tail\n" );
	AddFile( "ui/stringize.menu", text );
	errors = 0;
	lastError[0] = '\0';
	PS_SetBaseFolder( "" );
	handle = LOAD( "ui/stringize.menu" );
	Check( handle > 0, "menu loads" );
	if ( handle ) {
		Check( PC_ReadTokenHandle( handle, &pc ) && !strcmp( pc.string, "marker" ), "marker" );
		Check( !PC_ReadTokenHandle( handle, &pc ) && errors == 1 && strstr( lastError, "can't stringize" ), "an oversized stringize is refused" );
		PC_FreeSourceHandle( handle );
	}
	Finish( NULL );

	/* the longest that fits: 1021 characters and two quotes */
	Begin();
	Append( "#define S(x) #x\nS(" );
	for ( i = 0; i < 1021; i++ ) Append( "a " );
	Append( ") gap S(native 123) end\n" );
	AddFile( "stringok.menu", text );
	source = Load( "stringok.menu" );
	{
		token_t token;
		Check( Read( source, &token ) && token.type == TT_STRING && strlen( token.string ) == 1023 &&
			   token.string[0] == '"' && token.string[1022] == '"', "a 1023-character stringize is complete" );
	}
	Expect( source, TT_NAME, "gap" );
	Expect( source, TT_STRING, "\"native123\"" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );
}

/* --- adjacent strings: the old join recursed once per string (Q3A 06180353) */

static void AdjacentStrings( void ) {
	source_t *source;
	int i;

	Begin();
	Append( "#define E \"\"\n#define S \"s\"\n" );
	for ( i = 0; i < 5000; i++ ) Append( "E " );
	Append( "S \"t\" S end\n" );
	AddFile( "strings.menu", text );
	source = Load( "strings.menu" );
	Expect( source, TT_STRING, "\"sts\"" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );

	/* ET skips a string in a skipped block before joining: keep that */
	AddFile( "skip.menu", "#define E \"a\"\nE\n#if 0\n\"x\"\n#endif\nE end\n#ifdef NOPE\nE\n#else\n\"g\"\n#endif\n\"h\" last\n" );
	source = Load( "skip.menu" );
	Expect( source, TT_STRING, "\"aa\"" );
	Expect( source, TT_NAME, "end" );
	Expect( source, TT_STRING, "\"gh\"" );
	Expect( source, TT_NAME, "last" );
	ExpectEnd( source );
	Finish( source );

	Begin();
	Append( "#define X \"x\"\n" );
	for ( i = 0; i < 2000; i++ ) Append( "X " );
	AddFile( "long.menu", text );
	source = Load( "long.menu" );
	Check( !ReadUntilFailure( source, 10 ), "an overlong join fails" );
	Check( errors == 1 && strstr( lastError, "MAX_TOKEN" ) != NULL, "one MAX_TOKEN diagnostic" );
	Finish( source );
}

/* --- macro calls nested in macro arguments (ET's PC_ReadDefineParms) -------- */

static void NestedArguments( int calls, int accepted ) {
	source_t *source;
	int i;

	Begin();
	Append( "#define F(x) x\nmarker " );
	for ( i = 0; i < calls; i++ ) Append( "F(" );
	Append( "1" );
	for ( i = 0; i < calls; i++ ) Append( ")" );
	Append( " tail\n" );
	AddFile( "nest.menu", text );
	source = Load( "nest.menu" );
	Expect( source, TT_NAME, "marker" );
	if ( accepted ) {
		Expect( source, TT_NUMBER, "1" );
		Expect( source, TT_NAME, "tail" );
		ExpectEnd( source );
	} else {
		ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "nested deeper than" );
	}
	Finish( source );
}

/* --- macro cycles and expansion (Q3A 06180353) ---------------------------- */

static void Cycles( const char *body, const char *diagnostic ) {
	source_t *source;

	AddFile( "cycle.menu", body );
	source = Load( "cycle.menu" );
	Expect( source, TT_NAME, "marker" );
	ExpectRefusal( source, ReadUntilFailure( source, 100000 ), 4096, diagnostic );
	Finish( source );
}

static void Exponential( void ) {
	int i;
	Begin();
	Append( "#define e0 x\n" );
	for ( i = 1; i <= 24; i++ ) Append( "#define e%d e%d e%d\n", i, i - 1, i - 1 );
	Append( "marker e24 tail\n" );
	Cycles( text, "macro expansion exceeds" );
}

/* --- includes (Q3A 06180353, fa39407b) ------------------------------------ */

static void IncludeChain( int files, int accepted ) {
	char name[64], body[128];
	source_t *source;
	int i;

	for ( i = 0; i < files; i++ ) {
		snprintf( name, sizeof( name ), "inc%d.h", i );
		if ( i + 1 < files ) {
			snprintf( body, sizeof( body ), "#include \"inc%d.h\"\n", i + 1 );
		} else {
			snprintf( body, sizeof( body ), "leaf\n" );
		}
		AddFile( name, body );
	}
	AddFile( "chain.menu", "first\n#include \"inc0.h\"\nafter\n" );
	source = Load( "chain.menu" );
	Expect( source, TT_NAME, "first" );
	if ( accepted ) {
		Expect( source, TT_NAME, "leaf" );
		Expect( source, TT_NAME, "after" );
		ExpectEnd( source );
	} else {
		ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "active source files" );
	}
	Finish( source );
}

static void FanOut( void ) {
	char name[64], body[128];
	source_t *source;
	int i;

	/* each file includes the next one twice: 2^40 includes at depth 42 */
	for ( i = 0; i <= 40; i++ ) {
		snprintf( name, sizeof( name ), "fan%d.h", i );
		if ( i < 40 ) {
			snprintf( body, sizeof( body ), "#include \"fan%d.h\"\n#include \"fan%d.h\"\n", i + 1, i + 1 );
		} else {
			snprintf( body, sizeof( body ), "leaf\n" );
		}
		AddFile( name, body );
	}
	AddFile( "fan.menu", "first\n#include \"fan0.h\"\nafter\n" );
	source = Load( "fan.menu" );
	Expect( source, TT_NAME, "first" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 << 20 ), 256, "more than 256 included files" );
	Finish( source );

	AddFile( "one.h", "leaf\n" );
	Begin();
	for ( i = 0; i < 256; i++ ) Append( "#include \"one.h\"\n" );
	Append( "end\n" );
	AddFile( "seq.menu", text );
	source = Load( "seq.menu" );
	for ( i = 0; i < 256; i++ ) Expect( source, TT_NAME, "leaf" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );
}

/* --- expressions, live tokens and conditionals (Q3A fa39407b, f9f73568) ---- */

static void Expressions( int hash ) {
	source_t *source;
	int i;

	/* 600 copies of a 199-token macro: 120,000 copied operands, where the
	   evaluator can use 64 values */
	Begin();
	Append( "#define A 1" );
	for ( i = 0; i < 99; i++ ) Append( " + 1" );
	Append( hash ? "\nmarker\n#if A" : "\nmarker $evalint(A" );
	for ( i = 1; i < 600; i++ ) Append( " + A" );
	Append( hash ? "\nyes\n#endif\ntail\n" : ") tail\n" );
	AddFile( "operands.menu", text );
	source = Load( "operands.menu" );
	Expect( source, TT_NAME, "marker" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "expression longer than 1024 tokens" );
	Finish( source );
}

static void ExpressionGoldens( void ) {
	source_t *source;
	int i;

	Begin();
	Append( "#define W 640\n#if " );
	for ( i = 0; i < 400; i++ ) Append( "(" );
	Append( "defined W && W > 100" );
	for ( i = 0; i < 400; i++ ) Append( ")" );
	Append( "\nyes $evalint(W / 2) $evalfloat(W * 0.5)\n#endif\nend\n" );
	AddFile( "expr.menu", text );
	source = Load( "expr.menu" );
	Expect( source, TT_NAME, "yes" );
	Expect( source, TT_NUMBER, "320" );
	Expect( source, TT_NUMBER, "320.00" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );

	/* failed expressions release their operands */
	AddFile( "exprbad.menu", "#if 1 +\nx\n#endif\n" );
	source = Load( "exprbad.menu" );
	Check( !ReadUntilFailure( source, 5 ) && errors == 1, "a bad expression fails once" );
	Finish( source );

	/* "defined" at the end of the line */
	AddFile( "defined.menu", "#if defined\nx\n#endif\nend\n" );
	source = Load( "defined.menu" );
	Check( !ReadUntilFailure( source, 5 ), "#if defined without a name fails" );
	Check( errors == 1 && strstr( lastError, "defined without name" ), "one diagnostic" );
	Finish( source );
}

static void LiveTokens( void ) {
	source_t *source;
	int i, j;

	/* 1000 definitions of 100 tokens: 100,000 retained copies, about 110 MB
	   of a 10 MB Mac zone */
	Begin();
	for ( i = 0; i < 1000; i++ ) {
		Append( "#define D%d", i );
		for ( j = 0; j < 100; j++ ) Append( " 1" );
		Append( "\n" );
	}
	Append( "marker\n" );
	AddFile( "defines.menu", text );
	source = Load( "defines.menu" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "too many tokens" );
	Finish( source );

	Begin();
	Append( "#define F(x) x\nmarker F(" );
	for ( i = 0; i < 100000; i++ ) Append( " 1" );
	Append( ") tail\n" );
	AddFile( "argument.menu", text );
	source = Load( "argument.menu" );
	Expect( source, TT_NAME, "marker" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "too many tokens" );
	Finish( source );

	/* retail legacy menus hold 6122 copies at their peak: 6200 fit */
	Begin();
	for ( i = 0; i < 61; i++ ) {
		Append( "#define BIG%d", i );
		for ( j = 0; j < 100; j++ ) Append( " b" );
		Append( "\n" );
	}
	Append( "BIG60 end\n" );
	AddFile( "big.menu", text );
	source = Load( "big.menu" );
	for ( j = 0; j < 100; j++ ) Expect( source, TT_NAME, "b" );
	Expect( source, TT_NAME, "end" );
	ExpectEnd( source );
	Finish( source );
}

static void EmptyDefines( void ) {
	source_t *source;
	int i;

	/* 120,000 definitions without a body: no token copies, but about 9 MB of
	   definitions in the zone */
	Begin();
	for ( i = 0; i < 120000; i++ ) Append( "#define d%d\n", i );
	Append( "marker\n" );
	AddFile( "empty.menu", text );
	source = Load( "empty.menu" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "out of memory" );
	Finish( source );
}

static void Fragmented( void ) {
	source_t *source;
	int i, j;

	/* 1500 definitions with 500-character names (a zone block of about
	   570 bytes each, too small for a token copy) between 1500 one-token
	   definitions; undefining the first leaves 0.8 MB of free holes, and a
	   long definition then exhausts the rest of the zone with that free */
	Begin();
	for ( i = 0; i < 1500; i++ ) {
		Append( "#define A%d_", i );
		for ( j = 0; j < 500; j++ ) Append( "a" );
		Append( "\n#define B%d x\n", i );
	}
	for ( i = 0; i < 1500; i++ ) {
		Append( "#undef A%d_", i );
		for ( j = 0; j < 500; j++ ) Append( "a" );
		Append( "\n" );
	}
	Append( "#define C" );
	for ( i = 0; i < 6000; i++ ) Append( " x" );
	Append( "\nmarker C\n" );
	AddFile( "fragment.menu", text );
	source = Load( "fragment.menu" );
	ExpectRefusal( source, ReadUntilFailure( source, 1 ), 1, "out of memory" );
#ifdef HAVE_Z_AVAILABLE
	Check( Z_AvailableMemory() > 512 * 1024, "the zone still had free bytes, only fragmented" );
#endif
	Finish( source );
}

static void RecursiveIncludes( void ) {
	source_t *source;
	int i;

	/* every line includes the file itself: each is refused as recursive,
	   but the old code loaded the whole file again for each one */
	Begin();
	for ( i = 0; i < 300; i++ ) Append( "#include \"self.menu\"\n" );
	Append( "end\n" );
	AddFile( "self.menu", text );
	source = Load( "self.menu" );
	Check( ReadUntilFailure( source, 5 ) == 0, "the source fails" );
	Check( strstr( lastError, "more than 256 included files" ) != NULL, "repeated recursive includes count against the include budget" );
	Finish( source );
}

static void Conditionals( int depth, int accepted ) {
	source_t *source;
	int i;

	Begin();
	for ( i = 0; i < depth; i++ ) Append( i & 1 ? "#ifndef NOPE\n" : "#if 1\n" );
	Append( "x\n" );
	for ( i = 0; i < depth; i++ ) Append( "#endif\n" );
	Append( "end\n" );
	AddFile( "nestif.menu", text );
	source = Load( "nestif.menu" );
	if ( accepted ) {
		Expect( source, TT_NAME, "x" );
		Expect( source, TT_NAME, "end" );
		ExpectEnd( source );
	} else {
		Check( !ReadUntilFailure( source, 10 ), "deep nesting fails" );
		Check( errors == 1 && strstr( lastError, "nested conditionals" ), "one nesting diagnostic" );
	}
	Finish( source );
}

/* --- handles: range checks and owner-tagged release (Q3A f9f73568) -------- */

static void Handles( void ) {
	pc_token_t pc;
	char out[MAX_QPATH];
	int line;

	Check( !PC_ReadTokenHandle( 0, &pc ) && !PC_ReadTokenHandle( -1, &pc ) && !PC_ReadTokenHandle( 64, &pc ), "out-of-range reads are refused" );
	Check( !PC_FreeSourceHandle( 0 ) && !PC_FreeSourceHandle( 64 ) && !PC_FreeSourceHandle( -5 ), "out-of-range frees are refused" );
	Check( !PC_SourceFileAndLine( 64, out, &line ) && !PC_SourceFileAndLine( 1, out, &line ), "out-of-range or closed names are refused" );
	PC_UnreadLastTokenHandle( 64 );
	PC_UnreadLastTokenHandle( -1 );
#ifdef PC_OWNER_UI
	{
		int ui, cgame, game;
		AddFile( "a.menu", "menuDef { name a\n" );
		AddFile( "b.weap", "weaponDef {\n" );
		AddFile( "c.char", "characterDef {\n" );
		PS_SetBaseFolder( "" );
		ui = PC_LoadSourceHandle( "a.menu", PC_OWNER_UI );
		cgame = PC_LoadSourceHandle( "b.weap", PC_OWNER_CGAME );
		game = PC_LoadSourceHandle( "c.char", PC_OWNER_GAME );
		Check( ui && cgame && game, "three modules open handles" );
		Check( PC_ReadTokenHandle( ui, &pc ) && PC_ReadTokenHandle( cgame, &pc ) && PC_ReadTokenHandle( game, &pc ), "each reads" );
		PC_FreeSourceHandles( PC_OWNER_CGAME );
		Check( !PC_ReadTokenHandle( cgame, &pc ), "the cgame's handle is freed with it" );
		Check( PC_ReadTokenHandle( ui, &pc ) && PC_ReadTokenHandle( game, &pc ), "the UI's and game's stay open" );
		PC_FreeSourceHandles( PC_OWNER_UI );
		PC_FreeSourceHandles( PC_OWNER_GAME );
		Check( !PC_ReadTokenHandle( ui, &pc ) && !PC_ReadTokenHandle( game, &pc ), "every module's handles are freed" );
	}
#endif
	{
		/* a UI that errors out of 63 menus leaks every slot; its shutdown has
		   to give them back, or no module can load a file afterwards */
		int h[64], n, next;
		AddFile( "leak.menu", "menuDef {\n" );
		PS_SetBaseFolder( "" );
		for ( n = 0; n < 63; n++ ) {
			h[n] = LOAD( "leak.menu" );
			Check( h[n] > 0, "a leaked handle opens" );
		}
		Check( !LOAD( "leak.menu" ), "the handle table is full" );
#ifdef PC_OWNER_UI
		PC_FreeSourceHandles( PC_OWNER_UI );
#endif
		next = LOAD( "leak.menu" );
		Check( next > 0, "a file loads after the UI that leaked its handles shuts down" );
		if ( next ) {
			PC_FreeSourceHandle( next );
		}
		for ( n = 0; n < 63; n++ ) {
			PC_FreeSourceHandle( h[n] );
		}
	}
	Finish( NULL );
}

/* --------------------------------------------------------------------------- */

typedef struct {
	const char *name;
	void ( *run )( int );
	int arg;
} testcase_t;

static void RunLongNames( int a ) { (void)a; LongNames(); }
static void RunPasting( int a ) { (void)a; Pasting(); }
static void RunStringizing( int a ) { (void)a; Stringizing(); }
static void RunAdjacent( int a ) { (void)a; AdjacentStrings(); }
static void RunNested( int a ) { NestedArguments( a < 0 ? -a : a, a < 0 ); }
static void RunPair( int a ) { (void)a; Cycles( "#define a b\n#define b a\nmarker a tail\n", "macro expansion exceeds" ); }
static void RunPairArg( int a ) { (void)a; Cycles( "#define a b\n#define b a\n#define F(x) x\nmarker F(a) tail\n", "macro expansion exceeds" ); }
static void RunFuncCycle( int a ) { (void)a; Cycles( "#define f(x) g(x x)\n#define g(x) f(x)\nmarker f(1) tail\n", "macro expansion exceeds" ); }
static void RunStream( int a ) { (void)a; Cycles( "#define a b\n#define b x a\nmarker a tail\n", "macro expansion exceeds" ); }
static void RunEvalCycle( int a ) { (void)a; Cycles( "#define a b\n#define b a\nmarker $evalint(a) tail\n", "macro expansion exceeds" ); }
static void RunExponential( int a ) { (void)a; Exponential(); }
static void RunChain( int a ) { IncludeChain( a < 0 ? -a : a, a < 0 ); }
static void RunFanOut( int a ) { (void)a; FanOut(); }
static void RunExpressions( int a ) { Expressions( a ); }
static void RunExprGoldens( int a ) { (void)a; ExpressionGoldens(); }
static void RunLive( int a ) { (void)a; LiveTokens(); }
static void RunConditionals( int a ) { Conditionals( a < 0 ? -a : a, a < 0 ); }
static void RunHandles( int a ) { (void)a; Handles(); }
static void RunEmptyDefines( int a ) { (void)a; EmptyDefines(); }
static void RunRecursive( int a ) { (void)a; RecursiveIncludes(); }
static void RunFragmented( int a ) { (void)a; Fragmented(); }

static const testcase_t cases[] = {
	{ "long source and include names are refused", RunLongNames, 0 },
	{ "token pasting is bounded", RunPasting, 0 },
	{ "stringizing is bounded", RunStringizing, 0 },
	{ "5000 adjacent strings join without recursion", RunAdjacent, 0 },
	{ "33 macro calls nested in arguments are accepted", RunNested, -33 },
	{ "2000 macro calls nested in arguments are refused", RunNested, 2000 },
	{ "mutual macro pair", RunPair, 0 },
	{ "mutual macro pair in a macro argument", RunPairArg, 0 },
	{ "function-like macro cycle", RunFuncCycle, 0 },
	{ "cycle publishing tokens forever", RunStream, 0 },
	{ "macro pair in $evalint", RunEvalCycle, 0 },
	{ "2^24-token exponential expansion", RunExponential, 0 },
	{ "63-file include chain is accepted", RunChain, -63 },
	{ "100-file include chain is refused", RunChain, 100 },
	{ "include fan-out stops after 256 includes", RunFanOut, 0 },
	{ "#if operand list", RunExpressions, 1 },
	{ "$evalint operand list", RunExpressions, 0 },
	{ "expression goldens, failures and #if defined", RunExprGoldens, 0 },
	{ "live token budget", RunLive, 0 },
	{ "4096 nested conditionals are accepted", RunConditionals, -4096 },
	{ "80000 nested conditionals are refused", RunConditionals, 80000 },
	{ "handle ranges and owner-tagged release", RunHandles, 0 },
	{ "120000 empty definitions are refused before the zone runs out", RunEmptyDefines, 0 },
	{ "300 recursive includes of a file count against the include budget", RunRecursive, 0 },
	{ "a fragmented zone refuses the source instead of failing fatally", RunFragmented, 0 },
};

static const testcase_t *current;
static void *Thread( void *arg ) { (void)arg; current->run( current->arg ); return NULL; }

int main( void ) {
	int i, status, bad = 0;
	pid_t pid;

	botimport.Print = Print;
	botimport.FS_FOpenFile = FileOpen;
	botimport.FS_Read = FileRead;
	botimport.FS_FCloseFile = FileClose;
	botimport.GetMemory = ZoneGetMemory;
	botimport.FreeMemory = ZoneFreeMemory;
	botimport.HunkAlloc = ZoneHunkAlloc;
#ifdef HAVE_Z_AVAILABLE
	botimport.AvailableMemory = Z_AvailableMemory;
#endif
#ifdef HAVE_Z_TRY
	botimport.TryGetMemory = ZoneTryGetMemory;
#endif
	mainzone = calloc( ZONE_BYTES, 1 );
	Z_ClearZone( mainzone, ZONE_BYTES );
	Z_TagMalloc( MAP_BYTES, TAG_GENERAL );
	setvbuf( stdout, NULL, _IONBF, 0 );
	for ( i = 0; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ); i++ ) {
		pid = fork();
		if ( pid == 0 ) {
			pthread_attr_t attr;
			pthread_t thread;
			current = &cases[i];
			alarm( 20 );
			pthread_attr_init( &attr );
			pthread_attr_setstacksize( &attr, CASE_STACK );
			if ( pthread_create( &thread, &attr, Thread, NULL ) ) {
				_exit( 4 );
			}
			pthread_join( thread, NULL );
			_exit( failures ? 1 : 0 );
		}
		waitpid( pid, &status, 0 );
		if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
			printf( "ok   %s\n", cases[i].name );
			continue;
		}
		bad++;
		if ( WIFSIGNALED( status ) && WTERMSIG( status ) == SIGALRM ) {
			printf( "FAIL %s: did not finish (hang)\n", cases[i].name );
		} else if ( WIFSIGNALED( status ) ) {
			printf( "FAIL %s: killed by signal %d\n", cases[i].name, WTERMSIG( status ) );
		} else {
			printf( "FAIL %s: exit %d\n", cases[i].name, WEXITSTATUS( status ) );
		}
	}
	if ( bad ) {
		printf( "botlib preprocessor: %d case(s) failed\n", bad );
		return 1;
	}
	printf( "botlib preprocessor: names, pastes, strings, macros, includes, expressions, conditionals and handles are bounded\n" );
	return 0;
}
