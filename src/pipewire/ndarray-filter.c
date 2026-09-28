/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stddef.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/dynamic.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/keys.h>
#include <pipewire/main-loop.h>
#include <pipewire/ndarray-filter.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>
#include <pipewire/run-control.h>

#define MAX_METAS 16u
#define MAX_META_BYTES 4096u
#define MAX_BUFFER_REGIONS (6u + MAX_METAS)
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE

#define INGRESS_TRACE_CAPACITY 65536u
#define INGRESS_TRACE_SLOTS_PER_PORT 64u

struct ingress_trace_record {
	uint64_t monotonic_ns;
	uint64_t activation;
	uint64_t frame;
	uint32_t port;
	uint32_t native_slot;
	uint32_t offset;
	int32_t result;
	char event;
	char direction;
};

struct ingress_trace {
	struct ingress_trace_record *records;
	const struct pw_buffer **slots;
	char *path;
	bool initialized;
	uint32_t used;
	uint32_t omitted;
	uint64_t activation;
	uint32_t n_inputs;
};

static _Atomic uint32_t next_ingress_trace_id;

static void ingress_trace_prefault(void *memory, size_t bytes)
{
	volatile unsigned char *pages = memory;
	long page_size = sysconf(_SC_PAGESIZE);
	size_t stride = page_size > 0 ? (size_t)page_size : 4096u;

	for (size_t i = 0; i < bytes; i += stride)
		pages[i] = 0;
	if (bytes != 0)
		pages[bytes - 1] = 0;
}

#endif

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
	struct spa_source *fifo_process_event;
	struct spa_source *properties_event;
	struct spa_source *progressive_event;
	struct pw_loop *progressive_data_loop;
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
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	struct ingress_trace ingress_trace;
#endif
	bool progressive;
	struct pw_ndarray_filter_format progressive_region_format;
	uint32_t *progressive_region_shape;
	char *progressive_region_schema;
	uint32_t progressive_region_bytes;
	uint32_t progressive_region_rows;
	uint32_t progressive_region_count;
	uint32_t progressive_input_port;
	uint64_t progressive_timeout_ns;
	int32_t progressive_cpu;
	pthread_t progressive_worker;
	pthread_mutex_t progressive_mutex;
	pthread_cond_t progressive_cond;
	bool progressive_sync_initialized;
	bool progressive_worker_started;
	bool progressive_worker_ready;
	bool progressive_job_pending;
	_Alignas(64) _Atomic bool progressive_spin_job;
	uint8_t progressive_spin_job_padding[64 - sizeof(_Atomic bool)];
	bool progressive_active;
	_Alignas(64) _Atomic bool progressive_stop;
	uint8_t progressive_stop_padding[64 - sizeof(_Atomic bool)];
	_Atomic bool progressive_cancel;
	_Atomic bool progressive_done;
	int progressive_prepare_result;
	int progressive_result;
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	uint64_t progressive_job_published_ns;
	uint64_t progressive_job_acquired_ns;
	uint64_t progressive_worker_started_ns;
	uint64_t progressive_first_started_ns;
	uint64_t progressive_first_finished_ns;
	uint64_t progressive_terminal_started_ns;
	uint64_t progressive_terminal_finished_ns;
	uint64_t progressive_worker_finished_ns;
#endif
	struct spa_meta_ndarray_progress *progressive_meta;
	uint64_t progressive_generation;
	uint64_t progressive_sequence;

	_Atomic int state;
	_Atomic int error;
	_Atomic bool prepared;
	_Atomic bool destroying;
	_Atomic bool fifo_process_scheduled;
	_Atomic bool properties_pending;
	bool initialized;
	bool connected;
	int64_t last_request_token;
	int64_t completed_token;
	int32_t run_control_result;
	enum pw_ao_run_control_state requested_state;
	enum pw_ao_run_control_state actual_state;
	int64_t last_reset_token;
	int64_t completed_reset_token;
	int32_t reset_result;
};

#ifdef PW_ENABLE_DIAGNOSTIC_TRACE

static int ingress_trace_init(struct ingress_trace *trace, uint32_t n_inputs,
		uint32_t n_outputs)
{
	const char *directory = getenv("PW_NDARRAY_FILTER_TRACE_DIR");
	size_t length, record_bytes, slot_bytes;
	uint32_t id;

	if (directory == NULL || directory[0] == '\0')
		return 0;
	trace->n_inputs = n_inputs;
	length = strlen(directory);
	if (length > SIZE_MAX - 64u)
		return -ENAMETOOLONG;
	record_bytes = INGRESS_TRACE_CAPACITY * sizeof(*trace->records);
	slot_bytes = (size_t)(n_inputs + n_outputs) *
		INGRESS_TRACE_SLOTS_PER_PORT * sizeof(*trace->slots);
	trace->path = malloc(length + 64u);
	trace->records = malloc(record_bytes);
	trace->slots = calloc((size_t)(n_inputs + n_outputs) *
			INGRESS_TRACE_SLOTS_PER_PORT,
			sizeof(*trace->slots));
	if (trace->path == NULL || trace->records == NULL ||
	    (n_inputs + n_outputs != 0 && trace->slots == NULL))
		return -ENOMEM;
	ingress_trace_prefault(trace->records, record_bytes);
	if (slot_bytes != 0)
		ingress_trace_prefault(trace->slots, slot_bytes);
	id = atomic_fetch_add_explicit(&next_ingress_trace_id, 1,
			memory_order_relaxed);
	snprintf(trace->path, length + 64u, "%s/ndarray-filter-%ld-%u.csv",
			directory, (long)getpid(), id);
	trace->initialized = true;
	return 0;
}

static uint32_t ingress_trace_slot(struct ingress_trace *trace,
		const struct ndarray_port *port, const struct pw_buffer *buffer)
{
	const struct pw_buffer **slots;
	uint32_t i, index;

	if (port == NULL || buffer == NULL)
		return UINT32_MAX;
	index = port->direction == SPA_DIRECTION_OUTPUT ?
		trace->n_inputs + port->index : port->data_index;
	slots = &trace->slots[(size_t)index *
			INGRESS_TRACE_SLOTS_PER_PORT];
	for (i = 0; i < INGRESS_TRACE_SLOTS_PER_PORT; i++)
		if (slots[i] == buffer)
			return i;
	for (i = 0; i < INGRESS_TRACE_SLOTS_PER_PORT; i++)
		if (slots[i] == NULL) {
			slots[i] = buffer;
			return i;
		}
	return UINT32_MAX;
}

static void ingress_trace_add(struct pw_ndarray_filter *filter, char event,
		const struct ndarray_port *port, const struct pw_buffer *buffer,
		int result)
{
	struct ingress_trace *trace = &filter->ingress_trace;
	struct ingress_trace_record *record;
	const struct spa_meta_header *header = NULL;
	struct timespec now;
	uint32_t slot;

	if (!trace->initialized)
		return;
	if (trace->used == INGRESS_TRACE_CAPACITY) {
		trace->omitted++;
		return;
	}
	slot = ingress_trace_slot(trace, port, buffer);
	if (slot == UINT32_MAX || clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		trace->omitted++;
		return;
	}
	if (buffer != NULL && buffer->buffer != NULL)
		header = spa_buffer_find_meta_data(buffer->buffer,
				SPA_META_Header, sizeof(*header));
	record = &trace->records[trace->used++];
	*record = (struct ingress_trace_record) {
		.monotonic_ns = (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec,
		.activation = trace->activation,
		.frame = header != NULL ? header->seq : UINT64_MAX,
		.port = port->direction == SPA_DIRECTION_OUTPUT ?
			port->index : port->data_index,
		.native_slot = slot,
		.offset = header != NULL ? header->offset : UINT32_MAX,
		.result = result,
		.event = event,
		.direction = port->direction == SPA_DIRECTION_OUTPUT ? 'O' : 'I',
	};
}

/* The worker publishes this timestamp before progressive_done. Only the
 * data loop appends records, so the diagnostic ring remains single-writer. */
static void ingress_trace_progress(struct pw_ndarray_filter *filter,
		char event, uint64_t timestamp_ns, int result)
{
	struct ingress_trace *trace = &filter->ingress_trace;
	struct ingress_trace_record *record;
	struct timespec now;

	if (!trace->initialized)
		return;
	if (trace->used == INGRESS_TRACE_CAPACITY) {
		trace->omitted++;
		return;
	}
	if (timestamp_ns == 0) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
			trace->omitted++;
			return;
		}
		timestamp_ns = (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
	}
	record = &trace->records[trace->used++];
	*record = (struct ingress_trace_record) {
		.monotonic_ns = timestamp_ns,
		.activation = trace->activation,
		.frame = filter->progressive_sequence,
		.port = filter->progressive_input_port,
		.native_slot = UINT32_MAX,
		.offset = 0,
		.result = result,
		.event = event,
		.direction = 'P',
	};
}

static void ingress_trace_dump(struct ingress_trace *trace)
{
	FILE *output;
	uint32_t i;

	if (!trace->initialized)
		return;
	output = fopen(trace->path, "w");
	if (output == NULL) {
		fprintf(stderr, "cannot write ndarray ingress trace %s: %s\n",
				trace->path, strerror(errno));
		return;
	}
	fprintf(output, "# capacity=%u used=%u omitted=%u\n",
			INGRESS_TRACE_CAPACITY, trace->used, trace->omitted);
	fputs("event,direction,monotonic_ns,activation,port,native_slot,frame,offset,result\n",
			output);
	for (i = 0; i < trace->used; i++) {
		const struct ingress_trace_record *record = &trace->records[i];

		fprintf(output, "%c,%c,%llu,%llu,%u,%u,%llu,%u,%d\n",
				record->event, record->direction,
				(unsigned long long)record->monotonic_ns,
				(unsigned long long)record->activation,
				record->port, record->native_slot,
				(unsigned long long)record->frame,
				record->offset, record->result);
	}
	fclose(output);
}

#else

#define ingress_trace_init(...) (0)
#define ingress_trace_add(...) do {} while (0)
#define ingress_trace_progress(...) do {} while (0)
#define ingress_trace_dump(...) do {} while (0)

#endif

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

	if (config == NULL ||
	    (config->version == PW_VERSION_NDARRAY_FILTER_CONFIG
		? config->struct_size < offsetof(struct pw_ndarray_filter_config,
			progressive_input_port)
		: config->version != PW_VERSION_NDARRAY_FILTER_CONFIG_PROGRESSIVE ||
		  config->struct_size < sizeof(*config)) ||
	    (config->flags & ~(PW_NDARRAY_FILTER_FLAG_RT_PROCESS |
		    PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS |
		    PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL |
		    PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES |
		    PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL |
		    PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS |
		    PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_BUSY_POLL |
		    PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE |
		    PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE)) != 0 ||
	    ((config->flags & (PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_BUSY_POLL |
			       PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE |
			       PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE)) &&
	     (config->version != PW_VERSION_NDARRAY_FILTER_CONFIG_PROGRESSIVE ||
	      config->progressive_cpu < 0)) ||
	    ((config->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE) &&
	     (config->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE)) ||
	    config->n_ports == 0 ||
	    config->n_ports > PW_NDARRAY_FILTER_MAX_PORTS ||
	    config->ports == NULL || config->events == NULL ||
	    (config->events->version != PW_VERSION_NDARRAY_FILTER_EVENTS_V1 &&
	     config->events->version != PW_VERSION_NDARRAY_FILTER_EVENTS &&
	     config->events->version !=
		PW_VERSION_NDARRAY_FILTER_EVENTS_PROGRESSIVE) ||
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
	memcpy(&filter->events, config->events,
		config->events->version == PW_VERSION_NDARRAY_FILTER_EVENTS_V1
			? offsetof(struct pw_ndarray_filter_events, enum_prop_info)
			: config->events->version == PW_VERSION_NDARRAY_FILTER_EVENTS
				? offsetof(struct pw_ndarray_filter_events,
					prepare_progressive_worker)
				: sizeof(filter->events));
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
	if ((config->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) &&
	    (config->events->version < PW_VERSION_NDARRAY_FILTER_EVENTS ||
	     config->events->enum_prop_info == NULL ||
	     config->events->get_props == NULL ||
	     config->events->set_props == NULL))
		return -EINVAL;
	if ((config->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL) &&
	    (config->events->version < PW_VERSION_NDARRAY_FILTER_EVENTS ||
	     config->events->reset == NULL))
		return -EINVAL;
	filter->n_inputs = input;
	filter->n_data_inputs = data_input;
	filter->n_parameter_inputs = parameter_input;
	filter->n_outputs = output;
	if (config->version == PW_VERSION_NDARRAY_FILTER_CONFIG_PROGRESSIVE) {
		struct ndarray_port *port;
		const struct pw_ndarray_filter_format *region =
			&config->progressive_region_format;
		size_t region_bytes;
		uint64_t frame_rate_product, region_rate_product;
		int32_t region_stride;
		uint32_t axis, j;

		if (config->events->version !=
			PW_VERSION_NDARRAY_FILTER_EVENTS_PROGRESSIVE ||
		    config->events->prepare_progressive_worker == NULL ||
		    config->events->abort_progressive_frame == NULL ||
		    !(config->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS) ||
		    !(config->flags & PW_NDARRAY_FILTER_FLAG_RT_PROCESS) ||
		    data_input != 1 || output == 0 ||
		    config->progressive_input_port >= input ||
		    config->progressive_timeout_ns == 0 ||
		    config->progressive_cpu < -1 ||
		    config->progressive_cpu >= CPU_SETSIZE)
			return -EINVAL;
		port = NULL;
		for (j = 0; j < filter->n_ports; j++)
			if (filter->ports[j].direction == SPA_DIRECTION_INPUT &&
			    filter->ports[j].index == config->progressive_input_port)
				port = &filter->ports[j];
		if (port == NULL ||
		    (port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) ||
		    (res = checked_format_size(region, &region_bytes,
			&region_stride)) < 0)
			return -EINVAL;
		axis = region->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR ? 0 :
			region->n_dimensions - 1;
		if (region->n_dimensions != port->format.n_dimensions ||
		    region->element_type != port->format.element_type ||
		    region->layout != port->format.layout ||
		    region->rate_denom == 0 ||
		    port->format.rate_denom == 0 ||
		    region->schema == NULL || port->format.schema == NULL ||
		    spa_streq(region->schema, port->format.schema) ||
		    region_bytes > port->size ||
		    port->size % region_bytes != 0 ||
		    port->format.shape[axis] % region->shape[axis] != 0)
			return -EINVAL;
		for (j = 0; j < region->n_dimensions; j++)
			if (j != axis && region->shape[j] != port->format.shape[j])
				return -EINVAL;
		region_rate_product = (uint64_t)region->rate_num *
			port->format.rate_denom;
		frame_rate_product = (uint64_t)port->format.rate_num *
			region->rate_denom;
		if (frame_rate_product > UINT64_MAX /
		    (port->size / region_bytes) ||
		    region_rate_product != frame_rate_product *
		    (port->size / region_bytes))
			return -EINVAL;
		filter->progressive_region_shape = malloc(region->n_dimensions *
			sizeof(uint32_t));
		filter->progressive_region_schema = strdup(region->schema);
		if (filter->progressive_region_shape == NULL ||
		    filter->progressive_region_schema == NULL)
			return -ENOMEM;
		memcpy(filter->progressive_region_shape, region->shape,
			region->n_dimensions * sizeof(uint32_t));
		filter->progressive_region_format = *region;
		filter->progressive_region_format.shape =
			filter->progressive_region_shape;
		filter->progressive_region_format.schema =
			filter->progressive_region_schema;
		filter->progressive_region_bytes = region_bytes;
		filter->progressive_region_rows = region->shape[axis];
		filter->progressive_region_count = port->size / region_bytes;
		filter->progressive_input_port = config->progressive_input_port;
		filter->progressive_timeout_ns = config->progressive_timeout_ns;
		filter->progressive_cpu = config->progressive_cpu;
		filter->progressive = true;
	}

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

static int add_port(struct pw_ndarray_filter *filter,
		struct ndarray_port *port)
{
	uint8_t storage[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	const struct spa_pod *params[5];
	struct pw_properties *properties;
	struct port_data *data;
	uint32_t i, n_params = 4;
	bool progressive_input = filter->progressive &&
		port->direction == SPA_DIRECTION_INPUT &&
		port->index == filter->progressive_input_port;

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
			SPA_POD_CHOICE_FLAGS_Int(progressive_input
				? (1u << SPA_DATA_MemFd)
				: (1u << SPA_DATA_MemPtr) |
				  (1u << SPA_DATA_MemFd)));
	params[2] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[3] = build_acquisition_meta(&builder);
	if (progressive_input) {
		params[4] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_NdarrayProgress),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_ndarray_progress)));
		n_params++;
	}
	for (i = 0; i < n_params; i++)
		if (params[i] == NULL) {
			pw_properties_free(properties);
			return -ENOSPC;
		}
	data = pw_filter_add_port(filter->filter,
			port->direction == SPA_DIRECTION_INPUT
				? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(*data), properties,
			params, n_params);
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
			if (filter->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS)
				ingress_trace_add(filter, 'R', filter->data_inputs[i],
						filter->input_buffers[i], 0);
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

static void fifo_process_event(void *data, uint64_t count SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	int res;

	atomic_store_explicit(&filter->fifo_process_scheduled, false,
			memory_order_release);
	if (atomic_load_explicit(&filter->destroying, memory_order_acquire) ||
	    filter->filter == NULL)
		return;
	if ((res = pw_filter_trigger_process(filter->filter)) < 0)
		signal_process_error(filter, res);
}

static void schedule_fifo_process(struct pw_ndarray_filter *filter)
{
	bool expected = false;
	int res;

	if (!atomic_compare_exchange_strong_explicit(
			&filter->fifo_process_scheduled, &expected, true,
			memory_order_acq_rel, memory_order_relaxed))
		return;
	res = pw_loop_signal_event(pw_main_loop_get_loop(filter->main_loop),
			filter->fifo_process_event);
	if (res < 0) {
		atomic_store_explicit(&filter->fifo_process_scheduled, false,
				memory_order_release);
		signal_process_error(filter, res);
	}
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
	else if ((filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) &&
		 (res = pw_ndarray_filter_notify_properties(filter)) < 0)
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

static bool dequeue_fifo_input(struct pw_ndarray_filter *filter,
		struct ndarray_port *port)
{
	uint32_t index = port->data_index;
	struct pw_buffer *buffer;

	if (filter->input_buffers[index] != NULL)
		return filter->input_available[index];
	while ((buffer = pw_filter_dequeue_buffer(port->filter_port)) != NULL) {
		if ((filter->flags & PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS) &&
		    data_buffer_absent(buffer)) {
			ingress_trace_add(filter, 'A', port, buffer, 0);
			pw_filter_queue_buffer(port->filter_port, buffer);
			continue;
		}
		ingress_trace_add(filter, 'D', port, buffer, 0);
		filter->input_buffers[index] = buffer;
		filter->input_available[index] = true;
		return true;
	}
	filter->input_available[index] = false;
	return false;
}

static void schedule_fifo_backlog(struct pw_ndarray_filter *filter)
{
	bool any_input = false, all_inputs = true;
	uint32_t i;

	for (i = 0; i < filter->n_data_inputs; i++) {
		bool available = dequeue_fifo_input(filter, filter->data_inputs[i]);

		any_input |= available;
		all_inputs &= available;
	}
	if (filter->n_data_inputs == 0 ||
	    ((filter->flags & PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS)
		? !any_input : !all_inputs))
		return;
	schedule_fifo_process(filter);
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

static int complete_progressive_on_data_loop(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct pw_ndarray_filter *filter = user_data;
	struct spa_meta_ndarray_progress *meta;
	uint32_t i;
	int res;

	if (!filter->progressive_active ||
	    !atomic_load_explicit(&filter->progressive_done, memory_order_acquire))
		return 0;
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	if (filter->progressive_job_published_ns != 0)
		ingress_trace_progress(filter, 'j',
			filter->progressive_job_published_ns, 0);
	if (filter->progressive_job_acquired_ns != 0)
		ingress_trace_progress(filter, 'k',
			filter->progressive_job_acquired_ns, 0);
	if (filter->progressive_worker_started_ns != 0)
		ingress_trace_progress(filter, 's',
			filter->progressive_worker_started_ns, 0);
	if (filter->progressive_first_started_ns != 0)
		ingress_trace_progress(filter, 'b',
			filter->progressive_first_started_ns, 0);
	if (filter->progressive_first_finished_ns != 0)
		ingress_trace_progress(filter, 'e',
			filter->progressive_first_finished_ns, 0);
	if (filter->progressive_terminal_started_ns != 0)
		ingress_trace_progress(filter, 't',
			filter->progressive_terminal_started_ns, 0);
	if (filter->progressive_terminal_finished_ns != 0)
		ingress_trace_progress(filter, 'u',
			filter->progressive_terminal_finished_ns, 0);
	ingress_trace_progress(filter, 'W',
			filter->progressive_worker_finished_ns,
			filter->progressive_result);
	ingress_trace_progress(filter, 'C', 0, filter->progressive_result);
#endif
	meta = filter->progressive_meta;
	if (filter->progressive_result == 0 &&
	    !atomic_load_explicit(&filter->progressive_cancel, memory_order_acquire) &&
	    !atomic_load_explicit(&filter->progressive_stop, memory_order_acquire)) {
		for (i = 0; i < filter->n_outputs; i++)
			if ((res = validate_completed_output(filter->outputs[i],
				filter->output_buffers[i],
				&filter->process_outputs[i])) < 0) {
				signal_process_error(filter, res);
				break;
			}
		if (i == filter->n_outputs)
			for (i = 0; i < filter->n_outputs; i++) {
				if (filter->process_outputs[i].flags &
				    PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE)
					continue;
				commit_output(filter->outputs[i],
					filter->output_buffers[i],
					&filter->process_outputs[i]);
				res = pw_filter_queue_buffer(
					filter->outputs[i]->filter_port,
					filter->output_buffers[i]);
				if (res < 0)
					signal_process_error(filter, res);
				filter->output_buffers[i] = NULL;
			}
	} else {
		clear_output_chunks(filter);
		if (filter->progressive_prepare_result < 0)
			signal_process_error(filter,
				filter->progressive_prepare_result);
	}
	ingress_trace_progress(filter, 'Q', 0, filter->progressive_result);
	if (filter->input_buffers[0] != NULL) {
		res = pw_filter_queue_buffer(filter->data_inputs[0]->filter_port,
				filter->input_buffers[0]);
		if (res < 0)
			signal_process_error(filter, res);
		filter->input_buffers[0] = NULL;
		filter->input_available[0] = false;
	}
	ingress_trace_progress(filter, 'R', 0, filter->progressive_result);
	/* The worker has finished its last pixel and metadata access. */
	if (meta != NULL)
		__atomic_store_n(&meta->released_generation,
			filter->progressive_generation, __ATOMIC_RELEASE);
	ingress_trace_progress(filter, 'A', 0, filter->progressive_result);
	filter->progressive_meta = NULL;
	filter->progressive_active = false;
	atomic_store_explicit(&filter->progressive_done, false,
			memory_order_release);
	if (!(filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE) &&
	    !atomic_load_explicit(&filter->progressive_stop, memory_order_acquire) &&
	    filter->filter != NULL &&
	    (res = pw_filter_trigger_process(filter->filter)) < 0)
		signal_process_error(filter, res);
	ingress_trace_progress(filter, 'T', 0, 0);
	return 0;
}

static void progressive_completion_event(void *data, uint64_t count SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;

	(void)complete_progressive_on_data_loop(NULL, false, 0,
			NULL, 0, filter);
}

static int add_progressive_event_on_data_loop(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct pw_ndarray_filter *filter = user_data;

	filter->progressive_event = pw_loop_add_event(
			filter->progressive_data_loop,
			progressive_completion_event, filter);
	return filter->progressive_event == NULL ?
		(errno != 0 ? -errno : -ENOMEM) : 0;
}

static int remove_progressive_event_on_data_loop(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct pw_ndarray_filter *filter = user_data;

	if (filter->progressive_event != NULL) {
		pw_loop_destroy_source(filter->progressive_data_loop,
				filter->progressive_event);
		filter->progressive_event = NULL;
	}
	return 0;
}

static void remove_progressive_event(struct pw_ndarray_filter *filter)
{
	int res;

	if (filter->progressive_event == NULL ||
	    filter->progressive_data_loop == NULL)
		return;
	res = pw_loop_invoke(filter->progressive_data_loop,
			remove_progressive_event_on_data_loop,
			0, NULL, 0, true, filter);
	if (res < 0)
		(void)pw_loop_locked(filter->progressive_data_loop,
			remove_progressive_event_on_data_loop,
			0, NULL, 0, filter);
}

static uint64_t monotonic_time_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
}

static int run_progressive_frame(struct pw_ndarray_filter *filter)
{
	struct spa_meta_ndarray_progress *meta = filter->progressive_meta;
	struct pw_ndarray_filter_buffer *view = &filter->process_inputs[0];
	uint32_t frame_header_flags = view->header.flags;
	uint32_t consumed = 0, committed, processable, state, i;
	uint64_t start = monotonic_time_ns();
	uint64_t deadline;
	int res = 0;

#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	filter->progressive_worker_started_ns = start;
#endif

	if (meta == NULL) {
		res = -ENOBUFS;
		goto abort;
	}
	if (start == 0 || UINT64_MAX - start < filter->progressive_timeout_ns) {
		res = -EOVERFLOW;
		goto abort;
	}
	deadline = start + filter->progressive_timeout_ns;
	for (;;) {
		uint64_t now;
		struct timespec pause = { .tv_nsec = 50000 };

		if (atomic_load_explicit(&filter->progressive_stop,
			memory_order_acquire) ||
		    atomic_load_explicit(&filter->progressive_cancel,
			memory_order_acquire)) {
			res = -ECANCELED;
			break;
		}
		if (meta->generation != filter->progressive_generation ||
		    meta->sequence != filter->progressive_sequence) {
			res = -ESTALE;
			break;
		}
		committed = __atomic_load_n(&meta->committed_regions,
			__ATOMIC_ACQUIRE);
		state = __atomic_load_n(&meta->state, __ATOMIC_ACQUIRE);
		if (state == SPA_META_NDARRAY_PROGRESS_TERMINAL)
			committed = __atomic_load_n(&meta->committed_regions,
				__ATOMIC_ACQUIRE);
		if (committed > filter->progressive_region_count ||
		    state > SPA_META_NDARRAY_PROGRESS_ABORTED) {
			res = -EPROTO;
			break;
		}
		if (state == SPA_META_NDARRAY_PROGRESS_ABORTED) {
			res = -ECANCELED;
			break;
		}
		if (state == SPA_META_NDARRAY_PROGRESS_TERMINAL &&
		    committed != filter->progressive_region_count) {
			res = -EPROTO;
			break;
		}
		processable = committed;
		if (state == SPA_META_NDARRAY_PROGRESS_ACTIVE &&
		    processable == filter->progressive_region_count)
			processable--;
		if (processable > consumed) {
			while (consumed < processable) {
				view->data = SPA_PTROFF(
					filter->input_buffers[0]->buffer->datas[0].data,
					filter->input_buffers[0]->buffer->datas[0].chunk->offset +
					(size_t)consumed * filter->progressive_region_bytes,
					void);
				view->size = filter->progressive_region_bytes;
				view->capacity =
					filter->input_buffers[0]->buffer->datas[0].maxsize -
					filter->input_buffers[0]->buffer->datas[0].chunk->offset -
					consumed * filter->progressive_region_bytes;
				view->header.offset = consumed *
					filter->progressive_region_rows;
				view->header.flags = frame_header_flags &
					~(SPA_META_HEADER_FLAG_DISCONT |
					  SPA_META_HEADER_FLAG_MARKER);
				if (consumed == 0)
					view->header.flags |= frame_header_flags &
						SPA_META_HEADER_FLAG_DISCONT;
				if (consumed + 1 == filter->progressive_region_count)
					view->header.flags |= SPA_META_HEADER_FLAG_MARKER;
				for (i = 0; i < filter->n_outputs; i++)
					filter->process_outputs[i].flags = 0;
				#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
				{
					uint64_t *started = consumed == 0 ?
						&filter->progressive_first_started_ns :
						&filter->progressive_terminal_started_ns;

					*started = monotonic_time_ns();
				}
				#endif
				res = filter->events.process(filter->user_data,
					view, 1, filter->process_outputs,
					filter->n_outputs);
				#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
				{
					uint64_t *finished = consumed == 0 ?
						&filter->progressive_first_finished_ns :
						&filter->progressive_terminal_finished_ns;

					*finished = monotonic_time_ns();
				}
				#endif
				if (res != 0) {
					if (res > 0)
						res = -EPROTO;
					filter->progressive_prepare_result = res;
					goto abort;
				}
				consumed++;
				if (atomic_load_explicit(&filter->progressive_cancel,
					memory_order_acquire)) {
					res = -ECANCELED;
					goto abort;
				}
			}
			now = monotonic_time_ns();
			if (now == 0 || UINT64_MAX - now <
			    filter->progressive_timeout_ns) {
				res = -EOVERFLOW;
				break;
			}
			deadline = now + filter->progressive_timeout_ns;
		}
		if (state == SPA_META_NDARRAY_PROGRESS_TERMINAL) {
			res = consumed == filter->progressive_region_count
				? 0 : -EPROTO;
			break;
		}
		now = monotonic_time_ns();
		if (now == 0 || now >= deadline) {
			res = -ETIMEDOUT;
			break;
		}
		if (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_BUSY_POLL) {
#if defined(__x86_64__) || defined(__i386__)
			__asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield");
#else
			atomic_signal_fence(memory_order_seq_cst);
#endif
		} else {
			nanosleep(&pause, NULL);
		}
	}
abort:
	if (res < 0) {
		int abort_res = filter->events.abort_progressive_frame(
			filter->user_data, filter->progressive_generation,
			filter->progressive_sequence, res);
		if (abort_res > 0)
			abort_res = -EPROTO;
		if (abort_res < 0)
			filter->progressive_prepare_result = abort_res;
	}
	return res;
}

static void run_progressive_job(struct pw_ndarray_filter *filter)
{
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	filter->progressive_job_acquired_ns = monotonic_time_ns();
#endif
	filter->progressive_result = run_progressive_frame(filter);
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	filter->progressive_worker_finished_ns = monotonic_time_ns();
#endif
	atomic_store_explicit(&filter->progressive_done, true, memory_order_release);
	if (filter->progressive_event != NULL &&
	    pw_loop_signal_event(filter->progressive_data_loop,
		    filter->progressive_event) < 0)
		signal_process_error(filter, -EIO);
}

static void *progressive_worker(void *data)
{
	struct pw_ndarray_filter *filter = data;
	int res = 0;

	if (filter->progressive_cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(filter->progressive_cpu, &set);
		res = -pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	}
	if (res == 0 &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE)) {
		const struct sched_param normal = { .sched_priority = 0 };

		/* A continuously spinning FIFO thread can exhaust the RT runtime. */
		res = -pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
	}
	if (res == 0)
		res = filter->events.prepare_progressive_worker(filter->user_data);
	if (res > 0)
		res = -EPROTO;
	pthread_mutex_lock(&filter->progressive_mutex);
	filter->progressive_prepare_result = res;
	filter->progressive_worker_ready = true;
	pthread_cond_signal(&filter->progressive_cond);
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE) {
		pthread_mutex_unlock(&filter->progressive_mutex);
		while (res == 0) {
			while (!atomic_load_explicit(&filter->progressive_spin_job,
					memory_order_acquire) &&
			       !atomic_load_explicit(&filter->progressive_stop,
					memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
				__asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
				__asm__ __volatile__("yield");
#else
				atomic_signal_fence(memory_order_seq_cst);
#endif
			}
			if (!atomic_exchange_explicit(&filter->progressive_spin_job,
					false, memory_order_acq_rel)) {
				if (atomic_load_explicit(&filter->progressive_stop,
					memory_order_acquire))
					break;
				continue;
			}
			run_progressive_job(filter);
		}
		return NULL;
	}
	while (res == 0) {
		while (!filter->progressive_job_pending &&
		       !atomic_load_explicit(&filter->progressive_stop,
			memory_order_acquire))
			pthread_cond_wait(&filter->progressive_cond,
				&filter->progressive_mutex);
		if (!filter->progressive_job_pending &&
		    atomic_load_explicit(&filter->progressive_stop,
			memory_order_acquire))
			break;
		filter->progressive_job_pending = false;
		pthread_mutex_unlock(&filter->progressive_mutex);
		run_progressive_job(filter);
		pthread_mutex_lock(&filter->progressive_mutex);
	}
	pthread_mutex_unlock(&filter->progressive_mutex);
	return NULL;
}

static int project_progressive_input(struct pw_ndarray_filter *filter,
		struct pw_buffer *buffer)
{
	struct spa_buffer *spa_buffer = buffer->buffer;
	struct spa_data *data;
	struct spa_meta *meta;
	struct spa_meta_ndarray_progress *progress;
	uint32_t committed, state;
	int res;

	if (spa_buffer == NULL || spa_buffer->n_datas != 1 ||
	    spa_buffer->datas == NULL)
		return -EINVAL;
	data = &spa_buffer->datas[0];
	if (data->type != SPA_DATA_MemFd || data->fd < 0 ||
	    !(data->flags & SPA_DATA_FLAG_MAPPABLE) ||
	    data->chunk == NULL ||
	    data->chunk->offset >= data->maxsize ||
	    data->chunk->size < filter->progressive_region_bytes ||
	    data->chunk->size > data->maxsize - data->chunk->offset)
		return -ENOTSUP;
	meta = spa_buffer_find_meta(spa_buffer, SPA_META_NdarrayProgress);
	if (meta == NULL || meta->data == NULL ||
	    meta->size < sizeof(*progress) ||
	    (uintptr_t)meta->data % _Alignof(struct spa_meta_ndarray_progress))
		return -ENOTSUP;
	progress = meta->data;
	if (!__atomic_is_lock_free(sizeof(progress->released_generation),
			&progress->released_generation) ||
	    !__atomic_is_lock_free(sizeof(progress->committed_regions),
			&progress->committed_regions) ||
	    !__atomic_is_lock_free(sizeof(progress->state),
			&progress->state))
		return -ENOTSUP;
	committed = __atomic_load_n(&progress->committed_regions,
			__ATOMIC_ACQUIRE);
	state = __atomic_load_n(&progress->state, __ATOMIC_ACQUIRE);
	if (progress->version != SPA_META_NDARRAY_PROGRESS_VERSION ||
	    progress->abi_size != sizeof(*progress) ||
	    progress->generation == 0 ||
	    progress->region_count != filter->progressive_region_count ||
	    progress->region_bytes != filter->progressive_region_bytes ||
	    committed == 0 || committed > progress->region_count ||
	    state > SPA_META_NDARRAY_PROGRESS_ABORTED ||
	    __atomic_load_n(&progress->released_generation,
		__ATOMIC_ACQUIRE) >= progress->generation)
		return -EPROTO;
	if ((res = collect_buffer_regions(buffer, filter->data_inputs[0],
			true, filter->buffer_regions,
			filter->n_buffer_regions)) < 0 ||
	    (res = project_buffer(buffer, filter->data_inputs[0], false,
			&filter->process_inputs[0])) < 0)
		return res;
	if (!(filter->process_inputs[0].metadata_valid &
	      PW_NDARRAY_FILTER_METADATA_HEADER) ||
	    filter->process_inputs[0].header.seq != progress->sequence)
		return -EPROTO;
	filter->progressive_meta = progress;
	filter->progressive_generation = progress->generation;
	filter->progressive_sequence = progress->sequence;
	return 0;
}

static void process_progressive(struct pw_ndarray_filter *filter)
{
	struct pw_buffer *buffer;
	uint32_t i;
	int res;
	bool abort_frame = false;

	for (i = 0; i < filter->n_inputs; i++)
		if (filter->inputs[i]->flags &
		    PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER)
			dequeue_parameter(filter->inputs[i]);
	if (filter->progressive_active)
		return;
	if (!dequeue_fifo_input(filter, filter->data_inputs[0]))
		return;
	buffer = filter->input_buffers[0];
	if ((res = project_progressive_input(filter, buffer)) < 0)
		goto error;
	for (i = 0; i < filter->n_outputs; i++) {
		struct pw_buffer *output = filter->output_buffers[i];

		if (output == NULL)
			output = pw_filter_dequeue_buffer(
				filter->outputs[i]->filter_port);
		filter->output_buffers[i] = output;
		if (output == NULL) {
			abort_frame = true;
			ingress_trace_progress(filter, 'S', 0, -ENOBUFS);
			filter->n_buffer_regions[i + 1] = 0;
			continue;
		}
		if ((res = collect_buffer_regions(output, filter->outputs[i],
				true,
				&filter->buffer_regions[(i + 1) *
					MAX_BUFFER_REGIONS],
				&filter->n_buffer_regions[i + 1])) < 0 ||
		    (res = project_buffer(output, filter->outputs[i], true,
				&filter->process_outputs[i])) < 0)
			goto error;
		output->buffer->datas[0].chunk->size = 0;
	}
	if ((res = validate_regions(filter)) < 0)
		goto error;
	filter->progressive_active = true;
	filter->progressive_prepare_result = 0;
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	filter->progressive_job_published_ns = 0;
	filter->progressive_job_acquired_ns = 0;
	filter->progressive_worker_started_ns = 0;
	filter->progressive_first_started_ns = 0;
	filter->progressive_first_finished_ns = 0;
	filter->progressive_terminal_started_ns = 0;
	filter->progressive_terminal_finished_ns = 0;
	filter->progressive_worker_finished_ns = 0;
#endif
	atomic_store_explicit(&filter->progressive_cancel, abort_frame,
			memory_order_release);
	atomic_store_explicit(&filter->progressive_done, false,
			memory_order_release);
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE) {
		filter->progressive_result = run_progressive_frame(filter);
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
		filter->progressive_worker_finished_ns = monotonic_time_ns();
#endif
		atomic_store_explicit(&filter->progressive_done, true,
			memory_order_release);
		(void)complete_progressive_on_data_loop(NULL, false, 0,
				NULL, 0, filter);
		return;
	}
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_SPIN_IDLE) {
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
		filter->progressive_job_published_ns = monotonic_time_ns();
#endif
		atomic_store_explicit(&filter->progressive_spin_job, true,
			memory_order_release);
	} else {
		pthread_mutex_lock(&filter->progressive_mutex);
		filter->progressive_job_pending = true;
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
		filter->progressive_job_published_ns = monotonic_time_ns();
#endif
		pthread_cond_signal(&filter->progressive_cond);
		pthread_mutex_unlock(&filter->progressive_mutex);
	}
	return;
error:
	clear_output_chunks(filter);
	signal_process_error(filter, res);
	recycle_inputs(filter);
}

static void process(void *data, struct spa_io_position *position SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	bool ready = true, any_input = false;
	bool fifo_inputs = filter->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS;
	uint32_t i, region = 0;
	int res;

	if (!atomic_load_explicit(&filter->prepared, memory_order_acquire) ||
	    atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return;
	if (filter->progressive) {
		process_progressive(filter);
		return;
	}
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	if (filter->ingress_trace.records != NULL)
		filter->ingress_trace.activation++;
#endif

	for (i = 0; i < filter->n_inputs; i++) {
		struct ndarray_port *port = filter->inputs[i];
		struct pw_buffer *buffer = NULL, *next;
		bool available = false;

		if (port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER) {
			dequeue_parameter(port);
			continue;
		}
		if (fifo_inputs) {
			available = dequeue_fifo_input(filter, port);
			buffer = filter->input_buffers[port->data_index];
		} else {
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
		}
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
	if (!ready) {
		if (!fifo_inputs)
			recycle_inputs(filter);
		return;
	}

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

	if (fifo_inputs)
		for (i = 0; i < filter->n_data_inputs; i++)
			if (filter->input_available[i])
				ingress_trace_add(filter, 'G', filter->data_inputs[i],
						filter->input_buffers[i], 0);
	res = filter->events.process(filter->user_data,
			filter->process_inputs, filter->n_data_inputs,
			filter->process_outputs, filter->n_outputs);
	if (fifo_inputs)
		for (i = 0; i < filter->n_data_inputs; i++)
			if (filter->input_available[i])
				ingress_trace_add(filter, 'E', filter->data_inputs[i],
						filter->input_buffers[i], res);
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
		if (fifo_inputs)
			ingress_trace_add(filter, 'O', filter->outputs[i],
					filter->output_buffers[i], 0);
		res = pw_filter_queue_buffer(filter->outputs[i]->filter_port,
				filter->output_buffers[i]);
		if (fifo_inputs && res < 0)
			ingress_trace_add(filter, 'F', filter->outputs[i],
					filter->output_buffers[i], res);
		filter->output_buffers[i] = NULL;
	}
	recycle_inputs(filter);
	if (fifo_inputs)
		schedule_fifo_backlog(filter);
	return;

error:
	clear_output_chunks(filter);
	signal_process_error(filter, res);
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
	if (res == 0 && filter->progressive)
		atomic_store_explicit(&filter->progressive_stop, false,
			memory_order_release);
	if (res == 0 && filter->progressive &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE)) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(filter->progressive_cpu, &set);
		res = -pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
		if (res == 0)
			res = filter->events.prepare_progressive_worker(filter->user_data);
		if (res > 0)
			res = -EPROTO;
	} else if (res == 0 && filter->progressive) {
		filter->progressive_worker_ready = false;
		res = -pthread_create(&filter->progressive_worker, NULL,
				progressive_worker, filter);
		if (res == 0) {
			filter->progressive_worker_started = true;
			pthread_mutex_lock(&filter->progressive_mutex);
			while (!filter->progressive_worker_ready)
				pthread_cond_wait(&filter->progressive_cond,
					&filter->progressive_mutex);
			res = filter->progressive_prepare_result;
			pthread_mutex_unlock(&filter->progressive_mutex);
			if (res < 0) {
				atomic_store_explicit(&filter->progressive_stop,
					true, memory_order_release);
				pthread_join(filter->progressive_worker, NULL);
				filter->progressive_worker_started = false;
			}
		}
	}
	if (res == 0)
		atomic_store_explicit(&filter->prepared, true, memory_order_release);
	return res;
}

static int progressive_data_loop_barrier(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED,
		void *user_data SPA_UNUSED)
{
	return 0;
}

static int deactivate(struct pw_ndarray_filter *filter)
{
	int res = 0;
	bool was_prepared = atomic_exchange_explicit(&filter->prepared, false,
			memory_order_acq_rel);

	if (was_prepared && filter->progressive &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_PROGRESSIVE_INLINE) &&
	    filter->filter != NULL) {
		struct pw_loop *loop = pw_filter_get_data_loop(filter->filter);
		int barrier;

		atomic_store_explicit(&filter->progressive_cancel, true,
			memory_order_release);
		atomic_store_explicit(&filter->progressive_stop, true,
			memory_order_release);
		if (loop == NULL)
			return -EIO;
		barrier = pw_loop_invoke(loop, progressive_data_loop_barrier,
			0, NULL, 0, true, filter);
		if (barrier < 0)
			barrier = pw_loop_locked(loop, progressive_data_loop_barrier,
				0, NULL, 0, filter);
		if (barrier < 0)
			return barrier;
	}
	if (filter->progressive_worker_started) {
		struct pw_loop *loop = filter->filter == NULL ? NULL :
			pw_filter_get_data_loop(filter->filter);
		int barrier = 0;

		if (loop != NULL) {
			barrier = pw_loop_invoke(loop, progressive_data_loop_barrier,
				0, NULL, 0, true, filter);
			if (barrier < 0)
				barrier = pw_loop_locked(loop,
					progressive_data_loop_barrier,
					0, NULL, 0, filter);
			if (barrier < 0)
				return barrier;
		}
		atomic_store_explicit(&filter->progressive_cancel, true,
			memory_order_release);
		atomic_store_explicit(&filter->progressive_stop, true,
			memory_order_release);
		pthread_mutex_lock(&filter->progressive_mutex);
		pthread_cond_signal(&filter->progressive_cond);
		pthread_mutex_unlock(&filter->progressive_mutex);
		pthread_join(filter->progressive_worker, NULL);
		filter->progressive_worker_started = false;
		if (loop != NULL) {
			int completion = pw_loop_invoke(loop,
				complete_progressive_on_data_loop, 0,
				NULL, 0, true, filter);

			if (completion < 0)
				completion = pw_loop_locked(loop,
					complete_progressive_on_data_loop, 0,
					NULL, 0, filter);
			if (completion < 0)
				return completion;
		}
	}
	if (was_prepared && filter->events.deactivate != NULL)
		res = filter->events.deactivate(filter->user_data);
	return res > 0 ? -EPROTO : res;
}

static void fail_on_main_loop(struct pw_ndarray_filter *filter, int res,
		const char *message);

struct ndarray_filter_control_status {
	int64_t completed_token;
	int32_t run_control_result;
	enum pw_ao_run_control_state actual_state;
	int64_t completed_reset_token;
	int32_t reset_result;
};

static void get_control_status(const struct pw_ndarray_filter *filter,
		struct ndarray_filter_control_status *status)
{
	status->completed_token = filter->completed_token;
	status->run_control_result = filter->run_control_result;
	status->actual_state = filter->actual_state;
	status->completed_reset_token = filter->completed_reset_token;
	status->reset_result = filter->reset_result;
}

static int publish_props(struct pw_ndarray_filter *filter,
		const struct ndarray_filter_control_status *status)
{
	uint8_t initial[4096];
	struct spa_pod_dynamic_builder builder;
	const struct spa_pod *props = NULL;
	const struct spa_pod *params[3];
	uint32_t offsets[3];
	uint32_t n_built = 0, n_params = 0;
	int res = 0;

	spa_pod_dynamic_builder_init(&builder, initial, sizeof(initial), 4096);
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL) {
		offsets[n_built] = builder.b.state.offset;
		if (pw_ao_run_control_build_status(&builder.b,
				status->completed_token, status->run_control_result,
				status->actual_state) == NULL) {
			res = -ENOSPC;
			goto done;
		}
		n_built++;
	}
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL) {
		offsets[n_built] = builder.b.state.offset;
		if (pw_ao_reset_control_build_status(&builder.b,
				status->completed_reset_token, status->reset_result) == NULL) {
			res = -ENOSPC;
			goto done;
		}
		n_built++;
	}
	for (uint32_t i = 0; i < n_built; i++)
		params[n_params++] = SPA_PTROFF(builder.b.data, offsets[i],
				const struct spa_pod);
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) {
		res = filter->events.get_props(filter->user_data, &props);
		if (res > 0)
			res = -EPROTO;
		else if (res == 0 && (props == NULL ||
			    !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props) ||
			    SPA_POD_OBJECT_ID(props) != SPA_PARAM_Props))
			res = -EINVAL;
		if (res < 0)
			goto done;
		params[n_params++] = props;
	}
	res = pw_filter_update_params(filter->filter, NULL, params, n_params);
done:
	spa_pod_dynamic_builder_clean(&builder);
	return res;
}

static void properties_event(void *data, uint64_t count SPA_UNUSED)
{
	struct pw_ndarray_filter *filter = data;
	struct ndarray_filter_control_status status;
	int res;

	if (!atomic_exchange_explicit(&filter->properties_pending, false,
			memory_order_acq_rel))
		return;
	get_control_status(filter, &status);
	res = publish_props(filter, &status);
	if (res < 0)
		fail_on_main_loop(filter, res,
				"can't publish ndarray owner properties");
}

static int publish_run_control_status(struct pw_ndarray_filter *filter,
		int64_t token, int result,
		enum pw_ao_run_control_state actual_state)
{
	struct ndarray_filter_control_status status;
	int res;

	filter->actual_state = actual_state;
	get_control_status(filter, &status);
	status.completed_token = token;
	status.run_control_result = result;
	res = publish_props(filter, &status);
	if (res >= 0) {
		filter->completed_token = token;
		filter->run_control_result = result;
	}
	return res;
}

static int publish_reset_control_status(struct pw_ndarray_filter *filter,
		int64_t token, int result)
{
	struct ndarray_filter_control_status status;
	int res;

	get_control_status(filter, &status);
	status.completed_reset_token = token;
	status.reset_result = result;
	res = publish_props(filter, &status);
	if (res >= 0) {
		filter->completed_reset_token = token;
		filter->reset_result = result;
	}
	return res;
}

static int publish_run_control_status_or_fail(struct pw_ndarray_filter *filter,
		int64_t token, int result,
		enum pw_ao_run_control_state actual_state)
{
	int res = publish_run_control_status(filter, token, result, actual_state);

	if (res < 0)
		fail_on_main_loop(filter, res,
				"can't publish ndarray run-control status");
	return res;
}

static int publish_reset_control_status_or_fail(
		struct pw_ndarray_filter *filter, int64_t token, int result)
{
	int res = publish_reset_control_status(filter, token, result);

	if (res < 0)
		fail_on_main_loop(filter, res,
				"can't publish ndarray reset-control status");
	return res;
}

static int publish_owner_prop_info(struct pw_ndarray_filter *filter)
{
	const struct spa_pod *params[PW_NDARRAY_FILTER_MAX_PORTS];
	uint32_t n_params = 0;
	uint32_t index;
	int res = 0;

	for (index = 0; index < PW_NDARRAY_FILTER_MAX_PORTS; index++) {
		const struct spa_pod *info = NULL;

		res = filter->events.enum_prop_info(filter->user_data,
				index, &info);
		if (res == -ENOENT)
			break;
		if (res > 0)
			res = -EPROTO;
		if (res < 0)
			return res;
		if (info == NULL ||
		    !spa_pod_is_object_type(info, SPA_TYPE_OBJECT_PropInfo) ||
		    SPA_POD_OBJECT_ID(info) != SPA_PARAM_PropInfo)
			res = -EINVAL;
		if (res < 0)
			return res;
		params[n_params++] = info;
	}
	if (index == PW_NDARRAY_FILTER_MAX_PORTS)
		return -E2BIG;
	return n_params == 0 ? 0 : pw_filter_update_params(filter->filter,
			NULL, params, n_params);
}

static void complete_run_control(struct pw_ndarray_filter *filter, int result,
		enum pw_ao_run_control_state actual_state)
{
	publish_run_control_status_or_fail(filter,
			filter->last_request_token, result, actual_state);
	filter->requested_state = PW_AO_RUN_CONTROL_STATE_UNKNOWN;
}

static int update_processing_state(struct pw_ndarray_filter *filter,
		enum pw_filter_state state,
		enum pw_ao_run_control_state *actual)
{
	int res = 0;

	*actual = filter->actual_state;
	if (state == PW_FILTER_STATE_STREAMING &&
	    (!(filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL) ||
	     filter->requested_state == PW_AO_RUN_CONTROL_STATE_RUNNING ||
	     filter->actual_state == PW_AO_RUN_CONTROL_STATE_RUNNING)) {
		struct pw_loop *data_loop = pw_filter_get_data_loop(filter->filter);

		if (data_loop == NULL)
			res = -EIO;
		else
			res = pw_loop_invoke(data_loop, prepare_process_thread,
					0, NULL, 0, true, filter);
		if (res < 0 && filter->events.deactivate != NULL)
			filter->events.deactivate(filter->user_data);
		if (res >= 0)
			*actual = PW_AO_RUN_CONTROL_STATE_RUNNING;
	} else if (state == PW_FILTER_STATE_PAUSED ||
		   state == PW_FILTER_STATE_UNCONNECTED) {
		res = deactivate(filter);
		if (res >= 0)
			*actual = PW_AO_RUN_CONTROL_STATE_STOPPED;
	}
	return res;
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
	enum pw_ao_run_control_state actual;
	int res;

	atomic_store_explicit(&filter->state, state, memory_order_release);
	res = update_processing_state(filter, state, &actual);
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL) {
		if (res < 0 &&
		    filter->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN)
			complete_run_control(filter, res, actual);
		else if (res >= 0 && actual == filter->requested_state)
			complete_run_control(filter, 0, actual);
		else if (filter->requested_state ==
				 PW_AO_RUN_CONTROL_STATE_UNKNOWN &&
			 actual != filter->actual_state)
			publish_run_control_status_or_fail(filter,
					filter->completed_token, 0, actual);
	}
	if (res < 0)
		fail_on_main_loop(filter, res,
				state == PW_FILTER_STATE_STREAMING
					? "ndarray process-thread preparation failed"
					: "ndarray deactivation failed");
	if (state == PW_FILTER_STATE_ERROR) {
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

	if (param == NULL)
		return;
	if (data_port == NULL && id == SPA_PARAM_Props &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL)) {
		struct pw_ao_run_control_request request = { 0 };
		enum pw_filter_state state;

		res = pw_ao_run_control_parse_request(param, &request);
		if (res < 0 && res != -ENOENT) {
			if (request.token > 0)
				publish_run_control_status_or_fail(filter,
						request.token, res, filter->actual_state);
			return;
		}
		if (res == -ENOENT)
			goto reset_control;
		if (filter->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN) {
			publish_run_control_status_or_fail(filter, request.token,
					-EBUSY, filter->actual_state);
			return;
		}
		if (request.token <= filter->last_request_token) {
			publish_run_control_status_or_fail(filter, request.token,
					request.token == filter->last_request_token
						? -EALREADY : -ESTALE,
					filter->actual_state);
			return;
		}
		filter->last_request_token = request.token;
		if (request.requested_state == filter->actual_state) {
			complete_run_control(filter, 0, filter->actual_state);
			return;
		}
		filter->requested_state = request.requested_state;
		res = pw_filter_set_active(filter->filter,
				request.requested_state ==
				PW_AO_RUN_CONTROL_STATE_RUNNING);
		if (res < 0) {
			complete_run_control(filter, res, filter->actual_state);
			return;
		}
		state = atomic_load_explicit(&filter->state, memory_order_acquire);
		if ((request.requested_state == PW_AO_RUN_CONTROL_STATE_RUNNING &&
		     state == PW_FILTER_STATE_STREAMING) ||
		    (request.requested_state == PW_AO_RUN_CONTROL_STATE_STOPPED &&
		     state == PW_FILTER_STATE_PAUSED)) {
			enum pw_ao_run_control_state actual;

			res = update_processing_state(filter, state, &actual);
			complete_run_control(filter, res, actual);
		}
		return;
	}

reset_control:
	if (data_port == NULL && id == SPA_PARAM_Props &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL)) {
		struct pw_ao_reset_control_request request = { 0 };

		res = pw_ao_reset_control_parse_request(param, &request);
		if (res < 0 && res != -ENOENT) {
			if (request.token > 0)
				publish_reset_control_status_or_fail(filter,
						request.token, res);
			return;
		}
		if (res == -ENOENT)
			goto owner_properties;
		if (request.token <= filter->last_reset_token) {
			publish_reset_control_status_or_fail(filter, request.token,
					request.token == filter->last_reset_token
						? -EALREADY : -ESTALE);
			return;
		}
		if (filter->actual_state != PW_AO_RUN_CONTROL_STATE_STOPPED ||
		    filter->requested_state != PW_AO_RUN_CONTROL_STATE_UNKNOWN) {
			filter->last_reset_token = request.token;
			publish_reset_control_status_or_fail(filter, request.token,
					-EBUSY);
			return;
		}
		filter->last_reset_token = request.token;
		res = filter->events.reset(filter->user_data);
		if (res > 0)
			res = -EPROTO;
		if (publish_reset_control_status_or_fail(filter,
				request.token, res) < 0 || res < 0)
			return;
		if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES)
			pw_ndarray_filter_notify_properties(filter);
		return;
	}

owner_properties:
	if (data_port == NULL && id == SPA_PARAM_Props &&
	    (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES)) {
		struct ndarray_filter_control_status status;

		res = filter->events.set_props(filter->user_data, param);
		if (res > 0)
			res = -EPROTO;
		if (res < 0)
			fail_on_main_loop(filter, res,
					"invalid ndarray owner properties");
		else {
			get_control_status(filter, &status);
			res = publish_props(filter, &status);
			if (res < 0)
				fail_on_main_loop(filter, res,
						"can't publish ndarray requested properties");
		}
		return;
	}
	if (data_port == NULL || id != SPA_PARAM_Format)
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

static bool invalidate_retained_buffer(struct pw_ndarray_filter *filter,
		const struct ndarray_port *port, struct pw_buffer *buffer)
{
	if (port->direction == SPA_DIRECTION_INPUT &&
	    !(port->flags & PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER)) {
		uint32_t index = port->data_index;

		if (filter->input_buffers[index] == buffer) {
			filter->input_buffers[index] = NULL;
			filter->input_available[index] = false;
			return true;
		}
	} else if (port->direction == SPA_DIRECTION_OUTPUT) {
		uint32_t index = port->index;

		if (filter->output_buffers[index] == buffer) {
			filter->output_buffers[index] = NULL;
			return true;
		}
	}
	return false;
}

struct retained_buffer_removal {
	struct pw_ndarray_filter *filter;
	const struct ndarray_port *port;
	struct pw_buffer *buffer;
	bool invalidated;
	bool was_prepared;
};

struct progressive_removal_check {
	struct pw_ndarray_filter *filter;
	const struct ndarray_port *port;
	struct pw_buffer *buffer;
	bool active;
};

static int check_progressive_removal_on_data_loop(
		struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct progressive_removal_check *check = user_data;
	struct pw_ndarray_filter *filter = check->filter;

	check->active = filter->progressive_active &&
		(filter->input_buffers[0] == check->buffer ||
		 (check->port->direction == SPA_DIRECTION_OUTPUT &&
		  filter->output_buffers[check->port->index] == check->buffer));
	return 0;
}

static int invalidate_retained_buffer_on_data_loop(
		struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct retained_buffer_removal *removal = user_data;

	removal->invalidated = invalidate_retained_buffer(removal->filter,
			removal->port, removal->buffer);
	if (removal->invalidated)
		removal->was_prepared = atomic_exchange_explicit(
				&removal->filter->prepared, false,
				memory_order_acq_rel);
	return 0;
}

static void filter_remove_buffer(void *data, void *port_data,
		struct pw_buffer *buffer)
{
	struct pw_ndarray_filter *filter = data;
	struct port_data *data_port = port_data;
	struct retained_buffer_removal removal;
	struct pw_loop *data_loop;
	int res;

	if (data_port == NULL || data_port->port == NULL || buffer == NULL ||
	    atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return;
	if (filter->progressive) {
		struct progressive_removal_check check = {
			.filter = filter,
			.port = data_port->port,
			.buffer = buffer,
		};
		struct pw_loop *loop = filter->filter == NULL ? NULL :
			pw_filter_get_data_loop(filter->filter);

		if (loop != NULL && pw_loop_locked(loop,
			check_progressive_removal_on_data_loop,
			0, NULL, 0, &check) < 0) {
			fail_on_main_loop(filter, -EIO,
				"can't inspect progressive ndarray buffer removal");
			return;
		}
		if (check.active) {
			atomic_store_explicit(&filter->progressive_cancel, true,
				memory_order_release);
			(void)deactivate(filter);
			fail_on_main_loop(filter, -EPIPE,
				"progressive ndarray buffer removed before completion");
			return;
		}
	}
	removal = (struct retained_buffer_removal) {
		.filter = filter,
		.port = data_port->port,
		.buffer = buffer,
	};
	data_loop = filter->filter == NULL ? NULL :
		pw_filter_get_data_loop(filter->filter);
	if (data_loop == NULL)
		invalidate_retained_buffer_on_data_loop(NULL, false, 0,
				NULL, 0, &removal);
	else if ((res = pw_loop_locked(data_loop,
			invalidate_retained_buffer_on_data_loop, 0,
			NULL, 0, &removal)) < 0) {
		fail_on_main_loop(filter, res,
				"can't synchronize ndarray buffer removal");
		return;
	}
	if (!removal.invalidated)
		return;
	if (removal.was_prepared && filter->events.deactivate != NULL)
		(void)filter->events.deactivate(filter->user_data);
	fail_on_main_loop(filter, -EPIPE,
			"retained ndarray buffer removed before processing completed");
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = filter_destroyed,
	.state_changed = filter_state_changed,
	.param_changed = filter_param_changed,
	.remove_buffer = filter_remove_buffer,
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
	if (filter->progressive_worker_started)
		(void)deactivate(filter);
	stop_parameter_loop(filter);
	if (filter->parameter_event != NULL && filter->parameter_loop != NULL)
		pw_loop_destroy_source(pw_thread_loop_get_loop(filter->parameter_loop),
				filter->parameter_event);
	if (filter->parameter_loop != NULL)
		pw_thread_loop_destroy(filter->parameter_loop);
	if (filter->error_event != NULL && filter->main_loop != NULL)
		pw_loop_destroy_source(pw_main_loop_get_loop(filter->main_loop),
				filter->error_event);
	if (filter->fifo_process_event != NULL && filter->main_loop != NULL)
		pw_loop_destroy_source(pw_main_loop_get_loop(filter->main_loop),
				filter->fifo_process_event);
	if (filter->properties_event != NULL && filter->main_loop != NULL)
		pw_loop_destroy_source(pw_main_loop_get_loop(filter->main_loop),
				filter->properties_event);
	remove_progressive_event(filter);
	if (filter->filter != NULL)
		pw_filter_destroy(filter->filter);
	if (filter->main_loop != NULL)
		pw_main_loop_destroy(filter->main_loop);
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	ingress_trace_dump(&filter->ingress_trace);
	free(filter->ingress_trace.records);
	free(filter->ingress_trace.slots);
	free(filter->ingress_trace.path);
#endif
	if (filter->progressive_sync_initialized) {
		pthread_cond_destroy(&filter->progressive_cond);
		pthread_mutex_destroy(&filter->progressive_mutex);
	}
	free(filter->progressive_region_shape);
	free(filter->progressive_region_schema);
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
	res = posix_memalign((void **)&filter, _Alignof(*filter),
			sizeof(*filter));
	if (res != 0)
		return -res;
	memset(filter, 0, sizeof(*filter));
	atomic_init(&filter->state, PW_FILTER_STATE_UNCONNECTED);
	atomic_init(&filter->error, 0);
	atomic_init(&filter->prepared, false);
	atomic_init(&filter->destroying, false);
	atomic_init(&filter->fifo_process_scheduled, false);
	atomic_init(&filter->properties_pending, false);
	if ((res = copy_config(filter, config)) < 0)
		goto error;
	if (filter->progressive) {
		res = -pthread_mutex_init(&filter->progressive_mutex, NULL);
		if (res < 0)
			goto error;
		res = -pthread_cond_init(&filter->progressive_cond, NULL);
		if (res < 0) {
			pthread_mutex_destroy(&filter->progressive_mutex);
			goto error;
		}
		filter->progressive_sync_initialized = true;
		atomic_init(&filter->progressive_stop, false);
		atomic_init(&filter->progressive_cancel, false);
		atomic_init(&filter->progressive_done, false);
		atomic_init(&filter->progressive_spin_job, false);
	}
	filter->actual_state = PW_AO_RUN_CONTROL_STATE_STOPPED;
	if ((filter->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS) &&
	    (res = ingress_trace_init(&filter->ingress_trace,
			filter->n_data_inputs, filter->n_outputs)) < 0)
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
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS) {
		filter->fifo_process_event = pw_loop_add_event(
				pw_main_loop_get_loop(filter->main_loop),
				fifo_process_event, filter);
		if (filter->fifo_process_event == NULL) {
			res = errno != 0 ? -errno : -ENOMEM;
			goto error;
		}
	}
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) {
		filter->properties_event = pw_loop_add_event(
				pw_main_loop_get_loop(filter->main_loop),
				properties_event, filter);
		if (filter->properties_event == NULL) {
			res = errno != 0 ? -errno : -ENOMEM;
			goto error;
		}
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
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL)
		pw_properties_set(properties, PW_AO_RUN_CONTROL_KEY_ENABLED, "true");
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL)
		pw_properties_set(properties, PW_AO_RESET_CONTROL_KEY_ENABLED, "true");
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
	uint8_t buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	const struct spa_pod *params[3];
	uint32_t n_params = 0;
	int res;

	if (filter == NULL || filter->filter == NULL || filter->connected)
		return -EINVAL;
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_RT_PROCESS)
		flags |= PW_FILTER_FLAG_RT_PROCESS;
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS)
		flags |= PW_FILTER_FLAG_OUTPUT_RETURN_RETRY;
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL) {
		flags |= PW_FILTER_FLAG_INACTIVE;
		params[n_params] = pw_ao_run_control_build_status(&builder, 0, 0,
				PW_AO_RUN_CONTROL_STATE_STOPPED);
		if (params[n_params] == NULL)
			return -ENOSPC;
		n_params++;
	}
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL) {
		params[n_params] = pw_ao_reset_control_build_status(&builder, 0, 0);
		if (params[n_params] == NULL)
			return -ENOSPC;
		n_params++;
	}
	if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) {
		res = filter->events.get_props(filter->user_data, &params[n_params]);
		if (res > 0)
			return -EPROTO;
		if (res < 0)
			return res;
		if (params[n_params] == NULL ||
		    !spa_pod_is_object_type(params[n_params], SPA_TYPE_OBJECT_Props) ||
		    SPA_POD_OBJECT_ID(params[n_params]) != SPA_PARAM_Props)
			return -EINVAL;
		n_params++;
	}
	res = pw_filter_connect(filter->filter, flags,
			n_params > 0 ? params : NULL, n_params);
	if (res >= 0) {
		filter->connected = true;
		if (filter->progressive) {
			filter->progressive_data_loop =
				pw_filter_get_data_loop(filter->filter);
			if (filter->progressive_data_loop == NULL)
				return -EIO;
			res = pw_loop_invoke(filter->progressive_data_loop,
				add_progressive_event_on_data_loop,
				0, NULL, 0, true, filter);
			if (res < 0)
				return res;
		}
		if (filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES)
			res = publish_owner_prop_info(filter);
	}
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
int pw_ndarray_filter_notify_properties(struct pw_ndarray_filter *filter)
{
	int res;

	if (filter == NULL || filter->properties_event == NULL ||
	    !(filter->flags & PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES) ||
	    atomic_load_explicit(&filter->destroying, memory_order_acquire))
		return -EINVAL;
	if (atomic_exchange_explicit(&filter->properties_pending, true,
			memory_order_acq_rel))
		return 0;
	res = pw_loop_signal_event(pw_main_loop_get_loop(filter->main_loop),
			filter->properties_event);
	if (res < 0)
		atomic_store_explicit(&filter->properties_pending, false,
				memory_order_release);
	return res;
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
	if (filter->progressive)
		(void)deactivate(filter);
	remove_progressive_event(filter);
	if (filter->filter != NULL && filter->connected) {
		pw_filter_disconnect(filter->filter);
		filter->connected = false;
	}
	stop_parameter_loop(filter);
	deactivate(filter);
	free_filter(filter);
}
