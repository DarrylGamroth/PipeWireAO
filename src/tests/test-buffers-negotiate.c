/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <unistd.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/filter.h>
#include <pipewire/private.h>
#include <pipewire/buffers.h>

struct test_node {
	struct spa_node node;
	struct spa_hook_list hooks;
	uint32_t requested;
};

static int add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct test_node *node = object;

	spa_hook_list_append(&node->hooks, listener, events, data);
	return 0;
}

static int enum_params(void *object, int seq, enum spa_direction direction SPA_UNUSED,
		uint32_t port_id, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct test_node *node = object;
	uint8_t storage[256], filtered[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod_builder result_builder = SPA_POD_BUILDER_INIT(filtered, sizeof(filtered));
	struct spa_result_node_params result = { .id = id, .index = start, .next = 1 };
	struct spa_pod *param;
	int res;

	if (id != SPA_PARAM_Buffers || port_id != 0)
		return -ENOENT;
	if (start != 0 || num == 0)
		return 0;
	param = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, id,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(node->requested),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(64),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(4),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
	if ((res = spa_pod_filter(&result_builder, &result.param, param, filter)) < 0)
		return res;
	spa_node_emit_result(&node->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
	return 0;
}

static const struct spa_node_methods methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.port_enum_params = enum_params,
};

static void check_negotiate(struct pw_context *context, uint32_t flags,
		uint32_t requested, uint32_t minimum)
{
	struct test_node output = { .requested = requested }, input = { .requested = requested };
	struct pw_buffers buffers = { 0 };
	uint32_t expected = SPA_MAX(requested, minimum);

	spa_hook_list_init(&output.hooks);
	spa_hook_list_init(&input.hooks);
	output.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &output);
	input.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &input);
	spa_assert_se(pw_buffers_negotiate(context, flags, &output.node, 0, &input.node, 0, &buffers) == 0);
	if (buffers.n_buffers != expected)
		fprintf(stderr, "request=%u flags=%#x negotiated=%u expected=%u\n",
				requested, flags, buffers.n_buffers, expected);
	spa_assert_se(buffers.n_buffers == expected);
	spa_assert_se(buffers.flags == flags);
	for (uint32_t i = 0; i < buffers.n_buffers; i++) {
		struct spa_data *data = &buffers.buffers[i]->datas[0];

		spa_assert_se(buffers.buffers[i]->n_datas == 1);
		spa_assert_se((data->data == NULL) == !!(flags & PW_BUFFERS_FLAG_NO_MEM));
		spa_assert_se(data->maxsize == ((flags & PW_BUFFERS_FLAG_NO_MEM) ? 0 : 64));
		spa_assert_se(!!(data->flags & SPA_DATA_FLAG_DYNAMIC) == !!(flags & PW_BUFFERS_FLAG_DYNAMIC));
	}
	pw_buffers_clear(&buffers);
	spa_assert_se(buffers.n_buffers == 0 && buffers.buffers == NULL && buffers.mem == NULL);
}

int main(int argc, char *argv[])
{
	struct pw_context context = { 0 };
	const uint32_t requested[] = { 2, 1, 3, 8 };
	const uint32_t modes[] = { 0, PW_BUFFERS_FLAG_RELIABLE,
		PW_BUFFERS_FLAG_ASYNC, PW_BUFFERS_FLAG_ASYNC | PW_BUFFERS_FLAG_RELIABLE };
	const uint32_t minimum[] = { 1, 1, 2, 3 };
	const uint32_t extras[] = { PW_BUFFERS_FLAG_NO_MEM, PW_BUFFERS_FLAG_SHARED,
		PW_BUFFERS_FLAG_DYNAMIC, PW_BUFFERS_FLAG_IN_PRIORITY };
	uint32_t cases = 0;

	pw_init(&argc, &argv);
	context.properties = pw_properties_new(NULL, NULL);
	context.pool = pw_mempool_new(NULL);
	context.settings.link_max_buffers = 16;
	context.sc_pagesize = sysconf(_SC_PAGESIZE);
	spa_assert_se(context.properties != NULL && context.pool != NULL);
	for (uint32_t mode = 0; mode < SPA_N_ELEMENTS(modes); mode++) {
		for (uint32_t combination = 0; combination < (1u << SPA_N_ELEMENTS(extras)); combination++) {
			uint32_t flags = modes[mode];

			for (uint32_t bit = 0; bit < SPA_N_ELEMENTS(extras); bit++)
				if (combination & (1u << bit))
					flags |= extras[bit];
			for (uint32_t index = 0; index < SPA_N_ELEMENTS(requested); index++) {
				check_negotiate(&context, flags, requested[index], minimum[mode]);
				cases++;
			}
		}
	}
	pw_mempool_destroy(context.pool);
	pw_properties_free(context.properties);
	pw_deinit();
	printf("%u buffer negotiation cases passed\n", cases);
	return 0;
}
