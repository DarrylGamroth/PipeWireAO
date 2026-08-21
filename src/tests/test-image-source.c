/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <pipewire/image-source.h>
#include <pipewire/impl.h>
#include <pipewire/main-loop.h>
#include <pipewire/private.h>
#include <pipewire/stream.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/utils/result.h>

#define N_BUFFERS 2u
#define PAYLOAD_SIZE 64u

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[3];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	struct spa_meta_progressive progressive;
	uint8_t payload[PAYLOAD_SIZE];
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
	storage->metas[2] = (struct spa_meta) {
		.type = SPA_META_Progressive,
		.size = sizeof(storage->progressive),
		.data = &storage->progressive,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void test_image_source(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_node *node;
	struct spa_io_buffers_latest latest = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest_link link = {
		.id = 10,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest,
		.notify_fd = -1,
	};
	struct test_buffer storage[N_BUFFERS];
	struct spa_buffer *buffers[N_BUFFERS];
	struct pw_image_source_config config = {
		.version = PW_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = N_BUFFERS,
		.max_buffers = N_BUFFERS,
		.flags = PW_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			PW_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION |
			PW_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE,
	};
	struct pw_image_source *source;
	struct pw_image_buffer *slot0, *slot1, *acquired;
	struct spa_meta_acquisition acquisition;
	struct pw_image_frame frame = {
		.version = PW_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.header_flags = SPA_META_HEADER_FLAG_DISCONT,
		.chunk_flags = 0,
		.offset = 0,
		.size = PAYLOAD_SIZE,
		.stride = 8,
		.sequence = 9,
		.pts = SPA_TIME_INVALID,
		.acquisition = &acquisition,
	};
	struct pw_image_progressive progressive = {
		.version = PW_VERSION_IMAGE_PROGRESSIVE,
		.payload_size = PAYLOAD_SIZE,
		.commit_granularity = 16,
		.committed = 16,
	};
	struct pw_image_source_stats stats;
	uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 1 };
	uint64_t submission_sequence;
	uint64_t snapshot;
	uint32_t id, committed;
	enum spa_meta_progressive_state state;
	uint32_t i;

	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i]);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(spa_meta_acquisition_init(&acquisition));
	spa_assert_se(spa_meta_acquisition_set_identity(&acquisition, domain, 3, 42));
	spa_assert_se(spa_meta_acquisition_set_exposure_start(&acquisition,
			123456, 10));

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "image-source-test", NULL);
	spa_assert_se(stream != NULL);
	spa_assert_se(pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NONE, NULL, 0) == 0);
	node = pw_impl_node_get_implementation(stream->node);
	spa_assert_se(node != NULL);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);

	source = pw_image_source_new(stream, &config);
	spa_assert_se(source != NULL);
	spa_assert_se(pw_image_source_prepare(source) == N_BUFFERS);
	spa_assert_se(pw_image_source_get_n_buffers(source) == N_BUFFERS);
	slot0 = pw_image_source_get_buffer(source, 0);
	slot1 = pw_image_source_get_buffer(source, 1);
	spa_assert_se(slot0 != NULL && slot1 != NULL);
	spa_assert_se(pw_image_buffer_get_pw_buffer(slot0)->buffer == buffers[0]);
	pw_image_buffer_set_user_data(slot0, &storage[0]);
	spa_assert_se(pw_image_buffer_get_user_data(slot0) == &storage[0]);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
	spa_assert_se(acquired == slot0);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
	spa_assert_se(acquired == slot1);

	memset(storage[0].payload, 0x5a, sizeof(storage[0].payload));
	spa_assert_se(pw_image_source_publish_complete(source, slot0, &frame) == 0);
	spa_assert_se(storage[0].header.seq == 42);
	spa_assert_se(storage[0].header.pts == 123456);
	spa_assert_se(storage[0].header.flags == SPA_META_HEADER_FLAG_DISCONT);
	spa_assert_se(storage[0].chunk.size == PAYLOAD_SIZE);
	spa_assert_se(spa_meta_acquisition_identity_equal(
			&storage[0].acquisition, &acquisition));
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 0);
	spa_assert_se(acquired == NULL);
	spa_assert_se(spa_io_buffers_latest_receive(&latest, &submission_sequence,
			&id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(buffers[id]->datas[0].data == storage[0].payload);
	spa_assert_se(pw_image_source_try_reclaim(source, &acquired) == 0);
	spa_assert_se(acquired == NULL);
	spa_assert_se(spa_io_buffers_latest_complete(&latest, id) == 0);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
	spa_assert_se(acquired == slot0);
	spa_assert_se(pw_image_buffer_get_state(slot0) ==
			PW_IMAGE_BUFFER_STATE_PRODUCER);

	spa_assert_se(pw_image_source_publish_complete(source, slot0, &frame) == 0);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 0);
	spa_assert_se(pw_image_source_try_reclaim(source, &acquired) == 1);
	spa_assert_se(acquired == slot0);
	spa_assert_se(spa_io_buffers_latest_receive(&latest, &submission_sequence,
			&id) == -EPIPE);

	storage[1].data.type = SPA_DATA_DmaBuf;
	spa_assert_se(pw_image_source_begin_progressive(source, slot1,
			&frame, &progressive) == -ENOTSUP);
	storage[1].data.type = SPA_DATA_MemPtr;
	spa_assert_se(pw_image_source_begin_progressive(source, slot1,
			&frame, &progressive) == 0);
	snapshot = spa_meta_progressive_load_acquire(&storage[1].progressive);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	spa_assert_se(committed == 16 && state == SPA_META_PROGRESSIVE_STATE_ACTIVE);
	spa_assert_se(spa_io_buffers_latest_receive(&latest, &submission_sequence,
			&id) == 0);
	spa_assert_se(id == 1);
	spa_assert_se(pw_image_source_update_progressive(source, slot1, 32) == 1);
	spa_assert_se(pw_image_source_update_progressive(source, slot1, 24) == -EINVAL);
	spa_assert_se(pw_image_source_finish_progressive(source, slot1,
			PAYLOAD_SIZE, SPA_META_PROGRESSIVE_STATE_COMPLETE, 0) == 0);
	spa_assert_se(spa_io_buffers_latest_complete(&latest, id) == 0);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
	spa_assert_se(acquired == slot1);
	spa_assert_se(pw_image_source_return_buffer(source, slot1) == 0);
	spa_assert_se(pw_image_buffer_get_state(slot1) ==
			PW_IMAGE_BUFFER_STATE_AVAILABLE);
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
	spa_assert_se(acquired == slot1);

	spa_assert_se(pw_image_source_get_stats(source, &stats, sizeof(stats)) == 0);
	spa_assert_se(stats.pool_size == N_BUFFERS);
	spa_assert_se(stats.acquire_calls == 7);
	spa_assert_se(stats.available_acquisitions == 3);
	spa_assert_se(stats.reusable_acquisitions == 2);
	spa_assert_se(stats.pool_exhaustions == 2);
	spa_assert_se(stats.producer_returns == 1);
	spa_assert_se(stats.complete_publications == 2);
	spa_assert_se(stats.progressive_started == 1);
	spa_assert_se(stats.progressive_updates == 1);
	spa_assert_se(stats.progressive_completed == 1);
	spa_assert_se(stats.forced_reclaims == 1);
	spa_assert_se(stats.metadata_errors == 1);
	spa_assert_se(stats.max_available_probes == N_BUFFERS);

	progressive.committed = 0;
	spa_assert_se(pw_image_source_begin_progressive(source, slot1,
			&frame, &progressive) == 0);
	spa_assert_se(spa_io_buffers_latest_receive(&latest, &submission_sequence,
			&id) == 0);
	spa_assert_se(pw_image_source_teardown(source) == 0);
	snapshot = spa_meta_progressive_load_acquire(&storage[1].progressive);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	spa_assert_se(state == SPA_META_PROGRESSIVE_STATE_ABORTED);
	spa_assert_se((storage[1].progressive.terminal_flags &
			SPA_META_PROGRESSIVE_FLAG_CANCELLED) != 0);
	spa_assert_se(pw_image_source_get_n_buffers(source) == 0);
	spa_assert_se(pw_image_source_get_stats(source, &stats, sizeof(stats)) == 0);
	spa_assert_se(stats.progressive_aborted == 1);
	spa_assert_se(stats.teardown_returns == 1);
	pw_image_source_destroy(source);

	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	pw_stream_destroy(stream);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void test_image_source_capacity(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_node *node;
	struct spa_io_buffers_latest latest = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest_link link = {
		.id = 20,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest,
		.notify_fd = -1,
	};
	struct test_buffer storage[PW_IMAGE_SOURCE_MAX_BUFFERS];
	struct spa_buffer *buffers[PW_IMAGE_SOURCE_MAX_BUFFERS];
	struct pw_image_source_config config = {
		.version = PW_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = PW_IMAGE_SOURCE_MAX_BUFFERS,
		.max_buffers = PW_IMAGE_SOURCE_MAX_BUFFERS,
	};
	struct pw_image_source *source;
	struct pw_image_buffer *acquired;
	uint32_t i;

	for (i = 0; i < SPA_N_ELEMENTS(storage); i++) {
		init_buffer(&storage[i]);
		buffers[i] = &storage[i].buffer;
	}
	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "image-source-capacity-test", NULL);
	spa_assert_se(stream != NULL);
	spa_assert_se(pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NONE, NULL, 0) == 0);
	node = pw_impl_node_get_implementation(stream->node);
	spa_assert_se(node != NULL);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);

	source = pw_image_source_new(stream, &config);
	spa_assert_se(source != NULL);
	spa_assert_se(pw_image_source_prepare(source) ==
			PW_IMAGE_SOURCE_MAX_BUFFERS);
	for (i = 0; i < PW_IMAGE_SOURCE_MAX_BUFFERS; i++) {
		spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 1);
		spa_assert_se(pw_image_buffer_get_index(acquired) == i);
	}
	spa_assert_se(pw_image_source_try_acquire(source, &acquired) == 0);
	spa_assert_se(acquired == NULL);
	spa_assert_se(pw_image_source_teardown(source) == 0);
	pw_image_source_destroy(source);

	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	pw_stream_destroy(stream);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	test_image_source();
	test_image_source_capacity();
	pw_deinit();
	return 0;
}
