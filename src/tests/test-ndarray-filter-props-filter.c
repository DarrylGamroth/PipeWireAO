/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include <pipewire/ndarray-filter.h>

struct test_data {
	struct pw_ndarray_filter *filter;
	sigset_t stop_signals;
	float gain;
	uint32_t owner_updates;
	uint32_t resets;
	uint8_t props_buffer[256];
	uint8_t prop_info_buffers[2][256];
	const struct spa_pod *prop_info[2];
};

static int process(void *userdata SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *inputs,
		uint32_t n_inputs, struct pw_ndarray_filter_buffer *outputs,
		uint32_t n_outputs)
{
	return inputs == NULL && n_inputs == 0 && outputs == NULL && n_outputs == 0
		? 0 : -EINVAL;
}

static int enum_prop_info(void *userdata, uint32_t index,
		const struct spa_pod **info)
{
	struct test_data *data = userdata;

	if (index >= SPA_N_ELEMENTS(data->prop_info))
		return -ENOENT;
	*info = data->prop_info[index];
	return 0;
}

static int get_props(void *userdata, const struct spa_pod **props)
{
	struct test_data *data = userdata;
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data->props_buffer,
			sizeof(data->props_buffer));
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("owner:gain"), SPA_POD_Float(data->gain), 0);
	spa_pod_builder_pop(&builder, &values);
	*props = spa_pod_builder_pop(&builder, &object);
	return *props == NULL ? -ENOSPC : 0;
}

static int set_props(void *userdata, const struct spa_pod *props)
{
	struct test_data *data = userdata;
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	const char *name;
	struct spa_pod *value;
	float gain = 0.0f;
	bool found = false;

	params = spa_pod_find_prop(props, NULL, SPA_PROP_params);
	if (params == NULL)
		return -EINVAL;
	spa_pod_parser_pod(&parser, &params->value);
	if (spa_pod_parser_push_struct(&parser, &frame) < 0)
		return -EINVAL;
	while (spa_pod_parser_get_string(&parser, &name) == 0 &&
	       spa_pod_parser_get_pod(&parser, &value) == 0) {
		if (!spa_streq(name, "owner:gain") || found ||
		    spa_pod_get_float(value, &gain) < 0)
			return -EINVAL;
		found = true;
	}
	if (!found)
		return -EINVAL;
	data->gain = gain;
	data->owner_updates++;
	return 0;
}

static int reset(void *userdata)
{
	struct test_data *data = userdata;

	data->resets++;
	return 0;
}

static const struct pw_ndarray_filter_events events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.process = process,
	.enum_prop_info = enum_prop_info,
	.get_props = get_props,
	.set_props = set_props,
	.reset = reset,
};

static int build_prop_info(struct test_data *data, uint32_t index,
		const char *name)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(
			data->prop_info_buffers[index],
			sizeof(data->prop_info_buffers[index]));
	struct spa_pod_frame object;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(&builder,
			SPA_PROP_INFO_name, SPA_POD_String(name),
			SPA_PROP_INFO_type, SPA_POD_Float(0.0f), 0);
	data->prop_info[index] = spa_pod_builder_pop(&builder, &object);
	return data->prop_info[index] == NULL ? -ENOSPC : 0;
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
	struct test_data data = { .gain = 1.0f };
	static const uint32_t shape[] = { 1 };
	static const struct pw_ndarray_filter_port ports[] = {
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_INPUT,
			.name = "input",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.n_dimensions = 1,
				.shape = shape,
			},
		},
	};
	struct pw_ndarray_filter_config config = {
		.struct_size = sizeof(config),
		.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
		.node_name = "ndarray-props-filter",
		.n_ports = SPA_N_ELEMENTS(ports),
		.flags = PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES |
			PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL |
			PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL,
		.ports = ports,
		.events = &events,
		.user_data = &data,
	};
	pthread_t stop_thread;
	bool stop_thread_started = false;
	int result, status = 1;

	(void)argc;
	(void)argv;
	if ((result = build_prop_info(&data, 0, "owner:gain")) < 0 ||
	    (result = build_prop_info(&data, 1, "owner:offset")) < 0) {
		fprintf(stderr, "could not build PropInfo: %s\n", spa_strerror(result));
		return 1;
	}
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
	puts("READY");
	fflush(stdout);
	result = pw_ndarray_filter_run(data.filter);
	if (result < 0 || pw_ndarray_filter_get_error(data.filter) < 0) {
		fprintf(stderr, "ndarray filter failed: %s\n", spa_strerror(result));
		goto done;
	}
	if (data.owner_updates != 1 || data.resets != 1) {
		fprintf(stderr, "incomplete Props exercise: owner-updates=%u resets=%u\n",
				data.owner_updates, data.resets);
		goto done;
	}
	printf("RESULT owner-updates=1 resets=1\n");
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
