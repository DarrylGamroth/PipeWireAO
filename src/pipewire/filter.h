/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_FILTER_H
#define PIPEWIRE_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup pw_filter Filter
 *
 * \brief PipeWire filter object class
 *
 * The filter object provides a convenient way to implement
 * processing filters.
 *
 * See also \ref api_pw_core
 */

/**
 * \addtogroup pw_filter
 * \{
 */
struct pw_filter;

#include <spa/buffer/buffer.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/pod/command.h>
#include <spa/pod/event.h>

#include <pipewire/core.h>
#include <pipewire/stream.h>

/** \enum pw_filter_state The state of a filter  */
enum pw_filter_state {
	PW_FILTER_STATE_ERROR = -1,		/**< the stream is in error */
	PW_FILTER_STATE_UNCONNECTED = 0,	/**< unconnected */
	PW_FILTER_STATE_CONNECTING = 1,		/**< connection is in progress */
	PW_FILTER_STATE_PAUSED = 2,		/**< filter is connected and paused */
	PW_FILTER_STATE_STREAMING = 3		/**< filter is streaming */
};

#if 0
struct pw_buffer {
	struct spa_buffer *buffer;	/**< the spa buffer */
	void *user_data;		/**< user data attached to the buffer */
	uint64_t size;			/**< For input ports, this field is set by pw_filter
					  *  with the duration of the buffer in ticks.
					  *  For output ports, this field is set by the user.
					  *  This field is added for all queued buffers and
					  *  returned in the time info. */
};
#endif

/** Events for a filter. These events are always called from the mainloop
 * unless explicitly documented otherwise. */
struct pw_filter_events {
#define PW_VERSION_FILTER_EVENTS	1
	uint32_t version;

	void (*destroy) (void *data);
	/** when the filter state changes. Since 1.4 this also sets errno when the
	 * new state is PW_FILTER_STATE_ERROR */
	void (*state_changed) (void *data, enum pw_filter_state old,
				enum pw_filter_state state, const char *error);

	/** when io changed on a port of the filter (when port_data is NULL). */
	void (*io_changed) (void *data, void *port_data,
			uint32_t id, void *area, uint32_t size);
	/** when a parameter changed on a port of the filter (when port_data is NULL). */
	void (*param_changed) (void *data, void *port_data,
			uint32_t id, const struct spa_pod *param);

        /** when a new buffer was created for a port */
        void (*add_buffer) (void *data, void *port_data, struct pw_buffer *buffer);
        /** when a buffer was destroyed for a port */
        void (*remove_buffer) (void *data, void *port_data, struct pw_buffer *buffer);

        /** do processing. This is normally called from the
	 *  mainloop but can also be called directly from the realtime data
	 *  thread if the user is prepared to deal with this with the
	 *  PW_FILTER_FLAG_RT_PROCESS. Only call methods marked with RT safe
	 *  from this event when called from the realtime thread. */
        void (*process) (void *data, struct spa_io_position *position);

	/** The filter is drained */
        void (*drained) (void *data);

	/** A command notify, Since 0.3.39:1 */
	void (*command) (void *data, const struct spa_command *command);
};

/** Convert a filter state to a readable string  */
const char * pw_filter_state_as_string(enum pw_filter_state state);

/** \enum pw_filter_flags Extra flags that can be used in \ref pw_filter_connect()  */
enum pw_filter_flags {
	PW_FILTER_FLAG_NONE = 0,			/**< no flags */
	PW_FILTER_FLAG_INACTIVE		= (1 << 0),	/**< start the filter inactive,
							  *  pw_filter_set_active() needs to be
							  *  called explicitly */
	PW_FILTER_FLAG_DRIVER		= (1 << 1),	/**< be a driver */
	PW_FILTER_FLAG_RT_PROCESS	= (1 << 2),	/**< call process from the realtime
							  *  thread. Only call methods marked as
							  *  RT safe. */
	PW_FILTER_FLAG_CUSTOM_LATENCY	= (1 << 3),	/**< don't call the default latency algorithm
							  *  but emit the param_changed event for the
							  *  ports when Latency params are received. */
	PW_FILTER_FLAG_TRIGGER		= (1 << 4),	/**< the filter will not be scheduled
							  *  automatically but _trigger_process()
							  *  needs to be called. This can be used
							  *  when the filter depends on processing
							  *  of other filters. */
	PW_FILTER_FLAG_ASYNC		= (1 << 5),	/**< Buffers will not be dequeued/queued from
							  *  the realtime process() function. This is
							  *  assumed when RT_PROCESS is unset but can
							  *  also be the case when the process() function
							  *  does a trigger_process() that will then
							  *  dequeue/queue a buffer from another process()
							  *  function. since 0.3.73 */
};

enum pw_filter_port_flags {
	PW_FILTER_PORT_FLAG_NONE		= 0,		/**< no flags */
	PW_FILTER_PORT_FLAG_MAP_BUFFERS		= (1 << 0),	/**< mmap the buffers except DmaBuf that is not
								  *  explicitly marked as mappable. */
	PW_FILTER_PORT_FLAG_ALLOC_BUFFERS	= (1 << 1),	/**< the application will allocate buffer
								  *  memory. In the add_buffer event, the
								  *  data of the buffer should be set */
};

/** Create a new unconnected \ref pw_filter
 * \return a newly allocated \ref pw_filter */
struct pw_filter *
pw_filter_new(struct pw_core *core,		/**< a \ref pw_core */
	      const char *name,			/**< a filter media name */
	      struct pw_properties *props	/**< filter properties, ownership is taken */);

struct pw_filter *
pw_filter_new_simple(struct pw_loop *loop,		/**< a \ref pw_loop to use */
		     const char *name,			/**< a filter media name */
		     struct pw_properties *props,	/**< filter properties, ownership is taken */
		     const struct pw_filter_events *events,	/**< filter events */
		     void *data					/**< data passed to events */);

/** Destroy a filter  */
void pw_filter_destroy(struct pw_filter *filter);

void pw_filter_add_listener(struct pw_filter *filter,
			    struct spa_hook *listener,
			    const struct pw_filter_events *events,
			    void *data);

/** Get the current filter state. Since 1.4 this also sets errno when the
 * state is PW_FILTER_STATE_ERROR */
enum pw_filter_state pw_filter_get_state(struct pw_filter *filter, const char **error);

const char *pw_filter_get_name(struct pw_filter *filter);

struct pw_core *pw_filter_get_core(struct pw_filter *filter);

/** Connect a filter for processing.
 * \return 0 on success < 0 on error.
 *
 * You should connect to the process event and use pw_filter_dequeue_buffer()
 * to get the latest metadata and data. */
int
pw_filter_connect(struct pw_filter *filter,		/**< a \ref pw_filter */
		  enum pw_filter_flags flags,		/**< filter flags */
		  const struct spa_pod **params,	/**< an array with params. */
		  uint32_t n_params			/**< number of items in \a params */);

/** Get the node ID of the filter.
 * \return node ID. */
uint32_t
pw_filter_get_node_id(struct pw_filter *filter);

/** Disconnect \a filter  */
int pw_filter_disconnect(struct pw_filter *filter);

/** add a port to the filter, returns user data of port_data_size. */
void *pw_filter_add_port(struct pw_filter *filter,	/**< a \ref pw_filter */
		enum pw_direction direction,		/**< port direction */
		enum pw_filter_port_flags flags,	/**< port flags */
		size_t port_data_size,			/**< allocated and given to the user as port_data */
		struct pw_properties *props,		/**< port properties, ownership is taken */
		const struct spa_pod **params,		/**< an array of params. The params should
							  *  ideally contain the supported formats */
		uint32_t n_params			/**< number of elements in \a params */);

/** remove a port from the filter */
int pw_filter_remove_port(void *port_data		/**< data associated with port */);

/** get properties, port_data of NULL will give global properties */
const struct pw_properties *pw_filter_get_properties(struct pw_filter *filter,
		void *port_data);

/** Update properties, use NULL port_data for global filter properties */
int pw_filter_update_properties(struct pw_filter *filter,
		void *port_data, const struct spa_dict *dict);

/** Set the filter in error state */
int pw_filter_set_error(struct pw_filter *filter,	/**< a \ref pw_filter */
			int res,			/**< a result code */
			const char *error,		/**< an error message */
			...
			) SPA_PRINTF_FUNC(3, 4);

/** Update params, use NULL port_data for global filter params */
int
pw_filter_update_params(struct pw_filter *filter,	/**< a \ref pw_filter */
			void *port_data,		/**< data associated with port */
			const struct spa_pod **params,	/**< an array of params. */
			uint32_t n_params		/**< number of elements in \a params */);


/** Query the time on the filter, deprecated, use the spa_io_position in the
 * process() method for timing information. RT safe. */
SPA_DEPRECATED
int pw_filter_get_time(struct pw_filter *filter, struct pw_time *time);

/** Get the current time in nanoseconds. This value can be compared with
 * the nsec value in the spa_io_position. RT safe. Since 1.1.0 */
uint64_t pw_filter_get_nsec(struct pw_filter *filter);

/** Get the data loop that is doing the processing of this filter. This loop
 * is assigned after pw_filter_connect(). Since 1.1.0 */
struct pw_loop *pw_filter_get_data_loop(struct pw_filter *filter);

/** Get a buffer that can be filled for output ports or consumed
 * for input ports. RT safe.
 *
 * Calls that dequeue, queue, begin, or end buffers on the same port must be
 * serialized by one worker. An input latest-buffer port may hold at most one
 * dequeued consumer buffer at a time. */
struct pw_buffer *pw_filter_dequeue_buffer(void *port_data);

/**
 * Try to claim a buffer directly from an input latest-buffer mailbox. RT safe.
 *
 * This skips the ordinary port queue and does not read or write errno. Returns
 * 1 and stores the claimed buffer in \a buffer, 0 when no publication is
 * currently visible, or a negative errno-style result for invalid state. The
 * caller must own the port's serialized buffer worker and must return a
 * claimed buffer before trying again.
 */
int pw_filter_try_dequeue_buffer_latest(void *port_data,
		struct pw_buffer **buffer);

/**
 * Caller-owned state for one continuous latest-input polling interval.
 *
 * Initialize this object with \ref pw_filter_buffer_latest_poller_init and
 * clear it on every cancellation or error exit. Do not copy or modify it while
 * initialized. A poller may retain a live-link lifetime pin across empty polls,
 * so its worker must continue polling and must not block between calls.
 */
struct pw_filter_buffer_latest_poller {
	void *port_data;
	struct spa_io_buffers_latest *io;
	uint32_t slot;
	uint32_t reserved;
};

#define PW_FILTER_BUFFER_LATEST_POLLER_INIT \
	((struct pw_filter_buffer_latest_poller) { NULL, NULL, SPA_ID_INVALID, 0 })

/**
 * Initialize a continuous latest-input polling interval. RT safe.
 *
 * The exclusive input worker must not hold a dequeued buffer. Initialization
 * performs all port-mode validation outside the empty-poll loop.
 */
int pw_filter_buffer_latest_poller_init(
		struct pw_filter_buffer_latest_poller *poller, void *port_data);

/**
 * Try to claim one input publication through an initialized poller. RT safe.
 *
 * Returns 1 and stores a buffer, 0 for ordinary no-work, or a negative
 * errno-style error. A successful claim or an error automatically releases
 * the retained link pin and finishes the polling interval. Empty polls retain
 * the pin while the link remains active. Link retirement is observed before
 * dereferencing its shared mailbox and releases the pin before returning 0.
 */
int pw_filter_buffer_latest_poller_try_dequeue(
		struct pw_filter_buffer_latest_poller *poller,
		struct pw_buffer **buffer);

/**
 * Finish a polling interval and release any retained live-link pin. RT safe.
 *
 * This operation is idempotent. Call it before a polling worker blocks, exits,
 * or stops checking the link so synchronous live-link retirement can finish.
 */
void pw_filter_buffer_latest_poller_clear(
		struct pw_filter_buffer_latest_poller *poller);

/**
 * Producer-local accounting for bounded latest-buffer acquisition.
 *
 * These counters are written only by the exclusive latest-buffer output
 * worker. They may be read by that worker after it has stopped publishing;
 * concurrent control-thread reads are not supported.
 */
struct pw_filter_buffer_latest_stats {
	uint64_t dequeue_attempts;	/**< output acquisition duty cycles */
	uint64_t recycle_returns;	/**< returned consumer leases examined */
	uint64_t buffer_probes;		/**< reusable pool slots examined */
	uint64_t pool_exhaustions;	/**< attempts with no safe allocation */
	uint64_t ready_reclaims;	/**< unclaimed publications reclaimed */
	uint64_t ready_withdrawals;	/**< subscriber ready slots withdrawn */
	uint64_t publications;		/**< output buffers offered to fan-out */
	uint64_t subscriber_visits;	/**< subscriber mailboxes visited */
	uint64_t subscriber_deliveries;	/**< subscriber leases created */
	uint64_t subscriber_supersessions; /**< subscriber-local ready IDs replaced */
	uint64_t subscriber_retirements; /**< retired slots acknowledged by producer */
	uint64_t retired_leases;	/**< leases recovered during retirement */
	uint64_t zero_recipient_publications; /**< offers delivered to no active slot */
	uint32_t max_buffer_probes;	/**< largest single bounded scan */
	uint32_t max_recycle_returns;	/**< largest aggregate drain per attempt */
	uint32_t max_ready_withdrawals; /**< largest reclaim fan-out per attempt */
	uint32_t max_subscriber_visits; /**< largest publication fan-out */
};

/**
 * Snapshot bounded latest-buffer output acquisition accounting. RT safe.
 *
 * The caller must own the exclusive latest-buffer output worker and must not
 * race this operation with publication. Returns -ENOTSUP for ordinary or
 * input ports.
 */
int pw_filter_get_buffer_latest_stats(void *port_data,
		struct pw_filter_buffer_latest_stats *stats);

/** Submit a buffer for playback or recycle a buffer for capture. RT safe.
 * The caller must own the port's serialized buffer worker. */
int pw_filter_queue_buffer(void *port_data, struct pw_buffer *buffer);

/**
 * Announce an output buffer on a graph-independent latest-buffer port while
 * retaining its producer lease. RT safe.
 *
 * The caller must initialize and publish its application-defined active state
 * before this call. It may continue to write only storage that the negotiated
 * progressive protocol still grants to the producer. The buffer must later be
 * passed exactly once to \ref pw_filter_end_progressive_buffer, not to
 * \ref pw_filter_queue_buffer.
 *
 * The caller must own the port's serialized output worker. Fan-out publication
 * is latest-value delivery to independent subscribers, not an atomic multicast:
 * subscribers may claim or supersede an offered buffer at different times.
 * Per-subscriber leases keep the allocation unavailable for reuse until every
 * subscriber has returned or superseded it and the producer lease has ended.
 *
 * This operation is supported only on an output port configured with
 * SPA_IO_BuffersLatest.
 */
int pw_filter_begin_progressive_buffer(void *port_data, struct pw_buffer *buffer);

/**
 * End the producer lease of an announced progressive output buffer. RT safe.
 *
 * Before this call, the producer must stop writing and publish its negotiated
 * terminal state. PipeWire makes the allocation reusable only after the input
 * consumer has also returned its lease. Consumer return and this call may
 * occur in either order.
 *
 * The caller must own the same serialized output worker that began the lease.
 */
int pw_filter_end_progressive_buffer(void *port_data, struct pw_buffer *buffer);

/**
 * Get the borrowed advisory notification fd for a latest-buffer port. RT safe.
 *
 * The descriptor is owned by the filter and remains valid only while the port
 * is connected. Callers must not close it. A readable descriptor is only a
 * hint: consumers must always retry \ref pw_filter_dequeue_buffer because
 * notifications can coalesce or be stale. Returns -ENOTSUP when the port is
 * not using SPA_IO_BuffersLatest and -ENODEV when its selected wait policy has
 * no notification descriptor.
 */
int pw_filter_get_buffer_latest_fd(void *port_data);

/** Get a data pointer to the buffer data. RT safe. */
void *pw_filter_get_dsp_buffer(void *port_data, uint32_t n_samples);

/** Activate or deactivate the filter  */
int pw_filter_set_active(struct pw_filter *filter, bool active);

/** Flush a filter. When \a drain is true, the drained callback will
 * be called when all data is played or recorded. The filter can be resumed
 * after the drain by setting it active again with
 * \ref pw_filter_set_active(). A flush without a drain is mostly useful afer
 * a state change to PAUSED, to flush any remaining data from the queues.
 * RT safe. */
int pw_filter_flush(struct pw_filter *filter, bool drain);

/** Check if the filter is driving. The filter needs to have the
 * PW_FILTER_FLAG_DRIVER set. When the filter is driving,
 * pw_filter_trigger_process() needs to be called when data is
 * available (output) or needed (input). Since 0.3.66 */
bool pw_filter_is_driving(struct pw_filter *filter);

/** Check if the graph is using lazy scheduling.
 * Since 1.4.0 */
bool pw_filter_is_lazy(struct pw_filter *filter);

/** Trigger a push/pull on the filter. One iteration of the graph will
 * be scheduled and process() will be called. RT safe. Since 0.3.66 */
int pw_filter_trigger_process(struct pw_filter *filter);

/** Emit an event from this filter. RT safe.
 * Since 1.2.6 */
int pw_filter_emit_event(struct pw_filter *filter, const struct spa_event *event);

/**
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_FILTER_H */
