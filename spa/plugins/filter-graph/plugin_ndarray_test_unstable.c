/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/pod.h>
#include <spa/utils/json.h>

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

struct recovering_instance {
	_Atomic uint64_t revision;
	_Atomic int64_t value;
	_Atomic int64_t mirror;
	_Atomic uint32_t process_calls;
	_Atomic uint32_t odd_revision_reads;
};

static const struct spa_fgn_property_info recovering_properties[] = {
	SPA_FGN_PROPERTY_INFO_INIT(
		0, SPA_FGN_PROPERTY_FLAG_READONLY,
		"value", "Test value", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	SPA_FGN_PROPERTY_INFO_INIT(
		1, SPA_FGN_PROPERTY_FLAG_READONLY,
		"mirror", "Test mirror", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
};

static int recovering_instantiate(const struct spa_fgn_descriptor *descriptor,
		const char *config, const struct spa_fgn_executor *executor,
		void **result)
{
	struct recovering_instance *instance;

	if (descriptor == NULL || config == NULL || executor == NULL ||
	    executor->version != SPA_FGN_EXECUTOR_VERSION || result == NULL ||
	    strcmp(config, "{}") != 0)
		return -EINVAL;
	if ((instance = calloc(1, sizeof(*instance))) == NULL)
		return -ENOMEM;
	atomic_init(&instance->revision, 0);
	atomic_init(&instance->value, 0);
	atomic_init(&instance->mirror, 0);
	atomic_init(&instance->process_calls, 0);
	atomic_init(&instance->odd_revision_reads, 0);
	*result = instance;
	return 0;
}

static int recovering_enum_prop_info(void *instance, uint32_t index,
		struct spa_fgn_property_info *info)
{
	if (instance == NULL)
		return -EINVAL;
	return spa_fgn_enum_prop_info_table(recovering_properties,
			SPA_N_ELEMENTS(recovering_properties), index, info);
}

static int recovering_get_prop(void *data, uint32_t id,
		struct spa_fgn_value *value)
{
	struct recovering_instance *instance = data;

	if (instance == NULL || value == NULL || id >= 2)
		return -EINVAL;
	*value = (struct spa_fgn_value)SPA_FGN_VALUE_LONG_INIT(
			id == 0
				? atomic_load_explicit(&instance->value, memory_order_acquire)
				: atomic_load_explicit(&instance->mirror, memory_order_acquire));
	return 0;
}

static uint64_t recovering_get_prop_revision(void *data)
{
	struct recovering_instance *instance = data;
	uint64_t revision = atomic_load_explicit(&instance->revision,
			memory_order_acquire);

	/* The first odd read closes process(); the second is the main-loop
	 * snapshot that the live test waits for before completing the write. */
	if ((revision & 1u) != 0 &&
	    atomic_fetch_add_explicit(&instance->odd_revision_reads, 1,
			memory_order_acq_rel) == 1) {
		puts("ODD_SNAPSHOT revision=1");
		fflush(stdout);
	}
	return revision;
}

static int recovering_process(void *data, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	struct recovering_instance *instance = data;
	uint32_t call;

	if (instance == NULL || inputs == NULL || n_inputs != 1 ||
	    inputs[0].buffer == NULL || outputs != NULL || n_outputs != 0)
		return -EINVAL;
	call = atomic_fetch_add_explicit(&instance->process_calls, 1,
			memory_order_acq_rel) + 1;
	if (call == 1) {
		atomic_store_explicit(&instance->revision, 1, memory_order_seq_cst);
		atomic_store_explicit(&instance->value, 2, memory_order_seq_cst);
	} else if (call == 2) {
		atomic_store_explicit(&instance->mirror, 20, memory_order_release);
		atomic_store_explicit(&instance->revision, 2, memory_order_release);
	}
	return 0;
}

static const struct spa_fgn_descriptor recovering_descriptor = {
	.struct_size = sizeof(struct spa_fgn_descriptor),
	.version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "recovering-properties",
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.instantiate = recovering_instantiate,
	.cleanup = free,
	.get_port_format = get_port_format,
	.enum_prop_info = recovering_enum_prop_info,
	.get_prop = recovering_get_prop,
	.get_prop_revision = recovering_get_prop_revision,
	.process = recovering_process,
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

/* Prepared requirements share static geometry, but belong to each instance. */
static const struct spa_meta_requirement prepared_metas[] = {
	{ SPA_META_AcquisitionIdentity, sizeof(struct spa_meta_acquisition_identity), 0, 0 },
	{ SPA_META_ElectricalTrigger, sizeof(struct spa_meta_electrical_trigger), SPA_META_REQUIREMENT_REQUIRED, 0 },
};
static const struct spa_fgn_port_info prepared_input[] = {{
	.struct_size = sizeof(struct spa_fgn_port_info), .index = 0,
	.direction = SPA_DIRECTION_INPUT, .name = "in",
	.n_metas = SPA_N_ELEMENTS(prepared_metas), .metas = prepared_metas,
}};
static const struct spa_fgn_port_info prepared_output[] = {{
	.struct_size = sizeof(struct spa_fgn_port_info), .index = 0,
	.direction = SPA_DIRECTION_OUTPUT, .name = "out",
	.n_metas = SPA_N_ELEMENTS(prepared_metas), .metas = prepared_metas,
}};
static const struct spa_fgn_port_info prepared_output_missing[] = {{
	.struct_size = sizeof(struct spa_fgn_port_info), .index = 0,
	.direction = SPA_DIRECTION_OUTPUT, .name = "out",
	.n_metas = 1, .metas = &prepared_metas[1],
}};
struct prepared_metadata_instance {
	struct spa_meta_requirement metas[SPA_N_ELEMENTS(prepared_metas)];
	char mode[32];
};
static int prepared_metadata_instantiate(const struct spa_fgn_descriptor *descriptor SPA_UNUSED,
		const char *config, const struct spa_fgn_executor *executor SPA_UNUSED,
		void **result)
{
	struct prepared_metadata_instance *instance = calloc(1, sizeof(*instance));
	struct spa_json object;
	const char *token;
	char key[32];
	int len;
	if (instance == NULL)
		return -ENOMEM;
	memcpy(instance->metas, prepared_metas, sizeof(prepared_metas));
	if (spa_json_begin_object(&object, config, strlen(config)) <= 0)
		goto invalid;
	while ((len = spa_json_object_next(&object, key, sizeof(key), &token)) > 0) {
		if (!spa_streq(key, "mode") ||
		    spa_json_parse_stringn(token, len, instance->mode, sizeof(instance->mode)) <= 0)
			goto invalid;
	}
	if (len < 0)
		goto invalid;
	if (spa_streq(instance->mode, "strict"))
		instance->metas[0].flags |= SPA_META_REQUIREMENT_REQUIRED;
	else if (spa_streq(instance->mode, "bad-size"))
		instance->metas[0].size++;
	else if (spa_streq(instance->mode, "drop-required"))
		instance->metas[1].flags = 0;
	else if (spa_streq(instance->mode, "duplicate"))
		instance->metas[1] = instance->metas[0];
	else if (spa_streq(instance->mode, "unknown-id"))
		instance->metas[0].type = SPA_META_ReferenceTimestamp;
	else if (spa_streq(instance->mode, "reserved"))
		instance->metas[0].reserved = 1;
	else if (spa_streq(instance->mode, "unknown-flags"))
		instance->metas[0].flags = 2;
	else if (spa_streq(instance->mode, "reordered")) {
		instance->metas[0] = prepared_metas[1];
		instance->metas[1] = prepared_metas[0];
	}
	*result = instance;
	return 0;
invalid:
	free(instance);
	return -EINVAL;
}
static int prepared_metadata_get(void *opaque, uint32_t port,
		const struct spa_meta_requirement **metas, uint32_t *n_metas)
{
	struct prepared_metadata_instance *instance = opaque;
	if (port != 0 || instance == NULL || metas == NULL || n_metas == NULL)
		return -EINVAL;
	if (spa_streq(instance->mode, "failure"))
		return -EIO;
	*metas = spa_streq(instance->mode, "null") ? NULL : instance->metas;
	*n_metas = spa_streq(instance->mode, "bounds") ? UINT32_MAX :
		spa_streq(instance->mode, "drop-count") ? 1 : SPA_N_ELEMENTS(prepared_metas);
	return 0;
}
#define PREPARED_INPUT_DESCRIPTOR(label, descriptor_size, callback) { \
	.struct_size = descriptor_size, .version = SPA_FGN_PLUGIN_ABI_VERSION, \
	.name = label, .n_ports = SPA_N_ELEMENTS(prepared_input), .ports = prepared_input, \
	.instantiate = prepared_metadata_instantiate, .cleanup = free, \
	.get_port_format = get_port_format, .process = process, .get_port_metadata = callback, \
}
#define PREPARED_OUTPUT_DESCRIPTOR(label, table) { \
	.struct_size = sizeof(struct spa_fgn_descriptor), .version = SPA_FGN_PLUGIN_ABI_VERSION, \
	.name = label, .n_ports = SPA_N_ELEMENTS(table), .ports = table, \
	.instantiate = instantiate, .cleanup = cleanup, \
	.get_port_format = get_port_format, .process = process, \
}
static const struct spa_fgn_descriptor prepared_descriptors[] = {
	PREPARED_INPUT_DESCRIPTOR("prepared-metadata", sizeof(struct spa_fgn_descriptor), prepared_metadata_get),
	PREPARED_INPUT_DESCRIPTOR("prepared-metadata-null", sizeof(struct spa_fgn_descriptor), NULL),
	PREPARED_INPUT_DESCRIPTOR("prepared-metadata-short", offsetof(struct spa_fgn_descriptor, get_port_metadata), prepared_metadata_get),
	PREPARED_OUTPUT_DESCRIPTOR("metadata-producer", prepared_output),
	PREPARED_OUTPUT_DESCRIPTOR("metadata-producer-missing", prepared_output_missing),
};

static const struct spa_fgn_descriptor *find_descriptor(const char *name)
{
	uint32_t i;

	if (name == NULL)
		return NULL;
	if (strcmp(name, descriptor.name) == 0)
		return &descriptor;
	if (strcmp(name, recovering_descriptor.name) == 0)
		return &recovering_descriptor;
	for (i = 0; i < SPA_N_ELEMENTS(flow_descriptors); i++)
		if (strcmp(name, flow_descriptors[i].name) == 0)
			return &flow_descriptors[i];
	for (i = 0; i < SPA_N_ELEMENTS(prepared_descriptors); i++)
		if (strcmp(name, prepared_descriptors[i].name) == 0)
			return &prepared_descriptors[i];
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
