/* The RoQ cinematic decoder (src/client/cl_cin.c, included whole) fed crafted and
 * mutated streams through CIN_PlayCinematic/CIN_RunCinematic. Run under ASan and UBSan.
 *
 * Valid movies come from a small encoder below. It models the decode independently
 * (codebook expansion, VQ, motion, both frame halves, audio) and every decoded frame
 * and audio chunk must match it. Then hostile streams: oversized chunks, frame sizes,
 * VQ before QUAD_INFO, oversized audio, short codebooks, VQ and motion past their
 * buffers, packets past the data read, truncation at every length, and mutations.
 *
 *   test_cin            run everything
 *   test_cin golden     print one digest per decoded frame and audio chunk
 *   test_cin case NAME  run one hostile case (for showing the original tree fails)
 */
#include <stdint.h>

static int swapPixels;          // simulate a big-endian LittleLong in the pixel packing
#define LittleLong TestLittleLong
#include "../../src/client/cl_cin.c"
#undef LittleLong

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int TestLittleLong( int l ) {
	unsigned int u = (unsigned int)l;
	if ( !swapPixels ) {
		return l;
	}
	return (int)( ( u >> 24 ) | ( ( u >> 8 ) & 0xff00 ) | ( ( u << 8 ) & 0xff0000 ) | ( u << 24 ) );
}

static void Fail( const char *what ) {
	fprintf( stderr, "FAIL: %s\n", what );
	exit( 1 );
}
#define CHECK( c, what ) do { if ( !( c ) ) { Fail( what ); } } while ( 0 )

/* ---------------------------------------------------------------- engine stubs */

static cvar_t timescaleVar, inGameVideoVar;
cvar_t *com_timescale = &timescaleVar, *cl_inGameVideo = &inGameVideoVar;
clientStatic_t cls;
glconfig_t glConfig;
refexport_t re;
vm_t *uivm;
int s_rawend[MAX_STREAMING_SOUNDS];
int s_soundtime;

static unsigned int nowMs = 1000;
static int errors;

void QDECL Com_Error( int level, const char *fmt, ... ) { errors++; Fail( "Com_Error" ); }
void QDECL Com_Printf( const char *fmt, ... ) {}
void QDECL Com_DPrintf( const char *fmt, ... ) {}
void Com_Memset( void *d, const int v, const size_t c ) { memset( d, v, c ); }
void Com_Memcpy( void *d, const void *s, const size_t c ) { memcpy( d, s, c ); }
#undef vsnprintf
int Q_vsnprintf( char *d, int n, const char *f, va_list a ) { return vsnprintf( d, n, f, a ); }
int CL_ScaledMilliseconds( void ) { return (int)nowMs; }
void Con_Close( void ) {}
void S_StopAllSounds( void ) {}
void S_Update( void ) {}
char *Cmd_Argv( int arg ) { return ""; }
char *Cvar_VariableString( const char *name ) { return ""; }
void Cvar_Set( const char *name, const char *value ) {}
void Cbuf_ExecuteText( int when, const char *text ) {}
int QDECL VM_Call( vm_t *vm, int callNum, ... ) { return 0; }
void SCR_AdjustFrom640( float *x, float *y, float *w, float *h ) {}
void *Hunk_AllocateTempMemory( int size ) { void *p = malloc( size ); CHECK( p, "alloc" ); return p; }
void Hunk_FreeTempMemory( void *p ) { free( p ); }

/* the movie file: an exact-size allocation, so ASan sees any read past it */
static byte *fileData;
static int fileLen, filePos, fileOpen, opens, closes, failReopen;

int FS_FOpenFileRead( const char *name, fileHandle_t *f, qboolean unique ) {
	if ( fileOpen ) {
		Fail( "file opened twice" );
	}
	if ( !fileData || ( opens && failReopen ) ) {
		*f = 0;
		return -1;
	}
	opens++;
	fileOpen = 1;
	filePos = 0;
	*f = 1;
	return fileLen;
}
void FS_FCloseFile( fileHandle_t f ) {
	CHECK( f == 1 && fileOpen, "close of a file not open" );
	fileOpen = 0;
	closes++;
}
int FS_Read( void *buffer, int len, fileHandle_t f ) {
	int n = fileLen - filePos;
	if ( !f ) {
		return 0;
	}
	CHECK( f == 1 && fileOpen && len >= 0, "read" );
	if ( n > len ) {
		n = len;
	}
	memcpy( buffer, fileData + filePos, n );
	filePos += n;
	return n;
}
void Sys_BeginStreamedFile( fileHandle_t f, int readahead ) {}
void Sys_EndStreamedFile( fileHandle_t f ) {}
int Sys_StreamedRead( void *buffer, int size, int count, fileHandle_t f ) {
	CHECK( size > 0 && count > 0, "streamed read size" );
	return FS_Read( buffer, size * count, f ) / size;
}

/* every pixel handed to the renderer is read, so ASan checks it */
static byte sink[512 * 512 * 4];
static void Touch( const byte *p, int n ) {
	CHECK( n >= 0 && n <= (int)sizeof( sink ), "renderer read size" );
	memcpy( sink, p, n );       // ASan checks the whole range
}
static void StubDrawStretchRaw( int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty ) {
	Touch( data, cols * rows * 4 );
}
static void StubUploadCinematic( int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty ) {
	Touch( data, cols * rows * 4 );
}

/* ---------------------------------------------------------------- digests and audio */

static uint64_t Fnv( uint64_t h, const void *p, size_t n ) {
	const byte *b = p;
	while ( n-- ) {
		h = ( h ^ *b++ ) * 1099511628211ULL;
	}
	return h;
}
#define FNV0 14695981039346656037ULL

typedef struct { int samples, channels; uint64_t digest; } audio_t;
static audio_t expectAudio[4096];
static int numExpectAudio, numAudio, checkAudio, golden;

void S_RawSamples( int samples, int rate, int width, int channels, const byte *data, float lvol, float rvol, int streamingIndex ) {
	uint64_t d;
	CHECK( samples >= 0 && rate == 22050 && width == 2 && ( channels == 1 || channels == 2 ), "audio format" );
	d = Fnv( FNV0, data, (size_t)samples * channels * 2 );
	if ( golden ) {
		printf( "audio %d %d %016llx\n", samples, channels, (unsigned long long)d );
	}
	if ( checkAudio ) {
		CHECK( numAudio < numExpectAudio, "more audio than encoded" );
		CHECK( expectAudio[numAudio].samples == samples && expectAudio[numAudio].channels == channels, "audio count" );
		CHECK( expectAudio[numAudio].digest == d, "audio samples differ from the model" );
	}
	numAudio++;
}

/* ---------------------------------------------------------------- RoQ writer */

typedef struct { byte *d; int len, cap; } out_t;

static void Put( out_t *o, const void *p, int n ) {
	if ( o->len + n > o->cap ) {
		o->cap = ( o->len + n ) * 2 + 1024;
		o->d = realloc( o->d, o->cap );
		CHECK( o->d, "alloc" );
	}
	memcpy( o->d + o->len, p, n );
	o->len += n;
}
static void Put8( out_t *o, int v ) { byte b = v; Put( o, &b, 1 ); }
static void Put16( out_t *o, int v ) { Put8( o, v & 255 ); Put8( o, ( v >> 8 ) & 255 ); }
static void Header( out_t *o, int id, unsigned int size, int flags ) {
	Put16( o, id ); Put16( o, size & 0xffff ); Put16( o, size >> 16 ); Put16( o, flags );
}
static int Begin( out_t *o, int id, int flags ) { int at = o->len; Header( o, id, 0, flags ); return at; }
static void End( out_t *o, int at ) {
	unsigned int size = o->len - at - 8;
	o->d[at + 2] = size; o->d[at + 3] = size >> 8; o->d[at + 4] = size >> 16; o->d[at + 5] = size >> 24;
}
static void FileHeader( out_t *o ) { Header( o, 0x1084, 0xffffffff, 30 ); }

static unsigned int rngState = 12345;
static unsigned int Rand( void ) {
	rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
	return rngState;
}

/* ---------------------------------------------------------------- encoder and decode model */

typedef struct {
	int w, h, f;                    // f: motion unit in pixels (2 for 4:1 frames)
	unsigned int *half[2];
	unsigned int cb2[256][4], cb4[256][16], cb8[256][64];
	uint64_t frameDigest[64];
	int frames;
	/* VQ control word being filled */
	int wordAt, codes;
} model_t;

static model_t model;
static int audioDiv = 1;    // shorter audio for the truncation and mutation movies
static int sqrTab( int b ) { return b < 128 ? b * b : -( ( b - 128 ) * ( b - 128 ) ); }

static void ExpectAudio( int samples, int channels, const short *s ) {
	CHECK( numExpectAudio < (int)( sizeof( expectAudio ) / sizeof( expectAudio[0] ) ), "audio table" );
	expectAudio[numExpectAudio].samples = samples;
	expectAudio[numExpectAudio].channels = channels;
	expectAudio[numExpectAudio].digest = Fnv( FNV0, s, (size_t)samples * channels * 2 );
	numExpectAudio++;
}

static void Mono( out_t *o, int n ) {
	int flag = Rand() & 0xffff, at = Begin( o, ZA_SOUND_MONO, flag ), i, prev = flag;
	short *s = malloc( n * 4 + 4 );
	for ( i = 0; i < n; i++ ) {
		int b = Rand() & 255;
		Put8( o, b );
		prev = (short)( prev + sqrTab( b ) );
		s[i * 2] = s[i * 2 + 1] = (short)prev;
	}
	End( o, at );
	ExpectAudio( n, 1, s );         // the engine submits n samples of its doubled buffer
	free( s );
}

static void Stereo( out_t *o, int n ) {
	int flag = Rand() & 0xffff, at = Begin( o, ZA_SOUND_STEREO, flag ), i;
	int l = flag & 0xff00, r = ( flag & 0xff ) << 8;
	short *s = malloc( n * 2 + 4 );
	for ( i = 0; i < n; i += 2 ) {
		int a = Rand() & 255, b = Rand() & 255;
		Put8( o, a ); Put8( o, b );
		l = (short)( l + sqrTab( a ) ); r = (short)( r + sqrTab( b ) );
		s[i] = (short)l; s[i + 1] = (short)r;
	}
	End( o, at );
	ExpectAudio( n / 2, 2, s );
	free( s );
}

static void Codebook( out_t *o, int two, int four ) {
	int flags = ( two == 256 && four == 256 ) ? 0 : ( ( two & 255 ) << 8 ) | four;
	int at = Begin( o, ROQ_CODEBOOK, flags ), i, k, x, y;
	for ( i = 0; i < two; i++ ) {
		int yy[4], cr, cb;
		for ( k = 0; k < 4; k++ ) {
			yy[k] = Rand() & 255; Put8( o, yy[k] );
		}
		cr = Rand() & 255; cb = Rand() & 255; Put8( o, cr ); Put8( o, cb );
		for ( k = 0; k < 4; k++ ) {
			model.cb2[i][k] = yuv_to_rgb24( yy[k], cr, cb );
		}
	}
	for ( i = 0; i < four; i++ ) {
		int q[4];
		for ( k = 0; k < 4; k++ ) {
			q[k] = Rand() & 255; Put8( o, q[k] );
		}
		/* 2x2 entries a b over c d; each 2x2 is p0 p1 over p2 p3 */
		for ( y = 0; y < 4; y++ ) {
			for ( x = 0; x < 4; x++ ) {
				model.cb4[i][y * 4 + x] = model.cb2[q[( y >> 1 ) * 2 + ( x >> 1 )]][( y & 1 ) * 2 + ( x & 1 )];
			}
		}
		for ( y = 0; y < 8; y++ ) {
			for ( x = 0; x < 8; x++ ) {
				model.cb8[i][y * 8 + x] = model.cb4[i][( y >> 1 ) * 4 + ( x >> 1 )];
			}
		}
	}
	End( o, at );
}

static void Code( out_t *o, int code ) {
	int word;
	if ( model.codes == 8 ) {
		model.codes = 0;
	}
	if ( !model.codes ) {
		model.wordAt = o->len;
		Put16( o, 0 );
	}
	word = o->d[model.wordAt] | ( o->d[model.wordAt + 1] << 8 );
	word |= code << ( 14 - 2 * model.codes );
	o->d[model.wordAt] = word; o->d[model.wordAt + 1] = word >> 8;
	model.codes++;
}

static void PutBlock( unsigned int *dst, int x, int y, const unsigned int *src, int n ) {
	int i, j;
	for ( j = 0; j < n; j++ ) {
		for ( i = 0; i < n; i++ ) {
			dst[( y + j ) * model.w + x + i] = src[j * n + i];
		}
	}
}

/* a motion code whose source block lies inside the frame, or -1 */
static int Motion( int x, int y, int n, int xoff, int yoff, int *sx, int *sy ) {
	int tries;
	for ( tries = 0; tries < 16; tries++ ) {
		int mx = Rand() & 15, my = Rand() & 15;
		int dx = mx + xoff - 8, dy = my + yoff - 8;
		*sx = x - model.f * dx; *sy = y - model.f * dy;
		if ( *sx >= 0 && *sy >= 0 && *sx + n <= model.w && *sy + n <= model.h ) {
			return mx * 16 + my;
		}
	}
	return -1;
}

static void Move( unsigned int *dst, const unsigned int *src, int x, int y, int sx, int sy, int n ) {
	int i, j;
	for ( j = 0; j < n; j++ ) {
		for ( i = 0; i < n; i++ ) {
			dst[( y + j ) * model.w + x + i] = src[( sy + j ) * model.w + sx + i];
		}
	}
}

static void Frame( out_t *o ) {
	int q = model.frames, cur = q & 1;
	unsigned int *d = model.half[cur], *s = model.half[cur ^ 1];
	int xoff = (int)( Rand() % 5 ) - 2, yoff = (int)( Rand() % 5 ) - 2;
	int at = Begin( o, ROQ_QUAD_VQ, ( ( xoff & 255 ) << 8 ) | ( yoff & 255 ) );
	int tx, ty, g, k, sx, sy, m;

	model.codes = 0;
	for ( ty = 0; ty < model.h; ty += 16 ) {
		for ( tx = 0; tx < model.w; tx += 16 ) {
			for ( g = 0; g < 4; g++ ) {
				int x = tx + ( g & 1 ) * 8, y = ty + ( g >> 1 ) * 8, r = Rand() % 100;
				if ( x + 8 > model.w || y + 8 > model.h ) {
					continue;
				}
				if ( r < 15 && q ) {
					Code( o, 0 );                                       // skip
				} else if ( r < 35 && q && ( m = Motion( x, y, 8, xoff, yoff, &sx, &sy ) ) >= 0 ) {
					Code( o, 1 ); Put8( o, m );
					Move( d, s, x, y, sx, sy, 8 );
				} else if ( r < 60 ) {
					int e = Rand() & 255;
					Code( o, 2 ); Put8( o, e );
					PutBlock( d, x, y, model.cb8[e], 8 );
				} else {
					Code( o, 3 );
					for ( k = 0; k < 4; k++ ) {
						int bx = x + ( k & 1 ) * 4, by = y + ( k >> 1 ) * 4, r2 = Rand() % 4, e, c;
						if ( r2 == 0 && !q ) {
							r2 = 2;
						}
						if ( r2 == 1 && ( !q || ( m = Motion( bx, by, 4, xoff, yoff, &sx, &sy ) ) < 0 ) ) {
							r2 = 2;
						}
						switch ( r2 ) {
						case 0:
							Code( o, 0 );
							break;
						case 1:
							Code( o, 1 ); Put8( o, m );
							Move( d, s, bx, by, sx, sy, 4 );
							break;
						case 2:
							e = Rand() & 255;
							Code( o, 2 ); Put8( o, e );
							PutBlock( d, bx, by, model.cb4[e], 4 );
							break;
						default:
							Code( o, 3 );
							for ( c = 0; c < 4; c++ ) {
								unsigned int p[4];
								e = Rand() & 255;
								Put8( o, e );
								memcpy( p, model.cb2[e], sizeof( p ) );
								PutBlock( d, bx + ( c & 1 ) * 2, by + ( c >> 1 ) * 2, p, 2 );
							}
							break;
						}
					}
				}
			}
		}
	}
	End( o, at );
	if ( q == 0 ) {
		memcpy( model.half[1], model.half[0], model.w * model.h * 4 );
	}
	CHECK( q < 64, "too many frames" );
	model.frameDigest[q] = Fnv( FNV0, d, model.w * model.h * 4 );
	model.frames++;
}

/* A complete movie: info, a full codebook, audio, frames with partial codebooks,
 * audio in packets, and a trailing empty chunk (the engine never plays the last chunk). */
static void Movie( out_t *o, int w, int h, int frames, unsigned int seed ) {
	int i, at;

	rngState = seed;
	memset( o, 0, sizeof( *o ) );
	free( model.half[0] ); free( model.half[1] );
	memset( &model, 0, sizeof( model ) );
	model.w = w; model.h = h; model.f = ( w == h * 4 ) ? 2 : 1;
	model.half[0] = calloc( w * h, 4 ); model.half[1] = calloc( w * h, 4 );
	numExpectAudio = 0;
	ROQ_GenYUVTables();

	FileHeader( o );
	at = Begin( o, ROQ_QUAD_INFO, 0 );
	Put16( o, w ); Put16( o, h ); Put16( o, 8 ); Put16( o, 4 );
	End( o, at );
	Codebook( o, 256, 256 );
	Stereo( o, 1470 / audioDiv & ~1 );
	for ( i = 0; i < frames; i++ ) {
		if ( i % 3 == 2 ) {
			Codebook( o, 1 + Rand() % 255, 1 + Rand() % 255 );
		}
		if ( i % 2 ) {
			Mono( o, 735 / audioDiv );
		} else {
			at = Begin( o, ROQ_PACKET, 2 );
			Mono( o, ( 100 + Rand() % 300 ) / audioDiv );
			Stereo( o, 2 * ( ( 50 + Rand() % 300 ) / audioDiv ) );
			End( o, at );
		}
		Frame( o );
	}
	Header( o, ROQ_QUAD_JPEG, 0, 0 );
}

/* ---------------------------------------------------------------- playing */

typedef struct { int handle, frames, calls, audio; e_status status; } play_t;

static void ResetAll( void ) {
	// CIN_PlayCinematic clears cin itself
	memset( cinTable, 0, sizeof( cinTable ) );
	currentHandle = -1;
	CL_handle = -1;
	fileOpen = 0;
	opens = closes = failReopen = 0;
	numAudio = 0;
}

static void SetFile( const byte *d, int len ) {
	free( fileData );
	fileData = malloc( len ? len : 1 );
	CHECK( fileData, "alloc" );
	memcpy( fileData, d, len );
	fileLen = len;
}

/* Play to the end, stepping the clock one frame per CIN_RunCinematic call, and draw
 * and upload each frame. With checkFrames, every frame must equal the model's. */
static play_t Play( int bits, int maxCalls, int checkFrames ) {
	play_t r;
	cin_cache *c;
	long last = -1;

	memset( &r, 0, sizeof( r ) );
	r.handle = CIN_PlayCinematic( "test.roq", 0, 0, 640, 480, bits );
	if ( r.handle < 0 ) {
		r.status = FMV_EOF;
		return r;
	}
	c = &cinTable[r.handle];
	while ( r.calls < maxCalls ) {
		nowMs = c->startTime + ( 100 * ( c->numQuads + 1 ) + 2 ) / 3;
		r.status = CIN_RunCinematic( r.handle );
		r.calls++;
		if ( c->buf && c->numQuads > 0 && c->numQuads != last && c->status != FMV_IDLE ) {
			int n = c->numQuads - 1;
			last = c->numQuads;
			if ( golden ) {
				printf( "frame %d %016llx\n", n, (unsigned long long)Fnv( FNV0, c->buf, c->xsize * c->ysize * 4 ) );
			}
			if ( checkFrames ) {
				CHECK( n == r.frames, "a frame was skipped" );
				CHECK( n < model.frames, "more frames than encoded" );
				CHECK( Fnv( FNV0, c->buf, model.w * model.h * 4 ) == model.frameDigest[n], "frame pixels differ from the model" );
			}
			r.frames++;
		}
		CIN_DrawCinematic( r.handle );
		CIN_UploadCinematic( r.handle );
		if ( !c->fileName[0] || c->status == FMV_EOF || c->status == FMV_IDLE ) {
			break;
		}
	}
	r.audio = numAudio;
	CIN_StopCinematic( r.handle );
	return r;
}

static play_t PlayBytes( const byte *d, int len, int bits, int maxCalls ) {
	ResetAll();
	SetFile( d, len );
	return Play( bits, maxCalls, 0 );
}

/* refused at open: no handle, the file closed, the slot free */
static void ExpectOpenRefused( const byte *d, int len, const char *what ) {
	play_t r = PlayBytes( d, len, CIN_loop, 10 );
	if ( r.handle >= 0 || fileOpen || cinTable[0].fileName[0] ) {
		Fail( what );
	}
}

/* plays, then stops: not looped, the file closed and the slot freed */
static play_t ExpectStopped( const byte *d, int len, int bits, const char *what ) {
	play_t r = PlayBytes( d, len, bits, 50 );
	if ( r.handle < 0 || opens != 1 || fileOpen || cinTable[r.handle].fileName[0] ) {
		fprintf( stderr, "handle %d opens %d open %d name '%s'\n", r.handle, opens, fileOpen, r.handle >= 0 ? cinTable[r.handle].fileName : "" );
		Fail( what );
	}
	return r;
}

/* ---------------------------------------------------------------- valid movies */

static void ValidMovies( void ) {
	static const int sizes[][3] = {
		{ 64, 64, 8 }, { 24, 40, 6 }, { 128, 32, 6 }, { 8, 8, 4 }, { 256, 256, 4 },
		{ 512, 256, 3 }, { 512, 512, 3 }, { 16, 512, 3 }, { 512, 8, 3 },
	};
	int i, sw;
	out_t o;

	for ( sw = 0; sw < 2; sw++ ) {
		swapPixels = sw;
		for ( i = 0; i < (int)( sizeof( sizes ) / sizeof( sizes[0] ) ); i++ ) {
			play_t r;
			Movie( &o, sizes[i][0], sizes[i][1], sizes[i][2], 777 + i );
			ResetAll();
			SetFile( o.d, o.len );
			checkAudio = 1;
			glConfig.maxTextureSize = ( i & 1 ) ? 256 : 2048;   // also the 256 resampling path
			r = Play( 0, 1000, 1 );
			checkAudio = 0;
			if ( r.frames != model.frames || numAudio != numExpectAudio ) {
				fprintf( stderr, "%dx%d: %d of %d frames, %d of %d audio\n", sizes[i][0], sizes[i][1], r.frames, model.frames, numAudio, numExpectAudio );
				Fail( "valid movie not fully decoded" );
			}
			CHECK( opens == 1 && !fileOpen, "valid movie left its file open" );
			if ( !sw && !golden ) {
				printf( "valid %dx%d: %d frames, %d audio chunks match the model\n", sizes[i][0], sizes[i][1], r.frames, numAudio );
			}
			free( o.d );
		}
	}
	swapPixels = 0;
	glConfig.maxTextureSize = 2048;

	/* looping plays it again from the start */
	Movie( &o, 32, 32, 4, 99 );
	{
		play_t r = PlayBytes( o.d, o.len, CIN_loop, 30 );
		CHECK( opens >= 2 && r.frames >= 5, "valid movie did not loop" );
	}
	free( o.d );
}

static void Golden( void ) {
	static const int sizes[][3] = { { 64, 64, 8 }, { 24, 40, 6 }, { 128, 32, 6 }, { 512, 512, 3 } };
	int i;
	out_t o;
	golden = 1;
	for ( i = 0; i < 4; i++ ) {
		Movie( &o, sizes[i][0], sizes[i][1], sizes[i][2], 777 + i );
		printf( "movie %dx%d\n", sizes[i][0], sizes[i][1] );
		PlayBytes( o.d, o.len, 0, 1000 );
		free( o.d );
	}
	golden = 0;
}

/* ---------------------------------------------------------------- hostile cases */

/* header, QUAD_INFO and a full codebook, so that VQ can follow */
static void Start( out_t *o, int w, int h ) {
	int at;
	memset( o, 0, sizeof( *o ) );
	free( model.half[0] ); free( model.half[1] );
	memset( &model, 0, sizeof( model ) );
	FileHeader( o );
	at = Begin( o, ROQ_QUAD_INFO, 0 );
	Put16( o, w ); Put16( o, h ); Put16( o, 8 ); Put16( o, 4 );
	End( o, at );
	at = Begin( o, ROQ_CODEBOOK, 0 );
	for ( int i = 0; i < 2560; i++ ) {
		Put8( o, Rand() );
	}
	End( o, at );
}
static void Finish( out_t *o ) {
	Header( o, ROQ_QUAD_JPEG, 0, 0 );
	Header( o, ROQ_QUAD_JPEG, 0, 0 );
}
static void Filler( out_t *o, unsigned int n ) {
	while ( n-- ) {
		Put8( o, 0x11 );
	}
}

static void CaseOversizedFirst( void ) {
	static const unsigned int sizes[] = { 65529, 65536, 65537, 0xffffff };
	int i;
	for ( i = 0; i < 4; i++ ) {
		out_t o = { 0 };
		FileHeader( &o );
		Header( &o, ROQ_QUAD_HANG, sizes[i], 0 );
		Filler( &o, sizes[i] < 70000 ? sizes[i] + 16 : 16 );
		ExpectOpenRefused( o.d, o.len, "oversized first chunk accepted" );
		free( o.d );
	}
	{
		out_t o = { 0 };           // the largest that fits is accepted
		FileHeader( &o );
		Header( &o, ROQ_QUAD_HANG, 65528, 0 );
		Filler( &o, 65528 );
		Finish( &o );
		ExpectStopped( o.d, o.len, 0, "65528-byte chunk" );
		free( o.d );
	}
}

static void CaseOversizedLater( void ) {
	static const unsigned int sizes[] = { 65529, 65536, 65537, 0xffffff };
	int i, bits;
	for ( i = 0; i < 4; i++ ) {
		for ( bits = 0; bits <= CIN_loop; bits += CIN_loop ) {
			out_t o;
			Start( &o, 16, 16 );
			Header( &o, ZA_SOUND_MONO, sizes[i], 0 );
			Filler( &o, sizes[i] < 70000 ? sizes[i] + 16 : 64 );
			ExpectStopped( o.d, o.len, bits, "oversized chunk played or looped" );
			free( o.d );
		}
	}
}

static void CaseDimensions( void ) {
	static const int dims[][2] = {
		{ 0, 16 }, { 16, 0 }, { 12, 16 }, { 16, 20 }, { 1, 1 }, { 4, 4 }, { 520, 8 }, { 8, 520 },
		{ 1024, 256 }, { 8, 32768 }, { 32768, 8 }, { 65535, 65535 }, { 65528, 8 }, { 640, 480 },
	};
	int i;
	for ( i = 0; i < (int)( sizeof( dims ) / sizeof( dims[0] ) ); i++ ) {
		out_t o;
		Start( &o, dims[i][0], dims[i][1] );
		Header( &o, ROQ_QUAD_VQ, 2, 0 );
		Put16( &o, 0 );
		Finish( &o );
		ExpectStopped( o.d, o.len, CIN_loop, "bad frame size accepted" );
		CHECK( !cinTable[0].buf, "a frame was decoded at a bad size" );
		free( o.d );
	}
	{
		out_t o;                   // a short QUAD_INFO
		memset( &o, 0, sizeof( o ) );
		FileHeader( &o );
		Header( &o, ROQ_QUAD_INFO, 4, 0 );
		Put16( &o, 16 ); Put16( &o, 16 );
		Header( &o, ROQ_QUAD_VQ, 2, 0 );
		Put16( &o, 0 );
		Finish( &o );
		ExpectStopped( o.d, o.len, 0, "short quad info accepted" );
		free( o.d );
	}
}

static void CaseVqBeforeInfo( void ) {
	out_t o = { 0 };
	int at;
	FileHeader( &o );
	at = Begin( &o, ROQ_CODEBOOK, 0 );
	Filler( &o, 2560 );
	End( &o, at );
	Header( &o, ROQ_QUAD_VQ, 64, 0 );
	Filler( &o, 64 );
	Finish( &o );
	ExpectStopped( o.d, o.len, CIN_loop, "VQ before quad info" );
	CHECK( !cinTable[0].buf, "VQ before quad info decoded" );
	free( o.d );

	/* another movie opened in between clears the shared quad tables */
	{
		out_t a, b = { 0 };
		int h;
		Start( &a, 16, 16 );
		Header( &a, ROQ_QUAD_VQ, 2, 0 );
		Put16( &a, 0x0000 );
		for ( int i = 0; i < 8; i++ ) {
			Header( &a, ROQ_QUAD_VQ, 2, 0 );
			Put16( &a, 0 );
		}
		Finish( &a );
		ResetAll();
		SetFile( a.d, a.len );
		h = CIN_PlayCinematic( "a.roq", 0, 0, 64, 64, 0 );
		CHECK( h == 0, "open a" );
		nowMs = cinTable[h].startTime;
		CIN_RunCinematic( h );
		nowMs += 34;
		CIN_RunCinematic( h );
		CHECK( cinTable[h].numQuads >= 1, "a decoded" );
		FileHeader( &b );          // a second movie that fails to open
		Header( &b, ROQ_QUAD_HANG, 65536, 0 );
		fileOpen = 0;              // the stub serves one file; keep a's position aside
		{
			byte *keep = fileData; int keepLen = fileLen, keepPos = filePos;
			fileData = NULL;
			SetFile( b.d, b.len );
			CHECK( CIN_PlayCinematic( "b.roq", 0, 0, 64, 64, 0 ) < 0, "b opened" );
			free( fileData );
			fileData = keep; fileLen = keepLen; filePos = keepPos;
		}
		fileOpen = 1;
		nowMs += 34;
		CIN_RunCinematic( h );      // must restart a (fresh quad tables), not blit through NULL
		nowMs += 340;
		CIN_RunCinematic( h );
		CIN_StopCinematic( h );
		free( a.d ); free( b.d );
	}
}

static void CaseAudio( void ) {
	static const struct { int id; unsigned int size; int plays; } c[] = {
		{ ZA_SOUND_MONO, 16384, 1 }, { ZA_SOUND_MONO, 16385, 0 }, { ZA_SOUND_MONO, 65528, 0 },
		{ ZA_SOUND_STEREO, 32768, 1 }, { ZA_SOUND_STEREO, 32770, 0 }, { ZA_SOUND_STEREO, 65528, 0 },
		{ ZA_SOUND_STEREO, 33, 0 }, { ZA_SOUND_STEREO, 1, 0 }, { ZA_SOUND_MONO, 0, 1 },
	};
	int i;
	for ( i = 0; i < (int)( sizeof( c ) / sizeof( c[0] ) ); i++ ) {
		out_t o = { 0 };
		play_t r;
		FileHeader( &o );
		Header( &o, c[i].id, c[i].size, 0 );
		Filler( &o, c[i].size );
		Finish( &o );
		r = PlayBytes( o.d, o.len, 0, 20 );
		if ( r.audio != c[i].plays ) {
			fprintf( stderr, "audio id %x size %u: %d chunks played\n", c[i].id, c[i].size, r.audio );
			Fail( "oversized audio not skipped, or valid audio skipped" );
		}
		free( o.d );
	}
}

static void CaseCodebook( void ) {
	int len, flags;
	static const int flagList[] = { 0, 0x0101, 0x00ff, 0xff00, 0x2010 };
	for ( flags = 0; flags < 5; flags++ ) {
		int two = flagList[flags] ? ( ( flagList[flags] >> 8 ) ? ( flagList[flags] >> 8 ) : 256 ) : 256;
		int four = flagList[flags] ? ( flagList[flags] & 255 ) : 256;
		int need = two * 6 + four * 4;
		for ( len = 0; len <= need; len += ( len < 16 || need - len < 16 ) ? 1 : 97 ) {
			out_t o = { 0 };
			play_t r;
			int at;
			FileHeader( &o );
			at = Begin( &o, ROQ_CODEBOOK, flagList[flags] );
			Filler( &o, len );
			End( &o, at );
			Finish( &o );
			r = PlayBytes( o.d, o.len, CIN_loop, 20 );
			if ( len < need && !( flagList[flags] == 0 && len >= 1536 ) ) {
				CHECK( r.handle >= 0 && !fileOpen && opens == 1, "short codebook accepted" );
			}
			free( o.d );
		}
	}
	/* Q3A #247: a 1536-byte default codebook is 2x2 entries only, and plays */
	{
		out_t o = { 0 };
		int at;
		unsigned short before = vq4[0];
		FileHeader( &o );
		at = Begin( &o, ROQ_CODEBOOK, 0 );
		Filler( &o, 1536 );
		End( &o, at );
		Header( &o, ZA_SOUND_MONO, 4, 0 );
		Filler( &o, 4 );
		Finish( &o );
		CHECK( PlayBytes( o.d, o.len, 0, 20 ).audio == 1, "2x2-only codebook stopped the movie" );
		CHECK( vq4[0] == before, "2x2-only codebook changed 4x4 entries" );
		free( o.d );
	}
}

/* VQ data that ends early, at every length */
static void CaseVqTruncated( void ) {
	out_t full;
	int n, at, k;
	Start( &full, 32, 32 );
	at = Begin( &full, ROQ_QUAD_VQ, 0 );
	/* 16 groups, all subdivided into 2x2: the most data a group can take */
	for ( k = 0; k < 16; k++ ) {
		Code( &full, 3 );
		for ( int j = 0; j < 4; j++ ) {
			Code( &full, 3 );
			Put8( &full, 1 ); Put8( &full, 2 ); Put8( &full, 3 ); Put8( &full, 4 );
		}
	}
	End( &full, at );
	n = full.len - at - 8;
	for ( k = 0; k <= n; k++ ) {
		out_t o = { 0 };
		play_t r;
		Put( &o, full.d, at + 8 + k );
		o.d[at + 2] = k; o.d[at + 3] = k >> 8;
		Finish( &o );
		r = PlayBytes( o.d, o.len, CIN_loop, 20 );
		if ( k < n ) {
			CHECK( !cinTable[0].fileName[0] && opens == 1 && !fileOpen, "truncated VQ data accepted" );
		} else {
			CHECK( r.frames == 1, "complete VQ frame refused" );
		}
		free( o.d );
	}
	free( full.d );
}

/* motion sources past either end of linbuf. A source outside its frame half but inside
 * linbuf is still accepted: it is memory-safe, and the original decoded it the same way. */
static void CaseMotion( void ) {
	static const struct { int w, h, xoff, yoff, motion; } c[] = {
		{ 512, 512, -128, -128, 0x00 },     // source below the second half
		{ 512, 512, 127, 127, 0xff },       // source above the first half, on frame 2
		{ 512, 128, 127, 127, 0xff },       // 4:1 doubles the offsets
	};
	int i;
	for ( i = 0; i < 3; i++ ) {
		out_t o;
		int frames, g, groups = c[i].w * c[i].h / 64;
		Start( &o, c[i].w, c[i].h );
		for ( frames = 0; frames < 3; frames++ ) {
			int at = Begin( &o, ROQ_QUAD_VQ, ( ( c[i].xoff & 255 ) << 8 ) | ( c[i].yoff & 255 ) );
			model.codes = 0;
			for ( g = 0; g < groups; g++ ) {
				Code( &o, frames ? 1 : 0 );     // every block moved
				if ( frames ) {
					Put8( &o, c[i].motion );
				}
			}
			End( &o, at );
		}
		Finish( &o );
		ExpectStopped( o.d, o.len, CIN_loop, "motion outside linbuf accepted" );
		free( o.d );
	}
}

static void CasePackets( void ) {
	out_t o = { 0 };
	int at;
	/* a child larger than the packet */
	FileHeader( &o );
	at = Begin( &o, ROQ_PACKET, 1 );
	Header( &o, ZA_SOUND_MONO, 60000, 0 );
	Filler( &o, 16 );
	End( &o, at );
	Finish( &o );
	ExpectStopped( o.d, o.len, CIN_loop, "packet child past the data accepted" );
	free( o.d );

	/* more children declared than present */
	memset( &o, 0, sizeof( o ) );
	FileHeader( &o );
	at = Begin( &o, ROQ_PACKET, 60000 );
	Header( &o, ZA_SOUND_MONO, 4, 0 );
	Filler( &o, 4 );
	End( &o, at );
	ExpectStopped( o.d, o.len, CIN_loop, "packet child count past the data accepted" );
	free( o.d );

	/* nested packets of maximum size, each child walking further */
	memset( &o, 0, sizeof( o ) );
	FileHeader( &o );
	at = Begin( &o, ROQ_PACKET, 2 );
	for ( int i = 0; i < 8000; i++ ) {
		Header( &o, ROQ_PACKET, 0, 0xffff );
	}
	End( &o, at );
	ExpectStopped( o.d, o.len, CIN_loop, "nested packets" );
	free( o.d );
}

static void CaseReopen( void ) {
	out_t o;
	play_t r;
	Movie( &o, 16, 16, 2, 5 );
	ResetAll();
	SetFile( o.d, o.len );
	failReopen = 1;
	r = Play( CIN_loop, 50, 0 );
	CHECK( r.frames == 2 && !fileOpen && !cinTable[r.handle].fileName[0], "failed reopen" );
	free( o.d );
}

static void Truncations( void ) {
	out_t o;
	int len, bits;
	audioDiv = 50;
	Movie( &o, 24, 16, 4, 31 );
	audioDiv = 1;
	for ( len = 0; len <= o.len; len++ ) {
		for ( bits = 0; bits <= CIN_loop; bits += CIN_loop ) {
			PlayBytes( o.d, len, bits, 40 );
			if ( !bits ) {
				if ( fileOpen ) {
					fprintf( stderr, "length %d of %d\n", len, o.len );
				}
				CHECK( !fileOpen, "truncated movie left its file open" );
			}
		}
	}
	printf( "truncation: %d lengths, with and without looping\n", o.len + 1 );
	free( o.d );
}

static void Mutations( int count ) {
	out_t base[3];
	int i, k;
	audioDiv = 20;
	Movie( &base[0], 32, 32, 4, 41 );
	Movie( &base[1], 24, 16, 3, 42 );
	Movie( &base[2], 512, 512, 2, 43 );
	audioDiv = 1;
	rngState = 2024;
	for ( i = 0; i < count; i++ ) {
		out_t *b = &base[( i % 50 ) == 0 ? 2 : i & 1];
		byte *m = malloc( b->len );
		int len = b->len, ops = 1 + Rand() % 8;
		memcpy( m, b->d, len );
		for ( k = 0; k < ops; k++ ) {
			int at = Rand() % len, r = Rand() % 10;
			if ( r < 5 ) {
				m[at] = Rand();
			} else if ( r < 7 ) {
				m[at] ^= 1 << ( Rand() % 8 );
			} else if ( r < 9 ) {   // a size field to a boundary value
				static const unsigned int v[] = { 0, 1, 7, 8, 16384, 16385, 32768, 32769, 65528, 65529, 65536, 0xffffff };
				unsigned int s = v[Rand() % 12];
				if ( at + 3 < len ) {
					m[at] = s; m[at + 1] = s >> 8; m[at + 2] = s >> 16;
				}
			} else {
				len = 1 + Rand() % len;
			}
		}
		PlayBytes( m, len, ( Rand() & 1 ) ? CIN_loop : 0, 60 );
		free( m );
	}
	printf( "mutations: %d\n", count );
	for ( k = 0; k < 3; k++ ) {
		free( base[k].d );
	}
}

static const struct { const char *name; void ( *fn )( void ); } cases[] = {
	{ "oversized-first", CaseOversizedFirst },
	{ "oversized-later", CaseOversizedLater },
	{ "dimensions", CaseDimensions },
	{ "vq-before-info", CaseVqBeforeInfo },
	{ "audio", CaseAudio },
	{ "codebook", CaseCodebook },
	{ "vq-truncated", CaseVqTruncated },
	{ "motion", CaseMotion },
	{ "packets", CasePackets },
	{ "reopen", CaseReopen },
};

int main( int argc, char **argv ) {
	int i;

	timescaleVar.value = 1;
	inGameVideoVar.integer = 1;
	glConfig.maxTextureSize = 2048;
	re.DrawStretchRaw = StubDrawStretchRaw;
	re.UploadCinematic = StubUploadCinematic;

	if ( argc > 1 && !strcmp( argv[1], "golden" ) ) {
		Golden();
		return 0;
	}
	if ( argc > 2 && !strcmp( argv[1], "case" ) ) {
		for ( i = 0; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ); i++ ) {
			if ( !strcmp( argv[2], cases[i].name ) ) {
				cases[i].fn();
				printf( "ok: %s\n", argv[2] );
				return 0;
			}
		}
		return 2;
	}
	ValidMovies();
	for ( i = 0; i < (int)( sizeof( cases ) / sizeof( cases[0] ) ); i++ ) {
		cases[i].fn();
		printf( "ok: %s\n", cases[i].name );
	}
	Truncations();
	Mutations( argc > 1 ? atoi( argv[1] ) : 5000 );
	printf( "cin: all passed\n" );
	return 0;
}
