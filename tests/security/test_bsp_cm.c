/* CM_LoadMap against corrupted BSPs, with the real cm_load.c, cm_patch.c,
 * cm_test.c and cm_polylib.c. A refused map must drop before the previous
 * collision map, checksum or hunk changes. With BSP files as arguments, loads
 * each one, then truncates and mutates it. Run under ASan and UBSan. */
#include "../../src/qcommon/cm_local.h"
#include "bsp_fixture.h"

static cvar_t testCvar;
cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	(void)name; (void)value; (void)flags;
	return &testCvar;
}
#ifdef Hunk_Alloc
#undef Hunk_Alloc
#endif
void *Hunk_Alloc( int size, ha_pref preference ) {
	(void)preference;
	return TestHunk( size );
}
unsigned Com_BlockChecksum( const void *buffer, int length ) {
	const byte *b = buffer;
	unsigned h = 2166136261u;
	int i;
	for ( i = 0; i < length; i++ ) {
		h = ( h ^ b[i] ) * 16777619u;
	}
	return h;
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
			FS_FreeFile( fileBlock );   // the unfixed loader drops with the file open
		}
		return 0;
	}
	CM_LoadMap( name, qfalse, &checksum );
	expectDrop = 0;
	return 1;
}

static int LoaderAccept( const char *name ) {
	int i, patches = 0;
	if ( !LoaderTry( name ) ) {
		printf( "    unexpected drop: %s\n", lastError );
		return 0;
	}
	Check( cm.numSubModels >= 1 && cm.numNodes >= 1 && cm.numLeafs >= 1, "collision arrays published" );
	Check( Terminated( cm.entityString, cm.numEntityChars ), "entity string terminated" );
	for ( i = 0; i < cm.numSurfaces; i++ ) {
		patches += cm.surfaces[i] != NULL;
	}
	if ( strstr( name, "case" ) || strstr( name, "previous" ) ) {
		Check( patches == 1 && cm.numBrushes == 2, "fixture patch and brushes loaded" );
	}
	return 1;
}

static void LoaderReject( const char *name ) {
	clipMap_t before = cm;
	int checksum = 0x5a5a, hunk = hunkCount, r = reads, f = frees, d = drops;

	expectDrop = 1;
	if ( !setjmp( dropJump ) ) {
		CM_LoadMap( name, qfalse, &checksum );
		expectDrop = 0;
		Fail( "CM_LoadMap accepted the map" );
	}
	expectDrop = 0;
	Check( drops == d + 1, "one drop" );
	Check( fileBlock == NULL, "the file was freed before the drop" );
	Check( reads == r + 1 && frees == f + 1, "one read, one free" );
	Check( hunkCount == hunk, "no hunk allocation before the drop" );
	Check( checksum == 0x5a5a, "checksum unchanged" );
	Check( !memcmp( &cm, &before, sizeof( cm ) ), "previous collision map unchanged" );
}

int main( int argc, char **argv ) {
	const char *m = getenv( "BSP_MUTATIONS" );
	whichLoader = 0;
	Swap_Init();
	if ( argc > 1 ) {
		return RunRealMaps( argc, argv, m ? atoi( m ) : 200 ) ? 1 : 0;
	}
	printf( "CM_LoadMap:\n" );
	return RunCases() ? 1 : 0;
}
