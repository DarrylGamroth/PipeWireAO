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

struct test_data {
	struct pw_ndarray_filter *filter;
	sigset_t stop_signals;
	pthread_t process_thread;
	const void *retained_parameter;
	atomic_uint process_callbacks;
	atomic_uint parameter_callbacks;
	atomic_bool process_thread_prepared;
	atomic_bool parameter_prepared;
};

static int prepare_process_thread(void *userdata)
{
	struct test_data *data = userdata;

	data->process_thread = pthread_self();
	atomic_store_explicit(&data->process_thread_prepared, true,
			memory_order_release);
	printf("PREPARED\n");
	fflush(stdout);
	return 0;
}

static int process(void *userdata,
		const struct pw_ndarray_filter_buffer *inputs, uint32_t n_inputs,
		struct pw_ndarray_filter_buffer *outputs, uint32_t n_outputs)
{
	struct test_data *data = userdata;

	if (inputs != NULL || n_inputs != 0 || outputs != NULL || n_outputs != 0)
		return -EINVAL;
	atomic_fetch_add_explicit(&data->process_callbacks, 1,
			memory_order_relaxed);
	return 0;
}

static int update_parameter(void *userdata, uint32_t input_port,
		const struct pw_ndarray_filter_buffer *parameter)
{
	struct test_data *data = userdata;
	uint32_t callback;
	float value;

	if (!atomic_load_explicit(&data->process_thread_prepared,
		    memory_order_acquire) ||
	    pthread_equal(data->process_thread, pthread_self()) ||
	    input_port != 0 || parameter == NULL ||
	    parameter->size != sizeof(value) ||
	    parameter->capacity < sizeof(value) || parameter->data == NULL ||
	    (parameter->metadata_valid & PW_NDARRAY_FILTER_METADATA_HEADER) == 0 ||
	    parameter->header.seq != 1)
		return -EINVAL;
	memcpy(&value, parameter->data, sizeof(value));
	if (value != 5.0f)
		return -EINVAL;
	callback = atomic_fetch_add_explicit(&data->parameter_callbacks, 1,
			memory_order_relaxed) + 1;
	if (callback == 1) {
		data->retained_parameter = parameter->data;
		printf("PARAMETER_BUSY\n");
		fflush(stdout);
		return -EBUSY;
	}
	if (callback != 2 || parameter->data != data->retained_parameter)
		return -EINVAL;
	atomic_store_explicit(&data->parameter_prepared, true,
			memory_order_release);
	printf("PARAMETER_READY\n");
	fflush(stdout);
	return 0;
}

static const struct pw_ndarray_filter_events events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.prepare_process_thread = prepare_process_thread,
	.process = process,
	.update_parameter = update_parameter,
};

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
			.flags = PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER,
			.direction = SPA_DIRECTION_INPUT,
			.name = "parameter",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.n_dimensions = 1,
				.shape = shape,
			},
		},
	};
	struct test_data data = { 0 };
	struct pw_ndarray_filter_config config = {
		.struct_size = sizeof(config),
		.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
		.node_name = "ndarray-parameter-filter",
		.n_ports = SPA_N_ELEMENTS(ports),
		.flags = PW_NDARRAY_FILTER_FLAG_RT_PROCESS,
		.ports = ports,
		.events = &events,
		.user_data = &data,
	};
	pthread_t stop_thread;
	bool stop_thread_started = false;
	int result, status = 1;

	(void)argc;
	(void)argv;
	atomic_init(&data.process_callbacks, 0);
	atomic_init(&data.parameter_callbacks, 0);
	atomic_init(&data.process_thread_prepared, false);
	atomic_init(&data.parameter_prepared, false);
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
	if (atomic_load_explicit(&data.process_callbacks,
		    memory_order_acquire) < 2 ||
	    atomic_load_explicit(&data.parameter_callbacks,
		    memory_order_acquire) != 2 ||
	    !atomic_load_explicit(&data.parameter_prepared,
		    memory_order_acquire) ||
	    pw_ndarray_filter_get_error(data.filter) != 0) {
		fprintf(stderr, "incomplete Parameter Port proof: process=%u "
				"parameter=%u prepared=%u\n",
				atomic_load_explicit(&data.process_callbacks,
					memory_order_relaxed),
				atomic_load_explicit(&data.parameter_callbacks,
					memory_order_relaxed),
				atomic_load_explicit(&data.parameter_prepared,
					memory_order_relaxed));
		goto done;
	}
	printf("RESULT process=%u parameter=2 retained=1\n",
			atomic_load_explicit(&data.process_callbacks,
				memory_order_relaxed));
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
