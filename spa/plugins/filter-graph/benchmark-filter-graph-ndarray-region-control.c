/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Service-time baseline for the configured Calculon FGN graph equivalent to
 * the Julia progressive pyramid-control island. It includes
 * spa_fgn_graph_process, conditional edge scheduling, the Rust declarations,
 * and output publication. PipeWire node scheduling and transport are excluded.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/filter-graph/filter-graph-ndarray.h>

#define BLOCK_FLOATS 256u
#define COMMAND_FLOATS 277u
#define BLOCKS_PER_FRAME 4u

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
};

struct options {
	const char *plugin;
	uint32_t samples;
	uint32_t warmup;
	uint32_t repetitions;
};

struct fixture {
	struct spa_fgn_graph *graph;
	float blocks[BLOCKS_PER_FRAME][BLOCK_FLOATS];
	float command[COMMAND_FLOATS];
	struct test_buffer input;
	struct test_buffer output;
	struct spa_buffer *inputs[1];
	struct spa_buffer *outputs[1];
	uint64_t sequence;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC_RAW, &value) == 0);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

static uint32_t parse_u32(const char *text)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	spa_assert_se(errno == 0 && end != text && *end == '\0' && value > 0 &&
			value <= UINT32_MAX);
	return (uint32_t)value;
}

static struct options parse_options(int argc, char **argv)
{
	struct options options = {
		.samples = 5000,
		.warmup = 500,
		.repetitions = 5,
	};

	for (int index = 1; index < argc; index++) {
		spa_assert_se(index + 1 < argc);
		if (spa_streq(argv[index], "--plugin"))
			options.plugin = argv[++index];
		else if (spa_streq(argv[index], "--samples"))
			options.samples = parse_u32(argv[++index]);
		else if (spa_streq(argv[index], "--warmup"))
			options.warmup = parse_u32(argv[++index]);
		else if (spa_streq(argv[index], "--repetitions"))
			options.repetitions = parse_u32(argv[++index]);
		else
			spa_assert_not_reached();
	}
	spa_assert_se(options.plugin != NULL && strchr(options.plugin, '"') == NULL);
	return options;
}

static void init_buffer(struct test_buffer *storage, void *data,
		uint32_t bytes, bool input)
{
	memset(storage, 0, sizeof(*storage));
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.flags = SPA_DATA_FLAG_READWRITE;
	storage->data.fd = -1;
	storage->data.maxsize = bytes;
	storage->data.data = data;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = input ? bytes : 0;
	storage->chunk.stride = (int32_t)bytes;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static const char *profile_config(bool eight)
{
	if (eight)
		return
			"\"region_lines\":16,\"block_lines\":4,\"region_samples\":8,"
			"\"detector_origins\":[[0,0],[15,15],[0,16],[15,31],"
			"[31,0],[16,15],[31,16],[16,31]],"
			"\"detector_line_steps\":[[1,0],[-1,0],[1,0],[-1,0],"
			"[-1,0],[1,0],[-1,0],[1,0]],"
			"\"detector_sample_steps\":[[0,1],[0,-1],[0,1],[0,-1],"
			"[0,1],[0,-1],[0,1],[0,-1]]";
	return
		"\"region_lines\":16,\"block_lines\":4,\"region_samples\":32,"
		"\"detector_origins\":[[0,0],[31,0]],"
		"\"detector_line_steps\":[[1,0],[-1,0]],"
		"\"detector_sample_steps\":[[0,1],[0,1]]";
}

static char *make_mask(void)
{
	char *mask = calloc(1, 64u * 6u + 3u);
	size_t offset = 0;

	spa_assert_se(mask != NULL);
	mask[offset++] = '[';
	for (uint32_t index = 0; index < 64; index++) {
		int written = snprintf(mask + offset, 64u * 6u + 3u - offset,
				"%strue", index == 0 ? "" : ",");
		spa_assert_se(written > 0);
		offset += (size_t)written;
	}
	mask[offset++] = ']';
	mask[offset] = '\0';
	return mask;
}

static char *make_reconstructor(void)
{
	const size_t elements = COMMAND_FLOATS * BLOCK_FLOATS;
	const size_t capacity = elements * 20u + 3u;
	char *matrix = calloc(1, capacity);
	size_t offset = 0;

	spa_assert_se(matrix != NULL);
	matrix[offset++] = '[';
	for (uint32_t row = 0; row < COMMAND_FLOATS; row++) {
		for (uint32_t column = 0; column < BLOCK_FLOATS; column++) {
			int numerator = (int)((3u * (row + 1u) +
					5u * (column + 1u)) % 29u) - 14;
			float value = (float)numerator / (29.0f * BLOCK_FLOATS);
			int written = snprintf(matrix + offset, capacity - offset,
					"%s%.9g", row == 0 && column == 0 ? "" : ",",
					value);
			spa_assert_se(written > 0 && (size_t)written < capacity - offset);
			offset += (size_t)written;
		}
	}
	matrix[offset++] = ']';
	matrix[offset] = '\0';
	return matrix;
}

static char *make_config(const char *plugin, bool eight)
{
	char *mask = make_mask();
	char *reconstructor = make_reconstructor();
	size_t capacity = strlen(reconstructor) + 32768u;
	char *config = malloc(capacity);
	int written;

	spa_assert_se(config != NULL);
	written = snprintf(config, capacity,
		"{\"nodes\":["
		"{\"type\":\"ndarray\",\"name\":\"reconstruct\","
		"\"plugin\":\"%s\",\"label\":\"pwfs-readout-reconstructor-f32\","
		"\"config\":{\"detector_rows\":32,\"detector_columns\":32,%s,"
		"\"pupil_rows\":8,\"pupil_columns\":8,"
		"\"pupil_origins\":[[0,0],[0,24],[24,0],[24,24]],"
		"\"pupil_mask\":%s,\"reconstructed_count\":277,"
		"\"initial_reconstructor\":%s,"
		"\"reconstructed_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"integrate\","
		"\"plugin\":\"%s\",\"label\":\"leaky-integrator-f32\","
		"\"config\":{\"extent\":277,\"initial_state\":0.0,"
		"\"input_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"output_schema\":\"org.calculon.ao.controller-command/1\","
		"\"rate\":[1000,1]},"
		"\"props\":{\"gain\":0.25,\"pole\":0.75}}],"
		"\"links\":[{\"output\":\"reconstruct:reconstructed\","
		"\"input\":\"integrate:input\"}],"
		"\"inputs\":[\"reconstruct:readout-block\"],"
		"\"outputs\":[\"integrate:output\"]}",
		plugin, profile_config(eight), mask, reconstructor, plugin);
	free(mask);
	free(reconstructor);
	spa_assert_se(written > 0 && (size_t)written < capacity);
	return config;
}

static void init_blocks(struct fixture *fixture, bool eight)
{
	static const int32_t eight_origins[8][2] = {
		{ 0, 0 }, { 15, 15 }, { 0, 16 }, { 15, 31 },
		{ 31, 0 }, { 16, 15 }, { 31, 16 }, { 16, 31 },
	};
	static const int32_t eight_line_steps[8] = { 1, -1, 1, -1, -1, 1, -1, 1 };
	static const int32_t eight_sample_steps[8] = { 1, -1, 1, -1, 1, -1, 1, -1 };
	const uint32_t regions = eight ? 8u : 2u;
	const uint32_t samples = eight ? 8u : 32u;

	for (uint32_t ordinal = 0; ordinal < BLOCKS_PER_FRAME; ordinal++) {
		for (uint32_t region = 0; region < regions; region++) {
			int32_t origin_row = eight ? eight_origins[region][0] :
					(region == 0 ? 0 : 31);
			int32_t origin_column = eight ? eight_origins[region][1] : 0;
			int32_t line_step = eight ? eight_line_steps[region] :
					(region == 0 ? 1 : -1);
			int32_t sample_step = eight ? eight_sample_steps[region] : 1;
			for (uint32_t line = 0; line < 4u; line++) {
				for (uint32_t sample = 0; sample < samples; sample++) {
					int32_t row = origin_row +
						(int32_t)(ordinal * 4u + line) * line_step;
					int32_t column = origin_column +
						(int32_t)sample * sample_step;
					uint32_t detector_index = (uint32_t)row * 32u +
						(uint32_t)column + 1u;
					uint32_t block_index =
						(region * 4u + line) * samples + sample;
					fixture->blocks[ordinal][block_index] = 1.0f +
						(float)((7u * detector_index) % 31u) / 31.0f;
				}
			}
		}
	}
}

static void fixture_init(struct fixture *fixture, const char *plugin, bool eight)
{
	char *config = make_config(plugin, eight);
	int res;

	memset(fixture, 0, sizeof(*fixture));
	init_blocks(fixture, eight);
	init_buffer(&fixture->input, fixture->blocks[0], BLOCK_FLOATS * sizeof(float), true);
	init_buffer(&fixture->output, fixture->command, sizeof(fixture->command), false);
	fixture->inputs[0] = &fixture->input.buffer;
	fixture->outputs[0] = &fixture->output.buffer;
	fixture->sequence = 1;
	res = spa_fgn_graph_new(config, &fixture->graph);
	if (res < 0)
		fprintf(stderr, "graph construction failed: %s (%d)\nconfig=%s\n",
				strerror(-res), res, config);
	spa_assert_se(res == 0);
	free(config);
	spa_assert_se(spa_fgn_graph_activate(fixture->graph) == 0);
}

static void fixture_clear(struct fixture *fixture)
{
	spa_assert_se(spa_fgn_graph_deactivate(fixture->graph) == 0);
	spa_fgn_graph_free(fixture->graph);
}

static void execute_block(struct fixture *fixture, uint32_t ordinal)
{
	int res;

	fixture->input.header.seq = fixture->sequence;
	fixture->input.header.offset = ordinal;
	fixture->input.data.data = fixture->blocks[ordinal];
	fixture->input.header.flags = ordinal + 1 == BLOCKS_PER_FRAME ?
		SPA_META_HEADER_FLAG_MARKER : 0;
	fixture->output.chunk.size = 0;
	res = spa_fgn_graph_process(fixture->graph,
			fixture->inputs, 1, fixture->outputs, 1);
	if (res < 0)
		fprintf(stderr, "graph processing failed: %s (%d), ordinal=%u\n",
				strerror(-res), res, ordinal);
	spa_assert_se(res == SPA_FGN_PROCESS_RESULT_NONE ||
			res == SPA_FGN_PROCESS_RESULT_PROPS_CHANGED);
	if (ordinal + 1 == BLOCKS_PER_FRAME) {
		spa_assert_se(fixture->output.chunk.size == sizeof(fixture->command));
		spa_assert_se(fixture->output.header.seq == fixture->sequence);
	} else {
		spa_assert_se(fixture->output.chunk.size == 0);
	}
}

static void execute_frame(struct fixture *fixture)
{
	for (uint32_t ordinal = 0; ordinal < BLOCKS_PER_FRAME; ordinal++)
		execute_block(fixture, ordinal);
	fixture->sequence++;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *sorted, uint32_t count,
		uint32_t numerator, uint32_t denominator)
{
	uint64_t rank = ((uint64_t)count * numerator + denominator - 1u) / denominator;
	return sorted[SPA_CLAMP(rank, 1u, count) - 1u];
}

int main(int argc, char **argv)
{
	struct options options = parse_options(argc, argv);
	uint32_t run_id = 0;

	printf("run_id,runtime,boundary,profile,detector_rows,detector_columns,"
		"reconstruction_pixels,controller_coordinates,blocks_per_frame,samples,"
		"warmup,batch_ns_per_frame,minimum_ns,p50_ns,p90_ns,p99_ns,p99_9_ns,"
		"maximum_ns,checksum\n");
	for (uint32_t profile = 0; profile < 2; profile++) {
		for (uint32_t repetition = 0; repetition < options.repetitions; repetition++) {
			struct fixture fixture;
			uint64_t *latencies = calloc(options.samples, sizeof(*latencies));
			uint64_t start, elapsed;

			spa_assert_se(latencies != NULL);
			fixture_init(&fixture, options.plugin, profile != 0);
			for (uint32_t index = 0; index < options.warmup; index++)
				execute_frame(&fixture);
			start = monotonic_ns();
			for (uint32_t index = 0; index < options.samples; index++)
				execute_frame(&fixture);
			elapsed = monotonic_ns() - start;
			fixture_clear(&fixture);

			fixture_init(&fixture, options.plugin, profile != 0);
			for (uint32_t index = 0; index < options.warmup; index++)
				execute_frame(&fixture);
			for (uint32_t index = 0; index < options.samples; index++) {
				start = monotonic_ns();
				execute_frame(&fixture);
				latencies[index] = monotonic_ns() - start;
			}
			qsort(latencies, options.samples, sizeof(*latencies), compare_u64);
			printf("%u,fgn-configured-graph,region-block-frame,%s,32,32,256,277,4,"
				"%u,%u,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
				",%" PRIu64 ",%" PRIu64 ",%.9g\n",
				++run_id, profile == 0 ? "opposed-two-region" : "eight-region",
				options.samples, options.warmup,
				(double)elapsed / options.samples, latencies[0],
				percentile(latencies, options.samples, 50, 100),
				percentile(latencies, options.samples, 90, 100),
				percentile(latencies, options.samples, 99, 100),
				percentile(latencies, options.samples, 999, 1000),
				latencies[options.samples - 1], fixture.command[0]);
			fixture_clear(&fixture);
			free(latencies);
		}
	}
	return 0;
}
