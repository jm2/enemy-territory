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
#ifndef TR_IMAGE_CURSOR_H
#define TR_IMAGE_CURSOR_H

#include <limits.h>
/* Keep signed allocator header/alignment additions below INT_MAX. */
#define R_IMAGE_MAX_BYTES (INT_MAX - 4096u)
/*
 * Largest image a file may declare: 4096x4096 pixels, a 64 MiB RGBA buffer.
 * Retail art is far smaller and no Mac OS 9 era card takes more than 2048 per
 * side. ioquake3 and Quake3e bound only int overflow (0x1FFFFFFF pixels), which
 * still lets a tiny RLE file ask ri.Malloc for gigabytes, and a failed zone
 * allocation is ERR_FATAL. Loaders reject a larger size before allocating.
 */
#define R_IMAGE_MAX_PIXELS (4096u * 4096u)

/* Include q_shared.h before this private header. */
typedef struct {
	const byte *data;
	unsigned int length, position;
} imageCursor_t;

/** Accept a non-empty size within R_IMAGE_MAX_PIXELS; the division cannot overflow. */
static inline qboolean R_ImageSizeValid( unsigned int columns, unsigned int rows ) {
	return columns && rows && rows <= R_IMAGE_MAX_PIXELS / columns;
}

/** Obtain a complete byte span without overflow or unaligned native loads. */
static inline qboolean R_ImageBytes( imageCursor_t *cursor, unsigned int count, const byte **bytes ) {
	if ( cursor->position > cursor->length || count > cursor->length - cursor->position ) return qfalse;
	*bytes = cursor->data + cursor->position;
	cursor->position += count;
	return qtrue;
}

/** Decode checked two/four-byte little-endian header fields identically on PPC and hosts. */
static inline qboolean R_ImageLE( imageCursor_t *cursor, unsigned int count, unsigned int *value ) {
	const byte *bytes;
	unsigned int i, result = 0;
	if ( (count != 2 && count != 4) || !R_ImageBytes(cursor, count, &bytes) ) return qfalse;
	for ( i = 0; i < count; i++ ) result |= (unsigned int)bytes[i] << (i * 8);
	*value = result;
	return qtrue;
}

/*
 * The decoded image: tr_image.c's shared buffer, which stays valid until the next image and
 * is not freed by the caller. NULL when it cannot be had, so a texture too large for the
 * machine loads as the default image instead of dropping the map.
 */
void *R_TryImageBuffer( int size );

void R_LoadBMP( const char *name, byte **pic, int *width, int *height );
void R_LoadPCX( const char *name, byte **pic, int *width, int *height );
void R_LoadTGA( const char *name, byte **pic, int *width, int *height );
void R_LoadJPG( const char *name, byte **pic, int *width, int *height );

#endif
