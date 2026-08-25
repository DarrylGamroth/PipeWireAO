/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H
#define SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include <spa/buffer/buffer.h>
#include <spa/param/format.h>
#include <spa/param/ndarray.h>
#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_filter_graph
 * \{
 */

/** Version of the C ABI exported by ndarray operation shared libraries. */
#define SPA_FGN_PLUGIN_ABI_VERSION 1u

/** Symbol exported by an ndarray operation shared library. */
#define SPA_FGN_PLUGIN_ENTRY_NAME "spa_filter_graph_ndarray_plugin_get_interface"

/** Direction of an operation port. */
enum spa_fgn_port_direction {
	SPA_FGN_PORT_INPUT,
	SPA_FGN_PORT_OUTPUT,
};

/** Descriptor flags for an operation port. */
enum spa_fgn_port_flag {
	SPA_FGN_PORT_FLAG_NONE = 0,
	SPA_FGN_PORT_FLAG_OPTIONAL = (1u << 0),
	/** Sparse, retained plugin state supplied outside the data-loop process. */
	SPA_FGN_PORT_FLAG_PARAMETER = (1u << 1),
};

/**
 * One exact packed ndarray format.
 *
 * All pointed-to data is owned by the plugin and remains valid for the
 * lifetime of the operation instance. Integer fields are used instead of C
 * enum fields to make the ABI straightforward to implement in Rust.
 */
struct spa_fgn_format {
	uint32_t element_type;       /**< one of enum spa_element_type */
	uint32_t layout;             /**< one of enum spa_ndarray_layout */
	uint32_t rate_num;           /**< zero with rate_denom zero when absent */
	uint32_t rate_denom;
	uint32_t n_dimensions;
	const uint32_t *shape;
	const char *schema;          /**< optional semantic schema */
	const char *profile;         /**< optional interpretation profile */
};

/** Static description of one operation port. */
struct spa_fgn_port_info {
	uint32_t struct_size;
	uint32_t index;
	uint32_t direction;          /**< one of enum spa_fgn_port_direction */
	uint32_t flags;              /**< a mask of enum spa_fgn_port_flag */
	const char *name;
};

/** Property flags. */
enum spa_fgn_property_flag {
	SPA_FGN_PROPERTY_FLAG_NONE = 0,
	SPA_FGN_PROPERTY_FLAG_READONLY = (1u << 0),
	SPA_FGN_PROPERTY_FLAG_RANGE = (1u << 1),
};

/**
 * A scalar property value.
 *
 * type is one of SPA_TYPE_Bool, SPA_TYPE_Int, SPA_TYPE_Long, SPA_TYPE_Float,
 * SPA_TYPE_Double, SPA_TYPE_Id, or SPA_TYPE_String. String storage is borrowed
 * for the duration documented by the callback receiving or returning it.
 */
struct spa_fgn_value {
	uint32_t type;
	uint32_t reserved;
	union {
		int32_t boolean;
		int32_t integer;
		int64_t long_integer;
		float float_value;
		double double_value;
		uint32_t id;
		const char *string;
	} value;
};

/** Description of one instance property. */
struct spa_fgn_property_info {
	uint32_t struct_size;
	uint32_t id;
	uint32_t flags;              /**< a mask of enum spa_fgn_property_flag */
	uint32_t reserved;
	const char *name;
	const char *description;
	struct spa_fgn_value default_value;
	struct spa_fgn_value minimum;
	struct spa_fgn_value maximum;
};

/** One property assignment in a transaction. */
struct spa_fgn_property {
	uint32_t id;
	uint32_t reserved;
	struct spa_fgn_value value;
};

/** Buffer and negotiated format supplied to an operation callback. */
struct spa_fgn_buffer {
	struct spa_buffer *buffer;
	const struct spa_fgn_format *format;
};

struct spa_fgn_descriptor;

/**
 * Descriptor for one operation implementation.
 *
 * Descriptors and port arrays have static shared-library lifetime. Instances
 * and every object reachable through them are owned by the plugin.
 */
struct spa_fgn_descriptor {
	uint32_t struct_size;
	uint32_t version;
	const char *name;
	uint32_t n_ports;
	const struct spa_fgn_port_info *ports;

	int (*instantiate)(const struct spa_fgn_descriptor *descriptor,
			const char *config, void **instance);
	void (*cleanup)(void *instance);

	int (*get_port_format)(void *instance, uint32_t port,
			const struct spa_fgn_format **format);

	int (*enum_prop_info)(void *instance, uint32_t index,
			struct spa_fgn_property_info *info);
	int (*get_prop)(void *instance, uint32_t id,
			struct spa_fgn_value *value);

	/** Prepare and validate a complete local transaction off the data loop. */
	int (*prepare_props)(void *instance, const struct spa_fgn_property *props,
			uint32_t n_props, void **prepared);
	/** Publish a previously prepared transaction. This callback cannot fail. */
	void (*commit_props)(void *instance, void *prepared);
	/** Release a prepared transaction that will not be committed. */
	void (*discard_props)(void *instance, void *prepared);

	/**
	 * Prepare one sparse ndarray parameter update off the data loop.
	 *
	 * The input buffer is borrowed only for this call. Implementations copy or
	 * compile everything needed by process() into bounded plugin-owned state.
	 * Returning -EBUSY applies back pressure when no inactive state slot is
	 * available.
	 */
	int (*prepare_parameter)(void *instance, uint32_t port,
			const struct spa_fgn_buffer *buffer, void **prepared);
	/** Publish a prepared parameter update. This callback cannot fail. */
	void (*commit_parameter)(void *instance, void *prepared);
	/** Release a parameter update that will not be committed. */
	void (*discard_parameter)(void *instance, void *prepared);

	int (*activate)(void *instance);
	int (*deactivate)(void *instance);
	int (*reset)(void *instance);

	/**
	 * Process exactly one complete buffer on every connected port.
	 *
	 * This callback runs on the graph data loop. It must not allocate, block,
	 * unwind across the ABI, or retain a buffer after returning. Input buffers
	 * and their metadata are read-only, including when one output fans out to
	 * multiple consumers. Parameter-port entries are present in the input
	 * array with a NULL buffer; process() uses the plugin-owned active state
	 * previously published by commit_parameter().
	 */
	int (*process)(void *instance,
			const struct spa_fgn_buffer *inputs, uint32_t n_inputs,
			struct spa_fgn_buffer *outputs, uint32_t n_outputs);
};

/** Interface returned by one shared library. */
struct spa_fgn_plugin {
	uint32_t struct_size;
	uint32_t abi_version;
	const char *name;
	const struct spa_fgn_descriptor *(*find_descriptor)(const char *name);
};

typedef const struct spa_fgn_plugin *
(*spa_fgn_plugin_entry_func_t)(uint32_t abi_version);

/** \} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H */
