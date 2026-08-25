/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/pod.h>
#include <spa/utils/json.h>

enum property_id {
	PROPERTY_GAIN,
	PROPERTY_ACTIVE_GAIN,
	PROPERTY_REQUESTED_GENERATION,
	PROPERTY_ACTIVE_GENERATION,
};

struct instance {
	uint32_t shape[SPA_NDARRAY_MAX_DIMENSIONS];
	char schema[256];
	char profile[256];
	struct spa_fgn_format format;
	_Atomic uint64_t requested_state;
	_Atomic uint64_t active_state;
};

struct prepared_props {
	float gain;
};

static uint64_t pack_state(uint32_t generation, float gain)
{
	uint32_t gain_bits;

	memcpy(&gain_bits, &gain, sizeof(gain_bits));
	return ((uint64_t)generation << 32) | gain_bits;
}

static float state_gain(uint64_t state)
{
	uint32_t gain_bits = state;
	float gain;

	memcpy(&gain, &gain_bits, sizeof(gain));
	return gain;
}

static uint32_t state_generation(uint64_t state)
{
	return state >> 32;
}

static const struct spa_fgn_port_info ports[] = {
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 0,
		.direction = SPA_FGN_PORT_INPUT,
		.name = "in",
	},
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 1,
		.direction = SPA_FGN_PORT_OUTPUT,
		.name = "out",
	},
};

static int parse_shape(struct spa_json *parent, const char *token, int len,
		struct instance *instance)
{
	struct spa_json array;
	int value;

	if (!spa_json_is_array(token, len))
		return -EINVAL;
	spa_json_enter(parent, &array);
	while (spa_json_get_int(&array, &value) > 0) {
		if (value <= 0 || instance->format.n_dimensions >=
				SPA_NDARRAY_MAX_DIMENSIONS)
			return -EINVAL;
		instance->shape[instance->format.n_dimensions++] = value;
	}
	return instance->format.n_dimensions == 0 ? -EINVAL : 0;
}

static int instantiate(const struct spa_fgn_descriptor *descriptor SPA_UNUSED,
		const char *config, void **result)
{
	struct instance *instance;
	struct spa_json object;
	char key[256];
	const char *token;
	int len, res;

	if (result == NULL || config == NULL)
		return -EINVAL;
	if ((instance = calloc(1, sizeof(*instance))) == NULL)
		return -ENOMEM;
	instance->format.element_type = SPA_ELEMENT_TYPE_F32_LE;
	instance->format.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR;
	instance->format.shape = instance->shape;
	atomic_init(&instance->requested_state, pack_state(0, 1.0f));
	atomic_init(&instance->active_state, pack_state(0, 1.0f));

	if (spa_json_begin_object(&object, config, strlen(config)) <= 0) {
		res = -EINVAL;
		goto error;
	}
	while ((len = spa_json_object_next(&object, key, sizeof(key), &token)) > 0) {
		if (spa_streq(key, "shape")) {
			if ((res = parse_shape(&object, token, len, instance)) < 0)
				goto error;
		} else if (spa_streq(key, "schema")) {
			if (spa_json_parse_stringn(token, len, instance->schema,
					sizeof(instance->schema)) <= 0) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "profile")) {
			if (spa_json_parse_stringn(token, len, instance->profile,
					sizeof(instance->profile)) <= 0) {
				res = -EINVAL;
				goto error;
			}
		} else {
			res = -EINVAL;
			goto error;
		}
	}
	if (instance->format.n_dimensions == 0) {
		instance->format.n_dimensions = 1;
		instance->shape[0] = 4;
	}
	instance->format.schema = instance->schema[0] != '\0'
		? instance->schema : NULL;
	instance->format.profile = instance->profile[0] != '\0'
		? instance->profile : NULL;
	*result = instance;
	return 0;
error:
	free(instance);
	return res;
}

static void cleanup(void *data)
{
	free(data);
}

static int get_port_format(void *data, uint32_t port,
		const struct spa_fgn_format **format)
{
	struct instance *instance = data;

	if (instance == NULL || format == NULL || port >= SPA_N_ELEMENTS(ports))
		return -EINVAL;
	*format = &instance->format;
	return 0;
}

static struct spa_fgn_value float_value(float value)
{
	return (struct spa_fgn_value) {
		.type = SPA_TYPE_Float,
		.value.float_value = value,
	};
}

static struct spa_fgn_value long_value(int64_t value)
{
	return (struct spa_fgn_value) {
		.type = SPA_TYPE_Long,
		.value.long_integer = value,
	};
}

static int enum_prop_info(void *data, uint32_t index,
		struct spa_fgn_property_info *info)
{
	struct instance *instance = data;
	uint64_t state;
	static const char *const names[] = {
		"gain", "active-gain", "requested-generation", "active-generation",
	};
	static const char *const descriptions[] = {
		"Requested scalar gain",
		"Scalar gain adopted by process()",
		"Generation published by the control thread",
		"Generation adopted by process()",
	};

	if (instance == NULL || info == NULL)
		return -EINVAL;
	if (index >= SPA_N_ELEMENTS(names))
		return 0;
	*info = (struct spa_fgn_property_info) {
		.struct_size = sizeof(*info),
		.id = index,
		.flags = index == PROPERTY_GAIN ? SPA_FGN_PROPERTY_FLAG_RANGE
			: SPA_FGN_PROPERTY_FLAG_READONLY,
		.name = names[index],
		.description = descriptions[index],
	};
	if (index == PROPERTY_GAIN) {
		state = atomic_load_explicit(&instance->requested_state,
				memory_order_acquire);
		info->default_value = float_value(state_gain(state));
		info->minimum = float_value(-16.0f);
		info->maximum = float_value(16.0f);
	} else if (index == PROPERTY_ACTIVE_GAIN) {
		state = atomic_load_explicit(&instance->active_state,
				memory_order_acquire);
		info->default_value = float_value(state_gain(state));
	} else {
		info->default_value = long_value(0);
	}
	return 1;
}

static int get_prop(void *data, uint32_t id, struct spa_fgn_value *value)
{
	struct instance *instance = data;
	uint64_t state;

	if (instance == NULL || value == NULL)
		return -EINVAL;
	switch (id) {
	case PROPERTY_GAIN:
		state = atomic_load_explicit(&instance->requested_state,
				memory_order_acquire);
		*value = float_value(state_gain(state));
		break;
	case PROPERTY_ACTIVE_GAIN:
		state = atomic_load_explicit(&instance->active_state,
				memory_order_acquire);
		*value = float_value(state_gain(state));
		break;
	case PROPERTY_REQUESTED_GENERATION:
		state = atomic_load_explicit(&instance->requested_state,
				memory_order_acquire);
		*value = long_value(state_generation(state));
		break;
	case PROPERTY_ACTIVE_GENERATION:
		state = atomic_load_explicit(&instance->active_state,
				memory_order_acquire);
		*value = long_value(state_generation(state));
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int prepare_props(void *data, const struct spa_fgn_property *properties,
		uint32_t n_properties, void **result)
{
	struct instance *instance = data;
	struct prepared_props *prepared;
	uint32_t i;

	if (instance == NULL || result == NULL ||
	    (n_properties > 0 && properties == NULL))
		return -EINVAL;
	if ((prepared = malloc(sizeof(*prepared))) == NULL)
		return -ENOMEM;
	prepared->gain = state_gain(atomic_load_explicit(&instance->requested_state,
			memory_order_acquire));
	for (i = 0; i < n_properties; i++) {
		if (properties[i].id != PROPERTY_GAIN ||
		    properties[i].value.type != SPA_TYPE_Float ||
		    !isfinite(properties[i].value.value.float_value) ||
		    properties[i].value.value.float_value < -16.0f ||
		    properties[i].value.value.float_value > 16.0f) {
			free(prepared);
			return -EINVAL;
		}
		prepared->gain = properties[i].value.value.float_value;
	}
	*result = prepared;
	return 0;
}

static void commit_props(void *data, void *prepared_data)
{
	struct instance *instance = data;
	struct prepared_props *prepared = prepared_data;
	uint64_t current;

	current = atomic_load_explicit(&instance->requested_state,
			memory_order_relaxed);
	atomic_store_explicit(&instance->requested_state,
			pack_state(state_generation(current) + 1, prepared->gain),
			memory_order_release);
	free(prepared);
}

static void discard_props(void *data SPA_UNUSED, void *prepared)
{
	free(prepared);
}

static void copy_metadata(struct spa_buffer *output, const struct spa_buffer *input)
{
	uint32_t i;

	for (i = 0; i < output->n_metas; i++) {
		struct spa_meta *source = spa_buffer_find_meta(input,
				output->metas[i].type);
		if (source != NULL && source->data != NULL &&
		    output->metas[i].data != NULL)
			memmove(output->metas[i].data, source->data,
					SPA_MIN(source->size, output->metas[i].size));
	}
}

static int process(void *data, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	struct instance *instance = data;
	const struct spa_data *input_data;
	struct spa_data *output_data;
	const float *input;
	float *output;
	uint64_t state;
	float gain;
	uint32_t i, n_values = 1;

	if (instance == NULL || inputs == NULL || outputs == NULL ||
	    n_inputs != 1 || n_outputs != 1 || inputs[0].buffer == NULL ||
	    outputs[0].buffer == NULL)
		return -EINVAL;
	state = atomic_load_explicit(&instance->requested_state,
			memory_order_acquire);
	atomic_store_explicit(&instance->active_state, state, memory_order_release);
	gain = state_gain(state);
	for (i = 0; i < instance->format.n_dimensions; i++)
		n_values *= instance->shape[i];
	input_data = &inputs[0].buffer->datas[0];
	output_data = &outputs[0].buffer->datas[0];
	input = SPA_PTROFF(input_data->data, input_data->chunk->offset, const float);
	output = SPA_PTROFF(output_data->data, output_data->chunk->offset, float);
	for (i = 0; i < n_values; i++)
		output[i] = input[i] * gain;
	output_data->chunk->size = n_values * sizeof(float);
	copy_metadata(outputs[0].buffer, inputs[0].buffer);
	return 0;
}

static const struct spa_fgn_descriptor descriptor = {
	.struct_size = sizeof(struct spa_fgn_descriptor),
	.version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "scale-f32",
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.instantiate = instantiate,
	.cleanup = cleanup,
	.get_port_format = get_port_format,
	.enum_prop_info = enum_prop_info,
	.get_prop = get_prop,
	.prepare_props = prepare_props,
	.commit_props = commit_props,
	.discard_props = discard_props,
	.process = process,
};

static const struct spa_fgn_descriptor *find_descriptor(const char *name)
{
	return name != NULL && spa_streq(name, descriptor.name) ? &descriptor : NULL;
}

static const struct spa_fgn_plugin plugin = {
	.struct_size = sizeof(struct spa_fgn_plugin),
	.abi_version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "example-c",
	.find_descriptor = find_descriptor,
};

SPA_EXPORT
const struct spa_fgn_plugin *spa_filter_graph_ndarray_plugin_get_interface(
		uint32_t abi_version)
{
	return abi_version == SPA_FGN_PLUGIN_ABI_VERSION ? &plugin : NULL;
}
