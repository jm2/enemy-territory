#include "../client/client.h"
#include "mac_local.h"

void DoMenuCommand(long	menuAndItem);
void DoDrag(WindowPtr	myWindow,Point	mouseloc);
void DoGoAwayBox(WindowPtr myWindow, Point mouseloc);
void DoCloseWindow(WindowPtr myWindow);
void DoKeyDown(EventRecord *event);
void DoDiskEvent(EventRecord *event);
void DoOSEvent(EventRecord *event);
void DoUpdate(WindowPtr myWindow);
void DoActivate(WindowPtr myWindow, int myModifiers);
void DoAboutBox(void);
void DoMenuCommand(long	menuAndItem);
void DoMouseDown(EventRecord *event);
void DoMouseUp(EventRecord *event);
void DoMenuAdjust(void);
void DoKeyUp(EventRecord *event);

/*
================
Sys_MsecForMacEvent

Q3 event records take time in msec,
so convert the mac event record when
(60ths) to msec.  The base values
are updated ever frame, so this
is guaranteed to not drift.
=================
*/
int	Sys_MsecForMacEvent( void ) {
	int		tics;
	
	tics = sys_lastEventTic - sys_ticBase;
	
	return sys_msecBase + tics * 16;
}




void DoMouseDown(EventRecord *event)
{	
	int			myPart;
	WindowPtr	myWindow;
	Point		point;

	myPart = FindWindow(event->where, &myWindow);
	
	switch(myPart)
	{
		case inMenuBar:
			DrawMenuBar();
			DoMenuCommand(MenuSelect(event->where));
		break;
		case inSysWindow:
			SystemClick(event, myWindow);
		break;
		case inDrag:
			DoDrag(myWindow, event->where);
			
			// update the vid_xpos / vid_ypos cvars
			point.h = 0;
			point.v = 0;
			LocalToGlobal( &point );
			Cvar_SetValue( "vid_xpos", point.h );
			Cvar_SetValue( "vid_ypos", point.v );
			return;
		break;
		case inGoAway:
			DoGoAwayBox(myWindow, event->where);
		break;

		case inContent:
			if (myWindow != FrontWindow())
			{
				SelectWindow(myWindow);
			}
		break;
	}
}

void DoMouseUp(EventRecord *event)
{
}

void DoDrag(WindowPtr myWindow, Point mouseloc)
{
	Rect	dragBounds;
	
	dragBounds = (**GetGrayRgn()).rgnBBox;
	DragWindow(myWindow,mouseloc,&dragBounds);

	aglUpdateContext(aglGetCurrentContext());
}


void DoGoAwayBox(WindowPtr myWindow, Point mouseloc)
{
	if(TrackGoAway(myWindow,mouseloc))
	{ 
		DoCloseWindow(myWindow); 
	}
}

void DoCloseWindow(WindowPtr myWindow)
{
}

void DoMenuAdjust(void)
{
}

// 0 = no Quake key for this virtual key (previously the literal '?', which
// made unmapped keys type/bind a question mark).
int	vkeyToQuakeKey[256] = {
/*0x00*/	'a', 's', 'd', 'f', 'h', 'g', 'z', 'x',
/*0x08*/	'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
/*0x10*/	'y', 't', '1', '2', '3', '4', '6', '5',
/*0x18*/	'=', '9', '7', '-', '8', '0', ']', 'o',
/*0x20*/	'u', '[', 'i', 'p', K_ENTER, 'l', 'j', '\'',
/*0x28*/	'k', ';', '\\', ',', '/', 'n', 'm', '.',
/*0x30*/	K_TAB, K_SPACE, '`', K_BACKSPACE, 0, K_ESCAPE, 0, K_COMMAND,
/*0x38*/	K_SHIFT, K_CAPSLOCK, K_ALT, K_CTRL, 0, 0, 0, 0,
/*0x40*/	0, K_KP_DEL, 0, K_KP_STAR, 0, K_KP_PLUS, 0, K_KP_NUMLOCK,
/*0x48*/	0, 0, 0, K_KP_SLASH, K_KP_ENTER, 0, K_KP_MINUS, 0,
/*0x50*/	0, K_KP_EQUALS, K_KP_INS, K_KP_END, K_KP_DOWNARROW, K_KP_PGDN, K_KP_LEFTARROW, K_KP_5,
/*0x58*/	K_KP_RIGHTARROW, K_KP_HOME, 0, K_KP_UPARROW, K_KP_PGUP, 0, 0, 0,
/*0x60*/	K_F5, K_F6, K_F7, K_F3, K_F8, K_F9, 0, K_F11,
/*0x68*/	0, K_F13, 0, K_F14, 0, K_F10, 0, K_F12,
/*0x70*/	0, K_F15, K_INS, K_HOME, K_PGUP, K_DEL, K_F4, K_END,
/*0x78*/	K_F2, K_PGDN, K_F1, K_LEFTARROW, K_RIGHTARROW, K_DOWNARROW, K_UPARROW, K_POWER
};

void DoKeyDown(EventRecord *event)
{
	int		myCharCode;
	int		myKeyCode;
	int		quakeKey;

	myCharCode	= BitAnd(event->message,charCodeMask);
	myKeyCode = ( event->message & keyCodeMask ) >> 8;

	quakeKey = vkeyToQuakeKey[ myKeyCode ];
	if ( quakeKey ) {
		Sys_QueEvent( Sys_MsecForMacEvent(), SE_KEY, quakeKey, 1, 0, NULL );
	}
	Sys_QueEvent( Sys_MsecForMacEvent(), SE_CHAR, myCharCode, 0, 0, NULL );
}

void DoKeyUp(EventRecord *event)
{
	int		myKeyCode;
	int		quakeKey;

	myKeyCode = ( event->message & keyCodeMask ) >> 8;

	quakeKey = vkeyToQuakeKey[ myKeyCode ];
	if ( quakeKey ) {
		Sys_QueEvent( Sys_MsecForMacEvent(), SE_KEY, quakeKey, 0, 0, NULL );
	}
}

/*
==================
Sys_ModifierEvents
==================
*/
void Sys_ModifierEvents( int modifiers ) {
	static int		oldModifiers;
	int				changed;
	int				i;

	typedef struct {
		int		bit;
		int		keyCode;
	} modifierKey_t;

	static modifierKey_t	keys[] = {
		{ 128, K_MOUSE1 },
		{ 256, K_COMMAND },
		{ 512, K_SHIFT },
		{1024, K_CAPSLOCK },
		{2048, K_ALT },
		{4096, K_CTRL },
		{-1, -1 }
	};
	
	changed = modifiers ^ oldModifiers;

	for ( i = 0 ; keys[i].bit != -1 ; i++ ) {
		// if we have input sprockets running, ignore mouse events we
		// get from the debug passthrough driver
		if ( inputActive && keys[i].keyCode == K_MOUSE1 ) {
			continue;
		}

		if ( changed & keys[i].bit ) {
			int down = !!( modifiers & keys[i].bit );
			// btnState (bit 0x80) is SET when the mouse button is UP,
			// opposite to the modifier-key bits.
			if ( keys[i].bit == 128 ) {
				down = !down;
			}
			Sys_QueEvent( Sys_MsecForMacEvent(),
			SE_KEY, keys[i].keyCode, down, 0, NULL );
		}
	}

	oldModifiers = modifiers;
}


void DoDiskEvent(EventRecord	*event)
{

}

void	DoOSEvent(EventRecord	*event)
{
	// Suspend/resume: release InputSprocket and show the cursor when the
	// user switches away (cmd-tab to Finder/console), re-grab on return.
	// Previously empty, which left the cursor hidden and ISp capturing
	// input while the game was in the background.
	if ( ( ( event->message >> 24 ) & 0xFF ) == suspendResumeMessage ) {
		// doesActivateOnFGSwitch (mac_resources.r): no activate events
		// come with suspend and resume, so (un)highlight the front window
		// here, or it stays highlighted in the background (#19).
		if ( FrontWindow() ) {
			HiliteWindow( FrontWindow(), ( event->message & resumeFlag ) != 0 );
		}
		if ( event->message & resumeFlag ) {
			inputSystemSuspended = qfalse;
			Sys_ResumeInput();
		} else {
			// The key-ups for anything held now go to the front process,
			// so release it all here or it stays down (#291).  Modifiers
			// first, so Sys_ModifierEvents does not release them again.
			// Drain InputSprocket first: a button pressed since the last
			// Sys_Input is not yet tracked, so would not be released.
			Sys_Input();
			inputSystemSuspended = qtrue;
			Sys_ModifierEvents( btnState );
			Sys_ReleaseKeys();
			Sys_SuspendInput();
		}
	}
}
// mac_event.c

static qboolean ignoreUpdateEvents = qfalse;

/*
==================
Apple Events (#19)

The SIZE resource declares the application high-level event aware, so the
Finder sends it the required Apple Events: Open Application at launch, Open
Documents for files dropped on it, and Quit Application from its Shutdown and
Restart.  Retail installed none, so Shutdown could not quit a running game.
These run inside AEProcessAppleEvent, which Sys_SendKeyEvents calls,
possibly from inside the renderer (GLimp_EndFrame, Sys_PumpEvents), so Quit
only queues the quit command, which shuts the game down as the console's
quit does (Com_Quit_f) once the frame is over, and the reply is sent.
==================
*/
static pascal OSErr Sys_AEOpenApplication( const AppleEvent *event, AppleEvent *reply, long refcon ) {
	return noErr;
}

static pascal OSErr Sys_AEOpenDocuments( const AppleEvent *event, AppleEvent *reply, long refcon ) {
	// the game data comes from baseq3, not from documents
	Com_Printf( "Ignoring the Finder's Open Documents\n" );
	return noErr;
}

static pascal OSErr Sys_AEQuitApplication( const AppleEvent *event, AppleEvent *reply, long refcon ) {
	Cbuf_ExecuteText( EXEC_APPEND, "quit\n" );
	return noErr;
}

static void Sys_InstallAppleEventHandler( AEEventID id, AEEventHandlerProcPtr handler ) {
	OSErr	err;

	err = AEInstallEventHandler( kCoreEventClass, id, NewAEEventHandlerUPP( handler ), 0, false );
	if ( err ) {
		Com_Printf( "AEInstallEventHandler failed: %i\n", (int)err );
	}
}

void Sys_InitAppleEvents( void ) {
	Sys_InstallAppleEventHandler( kAEOpenApplication, Sys_AEOpenApplication );
	Sys_InstallAppleEventHandler( kAEOpenDocuments, Sys_AEOpenDocuments );
	Sys_InstallAppleEventHandler( kAEQuitApplication, Sys_AEQuitApplication );
}

//
// DoUpdate
//
void DoUpdate(WindowPtr	myWindow)
{
	GrafPtr		origPort;
	AGLContext	ctx;

    if ( !myWindow ) {
        return;
    }

	GetPort(&origPort);
	SetPort(myWindow);
	BeginUpdate(myWindow);
	// The game window is redrawn every frame, but the console window only
	// when written to: redraw it here, or a part uncovered by another
	// window stays blank (#19).
	if ( (void*)myWindow != (void*)sys_gl.drawable ) {
		Sys_ConsoleDraw( myWindow );
	}
	EndUpdate(myWindow);

	// Keep AGL in sync when the game window itself took the update. This
	// was disabled while event.message held garbage window pointers; that
	// was the mac68k struct-packing bug, fixed at the header level now.
	ctx = aglGetCurrentContext();
	if (ctx != NULL && (void*)myWindow == (void*)sys_gl.drawable) {
		aglUpdateContext(ctx);
	}

	SetPort(origPort);
}

// Nothing to do: the Window Manager highlights the window, the game window
// has no controls and is redrawn every frame, and suspend and resume (which
// do not send activate events, as the SIZE resource sets
// doesActivateOnFGSwitch) are handled by DoOSEvent.
void DoActivate( WindowPtr myWindow, int myModifiers) {

}

void DoAboutBox( void ) {
	DialogPtr	myDialog;
	short		itemHit;

	myDialog = GetNewDialog(kAboutDialog, nil, (WindowPtr) -1);
	ModalDialog(nil, &itemHit);
	DisposeDialog(myDialog);
}

void DoMenuCommand( long menuAndItem ) {
	int			myMenuNum;
	int			myItemNum;
	int			myResult;
	Str255		myDAName;
	WindowPtr	myWindow;
	
	myMenuNum	= HiWord(menuAndItem);
	myItemNum	= LoWord(menuAndItem);
	
	GetPort(&myWindow);
	
	switch (myMenuNum)  {
	case mApple:
		switch( myItemNum ) {
		case iAbout: 
			DoAboutBox();
			break;
		default: 
			GetMenuItemText(GetMenuHandle(mApple), myItemNum, myDAName);
			myResult = OpenDeskAcc(myDAName);
			break;
		}
		break;
	case mFile:
		switch (myItemNum) {
		case iQuit:
			Com_Quit_f();
			break;
		}
		break;
	}
	
	HiliteMenu(0);
}

void TestTime( EventRecord *ev ) {
	int		msec;
	int		tics;
	static int startTics, startMsec;
	
	msec = Sys_Milliseconds();
	tics = ev->when;
	
	if ( !startTics || ev->what == mouseDown ) {
		startTics = tics;
		startMsec = msec;
	}
	
	msec -= startMsec;
	tics -= startTics;
	
	if ( !tics ) {
		return;
	}
	Com_Printf( "%i msec to tic\n", msec / tics );
}

/*
==================
Sys_SendKeyEvents
==================
*/
void Sys_SendKeyEvents (void) {
	Sys_WaitEvent( 0, NULL );
}

/*
==================
Sys_WaitEvent

Handles one Mac event as Sys_SendKeyEvents does, but lets WaitNextEvent sleep
up to sleepTicks for it, giving the time to other processes: NET_Sleep and
the name lookup in Sys_StringToAdr wait here.  Returns qtrue when an event
was handled.  *cancel, when given, is set by Esc or Command-period.
==================
*/
qboolean Sys_WaitEvent( long sleepTicks, qboolean *cancel ) {
	Boolean		   gotEvent;
	EventRecord	   event;
    EventMask      mask = everyEvent;
	int			   c;

    if (ignoreUpdateEvents) {
        mask &= ~updateMask;
    }
	
	//Sys_LogPrintf("Sys_SendKeyEvents: Start\n");
	if ( !glConfig.isFullscreen || sys_waitNextEvent->value ) {
		// this call involves 68k code and task switching.
		// do it on the desktop, or if they explicitly ask for
		// it when fullscreen
		gotEvent = WaitNextEvent(mask, &event, sleepTicks, nil);
	} else {
		gotEvent = GetOSEvent( mask, &event );
	}
	//Sys_LogPrintf( "Sys_SendKeyEvents: Event check done, gotEvent=%d what=%d\n", gotEvent, event.what );

	// When no event is pending, WaitNextEvent/GetOSEvent return false with a
	// null event whose when/where/modifiers fields are still filled in.
	// Classic Mac OS posts no keyDown/keyUp for modifier keys, so their state
	// must be sampled on every poll, including null events, or Ctrl/Shift/Alt
	// presses and releases are only noticed when some other event arrives.
	// In the background (windowed, canBackground) null events carry the
	// system-wide modifiers, so hold them all up until resume (#291).
	Sys_ModifierEvents( inputSystemSuspended ? btnState : event.modifiers );
	sys_lastEventTic = event.when;

	if ( !gotEvent ) {
		return qfalse;
	}
    
    //Sys_LogPrintf("Sys_SendKeyEvents: Processing event types=%d\n", event.what);

	if ( cancel && event.what == keyDown ) {
		c = event.message & charCodeMask;
		if ( c == 27 || ( c == '.' && ( event.modifiers & cmdKey ) ) ) {
			*cancel = qtrue;
		}
	}

	if ( Sys_ConsoleEvent(&event) ) {
		return qtrue;
	}
	switch(event.what)
	{
		case mouseDown:
            //Sys_LogPrintf("Sys_SendKeyEvents: mouseDown\n");
			DoMouseDown(&event);
		break;
		case mouseUp:
            //Sys_LogPrintf("Sys_SendKeyEvents: mouseUp\n");
			DoMouseUp(&event);
		break;
		case keyDown:
            //Sys_LogPrintf("Sys_SendKeyEvents: keyDown\n");
			DoKeyDown(&event);
		break;
		case keyUp:
            //Sys_LogPrintf("Sys_SendKeyEvents: keyUp\n");
			DoKeyUp(&event);
		break;
		case autoKey:
            //Sys_LogPrintf("Sys_SendKeyEvents: autoKey\n");
			DoKeyDown(&event);
		break;
		case updateEvt:
            Sys_LogPrintf("Sys_SendKeyEvents: updateEvt\n");
			DoUpdate((WindowPtr) event.message);
            Sys_LogPrintf("Sys_SendKeyEvents: updateEvt done\n");
		break;
		case diskEvt:
            //Sys_LogPrintf("Sys_SendKeyEvents: diskEvt\n");
			DoDiskEvent(&event);
		break;
		case activateEvt:
            Sys_LogPrintf("Sys_SendKeyEvents: activateEvt\n");
			DoActivate((WindowPtr) event.message, event.modifiers);
            Sys_LogPrintf("Sys_SendKeyEvents: activateEvt done\n");
		break;
		case osEvt:
            Sys_LogPrintf("Sys_SendKeyEvents: osEvt\n");
			DoOSEvent(&event);
            Sys_LogPrintf("Sys_SendKeyEvents: osEvt done\n");
		break;
		case kHighLevelEvent:
			// the Finder's Apple Events, to Sys_InitAppleEvents' handlers;
			// any other is answered errAEEventNotHandled
			AEProcessAppleEvent(&event);
		break;
		default:
            //Sys_LogPrintf("Sys_SendKeyEvents: default %d\n", event.what);
		break;
	}
    //Sys_LogPrintf("Sys_SendKeyEvents: Done\n");
	return qtrue;
}
