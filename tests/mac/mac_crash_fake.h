/* What code/mac/mac_crash.c uses of <MacTypes.h>, <MacMemory.h> and
 * <MachineExceptions.h>, from Universal Interfaces 3.4. */
#ifndef MAC_CRASH_FAKE_H
#define MAC_CRASH_FAKE_H

#include <stdint.h>

typedef uint32_t	UInt32;
typedef int32_t		OSStatus;
typedef char		*Ptr;
typedef struct { UInt32 hi, lo; } UnsignedWide;
enum { noErr = 0 };

typedef struct {
	UnsignedWide	CTR, LR, PC;
	unsigned long	CR, XER, MSR, MQ, ExceptKind, DSISR;
	UnsignedWide	DAR, Reserved;
} MachineInformation;

typedef struct {
	UnsignedWide	R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15;
	UnsignedWide	R16, R17, R18, R19, R20, R21, R22, R23, R24, R25, R26, R27, R28, R29, R30, R31;
} RegisterInformation;

typedef unsigned long ExceptionKind;
enum {
	kUnknownException = 0, kIllegalInstructionException = 1, kTrapException = 2,
	kAccessException = 3, kUnmappedMemoryException = 4, kExcludedMemoryException = 5,
	kReadOnlyMemoryException = 6, kUnresolvablePageFaultException = 7,
	kPrivilegeViolationException = 8, kTraceException = 9,
	kInstructionBreakpointException = 10, kDataBreakpointException = 11,
	kIntegerException = 12, kFloatingPointException = 13, kStackOverflowException = 14,
	kDataAlignmentException = 17
};

typedef struct {
	unsigned long	theArea;
	void			*theAddress;
	long			theError;
	unsigned long	theReference;
} MemoryExceptionInformation;

typedef union {
	MemoryExceptionInformation	*memoryInfo;
} ExceptionInfo;

typedef struct {
	ExceptionKind		theKind;
	MachineInformation	*machineState;
	RegisterInformation	*registerImage;
	void				*FPUImage;
	ExceptionInfo		info;
} ExceptionInformation;

typedef OSStatus (*ExceptionHandler)( ExceptionInformation *theException );
ExceptionHandler	InstallExceptionHandler( ExceptionHandler theHandler );
Ptr					GetApplLimit( void );

#endif
