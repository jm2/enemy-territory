/* Issues #263 and #10: just enough of the Toolbox and of Retro68's
 * retro::ConsoleWindow for tests/mac_toolbox_init_regression.c to compile the
 * real code/mac/mac_consolehooks.cc on the host.  run_mac_toolbox_init_tests.sh
 * installs this file as retro/ConsoleWindow.h.  The window itself is the
 * test's: FakeConsoleWindowOpen, FakeConsoleWindowWrite,
 * FakeConsoleWindowReadLine and FakeConsoleWindowDraw. */
#ifndef MAC_CONSOLE_WINDOW_FAKE_H
#define MAC_CONSOLE_WINDOW_FAKE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { short top, left, bottom, right; } Rect;
typedef struct FakeGrafPort	*GrafPtr;
typedef unsigned char		Str255[256];
typedef const unsigned char	*ConstStr255Param;
typedef struct {
	void	*thePort;
	struct { Rect bounds; } screenBits;
} FakeQDGlobals;

extern FakeQDGlobals	qd;
void GetPort( GrafPtr *port );
void SetPort( GrafPtr port );
void InsetRect( Rect *r, short dh, short dv );

GrafPtr FakeConsoleWindowOpen( const Rect *r );
void FakeConsoleWindowWrite( const char *s, int n );
const char *FakeConsoleWindowReadLine( void );
void FakeConsoleWindowDraw( void );
void FakeConsoleWindowDelete( void );

#ifdef __cplusplus
}

#include <string>

namespace retro {
	class Console {
	public:
		static Console	*currentInstance;

		void write( const char *s, int n ) { FakeConsoleWindowWrite( s, n ); }
		std::string ReadLine() { return FakeConsoleWindowReadLine(); }
		void Draw() { FakeConsoleWindowDraw(); }
	};

	/* Only mac_consolehooks.cc includes this header. */
	Console	*Console::currentInstance;

	class ConsoleWindow : public Console {
	public:
		/* The title is "\p..." (unsigned with -fpascal-strings). */
		template <typename T> ConsoleWindow( Rect r, const T *title ) {
			/* As Retro68's constructor: the port is left on the console. */
			SetPort( FakeConsoleWindowOpen( &r ) );
		}
	};
}

extern "C" void FakeConsoleWindowDelete( void ) {
	if ( retro::Console::currentInstance &&
		 retro::Console::currentInstance != (retro::Console *)-1 ) {
		delete static_cast<retro::ConsoleWindow *>( retro::Console::currentInstance );
	}
	retro::Console::currentInstance = NULL;
}
#endif

#endif
