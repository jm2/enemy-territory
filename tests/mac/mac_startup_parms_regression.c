/* Issues #261 and #24: the classic Mac startup parameters.
 *
 * #261: Retro68 always calls main( 1, argv ), so the Mac build had no way to
 * pass "+set", "safe" or the CVAR_INIT startup cvars.  As in id's original
 * Classic Mac port, main now reads them from METParms.txt next to the
 * application (or a line typed at launch with Shift held).  The file's CR,
 * LF and CRLF line breaks must all become the '\n' console line breaks
 * Com_ParseCommandLine splits at, quoted values (with '+' or Mac Roman bytes
 * in them) must reach Cmd_TokenizeString byte for byte, other control bytes
 * (even NUL) must not cut the text short, and a missing or empty file must
 * leave the command line empty.
 *
 * #24: the command line is a fixed MAX_STRING_CHARS buffer.  Arguments and
 * files that exactly fill it (1023 bytes and the NUL) must be accepted with
 * their separators, and one byte more, a file of any larger size, or a CRLF
 * file that only fits before its line breaks are folded must be refused with
 * the command line left as it was.  The buffers here are heap blocks of
 * exactly the sizes main uses, so AddressSanitizer reports a write even one
 * byte past either of them.
 *
 * The runner extracts the real Sys_AppendStartupText and Sys_ReadStartupFile
 * (mac_main.c), Com_ParseCommandLine and Com_SafeMode (common.c) and
 * Cmd_TokenizeString, Cmd_Argc and Cmd_Argv (cmd.c) verbatim. */
#include "../../src/game/q_shared.h"
#include "et_compat.h"
#include "../../src/qcommon/qcommon.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMAND_SIZE		MAX_STRING_CHARS		/* main's commandLine */
#define TEXT_SIZE			( MAX_STRING_CHARS * 2 )	/* main's file buffer */
#define MAX_CONSOLE_LINES	32

/* common.c's command line state */
char	*com_consoleLines[MAX_CONSOLE_LINES];
int		com_numConsoleLines;

/* cmd.c's tokenizer state */
static int	cmd_argc;
static char	*cmd_argv[MAX_STRING_TOKENS];
static char	cmd_tokenized[BIG_INFO_STRING + MAX_STRING_TOKENS];
static char	cmd_cmd[BIG_INFO_STRING];

/* each step says what it is about to do, so a launch that stops here shows where */
static char	progress[256];

#undef Sys_LogPrintf	/* the Mac build has a real one (macintosh) */
void Sys_LogPrintf( const char *fmt, ... );
void Sys_LogPrintf( const char *fmt, ... ) {
	size_t	length = strlen( progress );
	va_list	args;

	va_start( args, fmt );
	vsnprintf( progress + length, sizeof( progress ) - length, fmt, args );
	va_end( args );
}

#include "mac_startup_extracted.c"
#include "com_command_line_extracted.c"
#include "cmd_tokenize_extracted.c"

static const char *currentCase = "setup";
static const char *testDir;
static char *commandLine;	/* exactly COMMAND_SIZE bytes */
static char *text;			/* exactly TEXT_SIZE bytes */
static int commandLength;

void QDECL Com_Error( int level, const char *fmt, ... ) {
	fprintf( stderr, "mac startup parms regression: Com_Error (%s): %s\n", currentCase, fmt );
	exit( 1 );
}

void QDECL Com_Printf( const char *fmt, ... ) {
}

static void Check( int ok, const char *message ) {
	if ( !ok ) {
		fprintf( stderr, "mac startup parms regression failed (%s): %s\n", currentCase, message );
		exit( 1 );
	}
}

static void Reset( const char *name ) {
	currentCase = name;
	memset( commandLine, 'X', COMMAND_SIZE );
	commandLine[0] = '\0';
	commandLength = 0;
}

static const char *WriteFile( const char *name, const char *data, int length ) {
	static char	path[4096];
	FILE		*f;

	snprintf( path, sizeof( path ), "%s/%s", testDir, name );
	f = fopen( path, "wb" );
	Check( f != NULL, "creates the parameters file" );
	Check( (int)fwrite( data, 1, length, f ) == length, "writes the parameters file" );
	fclose( f );
	return path;
}

static int ReadFile( const char *name, const char *data, int length ) {
	return Sys_ReadStartupFile( WriteFile( name, data, length ), text, TEXT_SIZE,
		commandLine, COMMAND_SIZE, &commandLength );
}

static int Append( const char *data, int length, char separator ) {
	return Sys_AppendStartupText( commandLine, COMMAND_SIZE, &commandLength,
		data, length, separator );
}

static void CheckCommandLine( const char *expected, int expectedLength ) {
	Check( commandLength == expectedLength, "command line length" );
	Check( !memcmp( commandLine, expected, expectedLength ), "command line bytes" );
	Check( commandLine[expectedLength] == '\0', "command line is NUL-terminated" );
}

/* What Com_Init would see: the non-empty console lines Com_ParseCommandLine
 * splits a copy of the command line into, each tokenized by
 * Cmd_TokenizeString, as "arg,arg|arg,arg". */
static const char *ConsoleLines( qboolean *safe ) {
	static char	copy[COMMAND_SIZE];
	static char	out[COMMAND_SIZE * 2];
	int			i, j;

	memcpy( copy, commandLine, commandLength + 1 );
	Com_ParseCommandLine( copy );
	out[0] = '\0';
	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( !Cmd_Argc() ) {
			continue;
		}
		if ( out[0] ) {
			Q_strcat( out, sizeof( out ), "|" );
		}
		for ( j = 0 ; j < Cmd_Argc() ; j++ ) {
			if ( j ) {
				Q_strcat( out, sizeof( out ), "," );
			}
			Q_strcat( out, sizeof( out ), Cmd_Argv( j ) );
		}
	}
	if ( safe ) {
		memcpy( copy, commandLine, commandLength + 1 );
		Com_ParseCommandLine( copy );
		*safe = Com_SafeMode();
	}
	return out;
}

static void TestMissingAndEmpty( void ) {
	char	path[4096];

	Reset( "missing file" );
	snprintf( path, sizeof( path ), "%s/no such file.txt", testDir );
	Check( Sys_ReadStartupFile( path, text, TEXT_SIZE, commandLine, COMMAND_SIZE,
		&commandLength ) == 0, "a missing file is reported as absent" );
	CheckCommandLine( "", 0 );

	Check( strstr( progress, "parameters: opening" ) && strstr( progress, "parameters: no file" ),
		"the log says a missing file was looked for" );

	Reset( "empty file" );
	progress[0] = 0;
	Check( ReadFile( "empty.txt", "", 0 ) == 1, "an empty file is read" );
	CheckCommandLine( "", 0 );
	{
		const char	*opening = strstr( progress, "parameters: opening" );
		const char	*reading = strstr( progress, "parameters: reading" );
		const char	*closing = strstr( progress, "parameters: read 0 bytes, closing" );

		Check( opening && reading && closing && opening < reading && reading < closing,
			"the log has each step of reading the file, in order" );
	}
	Check( !strcmp( ConsoleLines( NULL ), "" ), "an empty file gives no console lines" );

	Reset( "blank lines" );
	Check( ReadFile( "blank.txt", "\r\n\r\n  \r", 7 ) == 1, "a blank file is read" );
	Check( !strcmp( ConsoleLines( NULL ), "" ), "blank lines give no console lines" );
}

static void TestLineEndings( void ) {
	static const char	lf[] = "+set s_initsound 1\n+set fs_game mymod\nsafe\n";
	static const char	crlf[] = "+set s_initsound 1\r\n+set fs_game mymod\r\nsafe\r\n";
	static const char	cr[] = "+set s_initsound 1\r+set fs_game mymod\rsafe\r";
	static const char	mixed[] = "+set s_initsound 1\r\n+set fs_game mymod\rsafe\n";
	static const char	noPlus[] = "set com_hunkMegs 64\r\nset fs_game mymod\r\n";
	static const char	sameLine[] = "+set s_initsound 1 +set r_mode 3\r+map q3dm1\r";
	static const char	*expected = "set,s_initsound,1|set,fs_game,mymod|safe";
	const char			*files[4] = { lf, crlf, cr, mixed };
	const char			*names[4] = { "LF file", "CRLF file", "CR file", "mixed line endings" };
	qboolean			safe;
	int					i;

	for ( i = 0 ; i < 4 ; i++ ) {
		Reset( names[i] );
		Check( ReadFile( "parms.txt", files[i], strlen( files[i] ) ) == 1, "the file is read" );
		CheckCommandLine( lf, strlen( lf ) );
		Check( !strcmp( ConsoleLines( &safe ), expected ), "each line is its own console line" );
		Check( safe, "\"safe\" on its own line reaches Com_SafeMode" );
	}

	Reset( "lines without '+'" );
	Check( ReadFile( "parms.txt", noPlus, strlen( noPlus ) ) == 1, "the file is read" );
	Check( !strcmp( ConsoleLines( &safe ), "set,com_hunkMegs,64|set,fs_game,mymod" ),
		"a line break separates commands without '+'" );
	Check( !safe, "no safe mode without \"safe\"" );

	Reset( "several commands on a line" );
	Check( ReadFile( "parms.txt", sameLine, strlen( sameLine ) ) == 1, "the file is read" );
	Check( !strcmp( ConsoleLines( NULL ), "set,s_initsound,1|set,r_mode,3|map,q3dm1" ),
		"'+' still separates commands within a line" );
}

static void TestQuotingAndBytes( void ) {
	static const char	quoted[] = "+set sv_hostname \"A+B Server\" +set g_motd \"hi\"\r\n";
	static const char	macRoman[] = "+set name \"Caf\x8e \xd2Q\xd3\"\r";
	static const char	controls[] = "+set a 1\t\0+set b 2\x1b\x7f";
	qboolean			safe;

	Reset( "quoted values" );
	Check( ReadFile( "parms.txt", quoted, strlen( quoted ) ) == 1, "the file is read" );
	Check( !strcmp( ConsoleLines( &safe ), "set,sv_hostname,A+B Server|set,g_motd,hi" ),
		"a quoted '+' does not split the command" );

	Reset( "Mac Roman bytes" );
	Check( ReadFile( "parms.txt", macRoman, strlen( macRoman ) ) == 1, "the file is read" );
	CheckCommandLine( "+set name \"Caf\x8e \xd2Q\xd3\"\n", strlen( macRoman ) );
	Check( !strcmp( ConsoleLines( NULL ), "set,name,Caf\x8e \xd2Q\xd3" ),
		"Mac Roman bytes in a quoted value arrive unchanged" );

	Reset( "control bytes" );
	Check( ReadFile( "parms.txt", controls, sizeof( controls ) - 1 ) == 1, "the file is read" );
	CheckCommandLine( "+set a 1  +set b 2  ", sizeof( controls ) - 1 );
	Check( !strcmp( ConsoleLines( NULL ), "set,a,1|set,b,2" ),
		"a tab, NUL, escape or DEL becomes a space and does not cut the text short" );
}

static void FillArgument( char *argument, int length ) {
	memset( argument, 'a', length );
	argument[0] = '+';
	argument[length] = '\0';
}

static void TestArgumentBounds( void ) {
	static char	argument[COMMAND_SIZE * 2];
	static char	before[COMMAND_SIZE];

	Reset( "arguments" );
	Check( Append( "+set", 4, ' ' ) && Append( "s_initsound", 11, ' ' ) && Append( "1", 1, ' ' )
		&& Append( "safe", 4, ' ' ), "short arguments fit" );
	CheckCommandLine( "+set s_initsound 1 safe", 23 );
	Check( Append( "", 0, ' ' ), "an empty argument is accepted" );
	CheckCommandLine( "+set s_initsound 1 safe", 23 );

	Reset( "one exactly fitting argument" );
	FillArgument( argument, COMMAND_SIZE - 1 );
	Check( Append( argument, COMMAND_SIZE - 1, ' ' ), "a 1023-byte argument fits" );
	CheckCommandLine( argument, COMMAND_SIZE - 1 );
	Check( !Append( "x", 1, ' ' ), "no further argument fits" );
	CheckCommandLine( argument, COMMAND_SIZE - 1 );

	Reset( "one oversized argument" );
	FillArgument( argument, COMMAND_SIZE );
	Check( !Append( argument, COMMAND_SIZE, ' ' ), "a 1024-byte argument is refused" );
	CheckCommandLine( "", 0 );
	FillArgument( argument, COMMAND_SIZE * 2 - 1 );
	Check( !Append( argument, COMMAND_SIZE * 2 - 1, ' ' ), "a 2047-byte argument is refused" );
	CheckCommandLine( "", 0 );

	Reset( "exactly fitting with a separator" );
	FillArgument( argument, 1000 );
	Check( Append( argument, 1000, ' ' ), "the first argument fits" );
	FillArgument( argument, COMMAND_SIZE - 1 - 1000 - 1 );
	Check( Append( argument, COMMAND_SIZE - 1 - 1000 - 1, ' ' ), "the last byte is used" );
	Check( commandLength == COMMAND_SIZE - 1 && commandLine[1000] == ' ',
		"the separator is kept" );
	Check( commandLine[COMMAND_SIZE - 1] == '\0', "the command line is NUL-terminated" );

	Reset( "one byte over with a separator" );
	FillArgument( argument, 1000 );
	Check( Append( argument, 1000, ' ' ), "the first argument fits" );
	memcpy( before, commandLine, commandLength + 1 );
	FillArgument( argument, COMMAND_SIZE - 1 - 1000 );
	Check( !Append( argument, COMMAND_SIZE - 1 - 1000, ' ' ), "one byte over is refused" );
	CheckCommandLine( before, 1000 );

	Reset( "no room for the separator" );
	FillArgument( argument, COMMAND_SIZE - 1 );
	Check( Append( argument, COMMAND_SIZE - 1, ' ' ), "the first argument fits" );
	Check( !Append( "+", 1, '\n' ), "a separator alone does not fit" );
	CheckCommandLine( argument, COMMAND_SIZE - 1 );
}

static void TestFileBounds( void ) {
	static char	data[100000];
	static char	expected[COMMAND_SIZE];
	int			i;

	Reset( "exactly fitting file" );
	memset( data, 'a', COMMAND_SIZE - 1 );
	memcpy( data, "+set x ", 7 );
	Check( ReadFile( "parms.txt", data, COMMAND_SIZE - 1 ) == 1, "a 1023-byte file fits" );
	CheckCommandLine( data, COMMAND_SIZE - 1 );

	Reset( "one byte over" );
	memset( data, 'a', COMMAND_SIZE );
	Check( ReadFile( "parms.txt", data, COMMAND_SIZE ) == -1, "a 1024-byte file is refused" );
	CheckCommandLine( "", 0 );

	Reset( "a trailing line break over" );
	memset( data, 'a', COMMAND_SIZE - 1 );
	data[COMMAND_SIZE - 1] = '\n';
	Check( ReadFile( "parms.txt", data, COMMAND_SIZE ) == -1,
		"1023 bytes and a line break are refused" );
	CheckCommandLine( "", 0 );

	Reset( "CRLF file that fits once folded" );
	for ( i = 0 ; i < COMMAND_SIZE - 1 ; i++ ) {
		data[i * 2] = '\r';
		data[i * 2 + 1] = '\n';
	}
	Check( ReadFile( "parms.txt", data, ( COMMAND_SIZE - 1 ) * 2 ) == 1,
		"1023 CRLF line breaks fold into 1023 bytes" );
	memset( expected, '\n', COMMAND_SIZE - 1 );
	CheckCommandLine( expected, COMMAND_SIZE - 1 );

	Reset( "CRLF file one byte over once folded" );
	data[( COMMAND_SIZE - 1 ) * 2] = 'a';
	Check( ReadFile( "parms.txt", data, ( COMMAND_SIZE - 1 ) * 2 + 1 ) == -1,
		"a 2047-byte file that folds to 1024 bytes is refused" );
	CheckCommandLine( "", 0 );

	Reset( "file as large as the read buffer" );
	memset( data, '\r', TEXT_SIZE );
	Check( ReadFile( "parms.txt", data, TEXT_SIZE ) == -1, "a 2048-byte file is refused" );
	CheckCommandLine( "", 0 );

	Reset( "huge file" );
	memset( data, 'a', sizeof( data ) );
	Check( ReadFile( "parms.txt", data, sizeof( data ) ) == -1, "a 100000-byte file is refused" );
	CheckCommandLine( "", 0 );

	Reset( "file after arguments" );
	Check( Append( "safe", 4, ' ' ), "an argument fits" );
	memset( data, 'a', COMMAND_SIZE - 1 - 5 );
	data[0] = '+';
	Check( ReadFile( "parms.txt", data, COMMAND_SIZE - 1 - 5 ) == 1,
		"a file filling the rest after a '\\n' separator fits" );
	Check( commandLength == COMMAND_SIZE - 1 && commandLine[4] == '\n'
		&& commandLine[COMMAND_SIZE - 1] == '\0', "the file follows a line break" );

	Reset( "file one byte over after arguments" );
	Check( Append( "safe", 4, ' ' ), "an argument fits" );
	Check( ReadFile( "parms.txt", data, COMMAND_SIZE - 1 - 4 ) == -1,
		"a file one byte too long after the arguments is refused" );
	CheckCommandLine( "safe", 4 );
}

/* The command line goes to etboot.log and the crash report, which people
 * attach to bug reports, so password cvar values are masked there. */
static void TestMaskSecrets( void ) {
	static const char *cases[][2] = {
		{ "+set rconpassword hunter2 +map q3dm1", "+set rconpassword ******* +map q3dm1" },
		{ "+set g_password \"two words\"\n+set sv_hostname Mine",
		  "+set g_password \"*** *****\"\n+set sv_hostname Mine" },
		/* "set" takes every remaining word as the value */
		{ "+set g_password my secret phrase +map q3dm1", "+set g_password ** ****** ****** +map q3dm1" },
		{ "set rconpassword a b\r\nmap q3dm1", "set rconpassword * *\r\nmap q3dm1" },
		/* a line break ends the console line even inside an open quote, as in Com_ParseCommandLine */
		{ "+set rconpassword \"abc\n+map q3dm1\n+set sv_hostname Foo",
		  "+set rconpassword \"***\n+map q3dm1\n+set sv_hostname Foo" },
		/* a quoted '+' does not end the value */
		{ "+set g_password \"a+b\" +map q3dm1", "+set g_password \"***\" +map q3dm1" },
		{ "+sv_privatePassword x +RCONPASSWORD s3cret", "+sv_privatePassword * +RCONPASSWORD ******" },
		{ "safe\n+seta cl_password abc", "safe\n+seta cl_password ***" },
		{ "+set rconpassword", "+set rconpassword" },
		{ "+set rconpassword \"unterminated", "+set rconpassword \"************" },
		{ "+set s_initsound 1 +set com_hunkMegs 64", "+set s_initsound 1 +set com_hunkMegs 64" },
		{ "", "" },
	};
	char	line[COMMAND_SIZE];
	int		i;

	for ( i = 0 ; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ) ; i++ ) {
		Q_strncpyz( line, cases[i][0], sizeof( line ) );
		Sys_MaskStartupSecrets( line );
		if ( strcmp( line, cases[i][1] ) ) {
			fprintf( stderr, "masked \"%s\" as \"%s\"\n", cases[i][0], line );
		}
		Check( !strcmp( line, cases[i][1] ), "password cvar values are masked in the logged command line" );
	}
}

int main( int argc, char **argv ) {
	if ( argc != 2 ) {
		fprintf( stderr, "usage: %s scratch-directory\n", argv[0] );
		return 2;
	}
	testDir = argv[1];
	commandLine = malloc( COMMAND_SIZE );
	text = malloc( TEXT_SIZE );
	Check( commandLine && text, "allocates the buffers" );

	TestMissingAndEmpty();
	TestLineEndings();
	TestQuotingAndBytes();
	TestArgumentBounds();
	TestFileBounds();
	TestMaskSecrets();

	free( commandLine );
	free( text );
	printf( "mac startup parms regression passed\n" );
	return 0;
}
