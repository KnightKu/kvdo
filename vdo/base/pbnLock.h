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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLock.h#4 $
 */

#ifndef PBN_LOCK_H
#define PBN_LOCK_H

#include "atomic.h"
#include "types.h"

/**
 * The type of a PBN lock.
 **/
typedef enum {
  VIO_READ_LOCK = 0,
  VIO_WRITE_LOCK,
  VIO_COMPRESSED_WRITE_LOCK,
  VIO_BLOCK_MAP_WRITE_LOCK,
} PBNLockType;

struct pbn_lock_implementation;

/**
 * A PBN lock.
 **/
struct pbn_lock {
  /** The implementation of the lock */
  const struct pbn_lock_implementation *implementation;

  /** The number of VIOs holding or sharing this lock */
  VIOCount holderCount;
  /**
   * The number of compressed block writers holding a share of this lock while
   * they are acquiring a reference to the PBN.
   **/
  uint8_t fragmentLocks;

  /**
   * Whether the locked PBN has been provisionally referenced on behalf of the
   * lock holder.
   **/
  bool hasProvisionalReference;

  /**
   * For read locks, the number of references that were known to be available
   * on the locked block at the time the lock was acquired.
   **/
  uint8_t incrementLimit;

  /**
   * For read locks, the number of DataVIOs that have tried to claim one of
   * the available increments during the lifetime of the lock. Each claim will
   * first increment this counter, so it can exceed the increment limit.
   **/
  Atomic32 incrementsClaimed;
};

/**
 * Initialize a pbn_lock.
 *
 * @param lock  The lock to initialize
 * @param type  The type of the lock
 **/
void initializePBNLock(struct pbn_lock *lock, PBNLockType type);

/**
 * Check whether a pbn_lock is a read lock.
 *
 * @param lock  The lock to check
 *
 * @return <code>true</code> if the lock is a read lock
 **/
bool isPBNReadLock(const struct pbn_lock *lock)
  __attribute__((warn_unused_result));

/**
 * Downgrade a PBN write lock to a PBN read lock. The lock holder count is
 * cleared and the caller is responsible for setting the new count.
 *
 * @param lock  The PBN write lock to downgrade
 **/
void downgradePBNWriteLock(struct pbn_lock *lock);

/**
 * Try to claim one of the available reference count increments on a read
 * lock. Claims may be attempted from any thread. A claim is only valid until
 * the PBN lock is released.
 *
 * @param lock  The PBN read lock from which to claim an increment
 *
 * @return <code>true</code> if the claim succeeded, guaranteeing one
 *         increment can be made without overflowing the PBN's reference count
 **/
bool claimPBNLockIncrement(struct pbn_lock *lock)
  __attribute__((warn_unused_result));

/**
 * Check whether a PBN lock has a provisional reference.
 *
 * @param lock  The PBN lock
 **/
static inline bool hasProvisionalReference(struct pbn_lock *lock)
{
  return ((lock != NULL) && lock->hasProvisionalReference);
}

/**
 * Inform a PBN lock that it is responsible for a provisional reference.
 *
 * @param lock  The PBN lock
 **/
void assignProvisionalReference(struct pbn_lock *lock);

/**
 * Inform a PBN lock that it is no longer responsible for a provisional
 * reference.
 *
 * @param lock  The PBN lock
 **/
void unassignProvisionalReference(struct pbn_lock *lock);

/**
 * If the lock is responsible for a provisional reference, release that
 * reference. This method is called when the lock is released.
 *
 * @param lock       The lock
 * @param lockedPBN  The PBN covered by the lock
 * @param allocator  The block allocator from which to release the reference
 **/
void releaseProvisionalReference(struct pbn_lock     *lock,
                                 PhysicalBlockNumber  lockedPBN,
                                 struct block_allocator *allocator);

#endif /* PBN_LOCK_H */
