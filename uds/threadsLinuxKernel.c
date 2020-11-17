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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/threadsLinuxKernel.c#4 $
 */

#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include "memoryAlloc.h"
#include "logger.h"
#include "threads.h"
#include "uds-error.h"

static struct hlist_head kernelThreadList;
static struct mutex kernelThreadMutex;
static OnceState kernelThreadOnce;

typedef struct kernelThread {
  void (*threadFunc)(void *);
  void *threadData;
  struct hlist_node threadLinks;
  struct task_struct *threadTask;
  struct completion threadDone;
} KernelThread;

/**********************************************************************/
static void kernelThreadInit(void)
{
  mutex_init(&kernelThreadMutex);
}

/**********************************************************************/
static int threadStarter(void *arg)
{
  KernelThread *kt = arg;
  kt->threadTask = current;
  performOnce(&kernelThreadOnce, kernelThreadInit);
  mutex_lock(&kernelThreadMutex);
  hlist_add_head(&kt->threadLinks, &kernelThreadList);
  mutex_unlock(&kernelThreadMutex);
  RegisteredThread allocatingThread;
  registerAllocatingThread(&allocatingThread, NULL);
  kt->threadFunc(kt->threadData);
  unregisterAllocatingThread();
  complete(&kt->threadDone);
  return 0;
}

/**********************************************************************/
int createThread(void      (*threadFunc)(void *),
                 void       *threadData,
                 const char *name,
                 Thread     *newThread)
{
  char *nameColon = strchr(name, ':');
  char *myNameColon = strchr(current->comm, ':');
  KernelThread *kt;
  int result = ALLOCATE(1, KernelThread, __func__, &kt);
  if (result != UDS_SUCCESS) {
    logWarning("Error allocating memory for %s", name);
    return result;
  }
  kt->threadFunc = threadFunc;
  kt->threadData = threadData;
  init_completion(&kt->threadDone);
  struct task_struct *thread;
  /*
   * Start the thread, with an appropriate thread name.
   *
   * If the name supplied contains a colon character, use that name.  This
   * causes uds module threads to have names like "uds:callbackW" and the main
   * test runner thread to be named "zub:runtest".
   *
   * Otherwise if the current thread has a name containing a colon character,
   * prefix the name supplied with the name of the current thread up to (and
   * including) the colon character.  Thus when the "kvdo0:dedupeQ" thread
   * opens an index session, all the threads associated with that index will
   * have names like "kvdo0:foo".
   *
   * Otherwise just use the name supplied.  This should be a rare occurrence.
   */
  if ((nameColon == NULL) && (myNameColon != NULL)) {
    thread = kthread_run(threadStarter, kt, "%.*s:%s",
                         (int) (myNameColon - current->comm), current->comm,
                         name);
  } else {
    thread = kthread_run(threadStarter, kt, "%s", name);
  }
  if (IS_ERR(thread)) {
    FREE(kt);
    return UDS_ENOTHREADS;
  }
  *newThread = kt;
  return UDS_SUCCESS;
}
/**********************************************************************/
int joinThreads(Thread kt)
{
  while (wait_for_completion_interruptible(&kt->threadDone) != 0) {
  }
  mutex_lock(&kernelThreadMutex);
  hlist_del(&kt->threadLinks);
  mutex_unlock(&kernelThreadMutex);
  FREE(kt);
  return UDS_SUCCESS;
}

/**********************************************************************/
void applyToThreads(void applyFunc(void *, struct task_struct *),
                    void *argument)
{
  KernelThread *kt;
  performOnce(&kernelThreadOnce, kernelThreadInit);
  mutex_lock(&kernelThreadMutex);
  hlist_for_each_entry(kt, &kernelThreadList, threadLinks) {
    applyFunc(argument, kt->threadTask);
  }
  mutex_unlock(&kernelThreadMutex);
}

/**********************************************************************/
void exitThread(void)
{
  KernelThread *kt;
  struct completion *completion = NULL;
  performOnce(&kernelThreadOnce, kernelThreadInit);
  mutex_lock(&kernelThreadMutex);
  hlist_for_each_entry(kt, &kernelThreadList, threadLinks) {
    if (kt->threadTask == current) {
      completion = &kt->threadDone;
      break;
    }
  }
  mutex_unlock(&kernelThreadMutex);
  unregisterAllocatingThread();
  complete_and_exit(completion, 1);
}

/**********************************************************************/
ThreadId getThreadId(void)
{
  return current->pid;
}

/**********************************************************************/
unsigned int getNumCores(void)
{
  return num_online_cpus();
}

/**********************************************************************/
int initializeBarrier(Barrier *barrier, unsigned int threadCount)
{
  barrier->arrived     = 0;
  barrier->threadCount = threadCount;
  int result = initializeSemaphore(&barrier->mutex, 1);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return initializeSemaphore(&barrier->wait, 0);
}

/**********************************************************************/
int destroyBarrier(Barrier *barrier)
{
  int result = destroySemaphore(&barrier->mutex);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return destroySemaphore(&barrier->wait);
}

/**********************************************************************/
int enterBarrier(Barrier *barrier, bool *winner)
{
  acquireSemaphore(&barrier->mutex);
  bool lastThread = ++barrier->arrived == barrier->threadCount;
  if (lastThread) {
    // This is the last thread to arrive, so wake up the others
    int i;
    for (i = 1; i < barrier->threadCount; i++) {
      releaseSemaphore(&barrier->wait);
    }
    // Then reinitialize for the next cycle
    barrier->arrived = 0;
    releaseSemaphore(&barrier->mutex);
  } else {
    // This is NOT the last thread to arrive, so just wait
    releaseSemaphore(&barrier->mutex);
    acquireSemaphore(&barrier->wait);
  }
  if (winner != NULL) {
    *winner = lastThread;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int yieldScheduler(void)
{
  yield();
  return UDS_SUCCESS;
}
