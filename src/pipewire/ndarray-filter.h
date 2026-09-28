/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_NDARRAY_FILTER_H
#define PIPEWIRE_NDARRAY_FILTER_H

#include <stdint.h>

#include <spa/buffer/meta.h>
#include <spa/param/format.h>

#include <pipewire/filter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup pw_ndarray_filter Standalone ndarray filter
 *
 * Publish one callback-driven, packed-ndarray filter from an external
 * PipeWire client. The helper owns the PipeWire connection and buffer
 * mechanics; callbacks own their prepared numerical state.
 */

/** \{ */

#define PW_VERSION_NDARRAY_FILTER_EVENTS 2u
#define PW_VERSION_NDARRAY_FILTER_EVENTS_V1 1u
#define PW_VERSION_NDARRAY_FILTER_CONFIG 0u
#define PW_VERSION_NDARRAY_FILTER_EVENTS_PROGRESSIVE 3u
#define PW_VERSION_NDARRAY_FILTER_CONFIG_PROGRESSIVE 1u
#define PW_NDARRAY_FILTER_MAX_PORTS 1024u
#define PW_NDARRAY_FILTER_NAME_MAX 255u

/** Standalone ndarray filter execution options. */
enum pw_ndarray_filter_flags {
	PW_NDARRAY_FILTER_FLAG_NONE = 0,
	/**
	 * Invoke processing directly on a PipeWire real-time data-loop thread.
	 *
	 * Leave this unset when a runtime requires callbacks on the thread that
	 * runs the owned main loop. Runtimes that adopt foreign-created threads,
	 * including Julia callbacks created by its cfunction macro, may opt in
	 * after ensuring no exception or language unwind can cross the callback
	 * boundary.
	 */
	PW_NDARRAY_FILTER_FLAG_RT_PROCESS = (1u << 0),
	/**
	 * Invoke process when any frame-data input has arrived.
	 *
	 * The callback still receives every frame-data input in declaration
	 * order. Inputs without an arrival in that cycle carry
	 * PW_NDARRAY_FILTER_BUFFER_FLAG_INPUT_UNAVAILABLE and a NULL data
	 * pointer. A zero-sized input buffer is consumed as an explicit
	 * no-arrival token; this permits a PipeWire driver to satisfy graph-cycle
	 * scheduling without inventing a sample. The callback must mark every
	 * output unavailable when it only retains an input for a later joined
	 * cycle.
	 */
	PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS = (1u << 1),
	/**
	 * Accept Version 1 owner-mediated run-control requests.
	 *
	 * The filter connects inactive and publishes its initial stopped status.
	 * A controller may then request running or stopped through the node's
	 * SPA_PARAM_Props parameter. The helper applies the request locally and
	 * publishes a token-matched completion status.
	 */
	PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL = (1u << 2),
	/**
	 * Publish and accept the scientific owner's scalar Props surface.
	 *
	 * Version 2 property callbacks are required. Property requests execute on
	 * the owned main-loop thread. The owner calls
	 * pw_ndarray_filter_notify_properties() after a requested value becomes
	 * active at a frame boundary.
	 */
	PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES = (1u << 3),
	/**
	 * Accept Version 1 owner-mediated processing-state reset requests.
	 *
	 * Version 2 reset() is required. Reset is accepted only while processing
	 * is stopped and no run-control transition is pending.
	 */
	PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL = (1u << 4),
	/**
	 * Admit frame-data inputs in FIFO order without helper-side drops.
	 *
	 * Each valid dequeued input is retained until it has been presented to the
	 * process callback exactly once or processing terminates. Missing peer
	 * inputs or output buffers therefore apply bounded back pressure through
	 * the negotiated PipeWire buffer pools. After a callback, already queued
	 * input requests another graph cycle. The graph driver must service
	 * PipeWire RequestProcess commands, or provide its next scheduled cycle,
	 * for that input to make progress without another arrival. Leave this unset
	 * for the ordinary PipeWire drain-to-latest policy.
	 */
	PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS = (1u << 5),
	/** Poll committed progressive regions on a dedicated pinned worker CPU. */
	PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_BUSY_POLL = (1u << 6),
	/** Finish a progressive frame within its original filter process cycle. */
	PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE = (1u << 7),
	/**
	 * Keep the pinned worker polling for the next frame on a dedicated CPU.
	 * This consumes that CPU between frames. The worker switches to
	 * SCHED_OTHER to avoid exhausting Linux's finite real-time CPU budget.
	 * This flag cannot be combined with PROGRESSIVE_INLINE.
	 */
	PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE = (1u << 8),
};

/** Static ndarray-filter Port roles. */
enum pw_ndarray_filter_port_flags {
	PW_NDARRAY_FILTER_PORT_FLAG_NONE = 0,
	/**
	 * Sparse input Parameter Port prepared away from repeated processing.
	 *
	 * Parameter Ports must be inputs and must not declare a repeated rate.
	 * Their buffers are delivered to update_parameter() on a bounded worker
	 * rather than to the frame process callback. A buffer whose first data
	 * chunk has size zero means that no replacement is present in that graph
	 * cycle; it is recycled without invoking update_parameter().
	 */
	PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER = (1u << 0),
};

/** Metadata stored by value in one callback buffer. */
enum pw_ndarray_filter_metadata {
	PW_NDARRAY_FILTER_METADATA_NONE = 0,
	PW_NDARRAY_FILTER_METADATA_HEADER = (1u << 0),
	PW_NDARRAY_FILTER_METADATA_ACQUISITION = (1u << 1),
};

/** Per-callback buffer state controlled by an output callback. */
enum pw_ndarray_filter_buffer_flags {
	PW_NDARRAY_FILTER_BUFFER_FLAG_NONE = 0,
	/**
	 * Keep this output buffer for a later callback without publishing it.
	 *
	 * The helper clears this flag before every callback. It is valid only on
	 * output buffers. This permits progressive algorithms to consume several
	 * input blocks before publishing one completed output.
	 */
	PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE = (1u << 0),
	/**
	 * No buffer arrived for this input in the current callback.
	 *
	 * This flag is helper-owned and appears only on input buffers when
	 * PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS is enabled. The data pointer
	 * is NULL and the size, capacity, and metadata masks are zero.
	 */
	PW_NDARRAY_FILTER_BUFFER_FLAG_INPUT_UNAVAILABLE = (1u << 1),
};

/**
 * One borrowed packed ndarray supplied to a process callback.
 *
 * `data` is borrowed only until the callback returns, except for Version 1
 * progressive input: all its region views refer to one retained full-frame
 * buffer and remain readable through that frame's terminal process callback
 * or abort_progressive_frame() callback. A progressive callback may keep an
 * earlier input-region pointer only for that same frame on its worker.
 * An input payload is read-only even though the common C layout uses `void *`;
 * an output payload
 * is exclusively writable. `size` is the exact declared payload size and
 * `capacity` is the mapped capacity beginning at `data`.
 *
 * `metadata_available` reports which destination metadata records exist for
 * an output and which records were present and valid for an input. A callback
 * sets `metadata_valid` on outputs after filling the corresponding by-value
 * record. It may set `PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE` on an
	 * output to retain that buffer without publishing it. With independent input
	 * admission, an input without a new buffer carries
	 * `PW_NDARRAY_FILTER_BUFFER_FLAG_INPUT_UNAVAILABLE`. It must not change any
	 * structural field.
 */
struct pw_ndarray_filter_buffer {
	uint32_t struct_size;
	uint32_t flags;                 /**< mask of enum pw_ndarray_filter_buffer_flags */
	void *data;
	uint32_t size;
	uint32_t capacity;
	uint32_t metadata_available;    /**< mask of enum pw_ndarray_filter_metadata */
	uint32_t metadata_valid;        /**< subset of metadata_available */
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
};

/**
 * One exact packed ndarray format.
 *
 * `shape` and `schema` are borrowed during construction and copied by
 * pw_ndarray_filter_new(). Integer fields keep the ABI straightforward for
 * generated and foreign-language bindings.
 */
struct pw_ndarray_filter_format {
	uint32_t element_type;       /**< one of enum spa_element_type */
	uint32_t layout;             /**< one of enum spa_ndarray_layout */
	uint32_t rate_num;           /**< zero with rate_denom zero when absent */
	uint32_t rate_denom;
	uint32_t n_dimensions;
	const uint32_t *shape;
	const char *schema;          /**< optional semantic schema */
};

/** Static description of one external node port. */
struct pw_ndarray_filter_port {
	uint32_t struct_size;
	uint32_t flags;                 /**< mask of enum pw_ndarray_filter_port_flags */
	uint32_t direction;             /**< one of enum spa_direction */
	uint32_t reserved;              /**< zero */
	const char *name;               /**< non-empty local name */
	struct pw_ndarray_filter_format format;
};

/**
 * Callbacks for one standalone ndarray filter.
 *
 * Lifecycle callbacks are serialized and never overlap `process`.
 * `prepare_process_thread` runs on the exact data-loop thread after streaming
 * starts and before the first process call. It may allocate, compile, block,
 * and touch pages. `deactivate` runs after processing has quiesced and is
 * also called after a failed preparation attempt.
 *
 * `process` runs on the PipeWire data loop for Version 0 configurations. It
 * receives every frame-data input and output in direction-local declaration
 * order, excluding Parameter Ports. With Version 1 progressive configuration,
 * the native helper retains one full-frame input and calls `process` once for
 * each committed fixed region on its dedicated worker, or on the pinned
 * filter data loop with PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE. The input data and
 * declared size then describe the exact region format, and Header offset is
 * the region's offset along the split dimension. Only the terminal callback
 * can publish output. The progressive execution thread invokes
 * abort_progressive_frame() after a
 * timeout, source abort, cancellation, or failed process callback. Lifecycle
 * deactivation waits for that thread before releasing the frame.
 * `update_parameter` runs on one owned serial worker and receives the original
 * direction-local input-port index. It may allocate and block while copying or
 * preparing a replacement, but it must not retain the borrowed buffer. A
 * return of -EBUSY retains the Parameter buffer and retries it after a later
 * data-loop cycle; any other negative result terminates the filter. At most
 * one buffer is retained per Parameter Port. Newer buffers that arrive while
 * it is retained are immediately returned to their producer.
 *
 * Callbacks return zero on success or a negative errno-style value and must
 * not let an exception unwind across the callback boundary. Except for the
 * progressive input lifetime above, they must not retain borrowed pointers.
 * A retained progressive pointer must be cleared by the terminal callback or
 * abort_progressive_frame() before either returns. Outputs are published by
 * default. A process callback may
 * independently mark an output unavailable; the helper retains that buffer
 * and presents it again on the next callback.
 */
struct pw_ndarray_filter_events {
	uint32_t version;
	int (*prepare_process_thread)(void *data);
	int (*process)(void *data,
			const struct pw_ndarray_filter_buffer *inputs,
			uint32_t n_inputs,
			struct pw_ndarray_filter_buffer *outputs,
			uint32_t n_outputs);
	int (*deactivate)(void *data);
	int (*update_parameter)(void *data, uint32_t input_port,
			const struct pw_ndarray_filter_buffer *parameter);
	/**
	 * Return PropInfo at index, or -ENOENT after the final declaration.
	 * Every returned POD must remain valid until pw_ndarray_filter_connect()
	 * returns.
	 */
	int (*enum_prop_info)(void *data, uint32_t index,
			const struct spa_pod **info);
	/** Return the owner's current requested and active scalar property state. */
	int (*get_props)(void *data, const struct spa_pod **props);
	/** Validate and stage one scalar property request. */
	int (*set_props)(void *data, const struct spa_pod *props);
	/** Reset processing state while the node is stopped. */
	int (*reset)(void *data);
	/** Prepare the progressive execution thread before it processes frames. */
	int (*prepare_progressive_worker)(void *data);
	/** Discard partial state for one frame, on the progressive execution thread. */
	int (*abort_progressive_frame)(void *data, uint64_t generation,
			uint64_t sequence, int reason);
};

/**
 * Immutable construction configuration. All strings and shapes are copied.
 *
 * Version 1 opts in to one progressive frame-data input. Its ordinary Port
 * format is the distinct full-frame transport schema; region_format is the
 * exact callback format. The two formats must have the same element type,
 * layout, and unsplit dimensions. The callback rate must equal the frame rate
 * multiplied by the number of regions. A row-major frame splits along the
 * first dimension; a column-major frame splits along the last. The transport
 * requires negotiated NdarrayProgress metadata and mapped MemFd data. The
 * native helper rejects a frame if any output buffer is unavailable when the
 * frame arrives. A zero timeout is invalid. CPU -1 permits an explicitly
 * unpinned development worker; set a nonnegative CPU for latency qualification.
 */
struct pw_ndarray_filter_config {
	uint32_t struct_size;
	uint32_t version;
	const char *node_name;
	const char *remote_name;        /**< optional PipeWire remote */
	uint32_t n_ports;
	uint32_t flags;                 /**< mask of enum pw_ndarray_filter_flags */
	const struct pw_ndarray_filter_port *ports;
	const struct pw_ndarray_filter_events *events;
	void *user_data;
	/** Version 1: direction-local index of the progressive frame-data input. */
	uint32_t progressive_input_port;
	/** Version 1: exact fixed-region format presented to process(). */
	struct pw_ndarray_filter_format progressive_region_format;
	/** Version 1: finite interval without a new committed region, in ns. */
	uint64_t progressive_timeout_ns;
	/** Version 1: worker CPU, or -1 for explicitly unpinned development. */
	int32_t progressive_cpu;
};

/** Opaque owner of one main loop, PipeWire filter, and copied declaration. */
struct pw_ndarray_filter;

/** Construct an unconnected filter. Calls pw_init() once on success. */
int pw_ndarray_filter_new(const struct pw_ndarray_filter_config *config,
		struct pw_ndarray_filter **filter);

/** Add the configured node and ports to the selected PipeWire remote. */
int pw_ndarray_filter_connect(struct pw_ndarray_filter *filter);

/** Run the owned main loop until quit or a filter/process error. */
int pw_ndarray_filter_run(struct pw_ndarray_filter *filter);

/** Request main-loop termination. This operation may be called by another thread. */
int pw_ndarray_filter_quit(struct pw_ndarray_filter *filter);

/**
 * Schedule publication of the owner's current Props on the main-loop thread.
 *
 * This operation is non-blocking and may be called from a process callback.
 * Notifications are coalesced. It is valid only with
 * PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES.
 */
int pw_ndarray_filter_notify_properties(struct pw_ndarray_filter *filter);

/** Return the most recently observed state without entering the main loop. */
enum pw_filter_state pw_ndarray_filter_get_state(
		const struct pw_ndarray_filter *filter);

/** Return the first asynchronous lifecycle or process error, or zero. */
int pw_ndarray_filter_get_error(const struct pw_ndarray_filter *filter);

/** Return the PipeWire node id, or SPA_ID_INVALID before registration. */
uint32_t pw_ndarray_filter_get_node_id(const struct pw_ndarray_filter *filter);

/**
 * Disconnect and destroy the filter, then pair its successful pw_init().
 *
 * The caller must ensure `run` has returned and must destroy on the same
 * thread that constructed and ran the main loop. Passing NULL is allowed.
 */
void pw_ndarray_filter_destroy(struct pw_ndarray_filter *filter);

/** \} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIPEWIRE_NDARRAY_FILTER_H */
