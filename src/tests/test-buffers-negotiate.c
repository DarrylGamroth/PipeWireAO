/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <pipewire/buffers.h>
#include <pipewire/private.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/filter.h>
#include <stdio.h>
#include <unistd.h>

struct test_node {
	struct spa_node node;
	struct spa_hook_list hooks;
	uint32_t requested;
	uint32_t required_meta;
	bool required;
	uint32_t metadata_size;
	uint32_t metadata_supported;
	uint32_t metadata_type;
	uint32_t metadata_count;
	uint32_t metadata_features;
	uint32_t extra_bytes;
	bool ndarray;
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
	uint8_t storage[512], filtered[512];
	uint32_t total =
	    id == SPA_PARAM_Meta ? (node->metadata_count ? node->metadata_count : 1) : 1;
	if (port_id != 0 ||
	    (id != SPA_PARAM_Buffers && id != SPA_PARAM_Meta && id != SPA_PARAM_Format) ||
	    (id == SPA_PARAM_Meta && !node->metadata_supported) ||
	    (id == SPA_PARAM_Format && !node->ndarray))
		return -ENOENT;
	for (uint32_t i = start; num && i < total; i++) {
		struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
		struct spa_pod_builder result_builder =
		    SPA_POD_BUILDER_INIT(filtered, sizeof(filtered));
		struct spa_result_node_params result = {.id = id, .index = i, .next = i + 1};
		struct spa_pod *param;
		if (id == SPA_PARAM_Format) {
			param = spa_pod_builder_add_object(
			    &builder, SPA_TYPE_OBJECT_Format, id, SPA_FORMAT_mediaType,
			    SPA_POD_Id(SPA_MEDIA_TYPE_application), SPA_FORMAT_mediaSubtype,
			    SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray));
		} else if (id == SPA_PARAM_Meta) {
			uint32_t type = node->metadata_count
					    ? (i == 0 ? SPA_META_Header : 0x400 + i)
					    : node->metadata_type;
			uint32_t size = spa_meta_record_size(type);
			if (size == 0 && type < SPA_META_START_features)
				size = node->metadata_size ? node->metadata_size : 128;
			if (!node->metadata_count && node->metadata_size)
				size = node->metadata_size;
			if (node->metadata_count && i + 1 == total)
				size += node->extra_bytes;
			param = spa_pod_builder_add_object(
			    &builder, SPA_TYPE_OBJECT_ParamMeta, id, SPA_PARAM_META_type,
			    SPA_POD_Id(type), SPA_PARAM_META_size, SPA_POD_Int(size),
			    SPA_PARAM_META_required, SPA_POD_Bool(node->required),
			    SPA_PARAM_META_features, SPA_POD_Int(node->metadata_features));
		} else {
			param = spa_pod_builder_add_object(
			    &builder, SPA_TYPE_OBJECT_ParamBuffers, id, SPA_PARAM_BUFFERS_buffers,
			    SPA_POD_Int(node->requested), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			    SPA_PARAM_BUFFERS_size, SPA_POD_Int(64), SPA_PARAM_BUFFERS_stride,
			    SPA_POD_Int(4), SPA_PARAM_BUFFERS_dataType,
			    SPA_POD_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)),
			    SPA_PARAM_BUFFERS_metaType, SPA_POD_Int(node->required_meta));
		}
		if (spa_pod_filter(&result_builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&node->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
		return 0;
	}
	return 0;
}

static const struct spa_node_methods methods = {
    SPA_VERSION_NODE_METHODS,
    .add_listener = add_listener,
    .port_enum_params = enum_params,
};

static void check_negotiate(struct pw_context *context, uint32_t flags, uint32_t requested,
			    uint32_t minimum)
{
	struct test_node output = {.requested = requested}, input = {.requested = requested};
	struct pw_buffers buffers = {0};
	uint32_t expected = SPA_MAX(requested, minimum);

	spa_hook_list_init(&output.hooks);
	spa_hook_list_init(&input.hooks);
	output.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &output);
	input.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &input);
	spa_assert_se(
	    pw_buffers_negotiate(context, flags, &output.node, 0, &input.node, 0, &buffers) == 0);
	if (buffers.n_buffers != expected)
		fprintf(stderr, "request=%u flags=%#x negotiated=%u expected=%u\n", requested,
			flags, buffers.n_buffers, expected);
	spa_assert_se(buffers.n_buffers == expected);
	spa_assert_se(buffers.flags == flags);
	for (uint32_t i = 0; i < buffers.n_buffers; i++) {
		struct spa_data *data = &buffers.buffers[i]->datas[0];

		spa_assert_se(buffers.buffers[i]->n_datas == 1);
		spa_assert_se((data->data == NULL) == !!(flags & PW_BUFFERS_FLAG_NO_MEM));
		spa_assert_se(data->maxsize == ((flags & PW_BUFFERS_FLAG_NO_MEM) ? 0 : 64));
		spa_assert_se(!!(data->flags & SPA_DATA_FLAG_DYNAMIC) ==
			      !!(flags & PW_BUFFERS_FLAG_DYNAMIC));
	}
	pw_buffers_clear(&buffers);
	spa_assert_se(buffers.n_buffers == 0 && buffers.buffers == NULL && buffers.mem == NULL);
}

static void check_metadata_negotiate(struct pw_context *context, bool output_support,
				     bool input_support, uint32_t type, uint32_t input_size,
				     int expected)
{
	struct test_node output = {.requested = 2,
				   .metadata_supported = output_support,
				   .metadata_type = type,
				   .required = !input_support};
	struct test_node input = {.requested = 2,
				  .metadata_supported = input_support,
				  .metadata_type = type,
				  .metadata_size = input_size,
				  .required = true};
	if (type >= SPA_META_START_features || type == SPA_META_Bitmap)
		output.metadata_size = input_size;
	struct pw_buffers buffers = {0};
	spa_hook_list_init(&output.hooks);
	spa_hook_list_init(&input.hooks);
	output.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &output);
	input.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &input);
	int result = pw_buffers_negotiate(context, 0, &output.node, 0, &input.node, 0, &buffers);
	if (result != expected)
		fprintf(stderr, "type=%u support=%u/%u size=%u result=%d expected=%d\n", type,
			output_support, input_support, input_size, result, expected);
	spa_assert_se(result == expected);
	if (result == 0) {
		spa_assert_se(buffers.n_buffers == 2);
		spa_assert_se(spa_buffer_find_meta(buffers.buffers[0], type) != NULL);
	}
	pw_buffers_clear(&buffers);
}

static void check_metadata_bounds(struct pw_context *context, uint32_t count, uint32_t size,
				  uint32_t extra, uint32_t features, int expected)
{
	struct test_node output = {.requested = 2,
				   .metadata_supported = 1,
				   .metadata_count = count,
				   .metadata_size = size,
				   .extra_bytes = extra,
				   .metadata_features = features,
				   .ndarray = true};
	struct test_node input = output;
	input.required = true;
	struct pw_buffers buffers = {0};
	spa_hook_list_init(&output.hooks);
	spa_hook_list_init(&input.hooks);
	output.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &output);
	input.node.iface =
	    SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &methods, &input);
	int result = pw_buffers_negotiate(context, 0, &output.node, 0, &input.node, 0, &buffers);
	if (result != expected)
		fprintf(stderr, "count=%u size=%u extra=%u features=%u result=%d expected=%d\n",
			count, size, extra, features, result, expected);
	spa_assert_se(result == expected);
	if (result == 0)
		spa_assert_se(buffers.buffers[0]->n_metas == count * (features ? 2 : 1));
	pw_buffers_clear(&buffers);
}

int main(int argc, char *argv[])
{
	struct pw_context context = {0};
	const uint32_t requested[] = {2, 1, 3, 8};
	const uint32_t modes[] = {0, PW_BUFFERS_FLAG_RELIABLE, PW_BUFFERS_FLAG_ASYNC,
				  PW_BUFFERS_FLAG_ASYNC | PW_BUFFERS_FLAG_RELIABLE};
	const uint32_t minimum[] = {1, 1, 2, 3};
	const uint32_t extras[] = {PW_BUFFERS_FLAG_NO_MEM, PW_BUFFERS_FLAG_SHARED,
				   PW_BUFFERS_FLAG_DYNAMIC, PW_BUFFERS_FLAG_IN_PRIORITY};
	uint32_t cases = 0;

	pw_init(&argc, &argv);
	context.properties = pw_properties_new(NULL, NULL);
	context.pool = pw_mempool_new(NULL);
	context.settings.link_max_buffers = 16;
	context.sc_pagesize = sysconf(_SC_PAGESIZE);
	spa_assert_se(context.properties != NULL && context.pool != NULL);
	for (uint32_t mode = 0; mode < SPA_N_ELEMENTS(modes); mode++) {
		for (uint32_t combination = 0; combination < (1u << SPA_N_ELEMENTS(extras));
		     combination++) {
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
	for (uint32_t type = SPA_META_AcquisitionIdentity; type <= SPA_META_ReferenceTimestamp;
	     type++) {
		if (type == 0x302)
			continue;
		check_metadata_negotiate(&context, true, true, type, 0, 0);
		check_metadata_negotiate(&context, false, true, type, 0, -ENOTSUP);
		check_metadata_negotiate(&context, true, false, type, 0, -ENOTSUP);
		check_metadata_negotiate(&context, true, true, type, spa_meta_record_size(type) + 8,
					 -ENOTSUP);
	}
	check_metadata_negotiate(&context, true, true, 0x302, 336, -ENOTSUP);
	check_metadata_negotiate(&context, true, true, SPA_META_ExposureTiming, 64, -ENOTSUP);
	check_metadata_negotiate(&context, true, true, SPA_META_Bitmap, 8192, 0);
	check_metadata_negotiate(&context, true, true, SPA_META_Invalid, 128, -ENOTSUP);
	check_metadata_negotiate(&context, true, true, 0x3f0, 0, 0);
	check_metadata_negotiate(&context, false, true, 0x3f0, 0, -ENOTSUP);
	for (uint32_t type = 11; type <= 13; type++)
		check_metadata_negotiate(&context, true, true, type, 0, -ENOTSUP);
	check_metadata_negotiate(&context, true, true, SPA_META_TYPE_FEATURES(0x3f0, 1), 8,
				 -ENOTSUP);
	check_metadata_negotiate(&context, true, true, SPA_META_TYPE_FEATURES(13, 1), 0, -ENOTSUP);
	check_metadata_bounds(&context, 64, 64, 32, 0, 0); /* Exactly 64 entries and 4096 bytes. */
	check_metadata_bounds(&context, 64, 64, 40, 0, -ENOSPC);
	check_metadata_bounds(&context, 65, 8, 0, 0, -ENOSPC);
	check_metadata_bounds(&context, 32, 8, 0, 1, 0); /* Feature entries count toward 64. */
	check_metadata_bounds(&context, 33, 8, 0, 1, -ENOSPC);
	puts("custom required types beyond bit31, asymmetric requirements, unknown optional "
	     "storage, incompatible layouts and retired IDs passed");
	pw_mempool_destroy(context.pool);
	pw_properties_free(context.properties);
	pw_deinit();
	printf("%u buffer negotiation cases passed\n", cases);
	return 0;
}
