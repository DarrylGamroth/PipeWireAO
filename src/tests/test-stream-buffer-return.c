/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <pipewire/log.h>

PW_LOG_TOPIC(log_stream, "pw.stream");

#include "../pipewire/stream.c"

static void test_return(enum spa_direction direction, bool with_busy)
{
	struct stream impl = { 0 };
	struct pw_impl_node node = { 0 };
	struct spa_meta_busy busy = { 0 };
	struct buffer *buffer = &impl.buffers[0];
	struct pw_buffer *loan;
	uint32_t index, before;
	unsigned int i;

	impl.this.node = &node;
	impl.direction = direction;
	impl.using_trigger = true;
	impl.n_buffers = 1;
	buffer->this.size = 16;
	buffer->busy = with_busy ? &busy : NULL;
	/* Input receives Busy ownership with the queued delivery; output claims
	 * Busy ownership only when the application dequeues the buffer. */
	busy.count = direction == SPA_DIRECTION_INPUT ? 1 : 0;
	spa_ringbuffer_init(&impl.dequeued.ring);
	spa_ringbuffer_init(&impl.queued.ring);
	spa_assert_se(queue_push(&impl, &impl.dequeued, buffer) == 0);

	for (i = 0; i < 128; i++) {
		loan = pw_stream_dequeue_buffer(&impl.this);
		spa_assert_se(loan == &buffer->this);
		spa_assert_se(SPA_FLAG_IS_SET(buffer->flags, BUFFER_FLAG_DEQUEUED));
		spa_assert_se(!with_busy || busy.count == 1);
		spa_assert_se(pw_stream_return_buffer(&impl.this, loan) >= 0);
		spa_assert_se(!with_busy || busy.count ==
				(direction == SPA_DIRECTION_INPUT ? 1u : 0u));
		spa_assert_se(!SPA_FLAG_IS_SET(buffer->flags, BUFFER_FLAG_DEQUEUED));
		spa_ringbuffer_get_read_index(&impl.dequeued.ring, &before);
		spa_assert_se(pw_stream_return_buffer(&impl.this, loan) == -EINVAL);
		spa_ringbuffer_get_read_index(&impl.dequeued.ring, &index);
		spa_assert_se(index == before);
		spa_assert_se(impl.dequeued.outcount == 0);
	}
	loan = pw_stream_dequeue_buffer(&impl.this);
	spa_assert_se(loan == &buffer->this);
	spa_assert_se(pw_stream_queue_buffer(&impl.this, loan) == 0);
	spa_assert_se(!with_busy || busy.count == 0);
	spa_assert_se(pw_stream_return_buffer(&impl.this, loan) == -EINVAL);
	spa_assert_se(queue_pop(&impl, &impl.queued) == buffer);
	spa_assert_se(pw_stream_dequeue_buffer(&impl.this) == NULL);
}

static void test_failed_return(enum spa_direction direction, bool with_busy)
{
	struct stream impl = { 0 };
	struct pw_impl_node node = { 0 };
	struct spa_meta_busy busy = { 0 };
	struct buffer *buffer = &impl.buffers[0];
	uint32_t index;
	struct pw_buffer *loan;

	impl.this.node = &node;
	impl.direction = direction;
	impl.using_trigger = true;
	impl.n_buffers = 1;
	buffer->this.size = 16;
	buffer->busy = with_busy ? &busy : NULL;
	busy.count = direction == SPA_DIRECTION_INPUT ? 1 : 0;
	spa_ringbuffer_init(&impl.dequeued.ring);
	spa_assert_se(queue_push(&impl, &impl.dequeued, buffer) == 0);
	loan = pw_stream_dequeue_buffer(&impl.this);
	spa_assert_se(loan == &buffer->this);
	/* An invalid ring fill rejects the front insertion without publication. */
	spa_ringbuffer_get_read_index(&impl.dequeued.ring, &index);
	spa_ringbuffer_write_update(&impl.dequeued.ring, index - 1);
	spa_assert_se(pw_stream_return_buffer(&impl.this, loan) < 0);
	spa_assert_se(SPA_FLAG_IS_SET(buffer->flags, BUFFER_FLAG_DEQUEUED));
	spa_assert_se(!with_busy || busy.count == 1);
	spa_assert_se(impl.dequeued.outcount == 16);
	spa_ringbuffer_write_update(&impl.dequeued.ring, index);
	spa_assert_se(pw_stream_return_buffer(&impl.this, loan) >= 0);
	spa_assert_se(pw_stream_dequeue_buffer(&impl.this) == loan);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	if (argc > 1 && spa_streq(argv[1], "input")) {
		test_return(SPA_DIRECTION_INPUT, true);
	} else {
		test_return(SPA_DIRECTION_OUTPUT, true);
		test_return(SPA_DIRECTION_INPUT, true);
		test_return(SPA_DIRECTION_OUTPUT, false);
		test_return(SPA_DIRECTION_INPUT, false);
		test_failed_return(SPA_DIRECTION_OUTPUT, true);
		test_failed_return(SPA_DIRECTION_INPUT, true);
		test_failed_return(SPA_DIRECTION_OUTPUT, false);
		test_failed_return(SPA_DIRECTION_INPUT, false);
	}
	pw_deinit();
	return 0;
}
