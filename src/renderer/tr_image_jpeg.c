/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "../game/q_shared.h"
#include "../qcommon/qcommon.h"
#include "tr_public.h"
#include "tr_image_cursor.h"
#define JPEG_INTERNALS
#include "../jpeg-6/jpeglib.h"
#include "../jpeg-6/jerror.h"
#include <setjmp.h>

extern refimport_t ri;

/*
 * Static, so nothing it holds is lost to the longjmp. id's loader ended at libjpeg's error_exit,
 * which is ERR_FATAL: one bad file in a downloaded pk3 quit the game.
 */
static struct {
	struct jpeg_decompress_struct info;
	struct jpeg_error_mgr error;
	jmp_buf jump;
	char message[JMSG_LENGTH_MAX];
} jpegLoad;

static void R_JPEGErrorExit( j_common_ptr info ) {
	( *info->err->format_message )( info, jpegLoad.message );
	longjmp( jpegLoad.jump, 1 );
}

static void R_JPEGFail( const char *message ) {
	Q_strncpyz( jpegLoad.message, message, sizeof( jpegLoad.message ) );
	longjmp( jpegLoad.jump, 1 );
}

/*
 * Decode a whole file into opaque RGBA. libjpeg here is built with RGB_PIXELSIZE 4, so an RGB
 * row is already RGBA with its alpha unset. id took grayscale as it came, one byte a pixel in a
 * buffer of that size, and then wrote alpha through four: a heap overflow. This buffer holds
 * four, and grayscale is spread out afterwards. Anything else that cannot become RGB raises an
 * error in libjpeg.
 */
void R_LoadJPG( const char *name, byte **pic, int *width, int *height ) {
	struct jpeg_decompress_struct *info = &jpegLoad.info;
	byte *input = NULL;
	byte *output;
	int length;
	unsigned int columns, rows, components, i;
	JSAMPROW row;

	*pic = NULL;
	if ( width ) *width = 0;
	if ( height ) *height = 0;
	length = ri.FS_ReadFile( name, (void **)&input );
	if ( !input ) return;

	memset( &jpegLoad, 0, sizeof( jpegLoad ) );
	info->err = jpeg_std_error( &jpegLoad.error );
	jpegLoad.error.error_exit = R_JPEGErrorExit;
	if ( setjmp( jpegLoad.jump ) ) {
		jpeg_destroy_decompress( info );
		ri.FS_FreeFile( input );
		ri.Printf( PRINT_WARNING, "WARNING: LoadJPG: %s (%s)\n", jpegLoad.message, name );
		return;
	}
	jpeg_create_decompress( info );
	jpeg_mem_src( info, input, length < 0 ? 0 : length );
	if ( jpeg_read_header( info, TRUE ) != JPEG_HEADER_OK ) R_JPEGFail( "invalid image header" );
	if ( !R_ImageSizeValid( info->image_width, info->image_height ) ) R_JPEGFail( "empty image or more than 4096x4096 pixels" );
	// this libjpeg has no multi-scan support, and says so later with less of a reason
	if ( info->inputctl->has_multiple_scans ) R_JPEGFail( "multi-scan images are not supported" );
	info->out_color_space = info->jpeg_color_space == JCS_GRAYSCALE ? JCS_GRAYSCALE : JCS_RGB;
	jpeg_start_decompress( info );
	columns = info->output_width;
	rows = info->output_height;
	components = info->out_color_space == JCS_GRAYSCALE ? 1 : 4;
	if ( info->output_components != (int)components || !R_ImageSizeValid( columns, rows ) ) R_JPEGFail( "unsupported output format" );
	output = R_TryImageBuffer( columns * rows * 4 );
	if ( !output ) R_JPEGFail( "image buffer allocation failed" );
	while ( info->output_scanline < rows ) {
		row = output + columns * components * info->output_scanline;
		if ( jpeg_read_scanlines( info, &row, 1 ) != 1 ) R_JPEGFail( "incomplete image scanline" );
	}
	for ( i = columns * rows; i-- > 0; ) {
		if ( components == 1 ) {
			output[i * 4] = output[i * 4 + 1] = output[i * 4 + 2] = output[i];
		}
		output[i * 4 + 3] = 255;
	}
	jpeg_finish_decompress( info );
	jpeg_destroy_decompress( info );
	ri.FS_FreeFile( input );
	*pic = output;
	if ( width ) *width = columns;
	if ( height ) *height = rows;
}
