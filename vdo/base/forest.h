/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/forest.h#12 $
 */

#ifndef FOREST_H
#define FOREST_H

#include "blockMapTree.h"
#include "types.h"

/**
 * A function to be called for each allocated PBN when traversing the forest.
 *
 * @param pbn         A PBN of a tree node
 * @param completion  The parent completion of the traversal
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int entry_callback(physical_block_number_t pbn,
			   struct vdo_completion *completion);

/**
 * Get the tree page for a given height and page index.
 *
 * @param forest      The forest which holds the page
 * @param root_index  The index of the tree that holds the page
 * @param height      The height of the desired page
 * @param page_index  The index of the desired page
 *
 * @return The requested page
 **/
struct tree_page * __must_check
get_tree_page_by_index(struct forest *forest,
		       root_count_t root_index,
		       height_t height,
		       page_number_t page_index);

/**
 * Make a collection of trees for a block_map, expanding the existing forest if
 * there is one.
 *
 * @param map      The block map
 * @param entries  The number of entries the block map will hold
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_forest(struct block_map *map, block_count_t entries);

/**
 * Free a forest and all of the segments it contains and NULL out the reference
 * to it.
 *
 * @param forest_ptr  A pointer to the forest to free
 **/
void free_forest(struct forest **forest_ptr);

/**
 * Abandon the unused next forest from a block_map.
 *
 * @param map  The block map
 **/
void abandon_forest(struct block_map *map);

/**
 * Replace a block_map's forest with the already-prepared larger forest.
 *
 * @param map  The block map
 **/
void replace_forest(struct block_map *map);

/**
 * Walk the entire forest of a block map.
 *
 * @param map             The block map to traverse
 * @param entry_callback  A function to call with the pbn of each allocated node
 *                        in the forest
 * @param parent          The completion to notify on each traversed PBN, and
 *                        when the traversal is complete
 **/
void traverse_forest(struct block_map *map,
		     entry_callback *entry_callback,
		     struct vdo_completion *parent);

/**
 * Compute the approximate number of pages which the forest will allocate in
 * order to map the specified number of logical blocks. This method assumes
 * that the block map is entirely arboreal.
 *
 * @param logical_blocks  The number of blocks to map
 * @param root_count      The number of trees in the forest
 *
 * @return A (slight) over-estimate of the total number of possible forest
 *         pages including the leaves
 **/
block_count_t __must_check
compute_forest_size(block_count_t logical_blocks, root_count_t root_count);
#endif // FOREST_H
