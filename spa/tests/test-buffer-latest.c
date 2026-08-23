/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <spa/node/buffer-latest.h>

#define N_BUFFERS 2u

struct fixture {
	struct spa_buffer storage[N_BUFFERS];
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers_latest first;
	struct spa_io_buffers_latest second;
	struct spa_io_buffers_latest joined;
	struct spa_buffer_latest *latest;
};

static void set_link(struct spa_buffer_latest *latest, uint32_t id,
		struct spa_io_buffers_latest *io, bool active)
{
	struct spa_io_buffers_latest_link link = {
		.id = id,
		.flags = active ? SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE : 0,
		.io = active ? io : NULL,
		.notify_fd = -1,
	};

	spa_assert_se(spa_buffer_latest_set_io(latest,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
}

static void fixture_init(struct fixture *fixture)
{
	uint32_t i;

	memset(fixture, 0, sizeof(*fixture));
	fixture->first = SPA_IO_BUFFERS_LATEST_INIT;
	fixture->second = SPA_IO_BUFFERS_LATEST_INIT;
	fixture->joined = SPA_IO_BUFFERS_LATEST_INIT;
	for (i = 0; i < N_BUFFERS; i++)
		fixture->buffers[i] = &fixture->storage[i];
	fixture->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, fixture, NULL);
	spa_assert_se(fixture->latest != NULL);
	spa_buffer_latest_set_buffers(fixture->latest, fixture->buffers, N_BUFFERS);
}

static void fixture_clear(struct fixture *fixture)
{
	spa_buffer_latest_destroy(fixture->latest);
}

static void receive(struct spa_io_buffers_latest *io,
		uint64_t *sequence, uint32_t *buffer_id)
{
	spa_assert_se(spa_io_buffers_latest_receive(io, sequence, buffer_id) == 0);
}

static void test_fanout_leases(void)
{
	struct fixture fixture;
	struct spa_buffer_latest_stats stats;
	uint64_t first_sequence, second_sequence;
	uint32_t first_id, second_id, id;

	fixture_init(&fixture);
	set_link(fixture.latest, 10, &fixture.first, true);
	set_link(fixture.latest, 11, &fixture.second, true);
	spa_assert_se(spa_buffer_latest_worker_begin(fixture.latest) == 0);
	spa_assert_se(spa_buffer_latest_worker_begin(fixture.latest) == -EBUSY);

	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_queue(fixture.latest, id) == 0);
	receive(&fixture.first, &first_sequence, &first_id);
	receive(&fixture.second, &second_sequence, &second_id);
	spa_assert_se(first_id == second_id && first_id == id);
	spa_assert_se(first_sequence == second_sequence);

	spa_assert_se(spa_io_buffers_latest_complete(&fixture.first, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &first_id) == 0);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.second, 0) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);

	spa_assert_se(spa_buffer_latest_get_stats(fixture.latest, &stats,
			sizeof(stats)) == 0);
	spa_assert_se(stats.publications == 1);
	spa_assert_se(stats.subscriber_deliveries == 2);
	spa_assert_se(stats.completions == 2);
	spa_assert_se(spa_buffer_latest_worker_end(fixture.latest) == 0);
	spa_assert_se(spa_buffer_latest_worker_end(fixture.latest) == -EINVAL);
	fixture_clear(&fixture);
}

static void test_leaky_submission(void)
{
	struct fixture fixture;
	struct spa_buffer_latest_stats stats;
	uint64_t sequence;
	uint32_t id;

	fixture_init(&fixture);
	set_link(fixture.latest, 20, &fixture.first, true);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_queue(fixture.latest, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_queue(fixture.latest, id) == 0);
	receive(&fixture.first, &sequence, &id);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.first, 1) == 0);

	spa_assert_se(spa_buffer_latest_get_stats(fixture.latest, &stats,
			sizeof(stats)) == 0);
	spa_assert_se(stats.submission_overflows == 1);
	fixture_clear(&fixture);
}

static void test_live_retirement_and_join(void)
{
	struct fixture fixture;
	struct spa_buffer_latest_stats stats;
	uint64_t sequence;
	uint32_t id;

	fixture_init(&fixture);
	set_link(fixture.latest, 30, &fixture.first, true);
	set_link(fixture.latest, 31, &fixture.second, true);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(spa_buffer_latest_queue(fixture.latest, id) == 0);
	receive(&fixture.first, &sequence, &id);
	set_link(fixture.latest, 31, NULL, false);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.first, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);

	set_link(fixture.latest, 32, &fixture.joined, true);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(spa_buffer_latest_queue(fixture.latest, id) == 0);
	receive(&fixture.first, &sequence, &id);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.first, id) == 0);
	receive(&fixture.joined, &sequence, &id);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.joined, id) == 0);

	spa_assert_se(spa_buffer_latest_get_stats(fixture.latest, &stats,
			sizeof(stats)) == 0);
	spa_assert_se(stats.subscriber_retirements == 1);
	spa_assert_se(stats.retired_leases == 1);
	spa_assert_se(stats.subscriber_deliveries == 4);
	fixture_clear(&fixture);
}

static void test_progressive_lease(void)
{
	struct fixture fixture;
	uint64_t sequence;
	uint32_t id;

	fixture_init(&fixture);
	set_link(fixture.latest, 40, &fixture.first, true);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_begin_progressive(fixture.latest, id) == 0);
	receive(&fixture.first, &sequence, &id);
	spa_assert_se(id == 0);
	spa_assert_se(spa_io_buffers_latest_complete(&fixture.first, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 1);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	spa_assert_se(spa_buffer_latest_end_progressive(fixture.latest, 0) == 0);
	spa_assert_se(spa_buffer_latest_try_dequeue_reusable(fixture.latest, &id) == 1);
	spa_assert_se(id == 0);
	spa_assert_se(spa_buffer_latest_return(fixture.latest, id) == 0);
	fixture_clear(&fixture);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	test_fanout_leases();
	test_leaky_submission();
	test_live_retirement_and_join();
	test_progressive_lease();
	return 0;
}
