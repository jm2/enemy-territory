/* The real tr_shader.c against pk3 shader text: more stages than the table, a long
 * tcMod line, lightmap indexes out of range, NULL names, numbers that are not finite
 * or past their range, what a refused shader leaves behind, and the shaders after a
 * malformed one. Run under ASan/UBSan, with float-cast-overflow.
 * With file arguments (run in a scripts/ directory, file names only) it loads them as
 * the engine does and prints every label found and every shader's parsed state, for
 * comparing retail scripts before and after a change. */
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
static image_t white, lightmapImage, defaultImage, dlightImage;
static int fails, images, corpus;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		printf( "FAIL: %s\n", what );
		fails++;
	}
}
void QDECL Com_Error( int code, const char *fmt, ... ) { printf( "Com_Error\n" ); exit( 2 ); }
static void QDECL Error( int code, const char *fmt, ... ) { printf( "ri.Error: %s\n", fmt ); exit( 2 ); }
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

/* named images, so the corpus dump can say which image each stage has */
static image_t *namedImages[8192];
static int numNamedImages;

/* corpus mode with IMAGES=file: only the images listed (lower case, one per line) exist */
static char **imageList;
static int numImageList;
static int CompareNames( const void *a, const void *b ) {
	return strcmp( *(char * const *)a, *(char * const *)b );
}
static int ImageListed( const char *lower ) {
	const char *key = lower;
	return bsearch( &key, imageList, numImageList, sizeof( *imageList ), CompareNames ) != NULL;
}
/* R_LoadImage's rules: .tga, or a .jpg in its place; .pcx, .bmp and .jpg as named; pk3 names ignore case */
static int ImageExists( const char *name ) {
	char lower[MAX_QPATH];
	int i, len = strlen( name );
	if ( len < 5 || len >= MAX_QPATH ) {
		return 0;
	}
	for ( i = 0; i <= len; i++ ) {
		lower[i] = tolower( (unsigned char)name[i] );
	}
	if ( !strcmp( lower + len - 4, ".tga" ) ) {
		if ( ImageListed( lower ) ) {
			return 1;
		}
		strcpy( lower + len - 3, "jpg" );
		return ImageListed( lower );
	}
	if ( !strcmp( lower + len - 4, ".pcx" ) || !strcmp( lower + len - 4, ".bmp" ) || !strcmp( lower + len - 4, ".jpg" ) ) {
		return ImageListed( lower );
	}
	return 0;
}
static void LoadImageList( const char *path ) {
	FILE *f = fopen( path, "r" );
	char line[1024];
	int size = 0;
	if ( !f ) {
		exit( 3 );
	}
	while ( fgets( line, sizeof( line ), f ) ) {
		line[strcspn( line, "\r\n" )] = 0;
		if ( numImageList == size ) {
			size = size ? size * 2 : 1024;
			imageList = realloc( imageList, size * sizeof( *imageList ) );
		}
		imageList[numImageList++] = strdup( line );
	}
	fclose( f );
	qsort( imageList, numImageList, sizeof( *imageList ), CompareNames );
}

image_t *R_FindImageFile( const char *name, qboolean mip, qboolean picmip, int wrap, qboolean lightmap ) {
	int i;
	images++;
	if ( imageList && !strstr( name, "/lm_" ) && !ImageExists( name ) ) {
		return NULL;
	}
	if ( strstr( name, "/lm_" ) ) {
		return &lightmapImage;
	}
	if ( strstr( name, "missing" ) ) {
		return NULL;
	}
	if ( !corpus ) {
		return &white;
	}
	for ( i = 0; i < numNamedImages; i++ ) {
		if ( !strcmp( namedImages[i]->imgName, name ) ) {
			return namedImages[i];
		}
	}
	if ( numNamedImages == 8192 ) {
		return &white;
	}
	namedImages[numNamedImages] = calloc( 1, sizeof( image_t ) );
	Q_strncpyz( namedImages[numNamedImages]->imgName, name, MAX_QPATH );
	return namedImages[numNamedImages++];
}
image_t *R_FindImageFileExt( const char *name, qboolean mip, qboolean picmip, qboolean cc, int wrap ) { return &white; }

/* what a shader sets outside itself */
static int clouds, fogCalls;
static float cloudHeight;
static char fogLog[64][160];
static int numFogLog;
void R_InitSkyTexCoords( float h ) {
	clouds++;
	cloudHeight = h;
	if ( corpus && numFogLog < 64 ) {
		snprintf( fogLog[numFogLog++], sizeof( fogLog[0] ), "  clouds %.9g", h );
	}
}
void R_SetFog( int fogvar, int var1, int var2, float r, float g, float b, float density ) {
	fogCalls++;
	if ( numFogLog < 64 ) {
		snprintf( fogLog[numFogLog++], sizeof( fogLog[0] ), "  fog %d %d %d %.9g %.9g %.9g %.9g", fogvar, var1, var2, r, g, b, density );
	}
}
void R_SyncRenderThread( void ) {}
void RB_StageIteratorGeneric( void ) {}
void RB_StageIteratorSky( void ) {}
void RB_StageIteratorVertexLitTexture( void ) {}
void RB_StageIteratorLightmappedMultitexture( void ) {}
void R_DecomposeSort( unsigned sort, int *e, shader_t **s, int *f, int *d, int *a ) { *s = NULL; }
qboolean R_TouchImage( image_t *inImage ) { return qtrue; }
char *CopyString( const char *in ) { return strdup( in ); }
void ( APIENTRY * qglActiveTextureARB )( GLenum texture );
static int Cinematic( const char *n, int x, int y, int w, int h, int bits ) { return -1; }

/* shader files served from memory, or from the current directory in corpus mode */
typedef struct {
	const char *name, *text;
	int length;     // 0: strlen( text )
} memFile_t;
static const memFile_t *memFiles;
static int numMemFiles;
static char **diskFiles;
static int numDiskFiles;

static char **ListFiles( const char *dir, const char *ext, int *count ) {
	char **list;
	int i, n = corpus ? numDiskFiles : numMemFiles;
	list = calloc( n + 1, sizeof( *list ) );
	for ( i = 0; i < n; i++ ) {
		list[i] = strdup( corpus ? diskFiles[i] : memFiles[i].name );
	}
	*count = n;
	return list;
}
static void FreeFileList( char **list ) {
	int i;
	for ( i = 0; list[i]; i++ ) {
		free( list[i] );
	}
	free( list );
}
static int ReadFile( const char *path, void **buf ) {
	const char *name = !strncmp( path, "scripts/", 8 ) ? path + 8 : path;
	char *b;
	int i, length;
	if ( corpus ) {
		FILE *f = fopen( name, "rb" );
		if ( !f ) {
			*buf = NULL;
			return -1;
		}
		fseek( f, 0, SEEK_END );
		length = ftell( f );
		fseek( f, 0, SEEK_SET );
		b = malloc( length + 1 );
		if ( fread( b, 1, length, f ) != (size_t)length ) {
			exit( 3 );
		}
		fclose( f );
		b[length] = 0;
		*buf = b;
		return length;
	}
	for ( i = 0; i < numMemFiles; i++ ) {
		if ( !strcmp( memFiles[i].name, name ) ) {
			length = memFiles[i].length ? memFiles[i].length : (int)strlen( memFiles[i].text );
			b = malloc( length + 1 );
			memcpy( b, memFiles[i].text, length );
			b[length] = 0;
			*buf = b;
			return length;
		}
	}
	*buf = NULL;
	return -1;
}
static void FreeFile( void *buf ) { free( buf ); }

/* a fresh renderer with these shader files loaded, as R_InitShaders does */
static void Load( const memFile_t *files, int count, int cacheShaders ) {
	memFiles = files;
	numMemFiles = count;
	r_cacheShaders = cacheShaders ? &one : &zero;
	memset( hashTable, 0, sizeof( hashTable ) );
	memset( &tr, 0, sizeof( tr ) );
	tr.defaultImage = &defaultImage;
	tr.whiteImage = &white;
	tr.dlightImage = &dlightImage;
	tr.worldDir = "maps/test";
	tr.identityLight = 1;
	tr.numLightmaps = 2;
	tr.lightmaps[0] = tr.lightmaps[1] = &lightmapImage;
	CreateInternalShaders();
	ScanAndLoadShaderFiles();
	clouds = fogCalls = numFogLog = 0;
}

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
	static const memFile_t files[] = {
		{ "test.shader", "\ntest/explicit\n{\n{\nmap $lightmap\n}\n{\nmap textures/x.tga\nblendFunc filter\n}\n}\n" }
	};
	static const int special[] = { LIGHTMAP_2D, LIGHTMAP_BY_VERTEX, LIGHTMAP_WHITEIMAGE, LIGHTMAP_NONE };
	static const int low[] = { -5, -100, -2147483647 - 1 };
	static const int high[] = { MAX_LIGHTMAPS, MAX_LIGHTMAPS + 44, 9999, 2147483647 };
	shader_t *sh;
	int i;

	Load( files, 1, 0 );

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

/* ParseShader on a whole definition: one stage, then the given lines */
static qboolean Parse( const char *shaderLines, const char *stageLines ) {
	char *p;
	Reset();
	snprintf( text, sizeof( text ), "{\n%s\n{\nmap $whiteimage\n%s\n}\n}\n", shaderLines, stageLines );
	p = text;
	return ParseShader( &p );
}

/*
Numbers that are not finite, or past float range where they reach an integer: each
refuses the shader. On the old tree each was taken: UBSan stops at the conversions
(color bytes, R_SetFog's int); the rest show as accepted.
*/
static void NonFinite( void ) {
	static const char *stageLines[] = {
		"rgbGen wave sin nan 1 0 1", "rgbGen wave sin 0 inf 0 1", "rgbGen wave sin 0 1 1e39 1",
		"rgbGen wave sin 0 1 0 -inf", "alphaGen wave square 0 1 0 nan",
		"rgbGen const ( nan 0 0 )", "rgbGen const ( 0 0 inf )", "alphaGen const nan", "alphaGen const -inf",
		"alphaGen normalzfade nan", "alphaGen normalzfade 1 nan 1", "alphaGen normalzfade 1 0 inf",
		"alphaGen portal nan", "animMap inf a.tga b.tga", "animMap 1e39 a.tga",
		"tcMod rotate nan", "tcMod rotate 1e39", "tcMod turb 0 1 nan 1", "tcMod turb 0 1 0 1e39", "tcMod turb nan 1 0 1",
		"tcMod stretch sin 0 1 inf 1", "tcMod stretch sin nan 1 0 1", "tcMod scroll nan 0", "tcMod scale 1 nan",
		"tcMod transform nan 0 0 1 0 0", "tcMod transform 1 0 0 1 0 -nan",
		"tcGen vector ( nan 0 0 ) ( 0 1 0 )", "tcGen vector ( 1 0 0 ) ( 0 nan 0 )",
	};
	static const char *shaderLines[] = {
		"deformVertexes wave nan sin 0 1 0 1", "deformVertexes wave 1e-300 sin 0 1 0 1",
		"deformVertexes wave 1e-40 sin 0 1 0 1", "deformVertexes wave 100 sin 0 1 0 inf",
		"deformVertexes bulge nan 1 1", "deformVertexes bulge 1 1 1e39", "deformVertexes normal 1 inf",
		"deformVertexes move 0 0 nan sin 0 1 0 1", "deformVertexes move 0 0 1 sin 0 1 nan 1",
		"sort nan", "clampTime inf", "skyParms - nan -", "skyParms - 1e39 -",
		"q3map_sun 1 1 1 nan 0 0", "q3map_sun 1e30 1e30 1e30 100 0 0",
		"q3map_sun inf 0 0 100 0 0", "fogParms ( nan 0 0 ) 100", "fogParms ( 0 0 0 ) inf",
		"skyfogvars ( 0 0 0 ) nan", "skyfogvars ( 0 nan 0 ) 0.5", "waterfogvars ( 0 0 0 ) 1e10",
		"waterfogvars ( 0 0 0 ) nan", "fogvars ( 0 0 0 ) 3e9", "fogvars ( inf 0 0 ) 100", "fogvars ( 0 0 0 ) nan",
		"lightgridmulamb inf", "lightgridmuldir nan", "distancecull nan 100 0.5", "distancecull 0 1e-40 0.5",
		"distancecull 0 100 inf",
	};
	int i;
	char what[256];

	for ( i = 0; i < (int)( sizeof( stageLines ) / sizeof( stageLines[0] ) ); i++ ) {
		snprintf( what, sizeof( what ), "refused: stage '%s'", stageLines[i] );
		Check( !Parse( "", stageLines[i] ), what );
	}
	for ( i = 0; i < (int)( sizeof( shaderLines ) / sizeof( shaderLines[0] ) ); i++ ) {
		snprintf( what, sizeof( what ), "refused: '%s'", shaderLines[i] );
		Check( !Parse( shaderLines[i], "" ), what );
	}

	/* finite colors past 0..1 saturate instead of converting out of range */
	Check( Parse( "", "rgbGen const ( 2 -1 0.5 )\nalphaGen const 1.5" ) && stages[0].constantColor[0] == 255 &&
		   stages[0].constantColor[1] == 0 && stages[0].constantColor[2] == 127 && stages[0].constantColor[3] == 255,
		   "colors past 0..1 saturate" );
	Check( Parse( "", "alphaGen normalzfade -3" ) && stages[0].constantColor[3] == 0, "normalzfade alpha saturates" );
	Check( Parse( "fogParms ( 2 0.5 -1 ) 100", "" ) && shader.fogParms.color[0] == 1 && shader.fogParms.color[2] == 0 &&
		   ( (byte *)&shader.fogParms.colorInt )[0] == 255 && ( (byte *)&shader.fogParms.colorInt )[1] == 127,
		   "fog colors past 0..1 saturate" );
	/* last: the old tree dropped the game here */
	Check( !Parse( "", "tcMod scroll 1 2\ntcMod scroll 1 2\ntcMod scroll 1 2\ntcMod scroll 1 2\ntcMod scroll 1 2" ),
		   "refused: five tcMods" );
	printf( "nonfinite: %d stage and %d shader lines refused, colors saturate\n",
			(int)( sizeof( stageLines ) / sizeof( stageLines[0] ) ), (int)( sizeof( shaderLines ) / sizeof( shaderLines[0] ) ) );
}

/* What retail takes stays as it was */
static void Unchanged( void ) {
	texModInfo_t *t;

	Check( Parse( "", "rgbGen const ( 0.5 0.25 1 )\nalphaGen const 0.2" ) && stages[0].constantColor[0] == 127 &&
		   stages[0].constantColor[1] == 63 && stages[0].constantColor[2] == 255 && stages[0].constantColor[3] == 51,
		   "retail constant colors" );
	Check( Parse( "", "alphaGen normalzfade 1.0 -0.7 0.5" ) && stages[0].constantColor[3] == 255 &&
		   stages[0].zFadeBounds[0] == -0.7f && stages[0].zFadeBounds[1] == 0.5f, "normalzfade" );
	Check( Parse( "", "alphaGen normalzfade 0.5 -0.7" ) && stages[0].constantColor[3] == 127 &&
		   stages[0].zFadeBounds[1] == 0, "normalzfade missing upper bound reads 0" );
	Check( Parse( "", "alphaGen const" ) && stages[0].constantColor[3] == 0 && stages[0].alphaGen == AGEN_CONST, "alphaGen const with no value reads 0" );
	Check( Parse( "", "rgbGen wave sin 0.5" ) && stages[0].rgbGen == CGEN_WAVEFORM && stages[0].rgbWave.base == 0.5f, "missing wave parms warn" );
	Check( Parse( "", "rgbGen const ( 0.5 0.25 )" ) && stages[0].rgbGen == CGEN_CONST && stages[0].constantColor[0] == 127 &&
		   stages[0].constantColor[2] == 0, "malformed rgbGen const warns, unread element 0" );
	Check( Parse( "", "tcMod scroll 1e39 -1e39\ntcMod scale 3.4028235e38 1" ) && ( t = &stages[0].bundle[0].texMods[0] ) &&
		   isinf( t[0].scroll[0] ) && t[0].scroll[0] > 0 && isinf( t[0].scroll[1] ) && t[0].scroll[1] < 0 &&
		   t[1].scale[0] == FLT_MAX, "texture coordinate overflow keeps retail's float" );
	Check( Parse( "sort 1e39", "" ) && isinf( shader.sort ), "numeric sort overflow keeps retail's inf" );
	Check( Parse( "sort", "" ) && shader.sort == 0, "missing sort warns" );
	Check( Parse( "deformVertexes wave 0 sin 0 1 0 1", "" ) && shader.deforms[0].deformationSpread == 100.0f, "zero spread is 100" );
	Check( Parse( "deformVertexes wave 194 sin 0 3 0 .4", "" ) && shader.deforms[0].deformationSpread == (float)( 1.0f / 194.0 ) &&
		   shader.deforms[0].deformationWave.frequency == 0.4f, "retail deform" );
	Check( Parse( "deformVertexes wave", "" ) && shader.numDeforms == 1, "missing deform parms warn" );
	Check( Parse( "distancecull 256 4096 0.49", "" ) && shader.distanceCull[3] == (float)( 1.0 / 3840.0f ) &&
		   shader.distanceCull[2] == 0.49f, "retail distancecull" );
	Check( Parse( "distancecull 100 50 0.5", "" ) && shader.distanceCull[0] == 0 && shader.distanceCull[3] == 0, "empty distancecull range" );
	Check( Parse( "fogParms ( 0.4 0.4 0.4 ) 8192", "" ) && shader.fogParms.depthForOpaque == 8192 && shader.fogParms.tcScale == 1.0f / 8192,
		   "retail fogParms" );
	Check( Parse( "clampTime", "" ) && shader.clampTime == 0, "missing clampTime" );
	Check( Parse( "q3map_sun 0.281 0.288 0.370 10 35 45", "" ), "retail sun parses" );
	printf( "unchanged: retail values, missing values and float-only overflow as before\n" );
}

/*
A shader refused for an error in its text is empty and leaves nothing behind: no sun,
clouds, fog, light grid or sun shader. On the old tree it kept its stages and settings
and set the rest as it read them. A missing image is not an error in the text, and
stays as retail: the stages before it, and what was read, are kept.
*/
static void Fallback( void ) {
	static const memFile_t files[] = {
		{ "fallback.shader",
		  "test/partial\n{\n\tcull none\n\tsort 3\n\tdeformVertexes autosprite\n\tskyParms env/sky 512 -\n"
		  "\tfogParms ( 0.5 0.5 0.5 ) 100\n\tnocompress\n"
		  "\t{\n\t\tmap a.tga\n\t\tblendFunc add\n\t}\n\t{\n\t\tmap b.tga\n\t}\n\tbogusKeyword\n}\n"
		  "test/globals\n{\n\tq3map_sun 1 0 0 100 45 30\n\tfogvars ( 1 0 0 ) 2000\n\twaterfogvars ( 0 0 1 ) 500\n"
		  "\tskyfogvars ( 0 1 0 ) 0.5\n\tskyParms - 256 -\n\tlightgridmulamb 2\n\tlightgridmuldir 3\n\tsunshader sun/x\n"
		  "\t{\n\t\tmap a.tga\n\t}\n\tbogusKeyword\n}\n"
		  "test/globals_ok\n{\n\tq3map_sun 1 0 0 100 45 30\n\tfogvars ( 1 0 0 ) 2000\n\twaterfogvars ( 0 0 1 ) 500\n"
		  "\tskyfogvars ( 0 1 0 ) 0.5\n\tskyParms - 256 -\n\tlightgridmulamb 2\n\tlightgridmuldir 3\n\tsunshader sun/x\n"
		  "\t{\n\t\tmap a.tga\n\t}\n}\n"
		  "test/stale_a\n{\n\t{\n\t\tmap a.tga\n\t\ttcMod rotate 10\n\t}\n}\n"
		  "test/stale_b\n{\n\t{\n\t\tmap b.tga\n\t\ttcMod scale 1\n\t}\n}\n"
		  "test/badtime\n{\n\t{\n\t\tmap a.tga\n\t}\n}\n"
		  "test/missing_second\n{\n\tcull none\n\tsunshader sun/y\n\t{\n\t\tmap a.tga\n\t\tblendFunc add\n\t}\n"
		  "\t{\n\t\tmap missing.tga\n\t}\n}\n"
		  "test/missing_first\n{\n\t{\n\t\tmap gfx/missing/sun.tga\n\t}\n}\n" }
	};
	shader_t *sh, *again;

	Load( files, 1, 1 );
	tr.allowCompress = qtrue;
	sh = R_FindShader( "test/partial", 0, qtrue );
	Check( sh->defaultShader && sh->numUnfoggedPasses == 0 && !sh->stages[0], "refused shader draws nothing" );
	Check( !sh->isSky && sh->cullType == CT_FRONT_SIDED && sh->numDeforms == 0 && sh->sort == SS_FOG &&
		   sh->fogParms.depthForOpaque == 0 && !sh->explicitlyDefined, "refused shader keeps none of its settings" );
	Check( clouds == 0, "refused shader builds no cloud layer" );
	Check( tr.allowCompress == qtrue, "refused shader leaves compression at its default" );
	again = R_FindShader( "test/partial", 0, qtrue );
	Check( again == sh, "refused world shader found again, not made again" );

	tr.sunLight[0] = 7;
	tr.sunDirection[2] = 1;
	sh = R_FindShader( "test/globals", LIGHTMAP_NONE, qtrue );
	Check( sh->defaultShader, "globals shader refused" );
	Check( tr.sunLight[0] == 7 && tr.sunDirection[2] == 1, "refused shader leaves the sun" );
	Check( fogCalls == 0, "refused shader sets no fog" );
	Check( clouds == 0, "refused shader leaves the clouds" );
	Check( tr.lightGridMulAmbient == 0 && tr.lightGridMulDirected == 0, "refused shader leaves the light grid" );
	Check( tr.sunShaderName == NULL, "refused shader names no sun shader" );

	/* the same text without the error publishes as retail did */
	numFogLog = 0;
	sh = R_FindShader( "test/globals_ok", LIGHTMAP_NONE, qtrue );
	Check( !sh->defaultShader, "globals shader parses" );
	Check( fabs( tr.sunLight[0] - 100 ) < 1e-4 && tr.sunLight[1] == 0 &&
		   fabs( tr.sunDirection[2] - sin( 30 / 180.0 * M_PI ) ) < 1e-6, "sun published" );
	Check( clouds == 1 && cloudHeight == 256, "clouds published" );
	Check( fogCalls == 4, "four fog calls" );
	{
		int i, sky = 0, water = 0, map = 0, sw = 0;
		for ( i = 0; i < numFogLog; i++ ) {
			sky |= !strcmp( fogLog[i], "  fog 1 0 5 0 1 0 0.5" );
			water |= !strcmp( fogLog[i], "  fog 5 0 500 0 0 1 1.10000002" );
			map |= !strcmp( fogLog[i], "  fog 4 0 2000 1 0 0 2000" );
			sw |= !strcmp( fogLog[i], "  fog 10 4 50 0 0 0 0" );
		}
		Check( sky && water && map && sw, "fog calls as retail" );
		if ( !( sky && water && map && sw ) ) {
			for ( i = 0; i < numFogLog; i++ ) {
				printf( "%s\n", fogLog[i] );
			}
		}
	}
	Check( tr.lightGridMulAmbient == 2 && tr.lightGridMulDirected == 3, "light grid published" );
	Check( tr.sunShaderName && !strcmp( tr.sunShaderName, "sun/x" ), "sun shader published" );

	/* an incomplete tcMod does not take an earlier shader's modifier */
	R_FindShader( "test/stale_a", LIGHTMAP_NONE, qtrue );
	sh = R_FindShader( "test/stale_b", LIGHTMAP_NONE, qtrue );
	Check( sh->stages[0]->bundle[0].numTexMods == 1 && sh->stages[0]->bundle[0].texMods[0].type == TMOD_NONE &&
		   sh->stages[0]->bundle[0].texMods[0].rotateSpeed == 0, "incomplete tcMod is cleared" );

	/* a missing image: as retail */
	sh = R_FindShader( "test/missing_second", LIGHTMAP_NONE, qtrue );
	// retail counts the failed stage as a pass but drops it: one stage drawn
	Check( sh->defaultShader && sh->stages[0] && sh->stages[0]->bundle[0].image[0] == &white && !sh->stages[1] &&
		   sh->cullType == CT_TWO_SIDED, "missing second image keeps the first stage, as retail" );
	Check( tr.sunShaderName && !strcmp( tr.sunShaderName, "sun/y" ), "missing image publishes what was read, as retail" );
	sh = R_FindShader( "test/missing_first", LIGHTMAP_NONE, qtrue );
	Check( sh->defaultShader && !sh->stages[0], "missing first image draws nothing, as retail" );

	/* a remap time offset from the game */
	sh = R_FindShader( "test/badtime", LIGHTMAP_NONE, qtrue );
	sh->timeOffset = 5;
	R_RemapShader( "test/stale_a", "test/badtime", "nan" );
	Check( sh->timeOffset == 5 && !R_FindShader( "test/stale_a", LIGHTMAP_NONE, qtrue )->remappedShader, "non-finite remap offset refused" );
	R_RemapShader( "test/stale_a", "test/badtime", "1e39" );
	Check( sh->timeOffset == 5, "overflowing remap offset refused" );
	R_RemapShader( "test/stale_a", "test/badtime", "2.5" );
	Check( sh->timeOffset == 2.5f && R_FindShader( "test/stale_a", LIGHTMAP_NONE, qtrue )->remappedShader == sh, "valid remap offset" );
	printf( "fallback: refused shaders draw nothing and publish nothing; missing images as retail\n" );
}

/*
Malformed text in the middle of a file, and files that end badly, each loaded alone
between a shader before it and one after it. The shader after must still be found.
On the old tree each malformed one but the refused stage hid what followed.
*/
#define BEFORE "before/x\n{\n\t{\n\t\tmap x.tga\n\t}\n}\n"
#define AFTER( name ) name "\n{\n\t{\n\t\tmap x.tga\n\t}\n}\n"
static void Recovery( int cacheShaders ) {
	static const char nulFile[] = BEFORE "\0n/hidden\n{\n}\n";
	static const struct {
		const char *what, *malformed, *after, *refused;
	} cases[] = {
		{ "a quoted brace", "m/quoted\n{\n\t{\n\t\tmap \"}\"\n\t}\n}\n", "after/quoted", NULL },
		{ "an extra '}'", "m/extra_close\n{\n\t{\n\t\tmap x.tga\n\t}\n}\n}\n", "after/extra_close", NULL },
		{ "a label with no body", "m/no_body\n", "after/no_body", "m/no_body" },
		{ "a body with no label", "{\n\t{\n\t\tmap orphan.tga\n\t}\n}\n", "after/no_label", NULL },
		{ "an empty token", "\"\"\n", "after/empty", NULL },
		{ "a refused stage", "m/refused\n{\n\t{\n\t\tmap x.tga\n\t\tbogus\n\t}\n}\n", "after/refused", "m/refused" },
	};
	char file[1024], what[160];
	memFile_t one, two[2];
	shader_t *sh;
	int i, found = 0, total = 0;

	for ( i = 0; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ); i++ ) {
		snprintf( file, sizeof( file ), BEFORE "%s%s\n{\n\t{\n\t\tmap x.tga\n\t}\n}\n", cases[i].malformed, cases[i].after );
		one.name = "m.shader";
		one.text = file;
		one.length = 0;
		Load( &one, 1, cacheShaders );
		sh = R_FindShader( cases[i].after, LIGHTMAP_NONE, qtrue );
		snprintf( what, sizeof( what ), "shader after %s found (r_cacheShaders %d)", cases[i].what, cacheShaders );
		Check( sh->explicitlyDefined && !sh->defaultShader, what );
		found += sh->explicitlyDefined && !sh->defaultShader;
		total++;
		if ( cases[i].refused ) {
			snprintf( what, sizeof( what ), "%s refused", cases[i].refused );
			Check( R_FindShader( cases[i].refused, LIGHTMAP_NONE, qtrue )->defaultShader, what );
		}
	}

	/* listed first, joined last: after the other file's open body */
	two[0].name = "a.shader";
	two[0].text = AFTER( "after/next_file" );
	two[0].length = 0;
	two[1].name = "b.shader";
	two[1].text = BEFORE "m/open\n{\n\t{\n\t\tmap x.tga\n\t}\n";
	two[1].length = 0;
	Load( two, 2, cacheShaders );
	sh = R_FindShader( "after/next_file", LIGHTMAP_NONE, qtrue );
	snprintf( what, sizeof( what ), "file after an open body found (r_cacheShaders %d)", cacheShaders );
	Check( sh->explicitlyDefined && !sh->defaultShader, what );
	found += sh->explicitlyDefined && !sh->defaultShader;
	total++;
	Check( R_FindShader( "m/open", LIGHTMAP_NONE, qtrue )->defaultShader, "shader with no closing brace refused" );

	/* listed last, joined first: its NUL hid every file after it */
	two[0].text = AFTER( "after/nul" );
	two[1].name = "c.shader";
	two[1].text = nulFile;
	two[1].length = sizeof( nulFile ) - 1;
	Load( two, 2, cacheShaders );
	sh = R_FindShader( "after/nul", LIGHTMAP_NONE, qtrue );
	snprintf( what, sizeof( what ), "file after a NUL found (r_cacheShaders %d)", cacheShaders );
	Check( sh->explicitlyDefined && !sh->defaultShader, what );
	found += sh->explicitlyDefined && !sh->defaultShader;
	total++;
	Check( !R_FindShader( "before/x", LIGHTMAP_NONE, qtrue )->defaultShader, "text before a NUL is read" );
	Check( !R_FindShader( "n/hidden", LIGHTMAP_NONE, qtrue )->explicitlyDefined, "text after a NUL is not read" );
	printf( "recovery: %d of %d shaders after malformed ones found (r_cacheShaders %d)\n", found, total, cacheShaders );
}

static const char *ImageName( const image_t *image ) {
	if ( !image ) {
		return "-";
	}
	if ( image == &white ) {
		return "*white";
	}
	if ( image == &lightmapImage ) {
		return "*lightmap";
	}
	if ( image == &defaultImage ) {
		return "*default";
	}
	if ( image == &dlightImage ) {
		return "*dlight";
	}
	return image->imgName;
}

static void DumpWave( const char *what, const waveForm_t *w ) {
	printf( " %s=%d,%.9g,%.9g,%.9g,%.9g", what, w->func, w->base, w->amplitude, w->phase, w->frequency );
}

/* everything ParseShader and FinishShader decide, for the retail comparison */
static void Dump( const shader_t *sh ) {
	int i, b, m;

	printf( "  default=%d explicit=%d lm=%d sort=%.9g passes=%d cull=%d poly=%d nomip=%d nopicmip=%d merge=%d nofog=%d"
			" surf=%#x contents=%#x clamp=%.9g portal=%.9g fogpass=%d mtenv=%d needs=%d%d%d%d iter=%s\n",
			sh->defaultShader, sh->explicitlyDefined, sh->lightmapIndex, sh->sort, sh->numUnfoggedPasses, sh->cullType,
			sh->polygonOffset, sh->noMipMaps, sh->noPicMip, sh->entityMergable, sh->noFog, sh->surfaceFlags, sh->contentFlags,
			sh->clampTime, sh->portalRange, sh->fogPass, sh->multitextureEnv, sh->needsNormal, sh->needsST1, sh->needsST2,
			sh->needsColor,
			sh->optimalStageIteratorFunc == RB_StageIteratorGeneric ? "generic" : sh->optimalStageIteratorFunc == RB_StageIteratorSky ? "sky" :
			sh->optimalStageIteratorFunc == RB_StageIteratorVertexLitTexture ? "vertexlit" :
			sh->optimalStageIteratorFunc == RB_StageIteratorLightmappedMultitexture ? "lmmulti" : "other" );
	printf( "  sky=%d cloud=%.9g", sh->isSky, sh->sky.cloudHeight );
	for ( i = 0; i < 6; i++ ) {
		printf( " %s/%s", ImageName( sh->sky.outerbox[i] ), ImageName( sh->sky.innerbox[i] ) );
	}
	printf( " fog=%.9g,%.9g,%.9g,%.9g,%#x,%.9g cull=%.9g,%.9g,%.9g,%.9g\n", sh->fogParms.color[0], sh->fogParms.color[1],
			sh->fogParms.color[2], sh->fogParms.depthForOpaque, sh->fogParms.colorInt, sh->fogParms.tcScale,
			sh->distanceCull[0], sh->distanceCull[1], sh->distanceCull[2], sh->distanceCull[3] );
	for ( i = 0; i < sh->numDeforms; i++ ) {
		const deformStage_t *d = &sh->deforms[i];
		printf( "  deform %d move=%.9g,%.9g,%.9g spread=%.9g bulge=%.9g,%.9g,%.9g", d->deformation, d->moveVector[0],
				d->moveVector[1], d->moveVector[2], d->deformationSpread, d->bulgeWidth, d->bulgeHeight, d->bulgeSpeed );
		DumpWave( "wave", &d->deformationWave );
		printf( "\n" );
	}
	for ( i = 0; i < MAX_SHADER_STAGES && sh->stages[i]; i++ ) {
		const shaderStage_t *st = sh->stages[i];
		printf( "  stage %d active=%d state=%#x rgb=%d alpha=%d const=%d,%d,%d,%d acff=%d zfade=%.9g,%.9g detail=%d fogged=%d",
				i, st->active, st->stateBits, st->rgbGen, st->alphaGen, st->constantColor[0], st->constantColor[1],
				st->constantColor[2], st->constantColor[3], st->adjustColorsForFog, st->zFadeBounds[0], st->zFadeBounds[1],
				st->isDetail, st->isFogged );
		DumpWave( "rgbwave", &st->rgbWave );
		DumpWave( "alphawave", &st->alphaWave );
		printf( "\n" );
		for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			const textureBundle_t *bu = &st->bundle[b];
			if ( !bu->image[0] && !bu->numTexMods && !bu->tcGen ) {
				continue;
			}
			printf( "   bundle %d img=%s anims=%d speed=%.9g tcgen=%d vec=%.9g,%.9g,%.9g/%.9g,%.9g,%.9g lm=%d video=%d", b,
					ImageName( bu->image[0] ), bu->numImageAnimations, bu->imageAnimationSpeed, bu->tcGen,
					bu->tcGenVectors[0][0], bu->tcGenVectors[0][1], bu->tcGenVectors[0][2], bu->tcGenVectors[1][0],
					bu->tcGenVectors[1][1], bu->tcGenVectors[1][2], bu->isLightmap, bu->isVideoMap );
			for ( m = 1; m < bu->numImageAnimations && m < MAX_IMAGE_ANIMATIONS; m++ ) {
				printf( " %s", ImageName( bu->image[m] ) );
			}
			printf( "\n" );
			for ( m = 0; m < bu->numTexMods; m++ ) {
				const texModInfo_t *t = &bu->texMods[m];
				printf( "    tcmod %d", t->type );
				switch ( t->type ) {
				case TMOD_TURBULENT:
				case TMOD_STRETCH:
					DumpWave( "wave", &t->wave );
					break;
				case TMOD_TRANSFORM:
					printf( " %.9g %.9g %.9g %.9g %.9g %.9g", t->matrix[0][0], t->matrix[0][1], t->matrix[1][0],
							t->matrix[1][1], t->translate[0], t->translate[1] );
					break;
				case TMOD_SCALE:
					printf( " %.9g %.9g", t->scale[0], t->scale[1] );
					break;
				case TMOD_SCROLL:
					printf( " %.9g %.9g", t->scroll[0], t->scroll[1] );
					break;
				case TMOD_ROTATE:
					printf( " %.9g", t->rotateSpeed );
					break;
				default:
					break;
				}
				printf( "\n" );
			}
		}
	}
}

static int ComparePointers( const void *a, const void *b ) {
	const char *x = *(char * const *)a, *y = *(char * const *)b;
	return x < y ? -1 : x > y;
}

static int CompareLines( const void *a, const void *b ) {
	return strcmp( (const char *)a, (const char *)b );
}

/* Load the files as the engine does; print the labels found, then each shader */
static int Corpus( int argc, char **argv ) {
	static char *labels[100000];
	static vec3_t sunLight, sunDirection;
	static float ambient, directed;
	static const char *sunShader;
	int i, l, count = 0, refused = 0, lightmaps[2] = { LIGHTMAP_NONE, 0 };

	corpus = 1;
	if ( getenv( "IMAGES" ) ) {
		LoadImageList( getenv( "IMAGES" ) );
	}
	diskFiles = argv + 1;
	numDiskFiles = argc - 1;
	Load( NULL, 0, 1 );
	for ( i = 0; i < FILE_HASH_SIZE; i++ ) {
		shaderStringPointer_t *s;
		if ( shaderChecksumLookup[i].pStr ) {
			labels[count++] = shaderChecksumLookup[i].pStr;
		}
		for ( s = shaderChecksumLookup[i].next; s; s = s->next ) {
			labels[count++] = s->pStr;
		}
	}
	qsort( labels, count, sizeof( labels[0] ), ComparePointers );
	for ( i = 0; i < count; i++ ) {
		char *p = labels[i];
		printf( "label %s\n", COM_ParseExt( &p, qtrue ) );
	}
	for ( l = 0; l < 2; l++ ) {
		for ( i = 0; i < count; i++ ) {
			char name[MAX_QPATH], *p = labels[i];
			shader_t *sh;
			int f;

			Q_strncpyz( name, COM_ParseExt( &p, qtrue ), sizeof( name ) );
			numFogLog = 0;
			sh = R_FindShader( name, lightmaps[l], qtrue );
			printf( "shader %s lm %d\n", name, lightmaps[l] );
			qsort( fogLog, numFogLog, sizeof( fogLog[0] ), CompareLines );
			for ( f = 0; f < numFogLog; f++ ) {
				printf( "%s\n", fogLog[f] );
			}
			if ( !VectorCompare( sunLight, tr.sunLight ) || !VectorCompare( sunDirection, tr.sunDirection ) ) {
				VectorCopy( tr.sunLight, sunLight );
				VectorCopy( tr.sunDirection, sunDirection );
				printf( "  sun %.9g %.9g %.9g dir %.9g %.9g %.9g\n", sunLight[0], sunLight[1], sunLight[2],
						sunDirection[0], sunDirection[1], sunDirection[2] );
			}
			if ( ambient != tr.lightGridMulAmbient || directed != tr.lightGridMulDirected ) {
				ambient = tr.lightGridMulAmbient;
				directed = tr.lightGridMulDirected;
				printf( "  lightgrid %.9g %.9g\n", ambient, directed );
			}
			if ( sunShader != tr.sunShaderName ) {
				sunShader = tr.sunShaderName;
				printf( "  sunshader %s\n", sunShader ? sunShader : "-" );
			}
			Dump( sh );
			refused += l == 0 && sh->defaultShader;
		}
	}
	/* EXTRA=file: more names, as skins and sunshader ask for them */
	if ( getenv( "EXTRA" ) ) {
		FILE *f = fopen( getenv( "EXTRA" ), "r" );
		char line[1024];
		if ( !f ) {
			exit( 3 );
		}
		while ( fgets( line, sizeof( line ), f ) ) {
			line[strcspn( line, "\r\n" )] = 0;
			if ( !line[0] ) {
				continue;
			}
			printf( "extra %s\n", line );
			Dump( R_FindShader( line, LIGHTMAP_NONE, qtrue ) );
		}
		fclose( f );
	}
	fprintf( stderr, "corpus: %d labels, %d shaders default\n", count, refused );
	return 0;
}

int main( int argc, char **argv ) {
	ri.Printf = Print;
	ri.Error = Error;
	ri.Hunk_Alloc = Alloc;
	ri.Z_Malloc = AllocNamed;
	ri.CIN_PlayCinematic = Cinematic;
	ri.FS_ListFiles = ListFiles;
	ri.FS_FreeFileList = FreeFileList;
	ri.FS_ReadFile = ReadFile;
	ri.FS_FreeFile = FreeFile;
	tr.defaultImage = &defaultImage;
	tr.whiteImage = &white;
	tr.dlightImage = &dlightImage;
	tr.identityLight = 1;
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
	if ( Want( "nonfinite" ) ) {
		NonFinite();
	}
	if ( Want( "unchanged" ) ) {
		Unchanged();
	}
	if ( Want( "fallback" ) ) {
		Fallback();
	}
	if ( Want( "recovery" ) ) {
		Recovery( 1 );
		Recovery( 0 );
	}
	if ( fails ) {
		printf( "shader: %d FAILED\n", fails );
		return 1;
	}
	printf( "shader: all passed\n" );
	return 0;
}
