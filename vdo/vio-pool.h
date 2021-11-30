/*
 * Copyright Red Hat
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
 */

#ifndef VIO_POOL_H
#define VIO_POOL_H

#include <linux/list.h>

#include "permassert.h"

#include "completion.h"
#include "types.h"
#include "wait-queue.h"

/**
 * A vio_pool is a collection of preallocated vios used to write arbitrary
 * metadata blocks.
 **/

/**
 * A vio_pool_entry is the pair of vio and buffer whether in use or not.
 **/
struct vio_pool_entry {
	struct list_head available_entry;
	struct vio *vio;
	void *buffer;
	void *parent;
	void *context;
};

/**
 * A function which constructs a vio for a pool.
 *
 * @param [in]  vdo      The vdo in which the vio will operate
 * @param [in]  parent   The parent of the vio
 * @param [in]  buffer   The data buffer for the vio
 * @param [out] vio_ptr  A pointer to hold the new vio
 **/
typedef int vio_constructor(struct vdo *vdo,
			    void *parent,
			    void *buffer,
			    struct vio **vio_ptr);

int __must_check make_vio_pool(struct vdo *vdo,
			       size_t pool_size,
			       thread_id_t thread_id,
			       vio_constructor *constructor,
			       void *context,
			       struct vio_pool **pool_ptr);

void free_vio_pool(struct vio_pool *pool);

bool __must_check is_vio_pool_busy(struct vio_pool *pool);

int acquire_vio_from_pool(struct vio_pool *pool, struct waiter *waiter);

void return_vio_to_pool(struct vio_pool *pool, struct vio_pool_entry *entry);

/**
 * Convert a list entry to the vio_pool_entry that contains it.
 *
 * @param entry  The list entry to convert
 *
 * @return The vio_pool_entry wrapping the list entry
 **/
static inline struct vio_pool_entry *as_vio_pool_entry(struct list_head *entry)
{
	return list_entry(entry, struct vio_pool_entry, available_entry);
}

#endif /* VIO_POOL_H */
