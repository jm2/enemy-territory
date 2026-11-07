/* A fake File Manager for host regressions of code/mac/mac_syscalls.c, which
 * the regression includes whole; the runner points <reent.h>, <Files.h> and
 * <Errors.h> here.  Types and constants are those of Universal Interfaces 3.4
 * (Retro68 Files.h, MacErrors.h, StringCompare.h) and newlib's struct
 * _reent, cut to what mac_syscalls.c uses.
 *
 * The fake follows HFS: one flat table of files, each named by its full
 * path ("<directory>:<leaf>") and with a Finder lock and a length that
 * stands for its contents; names compare without regard to case.  A
 * directory is the path up to and including its last colon; adding a file
 * adds its directory, and FakeFM_AddDir adds one on another volume.  The
 * volume can be software-locked (vLckdErr) or read-only media (wPrErr).  A
 * request for write access to a locked file or volume fails, as does
 * exclusive write access to a file another path can write (opWrErr);
 * fsRdPerm is always granted.  SetEOF needs write access.
 * fakeFMHOpenDFUnsupported makes HOpenDF return paramErr, as on file
 * systems without it.  Every call is counted and the last permission
 * requested is kept.
 *
 * For rename and unlink: FSMakeFSSpec resolves a path to its directory ID
 * and leaf (dirNFErr for a missing directory, bdNamErr for a leaf longer
 * than HFS's 31 characters, fnfErr with a usable spec for a missing leaf).
 * FSpRename and HRename rename within a directory, CatMove (with a nil
 * newName) moves to another directory of the same volume and keeps the
 * name, FSpDelete and HDelete delete; all of them fail on a locked volume,
 * renaming or deleting a locked file is fLckdErr, deleting an open file is
 * fBsyErr, and a name already taken is dupFNErr.  FSpExchangeFiles swaps
 * two files' contents and open paths, keeping names and locks, and fails
 * for a locked file (fLckdErr) or files on two volumes (diffVolErr);
 * fakeFMExchangeErr makes it fail with that error instead (paramErr or
 * wrgVolTypErr, as on volumes without it, or another failure).
 *
 * Every file has a catalog ID (its file number), which stays with its entry
 * through renames, moves and exchanges (ExchangeFiles keeps file IDs with
 * the names); PBGetCatInfoSync returns it in ioDirID.  fakeFMNameLimit
 * makes the File Manager truncate leaf names to that many bytes instead of
 * refusing those longer than 31 (31 is what issue #327 fears HFS may do; a
 * shorter limit stands for any file system that folds two names into one):
 * FSMakeFSSpec keeps the name as given, up to a Str63, and every lookup,
 * creation and rename uses the truncated name. */
#ifndef MAC_FILES_FAKE_H
#define MAC_FILES_FAKE_H

#include <stdint.h>
#include <string.h>
#include <strings.h>

typedef int16_t			OSErr;
typedef int8_t			SInt8;
typedef uint32_t		OSType;
typedef unsigned char	Str255[256];
typedef unsigned char	*StringPtr;
typedef unsigned char	Str63[64];
typedef const unsigned char	*ConstStr255Param;
typedef unsigned char	Boolean;

enum { false = 0, true = 1 };

typedef struct FSSpec {
	short	vRefNum;
	long	parID;
	Str63	name;
} FSSpec;

/* the fields of CInfoPBRec's hFileInfo that PBGetCatInfoSync uses */
typedef struct HFileInfo {
	StringPtr	ioNamePtr;
	short		ioVRefNum;
	short		ioFDirIndex;
	SInt8		ioFlAttrib;
	long		ioDirID;
} HFileInfo;

typedef union CInfoPBRec {
	HFileInfo	hFileInfo;
} CInfoPBRec, *CInfoPBPtr;

struct _reent {
	int	_errno;
};

enum { noErr = 0 };
enum {
	dskFulErr = -34, ioErr = -36, nsvErr = -35, bdNamErr = -37, tmfoErr = -42, fnfErr = -43,
	wPrErr = -44, fLckdErr = -45, vLckdErr = -46, fBsyErr = -47, dupFNErr = -48,
	opWrErr = -49, paramErr = -50, permErr = -54, wrPermErr = -61, dirNFErr = -120,
	badMovErr = -122, wrgVolTypErr = -123, notAFileErr = -1302, diffVolErr = -1303,
	afpAccessDenied = -5000
};
enum { fsCurPerm = 0, fsRdPerm = 1, fsWrPerm = 2, fsRdWrPerm = 3, fsRdWrShPerm = 4 };

enum { FAKE_FM_FILES = 8, FAKE_FM_PATHS = 8, FAKE_FM_FIRST_REF = 100 };
enum { FAKE_FM_DIRS = 8, FAKE_FM_FIRST_DIR_ID = 100, FAKE_FM_FIRST_FILE_ID = 1000, FAKE_FM_VREFNUM = -1 };
enum { FAKE_VOLUME_WRITABLE, FAKE_VOLUME_SOFTWARE_LOCKED, FAKE_VOLUME_READ_ONLY_MEDIA };

typedef struct {
	int		used;
	char	name[256];
	long	id;
	int		locked;
	long	eof;
	OSType	creator, type;
} fakeFMFile_t;

typedef struct {
	int		used;
	int		file;
	SInt8	permission;
} fakeFMPath_t;

typedef struct {
	int		used;
	char	path[256];	// up to and including the last colon; "" is the default directory
	short	vRefNum;
} fakeFMDir_t;

static fakeFMFile_t	fakeFMFiles[FAKE_FM_FILES];
static fakeFMDir_t	fakeFMDirs[FAKE_FM_DIRS];
static fakeFMPath_t	fakeFMPaths[FAKE_FM_PATHS];
static int			fakeFMVolume;
static int			fakeFMHOpenDFUnsupported, fakeFMNameLimit;
static OSErr		fakeFMExchangeErr;
static long			fakeFMNextID;
static int			fakeFMRenames, fakeFMMoves, fakeFMDeletes, fakeFMExchanges, fakeFMCatInfos;
static int			fakeFMCreates, fakeFMOpenDFs, fakeFMOpens, fakeFMSetEOFs, fakeFMCloses;
static int			fakeFMLastPermission = -1;

const int kMacRefNumOffset = 10;	/* libretro/syscalls.c */

static void FakeFM_Reset( void ) {
	memset( fakeFMFiles, 0, sizeof( fakeFMFiles ) );
	memset( fakeFMPaths, 0, sizeof( fakeFMPaths ) );
	memset( fakeFMDirs, 0, sizeof( fakeFMDirs ) );
	fakeFMVolume = FAKE_VOLUME_WRITABLE;
	fakeFMHOpenDFUnsupported = fakeFMNameLimit = 0;
	fakeFMExchangeErr = noErr;
	fakeFMNextID = FAKE_FM_FIRST_FILE_ID;
	fakeFMRenames = fakeFMMoves = fakeFMDeletes = fakeFMExchanges = fakeFMCatInfos = 0;
	fakeFMCreates = fakeFMOpenDFs = fakeFMOpens = fakeFMSetEOFs = fakeFMCloses = 0;
	fakeFMLastPermission = -1;
}

/* The directory with this path (length bytes of it), added on vRefNum if
 * add is set and it is missing; its index, or -1. */
static int FakeFM_Dir( const char *path, size_t length, short vRefNum, int add ) {
	int i;

	for ( i = 0 ; i < FAKE_FM_DIRS ; i++ ) {
		if ( fakeFMDirs[i].used && strlen( fakeFMDirs[i].path ) == length &&
			!strncasecmp( fakeFMDirs[i].path, path, length ) ) {
			return i;
		}
	}
	if ( !add ) {
		return -1;
	}
	for ( i = 0 ; i < FAKE_FM_DIRS ; i++ ) {
		if ( !fakeFMDirs[i].used ) {
			fakeFMDirs[i].used = 1;
			memcpy( fakeFMDirs[i].path, path, length );
			fakeFMDirs[i].path[length] = 0;
			fakeFMDirs[i].vRefNum = vRefNum;
			return i;
		}
	}
	return -1;
}

/* The length of the directory part of a full name. */
static size_t FakeFM_DirLength( const char *name, size_t length ) {
	while ( length && name[length - 1] != ':' ) {
		length--;
	}
	return length;
}

/* The length of a full name as the File Manager keeps it: with
 * fakeFMNameLimit, the leaf cut to that many bytes. */
static size_t FakeFM_Truncated( const char *name, size_t length ) {
	size_t dirLength = FakeFM_DirLength( name, length );

	if ( fakeFMNameLimit && length - dirLength > (size_t)fakeFMNameLimit ) {
		return dirLength + fakeFMNameLimit;
	}
	return length;
}

static void FakeFM_AddDir( const char *path, short vRefNum ) {
	FakeFM_Dir( path, strlen( path ), vRefNum, 1 );
}

static int FakeFM_AddFile( const char *name, long eof, int locked ) {
	int i;

	FakeFM_Dir( name, FakeFM_DirLength( name, strlen( name ) ), FAKE_FM_VREFNUM, 1 );
	for ( i = 0 ; i < FAKE_FM_FILES ; i++ ) {
		if ( !fakeFMFiles[i].used ) {
			fakeFMFiles[i].used = 1;
			strcpy( fakeFMFiles[i].name, name );
			fakeFMFiles[i].name[FakeFM_Truncated( name, strlen( name ) )] = 0;
			fakeFMFiles[i].id = fakeFMNextID++;
			fakeFMFiles[i].eof = eof;
			fakeFMFiles[i].locked = locked;
			return i;
		}
	}
	return -1;
}

static int FakeFM_Find( ConstStr255Param name ) {
	size_t length = FakeFM_Truncated( (const char *)name + 1, name[0] );
	int i;

	for ( i = 0 ; i < FAKE_FM_FILES ; i++ ) {
		if ( fakeFMFiles[i].used && strlen( fakeFMFiles[i].name ) == length &&
			!strncasecmp( fakeFMFiles[i].name, (const char *)name + 1, length ) ) {
			return i;
		}
	}
	return -1;
}

static fakeFMFile_t *FakeFM_File( const char *name ) {
	Str255 pname;
	int i;

	pname[0] = (unsigned char)strlen( name );
	memcpy( pname + 1, name, pname[0] );
	i = FakeFM_Find( pname );
	return i < 0 ? NULL : &fakeFMFiles[i];
}

static fakeFMPath_t *FakeFM_Path( short refNum ) {
	int i = refNum - FAKE_FM_FIRST_REF;

	if ( i < 0 || i >= FAKE_FM_PATHS || !fakeFMPaths[i].used ) {
		return NULL;
	}
	return &fakeFMPaths[i];
}

static int FakeFM_OpenPaths( void ) {
	int i, count = 0;

	for ( i = 0 ; i < FAKE_FM_PATHS ; i++ ) {
		count += fakeFMPaths[i].used;
	}
	return count;
}

static OSErr FakeFM_VolumeWriteErr( void ) {
	if ( fakeFMVolume == FAKE_VOLUME_READ_ONLY_MEDIA ) {
		return wPrErr;
	}
	if ( fakeFMVolume == FAKE_VOLUME_SOFTWARE_LOCKED ) {
		return vLckdErr;
	}
	return noErr;
}

static OSErr HCreate( short vRefNum, long dirID, ConstStr255Param fileName, OSType creator, OSType fileType ) {
	OSErr err;
	char name[256];
	int i;

	fakeFMCreates++;
	if ( vRefNum != 0 || dirID != 0 ) {
		return paramErr;
	}
	if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
		return err;
	}
	if ( FakeFM_Find( fileName ) >= 0 ) {
		return dupFNErr;
	}
	memcpy( name, fileName + 1, fileName[0] );
	name[fileName[0]] = 0;
	i = FakeFM_AddFile( name, 0, 0 );
	if ( i < 0 ) {
		return dskFulErr;
	}
	fakeFMFiles[i].creator = creator;
	fakeFMFiles[i].type = fileType;
	return noErr;
}

static OSErr FakeFM_Open( ConstStr255Param fileName, SInt8 permission, short *refNum ) {
	int writes = permission != fsRdPerm;
	OSErr err;
	int file, i;

	fakeFMLastPermission = permission;
	file = FakeFM_Find( fileName );
	if ( file < 0 ) {
		return fnfErr;
	}
	if ( writes ) {
		if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
			return err;
		}
		if ( fakeFMFiles[file].locked ) {
			return permErr;
		}
		for ( i = 0 ; i < FAKE_FM_PATHS ; i++ ) {
			if ( fakeFMPaths[i].used && fakeFMPaths[i].file == file &&
				fakeFMPaths[i].permission != fsRdPerm &&
				!( permission == fsRdWrShPerm && fakeFMPaths[i].permission == fsRdWrShPerm ) ) {
				*refNum = (short)( FAKE_FM_FIRST_REF + i );
				return opWrErr;
			}
		}
	}
	for ( i = 0 ; i < FAKE_FM_PATHS ; i++ ) {
		if ( !fakeFMPaths[i].used ) {
			fakeFMPaths[i].used = 1;
			fakeFMPaths[i].file = file;
			fakeFMPaths[i].permission = permission;
			*refNum = (short)( FAKE_FM_FIRST_REF + i );
			return noErr;
		}
	}
	return tmfoErr;
}

static OSErr HOpenDF( short vRefNum, long dirID, ConstStr255Param fileName, SInt8 permission, short *refNum ) {
	fakeFMOpenDFs++;
	if ( fakeFMHOpenDFUnsupported ) {
		return paramErr;
	}
	if ( vRefNum != 0 || dirID != 0 ) {
		return paramErr;
	}
	return FakeFM_Open( fileName, permission, refNum );
}

static OSErr HOpen( short vRefNum, long dirID, ConstStr255Param fileName, SInt8 permission, short *refNum ) {
	fakeFMOpens++;
	if ( vRefNum != 0 || dirID != 0 ) {
		return paramErr;
	}
	return FakeFM_Open( fileName, permission, refNum );
}

static OSErr SetEOF( short refNum, long logEOF ) {
	fakeFMPath_t *path = FakeFM_Path( refNum );

	fakeFMSetEOFs++;
	if ( !path ) {
		return paramErr;
	}
	if ( path->permission == fsRdPerm ) {
		return wrPermErr;
	}
	fakeFMFiles[path->file].eof = logEOF;
	return noErr;
}

static OSErr FSClose( short refNum ) {
	fakeFMPath_t *path = FakeFM_Path( refNum );

	fakeFMCloses++;
	if ( !path ) {
		return paramErr;
	}
	path->used = 0;
	return noErr;
}

/* Rename and unlink. */

static Boolean EqualString( ConstStr255Param a, ConstStr255Param b, Boolean caseSensitive, Boolean diacSensitive ) {
	(void)diacSensitive;
	return a[0] == b[0] && !( caseSensitive ? memcmp( a + 1, b + 1, a[0] ) :
		strncasecmp( (const char *)a + 1, (const char *)b + 1, a[0] ) );
}

static OSErr FakeFM_SpecDir( short vRefNum, long dirID, int *dir ) {
	int i = (int)( dirID - FAKE_FM_FIRST_DIR_ID );

	if ( i < 0 || i >= FAKE_FM_DIRS || !fakeFMDirs[i].used || fakeFMDirs[i].vRefNum != vRefNum ) {
		return dirNFErr;
	}
	*dir = i;
	return noErr;
}

/* The file named leaf in directory dirID: its index, or an error. */
static OSErr FakeFM_Lookup( short vRefNum, long dirID, ConstStr255Param leaf, int *file ) {
	Str255 full;
	OSErr err;
	size_t length;
	int dir;

	if ( ( err = FakeFM_SpecDir( vRefNum, dirID, &dir ) ) != noErr ) {
		return err;
	}
	length = strlen( fakeFMDirs[dir].path );
	if ( !leaf || length + leaf[0] > 255 ) {
		return bdNamErr;
	}
	full[0] = (unsigned char)( length + leaf[0] );
	memcpy( full + 1, fakeFMDirs[dir].path, length );
	memcpy( full + 1 + length, leaf + 1, leaf[0] );
	*file = FakeFM_Find( full );
	return *file < 0 ? fnfErr : noErr;
}

/* Name the file leaf in directory dir. */
static OSErr FakeFM_SetName( int file, int dir, ConstStr255Param leaf ) {
	size_t length = strlen( fakeFMDirs[dir].path );
	size_t leafLength = leaf[0];

	if ( fakeFMNameLimit && leafLength > (size_t)fakeFMNameLimit ) {
		leafLength = fakeFMNameLimit;
	}
	if ( length + leafLength > 255 ) {
		return bdNamErr;
	}
	memcpy( fakeFMFiles[file].name, fakeFMDirs[dir].path, length );
	memcpy( fakeFMFiles[file].name + length, leaf + 1, leafLength );
	fakeFMFiles[file].name[length + leafLength] = 0;
	return noErr;
}

static int FakeFM_IsOpen( int file ) {
	int i;

	for ( i = 0 ; i < FAKE_FM_PATHS ; i++ ) {
		if ( fakeFMPaths[i].used && fakeFMPaths[i].file == file ) {
			return 1;
		}
	}
	return 0;
}

static OSErr FSMakeFSSpec( short vRefNum, long dirID, ConstStr255Param fileName, FSSpec *spec ) {
	size_t dirLength;
	int dir, file;

	if ( vRefNum != 0 || dirID != 0 ) {
		return paramErr;
	}
	dirLength = FakeFM_DirLength( (const char *)fileName + 1, fileName[0] );
	dir = FakeFM_Dir( (const char *)fileName + 1, dirLength, 0, 0 );
	if ( dir < 0 ) {
		return dirNFErr;
	}
	if ( fileName[0] == dirLength || fileName[0] - dirLength > ( fakeFMNameLimit ? 63 : 31 ) ) {
		return bdNamErr;
	}
	memset( spec, 0, sizeof( *spec ) );
	spec->vRefNum = fakeFMDirs[dir].vRefNum;
	spec->parID = FAKE_FM_FIRST_DIR_ID + dir;
	spec->name[0] = (unsigned char)( fileName[0] - dirLength );
	memcpy( spec->name + 1, fileName + 1 + dirLength, spec->name[0] );
	return FakeFM_Lookup( spec->vRefNum, spec->parID, spec->name, &file );
}

static OSErr FakeFM_Rename( short vRefNum, long dirID, ConstStr255Param oldName, ConstStr255Param newName ) {
	OSErr err;
	int file, other, dir;

	fakeFMRenames++;
	if ( ( err = FakeFM_Lookup( vRefNum, dirID, oldName, &file ) ) != noErr ) {
		return err;
	}
	if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
		return err;
	}
	if ( fakeFMFiles[file].locked ) {
		return fLckdErr;
	}
	if ( !newName[0] || newName[0] > ( fakeFMNameLimit ? 63 : 31 ) || memchr( newName + 1, ':', newName[0] ) ) {
		return bdNamErr;
	}
	err = FakeFM_Lookup( vRefNum, dirID, newName, &other );
	if ( err == noErr && other != file ) {
		return dupFNErr;
	}
	FakeFM_SpecDir( vRefNum, dirID, &dir );
	return FakeFM_SetName( file, dir, newName );
}

static OSErr FSpRename( const FSSpec *spec, ConstStr255Param newName ) {
	return FakeFM_Rename( spec->vRefNum, spec->parID, spec->name, newName );
}

static OSErr HRename( short vRefNum, long dirID, ConstStr255Param oldName, ConstStr255Param newName ) {
	return FakeFM_Rename( vRefNum, dirID, oldName, newName );
}

static OSErr CatMove( short vRefNum, long dirID, ConstStr255Param oldName, long newDirID, ConstStr255Param newName ) {
	OSErr err;
	int file, other, dir;

	fakeFMMoves++;
	if ( newName != NULL ) {
		return paramErr;	// the fake only takes the destination as a directory ID
	}
	if ( ( err = FakeFM_Lookup( vRefNum, dirID, oldName, &file ) ) != noErr ) {
		return err;
	}
	if ( ( err = FakeFM_SpecDir( vRefNum, newDirID, &dir ) ) != noErr ) {
		return err;
	}
	if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
		return err;
	}
	if ( FakeFM_Lookup( vRefNum, newDirID, oldName, &other ) == noErr ) {
		return dupFNErr;
	}
	return FakeFM_SetName( file, dir, oldName );
}

static OSErr FakeFM_Delete( int file ) {
	OSErr err;

	if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
		return err;
	}
	if ( fakeFMFiles[file].locked ) {
		return fLckdErr;
	}
	if ( FakeFM_IsOpen( file ) ) {
		return fBsyErr;
	}
	memset( &fakeFMFiles[file], 0, sizeof( fakeFMFiles[file] ) );
	return noErr;
}

static OSErr FSpDelete( const FSSpec *spec ) {
	OSErr err;
	int file;

	fakeFMDeletes++;
	if ( ( err = FakeFM_Lookup( spec->vRefNum, spec->parID, spec->name, &file ) ) != noErr ) {
		return err;
	}
	return FakeFM_Delete( file );
}

static OSErr HDelete( short vRefNum, long dirID, ConstStr255Param fileName ) {
	FSSpec spec;
	OSErr err;
	int file;

	fakeFMDeletes++;
	if ( vRefNum != 0 || dirID != 0 ) {
		return paramErr;
	}
	if ( fileName[0] > 0 && fileName[fileName[0]] == ':' ) {
		return bdNamErr;	// the fake has no directory entries to delete
	}
	if ( ( err = FSMakeFSSpec( 0, 0, fileName, &spec ) ) != noErr ) {
		return err;
	}
	FakeFM_Lookup( spec.vRefNum, spec.parID, spec.name, &file );
	return FakeFM_Delete( file );
}

static OSErr FSpExchangeFiles( const FSSpec *source, const FSSpec *dest ) {
	OSErr err;
	long eof;
	int a, b, i;

	fakeFMExchanges++;
	if ( fakeFMExchangeErr != noErr ) {
		return fakeFMExchangeErr;
	}
	if ( ( err = FakeFM_Lookup( source->vRefNum, source->parID, source->name, &a ) ) != noErr ||
		( err = FakeFM_Lookup( dest->vRefNum, dest->parID, dest->name, &b ) ) != noErr ) {
		return err;
	}
	if ( source->vRefNum != dest->vRefNum ) {
		return diffVolErr;
	}
	if ( ( err = FakeFM_VolumeWriteErr() ) != noErr ) {
		return err;
	}
	if ( fakeFMFiles[a].locked || fakeFMFiles[b].locked ) {
		return fLckdErr;
	}
	eof = fakeFMFiles[a].eof;
	fakeFMFiles[a].eof = fakeFMFiles[b].eof;
	fakeFMFiles[b].eof = eof;
	for ( i = 0 ; i < FAKE_FM_PATHS ; i++ ) {
		if ( fakeFMPaths[i].used && ( fakeFMPaths[i].file == a || fakeFMPaths[i].file == b ) ) {
			fakeFMPaths[i].file = a + b - fakeFMPaths[i].file;
		}
	}
	return noErr;
}

/* Only a lookup by name (ioFDirIndex 0) in directory ioDirID, as
 * mac_syscalls.c makes; ioDirID returns the file number. */
static OSErr PBGetCatInfoSync( CInfoPBPtr pb ) {
	OSErr err;
	int file;

	fakeFMCatInfos++;
	if ( pb->hFileInfo.ioFDirIndex != 0 || !pb->hFileInfo.ioNamePtr || !pb->hFileInfo.ioNamePtr[0] ) {
		return paramErr;
	}
	if ( ( err = FakeFM_Lookup( pb->hFileInfo.ioVRefNum, pb->hFileInfo.ioDirID, pb->hFileInfo.ioNamePtr, &file ) ) != noErr ) {
		return err;
	}
	pb->hFileInfo.ioFlAttrib = fakeFMFiles[file].locked ? 0x01 : 0;
	pb->hFileInfo.ioDirID = fakeFMFiles[file].id;
	return noErr;
}

#endif
