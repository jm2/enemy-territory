/* The renderer's world tree walks at any depth: R_RecursiveWorldNode and
 * R_BoxSurfaces_r, on worlds the real tr_bsp.c loads. The walks, R_MarkLeaves,
 * R_SetupFrustum and R_TestDecalBoundingBox are the real functions, extracted
 * from their files (WALK_INC); R_AddLeafSurfaces records what the walk asks
 * for: each leaf, in order, with its dynamic light and decal bits.
 *
 * A valid BSP can be a chain as deep as it has nodes. The recursive walks took
 * a C frame per level (112 and 96 bytes on PPC), so a chain of some 18,000
 * nodes overflowed the Mac's 2 MB stack. Here a DEEP-node chain is walked in a
 * thread with a 256 KB stack, and every answer must equal the one-node world
 * that splits space the same way.
 *
 * With BSP files as arguments: each world's tree depth, then digests of the
 * leaves walked from random viewpoints, with frustum, PVS, dynamic light and
 * decal culling, and of random mark-fragment box queries, to compare one build
 * with another. Run under ASan and UBSan: the walk stacks are hunk blocks of
 * their exact size. run.sh defines
 * WALK_STACKS, which also checks the stacks' recorded depth; without it the
 * test builds against the recursive walks, to show them overflow. */
#include "../../src/renderer/tr_bsp.c"
#include "bsp_fixture.h"
#include <pthread.h>
#include <time.h>

trGlobals_t tr;
glconfig_t glConfig;
refimport_t ri;
int skyboxportal;
static cvar_t zeroCvar, subdivisionsCvar, foliageCvar, nocullCvar, novisCvar;
cvar_t *r_lightmap = &zeroCvar, *r_mapOverBrightBits = &zeroCvar, *r_singleShader = &zeroCvar;
cvar_t *r_subdivisions = &subdivisionsCvar, *r_drawfoliage = &foliageCvar;
cvar_t *r_nocull = &nocullCvar, *r_novis = &novisCvar, *r_lockpvs = &zeroCvar, *r_showcluster = &zeroCvar;
static shader_t knownShader;
static image_t knownImage;

shader_t *R_FindShader( const char *name, int lightmapIndex, qboolean mipRawImage ) {
	(void)name; (void)lightmapIndex; (void)mipRawImage;
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
	(void)name; (void)pic; (void)width; (void)height; (void)mipmap; (void)allowPicmip; (void)wrapClampMode;
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

static int LoaderTry( const char *name ) {
	tr.worldMapLoaded = qfalse;     // RE_Shutdown's job between maps
	tr.numModels = 0;               // and R_ModelInit's
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
static int LoaderAccept( const char *name ) { return LoaderTry( name ); }
static void LoaderReject( const char *name ) { Check( !LoaderTry( name ), "map refused" ); }

/* ---- what the walks ask for ---- */

static unsigned int fnv;
static void Hash( const void *data, size_t size ) {
	const byte *b = data;
	size_t i;
	for ( i = 0; i < size; i++ ) {
		fnv = ( fnv ^ b[i] ) * 16777619u;
	}
}

#define MAX_RUNS 8
typedef struct {
	int leaf, dlightBits, decalBits;
} visit_t;
static visit_t runs[MAX_RUNS];      // the visits, with repeats of one visit collapsed
static int numRuns, numVisits;

static void R_AddLeafSurfaces( mnode_t *node, int dlightBits, int decalBits ) {
	visit_t v;
	v.leaf = node - s_worldData.nodes - s_worldData.numDecisionNodes;
	v.dlightBits = dlightBits;
	v.decalBits = decalBits;
	Hash( &v, sizeof( v ) );
	numVisits++;
	if ( numRuns && numRuns <= MAX_RUNS && !memcmp( &runs[numRuns - 1], &v, sizeof( v ) ) ) {
		return;
	}
	if ( numRuns < MAX_RUNS ) {
		runs[numRuns] = v;
	}
	numRuns++;
}

#include WALK_INC

/* Surface data pointers to surface numbers, to hash R_BoxSurfaces_r's list. */
typedef struct {
	const surfaceType_t *data;
	int index;
} surfaceIndex_t;
static surfaceIndex_t *surfaceIndex;
static int SurfaceIndexOrder( const void *a, const void *b ) {
	const surfaceIndex_t *x = a, *y = b;
	return x->data < y->data ? -1 : x->data > y->data;
}
static void IndexSurfaces( void ) {
	int i;
	free( surfaceIndex );
	surfaceIndex = malloc( ( s_worldData.numsurfaces + 1 ) * sizeof( *surfaceIndex ) );
	for ( i = 0; i < s_worldData.numsurfaces; i++ ) {
		surfaceIndex[i].data = s_worldData.surfaces[i].data;
		surfaceIndex[i].index = i;
	}
	qsort( surfaceIndex, s_worldData.numsurfaces, sizeof( *surfaceIndex ), SurfaceIndexOrder );
}
static int SurfaceNumber( const surfaceType_t *data ) {
	surfaceIndex_t key, *found;
	key.data = data;
	found = bsearch( &key, surfaceIndex, s_worldData.numsurfaces, sizeof( *surfaceIndex ), SurfaceIndexOrder );
	Check( found != NULL, "a listed surface is a world surface" );
	return found->index;
}

static unsigned int rngView;
static float Uniform( float low, float high ) {
	rngView = rngView * 1103515245u + 12345u;
	return low + ( high - low ) * ( ( rngView >> 8 ) & 0xffff ) / 65535.0f;
}

static dlight_t dlights[4];
static decalProjector_t projectors[3];

/* A view from origin along angles, with its PVS marked as R_AddWorldSurfaces does. */
static void View( const vec3_t origin, const vec3_t angles, float fov, float zFar ) {
	VectorCopy( origin, tr.viewParms.orientation.origin );
	AngleVectors( angles, tr.viewParms.orientation.axis[0], tr.viewParms.orientation.axis[1], tr.viewParms.orientation.axis[2] );
	VectorInverse( tr.viewParms.orientation.axis[1] );     // left, as R_RotateForViewer has it
	tr.viewParms.fovX = fov;
	tr.viewParms.fovY = fov * 0.75f;
	tr.viewParms.zFar = zFar;
	R_SetupFrustum();
	VectorCopy( origin, tr.viewParms.pvsOrigin );
	tr.refdef.areamaskModified = qtrue;
	R_MarkLeaves();
}

/* The world walk as R_AddWorldSurfaces calls it, with its visits counted and hashed. */
static void Walk( int dlightBits, int decalBits ) {
	numRuns = numVisits = 0;
	R_RecursiveWorldNode( tr.world->nodes, 255, dlightBits, decalBits );
}

/* ---- the deep chain ---- */

/* count nodes on one plane, x = 1 facing -x: each node's front (x < 1) is the
 * next node and its back (x > 1) the opaque leaf 0, which every node shares.
 * The last front is leaf 1. A one-node chain is the same partition of space. */
#define DEEP 65536
static void Chain( int count ) {
	unsigned int o;
	int i, j;
	BuildMap();
	o = GrowLump( LUMP_PLANES, sizeof( dplane_t ), 7 );
	Float( o + 6 * sizeof( dplane_t ) + offsetof( dplane_t, normal ), -1 );
	Float( o + 6 * sizeof( dplane_t ) + offsetof( dplane_t, dist ), -1 );
	o = Append( LUMP_NODES, count * sizeof( dnode_t ), 1 );
	for ( i = 0; i < count; i++ ) {
		SET( LUMP_NODES, dnode_t, i, planeNum, 6 );
		SET( LUMP_NODES, dnode_t, i, children[0], i + 1 < count ? i + 1 : -2 );
		SET( LUMP_NODES, dnode_t, i, children[1], -1 );
		for ( j = 0; j < 3; j++ ) {
			SET( LUMP_NODES, dnode_t, i, mins[j], -64 );
			SET( LUMP_NODES, dnode_t, i, maxs[j], 64 );
		}
	}
	// leaf 0 marks the planar surface, which leaf 1 also has
	SET( LUMP_LEAFS, dleaf_t, 0, numLeafSurfaces, 1 );
}

typedef struct {
	int numRuns, numVisits;
	visit_t runs[MAX_RUNS];
} walkAnswer_t;
typedef struct {
	int count, list[64];
} boxAnswer_t;
#define WALKS 4
#define BOXES 3
static walkAnswer_t walkAnswers[2][WALKS];
static boxAnswer_t boxAnswers[2][BOXES];

static void WalkAnswers( int which ) {
	static const vec3_t origin = { -32, 0, 0 }, angles = { 0, 0, 0 };
	int n;
	// a light and a decal at the surfaces, and one of each that reaches nothing
	VectorSet( dlights[0].origin, 0, 0, 0 );
	dlights[0].radius = 8;
	VectorSet( dlights[1].origin, 1000, 0, 0 );
	dlights[1].radius = 10;
	tr.refdef.dlights = dlights;
	tr.refdef.num_dlights = 2;
	projectors[0].shader = &knownShader;
	projectors[0].radius = 4;
	projectors[1].shader = NULL;
	projectors[1].radius = 4;
	tr.refdef.decalProjectors = projectors;
	tr.refdef.numDecalProjectors = 2;
	novisCvar.integer = 1;
	for ( n = 0; n < WALKS; n++ ) {
		nocullCvar.integer = n & 1;
		View( origin, angles, 90, 4096 );
		Walk( n & 2 ? 3 : 0, n & 2 ? 3 : 0 );
		walkAnswers[which][n].numRuns = numRuns;
		walkAnswers[which][n].numVisits = numVisits;
		memcpy( walkAnswers[which][n].runs, runs, sizeof( runs ) );
	}
	nocullCvar.integer = novisCvar.integer = 0;
}

static void BoxAnswers( int which ) {
	static const float x[BOXES][2] = { { 0, 2 }, { -0.5f, 0.5f }, { 1.5f, 2 } };   // across, in front, behind
	surfaceType_t *list[64];
	vec3_t mins, maxs, dir = { 0, 0, 1 };
	int n, i;
	novisCvar.integer = 1;
	View( vec3_origin, vec3_origin, 90, 4096 );
	novisCvar.integer = 0;
	for ( n = 0; n < BOXES; n++ ) {
		VectorSet( mins, x[n][0], -0.5f, -0.5f );
		VectorSet( maxs, x[n][1], 0.5f, 0.5f );
		tr.viewCount++;
		boxAnswers[which][n].count = 0;
		R_BoxSurfaces_r( tr.world->nodes, mins, maxs, list, 64, &boxAnswers[which][n].count, dir );
		for ( i = 0; i < boxAnswers[which][n].count; i++ ) {
			boxAnswers[which][n].list[i] = SurfaceNumber( list[i] );
		}
	}
}

/* Each walk in a child, in a thread with a 256 KB stack, so that an overflow
 * on the old code fails that walk only. */
static void *WalkThread( void *unused ) {
	(void)unused;
	WalkAnswers( 1 );
	return NULL;
}
static void *BoxThread( void *unused ) {
	(void)unused;
	BoxAnswers( 1 );
	return NULL;
}
static int Deep( const char *name, void *( *thread )( void * ) ) {
	pthread_attr_t attr;
	pthread_t id;
	pid_t pid;
	int status, n;

	fflush( stdout );
	pid = fork();
	if ( pid == 0 ) {
		alarm( 120 );
		Chain( 1 );
		Check( LoaderTry( "maps/one.bsp" ), "the one-node world loads" );
		IndexSurfaces();
		WalkAnswers( 0 );
		BoxAnswers( 0 );
		Chain( DEEP );
		Check( LoaderTry( "maps/deep.bsp" ), "the deep chain loads" );
#ifdef WALK_STACKS
		Check( s_worldData.nodeDepth == DEEP, "the world walk stacks hold the chain" );
#endif
		IndexSurfaces();
		Check( !pthread_attr_init( &attr ) && !pthread_attr_setstacksize( &attr, 256 * 1024 ), "thread attributes" );
		Check( !pthread_create( &id, &attr, thread, NULL ) && !pthread_join( id, NULL ), "walk thread" );
		if ( thread == WalkThread ) {
			for ( n = 0; n < WALKS; n++ ) {
				const walkAnswer_t *a = &walkAnswers[0][n], *b = &walkAnswers[1][n];
				Check( a->numVisits == 2 && b->numVisits == DEEP + 1, "leaf 0 is walked from every node" );
				Check( a->numRuns == 2 && a->runs[0].leaf == 1 && a->runs[1].leaf == 0, "front side first" );
				Check( b->numRuns == a->numRuns && !memcmp( a->runs, b->runs, sizeof( a->runs ) ), "the same leaves in the same order, with the same bits" );
				Check( !( n & 2 ) || ( a->runs[0].dlightBits == 1 && a->runs[0].decalBits == 1 ), "lights and decals culled as on one node" );
			}
		} else {
			for ( n = 0; n < BOXES; n++ ) {
				const boxAnswer_t *a = &boxAnswers[0][n], *b = &boxAnswers[1][n];
				Check( a->count == b->count && !memcmp( a->list, b->list, a->count * sizeof( int ) ), "the same mark surfaces in the same order" );
			}
			Check( boxAnswers[1][0].count > 1, "a box across the plane lists surfaces" );
		}
		fflush( stdout );
		exit( 0 );
	}
	waitpid( pid, &status, 0 );
	if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
		printf( "ok   %s down a %d-node chain in a 256 KB stack, as on one node\n", name, DEEP );
		return 0;
	}
	printf( "FAIL %s down a %d-node chain in a 256 KB stack\n", name, DEEP );
	return 1;
}

/* ---- real maps: depth and digests ---- */

/* The most decision nodes on a path down from node 0, counted here, apart from the engine. */
static unsigned int TreeDepth( void ) {
	unsigned int nodes = LumpLen( LUMP_NODES ) / sizeof( dnode_t ), *queue, *level, head = 0, tail = 0, i, j, child, depth = 0;
	if ( !nodes ) {
		return 0;
	}
	queue = malloc( nodes * 2 * sizeof( *queue ) );
	level = queue + nodes;
	queue[tail++] = 0;
	level[0] = 1;
	while ( head < tail ) {
		i = queue[head++];
		depth = level[i] > depth ? level[i] : depth;
		for ( j = 0; j < 2; j++ ) {
			child = Read( AT( LUMP_NODES, dnode_t, i, children ) + j * 4 );
			if ( !( child & 0x80000000u ) ) {
				Check( tail < nodes, "the tree reaches each node once" );
				level[child] = level[i] + 1;
				queue[tail++] = child;
			}
		}
	}
	free( queue );
	return depth;
}

static int RealMap( const char *path, int views ) {
	surfaceType_t *list[64];
	vec3_t lo, hi, origin, angles, mins, maxs, dir;
	unsigned int walkDigest, boxDigest;
	int k, i, n, dlightBits, decalBits, visits = 0, listed = 0;
	clock_t c0, c1, c2;

	if ( !LoadFile( path ) ) {
		printf( "FAIL %s: unreadable\n", path );
		return 1;
	}
	if ( !LoaderTry( path ) ) {
		printf( "FAIL %s: %s\n", path, lastError );
		return 1;
	}
	IndexSurfaces();
	VectorCopy( s_worldData.bmodels[0].bounds[0], lo );
	VectorCopy( s_worldData.bmodels[0].bounds[1], hi );
	tr.refdef.dlights = dlights;
	tr.refdef.decalProjectors = projectors;
	rngView = 2600;
	fnv = 2166136261u;
	c0 = clock();
	for ( k = 0; k < views; k++ ) {
		// mostly from leaves with a cluster, where a player can see from
		for ( n = 0; n < 64; n++ ) {
			for ( i = 0; i < 3; i++ ) {
				origin[i] = Uniform( lo[i], hi[i] );
			}
			if ( R_PointInLeaf( origin )->cluster >= 0 ) {
				break;
			}
		}
		VectorSet( angles, Uniform( -89, 89 ), Uniform( 0, 360 ), 0 );
		nocullCvar.integer = k % 8 == 7;
		novisCvar.integer = k % 16 == 5;
		View( origin, angles, Uniform( 60, 120 ), Uniform( 512, 8192 ) );
		tr.refdef.num_dlights = k % 5;
		for ( n = 0; n < tr.refdef.num_dlights; n++ ) {
			for ( i = 0; i < 3; i++ ) {
				dlights[n].origin[i] = origin[i] + Uniform( -512, 512 );
			}
			dlights[n].radius = Uniform( 16, 400 );
			dlights[n].flags = k % 7 == n ? REF_DIRECTED_DLIGHT : 0;
		}
		tr.refdef.numDecalProjectors = k % 4;
		for ( n = 0; n < tr.refdef.numDecalProjectors; n++ ) {
			for ( i = 0; i < 3; i++ ) {
				projectors[n].center[i] = origin[i] + Uniform( -256, 256 );
			}
			projectors[n].radius = Uniform( 8, 256 );
			projectors[n].shader = n == 2 ? NULL : &knownShader;
		}
		dlightBits = ( 1 << tr.refdef.num_dlights ) - 1;
		decalBits = ( 1 << tr.refdef.numDecalProjectors ) - 1;
		Hash( &k, sizeof( k ) );
		Walk( dlightBits, decalBits );
		visits += numVisits;
	}
	walkDigest = fnv;
	nocullCvar.integer = novisCvar.integer = 0;
	fnv = 2166136261u;
	c1 = clock();
	for ( k = 0; k < views; k++ ) {
		if ( k % 64 == 0 ) {
			// mark fragments skip what was not drawn in the last frames
			for ( i = 0; i < 3; i++ ) {
				origin[i] = Uniform( lo[i], hi[i] );
			}
			View( origin, vec3_origin, 90, 4096 );
		}
		for ( i = 0; i < 3; i++ ) {
			mins[i] = Uniform( lo[i], hi[i] );
			maxs[i] = mins[i] + Uniform( 4, 256 );
			dir[i] = Uniform( -1, 1 );
		}
		VectorNormalize( dir );
		tr.viewCount++;
		n = 0;
		R_BoxSurfaces_r( tr.world->nodes, mins, maxs, list, 64, &n, dir );
		Hash( &n, sizeof( n ) );
		for ( i = 0; i < n; i++ ) {
			int number = SurfaceNumber( list[i] );
			Hash( &number, sizeof( number ) );
		}
		listed += n;
	}
	boxDigest = fnv;
	c2 = clock();
#ifdef WALK_STACKS
	Check( s_worldData.nodeDepth == (int)TreeDepth(), "the world walk stacks hold the tree's depth" );
#endif
	printf( "PASS %s: %d decision nodes, tree depth %u\n", path, s_worldData.numDecisionNodes, TreeDepth() );
	printf( "     %d views (%d leaves walked) digest %08x; %d mark boxes (%d surfaces) digest %08x\n",
			views, visits, walkDigest, views, listed, boxDigest );
	printf( "     %.3f s of world walks, %.3f s of mark boxes\n",
			(double)( c1 - c0 ) / CLOCKS_PER_SEC, (double)( c2 - c1 ) / CLOCKS_PER_SEC );
	return 0;
}

int main( int argc, char **argv ) {
	const char *s = getenv( "BSP_WALK_SAMPLES" );
	int i, failed = 0;

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
	for ( i = 0; i < FUNCTABLE_SIZE; i++ ) {
		tr.sinTable[i] = sin( DEG2RAD( i * 360.0f / ( (float)( FUNCTABLE_SIZE - 1 ) ) ) );
	}
	tr.visCount = 1;
	if ( argc > 1 ) {
		for ( i = 1; i < argc; i++ ) {
			failed += RealMap( argv[i], s ? atoi( s ) : 4000 );
		}
		return failed ? 1 : 0;
	}
	failed += Deep( "R_RecursiveWorldNode", WalkThread );
	failed += Deep( "R_BoxSurfaces_r", BoxThread );
	return failed ? 1 : 0;
}
