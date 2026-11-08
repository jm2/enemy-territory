/* The real tr_image.c skin parser against pk3 .skin files: more surfaces and md3_
 * models than the skin holds, a quoted token of MAX_TOKEN_CHARS, and the default
 * skin's allocation. Run under ASan/UBSan. With file arguments it registers each
 * file as a skin and prints its counts, for comparing retail skins before and after. */
#include "../../src/renderer/tr_image.c"
#include <stdio.h>
#include <stdlib.h>

trGlobals_t tr;
glconfig_t glConfig;
glstate_t glState;
refimport_t ri;
static shader_t defaultShader, found;
static int fails, finds;
static const char *fileText;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		printf( "FAIL: %s\n", what );
		fails++;
	}
}
void QDECL Com_Error( int code, const char *fmt, ... ) { printf( "Com_Error\n" ); exit( 2 ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
static void QDECL Print( int level, const char *fmt, ... ) {}
/* exact sizes, so ASan sees a write past any allocation */
static void *Alloc( int size, ha_pref pref ) { return calloc( 1, size ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
#ifndef Com_Memset
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
#endif
#ifndef Com_Memcpy
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#endif
shader_t *R_FindShader( const char *name, int lightmapIndex, qboolean mip ) {
	finds++;
	return &found;
}
void R_SyncRenderThread( void ) {}
int Com_HashKey( char *string, int maxlen ) { return 0; }
static int ReadFile( const char *name, void **buf ) {
	size_t n;
	if ( !fileText ) {
		FILE *f = fopen( name, "rb" );
		long size;
		char *b;
		if ( !f ) {
			*buf = NULL;
			return -1;
		}
		fseek( f, 0, SEEK_END );
		size = ftell( f );
		fseek( f, 0, SEEK_SET );
		b = malloc( size + 1 );
		n = fread( b, 1, size, f );
		b[n] = 0;
		fclose( f );
		*buf = b;
		return (int)n;
	}
	n = strlen( fileText );
	*buf = malloc( n + 1 );
	memcpy( *buf, fileText, n + 1 );
	return (int)n;
}
static void FreeFile( void *buf ) { free( buf ); }

static char text[1 << 17];

static skin_t *Register( const char *name, const char *body ) {
	qhandle_t h;
	fileText = body;
	h = RE_RegisterSkin( name );
	return h ? tr.skins[h] : NULL;
}

static int Want( const char *name ) {
	return !getenv( "CASE" ) || !strcmp( getenv( "CASE" ), name );
}

int main( int argc, char **argv ) {
	skin_t *skin;
	char line[128];
	int i;

	ri.Printf = Print;
	ri.Hunk_Alloc = Alloc;
	ri.FS_ReadFile = ReadFile;
	ri.FS_FreeFile = FreeFile;
	tr.defaultShader = &defaultShader;

	if ( Want( "default" ) ) {
		R_InitSkins();
		Check( tr.skins[0]->numSurfaces == 1 && tr.skins[0]->surfaces[0]->shader == &defaultShader, "default skin" );
		printf( "default skin: a whole surface allocated\n" );
	} else {
		tr.numSkins = 1;
	}

	if ( argc > 1 ) {
		int a;
		for ( a = 1; a < argc; a++ ) {
			int before = tr.numSkins;

			fileText = NULL;
			i = RE_RegisterSkin( argv[a] );
			// the skin this file made, even one refused for having no surfaces
			skin = tr.numSkins > before ? tr.skins[tr.numSkins - 1] : NULL;
			if ( !skin ) {
				printf( "%s unreadable\n", argv[a] );
				continue;
			}
			printf( "%s %s surfaces=%d models=%d\n", argv[a], i ? "ok" : "default", skin->numSurfaces, skin->numModels );
		}
		return 0;
	}

	if ( Want( "surfaces" ) ) {
		text[0] = 0;
		for ( i = 0; i < 40; i++ ) {
			Com_sprintf( line, sizeof( line ), "surf%d,models/players/s%d.tga\n", i, i );
			strcat( text, line );
		}
		finds = 0;
		skin = Register( "models/test/many.skin", text );
		Check( skin && skin->numSurfaces == MD3_MAX_SURFACES && finds == MD3_MAX_SURFACES, "surfaces capped at MD3_MAX_SURFACES" );
		Check( skin && !strcmp( skin->surfaces[MD3_MAX_SURFACES - 1]->name, "surf31" ), "first surfaces kept in order" );
		printf( "skin: 40 surfaces capped at %d\n", MD3_MAX_SURFACES );
	}

	if ( Want( "models" ) ) {
		text[0] = 0;
		for ( i = 0; i < 8; i++ ) {
			Com_sprintf( line, sizeof( line ), "md3_part%d,models/players/p%d.md3\n", i, i );
			strcat( text, line );
		}
		strcat( text, "after,models/players/after.tga\n" );
		skin = Register( "models/test/models.skin", text );
		Check( skin && skin->numModels == MAX_PART_MODELS && !strcmp( skin->models[MAX_PART_MODELS - 1]->model, "models/players/p4.md3" ), "models capped at MAX_PART_MODELS" );
		Check( skin && skin->numSurfaces == 1 && !strcmp( skin->surfaces[0]->name, "after" ), "parsing stays in step after the cap" );
		printf( "skin: 8 md3_ models capped at %d\n", MAX_PART_MODELS );
	}

	if ( Want( "quoted" ) ) {
		strcpy( text, "\"" );
		for ( i = 0; i < MAX_TOKEN_CHARS + 8; i++ ) {
			strcat( text, "a" );
		}
		strcat( text, "\",x\nlegs,\"models/players/legs.tga\"\n" );
		skin = Register( "models/test/quoted.skin", text );
		Check( skin && skin->numSurfaces == 2 && !strcmp( skin->surfaces[1]->name, "legs" ), "long quoted token bounded" );
		printf( "skin: %d-character quoted token bounded\n", MAX_TOKEN_CHARS + 8 );
	}

	if ( Want( "retail" ) ) {
		skin = Register( "models/test/retail.skin",
						 "md3_lower,models/players/temperate/axis/body.md3\n"
						 "tag_head,\nh_body,\"models/players/temperate/axis/medic/body.tga\"\n"
						 "/* c */ u_body, models/players/temperate/axis/medic/body2.tga // x\n" );
		Check( skin && skin->numModels == 1 && skin->numSurfaces == 2 && !strcmp( skin->surfaces[1]->name, "u_body" ), "retail-style skin unchanged" );
		printf( "skin: retail-style skin unchanged\n" );
	}

	if ( fails ) {
		printf( "skin: %d FAILED\n", fails );
		return 1;
	}
	printf( "skin: all passed\n" );
	return 0;
}
