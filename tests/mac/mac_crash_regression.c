/* code/mac/mac_crash.c: an exception becomes a logged exit.  The handler runs
 * for real on fake exception records; Sys_CrashReport then runs as the resumed
 * code would, reading a fake stack, which fails the test if read off its end. */
#include <setjmp.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_START		0x10000000u
#define CODE_END		0x10400000u
#define CRASH_CODE_START	CODE_START
#define CRASH_CODE_END		CODE_END

#define STACK_LOW		0x00100000u
#define STACK_HIGH		0x00140000u
#define STACK_GOOD		( STACK_LOW + 0x30000u )	/* more room than CRASH_STACK_NEEDED */

static uint32_t		fakeStack[( STACK_HIGH - STACK_LOW ) / 4];
static int			failures, badReads;
static uint32_t ReadFake( uint32_t address ) {
	if ( address < STACK_LOW || address >= STACK_HIGH || ( address & 3 ) ) {
		badReads++;
		return 0;
	}
	return fakeStack[( address - STACK_LOW ) / 4];
}
#define CRASH_READ( address )		ReadFake( address )
static const uint32_t	reportVector[2] = { 0x10123450, 0x40031000 };
#define CRASH_TVECTOR( function )	reportVector
static int			snifferOffs;
#define CRASH_SNIFFER_OFF()		( snifferOffs++ )

#include "../../src/mac9/mac_crash.c"

static char			logText[65536];
static int			flushes, flushesBeforeError;
static char			errorText[256];
static jmp_buf		errorDone;
static ExceptionHandler	installed;

void Sys_LogRecord( const char *text ) { strncat( logText, text, sizeof( logText ) - strlen( logText ) - 1 ); }
void Sys_BootLogFlush( void ) { flushes++; }
static int			unlocks;
void Sys_BootLogUnlock( void ) { unlocks++; }
void Sys_Error( const char *error, ... ) {
	va_list	args;

	va_start( args, error );
	vsnprintf( errorText, sizeof( errorText ), error, args );
	va_end( args );
	flushesBeforeError = flushes;
	longjmp( errorDone, 1 );
}
ExceptionHandler InstallExceptionHandler( ExceptionHandler handler ) { installed = handler; return NULL; }
Ptr GetApplLimit( void ) { return (Ptr)(uintptr_t)STACK_LOW; }

static void Check( int condition, const char *what ) {
	if ( !condition ) {
		printf( "FAILED: %s\n", what );
		failures++;
	}
}

static MachineInformation	machine;
static RegisterInformation	registers;
static ExceptionInformation	info;

static void Reset( ExceptionKind kind, uint32_t pc, uint32_t sp ) {
	crashing = 0;
	logText[0] = errorText[0] = 0;
	flushes = flushesBeforeError = badReads = snifferOffs = unlocks = 0;
	memset( fakeStack, 0, sizeof( fakeStack ) );
	memset( &machine, 0, sizeof( machine ) );
	memset( &registers, 0, sizeof( registers ) );
	machine.PC.lo = pc;
	machine.LR.lo = CODE_START + 0x2000;
	machine.DAR.lo = 0xdeadbeef;
	registers.R1.lo = sp;
	registers.R2.lo = 0x12345678;
	registers.R31.lo = 0xcafef00d;
	info.theKind = kind;
	info.machineState = &machine;
	info.registerImage = &registers;
	info.info.memoryInfo = NULL;
	stackLow = STACK_LOW;
	stackHigh = STACK_HIGH;
}

/* frames at sp, sp+0x100...: each links to the next, whose saved-LR word holds a return into code */
static void BuildFrames( uint32_t sp, int count ) {
	int i;

	for ( i = 0 ; i < count ; i++, sp += 0x100 ) {
		fakeStack[( sp - STACK_LOW ) / 4] = sp + 0x100;
		fakeStack[( sp + 0x100 + 8 - STACK_LOW ) / 4] = CODE_START + 0x3000 + i * 4;
	}
}

static void Report( void ) {
	if ( !setjmp( errorDone ) ) {
		Sys_CrashReport();
		Check( 0, "Sys_CrashReport does not return" );
	}
}

static void Install( void ) {
	Sys_InstallCrashHandler();
	Check( installed == Sys_CrashHandler, "the handler is installed" );
	Check( stackLow == STACK_LOW, "the stack ends at the heap limit" );
}

static void Passes( void ) {
	static const ExceptionKind	passed[] = {
		kUnknownException, kTrapException, kTraceException, kInstructionBreakpointException,
		kDataBreakpointException, kIntegerException, kFloatingPointException
	};
	int i;

	for ( i = 0 ; i < (int)( sizeof( passed ) / sizeof( passed[0] ) ) ; i++ ) {
		Reset( passed[i], CODE_START + 0x1000, STACK_LOW + 0x4000 );
		Check( Sys_CrashHandler( &info ) != noErr, "a debugger's exceptions pass on" );
		Check( machine.PC.lo == CODE_START + 0x1000 && !crashing, "and nothing changes" );
	}
	Reset( kAccessException, 0, 0 );
	info.registerImage = NULL;
	Check( Sys_CrashHandler( &info ) != noErr && !crashing, "a record without registers passes on" );
}

static void Resumes( void ) {
	uint32_t	stack;

	Reset( kAccessException, CODE_START + 0x1000, STACK_GOOD );
	Check( Sys_CrashHandler( &info ) == noErr, "a fault is handled" );
	Check( machine.PC.lo == reportVector[0], "execution resumes at the report" );
	Check( registers.R2.lo == reportVector[1], "with this fragment's TOC" );
	Check( registers.R1.lo < STACK_GOOD - CRASH_RED_ZONE + 16 && registers.R1.lo >= STACK_GOOD - CRASH_RED_ZONE - 16
		&& !( registers.R1.lo & 15 ), "below the frames it will walk, on the stack that faulted" );
	Check( !crashOnPrivateStack, "a healthy stack keeps the stack sniffer happy" );
	Check( !snifferOffs, "and leaves it running" );
	Report();
	Check( unlocks == 1, "a fault inside a log write cannot silence the report" );

	/* too little stack left, or r1 not on the stack at all */
	Reset( kAccessException, CODE_START + 0x1000, STACK_LOW + CRASH_STACK_NEEDED - 16 );
	Check( Sys_CrashHandler( &info ) == noErr && crashOnPrivateStack, "a nearly full stack is not used" );
	Reset( kAccessException, CODE_START + 0x1000, STACK_HIGH + 16 );
	Check( Sys_CrashHandler( &info ) == noErr && crashOnPrivateStack, "nor is r1 above it" );
	Reset( kAccessException, CODE_START + 0x1000, STACK_LOW + 0x4000 );
	snifferOffs = 0;
	Check( Sys_CrashHandler( &info ) == noErr, "a fault is handled" );
	stack = (uint32_t)(uintptr_t)( crashStack + CRASH_STACK_SIZE );
	Check( crashOnPrivateStack && registers.R1.lo != STACK_LOW + 0x4000 && !( registers.R1.lo & 15 ), "it gets an aligned stack of its own" );
	Check( stack - registers.R1.lo >= CRASH_STACK_MARGIN && stack - registers.R1.lo < CRASH_STACK_MARGIN + 16,
		"with room for the frame it may write above itself" );
	Check( crashRegisters.R1.lo == STACK_LOW + 0x4000 && crashMachine.PC.lo == CODE_START + 0x1000,
		"the registers at the fault are kept" );
	Check( snifferOffs == 1, "and turns off the stack sniffer, which would call that stack an overflow, before anything runs there" );
	crashing = 1;

	/* the report itself faults */
	Check( Sys_CrashHandler( &info ) != noErr, "a second fault passes on" );
	Check( crashMachine.PC.lo == CODE_START + 0x1000, "and the first fault's record stands" );
}

static void Reports( void ) {
	static MemoryExceptionInformation	memory;

	Reset( kReadOnlyMemoryException, CODE_START + 0x1000, STACK_LOW + 0x4000 );
	BuildFrames( STACK_LOW + 0x4000, 3 );
	memory.theAddress = (void *)(uintptr_t)0xdead0000;
	memory.theReference = 1;
	info.info.memoryInfo = &memory;
	Sys_CrashHandler( &info );
	Report();
	Check( strstr( logText, "*** CRASH: write to read-only memory\n" ) != NULL, "the kind is logged" );
	Check( strstr( logText, "PC 10001000 code+0x1000\n" ) != NULL, "the PC, as an offset" );
	Check( strstr( logText, "LR 10002000 code+0x2000\n" ) != NULL, "the LR, as an offset" );
	Check( strstr( logText, "*** fault address dead0000, reference 1 " ) != NULL, "the system's record of the fault" );
	Check( strstr( logText, "DAR deadbeef" ) != NULL, "and the raw processor fields" );
	Check( strstr( logText, "cafef00d" ) != NULL, "the registers" );
	Check( strstr( logText, "frame  0 10003000 code+0x3000\n" ) != NULL
		&& strstr( logText, "frame  2 10003008 code+0x3008\n" ) != NULL, "the callers, in order" );
	Check( !badReads, "the walk stays on the stack" );
	Check( flushesBeforeError >= 2, "the log is on disk before Sys_Error" );
	Check( strstr( errorText, "write to read-only memory at code+0x1000" ) != NULL, "the alert says what and where" );
	Check( strstr( errorText, "etboot.log" ) != NULL, "and where the details are" );
}

static void Outside( void ) {
	Reset( kUnmappedMemoryException, 0x40800000, STACK_LOW + 0x4000 );
	Sys_CrashHandler( &info );
	Report();
	Check( strstr( logText, "PC 40800000 (outside the application)" ) != NULL, "a PC in a library is not given an offset" );
	Check( strstr( errorText, "40800000, outside the application" ) != NULL && !strstr( errorText, "code+" ),
		"nor in the alert, where code+0x0 would name the wrong function" );
}

static void Overflow( void ) {
	Reset( kAccessException, CODE_START + 0x1000, STACK_LOW - 0x40 );
	Sys_CrashHandler( &info );
	Report();
	Check( strstr( logText, "a stack overflow, or a fault on another stack" ) != NULL, "r1 under the heap limit is called what it is" );
	Check( !badReads, "and not walked" );
}

static void BadChains( void ) {
	static const uint32_t	links[] = { 0, STACK_LOW + 0x4000, STACK_LOW + 0x3000, STACK_LOW + 0x4002, STACK_HIGH, STACK_HIGH - 4, 0xfffffffcu, 0xffffffffu };
	int i;

	for ( i = 0 ; i < (int)( sizeof( links ) / sizeof( links[0] ) ) ; i++ ) {
		Reset( kAccessException, CODE_START + 0x1000, STACK_LOW + 0x4000 );
		fakeStack[0x1000] = links[i];
		Sys_CrashHandler( &info );
		Report();
		Check( !badReads && !strstr( logText, "frame" ), "a chain that does not climb ends the walk" );
		Check( errorText[0], "the report still finishes" );
	}
	/* a chain that never ends is cut off */
	Reset( kAccessException, CODE_START + 0x1000, STACK_LOW + 0x100 );
	BuildFrames( STACK_LOW + 0x100, 200 );
	Sys_CrashHandler( &info );
	Report();
	Check( strstr( logText, "frame 31 " ) && !strstr( logText, "frame 32 " ), "the walk has a limit" );
}

/* memoryInfo means nothing for other kinds, and a record without one has no fault address */
static void NoMemoryRecord( void ) {
	Reset( kIllegalInstructionException, CODE_START + 0x1000, STACK_LOW + 0x4000 );
	info.info.memoryInfo = (MemoryExceptionInformation *)(uintptr_t)1;
	Sys_CrashHandler( &info );
	Report();
	Check( strstr( logText, "*** CRASH: illegal instruction\n" ) != NULL && !strstr( logText, "fault address" ),
		"another kind's memoryInfo is not read" );
	Reset( kAccessException, CODE_START + 0x1000, STACK_LOW + 0x4000 );
	Sys_CrashHandler( &info );
	Report();
	Check( !strstr( logText, "fault address" ), "nor a missing one, or a previous fault's" );
}

static void Removes( void ) {
	Sys_RemoveCrashHandler();
	Check( installed == NULL, "sys_crashhandler 0 leaves no handler" );
	Install();
}

int main( void ) {
	Install();
	Removes();
	Passes();
	Resumes();
	Reports();
	NoMemoryRecord();
	Outside();
	Overflow();
	BadChains();
	if ( failures ) {
		return 1;
	}
	puts( "Mac crash handler logs a fault and exits through Sys_Error" );
	return 0;
}
