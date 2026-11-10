/* Issues #256, #291 and #18: classic Mac keyboard input and its event queue.
 *
 * #256: Mac OS posts no keyDown/keyUp for modifier keys; their state is only
 * in EventRecord.modifiers, which WaitNextEvent and GetOSEvent also fill in
 * for the null event they return when nothing is pending.  A modifier pressed
 * and released with no other input must give exactly one down and one up, on
 * the poll that sees it, as id's original Sys_SendKeyEvents did.
 *
 * #291: a windowed game in the background still gets null events, and they
 * carry the system-wide modifiers.  After a suspend event, modifiers pressed
 * in the front application must give no key events.  Keys, modifiers and
 * InputSprocket mouse buttons held at suspend must be released once, because
 * their key-ups go to the front application, and modifiers must resync on
 * resume.  The cursor is shown while suspended and hidden again on resume
 * only if Sys_InitInput hid it, so it stays visible without InputSprocket
 * (in_nomouse, a failed ISpStartup, or input shut down).  An InputSprocket
 * button press still buffered at the suspend event
 * must be drained and released with the rest, and one buffered across the
 * suspend must be flushed on resume, not delivered without its release.
 *
 * #18: when the 256-slot queue fills, the dropped event's Z_Malloc payload
 * must be freed, the newest events kept, and no key release lost, or a key
 * whose press was already delivered stays down.  The overflow is reported
 * once, from Sys_GetEvent, with the number of events dropped, and nothing is
 * printed from inside the queue.
 *
 * #17: Sys_InitInput must find a mouse's X and Y movement and its buttons by
 * element kind and label, wherever they are in the element list, clamp the
 * device and element counts to its buffers, map at most K_MOUSE1..K_MOUSE5
 * (a mouse listing more buttons once sent key numbers past K_LAST_KEY, which
 * CL_KeyEvent indexes keys[] with unchecked), skip mice it cannot use, and,
 * if InputSprocket fails or finds no usable mouse, shut it down and leave
 * the cursor and the Event Manager's mouse button alone.
 *
 * #19: the Finder's Open Application, Open Documents and Quit Application
 * Apple Events reach handlers through AEProcessAppleEvent, and Quit queues
 * the quit command; an update event for the console window redraws it; the
 * front window is unhighlighted on suspend and highlighted on resume; and
 * Sys_PumpEvents, which the renderer calls during long frames, pumps.
 *
 * #21: a dedicated server has no game window, so, as in id's SIOUX version,
 * keys typed go to the console window: echoed there, and each line handed to
 * the engine as an SE_CONSOLE event by Sys_GetEvent, with Delete taking back
 * a character.  A line longer than the ring is cut, not overflowed.
 * Command keys, and every key in a game that is not dedicated, still reach
 * the game.  Sys_WaitEvent, which NET_Sleep and the name lookup wait in,
 * passes its sleep to WaitNextEvent and reports Esc and Command-period.
 *
 * The runner extracts the real queue (Sys_QueEvent and its helpers),
 * Sys_GetEvent and Sys_PumpEvents from mac_main.c, and Sys_MsecForMacEvent,
 * vkeyToQuakeKey, DoKeyDown, DoKeyUp, Sys_ModifierEvents, DoOSEvent,
 * DoUpdate, the Apple Event handlers, Sys_InitAppleEvents, Sys_SendKeyEvents
 * and Sys_WaitEvent from mac_event.c, the console ring, Sys_ConsoleEvent and
 * Sys_ConsoleInput from mac_console.c, verbatim, and includes all of
 * mac_input.c after its #includes (Sys_QueEvent renamed
 * Sys_QueEvent_extracted, so a wrapper can tell when the queue is running).
 * The Event Manager is a fake that returns scripted events and otherwise null
 * events carrying the current modifiers, and dispatches high-level events to
 * the installed handlers; InputSprocket is a fake with devices, element lists
 * and per-element event queues.  The fixture plays the engine: it applies key
 * events to its own key state and frees packet payloads, as Com_EventLoop
 * does. */
#include "../../src/game/q_shared.h"
#include "et_compat.h"
#include "../../src/qcommon/qcommon.h"
#include "../../src/ui/keycodes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake Event Manager ---- */

typedef unsigned char	Boolean;
typedef unsigned short	EventMask;
typedef unsigned int	KeyMap[4];
typedef void			*WindowPtr;
typedef struct {
	short			what;
	long			message;
	unsigned long	when;
	struct { short v, h; } where;
	unsigned short	modifiers;
} EventRecord;

enum { nullEvent = 0, mouseDown = 1, mouseUp = 2, keyDown = 3, keyUp = 4,
	autoKey = 5, updateEvt = 6, diskEvt = 7, activateEvt = 8, osEvt = 15,
	kHighLevelEvent = 23 };
enum { everyEvent = 0xFFFF, updateMask = 0x0040 };
enum { charCodeMask = 0x000000FF, keyCodeMask = 0x0000FF00 };
enum { btnState = 0x0080, cmdKey = 0x0100, shiftKey = 0x0200, alphaLock = 0x0400,
	optionKey = 0x0800, controlKey = 0x1000 };
enum { suspendResumeMessage = 0x01, resumeFlag = 1 };
#define BitAnd( a, b )	( (a) & (b) )
#define nil			NULL

#define MAX_SCRIPTED	16
static EventRecord		scripted[MAX_SCRIPTED];
static int				scriptedHead, scriptedTail;
static unsigned short	macModifiers = btnState;	/* system-wide; button up */
static unsigned long	macTicks = 1000;

static void PostEvent( short what, long message ) {
	EventRecord *ev = &scripted[scriptedHead++ % MAX_SCRIPTED];
	memset( ev, 0, sizeof( *ev ) );
	ev->what = what;
	ev->message = message;
}

static Boolean NextEvent( EventRecord *event ) {
	macTicks++;
	if ( scriptedTail < scriptedHead ) {
		*event = scripted[scriptedTail++ % MAX_SCRIPTED];
		event->when = macTicks;
		event->modifiers = macModifiers;
		return 1;
	}
	/* a null event: when and modifiers are still filled in */
	memset( event, 0, sizeof( *event ) );
	event->what = nullEvent;
	event->when = macTicks;
	event->modifiers = macModifiers;
	return 0;
}

static long		lastSleep = -1;
static Boolean WaitNextEvent( EventMask mask, EventRecord *event, unsigned long sleep, void *rgn ) {
	(void)mask; (void)rgn;
	lastSleep = sleep;
	return NextEvent( event );
}
static Boolean GetOSEvent( EventMask mask, EventRecord *event ) {
	(void)mask;
	return NextEvent( event );
}
static unsigned long TickCount( void ) { return macTicks; }
static void GetKeys( KeyMap keys ) { memset( keys, 0, sizeof( KeyMap ) ); }
static void SysBeep( short duration ) { (void)duration; }
/* the cursor level: 0 shown, below 0 hidden; ShowCursor stops at 0 */
static int				cursorLevel;
static void ShowCursor( void ) {
	if ( cursorLevel < 0 ) {
		cursorLevel++;
	}
}
static void HideCursor( void ) { cursorLevel--; }

/* ---- fake Window Manager, QuickDraw and AGL ---- */

typedef void			*GrafPtr;
typedef void			*AGLContext;
static char				gameWindowStorage, consoleWindowStorage, otherPortStorage;
#define gameWindow		( (WindowPtr)&gameWindowStorage )
#define consoleWindow	( (WindowPtr)&consoleWindowStorage )
static struct { void *drawable; } sys_gl = { &gameWindowStorage };
static GrafPtr			currentPort = &otherPortStorage;
static WindowPtr		frontWindow = &gameWindowStorage;
static WindowPtr		updating;			/* between BeginUpdate and EndUpdate */
static int				updatesBegun, updatesEnded, aglUpdates;
static int				hilites, hilited = 1;
static WindowPtr		consoleDrawn;
static const char		*currentCase;
static int				failures;

static void GetPort( GrafPtr *port ) { *port = currentPort; }
static void SetPort( GrafPtr port ) { currentPort = port; }
static void BeginUpdate( WindowPtr window ) {
	if ( updating || currentPort != window ) {
		fprintf( stderr, "FAIL %s: BeginUpdate nested or not in the window's port\n", currentCase );
		failures++;
	}
	updating = window;
	updatesBegun++;
}
static void EndUpdate( WindowPtr window ) {
	if ( updating != window ) {
		fprintf( stderr, "FAIL %s: EndUpdate without its BeginUpdate\n", currentCase );
		failures++;
	}
	updating = NULL;
	updatesEnded++;
}
static AGLContext aglGetCurrentContext( void ) { return &gameWindowStorage; }
static void aglUpdateContext( AGLContext ctx ) { (void)ctx; aglUpdates++; }
static WindowPtr FrontWindow( void ) { return frontWindow; }
static void HiliteWindow( WindowPtr window, Boolean on ) {
	if ( window == frontWindow ) {
		hilites++;
		hilited = on;
	}
}
/* mac_consolehooks.cc */
void Sys_ConsoleDraw( WindowPtr window ) {
	if ( window == consoleWindow ) {
		if ( updating != consoleWindow ) {
			fprintf( stderr, "FAIL %s: the console drawn outside its update\n", currentCase );
			failures++;
		}
		consoleDrawn = window;
	}
}

/* ---- fake Apple Event Manager ---- */

typedef short			OSErr;
typedef unsigned int	OSType;
typedef OSType			AEEventClass;
typedef OSType			AEEventID;
typedef struct { AEEventClass eventClass; AEEventID eventID; } AppleEvent;
#define pascal
typedef OSErr ( *AEEventHandlerProcPtr )( const AppleEvent *event, AppleEvent *reply, long refcon );
typedef AEEventHandlerProcPtr	AEEventHandlerUPP;
#define NewAEEventHandlerUPP( proc )	( proc )
enum { noErr = 0, errAEEventNotHandled = -1708 };
enum { kCoreEventClass = 'aevt', kAEOpenApplication = 'oapp', kAEOpenDocuments = 'odoc',
	kAEPrintDocuments = 'pdoc', kAEQuitApplication = 'quit' };

#define MAX_AE_HANDLERS	8
static struct { AEEventClass eventClass; AEEventID eventID; AEEventHandlerUPP handler; } aeHandlers[MAX_AE_HANDLERS];
static int				numAEHandlers, aeProcessed;
static OSErr			lastAEResult = 1;

static OSErr AEInstallEventHandler( AEEventClass eventClass, AEEventID eventID,
		AEEventHandlerUPP handler, long refcon, Boolean isSysHandler ) {
	(void)refcon;
	if ( isSysHandler || numAEHandlers == MAX_AE_HANDLERS ) {
		return -50;	/* paramErr */
	}
	aeHandlers[numAEHandlers].eventClass = eventClass;
	aeHandlers[numAEHandlers].eventID = eventID;
	aeHandlers[numAEHandlers].handler = handler;
	numAEHandlers++;
	return noErr;
}

/* a high-level event has its class in message and its ID in where */
static void PostAppleEvent( AEEventClass eventClass, AEEventID eventID ) {
	EventRecord *ev = &scripted[scriptedHead++ % MAX_SCRIPTED];
	memset( ev, 0, sizeof( *ev ) );
	ev->what = kHighLevelEvent;
	ev->message = eventClass;
	ev->where.v = eventID >> 16;
	ev->where.h = eventID & 0xFFFF;
}

static OSErr AEProcessAppleEvent( const EventRecord *event ) {
	AppleEvent	ae, reply;
	int			i;

	aeProcessed++;
	ae.eventClass = (AEEventClass)event->message;
	ae.eventID = ( (unsigned)(unsigned short)event->where.v << 16 ) | (unsigned short)event->where.h;
	lastAEResult = errAEEventNotHandled;
	for ( i = 0 ; i < numAEHandlers ; i++ ) {
		if ( aeHandlers[i].eventClass == ae.eventClass && aeHandlers[i].eventID == ae.eventID ) {
			memset( &reply, 0, sizeof( reply ) );
			lastAEResult = aeHandlers[i].handler( &ae, &reply, 0 );
			break;
		}
	}
	return lastAEResult;
}

/* the command buffer */
static char				cbufText[256];
void Cbuf_ExecuteText( int exec_when, const char *text ) {
	if ( exec_when != EXEC_APPEND ) {
		fprintf( stderr, "FAIL %s: a command run at once from an Apple Event\n", currentCase );
		failures++;
	}
	strncat( cbufText, text, sizeof( cbufText ) - strlen( cbufText ) - 1 );
}

/* ---- fake InputSprocket: devices, element lists and element queues ---- */

typedef unsigned char	UInt8;
typedef unsigned int	UInt32;
typedef int				OSStatus;
enum { false = 0, true = 1 };	/* MacTypes.h */
typedef struct { UInt8 majorRev, minorAndBugRev, stage, nonRelRev; } NumVersion;
enum {
	kISpDeviceClass_Mouse = 'mous', kISpDeviceClass_Keyboard = 'keyd',
	kISpElementKind_Button = 'butn', kISpElementKind_Axis = 'axis', kISpElementKind_Delta = 'dlta',
	kISpElementKind_DPad = 'dpad',
	kISpElementLabel_None = 'none',
	kISpElementLabel_Axis_XAxis = 'xaxi', kISpElementLabel_Axis_YAxis = 'yaxi',
	kISpElementLabel_Axis_ZAxis = 'zaxi',
	kISpElementLabel_Delta_X = 'xdlt', kISpElementLabel_Delta_Y = 'ydlt',
	kISpElementLabel_Delta_Z = 'zdlt',
	kISpElementLabel_Delta_Cursor_X = 'curx', kISpElementLabel_Delta_Cursor_Y = 'cury',
	kISpElementLabel_Btn_MouseOne = 'mou1', kISpElementLabel_Btn_MouseTwo = 'mou2',
	kISpElementLabel_Btn_MouseThree = 'mou3'
};

#define ISP_QUEUE		16
#define ISP_ELEMENTS	2048
#define ISP_DEVICES		160
typedef struct fakeDevice_s fakeDevice_t;
typedef struct {
	fakeDevice_t	*device;
	OSType			kind, label;
	UInt32			queue[ISP_QUEUE];
	int				queued;
	UInt32			state;			/* movement since the last read */
} fakeElement_t;
struct fakeDevice_s {
	OSType			deviceClass;
	int				first, count;	/* its elements in ispElements */
	OSStatus		listErr, extractErr, activateErr;
	qboolean		active;
};
typedef fakeElement_t	*ISpElementReference;
typedef fakeDevice_t	*ISpDeviceReference;
typedef fakeDevice_t	*ISpElementListReference;	/* a device's own list */
typedef struct {
	OSType			theLabel;
	OSType			theKind;
	unsigned char	theString[64];	/* Str63 */
	UInt32			reserved1, reserved2;
} ISpElementInfo;
typedef struct {
	unsigned long long	when;
	ISpElementReference	element;
	UInt32				refCon;
	UInt32				data;
} ISpElementEvent;

static fakeElement_t	ispElements[ISP_ELEMENTS];
static fakeDevice_t		ispDevices[ISP_DEVICES];
static int				numIspElements, numIspDevices;
static fakeElement_t	*defaultButtons[5];	/* the default mouse's, by key */
static qboolean			ispStarted, ispSuspended;
static int				ispPressAtSuspend;	/* a press that lands as ISp suspends */
static int				suspendCalls, resumeCalls, flushCalls, shutdownCalls;

static fakeDevice_t *AddDevice( OSType deviceClass ) {
	fakeDevice_t *device = &ispDevices[numIspDevices++];
	device->deviceClass = deviceClass;
	device->first = numIspElements;
	return device;
}

/* elements go in the list of the device added last */
static fakeElement_t *AddElement( fakeDevice_t *device, OSType kind, OSType label ) {
	fakeElement_t *element = &ispElements[numIspElements++];
	element->device = device;
	element->kind = kind;
	element->label = label;
	device->count++;
	return element;
}

/* a keyboard, and a mouse listing X, Y and three buttons, as retail assumed */
static void DefaultDevices( void ) {
	fakeDevice_t	*mouse;
	int				i;

	AddDevice( kISpDeviceClass_Keyboard );
	mouse = AddDevice( kISpDeviceClass_Mouse );
	AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	for ( i = 0 ; i < 3 ; i++ ) {
		defaultButtons[i] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne + i );
	}
}

static void IspCheckElement( ISpElementReference element, const char *call ) {
	if ( element < ispElements || element >= ispElements + numIspElements ) {
		fprintf( stderr, "FAIL %s: %s on a bad element reference\n", currentCase, call );
		failures++;
		exit( 1 );
	}
}

static void IspCheckDevice( ISpDeviceReference device, const char *call ) {
	if ( device < ispDevices || device >= ispDevices + numIspDevices ) {
		fprintf( stderr, "FAIL %s: %s on a bad device reference\n", currentCase, call );
		failures++;
		exit( 1 );
	}
}

static void IspPress( fakeElement_t *element, int down ) {
	if ( element->queued < ISP_QUEUE ) {
		element->queue[element->queued++] = down;
	}
}

static void IspEvent( int button, int down ) {
	IspPress( defaultButtons[button - K_MOUSE1], down );
}

static void IspNeedsStartup( const char *call ) {
	if ( !ispStarted ) {
		fprintf( stderr, "FAIL %s: %s without ISpStartup\n", currentCase, call );
		failures++;
	}
}

static NumVersion ISpGetVersion( void ) {
	NumVersion v = { 1, 0x70, 0x80, 0 };
	return v;
}
static OSStatus ISpStartup( void ) { ispStarted = qtrue; return 0; }
static OSStatus ISpShutdown( void ) {
	int i;
	for ( i = 0 ; i < numIspDevices ; i++ ) {
		ispDevices[i].active = qfalse;
	}
	shutdownCalls++;
	ispStarted = qfalse;
	return 0;
}
static OSStatus ISpSuspend( void ) {
	IspNeedsStartup( "ISpSuspend" );
	suspendCalls++;
	ispSuspended = qtrue;
	if ( ispPressAtSuspend ) {
		IspEvent( ispPressAtSuspend, 1 );
		ispPressAtSuspend = 0;
	}
	return 0;
}
static OSStatus ISpResume( void ) {
	IspNeedsStartup( "ISpResume" );
	resumeCalls++;
	ispSuspended = qfalse;
	return 0;
}

/* As InputSprocket: the count is the total, the buffer gets what fits. */
static OSStatus ExtractDevices( OSType deviceClass, UInt32 bufferCount, UInt32 *count,
		ISpDeviceReference *buffer ) {
	int i;

	IspNeedsStartup( "ISpDevices_Extract" );
	*count = 0;
	for ( i = 0 ; i < numIspDevices ; i++ ) {
		if ( deviceClass && ispDevices[i].deviceClass != deviceClass ) {
			continue;
		}
		if ( *count < bufferCount ) {
			buffer[*count] = &ispDevices[i];
		}
		( *count )++;
	}
	return 0;
}
static OSStatus ISpDevices_Extract( UInt32 bufferCount, UInt32 *count, ISpDeviceReference *buffer ) {
	return ExtractDevices( 0, bufferCount, count, buffer );
}
static OSStatus ISpDevices_ExtractByClass( OSType deviceClass, UInt32 bufferCount, UInt32 *count,
		ISpDeviceReference *buffer ) {
	return ExtractDevices( deviceClass, bufferCount, count, buffer );
}
static OSStatus ISpDevices_Deactivate( UInt32 count, ISpDeviceReference *devices ) {
	UInt32 i;
	for ( i = 0 ; i < count ; i++ ) {
		IspCheckDevice( devices[i], "ISpDevices_Deactivate" );
		devices[i]->active = qfalse;
	}
	return 0;
}
static OSStatus ISpDevices_Activate( UInt32 count, ISpDeviceReference *devices ) {
	UInt32 i;
	for ( i = 0 ; i < count ; i++ ) {
		IspCheckDevice( devices[i], "ISpDevices_Activate" );
		if ( devices[i]->activateErr ) {
			return devices[i]->activateErr;
		}
	}
	for ( i = 0 ; i < count ; i++ ) {
		devices[i]->active = qtrue;
	}
	return 0;
}
static OSStatus ISpDevice_GetElementList( ISpDeviceReference device, ISpElementListReference *list ) {
	IspCheckDevice( device, "ISpDevice_GetElementList" );
	if ( device->listErr ) {
		return device->listErr;
	}
	*list = device;
	return 0;
}
static OSStatus ISpElementList_Extract( ISpElementListReference list, UInt32 bufferCount,
		UInt32 *count, ISpElementReference *buffer ) {
	int i;

	IspCheckDevice( list, "ISpElementList_Extract" );
	if ( list->extractErr ) {
		return list->extractErr;
	}
	for ( i = 0 ; i < list->count && i < (int)bufferCount ; i++ ) {
		buffer[i] = &ispElements[list->first + i];
	}
	*count = list->count;
	return 0;
}
static OSStatus ISpElement_GetInfo( ISpElementReference element, ISpElementInfo *info ) {
	IspCheckElement( element, "ISpElement_GetInfo" );
	memset( info, 0, sizeof( *info ) );
	info->theKind = element->kind;
	info->theLabel = element->label;
	info->theString[0] = 4;
	memcpy( info->theString + 1, "elem", 4 );
	return 0;
}
static void PStringToCString( char *s ) {
	int len = (unsigned char)s[0];
	memmove( s, s + 1, len );
	s[len] = 0;
}
static OSStatus ISpElement_GetNextEvent( ISpElementReference element, UInt32 size,
		ISpElementEvent *event, Boolean *wasEvent ) {
	IspCheckElement( element, "ISpElement_GetNextEvent" );
	IspNeedsStartup( "ISpElement_GetNextEvent" );
	*wasEvent = 0;
	if ( !ispStarted || ispSuspended || !element->device->active || size != sizeof( *event )
			|| !element->queued ) {
		return 0;
	}
	memset( event, 0, sizeof( *event ) );
	event->element = element;
	event->data = element->queue[0];
	memmove( element->queue, element->queue + 1, --element->queued * sizeof( UInt32 ) );
	*wasEvent = 1;
	return 0;
}
static OSStatus ISpElement_GetSimpleState( ISpElementReference element, UInt32 *state ) {
	IspCheckElement( element, "ISpElement_GetSimpleState" );
	IspNeedsStartup( "ISpElement_GetSimpleState" );
	*state = 0;
	if ( ispStarted && !ispSuspended && element->device->active ) {
		*state = element->state;
		element->state = 0;
	}
	return 0;
}
static OSStatus ISpElement_Flush( ISpElementReference element ) {
	IspCheckElement( element, "ISpElement_Flush" );
	if ( !ispStarted ) {
		fprintf( stderr, "FAIL %s: ISpElement_Flush without InputSprocket\n", currentCase );
		failures++;
		return -50;	/* paramErr */
	}
	flushCalls++;
	element->queued = 0;
	return 0;
}

static cvar_t		noMouseCvar, dedicatedCvar;
cvar_t				*com_dedicated = &dedicatedCvar;
cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	(void)value; (void)flags;
	if ( strcmp( name, "in_nomouse" ) ) {
		fprintf( stderr, "FAIL %s: unexpected Cvar_Get %s\n", currentCase, name );
		failures++;
	}
	return &noMouseCvar;
}

/* ---- the rest of the Mac port, as far as these functions reach ---- */

static struct { qboolean isFullscreen; } glConfig;
static cvar_t		waitNextEventCvar;
static cvar_t		*sys_waitNextEvent = &waitNextEventCvar;
static qboolean		ignoreUpdateEvents;
int					sys_ticBase, sys_msecBase, sys_lastEventTic;
extern qboolean		inputActive;
extern qboolean		inputSystemSuspended;

void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );
void Sys_QueEvent_extracted( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );
void Sys_ReleaseKeys( void );
void Sys_SendKeyEvents( void );
qboolean Sys_WaitEvent( long sleepTicks, qboolean *cancel );
void Sys_LogPrintf( const char *fmt, ... ) { (void)fmt; }	/* ET: mac_event.c logs update events */
void Sys_ModifierEvents( int modifiers );
void Sys_Input( void );
void Sys_SuspendInput( void );
void Sys_ResumeInput( void );
void Sys_ShutdownInput( void );
void Sys_InitInput( void );
void Sys_InitAppleEvents( void );
void Sys_PumpEvents( void );
void DoMouseDown( EventRecord *event ) { (void)event; }
void DoMouseUp( EventRecord *event ) { (void)event; }
void DoDiskEvent( EventRecord *event ) { (void)event; }
void DoActivate( WindowPtr window, int modifiers ) { (void)window; (void)modifiers; }
static qboolean Sys_GetPacket( netadr_t *from, msg_t *msg ) { (void)from; (void)msg; return qfalse; }
void MSG_Init( msg_t *buf, byte *data, int length ) {
	memset( buf, 0, sizeof( *buf ) );
	buf->data = data;
	buf->maxsize = length;
}
void Com_DumpFlightRecord( const char *name ) { (void)name; }
static void Sys_DumpRetroLogs( const char *name ) { (void)name; }

static int			milliseconds = 5000;
int Sys_Milliseconds( void ) { return milliseconds; }

/* zone allocations, counted */
static int			zoneAllocs, zoneFrees;
void *Z_Malloc( int size ) {
	zoneAllocs++;
	return calloc( 1, size );
}
void Z_Free( void *ptr ) {
	zoneFrees++;
	free( ptr );
}

/* Com_Printf must not run inside the queue (#18): it can reach the
 * console and the log while the queue is mid-update. */
static int			insideQueue, printedInsideQueue;
void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr ) {
	insideQueue++;
	Sys_QueEvent_extracted( time, type, value, value2, ptrLength, ptr );
	insideQueue--;
}

static char			printed[16384];
void QDECL Com_Printf( const char *fmt, ... ) {
	va_list	ap;
	size_t	len = strlen( printed );

	if ( insideQueue && !printedInsideQueue ) {
		fprintf( stderr, "FAIL %s: Com_Printf from inside Sys_QueEvent\n", currentCase );
		failures++;
		printedInsideQueue = 1;
	}
	va_start( ap, fmt );
	vsnprintf( printed + len, sizeof( printed ) - len, fmt, ap );
	va_end( ap );
}

/* what the console echoes */
static char			echoed[4096];
static int ConsoleEcho( const char *fmt, ... ) {
	va_list	ap;
	size_t	len = strlen( echoed );

	va_start( ap, fmt );
	vsnprintf( echoed + len, sizeof( echoed ) - len, fmt, ap );
	va_end( ap );
	return 0;
}
#define printf		ConsoleEcho
#define fflush( f )	( (void)( f ) )
#include "mac_console_extracted.c"
#undef printf
#undef fflush

#include "mac_event_extracted.c"
#include "mac_main_extracted.c"
#include "mac_input_extracted.c"
/* mac_input.c's, as Sys_InitInput leaves them */
extern UInt32		numDevices;

/* ---- the engine side ---- */

static int			keyDownState[256];
static int			downEvents[256], upEvents[256];
static int			packetsSeen, lastPacket;
static int			mouseX, mouseY;
static char			consoleLines[4096];
static int			consoleEvents;

static void Check( int ok, const char *message ) {
	if ( !ok ) {
		fprintf( stderr, "FAIL %s: %s\n", currentCase, message );
		failures++;
	}
}

static void ClearCounts( void ) {
	memset( downEvents, 0, sizeof( downEvents ) );
	memset( upEvents, 0, sizeof( upEvents ) );
}

/* one Com_EventLoop pass: poll and drain until the queue is empty */
static void Frame( void ) {
	sysEvent_t	ev;
	int			guard;

	milliseconds += 16;
	for ( guard = 0 ; guard < 4096 ; guard++ ) {
		ev = Sys_GetEvent();
		if ( ev.evType == SE_NONE ) {
			return;
		}
		/* CL_KeyEvent indexes keys[MAX_KEYS] with the key unchecked */
		if ( ev.evType == SE_KEY && ( ev.evValue < 0 || ev.evValue >= K_LAST_KEY ) ) {
			Check( 0, "a key event outside the key numbers" );
		} else if ( ev.evType == SE_KEY ) {
			keyDownState[ev.evValue] = ev.evValue2 != 0;
			if ( ev.evValue2 ) {
				downEvents[ev.evValue]++;
			} else {
				upEvents[ev.evValue]++;
			}
		}
		if ( ev.evType == SE_MOUSE ) {
			mouseX += ev.evValue;
			mouseY += ev.evValue2;
		}
		if ( ev.evType == SE_CONSOLE ) {
			Check( ev.evPtrLength == (int)strlen( ev.evPtr ) + 1, "a console line comes with its length" );
			consoleEvents++;
			strncat( consoleLines, ev.evPtr, sizeof( consoleLines ) - strlen( consoleLines ) - 2 );
			strcat( consoleLines, "\n" );
		}
		if ( ev.evType == SE_PACKET ) {
			packetsSeen++;
			lastPacket = *(int *)ev.evPtr;
		}
		if ( ev.evPtr ) {
			Z_Free( ev.evPtr );
		}
	}
	Check( 0, "the queue never drained" );
}

static int AnyKeyDown( void ) {
	int i;
	for ( i = 0 ; i < 256 ; i++ ) {
		if ( keyDownState[i] ) {
			return 1;
		}
	}
	return 0;
}

static int Occurrences( const char *text, const char *word ) {
	int count = 0;
	while ( ( text = strstr( text, word ) ) != NULL ) {
		count++;
		text += strlen( word );
	}
	return count;
}

static void Suspend( void ) { PostEvent( osEvt, (long)suspendResumeMessage << 24 ); }
static void Resume( void ) { PostEvent( osEvt, ( (long)suspendResumeMessage << 24 ) | resumeFlag ); }

/* #256 */
static void ModifierAlone( qboolean fullscreen ) {
	static const struct { int bit, key; } mods[] = {
		{ controlKey, K_CTRL }, { shiftKey, K_SHIFT }, { optionKey, K_ALT }, { cmdKey, K_COMMAND }
	};
	int i;

	glConfig.isFullscreen = fullscreen;	/* GetOSEvent, or WaitNextEvent on the desktop */
	Frame();
	for ( i = 0 ; i < (int)( sizeof( mods ) / sizeof( mods[0] ) ) ; i++ ) {
		ClearCounts();
		macModifiers = btnState | mods[i].bit;
		Frame();
		Check( keyDownState[mods[i].key], "a modifier pressed alone is down after one frame" );
		Check( downEvents[mods[i].key] == 1, "a modifier pressed alone gives one down event" );
		Frame();
		Check( downEvents[mods[i].key] == 1, "a held modifier gives no further events" );
		macModifiers = btnState;
		Frame();
		Check( !keyDownState[mods[i].key], "a modifier released alone is up after one frame" );
		Check( upEvents[mods[i].key] == 1, "a modifier released alone gives one up event" );
	}
	Check( !AnyKeyDown(), "nothing is left down" );
}

/* #291: presses made in the front application */
static void BackgroundModifiers( void ) {
	Frame();
	Suspend();
	Frame();
	Check( suspendCalls == 1, "the suspend event suspends InputSprocket" );
	Check( cursorLevel == 0, "the cursor is shown while in the background" );
	ClearCounts();
	macModifiers = btnState | controlKey | shiftKey | optionKey;
	Frame();
	Frame();
	Check( !downEvents[K_CTRL] && !downEvents[K_SHIFT] && !downEvents[K_ALT],
		"modifiers pressed in the background give no key events" );
	Check( !AnyKeyDown(), "nothing is down while in the background" );
	macModifiers = btnState;
	Frame();
	Check( !upEvents[K_CTRL] && !upEvents[K_SHIFT] && !upEvents[K_ALT],
		"modifiers released in the background give no key events" );
	/* back to the game with Shift held: it resyncs on resume */
	macModifiers = btnState | shiftKey;
	Resume();
	Frame();
	Frame();
	Check( resumeCalls == 1, "the resume event resumes InputSprocket" );
	Check( cursorLevel == -1, "the cursor is hidden again on resume" );
	Check( keyDownState[K_SHIFT] && downEvents[K_SHIFT] == 1, "a modifier held at resume is down once" );
	macModifiers = btnState;
	Frame();
	Check( !keyDownState[K_SHIFT] && upEvents[K_SHIFT] == 1, "and released once" );
}

/* #291: keys held across Cmd-Tab */
static void SuspendReleases( void ) {
	Frame();
	macModifiers = btnState | controlKey;
	PostEvent( keyDown, ( 0x0D << 8 ) | 'w' );	/* the W key */
	IspEvent( K_MOUSE2, 1 );
	Frame();
	Check( keyDownState['w'] && keyDownState[K_CTRL] && keyDownState[K_MOUSE2], "W, Ctrl and mouse 2 are down" );
	ClearCounts();
	/* Cmd-Tab: the suspend event arrives with Ctrl (and Cmd) still held */
	macModifiers = btnState | controlKey | cmdKey;
	Suspend();
	Frame();
	Check( !AnyKeyDown(), "every key held at suspend is released" );
	Check( upEvents['w'] == 1 && upEvents[K_CTRL] == 1 && upEvents[K_MOUSE2] == 1,
		"each held key is released once" );
	/* their real key-ups go to the front application; the game sees none */
	macModifiers = btnState;
	Frame();
	Check( upEvents[K_CTRL] == 1 && upEvents[K_COMMAND] <= 1, "no second release while in the background" );
	Check( !AnyKeyDown(), "nothing is down while in the background" );
}

/* #291: an InputSprocket click buffered when the suspend event arrives */
static void IspPressAtSuspend( void ) {
	Frame();
	ClearCounts();
	/* pressed after the last Sys_Input, in the poll that sees the suspend */
	IspEvent( K_MOUSE2, 1 );
	Suspend();
	Frame();
	Check( downEvents[K_MOUSE2] == 1 && upEvents[K_MOUSE2] == 1,
		"a press buffered at suspend is delivered and released" );
	Check( !AnyKeyDown(), "nothing is down while in the background" );
	Resume();
	Frame();
	Frame();
	Check( resumeCalls == 1, "the resume event resumes InputSprocket" );
	Check( !keyDownState[K_MOUSE2] && downEvents[K_MOUSE2] == 1,
		"the press is not delivered again after resume" );
	Check( !AnyKeyDown(), "nothing is left down after resume" );
}

/* #291: a click that lands after the drain, as InputSprocket suspends */
static void IspPressDuringSuspend( void ) {
	Frame();
	ClearCounts();
	ispPressAtSuspend = K_MOUSE3;
	Suspend();
	Frame();
	Check( suspendCalls == 1, "the suspend event suspends InputSprocket" );
	Resume();
	Frame();
	Frame();
	Check( flushCalls > 0, "the button queues are flushed on resume" );
	Check( !downEvents[K_MOUSE3] && !keyDownState[K_MOUSE3],
		"a press buffered across the suspend is not delivered after resume" );
	Check( !AnyKeyDown(), "nothing is left down after resume" );
}

/* #291: suspend and resume without InputSprocket (in_nomouse, or no ISp) */
static void NoInputSprocket( void ) {
	IspEvent( K_MOUSE2, 1 );
	Frame();
	Suspend();
	Frame();
	Resume();
	Frame();
	Frame();
	Check( !suspendCalls && !resumeCalls, "InputSprocket is not suspended or resumed without it" );
	Check( flushCalls == 0, "nothing is flushed without InputSprocket" );
	Check( cursorLevel == 0, "the cursor stays visible without InputSprocket" );
	Check( !downEvents[K_MOUSE2] && !AnyKeyDown(), "no button events without InputSprocket" );
}

/* #291: in_nomouse set while playing shuts input down, then a Cmd-Tab */
static void CursorAfterShutdown( void ) {
	Frame();
	noMouseCvar.integer = 1;
	Frame();
	Check( !inputActive && cursorLevel == 0, "shutting input down shows the cursor" );
	Suspend();
	Frame();
	Resume();
	Frame();
	Frame();
	Check( cursorLevel == 0, "the cursor stays visible after a suspend and resume" );
	Check( suspendCalls == 0 && resumeCalls == 0, "InputSprocket is not suspended or resumed after shutdown" );
}

/* #18: a key release and then more events than the queue holds */
static void Overflow( void ) {
	int i, *payload;

	Frame();
	Sys_QueEvent( 0, SE_KEY, K_SPACE, qtrue, 0, NULL );
	Frame();
	Check( keyDownState[K_SPACE], "Space is down" );
	printed[0] = 0;
	ClearCounts();
	Sys_QueEvent( 0, SE_KEY, K_SPACE, qfalse, 0, NULL );
	for ( i = 1 ; i <= 300 ; i++ ) {
		payload = Z_Malloc( sizeof( *payload ) );
		*payload = i;
		Sys_QueEvent( 0, SE_PACKET, 0, 0, sizeof( *payload ), payload );
	}
	Frame();
	Check( !keyDownState[K_SPACE] && upEvents[K_SPACE] == 1, "the queued Space release survives the overflow" );
	Check( zoneAllocs == zoneFrees, "every packet payload is freed, delivered or dropped" );
	Check( lastPacket == 300, "the newest packet is kept" );
	Check( packetsSeen == 254, "the queue keeps 255 events" );
	Check( strstr( printed, "Sys_QueEvent: overflow, dropped 46 events\n" ) != NULL,
		"the overflow is reported with the number dropped" );
	Check( Occurrences( printed, "overflow" ) == 1, "the overflow is reported once" );
	Frame();
	Check( !AnyKeyDown(), "nothing is left down" );
}

/* #18: keys mashed while the engine is not draining the queue */
static void OverflowReleases( void ) {
	int i;

	Frame();
	Sys_QueEvent( 0, SE_KEY, 'a', qtrue, 0, NULL );
	Sys_QueEvent( 0, SE_KEY, 'b', qtrue, 0, NULL );
	Frame();
	Check( keyDownState['a'] && keyDownState['b'], "A and B are down" );
	/* A released, then B released over and over: a queue of releases only */
	Sys_QueEvent( 0, SE_KEY, 'a', qfalse, 0, NULL );
	for ( i = 0 ; i < 300 ; i++ ) {
		Sys_QueEvent( 0, SE_KEY, 'b', qfalse, 0, NULL );
	}
	Frame();
	Check( !keyDownState['a'], "A's release survives a queue full of releases" );
	Check( !keyDownState['b'], "B is released" );
	/* and B pressed and released 150 times */
	Sys_QueEvent( 0, SE_KEY, 'a', qtrue, 0, NULL );
	Frame();
	Sys_QueEvent( 0, SE_KEY, 'a', qfalse, 0, NULL );
	for ( i = 0 ; i < 150 ; i++ ) {
		Sys_QueEvent( 0, SE_KEY, 'b', qtrue, 0, NULL );
		Sys_QueEvent( 0, SE_KEY, 'b', qfalse, 0, NULL );
	}
	Frame();
	Check( !AnyKeyDown(), "nothing is left down after mashing keys" );
}


/* #17: mouse movement, as Sys_Input scales it */
#define MOVE( n )	( (UInt32)( (n) * 163 ) )

static void CheckMouse( fakeElement_t *x, fakeElement_t *y, fakeElement_t **buttons, int numButtons ) {
	int i;

	Check( inputActive && cursorLevel == -1, "a usable mouse activates input and hides the cursor" );
	Frame();
	mouseX = mouseY = 0;
	x->state = MOVE( 3 );
	y->state = MOVE( -2 );
	Frame();
	Check( mouseX == 3 && mouseY == 2, "movement comes from the X and Y elements" );
	for ( i = 0 ; i < numButtons ; i++ ) {
		ClearCounts();
		IspPress( buttons[i], 1 );
		Frame();
		Check( downEvents[K_MOUSE1 + i] == 1 && keyDownState[K_MOUSE1 + i], "each button presses its mouse key" );
		IspPress( buttons[i], 0 );
		Frame();
		Check( upEvents[K_MOUSE1 + i] == 1 && !keyDownState[K_MOUSE1 + i], "and releases it" );
	}
	Check( !AnyKeyDown(), "nothing is left down" );
}

/* #17: a mouse whose list is not X, Y and then the buttons */
static void IspShuffled( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[3];

	AddDevice( kISpDeviceClass_Keyboard );
	mouse = AddDevice( kISpDeviceClass_Mouse );
	buttons[1] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseTwo );
	AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Z );	/* a wheel */
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	buttons[2] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseThree );
	Sys_InitInput();
	CheckMouse( x, y, buttons, 3 );
}

/* The wheel (Delta_Z) turns into a press and release of K_MWHEELUP or
 * K_MWHEELDOWN, one per frame it moved; a second wheel is ignored. */
static void CheckWheel( fakeElement_t *wheel, int turn, int key ) {
	ClearCounts();
	mouseX = mouseY = 0;
	wheel->state = (UInt32)turn;
	Frame();
	Check( downEvents[key] == 1 && upEvents[key] == 1 && !keyDownState[key],
		"a wheel turn presses and releases its wheel key" );
	Check( mouseX == 0 && mouseY == 0, "the wheel is not movement" );
	Check( downEvents[key == K_MWHEELUP ? K_MWHEELDOWN : K_MWHEELUP] == 0, "and not the other" );
}

static void IspWheel( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *wheel, *second, *buttons[1];

	mouse = AddDevice( kISpDeviceClass_Mouse );
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	wheel = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Z );
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	second = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Z );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	Sys_InitInput();
	CheckMouse( x, y, buttons, 1 );
	CheckWheel( wheel, 0x10000, K_MWHEELUP );
	CheckWheel( wheel, -0x10000, K_MWHEELDOWN );
	CheckWheel( wheel, 3 * 0x10000, K_MWHEELUP );
	ClearCounts();
	second->state = 0x10000;
	mouseX = mouseY = 0;
	Frame();
	Check( !downEvents[K_MWHEELUP] && !downEvents[K_MWHEELDOWN], "a still wheel, or a second one, sends nothing" );
	Check( mouseX == 0 && mouseY == 0, "a second wheel is not movement either" );
	Check( !AnyKeyDown(), "nothing is left down" );
}

/* #17: more buttons than mouse keys */
static void IspManyButtons( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[300];
	int				i;

	mouse = AddDevice( kISpDeviceClass_Mouse );
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	for ( i = 0 ; i < 300 ; i++ ) {
		buttons[i] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_None );
	}
	Sys_InitInput();
	CheckMouse( x, y, buttons, 5 );
	/* the buttons past the fifth have no mouse key */
	ClearCounts();
	for ( i = 5 ; i < 300 ; i++ ) {
		IspPress( buttons[i], 1 );
	}
	Frame();
	for ( i = 5 ; i < 300 ; i++ ) {
		IspPress( buttons[i], 0 );
	}
	Frame();
	Check( !AnyKeyDown(), "buttons past the fifth press no key" );
}

/* #17: a list longer than the buffer, and unlabeled movement */
static void IspOverCapacity( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[2];
	int				i;

	mouse = AddDevice( kISpDeviceClass_Mouse );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_None );
	for ( i = 0 ; i < 508 ; i++ ) {
		AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_None );
	}
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_None );
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_None );
	buttons[1] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_None );
	/* past the 512 that fit: these must not be seen */
	for ( i = 0 ; i < 100 ; i++ ) {
		AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
		AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	}
	Sys_InitInput();
	Check( strstr( printed, "clamping element list to 512 entries" ) != NULL, "the element count is clamped" );
	CheckMouse( x, y, buttons, 2 );
}

/* A driver whose movement is axes: retail took the first two elements whatever
 * they were, so that mouse must not be lost.  Delta elements still win
 * (IspOverCapacity lists axes before them). */
static void IspOtherKinds( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[1];

	mouse = AddDevice( kISpDeviceClass_Mouse );
	x = AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_None );
	y = AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_None );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	Sys_InitInput();
	Check( strstr( printed, "axis element(s)" ) != NULL, "the log says the movement is axes" );
	CheckMouse( x, y, buttons, 1 );
}

/* Axes may be a joystick's: their resting value would turn the view, so a
 * device with them is not used beside a mouse, whatever the order. */
static void IspAxesBesideMouse( void ) {
	fakeDevice_t	*stick, *pad, *mouse;
	fakeElement_t	*x, *y, *buttons[1];

	stick = AddDevice( kISpDeviceClass_Mouse );
	AddElement( stick, kISpElementKind_Axis, kISpElementLabel_Axis_XAxis );
	AddElement( stick, kISpElementKind_Axis, kISpElementLabel_Axis_YAxis );
	pad = AddDevice( kISpDeviceClass_Mouse );
	AddElement( pad, kISpElementKind_DPad, kISpElementLabel_None );
	AddElement( pad, kISpElementKind_DPad, kISpElementLabel_None );
	mouse = AddDevice( kISpDeviceClass_Mouse );
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	Sys_InitInput();
	Check( numDevices == 1, "only the mouse is used" );
	Check( mouse->active && !stick->active && !pad->active, "only the mouse is activated" );
	Check( strstr( printed, "axis element(s)" ) == NULL, "no axis is used" );
	CheckMouse( x, y, buttons, 1 );
}

/* Labels decide among axes, and the Z axis is not movement. */
static void IspAxisLabels( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[1];

	mouse = AddDevice( kISpDeviceClass_Mouse );
	AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_Axis_ZAxis );
	y = AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_Axis_YAxis );
	x = AddElement( mouse, kISpElementKind_Axis, kISpElementLabel_Axis_XAxis );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	Sys_InitInput();
	CheckMouse( x, y, buttons, 1 );
}

/* Only axes stand in for movement. */
static void IspDPadOnly( void ) {
	fakeDevice_t	*pad;

	pad = AddDevice( kISpDeviceClass_Mouse );
	AddElement( pad, kISpElementKind_DPad, kISpElementLabel_None );
	AddElement( pad, kISpElementKind_DPad, kISpElementLabel_None );
	Sys_InitInput();
	Check( !numDevices && !inputActive, "a device with no movement is not a mouse" );
	Check( cursorLevel == 0, "the cursor stays" );
}

/* #17: more devices than the buffer, and mice that cannot be used */
static void IspManyDevices( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[1];
	int				i;

	for ( i = 0 ; i < 40 ; i++ ) {
		AddDevice( kISpDeviceClass_Keyboard );
	}
	/* no elements; a lone button; Y movement only */
	AddDevice( kISpDeviceClass_Mouse );
	mouse = AddDevice( kISpDeviceClass_Mouse );
	AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	mouse = AddDevice( kISpDeviceClass_Mouse );
	AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
	/* a usable one */
	mouse = AddDevice( kISpDeviceClass_Mouse );
	x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Cursor_X );
	y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Cursor_Y );
	buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_None );
	for ( i = 0 ; i < 110 ; i++ ) {
		AddDevice( kISpDeviceClass_Mouse );
	}
	Sys_InitInput();
	Check( numDevices == 1, "only the usable mouse is used" );
	Check( mouse->active && !ispDevices[41].active && !ispDevices[42].active,
		"only the usable mouse is activated" );
	CheckMouse( x, y, buttons, 1 );
}

/* #17: InputSprocket failing on some mice, then on every mouse */
static void IspErrors( void ) {
	fakeDevice_t	*mouse;
	fakeElement_t	*x, *y, *buttons[1];
	int				i;

	for ( i = 0 ; i < 3 ; i++ ) {
		mouse = AddDevice( kISpDeviceClass_Mouse );
		x = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
		y = AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_Y );
		buttons[0] = AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
	}
	ispDevices[0].listErr = -50;
	ispDevices[1].extractErr = -50;
	Sys_InitInput();
	Check( numDevices == 1 && mouse->active, "the mouse InputSprocket works with is used" );
	CheckMouse( x, y, buttons, 1 );
}

static void IspNoUsableMouse( qboolean activateFails ) {
	fakeDevice_t	*mouse;

	AddDevice( kISpDeviceClass_Keyboard );
	if ( activateFails ) {
		DefaultDevices();
		ispDevices[2].activateErr = -50;
	} else {
		AddDevice( kISpDeviceClass_Mouse );
		mouse = AddDevice( kISpDeviceClass_Mouse );
		AddElement( mouse, kISpElementKind_Button, kISpElementLabel_Btn_MouseOne );
		AddElement( mouse, kISpElementKind_Delta, kISpElementLabel_Delta_X );
	}
	Sys_InitInput();
	Check( !inputActive && !ispStarted && shutdownCalls == 1,
		"without a usable mouse InputSprocket is shut down" );
	Check( cursorLevel == 0, "and the cursor is left shown" );
	/* the Event Manager's mouse button is the mouse now */
	Frame();
	ClearCounts();
	macModifiers = 0;	/* button down */
	Frame();
	Check( downEvents[K_MOUSE1] == 1, "the Event Manager's button presses mouse 1" );
	macModifiers = btnState;
	Frame();
	Check( upEvents[K_MOUSE1] == 1 && !AnyKeyDown(), "and releases it" );
	Suspend();
	Frame();
	Resume();
	Frame();
	Check( cursorLevel == 0 && !suspendCalls && !resumeCalls, "suspend and resume leave InputSprocket alone" );
}

/* #19: the Finder's Quit Application */
static void AppleEventQuit( void ) {
	Check( numAEHandlers == 3, "three Apple Event handlers are installed" );
	Frame();
	Check( !cbufText[0], "nothing queued before the event" );
	PostAppleEvent( kCoreEventClass, kAEQuitApplication );
	Frame();
	Check( aeProcessed == 1, "the high-level event goes to AEProcessAppleEvent" );
	Check( lastAEResult == noErr, "Quit Application is handled" );
	Check( !strcmp( cbufText, "quit\n" ), "Quit Application queues the quit command (Com_Quit_f)" );
}

/* #19: Open Application and Open Documents are accepted and do nothing;
 * others are left unhandled */
static void AppleEventOpen( void ) {
	Check( numAEHandlers == 3, "three Apple Event handlers are installed" );
	PostAppleEvent( kCoreEventClass, kAEOpenApplication );
	Frame();
	Check( aeProcessed == 1 && lastAEResult == noErr, "Open Application is handled" );
	PostAppleEvent( kCoreEventClass, kAEOpenDocuments );
	Frame();
	Check( aeProcessed == 2 && lastAEResult == noErr, "Open Documents is handled" );
	PostAppleEvent( kCoreEventClass, kAEPrintDocuments );
	Frame();
	Check( aeProcessed == 3 && lastAEResult == errAEEventNotHandled, "Print Documents is not" );
	Check( !cbufText[0], "none of them queues a command" );
}

/* #19: update events for the game window and the console window */
static void UpdateEvents( void ) {
	Frame();
	PostEvent( updateEvt, (long)gameWindow );
	Frame();
	Check( updatesBegun == 1 && updatesEnded == 1, "the game window's update is begun and ended" );
	Check( aglUpdates == 1, "and AGL is told" );
	Check( !consoleDrawn, "the console is not drawn for the game window" );
	Check( currentPort == &otherPortStorage, "the port is restored" );
	PostEvent( updateEvt, (long)consoleWindow );
	Frame();
	Check( updatesBegun == 2 && updatesEnded == 2, "the console window's update is begun and ended" );
	Check( consoleDrawn == consoleWindow, "the console window is redrawn" );
	Check( aglUpdates == 1, "AGL is not told about the console window" );
	Check( currentPort == &otherPortStorage, "the port is restored" );
}

/* #19: suspend and resume highlight the front window (doesActivateOnFGSwitch) */
static void SuspendHilite( void ) {
	Frame();
	Suspend();
	Frame();
	Check( hilites == 1 && !hilited, "the front window is unhighlighted on suspend" );
	Resume();
	Frame();
	Check( hilites == 2 && hilited, "and highlighted on resume" );
}

/* #19: the renderer's Sys_PumpEvents pumps */
static void PumpEvents( void ) {
	Frame();
	ClearCounts();
	PostEvent( keyDown, ( 0x0D << 8 ) | 'w' );	/* the W key */
	Sys_PumpEvents();
	Check( scriptedTail == scriptedHead, "Sys_PumpEvents takes the Event Manager's event" );
	IspEvent( K_MOUSE1, 1 );
	Sys_PumpEvents();
	Check( !defaultButtons[0]->queued, "Sys_PumpEvents takes InputSprocket's button event" );
	/* the queue holds them until the engine reads it */
	Check( !downEvents['w'] && !downEvents[K_MOUSE1], "nothing is delivered yet" );
	Frame();
	Check( downEvents['w'] == 1 && downEvents[K_MOUSE1] == 1, "Sys_PumpEvents queued the key and the button" );
}

/* #21: type text at the console, a frame per key, as the Mac delivers them */
static void Type( const char *text, int keyCode ) {
	for ( ; *text ; text++ ) {
		PostEvent( keyDown, ( keyCode << 8 ) | (unsigned char)*text );
		Frame();
	}
}

static void ConsoleDedicated( void ) {
	dedicatedCvar.integer = 1;
	Frame();
	ClearCounts();
	Type( "statux\bs\r", 0x24 );
	Check( !strcmp( consoleLines, "status\n" ), "a line typed at the console is one console command" );
	Type( "map q3dm1\r", 0x24 );
	Type( "\r", 0x24 );		/* an empty line */
	Type( "quit", 0x24 );
	Check( !strcmp( consoleLines, "status\nmap q3dm1\n\n" ) && consoleEvents == 3,
		"each line is a command once it is entered" );
	Type( "\b\b\b\b\b\b", 0x33 );	/* deletes quit, and no further */
	Type( "\r", 0x24 );
	Check( !strcmp( consoleLines, "status\nmap q3dm1\n\n\n" ) && consoleEvents == 4,
		"Delete takes back the line, and stops at its start" );
	Check( !strcmp( echoed, "statux\033[D \033[Ds\nmap q3dm1\n\nquit"
		"\033[D \033[D\033[D \033[D\033[D \033[D\033[D \033[D\n" ), "what is typed is echoed" );
	Check( !AnyKeyDown() && !downEvents[K_ENTER] && !downEvents[K_BACKSPACE], "the console keys reach no game" );
	/* Command-Q is still the menus' */
	macModifiers = btnState | cmdKey;
	PostEvent( keyDown, ( 0x0C << 8 ) | 'q' );
	Frame();
	Check( downEvents['q'] == 1 && consoleEvents == 4, "a command key is not typed at the console" );
}

static void ConsoleLong( void ) {
	static char text[1201];
	int length;

	dedicatedCvar.integer = 1;
	memset( text, 'x', 1200 );
	Type( text, 0x07 );
	Type( "\r", 0x24 );
	length = strlen( consoleLines );
	Check( consoleEvents == 1 && length == 1024 && consoleLines[1023] == '\n',
		"a line longer than the ring is cut to 1023 characters" );
	echoed[0] = consoleLines[0] = 0;
	Type( "status\r", 0x24 );
	Check( !strcmp( consoleLines, "status\n" ) && !strcmp( echoed, "status\n" ), "and the next line is whole" );
}

static void ConsoleClient( void ) {
	Frame();
	ClearCounts();
	Type( "a", 0x00 );
	Type( "\r", 0x24 );
	Check( consoleEvents == 0 && !echoed[0], "a game that is not dedicated keeps its keys" );
	Check( downEvents['a'] == 1, "and gets them as key events" );
}

static void WaitCancel( void ) {
	qboolean cancel;

	cancel = qfalse;
	Check( !Sys_WaitEvent( 7, &cancel ) && lastSleep == 7 && !cancel, "with no event the wait sleeps as asked" );
	PostEvent( keyDown, ( 0x00 << 8 ) | 'a' );
	Check( Sys_WaitEvent( 3, &cancel ) && lastSleep == 3 && !cancel, "a key is an event, not a cancel" );
	PostEvent( keyDown, ( 0x35 << 8 ) | 27 );
	Check( Sys_WaitEvent( 3, &cancel ) && cancel, "Esc cancels" );
	cancel = qfalse;
	macModifiers = btnState | cmdKey;
	PostEvent( keyDown, ( 0x2F << 8 ) | '.' );
	Check( Sys_WaitEvent( 3, &cancel ) && cancel, "Command-period cancels" );
	macModifiers = btnState;
	PostEvent( keyDown, ( 0x35 << 8 ) | 27 );
	Check( Sys_WaitEvent( 3, NULL ), "a wait that cannot be cancelled takes Esc as a key" );
	Sys_SendKeyEvents();
	Check( lastSleep == 0, "Sys_SendKeyEvents does not sleep" );
}

int main( int argc, char **argv ) {
	if ( argc != 2 ) {
		fprintf( stderr, "usage: %s case\n", argv[0] );
		return 2;
	}
	currentCase = argv[1];
	Sys_InitAppleEvents();	/* as Sys_Init */

	if ( !strncmp( currentCase, "isp-", 4 ) && strcmp( currentCase, "isp-press-at-suspend" )
			&& strcmp( currentCase, "isp-press-during-suspend" ) ) {
		/* the case sets up its own devices */
	} else {
		if ( !strcmp( currentCase, "no-isp" ) ) {
			noMouseCvar.integer = 1;
		}
		DefaultDevices();
		Sys_InitInput();
		if ( !strcmp( currentCase, "no-isp" ) ) {
			Check( !inputActive && cursorLevel == 0, "in_nomouse leaves input off and the cursor shown" );
		} else {
			Check( inputActive && cursorLevel == -1, "Sys_InitInput found the mouse and hid the cursor" );
		}
	}

	if ( !strcmp( currentCase, "modifier-alone" ) ) {
		ModifierAlone( qfalse );
	} else if ( !strcmp( currentCase, "modifier-alone-fullscreen" ) ) {
		ModifierAlone( qtrue );
	} else if ( !strcmp( currentCase, "background-modifiers" ) ) {
		BackgroundModifiers();
	} else if ( !strcmp( currentCase, "suspend-releases" ) ) {
		SuspendReleases();
	} else if ( !strcmp( currentCase, "isp-press-at-suspend" ) ) {
		IspPressAtSuspend();
	} else if ( !strcmp( currentCase, "isp-press-during-suspend" ) ) {
		IspPressDuringSuspend();
	} else if ( !strcmp( currentCase, "no-isp" ) ) {
		NoInputSprocket();
	} else if ( !strcmp( currentCase, "cursor-after-shutdown" ) ) {
		CursorAfterShutdown();
	} else if ( !strcmp( currentCase, "overflow" ) ) {
		Overflow();
	} else if ( !strcmp( currentCase, "overflow-releases" ) ) {
		OverflowReleases();
	} else if ( !strcmp( currentCase, "isp-shuffled" ) ) {
		IspShuffled();
	} else if ( !strcmp( currentCase, "isp-wheel" ) ) {
		IspWheel();
	} else if ( !strcmp( currentCase, "isp-many-buttons" ) ) {
		IspManyButtons();
	} else if ( !strcmp( currentCase, "isp-other-kinds" ) ) {
		IspOtherKinds();
	} else if ( !strcmp( currentCase, "isp-axes-beside-mouse" ) ) {
		IspAxesBesideMouse();
	} else if ( !strcmp( currentCase, "isp-axis-labels" ) ) {
		IspAxisLabels();
	} else if ( !strcmp( currentCase, "isp-dpad-only" ) ) {
		IspDPadOnly();
	} else if ( !strcmp( currentCase, "isp-over-capacity" ) ) {
		IspOverCapacity();
	} else if ( !strcmp( currentCase, "isp-many-devices" ) ) {
		IspManyDevices();
	} else if ( !strcmp( currentCase, "isp-errors" ) ) {
		IspErrors();
	} else if ( !strcmp( currentCase, "isp-no-usable-mouse" ) ) {
		IspNoUsableMouse( qfalse );
	} else if ( !strcmp( currentCase, "isp-activate-fails" ) ) {
		IspNoUsableMouse( qtrue );
	} else if ( !strcmp( currentCase, "apple-event-quit" ) ) {
		AppleEventQuit();
	} else if ( !strcmp( currentCase, "apple-event-open" ) ) {
		AppleEventOpen();
	} else if ( !strcmp( currentCase, "update-events" ) ) {
		UpdateEvents();
	} else if ( !strcmp( currentCase, "suspend-hilite" ) ) {
		SuspendHilite();
	} else if ( !strcmp( currentCase, "pump-events" ) ) {
		PumpEvents();
	} else if ( !strcmp( currentCase, "console-dedicated" ) ) {
		ConsoleDedicated();
	} else if ( !strcmp( currentCase, "console-long" ) ) {
		ConsoleLong();
	} else if ( !strcmp( currentCase, "console-client" ) ) {
		ConsoleClient();
	} else if ( !strcmp( currentCase, "wait-cancel" ) ) {
		WaitCancel();
	} else {
		fprintf( stderr, "unknown case %s\n", currentCase );
		return 2;
	}
	if ( failures ) {
		return 1;
	}
	printf( "%s: ok\n", currentCase );
	return 0;
}
