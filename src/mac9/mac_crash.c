// mac_crash.c -- turns a machine exception into a logged, recoverable exit
//
// A wild pointer otherwise ends in a bomb or a frozen, captured screen, with
// the last log line as the only clue. The exception handler keeps it small:
// an exception is no place for the File Manager, so it saves the registers and
// resumes at Sys_CrashReport, with this fragment's TOC in case the fault was
// in a shared library. That runs as ordinary code: it writes the registers and
// a stack walk to the boot log, then leaves through Sys_Error, which gives the
// display back and shows the alert.
//
// The report runs on the stack that faulted, below the frames it wants to
// walk, while that stack has room. crashStack is only for a stack that does
// not: it lies in the globals, below the heap limit, so the system's stack
// sniffer would call it an overflow (system error 28), so the handler clears
// the sniffer's low-water mark first, as Apple's Switch Stack sample does.
//
// Addresses are logged as code+0xNNNN, offsets into this application's code
// section, which are the addresses in the XCOFF the build leaves next to the
// PEF: mac_symbolize.py turns a log's offsets into source lines.
//
// A fault during the report is passed on, to MacsBug or the system, rather
// than handled again. So is any kind of exception a debugger wants. With
// MacsBug installed this handler runs first, and "+set sys_crashhandler 0"
// leaves crashes to MacsBug. A fault at interrupt time, in the sound callback
// or an Open Transport notifier, comes here too, and the report's file and
// dialog calls may then freeze the machine rather than bomb it.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <MachineExceptions.h>
#include "mac_bootlog.h"

void Sys_LogRecord( const char *text );
void Sys_Error( const char *error, ... );

#define CRASH_STACK_SIZE	( 64 * 1024 )
#define CRASH_STACK_MARGIN	256			// the frame a called function may write into
#define CRASH_STACK_NEEDED	( 128 * 1024 )	// room left on a stack it will use; the report needs about 34 KB
#define CRASH_RED_ZONE		1024		// below r1, what a leaf function may be using
#define CRASH_MAX_FRAMES	32
#define CRASH_MAIN_SLACK	0x10000		// startup frames above the one that installs this

// a stack frame's first word links to the caller's, and the word two along
// holds what its callee saved of LR
#define FRAME_BACK_CHAIN	0
#define FRAME_SAVED_LR		8

// the regression stands in fakes for these
#ifndef CRASH_CODE_START
extern char _text[], _etext[];		// the code section, from the linker
#define CRASH_CODE_START	( (UInt32)(uintptr_t)_text )
#define CRASH_CODE_END		( (UInt32)(uintptr_t)_etext )
#endif
#ifndef CRASH_READ
#define CRASH_READ( address )	( *(const UInt32 *)(uintptr_t)( address ) )
#endif
#ifndef CRASH_SNIFFER_OFF
// StkLowPt: 0 stops the VBL stack sniffer. Written directly because
// LMSetStackLowPoint would be one more hard import, and a missing one stops the
// application launching at all, for a line only a rare crash reaches.
#define CRASH_SNIFFER_OFF()		( *(volatile UInt32 *)0x110 = 0 )
#endif
#ifndef CRASH_TVECTOR
// a function pointer here points at its transition vector: code address, TOC
#define CRASH_TVECTOR( function )	( (const UInt32 *)(uintptr_t)( function ) )
#endif

static volatile int				crashing;
static int						crashOnPrivateStack;
static ExceptionKind			crashKind;
static MemoryExceptionInformation	crashMemory;
static int						crashHasMemory;
static MachineInformation		crashMachine;
static RegisterInformation		crashRegisters;
static unsigned char			crashStack[CRASH_STACK_SIZE];
static UInt32					stackHigh, stackLow;

static const char *Sys_CrashKindName( ExceptionKind kind ) {
	switch ( kind ) {
	case kIllegalInstructionException:		return "illegal instruction";
	case kAccessException:					return "memory access";
	case kUnmappedMemoryException:			return "unmapped memory";
	case kExcludedMemoryException:			return "excluded memory";
	case kReadOnlyMemoryException:			return "write to read-only memory";
	case kUnresolvablePageFaultException:	return "unresolvable page fault";
	case kPrivilegeViolationException:		return "privileged instruction";
	case kStackOverflowException:			return "stack overflow";
	case kDataAlignmentException:			return "data alignment";
	}
	return NULL;
}

// memoryInfo is only filled in for these
static int Sys_CrashIsMemoryKind( ExceptionKind kind ) {
	return kind == kAccessException || kind == kUnmappedMemoryException ||
		kind == kExcludedMemoryException || kind == kReadOnlyMemoryException ||
		kind == kUnresolvablePageFaultException;
}

static void Sys_CrashPrint( const char *fmt, UInt32 a, UInt32 b, UInt32 c, UInt32 d ) {
	char	line[128];

	snprintf( line, sizeof( line ), fmt, (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d );
	Sys_LogRecord( line );
}

// an address in this application's code, as an offset, or 0
static UInt32 Sys_CrashCodeOffset( UInt32 address ) {
	if ( address >= CRASH_CODE_START && address < CRASH_CODE_END ) {
		return address - CRASH_CODE_START;
	}
	return 0;
}

static void Sys_CrashPrintAddress( const char *name, UInt32 address ) {
	UInt32	offset = Sys_CrashCodeOffset( address );
	char	line[64];

	if ( offset ) {
		snprintf( line, sizeof( line ), "*** %s %08lx code+0x%lx\n", name, (unsigned long)address, (unsigned long)offset );
	} else {
		snprintf( line, sizeof( line ), "*** %s %08lx (outside the application)\n", name, (unsigned long)address );
	}
	Sys_LogRecord( line );
}

/*
=================
Sys_CrashWalkStack

Follows the back chain from sp while it stays on the stack and keeps climbing,
so a corrupt chain ends the walk rather than the report.
=================
*/
static void Sys_CrashWalkStack( UInt32 sp ) {
	char	name[16];
	UInt32	next;
	int		frame;

	if ( sp < stackLow ) {
		Sys_CrashPrint( "*** r1 %08lx is below the heap limit %08lx: a stack overflow, or a fault on another stack\n",
			sp, stackLow, 0, 0 );
		return;
	}
	for ( frame = 0 ; frame < CRASH_MAX_FRAMES ; frame++ ) {
		if ( sp < stackLow || sp >= stackHigh || ( sp & 3 ) ) {
			return;
		}
		next = CRASH_READ( sp + FRAME_BACK_CHAIN );
		if ( next <= sp || next >= stackHigh - FRAME_SAVED_LR || ( next & 3 ) ) {
			return;
		}
		snprintf( name, sizeof( name ), "frame %2i", frame );
		Sys_CrashPrintAddress( name, CRASH_READ( next + FRAME_SAVED_LR ) );
		sp = next;
	}
}

/*
=================
Sys_CrashReport

Where the handler resumes. Not called by anything, so it must never return.
=================
*/
static void Sys_CrashReport( void ) {
	const UnsignedWide	*r = &crashRegisters.R0;
	UInt32				pc = crashMachine.PC.lo;
	const char			*kind = Sys_CrashKindName( crashKind );
	int					i;

	Sys_BootLogUnlock();	// the fault may have been inside a log write
	Sys_LogRecord( "\n*** CRASH: " );
	Sys_LogRecord( kind );
	Sys_LogRecord( "\n" );
	Sys_CrashPrintAddress( "PC", pc );
	Sys_CrashPrintAddress( "LR", crashMachine.LR.lo );
	// The system's own account of the fault. Under emulation at least, the
	// DAR, DSISR and MSR below it are not what the processor saw, and nor is
	// this record's theError, so that is left out.
	if ( crashHasMemory ) {
		Sys_CrashPrint( "*** fault address %08lx, reference %lu (0 write, 1 read, 2 execute)\n",
			(UInt32)(uintptr_t)crashMemory.theAddress, crashMemory.theReference, 0, 0 );
	}
	Sys_CrashPrint( "*** CTR %08lx DAR %08lx DSISR %08lx MSR %08lx\n",
		crashMachine.CTR.lo, crashMachine.DAR.lo, crashMachine.DSISR, crashMachine.MSR );
	for ( i = 0 ; i < 32 ; i += 4 ) {
		Sys_CrashPrint( "*** %08lx %08lx %08lx %08lx\n", r[i].lo, r[i + 1].lo, r[i + 2].lo, r[i + 3].lo );
	}
	// what is known so far stays known if the stack is too bad to walk
	Sys_BootLogFlush();
	Sys_CrashWalkStack( crashRegisters.R1.lo );
	Sys_BootLogFlush();

	if ( Sys_CrashCodeOffset( pc ) ) {
		Sys_Error( "The game crashed: %s at code+0x%lx. The details are in etboot.log.",
			kind, (unsigned long)Sys_CrashCodeOffset( pc ) );
	}
	Sys_Error( "The game crashed: %s at %08lx, outside the application. The details are in etboot.log.",
		kind, (unsigned long)pc );
}

/*
=================
Sys_CrashHandler

Runs at exception time. Returning noErr resumes with what is in info, and
anything else passes the exception on.
=================
*/
static OSStatus Sys_CrashHandler( ExceptionInformation *info ) {
	const UInt32	*report;
	UInt32			sp;

	if ( crashing || !Sys_CrashKindName( info->theKind ) ||
		!info->machineState || !info->registerImage ) {
		return -1;
	}
	crashing = 1;
	crashKind = info->theKind;
	crashMachine = *info->machineState;
	crashRegisters = *info->registerImage;
	crashHasMemory = 0;
	if ( Sys_CrashIsMemoryKind( info->theKind ) && info->info.memoryInfo ) {
		crashMemory = *info->info.memoryInfo;
		crashHasMemory = 1;
	}

	report = CRASH_TVECTOR( Sys_CrashReport );
	info->machineState->PC.lo = report[0];
	info->registerImage->R2.lo = report[1];
	sp = info->registerImage->R1.lo;
	crashOnPrivateStack = sp < stackLow + CRASH_STACK_NEEDED || sp >= stackHigh;
	if ( crashOnPrivateStack ) {
		CRASH_SNIFFER_OFF();
		sp = (UInt32)(uintptr_t)( crashStack + CRASH_STACK_SIZE - CRASH_STACK_MARGIN );
	} else {
		sp -= CRASH_RED_ZONE;
	}
	info->registerImage->R1.lo = sp & ~15UL;
	return noErr;
}

/*
=================
Sys_InstallCrashHandler

Per-process, so the system removes it when the application quits.
=================
*/
void Sys_InstallCrashHandler( void ) {
	int		stackProbe;

	stackHigh = (UInt32)(uintptr_t)&stackProbe + CRASH_MAIN_SLACK;
	stackLow = (UInt32)(uintptr_t)GetApplLimit();
	InstallExceptionHandler( Sys_CrashHandler );
}

/*
=================
Sys_RemoveCrashHandler

For "+set sys_crashhandler 0", once the parameters are read: back to the
system's handling, so a fault stops in MacsBug.
=================
*/
void Sys_RemoveCrashHandler( void ) {
	InstallExceptionHandler( NULL );
}
