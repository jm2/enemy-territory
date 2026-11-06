// mac_syscalls.c
// newlib's open(), rename() and unlink() for the File Manager, replacing the
// ones in Retro68's libretro
//
// libretro's _open_r (Retro68 83b9c8d2c5, libretro/syscalls.c) works out the
// permission the caller asked for and then opens every file with fsRdWrPerm.
// That fails for a Finder-locked file (permErr), a CD or locked volume
// (wPrErr, vLckdErr) and a second open of a file another path already has
// open for writing (opWrErr), so fopen( path, "rb" ) could not read retail
// data from read-only media, or open a pk3 that was already open (issue #258).
//
// libretro's _rename_r and _unlink_r only return -1, so rename() and remove()
// (newlib's _remove_r calls _unlink_r) never worked: FS_SV_Rename finished
// every download by copying the whole .pk3.tmp through one malloc, and the
// .tmp was never deleted (issue #259).
//
// The linker takes these definitions: the AIX-style XCOFF linker ignores a
// second definition that comes from an archive member (bfd/xcofflink.c,
// "a redefinition in an object contained in an archive"), and this object
// always precedes libretrocrt.a on the link line.  cmake/static_modules.py
// fails the build unless the linker map shows each of them coming from this
// object.  libretro's other syscalls (_read_r, _write_r, _close_r,
// _lseek_r...) still come from libretrocrt.a, so file descriptors keep its
// encoding: refNum + kMacRefNumOffset.  newlib's stdio implements O_APPEND
// itself by seeking to the end before each write.
//
// Paths are passed to the File Manager as they are, like libretro does: the
// engine already builds HFS paths (full, or partial from the default
// directory).  A path with a component longer than 31 bytes is refused with
// ENAMETOOLONG first.  No name this API reaches is longer (HFS's limit,
// kHFSMaxFileNameChars; HFS Plus shows it longer names mangled to 31), so
// such a component cannot name what the caller meant, and whether the File
// Manager rejects it (bdNamErr) or truncates it to another name is not
// known on the target (issue #327).  A truncated name could create a file
// with another extension than the one FS_CheckFilenameIsMutable passed
// ("<27 bytes>.qvm.pk3.tmp" as "<27 bytes>.qvm"), or make a rename's two
// names one file.

#include <reent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <Files.h>
#include <Errors.h>
#include <StringCompare.h>

extern const int kMacRefNumOffset;	// libretro/syscalls.c

static int Mac_OSErrToErrno( OSErr err ) {
	switch ( err ) {
	case fnfErr:
	case dirNFErr:
	case nsvErr:
		return ENOENT;
	case dupFNErr:
		return EEXIST;
	case wPrErr:
	case vLckdErr:
		return EROFS;
	case permErr:
	case fLckdErr:
	case opWrErr:
	case afpAccessDenied:
		return EACCES;
	case fBsyErr:
		return EBUSY;
	case tmfoErr:
		return EMFILE;
	case dskFulErr:
		return ENOSPC;
	case diffVolErr:
		return EXDEV;
	case notAFileErr:
		return EISDIR;
	case bdNamErr:
	case badMovErr:
		return EINVAL;
	default:
		return EIO;
	}
}

// the C path as a Str255, or ENAMETOOLONG (libretro's length byte wrapped)
// for a path longer than 255 bytes or with a component longer than 31
static int Mac_PathToStr255( struct _reent *reent, const char *name, Str255 pname ) {
	const char	*s;
	size_t		length, component;

	length = strlen( name );
	for ( component = 0, s = name ; *s && component <= 31 ; s++ ) {
		component = *s == ':' ? 0 : component + 1;
	}
	if ( length > 255 || component > 31 ) {
		reent->_errno = ENAMETOOLONG;
		return 0;
	}
	pname[0] = (unsigned char)length;
	memcpy( pname + 1, name, length );
	return 1;
}

int _open_r( struct _reent *reent, const char *name, int flags, int mode ) {
	Str255	pname;
	SInt8	permission;
	short	ref;
	OSErr	err;

	(void)mode;

	if ( !Mac_PathToStr255( reent, name, pname ) ) {
		return -1;
	}

	switch ( flags & O_ACCMODE ) {
	case O_RDONLY:
		permission = fsRdPerm;
		break;
	case O_WRONLY:
		permission = fsWrPerm;
		break;
	case O_RDWR:
		permission = fsRdWrPerm;
		break;
	default:
		reent->_errno = EINVAL;
		return -1;
	}

	if ( flags & O_CREAT ) {
		// the type and creator ('????') libretro gives new files
		err = HCreate( 0, 0, pname, 0x3F3F3F3F, 'TEXT' );
		if ( err != noErr && ( err != dupFNErr || ( flags & O_EXCL ) ) ) {
			reent->_errno = Mac_OSErrToErrno( err );
			return -1;
		}
	}

	err = HOpenDF( 0, 0, pname, permission, &ref );
	if ( err == paramErr ) {
		// file systems without HOpenDF
		err = HOpen( 0, 0, pname, permission, &ref );
	}
	if ( err != noErr ) {
		reent->_errno = Mac_OSErrToErrno( err );
		return -1;
	}

	if ( ( flags & O_TRUNC ) && permission != fsRdPerm ) {
		err = SetEOF( ref, 0 );
		if ( err != noErr ) {
			FSClose( ref );
			reent->_errno = Mac_OSErrToErrno( err );
			return -1;
		}
	}

	return ref + kMacRefNumOffset;
}

// remove() comes here too.  HDelete also deletes an empty directory, as
// remove() should; a busy file or a directory that is not empty is EBUSY.
int _unlink_r( struct _reent *reent, const char *name ) {
	Str255	pname;
	OSErr	err;

	if ( !Mac_PathToStr255( reent, name, pname ) ) {
		return -1;
	}
	err = HDelete( 0, 0, pname );
	if ( err != noErr ) {
		reent->_errno = Mac_OSErrToErrno( err );
		return -1;
	}
	return 0;
}

static int Mac_SameName( ConstStr255Param a, ConstStr255Param b ) {
	return a[0] == b[0] && !memcmp( a + 1, b + 1, a[0] );
}

// Move the entry source names to dest, which does not exist.  HRename only
// renames within a directory and CatMove only moves (keeping the name), so
// a rename across directories is a CatMove and then an HRename, undone if
// the HRename fails: the old entry stays, or the new one is complete.
// CatMove takes a nil newName as "the directory newDirID" (MoreFiles'
// HMoveRename relies on that too).  Because the move keeps the name, it
// fails (dupFNErr, EEXIST) when dest's directory already has an entry
// under the source's leaf, where POSIX would succeed; nothing changes.  No
// caller renames across directories (FS_SV_Rename's one caller renames
// .pk3.tmp to .pk3 in place), so this does not move under a temporary
// name first.
static OSErr Mac_MoveRename( const FSSpec *source, const FSSpec *dest ) {
	OSErr	err;

	if ( source->parID == dest->parID ) {
		return FSpRename( source, dest->name );
	}
	err = CatMove( source->vRefNum, source->parID, source->name, dest->parID, NULL );
	if ( err != noErr || Mac_SameName( source->name, dest->name ) ) {
		return err;
	}
	err = HRename( dest->vRefNum, dest->parID, source->name, dest->name );
	if ( err != noErr ) {
		CatMove( dest->vRefNum, dest->parID, source->name, source->parID, NULL );
	}
	return err;
}

// The catalog ID of the entry spec names.  PBGetCatInfo returns a file's
// number, or a directory's ID, in ioDirID (MoreFiles' DirectoryCopy.c
// relies on this); files and directories share that numbering on a volume.
static OSErr Mac_CatalogID( const FSSpec *spec, long *id ) {
	CInfoPBRec	pb;
	OSErr		err;

	memset( &pb, 0, sizeof( pb ) );
	pb.hFileInfo.ioNamePtr = (StringPtr)spec->name;
	pb.hFileInfo.ioVRefNum = spec->vRefNum;
	pb.hFileInfo.ioFDirIndex = 0;	// look up ioNamePtr in ioDirID
	pb.hFileInfo.ioDirID = spec->parID;
	err = PBGetCatInfoSync( &pb );
	*id = pb.hFileInfo.ioDirID;
	return err;
}

// POSIX rename: an existing dest is replaced.  FSpExchangeFiles swaps the
// two files' contents in the catalog in one call, so dest names either its
// old or its new contents at every moment; deleting the source, which now
// holds dest's old contents, completes the rename, and if that delete fails
// (an open or locked file) the contents are swapped back.  Volumes without
// FSpExchangeFiles (paramErr, wrgVolTypErr: some foreign file systems)
// fall back to deleting dest and then moving the source: between the two
// dest does not exist, and if the move fails it is gone (there an empty
// directory dest is deleted like a file).  The name, Finder info and lock
// of dest stay with dest.  Otherwise a rename onto a directory, or of a
// directory onto an existing entry, fails (notAFileErr, EISDIR).  Renaming
// across volumes is EXDEV, as POSIX has it.
//
// Two names of one entry are found by its catalog ID, not by comparing the
// names: a file system that folds two names EqualString tells apart (one
// that truncates them) would otherwise have the file exchanged with itself
// and then deleted.  As in POSIX, renaming an entry onto itself does
// nothing, except that a change of case within its directory is made.
int _rename_r( struct _reent *reent, const char *from, const char *to ) {
	Str255	pfrom, pto;
	FSSpec	source, dest;
	long	sourceID, destID;
	OSErr	err;

	if ( !Mac_PathToStr255( reent, from, pfrom ) || !Mac_PathToStr255( reent, to, pto ) ) {
		return -1;
	}

	err = FSMakeFSSpec( 0, 0, pfrom, &source );
	if ( err == noErr ) {
		err = FSMakeFSSpec( 0, 0, pto, &dest );
		if ( err == noErr || err == fnfErr ) {
			if ( source.vRefNum != dest.vRefNum ) {
				err = diffVolErr;
			} else if ( err == fnfErr ) {
				err = Mac_MoveRename( &source, &dest );
			} else if ( ( err = Mac_CatalogID( &source, &sourceID ) ) == noErr &&
				( err = Mac_CatalogID( &dest, &destID ) ) == noErr && sourceID == destID ) {
				if ( source.parID == dest.parID && !Mac_SameName( source.name, dest.name ) &&
					EqualString( source.name, dest.name, false, true ) ) {
					err = FSpRename( &source, dest.name );
				}
			} else if ( err == noErr ) {
				err = FSpExchangeFiles( &source, &dest );
				if ( err == noErr ) {
					err = FSpDelete( &source );
					if ( err != noErr ) {
						FSpExchangeFiles( &source, &dest );
					}
				} else if ( err == paramErr || err == wrgVolTypErr ) {
					err = FSpDelete( &dest );
					if ( err == noErr ) {
						err = Mac_MoveRename( &source, &dest );
					}
				}
			}
		}
	}
	if ( err != noErr ) {
		reent->_errno = Mac_OSErrToErrno( err );
		return -1;
	}
	return 0;
}
