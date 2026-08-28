/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Callback service-time comparison for a complete-frame and a row-block
 * Calculon SHWFS front end. This excludes detector readout, PipeWire
 * scheduling, input publication, and graph construction. In addition to
 * aggregate service, the CSV records every row-block callback so an external
 * harness can replay the trace against an independent camera-arrival schedule.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/filter-graph/filter-graph-ndarray.h>

#define IMAGE_ROWS 352u
#define IMAGE_COLUMNS 352u
#define SUBAPERTURE_ROWS 22u
#define SUBAPERTURE_COLUMNS 22u
#define SUBAPERTURES 256u
#define SLOPES (SUBAPERTURES * 2u)
#define ACTUATORS 277u
#define DEFAULT_SAMPLES 5000u
#define DEFAULT_WARMUP 1000u
#define DEFAULT_BLOCK_ROWS 8u

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
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

static void init_buffer(struct test_buffer *storage, void *data,
		uint32_t bytes, int32_t stride, bool input)
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
	storage->chunk.stride = stride;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static char *make_origins(void)
{
	size_t capacity = SUBAPERTURES * 24u + 3u;
	char *origins = malloc(capacity);
	size_t offset = 0;

	spa_assert_se(origins != NULL);
	origins[offset++] = '[';
	for (uint32_t row = 0; row < IMAGE_ROWS / SUBAPERTURE_ROWS; row++) {
		for (uint32_t column = 0;
				column < IMAGE_COLUMNS / SUBAPERTURE_COLUMNS; column++) {
			int written = snprintf(origins + offset, capacity - offset,
					"%s[%u,%u]", offset == 1 ? "" : ",",
					row * SUBAPERTURE_ROWS,
					column * SUBAPERTURE_COLUMNS);

			spa_assert_se(written > 0 &&
					(size_t)written < capacity - offset);
			offset += (size_t)written;
		}
	}
	origins[offset++] = ']';
	origins[offset] = '\0';
	return origins;
}

static char *make_config(const char *plugin, const char *origins,
		uint32_t block_rows, bool row)
{
	size_t capacity = strlen(plugin) * 3u + strlen(origins) + 8192u;
	char *config = malloc(capacity);
	int written;

	spa_assert_se(config != NULL && strchr(plugin, '"') == NULL);
	if (row) {
		written = snprintf(config, capacity,
			"{\"nodes\":["
			"{\"type\":\"ndarray\",\"name\":\"calibrate\","
			"\"plugin\":\"%s\",\"label\":\"pixel-calibration-row-u16-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"row_block_rows\":%u,\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"reconstruct\","
			"\"plugin\":\"%s\",\"label\":\"shwfs-row-reconstructor-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"row_block_rows\":%u,\"subaperture_rows\":%u,"
			"\"subaperture_columns\":%u,\"subaperture_count\":%u,"
			"\"actuator_count\":%u,\"initial_subaperture_origins\":%s,"
			"\"coordinate_scale\":1.0,\"pixel_threshold\":0.0,"
			"\"flux_threshold\":1.0,"
			"\"reconstructed_schema\":"
			"\"org.calculon.ao.controller-residual-error/1\","
			"\"rate\":[1000,1]}}],"
			"\"links\":[{\"output\":\"calibrate:calibrated\","
			"\"input\":\"reconstruct:row-block\"}],"
			"\"inputs\":[\"calibrate:raw\",\"calibrate:flat\","
			"\"calibrate:background\",\"reconstruct:subaperture-origins\","
			"\"reconstruct:coordinates\",\"reconstruct:reference-slopes\","
			"\"reconstruct:thresholds\",\"reconstruct:active\","
			"\"reconstruct:reconstructor\"],"
			"\"outputs\":[\"reconstruct:reconstructed\"]}",
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows,
			SUBAPERTURE_ROWS, SUBAPERTURE_COLUMNS, SUBAPERTURES,
			ACTUATORS, origins);
	} else {
		written = snprintf(config, capacity,
			"{\"nodes\":["
			"{\"type\":\"ndarray\",\"name\":\"calibrate\","
			"\"plugin\":\"%s\",\"label\":\"pixel-calibration-u16-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"measure\","
			"\"plugin\":\"%s\",\"label\":\"shack-hartmann-image-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"subaperture_rows\":%u,\"subaperture_columns\":%u,"
			"\"subaperture_count\":%u,\"initial_subaperture_origins\":%s,"
			"\"coordinate_scale\":1.0,\"pixel_threshold\":0.0,"
			"\"flux_threshold\":1.0,\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"reconstruct\","
			"\"plugin\":\"%s\",\"label\":\"shwfs-reconstructor-f32\","
			"\"config\":{\"actuator_count\":%u,"
			"\"subaperture_count\":%u,\"reconstructed_schema\":"
			"\"org.calculon.ao.controller-residual-error/1\","
			"\"rate\":[1000,1]}}],"
			"\"links\":[{\"output\":\"calibrate:calibrated\","
			"\"input\":\"measure:image\"},"
			"{\"output\":\"measure:slopes\","
			"\"input\":\"reconstruct:slopes\"}],"
			"\"inputs\":[\"calibrate:raw\",\"calibrate:flat\","
			"\"calibrate:background\",\"measure:subaperture-origins\","
			"\"measure:coordinates\",\"measure:reference-slopes\","
			"\"measure:thresholds\",\"measure:active\","
			"\"reconstruct:reconstructor\"],"
			"\"outputs\":[\"reconstruct:reconstructed\"]}",
			plugin, IMAGE_ROWS, IMAGE_COLUMNS,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, SUBAPERTURE_ROWS,
			SUBAPERTURE_COLUMNS, SUBAPERTURES, origins,
			plugin, ACTUATORS, SUBAPERTURES);
	}
	spa_assert_se(written > 0 && (size_t)written < capacity);
	return config;
}

static void fill_inputs(uint16_t *image, float *matrix)
{
	for (uint32_t row = 0; row < IMAGE_ROWS; row++) {
		for (uint32_t column = 0; column < IMAGE_COLUMNS; column++) {
			uint32_t local_row = row % SUBAPERTURE_ROWS;
			uint32_t local_column = column % SUBAPERTURE_COLUMNS;

			image[(size_t)row * IMAGE_COLUMNS + column] =
				(uint16_t)(100u + 2u * local_row + local_column);
		}
	}
	for (uint32_t actuator = 0; actuator < ACTUATORS; actuator++) {
		for (uint32_t slope = 0; slope < SLOPES; slope++) {
			int32_t coefficient =
				(int32_t)((actuator * 17u + slope * 13u) % 31u) - 15;

			matrix[(size_t)actuator * SLOPES + slope] =
				(float)coefficient * 0.00001f;
		}
	}
}

static int process_full(struct spa_fgn_graph *graph,
		struct test_buffer *input, struct test_buffer *output,
		struct spa_buffer **inputs, struct spa_buffer **outputs, uint64_t seq)
{
	input->header.seq = seq;
	input->header.offset = 0;
	input->header.pts = SPA_TIME_INVALID;
	input->header.flags = SPA_META_HEADER_FLAG_MARKER;
	return spa_fgn_graph_process(graph, inputs, 9, outputs, 1);
}

static void prepare_row(struct test_buffer *input, uint16_t *image,
		uint32_t block_rows, uint32_t first_row, uint64_t seq)
{
	input->data.data = &image[(size_t)first_row * IMAGE_COLUMNS];
	input->header.seq = seq;
	input->header.offset = first_row;
	input->header.pts = SPA_TIME_INVALID;
	input->header.flags = first_row + block_rows == IMAGE_ROWS ?
		SPA_META_HEADER_FLAG_MARKER : 0;
}

static uint64_t process_rows(struct spa_fgn_graph *graph,
		struct test_buffer *input, struct test_buffer *output,
		struct spa_buffer **inputs, struct spa_buffer **outputs,
		uint16_t *image, uint32_t block_rows, uint64_t seq,
		uint64_t *terminal, uint64_t *maximum, uint64_t *block_times)
{
	uint64_t total = 0;
	uint32_t block = 0;

	*terminal = 0;
	*maximum = 0;
	for (uint32_t first_row = 0; first_row < IMAGE_ROWS;
			first_row += block_rows) {
		uint64_t start, elapsed;
		int res;

		prepare_row(input, image, block_rows, first_row, seq);
		start = monotonic_ns();
		res = spa_fgn_graph_process(graph, inputs, 9, outputs, 1);
		elapsed = monotonic_ns() - start;
		spa_assert_se(res >= 0);
		if (block_times != NULL)
			block_times[block] = elapsed;
		block++;
		total += elapsed;
		if (elapsed > *maximum)
			*maximum = elapsed;
		if (first_row + block_rows == IMAGE_ROWS) {
			*terminal = elapsed;
			spa_assert_se(output->chunk.size == ACTUATORS * sizeof(float));
			spa_assert_se(output->header.seq == seq);
		} else {
			spa_assert_se(output->chunk.size == 0);
		}
	}
	return total;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static size_t percentile(size_t count, uint32_t numerator,
		uint32_t denominator)
{
	return ((count * numerator + denominator - 1u) / denominator) - 1u;
}

static uint64_t report(const char *name, uint64_t *values, size_t count)
{
	long double sum = 0.0;

	qsort(values, count, sizeof(*values), compare_u64);
	for (size_t i = 0; i < count; i++)
		sum += values[i];
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64 " ns p99=%" PRIu64,
			name, count, sum / count, values[count / 2],
			values[percentile(count, 90, 100)],
			values[percentile(count, 99, 100)]);
	if (count >= 1000)
		printf(" ns p99.9=%" PRIu64,
				values[percentile(count, 999, 1000)]);
	printf(" ns max=%" PRIu64 " ns\n", values[count - 1]);
	return values[count / 2];
}

static void write_csv(const char *path, const uint64_t *full,
		const uint64_t *row_total, const uint64_t *row_terminal,
		const uint64_t *row_maximum, const uint64_t *row_blocks,
		uint32_t blocks_per_frame, size_t count)
{
	FILE *file;

	if (path == NULL)
		return;
	file = fopen(path, "we");
	spa_assert_se(file != NULL);
	spa_assert_se(fputs("iteration,full_ns,row_total_ns,row_terminal_ns,row_max_ns",
			file) >= 0);
	for (uint32_t block = 0; block < blocks_per_frame; block++)
		spa_assert_se(fprintf(file, ",row_block_%u_ns", block) > 0);
	spa_assert_se(fputc('\n', file) != EOF);
	for (size_t i = 0; i < count; i++) {
		spa_assert_se(fprintf(file, "%zu,%" PRIu64 ",%" PRIu64
				",%" PRIu64 ",%" PRIu64, i, full[i],
				row_total[i], row_terminal[i], row_maximum[i]) > 0);
		for (uint32_t block = 0; block < blocks_per_frame; block++)
			spa_assert_se(fprintf(file, ",%" PRIu64,
					row_blocks[i * blocks_per_frame + block]) > 0);
		spa_assert_se(fputc('\n', file) != EOF);
	}
	spa_assert_se(fclose(file) == 0);
}

int main(int argc, char *argv[])
{
	struct spa_fgn_graph *full_graph = NULL, *row_graph = NULL;
	struct test_buffer full_input, row_input, full_output, row_output;
	struct test_buffer full_parameter, row_parameter;
	struct spa_buffer *full_inputs[9] = { NULL }, *row_inputs[9] = { NULL };
	struct spa_buffer *full_outputs[1], *row_outputs[1];
	uint16_t *image;
	float *matrix, *full_values, *row_values;
	uint64_t *full_times, *row_total_times, *row_terminal_times, *row_max_times;
	uint64_t *row_block_times;
	uint64_t full_median, row_total_median, row_terminal_median;
	char *origins, *full_config, *row_config;
	uint32_t samples = DEFAULT_SAMPLES, warmup = DEFAULT_WARMUP;
	uint32_t block_rows = DEFAULT_BLOCK_ROWS;
	uint32_t blocks_per_frame;
	size_t image_elements = (size_t)IMAGE_ROWS * IMAGE_COLUMNS;
	size_t matrix_elements = (size_t)ACTUATORS * SLOPES;
	size_t block_trace_elements;
	uint64_t sequence = 1;

	spa_assert_se(argc >= 2 && argc <= 5);
	if (argc >= 3)
		samples = parse_u32(argv[2]);
	if (argc >= 4)
		warmup = parse_u32(argv[3]);
	if (argc == 5)
		block_rows = parse_u32(argv[4]);
	spa_assert_se(block_rows < IMAGE_ROWS && IMAGE_ROWS % block_rows == 0);
	blocks_per_frame = IMAGE_ROWS / block_rows;
	spa_assert_se((size_t)blocks_per_frame <= SIZE_MAX / (size_t)samples);
	block_trace_elements = (size_t)samples * blocks_per_frame;
	image = malloc(image_elements * sizeof(*image));
	matrix = malloc(matrix_elements * sizeof(*matrix));
	full_values = calloc(ACTUATORS, sizeof(*full_values));
	row_values = calloc(ACTUATORS, sizeof(*row_values));
	spa_assert_se(image != NULL && matrix != NULL && full_values != NULL &&
			row_values != NULL);
	fill_inputs(image, matrix);
	origins = make_origins();
	full_config = make_config(argv[1], origins, block_rows, false);
	row_config = make_config(argv[1], origins, block_rows, true);
	spa_assert_se(spa_fgn_graph_new(full_config, &full_graph) == 0);
	spa_assert_se(spa_fgn_graph_new(row_config, &row_graph) == 0);
	spa_assert_se(spa_fgn_graph_get_n_inputs(full_graph) == 9 &&
			spa_fgn_graph_get_n_inputs(row_graph) == 9);
	spa_assert_se(spa_fgn_graph_get_n_outputs(full_graph) == 1 &&
			spa_fgn_graph_get_n_outputs(row_graph) == 1);

	init_buffer(&full_input, image, (uint32_t)(image_elements * sizeof(*image)),
			(int32_t)(IMAGE_COLUMNS * sizeof(*image)), true);
	init_buffer(&row_input, image,
			block_rows * IMAGE_COLUMNS * (uint32_t)sizeof(*image),
			(int32_t)(IMAGE_COLUMNS * sizeof(*image)), true);
	init_buffer(&full_output, full_values, sizeof(float) * ACTUATORS,
			sizeof(float), false);
	init_buffer(&row_output, row_values, sizeof(float) * ACTUATORS,
			sizeof(float), false);
	init_buffer(&full_parameter, matrix,
			(uint32_t)(matrix_elements * sizeof(*matrix)),
			(int32_t)(SLOPES * sizeof(*matrix)), true);
	init_buffer(&row_parameter, matrix,
			(uint32_t)(matrix_elements * sizeof(*matrix)),
			(int32_t)(SLOPES * sizeof(*matrix)), true);
	full_parameter.header.seq = 1;
	row_parameter.header.seq = 1;
	full_inputs[0] = &full_input.buffer;
	row_inputs[0] = &row_input.buffer;
	full_outputs[0] = &full_output.buffer;
	row_outputs[0] = &row_output.buffer;
	spa_assert_se(spa_fgn_graph_activate(full_graph) == 0);
	spa_assert_se(spa_fgn_graph_activate(row_graph) == 0);
	spa_assert_se(spa_fgn_graph_update_parameter(full_graph, 8,
			&full_parameter.buffer) == 0);
	spa_assert_se(spa_fgn_graph_update_parameter(row_graph, 8,
			&row_parameter.buffer) == 0);

	spa_assert_se(process_full(full_graph, &full_input, &full_output,
			full_inputs, full_outputs, sequence) >= 0);
	{
		uint64_t terminal, maximum;

		(void)process_rows(row_graph, &row_input, &row_output, row_inputs,
				row_outputs, image, block_rows, sequence, &terminal, &maximum,
				NULL);
	}
	for (uint32_t actuator = 0; actuator < ACTUATORS; actuator++) {
		float absolute = fabsf(full_values[actuator] - row_values[actuator]);
		float scale = fmaxf(fabsf(full_values[actuator]),
				fabsf(row_values[actuator]));

		spa_assert_se(absolute <= 2.0e-6f * (1.0f + scale));
	}
	printf("numerical-equivalence: %u/%u reconstructed values within "
			"2e-6 absolute/scaled-relative tolerance\n", ACTUATORS, ACTUATORS);
	printf("workload: detector=%ux%u subapertures=%u subaperture=%ux%u "
			"slopes=%u actuators=%u block-rows=%u blocks=%u warmup=%u samples=%u\n",
			IMAGE_ROWS, IMAGE_COLUMNS, SUBAPERTURES, SUBAPERTURE_ROWS,
			SUBAPERTURE_COLUMNS, SLOPES, ACTUATORS, block_rows,
			IMAGE_ROWS / block_rows, warmup, samples);
	printf("boundary: warmed closed-loop graph callbacks; detector readout, "
			"PipeWire scheduling, publication, graph setup, and parameter setup excluded\n");

	for (uint32_t i = 0; i < warmup; i++) {
		uint64_t terminal, maximum;

		sequence++;
		spa_assert_se(process_full(full_graph, &full_input, &full_output,
				full_inputs, full_outputs, sequence) >= 0);
		(void)process_rows(row_graph, &row_input, &row_output, row_inputs,
				row_outputs, image, block_rows, sequence, &terminal, &maximum,
				NULL);
	}
	full_times = calloc(samples, sizeof(*full_times));
	row_total_times = calloc(samples, sizeof(*row_total_times));
	row_terminal_times = calloc(samples, sizeof(*row_terminal_times));
	row_max_times = calloc(samples, sizeof(*row_max_times));
	row_block_times = calloc(block_trace_elements, sizeof(*row_block_times));
	spa_assert_se(full_times != NULL && row_total_times != NULL &&
			row_terminal_times != NULL && row_max_times != NULL &&
			row_block_times != NULL);
	for (uint32_t i = 0; i < samples; i++) {
		uint64_t start;

		sequence++;
		if ((i & 1u) == 0) {
			start = monotonic_ns();
			spa_assert_se(process_full(full_graph, &full_input, &full_output,
					full_inputs, full_outputs, sequence) >= 0);
			full_times[i] = monotonic_ns() - start;
			row_total_times[i] = process_rows(row_graph, &row_input,
					&row_output, row_inputs, row_outputs, image,
					block_rows, sequence, &row_terminal_times[i],
					&row_max_times[i],
					&row_block_times[(size_t)i * blocks_per_frame]);
		} else {
			row_total_times[i] = process_rows(row_graph, &row_input,
					&row_output, row_inputs, row_outputs, image,
					block_rows, sequence, &row_terminal_times[i],
					&row_max_times[i],
					&row_block_times[(size_t)i * blocks_per_frame]);
			start = monotonic_ns();
			spa_assert_se(process_full(full_graph, &full_input, &full_output,
					full_inputs, full_outputs, sequence) >= 0);
			full_times[i] = monotonic_ns() - start;
		}
	}
	write_csv(getenv("PW_FGN_ROW_BENCHMARK_CSV"), full_times,
			row_total_times, row_terminal_times, row_max_times,
			row_block_times, blocks_per_frame, samples);
	full_median = report("complete-frame service", full_times, samples);
	row_total_median = report("row total service", row_total_times, samples);
	row_terminal_median = report("row terminal-block service",
			row_terminal_times, samples);
	(void)report("row maximum-block service", row_max_times, samples);
	printf("median total-service ratio: %.4f row/full (%+.2f%%)\n",
			(double)row_total_median / (double)full_median,
			100.0 * ((double)row_total_median / (double)full_median - 1.0));
	printf("median post-readout service ratio: %.4f row-terminal/full (%+.2f%%)\n",
			(double)row_terminal_median / (double)full_median,
			100.0 * ((double)row_terminal_median / (double)full_median - 1.0));

	spa_assert_se(spa_fgn_graph_deactivate(row_graph) == 0);
	spa_assert_se(spa_fgn_graph_deactivate(full_graph) == 0);
	spa_fgn_graph_free(row_graph);
	spa_fgn_graph_free(full_graph);
	free(row_block_times);
	free(row_max_times);
	free(row_terminal_times);
	free(row_total_times);
	free(full_times);
	free(row_config);
	free(full_config);
	free(origins);
	free(row_values);
	free(full_values);
	free(matrix);
	free(image);
	return 0;
}
