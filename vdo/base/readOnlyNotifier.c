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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyNotifier.c#14 $
 */

#include "readOnlyNotifier.h"

#include "atomic.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "physicalLayer.h"
#include "threadConfig.h"

/**
 * A read_only_notifier has a single completion which is used to perform
 * read-only notifications, however, enter_read_only_mode() may be called from
 * any base thread. A pair of atomic fields are used to control the read-only
 * mode entry process. The first field holds the read-only error. The second is
 * the state field, which may hold any of the four special values enumerated
 * here.
 *
 * When enter_read_only_mode() is called from some base thread, a
 * compare-and-swap is done on the readOnlyError, setting it to the supplied
 * error if the value was VDO_SUCCESS. If this fails, some other thread has
 * already intiated read-only entry or scheduled a pending entry, so the call
 * exits. Otherwise, a compare-and-swap is done on the state, setting it to
 * NOTIFYING if the value was MAY_NOTIFY. If this succeeds, the caller initiates
 * the notification. If this failed due to notifications being disallowed, the
 * notifier will be in the MAY_NOT_NOTIFY state but readOnlyError will not be
 * VDO_SUCCESS. This configuration will indicate to allow_read_only_mode_entry()
 * that there is a pending notification to perform.
 **/
enum {
	/** Notifications are allowed but not in progress */
	MAY_NOTIFY = 0,
	/** A notification is in progress */
	NOTIFYING,
	/** Notifications are not allowed */
	MAY_NOT_NOTIFY,
	/** A notification has completed */
	NOTIFIED,
};

/**
 * An object to be notified when the VDO enters read-only mode
 **/
struct read_only_listener {
	/** The listener */
	void *listener;
	/** The method to call to notify the listener */
	read_only_notification *notify;
	/** A pointer to the next listener */
	struct read_only_listener *next;
};

/**
 * Data associated with each base code thread.
 **/
struct thread_data {
	/**
	 * Each thread maintains its own notion of whether the VDO is read-only
	 * so that the read-only state can be checked from any base thread
	 * without worrying about synchronization or thread safety. This does
	 * mean that knowledge of the VDO going read-only does not occur
	 * simultaneously across the VDO's threads, but that does not seem to
	 * cause any problems.
	 */
	bool is_read_only;
	/**
	 * A list of objects waiting to be notified on this thread that the VDO
	 * has entered read-only mode.
	 **/
	struct read_only_listener *listeners;
};

struct read_only_notifier {
	/** The completion for entering read-only mode */
	struct vdo_completion completion;
	/** A completion waiting for notifications to be drained or enabled */
	struct vdo_completion *waiter;
	/** The code of the error which put the VDO into read-only mode */
	Atomic32 read_only_error;
	/** The current state of the notifier (values described above) */
	Atomic32 state;
	/** The thread config of the VDO */
	const struct thread_config *thread_config;
	/** The array of per-thread data */
	struct thread_data thread_data[];
};

/**
 * Convert a generic vdo_completion to a read_only_notifier.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a read_only_notifier
 **/
static inline struct read_only_notifier *
as_notifier(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, READ_ONLY_MODE_COMPLETION);
	return container_of(completion, struct read_only_notifier, completion);
}

/**********************************************************************/
int make_read_only_notifier(bool is_read_only,
			    const struct thread_config *thread_config,
			    PhysicalLayer *layer,
			    struct read_only_notifier **notifier_ptr)
{
	struct read_only_notifier *notifier;
	int result = ALLOCATE_EXTENDED(struct read_only_notifier,
				       thread_config->base_thread_count,
				       struct thread_data,
				       __func__,
				       &notifier);
	if (result != VDO_SUCCESS) {
		return result;
	}

	notifier->thread_config = thread_config;
	if (is_read_only) {
		atomicStore32(&notifier->read_only_error,
			      (uint32_t) VDO_READ_ONLY);
		atomicStore32(&notifier->state, NOTIFIED);
	} else {
		atomicStore32(&notifier->state, MAY_NOTIFY);
	}
	result = initialize_enqueueable_completion(&notifier->completion,
						   READ_ONLY_MODE_COMPLETION,
						   layer);
	if (result != VDO_SUCCESS) {
		free_read_only_notifier(&notifier);
		return result;
	}

	ThreadCount id;
	for (id = 0; id < thread_config->base_thread_count; id++) {
		notifier->thread_data[id].is_read_only = is_read_only;
	}

	*notifier_ptr = notifier;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_read_only_notifier(struct read_only_notifier **notifier_ptr)
{
	struct read_only_notifier *notifier = *notifier_ptr;
	if (notifier == NULL) {
		return;
	}

	ThreadCount id;
	for (id = 0; id < notifier->thread_config->base_thread_count; id++) {
		struct thread_data *thread_data = &notifier->thread_data[id];
		struct read_only_listener *listener = thread_data->listeners;
		while (listener != NULL) {
			struct read_only_listener *to_free = listener;
			listener = listener->next;
			FREE(to_free);
		}
	}

	destroy_enqueueable(&notifier->completion);
	FREE(notifier);
	*notifier_ptr = NULL;
}

/**
 * Check that a function was called on the admin thread.
 *
 * @param notifier  The notifier
 * @param caller    The name of the function (for logging)
 **/
static void assert_on_admin_thread(struct read_only_notifier *notifier,
				   const char *caller)
{
	ThreadID threadID = getCallbackThreadID();
	ASSERT_LOG_ONLY((get_admin_thread(notifier->thread_config) == threadID),
			"%s called on admin thread",
			caller);
}

/**********************************************************************/
void wait_until_not_entering_read_only_mode(struct read_only_notifier *notifier,
					    struct vdo_completion *parent)
{
	if (notifier == NULL) {
		finish_completion(parent, VDO_SUCCESS);
		return;
	}

	assert_on_admin_thread(notifier, __func__);
	if (notifier->waiter != NULL) {
		finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	uint32_t state = atomicLoad32(&notifier->state);
	if ((state == MAY_NOT_NOTIFY) || (state == NOTIFIED)) {
		// Notifications are already done or disallowed.
		complete_completion(parent);
		return;
	}

	if (compareAndSwap32(&notifier->state, MAY_NOTIFY, MAY_NOT_NOTIFY)) {
		// A notification was not in progress, and now they are
		// disallowed.
		complete_completion(parent);
		return;
	}

	/*
	 * A notification is in progress, so wait for it to finish. There is no
	 * race here since the notification can't finish while the admin thread
	 * is in this method.
	 */
	notifier->waiter = parent;
}

/**
 * Complete the process of entering read only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void finish_entering_read_only_mode(struct vdo_completion *completion)
{
	struct read_only_notifier *notifier = as_notifier(completion);
	assert_on_admin_thread(notifier, __func__);
	atomicStore32(&notifier->state, NOTIFIED);

	struct vdo_completion *waiter = notifier->waiter;
	if (waiter != NULL) {
		notifier->waiter = NULL;
		finish_completion(waiter, completion->result);
	}
}

/**
 * Inform each thread that the VDO is in read-only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void make_thread_read_only(struct vdo_completion *completion)
{
	ThreadID thread_id = completion->callbackThreadID;
	struct read_only_notifier *notifier = as_notifier(completion);
	struct read_only_listener *listener = completion->parent;
	if (listener == NULL) {
		// This is the first call on this thread
		struct thread_data *thread_data =
			&notifier->thread_data[thread_id];
		thread_data->is_read_only = true;
		listener = thread_data->listeners;
		if (thread_id == 0) {
			// Note: This message must be recognizable by
			// Permabit::UserMachine.
			logErrorWithStringError((int) atomicLoad32(&notifier->read_only_error),
						"Unrecoverable error, entering read-only mode");
		}
	} else {
		// We've just finished notifying a listener
		listener = listener->next;
	}

	if (listener != NULL) {
		// We have a listener to notify
		prepare_completion(completion,
				   make_thread_read_only,
				   make_thread_read_only,
				   thread_id,
				   listener);
		listener->notify(listener->listener, completion);
		return;
	}

	// We're done with this thread
	if (++thread_id >= notifier->thread_config->base_thread_count) {
		// There are no more threads
		prepare_completion(completion,
				   finish_entering_read_only_mode,
				   finish_entering_read_only_mode,
				   get_admin_thread(notifier->thread_config),
				   NULL);
	} else {
		prepare_completion(completion,
				   make_thread_read_only,
				   make_thread_read_only,
				   thread_id,
				   NULL);
	}

	invoke_callback(completion);
}

/**********************************************************************/
void allow_read_only_mode_entry(struct read_only_notifier *notifier,
				struct vdo_completion *parent)
{
	assert_on_admin_thread(notifier, __func__);
	if (notifier->waiter != NULL) {
		finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	if (!compareAndSwap32(&notifier->state, MAY_NOT_NOTIFY, MAY_NOTIFY)) {
		// Notifications were already allowed or complete
		complete_completion(parent);
		return;
	}

	if ((int) atomicLoad32(&notifier->read_only_error) == VDO_SUCCESS) {
		// We're done
		complete_completion(parent);
		return;
	}

	// There may have been a pending notification
	if (!compareAndSwap32(&notifier->state, MAY_NOTIFY, NOTIFYING)) {
		/*
		 * There wasn't, the error check raced with a thread calling
		 * enter_read_only_mode() after we set the state to MAY_NOTIFY.
		 * It has already started the notification.
		 */
		complete_completion(parent);
		return;
	}

	// Do the pending notification.
	notifier->waiter = parent;
	make_thread_read_only(&notifier->completion);
}

/**********************************************************************/
void enter_read_only_mode(struct read_only_notifier *notifier, int errorCode)
{
	struct thread_data *thread_data =
		&notifier->thread_data[getCallbackThreadID()];
	if (thread_data->is_read_only) {
		// This thread has already gone read-only.
		return;
	}

	// Record for this thread that the VDO is read-only.
	thread_data->is_read_only = true;

	if (!compareAndSwap32(&notifier->read_only_error,
			      (uint32_t) VDO_SUCCESS,
			      (uint32_t) errorCode)) {
		// The notifier is already aware of a read-only error
		return;
	}

	if (compareAndSwap32(&notifier->state, MAY_NOTIFY, NOTIFYING)) {
		// Initiate a notification starting on the lowest numbered
		// thread.
		launch_callback(&notifier->completion, make_thread_read_only, 0);
	}
}

/**********************************************************************/
bool is_read_only(struct read_only_notifier *notifier)
{
	return notifier->thread_data[getCallbackThreadID()].is_read_only;
}

/**********************************************************************/
int register_read_only_listener(struct read_only_notifier *notifier,
				void *listener,
				read_only_notification *notification,
				ThreadID thread_id)
{
	struct read_only_listener *read_only_listener;
	int result = ALLOCATE(1,
			      struct read_only_listener,
			      __func__,
			      &read_only_listener);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct thread_data *thread_data = &notifier->thread_data[thread_id];
	*read_only_listener = (struct read_only_listener) {
		     .listener = listener,
		     .notify = notification,
		     .next = thread_data->listeners,
	};

	thread_data->listeners = read_only_listener;
	return VDO_SUCCESS;
}
