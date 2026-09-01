/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/debug/filter-graph-ndarray.h>
#include <spa/filter-graph/filter-graph-ndarray.h>
#include <spa/filter-graph/ndarray-executor.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta meta;
	struct spa_meta_header header;
	float values[5];
};

struct report_capture {
	struct spa_debug_context context;
	char text[32768];
	size_t length;
	bool truncated;
};

static void capture_report_line(struct spa_debug_context *context,
		const char *format, ...) SPA_PRINTF_FUNC(2, 3);

static void capture_report_line(struct spa_debug_context *context,
		const char *format, ...)
{
	struct report_capture *capture = SPA_CONTAINER_OF(context,
			struct report_capture, context);
	size_t available = sizeof(capture->text) - capture->length;
	va_list args;
	int written;

	if (available <= 1) {
		capture->truncated = true;
		return;
	}
	va_start(args, format);
	written = vsnprintf(capture->text + capture->length, available,
			format, args);
	va_end(args);
	if (written < 0 || (size_t)written >= available) {
		capture->truncated = true;
		capture->length = sizeof(capture->text) - 1;
		capture->text[capture->length] = '\0';
		return;
	}
	capture->length += (size_t)written;
	if (capture->length + 1 >= sizeof(capture->text)) {
		capture->truncated = true;
		return;
	}
	capture->text[capture->length++] = '\n';
	capture->text[capture->length] = '\0';
}

static void init_buffer(struct test_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->chunk.size = 4 * sizeof(float);
	buffer->chunk.stride = 2 * sizeof(float);
	buffer->data.type = SPA_DATA_MemPtr;
	buffer->data.flags = SPA_DATA_FLAG_READWRITE;
	buffer->data.fd = -1;
	buffer->data.maxsize = sizeof(buffer->values);
	buffer->data.data = buffer->values;
	buffer->data.chunk = &buffer->chunk;
	buffer->meta.type = SPA_META_Header;
	buffer->meta.size = sizeof(buffer->header);
	buffer->meta.data = &buffer->header;
	buffer->buffer.n_metas = 1;
	buffer->buffer.metas = &buffer->meta;
	buffer->buffer.n_datas = 1;
	buffer->buffer.datas = &buffer->data;
}

static void test_graph_inspection(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	struct spa_fgn_graph_info graph_info = {
		.struct_size = sizeof(graph_info),
	};
	struct spa_fgn_graph_info short_info = {
		.struct_size = sizeof(short_info.struct_size),
	};
	struct spa_fgn_node_info node = { .struct_size = sizeof(node) };
	struct spa_fgn_graph_port_info port = { .struct_size = sizeof(port) };
	struct spa_fgn_link_info link = { .struct_size = sizeof(link) };
	struct report_capture capture = {
		.context = { .log = capture_report_line },
	};
	char config[4096];
	int res;

	/* Declaration order is intentionally the reverse of execution order. */
	res = snprintf(config, sizeof(config),
		"{ workers = { helpers = 3 } nodes = ["
		" { type = ndarray name = consumer plugin = \"%s\""
		"   label = scale-f32"
		"   config = { shape = [ 2 2 ] schema = test.matrix/1 } }"
		" { type = ndarray name = producer plugin = \"%s\""
		"   label = scale-f32"
		"   config = { shape = [ 2 2 ] schema = test.matrix/1 } }"
		"] links = ["
		" { output = \"producer:out\" input = \"consumer:in\" }"
		"] inputs = [ \"producer:in\" \"producer:coefficients\""
		"             \"consumer:coefficients\" ]"
		" outputs = [ \"consumer:out\" ] }",
		plugin, plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);

	assert(spa_fgn_graph_get_info(NULL, &graph_info) == -EINVAL);
	assert(spa_fgn_graph_get_info(graph, NULL) == -EINVAL);
	assert(spa_fgn_graph_get_info(graph, &short_info) == -ENOSPC);
	assert(spa_fgn_graph_get_info(graph, &graph_info) == 0);
	assert(graph_info.struct_size == sizeof(graph_info));
	assert(graph_info.n_nodes == 2);
	assert(graph_info.n_links == 1);
	assert(graph_info.n_inputs == 3);
	assert(graph_info.n_outputs == 1);
	assert(graph_info.n_lanes == 4);
	assert(graph_info.executor_flags == SPA_FGN_EXECUTOR_FLAG_POLLING);
	assert(graph_info.allocated_output_buffer_bytes == 32);
	assert(graph_info.internal_output_buffer_bytes == 16);

	assert(spa_fgn_graph_get_node_info(graph, 0, &node) == 0);
	assert(node.execution_index == 0);
	assert(strcmp(node.name, "producer") == 0);
	assert(strcmp(node.plugin_path, plugin) == 0);
	assert(node.plugin_name != NULL && node.plugin_name[0] != '\0');
	assert(strcmp(node.descriptor_name, "scale-f32") == 0);
	assert(node.n_inputs == 2);
	assert(node.n_outputs == 1);
	assert(node.n_properties == 8);
	assert(node.capabilities ==
			(SPA_FGN_NODE_CAPABILITY_RUNTIME_PROPERTIES |
			 SPA_FGN_NODE_CAPABILITY_PARAMETER_INPUTS));
	assert(node.allocated_output_buffer_bytes == 16);
	assert(node.internal_output_buffer_bytes == 16);
	assert(spa_fgn_graph_get_node_info(graph, 1, &node) == 0);
	assert(strcmp(node.name, "consumer") == 0);
	assert(node.allocated_output_buffer_bytes == 16);
	assert(node.internal_output_buffer_bytes == 0);
	assert(spa_fgn_graph_get_node_info(graph, 2, &node) == -ENOENT);

	assert(spa_fgn_graph_get_node_port_info(graph, 0,
			SPA_DIRECTION_INPUT, 0, &port) == 0);
	assert(strcmp(port.info->name, "in") == 0);
	assert(port.external_index == 0);
	assert(port.source_execution_index == SPA_ID_INVALID);
	assert(port.source_port_ordinal == SPA_ID_INVALID);
	assert(port.payload_bytes == 4 * sizeof(float));
	assert(port.format->element_type == SPA_ELEMENT_TYPE_F32_LE);
	assert(port.format->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	assert(port.format->n_dimensions == 2);
	assert(port.format->shape[0] == 2 && port.format->shape[1] == 2);
	assert(strcmp(port.format->schema, "test.matrix/1") == 0);
	assert(port.allocated_buffer_bytes == 0);
	assert(port.internal_buffer_bytes == 0);

	assert(spa_fgn_graph_get_node_port_info(graph, 1,
			SPA_DIRECTION_INPUT, 0, &port) == 0);
	assert(strcmp(port.info->name, "in") == 0);
	assert(port.external_index == SPA_ID_INVALID);
	assert(port.source_execution_index == 0);
	assert(port.source_port_ordinal == 0);
	assert(spa_fgn_graph_get_node_port_info(graph, 0,
			SPA_DIRECTION_OUTPUT, 0, &port) == 0);
	assert(strcmp(port.info->name, "out") == 0);
	assert(port.n_consumers == 1);
	assert(port.external_index == SPA_ID_INVALID);
	assert(port.allocated_buffer_bytes == 16);
	assert(port.internal_buffer_bytes == 16);
	assert(spa_fgn_graph_get_node_port_info(graph, 1,
			SPA_DIRECTION_OUTPUT, 0, &port) == 0);
	assert(port.external_index == 0);
	assert(port.n_consumers == 0);
	assert(port.allocated_buffer_bytes == 16);
	assert(port.internal_buffer_bytes == 0);
	assert(spa_fgn_graph_get_node_port_info(graph, 0,
			(enum spa_direction)2, 0, &port) == -EINVAL);
	assert(spa_fgn_graph_get_node_port_info(graph, 0,
			SPA_DIRECTION_OUTPUT, 1, &port) == -ENOENT);

	assert(spa_fgn_graph_get_link_info(graph, 0, &link) == 0);
	assert(link.link_index == 0);
	assert(link.output_execution_index == 0);
	assert(link.output_port_ordinal == 0);
	assert(link.input_execution_index == 1);
	assert(link.input_port_ordinal == 0);
	assert(spa_fgn_graph_get_link_info(graph, 1, &link) == -ENOENT);

	assert(spa_debugc_fgn_graph(&capture.context, 0, graph) == 0);
	assert(!capture.truncated);
	assert(strstr(capture.text,
			"ndarray graph: nodes=2 links=1 inputs=3 outputs=1 lanes=4") != NULL);
	assert(strstr(capture.text,
			"output buffers: allocated=32 internal=16 bytes") != NULL);
	assert(strstr(capture.text, "[0] producer:") != NULL);
	assert(strstr(capture.text, "[1] consumer:") != NULL);
	assert(strstr(capture.text, "f32-le[2,2] row-major, 16 bytes") != NULL);
	assert(strstr(capture.text, "schema=test.matrix/1") != NULL);
	assert(strstr(capture.text, "runtime-properties parameter-inputs") != NULL);
	assert(strstr(capture.text, "producer:out -> consumer:in") != NULL);

	spa_fgn_graph_free(graph);
}

static struct spa_pod *build_gain_update(void *data, size_t size,
		float first, float second)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("first:gain"), SPA_POD_Float(first),
			SPA_POD_String("second:gain"), SPA_POD_Float(second),
			0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_two_float_update(void *data, size_t size,
		const char *first_name, float first,
		const char *second_name, float second)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String(first_name), SPA_POD_Float(first),
			SPA_POD_String(second_name), SPA_POD_Float(second),
			0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_mode_update(void *data, size_t size, uint32_t mode)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("first:mode"), SPA_POD_Id(mode), 0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_empty_update(void *data, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_odd_update(void *data, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, "first:gain");
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_non_string_key_update(void *data, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_int(&builder, 1);
	spa_pod_builder_float(&builder, 2.0f);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_duplicate_update(void *data, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("first:gain"), SPA_POD_Float(2.0f),
			SPA_POD_String("first:gain"), SPA_POD_Float(3.0f),
			0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static void check_properties(struct spa_fgn_graph *graph)
{
	uint8_t data[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod *pod = NULL;
	uint32_t count = 0;

	while (spa_fgn_graph_enum_prop_info(graph, count, &builder, &pod) == 1) {
		const struct spa_pod_prop *type;

		assert(pod != NULL);
		type = spa_pod_find_prop(pod, NULL, SPA_PROP_INFO_type);
		assert(type != NULL);
		if (count == 0) {
			assert((type->flags & SPA_POD_PROP_FLAG_READONLY) == 0);
			assert(spa_pod_is_choice(&type->value));
			assert(SPA_POD_CHOICE_TYPE(&type->value) == SPA_CHOICE_Range);
		} else if (count == 1) {
			assert((type->flags & SPA_POD_PROP_FLAG_READONLY) != 0);
		} else if (count == 6) {
			const struct spa_pod_prop *labels = spa_pod_find_prop(
					pod, NULL, SPA_PROP_INFO_labels);
			assert(spa_pod_is_choice(&type->value));
			assert(SPA_POD_CHOICE_TYPE(&type->value) == SPA_CHOICE_Enum);
			assert(SPA_POD_CHOICE_N_VALUES(&type->value) == 3);
			assert(labels != NULL && spa_pod_is_struct(&labels->value));
		}
		count++;
		spa_pod_builder_init(&builder, data, sizeof(data));
	}
	assert(count == 16);
	spa_pod_builder_init(&builder, data, sizeof(data));
	assert(spa_fgn_graph_get_props(graph, &builder, &pod) == 1);
	assert(pod != NULL);
}

static void check_failed_property_snapshot(const char *plugin)
{
	uint8_t data[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod_builder_state state;
	struct spa_pod_frame outer;
	struct spa_fgn_graph *graph = NULL;
	struct spa_pod *pod;
	char config[2048];
	uint32_t outer_size;
	int res;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = unstable plugin = \"%s\""
		"   label = unstable-properties }"
		"] inputs = [ \"unstable:in\" ] outputs = [ ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);

	spa_pod_builder_push_struct(&builder, &outer);
	spa_pod_builder_int(&builder, 42);
	spa_pod_builder_get_state(&builder, &state);
	outer_size = outer.pod.size;
	assert(spa_fgn_graph_get_props(graph, &builder, &pod) == -EAGAIN);
	assert(builder.state.offset == state.offset);
	assert(builder.state.flags == state.flags);
	assert(builder.state.frame == state.frame);
	assert(outer.pod.size == outer_size);
	spa_pod_builder_int(&builder, 24);
	assert(spa_pod_builder_pop(&builder, &outer) != NULL);

	spa_fgn_graph_free(graph);
}

static void test_process_failure_state_boundary(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	struct spa_fgn_node_info node = { .struct_size = sizeof(node) };
	struct test_buffer input, output;
	struct spa_buffer *inputs[1], *outputs[1];
	char config[2048];
	uint32_t i;
	int res;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = stateful plugin = \"%s\""
		"   label = stateful-pass-f32 }"
		" { type = ndarray name = failing plugin = \"%s\""
		"   label = fail-marker-f32 }"
		"] links = ["
		" { output = \"stateful:out\" input = \"failing:in\" }"
		"] inputs = [ \"stateful:in\" ] outputs = [ \"failing:out\" ] }",
		plugin, plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	assert(spa_fgn_graph_get_node_info(graph, 0, &node) == 0);
	assert(node.capabilities & SPA_FGN_NODE_CAPABILITY_RESETTABLE);
	init_buffer(&input);
	init_buffer(&output);
	for (i = 0; i < 4; i++)
		input.values[i] = (float)i;
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);

	/* The first node commits its private call count before the downstream node
	 * fails. The graph withholds external output but does not roll that state
	 * back. */
	input.header.flags = SPA_META_HEADER_FLAG_MARKER;
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == -EIO);
	assert(output.chunk.size == 0);

	input.header.flags = 0;
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(output.chunk.size == 4 * sizeof(float));
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] + 2.0f);

	assert(spa_fgn_graph_reset(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] + 1.0f);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
}

static int64_t get_long_property(struct spa_fgn_graph *graph, const char *wanted)
{
	uint8_t data[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	const struct spa_pod_prop *params;
	struct spa_pod *props, *value;
	const char *name;
	int64_t result;
	int res;

	do {
		spa_pod_builder_init(&builder, data, sizeof(data));
		res = spa_fgn_graph_get_props(graph, &builder, &props);
		if (res == -EAGAIN)
			sched_yield();
	} while (res == -EAGAIN);
	assert(res == 1);
	params = spa_pod_find_prop(props, NULL, SPA_PROP_params);
	assert(params != NULL && spa_pod_is_struct(&params->value));
	spa_pod_parser_pod(&parser, &params->value);
	assert(spa_pod_parser_push_struct(&parser, &frame) == 0);
	while (spa_pod_parser_get_string(&parser, &name) == 0) {
		assert(spa_pod_parser_get_pod(&parser, &value) == 0);
		if (strcmp(name, wanted) == 0) {
			assert(spa_pod_get_long(value, &result) == 0);
			return result;
		}
	}
	assert(!"property not found");
	return INT64_MIN;
}

struct publication_stress {
	struct spa_fgn_graph *graph;
	struct test_buffer *parameter;
	_Atomic uint32_t ready;
	_Atomic bool start;
	_Atomic bool done;
	_Atomic int error;
};

static void get_parameter_sequences(struct spa_fgn_graph *graph,
		int64_t *requested, int64_t *active)
{
	uint8_t data[4096];
	struct spa_pod_builder builder;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	const struct spa_pod_prop *params;
	struct spa_pod *props, *value;
	const char *name;
	bool have_requested = false, have_active = false;
	int res;

	do {
		spa_pod_builder_init(&builder, data, sizeof(data));
		res = spa_fgn_graph_get_props(graph, &builder, &props);
		if (res == -EAGAIN)
			sched_yield();
	} while (res == -EAGAIN);
	assert(res == 1);
	params = spa_pod_find_prop(props, NULL, SPA_PROP_params);
	assert(params != NULL && spa_pod_is_struct(&params->value));
	spa_pod_parser_pod(&parser, &params->value);
	assert(spa_pod_parser_push_struct(&parser, &frame) == 0);
	while (spa_pod_parser_get_string(&parser, &name) == 0) {
		assert(spa_pod_parser_get_pod(&parser, &value) == 0);
		if (strcmp(name, "first:requested-parameter-sequence") == 0) {
			assert(spa_pod_get_long(value, requested) == 0);
			have_requested = true;
		} else if (strcmp(name, "first:active-parameter-sequence") == 0) {
			assert(spa_pod_get_long(value, active) == 0);
			have_active = true;
		}
	}
	assert(have_requested && have_active);
}

static void stress_start(struct publication_stress *stress)
{
	atomic_fetch_add_explicit(&stress->ready, 1, memory_order_acq_rel);
	while (!atomic_load_explicit(&stress->start, memory_order_acquire))
		sched_yield();
}

static void *stress_parameter(void *data)
{
	struct publication_stress *stress = data;
	uint64_t sequence;

	stress_start(stress);
	for (sequence = 9; sequence < 2009; sequence++) {
		int res;
		stress->parameter->header.seq = sequence;
		do {
			res = spa_fgn_graph_update_parameter(stress->graph, 1,
					&stress->parameter->buffer);
			if (res == -EBUSY)
				sched_yield();
		} while (res == -EBUSY);
		if (res < 0) {
			atomic_store_explicit(&stress->error, res,
					memory_order_release);
			break;
		}
	}
	atomic_store_explicit(&stress->done, true, memory_order_release);
	return NULL;
}

static void *stress_observer(void *data)
{
	struct publication_stress *stress = data;
	int64_t previous_requested = 0, previous_active = 0;

	stress_start(stress);
	do {
		int64_t requested, active;
		get_parameter_sequences(stress->graph, &requested, &active);
		if (requested < previous_requested || active < previous_active ||
		    active > requested) {
			atomic_store_explicit(&stress->error, -EIO,
					memory_order_release);
			break;
		}
		previous_requested = requested;
		previous_active = active;
	} while (!atomic_load_explicit(&stress->done, memory_order_acquire));
	return NULL;
}

static void stress_publication(struct spa_fgn_graph *graph,
		struct test_buffer *input, struct test_buffer *output,
		struct test_buffer *parameter)
{
	struct publication_stress stress = {
		.graph = graph,
		.parameter = parameter,
	};
	struct spa_buffer *inputs[] = { &input->buffer, NULL };
	struct spa_buffer *outputs[] = { &output->buffer };
	pthread_t parameter_thread, observer_thread;
	int res;

	atomic_init(&stress.ready, 0);
	atomic_init(&stress.start, false);
	atomic_init(&stress.done, false);
	atomic_init(&stress.error, 0);
	assert(pthread_create(&parameter_thread, NULL,
			stress_parameter, &stress) == 0);
	assert(pthread_create(&observer_thread, NULL,
			stress_observer, &stress) == 0);
	while (atomic_load_explicit(&stress.ready, memory_order_acquire) != 2)
		sched_yield();
	atomic_store_explicit(&stress.start, true, memory_order_release);
	while (!atomic_load_explicit(&stress.done, memory_order_acquire)) {
		res = spa_fgn_graph_process(graph, inputs, 2, outputs, 1);
		assert(res == 0 || res == SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	}
	assert(pthread_join(parameter_thread, NULL) == 0);
	assert(pthread_join(observer_thread, NULL) == 0);
	if (atomic_load_explicit(&stress.error, memory_order_acquire) != 0)
		fprintf(stderr, "publication stress failed: %d\n",
				atomic_load_explicit(&stress.error, memory_order_acquire));
	assert(atomic_load_explicit(&stress.error, memory_order_acquire) == 0);
	do {
		res = spa_fgn_graph_process(graph, inputs, 2, outputs, 1);
		assert(res == 0 || res == SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	} while (get_long_property(graph,
			"first:active-parameter-sequence") != 2008);
}

static void test_calculon_plugin(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	const struct spa_fgn_format *format;
	const struct spa_fgn_port_info *port_info;
	const char *node_name;
	struct test_buffer input, output, parameter;
	struct spa_buffer *inputs[2], *outputs[1];
	uint8_t props_data[1024];
	struct spa_pod *props;
	char config[4096];
	int res;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = integrator plugin = \"%s\""
		"   label = leaky-integrator-f32"
		"   config = { gain = 0.5 pole = 0.75"
		"              state = [ 4.0 -4.0 ] } }"
		"] inputs = [ \"integrator:in\" ]"
		" outputs = [ \"integrator:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "Calculon spa_fgn_graph_new failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
			0, &format) == 0);
	assert(format->element_type == SPA_ELEMENT_TYPE_F32_LE);
	assert(format->n_dimensions == 1 && format->shape[0] == 2);
	assert(strcmp(format->schema, "calculon.leaky-integrator.f32/1") == 0);

	init_buffer(&input);
	init_buffer(&output);
	input.values[0] = 2.0f;
	input.values[1] = 2.0f;
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(output.values[0] == 4.0f);
	assert(output.values[1] == -2.0f);
	assert(get_long_property(graph, "integrator:requested-generation") == 0);
	assert(get_long_property(graph, "integrator:active-generation") == 0);
	outputs[0] = &input.buffer;
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == -EINVAL);
	outputs[0] = &output.buffer;

	props = build_two_float_update(props_data, sizeof(props_data),
			"integrator:gain", -0.25f,
			"integrator:pole", 0.5f);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == 0);
	assert(get_long_property(graph, "integrator:requested-generation") == 0);
	assert(spa_fgn_graph_set_props(graph, props) == -EBUSY);
	input.values[0] = 4.0f;
	input.values[1] = -4.0f;
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(output.values[0] == 1.0f);
	assert(output.values[1] == 0.0f);
	assert(get_long_property(graph, "integrator:requested-generation") == 1);
	assert(get_long_property(graph, "integrator:active-generation") == 1);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = power_limit plugin = \"%s\""
		"   label = pdm-command-power-limit-f32"
		"   config = { \"actuator_count\": 2,"
		"              \"max_command_power\": 4.0 } }"
		"] inputs = [ \"power_limit:in\" ]"
		" outputs = [ \"power_limit:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "Calculon PDM power-limit graph failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
			0, &format) == 0);
	assert(format->n_dimensions == 1 && format->shape[0] == 2);
	assert(strcmp(format->schema,
			"org.calculon.ao.demanded-pdm-command/1") == 0);

	init_buffer(&input);
	init_buffer(&output);
	input.values[0] = 3.0f;
	input.values[1] = 4.0f;
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(fabsf(output.values[0] - 1.2f) < 1.0e-6f);
	assert(fabsf(output.values[1] - 1.6f) < 1.0e-6f);
	input.values[0] = NAN;
	output.chunk.size = 2 * sizeof(float);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == -EINVAL);
	assert(output.chunk.size == 0);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = corrector plugin = \"%s\""
		"   label = optical-gain-correction-f32"
		"   config = { \"optical_gains\": [ 0.5, 2.0 ] } }"
		"] inputs = [ \"corrector:in\" \"corrector:parameter\" ]"
		" outputs = [ \"corrector:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "Calculon optical-gain graph failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(spa_fgn_graph_get_n_inputs(graph) == 2);
	assert(spa_fgn_graph_get_port_info(graph, SPA_DIRECTION_INPUT,
			1, &node_name, &port_info) == 0);
	assert(strcmp(node_name, "corrector") == 0);
	assert(strcmp(port_info->name, "parameter") == 0);
	assert((port_info->flags & SPA_FGN_PORT_FLAG_PARAMETER) != 0);

	init_buffer(&input);
	init_buffer(&output);
	init_buffer(&parameter);
	input.values[0] = 1.0f;
	input.values[1] = 4.0f;
	inputs[0] = &input.buffer;
	inputs[1] = NULL;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	assert(output.values[0] == 2.0f);
	assert(output.values[1] == 2.0f);

	parameter.values[0] = 2.0f;
	parameter.values[1] = 4.0f;
	parameter.header.seq = 9;
	res = spa_fgn_graph_update_parameter(graph, 1, &parameter.buffer);
	if (res < 0)
		fprintf(stderr, "Calculon optical-gain parameter failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(get_long_property(graph,
			"corrector:requested-parameter-sequence") == 9);
	assert(get_long_property(graph,
			"corrector:active-parameter-sequence") == 0);
	parameter.values[0] = 8.0f;
	parameter.values[1] = 8.0f;
	assert(spa_fgn_graph_update_parameter(graph, 1, &parameter.buffer) == -EBUSY);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(output.values[0] == 0.5f);
	assert(output.values[1] == 1.0f);
	assert(get_long_property(graph,
			"corrector:active-parameter-sequence") == 9);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = excitation plugin = \"%s\""
		"   label = docrime-excitation-f32"
		"   config = { \"amplitudes\": [ 1.0, 2.0 ], \"seed\": 0 } }"
		"] inputs = [ ]"
		" outputs = [ \"excitation:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "Calculon DO-CRIME excitation graph failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(spa_fgn_graph_get_n_inputs(graph) == 0);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_OUTPUT,
			0, &format) == 0);
	assert(strcmp(format->schema,
			"org.calculon.ao.docrime-excitation/1") == 0);
	init_buffer(&output);
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, NULL, 0, outputs, 1) == 0);
	assert(isfinite(output.values[0]) && fabsf(output.values[0]) < 1.0f);
	assert(isfinite(output.values[1]) && fabsf(output.values[1]) < 2.0f);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = binary_excitation plugin = \"%s\""
		"   label = docrime-binary-excitation-f32"
		"   config = { \"amplitudes\": [ 1.0, 2.0 ], \"seed\": 0 } }"
		"] inputs = [ \"binary_excitation:parameter\" ]"
		" outputs = [ \"binary_excitation:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr,
				"Calculon binary DO-CRIME excitation graph failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	init_buffer(&output);
	init_buffer(&parameter);
	inputs[0] = NULL;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(fabsf(output.values[0]) == 1.0f);
	assert(fabsf(output.values[1]) == 2.0f);

	parameter.values[0] = 0.5f;
	parameter.values[1] = 0.25f;
	parameter.header.seq = 11;
	assert(spa_fgn_graph_update_parameter(graph, 0, &parameter.buffer) == 0);
	assert(get_long_property(graph,
			"binary_excitation:requested-parameter-sequence") == 11);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(fabsf(output.values[0]) == 0.5f);
	assert(fabsf(output.values[1]) == 0.25f);
	assert(get_long_property(graph,
			"binary_excitation:active-parameter-sequence") == 11);
	assert(spa_fgn_graph_reset(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(fabsf(output.values[0]) == 0.5f);
	assert(fabsf(output.values[1]) == 0.25f);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = regions plugin = \"%s\""
		"   label = region-extraction-f32"
		"   config = { image_rows = 2 image_columns = 2"
		"              region_rows = 1 region_columns = 2"
		"              origins = [ [ 0 0 ] [ 1 0 ] ]"
		"              rate = [ 1000 1 ] } }"
		" { type = ndarray name = slopes plugin = \"%s\""
		"   label = shack-hartmann-f32"
		"   config = { region_rows = 1 region_columns = 2"
		"              subaperture_count = 2 coordinate_scale = 1.0"
		"              pixel_threshold = 0.0 flux_threshold = 0.0"
		"              rate = [ 1000 1 ] } }"
		" { type = ndarray name = reconstruct plugin = \"%s\""
		"   label = shwfs-reconstructor-f32"
		"   config = { rows = 1 columns = 4"
		"              matrix = [ 1.0 0.0 0.0 0.0 ]"
		"              rate = [ 1000 1 ] } }"
		" { type = ndarray name = integrate plugin = \"%s\""
		"   label = leaky-integrator-f32"
		"   config = { gain = 1.0 pole = 0.0 state = [ 0.0 ]"
		"              rate = [ 1000 1 ] } }"
		"] links = ["
		" { output = \"regions:regions\" input = \"slopes:regions\" }"
		" { output = \"slopes:slopes\" input = \"reconstruct:slopes\" }"
		" { output = \"reconstruct:reconstructed\" input = \"integrate:in\" }"
		"] inputs = [ \"regions:image\" \"reconstruct:reconstructor\" ]"
		" outputs = [ \"integrate:out\" ] }",
		plugin, plugin, plugin, plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "Calculon SHWFS graph failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(spa_fgn_graph_get_n_inputs(graph) == 2);
	assert(spa_fgn_graph_get_n_outputs(graph) == 1);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
			0, &format) == 0);
	assert(format->n_dimensions == 2 && format->shape[0] == 2 &&
			format->shape[1] == 2);
	assert(format->rate_num == 1000 && format->rate_denom == 1);

	init_buffer(&input);
	init_buffer(&output);
	init_buffer(&parameter);
	input.values[0] = 1.0f;
	input.values[1] = 3.0f;
	input.values[2] = 2.0f;
	input.values[3] = 2.0f;
	inputs[0] = &input.buffer;
	inputs[1] = NULL;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	assert(fabsf(output.values[0] - 0.25f) < 1.0e-6f);

	parameter.values[0] = 0.0f;
	parameter.values[1] = 0.0f;
	parameter.values[2] = 1.0f;
	parameter.values[3] = 0.0f;
	parameter.header.seq = 17;
	assert(spa_fgn_graph_update_parameter(graph, 1,
			&parameter.buffer) == 0);
	assert(get_long_property(graph,
			"reconstruct:requested-parameter-sequence") == 17);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(output.values[0] == 0.0f);
	assert(get_long_property(graph,
			"reconstruct:active-parameter-sequence") == 17);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
}

static void test_conditional_outputs(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	struct spa_fgn_node_info node = { .struct_size = sizeof(node) };
	const struct spa_fgn_format *format;
	struct test_buffer input, output;
	struct spa_buffer *inputs[2], *outputs[1];
	char config[4096];
	uint32_t i;
	int res;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = defer plugin = \"%s\" label = defer-f32 }"
		" { type = ndarray name = scale plugin = \"%s\" label = scale-f32"
		"   config = { shape = [ 4 ] schema = test.deferred/1 } }"
		"] links = [ { output = \"defer:out\" input = \"scale:in\" } ]"
		" inputs = [ \"defer:in\" \"scale:coefficients\" ]"
		" outputs = [ \"scale:out\" ] }",
		plugin, plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	assert(spa_fgn_graph_get_node_info(graph, 0, &node) == 0);
	assert(node.capabilities & SPA_FGN_NODE_CAPABILITY_CONDITIONAL_OUTPUTS);
	init_buffer(&input);
	init_buffer(&output);
	for (i = 0; i < 4; i++) {
		input.values[i] = (float)i + 1.0f;
		output.values[i] = -1.0f;
	}
	inputs[0] = &input.buffer;
	inputs[1] = NULL;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);

	/* The conditional producer succeeds without publishing. Its required
	 * downstream consumer is not called, and absence reaches the graph output. */
	input.header.flags = 0;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	assert(output.chunk.size == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == -1.0f);

	input.header.flags = SPA_META_HEADER_FLAG_MARKER;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	assert(output.chunk.size == 4 * sizeof(float));
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i]);
	assert(output.header.flags & SPA_META_HEADER_FLAG_MARKER);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);

	/* An aggregating node may declare a block-rate input and frame-rate
	 * conditional output. Exact rates are still enforced on every link. */
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = aggregate plugin = \"%s\""
		"   label = aggregate-f32 }"
		"] inputs = [ \"aggregate:in\" ] outputs = [ \"aggregate:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
			0, &format) == 0);
	assert(format->rate_num == 2 && format->rate_denom == 1);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_OUTPUT,
			0, &format) == 0);
	assert(format->rate_num == 1 && format->rate_denom == 1);
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(output.chunk.size == 4 * sizeof(float));
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
}

struct process_thread_context {
	struct spa_fgn_graph *graph;
	struct spa_buffer **inputs;
	struct spa_buffer **outputs;
	int result;
};

static void *process_on_foreign_thread(void *data)
{
	struct process_thread_context *context = data;

	context->result = spa_fgn_graph_process(context->graph,
			context->inputs, 1, context->outputs, 1);
	return NULL;
}

static void test_process_thread_preparation(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	struct spa_fgn_node_info node = { .struct_size = sizeof(node) };
	struct test_buffer input, output;
	struct spa_buffer *inputs[1], *outputs[1];
	struct process_thread_context context;
	pthread_t thread;
	char config[2048];
	uint32_t i;
	int res;

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = prepared plugin = \"%s\""
		"   label = thread-preparation-f32 }"
		"] inputs = [ \"prepared:in\" ] outputs = [ \"prepared:out\" ] }",
		plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	assert(spa_fgn_graph_get_node_info(graph, 0, &node) == 0);
	assert(node.capabilities &
			SPA_FGN_NODE_CAPABILITY_PROCESS_THREAD_PREPARATION);
	init_buffer(&input);
	init_buffer(&output);
	for (i = 0; i < 4; i++) {
		input.values[i] = (float)i + 1.0f;
		output.values[i] = -1.0f;
	}
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;

	assert(spa_fgn_graph_prepare_process_thread(graph) == -EINVAL);
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == -EAGAIN);
	assert(spa_fgn_graph_prepare_process_thread(graph) == 0);
	assert(spa_fgn_graph_prepare_process_thread(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i]);

	context = (struct process_thread_context) {
		.graph = graph,
		.inputs = inputs,
		.outputs = outputs,
	};
	assert(pthread_create(&thread, NULL, process_on_foreign_thread,
			&context) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(context.result == -EPERM);
	assert(output.chunk.size == 0);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == -EAGAIN);
	assert(spa_fgn_graph_prepare_process_thread(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
}

static void reference_dense_f32(const float *matrix, const float *input,
		float *output, uint32_t rows, uint32_t columns)
{
	uint32_t row;

	for (row = 0; row < rows; row++) {
		float sum = 0.0f;
		uint32_t column;

		for (column = 0; column < columns; column++)
			sum += matrix[(size_t)row * columns + column] * input[column];
		output[row] = sum;
	}
}

struct lane_test_context {
	_Atomic uint32_t visited;
	int status[4];
};

static int record_lane(void *data, uint32_t lane, uint32_t n_lanes)
{
	struct lane_test_context *context = data;

	assert(context != NULL);
	assert(n_lanes > 0 && n_lanes <= 4 && lane < n_lanes);
	atomic_fetch_or_explicit(&context->visited, 1u << lane,
			memory_order_relaxed);
	return context->status[lane];
}

static void test_lane_executor(void)
{
	struct spa_fgn_worker_group *group = NULL;
	const struct spa_fgn_executor *executor;
	struct lane_test_context context = { 0 };
	struct spa_fgn_lanes_task task = {
		.struct_size = sizeof(task),
		.data = &context,
		.run = record_lane,
		.n_lanes = 4,
	};

	assert(atomic_is_lock_free(&context.visited));
	assert(spa_fgn_worker_group_new(3, &group) == 0);
	executor = spa_fgn_worker_group_get_executor(group);
	assert(executor != NULL && executor->run_lanes != NULL);
	assert(executor->cache_line_size == SPA_CACHE_LINE_SIZE);
	assert(executor->reserved == 0);
	assert(spa_fgn_worker_group_activate(group) == 0);
	assert(executor->run_lanes(executor->data, &task) == 0);
	assert(atomic_load_explicit(&context.visited,
			memory_order_relaxed) == 0x0fu);

	/* Coordinator failure wins, followed by the lowest helper lane. */
	atomic_store_explicit(&context.visited, 0, memory_order_relaxed);
	context.status[0] = -EIO;
	context.status[1] = -ERANGE;
	assert(executor->run_lanes(executor->data, &task) == -EIO);
	assert(atomic_load_explicit(&context.visited,
			memory_order_relaxed) == 0x0fu);
	context.status[0] = 0;
	assert(executor->run_lanes(executor->data, &task) == -ERANGE);
	context.status[1] = 0;

	/* Malformed tasks are rejected before dispatch. */
	atomic_store_explicit(&context.visited, 0, memory_order_relaxed);
	task.flags = 1;
	assert(executor->run_lanes(executor->data, &task) == -EINVAL);
	task.flags = 0;
	task.n_lanes = 5;
	assert(executor->run_lanes(executor->data, &task) == -EINVAL);
	assert(atomic_load_explicit(&context.visited,
			memory_order_relaxed) == 0);
	task.n_lanes = 4;
	assert(spa_fgn_worker_group_deactivate(group) == 0);
	assert(executor->run_lanes(executor->data, &task) == -EINVAL);
	/* Restart observes the retained generation and cannot replay shutdown. */
	atomic_store_explicit(&context.visited, 0, memory_order_relaxed);
	assert(spa_fgn_worker_group_activate(group) == 0);
	assert(executor->run_lanes(executor->data, &task) == 0);
	assert(atomic_load_explicit(&context.visited,
			memory_order_relaxed) == 0x0fu);
	assert(spa_fgn_worker_group_deactivate(group) == 0);
	spa_fgn_worker_group_free(group);
}

static void test_dense_executor(void)
{
	enum { ROWS = 277, COLUMNS = 376 };
	struct spa_fgn_worker_group *serial = NULL, *parallel = NULL;
	const struct spa_fgn_executor *executor;
	struct spa_fgn_dense_f32_task task;
	float *matrix, *input, *reference, *batch_reference, *storage, *output;
	size_t matrix_bytes = (size_t)ROWS * COLUMNS * sizeof(float);
	size_t output_bytes = (ROWS + 1u) * sizeof(float);
	uint32_t row, column, iteration;

	assert(posix_memalign((void **)&matrix, SPA_CACHE_LINE_SIZE,
			matrix_bytes) == 0);
	assert(posix_memalign((void **)&input, SPA_CACHE_LINE_SIZE,
			COLUMNS * sizeof(float)) == 0);
	assert(posix_memalign((void **)&reference, SPA_CACHE_LINE_SIZE,
			ROWS * sizeof(float)) == 0);
	assert(posix_memalign((void **)&batch_reference, SPA_CACHE_LINE_SIZE,
			ROWS * sizeof(float)) == 0);
	assert(posix_memalign((void **)&storage, SPA_CACHE_LINE_SIZE,
			output_bytes) == 0);
	output = storage + 1; /* Exercise a non-cache-line-aligned output base. */
	for (row = 0; row < ROWS; row++)
		for (column = 0; column < COLUMNS; column++)
			matrix[(size_t)row * COLUMNS + column] =
				(float)((int)((row * 17u + column * 7u) % 29u) - 14) /
				127.0f;
	for (column = 0; column < COLUMNS; column++)
		input[column] = (float)((int)(column % 23u) - 11) / 37.0f;
	reference_dense_f32(matrix, input, reference, ROWS, COLUMNS);

	task = (struct spa_fgn_dense_f32_task) {
		.struct_size = sizeof(task),
		.matrix = matrix,
		.input = input,
		.output = output,
		.n_rows = ROWS,
		.matrix_row_stride = COLUMNS,
		.n_columns = COLUMNS,
	};
	assert(spa_fgn_worker_group_new(0, &serial) == 0);
	assert(spa_fgn_worker_group_activate(serial) == 0);
	assert(spa_fgn_worker_group_run_dense_f32(serial, &task) == 0);
	for (row = 0; row < ROWS; row++)
		assert(fabsf(output[row] - reference[row]) <=
				1.0e-4f * SPA_MAX(fabsf(reference[row]), 1.0f));
	memcpy(reference, output, ROWS * sizeof(float));

	/* Establish the one-lane oracle for three ordered column batches. */
	memset(batch_reference, 0, ROWS * sizeof(float));
	task.output = batch_reference;
	task.n_columns = 113;
	assert(spa_fgn_worker_group_run_dense_f32(serial, &task) == 0);
	task.flags = SPA_FGN_DENSE_F32_FLAG_ACCUMULATE;
	task.first_column = 113;
	task.input = input + 113;
	task.n_columns = 127;
	assert(spa_fgn_worker_group_run_dense_f32(serial, &task) == 0);
	task.first_column = 240;
	task.input = input + 240;
	task.n_columns = COLUMNS - 240;
	assert(spa_fgn_worker_group_run_dense_f32(serial, &task) == 0);
	task = (struct spa_fgn_dense_f32_task) {
		.struct_size = sizeof(task),
		.matrix = matrix,
		.input = input,
		.output = output,
		.n_rows = ROWS,
		.matrix_row_stride = COLUMNS,
		.n_columns = COLUMNS,
	};
	assert(spa_fgn_worker_group_deactivate(serial) == 0);
	assert(spa_fgn_worker_group_run_dense_f32(serial, &task) == -EINVAL);
	spa_fgn_worker_group_free(serial);

	assert(spa_fgn_worker_group_new(3, &parallel) == 0);
	executor = spa_fgn_worker_group_get_executor(parallel);
	assert(executor != NULL && executor->struct_size == sizeof(*executor));
	assert(executor->version == SPA_FGN_EXECUTOR_VERSION);
	assert(executor->n_lanes == 4);
	assert(executor->flags == SPA_FGN_EXECUTOR_FLAG_POLLING);
	assert(spa_fgn_worker_group_activate(parallel) == 0);
	for (iteration = 0; iteration < 64; iteration++) {
		memset(output, 0xa5, ROWS * sizeof(float));
		assert(executor->run_dense_f32(executor->data, &task) == 0);
		assert(memcmp(output, reference, ROWS * sizeof(float)) == 0);
	}

	/* The same primitive admits ordered progressive column batches. */
	memset(output, 0, ROWS * sizeof(float));
	task.n_columns = 113;
	assert(executor->run_dense_f32(executor->data, &task) == 0);
	task.flags = SPA_FGN_DENSE_F32_FLAG_ACCUMULATE;
	task.first_column = 113;
	task.input = input + 113;
	task.n_columns = 127;
	assert(executor->run_dense_f32(executor->data, &task) == 0);
	task.first_column = 240;
	task.input = input + 240;
	task.n_columns = COLUMNS - 240;
	assert(executor->run_dense_f32(executor->data, &task) == 0);
	assert(memcmp(output, batch_reference, ROWS * sizeof(float)) == 0);

	/* Mutable output aliases are rejected before any lane is dispatched. */
	task = (struct spa_fgn_dense_f32_task) {
		.struct_size = sizeof(task),
		.matrix = matrix,
		.input = input,
		.output = input,
		.n_rows = ROWS,
		.matrix_row_stride = COLUMNS,
		.n_columns = COLUMNS,
	};
	assert(executor->run_dense_f32(executor->data, &task) == -EINVAL);
	assert(spa_fgn_worker_group_deactivate(parallel) == 0);
	spa_fgn_worker_group_free(parallel);
	free(storage);
	free(batch_reference);
	free(reference);
	free(input);
	free(matrix);
}

static void test_dense_graph_workers(const char *plugin)
{
	struct spa_fgn_graph *graph = NULL;
	struct test_buffer input, output;
	struct spa_buffer *inputs[] = { &input.buffer };
	struct spa_buffer *outputs[] = { &output.buffer };
	char config[2048];
	uint32_t i;
	int res;

	res = snprintf(config, sizeof(config),
		"{ workers = { helpers = 3 } nodes = ["
		" { type = ndarray name = dense plugin = \"%s\""
		"   label = dense-example-f32 config = { rows = 4 columns = 4 } }"
		"] }", plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	init_buffer(&input);
	init_buffer(&output);
	for (i = 0; i < 4; i++) {
		input.values[i] = (float)i + 0.5f;
		output.values[i] = -1.0f;
	}
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i]);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);

	res = snprintf(config, sizeof(config),
		"{ workers = { helpers = 64 } nodes = ["
		" { type = ndarray name = dense plugin = \"%s\""
		"   label = dense-example-f32 } ] }", plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == -EINVAL);
	assert(graph == NULL);

	res = snprintf(config, sizeof(config),
		"{ workers = { threads = 3 } nodes = ["
		" { type = ndarray name = dense plugin = \"%s\""
		"   label = dense-example-f32 } ] }", plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == -EINVAL);
	assert(graph == NULL);
}

int main(int argc, char *argv[])
{
	struct spa_fgn_graph *graph = NULL;
	struct spa_fgn_graph *invalid_graph = NULL;
	const struct spa_fgn_format *format;
	const struct spa_fgn_port_info *port_info;
	const char *node_name;
	struct test_buffer input, output, parameter;
	struct spa_buffer *inputs[2], *outputs[1];
	struct spa_fgn_parameter_update parameter_updates[2];
	uint8_t props_data[1024];
	struct spa_pod *props;
	char config[8192];
	_Alignas(float) uint8_t misaligned_storage[4 * sizeof(float) + 1];
	struct spa_meta duplicate_metas[2];
	struct spa_meta_header duplicate_headers[2];
	void *saved_data;
	struct spa_meta *saved_metas;
	uint32_t saved_maxsize, saved_n_metas;
	uint32_t i;
	int res;

	assert(argc >= 2 && argc <= 4);
	test_lane_executor();
	test_dense_executor();
	test_graph_inspection(argv[1]);
	if (argc >= 3) {
		check_failed_property_snapshot(argv[2]);
		test_process_failure_state_boundary(argv[2]);
	}
	if (argc == 4 && spa_streq(argv[3], "conditional"))
		test_conditional_outputs(argv[1]);
	if (argc == 4 && spa_streq(argv[3], "conditional"))
		test_dense_graph_workers(argv[1]);
	if (argc == 4 && spa_streq(argv[3], "conditional"))
		test_process_thread_preparation(argv[1]);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = duplicate plugin = \"%s\""
		"   label = scale-f32 props = { gain = 2.0 gain = 3.0 } }"
		"] }",
		argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &invalid_graph) == -EEXIST);
	assert(invalid_graph == NULL);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = construction plugin = \"%s\""
		"   label = scale-f32 props = { extent = 4 } }"
		"] }",
		argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &invalid_graph) == -EPERM);
	assert(invalid_graph == NULL);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = non_object plugin = \"%s\""
		"   label = scale-f32 config = [ 1 2 ] }"
		"] }",
		argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &invalid_graph) == -EINVAL);
	assert(invalid_graph == NULL);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = audio_control plugin = \"%s\""
		"   label = scale-f32 control = { gain = 2.0 } }"
		"] }",
		argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &invalid_graph) == -EINVAL);
	assert(invalid_graph == NULL);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = cycle-first plugin = \"%s\""
		"   label = scale-f32 }"
		" { type = ndarray name = cycle-second plugin = \"%s\""
		"   label = scale-f32 }"
		"] links = ["
		" { output = \"cycle-first:out\" input = \"cycle-second:in\" }"
		" { output = \"cycle-second:out\" input = \"cycle-first:in\" }"
		"] inputs = [ \"cycle-first:coefficients\""
		"             \"cycle-second:coefficients\" ]"
		" outputs = [ \"cycle-second:out\" ] }",
		argv[1], argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &invalid_graph) == -ELOOP);
	assert(invalid_graph == NULL);

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		"  { type = ndarray name = first plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.matrix/1 } }"
		"  { type = ndarray name = second plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.other-matrix/1 } }"
		"] links = [ { output = \"first:out\" input = \"second:in\" } ] }",
		argv[1], argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &invalid_graph);
	if (res == 0) {
		/* A fixed-format plugin may ignore both schema configuration values;
		 * in that case the host sees equal formats and correctly admits them. */
		assert(spa_fgn_graph_get_port_format(invalid_graph,
				SPA_DIRECTION_INPUT, 0, &format) == 0);
		assert(strcmp(format->schema, "test.matrix/1") == 0);
		spa_fgn_graph_free(invalid_graph);
		invalid_graph = NULL;
	} else {
		assert(res == -EINVAL);
		assert(invalid_graph == NULL);
	}

	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		"  { type = ndarray name = first plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.matrix/1 }"
		"    props = { gain = 2.0 } }"
		"  { type = ndarray name = second plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.matrix/1 }"
		"    props = { gain = 3.0 } }"
		"] links = [ { output = \"first:out\" input = \"second:in\" } ]"
		" inputs = [ \"first:in\" \"first:coefficients\" ]"
		" outputs = [ \"second:out\" ] }",
		argv[1], argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "spa_fgn_graph_new failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(graph != NULL);
	assert(spa_fgn_graph_get_n_inputs(graph) == 2);
	assert(spa_fgn_graph_get_n_outputs(graph) == 1);
	assert(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
			0, &format) == 0);
	assert(spa_fgn_graph_get_port_info(graph, SPA_DIRECTION_INPUT,
			1, &node_name, &port_info) == 0);
	assert(strcmp(node_name, "first") == 0);
	assert(strcmp(port_info->name, "coefficients") == 0);
	assert((port_info->flags & SPA_FGN_PORT_FLAG_PARAMETER) != 0);
	assert(format->element_type == SPA_ELEMENT_TYPE_F32_LE);
	assert(format->n_dimensions == 2);
	assert(format->shape[0] == 2 && format->shape[1] == 2);
	assert(strcmp(format->schema, "test.matrix/1") == 0);
	check_properties(graph);
	assert(get_long_property(graph, "first:requested-generation") == 0);
	assert(get_long_property(graph, "second:requested-generation") == 0);
	props = build_mode_update(props_data, sizeof(props_data), 1);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -EBUSY);

	init_buffer(&input);
	init_buffer(&output);
	init_buffer(&parameter);
	for (i = 0; i < 4; i++)
		input.values[i] = (float)i + 1.0f;
	input.header.seq = 42;
	input.header.pts = 1234567;
	inputs[0] = &input.buffer;
	inputs[1] = NULL;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(get_long_property(graph, "first:active-generation") == 1);
	assert(get_long_property(graph, "second:active-generation") == 1);
	assert(spa_fgn_graph_set_props(graph, props) == -EPERM);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 6.0f);
	assert(output.header.seq == input.header.seq);
	assert(output.header.pts == input.header.pts);

	/* External output offsets are caller-owned and survive graph processing. */
	output.values[0] = -123.0f;
	output.chunk.offset = sizeof(float);
	output.chunk.size = 0;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	assert(output.chunk.offset == sizeof(float));
	assert(output.chunk.size == 4 * sizeof(float));
	assert(output.values[0] == -123.0f);
	for (i = 0; i < 4; i++)
		assert(output.values[i + 1] == input.values[i] * 6.0f);
	output.chunk.offset = 0;

	/* A shared descriptor cannot bypass alias admission with a shifted chunk. */
	input.chunk.offset = sizeof(float);
	outputs[0] = &input.buffer;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	input.chunk.offset = 0;
	outputs[0] = &output.buffer;

	/* Reject malformed alignment, metadata bounds, and descriptor aliasing. */
	saved_data = output.data.data;
	output.data.data = &misaligned_storage[1];
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	output.data.data = saved_data;
	saved_n_metas = output.buffer.n_metas;
	output.buffer.n_metas = SPA_FGN_MAX_METAS + 1;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	output.buffer.n_metas = saved_n_metas;
	saved_metas = output.buffer.metas;
	memset(duplicate_headers, 0, sizeof(duplicate_headers));
	duplicate_metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(duplicate_headers[0]),
		.data = &duplicate_headers[0],
	};
	duplicate_metas[1] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(duplicate_headers[1]),
		.data = &duplicate_headers[1],
	};
	output.buffer.metas = duplicate_metas;
	output.buffer.n_metas = 2;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EEXIST);
	output.buffer.metas = saved_metas;
	output.buffer.n_metas = saved_n_metas;
	output.meta.data = input.meta.data;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	output.meta.data = &output.header;

	/* Declared input extents must remain within maxsize after the offset. */
	input.chunk.offset = sizeof(float);
	input.chunk.size = input.data.maxsize;
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EMSGSIZE);
	input.chunk.offset = 0;
	input.chunk.size = 4 * sizeof(float);

	/* Mutable output regions cannot alias either outer pointer array. */
	saved_data = output.data.data;
	saved_maxsize = output.data.maxsize;
	output.data.data = inputs;
	output.data.maxsize = sizeof(inputs);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	output.data.data = outputs;
	output.data.maxsize = 4 * sizeof(float);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == -EINVAL);
	output.data.data = saved_data;
	output.data.maxsize = saved_maxsize;

	for (i = 0; i < 4; i++)
		parameter.values[i] = (float)i + 1.0f;
	parameter.header.seq = 7;
	parameter_updates[0] = (struct spa_fgn_parameter_update) {
		.input_port = 1,
		.buffer = &parameter.buffer,
	};
	assert(spa_fgn_graph_set_parameters(NULL, parameter_updates, 1) == -EINVAL);
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates, 0) == -EINVAL);
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates,
			SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS + 1u) == -E2BIG);
	parameter_updates[0].reserved = 1;
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates, 1) == -EINVAL);
	parameter_updates[0].reserved = 0;
	parameter_updates[0].input_port = 0;
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates, 1) == -EINVAL);
	parameter_updates[0].input_port = 1;
	parameter_updates[1] = parameter_updates[0];
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates, 2) == -EEXIST);
	/* Batch publication is optional for legacy-shaped descriptors. */
	assert(spa_fgn_graph_set_parameters(graph, parameter_updates, 1) == -ENOTSUP);
	assert(spa_fgn_graph_update_parameter(graph, 0, &parameter.buffer) == -EINVAL);
	parameter.chunk.offset = sizeof(float);
	parameter.chunk.size = parameter.data.maxsize;
	assert(spa_fgn_graph_update_parameter(graph, 1,
			&parameter.buffer) == -EMSGSIZE);
	parameter.chunk.offset = 0;
	parameter.chunk.size = 4 * sizeof(float);
	assert(spa_fgn_graph_update_parameter(graph, 1, &parameter.buffer) == 0);
	assert(get_long_property(graph, "first:requested-parameter-sequence") == 7);
	assert(get_long_property(graph, "first:active-parameter-sequence") == 0);
	/* The update owns a copy; the PipeWire buffer can be recycled immediately. */
	for (i = 0; i < 4; i++)
		parameter.values[i] = 9.0f;
	/* Two slots give deterministic back pressure until process() adopts it. */
	assert(spa_fgn_graph_update_parameter(graph, 1, &parameter.buffer) == -EBUSY);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(get_long_property(graph, "first:active-parameter-sequence") == 7);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 6.0f * ((float)i + 1.0f));

	for (i = 0; i < 4; i++)
		parameter.values[i] = 2.0f;
	parameter.header.seq = 8;
	assert(spa_fgn_graph_update_parameter(graph, 1, &parameter.buffer) == 0);
	assert(get_long_property(graph, "first:requested-parameter-sequence") == 8);
	assert(get_long_property(graph, "first:active-parameter-sequence") == 7);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(get_long_property(graph, "first:active-parameter-sequence") == 8);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 12.0f);
	stress_publication(graph, &input, &output, &parameter);

	props = build_empty_update(props_data, sizeof(props_data));
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == 0);
	assert(get_long_property(graph, "first:requested-generation") == 1);
	props = build_odd_update(props_data, sizeof(props_data));
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -EINVAL);
	props = build_non_string_key_update(props_data, sizeof(props_data));
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -EINVAL);
	props = build_duplicate_update(props_data, sizeof(props_data));
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -EEXIST);

	props = build_gain_update(props_data, sizeof(props_data), 4.0f, 5.0f);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == 0);
	/* Publication waits for one graph-cycle boundary. */
	assert(get_long_property(graph, "first:requested-generation") == 1);
	assert(get_long_property(graph, "first:active-generation") == 1);
	/* One requested generation gives deterministic property back pressure. */
	assert(spa_fgn_graph_set_props(graph, props) == -EBUSY);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) ==
			SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	assert(get_long_property(graph, "first:active-generation") == 2);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 40.0f);

	/* Validation is graph-wide: the valid first update is not committed. */
	props = build_gain_update(props_data, sizeof(props_data), 7.0f, 100.0f);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -ERANGE);
	assert(spa_fgn_graph_process(graph, inputs, 2, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 40.0f);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	graph = NULL;

	/* A graph with no external outputs is a valid sink. */
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		" { type = ndarray name = sink plugin = \"%s\" label = scale-f32"
		"   config = { shape = [ 2 2 ] schema = test.matrix/1 } }"
		"] inputs = [ \"sink:in\" ] outputs = [ ] }",
		argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &graph) == 0);
	assert(spa_fgn_graph_get_n_inputs(graph) == 1);
	assert(spa_fgn_graph_get_n_outputs(graph) == 0);
	init_buffer(&input);
	inputs[0] = &input.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, NULL, 0) == 0);
	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	if (argc == 4 && !spa_streq(argv[3], "conditional"))
		test_calculon_plugin(argv[3]);
	return EXIT_SUCCESS;
}
