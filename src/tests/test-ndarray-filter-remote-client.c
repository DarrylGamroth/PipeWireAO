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
	struct pw_stream *sink;
	struct spa_hook source_listener;
	struct spa_hook sink_listener;
	struct spa_source *control;
	struct spa_source *timeout_timer;
	atomic_int source_state;
	atomic_int sink_state;
	atomic_uint produced;
	atomic_uint received;
	atomic_bool streaming_reported;
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

static void report_streaming(struct test_data *data)
{
	if (atomic_load_explicit(&data->source_state, memory_order_acquire) ==
			PW_STREAM_STATE_STREAMING &&
	    atomic_load_explicit(&data->sink_state, memory_order_acquire) ==
			PW_STREAM_STATE_STREAMING &&
	    !atomic_exchange_explicit(&data->streaming_reported, true,
			memory_order_acq_rel)) {
		printf("STREAMING\n");
		fflush(stdout);
	}
}

static void stream_state_changed(struct test_data *data,
		enum pw_stream_state state, const char *error)
{
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "stream error: %s\n", error == NULL ? "unknown" : error);
		data->result = 1;
		pw_main_loop_quit(data->loop);
		return;
	}
	report_streaming(data);
}

static void source_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct test_data *data = userdata;

	(void)old;
	atomic_store_explicit(&data->source_state, state, memory_order_release);
	stream_state_changed(data, state, error);
}

static void sink_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct test_data *data = userdata;

	(void)old;
	atomic_store_explicit(&data->sink_state, state, memory_order_release);
	stream_state_changed(data, state, error);
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
	if (sequence > 2) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source produced too many buffers");
		return;
	}
	value = (float)sequence;
	memcpy(block->data, &value, sizeof(value));
	block->chunk->offset = 0;
	block->chunk->size = sizeof(value);
	block->chunk->stride = sizeof(value);
	header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source Header metadata is unavailable");
		return;
	}
	*header = (struct spa_meta_header) {
		.flags = sequence == 2 ? SPA_META_HEADER_FLAG_MARKER : 0,
		.offset = sequence * 10,
		.pts = 1000 + sequence,
		.dts_offset = 0,
		.seq = sequence,
	};
	(void)pw_stream_queue_buffer(data->source, buffer);
	printf("SOURCE %u\n", sequence);
	fflush(stdout);
}

static void sink_process(void *userdata)
{
	struct test_data *data = userdata;
	struct pw_buffer *buffer;
	struct spa_data *block;
	struct spa_meta_header *header;
	float value;

	buffer = pw_stream_dequeue_buffer(data->sink);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
	    block->chunk->offset > block->maxsize ||
	    block->chunk->size != sizeof(value) ||
	    block->maxsize - block->chunk->offset < sizeof(value)) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "sink received an invalid buffer");
		return;
	}
	memcpy(&value, SPA_PTROFF(block->data, block->chunk->offset, void),
			sizeof(value));
	header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header,
			sizeof(*header));
	if (value != 3.0f || header == NULL || header->seq != 2 ||
	    header->offset != 22 || header->pts != 2002 ||
	    header->dts_offset != 0 ||
	    header->flags != (SPA_META_HEADER_FLAG_MARKER |
		    SPA_META_HEADER_FLAG_DISCONT)) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "sink received an unexpected artifact");
		return;
	}
	if (atomic_fetch_add_explicit(&data->received, 1,
			memory_order_relaxed) != 0) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "sink received more than one artifact");
		return;
	}
	(void)pw_stream_queue_buffer(data->sink, buffer);
	printf("RESULT produced=%u received=1 value=3 seq=2\n",
			atomic_load_explicit(&data->produced, memory_order_relaxed));
	fflush(stdout);
	pw_main_loop_quit(data->loop);
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

static void trigger_source(void *userdata, int fd, uint32_t mask)
{
	struct test_data *data = userdata;
	uint8_t command;

	if ((mask & SPA_IO_IN) == 0 || read(fd, &command, sizeof(command)) !=
			(ssize_t)sizeof(command)) {
		quit_with_error(data, "could not read ndarray source trigger");
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
	quit_with_error(data, "ndarray endpoint test interrupted");
}

static void timeout(void *userdata, uint64_t expirations)
{
	struct test_data *data = userdata;

	(void)expirations;
	quit_with_error(data, "ndarray endpoint test timed out");
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
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_COLUMN_MAJOR),
			SPA_FORMAT_NDARRAY_rate,
			SPA_POD_Fraction(&SPA_FRACTION(1, 1)),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.pipewire.test.ndarray-retention/1"), 0);
	return spa_pod_builder_pop(builder, &object);
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
		"[ { loop.name=ndarray-source loop.class=data.rt loop.idle=eventfd } "
		"{ loop.name=ndarray-sink loop.class=data.rt loop.idle=eventfd } ]";
	struct test_data data = { .result = 0 };
	struct pw_properties *context_props, *source_props, *sink_props;
	struct spa_pod_builder source_builder, sink_builder;
	struct spa_pod *source_params[2], *sink_params[2];
	struct timespec timeout_value = { .tv_sec = 10 };
	uint8_t source_pods[512], sink_pods[512];
	int result;

	pw_init(&argc, &argv);
	atomic_init(&data.source_state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&data.sink_state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&data.produced, 0);
	atomic_init(&data.received, 0);
	atomic_init(&data.streaming_reported, false);
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
			"ndarray-retention-source", PW_KEY_NODE_LOOP_NAME,
			"ndarray-source", PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Test",
			NULL);
	sink_props = pw_properties_new(PW_KEY_NODE_NAME,
			"ndarray-retention-sink", PW_KEY_NODE_LOOP_NAME,
			"ndarray-sink", PW_KEY_MEDIA_TYPE, "Application",
			PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Test",
			NULL);
	data.source = pw_stream_new(data.core, "ndarray retention source",
			source_props);
	data.sink = pw_stream_new(data.core, "ndarray retention sink", sink_props);
	if (data.source == NULL || data.sink == NULL) {
		fprintf(stderr, "could not create streams: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_stream_add_listener(data.source, &data.source_listener,
			&source_events, &data);
	pw_stream_add_listener(data.sink, &data.sink_listener,
			&sink_events, &data);
	spa_pod_builder_init(&source_builder, source_pods, sizeof(source_pods));
	source_params[0] = build_format(&source_builder);
	source_params[1] = build_header_meta(&source_builder);
	spa_pod_builder_init(&sink_builder, sink_pods, sizeof(sink_pods));
	sink_params[0] = build_format(&sink_builder);
	sink_params[1] = build_header_meta(&sink_builder);
	if (source_params[0] == NULL || source_params[1] == NULL ||
	    sink_params[0] == NULL || sink_params[1] == NULL) {
		fprintf(stderr, "could not build ndarray stream parameters\n");
		data.result = 1;
		goto done;
	}
	result = pw_stream_connect(data.source, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT,
			(const struct spa_pod **)source_params,
			SPA_N_ELEMENTS(source_params));
	if (result < 0) {
		fprintf(stderr, "could not connect source: %s\n", spa_strerror(result));
		data.result = 1;
		goto done;
	}
	result = pw_stream_connect(data.sink, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT,
			(const struct spa_pod **)sink_params,
			SPA_N_ELEMENTS(sink_params));
	if (result < 0) {
		fprintf(stderr, "could not connect sink: %s\n", spa_strerror(result));
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
	    (atomic_load_explicit(&data.produced, memory_order_relaxed) != 2 ||
	     atomic_load_explicit(&data.received, memory_order_relaxed) != 1)) {
		fprintf(stderr, "incomplete transfer: produced=%u received=%u\n",
				atomic_load_explicit(&data.produced, memory_order_relaxed),
				atomic_load_explicit(&data.received, memory_order_relaxed));
		data.result = 1;
	}

done:
	if (data.source != NULL)
		pw_stream_destroy(data.source);
	if (data.sink != NULL)
		pw_stream_destroy(data.sink);
	if (data.core != NULL)
		pw_core_disconnect(data.core);
	if (data.context != NULL)
		pw_context_destroy(data.context);
	if (data.loop != NULL)
		pw_main_loop_destroy(data.loop);
	pw_deinit();
	return data.result;
}
