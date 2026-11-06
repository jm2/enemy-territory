// mac_consolehooks.cc -- the Retro68 console window, opened on demand
//
// newlib sends stdin, stdout and stderr to _consoleread and _consolewrite.
// Defining them here keeps libRetroConsole's InitConsole.cc out of the link.
// That InitConsole ran InitGraf, InitFonts, InitWindows and InitMenus on the
// first byte of output, so any printf opened the console window (issue #10)
// and also initialized the Toolbox (issue #263). main now initializes the
// Toolbox itself (Sys_InitToolbox). Opening the window later, after the game
// window exists, must not run InitWindows and InitMenus again: that would
// reset the window and menu lists. InitCursor would also show the cursor that
// Sys_InitInput hid.
//
// The window is the same retro::ConsoleWindow, at the same position. Output
// opens and reaches it only while Sys_ConsoleWanted (mac_console.c) accepts
// it. Reading stdin always opens it.

#include <sys/types.h>
#include <string.h>
#include <string>

#include "retro/ConsoleWindow.h"

extern "C" int Sys_ConsoleWanted( int fd );

using namespace retro;

static GrafPtr	consoleWindow;

static void Sys_OpenConsoleWindow( void ) {
	Rect	r;
	GrafPtr	save;

	if ( Console::currentInstance ) {
		return;
	}
	Console::currentInstance = (Console *)-1;	// as InitConsole: no recursion

	r = qd.screenBits.bounds;
	r.top += 40;
	InsetRect( &r, 5, 5 );
	// The ConsoleWindow constructor leaves the current port on the console.
	// Opened mid-game, that would move port-relative code such as the drag
	// handler's LocalToGlobal (mac_event.c) off the game window.
	GetPort( &save );
	Console::currentInstance = new ConsoleWindow( r, "\pRetro68 Console" );
	GetPort( &consoleWindow );
	SetPort( save );
}

// Redraws the console window for DoUpdate (mac_event.c), between its
// BeginUpdate and EndUpdate. The game loop takes the console window's update
// events, so without this a part uncovered by another window stayed blank.
extern "C" void Sys_ConsoleDraw( GrafPtr window ) {
	if ( !consoleWindow || window != consoleWindow
		|| !Console::currentInstance || Console::currentInstance == (Console *)-1 ) {
		return;
	}
	Console::currentInstance->Draw();
}

// Each write is gated, not just the first: once viewlog 0 hides the console,
// the window (which cannot be closed, like retail's SIOUX) stops receiving
// stdout, as retail's Sys_Print stopped printing.
extern "C" ssize_t _consolewrite( int fd, const void *buf, size_t count ) {
	if ( !Sys_ConsoleWanted( fd ) ) {
		return count;	// hidden: the crash ring has Sys_LogPrintf's copy
	}
	if ( !Console::currentInstance ) {
		Sys_OpenConsoleWindow();
	}
	if ( Console::currentInstance == (Console *)-1 ) {
		return 0;
	}
	Console::currentInstance->write( (const char *)buf, count );
	return count;
}

extern "C" ssize_t _consoleread( int fd, void *buf, size_t count ) {
	static std::string	consoleBuf;

	if ( !Console::currentInstance ) {
		Sys_OpenConsoleWindow();
	}
	if ( Console::currentInstance == (Console *)-1 ) {
		return 0;
	}
	if ( consoleBuf.size() == 0 ) {
		consoleBuf = Console::currentInstance->ReadLine();
	}
	if ( count > consoleBuf.size() ) {
		count = consoleBuf.size();
	}
	memcpy( buf, consoleBuf.data(), count );
	consoleBuf = consoleBuf.substr( count );
	return count;
}
