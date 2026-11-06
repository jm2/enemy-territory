// fs_path.c -- what counts as a safe path, for names that may come from a peer.
//
// From the Quake III port's FS_CheckQPath, which follows ioquake3 and adds HFS: ':' is the separator there, and each separator after
// another one walks up a directory, as ".." does elsewhere.
#include "../game/q_shared.h"
#include "qcommon.h"

#ifndef ET_PATH_SEP
#define ET_PATH_SEP PATH_SEP
#endif

/*
====================
FS_CheckQPath

Returns qfalse for a qpath that could leave its game directory once its
separators are replaced.
====================
*/
qboolean FS_CheckQPath( const char *qpath ) {
	const char *s;

	if ( strchr( qpath, ':' ) || strstr( qpath, ".." ) ) {
		return qfalse;
	}
	if ( ET_PATH_SEP == ':' ) {
		for ( s = qpath; *s; s++ ) {
			if ( ( *s == '/' || *s == '\\' ) && ( s == qpath || s[-1] == '/' || s[-1] == '\\' ) ) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

/*
====================
FS_CheckGameDir

A game directory is a single name directly under a search path.
====================
*/
qboolean FS_CheckGameDir( const char *dir ) {
	if ( !dir[0] || !strcmp( dir, "." ) || strstr( dir, ".." ) || strchr( dir, ':' ) ||
		 strchr( dir, '/' ) || strchr( dir, '\\' ) ) {
		return qfalse;
	}
	return qtrue;
}

/*
====================
FS_CheckPakName

A server's pak name, without its extension: "game/name" and nothing else. '@'
separates the fields of the download list, so it cannot appear either.
====================
*/
qboolean FS_CheckPakName( const char *name ) {
	const char *slash;

	if ( !FS_CheckQPath( name ) || strchr( name, '@' ) || strchr( name, '\\' ) ) {
		return qfalse;
	}
	slash = strchr( name, '/' );
	if ( !slash || slash == name || !slash[1] || strchr( slash + 1, '/' ) ) {
		return qfalse;
	}
	return qtrue;
}

/*
====================
FS_CheckDownloadName

A pair of the download list: a pak name that ends in .pk3, which a truncated
list would not.
====================
*/
qboolean FS_CheckDownloadName( const char *name ) {
	int length = strlen( name );

	if ( length <= 4 || Q_stricmp( name + length - 4, ".pk3" ) ) {
		return qfalse;
	}
	return FS_CheckPakName( name );
}
