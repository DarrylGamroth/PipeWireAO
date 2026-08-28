/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Direct-call numerical and service-time qualification for the generated
 * REVOLT Classic ndarray filter graph. This excludes PipeWire scheduling and
 * device I/O. The fixture is prepared by calculon-revolt-classic-direct-replay.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/filter-graph/filter-graph-ndarray.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#define FRAME_COUNT 7u
#define FRAME_ROWS 352u
#define FRAME_COLUMNS 352u
#define FRAME_PIXELS ((size_t)FRAME_ROWS * FRAME_COLUMNS)
#define SUBAPERTURE_COUNT 188u
#define SUBAPERTURE_PIXELS (22u * 22u)
#define SLOPE_COUNT (SUBAPERTURE_COUNT * 2u)
#define CONTROLLED_VDM_SIZE 221u
#define FULL_VDM_SIZE 277u
#define PDM_SIZE 277u
#define GRAPH_INPUT_COUNT 15u
#define GRAPH_OUTPUT_COUNT 6u
#define PARAMETER_COUNT (GRAPH_INPUT_COUNT - 2u)

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
	uint8_t *payload;
	size_t size;
};

struct comparison {
	const char *name;
	double maximum_absolute_error;
	double maximum_scaled_relative_error;
	size_t worst_index;
	uint32_t worst_frame;
	bool exact;
};

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static void check_result(int result, const char *operation)
{
	if (result < 0) {
		fprintf(stderr, "%s failed: %s (%d)\n", operation,
				spa_strerror(result), result);
		exit(EXIT_FAILURE);
	}
}

static uint32_t parse_u32(const char *text, bool allow_zero)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    (!allow_zero && value == 0) || value > UINT32_MAX)
		fail("invalid sample or warmup count");
	return (uint32_t)value;
}

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0)
		fail("clock_gettime failed");
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

static size_t element_size(uint32_t element_type)
{
	size_t size = spa_element_type_size(element_type);

	if (size == 0)
		fail("graph exposed an unknown ndarray element type");
	return size;
}

static size_t format_size(const struct spa_fgn_format *format)
{
	size_t elements = 1;
	uint32_t i;

	if (format == NULL || format->shape == NULL || format->n_dimensions == 0)
		fail("graph exposed an invalid ndarray format");
	for (i = 0; i < format->n_dimensions; i++) {
		if (format->shape[i] == 0 || elements > SIZE_MAX / format->shape[i])
			fail("graph ndarray extent overflow");
		elements *= format->shape[i];
	}
	if (elements > SIZE_MAX / element_size(format->element_type))
		fail("graph ndarray byte extent overflow");
	return elements * element_size(format->element_type);
}

static void init_buffer(struct test_buffer *storage,
		const struct spa_fgn_format *format, bool input)
{
	uint32_t contiguous_axis;
	size_t stride;

	memset(storage, 0, sizeof(*storage));
	storage->size = format_size(format);
	if (storage->size > UINT32_MAX)
		fail("test buffer exceeds SPA capacity");
	storage->payload = aligned_alloc(64,
			SPA_ROUND_UP_N(storage->size, 64u));
	if (storage->payload == NULL)
		fail("test buffer allocation failed");
	memset(storage->payload, 0, storage->size);
	contiguous_axis = format->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR
		? format->n_dimensions - 1u : 0u;
	stride = (size_t)format->shape[contiguous_axis] *
		element_size(format->element_type);
	if (stride > INT32_MAX)
		fail("test buffer stride exceeds SPA capacity");
	storage->header.pts = SPA_TIME_INVALID;
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = (uint32_t)storage->size;
	storage->data.data = storage->payload;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = input ? (uint32_t)storage->size : 0u;
	storage->chunk.stride = (int32_t)stride;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void clear_buffer(struct test_buffer *storage)
{
	free(storage->payload);
	memset(storage, 0, sizeof(*storage));
}

static char *fixture_path(const char *directory, const char *name)
{
	size_t size = strlen(directory) + strlen(name) + 2u;
	char *path = malloc(size);

	if (path == NULL)
		fail("fixture path allocation failed");
	if (snprintf(path, size, "%s/%s", directory, name) < 0)
		fail("fixture path formatting failed");
	return path;
}

static size_t file_size(FILE *file, const char *path)
{
	long size;

	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "cannot determine fixture size: %s\n", path);
		exit(EXIT_FAILURE);
	}
	return (size_t)size;
}

static void read_exact_file(const char *directory, const char *name,
		void *destination, size_t expected_size)
{
	char *path = fixture_path(directory, name);
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		fprintf(stderr, "cannot open fixture %s: %s\n", path,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (file_size(file, path) != expected_size ||
	    fread(destination, 1, expected_size, file) != expected_size ||
	    fgetc(file) != EOF || fclose(file) != 0) {
		fprintf(stderr, "fixture has an unexpected extent: %s\n", path);
		exit(EXIT_FAILURE);
	}
	free(path);
}

static void *read_file(const char *directory, const char *name, size_t *size)
{
	char *path = fixture_path(directory, name);
	FILE *file = fopen(path, "rb");
	void *data;

	if (file == NULL) {
		fprintf(stderr, "cannot open fixture %s: %s\n", path,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	*size = file_size(file, path);
	if (*size == 0 || (data = malloc(*size)) == NULL)
		fail("fixture allocation failed");
	if (fread(data, 1, *size, file) != *size || fgetc(file) != EOF ||
	    fclose(file) != 0) {
		fprintf(stderr, "cannot read fixture %s\n", path);
		exit(EXIT_FAILURE);
	}
	free(path);
	return data;
}

static char *read_graph_config(const char *path)
{
	struct spa_json top;
	const char *token;
	char key[256], *text, *graph = NULL;
	size_t size;
	FILE *file = fopen(path, "rb");
	int len;

	if (file == NULL) {
		fprintf(stderr, "cannot open graph configuration %s: %s\n",
				path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	size = file_size(file, path);
	if (size == SIZE_MAX || (text = malloc(size + 1u)) == NULL)
		fail("graph configuration allocation failed");
	if (fread(text, 1, size, file) != size || fgetc(file) != EOF ||
	    fclose(file) != 0)
		fail("cannot read graph configuration");
	text[size] = '\0';
	if (spa_json_begin_object_relax(&top, text, size) <= 0)
		fail("graph configuration is not an object");
	while ((len = spa_json_object_next(&top, key, sizeof(key), &token)) > 0) {
		if (!spa_streq(key, "filter.graph")) {
			if (spa_json_is_container(token, len) &&
			    spa_json_container_len(&top, token, len) <= 0)
				fail("invalid graph configuration value");
			continue;
		}
		if (graph != NULL || !spa_json_is_object(token, len) ||
		    (len = spa_json_container_len(&top, token, len)) <= 0 ||
		    (graph = malloc((size_t)len + 1u)) == NULL)
			fail("invalid or duplicate filter.graph configuration");
		memcpy(graph, token, (size_t)len);
		graph[len] = '\0';
	}
	free(text);
	if (len < 0 || graph == NULL)
		fail("graph configuration has no valid filter.graph object");
	return graph;
}

static void require_port(struct spa_fgn_graph *graph,
		enum spa_direction direction, uint32_t index,
		const char *node, const char *name, bool parameter)
{
	const struct spa_fgn_port_info *info;
	const char *actual_node;
	int result = spa_fgn_graph_get_port_info(graph, direction, index,
			&actual_node, &info);

	check_result(result, "get graph port information");
	if (!spa_streq(actual_node, node) || !spa_streq(info->name, name) ||
	    !!(info->flags & SPA_FGN_PORT_FLAG_PARAMETER) != parameter)
		fail("Classic graph external port order is not canonical");
}

static void compare_values(struct comparison *comparison, const float *actual,
		const float *expected, size_t count, uint32_t frame)
{
	size_t i;

	for (i = 0; i < count; i++) {
		double absolute, scaled_relative, scale;

		if (!isfinite(actual[i]) || !isfinite(expected[i]))
			fail("non-finite value at a Classic comparison boundary");
		if (comparison->exact && memcmp(&actual[i], &expected[i],
				sizeof(float)) != 0)
			fail("calibrated Classic pixels are not bitwise equal");
		absolute = fabs((double)actual[i] - (double)expected[i]);
		scale = fmax(fmax(fabs((double)actual[i]), fabs((double)expected[i])),
				1.0);
		scaled_relative = absolute / scale;
		if (!comparison->exact && absolute > 1.0e-6 &&
		    scaled_relative > 1.0e-6) {
			fprintf(stderr,
				"%s frame %u index %zu differs: actual %.9g expected %.9g abs %.9g scaled-rel %.9g\n",
				comparison->name, frame, i, (double)actual[i],
				(double)expected[i], absolute, scaled_relative);
			exit(EXIT_FAILURE);
		}
		if (absolute > comparison->maximum_absolute_error) {
			comparison->maximum_absolute_error = absolute;
			comparison->worst_index = i;
			comparison->worst_frame = frame;
		}
		comparison->maximum_scaled_relative_error = fmax(
				comparison->maximum_scaled_relative_error, scaled_relative);
	}
}

static void prepare_frame(struct test_buffer *raw,
		struct test_buffer *feedback, const uint16_t *frames,
		uint64_t sequence)
{
	size_t frame_index = (size_t)((sequence - 1u) % FRAME_COUNT);

	memcpy(raw->payload, frames + frame_index * FRAME_PIXELS, raw->size);
	raw->chunk.size = (uint32_t)raw->size;
	raw->header.seq = (uint32_t)sequence;
	feedback->chunk.size = (uint32_t)feedback->size;
}

static void carry_feedback(struct test_buffer *input,
		const struct test_buffer *output)
{
	memcpy(input->payload, output->payload, input->size);
	input->header = output->header;
	input->chunk.size = (uint32_t)input->size;
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

static void report(const char *name, uint64_t *samples, size_t count)
{
	long double sum = 0.0;
	size_t i;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (i = 0; i < count; i++)
		sum += samples[i];
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64 " ns p99=%" PRIu64,
			name, count, sum / count, samples[count / 2u],
			samples[percentile(count, 90, 100)],
			samples[percentile(count, 99, 100)]);
	if (count >= 1000u)
		printf(" ns p99.9=%" PRIu64,
				samples[percentile(count, 999, 1000)]);
	printf(" ns max=%" PRIu64 " ns\n", samples[count - 1u]);
}

static void write_csv(const char *path, const uint64_t *samples, size_t count)
{
	FILE *file;
	size_t i;

	if (path == NULL)
		return;
	file = fopen(path, "w");
	if (file == NULL)
		fail("cannot create graph timing CSV");
	if (fputs("iteration,graph_ns\n", file) < 0)
		fail("cannot write graph timing CSV");
	for (i = 0; i < count; i++)
		if (fprintf(file, "%zu,%" PRIu64 "\n", i, samples[i]) < 0)
			fail("cannot write graph timing sample");
	if (fclose(file) != 0)
		fail("cannot close graph timing CSV");
}

int main(int argc, char *argv[])
{
	static const char *parameter_files[PARAMETER_COUNT] = {
		"background.f32le",
		"subaperture-origins.u32le",
		"shack-hartmann-coordinates.f32le",
		"reference-slopes.f32le",
		"thresholds.f32le",
		"active-subapertures.u8",
		"reconstructor.f32le",
		"controller-to-vdm.f32le",
		"active-to-full-vdm.f32le",
		"vdm-to-pdm.f32le",
		"full-to-active-vdm.f32le",
		"pdm-to-vdm.f32le",
		"vdm-to-controller.f32le",
	};
	static const char *expected_files[5] = {
		"calibrated_pixels.f32le", "slopes.f32le", "dm_error.f32le",
		"vdm_command.f32le", "demanded_pdm_command.f32le",
	};
	static const size_t output_elements[GRAPH_OUTPUT_COUNT] = {
		FRAME_PIXELS, SLOPE_COUNT, CONTROLLED_VDM_SIZE,
		CONTROLLED_VDM_SIZE, PDM_SIZE, CONTROLLED_VDM_SIZE,
	};
	static const char *input_nodes[GRAPH_INPUT_COUNT] = {
		"pixel-calibration", "closed-loop-correction",
		"pixel-calibration", "shack-hartmann", "shack-hartmann",
		"shack-hartmann", "shack-hartmann", "shack-hartmann",
		"reconstruction", "controller-to-vdm", "vdm-to-pdm",
		"vdm-to-pdm", "pdm-feedback-to-vdm", "pdm-feedback-to-vdm",
		"vdm-feedback-to-controller",
	};
	static const char *input_names[GRAPH_INPUT_COUNT] = {
		"raw", "constraint-feedback", "background",
		"subaperture-origins", "coordinates", "reference-slopes",
		"thresholds", "active", "reconstructor", "controller-to-vdm",
		"active-to-full", "vdm-to-pdm", "full-to-active", "pdm-to-vdm",
		"vdm-to-controller",
	};
	static const char *output_nodes[GRAPH_OUTPUT_COUNT] = {
		"pixel-calibration", "shack-hartmann", "reconstruction",
		"closed-loop-correction", "pdm-command",
		"vdm-feedback-to-controller",
	};
	static const char *output_names[GRAPH_OUTPUT_COUNT] = {
		"calibrated", "slopes", "reconstructed", "correction", "demanded",
		"controller-constraint-feedback",
	};
	struct spa_fgn_graph *graph = NULL;
	struct test_buffer inputs[GRAPH_INPUT_COUNT], outputs[GRAPH_OUTPUT_COUNT];
	struct spa_buffer *process_inputs[GRAPH_INPUT_COUNT] = { NULL };
	struct spa_buffer *process_outputs[GRAPH_OUTPUT_COUNT];
	struct spa_fgn_parameter_update updates[PARAMETER_COUNT];
	const struct spa_fgn_format *format;
	struct comparison comparisons[5] = {
		{ .name = "calibrated pixels", .exact = true },
		{ .name = "slopes" },
		{ .name = "reconstructed DM error" },
		{ .name = "unconstrained VDM command" },
		{ .name = "demanded PDM command" },
	};
	uint16_t *raw_frames;
	float *expected[5], *feedback_expected[3];
	size_t size, feedback_size[3], feedback_frames;
	char *config;
	uint64_t sequence;
	uint64_t *times;
	uint32_t samples = 10000u, warmup = 1000u, i;
	size_t feedback_frame;
	const char *csv = NULL;
	uint32_t expected_nonzero_feedback = 0, actual_nonzero_feedback = 0;
	const uint16_t endian_probe = 1u;

	if (argc < 3 || argc > 6) {
		fprintf(stderr,
				"usage: %s RESOLVED_FILTER_CHAIN_CONFIG FIXTURE_DIRECTORY [SAMPLES [WARMUP [CSV]]]\n",
				argv[0]);
		return EXIT_FAILURE;
	}
	if (*(const uint8_t *)&endian_probe != 1u)
		fail("the little-endian Classic fixture is not supported on this host");
	if (argc >= 4)
		samples = parse_u32(argv[3], false);
	if (argc >= 5)
		warmup = parse_u32(argv[4], true);
	if (argc == 6)
		csv = argv[5];
	config = read_graph_config(argv[1]);
	check_result(spa_fgn_graph_new(config, &graph), "build Classic graph");
	free(config);
	if (spa_fgn_graph_get_n_inputs(graph) != GRAPH_INPUT_COUNT ||
	    spa_fgn_graph_get_n_outputs(graph) != GRAPH_OUTPUT_COUNT)
		fail("Classic graph external port count is incorrect");

	for (i = 0; i < GRAPH_INPUT_COUNT; i++)
		require_port(graph, SPA_DIRECTION_INPUT, i, input_nodes[i],
				input_names[i], i >= 2u);
	for (i = 0; i < GRAPH_OUTPUT_COUNT; i++)
		require_port(graph, SPA_DIRECTION_OUTPUT, i, output_nodes[i],
				output_names[i], false);

	for (i = 0; i < GRAPH_INPUT_COUNT; i++) {
		check_result(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_INPUT,
				i, &format), "get Classic input format");
		init_buffer(&inputs[i], format, true);
	}
	for (i = 0; i < GRAPH_OUTPUT_COUNT; i++) {
		check_result(spa_fgn_graph_get_port_format(graph, SPA_DIRECTION_OUTPUT,
				i, &format), "get Classic output format");
		init_buffer(&outputs[i], format, false);
		process_outputs[i] = &outputs[i].buffer;
		if (outputs[i].size != output_elements[i] * sizeof(float))
			fail("Classic output format has an unexpected extent");
	}
	if (inputs[0].size != FRAME_PIXELS * sizeof(uint16_t) ||
	    inputs[1].size != CONTROLLED_VDM_SIZE * sizeof(float))
		fail("Classic data input format has an unexpected extent");
	process_inputs[0] = &inputs[0].buffer;
	process_inputs[1] = &inputs[1].buffer;

	for (i = 0; i < PARAMETER_COUNT; i++) {
		read_exact_file(argv[2], parameter_files[i], inputs[i + 2u].payload,
				inputs[i + 2u].size);
		inputs[i + 2u].header.seq = 1u;
		updates[i] = (struct spa_fgn_parameter_update) {
			.input_port = i + 2u,
			.buffer = &inputs[i + 2u].buffer,
		};
	}
	check_result(spa_fgn_graph_set_parameters(graph, updates, PARAMETER_COUNT),
			"prepare Classic startup parameter transaction");
	check_result(spa_fgn_graph_activate(graph), "activate Classic graph");

	raw_frames = read_file(argv[2], "raw-frames.u16le", &size);
	if (size != FRAME_COUNT * FRAME_PIXELS * sizeof(*raw_frames))
		fail("Classic raw-frame fixture has an unexpected extent");
	for (i = 0; i < 5; i++) {
		expected[i] = read_file(argv[2], expected_files[i], &size);
		if (size != FRAME_COUNT * output_elements[i] * sizeof(float))
			fail("Classic boundary fixture has an unexpected extent");
	}

	for (sequence = 1; sequence <= FRAME_COUNT; sequence++) {
		int process_result;

		prepare_frame(&inputs[0], &inputs[1], raw_frames, sequence);
		process_result = spa_fgn_graph_process(graph, process_inputs,
				GRAPH_INPUT_COUNT, process_outputs, GRAPH_OUTPUT_COUNT);
		check_result(process_result, "process Classic qualification frame");
		if (sequence == 1u &&
		    !(process_result & SPA_FGN_PROCESS_RESULT_PROPS_CHANGED))
			fail("Classic startup transaction was not published on the first frame");
		for (i = 0; i < GRAPH_OUTPUT_COUNT; i++)
			if (outputs[i].header.seq != (uint32_t)sequence)
				fail("Classic output sequence was not propagated");
		for (i = 0; i < 5; i++)
			compare_values(&comparisons[i], (const float *)outputs[i].payload,
				expected[i] + (sequence - 1u) * output_elements[i],
				output_elements[i], (uint32_t)sequence);
		carry_feedback(&inputs[1], &outputs[5]);
	}
	printf("numerical-equivalence: 7/7 frames; five public boundaries match\n");
	for (i = 0; i < 5; i++)
		printf("  %s: max-abs=%.9g max-scaled-rel=%.9g worst-frame=%u worst-index=%zu%s\n",
				comparisons[i].name, comparisons[i].maximum_absolute_error,
				comparisons[i].maximum_scaled_relative_error,
				comparisons[i].worst_frame, comparisons[i].worst_index,
				comparisons[i].exact ? " bitwise" : "");

	feedback_expected[0] = read_file(argv[2],
			"feedback-vdm-command.f32le", &feedback_size[0]);
	feedback_expected[1] = read_file(argv[2],
			"feedback-demanded-pdm-command.f32le", &feedback_size[1]);
	feedback_expected[2] = read_file(argv[2],
			"feedback-controller-constraint-feedback.f32le", &feedback_size[2]);
	if (feedback_size[0] % (CONTROLLED_VDM_SIZE * sizeof(float)) != 0)
		fail("feedback replay VDM fixture has an invalid extent");
	feedback_frames = feedback_size[0] /
		(CONTROLLED_VDM_SIZE * sizeof(float));
	if (feedback_frames == 0 ||
	    feedback_size[1] != feedback_frames * PDM_SIZE * sizeof(float) ||
	    feedback_size[2] != feedback_frames * CONTROLLED_VDM_SIZE * sizeof(float))
		fail("feedback replay fixtures have incompatible extents");
	sequence = FRAME_COUNT;
	for (feedback_frame = 0; feedback_frame < feedback_frames; feedback_frame++) {
		const float *expected_feedback = feedback_expected[2] +
			feedback_frame * CONTROLLED_VDM_SIZE;
		size_t index;

		sequence++;
		prepare_frame(&inputs[0], &inputs[1], raw_frames, sequence);
		check_result(spa_fgn_graph_process(graph, process_inputs,
				GRAPH_INPUT_COUNT, process_outputs, GRAPH_OUTPUT_COUNT),
				"process Classic feedback replay frame");
		compare_values(&comparisons[3], (const float *)outputs[3].payload,
			feedback_expected[0] + feedback_frame * CONTROLLED_VDM_SIZE,
			CONTROLLED_VDM_SIZE, (uint32_t)sequence);
		compare_values(&comparisons[4], (const float *)outputs[4].payload,
			feedback_expected[1] + feedback_frame * PDM_SIZE,
			PDM_SIZE, (uint32_t)sequence);
		compare_values(&(struct comparison) { .name = "controller feedback" },
			(const float *)outputs[5].payload, expected_feedback,
			CONTROLLED_VDM_SIZE, (uint32_t)sequence);
		for (index = 0; index < CONTROLLED_VDM_SIZE; index++)
			if (expected_feedback[index] != 0.0f) {
				expected_nonzero_feedback++;
				break;
			}
		for (index = 0; index < CONTROLLED_VDM_SIZE; index++)
			if (((const float *)outputs[5].payload)[index] != 0.0f) {
				actual_nonzero_feedback++;
				break;
			}
		carry_feedback(&inputs[1], &outputs[5]);
	}
	printf("delayed-feedback-equivalence: frames=%zu expected-nonzero=%u actual-nonzero=%u\n",
			feedback_frames, expected_nonzero_feedback, actual_nonzero_feedback);
	if (expected_nonzero_feedback == 0 ||
	    actual_nonzero_feedback != expected_nonzero_feedback)
		fail("feedback replay did not exercise equivalent nonzero constraint feedback");

	check_result(spa_fgn_graph_reset(graph), "reset Classic graph before benchmark");
	memset(inputs[1].payload, 0, inputs[1].size);
	memset(&inputs[1].header, 0, sizeof(inputs[1].header));
	sequence = 0;
	for (i = 0; i < warmup; i++) {
		sequence++;
		prepare_frame(&inputs[0], &inputs[1], raw_frames, sequence);
		check_result(spa_fgn_graph_process(graph, process_inputs,
				GRAPH_INPUT_COUNT, process_outputs, GRAPH_OUTPUT_COUNT),
				"warm Classic graph");
		carry_feedback(&inputs[1], &outputs[5]);
	}
	times = calloc(samples, sizeof(*times));
	if (times == NULL)
		fail("timing sample allocation failed");
	for (i = 0; i < samples; i++) {
		uint64_t start;

		sequence++;
		prepare_frame(&inputs[0], &inputs[1], raw_frames, sequence);
		start = monotonic_ns();
		check_result(spa_fgn_graph_process(graph, process_inputs,
				GRAPH_INPUT_COUNT, process_outputs, GRAPH_OUTPUT_COUNT),
				"benchmark Classic graph");
		times[i] = monotonic_ns() - start;
		carry_feedback(&inputs[1], &outputs[5]);
	}
	write_csv(csv, times, samples);
	report("nine-node REVOLT Classic FGN graph", times, samples);
	printf("benchmark-scope: direct graph callback only; warmup=%u samples=%u; PipeWire scheduling and device I/O excluded\n",
			warmup, samples);

	check_result(spa_fgn_graph_deactivate(graph), "deactivate Classic graph");
	spa_fgn_graph_free(graph);
	for (i = 0; i < GRAPH_INPUT_COUNT; i++)
		clear_buffer(&inputs[i]);
	for (i = 0; i < GRAPH_OUTPUT_COUNT; i++)
		clear_buffer(&outputs[i]);
	for (i = 0; i < 5; i++)
		free(expected[i]);
	for (i = 0; i < 3; i++)
		free(feedback_expected[i]);
	free(raw_frames);
	free(times);
	return EXIT_SUCCESS;
}
