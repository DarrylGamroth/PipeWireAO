/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H
#define SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H

#include <errno.h>
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

/** Version of the C ABI exported by ndarray plugin libraries. */
#define SPA_FGN_PLUGIN_ABI_VERSION 6u
#define SPA_FGN_MAX_METAS 16u
#define SPA_FGN_MAX_META_BYTES 4096u
#define SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS 4096u

/** Symbol exported by an ndarray plugin library. */
#define SPA_FGN_PLUGIN_ENTRY_NAME "spa_filter_graph_ndarray_plugin_get_interface"

/** Maximum UTF-8 byte length of a descriptor, port, or property local name. */
#define SPA_FGN_LOCAL_NAME_MAX 255u

/** Shared-library lifetime requested by a plugin registry. */
enum spa_fgn_plugin_flag {
	SPA_FGN_PLUGIN_FLAG_NONE = 0,
	/**
	 * Keep the library mapped until process termination.
	 *
	 * This supports language runtimes whose immutable descriptor registry has
	 * process lifetime and cannot be destroyed safely by dlclose(). The plugin
	 * MUST bound retained state per mapped library rather than per instance.
	 */
	SPA_FGN_PLUGIN_FLAG_RETAIN_LIBRARY = (1u << 0),
};

/** Descriptor flags for a plugin port. */
enum spa_fgn_port_flag {
	SPA_FGN_PORT_FLAG_NONE = 0,
	SPA_FGN_PORT_FLAG_OPTIONAL = (1u << 0),
	/** Sparse, retained plugin state supplied outside the data-loop process. */
	SPA_FGN_PORT_FLAG_PARAMETER = (1u << 1),
	/**
	 * An output may intentionally produce no artifact in one process call.
	 *
	 * The plugin leaves the output chunk size zero for that call. The graph
	 * skips consumers whose required input is absent and propagates absence to
	 * their outputs without invoking them. A later call may complete the
	 * output using the same unpublished caller-owned buffer.
	 */
	SPA_FGN_PORT_FLAG_CONDITIONAL = (1u << 2),
};

/**
 * One exact packed ndarray format.
 *
 * All pointed-to data is owned by the plugin and remains valid for the
 * lifetime of the plugin instance. Integer fields are used instead of C
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
};

/** Static description of one plugin port. */
struct spa_fgn_port_info {
	uint32_t struct_size;
	uint32_t index;
	uint32_t direction;          /**< one of enum spa_direction */
	uint32_t flags;              /**< a mask of enum spa_fgn_port_flag */
	/** Non-empty; at most SPA_FGN_LOCAL_NAME_MAX bytes. */
	const char *name;
};

/**
 * Property flags.
 *
 * Every descriptor sets exactly one of READONLY, RUNTIME, CONSTRUCTION, or
 * GRAPH_REBUILD. RANGE and CHOICES are mutually exclusive value constraints.
 */
enum spa_fgn_property_flag {
	SPA_FGN_PROPERTY_FLAG_NONE = 0,
	/** The property is reported but cannot be assigned. */
	SPA_FGN_PROPERTY_FLAG_READONLY = (1u << 0),
	SPA_FGN_PROPERTY_FLAG_RANGE = (1u << 1),
	/** The property can be adopted at a process boundary. */
	SPA_FGN_PROPERTY_FLAG_RUNTIME = (1u << 2),
	/** The property is accepted only during instance construction. */
	SPA_FGN_PROPERTY_FLAG_CONSTRUCTION = (1u << 3),
	/** Changing the property requires graph reconstruction. */
	SPA_FGN_PROPERTY_FLAG_GRAPH_REBUILD = (1u << 4),
	SPA_FGN_PROPERTY_FLAG_CHOICES = (1u << 5),
};

/**
 * A scalar property value.
 *
 * type is one of SPA_TYPE_Bool, SPA_TYPE_Int, SPA_TYPE_Long, SPA_TYPE_Float,
 * SPA_TYPE_Double, SPA_TYPE_Id, or SPA_TYPE_String. An input string passed to
 * prepare_props() is borrowed only for that call. A string returned by
 * get_prop() remains valid through callback return and the host's immediate
 * copy into a POD.
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

/** One admitted value and label for an enumerated scalar property. */
struct spa_fgn_property_choice {
	uint32_t struct_size;
	uint32_t reserved;
	struct spa_fgn_value value;
	const char *name;
	const char *description;
};

/** Description of one instance property. */
struct spa_fgn_property_info {
	uint32_t struct_size;
	uint32_t id;
	uint32_t flags;              /**< a mask of enum spa_fgn_property_flag */
	uint32_t reserved;
	/** Non-empty; at most SPA_FGN_LOCAL_NAME_MAX bytes. */
	const char *name;
	const char *description;
	const char *unit;             /**< non-empty canonical unit; "1" is dimensionless */
	struct spa_fgn_value default_value;
	struct spa_fgn_value minimum;
	struct spa_fgn_value maximum;
	uint32_t n_choices;
	uint32_t choices_reserved;
	const struct spa_fgn_property_choice *choices;
};

/** Static-initializer helpers for declarative C property tables. */
#define SPA_FGN_VALUE_NONE_INIT						\
	{ 0 }
#define SPA_FGN_VALUE_BOOL_INIT(value_)					\
	{ .type = SPA_TYPE_Bool, .value.boolean = (value_) }
#define SPA_FGN_VALUE_INT_INIT(value_)					\
	{ .type = SPA_TYPE_Int, .value.integer = (value_) }
#define SPA_FGN_VALUE_LONG_INIT(value_)					\
	{ .type = SPA_TYPE_Long, .value.long_integer = (value_) }
#define SPA_FGN_VALUE_FLOAT_INIT(value_)					\
	{ .type = SPA_TYPE_Float, .value.float_value = (value_) }
#define SPA_FGN_VALUE_DOUBLE_INIT(value_)					\
	{ .type = SPA_TYPE_Double, .value.double_value = (value_) }
#define SPA_FGN_VALUE_ID_INIT(value_)					\
	{ .type = SPA_TYPE_Id, .value.id = (value_) }
#define SPA_FGN_VALUE_STRING_INIT(value_)					\
	{ .type = SPA_TYPE_String, .value.string = (value_) }

#define SPA_FGN_PROPERTY_INFO_INIT(id_, flags_, name_, description_, unit_, \
		default_, minimum_, maximum_, n_choices_, choices_)		\
	{									\
		.struct_size = sizeof(struct spa_fgn_property_info),		\
		.id = (id_),							\
		.flags = (flags_),						\
		.name = (name_),						\
		.description = (description_),					\
		.unit = (unit_),						\
		.default_value = default_,					\
		.minimum = minimum_,						\
		.maximum = maximum_,						\
		.n_choices = (n_choices_),					\
		.choices = (choices_),						\
	}

/** Enumerate one descriptor from a static C property table. */
static inline int spa_fgn_enum_prop_info_table(
		const struct spa_fgn_property_info *properties,
		uint32_t n_properties, uint32_t index,
		struct spa_fgn_property_info *info)
{
	if (info == NULL || (n_properties > 0 && properties == NULL))
		return -EINVAL;
	if (index >= n_properties)
		return 0;
	*info = properties[index];
	return 1;
}

/** One property assignment in a transaction. */
struct spa_fgn_property {
	uint32_t id;
	uint32_t reserved;
	struct spa_fgn_value value;
};

/** Buffer and negotiated format supplied to a plugin-instance callback. */
struct spa_fgn_buffer {
	struct spa_buffer *buffer;
	const struct spa_fgn_format *format;
};

/** One sparse parameter assignment in a local prepared-plan transaction. */
struct spa_fgn_parameter {
	uint32_t port;
	uint32_t reserved;
	struct spa_fgn_buffer buffer;
};

struct spa_fgn_descriptor;

/**
 * Descriptor for one ndarray filter-graph plugin.
 *
 * Descriptors and port arrays have static shared-library lifetime. Formats,
 * shapes, property descriptors, choices, and descriptor strings returned for
 * an instance remain valid until cleanup. Instances and every object reachable
 * through them are owned by the plugin.
 *
 * instantiate(), admission enumeration, activate(), deactivate(), reset(), and
 * cleanup() are serial and do not overlap process(). Control callbacks are
 * serial with one another but get_prop(), prepare_props(), discard_props(), and
 * parameter preparation or discard may overlap process(). A legacy
 * commit_parameter() may run on that serial owner and overlap process(); a
 * graph-transaction commit and adopt_parameters() run on the data-loop owner
 * before any node process callback in that cycle. The host never locks
 * process(); plugins use an explicit lock-free publication protocol for shared
 * state.
 */
struct spa_fgn_descriptor {
	uint32_t struct_size;
	uint32_t version;
	/** Non-empty; at most SPA_FGN_LOCAL_NAME_MAX bytes. */
	const char *name;
	uint32_t n_ports;
	const struct spa_fgn_port_info *ports;

	/**
	 * Construct an instance from one standard JSON object.
	 *
	 * The graph host canonicalizes PipeWire's relaxed SPA syntax before this
	 * callback. The string is borrowed for this call only.
	 */
	int (*instantiate)(const struct spa_fgn_descriptor *descriptor,
			const char *config, void **instance);
	void (*cleanup)(void *instance);

	int (*get_port_format)(void *instance, uint32_t port,
			const struct spa_fgn_format **format);

	int (*enum_prop_info)(void *instance, uint32_t index,
			struct spa_fgn_property_info *info);
	int (*get_prop)(void *instance, uint32_t id,
			struct spa_fgn_value *value);
	/**
	 * Return a monotonic sequence revision for externally visible values.
	 *
	 * Stable snapshots use even revisions. The value remains odd while one or
	 * more writers change property-visible state and advances to a greater even
	 * value only after the final writer completes. The graph retries a control
	 * snapshot that sees an odd or changed revision and reports process-time
	 * changes to its caller.
	 * The callback satisfies the same real-time restrictions as process().
	 */
	uint64_t (*get_prop_revision)(void *instance);

	/**
	 * Prepare and validate a complete local transaction off the data loop.
	 *
	 * Property values and strings are borrowed only for this call. The returned
	 * object is self-contained and owned by the plugin.
	 */
	int (*prepare_props)(void *instance, const struct spa_fgn_property *props,
			uint32_t n_props, void **prepared);
	/**
	 * Publish a previously prepared transaction at graph-cycle start.
	 *
	 * This callback consumes prepared and cannot fail, allocate, block, destroy
	 * retired state, or unwind.
	 */
	void (*commit_props)(void *instance, void *prepared);
	/** Consume an unpublished prepared transaction on the control path. */
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
	/**
	 * Failure-atomically prepare one nonempty local parameter transaction.
	 *
	 * Ports are unique parameter inputs on this instance. Buffers and formats
	 * are borrowed only for this call. The returned token represents one
	 * complete replacement and is consumed by commit_parameter() or
	 * discard_parameter().
	 */
	int (*prepare_parameters)(void *instance,
			const struct spa_fgn_parameter *parameters,
			uint32_t n_parameters, void **prepared);
	/**
	 * Adopt every completely published parameter replacement at graph-cycle
	 * start. This callback cannot fail, allocate, block, destroy retired state,
	 * or unwind.
	 */
	void (*adopt_parameters)(void *instance);

	int (*activate)(void *instance);
	int (*deactivate)(void *instance);
	int (*reset)(void *instance);

	/**
	 * Process one complete buffer on every connected input port.
	 *
	 * This callback runs on the graph data loop. It must not allocate, block,
	 * unwind across the ABI, or retain a buffer after returning. Input buffers
	 * and their metadata are read-only, including when one output fans out to
	 * multiple consumers. Input and output buffer descriptors, chunks,
	 * metadata, and data regions do not overlap. Each buffer has exactly one
	 * data plane and at most SPA_FGN_MAX_METAS entries containing at most
	 * SPA_FGN_MAX_META_BYTES in total. A plugin instance writes at the supplied
	 * output chunk offset, preserves that offset, and sets the completed size
	 * only after successful processing. It leaves the completed size zero on
	 * failure. An output declared with SPA_FGN_PORT_FLAG_CONDITIONAL may also
	 * leave its completed size zero on success to defer publication. A
	 * non-conditional output must be complete whenever this callback runs.
	 * Parameter-port entries are present in the input
	 * array with a NULL buffer; process() uses the plugin-owned active state
	 * previously published by commit_parameter().
	 * The inputs or outputs array may be NULL exactly when its count is zero.
	 *
	 * The graph host validates these structural, extent, alignment, and aliasing
	 * conditions before entering a plugin. A direct C caller that bypasses the
	 * graph host must provide accessible, correctly aligned outer arrays and
	 * nested pointers satisfying the same contract; an arbitrary inaccessible
	 * pointer cannot be validated portably by the callee.
	 */
	int (*process)(void *instance,
			const struct spa_fgn_buffer *inputs, uint32_t n_inputs,
			struct spa_fgn_buffer *outputs, uint32_t n_outputs);
};

/** Interface returned by one shared library. */
struct spa_fgn_plugin {
	uint32_t struct_size;
	uint32_t abi_version;
	const char *name;              /**< non-empty plugin family name */
	const struct spa_fgn_descriptor *(*find_descriptor)(const char *name);
	uint32_t flags;                /**< enum spa_fgn_plugin_flag */
};

typedef const struct spa_fgn_plugin *
(*spa_fgn_plugin_entry_func_t)(uint32_t abi_version);

/** \} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_FILTER_GRAPH_NDARRAY_PLUGIN_H */
