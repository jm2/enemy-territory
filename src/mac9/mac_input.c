
#include "../client/client.h"
#include "mac_local.h"
#include "InputSprocket.h"

qboolean			inputActive;
qboolean			inputSuspended;
qboolean			inputSystemSuspended;	// set by DoOSEvent on suspend/resume

#define	MAX_INPUT_DEVICES	100
ISpDeviceReference	devices[MAX_INPUT_DEVICES];

#define	MAX_ELEMENTS	512
#define	MAX_MOUSE_DEVICES	2
#define	MAX_MOUSE_BUTTONS	5	// K_MOUSE1 .. K_MOUSE5

// The elements Sys_InitInput found on a mouse, by kind and label (#17): an
// element list need not be X, Y and then the buttons, and a mouse can list
// more buttons than there are mouse keys.
typedef struct {
	ISpElementReference	xAxis, yAxis;
	ISpElementReference	wheel;			// NULL: no wheel
	ISpElementReference	buttons[MAX_MOUSE_BUTTONS];	// NULL: no such button
} macMouse_t;

UInt32				numDevices;		// mice in use, in mice[]
macMouse_t			mice[MAX_MOUSE_DEVICES];
static ISpElementReference	elements[MAX_ELEMENTS];

cvar_t				*in_nomouse;

void Input_Init(void);
void Input_GetState( void );

/*
=================
Sys_InitMouse

Finds a mouse's X and Y movement and its buttons by element kind and label,
and activates it.  Returns qfalse, leaving the device inactive, if
InputSprocket fails or the mouse has no X and Y movement.
=================
*/
static qboolean Sys_InitMouse( ISpDeviceReference device, macMouse_t *mouse, qboolean useAxes ) {
	ISpElementListReference	elementList;
	ISpElementInfo		info;
	ISpElementReference	deltas[2];		// other movement, in list order
	ISpElementReference	axisX, axisY;	// axes labeled X and Y
	ISpElementReference	axes[2];		// unlabeled axes, in list order
	ISpElementReference	others[MAX_MOUSE_BUTTONS];	// buttons not labeled mou1-3
	UInt32				count;
	int					i, button, numDeltas, numAxes, numOthers, used;
	OSStatus			err;

	memset( mouse, 0, sizeof( *mouse ) );

	err = ISpDevice_GetElementList( device, &elementList );
	if ( err ) {
		Com_Printf( "ISpDevice_GetElementList failed: %i\n", (int)err );
		return qfalse;
	}
	err = ISpElementList_Extract( elementList, MAX_ELEMENTS, &count, elements );
	if ( err ) {
		Com_Printf( "ISpElementList_Extract failed: %i\n", (int)err );
		return qfalse;
	}
	Com_Printf("%i elements in list\n", (int)count );
	if ( count > MAX_ELEMENTS ) {
		Com_Printf( "clamping element list to %i entries\n", MAX_ELEMENTS );
		count = MAX_ELEMENTS;
	}

	axisX = axisY = 0;
	numDeltas = numAxes = numOthers = 0;
	for ( i = 0 ; i < count ; i++ ) {
		if ( ISpElement_GetInfo( elements[i], &info ) ) {
			continue;
		}
		PStringToCString( (char *)info.theString );
		Com_Printf( "%i : %s (kind 0x%x, label 0x%x)\n", i, info.theString,
			(unsigned)info.theKind, (unsigned)info.theLabel );

		if ( info.theKind == kISpElementKind_Delta ) {
			if ( ( info.theLabel == kISpElementLabel_Delta_X
				|| info.theLabel == kISpElementLabel_Delta_Cursor_X ) && !mouse->xAxis ) {
				mouse->xAxis = elements[i];
			} else if ( ( info.theLabel == kISpElementLabel_Delta_Y
				|| info.theLabel == kISpElementLabel_Delta_Cursor_Y ) && !mouse->yAxis ) {
				mouse->yAxis = elements[i];
			} else if ( info.theLabel == kISpElementLabel_Delta_Z ) {
				// the wheel: retail ignored it, so it never reached the
				// K_MWHEELUP and K_MWHEELDOWN binds
				if ( !mouse->wheel ) {
					mouse->wheel = elements[i];
				}
			} else if ( numDeltas < 2 ) {
				deltas[numDeltas++] = elements[i];
			}
		} else if ( info.theKind == kISpElementKind_Button ) {
			if ( info.theLabel == kISpElementLabel_Btn_MouseOne ) {
				button = 0;
			} else if ( info.theLabel == kISpElementLabel_Btn_MouseTwo ) {
				button = 1;
			} else if ( info.theLabel == kISpElementLabel_Btn_MouseThree ) {
				button = 2;
			} else {
				button = -1;
			}
			if ( button >= 0 && !mouse->buttons[button] ) {
				mouse->buttons[button] = elements[i];
			} else if ( numOthers < MAX_MOUSE_BUTTONS ) {
				others[numOthers++] = elements[i];
			}
		} else if ( useAxes && info.theKind == kISpElementKind_Axis ) {
			if ( info.theLabel == kISpElementLabel_Axis_XAxis && !axisX ) {
				axisX = elements[i];
			} else if ( info.theLabel == kISpElementLabel_Axis_YAxis && !axisY ) {
				axisY = elements[i];
			} else if ( info.theLabel == kISpElementLabel_None && numAxes < 2 ) {
				axes[numAxes++] = elements[i];
			}
		}
	}

	// Movement not labeled X or Y (other than the wheel) is X then Y, as
	// retail assumed of the first two elements.
	i = 0;
	if ( !mouse->xAxis && i < numDeltas ) {
		mouse->xAxis = deltas[i++];
	}
	if ( !mouse->yAxis && i < numDeltas ) {
		mouse->yAxis = deltas[i++];
	}
	// Retail did not look at the kind at all, so a driver that reports its
	// movement as axes worked there: without this it would lose the mouse,
	// and the menus with it.  Unlabeled axes are X then Y.
	i = used = 0;
	if ( !mouse->xAxis && axisX ) {
		mouse->xAxis = axisX;
		used++;
	} else if ( !mouse->xAxis && i < numAxes ) {
		mouse->xAxis = axes[i++];
		used++;
	}
	if ( !mouse->yAxis && axisY ) {
		mouse->yAxis = axisY;
		used++;
	} else if ( !mouse->yAxis && i < numAxes ) {
		mouse->yAxis = axes[i++];
		used++;
	}
	if ( used ) {
		Com_Printf( "using %i axis element(s) for mouse movement\n", used );
	}
	if ( !mouse->xAxis || !mouse->yAxis ) {
		Com_Printf( "no X and Y movement, skipping this device\n" );
		return qfalse;
	}

	// the other buttons, in list order, take the mouse keys left over;
	// any beyond K_MOUSE5 are ignored
	button = 0;
	for ( i = 0 ; i < numOthers ; i++ ) {
		while ( button < MAX_MOUSE_BUTTONS && mouse->buttons[button] ) {
			button++;
		}
		if ( button == MAX_MOUSE_BUTTONS ) {
			break;
		}
		mouse->buttons[button] = others[i];
	}

	err = ISpDevices_Activate( 1, &device );
	if ( err ) {
		Com_Printf( "ISpDevices_Activate failed: %i\n", (int)err );
		return qfalse;
	}
	return qtrue;
}

/*
=================
Sys_InitInput
=================
*/
void Sys_InitInput( void ) {
	NumVersion		ver;
	UInt32			count;
	int				i, useAxes;
	OSStatus		err;
	
	numDevices = 0;

	// no input with dedicated servers
	if ( com_dedicated->integer ) {
		return;
	}
	
	Com_Printf( "------- Input Initialization -------\n" );
	in_nomouse = Cvar_Get( "in_nomouse", "0", 0 );
	if ( in_nomouse->integer != 0 ) {
		Com_Printf( "in_nomouse is set, skipping.\n" );
		Com_Printf( "------------------------------------\n" );
		return;
	}
	
	ver = ISpGetVersion();
	Com_Printf( "InputSprocket version: 0x%x\n", (unsigned)ver.majorRev << 24 | ver.minorAndBugRev << 16 | ver.stage << 8 | ver.nonRelRev );
		
	err = ISpStartup();
	if ( err ) {
		Com_Printf( "ISpStartup failed: %i\n", (int)err );
		Com_Printf( "------------------------------------\n" );
		return;
	}

	// disable everything
	err = ISpDevices_Extract( MAX_INPUT_DEVICES, &count, devices );
	if ( err ) {
		Com_Printf( "ISpDevices_Extract failed: %i\n", (int)err );
		goto fail;
	}
	Com_Printf("%i total devices\n", (int)count);
	if ( count > MAX_INPUT_DEVICES ) {
		count = MAX_INPUT_DEVICES;
	}
	err = ISpDevices_Deactivate( count, devices );
	if ( err ) {
		Com_Printf( "ISpDevices_Deactivate failed: %i\n", (int)err );
		goto fail;
	}
	
	// enable the first mice with X and Y movement
	err = ISpDevices_ExtractByClass(
			kISpDeviceClass_Mouse,
			MAX_INPUT_DEVICES,
			&count,
			devices);
	if ( err ) {
		Com_Printf( "ISpDevices_ExtractByClass failed: %i\n", (int)err );
		goto fail;
	}
	Com_Printf("%i mouse devices\n", (int)count);
	if ( count > MAX_INPUT_DEVICES ) {
		count = MAX_INPUT_DEVICES;
	}
	// axes are a last resort, as they may be a joystick's: never with a mouse
	for ( useAxes = 0 ; useAxes < 2 && !numDevices ; useAxes++ ) {
		for ( i = 0 ; i < count && numDevices < MAX_MOUSE_DEVICES ; i++ ) {
			if ( Sys_InitMouse( devices[i], &mice[numDevices], useAxes ) ) {
				numDevices++;
			}
		}
	}
	if ( !numDevices ) {
		Com_Printf( "no usable InputSprocket mouse\n" );
		goto fail;
	}
	
	inputActive = true;

	HideCursor();

	Com_Printf( "------------------------------------\n" );
	return;

fail:
	// nothing is active, and the cursor stays as it is
	ISpShutdown();
	numDevices = 0;
	Com_Printf( "------------------------------------\n" );
}

/*
=================
Sys_ShutdownInput
=================
*/
void Sys_ShutdownInput( void ) {
	if ( !inputActive ) {
		return;
	}
	ShowCursor();
	ISpShutdown();
	inputActive = qfalse;
	numDevices = 0;
}

void Sys_SuspendInput( void ) {
	if ( inputSuspended ) {
		return;
	}
	inputSuspended = true;
	// no InputSprocket calls without ISpStartup (in_nomouse, or it failed),
	// and no cursor change: Sys_InitInput only hid it if ISp started
	if ( inputActive ) {
		ShowCursor();
		ISpSuspend();
	}
}

void Sys_ResumeInput( void ) {
	int		device, button;

	if ( !inputSuspended ) {
		return;
	}
	inputSuspended = false;
	// no InputSprocket calls without ISpStartup (in_nomouse, or it failed),
	// and no cursor change: Sys_InitInput only hid it if ISp started
	if ( !inputActive ) {
		return;
	}
	HideCursor();
	ISpResume();

	// Discard button events buffered across the suspend (#291): a press
	// that came after DoOSEvent drained the queues was not released by
	// Sys_ReleaseKeys, and its release went to the front process.
	for ( device = 0 ; device < numDevices ; device++ ) {
		for ( button = 0 ; button < MAX_MOUSE_BUTTONS ; button++ ) {
			if ( mice[device].buttons[button] ) {
				ISpElement_Flush( mice[device].buttons[button] );
			}
		}
	}
}

/*
=================
Sys_Input
=================
*/
void Sys_Input( void ) {
	ISpElementEvent		event;
	Boolean				wasEvent;
	UInt32				state, state2;
	int					xmove, ymove;
	int					button;
	static int xtotal, ytotal;
	int					device;
	macMouse_t			*mouse;
	
	if ( !inputActive ) {
		return;
	}

	// during debugging it is sometimes usefull to be able to kill mouse support
	if ( in_nomouse->integer ) {
		Com_Printf( "Shutting down input.\n");
		Sys_ShutdownInput();
		return;
	}
	
	// always suspend for dedicated 
	if ( com_dedicated->integer ) {
		Sys_SuspendInput();
		return;
	}
	
	// temporarily deactivate if not in the game and
    // Antigravity: Input suspension disabled for Mac OS 9 compatibility
	// if ( cls.keyCatchers || cls.state != CA_ACTIVE ) {
	// 	if ( !glConfig.isFullscreen ) {
	// 		Sys_SuspendInput();
	// 		return;
	// 	}
	// }

	// while the app is suspended (cmd-tab away), leave ISp released
	if ( inputSystemSuspended ) {
		return;
	}

	Sys_ResumeInput();

	// send all button events
	for ( device = 0 ; device < numDevices ; device++ ) {
		mouse = &mice[device];

		// mouse buttons
		
		for ( button = 0 ; button < MAX_MOUSE_BUTTONS ; button++ ) {
			int eventCount = 0;
			if ( !mouse->buttons[button] ) {
				continue;
			}
			while ( 1 ) {
				if ( ISpElement_GetNextEvent( mouse->buttons[button], sizeof( event ), &event, &wasEvent )
					|| !wasEvent ) {
					break;
				}
				if ( event.data ) {
					Sys_QueEvent( 0, SE_KEY, K_MOUSE1 + button, 1, 0, NULL );
				} else {
					Sys_QueEvent( 0, SE_KEY, K_MOUSE1 + button, 0, 0, NULL );
				}
				// Safety break to prevent infinite loop. Checked AFTER
				// queueing: the old order discarded the event it had just
				// fetched, which could permanently latch a mouse button.
				eventCount++;
				if (eventCount > 50) {
					break;
				}
			}
		}
		
		// mouse movement
		
#define	MAC_MOUSE_SCALE		163		// why this constant?
		// send mouse event
		if ( ISpElement_GetSimpleState( mouse->xAxis, &state ) ) {
			state = 0;
		}
		xmove = (int)state / MAC_MOUSE_SCALE;
		
		if ( ISpElement_GetSimpleState( mouse->yAxis, &state2 ) ) {
			state2 = 0;
		}
		ymove = (int)state2 / -MAC_MOUSE_SCALE;
		
		if ( xmove || ymove ) {
			xtotal += xmove;
			ytotal += ymove;
			Sys_QueEvent( 0, SE_MOUSE, xmove, ymove, 0, NULL );
		}

		// The wheel's scale is not documented, so any turn since the last
		// frame is one notch, up when positive, as HID wheels count away
		// from the user.  A wheel key is pressed and released at once, as
		// on win32.
		if ( mouse->wheel && !ISpElement_GetSimpleState( mouse->wheel, &state ) && state ) {
			button = (int)state > 0 ? K_MWHEELUP : K_MWHEELDOWN;
			Sys_QueEvent( 0, SE_KEY, button, qtrue, 0, NULL );
			Sys_QueEvent( 0, SE_KEY, button, qfalse, 0, NULL );
		}
	}
	
}
