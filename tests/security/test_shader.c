/* The real tr_shader.c against pk3 shader text: more stages than the table, a long
 * tcMod line, lightmap indexes out of range and NULL names. Run under ASan/UBSan.
 * With file arguments it parses every shader in them and prints the stage counts,
 * for comparing retail scripts before and after a change. */
#include "../../src/renderer/tr_shader.c"
#include <stdio.h>
#include <stdlib.h>

trGlobals_t tr;
glconfig_t glConfig;
glstate_t glState;
refimport_t ri;
static cvar_t zero, one = { .integer = 1 };
cvar_t *r_smp = &zero, *r_printShaders = &zero, *r_cacheShaders = &zero, *r_cache = &zero,
	*r_ignoreFastPath = &one, *r_detailTextures = &one, *r_vertexLight = &zero, *r_uiFullScreen = &zero,
	*r_lightmap = &zero, *r_fullbright = &zero, *r_ext_texture_env_add = &zero, *r_dlightScale = &one,
	*r_ext_compressed_textures = &zero, *r_ext_texture_filter_anisotropic = &zero;
static backEndData_t commands;
backEndData_t *backEndData[SMP_FRAMES] = { &commands, &commands };
static image_t white, lightmapImage;
static int fails, images;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		printf( "FAIL: %s\n", what );
		fails++;
	}
}
void QDECL Com_Error( int code, const char *fmt, ... ) { printf( "Com_Error\n" ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
static void QDECL Print( int level, const char *fmt, ... ) {}
static void *Alloc( int size, ha_pref pref ) { return calloc( 1, size ? size : 1 ); }
static void *AllocNamed( int size, const char *name ) { return calloc( 1, size ? size : 1 ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
#ifndef Com_Memset
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
#endif
#ifndef Com_Memcpy
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#endif
image_t *R_FindImageFile( const char *name, qboolean mip, qboolean picmip, int wrap, qboolean lightmap ) {
	images++;
	if ( strstr( name, "/lm_" ) ) {
		return &lightmapImage;
	}
	return strstr( name, "missing" ) ? NULL : &white;
}
image_t *R_FindImageFileExt( const char *name, qboolean mip, qboolean picmip, qboolean cc, int wrap ) { return &white; }
void R_InitSkyTexCoords( float h ) {}
void R_SyncRenderThread( void ) {}
void RB_StageIteratorGeneric( void ) {}
void RB_StageIteratorSky( void ) {}
void RB_StageIteratorVertexLitTexture( void ) {}
void RB_StageIteratorLightmappedMultitexture( void ) {}
void R_DecomposeSort( unsigned sort, int *e, shader_t **s, int *f, int *d, int *a ) { *s = NULL; }
void R_SetFog( int fogvar, int var1, int var2, float r, float g, float b, float density ) {}
qboolean R_TouchImage( image_t *inImage ) { return qtrue; }
char *CopyString( const char *in ) { return strdup( in ); }
void ( APIENTRY * qglActiveTextureARB )( GLenum texture );
static int Cinematic( const char *n, int x, int y, int w, int h, int bits ) { return -1; }

static void Reset( void ) {
	int i;
	memset( &shader, 0, sizeof( shader ) );
	memset( stages, 0, sizeof( stages ) );
	memset( texMods, 0, sizeof( texMods ) );
	Q_strncpyz( shader.name, "test/shader", sizeof( shader.name ) );
	shader.lightmapIndex = LIGHTMAP_NONE;
	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		stages[i].bundle[0].texMods = texMods[i];
	}
}

static char text[1 << 16];

static char *Shader( int count ) {
	int i;
	strcpy( text, "{\n" );
	for ( i = 0; i < count; i++ ) {
		strcat( text, "{\nmap $whiteimage\nblendFunc filter\ntcMod scroll 0.5 -0.25\n}\n" );
	}
	strcat( text, "}\n" );
	return text;
}

static void Stages( void ) {
	char *p;
	int n;

	for ( n = 1; n <= 12; n++ ) {
		qboolean ok;
		Reset();
		p = Shader( n );
		ok = ParseShader( &p );
		Check( ok == ( n <= MAX_SHADER_STAGES ), "stage count accepted only up to MAX_SHADER_STAGES" );
		if ( n <= MAX_SHADER_STAGES ) {
			Check( stages[n - 1].active && stages[n - 1].bundle[0].numTexMods == 1, "last legal stage parsed" );
		}
	}
	printf( "stages: 1-8 accepted, 9-12 refused\n" );
}

static void LongTcMod( void ) {
	char *p;
	int i;

	Reset();
	strcpy( text, "map $whiteimage\ntcMod scroll 1 2" );
	for ( i = 0; i < 400; i++ ) {
		strcat( text, " 0.123456" );
	}
	strcat( text, "\n}\n" );
	p = text;
	Check( ParseStage( &stages[0], &p ), "long tcMod stage parses" );
	Check( stages[0].bundle[0].numTexMods == 1 && stages[0].bundle[0].texMods[0].type == TMOD_SCROLL &&
		   stages[0].bundle[0].texMods[0].scroll[1] == 2.0f, "long tcMod keeps its first fields" );

	/* a retail-style line */
	Reset();
	strcpy( text, "map $whiteimage\ntcMod transform 0.25 0 0 0.25 0.1075 0.1075\n}\n" );
	p = text;
	Check( ParseStage( &stages[0], &p ) && stages[0].bundle[0].texMods[0].type == TMOD_TRANSFORM &&
		   stages[0].bundle[0].texMods[0].matrix[0][0] == 0.25f &&
		   stages[0].bundle[0].texMods[0].translate[1] == 0.1075f, "retail-length tcMod unchanged" );
	printf( "tcMod: 3600-character line bounded\n" );
}

static int Want( const char *name ) {
	return !getenv( "CASE" ) || !strcmp( getenv( "CASE" ), name );
}

static void BadLightmaps( const int *bad, int count ) {
	shader_t *sh;
	int i;

	for ( i = 0; i < count; i++ ) {
		sh = R_FindShader( "textures/implicit", bad[i], qtrue );
		Check( sh->lightmapIndex == LIGHTMAP_BY_VERTEX, "bad lightmap index becomes vertex lit" );
		sh = R_FindShader( "test/explicit", bad[i], qtrue );
		Check( sh->lightmapIndex == LIGHTMAP_BY_VERTEX && !sh->defaultShader, "explicit shader with bad lightmap index" );
	}
}

static void Registration( void ) {
	static char shaders[] =
		"\ntest/explicit\n{\n{\nmap $lightmap\n}\n{\nmap textures/x.tga\nblendFunc filter\n}\n}\n";
	static const int special[] = { LIGHTMAP_2D, LIGHTMAP_BY_VERTEX, LIGHTMAP_WHITEIMAGE, LIGHTMAP_NONE };
	static const int low[] = { -5, -100, -2147483647 - 1 };
	static const int high[] = { MAX_LIGHTMAPS, MAX_LIGHTMAPS + 44, 9999, 2147483647 };
	shader_t *sh;
	int i;

	memset( &tr, 0, sizeof( tr ) );
	tr.defaultImage = tr.whiteImage = tr.dlightImage = &white;
	tr.worldDir = "maps/test";
	s_shaderText = shaders;
	memset( hashTable, 0, sizeof( hashTable ) );
	Reset();
	CreateInternalShaders();

	tr.numLightmaps = 2;
	tr.lightmaps[0] = tr.lightmaps[1] = &lightmapImage;

	if ( Want( "lightmap" ) ) {
		for ( i = 0; i < 4; i++ ) {
			sh = R_FindShader( "textures/implicit", special[i], qtrue );
			Check( sh->lightmapIndex == special[i], "special lightmap value kept" );
		}
		sh = R_FindShader( "textures/implicit", 1, qtrue );
		Check( sh->lightmapIndex == 1 && sh->stages[0]->bundle[0].image[0] == &lightmapImage, "valid lightmap kept" );
		sh = R_FindShader( "textures/implicit", 7, qtrue );
		Check( sh->lightmapIndex == 7 && tr.numLightmaps == 8 && tr.lightmaps[7] == &lightmapImage, "external lightmap below the cap loads" );
		printf( "lightmaps: specials, loaded and external kept\n" );
	}
	if ( Want( "lightmap-high" ) ) {
		BadLightmaps( high, 4 );
		Check( tr.numLightmaps <= MAX_LIGHTMAPS, "no lightmap slot past the table" );
		printf( "lightmaps: 4 indexes >= MAX_LIGHTMAPS vertex lit\n" );
	}
	if ( Want( "lightmap-low" ) ) {
		BadLightmaps( low, 3 );
		printf( "lightmaps: 3 unknown negative indexes vertex lit\n" );
	}
	if ( Want( "null" ) ) {
		Check( RE_RegisterShader( NULL ) == 0, "RE_RegisterShader(NULL)" );
		Check( RE_RegisterShaderNoMip( NULL ) == 0, "RE_RegisterShaderNoMip(NULL)" );
		Check( RE_RegisterShaderLightMap( NULL, 0 ) == 0, "RE_RegisterShaderLightMap(NULL)" );
		Check( RE_RegisterShader( "test/explicit" ) != 0, "named registration still works" );
		Check( RE_RegisterShader( "textures/caf\xe9\xff" ) != 0, "extended bytes hash" );
		Check( RE_RegisterShader( "textures/caf\xe9\xff" ) == RE_RegisterShader( "TEXTURES/CAF\xe9\xff" ), "extended bytes hash stable" );
		printf( "registration: NULL names refused\n" );
	}
}

/* Parse every shader in the given files like the engine finds them: name, then a braced body. */
static int Corpus( int argc, char **argv ) {
	int a, total = 0, refused = 0;

	for ( a = 1; a < argc; a++ ) {
		FILE *f = fopen( argv[a], "rb" );
		long size;
		char *buf, *p, *token;

		if ( !f ) {
			return 1;
		}
		fseek( f, 0, SEEK_END );
		size = ftell( f );
		fseek( f, 0, SEEK_SET );
		buf = calloc( 1, size + 1 );
		if ( fread( buf, 1, size, f ) != (size_t)size ) {
			return 1;
		}
		fclose( f );
		COM_FixPath( buf );
		p = buf;
		while ( 1 ) {
			char name[MAX_QPATH], *body;
			qboolean ok;
			int s;

			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] ) {
				break;
			}
			Q_strncpyz( name, token, sizeof( name ) );
			body = p;
			Reset();
			Q_strncpyz( shader.name, name, sizeof( shader.name ) );
			ok = ParseShader( &body );
			for ( s = 0; s < MAX_SHADER_STAGES && stages[s].active; s++ ) {
			}
			printf( "%s %s %s stages=%d\n", argv[a], name, ok ? "ok" : "REFUSED", s );
			total++;
			refused += !ok;
			SkipBracedSection( &p );
		}
		free( buf );
	}
	fprintf( stderr, "corpus: %d shaders, %d refused\n", total, refused );
	return 0;
}

int main( int argc, char **argv ) {
	ri.Printf = Print;
	ri.Hunk_Alloc = Alloc;
	ri.Z_Malloc = AllocNamed;
	ri.CIN_PlayCinematic = Cinematic;
	tr.defaultImage = tr.whiteImage = tr.dlightImage = &white;
	if ( argc > 1 ) {
		return Corpus( argc, argv );
	}
	/* CASE=name runs one case alone, to show each failing on an unfixed tree */
	if ( Want( "stages" ) ) {
		Stages();
	}
	if ( Want( "tcmod" ) ) {
		LongTcMod();
	}
	Registration();
	if ( fails ) {
		printf( "shader: %d FAILED\n", fails );
		return 1;
	}
	printf( "shader: all passed\n" );
	return 0;
}
