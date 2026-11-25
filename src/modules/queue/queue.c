/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <errno.h>
#include <stdbool.h>

int pwao_queue_ring_init(struct pwao_queue_ring *ring, uint32_t capacity,
		enum pwao_queue_overflow overflow)
{
	uint32_t i, mask = 0;

	if (ring == NULL || capacity == 0 ||
			capacity > PWAO_QUEUE_RING_MAX_SLOTS ||
			(overflow != PWAO_QUEUE_OVERFLOW_BACKPRESSURE &&
			 overflow != PWAO_QUEUE_OVERFLOW_DROP_OLDEST &&
			 overflow != PWAO_QUEUE_OVERFLOW_DROP_NEWEST))
		return -EINVAL;

	/* The slot cycle must divide the uint64_t position cycle. */
	while (mask < capacity - 1u)
		mask = (mask << 1u) | 1u;
	ring->capacity = capacity;
	ring->mask = mask;
	ring->overflow = overflow;
	atomic_init(&ring->read_index, 0);
	atomic_init(&ring->write_index, 0);
	for (i = 0; i < PWAO_QUEUE_RING_MAX_SLOTS; i++)
		atomic_init(&ring->slots[i], UINT64_MAX);
	return 0;
}

static void publish(struct pwao_queue_ring *ring, uint64_t write_index,
		uint64_t value)
{
	atomic_store_explicit(&ring->slots[write_index & ring->mask], value,
			memory_order_relaxed);
	atomic_store_explicit(&ring->write_index, write_index + 1u,
			memory_order_release);
}

int pwao_queue_ring_try_push(struct pwao_queue_ring *ring, uint64_t value)
{
	uint64_t read_index, write_index;

	if (ring == NULL || ring->capacity == 0 || value == UINT64_MAX)
		return -EINVAL;
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_relaxed);
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	if (write_index - read_index >= ring->capacity)
		return 0;
	publish(ring, write_index, value);
	return 1;
}

static int consume(struct pwao_queue_ring *ring, uint64_t expected,
		uint64_t *value)
{
	const bool claim = expected != UINT64_MAX;
	const bool drop_oldest = ring->overflow == PWAO_QUEUE_OVERFLOW_DROP_OLDEST;
	uint32_t attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		/* An eviction publishes the producer's preceding write index. */
		uint64_t read_index = atomic_load_explicit(&ring->read_index,
				memory_order_acquire);
		const uint64_t write_index = atomic_load_explicit(
				&ring->write_index, memory_order_acquire);
		uint64_t candidate;

		if (read_index == write_index)
			return 0;
		candidate = atomic_load_explicit(&ring->slots[read_index & ring->mask],
				memory_order_relaxed);
		if (claim && candidate != expected)
			return -EAGAIN;
		if (drop_oldest) {
			if (!atomic_compare_exchange_strong_explicit(&ring->read_index,
					&read_index, read_index + 1u,
					memory_order_acq_rel, memory_order_relaxed))
				continue;
		} else {
			atomic_store_explicit(&ring->read_index, read_index + 1u,
					memory_order_release);
		}
		*value = candidate;
		return 1;
	}
	return claim ? -EAGAIN : 0;
}

int pwao_queue_ring_try_pop(struct pwao_queue_ring *ring, uint64_t *value)
{
	if (ring == NULL || value == NULL || ring->capacity == 0)
		return -EINVAL;
	*value = UINT64_MAX;
	return consume(ring, UINT64_MAX, value);
}

int pwao_queue_ring_try_peek(const struct pwao_queue_ring *ring,
		uint64_t *value)
{
	uint64_t read_index, write_index;

	if (ring == NULL || value == NULL || ring->capacity == 0)
		return -EINVAL;
	*value = UINT64_MAX;
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_acquire);
	if (read_index == write_index)
		return 0;
	*value = atomic_load_explicit(&ring->slots[read_index & ring->mask],
			memory_order_relaxed);
	return *value == UINT64_MAX ? -EPROTO : 1;
}

int pwao_queue_ring_try_claim(struct pwao_queue_ring *ring,
		uint64_t expected_value)
{
	uint64_t value;

	if (ring == NULL || ring->capacity == 0 || expected_value == UINT64_MAX)
		return -EINVAL;
	return consume(ring, expected_value, &value);
}

int pwao_queue_ring_admit(struct pwao_queue_ring *ring, uint64_t value,
		uint64_t *released_value)
{
	uint64_t read_index, write_index;
	int result = PWAO_QUEUE_ADMIT_QUEUED;

	if (released_value == NULL)
		return -EINVAL;
	*released_value = UINT64_MAX;
	if (ring == NULL || ring->capacity == 0 || value == UINT64_MAX)
		return -EINVAL;
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_relaxed);
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	if (write_index - read_index >= ring->capacity) {
		uint64_t candidate;

		if (ring->overflow == PWAO_QUEUE_OVERFLOW_BACKPRESSURE)
			return PWAO_QUEUE_ADMIT_BACKPRESSURE;
		if (ring->overflow == PWAO_QUEUE_OVERFLOW_DROP_NEWEST) {
			*released_value = value;
			return PWAO_QUEUE_ADMIT_DROPPED;
		}
		candidate = atomic_load_explicit(&ring->slots[read_index & ring->mask],
				memory_order_relaxed);
		/* On failure, acquire the consumer's release before slot reuse. */
		if (atomic_compare_exchange_strong_explicit(&ring->read_index,
				&read_index, read_index + 1u,
				memory_order_acq_rel, memory_order_acquire)) {
			*released_value = candidate;
			result = PWAO_QUEUE_ADMIT_REPLACED;
		}
	}
	publish(ring, write_index, value);
	return result;
}

uint32_t pwao_queue_ring_size(const struct pwao_queue_ring *ring)
{
	uint64_t read_index, write_index, size;

	if (ring == NULL || ring->capacity == 0)
		return 0;
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_acquire);
	size = write_index - read_index;
	return size > ring->capacity ? ring->capacity : (uint32_t)size;
}

void pwao_queue_ring_reset(struct pwao_queue_ring *ring)
{
	uint32_t i;

	if (ring == NULL)
		return;
	for (i = 0; i < PWAO_QUEUE_RING_MAX_SLOTS; i++)
		atomic_store_explicit(&ring->slots[i], UINT64_MAX,
				memory_order_relaxed);
	atomic_store_explicit(&ring->read_index, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->write_index, 0, memory_order_relaxed);
}
