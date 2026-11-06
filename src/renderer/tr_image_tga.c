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
#include <limits.h>

extern refimport_t ri;

/** Validate the raw/RLE packets that fill the image and decode exactly its pixels in native row order. */
static qboolean R_TGAPixels( imageCursor_t *cursor, unsigned int columns, unsigned int rows,
                            unsigned int bytesPerPixel, qboolean rle, byte *output ) {
	unsigned int remaining = columns * rows, count, i, row = rows - 1, column = 0;
	const byte *packet, *source, *pixel;
	qboolean repeat;
	byte *target = output ? output + row * columns * 4 : NULL;
	while ( remaining ) {
		count = remaining; repeat = qfalse;
		if ( rle ) {
			if ( !R_ImageBytes(cursor, 1, &packet) ) return qfalse;
			count = (*packet & 0x7f) + 1; repeat = (*packet & 0x80) != 0;
			// a packet that runs past the last pixel ends the image, as retail
			if ( count > remaining ) count = remaining;
		}
		if ( !R_ImageBytes(cursor, (repeat ? 1 : count) * bytesPerPixel, &source) ) return qfalse;
		if ( output ) {
			for ( i = 0; i < count; i++ ) {
				pixel = source + (repeat ? 0 : i * bytesPerPixel);
				if ( bytesPerPixel == 1 ) target[0] = target[1] = target[2] = pixel[0];
				else { target[0] = pixel[2]; target[1] = pixel[1]; target[2] = pixel[0]; }
				target[3] = bytesPerPixel == 4 ? pixel[3] : 255;
				target += 4;
				if ( ++column == columns ) {
					column = 0;
					if ( row ) { row--; target = output + row * columns * 4; }
				}
			}
		}
		remaining -= count;
	}
	return qtrue;
}

/** Check header/ID, allocation arithmetic and all payload bytes before allocating RGBA. */
static qboolean R_DecodeTGA( const byte *buffer, unsigned int length, byte **pic,
                            int *width, int *height, qboolean *topDown ) {
	imageCursor_t cursor, preflight;
	const byte *format, *ignored, *pixelFormat;
	unsigned int columns, rows, depth;
	qboolean rle;
	byte *output;
	cursor.data = buffer; cursor.length = length; cursor.position = 0;
	if ( !R_ImageBytes(&cursor, 3, &format) || !R_ImageBytes(&cursor, 9, &ignored) ||
	     !R_ImageLE(&cursor, 2, &columns) || !R_ImageLE(&cursor, 2, &rows) ||
	     !R_ImageBytes(&cursor, 2, &pixelFormat) || !R_ImageBytes(&cursor, format[0], &ignored) ) return qfalse;
	depth = pixelFormat[0]; rle = format[2] == 10;
	if ( format[1] || (format[2] != 2 && format[2] != 3 && format[2] != 10) ||
	     (depth != 24 && depth != 32 && !(format[2] == 3 && depth == 8)) ||
	     !R_ImageSizeValid(columns, rows) ) return qfalse;
	preflight = cursor;
	if ( !R_TGAPixels(&preflight, columns, rows, depth / 8, rle, NULL) ) return qfalse;
	output = R_TryImageBuffer( columns * rows * 4 );
	if ( !output ) return qfalse;
	if ( !R_TGAPixels(&cursor, columns, rows, depth / 8, rle, output) ) return qfalse;
	*pic = output; *width = columns; *height = rows; *topDown = (pixelFormat[1] & 0x20) != 0;
	return qtrue;
}

/** Release input before errors/warnings and preserve retail Quake III's declared-origin behavior. */
void R_LoadTGA( const char *name, byte **pic, int *width, int *height ) {
	byte *buffer = NULL, *output = NULL;
	int length, columns = 0, rows = 0;
	qboolean valid, topDown = qfalse;
	*pic = NULL;
	if ( width ) *width = 0;
	if ( height ) *height = 0;
	length = ri.FS_ReadFile( name, (void **)&buffer );
	if ( !buffer ) return;
	valid = length >= 0 && R_DecodeTGA( buffer, length, &output, &columns, &rows, &topDown );
	ri.FS_FreeFile(buffer);
	if ( !valid ) { ri.Printf( PRINT_WARNING, "WARNING: LoadTGA: %s is malformed, truncated, unsupported or larger than %u pixels\n", name, R_IMAGE_MAX_PIXELS ); return; }
	if ( topDown ) ri.Printf( PRINT_WARNING, "WARNING: '%s' TGA file header declares top-down image, ignoring\n", name );
	*pic = output;
	if ( width ) *width = columns;
	if ( height ) *height = rows;
}
