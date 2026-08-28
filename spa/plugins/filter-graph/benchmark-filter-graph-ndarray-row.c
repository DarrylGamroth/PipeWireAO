/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Callback service-time comparison for a complete-frame and a row-block
 * Calculon wavefront-sensor front end. This excludes detector readout, PipeWire
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

#ifdef BENCHMARK_PWFS
#define IMAGE_ROWS 64u
#define IMAGE_COLUMNS 64u
#define PUPIL_ROWS 30u
#define PUPIL_COLUMNS 30u
#define SELECTED_PER_PUPIL (PUPIL_ROWS * PUPIL_COLUMNS)
#define RECONSTRUCTION_INPUTS (4u * SELECTED_PER_PUPIL)
#define RECONSTRUCTED 253u
#define GRAPH_INPUTS 6u
#define GRAPH_OUTPUTS 3u
#define RECONSTRUCTOR_INPUT 5u
#define SENSOR_NAME "PWFS"
#else
#define IMAGE_ROWS 352u
#define IMAGE_COLUMNS 352u
#define SUBAPERTURE_ROWS 22u
#define SUBAPERTURE_COLUMNS 22u
#define SUBAPERTURES 256u
#define RECONSTRUCTION_INPUTS (SUBAPERTURES * 2u)
#define RECONSTRUCTED 277u
#define GRAPH_INPUTS 9u
#define GRAPH_OUTPUTS 1u
#define RECONSTRUCTOR_INPUT 8u
#define SENSOR_NAME "SHWFS"
#endif
#define SLOPES RECONSTRUCTION_INPUTS
#define ACTUATORS RECONSTRUCTED
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

static uint32_t parse_helpers(const char *text)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	spa_assert_se(errno == 0 && end != text && *end == '\0' &&
			value <= SPA_FGN_EXECUTOR_MAX_HELPERS);
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
#ifdef BENCHMARK_PWFS
	static const char value[] = "[[33,34],[0,0],[34,1],[0,34]]";
	char *origins = malloc(sizeof(value));

	spa_assert_se(origins != NULL);
	memcpy(origins, value, sizeof(value));
	return origins;
#else
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
#endif
}

#ifdef BENCHMARK_PWFS
static char *make_mask(void)
{
	size_t capacity = SELECTED_PER_PUPIL * 6u + 3u;
	char *mask = malloc(capacity);
	size_t offset = 0;

	spa_assert_se(mask != NULL);
	mask[offset++] = '[';
	for (uint32_t pixel = 0; pixel < SELECTED_PER_PUPIL; pixel++) {
		int written = snprintf(mask + offset, capacity - offset,
				"%strue", pixel == 0 ? "" : ",");

		spa_assert_se(written > 0 && (size_t)written < capacity - offset);
		offset += (size_t)written;
	}
	mask[offset++] = ']';
	mask[offset] = '\0';
	return mask;
}
#endif

static char *make_config(const char *plugin, const char *origins,
		const char *mask, uint32_t block_rows, uint32_t worker_helpers,
		bool row)
{
	size_t capacity = strlen(plugin) * 3u + strlen(origins) +
		(mask == NULL ? 0u : strlen(mask)) + 8192u;
	char *config = malloc(capacity);
	int written;

	spa_assert_se(config != NULL && strchr(plugin, '"') == NULL);
#ifdef BENCHMARK_PWFS
	spa_assert_se(mask != NULL);
	if (row) {
		written = snprintf(config, capacity,
			"{\"workers\":{\"helpers\":%u},\"nodes\":["
			"{\"type\":\"ndarray\",\"name\":\"calibrate\","
			"\"plugin\":\"%s\",\"label\":\"pixel-calibration-row-u16-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"row_block_rows\":%u,\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"reconstruct\","
			"\"plugin\":\"%s\",\"label\":\"pwfs-row-reconstructor-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"row_block_rows\":%u,\"pupil_rows\":%u,"
			"\"pupil_columns\":%u,\"pupil_origins\":%s,"
			"\"pupil_mask\":%s,\"reconstructed_count\":%u,"
			"\"reconstructed_schema\":\"org.calculon.benchmark.pwfs/1\","
			"\"rate\":[1000,1]}}],"
			"\"links\":[{\"output\":\"calibrate:calibrated\","
			"\"input\":\"reconstruct:row-block\"}],"
			"\"inputs\":[\"calibrate:raw\",\"calibrate:flat\","
			"\"calibrate:background\",\"reconstruct:pupil-origins\","
			"\"reconstruct:pupil-mask\",\"reconstruct:reconstructor\"],"
			"\"outputs\":[\"reconstruct:reconstruction-pixels\","
			"\"reconstruct:mean-pupil-intensity\","
			"\"reconstruct:reconstructed\"]}",
			worker_helpers, plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows, PUPIL_ROWS,
			PUPIL_COLUMNS, origins, mask, RECONSTRUCTED);
	} else {
		written = snprintf(config, capacity,
			"{\"workers\":{\"helpers\":%u},\"nodes\":["
			"{\"type\":\"ndarray\",\"name\":\"calibrate\","
			"\"plugin\":\"%s\",\"label\":\"pixel-calibration-u16-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"normalize\","
			"\"plugin\":\"%s\",\"label\":\"pyramid-pixel-image-f32\","
			"\"config\":{\"image_rows\":%u,\"image_columns\":%u,"
			"\"pupil_rows\":%u,\"pupil_columns\":%u,"
			"\"pupil_origins\":%s,\"pupil_mask\":%s,"
			"\"rate\":[1000,1]}},"
			"{\"type\":\"ndarray\",\"name\":\"reconstruct\","
			"\"plugin\":\"%s\",\"label\":\"pwfs-reconstructor-f32\","
			"\"config\":{\"reconstructed_count\":%u,"
			"\"selected_pixel_count\":%u,"
			"\"reconstructed_schema\":\"org.calculon.benchmark.pwfs/1\","
			"\"rate\":[1000,1]}}],"
			"\"links\":[{\"output\":\"calibrate:calibrated\","
			"\"input\":\"normalize:image\"},"
			"{\"output\":\"normalize:reconstruction-pixels\","
			"\"input\":\"reconstruct:reconstruction-pixels\"}],"
			"\"inputs\":[\"calibrate:raw\",\"calibrate:flat\","
			"\"calibrate:background\",\"normalize:pupil-origins\","
			"\"normalize:pupil-mask\",\"reconstruct:reconstructor\"],"
			"\"outputs\":[\"normalize:reconstruction-pixels\","
			"\"normalize:mean-pupil-intensity\","
			"\"reconstruct:reconstructed\"]}",
			worker_helpers, plugin, IMAGE_ROWS, IMAGE_COLUMNS,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, PUPIL_ROWS, PUPIL_COLUMNS,
			origins, mask, plugin, RECONSTRUCTED, SELECTED_PER_PUPIL);
	}
#else
	(void)mask;
	if (row) {
		written = snprintf(config, capacity,
			"{\"workers\":{\"helpers\":%u},\"nodes\":["
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
			worker_helpers, plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, block_rows,
			SUBAPERTURE_ROWS, SUBAPERTURE_COLUMNS, SUBAPERTURES,
			ACTUATORS, origins);
	} else {
		written = snprintf(config, capacity,
			"{\"workers\":{\"helpers\":%u},\"nodes\":["
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
			worker_helpers, plugin, IMAGE_ROWS, IMAGE_COLUMNS,
			plugin, IMAGE_ROWS, IMAGE_COLUMNS, SUBAPERTURE_ROWS,
			SUBAPERTURE_COLUMNS, SUBAPERTURES, origins,
			plugin, ACTUATORS, SUBAPERTURES);
	}
#endif
	spa_assert_se(written > 0 && (size_t)written < capacity);
	return config;
}

static void fill_inputs(uint16_t *image, float *matrix)
{
	for (uint32_t row = 0; row < IMAGE_ROWS; row++) {
		for (uint32_t column = 0; column < IMAGE_COLUMNS; column++) {
#ifdef BENCHMARK_PWFS
			image[(size_t)row * IMAGE_COLUMNS + column] =
				(uint16_t)(100u + 3u * row + 2u * column);
#else
			uint32_t local_row = row % SUBAPERTURE_ROWS;
			uint32_t local_column = column % SUBAPERTURE_COLUMNS;

			image[(size_t)row * IMAGE_COLUMNS + column] =
				(uint16_t)(100u + 2u * local_row + local_column);
#endif
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

static void load_matrix_if_requested(float *matrix, size_t elements)
{
	const char *path = getenv("PW_FGN_ROW_BENCHMARK_MATRIX_F32");
	FILE *file;

	if (path == NULL)
		return;
	file = fopen(path, "re");
	spa_assert_se(file != NULL);
	spa_assert_se(fread(matrix, sizeof(*matrix), elements, file) == elements);
	spa_assert_se(fgetc(file) == EOF);
	spa_assert_se(fclose(file) == 0);
}

static int process_full(struct spa_fgn_graph *graph,
		struct test_buffer *input, struct test_buffer *output,
		struct spa_buffer **inputs, struct spa_buffer **outputs, uint64_t seq)
{
	input->header.seq = seq;
	input->header.offset = 0;
	input->header.pts = SPA_TIME_INVALID;
	input->header.flags = SPA_META_HEADER_FLAG_MARKER;
	return spa_fgn_graph_process(graph, inputs, GRAPH_INPUTS,
			outputs, GRAPH_OUTPUTS);
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
		res = spa_fgn_graph_process(graph, inputs, GRAPH_INPUTS,
				outputs, GRAPH_OUTPUTS);
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
	struct spa_buffer *full_inputs[GRAPH_INPUTS] = { NULL };
	struct spa_buffer *row_inputs[GRAPH_INPUTS] = { NULL };
	struct spa_buffer *full_outputs[GRAPH_OUTPUTS];
	struct spa_buffer *row_outputs[GRAPH_OUTPUTS];
#ifdef BENCHMARK_PWFS
	struct test_buffer full_pixels_output, row_pixels_output;
	struct test_buffer full_mean_output, row_mean_output;
	float *full_pixels, *row_pixels;
	float full_mean = -1.0f, row_mean = -1.0f;
	char *mask;
#endif
	uint16_t *image;
	float *matrix, *full_values, *row_values;
	uint64_t *full_times, *row_total_times, *row_terminal_times, *row_max_times;
	uint64_t *row_block_times;
	uint64_t full_median, row_total_median, row_terminal_median;
	char *origins, *full_config, *row_config;
	uint32_t samples = DEFAULT_SAMPLES, warmup = DEFAULT_WARMUP;
	uint32_t block_rows = DEFAULT_BLOCK_ROWS;
	uint32_t worker_helpers = 0;
	uint32_t blocks_per_frame;
	bool row_first = getenv("PW_FGN_ROW_BENCHMARK_ROW_FIRST") != NULL;
	size_t image_elements = (size_t)IMAGE_ROWS * IMAGE_COLUMNS;
	size_t matrix_elements = (size_t)ACTUATORS * SLOPES;
	size_t block_trace_elements;
	uint64_t sequence = 1;

	spa_assert_se(argc >= 2 && argc <= 6);
	if (argc >= 3)
		samples = parse_u32(argv[2]);
	if (argc >= 4)
		warmup = parse_u32(argv[3]);
	if (argc == 5)
		block_rows = parse_u32(argv[4]);
	if (argc == 6) {
		block_rows = parse_u32(argv[4]);
		worker_helpers = parse_helpers(argv[5]);
	}
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
	load_matrix_if_requested(matrix, matrix_elements);
	origins = make_origins();
#ifdef BENCHMARK_PWFS
	mask = make_mask();
#else
	const char *mask = NULL;
#endif
	full_config = make_config(argv[1], origins, mask, block_rows,
			worker_helpers, false);
	row_config = make_config(argv[1], origins, mask, block_rows,
			worker_helpers, true);
	spa_assert_se(spa_fgn_graph_new(full_config, &full_graph) == 0);
	spa_assert_se(spa_fgn_graph_new(row_config, &row_graph) == 0);
	spa_assert_se(spa_fgn_graph_get_n_inputs(full_graph) == GRAPH_INPUTS &&
			spa_fgn_graph_get_n_inputs(row_graph) == GRAPH_INPUTS);
	spa_assert_se(spa_fgn_graph_get_n_outputs(full_graph) == GRAPH_OUTPUTS &&
			spa_fgn_graph_get_n_outputs(row_graph) == GRAPH_OUTPUTS);

	init_buffer(&full_input, image, (uint32_t)(image_elements * sizeof(*image)),
			(int32_t)(IMAGE_COLUMNS * sizeof(*image)), true);
	init_buffer(&row_input, image,
			block_rows * IMAGE_COLUMNS * (uint32_t)sizeof(*image),
			(int32_t)(IMAGE_COLUMNS * sizeof(*image)), true);
	init_buffer(&full_output, full_values, sizeof(float) * ACTUATORS,
			sizeof(float), false);
	init_buffer(&row_output, row_values, sizeof(float) * ACTUATORS,
			sizeof(float), false);
#ifdef BENCHMARK_PWFS
	full_pixels = calloc(RECONSTRUCTION_INPUTS, sizeof(*full_pixels));
	row_pixels = calloc(RECONSTRUCTION_INPUTS, sizeof(*row_pixels));
	spa_assert_se(full_pixels != NULL && row_pixels != NULL);
	init_buffer(&full_pixels_output, full_pixels,
			RECONSTRUCTION_INPUTS * (uint32_t)sizeof(*full_pixels),
			(int32_t)(SELECTED_PER_PUPIL * sizeof(*full_pixels)), false);
	init_buffer(&row_pixels_output, row_pixels,
			RECONSTRUCTION_INPUTS * (uint32_t)sizeof(*row_pixels),
			(int32_t)(SELECTED_PER_PUPIL * sizeof(*row_pixels)), false);
	init_buffer(&full_mean_output, &full_mean, sizeof(full_mean),
			sizeof(full_mean), false);
	init_buffer(&row_mean_output, &row_mean, sizeof(row_mean),
			sizeof(row_mean), false);
#endif
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
#ifdef BENCHMARK_PWFS
	full_outputs[0] = &full_pixels_output.buffer;
	full_outputs[1] = &full_mean_output.buffer;
	full_outputs[2] = &full_output.buffer;
	row_outputs[0] = &row_pixels_output.buffer;
	row_outputs[1] = &row_mean_output.buffer;
	row_outputs[2] = &row_output.buffer;
#else
	full_outputs[0] = &full_output.buffer;
	row_outputs[0] = &row_output.buffer;
#endif
	spa_assert_se(spa_fgn_graph_activate(full_graph) == 0);
	spa_assert_se(spa_fgn_graph_update_parameter(full_graph, RECONSTRUCTOR_INPUT,
			&full_parameter.buffer) == 0);
#ifdef BENCHMARK_PWFS
	for (sequence = 1; sequence <= 2; sequence++)
		spa_assert_se(process_full(full_graph, &full_input, &full_output,
				full_inputs, full_outputs, sequence) >= 0);
#else
	spa_assert_se(process_full(full_graph, &full_input, &full_output,
			full_inputs, full_outputs, sequence) >= 0);
#endif
	spa_assert_se(spa_fgn_graph_deactivate(full_graph) == 0);
	spa_assert_se(spa_fgn_graph_activate(row_graph) == 0);
	spa_assert_se(spa_fgn_graph_update_parameter(row_graph, RECONSTRUCTOR_INPUT,
			&row_parameter.buffer) == 0);
	{
		uint64_t terminal, maximum;

#ifdef BENCHMARK_PWFS
		for (sequence = 1; sequence <= 2; sequence++)
			(void)process_rows(row_graph, &row_input, &row_output, row_inputs,
					row_outputs, image, block_rows, sequence, &terminal,
					&maximum, NULL);
#else
		(void)process_rows(row_graph, &row_input, &row_output, row_inputs,
				row_outputs, image, block_rows, sequence, &terminal, &maximum,
				NULL);
#endif
	}
	spa_assert_se(spa_fgn_graph_deactivate(row_graph) == 0);
#ifdef BENCHMARK_PWFS
	spa_assert_se(full_mean == row_mean);
	for (uint32_t pixel = 0; pixel < RECONSTRUCTION_INPUTS; pixel++)
		spa_assert_se(full_pixels[pixel] == row_pixels[pixel]);
#endif
	for (uint32_t actuator = 0; actuator < ACTUATORS; actuator++) {
		float absolute = fabsf(full_values[actuator] - row_values[actuator]);
		float scale = fmaxf(fabsf(full_values[actuator]),
				fabsf(row_values[actuator]));

		spa_assert_se(absolute <= 2.0e-6f * (1.0f + scale));
	}
	printf("numerical-equivalence: %u/%u reconstructed values within "
			"2e-6 absolute/scaled-relative tolerance\n", ACTUATORS, ACTUATORS);
	#ifdef BENCHMARK_PWFS
	printf("workload: sensor=%s detector=%ux%u pupils=4 pupil=%ux%u "
			"pixels=%u reconstructed=%u block-rows=%u blocks=%u helpers=%u "
			"warmup=%u samples=%u\n",
			SENSOR_NAME, IMAGE_ROWS, IMAGE_COLUMNS, PUPIL_ROWS, PUPIL_COLUMNS,
			RECONSTRUCTION_INPUTS, RECONSTRUCTED, block_rows,
			IMAGE_ROWS / block_rows, worker_helpers, warmup, samples);
	#else
	printf("workload: sensor=%s detector=%ux%u subapertures=%u subaperture=%ux%u "
			"slopes=%u actuators=%u block-rows=%u blocks=%u helpers=%u "
			"warmup=%u samples=%u\n",
			SENSOR_NAME, IMAGE_ROWS, IMAGE_COLUMNS, SUBAPERTURES, SUBAPERTURE_ROWS,
			SUBAPERTURE_COLUMNS, SLOPES, ACTUATORS, block_rows,
			IMAGE_ROWS / block_rows, worker_helpers, warmup, samples);
	#endif
	printf("boundary: warmed closed-loop graph callbacks; detector readout, "
			"PipeWire scheduling, publication, graph setup, and parameter setup excluded\n");
	printf("execution-order: %s first; only one polling worker group active at a time\n",
			row_first ? "row" : "complete-frame");
	full_times = calloc(samples, sizeof(*full_times));
	row_total_times = calloc(samples, sizeof(*row_total_times));
	row_terminal_times = calloc(samples, sizeof(*row_terminal_times));
	row_max_times = calloc(samples, sizeof(*row_max_times));
	row_block_times = calloc(block_trace_elements, sizeof(*row_block_times));
	spa_assert_se(full_times != NULL && row_total_times != NULL &&
			row_terminal_times != NULL && row_max_times != NULL &&
			row_block_times != NULL);
	for (uint32_t phase = 0; phase < 2; phase++) {
		bool row_phase = (phase == 0) == row_first;
		struct spa_fgn_graph *graph = row_phase ? row_graph : full_graph;

		spa_assert_se(spa_fgn_graph_activate(graph) == 0);
		for (uint32_t i = 0; i < warmup; i++) {
			sequence++;
			if (row_phase) {
				uint64_t terminal, maximum;

				(void)process_rows(row_graph, &row_input, &row_output,
						row_inputs, row_outputs, image, block_rows,
						sequence, &terminal, &maximum, NULL);
			} else {
				spa_assert_se(process_full(full_graph, &full_input,
						&full_output, full_inputs, full_outputs,
						sequence) >= 0);
			}
		}
		for (uint32_t i = 0; i < samples; i++) {
			sequence++;
			if (row_phase) {
				row_total_times[i] = process_rows(row_graph, &row_input,
						&row_output, row_inputs, row_outputs, image,
						block_rows, sequence, &row_terminal_times[i],
						&row_max_times[i],
						&row_block_times[(size_t)i * blocks_per_frame]);
			} else {
				uint64_t start = monotonic_ns();

				spa_assert_se(process_full(full_graph, &full_input,
						&full_output, full_inputs, full_outputs,
						sequence) >= 0);
				full_times[i] = monotonic_ns() - start;
			}
		}
		spa_assert_se(spa_fgn_graph_deactivate(graph) == 0);
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
#ifdef BENCHMARK_PWFS
	free(mask);
	free(row_pixels);
	free(full_pixels);
#endif
	free(row_values);
	free(full_values);
	free(matrix);
	free(image);
	return 0;
}
