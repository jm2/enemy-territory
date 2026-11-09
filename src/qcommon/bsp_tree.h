/*
BSP tree topology; complete layout and references must already be valid.

From the Quake III port (Q3A 132e0dd0). R_SetParent recurses from the world
root and stores one parent per decision node and visible leaf, so a cycle or a
shared decision node would recurse forever or exponentially.
*/
#ifndef BSP_TREE_H
#define BSP_TREE_H
#include "bsp_references.h"

static const char *BSP_ValidateTree( const void *buffer, const dheader_t *header ) {
	const byte *base = buffer, *records = base + header->lumps[LUMP_NODES].fileofs;
	const byte *leafRecords = base + header->lumps[LUMP_LEAFS].fileofs;
	unsigned int nodes = header->lumps[LUMP_NODES].filelen / sizeof( dnode_t );
	unsigned int leaves = header->lumps[LUMP_LEAFS].filelen / sizeof( dleaf_t );
	unsigned int *parents, *queue, i, j, child, leaf, head = 0, tail = 0;
	bspArrayAllocation_t allocation;
	const char *error;

	allocation.count = nodes * 2 + leaves;
	allocation.elementSize = sizeof( *parents );
	allocation.extraElements = 0;
	error = BSP_ValidateAllocations( &allocation, 1 );
	if ( error ) {
		return error;
	}
	parents = calloc( allocation.count, sizeof( *parents ) );
	if ( !parents ) {
		return "BSP tree validation allocation failed";
	}
	queue = parents + nodes + leaves;
	for ( i = 0; i < nodes; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			child = BSP_FileWord( records + i * sizeof( dnode_t ) + offsetof( dnode_t, children ) + j * 4 );
			if ( !( child & 0x80000000u ) ) {
				if ( parents[child] ) {
					error = "BSP decision node has multiple incoming edges";
					goto done;
				}
				parents[child] = i + 1;
			} else {
				leaf = ~child;
				// opaque leaves have no PVS ancestor traversal and may be shared
				if ( BSP_FileWord( leafRecords + leaf * sizeof( dleaf_t ) + offsetof( dleaf_t, cluster ) ) == 0xffffffffu ) {
					continue;
				}
				if ( parents[nodes + leaf] && parents[nodes + leaf] != i + 1 ) {
					error = "BSP visible leaf has multiple parents";
					goto done;
				}
				parents[nodes + leaf] = i + 1;
			}
		}
	}
	if ( parents[0] ) {
		error = "BSP world root has an incoming edge";
		goto done;
	}
	// inline-model trees may form a forest; inspect every component without recursion
	for ( i = 0; i < nodes; i++ ) {
		if ( !parents[i] ) {
			queue[tail++] = i;
		}
	}
	while ( head < tail ) {
		i = queue[head++];
		for ( j = 0; j < 2; j++ ) {
			child = BSP_FileWord( records + i * sizeof( dnode_t ) + offsetof( dnode_t, children ) + j * 4 );
			if ( !( child & 0x80000000u ) ) {
				queue[tail++] = child;
			}
		}
	}
	if ( tail != nodes ) {
		error = "cyclic BSP node graph";
	}
done:
	free( parents );
	return error;
}

/*
The most decision nodes on any path down from the world root, node 0: what an
explicit stack for a walk of the world tree must hold, one pending sibling per
decision node above the current one. BSP_ValidateTree must have accepted the
tree. A map with no nodes has depth 0.
*/
static const char *BSP_TreeDepth( const void *buffer, const dheader_t *header, unsigned int *depth ) {
	const byte *records = (const byte *)buffer + header->lumps[LUMP_NODES].fileofs;
	unsigned int nodes = header->lumps[LUMP_NODES].filelen / sizeof( dnode_t );
	unsigned int *queue, *levels, i, j, child, head = 0, tail = 0;
	bspArrayAllocation_t allocation;
	const char *error;

	*depth = 0;
	if ( !nodes ) {
		return NULL;
	}
	allocation.count = nodes * 2;
	allocation.elementSize = sizeof( *queue );
	allocation.extraElements = 0;
	error = BSP_ValidateAllocations( &allocation, 1 );
	if ( error ) {
		return error;
	}
	queue = malloc( allocation.count * sizeof( *queue ) );
	if ( !queue ) {
		return "BSP tree depth allocation failed";
	}
	levels = queue + nodes;
	// breadth first: a tree reaches each node once, so the queue holds at most every node
	queue[tail++] = 0;
	levels[0] = 1;
	while ( head < tail ) {
		i = queue[head++];
		if ( levels[i] > *depth ) {
			*depth = levels[i];
		}
		for ( j = 0; j < 2; j++ ) {
			child = BSP_FileWord( records + i * sizeof( dnode_t ) + offsetof( dnode_t, children ) + j * 4 );
			if ( !( child & 0x80000000u ) ) {
				levels[child] = levels[i] + 1;
				queue[tail++] = child;
			}
		}
	}
	free( queue );
	return NULL;
}
#endif
