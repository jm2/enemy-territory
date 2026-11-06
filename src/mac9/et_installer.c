// One-file installer: its data fork holds the PEF and then the game (et_installer_format.h), so a
// full install is a single MacBinary that needs no disk image software. It unpacks into a working
// folder beside itself and renames that to its final name only when every checksum has passed, so
// a failed run leaves no half-installed game, and an existing install is never touched.
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Fonts.h>
#include <MacMemory.h>
#include <MacWindows.h>
#include <Menus.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <stdio.h>
#include <string.h>
#include "et_installer_format.h"

#define COPY_BYTES ( 1024L * 1024L )
#define BAD_CRC ( -1 )

static const unsigned char FINAL_NAME[] = "\017Enemy Territory";
static const unsigned char WORK_NAME[] = "\021ET being unpacked";

static WindowPtr window;
static Str255 statusText;
static Ptr copyBuffer;
static short sourceRef;
static short volume;
static long parentDir;      // the folder holding this application
static long workDir;        // 0 until the working folder exists
static long dirIds[ET_MAX_ENTRIES];
static etEntry_t entries[ET_MAX_ENTRIES];
static int created;         // entries made so far, for the clean up
static long long doneBytes, totalBytes;

static void Pascal( Str255 out, const char *in ) {
	size_t n = strlen( in );

	if ( n > 255 ) {
		n = 255;
	}
	out[0] = (unsigned char)n;
	memcpy( out + 1, in, n );
}

static void DrawStatus( void ) {
	GrafPtr port = (GrafPtr)window;

	SetPort( port );
	EraseRect( &port->portRect );
	MoveTo( 16, 28 );
	DrawString( statusText );
}

static void Pump( void ) {
	EventRecord event;

	while ( WaitNextEvent( everyEvent, &event, 0, NULL ) ) {
		if ( event.what == updateEvt && (WindowPtr)event.message == window ) {
			BeginUpdate( window );
			DrawStatus();
			EndUpdate( window );
		}
	}
}

static void Status( const char *text ) {
	Pascal( statusText, text );
	DrawStatus();
	Pump();
}

static void Finish( const char *title, const char *message ) {
	Str255 t, m;

	Pascal( t, title );
	Pascal( m, message );
	ParamText( t, m, m, m );
	if ( sourceRef ) {
		FSClose( sourceRef );   // the message may say to delete this file
		sourceRef = 0;
	}
	Alert( 128, NULL );
	ExitToShell();
}

static long ParentOf( int i ) {
	return entries[i].parent < 0 ? workDir : dirIds[entries[i].parent];
}

// true when nothing of this run is left on the disk
static Boolean CleanUp( void ) {
	Boolean clean = true;
	int i;

	if ( !workDir ) {
		return true;
	}
	for ( i = created - 1; i >= 0; i-- ) {
		if ( HDelete( volume, ParentOf( i ), entries[i].name ) != noErr ) {
			clean = false;
		}
	}
	if ( HDelete( volume, parentDir, WORK_NAME ) != noErr ) {
		clean = false;
	}
	return clean;
}

static void Fail( const char *what, OSErr err ) {
	char text[240];
	const char *left = CleanUp() ? "Nothing was installed."
					   : "The folder ET being unpacked could not be removed. Delete it by hand.";

	if ( err == BAD_CRC ) {
		snprintf( text, sizeof( text ), "%s failed its checksum, so this copy of the installer is damaged. "
				  "Transfer ET_Installer.bin again in binary mode. %s", what, left );
	} else if ( err == dskFulErr ) {
		snprintf( text, sizeof( text ), "The disk is full. %s", left );
	} else {
		snprintf( text, sizeof( text ), "%s failed with Mac OS error %d. %s", what, (int)err, left );
	}
	Finish( "Enemy Territory was not installed.", text );
}

// the source is already positioned at this fork
static OSErr CopyFork( short dest, unsigned long length, unsigned long expected, const char *name ) {
	unsigned long crc = 0;
	char text[80];
	int shown = -1;
	OSErr err;

	if ( ( err = SetEOF( dest, (long)length ) ) != noErr ) {
		return err;
	}
	while ( length ) {
		long count = length > (unsigned long)COPY_BYTES ? COPY_BYTES : (long)length;
		long wanted = count;
		int percent;

		if ( ( err = FSRead( sourceRef, &count, copyBuffer ) ) != noErr ) {
			return err;
		}
		if ( count != wanted ) {
			return eofErr;
		}
		crc = ET_Crc( crc, (const unsigned char *)copyBuffer, count );
		if ( ( err = FSWrite( dest, &count, copyBuffer ) ) != noErr ) {
			return err;
		}
		length -= count;
		doneBytes += count;
		percent = (int)( doneBytes * 100 / totalBytes );
		if ( percent != shown ) {
			snprintf( text, sizeof( text ), "Unpacking %s: %d%% of the install", name, percent );
			Status( text );
			shown = percent;
		} else {
			Pump();
		}
	}
	return crc == expected ? noErr : BAD_CRC;
}

static void InstallFile( int i ) {
	const etEntry_t *e = &entries[i];
	long dir = ParentOf( i );
	char name[32];
	short ref;
	FInfo info;
	OSErr err, closing;

	memcpy( name, e->name + 1, e->name[0] );
	name[e->name[0]] = 0;
	if ( ( err = HCreate( volume, dir, e->name, e->creator, e->type ) ) != noErr ) {
		Fail( name, err );
	}
	created = i + 1;
	if ( ( err = HOpenDF( volume, dir, e->name, fsRdWrPerm, &ref ) ) != noErr ) {
		Fail( name, err );
	}
	err = CopyFork( ref, e->dataLength, e->dataCrc, name );
	closing = FSClose( ref );
	if ( err == noErr ) {
		err = closing;
	}
	if ( err != noErr ) {
		Fail( name, err );
	}
	if ( e->rsrcLength ) {
		if ( ( err = HOpenRF( volume, dir, e->name, fsRdWrPerm, &ref ) ) != noErr ) {
			Fail( name, err );
		}
		err = CopyFork( ref, e->rsrcLength, e->rsrcCrc, name );
		closing = FSClose( ref );
		if ( err == noErr ) {
			err = closing;
		}
		if ( err != noErr ) {
			Fail( name, err );
		}
	}
	if ( ( err = HGetFInfo( volume, dir, e->name, &info ) ) != noErr ) {
		Fail( name, err );
	}
	info.fdFlags = (unsigned short)e->flags;
	if ( ( err = HSetFInfo( volume, dir, e->name, &info ) ) != noErr ) {
		Fail( name, err );
	}
}

// any name already taken stops the install before anything is written
static void RefuseIfPresent( const unsigned char *name, const char *text ) {
	FSSpec spec;
	OSErr err = FSMakeFSSpec( volume, parentDir, name, &spec );

	if ( err == noErr ) {
		Finish( "Enemy Territory was not installed.", text );
	}
	if ( err != fnfErr ) {
		Fail( "Looking at the disk", err );
	}
}

int main( void ) {
	static unsigned char trailerBytes[ET_TRAILER_BYTES];
	ProcessSerialNumber psn;
	ProcessInfoRec info;
	FSSpec self;
	etTrailer_t trailer;
	Rect bounds;
	Ptr header;
	long length, count, id;
	int i, entryCount;
	OSErr err;

	InitGraf( &qd.thePort );
	InitFonts();
	FlushEvents( everyEvent, 0 );
	InitWindows();
	InitMenus();
	TEInit();
	InitDialogs( NULL );
	InitCursor();

	SetRect( &bounds, 0, 0, 460, 48 );
	OffsetRect( &bounds, ( qd.screenBits.bounds.right - 460 ) / 2, 120 );
	window = NewWindow( NULL, &bounds, "\031Enemy Territory Installer", true, noGrowDocProc, (WindowPtr)-1L, false, 0 );
	Status( "Checking the installer" );

	memset( &info, 0, sizeof( info ) );
	info.processInfoLength = sizeof( info );
	info.processAppSpec = &self;
	if ( ( err = GetCurrentProcess( &psn ) ) != noErr || ( err = GetProcessInformation( &psn, &info ) ) != noErr
		 || ( err = FSpOpenDF( &self, fsRdPerm, &sourceRef ) ) != noErr ) {
		Fail( "Opening the installer", err );
	}
	volume = self.vRefNum;
	parentDir = self.parID;
	RefuseIfPresent( FINAL_NAME, "Something called Enemy Territory is already beside this installer. "
					 "Move it somewhere else, or move the installer, and run it again." );
	RefuseIfPresent( WORK_NAME, "Something called ET being unpacked is already beside this installer, "
					 "left by a run that was stopped. Delete it and run the installer again." );

	if ( ( err = GetEOF( sourceRef, &length ) ) != noErr ) {
		Fail( "Reading the installer", err );
	}
	count = ET_TRAILER_BYTES;
	if ( length < ET_TRAILER_BYTES || SetFPos( sourceRef, fsFromStart, length - ET_TRAILER_BYTES ) != noErr
		 || FSRead( sourceRef, &count, trailerBytes ) != noErr
		 || ET_ParseTrailer( trailerBytes, length, &trailer ) != 0 ) {
		Fail( "The end of the installer", BAD_CRC );
	}
	header = NewPtr( trailer.headerLength );
	copyBuffer = NewPtr( COPY_BYTES );
	if ( !header || !copyBuffer ) {
		Fail( "Allocating memory", memFullErr );
	}
	count = trailer.headerLength;
	if ( SetFPos( sourceRef, fsFromStart, trailer.headerOffset ) != noErr || FSRead( sourceRef, &count, header ) != noErr ) {
		Fail( "The installer's file list", BAD_CRC );
	}
	entryCount = ET_ParseHeader( (const unsigned char *)header, &trailer, length, entries );
	if ( entryCount < 0 ) {
		Fail( "The installer's file list", BAD_CRC );
	}
	for ( i = 0; i < entryCount; i++ ) {
		totalBytes += (long long)entries[i].dataLength + entries[i].rsrcLength;
	}

	// not into workDir itself: it says a folder exists to clean up, and a call that fails may write here
	if ( ( err = DirCreate( volume, parentDir, WORK_NAME, &id ) ) != noErr ) {
		Fail( "Making the folder", err );
	}
	workDir = id;
	for ( i = 0; i < entryCount; i++ ) {
		if ( entries[i].type == ET_FOLDER_TYPE ) {
			if ( ( err = DirCreate( volume, ParentOf( i ), entries[i].name, &dirIds[i] ) ) != noErr ) {
				Fail( "Making a folder", err );
			}
			created = i + 1;
		} else {
			InstallFile( i );
		}
	}
	// on disk before the name is, because HFS has no journal to put a crash right
	if ( ( err = FlushVol( NULL, volume ) ) != noErr ) {
		Fail( "Saving to the disk", err );
	}
	if ( ( err = HRename( volume, parentDir, WORK_NAME, FINAL_NAME ) ) != noErr ) {
		Fail( "Renaming the folder", err );
	}
	// Fail would delete the finished install now, so this only says so
	if ( FlushVol( NULL, volume ) != noErr ) {
		Finish( "Enemy Territory is installed, but the disk reported an error.",
				"All files passed their checksums, but the last save to the disk failed. "
				"Check the disk, and run the installer again if the game does not start." );
	}
	Finish( "Enemy Territory is installed.", "All files passed their checksums. "
			"The Enemy Territory folder is beside this installer, which you can now delete." );
	return 0;
}
