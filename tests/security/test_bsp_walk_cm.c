/* The collision map's world tree walks at any depth: CM_BoxLeafnums_r, under
 * CM_BoxLeafnums, CM_BoxBrushes and position tests, and CM_TraceThroughTree,
 * under CM_BoxTrace, with the real cm_load.c, cm_test.c and cm_trace.c.
 *
 * A valid BSP can be a chain as deep as it has nodes. The recursive walks took
 * a C frame per level (80 and 144 bytes on PPC), so a chain of some 15,000
 * nodes overflowed the Mac's 2 MB stack. Here a DEEP-node chain is walked in a
 * thread with a 256 KB stack, in both directions across its plane, and every
 * answer must equal the one-node map that splits space the same way.
 *
 * With BSP files as arguments: each map's tree depth, then digests of random
 * traces and box queries, to compare one build with another. Run under ASan
 * and UBSan: the walk stacks are hunk blocks of their exact size. run.sh defines
 * WALK_STACKS, which also checks the stacks' recorded depth; without it the
 * test builds against the recursive walks, to show them overflow. */
#include "../../src/qcommon/cm_local.h"
#include "bsp_fixture.h"
#include <pthread.h>
#include <time.h>

static cvar_t testCvar;
cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	(void)name; (void)value; (void)flags;
	return &testCvar;
}
int Hunk_AllocationSize( int size ) {
	return size < 0 || size > INT_MAX - 31 ? -1 : ( size + 31 ) & ~31;
}
int Hunk_MemoryRemaining( void ) {
	return 0x40000000;
}
#ifdef Hunk_Alloc
#undef Hunk_Alloc
#endif
void *Hunk_Alloc( int size, ha_pref preference ) {
	(void)preference;
	return TestHunk( size );
}
unsigned Com_BlockChecksum( const void *buffer, int length ) {
	(void)buffer; (void)length;
	return 0;
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
			FS_FreeFile( fileBlock );
		}
		return 0;
	}
	CM_LoadMap( name, qfalse, &checksum );
	expectDrop = 0;
	return 1;
}
static int LoaderAccept( const char *name ) { return LoaderTry( name ); }
static void LoaderReject( const char *name ) { Check( !LoaderTry( name ), "map refused" ); }

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

/* ---- the deep chain ---- */

/* count nodes on one plane, x = 1 facing -x: each node's front (x < 1) is the
 * next node and its back (x > 1) the opaque leaf 0, which every node shares.
 * The last front is leaf 1, with the brush and the surfaces. A one-node chain
 * is the same partition of space. */
#define DEEP 65536
static void Chain( int count ) {
	unsigned int o;
	int i;
	BuildMap();
	o = GrowLump( LUMP_PLANES, sizeof( dplane_t ), 7 );
	Float( o + 6 * sizeof( dplane_t ) + offsetof( dplane_t, normal ), -1 );
	Float( o + 6 * sizeof( dplane_t ) + offsetof( dplane_t, dist ), -1 );
	o = Append( LUMP_NODES, count * sizeof( dnode_t ), 1 );
	for ( i = 0; i < count; i++ ) {
		SET( LUMP_NODES, dnode_t, i, planeNum, 6 );
		SET( LUMP_NODES, dnode_t, i, children[0], i + 1 < count ? i + 1 : -2 );
		SET( LUMP_NODES, dnode_t, i, children[1], -1 );
	}
}

#define MAX_ANSWERS 512
typedef struct {
	int count, last, leafs[8], runs;        // leaf list with runs of leaf 0 collapsed
	int brushes, brush[4];
	trace_t trace[4];
} answer_t;
static answer_t answers[2][MAX_ANSWERS];
static int *leafList;
static int askBoxes, askTraces;     // which walk this child asks about

static void Box( vec3_t mins, vec3_t maxs, float x0, float x1 ) {
	VectorSet( mins, x0, -0.5f, -0.5f );
	VectorSet( maxs, x1, 0.5f, 0.5f );
}
static float Uniform( float low, float high ) {
	return low + ( high - low ) * ( Random() & 0xffff ) / 65535.0f;
}

/* Box queries and traces, the same ones on each map: fixed ones across, in
 * front of and behind the plane, then random ones around the box brush. */
static void Answer( answer_t *a, int n ) {
	static const int masks[] = { -1, CONTENTS_SOLID, CONTENTS_SOLID | CONTENTS_BODY };
	vec3_t mins, maxs, start, end, tmins, tmaxs;
	cbrush_t *brushes[4];
	int i, j;

	rng = 4321 + n;
	switch ( n ) {
	case 0: Box( mins, maxs, 0, 2 ); break;         // across the plane
	case 1: Box( mins, maxs, -0.5f, 0.5f ); break;  // in front
	case 2: Box( mins, maxs, 1.5f, 2 ); break;      // behind
	default:
		for ( i = 0; i < 3; i++ ) {
			start[i] = Uniform( -3, 3 );
			end[i] = Uniform( 0, 2 );
		}
		VectorSubtract( start, end, mins );
		VectorAdd( start, end, maxs );
		break;
	}
	memset( a, 0, sizeof( *a ) );
	if ( askBoxes ) {
		a->count = CM_BoxLeafnums( mins, maxs, leafList, DEEP + 2, &a->last );
	}
	for ( i = 0; i < a->count; i++ ) {
		if ( leafList[i] == 0 && i && leafList[i - 1] == 0 ) {
			continue;
		}
		Check( a->runs < 8, "few leaf runs" );
		a->leafs[a->runs++] = leafList[i];
	}
	if ( askBoxes ) {
		a->brushes = CM_BoxBrushes( mins, maxs, brushes, 4 );
	}
	for ( i = 0; i < a->brushes; i++ ) {
		a->brush[i] = brushes[i] - cm.brushes;
	}
	for ( j = 0; j < 4 && askTraces; j++ ) {
		for ( i = 0; i < 3; i++ ) {
			start[i] = Uniform( -3, 3 );
			end[i] = j == 3 ? start[i] : Uniform( -3, 3 );     // the last one a position test
			tmaxs[i] = j & 1 ? Uniform( 0, 0.75f ) : 0;
			tmins[i] = -tmaxs[i];
		}
		if ( n < 3 ) {      // straight across the plane, each way
			VectorSet( start, j & 2 ? 2 : 0, 0, 0 );
			VectorSet( end, j & 2 ? 0 : 2, 0, 0 );
		}
		CM_BoxTrace( &a->trace[j], start, end, tmins, tmaxs, 0, masks[n % 3], n % 5 == 4 );
	}
}

static void AnswerAll( int which ) {
	int n;
	for ( n = 0; n < MAX_ANSWERS; n++ ) {
		Answer( &answers[which][n], n );
	}
}

static void *DeepThread( void *unused ) {
	(void)unused;
	AnswerAll( 1 );
	return NULL;
}

static int SameTrace( const trace_t *a, const trace_t *b ) {
	return a->allsolid == b->allsolid && a->startsolid == b->startsolid &&
		   !memcmp( &a->fraction, &b->fraction, sizeof( float ) ) && !memcmp( a->endpos, b->endpos, sizeof( vec3_t ) ) &&
		   !memcmp( a->plane.normal, b->plane.normal, sizeof( vec3_t ) ) && !memcmp( &a->plane.dist, &b->plane.dist, sizeof( float ) ) &&
		   a->plane.type == b->plane.type && a->plane.signbits == b->plane.signbits &&
		   a->surfaceFlags == b->surfaceFlags && a->contents == b->contents && a->entityNum == b->entityNum;
}

/* One walk's questions, in a child, so that an overflow on the old code fails that walk only. */
static int DeepChain( const char *name, int boxes, int traces ) {
	pthread_attr_t attr;
	pthread_t thread;
	pid_t pid;
	int status, n, j, hits = 0;

	fflush( stdout );
	pid = fork();
	if ( pid == 0 ) {
		alarm( 120 );
		askBoxes = boxes;
		askTraces = traces;
		leafList = malloc( ( DEEP + 2 ) * sizeof( *leafList ) );
		Chain( 1 );
		Check( LoaderTry( "maps/one.bsp" ), "the one-node map loads" );
		AnswerAll( 0 );

		Chain( DEEP );
		Check( TreeDepth() == DEEP, "the chain is as deep as it has nodes" );
		Check( LoaderTry( "maps/deep.bsp" ), "the deep chain loads" );
#ifdef WALK_STACKS
		Check( cm.treeDepth == DEEP, "the collision walk stacks hold the chain" );
#endif
		Check( !pthread_attr_init( &attr ) && !pthread_attr_setstacksize( &attr, 256 * 1024 ), "thread attributes" );
		Check( !pthread_create( &thread, &attr, DeepThread, NULL ) && !pthread_join( thread, NULL ), "walk thread" );

		// across the plane, front side first: leaf 1 at the bottom, then leaf 0 from every node
		Check( !boxes || ( answers[1][0].count == DEEP + 1 && answers[0][0].count == 2 ), "a box across the plane reaches leaf 0 from every node" );
		for ( n = 0; n < MAX_ANSWERS; n++ ) {
			const answer_t *a = &answers[0][n], *b = &answers[1][n];
			Check( a->runs == b->runs && !memcmp( a->leafs, b->leafs, sizeof( a->leafs ) ) && a->last == b->last, "the same leaves in the same order" );
			Check( a->brushes == b->brushes && !memcmp( a->brush, b->brush, sizeof( a->brush ) ), "the same brushes" );
			for ( j = 0; j < 4; j++ ) {
				Check( SameTrace( &a->trace[j], &b->trace[j] ), "the same trace" );
				hits += a->trace[j].fraction < 1 || a->trace[j].startsolid;
			}
		}
		if ( traces ) {
			Check( hits > MAX_ANSWERS / 4, "many traces hit the brush" );
			printf( "     %d of %d traces hit the brush\n", hits, MAX_ANSWERS * 4 );
		}
		free( leafList );
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

static unsigned int fnv;
static void Hash( const void *data, size_t size ) {
	const byte *b = data;
	size_t i;
	for ( i = 0; i < size; i++ ) {
		fnv = ( fnv ^ b[i] ) * 16777619u;
	}
}
static void HashTrace( const trace_t *t ) {
	Hash( &t->allsolid, sizeof( t->allsolid ) );
	Hash( &t->startsolid, sizeof( t->startsolid ) );
	Hash( &t->fraction, sizeof( t->fraction ) );
	Hash( t->endpos, sizeof( t->endpos ) );
	Hash( t->plane.normal, sizeof( t->plane.normal ) );
	Hash( &t->plane.dist, sizeof( t->plane.dist ) );
	Hash( &t->plane.type, 1 );
	Hash( &t->plane.signbits, 1 );
	Hash( &t->surfaceFlags, sizeof( t->surfaceFlags ) );
	Hash( &t->contents, sizeof( t->contents ) );
}

static int RealMap( const char *path, int samples ) {
	static int list[1024];
	static cbrush_t *brushes[1024];
	static const int sizes[] = { 8, 64, 1024 };
	// bg_public.h's MASK_ALL, CONTENTS_SOLID, MASK_PLAYERSOLID and MASK_SHOT
	static const int masks[] = { -1, CONTENTS_SOLID, CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY,
								 CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_CORPSE };
	vec3_t lo, hi, start, end, mins, maxs;
	trace_t t;
	unsigned int traceDigest, leafDigest, brushDigest;
	int i, k, n, count, last, hits = 0, leafs = 0;
	clock_t c0, c1, c2;

	if ( !LoadFile( path ) ) {
		printf( "FAIL %s: unreadable\n", path );
		return 1;
	}
	if ( !LoaderTry( path ) ) {
		printf( "FAIL %s: %s\n", path, lastError );
		return 1;
	}
	VectorCopy( cm.cmodels[0].mins, lo );
	VectorCopy( cm.cmodels[0].maxs, hi );
	rng = 2600;
	fnv = 2166136261u;
	c0 = clock();
	for ( k = 0; k < samples; k++ ) {
		// mostly from open space, where the game traces
		for ( n = 0; n < 64; n++ ) {
			for ( i = 0; i < 3; i++ ) {
				start[i] = Uniform( lo[i], hi[i] );
			}
			if ( !CM_PointContents( start, 0 ) ) {
				break;
			}
		}
		for ( i = 0; i < 3; i++ ) {
			end[i] = k % 16 == 15 ? start[i] : start[i] + Uniform( -1024, 1024 );
			maxs[i] = k % 3 ? Uniform( 1, 48 ) : 0;
			mins[i] = -maxs[i];
		}
		CM_BoxTrace( &t, start, end, mins, maxs, 0, masks[k % 4], k % 3 == 2 );
		HashTrace( &t );
		hits += t.fraction < 1;
	}
	traceDigest = fnv;
	fnv = 2166136261u;
	c1 = clock();
	for ( k = 0; k < samples; k++ ) {
		for ( i = 0; i < 3; i++ ) {
			start[i] = Uniform( lo[i], hi[i] );
			end[i] = Uniform( 1, 512 );
		}
		VectorSubtract( start, end, mins );
		VectorAdd( start, end, maxs );
		count = CM_BoxLeafnums( mins, maxs, list, sizes[k % 3], &last );
		Hash( &count, sizeof( count ) );
		Hash( &last, sizeof( last ) );
		Hash( list, count * sizeof( *list ) );
		leafs += count;
	}
	leafDigest = fnv;
	fnv = 2166136261u;
	for ( k = 0; k < samples; k++ ) {
		for ( i = 0; i < 3; i++ ) {
			start[i] = Uniform( lo[i], hi[i] );
			end[i] = Uniform( 1, 512 );
		}
		VectorSubtract( start, end, mins );
		VectorAdd( start, end, maxs );
		count = CM_BoxBrushes( mins, maxs, brushes, 1024 );
		Hash( &count, sizeof( count ) );
		for ( i = 0; i < count; i++ ) {
			last = brushes[i] - cm.brushes;
			Hash( &last, sizeof( last ) );
		}
	}
	brushDigest = fnv;
	c2 = clock();
#ifdef WALK_STACKS
	Check( cm.treeDepth == (int)TreeDepth(), "the collision walk stacks hold the tree's depth" );
#endif
	printf( "PASS %s: %d nodes, tree depth %u\n", path, cm.numNodes, TreeDepth() );
	printf( "     %d traces (%d hit) digest %08x; %d leaf queries (%d leaves) digest %08x; brush queries digest %08x\n",
			samples, hits, traceDigest, samples, leafs, leafDigest, brushDigest );
	printf( "     %.3f s of traces, %.3f s of box queries\n",
			(double)( c1 - c0 ) / CLOCKS_PER_SEC, (double)( c2 - c1 ) / CLOCKS_PER_SEC );
	return 0;
}

int main( int argc, char **argv ) {
	const char *s = getenv( "BSP_WALK_SAMPLES" );
	int i, failed = 0;
	Swap_Init();
	if ( argc > 1 ) {
		for ( i = 1; i < argc; i++ ) {
			failed += RealMap( argv[i], s ? atoi( s ) : 20000 );
		}
		return failed ? 1 : 0;
	}
	failed += DeepChain( "CM_BoxLeafnums_r: 512 leaf and brush box queries", 1, 0 );
	failed += DeepChain( "CM_TraceThroughTree: 2048 traces and position tests", 0, 1 );
	return failed ? 1 : 0;
}
