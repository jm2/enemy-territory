/* Issues #3, #34, #15 and #16: just enough of Universal Interfaces 3.4
 * (MacTypes.h, Quickdraw.h, QDOffscreen.h, MacWindows.h, Devices.h,
 * Video.h, MacMemory.h, DrawSprocket.h) and of Apple's classic agl.h and
 * glm.h for tests/mac_glimp_regression.c to compile the real
 * code/mac/mac_glimp.c and code/mac/MacGamma.c on the host.
 * run_mac_glimp_tests.sh installs this file under each of those header
 * names.  Only names, field order and types are mirrored, not the mac68k
 * packing; GLint is long, as in Apple's classic gl.h.  Every function is
 * the test's fake. */
#ifndef MAC_GLIMP_FAKE_H
#define MAC_GLIMP_FAKE_H

#include <stddef.h>

#ifndef true
#define true	1
#define false	0
#endif
#ifndef nil
#define nil		NULL
#endif

/* ---- MacTypes.h ---- */

typedef char				*Ptr;
typedef unsigned char		Boolean;
typedef short				OSErr;
typedef long				OSStatus;
typedef unsigned char		Str255[256];
typedef const unsigned char	*ConstStr255Param;
typedef unsigned char		*StringPtr;

enum {
	noErr			= 0,
	memFullErr		= -108
};

/* ---- MacMemory.h ---- */

Ptr		NewPtr( long byteCount );
Ptr		NewPtrClear( long byteCount );
void	DisposePtr( Ptr p );
void	BlockMove( const void *srcPtr, void *destPtr, long byteCount );

/* ---- Quickdraw.h / QDOffscreen.h / MacWindows.h ---- */

typedef struct Rect {
	short	top, left, bottom, right;
} Rect;

typedef struct RGBColor {
	unsigned short	red, green, blue;
} RGBColor;

typedef struct ColorSpec {
	short		value;
	RGBColor	rgb;
} ColorSpec;

typedef struct ColorTable {
	long		ctSeed;
	short		ctFlags;
	short		ctSize;
	ColorSpec	ctTable[1];
} ColorTable, *CTabPtr, **CTabHandle;

typedef struct PixMap {
	short		pixelSize;
	CTabHandle	pmTable;
} PixMap, *PixMapPtr, **PixMapHandle;

typedef struct GDevice	GDevice, *GDPtr, **GDHandle;
struct GDevice {
	short			gdRefNum;
	PixMapHandle	gdPMap;
	GDHandle		gdNextGD;
};

/* windows, ports and GWorlds are all one fake type, as the code casts
 * freely between them */
typedef struct FakeWindow	GrafPort, CGrafPort;
typedef GrafPort			*GrafPtr, *CGrafPtr, *WindowPtr, *WindowRef, *GWorldPtr;

GDHandle	GetDeviceList( void );
GDHandle	GetNextDevice( GDHandle curDevice );
GDHandle	GetGDevice( void );
GDHandle	GetGWorldDevice( GWorldPtr offscreenGWorld );
void		GetGWorld( CGrafPtr *port, GDHandle *gdh );
void		SetGWorld( CGrafPtr port, GDHandle gdh );
void		SetPort( GrafPtr port );

enum {
	blackColor	= 33,
	whiteColor	= 30
};

void		ForeColor( long color );
void		BackColor( long color );
void		SetRect( Rect *r, short left, short top, short right, short bottom );
void		EraseRect( const Rect *r );
void		MoveTo( short h, short v );
void		DrawText( const void *textBuf, short firstByte, short byteCount );

/* ---- Events.h ---- */

enum {
	keyDownMask		= 0x0008,
	autoKeyMask		= 0x0020,
	charCodeMask	= 0x000000FF
};

typedef struct EventRecord {
	short			what;
	unsigned long	message;
} EventRecord;

void			FlushEvents( short whichMask, short stopMask );
Boolean			WaitNextEvent( short eventMask, EventRecord *theEvent, unsigned long sleep, void *mouseRgn );
unsigned long	TickCount( void );

WindowPtr	NewCWindow( void *wStorage, const Rect *boundsRect, ConstStr255Param title,
				Boolean visible, short procID, WindowPtr behind, Boolean goAwayFlag, long refCon );
void		DisposeWindow( WindowPtr window );
void		SizeWindow( WindowPtr window, short w, short h, Boolean fUpdate );
void		MoveWindow( WindowPtr window, short hGlobal, short vGlobal, Boolean front );
void		ShowWindow( WindowPtr window );
void		HiliteWindow( WindowPtr window, Boolean fHilite );
void		SelectWindow( WindowPtr window );

/* ---- Devices.h / Video.h ---- */

typedef struct CntrlParam {
	void			*ioCompletion;
	OSErr			ioResult;
	StringPtr		ioNamePtr;
	short			ioVRefNum;
	short			ioCRefNum;
	short			csCode;
	/* aligned so the code's *(Ptr *)csParam store is a valid host store */
	short			csParam[11] __attribute__(( aligned( sizeof( void * ) ) ));
} CntrlParam;

typedef CntrlParam		*ParmBlkPtr;

OSErr	PBStatus( ParmBlkPtr paramBlock, Boolean async );
OSErr	Control( short refNum, short csCode, const void *csParamPtr );

enum {
	cscSetEntries	= 3,
	cscSetGamma		= 4,
	cscGetGamma		= 8
};

typedef struct GammaTbl {
	short	gVersion;
	short	gType;
	short	gFormulaSize;
	short	gChanCnt;
	short	gDataCnt;
	short	gDataWidth;
	short	gFormulaData[1];
} GammaTbl, *GammaTblPtr;

typedef struct VDGammaRecord {
	Ptr		csGTable;
} VDGammaRecord;

typedef struct VDSetEntryRecord {
	ColorSpec	*csTable;
	short		csStart;
	short		csCount;
} VDSetEntryRecord;

/* ---- DrawSprocket.h ---- */

typedef struct OpaqueDSpContextReference	*DSpContextReference;
typedef unsigned long						DSpContextState;
typedef unsigned long						DSpDepthMask;
typedef unsigned long						DSpColorNeeds;

enum {
	kDSpContextState_Active		= 0,
	kDSpContextState_Paused		= 1,
	kDSpContextState_Inactive	= 2
};

enum {
	kDSpDepthMask_16	= 1 << 4,
	kDSpDepthMask_32	= 1 << 5
};

enum {
	kDSpColorNeeds_Require	= 2
};

typedef struct DSpContextAttributes {
	unsigned long	frequency;
	unsigned long	displayWidth;
	unsigned long	displayHeight;
	unsigned long	reserved1;
	unsigned long	reserved2;
	DSpColorNeeds	colorNeeds;
	CTabHandle		colorTable;
	unsigned long	contextOptions;
	DSpDepthMask	backBufferDepthMask;
	DSpDepthMask	displayDepthMask;
	unsigned long	backBufferBestDepth;
	unsigned long	displayBestDepth;
	unsigned long	pageCount;
	char			filler[3];
	Boolean			gameMustConfirmSwitch;
	unsigned long	reserved3[4];
} DSpContextAttributes;

typedef unsigned long	DisplayIDType;

OSStatus	DSpStartup( void );
OSStatus	DSpShutdown( void );
OSStatus	DSpFindBestContext( DSpContextAttributes *inDesiredAttributes, DSpContextReference *outContext );
OSStatus	DSpGetFirstContext( DisplayIDType inDisplayID, DSpContextReference *outContext );
OSStatus	DSpGetNextContext( DSpContextReference inCurrentContext, DSpContextReference *outContext );
OSStatus	DSpContext_GetDisplayID( DSpContextReference inContext, DisplayIDType *outDisplayID );
OSStatus	DSpContext_GetFrontBuffer( DSpContextReference inContext, CGrafPtr *outFrontBuffer );
OSStatus	DSpContext_Reserve( DSpContextReference inContext, DSpContextAttributes *inDesiredAttributes );
OSStatus	DSpContext_Release( DSpContextReference inContext );
OSStatus	DSpContext_GetAttributes( DSpContextReference inContext, DSpContextAttributes *outAttributes );
OSStatus	DSpContext_SetState( DSpContextReference inContext, DSpContextState inState );
OSStatus	DSpContext_FadeGammaIn( const void *inZeroIntensityColor, void *inCallback );

/* ---- gl.h / agl.h / glm.h ---- */

typedef unsigned long	GLenum;
typedef unsigned char	GLboolean;
typedef long			GLint;
typedef int	GLsizei;
typedef unsigned long	GLbitfield;
typedef float			GLclampf;
typedef float			GLfloat;
typedef unsigned char	GLubyte;

#define GL_FALSE					0
#define GL_TRUE						1
#define GL_COLOR_BUFFER_BIT			0x00004000
#define GL_VENDOR					0x1F00
#define GL_RENDERER					0x1F01
#define GL_VERSION					0x1F02
#define GL_EXTENSIONS				0x1F03
#define GL_FASTEST					0x1101
#define GL_TRANSFORM_HINT_APPLE		0x85B1

void	glHint( GLenum target, GLenum mode );
void	glMultiTexCoord2fARB( GLenum texture, GLfloat s, GLfloat t );
void	glActiveTextureARB( GLenum texture );
void	glClientActiveTextureARB( GLenum texture );
void	glLockArraysEXT( GLint first, GLsizei count );
void	glUnlockArraysEXT( void );
void	qglGetIntegerv( GLenum name, GLint *value );
#define GL_MAX_ACTIVE_TEXTURES_ARB	0x84E2

typedef struct FakeAGLPixelFormat	*AGLPixelFormat;
typedef struct FakeAGLContext		*AGLContext;
typedef struct FakeAGLRendererInfo	*AGLRendererInfo;
typedef GDHandle					AGLDevice;
typedef CGrafPtr					AGLDrawable;

#define AGL_NONE				0
#define AGL_BUFFER_SIZE			2
#define AGL_RGBA				4
#define AGL_DOUBLEBUFFER		5
#define AGL_RED_SIZE			8
#define AGL_GREEN_SIZE			9
#define AGL_BLUE_SIZE			10
#define AGL_ALPHA_SIZE			11
#define AGL_DEPTH_SIZE			12
#define AGL_STENCIL_SIZE		13
#define AGL_PIXEL_SIZE			50
#define AGL_RENDERER_ID			70
#define AGL_NO_RECOVERY			72
#define AGL_ACCELERATED			73
#define AGL_SWAP_INTERVAL		222
#define AGL_RASTERIZATION		221
#define AGL_VIDEO_MEMORY		120
#define AGL_TEXTURE_MEMORY		121
#define AGL_NO_ERROR			0
#define AGL_BAD_ALLOC			10016

#define GLM_PAGE_SIZE			0x0001
#define GLM_NUMBER_PAGES		0x0002
#define GLM_CURRENT_MEMORY		0x0003
#define GLM_MAXIMUM_MEMORY		0x0004

void			aglGetVersion( GLint *major, GLint *minor );
AGLPixelFormat	aglChoosePixelFormat( const AGLDevice *gdevs, GLint ndev, const GLint *attribs );
void			aglDestroyPixelFormat( AGLPixelFormat pix );
GLboolean		aglDescribePixelFormat( AGLPixelFormat pix, GLint attrib, GLint *value );
AGLContext		aglCreateContext( AGLPixelFormat pix, AGLContext share );
GLboolean		aglDestroyContext( AGLContext ctx );
GLboolean		aglSetCurrentContext( AGLContext ctx );
GLboolean		aglSetDrawable( AGLContext ctx, AGLDrawable draw );
void			aglSwapBuffers( AGLContext ctx );
GLboolean		aglEnable( AGLContext ctx, GLenum pname );
GLboolean		aglDisable( AGLContext ctx, GLenum pname );
GLboolean		aglSetInteger( AGLContext ctx, GLenum pname, const GLint *params );
GLenum			aglGetError( void );
const GLubyte	*aglErrorString( GLenum code );
AGLRendererInfo	aglQueryRendererInfo( const AGLDevice *gdevs, GLint ndev );
void			aglDestroyRendererInfo( AGLRendererInfo rend );
AGLRendererInfo	aglNextRendererInfo( AGLRendererInfo rend );
GLboolean		aglDescribeRenderer( AGLRendererInfo rend, GLint prop, GLint *value );

#endif /* MAC_GLIMP_FAKE_H */
