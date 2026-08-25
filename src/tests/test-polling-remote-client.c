/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *source;
	struct pw_stream *sink;
	struct spa_hook source_listener;
	struct spa_hook sink_listener;
	struct spa_source *trigger_timer;
	struct spa_source *timeout_timer;
	struct pw_buffer *held;
	atomic_int source_state;
	atomic_int sink_state;
	atomic_bool trigger_ready;
	atomic_bool ready_reported;
	atomic_bool stopping;
	atomic_uint produced;
	atomic_uint received;
	uint32_t frames;
	bool hold;
	int result;
	char source_name[128];
	char sink_name[128];
};

static void quit_with_error(struct data *data, const char *message)
{
	if (data->result == 0) {
		fprintf(stderr, "%s\n", message);
		data->result = 1;
	}
	pw_main_loop_quit(data->loop);
}

static void stream_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *data = userdata;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "stream error: %s\n", error == NULL ? "unknown" : error);
		data->result = 1;
		pw_main_loop_quit(data->loop);
	}
}

static void source_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *data = userdata;

	atomic_store_explicit(&data->source_state, state, memory_order_release);
	stream_state_changed(userdata, old, state, error);
}

static void sink_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *data = userdata;

	atomic_store_explicit(&data->sink_state, state, memory_order_release);
	stream_state_changed(userdata, old, state, error);
}

static void source_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *buffer;
	struct spa_data *block;
	uint64_t sequence;

	buffer = pw_stream_dequeue_buffer(data->source);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
			block->maxsize < sizeof(sequence)) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source received an invalid buffer");
		return;
	}
	sequence = atomic_fetch_add_explicit(&data->produced, 1,
			memory_order_relaxed) + 1u;
	memcpy(block->data, &sequence, sizeof(sequence));
	block->chunk->offset = 0;
	block->chunk->size = sizeof(sequence);
	block->chunk->stride = sizeof(sequence);
	(void)pw_stream_queue_buffer(data->source, buffer);
}

static void source_trigger_done(void *userdata)
{
	struct data *data = userdata;

	atomic_store_explicit(&data->trigger_ready, true, memory_order_release);
}

static void sink_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *buffer;
	struct spa_data *block;
	uint64_t sequence;
	uint32_t received;

	buffer = pw_stream_dequeue_buffer(data->sink);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
			block->chunk->size < sizeof(sequence)) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "sink received an invalid buffer");
		return;
	}
	memcpy(&sequence, SPA_PTROFF(block->data, block->chunk->offset, void),
			sizeof(sequence));
	received = atomic_fetch_add_explicit(&data->received, 1,
			memory_order_relaxed) + 1u;
	if (sequence != received) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "cross-process payload sequence mismatch");
		return;
	}
	if (data->hold && received == 1) {
		data->held = buffer;
		printf("HOLDING sequence=%" PRIu64 "\n", sequence);
		fflush(stdout);
		pw_main_loop_quit(data->loop);
		return;
	}
	(void)pw_stream_queue_buffer(data->sink, buffer);
	if (received == data->frames)
		pw_main_loop_quit(data->loop);
}

static const struct pw_stream_events source_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = source_state_changed,
	.process = source_process,
	.trigger_done = source_trigger_done,
};

static const struct pw_stream_events sink_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = sink_state_changed,
	.process = sink_process,
};

static void trigger_timer(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	int source_state = atomic_load_explicit(&data->source_state,
			memory_order_acquire);
	int sink_state = atomic_load_explicit(&data->sink_state,
			memory_order_acquire);

	(void)expirations;
	if (!atomic_load_explicit(&data->ready_reported, memory_order_acquire) &&
			source_state >= PW_STREAM_STATE_PAUSED &&
			sink_state >= PW_STREAM_STATE_PAUSED) {
		atomic_store_explicit(&data->ready_reported, true,
				memory_order_release);
		printf("READY source=%s sink=%s\n", data->source_name,
				data->sink_name);
		fflush(stdout);
	}
	if (source_state != PW_STREAM_STATE_STREAMING ||
			sink_state != PW_STREAM_STATE_STREAMING ||
			atomic_load_explicit(&data->stopping, memory_order_acquire) ||
			atomic_load_explicit(&data->produced, memory_order_relaxed) >=
				data->frames ||
			!atomic_exchange_explicit(&data->trigger_ready, false,
				memory_order_acq_rel))
		return;
	if (pw_stream_trigger_process(data->source) < 0)
		quit_with_error(data, "could not trigger exported source stream");
}

static void timeout_timer(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;

	(void)expirations;
	quit_with_error(data, "cross-process polling test timed out");
}

static void signal_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;

	(void)signal_number;
	atomic_store_explicit(&data->stopping, true, memory_order_release);
	pw_main_loop_quit(data->loop);
}

static const char *loop_name(const char *idle)
{
	if (spa_streq(idle, "busy-spin"))
		return "client-poll";
	if (spa_streq(idle, "eventfd"))
		return "client-event";
	return NULL;
}

int main(int argc, char *argv[])
{
	static const char data_loops[] =
		"[ { loop.name=client-event loop.class=data.rt loop.idle=eventfd } "
		"{ loop.name=client-poll loop.class=data.rt loop.idle=busy-spin } ]";
	struct data data = { .result = 0 };
	struct pw_properties *context_props, *source_props, *sink_props;
	struct spa_pod_builder builder;
	struct spa_pod *format;
	const struct spa_pod *params[1];
	struct timespec trigger_interval = { .tv_nsec = SPA_NSEC_PER_MSEC };
	struct timespec timeout = { .tv_sec = 10 };
	uint8_t pod_buffer[512];
	const char *source_loop, *sink_loop;
	int result;

	if (argc < 5 || argc > 6)
		return 2;
	pw_init(&argc, &argv);
	source_loop = loop_name(argv[2]);
	sink_loop = loop_name(argv[3]);
	data.frames = (uint32_t)strtoul(argv[4], NULL, 10);
	data.hold = argc == 6 && spa_streq(argv[5], "hold");
	if (source_loop == NULL || sink_loop == NULL || data.frames == 0) {
		data.result = 2;
		goto done;
	}
	if (snprintf(data.source_name, sizeof(data.source_name),
			"polling-export-source-%s", argv[1]) >=
			(int)sizeof(data.source_name) ||
			snprintf(data.sink_name, sizeof(data.sink_name),
			"polling-export-sink-%s", argv[1]) >=
			(int)sizeof(data.sink_name)) {
		data.result = 2;
		goto done;
	}
	atomic_init(&data.source_state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&data.sink_state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&data.trigger_ready, true);
	atomic_init(&data.ready_reported, false);
	atomic_init(&data.stopping, false);
	atomic_init(&data.produced, 0);
	atomic_init(&data.received, 0);

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
	source_props = pw_properties_new(PW_KEY_NODE_NAME, data.source_name,
			PW_KEY_NODE_LOOP_NAME, source_loop,
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Test", NULL);
	sink_props = pw_properties_new(PW_KEY_NODE_NAME, data.sink_name,
			PW_KEY_NODE_LOOP_NAME, sink_loop,
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Test", NULL);
	data.source = pw_stream_new(data.core, "polling export source",
			source_props);
	data.sink = pw_stream_new(data.core, "polling export sink", sink_props);
	if (data.source == NULL || data.sink == NULL) {
		fprintf(stderr, "could not create streams: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_stream_add_listener(data.source, &data.source_listener,
			&source_events, &data);
	pw_stream_add_listener(data.sink, &data.sink_listener, &sink_events, &data);
	spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
	format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16,
				.rate = 1000, .channels = 1));
	params[0] = format;
	result = pw_stream_connect(data.source, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT,
			params, 1);
	if (result < 0) {
		fprintf(stderr, "could not connect source: %s\n", spa_strerror(result));
		data.result = 1;
		goto done;
	}
	result = pw_stream_connect(data.sink, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT,
			params, 1);
	if (result < 0) {
		fprintf(stderr, "could not connect sink: %s\n", spa_strerror(result));
		data.result = 1;
		goto done;
	}
	data.trigger_timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
			trigger_timer, &data);
	data.timeout_timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
			timeout_timer, &data);
	if (data.trigger_timer == NULL || data.timeout_timer == NULL) {
		fprintf(stderr, "could not create timers: %s\n", strerror(errno));
		data.result = 1;
		goto done;
	}
	pw_loop_update_timer(pw_main_loop_get_loop(data.loop), data.trigger_timer,
			&trigger_interval, &trigger_interval, false);
	pw_loop_update_timer(pw_main_loop_get_loop(data.loop), data.timeout_timer,
			&timeout, NULL, false);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, signal_quit,
			&data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, signal_quit,
			&data);
	pw_main_loop_run(data.loop);
	if (data.result == 0 && !data.hold &&
			atomic_load_explicit(&data.received, memory_order_relaxed) !=
				data.frames) {
		fprintf(stderr, "incomplete transfer: produced=%u received=%u\n",
				atomic_load_explicit(&data.produced, memory_order_relaxed),
				atomic_load_explicit(&data.received, memory_order_relaxed));
		data.result = 1;
	}
	if (data.result == 0)
		printf("RESULT produced=%u received=%u held=%u\n",
				atomic_load_explicit(&data.produced, memory_order_relaxed),
				atomic_load_explicit(&data.received, memory_order_relaxed),
				data.held != NULL);

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
