/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_MODULE_NDARRAY_FILTER_CHAIN_PARAMETER_H
#define PIPEWIREAO_MODULE_NDARRAY_FILTER_CHAIN_PARAMETER_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

struct pw_buffer;

struct ndarray_parameter_handoff {
	_Atomic(struct pw_buffer *) pending;
	_Atomic bool scheduled;
	_Atomic bool retry;
	_Atomic bool completed;
};

static inline void ndarray_parameter_handoff_init(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_init(&handoff->pending, NULL);
	atomic_init(&handoff->scheduled, false);
	atomic_init(&handoff->retry, false);
	atomic_init(&handoff->completed, false);
}

static inline struct pw_buffer *ndarray_parameter_handoff_pending(
		const struct ndarray_parameter_handoff *handoff)
{
	return atomic_load_explicit(&handoff->pending, memory_order_acquire);
}

static inline bool ndarray_parameter_handoff_schedule(
		struct ndarray_parameter_handoff *handoff, struct pw_buffer *buffer)
{
	struct pw_buffer *expected = NULL;

	if (!atomic_compare_exchange_strong_explicit(&handoff->pending,
			&expected, buffer, memory_order_release,
			memory_order_relaxed))
		return false;
	atomic_store_explicit(&handoff->scheduled, true, memory_order_release);
	return true;
}

static inline bool ndarray_parameter_handoff_claim(
		struct ndarray_parameter_handoff *handoff)
{
	return atomic_exchange_explicit(&handoff->scheduled, false,
			memory_order_acq_rel);
}

static inline void ndarray_parameter_handoff_cancel_schedule(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_store_explicit(&handoff->scheduled, false, memory_order_release);
	atomic_store_explicit(&handoff->pending, NULL, memory_order_release);
}

static inline void ndarray_parameter_handoff_mark_retry(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_store_explicit(&handoff->retry, true, memory_order_release);
}

static inline bool ndarray_parameter_handoff_rearm_retry(
		struct ndarray_parameter_handoff *handoff)
{
	if (!atomic_exchange_explicit(&handoff->retry, false,
			memory_order_acq_rel))
		return false;
	atomic_store_explicit(&handoff->scheduled, true, memory_order_release);
	return true;
}

static inline void ndarray_parameter_handoff_restore_retry(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_store_explicit(&handoff->scheduled, false, memory_order_release);
	atomic_store_explicit(&handoff->retry, true, memory_order_release);
}

static inline void ndarray_parameter_handoff_complete(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_store_explicit(&handoff->completed, true, memory_order_release);
}

static inline struct pw_buffer *ndarray_parameter_handoff_take_completed(
		struct ndarray_parameter_handoff *handoff)
{
	if (!atomic_exchange_explicit(&handoff->completed, false,
			memory_order_acq_rel))
		return NULL;
	return atomic_exchange_explicit(&handoff->pending, NULL,
			memory_order_acq_rel);
}

static inline struct pw_buffer *ndarray_parameter_handoff_cancel(
		struct ndarray_parameter_handoff *handoff)
{
	atomic_store_explicit(&handoff->scheduled, false, memory_order_release);
	atomic_store_explicit(&handoff->retry, false, memory_order_release);
	atomic_store_explicit(&handoff->completed, false, memory_order_release);
	return atomic_exchange_explicit(&handoff->pending, NULL,
			memory_order_acq_rel);
}

#endif /* PIPEWIREAO_MODULE_NDARRAY_FILTER_CHAIN_PARAMETER_H */
