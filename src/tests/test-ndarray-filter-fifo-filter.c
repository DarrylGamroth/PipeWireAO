/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/param/ndarray.h>
#include <spa/utils/result.h>

#include <pipewire/ndarray-filter.h>

/* Observe opaque public handles; all calls still use the native implementation. */
static struct pw_filter *native_filter;
static struct pw_filter_events observed_events;
static void (*native_process)(void *, struct spa_io_position *);
static void observe_process(void *userdata, struct spa_io_position *position)
{
	printf("NATIVE_ACTIVATION\n");
	fflush(stdout);
	native_process(userdata, position);
}
static void *native_output;
static struct pw_filter *capture_new(struct pw_loop *loop, const char *name,
		struct pw_properties *props, const struct pw_filter_events *events,
		void *userdata);
static void *capture_port(struct pw_filter *filter, enum pw_direction direction,
		enum pw_filter_port_flags flags, size_t size, struct pw_properties *props,
		const struct spa_pod **params, uint32_t n_params);
#define pw_filter_new_simple capture_new
#define pw_filter_add_port capture_port
#ifdef TEST_DISABLE_FIFO_BACKLOG
static int suppress_backlog_request(struct pw_filter *filter SPA_UNUSED)
{
	printf("SUPPRESSED_BACKLOG_REQUEST\n");
	fflush(stdout);
	return 0;
}
#define pw_filter_trigger_process suppress_backlog_request
#endif
#define PW_ENABLE_DIAGNOSTIC_TRACE
#include "../pipewire/ndarray-filter.c"
#undef pw_filter_new_simple
#undef pw_filter_add_port
#ifdef TEST_DISABLE_FIFO_BACKLOG
#undef pw_filter_trigger_process
#endif

static struct pw_filter *capture_new(struct pw_loop *loop, const char *name,
		struct pw_properties *props, const struct pw_filter_events *events,
		void *userdata)
{
	observed_events = *events;
	native_process = events->process;
	observed_events.process = observe_process;
	pw_properties_set(props, PW_KEY_NODE_RELIABLE, "true");
	return native_filter = pw_filter_new_simple(loop, name, props, &observed_events, userdata);
}

static void *capture_port(struct pw_filter *filter, enum pw_direction direction,
		enum pw_filter_port_flags flags, size_t size, struct pw_properties *props,
		const struct spa_pod **params, uint32_t n_params)
{
	void *port = pw_filter_add_port(filter, direction, flags, size, props, params, n_params);
	if (direction == PW_DIRECTION_OUTPUT)
		native_output = port;
	return port;
}

struct test_data {
	struct pw_ndarray_filter *filter;
	sigset_t stop_signals;
	void *retained_output;
	atomic_uint callbacks;
	atomic_bool retained;
	struct pw_buffer *held[64];
	uint32_t n_held;
};

static int fifo_prepare(void *userdata SPA_UNUSED)
{
	printf("PREPARED\n");
	fflush(stdout);
	return 0;
}

static int fifo_process(void *userdata,
		const struct pw_ndarray_filter_buffer *inputs, uint32_t n_inputs,
		struct pw_ndarray_filter_buffer *outputs, uint32_t n_outputs)
{
	struct test_data *data = userdata;
	const struct pw_ndarray_filter_buffer *input = &inputs[0];
	struct pw_ndarray_filter_buffer *output = &outputs[0];
	uint32_t packet = atomic_fetch_add_explicit(&data->callbacks, 1,
			memory_order_relaxed) + 1;
	float value;
	uint64_t sequence = 100 + (packet - 1) / 4;
	uint32_t row = ((packet - 1) % 4) * 2;
	static const uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 1 };
	struct spa_meta_acquisition expected;
	spa_meta_acquisition_init(&expected);
	spa_meta_acquisition_set_identity(&expected, domain, 7, sequence);
	spa_meta_acquisition_set_exposure_start(&expected, 100000 + packet, 50);

	if (n_inputs != 1 || n_outputs != 1 || packet > 8 ||
	    input->size != sizeof(value) || output->size != sizeof(value) ||
	    input->data == NULL || output->data == NULL ||
	    (input->metadata_valid & PW_NDARRAY_FILTER_METADATA_HEADER) == 0 ||
	    (input->metadata_valid & PW_NDARRAY_FILTER_METADATA_ACQUISITION) == 0 ||
	    input->header.seq != sequence || input->header.offset != row ||
	    input->header.pts != 1000 + (int64_t)sequence ||
	    input->header.dts_offset != 0 ||
	    input->header.flags != (packet % 4 == 0 ? SPA_META_HEADER_FLAG_MARKER : 0) ||
	    memcmp(&input->acquisition, &expected, sizeof(expected)) != 0)
		return -EINVAL;
	memcpy(&value, input->data, sizeof(value));
	if (value != (float)packet)
		return -EINVAL;
	printf("CALLBACK packet=%u seq=%"PRIu64" row=%u\n", packet, sequence, row);
	if (packet == 1) {
		data->retained_output = output->data;
		value = 41.0f;
		memcpy(output->data, &value, sizeof(value));
		output->flags |= PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE;
		printf("DEFERRED packet=1\n");
	} else {
		if (packet == 2) {
			memcpy(&value, output->data, sizeof(value));
			if (output->data != data->retained_output || value != 41.0f)
				return -EINVAL;
			atomic_store_explicit(&data->retained, true, memory_order_release);
		}
		value = (float)packet;
		memcpy(output->data, &value, sizeof(value));
		output->header = input->header;
		output->acquisition = input->acquisition;
		output->metadata_valid |= PW_NDARRAY_FILTER_METADATA_HEADER |
			PW_NDARRAY_FILTER_METADATA_ACQUISITION;
	}
	fflush(stdout);
	return 0;
}

static const struct pw_ndarray_filter_events fifo_events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.prepare_process_thread = fifo_prepare,
	.process = fifo_process,
};

static int capacity_action(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t sequence, const void *payload SPA_UNUSED,
		size_t size SPA_UNUSED, void *userdata)
{
	struct test_data *data = userdata;
	struct pw_buffer *buffer;
	int result;
	if (sequence == 'H') {
		while ((buffer = pw_filter_dequeue_buffer(native_output)) != NULL) {
			if (data->n_held == SPA_N_ELEMENTS(data->held)) {
				(void)pw_filter_queue_buffer(native_output, buffer);
				return -EOVERFLOW;
			}
			data->held[data->n_held++] = buffer;
		}
		if (data->n_held == 0)
			return -ENOBUFS;
		printf("HELD_OUTPUT_POOL %u\n", data->n_held);
	} else if (sequence == 'R') {
		if (data->n_held == 0)
			return -EINVAL;
		for (uint32_t i = 0; i < data->n_held; i++) {
			data->held[i]->buffer->datas[0].chunk->size = 0;
			if ((result = pw_filter_queue_buffer(native_output, data->held[i])) < 0)
				return result;
		}
		printf("RELEASED_OUTPUT_POOL %u\n", data->n_held);
		data->n_held = 0;
		/* This wake refers to returned capacity, never another source frame. */
		if ((result = pw_filter_trigger_process(native_filter)) < 0)
			return result;
	} else {
		return -EINVAL;
	}
	fflush(stdout);
	return 0;
}

static void capacity_control(void *userdata, int fd, uint32_t mask)
{
	struct test_data *data = userdata;
	char command;
	int result;
	if (!(mask & SPA_IO_IN) || read(fd, &command, 1) != 1)
		return;
	result = pw_loop_invoke(pw_filter_get_data_loop(native_filter), capacity_action,
			(uint32_t)command, NULL, 0, true, data);
	if (result < 0) {
		fprintf(stderr, "native capacity action failed: %s\n", spa_strerror(result));
		pw_ndarray_filter_quit(data->filter);
	}
}

static void *wait_for_stop(void *userdata)
{
	struct test_data *data = userdata;
	int signal_number;

	if (sigwait(&data->stop_signals, &signal_number) == 0)
		(void)pw_ndarray_filter_quit(data->filter);
	return NULL;
}

int main(int argc, char *argv[])
{
	static const uint32_t shape[] = { 1 };
	static const struct pw_ndarray_filter_port ports[] = {
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_INPUT,
			.name = "input",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.rate_num = 1,
				.rate_denom = 1,
				.n_dimensions = 1,
				.shape = shape,
				.schema = "org.pipewire.test.ndarray-fifo/1",
			},
		},
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_OUTPUT,
			.name = "output",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.rate_num = 1,
				.rate_denom = 1,
				.n_dimensions = 1,
				.shape = shape,
				.schema = "org.pipewire.test.ndarray-fifo/1",
			},
		},
	};
	struct test_data data = { 0 };
	struct pw_ndarray_filter_config config = {
		.struct_size = sizeof(config),
		.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
		.node_name = "ndarray-fifo-filter",
		.n_ports = SPA_N_ELEMENTS(ports),
		.flags = PW_NDARRAY_FILTER_FLAG_RT_PROCESS | PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS,
		.ports = ports,
		.events = &fifo_events,
		.user_data = &data,
	};
	pthread_t stop_thread;
	bool stop_thread_started = false;
	int result, status = 1;

	if (argc != 1) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 2;
	}
	atomic_init(&data.callbacks, 0);
	atomic_init(&data.retained, false);
	sigemptyset(&data.stop_signals);
	sigaddset(&data.stop_signals, SIGINT);
	sigaddset(&data.stop_signals, SIGTERM);
	if ((result = pthread_sigmask(SIG_BLOCK, &data.stop_signals, NULL)) != 0) {
		fprintf(stderr, "could not block stop signals: %s\n", strerror(result));
		return 1;
	}
	if ((result = pw_ndarray_filter_new(&config, &data.filter)) < 0) {
		fprintf(stderr, "could not create ndarray filter: %s\n",
				spa_strerror(result));
		return 1;
	}
	if ((result = pw_ndarray_filter_connect(data.filter)) < 0) {
		fprintf(stderr, "could not connect ndarray filter: %s\n",
				spa_strerror(result));
		goto done;
	}
	struct pw_loop *main_loop = pw_context_get_main_loop(
		pw_core_get_context(pw_filter_get_core(native_filter)));
	struct spa_source *control = pw_loop_add_io(main_loop, STDIN_FILENO, SPA_IO_IN, false, capacity_control, &data);
	if (control == NULL)
		goto done;
	result = pthread_create(&stop_thread, NULL, wait_for_stop, &data);
	if (result != 0) {
		fprintf(stderr, "could not create stop thread: %s\n", strerror(result));
		goto done;
	}
	stop_thread_started = true;
	printf("READY\n");
	fflush(stdout);
	result = pw_ndarray_filter_run(data.filter);
	if (result < 0) {
		fprintf(stderr, "ndarray filter failed: %s\n", spa_strerror(result));
		goto done;
	}
	printf("SUMMARY callbacks=%u retained=%u error=%d\n",
		atomic_load_explicit(&data.callbacks, memory_order_acquire),
		atomic_load_explicit(&data.retained, memory_order_acquire),
		pw_ndarray_filter_get_error(data.filter));
	fflush(stdout);
	if (atomic_load_explicit(&data.callbacks, memory_order_acquire) != 8 ||
	    !atomic_load_explicit(&data.retained, memory_order_acquire) ||
	    pw_ndarray_filter_get_error(data.filter) != 0) {
		fprintf(stderr, "incomplete retention proof: callbacks=%u retained=%u\n",
				atomic_load_explicit(&data.callbacks, memory_order_relaxed),
				atomic_load_explicit(&data.retained, memory_order_relaxed));
		goto done;
	}
	printf("RESULT callbacks=%u retained=%u\n",
			atomic_load_explicit(&data.callbacks, memory_order_relaxed),
			atomic_load_explicit(&data.retained, memory_order_relaxed));
	fflush(stdout);
	status = 0;

done:
	if (stop_thread_started) {
		(void)pthread_kill(stop_thread, SIGTERM);
		(void)pthread_join(stop_thread, NULL);
	}
	pw_ndarray_filter_destroy(data.filter);
	return status;
}
