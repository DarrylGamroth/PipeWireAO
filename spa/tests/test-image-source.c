/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/buffer/image-source.h>

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

struct test_transport {
	void *submission;
	void *reusable[N_BUFFERS];
	uint32_t n_reusable;
	uint32_t publications;
	uint32_t progressive_starts;
	uint32_t progressive_ends;
	uint32_t returns;
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

static int try_dequeue_reusable(void *data, void **handle)
{
	struct test_transport *transport = data;

	*handle = NULL;
	if (transport->n_reusable == 0)
		return 0;
	*handle = transport->reusable[0];
	transport->reusable[0] = transport->reusable[--transport->n_reusable];
	return 1;
}

static int try_reclaim_submission(void *data, void **handle)
{
	struct test_transport *transport = data;

	*handle = transport->submission;
	transport->submission = NULL;
	return *handle == NULL ? 0 : 1;
}

static int publish_complete(void *data, void *handle)
{
	struct test_transport *transport = data;

	if (transport->submission != NULL)
		return -EBUSY;
	transport->submission = handle;
	transport->publications++;
	return 0;
}

static int begin_progressive(void *data, void *handle)
{
	struct test_transport *transport = data;

	if (transport->submission != NULL)
		return -EBUSY;
	transport->submission = handle;
	transport->progressive_starts++;
	return 0;
}

static int end_progressive(void *data, void *handle)
{
	struct test_transport *transport = data;

	if (transport->submission != handle)
		return -EPROTO;
	transport->progressive_ends++;
	return 0;
}

static int return_buffer(void *data, void *handle SPA_UNUSED)
{
	struct test_transport *transport = data;

	transport->returns++;
	return 0;
}

static void complete_submission(struct test_transport *transport)
{
	spa_assert(transport->submission != NULL);
	spa_assert(transport->n_reusable < SPA_N_ELEMENTS(transport->reusable));
	transport->reusable[transport->n_reusable++] = transport->submission;
	transport->submission = NULL;
}

static const struct spa_image_source_transport transport_methods = {
	.try_dequeue_reusable = try_dequeue_reusable,
	.try_reclaim_submission = try_reclaim_submission,
	.publish_complete = publish_complete,
	.begin_progressive = begin_progressive,
	.end_progressive = end_progressive,
	.return_buffer = return_buffer,
};

static void test_source(void)
{
	struct test_buffer storage[N_BUFFERS];
	struct spa_image_source_buffer_info buffers[N_BUFFERS];
	struct test_transport transport = { 0 };
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = N_BUFFERS,
		.max_buffers = N_BUFFERS,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION |
			SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE,
	};
	struct spa_image_source source;
	struct spa_image_source_buffer *slot0, *slot1, *acquired;
	struct spa_meta_acquisition acquisition;
	struct spa_image_frame frame = {
		.version = SPA_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.header_flags = SPA_META_HEADER_FLAG_DISCONT,
		.size = PAYLOAD_SIZE,
		.stride = 8,
		.sequence = 9,
		.pts = SPA_TIME_INVALID,
		.acquisition = &acquisition,
	};
	struct spa_image_progressive progressive = {
		.version = SPA_VERSION_IMAGE_PROGRESSIVE,
		.payload_size = PAYLOAD_SIZE,
		.commit_granularity = 16,
		.committed = 16,
	};
	struct spa_image_source_stats stats;
	uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 1 };
	uint64_t snapshot;
	uint32_t committed, i;
	enum spa_meta_progressive_state state;

	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i]);
		buffers[i].buffer = &storage[i].buffer;
		buffers[i].handle = &storage[i];
	}
	spa_assert_se(spa_meta_acquisition_init(&acquisition));
	spa_assert_se(spa_meta_acquisition_set_identity(&acquisition,
			domain, 3, 42));
	spa_assert_se(spa_meta_acquisition_set_exposure_start(&acquisition,
			123456, 10));

	spa_assert_se(spa_image_source_init(&source, &config,
			&transport_methods, &transport) == 0);
	spa_assert_se(spa_image_source_prepare(&source, buffers, N_BUFFERS) ==
			N_BUFFERS);
	spa_assert_se(spa_image_source_get_n_buffers(&source) == N_BUFFERS);
	slot0 = spa_image_source_get_buffer(&source, 0);
	slot1 = spa_image_source_get_buffer(&source, 1);
	spa_assert_se(spa_image_source_buffer_get_buffer(slot0) ==
			&storage[0].buffer);
	spa_image_source_buffer_set_user_data(slot0, &storage[0]);
	spa_assert_se(spa_image_source_buffer_get_user_data(slot0) == &storage[0]);
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 1);
	spa_assert_se(acquired == slot0);
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 1);
	spa_assert_se(acquired == slot1);

	memset(storage[0].payload, 0x5a, sizeof(storage[0].payload));
	spa_assert_se(spa_image_source_publish_complete(&source, slot0,
			&frame) == 0);
	spa_assert_se(storage[0].header.seq == 42);
	spa_assert_se(storage[0].header.pts == 123456);
	spa_assert_se(storage[0].header.flags == SPA_META_HEADER_FLAG_DISCONT);
	spa_assert_se(storage[0].chunk.size == PAYLOAD_SIZE);
	spa_assert_se(spa_meta_acquisition_identity_equal(
			&storage[0].acquisition, &acquisition));
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 0);
	spa_assert_se(acquired == NULL);
	complete_submission(&transport);
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 1);
	spa_assert_se(acquired == slot0);

	spa_assert_se(spa_image_source_publish_complete(&source, slot0,
			&frame) == 0);
	spa_assert_se(spa_image_source_try_reclaim_submission(&source,
			&acquired) == 1);
	spa_assert_se(acquired == slot0);

	storage[1].data.type = SPA_DATA_DmaBuf;
	spa_assert_se(spa_image_source_begin_progressive(&source, slot1,
			&frame, &progressive) == -ENOTSUP);
	storage[1].data.type = SPA_DATA_MemPtr;
	spa_assert_se(spa_image_source_begin_progressive(&source, slot1,
			&frame, &progressive) == 0);
	snapshot = spa_meta_progressive_load_acquire(&storage[1].progressive);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	spa_assert_se(committed == 16 &&
			state == SPA_META_PROGRESSIVE_STATE_ACTIVE);
	spa_assert_se(spa_image_source_update_progressive(&source, slot1, 32) == 1);
	spa_assert_se(spa_image_source_update_progressive(&source, slot1, 24) ==
			-EINVAL);
	spa_assert_se(spa_image_source_finish_progressive(&source, slot1,
			PAYLOAD_SIZE, SPA_META_PROGRESSIVE_STATE_COMPLETE, 0) == 0);
	complete_submission(&transport);
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 1);
	spa_assert_se(acquired == slot1);
	spa_assert_se(spa_image_source_return_buffer(&source, slot1) == 0);
	spa_assert_se(spa_image_source_try_acquire(&source, &acquired) == 1);
	spa_assert_se(acquired == slot1);

	spa_assert_se(spa_image_source_get_stats(&source, &stats,
			sizeof(stats)) == 0);
	spa_assert_se(stats.pool_size == N_BUFFERS);
	spa_assert_se(stats.complete_publications == 2);
	spa_assert_se(stats.progressive_started == 1);
	spa_assert_se(stats.progressive_updates == 1);
	spa_assert_se(stats.progressive_completed == 1);
	spa_assert_se(stats.forced_reclaims == 1);
	spa_assert_se(stats.metadata_errors == 1);

	progressive.committed = 0;
	spa_assert_se(spa_image_source_begin_progressive(&source, slot1,
			&frame, &progressive) == 0);
	spa_assert_se(spa_image_source_teardown(&source) == 0);
	snapshot = spa_meta_progressive_load_acquire(&storage[1].progressive);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	spa_assert_se(state == SPA_META_PROGRESSIVE_STATE_ABORTED);
	spa_assert_se((storage[1].progressive.terminal_flags &
			SPA_META_PROGRESSIVE_FLAG_CANCELLED) != 0);
	spa_assert_se(spa_image_source_get_n_buffers(&source) == 0);
	spa_assert_se(transport.progressive_starts == 2);
	spa_assert_se(transport.progressive_ends == 2);
	spa_assert_se(transport.returns == 1);
}

static void test_prepare_validation(void)
{
	struct test_buffer storage[N_BUFFERS];
	struct spa_image_source_buffer_info buffers[N_BUFFERS];
	struct test_transport transport = { 0 };
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = N_BUFFERS,
		.max_buffers = N_BUFFERS,
	};
	struct spa_image_source source;
	uint32_t i;

	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i]);
		buffers[i].buffer = &storage[i].buffer;
		buffers[i].handle = &storage[i];
	}
	spa_assert_se(spa_image_source_init(&source, &config,
			&transport_methods, &transport) == 0);
	spa_assert_se(spa_image_source_prepare(&source, buffers, 1) == -ENOBUFS);
	buffers[1].handle = buffers[0].handle;
	spa_assert_se(spa_image_source_prepare(&source, buffers, N_BUFFERS) ==
			-EINVAL);
	buffers[1].handle = &storage[1];
	spa_assert_se(spa_image_source_prepare(&source, buffers, N_BUFFERS) ==
			N_BUFFERS);
	spa_assert_se(spa_image_source_prepare(&source, buffers, N_BUFFERS) ==
			-EINVAL);
	spa_assert_se(spa_image_source_teardown(&source) == 0);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	test_source();
	test_prepare_validation();
	return 0;
}
