
// typedef int sysEventType_t;	// FIXME...
#include "../renderer/tr_local.h"
#include "mac_local.h"
#include <glm.h>
#include "MacGamma.h"

// MAX_DEVICES and macGlInfo moved to mac_local.h

cvar_t			*r_device;
cvar_t			*r_ext_transform_hint;
cvar_t			*r_ext_texture_filter_anisotropic;

#ifndef GLM_PAGE_SIZE
#define GLM_PAGE_SIZE 1
#endif
GLint glmGetInteger( GLenum param ) { return 4096; }
glHardwareType_t		sys_hardwareType;
macGlInfo		sys_gl;

void GLimp_EndFrame( void );
static void GLimp_Extensions( void );


void CToPStr(char *cs, Str255 ps)
{
	GLint i, l;
	
	l = strlen(cs);
	if(l > 255) l = 255;
	ps[0] = l;
	for(i = 0; i < l; i++) ps[i + 1] = cs[i];
}

/*
============
CheckErrors
============
*/
void CheckErrors( void ) {		
	GLenum   err;

	err = aglGetError();
	if( err != AGL_NO_ERROR ) {
		ri.Error( ERR_FATAL, "aglGetError: %s", 
			aglErrorString( err ) );
	}
}

//=======================================================================

/*
=====================
GLimp_ResetDisplay
=====================
*/
void GLimp_ResetDisplay( void ) {
	glConfig.isFullscreen = qfalse;

	// sys_gl.DSpContext is set only while GLimp_ChangeDisplay holds the
	// display; glConfig is cleared by GLimp_Init, so it can't say
	if ( !sys_gl.DSpContext ) {
		return;
	}
	
	// put the context into the inactive state
	DSpContext_SetState( sys_gl.DSpContext, kDSpContextState_Inactive );

	// release the context
	DSpContext_Release( sys_gl.DSpContext );
	sys_gl.DSpContext = NULL;

	// shutdown draw sprockets
	DSpShutdown();
}

/*
=====================
GLimp_ChangeDisplay

Returns qfalse, with DrawSprocket shut down again, if the display could
not be switched to the mode, so another mode can be tried
=====================
*/
qboolean GLimp_ChangeDisplay( int *actualWidth, int *actualHeight ) {
	OSStatus				theError;
	DSpContextAttributes 	inAttributes;
	DSpContextReference		context;
	int						colorBits;
	
	// startup DrawSprocket
	theError = DSpStartup();
	if( theError ) {
		ri.Printf( PRINT_ALL, "DSpStartup() failed: %i\n", (int)theError );
		*actualWidth = 640;
		*actualHeight = 480;
		return qtrue;	// as retail: carry on in a window, holding nothing
	}	
	
	if ( r_colorbits->integer == 24 || r_colorbits->integer == 32 ) {
		colorBits = kDSpDepthMask_32;
	} else {
		colorBits = kDSpDepthMask_16;
	}
	
	memset( &inAttributes, 0, sizeof( inAttributes ) );
	inAttributes.frequency					= 0;
	inAttributes.displayWidth				= glConfig.vidWidth;
	inAttributes.displayHeight				= glConfig.vidHeight;
	inAttributes.reserved1					= 0;
	inAttributes.reserved2					= 0;
	inAttributes.colorNeeds					= kDSpColorNeeds_Require;
	inAttributes.colorTable					= NULL;
	inAttributes.contextOptions				= 0;
	inAttributes.backBufferDepthMask		= colorBits;
	inAttributes.displayDepthMask			= colorBits;
	inAttributes.backBufferBestDepth		= colorBits;
	inAttributes.displayBestDepth			= colorBits;
	inAttributes.pageCount					= 1;
	inAttributes.gameMustConfirmSwitch		= false;
	inAttributes.reserved3[0]				= 0;
	inAttributes.reserved3[1]				= 0;
	inAttributes.reserved3[2]				= 0;
	inAttributes.reserved3[3]				= 0;

	context = NULL;
	theError = DSpFindBestContext( &inAttributes, &context );
	if ( theError || !context ) {
		ri.Printf( PRINT_ALL, "DSpFindBestContext() failed: %i\n", (int)theError );
		DSpShutdown();
		return qfalse;
	}

	inAttributes.displayWidth				= glConfig.vidWidth;
	inAttributes.displayHeight				= glConfig.vidHeight;
	inAttributes.backBufferDepthMask		= colorBits;
	inAttributes.displayDepthMask			= colorBits;
	inAttributes.backBufferBestDepth		= colorBits;
	inAttributes.displayBestDepth			= colorBits;
	inAttributes.pageCount					= 1;

	theError = DSpContext_Reserve( context, &inAttributes );
	if ( theError ) {
		ri.Printf( PRINT_ALL, "DSpContext_Reserve() failed: %i\n", (int)theError );
		DSpShutdown();
		return qfalse;
	}

	// find out what res we actually got
	theError = DSpContext_GetAttributes( context, &inAttributes );

	if ( !theError ) {
		// put the context into the active state
		theError = DSpContext_SetState( context, kDSpContextState_Active );
	}
	if ( theError ) {
		ri.Printf( PRINT_ALL, "DSpContext activation failed: %i\n", (int)theError );
		DSpContext_Release( context );
		DSpShutdown();
		return qfalse;
	}
	
	*actualWidth = inAttributes.displayWidth;
	*actualHeight = inAttributes.displayHeight;
	
	// fade back in
	theError = DSpContext_FadeGammaIn( NULL, NULL );

	sys_gl.DSpContext = context;
	glConfig.isFullscreen = qtrue;
	return qtrue;
}

//=======================================================================


/*
===================
GLimp_AglDescribe_f

===================
*/
void GLimp_AglDescribe_f( void ) {
	long		value;
	long		r,g,b,a;
	long		stencil, depth;
	
	ri.Printf( PRINT_ALL, "Selected pixel format 0x%x\n", (int)sys_gl.fmt );
	
	ri.Printf( PRINT_ALL, "TEXTURE_MEMORY: %i\n", (int)sys_gl.textureMemory );
	ri.Printf( PRINT_ALL, "VIDEO_MEMORY: %i\n", (int)sys_gl.videoMemory );
	
	aglDescribePixelFormat(sys_gl.fmt, AGL_RED_SIZE, &r);
	aglDescribePixelFormat(sys_gl.fmt, AGL_GREEN_SIZE, &g);
	aglDescribePixelFormat(sys_gl.fmt, AGL_BLUE_SIZE, &b);
	aglDescribePixelFormat(sys_gl.fmt, AGL_ALPHA_SIZE, &a);
	aglDescribePixelFormat(sys_gl.fmt, AGL_STENCIL_SIZE, &stencil);
	aglDescribePixelFormat(sys_gl.fmt, AGL_DEPTH_SIZE, &depth);
	ri.Printf( PRINT_ALL, "red:%i green:%i blue:%i alpha:%i depth:%i stencil:%i\n",
		(int)r, (int)g, (int)b, (int)a, (int)depth, (int)stencil );

	aglDescribePixelFormat(sys_gl.fmt, AGL_BUFFER_SIZE, &value);
	ri.Printf( PRINT_ALL, "BUFFER_SIZE: %i\n", (int)value );

	aglDescribePixelFormat(sys_gl.fmt, AGL_PIXEL_SIZE, &value);
	ri.Printf( PRINT_ALL, "PIXEL_SIZE: %i\n", (int)value );

	aglDescribePixelFormat(sys_gl.fmt, AGL_RENDERER_ID, &value);
	ri.Printf( PRINT_ALL, "RENDERER_ID: %i\n", (int)value );

	// memory functions
	value = glmGetInteger( GLM_PAGE_SIZE );
	ri.Printf( PRINT_ALL, "GLM_PAGE_SIZE: %i\n", (int)value );
	
	value = glmGetInteger( GLM_NUMBER_PAGES );
	ri.Printf( PRINT_ALL, "GLM_NUMBER_PAGES: %i\n", (int)value );
	
	value = glmGetInteger( GLM_CURRENT_MEMORY );
	ri.Printf( PRINT_ALL, "GLM_CURRENT_MEMORY: %i\n", (int)value );
	
	value = glmGetInteger( GLM_MAXIMUM_MEMORY );
	ri.Printf( PRINT_ALL, "GLM_MAXIMUM_MEMORY: %i\n", (int)value );
	
}

/*
===================
GLimp_AglState_f

===================
*/
void GLimp_AglState_f( void ) {
	char	*cmd;
	int		state, value;
	
	if ( ri.Cmd_Argc() != 3 ) {
		ri.Printf( PRINT_ALL, "Usage: aglstate <parameter> <0/1>\n" );
		return;
	}

	cmd = ri.Cmd_Argv( 1 );
	if ( !Q_stricmp( cmd, "rasterization" ) ) {
		state = AGL_RASTERIZATION;
	} else {
		ri.Printf( PRINT_ALL, "Unknown agl state: %s\n", cmd );
		return;
	}
	
	cmd = ri.Cmd_Argv( 2 );
	value = atoi( cmd );
	
	if ( value ) {
		aglEnable( sys_gl.context, state );
	} else {
		aglDisable( sys_gl.context, state );
	}
}

/*
===================
GLimp_Extensions

===================
*/
static void GLimp_Extensions( void ) {
	const char	*extensions;

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (const char *)qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (const char *)qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (const char *)qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (const char *)qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );

	extensions = glConfig.extensions_string;

	// GL_ARB_multitexture
	qglMultiTexCoord2fARB = NULL;
	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;

	if ( strstr( extensions, "GL_ARB_multitexture" )  ) {
		if ( r_ext_multitexture->integer && r_allowExtensions->integer ) {
			// qglMultiTexCoord2fARB = glMultiTexCoord2fARB;
			// qglActiveTextureARB = glActiveTextureARB;
			// qglClientActiveTextureARB = glClientActiveTextureARB;

			ri.Printf( PRINT_ALL, "...GL_ARB_multitexture found, not used by this build\n" );
		} else {
			ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
		}
	} else {
		ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );
	}

	// GL_EXT_compiled_vertex_array
	qglLockArraysEXT = NULL;
	qglUnlockArraysEXT = NULL;

	if ( strstr( extensions, "GL_EXT_compiled_vertex_array" ) ) {
		if ( r_ext_compiled_vertex_array->integer && r_allowExtensions->integer ) {
			// qglLockArraysEXT = glLockArraysEXT;
			// qglUnlockArraysEXT = glUnlockArraysEXT;

			ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array found, not used by this build\n" );
		} else {
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
		}
	} else {
		ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
	}
	
	// GL_EXT_texture_env_add
	glConfig.textureEnvAddAvailable = qfalse;
	if ( strstr( glConfig.extensions_string, "EXT_texture_env_add" ) ) {
		if ( r_ext_texture_env_add->integer ) {
			glConfig.textureEnvAddAvailable = qtrue;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
		} else {
			glConfig.textureEnvAddAvailable = qfalse;
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
		}
	} else {
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );
	}

	// GL_EXT_texture_filter_anisotropic
	glConfig.anisotropicAvailable = qfalse;
	if ( strstr( glConfig.extensions_string, "EXT_texture_filter_anisotropic" ) )
	{
		glConfig.anisotropicAvailable = r_ext_texture_filter_anisotropic->integer ? qtrue : qfalse;
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic available\n" );

		if ( r_ext_texture_filter_anisotropic->integer )
		{
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_filter_anisotropic\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
		ri.Cvar_Set( "r_ext_texture_filter_anisotropic_avail", "1" );
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not found\n" );
		ri.Cvar_Set( "r_ext_texture_filter_anisotropic_avail", "0" );
	}

	// apple transform hint
	if ( strstr( extensions, "GL_APPLE_transform_hint" ) ) {
		if ( r_ext_compiled_vertex_array->integer && r_allowExtensions->integer ) {
			glHint( GL_TRANSFORM_HINT_APPLE, GL_FASTEST );
			ri.Printf( PRINT_ALL, "...using GL_APPLE_transform_hint\n" );
		} else {
			ri.Printf( PRINT_ALL, "...ignoring GL_APPLE_transform_hint\n" );
		}
	} else {
		ri.Printf( PRINT_ALL, "...GL_APPLE_transform_hint not found\n" );
	}
}

/*
============================
GLimp_SufficientVideoMemory
============================
*/
#if 0

qboolean	GLimp_SufficientVideoMemory( void ) {
	AGLRendererInfo head_info, info;
	GLint 		accelerated;
	AGLDevice 	*device;
	GLint 		i, ndevs;
		
	device = aglDevicesOfPixelFormat( sys_gl.fmt, &ndevs);
	if (!device || ndevs < 1) {
		ri.Printf( PRINT_ALL, "aglDevicesOfPixelFormat failed.\n" );
		return 0;
	}
	
	ri.Printf( PRINT_ALL, "%i rendering devices\n", ndevs );
	
	head_info =  aglQueryRendererInfo( device, 1 );
	info = head_info;
	if (!info) {
		ri.Printf( PRINT_ALL, "aglQueryRendererInfo failed.\n" );
		return 0;
	}
	
	for ( i = 0 ; i < ndevs ; i++ ) {
		// ignore the software renderer listing
		aglDescribeRenderer( info, AGL_ACCELERATED, &accelerated );
		if ( accelerated ) {		
			aglDescribeRenderer( info, AGL_TEXTURE_MEMORY, &sys_gl.textureMemory );
			aglDescribeRenderer( info, AGL_VIDEO_MEMORY, &sys_gl.videoMemory );
		}
		info = aglNextRendererInfo(info);
	}

	aglDestroyRendererInfo(head_info);
#if 0
	if ( sys_gl.videoMemory < 16000000 ) {
		glConfig.hardwareType = GLHW_RAGEPRO;		// FIXME when voodoo is available
	} else {
		glConfig.isRagePro = GLHW_GENERIC;		// FIXME when voodoo is available
	}
#endif
	ri.Printf( PRINT_ALL, "%i texture memory, %i video memory\n", sys_gl.textureMemory, sys_gl.videoMemory );

	return qtrue;
}

#endif

/*
=======================
CheckDeviceRenderers

========================
*/
/*
=======================
CheckDeviceRenderers

========================
*/
static void CheckDeviceRenderers( GDHandle device ) {
	AGLRendererInfo info, head_info;
	GLint inum;
	GLint	accelerated;
	GLint	textureMemory, videoMemory;
	
    Com_FlightRecord("CheckDeviceRenderers: Start\n");
    Debug_Breadcrumb(410); // CheckDeviceRenderers Entry

	head_info =  aglQueryRendererInfo(&device, 1);
	if( !head_info ) {
		ri.Printf( PRINT_ALL, "aglQueryRendererInfo : Info Error\n");
        Com_FlightRecord("CheckDeviceRenderers: aglQueryRendererInfo failed\n");
		return;
	}
	
	info = head_info;
	inum = 0;
	while( info ) {
        Com_FlightRecord("CheckDeviceRenderers: Loop inum=%d\n", (int)inum);
        Debug_Breadcrumb(411); // CheckDeviceRenderers Loop start

		ri.Printf( PRINT_ALL, "  Renderer : %d\n", (int)inum);
		
		aglDescribeRenderer( info, AGL_ACCELERATED, &accelerated );
		
		if ( accelerated ) {		
			aglDescribeRenderer( info, AGL_TEXTURE_MEMORY, &textureMemory );
			aglDescribeRenderer( info, AGL_VIDEO_MEMORY, &videoMemory );
			ri.Printf( PRINT_ALL, "    AGL_VIDEO_MEMORY: %i\n", (int)textureMemory );
			ri.Printf( PRINT_ALL, "    AGL_TEXTURE_MEMORY: %i\n", (int)videoMemory );
			
			// save the device with the most texture memory
			if ( sys_gl.textureMemory < textureMemory ) {
				sys_gl.textureMemory = textureMemory;
				sys_gl.device = device;
			}
		} else {
			ri.Printf( PRINT_ALL, "    Not accelerated.\n" );
		}

		info = aglNextRendererInfo(info);
		inum++;
        
        // Safety Break
        if (inum > 100) {
            Com_FlightRecord("CheckDeviceRenderers: Loop runaway! Breaking.\n");
            break;
        }
	}
	
	aglDestroyRendererInfo(head_info);
    Com_FlightRecord("CheckDeviceRenderers: End\n");
    Debug_Breadcrumb(412); // CheckDeviceRenderers Exit
}


/*
=======================
CheckDevices

Make sure there is a device with enough video memory to play
=======================
*/
static void CheckDevices( void ) {
	GDHandle device;
	static	qboolean	checkedFullscreen;
	
    Com_FlightRecord("CheckDevices: Start\n");
    Debug_Breadcrumb(400); // CheckDevices Entry

	if ( checkedFullscreen ) {
		return;
	}
	if ( glConfig.isFullscreen ) {
		checkedFullscreen = qtrue;
	}

	device = GetDeviceList();
	sys_gl.numDevices = 0;
    Com_FlightRecord("CheckDevices: GetDeviceList returned %p\n", device);

	while( device && sys_gl.numDevices < MAX_DEVICES ) {
        Com_FlightRecord("CheckDevices: Loop device=%p numDevices=%d\n", device, sys_gl.numDevices);
        Debug_Breadcrumb(401); // CheckDevices Loop

		sys_gl.devices[ sys_gl.numDevices ] = device;

		ri.Printf( PRINT_ALL, "Device : %d\n", sys_gl.numDevices);
		CheckDeviceRenderers(device);
		
		device = GetNextDevice(device);
		
		sys_gl.numDevices++;
	}

    Com_FlightRecord("CheckDevices: Loop finished/Devices checked.\n");
    Debug_Breadcrumb(402); // CheckDevices Loop End

	CheckErrors();		
	
	if ( sys_gl.textureMemory < 4000000 ) {
		ri.Error( ERR_FATAL, "You must have at least four megs of video memory to play" );
	}

	if ( sys_gl.textureMemory < 16000000 ) {
		sys_hardwareType = GLHW_RAGEPRO;		// this will have to change with voodoo
	} else {
		sys_hardwareType = GLHW_GENERIC;
	}
    Com_FlightRecord("CheckDevices: End\n");
}

/*
=================
CreateGameWindow
=================
*/
static qboolean CreateGameWindow( void ) {
	cvar_t		*vid_xpos;
	cvar_t		*vid_ypos;
	int			mode;
	int			x, y;
	Str255    	pstr;

	Rect		windowRect;
	
	Sys_LogPrintf("DEBUG: CreateGameWindow: Starting\n");
	
	vid_xpos = ri.Cvar_Get( "vid_xpos", "30", 0 );
	vid_ypos = ri.Cvar_Get( "vid_ypos", "30", 0 );
	
	// get mode info
	mode = r_mode->integer;
    ri.Printf( PRINT_ALL, "...setting mode %d:", mode );
	Sys_LogPrintf("DEBUG: CreateGameWindow: mode=%d\n", mode);

    if ( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ) )  {
        ri.Printf( PRINT_ALL, " invalid mode\n" );
		Sys_LogPrintf("DEBUG: CreateGameWindow: R_GetModeInfo failed (invalid mode)\n");
        return false;
    }
    ri.Printf( PRINT_ALL, " %d %d\n", glConfig.vidWidth, glConfig.vidHeight );
	Sys_LogPrintf("DEBUG: CreateGameWindow: vidWidth=%d, vidHeight=%d\n", glConfig.vidWidth, glConfig.vidHeight);

	/* Create window - using NewCWindow instead of GetNewCWindow (no resources needed) */
	if ( r_fullscreen->integer ) {
		int		actualWidth, actualHeight;
		
		Sys_LogPrintf("DEBUG: CreateGameWindow: Fullscreen mode, calling GLimp_ChangeDisplay\n");
		
		// change display resolution
		if ( !GLimp_ChangeDisplay( &actualWidth, &actualHeight ) ) {
			return qfalse;
		}
		
		x = ( actualWidth - glConfig.vidWidth ) / 2;
		y = ( actualHeight - glConfig.vidHeight ) / 2;
		
		Sys_LogPrintf("DEBUG: CreateGameWindow: actualWidth=%d, actualHeight=%d, x=%d, y=%d\n", actualWidth, actualHeight, x, y);
	} else {
		// Windowed: honor r_mode (glConfig dims set by R_GetModeInfo above)
		// and the vid_xpos/vid_ypos cvars. A debug override used to force
		// 320x240 at a fixed position here.
		x = vid_xpos->integer;
		y = vid_ypos->integer;
		if ( y < 40 ) {
			y = 40;		// keep the title bar below the menu bar
		}
		Sys_LogPrintf("CreateGameWindow: windowed %dx%d at (%d,%d)\n",
			glConfig.vidWidth, glConfig.vidHeight, x, y);
	}
	
	// Create window rect
	windowRect.left = x;
	windowRect.top = y;
	windowRect.right = x + glConfig.vidWidth;
	windowRect.bottom = y + glConfig.vidHeight;
	
	Sys_LogPrintf("DEBUG: CreateGameWindow: Calling NewCWindow, rect=(%d,%d,%d,%d)\n", 
		   windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
	
	// Use NewCWindow instead of GetNewCWindow to avoid resource dependency
	// Window style: 5 = movableDBoxProc (a title bar, no close or grow box)
	// For fullscreen, use 2 = plainDBox (plain box)
	{
		short windowStyle = r_fullscreen->integer ? 2 : 5;
		CToPStr("Enemy Territory", pstr);
		sys_gl.drawable = (AGLDrawable) NewCWindow(nil, &windowRect, pstr, true, windowStyle, (WindowPtr)-1L, false, 0);
	}
	
	if( !sys_gl.drawable ) {
		Sys_LogPrintf("DEBUG: CreateGameWindow: NewCWindow FAILED!\n");
		return qfalse;
	}
	
	Sys_LogPrintf("DEBUG: CreateGameWindow: NewCWindow succeeded, drawable=%p\n", sys_gl.drawable);
	
	SizeWindow((GrafPort *) sys_gl.drawable, glConfig.vidWidth, glConfig.vidHeight,GL_FALSE);
	MoveWindow((GrafPort *) sys_gl.drawable,x, y, GL_FALSE);
	ShowWindow((GrafPort *) sys_gl.drawable);
	SetPort((GrafPort *) sys_gl.drawable);
	HiliteWindow((GrafPort *) sys_gl.drawable, 1);
	SelectWindow((WindowPtr)sys_gl.drawable);
	
	Sys_LogPrintf("DEBUG: CreateGameWindow: Window setup complete\n");
	
	return qtrue;
}


/*
===================
GLimp_ReleaseDisplay

Gives the screen back after a fatal error or crash that GLimp_Shutdown will
not run for (a recursive Com_Error skips CL_Shutdown), so the Stop alert can
be seen and the desktop is not left captured, with the game's gamma, behind
it. The caller is exiting, so nothing else is released. Safe to call when
GLimp_Shutdown has run, or before GLimp_Init.
===================
*/
void GLimp_ReleaseDisplay( void ) {
	if ( sys_gl.systemGammas ) {
		RestoreSystemGammas( sys_gl.systemGammas );
	}
	if ( sys_gl.context ) {
		aglSetCurrentContext( NULL );
		aglSetDrawable( sys_gl.context, NULL );
	}
	GLimp_ResetDisplay();
}

/*
===================
GLimp_ReleaseMode

Undoes, in reverse order, whatever GLimp_SetMode got done, so a failed
attempt leaves nothing behind for the fallback mode, vid_restart or quit
===================
*/
static void GLimp_ReleaseMode( void ) {
	if ( sys_gl.context ) {
		aglSetCurrentContext( NULL );
		aglSetDrawable( sys_gl.context, NULL );
		aglDestroyContext( sys_gl.context );
		sys_gl.context = 0;
	}
	if ( sys_gl.fmt ) {
	    aglDestroyPixelFormat( sys_gl.fmt );
		sys_gl.fmt = 0;
	}
	if ( sys_gl.drawable ) {
		DisposeWindow( (GrafPort *) sys_gl.drawable );
		sys_gl.drawable = 0;
	}
	GLimp_ResetDisplay();

	// drop the error a failed AGL call left pending, or the next
	// attempt's first CheckErrors would make it fatal
	aglGetError();
}

/*
===================
GLimp_SetMode

Returns false if the mode / fullscrenn / options combination failed,
so another fallback can be tried
===================
*/
qboolean GLimp_SetMode( void ) {
	GLint     	value;
	GLint		attrib[64];
	int			i;

	Sys_LogPrintf("DEBUG: GLimp_SetMode: Starting\n");

	if ( !CreateGameWindow() ) {
		Sys_LogPrintf("DEBUG: GLimp_SetMode: CreateGameWindow FAILED\n");
		ri.Printf( PRINT_ALL, "GLimp_Init: window could not be created" );
		GLimp_ReleaseMode();
		return qfalse;
	}
	
	Sys_LogPrintf("DEBUG: GLimp_SetMode: CreateGameWindow succeeded\n");
	
	// check devices now that the game has set the display mode,
	// because RAVE devices don't get reported if in an 8 bit desktop
	Sys_LogPrintf("DEBUG: GLimp_SetMode: Calling CheckDevices\n");
	CheckDevices();
	
	Sys_LogPrintf("DEBUG: GLimp_SetMode: CheckDevices complete, numDevices=%d\n", sys_gl.numDevices);
	
	// set up the attribute list
	i = 0;
	attrib[i++] = AGL_RGBA;
	attrib[i++] = AGL_DOUBLEBUFFER;
	attrib[i++] = AGL_NO_RECOVERY;
	attrib[i++] = AGL_ACCELERATED;

	// r_colorbits 24 or 32 asks for 8/8/8, as on the 32-bit display
	// GLimp_ChangeDisplay sets; 16 (the fallback) and 0 (the default) ask
	// for 5/5/5, as on its 16-bit display
	if ( r_colorbits->integer > 16 ) {
		attrib[i++] = AGL_RED_SIZE;
		attrib[i++] = 8;
		attrib[i++] = AGL_GREEN_SIZE;
		attrib[i++] = 8;
		attrib[i++] = AGL_BLUE_SIZE;
		attrib[i++] = 8;
		attrib[i++] = AGL_ALPHA_SIZE;
		attrib[i++] = 0;
	} else {
		attrib[i++] = AGL_RED_SIZE;
		attrib[i++] = 5;
		attrib[i++] = AGL_GREEN_SIZE;
		attrib[i++] = 5;
		attrib[i++] = AGL_BLUE_SIZE;
		attrib[i++] = 5;
		attrib[i++] = AGL_ALPHA_SIZE;
		attrib[i++] = 0;
	}
	
	attrib[i++] = AGL_STENCIL_SIZE;
	if ( r_stencilbits->integer ) {
		attrib[i++] = r_stencilbits->integer;		
	} else {
		attrib[i++] = 0;
	}

	attrib[i++] = AGL_DEPTH_SIZE;
	if ( r_depthbits->integer ) {
		attrib[i++] = r_depthbits->integer;
	} else {
		attrib[i++] = 16;
	}
	
	attrib[i++] = 0;
	
	/* Choose pixel format */
	Sys_LogPrintf("DEBUG: GLimp_SetMode: Calling aglChoosePixelFormat\n");
	ri.Printf( PRINT_ALL, "aglChoosePixelFormat\n" );
	if ( r_device->integer < 0 || r_device->integer >= sys_gl.numDevices ) {
		ri.Cvar_Set( "r_device", "0" );
	}
	sys_gl.fmt = aglChoosePixelFormat( &sys_gl.devices[ r_device->integer ], 1, attrib);
	if(!sys_gl.fmt) {
		Sys_LogPrintf("DEBUG: GLimp_SetMode: aglChoosePixelFormat FAILED\n");
		ri.Printf( PRINT_ALL, "GLimp_Init: Pixel format could not be achieved\n");
		GLimp_ReleaseMode();
		return qfalse;
	}
	Sys_LogPrintf("DEBUG: GLimp_SetMode: aglChoosePixelFormat succeeded, fmt=0x%x\n", (int)sys_gl.fmt);
	ri.Printf( PRINT_ALL, "Selected pixel format 0x%x\n", (int)sys_gl.fmt );
	
	aglDescribePixelFormat(sys_gl.fmt, AGL_RED_SIZE, &value);
	glConfig.colorBits = value * 3;
	aglDescribePixelFormat(sys_gl.fmt, AGL_STENCIL_SIZE, &value);
	glConfig.stencilBits = value;
	aglDescribePixelFormat(sys_gl.fmt, AGL_DEPTH_SIZE, &value);
	glConfig.depthBits = value;

	CheckErrors();
	
	/* Create context */
	sys_gl.context = aglCreateContext(sys_gl.fmt, NULL);
	if(!sys_gl.context) {
		ri.Printf( PRINT_ALL, "GLimp_init: Context could not be created\n");
		GLimp_ReleaseMode();
		return qfalse;
	}
	
	CheckErrors();
	
	/* Make context current */

    Sys_LogPrintf("DEBUG: GLimp_SetMode: Calling aglSetDrawable, context=%p, drawable=%p\n", sys_gl.context, sys_gl.drawable);
	if(!aglSetDrawable(sys_gl.context, sys_gl.drawable)) {
		GLenum err = aglGetError();
        Sys_LogPrintf("DEBUG: GLimp_SetMode: aglSetDrawable FAILED, err=%d\n", (int)err);
		ri.Printf( PRINT_ALL, "GLimp_Init: Could not attach to context\n" );
		GLimp_ReleaseMode();
		return qfalse;
	}
    Sys_LogPrintf("DEBUG: GLimp_SetMode: aglSetDrawable succeeded. drawable=%p\n", sys_gl.drawable);
	
	CheckErrors();
	
	if( !aglSetCurrentContext(sys_gl.context) ) {
		ri.Printf( PRINT_ALL, "GLimp_Init: Could not attach to context");
		GLimp_ReleaseMode();
		return qfalse;
	}
    Sys_LogPrintf("DEBUG: GLimp_SetMode: aglSetCurrentContext succeeded. drawable=%p\n", sys_gl.drawable);

	CheckErrors();
	
	// check video memory and determine ragePro status
#if 0
	if ( !GLimp_SufficientVideoMemory() ) {
		return qfalse;
	}
#endif
	glConfig.hardwareType = sys_hardwareType;		// FIXME: this isn't really right
	
	// draw something to show that GL is alive	
	qglClearColor( 1, 0.5, 0.2, 0 );
	qglClear( GL_COLOR_BUFFER_BIT );
    
    Sys_LogPrintf("DEBUG: GLimp_SetMode: Calling GLimp_EndFrame. drawable=%p\n", sys_gl.drawable);
	GLimp_EndFrame();
    Sys_LogPrintf("DEBUG: GLimp_SetMode: Returned from GLimp_EndFrame. drawable=%p\n", sys_gl.drawable);

	CheckErrors();

	// get the extensions	
	GLimp_Extensions();

	CheckErrors();
	
	return qtrue;
}



/*
===================
GLimp_Init

Don't return unless OpenGL has been properly initialized
===================
*/
void GLimp_Init( void ) {
	GLint		major, minor;
	
	ri.Printf( PRINT_ALL, "--- GLimp_Init ---\n" );
	Sys_LogPrintf("DEBUG: GLimp_Init: Starting\n");

	aglGetVersion( &major, &minor );
	ri.Printf( PRINT_ALL, "aglVersion: %i.%i\n", (int)major, (int)minor );
	Sys_LogPrintf("DEBUG: GLimp_Init: aglVersion %d.%d\n", (int)major, (int)minor);
	
	r_device = ri.Cvar_Get( "r_device", "0", CVAR_LATCH | CVAR_ARCHIVE );
	r_ext_transform_hint = ri.Cvar_Get( "r_ext_transform_hint", "1", CVAR_LATCH | CVAR_ARCHIVE );
	r_ext_texture_filter_anisotropic = ri.Cvar_Get(
		"r_ext_texture_filter_anisotropic", "0", CVAR_LATCH | CVAR_ARCHIVE );
	
	// GLimp_Shutdown removes these, as they use the context
	ri.Cmd_AddCommand( "aglDescribe", GLimp_AglDescribe_f );
	ri.Cmd_AddCommand( "aglState", GLimp_AglState_f );
	
	memset( &glConfig, 0, sizeof( glConfig ) );


	r_swapInterval->modified = qtrue;	// force a set next frame


	// FIXME: try for a voodoo first
	sys_gl.systemGammas = GetSystemGammas();

	// change the desktop gamma only if GLimp_Shutdown can put it back
	glConfig.deviceSupportsGamma = sys_gl.systemGammas ? qtrue : qfalse;
	if ( !glConfig.deviceSupportsGamma ) {
		ri.Printf( PRINT_ALL, "...could not save the desktop gamma, not using hardware gamma\n" );
	}

	if ( GLimp_SetMode() ) {
		ri.Printf( PRINT_ALL, "------------------\n" );
		return;
	}

	// fall back to the known-good mode: 640x480, 16-bit, fullscreen, as
	// retail and win32 do, so a desktop accelerated GL can't draw a window
	// on (8-bit, say) still gets a display it can use
	ri.Cvar_Set( "r_fullscreen", "1" );
	ri.Cvar_Set( "r_mode", "3" );
	ri.Cvar_Set( "r_stereo", "0" );
	ri.Cvar_Set( "r_depthBits", "16" );
	ri.Cvar_Set( "r_colorBits", "16" );
	ri.Cvar_Set( "r_stencilBits", "0" );

	if ( GLimp_SetMode() ) {
		ri.Printf( PRINT_ALL, "------------------\n" );
		return;
	}

	Sys_LogPrintf("DEBUG: GLimp_Init: Both GLimp_SetMode attempts failed!\n");
	ri.Error( ERR_FATAL, "Could not initialize OpenGL" );
}


/*
===============
GLimp_EndFrame

===============
*/
void GLimp_EndFrame( void ) {
	// check for variable changes
	if ( r_swapInterval->modified ) {
		r_swapInterval->modified = qfalse;
		ri.Printf( PRINT_ALL, "Changing AGL_SWAP_INTERVAL\n" );
		aglSetInteger( sys_gl.context, AGL_SWAP_INTERVAL, (long *)&r_swapInterval->integer );
	}

	// make sure the event loop is pumped
	Sys_SendKeyEvents();
	
	aglSwapBuffers( sys_gl.context );
}

/*
===============
GLimp_Shutdown

===============
*/
void GLimp_Shutdown( void ) {	
	ri.Cmd_RemoveCommand( "aglDescribe" );
	ri.Cmd_RemoveCommand( "aglState" );

	if ( sys_gl.systemGammas ) {
		RestoreSystemGammas( sys_gl.systemGammas );
		DisposeSystemGammas( &sys_gl.systemGammas );
		sys_gl.systemGammas = 0;
	}
	
	GLimp_ReleaseMode();
	
	memset( &glConfig, 0, sizeof( glConfig ) );
}


void		GLimp_LogComment( char *comment ) {
}
qboolean	GLimp_SpawnRenderThread( void (*function)( void ) ) {
	return qfalse;
}
void *GLimp_RendererSleep( void ) {
	return NULL;
}

void GLimp_FrontEndSleep( void ) {

}

void GLimp_WakeRenderer( void * data ) {

}



/*
===============
GLimp_SetGamma

===============
*/
void		GLimp_SetGamma( unsigned char red[256], 
						    unsigned char green[256],
							unsigned char blue[256] ) {
	char	color[3][256];
	int		i;

	// no snapshot, no way to put the desktop gamma back
	if ( !sys_gl.systemGammas ) {
		return;
	}

	for ( i = 0 ; i < 256 ; i++ ) {
		color[0][i] = red[i];
		color[1][i] = green[i];
		color[2][i] = blue[i];	
	}
	SetDeviceGammaRampGD( sys_gl.device, color[0] );
}

