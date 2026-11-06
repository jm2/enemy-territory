/* What the Unix platform layer took from assembly, in C, and the VM stubs: all
 * modules are native here, so nothing reaches the interpreter's compiler. */
#include <math.h>
#include <string.h>
#include "../../src/game/q_shared.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/qcommon/vm_local.h"

void Sys_SnapVector( float *v ) {
	v[0] = rint( v[0] );
	v[1] = rint( v[1] );
	v[2] = rint( v[2] );
}

void Sys_SnapVectorCW( float *v, unsigned short cw ) {
	Sys_SnapVector( v );
}

void VM_Compile( vm_t *vm, vmHeader_t *header ) {
	Com_Error( ERR_FATAL, "VM_Compile: no compiler in this build" );
}

int VM_CallCompiled( vm_t *vm, int *args ) {
	return 0;
}

void Snd_Memset( void *dest, const int val, const size_t count ) {
	memset( dest, val, count );
}

cvar_t *in_subframe;
