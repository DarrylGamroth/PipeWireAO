/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

struct test_data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *source;
	struct spa_hook source_listener;
	struct spa_source *control;
	struct spa_source *timeout_timer;
	atomic_int source_state;
	atomic_uint produced;
	int result;
};

static void quit_with_error(struct test_data *data, const char *message)
{
	if (data->result == 0) {
		fprintf(stderr, "%s\n", message);
		data->result = 1;
	}
	pw_main_loop_quit(data->loop);
}

static void source_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct test_data *data = userdata;

	(void)old;
	atomic_store_explicit(&data->source_state, state, memory_order_release);
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "stream error: %s\n", error == NULL ? "unknown" : error);
		data->result = 1;
		pw_main_loop_quit(data->loop);
	} else if (state == PW_STREAM_STATE_STREAMING) {
		printf("STREAMING\n");
		fflush(stdout);
	}
}

static void source_process(void *userdata)
{
	struct test_data *data = userdata;
	struct pw_buffer *buffer;
	struct spa_data *block;
	struct spa_meta_header *header;
	uint32_t sequence;
	float value;

	buffer = pw_stream_dequeue_buffer(data->source);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
	    block->maxsize < sizeof(value)) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source received an invalid buffer");
		return;
	}
	sequence = atomic_fetch_add_explicit(&data->produced, 1,
			memory_order_relaxed) + 1;
	if (sequence > 3) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source produced too many buffers");
		return;
	}
	block->chunk->offset = 0;
	block->chunk->stride = sizeof(value);
	if (sequence == 1) {
		block->chunk->size = 0;
		(void)pw_stream_queue_buffer(data->source, buffer);
		printf("SOURCE 1 absent=1\n");
		fflush(stdout);
		return;
	}
	value = sequence == 2 ? 5.0f : 9.0f;
	memcpy(block->data, &value, sizeof(value));
	block->chunk->size = sizeof(value);
	header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source Header metadata is unavailable");
		return;
	}
	*header = (struct spa_meta_header) { .seq = sequence - 1u };
	(void)pw_stream_queue_buffer(data->source, buffer);
	printf("SOURCE %u value=%g\n", sequence, (double)value);
	fflush(stdout);
}

static const struct pw_stream_events source_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = source_state_changed,
	.process = source_process,
};

static void trigger_source(void *userdata, int fd, uint32_t mask)
{
	struct test_data *data = userdata;
	uint8_t command;

	if ((mask & SPA_IO_IN) == 0 || read(fd, &command, sizeof(command)) !=
			(ssize_t)sizeof(command)) {
		quit_with_error(data, "could not read ndarray source trigger");
		return;
	}
	if (command == 'q') {
		if (atomic_load_explicit(&data->produced,
			    memory_order_acquire) != 3)
			quit_with_error(data, "Parameter source stopped before three cycles");
		else
			pw_main_loop_quit(data->loop);
		return;
	}
	if (atomic_load_explicit(&data->source_state, memory_order_acquire) !=
			PW_STREAM_STATE_STREAMING ||
	    pw_stream_trigger_process(data->source) < 0)
		quit_with_error(data, "could not trigger ndarray source");
}

static void stop(void *userdata, int signal_number)
{
	struct test_data *data = userdata;

	(void)signal_number;
	if (atomic_load_explicit(&data->produced, memory_order_acquire) != 3)
		quit_with_error(data, "Parameter Port test interrupted");
	else
		pw_main_loop_quit(data->loop);
}

static void timeout(void *userdata, uint64_t expirations)
{
	struct test_data *data = userdata;

	(void)expirations;
	quit_with_error(data, "Parameter Port test timed out");
}

static struct spa_pod *build_format(struct spa_pod_builder *builder)
{
	const struct spa_ndarray_info info = SPA_NDARRAY_INFO_INIT(
		.element_type = SPA_ELEMENT_TYPE_F32_LE,
		.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
		.n_dimensions = 1,
		.shape = { 1 });

	return spa_format_ndarray_build(builder, SPA_PARAM_EnumFormat, &info);
}

static struct spa_pod *build_header_meta(struct spa_pod_builder *builder)
{
	return spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));
}

int main(int argc, char *argv[])
{
	static const char data_loops[] =
		"[ { loop.name=ndarray-parameter-source loop.class=data.rt "
		"loop.idle=eventfd } ]";
	struct test_data data = { .result = 0 };
	struct pw_properties *context_props, *source_props;
	struct spa_pod_builder builder;
	struct spa_pod *params[2];
	struct timespec timeout_value = { .tv_sec = 10 };
	uint8_t pods[512];
	int result;

	pw_init(&argc, &argv);
	atomic_init(&data.source_state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&data.produced, 0);
	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "could not create main loop: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	context_props = pw_properties_new("context.data-loops", data_loops, NULL);
	data.context = pw_context_new(pw_main_loop_get_loop(data.loop),
			context_props, 0);
	if (data.context == NULL) {
		fprintf(stderr, "could not create context: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	data.core = pw_context_connect(data.context, NULL, 0);
	if (data.core == NULL) {
		fprintf(stderr, "could not connect core: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	source_props = pw_properties_new(PW_KEY_NODE_NAME,
			"ndarray-parameter-source", PW_KEY_NODE_LOOP_NAME,
			"ndarray-parameter-source", PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Test",
			NULL);
	data.source = pw_stream_new(data.core, "ndarray Parameter source",
			source_props);
	if (data.source == NULL) {
		fprintf(stderr, "could not create stream: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_stream_add_listener(data.source, &data.source_listener,
			&source_events, &data);
	spa_pod_builder_init(&builder, pods, sizeof(pods));
	params[0] = build_format(&builder);
	params[1] = build_header_meta(&builder);
	if (params[0] == NULL || params[1] == NULL) {
		fprintf(stderr, "could not build ndarray stream parameters\n");
		data.result = 1;
		goto done;
	}
	result = pw_stream_connect(data.source, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT,
			(const struct spa_pod **)params, SPA_N_ELEMENTS(params));
	if (result < 0) {
		fprintf(stderr, "could not connect source: %s\n", spa_strerror(result));
		data.result = 1;
		goto done;
	}
	data.timeout_timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
			timeout, &data);
	if (data.timeout_timer == NULL) {
		fprintf(stderr, "could not create timeout timer: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_loop_update_timer(pw_main_loop_get_loop(data.loop), data.timeout_timer,
			&timeout_value, NULL, false);
	data.control = pw_loop_add_io(pw_main_loop_get_loop(data.loop),
			STDIN_FILENO, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false,
			trigger_source, &data);
	if (data.control == NULL) {
		fprintf(stderr, "could not create source control: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, stop, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, stop, &data);
	printf("READY\n");
	fflush(stdout);
	pw_main_loop_run(data.loop);
	if (data.result == 0 &&
	    atomic_load_explicit(&data.produced, memory_order_relaxed) != 3) {
		fprintf(stderr, "incomplete Parameter transfer: produced=%u\n",
				atomic_load_explicit(&data.produced,
					memory_order_relaxed));
		data.result = 1;
	}
	if (data.result == 0) {
		printf("RESULT produced=3 absent=1\n");
		fflush(stdout);
	}

done:
	if (data.source != NULL)
		pw_stream_destroy(data.source);
	if (data.core != NULL)
		pw_core_disconnect(data.core);
	if (data.context != NULL)
		pw_context_destroy(data.context);
	if (data.loop != NULL)
		pw_main_loop_destroy(data.loop);
	pw_deinit();
	return data.result;
}
