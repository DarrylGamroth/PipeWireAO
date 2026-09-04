/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <spa/param/ndarray.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/filter.h>

#include <pipewire/ndarray-filter.h>

static int process(void *data SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *inputs SPA_UNUSED,
		uint32_t n_inputs SPA_UNUSED,
		struct pw_ndarray_filter_buffer *outputs SPA_UNUSED,
		uint32_t n_outputs SPA_UNUSED)
{
	return 0;
}

static int update_parameter(void *data SPA_UNUSED, uint32_t input_port SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *parameter SPA_UNUSED)
{
	return 0;
}

static const struct pw_ndarray_filter_events events = {
	PW_VERSION_NDARRAY_FILTER_EVENTS,
	.process = process,
	.update_parameter = update_parameter,
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

static struct spa_pod *build_format(struct spa_pod_builder *builder)
{
	struct spa_pod_frame object;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
			SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_COLUMN_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&SPA_FRACTION(1000, 1)),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.calculon.test.input/1"),
			0);
	return spa_pod_builder_pop(builder, &object);
}

static void test_negotiated_format_strings(void)
{
	uint8_t first_data[1024], second_data[1024], result_data[2048];
	struct spa_pod_builder first_builder =
		SPA_POD_BUILDER_INIT(first_data, sizeof(first_data));
	struct spa_pod_builder second_builder =
		SPA_POD_BUILDER_INIT(second_data, sizeof(second_data));
	struct spa_pod_builder result_builder =
		SPA_POD_BUILDER_INIT(result_data, sizeof(result_data));
	const struct spa_pod *first = build_format(&first_builder);
	const struct spa_pod *second = build_format(&second_builder);
	const struct spa_pod_prop *property;
	struct spa_pod *result = NULL;
	const char *value;

	spa_assert_se(first != NULL && second != NULL);
	spa_assert_se(spa_format_ndarray_parse_string(first,
			SPA_FORMAT_NDARRAY_schema, &value) == 0);
	spa_assert_se(spa_streq(value, "org.calculon.test.input/1"));
	spa_assert_se(spa_pod_filter(&result_builder, &result, first, second) == 0);
	spa_assert_se(result != NULL);

	property = spa_pod_find_prop(result, NULL, SPA_FORMAT_NDARRAY_schema);
	spa_assert_se(property != NULL && spa_pod_is_choice(&property->value));
	spa_assert_se(spa_format_ndarray_parse_string(result,
			SPA_FORMAT_NDARRAY_schema, &value) == 0);
	spa_assert_se(spa_streq(value, "org.calculon.test.input/1"));

	spa_assert_se(spa_format_ndarray_parse_string(result,
			SPA_FORMAT_NDARRAY_rate + 100u, &value) == 0);
	spa_assert_se(value == NULL);
}

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

	spa_assert_se(PW_NDARRAY_FILTER_BUFFER_FLAG_NONE == 0);
	spa_assert_se(PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE == (1u << 0));
	spa_assert_se(PW_NDARRAY_FILTER_BUFFER_FLAG_INPUT_UNAVAILABLE == (1u << 1));
	spa_assert_se(PW_NDARRAY_FILTER_PORT_FLAG_NONE == 0);
	spa_assert_se(PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER == (1u << 0));
	spa_assert_se(PW_VERSION_NDARRAY_FILTER_EVENTS == 1);
	spa_assert_se(PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS == (1u << 1));
	spa_assert_se(PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL == (1u << 2));

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
	config.flags = PW_NDARRAY_FILTER_FLAG_INDEPENDENT_INPUTS;
	filter = NULL;
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == 0);
	spa_assert_se(filter != NULL);
	pw_ndarray_filter_destroy(filter);
	config.flags = PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL;
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
	struct pw_ndarray_filter_events missing_parameter = events;
	uint32_t invalid_shape[] = { 0 };
	char long_name[PW_NDARRAY_FILTER_NAME_MAX + 2];

	spa_assert_se(pw_ndarray_filter_new(&config, NULL) == -EINVAL);

	config.struct_size = 0;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.version++;
	expect_new_error(-EINVAL);
	config = saved_config;
	config.flags = (PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL << 1);
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
	ports[0].flags = PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER << 1;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	ports[0].flags = PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER;
	ports[0].format.rate_num = 0;
	ports[0].format.rate_denom = 0;
	missing_parameter.update_parameter = NULL;
	config.events = &missing_parameter;
	expect_new_error(-EINVAL);
	config.events = &events;
	{
		struct pw_ndarray_filter *filter = NULL;

		spa_assert_se(pw_ndarray_filter_new(&config, &filter) == 0);
		pw_ndarray_filter_destroy(filter);
	}
	ports[0].direction = SPA_DIRECTION_OUTPUT;
	expect_new_error(-EINVAL);
	ports[0] = saved_port;
	config = saved_config;
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

	ports[1].direction = SPA_DIRECTION_INPUT;
	ports[1].name = ports[0].name;
	expect_new_error(-EEXIST);
	ports[1].direction = SPA_DIRECTION_OUTPUT;
	ports[1].name = "output";
	config = saved_config;
}

int main(void)
{
	test_negotiated_format_strings();
	test_valid_config();
	test_config_validation();
	return 0;
}
