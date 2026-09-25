/* pvs.glsl -- querying the world's potentially visible sets
 *
 * Include after hl_shared.h and after declaring the PVS buffer's contents
 * (see hl_shared.h) as a readable array of uints named pvs_table, e.g. in
 * a storage buffer block or through a buffer reference.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 */

#ifndef PVS_GLSL
#define PVS_GLSL

/* Can something in cluster from_cluster see cluster to_cluster? Clusters
 * are symmetric (visible both ways) and connected across water. Unknown
 * clusters (-1: submodel triangles, a point in solid) count as visible. */
bool
pvs_visible(int from_cluster, int to_cluster)
{
	uint num_clusters = pvs_table[0];

	if (from_cluster < 0 || to_cluster < 0 ||
	    uint(from_cluster) >= num_clusters || uint(to_cluster) >= num_clusters)
		return true;

	uint word = pvs_table[PVS_HEADER_UINTS + uint(from_cluster) * pvs_table[1] + (uint(to_cluster) >> 5)];
	return (word & (1u << (uint(to_cluster) & 31u))) != 0u;
}

#endif	/* PVS_GLSL */
