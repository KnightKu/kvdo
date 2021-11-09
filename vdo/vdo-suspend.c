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

#include "vdo-suspend.h"

#include "logger.h"
#include "permassert.h"

#include "admin-completion.h"
#include "block-map.h"
#include "completion.h"
#include "dedupeIndex.h"
#include "kernel-types.h"
#include "logical-zone.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-init.h"

enum {
	SUSPEND_PHASE_START = 0,
	SUSPEND_PHASE_PACKER,
	SUSPEND_PHASE_DATA_VIOS,
	SUSPEND_PHASE_FLUSHES,
	SUSPEND_PHASE_LOGICAL_ZONES,
	SUSPEND_PHASE_BLOCK_MAP,
	SUSPEND_PHASE_JOURNAL,
	SUSPEND_PHASE_DEPOT,
	SUSPEND_PHASE_READ_ONLY_WAIT,
	SUSPEND_PHASE_WRITE_SUPER_BLOCK,
	SUSPEND_PHASE_END,
};

static const char *SUSPEND_PHASE_NAMES[] = {
	"SUSPEND_PHASE_START",
	"SUSPEND_PHASE_PACKER",
	"SUSPEND_PHASE_DATA_VIOS",
	"SUSPEND_PHASE_FLUSHES",
	"SUSPEND_PHASE_LOGICAL_ZONES",
	"SUSPEND_PHASE_BLOCK_MAP",
	"SUSPEND_PHASE_JOURNAL",
	"SUSPEND_PHASE_DEPOT",
	"SUSPEND_PHASE_READ_ONLY_WAIT",
	"SUSPEND_PHASE_WRITE_SUPER_BLOCK",
	"SUSPEND_PHASE_END",
};

/**
 * Implements vdo_thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		admin_completion->vdo->thread_config;
	switch (admin_completion->phase) {
	case SUSPEND_PHASE_PACKER:
	case SUSPEND_PHASE_FLUSHES:
		return thread_config->packer_thread;

	case SUSPEND_PHASE_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
	}
}

/**
 * Update the VDO state and save the super block.
 *
 * @param vdo         The vdo being suspended
 * @param completion  The admin_completion's sub-task completion
 **/
static void write_super_block(struct vdo *vdo,
			      struct vdo_completion *completion)
{
	switch (get_vdo_state(vdo)) {
	case VDO_DIRTY:
	case VDO_NEW:
		set_vdo_state(vdo, VDO_CLEAN);
		break;

	case VDO_CLEAN:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		break;

	case VDO_REPLAYING:
	default:
		finish_vdo_completion(completion, UDS_BAD_STATE);
		return;
	}

	save_vdo_components(vdo, completion);
}

/**
 * Callback to initiate a suspend, registered in suspend_vdo().
 *
 * @param completion  The sub-task completion
 **/
static void suspend_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;
	struct admin_state *admin_state = &vdo->admin_state;
	int result;

	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_SUSPEND);
	assert_vdo_admin_phase_thread(admin_completion, __func__,
				      SUSPEND_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case SUSPEND_PHASE_START:
		if (start_vdo_draining(admin_state,
				       vdo->suspend_type,
				       &admin_completion->completion,
				       NULL)) {
			complete_vdo_completion(reset_vdo_admin_sub_task(completion));
		}
		return;

	case SUSPEND_PHASE_PACKER:
		/*
		 * If the VDO was already resumed from a prior suspend while
		 * read-only, some of the components may not have been resumed.
		 * By setting a read-only error here, we guarantee that the
		 * result of this suspend will be VDO_READ_ONLY and not
		 * VDO_INVALID_ADMIN_STATE in that case.
		 */
		if (in_vdo_read_only_mode(vdo)) {
			set_vdo_completion_result(&admin_completion->completion,
						  VDO_READ_ONLY);
		}

		drain_vdo_packer(vdo->packer,
				 reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DATA_VIOS:
		drain_vdo_limiter(&vdo->request_limiter,
				  reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_FLUSHES:
		drain_vdo_flusher(vdo->flusher,
				  reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_LOGICAL_ZONES:
		/*
		 * Attempt to flush all I/O before completing post suspend
		 * work. We believe a suspended device is expected to have
		 * persisted all data written before the suspend, even if it
		 * hasn't been flushed yet.
		 */
		result = vdo_synchronous_flush(vdo);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo->read_only_notifier,
						 result);
		}

		drain_vdo_logical_zones(vdo->logical_zones,
					get_vdo_admin_state_code(admin_state),
					reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_BLOCK_MAP:
		drain_vdo_block_map(vdo->block_map,
				    get_vdo_admin_state_code(admin_state),
				    reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_JOURNAL:
		drain_vdo_recovery_journal(vdo->recovery_journal,
					   get_vdo_admin_state_code(admin_state),
					   reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DEPOT:
		drain_vdo_slab_depot(vdo->depot,
				     get_vdo_admin_state_code(admin_state),
				     reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_READ_ONLY_WAIT:
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
							   reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
		if (is_vdo_state_suspending(admin_state) ||
		    (admin_completion->completion.result != VDO_SUCCESS)) {
			// If we didn't save the VDO or there was an error,
			// we're done.
			break;
		}

		write_super_block(vdo, reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_END:
		suspend_vdo_dedupe_index(vdo->dedupe_index,
					 (vdo->suspend_type
					  == VDO_ADMIN_STATE_SAVING));
		break;

	default:
		set_vdo_completion_result(completion, UDS_BAD_STATE);
	}

	finish_vdo_draining_with_result(admin_state, completion->result);
}

/**********************************************************************/
int suspend_vdo(struct vdo *vdo)
{
	const char *device_name;
	int result;

	device_name = get_vdo_device_name(vdo->device_config->owning_target);
	uds_log_info("suspending device '%s'", device_name);

	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	result = perform_vdo_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_SUSPEND,
					     get_thread_id_for_phase,
					     suspend_callback,
					     preserve_vdo_completion_error_and_continue);

	// Treat VDO_READ_ONLY as a success since a read-only suspension still
	// leaves the VDO suspended.
	if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
		uds_log_info("device '%s' suspended", device_name);
		return VDO_SUCCESS;
	}

	if (result == VDO_INVALID_ADMIN_STATE) {
		uds_log_error("Suspend invoked while in unexpected state: %s",
			      get_vdo_admin_state(vdo)->name);
		result = -EINVAL;
	}

	uds_log_error_strerror(result,
			       "Suspend of device '%s' failed",
			       device_name);
	return result;
}
