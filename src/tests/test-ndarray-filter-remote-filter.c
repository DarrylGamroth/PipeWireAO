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
#include "metadata-fixture.h"

struct test_data {
	struct pw_ndarray_filter *filter;
	sigset_t stop_signals;
	void *retained_output;
	atomic_uint callbacks;
	atomic_bool retained;
	bool metadata;
	bool trigger_mismatch_output;
};

static int prepare_process_thread(void *userdata SPA_UNUSED)
{
	printf("PREPARED\n");
	fflush(stdout);
	return 0;
}

static int process(void *userdata,
		const struct pw_ndarray_filter_buffer *inputs, uint32_t n_inputs,
		struct pw_ndarray_filter_buffer *outputs, uint32_t n_outputs)
{
	struct test_data *data = userdata;
	const struct pw_ndarray_filter_buffer *input;
	struct pw_ndarray_filter_buffer *output;
	uint32_t callback;
	float input_value, output_value;

	if (n_inputs != 1 || n_outputs != 1)
		return -EINVAL;
	input = &inputs[0];
	output = &outputs[0];
	if (input->size != sizeof(input_value) || output->size != sizeof(output_value) ||
	    input->data == NULL || output->data == NULL || test_header(input) == NULL || test_header(output) == NULL ||
	    !test_check_acquisition(input->metas, input->n_metas, test_header(input)->seq, 100000 + test_header(input)->seq))
		return -EINVAL;
	if (!spa_meta_records_copy(output->metas, output->n_metas, input->metas, input->n_metas)) return -ENOSPC;
	memcpy(&input_value, input->data, sizeof(input_value));
	callback = atomic_fetch_add_explicit(&data->callbacks, 1,
			memory_order_relaxed) + 1;

	if (callback == 1) {
		if (input_value != 1.0f || test_header(input)->seq != 1)
			return -EINVAL;
		data->retained_output = output->data;
		output_value = 41.0f;
		memcpy(output->data, &output_value, sizeof(output_value));
		output->flags |= PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE;
		printf("DEFERRED\n");
		fflush(stdout);
		return 0;
	}
	if (callback != 2 || input_value != 2.0f ||
	    test_header(input)->seq != 2 || output->data != data->retained_output)
		return -EINVAL;
	memcpy(&output_value, output->data, sizeof(output_value));
	if (output_value != 41.0f)
		return -EINVAL;

	atomic_store_explicit(&data->retained, true, memory_order_release);
	output_value = 3.0f;
	memcpy(output->data, &output_value, sizeof(output_value));
	*test_header(output) = (struct spa_meta_header) {
		.flags = SPA_META_HEADER_FLAG_MARKER | SPA_META_HEADER_FLAG_DISCONT,
		.offset = 22,
		.pts = 2002,
		.dts_offset = 0,
		.seq = 2,
	};
	if (data->trigger_mismatch_output) {
		struct spa_meta *m = spa_meta_records_find(output->metas, output->n_metas, SPA_META_ElectricalTrigger);
		if (m == NULL) return -EINVAL;
		*(struct spa_meta_electrical_trigger *)m->data = (struct spa_meta_electrical_trigger){SPA_META_RECORD_VALID,4};
	}
	printf("COMPLETE\n");
	fflush(stdout);
	return 0;
}

static const struct pw_ndarray_filter_events events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.prepare_process_thread = prepare_process_thread,
	.process = process,
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
	struct pw_ndarray_filter_port ports[] = {
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_INPUT,
			.n_metas = SPA_N_ELEMENTS(test_metadata), .metas = test_metadata,
			.name = "input",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.rate_num = 1,
				.rate_denom = 1,
				.n_dimensions = 1,
				.shape = shape,
				.schema = "org.pipewire.test.ndarray-retention/1",
			},
		},
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_OUTPUT,
			.n_metas = SPA_N_ELEMENTS(test_metadata), .metas = test_metadata, .propagation = SPA_META_PROPAGATION_PRESERVE,
			.name = "output",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.rate_num = 1,
				.rate_denom = 1,
				.n_dimensions = 1,
				.shape = shape,
				.schema = "org.pipewire.test.ndarray-retention/1",
			},
		},
	};
	struct test_data data = { 0 };
	struct pw_ndarray_filter_config config = {
		.struct_size = sizeof(config),
		.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
		.node_name = "ndarray-retention-filter",
		.n_ports = SPA_N_ELEMENTS(ports),
		.flags = PW_NDARRAY_FILTER_FLAG_RT_PROCESS,
		.ports = ports,
		.events = &events,
		.user_data = &data,
	};
	pthread_t stop_thread;
	bool stop_thread_started = false;
	int result, status = 1;

	for (int i = 1; i < argc; i++) {
		if (spa_streq(argv[i], "--fifo-inputs"))
			config.flags |= PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS;
		else if (spa_streq(argv[i], "--malformed-output")) {
			data.trigger_mismatch_output = true;
		} else if (spa_streq(argv[i], "--metadata")) {
			data.metadata = true;
		} else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return 2;
		}
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
	if (atomic_load_explicit(&data.callbacks, memory_order_acquire) != 2 ||
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
