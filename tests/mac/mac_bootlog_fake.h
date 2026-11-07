/* A fake File Manager for tests/mac_bootlog_regression.c, stored in place of
 * <Files.h>, <Devices.h> and <Errors.h>.  Types and constants are those of
 * Universal Interfaces 3.4, cut to what code/mac/mac_bootlog.c uses.
 *
 * It keeps what makes logging on HFS hard.  Each file has a cached copy and a
 * copy on disk, and the disk copy has its own length, standing for the
 * catalog entry.  A write changes only the cache.  PBFlushFileSync sends the
 * bytes inside the catalog length to disk, and the catalog length itself
 * changes only at FlushVol or FSClose.  FakeFM_HardReset then drops everything
 * that was not sent: what is left is what a reset leaves.  A write with
 * noCacheMask set is treated like any other, since that bit is only a hint. */
#ifndef MAC_BOOTLOG_FAKE_H
#define MAC_BOOTLOG_FAKE_H

#include <stdint.h>
#include <string.h>

typedef int16_t			OSErr;
typedef int8_t			SInt8;
typedef uint32_t		OSType;
typedef unsigned char	Str255[256];
typedef const unsigned char *ConstStr255Param;

enum { noErr = 0, dupFNErr = -48, fnfErr = -43, wPrErr = -44, dskFulErr = -34, ioErr = -36, paramErr = -50 };
enum { fsRdWrPerm = 3, fsFromStart = 1 };

typedef struct {
	short	ioRefNum;
	char	*ioBuffer;
	long	ioReqCount;
	short	ioPosMode;
	long	ioPosOffset;
} IOParam;

typedef union {
	IOParam	ioParam;
} ParamBlockRec;

#define FAKE_FILE_CAPACITY	( 4 * 1024 * 1024 )

typedef struct {
	char	name[256];
	char	cache[FAKE_FILE_CAPACITY];
	long	length;
	char	disk[FAKE_FILE_CAPACITY];
	long	catalogLength;
	OSType	type;
	int		exists;
} FakeFile;

extern FakeFile	fakeFiles[3];
extern int		fakeLocked, fakeDiskFullAt, fakeWriteError, fakeFlushError, fakeSyncs, fakeHOpenDFMissing;
extern OSErr	fakeRenameError;	/* fBsyErr-like, for a log open in an editor */
extern void		(*fakeWriteHook)( void );	/* runs inside each positioned write */

OSErr HCreate( short v, long d, ConstStr255Param name, OSType creator, OSType type );
OSErr HDelete( short v, long d, ConstStr255Param name );
OSErr HRename( short v, long d, ConstStr255Param oldName, ConstStr255Param newName );
OSErr HOpenDF( short v, long d, ConstStr255Param name, SInt8 permission, short *ref );
OSErr HOpen( short v, long d, ConstStr255Param name, SInt8 permission, short *ref );
OSErr SetEOF( short ref, long length );
OSErr FSWrite( short ref, long *count, const void *buffer );
OSErr GetVRefNum( short ref, short *volume );
OSErr FlushVol( const unsigned char *name, short volume );
OSErr FSClose( short ref );
OSErr PBWriteSync( ParamBlockRec *pb );
OSErr PBFlushFileSync( ParamBlockRec *pb );

void	FakeFM_Reset( void );
void	FakeFM_HardReset( void );

#endif
