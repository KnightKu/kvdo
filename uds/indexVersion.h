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
 *
 * $Id: //eng/uds-releases/krusty/src/uds/indexVersion.h#4 $
 */

#ifndef INDEX_VERSION_H
#define INDEX_VERSION_H

#include "typeDefs.h"

struct index_version {
	bool chapter_index_header_native_endian;
};

enum {
	SUPER_VERSION_MINIMUM = 1,
	SUPER_VERSION_CURRENT = 3,
	SUPER_VERSION_MAXIMUM = 7,
};

/**
 * Initialize the version parameters that we normally learn when loading the
 * index but need to use during index operation.
 *
 * @param version        The version parameters
 * @param super_version  The SuperBlock version number
 **/
void initialize_index_version(struct index_version *version,
			      uint32_t super_version);

#endif // INDEX_VERSION_H
