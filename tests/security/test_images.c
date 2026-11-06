// Hostile images: truncated, oversized and mutated files must load cleanly or not at all.
#include "image_harness.h"
#include "../../src/renderer/tr_image_cursor.h"

typedef void ( *loader_t )( const char *, byte **, int *, int * );

static byte *pic;
static int width, height;

static void Load( loader_t load, const byte *data, int length ) {
	testFile = data; testLength = length;
	load( "test", &pic, &width, &height );
	if ( testFiles ) { fprintf( stderr, "FAIL: file not released\n" ); exit( 1 ); }
	// touch every pixel a loader claims, so a short buffer shows up under the sanitizer
	if ( pic ) {
		volatile byte sum = 0;
		int i;
		for ( i = 0; i < width * height * 4; i++ ) sum += pic[i];
	}
}
static void Expect( const char *what, int loaded ) {
	if ( ( pic != NULL ) != loaded ) { fprintf( stderr, "FAIL: %s\n", what ); exit( 1 ); }
}

static int MakeTGA( byte *b, int type, int depth, int w, int h, int rle ) {
	int n = 18, i;
	memset( b, 0, 18 );
	b[2] = type; b[12] = w & 255; b[13] = w >> 8; b[14] = h & 255; b[15] = h >> 8; b[16] = depth;
	if ( rle ) {
		for ( i = 0; i < w * h; i += 128 ) { b[n++] = 0xff; memset( b + n, 0x40, depth / 8 ); n += depth / 8; }
	} else {
		memset( b + n, 0x40, w * h * depth / 8 ); n += w * h * depth / 8;
	}
	return n;
}
static int MakeBMP( byte *b, int depth, int w, int h ) {
	int stride = ( w * depth / 8 + 3 ) & ~3, palette = depth == 8 ? 1024 : 0, off = 54 + palette;
	int size = off + stride * h;
	memset( b, 0, size );
	b[0] = 'B'; b[1] = 'M';
	b[2] = size; b[3] = size >> 8; b[4] = size >> 16;
	b[10] = off; b[11] = off >> 8; b[14] = 40;
	b[18] = w; b[19] = w >> 8; b[22] = h; b[23] = h >> 8; b[26] = 1; b[28] = depth;
	return size;
}
static int MakePCX( byte *b, int w, int h ) {
	int n = 128, i;
	memset( b, 0, 128 );
	b[0] = 0x0a; b[1] = 5; b[2] = 1; b[3] = 8;
	b[8] = ( w - 1 ) & 255; b[9] = ( w - 1 ) >> 8; b[10] = ( h - 1 ) & 255; b[11] = ( h - 1 ) >> 8;
	for ( i = 0; i < w * h; i++ ) b[n++] = 5;
	memset( b + n, 0x30, 768 );
	return n + 768;
}

static void Fuzz( loader_t load, const byte *seed, int length, int rounds ) {
	static byte b[1 << 20];
	int r, m, k;
	for ( r = 0; r < rounds; r++ ) {
		memcpy( b, seed, length );
		m = 1 + rand() % 8;
		for ( k = 0; k < m; k++ ) {
			int at = rand() % ( rand() & 1 ? length : ( length < 64 ? length : 64 ) );
			switch ( rand() % 3 ) {
			case 0: b[at] = rand(); break;
			case 1: b[at] = 0xff; break;
			default: b[at] ^= 1 << ( rand() & 7 ); break;
			}
		}
		Load( load, b, rand() & 3 ? length : rand() % ( length + 1 ) );
	}
}

#include "jpeg_seeds.h"

int main( void ) {
	static byte b[1 << 20];
	int n, i;

	H_Init();
	srand( 1234 );

	// well-formed files of each kind still load
	n = MakeTGA( b, 2, 24, 16, 16, 0 ); Load( R_LoadTGA, b, n ); Expect( "tga24", 1 );
	if ( width != 16 || pic[0] != 0x40 || pic[3] != 255 ) { fprintf( stderr, "FAIL: tga pixels\n" ); return 1; }
	n = MakeTGA( b, 2, 32, 16, 16, 0 ); Load( R_LoadTGA, b, n ); Expect( "tga32", 1 );
	n = MakeTGA( b, 3, 8, 16, 16, 0 ); Load( R_LoadTGA, b, n ); Expect( "tga8", 1 );
	n = MakeTGA( b, 10, 24, 64, 64, 1 ); Load( R_LoadTGA, b, n ); Expect( "tga rle", 1 );
	n = MakeBMP( b, 24, 16, 16 ); Load( R_LoadBMP, b, n ); Expect( "bmp24", 1 );
	n = MakeBMP( b, 8, 16, 16 ); Load( R_LoadBMP, b, n ); Expect( "bmp8", 1 );
	n = MakePCX( b, 16, 16 ); Load( R_LoadPCX, b, n ); Expect( "pcx", 1 );
	Load( R_LoadJPG, gray_jpg, gray_jpg_len ); Expect( "gray jpg", 1 );
	if ( pic[0] != pic[1] || pic[3] != 255 ) { fprintf( stderr, "FAIL: gray expansion\n" ); return 1; }
	Load( R_LoadJPG, color_jpg, color_jpg_len ); Expect( "color jpg", 1 );

	// truncation at every length, against the old loaders' read past the end
	n = MakeTGA( b, 2, 24, 16, 16, 0 ); for ( i = 0; i < n; i++ ) { Load( R_LoadTGA, b, i ); Expect( "short tga", 0 ); }
	n = MakeTGA( b, 10, 24, 64, 64, 1 ); for ( i = 0; i < n; i++ ) { Load( R_LoadTGA, b, i ); Expect( "short rle tga", 0 ); }
	n = MakeBMP( b, 24, 16, 16 ); for ( i = 0; i < n; i++ ) { Load( R_LoadBMP, b, i ); Expect( "short bmp", 0 ); }
	n = MakePCX( b, 16, 16 ); for ( i = 0; i < 128 + 16 * 16; i++ ) { Load( R_LoadPCX, b, i ); Expect( "short pcx", 0 ); }
	for ( i = 0; i < (int)color_jpg_len; i++ ) Load( R_LoadJPG, color_jpg, i );

	// sizes that overflow or ask for gigabytes from a few bytes
	n = MakeTGA( b, 2, 24, 1, 1, 0 ); b[12] = b[13] = b[14] = b[15] = 0xff; Load( R_LoadTGA, b, n ); Expect( "huge tga", 0 );
	n = MakeBMP( b, 24, 1, 1 ); b[19] = b[20] = b[21] = 0x7f; b[23] = b[24] = 0x7f; Load( R_LoadBMP, b, n ); Expect( "huge bmp", 0 );
	n = MakeTGA( b, 2, 24, 0, 16, 0 ); Load( R_LoadTGA, b, n ); Expect( "empty tga", 0 );
	// one row of 5000: few pixels, but past the limit a side, and Upload32 cannot halve it evenly
	n = MakeTGA( b, 2, 24, 5000, 1, 0 ); Load( R_LoadTGA, b, n ); Expect( "long thin tga", 0 );

	// palette index past a short palette
	n = MakeBMP( b, 8, 4, 4 ); b[46] = 2; b[54 + 1024] = 200; Load( R_LoadBMP, b, n ); Expect( "bmp palette", 0 );

	// CVE-2013-6629: one component named twice in the scan header
	for ( i = 0; i + 1 < (int)color_jpg_len; i++ ) {
		if ( color_jpg[i] == 0xff && color_jpg[i + 1] == 0xda ) break;
	}
	memcpy( b, color_jpg, color_jpg_len );
	b[i + 7] = b[i + 5];
	Load( R_LoadJPG, b, color_jpg_len ); Expect( "duplicate jpeg component", 0 );

	// a scan in another order than the frame: all there, and the rows must follow the component.
	// libjpeg wrote a larger plane's rows into a subsampled one, past its end
	for ( i = 0; i + 1 < (int)color_jpg_len; i++ ) {
		if ( color_jpg[i] == 0xff && color_jpg[i + 1] == 0xda ) break;
	}
	memcpy( b, color_jpg, color_jpg_len );
	n = b[i + 5]; b[i + 5] = b[i + 9]; b[i + 9] = n;
	n = b[i + 6]; b[i + 6] = b[i + 10]; b[i + 10] = n;
	Load( R_LoadJPG, b, color_jpg_len ); Expect( "reordered jpeg scan", 1 );

	// a DC symbol past the end of extend_test: the first table is a DC one
	for ( i = 0; i + 1 < (int)color_jpg_len; i++ ) {
		if ( color_jpg[i] == 0xff && color_jpg[i + 1] == 0xc4 ) break;
	}
	memcpy( b, color_jpg, color_jpg_len );
	b[i + 5 + 16] = 0xff;
	Load( R_LoadJPG, b, color_jpg_len ); Expect( "bad DC symbol", 0 );

	n = MakeTGA( b, 2, 24, 32, 32, 0 ); Fuzz( R_LoadTGA, b, n, 60000 );
	n = MakeTGA( b, 10, 32, 64, 64, 1 ); Fuzz( R_LoadTGA, b, n, 60000 );
	n = MakeBMP( b, 8, 32, 32 ); Fuzz( R_LoadBMP, b, n, 60000 );
	n = MakePCX( b, 32, 32 ); Fuzz( R_LoadPCX, b, n, 60000 );
	Fuzz( R_LoadJPG, gray_jpg, gray_jpg_len, 40000 );
	Fuzz( R_LoadJPG, color_jpg, color_jpg_len, 40000 );
	printf( "image tests passed\n" );
	return 0;
}
