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

#define PW_VERSION_NDARRAY_FILTER_EVENTS 0u
#define PW_VERSION_NDARRAY_FILTER_CONFIG 0u
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
};

/**
 * One borrowed packed ndarray supplied to a process callback.
 *
 * `data` is borrowed only until the callback returns. An input payload is
 * read-only even though the common C layout uses `void *`; an output payload
 * is exclusively writable. `size` is the exact declared payload size and
 * `capacity` is the mapped capacity beginning at `data`.
 *
 * `metadata_available` reports which destination metadata records exist for
 * an output and which records were present and valid for an input. A callback
 * sets `metadata_valid` on outputs after filling the corresponding by-value
 * record. It may set `PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE` on an
 * output to retain that buffer without publishing it. It must not change any
 * other structural field.
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
	uint32_t flags;                 /**< reserved; zero */
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
 * and touch pages. `deactivate` runs after the data loop has stopped and is
 * also called after a failed preparation attempt.
 *
 * `process` runs on the PipeWire data loop. It receives every input and output
 * in direction-local declaration order. It returns zero on success or a
 * negative errno-style value. The optional lifecycle callbacks use the same
 * return convention. Callbacks must not retain borrowed pointers or let an
 * exception unwind across the callback boundary. Outputs are published by
 * default. A process callback may independently mark an output unavailable;
 * the helper retains that buffer and presents it again on the next callback.
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
};

/** Immutable construction configuration. All strings and shapes are copied. */
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
