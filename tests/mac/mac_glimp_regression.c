/* Issues #3, #34, #15, #16, #6 and #7: the classic Mac renderer's GLimp_Init and
 * GLimp_Shutdown.
 *
 * #3: GLimp_Extensions read r_ext_texture_filter_anisotropic->integer from
 * a cvar nothing registered, so every driver that advertised
 * GL_EXT_texture_filter_anisotropic crashed GL startup.
 *
 * #34: GLimp_Init's "registered" guard was never set, so every vid_restart
 * added aglDescribe and aglState again; they are now added by each
 * GLimp_Init and removed by GLimp_Shutdown, since both use the context.
 *
 * #15: GLimp_ChangeDisplay ignored every DrawSprocket result after
 * DSpStartup, and GLimp_SetMode returned on failure holding whatever window,
 * pixel format, context and display reservation it had made, so GLimp_Init's
 * fallback mode was set up over them; GLimp_Shutdown destroyed the context
 * while it was still current.
 *
 * #16: GetSystemGammas (MacGamma.c) returned a half-built snapshot when an
 * allocation failed, which GLimp_Shutdown then walked, and GLimp_Init
 * claimed hardware gamma whether or not the desktop gamma could be put back.
 *
 * #6: GLimp_Init forced r_fullscreen 0, and its fallback no longer set
 * r_fullscreen 1 as retail did, so a windowed attempt that failed (accelerated
 * GL on an 8-bit desktop) was fatal instead of recovering fullscreen at
 * 640x480x16.
 *
 * #7: r_colorbits 16, the fallback's, asked AGL for 8/8/8 color, so the
 * fallback failed again on a 16-bit-only accelerator.
 *
 * The fixture compiles the real mac_glimp.c (its #includes blanked by the
 * runner) and MacGamma.c against tests/mac_glimp_fake.h.  The fake AGL,
 * DrawSprocket and Window Manager count every live object and can fail any
 * one call, or every call of one kind; contexts, pixel formats and windows
 * are heap blocks freed on disposal, so ASan catches use after release.
 * The fake Memory Manager can fail the Nth allocation and fills NewPtr's
 * memory with garbage, as the real one does not clear it. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/game/q_shared.h"
#include "et_compat.h"
#include "../../src/renderer/tr_public.h"
#include <Quickdraw.h>

static int			failures;
static const char	*currentCase = "";

static void Check( int ok, const char *message ) {
	if ( !ok ) {
		fprintf( stderr, "FAIL: %s: %s\n", currentCase, message );
		failures++;
	}
}

/* ---- the renderer and engine around mac_glimp.c ---- */

glconfig_t	glConfig;

static cvar_t	*CvarGet( const char *name, const char *value, int flags );
static void		CvarSet( const char *name, const char *value );

cvar_t	*r_mode, *r_fullscreen, *r_colorbits, *r_stencilbits, *r_depthbits, *r_displayRefresh;
cvar_t	*r_swapInterval, *r_ext_multitexture, *r_ext_compiled_vertex_array;
cvar_t	*r_ext_texture_env_add, *r_allowExtensions, *r_ext_compressed_textures;

void	*qglMultiTexCoord2fARB, *qglActiveTextureARB, *qglClientActiveTextureARB;
void	*qglLockArraysEXT, *qglUnlockArraysEXT;

static const char	*glExtensions = "";
static int			glClears;

const GLubyte *qglGetString( GLenum name ) {
	switch ( name ) {
	case GL_VENDOR:		return (const GLubyte *)"Fake";
	case GL_RENDERER:	return (const GLubyte *)"Fake Rage 128";
	case GL_VERSION:	return (const GLubyte *)"1.1";
	default:			return (const GLubyte *)glExtensions;
	}
}

void qglClearColor( GLclampf r, GLclampf g, GLclampf b, GLclampf a ) {
}

void qglClear( GLbitfield mask ) {
	glClears++;
}

void glHint( GLenum target, GLenum mode ) {
}

/* the five entry points retail called, which become PEF imports */
void glMultiTexCoord2fARB( GLenum texture, GLfloat s, GLfloat t ) {
}

void glActiveTextureARB( GLenum texture ) {
}

void glClientActiveTextureARB( GLenum texture ) {
}

void glLockArraysEXT( GLint first, GLsizei count ) {
}

void glUnlockArraysEXT( void ) {
}

static GLint	glTextureUnits;

void qglGetIntegerv( GLenum name, GLint *value ) {
	Check( name == GL_MAX_ACTIVE_TEXTURES_ARB, "only the texture units are asked for" );
	*value = glTextureUnits;
}

qboolean R_GetModeInfo( int *width, int *height, float *windowAspect, int mode ) {
	static const int	modes[][2] = {
		{ 320, 240 }, { 400, 300 }, { 512, 384 }, { 640, 480 }, { 800, 600 }, { 1024, 768 }
	};

	if ( mode < 0 || mode >= (int)( sizeof( modes ) / sizeof( modes[0] ) ) ) {
		return qfalse;
	}
	*width = modes[mode][0];
	*height = modes[mode][1];
	*windowAspect = (float)*width / *height;
	return qtrue;
}

void Sys_LogPrintf( const char *fmt, ... ) {
}

void QDECL Com_FlightRecord( const char *fmt, ... ) {
}

void Debug_Breadcrumb( int color ) {
}

void Sys_SendKeyEvents( void ) {
}

void QDECL Com_Error( int level, const char *fmt, ... ) {
	fprintf( stderr, "Com_Error in %s\n", currentCase );
	abort();
}

void QDECL Com_Printf( const char *fmt, ... ) {
}

/* ri */

static int		verbose;
static jmp_buf	errorJump;
static int		errorJumpSet;
static int		fatalErrors;
static char		lastError[1024];

static void QDECL RiPrintf( int level, const char *fmt, ... ) {
	va_list	argptr;

	if ( verbose ) {
		va_start( argptr, fmt );
		vprintf( fmt, argptr );
		va_end( argptr );
	}
}

static void QDECL RiError( int level, const char *fmt, ... ) {
	va_list	argptr;

	va_start( argptr, fmt );
	vsnprintf( lastError, sizeof( lastError ), fmt, argptr );
	va_end( argptr );
	fatalErrors++;
	if ( !errorJumpSet ) {
		fprintf( stderr, "FAIL: %s: unexpected ri.Error: %s\n", currentCase, lastError );
		abort();
	}
	longjmp( errorJump, 1 );
}

#define	MAX_FAKE_CVARS	64

typedef struct {
	cvar_t	cvar;
	char	name[64];
	char	string[64];
} fakeCvar_t;

static fakeCvar_t	cvars[MAX_FAKE_CVARS];
static int			numCvars;

static fakeCvar_t *CvarFind( const char *name ) {
	int	i;

	for ( i = 0 ; i < numCvars ; i++ ) {
		if ( !Q_stricmp( cvars[i].name, name ) ) {
			return &cvars[i];
		}
	}
	return NULL;
}

static void CvarStore( fakeCvar_t *v, const char *value ) {
	Q_strncpyz( v->string, value, sizeof( v->string ) );
	v->cvar.string = v->string;
	v->cvar.value = atof( value );
	v->cvar.integer = atoi( value );
	v->cvar.modified = qtrue;
}

static cvar_t *CvarGet( const char *name, const char *value, int flags ) {
	fakeCvar_t	*v;

	v = CvarFind( name );
	if ( !v ) {
		if ( numCvars == MAX_FAKE_CVARS ) {
			abort();
		}
		v = &cvars[numCvars++];
		Q_strncpyz( v->name, name, sizeof( v->name ) );
		v->cvar.name = v->name;
		CvarStore( v, value );
	}
	v->cvar.flags |= flags;
	return &v->cvar;
}

static void CvarSet( const char *name, const char *value ) {
	fakeCvar_t	*v;

	v = CvarFind( name );
	if ( !v ) {
		CvarGet( name, value, 0 );
		return;
	}
	CvarStore( v, value );
}

#define	MAX_FAKE_COMMANDS	8

static struct {
	char	name[32];
	void	(*function)( void );
} commands[MAX_FAKE_COMMANDS];
static int	numCommands;

static int CommandIndex( const char *name ) {
	int	i;

	for ( i = 0 ; i < numCommands ; i++ ) {
		if ( !Q_stricmp( commands[i].name, name ) ) {
			return i;
		}
	}
	return -1;
}

static void CmdAddCommand( const char *name, void (*function)( void ) ) {
	/* Cmd_AddCommand prints "already defined" and keeps the old one */
	if ( CommandIndex( name ) >= 0 ) {
		Check( 0, "a command is added while it is already defined" );
		return;
	}
	if ( numCommands == MAX_FAKE_COMMANDS ) {
		abort();
	}
	Q_strncpyz( commands[numCommands].name, name, sizeof( commands[0].name ) );
	commands[numCommands].function = function;
	numCommands++;
}

static void CmdRemoveCommand( const char *name ) {
	int	i;

	i = CommandIndex( name );
	if ( i < 0 ) {
		return;
	}
	commands[i] = commands[--numCommands];
}

static int		cmdArgc;
static char		*cmdArgv[4];

static int CmdArgc( void ) {
	return cmdArgc;
}

static char *CmdArgv( int i ) {
	return i < cmdArgc ? cmdArgv[i] : "";
}

refimport_t	ri;

/* ---- fault injection ---- */

static const char	*failName;	/* the call to fail */
static int			failNth;	/* its Nth call fails; 0 fails all */
static int			failCount;
static int			failed;

static int Fail( const char *name ) {
	if ( !failName || strcmp( name, failName ) ) {
		return 0;
	}
	failCount++;
	if ( failNth && failCount != failNth ) {
		return 0;
	}
	failed++;
	return 1;
}

/* ---- fake Memory Manager ---- */

static int	ptrsLive;
static int	ptrAllocs;
static int	failPtrAt;	/* the ptrAllocs count that fails; 0 none */

static Ptr AllocPtr( long byteCount, int clear ) {
	Ptr	p;

	ptrAllocs++;
	if ( failPtrAt && ptrAllocs == failPtrAt ) {
		return NULL;
	}
	p = malloc( byteCount );
	/* the real NewPtr leaves whatever was in the heap */
	memset( p, clear ? 0 : 0x5A, byteCount );
	ptrsLive++;
	return p;
}

Ptr NewPtr( long byteCount ) {
	return AllocPtr( byteCount, 0 );
}

Ptr NewPtrClear( long byteCount ) {
	return AllocPtr( byteCount, 1 );
}

void DisposePtr( Ptr p ) {
	Check( p != NULL, "DisposePtr gets a pointer" );
	if ( p ) {
		free( p );
		ptrsLive--;
	}
}

void BlockMove( const void *srcPtr, void *destPtr, long byteCount ) {
	memmove( destPtr, srcPtr, byteCount );
}

/* ---- fake displays and their video drivers ---- */

#define	NUM_DEVICES		2
#define	GAMMA_SIZE		( 3 * 256 )

typedef struct {
	GDevice			gd;
	GDevice			*gdPtr;
	PixMap			pixMap;
	PixMap			*pixMapPtr;
	int				noGamma;		/* driver has no cscGetGamma */
	GammaTbl		*gamma;			/* the driver's live table */
	unsigned char	original[GAMMA_SIZE];
} fakeDevice_t;

static fakeDevice_t	devices[NUM_DEVICES];
static int			gammaWrites;

static unsigned char *GammaData( GammaTbl *table ) {
	return (unsigned char *)&table->gFormulaData + table->gFormulaSize;
}

static void ResetDevices( void ) {
	int	i, j;

	for ( i = 0 ; i < NUM_DEVICES ; i++ ) {
		fakeDevice_t	*d = &devices[i];

		free( d->gamma );
		memset( d, 0, sizeof( *d ) );
		d->gdPtr = &d->gd;
		d->pixMapPtr = &d->pixMap;
		d->pixMap.pixelSize = 32;
		d->gd.gdRefNum = -50 - i;
		d->gd.gdPMap = &d->pixMapPtr;
		d->gd.gdNextGD = i + 1 < NUM_DEVICES ? &devices[i + 1].gdPtr : NULL;
		d->gamma = calloc( 1, sizeof( GammaTbl ) + GAMMA_SIZE );
		d->gamma->gChanCnt = 3;
		d->gamma->gDataCnt = 256;
		d->gamma->gDataWidth = 8;
		for ( j = 0 ; j < GAMMA_SIZE ; j++ ) {
			d->original[j] = (unsigned char)( ( j & 255 ) ^ ( i * 0x33 ) );
		}
		memcpy( GammaData( d->gamma ), d->original, GAMMA_SIZE );
	}
	gammaWrites = 0;
}

static void FreeDevices( void ) {
	int	i;

	for ( i = 0 ; i < NUM_DEVICES ; i++ ) {
		free( devices[i].gamma );
		devices[i].gamma = NULL;
	}
}

static fakeDevice_t *DeviceForRefNum( short refNum ) {
	int	i;

	for ( i = 0 ; i < NUM_DEVICES ; i++ ) {
		if ( devices[i].gd.gdRefNum == refNum ) {
			return &devices[i];
		}
	}
	return NULL;
}

static int GammaIsOriginal( int i ) {
	return !memcmp( GammaData( devices[i].gamma ), devices[i].original, GAMMA_SIZE );
}

GDHandle GetDeviceList( void ) {
	return &devices[0].gdPtr;
}

GDHandle GetNextDevice( GDHandle curDevice ) {
	return (**curDevice).gdNextGD;
}

GDHandle GetGDevice( void ) {
	return &devices[0].gdPtr;
}

GDHandle GetGWorldDevice( GWorldPtr offscreenGWorld ) {
	return &devices[0].gdPtr;
}

void GetGWorld( CGrafPtr *port, GDHandle *gdh ) {
	*port = NULL;
	*gdh = &devices[0].gdPtr;
}

void SetGWorld( CGrafPtr port, GDHandle gdh ) {
}

OSErr PBStatus( ParmBlkPtr paramBlock, Boolean async ) {
	fakeDevice_t	*d = DeviceForRefNum( paramBlock->ioCRefNum );
	VDGammaRecord	*rec = *(VDGammaRecord **)paramBlock->csParam;

	Check( d != NULL, "PBStatus names a display driver" );
	if ( !d || paramBlock->csCode != cscGetGamma || d->noGamma ) {
		return -18;		/* statusErr */
	}
	rec->csGTable = (Ptr)d->gamma;
	return noErr;
}

OSErr Control( short refNum, short csCode, const void *csParamPtr ) {
	fakeDevice_t	*d = DeviceForRefNum( refNum );
	VDGammaRecord	*rec = *(VDGammaRecord * const *)csParamPtr;
	GammaTbl		*table;

	Check( d != NULL, "Control names a display driver" );
	if ( !d || csCode != cscSetGamma || d->noGamma ) {
		return -17;		/* controlErr */
	}
	gammaWrites++;
	table = (GammaTbl *)rec->csGTable;
	if ( !table ) {
		int	j;

		for ( j = 0 ; j < GAMMA_SIZE ; j++ ) {
			GammaData( d->gamma )[j] = (unsigned char)j;
		}
		return noErr;
	}
	Check( table->gChanCnt == 3 && table->gDataCnt == 256 && table->gDataWidth == 8,
		"cscSetGamma gets a 3x256x8 table" );
	memcpy( GammaData( d->gamma ), GammaData( table ), GAMMA_SIZE );
	return noErr;
}

/* ---- fake Window Manager ---- */

struct FakeWindow {
	int		attached;	/* contexts drawing into it */
};

static int	windowsLive;

void SetPort( GrafPtr port ) {
}

WindowPtr NewCWindow( void *wStorage, const Rect *boundsRect, ConstStr255Param title,
		Boolean visible, short procID, WindowPtr behind, Boolean goAwayFlag, long refCon ) {
	if ( Fail( "NewCWindow" ) ) {
		return NULL;
	}
	windowsLive++;
	return calloc( 1, sizeof( struct FakeWindow ) );
}

void DisposeWindow( WindowPtr window ) {
	Check( window->attached == 0, "no context draws into a disposed window" );
	windowsLive--;
	free( window );
}

void SizeWindow( WindowPtr window, short w, short h, Boolean fUpdate ) {
	window->attached += 0;
}

void MoveWindow( WindowPtr window, short hGlobal, short vGlobal, Boolean front ) {
	window->attached += 0;
}

void ShowWindow( WindowPtr window ) {
	window->attached += 0;
}

void HiliteWindow( WindowPtr window, Boolean fHilite ) {
	window->attached += 0;
}

void SelectWindow( WindowPtr window ) {
	window->attached += 0;
}

/* ---- fake DrawSprocket ---- */

struct OpaqueDSpContextReference {
	int				reserved;		/* of the display, kept in dspDisplay */
	int				active;
	unsigned long	width, height;
	unsigned long	frequency;
	unsigned long	depthMask;
	int				mustConfirm;
};

/* the context DSpFindBestContext gives, and the rest of what the display lists */
static struct OpaqueDSpContextReference	dspDisplay;
static struct OpaqueDSpContextReference	dspModes[8];
static int								dspModeCount;
static DSpContextReference				dspHeld;	/* the reserved context */
static unsigned long					dspHeldFrequency;	/* as asked of DSpContext_Reserve */
static int								dspStarted;
static int								dspConfirmSwitch;
static int								dspNoDisplayID;
static int								dspNoFrontBuffer;
static DSpDepthMask						dspDepth;	/* of the reserved display */

OSStatus DSpStartup( void ) {
	if ( Fail( "DSpStartup" ) ) {
		return -30440;	/* kDSpSystemSWTooOldErr */
	}
	Check( dspStarted == 0, "DSpStartup is not nested" );
	dspStarted++;
	return noErr;
}

OSStatus DSpShutdown( void ) {
	Check( dspStarted == 1, "DSpShutdown follows DSpStartup" );
	Check( !dspDisplay.reserved, "DSpShutdown leaves no display reserved" );
	dspStarted--;
	return noErr;
}

OSStatus DSpFindBestContext( DSpContextAttributes *inDesiredAttributes, DSpContextReference *outContext ) {
	Check( dspStarted == 1, "DrawSprocket is started" );
	if ( Fail( "DSpFindBestContext" ) ) {
		return -30441;	/* kDSpNotInitializedErr */
	}
	*outContext = &dspDisplay;
	return noErr;
}

static int DspKnows( DSpContextReference context ) {
	return context == &dspDisplay || ( context >= dspModes && context < dspModes + dspModeCount );
}

OSStatus DSpContext_GetDisplayID( DSpContextReference inContext, DisplayIDType *outDisplayID ) {
	Check( DspKnows( inContext ), "DSpContext_GetDisplayID gets a context" );
	*outDisplayID = 1;
	return dspNoDisplayID ? -30442 : noErr;
}

OSStatus DSpGetFirstContext( DisplayIDType inDisplayID, DSpContextReference *outContext ) {
	Check( inDisplayID == 1, "DSpGetFirstContext gets the display" );
	*outContext = dspModes;
	return dspModeCount ? noErr : -30446;	/* kDSpContextNotFoundErr */
}

OSStatus DSpGetNextContext( DSpContextReference inCurrentContext, DSpContextReference *outContext ) {
	Check( DspKnows( inCurrentContext ), "DSpGetNextContext gets a context" );
	if ( inCurrentContext + 1 == dspModes + dspModeCount ) {
		return -30446;
	}
	*outContext = inCurrentContext + 1;
	return noErr;
}

OSStatus DSpContext_GetFrontBuffer( DSpContextReference inContext, CGrafPtr *outFrontBuffer ) {
	static struct FakeWindow	front;

	Check( inContext == dspHeld && dspDisplay.active, "the front buffer is of the active context" );
	*outFrontBuffer = &front;
	return dspNoFrontBuffer ? -30442 : noErr;
}

OSStatus DSpContext_Reserve( DSpContextReference inContext, DSpContextAttributes *inDesiredAttributes ) {
	Check( DspKnows( inContext ), "DSpContext_Reserve gets a context" );
	if ( Fail( "DSpContext_Reserve" ) ) {
		return -30442;
	}
	Check( !dspDisplay.reserved, "a display is reserved once" );
	Check( inDesiredAttributes->displayDepthMask == inDesiredAttributes->backBufferDepthMask,
		"the display and back buffer depths agree" );
	Check( inDesiredAttributes->frequency == inContext->frequency, "the refresh is the context's" );
	dspDisplay.reserved = 1;
	dspHeld = inContext;
	dspHeldFrequency = inDesiredAttributes->frequency;
	dspDepth = inDesiredAttributes->displayDepthMask;
	return noErr;
}

OSStatus DSpContext_Release( DSpContextReference inContext ) {
	Check( inContext == dspHeld && dspDisplay.reserved, "DSpContext_Release gets a reserved context" );
	Check( !dspDisplay.active, "a context is made inactive before release" );
	dspDisplay.reserved = 0;
	dspHeld = NULL;
	return noErr;
}

OSStatus DSpContext_GetAttributes( DSpContextReference inContext, DSpContextAttributes *outAttributes ) {
	Check( DspKnows( inContext ), "DSpContext_GetAttributes gets a context" );
	if ( Fail( "DSpContext_GetAttributes" ) ) {
		return -30443;
	}
	/* a bigger display than asked for, as DrawSprocket may pick */
	outAttributes->displayWidth = inContext->width;
	outAttributes->displayHeight = inContext->height;
	outAttributes->frequency = inContext->frequency;
	outAttributes->displayDepthMask = inContext->depthMask;
	outAttributes->gameMustConfirmSwitch = inContext->mustConfirm;
	return noErr;
}

OSStatus DSpContext_SetState( DSpContextReference inContext, DSpContextState inState ) {
	Check( inContext == dspHeld && dspDisplay.reserved, "DSpContext_SetState gets a reserved context" );
	if ( inState == kDSpContextState_Active && Fail( "DSpContext_SetState" ) ) {
		return -30444;
	}
	dspDisplay.active = inState == kDSpContextState_Active;
	/* the switch is made, and the game is told to confirm it can be seen */
	return dspDisplay.active && ( dspConfirmSwitch || inContext->mustConfirm ) ? -30448 : noErr;
}

OSStatus DSpContext_FadeGammaIn( const void *inZeroIntensityColor, void *inCallback ) {
	return noErr;
}

/* ---- fake Quickdraw text and the event queue ---- */

static int				promptLines;
static const char		*keysQueued;	/* before the prompt showed */
static const char		*keysTyped;		/* after it */
static unsigned long	ticks;

void ForeColor( long color ) {
}

void BackColor( long color ) {
}

void SetRect( Rect *r, short left, short top, short right, short bottom ) {
	r->left = left;
	r->top = top;
	r->right = right;
	r->bottom = bottom;
}

void EraseRect( const Rect *r ) {
	Check( r->right == glConfig.vidWidth && r->bottom == glConfig.vidHeight, "the whole screen is cleared" );
}

void MoveTo( short h, short v ) {
	Check( h >= 0 && h < glConfig.vidWidth && v >= 0 && v < glConfig.vidHeight, "the prompt is on the screen" );
}

void DrawText( const void *textBuf, short firstByte, short byteCount ) {
	Check( firstByte == 0 && byteCount > 0 && byteCount == (short)strlen( textBuf ), "a whole line is drawn" );
	promptLines++;
}

void FlushEvents( short whichMask, short stopMask ) {
	Check( whichMask & keyDownMask, "keys pressed earlier are dropped" );
	keysQueued = "";
}

Boolean WaitNextEvent( short eventMask, EventRecord *theEvent, unsigned long sleep, void *mouseRgn ) {
	const char	**keys;

	Check( ( eventMask & keyDownMask ) && !mouseRgn, "only keys are waited for" );
	ticks += sleep;
	keys = *keysQueued ? &keysQueued : &keysTyped;
	if ( !**keys ) {
		return 0;
	}
	theEvent->message = 0x1200 | (unsigned char)*(*keys)++;
	return 1;
}

unsigned long TickCount( void ) {
	return ticks;
}

/* ---- fake AGL ---- */

struct FakeAGLPixelFormat {
	int		magic;
	GLint	red;
};

struct FakeAGLContext {
	AGLDrawable	drawable;
	long		enabled;
};

struct FakeAGLRendererInfo {
	int		magic;
};

static int			fmtsLive, contextsLive;
static AGLContext	currentContext;
static GLenum		aglError;
static int			swaps;
static int			aglEnables;

/* the attributes of the last aglChoosePixelFormat */
static GLint		pfRGBA, pfRed, pfGreen, pfBlue, pfDepth, pfStencil;
static int			pfChosen;
/* an accelerator that has only 16-bit (5/5/5) formats, and one that
 * can't draw into a window on the desktop (an 8-bit one, say) */
static int			only16Bit, noWindowedGL, noStencil;

void aglGetVersion( GLint *major, GLint *minor ) {
	*major = 2;
	*minor = 1;
}

AGLPixelFormat aglChoosePixelFormat( const AGLDevice *gdevs, GLint ndev, const GLint *attribs ) {
	AGLPixelFormat	pix;
	int				i;

	pfRGBA = 0;
	pfRed = pfGreen = pfBlue = pfDepth = pfStencil = -1;
	for ( i = 0 ; attribs[i] != AGL_NONE ; i++ ) {
		switch ( attribs[i] ) {
		case AGL_RGBA:			pfRGBA = 1; break;
		case AGL_DOUBLEBUFFER:
		case AGL_NO_RECOVERY:
		case AGL_ACCELERATED:	break;
		case AGL_RED_SIZE:		pfRed = attribs[++i]; break;
		case AGL_GREEN_SIZE:	pfGreen = attribs[++i]; break;
		case AGL_BLUE_SIZE:		pfBlue = attribs[++i]; break;
		case AGL_ALPHA_SIZE:	++i; break;
		case AGL_DEPTH_SIZE:	pfDepth = attribs[++i]; break;
		case AGL_STENCIL_SIZE:	pfStencil = attribs[++i]; break;
		default:
			Check( 0, "aglChoosePixelFormat gets known attributes" );
			break;
		}
	}
	pfChosen++;
	if ( Fail( "aglChoosePixelFormat" )
		|| ( only16Bit && pfRed > 5 ) || ( noWindowedGL && !dspDisplay.active )
		|| ( noStencil && pfStencil > 0 ) ) {
		aglError = AGL_BAD_ALLOC;
		return NULL;
	}
	fmtsLive++;
	pix = calloc( 1, sizeof( struct FakeAGLPixelFormat ) );
	pix->red = pfRed;
	return pix;
}

void aglDestroyPixelFormat( AGLPixelFormat pix ) {
	fmtsLive--;
	free( pix );
}

GLboolean aglDescribePixelFormat( AGLPixelFormat pix, GLint attrib, GLint *value ) {
	Check( pix != NULL, "aglDescribePixelFormat gets a pixel format" );
	pix->magic += 0;
	switch ( attrib ) {
	case AGL_RED_SIZE:		*value = pix->red; break;
	case AGL_DEPTH_SIZE:	*value = 16; break;
	default:				*value = 0; break;
	}
	return GL_TRUE;
}

AGLContext aglCreateContext( AGLPixelFormat pix, AGLContext share ) {
	pix->magic += 0;
	if ( Fail( "aglCreateContext" ) ) {
		aglError = AGL_BAD_ALLOC;
		return NULL;
	}
	contextsLive++;
	return calloc( 1, sizeof( struct FakeAGLContext ) );
}

GLboolean aglDestroyContext( AGLContext ctx ) {
	Check( ctx != currentContext, "the current context is detached before it is destroyed" );
	if ( ctx == currentContext ) {
		currentContext = NULL;
	}
	Check( ctx->drawable == NULL, "the drawable is detached before the context is destroyed" );
	if ( ctx->drawable ) {
		ctx->drawable->attached--;
	}
	contextsLive--;
	free( ctx );
	return GL_TRUE;
}

GLboolean aglSetCurrentContext( AGLContext ctx ) {
	if ( ctx ) {
		ctx->enabled += 0;
		if ( Fail( "aglSetCurrentContext" ) ) {
			aglError = AGL_BAD_ALLOC;
			return GL_FALSE;
		}
	}
	currentContext = ctx;
	return GL_TRUE;
}

GLboolean aglSetDrawable( AGLContext ctx, AGLDrawable draw ) {
	if ( draw && Fail( "aglSetDrawable" ) ) {
		aglError = AGL_BAD_ALLOC;
		return GL_FALSE;
	}
	if ( ctx->drawable ) {
		ctx->drawable->attached--;
	}
	ctx->drawable = draw;
	if ( draw ) {
		draw->attached++;
	}
	return GL_TRUE;
}

void aglSwapBuffers( AGLContext ctx ) {
	Check( ctx != NULL && ctx == currentContext, "aglSwapBuffers gets the current context" );
	swaps++;
}

GLboolean aglEnable( AGLContext ctx, GLenum pname ) {
	Check( ctx != NULL && ctx->drawable != NULL, "aglEnable gets a live context" );
	ctx->enabled |= 1;
	aglEnables++;
	return GL_TRUE;
}

GLboolean aglDisable( AGLContext ctx, GLenum pname ) {
	Check( ctx != NULL && ctx->drawable != NULL, "aglDisable gets a live context" );
	ctx->enabled &= ~1;
	aglEnables++;
	return GL_TRUE;
}

GLboolean aglSetInteger( AGLContext ctx, GLenum pname, const GLint *params ) {
	Check( ctx != NULL, "aglSetInteger gets a context" );
	ctx->enabled += 0;
	return GL_TRUE;
}

/* like glGetError, AGL keeps a failed call's error until it is read */
GLenum aglGetError( void ) {
	GLenum	err = aglError;

	aglError = AGL_NO_ERROR;
	return err;
}

const GLubyte *aglErrorString( GLenum code ) {
	return (const GLubyte *)"fake AGL error";
}

static struct FakeAGLRendererInfo	rendererInfo;

AGLRendererInfo aglQueryRendererInfo( const AGLDevice *gdevs, GLint ndev ) {
	return &rendererInfo;
}

void aglDestroyRendererInfo( AGLRendererInfo rend ) {
}

AGLRendererInfo aglNextRendererInfo( AGLRendererInfo rend ) {
	return NULL;
}

GLboolean aglDescribeRenderer( AGLRendererInfo rend, GLint prop, GLint *value ) {
	switch ( prop ) {
	case AGL_ACCELERATED:		*value = 1; break;
	case AGL_TEXTURE_MEMORY:	*value = 16 * 1024 * 1024; break;
	case AGL_VIDEO_MEMORY:		*value = 32 * 1024 * 1024; break;
	default:					*value = 0; break;
	}
	return GL_TRUE;
}

/* ---- the code under test ---- */

#include "../../src/mac9/MacGamma.h"
#include "mac_glimp_extracted.c"
#include "../../src/mac9/MacGamma.c"


/* ---- helpers ---- */

static void ResetAll( void ) {
	failName = NULL;
	glTextureUnits = 2;
	dspConfirmSwitch = 0;
	dspNoDisplayID = 0;
	dspNoFrontBuffer = 0;
	dspModeCount = 0;
	dspHeld = NULL;
	dspDisplay.width = 1024;
	dspDisplay.height = 768;
	dspDisplay.frequency = 0;
	dspDisplay.depthMask = kDSpDepthMask_16 | kDSpDepthMask_32;
	dspDisplay.mustConfirm = 0;
	promptLines = 0;
	keysQueued = "";
	keysTyped = "";
	ticks = 0;
	failNth = 0;
	failCount = 0;
	failed = 0;
	failPtrAt = 0;
	ptrAllocs = 0;
	fatalErrors = 0;
	aglError = AGL_NO_ERROR;
	only16Bit = 0;
	noStencil = 0;
	noWindowedGL = 0;
	pfChosen = 0;
	glExtensions = "GL_ARB_multitexture GL_EXT_texture_env_add";
	CvarSet( "r_mode", "4" );
	CvarSet( "r_fullscreen", "0" );
	CvarSet( "r_colorbits", "32" );
	CvarSet( "r_depthbits", "16" );
	CvarSet( "r_stencilbits", "0" );
	CvarSet( "r_displayRefresh", "0" );
	CvarSet( "r_displayConfirm", "0" );
	CvarSet( "r_displayConfirmed", "" );
	ResetDevices();
}

static void Setup( void ) {
	ri.Printf = RiPrintf;
	ri.Error = RiError;
	ri.Cvar_Get = CvarGet;
	ri.Cvar_Set = CvarSet;
	ri.Cmd_AddCommand = CmdAddCommand;
	ri.Cmd_RemoveCommand = CmdRemoveCommand;
	ri.Cmd_Argc = CmdArgc;
	ri.Cmd_Argv = CmdArgv;

	r_mode = CvarGet( "r_mode", "4", 0 );
	r_fullscreen = CvarGet( "r_fullscreen", "0", 0 );
	r_colorbits = CvarGet( "r_colorbits", "32", 0 );
	r_stencilbits = CvarGet( "r_stencilbits", "0", 0 );
	r_depthbits = CvarGet( "r_depthbits", "16", 0 );
	r_displayRefresh = CvarGet( "r_displayRefresh", "0", 0 );
	r_swapInterval = CvarGet( "r_swapInterval", "0", 0 );
	r_ext_multitexture = CvarGet( "r_ext_multitexture", "1", 0 );
	r_ext_compiled_vertex_array = CvarGet( "r_ext_compiled_vertex_array", "1", 0 );
	r_ext_texture_env_add = CvarGet( "r_ext_texture_env_add", "1", 0 );
	r_allowExtensions = CvarGet( "r_allowExtensions", "1", 0 );
	r_ext_compressed_textures = CvarGet( "r_ext_compressed_textures", "1", 0 );
	verbose = getenv( "Q3_TEST_VERBOSE" ) != NULL;
}

/* runs GLimp_Init, returning 0 if it made a fatal error */
static int Init( void ) {
	errorJumpSet = 1;
	if ( setjmp( errorJump ) ) {
		errorJumpSet = 0;
		return 0;
	}
	GLimp_Init();
	errorJumpSet = 0;
	return 1;
}

/* exactly one usable window, context and pixel format, and the display
 * reserved and active exactly when glConfig says fullscreen */
static void CheckLive( void ) {
	Check( windowsLive == 1, "one window is live" );
	Check( contextsLive == 1, "one context is live" );
	Check( fmtsLive == 1, "one pixel format is live" );
	Check( sys_gl.context != NULL && currentContext == sys_gl.context, "the context is current" );
	Check( sys_gl.context != NULL && sys_gl.context->drawable == sys_gl.drawable,
		"the context draws into the window" );
	Check( glConfig.vidWidth > 0 && glConfig.vidHeight > 0, "glConfig has the mode" );
	if ( glConfig.isFullscreen ) {
		Check( dspStarted == 1 && dspDisplay.reserved && dspDisplay.active,
			"a fullscreen renderer holds an active display" );
		Check( sys_gl.DSpContext != NULL && sys_gl.DSpContext == dspHeld, "sys_gl has the display" );
	} else {
		Check( dspStarted == 0 && !dspDisplay.reserved && !dspDisplay.active,
			"a windowed renderer holds no display" );
	}
}

static void CheckReleased( void ) {
	Check( windowsLive == 0, "no window is left" );
	Check( contextsLive == 0, "no context is left" );
	Check( fmtsLive == 0, "no pixel format is left" );
	Check( currentContext == NULL, "no context is current" );
	Check( dspStarted == 0 && !dspDisplay.reserved && !dspDisplay.active,
		"DrawSprocket is shut down" );
	Check( sys_gl.context == NULL && sys_gl.fmt == NULL && sys_gl.drawable == NULL
		&& sys_gl.DSpContext == NULL, "sys_gl holds nothing" );
	Check( sys_gl.systemGammas == NULL, "the gamma snapshot is disposed" );
	Check( CommandIndex( "aglDescribe" ) < 0 && CommandIndex( "aglState" ) < 0,
		"the AGL commands are removed with the context" );
	Check( ptrsLive == 0, "every Memory Manager block is disposed" );
	Check( !glConfig.isFullscreen, "glConfig is not fullscreen" );
}

/* ---- the cases ---- */

/* #3 */
static void TestAnisotropic( void ) {
	cvar_t	*avail;

	currentCase = "anisotropic-present";
	ResetAll();
	glExtensions = "GL_ARB_multitexture GL_EXT_texture_filter_anisotropic";
	Check( Init(), "GLimp_Init succeeds with GL_EXT_texture_filter_anisotropic" );
	CheckLive();
	Check( r_ext_texture_filter_anisotropic != NULL
		&& CvarFind( "r_ext_texture_filter_anisotropic" ) != NULL,
		"r_ext_texture_filter_anisotropic is registered" );
	if ( r_ext_texture_filter_anisotropic ) {
		/* on for the Radeon; r_textureAnisotropy stays 1, so it costs nothing until raised */
		Check( !strcmp( r_ext_texture_filter_anisotropic->string, "1" ),
			"r_ext_texture_filter_anisotropic defaults to 1" );
		Check( ( r_ext_texture_filter_anisotropic->flags & ( CVAR_LATCH | CVAR_ARCHIVE ) )
			== ( CVAR_LATCH | CVAR_ARCHIVE ), "r_ext_texture_filter_anisotropic is latched and archived" );
	}
	avail = CvarGet( "r_ext_texture_filter_anisotropic_avail", "", 0 );
	Check( avail->integer == 1, "r_ext_texture_filter_anisotropic_avail is 1" );
	/* ET: glConfig.anisotropicAvailable means in use, so it follows the cvar */
	Check( glConfig.anisotropicAvailable, "anisotropy is used by default" );
	GLimp_Shutdown();
	CheckReleased();

	currentCase = "anisotropic-present-off";
	ResetAll();
	CvarSet( "r_ext_texture_filter_anisotropic", "0" );
	glExtensions = "GL_ARB_multitexture GL_EXT_texture_filter_anisotropic";
	Check( Init(), "GLimp_Init succeeds with the cvar off" );
	CheckLive();
	Check( !glConfig.anisotropicAvailable, "anisotropy is off while the cvar is 0" );
	GLimp_Shutdown();
	CheckReleased();
	CvarSet( "r_ext_texture_filter_anisotropic", "1" );

	currentCase = "anisotropic-extensions-off";
	ResetAll();
	CvarSet( "r_allowExtensions", "0" );
	glExtensions = "GL_ARB_multitexture GL_EXT_texture_filter_anisotropic";
	Check( Init(), "GLimp_Init succeeds with r_allowExtensions 0" );
	CheckLive();
	Check( !glConfig.anisotropicAvailable, "r_allowExtensions 0 turns anisotropy off" );
	GLimp_Shutdown();
	CheckReleased();
	CvarSet( "r_allowExtensions", "1" );

	currentCase = "anisotropic-present-on";
	ResetAll();
	CvarSet( "r_ext_texture_filter_anisotropic", "1" );
	glExtensions = "GL_ARB_multitexture GL_EXT_texture_filter_anisotropic";
	Check( Init(), "GLimp_Init succeeds with the cvar on" );
	CheckLive();
	Check( glConfig.anisotropicAvailable, "anisotropy is used with the cvar on" );
	GLimp_Shutdown();
	CheckReleased();
	Check( !glConfig.anisotropicAvailable, "shutdown clears it" );
	CvarSet( "r_ext_texture_filter_anisotropic", "0" );

	currentCase = "anisotropic-absent";
	ResetAll();
	CvarSet( "r_ext_texture_filter_anisotropic", "1" );
	Check( Init(), "GLimp_Init succeeds without GL_EXT_texture_filter_anisotropic" );
	CheckLive();
	avail = CvarGet( "r_ext_texture_filter_anisotropic_avail", "", 0 );
	Check( avail->integer == 0, "r_ext_texture_filter_anisotropic_avail is 0" );
	Check( !glConfig.anisotropicAvailable, "no anisotropy without the extension" );
	GLimp_Shutdown();
	CheckReleased();
	CvarSet( "r_ext_texture_filter_anisotropic", "0" );
}

/* S3TC is taken as the Windows renderer takes it */
static void S3tcCase( const char *name, const char *extensions, const char *cvar, const char *allow, int want ) {
	currentCase = name;
	ResetAll();
	CvarSet( "r_ext_compressed_textures", cvar );
	CvarSet( "r_allowExtensions", allow );
	glExtensions = extensions;
	Check( Init(), "GLimp_Init succeeds" );
	CheckLive();
	Check( glConfig.textureCompression == ( want ? TC_EXT_COMP_S3TC : TC_NONE ), "texture compression is as expected" );
	GLimp_Shutdown();
	CheckReleased();
	CvarSet( "r_ext_compressed_textures", "1" );
	CvarSet( "r_allowExtensions", "1" );
}

static void TestS3tc( void ) {
	const char *with = "GL_ARB_multitexture GL_EXT_texture_compression_s3tc";

	S3tcCase( "s3tc-present", with, "1", "1", 1 );
	S3tcCase( "s3tc-cvar-off", with, "0", "1", 0 );
	S3tcCase( "s3tc-extensions-off", with, "1", "0", 0 );
	S3tcCase( "s3tc-absent", "GL_ARB_multitexture GL_S3_s3tc", "1", "1", 0 );
}

/* #34, and repeated vid_restart for #15 and #16 */
static void TestRestartCycles( void ) {
	int		i, index;
	char	arg0[] = "aglState", arg1[] = "rasterization", arg2[] = "0";

	currentCase = "vid_restart";
	ResetAll();
	for ( i = 0 ; i < 40 ; i++ ) {
		CvarSet( "r_fullscreen", ( i & 1 ) ? "1" : "0" );
		CvarSet( "r_mode", ( i & 2 ) ? "3" : "5" );
		Check( Init(), "GLimp_Init succeeds" );
		CheckLive();
		Check( glConfig.isFullscreen == ( ( i & 1 ) != 0 ), "the mode is fullscreen when asked" );
		Check( sys_gl.systemGammas != NULL && glConfig.deviceSupportsGamma, "the gamma snapshot is taken" );
		Check( CommandIndex( "aglDescribe" ) >= 0, "aglDescribe is registered" );
		index = CommandIndex( "aglState" );
		Check( index >= 0, "aglState is registered" );
		if ( index >= 0 ) {
			int		before = aglEnables;

			cmdArgc = 3;
			cmdArgv[0] = arg0;
			cmdArgv[1] = arg1;
			cmdArgv[2] = arg2;
			commands[index].function();
			Check( aglEnables == before + 1, "aglState reaches the live context" );
			cmdArgc = 0;
		}
		GLimp_Shutdown();
		CheckReleased();
		Check( GammaIsOriginal( 0 ) && GammaIsOriginal( 1 ), "the desktop gamma is put back" );
	}
	Check( fatalErrors == 0, "no cycle is fatal" );
}

/* #15: an unusable mode falls back to mode 3 */
static void TestBadMode( void ) {
	currentCase = "bad-mode";
	ResetAll();
	CvarSet( "r_mode", "99" );
	Check( Init(), "GLimp_Init falls back" );
	CheckLive();
	Check( r_mode->integer == 3 && glConfig.vidWidth == 640, "the fallback is mode 3" );
	GLimp_Shutdown();
	CheckReleased();
}

/* kDSpConfirmSwitchWarning from a real Radeon 9200, whose switch left a CRT out
 * of range: the mode stays only if the player says it can be seen */
#define	CONFIRM_TICKS	( 15 * 60 )

static void AddMode( int hz, int mustConfirm ) {
	struct OpaqueDSpContextReference	*mode = &dspModes[dspModeCount++];

	memset( mode, 0, sizeof( *mode ) );
	mode->width = 1024;
	mode->height = 768;
	mode->frequency = (unsigned long)hz << 16;
	mode->depthMask = kDSpDepthMask_16 | kDSpDepthMask_32;
	mode->mustConfirm = mustConfirm;
}

/* starts ResetAll's 1024x768x32 fullscreen, which is mode 5 in this fixture */
static void ResetFullscreen( const char *name ) {
	currentCase = name;
	ResetAll();
	CvarSet( "r_fullscreen", "1" );
	CvarSet( "r_mode", "5" );
	CvarSet( "r_colorbits", "32" );
}

static void CheckKept( void ) {
	Check( Init(), "GLimp_Init succeeds" );
	CheckLive();
	Check( glConfig.isFullscreen && glConfig.vidWidth == 1024, "the mode is fullscreen 1024x768" );
	Check( r_mode->integer == 5 && r_colorbits->integer == 32, "the settings asked for are kept" );
	Check( fatalErrors == 0, "nothing is fatal" );
}

static void CheckWentBack( void ) {
	Check( Init(), "GLimp_Init survives a refused mode" );
	CheckLive();
	Check( glConfig.isFullscreen && r_mode->integer == 3 && r_colorbits->integer == 16,
		"the display goes back to the fallback, which the config then keeps" );
	Check( !CvarFind( "r_displayConfirmed" )->cvar.string[0], "the refused mode is not remembered" );
	Check( fatalErrors == 0, "nothing is fatal" );
	Check( promptLines == 2, "only the refused mode was asked about, not the fallback" );
}

static void TestConfirmSwitch( void ) {
	static const struct { const char *name, *keys; int soon; } refused[] = {
		{ "confirm-no-answer", "", 0 },
		{ "confirm-blind-keys", " \r\tq", 0 },
		{ "confirm-n", "n", 1 },
		{ "confirm-escape", "\033", 1 }
	};
	int		i;

	/* yes */
	ResetFullscreen( "confirm-yes" );
	dspConfirmSwitch = 1;
	keysTyped = "y";
	CheckKept();
	Check( promptLines == 2 && ticks < CONFIRM_TICKS, "the prompt was shown and answered" );
	Check( !strcmp( CvarFind( "r_displayConfirmed" )->cvar.string, "1024x768x32@0" ), "the answer is remembered" );
	GLimp_Shutdown();
	CheckReleased();

	/* and isn't asked again, which a player away from the keys would fail */
	promptLines = 0;
	keysTyped = "";
	ticks = 0;
	CheckKept();
	Check( promptLines == 0 && ticks == 0, "a confirmed mode is not asked about again" );
	GLimp_Shutdown();
	CheckReleased();

	/* another refresh is another mode */
	ResetFullscreen( "confirm-other-refresh" );
	dspConfirmSwitch = 1;
	dspDisplay.frequency = 85 << 16;
	CvarSet( "r_displayConfirmed", "1024x768x32@100" );
	keysTyped = "Y";
	CheckKept();
	Check( promptLines == 2, "a different refresh is asked about" );
	Check( !strcmp( CvarFind( "r_displayConfirmed" )->cvar.string, "1024x768x32@85" ), "the new answer is remembered" );
	GLimp_Shutdown();
	CheckReleased();

	/* anything but yes puts the display back; the screen may be blank */
	for ( i = 0 ; i < (int)( sizeof( refused ) / sizeof( refused[0] ) ) ; i++ ) {
		ResetFullscreen( refused[i].name );
		dspConfirmSwitch = 1;
		keysTyped = refused[i].keys;
		CheckWentBack();
		Check( refused[i].soon ? ticks < CONFIRM_TICKS : ticks >= CONFIRM_TICKS,
			"no answer waits, and no goes back at once" );
		Check( ticks <= CONFIRM_TICKS + 6, "the wait is over in time" );
		GLimp_Shutdown();
		CheckReleased();
	}

	/* a key pressed before the prompt, as the menu's own, is no answer */
	ResetFullscreen( "confirm-stale-key" );
	dspConfirmSwitch = 1;
	keysQueued = "y";
	CheckWentBack();
	GLimp_Shutdown();
	CheckReleased();

	/* a prompt that can't be drawn still can't keep a mode on its own */
	ResetFullscreen( "confirm-no-front-buffer" );
	dspConfirmSwitch = 1;
	dspNoFrontBuffer = 1;
	Check( Init(), "GLimp_Init survives" );
	CheckLive();
	Check( r_mode->integer == 3 && promptLines == 0, "the display goes back undrawn" );
	GLimp_Shutdown();
	CheckReleased();

	/* the flag alone asks, and so does r_displayConfirm */
	ResetFullscreen( "confirm-flag" );
	dspDisplay.mustConfirm = 1;
	keysTyped = "y";
	dspConfirmSwitch = 0;
	CheckKept();
	Check( promptLines == 2, "a flagged mode is asked about" );
	GLimp_Shutdown();
	CheckReleased();

	ResetFullscreen( "confirm-forced" );
	CvarSet( "r_displayConfirm", "1" );
	keysTyped = "y";
	CheckKept();
	Check( promptLines == 2, "r_displayConfirm asks about any mode" );
	GLimp_Shutdown();
	CheckReleased();

	/* the lookup after DSpContext_Reserve fails: found first, then each mode
	 * listed, so failing the picker's lookups is not this */
	for ( i = 0 ; i < 2 ; i++ ) {
		ResetFullscreen( i ? "attributes-after-reserve-listed" : "attributes-after-reserve" );
		if ( i ) {
			AddMode( 75, 0 );
			AddMode( 85, 0 );
		}
		failName = "DSpContext_GetAttributes";
		failNth = 2 + dspModeCount;
		Check( Init(), "GLimp_Init survives" );
		CheckLive();
		Check( failed == 1 && r_mode->integer == 3, "the reserved context is released and the mode falls back" );
		failName = NULL;
		GLimp_Shutdown();
		CheckReleased();
	}

	ResetFullscreen( "confirm-none-needed" );
	CheckKept();
	Check( promptLines == 0 && ticks == 0, "a trusted mode is not asked about" );
	GLimp_Shutdown();
	CheckReleased();
}

/* DrawSprocket ignores the refresh asked for, and chose one a CRT couldn't show */
static void CheckRefresh( const char *name, int hz, int prompts ) {
	currentCase = name;
	keysTyped = "y";
	CheckKept();
	Check( glConfig.displayFrequency == hz, "the refresh is chosen and reported" );
	Check( (int)( dspHeldFrequency < 0x10000 ? dspHeldFrequency : dspHeldFrequency >> 16 ) == hz,
		"that refresh is the one reserved" );
	Check( promptLines == prompts, "only an untrusted refresh is asked about" );
	GLimp_Shutdown();
	CheckReleased();
}

/* r_stencilbits defaults to 8 and a Radeon refuses it: the mode the player
 * just confirmed must not go to the fallback over that */
static void TestStencil( void ) {
	ResetFullscreen( "stencil-refused" );
	noStencil = 1;
	CvarSet( "r_stencilbits", "8" );
	CheckKept();
	Check( pfChosen == 2 && pfStencil == 0, "the format is asked for again without stencil" );
	Check( r_stencilbits->integer == 8, "the cvar is left alone" );
	GLimp_Shutdown();
	CheckReleased();

	ResetFullscreen( "stencil-offered" );
	CvarSet( "r_stencilbits", "8" );
	CheckKept();
	Check( pfChosen == 1 && pfStencil == 8, "stencil is used where it is offered" );
	GLimp_Shutdown();
	CheckReleased();

	/* with nothing to drop, a failure still reaches the fallback */
	ResetFullscreen( "stencil-none-to-drop" );
	noStencil = 1;
	only16Bit = 1;
	CvarSet( "r_stencilbits", "0" );
	Check( Init(), "GLimp_Init survives" );
	CheckLive();
	Check( pfChosen == 2 && r_mode->integer == 3, "the mode falls back as before" );
	GLimp_Shutdown();
	CheckReleased();
}

static void TestRefresh( void ) {
	ResetFullscreen( "refresh-fastest-trusted" );
	AddMode( 60, 0 ); AddMode( 85, 0 ); AddMode( 75, 0 ); AddMode( 120, 0 );
	CheckRefresh( "refresh-fastest-trusted", 85, 0 );

	ResetFullscreen( "refresh-only-above-limit" );
	AddMode( 120, 0 ); AddMode( 100, 0 );
	CheckRefresh( "refresh-only-above-limit", 100, 0 );

	ResetFullscreen( "refresh-trusted-beats-faster" );
	AddMode( 120, 1 ); AddMode( 100, 1 ); AddMode( 70, 0 );
	CheckRefresh( "refresh-trusted-beats-faster", 70, 0 );

	/* the Radeon 9200's CRT: the slowest is the likeliest to be in range */
	ResetFullscreen( "refresh-slowest-untrusted" );
	AddMode( 120, 1 ); AddMode( 85, 1 ); AddMode( 60, 1 ); AddMode( 75, 1 );
	dspDisplay.frequency = 120 << 16;	/* DSpFindBestContext's own choice */
	dspDisplay.mustConfirm = 1;
	CheckRefresh( "refresh-slowest-untrusted", 60, 2 );

	ResetFullscreen( "refresh-asked-for" );
	AddMode( 120, 1 ); AddMode( 85, 1 ); AddMode( 60, 0 );
	CvarSet( "r_displayRefresh", "85" );
	CheckRefresh( "refresh-asked-for", 85, 2 );

	/* a refresh the monitor can't show is refused, and the fallback, which
	 * can't ask, must not use it too */
	ResetFullscreen( "refresh-asked-for-refused" );
	AddMode( 120, 1 ); AddMode( 60, 1 );
	dspModes[0].width = dspModes[1].width = 640;
	dspModes[0].height = dspModes[1].height = 480;
	AddMode( 120, 1 ); AddMode( 60, 1 );
	CvarSet( "r_displayRefresh", "120" );
	dspDisplay.frequency = 120 << 16;
	dspDisplay.mustConfirm = 1;
	Check( Init(), "GLimp_Init survives a refused refresh" );
	CheckLive();
	Check( r_mode->integer == 3 && promptLines == 2, "only the refresh asked for was asked about" );
	Check( glConfig.displayFrequency == 60, "the fallback ignores r_displayRefresh" );
	GLimp_Shutdown();
	CheckReleased();

	ResetFullscreen( "refresh-not-offered" );
	AddMode( 60, 0 ); AddMode( 75, 0 );
	CvarSet( "r_displayRefresh", "85" );
	CheckRefresh( "refresh-not-offered", 75, 0 );

	/* a refresh of 0 is unknown, and isn't what r_displayRefresh 0 asks for */
	ResetFullscreen( "refresh-unknown" );
	AddMode( 75, 0 ); AddMode( 0, 0 );
	CheckRefresh( "refresh-unknown", 75, 0 );

	ResetFullscreen( "refresh-plain-hz" );
	AddMode( 60, 0 );
	dspModes[0].frequency = 72;
	CheckRefresh( "refresh-plain-hz", 72, 0 );

	/* a mode of another size is never used */
	ResetFullscreen( "refresh-other-size" );
	AddMode( 85, 0 );
	dspModes[0].width = 800;
	dspDisplay.frequency = 67 << 16;
	CheckRefresh( "refresh-other-size", 67, 0 );

	ResetFullscreen( "refresh-other-depth" );
	AddMode( 85, 0 );
	dspModes[0].depthMask = kDSpDepthMask_16;
	dspDisplay.frequency = 67 << 16;
	CheckRefresh( "refresh-other-depth", 67, 0 );

	/* without a list, what DSpFindBestContext found still works, and still asks */
	ResetFullscreen( "refresh-no-list" );
	AddMode( 85, 0 );
	dspNoDisplayID = 1;
	dspDisplay.mustConfirm = 1;
	dspDisplay.frequency = 120 << 16;
	CheckRefresh( "refresh-no-list", 120, 2 );
}

/* #28: both fast paths are on where the driver has them, and each can be
 * refused by its cvar */
static void CheckFastPaths( const char *name, int multitexture, int cva ) {
	currentCase = name;
	Check( Init(), "GLimp_Init succeeds" );
	CheckLive();
	if ( multitexture ) {
		Check( qglMultiTexCoord2fARB == (void *)glMultiTexCoord2fARB
			&& qglActiveTextureARB == (void *)glActiveTextureARB
			&& qglClientActiveTextureARB == (void *)glClientActiveTextureARB,
			"multitexture is on" );
		Check( glConfig.maxActiveTextures == glTextureUnits, "glConfig has the texture units" );
	} else {
		Check( !qglMultiTexCoord2fARB && !qglActiveTextureARB && !qglClientActiveTextureARB,
			"multitexture is off" );
	}
	if ( cva ) {
		Check( qglLockArraysEXT == (void *)glLockArraysEXT
			&& qglUnlockArraysEXT == (void *)glUnlockArraysEXT, "compiled vertex arrays are on" );
	} else {
		Check( !qglLockArraysEXT && !qglUnlockArraysEXT, "compiled vertex arrays are off" );
	}
	GLimp_Shutdown();
	CheckReleased();
}

static void TestFastPaths( void ) {
	static const char	both[] = "GL_ARB_multitexture GL_EXT_compiled_vertex_array";

	ResetAll();
	glExtensions = both;
	CheckFastPaths( "fast paths, both", 1, 1 );

	glExtensions = "GL_EXT_texture_env_add";
	CheckFastPaths( "fast paths, no extensions", 0, 0 );

	glExtensions = both;
	glTextureUnits = 1;
	CheckFastPaths( "fast paths, one texture unit", 0, 1 );
	glTextureUnits = 2;

	CvarSet( "r_ext_multitexture", "0" );
	CheckFastPaths( "fast paths, r_ext_multitexture 0", 0, 1 );
	CvarSet( "r_ext_multitexture", "1" );

	CvarSet( "r_ext_compiled_vertex_array", "0" );
	CheckFastPaths( "fast paths, r_ext_compiled_vertex_array 0", 1, 0 );
	CvarSet( "r_ext_compiled_vertex_array", "1" );

	CvarSet( "r_allowExtensions", "0" );
	CheckFastPaths( "fast paths, r_allowExtensions 0", 0, 0 );
	CvarSet( "r_allowExtensions", "1" );

	/* and on again once nothing refuses them */
	glExtensions = both;
	CheckFastPaths( "fast paths, restored", 1, 1 );
}

/* #15: fail each call once, and every time, windowed and fullscreen */
static void TestFaults( void ) {
	static const char	*calls[] = {
		"DSpStartup", "DSpFindBestContext", "DSpContext_Reserve", "DSpContext_GetAttributes",
		"DSpContext_SetState", "NewCWindow", "aglChoosePixelFormat", "aglCreateContext",
		"aglSetDrawable", "aglSetCurrentContext"
	};
	static char	name[128];
	int			c, fullscreen, nth, ok;

	for ( fullscreen = 0 ; fullscreen < 2 ; fullscreen++ ) {
		for ( c = 0 ; c < (int)( sizeof( calls ) / sizeof( calls[0] ) ) ; c++ ) {
			for ( nth = 1 ; nth >= 0 ; nth-- ) {
				snprintf( name, sizeof( name ), "fault %s %s %s", calls[c],
					nth ? "once" : "always", fullscreen ? "fullscreen" : "windowed" );
				currentCase = name;
				ResetAll();
				CvarSet( "r_fullscreen", fullscreen ? "1" : "0" );
				failName = calls[c];
				failNth = nth;
				ok = Init();
				if ( !failed ) {
					/* the windowed path makes no DrawSprocket calls */
					Check( ok && !fullscreen && !strncmp( calls[c], "DSp", 3 ),
						"every call is reached" );
				}
				if ( ok ) {
					CheckLive();
					if ( !strcmp( calls[c], "DSpStartup" ) ) {
						/* as retail, a missing DrawSprocket leaves a window */
						Check( !glConfig.isFullscreen, "without DrawSprocket the mode is windowed" );
					} else if ( fullscreen || failed ) {
						/* the mode asked for, or the fullscreen fallback
						 * a failed windowed one falls back to, as retail */
						Check( glConfig.isFullscreen, "the mode or its fallback is fullscreen" );
						Check( r_fullscreen->integer == 1, "r_fullscreen says fullscreen" );
					}
				} else {
					Check( nth == 0 && strcmp( calls[c], "DSpStartup" ),
						"only a call that always fails is fatal" );
					Check( !strcmp( lastError, "Could not initialize OpenGL" ),
						"the fatal error is that both modes failed" );
				}
				/* Com_Error's CL_Shutdown runs GLimp_Shutdown after a fatal one */
				failName = NULL;
				GLimp_Shutdown();
				CheckReleased();
				Check( GammaIsOriginal( 0 ) && GammaIsOriginal( 1 ), "the desktop gamma is put back" );
				/* and the display is usable again */
				ok = Init();
				Check( ok, "a later GLimp_Init succeeds" );
				if ( ok ) {
					CheckLive();
				}
				GLimp_Shutdown();
				CheckReleased();
			}
		}
	}
}

/* #6: r_fullscreen is honoured, and a windowed mode that accelerated GL
 * can't draw (an 8-bit desktop) falls back to 640x480x16 fullscreen, as
 * retail (c79ba93b) and win32 do, instead of failing */
static void TestFullscreenConfig( void ) {
	int		fullscreen;

	for ( fullscreen = 0 ; fullscreen < 2 ; fullscreen++ ) {
		currentCase = fullscreen ? "r_fullscreen 1" : "r_fullscreen 0";
		ResetAll();
		CvarSet( "r_fullscreen", fullscreen ? "1" : "0" );
		Check( Init(), "GLimp_Init succeeds" );
		CheckLive();
		Check( pfChosen == 1, "the first mode is used" );
		Check( r_fullscreen->integer == fullscreen && r_mode->integer == 4,
			"GLimp_Init keeps the configured r_fullscreen and r_mode" );
		Check( glConfig.isFullscreen == fullscreen, "the mode is fullscreen exactly when asked" );
		Check( glConfig.vidWidth == 800 && glConfig.vidHeight == 600, "the configured mode is set" );
		GLimp_Shutdown();
		CheckReleased();
	}

	currentCase = "windowed fails, fullscreen fallback";
	ResetAll();
	noWindowedGL = 1;
	Check( Init(), "GLimp_Init falls back instead of failing" );
	CheckLive();
	Check( pfChosen == 2, "the windowed mode was tried first" );
	Check( r_fullscreen->integer == 1, "the fallback sets r_fullscreen 1" );
	Check( glConfig.isFullscreen, "the fallback is fullscreen" );
	Check( r_mode->integer == 3 && glConfig.vidWidth == 640 && glConfig.vidHeight == 480,
		"the fallback is mode 3" );
	Check( r_colorbits->integer == 16 && dspDepth == kDSpDepthMask_16, "the fallback display is 16-bit" );
	Check( pfRed == 5 && pfGreen == 5 && pfBlue == 5 && pfDepth == 16 && pfStencil == 0,
		"the fallback asks for 5/5/5 color, 16-bit depth and no stencil" );
	GLimp_Shutdown();
	CheckReleased();
}

/* #7: the color AGL is asked for follows r_colorbits, matching the display
 * depth GLimp_ChangeDisplay sets, and the 16-bit fallback runs on a
 * 16-bit-only accelerator */
static void TestPixelFormat( void ) {
	static const char	*colorbits[] = { "0", "15", "16", "24", "32" };
	static char			name[64];
	int					c, fullscreen, size, high;

	for ( fullscreen = 0 ; fullscreen < 2 ; fullscreen++ ) {
		for ( c = 0 ; c < (int)( sizeof( colorbits ) / sizeof( colorbits[0] ) ) ; c++ ) {
			snprintf( name, sizeof( name ), "pixel format r_colorbits %s %s", colorbits[c],
				fullscreen ? "fullscreen" : "windowed" );
			currentCase = name;
			ResetAll();
			CvarSet( "r_fullscreen", fullscreen ? "1" : "0" );
			CvarSet( "r_colorbits", colorbits[c] );
			CvarSet( "r_depthbits", fullscreen ? "0" : "24" );
			CvarSet( "r_stencilbits", fullscreen ? "8" : "0" );
			high = atoi( colorbits[c] ) > 16;
			size = high ? 8 : 5;
			Check( Init(), "GLimp_Init succeeds" );
			CheckLive();
			Check( pfChosen == 1, "the first mode is used" );
			Check( pfRGBA, "AGL is asked for RGBA" );
			Check( pfRed == size && pfGreen == size && pfBlue == size,
				high ? "above 16 bits asks for 8/8/8" : "16 bits or fewer asks for 5/5/5" );
			Check( pfDepth == ( fullscreen ? 16 : 24 ), "AGL is asked for r_depthbits, or 16" );
			Check( pfStencil == ( fullscreen ? 8 : 0 ), "AGL is asked for r_stencilbits" );
			Check( glConfig.colorBits == size * 3, "glConfig has the format's color bits" );
			if ( fullscreen ) {
				Check( dspDepth == ( high ? kDSpDepthMask_32 : kDSpDepthMask_16 ),
					"the display depth matches the color asked for" );
			}
			GLimp_Shutdown();
			CheckReleased();
		}
	}

	currentCase = "16-bit-only accelerator, r_colorbits 16";
	ResetAll();
	only16Bit = 1;
	CvarSet( "r_colorbits", "16" );
	Check( Init(), "GLimp_Init succeeds" );
	CheckLive();
	Check( pfChosen == 1 && !glConfig.isFullscreen, "a 16-bit request needs no fallback" );
	GLimp_Shutdown();
	CheckReleased();

	currentCase = "16-bit-only accelerator, r_colorbits 32";
	ResetAll();
	only16Bit = 1;
	Check( Init(), "GLimp_Init falls back instead of failing" );
	CheckLive();
	Check( pfChosen == 2 && r_colorbits->integer == 16, "the 16-bit fallback is used" );
	Check( pfRed == 5 && pfGreen == 5 && pfBlue == 5, "the fallback asks for 5/5/5" );
	Check( glConfig.isFullscreen && dspDepth == kDSpDepthMask_16, "the fallback display is 16-bit" );
	GLimp_Shutdown();
	CheckReleased();
}

/* is the snapshot complete enough that restoring it puts every device back */
static int SnapshotComplete( Ptr snapshot ) {
	precSystemGamma	sys = (precSystemGamma)snapshot;
	int				i;

	if ( sys->numDevices != NUM_DEVICES || !sys->devGamma ) {
		return 0;
	}
	for ( i = 0 ; i < NUM_DEVICES ; i++ ) {
		if ( !sys->devGamma[i] || sys->devGamma[i]->hGD != &devices[i].gdPtr ) {
			return 0;
		}
		if ( !devices[i].noGamma && !sys->devGamma[i]->pDeviceGamma ) {
			return 0;
		}
	}
	return 1;
}

/* #16 */
static void TestGamma( void ) {
	static unsigned char	ramp[3][256];
	Ptr						snapshot;
	int						i, n, allocs;

	for ( i = 0 ; i < 256 ; i++ ) {
		ramp[0][i] = ramp[1][i] = ramp[2][i] = (unsigned char)( 255 - i );
	}

	currentCase = "gamma-restore";
	ResetAll();
	Check( Init(), "GLimp_Init succeeds" );
	Check( glConfig.deviceSupportsGamma, "a complete snapshot allows hardware gamma" );
	GLimp_SetGamma( ramp[0], ramp[1], ramp[2] );
	Check( !GammaIsOriginal( 0 ), "GLimp_SetGamma changes the display" );
	GLimp_Shutdown();
	Check( GammaIsOriginal( 0 ) && GammaIsOriginal( 1 ), "GLimp_Shutdown puts the gamma back" );
	n = gammaWrites;
	GLimp_Shutdown();
	Check( gammaWrites == n, "a second GLimp_Shutdown writes no gamma" );
	CheckReleased();

	currentCase = "gamma-no-driver-gamma";
	ResetAll();
	devices[1].noGamma = 1;
	snapshot = GetSystemGammas();
	Check( snapshot != NULL, "a display without gamma does not stop the snapshot" );
	if ( snapshot ) {
		Check( SnapshotComplete( snapshot ), "the snapshot covers every display" );
		RestoreSystemGammas( snapshot );
		Check( GammaIsOriginal( 0 ), "the gamma is restored" );
		DisposeSystemGammas( &snapshot );
		Check( snapshot == NULL, "DisposeSystemGammas clears the pointer" );
	}
	Check( ptrsLive == 0, "the snapshot is disposed" );

	/* count a full snapshot's allocations, then fail each one */
	currentCase = "gamma-alloc-count";
	ResetAll();
	snapshot = GetSystemGammas();
	allocs = ptrAllocs;
	Check( snapshot != NULL && allocs > 0, "the snapshot allocates" );
	DisposeSystemGammas( &snapshot );

	for ( n = 1 ; n <= allocs ; n++ ) {
		static char	name[64];

		snprintf( name, sizeof( name ), "gamma-alloc-fail %d", n );
		currentCase = name;
		ResetAll();
		failPtrAt = n;
		snapshot = GetSystemGammas();
		/* every allocation is needed, so one that failed fails the snapshot */
		Check( snapshot == NULL, "a failed allocation fails the snapshot, not half-builds it" );
		Check( gammaWrites == 0, "taking a snapshot writes no gamma" );
		if ( snapshot ) {
			/* half-built: leave it rather than walk its garbage */
			continue;
		}
		Check( ptrsLive == 0, "a failed snapshot frees what it took" );

		/* the renderer must then leave the desktop gamma alone */
		ResetAll();
		failPtrAt = n;
		Check( Init(), "GLimp_Init succeeds without a gamma snapshot" );
		Check( !glConfig.deviceSupportsGamma, "no snapshot, no hardware gamma" );
		GLimp_SetGamma( ramp[0], ramp[1], ramp[2] );
		Check( gammaWrites == 0, "GLimp_SetGamma does not touch an unsaved gamma" );
		GLimp_Shutdown();
		Check( gammaWrites == 0, "GLimp_Shutdown restores nothing it did not save" );
		CheckReleased();
		Check( GammaIsOriginal( 0 ) && GammaIsOriginal( 1 ), "the desktop gamma is untouched" );
	}
}

/* A recursive Com_Error reaches Sys_Error with the display still captured. */
static void TestFatalRelease( void ) {
	static unsigned char	ramp[3][256];
	int						i;

	for ( i = 0 ; i < 256 ; i++ ) {
		ramp[0][i] = ramp[1][i] = ramp[2][i] = (unsigned char)( 255 - i );
	}

	currentCase = "fatal release";
	ResetAll();
	CvarSet( "r_fullscreen", "1" );
	Check( Init(), "GLimp_Init succeeds" );
	Check( glConfig.isFullscreen && sys_gl.DSpContext, "fullscreen holds the display" );
	GLimp_SetGamma( ramp[0], ramp[1], ramp[2] );
	GLimp_ReleaseDisplay();
	Check( !glConfig.isFullscreen && !sys_gl.DSpContext && !dspDisplay.reserved && !dspStarted,
		"the display is released" );
	Check( GammaIsOriginal( 0 ) && GammaIsOriginal( 1 ), "the desktop gamma is back" );
	Check( !currentContext, "no context stays current" );
	GLimp_ReleaseDisplay();		/* a second fatal error, or one after shutdown */
	GLimp_Shutdown();
	CheckReleased();
	GLimp_ReleaseDisplay();
}

int main( void ) {
	Setup();

	TestAnisotropic();
	TestS3tc();
	TestRestartCycles();
	TestBadMode();
	TestConfirmSwitch();
	TestRefresh();
	TestStencil();
	TestFastPaths();
	TestFaults();
	TestFullscreenConfig();
	TestPixelFormat();
	TestGamma();
	TestFatalRelease();

	FreeDevices();
	if ( failures ) {
		fprintf( stderr, "mac_glimp: %d failure(s)\n", failures );
		return 1;
	}
	printf( "mac_glimp: all cases passed\n" );
	return 0;
}
