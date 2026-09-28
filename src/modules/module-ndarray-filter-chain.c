/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spa/debug/filter-graph-ndarray.h>
#include <spa/debug/log.h>
#include <spa/filter-graph/filter-graph-ndarray.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/filter.h>
#include <pipewire/impl.h>
#include <pipewire/run-control.h>

#include "module-ndarray-filter-chain-parameter.h"

#define NAME "ndarray-filter-chain"
#define STARTUP_PARAMETER_PREFIX "pipewireao.startup-parameter."
#define FEEDBACK_PREFIX "pipewireao.feedback."

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/** \page page_module_ndarray_filter_chain Ndarray Filter Chain
 *
 * Expose a synchronous ndarray filter graph as one native PipeWire node.
 * The module accepts standard PipeWire configuration syntax:
 *
 * \code{.unparsed}
 * pw-cli load-module libpipewire-module-ndarray-filter-chain '{
 *   node.name = ao-composite
 *   pipewireao.run-control = true
 *   filter.graph = {
 *     nodes = [
 *       { type=ndarray name=algorithm plugin=/path/libcalculon.so label=step }
 *     ]
 *   }
 * }'
 * \endcode
 *
 * External graph ports become ordinary application/ndarray ports. Sparse
 * parameter ports are marked as control ports and use a bounded worker
 * handoff; their preparation never runs in the real-time process callback.
 * A startup-only F32 artifact can be assigned with
 * `pipewireao.startup-parameter.<node>:<port>=/absolute/artifact.f32`.
 * The module resolves and validates every such assignment before activation,
 * then prepares them as one graph transaction.  Mapped artifacts remain
 * available until graph teardown.
 * `pipewireao.feedback.<input-node>:<input-port>=<output-node>:<output-port>`
 * retains an output for the next graph cycle. Both graph endpoints are private
 * to this module and must have identical F32 ndarray formats. The first graph
 * cycle receives zeros; a successful graph reset clears the retained output.
 * `pipewireao.fifo-inputs=true` consumes the oldest input buffer and retains
 * it until all required buffers are available for the graph cycle.
 * The module logs the resolved execution order, formats, connections, and
 * graph-owned buffer sizes once at debug log level during construction.
 * With pipewireao.run-control=true, the node connects inactive and accepts
 * Version 1 owner-mediated start and stop requests through SPA_PARAM_Props.
 */

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "PipeWireAO contributors" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a composite ndarray filter node" },
	{ PW_KEY_MODULE_USAGE, "filter.graph=<graph> (node.name=<name>) "
		"(pipewireao.run-control=<bool>) (pipewireao.fifo-inputs=<bool>)" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl;

struct port {
	struct impl *impl;
	uint32_t index;
	enum spa_direction direction;
	uint32_t flags;
	struct ndarray_parameter_handoff parameter_handoff;
	_Atomic uint64_t dropped_parameters;
};

struct startup_parameter {
	void *mapping;
	size_t size;
	struct spa_chunk chunk;
	struct spa_data data;
	struct spa_buffer buffer;
};

struct feedback_buffer {
	void *memory;
	size_t size;
	struct spa_chunk chunk;
	struct spa_data data;
	struct spa_meta_header header;
	struct spa_meta meta;
	struct spa_buffer buffer;
};

struct feedback_bridge {
	uint32_t input_port;
	uint32_t output_port;
	struct feedback_buffer input;
	struct feedback_buffer output;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	bool disconnect_core;
	bool core_listeners_added;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_filter *filter;
	struct spa_hook filter_listener;
	struct pw_loop *main_loop;
	struct spa_source *main_event;
	struct pw_thread_loop *parameter_loop;
	struct spa_source *parameter_event;
	bool parameter_loop_started;
	pthread_mutex_t control_lock;
	bool control_lock_initialized;
	_Atomic bool destroying;
	_Atomic int process_error;
	_Atomic bool process_failed;
	_Atomic bool publish_after_process;
	bool run_control;
	bool reset_control;
	bool fifo_inputs;
	int64_t last_request_token;
	int64_t completed_token;
	int32_t run_control_result;
	enum pw_ao_run_control_state requested_state;
	enum pw_ao_run_control_state actual_state;
	int64_t last_reset_token;
	int64_t completed_reset_token;
	int32_t reset_result;
	enum pw_filter_state filter_state;

	struct spa_fgn_graph *graph;
	struct startup_parameter *startup_parameters;
	uint32_t n_startup_parameters;
	struct feedback_bridge *feedback_bridges;
	uint32_t n_feedback_bridges;
	struct port **inputs;
	struct port **outputs;
	uint32_t n_inputs;
	uint32_t n_outputs;
	struct spa_buffer **process_inputs;
	struct spa_buffer **process_outputs;
	struct pw_buffer **input_buffers;
	struct pw_buffer **output_buffers;
};

static void log_graph_report(const struct spa_fgn_graph *graph)
{
	struct spa_debug_log_ctx context;

	if (!pw_log_topic_enabled(SPA_LOG_LEVEL_DEBUG, PW_LOG_TOPIC_DEFAULT))
		return;
	context = SPA_LOGT_DEBUG_INIT(pw_log_get(), SPA_LOG_LEVEL_DEBUG,
			PW_LOG_TOPIC_DEFAULT);
	(void)spa_debugc_fgn_graph(&context.ctx, 0, graph);
}

static int fgn_format_size(const struct spa_fgn_format *format,
		size_t *size, int32_t *stride)
{
	size_t elements = 1, bytes, contiguous;
	uint32_t element_size, axis, i;

	if (format == NULL || size == NULL || stride == NULL ||
	    format->shape == NULL || format->n_dimensions == 0 ||
	    format->n_dimensions > SPA_NDARRAY_MAX_DIMENSIONS ||
	    (format->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR &&
	     format->layout != SPA_NDARRAY_LAYOUT_COLUMN_MAJOR) ||
	    (format->rate_num == 0) != (format->rate_denom == 0) ||
	    (element_size = spa_element_type_size(format->element_type)) == 0)
		return -EINVAL;
	for (i = 0; i < format->n_dimensions; i++) {
		if (format->shape[i] == 0 || format->shape[i] > INT32_MAX)
			return -EINVAL;
		if (elements > SIZE_MAX / format->shape[i])
			return -EOVERFLOW;
		elements *= format->shape[i];
	}
	if (elements > SIZE_MAX / element_size)
		return -EOVERFLOW;
	bytes = elements * element_size;
	axis = format->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR
		? format->n_dimensions - 1 : 0;
	contiguous = (size_t)format->shape[axis] * element_size;
	if (bytes > INT32_MAX || contiguous > INT32_MAX)
		return -EOVERFLOW;
	*size = bytes;
	*stride = (int32_t)contiguous;
	return 0;
}

static void clear_startup_parameters(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_startup_parameters; i++)
		if (impl->startup_parameters[i].mapping != NULL)
			munmap(impl->startup_parameters[i].mapping,
					impl->startup_parameters[i].size);
	free(impl->startup_parameters);
	impl->startup_parameters = NULL;
	impl->n_startup_parameters = 0;
}

static int find_startup_parameter_port(struct impl *impl,
		const char *qualified_name, uint32_t *input_port)
{
	uint32_t i;

	for (i = 0; i < impl->n_inputs; i++) {
		const struct spa_fgn_port_info *info;
		const char *node_name;
		char name[512];
		int res, length;

		if ((res = spa_fgn_graph_get_port_info(impl->graph,
				SPA_DIRECTION_INPUT, i, &node_name, &info)) < 0)
			return res;
		length = spa_scnprintf(name, sizeof(name), "%s:%s", node_name,
				info->name);
		if (length < 0 || (size_t)length >= sizeof(name))
			return -ENOSPC;
		if (spa_streq(name, qualified_name)) {
			if (!(info->flags & SPA_FGN_PORT_FLAG_PARAMETER))
				return -EINVAL;
			*input_port = i;
			return 0;
		}
	}
	return -ENOENT;
}

static int configure_startup_parameters(struct impl *impl,
		const struct pw_properties *properties)
{
	const struct spa_dict_item *item;
	struct spa_fgn_parameter_update *updates = NULL;
	bool *assigned = NULL;
	uint32_t n_parameters = 0, i = 0;
	int res = 0;

	spa_dict_for_each(item, &properties->dict)
		if (spa_strstartswith(item->key, STARTUP_PARAMETER_PREFIX))
			n_parameters++;
	if (n_parameters == 0)
		return 0;
	if (n_parameters > SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS)
		return -E2BIG;
	if ((impl->startup_parameters = calloc(n_parameters,
				sizeof(*impl->startup_parameters))) == NULL ||
	    (updates = calloc(n_parameters, sizeof(*updates))) == NULL ||
	    (assigned = calloc(impl->n_inputs, sizeof(*assigned))) == NULL) {
		res = -ENOMEM;
		goto error;
	}
	impl->n_startup_parameters = n_parameters;

	spa_dict_for_each(item, &properties->dict) {
		struct startup_parameter *parameter;
		const struct spa_fgn_format *format;
		const char *qualified_name;
		struct stat st;
		size_t size;
		int32_t stride;
		uint32_t input_port;
		int fd;

		if (!spa_strstartswith(item->key, STARTUP_PARAMETER_PREFIX))
			continue;
		qualified_name = item->key + strlen(STARTUP_PARAMETER_PREFIX);
		if (*qualified_name == '\0' || item->value == NULL || *item->value == '\0') {
			pw_log_error("invalid empty startup parameter assignment");
			res = -EINVAL;
			goto error;
		}
		if ((res = find_startup_parameter_port(impl, qualified_name,
				&input_port)) < 0) {
			pw_log_error("startup parameter '%s' is not an external parameter input",
					qualified_name);
			goto error;
		}
		if (assigned[input_port]) {
			pw_log_error("duplicate startup parameter '%s'", qualified_name);
			res = -EEXIST;
			goto error;
		}
		if ((res = spa_fgn_graph_get_port_format(impl->graph,
				SPA_DIRECTION_INPUT, input_port, &format)) < 0 ||
		    (res = fgn_format_size(format, &size, &stride)) < 0)
			goto error;
		if (format->element_type != SPA_ELEMENT_TYPE_F32_LE ||
		    format->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
		    size % _Alignof(float) != 0) {
			pw_log_error("startup parameter '%s' requires F32_LE row-major data",
					qualified_name);
			res = -EINVAL;
			goto error;
		}
		fd = open(item->value, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0) {
			res = -errno;
			pw_log_error("can't open startup parameter '%s': %s", qualified_name,
					spa_strerror(res));
			goto error;
		}
		if (fstat(fd, &st) < 0) {
			res = -errno;
			close(fd);
			goto error;
		}
		if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
		    (uintmax_t)st.st_size != (uintmax_t)size) {
			close(fd);
			pw_log_error("startup parameter '%s' has invalid byte count", qualified_name);
			res = -EMSGSIZE;
			goto error;
		}
		parameter = &impl->startup_parameters[i];
		parameter->mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (parameter->mapping == MAP_FAILED) {
			parameter->mapping = NULL;
			res = -errno;
			goto error;
		}
		parameter->size = size;
		parameter->chunk = (struct spa_chunk) {
			.offset = 0,
			.size = (uint32_t)size,
			.stride = stride,
		};
		parameter->data = (struct spa_data) {
			.type = SPA_DATA_MemPtr,
			.flags = SPA_DATA_FLAG_READABLE,
			.fd = -1,
			.maxsize = (uint32_t)size,
			.data = parameter->mapping,
			.chunk = &parameter->chunk,
		};
		parameter->buffer = (struct spa_buffer) {
			.n_datas = 1,
			.datas = &parameter->data,
		};
		updates[i] = (struct spa_fgn_parameter_update) {
			.input_port = input_port,
			.buffer = &parameter->buffer,
		};
		assigned[input_port] = true;
		i++;
	}
	res = spa_fgn_graph_set_parameters(impl->graph, updates, n_parameters);
	if (res < 0)
		pw_log_error("can't prepare startup parameter transaction: %s",
				spa_strerror(res));
	else
		pw_log_info("prepared %u startup parameter artifacts", n_parameters);
error:
	free(assigned);
	free(updates);
	if (res < 0)
		clear_startup_parameters(impl);
	return res;
}

static int find_graph_port(struct impl *impl, enum spa_direction direction,
		const char *qualified_name, uint32_t *index,
		const struct spa_fgn_port_info **info)
{
	uint32_t n_ports = direction == SPA_DIRECTION_INPUT
		? impl->n_inputs : impl->n_outputs;
	uint32_t i;

	for (i = 0; i < n_ports; i++) {
		const struct spa_fgn_port_info *port_info;
		const char *node_name;
		char name[512];
		int res, length;

		if ((res = spa_fgn_graph_get_port_info(impl->graph, direction, i,
				&node_name, &port_info)) < 0)
			return res;
		length = spa_scnprintf(name, sizeof(name), "%s:%s", node_name,
				port_info->name);
		if (length < 0 || (size_t)length >= sizeof(name))
			return -ENOSPC;
		if (spa_streq(name, qualified_name)) {
			*index = i;
			*info = port_info;
			return 0;
		}
	}
	return -ENOENT;
}

static bool feedback_format_equal(const struct spa_fgn_format *input,
		const struct spa_fgn_format *output)
{
	uint32_t i;

	if (input->element_type != SPA_ELEMENT_TYPE_F32_LE ||
	    output->element_type != SPA_ELEMENT_TYPE_F32_LE ||
	    input->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
	    output->layout != input->layout ||
	    output->rate_num != input->rate_num ||
	    output->rate_denom != input->rate_denom ||
	    output->n_dimensions != input->n_dimensions ||
	    input->schema == NULL || output->schema == NULL ||
	    !spa_streq(input->schema, output->schema))
		return false;
	for (i = 0; i < input->n_dimensions; i++)
		if (input->shape[i] != output->shape[i])
			return false;
	return true;
}

static int init_feedback_buffer(struct feedback_buffer *buffer,
		size_t size, int32_t stride, bool input)
{
	if ((buffer->memory = calloc(1, size)) == NULL)
		return -ENOMEM;
	buffer->size = size;
	buffer->chunk = (struct spa_chunk) {
		.offset = 0,
		.size = input ? (uint32_t)size : 0,
		.stride = stride,
	};
	buffer->data = (struct spa_data) {
		.type = SPA_DATA_MemPtr,
		.flags = SPA_DATA_FLAG_READWRITE,
		.fd = -1,
		.maxsize = (uint32_t)size,
		.data = buffer->memory,
		.chunk = &buffer->chunk,
	};
	buffer->header.pts = SPA_TIME_INVALID;
	buffer->meta = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(buffer->header),
		.data = &buffer->header,
	};
	buffer->buffer = (struct spa_buffer) {
		.n_metas = 1,
		.metas = &buffer->meta,
		.n_datas = 1,
		.datas = &buffer->data,
	};
	return 0;
}

static void clear_feedback_bridges(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_feedback_bridges; i++) {
		free(impl->feedback_bridges[i].input.memory);
		free(impl->feedback_bridges[i].output.memory);
	}
	free(impl->feedback_bridges);
	impl->feedback_bridges = NULL;
	impl->n_feedback_bridges = 0;
}

static int configure_feedback_bridges(struct impl *impl,
		const struct pw_properties *properties)
{
	const struct spa_dict_item *item;
	uint32_t count = 0, index = 0;
	int res = 0;

	spa_dict_for_each(item, &properties->dict)
		if (spa_strstartswith(item->key, FEEDBACK_PREFIX))
			count++;
	if (count == 0)
		return 0;
	if (count > impl->n_inputs || count > impl->n_outputs)
		return -E2BIG;
	if ((impl->feedback_bridges = calloc(count,
				sizeof(*impl->feedback_bridges))) == NULL)
		return -ENOMEM;
	impl->n_feedback_bridges = count;

	spa_dict_for_each(item, &properties->dict) {
		struct feedback_bridge *bridge;
		const struct spa_fgn_port_info *input_info, *output_info;
		const struct spa_fgn_format *input_format, *output_format;
		const char *input_name;
		size_t input_size, output_size;
		int32_t input_stride, output_stride;
		uint32_t previous;

		if (!spa_strstartswith(item->key, FEEDBACK_PREFIX))
			continue;
		input_name = item->key + strlen(FEEDBACK_PREFIX);
		if (*input_name == '\0' || item->value == NULL || *item->value == '\0') {
			res = -EINVAL;
			goto error;
		}
		bridge = &impl->feedback_bridges[index];
		if ((res = find_graph_port(impl, SPA_DIRECTION_INPUT, input_name,
				&bridge->input_port, &input_info)) < 0 ||
		    (res = find_graph_port(impl, SPA_DIRECTION_OUTPUT, item->value,
				&bridge->output_port, &output_info)) < 0)
			goto error;
		if ((input_info->flags & SPA_FGN_PORT_FLAG_PARAMETER) ||
		    (output_info->flags & SPA_FGN_PORT_FLAG_CONDITIONAL)) {
			res = -EINVAL;
			goto error;
		}
		for (previous = 0; previous < index; previous++)
			if (impl->feedback_bridges[previous].input_port == bridge->input_port ||
			    impl->feedback_bridges[previous].output_port == bridge->output_port) {
				res = -EEXIST;
				goto error;
			}
		if ((res = spa_fgn_graph_get_port_format(impl->graph,
				SPA_DIRECTION_INPUT, bridge->input_port,
				&input_format)) < 0 ||
		    (res = spa_fgn_graph_get_port_format(impl->graph,
				SPA_DIRECTION_OUTPUT, bridge->output_port,
				&output_format)) < 0 ||
		    (res = fgn_format_size(input_format, &input_size,
				&input_stride)) < 0 ||
		    (res = fgn_format_size(output_format, &output_size,
				&output_stride)) < 0)
			goto error;
		if (!feedback_format_equal(input_format, output_format) ||
		    input_size != output_size || input_stride != output_stride) {
			res = -EINVAL;
			goto error;
		}
		if ((res = init_feedback_buffer(&bridge->input, input_size,
				input_stride, true)) < 0 ||
		    (res = init_feedback_buffer(&bridge->output, output_size,
				output_stride, false)) < 0)
			goto error;
		index++;
	}
	return 0;
error:
	pw_log_error("invalid feedback bridge '%s': '%s': %s",
			item->key, item->value == NULL ? "" : item->value,
			spa_strerror(res));
	clear_feedback_bridges(impl);
	return res;
}

static bool feedback_input_private(const struct impl *impl, uint32_t input)
{
	uint32_t i;

	for (i = 0; i < impl->n_feedback_bridges; i++)
		if (impl->feedback_bridges[i].input_port == input)
			return true;
	return false;
}

static bool feedback_output_private(const struct impl *impl, uint32_t output)
{
	uint32_t i;

	for (i = 0; i < impl->n_feedback_bridges; i++)
		if (impl->feedback_bridges[i].output_port == output)
			return true;
	return false;
}

static int commit_feedback_bridges(struct impl *impl)
{
	bool absent = false, present = false;
	uint32_t i;

	for (i = 0; i < impl->n_feedback_bridges; i++) {
		const struct feedback_bridge *bridge = &impl->feedback_bridges[i];

		if (bridge->output.chunk.size == 0)
			absent = true;
		else if (bridge->output.chunk.size == bridge->output.size)
			present = true;
		else
			return -ENODATA;
	}
	if (absent && present)
		return -ENODATA;
	if (absent)
		return 0;
	for (i = 0; i < impl->n_feedback_bridges; i++) {
		struct feedback_bridge *bridge = &impl->feedback_bridges[i];

		memcpy(bridge->input.memory, bridge->output.memory,
				bridge->input.size);
		bridge->input.header = bridge->output.header;
	}
	return 0;
}

static void reset_feedback_bridges(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_feedback_bridges; i++) {
		struct feedback_bridge *bridge = &impl->feedback_bridges[i];

		memset(bridge->input.memory, 0, bridge->input.size);
		memset(&bridge->input.header, 0, sizeof(bridge->input.header));
		bridge->input.header.pts = SPA_TIME_INVALID;
		bridge->input.chunk.size = bridge->input.size;
		bridge->output.chunk.size = 0;
	}
}

static struct spa_pod *build_format(struct spa_pod_builder *builder,
		uint32_t id, const struct spa_fgn_format *format)
{
	struct spa_pod_frame object;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(format->element_type),
			SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				format->n_dimensions, format->shape),
			SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(format->layout),
			0);
	if (format->rate_denom != 0)
		spa_pod_builder_add(builder, SPA_FORMAT_NDARRAY_rate,
				SPA_POD_Fraction(&SPA_FRACTION(
					format->rate_num, format->rate_denom)), 0);
	if (format->schema != NULL)
		spa_pod_builder_add(builder, SPA_FORMAT_NDARRAY_schema,
				SPA_POD_String(format->schema), 0);
	return spa_pod_builder_pop(builder, &object);
}

static struct spa_pod *build_acquisition_meta(struct spa_pod_builder *builder)
{
	struct spa_pod_frame frame;

	spa_pod_builder_push_object(builder, &frame,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(builder,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_acquisition)),
			0);
	spa_pod_builder_prop(builder, SPA_PARAM_META_features,
			SPA_POD_PROP_FLAG_MANDATORY);
	spa_pod_builder_int(builder, SPA_META_FEATURE_ACQUISITION_VERSION_2);
	return spa_pod_builder_pop(builder, &frame);
}

static bool strings_equal(const char *a, const char *b)
{
	return a == b || (a != NULL && b != NULL && spa_streq(a, b));
}

static int validate_port_format(struct port *port, const struct spa_pod *param)
{
	const struct spa_fgn_format *expected;
	struct spa_ndarray_info actual;
	const char *schema;
	uint32_t i;
	int res;

	if ((res = spa_fgn_graph_get_port_format(port->impl->graph,
			port->direction, port->index, &expected)) < 0 ||
	    (res = spa_format_ndarray_parse(param, &actual)) < 0 ||
	    (res = spa_format_ndarray_parse_string(param,
			SPA_FORMAT_NDARRAY_schema, &schema)) < 0)
		return res;
	if ((uint32_t)actual.element_type != expected->element_type ||
	    (uint32_t)actual.layout != expected->layout ||
	    actual.rate.num != expected->rate_num ||
	    actual.rate.denom != expected->rate_denom ||
	    actual.n_dimensions != expected->n_dimensions ||
	    !strings_equal(schema, expected->schema))
		return -EINVAL;
	for (i = 0; i < actual.n_dimensions; i++)
		if (actual.shape[i] != expected->shape[i])
			return -EINVAL;
	return 0;
}

static int build_graph_param_offsets(struct impl *impl,
		struct spa_pod_dynamic_builder *builder,
		uint32_t *offsets, uint32_t *n_params)
{
	struct spa_pod *props;
	int res;

	offsets[*n_params] = builder->b.state.offset;
	pthread_mutex_lock(&impl->control_lock);
	res = spa_fgn_graph_get_props(impl->graph, &builder->b, &props);
	pthread_mutex_unlock(&impl->control_lock);
	if (res < 0)
		return res;
	(*n_params)++;
	if (impl->run_control) {
		offsets[*n_params] = builder->b.state.offset;
		props = pw_ao_run_control_build_status(&builder->b,
				impl->completed_token, impl->run_control_result,
				impl->actual_state);
		if (props == NULL)
			return -ENOSPC;
		(*n_params)++;
	}
	if (impl->reset_control) {
		offsets[*n_params] = builder->b.state.offset;
		props = pw_ao_reset_control_build_status(&builder->b,
				impl->completed_reset_token, impl->reset_result);
		if (props == NULL)
			return -ENOSPC;
		(*n_params)++;
	}
	return 0;
}

static int publish_graph_props(struct impl *impl)
{
	uint8_t initial[4096];
	struct spa_pod_dynamic_builder builder;
	const struct spa_pod *params[3];
	uint32_t offsets[3];
	uint32_t n_params = 0;
	int res;

	spa_pod_dynamic_builder_init(&builder, initial, sizeof(initial), 4096);
	res = build_graph_param_offsets(impl, &builder, offsets, &n_params);
	for (uint32_t i = 0; res >= 0 && i < n_params; i++)
		params[i] = SPA_PTROFF(builder.b.data, offsets[i],
				const struct spa_pod);
	if (res >= 0 && impl->filter != NULL)
		res = pw_filter_update_params(impl->filter, NULL, params, n_params);
	spa_pod_dynamic_builder_clean(&builder);
	return res;
}

static void schedule_param_publication(struct impl *impl)
{
	atomic_store_explicit(&impl->publish_after_process, true,
			memory_order_release);
	if (impl->main_event != NULL)
		pw_loop_signal_event(impl->main_loop, impl->main_event);
}

static void publish_run_control_response(struct impl *impl, int64_t token,
		int result, enum pw_ao_run_control_state actual_state)
{
	impl->completed_token = token;
	impl->run_control_result = result;
	impl->actual_state = actual_state;
	schedule_param_publication(impl);
}

static void complete_run_control(struct impl *impl, int result,
		enum pw_ao_run_control_state actual_state)
{
	publish_run_control_response(impl, impl->last_request_token, result,
			actual_state);
	impl->requested_state = PW_AO_RUN_CONTROL_STATE_UNKNOWN;
}

static void main_event(void *data, uint64_t count SPA_UNUSED)
{
	struct impl *impl = data;
	int res;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	res = atomic_exchange_explicit(&impl->process_error, 0,
			memory_order_acq_rel);
	if (res < 0 && impl->filter != NULL) {
		pw_filter_set_error(impl->filter, res,
				"ndarray graph process failed: %s", spa_strerror(res));
		return;
	}
	if (!atomic_exchange_explicit(&impl->publish_after_process, false,
			memory_order_acq_rel))
		return;
	res = publish_graph_props(impl);
	if (res == -EAGAIN) {
		atomic_store_explicit(&impl->publish_after_process, true,
				memory_order_release);
		return;
	}
	if (res < 0 && impl->filter != NULL)
		pw_filter_set_error(impl->filter, res,
				"can't publish ndarray graph properties: %s",
				spa_strerror(res));
}

static void update_parameter(struct port *port)
{
	struct impl *impl = port->impl;
	struct pw_buffer *buffer;
	int res;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	buffer = ndarray_parameter_handoff_pending(&port->parameter_handoff);
	if (buffer == NULL)
		return;
	pthread_mutex_lock(&impl->control_lock);
	res = spa_fgn_graph_update_parameter(impl->graph, port->index,
			buffer->buffer);
	pthread_mutex_unlock(&impl->control_lock);
	if (res == -EBUSY) {
		ndarray_parameter_handoff_mark_retry(&port->parameter_handoff);
		return;
	}
	if (res < 0)
		atomic_fetch_add_explicit(&port->dropped_parameters, 1,
				memory_order_relaxed);
	else {
		atomic_store_explicit(&impl->publish_after_process, true,
				memory_order_release);
		pw_loop_signal_event(impl->main_loop, impl->main_event);
	}
	/* The data loop is the sole producer for pw_filter_queue_buffer(). */
	ndarray_parameter_handoff_complete(&port->parameter_handoff);
}

static void parameter_event(void *data, uint64_t count SPA_UNUSED)
{
	struct impl *impl = data;
	uint32_t i;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	for (i = 0; i < impl->n_inputs; i++) {
		struct port *port = impl->inputs[i];
		if (port != NULL &&
		    (port->flags & SPA_FGN_PORT_FLAG_PARAMETER) &&
		    ndarray_parameter_handoff_claim(&port->parameter_handoff))
			update_parameter(port);
	}
}

static void schedule_parameter(struct port *port, struct pw_buffer *buffer)
{
	struct impl *impl = port->impl;
	int res;

	if (!ndarray_parameter_handoff_schedule(&port->parameter_handoff, buffer)) {
		atomic_fetch_add_explicit(&port->dropped_parameters, 1,
				memory_order_relaxed);
		pw_filter_queue_buffer(port, buffer);
		return;
	}
	res = pw_loop_signal_event(pw_thread_loop_get_loop(impl->parameter_loop),
			impl->parameter_event);
	if (res < 0 && ndarray_parameter_handoff_cancel_schedule(
			&port->parameter_handoff)) {
		atomic_fetch_add_explicit(&port->dropped_parameters, 1,
				memory_order_relaxed);
		pw_filter_queue_buffer(port, buffer);
	}
}

static bool parameter_buffer_absent(const struct pw_buffer *buffer)
{
	const struct spa_buffer *spa_buffer;
	const struct spa_data *data;

	if (buffer == NULL || (spa_buffer = buffer->buffer) == NULL ||
	    spa_buffer->n_datas != 1 || spa_buffer->datas == NULL ||
	    (uintptr_t)spa_buffer->datas % _Alignof(struct spa_data) != 0)
		return false;
	data = &spa_buffer->datas[0];
	return data->data != NULL && data->chunk != NULL &&
		(uintptr_t)data->chunk % _Alignof(struct spa_chunk) == 0 &&
		data->chunk->offset <= data->maxsize && data->chunk->size == 0;
}

static void dequeue_parameter(struct port *port)
{
	struct pw_buffer *buffer = NULL, *next;
	struct pw_buffer *completed;

	completed = ndarray_parameter_handoff_take_completed(
			&port->parameter_handoff);
	if (completed != NULL)
		pw_filter_queue_buffer(port, completed);

	while ((next = pw_filter_dequeue_buffer(port)) != NULL) {
		if (parameter_buffer_absent(next)) {
			pw_filter_queue_buffer(port, next);
			continue;
		}
		if (buffer != NULL)
			pw_filter_queue_buffer(port, buffer);
		buffer = next;
	}
	if (buffer != NULL)
		schedule_parameter(port, buffer);
}

static void recycle_cycle_inputs(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_inputs; i++)
		if (impl->input_buffers[i] != NULL) {
			pw_filter_queue_buffer(impl->inputs[i], impl->input_buffers[i]);
			impl->input_buffers[i] = NULL;
		}
}

static void publish_completed_outputs(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_outputs; i++) {
		struct pw_buffer *buffer = impl->output_buffers[i];
		struct spa_buffer *spa_buffer;

		if (buffer == NULL)
			continue;
		spa_buffer = buffer->buffer;
		if (spa_buffer != NULL && spa_buffer->n_datas > 0 &&
		    spa_buffer->datas != NULL && spa_buffer->datas[0].chunk != NULL &&
		    spa_buffer->datas[0].chunk->size == 0)
			continue;
		pw_filter_queue_buffer(impl->outputs[i], buffer);
		impl->output_buffers[i] = NULL;
	}
}

static void process(void *data, struct spa_io_position *position SPA_UNUSED)
{
	struct impl *impl = data;
	bool ready = true;
	uint32_t i;
	int res;

	if (atomic_load_explicit(&impl->process_failed, memory_order_acquire))
		return;
	if (impl->n_inputs > 0)
		memset(impl->process_inputs, 0,
				impl->n_inputs * sizeof(*impl->process_inputs));
	if (impl->n_outputs > 0)
		memset(impl->process_outputs, 0,
				impl->n_outputs * sizeof(*impl->process_outputs));
	for (i = 0; i < impl->n_inputs; i++) {
		struct port *port = impl->inputs[i];
		struct pw_buffer *buffer = NULL, *next;

		if (port == NULL)
			continue;
		if (port->flags & SPA_FGN_PORT_FLAG_PARAMETER) {
			dequeue_parameter(port);
			continue;
		}
		if (impl->fifo_inputs) {
			buffer = impl->input_buffers[i];
			if (buffer == NULL)
				buffer = pw_filter_dequeue_buffer(port);
		} else {
			while ((next = pw_filter_dequeue_buffer(port)) != NULL) {
				if (buffer != NULL)
					pw_filter_queue_buffer(port, buffer);
				buffer = next;
			}
		}
		impl->input_buffers[i] = buffer;
		if (buffer != NULL)
			impl->process_inputs[i] = buffer->buffer;
		else if (!(port->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
			ready = false;
	}
	for (i = 0; i < impl->n_outputs; i++) {
		struct pw_buffer *buffer;

		if (impl->outputs[i] == NULL)
			continue;
		buffer = impl->output_buffers[i];

		if (buffer == NULL)
			buffer = pw_filter_dequeue_buffer(impl->outputs[i]);
		impl->output_buffers[i] = buffer;
		if (buffer == NULL)
			ready = false;
		else
			impl->process_outputs[i] = buffer->buffer;
	}
	for (i = 0; i < impl->n_feedback_bridges; i++) {
		struct feedback_bridge *bridge = &impl->feedback_bridges[i];

		impl->process_inputs[bridge->input_port] = &bridge->input.buffer;
		impl->process_outputs[bridge->output_port] = &bridge->output.buffer;
	}
	if (ready) {
		res = spa_fgn_graph_process(impl->graph,
				impl->process_inputs, impl->n_inputs,
				impl->process_outputs, impl->n_outputs);
		if (res >= 0) {
			int feedback_res = commit_feedback_bridges(impl);
			if (feedback_res < 0) {
				res = feedback_res;
				pw_log_error("ndarray feedback output incomplete: %s",
						spa_strerror(res));
			}
		}
		if (res < 0) {
			atomic_store_explicit(&impl->process_failed, true,
					memory_order_release);
			for (i = 0; i < impl->n_outputs; i++) {
				struct spa_buffer *buffer = impl->process_outputs[i];
				if (buffer != NULL && buffer->n_datas > 0 &&
				    buffer->datas != NULL && buffer->datas[0].chunk != NULL)
					buffer->datas[0].chunk->size = 0;
			}
			if (atomic_exchange_explicit(&impl->process_error, res,
					memory_order_acq_rel) == 0)
				pw_loop_signal_event(impl->main_loop, impl->main_event);
		}
		if (res >= 0) {
			if (res & SPA_FGN_PROCESS_RESULT_PROPS_CHANGED)
				atomic_store_explicit(&impl->publish_after_process, true,
						memory_order_release);
			if (atomic_load_explicit(&impl->publish_after_process,
					memory_order_acquire))
				pw_loop_signal_event(impl->main_loop, impl->main_event);
			for (i = 0; i < impl->n_inputs; i++) {
				struct port *port = impl->inputs[i];
				if (port != NULL &&
				    (port->flags & SPA_FGN_PORT_FLAG_PARAMETER) &&
				    ndarray_parameter_handoff_rearm_retry(
						&port->parameter_handoff)) {
					int signal_res = pw_loop_signal_event(
							pw_thread_loop_get_loop(impl->parameter_loop),
							impl->parameter_event);
					if (signal_res < 0)
						ndarray_parameter_handoff_restore_retry(
								&port->parameter_handoff);
				}
			}
		}
		/* Queueing an output buffer publishes it. A failed graph call keeps
		 * every dequeued output owned by this filter until error teardown. */
		if (res >= 0)
			publish_completed_outputs(impl);
	}
	if (ready || !impl->fifo_inputs)
		recycle_cycle_inputs(impl);
}

static int prepare_process_thread(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED,
		void *user_data)
{
	struct impl *impl = user_data;

	return spa_fgn_graph_prepare_process_thread(impl->graph);
}

static int update_graph_processing(struct impl *impl,
		enum pw_filter_state state,
		enum pw_ao_run_control_state *actual)
{
	int res = 0;

	*actual = impl->actual_state;
	pthread_mutex_lock(&impl->control_lock);
	if (state == PW_FILTER_STATE_STREAMING &&
	    (!impl->run_control ||
	     impl->requested_state == PW_AO_RUN_CONTROL_STATE_RUNNING ||
	     impl->actual_state == PW_AO_RUN_CONTROL_STATE_RUNNING)) {
		struct pw_loop *data_loop;

		if (atomic_load_explicit(&impl->process_failed,
				memory_order_acquire)) {
			res = -EIO;
			goto done;
		}
		res = spa_fgn_graph_activate(impl->graph);
		data_loop = pw_filter_get_data_loop(impl->filter);
		if (res >= 0 && data_loop == NULL)
			res = -EIO;
		if (res >= 0)
			res = pw_loop_invoke(data_loop, prepare_process_thread,
					0, NULL, 0, true, impl);
		if (res < 0)
			spa_fgn_graph_deactivate(impl->graph);
		else
			*actual = PW_AO_RUN_CONTROL_STATE_RUNNING;
	} else if (state == PW_FILTER_STATE_PAUSED) {
		res = spa_fgn_graph_deactivate(impl->graph);
		if (res >= 0)
			*actual = PW_AO_RUN_CONTROL_STATE_STOPPED;
	}
done:
	pthread_mutex_unlock(&impl->control_lock);
	return res;
}

static void filter_state_changed(void *data, enum pw_filter_state old SPA_UNUSED,
		enum pw_filter_state state, const char *error)
{
	struct impl *impl = data;
	enum pw_ao_run_control_state actual;
	int res;

	impl->filter_state = state;
	res = update_graph_processing(impl, state, &actual);
	if (impl->run_control) {
		if (res < 0 && impl->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN)
			complete_run_control(impl, res, actual);
		else if (res >= 0 && actual == impl->requested_state)
			complete_run_control(impl, 0, actual);
		else if (impl->requested_state == PW_AO_RUN_CONTROL_STATE_UNKNOWN &&
			 actual != PW_AO_RUN_CONTROL_STATE_UNKNOWN &&
			 actual != impl->actual_state) {
			impl->actual_state = actual;
			schedule_param_publication(impl);
		}
	}
	if (res < 0)
		pw_filter_set_error(impl->filter, res,
				"ndarray graph state change failed: %s", spa_strerror(res));
	if (state == PW_FILTER_STATE_ERROR)
		pw_log_error("filter error: %s", error);
}

static void filter_param_changed(void *data, void *port_data,
		uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	int res;

	if (param == NULL)
		return;
	if (port_data != NULL && id == SPA_PARAM_Format) {
		res = validate_port_format(port_data, param);
	} else if (port_data == NULL && id == SPA_PARAM_Props) {
		struct pw_ao_run_control_request request = { 0 };

		res = impl->run_control
			? pw_ao_run_control_parse_request(param, &request) : -ENOENT;
		if (res != -ENOENT) {
			if (res < 0) {
				pw_log_warn("invalid run-control request: %s",
						spa_strerror(res));
				if (request.token > 0)
					publish_run_control_response(impl, request.token,
							res, impl->actual_state);
				return;
			}
			if (impl->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN) {
				pw_log_warn("run-control request %" PRIi64
						" rejected while request %" PRIi64 " is pending",
						request.token, impl->last_request_token);
				publish_run_control_response(impl, request.token, -EBUSY,
						impl->actual_state);
				return;
			}
			if (request.token <= impl->last_request_token) {
				pw_log_warn("stale or duplicate run-control request %" PRIi64,
						request.token);
				publish_run_control_response(impl, request.token,
						request.token == impl->last_request_token
							? -EALREADY : -ESTALE,
						impl->actual_state);
				return;
			}
			impl->last_request_token = request.token;
			if (request.requested_state == impl->actual_state) {
				complete_run_control(impl, 0, impl->actual_state);
				return;
			}
			impl->requested_state = request.requested_state;
			res = pw_filter_set_active(impl->filter,
					request.requested_state ==
					PW_AO_RUN_CONTROL_STATE_RUNNING);
			if (res < 0)
				complete_run_control(impl, res, impl->actual_state);
			else if ((request.requested_state ==
					PW_AO_RUN_CONTROL_STATE_RUNNING &&
				  impl->filter_state == PW_FILTER_STATE_STREAMING) ||
				 (request.requested_state ==
					PW_AO_RUN_CONTROL_STATE_STOPPED &&
				  impl->filter_state == PW_FILTER_STATE_PAUSED)) {
				enum pw_ao_run_control_state actual;

				res = update_graph_processing(impl, impl->filter_state,
						&actual);
				complete_run_control(impl, res, actual);
			}
			return;
		}
		if (impl->reset_control) {
			struct pw_ao_reset_control_request reset = { 0 };

			res = pw_ao_reset_control_parse_request(param, &reset);
			if (res != -ENOENT) {
				if (res < 0) {
					pw_log_warn("invalid reset-control request: %s",
							spa_strerror(res));
					if (reset.token > 0) {
						impl->completed_reset_token = reset.token;
						impl->reset_result = res;
						schedule_param_publication(impl);
					}
					return;
				}
				if (reset.token <= impl->last_reset_token)
					res = reset.token == impl->last_reset_token
						? -EALREADY : -ESTALE;
				if (res >= 0 &&
				    (impl->actual_state != PW_AO_RUN_CONTROL_STATE_STOPPED ||
				     impl->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN))
					res = -EBUSY;
				if (res != -EALREADY && res != -ESTALE)
					impl->last_reset_token = reset.token;
				if (res >= 0) {
					pthread_mutex_lock(&impl->control_lock);
					res = spa_fgn_graph_reset(impl->graph);
					if (res >= 0) {
						reset_feedback_bridges(impl);
						atomic_store_explicit(&impl->process_error, 0,
								memory_order_release);
						atomic_store_explicit(&impl->process_failed, false,
								memory_order_release);
					}
					pthread_mutex_unlock(&impl->control_lock);
				}
				if (reset.token > 0) {
					impl->completed_reset_token = reset.token;
					impl->reset_result = res;
					schedule_param_publication(impl);
				}
				return;
			}
		}
		pthread_mutex_lock(&impl->control_lock);
		res = spa_fgn_graph_set_props(impl->graph, param);
		pthread_mutex_unlock(&impl->control_lock);
		if (res == -EBUSY) {
			pw_log_warn("ndarray graph property update rejected while a "
					"transaction is pending");
			return;
		}
		if (res >= 0)
			atomic_store_explicit(&impl->publish_after_process, true,
					memory_order_release);
	} else {
		return;
	}
	if (res < 0)
		pw_filter_set_error(impl->filter, res,
				"invalid ndarray filter parameter: %s", spa_strerror(res));
}

static void filter_destroyed(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->filter_listener);
	impl->filter = NULL;
}

static bool invalidate_retained_buffer(struct impl *impl,
		struct port *port, struct pw_buffer *buffer)
{
	struct pw_buffer **retained;

	if (port == NULL || port->impl != impl)
		return false;
	if (port->direction == SPA_DIRECTION_INPUT) {
		if (port->index >= impl->n_inputs)
			return false;
		retained = &impl->input_buffers[port->index];
	} else if (port->direction == SPA_DIRECTION_OUTPUT) {
		if (port->index >= impl->n_outputs)
			return false;
		retained = &impl->output_buffers[port->index];
	} else {
		return false;
	}
	if (*retained != buffer)
		return false;
	*retained = NULL;
	return true;
}

struct retained_buffer_removal {
	struct impl *impl;
	struct port *port;
	struct pw_buffer *buffer;
};

static int invalidate_retained_buffer_on_data_loop(
		struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct retained_buffer_removal *removal = user_data;

	(void)invalidate_retained_buffer(removal->impl, removal->port,
			removal->buffer);
	return 0;
}

static void filter_remove_buffer(void *data, void *port_data,
		struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct port *port = port_data;
	struct retained_buffer_removal removal;
	struct pw_loop *data_loop;
	int res;

	if (port == NULL || buffer == NULL ||
	    atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	removal = (struct retained_buffer_removal) {
		.impl = impl,
		.port = port,
		.buffer = buffer,
	};
	data_loop = impl->filter == NULL ? NULL :
		pw_filter_get_data_loop(impl->filter);
	if (data_loop == NULL)
		invalidate_retained_buffer_on_data_loop(NULL, false, 0,
				NULL, 0, &removal);
	else if ((res = pw_loop_locked(data_loop,
			invalidate_retained_buffer_on_data_loop, 0,
			NULL, 0, &removal)) < 0) {
		pw_filter_set_error(impl->filter, res,
				"can't synchronize retained ndarray buffer removal: %s",
				spa_strerror(res));
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = filter_destroyed,
	.state_changed = filter_state_changed,
	.param_changed = filter_param_changed,
	.remove_buffer = filter_remove_buffer,
	.process = process,
};

static int add_graph_port(struct impl *impl, enum spa_direction direction,
		uint32_t index)
{
	uint8_t buffer[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_fgn_port_info *info;
	const struct spa_fgn_format *format;
	const struct spa_pod *params[4];
	struct pw_properties *properties;
	struct port *port;
	const char *node_name;
	char name[512];
	size_t size;
	int32_t stride;
	uint32_t n_params = 0;
	int res;

	if ((res = spa_fgn_graph_get_port_info(impl->graph, direction, index,
			&node_name, &info)) < 0 ||
	    (res = spa_fgn_graph_get_port_format(impl->graph, direction, index,
			&format)) < 0 ||
	    (res = fgn_format_size(format, &size, &stride)) < 0)
		return res;
	if (spa_scnprintf(name, sizeof(name), "%s:%s", node_name, info->name) < 0)
		return -ENOSPC;
	properties = pw_properties_new(
			PW_KEY_PORT_NAME, name,
			PW_KEY_MEDIA_TYPE, "Application",
			NULL);
	if (properties == NULL)
		return -errno;
	if (info->flags & SPA_FGN_PORT_FLAG_PARAMETER)
		pw_properties_set(properties, PW_KEY_PORT_CONTROL, "true");
	params[n_params++] = build_format(&builder, SPA_PARAM_EnumFormat, format);
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)size),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
				(1u << SPA_DATA_MemFd)));
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[n_params++] = build_acquisition_meta(&builder);
	for (uint32_t i = 0; i < n_params; i++)
		if (params[i] == NULL) {
			pw_properties_free(properties);
			return -ENOSPC;
		}
	port = pw_filter_add_port(impl->filter,
			direction == SPA_DIRECTION_INPUT
				? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(*port), properties,
			params, n_params);
	if (port == NULL)
		return -errno;
	port->impl = impl;
	port->index = index;
	port->direction = direction;
	port->flags = info->flags;
	ndarray_parameter_handoff_init(&port->parameter_handoff);
	atomic_init(&port->dropped_parameters, 0);
	if (direction == SPA_DIRECTION_INPUT)
		impl->inputs[index] = port;
	else
		impl->outputs[index] = port;
	return 0;
}

static int connect_filter(struct impl *impl)
{
	uint8_t initial[4096];
	struct spa_pod_dynamic_builder builder;
	const struct spa_pod **params = NULL;
	uint32_t *offsets = NULL;
	uint32_t n_params = 0, index;
	int res;

	spa_pod_dynamic_builder_init(&builder, initial, sizeof(initial), 4096);
	for (index = 0; ; index++) {
		struct spa_pod *pod;
		uint32_t *new_offsets;
		uint32_t offset = builder.b.state.offset;

		res = spa_fgn_graph_enum_prop_info(impl->graph, index,
				&builder.b, &pod);
		if (res == 0)
			break;
		if (res < 0)
			goto done;
		new_offsets = realloc(offsets, sizeof(*offsets) * (n_params + 1));
		if (new_offsets == NULL) {
			res = -ENOMEM;
			goto done;
		}
		offsets = new_offsets;
		offsets[n_params++] = offset;
	}
	{
		uint32_t *new_offsets;
		uint32_t built_offsets[3];
		uint32_t n_built = 0, built_index;

		if ((res = build_graph_param_offsets(impl, &builder,
				built_offsets, &n_built)) < 0)
			goto done;
		new_offsets = realloc(offsets,
				sizeof(*offsets) * (n_params + n_built));
		if (new_offsets == NULL) {
			res = -ENOMEM;
			goto done;
		}
		offsets = new_offsets;
		for (built_index = 0; built_index < n_built; built_index++)
			offsets[n_params++] = built_offsets[built_index];
	}
	if ((params = calloc(n_params, sizeof(*params))) == NULL) {
		res = -ENOMEM;
		goto done;
	}
	for (index = 0; index < n_params; index++)
		params[index] = SPA_PTROFF(builder.b.data, offsets[index],
				const struct spa_pod);
	res = pw_filter_connect(impl->filter, PW_FILTER_FLAG_RT_PROCESS |
			(impl->run_control ? PW_FILTER_FLAG_INACTIVE : 0),
			params, n_params);
done:
	free(params);
	free(offsets);
	spa_pod_dynamic_builder_clean(&builder);
	return res;
}

static void core_error(void *data, uint32_t id, int seq SPA_UNUSED,
		int res, const char *message SPA_UNUSED)
{
	struct impl *impl = data;

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroyed(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->core_proxy_listener);
	spa_hook_remove(&impl->core_listener);
	impl->core_listeners_added = false;
	impl->core = NULL;
	if (!atomic_load_explicit(&impl->destroying, memory_order_acquire))
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroyed,
};

static void impl_destroy(struct impl *impl)
{
	uint32_t i;

	if (impl == NULL)
		return;
	atomic_store_explicit(&impl->destroying, true, memory_order_release);
	if (impl->filter != NULL)
		pw_filter_disconnect(impl->filter);
	if (impl->parameter_loop_started)
		pw_thread_loop_stop(impl->parameter_loop);
	if (impl->parameter_event != NULL) {
		pw_loop_destroy_source(pw_thread_loop_get_loop(impl->parameter_loop),
				impl->parameter_event);
		impl->parameter_event = NULL;
	}
	/* Producers are quiescent. Cancel the preallocated notification source and
	 * drain main-loop work before releasing callback storage. */
	if (impl->main_event != NULL) {
		pw_loop_destroy_source(impl->main_loop, impl->main_event);
		impl->main_event = NULL;
	}
	pw_loop_invoke(impl->main_loop, NULL, 0, NULL, 0, false, impl);
	for (i = 0; i < impl->n_inputs; i++)
		if (impl->inputs != NULL && impl->inputs[i] != NULL) {
			struct pw_buffer *buffer = ndarray_parameter_handoff_cancel(
					&impl->inputs[i]->parameter_handoff);
			if (buffer != NULL && impl->filter != NULL)
				pw_filter_queue_buffer(impl->inputs[i], buffer);
		}
	if (impl->filter != NULL)
		pw_filter_destroy(impl->filter);
	if (impl->parameter_loop != NULL)
		pw_thread_loop_destroy(impl->parameter_loop);
	if (impl->graph != NULL)
		spa_fgn_graph_free(impl->graph);
	clear_feedback_bridges(impl);
	clear_startup_parameters(impl);
	if (impl->core != NULL) {
		if (impl->core_listeners_added) {
			spa_hook_remove(&impl->core_listener);
			spa_hook_remove(&impl->core_proxy_listener);
			impl->core_listeners_added = false;
		}
		if (impl->disconnect_core)
			pw_core_disconnect(impl->core);
	}
	if (impl->control_lock_initialized)
		pthread_mutex_destroy(&impl->control_lock);
	free(impl->output_buffers);
	free(impl->input_buffers);
	free(impl->process_outputs);
	free(impl->process_inputs);
	free(impl->outputs);
	free(impl->inputs);
	free(impl);
}

static void module_destroyed(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroyed,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *properties = NULL;
	struct impl *impl;
	const char *graph_config, *name, *remote;
	const char *run_control, *reset_control, *fifo_inputs;
	uint32_t id, i;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);
	if ((impl = calloc(1, sizeof(*impl))) == NULL)
		return -errno;
	impl->context = context;
	impl->module = module;
	impl->main_loop = pw_context_get_main_loop(context);
	atomic_init(&impl->destroying, false);
	atomic_init(&impl->process_error, 0);
	atomic_init(&impl->process_failed, false);
	atomic_init(&impl->publish_after_process, false);
	if ((res = pthread_mutex_init(&impl->control_lock, NULL)) != 0) {
		res = -res;
		goto error;
	}
	impl->control_lock_initialized = true;
	properties = args != NULL
		? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error;
	}
	if ((graph_config = pw_properties_get(properties, "filter.graph")) == NULL) {
		res = -EINVAL;
		pw_log_error("missing filter.graph");
		goto error;
	}
	run_control = pw_properties_get(properties, PW_AO_RUN_CONTROL_KEY_ENABLED);
	impl->run_control = run_control != NULL &&
			pw_properties_parse_bool(run_control);
	reset_control = pw_properties_get(properties,
			PW_AO_RESET_CONTROL_KEY_ENABLED);
	impl->reset_control = reset_control != NULL &&
			pw_properties_parse_bool(reset_control);
	fifo_inputs = pw_properties_get(properties, "pipewireao.fifo-inputs");
	impl->fifo_inputs = fifo_inputs != NULL &&
			pw_properties_parse_bool(fifo_inputs);
	impl->actual_state = PW_AO_RUN_CONTROL_STATE_STOPPED;
	impl->filter_state = PW_FILTER_STATE_UNCONNECTED;
	if ((res = spa_fgn_graph_new(graph_config, &impl->graph)) < 0) {
		pw_log_error("can't create ndarray graph: %s", spa_strerror(res));
		goto error;
	}
	impl->n_inputs = spa_fgn_graph_get_n_inputs(impl->graph);
	impl->n_outputs = spa_fgn_graph_get_n_outputs(impl->graph);
	if ((res = configure_feedback_bridges(impl, properties)) < 0) {
		pw_log_error("can't configure feedback bridges: %s", spa_strerror(res));
		goto error;
	}
	if ((res = configure_startup_parameters(impl, properties)) < 0) {
		pw_log_error("can't configure startup parameters: %s", spa_strerror(res));
		goto error;
	}
	log_graph_report(impl->graph);
	if ((impl->n_inputs > 0 &&
	     ((impl->inputs = calloc(impl->n_inputs,
			 sizeof(*impl->inputs))) == NULL ||
	      (impl->process_inputs = calloc(impl->n_inputs,
			 sizeof(*impl->process_inputs))) == NULL ||
	      (impl->input_buffers = calloc(impl->n_inputs,
			 sizeof(*impl->input_buffers))) == NULL)) ||
	    (impl->n_outputs > 0 &&
	     ((impl->outputs = calloc(impl->n_outputs,
			 sizeof(*impl->outputs))) == NULL ||
	      (impl->process_outputs = calloc(impl->n_outputs,
			 sizeof(*impl->process_outputs))) == NULL ||
	      (impl->output_buffers = calloc(impl->n_outputs,
			 sizeof(*impl->output_buffers))) == NULL))) {
		res = -ENOMEM;
		goto error;
	}
	if (pw_properties_get(properties, PW_KEY_NODE_NAME) == NULL) {
		id = pw_global_get_id(pw_impl_module_get_global(module));
		pw_properties_setf(properties, PW_KEY_NODE_NAME,
				"ndarray-filter-chain-%u-%u", getpid(), id);
	}
	if (pw_properties_get(properties, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(properties, PW_KEY_NODE_DESCRIPTION,
				pw_properties_get(properties, PW_KEY_NODE_NAME));
	if (pw_properties_get(properties, PW_KEY_MEDIA_TYPE) == NULL)
		pw_properties_set(properties, PW_KEY_MEDIA_TYPE, "Application");
	if (pw_properties_get(properties, PW_KEY_MEDIA_CATEGORY) == NULL)
		pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Filter");
	if (pw_properties_get(properties, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(properties, PW_KEY_NODE_VIRTUAL, "true");
	if (impl->reset_control)
		pw_properties_set(properties, PW_AO_RESET_CONTROL_KEY_ENABLED, "true");
	remote = pw_properties_get(properties, PW_KEY_REMOTE_NAME);
	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		impl->core = pw_context_connect(context,
				pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL), 0);
		impl->disconnect_core = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		goto error;
	}
	pw_proxy_add_listener((struct pw_proxy *)impl->core,
			&impl->core_proxy_listener, &core_proxy_events, impl);
	pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);
	impl->core_listeners_added = true;
	name = pw_properties_get(properties, PW_KEY_NODE_NAME);
	pw_properties_set(properties, "filter.graph", NULL);
	impl->filter = pw_filter_new(impl->core, name, properties);
	properties = NULL;
	if (impl->filter == NULL) {
		res = -errno;
		goto error;
	}
	pw_filter_add_listener(impl->filter, &impl->filter_listener,
			&filter_events, impl);
	for (i = 0; i < impl->n_inputs; i++)
		if (!feedback_input_private(impl, i) &&
		    (res = add_graph_port(impl, SPA_DIRECTION_INPUT, i)) < 0)
			goto error;
	for (i = 0; i < impl->n_outputs; i++)
		if (!feedback_output_private(impl, i) &&
		    (res = add_graph_port(impl, SPA_DIRECTION_OUTPUT, i)) < 0)
			goto error;
	impl->parameter_loop = pw_thread_loop_new("ndarray-parameters", NULL);
	if (impl->parameter_loop == NULL) {
		res = -errno;
		goto error;
	}
	impl->main_event = pw_loop_add_event(impl->main_loop, main_event, impl);
	impl->parameter_event = pw_loop_add_event(
			pw_thread_loop_get_loop(impl->parameter_loop),
			parameter_event, impl);
	if (impl->main_event == NULL || impl->parameter_event == NULL) {
		res = -errno;
		goto error;
	}
	if ((res = pw_thread_loop_start(impl->parameter_loop)) < 0)
		goto error;
	impl->parameter_loop_started = true;
	if ((res = connect_filter(impl)) < 0) {
		pw_log_error("can't connect ndarray filter: %s", spa_strerror(res));
		goto error;
	}
	pw_impl_module_add_listener(module, &impl->module_listener,
			&module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	return 0;
error:
	pw_properties_free(properties);
	impl_destroy(impl);
	return res;
}
