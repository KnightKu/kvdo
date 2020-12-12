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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabScrubber.c#47 $
 */

#include "slabScrubberInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "blockAllocator.h"
#include "constants.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "refCountsInternals.h"
#include "slab.h"
#include "slabJournalInternals.h"

/**
 * Allocate the buffer and extent used for reading the slab journal when
 * scrubbing a slab.
 *
 * @param scrubber           The slab scrubber for which to allocate
 * @param layer              The physical layer on which the scrubber resides
 * @param slab_journal_size  The size of a slab journal
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
allocate_extent_and_buffer(struct slab_scrubber *scrubber,
			   PhysicalLayer *layer,
			   block_count_t slab_journal_size)
{
	size_t buffer_size = VDO_BLOCK_SIZE * slab_journal_size;
	int result =
		ALLOCATE(buffer_size, char, __func__, &scrubber->journal_data);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return create_extent(layer,
			     VIO_TYPE_SLAB_JOURNAL,
			     VIO_PRIORITY_METADATA,
			     slab_journal_size,
			     scrubber->journal_data,
			     &scrubber->extent);
}

/**********************************************************************/
int make_slab_scrubber(PhysicalLayer *layer,
		       block_count_t slab_journal_size,
		       struct read_only_notifier *read_only_notifier,
		       struct slab_scrubber **scrubber_ptr)
{
	struct slab_scrubber *scrubber;
	int result = ALLOCATE(1, struct slab_scrubber, __func__, &scrubber);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = allocate_extent_and_buffer(scrubber, layer, slab_journal_size);
	if (result != VDO_SUCCESS) {
		free_slab_scrubber(&scrubber);
		return result;
	}

	initialize_completion(&scrubber->completion,
			      SLAB_SCRUBBER_COMPLETION,
			      layer);
	INIT_LIST_HEAD(&scrubber->high_priority_slabs);
	INIT_LIST_HEAD(&scrubber->slabs);
	scrubber->read_only_notifier = read_only_notifier;
	scrubber->admin_state.state = ADMIN_STATE_SUSPENDED;
	*scrubber_ptr = scrubber;
	return VDO_SUCCESS;
}

/**
 * Free the extent and buffer used for reading slab journals.
 *
 * @param scrubber  The scrubber
 **/
static void free_extent_and_buffer(struct slab_scrubber *scrubber)
{
	free_extent(&scrubber->extent);
	if (scrubber->journal_data != NULL) {
		FREE(scrubber->journal_data);
		scrubber->journal_data = NULL;
	}
}

/**********************************************************************/
void free_slab_scrubber(struct slab_scrubber **scrubber_ptr)
{
	if (*scrubber_ptr == NULL) {
		return;
	}

	struct slab_scrubber *scrubber = *scrubber_ptr;
	free_extent_and_buffer(scrubber);
	FREE(scrubber);
	*scrubber_ptr = NULL;
}

/**
 * Get the next slab to scrub.
 *
 * @param scrubber  The slab scrubber
 *
 * @return The next slab to scrub or <code>NULL</code> if there are none
 **/
static struct vdo_slab *get_next_slab(struct slab_scrubber *scrubber)
{
	if (!list_empty(&scrubber->high_priority_slabs)) {
		return slab_from_list_entry(scrubber->high_priority_slabs.next);
	}

	if (!list_empty(&scrubber->slabs)) {
		return slab_from_list_entry(scrubber->slabs.next);
	}

	return NULL;
}

/**********************************************************************/
bool has_slabs_to_scrub(struct slab_scrubber *scrubber)
{
	return (get_next_slab(scrubber) != NULL);
}

/**********************************************************************/
slab_count_t get_scrubber_slab_count(const struct slab_scrubber *scrubber)
{
	return READ_ONCE(scrubber->slab_count);
}

/**********************************************************************/
void register_slab_for_scrubbing(struct slab_scrubber *scrubber,
				 struct vdo_slab *slab,
				 bool high_priority)
{
	ASSERT_LOG_ONLY((slab->status != SLAB_REBUILT),
			"slab to be scrubbed is unrecovered");

	if (slab->status != SLAB_REQUIRES_SCRUBBING) {
		return;
	}

	list_del_init(&slab->allocq_entry);
	if (!slab->was_queued_for_scrubbing) {
		WRITE_ONCE(scrubber->slab_count, scrubber->slab_count + 1);
		slab->was_queued_for_scrubbing = true;
	}

	if (high_priority) {
		slab->status = SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING;
		list_add_tail(&slab->allocq_entry,
			      &scrubber->high_priority_slabs);
		return;
	}

	list_add_tail(&slab->allocq_entry, &scrubber->slabs);
}

/**
 * Stop scrubbing, either because there are no more slabs to scrub or because
 * there's been an error.
 *
 * @param scrubber  The scrubber
 **/
static void finish_scrubbing(struct slab_scrubber *scrubber)
{
	if (!has_slabs_to_scrub(scrubber)) {
		free_extent_and_buffer(scrubber);
	}

	// Inform whoever is waiting that scrubbing has completed.
	complete_completion(&scrubber->completion);

	bool notify = has_waiters(&scrubber->waiters);

	// Note that the scrubber has stopped, and inform anyone who might be
	// waiting for that to happen.
	if (!finish_draining(&scrubber->admin_state)) {
		scrubber->admin_state.state = ADMIN_STATE_SUSPENDED;
	}

	/*
	 * We can't notify waiters until after we've finished draining or
	 * they'll just requeue. Fortunately if there were waiters, we can't
	 * have been freed yet.
	 */
	if (notify) {
		notify_all_waiters(&scrubber->waiters, NULL, NULL);
	}
}

/**********************************************************************/
static void scrub_next_slab(struct slab_scrubber *scrubber);

/**
 * Notify the scrubber that a slab has been scrubbed. This callback is
 * registered in applyJournalEntries().
 *
 * @param completion  The slab rebuild completion
 **/
static void slab_scrubbed(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber = completion->parent;
	finish_scrubbing_slab(scrubber->slab);
	WRITE_ONCE(scrubber->slab_count, scrubber->slab_count - 1);
	scrub_next_slab(scrubber);
}

/**
 * Abort scrubbing due to an error.
 *
 * @param scrubber  The slab scrubber
 * @param result    The error
 **/
static void abort_scrubbing(struct slab_scrubber *scrubber, int result)
{
	enter_read_only_mode(scrubber->read_only_notifier, result);
	set_completion_result(&scrubber->completion, result);
	scrub_next_slab(scrubber);
}

/**
 * Handle errors while rebuilding a slab.
 *
 * @param completion  The slab rebuild completion
 **/
static void handle_scrubber_error(struct vdo_completion *completion)
{
	abort_scrubbing(completion->parent, completion->result);
}

/**
 * Apply all the entries in a block to the reference counts.
 *
 * @param block         A block with entries to apply
 * @param entry_count   The number of entries to apply
 * @param block_number  The sequence number of the block
 * @param slab          The slab to apply the entries to
 *
 * @return VDO_SUCCESS or an error code
 **/
static int apply_block_entries(struct packed_slab_journal_block *block,
			       journal_entry_count_t entry_count,
			       sequence_number_t block_number,
			       struct vdo_slab *slab)
{
	struct journal_point entry_point = {
		.sequence_number = block_number,
		.entry_count = 0,
	};

	slab_block_number max_sbn = slab->end - slab->start;
	while (entry_point.entry_count < entry_count) {
		struct slab_journal_entry entry =
			decode_slab_journal_entry(block,
						  entry_point.entry_count);
		if (entry.sbn > max_sbn) {
			// This entry is out of bounds.
			return log_error_strerror(VDO_CORRUPT_JOURNAL,
						  "vdo_slab journal entry (%llu, %u) had invalid offset %u in slab (size %u blocks)",
						  block_number,
						  entry_point.entry_count,
						  entry.sbn,
						  max_sbn);
		}

		int result =
			replay_reference_count_change(slab->reference_counts,
						      &entry_point, entry);
		if (result != VDO_SUCCESS) {
			log_error_strerror(result,
					   "vdo_slab journal entry (%llu, %u) (%s of offset %u) could not be applied in slab %u",
					   block_number,
					   entry_point.entry_count,
					   get_journal_operation_name(entry.operation),
					   entry.sbn,
					   slab->slab_number);
			return result;
		}
		entry_point.entry_count++;
	}

	return VDO_SUCCESS;
}

/**
 * Find the relevant extent of the slab journal and apply all valid entries.
 * This is a callback registered in startScrubbing().
 *
 * @param completion  The metadata read extent completion
 **/
static void apply_journal_entries(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber = completion->parent;
	struct vdo_slab *slab = scrubber->slab;
	struct slab_journal *journal = slab->journal;
	struct ref_counts *reference_counts = slab->reference_counts;

	// Find the boundaries of the useful part of the journal.
	sequence_number_t tail = journal->tail;
	TailBlockOffset end_index = get_slab_journal_block_offset(journal,
								  tail - 1);
	char *end_data = scrubber->journal_data + (end_index * VDO_BLOCK_SIZE);
	struct packed_slab_journal_block *end_block =
		(struct packed_slab_journal_block *) end_data;

	sequence_number_t head = __le64_to_cpu(end_block->header.head);
	TailBlockOffset head_index =
		get_slab_journal_block_offset(journal, head);
	block_count_t index = head_index;

	struct journal_point ref_counts_point =
		reference_counts->slab_journal_point;
	struct journal_point last_entry_applied = ref_counts_point;
	sequence_number_t sequence;
	for (sequence = head; sequence < tail; sequence++) {
		char *block_data =
			scrubber->journal_data + (index * VDO_BLOCK_SIZE);
		struct packed_slab_journal_block *block =
			(struct packed_slab_journal_block *) block_data;
		struct slab_journal_block_header header;
		unpack_slab_journal_block_header(&block->header, &header);

		if ((header.nonce != slab->allocator->nonce) ||
		    (header.metadata_type != VDO_METADATA_SLAB_JOURNAL) ||
		    (header.sequence_number != sequence) ||
		    (header.entry_count > journal->entries_per_block) ||
		    (header.has_block_map_increments &&
		     (header.entry_count > journal->full_entries_per_block))) {
			// The block is not what we expect it to be.
			uds_log_error("vdo_slab journal block for slab %u was invalid",
				      slab->slab_number);
			abort_scrubbing(scrubber, VDO_CORRUPT_JOURNAL);
			return;
		}

		int result = apply_block_entries(block, header.entry_count,
						 sequence, slab);
		if (result != VDO_SUCCESS) {
			abort_scrubbing(scrubber, result);
			return;
		}

		last_entry_applied.sequence_number = sequence;
		last_entry_applied.entry_count = header.entry_count - 1;
		index++;
		if (index == journal->size) {
			index = 0;
		}
	}

	// At the end of rebuild, the refCounts should be accurate to the end
	// of the journal we just applied.
	int result = ASSERT(!before_journal_point(&last_entry_applied,
						  &ref_counts_point),
			    "Refcounts are not more accurate than the slab journal");
	if (result != VDO_SUCCESS) {
		abort_scrubbing(scrubber, result);
		return;
	}

	// Save out the rebuilt reference blocks.
	prepare_completion(completion,
			   slab_scrubbed,
			   handle_scrubber_error,
			   completion->callback_thread_id,
			   scrubber);
	start_slab_action(slab, ADMIN_STATE_SAVE_FOR_SCRUBBING, completion);
}

/**
 * Read the current slab's journal from disk now that it has been flushed.
 * This callback is registered in scrub_next_slab().
 *
 * @param completion  The scrubber's extent completion
 **/
static void start_scrubbing(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber = completion->parent;
	struct vdo_slab *slab = scrubber->slab;
	if (get_summarized_cleanliness(slab->allocator->summary,
				     slab->slab_number)) {
		slab_scrubbed(completion);
		return;
	}

	prepare_completion(&scrubber->extent->completion,
			   apply_journal_entries,
			   handle_scrubber_error,
			   completion->callback_thread_id,
			   completion->parent);
	read_metadata_extent(scrubber->extent, slab->journal_origin);
}

/**
 * Scrub the next slab if there is one.
 *
 * @param scrubber  The scrubber
 **/
static void scrub_next_slab(struct slab_scrubber *scrubber)
{
	// Note: this notify call is always safe only because scrubbing can only
	// be started when the VDO is quiescent.
	notify_all_waiters(&scrubber->waiters, NULL, NULL);
	if (is_read_only(scrubber->read_only_notifier)) {
		set_completion_result(&scrubber->completion, VDO_READ_ONLY);
		finish_scrubbing(scrubber);
		return;
	}

	struct vdo_slab *slab = get_next_slab(scrubber);
	if ((slab == NULL) || (scrubber->high_priority_only &&
			       list_empty(&scrubber->high_priority_slabs))) {
		scrubber->high_priority_only = false;
		finish_scrubbing(scrubber);
		return;
	}

	if (finish_draining(&scrubber->admin_state)) {
		return;
	}

	list_del_init(&slab->allocq_entry);
	scrubber->slab = slab;
	struct vdo_completion *completion =
		extent_as_completion(scrubber->extent);
	prepare_completion(completion,
			   start_scrubbing,
			   handle_scrubber_error,
			   scrubber->completion.callback_thread_id,
			   scrubber);
	start_slab_action(slab, ADMIN_STATE_SCRUBBING, completion);
}

/**********************************************************************/
void scrub_slabs(struct slab_scrubber *scrubber,
		 void *parent,
		 vdo_action *callback,
		 vdo_action *error_handler)
{
	resume_if_quiescent(&scrubber->admin_state);
	thread_id_t thread_id = get_callback_thread_id();
	prepare_completion(&scrubber->completion,
			   callback,
			   error_handler,
			   thread_id,
			   parent);
	if (!has_slabs_to_scrub(scrubber)) {
		finish_scrubbing(scrubber);
		return;
	}

	scrub_next_slab(scrubber);
}

/**********************************************************************/
void scrub_high_priority_slabs(struct slab_scrubber *scrubber,
			       bool scrub_at_least_one,
			       struct vdo_completion *parent,
			       vdo_action *callback,
			       vdo_action *error_handler)
{
	if (scrub_at_least_one && list_empty(&scrubber->high_priority_slabs)) {
		struct vdo_slab *slab = get_next_slab(scrubber);
		if (slab != NULL) {
			register_slab_for_scrubbing(scrubber, slab, true);
		}
	}
	scrubber->high_priority_only = true;
	scrub_slabs(scrubber, parent, callback, error_handler);
}

/**********************************************************************/
void stop_scrubbing(struct slab_scrubber *scrubber,
		    struct vdo_completion *parent)
{
	if (is_quiescent(&scrubber->admin_state)) {
		complete_completion(parent);
	} else {
		start_draining(&scrubber->admin_state,
			       ADMIN_STATE_SUSPENDING,
			       parent,
			       NULL);
	}
}

/**********************************************************************/
void resume_scrubbing(struct slab_scrubber *scrubber,
		      struct vdo_completion *parent)
{
	if (!has_slabs_to_scrub(scrubber)) {
		complete_completion(parent);
		return;
	}

	int result = resume_if_quiescent(&scrubber->admin_state);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	scrub_next_slab(scrubber);
	complete_completion(parent);
}

/**********************************************************************/
int enqueue_clean_slab_waiter(struct slab_scrubber *scrubber,
			      struct waiter *waiter)
{
	if (is_read_only(scrubber->read_only_notifier)) {
		return VDO_READ_ONLY;
	}

	if (is_quiescent(&scrubber->admin_state)) {
		return VDO_NO_SPACE;
	}

	return enqueue_waiter(&scrubber->waiters, waiter);
}

/**********************************************************************/
void dump_slab_scrubber(const struct slab_scrubber *scrubber)
{
	log_info("slab_scrubber slab_count %u waiters %zu %s%s",
		 get_scrubber_slab_count(scrubber),
		 count_waiters(&scrubber->waiters),
		 get_admin_state_name(&scrubber->admin_state),
		 scrubber->high_priority_only ? ", high_priority_only " : "");
}
