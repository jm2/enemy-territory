/* CM_LoadMap against corrupted BSPs, with the real cm_load.c, cm_patch.c,
 * cm_test.c and cm_polylib.c. A refused map must drop before the previous
 * collision map, checksum or hunk changes. With BSP files as arguments, loads
 * each one, then truncates and mutates it. Run under ASan and UBSan. */
#include "../../src/qcommon/cm_local.h"
#include "../../src/qcommon/cm_patch.h"
#include "bsp_fixture.h"

static cvar_t testCvar;
cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	(void)name; (void)value; (void)flags;
	return &testCvar;
}

/* The hunk's accounting, as common.c has it without HUNK_DEBUG. Unless a
 * budget is set, the hunk is as large as the test needs. With one, an
 * allocation past it drops the game as Hunk_Alloc does: halfway through the
 * load, with the file still open, unless the loader refused the map first. */
static int budgetActive, budgetLeft;
static unsigned int hunkCost;     // every allocation's cost, wrapping over a long run
int Hunk_AllocationSize( int size ) {
	return size < 0 || size > INT_MAX - 31 ? -1 : ( size + 31 ) & ~31;
}
int Hunk_MemoryRemaining( void ) {
	return budgetActive ? budgetLeft : 0x40000000;
}
#ifdef Hunk_Alloc
#undef Hunk_Alloc
#endif
void *Hunk_Alloc( int size, ha_pref preference ) {
	int cost = Hunk_AllocationSize( size );
	(void)preference;
	if ( cost < 0 || cost > Hunk_MemoryRemaining() ) {
		Com_Error( ERR_DROP, "Hunk_Alloc failed on %i", cost );
	}
	if ( budgetActive ) {
		budgetLeft -= cost;
	}
	hunkCost += cost;
	return TestHunk( size );
}
unsigned Com_BlockChecksum( const void *buffer, int length ) {
	const byte *b = buffer;
	unsigned h = 2166136261u;
	int i;
	for ( i = 0; i < length; i++ ) {
		h = ( h ^ b[i] ) * 16777619u;
	}
	return h;
}
void BotDrawDebugPolygons( void ( *drawPoly )( int color, int numPoints, float *points ), int value ) {
	(void)drawPoly; (void)value;
}

static int LoaderTry( const char *name ) {
	int checksum;
	expectDrop = 1;
	if ( setjmp( dropJump ) ) {
		expectDrop = 0;
		if ( fileBlock ) {
			FS_FreeFile( fileBlock );   // the unfixed loader drops with the file open
		}
		return 0;
	}
	CM_LoadMap( name, qfalse, &checksum );
	expectDrop = 0;
	return 1;
}

/* ---- a digest of everything the collision map holds, pointers as indexes ---- */

// the box hull cm_load.c reserves past each array
#define BOX_BRUSHES 1
#define BOX_SIDES   6
#define BOX_PLANES  12

static unsigned digest;
static void Digest( const void *data, size_t length ) {
	const byte *b = data;
	size_t i;
	for ( i = 0; i < length; i++ ) {
		digest = ( digest ^ b[i] ) * 16777619u;
	}
}
static void DigestInt( int value ) { Digest( &value, sizeof( value ) ); }

static unsigned CollisionDigest( void ) {
	int i, j;
	digest = 2166136261u;
	DigestInt( cm.numShaders );
	Digest( cm.shaders, cm.numShaders * sizeof( *cm.shaders ) );
	DigestInt( cm.numPlanes );
	Digest( cm.planes, ( cm.numPlanes + BOX_PLANES ) * sizeof( *cm.planes ) );
	DigestInt( cm.numBrushSides );
	for ( i = 0; i < cm.numBrushSides + BOX_SIDES; i++ ) {
		DigestInt( cm.brushsides[i].plane - cm.planes );
		DigestInt( cm.brushsides[i].surfaceFlags );
		DigestInt( cm.brushsides[i].shaderNum );
	}
	DigestInt( cm.numNodes );
	for ( i = 0; i < cm.numNodes; i++ ) {
		DigestInt( cm.nodes[i].plane - cm.planes );
		Digest( cm.nodes[i].children, sizeof( cm.nodes[i].children ) );
	}
	DigestInt( cm.numLeafs );
	Digest( cm.leafs, cm.numLeafs * sizeof( *cm.leafs ) );
	DigestInt( cm.numLeafBrushes );
	Digest( cm.leafbrushes, ( cm.numLeafBrushes + 1 ) * sizeof( int ) );
	DigestInt( cm.numLeafSurfaces );
	Digest( cm.leafsurfaces, cm.numLeafSurfaces * sizeof( int ) );
	DigestInt( cm.numSubModels );
	for ( i = 0; i < cm.numSubModels; i++ ) {
		const cLeaf_t *leaf = &cm.cmodels[i].leaf;
		Digest( cm.cmodels[i].mins, sizeof( vec3_t ) );
		Digest( cm.cmodels[i].maxs, sizeof( vec3_t ) );
		DigestInt( leaf->cluster );
		DigestInt( leaf->area );
		// an inline model's lists are blocks of their own, found by an int offset from
		// cm.leafbrushes that a 64-bit host's heap need not keep: only their lengths,
		// since what they hold is firstBrush/firstSurface + j from the file
		DigestInt( leaf->numLeafBrushes );
		DigestInt( leaf->numLeafSurfaces );
		if ( !i ) {
			DigestInt( leaf->firstLeafBrush );
			DigestInt( leaf->firstLeafSurface );
		}
	}
	DigestInt( cm.numBrushes );
	for ( i = 0; i < cm.numBrushes + BOX_BRUSHES; i++ ) {
		DigestInt( cm.brushes[i].shaderNum );
		DigestInt( cm.brushes[i].contents );
		Digest( cm.brushes[i].bounds, sizeof( cm.brushes[i].bounds ) );
		DigestInt( cm.brushes[i].numsides );
		DigestInt( cm.brushes[i].sides - cm.brushsides );
	}
	DigestInt( cm.numClusters );
	DigestInt( cm.clusterBytes );
	DigestInt( cm.vised );
	Digest( cm.visibility, cm.vised ? cm.numClusters * cm.clusterBytes : cm.clusterBytes );
	DigestInt( cm.numEntityChars );
	Digest( cm.entityString, cm.numEntityChars + 1 );
	DigestInt( cm.numAreas );
	Digest( cm.areas, cm.numAreas * sizeof( *cm.areas ) );
	Digest( cm.areaPortals, cm.numAreas * cm.numAreas * sizeof( int ) );
	DigestInt( cm.numSurfaces );
	for ( i = 0; i < cm.numSurfaces; i++ ) {
		const cPatch_t *patch = cm.surfaces[i];
		const patchCollide_t *pc;
		DigestInt( patch != NULL );
		if ( !patch ) {
			continue;
		}
		DigestInt( patch->surfaceFlags );
		DigestInt( patch->contents );
		pc = patch->pc;
		Digest( pc->bounds, sizeof( pc->bounds ) );
		DigestInt( pc->numPlanes );
		Digest( pc->planes, pc->numPlanes * sizeof( *pc->planes ) );
		DigestInt( pc->numFacets );
		Digest( pc->facets, pc->numFacets * sizeof( *pc->facets ) );
	}
	DigestInt( cm.floodvalid );
	return digest;
}

static void LoaderReject( const char *name );

/* A loaded map's hunk cost is exactly its budget: one byte less is refused. */
static void BudgetBoundary( const char *name, int cost ) {
	budgetActive = 1;
	budgetLeft = cost - 1;
	LoaderReject( name );
	Check( budgetLeft == cost - 1, "a refused map takes nothing from the hunk" );
	budgetLeft = 0;
	LoaderReject( name );
	budgetLeft = cost;
	Check( LoaderTry( name ), "a map loads in exactly its own hunk cost" );
	Check( budgetLeft == 0, "the load took exactly the cost it was charged" );
	budgetActive = 0;
}

static int LoaderAccept( const char *name ) {
	int i, patches = 0;
	unsigned int cost = hunkCost;
	if ( !LoaderTry( name ) ) {
		printf( "    unexpected drop: %s\n", lastError );
		return 0;
	}
	cost = hunkCost - cost;
	Check( cm.numSubModels >= 1 && cm.numNodes >= 1 && cm.numLeafs >= 1, "collision arrays published" );
	Check( Terminated( cm.entityString, cm.numEntityChars ), "entity string terminated" );
	for ( i = 0; i < cm.numSurfaces; i++ ) {
		patches += cm.surfaces[i] != NULL;
	}
	if ( strstr( name, "case" ) || strstr( name, "previous" ) ) {
		Check( patches == 1 && cm.numBrushes == 2, "fixture patch and brushes loaded" );
	}
	if ( realMaps ) {
		unsigned d = CollisionDigest();
		printf( "     %s: collision digest %08x, %d patches, %u hunk bytes, file %d bytes\n",
				name, d, patches, cost, sourceSize );
		BudgetBoundary( name, cost );
		Check( CollisionDigest() == d, "the same collision map at its exact budget" );
	}
	return 1;
}

static void LoaderReject( const char *name ) {
	clipMap_t before = cm;
	int checksum = 0x5a5a, hunk = hunkCount, r = reads, f = frees, d = drops;

	expectDrop = 1;
	if ( !setjmp( dropJump ) ) {
		CM_LoadMap( name, qfalse, &checksum );
		expectDrop = 0;
		Fail( "CM_LoadMap accepted the map" );
	}
	expectDrop = 0;
	Check( drops == d + 1, "one drop" );
	Check( fileBlock == NULL, "the file was freed before the drop" );
	Check( reads == r + 1 && frees == f + 1, "one read, one free" );
	Check( hunkCount == hunk, "no hunk allocation before the drop" );
	Check( checksum == 0x5a5a, "checksum unchanged" );
	Check( !memcmp( &cm, &before, sizeof( cm ) ), "previous collision map unchanged" );
}

/* ---- hunk budget and patch plane and facet budget cases (Q3A 7cc5d385, 8ee4b726) ---- */

/* Move the fixture's patch onto a width x height grid of fresh vertices. */
static void PatchGrid( int width, int height, int mode, float amplitude ) {
	unsigned int o = GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + width * height ), v;
	int i, j, odd;
	for ( j = 0; j < height; j++ ) {
		for ( i = 0; i < width; i++ ) {
			v = o + ( 21 + j * width + i ) * sizeof( drawVert_t );
			odd = ( mode == 1 ? i : mode == 2 ? i + j : 0 ) % 2;
			Float( v, i * 32.0f );
			Float( v + 4, j * 32.0f );
			Float( v + 8, ( odd ? amplitude : 0 ) + ( mode == 3 ? i * j * 7 + ( ( i + j ) % 2 ) * amplitude : 0 ) );
			Float( v + offsetof( drawVert_t, normal ) + 8, 1 );
		}
	}
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, firstVert, 21 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, numVerts, width * height );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, width );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, height );
}

static void B_Minimal( void ) {}
static void B_HighestArea( void ) {
	// the square portal matrix of the largest area number the ABI allows
	SET( LUMP_LEAFS, dleaf_t, 1, area, MAX_MAP_AREA_BYTES * 8 - 1 );
}
static void B_NoVis( void ) { Lump( LUMP_VISIBILITY, 0, 0 ); SET( LUMP_LEAFS, dleaf_t, 1, cluster, 1 ); }
static void B_VisHeader( void ) {
	// the copy keeps the 8 header bytes; 25-byte rows are not a multiple of anything
	unsigned int o = Append( LUMP_VISIBILITY, 33, 1 );
	Word( o, 1 );
	Word( o + 4, 25 );
	memset( source + o + 8, 0x5a, 25 );
}
static void B_OpaqueLeaves( void ) {
	// no areas and no clusters: zero-byte blocks still go through Hunk_Alloc
	Lump( LUMP_VISIBILITY, 0, 0 );
	SET( LUMP_LEAFS, dleaf_t, 1, cluster, -1 );
	SET( LUMP_LEAFS, dleaf_t, 1, area, -1 );
}
static void B_InlineModels( void ) {
	// five inline models on the same brush: each gets its own index lists
	unsigned int o = GrowLump( LUMP_MODELS, sizeof( dmodel_t ), 6 ), i;
	for ( i = 2; i < 6; i++ ) {
		memcpy( source + o + i * sizeof( dmodel_t ), source + o + sizeof( dmodel_t ), sizeof( dmodel_t ) );
	}
	SET( LUMP_MODELS, dmodel_t, 3, numBrushes, 0 );
	SET( LUMP_MODELS, dmodel_t, 4, firstSurface, S_PATCH );
	SET( LUMP_MODELS, dmodel_t, 4, numSurfaces, 1 );
}
static void B_RepeatedPatches( void ) {
	// seven more surfaces naming the same patch: each is generated and stored again
	unsigned int o = GrowLump( LUMP_SURFACES, sizeof( dsurface_t ), NUM_SURFS + 7 ), i;
	for ( i = NUM_SURFS; i < NUM_SURFS + 7; i++ ) {
		memcpy( source + o + i * sizeof( dsurface_t ), source + o + S_PATCH * sizeof( dsurface_t ), sizeof( dsurface_t ) );
	}
}
static void B_WidePatch( void ) { PatchGrid( 129, 3, 0, 0 ); }
static void B_BumpyPatch( void ) {
	// hundreds of facets and planes, all charged at their generated sizes
	PatchGrid( 15, 15, 2, 32 );
}

static void FacetsPastMax( void ) { PatchGrid( 31, 31, 2, 32 ); }
static void PlanesPastMax( void ) { PatchGrid( 31, 31, 3, 128 ); }

typedef struct {
	const char *name;
	void ( *mutate )( void );
	const char *error;      // NULL: the map loads, and is then refused one byte short
} budgetCase_t;

static const budgetCase_t budgetCases[] = {
	{ "hunk budget: minimal map", B_Minimal, NULL },
	{ "hunk budget: area 255 portal matrix", B_HighestArea, NULL },
	{ "hunk budget: novis rows", B_NoVis, NULL },
	{ "hunk budget: visibility with its header", B_VisHeader, NULL },
	{ "hunk budget: opaque leaves, zero-byte blocks", B_OpaqueLeaves, NULL },
	{ "hunk budget: inline model index lists", B_InlineModels, NULL },
	{ "hunk budget: repeated patch surfaces", B_RepeatedPatches, NULL },
	{ "hunk budget: 129x3 patch", B_WidePatch, NULL },
	{ "hunk budget: 15x15 bumpy patch", B_BumpyPatch, NULL },
	{ "31x31 patch past MAX_FACETS", FacetsPastMax, "MAX_FACETS" },
	{ "31x31 patch past MAX_PATCH_PLANES", PlanesPastMax, "MAX_PATCH_PLANES" },
};

static void BudgetCase( const budgetCase_t *c ) {
	int patches, i;
	unsigned int cost;
	unsigned d;

	BuildMap();
	Check( LoaderAccept( "maps/previous.bsp" ), "the unmutated map loads" );
	c->mutate();
	if ( c->error ) {
		LoaderReject( "maps/case.bsp" );
		printf( "    dropped: %s\n", lastError );
		Check( strstr( lastError, c->error ) != NULL, "refused for the budget it exceeds" );
		return;
	}
	cost = hunkCost;
	Check( LoaderTry( "maps/budget.bsp" ), "the map loads with room to spare" );
	cost = hunkCost - cost;
	d = CollisionDigest();
	for ( i = patches = 0; i < cm.numSurfaces; i++ ) {
		patches += cm.surfaces[i] != NULL;
	}
	printf( "    %u hunk bytes, %d patches\n", cost, patches );
	BudgetBoundary( "maps/budget.bsp", cost );
	Check( CollisionDigest() == d, "the same collision map at its exact budget" );
	if ( c->mutate == B_HighestArea ) {
		Check( cm.numAreas == MAX_MAP_AREA_BYTES * 8, "the full portal matrix was allocated" );
	} else if ( c->mutate == B_NoVis ) {
		Check( !cm.vised && cm.clusterBytes == 32, "novis rows keep their rounding" );
	} else if ( c->mutate == B_VisHeader ) {
		Check( cm.vised && cm.clusterBytes == 25 && cm.visibility[24] == 0x5a, "visibility copied with its rows" );
	} else if ( c->mutate == B_OpaqueLeaves ) {
		Check( !cm.numAreas && !cm.numClusters, "no areas and no clusters" );
	} else if ( c->mutate == B_InlineModels ) {
		Check( cm.numSubModels == 6 && cm.cmodels[4].leaf.numLeafSurfaces == 1 && !cm.cmodels[3].leaf.numLeafBrushes, "inline models loaded" );
	} else if ( c->mutate == B_RepeatedPatches ) {
		Check( patches == 8, "every repeated patch generated" );
	} else if ( c->mutate == B_BumpyPatch ) {
		printf( "    %d facets, %d planes\n", cm.surfaces[S_PATCH]->pc->numFacets, cm.surfaces[S_PATCH]->pc->numPlanes );
		Check( cm.surfaces[S_PATCH]->pc->numFacets > 64, "a patch of many facets" );
	}
}

static int RunBudgetCases( void ) {
	unsigned int i;
	int failed = 0, status;
	pid_t pid;
	for ( i = 0; i < sizeof( budgetCases ) / sizeof( budgetCases[0] ); i++ ) {
		printf( "case: %s\n", budgetCases[i].name );
		fflush( stdout );
		pid = fork();
		if ( pid == 0 ) {
			alarm( 60 );
			BudgetCase( &budgetCases[i] );
			fflush( stdout );
			exit( 0 );
		}
		waitpid( pid, &status, 0 );
		if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
			printf( "PASS %s\n", budgetCases[i].name );
		} else {
			printf( "FAIL %s (%s %d)\n", budgetCases[i].name, WIFSIGNALED( status ) ? "signal" : "exit",
					WIFSIGNALED( status ) ? WTERMSIG( status ) : WEXITSTATUS( status ) );
			failed++;
		}
	}
	printf( "%d of %d budget cases passed\n", (int)( sizeof( budgetCases ) / sizeof( budgetCases[0] ) ) - failed,
			(int)( sizeof( budgetCases ) / sizeof( budgetCases[0] ) ) );
	return failed;
}

int main( int argc, char **argv ) {
	const char *m = getenv( "BSP_MUTATIONS" );
	int failed;
	whichLoader = 0;
	Swap_Init();
	if ( argc > 1 ) {
		return RunRealMaps( argc, argv, m ? atoi( m ) : 200 ) ? 1 : 0;
	}
	printf( "CM_LoadMap:\n" );
	failed = RunCases();
	failed += RunBudgetCases();
	return failed ? 1 : 0;
}
