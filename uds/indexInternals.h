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
 * $Id: //eng/uds-releases/jasper/src/uds/indexInternals.h#3 $
 */

#ifndef INDEX_INTERNALS_H
#define INDEX_INTERNALS_H

#include "index.h"
#include "loadType.h"
#include "request.h"

/**
 * Construct a new index from the given configuration.
 *
 * @param layout      The index layout to use
 * @param config      The configuration to use
 * @param userParams  The index session parameters.  If NULL, the default
 *                    session parameters will be used.
 * @param zoneCount   The number of zones for this index to use
 * @param loadType    How to create the index:  it can be create only, allow
 *                    loading from files, and allow rebuilding from the volume
 * @param newIndex    A pointer to hold a pointer to the new index
 *
 * @return UDS_SUCCESS or an error code
 **/
int allocateIndex(IndexLayout                  *layout,
                  const Configuration          *config,
                  const struct uds_parameters  *userParams,
                  unsigned int                  zoneCount,
                  LoadType                      loadType,
                  Index                       **newIndex)
  __attribute__((warn_unused_result));

/**
 * Clean up the index and its memory.
 *
 * @param index    The index to destroy.
 **/
void releaseIndex(Index *index);

#endif /* INDEX_INTERNALS_H */
