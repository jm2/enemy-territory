/* Fresh-image loads for statically linked game modules (issue #457). */
#ifndef VM_STATIC_H
#define VM_STATIC_H

/*
Retail 1.32c loads qagame, cgame and ui as QVMs, so every VM_Create and
VM_Restart starts a module from its image: initialized data as in the file,
everything else zero. A statically linked module keeps its globals instead,
so Sys_LoadDll puts that image back before each load. The linker brackets each
module's initialized data and its zero-initialized data (cmake/static_modules.py
on the Mac); code shared with the engine or another module stays outside.
*/
typedef struct {
	const char		*name;			// the module name VM_Create loads
	unsigned char	*dataStart, *dataEnd;	// initialized data
	unsigned char	*bssStart, *bssEnd;		// zero-initialized data
	unsigned char	*image;			// the initialized data before any module code ran
	int				loaded;			// between VM_LoadStaticModule and VM_UnloadStaticModule
} vmStaticModule_t;

// Checks that every bracket is ordered and that no two overlap, then saves
// each module's initialized data. Call it before any module code runs.
// Returns NULL, or what is wrong.
const char			*VM_InitStaticModules( vmStaticModule_t *modules, int count );

vmStaticModule_t	*VM_FindStaticModule( vmStaticModule_t *modules, int count, const char *name );

// Puts back the module's fresh image and marks it loaded. Returns NULL, or
// what is wrong (the module is still loaded, or was never saved), in which
// case nothing changes.
const char			*VM_LoadStaticModule( vmStaticModule_t *module );
void				VM_UnloadStaticModule( vmStaticModule_t *module );

#endif
