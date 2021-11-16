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

#include "thread-config.h"


#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "device-config.h"
#include "kernel-types.h"
#include "status-codes.h"
#include "types.h"

static int allocate_thread_config(zone_count_t logical_zone_count,
				  zone_count_t physical_zone_count,
				  zone_count_t hash_zone_count,
				  zone_count_t base_thread_count,
				  zone_count_t bio_thread_count,
				  struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result =
		UDS_ALLOCATE(1, struct thread_config, "thread config", &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(logical_zone_count,
			      thread_id_t,
			      "logical thread array",
			      &config->logical_threads);
	if (result != VDO_SUCCESS) {
		vdo_free_thread_config(config);
		return result;
	}

	result = UDS_ALLOCATE(physical_zone_count,
			      thread_id_t,
			      "physical thread array",
			      &config->physical_threads);
	if (result != VDO_SUCCESS) {
		vdo_free_thread_config(config);
		return result;
	}

	result = UDS_ALLOCATE(hash_zone_count,
			      thread_id_t,
			      "hash thread array",
			      &config->hash_zone_threads);
	if (result != VDO_SUCCESS) {
		vdo_free_thread_config(config);
		return result;
	}

	result = UDS_ALLOCATE(bio_thread_count,
			      thread_id_t,
			      "bio thread array",
			      &config->bio_threads);
	if (result != VDO_SUCCESS) {
		vdo_free_thread_config(config);
		return result;
	}

	config->logical_zone_count = logical_zone_count;
	config->physical_zone_count = physical_zone_count;
	config->hash_zone_count = hash_zone_count;
	config->base_thread_count = base_thread_count;
	config->bio_thread_count = bio_thread_count;

	*config_ptr = config;
	return VDO_SUCCESS;
}

static void assign_thread_ids(struct thread_config *config,
			      thread_id_t thread_ids[],
			      zone_count_t count)
{
	zone_count_t zone;

	for (zone = 0; zone < count; zone++) {
		thread_ids[zone] = config->thread_count++;
	}
}

/**
 * Make a thread configuration. If the logical, physical, and hash zone counts
 * are all 0, a single thread will be shared by all three plus the packer and
 * recovery journal. Otherwise, there must be at least one of each type, and
 * each will have its own thread, as will the packer and recovery journal.
 *
 * @param [in]  counts      The counts of each type of thread
 * @param [out] config_ptr  A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_make_thread_config(struct thread_count_config counts,
			   struct thread_config **config_ptr)
{
	int result;
	struct thread_config *config;
	thread_count_t total = (counts.logical_zones
				+ counts.physical_zones
				+ counts.hash_zones);

	if (total == 0) {
		result = allocate_thread_config(1,
						1,
						1,
						1,
						counts.bio_threads,
						&config);
		if (result != VDO_SUCCESS) {
			return result;
		}

		config->logical_threads[0] = config->thread_count;
		config->physical_threads[0] = config->thread_count;
		config->hash_zone_threads[0] = config->thread_count++;
	} else {
		/*
		 * Add in the admin/recovery journal and packer threads, of 
		 * which, there are one each. 
		 */
		total += 2;
		result = allocate_thread_config(counts.logical_zones,
						counts.physical_zones,
						counts.hash_zones,
						total,
						counts.bio_threads,
						&config);
		if (result != VDO_SUCCESS) {
			return result;
		}

		config->admin_thread = config->thread_count;
		config->journal_thread = config->thread_count++;
		config->packer_thread = config->thread_count++;
		assign_thread_ids(config,
				  config->logical_threads,
				  counts.logical_zones);
		assign_thread_ids(config,
				  config->physical_threads,
				  counts.physical_zones);
		assign_thread_ids(config,
				  config->hash_zone_threads,
				  counts.hash_zones);
	}

	config->dedupe_thread = config->thread_count++;
	config->bio_ack_thread = ((counts.bio_ack_threads > 0) ?
				  config->thread_count++
				  : VDO_INVALID_THREAD_ID);
	config->cpu_thread = config->thread_count++;
	assign_thread_ids(config, config->bio_threads, counts.bio_threads);

	*config_ptr = config;
	return VDO_SUCCESS;
}

/**
 * Destroy a thread configuration.
 *
 * @param config  The thread configuration to destroy
 **/
void vdo_free_thread_config(struct thread_config *config)
{
	if (config == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(config->logical_threads));
	UDS_FREE(UDS_FORGET(config->physical_threads));
	UDS_FREE(UDS_FORGET(config->hash_zone_threads));
	UDS_FREE(UDS_FORGET(config->bio_threads));
	UDS_FREE(config);
}

static bool get_zone_thread_name(const thread_id_t thread_ids[],
				 zone_count_t count,
				 thread_id_t id,
				 const char *prefix,
				 char *buffer,
				 size_t buffer_length)
{
	if (id >= thread_ids[0]) {
		thread_id_t index = id - thread_ids[0];

		if (index < count) {
			snprintf(buffer, buffer_length, "%s%d", prefix, index);
			return true;
		}
	}
	return false;
}

/**
 * Format the name of the worker thread desired to support a given
 * work queue. The physical layer may add a prefix identifying the
 * product; the output from this function should just identify the
 * thread.
 *
 * @param thread_config  The thread configuration
 * @param thread_id      The thread id
 * @param buffer         Where to put the formatted name
 * @param buffer_length  Size of the output buffer
 **/
void vdo_get_thread_name(const struct thread_config *thread_config,
			 thread_id_t thread_id,
			 char *buffer,
			 size_t buffer_length)
{
	if ((thread_config->base_thread_count == 1)
	    && (thread_id == 0)) {
		/* Historically this was the "request queue" thread. */
		snprintf(buffer, buffer_length, "reqQ");
		return;
	}

	if (thread_id == thread_config->journal_thread) {
		snprintf(buffer, buffer_length, "journalQ");
		return;
	} else if (thread_id == thread_config->admin_thread) {
		/*
		 * Theoretically this could be different from the journal 
		 * thread. 
		 */
		snprintf(buffer, buffer_length, "adminQ");
		return;
	} else if (thread_id == thread_config->packer_thread) {
		snprintf(buffer, buffer_length, "packerQ");
		return;
	} else if (thread_id == thread_config->dedupe_thread) {
		snprintf(buffer, buffer_length, "dedupeQ");
		return;
	} else if (thread_id == thread_config->bio_ack_thread) {
		snprintf(buffer, buffer_length, "ackQ");
		return;
	} else if (thread_id == thread_config->cpu_thread) {
		snprintf(buffer, buffer_length, "cpuQ");
		return;
	}

	if (get_zone_thread_name(thread_config->logical_threads,
				 thread_config->logical_zone_count,
				 thread_id,
				 "logQ",
				 buffer,
				 buffer_length)) {
		return;
	}

	if (get_zone_thread_name(thread_config->physical_threads,
				 thread_config->physical_zone_count,
				 thread_id,
				 "physQ",
				 buffer,
				 buffer_length)) {
		return;
	}

	if (get_zone_thread_name(thread_config->hash_zone_threads,
				 thread_config->hash_zone_count,
				 thread_id,
				 "hashQ",
				 buffer,
				 buffer_length)) {
		return;
	}

	if (get_zone_thread_name(thread_config->bio_threads,
				 thread_config->bio_thread_count,
				 thread_id,
				 "bioQ",
				 buffer,
				 buffer_length)) {
		return;
	}

	/* Some sort of misconfiguration? */
	snprintf(buffer, buffer_length, "reqQ%d", thread_id);
}
