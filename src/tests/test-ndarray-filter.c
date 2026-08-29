/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <spa/param/ndarray.h>

#include <pipewire/ndarray-filter.h>

static int process(void *data SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *inputs SPA_UNUSED,
		uint32_t n_inputs SPA_UNUSED,
		struct pw_ndarray_filter_buffer *outputs SPA_UNUSED,
		uint32_t n_outputs SPA_UNUSED)
{
	return 0;
}

static const struct pw_ndarray_filter_events events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.process = process,
};

static const uint32_t shape[] = { 277 };

static struct pw_ndarray_filter_port ports[] = {
	{
		.struct_size = sizeof(struct pw_ndarray_filter_port),
		.direction = SPA_DIRECTION_INPUT,
		.name = "input",
		.format = {
			.element_type = SPA_ELEMENT_TYPE_F32_LE,
			.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
			.rate_num = 1000,
			.rate_denom = 1,
			.n_dimensions = SPA_N_ELEMENTS(shape),
			.shape = shape,
			.schema = "org.calculon.test.input/1",
			.profile = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		},
	},
	{
		.struct_size = sizeof(struct pw_ndarray_filter_port),
		.direction = SPA_DIRECTION_OUTPUT,
		.name = "output",
		.format = {
			.element_type = SPA_ELEMENT_TYPE_F32_LE,
			.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
			.rate_num = 1000,
			.rate_denom = 1,
			.n_dimensions = SPA_N_ELEMENTS(shape),
			.shape = shape,
			.schema = "org.calculon.test.output/1",
			.profile = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
		},
	},
};

static struct pw_ndarray_filter_config config = {
	.struct_size = sizeof(struct pw_ndarray_filter_config),
	.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
	.node_name = "test.ndarray.filter",
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.events = &events,
};

static void expect_new_error(int expected)
{
	struct pw_ndarray_filter *filter = (void *)(uintptr_t)1;
	int res = pw_ndarray_filter_new(&config, &filter);

	spa_assert_se(res == expected);
	spa_assert_se(filter == NULL);
}

static void test_valid_config(void)
{
	struct pw_ndarray_filter *filter = NULL;
	uint32_t saved_flags = config.flags;
	int res;

	res = pw_ndarray_filter_new(&config, &filter);
	spa_assert_se(res == 0);
	spa_assert_se(filter != NULL);
	spa_assert_se(pw_ndarray_filter_get_state(filter) ==
			PW_FILTER_STATE_UNCONNECTED);
	spa_assert_se(pw_ndarray_filter_get_error(filter) == 0);
	spa_assert_se(pw_ndarray_filter_get_node_id(filter) == SPA_ID_INVALID);
	spa_assert_se(pw_ndarray_filter_run(filter) == -EINVAL);
	pw_ndarray_filter_destroy(filter);
	pw_ndarray_filter_destroy(NULL);

	config.flags = PW_NDARRAY_FILTER_FLAG_RT_PROCESS;
	filter = NULL;
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == 0);
	spa_assert_se(filter != NULL);
	pw_ndarray_filter_destroy(filter);
	config.flags = saved_flags;
}

static void test_config_validation(void)
{
	struct pw_ndarray_filter_config saved_config = config;
	struct pw_ndarray_filter_port saved_port = ports[0];
	struct pw_ndarray_filter_events missing_process = events;
	uint32_t invalid_shape[] = { 0 };
	char long_name[PW_NDARRAY_FILTER_NAME_MAX + 2];

	spa_assert_se(pw_ndarray_filter_new(&config, NULL) == -EINVAL);

	config.struct_size = 0;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.version++;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.flags = (PW_NDARRAY_FILTER_FLAG_RT_PROCESS << 1);
	expect_new_error(-EINVAL);
	config = saved_config;
	config.node_name = "";
	expect_new_error(-EINVAL);
	config = saved_config;
	memset(long_name, 'n', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	config.node_name = long_name;
	expect_new_error(-ENAMETOOLONG);
	config = saved_config;
	config.remote_name = "";
	expect_new_error(-EINVAL);
	config = saved_config;
	config.n_ports = 0;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.n_ports = PW_NDARRAY_FILTER_MAX_PORTS + 1;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.ports = NULL;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.events = NULL;
	expect_new_error(-EINVAL);
	config = saved_config;
	missing_process.process = NULL;
	config.events = &missing_process;
	expect_new_error(-EINVAL);
	config = saved_config;

	ports[0].struct_size = 0;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].flags = 1;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].reserved = 1;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].direction = 2;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].name = "";
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.n_dimensions = 0;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.n_dimensions = SPA_NDARRAY_MAX_DIMENSIONS + 1;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.shape = NULL;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.shape = invalid_shape;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.element_type = SPA_ELEMENT_TYPE_UNKNOWN;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.layout = SPA_NDARRAY_LAYOUT_UNKNOWN;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.rate_denom = 0;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.schema = "";
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].format.profile = "";
	expect_new_error(-EINVAL);
	ports[0] = saved_port;

	ports[1].direction = SPA_DIRECTION_INPUT;
	ports[1].name = ports[0].name;
	expect_new_error(-EEXIST);
	ports[1].direction = SPA_DIRECTION_OUTPUT;
	ports[1].name = "output";
	config = saved_config;
}

int main(void)
{
	test_valid_config();
	test_config_validation();
	return 0;
}
