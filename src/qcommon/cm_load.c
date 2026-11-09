/*
===========================================================================

Wolfenstein: Enemy Territory GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Wolfenstein: Enemy Territory GPL Source Code (Wolf ET Source Code).  

Wolf ET Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Wolf ET Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Wolf ET Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Wolf: ET Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Wolf ET Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// cmodel.c -- model loading

#include "cm_local.h"
#include "cm_patch.h"
#include "bsp_geometry.h"
#include "bsp_tree.h"

#ifdef BSPC

#include "../bspc/l_qfiles.h"

void SetPlaneSignbits( cplane_t *out ) {
	int bits, j;

	// for fast box on planeside test
	bits = 0;
	for ( j = 0 ; j < 3 ; j++ ) {
		if ( out->normal[j] < 0 ) {
			bits |= 1 << j;
		}
	}
	out->signbits = bits;
}
#endif //BSPC

// to allow boxes to be treated as brush models, we allocate
// some extra indexes along with those needed by the map
#define BOX_LEAF_BRUSHES    1   // ydnar
#define BOX_BRUSHES     1
#define BOX_SIDES       6
#define BOX_LEAFS       2
#define BOX_PLANES      12

#define LL( x ) x = LittleLong( x )


clipMap_t cm;
int c_pointcontents;
int c_traces, c_brush_traces, c_patch_traces;


byte        *cmod_base;

#ifndef BSPC
cvar_t      *cm_noAreas;
cvar_t      *cm_noCurves;
cvar_t      *cm_playerCurveClip;
cvar_t      *cm_optimize;
#endif

cmodel_t box_model;
cplane_t    *box_planes;
cbrush_t    *box_brush;



void    CM_InitBoxHull( void );
void    CM_FloodAreaConnections( void );


/*
===============================================================================

					MAP LOADING

===============================================================================
*/

/*
=================
CMod_LoadShaders
=================
*/
void CMod_LoadShaders( lump_t *l ) {
	dshader_t   *in, *out;
	int i, count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "CMod_LoadShaders: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no shaders" );
	}
	cm.shaders = Hunk_Alloc( count * sizeof( *cm.shaders ), h_high );
	cm.numShaders = count;

	memcpy( cm.shaders, in, count * sizeof( *cm.shaders ) );

	if ( LittleLong( 1 ) != 1 ) {
		out = cm.shaders;
		for ( i = 0 ; i < count ; i++, in++, out++ ) {
			out->contentFlags = LittleLong( out->contentFlags );
			out->surfaceFlags = LittleLong( out->surfaceFlags );
		}
	}
}


/*
=================
CMod_LoadSubmodels
=================
*/
void CMod_LoadSubmodels( lump_t *l ) {
	dmodel_t    *in;
	cmodel_t    *out;
	int i, j, count;
	int         *indexes;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "CMod_LoadSubmodels: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no models" );
	}
	cm.cmodels = Hunk_Alloc( count * sizeof( *cm.cmodels ), h_high );
	cm.numSubModels = count;

	if ( count > MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "MAX_SUBMODELS exceeded" );
	}

	for ( i = 0 ; i < count ; i++, in++, out++ )
	{
		out = &cm.cmodels[i];

		for ( j = 0 ; j < 3 ; j++ )
		{   // spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat( in->mins[j] ) - 1;
			out->maxs[j] = LittleFloat( in->maxs[j] ) + 1;
		}

		if ( i == 0 ) {
			continue;   // world model doesn't need other info
		}

		// make a "leaf" just to hold the model's brushes and surfaces
		out->leaf.numLeafBrushes = LittleLong( in->numBrushes );
		indexes = Hunk_Alloc( out->leaf.numLeafBrushes * 4, h_high );
		out->leaf.firstLeafBrush = indexes - cm.leafbrushes;
		for ( j = 0 ; j < out->leaf.numLeafBrushes ; j++ ) {
			indexes[j] = LittleLong( in->firstBrush ) + j;
		}

		out->leaf.numLeafSurfaces = LittleLong( in->numSurfaces );
		indexes = Hunk_Alloc( out->leaf.numLeafSurfaces * 4, h_high );
		out->leaf.firstLeafSurface = indexes - cm.leafsurfaces;
		for ( j = 0 ; j < out->leaf.numLeafSurfaces ; j++ ) {
			indexes[j] = LittleLong( in->firstSurface ) + j;
		}
	}
}


/*
=================
CMod_LoadNodes

=================
*/
void CMod_LoadNodes( lump_t *l ) {
	dnode_t     *in;
	int child;
	cNode_t     *out;
	int i, j, count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map has no nodes" );
	}
	cm.nodes = Hunk_Alloc( count * sizeof( *cm.nodes ), h_high );
	cm.numNodes = count;

	out = cm.nodes;

	for ( i = 0 ; i < count ; i++, out++, in++ )
	{
		out->plane = cm.planes + LittleLong( in->planeNum );
		for ( j = 0 ; j < 2 ; j++ )
		{
			child = LittleLong( in->children[j] );
			out->children[j] = child;
		}
	}

}

/*
=================
CM_BoundBrush

=================
*/
void CM_BoundBrush( cbrush_t *b ) {
	b->bounds[0][0] = -b->sides[0].plane->dist;
	b->bounds[1][0] = b->sides[1].plane->dist;

	b->bounds[0][1] = -b->sides[2].plane->dist;
	b->bounds[1][1] = b->sides[3].plane->dist;

	b->bounds[0][2] = -b->sides[4].plane->dist;
	b->bounds[1][2] = b->sides[5].plane->dist;
}


/*
=================
CMod_LoadBrushes

=================
*/
void CMod_LoadBrushes( lump_t *l ) {
	dbrush_t    *in;
	cbrush_t    *out;
	int i, count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	cm.brushes = Hunk_Alloc( ( BOX_BRUSHES + count ) * sizeof( *cm.brushes ), h_high );
	cm.numBrushes = count;

	out = cm.brushes;

	for ( i = 0 ; i < count ; i++, out++, in++ ) {
		out->sides = cm.brushsides + LittleLong( in->firstSide );
		out->numsides = LittleLong( in->numSides );

		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushes: bad shaderNum: %i", out->shaderNum );
		}
		out->contents = cm.shaders[out->shaderNum].contentFlags;

		CM_BoundBrush( out );
	}

}

/*
=================
CMod_LoadLeafs
=================
*/
void CMod_LoadLeafs( lump_t *l ) {
	int i;
	cLeaf_t     *out;
	dleaf_t     *in;
	int count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no leafs" );
	}

	cm.leafs = Hunk_Alloc( ( BOX_LEAFS + count ) * sizeof( *cm.leafs ), h_high );
	cm.numLeafs = count;

	out = cm.leafs;
	for ( i = 0 ; i < count ; i++, in++, out++ )
	{
		out->cluster = LittleLong( in->cluster );
		out->area = LittleLong( in->area );
		out->firstLeafBrush = LittleLong( in->firstLeafBrush );
		out->numLeafBrushes = LittleLong( in->numLeafBrushes );
		out->firstLeafSurface = LittleLong( in->firstLeafSurface );
		out->numLeafSurfaces = LittleLong( in->numLeafSurfaces );

		if ( out->cluster >= cm.numClusters ) {
			cm.numClusters = out->cluster + 1;
		}
		if ( out->area >= cm.numAreas ) {
			cm.numAreas = out->area + 1;
		}
	}

	cm.areas = Hunk_Alloc( cm.numAreas * sizeof( *cm.areas ), h_high );
	cm.areaPortals = Hunk_Alloc( cm.numAreas * cm.numAreas * sizeof( *cm.areaPortals ), h_high );
}

/*
=================
CMod_LoadPlanes
=================
*/
void CMod_LoadPlanes( lump_t *l ) {
	int i, j;
	cplane_t    *out;
	dplane_t    *in;
	int count;
	int bits;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	if ( count < 1 ) {
		Com_Error( ERR_DROP, "Map with no planes" );
	}
	cm.planes = Hunk_Alloc( ( BOX_PLANES + count ) * sizeof( *cm.planes ), h_high );
	cm.numPlanes = count;

	out = cm.planes;

	for ( i = 0 ; i < count ; i++, in++, out++ )
	{
		bits = 0;
		for ( j = 0 ; j < 3 ; j++ )
		{
			out->normal[j] = LittleFloat( in->normal[j] );
			if ( out->normal[j] < 0 ) {
				bits |= 1 << j;
			}
		}

		out->dist = LittleFloat( in->dist );
		out->type = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
void CMod_LoadLeafBrushes( lump_t *l ) {
	int i;
	int         *out;
	int         *in;
	int count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	// ydnar: more than <count> brushes are stored in leafbrushes...
	cm.leafbrushes = Hunk_Alloc( ( BOX_LEAF_BRUSHES + count ) * sizeof( *cm.leafbrushes ), h_high );
	cm.numLeafBrushes = count;

	out = cm.leafbrushes;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		*out = LittleLong( *in );
	}
}

/*
=================
CMod_LoadLeafSurfaces
=================
*/
void CMod_LoadLeafSurfaces( lump_t *l ) {
	int i;
	int         *out;
	int         *in;
	int count;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	cm.leafsurfaces = Hunk_Alloc( count * sizeof( *cm.leafsurfaces ), h_high );
	cm.numLeafSurfaces = count;

	out = cm.leafsurfaces;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		*out = LittleLong( *in );
	}
}

/*
=================
CMod_LoadBrushSides
=================
*/
void CMod_LoadBrushSides( lump_t *l ) {
	int i;
	cbrushside_t    *out;
	dbrushside_t    *in;
	int count;
	int num;

	in = ( void * )( cmod_base + l->fileofs );
	if ( l->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	count = l->filelen / sizeof( *in );

	cm.brushsides = Hunk_Alloc( ( BOX_SIDES + count ) * sizeof( *cm.brushsides ), h_high );
	cm.numBrushSides = count;

	out = cm.brushsides;

	for ( i = 0 ; i < count ; i++, in++, out++ ) {
		num = LittleLong( in->planeNum );
		out->plane = &cm.planes[num];
		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushSides: bad shaderNum: %i", out->shaderNum );
		}
		out->surfaceFlags = cm.shaders[out->shaderNum].surfaceFlags;
	}
}


/*
=================
CMod_LoadEntityString
=================
*/
void CMod_LoadEntityString( lump_t *l ) {
	// the lump need not end in a NUL; the game parses this as a C string
	cm.entityString = Hunk_Alloc( l->filelen + 1, h_high );
	cm.numEntityChars = l->filelen;
	memcpy( cm.entityString, cmod_base + l->fileofs, l->filelen );
	cm.entityString[l->filelen] = 0;
}

/*
=================
CMod_LoadVisibility
=================
*/
#define VIS_HEADER  8
void CMod_LoadVisibility( lump_t *l ) {
	int len;
	byte    *buf;

	len = l->filelen;
	if ( !len ) {
		cm.clusterBytes = ( cm.numClusters + 31 ) & ~31;
		cm.visibility = Hunk_Alloc( cm.clusterBytes, h_high );
		memset( cm.visibility, 255, cm.clusterBytes );
		return;
	}
	buf = cmod_base + l->fileofs;

	cm.vised = qtrue;
	cm.visibility = Hunk_Alloc( len, h_high );
	cm.numClusters = LittleLong( ( (int *)buf )[0] );
	cm.clusterBytes = LittleLong( ( (int *)buf )[1] );
	memcpy( cm.visibility, buf + VIS_HEADER, len - VIS_HEADER );
}

//==================================================================


/*
=================
CMod_LoadPatches
=================
*/
#define MAX_PATCH_VERTS     1024
void CMod_LoadPatches( lump_t *surfs, lump_t *verts ) {
	drawVert_t  *dv, *dv_p;
	dsurface_t  *in;
	int count;
	int i, j;
	int c;
	cPatch_t    *patch;
	vec3_t points[MAX_PATCH_VERTS];
	int width, height;
	int shaderNum;

	in = ( void * )( cmod_base + surfs->fileofs );
	if ( surfs->filelen % sizeof( *in ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}
	cm.numSurfaces = count = surfs->filelen / sizeof( *in );
	cm.surfaces = Hunk_Alloc( cm.numSurfaces * sizeof( cm.surfaces[0] ), h_high );

	dv = ( void * )( cmod_base + verts->fileofs );
	if ( verts->filelen % sizeof( *dv ) ) {
		Com_Error( ERR_DROP, "MOD_LoadBmodel: funny lump size" );
	}

	// scan through all the surfaces, but only load patches,
	// not planar faces
	for ( i = 0 ; i < count ; i++, in++ ) {
		if ( LittleLong( in->surfaceType ) != MST_PATCH ) {
			continue;       // ignore other surfaces
		}
		// FIXME: check for non-colliding patches

		cm.surfaces[ i ] = patch = Hunk_Alloc( sizeof( *patch ), h_high );

		// load the full drawverts onto the stack
		width = LittleLong( in->patchWidth );
		height = LittleLong( in->patchHeight );
		c = width * height;
		if ( c > MAX_PATCH_VERTS ) {
			Com_Error( ERR_DROP, "ParseMesh: MAX_PATCH_VERTS" );
		}

		dv_p = dv + LittleLong( in->firstVert );
		for ( j = 0 ; j < c ; j++, dv_p++ ) {
			points[j][0] = LittleFloat( dv_p->xyz[0] );
			points[j][1] = LittleFloat( dv_p->xyz[1] );
			points[j][2] = LittleFloat( dv_p->xyz[2] );
		}

		shaderNum = LittleLong( in->shaderNum );
		patch->contents = cm.shaders[shaderNum].contentFlags;
		patch->surfaceFlags = cm.shaders[shaderNum].surfaceFlags;

		// create the internal facet structure
		patch->pc = CM_GeneratePatchCollide( width, height, points, qtrue );
	}
}

//==================================================================


#if 0 //BSPC
/*
==================
CM_FreeMap

Free any loaded map and all submodels
==================
*/
void CM_FreeMap( void ) {
	memset( &cm, 0, sizeof( cm ) );
	Hunk_ClearHigh();
	CM_ClearLevelPatches();
}
#endif //BSPC

unsigned CM_LumpChecksum( lump_t *lump ) {
	return LittleLong( Com_BlockChecksum( cmod_base + lump->fileofs, lump->filelen ) );
}

unsigned CM_Checksum( dheader_t *header ) {
	unsigned checksums[16];
	checksums[0] = CM_LumpChecksum( &header->lumps[LUMP_SHADERS] );
	checksums[1] = CM_LumpChecksum( &header->lumps[LUMP_LEAFS] );
	checksums[2] = CM_LumpChecksum( &header->lumps[LUMP_LEAFBRUSHES] );
	checksums[3] = CM_LumpChecksum( &header->lumps[LUMP_LEAFSURFACES] );
	checksums[4] = CM_LumpChecksum( &header->lumps[LUMP_PLANES] );
	checksums[5] = CM_LumpChecksum( &header->lumps[LUMP_BRUSHSIDES] );
	checksums[6] = CM_LumpChecksum( &header->lumps[LUMP_BRUSHES] );
	checksums[7] = CM_LumpChecksum( &header->lumps[LUMP_MODELS] );
	checksums[8] = CM_LumpChecksum( &header->lumps[LUMP_NODES] );
	checksums[9] = CM_LumpChecksum( &header->lumps[LUMP_SURFACES] );
	checksums[10] = CM_LumpChecksum( &header->lumps[LUMP_DRAWVERTS] );

	return LittleLong( Com_BlockChecksum( checksums, 11 * 4 ) );
}

/*
==================
CM_BudgetAllocation

Takes one Hunk_Alloc( size )'s real cost off what is left (Q3A 7cc5d385).
Subtracting keeps a hostile sum from overflowing. A NULL budget (BSPC)
checks nothing.
==================
*/
#define CM_HUNK_BUDGET_ERROR "collision map needs more hunk memory than is left"
static qboolean CM_BudgetAllocation( unsigned int size, int *remaining ) {
#ifndef BSPC
	int cost;

	if ( !remaining ) {
		return qtrue;
	}
	cost = size > INT_MAX ? -1 : Hunk_AllocationSize( (int)size );
	if ( cost < 0 || cost > *remaining ) {
		return qfalse;
	}
	*remaining -= cost;
#endif
	return qtrue;
}

/*
==================
CM_ValidateBSPDerivedAllocations

The hunk blocks sized by record contents rather than lump lengths: the
areas and their portal matrix, the visibility rows, and each inline
model's brush and surface lists. Spans and indexes are already checked.
==================
*/
static const char *CM_ValidateBSPDerivedAllocations( const byte *buf, const dheader_t *header, int *remaining ) {
	const byte *record;
	unsigned int i, value, clusters = 0, areas = 0, visibility;

	record = buf + header->lumps[LUMP_LEAFS].fileofs;
	for ( i = 0 ; i < header->lumps[LUMP_LEAFS].filelen / sizeof( dleaf_t ) ; i++, record += sizeof( dleaf_t ) ) {
		value = BSP_FileWord( record + offsetof( dleaf_t, cluster ) );
		if ( value != 0xffffffffu && value >= clusters ) {
			clusters = value + 1;
		}
		value = BSP_FileWord( record + offsetof( dleaf_t, area ) );
		if ( value != 0xffffffffu && value >= areas ) {
			areas = value + 1;
		}
	}
	// CMod_LoadVisibility copies the lump whole, header included, or makes novis rows
	visibility = header->lumps[LUMP_VISIBILITY].filelen;
	if ( !visibility ) {
		visibility = ( clusters + 31 ) & ~31u;
	}
	// areas is at most MAX_MAP_AREA_BYTES * 8, so the square cannot overflow
	if ( !CM_BudgetAllocation( areas * sizeof( cArea_t ), remaining ) ||
		 !CM_BudgetAllocation( areas * areas * sizeof( int ), remaining ) ||
		 !CM_BudgetAllocation( visibility, remaining ) ) {
		return CM_HUNK_BUDGET_ERROR;
	}

	record = buf + header->lumps[LUMP_MODELS].fileofs + sizeof( dmodel_t );
	for ( i = 1 ; i < header->lumps[LUMP_MODELS].filelen / sizeof( dmodel_t ) ; i++, record += sizeof( dmodel_t ) ) {
		if ( !CM_BudgetAllocation( BSP_FileWord( record + offsetof( dmodel_t, numBrushes ) ) * 4, remaining ) ||
			 !CM_BudgetAllocation( BSP_FileWord( record + offsetof( dmodel_t, numSurfaces ) ) * 4, remaining ) ) {
			return CM_HUNK_BUDGET_ERROR;
		}
	}
	return NULL;
}

/*
==================
CM_ValidateBSPPatchGrids

Runs each patch's collision refinement and plane and facet build on scratch
storage. Curvature, not the source size, decides how far it subdivides and
how many planes and facets it makes, so this is the only way to know
CM_GeneratePatchCollide will fit, and what it will take from the hunk.
Spans and sizes are already checked.
==================
*/
static const char *CM_ValidateBSPPatchGrids( const byte *buf, const dheader_t *header, int *remaining ) {
	const byte *surface, *vertices;
	vec3_t points[MAX_PATCH_VERTS];
	int i, j, k, count, width, height, sizes[3];
	const char *error;

	count = header->lumps[LUMP_SURFACES].filelen / sizeof( dsurface_t );
	surface = buf + header->lumps[LUMP_SURFACES].fileofs;
	for ( i = 0 ; i < count ; i++, surface += sizeof( dsurface_t ) ) {
		if ( BSP_FileWord( surface + offsetof( dsurface_t, surfaceType ) ) != MST_PATCH ) {
			continue;
		}
		width = BSP_FileWord( surface + offsetof( dsurface_t, patchWidth ) );
		height = BSP_FileWord( surface + offsetof( dsurface_t, patchHeight ) );
		vertices = buf + header->lumps[LUMP_DRAWVERTS].fileofs +
				   BSP_FileWord( surface + offsetof( dsurface_t, firstVert ) ) * sizeof( drawVert_t );
		for ( j = 0 ; j < width * height ; j++ ) {
			for ( k = 0 ; k < 3 ; k++ ) {
				points[j][k] = BSP_GeometryFloat( vertices + j * sizeof( drawVert_t ) + k * 4 );
			}
		}
		error = CM_ValidatePatchCollideAllocations( width, height, points, sizes );
		if ( error ) {
			return error;
		}
		if ( !CM_BudgetAllocation( sizeof( cPatch_t ), remaining ) ) {
			return CM_HUNK_BUDGET_ERROR;
		}
		for ( j = 0 ; j < 3 ; j++ ) {
			if ( !CM_BudgetAllocation( sizes[j], remaining ) ) {
				return CM_HUNK_BUDGET_ERROR;
			}
		}
	}
	return NULL;
}

/*
==================
CM_ValidateBSP

Every lump, cross-lump index, span and patch size the loaders below trust,
checked on the file buffer before CM_LoadMap changes anything. With a
budget, every hunk block the load will take is also charged against it,
so a map too big for the hunk drops here instead of halfway through.
==================
*/
static const char *CM_ValidateBSP( const byte *buf, int length, dheader_t *header, int *remaining ) {
	const char *error;
	unsigned int i;

	error = BSP_ValidateHeader( buf, length, header );
	if ( !error ) {
		// the direct hunk arrays, including the reserved box hull and entity NUL
		const bspArrayAllocation_t arrays[] = {
			{ header->lumps[LUMP_SHADERS].filelen / sizeof( dshader_t ), sizeof( dshader_t ), 0 },
			{ header->lumps[LUMP_LEAFS].filelen / sizeof( dleaf_t ), sizeof( cLeaf_t ), BOX_LEAFS },
			{ header->lumps[LUMP_LEAFBRUSHES].filelen / 4, sizeof( int ), BOX_LEAF_BRUSHES },
			{ header->lumps[LUMP_LEAFSURFACES].filelen / 4, sizeof( int ), 0 },
			{ header->lumps[LUMP_PLANES].filelen / sizeof( dplane_t ), sizeof( cplane_t ), BOX_PLANES },
			{ header->lumps[LUMP_BRUSHSIDES].filelen / sizeof( dbrushside_t ), sizeof( cbrushside_t ), BOX_SIDES },
			{ header->lumps[LUMP_BRUSHES].filelen / sizeof( dbrush_t ), sizeof( cbrush_t ), BOX_BRUSHES },
			{ header->lumps[LUMP_MODELS].filelen / sizeof( dmodel_t ), sizeof( cmodel_t ), 0 },
			{ header->lumps[LUMP_NODES].filelen / sizeof( dnode_t ), sizeof( cNode_t ), 0 },
			{ header->lumps[LUMP_SURFACES].filelen / sizeof( dsurface_t ), sizeof( cPatch_t * ), 0 },
			{ header->lumps[LUMP_ENTITIES].filelen, 1, 1 }
		};
		error = BSP_ValidateAllocations( arrays, sizeof( arrays ) / sizeof( arrays[0] ) );
		// each count fits ( INT_MAX - 4096 ) / elementSize with its extras, checked above
		for ( i = 0 ; !error && i < sizeof( arrays ) / sizeof( arrays[0] ) ; i++ ) {
			if ( !CM_BudgetAllocation( ( arrays[i].count + arrays[i].extraElements ) * arrays[i].elementSize, remaining ) ) {
				error = CM_HUNK_BUDGET_ERROR;
			}
		}
	}
	if ( !error && header->lumps[LUMP_MODELS].filelen / sizeof( dmodel_t ) > MAX_SUBMODELS ) {
		error = "MAX_SUBMODELS exceeded";
	}
	if ( !error ) {
		error = BSP_ValidateReferences( buf, header );
	}
	if ( !error ) {
		error = BSP_ValidateTree( buf, header );
	}
	if ( !error ) {
		error = BSP_ValidateGeometry( buf, header, MAX_GRID_SIZE, MAX_PATCH_VERTS );
	}
	if ( !error ) {
		error = CM_ValidateBSPDerivedAllocations( buf, header, remaining );
	}
	if ( !error ) {
		error = CM_ValidateBSPPatchGrids( buf, header, remaining );
	}
	return error;
}

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
void CM_LoadMap( const char *name, qboolean clientload, int *checksum ) {
	int             *buf;
	const char      *error;
	dheader_t header;
	int length;
	int remaining, *budget = NULL;
	static unsigned last_checksum;

	if ( !name || !name[0] ) {
		Com_Error( ERR_DROP, "CM_LoadMap: NULL name" );
	}

#ifndef BSPC
	cm_noAreas = Cvar_Get( "cm_noAreas", "0", CVAR_CHEAT );
	cm_noCurves = Cvar_Get( "cm_noCurves", "0", CVAR_CHEAT );
	cm_playerCurveClip = Cvar_Get( "cm_playerCurveClip", "1", CVAR_ARCHIVE | CVAR_CHEAT );
	cm_optimize = Cvar_Get( "cm_optimize", "1", CVAR_CHEAT );
#endif
	Com_DPrintf( "CM_LoadMap( %s, %i )\n", name, clientload );

	if ( !strcmp( cm.name, name ) && clientload ) {
		*checksum = last_checksum;
		return;
	}

	if ( !name[0] ) {
		memset( &cm, 0, sizeof( cm ) );
		CM_ClearLevelPatches();
		cm.numLeafs = 1;
		cm.numClusters = 1;
		cm.numAreas = 1;
		cm.cmodels = Hunk_Alloc( sizeof( *cm.cmodels ), h_high );
		*checksum = 0;
		return;
	}

	//
	// load the file
	//
#ifndef BSPC
	length = FS_ReadFile( name, (void **)&buf );
#else
	length = LoadQuakeFile( (quakefile_t *) name, (void **)&buf );
#endif

	if ( !buf ) {
		Com_Error( ERR_DROP, "Couldn't load %s", name );
	}

	// check the whole file, and that all of it fits beside the file in the
	// hunk, before the old map, checksum or hunk is touched
#ifndef BSPC
	remaining = Hunk_MemoryRemaining();
	budget = &remaining;
#endif
	error = CM_ValidateBSP( (const byte *)buf, length, &header, budget );
	if ( error ) {
		FS_FreeFile( buf );
		Com_Error( ERR_DROP, "CM_LoadMap: %s: %s", name, error );
	}

	last_checksum = LittleLong( Com_BlockChecksum( buf, length ) );
	*checksum = last_checksum;

	// free old stuff
	memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();

	cmod_base = (byte *)buf;

	// load into heap
	CMod_LoadShaders( &header.lumps[LUMP_SHADERS] );
	CMod_LoadLeafs( &header.lumps[LUMP_LEAFS] );
	CMod_LoadLeafBrushes( &header.lumps[LUMP_LEAFBRUSHES] );
	CMod_LoadLeafSurfaces( &header.lumps[LUMP_LEAFSURFACES] );
	CMod_LoadPlanes( &header.lumps[LUMP_PLANES] );
	CMod_LoadBrushSides( &header.lumps[LUMP_BRUSHSIDES] );
	CMod_LoadBrushes( &header.lumps[LUMP_BRUSHES] );
	CMod_LoadSubmodels( &header.lumps[LUMP_MODELS] );
	CMod_LoadNodes( &header.lumps[LUMP_NODES] );
	CMod_LoadEntityString( &header.lumps[LUMP_ENTITIES] );
	CMod_LoadVisibility( &header.lumps[LUMP_VISIBILITY] );
	CMod_LoadPatches( &header.lumps[LUMP_SURFACES], &header.lumps[LUMP_DRAWVERTS] );

	// we are NOT freeing the file, because it is cached for the ref
	FS_FreeFile( buf );

	CM_InitBoxHull();

	CM_FloodAreaConnections();

	// allow this to be cached if it is loaded by the server
	if ( !clientload ) {
		Q_strncpyz( cm.name, name, sizeof( cm.name ) );
	}
}

/*
==================
CM_ClearMap
==================
*/
void CM_ClearMap( void ) {
	Com_Memset( &cm, 0, sizeof( cm ) );
	CM_ClearLevelPatches();
}

/*
==================
CM_ClipHandleToModel
==================
*/
cmodel_t    *CM_ClipHandleToModel( clipHandle_t handle ) {
	if ( handle < 0 ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle );
	}
	if ( handle < cm.numSubModels ) {
		return &cm.cmodels[handle];
	}
	if ( handle == BOX_MODEL_HANDLE || handle == CAPSULE_MODEL_HANDLE ) {
		return &box_model;
	}
	if ( handle < MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i < %i < %i",
				   cm.numSubModels, handle, MAX_SUBMODELS );
	}
	Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle + MAX_SUBMODELS );

	return NULL;

}

/*
==================
CM_InlineModel
==================
*/
clipHandle_t    CM_InlineModel( int index ) {
	if ( index < 0 || index >= cm.numSubModels ) {
		Com_Error( ERR_DROP, "CM_InlineModel: bad number" );
	}
	return index;
}

int     CM_NumClusters( void ) {
	return cm.numClusters;
}

int     CM_NumInlineModels( void ) {
	return cm.numSubModels;
}

char    *CM_EntityString( void ) {
	return cm.entityString;
}

int     CM_LeafCluster( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cm.numLeafs ) {
		Com_Error( ERR_DROP, "CM_LeafCluster: bad number" );
	}
	return cm.leafs[leafnum].cluster;
}

int     CM_LeafArea( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cm.numLeafs ) {
		Com_Error( ERR_DROP, "CM_LeafArea: bad number" );
	}
	return cm.leafs[leafnum].area;
}

//=======================================================================


/*
===================
CM_InitBoxHull

Set up the planes and nodes so that the six floats of a bounding box
can just be stored out and get a proper clipping hull structure.
===================
*/
void CM_InitBoxHull( void ) {
	int i;
	int side;
	cplane_t    *p;
	cbrushside_t    *s;

	box_planes = &cm.planes[cm.numPlanes];

	box_brush = &cm.brushes[cm.numBrushes];
	box_brush->numsides = 6;
	box_brush->sides = cm.brushsides + cm.numBrushSides;
	box_brush->contents = CONTENTS_BODY;

	box_model.leaf.numLeafBrushes = 1;
//	box_model.leaf.firstLeafBrush = cm.numBrushes;
	box_model.leaf.firstLeafBrush = cm.numLeafBrushes;
	cm.leafbrushes[cm.numLeafBrushes] = cm.numBrushes;

	for ( i = 0 ; i < 6 ; i++ )
	{
		side = i & 1;

		// brush sides
		s = &cm.brushsides[cm.numBrushSides + i];
		s->plane =  cm.planes + ( cm.numPlanes + i * 2 + side );
		s->surfaceFlags = 0;

		// planes
		p = &box_planes[i * 2];
		p->type = i >> 1;
		p->signbits = 0;
		VectorClear( p->normal );
		p->normal[i >> 1] = 1;

		p = &box_planes[i * 2 + 1];
		p->type = 3 + ( i >> 1 );
		p->signbits = 0;
		VectorClear( p->normal );
		p->normal[i >> 1] = -1;

		SetPlaneSignbits( p );
	}
}

/*
===================
CM_TempBoxModel

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
Capsules are handled differently though.
===================
*/
clipHandle_t CM_TempBoxModel( const vec3_t mins, const vec3_t maxs, int capsule ) {

	VectorCopy( mins, box_model.mins );
	VectorCopy( maxs, box_model.maxs );

	if ( capsule ) {
		return CAPSULE_MODEL_HANDLE;
	}

	box_planes[0].dist = maxs[0];
	box_planes[1].dist = -maxs[0];
	box_planes[2].dist = mins[0];
	box_planes[3].dist = -mins[0];
	box_planes[4].dist = maxs[1];
	box_planes[5].dist = -maxs[1];
	box_planes[6].dist = mins[1];
	box_planes[7].dist = -mins[1];
	box_planes[8].dist = maxs[2];
	box_planes[9].dist = -maxs[2];
	box_planes[10].dist = mins[2];
	box_planes[11].dist = -mins[2];

	VectorCopy( mins, box_brush->bounds[0] );
	VectorCopy( maxs, box_brush->bounds[1] );

	return BOX_MODEL_HANDLE;
}

// DHM - Nerve
void CM_SetTempBoxModelContents( int contents ) {

	box_brush->contents = contents;
}
// dhm

/*
===================
CM_ModelBounds
===================
*/
void CM_ModelBounds( clipHandle_t model, vec3_t mins, vec3_t maxs ) {
	cmodel_t    *cmod;

	cmod = CM_ClipHandleToModel( model );
	VectorCopy( cmod->mins, mins );
	VectorCopy( cmod->maxs, maxs );
}


