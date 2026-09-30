/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
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

int main(void)
{
	test_first_all_absent_preserves_initial_zeros();
	test_later_all_absent_preserves_feedback_and_metadata();
	test_rejected_outputs_do_not_partially_update();
	test_retained_input_removal();
#ifdef PW_ENABLE_DIAGNOSTIC_TRACE
	test_process_trace();
#endif
	return 0;
}
