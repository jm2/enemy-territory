/* #266: Sys_GetClipboardData and Sys_ClipboardText (code/mac/mac_main.c),
 * against a fake Scrap Manager and Memory Manager.  Paste takes the first
 * line of the clipboard's TEXT, as win32 does, at most MAX_STRING_CHARS - 1
 * bytes, in a zone copy; nothing for an empty line or no TEXT; and every
 * handle it makes is disposed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_STRING_CHARS	1024
typedef long SInt32;
typedef char **Handle;
typedef unsigned int ResType;
#define kScrapFlavorTypeText	0x54455854u	/* 'TEXT' */

static char		*scrap;			/* the clipboard's TEXT, or NULL */
static long		scrapLength;
static long		scrapError;		/* GetScrap's result when not 0 */
static int		handles, zoneBlocks;
static int		failures;
static const char	*currentCase;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		fprintf( stderr, "Mac clipboard regression failed (%s): %s\n", currentCase, what );
		failures++;
	}
}

static Handle NewHandle( long size ) {
	Handle	h = malloc( sizeof( char * ) );
	*h = malloc( size > 0 ? size : 1 );
	handles++;
	return h;
}
static void DisposeHandle( Handle h ) { free( *h ); free( h ); handles--; }
static long GetScrap( Handle h, ResType type, SInt32 *offset ) {
	Check( type == kScrapFlavorTypeText, "asks for TEXT" );
	if ( scrapError ) {
		return scrapError;
	}
	if ( !scrap ) {
		return -102;	/* noTypeErr */
	}
	*h = realloc( *h, scrapLength );
	memcpy( *h, scrap, scrapLength );
	*offset = 0;
	return scrapLength;
}
static void *Z_Malloc( int size ) { zoneBlocks++; return calloc( 1, size ); }
static void Z_Free( void *p ) { zoneBlocks--; free( p ); }

#include "mac_clipboard_extracted.c"

static void CheckPaste( const char *name, const char *text, long length, const char *want ) {
	char	*got;

	currentCase = name;
	scrap = (char *)text;
	scrapLength = length;
	got = Sys_GetClipboardData();
	if ( want ) {
		Check( got && !strcmp( got, want ), "pastes the first line" );
	} else {
		Check( !got, "pastes nothing" );
	}
	if ( got ) {
		Z_Free( got );
	}
	Check( handles == 0 && zoneBlocks == 0, "frees what it allocates" );
}

int main( void ) {
	static char	big[3000];

	CheckPaste( "address", "192.168.1.5:27960", 17, "192.168.1.5:27960" );
	CheckPaste( "lines", "connect idnewt\rsecond line", 26, "connect idnewt" );
	CheckPaste( "lf", "first\nsecond", 12, "first" );
	CheckPaste( "backspace", "ab\bc", 4, "ab" );
	CheckPaste( "nul", "ab\0cd", 5, "ab" );
	CheckPaste( "leading break", "\rabc", 4, NULL );
	CheckPaste( "no text", NULL, 0, NULL );
	memset( big, 'x', sizeof( big ) );
	currentCase = "long";
	scrap = big;
	scrapLength = sizeof( big );
	{
		char	*got = Sys_GetClipboardData();
		Check( got && strlen( got ) == MAX_STRING_CHARS - 1, "stops at MAX_STRING_CHARS - 1" );
		if ( got ) {
			Z_Free( got );
		}
		Check( handles == 0 && zoneBlocks == 0, "frees what it allocates" );
	}
	currentCase = "error";
	scrapError = -108;	/* memFullErr: too big for the heap */
	scrap = big;
	Check( Sys_GetClipboardData() == NULL && handles == 0 && zoneBlocks == 0, "an error pastes nothing" );
	if ( failures ) {
		return 1;
	}
	puts( "mac clipboard regression passed" );
	return 0;
}
