/* Collision patch facet budgets and winding ownership, with cm_patch.c
 * included whole for its static build arrays and the real cm_polylib.c.
 * Run under ASan and UBSan.
 *
 * Adapted from Q3A tests/bsp_winding_regression.c (734ebe51, 8ee4b726). */
#include "../../src/qcommon/cm_patch.c"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int c_active_windings;
static void *zones[16];
static int zoneSizes[16], zoneLive, zoneAllocations, zoneFrees;
static int hunkCalls;

/* Hunk blocks live until exit, as the engine hunk does. */
const char *__asan_default_options( void ) { return "detect_leaks=0"; }

static void Check( int ok, const char *message ) {
	if ( !ok ) {
		fprintf( stderr, "  check failed: %s\n", message );
		exit( 1 );
	}
}

/* ---- engine stubs ---- */

void *Z_Malloc( int size ) {
	void *p;
	Check( size > 0 && zoneLive < 16, "bounded winding allocation" );
	p = malloc( size );
	Check( p != NULL, "winding allocation" );
	zones[zoneLive] = p;
	zoneSizes[zoneLive++] = size;
	zoneAllocations++;
	return p;
}
void Z_Free( void *p ) {
	int i;
	for ( i = 0; i < zoneLive; i++ ) {
		if ( zones[i] == p ) {
			free( p );
			zoneLive--;
			zones[i] = zones[zoneLive];
			zoneSizes[i] = zoneSizes[zoneLive];
			zoneFrees++;
			return;
		}
	}
	Check( 0, "freed a block that is not a winding" );
}
#ifdef Hunk_Alloc
#undef Hunk_Alloc
#endif
void *Hunk_Alloc( int size, ha_pref preference ) {
	(void)preference;
	hunkCalls++;
	return calloc( 1, size ? size : 1 );
}
void QDECL Com_Error( int level, const char *fmt, ... ) {
	(void)level;
	fprintf( stderr, "  unexpected error: %s\n", fmt );
	exit( 1 );
}
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_DPrintf( const char *fmt, ... ) { (void)fmt; }
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	static cvar_t v;
	(void)name; (void)value; (void)flags;
	return &v;
}
cvar_t *cm_playerCurveClip;
void BotDrawDebugPolygons( BotPolyFunc drawPoly, int value ) { (void)drawPoly; (void)value; }

/* ---- facets ---- */

/* A 32-unit square on z = 0, bounded by its four sides. */
static void BuildFacet( facet_t *facet ) {
	int i;
	memset( planes, 0, sizeof( planes ) );
	memset( facet, 0, sizeof( *facet ) );
	planes[0].plane[2] = 1;
	VectorSet( planes[1].plane, 1, 0, 0 ); planes[1].plane[3] = -16;
	VectorSet( planes[2].plane, -1, 0, 0 ); planes[2].plane[3] = -16;
	VectorSet( planes[3].plane, 0, 1, 0 ); planes[3].plane[3] = -16;
	VectorSet( planes[4].plane, 0, -1, 0 ); planes[4].plane[3] = -16;
	facet->surfacePlane = 0;
	facet->numBorders = 4;
	for ( i = 0; i < 4; i++ ) {
		facet->borderPlanes[i] = i + 1;
		facet->borderInward[i] = qtrue;
	}
}

/* The same square turned 45 degrees: its edges are not axial, so it takes edge bevels. */
static void TurnFacet( void ) {
	int i;
	for ( i = 1; i <= 4; i++ ) {
		float x = planes[i].plane[0], y = planes[i].plane[1];
		planes[i].plane[0] = ( x - y ) * 0.70710677f;
		planes[i].plane[1] = ( x + y ) * 0.70710677f;
	}
}

/* The facet tilted 30 degrees about x, so that its edges slant out of every axis
 * plane and take edge bevels. Planes through the origin keep their distances. */
static void TiltFacet( void ) {
	int i;
	for ( i = 0; i <= 4; i++ ) {
		float y = planes[i].plane[1], z = planes[i].plane[2];
		planes[i].plane[1] = y * 0.8660254f - z * 0.5f;
		planes[i].plane[2] = y * 0.5f + z * 0.8660254f;
	}
}

/* Far axial planes as borders 4-9, so the axial bevel step adds none. */
static void AxialBorders( facet_t *facet ) {
	int i;
	for ( i = 0; i < 6; i++ ) {
		VectorClear( planes[5 + i].plane );
		planes[5 + i].plane[i / 2] = ( i & 1 ) ? -1 : 1;
		planes[5 + i].plane[3] = -1000;
		facet->borderPlanes[4 + i] = 5 + i;
		facet->borderInward[4 + i] = qtrue;
	}
	facet->numBorders = 10;
	numPlanes = 11;
}

static void FacetOwnership( void ) {
	facet_t facet;
	int missing, repeat, before;

	BuildFacet( &facet );
	Check( CM_ValidateFacet( &facet ) && !zoneLive && !c_active_windings, "a bounded facet frees its winding" );
	for ( missing = 0; missing < 4; missing++ ) {
		for ( repeat = 0; repeat < 128; repeat++ ) {
			BuildFacet( &facet );
			facet.borderPlanes[missing] = -1;
			before = zoneAllocations;
			Check( !CM_ValidateFacet( &facet ), "a missing border is refused" );
			Check( zoneAllocations > before && !zoneLive && !c_active_windings, "a missing border frees the winding" );
		}
	}
	BuildFacet( &facet );
	facet.surfacePlane = -1;
	before = zoneAllocations;
	Check( !CM_ValidateFacet( &facet ) && zoneAllocations == before, "a missing surface plane allocates nothing" );
	BuildFacet( &facet );
	planes[1].plane[3] = MAX_MAP_BOUNDS * 2;
	Check( !CM_ValidateFacet( &facet ) && !zoneLive && !c_active_windings, "a facet chopped away frees its winding" );
	BuildFacet( &facet );
	facet.numBorders = 0;
	Check( !CM_ValidateFacet( &facet ) && !zoneLive && !c_active_windings, "an unbounded facet frees its winding" );
}

static void CopyOwnership( void ) {
	int count, i, j;
	for ( count = 0; count <= MAX_POINTS_ON_WINDING; count++ ) {
		winding_t *original = AllocWinding( count ), *copy;
		int bytes = offsetof( winding_t, p ) + count * sizeof( vec3_t );
		original->numpoints = count;
		for ( i = 0; i < count; i++ ) {
			for ( j = 0; j < 3; j++ ) {
				original->p[i][j] = ( i * 3 + j - 96 ) * 0.125f;
			}
		}
		copy = CopyWinding( original );
		Check( zoneLive == 2 && zoneSizes[0] == bytes && zoneSizes[1] == bytes && !memcmp( original, copy, bytes ),
			   "a winding copy has the header and every point" );
		if ( count ) {
			copy->p[count - 1][2] += 1;
			Check( original->p[count - 1][2] != copy->p[count - 1][2], "a winding copy is its own" );
		}
		FreeWinding( copy );
		FreeWinding( original );
		Check( !zoneLive && !c_active_windings, "both windings freed" );
	}
}

static void PlaneAndBorderBudgets( void ) {
	facet_t facet;
	float plane[4] = { 1, 0, 0, 123 }, other[4] = { 0, 1, 0, 456 };
	vec3_t a = { 0, 0, 123 }, b = { 0, 1, 123 }, c = { 1, 0, 123 };
	int i, flipped;
	const char *error;

	memset( planes, 0, sizeof( planes ) );
	for ( i = 0; i < MAX_PATCH_PLANES; i++ ) {
		planes[i].plane[0] = 1;
		planes[i].plane[3] = -1000 - i;
	}
	numPlanes = MAX_PATCH_PLANES;
	Check( CM_FindPlane2( plane, &flipped ) == CM_PATCH_PLANE_LIMIT && numPlanes == MAX_PATCH_PLANES, "a full plane table refuses a bevel plane" );
	Check( CM_FindPlane( a, b, c ) == CM_PATCH_PLANE_LIMIT && numPlanes == MAX_PATCH_PLANES, "a full plane table refuses a triangle plane" );
	Vector4Copy( plane, planes[7].plane );
	Check( CM_FindPlane2( plane, &flipped ) == 7 && !flipped, "a full plane table still finds a plane it holds" );
	numPlanes = MAX_PATCH_PLANES - 1;
	Check( CM_FindPlane2( other, &flipped ) == MAX_PATCH_PLANES - 1 && numPlanes == MAX_PATCH_PLANES, "the last plane fits" );
	Check( !strcmp( CM_PatchPlaneError( CM_PATCH_PLANE_LIMIT ), "MAX_PATCH_PLANES" ) && CM_PatchPlaneError( -1 ) == NULL &&
		   CM_PatchPlaneError( CM_PATCH_UNRESOLVABLE_PLANE ) != NULL, "plane failures are told from degenerate planes" );

	// the last of the 26 borders holds the opposite plane
	BuildFacet( &facet );
	numPlanes = 5;
	for ( i = 4; i < 26; i++ ) {
		facet.borderPlanes[i] = 4;
		facet.borderInward[i] = qtrue;
	}
	facet.numBorders = 25;
	Check( CM_AddFacetBevels( &facet ) == NULL && facet.numBorders == 26 && facet.borderPlanes[25] == facet.surfacePlane && !zoneLive,
		   "the opposite plane fills the last border" );
	// with all 26 used, the old code wrote the opposite plane into borderPlanes[26]
	facet.numBorders = 26;
	error = CM_AddFacetBevels( &facet );
	Check( error && !strcmp( error, "MAX_FACET_BORDERS" ) && facet.numBorders == 26 && !zoneLive, "no border past the array" );
	facet.numBorders = 27;
	error = CM_AddFacetBevels( &facet );
	Check( error && !strcmp( error, "MAX_FACET_BORDERS" ) && !zoneLive, "a border count past the array is refused before use" );

	// an edge bevel may not take the opposite plane's border
	BuildFacet( &facet );
	numPlanes = 5;
	TurnFacet();
	for ( i = 4; i < 25; i++ ) {
		facet.borderPlanes[i] = 4;
		facet.borderInward[i] = qtrue;
	}
	facet.numBorders = 25;
	error = CM_AddFacetBevels( &facet );
	Check( error && !strcmp( error, "MAX_FACET_BORDERS" ) && facet.numBorders == 25 && !zoneLive && !c_active_windings,
		   "a bevel past the border budget frees the winding" );

	// a tilted facet whose axial bevels are already borders: with 25 borders, its edge
	// bevel candidate may still be written into borderPlanes[25] (the old code dropped
	// some there and went on), but keeping it is refused at the count, winding freed
	BuildFacet( &facet );
	TurnFacet();
	TiltFacet();
	AxialBorders( &facet );
	Check( CM_AddFacetBevels( &facet ) == NULL && numPlanes > 11 && facet.borderPlanes[facet.numBorders - 1] == facet.surfacePlane,
		   "the tilted square takes edge bevels" );
	BuildFacet( &facet );
	TurnFacet();
	TiltFacet();
	AxialBorders( &facet );
	for ( i = 10; i < 25; i++ ) {
		facet.borderPlanes[i] = 4;
		facet.borderInward[i] = qtrue;
	}
	facet.numBorders = 25;
	error = CM_AddFacetBevels( &facet );
	Check( error && !strcmp( error, "MAX_FACET_BORDERS" ) && facet.numBorders == 25 && facet.borderPlanes[25] >= 11 &&
		   !zoneLive && !c_active_windings, "an edge bevel candidate in the last slot is refused when kept" );

	// a bevel plane that does not fit the table
	BuildFacet( &facet );
	numPlanes = MAX_PATCH_PLANES;
	TurnFacet();
	error = CM_AddFacetBevels( &facet );
	Check( error && !strcmp( error, "MAX_PATCH_PLANES" ) && !zoneLive && !c_active_windings, "a bevel plane past the table frees the winding" );
	numPlanes = 0;
}

static void PreflightState( void ) {
	vec3_t points[9], savedPoints[4], curved[9];
	int i, sizes[3], before[3], hunk = hunkCalls;
	facet_t oldFacet;
	patchCollide_t oldPatch, *pc;

	for ( i = 0; i < 9; i++ ) {
		VectorSet( points[i], ( i % 3 ) * 32, ( i / 3 ) * 32, 0 );
		VectorSet( curved[i], ( i % 3 ) * 32, ( i / 3 ) * 32, i == 4 ? 1e7f : 0 );
	}
	debugBlock = qtrue;
	for ( i = 0; i < 4; i++ ) {
		VectorSet( debugBlockPoints[i], i + 0.25f, i + 0.5f, i + 0.75f );
	}
	memcpy( savedPoints, debugBlockPoints, sizeof( savedPoints ) );
	numPlanes = 17;
	numFacets = 11;
	debugFacet = &oldFacet;
	debugPatchCollide = &oldPatch;
	Check( CM_ValidatePatchCollideAllocations( 3, 3, points, sizes ) == NULL && numPlanes == 17 && numFacets == 11 && debugBlock &&
		   !memcmp( savedPoints, debugBlockPoints, sizeof( savedPoints ) ) && debugFacet == &oldFacet && debugPatchCollide == &oldPatch &&
		   !zoneLive && hunkCalls == hunk, "the preflight changes no collision state" );
	debugFacet = NULL;
	debugPatchCollide = NULL;

	// the sizes are those CM_GeneratePatchCollide then allocates
	pc = CM_GeneratePatchCollide( 3, 3, points, qtrue );
	Check( hunkCalls == hunk + 3 && sizes[0] == (int)sizeof( patchCollide_t ) &&
		   sizes[1] == pc->numFacets * (int)sizeof( facet_t ) && sizes[2] == pc->numPlanes * (int)sizeof( patchPlane_t ) && pc->numFacets > 0,
		   "the preflight sizes match the generated patch" );
	memcpy( before, sizes, sizeof( sizes ) );
	Check( CM_ValidatePatchCollideAllocations( 3, 3, curved, sizes ) != NULL && !memcmp( before, sizes, sizeof( sizes ) ) && !zoneLive,
		   "a refused patch writes no sizes" );
	Check( CM_ValidatePatchCollideAllocations( 2, 3, NULL, sizes ) != NULL && !memcmp( before, sizes, sizeof( sizes ) ),
		   "bad parameters are refused before the points are read" );
}

/* A 3x5 patch whose first segment lies on one line: CM_GridPlane finds no plane
 * for its cells. ET 2.60 warns and loads; the edge-plane code read planes[-1]. */
static void SquashedSegment( void ) {
	vec3_t points[15];
	int i, j, sizes[3];
	patchCollide_t *pc;

	for ( j = 0; j < 5; j++ ) {
		for ( i = 0; i < 3; i++ ) {
			if ( j < 3 ) {
				VectorSet( points[j * 3 + i], i * 32 + j * 4, 0, 0 );
			} else {
				VectorSet( points[j * 3 + i], i * 32, ( j - 2 ) * 32, 0 );
			}
		}
	}
	Check( CM_ValidatePatchCollideAllocations( 3, 5, points, sizes ) == NULL, "a segment on a line passes the preflight" );
	pc = CM_GeneratePatchCollide( 3, 5, points, qtrue );
	Check( pc && pc->numFacets >= 1 && sizes[1] == pc->numFacets * (int)sizeof( facet_t ) &&
		   sizes[2] == pc->numPlanes * (int)sizeof( patchPlane_t ) && !zoneLive, "a segment on a line loads, its flat cells dropped" );
	for ( i = 0; i < pc->numFacets; i++ ) {
		Check( pc->facets[i].surfacePlane >= 0 && pc->facets[i].surfacePlane < pc->numPlanes, "every kept facet has a surface plane" );
		for ( j = 0; j < pc->facets[i].numBorders; j++ ) {
			Check( pc->facets[i].borderPlanes[j] >= -1 && pc->facets[i].borderPlanes[j] < pc->numPlanes, "every border is a plane or none" );
		}
	}
}

int main( void ) {
	FacetOwnership();
	CopyOwnership();
	PlaneAndBorderBudgets();
	PreflightState();
	SquashedSegment();
	Check( zoneAllocations == zoneFrees && !zoneLive && !c_active_windings, "every winding freed" );
	puts( "collision patch facet budgets and winding ownership passed" );
	return 0;
}
