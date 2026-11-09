/* Fresh-image loads for statically linked game modules (issue #457). */
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vm_static.h"

// The brackets come from different objects, so compare addresses as integers.
static int VM_StaticRangesOverlap( const unsigned char *a, const unsigned char *aEnd,
	const unsigned char *b, const unsigned char *bEnd ) {
	return (uintptr_t)a < (uintptr_t)bEnd && (uintptr_t)b < (uintptr_t)aEnd;
}

/*
A module's data and bss are copied and cleared whole, from one bracket to the
other. Under AddressSanitizer they hold the redzones between its globals too,
which memcpy and memset report as overflows, so a sanitized build copies them
byte by byte without instrumentation (and without the loop becoming a memcpy).
*/
#if defined( __SANITIZE_ADDRESS__ )
__attribute__( ( no_sanitize_address, noinline, optimize( "no-tree-loop-distribute-patterns" ) ) )
static void VM_StaticCopy( unsigned char *to, const unsigned char *from, size_t size ) {
	while ( size-- ) {
		*to++ = *from++;
	}
}
__attribute__( ( no_sanitize_address, noinline, optimize( "no-tree-loop-distribute-patterns" ) ) )
static void VM_StaticClear( unsigned char *to, size_t size ) {
	while ( size-- ) {
		*to++ = 0;
	}
}
#else
#define VM_StaticCopy( to, from, size ) memcpy( ( to ), ( from ), ( size ) )
#define VM_StaticClear( to, size ) memset( ( to ), 0, ( size ) )
#endif

/*
=================
VM_InitStaticModules
=================
*/
const char *VM_InitStaticModules( vmStaticModule_t *modules, int count ) {
	int		i, j;

	for ( i = 0; i < count; i++ ) {
		vmStaticModule_t *m = &modules[i];

		if ( !m->name || !m->dataStart || !m->dataEnd || !m->bssStart || !m->bssEnd ||
			(uintptr_t)m->dataEnd < (uintptr_t)m->dataStart ||
			(uintptr_t)m->bssEnd < (uintptr_t)m->bssStart ) {
			return "a static module bracket is missing or reversed";
		}
		if ( VM_StaticRangesOverlap( m->dataStart, m->dataEnd, m->bssStart, m->bssEnd ) ) {
			return "a static module's data and bss brackets overlap";
		}
		for ( j = 0; j < i; j++ ) {
			vmStaticModule_t *o = &modules[j];

			if ( VM_StaticRangesOverlap( m->dataStart, m->dataEnd, o->dataStart, o->dataEnd ) ||
				VM_StaticRangesOverlap( m->dataStart, m->dataEnd, o->bssStart, o->bssEnd ) ||
				VM_StaticRangesOverlap( m->bssStart, m->bssEnd, o->dataStart, o->dataEnd ) ||
				VM_StaticRangesOverlap( m->bssStart, m->bssEnd, o->bssStart, o->bssEnd ) ) {
				return "two static modules' brackets overlap";
			}
		}
	}

	for ( i = 0; i < count; i++ ) {
		vmStaticModule_t *m = &modules[i];
		size_t	size = (size_t)( m->dataEnd - m->dataStart );

		m->loaded = 0;
		m->image = malloc( size ? size : 1 );
		if ( !m->image ) {
			return "out of memory for the static module images";
		}
		VM_StaticCopy( m->image, m->dataStart, size );
	}
	return NULL;
}

/*
=================
VM_FindStaticModule

Names compare without case, as Sys_LoadDll always matched them.
=================
*/
vmStaticModule_t *VM_FindStaticModule( vmStaticModule_t *modules, int count, const char *name ) {
	int			i;
	const char	*a, *b;

	for ( i = 0; i < count; i++ ) {
		for ( a = modules[i].name, b = name;
			*a && tolower( (unsigned char)*a ) == tolower( (unsigned char)*b ); a++, b++ ) {
		}
		if ( !*a && !*b ) {
			return &modules[i];
		}
	}
	return NULL;
}

/*
=================
VM_LoadStaticModule

What loading the module's QVM image did in retail: the initialized data as it
was before the module first ran, and every other module global zero.
=================
*/
const char *VM_LoadStaticModule( vmStaticModule_t *module ) {
	if ( module->loaded ) {
		return "the static module is already loaded";
	}
	if ( !module->image ) {
		return "the static module's image was never saved";
	}
	VM_StaticCopy( module->dataStart, module->image, (size_t)( module->dataEnd - module->dataStart ) );
	VM_StaticClear( module->bssStart, (size_t)( module->bssEnd - module->bssStart ) );
	module->loaded = 1;
	return NULL;
}

/*
=================
VM_UnloadStaticModule
=================
*/
void VM_UnloadStaticModule( vmStaticModule_t *module ) {
	module->loaded = 0;
}
