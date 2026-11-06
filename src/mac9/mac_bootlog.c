// mac_bootlog.c -- a log that survives a freeze or a hard reset
//
// The crash rings in mac_main.c and common.c reach the disk only at quit, at a
// fatal error or on Cmd-D, so a freeze leaves nothing behind. Writing a normal
// log file does not help: HFS records a file's length in the catalog only
// when the file is closed or its volume flushed, so a file that grew since
// then comes back from a hard reset without the new text.
//
// So this file is given its full length once, at startup, and the volume is
// flushed then. Later writes only replace bytes inside that length, and
// PBFlushFileSync sends each one to the disk, which leaves no catalog change
// to lose. Text is held until a whole line is ready. Each line starts with
// the time in seconds, and "<<< end of log >>>" follows the newest text, so
// the end can be found after a reset and after the log wraps. When full, it
// wraps to BOOTLOG_KEEP, keeping the startup at the top of the file.
//
// The log of the previous launch is kept as q3boot.prev.log, because the first
// thing anyone does after a freeze is restart the game.
//
// Each line costs a disk write, and each goes out as it is finished: the last
// line before a freeze is the one that matters, so none is held back. Startup
// and map loads print a few hundred lines, and a frame normally prints none. A
// warning printed every frame will slow the game, and "+set sys_bootlog 0" is
// the way out of that.
//
// Sys_BootLogWrite is for the main task only: the File Manager calls here are
// synchronous, so they must not run at interrupt time.

#include <stdio.h>
#include <string.h>
#include <Files.h>
#include <Devices.h>
#include <Errors.h>
#include "mac_bootlog.h"

int Sys_Milliseconds( void );

#define BOOTLOG_NAME					"etboot.log"
#define BOOTLOG_PREVIOUS_NAME			"etboot.prev.log"
#define BOOTLOG_SIZE					( 2048L * 1024 )
#define BOOTLOG_KEEP					( 256L * 1024 )
#define BOOTLOG_PENDING					8192
#define BOOTLOG_END						"\n<<< end of log >>>\n"
#define BOOTLOG_END_LENGTH				( (int)sizeof( BOOTLOG_END ) - 1 )
#define BOOTLOG_STAMP_LENGTH			16

// ioPosMode bit 5: don't cache this request (Inside Macintosh: Files)
#define BOOTLOG_NO_CACHE				0x0020

static short	logRefNum;			// 0: no log
static short	logVolume;
static long		logPosition;		// where the next text goes
static int		logWrapped;
static char		pending[BOOTLOG_PENDING + BOOTLOG_END_LENGTH];
static int		pendingLength;
static int		atLineStart;
static int		writing;			// no reentry from what a write prints

/*
=================
Sys_BootLogPath

The C path directory:leaf as a Str255. Returns 0 if it is longer than that.
A volume root from Sys_GetCwd ("Vol:") already ends in the separator.
=================
*/
static int Sys_BootLogPath( Str255 path, const char *directory, const char *leaf ) {
	int		length, leafLength;

	length = strlen( directory );
	leafLength = strlen( leaf );
	if ( length > 0 && directory[length - 1] == ':' ) {
		length--;
	}
	if ( length + 1 + leafLength > 255 ) {
		return 0;
	}
	path[0] = length + 1 + leafLength;
	memcpy( path + 1, directory, length );
	path[length + 1] = ':';
	memcpy( path + length + 2, leaf, leafLength );
	return 1;
}

/*
=================
Sys_BootLogStop

A write failed: give up on the log rather than fail on every line.
=================
*/
static void Sys_BootLogStop( void ) {
	FSClose( logRefNum );
	logRefNum = 0;
	pendingLength = 0;
}

/*
=================
Sys_BootLogPut
=================
*/
static OSErr Sys_BootLogPut( long position, char *data, long length ) {
	ParamBlockRec	pb;

	memset( &pb, 0, sizeof( pb ) );
	pb.ioParam.ioRefNum = logRefNum;
	pb.ioParam.ioBuffer = data;
	pb.ioParam.ioReqCount = length;
	pb.ioParam.ioPosMode = fsFromStart | BOOTLOG_NO_CACHE;
	pb.ioParam.ioPosOffset = position;
	return PBWriteSync( &pb );
}

/*
=================
Sys_BootLogSync

Writes the pending text and sends it to the disk.
=================
*/
static void Sys_BootLogSync( void ) {
	static char		noMarker[BOOTLOG_END_LENGTH];
	ParamBlockRec	pb;
	OSErr			err;

	if ( !logRefNum || !pendingLength ) {
		return;
	}
	err = noErr;
	if ( logPosition + pendingLength + BOOTLOG_END_LENGTH > BOOTLOG_SIZE ) {
		// the end marker moves back up: there must be only one to find
		memset( noMarker, ' ', sizeof( noMarker ) );
		err = Sys_BootLogPut( logPosition, noMarker, sizeof( noMarker ) );
		logPosition = BOOTLOG_KEEP;
		logWrapped = 1;
	}
	memcpy( pending + pendingLength, BOOTLOG_END, BOOTLOG_END_LENGTH );
	if ( err == noErr ) {
		err = Sys_BootLogPut( logPosition, pending, pendingLength + BOOTLOG_END_LENGTH );
	}
	if ( err == noErr ) {
		memset( &pb, 0, sizeof( pb ) );
		pb.ioParam.ioRefNum = logRefNum;
		err = PBFlushFileSync( &pb );
	}
	if ( err != noErr ) {
		Sys_BootLogStop();
		return;
	}
	logPosition += pendingLength;
	pendingLength = 0;
}

/*
=================
Sys_BootLogOpen

Starts a log next to the application, keeping the last one, and returns 1. On
a locked or full volume it returns 0, and the game runs without one.
=================
*/
int Sys_BootLogOpen( const char *directory ) {
	static char	blank[32768];
	Str255	path, previousPath;
	Str255	previousLeaf;
	long	remaining, count;
	OSErr	err;

	if ( logRefNum ) {
		return 1;
	}
	if ( !Sys_BootLogPath( path, directory, BOOTLOG_NAME ) ||
		!Sys_BootLogPath( previousPath, directory, BOOTLOG_PREVIOUS_NAME ) ) {
		return 0;
	}
	previousLeaf[0] = strlen( BOOTLOG_PREVIOUS_NAME );
	memcpy( previousLeaf + 1, BOOTLOG_PREVIOUS_NAME, previousLeaf[0] );

	// If the last log cannot move out of the way, because q3boot.prev.log is
	// open in an editor, say, leave it alone: it may be the log of a freeze.
	HDelete( 0, 0, previousPath );
	err = HRename( 0, 0, path, previousLeaf );
	if ( err != noErr && err != fnfErr ) {
		return 0;
	}

	err = HCreate( 0, 0, path, 'ttxt', 'TEXT' );
	if ( err != noErr && err != dupFNErr ) {
		return 0;
	}
	err = HOpenDF( 0, 0, path, fsRdWrPerm, &logRefNum );
	if ( err == paramErr ) {
		err = HOpen( 0, 0, path, fsRdWrPerm, &logRefNum );
	}
	if ( err != noErr ) {
		logRefNum = 0;
		return 0;
	}

	// blanks, so the unused part reads as nothing in any text editor
	memset( blank, ' ', sizeof( blank ) );
	err = SetEOF( logRefNum, 0 );
	for ( remaining = BOOTLOG_SIZE ; err == noErr && remaining > 0 ; remaining -= count ) {
		count = remaining < (long)sizeof( blank ) ? remaining : (long)sizeof( blank );
		err = FSWrite( logRefNum, &count, blank );
	}
	if ( err == noErr ) {
		err = GetVRefNum( logRefNum, &logVolume );
	}
	if ( err == noErr ) {
		err = FlushVol( NULL, logVolume );
	}
	if ( err != noErr ) {
		FSClose( logRefNum );
		logRefNum = 0;
		HDelete( 0, 0, path );
		return 0;
	}

	logPosition = 0;
	logWrapped = 0;
	pendingLength = 0;
	atLineStart = 1;
	return 1;
}

/*
=================
Sys_BootLogWrite
=================
*/
void Sys_BootLogWrite( const char *text ) {
	int		now, lineEnded;

	if ( !logRefNum || writing ) {
		return;
	}
	writing = 1;

	now = Sys_Milliseconds();
	lineEnded = 0;
	for ( ; *text && logRefNum ; text++ ) {
		if ( atLineStart ) {
			if ( pendingLength + BOOTLOG_STAMP_LENGTH >= BOOTLOG_PENDING ) {
				Sys_BootLogSync();
			}
			pendingLength += sprintf( pending + pendingLength, "[%6d.%03d] ", now / 1000, now % 1000 );
			atLineStart = 0;
		}
		pending[pendingLength++] = *text;
		if ( *text == '\n' ) {
			atLineStart = 1;
			lineEnded = 1;
		}
		if ( pendingLength >= BOOTLOG_PENDING ) {
			Sys_BootLogSync();
		}
	}

	if ( lineEnded ) {
		Sys_BootLogSync();
	}
	writing = 0;
}

/*
=================
Sys_BootLogFlush

Sends an unfinished line to the disk.
=================
*/
void Sys_BootLogFlush( void ) {
	if ( !writing ) {
		Sys_BootLogSync();
	}
}

/*
=================
Sys_BootLogUnlock

The crash handler's: a fault inside Sys_BootLogWrite must not keep the crash
report out of the log.
=================
*/
void Sys_BootLogUnlock( void ) {
	writing = 0;
}

/*
=================
Sys_BootLogClose

An orderly quit: the file shrinks to the text, if it never wrapped.
=================
*/
void Sys_BootLogClose( void ) {
	if ( !logRefNum ) {
		return;
	}
	Sys_BootLogSync();
	if ( !logRefNum ) {
		return;
	}
	if ( !logWrapped ) {
		SetEOF( logRefNum, logPosition );
	}
	FSClose( logRefNum );
	logRefNum = 0;
	FlushVol( NULL, logVolume );
}
