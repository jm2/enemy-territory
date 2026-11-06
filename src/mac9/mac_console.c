#include <stdio.h>
#include "../client/client.h"
#include "mac_local.h"
#include <DriverServices.h>

#define	CONSOLE_MASK	1023
static char	consoleChars[CONSOLE_MASK+1];
static int consoleHead, consoleTail;
static qboolean consoleDisplayed;

/*
==================
Sys_InitConsole
==================
*/
void	Sys_InitConsole( void ) {
    // StdCLib should handle console initialization if linked.
}

/*
==================
Sys_ShowConsole
==================
*/
void	Sys_ShowConsole( int level, qboolean quitOnClose ) {
	
	if ( level ) {
		consoleDisplayed = qtrue;
		printf( "\n" );
	} else {
		consoleDisplayed = qfalse;
	}	
}


/*
==================
Sys_ConsoleWanted

Asked by _consolewrite (mac_consolehooks.cc) for each write to fd. Errors
on stderr always open and reach the console window. Other output does only
while the console is displayed (viewlog, or the Shift prompt at launch).
Hidden output still reaches the crash ring.
==================
*/
int Sys_ConsoleWanted( int fd ) {
	return fd == 2 || consoleDisplayed;
}


/*
================
Sys_Print

This is called for all console output, even if the game is running
full screen and the dedicated console window is hidden.
================
*/
void	Sys_Print( const char *text ) {
	// Always record into the crash-dump ring buffer, even with the
	// on-screen console hidden (viewlog 0) — otherwise the post-mortem
	// dumps contain none of the engine's console output.
	Sys_LogRecord( text );

	if ( !consoleDisplayed ) {
		return;
	}
	printf( "%s", text );
}


/*
==================
Sys_ConsoleEvent

A dedicated server has no game window, so, as in id's SIOUX version, what is
typed goes to the console: echoed in the console window and kept until
Sys_ConsoleInput hands back the line.  Delete (or left arrow) takes back a
character.  Command keys still reach the menus.  The game takes the keys
when it is not dedicated.
==================
*/
qboolean Sys_ConsoleEvent( EventRecord *event ) {
	int		c;

	if ( !com_dedicated || !com_dedicated->integer ) {
		return qfalse;
	}
	if ( ( event->what != keyDown && event->what != autoKey ) || ( event->modifiers & cmdKey ) ) {
		return qfalse;
	}

	c = event->message & charCodeMask;
	if ( c == 8 || c == 28 ) {
		// never into a line already entered
		if ( consoleHead > consoleTail && consoleChars[ ( consoleHead - 1 ) & CONSOLE_MASK ] != 13 ) {
			consoleHead--;
			printf( "\033[D \033[D" );	// Retro68's console has no backspace
		}
	} else if ( ( c >= 32 && c != 127 ) || c == 13 ) {
		// a full ring takes no more characters, but always a return
		if ( consoleHead - consoleTail < CONSOLE_MASK
			|| ( c == 13 && consoleHead - consoleTail <= CONSOLE_MASK ) ) {
			consoleChars[ consoleHead & CONSOLE_MASK ] = c;
			consoleHead++;
			printf( "%c", c == 13 ? '\n' : c );
		}
	}
	fflush( stdout );

	return qtrue;
}


/*
================
Sys_ConsoleInput

Checks for a complete line of text typed in at the console.
Return NULL if a complete line is not ready.
================
*/
char *Sys_ConsoleInput( void ) {
	static char	string[CONSOLE_MASK+1];
	int		i;

	for ( i = 0 ; consoleTail + i < consoleHead ; i++ ) {
		string[i] = consoleChars[ ( consoleTail + i ) & CONSOLE_MASK ];
		if ( string[i] == 13 ) {
			consoleTail += i + 1;
			string[i] = 0;
			return string;
		}
	}

	return NULL;
}
