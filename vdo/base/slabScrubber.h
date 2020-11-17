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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabScrubber.h#12 $
 */

#ifndef SLAB_SCRUBBER_H
#define SLAB_SCRUBBER_H

#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Create a slab scrubber
 *
 * @param layer               The physical layer of the VDO
 * @param slab_journal_size   The size of a slab journal in blocks
 * @param read_only_notifier  The context for entering read-only mode
 * @param scrubber_ptr        A pointer to hold the scrubber
 *
 * @return VDO_SUCCESS or an error
 **/
int make_slab_scrubber(PhysicalLayer *layer,
		       block_count_t slab_journal_size,
		       struct read_only_notifier *read_only_notifier,
		       struct slab_scrubber **scrubber_ptr)
	__attribute__((warn_unused_result));

/**
 * Free a slab scrubber and null out the reference to it.
 *
 * @param scrubber_ptr  A pointer to the scrubber to destroy
 **/
void free_slab_scrubber(struct slab_scrubber **scrubber_ptr);

/**
 * Check whether a scrubber has slabs to scrub.
 *
 * @param scrubber  The scrubber to check
 *
 * @return <code>true</code> if the scrubber has slabs to scrub
 **/
bool has_slabs_to_scrub(struct slab_scrubber *scrubber)
	__attribute__((warn_unused_result));

/**
 * Register a slab with a scrubber.
 *
 * @param scrubber       The scrubber
 * @param slab           The slab to scrub
 * @param high_priority  <code>true</code> if the slab should be put on the
 *                      high-priority queue
 **/
void register_slab_for_scrubbing(struct slab_scrubber *scrubber,
				 struct vdo_slab *slab,
				 bool high_priority);

/**
 * Scrub all the slabs which have been registered with a slab scrubber.
 *
 * @param scrubber       The scrubber
 * @param parent         The object to notify when scrubbing is complete
 * @param callback       The function to run when scrubbing is complete
 * @param error_handler  The handler for scrubbing errors
 **/
void scrub_slabs(struct slab_scrubber *scrubber,
		 void *parent,
		 vdo_action *callback,
		 vdo_action *error_handler);

/**
 * Scrub any slabs which have been registered at high priority with a slab
 * scrubber.
 *
 * @param scrubber            The scrubber
 * @param scrub_at_least_one  <code>true</code> if one slab should always be
 *                            scrubbed, even if there are no high-priority slabs
 *                            (and there is at least one low priority slab)
 * @param parent              The completion to notify when scrubbing is
 *                            complete
 * @param callback            The function to run when scrubbing is complete
 * @param error_handler       The handler for scrubbing errors
 **/
void scrub_high_priority_slabs(struct slab_scrubber *scrubber,
			       bool scrub_at_least_one,
			       struct vdo_completion *parent,
			       vdo_action *callback,
			       vdo_action *error_handler);

/**
 * Tell the scrubber to stop scrubbing after it finishes the slab it is
 * currently working on.
 *
 * @param scrubber  The scrubber to stop
 * @param parent    The completion to notify when scrubbing has stopped
 **/
void stop_scrubbing(struct slab_scrubber *scrubber,
		    struct vdo_completion *parent);

/**
 * Tell the scrubber to resume scrubbing if it has been stopped.
 *
 * @param scrubber  The scrubber to resume
 * @param parent    The object to notify once scrubbing has resumed
 **/
void resume_scrubbing(struct slab_scrubber *scrubber,
		      struct vdo_completion *parent);

/**
 * Wait for a clean slab.
 *
 * @param scrubber  The scrubber on which to wait
 * @param waiter    The waiter
 *
 * @return VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise
 **/
int enqueue_clean_slab_waiter(struct slab_scrubber *scrubber,
			      struct waiter *waiter);

/**
 * Get the number of slabs that are unrecovered or being scrubbed.
 *
 * @param scrubber  The scrubber to query
 *
 * @return the number of slabs that are unrecovered or being scrubbed
 **/
SlabCount get_scrubber_slab_count(const struct slab_scrubber *scrubber)
	__attribute__((warn_unused_result));

/**
 * Dump information about a slab scrubber to the log for debugging.
 *
 * @param scrubber   The scrubber to dump
 **/
void dump_slab_scrubber(const struct slab_scrubber *scrubber);

#endif /* SLAB_SCRUBBER_H */
