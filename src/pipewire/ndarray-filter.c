/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/keys.h>
#include <pipewire/main-loop.h>
#include <pipewire/ndarray-filter.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>

#define MAX_METAS 16u
#define MAX_META_BYTES 4096u
#define MAX_BUFFER_REGIONS (6u + MAX_METAS)
struct memory_region {
	uintptr_t start;
	uintptr_t end;
};

struct pw_ndarray_filter;

struct ndarray_port {
	struct pw_ndarray_filter *filter;
	uint32_t index;
	uint32_t data_index;
	uint32_t direction;
	uint32_t flags;
	char *name;
	uint32_t *shape;
	char *schema;
	struct pw_ndarray_filter_format format;
	size_t size;
	int32_t stride;
	void *filter_port;
	_Atomic(struct pw_buffer *) pending_parameter;
	_Atomic bool parameter_scheduled;
	_Atomic bool retry_parameter;
	_Atomic bool completed_parameter;
	struct pw_ndarray_filter_buffer parameter_view;
};

struct port_data {
	struct ndarray_port *port;
};

struct pw_ndarray_filter {
	struct pw_main_loop *main_loop;
	struct pw_filter *filter;
	struct spa_source *error_event;
	struct pw_thread_loop *parameter_loop;
	struct spa_source *parameter_event;
	bool parameter_loop_started;

	char *node_name;
	char *remote_name;
	struct pw_ndarray_filter_events events;
	void *user_data;
	uint32_t flags;

	struct ndarray_port *ports;
	uint32_t n_ports;
	struct ndarray_port **inputs;
	struct ndarray_port **data_inputs;
	struct ndarray_port **outputs;
	uint32_t n_inputs;
	uint32_t n_data_inputs;
	uint32_t n_parameter_inputs;
	uint32_t n_outputs;

	struct pw_buffer **input_buffers;
	bool *input_available;
	struct pw_buffer **output_buffers;
	struct pw_ndarray_filter_buffer *process_inputs;
	struct pw_ndarray_filter_buffer *process_outputs;
	struct memory_region *buffer_regions;
	uint32_t *n_buffer_regions;

	_Atomic int state;
	_Atomic int error;
	_Atomic bool prepared;
	_Atomic bool destroying;
	bool initialized;
	bool connected;
};

static int checked_format_size(const struct pw_ndarray_filter_format *format,
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

static bool strings_equal(const char *first, const char *second)
{
	return first == second ||
		(first != NULL && second != NULL && spa_streq(first, second));
}

static int valid_name(const char *name)
{
	size_t length;

	if (name == NULL || name[0] == '\0')
		return -EINVAL;
	length = strlen(name);
	return length <= PW_NDARRAY_FILTER_NAME_MAX ? 0 : -ENAMETOOLONG;
}

static int copy_port(struct ndarray_port *destination,
		const struct pw_ndarray_filter_port *source)
{
	int res;

	if (source == NULL || source->struct_size < sizeof(*source) ||
	    (source->flags & ~PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) != 0 ||
	    source->reserved != 0 ||
	    (source->direction != SPA_DIRECTION_INPUT &&
	     source->direction != SPA_DIRECTION_OUTPUT) ||
	    ((source->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) &&
	     (source->direction != SPA_DIRECTION_INPUT ||
	      source->format.rate_denom != 0)) ||
	    (source->format.schema != NULL && source->format.schema[0] == '\0'))
		return -EINVAL;
	if ((res = valid_name(source->name)) < 0 ||
	    (res = checked_format_size(&source->format,
		    &destination->size, &destination->stride)) < 0)
		return res;

	destination->direction = source->direction;
	destination->flags = source->flags;
	destination->name = strdup(source->name);
	destination->shape = malloc(source->format.n_dimensions *
			sizeof(*destination->shape));
	if (destination->name == NULL || destination->shape == NULL)
		return -ENOMEM;
	memcpy(destination->shape, source->format.shape,
			source->format.n_dimensions * sizeof(*destination->shape));
	if (source->format.schema != NULL &&
	    (destination->schema = strdup(source->format.schema)) == NULL)
		return -ENOMEM;
	destination->format = source->format;
	destination->format.shape = destination->shape;
	destination->format.schema = destination->schema;
	return 0;
}

static void clear_port(struct ndarray_port *port)
{
	free(port->schema);
	free(port->shape);
	free(port->name);
}

static int copy_config(struct pw_ndarray_filter *filter,
		const struct pw_ndarray_filter_config *config)
{
	uint32_t i, input = 0, data_input = 0, parameter_input = 0, output = 0;
	int res;

	if (config == NULL || config->struct_size < sizeof(*config) ||
	    config->version != PW_VERSION_NDARRAY_FILTER_CONFIG ||
	    (config->flags & ~(PW_NDARRAY_FILTER_FLAG_RT_PROCESS |
		    PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS)) != 0 ||
	    config->n_ports == 0 ||
	    config->n_ports > PW_NDARRAY_FILTER_MAX_PORTS ||
	    config->ports == NULL || config->events == NULL ||
	    config->events->version != PW_VERSION_NDARRAY_FILTER_EVENTS ||
	    config->events->process == NULL ||
	    (config->remote_name != NULL && config->remote_name[0] == '\0'))
		return -EINVAL;
	if ((res = valid_name(config->node_name)) < 0)
		return res;

	filter->node_name = strdup(config->node_name);
	if (config->remote_name != NULL)
		filter->remote_name = strdup(config->remote_name);
	filter->ports = calloc(config->n_ports, sizeof(*filter->ports));
	if (filter->node_name == NULL ||
	    (config->remote_name != NULL && filter->remote_name == NULL) ||
	    filter->ports == NULL)
		return -ENOMEM;
	filter->events = *config->events;
	filter->user_data = config->user_data;
	filter->flags = config->flags;
	filter->n_ports = config->n_ports;

	for (i = 0; i < config->n_ports; i++) {
		uint32_t j;

		filter->ports[i].filter = filter;
		if ((res = copy_port(&filter->ports[i], &config->ports[i])) < 0)
			return res;
		for (j = 0; j < i; j++)
			if (filter->ports[j].direction == filter->ports[i].direction &&
			    spa_streq(filter->ports[j].name, filter->ports[i].name))
				return -EEXIST;
		if (filter->ports[i].direction == SPA_DIRECTION_INPUT) {
			filter->ports[i].index = input++;
			if (filter->ports[i].flags &
			    PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) {
				filter->ports[i].data_index = UINT32_MAX;
				parameter_input++;
			} else {
				filter->ports[i].data_index = data_input++;
			}
		} else {
			filter->ports[i].index = output++;
			filter->ports[i].data_index = UINT32_MAX;
		}
		atomic_init(&filter->ports[i].pending_parameter, NULL);
		atomic_init(&filter->ports[i].parameter_scheduled, false);
		atomic_init(&filter->ports[i].retry_parameter, false);
		atomic_init(&filter->ports[i].completed_parameter, false);
	}
	if (parameter_input > 0 && config->events->update_parameter == NULL)
		return -EINVAL;
	filter->n_inputs = input;
	filter->n_data_inputs = data_input;
	filter->n_parameter_inputs = parameter_input;
	filter->n_outputs = output;

	if ((input > 0 &&
	     (filter->inputs = calloc(input, sizeof(*filter->inputs))) == NULL) ||
	    (data_input > 0 &&
	     ((filter->data_inputs = calloc(data_input,
		      sizeof(*filter->data_inputs))) == NULL ||
	      (filter->input_buffers = calloc(data_input,
		      sizeof(*filter->input_buffers))) == NULL ||
	      (filter->input_available = calloc(data_input,
		      sizeof(*filter->input_available))) == NULL ||
	      (filter->process_inputs = calloc(data_input,
		      sizeof(*filter->process_inputs))) == NULL)) ||
	    (output > 0 &&
	     ((filter->outputs = calloc(output, sizeof(*filter->outputs))) == NULL ||
	      (filter->output_buffers = calloc(output,
		      sizeof(*filter->output_buffers))) == NULL ||
	      (filter->process_outputs = calloc(output,
		      sizeof(*filter->process_outputs))) == NULL)) ||
	    (data_input + output > 0 &&
	     ((filter->buffer_regions = calloc(
		      (data_input + output) * MAX_BUFFER_REGIONS,
		      sizeof(*filter->buffer_regions))) == NULL ||
	      (filter->n_buffer_regions = calloc(data_input + output,
		      sizeof(*filter->n_buffer_regions))) == NULL)))
		return -ENOMEM;

	for (i = 0; i < config->n_ports; i++) {
		struct ndarray_port *port = &filter->ports[i];

		if (port->direction == SPA_DIRECTION_INPUT) {
			filter->inputs[port->index] = port;
			if (!(port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER))
				filter->data_inputs[port->data_index] = port;
		} else {
			filter->outputs[port->index] = port;
		}
	}
	return 0;
}

static struct spa_pod *build_format(struct spa_pod_builder *builder,
		uint32_t id, const struct pw_ndarray_filter_format *format)
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

static int add_port(struct pw_ndarray_filter *filter,
		struct ndarray_port *port)
{
	uint8_t storage[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	const struct spa_pod *params[4];
	struct pw_properties *properties;
	struct port_data *data;
	uint32_t i;

	properties = pw_properties_new(
			PW_KEY_PORT_NAME, port->name,
			PW_KEY_MEDIA_TYPE, "Application",
			NULL);
	if (properties == NULL)
		return -ENOMEM;
	if (port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER)
		pw_properties_set(properties, PW_KEY_PORT_CONTROL, "true");
	params[0] = build_format(&builder, SPA_PARAM_EnumFormat, &port->format);
	params[1] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)port->size),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(port->stride),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
				(1u << SPA_DATA_MemFd)));
	params[2] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[3] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
	for (i = 0; i < SPA_N_ELEMENTS(params); i++)
		if (params[i] == NULL) {
			pw_properties_free(properties);
			return -ENOSPC;
		}
	data = pw_filter_add_port(filter->filter,
			port->direction == SPA_DIRECTION_INPUT
				? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(*data), properties,
			params, SPA_N_ELEMENTS(params));
	if (data == NULL)
		return errno != 0 ? -errno : -ENOMEM;
	data->port = port;
	port->filter_port = data;
	return 0;
}

static int validate_port_format(struct ndarray_port *port,
		const struct spa_pod *param)
{
	struct spa_ndarray_info actual;
	const char *schema;
	uint32_t i;
	int res;

	if (port == NULL || param == NULL)
		return -EINVAL;
	if ((res = spa_format_ndarray_parse(param, &actual)) < 0 ||
	    (res = spa_format_ndarray_parse_string(param,
		    SPA_FORMAT_NDARRAY_schema, &schema)) < 0)
		return res;
	if ((uint32_t)actual.element_type != port->format.element_type ||
	    (uint32_t)actual.layout != port->format.layout ||
	    actual.rate.num != port->format.rate_num ||
	    actual.rate.denom != port->format.rate_denom ||
	    actual.n_dimensions != port->format.n_dimensions ||
	    !strings_equal(schema, port->format.schema))
		return -EINVAL;
	for (i = 0; i < actual.n_dimensions; i++)
		if (actual.shape[i] != port->format.shape[i])
			return -EINVAL;
	return 0;
}

static int memory_region_init(struct memory_region *region,
		const void *pointer, size_t size)
{
	uintptr_t start = (uintptr_t)pointer;

	if (size == 0) {
		region->start = region->end = 0;
		return 0;
	}
	if (pointer == NULL || start > UINTPTR_MAX - size)
		return -EOVERFLOW;
	region->start = start;
	region->end = start + size;
	return 0;
}

static bool memory_regions_overlap(const struct memory_region *first,
		const struct memory_region *second)
{
	return first->start != first->end && second->start != second->end &&
		first->start < second->end && second->start < first->end;
}

static int collect_buffer_regions(struct pw_buffer *pw_buffer,
		const struct ndarray_port *port, bool output,
		struct memory_region regions[MAX_BUFFER_REGIONS], uint32_t *n_regions)
{
	struct spa_buffer *buffer;
	struct spa_data *data;
	size_t metadata_bytes = 0;
	uintptr_t payload;
	uint32_t element_size, count = 0, i, j;
	int res;

#define ADD_REGION(pointer, region_size) \
	do { \
		if ((res = memory_region_init(&regions[count], \
				(pointer), (region_size))) < 0) \
			return res; \
		count++; \
	} while (0)

	if (pw_buffer == NULL || (buffer = pw_buffer->buffer) == NULL ||
	    buffer->n_datas != 1 || buffer->datas == NULL ||
	    (uintptr_t)buffer->datas % _Alignof(struct spa_data) != 0 ||
	    buffer->n_metas > MAX_METAS ||
	    (buffer->n_metas != 0 &&
	     (buffer->metas == NULL ||
	      (uintptr_t)buffer->metas % _Alignof(struct spa_meta) != 0)))
		return -EINVAL;
	data = &buffer->datas[0];
	element_size = spa_element_type_size(port->format.element_type);
	if (data->data == NULL || data->chunk == NULL ||
	    (uintptr_t)data->chunk % _Alignof(struct spa_chunk) != 0 ||
	    data->chunk->offset >= data->maxsize ||
	    data->maxsize - data->chunk->offset < port->size ||
	    (uintptr_t)data->data > UINTPTR_MAX - data->chunk->offset)
		return -ENOSPC;
	payload = (uintptr_t)data->data + data->chunk->offset;
	if (payload % element_size != 0)
		return -EINVAL;
	if (!output && (data->chunk->size < port->size ||
	    data->chunk->size > data->maxsize - data->chunk->offset))
		return -EMSGSIZE;
	for (i = 0; i < buffer->n_metas; i++) {
		const struct spa_meta *meta = &buffer->metas[i];

		if ((meta->size != 0 && meta->data == NULL) ||
		    meta->size > MAX_META_BYTES ||
		    metadata_bytes > MAX_META_BYTES - meta->size)
			return -EINVAL;
		metadata_bytes += meta->size;
		for (j = 0; j < i; j++)
			if (buffer->metas[j].type == meta->type)
				return -EEXIST;
		if (meta->type == SPA_META_Header &&
		    (meta->size < sizeof(struct spa_meta_header) ||
		     (uintptr_t)meta->data % _Alignof(struct spa_meta_header) != 0))
			return -EINVAL;
		if (meta->type == SPA_META_Acquisition &&
		    (meta->size < sizeof(struct spa_meta_acquisition) ||
		     (uintptr_t)meta->data % _Alignof(struct spa_meta_acquisition) != 0))
			return -EINVAL;
	}
	ADD_REGION(pw_buffer, sizeof(*pw_buffer));
	ADD_REGION(buffer, sizeof(*buffer));
	ADD_REGION(buffer->datas, sizeof(*buffer->datas));
	ADD_REGION(data->chunk, sizeof(*data->chunk));
	ADD_REGION(buffer->metas, buffer->n_metas * sizeof(*buffer->metas));
	ADD_REGION((void *)payload, port->size);
	for (i = 0; i < buffer->n_metas; i++)
		ADD_REGION(buffer->metas[i].data, buffer->metas[i].size);
	*n_regions = count;
	return 0;
#undef ADD_REGION
}

static int validate_regions(struct pw_ndarray_filter *filter)
{
	struct memory_region input_views, output_views;
	uint32_t i, j, first, second;
	int res;

	if ((res = memory_region_init(&input_views, filter->process_inputs,
		filter->n_data_inputs * sizeof(*filter->process_inputs))) < 0 ||
	    (res = memory_region_init(&output_views, filter->process_outputs,
		filter->n_outputs * sizeof(*filter->process_outputs))) < 0)
		return res;
	for (i = 0; i < filter->n_data_inputs + filter->n_outputs; i++) {
		struct memory_region *regions = &filter->buffer_regions[
			i * MAX_BUFFER_REGIONS];

		for (first = 0; first < filter->n_buffer_regions[i]; first++) {
			if (memory_regions_overlap(&regions[first], &input_views) ||
			    memory_regions_overlap(&regions[first], &output_views))
				return -EINVAL;
			for (second = 0; second < first; second++)
				if (memory_regions_overlap(&regions[first], &regions[second]))
					return -EINVAL;
		}
		for (j = 0; j < i; j++) {
			struct memory_region *other = &filter->buffer_regions[
				j * MAX_BUFFER_REGIONS];

			for (first = 0; first < filter->n_buffer_regions[i]; first++)
				for (second = 0;
				     second < filter->n_buffer_regions[j]; second++)
					if (memory_regions_overlap(&regions[first],
						    &other[second]))
						return -EINVAL;
		}
	}
	return 0;
}

static int project_buffer(struct pw_buffer *pw_buffer,
		const struct ndarray_port *port, bool output,
		struct pw_ndarray_filter_buffer *view)
{
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_data *data = &buffer->datas[0];
	struct spa_meta *meta;

	memset(view, 0, sizeof(*view));
	view->struct_size = sizeof(*view);
	view->data = SPA_PTROFF(data->data, data->chunk->offset, void);
	view->size = (uint32_t)port->size;
	view->capacity = data->maxsize - data->chunk->offset;
	spa_meta_acquisition_init(&view->acquisition);

	if ((meta = spa_buffer_find_meta(buffer, SPA_META_Header)) != NULL) {
		view->metadata_available |= PW_NDARRAY_FILTER_METADATA_HEADER;
		if (!output) {
			memcpy(&view->header, meta->data, sizeof(view->header));
			view->metadata_valid |= PW_NDARRAY_FILTER_METADATA_HEADER;
		}
	}
	if ((meta = spa_buffer_find_meta(buffer, SPA_META_Acquisition)) != NULL) {
		if (!output && !spa_meta_acquisition_is_valid(meta))
			return -EINVAL;
		view->metadata_available |= PW_NDARRAY_FILTER_METADATA_ACQUISITION;
		if (!output) {
			memcpy(&view->acquisition, meta->data, sizeof(view->acquisition));
			view->metadata_valid |= PW_NDARRAY_FILTER_METADATA_ACQUISITION;
		}
	}
	return 0;
}

static void project_unavailable_input(struct pw_ndarray_filter_buffer *view)
{
	memset(view, 0, sizeof(*view));
	view->struct_size = sizeof(*view);
	view->flags = PW_NDARRAY_FILTER_BUFFER_FLAG_INPUT_UNAVAILABLE;
}

static int validate_completed_output(const struct ndarray_port *port,
		struct pw_buffer *pw_buffer,
		const struct pw_ndarray_filter_buffer *view)
{
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_data *data = &buffer->datas[0];
	struct spa_meta acquisition_meta;
	uint32_t metadata_available = 0;
	void *expected = SPA_PTROFF(data->data, data->chunk->offset, void);

	if (spa_buffer_find_meta(buffer, SPA_META_Header) != NULL)
		metadata_available |= PW_NDARRAY_FILTER_METADATA_HEADER;
	if (spa_buffer_find_meta(buffer, SPA_META_Acquisition) != NULL)
		metadata_available |= PW_NDARRAY_FILTER_METADATA_ACQUISITION;

	if (view->struct_size < sizeof(*view) ||
	    view->flags & ~PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE ||
	    view->data != expected || view->size != port->size ||
	    view->capacity != data->maxsize - data->chunk->offset ||
	    view->metadata_available != metadata_available ||
	    view->metadata_valid & ~view->metadata_available)
		return -EINVAL;
	if (view->metadata_valid & PW_NDARRAY_FILTER_METADATA_ACQUISITION) {
		acquisition_meta = (struct spa_meta) {
			.type = SPA_META_Acquisition,
			.size = sizeof(view->acquisition),
			.data = (void *)&view->acquisition,
		};
		if (!spa_meta_acquisition_is_valid(&acquisition_meta))
			return -EINVAL;
	}
	return 0;
}

static void commit_output(const struct ndarray_port *port,
		struct pw_buffer *pw_buffer,
		const struct pw_ndarray_filter_buffer *view)
{
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_data *data = &buffer->datas[0];
	struct spa_meta *meta;

	data->chunk->size = (uint32_t)port->size;
	data->chunk->stride = port->stride;
	data->chunk->flags = 0;
	if ((meta = spa_buffer_find_meta(buffer, SPA_META_Header)) != NULL) {
		if (view->metadata_valid & PW_NDARRAY_FILTER_METADATA_HEADER)
			memcpy(meta->data, &view->header, sizeof(view->header));
		else
			memset(meta->data, 0, sizeof(struct spa_meta_header));
	}
	if ((meta = spa_buffer_find_meta(buffer, SPA_META_Acquisition)) != NULL) {
		if (view->metadata_valid & PW_NDARRAY_FILTER_METADATA_ACQUISITION)
			memcpy(meta->data, &view->acquisition, sizeof(view->acquisition));
		else
			spa_meta_acquisition_init(meta->data);
	}
}

static void recycle_inputs(struct pw_ndarray_filter *filter)
{
	uint32_t i;

	for (i = 0; i < filter->n_data_inputs; i++)
		if (filter->input_buffers[i] != NULL) {
			pw_filter_queue_buffer(filter->data_inputs[i]->filter_port,
					filter->input_buffers[i]);
			filter->input_buffers[i] = NULL;
			filter->input_available[i] = false;
		}
}

static void clear_output_chunks(struct pw_ndarray_filter *filter)
{
	uint32_t i;

	for (i = 0; i < filter->n_outputs; i++) {
		struct pw_buffer *buffer = filter->output_buffers[i];

		if (buffer != NULL && buffer->buffer != NULL &&
		    buffer->buffer->n_datas > 0 && buffer->buffer->datas != NULL &&
		    buffer->buffer->datas[0].chunk != NULL)
			buffer->buffer->datas[0].chunk->size = 0;
	}
}

static void signal_process_error(struct pw_ndarray_filter *filter, int res)
{
	int expected = 0;

	if (res >= 0)
		res = -EPROTO;
	if (atomic_compare_exchange_strong_explicit(&filter->error, &expected, res,
			memory_order_acq_rel, memory_order_relaxed) &&
	    filter->error_event != NULL)
		pw_loop_signal_event(pw_main_loop_get_loop(filter->main_loop),
				filter->error_event);
}

static int project_parameter_buffer(struct ndarray_port *port,
		struct pw_buffer *buffer)
{
	struct memory_region regions[MAX_BUFFER_REGIONS], view;
	uint32_t n_regions, first, second;
	int res;

	if ((res = collect_buffer_regions(buffer, port, false, regions,
			&n_regions)) < 0 ||
	    (res = memory_region_init(&view, &port->parameter_view,
			sizeof(port->parameter_view))) < 0)
		return res;
	for (first = 0; first < n_regions; first++) {
		if (memory_regions_overlap(&regions[first], &view))
			return -EINVAL;
		for (second = 0; second < first; second++)
			if (memory_regions_overlap(&regions[first], &regions[second]))
				return -EINVAL;
	}
	return project_buffer(buffer, port, false, &port->parameter_view);
}

static void update_parameter(struct ndarray_port *port)
{
	struct pw_ndarray_filter *filter = port->filter;
	struct pw_buffer *buffer;
	int res;

	if (atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return;
	buffer = atomic_load_explicit(&port->pending_parameter,
			memory_order_acquire);
	if (buffer == NULL)
		return;
	res = filter->events.update_parameter(filter->user_data, port->index,
			&port->parameter_view);
	if (res > 0)
		res = -EPROTO;
	if (res == -EBUSY) {
		atomic_store_explicit(&port->retry_parameter, true,
				memory_order_release);
		return;
	}
	if (res < 0)
		signal_process_error(filter, res);
	atomic_store_explicit(&port->completed_parameter, true,
			memory_order_release);
}

static void parameter_event(void *data, uint64_t count SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	uint32_t i;

	if (atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return;
	for (i = 0; i < filter->n_inputs; i++) {
		struct ndarray_port *port = filter->inputs[i];

		if ((port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) &&
		    atomic_exchange_explicit(&port->parameter_scheduled, false,
			    memory_order_acq_rel))
			update_parameter(port);
	}
}

static void schedule_parameter(struct ndarray_port *port,
		struct pw_buffer *buffer)
{
	struct pw_ndarray_filter *filter = port->filter;
	struct pw_buffer *expected = NULL;
	int res;

	if ((res = project_parameter_buffer(port, buffer)) < 0)
		goto error;
	if (!atomic_compare_exchange_strong_explicit(&port->pending_parameter,
			&expected, buffer, memory_order_release,
			memory_order_relaxed)) {
		pw_filter_queue_buffer(port->filter_port, buffer);
		return;
	}
	atomic_store_explicit(&port->parameter_scheduled, true,
			memory_order_release);
	res = pw_loop_signal_event(pw_thread_loop_get_loop(filter->parameter_loop),
			filter->parameter_event);
	if (res >= 0)
		return;
	atomic_store_explicit(&port->parameter_scheduled, false,
			memory_order_release);
	atomic_store_explicit(&port->pending_parameter, NULL,
			memory_order_release);
error:
	pw_filter_queue_buffer(port->filter_port, buffer);
	signal_process_error(filter, res);
}

static void retry_parameter(struct ndarray_port *port)
{
	struct pw_ndarray_filter *filter = port->filter;
	int res;

	if (!atomic_exchange_explicit(&port->retry_parameter, false,
			memory_order_acq_rel))
		return;
	atomic_store_explicit(&port->parameter_scheduled, true,
			memory_order_release);
	res = pw_loop_signal_event(pw_thread_loop_get_loop(filter->parameter_loop),
			filter->parameter_event);
	if (res >= 0)
		return;
	atomic_store_explicit(&port->parameter_scheduled, false,
			memory_order_release);
	atomic_store_explicit(&port->retry_parameter, true,
			memory_order_release);
	signal_process_error(filter, res);
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

static bool data_buffer_absent(const struct pw_buffer *buffer)
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

static void dequeue_parameter(struct ndarray_port *port)
{
	struct pw_buffer *buffer = NULL, *next, *completed;

	if (atomic_exchange_explicit(&port->completed_parameter, false,
			memory_order_acq_rel)) {
		completed = atomic_exchange_explicit(&port->pending_parameter, NULL,
				memory_order_acq_rel);
		if (completed != NULL)
			pw_filter_queue_buffer(port->filter_port, completed);
	}
	if (atomic_load_explicit(&port->pending_parameter,
			memory_order_acquire) != NULL) {
		while ((next = pw_filter_dequeue_buffer(port->filter_port)) != NULL)
			pw_filter_queue_buffer(port->filter_port, next);
		retry_parameter(port);
		return;
	}
	while ((next = pw_filter_dequeue_buffer(port->filter_port)) != NULL) {
		if (parameter_buffer_absent(next)) {
			pw_filter_queue_buffer(port->filter_port, next);
			continue;
		}
		if (buffer != NULL)
			pw_filter_queue_buffer(port->filter_port, buffer);
		buffer = next;
	}
	if (buffer != NULL)
		schedule_parameter(port, buffer);
}

static void process(void *data, struct spa_io_position *position SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	bool ready = true, any_input = false;
	uint32_t i, region = 0;
	int res;

	if (!atomic_load_explicit(&filter->prepared, memory_order_acquire) ||
	    atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return;

	for (i = 0; i < filter->n_inputs; i++) {
		struct ndarray_port *port = filter->inputs[i];
		struct pw_buffer *buffer = NULL, *next;
		bool available = false;

		if (port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) {
			dequeue_parameter(port);
			continue;
		}
		while ((next = pw_filter_dequeue_buffer(
				port->filter_port)) != NULL) {
			if (buffer != NULL)
				pw_filter_queue_buffer(port->filter_port, buffer);
			buffer = next;
			available = !((filter->flags &
				PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS) &&
				data_buffer_absent(next));
		}
		filter->input_buffers[port->data_index] = buffer;
		filter->input_available[port->data_index] = available;
		if (available)
			any_input = true;
		else if (!(filter->flags &
			    PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS))
			ready = false;
	}
	for (i = 0; i < filter->n_outputs; i++) {
		struct pw_buffer *buffer = filter->output_buffers[i];

		if (buffer == NULL)
			buffer = pw_filter_dequeue_buffer(filter->outputs[i]->filter_port);
		filter->output_buffers[i] = buffer;
		if (buffer == NULL)
			ready = false;
	}
	if ((filter->flags & PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS) &&
	    filter->n_data_inputs > 0 && !any_input)
		ready = false;
	if (!ready)
		goto done;

	for (i = 0; i < filter->n_data_inputs; i++, region++) {
		if (!filter->input_available[i]) {
			filter->n_buffer_regions[region] = 0;
			project_unavailable_input(&filter->process_inputs[i]);
			continue;
		}
		if ((res = collect_buffer_regions(filter->input_buffers[i],
				filter->data_inputs[i], false,
				&filter->buffer_regions[region * MAX_BUFFER_REGIONS],
				&filter->n_buffer_regions[region])) < 0 ||
		    (res = project_buffer(filter->input_buffers[i], filter->data_inputs[i],
				false, &filter->process_inputs[i])) < 0)
			goto error;
	}
	for (i = 0; i < filter->n_outputs; i++, region++) {
		if ((res = collect_buffer_regions(filter->output_buffers[i],
				filter->outputs[i], true,
				&filter->buffer_regions[region * MAX_BUFFER_REGIONS],
				&filter->n_buffer_regions[region])) < 0 ||
		    (res = project_buffer(filter->output_buffers[i], filter->outputs[i],
				true, &filter->process_outputs[i])) < 0)
			goto error;
		filter->output_buffers[i]->buffer->datas[0].chunk->size = 0;
	}
	if ((res = validate_regions(filter)) < 0)
		goto error;

	res = filter->events.process(filter->user_data,
			filter->process_inputs, filter->n_data_inputs,
			filter->process_outputs, filter->n_outputs);
	if (res != 0) {
		if (res > 0)
			res = -EPROTO;
		goto error;
	}
	for (i = 0; i < filter->n_outputs; i++)
		if ((res = validate_completed_output(filter->outputs[i],
				filter->output_buffers[i],
				&filter->process_outputs[i])) < 0)
			goto error;
	for (i = 0; i < filter->n_outputs; i++) {
		if (filter->process_outputs[i].flags &
		    PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE)
			continue;
		commit_output(filter->outputs[i], filter->output_buffers[i],
				&filter->process_outputs[i]);
		pw_filter_queue_buffer(filter->outputs[i]->filter_port,
				filter->output_buffers[i]);
		filter->output_buffers[i] = NULL;
	}
	goto done;

error:
	clear_output_chunks(filter);
	signal_process_error(filter, res);
done:
	recycle_inputs(filter);
}

static int prepare_process_thread(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct pw_ndarray_filter *filter = user_data;
	int res = 0;

	if (atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return -ECANCELED;
	if (atomic_load_explicit(&filter->prepared, memory_order_acquire))
		return 0;
	if (filter->events.prepare_process_thread != NULL)
		res = filter->events.prepare_process_thread(filter->user_data);
	if (res > 0)
		res = -EPROTO;
	if (res == 0)
		atomic_store_explicit(&filter->prepared, true, memory_order_release);
	return res;
}

static int deactivate(struct pw_ndarray_filter *filter)
{
	int res = 0;

	if (!atomic_exchange_explicit(&filter->prepared, false,
			memory_order_acq_rel))
		return 0;
	if (filter->events.deactivate != NULL)
		res = filter->events.deactivate(filter->user_data);
	return res > 0 ? -EPROTO : res;
}

static void fail_on_main_loop(struct pw_ndarray_filter *filter, int res,
		const char *message)
{
	int expected = 0;

	if (res >= 0)
		res = -EIO;
	atomic_compare_exchange_strong_explicit(&filter->error, &expected, res,
			memory_order_acq_rel, memory_order_relaxed);
	if (filter->filter != NULL)
		pw_filter_set_error(filter->filter, res, "%s: %s",
				message, spa_strerror(res));
	if (filter->main_loop != NULL)
		pw_main_loop_quit(filter->main_loop);
}

static void filter_state_changed(void *data, enum pw_filter_state old SPA_UNUSED,
		enum pw_filter_state state, const char *error SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	int res = 0;

	atomic_store_explicit(&filter->state, state, memory_order_release);
	if (state == PW_FILTER_STATE_STREAMING) {
		struct pw_loop *data_loop = pw_filter_get_data_loop(filter->filter);

		if (data_loop == NULL)
			res = -EIO;
		else
			res = pw_loop_invoke(data_loop, prepare_process_thread,
					0, NULL, 0, true, filter);
		if (res < 0) {
			if (filter->events.deactivate != NULL)
				filter->events.deactivate(filter->user_data);
			fail_on_main_loop(filter, res,
					"ndarray process-thread preparation failed");
		}
	} else if (state == PW_FILTER_STATE_PAUSED ||
		   state == PW_FILTER_STATE_UNCONNECTED) {
		if ((res = deactivate(filter)) < 0)
			fail_on_main_loop(filter, res, "ndarray deactivation failed");
	} else if (state == PW_FILTER_STATE_ERROR) {
		int state_error = errno != 0 ? -errno : -EIO;
		int expected = 0;

		deactivate(filter);
		atomic_compare_exchange_strong_explicit(&filter->error,
				&expected, state_error,
				memory_order_acq_rel, memory_order_relaxed);
		pw_main_loop_quit(filter->main_loop);
	}
}

static void filter_param_changed(void *data, void *port_data,
		uint32_t id, const struct spa_pod *param)
{
	struct pw_ndarray_filter *filter = data;
	struct port_data *data_port = port_data;
	int res;

	if (data_port == NULL || id != SPA_PARAM_Format || param == NULL)
		return;
	if ((res = validate_port_format(data_port->port, param)) < 0)
		fail_on_main_loop(filter, res, "invalid ndarray port format");
}

static void filter_destroyed(void *data)
{
	struct pw_ndarray_filter *filter = data;

	filter->filter = NULL;
	filter->connected = false;
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = filter_destroyed,
	.state_changed = filter_state_changed,
	.param_changed = filter_param_changed,
	.process = process,
};

static void error_event(void *data, uint64_t count SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	int res = atomic_load_explicit(&filter->error, memory_order_acquire);

	if (res < 0)
		fail_on_main_loop(filter, res, "ndarray process callback failed");
}

static void stop_parameter_loop(struct pw_ndarray_filter *filter)
{
	if (!filter->parameter_loop_started)
		return;
	pw_thread_loop_stop(filter->parameter_loop);
	filter->parameter_loop_started = false;
}

static void free_filter(struct pw_ndarray_filter *filter)
{
	uint32_t i;

	if (filter == NULL)
		return;
	atomic_store_explicit(&filter->destroying, true, memory_order_release);
	stop_parameter_loop(filter);
	if (filter->parameter_event != NULL && filter->parameter_loop != NULL)
		pw_loop_destroy_source(pw_thread_loop_get_loop(filter->parameter_loop),
				filter->parameter_event);
	if (filter->parameter_loop != NULL)
		pw_thread_loop_destroy(filter->parameter_loop);
	if (filter->error_event != NULL && filter->main_loop != NULL)
		pw_loop_destroy_source(pw_main_loop_get_loop(filter->main_loop),
				filter->error_event);
	if (filter->filter != NULL)
		pw_filter_destroy(filter->filter);
	if (filter->main_loop != NULL)
		pw_main_loop_destroy(filter->main_loop);
	for (i = 0; i < filter->n_ports; i++)
		clear_port(&filter->ports[i]);
	free(filter->n_buffer_regions);
	free(filter->buffer_regions);
	free(filter->process_outputs);
	free(filter->process_inputs);
	free(filter->output_buffers);
	free(filter->input_available);
	free(filter->input_buffers);
	free(filter->outputs);
	free(filter->data_inputs);
	free(filter->inputs);
	free(filter->ports);
	free(filter->remote_name);
	free(filter->node_name);
	if (filter->initialized)
		pw_deinit();
	free(filter);
}

SPA_EXPORT
int pw_ndarray_filter_new(const struct pw_ndarray_filter_config *config,
		struct pw_ndarray_filter **result)
{
	struct pw_ndarray_filter *filter;
	struct pw_properties *properties;
	uint32_t i;
	int res;

	if (result == NULL)
		return -EINVAL;
	*result = NULL;
	if ((filter = calloc(1, sizeof(*filter))) == NULL)
		return -ENOMEM;
	atomic_init(&filter->state, PW_FILTER_STATE_UNCONNECTED);
	atomic_init(&filter->error, 0);
	atomic_init(&filter->prepared, false);
	atomic_init(&filter->destroying, false);
	if ((res = copy_config(filter, config)) < 0)
		goto error;

	pw_init(NULL, NULL);
	filter->initialized = true;
	if ((filter->main_loop = pw_main_loop_new(NULL)) == NULL) {
		res = errno != 0 ? -errno : -ENOMEM;
		goto error;
	}
	filter->error_event = pw_loop_add_event(
			pw_main_loop_get_loop(filter->main_loop), error_event, filter);
	if (filter->error_event == NULL) {
		res = errno != 0 ? -errno : -ENOMEM;
		goto error;
	}
	properties = pw_properties_new(
			PW_KEY_NODE_NAME, filter->node_name,
			PW_KEY_NODE_DESCRIPTION, filter->node_name,
			PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Filter",
			PW_KEY_NODE_VIRTUAL, "true",
			PW_KEY_NODE_PASSIVE, "true",
			NULL);
	if (properties == NULL) {
		res = -ENOMEM;
		goto error;
	}
	if (filter->remote_name != NULL &&
	    (res = pw_properties_set(properties,
		    PW_KEY_REMOTE_NAME, filter->remote_name)) < 0) {
		pw_properties_free(properties);
		goto error;
	}
	filter->filter = pw_filter_new_simple(
			pw_main_loop_get_loop(filter->main_loop), filter->node_name,
			properties, &filter_events, filter);
	if (filter->filter == NULL) {
		res = errno != 0 ? -errno : -ENOMEM;
		goto error;
	}
	for (i = 0; i < filter->n_ports; i++)
		if ((res = add_port(filter, &filter->ports[i])) < 0)
			goto error;
	if (filter->n_parameter_inputs > 0) {
		filter->parameter_loop = pw_thread_loop_new(
				"ndarray-filter-parameters", NULL);
		if (filter->parameter_loop == NULL) {
			res = errno != 0 ? -errno : -ENOMEM;
			goto error;
		}
		filter->parameter_event = pw_loop_add_event(
				pw_thread_loop_get_loop(filter->parameter_loop),
				parameter_event, filter);
		if (filter->parameter_event == NULL) {
			res = errno != 0 ? -errno : -ENOMEM;
			goto error;
		}
		if ((res = pw_thread_loop_start(filter->parameter_loop)) < 0)
			goto error;
		filter->parameter_loop_started = true;
	}
	*result = filter;
	return 0;

error:
	free_filter(filter);
	return res;
}

SPA_EXPORT
int pw_ndarray_filter_connect(struct pw_ndarray_filter *filter)
{
	enum pw_filter_flags flags = 0;
	int res;

	if (filter == NULL || filter->filter == NULL || filter->connected)
		return -EINVAL;
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_RT_PROCESS)
		flags |= PW_FILTER_FLAG_RT_PROCESS;
	res = pw_filter_connect(filter->filter, flags, NULL, 0);
	if (res >= 0)
		filter->connected = true;
	return res;
}

SPA_EXPORT
int pw_ndarray_filter_run(struct pw_ndarray_filter *filter)
{
	int res, error;

	if (filter == NULL || filter->main_loop == NULL || !filter->connected)
		return -EINVAL;
	res = pw_main_loop_run(filter->main_loop);
	error = atomic_load_explicit(&filter->error, memory_order_acquire);
	return error < 0 ? error : res;
}

SPA_EXPORT
int pw_ndarray_filter_quit(struct pw_ndarray_filter *filter)
{
	if (filter == NULL || filter->main_loop == NULL)
		return -EINVAL;
	return pw_main_loop_quit(filter->main_loop);
}

SPA_EXPORT
enum pw_filter_state pw_ndarray_filter_get_state(
		const struct pw_ndarray_filter *filter)
{
	if (filter == NULL)
		return PW_FILTER_STATE_ERROR;
	return atomic_load_explicit(&filter->state, memory_order_acquire);
}

SPA_EXPORT
int pw_ndarray_filter_get_error(const struct pw_ndarray_filter *filter)
{
	if (filter == NULL)
		return -EINVAL;
	return atomic_load_explicit(&filter->error, memory_order_acquire);
}

SPA_EXPORT
uint32_t pw_ndarray_filter_get_node_id(const struct pw_ndarray_filter *filter)
{
	if (filter == NULL || filter->filter == NULL)
		return SPA_ID_INVALID;
	return pw_filter_get_node_id(filter->filter);
}

SPA_EXPORT
void pw_ndarray_filter_destroy(struct pw_ndarray_filter *filter)
{
	if (filter == NULL)
		return;
	atomic_store_explicit(&filter->destroying, true, memory_order_release);
	if (filter->filter != NULL && filter->connected) {
		pw_filter_disconnect(filter->filter);
		filter->connected = false;
	}
	stop_parameter_loop(filter);
	deactivate(filter);
	free_filter(filter);
}
