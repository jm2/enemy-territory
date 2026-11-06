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

/** Validate complete Windows BI_RGB layout and decode only checked source rows into RGBA. */
static qboolean R_DecodeBMP( const byte *buffer, unsigned int length, byte **pic, int *width, int *height ) {
	imageCursor_t cursor, rowCursor;
	const byte *magic, *ignored, *palette = NULL, *pixel;
	unsigned int fileSize, offset, dibSize, columns, rawHeight, rows, planes, depth;
	unsigned int compression, imageSize, colors, stride, required, paletteCount = 0;
	unsigned int row, column, outputRow, word, channel, bytesPerPixel;
	qboolean topDown;
	byte *output, *target;
	cursor.data = buffer; cursor.length = length; cursor.position = 0;
	if ( !R_ImageBytes(&cursor, 2, &magic) || magic[0] != 'B' || magic[1] != 'M' ||
	     !R_ImageLE(&cursor, 4, &fileSize) || !R_ImageBytes(&cursor, 4, &ignored) ||
	     !R_ImageLE(&cursor, 4, &offset) || !R_ImageLE(&cursor, 4, &dibSize) ||
	     !R_ImageLE(&cursor, 4, &columns) || !R_ImageLE(&cursor, 4, &rawHeight) ||
	     !R_ImageLE(&cursor, 2, &planes) || !R_ImageLE(&cursor, 2, &depth) ||
	     !R_ImageLE(&cursor, 4, &compression) || !R_ImageLE(&cursor, 4, &imageSize) ||
	     !R_ImageBytes(&cursor, 8, &ignored) || !R_ImageLE(&cursor, 4, &colors) ||
	     !R_ImageBytes(&cursor, 4, &ignored) ) return qfalse;
	if ( fileSize != length || dibSize < 40 || !R_ImageBytes(&cursor, dibSize - 40, &ignored) ||
	     planes != 1 || compression || (depth != 8 && depth != 16 && depth != 24 && depth != 32) ) return qfalse;
	topDown = (rawHeight & 0x80000000u) != 0;
	rows = topDown ? 0u - rawHeight : rawHeight;
	if ( !R_ImageSizeValid(columns, rows) ) return qfalse;
	if ( depth == 8 ) {
		paletteCount = colors ? colors : 256;
		if ( paletteCount > 256 || !R_ImageBytes(&cursor, paletteCount * 4, &palette) ) return qfalse;
	} else if ( colors ) {
		if ( colors > (cursor.length - cursor.position) / 4 || !R_ImageBytes(&cursor, colors * 4, &ignored) ) return qfalse;
	}
	if ( offset < cursor.position || offset > length ) return qfalse;
	bytesPerPixel = depth / 8;
	stride = (columns * bytesPerPixel + 3) & ~3u;
	if ( rows > (length - offset) / stride ) return qfalse;
	required = rows * stride;
	if ( imageSize && (imageSize < required || imageSize > length - offset) ) return qfalse;
	/* Palette indices must be valid before allocation or any output is written. */
	if ( depth == 8 ) {
		for ( row = 0; row < rows; row++ ) {
			for ( column = 0; column < columns; column++ ) {
				if ( buffer[offset + row * stride + column] >= paletteCount ) return qfalse;
			}
		}
	}
	output = R_TryImageBuffer( columns * rows * 4 );
	if ( !output ) return qfalse;
	for ( row = 0; row < rows; row++ ) {
		rowCursor.data = buffer + offset + row * stride; rowCursor.length = stride; rowCursor.position = 0;
		outputRow = topDown ? row : rows - row - 1;
		for ( column = 0; column < columns; column++ ) {
			if ( !R_ImageBytes(&rowCursor, bytesPerPixel, &pixel) ) return qfalse;
			target = output + (outputRow * columns + column) * 4;
			if ( depth == 8 ) {
				target[0] = palette[pixel[0] * 4 + 2]; target[1] = palette[pixel[0] * 4 + 1]; target[2] = palette[pixel[0] * 4]; target[3] = 255;
			} else if ( depth == 16 ) {
				word = pixel[0] | (unsigned int)pixel[1] << 8;
				for ( channel = 0; channel < 3; channel++ ) {
					unsigned int value = (word >> (10 - channel * 5)) & 31;
					target[channel] = (value << 3) | (value >> 2);
				}
				target[3] = 255;
			} else {
				target[0] = pixel[2]; target[1] = pixel[1]; target[2] = pixel[0]; target[3] = depth == 32 ? pixel[3] : 255;
			}
		}
	}
	*pic = output;
	if ( width ) *width = columns;
	if ( height ) *height = rows;
	return qtrue;
}

/** Release file ownership before reporting malformed data; publish output only after success. */
void R_LoadBMP( const char *name, byte **pic, int *width, int *height ) {
	byte *buffer = NULL;
	int length;
	qboolean valid;
	*pic = NULL;
	if ( width ) *width = 0;
	if ( height ) *height = 0;
	length = ri.FS_ReadFile( name, (void **)&buffer );
	if ( !buffer ) return;
	valid = length >= 0 && R_DecodeBMP( buffer, length, pic, width, height );
	ri.FS_FreeFile( buffer );
	if ( !valid ) ri.Printf( PRINT_WARNING, "WARNING: LoadBMP: %s is malformed, truncated, unsupported or larger than %u pixels\n", name, R_IMAGE_MAX_PIXELS );
}
