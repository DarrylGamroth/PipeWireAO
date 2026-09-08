/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "../modules/module-ndarray-filter-chain-parameter.h"

struct pw_buffer {
	unsigned int value;
};

#define STRESS_ITERATIONS 10000u

static void test_ordered_handoffs(void)
{
	struct ndarray_parameter_handoff first, second;
	struct pw_buffer first_buffer = { .value = 1 };
	struct pw_buffer next_buffer = { .value = 2 };
	struct pw_buffer second_buffer = { .value = 3 };

	ndarray_parameter_handoff_init(&first);
	ndarray_parameter_handoff_init(&second);

	assert(ndarray_parameter_handoff_schedule(&first, &first_buffer));
	assert(ndarray_parameter_handoff_claim(&first));
	assert(!ndarray_parameter_handoff_claim(&first));
	ndarray_parameter_handoff_complete(&first);

	/* A wake for another port must not re-enter completed storage while the
	 * data-loop owner has not returned it yet. */
	assert(ndarray_parameter_handoff_schedule(&second, &second_buffer));
	assert(!ndarray_parameter_handoff_claim(&first));
	assert(ndarray_parameter_handoff_claim(&second));
	assert(ndarray_parameter_handoff_pending(&first) == &first_buffer);
	assert(!ndarray_parameter_handoff_schedule(&first, &next_buffer));
	assert(ndarray_parameter_handoff_take_completed(&first) == &first_buffer);
	first_buffer.value = 99;
	assert(!ndarray_parameter_handoff_claim(&first));

	assert(ndarray_parameter_handoff_schedule(&first, &next_buffer));
	assert(ndarray_parameter_handoff_claim(&first));
	/* A failed wake cannot cancel storage already claimed through another
	 * port's wake. */
	assert(!ndarray_parameter_handoff_cancel_schedule(&first));
	assert(ndarray_parameter_handoff_pending(&first) == &next_buffer);
	ndarray_parameter_handoff_mark_retry(&first);
	assert(ndarray_parameter_handoff_rearm_retry(&first));
	/* Wake failure before a claim restores the retry. */
	ndarray_parameter_handoff_restore_retry(&first);
	assert(!ndarray_parameter_handoff_claim(&first));
	assert(ndarray_parameter_handoff_rearm_retry(&first));
	assert(ndarray_parameter_handoff_claim(&first));
	/* Wake failure after a claim must not schedule a duplicate retry. */
	ndarray_parameter_handoff_restore_retry(&first);
	assert(!ndarray_parameter_handoff_rearm_retry(&first));
	assert(ndarray_parameter_handoff_cancel(&first) == &next_buffer);
	assert(ndarray_parameter_handoff_pending(&first) == NULL);

	ndarray_parameter_handoff_complete(&second);
	assert(ndarray_parameter_handoff_take_completed(&second) == &second_buffer);
	assert(ndarray_parameter_handoff_schedule(&second, &second_buffer));
	assert(ndarray_parameter_handoff_cancel_schedule(&second));
	assert(ndarray_parameter_handoff_pending(&second) == NULL);
	assert(!ndarray_parameter_handoff_claim(&second));
}

enum stress_mode {
	STRESS_CANCEL_SCHEDULE,
	STRESS_RESTORE_RETRY,
};

struct handoff_stress {
	struct ndarray_parameter_handoff handoff;
	struct pw_buffer buffers[2];
	_Atomic unsigned int iteration;
	_Atomic unsigned int release;
	_Atomic unsigned int race_done;
	_Atomic unsigned int worker_done;
	_Atomic unsigned int cancellation_done;
	_Atomic enum stress_mode mode;
	_Atomic bool worker_claimed;
	_Atomic bool cancellation_succeeded;
	_Atomic(struct pw_buffer *) claimed_buffer;
};

/* Keep the worker and cancellation threads alive so each iteration races only
 * the handoff operations, rather than thread creation and teardown. */
static void wait_for_at_least(_Atomic unsigned int *value, unsigned int wanted)
{
	while (atomic_load_explicit(value, memory_order_acquire) < wanted)
		sched_yield();
}

static void *stress_worker(void *data)
{
	struct handoff_stress *stress = data;
	unsigned int i;

	for (i = 1; i <= STRESS_ITERATIONS; i++) {
		bool claimed;

		wait_for_at_least(&stress->iteration, i);
		if ((i & 3u) == 0)
			sched_yield();
		claimed = ndarray_parameter_handoff_claim(&stress->handoff);
		atomic_store_explicit(&stress->worker_claimed, claimed,
				memory_order_release);
		if (claimed)
			atomic_store_explicit(&stress->claimed_buffer,
					ndarray_parameter_handoff_pending(&stress->handoff),
					memory_order_release);
		atomic_fetch_add_explicit(&stress->race_done, 1,
				memory_order_acq_rel);

		wait_for_at_least(&stress->release, i);
		if (claimed)
			ndarray_parameter_handoff_complete(&stress->handoff);
		atomic_store_explicit(&stress->worker_done, i,
				memory_order_release);
	}
	return NULL;
}

static void *stress_cancellation(void *data)
{
	struct handoff_stress *stress = data;
	unsigned int i;

	for (i = 1; i <= STRESS_ITERATIONS; i++) {
		enum stress_mode mode;

		wait_for_at_least(&stress->iteration, i);
		if ((i & 3u) == 1)
			sched_yield();
		mode = atomic_load_explicit(&stress->mode, memory_order_acquire);
		if (mode == STRESS_CANCEL_SCHEDULE) {
			bool cancelled = ndarray_parameter_handoff_cancel_schedule(
					&stress->handoff);
			atomic_store_explicit(&stress->cancellation_succeeded,
					cancelled, memory_order_release);
		} else {
			ndarray_parameter_handoff_restore_retry(&stress->handoff);
		}
		atomic_fetch_add_explicit(&stress->race_done, 1,
				memory_order_acq_rel);

		wait_for_at_least(&stress->release, i);
		atomic_store_explicit(&stress->cancellation_done, i,
				memory_order_release);
	}
	return NULL;
}

static void test_concurrent_handoffs(void)
{
	struct handoff_stress stress = { 0 };
	pthread_t worker, cancellation;
	unsigned int i;

	ndarray_parameter_handoff_init(&stress.handoff);
	atomic_init(&stress.iteration, 0);
	atomic_init(&stress.release, 0);
	atomic_init(&stress.race_done, 0);
	atomic_init(&stress.worker_done, 0);
	atomic_init(&stress.cancellation_done, 0);
	atomic_init(&stress.mode, STRESS_CANCEL_SCHEDULE);
	atomic_init(&stress.worker_claimed, false);
	atomic_init(&stress.cancellation_succeeded, false);
	atomic_init(&stress.claimed_buffer, NULL);
	assert(pthread_create(&worker, NULL, stress_worker, &stress) == 0);
	assert(pthread_create(&cancellation, NULL,
			stress_cancellation, &stress) == 0);

	for (i = 1; i <= STRESS_ITERATIONS; i++) {
		struct pw_buffer *buffer = &stress.buffers[i & 1u];
		bool claimed;
		enum stress_mode mode = (i & 1u) == 0 ?
			STRESS_CANCEL_SCHEDULE : STRESS_RESTORE_RETRY;

		buffer->value = i;
		atomic_store_explicit(&stress.mode, mode, memory_order_relaxed);
		atomic_store_explicit(&stress.worker_claimed, false,
				memory_order_relaxed);
		atomic_store_explicit(&stress.cancellation_succeeded, false,
				memory_order_relaxed);
		atomic_store_explicit(&stress.claimed_buffer, NULL,
				memory_order_relaxed);
		assert(ndarray_parameter_handoff_schedule(&stress.handoff, buffer));
		if (mode == STRESS_RESTORE_RETRY) {
			/* Model an update that returned -EBUSY before its retry wake. */
			assert(ndarray_parameter_handoff_claim(&stress.handoff));
			ndarray_parameter_handoff_mark_retry(&stress.handoff);
			assert(ndarray_parameter_handoff_rearm_retry(&stress.handoff));
		}

		atomic_store_explicit(&stress.iteration, i, memory_order_release);
		wait_for_at_least(&stress.race_done, 2 * i);
		claimed = atomic_load_explicit(&stress.worker_claimed,
				memory_order_acquire);

		if (mode == STRESS_CANCEL_SCHEDULE) {
			bool cancelled = atomic_load_explicit(
					&stress.cancellation_succeeded,
					memory_order_acquire);
			assert(claimed != cancelled);
			if (claimed) {
				assert(atomic_load_explicit(&stress.claimed_buffer,
						memory_order_acquire) == buffer);
				assert(ndarray_parameter_handoff_pending(
						&stress.handoff) == buffer);
				assert(ndarray_parameter_handoff_take_completed(
						&stress.handoff) == NULL);
			} else {
				assert(ndarray_parameter_handoff_pending(
						&stress.handoff) == NULL);
			}
		} else if (claimed) {
			assert(atomic_load_explicit(&stress.claimed_buffer,
					memory_order_acquire) == buffer);
			assert(!ndarray_parameter_handoff_rearm_retry(
					&stress.handoff));
			assert(ndarray_parameter_handoff_take_completed(
					&stress.handoff) == NULL);
		} else {
			assert(ndarray_parameter_handoff_rearm_retry(&stress.handoff));
			assert(ndarray_parameter_handoff_claim(&stress.handoff));
			assert(!ndarray_parameter_handoff_claim(&stress.handoff));
			ndarray_parameter_handoff_complete(&stress.handoff);
		}

		atomic_store_explicit(&stress.release, i, memory_order_release);
		/* Do not reclaim or reuse this iteration's state until both racing
		 * threads have left it. */
		wait_for_at_least(&stress.worker_done, i);
		wait_for_at_least(&stress.cancellation_done, i);
		if (mode == STRESS_RESTORE_RETRY || claimed) {
			assert(ndarray_parameter_handoff_take_completed(
					&stress.handoff) == buffer);
			assert(buffer->value == i);
			buffer->value = 0;
			assert(ndarray_parameter_handoff_take_completed(
					&stress.handoff) == NULL);
		}
		assert(ndarray_parameter_handoff_pending(&stress.handoff) == NULL);
	}

	assert(pthread_join(worker, NULL) == 0);
	assert(pthread_join(cancellation, NULL) == 0);
	assert(ndarray_parameter_handoff_cancel(&stress.handoff) == NULL);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	test_ordered_handoffs();
	test_concurrent_handoffs();
	return 0;
}
