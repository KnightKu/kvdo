/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelTypes.h#7 $
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include "types.h"

/**
 * The size of a discard request in bytes.
 **/
typedef uint32_t DiscardSize;

/**
 * A time in jiffies.
 **/
typedef uint64_t Jiffies;

/**
 * A timeout in jiffies.
 **/
typedef int64_t TimeoutJiffies;

struct io_submitter;
struct kvdo;
struct kvdoFlush;
struct kvdo_work_queue;
struct kvio;

typedef struct atomicBioStats AtomicBioStats;
typedef struct dataKVIO       DataKVIO;
typedef struct dedupeContext  DedupeContext;
typedef struct dedupeIndex    DedupeIndex;
typedef struct kernelLayer    KernelLayer;
typedef struct kvdoWorkItem   KvdoWorkItem;

typedef void (*KVIOCallback)(struct kvio *kvio);
typedef void (*DataKVIOCallback)(DataKVIO *dataKVIO);
typedef void (*KvdoWorkFunction)(KvdoWorkItem *workItem);

#endif /* KERNEL_TYPES_H */
