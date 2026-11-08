/*
 * Host test of the real model loaders in src/renderer/tr_model.c and of the
 * code that trusts what they load: tr_animation_mds.c, tr_animation_mdm.c,
 * tr_mesh.c, tr_cmesh.c and tr_surface.c. Only renderer imports and GL entry
 * points are stubbed; models reach the loaders through RE_RegisterModel and
 * ri.FS_ReadFile, in exact-size allocations so ASan sees any overrun.
 *
 * Build with -DMODEL_TEST_SWAP to make LittleLong, LittleShort and LittleFloat
 * byte-swap, as they do on the big-endian target, so the loaders' swap loops
 * run on the host. The synthetic models are then written big-endian.
 *
 * usage: test_models                    every case, then the mutation fuzz
 *        test_models list               the case names
 *        test_models case NAME          one case
 *        test_models fuzz N FILE...     mutate each model file N times
 *        test_models retail ROOT PATH...  load models from ROOT and print what loaded
 */
#include "../../src/game/q_shared.c"   // included for the swap pointers it keeps static
#include "../../src/renderer/tr_local.h"
#include <setjmp.h>
#include <stddef.h>

#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
#ifndef ARRAY_LEN
#define ARRAY_LEN( x ) ( sizeof( x ) / sizeof( *( x ) ) )
#endif

void RB_SurfaceMesh( md3Surface_t *surface );
void RB_SurfaceCMesh( mdcSurface_t *surface );

/* ------------------------------------------------------------------ stubs */

trGlobals_t tr;
shaderCommands_t tess;
backEndState_t backEnd;
glconfig_t glConfig;
refimport_t ri;
vec4hack_t tess_xyz[SHADER_MAX_VERTEXES];
vec4hack_t tess_normal[SHADER_MAX_VERTEXES];
vec2hack_t tess_texCoords0[SHADER_MAX_VERTEXES];
vec2hack_t tess_texCoords1[SHADER_MAX_VERTEXES];
glIndex_t tess_indexes[SHADER_MAX_INDEXES];
color4ubhack_t tess_vertexColors[SHADER_MAX_VERTEXES];

static cvar_t cvars[16];
cvar_t *r_cache = &cvars[0], *r_cacheGathering = &cvars[1], *r_cacheModels = &cvars[2];
cvar_t *r_bonesDebug = &cvars[3], *r_lodbias = &cvars[4], *r_lodscale = &cvars[5], *r_shadows = &cvars[6];
cvar_t *r_lodCurveError = &cvars[7], *r_railSegmentLength = &cvars[8], *r_railWidth = &cvars[9];

void ( APIENTRY * qglBegin )( GLenum mode );
void ( APIENTRY * qglBlendFunc )( GLenum sfactor, GLenum dfactor );
void ( APIENTRY * qglCallList )( GLuint list );
void ( APIENTRY * qglColor3f )( GLfloat red, GLfloat green, GLfloat blue );
void ( APIENTRY * qglColor3fv )( const GLfloat *v );
void ( APIENTRY * qglColor4f )( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha );
void ( APIENTRY * qglDepthRange )( GLclampd zNear, GLclampd zFar );
void ( APIENTRY * qglDisable )( GLenum cap );
void ( APIENTRY * qglEnable )( GLenum cap );
void ( APIENTRY * qglEnd )( void );
void ( APIENTRY * qglLineWidth )( GLfloat width );
void ( APIENTRY * qglPointSize )( GLfloat size );
void ( APIENTRY * qglVertex3f )( GLfloat x, GLfloat y, GLfloat z );
void ( APIENTRY * qglVertex3fv )( const GLfloat *v );

static jmp_buf errorJump;
static int errorArmed, errorCount, verbose;
static char lastMessage[256];

void QDECL Com_Error( int level, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( lastMessage, sizeof( lastMessage ), fmt, ap );
	va_end( ap );
	errorCount++;
	if ( errorArmed ) {
		longjmp( errorJump, 1 );
	}
	fprintf( stderr, "unexpected error: %s\n", lastMessage );
	abort();
}

void QDECL Com_Printf( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( lastMessage, sizeof( lastMessage ), fmt, ap );
	va_end( ap );
	if ( verbose ) {
		fputs( lastMessage, stderr );
	}
}

static void QDECL StubPrintf( int level, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( lastMessage, sizeof( lastMessage ), fmt, ap );
	va_end( ap );
	if ( verbose ) {
		fputs( lastMessage, stderr );
	}
}

static void QDECL StubError( int level, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( lastMessage, sizeof( lastMessage ), fmt, ap );
	va_end( ap );
	Com_Error( level, "%s", lastMessage );
}

int Com_HashKey( char *string, int maxlen ) {
	int hash = 0, i;
	for ( i = 0; i < maxlen && string[i]; i++ ) {
		hash += string[i] * ( 119 + i );
	}
	return hash;
}

/* every hunk allocation is exact-size and freed between models */
static void *allocs[65536];
static int numAllocs;

static void *StubHunkAlloc( int size, ha_pref pref ) {
	void *p;
	if ( size < 0 || numAllocs == (int)ARRAY_LEN( allocs ) ) {
		Com_Error( ERR_FATAL, "bad hunk allocation %d", size );
	}
	p = calloc( 1, size ? size : 1 );
	allocs[numAllocs++] = p;
	return p;
}

static void *StubTempAlloc( int size ) {
	return malloc( size ? size : 1 );
}

static void StubTempFree( void *p ) {
	free( p );
}

/* files: either virtual buffers, or (retail mode) files under a root directory */
typedef struct {
	char name[MAX_QPATH];
	byte *data;
	int length;
} vfile_t;
static vfile_t files[8];
static int numFiles;
static const char *diskRoot, *diskExtension;

static void ClearFiles( void ) {
	int i;
	for ( i = 0; i < numFiles; i++ ) {
		free( files[i].data );
	}
	numFiles = 0;
}

static void AddFile( const char *name, const byte *data, int length ) {
	vfile_t *f = &files[numFiles++];
	Q_strncpyz( f->name, name, sizeof( f->name ) );
	f->data = malloc( length ? length : 1 );
	memcpy( f->data, data, length );
	f->length = length;
}

static byte *ReadDisk( const char *name, int *length ) {
	char path[MAX_OSPATH];
	const char *ext = strrchr( name, '.' );
	FILE *f;
	byte *data;
	long n;

	// only the requested format, so an .md3 is not hidden by an .mdc beside it
	if ( !ext || Q_stricmp( ext, diskExtension ) ) {
		return NULL;
	}
	Com_sprintf( path, sizeof( path ), "%s/%s", diskRoot, name );
	if ( !( f = fopen( path, "rb" ) ) ) {
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	n = ftell( f );
	fseek( f, 0, SEEK_SET );
	data = malloc( n + 1 );
	if ( fread( data, 1, n, f ) != (size_t)n ) {
		Com_Error( ERR_FATAL, "short read %s", path );
	}
	fclose( f );
	*length = (int)n;
	return data;
}

static int StubReadFile( const char *name, void **buffer ) {
	int i, length;
	byte *data = NULL;

	if ( diskRoot ) {
		data = ReadDisk( name, &length );
	} else {
		for ( i = 0; i < numFiles; i++ ) {
			if ( !strcmp( files[i].name, name ) ) {
				length = files[i].length;
				data = malloc( length + 1 );
				memcpy( data, files[i].data, length );
				break;
			}
		}
	}
	if ( !data ) {
		if ( buffer ) {
			*buffer = NULL;
		}
		return -1;
	}
	data[length] = 0;   // FS_ReadFile terminates the buffer, as here
	if ( !buffer ) {
		free( data );
	} else {
		*buffer = data;
	}
	return length;
}

static void StubFreeFile( void *buffer ) {
	free( buffer );
}

static void StubExecuteText( int when, const char *text ) {
}

static shader_t shaders[64];

shader_t *R_FindShader( const char *name, int lightmapIndex, qboolean mipRawImage ) {
	// the name must be terminated inside the model: ASan checks this strlen
	return &shaders[1 + (unsigned int)Com_HashKey( (char *)name, (int)strlen( name ) + 1 ) % 63];
}

shader_t *R_GetShaderByHandle( qhandle_t h ) {
	return &shaders[0];
}

skin_t *R_GetSkinByHandle( qhandle_t h ) {
	return NULL;
}

static void *drawSurfs[256];
static int numDrawSurfs;

void R_AddDrawSurf( surfaceType_t *surface, shader_t *shader, int fogNum, int frontFace, int dlightMap ) {
	if ( numDrawSurfs < (int)ARRAY_LEN( drawSurfs ) ) {
		drawSurfs[numDrawSurfs++] = surface;
	}
}

int R_CullLocalBox( vec3_t bounds[2] ) {
	return CULL_CLIP;
}

int R_CullLocalPointAndRadius( vec3_t origin, float radius ) {
	return CULL_CLIP;
}

void R_DebugText( const vec3_t org, float r, float g, float b, const char *text, qboolean neverOcclude ) {
}
void R_SetupEntityLighting( const trRefdef_t *refdef, trRefEntity_t *ent ) {
}
void GL_Bind( image_t *image ) {
}
void GL_State( unsigned long stateVector ) {
}
void R_SyncRenderThread( void ) {
}
void R_Init( void ) {
}
void R_ClearFlares( void ) {
}
void RE_ClearScene( void ) {
}
void RE_StretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader ) {
}

void RB_BeginSurface( shader_t *shader, int fogNum ) {
	tess.shader = shader;
	tess.numIndexes = tess.numVertexes = 0;
}

void RB_EndSurface( void ) {
	tess.numIndexes = tess.numVertexes = 0;
}

/* ---------------------------------------------------------------- harness */

static int swapMode;
static int failures;

#define CHECK( cond, ... ) do { if ( !( cond ) ) { failures++; printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } while ( 0 )

static void ResetRenderer( void ) {
	int i;

	for ( i = 0; i < numAllocs; i++ ) {
		free( allocs[i] );
	}
	numAllocs = 0;
	ClearFiles();
	memset( &tr, 0, sizeof( tr ) );
	for ( i = 0; i < (int)ARRAY_LEN( shaders ); i++ ) {
		shaders[i].index = i;
		tr.shaders[i] = &shaders[i];
	}
	tr.numShaders = ARRAY_LEN( shaders );
	tr.refdef.rdflags = RDF_NOWORLDMODEL;
	tr.defaultShader = &shaders[0];
	tess.xyz = tess_xyz;
	tess.normal = tess_normal;
	tess.texCoords0 = tess_texCoords0;
	tess.texCoords1 = tess_texCoords1;
	tess.indexes = tess_indexes;
	tess.vertexColors = tess_vertexColors;
	tess.maxShaderVerts = SHADER_MAX_VERTEXES;
	tess.maxShaderIndicies = SHADER_MAX_INDEXES;
	tess.numVertexes = tess.numIndexes = 0;
	r_lodbias->value = 0;
	r_lodbias->integer = 0;
	r_lodscale->value = 5;
	R_ModelInit();
}

/* register a model and report what happened; no error may escape the loaders */
static qhandle_t Register( const char *name ) {
	qhandle_t h;
	int errors = errorCount;

	errorArmed = 1;
	if ( setjmp( errorJump ) ) {
		errorArmed = 0;
		CHECK( 0, "error escaped while loading %s: %s", name, lastMessage );
		return 0;
	}
	h = RE_RegisterModel( name );
	errorArmed = 0;
	CHECK( errorCount == errors, "loader raised an error for %s", name );
	return h;
}

/* ---------------------------------------------------------------- builders */

typedef struct {
	byte data[16384];
	int length;
	int surf[2];        // offsets of the first two surfaces
	int tag[2];         // offsets of the first two (chained) tags
	int verts, tris, collapse, refs, bones, frames, frameSize, tagNames, tags;
	int xyz, xyzComp, baseFrames, compFrames, shader;
} model_t_;

static void W32( byte *p, unsigned int v ) {
	if ( swapMode ) {
		p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
	} else {
		p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
	}
}

static unsigned int R32( const byte *p ) {
	if ( swapMode ) {
		return ( (unsigned)p[0] << 24 ) | ( p[1] << 16 ) | ( p[2] << 8 ) | p[3];
	}
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 );
}

static void W16( byte *p, int v ) {
	if ( swapMode ) {
		p[0] = v >> 8; p[1] = v;
	} else {
		p[0] = v; p[1] = v >> 8;
	}
}

static void WF( byte *p, float f ) {
	unsigned int u;
	memcpy( &u, &f, 4 );
	W32( p, u );
}

#define F32( m, base, type, field, v ) W32( ( m )->data + ( base ) + offsetof( type, field ), (unsigned int)( v ) )
#define FSTR( m, base, type, field, s ) strcpy( (char *)( m )->data + ( base ) + offsetof( type, field ), s )

static void Frame( model_t_ *m, int at ) {
	int j;
	for ( j = 0; j < 3; j++ ) {
		WF( m->data + at + j * 4, -8 );
		WF( m->data + at + 12 + j * 4, 8 );
	}
	WF( m->data + at + 36, 14 );     // radius, not 1, which the MD3 loader treats as a plugin bug
}

/* MD3: 2 frames, 2 tags, 2 surfaces of 1 shader, 4 vertexes and 2 triangles */
static void BuildMD3( model_t_ *m ) {
	int at, s, i, f;
	static const int tri[6] = { 0, 1, 2, 2, 3, 0 };

	memset( m, 0, sizeof( *m ) );
	at = sizeof( md3Header_t );
	F32( m, 0, md3Header_t, ident, MD3_IDENT );
	F32( m, 0, md3Header_t, version, MD3_VERSION );
	FSTR( m, 0, md3Header_t, name, "test" );
	F32( m, 0, md3Header_t, numFrames, 2 );
	F32( m, 0, md3Header_t, numTags, 2 );
	F32( m, 0, md3Header_t, numSurfaces, 2 );
	m->frames = at;
	F32( m, 0, md3Header_t, ofsFrames, at );
	for ( f = 0; f < 2; f++, at += sizeof( md3Frame_t ) ) {
		Frame( m, at );
	}
	m->tags = at;
	F32( m, 0, md3Header_t, ofsTags, at );
	for ( i = 0; i < 4; i++, at += sizeof( md3Tag_t ) ) {
		strcpy( (char *)m->data + at, i & 1 ? "tag_b" : "tag_a" );
		WF( m->data + at + offsetof( md3Tag_t, axis ), 1 );
		WF( m->data + at + offsetof( md3Tag_t, axis ) + 16, 1 );
		WF( m->data + at + offsetof( md3Tag_t, axis ) + 32, 1 );
	}
	F32( m, 0, md3Header_t, ofsSurfaces, at );
	for ( s = 0; s < 2; s++ ) {
		int base = at, o = sizeof( md3Surface_t );
		m->surf[s] = base;
		F32( m, base, md3Surface_t, ident, MD3_IDENT );
		FSTR( m, base, md3Surface_t, name, s ? "surf_1" : "hand" );
		F32( m, base, md3Surface_t, numFrames, 2 );
		F32( m, base, md3Surface_t, numShaders, 1 );
		F32( m, base, md3Surface_t, numVerts, 4 );
		F32( m, base, md3Surface_t, numTriangles, 2 );
		F32( m, base, md3Surface_t, ofsShaders, o );
		if ( !s ) {
			m->shader = base + o;
		}
		strcpy( (char *)m->data + base + o, "models/test" );
		o += sizeof( md3Shader_t );
		F32( m, base, md3Surface_t, ofsTriangles, o );
		if ( !s ) {
			m->tris = base + o;
		}
		for ( i = 0; i < 6; i++, o += 4 ) {
			W32( m->data + base + o, tri[i] );
		}
		F32( m, base, md3Surface_t, ofsSt, o );
		for ( i = 0; i < 8; i++, o += 4 ) {
			WF( m->data + base + o, i * 0.125f );
		}
		F32( m, base, md3Surface_t, ofsXyzNormals, o );
		if ( !s ) {
			m->xyz = base + o;
		}
		for ( i = 0; i < 2 * 4 * 4; i++, o += 2 ) {
			W16( m->data + base + o, i * 37 );
		}
		F32( m, base, md3Surface_t, ofsEnd, o );
		at = base + o;
	}
	F32( m, 0, md3Header_t, ofsEnd, at );
	m->length = at;
}

/* MDC: 3 frames (2 base, 1 compressed), 1 tag, 2 surfaces of 4 vertexes */
static void BuildMDC( model_t_ *m ) {
	int at, s, i, f;
	static const int tri[6] = { 0, 1, 2, 2, 3, 0 };

	memset( m, 0, sizeof( *m ) );
	at = sizeof( mdcHeader_t );
	F32( m, 0, mdcHeader_t, ident, MDC_IDENT );
	F32( m, 0, mdcHeader_t, version, MDC_VERSION );
	FSTR( m, 0, mdcHeader_t, name, "test" );
	F32( m, 0, mdcHeader_t, numFrames, 3 );
	F32( m, 0, mdcHeader_t, numTags, 1 );
	F32( m, 0, mdcHeader_t, numSurfaces, 2 );
	F32( m, 0, mdcHeader_t, ofsFrames, at );
	for ( f = 0; f < 3; f++, at += sizeof( md3Frame_t ) ) {
		Frame( m, at );
	}
	m->tagNames = at;
	F32( m, 0, mdcHeader_t, ofsTagNames, at );
	strcpy( (char *)m->data + at, "tag_a" );
	at += sizeof( mdcTagName_t );
	m->tags = at;
	F32( m, 0, mdcHeader_t, ofsTags, at );
	for ( i = 0; i < 3 * 6; i++, at += 2 ) {
		W16( m->data + at, i * 100 );
	}
	F32( m, 0, mdcHeader_t, ofsSurfaces, at );
	for ( s = 0; s < 2; s++ ) {
		int base = at, o = sizeof( mdcSurface_t );
		m->surf[s] = base;
		F32( m, base, mdcSurface_t, ident, MDC_IDENT );
		FSTR( m, base, mdcSurface_t, name, s ? "surf_1" : "hand" );
		F32( m, base, mdcSurface_t, numBaseFrames, 2 );
		F32( m, base, mdcSurface_t, numCompFrames, 1 );
		F32( m, base, mdcSurface_t, numShaders, 1 );
		F32( m, base, mdcSurface_t, numVerts, 4 );
		F32( m, base, mdcSurface_t, numTriangles, 2 );
		F32( m, base, mdcSurface_t, ofsTriangles, o );
		if ( !s ) {
			m->tris = base + o;
		}
		for ( i = 0; i < 6; i++, o += 4 ) {
			W32( m->data + base + o, tri[i] );
		}
		F32( m, base, mdcSurface_t, ofsShaders, o );
		if ( !s ) {
			m->shader = base + o;
		}
		strcpy( (char *)m->data + base + o, "models/test" );
		o += sizeof( md3Shader_t );
		F32( m, base, mdcSurface_t, ofsSt, o );
		for ( i = 0; i < 8; i++, o += 4 ) {
			WF( m->data + base + o, i * 0.125f );
		}
		F32( m, base, mdcSurface_t, ofsXyzNormals, o );
		if ( !s ) {
			m->xyz = base + o;
		}
		for ( i = 0; i < 2 * 4 * 4; i++, o += 2 ) {
			W16( m->data + base + o, i * 37 );
		}
		F32( m, base, mdcSurface_t, ofsXyzCompressed, o );
		if ( !s ) {
			m->xyzComp = base + o;
		}
		for ( i = 0; i < 4; i++, o += 4 ) {
			W32( m->data + base + o, 0x00808080u + i );
		}
		F32( m, base, mdcSurface_t, ofsFrameBaseFrames, o );
		if ( !s ) {
			m->baseFrames = base + o;
		}
		W16( m->data + base + o, 0 ); W16( m->data + base + o + 2, 1 ); W16( m->data + base + o + 4, 1 );
		o += 8;
		F32( m, base, mdcSurface_t, ofsFrameCompFrames, o );
		if ( !s ) {
			m->compFrames = base + o;
		}
		W16( m->data + base + o, -1 ); W16( m->data + base + o + 2, -1 ); W16( m->data + base + o + 4, 0 );
		o += 8;
		F32( m, base, mdcSurface_t, ofsEnd, o );
		at = base + o;
	}
	F32( m, 0, mdcHeader_t, ofsEnd, at );
	m->length = at;
}

/*
Skeletal surface shared by MDS and MDM: 4 vertexes weighted to bones 0..2,
2 triangles, collapse map {0,0,1,2}, bone references {0,1,2}.
*/
static int SkeletalSurface( model_t_ *m, int base, int surfaceHeader, int vertexHeader ) {
	int o = surfaceHeader, i, k;
	static const int weights[4] = { 1, 2, 1, 3 };
	static const int tri[6] = { 0, 1, 2, 2, 3, 0 };
	static const int collapse[4] = { 0, 0, 1, 2 };

	F32( m, base, mdsSurface_t, ident, 9 );
	FSTR( m, base, mdsSurface_t, name, "body" );
	FSTR( m, base, mdsSurface_t, shader, "models/body" );
	F32( m, base, mdsSurface_t, minLod, 2 );
	F32( m, base, mdsSurface_t, ofsHeader, -base );
	F32( m, base, mdsSurface_t, numVerts, 4 );
	F32( m, base, mdsSurface_t, ofsVerts, o );
	m->verts = base + o;
	for ( i = 0; i < 4; i++ ) {
		WF( m->data + base + o, 1 );
		WF( m->data + base + o + 12, 0.5f );
		W32( m->data + base + o + offsetof( mdsVertex_t, numWeights ), weights[i] );
		o += vertexHeader;
		for ( k = 0; k < weights[i]; k++, o += sizeof( mdsWeight_t ) ) {
			W32( m->data + base + o, ( i + k ) % 3 );
			WF( m->data + base + o + 4, 1.0f / weights[i] );
			WF( m->data + base + o + 8, (float)k );
		}
	}
	F32( m, base, mdsSurface_t, numTriangles, 2 );
	F32( m, base, mdsSurface_t, ofsTriangles, o );
	m->tris = base + o;
	for ( i = 0; i < 6; i++, o += 4 ) {
		W32( m->data + base + o, tri[i] );
	}
	F32( m, base, mdsSurface_t, ofsCollapseMap, o );
	m->collapse = base + o;
	for ( i = 0; i < 4; i++, o += 4 ) {
		W32( m->data + base + o, collapse[i] );
	}
	F32( m, base, mdsSurface_t, numBoneReferences, 3 );
	F32( m, base, mdsSurface_t, ofsBoneReferences, o );
	m->refs = base + o;
	for ( i = 0; i < 3; i++, o += 4 ) {
		W32( m->data + base + o, i );
	}
	F32( m, base, mdsSurface_t, ofsEnd, o );
	return base + o;
}

static int Bones( model_t_ *m, int at, int count ) {
	int i;
	m->bones = at;
	for ( i = 0; i < count; i++, at += sizeof( mdsBoneInfo_t ) ) {
		sprintf( (char *)m->data + at, "bone%d", i );
		W32( m->data + at + offsetof( mdsBoneInfo_t, parent ), i - 1 );
		WF( m->data + at + offsetof( mdsBoneInfo_t, torsoWeight ), i == 2 ? 0.5f : 0 );
		WF( m->data + at + offsetof( mdsBoneInfo_t, parentDist ), 4 );
	}
	return at;
}

/* MDS: 2 frames, 3 bones in a chain, 1 tag on bone 2, 1 surface */
static void BuildMDS( model_t_ *m ) {
	int at, f, i;

	memset( m, 0, sizeof( *m ) );
	at = sizeof( mdsHeader_t );
	F32( m, 0, mdsHeader_t, ident, MDS_IDENT );
	F32( m, 0, mdsHeader_t, version, MDS_VERSION );
	FSTR( m, 0, mdsHeader_t, name, "test" );
	WF( m->data + offsetof( mdsHeader_t, lodScale ), 1 );
	F32( m, 0, mdsHeader_t, numFrames, 2 );
	F32( m, 0, mdsHeader_t, numBones, 3 );
	F32( m, 0, mdsHeader_t, torsoParent, 1 );
	m->frameSize = offsetof( mdsFrame_t, bones ) + 3 * sizeof( mdsBoneFrameCompressed_t );
	m->frames = at;
	F32( m, 0, mdsHeader_t, ofsFrames, at );
	for ( f = 0; f < 2; f++, at += m->frameSize ) {
		Frame( m, at );
		for ( i = 0; i < 3 * 6; i++ ) {
			W16( m->data + at + offsetof( mdsFrame_t, bones ) + i * 2, i * 1000 + f );
		}
	}
	F32( m, 0, mdsHeader_t, ofsBones, at );
	at = Bones( m, at, 3 );
	F32( m, 0, mdsHeader_t, numTags, 1 );
	F32( m, 0, mdsHeader_t, ofsTags, at );
	m->tags = at;
	strcpy( (char *)m->data + at, "tag_a" );
	W32( m->data + at + offsetof( mdsTag_t, boneIndex ), 2 );
	at += sizeof( mdsTag_t );
	F32( m, 0, mdsHeader_t, numSurfaces, 1 );
	F32( m, 0, mdsHeader_t, ofsSurfaces, at );
	m->surf[0] = at;
	at = SkeletalSurface( m, at, sizeof( mdsSurface_t ), offsetof( mdsVertex_t, weights ) );
	F32( m, 0, mdsHeader_t, ofsEnd, at );
	m->length = at;
}

/* MDM: 1 surface on bones 0..2, then 2 tags each with their bone references */
static void BuildMDM( model_t_ *m ) {
	int at, t, i;

	memset( m, 0, sizeof( *m ) );
	at = sizeof( mdmHeader_t );
	F32( m, 0, mdmHeader_t, ident, MDM_IDENT );
	F32( m, 0, mdmHeader_t, version, MDM_VERSION );
	FSTR( m, 0, mdmHeader_t, name, "test" );
	WF( m->data + offsetof( mdmHeader_t, lodScale ), 1 );
	F32( m, 0, mdmHeader_t, numSurfaces, 1 );
	F32( m, 0, mdmHeader_t, ofsSurfaces, at );
	m->surf[0] = at;
	at = SkeletalSurface( m, at, sizeof( mdmSurface_t ), offsetof( mdmVertex_t, weights ) );
	F32( m, 0, mdmHeader_t, numTags, 2 );
	F32( m, 0, mdmHeader_t, ofsTags, at );
	for ( t = 0; t < 2; t++ ) {
		int base = at;
		m->tag[t] = base;
		strcpy( (char *)m->data + base, t ? "tag_b" : "tag_a" );
		WF( m->data + base + offsetof( mdmTag_t, axis ), 1 );
		WF( m->data + base + offsetof( mdmTag_t, axis ) + 16, 1 );
		WF( m->data + base + offsetof( mdmTag_t, axis ) + 32, 1 );
		F32( m, base, mdmTag_t, boneIndex, 2 - t );
		F32( m, base, mdmTag_t, numBoneReferences, 3 - t );
		F32( m, base, mdmTag_t, ofsBoneReferences, sizeof( mdmTag_t ) );
		for ( i = 0; i < 3 - t; i++ ) {
			W32( m->data + base + sizeof( mdmTag_t ) + i * 4, i );
		}
		F32( m, base, mdmTag_t, ofsEnd, sizeof( mdmTag_t ) + ( 3 - t ) * 4 );
		at = base + sizeof( mdmTag_t ) + ( 3 - t ) * 4;
	}
	F32( m, 0, mdmHeader_t, ofsEnd, at );
	m->length = at;
}

/* MDX: 3 frames, numBones bones in a chain */
static void BuildMDX( model_t_ *m, int numBones ) {
	int at, f, i;

	memset( m, 0, sizeof( *m ) );
	at = sizeof( mdxHeader_t );
	F32( m, 0, mdxHeader_t, ident, MDX_IDENT );
	F32( m, 0, mdxHeader_t, version, MDX_VERSION );
	FSTR( m, 0, mdxHeader_t, name, "test" );
	F32( m, 0, mdxHeader_t, numFrames, 3 );
	F32( m, 0, mdxHeader_t, numBones, numBones );
	F32( m, 0, mdxHeader_t, torsoParent, 1 );
	m->frameSize = sizeof( mdxFrame_t ) + numBones * sizeof( mdxBoneFrameCompressed_t );
	m->frames = at;
	F32( m, 0, mdxHeader_t, ofsFrames, at );
	for ( f = 0; f < 3; f++, at += m->frameSize ) {
		Frame( m, at );
		for ( i = 0; i < numBones * 6; i++ ) {
			W16( m->data + at + sizeof( mdxFrame_t ) + i * 2, i * 999 + f );
		}
	}
	F32( m, 0, mdxHeader_t, ofsBones, at );
	at = Bones( m, at, numBones );
	F32( m, 0, mdxHeader_t, ofsEnd, at );
	m->length = at;
}

static void Set32( model_t_ *m, int at, unsigned int v ) {
	W32( m->data + at, v );
}

static void Set16( model_t_ *m, int at, int v ) {
	W16( m->data + at, v );
}

/* ---------------------------------------------------------------- runtime */

/*
Drive what the renderer does with a loaded model: tags, surface submission and
vertex generation at several LODs. skeleton is the MDX handle an MDM is paired
with. Frames and tag start indexes come from the game, not the model; they stay
in range for every model unless oddInputs is set, so a case that fails on the
old code fails at its own model, not at the game's numbers.
*/
static int oddInputs;

static void Exercise( qhandle_t h, qhandle_t skeleton, qhandle_t otherSkeleton ) {
	static const int oddFrames[][2] = { { 0, 0 }, { 1, 0 }, { 2, 2 }, { 2, 1 }, { -1, 0 }, { 5000, 1 },
										{ 0x7fffffff, (int)0x80000000 }, { -7, -7 } };
	static const int saneFrames[][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const char *tagNames[] = { "tag_a", "tag_b", "tag_none" };
	static const int starts[] = { 0, 1, -3, 100 };
	const int ( *frames )[2] = oddInputs ? oddFrames : saneFrames;
	int numPairs = oddInputs ? (int)ARRAY_LEN( oddFrames ) : (int)ARRAY_LEN( saneFrames );
	int numStarts = oddInputs ? (int)ARRAY_LEN( starts ) : 2;
	model_t *mod = R_GetModelByHandle( h );
	trRefEntity_t ent;
	orientation_t o;
	int i, j, k, pass, pair;

	errorArmed = 1;
	if ( setjmp( errorJump ) ) {
		errorArmed = 0;
		CHECK( 0, "error escaped while rendering: %s", lastMessage );
		return;
	}
	for ( pass = 0; pass < 2 * numPairs; pass++ ) {
		pair = pass / 2;
		memset( &ent, 0, sizeof( ent ) );
		AxisClear( ent.e.axis );
		AxisClear( ent.e.torsoAxis );
		ent.e.hModel = h;
		ent.e.frameModel = ent.e.oldframeModel = ent.e.torsoFrameModel = ent.e.oldTorsoFrameModel = skeleton;
		if ( pass % 4 == 3 ) {
			ent.e.torsoFrameModel = otherSkeleton;     // a second skeleton for the torso
		}
		ent.e.frame = frames[pair][0];
		ent.e.oldframe = frames[pair][1];
		ent.e.torsoFrame = frames[( pair + 3 ) % numPairs][0];
		ent.e.oldTorsoFrame = frames[( pair + 3 ) % numPairs][1];
		ent.e.backlerp = pair & 1 ? 0.25f : 0;
		ent.e.torsoBacklerp = pair & 2 ? 0.5f : 0;
		ent.e.reFlags = ( pass & 1 ) ? REFLAG_FORCE_LOD : pair == 2 ? REFLAG_DEAD_LOD : 0;
		ent.e.renderfx = pair == 3 ? RF_WRAP_FRAMES : 0;
		r_lodbias->value = ( pass & 1 ) ? 4 : 0;   // drop skeletal LOD to the collapse map
		r_lodbias->integer = 0;

		// a tag lookup on a skeleton is the game's mistake, so it is an odd input too
		for ( i = 0; i < (int)ARRAY_LEN( tagNames ) && ( oddInputs || mod->type != MOD_MDX ); i++ ) {
			for ( j = 0; j < numStarts; j++ ) {
				R_LerpTag( &o, &ent.e, tagNames[i], starts[j] );
			}
		}

		tr.currentModel = mod;
		numDrawSurfs = 0;
		switch ( mod->type ) {
		case MOD_MESH:
			R_AddMD3Surfaces( &ent );
			break;
		case MOD_MDC:
			R_AddMDCSurfaces( &ent );
			break;
		case MOD_MDS:
			R_AddAnimSurfaces( &ent );
			break;
		case MOD_MDM:
			R_MDM_AddAnimSurfaces( &ent );
			break;
		default:
			break;
		}
		backEnd.currentEntity = &ent;
		for ( k = 0; k < numDrawSurfs; k++ ) {
			tess.numVertexes = tess.numIndexes = 0;
			switch ( *(surfaceType_t *)drawSurfs[k] ) {
			case SF_MD3:
				RB_SurfaceMesh( drawSurfs[k] );
				break;
			case SF_MDC:
				RB_SurfaceCMesh( drawSurfs[k] );
				break;
			case SF_MDS:
				RB_SurfaceAnim( drawSurfs[k] );
				break;
			case SF_MDM:
				RB_MDM_SurfaceAnim( drawSurfs[k] );
				break;
			default:
				break;
			}
			// every index must name a vertex the surface produced
			for ( i = 0; i < tess.numIndexes; i++ ) {
				CHECK( tess.indexes[i] < (glIndex_t)tess.numVertexes, "index %u past %d vertexes", tess.indexes[i], tess.numVertexes );
				if ( tess.indexes[i] >= (glIndex_t)tess.numVertexes ) {
					break;
				}
			}
		}
	}
	errorArmed = 0;
	tess.numVertexes = tess.numIndexes = 0;
}

/* ---------------------------------------------------------------- cases */

static const char *Ext( int kind ) {
	static const char *e[] = { ".md3", ".mdc", ".mds", ".mdm", ".mdx" };
	return e[kind];
}

enum { K_MD3, K_MDC, K_MDS, K_MDM, K_MDX };

static void Build( model_t_ *m, int kind ) {
	switch ( kind ) {
	case K_MD3: BuildMD3( m ); break;
	case K_MDC: BuildMDC( m ); break;
	case K_MDS: BuildMDS( m ); break;
	case K_MDM: BuildMDM( m ); break;
	default: BuildMDX( m, 3 ); break;
	}
}

static qhandle_t skeleton, skeletonSmall;
static byte *skeletonData;      // fuzz mode: MODEL_TEST_SKELETON names a retail MDX
static int skeletonLength;
static byte *ReadWhole( const char *path, int *length );

/* load a model of the given kind (and for MDM its skeletons); returns its handle */
static qhandle_t Load( int kind, const byte *data, int length ) {
	model_t_ x;
	char name[MAX_QPATH];

	ResetRenderer();
	BuildMDX( &x, 3 );
	if ( skeletonData ) {
		AddFile( "models/skel.mdx", skeletonData, skeletonLength );   // a retail skeleton for retail meshes
	} else {
		AddFile( "models/skel.mdx", x.data, x.length );
	}
	BuildMDX( &x, 2 );
	AddFile( "models/small.mdx", x.data, x.length );
	skeleton = Register( "models/skel.mdx" );
	skeletonSmall = Register( "models/small.mdx" );
	CHECK( skeleton && skeletonSmall, "reference skeletons did not load" );
	Com_sprintf( name, sizeof( name ), "models/test%s", Ext( kind ) );
	AddFile( name, data, length );
	return Register( name );
}

static void ExpectLoad( int kind, model_t_ *m, const char *what ) {
	qhandle_t h = Load( kind, m->data, m->length );
	CHECK( h, "%s: valid %s refused (%s)", what, Ext( kind ), lastMessage );
	if ( h ) {
		Exercise( h, skeleton, skeleton );
	}
}

static void ExpectRefuse( int kind, model_t_ *m, const char *what ) {
	qhandle_t h = Load( kind, m->data, m->length );
	CHECK( !h, "%s: malformed %s loaded", what, Ext( kind ) );
	if ( h ) {
		Exercise( h, skeleton, skeleton );
	}
}

static void CaseValid( void ) {
	model_t_ m;
	int kind;
	for ( kind = K_MD3; kind <= K_MDX; kind++ ) {
		Build( &m, kind );
		ExpectLoad( kind, &m, "valid" );
	}
}

/* every proper prefix of each model is refused */
static void CaseTruncation( void ) {
	model_t_ m;
	int kind, n;
	for ( kind = K_MD3; kind <= K_MDX; kind++ ) {
		Build( &m, kind );
		for ( n = 0; n < m.length; n++ ) {
			qhandle_t h = Load( kind, m.data, n );
			CHECK( !h, "%s truncated to %d bytes loaded", Ext( kind ), n );
		}
	}
}

/* MODEL_TEST_ONLY=N runs only the Nth malformed model of a case, to show each one alone */
static int mutateIndex, onlyIndex = -1;
#define MUTATE( kind, what, stmt ) do { if ( onlyIndex < 0 || onlyIndex == mutateIndex ) { model_t_ m; Build( &m, kind ); stmt; \
	if ( onlyIndex >= 0 ) printf( "  %s\n", what ); ExpectRefuse( kind, &m, what ); } mutateIndex++; } while ( 0 )

static void CaseMD3( void ) {
	MUTATE( K_MD3, "end past file", Set32( &m, offsetof( md3Header_t, ofsEnd ), m.length + 4 ) );
	MUTATE( K_MD3, "negative frames offset", Set32( &m, offsetof( md3Header_t, ofsFrames ), -64 ) );
	MUTATE( K_MD3, "huge tags offset", Set32( &m, offsetof( md3Header_t, ofsTags ), 0x7ffffff0 ) );
	MUTATE( K_MD3, "surfaces offset past end", Set32( &m, offsetof( md3Header_t, ofsSurfaces ), m.length ) );
	MUTATE( K_MD3, "surface count past the file", Set32( &m, offsetof( md3Header_t, numSurfaces ), 0x01000000 ) );
	MUTATE( K_MD3, "no frames", Set32( &m, offsetof( md3Header_t, numFrames ), 0 ) );
	MUTATE( K_MD3, "frame count overflow", Set32( &m, offsetof( md3Header_t, numFrames ), 0x40000001 ) );
	MUTATE( K_MD3, "tag count overflow", Set32( &m, offsetof( md3Header_t, numTags ), 0x20000000 ) );
	MUTATE( K_MD3, "triangle count overflowing *3", Set32( &m, m.surf[0] + offsetof( md3Surface_t, numTriangles ), 0x55555556 ) );
	MUTATE( K_MD3, "negative vertex count", Set32( &m, m.surf[0] + offsetof( md3Surface_t, numVerts ), -1 ) );
	MUTATE( K_MD3, "too many vertexes", Set32( &m, m.surf[0] + offsetof( md3Surface_t, numVerts ), SHADER_MAX_VERTEXES ) );
	MUTATE( K_MD3, "surface end does not advance", Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsEnd ), 0 ) );
	MUTATE( K_MD3, "surface end negative", Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsEnd ), -(int)sizeof( md3Surface_t ) ) );
	MUTATE( K_MD3, "surface end past file", Set32( &m, m.surf[1] + offsetof( md3Surface_t, ofsEnd ), 0x10000 ) );
	MUTATE( K_MD3, "triangles outside surface", Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsTriangles ), 0x7ffffff0 ) );
	MUTATE( K_MD3, "xyz outside surface", Set32( &m, m.surf[1] + offsetof( md3Surface_t, ofsXyzNormals ), -4 ) );
	MUTATE( K_MD3, "misaligned texture coordinates", Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsSt ), R32( m.data + m.surf[0] + offsetof( md3Surface_t, ofsSt ) ) + 2 ) );
	MUTATE( K_MD3, "shaders over triangles", Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsShaders ), R32( m.data + m.surf[0] + offsetof( md3Surface_t, ofsTriangles ) ) ) );
	MUTATE( K_MD3, "surface frames past its vertexes", Set32( &m, m.surf[1] + offsetof( md3Surface_t, numFrames ), 3 ) );
	MUTATE( K_MD3, "triangle vertex out of range", Set32( &m, m.tris + 8, 4 ) );
	MUTATE( K_MD3, "negative triangle vertex", Set32( &m, m.tris, -1 ) );
	MUTATE( K_MD3, "unterminated surface name", memset( m.data + m.surf[0] + offsetof( md3Surface_t, name ), 'a', MAX_QPATH ) );
	MUTATE( K_MD3, "unterminated shader name", memset( m.data + m.shader, 'a', MAX_QPATH ) );
	MUTATE( K_MD3, "unterminated tag name", memset( m.data + m.tags, 'a', MAX_QPATH ) );
	MUTATE( K_MD3, "wrong version", Set32( &m, offsetof( md3Header_t, version ), 14 ) );
}

static void CaseMDC( void ) {
	MUTATE( K_MDC, "end past file", Set32( &m, offsetof( mdcHeader_t, ofsEnd ), m.length + 4 ) );
	MUTATE( K_MDC, "tag names outside file", Set32( &m, offsetof( mdcHeader_t, ofsTagNames ), -4 ) );
	MUTATE( K_MDC, "tags outside file", Set32( &m, offsetof( mdcHeader_t, ofsTags ), m.length - 2 ) );
	MUTATE( K_MDC, "base frame out of range", Set16( &m, m.baseFrames + 2, 2 ) );
	MUTATE( K_MDC, "negative base frame", Set16( &m, m.baseFrames, -1 ) );
	MUTATE( K_MDC, "compressed frame out of range", Set16( &m, m.compFrames + 4, 1 ) );
	MUTATE( K_MDC, "too many base frames", Set32( &m, m.surf[0] + offsetof( mdcSurface_t, numBaseFrames ), 4 ) );
	MUTATE( K_MDC, "compressed frames overflow", Set32( &m, m.surf[0] + offsetof( mdcSurface_t, numCompFrames ), 0x40000000 ) );
	MUTATE( K_MDC, "compressed xyz outside surface", Set32( &m, m.surf[0] + offsetof( mdcSurface_t, ofsXyzCompressed ), 0x7ffffffc ) );
	MUTATE( K_MDC, "frame tables overlap", Set32( &m, m.surf[0] + offsetof( mdcSurface_t, ofsFrameCompFrames ), R32( m.data + m.surf[0] + offsetof( mdcSurface_t, ofsFrameBaseFrames ) ) ) );
	MUTATE( K_MDC, "surface end does not advance", Set32( &m, m.surf[1] + offsetof( mdcSurface_t, ofsEnd ), 0 ) );
	MUTATE( K_MDC, "triangle count overflowing *3", Set32( &m, m.surf[1] + offsetof( mdcSurface_t, numTriangles ), 0x55555556 ) );
	MUTATE( K_MDC, "triangle vertex out of range", Set32( &m, m.tris + 4, 9 ) );
	MUTATE( K_MDC, "unterminated tag name", memset( m.data + m.tagNames, 'a', sizeof( mdcTagName_t ) ) );
}

static void CaseMDS( void ) {
	MUTATE( K_MDS, "too many bones", Set32( &m, offsetof( mdsHeader_t, numBones ), MDS_MAX_BONES + 1 ) );
	MUTATE( K_MDS, "bone is its own parent", Set32( &m, m.bones + 2 * sizeof( mdsBoneInfo_t ) + offsetof( mdsBoneInfo_t, parent ), 2 ) );
	MUTATE( K_MDS, "bone parent cycle", Set32( &m, m.bones + offsetof( mdsBoneInfo_t, parent ), 2 ) );
	MUTATE( K_MDS, "bone parent out of range", Set32( &m, m.bones + offsetof( mdsBoneInfo_t, parent ), 1000 ) );
	MUTATE( K_MDS, "tag bone out of range", Set32( &m, m.tags + offsetof( mdsTag_t, boneIndex ), 3 ) );
	MUTATE( K_MDS, "negative tag bone", Set32( &m, m.tags + offsetof( mdsTag_t, boneIndex ), -1 ) );
	MUTATE( K_MDS, "weight bone out of range", Set32( &m, m.verts + offsetof( mdsVertex_t, weights ), 3 ) );
	MUTATE( K_MDS, "weights walk out of the surface", Set32( &m, m.verts + offsetof( mdsVertex_t, numWeights ), 1000 ) );
	MUTATE( K_MDS, "weight count overflow", Set32( &m, m.verts + offsetof( mdsVertex_t, numWeights ), 0x0ccccccd ) );
	MUTATE( K_MDS, "vertex without weights", Set32( &m, m.verts + offsetof( mdsVertex_t, numWeights ), 0 ) );
	MUTATE( K_MDS, "collapse map forward", Set32( &m, m.collapse + 8, 2 ) );
	MUTATE( K_MDS, "collapse map out of range", Set32( &m, m.collapse, 4 ) );
	MUTATE( K_MDS, "bone reference out of range", Set32( &m, m.refs + 4, 3 ) );
	MUTATE( K_MDS, "header back reference wrong", Set32( &m, m.surf[0] + offsetof( mdsSurface_t, ofsHeader ), 0 ) );
	MUTATE( K_MDS, "frames outside file", Set32( &m, offsetof( mdsHeader_t, numFrames ), 1000 ) );
	MUTATE( K_MDS, "frame count overflow", Set32( &m, offsetof( mdsHeader_t, numFrames ), 0x20000000 ) );
	MUTATE( K_MDS, "triangle count overflowing *3", Set32( &m, m.surf[0] + offsetof( mdsSurface_t, numTriangles ), 0x55555556 ) );
	MUTATE( K_MDS, "too many vertexes", Set32( &m, m.surf[0] + offsetof( mdsSurface_t, numVerts ), MDS_MAX_VERTS + 1 ) );
	MUTATE( K_MDS, "surface end does not advance", Set32( &m, m.surf[0] + offsetof( mdsSurface_t, ofsEnd ), 0 ) );
	MUTATE( K_MDS, "verts over triangles", Set32( &m, m.surf[0] + offsetof( mdsSurface_t, ofsTriangles ), R32( m.data + m.surf[0] + offsetof( mdsSurface_t, ofsVerts ) ) ) );
	MUTATE( K_MDS, "unterminated shader name", memset( m.data + m.surf[0] + offsetof( mdsSurface_t, shader ), 'a', MAX_QPATH ) );
	MUTATE( K_MDS, "unterminated tag name", memset( m.data + m.tags, 'a', MAX_QPATH ) );
}

static void CaseMDM( void ) {
	MUTATE( K_MDM, "weight bone past the bone arrays", Set32( &m, m.verts + offsetof( mdmVertex_t, weights ), MDX_MAX_BONES ) );
	MUTATE( K_MDM, "bone reference past the bone arrays", Set32( &m, m.refs, 0x10000 ) );
	MUTATE( K_MDM, "tag bone past the bone arrays", Set32( &m, m.tag[0] + offsetof( mdmTag_t, boneIndex ), MDX_MAX_BONES ) );
	MUTATE( K_MDM, "tag reference past the bone arrays", Set32( &m, m.tag[1] + sizeof( mdmTag_t ), -5 ) );
	MUTATE( K_MDM, "tag end does not advance", Set32( &m, m.tag[0] + offsetof( mdmTag_t, ofsEnd ), 0 ) );
	MUTATE( K_MDM, "tag end past file", Set32( &m, m.tag[1] + offsetof( mdmTag_t, ofsEnd ), 0x1000 ) );
	MUTATE( K_MDM, "tag references outside tag", Set32( &m, m.tag[0] + offsetof( mdmTag_t, numBoneReferences ), 1000 ) );
	MUTATE( K_MDM, "tag references over tag header", Set32( &m, m.tag[0] + offsetof( mdmTag_t, ofsBoneReferences ), 0 ) );
	MUTATE( K_MDM, "too many tags", Set32( &m, offsetof( mdmHeader_t, numTags ), MDM_MAX_TAGS + 1 ) );
	MUTATE( K_MDM, "tags over surfaces", Set32( &m, offsetof( mdmHeader_t, ofsTags ), m.surf[0] ) );
	MUTATE( K_MDM, "weights walk out of the surface", Set32( &m, m.verts + offsetof( mdmVertex_t, numWeights ), 50 ) );
	MUTATE( K_MDM, "collapse map forward", Set32( &m, m.collapse + 4, 1 ) );
	MUTATE( K_MDM, "header back reference wrong", Set32( &m, m.surf[0] + offsetof( mdmSurface_t, ofsHeader ), 4 ) );
	MUTATE( K_MDM, "surface end does not advance", Set32( &m, m.surf[0] + offsetof( mdmSurface_t, ofsEnd ), 0 ) );
	MUTATE( K_MDM, "unterminated tag name", memset( m.data + m.tag[0], 'a', MAX_QPATH ) );
}

static void CaseMDX( void ) {
	MUTATE( K_MDX, "too many bones", Set32( &m, offsetof( mdxHeader_t, numBones ), MDX_MAX_BONES + 1 ) );
	MUTATE( K_MDX, "bone parent cycle", Set32( &m, m.bones + offsetof( mdxBoneInfo_t, parent ), 1 ) );
	MUTATE( K_MDX, "bone parent out of range", Set32( &m, m.bones + sizeof( mdxBoneInfo_t ) + offsetof( mdxBoneInfo_t, parent ), 77 ) );
	MUTATE( K_MDX, "no frames", Set32( &m, offsetof( mdxHeader_t, numFrames ), 0 ) );
	MUTATE( K_MDX, "frame count overflow", Set32( &m, offsetof( mdxHeader_t, numFrames ), 0x10000000 ) );
	MUTATE( K_MDX, "bones outside file", Set32( &m, offsetof( mdxHeader_t, ofsBones ), m.length - 8 ) );
	MUTATE( K_MDX, "frames over bones", Set32( &m, offsetof( mdxHeader_t, ofsFrames ), m.bones ) );
	MUTATE( K_MDX, "end past file", Set32( &m, offsetof( mdxHeader_t, ofsEnd ), 0x7fffffff ) );
}

/* an MDM rendered with a skeleton that lacks its bones, or that is not an MDX */
static void CasePairing( void ) {
	model_t_ m, x;
	qhandle_t h, md3, tiny;

	BuildMDM( &m );
	h = Load( K_MDM, m.data, m.length );
	CHECK( h, "valid MDM refused" );
	BuildMDX( &x, 1 );
	AddFile( "models/tiny.mdx", x.data, x.length );
	tiny = Register( "models/tiny.mdx" );
	BuildMD3( &x );
	AddFile( "models/notskel.md3", x.data, x.length );
	md3 = Register( "models/notskel.md3" );
	CHECK( tiny && md3, "pairing models did not load" );
	if ( h && tiny && md3 ) {
		Exercise( h, tiny, tiny );          // the mesh uses bones 1 and 2, which tiny lacks
		Exercise( h, skeleton, tiny );      // the torso skeleton disagrees on bone count
		Exercise( h, md3, md3 );            // the skeleton handle names an MD3
	}
}

/* an MD3 whose coarser LOD has fewer frames than the first */
static void CaseLodFrames( void ) {
	model_t_ m, lod, x;
	qhandle_t h;
	int kind;

	for ( kind = K_MD3; kind <= K_MDC; kind++ ) {
		char name[MAX_QPATH];
		Build( &m, kind );
		Build( &lod, kind );
		if ( kind == K_MD3 ) {
			Set32( &lod, offsetof( md3Header_t, numFrames ), 1 );
			Set32( &lod, lod.surf[0] + offsetof( md3Surface_t, numFrames ), 1 );
			Set32( &lod, lod.surf[1] + offsetof( md3Surface_t, numFrames ), 1 );
		} else {
			// one frame in the header; the surfaces' frame tables keep three entries,
			// and the ones past the first are not validated, so make them wild
			Set32( &lod, offsetof( mdcHeader_t, numFrames ), 1 );
			Set32( &lod, lod.surf[0] + offsetof( mdcSurface_t, numBaseFrames ), 1 );
			Set32( &lod, lod.surf[1] + offsetof( mdcSurface_t, numBaseFrames ), 1 );
			Set32( &lod, lod.surf[0] + offsetof( mdcSurface_t, numCompFrames ), 0 );
			Set32( &lod, lod.surf[1] + offsetof( mdcSurface_t, numCompFrames ), 0 );
			Set16( &lod, lod.baseFrames + 2, 3000 );
			Set16( &lod, lod.baseFrames + 4, 3000 );
		}
		ResetRenderer();
		BuildMDX( &x, 3 );
		AddFile( "models/skel.mdx", x.data, x.length );
		skeleton = Register( "models/skel.mdx" );
		Com_sprintf( name, sizeof( name ), "models/test%s", Ext( kind ) );
		AddFile( name, m.data, m.length );
		Com_sprintf( name, sizeof( name ), "models/test_1%s", Ext( kind ) );
		AddFile( name, lod.data, lod.length );
		h = Register( kind == K_MD3 ? "models/test.md3" : "models/test.mdc" );
		CHECK( h && R_GetModelByHandle( h )->numLods > 1, "model with a coarser LOD did not load" );
		if ( h ) {
			oddInputs = 1;      // frame 2 is in range for LOD 0 only
			Exercise( h, skeleton, skeleton );
			oddInputs = 0;
		}
	}
}

/*
LOD sets: md3[] and mdc[] are one union, so an MD3 base with an MDC LOD (or the
reverse) must not leave one format's data under the other's type; missing and
refused LODs must not leave a NULL slot the renderer can pick.
*/
static void CaseLodMix( void ) {
	static const struct {
		const char *what, *base;
		const char *files[3];     // name, with its kind taken from the extension; 't' marks a truncated file
		int type, numLods;
	} sets[] = {
		{ "MDC base, MD3 LOD", "models/test.mdc", { "models/test.mdc", "models/test_1.md3" }, MOD_MDC, 1 },
		{ "MD3 base, MDC LOD", "models/test.md3", { "models/test.md3", "models/test_1.mdc" }, MOD_MESH, 1 },
		{ "MD3 with an MDC LOD between", "models/test.md3", { "models/test.md3", "models/test_1.mdc", "models/test_2.md3" }, MOD_MESH, 3 },
		{ "MD3 with LODs 1 and 2 missing", "models/test.md3", { "models/test.md3", "models/test_3.md3" }, MOD_MESH, 4 },
		{ "MDC with LODs 1 and 2 missing", "models/test.mdc", { "models/test.mdc", "models/test_3.mdc" }, MOD_MDC, 4 },
		{ "MD3 with a bad LOD 1", "models/test.md3", { "models/test.md3", "models/test_1.md3t", "models/test_2.md3" }, MOD_MESH, 3 },
	};
	int i, j, lod;

	for ( i = 0; i < (int)ARRAY_LEN( sets ); i++ ) {
		model_t_ m, x;
		qhandle_t h;
		model_t *mod;

		ResetRenderer();
		BuildMDX( &x, 3 );
		AddFile( "models/skel.mdx", x.data, x.length );
		skeleton = Register( "models/skel.mdx" );
		for ( j = 0; j < 3 && sets[i].files[j]; j++ ) {
			char name[MAX_QPATH];
			int truncated;
			Q_strncpyz( name, sets[i].files[j], sizeof( name ) );
			truncated = name[strlen( name ) - 1] == 't';
			if ( truncated ) {
				name[strlen( name ) - 1] = 0;
			}
			Build( &m, name[strlen( name ) - 1] == 'c' ? K_MDC : K_MD3 );
			AddFile( name, m.data, truncated ? m.length / 2 : m.length );
		}
		h = Register( sets[i].base );
		mod = R_GetModelByHandle( h );
		CHECK( h && mod->type == sets[i].type && mod->numLods == sets[i].numLods,
			   "%s: handle %d type %d lods %d", sets[i].what, h, mod->type, mod->numLods );
		if ( !h ) {
			continue;
		}
		for ( lod = 0; lod < mod->numLods; lod++ ) {
			CHECK( mod->model.md3[lod], "%s: LOD %d of %d is empty", sets[i].what, lod, mod->numLods );
		}
		Exercise( h, skeleton, skeleton );
	}
}

/* the MD3/MDC loaders write through file offsets when they swap; check an overlap of
   shaders and triangles is refused before the shaderIndex store reaches a triangle */
static void CaseOverlap( void ) {
	model_t_ m;
	int shadersAt;

	BuildMD3( &m );
	shadersAt = R32( m.data + m.surf[0] + offsetof( md3Surface_t, ofsShaders ) );
	Set32( &m, m.surf[0] + offsetof( md3Surface_t, ofsTriangles ), shadersAt + offsetof( md3Shader_t, shaderIndex ) - 8 );
	ExpectRefuse( K_MD3, &m, "triangles under shaderIndex" );
}

/* ---------------------------------------------------------------- fuzz */

static unsigned int rng = 2463534242u;

static unsigned int Rand( void ) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static int KindOf( const byte *data, int length ) {
	int ident;
	if ( length < 4 ) {
		return -1;
	}
	ident = LittleLong( *(const int *)data );
	return ident == MD3_IDENT ? K_MD3 : ident == MDC_IDENT ? K_MDC : ident == MDS_IDENT ? K_MDS :
		   ident == MDM_IDENT ? K_MDM : ident == MDX_IDENT ? K_MDX : -1;
}

static int fuzzLoaded, fuzzRefused;

static void Mutate( byte *data, int *length, int maxLength ) {
	static const unsigned int interesting[] = { 0, 1, 2, 3, 4, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x55555556u,
		0x40000000u, 0x10000u, 1024, 1025, 2049, 2050, 127, 128, 129, 6000, 6001 };
	int ops = 1 + Rand() % 3, i;

	for ( i = 0; i < ops && *length > 4; i++ ) {
		int at = Rand() % *length;
		switch ( Rand() % 6 ) {
		case 0:
			data[at] ^= 1 << ( Rand() % 8 );
			break;
		case 1:
			data[at] = Rand();
			break;
		case 2:
			at &= ~3;
			if ( at + 4 <= *length ) {
				W32( data + at, interesting[Rand() % ARRAY_LEN( interesting )] );
			}
			break;
		case 3:
			at &= ~3;
			if ( at + 4 <= *length ) {
				W32( data + at, R32( data + at ) + ( Rand() % 33 ) - 16 );
			}
			break;
		case 4:
			at &= ~1;
			if ( at + 2 <= *length ) {
				W16( data + at, (int)( Rand() % 9 ) - 2 );
			}
			break;
		default:
			if ( Rand() % 4 == 0 ) {
				*length = at;
			}
			break;
		}
	}
}

/* load one mutated model; exercise it, and a mutated skeleton under a valid mesh */
static void FuzzOne( int kind, const byte *data, int length ) {
	qhandle_t h = Load( kind, data, length );

	if ( !h ) {
		fuzzRefused++;
		return;
	}
	fuzzLoaded++;
	Exercise( h, skeleton, skeleton );
	Exercise( h, skeletonSmall, skeleton );
	if ( kind == K_MDX ) {
		model_t_ mesh;
		qhandle_t mh;
		BuildMDM( &mesh );
		AddFile( "models/mesh.mdm", mesh.data, mesh.length );
		mh = Register( "models/mesh.mdm" );
		if ( mh ) {
			Exercise( mh, h, h );
			Exercise( mh, h, skeleton );
		}
	}
}

static void FuzzBuffer( const byte *seed, int length, int iterations ) {
	byte *work = malloc( length + 1 );
	int kind = KindOf( seed, length ), n, it;

	if ( kind < 0 ) {
		printf( "  not a model in this byte order\n" );
		free( work );
		return;
	}
	for ( it = 0; it < iterations && !failures; it++ ) {
		memcpy( work, seed, length );
		n = length;
		Mutate( work, &n, length );
		FuzzOne( kind, work, n );
	}
	free( work );
}

static void FuzzSynthetic( int iterations ) {
	model_t_ m;
	byte work[sizeof( m.data )];
	int kind, it, n;

	for ( kind = K_MD3; kind <= K_MDX; kind++ ) {
		Build( &m, kind );
		for ( it = 0; it < iterations && !failures; it++ ) {
			memcpy( work, m.data, m.length );
			n = m.length;
			Mutate( work, &n, m.length );
			FuzzOne( kind, work, n );
		}
	}
}

/* the game's frame numbers and tag start indexes past every model */
static void CaseRuntime( void ) {
	model_t_ m;
	int kind;

	oddInputs = 1;
	for ( kind = K_MD3; kind <= K_MDX; kind++ ) {
		Build( &m, kind );
		ExpectLoad( kind, &m, "valid, odd frames" );
	}
	oddInputs = 0;
}

static void CaseFuzz( void ) {
	oddInputs = 1;
	fuzzLoaded = fuzzRefused = 0;
	FuzzSynthetic( getenv( "MODEL_TEST_FUZZ" ) ? atoi( getenv( "MODEL_TEST_FUZZ" ) ) : 4000 );
	printf( "  synthetic fuzz: %d loaded, %d refused\n", fuzzLoaded, fuzzRefused );
	oddInputs = 0;
}

/* ---------------------------------------------------------------- retail */

static unsigned int Checksum( const void *data, int size ) {
	const byte *p = data;
	unsigned int h = 2166136261u;
	int i;
	for ( i = 0; i < size; i++ ) {
		h = ( h ^ p[i] ) * 16777619u;
	}
	return h;
}

static void Describe( const char *path, qhandle_t h ) {
	model_t *mod = R_GetModelByHandle( h );
	int lod, i, surfaces, verts, tris;

	printf( "%s ", path );
	if ( !h ) {
		lastMessage[strcspn( lastMessage, "\n" )] = 0;
		printf( "REFUSED %s", lastMessage );
		return;
	}
	switch ( mod->type ) {
	case MOD_MESH:
	case MOD_MDC:
		printf( "%s lods %d", mod->type == MOD_MESH ? "md3" : "mdc", mod->numLods );
		for ( lod = 0; lod < MD3_MAX_LODS; lod++ ) {
			void *header = mod->type == MOD_MESH ? (void *)mod->model.md3[lod] : (void *)mod->model.mdc[lod];
			int size, ofs, count;
			if ( !header || ( lod && header == ( mod->type == MOD_MESH ? (void *)mod->model.md3[lod - 1] : (void *)mod->model.mdc[lod - 1] ) ) ) {
				continue;
			}
			if ( mod->type == MOD_MESH ) {
				md3Header_t *m = header;
				md3Surface_t *s = (md3Surface_t *)( (byte *)m + m->ofsSurfaces );
				size = m->ofsEnd; count = m->numSurfaces;
				for ( verts = tris = i = 0; i < count; i++, s = (md3Surface_t *)( (byte *)s + s->ofsEnd ) ) {
					verts += s->numVerts; tris += s->numTriangles;
				}
				ofs = m->numFrames;
			} else {
				mdcHeader_t *m = header;
				mdcSurface_t *s = (mdcSurface_t *)( (byte *)m + m->ofsSurfaces );
				size = m->ofsEnd; count = m->numSurfaces;
				for ( verts = tris = i = 0; i < count; i++, s = (mdcSurface_t *)( (byte *)s + s->ofsEnd ) ) {
					verts += s->numVerts; tris += s->numTriangles;
				}
				ofs = m->numFrames;
			}
			printf( " [lod%d frames %d surfaces %d verts %d tris %d bytes %d sum %08x]", lod, ofs, count, verts, tris, size, Checksum( header, size ) );
		}
		break;
	case MOD_MDS: {
		mdsHeader_t *m = mod->model.mds;
		printf( "mds frames %d bones %d surfaces %d bytes %d sum %08x", m->numFrames, m->numBones, m->numSurfaces, m->ofsEnd, Checksum( m, m->ofsEnd ) );
		break;
	}
	case MOD_MDM: {
		mdmHeader_t *m = mod->model.mdm;
		mdmSurface_t *s = (mdmSurface_t *)( (byte *)m + m->ofsSurfaces );
		for ( verts = tris = surfaces = 0; surfaces < m->numSurfaces; surfaces++, s = (mdmSurface_t *)( (byte *)s + s->ofsEnd ) ) {
			verts += s->numVerts; tris += s->numTriangles;
		}
		printf( "mdm surfaces %d tags %d verts %d tris %d bytes %d sum %08x", m->numSurfaces, m->numTags, verts, tris, m->ofsEnd, Checksum( m, m->ofsEnd ) );
		break;
	}
	case MOD_MDX: {
		mdxHeader_t *m = mod->model.mdx;
		printf( "mdx frames %d bones %d bytes %d sum %08x", m->numFrames, m->numBones, m->ofsEnd, Checksum( m, m->ofsEnd ) );
		break;
	}
	default:
		printf( "type %d", mod->type );
	}
}

static int Retail( const char *root, int count, char **paths ) {
	int i, refused = 0;

	diskRoot = root;
	for ( i = 0; i < count; i++ ) {
		qhandle_t h;
		diskExtension = strrchr( paths[i], '.' );
		ResetRenderer();
		lastMessage[0] = 0;
		h = Register( paths[i] );
		Describe( paths[i], h );
		printf( "\n" );
		refused += !h;
	}
	diskRoot = NULL;
	return refused;
}

/* ---------------------------------------------------------------- main */

static const struct {
	const char *name;
	void ( *run )( void );
} cases[] = {
	{ "valid", CaseValid },
	{ "truncation", CaseTruncation },
	{ "md3", CaseMD3 },
	{ "mdc", CaseMDC },
	{ "mds", CaseMDS },
	{ "mdm", CaseMDM },
	{ "mdx", CaseMDX },
	{ "pairing", CasePairing },
	{ "lodframes", CaseLodFrames },
	{ "lodmix", CaseLodMix },
	{ "overlap", CaseOverlap },
	{ "runtime", CaseRuntime },
	{ "fuzz", CaseFuzz },
};

static byte *ReadWhole( const char *path, int *length ) {
	FILE *f = fopen( path, "rb" );
	byte *data;
	long n;
	if ( !f ) {
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	n = ftell( f );
	fseek( f, 0, SEEK_SET );
	data = malloc( n + 1 );
	if ( fread( data, 1, n, f ) != (size_t)n ) {
		n = 0;
	}
	fclose( f );
	*length = (int)n;
	return data;
}

int main( int argc, char **argv ) {
	int i;

	Swap_Init();
#ifdef MODEL_TEST_SWAP
	_LittleShort = ShortSwap;
	_LittleLong = LongSwap;
	_LittleFloat = FloatSwap;
	swapMode = 1;
#endif
	verbose = getenv( "MODEL_TEST_VERBOSE" ) != NULL;
	if ( getenv( "MODEL_TEST_ONLY" ) ) {
		onlyIndex = atoi( getenv( "MODEL_TEST_ONLY" ) );
	}
	ri.Printf = StubPrintf;
	ri.Error = StubError;
	ri.Hunk_Alloc = StubHunkAlloc;
	ri.Hunk_AllocateTempMemory = StubTempAlloc;
	ri.Hunk_FreeTempMemory = StubTempFree;
	ri.FS_ReadFile = StubReadFile;
	ri.FS_FreeFile = StubFreeFile;
	ri.Cmd_ExecuteText = StubExecuteText;

	if ( argc > 1 && !strcmp( argv[1], "list" ) ) {
		for ( i = 0; i < (int)ARRAY_LEN( cases ); i++ ) {
			printf( "%s\n", cases[i].name );
		}
		return 0;
	}
	if ( argc > 2 && !strcmp( argv[1], "case" ) ) {
		for ( i = 0; i < (int)ARRAY_LEN( cases ); i++ ) {
			if ( !strcmp( cases[i].name, argv[2] ) ) {
				cases[i].run();
				printf( "%s: %s\n", argv[2], failures ? "FAIL" : "ok" );
				return failures != 0;
			}
		}
		return 2;
	}
	if ( argc > 3 && !strcmp( argv[1], "fuzz" ) ) {
		int iterations = atoi( argv[2] );
		oddInputs = 1;
		if ( getenv( "MODEL_TEST_SKELETON" ) ) {
			skeletonData = ReadWhole( getenv( "MODEL_TEST_SKELETON" ), &skeletonLength );
		}
		for ( i = 3; i < argc && !failures; i++ ) {
			int length;
			byte *data = ReadWhole( argv[i], &length );
			if ( !data ) {
				printf( "cannot read %s\n", argv[i] );
				return 2;
			}
			fuzzLoaded = fuzzRefused = 0;
			FuzzBuffer( data, length, iterations );
			printf( "fuzz %s: %d loaded, %d refused\n", argv[i], fuzzLoaded, fuzzRefused );
			free( data );
		}
		return failures != 0;
	}
	if ( argc > 3 && !strcmp( argv[1], "retail" ) ) {
		return Retail( argv[2], argc - 3, argv + 3 ) != 0;
	}

	for ( i = 0; i < (int)ARRAY_LEN( cases ); i++ ) {
		int before = failures;
		cases[i].run();
		printf( "%s: %s\n", cases[i].name, failures == before ? "ok" : "FAIL" );
	}
	printf( "models (%s): %s\n", swapMode ? "byte-swapped" : "native", failures ? "FAIL" : "ok" );
	return failures != 0;
}
