/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include <dlfcn.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>

#include "egrabber.hpp"

namespace {

constexpr uint32_t requested_buffers = 8;
constexpr uint32_t requested_frames = 10;

struct param_result {
	uint32_t expected = SPA_ID_INVALID;
	uint8_t storage[2048] = {};
	struct spa_pod *param = nullptr;
};

struct test_buffer {
	struct spa_buffer buffer = {};
	struct spa_data data = {};
	struct spa_chunk chunk = {};
	struct spa_meta metas[2] = {};
	struct spa_meta_header header = {};
	struct spa_meta_acquisition acquisition = {};
	void *payload = nullptr;
};

void on_result(void *data, int, int res, uint32_t type, const void *result)
{
	auto *capture = static_cast<param_result *>(data);

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	const auto *params = static_cast<const struct spa_result_node_params *>(result);
	if (params->id != capture->expected || params->param == nullptr)
		return;
	const uint32_t size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = reinterpret_cast<struct spa_pod *>(capture->storage);
}

const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

struct spa_pod *enum_one(struct spa_node *node, param_result &capture,
		uint32_t id)
{
	capture.expected = id;
	capture.param = nullptr;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, 0, 1, nullptr) == 0);
	spa_assert_se(capture.param != nullptr);
	return capture.param;
}

void init_buffer(test_buffer &storage, uint32_t size, uint32_t alignment)
{
	alignment = SPA_MAX(alignment, static_cast<uint32_t>(alignof(max_align_t)));
	spa_assert_se((alignment & (alignment - 1u)) == 0);
	spa_assert_se(posix_memalign(&storage.payload, alignment, size) == 0);
	memset(storage.payload, 0, size);
	storage.data.type = SPA_DATA_MemPtr;
	storage.data.data = storage.payload;
	storage.data.maxsize = size;
	storage.data.fd = -1;
	storage.data.chunk = &storage.chunk;
	storage.metas[0] = {
		.type = SPA_META_Header,
		.size = sizeof(storage.header),
		.data = &storage.header,
	};
	storage.metas[1] = {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage.acquisition),
		.data = &storage.acquisition,
	};
	storage.buffer.n_metas = SPA_N_ELEMENTS(storage.metas);
	storage.buffer.metas = storage.metas;
	storage.buffer.n_datas = 1;
	storage.buffer.datas = &storage.data;
}

uint64_t monotonic_nsec()
{
	struct timespec now;
	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return static_cast<uint64_t>(now.tv_sec) * SPA_NSEC_PER_SEC + now.tv_nsec;
}

int capture(const struct spa_handle_factory *factory)
{
	const size_t size = factory->get_size(factory, nullptr);
	std::unique_ptr<void, decltype(&free)> memory(calloc(1, size), free);
	spa_assert_se(memory != nullptr);
	auto *handle = static_cast<struct spa_handle *>(memory.get());
	const int initialized = factory->init(factory, handle, nullptr, nullptr, 0);
	if (initialized < 0)
		return 77;

	struct spa_node *node = nullptr;
	struct spa_hook listener;
	param_result params;
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			reinterpret_cast<void **>(&node)) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events, &params) == 0);

	struct spa_pod *format = enum_one(node, params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	struct spa_video_info_raw video = {};
	spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);

	struct spa_pod *buffers_param = enum_one(node, params, SPA_PARAM_Buffers);
	int32_t payload_size = 0;
	int32_t alignment = 1;
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, nullptr,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size),
			SPA_PARAM_BUFFERS_align, SPA_POD_Int(&alignment)) >= 0);
	spa_assert_se(payload_size > 0);
	spa_assert_se(alignment > 0);

	std::vector<test_buffer> storage(requested_buffers);
	std::vector<struct spa_buffer *> buffers(requested_buffers);
	for (uint32_t i = 0; i < requested_buffers; i++) {
		init_buffer(storage[i], static_cast<uint32_t>(payload_size),
				static_cast<uint32_t>(alignment));
		buffers[i] = &storage[i].buffer;
	}
	struct spa_io_buffers_latest io = {};
	struct spa_io_buffers_latest_link link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &io,
		.notify_fd = -1,
	};
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers.data(), buffers.size()) == 0);
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	spa_assert_se(spa_node_send_command(node, &start) == 0);

	const uint64_t deadline = monotonic_nsec() + 3 * SPA_NSEC_PER_SEC;
	uint32_t frames = 0;
	while (frames < requested_frames) {
		spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
		uint64_t submission;
		uint32_t id;
		const int received = spa_io_buffers_latest_receive(&io, &submission, &id);
		if (received == -EPIPE) {
			spa_assert_se(monotonic_nsec() < deadline);
			continue;
		}
		spa_assert_se(received == 0);
		spa_assert_se(id < storage.size());
		spa_assert_se(storage[id].chunk.size > 0);
		spa_assert_se(storage[id].chunk.size <=
				static_cast<uint32_t>(payload_size));
		spa_assert_se(spa_meta_acquisition_is_valid(&storage[id].metas[1]));
		spa_assert_se(spa_io_buffers_latest_complete(&io, id) == 0);
		frames++;
	}

	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			nullptr, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	for (auto &item : storage)
		free(item.payload);
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	spa_assert_se(argc == 2);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != nullptr);
	auto enumerate = reinterpret_cast<spa_handle_factory_enum_func_t>(
			dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME));
	spa_assert_se(enumerate != nullptr);
	const struct spa_handle_factory *factory = nullptr;
	uint32_t index = 0;
	while (enumerate(&factory, &index) > 0 &&
			!spa_streq(factory->name, SPA_NAME_API_EGRABBER_SOURCE))
		factory = nullptr;
	spa_assert_se(factory != nullptr);
	const int res = capture(factory);
	spa_assert_se(dlclose(library) == 0);
	return res;
}
