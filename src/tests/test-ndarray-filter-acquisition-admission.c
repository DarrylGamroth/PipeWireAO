/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/ndarray-filter.h>

static int process(void *data SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *in SPA_UNUSED,
		uint32_t ni SPA_UNUSED, struct pw_ndarray_filter_buffer *out SPA_UNUSED,
		uint32_t no SPA_UNUSED)
{
	return 0;
}

int main(int argc, char *argv[])
{
	const uint32_t shape[] = { 1 };
	const struct pw_ndarray_filter_events events = {
		PW_VERSION_NDARRAY_FILTER_EVENTS, .process = process,
	};
	const struct pw_ndarray_filter_port ports[] = {
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_INPUT,
			.name = "input",
			.flags = PW_NDARRAY_FILTER_PORT_FLAG_ACQUISITION_V3,
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.n_dimensions = 1,
				.shape = shape,
			},
		},
		{
			.struct_size = sizeof(struct pw_ndarray_filter_port),
			.direction = SPA_DIRECTION_OUTPUT,
			.name = "output",
			.format = {
				.element_type = SPA_ELEMENT_TYPE_F32_LE,
				.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
				.n_dimensions = 1,
				.shape = shape,
			},
		},
	};
	const struct pw_ndarray_filter_config config = {
		.struct_size = sizeof(config),
		.version = PW_VERSION_NDARRAY_FILTER_CONFIG,
		.node_name = "acquisition-v3-admission",
		.n_ports = SPA_N_ELEMENTS(ports),
		.ports = ports,
		.events = &events,
	};
	struct pw_ndarray_filter *filter = (void *)1;
	bool expect_unsupported = argc == 2 && strcmp(argv[1], "--expect-unsupported") == 0;
	int result, expected = expect_unsupported ? -EINVAL : 0;
	bool valid_result;

	if (argc > 2 || (argc == 2 && !expect_unsupported))
		return 2;
	result = pw_ndarray_filter_new(&config, &filter);
	valid_result = expect_unsupported ? filter == NULL : filter != NULL;
	printf("Acquisition V3 admission: result=%d expected=%d result_null=%d\n",
			result, expected, filter == NULL);
	if (result == 0)
		pw_ndarray_filter_destroy(filter);
	return result == expected && valid_result ? 0 : 1;
}
