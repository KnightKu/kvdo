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

#ifndef VDO_SUSPEND_H
#define VDO_SUSPEND_H

#include "kernel-types.h"

/**
 * Ensure that the vdo has no outstanding I/O and will issue none until it is
 * resumed.
 *
 * @param vdo   The vdo to suspend
 *
 * @return VDO_SUCCESS or an error
 **/
int suspend_vdo(struct vdo *vdo);

#endif /* VDO_SUSPEND_H */
