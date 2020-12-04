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
 * $Id: //eng/uds-releases/krusty/src/uds/request.h#13 $
 */

#ifndef REQUEST_H
#define REQUEST_H

#include "cacheCounters.h"
#include "common.h"
#include "compiler.h"
#include "opaqueTypes.h"
#include "threads.h"
#include "timeUtils.h"
#include "uds.h"
#include "util/funnelQueue.h"

/**
 * enum request_action values indicate what action, command, or query is to be
 * performed when processing a Request instance.
 **/
enum request_action {
	// Map the API's uds_callback_type values directly to a corresponding
	// action.
	REQUEST_INDEX = UDS_POST,
	REQUEST_UPDATE = UDS_UPDATE,
	REQUEST_DELETE = UDS_DELETE,
	REQUEST_QUERY = UDS_QUERY,

	REQUEST_CONTROL,

	// REQUEST_SPARSE_CACHE_BARRIER is the action for the control request
	// used
	// by localIndexRouter.
	REQUEST_SPARSE_CACHE_BARRIER,

	// REQUEST_ANNOUNCE_CHAPTER_CLOSED is the action for the control
	// request used by an indexZone to signal the other zones that it
	// has closed the current open chapter.
	REQUEST_ANNOUNCE_CHAPTER_CLOSED,
};

/**
 * The block's rough location in the index, if any.
 **/
enum index_region {
	/* the block doesn't exist or the location isn't available */
	LOC_UNAVAILABLE,
	/* if the block was found in the open chapter */
	LOC_IN_OPEN_CHAPTER,
	/* if the block was found in the dense part of the index */
	LOC_IN_DENSE,
	/* if the block was found in the sparse part of the index */
	LOC_IN_SPARSE
};

/**
 * Abstract request pipeline stages, which can also be viewed as stages in the
 * life-cycle of a request.
 **/
enum request_stage {
	STAGE_TRIAGE,
	STAGE_INDEX,
	STAGE_CALLBACK,
};

/**
 * Control message fields for the barrier messages used to coordinate the
 * addition of a chapter to the sparse chapter index cache.
 **/
struct barrier_message_data {
	/** virtual chapter number of the chapter index to add to the sparse
	 * cache */
	uint64_t virtual_chapter;
};

/**
 * Control message fields for the chapter closed messages used to inform
 * lagging zones of the first zone to close a given open chapter.
 **/
struct chapter_closed_message_data {
	/** virtual chapter number of the chapter which was closed */
	uint64_t virtual_chapter;
};

/**
 * Union of the all the zone control message fields. The  request_action field
 * (or launch function argument) selects which of the members is valid.
 **/
union zone_message_data {
	/** for REQUEST_SPARSE_CACHE_BARRIER */
	struct barrier_message_data barrier;
	/** for REQUEST_ANNOUNCE_CHAPTER_CLOSED */
	struct chapter_closed_message_data chapter_closed;
};

struct zone_message {
	/** the index to which the message is directed */
	struct index *index;
	/** the message specific data */
	union zone_message_data data;
};

/**
 * Request context for queuing throughout the uds pipeline
 *
 * XXX Note that the typedef for this struct defines "Request", and that this
 *     should therefore be "struct request".  However, this conflicts with the
 *     Linux kernel which also has a "struct request".  This is a workaround so
 *     that we can make upstreaming progress.  The real solution is to expose
 *     this structure as the true "struct uds_request" and do a lot of
 *     renaming.
 **/
struct internal_request {
	/*
	 * The first part of this structure must be exactly parallel to the
	 * UdsRequest structure, which is part of the public UDS API.
	 */
	struct uds_chunk_name chunk_name;   // hash value
	struct uds_chunk_data old_metadata; // metadata from index
	struct uds_chunk_data new_metadata; // metadata from request
	uds_chunk_callback_t *callback;     // callback method when complete
	struct uds_index_session *session;  // The public index session
	enum uds_callback_type type;        // the type of request
	int status;                         // success/error code for request
	bool found;                         // True if the block found in index
	bool update;                        // move record to newest chapter
					    // if found

	/*
	 * The remainder of this structure is private to the UDS
	 * implementation.
	 */
	struct funnel_queue_entry request_queue_link; // for lock-free request
						      // queue
	Request *next_request;
	struct index_router *router;

	// Data for control message requests
	struct zone_message zone_message;
	bool is_control_message;

	bool unbatched;                // if true, wake worker when enqueued
	bool requeued;
	enum request_action action;    // the action for the index to perform
	unsigned int zone_number;      // the zone for this request to use
	enum index_region location;    // if and where the block was found

	bool sl_location_known;        // slow lane has determined a location
	enum index_region sl_location; // location determined by slowlane
};

typedef void (*request_restarter_t)(Request *);

/**
 * Make an asynchronous control message for an index zone and enqueue it for
 * processing.
 *
 * @param action   The control action to perform
 * @param message  The message to send
 * @param zone     The zone number of the zone to receive the message
 * @param router   The index router responsible for handling the message
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check launch_zone_control_message(enum request_action action,
					     struct zone_message message,
					     unsigned int zone,
					     struct index_router *router);

/**
 * Enqueue a request for the next stage of the pipeline. If there is more than
 * one possible queue for a stage, this function uses the request to decide
 * which queue should handle it.
 *
 * @param request       The request to enqueue
 * @param next_stage    The next stage of the pipeline to process the request
 **/
void enqueue_request(Request *request, enum request_stage next_stage);

/**
 * A method to restart delayed requests.
 *
 * @param request    The request to restart
 **/
void restart_request(Request *request);

/**
 * Set the function pointer which is used to restart requests.
 * This is needed by albserver code and is used as a test hook by the unit
 * tests.
 *
 * @param restarter   The function to call to restart requests.
 **/
void set_request_restarter(request_restarter_t restarter);

/**
 * Enter the callback stage of processing for a request, notifying the waiting
 * thread if the request is synchronous, freeing the request if it is an
 * asynchronous control message, or placing it on the callback queue if it is
 * an asynchronous client request.
 *
 * @param request  the request which has completed execution
 **/
void enter_callback_stage(Request *request);

/**
 * Update the context statistics to reflect the successful completion of a
 * client request.
 *
 * @param request  a client request that has successfully completed execution
 **/
void update_request_context_stats(Request *request);

/**
 * Compute the cache_probe_type value reflecting the request and page type.
 *
 * @param request      The request being processed, or NULL
 * @param is_index_page  Whether the cache probe will be for an index page
 *
 * @return the cache probe type enumeration
 **/
static INLINE enum cache_probe_type cache_probe_type(Request *request,
						     bool is_index_page)
{
	if ((request != NULL) && request->requeued) {
		return is_index_page ? CACHE_PROBE_INDEX_RETRY :
				       CACHE_PROBE_RECORD_RETRY;
	} else {
		return is_index_page ? CACHE_PROBE_INDEX_FIRST :
				       CACHE_PROBE_RECORD_FIRST;
	}
}
#endif /* REQUEST_H */
