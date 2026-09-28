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

int main(void)
{
	test_first_all_absent_preserves_initial_zeros();
	test_later_all_absent_preserves_feedback_and_metadata();
	test_rejected_outputs_do_not_partially_update();
	return 0;
}
