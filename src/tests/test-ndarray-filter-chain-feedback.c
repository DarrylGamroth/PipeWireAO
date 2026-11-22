/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compile the module into this test so these checks cover its private
 * feedback commit implementation without adding a production test hook. */
#include "../modules/module-ndarray-filter-chain.c"

struct fixture {
	struct impl impl;
	struct feedback_bridge bridges[2];
	float input_values[2][4];
	float output_values[2][4];
};

static void init_fixture(struct fixture *fixture)
{
	uint32_t i, j;

	memset(fixture, 0, sizeof(*fixture));
	fixture->impl.feedback_bridges = fixture->bridges;
	fixture->impl.n_feedback_bridges = SPA_N_ELEMENTS(fixture->bridges);
	for (i = 0; i < SPA_N_ELEMENTS(fixture->bridges); i++) {
		struct feedback_bridge *bridge = &fixture->bridges[i];

		bridge->input.memory = fixture->input_values[i];
		bridge->input.size = sizeof(fixture->input_values[i]);
		bridge->input.header = (struct spa_meta_header) {
			.pts = SPA_TIME_INVALID,
		};
		bridge->output.memory = fixture->output_values[i];
		bridge->output.size = sizeof(fixture->output_values[i]);
		for (j = 0; j < SPA_N_ELEMENTS(fixture->input_values[i]); j++) {
			fixture->input_values[i][j] = (float)(10 * (i + 1) + j);
			fixture->output_values[i][j] = (float)(100 * (i + 1) + j);
		}
	}
}

static void assert_inputs_unchanged(const struct fixture *fixture,
		const float expected_values[2][4],
		const struct spa_meta_header expected_headers[2])
{
	uint32_t i;

	for (i = 0; i < SPA_N_ELEMENTS(fixture->bridges); i++) {
		assert(memcmp(fixture->input_values[i], expected_values[i],
				sizeof(fixture->input_values[i])) == 0);
		assert(memcmp(&fixture->bridges[i].input.header, &expected_headers[i],
				sizeof(expected_headers[i])) == 0);
	}
}

static void test_first_all_absent_preserves_initial_zeros(void)
{
	struct fixture fixture;
	const float zeros[2][4] = { { 0 } };
	const struct spa_meta_header headers[2] = {
		{ .pts = SPA_TIME_INVALID },
		{ .pts = SPA_TIME_INVALID },
	};
	uint32_t i;

	init_fixture(&fixture);
	for (i = 0; i < SPA_N_ELEMENTS(fixture.bridges); i++) {
		memset(fixture.input_values[i], 0, sizeof(fixture.input_values[i]));
		fixture.bridges[i].output.chunk.size = 0;
	}
	assert(commit_feedback_bridges(&fixture.impl) == 0);
	assert_inputs_unchanged(&fixture, zeros, headers);
}

static void test_later_all_absent_preserves_feedback_and_metadata(void)
{
	struct fixture fixture;
	float expected_values[2][4];
	struct spa_meta_header expected_headers[2];
	uint32_t i;

	init_fixture(&fixture);
	for (i = 0; i < SPA_N_ELEMENTS(fixture.bridges); i++) {
		fixture.bridges[i].input.header = (struct spa_meta_header) {
			.flags = SPA_META_HEADER_FLAG_MARKER,
			.offset = i + 1,
			.pts = 100 + i,
			.dts_offset = -10 - (int64_t)i,
			.seq = 1000 + i,
		};
		fixture.bridges[i].output.chunk.size = 0;
	}
	memcpy(expected_values, fixture.input_values, sizeof(expected_values));
	for (i = 0; i < SPA_N_ELEMENTS(fixture.bridges); i++)
		expected_headers[i] = fixture.bridges[i].input.header;
	assert(commit_feedback_bridges(&fixture.impl) == 0);
	assert_inputs_unchanged(&fixture, expected_values, expected_headers);
}

static void test_rejected_outputs_do_not_partially_update(void)
{
	struct fixture fixture;
	float expected_values[2][4];
	struct spa_meta_header expected_headers[2];
	uint32_t i;

	init_fixture(&fixture);
	for (i = 0; i < SPA_N_ELEMENTS(fixture.bridges); i++)
		fixture.bridges[i].input.header.seq = 10 + i;
	memcpy(expected_values, fixture.input_values, sizeof(expected_values));
	for (i = 0; i < SPA_N_ELEMENTS(fixture.bridges); i++)
		expected_headers[i] = fixture.bridges[i].input.header;

	fixture.bridges[0].output.chunk.size = fixture.bridges[0].output.size;
	fixture.bridges[1].output.chunk.size = 0;
	assert(commit_feedback_bridges(&fixture.impl) == -ENODATA);
	assert_inputs_unchanged(&fixture, expected_values, expected_headers);

	fixture.bridges[0].output.chunk.size = fixture.bridges[0].output.size;
	fixture.bridges[1].output.chunk.size = fixture.bridges[1].output.size - sizeof(float);
	assert(commit_feedback_bridges(&fixture.impl) == -ENODATA);
	assert_inputs_unchanged(&fixture, expected_values, expected_headers);
}

static void test_retained_input_removal(void)
{
	struct impl impl = { .n_inputs = 1 };
	struct pw_buffer retained = { 0 }, other = { 0 };
	struct pw_buffer *inputs[1] = { &retained };
	struct port port = {
		.impl = &impl,
		.index = 0,
		.direction = SPA_DIRECTION_INPUT,
	};

	impl.input_buffers = inputs;
	assert(!invalidate_retained_buffer(&impl, &port, &other));
	assert(inputs[0] == &retained);
	assert(invalidate_retained_buffer(&impl, &port, &retained));
	assert(inputs[0] == NULL);
	assert(!invalidate_retained_buffer(&impl, &port, &retained));
}

struct feedback_log_capture {
	struct spa_log log;
	uint32_t errors;
	char message[128];
};

static SPA_PRINTF_FUNC(7, 0) void
capture_feedback_error(void *object, enum spa_log_level level,
		const struct spa_log_topic *topic, const char *file, int line,
		const char *func, const char *fmt, va_list args)
{
	struct feedback_log_capture *capture = object;

	if (level == SPA_LOG_LEVEL_ERROR && topic == mod_topic) {
		capture->errors++;
		vsnprintf(capture->message, sizeof(capture->message), fmt, args);
	}
}

static const struct spa_log_methods feedback_log_methods = {
	SPA_VERSION_LOG_METHODS,
	.logtv = capture_feedback_error,
};

static void test_feedback_error_publication(const char *plugin)
{
	struct feedback_log_capture capture = {
		.log.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log,
				SPA_VERSION_LOG, &feedback_log_methods, &capture),
	};
	struct spa_log *previous_log = pw_log_get();
	enum spa_log_level previous_level = mod_topic->level;
	bool previous_custom_level = mod_topic->has_custom_level;
	struct port *inputs[2] = { NULL }, *outputs[2] = { NULL };
	struct spa_buffer *process_inputs[2], *process_outputs[2];
	struct pw_buffer *input_buffers[2] = { NULL }, *output_buffers[2] = { NULL };
	struct impl impl = {
		.n_inputs = 2, .n_outputs = 2, .inputs = inputs, .outputs = outputs,
		.process_inputs = process_inputs, .process_outputs = process_outputs,
		.input_buffers = input_buffers, .output_buffers = output_buffers,
	};
	const struct pw_filter_events events = { PW_VERSION_FILTER_EVENTS };
	struct pw_main_loop *main = pw_main_loop_new(NULL);
	struct pw_properties *feedback;
	const char *state_error;
	char config[4096], expected[128];
	int res;

	res = snprintf(config, sizeof(config),
			"{ nodes = ["
			" { type = ndarray name = defer plugin = \"%s\" label = defer-f32 }"
			" { type = ndarray name = skipped plugin = \"%s\" label = scale-f32"
			"   config = { shape = [ 4 ] schema = test.deferred/1 } }"
			" { type = ndarray name = ready plugin = \"%s\" label = scale-f32"
			"   config = { shape = [ 4 ] schema = test.deferred/1 } }"
			"] links = [ { output = \"defer:out\" input = \"skipped:in\" } ]"
			" inputs = [ \"defer:in\" \"ready:in\" ]"
			" outputs = [ \"skipped:out\" \"ready:out\" ] }",
			plugin, plugin, plugin);
	assert(res > 0 && (size_t)res < sizeof(config));
	assert(spa_fgn_graph_new(config, &impl.graph) == 0);
	feedback = pw_properties_new(FEEDBACK_PREFIX "defer:in", "skipped:out",
			FEEDBACK_PREFIX "ready:in", "ready:out", NULL);
	assert(feedback != NULL);
	assert(configure_feedback_bridges(&impl, feedback) == 0);
	pw_properties_free(feedback);
	assert(spa_fgn_graph_activate(impl.graph) == 0);

	assert(main != NULL);
	impl.main_loop = pw_main_loop_get_loop(main);
	pw_loop_enter(impl.main_loop);
	impl.filter = pw_filter_new_simple(impl.main_loop, "feedback-error",
			pw_properties_new(PW_KEY_CONFIG_NAME, "null",
					"support.dbus", "false",
					"context.modules.allow-empty", "true", NULL), &events, NULL);
	assert(impl.filter != NULL);
	impl.main_event = pw_loop_add_event(impl.main_loop, main_event, &impl);
	assert(impl.main_event != NULL);
	pw_log_set(&capture.log);
	mod_topic->level = SPA_LOG_LEVEL_ERROR;
	mod_topic->has_custom_level = true;

	/* One bridge has an output while the other inherits the deferred output. */
	process(&impl, NULL);
	assert(atomic_load(&impl.process_error) == -ENODATA);
	assert(capture.errors == 0);
	assert(pw_filter_get_state(impl.filter, NULL) == PW_FILTER_STATE_UNCONNECTED);
	assert(pw_loop_iterate(impl.main_loop, 0) >= 0);
	assert(atomic_load(&impl.process_error) == 0);
	assert(capture.errors == 1);
	snprintf(expected, sizeof(expected), "ndarray feedback output incomplete: %s",
			spa_strerror(-ENODATA));
	assert(strcmp(capture.message, expected) == 0);
	assert(pw_filter_get_state(impl.filter, &state_error) == PW_FILTER_STATE_ERROR);
	assert(errno == ENODATA);
	snprintf(expected, sizeof(expected), "ndarray graph process failed: %s",
			spa_strerror(-ENODATA));
	assert(strcmp(state_error, expected) == 0);

	mod_topic->level = previous_level;
	mod_topic->has_custom_level = previous_custom_level;
	pw_log_set(previous_log);
	pw_loop_destroy_source(impl.main_loop, impl.main_event);
	pw_filter_destroy(impl.filter);
	pw_loop_leave(impl.main_loop);
	pw_main_loop_destroy(main);
	spa_fgn_graph_free(impl.graph);
	clear_feedback_bridges(&impl);
}

#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
static void test_process_trace(void)
{
	struct fgn_process_trace trace = { 0 };
	struct fgn_process_trace_record *record;
	struct spa_meta_header header = { .seq = 42, .offset = 11 };
	struct spa_meta meta = { .type = SPA_META_Header,
		.size = sizeof(header), .data = &header };
	struct spa_buffer buffer = { .n_metas = 1, .metas = &meta };
	char directory[] = "/tmp/pw-fgn-process-trace-XXXXXX";
	char line[256];
	char *path;
	FILE *input;
	unsigned int callback, offset;
	unsigned long long sequence, start, end;
	int result;

	assert(unsetenv("PW_FGN_PROCESS_TRACE_DIR") == 0);
	assert(fgn_process_trace_init(&trace) == 0);
	assert(trace.records == NULL && trace.path == NULL);
	assert(fgn_process_trace_begin(&trace, &buffer) == NULL);
	fgn_process_trace_end(&trace, NULL, -EIO);
	assert(trace.used == 0 && trace.omitted == 0);
	fgn_process_trace_clear(&trace);
	assert(mkdtemp(directory) != NULL);
	assert(setenv("PW_FGN_PROCESS_TRACE_DIR", directory, 1) == 0);
	assert(fgn_process_trace_init(&trace) == 0);
	assert(trace.initialized && trace.records != NULL);
	path = strdup(trace.path);
	assert(path != NULL && access(path, F_OK) < 0);
	record = fgn_process_trace_begin(&trace, &buffer);
	assert(record != NULL && trace.used == 0);
	header.seq = 43;
	header.offset = 22;
	fgn_process_trace_end(&trace, record, -EIO);
	assert(trace.used == 1 && trace.omitted == 0);
	assert(record->sequence == 42 && record->offset == 11);
	assert(record->start_ns > 0 && record->end_ns >= record->start_ns);
	assert(record->result == -EIO);
	record = fgn_process_trace_begin(&trace, NULL);
	fgn_process_trace_end(&trace, record, 0);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	meta.size = sizeof(header) - 1;
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, 0);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	meta.size = sizeof(header);
	buffer.metas = NULL;
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, -EINVAL);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	buffer.metas = &meta;
	buffer.n_metas = SPA_FGN_MAX_METAS + 1;
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, -EINVAL);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	buffer.n_metas = 1;
	buffer.metas = (struct spa_meta *)((char *)&meta + 1);
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, -EINVAL);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	buffer.metas = &meta;
	meta.data = NULL;
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, -EINVAL);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	meta.data = (char *)&header + 1;
	record = fgn_process_trace_begin(&trace, &buffer);
	fgn_process_trace_end(&trace, record, -EINVAL);
	assert(record->sequence == UINT64_MAX && record->offset == UINT32_MAX);
	meta.data = &header;
	while (trace.used < FGN_PROCESS_TRACE_CAPACITY) {
		record = fgn_process_trace_begin(&trace, &buffer);
		assert(record != NULL);
		fgn_process_trace_end(&trace, record, 0);
	}
	assert(fgn_process_trace_begin(&trace, &buffer) == NULL);
	fgn_process_trace_end(&trace, NULL, -EIO);
	assert(trace.used == FGN_PROCESS_TRACE_CAPACITY && trace.omitted == 1);
	fgn_process_trace_clear(&trace);
	assert(trace.records == NULL && trace.path == NULL);
	input = fopen(path, "r");
	assert(input != NULL);
	assert(fgets(line, sizeof(line), input) != NULL);
	assert(strcmp(line, "# capacity=32768 used=32768 omitted=1\n") == 0);
	assert(fgets(line, sizeof(line), input) != NULL);
	assert(strcmp(line, "callback,sequence,offset,start_ns,end_ns,result\n") == 0);
	assert(fgets(line, sizeof(line), input) != NULL);
	assert(sscanf(line, "%u,%llu,%u,%llu,%llu,%d", &callback, &sequence,
			&offset, &start, &end, &result) == 6);
	assert(callback == 1 && sequence == 42 && offset == 11 && result == -EIO);
	assert(start > 0 && end >= start);
	fclose(input);
	assert(unlink(path) == 0 && rmdir(directory) == 0);
	free(path);
	assert(unsetenv("PW_FGN_PROCESS_TRACE_DIR") == 0);
}
#endif

int main(int argc, char *argv[])
{
	assert(argc == 2);
	pw_init(&argc, &argv);
	test_first_all_absent_preserves_initial_zeros();
	test_later_all_absent_preserves_feedback_and_metadata();
	test_rejected_outputs_do_not_partially_update();
	test_retained_input_removal();
	test_feedback_error_publication(argv[1]);
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	test_process_trace();
#endif
	pw_deinit();
	return 0;
}
