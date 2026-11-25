/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_QUEUE_H
#define PIPEWIREAO_QUEUE_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include <spa/utils/defs.h>

#define PWAO_QUEUE_RING_MAX_SLOTS 64u
#define PWAO_QUEUE_ID_PROPERTY "pipewireao.queue.id"

enum pwao_queue_overflow {
	PWAO_QUEUE_OVERFLOW_BACKPRESSURE,
	PWAO_QUEUE_OVERFLOW_DROP_OLDEST,
	PWAO_QUEUE_OVERFLOW_DROP_NEWEST,
};

struct pwao_queue_ring {
	_Alignas(SPA_CACHE_LINE_SIZE) uint32_t capacity;
	uint32_t mask;
	enum pwao_queue_overflow overflow;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint64_t read_index;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint64_t write_index;
	_Alignas(SPA_CACHE_LINE_SIZE)
	_Atomic uint64_t slots[PWAO_QUEUE_RING_MAX_SLOTS];
};

_Static_assert(_Generic((uint64_t)0, unsigned long: ATOMIC_LONG_LOCK_FREE,
		unsigned long long: ATOMIC_LLONG_LOCK_FREE,
		unsigned int: ATOMIC_INT_LOCK_FREE, default: 0) == 2,
		"queue requires lock-free uint64_t atomics");
_Static_assert((PWAO_QUEUE_RING_MAX_SLOTS & (PWAO_QUEUE_RING_MAX_SLOTS - 1u)) == 0,
		"queue maximum slot count must be a power of two");
_Static_assert(offsetof(struct pwao_queue_ring, read_index) %
		SPA_CACHE_LINE_SIZE == 0, "queue read index alignment");
_Static_assert(offsetof(struct pwao_queue_ring, write_index) %
		SPA_CACHE_LINE_SIZE == 0, "queue write index alignment");
_Static_assert(offsetof(struct pwao_queue_ring, slots) %
		SPA_CACHE_LINE_SIZE == 0, "queue slot alignment");
_Static_assert(sizeof(struct pwao_queue_ring) % SPA_CACHE_LINE_SIZE == 0,
		"queue ring size alignment");

enum pwao_queue_admit_result {
	PWAO_QUEUE_ADMIT_QUEUED = 1,
	PWAO_QUEUE_ADMIT_REPLACED,
	PWAO_QUEUE_ADMIT_DROPPED,
	PWAO_QUEUE_ADMIT_BACKPRESSURE,
};

/* Set capacity and overflow policy before publishing the ring to its endpoints. */
int pwao_queue_ring_init(struct pwao_queue_ring *ring, uint32_t capacity,
		enum pwao_queue_overflow overflow);

/* These functions are valid only for one producer and one consumer.
 * try_push always returns 0 at capacity, regardless of the overflow policy. */
int pwao_queue_ring_try_push(struct pwao_queue_ring *ring, uint64_t value);
int pwao_queue_ring_try_pop(struct pwao_queue_ring *ring, uint64_t *value);

/*
 * Inspect the oldest value without claiming it, then claim it only if it is
 * still the oldest value.  The conditional claim lets a consumer wait for a
 * value-specific resource without removing and reordering that value.  A
 * concurrent drop-oldest producer can make try_claim return -EAGAIN.
 */
int pwao_queue_ring_try_peek(const struct pwao_queue_ring *ring,
		uint64_t *value);
int pwao_queue_ring_try_claim(struct pwao_queue_ring *ring,
		uint64_t expected_value);

/*
 * Apply the ring's overflow policy without waiting. released_value identifies
 * the displaced queued value or dropped arriving value, otherwise UINT64_MAX.
 */
int pwao_queue_ring_admit(struct pwao_queue_ring *ring, uint64_t value,
		uint64_t *released_value);

uint32_t pwao_queue_ring_size(const struct pwao_queue_ring *ring);

/* The caller must quiesce both ring endpoints before reset. */
void pwao_queue_ring_reset(struct pwao_queue_ring *ring);

#endif
