/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
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
	PROPERTY_REQUESTED_PARAMETER_SEQUENCE,
	PROPERTY_ACTIVE_PARAMETER_SEQUENCE,
	PROPERTY_MODE,
};

#define PARAMETER_SLOT_NONE UINT32_MAX
#define PROPERTY_WRITER_BITS 2u
#define PROPERTY_WRITER_MASK ((UINT64_C(1) << PROPERTY_WRITER_BITS) - 1u)
#define PROPERTY_WRITER_END (PROPERTY_WRITER_MASK)

struct parameter_slot {
	float *values;
	uint64_t sequence;
};

struct prepared_props {
	float gain;
};

struct prepared_parameter {
	uint32_t slot;
};

struct instance {
	uint32_t shape[SPA_NDARRAY_MAX_DIMENSIONS];
	char schema[256];
	char profile[256];
	struct spa_fgn_format format;
	uint32_t n_values;
	_Atomic uint64_t requested_state;
	_Atomic uint64_t active_state;
	struct parameter_slot parameter_slots[2];
	_Atomic uint32_t requested_parameter_slot;
	_Atomic uint32_t active_parameter_slot;
	_Atomic uint64_t requested_parameter_sequence;
	_Atomic uint64_t active_parameter_sequence;
	_Atomic uint64_t property_publication;
	_Atomic bool prepared_props_busy;
	struct prepared_props prepared_props;
	_Atomic bool prepared_parameter_busy;
	struct prepared_parameter prepared_parameter;
};

static void property_write_begin(struct instance *instance)
{
	atomic_fetch_add_explicit(&instance->property_publication, 1,
			memory_order_seq_cst);
}

static void property_write_end(struct instance *instance)
{
	/* Increment the completed generation and decrement the writer count in one
	 * indivisible publication-state transition. Callback ownership admits at
	 * most one control writer and one process writer. */
	atomic_fetch_add_explicit(&instance->property_publication,
			PROPERTY_WRITER_END,
			memory_order_seq_cst);
}

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
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 2,
		.direction = SPA_FGN_PORT_INPUT,
		.flags = SPA_FGN_PORT_FLAG_OPTIONAL | SPA_FGN_PORT_FLAG_PARAMETER,
		.name = "coefficients",
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
	size_t n_values = 1;
	uint32_t i, slot;
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
	for (i = 0; i < instance->format.n_dimensions; i++) {
		if (n_values > UINT32_MAX / instance->shape[i]) {
			res = -EOVERFLOW;
			goto error;
		}
		n_values *= instance->shape[i];
	}
	if (n_values > UINT32_MAX / sizeof(float)) {
		res = -EOVERFLOW;
		goto error;
	}
	instance->n_values = (uint32_t)n_values;
	for (slot = 0; slot < SPA_N_ELEMENTS(instance->parameter_slots); slot++) {
		if ((instance->parameter_slots[slot].values =
				calloc(n_values, sizeof(float))) == NULL) {
			res = -ENOMEM;
			goto error;
		}
		for (i = 0; i < n_values; i++)
			instance->parameter_slots[slot].values[i] = 1.0f;
	}
	atomic_init(&instance->requested_parameter_slot, PARAMETER_SLOT_NONE);
	atomic_init(&instance->active_parameter_slot, 0);
	atomic_init(&instance->requested_parameter_sequence, 0);
	atomic_init(&instance->active_parameter_sequence, 0);
	atomic_init(&instance->property_publication, 0);
	atomic_init(&instance->prepared_props_busy, false);
	atomic_init(&instance->prepared_parameter_busy, false);
	instance->format.schema = instance->schema[0] != '\0'
		? instance->schema : NULL;
	instance->format.profile = instance->profile[0] != '\0'
		? instance->profile : NULL;
	*result = instance;
	return 0;
error:
	for (slot = 0; slot < SPA_N_ELEMENTS(instance->parameter_slots); slot++)
		free(instance->parameter_slots[slot].values);
	free(instance);
	return res;
}

static void cleanup(void *data)
{
	struct instance *instance = data;
	uint32_t slot;

	if (instance == NULL)
		return;
	for (slot = 0; slot < SPA_N_ELEMENTS(instance->parameter_slots); slot++)
		free(instance->parameter_slots[slot].values);
	free(instance);
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

static struct spa_fgn_value id_value(uint32_t value)
{
	return (struct spa_fgn_value) {
		.type = SPA_TYPE_Id,
		.value.id = value,
	};
}

static const struct spa_fgn_property_choice mode_choices[] = {
	{
		.struct_size = sizeof(struct spa_fgn_property_choice),
		.value = SPA_FGN_VALUE_ID_INIT(0),
		.name = "normal",
		.description = "Normal",
	},
	{
		.struct_size = sizeof(struct spa_fgn_property_choice),
		.value = SPA_FGN_VALUE_ID_INIT(1),
		.name = "diagnostic",
		.description = "Diagnostic",
	},
};

static const struct spa_fgn_property_info property_infos[] = {
	[PROPERTY_GAIN] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_GAIN,
		SPA_FGN_PROPERTY_FLAG_RUNTIME | SPA_FGN_PROPERTY_FLAG_RANGE,
		"gain", "Requested scalar gain", "1",
		SPA_FGN_VALUE_FLOAT_INIT(1.0f),
		SPA_FGN_VALUE_FLOAT_INIT(-16.0f),
		SPA_FGN_VALUE_FLOAT_INIT(16.0f), 0, NULL),
	[PROPERTY_ACTIVE_GAIN] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_ACTIVE_GAIN, SPA_FGN_PROPERTY_FLAG_READONLY,
		"active-gain", "Scalar gain adopted by process()", "1",
		SPA_FGN_VALUE_FLOAT_INIT(1.0f),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	[PROPERTY_REQUESTED_GENERATION] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_REQUESTED_GENERATION, SPA_FGN_PROPERTY_FLAG_READONLY,
		"requested-generation", "Generation published by the control thread", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	[PROPERTY_ACTIVE_GENERATION] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_ACTIVE_GENERATION, SPA_FGN_PROPERTY_FLAG_READONLY,
		"active-generation", "Generation adopted by process()", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	[PROPERTY_REQUESTED_PARAMETER_SEQUENCE] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_REQUESTED_PARAMETER_SEQUENCE, SPA_FGN_PROPERTY_FLAG_READONLY,
		"requested-parameter-sequence",
		"Sequence of the latest accepted coefficient update", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	[PROPERTY_ACTIVE_PARAMETER_SEQUENCE] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_ACTIVE_PARAMETER_SEQUENCE, SPA_FGN_PROPERTY_FLAG_READONLY,
		"active-parameter-sequence", "Coefficient sequence adopted by process()", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
	[PROPERTY_MODE] = SPA_FGN_PROPERTY_INFO_INIT(
		PROPERTY_MODE,
		SPA_FGN_PROPERTY_FLAG_READONLY | SPA_FGN_PROPERTY_FLAG_CHOICES,
		"mode", "Example enumerated execution mode", "1",
		SPA_FGN_VALUE_ID_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT,
		SPA_N_ELEMENTS(mode_choices), mode_choices),
};

static int enum_prop_info(void *data, uint32_t index,
		struct spa_fgn_property_info *info)
{
	if (data == NULL)
		return -EINVAL;
	return spa_fgn_enum_prop_info_table(property_infos,
			SPA_N_ELEMENTS(property_infos), index, info);
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
	case PROPERTY_REQUESTED_PARAMETER_SEQUENCE: {
		*value = long_value(atomic_load_explicit(
				&instance->requested_parameter_sequence,
				memory_order_acquire));
		break;
	}
	case PROPERTY_ACTIVE_PARAMETER_SEQUENCE: {
		*value = long_value(atomic_load_explicit(
				&instance->active_parameter_sequence,
				memory_order_acquire));
		break;
	}
	case PROPERTY_MODE:
		*value = id_value(0);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static uint64_t get_prop_revision(void *data)
{
	struct instance *instance = data;
	uint64_t publication;

	if (instance == NULL)
		return 0;
	publication = atomic_load_explicit(&instance->property_publication,
			memory_order_seq_cst);
	return (publication >> PROPERTY_WRITER_BITS) * 2u +
		((publication & PROPERTY_WRITER_MASK) != 0);
}

static int prepare_props(void *data, const struct spa_fgn_property *properties,
		uint32_t n_properties, void **result)
{
	struct instance *instance = data;
	struct prepared_props *prepared;
	uint64_t requested, active;
	uint32_t i;
	bool expected = false;
	bool gain_seen = false;

	if (instance == NULL || result == NULL ||
	    (n_properties > 0 && properties == NULL))
		return -EINVAL;
	requested = atomic_load_explicit(&instance->requested_state,
			memory_order_acquire);
	active = atomic_load_explicit(&instance->active_state,
			memory_order_acquire);
	if (state_generation(requested) != state_generation(active))
		return -EBUSY;
	if (state_generation(requested) == UINT32_MAX)
		return -EOVERFLOW;
	if (!atomic_compare_exchange_strong_explicit(
			&instance->prepared_props_busy, &expected, true,
			memory_order_acquire, memory_order_relaxed))
		return -EBUSY;
	prepared = &instance->prepared_props;
	prepared->gain = state_gain(requested);
	for (i = 0; i < n_properties; i++) {
		if (gain_seen) {
			atomic_store_explicit(&instance->prepared_props_busy, false,
					memory_order_release);
			return -EEXIST;
		}
		if (properties[i].id != PROPERTY_GAIN ||
		    properties[i].value.type != SPA_TYPE_Float ||
		    !isfinite(properties[i].value.value.float_value) ||
		    properties[i].value.value.float_value < -16.0f ||
		    properties[i].value.value.float_value > 16.0f) {
			atomic_store_explicit(&instance->prepared_props_busy, false,
					memory_order_release);
			return -EINVAL;
		}
		prepared->gain = properties[i].value.value.float_value;
		gain_seen = true;
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
	property_write_begin(instance);
	atomic_store_explicit(&instance->requested_state,
			pack_state(state_generation(current) + 1, prepared->gain),
			memory_order_release);
	property_write_end(instance);
	atomic_store_explicit(&instance->prepared_props_busy, false,
			memory_order_release);
}

static void discard_props(void *data, void *prepared)
{
	struct instance *instance = data;

	if (instance != NULL && prepared == &instance->prepared_props)
		atomic_store_explicit(&instance->prepared_props_busy, false,
				memory_order_release);
}

static int prepare_parameter(void *data, uint32_t port,
		const struct spa_fgn_buffer *buffer, void **result)
{
	struct instance *instance = data;
	const struct spa_meta_header *header;
	const struct spa_data *spa_data;
	const float *values;
	uint32_t active, slot, i;
	uint64_t sequence;

	if (instance == NULL || port != 2 || buffer == NULL ||
	    buffer->buffer == NULL || result == NULL)
		return -EINVAL;
	if (atomic_load_explicit(&instance->requested_parameter_slot,
			memory_order_acquire) != PARAMETER_SLOT_NONE)
		return -EBUSY;
	spa_data = &buffer->buffer->datas[0];
	values = SPA_PTROFF(spa_data->data, spa_data->chunk->offset, const float);
	for (i = 0; i < instance->n_values; i++)
		if (!isfinite(values[i]))
			return -EINVAL;
	header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header,
			sizeof(*header));
	if (header != NULL) {
		if (header->seq > INT64_MAX)
			return -EOVERFLOW;
		sequence = header->seq;
	} else {
		sequence = atomic_load_explicit(
				&instance->requested_parameter_sequence,
				memory_order_acquire);
		if (sequence >= INT64_MAX)
			return -EOVERFLOW;
		sequence++;
	}
	if (atomic_exchange_explicit(&instance->prepared_parameter_busy, true,
			memory_order_acquire))
		return -EBUSY;
	active = atomic_load_explicit(&instance->active_parameter_slot,
			memory_order_acquire);
	slot = active ^ 1u;
	memcpy(instance->parameter_slots[slot].values, values,
			instance->n_values * sizeof(float));
	instance->parameter_slots[slot].sequence = sequence;
	instance->prepared_parameter.slot = slot;
	*result = &instance->prepared_parameter;
	return 0;
}

static void commit_parameter(void *data, void *prepared_data)
{
	struct instance *instance = data;
	struct prepared_parameter *prepared = prepared_data;
	uint64_t sequence = instance->parameter_slots[prepared->slot].sequence;

	property_write_begin(instance);
	atomic_store_explicit(&instance->requested_parameter_sequence, sequence,
			memory_order_release);
	atomic_store_explicit(&instance->requested_parameter_slot, prepared->slot,
			memory_order_release);
	property_write_end(instance);
	atomic_store_explicit(&instance->prepared_parameter_busy, false,
			memory_order_release);
}

static void discard_parameter(void *data, void *prepared)
{
	struct instance *instance = data;

	if (instance != NULL && prepared == &instance->prepared_parameter)
		atomic_store_explicit(&instance->prepared_parameter_busy, false,
				memory_order_release);
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
	uint32_t parameter_slot, requested_parameter_slot;
	uint32_t i;
	bool state_changed, parameter_changed;

	if (instance == NULL || inputs == NULL || outputs == NULL ||
	    n_inputs != 2 || n_outputs != 1 || inputs[0].buffer == NULL ||
	    inputs[1].buffer != NULL ||
	    outputs[0].buffer == NULL)
		return -EINVAL;
	state = atomic_load_explicit(&instance->requested_state,
			memory_order_acquire);
	state_changed = state != atomic_load_explicit(&instance->active_state,
			memory_order_relaxed);
	requested_parameter_slot = atomic_load_explicit(
			&instance->requested_parameter_slot, memory_order_acquire);
	parameter_changed = requested_parameter_slot != PARAMETER_SLOT_NONE;
	if (state_changed || parameter_changed)
		property_write_begin(instance);
	if (state_changed)
		atomic_store_explicit(&instance->active_state, state,
				memory_order_release);
	gain = state_gain(state);
	if (parameter_changed) {
		atomic_store_explicit(&instance->active_parameter_slot,
				requested_parameter_slot, memory_order_release);
		atomic_store_explicit(&instance->active_parameter_sequence,
				instance->parameter_slots[requested_parameter_slot].sequence,
				memory_order_release);
		atomic_store_explicit(&instance->requested_parameter_slot,
				PARAMETER_SLOT_NONE, memory_order_release);
	}
	if (state_changed || parameter_changed)
		property_write_end(instance);
	parameter_slot = atomic_load_explicit(&instance->active_parameter_slot,
			memory_order_acquire);
	input_data = &inputs[0].buffer->datas[0];
	output_data = &outputs[0].buffer->datas[0];
	input = SPA_PTROFF(input_data->data, input_data->chunk->offset, const float);
	output = SPA_PTROFF(output_data->data, output_data->chunk->offset, float);
	for (i = 0; i < instance->n_values; i++)
		output[i] = input[i] * gain *
			instance->parameter_slots[parameter_slot].values[i];
	output_data->chunk->size = instance->n_values * sizeof(float);
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
	.get_prop_revision = get_prop_revision,
	.prepare_props = prepare_props,
	.commit_props = commit_props,
	.discard_props = discard_props,
	.prepare_parameter = prepare_parameter,
	.commit_parameter = commit_parameter,
	.discard_parameter = discard_parameter,
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
