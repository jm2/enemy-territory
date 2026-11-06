/* The client's display, input and sound on a headless host. Nothing is drawn,
 * but everything up to the driver runs: every map, model, shader, image and
 * sound a level loads, the static modules and their resets. */
#include <stdlib.h>
#include <string.h>
#include "../../src/renderer/tr_local.h"
#include "../../src/client/snd_local.h"
#include "../../src/client/client.h"

void GLimp_Init( void ) {
	memset( &glConfig, 0, sizeof( glConfig ) );
	glConfig.vidWidth = 640;
	glConfig.vidHeight = 480;
	glConfig.windowAspect = 640.0f / 480.0f;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.isFullscreen = qtrue;
	glConfig.textureEnvAddAvailable = qtrue;
	strcpy( glConfig.vendor_string, "Stub" );
	strcpy( glConfig.renderer_string, "Stub" );
	strcpy( glConfig.version_string, "1.2" );
	strcpy( glConfig.extensions_string, "GL_EXT_texture_env_add" );
}
void GLimp_Shutdown( void ) { memset( &glConfig, 0, sizeof( glConfig ) ); }
void GLimp_EndFrame( void ) {}
void GLimp_LogComment( char *comment ) {}
void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {}
void GLimp_EnableLogging( qboolean enable ) {}
qboolean GLimp_SpawnRenderThread( void ( *function )( void ) ) { return qfalse; }
void *GLimp_RendererSleep( void ) { return NULL; }
void GLimp_FrontEndSleep( void ) {}
void GLimp_WakeRenderer( void *data ) {}
qboolean QGL_Init( const char *dllname ) { return qtrue; }
void QGL_Shutdown( void ) {}
void QGL_EnableLogging( qboolean enable ) {}

void IN_Init( void ) {}
void IN_Frame( void ) {}
void IN_Shutdown( void ) {}
void IN_Restart( void ) {}
void IN_Activate( void ) {}
void Sys_SendKeyEvents( void ) {}
void Sys_GLimpSafeInit( void ) {}
void Sys_GLimpInit( void ) {}

static short ring[0x8000];
qboolean SNDDMA_Init( void ) {
	dma.channels = 2;
	dma.samples = 0x8000;
	dma.submission_chunk = 0x100;
	dma.samplebits = 16;
	dma.speed = 22050;
	dma.buffer = (byte *)ring;
	return qtrue;
}
int SNDDMA_GetDMAPos( void ) { return ( Sys_Milliseconds() * 44 ) & 0x7ff0; }
void SNDDMA_Shutdown( void ) {}
void SNDDMA_BeginPainting( void ) {}
void SNDDMA_Submit( void ) {}

int gl_NormalFontBase;
const char *glx_extensions_string = "";
