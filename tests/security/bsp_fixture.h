/* Shared BSP fixture for test_bsp_cm.c and test_bsp_tr.c.
 *
 * Builds a minimal valid ET (version 47) BSP with one surface of every type,
 * fogs including a global one, a lightmap, a light grid and visibility, then
 * corrupts one field per case. Each case runs in its own process so that a
 * sanitizer abort on the unfixed tree fails that case only. The including
 * file supplies LoaderAccept(), LoaderReject() and LoaderTry().
 *
 * Adapted from Q3A tests/bsp_fixture.h and tests/bsp_*_regression.c.
 */
#ifndef MAX_SUBMODELS
#define MAX_SUBMODELS 512     /* cm_local.h */
#endif
#include <stddef.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined( __has_include )
#if __has_include( "../../src/qcommon/bsp_tree.h" )
#include "../../src/qcommon/bsp_geometry.h"
#include "../../src/qcommon/bsp_tree.h"
#define HAVE_BSP_VALIDATOR 1
#endif
#endif

/* Hunk blocks live until exit, as the engine hunk does. */
const char *__asan_default_options( void ) { return "detect_leaks=0"; }

/* ---- engine stubs ---- */

#undef vsnprintf

static jmp_buf dropJump;
static int expectDrop, drops;
static char lastError[1024];

static void Fail( const char *message ) {
	fprintf( stderr, "  check failed: %s\n", message );
	exit( 1 );
}
static void Check( int ok, const char *message ) {
	if ( !ok ) {
		Fail( message );
	}
}

void QDECL Com_Error( int level, const char *fmt, ... ) {
	va_list args;
	va_start( args, fmt );
	vsnprintf( lastError, sizeof( lastError ), fmt, args );
	va_end( args );
	if ( !expectDrop || level != ERR_DROP ) {
		fprintf( stderr, "  unexpected error %d: %s\n", level, lastError );
		exit( 1 );
	}
	drops++;
	longjmp( dropJump, 1 );
}
void QDECL Com_Printf( const char *fmt, ... ) { (void)fmt; }
void QDECL Com_DPrintf( const char *fmt, ... ) { (void)fmt; }
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
char *CopyString( const char *in ) {
	char *out = malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

/* Terminated within the lump, or by the byte the loader adds after it. */
static int Terminated( const char *text, int length ) {
	return memchr( text, 0, length ) != NULL || text[length] == 0;
}

/* Every hunk block is its own heap block, so ASan sees overruns past one. */
static void **hunkBlocks;
static int hunkCount, hunkCapacity;
static void *TestHunk( int size ) {
	void *p;
	Check( size >= 0, "negative hunk allocation" );
	if ( hunkCount == hunkCapacity ) {
		hunkCapacity = hunkCapacity ? hunkCapacity * 2 : 4096;
		hunkBlocks = realloc( hunkBlocks, hunkCapacity * sizeof( *hunkBlocks ) );
	}
	p = calloc( 1, size ? size : 1 );
	Check( p != NULL, "hunk allocation" );
	hunkBlocks[hunkCount++] = p;
	return p;
}

/* R_StitchPatches reads heightLodError[k + 1] (and widthLodError[k + 1]) in its
 * reversed loops, one float past the array when k is the last row; Q3 and Q3A
 * have it too, and retail radar.bsp reaches it. Only the real-map run allows
 * that one float, so the corrupted-map cases stay strict. */
static int zSlack;
static int realMaps;    // set while loading real maps over and over
void *Z_Malloc( int size ) { return calloc( 1, ( size ? size : 1 ) + zSlack ); }
void Z_Free( void *p ) { free( p ); }

/* The file: exactly the advertised bytes, with no terminating NUL past them. */
static byte *source;
static int sourceSize;
#define SOURCE_CAPACITY ( 40 * 1024 * 1024 )
static void *fileBlock;
static int reads, frees;

int FS_ReadFile( const char *name, void **buffer ) {
	(void)name;
	Check( fileBlock == NULL, "file read while one is open" );
	reads++;
	fileBlock = malloc( sourceSize ? sourceSize : 1 );
	memcpy( fileBlock, source, sourceSize );
	*buffer = fileBlock;
	return sourceSize;
}
void FS_FreeFile( void *buffer ) {
	Check( buffer == fileBlock, "freed a block that is not the file" );
	free( fileBlock );
	fileBlock = NULL;
	frees++;
}

/* ---- map writer ---- */

static void Word( unsigned int offset, unsigned int value ) {
	int i;
	for ( i = 0; i < 4; i++ ) {
		source[offset + i] = ( value >> ( 8 * i ) ) & 255;
	}
}
static unsigned int Read( unsigned int offset ) {
	return source[offset] | ( source[offset + 1] << 8 ) | ( source[offset + 2] << 16 ) | ( (unsigned int)source[offset + 3] << 24 );
}
static void Float( unsigned int offset, float value ) {
	unsigned int word;
	memcpy( &word, &value, 4 );
	Word( offset, word );
}
static void Lump( int lump, unsigned int offset, unsigned int size ) {
	Word( 8 + lump * 8, offset );
	Word( 12 + lump * 8, size );
}
static unsigned int LumpOfs( int lump ) { return Read( 8 + lump * 8 ); }
static unsigned int LumpLen( int lump ) { return Read( 12 + lump * 8 ); }
static unsigned int Append( int lump, unsigned int size, int align ) {
	unsigned int offset = align ? ( sourceSize + 3 ) & ~3u : (unsigned int)sourceSize;
	memset( source + sourceSize, 0, offset - sourceSize + size );
	Lump( lump, offset, size );
	sourceSize = offset + size;
	return offset;
}
/* Field of record n in a lump. */
#define AT( lump, type, n, field ) ( LumpOfs( lump ) + ( n ) * sizeof( type ) + offsetof( type, field ) )

#define NUM_SURFS   5
enum { S_PLANAR, S_TRISOUP, S_PATCH, S_FLARE, S_FOLIAGE };
static const char worldspawn[] = "{\n\"classname\" \"worldspawn\"\n\"gridsize\" \"64 64 128\"\n}\n";

static void Vertex( int n, float x, float y, float z ) {
	unsigned int o = LumpOfs( LUMP_DRAWVERTS ) + n * sizeof( drawVert_t );
	Float( o + offsetof( drawVert_t, xyz ), x );
	Float( o + offsetof( drawVert_t, xyz ) + 4, y );
	Float( o + offsetof( drawVert_t, xyz ) + 8, z );
	Float( o + offsetof( drawVert_t, normal ) + 8, 1 );
	Word( o + offsetof( drawVert_t, color ), 0xffffffffu );
}

static void Surface( int n, int type, int fog, int firstVert, int numVerts, int firstIndex, int numIndexes, int lightmap ) {
	Word( AT( LUMP_SURFACES, dsurface_t, n, shaderNum ), 0 );
	Word( AT( LUMP_SURFACES, dsurface_t, n, fogNum ), fog );
	Word( AT( LUMP_SURFACES, dsurface_t, n, surfaceType ), type );
	Word( AT( LUMP_SURFACES, dsurface_t, n, firstVert ), firstVert );
	Word( AT( LUMP_SURFACES, dsurface_t, n, numVerts ), numVerts );
	Word( AT( LUMP_SURFACES, dsurface_t, n, firstIndex ), firstIndex );
	Word( AT( LUMP_SURFACES, dsurface_t, n, numIndexes ), numIndexes );
	Word( AT( LUMP_SURFACES, dsurface_t, n, lightmapNum ), lightmap );
	Float( AT( LUMP_SURFACES, dsurface_t, n, lightmapVecs ) + 8 * 4, 1 );    // plane normal z
}

static void BuildMap( void ) {
	unsigned int o;
	int i, j;
	static const int indexes[] = { 0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 1, 2 };

	if ( !source ) {
		source = malloc( SOURCE_CAPACITY );
	}
	memset( source, 0, sizeof( dheader_t ) );
	Word( 0, BSP_IDENT );
	Word( 4, BSP_VERSION );
	sourceSize = sizeof( dheader_t );

	// entities first, so a missing NUL runs into the shader names that follow
	o = Append( LUMP_ENTITIES, sizeof( worldspawn ), 0 );
	memcpy( source + o, worldspawn, sizeof( worldspawn ) );

	Append( LUMP_SHADERS, 2 * sizeof( dshader_t ), 1 );
	strcpy( (char *)source + AT( LUMP_SHADERS, dshader_t, 0, shader ), "textures/test" );
	Word( AT( LUMP_SHADERS, dshader_t, 0, contentFlags ), CONTENTS_SOLID );
	strcpy( (char *)source + AT( LUMP_SHADERS, dshader_t, 1, shader ), "textures/nodraw" );
	Word( AT( LUMP_SHADERS, dshader_t, 1, surfaceFlags ), SURF_NODRAW );

	// six axial planes of the box -1..1: -x, +x, -y, +y, -z, +z
	Append( LUMP_PLANES, 6 * sizeof( dplane_t ), 1 );
	for ( i = 0; i < 6; i++ ) {
		Float( AT( LUMP_PLANES, dplane_t, i, normal ) + ( i / 2 ) * 4, ( i & 1 ) ? 1 : -1 );
		Float( AT( LUMP_PLANES, dplane_t, i, dist ), 1 );
	}

	Append( LUMP_NODES, sizeof( dnode_t ), 1 );
	Word( AT( LUMP_NODES, dnode_t, 0, planeNum ), 1 );
	Word( AT( LUMP_NODES, dnode_t, 0, children ), (unsigned int)-1 );
	Word( AT( LUMP_NODES, dnode_t, 0, children ) + 4, (unsigned int)-2 );

	Append( LUMP_LEAFS, 2 * sizeof( dleaf_t ), 1 );
	Word( AT( LUMP_LEAFS, dleaf_t, 0, cluster ), (unsigned int)-1 );
	Word( AT( LUMP_LEAFS, dleaf_t, 0, area ), (unsigned int)-1 );
	Word( AT( LUMP_LEAFS, dleaf_t, 1, numLeafSurfaces ), NUM_SURFS );
	Word( AT( LUMP_LEAFS, dleaf_t, 1, numLeafBrushes ), 1 );

	o = Append( LUMP_LEAFSURFACES, NUM_SURFS * 4, 1 );
	for ( i = 0; i < NUM_SURFS; i++ ) {
		Word( o + i * 4, i );
	}
	Append( LUMP_LEAFBRUSHES, 4, 1 );

	Append( LUMP_MODELS, 2 * sizeof( dmodel_t ), 1 );
	for ( j = 0; j < 3; j++ ) {
		Float( AT( LUMP_MODELS, dmodel_t, 0, mins ) + j * 4, -64 );
		Float( AT( LUMP_MODELS, dmodel_t, 0, maxs ) + j * 4, 64 );
		Float( AT( LUMP_MODELS, dmodel_t, 1, mins ) + j * 4, -1 );
		Float( AT( LUMP_MODELS, dmodel_t, 1, maxs ) + j * 4, 1 );
	}
	Word( AT( LUMP_MODELS, dmodel_t, 0, numSurfaces ), NUM_SURFS );
	Word( AT( LUMP_MODELS, dmodel_t, 0, numBrushes ), 1 );
	Word( AT( LUMP_MODELS, dmodel_t, 1, firstBrush ), 1 );
	Word( AT( LUMP_MODELS, dmodel_t, 1, numBrushes ), 1 );

	Append( LUMP_BRUSHES, 2 * sizeof( dbrush_t ), 1 );
	for ( i = 0; i < 2; i++ ) {
		Word( AT( LUMP_BRUSHES, dbrush_t, i, firstSide ), i * 6 );
		Word( AT( LUMP_BRUSHES, dbrush_t, i, numSides ), 6 );
	}
	Append( LUMP_BRUSHSIDES, 12 * sizeof( dbrushside_t ), 1 );
	for ( i = 0; i < 12; i++ ) {
		Word( AT( LUMP_BRUSHSIDES, dbrushside_t, i, planeNum ), i % 6 );
	}

	// planar quad 0-3, triangle 4-6, 3x3 patch 7-15, foliage mesh 16-18 and two origins 19-20
	Append( LUMP_DRAWVERTS, 21 * sizeof( drawVert_t ), 1 );
	Vertex( 0, -1, -1, 0 ); Vertex( 1, 1, -1, 0 ); Vertex( 2, 1, 1, 0 ); Vertex( 3, -1, 1, 0 );
	Vertex( 4, -1, -1, 0 ); Vertex( 5, 1, -1, 0 ); Vertex( 6, 0, 1, 0 );
	for ( i = 0; i < 9; i++ ) {
		Vertex( 7 + i, ( i % 3 ) - 1, ( i / 3 ) - 1, i == 4 ? 0.5f : 0 );
	}
	Vertex( 16, -1, 0, 0 ); Vertex( 17, 1, 0, 0 ); Vertex( 18, 0, 0, 1 );
	Vertex( 19, 8, 8, 0 ); Vertex( 20, -8, 8, 0 );

	o = Append( LUMP_DRAWINDEXES, sizeof( indexes ), 1 );
	for ( i = 0; i < (int)( sizeof( indexes ) / sizeof( indexes[0] ) ); i++ ) {
		Word( o + i * 4, indexes[i] );
	}

	Append( LUMP_FOGS, 2 * sizeof( dfog_t ), 1 );
	strcpy( (char *)source + AT( LUMP_FOGS, dfog_t, 0, shader ), "textures/fog" );
	Word( AT( LUMP_FOGS, dfog_t, 0, brushNum ), 1 );
	Word( AT( LUMP_FOGS, dfog_t, 0, visibleSide ), 5 );
	strcpy( (char *)source + AT( LUMP_FOGS, dfog_t, 1, shader ), "textures/globalfog" );
	Word( AT( LUMP_FOGS, dfog_t, 1, brushNum ), (unsigned int)-1 );
	Word( AT( LUMP_FOGS, dfog_t, 1, visibleSide ), (unsigned int)-1 );

	Append( LUMP_SURFACES, NUM_SURFS * sizeof( dsurface_t ), 1 );
	Surface( S_PLANAR, MST_PLANAR, -1, 0, 4, 0, 6, 0 );
	Surface( S_TRISOUP, MST_TRIANGLE_SOUP, 0, 4, 3, 6, 3, -3 );
	Surface( S_PATCH, MST_PATCH, -1, 7, 9, 0, 0, 0 );
	Word( AT( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth ), 3 );
	Word( AT( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight ), 3 );
	for ( j = 0; j < 3; j++ ) {
		Float( AT( LUMP_SURFACES, dsurface_t, S_PATCH, lightmapVecs ) + j * 4, -1 );
		Float( AT( LUMP_SURFACES, dsurface_t, S_PATCH, lightmapVecs ) + 12 + j * 4, 1 );
	}
	Surface( S_FLARE, MST_FLARE, 1, 0, 0, 0, 0, -3 );
	Float( AT( LUMP_SURFACES, dsurface_t, S_FLARE, lightmapVecs ), 1 );
	Surface( S_FOLIAGE, MST_FOLIAGE, -1, 16, 5, 9, 3, -3 );
	Word( AT( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth ), 2 );     // instances
	Word( AT( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchHeight ), 3 );    // mesh vertices

	o = Append( LUMP_LIGHTMAPS, 2 * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3, 0 );
	memset( source + o, 0x40, 2 * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3 );

	// gridsize 64 64 128 over -64..64 gives 3 x 3 x 1 points
	o = Append( LUMP_LIGHTGRID, 3 * 3 * 1 * 8, 0 );
	memset( source + o, 0x20, 3 * 3 * 1 * 8 );

	o = Append( LUMP_VISIBILITY, 9, 1 );
	Word( o, 1 );
	Word( o + 4, 1 );
	source[o + 8] = 1;
}

/* Replace the entity text; the lump stays where it is when it fits, else moves to the end. */
static void SetEntities( const char *text, int withNul ) {
	unsigned int length = strlen( text ) + ( withNul ? 1 : 0 );
	unsigned int o = LumpOfs( LUMP_ENTITIES );
	if ( length > LumpLen( LUMP_ENTITIES ) ) {
		o = Append( LUMP_ENTITIES, length, 0 );
	}
	memcpy( source + o, text, length );
	Lump( LUMP_ENTITIES, o, length );
}

/* Grow a lump to count records of size bytes, moved to the end of the file. */
static unsigned int GrowLump( int lump, unsigned int size, unsigned int count ) {
	unsigned int oldOfs = LumpOfs( lump ), oldLen = LumpLen( lump ), o;
	o = Append( lump, size * count, 1 );
	memcpy( source + o, source + oldOfs, oldLen < size * count ? oldLen : size * count );
	return o;
}

/* ---- case table and runner ---- */

enum { ACCEPT, REJECT, SKIP };
typedef struct {
	const char *name;
	void ( *mutate )( void );
	int cm, tr;     // expected result for the collision and renderer loader
} bspCase_t;

static int LoaderAccept( const char *name );      // 1 if the loader succeeds; checks results
static void LoaderReject( const char *name );     // the loader drops, with no state change
static int LoaderTry( const char *name );         // 1 accepted, 0 dropped; must not crash
static int whichLoader;                           // 0 collision, 1 renderer

static int RunCase( const bspCase_t *c ) {
	pid_t pid;
	int status, expect = whichLoader ? c->tr : c->cm;

	if ( expect == SKIP ) {
		return 0;
	}
	printf( "case: %s\n", c->name );
	fflush( stdout );
	pid = fork();
	if ( pid == 0 ) {
		BuildMap();
		Check( LoaderAccept( "maps/previous.bsp" ), "the unmutated map loads" );
		c->mutate();
#ifdef HAVE_BSP_VALIDATOR
		{
			dheader_t h;
			const char *e = BSP_ValidateHeader( source, sourceSize, &h );
			if ( !e ) {
				e = BSP_ValidateReferences( source, &h );
			}
			if ( !e ) {
				e = BSP_ValidateTree( source, &h );
			}
			// the validator's own verdict is printed; the loader's is what is checked
			if ( e ) {
				printf( "    shared validator: %s\n", e );
			}
		}
#endif
		if ( expect == ACCEPT ) {
			Check( LoaderAccept( "maps/case.bsp" ), "the loader refused a valid map" );
		} else {
			LoaderReject( "maps/case.bsp" );
			printf( "    dropped: %s\n", lastError );
		}
		fflush( stdout );
		exit( 0 );
	}
	waitpid( pid, &status, 0 );
	if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
		printf( "PASS %s\n", c->name );
		return 0;
	}
	printf( "FAIL %s (%s %d)\n", c->name, WIFSIGNALED( status ) ? "signal" : "exit",
			WIFSIGNALED( status ) ? WTERMSIG( status ) : WEXITSTATUS( status ) );
	return 1;
}

/* ---- mutations ---- */

#define SET( lump, type, n, field, value ) Word( AT( lump, type, n, field ), (unsigned int)( value ) )

static void M_None( void ) {}
static void M_Ident( void ) { Word( 0, 0x12345678 ); }
static void M_Version( void ) { Word( 4, 46 ); }
static void M_ShortHeader( void ) { sourceSize = sizeof( dheader_t ) - 4; }
static void M_LumpPastEnd( void ) { Lump( LUMP_PLANES, sourceSize - 16, 32 ); }
static void M_LumpOffsetPastEnd( void ) { Lump( LUMP_NODES, sourceSize + 64, sizeof( dnode_t ) ); }
static void M_LumpWrap( void ) { Lump( LUMP_LEAFS, 0xfffffff0u, 0x40 ); }
static void M_LumpNegativeLength( void ) { Lump( LUMP_SURFACES, LumpOfs( LUMP_SURFACES ), 0x80000000u ); }
static void M_LumpInHeader( void ) { Lump( LUMP_SHADERS, 8, sizeof( dshader_t ) ); }
static void M_LumpOverlap( void ) { Lump( LUMP_LEAFSURFACES, LumpOfs( LUMP_LEAFBRUSHES ), 4 ); }
static void M_LumpUnaligned( void ) { Lump( LUMP_PLANES, LumpOfs( LUMP_PLANES ) + 2, sizeof( dplane_t ) ); }
static void M_LumpFunnySize( void ) { Lump( LUMP_SHADERS, LumpOfs( LUMP_SHADERS ), sizeof( dshader_t ) + 4 ); }
static void M_FileTruncated( void ) { sourceSize = LumpOfs( LUMP_SURFACES ) + 8; }
static void M_VisShort( void ) { Lump( LUMP_VISIBILITY, LumpOfs( LUMP_VISIBILITY ), 4 ); }
static void M_VisOneByte( void ) { Lump( LUMP_VISIBILITY, LumpOfs( LUMP_VISIBILITY ), 1 ); }
static void M_VisRows( void ) { Word( LumpOfs( LUMP_VISIBILITY ) + 4, 64 ); }
static void M_VisClusters( void ) { Word( LumpOfs( LUMP_VISIBILITY ), 0x10000000 ); }
static void M_VisRowTooNarrow( void ) { Word( LumpOfs( LUMP_VISIBILITY ), 9 ); Word( LumpOfs( LUMP_VISIBILITY ) + 4, 0 ); }
static void M_NoVisCluster( void ) { Lump( LUMP_VISIBILITY, 0, 0 ); SET( LUMP_LEAFS, dleaf_t, 1, cluster, 0x7ffff000 ); }
static void M_NoVisSmallCluster( void ) { Lump( LUMP_VISIBILITY, 0, 0 ); SET( LUMP_LEAFS, dleaf_t, 1, cluster, 1 ); }
static void M_ShaderName( void ) { memset( source + AT( LUMP_SHADERS, dshader_t, 0, shader ), 'a', MAX_QPATH ); }
static void M_SideShader( void ) { SET( LUMP_BRUSHSIDES, dbrushside_t, 3, shaderNum, 2 ); }
static void M_SidePlane( void ) { SET( LUMP_BRUSHSIDES, dbrushside_t, 2, planeNum, 6 ); }
static void M_SidePlaneNegative( void ) { SET( LUMP_BRUSHSIDES, dbrushside_t, 7, planeNum, -100000 ); }
static void M_BrushSides( void ) { SET( LUMP_BRUSHES, dbrush_t, 0, numSides, 5 ); }
static void M_BrushFirstSide( void ) { SET( LUMP_BRUSHES, dbrush_t, 1, firstSide, 7 ); }
static void M_BrushSideWrap( void ) { SET( LUMP_BRUSHES, dbrush_t, 1, firstSide, 0x7fffffff ); }
static void M_BrushShader( void ) { SET( LUMP_BRUSHES, dbrush_t, 0, shaderNum, -1 ); }
static void M_NodePlane( void ) { SET( LUMP_NODES, dnode_t, 0, planeNum, 6 ); }
static void M_NodeChild( void ) { Word( AT( LUMP_NODES, dnode_t, 0, children ), 1 ); }
static void M_NodeLeaf( void ) { Word( AT( LUMP_NODES, dnode_t, 0, children ) + 4, (unsigned int)-3 ); }
static void M_NodeCycle( void ) { Word( AT( LUMP_NODES, dnode_t, 0, children ), 0 ); }
static void M_NodeShared( void ) {
	unsigned int o = GrowLump( LUMP_NODES, sizeof( dnode_t ), 3 );
	(void)o;
	Word( AT( LUMP_NODES, dnode_t, 0, children ), 1 );
	Word( AT( LUMP_NODES, dnode_t, 0, children ) + 4, 2 );
	SET( LUMP_NODES, dnode_t, 1, planeNum, 1 );
	Word( AT( LUMP_NODES, dnode_t, 1, children ), 2 );
	Word( AT( LUMP_NODES, dnode_t, 1, children ) + 4, (unsigned int)-1 );
	SET( LUMP_NODES, dnode_t, 2, planeNum, 1 );
	Word( AT( LUMP_NODES, dnode_t, 2, children ), (unsigned int)-1 );
	Word( AT( LUMP_NODES, dnode_t, 2, children ) + 4, (unsigned int)-2 );
}
static void M_LeafSurface( void ) { Word( LumpOfs( LUMP_LEAFSURFACES ) + 4, NUM_SURFS ); }
static void M_LeafSurfaceNegative( void ) { Word( LumpOfs( LUMP_LEAFSURFACES ), (unsigned int)-1 ); }
static void M_LeafBrush( void ) { Word( LumpOfs( LUMP_LEAFBRUSHES ), 2 ); }
static void M_LeafSurfaceSpan( void ) { SET( LUMP_LEAFS, dleaf_t, 1, firstLeafSurface, 1 ); }
static void M_LeafBrushSpan( void ) { SET( LUMP_LEAFS, dleaf_t, 1, firstLeafBrush, -1 ); }
static void M_LeafSpanWrap( void ) { SET( LUMP_LEAFS, dleaf_t, 1, firstLeafSurface, 2 ); SET( LUMP_LEAFS, dleaf_t, 1, numLeafSurfaces, -1 ); }
static void M_LeafCluster( void ) { SET( LUMP_LEAFS, dleaf_t, 1, cluster, 1 ); }
static void M_LeafArea( void ) { SET( LUMP_LEAFS, dleaf_t, 1, area, MAX_MAP_AREA_BYTES * 8 ); }
static void M_ModelSurfaces( void ) { SET( LUMP_MODELS, dmodel_t, 1, firstSurface, 3 ); SET( LUMP_MODELS, dmodel_t, 1, numSurfaces, 3 ); }
static void M_ModelBrushes( void ) { SET( LUMP_MODELS, dmodel_t, 1, numBrushes, 2 ); }
static void M_ModelBrushWrap( void ) { SET( LUMP_MODELS, dmodel_t, 1, firstBrush, 0x7fffffff ); SET( LUMP_MODELS, dmodel_t, 1, numBrushes, 0x7fffffff ); }
static void M_ModelBounds( void ) { Float( AT( LUMP_MODELS, dmodel_t, 0, mins ), 100 ); }
static void M_TooManyModels( void ) {
	unsigned int i;
	GrowLump( LUMP_MODELS, sizeof( dmodel_t ), MAX_SUBMODELS + 1 );
	for ( i = 2; i < MAX_SUBMODELS + 1; i++ ) {
		memcpy( source + LumpOfs( LUMP_MODELS ) + i * sizeof( dmodel_t ), source + LumpOfs( LUMP_MODELS ) + sizeof( dmodel_t ), sizeof( dmodel_t ) );
	}
}
static void M_NoModels( void ) { Lump( LUMP_MODELS, 0, 0 ); }
static void M_NoPlanes( void ) { Lump( LUMP_PLANES, 0, 0 ); }
static void M_FogBrush( void ) { SET( LUMP_FOGS, dfog_t, 0, brushNum, 2 ); }
static void M_FogBrushNegative( void ) { SET( LUMP_FOGS, dfog_t, 0, brushNum, -2 ); }
static void M_FogVisibleSide( void ) { SET( LUMP_FOGS, dfog_t, 0, visibleSide, 6 ); }
static void M_FogVisibleSideFar( void ) { SET( LUMP_FOGS, dfog_t, 0, visibleSide, 12 ); }
static void M_FogGlobalSide( void ) { SET( LUMP_FOGS, dfog_t, 1, visibleSide, 7 ); }
static void M_FogName( void ) { memset( source + AT( LUMP_FOGS, dfog_t, 1, shader ), 'f', MAX_QPATH ); }
static void M_FogFewSides( void ) {
	// fewer than six sides in the lump: the old check's sidesCount - 6 underflowed
	Lump( LUMP_BRUSHSIDES, LumpOfs( LUMP_BRUSHSIDES ), 4 * sizeof( dbrushside_t ) );
	SET( LUMP_BRUSHES, dbrush_t, 0, numSides, 4 );
	SET( LUMP_BRUSHES, dbrush_t, 1, firstSide, 0 );
	SET( LUMP_BRUSHES, dbrush_t, 1, numSides, 4 );
}
static void M_SurfShader( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, shaderNum, 2 ); }
static void M_SurfType( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, surfaceType, MST_FOLIAGE + 1 ); }
static void M_SurfTypeBad( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, surfaceType, MST_BAD ); }
static void M_SurfFog( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, fogNum, 2 ); }
static void M_SurfFogNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, fogNum, -2 ); }
static void M_PatchFog( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, fogNum, 1000 ); }
static void M_FoliageFog( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, fogNum, 2 ); }
static void M_FlareFogUnused( void ) { Lump( LUMP_FOGS, 0, 0 ); SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, fogNum, -1 ); SET( LUMP_SURFACES, dsurface_t, S_FLARE, fogNum, 0 ); }
static void M_FlareFogLarge( void ) { SET( LUMP_SURFACES, dsurface_t, S_FLARE, fogNum, 100000 ); }
static void M_VertSpan( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, firstVert, 18 ); }
static void M_VertWrap( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, firstVert, 0x7fffffff ); SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, numVerts, 0x7fffffff ); }
static void M_VertNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, firstVert, -4 ); }
static void M_IndexSpan( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, firstIndex, 10 ); }
static void M_IndexCount( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, numIndexes, 0x40000000 ); }
static void M_IndexNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, numIndexes, -6 ); }
static void M_LocalIndex( void ) { Word( LumpOfs( LUMP_DRAWINDEXES ) + 6 * 4 + 4, 3 ); }
static void M_LocalIndexNegative( void ) { Word( LumpOfs( LUMP_DRAWINDEXES ) + 4, (unsigned int)-1 ); }
static void M_PlanarNoVerts( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, numVerts, 0 ); SET( LUMP_SURFACES, dsurface_t, S_PLANAR, numIndexes, 0 ); }
static void M_TriCountOverflow( void ) { SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, numVerts, 0x3fffffff ); SET( LUMP_SURFACES, dsurface_t, S_TRISOUP, numIndexes, 0x40000000 ); }
static void M_FoliageInstances( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth, 3 ); }
static void M_FoliageInstanceWrap( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth, -2 ); }
static void M_FoliageMesh( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchHeight, 6 ); }
static void M_FoliageMeshEmpty( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchHeight, 0 ); SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, numIndexes, 0 ); }
static void M_FoliageIndex( void ) { Word( LumpOfs( LUMP_DRAWINDEXES ) + 9 * 4 + 8, 3 ); }
static void M_FoliageCountOverflow( void ) { SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, numIndexes, 0x40000000 ); SET( LUMP_SURFACES, dsurface_t, S_FOLIAGE, patchWidth, 0x10000000 ); }
static void M_LightmapNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, lightmapNum, -5 ); }
static void M_LightmapPatchNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, lightmapNum, 0x80000000u ); }
static void M_LightmapLarge( void ) { SET( LUMP_SURFACES, dsurface_t, S_PLANAR, lightmapNum, 100000 ); }
static void M_PatchEven( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 4 ); SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 2 ); }
static void M_PatchSmall( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 1 ); }
static void M_PatchNegative( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, -1 ); SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, -9 ); }
static void M_PatchProductWrap( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 65537 ); SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 65537 ); }
static void M_PatchVerts( void ) { SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 5 ); }
static unsigned int PatchVerts( int count ) {
	// move the patch onto count fresh vertices at the end of the file
	unsigned int o = GrowLump( LUMP_DRAWVERTS, sizeof( drawVert_t ), 21 + count ), i;
	for ( i = 0; i < (unsigned int)count; i++ ) {
		Float( o + ( 21 + i ) * sizeof( drawVert_t ), (float)( i % 67 ) );
		Float( o + ( 21 + i ) * sizeof( drawVert_t ) + 4, (float)( i / 67 ) );
		Float( o + ( 21 + i ) * sizeof( drawVert_t ) + offsetof( drawVert_t, normal ) + 8, 1 );
	}
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, firstVert, 21 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, numVerts, count );
	return o;
}
static void M_PatchControls( void ) {
	// 33 x 33 = 1089 controls: past both loaders' 1024-point control arrays
	PatchVerts( 33 * 33 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 33 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 33 );
}
static void M_PatchWide( void ) {
	// 67 x 3: within collision's 129 grid, past the renderer's 65-wide ctrl grid
	PatchVerts( 67 * 3 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 67 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 3 );
}
static void M_PatchWideNodraw( void ) { M_PatchWide(); SET( LUMP_SURFACES, dsurface_t, S_PATCH, shaderNum, 1 ); }
static void M_PatchTall( void ) {
	// 3 x 131: past collision's 129 grid as well
	PatchVerts( 3 * 131 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchWidth, 3 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, patchHeight, 131 );
	SET( LUMP_SURFACES, dsurface_t, S_PATCH, shaderNum, 1 );
}
static void M_NanPlane( void ) { Word( AT( LUMP_PLANES, dplane_t, 2, dist ), 0x7fc00000 ); }
static void M_NanVertex( void ) { Word( LumpOfs( LUMP_DRAWVERTS ) + 8 * sizeof( drawVert_t ), 0x7f800000 ); }
static void M_NanLightmapCoord( void ) { Word( LumpOfs( LUMP_DRAWVERTS ) + offsetof( drawVert_t, lightmap ), 0x7fc00000 ); }
static void M_EntityNoNul( void ) {
	// drop the NUL and fill the gap up to the shader names, so a string copy runs on
	unsigned int k, end = LumpOfs( LUMP_ENTITIES ) + strlen( worldspawn );
	Lump( LUMP_ENTITIES, LumpOfs( LUMP_ENTITIES ), strlen( worldspawn ) );
	for ( k = end; k < LumpOfs( LUMP_SHADERS ); k++ ) {
		source[k] = ' ';
	}
}
static void M_EntityEmpty( void ) { Lump( LUMP_ENTITIES, 0, 0 ); }
static void M_EntityQuote( void ) { SetEntities( "{\n\"gridsize\" \"64 64 128", 0 ); }
static void M_GridZero( void ) { SetEntities( "{ \"gridsize\" \"0 64 128\" }", 1 ); }
static void M_GridNegative( void ) { SetEntities( "{ \"gridsize\" \"64 -64 128\" }", 1 ); }
static void M_GridNan( void ) { SetEntities( "{ \"gridsize\" \"nan 64 128\" }", 1 ); }
static void M_GridInf( void ) { SetEntities( "{ \"gridsize\" \"64 64 inf\" }", 1 ); }
static void M_GridTiny( void ) { SetEntities( "{ \"gridsize\" \"1e-40 64 128\" }", 1 ); }
static void M_GridPoints( void ) { SetEntities( "{ \"gridsize\" \"0.001 0.001 0.001\" }", 1 ); }
static void M_GridPartial( void ) { SetEntities( "{ \"gridsize\" \"64\" }", 1 ); }
static void M_GridMismatch( void ) { SetEntities( "{ \"gridsize\" \"32 32 32\" }", 1 ); }
static void M_GridEmptyBounds( void ) {
	// a flat world: z gives 0 grid rows, so no grid at all instead of a zero-byte one
	int j;
	for ( j = 0; j < 3; j++ ) {
		Float( AT( LUMP_MODELS, dmodel_t, 0, mins ) + j * 4, j == 2 ? 10 : -64 );
		Float( AT( LUMP_MODELS, dmodel_t, 0, maxs ) + j * 4, j == 2 ? 20 : 64 );
	}
	Lump( LUMP_LIGHTGRID, 0, 0 );
}
static void M_LightmapSingle( void ) {
	// one lightmap: the old hack uploaded a second one from the bytes past the lump
	Lump( LUMP_LIGHTMAPS, LumpOfs( LUMP_LIGHTMAPS ) + LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3, LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3 );
}
static void M_LightmapPartial( void ) { Lump( LUMP_LIGHTMAPS, LumpOfs( LUMP_LIGHTMAPS ), LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3 - 3 ); }
static void M_LightmapTooMany( void ) {
	unsigned int size = LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
	Append( LUMP_LIGHTMAPS, size * 257, 0 );
}

static const bspCase_t bspCases[] = {
	{ "valid minimal map", M_None, ACCEPT, ACCEPT },
	{ "bad ident", M_Ident, REJECT, REJECT },
	{ "bad version", M_Version, REJECT, REJECT },
	{ "file shorter than the header", M_ShortHeader, REJECT, REJECT },
	{ "lump length past the end", M_LumpPastEnd, REJECT, REJECT },
	{ "lump offset past the end", M_LumpOffsetPastEnd, REJECT, REJECT },
	{ "lump offset+length wraps", M_LumpWrap, REJECT, REJECT },
	{ "negative lump length", M_LumpNegativeLength, REJECT, REJECT },
	{ "lump inside the header", M_LumpInHeader, REJECT, REJECT },
	{ "overlapping lumps", M_LumpOverlap, REJECT, REJECT },
	{ "unaligned word lump", M_LumpUnaligned, REJECT, REJECT },
	{ "lump not a whole number of records", M_LumpFunnySize, REJECT, REJECT },
	{ "file truncated inside the surfaces", M_FileTruncated, REJECT, REJECT },
	{ "visibility lump of 4 bytes", M_VisShort, REJECT, REJECT },
	{ "visibility lump of 1 byte", M_VisOneByte, REJECT, REJECT },
	{ "visibility rows past the lump", M_VisRows, REJECT, REJECT },
	{ "visibility clusters past the lump", M_VisClusters, REJECT, REJECT },
	{ "visibility row narrower than the clusters", M_VisRowTooNarrow, REJECT, REJECT },
	{ "huge cluster without visibility", M_NoVisCluster, REJECT, REJECT },
	{ "cluster past the leaves without visibility", M_NoVisSmallCluster, ACCEPT, ACCEPT },
	{ "unterminated shader name", M_ShaderName, REJECT, REJECT },
	{ "brush side shader out of range", M_SideShader, REJECT, REJECT },
	{ "brush side plane out of range", M_SidePlane, REJECT, REJECT },
	{ "brush side plane negative", M_SidePlaneNegative, REJECT, REJECT },
	{ "brush with five sides", M_BrushSides, REJECT, REJECT },
	{ "brush sides past the lump", M_BrushFirstSide, REJECT, REJECT },
	{ "brush first side wraps", M_BrushSideWrap, REJECT, REJECT },
	{ "brush shader negative", M_BrushShader, REJECT, REJECT },
	{ "node plane out of range", M_NodePlane, REJECT, REJECT },
	{ "node child out of range", M_NodeChild, REJECT, REJECT },
	{ "node leaf out of range", M_NodeLeaf, REJECT, REJECT },
	{ "node is its own child", M_NodeCycle, REJECT, REJECT },
	{ "decision node with two parents", M_NodeShared, REJECT, REJECT },
	{ "leaf surface index out of range", M_LeafSurface, REJECT, REJECT },
	{ "leaf surface index negative", M_LeafSurfaceNegative, REJECT, REJECT },
	{ "leaf brush index out of range", M_LeafBrush, REJECT, REJECT },
	{ "leaf surface span past the lump", M_LeafSurfaceSpan, REJECT, REJECT },
	{ "leaf brush span negative", M_LeafBrushSpan, REJECT, REJECT },
	{ "leaf surface span wraps", M_LeafSpanWrap, REJECT, REJECT },
	{ "leaf cluster past the visibility", M_LeafCluster, REJECT, REJECT },
	{ "leaf area past MAX_MAP_AREA_BYTES", M_LeafArea, REJECT, REJECT },
	{ "model surfaces past the lump", M_ModelSurfaces, REJECT, REJECT },
	{ "model brushes past the lump", M_ModelBrushes, REJECT, REJECT },
	{ "model brush span wraps", M_ModelBrushWrap, REJECT, REJECT },
	{ "reversed model bounds", M_ModelBounds, REJECT, REJECT },
	{ "more than MAX_SUBMODELS models", M_TooManyModels, REJECT, ACCEPT },
	{ "no models", M_NoModels, REJECT, REJECT },
	{ "no planes", M_NoPlanes, REJECT, REJECT },
	{ "fog brush out of range", M_FogBrush, REJECT, REJECT },
	{ "fog brush -2", M_FogBrushNegative, REJECT, REJECT },
	{ "fog visible side past its brush", M_FogVisibleSide, REJECT, REJECT },
	{ "fog visible side past the sides, ignored", M_FogVisibleSideFar, ACCEPT, ACCEPT },
	{ "global fog visible side past the sides", M_FogGlobalSide, REJECT, REJECT },
	{ "unterminated fog name", M_FogName, REJECT, REJECT },
	{ "fog brush with fewer than six sides", M_FogFewSides, REJECT, REJECT },
	{ "surface shader out of range", M_SurfShader, REJECT, REJECT },
	{ "surface type past MST_FOLIAGE", M_SurfType, REJECT, REJECT },
	{ "surface type MST_BAD", M_SurfTypeBad, REJECT, REJECT },
	{ "planar fogNum out of range", M_SurfFog, REJECT, REJECT },
	{ "triangle fogNum -2", M_SurfFogNegative, REJECT, REJECT },
	{ "patch fogNum out of range", M_PatchFog, REJECT, REJECT },
	{ "foliage fogNum out of range", M_FoliageFog, REJECT, REJECT },
	{ "flare fogNum 0 with no fogs", M_FlareFogUnused, ACCEPT, ACCEPT },
	{ "flare fogNum far out of range", M_FlareFogLarge, ACCEPT, ACCEPT },
	{ "surface vertices past the lump", M_VertSpan, REJECT, REJECT },
	{ "surface vertex span wraps", M_VertWrap, REJECT, REJECT },
	{ "surface first vertex negative", M_VertNegative, REJECT, REJECT },
	{ "surface indexes past the lump", M_IndexSpan, REJECT, REJECT },
	{ "surface index count huge", M_IndexCount, REJECT, REJECT },
	{ "surface index count negative", M_IndexNegative, REJECT, REJECT },
	{ "triangle index past its vertices", M_LocalIndex, REJECT, REJECT },
	{ "planar index negative", M_LocalIndexNegative, REJECT, REJECT },
	{ "planar surface with no vertices", M_PlanarNoVerts, REJECT, REJECT },
	{ "triangle size arithmetic overflows", M_TriCountOverflow, REJECT, REJECT },
	{ "foliage instances past the vertices", M_FoliageInstances, REJECT, REJECT },
	{ "foliage instance count negative", M_FoliageInstanceWrap, REJECT, REJECT },
	{ "foliage mesh past the vertices", M_FoliageMesh, REJECT, REJECT },
	{ "foliage with no mesh vertices", M_FoliageMeshEmpty, REJECT, REJECT },
	{ "foliage index past its mesh", M_FoliageIndex, REJECT, REJECT },
	{ "foliage size arithmetic overflows", M_FoliageCountOverflow, REJECT, REJECT },
	{ "negative lightmap below LIGHTMAP_2D", M_LightmapNegative, REJECT, REJECT },
	{ "patch lightmap INT_MIN", M_LightmapPatchNegative, REJECT, REJECT },
	{ "lightmap past MAX_LIGHTMAPS, vertex lit", M_LightmapLarge, ACCEPT, ACCEPT },
	{ "even patch size", M_PatchEven, REJECT, REJECT },
	{ "patch height 1", M_PatchSmall, REJECT, REJECT },
	{ "negative patch size", M_PatchNegative, REJECT, REJECT },
	{ "patch size product wraps", M_PatchProductWrap, REJECT, REJECT },
	{ "patch controls past its vertices", M_PatchVerts, REJECT, REJECT },
	{ "33x33 patch past 1024 controls", M_PatchControls, REJECT, REJECT },
	{ "67x3 drawn patch past the renderer grid", M_PatchWide, ACCEPT, REJECT },
	{ "67x3 nodraw patch", M_PatchWideNodraw, ACCEPT, ACCEPT },
	{ "3x131 nodraw patch past the collision grid", M_PatchTall, REJECT, ACCEPT },
	{ "NaN plane", M_NanPlane, REJECT, REJECT },
	{ "infinite vertex", M_NanVertex, REJECT, REJECT },
	{ "NaN lightmap coordinate, as in retail maps", M_NanLightmapCoord, ACCEPT, ACCEPT },
	{ "entity lump without a NUL", M_EntityNoNul, ACCEPT, ACCEPT },
	{ "empty entity lump", M_EntityEmpty, ACCEPT, ACCEPT },
	{ "unterminated quote in worldspawn", M_EntityQuote, ACCEPT, ACCEPT },
	{ "gridsize 0", M_GridZero, SKIP, REJECT },
	{ "gridsize negative", M_GridNegative, SKIP, REJECT },
	{ "gridsize nan", M_GridNan, SKIP, REJECT },
	{ "gridsize inf", M_GridInf, SKIP, REJECT },
	{ "gridsize denormal", M_GridTiny, SKIP, REJECT },
	{ "gridsize point count overflows", M_GridPoints, SKIP, REJECT },
	{ "gridsize with one value", M_GridPartial, SKIP, ACCEPT },
	{ "gridsize not matching the lump", M_GridMismatch, SKIP, ACCEPT },
	{ "grid with no points and an empty lump", M_GridEmptyBounds, SKIP, ACCEPT },
	{ "single lightmap, uploaded twice", M_LightmapSingle, ACCEPT, ACCEPT },
	{ "lightmap lump not whole lightmaps", M_LightmapPartial, REJECT, REJECT },
	{ "257 lightmaps", M_LightmapTooMany, ACCEPT, REJECT },
};

static int RunCases( void ) {
	unsigned int i;
	int failed = 0, run = 0;
	for ( i = 0; i < sizeof( bspCases ) / sizeof( bspCases[0] ); i++ ) {
		if ( ( whichLoader ? bspCases[i].tr : bspCases[i].cm ) == SKIP ) {
			continue;
		}
		run++;
		failed += RunCase( &bspCases[i] );
	}
	printf( "%d of %d cases passed\n", run - failed, run );
	return failed;
}

/* ---- real maps: load, truncate and mutate ---- */

static unsigned int rng = 12345;
static unsigned int Random( void ) {
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static int LoadFile( const char *path ) {
	FILE *f = fopen( path, "rb" );
	long n;
	if ( !f ) {
		return 0;
	}
	if ( !source ) {
		source = malloc( SOURCE_CAPACITY );
	}
	fseek( f, 0, SEEK_END );
	n = ftell( f );
	rewind( f );
	if ( n > SOURCE_CAPACITY || fread( source, 1, n, f ) != (size_t)n ) {
		fclose( f );
		return 0;
	}
	fclose( f );
	sourceSize = n;
	return 1;
}

/* The word a mutation overwrites: a header field, or an index-bearing record field. */
static unsigned int MutationOffset( void ) {
	static const int lumps[] = { LUMP_NODES, LUMP_LEAFS, LUMP_LEAFSURFACES, LUMP_LEAFBRUSHES, LUMP_MODELS,
								 LUMP_BRUSHES, LUMP_BRUSHSIDES, LUMP_DRAWINDEXES, LUMP_FOGS, LUMP_SURFACES, LUMP_VISIBILITY };
	int lump;
	unsigned int len;
	if ( Random() % 8 == 0 ) {
		return 8 + ( Random() % ( HEADER_LUMPS * 2 ) ) * 4;
	}
	lump = lumps[Random() % ( sizeof( lumps ) / sizeof( lumps[0] ) )];
	len = LumpLen( lump );
	if ( len < 4 ) {
		return 4;
	}
	return LumpOfs( lump ) + ( Random() % ( len / 4 ) ) * 4;
}
static unsigned int MutationValue( unsigned int old ) {
	switch ( Random() % 8 ) {
	case 0: return 0;
	case 1: return 0xffffffffu;
	case 2: return old + 1;
	case 3: return old - 1;
	case 4: return 0x7fffffffu;
	case 5: return 0x80000000u;
	case 6: return old * 2 + 1;
	default: return Random();
	}
}

static int RunRealMaps( int argc, char **argv, int mutations ) {
	int i, k, accepted, rejected, failed = 0;
	byte *original = malloc( SOURCE_CAPACITY );
	int originalSize;
	pid_t pid;
	int status;

	zSlack = sizeof( float );
	realMaps = 1;
	for ( i = 1; i < argc; i++ ) {
		if ( !LoadFile( argv[i] ) ) {
			printf( "FAIL %s: unreadable\n", argv[i] );
			failed++;
			continue;
		}
#ifdef HAVE_BSP_VALIDATOR
		{
			dheader_t h;
			const char *e = BSP_ValidateHeader( source, sourceSize, &h );
			if ( !e ) {
				e = BSP_ValidateReferences( source, &h );
			}
			if ( !e ) {
				e = BSP_ValidateTree( source, &h );
			}
			printf( "%s shared validator %s: %s\n", e ? "FAIL" : "PASS", argv[i], e ? e : "accepted" );
			failed += e != NULL;
		}
#endif
		expectDrop = 1;
		if ( !LoaderTry( argv[i] ) ) {
			printf( "FAIL %s loader: %s\n", argv[i], lastError );
			failed++;
			continue;
		}
		Check( LoaderAccept( argv[i] ), "retail map result checks" );
		printf( "PASS %s loader\n", argv[i] );
		if ( !mutations ) {
			continue;
		}
		memcpy( original, source, sourceSize );
		originalSize = sourceSize;
		fflush( stdout );
		// each map's sweep runs in a child, so a sanitizer abort is counted, not fatal
		pid = fork();
		if ( pid == 0 ) {
			accepted = rejected = 0;
			for ( k = 0; k < 16; k++ ) {
				sourceSize = k == 0 ? (int)sizeof( dheader_t ) - 1 : (int)( (unsigned long long)originalSize * k / 16 );
				if ( LoaderTry( argv[i] ) ) {
					accepted++;
				} else {
					rejected++;
				}
			}
			sourceSize = originalSize;
			for ( k = 0; k < mutations; k++ ) {
				unsigned int o = MutationOffset();
				memcpy( source, original, originalSize );
				Word( o, MutationValue( Read( o ) ) );
				if ( LoaderTry( argv[i] ) ) {
					accepted++;
				} else {
					rejected++;
				}
			}
			printf( "     %s: 16 truncations and %d mutations: %d accepted, %d refused, no fault\n",
					argv[i], mutations, accepted, rejected );
			fflush( stdout );
			exit( 0 );
		}
		waitpid( pid, &status, 0 );
		if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ) {
			printf( "PASS %s truncation and mutation\n", argv[i] );
		} else {
			printf( "FAIL %s truncation and mutation\n", argv[i] );
			failed++;
		}
	}
	free( original );
	return failed;
}
