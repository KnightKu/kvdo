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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.h#7 $
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"
#include "kernelTypes.h"
#include "threadRegistry.h"
#include "workQueue.h"

struct kvdo_thread {
  struct kvdo            *kvdo;
  ThreadID                threadID;
  struct kvdo_work_queue *requestQueue;
  RegisteredThread        allocatingThread;
};

struct kvdo {
  struct kvdo_thread *threads;
  ThreadID            initializedThreadCount;
  KvdoWorkItem        workItem;
  VDOAction          *action;
  VDOCompletion      *completion;
  // Base-code device info
  VDO                *vdo;
};

typedef enum reqQAction {
  REQ_Q_ACTION_COMPLETION,
  REQ_Q_ACTION_FLUSH,
  REQ_Q_ACTION_MAP_BIO,
  REQ_Q_ACTION_SYNC,
  REQ_Q_ACTION_VIO_CALLBACK
} ReqQAction;

/**
 * Initialize the base code interface.
 *
 * @param [in]  kvdo          The kvdo to be initialized
 * @param [in]  threadConfig  The base-code thread configuration
 * @param [out] reason        The reason for failure
 *
 * @return  VDO_SUCCESS or an error code
 **/
int initializeKVDO(struct kvdo         *kvdo,
                   const ThreadConfig  *threadConfig,
                   char               **reason);

/**
 * Starts the base VDO instance associated with the kernel layer
 *
 * @param [in]  kvdo                  The kvdo to be started
 * @param [in]  common                The physical layer pointer
 * @param [in]  loadConfig            Load-time parameters for the VDO
 * @param [in]  vioTraceRecording     Debug flag to store
 * @param [out] reason                The reason for failure
 *
 * @return VDO_SUCCESS if started, otherwise error
 */
int startKVDO(struct kvdo          *kvdo,
              PhysicalLayer        *common,
              const VDOLoadConfig  *loadConfig,
              bool                  vioTraceRecording,
              char                **reason);

/**
 * Stops the base VDO instance associated with the kernel layer
 *
 * @param kvdo          The kvdo to be stopped
 *
 * @return VDO_SUCCESS if stopped, otherwise error
 */
int stopKVDO(struct kvdo *kvdo);

/**
 * Shut down the base code interface. The kvdo object must first be
 * stopped.
 *
 * @param kvdo         The kvdo to be shut down
 **/
void finishKVDO(struct kvdo *kvdo);

/**
 * Free up storage of the base code interface. The kvdo object must
 * first have been "finished".
 *
 * @param kvdo         The kvdo object to be destroyed
 **/
void destroyKVDO(struct kvdo *kvdo);


/**
 * Dump to the kernel log any work-queue info associated with the base
 * code.
 *
 * @param kvdo     The kvdo object to be examined
 **/
void dumpKVDOWorkQueue(struct kvdo *kvdo);

/**
 * Get the VDO pointer for a kvdo object
 *
 * @param kvdo          The kvdo object
 *
 * @return the VDO pointer
 */
static inline VDO *getVDO(struct kvdo *kvdo)
{
  return kvdo->vdo;
}

/**
 * Set whether compression is enabled.
 *
 * @param kvdo               The kvdo object
 * @param enableCompression  The new compression mode
 *
 * @return state of compression before new value is set
 **/
bool setKVDOCompressing(struct kvdo *kvdo, bool enableCompression);

/**
 * Get the current compression mode
 *
 * @param kvdo          The kvdo object to be queried
 *
 * @return whether compression is currently enabled
 */
bool getKVDOCompressing(struct kvdo *kvdo);

/**
 * Gets the latest statistics gathered by the base code.
 *
 * @param kvdo  the kvdo object
 * @param stats the statistics struct to fill in
 */
void getKVDOStatistics(struct kvdo *kvdo, VDOStatistics *stats);

/**
 * Get the current write policy
 *
 * @param kvdo          The kvdo to be queried
 *
 * @return  the write policy in effect
 */
WritePolicy getKVDOWritePolicy(struct kvdo *kvdo);

/**
 * Dump base code status information to the kernel log for debugging.
 *
 * @param kvdo          The kvdo to be examined
 */
void dumpKVDOStatus(struct kvdo *kvdo);

/**
 * Request the base code prepare to grow the physical space.
 *
 * @param kvdo           The kvdo to be updated
 * @param physicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoPrepareToGrowPhysical(struct kvdo *kvdo, BlockCount physicalCount);

/**
 * Notify the base code of resized physical storage.
 *
 * @param kvdo           The kvdo to be updated
 * @param physicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoResizePhysical(struct kvdo *kvdo, BlockCount physicalCount);

/**
 * Request the base code prepare to grow the logical space.
 *
 * @param kvdo          The kvdo to be updated
 * @param logicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoPrepareToGrowLogical(struct kvdo *kvdo, BlockCount logicalCount);

/**
 * Request the base code grow the logical space.
 *
 * @param kvdo          The kvdo to be updated
 * @param logicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoResizeLogical(struct kvdo *kvdo, BlockCount logicalCount);

/**
 * Request the base code go read-only.
 *
 * @param kvdo          The kvdo to be updated
 * @param result        The error code causing the read only
 */
void setKVDOReadOnly(struct kvdo *kvdo, int result);

/**
 * Perform an extended base-code command
 *
 * @param kvdo          The kvdo upon which to perform the operation.
 * @param argc          The number of arguments to the command.
 * @param argv          The command arguments. Note that all extended
 *                        command argv[0] strings start with "x-".
 *
 * @return VDO_SUCCESS or an error code
 **/
int performKVDOExtendedCommand(struct kvdo *kvdo, int argc, char **argv);

/**
 * Enqueue a work item to be processed in the base code context.
 *
 * @param kvdo         The kvdo object in which to run the work item
 * @param item         The work item to be run
 * @param threadID     The thread on which to run the work item
 **/
void enqueueKVDOWork(struct kvdo *kvdo, KvdoWorkItem *item, ThreadID threadID);

/**
 * Set up and enqueue a VIO's work item to be processed in the base code
 * context.
 *
 * @param kvio           The VIO with the work item to be run
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
void enqueueKVIO(struct kvio      *kvio,
                 KvdoWorkFunction  work,
                 void             *statsFunction,
                 unsigned int      action);

/**
 * Enqueue an arbitrary completion for execution on its indicated
 * thread.
 *
 * @param enqueueable  The Enqueueable object containing the completion pointer
 **/
void kvdoEnqueue(Enqueueable *enqueueable);

#endif // KERNEL_VDO_H
