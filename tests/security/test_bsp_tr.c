/* RE_LoadWorldMap against corrupted BSPs, with the real tr_bsp.c and
 * tr_curve.c; shaders, images and models are stubs. A refused map must drop
 * before tr, the world, the hunk, the zone or any shader changes. Also
 * R_GetSurfMemory, and R_FindLightmap extracted from tr_shader.c
 * (LIGHTMAP_INC), and the light grid sampler of the real tr_light.c. With BSP
 * files as arguments, loads each one, checks it is refused one byte under its
 * hunk cost and loads at exactly that cost, then truncates and mutates it.
 * Run under ASan and UBSan. */
#include "../../src/renderer/tr_bsp.c"
#include "../../src/renderer/tr_light.c"
#include "bsp_fixture.h"

trGlobals_t tr;
glconfig_t glConfig;
refimport_t ri;
int skyboxportal;
static cvar_t zeroCvar, oneCvar, subdivisionsCvar, foliageCvar;
cvar_t *r_lightmap = &zeroCvar, *r_mapOverBrightBits = &zeroCvar, *r_singleShader = &zeroCvar;
cvar_t *r_subdivisions = &subdivisionsCvar, *r_drawfoliage = &foliageCvar;
// image_t blocks come from the hunk unless both are set (R_CacheImageAlloc)
cvar_t *r_cache = &zeroCvar, *r_cacheShaders = &zeroCvar;
static cvar_t ambientScaleCvar, directedScaleCvar, mapOverBrightCvar;
cvar_t *r_ambientScale = &ambientScaleCvar, *r_directedScale = &directedScaleCvar, *r_debugLight = &zeroCvar;
static shader_t knownShader;
static int shaderCalls, imageCalls;
static int dropImage;       // R_CreateImage drops, as at MAX_DRAWIMAGES
static image_t knownImage;
static unsigned int imageHash;

/* The hunk's accounting, as common.c has it without HUNK_DEBUG (test_bsp_cm.c's).
 * Unless a budget is set, the hunk is as large as the test needs. With one, an
 * allocation past it drops the game as Hunk_Alloc does: halfway through the
 * load, with the file still open, unless the loader refused the map first. */
static int budgetActive, budgetLeft;
static unsigned int hunkCost;     // every allocation's cost, wrapping over a long run
static int TestAllocationSize( int size ) {
	return size < 0 || size > INT_MAX - 31 ? -1 : ( size + 31 ) & ~31;
}
static int TestMemoryRemaining( void ) {
	return budgetActive ? budgetLeft : 0x40000000;
}
static void *TestRiHunk( int size, ha_pref preference ) {
	int cost = TestAllocationSize( size );
	(void)preference;
	if ( cost < 0 || cost > TestMemoryRemaining() ) {
		Com_Error( ERR_DROP, "Hunk_Alloc failed on %i", cost );
	}
	if ( budgetActive ) {
		budgetLeft -= cost;
	}
	hunkCost += cost;
	return TestHunk( size );
}

/* The renderer's zone blocks, each charged its size and 128 bytes for
 * Z_TagMalloc's header and slack. Z_Malloc running out is fatal in the
 * engine, so here it fails the test; Z_TryMalloc answers NULL instead, and the
 * preflight must then refuse the map. */
#define TEST_ZONE_OVERHEAD 128
static int zoneLeft = 0x40000000, zoneLow = 0x40000000, zoneBlocks;
static void *TestZMalloc( int size ) {
	int *p;
	Check( size >= 0 && size <= zoneLeft - TEST_ZONE_OVERHEAD, "Z_Malloc past the zone: fatal in the engine" );
	zoneLeft -= size + TEST_ZONE_OVERHEAD;
	if ( zoneLeft < zoneLow ) {
		zoneLow = zoneLeft;
	}
	zoneBlocks++;
	p = calloc( 1, size + 16 );
	Check( p != NULL, "zone allocation" );
	p[0] = size;
	return (byte *)p + 16;
}
static void TestZFree( void *block ) {
	int *p = (int *)( (byte *)block - 16 );
	zoneLeft += p[0] + TEST_ZONE_OVERHEAD;
	zoneBlocks--;
	free( p );
}
static void *TestZTryMalloc( int size ) {
	if ( size < 0 || size > zoneLeft - TEST_ZONE_OVERHEAD ) {
		return NULL;    // Z_TryTagMalloc: no block large enough
	}
	return TestZMalloc( size );
}

static unsigned int Fnv( unsigned int h, const void *data, size_t size ) {
	const byte *b = data;
	size_t i;
	for ( i = 0; i < size; i++ ) {
		h = ( h ^ b[i] ) * 16777619u;
	}
	return h;
}

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
	mod = ri.Hunk_Alloc( sizeof( *mod ), h_low );
	mod->index = tr.numModels;
	tr.models[tr.numModels++] = mod;
	return mod;
}
image_t *R_CreateImage( const char *name, const byte *pic, int width, int height, qboolean mipmap, qboolean allowPicmip, int wrapClampMode ) {
	(void)name; (void)mipmap; (void)allowPicmip; (void)wrapClampMode;
	Check( pic != NULL && width == LIGHTMAP_SIZE && height == LIGHTMAP_SIZE, "lightmap upload" );
	if ( dropImage ) {
		Com_Error( ERR_DROP, "R_CreateImage: MAX_DRAWIMAGES hit" );
	}
	imageCalls++;
	imageHash = Fnv( imageHash, pic, width * height * 4 );
	// R_CacheImageAlloc's image_t
	if ( !( r_cache->integer && r_cacheShaders->integer ) ) {
		ri.Hunk_Alloc( sizeof( image_t ), h_low );
	}
	return &knownImage;
}
void R_RemapShader( const char *oldShader, const char *newShader, const char *timeOffset ) { (void)oldShader; (void)newShader; (void)timeOffset; }
void R_SetFog( int fogvar, int var1, int var2, float r, float g, float b, float density ) {
	(void)fogvar; (void)var1; (void)var2; (void)r; (void)g; (void)b; (void)density;
}
void R_SyncRenderThread( void ) {}
int R_CullPointAndRadius( vec3_t origin, float radius ) { (void)origin; (void)radius; return CULL_IN; }
static void QDECL TestPrintf( int level, const char *fmt, ... ) { (void)level; (void)fmt; }
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
	imageHash = 2166136261u;
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

static unsigned int GenericDigest( unsigned int h, const srfGeneric_t *g ) {
	h = Fnv( h, g->bounds, sizeof( g->bounds ) );
	h = Fnv( h, g->origin, sizeof( g->origin ) );
	h = Fnv( h, &g->radius, sizeof( g->radius ) );
	h = Fnv( h, g->plane.normal, sizeof( g->plane.normal ) );
	h = Fnv( h, &g->plane.dist, sizeof( g->plane.dist ) );
	h = Fnv( h, &g->plane.type, 1 );
	return Fnv( h, &g->plane.signbits, 1 );
}

/* What the loader published for every surface, lightmap, plane, fog and
 * model, and the light grid, to compare trees: same bits, same world. */
static unsigned int WorldDigest( const char *name ) {
	const world_t *w = &s_worldData;
	unsigned int h = 2166136261u;
	int i, n;

	for ( i = 0; i < w->numsurfaces; i++ ) {
		const msurface_t *surf = &w->surfaces[i];
		int shader = surf->shader == &knownShader ? 1 : surf->shader == tr.defaultShader ? 2 : 0;
		h = Fnv( h, &surf->fogIndex, sizeof( surf->fogIndex ) );
		h = Fnv( h, &shader, sizeof( shader ) );
		h = Fnv( h, surf->data, sizeof( *surf->data ) );
		switch ( *surf->data ) {
		case SF_TRIANGLES: {
			const srfTriangles_t *t = (const srfTriangles_t *)surf->data;
			h = GenericDigest( h, (const srfGeneric_t *)t );
			h = Fnv( h, &t->numVerts, sizeof( t->numVerts ) );
			h = Fnv( h, &t->numIndexes, sizeof( t->numIndexes ) );
			h = Fnv( h, t->verts, t->numVerts * sizeof( t->verts[0] ) );
			h = Fnv( h, t->indexes, t->numIndexes * sizeof( t->indexes[0] ) );
			break;
		}
		case SF_GRID: {
			const srfGridMesh_t *g = (const srfGridMesh_t *)surf->data;
			h = GenericDigest( h, (const srfGeneric_t *)g );
			h = Fnv( h, g->lodOrigin, sizeof( g->lodOrigin ) );
			h = Fnv( h, &g->lodRadius, sizeof( g->lodRadius ) );
			h = Fnv( h, &g->lodFixed, sizeof( g->lodFixed ) );
			h = Fnv( h, &g->lodStitched, sizeof( g->lodStitched ) );
			h = Fnv( h, &g->width, sizeof( g->width ) );
			h = Fnv( h, &g->height, sizeof( g->height ) );
			h = Fnv( h, g->widthLodError, g->width * sizeof( float ) );
			h = Fnv( h, g->heightLodError, g->height * sizeof( float ) );
			h = Fnv( h, g->verts, g->width * g->height * sizeof( g->verts[0] ) );
			break;
		}
		case SF_FOLIAGE: {
			const srfFoliage_t *f = (const srfFoliage_t *)surf->data;
			h = GenericDigest( h, (const srfGeneric_t *)f );
			h = Fnv( h, &f->numVerts, sizeof( f->numVerts ) );
			h = Fnv( h, &f->numIndexes, sizeof( f->numIndexes ) );
			h = Fnv( h, &f->numInstances, sizeof( f->numInstances ) );
			h = Fnv( h, f->xyz, f->numVerts * sizeof( f->xyz[0] ) );
			h = Fnv( h, f->normal, f->numVerts * sizeof( f->normal[0] ) );
			h = Fnv( h, f->texCoords, f->numVerts * sizeof( f->texCoords[0] ) );
			h = Fnv( h, f->lmTexCoords, f->numVerts * sizeof( f->lmTexCoords[0] ) );
			h = Fnv( h, f->indexes, f->numIndexes * sizeof( f->indexes[0] ) );
			for ( n = 0; n < f->numInstances; n++ ) {
				h = Fnv( h, f->instances[n].origin, sizeof( f->instances[n].origin ) );
				h = Fnv( h, f->instances[n].color, sizeof( f->instances[n].color ) );
			}
			break;
		}
		case SF_FLARE: {
			const srfFlare_t *fl = (const srfFlare_t *)surf->data;
			h = Fnv( h, fl->origin, sizeof( fl->origin ) );
			h = Fnv( h, fl->normal, sizeof( fl->normal ) );
			h = Fnv( h, fl->color, sizeof( fl->color ) );
			break;
		}
		default:
			break;
		}
	}
	for ( i = 0; i < w->numplanes; i++ ) {
		h = Fnv( h, w->planes[i].normal, sizeof( w->planes[i].normal ) );
		h = Fnv( h, &w->planes[i].dist, sizeof( w->planes[i].dist ) );
		h = Fnv( h, &w->planes[i].type, 1 );
		h = Fnv( h, &w->planes[i].signbits, 1 );
	}
	for ( i = 0; i < w->numfogs; i++ ) {
		h = Fnv( h, &w->fogs[i].originalBrushNumber, sizeof( w->fogs[i].originalBrushNumber ) );
		h = Fnv( h, w->fogs[i].bounds, sizeof( w->fogs[i].bounds ) );
	}
	for ( i = 0; i < w->numBModels; i++ ) {
		n = w->bmodels[i].firstSurface - w->surfaces;
		h = Fnv( h, w->bmodels[i].bounds, sizeof( w->bmodels[i].bounds ) );
		h = Fnv( h, &n, sizeof( n ) );
		h = Fnv( h, &w->bmodels[i].numSurfaces, sizeof( w->bmodels[i].numSurfaces ) );
	}
	h = Fnv( h, w->lightGridOrigin, sizeof( w->lightGridOrigin ) );
	h = Fnv( h, w->lightGridSize, sizeof( w->lightGridSize ) );
	h = Fnv( h, w->lightGridInverseSize, sizeof( w->lightGridInverseSize ) );
	h = Fnv( h, w->lightGridBounds, sizeof( w->lightGridBounds ) );
	if ( w->lightGridData ) {
		h = Fnv( h, w->lightGridData, w->lightGridBounds[0] * w->lightGridBounds[1] * w->lightGridBounds[2] * 8 );
	}
	if ( name ) {
		printf( "     %s: %d surfaces, %d lightmaps, world digest %08x, lightmap digest %08x\n",
				name, w->numsurfaces, tr.numLightmaps, h, imageHash );
	}
	return h;
}

static void LoaderReject( const char *name );

/* A loaded map's hunk cost is exactly its budget: one byte less, or none, is
 * refused with nothing changed, and at the cost it loads and takes all of it. */
static void BudgetBoundary( const char *name, int cost ) {
	budgetActive = 1;
	budgetLeft = cost - 1;
	LoaderReject( name );
	Check( strstr( lastError, "hunk memory" ) != NULL, "refused for the hunk budget" );
	Check( budgetLeft == cost - 1, "a refused map takes nothing from the hunk" );
	budgetLeft = 0;
	LoaderReject( name );
	budgetLeft = cost;
	Check( LoaderTry( name ), "a map loads in exactly its own hunk cost" );
	Check( budgetLeft == 0, "the load took exactly the cost it was charged" );
	budgetActive = 0;
}

static int LoaderAccept( const char *name ) {
	int i, zone = zoneBlocks;
	unsigned int cost = hunkCost;
	if ( !LoaderTry( name ) ) {
		printf( "    unexpected drop: %s\n", lastError );
		return 0;
	}
	cost = hunkCost - cost;
	// fewer when it freed what an earlier dropped load left
	Check( zoneBlocks <= zone, "every zone grid was moved to the hunk and freed" );
	{
		// the budget charges the walk stacks at BSP_TreeDepth; the load sizes them by R_SetParents
		dheader_t h;
		unsigned int depth;
		Check( !BSP_ValidateHeader( source, sourceSize, &h ) && !BSP_TreeDepth( source, &h, &depth ) &&
			   (int)depth == s_worldData.nodeDepth, "BSP_TreeDepth and R_SetParents give the same depth" );
		if ( realMaps ) {
			printf( "     %s: tree depth %d\n", name, s_worldData.nodeDepth );
		}
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
		h = WorldDigest( name );
		printf( "     %s: renderer hunk %u bytes, file %d bytes\n", name, cost, sourceSize );
		BudgetBoundary( name, cost );
		Check( WorldDigest( NULL ) == h, "the same world at its exact budget" );
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
	int zone = zoneBlocks, zoneFree = zoneLeft;

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
	Check( zoneBlocks == zone && zoneLeft == zoneFree, "the preflight's zone grids were freed" );
	Check( shaderCalls == shaders && imageCalls == images, "no shader or image before the drop" );
	Check( !memcmp( &tr, &trBefore, sizeof( tr ) ), "renderer globals unchanged" );
	Check( !memcmp( &s_worldData, &worldBefore, sizeof( s_worldData ) ), "previous world unchanged" );
}

/* ---- renderer-only cases ---- */

static void M_ModelSlots( void ) { tr.numModels = MAX_MOD_KNOWN - 1; }
static void M_ModelSlotsFit( void ) { tr.numModels = MAX_MOD_KNOWN - 2; }

/* Finite input whose renderer arithmetic overflows (Q3A 8bc70a8a, cc655e82).
 * The fixture's 3x3 patch, vertices 7-15, has its middle control raised, so
 * it keeps all nine points; the planar quad is vertices 0-3, the triangle 4-6,
 * the foliage mesh 16-18 with instance origins 19 and 20. */
#define VERT( n, field ) ( LumpOfs( LUMP_DRAWVERTS ) + ( n ) * sizeof( drawVert_t ) + offsetof( drawVert_t, field ) )
#define SURF_VEC( n, v, axis ) ( AT( LUMP_SURFACES, dsurface_t, n, lightmapVecs ) + ( ( v ) * 3 + ( axis ) ) * 4 )
static void M_PatchMidpoint( void ) {
	int i;
	for ( i = 7; i < 16; i++ ) {
		Float( VERT( i, xyz ), FLT_MAX );   // ( a + 2b + c ) / 4 overflows before the divide
	}
}
static void M_PatchTexCoords( void ) {
	int i;
	for ( i = 7; i < 16; i++ ) {
		Float( VERT( i, st ), FLT_MAX );    // the points put on the curve average two of them
	}
}
static void M_PatchDeviation( void ) {
	// the first and last controls of a row FLT_MAX apart each way
	Float( VERT( 7, xyz ), -FLT_MAX );
	Float( VERT( 9, xyz ), FLT_MAX );
	Float( VERT( 8, xyz ), 0 );
}
static void M_PatchLodBounds( void ) {
	Float( SURF_VEC( S_PATCH, 0, 0 ), FLT_MAX );
	Float( SURF_VEC( S_PATCH, 1, 0 ), FLT_MAX );
}
static void M_PatchFinite( void ) {
	// far but representable: the same patch 1e18 away on x still loads. (Past
	// about 1.8e19 its sphere overflows: ClearBounds starts at +-99999, so the
	// bounds always reach back to there, and VectorLength squares)
	int i;
	for ( i = 7; i < 16; i++ ) {
		Float( VERT( i, xyz ), 1e18f );
	}
	Float( SURF_VEC( S_PATCH, 0, 0 ), 1e18f );
	Float( SURF_VEC( S_PATCH, 1, 0 ), 1e18f );
}
static void M_PlanarPlaneDist( void ) {
	// a far quad, whose plane distance is 1e18 * 1e21
	int i;
	for ( i = 0; i < 4; i++ ) {
		Float( VERT( i, xyz ), 1e18f );
	}
	Float( SURF_VEC( S_PLANAR, 2, 0 ), 1e21f );
}
static void M_PlanarPlaneFar( void ) {
	int i;
	for ( i = 0; i < 4; i++ ) {
		Float( VERT( i, xyz ), 1e18f );
	}
	Float( SURF_VEC( S_PLANAR, 2, 0 ), 1e20f );     // 1e38 fits
}
static void M_TriangleSphere( void ) {
	// a triangle FLT_MAX each way: the sphere's radius squares past FLT_MAX
	Float( VERT( 4, xyz ), -FLT_MAX );
	Float( VERT( 5, xyz ), FLT_MAX );
}
static void M_FoliageInstanceFar( void ) { Float( VERT( 19, xyz ), FLT_MAX ); }

/* Surfaces against the tessellator: RB_CHECKOVERFLOW drops a surface of
 * SHADER_MAX_VERTEXES vertices or SHADER_MAX_INDEXES indexes when it is drawn. */
static void PlanarVerts( int surf, int count ) {
	GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + count );
	SET( LUMP_SURFACES, dsurface_t, surf, firstVert, 21 );
	SET( LUMP_SURFACES, dsurface_t, surf, numVerts, count );
}
static void TriIndexes( int count ) {
	GrowLump( LUMP_DRAWINDEXES, 4, 12 + count );    // new indexes are vertex 0
	SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, firstIndex, 12 );
	SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, numIndexes, count );
}
static void M_PlanarCap( void ) { PlanarVerts( S_PLANAR, SHADER_MAX_VERTEXES ); }
static void M_PlanarCapFits( void ) { PlanarVerts( S_PLANAR, SHADER_MAX_VERTEXES - 1 ); }
static void M_TriIndexCap( void ) { TriIndexes( SHADER_MAX_INDEXES ); }
static void M_TriCapFits( void ) {
	PlanarVerts( S_TRISOUP, SHADER_MAX_VERTEXES - 1 );
	TriIndexes( SHADER_MAX_INDEXES - 3 );
}
static void M_FoliageMeshCap( void ) {
	GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + SHADER_MAX_VERTEXES + 2 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, firstVert, 21 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, numVerts, SHADER_MAX_VERTEXES + 2 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchHeight, SHADER_MAX_VERTEXES );
}
static void M_FoliageInstancesFit( void ) {
	// radar has a foliage surface of 17 mesh vertices and 996 instances, 1,013
	// vertices in all: each instance is drawn alone, so only the mesh counts
	GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 16 + 3 + 1100 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, numVerts, 3 + 1100 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth, 1100 );
}

static const bspCase_t rendererCases[] = {
	{ "submodels past the free model slots", M_ModelSlots, SKIP, REJECT },
	{ "submodels exactly filling the model slots", M_ModelSlotsFit, SKIP, ACCEPT },
	{ "patch midpoint overflows", M_PatchMidpoint, SKIP, REJECT },
	{ "patch texture coordinates overflow on the curve", M_PatchTexCoords, SKIP, REJECT },
	{ "patch row spans more than FLT_MAX", M_PatchDeviation, SKIP, REJECT },
	{ "patch LOD bounds overflow", M_PatchLodBounds, SKIP, REJECT },
	{ "patch 1e18 away", M_PatchFinite, SKIP, ACCEPT },
	{ "planar plane distance overflows", M_PlanarPlaneDist, SKIP, REJECT },
	{ "planar plane distance of 1e38", M_PlanarPlaneFar, SKIP, ACCEPT },
	{ "triangle bounding sphere overflows", M_TriangleSphere, SKIP, REJECT },
	{ "foliage instance FLT_MAX away: its sphere overflows", M_FoliageInstanceFar, SKIP, REJECT },
	{ "planar surface of SHADER_MAX_VERTEXES", M_PlanarCap, SKIP, REJECT },
	{ "planar surface one vertex under the tessellator", M_PlanarCapFits, SKIP, ACCEPT },
	{ "triangle surface of SHADER_MAX_INDEXES", M_TriIndexCap, SKIP, REJECT },
	{ "triangle surface just under both caps", M_TriCapFits, SKIP, ACCEPT },
	{ "foliage mesh of SHADER_MAX_VERTEXES", M_FoliageMeshCap, SKIP, REJECT },
	{ "foliage of 1,103 vertices with its instances", M_FoliageInstancesFit, SKIP, ACCEPT },
};

/* ---- hunk and zone budget cases ---- */

/* A 3x3 patch on fresh vertices from first, x 0 to 64 and y 0 to 64 times
 * side: flat, but for a bump of the given height in the middle of its far row,
 * which decides how finely its columns are cut. */
static void Patch3x3( int surf, int first, float side, float bump ) {
	int i;
	for ( i = 0; i < 9; i++ ) {
		Vertex( first + i, ( i % 3 ) * 32.0f, side * ( i / 3 ) * 32.0f, i == 7 ? bump : 0 );
	}
	SET( LUMP_SURFACES, dsurface_t, surf, firstVert, first );
	SET( LUMP_SURFACES, dsurface_t, surf, numVerts, 9 );
}
/* The fixture's patch and a sixth surface, a coarser patch sharing its y = 0
 * edge. In one LOD group, stitching inserts the finer edge's points into the
 * coarser grid, so its hunk copy is larger than the grid ParseMesh built. */
static void PatchPair( int oneGroup ) {
	unsigned int o;
	int j;
	GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + 18 );
	o = GrowLump( LUMP_SURFACES, sizeof( dsurface_t ), NUM_SURFS + 1 );
	memcpy( source + o + NUM_SURFS * sizeof( dsurface_t ), source + o + S_PATCH * sizeof( dsurface_t ), sizeof( dsurface_t ) );
	SET( LUMP_MODELS, dmodel_t, 0, numSurfaces, NUM_SURFS + 1 );
	Patch3x3( S_PATCH, 21, 1, 16 );
	Patch3x3( NUM_SURFS, 30, -1, 4 );
	for ( j = 0; j < 3; j++ ) {
		Float( SURF_VEC( S_PATCH, 0, j ), -64 );
		Float( SURF_VEC( S_PATCH, 1, j ), 64 );
		Float( SURF_VEC( NUM_SURFS, 0, j ), oneGroup ? -64 : -32 );
		Float( SURF_VEC( NUM_SURFS, 1, j ), 64 );
	}
}

static void B_Fixture( void ) {}
static void B_CachedImages( void ) { r_cache = r_cacheShaders = &oneCvar; }
static void B_OneLightmap( void ) { Lump( LUMP_LIGHTMAPS, LumpOfs( LUMP_LIGHTMAPS ), LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3 ); }
static void B_NoLightmaps( void ) { Lump( LUMP_LIGHTMAPS, 0, 0 ); }
static void B_SurfaceChunks( void ) {
	// twelve 1,000-vertex planar surfaces fill R_GetSurfMemory's 256 KB chunk
	// and start more; 20,000 foliage instances need a block larger than a chunk
	unsigned int o, i;
	GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + 1000 + 20003 );
	SET( LUMP_SURFACES, dsurface_t, S_PLANAR, firstVert, 21 );
	SET( LUMP_SURFACES, dsurface_t, S_PLANAR, numVerts, 1000 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, firstVert, 21 + 1000 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, numVerts, 20003 );
	SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth, 20000 );
	o = GrowLump( LUMP_SURFACES, sizeof( dsurface_t ), NUM_SURFS + 11 );
	for ( i = NUM_SURFS; i < NUM_SURFS + 11; i++ ) {
		memcpy( source + o + i * sizeof( dsurface_t ), source + o + S_PLANAR * sizeof( dsurface_t ), sizeof( dsurface_t ) );
	}
}
static void B_NodrawPatch( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, shaderNum, 1 ); }
static void B_StitchedPatches( void ) { PatchPair( 1 ); }
static const byte sharedVis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static void B_SharedVis( void ) { tr.externalVisData = sharedVis; }
static void B_NoVis( void ) {
	Lump( LUMP_VISIBILITY, 0, 0 );
	SET( LUMP_LEAFS, dleaf_t, 1, cluster, 1 );      // two clusters, 64 bytes of novis rows
}

typedef struct {
	const char *name;
	void ( *mutate )( void );
} budgetCase_t;

static const budgetCase_t budgetCases[] = {
	{ "hunk budget: fixture map, lightmap images on the hunk", B_Fixture },
	{ "hunk budget: lightmap images cached off the hunk", B_CachedImages },
	{ "hunk budget: single-lightmap hack's second image", B_OneLightmap },
	{ "hunk budget: no lightmaps", B_NoLightmaps },
	{ "hunk budget: surfaces across R_GetSurfMemory chunks", B_SurfaceChunks },
	{ "hunk budget: nodraw patch, no grid", B_NodrawPatch },
	{ "hunk budget: stitching grows a patch grid", B_StitchedPatches },
	{ "hunk budget: visibility shared with the collision map", B_SharedVis },
	{ "hunk budget: novis rows", B_NoVis },
};

static srfGridMesh_t *SurfaceGrid( int n ) {
	srfGridMesh_t *g = (srfGridMesh_t *)s_worldData.surfaces[n].data;
	Check( g->surfaceType == SF_GRID, "patch grid" );
	return g;
}

static void BudgetCase( const budgetCase_t *c ) {
	unsigned int cost;
	int width;

	BuildMap();
	Check( LoaderAccept( "maps/previous.bsp" ), "the unmutated map loads" );
	c->mutate();
	cost = hunkCost;
	if ( !LoaderTry( "maps/budget.bsp" ) ) {
		printf( "    dropped: %s\n", lastError );
		Fail( "the map loads with room to spare" );
	}
	cost = hunkCost - cost;
	printf( "    %u hunk bytes, %d surfaces, %d lightmaps\n", cost, s_worldData.numsurfaces, tr.numLightmaps );
	BudgetBoundary( "maps/budget.bsp", cost );
	if ( c->mutate == B_SurfaceChunks ) {
		Check( s_worldData.numsurfaces == NUM_SURFS + 11 && ( (srfFoliage_t *)s_worldData.surfaces[S_FOLIAGE].data )->numInstances == 20000, "every surface loaded" );
	} else if ( c->mutate == B_NodrawPatch ) {
		Check( s_worldData.surfaces[S_PATCH].data == &skipData, "the nodraw patch has no grid" );
	} else if ( c->mutate == B_SharedVis ) {
		Check( s_worldData.vis == sharedVis, "the collision map's rows are used" );
	} else if ( c->mutate == B_NoVis ) {
		Check( s_worldData.numClusters == 2 && !s_worldData.vis, "novis rows only" );
	} else if ( c->mutate == B_StitchedPatches ) {
		// the same two patches in two LOD groups are not stitched
		width = SurfaceGrid( NUM_SURFS )->width;
		printf( "    stitched: %dx%d and %dx%d grids\n", SurfaceGrid( S_PATCH )->width, SurfaceGrid( S_PATCH )->height,
				width, SurfaceGrid( NUM_SURFS )->height );
		BuildMap();
		PatchPair( 0 );
		cost = hunkCost;
		Check( LoaderTry( "maps/apart.bsp" ), "the patches load apart" );
		cost = hunkCost - cost;
		printf( "    apart: %dx%d and %dx%d grids, %u hunk bytes\n", SurfaceGrid( S_PATCH )->width, SurfaceGrid( S_PATCH )->height,
				SurfaceGrid( NUM_SURFS )->width, SurfaceGrid( NUM_SURFS )->height, cost );
		Check( width > SurfaceGrid( NUM_SURFS )->width, "stitching widened the coarser grid" );
	}
}

/* A patch grid the zone has no block for is refused through Z_TryMalloc, not
 * by a fatal Z_Malloc; with exactly the room it takes, the map loads. */
static void ZoneCase( void ) {
	int need;

	BuildMap();
	zoneLow = zoneLeft;
	Check( LoaderAccept( "maps/previous.bsp" ), "the unmutated map loads" );
	need = zoneLeft - zoneLow;
	printf( "    the patch grid takes %d zone bytes\n", need );
	zoneLeft = need - 1;
	LoaderReject( "maps/zone.bsp" );
	Check( strstr( lastError, "zone memory" ) != NULL, "refused for the zone" );
	zoneLeft = need;
	Check( LoaderTry( "maps/zone.bsp" ), "the map loads with exactly that zone" );
	Check( zoneLeft == need, "the grid went to the hunk and left the zone" );
}

/* The stitch pair and 100 copies of its coarse patch, all in one LOD group:
 * stitching widens each copy after it was built. Where the zone holds the
 * built grids but not every wider one, Z_TryMalloc's NULL refuses the map
 * (the review's window was 97 to 120 KB); with room, it loads. */
#define STITCH_COPIES 100
static void StitchZoneCase( void ) {
	static const int sizes[] = { 97 * 1024, 100 * 1024, 110 * 1024, 120 * 1024 };
	unsigned int o, i;
	int start = zoneLeft, k, zone;

	BuildMap();
	PatchPair( 1 );
	o = GrowLump( LUMP_SURFACES, sizeof( dsurface_t ), NUM_SURFS + 1 + STITCH_COPIES );
	for ( i = NUM_SURFS + 1; i < NUM_SURFS + 1 + STITCH_COPIES; i++ ) {
		memcpy( source + o + i * sizeof( dsurface_t ), source + o + NUM_SURFS * sizeof( dsurface_t ), sizeof( dsurface_t ) );
	}
	SET( LUMP_MODELS, dmodel_t, 0, numSurfaces, NUM_SURFS + 1 + STITCH_COPIES );
	for ( k = 0; k < (int)( sizeof( sizes ) / sizeof( sizes[0] ) ); k++ ) {
		zoneLeft = sizes[k];
		zone = zoneBlocks;
		LoaderReject( "maps/stitchzone.bsp" );
		Check( strstr( lastError, "zone memory" ) != NULL, "refused for the zone" );
		Check( zoneBlocks == zone && zoneLeft == sizes[k], "every grid built or widened was freed" );
		printf( "    %d KB of zone: %s\n", sizes[k] / 1024, lastError );
	}
	zoneLeft = start;
	zoneLow = start;
	Check( LoaderAccept( "maps/stitchzone.bsp" ), "the map loads with room" );
	printf( "    with room: %d zone bytes at the peak, %dx%d grids for the copies\n", start - zoneLow,
			SurfaceGrid( NUM_SURFS + 1 )->width, SurfaceGrid( NUM_SURFS + 1 )->height );
	Check( SurfaceGrid( NUM_SURFS + STITCH_COPIES )->width == 5, "every copy was widened" );
}

/* A drop after the preflight built the grids (MAX_DRAWIMAGES in
 * R_LoadLightmaps) leaves them in the zone; the next load frees them. */

static void LeakCase( void ) {
	int zone, ok;

	BuildMap();
	PatchPair( 1 );
	zone = zoneBlocks;
	dropImage = 1;
	ok = LoaderTry( "maps/leak.bsp" );
	dropImage = 0;
	Check( !ok && strstr( lastError, "MAX_DRAWIMAGES" ) != NULL, "the load dropped after the preflight" );
	printf( "    %d zone blocks left by the dropped load\n", zoneBlocks - zone );
	Check( zoneBlocks > zone, "the dropped load left its grids" );
	Check( LoaderAccept( "maps/leak.bsp" ), "the next load succeeds" );
	Check( zoneBlocks == zone, "no stale zone blocks after the next load" );
	// and a refused map frees what an earlier drop left as well
	dropImage = 1;
	Check( !LoaderTry( "maps/leak.bsp" ), "the load dropped again" );
	dropImage = 0;
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, surfaceType, 99 );
	expectDrop = 1;
	if ( !setjmp( dropJump ) ) {
		tr.worldMapLoaded = qfalse;
		RE_LoadWorldMap( "maps/bad.bsp" );
		Fail( "a bad surface type loaded" );
	}
	expectDrop = 0;
	Check( fileBlock == NULL, "the file was freed" );
	Check( zoneBlocks == zone, "no stale zone blocks after a refused map" );
}

typedef struct {
	const char *name;
	void ( *run )( void );
} extraBudgetCase_t;

static const extraBudgetCase_t extraBudgetCases[] = {
	{ "zone budget: patch grid one byte past the zone left", ZoneCase },
	{ "zone budget: stitching widens grids past the zone left", StitchZoneCase },
	{ "zone grids of a load that dropped after the preflight are freed", LeakCase },
};

static int RunBudgetCase( const char *name, const budgetCase_t *c, void ( *run )( void ) ) {
	pid_t pid;
	int status;
	printf( "case: %s\n", name );
	fflush( stdout );
	pid = fork();
	if ( pid == 0 ) {
		alarm( 60 );
		if ( c ) {
			BudgetCase( c );
		} else {
			run();
		}
		fflush( stdout );
		exit( 0 );
	}
	waitpid( pid, &status, 0 );
	if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
		printf( "PASS %s\n", name );
		return 0;
	}
	printf( "FAIL %s (%s %d)\n", name, WIFSIGNALED( status ) ? "signal" : "exit",
			WIFSIGNALED( status ) ? WTERMSIG( status ) : WEXITSTATUS( status ) );
	return 1;
}

static int RunBudgetCases( void ) {
	unsigned int i;
	int failed = 0, total = sizeof( budgetCases ) / sizeof( budgetCases[0] ) + sizeof( extraBudgetCases ) / sizeof( extraBudgetCases[0] );
	for ( i = 0; i < sizeof( budgetCases ) / sizeof( budgetCases[0] ); i++ ) {
		failed += RunBudgetCase( budgetCases[i].name, &budgetCases[i], NULL );
	}
	for ( i = 0; i < sizeof( extraBudgetCases ) / sizeof( extraBudgetCases[0] ); i++ ) {
		failed += RunBudgetCase( extraBudgetCases[i].name, NULL, extraBudgetCases[i].run );
	}
	printf( "%d of %d budget cases passed\n", total - failed, total );
	return failed;
}

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

/* q3map writes a fractional grid by float division before ceil and floor:
 * gridsize 0.1 over one unit is 11 points an axis, where doubles give 10.
 * The preflight keeps that (Q3A 7ff722bb, d94876f4; here since PR #31). */
static void T_GridFraction( void ) {
	int j;
	unsigned int o;
	BuildMap();
	SetEntities( "{ \"gridsize\" \"0.1 0.1 128\" }", 1 );
	for ( j = 0; j < 2; j++ ) {
		Float( AT( LUMP_MODELS, dmodel_t, 0, mins ) + j * 4, 0 );
		Float( AT( LUMP_MODELS, dmodel_t, 0, maxs ) + j * 4, 1 );
	}
	o = Append( LUMP_LIGHTGRID, 11 * 11 * 8, 0 );
	memset( source + o, 0x20, 11 * 11 * 8 );
	Check( LoaderAccept( "maps/grid.bsp" ), "fractional grid map loads" );
	Check( s_worldData.lightGridBounds[0] == 11 && s_worldData.lightGridBounds[1] == 11 && s_worldData.lightGridBounds[2] == 1 &&
		   s_worldData.lightGridData != NULL, "gridsize 0.1 keeps q3map's 11 points an axis" );
}

/* The old shift, in 64 bits so it is defined at every shift it was defined at
 * in int (0 to 14), and beyond; a negative shift dims. */
static void ShiftReference( long long shift, const byte in[3], byte out[3] ) {
	long long c[3], max;
	int i;
	for ( i = 0; i < 3; i++ ) {
		c[i] = shift < 0 ? in[i] >> ( shift < -31 ? 31 : -shift ) : (long long)in[i] << ( shift > 40 ? 40 : shift );
	}
	if ( ( c[0] | c[1] | c[2] ) > 255 ) {
		max = c[0] > c[1] ? c[0] : c[1];
		max = max > c[2] ? max : c[2];
		for ( i = 0; i < 3; i++ ) {
			c[i] = c[i] * 255 / max;
		}
	}
	for ( i = 0; i < 3; i++ ) {
		out[i] = (byte)c[i];
	}
}
/* r_mapOverBrightBits is any integer, tr.overbrightBits 0 to 2. A negative
 * shift or an overflowing difference was undefined (Q3A c743f116). */
static void T_ColorShift( void ) {
	static const int settings[][2] = {
		{ 2, 0 }, { 2, 1 }, { 2, 2 }, { 1, 0 }, { 0, 0 }, { 3, 0 }, { 8, 0 }, { 9, 0 }, { 14, 0 },
		{ 1, 2 }, { 0, 1 }, { 0, 2 }, { -1, 0 }, { -7, 1 }, { 40, 0 },
		{ INT_MAX, 0 }, { INT_MAX, 2 }, { INT_MIN, 0 }, { INT_MIN, 2 }, { INT_MAX, INT_MIN }, { INT_MIN, INT_MAX },
	};
	static const byte blues[] = { 0, 1, 63, 64, 127, 128, 200, 255 };
	byte in[4], out[4], want[3];
	int s, r, g, b;
	r_mapOverBrightBits = &mapOverBrightCvar;
	for ( s = 0; s < (int)( sizeof( settings ) / sizeof( settings[0] ) ); s++ ) {
		mapOverBrightCvar.integer = settings[s][0];
		tr.overbrightBits = settings[s][1];
		for ( r = 0; r < 256; r++ ) {
			for ( g = 0; g < 256; g++ ) {
				for ( b = 0; b < (int)sizeof( blues ); b++ ) {
					in[0] = r;
					in[1] = g;
					in[2] = blues[b];
					in[3] = 77;
					ShiftReference( (long long)settings[s][0] - settings[s][1], in, want );
					R_ColorShiftLightingBytes( in, out );
					if ( memcmp( out, want, 3 ) || out[3] != 77 ) {
						fprintf( stderr, "    r_mapOverBrightBits %d, overbrightBits %d: %d %d %d gives %d %d %d %d, not %d %d %d 77\n",
								settings[s][0], settings[s][1], in[0], in[1], in[2], out[0], out[1], out[2], out[3], want[0], want[1], want[2] );
						Fail( "color shift" );
					}
					// in place, as R_LoadLightGrid shifts its samples
					R_ColorShiftLightingBytes( in, in );
					Check( !memcmp( in, out, 4 ), "color shift in place" );
				}
			}
		}
	}
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
	ri.Z_Malloc = TestZMalloc;
	ri.Free = TestZFree;
	ri.Hunk_MemoryRemaining = TestMemoryRemaining;
	ri.Hunk_AllocationSize = TestAllocationSize;
	ri.Z_TryMalloc = TestZTryMalloc;
	oneCvar.integer = 1;
	ri.FS_ReadFile = FS_ReadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.Cmd_ExecuteText = TestExecuteText;
	ambientScaleCvar.value = 0.5f;      // R_Register's defaults
	directedScaleCvar.value = 1;
	// to compare trees at other overbright settings: BSP_MAP_OVERBRIGHT, BSP_OVERBRIGHT
	if ( getenv( "BSP_MAP_OVERBRIGHT" ) ) {
		mapOverBrightCvar.integer = atoi( getenv( "BSP_MAP_OVERBRIGHT" ) );
		r_mapOverBrightBits = &mapOverBrightCvar;
	}
	// BSP_R_CACHE: lightmap images off the hunk, as at the default r_cache 1 and r_cacheShaders 1
	if ( getenv( "BSP_R_CACHE" ) && atoi( getenv( "BSP_R_CACHE" ) ) ) {
		r_cache = r_cacheShaders = &oneCvar;
	}
	if ( getenv( "BSP_OVERBRIGHT" ) ) {
		tr.overbrightBits = atoi( getenv( "BSP_OVERBRIGHT" ) );
	}
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
	failed += RunExtra( "overbright shift for every r_mapOverBrightBits", T_ColorShift );
	failed += RunExtra( "gridsize 0.1 over one unit, as q3map rounds it", T_GridFraction );
	failed += RunBudgetCases();
#ifdef LIGHTMAP_INC
	failed += RunExtra( "R_FindLightmap index past MAX_LIGHTMAPS", T_FindLightmapIndex );
	failed += RunExtra( "R_FindLightmap long world directory", T_FindLightmapLongDir );
#endif
	printf( "%d renderer failures\n", failed );
	return failed ? 1 : 0;
}
