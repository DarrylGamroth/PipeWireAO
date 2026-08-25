/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/ndarray-graph.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta meta;
	struct spa_meta_header header;
	float values[4];
};

static void init_buffer(struct test_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->chunk.size = sizeof(buffer->values);
	buffer->chunk.stride = 2 * sizeof(float);
	buffer->data.type = SPA_DATA_MemPtr;
	buffer->data.flags = SPA_DATA_FLAG_READWRITE;
	buffer->data.fd = -1;
	buffer->data.maxsize = sizeof(buffer->values);
	buffer->data.data = buffer->values;
	buffer->data.chunk = &buffer->chunk;
	buffer->meta.type = SPA_META_Header;
	buffer->meta.size = sizeof(buffer->header);
	buffer->meta.data = &buffer->header;
	buffer->buffer.n_metas = 1;
	buffer->buffer.metas = &buffer->meta;
	buffer->buffer.n_datas = 1;
	buffer->buffer.datas = &buffer->data;
}

static struct spa_pod *build_gain_update(void *data, size_t size,
		float first, float second)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("first:gain"), SPA_POD_Float(first),
			SPA_POD_String("second:gain"), SPA_POD_Float(second),
			0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static void check_properties(struct spa_fgn_graph *graph)
{
	uint8_t data[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod *pod = NULL;
	uint32_t count = 0;

	while (spa_fgn_graph_enum_prop_info(graph, count, &builder, &pod) == 1) {
		assert(pod != NULL);
		count++;
		spa_pod_builder_init(&builder, data, sizeof(data));
	}
	assert(count == 8);
	spa_pod_builder_init(&builder, data, sizeof(data));
	assert(spa_fgn_graph_get_props(graph, &builder, &pod) == 1);
	assert(pod != NULL);
}

int main(int argc, char *argv[])
{
	struct spa_fgn_graph *graph = NULL;
	const struct spa_fgn_format *format;
	struct test_buffer input, output;
	struct spa_buffer *inputs[1], *outputs[1];
	uint8_t props_data[1024];
	struct spa_pod *props;
	char config[8192];
	uint32_t i;
	int res;

	assert(argc == 2);
	res = snprintf(config, sizeof(config),
		"{ nodes = ["
		"  { type = ndarray name = first plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.matrix/1 }"
		"    props = { gain = 2.0 } }"
		"  { type = ndarray name = second plugin = \"%s\" label = scale-f32"
		"    config = { shape = [ 2 2 ] schema = test.matrix/1 }"
		"    props = { gain = 3.0 } }"
		"] links = [ { output = \"first:out\" input = \"second:in\" } ]"
		" inputs = [ \"first:in\" ] outputs = [ \"second:out\" ] }",
		argv[1], argv[1]);
	assert(res > 0 && (size_t)res < sizeof(config));
	res = spa_fgn_graph_new(config, &graph);
	if (res < 0)
		fprintf(stderr, "spa_fgn_graph_new failed: %s (%d)\n",
				strerror(-res), res);
	assert(res == 0);
	assert(graph != NULL);
	assert(spa_fgn_graph_get_n_inputs(graph) == 1);
	assert(spa_fgn_graph_get_n_outputs(graph) == 1);
	assert(spa_fgn_graph_get_port_format(graph, SPA_FGN_PORT_INPUT,
			0, &format) == 0);
	assert(format->element_type == SPA_ELEMENT_TYPE_F32_LE);
	assert(format->n_dimensions == 2);
	assert(format->shape[0] == 2 && format->shape[1] == 2);
	assert(strcmp(format->schema, "test.matrix/1") == 0);
	check_properties(graph);

	init_buffer(&input);
	init_buffer(&output);
	for (i = 0; i < 4; i++)
		input.values[i] = (float)i + 1.0f;
	input.header.seq = 42;
	input.header.pts = 1234567;
	inputs[0] = &input.buffer;
	outputs[0] = &output.buffer;
	assert(spa_fgn_graph_activate(graph) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 6.0f);
	assert(output.header.seq == input.header.seq);
	assert(output.header.pts == input.header.pts);

	props = build_gain_update(props_data, sizeof(props_data), 4.0f, 5.0f);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == 0);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 20.0f);

	/* Validation is graph-wide: the valid first update is not committed. */
	props = build_gain_update(props_data, sizeof(props_data), 7.0f, 100.0f);
	assert(props != NULL);
	assert(spa_fgn_graph_set_props(graph, props) == -ERANGE);
	assert(spa_fgn_graph_process(graph, inputs, 1, outputs, 1) == 0);
	for (i = 0; i < 4; i++)
		assert(output.values[i] == input.values[i] * 20.0f);

	assert(spa_fgn_graph_deactivate(graph) == 0);
	spa_fgn_graph_free(graph);
	return EXIT_SUCCESS;
}
