/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/pod.h>

static const uint32_t shape[] = { 1 };

static const struct spa_fgn_format format = {
	.element_type = SPA_ELEMENT_TYPE_F32_LE,
	.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
	.n_dimensions = 1,
	.shape = shape,
};

static const struct spa_fgn_port_info ports[] = {
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 0,
		.direction = SPA_DIRECTION_INPUT,
		.name = "in",
	},
};

static const struct spa_fgn_property_info properties[] = {
	SPA_FGN_PROPERTY_INFO_INIT(
		0, SPA_FGN_PROPERTY_FLAG_READONLY,
		"value", "Test value", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
};

static int instantiate(const struct spa_fgn_descriptor *descriptor,
		const char *config, const struct spa_fgn_executor *executor,
		void **result)
{
	if (descriptor == NULL || config == NULL || executor == NULL ||
	    executor->version != SPA_FGN_EXECUTOR_VERSION || result == NULL)
		return -EINVAL;
	*result = (void *)descriptor;
	return 0;
}

static void cleanup(void *instance SPA_UNUSED)
{
}

static int get_port_format(void *instance, uint32_t port,
		const struct spa_fgn_format **result)
{
	if (instance == NULL || port != 0 || result == NULL)
		return -EINVAL;
	*result = &format;
	return 0;
}

static int enum_prop_info(void *instance, uint32_t index,
		struct spa_fgn_property_info *info)
{
	if (instance == NULL)
		return -EINVAL;
	return spa_fgn_enum_prop_info_table(properties,
			SPA_N_ELEMENTS(properties), index, info);
}

static int get_prop(void *instance, uint32_t id, struct spa_fgn_value *value)
{
	if (instance == NULL || id != 0 || value == NULL)
		return -EINVAL;
	*value = (struct spa_fgn_value)SPA_FGN_VALUE_LONG_INIT(0);
	return 0;
}

static uint64_t get_prop_revision(void *instance SPA_UNUSED)
{
	return 1;
}

static int process(void *instance, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	if (instance == NULL || inputs == NULL || n_inputs != 1 ||
	    outputs != NULL || n_outputs != 0)
		return -EINVAL;
	return 0;
}

static const struct spa_fgn_descriptor descriptor = {
	.struct_size = sizeof(struct spa_fgn_descriptor),
	.version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "unstable-properties",
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.instantiate = instantiate,
	.cleanup = cleanup,
	.get_port_format = get_port_format,
	.enum_prop_info = enum_prop_info,
	.get_prop = get_prop,
	.get_prop_revision = get_prop_revision,
	.process = process,
};

enum flow_kind {
	FLOW_STATEFUL,
	FLOW_FAIL_MARKER,
};

struct flow_instance {
	enum flow_kind kind;
	uint32_t calls;
};

static const uint32_t flow_shape[] = { 4 };

static const struct spa_fgn_format flow_format = {
	.element_type = SPA_ELEMENT_TYPE_F32_LE,
	.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
	.n_dimensions = 1,
	.shape = flow_shape,
	.schema = "test.failure-boundary/1",
};

static const struct spa_fgn_port_info flow_ports[] = {
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 0,
		.direction = SPA_DIRECTION_INPUT,
		.name = "in",
	},
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 1,
		.direction = SPA_DIRECTION_OUTPUT,
		.name = "out",
	},
};

static int flow_instantiate(const struct spa_fgn_descriptor *descriptor,
		const char *config, const struct spa_fgn_executor *executor,
		void **result)
{
	struct flow_instance *instance;

	if (descriptor == NULL || config == NULL || executor == NULL ||
	    executor->version != SPA_FGN_EXECUTOR_VERSION || result == NULL ||
	    strcmp(config, "{}") != 0)
		return -EINVAL;
	if ((instance = calloc(1, sizeof(*instance))) == NULL)
		return -ENOMEM;
	if (strcmp(descriptor->name, "stateful-pass-f32") == 0)
		instance->kind = FLOW_STATEFUL;
	else if (strcmp(descriptor->name, "fail-marker-f32") == 0)
		instance->kind = FLOW_FAIL_MARKER;
	else {
		free(instance);
		return -EINVAL;
	}
	*result = instance;
	return 0;
}

static int flow_get_port_format(void *instance, uint32_t port,
		const struct spa_fgn_format **result)
{
	if (instance == NULL || port >= SPA_N_ELEMENTS(flow_ports) || result == NULL)
		return -EINVAL;
	*result = &flow_format;
	return 0;
}

static int flow_reset(void *data)
{
	struct flow_instance *instance = data;

	if (instance == NULL)
		return -EINVAL;
	instance->calls = 0;
	return 0;
}

static int flow_process(void *data, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	struct flow_instance *instance = data;
	const struct spa_meta_header *input_header;
	struct spa_meta_header *output_header;
	const struct spa_data *input_data;
	struct spa_data *output_data;
	const float *input;
	float *output;
	uint32_t i;

	if (instance == NULL || inputs == NULL || outputs == NULL ||
	    n_inputs != 1 || n_outputs != 1 || inputs[0].buffer == NULL ||
	    outputs[0].buffer == NULL)
		return -EINVAL;
	input_header = spa_buffer_find_meta_data(inputs[0].buffer, SPA_META_Header,
			sizeof(*input_header));
	if (instance->kind == FLOW_FAIL_MARKER && input_header != NULL &&
	    (input_header->flags & SPA_META_HEADER_FLAG_MARKER))
		return -EIO;
	input_data = &inputs[0].buffer->datas[0];
	output_data = &outputs[0].buffer->datas[0];
	input = SPA_PTROFF(input_data->data, input_data->chunk->offset, const float);
	output = SPA_PTROFF(output_data->data, output_data->chunk->offset, float);
	if (instance->kind == FLOW_STATEFUL)
		instance->calls++;
	for (i = 0; i < flow_shape[0]; i++)
		output[i] = input[i] + (float)instance->calls;
	output_header = spa_buffer_find_meta_data(outputs[0].buffer, SPA_META_Header,
			sizeof(*output_header));
	if (input_header != NULL && output_header != NULL)
		*output_header = *input_header;
	output_data->chunk->size = flow_shape[0] * sizeof(float);
	return 0;
}

#define FLOW_DESCRIPTOR(label_) \
	{ \
		.struct_size = sizeof(struct spa_fgn_descriptor), \
		.version = SPA_FGN_PLUGIN_ABI_VERSION, \
		.name = label_, \
		.n_ports = SPA_N_ELEMENTS(flow_ports), \
		.ports = flow_ports, \
		.instantiate = flow_instantiate, \
		.cleanup = free, \
		.get_port_format = flow_get_port_format, \
		.reset = flow_reset, \
		.process = flow_process, \
	}

static const struct spa_fgn_descriptor flow_descriptors[] = {
	FLOW_DESCRIPTOR("stateful-pass-f32"),
	FLOW_DESCRIPTOR("fail-marker-f32"),
};

static const struct spa_fgn_descriptor *find_descriptor(const char *name)
{
	uint32_t i;

	if (name == NULL)
		return NULL;
	if (strcmp(name, descriptor.name) == 0)
		return &descriptor;
	for (i = 0; i < SPA_N_ELEMENTS(flow_descriptors); i++)
		if (strcmp(name, flow_descriptors[i].name) == 0)
			return &flow_descriptors[i];
	return NULL;
}

static const struct spa_fgn_plugin plugin = {
	.struct_size = sizeof(struct spa_fgn_plugin),
	.abi_version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "test-unstable-properties",
	.find_descriptor = find_descriptor,
};

SPA_EXPORT
const struct spa_fgn_plugin *spa_filter_graph_ndarray_plugin_get_interface(
		uint32_t abi_version)
{
	return abi_version == SPA_FGN_PLUGIN_ABI_VERSION ? &plugin : NULL;
}
