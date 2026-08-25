/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

struct frame_payload {
	uint64_t sequence;
	uint64_t trigger_ns;
	uint64_t source_ns;
};

struct latency_sample {
	uint64_t trigger_to_source_ns;
	uint64_t source_to_sink_ns;
	uint64_t trigger_to_sink_ns;
};

struct profile_threads {
	pid_t source_tid;
	pid_t sink_tid;
};

struct benchmark_profile_gate {
	int ready_fd;
	int start_fd;
	int done_fd;
	int finish_fd;
	bool enabled;
};

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
	atomic_uint_fast64_t trigger_ns;
	atomic_int source_tid;
	atomic_int sink_tid;
	atomic_int source_cpu;
	atomic_int sink_cpu;
	atomic_int source_policy;
	atomic_int sink_policy;
	atomic_int source_priority;
	atomic_int sink_priority;
	uint32_t frames;
	uint32_t warmup;
	uint32_t samples;
	uint64_t measurement_start_ns;
	uint64_t measurement_end_ns;
	struct latency_sample *latencies;
	struct benchmark_profile_gate profile_gate;
	const char *latency_path;
	bool benchmark;
	bool measurement_started;
	bool hold;
	int result;
	char source_name[128];
	char sink_name[128];
};

static uint64_t raw_time_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int profile_gate_fd(const char *name)
{
	const char *value = getenv(name);
	char *end = NULL;
	long fd;

	if (value == NULL)
		return -1;
	errno = 0;
	fd = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || fd < 0 || fd > INT_MAX)
		return -1;
	return (int)fd;
}

static bool profile_gate_init(struct benchmark_profile_gate *gate)
{
	gate->ready_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_READY_FD");
	gate->start_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_START_FD");
	gate->done_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_DONE_FD");
	gate->finish_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_FINISH_FD");
	gate->enabled = gate->ready_fd >= 0 || gate->start_fd >= 0 ||
			gate->done_fd >= 0 || gate->finish_fd >= 0;
	return !gate->enabled || (gate->ready_fd >= 0 && gate->start_fd >= 0 &&
			gate->done_fd >= 0 && gate->finish_fd >= 0);
}

static bool transfer_exact(int fd, void *data, size_t size, bool write_data)
{
	uint8_t *bytes = data;

	while (size > 0) {
		ssize_t result = write_data ? write(fd, bytes, size) :
				read(fd, bytes, size);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return false;
		bytes += result;
		size -= (size_t)result;
	}
	return true;
}

static bool profile_gate_begin(struct data *data)
{
	struct profile_threads threads = {
		.source_tid = atomic_load_explicit(&data->source_tid,
				memory_order_acquire),
		.sink_tid = atomic_load_explicit(&data->sink_tid,
				memory_order_acquire),
	};
	uint8_t token;

	if (!data->profile_gate.enabled)
		return true;
	return threads.source_tid > 0 && threads.sink_tid > 0 &&
			transfer_exact(data->profile_gate.ready_fd, &threads,
				sizeof(threads), true) &&
			transfer_exact(data->profile_gate.start_fd, &token,
				sizeof(token), false);
}

static bool profile_gate_end(struct data *data)
{
	const uint8_t token = 1;
	uint8_t reply;

	if (!data->profile_gate.enabled)
		return true;
	return transfer_exact(data->profile_gate.done_fd, (void *)&token,
				sizeof(token), true) &&
			transfer_exact(data->profile_gate.finish_fd, &reply,
				sizeof(reply), false);
}

static void observe_loop_thread(atomic_int *tid, atomic_int *cpu,
		atomic_int *policy, atomic_int *priority)
{
	struct sched_param parameters = { 0 };
	int observed_policy;

	if (atomic_load_explicit(tid, memory_order_relaxed) != 0)
		return;
	atomic_store_explicit(cpu, sched_getcpu(), memory_order_relaxed);
	observed_policy = sched_getscheduler(0);
#ifdef SCHED_RESET_ON_FORK
	if (observed_policy >= 0)
		observed_policy &= ~SCHED_RESET_ON_FORK;
#endif
	atomic_store_explicit(policy, observed_policy, memory_order_relaxed);
	if (sched_getparam(0, &parameters) < 0)
		parameters.sched_priority = -1;
	atomic_store_explicit(priority, parameters.sched_priority,
			memory_order_relaxed);
	atomic_store_explicit(tid, (int)syscall(SYS_gettid), memory_order_release);
}

static void prefault_writable_pages(void *memory, size_t size)
{
	volatile uint8_t *bytes = memory;
	long page_size = sysconf(_SC_PAGESIZE);
	size_t offset;

	if (page_size <= 0)
		return;
	for (offset = 0; offset < size; offset += (size_t)page_size)
		bytes[offset] = 0;
	if (size > 0)
		bytes[size - 1u] = 0;
}

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
	struct frame_payload payload;

	observe_loop_thread(&data->source_tid, &data->source_cpu,
			&data->source_policy, &data->source_priority);

	buffer = pw_stream_dequeue_buffer(data->source);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
			block->maxsize < sizeof(payload)) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source received an invalid buffer");
		return;
	}
	payload.sequence = atomic_fetch_add_explicit(&data->produced, 1,
			memory_order_relaxed) + 1u;
	payload.trigger_ns = data->benchmark ?
			atomic_exchange_explicit(&data->trigger_ns, 0,
				memory_order_acq_rel) : 0;
	payload.source_ns = data->benchmark ? raw_time_nsec() : 0;
	if (data->benchmark && (payload.trigger_ns == 0 ||
			payload.source_ns < payload.trigger_ns)) {
		(void)pw_stream_queue_buffer(data->source, buffer);
		quit_with_error(data, "source observed an invalid trigger timestamp");
		return;
	}
	memcpy(block->data, &payload, sizeof(payload));
	block->chunk->offset = 0;
	block->chunk->size = sizeof(payload);
	block->chunk->stride = sizeof(payload);
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
	struct frame_payload payload;
	uint64_t sink_ns = data->benchmark ? raw_time_nsec() : 0;
	uint32_t received;

	observe_loop_thread(&data->sink_tid, &data->sink_cpu,
			&data->sink_policy, &data->sink_priority);

	buffer = pw_stream_dequeue_buffer(data->sink);
	if (buffer == NULL)
		return;
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL ||
			block->chunk->size < sizeof(payload)) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "sink received an invalid buffer");
		return;
	}
	memcpy(&payload, SPA_PTROFF(block->data, block->chunk->offset, void),
			sizeof(payload));
	received = atomic_fetch_add_explicit(&data->received, 1,
			memory_order_relaxed) + 1u;
	if (payload.sequence != received) {
		(void)pw_stream_queue_buffer(data->sink, buffer);
		quit_with_error(data, "cross-process payload sequence mismatch");
		return;
	}
	if (data->benchmark) {
		if (sink_ns < payload.source_ns ||
				payload.source_ns < payload.trigger_ns) {
			(void)pw_stream_queue_buffer(data->sink, buffer);
			quit_with_error(data, "sink observed invalid frame timestamps");
			return;
		}
		if (received > data->warmup) {
			uint32_t index = received - data->warmup - 1u;

			if (index < data->samples) {
				data->latencies[index].trigger_to_source_ns =
						payload.source_ns - payload.trigger_ns;
				data->latencies[index].source_to_sink_ns =
						sink_ns - payload.source_ns;
				data->latencies[index].trigger_to_sink_ns =
						sink_ns - payload.trigger_ns;
			}
		}
	}
	if (data->hold && received == 1) {
		data->held = buffer;
		printf("HOLDING sequence=%" PRIu64 "\n", payload.sequence);
		fflush(stdout);
		pw_main_loop_quit(data->loop);
		return;
	}
	(void)pw_stream_queue_buffer(data->sink, buffer);
	if (received == data->frames) {
		if (data->benchmark)
			data->measurement_end_ns = sink_ns;
		pw_main_loop_quit(data->loop);
	}
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
	if (data->benchmark && !data->measurement_started &&
			atomic_load_explicit(&data->received,
				memory_order_acquire) >= data->warmup) {
		if (!profile_gate_begin(data)) {
			quit_with_error(data, "could not enter profile gate");
			return;
		}
		data->measurement_start_ns = raw_time_nsec();
		data->measurement_started = true;
	}
	if (source_state != PW_STREAM_STATE_STREAMING ||
			sink_state != PW_STREAM_STATE_STREAMING ||
			atomic_load_explicit(&data->stopping, memory_order_acquire) ||
			atomic_load_explicit(&data->produced, memory_order_relaxed) >=
				data->frames ||
			(data->benchmark &&
			 atomic_load_explicit(&data->produced, memory_order_acquire) !=
			 atomic_load_explicit(&data->received, memory_order_acquire)) ||
			!atomic_exchange_explicit(&data->trigger_ready, false,
				memory_order_acq_rel))
		return;
	if (data->benchmark) {
		uint64_t now = raw_time_nsec();

		if (now == 0 || atomic_exchange_explicit(&data->trigger_ns, now,
				memory_order_acq_rel) != 0) {
			quit_with_error(data, "overlapping benchmark trigger");
			return;
		}
	}
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

static bool parse_u32(const char *value, uint32_t *result)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
		return false;
	*result = (uint32_t)parsed;
	return true;
}

static bool parse_u64(const char *value, uint64_t *result)
{
	char *end = NULL;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0')
		return false;
	*result = (uint64_t)parsed;
	return true;
}

static bool write_latencies(const struct data *data)
{
	FILE *output;
	uint32_t i;

	output = fopen(data->latency_path, "w");
	if (output == NULL)
		return false;
	fprintf(output, "iteration,trigger_to_source_ns,source_to_sink_ns,"
			"trigger_to_sink_ns\n");
	for (i = 0; i < data->samples; i++)
		fprintf(output, "%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", i,
				data->latencies[i].trigger_to_source_ns,
				data->latencies[i].source_to_sink_ns,
				data->latencies[i].trigger_to_sink_ns);
	return fclose(output) == 0;
}

int main(int argc, char *argv[])
{
	static const char test_data_loops[] =
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
	char benchmark_data_loops[512];
	const char *data_loops = test_data_loops;
	const char *source_loop, *sink_loop;
	uint32_t source_cpu = 0, sink_cpu = 0, rt_priority = 0;
	uint64_t trigger_interval_ns = SPA_NSEC_PER_MSEC;
	int result;

	if (argc < 5)
		return 2;
	pw_init(&argc, &argv);
	if (loop_name(argv[2]) == NULL || loop_name(argv[3]) == NULL) {
		data.result = 2;
		goto done;
	}
	data.benchmark = spa_streq(argv[4], "benchmark");
	if (data.benchmark) {
		uint64_t total_frames;

		if (argc != 12 || !parse_u32(argv[5], &data.warmup) ||
				!parse_u32(argv[6], &data.samples) || data.warmup == 0 ||
				data.samples == 0 || !parse_u64(argv[7], &trigger_interval_ns) ||
				trigger_interval_ns == 0 ||
				!parse_u32(argv[8], &source_cpu) ||
				!parse_u32(argv[9], &sink_cpu) || source_cpu == sink_cpu ||
				!parse_u32(argv[10], &rt_priority) || rt_priority > 99 ||
				!profile_gate_init(&data.profile_gate)) {
			data.result = 2;
			goto done;
		}
		total_frames = (uint64_t)data.warmup + data.samples;
		if (total_frames > UINT32_MAX) {
			data.result = 2;
			goto done;
		}
		data.frames = (uint32_t)total_frames;
		data.latency_path = argv[11];
		data.latencies = calloc(data.samples, sizeof(*data.latencies));
		if (data.latencies == NULL) {
			data.result = 1;
			goto done;
		}
		prefault_writable_pages(data.latencies,
				data.samples * sizeof(*data.latencies));
		if (snprintf(benchmark_data_loops, sizeof(benchmark_data_loops),
				"[ { loop.name=client-source loop.class=data.rt "
				"loop.idle=%s loop.rt-prio=%u thread.affinity=[ %u ] } "
				"{ loop.name=client-sink loop.class=data.rt "
				"loop.idle=%s loop.rt-prio=%u thread.affinity=[ %u ] } ]",
				argv[2], rt_priority, source_cpu, argv[3], rt_priority,
				sink_cpu) >= (int)sizeof(benchmark_data_loops)) {
			data.result = 2;
			goto done;
		}
		data_loops = benchmark_data_loops;
		source_loop = "client-source";
		sink_loop = "client-sink";
		trigger_interval.tv_sec = (time_t)(trigger_interval_ns /
				SPA_NSEC_PER_SEC);
		trigger_interval.tv_nsec = (long)(trigger_interval_ns %
				SPA_NSEC_PER_SEC);
		timeout.tv_sec = 60;
	} else {
		if (argc > 6 || !parse_u32(argv[4], &data.frames) ||
				data.frames == 0) {
			data.result = 2;
			goto done;
		}
		source_loop = loop_name(argv[2]);
		sink_loop = loop_name(argv[3]);
		data.hold = argc == 6 && spa_streq(argv[5], "hold");
		if (argc == 6 && !data.hold) {
			data.result = 2;
			goto done;
		}
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
	atomic_init(&data.trigger_ns, 0);
	atomic_init(&data.source_tid, 0);
	atomic_init(&data.sink_tid, 0);
	atomic_init(&data.source_cpu, -1);
	atomic_init(&data.sink_cpu, -1);
	atomic_init(&data.source_policy, -1);
	atomic_init(&data.sink_policy, -1);
	atomic_init(&data.source_priority, -1);
	atomic_init(&data.sink_priority, -1);

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
	if (data.result == 0 && data.benchmark && !profile_gate_end(&data)) {
		fprintf(stderr, "could not finish profile gate\n");
		data.result = 1;
	}
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
	if (data.result == 0 && data.benchmark) {
		if (!data.measurement_started ||
				data.measurement_end_ns <= data.measurement_start_ns ||
				!write_latencies(&data)) {
			fprintf(stderr, "could not write complete benchmark results\n");
			data.result = 1;
		} else {
			printf("BENCHMARK samples=%u measurement-ns=%" PRIu64
					" source-tid=%d source-cpu=%d source-policy=%d "
					"source-priority=%d sink-tid=%d sink-cpu=%d "
					"sink-policy=%d sink-priority=%d\n",
					data.samples,
					data.measurement_end_ns - data.measurement_start_ns,
					atomic_load_explicit(&data.source_tid,
						memory_order_relaxed),
					atomic_load_explicit(&data.source_cpu,
						memory_order_relaxed),
					atomic_load_explicit(&data.source_policy,
						memory_order_relaxed),
					atomic_load_explicit(&data.source_priority,
						memory_order_relaxed),
					atomic_load_explicit(&data.sink_tid,
						memory_order_relaxed),
					atomic_load_explicit(&data.sink_cpu,
						memory_order_relaxed),
					atomic_load_explicit(&data.sink_policy,
						memory_order_relaxed),
					atomic_load_explicit(&data.sink_priority,
						memory_order_relaxed));
		}
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
	free(data.latencies);
	pw_deinit();
	return data.result;
}
