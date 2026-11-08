/*
Shared collision/renderer preflight for the ET version-47 BSP file layout.

Adapted from the Quake III port (Q3A 05383846). CM_LoadMap and RE_LoadWorldMap
each read the map with FS_ReadFile and run these checks on their own buffer,
before either one changes any loader state. Every word is decoded from little-
endian bytes here, so the checks see the same values on a big-endian Mac as the
loaders do after LittleLong, and the buffer needs no alignment.
*/
#ifndef BSP_VALIDATE_H
#define BSP_VALIDATE_H

#include "../game/q_shared.h"
#include "qfiles.h"
#include <limits.h>
#include <stddef.h>

/** Decode a complete bounded file word without assuming FS input alignment. */
static unsigned int BSP_FileWord( const byte *data ) {
	return (unsigned int)data[0] | ( (unsigned int)data[1] << 8 ) |
		   ( (unsigned int)data[2] << 16 ) | ( (unsigned int)data[3] << 24 );
}

/** Check every lump before checksum, payload access, allocation or state reset. */
static const char *BSP_ValidateHeader( const void *buffer, int length, dheader_t *header ) {
	static const unsigned int strides[HEADER_LUMPS] = {
		1, sizeof( dshader_t ), sizeof( dplane_t ), sizeof( dnode_t ), sizeof( dleaf_t ),
		4, 4, sizeof( dmodel_t ), sizeof( dbrush_t ), sizeof( dbrushside_t ), sizeof( drawVert_t ),
		4, sizeof( dfog_t ), sizeof( dsurface_t ), LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3, 8, 1
	};
	const byte *data = buffer;
	dheader_t validated;
	unsigned int offsets[HEADER_LUMPS], sizes[HEADER_LUMPS], i, j, offset, size, clusters, rowBytes;

	if ( !data || !header || length < (int)sizeof( dheader_t ) || length > INT_MAX - 4096 ) {
		return "invalid/truncated BSP header";
	}
	if ( BSP_FileWord( data + offsetof( dheader_t, ident ) ) != BSP_IDENT ||
		 BSP_FileWord( data + offsetof( dheader_t, version ) ) != BSP_VERSION ) {
		return "invalid BSP identification/version";
	}
	validated.ident = BSP_IDENT;
	validated.version = BSP_VERSION;
	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		offset = BSP_FileWord( data + offsetof( dheader_t, lumps ) + i * sizeof( lump_t ) + offsetof( lump_t, fileofs ) );
		size = BSP_FileWord( data + offsetof( dheader_t, lumps ) + i * sizeof( lump_t ) + offsetof( lump_t, filelen ) );
		if ( offset > (unsigned int)length || size > (unsigned int)length - offset ||
			 ( size && offset < sizeof( dheader_t ) ) ) {
			return "BSP lump outside file";
		}
		if ( size % strides[i] ) {
			return "invalid BSP lump size";
		}
		// text, RGB lightmaps and light grid samples are read by byte; the rest by word
		if ( size && i != LUMP_ENTITIES && i != LUMP_LIGHTMAPS && i != LUMP_LIGHTGRID && offset % 4 ) {
			return "unaligned BSP lump";
		}
		offsets[i] = offset;
		sizes[i] = size;
		validated.lumps[i].fileofs = offset;
		validated.lumps[i].filelen = size;
	}
	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		for ( j = i + 1; j < HEADER_LUMPS; j++ ) {
			if ( sizes[i] && sizes[j] && offsets[i] < offsets[j] + sizes[j] && offsets[j] < offsets[i] + sizes[i] ) {
				return "overlapping BSP lumps";
			}
		}
	}
	if ( sizes[LUMP_VISIBILITY] ) {
		// both loaders read an 8-byte header, then numClusters rows of clusterBytes
		if ( sizes[LUMP_VISIBILITY] < 8 ) {
			return "truncated BSP visibility header";
		}
		clusters = BSP_FileWord( data + offsets[LUMP_VISIBILITY] );
		rowBytes = BSP_FileWord( data + offsets[LUMP_VISIBILITY] + 4 );
		if ( clusters > INT_MAX - 63u || rowBytes > INT_MAX ||
			 rowBytes < ( clusters + 7 ) / 8 || ( clusters && rowBytes > ( sizes[LUMP_VISIBILITY] - 8 ) / clusters ) ) {
			return "invalid BSP visibility dimensions";
		}
	}
	*header = validated;
	return NULL;
}

/* Sizes come from the caller's actual types, including pointer widths and reserved elements. */
typedef struct { unsigned int count, elementSize, extraElements; } bspArrayAllocation_t;

/** Reject signed hunk allocation size overflow. */
static const char *BSP_ValidateAllocations( const bspArrayAllocation_t *arrays, unsigned int count ) {
	unsigned int i, capacity;
	for ( i = 0; i < count; i++ ) {
		if ( !arrays[i].elementSize ) {
			return "invalid BSP allocation element size";
		}
		capacity = ( INT_MAX - 4096u ) / arrays[i].elementSize;
		if ( arrays[i].extraElements > capacity || arrays[i].count > capacity - arrays[i].extraElements ) {
			return "BSP native array allocation overflow";
		}
	}
	return NULL;
}

#endif
