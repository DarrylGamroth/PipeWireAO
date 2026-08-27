/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Direct-call service-time comparison for the existing fused Calculon SPA
 * controller and either the four-node direct or eight-node decomposed-DM
 * Filter Graph Ndarray controller. Shack-Hartmann subapertures remain
 * detector-image views inside the first Calculon plan, rather than becoming a
 * materialized graph edge.
 * This deliberately excludes PipeWire scheduling and device I/O.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

#include <spa/filter-graph/filter-graph-ndarray.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/support/plugin.h>

#define DEFAULT_SAMPLES 10000u
#define DEFAULT_WARMUP 1000u
#define DEFAULT_WIDTH 352u
#define DEFAULT_HEIGHT 352u
#define DEFAULT_REGION_WIDTH 22u
#define DEFAULT_REGION_HEIGHT 22u
#define DEFAULT_ACTUATORS 277u
#define MAX_EXTENT 4096u

#define FUSED_FACTORY "api.calculon.shwfs-controller"
#define KEY_SIZE "api.calculon.detector-size"
#define KEY_RATE "api.calculon.detector-rate"
#define KEY_PROFILE "api.calculon.detector-profile"
#define KEY_REGION_SIZE "api.calculon.region-size"
#define KEY_REGION_ORIGINS "api.calculon.region-origins"
#define KEY_ACTUATOR_COUNT "api.calculon.actuator-count"
#define KEY_MATRIX_PATH "api.calculon.reconstruction-matrix-path"
#define KEY_COORDINATE_SCALE "api.calculon.coordinate-scale"
#define KEY_PIXEL_THRESHOLD "api.calculon.pixel-threshold"
#define KEY_FLUX_THRESHOLD "api.calculon.flux-threshold"
#define KEY_CONTROLLER_GAIN "api.calculon.controller-gain"
#define KEY_CONTROLLER_POLE "api.calculon.controller-pole"
#define KEY_COMMAND_MINIMUM "api.calculon.command-minimum"
#define KEY_COMMAND_MAXIMUM "api.calculon.command-maximum"

static const char profile[] =
	"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct loaded_plugin {
	void *library;
	spa_handle_factory_enum_func_t enumerate;
};

struct fused_controller {
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_io_buffers input_io;
	struct spa_io_buffers output_io;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
	uint8_t *payload;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC_RAW, &value) == 0);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

static uint32_t parse_u32(const char *text, uint32_t maximum)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	spa_assert_se(errno == 0 && end != text && *end == '\0' && value > 0 &&
			value <= maximum);
	return (uint32_t)value;
}

static struct loaded_plugin load_plugin(const char *path)
{
	struct loaded_plugin plugin = { 0 };
	void *symbol;

	plugin.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (plugin.library == NULL) {
		fprintf(stderr, "cannot load fused plugin %s: %s\n", path, dlerror());
		exit(EXIT_FAILURE);
	}
	symbol = dlsym(plugin.library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	memcpy(&plugin.enumerate, &symbol, sizeof(plugin.enumerate));
	return plugin;
}

static const struct spa_handle_factory *find_factory(
		const struct loaded_plugin *plugin, const char *name)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;

	while (plugin->enumerate(&factory, &index) == 1)
		if (spa_streq(factory->name, name))
			return factory;
	return NULL;
}

static void init_buffer(struct test_buffer *storage, size_t bytes,
		int32_t stride, bool input)
{
	spa_assert_se(bytes > 0 && bytes <= UINT32_MAX);
	memset(storage, 0, sizeof(*storage));
	storage->payload = calloc(1, bytes);
	spa_assert_se(storage->payload != NULL);
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = (uint32_t)bytes;
	storage->data.data = storage->payload;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = input ? (uint32_t)bytes : 0;
	storage->chunk.stride = stride;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

struct format_capture {
	uint8_t storage[4096];
	struct spa_pod *format;
};

static void capture_result(void *data, int seq, int result, uint32_t type,
		const void *value)
{
	struct format_capture *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	(void)seq;
	spa_assert_se(result >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = value;
	if (params->id != SPA_PARAM_EnumFormat || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->format = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = capture_result,
};

static void configure_fused(struct fused_controller *controller,
		const struct spa_handle_factory *factory, const struct spa_dict *info,
		struct test_buffer *input, struct test_buffer *output)
{
	struct format_capture capture = { 0 };
	struct spa_hook listener;
	struct spa_buffer *buffers[1];
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	size_t size = spa_handle_factory_get_size(factory, info);

	memset(controller, 0, sizeof(*controller));
	controller->handle = calloc(1, size);
	spa_assert_se(controller->handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, controller->handle, info,
			NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(controller->handle,
			SPA_TYPE_INTERFACE_Node, (void **)&controller->node) == 0);
	spa_assert_se(spa_node_add_listener(controller->node, &listener,
			&node_events, &capture) == 0);
	spa_assert_se(spa_node_port_enum_params(controller->node, 1,
			SPA_DIRECTION_INPUT, 0, SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_assert_se(capture.format != NULL);
	spa_assert_se(spa_node_port_set_param(controller->node,
			SPA_DIRECTION_INPUT, 0, SPA_PARAM_Format, 0,
			capture.format) == 0);
	capture.format = NULL;
	spa_assert_se(spa_node_port_enum_params(controller->node, 2,
			SPA_DIRECTION_OUTPUT, 0, SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_assert_se(capture.format != NULL);
	spa_assert_se(spa_node_port_set_param(controller->node,
			SPA_DIRECTION_OUTPUT, 0, SPA_PARAM_Format, 0,
			capture.format) == 0);
	spa_hook_remove(&listener);
	buffers[0] = &input->buffer;
	spa_assert_se(spa_node_port_use_buffers(controller->node,
			SPA_DIRECTION_INPUT, 0, 0, buffers, 1) == 0);
	buffers[0] = &output->buffer;
	spa_assert_se(spa_node_port_use_buffers(controller->node,
			SPA_DIRECTION_OUTPUT, 0, 0, buffers, 1) == 0);
	controller->input_io.status = SPA_STATUS_NEED_DATA;
	controller->input_io.buffer_id = SPA_ID_INVALID;
	controller->output_io.status = SPA_STATUS_NEED_DATA;
	controller->output_io.buffer_id = SPA_ID_INVALID;
	spa_assert_se(spa_node_port_set_io(controller->node, SPA_DIRECTION_INPUT,
			0, SPA_IO_Buffers, &controller->input_io,
			sizeof(controller->input_io)) == 0);
	spa_assert_se(spa_node_port_set_io(controller->node, SPA_DIRECTION_OUTPUT,
			0, SPA_IO_Buffers, &controller->output_io,
			sizeof(controller->output_io)) == 0);
	spa_assert_se(spa_node_send_command(controller->node, &start) == 0);
}

static void process_fused(struct fused_controller *controller)
{
	controller->input_io.status = SPA_STATUS_HAVE_DATA;
	controller->input_io.buffer_id = 0;
	controller->output_io.status = SPA_STATUS_NEED_DATA;
	controller->output_io.buffer_id = 0;
	spa_assert_se(spa_node_process(controller->node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(controller->input_io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(controller->output_io.status == SPA_STATUS_HAVE_DATA);
}

static void clear_fused(struct fused_controller *controller)
{
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);

	spa_assert_se(spa_node_send_command(controller->node, &pause) == 0);
	spa_assert_se(spa_handle_clear(controller->handle) == 0);
	free(controller->handle);
}

static char *make_size_text(uint32_t width, uint32_t height)
{
	char *text = malloc(32);

	spa_assert_se(text != NULL);
	spa_assert_se(snprintf(text, 32, "%ux%u", width, height) > 0);
	return text;
}

static char *make_origins(uint32_t width, uint32_t height,
		uint32_t region_width, uint32_t region_height, bool json,
		uint32_t *region_count)
{
	uint32_t rows = height / region_height;
	uint32_t columns = width / region_width;
	size_t count = (size_t)rows * columns;
	size_t capacity = count * 40u + 3u;
	char *text = malloc(capacity);
	size_t offset = 0;
	uint32_t row, column;

	spa_assert_se(width % region_width == 0 && height % region_height == 0 &&
			count > 0 && count <= UINT32_MAX / 2u);
	spa_assert_se(text != NULL);
	if (json)
		text[offset++] = '[';
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			int written = snprintf(text + offset, capacity - offset,
					json ? "%s[%u,%u]" : "%s%u,%u",
					(row == 0 && column == 0) ? "" : (json ? "," : ";"),
					row * region_height, column * region_width);

			spa_assert_se(written > 0 && (size_t)written < capacity - offset);
			offset += (size_t)written;
		}
	}
	if (json)
		text[offset++] = ']';
	text[offset] = '\0';
	*region_count = (uint32_t)count;
	return text;
}

static float *write_matrix(char path[static 32], uint32_t rows, uint32_t columns)
{
	float *matrix;
	size_t elements = (size_t)rows * columns;
	size_t bytes = elements * sizeof(*matrix), offset = 0;
	int fd;

	matrix = calloc(elements, sizeof(*matrix));
	spa_assert_se(matrix != NULL);
	for (uint32_t row = 0; row < rows; row++)
		matrix[(size_t)row * columns + row % columns] = 0.01f;
	memcpy(path, "/tmp/calculon-fgn-matrix-XXXXXX",
			sizeof("/tmp/calculon-fgn-matrix-XXXXXX"));
	fd = mkstemp(path);
	spa_assert_se(fd >= 0);
	while (offset < bytes) {
		ssize_t written = write(fd, (uint8_t *)matrix + offset, bytes - offset);

		if (written < 0 && errno == EINTR)
			continue;
		spa_assert_se(written > 0);
		offset += (size_t)written;
	}
	spa_assert_se(close(fd) == 0);
	return matrix;
}

static char *make_graph_config(const char *plugin, const char *origins,
		uint32_t width, uint32_t height,
		uint32_t region_width, uint32_t region_height,
		uint32_t region_count, uint32_t actuators, bool image_views,
		bool decomposed_dm)
{
	const char *measurement_link, *measurement_input;
	size_t measurement_capacity = strlen(plugin) * 2u + strlen(origins) +
			4096u;
	char *measurement = malloc(measurement_capacity);
	size_t capacity;
	char *config;
	int written;

	spa_assert_se(measurement != NULL && strchr(plugin, '"') == NULL);
	if (image_views) {
		written = snprintf(measurement, measurement_capacity,
		"{\"type\":\"ndarray\",\"name\":\"measure\",\"plugin\":\"%s\","
		"\"label\":\"shack-hartmann-image-f32\",\"config\":{"
		"\"image_rows\":%u,\"image_columns\":%u,"
		"\"subaperture_rows\":%u,\"subaperture_columns\":%u,"
		"\"subaperture_count\":%u,\"coordinate_scale\":1.0,"
		"\"pixel_threshold\":0.0,\"flux_threshold\":1.0,"
		"\"rate\":[1000,1]}}",
		plugin, height, width, region_height, region_width, region_count);
		measurement_link =
			"{\"output\":\"measure:slopes\",\"input\":\"reconstruct:slopes\"}";
		measurement_input = "\"measure:image\"";
	} else {
		written = snprintf(measurement, measurement_capacity,
		"{\"type\":\"ndarray\",\"name\":\"regions\",\"plugin\":\"%s\","
		"\"label\":\"region-extraction-f32\",\"config\":{"
		"\"image_rows\":%u,\"image_columns\":%u,\"region_rows\":%u,"
		"\"region_columns\":%u,\"origins\":%s,\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"measure\",\"plugin\":\"%s\","
		"\"label\":\"shack-hartmann-f32\",\"config\":{"
		"\"region_rows\":%u,\"region_columns\":%u,"
		"\"subaperture_count\":%u,\"coordinate_scale\":1.0,"
		"\"pixel_threshold\":0.0,\"flux_threshold\":1.0,"
		"\"rate\":[1000,1]}}",
		plugin, height, width, region_height, region_width, origins,
		plugin, region_height, region_width, region_count);
		measurement_link =
			"{\"output\":\"regions:regions\",\"input\":\"measure:regions\"},"
			"{\"output\":\"measure:slopes\",\"input\":\"reconstruct:slopes\"}";
		measurement_input = "\"regions:image\"";
	}
	spa_assert_se(written > 0 && (size_t)written < measurement_capacity);

	capacity = strlen(measurement) + strlen(plugin) * (decomposed_dm ? 7u : 3u) +
			strlen(measurement_link) + (decomposed_dm ? 8192u : 4096u);
	config = malloc(capacity);
	spa_assert_se(config != NULL);
	if (decomposed_dm) {
		written = snprintf(config, capacity,
		"{\"nodes\":[%s,"
		"{\"type\":\"ndarray\",\"name\":\"reconstruct\",\"plugin\":\"%s\","
		"\"label\":\"shwfs-reconstructor-f32\",\"config\":{"
		"\"actuator_count\":%u,\"subaperture_count\":%u,"
		"\"reconstructed_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"integrate\",\"plugin\":\"%s\","
		"\"label\":\"leaky-integrator-f32\",\"config\":{"
		"\"initial_state\":0.0,\"extent\":%u,"
		"\"input_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"output_schema\":\"org.calculon.ao.controller-command/1\","
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"controller-to-vdm\","
		"\"plugin\":\"%s\",\"label\":\"controller-to-vdm-f32\",\"config\":{"
		"\"controller_command_extent\":%u,\"vdm_size\":%u,"
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"vdm-to-pdm\",\"plugin\":\"%s\","
		"\"label\":\"vdm-to-pdm-f32\",\"config\":{"
		"\"active_vdm_size\":%u,\"full_vdm_size\":%u,"
		"\"physical_actuator_extent\":%u,"
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"command\",\"plugin\":\"%s\","
		"\"label\":\"pdm-command-f32\",\"config\":{"
		"\"actuator_count\":%u,\"lower_limit\":-1.0,\"upper_limit\":1.0,"
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"pdm-feedback-to-vdm\","
		"\"plugin\":\"%s\",\"label\":\"pdm-feedback-to-vdm-f32\",\"config\":{"
		"\"active_vdm_size\":%u,\"full_vdm_size\":%u,"
		"\"physical_actuator_extent\":%u,"
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"vdm-feedback-to-controller\","
		"\"plugin\":\"%s\",\"label\":\"vdm-feedback-to-controller-f32\","
		"\"config\":{\"controller_command_extent\":%u,\"vdm_size\":%u,"
		"\"rate\":[1000,1]}}],"
		"\"links\":[%s,"
		"{\"output\":\"reconstruct:reconstructed\",\"input\":\"integrate:input\"},"
		"{\"output\":\"integrate:output\",\"input\":\"controller-to-vdm:controller-command\"},"
		"{\"output\":\"controller-to-vdm:vdm-command\",\"input\":\"vdm-to-pdm:vdm-command\"},"
		"{\"output\":\"vdm-to-pdm:requested-pdm-command\",\"input\":\"command:requested\"},"
		"{\"output\":\"command:constraint-feedback\","
		"\"input\":\"pdm-feedback-to-vdm:pdm-constraint-feedback\"},"
		"{\"output\":\"pdm-feedback-to-vdm:vdm-constraint-feedback\","
		"\"input\":\"vdm-feedback-to-controller:vdm-constraint-feedback\"}],"
		"\"inputs\":[%s,\"reconstruct:reconstructor\","
		"\"controller-to-vdm:controller-to-vdm\","
		"\"vdm-to-pdm:active-to-full\",\"vdm-to-pdm:vdm-to-pdm\","
		"\"pdm-feedback-to-vdm:full-to-active\","
		"\"pdm-feedback-to-vdm:pdm-to-vdm\","
		"\"vdm-feedback-to-controller:vdm-to-controller\"],"
		"\"outputs\":[\"command:demanded\","
		"\"vdm-feedback-to-controller:controller-constraint-feedback\"]}",
		measurement,
		plugin, actuators, region_count,
		plugin, actuators,
		plugin, actuators, actuators,
		plugin, actuators, actuators, actuators,
		plugin, actuators,
		plugin, actuators, actuators, actuators,
		plugin, actuators, actuators,
		measurement_link, measurement_input);
	} else {
		written = snprintf(config, capacity,
		"{\"nodes\":[%s,"
		"{\"type\":\"ndarray\",\"name\":\"reconstruct\",\"plugin\":\"%s\","
		"\"label\":\"shwfs-reconstructor-f32\",\"config\":{"
		"\"actuator_count\":%u,\"subaperture_count\":%u,"
		"\"reconstructed_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"integrate\",\"plugin\":\"%s\","
		"\"label\":\"leaky-integrator-f32\",\"config\":{"
		"\"initial_state\":0.0,\"extent\":%u,"
		"\"input_schema\":\"org.calculon.ao.controller-residual-error/1\","
		"\"output_schema\":\"org.calculon.ao.requested-pdm-command/1\","
		"\"rate\":[1000,1]}},"
		"{\"type\":\"ndarray\",\"name\":\"command\",\"plugin\":\"%s\","
		"\"label\":\"pdm-command-f32\",\"config\":{"
		"\"actuator_count\":%u,\"lower_limit\":-1.0,\"upper_limit\":1.0,"
		"\"rate\":[1000,1]}}],"
		"\"links\":[%s,"
		"{\"output\":\"reconstruct:reconstructed\",\"input\":\"integrate:input\"},"
		"{\"output\":\"integrate:output\",\"input\":\"command:requested\"}],"
		"\"inputs\":[%s,\"reconstruct:reconstructor\"],"
		"\"outputs\":[\"command:demanded\"]}",
		measurement,
		plugin, actuators, region_count,
		plugin, actuators, plugin, actuators,
		measurement_link, measurement_input);
	}
	spa_assert_se(written > 0 && (size_t)written < capacity);
	free(measurement);
	return config;
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

static uint64_t report(const char *name, uint64_t *samples, size_t count)
{
	long double sum = 0.0;
	size_t p90, p99, p999;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (size_t i = 0; i < count; i++)
		sum += samples[i];
	p90 = percentile(count, 90, 100);
	p99 = percentile(count, 99, 100);
	p999 = percentile(count, 999, 1000);
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64 " ns p90=%" PRIu64
			" ns p99=%" PRIu64, name, count, sum / count,
			samples[count / 2], samples[p90], samples[p99]);
	if (count >= 1000)
		printf(" ns p99.9=%" PRIu64, samples[p999]);
	printf(" ns max=%" PRIu64 " ns\n", samples[count - 1]);
	return samples[count / 2];
}

static void write_csv(const char *path, const uint64_t *fused,
		const uint64_t *graph, size_t count)
{
	FILE *file;

	if (path == NULL)
		return;
	file = fopen(path, "we");
	spa_assert_se(file != NULL);
	spa_assert_se(fputs("iteration,fused_ns,graph_ns\n", file) >= 0);
	for (size_t i = 0; i < count; i++)
		spa_assert_se(fprintf(file, "%zu,%" PRIu64 ",%" PRIu64 "\n",
				i, fused[i], graph[i]) > 0);
	spa_assert_se(fclose(file) == 0);
}

int main(int argc, char *argv[])
{
	struct loaded_plugin fused_plugin;
	const struct spa_handle_factory *factory;
	struct fused_controller fused;
	struct spa_fgn_graph *graph = NULL;
	struct test_buffer fused_input, fused_output, graph_input, graph_output;
	struct test_buffer graph_parameter, graph_dm_parameter, graph_feedback;
	struct spa_buffer *graph_inputs[8] = { NULL }, *graph_outputs[2];
	struct spa_dict_item items[14];
	struct spa_dict info;
	char matrix_path[32], actuators_text[16];
	char *size_text, *region_size_text, *fused_origins, *json_origins, *config;
	uint32_t samples = DEFAULT_SAMPLES, warmup = DEFAULT_WARMUP;
	uint32_t width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
	uint32_t region_width = DEFAULT_REGION_WIDTH;
	uint32_t region_height = DEFAULT_REGION_HEIGHT;
	uint32_t actuators = DEFAULT_ACTUATORS, fused_regions, json_regions;
	uint64_t *fused_times, *graph_times, fused_median, graph_median;
	float *matrix_values;
	size_t image_bytes, output_bytes, matrix_bytes, dm_matrix_bytes = 0;
	const char *mode = getenv("PW_FGN_BENCHMARK_MODE");
	const char *sensor_mode = getenv("PW_FGN_BENCHMARK_SENSOR");
	const char *dm_mode = getenv("PW_FGN_BENCHMARK_DM");
	bool run_fused, run_graph, image_views, decomposed_dm;

	spa_assert_se(argc == 3 || argc == 4 || argc == 9);
	if (argc >= 4)
		samples = parse_u32(argv[3], UINT32_MAX);
	if (argc == 9) {
		actuators = parse_u32(argv[4], UINT32_MAX / sizeof(float));
		width = parse_u32(argv[5], MAX_EXTENT);
		height = parse_u32(argv[6], MAX_EXTENT);
		region_width = parse_u32(argv[7], width);
		region_height = parse_u32(argv[8], height);
	}
	if (getenv("PW_FGN_BENCHMARK_WARMUP") != NULL)
		warmup = parse_u32(getenv("PW_FGN_BENCHMARK_WARMUP"), UINT32_MAX);
	if (mode == NULL)
		mode = "both";
	if (sensor_mode == NULL)
		sensor_mode = "view";
	if (dm_mode == NULL)
		dm_mode = "direct";
	run_fused = spa_streq(mode, "both") || spa_streq(mode, "fused");
	run_graph = spa_streq(mode, "both") || spa_streq(mode, "graph");
	image_views = spa_streq(sensor_mode, "view");
	decomposed_dm = spa_streq(dm_mode, "decomposed");
	spa_assert_se(run_fused || run_graph);
	spa_assert_se(image_views || spa_streq(sensor_mode, "materialized"));
	spa_assert_se(decomposed_dm || spa_streq(dm_mode, "direct"));
	spa_assert_se(width % region_width == 0 && height % region_height == 0);
	image_bytes = (size_t)width * height * sizeof(float);
	output_bytes = (size_t)actuators * sizeof(float);
	spa_assert_se(image_bytes <= UINT32_MAX && output_bytes <= UINT32_MAX);
	size_text = make_size_text(width, height);
	region_size_text = make_size_text(region_width, region_height);
	fused_origins = make_origins(width, height, region_width, region_height,
			false, &fused_regions);
	json_origins = make_origins(width, height, region_width, region_height,
			true, &json_regions);
	spa_assert_se(fused_regions == json_regions && fused_regions <= UINT32_MAX / 2u);
	matrix_bytes = (size_t)actuators * fused_regions * 2u * sizeof(float);
	spa_assert_se(matrix_bytes <= UINT32_MAX);
	matrix_values = write_matrix(matrix_path, actuators, fused_regions * 2u);
	spa_assert_se(snprintf(actuators_text, sizeof(actuators_text), "%u",
			actuators) > 0);
	items[0] = SPA_DICT_ITEM_INIT(KEY_SIZE, size_text);
	items[1] = SPA_DICT_ITEM_INIT(KEY_RATE, "1000/1");
	items[2] = SPA_DICT_ITEM_INIT(KEY_PROFILE, profile);
	items[3] = SPA_DICT_ITEM_INIT(KEY_REGION_SIZE, region_size_text);
	items[4] = SPA_DICT_ITEM_INIT(KEY_REGION_ORIGINS, fused_origins);
	items[5] = SPA_DICT_ITEM_INIT(KEY_ACTUATOR_COUNT, actuators_text);
	items[6] = SPA_DICT_ITEM_INIT(KEY_MATRIX_PATH, matrix_path);
	items[7] = SPA_DICT_ITEM_INIT(KEY_COORDINATE_SCALE, "1");
	items[8] = SPA_DICT_ITEM_INIT(KEY_PIXEL_THRESHOLD, "0");
	items[9] = SPA_DICT_ITEM_INIT(KEY_FLUX_THRESHOLD, "1");
	items[10] = SPA_DICT_ITEM_INIT(KEY_CONTROLLER_GAIN, "1");
	items[11] = SPA_DICT_ITEM_INIT(KEY_CONTROLLER_POLE, "0");
	items[12] = SPA_DICT_ITEM_INIT(KEY_COMMAND_MINIMUM, "-1");
	items[13] = SPA_DICT_ITEM_INIT(KEY_COMMAND_MAXIMUM, "1");
	info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	init_buffer(&fused_input, image_bytes, (int32_t)(width * sizeof(float)), true);
	init_buffer(&fused_output, output_bytes, sizeof(float), false);
	init_buffer(&graph_input, image_bytes, (int32_t)(width * sizeof(float)), true);
	init_buffer(&graph_output, output_bytes, sizeof(float), false);
	init_buffer(&graph_parameter, matrix_bytes,
			(int32_t)(fused_regions * 2u * sizeof(float)), true);
	memcpy(graph_parameter.payload, matrix_values, matrix_bytes);
	graph_parameter.header.seq = 1;
	if (decomposed_dm) {
		dm_matrix_bytes = (size_t)actuators * actuators * sizeof(float);
		spa_assert_se(dm_matrix_bytes <= UINT32_MAX);
		init_buffer(&graph_dm_parameter, dm_matrix_bytes,
				(int32_t)(actuators * sizeof(float)), true);
		init_buffer(&graph_feedback, output_bytes, sizeof(float), false);
		for (uint32_t actuator = 0; actuator < actuators; actuator++)
			((float *)graph_dm_parameter.payload)[
					(size_t)actuator * actuators + actuator] = 1.0f;
	}
	free(matrix_values);
	for (size_t i = 0; i < image_bytes / sizeof(float); i++) {
		float value = 1.0f + (float)(i % region_width) +
				2.0f * (float)((i / width) % region_height);
		((float *)fused_input.payload)[i] = value;
		((float *)graph_input.payload)[i] = value;
	}
	fused_plugin = load_plugin(argv[1]);
	factory = find_factory(&fused_plugin, FUSED_FACTORY);
	spa_assert_se(factory != NULL);
	configure_fused(&fused, factory, &info, &fused_input, &fused_output);
	config = make_graph_config(argv[2], json_origins, width, height,
			region_width, region_height, fused_regions, actuators, image_views,
			decomposed_dm);
	spa_assert_se(spa_fgn_graph_new(config, &graph) == 0);
	spa_assert_se(spa_fgn_graph_get_n_inputs(graph) ==
			(decomposed_dm ? 8u : 2u) &&
			spa_fgn_graph_get_n_outputs(graph) == (decomposed_dm ? 2u : 1u));
	spa_assert_se(spa_fgn_graph_activate(graph) == 0);
	spa_assert_se(unlink(matrix_path) == 0);
	graph_inputs[0] = &graph_input.buffer;
	graph_outputs[0] = &graph_output.buffer;
	if (decomposed_dm)
		graph_outputs[1] = &graph_feedback.buffer;
	spa_assert_se(spa_fgn_graph_update_parameter(graph, 1,
			&graph_parameter.buffer) == 0);
	spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
			decomposed_dm ? 8u : 2u, graph_outputs,
			decomposed_dm ? 2u : 1u) >= 0);
	if (decomposed_dm) {
		for (uint32_t port = 2; port < 8; port++) {
			graph_dm_parameter.header.seq = port;
			spa_assert_se(spa_fgn_graph_update_parameter(graph, port,
					&graph_dm_parameter.buffer) == 0);
			spa_assert_se(spa_fgn_graph_process(graph, graph_inputs, 8,
					graph_outputs, 2) >= 0);
		}
	}
	printf("workload: detector=%ux%u region=%ux%u regions=%u slopes=%u "
			"actuators=%u matrix-bytes=%zu warmup=%u samples=%u\n",
			width, height, region_width, region_height, fused_regions,
			fused_regions * 2u, actuators,
			(size_t)actuators * fused_regions * 2u * sizeof(float),
			warmup, samples);
	printf("measurement-mode: %s\n", mode);
	printf("sensor-boundary: %s\n", sensor_mode);
	printf("dm-boundary: %s\n", dm_mode);
	/* One untimed call per implementation proves numerical equivalence. */
	fused_input.header.seq++;
	graph_input.header.seq++;
	process_fused(&fused);
	spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
			decomposed_dm ? 8u : 2u, graph_outputs,
			decomposed_dm ? 2u : 1u) >= 0);
	for (uint32_t i = 0; i < actuators; i++)
		spa_assert_se(((float *)fused_output.payload)[i] ==
				((float *)graph_output.payload)[i]);
	if (decomposed_dm)
		for (uint32_t i = 0; i < actuators; i++)
			spa_assert_se(((float *)graph_feedback.payload)[i] == 0.0f);
	/* A single-mode profile must not spend its warmup in the other runtime. */
	for (uint32_t i = 0; i < warmup; i++) {
		fused_input.header.seq++;
		graph_input.header.seq++;
		if (run_fused)
			process_fused(&fused);
		if (run_graph)
			spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
					decomposed_dm ? 8u : 2u, graph_outputs,
					decomposed_dm ? 2u : 1u) >= 0);
	}
	fused_times = calloc(samples, sizeof(*fused_times));
	graph_times = calloc(samples, sizeof(*graph_times));
	spa_assert_se(fused_times != NULL && graph_times != NULL);
	for (uint32_t i = 0; i < samples; i++) {
		uint64_t start;

		fused_input.header.seq++;
		graph_input.header.seq++;
		if (run_fused && run_graph && (i & 1u) == 0) {
			start = monotonic_ns();
			process_fused(&fused);
			fused_times[i] = monotonic_ns() - start;
			start = monotonic_ns();
			spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
					decomposed_dm ? 8u : 2u, graph_outputs,
					decomposed_dm ? 2u : 1u) >= 0);
			graph_times[i] = monotonic_ns() - start;
		} else if (run_fused && run_graph) {
			start = monotonic_ns();
			spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
					decomposed_dm ? 8u : 2u, graph_outputs,
					decomposed_dm ? 2u : 1u) >= 0);
			graph_times[i] = monotonic_ns() - start;
			start = monotonic_ns();
			process_fused(&fused);
			fused_times[i] = monotonic_ns() - start;
		} else if (run_fused) {
			start = monotonic_ns();
			process_fused(&fused);
			fused_times[i] = monotonic_ns() - start;
		} else {
			start = monotonic_ns();
			spa_assert_se(spa_fgn_graph_process(graph, graph_inputs,
					decomposed_dm ? 8u : 2u, graph_outputs,
					decomposed_dm ? 2u : 1u) >= 0);
			graph_times[i] = monotonic_ns() - start;
		}
	}
	write_csv(getenv("PW_FGN_BENCHMARK_CSV"), fused_times, graph_times, samples);
	if (run_fused)
		fused_median = report("fused SPA controller", fused_times, samples);
	if (run_graph)
		graph_median = report(decomposed_dm ?
				(image_views ? "eight-node decomposed-DM FGN controller" :
				 "nine-node decomposed-DM FGN controller") :
				(image_views ? "four-node FGN controller" :
				 "five-node FGN controller"), graph_times, samples);
	if (run_fused && run_graph)
		printf("median ratio: %.4f graph/fused (%+.2f%%)\n",
				(double)graph_median / (double)fused_median,
				100.0 * ((double)graph_median /
				(double)fused_median - 1.0));
	spa_assert_se(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	clear_fused(&fused);
	spa_assert_se(dlclose(fused_plugin.library) == 0);
	free(graph_times);
	free(fused_times);
	free(config);
	free(json_origins);
	free(fused_origins);
	free(region_size_text);
	free(size_text);
	free(graph_output.payload);
	free(graph_input.payload);
	free(graph_parameter.payload);
	if (decomposed_dm) {
		free(graph_feedback.payload);
		free(graph_dm_parameter.payload);
	}
	free(fused_output.payload);
	free(fused_input.payload);
	return 0;
}
