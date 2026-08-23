/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <pipewire/pipewire.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/video/format-utils.h>

#define N_BUFFERS 3u
#define WIDTH 64u
#define HEIGHT 64u
#define STRIDE (WIDTH * 2u)
#define FRAME_SIZE (STRIDE * HEIGHT)

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	uint8_t payload[FRAME_SIZE];
};

struct node_info_state {
	uint64_t flags;
};

static void on_node_info(void *data, const struct spa_node_info *info)
{
	struct node_info_state *state = data;

	if (info->change_mask & SPA_NODE_CHANGE_MASK_FLAGS)
		state->flags = info->flags;
}

static const struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static void init_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.data = storage->payload;
	storage->data.maxsize = sizeof(storage->payload);
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

static struct spa_pod *build_format(struct spa_pod_builder *builder)
{
	return spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_GRAY16_LE),
			SPA_FORMAT_VIDEO_size,
			SPA_POD_Rectangle(&SPA_RECTANGLE(WIDTH, HEIGHT)),
			SPA_FORMAT_VIDEO_framerate,
			SPA_POD_Fraction(&SPA_FRACTION(1000, 1)));
}

static uint64_t receive(struct spa_io_buffers_latest *io, uint32_t *id)
{
	uint64_t sequence;

	spa_assert_se(spa_io_buffers_latest_receive(io, &sequence, id) == 0);
	spa_assert_se(*id < N_BUFFERS);
	return sequence;
}

static void check_frame(struct test_buffer *storage, uint64_t sequence)
{
	uint16_t first;

	spa_assert_se(storage->chunk.offset == 0);
	spa_assert_se(storage->chunk.size == FRAME_SIZE);
	spa_assert_se(storage->chunk.stride == STRIDE);
	spa_assert_se(storage->header.seq == sequence);
	spa_assert_se(storage->header.pts == (int64_t)(sequence * SPA_NSEC_PER_MSEC));
	spa_assert_se(spa_meta_acquisition_is_valid(&storage->metas[1]));
	spa_assert_se(storage->acquisition.sequence == sequence);
	spa_assert_se(storage->acquisition.generation == 1);
	memcpy(&first, storage->payload, sizeof(first));
	spa_assert_se(first == (uint16_t)sequence);
}

static void set_link(struct spa_node *node, uint32_t id,
		struct spa_io_buffers_latest *io, bool active)
{
	struct spa_io_buffers_latest_link link = {
		.id = id,
		.flags = active ? SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE : 0,
		.io = io,
		.notify_fd = -1,
	};

	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
}

static void test_ao_imagesrc(void)
{
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_hook listener;
	struct node_info_state info = { 0 };
	struct test_buffer storage[N_BUFFERS];
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers_latest io1 = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest io2 = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_pod_builder builder;
	uint8_t pod_buffer[256];
	struct spa_pod *format;
	uint32_t held1[N_BUFFERS], held2[N_BUFFERS], id1, id2, i;
	uint64_t sequence;

	handle = pw_load_spa_handle("test/libspa-test", "test.ao-imagesrc",
			NULL, 0, NULL);
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events, &info) == 0);
	spa_assert_se(SPA_FLAG_IS_SET(info.flags, SPA_NODE_FLAG_RT));
	spa_assert_se(SPA_FLAG_IS_SET(info.flags, SPA_NODE_FLAG_RTC_PROCESS));

	spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
	format = build_format(&builder);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i]);
		buffers[i] = &storage[i].buffer;
	}
	set_link(node, 11, &io1, true);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, N_BUFFERS) == 0);
	spa_assert_se(spa_node_send_command(node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start)) == 0);

	/* Hold every lease. The producer must stop publishing, not block or copy. */
	for (i = 0; i < N_BUFFERS; i++) {
		spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
		sequence = receive(&io1, &held1[i]);
		spa_assert_se(sequence == i + 1u);
		check_frame(&storage[held1[i]], sequence);
	}
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(spa_io_buffers_latest_receive(&io1, &sequence, &id1) == -EPIPE);

	/* A completion restores progress without allocating a replacement buffer. */
	spa_assert_se(spa_io_buffers_latest_complete(&io1, held1[0]) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	sequence = receive(&io1, &id1);
	spa_assert_se(sequence == N_BUFFERS + 1u);
	spa_assert_se(id1 == held1[0]);

	/* A live subscriber receives the same pool ID, never a copied payload. */
	set_link(node, 12, &io2, true);
	spa_assert_se(spa_io_buffers_latest_complete(&io1, held1[1]) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	sequence = receive(&io1, &id1);
	spa_assert_se(receive(&io2, &id2) == sequence);
	spa_assert_se(id1 == id2);
	spa_assert_se(storage[id1].data.data == storage[id2].data.data);

	/* Retirement releases subscriber two's outstanding lease synchronously. */
	set_link(node, 12, &io2, false);
	spa_assert_se(spa_io_buffers_latest_complete(&io1, held1[2]) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	sequence = receive(&io1, &held2[0]);
	check_frame(&storage[held2[0]], sequence);
	spa_assert_se(spa_io_buffers_latest_receive(&io2, &sequence, &id2) == -EPIPE);

	spa_assert_se(spa_node_send_command(node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause)) == 0);
	set_link(node, 11, &io1, false);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, NULL, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(pw_unload_spa_handle(handle) == 0);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	test_ao_imagesrc();
	pw_deinit();
	return 0;
}
