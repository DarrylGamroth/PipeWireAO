/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/param/ndarray-utils.h>
#include <spa/utils/result.h>

#include <pipewire/impl.h>

enum source_phase {
	SOURCE_IDLE,
	SOURCE_ABSENT,
	SOURCE_WAIT_RESUME,
	SOURCE_RESUME,
	SOURCE_WAIT_OUTPUT,
};

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *source;
	struct pw_stream *sink;
	struct pw_impl_module *module;
	struct spa_hook source_listener;
	struct spa_hook sink_listener;
	struct spa_source *timer;
	struct spa_source *control;
	atomic_int source_phase;
	atomic_bool sink_replaced;
	int result;
};

static void fail(struct test_data *data, const char *message)
{
	if (data->result == 0)
		fprintf(stderr, "%s\n", message);
	data->result = 1;
	pw_main_loop_quit(data->main_loop);
}

static struct spa_pod *build_format(struct spa_pod_builder *builder)
{
	static const uint32_t shape[] = { 1 };
	struct spa_pod_frame object;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
			SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR), 0);
	return spa_pod_builder_pop(builder, &object);
}

static int connect_sink(struct test_data *data)
{
	struct pw_properties *properties;
	struct spa_pod_builder builder;
	struct spa_pod *params[1];
	uint8_t buffer[256];

	properties = pw_properties_new(PW_KEY_NODE_NAME,
			"ndarray-buffer-removal-sink", PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Test", NULL);
	if ((data->sink = pw_stream_new(data->core, "ndarray buffer removal sink",
			properties)) == NULL)
		return -errno;
	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[0] = build_format(&builder);
	if (params[0] == NULL)
		return -ENOSPC;
	return pw_stream_connect(data->sink, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT, (const struct spa_pod **)params,
			SPA_N_ELEMENTS(params));
}

static void source_state_changed(void *userdata,
		enum pw_stream_state old SPA_UNUSED, enum pw_stream_state state,
		const char *error)
{
	struct test_data *data = userdata;

	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "source stream error: %s\n", error);
		fail(data, "source stream entered ERROR");
	} else if (state == PW_STREAM_STATE_STREAMING) {
		puts("SOURCE_STREAMING");
		fflush(stdout);
	}
}

static void sink_state_changed(void *userdata,
		enum pw_stream_state old SPA_UNUSED, enum pw_stream_state state,
		const char *error)
{
	struct test_data *data = userdata;

	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "sink stream error: %s\n", error);
		fail(data, "sink stream entered ERROR");
	} else if (state == PW_STREAM_STATE_STREAMING) {
		puts(atomic_load_explicit(&data->sink_replaced,
				memory_order_acquire) ?
				"SINK_REPLACEMENT_STREAMING" : "SINK_STREAMING");
		fflush(stdout);
	}
}

static void source_process(void *userdata)
{
	struct test_data *data = userdata;
	enum source_phase phase = atomic_load_explicit(&data->source_phase,
			memory_order_acquire);
	struct pw_buffer *buffer;
	struct spa_data *block;
	float value = 4.0f;

	if (phase == SOURCE_ABSENT) {
		/* Leave the available output buffer queued. The source consequently
		 * completes this driver cycle with SPA_STATUS_NEED_DATA, so the filter
		 * sees a genuinely absent required input while it still owns an output. */
		atomic_store_explicit(&data->source_phase, SOURCE_WAIT_RESUME,
				memory_order_release);
		puts("INPUT_ABSENT");
		fflush(stdout);
		return;
	}
	if (phase != SOURCE_RESUME)
		return;
	buffer = pw_stream_dequeue_buffer(data->source);
	if (buffer == NULL) {
		fail(data, "source had no buffer after output pool replacement");
		return;
	}
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
	    block->maxsize < sizeof(value)) {
		fail(data, "source received an invalid buffer after pool replacement");
		return;
	}
	memcpy(block->data, &value, sizeof(value));
	*block->chunk = (struct spa_chunk) {
		.offset = 0,
		.size = sizeof(value),
		.stride = sizeof(value),
	};
	atomic_store_explicit(&data->source_phase, SOURCE_WAIT_OUTPUT,
			memory_order_release);
	if (pw_stream_queue_buffer(data->source, buffer) < 0) {
		fail(data, "could not publish the resumed input");
		return;
	}
	puts("INPUT_RESUMED");
	fflush(stdout);
}

static void sink_process(void *userdata)
{
	struct test_data *data = userdata;
	struct pw_buffer *buffer = pw_stream_dequeue_buffer(data->sink);
	struct spa_data *block;
	float value;

	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
	    block->chunk->size != sizeof(value)) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		fail(data, "sink received an invalid resumed output");
		return;
	}
	memcpy(&value, SPA_PTROFF(block->data, block->chunk->offset, void),
			sizeof(value));
	(void)pw_stream_queue_buffer(data->sink, buffer);
	if (value != 4.0f) {
		fail(data, "sink received the wrong resumed output");
		return;
	}
	puts("RESULT output=4 after output-pool replacement");
	fflush(stdout);
	pw_main_loop_quit(data->main_loop);
}

static const struct pw_stream_events source_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = source_state_changed,
	.process = source_process,
};

static const struct pw_stream_events sink_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = sink_state_changed,
	.process = sink_process,
};

static int make_sink(struct test_data *data)
{
	int res;

	res = connect_sink(data);
	if (res < 0)
		return res;
	pw_stream_add_listener(data->sink, &data->sink_listener, &sink_events, data);
	return 0;
}

static void control(void *userdata, int fd, uint32_t mask)
{
	struct test_data *data = userdata;
	char command;
	int res;

	if ((mask & SPA_IO_IN) == 0 || read(fd, &command, 1) != 1) {
		fail(data, "could not read test command");
		return;
	}
	switch (command) {
	case 'a':
		atomic_store_explicit(&data->source_phase, SOURCE_ABSENT,
				memory_order_release);
		res = pw_stream_trigger_process(data->source);
		if (res < 0)
			fail(data, "could not trigger the absent-input cycle");
		break;
	case 'd':
		if (atomic_load_explicit(&data->source_phase,
				memory_order_acquire) != SOURCE_WAIT_RESUME ||
		    data->sink == NULL) {
			fail(data, "output pool removal was requested before input became absent");
			break;
		}
		pw_stream_destroy(data->sink);
		data->sink = NULL;
		puts("OUTPUT_POOL_REMOVED");
		fflush(stdout);
		break;
	case 'n':
		if (data->sink != NULL || (res = make_sink(data)) < 0) {
			fail(data, "could not create the replacement output pool");
			break;
		}
		atomic_store_explicit(&data->sink_replaced, true,
				memory_order_release);
		puts("OUTPUT_POOL_REPLACED");
		fflush(stdout);
		break;
	case 'r':
		if (!atomic_load_explicit(&data->sink_replaced,
				memory_order_acquire)) {
			fail(data, "resume was requested before output pool replacement");
			break;
		}
		atomic_store_explicit(&data->source_phase, SOURCE_RESUME,
				memory_order_release);
		res = pw_stream_trigger_process(data->source);
		if (res < 0)
			fail(data, "could not trigger the resumed input");
		break;
	default:
		fail(data, "unknown test command");
	}
}

static void timeout(void *userdata, uint64_t count SPA_UNUSED)
{
	fail(userdata, "ndarray filter-chain output-pool replacement test timed out");
}

int main(int argc, char *argv[])
{
	struct test_data data = { 0 };
	struct pw_properties *source_properties;
	struct spa_pod_builder builder;
	struct spa_pod *params[1];
	struct timespec deadline = { .tv_sec = 10 };
	uint8_t buffer[256];
	char *args;

	if (argc != 2)
		return 2;
	pw_init(&argc, &argv);
	atomic_init(&data.source_phase, SOURCE_IDLE);
	atomic_init(&data.sink_replaced, false);
	data.main_loop = pw_main_loop_new(NULL);
	if (data.main_loop == NULL)
		return 1;
	data.context = pw_context_new(pw_main_loop_get_loop(data.main_loop), NULL, 0);
	if (data.context == NULL) {
		data.result = 1;
		goto done;
	}
	data.core = pw_context_connect(data.context, NULL, 0);
	if (data.core == NULL) {
		data.result = 1;
		goto done;
	}
	spa_assert_se(asprintf(&args,
		"node.name = ndarray-buffer-removal-filter "
		"filter.graph = { nodes = [ { type = ndarray name = scale "
		"plugin = \"%s\" label = scale-f32 config = { shape = [ 1 ] } } ] "
		"inputs = [ \"scale:in\" ] outputs = [ \"scale:out\" ] }", argv[1]) > 0);
	data.module = pw_context_load_module(data.context,
			"libpipewire-module-ndarray-filter-chain", args, NULL);
	free(args);
	if (data.module == NULL) {
		data.result = 1;
		goto done;
	}
	source_properties = pw_properties_new(PW_KEY_NODE_NAME,
			"ndarray-buffer-removal-source", PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Test", NULL);
	data.source = pw_stream_new(data.core, "ndarray buffer removal source",
			source_properties);
	if (data.source == NULL || make_sink(&data) < 0) {
		data.result = 1;
		goto done;
	}
	pw_stream_add_listener(data.source, &data.source_listener, &source_events, &data);
	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[0] = build_format(&builder);
	if (params[0] == NULL || pw_stream_connect(data.source,
			PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_DRIVER |
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT, (const struct spa_pod **)params,
			SPA_N_ELEMENTS(params)) < 0) {
		data.result = 1;
		goto done;
	}
	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.main_loop), timeout,
			&data);
	data.control = pw_loop_add_io(pw_main_loop_get_loop(data.main_loop),
			STDIN_FILENO, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false, control, &data);
	if (data.timer == NULL || data.control == NULL || pw_loop_update_timer(
			pw_main_loop_get_loop(data.main_loop), data.timer, &deadline,
			NULL, false) < 0) {
		data.result = 1;
		goto done;
	}
	puts("READY");
	fflush(stdout);
	pw_main_loop_run(data.main_loop);

done:
	if (data.sink != NULL)
		pw_stream_destroy(data.sink);
	if (data.source != NULL)
		pw_stream_destroy(data.source);
	if (data.module != NULL)
		pw_impl_module_destroy(data.module);
	if (data.core != NULL)
		pw_core_disconnect(data.core);
	if (data.context != NULL)
		pw_context_destroy(data.context);
	if (data.main_loop != NULL)
		pw_main_loop_destroy(data.main_loop);
	pw_deinit();
	return data.result;
}
