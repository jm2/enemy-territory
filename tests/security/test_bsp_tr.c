/* RE_LoadWorldMap against corrupted BSPs, with the real tr_bsp.c and
 * tr_curve.c; shaders, images and models are stubs. A refused map must drop
 * before tr, the world, the hunk or any shader changes. Also R_GetSurfMemory,
 * and R_FindLightmap extracted from tr_shader.c (LIGHTMAP_INC). With BSP files
 * as arguments, loads each one, then truncates and mutates it. Run under ASan
 * and UBSan. */
#include "../../src/renderer/tr_bsp.c"
#include "bsp_fixture.h"

trGlobals_t tr;
glconfig_t glConfig;
refimport_t ri;
int skyboxportal;
static cvar_t zeroCvar, subdivisionsCvar, foliageCvar;
cvar_t *r_lightmap = &zeroCvar, *r_mapOverBrightBits = &zeroCvar, *r_singleShader = &zeroCvar;
cvar_t *r_subdivisions = &subdivisionsCvar, *r_drawfoliage = &foliageCvar;
static shader_t knownShader;
static int shaderCalls, imageCalls;
static image_t knownImage;

shader_t *R_FindShader( const char *name, int lightmapIndex, qboolean mipRawImage ) {
	(void)name; (void)lightmapIndex; (void)mipRawImage;
	shaderCalls++;
	return &knownShader;
}
model_t *R_AllocModel( void ) {
	model_t *mod;
	if ( tr.numModels == MAX_MOD_KNOWN ) {
		return NULL;
	}
	mod = TestHunk( sizeof( *mod ) );
	mod->index = tr.numModels;
	tr.models[tr.numModels++] = mod;
	return mod;
}
image_t *R_CreateImage( const char *name, const byte *pic, int width, int height, qboolean mipmap, qboolean allowPicmip, int wrapClampMode ) {
	(void)name; (void)mipmap; (void)allowPicmip; (void)wrapClampMode;
	Check( pic != NULL && width == LIGHTMAP_SIZE && height == LIGHTMAP_SIZE, "lightmap upload" );
	imageCalls++;
	return &knownImage;
}
void R_RemapShader( const char *oldShader, const char *newShader, const char *timeOffset ) { (void)oldShader; (void)newShader; (void)timeOffset; }
void R_SetFog( int fogvar, int var1, int var2, float r, float g, float b, float density ) {
	(void)fogvar; (void)var1; (void)var2; (void)r; (void)g; (void)b; (void)density;
}
void R_SyncRenderThread( void ) {}
static void QDECL TestPrintf( int level, const char *fmt, ... ) { (void)level; (void)fmt; }
static void *TestRiHunk( int size, ha_pref preference ) { (void)preference; return TestHunk( size ); }
static void TestExecuteText( int when, const char *text ) { (void)when; (void)text; }

#ifdef LIGHTMAP_INC
/* R_FindLightmap as tr_shader.c has it, with its image loader stubbed. */
static char requestedImage[256];
image_t *R_FindImageFile( const char *name, qboolean mipmap, qboolean allowPicmip, int glWrapClampMode, qboolean lightmap ) {
	(void)mipmap; (void)allowPicmip; (void)glWrapClampMode; (void)lightmap;
	snprintf( requestedImage, sizeof( requestedImage ), "%s", name );
	return &knownImage;
}
#include LIGHTMAP_INC
#endif

static int LoaderTry( const char *name ) {
	tr.worldMapLoaded = qfalse;     // RE_Shutdown's job between maps
	if ( realMaps ) {
		tr.numModels = 0;           // and R_ModelInit's
	}
	expectDrop = 1;
	if ( setjmp( dropJump ) ) {
		expectDrop = 0;
		if ( fileBlock ) {
			FS_FreeFile( fileBlock );
		}
		return 0;
	}
	RE_LoadWorldMap( name );
	expectDrop = 0;
	return 1;
}

static int LoaderAccept( const char *name ) {
	int i;
	if ( !LoaderTry( name ) ) {
		printf( "    unexpected drop: %s\n", lastError );
		return 0;
	}
	Check( tr.world == &s_worldData && s_worldData.numsurfaces >= 1, "world published" );
	Check( Terminated( s_worldData.entityString, LumpLen( LUMP_ENTITIES ) ), "entity string terminated" );
	for ( i = 0; i < s_worldData.numsurfaces; i++ ) {
		Check( s_worldData.surfaces[i].fogIndex >= 0 && s_worldData.surfaces[i].fogIndex < s_worldData.numfogs, "surface fog index in range" );
	}
	Check( tr.numLightmaps <= MAX_LIGHTMAPS, "lightmap count in range" );
	if ( !s_worldData.lightGridBounds[0] || !s_worldData.lightGridBounds[1] || !s_worldData.lightGridBounds[2] ) {
		Check( s_worldData.lightGridData == NULL, "no grid data for an empty grid" );
	}
	if ( s_worldData.lightGridData ) {
		Check( s_worldData.lightGridBounds[0] > 0 && s_worldData.lightGridBounds[1] > 0 && s_worldData.lightGridBounds[2] > 0 &&
			   (long long)s_worldData.lightGridBounds[0] * s_worldData.lightGridBounds[1] * s_worldData.lightGridBounds[2] * 8 == LumpLen( LUMP_LIGHTGRID ),
			   "grid data matches its bounds" );
	}
	if ( LumpLen( LUMP_LIGHTMAPS ) == LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3 ) {
		Check( tr.numLightmaps == 2 && tr.lightmaps[1], "the single-lightmap hack keeps two textures" );
	}
	if ( !strcmp( name, "maps/previous.bsp" ) ) {
		// the unmutated fixture, as the unfixed loader also builds it
		Check( s_worldData.numsurfaces == NUM_SURFS && s_worldData.numfogs == 3 && s_worldData.globalFog == 2, "fixture surfaces and fogs" );
		Check( s_worldData.surfaces[S_FLARE].fogIndex == 2 && s_worldData.surfaces[S_TRISOUP].fogIndex == 1, "fixture fog indexes" );
		Check( s_worldData.lightGridBounds[0] == 3 && s_worldData.lightGridBounds[1] == 3 && s_worldData.lightGridBounds[2] == 1 &&
			   s_worldData.lightGridData != NULL, "fixture light grid" );
		Check( tr.numLightmaps == 2 && tr.lightmaps[0] && tr.lightmaps[1], "two lightmaps" );
		Check( s_worldData.surfaces[S_PATCH].data && *s_worldData.surfaces[S_PATCH].data == SF_GRID, "fixture patch" );
		Check( *s_worldData.surfaces[S_FOLIAGE].data == SF_FOLIAGE && ( (srfFoliage_t *)s_worldData.surfaces[S_FOLIAGE].data )->numInstances == 2, "fixture foliage" );
	}
	return 1;
}

static trGlobals_t trBefore;
static world_t worldBefore;
static void LoaderReject( const char *name ) {
	int hunk = hunkCount, r = reads, f = frees, d = drops, shaders = shaderCalls, images = imageCalls;

	tr.worldMapLoaded = qfalse;
	trBefore = tr;
	worldBefore = s_worldData;
	expectDrop = 1;
	if ( !setjmp( dropJump ) ) {
		RE_LoadWorldMap( name );
		expectDrop = 0;
		Fail( "RE_LoadWorldMap accepted the map" );
	}
	expectDrop = 0;
	Check( drops == d + 1, "one drop" );
	Check( fileBlock == NULL, "the file was freed before the drop" );
	Check( reads == r + 1 && frees == f + 1, "one read, one free" );
	Check( hunkCount == hunk, "no hunk allocation before the drop" );
	Check( shaderCalls == shaders && imageCalls == images, "no shader or image before the drop" );
	Check( !memcmp( &tr, &trBefore, sizeof( tr ) ), "renderer globals unchanged" );
	Check( !memcmp( &s_worldData, &worldBefore, sizeof( s_worldData ) ), "previous world unchanged" );
}

/* ---- renderer-only cases ---- */

static void M_ModelSlots( void ) { tr.numModels = MAX_MOD_KNOWN - 1; }
static void M_ModelSlotsFit( void ) { tr.numModels = MAX_MOD_KNOWN - 2; }

static const bspCase_t rendererCases[] = {
	{ "submodels past the free model slots", M_ModelSlots, SKIP, REJECT },
	{ "submodels exactly filling the model slots", M_ModelSlotsFit, SKIP, ACCEPT },
};

static int RunExtra( const char *name, void ( *test )( void ) ) {
	pid_t pid;
	int status;
	printf( "case: %s\n", name );
	fflush( stdout );
	pid = fork();
	if ( pid == 0 ) {
		test();
		exit( 0 );
	}
	waitpid( pid, &status, 0 );
	if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
		printf( "PASS %s\n", name );
		return 0;
	}
	printf( "FAIL %s\n", name );
	return 1;
}

static void T_SurfMemoryLarge( void ) {
	byte *a, *b;
	R_InitSurfMemory();
	a = R_GetSurfMemory( 64 );
	b = R_GetSurfMemory( SURF_HUNK_MAXSIZE + 4096 );
	memset( b, 1, SURF_HUNK_MAXSIZE + 4096 );   // ASan: the whole block is usable
	Check( a != NULL && b != NULL, "surface memory" );
}
static void T_SurfMemoryNegative( void ) {
	byte *a, *b;
	R_InitSurfMemory();
	a = R_GetSurfMemory( 64 );
	expectDrop = 1;
	if ( !setjmp( dropJump ) ) {
		b = R_GetSurfMemory( -4096 );
		expectDrop = 0;
		Check( b > a, "R_GetSurfMemory moved backwards" );
		Fail( "negative surface size accepted" );
	}
	expectDrop = 0;
}
#ifdef LIGHTMAP_INC
static void T_FindLightmapIndex( void ) {
	int index = MAX_LIGHTMAPS + 100;
	tr.worldDir = "maps/test";
	R_FindLightmap( &index );
	Check( index == LIGHTMAP_BY_VERTEX, "index past tr.lightmaps falls back to vertex light" );
	index = 5;
	R_FindLightmap( &index );
	Check( index == 5 && tr.lightmaps[5] == &knownImage && tr.numLightmaps == 6, "external lightmap in range still loads" );
	Check( !strcmp( requestedImage, "maps/test/lm_0005.tga" ), "external lightmap name" );
}
static void T_FindLightmapLongDir( void ) {
	char dir[200];
	int index = 3;
	memset( dir, 'd', sizeof( dir ) - 1 );
	dir[sizeof( dir ) - 1] = 0;
	tr.worldDir = dir;
	R_FindLightmap( &index );       // the name is truncated, not written past fileName
}
#endif

int main( int argc, char **argv ) {
	const char *m = getenv( "BSP_MUTATIONS" );
	unsigned int i;
	int failed = 0;

	whichLoader = 1;
	Swap_Init();
	subdivisionsCvar.value = 4;
	foliageCvar.value = 1;
	ri.Printf = TestPrintf;
	ri.Error = Com_Error;
	ri.Hunk_Alloc = TestRiHunk;
	ri.Z_Malloc = Z_Malloc;
	ri.Free = Z_Free;
	ri.FS_ReadFile = FS_ReadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.Cmd_ExecuteText = TestExecuteText;
	if ( argc > 1 ) {
		return RunRealMaps( argc, argv, m ? atoi( m ) : 200 ) ? 1 : 0;
	}
	printf( "RE_LoadWorldMap:\n" );
	failed += RunCases();
	for ( i = 0; i < sizeof( rendererCases ) / sizeof( rendererCases[0] ); i++ ) {
		failed += RunCase( &rendererCases[i] );
	}
	failed += RunExtra( "R_GetSurfMemory larger than a chunk", T_SurfMemoryLarge );
	failed += RunExtra( "R_GetSurfMemory negative size", T_SurfMemoryNegative );
#ifdef LIGHTMAP_INC
	failed += RunExtra( "R_FindLightmap index past MAX_LIGHTMAPS", T_FindLightmapIndex );
	failed += RunExtra( "R_FindLightmap long world directory", T_FindLightmapLongDir );
#endif
	printf( "%d renderer failures\n", failed );
	return failed ? 1 : 0;
}
