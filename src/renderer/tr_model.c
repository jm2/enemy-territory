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

// tr_models.c -- model loading and caching

#include "tr_local.h"

#define LL( x ) x = LittleLong( x )

// Ridah
static qboolean R_LoadMDC( model_t *mod, int lod, void *buffer, int length, const char *mod_name );
// done.
static qboolean R_LoadMD3( model_t *mod, int lod, void *buffer, int length, const char *name );
static qboolean R_LoadMDS( model_t *mod, void *buffer, int length, const char *name );
static qboolean R_LoadMDM( model_t *mod, void *buffer, int length, const char *name );
static qboolean R_LoadMDX( model_t *mod, void *buffer, int length, const char *name );

model_t *loadmodel;

extern cvar_t *r_buildScript;

/*
** R_GetModelByHandle
*/
model_t *R_GetModelByHandle( qhandle_t index ) {
	model_t     *mod;

	// out of range gets the defualt model
	if ( index < 1 || index >= tr.numModels ) {
		return tr.models[0];
	}

	mod = tr.models[index];

	return mod;
}

//===============================================================================

/*
** R_AllocModel
*/
model_t *R_AllocModel( void ) {
	model_t     *mod;

	if ( tr.numModels == MAX_MOD_KNOWN ) {
		return NULL;
	}

	mod = ri.Hunk_Alloc( sizeof( *tr.models[tr.numModels] ), h_low );
	mod->index = tr.numModels;
	tr.models[tr.numModels] = mod;
	tr.numModels++;

	return mod;
}



/*
R_LoadModelShadow()
loads a model's shadow script
*/

void R_LoadModelShadow( model_t *mod ) {
	unsigned    *buf;
	char filename[ 1024 ];
	shader_t    *sh;

	// set default shadow
	mod->shadowShader = 0;

	// build name
	COM_StripExtension2( mod->name, filename, sizeof( filename ) );
	COM_DefaultExtension( filename, 1024, ".shadow" );

	// load file
	ri.FS_ReadFile( filename, (void**) &buf );
	if ( buf != NULL ) {
		char    *shadowBits;

		shadowBits = strchr( (char*) buf, ' ' );
		if ( shadowBits != NULL ) {
			*shadowBits = '\0';
			shadowBits++;

			if ( strlen( (char*) buf ) >= MAX_QPATH ) {
				Com_Printf( "R_LoadModelShadow: Shader name exceeds MAX_QPATH\n" );
				mod->shadowShader = 0;
			} else {
				sh = R_FindShader( (char*) buf, LIGHTMAP_NONE, qtrue );

				if ( sh->defaultShader ) {
					mod->shadowShader = 0;
				} else {
					mod->shadowShader = sh->index;
				}
			}
			sscanf( shadowBits, "%f %f %f %f %f %f",
					&mod->shadowParms[ 0 ], &mod->shadowParms[ 1 ], &mod->shadowParms[ 2 ],
					&mod->shadowParms[ 3 ], &mod->shadowParms[ 4 ], &mod->shadowParms[ 5 ] );
		}
		ri.FS_FreeFile( buf );
	}
}



/*
====================
RE_RegisterModel

Loads in a model for the given name

Zero will be returned if the model fails to load.
An entry will be retained for failed models as an
optimization to prevent disk rescanning if they are
asked for again.
====================
*/
qhandle_t RE_RegisterModel( const char *name ) {
	model_t     *mod;
	unsigned    *buf;
	int lod;
	int ident = 0;         // TTimo: init
	qboolean loaded;
	qhandle_t hModel;
	int numLoaded;
	int firstIdent = 0, firstLod = -1;
	int length;
	char filename[1024];

	if ( !name || !name[0] ) {
		// Ridah, disabled this, we can see models that can't be found because they won't be there
		//ri.Printf( PRINT_ALL, "RE_RegisterModel: NULL name\n" );
		return 0;
	}

	if ( strlen( name ) >= MAX_QPATH ) {
		Com_Printf( "Model name exceeds MAX_QPATH\n" );
		return 0;
	}

	// Ridah, caching
	if ( r_cacheGathering->integer ) {
		ri.Cmd_ExecuteText( EXEC_NOW, va( "cache_usedfile model %s\n", name ) );
	}

	//
	// search the currently loaded models
	//
	for ( hModel = 1 ; hModel < tr.numModels; hModel++ ) {
		mod = tr.models[hModel];
		if ( !Q_stricmp( mod->name, name ) ) {
			if ( mod->type == MOD_BAD ) {
				return 0;
			}
			return hModel;
		}
	}

	// allocate a new model_t

	if ( ( mod = R_AllocModel() ) == NULL ) {
		ri.Printf( PRINT_WARNING, "RE_RegisterModel: R_AllocModel() failed for '%s'\n", name );
		return 0;
	}

	// only set the name after the model has been successfully loaded
	Q_strncpyz( mod->name, name, sizeof( mod->name ) );


	// make sure the render thread is stopped
	R_SyncRenderThread();

	// Ridah, look for it cached
	if ( R_FindCachedModel( name, mod ) ) {
		R_LoadModelShadow( mod );
		return mod->index;
	}
	// done.

	R_LoadModelShadow( mod );

	mod->numLods = 0;

	//
	// load the files
	//
	numLoaded = 0;

	if ( strstr( name, ".mds" ) || strstr( name, ".mdm" ) || strstr( name, ".mdx" ) ) {    // try loading skeletal file
		loaded = qfalse;
		length = ri.FS_ReadFile( name, (void **)&buf );
		if ( buf ) {
			loadmodel = mod;

			ident = length >= 4 ? LittleLong( *(unsigned *)buf ) : 0;
			if ( ident == MDS_IDENT ) {
				loaded = R_LoadMDS( mod, buf, length, name );
			} else if ( ident == MDM_IDENT ) {
				loaded = R_LoadMDM( mod, buf, length, name );
			} else if ( ident == MDX_IDENT ) {
				loaded = R_LoadMDX( mod, buf, length, name );
			}

			ri.FS_FreeFile( buf );
		}

		if ( loaded ) {
			return mod->index;
		}
	}

	for ( lod = MD3_MAX_LODS - 1 ; lod >= 0 ; lod-- ) {

		strcpy( filename, name );

		if ( lod != 0 ) {
			char namebuf[80];

			if ( strrchr( filename, '.' ) ) {
				*strrchr( filename, '.' ) = 0;
			}
			sprintf( namebuf, "_%d.md3", lod );
			strcat( filename, namebuf );
		}

		filename[strlen( filename ) - 1] = 'c';  // try MDC first
		length = ri.FS_ReadFile( filename, (void **)&buf );

		if ( !buf ) {
			filename[strlen( filename ) - 1] = '3';  // try MD3 second
			length = ri.FS_ReadFile( filename, (void **)&buf );
			if ( !buf ) {
				continue;
			}
		}

		loadmodel = mod;

		ident = length >= 4 ? LittleLong( *(unsigned *)buf ) : 0;
		// Ridah, mesh compression
		if ( ident != MD3_IDENT && ident != MDC_IDENT ) {
			ri.Printf( PRINT_WARNING,"RE_RegisterModel: unknown fileid for %s\n", name );
			ri.FS_FreeFile( buf );
			goto fail;
		}

		// md3[] and mdc[] share storage and mod->type names one format, so all
		// LODs must match. The base file decides: a coarser LOD of the other
		// format is skipped, or dropped when the base turns out to differ.
		if ( firstLod >= 0 && ident != firstIdent ) {
			ri.Printf( PRINT_WARNING, "RE_RegisterModel: %s mixes MD3 and MDC levels of detail\n", name );
			if ( lod != 0 ) {
				ri.FS_FreeFile( buf );
				continue;
			}
			memset( &mod->model, 0, sizeof( mod->model ) );
			mod->dataSize = 0;
			firstLod = -1;
			numLoaded = 0;
		}
		if ( ident == MD3_IDENT ) {
			loaded = R_LoadMD3( mod, lod, buf, length, name );
		} else {
			loaded = R_LoadMDC( mod, lod, buf, length, name );
		}
		// done.

		ri.FS_FreeFile( buf );

		if ( !loaded ) {
			if ( lod == 0 ) {
				goto fail;
			} else {
				break;
			}
		} else {
			if ( firstLod < 0 ) {
				firstLod = lod;
				firstIdent = ident;
			}
			numLoaded++;
			// if we have a valid model and are biased
			// so that we won't see any higher detail ones,
			// stop loading them
// Arnout: don't need this anymore,
//			if ( lod <= r_lodbias->integer ) {
//				break;
//			}
		}
	}


	if ( numLoaded ) {
		// duplicate into higher lod spots that weren't
		// loaded, in case the user changes r_lodbias on the fly.
		// The renderer may pick any slot below numLods, so fill every one:
		// a missing LOD between two that loaded, the slot of a refused LOD,
		// and those below it all take the next coarser LOD that loaded.
		mod->numLods = firstLod + 1;
		for ( lod = firstLod - 1 ; lod >= 0 ; lod-- ) {
			if ( firstIdent == MD3_IDENT ) {
				if ( !mod->model.md3[lod] ) {
					mod->model.md3[lod] = mod->model.md3[lod + 1];
				}
			} else if ( !mod->model.mdc[lod] ) {
				mod->model.mdc[lod] = mod->model.mdc[lod + 1];
			}
		}

		return mod->index;
	}

fail:
	// we still keep the model_t around, so if the model name is asked for
	// again, we won't bother scanning the filesystem
	mod->type = MOD_BAD;
	return 0;
}

//-------------------------------------------------------------------------------
// Ridah, mesh compression
float r_anormals[NUMMDCVERTEXNORMALS][3] = {
#include "anorms256.h"
};

/*
=============
R_MDC_GetVec
=============
*/
void R_MDC_GetVec( unsigned char anorm, vec3_t dir ) {
	VectorCopy( r_anormals[anorm], dir );
}

/*
=============
R_MDC_GetAnorm
=============
*/
unsigned char R_MDC_GetAnorm( const vec3_t dir ) {
	int i, best_start_i[3], next_start, next_end;
	int best = 0; // TTimo: init
	float best_diff, group_val, this_val, diff;
	float   *this_norm;

	// find best Z match

	if ( dir[2] > 0.097545f ) {
		next_start = 144;
		next_end = NUMMDCVERTEXNORMALS;
	} else
	{
		next_start = 0;
		next_end = 144;
	}

	best_diff = 999;
	this_val = -999;

	for ( i = next_start ; i < next_end ; i++ )
	{
		if ( r_anormals[i][2] == this_val ) {
			continue;
		} else {
			this_val = r_anormals[i][2];
		}

		if ( ( diff = Q_fabs( dir[2] - r_anormals[i][2] ) ) < best_diff ) {
			best_diff = diff;
			best_start_i[2] = i;

		}

		if ( next_start ) {
			if ( r_anormals[i][2] > dir[2] ) {
				break;  // we've gone passed the dir[2], so we can't possibly find a better match now
			}
		} else {
			if ( r_anormals[i][2] < dir[2] ) {
				break;  // we've gone passed the dir[2], so we can't possibly find a better match now
			}
		}
	}

	best_diff = -999;

	// find best match within the Z group

	for ( i = best_start_i[2], group_val = r_anormals[i][2]; i < NUMMDCVERTEXNORMALS; i++ )
	{
		this_norm = r_anormals[i];

		if ( this_norm[2] != group_val ) {
			break; // done checking the group
		}
/*
		if (	(this_norm[0] < 0 && dir[0] > 0)
			||	(this_norm[0] > 0 && dir[0] < 0)
			||	(this_norm[1] < 0 && dir[1] > 0)
			||	(this_norm[1] > 0 && dir[1] < 0))
			continue;
*/
		diff = DotProduct( dir, this_norm );

		if ( diff > best_diff ) {
			best_diff = diff;
			best = i;
		}
	}

	return (unsigned char)best;
}

/*
=================
R_MDC_EncodeOfsVec
=================
*/
qboolean R_MDC_EncodeXyzCompressed( const vec3_t vec, const vec3_t normal, mdcXyzCompressed_t *out ) {
	mdcXyzCompressed_t retval;
	int i;
	unsigned char anorm;

	i = sizeof( mdcXyzCompressed_t );

	retval.ofsVec = 0;
	for ( i = 0; i < 3; i++ ) {
		if ( Q_fabs( vec[i] ) >= MDC_MAX_DIST ) {
			return qfalse;
		}
		retval.ofsVec += ( ( (int)Q_fabs( ( vec[i] + MDC_DIST_SCALE * 0.5 ) * ( 1.0 / MDC_DIST_SCALE ) + MDC_MAX_OFS ) ) << ( i * MDC_BITS_PER_AXIS ) );
	}
	anorm = R_MDC_GetAnorm( normal );
	retval.ofsVec |= ( (int)anorm ) << 24;

	*out = retval;
	return qtrue;
}

/*
=================
R_MDC_DecodeXyzCompressed
=================
*/
#if 0   // unoptimized version, used for finding right settings
void R_MDC_DecodeXyzCompressed( mdcXyzCompressed_t *xyzComp, vec3_t out, vec3_t normal ) {
	int i;

	for ( i = 0; i < 3; i++ ) {
		out[i] = ( (float)( ( xyzComp->ofsVec >> ( i * MDC_BITS_PER_AXIS ) ) & ( ( 1 << MDC_BITS_PER_AXIS ) - 1 ) ) - MDC_MAX_OFS ) * MDC_DIST_SCALE;
	}
	R_MDC_GetVec( ( unsigned char )( xyzComp->ofsVec >> 24 ), normal );
}
#endif

// rain - unused
#if 0
/*
=================
R_MDC_GetXyzCompressed
=================
*/
static qboolean R_MDC_GetXyzCompressed( md3Header_t *md3, md3XyzNormal_t *newXyz, vec3_t oldPos, mdcXyzCompressed_t *out, qboolean verify ) {
	vec3_t newPos, vec;
	int i;
	vec3_t pos, dir, norm, outnorm;

	for ( i = 0; i < 3; i++ ) {
		newPos[i] = (float)newXyz->xyz[i] * MD3_XYZ_SCALE;
	}

	VectorSubtract( newPos, oldPos, vec );
	R_LatLongToNormal( norm, newXyz->normal );
	if ( !R_MDC_EncodeXyzCompressed( vec, norm, out ) ) {
		return qfalse;
	}

	// calculate the uncompressed position
	R_MDC_DecodeXyzCompressed( out->ofsVec, dir, outnorm );
	VectorAdd( oldPos, dir, pos );

	if ( verify ) {
		if ( Distance( newPos, pos ) > MDC_MAX_ERROR ) {
			return qfalse;
		}
	}

	return qtrue;
}


/*
=================
R_MDC_CompressSurfaceFrame
=================
*/
static qboolean R_MDC_CompressSurfaceFrame( md3Header_t *md3, md3Surface_t *surf, int frame, int lastBaseFrame, mdcXyzCompressed_t *out ) {
	int i, j;
	md3XyzNormal_t  *xyz, *baseXyz;
	vec3_t oldPos;

	xyz = ( md3XyzNormal_t * )( (byte *)surf + surf->ofsXyzNormals );
	baseXyz = xyz + ( lastBaseFrame * surf->numVerts );
	xyz += ( frame * surf->numVerts );

	for ( i = 0; i < surf->numVerts; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			oldPos[j] = (float)baseXyz[i].xyz[j] * MD3_XYZ_SCALE;
		}
		if ( !R_MDC_GetXyzCompressed( md3, &xyz[i], oldPos, &out[i], qtrue ) ) {
			return qfalse;
		}
	}

	return qtrue;
}

/*
=================
R_MDC_CanCompressSurfaceFrame
=================
*/
static qboolean R_MDC_CanCompressSurfaceFrame( md3Header_t *md3, md3Surface_t *surf, int frame, int lastBaseFrame ) {
	int i, j;
	md3XyzNormal_t  *xyz, *baseXyz;
	mdcXyzCompressed_t xyzComp;
	vec3_t oldPos;

	xyz = ( md3XyzNormal_t * )( (byte *)surf + surf->ofsXyzNormals );
	baseXyz = xyz + ( lastBaseFrame * surf->numVerts );
	xyz += ( frame * surf->numVerts );

	for ( i = 0; i < surf->numVerts; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			oldPos[j] = (float)baseXyz[i].xyz[j] * MD3_XYZ_SCALE;
		}
		if ( !R_MDC_GetXyzCompressed( md3, &xyz[i], oldPos, &xyzComp, qtrue ) ) {
			return qfalse;
		}
	}

	return qtrue;
}

/*
=================
R_MD3toMDC

  Converts a model_t from md3 to mdc format
=================
*/
static qboolean R_MDC_ConvertMD3( model_t *mod, int lod, const char *mod_name ) {
	int i, j, f, c, k;
	md3Surface_t        *surf;
	md3Header_t         *md3;
	int                 *baseFrames;
	int numBaseFrames;

	qboolean foundBase;

	mdcHeader_t         *mdc, mdcHeader;
	mdcSurface_t        *cSurf;
	short               *frameBaseFrames, *frameCompFrames;

	mdcTag_t            *mdcTag;
	md3Tag_t            *md3Tag;

	vec3_t axis[3], angles;
	float ftemp;

	md3 = mod->model.md3[lod];

	baseFrames = ri.Hunk_AllocateTempMemory( sizeof( *baseFrames ) * md3->numFrames );

	// the first frame is always a base frame
	numBaseFrames = 0;
	memset( baseFrames, 0, sizeof( *baseFrames ) * md3->numFrames );
	baseFrames[numBaseFrames++] = 0;

	// first calculate how many baseframes we need, and which frames they are on
	// we need to treat the entire model as a single surface, if we compress some surfaces, and not others,
	// we may get tearing between surfaces

	for ( f = 1; f < md3->numFrames; f++ ) {

		surf = ( md3Surface_t * )( (byte *)md3 + md3->ofsSurfaces );
		foundBase = qfalse;
		for ( i = 0 ; i < md3->numSurfaces ; i++ ) {

			// process the verts in this surface, checking to see if the compressed
			// version will be close enough to the actual vert
			if ( !foundBase && !R_MDC_CanCompressSurfaceFrame( md3, surf, f, baseFrames[numBaseFrames - 1] ) ) {
				baseFrames[numBaseFrames++] = f;
				foundBase = qtrue;
			}

			// find the next surface
			surf = ( md3Surface_t * )( (byte *)surf + surf->ofsEnd );
		}

	}

	// success, so fill in the necessary data to the model_t
	mdcHeader.ident = MDC_IDENT;
	mdcHeader.version = MDC_VERSION;
	Q_strncpyz( mdcHeader.name, md3->name, sizeof( mdcHeader.name ) );
	mdcHeader.flags = md3->flags;
	mdcHeader.numFrames = md3->numFrames;
	mdcHeader.numTags = md3->numTags;
	mdcHeader.numSurfaces = md3->numSurfaces;
	mdcHeader.numSkins = md3->numSkins;
	mdcHeader.ofsFrames = sizeof( mdcHeader_t );
	mdcHeader.ofsTagNames = mdcHeader.ofsFrames + mdcHeader.numFrames * sizeof( md3Frame_t );
	mdcHeader.ofsTags = mdcHeader.ofsTagNames + mdcHeader.numTags * sizeof( mdcTagName_t );
	mdcHeader.ofsSurfaces = mdcHeader.ofsTags + mdcHeader.numTags * mdcHeader.numFrames * sizeof( mdcTag_t );
	mdcHeader.ofsEnd = mdcHeader.ofsSurfaces;

	surf = ( md3Surface_t * )( (byte *)md3 + md3->ofsSurfaces );
	for ( f = 0; f < md3->numSurfaces; f++ ) {
		mdcHeader.ofsEnd += sizeof( mdcSurface_t )
							+ surf->numShaders * sizeof( md3Shader_t )
							+ surf->numTriangles * sizeof( md3Triangle_t )
							+ surf->numVerts * sizeof( md3St_t )
							+ surf->numVerts * numBaseFrames * sizeof( md3XyzNormal_t )
							+ surf->numVerts * ( md3->numFrames - numBaseFrames ) * sizeof( mdcXyzCompressed_t )
							+ sizeof( short ) * md3->numFrames
							+ sizeof( short ) * md3->numFrames;

		surf = ( md3Surface_t * )( (byte *)surf + surf->ofsEnd );
	}

	// report the memory differences
	Com_Printf( "Compressed %s. Old = %i, New = %i\n", mod_name, md3->ofsEnd, mdcHeader.ofsEnd );

	mdc = ri.Hunk_Alloc( mdcHeader.ofsEnd, h_low );
	mod->model.mdc[lod] = mdc;

	// we have the memory allocated, so lets fill it in

	// header info
	*mdc = mdcHeader;
	// frames
	memcpy( ( md3Frame_t * )( (byte *)mdc + mdc->ofsFrames ), ( md3Frame_t * )( (byte *)md3 + md3->ofsFrames ), mdcHeader.numFrames * sizeof( md3Frame_t ) );
	// tag names
	for ( j = 0; j < md3->numTags; j++ ) {
		memcpy( ( mdcTagName_t * )( (byte *)mdc + mdc->ofsTagNames ) + j, ( ( md3Tag_t * )( (byte *)md3 + md3->ofsTags ) + j )->name, sizeof( mdcTagName_t ) );
	}
	// tags
	mdcTag = ( ( mdcTag_t * )( (byte *)mdc + mdc->ofsTags ) );
	md3Tag = ( ( md3Tag_t * )( (byte *)md3 + md3->ofsTags ) );
	for ( f = 0; f < md3->numFrames; f++ ) {
		for ( j = 0; j < md3->numTags; j++, mdcTag++, md3Tag++ ) {
			for ( k = 0; k < 3; k++ ) {
				// origin
				ftemp = md3Tag->origin[k] / MD3_XYZ_SCALE;
				mdcTag->xyz[k] = (short)ftemp;
				// axis
				VectorCopy( md3Tag->axis[k], axis[k] );
			}
			// convert the axis to angles
			AxisToAngles( axis, angles );
			// copy them into the new tag
			for ( k = 0; k < 3; k++ ) {
				mdcTag->angles[k] = angles[k] / MDC_TAG_ANGLE_SCALE;
			}
		}
	}
	// surfaces
	surf = ( md3Surface_t * )( (byte *)md3 + md3->ofsSurfaces );
	cSurf = ( mdcSurface_t * )( (byte *)mdc + mdc->ofsSurfaces );
	for ( j = 0 ; j < md3->numSurfaces ; j++ ) {

		cSurf->ident = SF_MDC;
		Q_strncpyz( cSurf->name, surf->name, sizeof( cSurf->name ) );
		cSurf->flags = surf->flags;
		cSurf->numCompFrames = ( mdc->numFrames - numBaseFrames );
		cSurf->numBaseFrames = numBaseFrames;
		cSurf->numShaders = surf->numShaders;
		cSurf->numVerts = surf->numVerts;
		cSurf->numTriangles = surf->numTriangles;
		cSurf->ofsTriangles = sizeof( mdcSurface_t );
		cSurf->ofsShaders = cSurf->ofsTriangles + cSurf->numTriangles * sizeof( md3Triangle_t );
		cSurf->ofsSt = cSurf->ofsShaders + cSurf->numShaders * sizeof( md3Shader_t );
		cSurf->ofsXyzNormals = cSurf->ofsSt + cSurf->numVerts * sizeof( md3St_t );
		cSurf->ofsXyzCompressed = cSurf->ofsXyzNormals + cSurf->numVerts * numBaseFrames * sizeof( md3XyzNormal_t );
		cSurf->ofsFrameBaseFrames = cSurf->ofsXyzCompressed + cSurf->numVerts * ( mdc->numFrames - numBaseFrames ) * sizeof( mdcXyzCompressed_t );
		cSurf->ofsFrameCompFrames = cSurf->ofsFrameBaseFrames + mdc->numFrames * sizeof( short );
		cSurf->ofsEnd = cSurf->ofsFrameCompFrames + mdc->numFrames * sizeof( short );

		// triangles
		memcpy( (byte *)cSurf + cSurf->ofsTriangles, (byte *)surf + surf->ofsTriangles, cSurf->numTriangles * sizeof( md3Triangle_t ) );
		// shaders
		memcpy( (byte *)cSurf + cSurf->ofsShaders, (byte *)surf + surf->ofsShaders, cSurf->numShaders * sizeof( md3Shader_t ) );
		// st
		memcpy( (byte *)cSurf + cSurf->ofsSt, (byte *)surf + surf->ofsSt, cSurf->numVerts * sizeof( md3St_t ) );

		// rest
		frameBaseFrames = ( short * )( (byte *)cSurf + cSurf->ofsFrameBaseFrames );
		frameCompFrames = ( short * )( (byte *)cSurf + cSurf->ofsFrameCompFrames );
		for ( f = 0, i = 0, c = 0; f < md3->numFrames; f++ ) {
			if ( i < numBaseFrames && f == baseFrames[i] ) {
				// copy this baseFrame from the md3
				memcpy( (byte *)cSurf + cSurf->ofsXyzNormals + ( sizeof( md3XyzNormal_t ) * cSurf->numVerts * i ),
						(byte *)surf + surf->ofsXyzNormals + ( sizeof( md3XyzNormal_t ) * cSurf->numVerts * f ),
						sizeof( md3XyzNormal_t ) * cSurf->numVerts );
				i++;
				frameCompFrames[f] = -1;
				frameBaseFrames[f] = i - 1;
			} else {
				if ( !R_MDC_CompressSurfaceFrame( md3, surf, f, baseFrames[i - 1], ( mdcXyzCompressed_t * )( (byte *)cSurf + cSurf->ofsXyzCompressed + sizeof( mdcXyzCompressed_t ) * cSurf->numVerts * c ) ) ) {
					ri.Error( ERR_DROP, "R_MDC_ConvertMD3: tried to compress an unsuitable frame\n" );
				}
				frameCompFrames[f] = c;
				frameBaseFrames[f] = i - 1;
				c++;
			}
		}

		// find the next surface
		surf = ( md3Surface_t * )( (byte *)surf + surf->ofsEnd );
		cSurf = ( mdcSurface_t * )( (byte *)cSurf + cSurf->ofsEnd );
	}

	mod->type = MOD_MDC;

	// free allocated memory
	ri.Hunk_FreeTempMemory( baseFrames );

	// kill the md3 memory
	ri.Hunk_FreeTempMemory( md3 );
	mod->model.md3[lod] = NULL;

	return qtrue;
}
#endif
// rain - #if 0

/*
==============================================================================

Model layout validation

A model can come from any pk3 a server offers. Each loader checks the whole
layout against the real file length before it allocates, copies, registers
a shader or byte-swaps anything. Fields are read through LittleLong and
LittleShort, exactly as the swap loops produce them, so the values checked
are the values the renderer later uses. Offsets are checked by division
before a pointer is formed. Sections that the loader writes must not overlap:
a big-endian swap, or a shaderIndex or ident store, would otherwise change a
value after it was checked. The checks follow Quake III's MD3/MD4 validation;
MDC, MDS, MDM and MDX use the same rules.

==============================================================================
*/

typedef struct {
	unsigned int offset, size;
} modelRange_t;

static unsigned int R_ModelWord( const byte *data ) {
	int word;

	memcpy( &word, data, sizeof( word ) );
	return (unsigned int)LittleLong( word );
}

static int R_ModelShort( const byte *data ) {
	short word;

	memcpy( &word, data, sizeof( word ) );
	return LittleShort( word );
}

#define R_MODEL_FIELD( data, type, field ) R_ModelWord( (const byte *)( data ) + offsetof( type, field ) )

// a complete array inside [0, limit), past the enclosing header and aligned for native access
static qboolean R_ModelRange( unsigned int limit, unsigned int headerSize, unsigned int offset,
							  unsigned int count, unsigned int stride, unsigned int alignment, modelRange_t *range ) {
	if ( offset > limit || offset % alignment || count > ( limit - offset ) / stride || ( count && offset < headerSize ) ) {
		return qfalse;
	}
	range->offset = offset;
	range->size = count * stride;
	return qtrue;
}

static qboolean R_ModelDisjoint( const modelRange_t *ranges, int count ) {
	int i, j;

	for ( i = 0; i < count; i++ ) {
		for ( j = i + 1; j < count; j++ ) {
			if ( ranges[i].size && ranges[j].size &&
				 ranges[i].offset < ranges[j].offset + ranges[j].size &&
				 ranges[j].offset < ranges[i].offset + ranges[i].size ) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

static qboolean R_ModelName( const byte *name, int size ) {
	return memchr( name, 0, size ) != NULL;
}

// one entry of a chain of surfaces or tags: its header inside the file, and an
// ofsEnd that covers the header, stays inside the file and strictly advances
static const char *R_ModelChainEntry( const byte *data, unsigned int size, unsigned int offset,
									  unsigned int headerSize, unsigned int endField, unsigned int *entrySize ) {
	if ( headerSize > size - offset ) {
		return "truncated surface or tag";
	}
	*entrySize = R_ModelWord( data + offset + endField );
	if ( *entrySize < headerSize || *entrySize > size - offset || *entrySize % 4 ) {
		return "surface or tag end outside the file";
	}
	return NULL;
}

/*
Skeletal surfaces (MDS and MDM) share a layout: a header whose ofsHeader points
back at the model header, variable-size vertexes, triangles, a collapse map and
bone references. The two surface headers are identical, as are numWeights
and the leading boneIndex of a weight; vertexHeader is offsetof( vertex, weights ).
*/
static const char *R_ValidateSkeletalSurface( const byte *surface, unsigned int surfaceOffset, unsigned int surfaceSize,
											  unsigned int surfaceHeader, unsigned int vertexHeader, unsigned int weightSize,
											  unsigned int maxVerts, unsigned int maxTriangles, unsigned int numBones ) {
	unsigned int vertices, triangles, references, vertexOffset, weights, i, j;
	modelRange_t ranges[4];

	vertices = R_MODEL_FIELD( surface, mdsSurface_t, numVerts );
	triangles = R_MODEL_FIELD( surface, mdsSurface_t, numTriangles );
	references = R_MODEL_FIELD( surface, mdsSurface_t, numBoneReferences );

	if ( R_MODEL_FIELD( surface, mdsSurface_t, ofsHeader ) != 0u - surfaceOffset ) {
		return "surface does not point back at the header";
	}
	if ( !R_ModelName( surface + offsetof( mdsSurface_t, name ), MAX_QPATH ) ||
		 !R_ModelName( surface + offsetof( mdsSurface_t, shader ), MAX_QPATH ) ) {
		return "unterminated surface or shader name";
	}
	if ( vertices >= SHADER_MAX_VERTEXES || vertices > maxVerts ||
		 triangles >= SHADER_MAX_INDEXES / 3 || triangles > maxTriangles ) {
		return "too many vertexes or triangles on a surface";
	}
	if ( !R_ModelRange( surfaceSize, surfaceHeader, R_MODEL_FIELD( surface, mdsSurface_t, ofsVerts ), vertices, vertexHeader, 4, &ranges[0] ) ||
		 !R_ModelRange( surfaceSize, surfaceHeader, R_MODEL_FIELD( surface, mdsSurface_t, ofsTriangles ), triangles, sizeof( mdsTriangle_t ), 4, &ranges[1] ) ||
		 !R_ModelRange( surfaceSize, surfaceHeader, R_MODEL_FIELD( surface, mdsSurface_t, ofsCollapseMap ), vertices, sizeof( int ), 4, &ranges[2] ) ||
		 !R_ModelRange( surfaceSize, surfaceHeader, R_MODEL_FIELD( surface, mdsSurface_t, ofsBoneReferences ), references, sizeof( int ), 4, &ranges[3] ) ) {
		return "surface array outside the surface";
	}

	// every vertex, and every weight, must lie inside the surface
	vertexOffset = ranges[0].offset;
	for ( i = 0; i < vertices; i++ ) {
		if ( vertexHeader > surfaceSize - vertexOffset ) {
			return "vertex outside the surface";
		}
		// the renderer reads weights[0] of every vertex
		weights = R_ModelWord( surface + vertexOffset + offsetof( mdsVertex_t, numWeights ) );
		vertexOffset += vertexHeader;
		if ( weights < 1 || weights > ( surfaceSize - vertexOffset ) / weightSize ) {
			return "vertex weights outside the surface";
		}
		for ( j = 0; j < weights; j++ ) {
			if ( R_ModelWord( surface + vertexOffset + j * weightSize ) >= numBones ) {
				return "vertex weight bone out of range";
			}
		}
		vertexOffset += weights * weightSize;
	}
	ranges[0].size = vertexOffset - ranges[0].offset;
	if ( !R_ModelDisjoint( ranges, 4 ) ) {
		return "overlapping surface arrays";
	}

	for ( i = 0; i < triangles * 3; i++ ) {
		if ( R_ModelWord( surface + ranges[1].offset + i * sizeof( int ) ) >= vertices ) {
			return "triangle vertex out of range";
		}
	}
	// a vertex collapses onto an earlier one, which the renderer has already placed
	for ( i = 0; i < vertices; i++ ) {
		if ( R_ModelWord( surface + ranges[2].offset + i * sizeof( int ) ) >= ( i ? i : vertices ) ) {
			return "collapse map out of range";
		}
	}
	for ( i = 0; i < references; i++ ) {
		if ( R_ModelWord( surface + ranges[3].offset + i * sizeof( int ) ) >= numBones ) {
			return "bone reference out of range";
		}
	}
	return NULL;
}

// a parent must come before its child, so parent chains end and have no cycles
static const char *R_ValidateBoneInfo( const byte *data, unsigned int offset, unsigned int numBones ) {
	unsigned int i;
	int parent;

	for ( i = 0; i < numBones; i++ ) {
		// mdsBoneInfo_t and mdxBoneInfo_t are identical
		parent = (int)R_ModelWord( data + offset + i * sizeof( mdsBoneInfo_t ) + offsetof( mdsBoneInfo_t, parent ) );
		if ( parent >= (int)i ) {
			return "bone parent out of order";
		}
	}
	return NULL;
}

static const char *R_ValidateMD3( const byte *data, int length, int *validatedSize ) {
	const byte *surface;
	unsigned int size, frames, tags, surfaces, surfaceOffset, surfaceSize, vertices, triangles, shaders, surfaceFrames, i, j;
	modelRange_t fileRanges[3], ranges[4];
	const char *error;

	if ( !data || length < (int)sizeof( md3Header_t ) ) {
		return "truncated header";
	}
	if ( R_MODEL_FIELD( data, md3Header_t, version ) != MD3_VERSION ) {
		return "wrong version";
	}
	size = R_MODEL_FIELD( data, md3Header_t, ofsEnd );
	frames = R_MODEL_FIELD( data, md3Header_t, numFrames );
	tags = R_MODEL_FIELD( data, md3Header_t, numTags );
	surfaces = R_MODEL_FIELD( data, md3Header_t, numSurfaces );
	if ( size < sizeof( md3Header_t ) || size > (unsigned int)length ) {
		return "end offset outside the file";
	}
	// nothing keeps a per-surface array, and legacy's secretdocs.md3 has 38 surfaces,
	// more than MD3_MAX_SURFACES, so the surface count is bounded by the file instead
	if ( frames < 1 || frames > MD3_MAX_FRAMES || tags > MD3_MAX_TAGS ) {
		return "bad frame or tag count";
	}
	if ( !R_ModelRange( size, sizeof( md3Header_t ), R_MODEL_FIELD( data, md3Header_t, ofsFrames ), frames, sizeof( md3Frame_t ), 4, &fileRanges[0] ) ||
		 !R_ModelRange( size, sizeof( md3Header_t ), R_MODEL_FIELD( data, md3Header_t, ofsTags ), frames * tags, sizeof( md3Tag_t ), 4, &fileRanges[1] ) ) {
		return "frames or tags outside the file";
	}
	for ( i = 0; i < frames * tags; i++ ) {
		if ( !R_ModelName( data + fileRanges[1].offset + i * sizeof( md3Tag_t ), MAX_QPATH ) ) {
			return "unterminated tag name";
		}
	}

	surfaceOffset = R_MODEL_FIELD( data, md3Header_t, ofsSurfaces );
	if ( !R_ModelRange( size, sizeof( md3Header_t ), surfaceOffset, surfaces, sizeof( md3Surface_t ), 4, &fileRanges[2] ) ) {
		return "surfaces outside the file";
	}
	for ( i = 0; i < surfaces; i++ ) {
		if ( ( error = R_ModelChainEntry( data, size, surfaceOffset, sizeof( md3Surface_t ), offsetof( md3Surface_t, ofsEnd ), &surfaceSize ) ) != NULL ) {
			return error;
		}
		surface = data + surfaceOffset;
		vertices = R_MODEL_FIELD( surface, md3Surface_t, numVerts );
		triangles = R_MODEL_FIELD( surface, md3Surface_t, numTriangles );
		shaders = R_MODEL_FIELD( surface, md3Surface_t, numShaders );
		// the renderer reads the model's frames and the big-endian swap the surface's;
		// legacy's editor camera has a surface frame count of 0, so cover both
		surfaceFrames = R_MODEL_FIELD( surface, md3Surface_t, numFrames );
		if ( surfaceFrames > MD3_MAX_FRAMES ) {
			return "bad surface frame count";
		}
		if ( surfaceFrames < frames ) {
			surfaceFrames = frames;
		}
		if ( vertices >= SHADER_MAX_VERTEXES || vertices > MD3_MAX_VERTS ||
			 triangles >= SHADER_MAX_INDEXES / 3 || triangles > MD3_MAX_TRIANGLES || shaders > MD3_MAX_SHADERS ) {
			return "too many vertexes, triangles or shaders on a surface";
		}
		if ( !R_ModelName( surface + offsetof( md3Surface_t, name ), MAX_QPATH ) ) {
			return "unterminated surface name";
		}
		if ( !R_ModelRange( surfaceSize, sizeof( md3Surface_t ), R_MODEL_FIELD( surface, md3Surface_t, ofsTriangles ), triangles, sizeof( md3Triangle_t ), 4, &ranges[0] ) ||
			 !R_ModelRange( surfaceSize, sizeof( md3Surface_t ), R_MODEL_FIELD( surface, md3Surface_t, ofsShaders ), shaders, sizeof( md3Shader_t ), 4, &ranges[1] ) ||
			 !R_ModelRange( surfaceSize, sizeof( md3Surface_t ), R_MODEL_FIELD( surface, md3Surface_t, ofsSt ), vertices, sizeof( md3St_t ), 4, &ranges[2] ) ||
			 !R_ModelRange( surfaceSize, sizeof( md3Surface_t ), R_MODEL_FIELD( surface, md3Surface_t, ofsXyzNormals ), vertices * surfaceFrames, sizeof( md3XyzNormal_t ), 2, &ranges[3] ) ) {
			return "surface array outside the surface";
		}
		if ( !R_ModelDisjoint( ranges, 4 ) ) {
			return "overlapping surface arrays";
		}
		for ( j = 0; j < triangles * 3; j++ ) {
			if ( R_ModelWord( surface + ranges[0].offset + j * sizeof( int ) ) >= vertices ) {
				return "triangle vertex out of range";
			}
		}
		for ( j = 0; j < shaders; j++ ) {
			if ( !R_ModelName( surface + ranges[1].offset + j * sizeof( md3Shader_t ), MAX_QPATH ) ) {
				return "unterminated shader name";
			}
		}
		surfaceOffset += surfaceSize;
	}
	fileRanges[2].size = surfaceOffset - fileRanges[2].offset;
	if ( !R_ModelDisjoint( fileRanges, 3 ) ) {
		return "overlapping frames, tags and surfaces";
	}
	*validatedSize = size;
	return NULL;
}

static const char *R_ValidateMDC( const byte *data, int length, int *validatedSize ) {
	const byte *surface;
	unsigned int size, frames, tags, surfaces, surfaceOffset, surfaceSize, vertices, triangles, shaders;
	unsigned int baseFrames, compFrames, i, j;
	int frame;
	modelRange_t fileRanges[4], ranges[7];
	const char *error;

	if ( !data || length < (int)sizeof( mdcHeader_t ) ) {
		return "truncated header";
	}
	if ( R_MODEL_FIELD( data, mdcHeader_t, version ) != MDC_VERSION ) {
		return "wrong version";
	}
	size = R_MODEL_FIELD( data, mdcHeader_t, ofsEnd );
	frames = R_MODEL_FIELD( data, mdcHeader_t, numFrames );
	tags = R_MODEL_FIELD( data, mdcHeader_t, numTags );
	surfaces = R_MODEL_FIELD( data, mdcHeader_t, numSurfaces );
	if ( size < sizeof( mdcHeader_t ) || size > (unsigned int)length ) {
		return "end offset outside the file";
	}
	// as for MD3, the surface count is bounded by the file
	if ( frames < 1 || frames > MD3_MAX_FRAMES || tags > MD3_MAX_TAGS ) {
		return "bad frame or tag count";
	}
	if ( !R_ModelRange( size, sizeof( mdcHeader_t ), R_MODEL_FIELD( data, mdcHeader_t, ofsFrames ), frames, sizeof( md3Frame_t ), 4, &fileRanges[0] ) ||
		 !R_ModelRange( size, sizeof( mdcHeader_t ), R_MODEL_FIELD( data, mdcHeader_t, ofsTagNames ), tags, sizeof( mdcTagName_t ), 1, &fileRanges[1] ) ||
		 !R_ModelRange( size, sizeof( mdcHeader_t ), R_MODEL_FIELD( data, mdcHeader_t, ofsTags ), frames * tags, sizeof( mdcTag_t ), 2, &fileRanges[2] ) ) {
		return "frames or tags outside the file";
	}
	for ( i = 0; i < tags; i++ ) {
		if ( !R_ModelName( data + fileRanges[1].offset + i * sizeof( mdcTagName_t ), sizeof( mdcTagName_t ) ) ) {
			return "unterminated tag name";
		}
	}

	surfaceOffset = R_MODEL_FIELD( data, mdcHeader_t, ofsSurfaces );
	if ( !R_ModelRange( size, sizeof( mdcHeader_t ), surfaceOffset, surfaces, sizeof( mdcSurface_t ), 4, &fileRanges[3] ) ) {
		return "surfaces outside the file";
	}
	for ( i = 0; i < surfaces; i++ ) {
		if ( ( error = R_ModelChainEntry( data, size, surfaceOffset, sizeof( mdcSurface_t ), offsetof( mdcSurface_t, ofsEnd ), &surfaceSize ) ) != NULL ) {
			return error;
		}
		surface = data + surfaceOffset;
		vertices = R_MODEL_FIELD( surface, mdcSurface_t, numVerts );
		triangles = R_MODEL_FIELD( surface, mdcSurface_t, numTriangles );
		shaders = R_MODEL_FIELD( surface, mdcSurface_t, numShaders );
		baseFrames = R_MODEL_FIELD( surface, mdcSurface_t, numBaseFrames );
		compFrames = R_MODEL_FIELD( surface, mdcSurface_t, numCompFrames );
		if ( baseFrames > frames || compFrames > frames ) {
			return "surface frame count exceeds the model";
		}
		if ( vertices >= SHADER_MAX_VERTEXES || vertices > MD3_MAX_VERTS ||
			 triangles >= SHADER_MAX_INDEXES / 3 || triangles > MD3_MAX_TRIANGLES || shaders > MD3_MAX_SHADERS ) {
			return "too many vertexes, triangles or shaders on a surface";
		}
		if ( !R_ModelName( surface + offsetof( mdcSurface_t, name ), MAX_QPATH ) ) {
			return "unterminated surface name";
		}
		if ( !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsTriangles ), triangles, sizeof( md3Triangle_t ), 4, &ranges[0] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsShaders ), shaders, sizeof( md3Shader_t ), 4, &ranges[1] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsSt ), vertices, sizeof( md3St_t ), 4, &ranges[2] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsXyzNormals ), vertices * baseFrames, sizeof( md3XyzNormal_t ), 2, &ranges[3] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsXyzCompressed ), vertices * compFrames, sizeof( mdcXyzCompressed_t ), 4, &ranges[4] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsFrameBaseFrames ), frames, sizeof( short ), 2, &ranges[5] ) ||
			 !R_ModelRange( surfaceSize, sizeof( mdcSurface_t ), R_MODEL_FIELD( surface, mdcSurface_t, ofsFrameCompFrames ), frames, sizeof( short ), 2, &ranges[6] ) ) {
			return "surface array outside the surface";
		}
		if ( !R_ModelDisjoint( ranges, 7 ) ) {
			return "overlapping surface arrays";
		}
		for ( j = 0; j < triangles * 3; j++ ) {
			if ( R_ModelWord( surface + ranges[0].offset + j * sizeof( int ) ) >= vertices ) {
				return "triangle vertex out of range";
			}
		}
		for ( j = 0; j < shaders; j++ ) {
			if ( !R_ModelName( surface + ranges[1].offset + j * sizeof( md3Shader_t ), MAX_QPATH ) ) {
				return "unterminated shader name";
			}
		}
		// every frame names a base frame, and a compressed frame when the surface has any
		for ( j = 0; j < frames; j++ ) {
			frame = R_ModelShort( surface + ranges[5].offset + j * sizeof( short ) );
			if ( frame < 0 || frame >= (int)baseFrames ) {
				return "base frame out of range";
			}
			frame = R_ModelShort( surface + ranges[6].offset + j * sizeof( short ) );
			if ( compFrames && frame >= (int)compFrames ) {
				return "compressed frame out of range";
			}
		}
		surfaceOffset += surfaceSize;
	}
	fileRanges[3].size = surfaceOffset - fileRanges[3].offset;
	if ( !R_ModelDisjoint( fileRanges, 4 ) ) {
		return "overlapping frames, tags and surfaces";
	}
	*validatedSize = size;
	return NULL;
}

static const char *R_ValidateMDS( const byte *data, int length, int *validatedSize ) {
	const byte *surface, *tag;
	unsigned int size, frames, bones, tags, surfaces, frameSize, surfaceOffset, surfaceSize, i;
	modelRange_t fileRanges[4];
	const char *error;

	if ( !data || length < (int)sizeof( mdsHeader_t ) ) {
		return "truncated header";
	}
	if ( R_MODEL_FIELD( data, mdsHeader_t, version ) != MDS_VERSION ) {
		return "wrong version";
	}
	size = R_MODEL_FIELD( data, mdsHeader_t, ofsEnd );
	frames = R_MODEL_FIELD( data, mdsHeader_t, numFrames );
	bones = R_MODEL_FIELD( data, mdsHeader_t, numBones );
	tags = R_MODEL_FIELD( data, mdsHeader_t, numTags );
	surfaces = R_MODEL_FIELD( data, mdsHeader_t, numSurfaces );
	if ( size < sizeof( mdsHeader_t ) || size > (unsigned int)length ) {
		return "end offset outside the file";
	}
	if ( frames < 1 || bones > MDS_MAX_BONES || tags > MDS_MAX_TAGS || surfaces > MDS_MAX_SURFACES ) {
		return "bad frame, bone, tag or surface count";
	}
	frameSize = offsetof( mdsFrame_t, bones ) + bones * sizeof( mdsBoneFrameCompressed_t );
	if ( !R_ModelRange( size, sizeof( mdsHeader_t ), R_MODEL_FIELD( data, mdsHeader_t, ofsFrames ), frames, frameSize, 4, &fileRanges[0] ) ||
		 !R_ModelRange( size, sizeof( mdsHeader_t ), R_MODEL_FIELD( data, mdsHeader_t, ofsBones ), bones, sizeof( mdsBoneInfo_t ), 4, &fileRanges[1] ) ||
		 !R_ModelRange( size, sizeof( mdsHeader_t ), R_MODEL_FIELD( data, mdsHeader_t, ofsTags ), tags, sizeof( mdsTag_t ), 4, &fileRanges[2] ) ) {
		return "frames, bones or tags outside the file";
	}
	if ( ( error = R_ValidateBoneInfo( data, fileRanges[1].offset, bones ) ) != NULL ) {
		return error;
	}
	for ( i = 0; i < tags; i++ ) {
		tag = data + fileRanges[2].offset + i * sizeof( mdsTag_t );
		if ( !R_ModelName( tag, MAX_QPATH ) ) {
			return "unterminated tag name";
		}
		if ( R_MODEL_FIELD( tag, mdsTag_t, boneIndex ) >= bones ) {
			return "tag bone out of range";
		}
	}

	surfaceOffset = R_MODEL_FIELD( data, mdsHeader_t, ofsSurfaces );
	if ( !R_ModelRange( size, sizeof( mdsHeader_t ), surfaceOffset, surfaces, sizeof( mdsSurface_t ), 4, &fileRanges[3] ) ) {
		return "surfaces outside the file";
	}
	for ( i = 0; i < surfaces; i++ ) {
		if ( ( error = R_ModelChainEntry( data, size, surfaceOffset, sizeof( mdsSurface_t ), offsetof( mdsSurface_t, ofsEnd ), &surfaceSize ) ) != NULL ) {
			return error;
		}
		surface = data + surfaceOffset;
		if ( ( error = R_ValidateSkeletalSurface( surface, surfaceOffset, surfaceSize, sizeof( mdsSurface_t ),
												  offsetof( mdsVertex_t, weights ), sizeof( mdsWeight_t ),
												  MDS_MAX_VERTS, MDS_MAX_TRIANGLES, bones ) ) != NULL ) {
			return error;
		}
		surfaceOffset += surfaceSize;
	}
	fileRanges[3].size = surfaceOffset - fileRanges[3].offset;
	if ( !R_ModelDisjoint( fileRanges, 4 ) ) {
		return "overlapping frames, bones, tags and surfaces";
	}
	*validatedSize = size;
	return NULL;
}

/*
MDM bone numbers index the skeleton of whichever MDX the entity animates with,
which is paired at render time. Here they are bounded by the renderer's bone
arrays; R_CalcBones checks them against the paired MDX.
*/
static const char *R_ValidateMDM( const byte *data, int length, int *validatedSize ) {
	const byte *surface, *tag;
	unsigned int size, tags, surfaces, surfaceOffset, surfaceSize, tagOffset, tagSize, i, j;
	modelRange_t fileRanges[2], references;
	const char *error;

	if ( !data || length < (int)sizeof( mdmHeader_t ) ) {
		return "truncated header";
	}
	if ( R_MODEL_FIELD( data, mdmHeader_t, version ) != MDM_VERSION ) {
		return "wrong version";
	}
	size = R_MODEL_FIELD( data, mdmHeader_t, ofsEnd );
	tags = R_MODEL_FIELD( data, mdmHeader_t, numTags );
	surfaces = R_MODEL_FIELD( data, mdmHeader_t, numSurfaces );
	if ( size < sizeof( mdmHeader_t ) || size > (unsigned int)length ) {
		return "end offset outside the file";
	}
	if ( tags > MDM_MAX_TAGS || surfaces > MDM_MAX_SURFACES ) {
		return "bad tag or surface count";
	}

	surfaceOffset = R_MODEL_FIELD( data, mdmHeader_t, ofsSurfaces );
	if ( !R_ModelRange( size, sizeof( mdmHeader_t ), surfaceOffset, surfaces, sizeof( mdmSurface_t ), 4, &fileRanges[0] ) ) {
		return "surfaces outside the file";
	}
	for ( i = 0; i < surfaces; i++ ) {
		if ( ( error = R_ModelChainEntry( data, size, surfaceOffset, sizeof( mdmSurface_t ), offsetof( mdmSurface_t, ofsEnd ), &surfaceSize ) ) != NULL ) {
			return error;
		}
		surface = data + surfaceOffset;
		if ( ( error = R_ValidateSkeletalSurface( surface, surfaceOffset, surfaceSize, sizeof( mdmSurface_t ),
												  offsetof( mdmVertex_t, weights ), sizeof( mdmWeight_t ),
												  MDM_MAX_VERTS, MDM_MAX_TRIANGLES, MDX_MAX_BONES ) ) != NULL ) {
			return error;
		}
		surfaceOffset += surfaceSize;
	}
	fileRanges[0].size = surfaceOffset - fileRanges[0].offset;

	// tags are a chain too, each with its own bone references
	tagOffset = R_MODEL_FIELD( data, mdmHeader_t, ofsTags );
	if ( !R_ModelRange( size, sizeof( mdmHeader_t ), tagOffset, tags, sizeof( mdmTag_t ), 4, &fileRanges[1] ) ) {
		return "tags outside the file";
	}
	for ( i = 0; i < tags; i++ ) {
		if ( ( error = R_ModelChainEntry( data, size, tagOffset, sizeof( mdmTag_t ), offsetof( mdmTag_t, ofsEnd ), &tagSize ) ) != NULL ) {
			return error;
		}
		tag = data + tagOffset;
		if ( !R_ModelName( tag, MAX_QPATH ) ) {
			return "unterminated tag name";
		}
		if ( R_MODEL_FIELD( tag, mdmTag_t, boneIndex ) >= MDX_MAX_BONES ) {
			return "tag bone out of range";
		}
		if ( !R_ModelRange( tagSize, sizeof( mdmTag_t ), R_MODEL_FIELD( tag, mdmTag_t, ofsBoneReferences ),
							R_MODEL_FIELD( tag, mdmTag_t, numBoneReferences ), sizeof( int ), 4, &references ) ) {
			return "tag bone references outside the tag";
		}
		for ( j = 0; j < references.size / sizeof( int ); j++ ) {
			if ( R_ModelWord( tag + references.offset + j * sizeof( int ) ) >= MDX_MAX_BONES ) {
				return "tag bone reference out of range";
			}
		}
		tagOffset += tagSize;
	}
	fileRanges[1].size = tagOffset - fileRanges[1].offset;
	if ( !R_ModelDisjoint( fileRanges, 2 ) ) {
		return "overlapping surfaces and tags";
	}
	*validatedSize = size;
	return NULL;
}

static const char *R_ValidateMDX( const byte *data, int length, int *validatedSize ) {
	unsigned int size, frames, bones, frameSize;
	modelRange_t fileRanges[2];
	const char *error;

	if ( !data || length < (int)sizeof( mdxHeader_t ) ) {
		return "truncated header";
	}
	if ( R_MODEL_FIELD( data, mdxHeader_t, version ) != MDX_VERSION ) {
		return "wrong version";
	}
	size = R_MODEL_FIELD( data, mdxHeader_t, ofsEnd );
	frames = R_MODEL_FIELD( data, mdxHeader_t, numFrames );
	bones = R_MODEL_FIELD( data, mdxHeader_t, numBones );
	if ( size < sizeof( mdxHeader_t ) || size > (unsigned int)length ) {
		return "end offset outside the file";
	}
	if ( frames < 1 || bones > MDX_MAX_BONES ) {
		return "bad frame or bone count";
	}
	frameSize = sizeof( mdxFrame_t ) + bones * sizeof( mdxBoneFrameCompressed_t );
	if ( !R_ModelRange( size, sizeof( mdxHeader_t ), R_MODEL_FIELD( data, mdxHeader_t, ofsFrames ), frames, frameSize, 4, &fileRanges[0] ) ||
		 !R_ModelRange( size, sizeof( mdxHeader_t ), R_MODEL_FIELD( data, mdxHeader_t, ofsBones ), bones, sizeof( mdxBoneInfo_t ), 4, &fileRanges[1] ) ) {
		return "frames or bones outside the file";
	}
	if ( ( error = R_ValidateBoneInfo( data, fileRanges[1].offset, bones ) ) != NULL ) {
		return error;
	}
	if ( !R_ModelDisjoint( fileRanges, 2 ) ) {
		return "overlapping frames and bones";
	}
	*validatedSize = size;
	return NULL;
}

/*
=================
R_LoadMDC
=================
*/
static qboolean R_LoadMDC( model_t *mod, int lod, void *buffer, int length, const char *mod_name ) {
	int i, j;
	md3Frame_t          *frame;
	mdcSurface_t        *surf;
	md3Shader_t         *shader;
	md3Triangle_t       *tri;
	md3St_t             *st;
	md3XyzNormal_t      *xyz;
	mdcXyzCompressed_t  *xyzComp;
	mdcTag_t            *tag;
	short               *ps;
	const char          *error;
	int size;

	error = R_ValidateMDC( buffer, length, &size );
	if ( error ) {
		ri.Printf( PRINT_WARNING, "R_LoadMDC: %s: %s\n", mod_name, error );
		return qfalse;
	}

	mod->type = MOD_MDC;
	mod->dataSize += size;
	mod->model.mdc[lod] = ri.Hunk_Alloc( size, h_low );

	memcpy( mod->model.mdc[lod], buffer, size );

	LL( mod->model.mdc[lod]->ident );
	LL( mod->model.mdc[lod]->version );
	LL( mod->model.mdc[lod]->numFrames );
	LL( mod->model.mdc[lod]->numTags );
	LL( mod->model.mdc[lod]->numSurfaces );
	LL( mod->model.mdc[lod]->ofsFrames );
	LL( mod->model.mdc[lod]->ofsTagNames );
	LL( mod->model.mdc[lod]->ofsTags );
	LL( mod->model.mdc[lod]->ofsSurfaces );
	LL( mod->model.mdc[lod]->ofsEnd );
	LL( mod->model.mdc[lod]->flags );
	LL( mod->model.mdc[lod]->numSkins );


	// swap all the frames
	frame = ( md3Frame_t * )( (byte *)mod->model.mdc[lod] + mod->model.mdc[lod]->ofsFrames );
	for ( i = 0 ; i < mod->model.mdc[lod]->numFrames ; i++, frame++ ) {
		frame->radius = LittleFloat( frame->radius );
		if ( strstr( mod->name,"sherman" ) || strstr( mod->name, "mg42" ) ) {
			frame->radius = 256;
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = 128;
				frame->bounds[1][j] = -128;
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
			}
		} else
		{
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = LittleFloat( frame->bounds[0][j] );
				frame->bounds[1][j] = LittleFloat( frame->bounds[1][j] );
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
			}
		}
	}

	// swap all the tags
	tag = ( mdcTag_t * )( (byte *)mod->model.mdc[lod] + mod->model.mdc[lod]->ofsTags );
	if ( LittleLong( 1 ) != 1 ) {
		for ( i = 0 ; i < mod->model.mdc[lod]->numTags * mod->model.mdc[lod]->numFrames ; i++, tag++ ) {
			for ( j = 0 ; j < 3 ; j++ ) {
				tag->xyz[j] = LittleShort( tag->xyz[j] );
				tag->angles[j] = LittleShort( tag->angles[j] );
			}
		}
	}

	// swap all the surfaces
	surf = ( mdcSurface_t * )( (byte *)mod->model.mdc[lod] + mod->model.mdc[lod]->ofsSurfaces );
	for ( i = 0 ; i < mod->model.mdc[lod]->numSurfaces ; i++ ) {

		LL( surf->ident );
		LL( surf->flags );
		LL( surf->numBaseFrames );
		LL( surf->numCompFrames );
		LL( surf->numShaders );
		LL( surf->numTriangles );
		LL( surf->ofsTriangles );
		LL( surf->numVerts );
		LL( surf->ofsShaders );
		LL( surf->ofsSt );
		LL( surf->ofsXyzNormals );
		LL( surf->ofsXyzCompressed );
		LL( surf->ofsFrameBaseFrames );
		LL( surf->ofsFrameCompFrames );
		LL( surf->ofsEnd );

		// change to surface identifier
		surf->ident = SF_MDC;

		// lowercase the surface name so skin compares are faster
		Q_strlwr( surf->name );

		// strip off a trailing _1 or _2
		// this is a crutch for q3data being a mess
		j = strlen( surf->name );
		if ( j > 2 && surf->name[j - 2] == '_' ) {
			surf->name[j - 2] = 0;
		}

		// register the shaders
		shader = ( md3Shader_t * )( (byte *)surf + surf->ofsShaders );
		for ( j = 0 ; j < surf->numShaders ; j++, shader++ ) {
			shader_t    *sh;

			sh = R_FindShader( shader->name, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				shader->shaderIndex = 0;
			} else {
				shader->shaderIndex = sh->index;
			}
		}

		// Ridah, optimization, only do the swapping if we really need to
		if ( LittleShort( 1 ) != 1 ) {

			// swap all the triangles
			tri = ( md3Triangle_t * )( (byte *)surf + surf->ofsTriangles );
			for ( j = 0 ; j < surf->numTriangles ; j++, tri++ ) {
				LL( tri->indexes[0] );
				LL( tri->indexes[1] );
				LL( tri->indexes[2] );
			}

			// swap all the ST
			st = ( md3St_t * )( (byte *)surf + surf->ofsSt );
			for ( j = 0 ; j < surf->numVerts ; j++, st++ ) {
				st->st[0] = LittleFloat( st->st[0] );
				st->st[1] = LittleFloat( st->st[1] );
			}

			// swap all the XyzNormals
			xyz = ( md3XyzNormal_t * )( (byte *)surf + surf->ofsXyzNormals );
			for ( j = 0 ; j < surf->numVerts * surf->numBaseFrames ; j++, xyz++ )
			{
				xyz->xyz[0] = LittleShort( xyz->xyz[0] );
				xyz->xyz[1] = LittleShort( xyz->xyz[1] );
				xyz->xyz[2] = LittleShort( xyz->xyz[2] );

				xyz->normal = LittleShort( xyz->normal );
			}

			// swap all the XyzCompressed
			xyzComp = ( mdcXyzCompressed_t * )( (byte *)surf + surf->ofsXyzCompressed );
			for ( j = 0 ; j < surf->numVerts * surf->numCompFrames ; j++, xyzComp++ )
			{
				LL( xyzComp->ofsVec );
			}

			// swap the frameBaseFrames
			ps = ( short * )( (byte *)surf + surf->ofsFrameBaseFrames );
			for ( j = 0; j < mod->model.mdc[lod]->numFrames; j++, ps++ )
			{
				*ps = LittleShort( *ps );
			}

			// swap the frameCompFrames
			ps = ( short * )( (byte *)surf + surf->ofsFrameCompFrames );
			for ( j = 0; j < mod->model.mdc[lod]->numFrames; j++, ps++ )
			{
				*ps = LittleShort( *ps );
			}
		}
		// done.

		// find the next surface
		surf = ( mdcSurface_t * )( (byte *)surf + surf->ofsEnd );
	}

	return qtrue;
}

// done.
//-------------------------------------------------------------------------------

/*
=================
R_LoadMD3
=================
*/
static qboolean R_LoadMD3( model_t *mod, int lod, void *buffer, int length, const char *mod_name ) {
	int i, j;
	md3Frame_t          *frame;
	md3Surface_t        *surf;
	md3Shader_t         *shader;
	md3Triangle_t       *tri;
	md3St_t             *st;
	md3XyzNormal_t      *xyz;
	md3Tag_t            *tag;
	const char          *error;
	int size;
	qboolean fixRadius = qfalse;

	error = R_ValidateMD3( buffer, length, &size );
	if ( error ) {
		ri.Printf( PRINT_WARNING, "R_LoadMD3: %s: %s\n", mod_name, error );
		return qfalse;
	}

	mod->type = MOD_MESH;
	mod->dataSize += size;
	mod->model.md3[lod] = ri.Hunk_Alloc( size, h_low );

	memcpy( mod->model.md3[lod], buffer, size );

	LL( mod->model.md3[lod]->ident );
	LL( mod->model.md3[lod]->version );
	LL( mod->model.md3[lod]->numFrames );
	LL( mod->model.md3[lod]->numTags );
	LL( mod->model.md3[lod]->numSurfaces );
	LL( mod->model.md3[lod]->ofsFrames );
	LL( mod->model.md3[lod]->ofsTags );
	LL( mod->model.md3[lod]->ofsSurfaces );
	LL( mod->model.md3[lod]->ofsEnd );

	if ( strstr( mod->name,"sherman" ) || strstr( mod->name, "mg42" ) ) {
		fixRadius = qtrue;
	}

	// swap all the frames
	frame = ( md3Frame_t * )( (byte *)mod->model.md3[lod] + mod->model.md3[lod]->ofsFrames );
	for ( i = 0 ; i < mod->model.md3[lod]->numFrames ; i++, frame++ ) {
		frame->radius = LittleFloat( frame->radius );
		if ( fixRadius ) {
			frame->radius = 256;
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = 128;
				frame->bounds[1][j] = -128;
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
			}
		}
		// Hack for Bug using plugin generated model
		else if ( frame->radius == 1 ) {
			frame->radius = 256;
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = 128;
				frame->bounds[1][j] = -128;
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
			}
		} else
		{
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = LittleFloat( frame->bounds[0][j] );
				frame->bounds[1][j] = LittleFloat( frame->bounds[1][j] );
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
			}
		}
	}

	// swap all the tags
	tag = ( md3Tag_t * )( (byte *)mod->model.md3[lod] + mod->model.md3[lod]->ofsTags );
	for ( i = 0 ; i < mod->model.md3[lod]->numTags * mod->model.md3[lod]->numFrames ; i++, tag++ ) {
		for ( j = 0 ; j < 3 ; j++ ) {
			tag->origin[j] = LittleFloat( tag->origin[j] );
			tag->axis[0][j] = LittleFloat( tag->axis[0][j] );
			tag->axis[1][j] = LittleFloat( tag->axis[1][j] );
			tag->axis[2][j] = LittleFloat( tag->axis[2][j] );
		}
	}

	// swap all the surfaces
	surf = ( md3Surface_t * )( (byte *)mod->model.md3[lod] + mod->model.md3[lod]->ofsSurfaces );
	for ( i = 0 ; i < mod->model.md3[lod]->numSurfaces ; i++ ) {

		LL( surf->ident );
		LL( surf->flags );
		LL( surf->numFrames );
		LL( surf->numShaders );
		LL( surf->numTriangles );
		LL( surf->ofsTriangles );
		LL( surf->numVerts );
		LL( surf->ofsShaders );
		LL( surf->ofsSt );
		LL( surf->ofsXyzNormals );
		LL( surf->ofsEnd );

		// change to surface identifier
		surf->ident = SF_MD3;

		// lowercase the surface name so skin compares are faster
		Q_strlwr( surf->name );

		// strip off a trailing _1 or _2
		// this is a crutch for q3data being a mess
		j = strlen( surf->name );
		if ( j > 2 && surf->name[j - 2] == '_' ) {
			surf->name[j - 2] = 0;
		}

		// register the shaders
		shader = ( md3Shader_t * )( (byte *)surf + surf->ofsShaders );
		for ( j = 0 ; j < surf->numShaders ; j++, shader++ ) {
			shader_t    *sh;

			sh = R_FindShader( shader->name, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				shader->shaderIndex = 0;
			} else {
				shader->shaderIndex = sh->index;
			}
		}

		// Ridah, optimization, only do the swapping if we really need to
		if ( LittleShort( 1 ) != 1 ) {

			// swap all the triangles
			tri = ( md3Triangle_t * )( (byte *)surf + surf->ofsTriangles );
			for ( j = 0 ; j < surf->numTriangles ; j++, tri++ ) {
				LL( tri->indexes[0] );
				LL( tri->indexes[1] );
				LL( tri->indexes[2] );
			}

			// swap all the ST
			st = ( md3St_t * )( (byte *)surf + surf->ofsSt );
			for ( j = 0 ; j < surf->numVerts ; j++, st++ ) {
				st->st[0] = LittleFloat( st->st[0] );
				st->st[1] = LittleFloat( st->st[1] );
			}

			// swap all the XyzNormals
			xyz = ( md3XyzNormal_t * )( (byte *)surf + surf->ofsXyzNormals );
			for ( j = 0 ; j < surf->numVerts * surf->numFrames ; j++, xyz++ )
			{
				xyz->xyz[0] = LittleShort( xyz->xyz[0] );
				xyz->xyz[1] = LittleShort( xyz->xyz[1] );
				xyz->xyz[2] = LittleShort( xyz->xyz[2] );

				xyz->normal = LittleShort( xyz->normal );
			}

		}
		// done.

		// find the next surface
		surf = ( md3Surface_t * )( (byte *)surf + surf->ofsEnd );
	}

	return qtrue;
}


/*
=================
R_LoadMDS
=================
*/
static qboolean R_LoadMDS( model_t *mod, void *buffer, int length, const char *mod_name ) {
	int i, j, k;
	mdsHeader_t         *mds;
	mdsFrame_t          *frame;
	mdsSurface_t        *surf;
	mdsTriangle_t       *tri;
	mdsVertex_t         *v;
	mdsBoneInfo_t       *bi;
	mdsTag_t            *tag;
	const char          *error;
	int size;
	shader_t            *sh;
	int frameSize;
	int                 *collapseMap, *boneref;

	error = R_ValidateMDS( buffer, length, &size );
	if ( error ) {
		ri.Printf( PRINT_WARNING, "R_LoadMDS: %s: %s\n", mod_name, error );
		return qfalse;
	}

	mod->type = MOD_MDS;
	mod->dataSize += size;
	mds = mod->model.mds = ri.Hunk_Alloc( size, h_low );

	memcpy( mds, buffer, size );

	LL( mds->ident );
	LL( mds->version );
	LL( mds->numFrames );
	LL( mds->numBones );
	LL( mds->numTags );
	LL( mds->numSurfaces );
	LL( mds->ofsFrames );
	LL( mds->ofsBones );
	LL( mds->ofsTags );
	LL( mds->ofsEnd );
	LL( mds->ofsSurfaces );
	mds->lodBias = LittleFloat( mds->lodBias );
	mds->lodScale = LittleFloat( mds->lodScale );
	LL( mds->torsoParent );

	if ( LittleLong( 1 ) != 1 ) {
		// swap all the frames
		//frameSize = (int)( &((mdsFrame_t *)0)->bones[ mds->numBones ] );
		frameSize = (int) ( sizeof( mdsFrame_t ) - sizeof( mdsBoneFrameCompressed_t ) + mds->numBones * sizeof( mdsBoneFrameCompressed_t ) );
		for ( i = 0 ; i < mds->numFrames ; i++, frame++ ) {
			frame = ( mdsFrame_t * )( (byte *)mds + mds->ofsFrames + i * frameSize );
			frame->radius = LittleFloat( frame->radius );
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = LittleFloat( frame->bounds[0][j] );
				frame->bounds[1][j] = LittleFloat( frame->bounds[1][j] );
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
				frame->parentOffset[j] = LittleFloat( frame->parentOffset[j] );
			}
			for ( j = 0 ; j < mds->numBones * sizeof( mdsBoneFrameCompressed_t ) / sizeof( short ) ; j++ ) {
				( (short *)frame->bones )[j] = LittleShort( ( (short *)frame->bones )[j] );
			}
		}

		// swap all the tags
		tag = ( mdsTag_t * )( (byte *)mds + mds->ofsTags );
		for ( i = 0 ; i < mds->numTags ; i++, tag++ ) {
			LL( tag->boneIndex );
			tag->torsoWeight = LittleFloat( tag->torsoWeight );
		}

		// swap all the bones
		for ( i = 0 ; i < mds->numBones ; i++, bi++ ) {
			bi = ( mdsBoneInfo_t * )( (byte *)mds + mds->ofsBones + i * sizeof( mdsBoneInfo_t ) );
			LL( bi->parent );
			bi->torsoWeight = LittleFloat( bi->torsoWeight );
			bi->parentDist = LittleFloat( bi->parentDist );
			LL( bi->flags );
		}
	}

	// swap all the surfaces
	surf = ( mdsSurface_t * )( (byte *)mds + mds->ofsSurfaces );
	for ( i = 0 ; i < mds->numSurfaces ; i++ ) {
		if ( LittleLong( 1 ) != 1 ) {
			//LL(surf->ident);
			LL( surf->shaderIndex );
			LL( surf->minLod );
			LL( surf->ofsHeader );
			LL( surf->ofsCollapseMap );
			LL( surf->numTriangles );
			LL( surf->ofsTriangles );
			LL( surf->numVerts );
			LL( surf->ofsVerts );
			LL( surf->numBoneReferences );
			LL( surf->ofsBoneReferences );
			LL( surf->ofsEnd );
		}

		// change to surface identifier
		surf->ident = SF_MDS;

		// register the shaders
		if ( surf->shader[0] ) {
			sh = R_FindShader( surf->shader, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				surf->shaderIndex = 0;
			} else {
				surf->shaderIndex = sh->index;
			}
		} else {
			surf->shaderIndex = 0;
		}

		if ( LittleLong( 1 ) != 1 ) {
			// swap all the triangles
			tri = ( mdsTriangle_t * )( (byte *)surf + surf->ofsTriangles );
			for ( j = 0 ; j < surf->numTriangles ; j++, tri++ ) {
				LL( tri->indexes[0] );
				LL( tri->indexes[1] );
				LL( tri->indexes[2] );
			}

			// swap all the vertexes
			v = ( mdsVertex_t * )( (byte *)surf + surf->ofsVerts );
			for ( j = 0 ; j < surf->numVerts ; j++ ) {
				v->normal[0] = LittleFloat( v->normal[0] );
				v->normal[1] = LittleFloat( v->normal[1] );
				v->normal[2] = LittleFloat( v->normal[2] );

				v->texCoords[0] = LittleFloat( v->texCoords[0] );
				v->texCoords[1] = LittleFloat( v->texCoords[1] );

				v->numWeights = LittleLong( v->numWeights );

				for ( k = 0 ; k < v->numWeights ; k++ ) {
					v->weights[k].boneIndex = LittleLong( v->weights[k].boneIndex );
					v->weights[k].boneWeight = LittleFloat( v->weights[k].boneWeight );
					v->weights[k].offset[0] = LittleFloat( v->weights[k].offset[0] );
					v->weights[k].offset[1] = LittleFloat( v->weights[k].offset[1] );
					v->weights[k].offset[2] = LittleFloat( v->weights[k].offset[2] );
				}

				// find the fixedParent for this vert (if exists)
				v->fixedParent = -1;
				if ( v->numWeights == 2 ) {
					// find the closest parent
					if ( VectorLength( v->weights[0].offset ) < VectorLength( v->weights[1].offset ) ) {
						v->fixedParent = 0;
					} else {
						v->fixedParent = 1;
					}
					v->fixedDist = VectorLength( v->weights[v->fixedParent].offset );
				}

				v = (mdsVertex_t *)&v->weights[v->numWeights];
			}

			// swap the collapse map
			collapseMap = ( int * )( (byte *)surf + surf->ofsCollapseMap );
			for ( j = 0; j < surf->numVerts; j++, collapseMap++ ) {
				*collapseMap = LittleLong( *collapseMap );
			}

			// swap the bone references
			boneref = ( int * )( ( byte *)surf + surf->ofsBoneReferences );
			for ( j = 0; j < surf->numBoneReferences; j++, boneref++ ) {
				*boneref = LittleLong( *boneref );
			}
		}

		// find the next surface
		surf = ( mdsSurface_t * )( (byte *)surf + surf->ofsEnd );
	}

	return qtrue;
}

/*
=================
R_LoadMDM
=================
*/
static qboolean R_LoadMDM( model_t *mod, void *buffer, int length, const char *mod_name ) {
	int i, j, k;
	mdmHeader_t         *mdm;
//    mdmFrame_t			*frame;
	mdmSurface_t        *surf;
	mdmTriangle_t       *tri;
	mdmVertex_t         *v;
	mdmTag_t            *tag;
	const char          *error;
	int size;
	shader_t            *sh;
	int                 *collapseMap, *boneref;

	error = R_ValidateMDM( buffer, length, &size );
	if ( error ) {
		ri.Printf( PRINT_WARNING, "R_LoadMDM: %s: %s\n", mod_name, error );
		return qfalse;
	}

	mod->type = MOD_MDM;
	mod->dataSize += size;
	mdm = mod->model.mdm = ri.Hunk_Alloc( size, h_low );

	memcpy( mdm, buffer, size );

	LL( mdm->ident );
	LL( mdm->version );
//    LL(mdm->numFrames);
	LL( mdm->numTags );
	LL( mdm->numSurfaces );
//    LL(mdm->ofsFrames);
	LL( mdm->ofsTags );
	LL( mdm->ofsEnd );
	LL( mdm->ofsSurfaces );
	mdm->lodBias = LittleFloat( mdm->lodBias );
	mdm->lodScale = LittleFloat( mdm->lodScale );

/*	mdm->skel = RE_RegisterModel(mdm->bonesfile);
	if ( !mdm->skel ) {
		ri.Error (ERR_DROP, "R_LoadMDM: %s skeleton not found\n", mdm->bonesfile );
	}

	if ( mdm->numFrames < 1 ) {
		ri.Printf( PRINT_WARNING, "R_LoadMDM: %s has no frames\n", mod_name );
		return qfalse;
	}*/

	if ( LittleLong( 1 ) != 1 ) {
		// swap all the frames
		/*frameSize = (int) ( sizeof( mdmFrame_t ) );
		for ( i = 0 ; i < mdm->numFrames ; i++, frame++) {
			frame = (mdmFrame_t *) ( (byte *)mdm + mdm->ofsFrames + i * frameSize );
			frame->radius = LittleFloat( frame->radius );
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = LittleFloat( frame->bounds[0][j] );
				frame->bounds[1][j] = LittleFloat( frame->bounds[1][j] );
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
				frame->parentOffset[j] = LittleFloat( frame->parentOffset[j] );
			}
		}*/

		// swap all the tags
		tag = ( mdmTag_t * )( (byte *)mdm + mdm->ofsTags );
		for ( i = 0 ; i < mdm->numTags ; i++ ) {
			int ii;
			for ( ii = 0; ii < 3; ii++ )
			{
				tag->axis[ii][0] = LittleFloat( tag->axis[ii][0] );
				tag->axis[ii][1] = LittleFloat( tag->axis[ii][1] );
				tag->axis[ii][2] = LittleFloat( tag->axis[ii][2] );
			}

			LL( tag->boneIndex );
			//tag->torsoWeight = LittleFloat( tag->torsoWeight );
			tag->offset[0] = LittleFloat( tag->offset[0] );
			tag->offset[1] = LittleFloat( tag->offset[1] );
			tag->offset[2] = LittleFloat( tag->offset[2] );

			LL( tag->numBoneReferences );
			LL( tag->ofsBoneReferences );
			LL( tag->ofsEnd );

			// swap the bone references
			boneref = ( int * )( ( byte *)tag + tag->ofsBoneReferences );
			for ( j = 0; j < tag->numBoneReferences; j++, boneref++ ) {
				*boneref = LittleLong( *boneref );
			}

			// find the next tag
			tag = ( mdmTag_t * )( (byte *)tag + tag->ofsEnd );
		}
	}

	// swap all the surfaces
	surf = ( mdmSurface_t * )( (byte *)mdm + mdm->ofsSurfaces );
	for ( i = 0 ; i < mdm->numSurfaces ; i++ ) {
		if ( LittleLong( 1 ) != 1 ) {
			//LL(surf->ident);
			LL( surf->shaderIndex );
			LL( surf->minLod );
			LL( surf->ofsHeader );
			LL( surf->ofsCollapseMap );
			LL( surf->numTriangles );
			LL( surf->ofsTriangles );
			LL( surf->numVerts );
			LL( surf->ofsVerts );
			LL( surf->numBoneReferences );
			LL( surf->ofsBoneReferences );
			LL( surf->ofsEnd );
		}

		// change to surface identifier
		surf->ident = SF_MDM;

		// register the shaders
		if ( surf->shader[0] ) {
			sh = R_FindShader( surf->shader, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				surf->shaderIndex = 0;
			} else {
				surf->shaderIndex = sh->index;
			}
		} else {
			surf->shaderIndex = 0;
		}

		if ( LittleLong( 1 ) != 1 ) {
			// swap all the triangles
			tri = ( mdmTriangle_t * )( (byte *)surf + surf->ofsTriangles );
			for ( j = 0 ; j < surf->numTriangles ; j++, tri++ ) {
				LL( tri->indexes[0] );
				LL( tri->indexes[1] );
				LL( tri->indexes[2] );
			}

			// swap all the vertexes
			v = ( mdmVertex_t * )( (byte *)surf + surf->ofsVerts );
			for ( j = 0 ; j < surf->numVerts ; j++ ) {
				v->normal[0] = LittleFloat( v->normal[0] );
				v->normal[1] = LittleFloat( v->normal[1] );
				v->normal[2] = LittleFloat( v->normal[2] );

				v->texCoords[0] = LittleFloat( v->texCoords[0] );
				v->texCoords[1] = LittleFloat( v->texCoords[1] );

				v->numWeights = LittleLong( v->numWeights );

				for ( k = 0 ; k < v->numWeights ; k++ ) {
					v->weights[k].boneIndex = LittleLong( v->weights[k].boneIndex );
					v->weights[k].boneWeight = LittleFloat( v->weights[k].boneWeight );
					v->weights[k].offset[0] = LittleFloat( v->weights[k].offset[0] );
					v->weights[k].offset[1] = LittleFloat( v->weights[k].offset[1] );
					v->weights[k].offset[2] = LittleFloat( v->weights[k].offset[2] );
				}

				v = (mdmVertex_t *)&v->weights[v->numWeights];
			}

			// swap the collapse map
			collapseMap = ( int * )( (byte *)surf + surf->ofsCollapseMap );
			for ( j = 0; j < surf->numVerts; j++, collapseMap++ ) {
				*collapseMap = LittleLong( *collapseMap );
			}

			// swap the bone references
			boneref = ( int * )( ( byte *)surf + surf->ofsBoneReferences );
			for ( j = 0; j < surf->numBoneReferences; j++, boneref++ ) {
				*boneref = LittleLong( *boneref );
			}
		}

		// find the next surface
		surf = ( mdmSurface_t * )( (byte *)surf + surf->ofsEnd );
	}

	return qtrue;
}

/*
=================
R_LoadMDX
=================
*/
static qboolean R_LoadMDX( model_t *mod, void *buffer, int length, const char *mod_name ) {
	int i, j;
	mdxHeader_t                 *mdx;
	mdxFrame_t                  *frame;
	short                       *bframe;
	mdxBoneInfo_t               *bi;
	const char          *error;
	int size;
	int frameSize;

	error = R_ValidateMDX( buffer, length, &size );
	if ( error ) {
		ri.Printf( PRINT_WARNING, "R_LoadMDX: %s: %s\n", mod_name, error );
		return qfalse;
	}

	mod->type = MOD_MDX;
	mod->dataSize += size;
	mdx = mod->model.mdx = ri.Hunk_Alloc( size, h_low );

	memcpy( mdx, buffer, size );

	LL( mdx->ident );
	LL( mdx->version );
	LL( mdx->numFrames );
	LL( mdx->numBones );
	LL( mdx->ofsFrames );
	LL( mdx->ofsBones );
	LL( mdx->ofsEnd );
	LL( mdx->torsoParent );

	if ( LittleLong( 1 ) != 1 ) {
		// swap all the frames
		frameSize = (int) ( sizeof( mdxBoneFrameCompressed_t ) ) * mdx->numBones;
		for ( i = 0 ; i < mdx->numFrames ; i++ ) {
			frame = ( mdxFrame_t * )( (byte *)mdx + mdx->ofsFrames + i * frameSize + i * sizeof( mdxFrame_t ) );
			frame->radius = LittleFloat( frame->radius );
			for ( j = 0 ; j < 3 ; j++ ) {
				frame->bounds[0][j] = LittleFloat( frame->bounds[0][j] );
				frame->bounds[1][j] = LittleFloat( frame->bounds[1][j] );
				frame->localOrigin[j] = LittleFloat( frame->localOrigin[j] );
				frame->parentOffset[j] = LittleFloat( frame->parentOffset[j] );
			}

			bframe = ( short * )( (byte *)mdx + mdx->ofsFrames + i * frameSize + ( ( i + 1 ) * sizeof( mdxFrame_t ) ) );
			for ( j = 0 ; j < mdx->numBones * sizeof( mdxBoneFrameCompressed_t ) / sizeof( short ) ; j++ ) {
				( (short *)bframe )[j] = LittleShort( ( (short *)bframe )[j] );
			}
		}

		// swap all the bones
		for ( i = 0 ; i < mdx->numBones ; i++ ) {
			bi = ( mdxBoneInfo_t * )( (byte *)mdx + mdx->ofsBones + i * sizeof( mdxBoneInfo_t ) );
			LL( bi->parent );
			bi->torsoWeight = LittleFloat( bi->torsoWeight );
			bi->parentDist = LittleFloat( bi->parentDist );
			LL( bi->flags );
		}
	}

	return qtrue;
}


//=============================================================================

/*
** RE_BeginRegistration
*/
void RE_BeginRegistration( glconfig_t *glconfigOut ) {
	ri.Hunk_Clear();    // (SA) MEM NOTE: not in missionpack

	R_Init();
	*glconfigOut = glConfig;

	R_SyncRenderThread();

	tr.viewCluster = -1;        // force markleafs to regenerate
	R_ClearFlares();
	RE_ClearScene();

	tr.registered = qtrue;

	// NOTE: this sucks, for some reason the first stretch pic is never drawn
	// without this we'd see a white flash on a level load because the very
	// first time the level shot would not be drawn
	RE_StretchPic( 0, 0, 0, 0, 0, 0, 1, 1, 0 );
}

/*
===============
R_ModelInit
===============
*/
void R_ModelInit( void ) {
	model_t     *mod;

	// leave a space for NULL model
	tr.numModels = 0;

	mod = R_AllocModel();
	if ( !mod ) {
		ri.Error( ERR_DROP, "R_ModelInit: R_AllocModel failed" );
	}
	mod->type = MOD_BAD;

	// Ridah, load in the cacheModels
	R_LoadCacheModels();
	// done.
}


/*
================
R_Modellist_f
================
*/

void R_Modellist_f( void ) {
	int i, j;
	model_t *mod;
	int total;
	int lods;

	total = 0;
	for ( i = 1 ; i < tr.numModels; i++ ) {
		mod = tr.models[i];
		lods = 1;
		for ( j = 1 ; j < MD3_MAX_LODS ; j++ ) {
			if ( mod->model.md3[j] && mod->model.md3[j] != mod->model.md3[j - 1] ) {
				lods++;
			}
		}
		ri.Printf( PRINT_ALL, "%8i : (%i) %s\n",mod->dataSize, lods, mod->name );
		total += mod->dataSize;
	}
	ri.Printf( PRINT_ALL, "%8i : Total models\n", total );

#if 0       // not working right with new hunk
	if ( tr.world ) {
		ri.Printf( PRINT_ALL, "\n%8i : %s\n", tr.world->dataSize, tr.world->name );
	}
#endif
}


//=============================================================================


/*
================
R_GetTag
================
*/
static int R_GetTag( byte *mod, int frame, const char *tagName, int startTagIndex, md3Tag_t **outTag ) {
	md3Tag_t        *tag;
	int i;
	md3Header_t     *md3;

	md3 = (md3Header_t *) mod;

	if ( frame >= md3->numFrames ) {
		// it is possible to have a bad frame while changing models, so don't error
		frame = md3->numFrames - 1;
	}
	if ( frame < 0 ) {
		frame = 0;
	}

	if ( startTagIndex > md3->numTags ) {
		*outTag = NULL;
		return -1;
	}

	tag = ( md3Tag_t * )( (byte *)mod + md3->ofsTags ) + frame * md3->numTags;
	for ( i = 0 ; i < md3->numTags ; i++, tag++ ) {
		if ( ( i >= startTagIndex ) && !strcmp( tag->name, tagName ) ) {

			// if we are looking for an indexed tag, wait until we find the correct number of matches
			//if (startTagIndex) {
			//	startTagIndex--;
			//	continue;
			//}

			*outTag = tag;
			return i;   // found it
		}
	}

	*outTag = NULL;
	return -1;
}

/*
================
R_GetMDCTag
================
*/
static int R_GetMDCTag( byte *mod, int frame, const char *tagName, int startTagIndex, mdcTag_t **outTag ) {
	mdcTag_t        *tag;
	mdcTagName_t    *pTagName;
	int i;
	mdcHeader_t     *mdc;

	mdc = (mdcHeader_t *) mod;

	if ( frame >= mdc->numFrames ) {
		// it is possible to have a bad frame while changing models, so don't error
		frame = mdc->numFrames - 1;
	}
	if ( frame < 0 ) {
		frame = 0;
	}

	if ( startTagIndex > mdc->numTags ) {
		*outTag = NULL;
		return -1;
	}

	pTagName = ( mdcTagName_t * )( (byte *)mod + mdc->ofsTagNames );
	for ( i = 0 ; i < mdc->numTags ; i++, pTagName++ ) {
		if ( ( i >= startTagIndex ) && !strcmp( pTagName->name, tagName ) ) {
			break;  // found it
		}
	}

	if ( i >= mdc->numTags ) {
		*outTag = NULL;
		return -1;
	}

	tag = ( mdcTag_t * )( (byte *)mod + mdc->ofsTags ) + frame * mdc->numTags + i;
	*outTag = tag;
	return i;
}

/*
================
R_GetMDSTag
================
*/
/*
// TTimo: unused
static int R_GetMDSTag( byte *mod, const char *tagName, int startTagIndex, mdsTag_t **outTag ) {
	mdsTag_t		*tag;
	int				i;
	mdsHeader_t		*mds;

	mds = (mdsHeader_t *) mod;

	if (startTagIndex > mds->numTags) {
		*outTag = NULL;
		return -1;
	}

	tag = (mdsTag_t *)((byte *)mod + mds->ofsTags);
	for ( i = 0 ; i < mds->numTags ; i++ ) {
		if ( (i >= startTagIndex) && !strcmp( tag->name, tagName ) ) {
			break;	// found it
		}

		tag = (mdsTag_t *) ((byte *)tag + sizeof(mdsTag_t) - sizeof(mdsBoneFrameCompressed_t) + mds->numFrames * sizeof(mdsBoneFrameCompressed_t) );
	}

	if (i >= mds->numTags) {
		*outTag = NULL;
		return -1;
	}

	*outTag = tag;
	return i;
}
*/

/*
================
R_LerpTag

  returns the index of the tag it found, for cycling through tags with the same name
================
*/
int R_LerpTag( orientation_t *tag, const refEntity_t *refent, const char *tagNameIn, int startIndex ) {
	md3Tag_t    *start, *end;
	md3Tag_t ustart, uend;
	int i;
	float frontLerp, backLerp;
	model_t     *model;
	vec3_t sangles, eangles;
	char tagName[MAX_QPATH];       //, *ch;
	int retval;
	qhandle_t handle;
	int startFrame, endFrame;
	float frac;

	handle = refent->hModel;
	startFrame = refent->oldframe;
	endFrame = refent->frame;
	frac = 1.0 - refent->backlerp;

	Q_strncpyz( tagName, tagNameIn, MAX_QPATH );
/*
	// if the tagName has a space in it, then it is passing through the starting tag number
	if (ch = strrchr(tagName, ' ')) {
		*ch = 0;
		ch++;
		startIndex = atoi(ch);
	}
*/
	model = R_GetModelByHandle( handle );
	if ( !model->model.md3[0] && !model->model.mdc[0] && !model->model.mds ) {
		AxisClear( tag->axis );
		VectorClear( tag->origin );
		return -1;
	}

	frontLerp = frac;
	backLerp = 1.0 - frac;

	if ( model->type == MOD_MESH ) {
		// old MD3 style
		retval = R_GetTag( (byte *)model->model.md3[0], startFrame, tagName, startIndex, &start );
		retval = R_GetTag( (byte *)model->model.md3[0], endFrame, tagName, startIndex, &end );

	} else if ( model->type == MOD_MDS ) {    // use bone lerping

		retval = R_GetBoneTag( tag, model->model.mds, startIndex, refent, tagNameIn );

		if ( retval >= 0 ) {
			return retval;
		}

		// failed
		return -1;

	} else if ( model->type == MOD_MDM ) {    // use bone lerping

		retval = R_MDM_GetBoneTag( tag, model->model.mdm, startIndex, refent, tagNameIn );

		if ( retval >= 0 ) {
			return retval;
		}

		// failed
		return -1;

	} else if ( model->type != MOD_MDC ) {
		// a brush model or skeleton has no tags, and its pointer is not an MDC
		start = NULL;
		end = NULL;
		retval = -1;
	} else {
		// psuedo-compressed MDC tags
		mdcTag_t    *cstart, *cend;

		retval = R_GetMDCTag( (byte *)model->model.mdc[0], startFrame, tagName, startIndex, &cstart );
		retval = R_GetMDCTag( (byte *)model->model.mdc[0], endFrame, tagName, startIndex, &cend );

		// uncompress the MDC tags into MD3 style tags
		if ( cstart && cend ) {
			for ( i = 0; i < 3; i++ ) {
				ustart.origin[i] = (float)cstart->xyz[i] * MD3_XYZ_SCALE;
				uend.origin[i] = (float)cend->xyz[i] * MD3_XYZ_SCALE;
				sangles[i] = (float)cstart->angles[i] * MDC_TAG_ANGLE_SCALE;
				eangles[i] = (float)cend->angles[i] * MDC_TAG_ANGLE_SCALE;
			}

			AnglesToAxis( sangles, ustart.axis );
			AnglesToAxis( eangles, uend.axis );

			start = &ustart;
			end = &uend;
		} else {
			start = NULL;
			end = NULL;
		}

	}

	if ( !start || !end ) {
		AxisClear( tag->axis );
		VectorClear( tag->origin );
		return -1;
	}

	for ( i = 0 ; i < 3 ; i++ ) {
		tag->origin[i] = start->origin[i] * backLerp +  end->origin[i] * frontLerp;
		tag->axis[0][i] = start->axis[0][i] * backLerp +  end->axis[0][i] * frontLerp;
		tag->axis[1][i] = start->axis[1][i] * backLerp +  end->axis[1][i] * frontLerp;
		tag->axis[2][i] = start->axis[2][i] * backLerp +  end->axis[2][i] * frontLerp;
	}

	VectorNormalize( tag->axis[0] );
	VectorNormalize( tag->axis[1] );
	VectorNormalize( tag->axis[2] );

	return retval;
}

/*
===============
R_TagInfo_f
===============
*/
void R_TagInfo_f( void ) {

	Com_Printf( "command not functional\n" );

/*
	int handle;
	orientation_t tag;
	int frame = -1;

	if (ri.Cmd_Argc() < 3) {
		Com_Printf("usage: taginfo <model> <tag>\n");
		return;
	}

	handle = RE_RegisterModel( ri.Cmd_Argv(1) );

	if (handle) {
		Com_Printf("found model %s..\n", ri.Cmd_Argv(1));
	} else {
		Com_Printf("cannot find model %s\n", ri.Cmd_Argv(1));
		return;
	}

	if (ri.Cmd_Argc() < 3) {
		frame = 0;
	} else {
		frame = atoi(ri.Cmd_Argv(3));
	}

	Com_Printf("using frame %i..\n", frame);

	R_LerpTag( &tag, handle, frame, frame, 0.0, (const char *)ri.Cmd_Argv(2) );

	Com_Printf("%s at position: %.1f %.1f %.1f\n", ri.Cmd_Argv(2), tag.origin[0], tag.origin[1], tag.origin[2] );
*/
}

/*
====================
R_ModelBounds
====================
*/
void R_ModelBounds( qhandle_t handle, vec3_t mins, vec3_t maxs ) {
	model_t     *model;
	md3Header_t *header;
	md3Frame_t  *frame;

	model = R_GetModelByHandle( handle );

	// Gordon: fixing now that it's a union
	switch ( model->type ) {
	case MOD_BRUSH:
		VectorCopy( model->model.bmodel->bounds[0], mins );
		VectorCopy( model->model.bmodel->bounds[1], maxs );
		return;
	case MOD_MESH:
		header = model->model.md3[0];

		frame = ( md3Frame_t * )( (byte *)header + header->ofsFrames );

		VectorCopy( frame->bounds[0], mins );
		VectorCopy( frame->bounds[1], maxs );
		return;
	case MOD_MDC:
		frame = ( md3Frame_t * )( (byte *)model->model.mdc[0] + model->model.mdc[0]->ofsFrames );

		VectorCopy( frame->bounds[0], mins );
		VectorCopy( frame->bounds[1], maxs );
		return;
	default:
		break;
	}

	VectorClear( mins );
	VectorClear( maxs );
	// done.
}

//---------------------------------------------------------------------------
// Virtual Memory, used for model caching, since we can't allocate them
// in the main Hunk (since it gets cleared on level changes), and they're
// too large to go into the Zone, we have a special memory chunk just for
// caching models in between levels.
//
// Optimized for Win32 systems, so that they'll grow the swapfile at startup
// if needed, but won't actually commit it until it's needed.
//
// GOAL: reserve a big chunk of virtual memory for the media cache, and only
// use it when we actually need it. This will make sure the swap file grows
// at startup if needed, rather than each allocation we make.
byte    *membase = NULL;
int hunkmaxsize;
int cursize;

#define R_HUNK_MEGS     24
#define R_HUNK_SIZE     ( R_HUNK_MEGS*1024*1024 )

void *R_Hunk_Begin( void ) {
	int maxsize = R_HUNK_SIZE;

	//Com_Printf("R_Hunk_Begin\n");

	// reserve a huge chunk of memory, but don't commit any yet
	cursize = 0;
	hunkmaxsize = maxsize;

	if ( !membase ) {
#ifdef _WIN32
		// this will "reserve" a chunk of memory for use by this application
		// it will not be "committed" just yet, but the swap file will grow
		// now if needed
		membase = VirtualAlloc( NULL, maxsize, MEM_RESERVE, PAGE_NOACCESS );
#else
		// show_bug.cgi?id=440
		// if not win32, then just allocate it now
		// it is possible that we have been allocated already, in case we don't do anything
		membase = malloc( maxsize );
		if ( !membase ) {
			ri.Error( ERR_DROP, "R_Hunk_Begin: reserve failed" );
		}
		// TTimo NOTE: initially, I was doing the memset even if we had an existing membase
		// but this breaks some shaders (i.e. /map mp_beach, then go back to the main menu .. some shaders are missing)
		// I assume the shader missing is because we don't clear memory either on win32
		// meaning even on win32 we are using memory that is still reserved but was uncommited .. it works out of pure luck
		memset( membase, 0, maxsize );
#endif
	}

	if ( !membase ) {
		ri.Error( ERR_DROP, "R_Hunk_Begin: reserve failed" );
	}

	return (void *)membase;
}

void *R_Hunk_Alloc( int size ) {
#ifdef _WIN32
	void    *buf;
#endif

	//Com_Printf("R_Hunk_Alloc(%d)\n", size);

	// round to cacheline
	size = ( size + 31 ) & ~31;

#ifdef _WIN32

	// commit pages as needed
	buf = VirtualAlloc( membase, cursize + size, MEM_COMMIT, PAGE_READWRITE );

	if ( !buf ) {
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), (LPTSTR) &buf, 0, NULL );
		ri.Error( ERR_DROP, "VirtualAlloc commit failed.\n%s", buf );
	}

#endif

	cursize += size;
	if ( cursize > hunkmaxsize ) {
		ri.Error( ERR_DROP, "R_Hunk_Alloc overflow" );
	}

	return ( void * )( membase + cursize - size );
}

// this is only called when we shutdown GL
void R_Hunk_End( void ) {
	//Com_Printf("R_Hunk_End\n");

	if ( membase ) {
#ifdef _WIN32
		VirtualFree( membase, 0, MEM_RELEASE );
#else
		free( membase );
#endif
	}

	membase = NULL;
}

void R_Hunk_Reset( void ) {
	//Com_Printf("R_Hunk_Reset\n");

	if ( !membase ) {
		ri.Error( ERR_DROP, "R_Hunk_Reset called without a membase!" );
	}

#ifdef _WIN32
	// mark the existing committed pages as reserved, but not committed
	VirtualFree( membase, cursize, MEM_DECOMMIT );
#endif
	// on non win32 OS, we keep the allocated chunk as is, just start again to curzise = 0

	// start again at the top
	cursize = 0;
}

//=============================================================================
// Ridah, model caching

// TODO: convert the Hunk_Alloc's in the model loading to malloc's, so we don't have
// to move so much memory around during transitions

static model_t backupModels[MAX_MOD_KNOWN];
static int numBackupModels = 0;

/*
===============
R_CacheModelAlloc
===============
*/
void *R_CacheModelAlloc( int size ) {
	if ( r_cache->integer && r_cacheModels->integer ) {
		return R_Hunk_Alloc( size );
	} else {
		return ri.Hunk_Alloc( size, h_low );
	}
}

/*
===============
R_CacheModelFree
===============
*/
void R_CacheModelFree( void *ptr ) {
	if ( r_cache->integer && r_cacheModels->integer ) {
		// TTimo: it's in the hunk, leave it there, next R_Hunk_Begin will clear it all
	} else
	{
		// if r_cache 0, this is never supposed to get called anyway
		Com_Printf( "FIXME: unexpected R_CacheModelFree call (r_cache 0)\n" );
	}
}

/*
===============
R_PurgeModels
===============
*/
void R_PurgeModels( int count ) {
	static int lastPurged = 0;

	if ( !numBackupModels ) {
		return;
	}

	lastPurged = 0;
	numBackupModels = 0;

	// note: we can only do this since we only use the virtual memory for the model caching!
	R_Hunk_Reset();
}

/*
===============
R_BackupModels
===============
*/
void R_BackupModels( void ) {
	int i, j;
	model_t *mod, *modBack;

	if ( !r_cache->integer ) {
		return;
	}
	if ( !r_cacheModels->integer ) {
		return;
	}

	if ( numBackupModels ) {
		R_PurgeModels( numBackupModels + 1 ); // get rid of them all
	}

	// copy each model in memory across to the backupModels
	modBack = backupModels;
	for ( i = 0; i < tr.numModels; i++ ) {
		mod = tr.models[i];

		if ( mod->type && mod->type != MOD_BRUSH && mod->type != MOD_MDS ) {
			memcpy( modBack, mod, sizeof( *mod ) );
			switch ( mod->type ) {
			case MOD_MESH:
				for ( j = MD3_MAX_LODS - 1; j >= 0; j-- ) {
					if ( j < mod->numLods && mod->model.md3[j] ) {
						if ( ( j == MD3_MAX_LODS - 1 ) || ( mod->model.md3[j] != mod->model.md3[j + 1] ) ) {
							modBack->model.md3[j] = R_CacheModelAlloc( mod->model.md3[j]->ofsEnd );
							memcpy( modBack->model.md3[j], mod->model.md3[j], mod->model.md3[j]->ofsEnd );
						} else {
							modBack->model.md3[j] = modBack->model.md3[j + 1];
						}
					}
				}
				break;
			case MOD_MDC:
				for ( j = MD3_MAX_LODS - 1; j >= 0; j-- ) {
					if ( j < mod->numLods && mod->model.mdc[j] ) {
						if ( ( j == MD3_MAX_LODS - 1 ) || ( mod->model.mdc[j] != mod->model.mdc[j + 1] ) ) {
							modBack->model.mdc[j] = R_CacheModelAlloc( mod->model.mdc[j]->ofsEnd );
							memcpy( modBack->model.mdc[j], mod->model.mdc[j], mod->model.mdc[j]->ofsEnd );
						} else {
							modBack->model.mdc[j] = modBack->model.mdc[j + 1];
						}
					}
				}
				break;
			default:
				break; // MOD_BAD MOD_BRUSH MOD_MDS not handled
			}
			modBack++;
			numBackupModels++;
		}
	}
}


/*
=================
R_RegisterMDCShaders
=================
*/
static void R_RegisterMDCShaders( model_t *mod, int lod ) {
	mdcSurface_t        *surf;
	md3Shader_t         *shader;
	int i, j;

	// swap all the surfaces
	surf = ( mdcSurface_t * )( (byte *)mod->model.mdc[lod] + mod->model.mdc[lod]->ofsSurfaces );
	for ( i = 0 ; i < mod->model.mdc[lod]->numSurfaces ; i++ ) {
		// register the shaders
		shader = ( md3Shader_t * )( (byte *)surf + surf->ofsShaders );
		for ( j = 0 ; j < surf->numShaders ; j++, shader++ ) {
			shader_t    *sh;

			sh = R_FindShader( shader->name, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				shader->shaderIndex = 0;
			} else {
				shader->shaderIndex = sh->index;
			}
		}
		// find the next surface
		surf = ( mdcSurface_t * )( (byte *)surf + surf->ofsEnd );
	}
}

/*
=================
R_RegisterMD3Shaders
=================
*/
static void R_RegisterMD3Shaders( model_t *mod, int lod ) {
	md3Surface_t        *surf;
	md3Shader_t         *shader;
	int i, j;

	// swap all the surfaces
	surf = ( md3Surface_t * )( (byte *)mod->model.md3[lod] + mod->model.md3[lod]->ofsSurfaces );
	for ( i = 0 ; i < mod->model.md3[lod]->numSurfaces ; i++ ) {
		// register the shaders
		shader = ( md3Shader_t * )( (byte *)surf + surf->ofsShaders );
		for ( j = 0 ; j < surf->numShaders ; j++, shader++ ) {
			shader_t    *sh;

			sh = R_FindShader( shader->name, LIGHTMAP_NONE, qtrue );
			if ( sh->defaultShader ) {
				shader->shaderIndex = 0;
			} else {
				shader->shaderIndex = sh->index;
			}
		}
		// find the next surface
		surf = ( md3Surface_t * )( (byte *)surf + surf->ofsEnd );
	}
}

/*
===============
R_FindCachedModel

  look for the given model in the list of backupModels
===============
*/
qboolean R_FindCachedModel( const char *name, model_t *newmod ) {
	int i,j, index;
	model_t *mod;

	// NOTE TTimo
	// would need an r_cache check here too?

	if ( !r_cacheModels->integer ) {
		return qfalse;
	}

	if ( !numBackupModels ) {
		return qfalse;
	}

	mod = backupModels;
	for ( i = 0; i < numBackupModels; i++, mod++ ) {
		if ( !Q_strncmp( mod->name, name, sizeof( mod->name ) ) ) {
			// copy it to a new slot
			index = newmod->index;
			memcpy( newmod, mod, sizeof( model_t ) );
			newmod->index = index;
			switch ( mod->type ) {
			case MOD_MDS:
				return qfalse;  // not supported yet
			case MOD_MDM:
				return qfalse;  // not supported yet
			case MOD_MDX:
				return qfalse;  // not supported yet
			case MOD_MESH:
				for ( j = MD3_MAX_LODS - 1; j >= 0; j-- ) {
					if ( j < mod->numLods && mod->model.md3[j] ) {
						if ( ( j == MD3_MAX_LODS - 1 ) || ( mod->model.md3[j] != mod->model.md3[j + 1] ) ) {
							newmod->model.md3[j] = ri.Hunk_Alloc( mod->model.md3[j]->ofsEnd, h_low );
							memcpy( newmod->model.md3[j], mod->model.md3[j], mod->model.md3[j]->ofsEnd );
							R_RegisterMD3Shaders( newmod, j );
							R_CacheModelFree( mod->model.md3[j] );
						} else {
							newmod->model.md3[j] = mod->model.md3[j + 1];
						}
					}
				}
				break;
			case MOD_MDC:
				for ( j = MD3_MAX_LODS - 1; j >= 0; j-- ) {
					if ( j < mod->numLods && mod->model.mdc[j] ) {
						if ( ( j == MD3_MAX_LODS - 1 ) || ( mod->model.mdc[j] != mod->model.mdc[j + 1] ) ) {
							newmod->model.mdc[j] = ri.Hunk_Alloc( mod->model.mdc[j]->ofsEnd, h_low );
							memcpy( newmod->model.mdc[j], mod->model.mdc[j], mod->model.mdc[j]->ofsEnd );
							R_RegisterMDCShaders( newmod, j );
							R_CacheModelFree( mod->model.mdc[j] );
						} else {
							newmod->model.mdc[j] = mod->model.mdc[j + 1];
						}
					}
				}
				break;
			default:
				break; // MOD_BAD MOD_BRUSH
			}

			mod->type = MOD_BAD;    // don't try and purge it later
			mod->name[0] = 0;
			return qtrue;
		}
	}

	return qfalse;
}

/*
===============
R_LoadCacheModels
===============
*/
void R_LoadCacheModels( void ) {
	int len;
	byte *buf;
	char    *token, *pString;
	char name[MAX_QPATH];

	if ( !r_cacheModels->integer ) {
		return;
	}

	// don't load the cache list in between level loads, only on startup, or after a vid_restart
	if ( numBackupModels > 0 ) {
		return;
	}

	len = ri.FS_ReadFile( "model.cache", NULL );

	if ( len <= 0 ) {
		return;
	}

	buf = (byte *)ri.Hunk_AllocateTempMemory( len );
	ri.FS_ReadFile( "model.cache", (void **)&buf );
	pString = buf;

	while ( ( token = COM_ParseExt( &pString, qtrue ) ) && token[0] ) {
		Q_strncpyz( name, token, sizeof( name ) );
		RE_RegisterModel( name );
	}

	ri.Hunk_FreeTempMemory( buf );
}
// done.
//========================================================================
