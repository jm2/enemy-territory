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

extern refimport_t ri;

/**
 * Decode rows exactly as 1.32c's LoadPCX: every row restarts at x=0 and takes runs until it
 * reaches columns, ignoring bytes_per_line, so stream padding becomes the next row's pixels.
 * A run may overshoot its row. id wrote the excess into the following rows, which those rows
 * then overwrite, or past the last row into its Z_TagMalloc block; it is dropped here. Within
 * the block's 4-byte rounding slack retail loaded the image with these same pixels; beyond it,
 * hitting the ZONEID trash marker, the free raised ERR_FATAL. Loading those files
 * clamped is a deliberate, safe divergence. A read past the file (id's malformed check) is
 * rejected. id also accepted zero-length runs (0xc0 plus a value byte) as no-ops, as does this.
 */
static qboolean R_PCXRows( imageCursor_t *cursor, unsigned int columns, unsigned int rows,
                          const byte *palette, byte *output ) {
	unsigned int row, column, run, visible, i;
	const byte *token, *value;
	byte *pixel;
	for ( row = 0; row < rows; row++ ) {
		column = 0;
		while ( column < columns ) {
			if ( !R_ImageBytes(cursor, 1, &token) ) return qfalse;
			value = token; run = 1;
			if ( (*token & 0xc0) == 0xc0 ) {
				run = *token & 0x3f;
				if ( !R_ImageBytes(cursor, 1, &value) ) return qfalse;
			}
			if ( output ) {
				visible = run < columns - column ? run : columns - column;
				pixel = output + (row * columns + column) * 4;
				for ( i = 0; i < visible; i++, pixel += 4 ) {
					pixel[0] = palette[*value * 3]; pixel[1] = palette[*value * 3 + 1];
					pixel[2] = palette[*value * 3 + 2]; pixel[3] = 255;
				}
			}
			column += run;
		}
	}
	return qtrue;
}

/**
 * Validate the header and the whole RLE stream before allocating the single RGBA output.
 * Like 1.32c: the size is (xmax+1) x (ymax+1) with xmin, ymin, color_planes and bytes_per_line
 * ignored, either axis above 1024 is rejected, the stream may run to the end of the file and
 * the palette is the last 768 bytes with no 0x0c marker check. Files shorter than 768 bytes,
 * where id read its palette from before the buffer, are rejected.
 */
static qboolean R_DecodePCX( const byte *buffer, unsigned int length, byte **pic, int *width, int *height ) {
	imageCursor_t header, encoded, preflight;
	const byte *format, *ignored, *palette;
	unsigned int xmax, ymax, columns, rows;
	byte *output;
	if ( length < 768 ) return qfalse;
	header.data = buffer; header.length = 128; header.position = 0;
	/* xmax and ymax are read unsigned as on little-endian retail; big-endian 1.32c sign-extended
	   values from 0x8000, giving the empty or negative sizes that are rejected here. */
	if ( !R_ImageBytes(&header, 4, &format) || format[0] != 0x0a || format[1] != 5 ||
	     format[2] != 1 || format[3] != 8 || !R_ImageBytes(&header, 4, &ignored) ||
	     !R_ImageLE(&header, 2, &xmax) || !R_ImageLE(&header, 2, &ymax) ||
	     xmax >= 1024 || ymax >= 1024 ) return qfalse;
	columns = xmax + 1; rows = ymax + 1;
	palette = buffer + length - 768;
	encoded.data = buffer; encoded.length = length; encoded.position = 128;
	preflight = encoded;
	if ( !R_PCXRows(&preflight, columns, rows, palette, NULL) ) return qfalse;
	output = R_TryImageBuffer( columns * rows * 4 );
	if ( !output ) return qfalse;
	if ( !R_PCXRows(&encoded, columns, rows, palette, output) ) return qfalse;
	*pic = output;
	if ( width ) *width = columns;
	if ( height ) *height = rows;
	return qtrue;
}

/** Preserve nonfatal PCX rejection and release the file before warnings or output publication. */
void R_LoadPCX( const char *name, byte **pic, int *width, int *height ) {
	byte *buffer = NULL, *output = NULL;
	int length, columns = 0, rows = 0;
	qboolean valid;
	*pic = NULL;
	if ( width ) *width = 0;
	if ( height ) *height = 0;
	length = ri.FS_ReadFile( name, (void **)&buffer );
	if ( !buffer ) return;
	valid = length >= 0 && R_DecodePCX( buffer, length, &output, &columns, &rows );
	ri.FS_FreeFile( buffer );
	if ( !valid ) { ri.Printf( PRINT_ALL, "Bad, truncated or unsupported PCX file: %s\n", name ); return; }
	*pic = output;
	if ( width ) *width = columns;
	if ( height ) *height = rows;
}
