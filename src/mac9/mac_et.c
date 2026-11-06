// mac_et.c -- what Enemy Territory's engine asks of the platform beyond Quake III's
#include <stdio.h>
#include <Files.h>
#include <Events.h>
#include <Timer.h>
#include <string.h>
#include "../game/q_shared.h"
#include "../qcommon/qcommon.h"

// One thread: the critical sections guard nothing here.
void *Sys_InitializeCriticalSection( void ) { return NULL; }
void Sys_EnterCriticalSection( void *ptr ) {}
void Sys_LeaveCriticalSection( void *ptr ) {}

qboolean Sys_IsNumLockDown( void ) {
	return qfalse;
}

float Sys_GetCPUSpeed( void ) {
	return 0;
}

// No browser or second process on this platform, only a clean refusal.
void Sys_OpenURL( const char *url, qboolean doexit ) {
	Com_Printf( "Sys_OpenURL: not available on this platform: %s\n", url );
}

void Sys_StartProcess( char *cmdline, qboolean doexit ) {
	Com_Printf( "Sys_StartProcess: not available on this platform: %s\n", cmdline );
}

// No system entropy source. The microsecond clock's low bits are the best
// available, stirred so that each byte depends on all the earlier reads.
qboolean Sys_RandomBytes( void *bytes, int len ) {
	byte			*out = bytes;
	unsigned int	state = TickCount();
	UnsignedWide	now;
	int				i;

	for ( i = 0; i < len; i++ ) {
		Microseconds( &now );
		state = state * 69069 + now.lo + 1;
		out[i] = (byte)( state >> 16 );
	}
	return qtrue;
}

// newlib's stub: the File Manager removes an empty directory like a file.
int rmdir( const char *path ) {
	Str255	name;
	FSSpec	spec;
	int		length = strlen( path );

	if ( length > 255 ) {
		return -1;
	}
	name[0] = length;
	memcpy( name + 1, path, length );
	if ( FSMakeFSSpec( 0, 0, name, &spec ) != noErr ) {
		return -1;
	}
	return FSpDelete( &spec ) == noErr ? 0 : -1;
}

int gl_NormalFontBase;
