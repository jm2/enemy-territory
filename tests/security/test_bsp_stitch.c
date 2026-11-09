/* R_StitchPatches' reversed loops (the grid1 edge walked from its last point
 * down) insert grid1's point k - 1 into grid2 with the LOD error at k + 1. On
 * the first pass k is the last point, so that read was one float past the
 * error array. In retail the array is a zone block (ri.Z_Malloc is
 * Z_TagMalloc with TAG_RENDERER), and the float after it is usually the
 * zone's trash tester, ZONEID (0x1d4a11): read as a float, a denormal of
 * about 2.7e-39, which RB_SurfaceGrid's `error <= lodError` treats as no
 * error at all (or stale bytes, when the zone left a small fragment
 * unsplit). The read must stay in bounds (ASan reports it otherwise) and
 * give that zero; every read inside the array keeps its value, so stitching
 * on valid maps is unchanged. The real tr_bsp.c, with the grid inserts
 * stubbed. Adapted from Quake III's tests/bsp_stitch_regression.c
 * (1b5fe234).
 *
 * Also R_FixSharedVertexLodError, which recursed once per touching patch of a
 * LOD group: a 4,096-patch chain must not overflow a 256 KB stack, and random
 * small groups print a digest of every grid after it, to compare trees with
 * (Q3A 41ac424f). */
#include "../../src/renderer/tr_bsp.c"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

trGlobals_t tr;
glconfig_t glConfig;
refimport_t ri;

void QDECL Com_Error( int level, const char *fmt, ... ) { fprintf( stderr, "Com_Error: %s\n", fmt ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }

static int inserts;
static float insertedError;
static vec3_t insertedPoint;
static void Check( int condition, const char *message ) {
	if ( !condition ) {
		printf( "FAIL: %s\n", message );
		exit( 1 );
	}
}
static int insertFails;     // the zone has no block for the wider grid: Z_TryMalloc's NULL
srfGridMesh_t *R_GridInsertColumn( srfGridMesh_t *grid, int column, int row, vec3_t point, float loderror ) {
	(void)column; (void)row;
	inserts++;
	insertedError = loderror;
	VectorCopy( point, insertedPoint );
	return insertFails ? NULL : grid;
}
srfGridMesh_t *R_GridInsertRow( srfGridMesh_t *grid, int row, int column, vec3_t point, float loderror ) {
	return R_GridInsertColumn( grid, column, row, point, loderror );
}

/* A width x 2 grid, or 2 x height when transposed, whose first row (column)
 * runs along x at y = rowY and whose other one is far away. */
static srfGridMesh_t *Grid( int count, const float *xs, float rowY, float farY, int transposed ) {
	int width = transposed ? 2 : count, height = transposed ? count : 2;
	srfGridMesh_t *grid = calloc( 1, sizeof( *grid ) + width * height * sizeof( drawVert_t ) );
	int i;
	Check( grid != NULL, "grid allocation" );
	grid->surfaceType = SF_GRID;
	grid->width = width;
	grid->height = height;
	/* exact-size heap arrays, so a read past the end is caught */
	grid->widthLodError = malloc( width * sizeof( float ) );
	grid->heightLodError = malloc( height * sizeof( float ) );
	Check( grid->widthLodError && grid->heightLodError, "LOD error arrays" );
	for ( i = 0; i < width; i++ ) {
		grid->widthLodError[i] = 10.0f + i;
	}
	for ( i = 0; i < height; i++ ) {
		grid->heightLodError[i] = 20.0f + i;
	}
	for ( i = 0; i < count; i++ ) {
		drawVert_t *edge = transposed ? &grid->verts[i * width] : &grid->verts[i];
		drawVert_t *other = transposed ? &grid->verts[i * width + 1] : &grid->verts[width + i];
		edge->xyz[0] = xs[i];
		edge->xyz[1] = rowY;
		other->xyz[0] = xs[i];
		other->xyz[1] = farY;
	}
	return grid;
}

static void FreeGrid( srfGridMesh_t *grid ) {
	free( grid->widthLodError );
	free( grid->heightLodError );
	free( grid );
}

/* grid2's edge runs the other way from grid1's, so only a reversed loop matches it */
static void Stitch( int count1, int transposed1, int transposed2, float wantError, const char *name ) {
	static const float xs1[] = { 0, 1, 2, 3, 4 }, xs2[] = { 2, 0 };
	srfGridMesh_t *grids[2];
	srfGridMesh_t *grid1 = Grid( count1, xs1, 0, 100, transposed1 );
	srfGridMesh_t *grid2 = Grid( 2, xs2, 0, -100, transposed2 );

	// the grids the map preflight stitches, in surface order
	grids[0] = grid1;
	grids[1] = grid2;
	s_stitchGrids = grids;
	s_numStitchGrids = 2;
	inserts = 0;
	insertedError = -1;

	Check( R_StitchPatches( 0, 1 ) && inserts == 1, "the reversed edge is stitched once" );
	Check( insertedPoint[0] == 1 && insertedPoint[1] == 0, "grid1's middle point is inserted" );
	Check( insertedError == wantError, "the inserted point's LOD error" );
	printf( "ok   %s\n", name );
	FreeGrid( grid1 );
	FreeGrid( grid2 );
}

/* Each of R_StitchPatches' eight inserts (grid1's width or height edge, walked
 * forward or reversed, into a column or a row of grid2) with the insert
 * answering NULL: R_StitchPatches must return qfalse with s_stitchFailed set
 * and the old grid still in the list, and read nothing through the NULL. */
static void NullInsert( int transposed1, int transposed2, int reversed, const char *name ) {
	static const float xs1[] = { 0, 1, 2 }, forward[] = { 0, 2 }, backward[] = { 2, 0 };
	srfGridMesh_t *grids[2];
	srfGridMesh_t *grid1 = Grid( 3, xs1, 0, 100, transposed1 );
	srfGridMesh_t *grid2 = Grid( 2, reversed ? backward : forward, 0, -100, transposed2 );

	grids[0] = grid1;
	grids[1] = grid2;
	s_stitchGrids = grids;
	s_numStitchGrids = 2;
	s_stitchFailed = qfalse;
	inserts = 0;
	insertFails = 1;
	Check( !R_StitchPatches( 0, 1 ), "a failed insert stitches nothing" );
	insertFails = 0;
	Check( inserts == 1 && insertedPoint[0] == 1, "the insert was tried once, with grid1's middle point" );
	Check( s_stitchFailed, "the failed insert is reported" );
	Check( grids[1] == grid2 && grids[0] == grid1, "the old grids stay in the list" );
	printf( "ok   NULL insert: %s\n", name );
	s_stitchFailed = qfalse;
	FreeGrid( grid1 );
	FreeGrid( grid2 );
}

/* ---- R_FixSharedVertexLodError ---- */

/* A grid of exactly its own bytes, its LOD errors exact-size heap arrays. */
static srfGridMesh_t *LodGrid( int width, int height ) {
	srfGridMesh_t *grid = calloc( 1, sizeof( *grid ) + ( width * height - 1 ) * sizeof( drawVert_t ) );
	Check( grid != NULL, "grid allocation" );
	grid->surfaceType = SF_GRID;
	grid->width = width;
	grid->height = height;
	grid->widthLodError = calloc( width, sizeof( float ) );
	grid->heightLodError = calloc( height, sizeof( float ) );
	Check( grid->widthLodError && grid->heightLodError, "LOD error arrays" );
	return grid;
}

static void SetSurfaces( msurface_t *surfaces, srfGridMesh_t **grids, int count ) {
	int i;
	memset( surfaces, 0, count * sizeof( *surfaces ) );
	for ( i = 0; i < count; i++ ) {
		surfaces[i].data = (surfaceType_t *)grids[i];
	}
	s_worldData.surfaces = surfaces;
	s_worldData.numsurfaces = count;
}

static void CheckWalkCleared( srfGridMesh_t **grids, int count ) {
	int i;
	for ( i = 0; i < count; i++ ) {
		if ( grids[i]->surfaceType == SF_GRID ) {
			Check( grids[i]->lodParent == NULL && grids[i]->lodNextSurface == 0, "the walk leaves no continuation behind" );
		}
	}
}

#define CHAIN 4096
static srfGridMesh_t *chain[CHAIN];
static msurface_t chainSurfaces[CHAIN];

static void *ChainThread( void *unused ) {
	(void)unused;
	R_FixSharedVertexLodError();
	return NULL;
}

/* Patch i spans x 2i to 2i + 2: its right column is patch i + 1's left one,
 * whose middle point is the only one they share, so the walk goes from each
 * patch to the next, CHAIN deep. Run in a thread with a 256 KB stack. */
static void LodChain( void ) {
	pthread_attr_t attr;
	pthread_t thread;
	int i, x, y;

	for ( i = 0; i < CHAIN; i++ ) {
		chain[i] = LodGrid( 3, 3 );
		for ( y = 0; y < 3; y++ ) {
			for ( x = 0; x < 3; x++ ) {
				chain[i]->verts[y * 3 + x].xyz[0] = 2 * i + x;
				chain[i]->verts[y * 3 + x].xyz[1] = y;
			}
			chain[i]->heightLodError[y] = 100.0f + i;
		}
		chain[i]->lodRadius = 50;     // one LOD group
	}
	chain[0]->heightLodError[1] = 7;
	SetSurfaces( chainSurfaces, chain, CHAIN );
	Check( !pthread_attr_init( &attr ) && !pthread_attr_setstacksize( &attr, 256 * 1024 ), "thread attributes" );
	Check( !pthread_create( &thread, &attr, ChainThread, NULL ) && !pthread_join( thread, NULL ), "LOD thread" );
	for ( i = 0; i < CHAIN; i++ ) {
		Check( chain[i]->lodFixed == 2 && chain[i]->heightLodError[1] == 7, "the first patch's error reaches the whole chain" );
		Check( chain[i]->heightLodError[0] == 100.0f + i && chain[i]->heightLodError[2] == 100.0f + i, "edge corners keep their errors" );
	}
	CheckWalkCleared( chain, CHAIN );
	printf( "ok   R_FixSharedVertexLodError down a %d-patch chain in a 256 KB stack\n", CHAIN );
}

static unsigned int lodRng = 1;
static int LodRandom( int n ) {
	lodRng = lodRng * 1103515245u + 12345u;
	return ( lodRng >> 8 ) % n;
}

/* 3,000 groups of up to 8 surfaces: grids of 2 to 5 points a side on a
 * coarse lattice, so edges often meet, with jitter around the 0.1 match
 * distance, two LOD groups, some surfaces not grids and some grids already
 * fixed. Prints a digest of every grid's errors and lodFixed afterwards. */
static void LodRandomGroups( void ) {
	static const float jitter[] = { 0, 0, 0, 0.05f, 0.099f, 0.11f, -0.05f };
	srfGridMesh_t *grids[8];
	msurface_t surfaces[8];
	surfaceType_t other = SF_SKIP;
	unsigned int h = 2166136261u;
	int fixture, count, i, j, k, changed = 0;

	for ( fixture = 0; fixture < 3000; fixture++ ) {
		count = 2 + LodRandom( 7 );
		for ( i = 0; i < count; i++ ) {
			grids[i] = LodGrid( 2 + LodRandom( 4 ), 2 + LodRandom( 4 ) );
			for ( j = 0; j < grids[i]->width * grids[i]->height; j++ ) {
				for ( k = 0; k < 3; k++ ) {
					grids[i]->verts[j].xyz[k] = LodRandom( 3 ) + jitter[LodRandom( 7 )];
				}
			}
			for ( j = 0; j < grids[i]->width; j++ ) {
				grids[i]->widthLodError[j] = LodRandom( 50 );
			}
			for ( j = 0; j < grids[i]->height; j++ ) {
				grids[i]->heightLodError[j] = 100 + LodRandom( 50 );
			}
			grids[i]->lodRadius = LodRandom( 4 ) ? 10 : 20;
			grids[i]->lodOrigin[LodRandom( 3 )] = LodRandom( 8 ) ? 0 : 1;
			grids[i]->lodFixed = LodRandom( 10 ) ? 0 : 1 + LodRandom( 2 );
		}
		SetSurfaces( surfaces, grids, count );
		for ( i = 0; i < count; i++ ) {
			if ( !LodRandom( 6 ) ) {
				surfaces[i].data = &other;
			}
		}
		R_FixSharedVertexLodError();
		for ( i = 0; i < count; i++ ) {
			srfGridMesh_t *g = grids[i];
			for ( j = 0; j < g->width; j++ ) {
				changed += g->widthLodError[j] >= 100;     // a height error moved into a width one
				h = ( h ^ (unsigned int)g->widthLodError[j] ) * 16777619u;
			}
			for ( j = 0; j < g->height; j++ ) {
				changed += g->heightLodError[j] < 100;
				h = ( h ^ (unsigned int)g->heightLodError[j] ) * 16777619u;
			}
			h = ( h ^ (unsigned int)g->lodFixed ) * 16777619u;
		}
		CheckWalkCleared( grids, count );
		for ( i = 0; i < count; i++ ) {
			FreeGrid( grids[i] );
		}
	}
	Check( changed > 1000, "the random groups exercise the walk" );
	printf( "ok   R_FixSharedVertexLodError on 3000 random groups, %d errors crossed edges, LOD digest %08x\n", changed, h );
	fflush( stdout );
}

int main( void ) {
	/* the last point's error, past the array: retail's zero */
	Stitch( 3, 0, 0, 0.0f, "reversed width edge into a column" );
	Stitch( 3, 0, 1, 0.0f, "reversed width edge into a row" );
	Stitch( 3, 1, 0, 0.0f, "reversed height edge into a column" );
	Stitch( 3, 1, 1, 0.0f, "reversed height edge into a row" );
	/* matched one pass later, at k = 2: point 3's error, inside the array, as before */
	Stitch( 5, 0, 0, 13.0f, "reversed width edge, error inside the array" );
	Stitch( 5, 1, 1, 23.0f, "reversed height edge, error inside the array" );
	printf( "bsp stitch: R_StitchPatches reads no LOD error past its array\n" );
	NullInsert( 0, 0, 0, "width edge into a column" );
	NullInsert( 0, 1, 0, "width edge into a row" );
	NullInsert( 1, 0, 0, "height edge into a column" );
	NullInsert( 1, 1, 0, "height edge into a row" );
	NullInsert( 0, 0, 1, "reversed width edge into a column" );
	NullInsert( 0, 1, 1, "reversed width edge into a row" );
	NullInsert( 1, 0, 1, "reversed height edge into a column" );
	NullInsert( 1, 1, 1, "reversed height edge into a row" );
	printf( "bsp stitch: a NULL insert at each of the eight sites is refused, the old grid kept\n" );
	fflush( stdout );
	LodRandomGroups();
	LodChain();
	printf( "bsp LOD: R_FixSharedVertexLodError takes constant stack\n" );
	return 0;
}
