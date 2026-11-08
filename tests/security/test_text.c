/* Shared text handling: COM_ParseExt's quoted tokens at and past MAX_TOKEN_CHARS and
 * an unterminated quote, Info_SetValueForKey(_Big) at exactly the buffer size, and
 * tab completion's FindMatches (extracted from common.c) with candidates longer than
 * shortestMatch, as a server's systeminfo can create. Run under ASan.
 * An argument runs one case only. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"

void QDECL Com_Error( int code, const char *fmt, ... ) { fprintf( stderr, "FAIL: unexpected Com_Error: %s\n", fmt ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

#include FINDMATCHES_INC

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		printf( "FAIL: %s\n", what );
		exit( 1 );
	}
}

/* a heap buffer of exactly the text and its NUL, so ASan sees any read past it */
static char *Text( const char *s ) {
	char *b = malloc( strlen( s ) + 1 );
	strcpy( b, s );
	return b;
}

static char *Run( char c, int n ) {
	char *b = malloc( n + 1 );
	memset( b, c, n );
	b[n] = 0;
	return b;
}

/* "<n x's>" then a following token */
static void Quoted( int n ) {
	char *body = Run( 'x', n ), *text = malloc( n + 16 ), *p, *t;

	sprintf( text, "\"%s\" next", body );
	p = text;
	t = COM_ParseExt( &p, qtrue );
	Check( (int)strlen( t ) == ( n < MAX_TOKEN_CHARS ? n : MAX_TOKEN_CHARS - 1 ), "quoted token kept up to MAX_TOKEN_CHARS - 1" );
	Check( !strncmp( t, body, strlen( t ) ), "quoted token content" );
	t = COM_ParseExt( &p, qtrue );
	Check( !strcmp( t, "next" ), "token after a long quoted token" );
	free( body );
	free( text );
}

static void ParseQuoted( void ) {
	char *body, *text, *p, *t;

	Quoted( 10 );
	Quoted( MAX_TOKEN_CHARS - 2 );
	Quoted( MAX_TOKEN_CHARS - 1 );  /* 1023 */
	Quoted( MAX_TOKEN_CHARS );      /* 1024 */
	Quoted( MAX_TOKEN_CHARS + 1 );  /* 1025 */
	Quoted( 4000 );
	/* string-in-string, short and long */
	text = Text( "\"say \\\"hi\\\" now\" after" );
	p = text;
	t = COM_ParseExt( &p, qtrue );
	Check( !strcmp( t, "say \"hi\" now" ), "string-in-string" );
	Check( !strcmp( COM_ParseExt( &p, qtrue ), "after" ), "token after string-in-string" );
	free( text );
	body = Run( 'y', 2000 );
	text = malloc( 4100 );
	sprintf( text, "\"\\\"%s\\\"%s\" after", body, body );
	p = text;
	t = COM_ParseExt( &p, qtrue );
	Check( (int)strlen( t ) == MAX_TOKEN_CHARS - 1 && t[0] == '"', "long string-in-string kept up to MAX_TOKEN_CHARS - 1" );
	Check( !strcmp( COM_ParseExt( &p, qtrue ), "after" ), "token after long string-in-string" );
	free( body );
	free( text );
	printf( "parse quoted: passed\n" );
}

static void ParseWord( void ) {
	char *body, *text, *p, *t;
	int n[] = { 1, MAX_TOKEN_CHARS - 1, MAX_TOKEN_CHARS, MAX_TOKEN_CHARS + 1, 4000 };
	int i;

	for ( i = 0; i < 5; i++ ) {
		body = Run( 'w', n[i] );
		text = malloc( n[i] + 8 );
		sprintf( text, "%s next", body );
		p = text;
		t = COM_ParseExt( &p, qtrue );
		/* unchanged behaviour: a word of MAX_TOKEN_CHARS or more is discarded */
		Check( n[i] < MAX_TOKEN_CHARS ? !strcmp( t, body ) : !t[0], "plain word" );
		Check( !strcmp( COM_ParseExt( &p, qtrue ), "next" ), "token after plain word" );
		free( body );
		free( text );
	}
	printf( "parse word: passed\n" );
}

static void Unterminated( const char *s, const char *want ) {
	char *text = Text( s ), *p = text, *q, *t;

	t = COM_ParseExt( &p, qtrue );
	Check( !strcmp( t, want ), "unterminated quote's token" );
	q = p;
	t = COM_ParseExt( &q, qtrue );    /* must not read past the buffer */
	Check( !t[0], "nothing after an unterminated quote" );
	Check( p == text + strlen( text ), "unterminated quote stops at the NUL" );
	free( text );
}

static void ParseUnterminated( void ) {
	Unterminated( "\"abc", "abc" );
	Unterminated( "\"", "" );
	Unterminated( "\"say \\\"hi", "say \"hi" );
	Unterminated( "\"say \\\"hi\\\"", "say \"hi\"" );
	printf( "parse unterminated: passed\n" );
}

static void InfoAt( int size, void ( *set )( char *, const char *, const char * ), int fill ) {
	/* "\k\<value>" is 3 + value bytes; fill s so the sum is exactly fill */
	char *s = calloc( 1, size ), *v;
	int base = size / 2;

	memset( s, 'a', base );
	s[0] = '\\';
	s[2] = '\\';
	s[1] = 'b';
	v = Run( 'v', fill - base - 3 );
	set( s, "k", v );
	if ( fill < size ) {
		Check( (int)strlen( s ) == fill, "a pair that fits is added" );
	} else {
		Check( (int)strlen( s ) == base, "a pair reaching the buffer size is refused" );
	}
	free( v );
	free( s );
}

static void Info( void ) {
	InfoAt( MAX_INFO_STRING, Info_SetValueForKey, MAX_INFO_STRING - 1 );
	InfoAt( MAX_INFO_STRING, Info_SetValueForKey, MAX_INFO_STRING );
	InfoAt( MAX_INFO_STRING, Info_SetValueForKey, MAX_INFO_STRING + 1 );
	InfoAt( BIG_INFO_STRING, Info_SetValueForKey_Big, BIG_INFO_STRING - 1 );
	InfoAt( BIG_INFO_STRING, Info_SetValueForKey_Big, BIG_INFO_STRING );
	printf( "info set value: passed\n" );
}

/* adapted from Quake III's tests/field_complete_regression.c (7ef6c86b) */
static void Start( const char *typed ) {
	completionString = typed;
	matchCount = 0;
	shortestMatch[0] = 0;
}

static char *Name( int length, int differAt, char differ ) {
	char *name = Run( 'a', length );

	if ( differAt >= 0 ) {
		name[differAt] = differ;
	}
	return name;
}

static void Complete( void ) {
	char *first, *second, *third, *two;

	first = Name( BIG_INFO_KEY - 1, MAX_TOKEN_CHARS + 100, 'x' );
	second = Name( BIG_INFO_KEY - 1, MAX_TOKEN_CHARS + 100, 'y' );
	third = Name( BIG_INFO_KEY - 1, -1, 0 );
	two = Name( 2000, -1, 0 );
	Start( "a" );
	FindMatches( first );
	FindMatches( second );
	FindMatches( third );
	FindMatches( two );
	Check( matchCount == 4, "every long candidate is counted" );
	Check( strlen( shortestMatch ) == MAX_TOKEN_CHARS - 1, "the common prefix fills the buffer" );

	Start( "a" );
	FindMatches( "aaab" );
	FindMatches( first );
	Check( matchCount == 2 && !strcmp( shortestMatch, "aaa" ), "a long candidate cuts at the first difference" );
	FindMatches( "aa" );
	Check( matchCount == 3 && !strcmp( shortestMatch, "aa" ), "a shorter candidate cuts at its end" );

	Start( "cl_" );
	FindMatches( "cl_allowDownload" );
	FindMatches( "CL_ALLOWlist" );
	FindMatches( "com_maxfps" );
	Check( matchCount == 2 && !strcmp( shortestMatch, "cl_allow" ), "common prefix of ordinary names" );
	free( first );
	free( second );
	free( third );
	free( two );
	printf( "find matches: passed\n" );
}

int main( int argc, char **argv ) {
	static const struct { const char *name; void ( *run )( void ); } cases[] = {
		{ "quoted", ParseQuoted }, { "word", ParseWord }, { "unterminated", ParseUnterminated },
		{ "info", Info }, { "complete", Complete },
	};
	int i;

	for ( i = 0; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ); i++ ) {
		if ( argc < 2 || !strcmp( argv[1], cases[i].name ) ) {
			cases[i].run();
		}
	}
	return 0;
}
