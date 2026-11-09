/* RE_LoadWorldMap against corrupted BSPs, with the real tr_bsp.c and
 * tr_curve.c; shaders, images and models are stubs. A refused map must drop
 * before tr, the world, the hunk or any shader changes. Also R_GetSurfMemory,
 * and R_FindLightmap extracted from tr_shader.c (LIGHTMAP_INC), and the light
 * grid sampler of the real tr_light.c. With BSP files as arguments, loads each
 * one, then truncates and mutates it. Run under ASan and UBSan. */
#include "../../src/renderer/tr_bsp.c"
#include "../../src/renderer/tr_light.c"
#include "bsp_fixture.h"

trGlobals_t tr;
glconfig_t glConfig;
refimport_t ri;
int skyboxportal;
static cvar_t zeroCvar, subdivisionsCvar, foliageCvar;
cvar_t *r_lightmap = &zeroCvar, *r_mapOverBrightBits = &zeroCvar, *r_singleShader = &zeroCvar;
cvar_t *r_subdivisions = &subdivisionsCvar, *r_drawfoliage = &foliageCvar;
static cvar_t ambientScaleCvar, directedScaleCvar;
cvar_t *r_ambientScale = &ambientScaleCvar, *r_directedScale = &directedScaleCvar, *r_debugLight = &zeroCvar;
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
int R_CullPointAndRadius( vec3_t origin, float radius ) { (void)origin; (void)radius; return CULL_IN; }
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

/* R_LightForPoint at every grid cell inside the grid, where a sample has all
 * eight corners: three offsets per axis, from the cell's low corner. Hashes the
 * bits of what it returns, to compare trees. */
static void LightGridDigest( const char *name ) {
	static const float offsets[] = { 0, 0.25f, 0.625f };
	const world_t *w = &s_worldData;
	unsigned int h = 2166136261u, k;
	int c[3], o[3], i, samples = 0;
	vec3_t point, ambient, directed, dir;
	float v;
	const byte *b;

	if ( !w->lightGridData ) {
		printf( "     %s: no light grid\n", name );
		return;
	}
	for ( c[2] = 0; c[2] < w->lightGridBounds[2] - 1; c[2]++ ) {
		for ( c[1] = 0; c[1] < w->lightGridBounds[1] - 1; c[1]++ ) {
			for ( c[0] = 0; c[0] < w->lightGridBounds[0] - 1; c[0]++ ) {
				for ( o[0] = 0; o[0] < 27; o[0]++ ) {
					o[1] = o[0];
					for ( i = 0; i < 3; i++, o[1] /= 3 ) {
						point[i] = w->lightGridOrigin[i] + ( c[i] + offsets[o[1] % 3] ) * w->lightGridSize[i];
						// in range as R_SetupEntityLightingGrid computes it
						v = ( point[i] - w->lightGridOrigin[i] ) * w->lightGridInverseSize[i];
						if ( !( v >= 0 && v < w->lightGridBounds[i] - 1 ) ) {
							break;
						}
					}
					if ( i < 3 ) {
						continue;
					}
					Check( R_LightForPoint( point, ambient, directed, dir ), "grid sample" );
					samples++;
					for ( i = 0; i < 3; i++ ) {
						b = (const byte *)&ambient[i];
						for ( k = 0; k < 4; k++ ) {
							h = ( h ^ b[k] ) * 16777619u;
						}
						b = (const byte *)&directed[i];
						for ( k = 0; k < 4; k++ ) {
							h = ( h ^ b[k] ) * 16777619u;
						}
						b = (const byte *)&dir[i];
						for ( k = 0; k < 4; k++ ) {
							h = ( h ^ b[k] ) * 16777619u;
						}
					}
				}
			}
		}
	}
	printf( "     %s: %dx%dx%d light grid, %d samples inside it, light digest %08x\n", name,
			w->lightGridBounds[0], w->lightGridBounds[1], w->lightGridBounds[2], samples, h );
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
	if ( s_worldData.numDecisionNodes == DEEP_NODES ) {
		mnode_t *n = s_worldData.nodes;
		Check( n[DEEP_NODES - 1].parent == &n[DEEP_NODES - 2] && n[1].parent == &n[0] && !n[0].parent, "deep chain parents" );
		Check( n[DEEP_NODES].parent == &n[0] && n[DEEP_NODES + 1].parent == &n[DEEP_NODES - 1], "shared leaf keeps the last parent in walk order" );
	}
	if ( realMaps ) {
		// parents and surface bounds of every node, to compare with the old recursive walk
		unsigned int h = 2166136261u, k;
		const byte *b;
		for ( i = 0; i < s_worldData.numnodes; i++ ) {
			mnode_t *n = &s_worldData.nodes[i];
			int parent = n->parent ? (int)( n->parent - s_worldData.nodes ) : -1;
			b = (const byte *)&parent;
			for ( k = 0; k < sizeof( parent ); k++ ) {
				h = ( h ^ b[k] ) * 16777619u;
			}
			b = (const byte *)n->surfMins;
			for ( k = 0; k < sizeof( vec3_t ); k++ ) {
				h = ( h ^ b[k] ) * 16777619u;
			}
			b = (const byte *)n->surfMaxs;
			for ( k = 0; k < sizeof( vec3_t ); k++ ) {
				h = ( h ^ b[k] ) * 16777619u;
			}
		}
		printf( "     %s: %d nodes, tree digest %08x\n", name, s_worldData.numnodes, h );
		// every grid after stitching: its size, points and LOD errors. A denormal
		// error (retail's read of the zone's trash tester) counts as 0, as it does
		// in RB_SurfaceGrid's `error <= lodError`
		{
			int grids = 0, j, count;
			float e;
			h = 2166136261u;
			for ( i = 0; i < s_worldData.numsurfaces; i++ ) {
				srfGridMesh_t *g = (srfGridMesh_t *)s_worldData.surfaces[i].data;
				if ( g->surfaceType != SF_GRID ) {
					continue;
				}
				grids++;
				for ( j = 0; j < 2 + g->width + g->height + g->width * g->height * 3; j++ ) {
					if ( j < 2 ) {
						count = j ? g->height : g->width;
						b = (const byte *)&count;
					} else if ( j < 2 + g->width + g->height ) {
						e = j < 2 + g->width ? g->widthLodError[j - 2] : g->heightLodError[j - 2 - g->width];
						if ( e > -FLT_MIN && e < FLT_MIN ) {
							e = 0;
						}
						b = (const byte *)&e;
					} else {
						count = j - 2 - g->width - g->height;
						b = (const byte *)&g->verts[count / 3].xyz[count % 3];
					}
					for ( k = 0; k < 4; k++ ) {
						h = ( h ^ b[k] ) * 16777619u;
					}
				}
			}
			printf( "     %s: %d grids, patch digest %08x\n", name, grids, h );
		}
		LightGridDigest( name );
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
/* A grid of exactly its own bytes, so ASan sees a read past it. Point p has
 * ambient 8p + 1 + channel and directed 100 + p + channel, light straight up:
 * linear in the grid coordinates, so a trilinear sample is that same line at
 * the sample's clamped coordinates. */
static void LightGrid( int x, int y, int z ) {
	static world_t world;
	int p, c;
	memset( &world, 0, sizeof( world ) );
	VectorSet( world.lightGridOrigin, -64, -64, 0 );
	VectorSet( world.lightGridSize, 64, 64, 128 );
	VectorSet( world.lightGridInverseSize, 1 / 64.0f, 1 / 64.0f, 1 / 128.0f );
	world.lightGridBounds[0] = x;
	world.lightGridBounds[1] = y;
	world.lightGridBounds[2] = z;
	world.lightGridData = malloc( x * y * z * 8 );
	ambientScaleCvar.value = 1;     // the grid's own bytes
	for ( p = 0; p < x * y * z; p++ ) {
		for ( c = 0; c < 3; c++ ) {
			world.lightGridData[p * 8 + c] = 8 * p + 1 + c;
			world.lightGridData[p * 8 + 3 + c] = 100 + p + c;
		}
		world.lightGridData[p * 8 + 6] = world.lightGridData[p * 8 + 7] = 0;
	}
	tr.world = &world;
}
static float GridClamp( float v, int bounds ) {
	return !( v > 0 ) ? 0 : v > bounds - 1 ? bounds - 1 : v;
}
/* A sample at grid coordinates gx, gy, gz must give the clamped coordinates' light. */
static void LightSample( float gx, float gy, float gz ) {
	const world_t *w = tr.world;
	vec3_t point, ambient, directed, dir;
	float p;
	int c;
	point[0] = w->lightGridOrigin[0] + gx * w->lightGridSize[0];
	point[1] = w->lightGridOrigin[1] + gy * w->lightGridSize[1];
	point[2] = w->lightGridOrigin[2] + gz * w->lightGridSize[2];
	Check( R_LightForPoint( point, ambient, directed, dir ), "grid sample" );
	p = GridClamp( gx, w->lightGridBounds[0] ) + w->lightGridBounds[0] * ( GridClamp( gy, w->lightGridBounds[1] ) +
		w->lightGridBounds[1] * GridClamp( gz, w->lightGridBounds[2] ) );
	for ( c = 0; c < 3; c++ ) {
		if ( !( fabs( ambient[c] - ( 8 * p + 1 + c ) ) < 0.001 && fabs( directed[c] - ( 100 + p + c ) ) < 0.001 ) ) {
			fprintf( stderr, "    sample %g %g %g: ambient %g, directed %g; the grid there has %g and %g\n",
					gx, gy, gz, ambient[c], directed[c], 8 * p + 1 + c, 100 + p + c );
			Fail( "light grid sample" );
		}
	}
	Check( fabs( dir[0] ) < 0.001 && fabs( dir[1] ) < 0.001 && fabs( dir[2] - 1 ) < 0.001, "light direction" );
}
static void T_LightGridEdge( void ) {
	static const float edge[] = { 2, 2.25f, 2.999f, 3, 7.5f, 1e30f };
	int i;
	LightGrid( 3, 3, 2 );
	// inside: plain trilinear, all eight corners
	LightSample( 0, 0, 0 );
	LightSample( 0.5f, 1.25f, 0.75f );
	LightSample( 1.75f, 1.5f, 0.25f );
	LightSample( 1.999f, 1.999f, 0.999f );
	// on the last plane of each axis and past it: that plane, nothing past it
	LightSample( 2, 2, 1 );
	LightSample( 2.5f, 2.5f, 1.5f );
	for ( i = 0; i < (int)( sizeof( edge ) / sizeof( edge[0] ) ); i++ ) {
		LightSample( edge[i], 0.5f, 0.5f );
		LightSample( 0.5f, edge[i], 0.5f );
		LightSample( 0.5f, 0.5f, edge[i] - 1 );
		LightSample( edge[i], edge[i], edge[i] - 1 );
	}
	// below the grid, and coordinates no int can hold
	LightSample( -0.25f, 0.5f, 0.5f );
	LightSample( -1e30f, -1e30f, -1e30f );
	LightSample( FLT_MAX, FLT_MAX, FLT_MAX );
	LightSample( INFINITY, -INFINITY, NAN );
	free( tr.world->lightGridData );
}
static void T_LightGridOnePoint( void ) {
	LightGrid( 1, 1, 1 );
	LightSample( 0, 0, 0 );
	LightSample( 0.5f, 0.5f, 0.5f );
	LightSample( -0.5f, -0.5f, -0.5f );
	LightSample( NAN, INFINITY, -INFINITY );
	free( tr.world->lightGridData );
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
	ambientScaleCvar.value = 0.5f;      // R_Register's defaults
	directedScaleCvar.value = 1;
	for ( i = 0; i < FUNCTABLE_SIZE; i++ ) {
		tr.sinTable[i] = sin( DEG2RAD( i * 360.0f / ( (float)( FUNCTABLE_SIZE - 1 ) ) ) );
	}
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
	failed += RunExtra( "light grid sampled at and past its upper edge", T_LightGridEdge );
	failed += RunExtra( "light grid of one point", T_LightGridOnePoint );
#ifdef LIGHTMAP_INC
	failed += RunExtra( "R_FindLightmap index past MAX_LIGHTMAPS", T_FindLightmapIndex );
	failed += RunExtra( "R_FindLightmap long world directory", T_FindLightmapLongDir );
#endif
	printf( "%d renderer failures\n", failed );
	return failed ? 1 : 0;
}
