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
 * (1b5fe234). */
#include "../../src/renderer/tr_bsp.c"
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
srfGridMesh_t *R_GridInsertColumn( srfGridMesh_t *grid, int column, int row, vec3_t point, float loderror ) {
	(void)column; (void)row;
	inserts++;
	insertedError = loderror;
	VectorCopy( point, insertedPoint );
	return grid;
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
	msurface_t surfaces[2];
	srfGridMesh_t *grid1 = Grid( count1, xs1, 0, 100, transposed1 );
	srfGridMesh_t *grid2 = Grid( 2, xs2, 0, -100, transposed2 );

	memset( surfaces, 0, sizeof( surfaces ) );
	surfaces[0].data = (void *)grid1;
	surfaces[1].data = (void *)grid2;
	s_worldData.surfaces = surfaces;
	s_worldData.numsurfaces = 2;
	inserts = 0;
	insertedError = -1;

	Check( R_StitchPatches( 0, 1 ) && inserts == 1, "the reversed edge is stitched once" );
	Check( insertedPoint[0] == 1 && insertedPoint[1] == 0, "grid1's middle point is inserted" );
	Check( insertedError == wantError, "the inserted point's LOD error" );
	printf( "ok   %s\n", name );
	FreeGrid( grid1 );
	FreeGrid( grid2 );
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
	return 0;
}
