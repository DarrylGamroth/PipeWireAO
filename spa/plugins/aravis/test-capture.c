/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>

#include "aravis.h"

#define REQUESTED_BUFFERS 8u
#define REQUESTED_FRAMES 16u

struct param_result {
	uint32_t expected;
	uint8_t *storage;
	size_t capacity;
	struct spa_pod *param;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	void *payload;
};

static void on_result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct param_result *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = result;
	if (params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	if (size > capture->capacity) {
		capture->storage = realloc(capture->storage, size);
		spa_assert_se(capture->storage != NULL);
		capture->capacity = size;
	}
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct param_result *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static void init_buffer(struct test_buffer *storage, uint32_t size)
{
	storage->payload = calloc(1, size);
	spa_assert_se(storage->payload != NULL);
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.data = storage->payload;
	storage->data.maxsize = size;
	storage->data.fd = -1;
	storage->data.chunk = &storage->chunk;
	storage->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	storage->metas[1] = (struct spa_meta) {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage->acquisition),
		.data = &storage->acquisition,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int capture(const struct spa_handle_factory *factory,
		const char *device_id)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_DEVICE, device_id),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct test_buffer storage[REQUESTED_BUFFERS] = { 0 };
	struct spa_buffer *buffers[REQUESTED_BUFFERS];
	struct spa_io_buffers_latest io = { 0 };
	struct spa_io_buffers_latest_link link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &io,
		.notify_fd = -1,
	};
	struct param_result params = { .expected = SPA_ID_INVALID };
	struct spa_hook listener;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_video_info_raw video = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_pod *format, *buffers_param;
	uint64_t deadline;
	int64_t last_pts = SPA_TIME_INVALID;
	int32_t payload_size = 0;
	uint32_t frames = 0, i;

	handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(handle != NULL);
	if (factory->init(factory, handle, &info, NULL, 0) < 0) {
		free(handle);
		return 77;
	}
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&params) == 0);
	format = enum_one(node, &params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);
	spa_assert_se(video.size.width > 0 && video.size.height > 0);
	buffers_param = enum_one(node, &params, SPA_PARAM_Buffers);
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size)) >= 0);
	spa_assert_se(payload_size > 0);
	for (i = 0; i < REQUESTED_BUFFERS; i++) {
		init_buffer(&storage[i], (uint32_t)payload_size);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, REQUESTED_BUFFERS) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	deadline = monotonic_nsec() + 5 * SPA_NSEC_PER_SEC;
	while (frames < REQUESTED_FRAMES) {
		uint64_t submission;
		uint32_t id;
		int res;

		res = spa_node_process(node);
		spa_assert_se(res >= SPA_STATUS_OK);
		res = spa_io_buffers_latest_receive(&io, &submission, &id);
		if (res == -EPIPE) {
			spa_assert_se(monotonic_nsec() < deadline);
			continue;
		}
		spa_assert_se(res == 0 && id < REQUESTED_BUFFERS);
		spa_assert_se(storage[id].chunk.size > 0 &&
				storage[id].chunk.size <= (uint32_t)payload_size);
		spa_assert_se(storage[id].header.seq > 0);
		spa_assert_se(storage[id].header.pts != SPA_TIME_INVALID);
		if (last_pts != SPA_TIME_INVALID)
			spa_assert_se(storage[id].header.pts > last_pts);
		last_pts = storage[id].header.pts;
		spa_assert_se(spa_meta_acquisition_is_valid(&storage[id].metas[1]));
		spa_assert_se(spa_io_buffers_latest_complete(&io, id) == 0);
		frames++;
	}
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	free(params.storage);
	for (i = 0; i < REQUESTED_BUFFERS; i++)
		free(storage[i].payload);
	printf("captured %u Aravis frames\n", frames);
	return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	void *library;
	int res;

	spa_assert_se(argc == 3);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_ARAVIS_SOURCE));
	res = capture(factory, argv[2]);
	spa_assert_se(dlclose(library) == 0);
	return res;
}
