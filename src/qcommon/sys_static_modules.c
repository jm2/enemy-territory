// sys_static_modules.c -- the mod's modules linked into the application.
//
// A shared library gets a fresh image on every load. Linked in, a module keeps
// whatever it last wrote, so each load restores its data and clears its bss
// (vm_static.c), as Quake III's Mac port does for the same reason.
#include "../game/q_shared.h"
#include "qcommon.h"
#include "vm_static.h"

#ifdef ET_STATIC_MODULES

#include <stdint.h>

typedef intptr_t ( QDECL * modSyscall_t )( intptr_t, ... );

#define MODULE_EXTERNS( x, pfx ) \
	extern intptr_t pfx ## vmMain( intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t ); \
	extern void pfx ## dllEntry( modSyscall_t );

// Brackets: ELF publishes the renamed sections, the Mac link script names them.
#if defined( __ELF__ )
#define BRACKETS( x ) \
	extern unsigned char __start_moddata_ ## x[], __stop_moddata_ ## x[], __start_modbss_ ## x[], __stop_modbss_ ## x[];
#define MODULE_ENTRY( n, x ) { n, __start_moddata_ ## x, __stop_moddata_ ## x, __start_modbss_ ## x, __stop_modbss_ ## x }
#else
#define BRACKETS( x ) \
	extern unsigned char etstatic_ ## x ## _data_start[], etstatic_ ## x ## _data_end[], etstatic_ ## x ## _bss_start[], etstatic_ ## x ## _bss_end[];
#define MODULE_ENTRY( n, x ) { n, etstatic_ ## x ## _data_start, etstatic_ ## x ## _data_end, etstatic_ ## x ## _bss_start, etstatic_ ## x ## _bss_end }
#endif

MODULE_EXTERNS( qagame, etqa_ )
BRACKETS( qagame )
#ifndef DEDICATED
MODULE_EXTERNS( cgame, etcg_ )
MODULE_EXTERNS( ui, etui_ )
BRACKETS( cgame )
BRACKETS( ui )
#endif

static vmStaticModule_t sys_staticModules[] = {
	MODULE_ENTRY( "qagame", qagame ),
#ifndef DEDICATED
	MODULE_ENTRY( "cgame", cgame ),
	MODULE_ENTRY( "ui", ui ),
#endif
};
#define NUM_STATIC_MODULES ( sizeof( sys_staticModules ) / sizeof( sys_staticModules[0] ) )

/*
=================
Sys_InitStaticModules

Before any module runs: their initial images are saved here.
=================
*/
void Sys_InitStaticModules( void ) {
	const char *error = VM_InitStaticModules( sys_staticModules, NUM_STATIC_MODULES );

	if ( error ) {
		Sys_Error( "Sys_InitStaticModules: %s", error );
	}
}

#ifndef DEDICATED
/*
=================
Sys_ReferenceStaticModule

A pure server takes the checksum of the pk3 holding cgame and ui from the
references opening those libraries sets (see FS_ReferencedPakPureChecksums).
Nothing opens them here, so open the release pk3's library by name.
=================
*/
static void Sys_ReferenceStaticModule( const char *name ) {
	fileHandle_t	f;

	FS_FOpenFileRead( Sys_GetDLLName( name ), &f, qfalse );
	if ( f ) {
		FS_FCloseFile( f );
	}
}
#endif

/*
=================
Sys_GetDLLName
=================
*/
char *Sys_GetDLLName( const char *name ) {
	return va( "%s.mp.i386.so", name );
}

/*
=================
Sys_LoadDll

The handle is the module's table entry, so Sys_UnloadDll can mark it unloaded.
=================
*/
void *Sys_LoadDll( const char *name, char *fqpath, int ( **entryPoint )( int, ... ), int ( *systemcalls )( int, ... ) ) {
	vmStaticModule_t	*module;
	const char			*error;

	*fqpath = 0;
	module = VM_FindStaticModule( sys_staticModules, NUM_STATIC_MODULES, name );
	if ( !module ) {
		return NULL;
	}
	error = VM_LoadStaticModule( module );
	if ( error ) {
		Com_Error( ERR_FATAL, "Sys_LoadDll( %s ): %s", name, error );
	}

#ifndef DEDICATED
	if ( !Q_stricmp( name, "ui" ) ) {
		Sys_ReferenceStaticModule( name );
		*entryPoint = (int ( * )( int, ... ))etui_vmMain;
		etui_dllEntry( (modSyscall_t)systemcalls );
		return module;
	}
	if ( !Q_stricmp( name, "cgame" ) ) {
		Sys_ReferenceStaticModule( name );
		*entryPoint = (int ( * )( int, ... ))etcg_vmMain;
		etcg_dllEntry( (modSyscall_t)systemcalls );
		return module;
	}
#endif
	*entryPoint = (int ( * )( int, ... ))etqa_vmMain;
	etqa_dllEntry( (modSyscall_t)systemcalls );
	return module;
}

/*
=================
Sys_UnloadDll
=================
*/
void Sys_UnloadDll( void *dllHandle ) {
	if ( dllHandle ) {
		VM_UnloadStaticModule( (vmStaticModule_t *)dllHandle );
	}
}

#endif // ET_STATIC_MODULES
