/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_RTC_DATA_LOOP_H
#define PIPEWIRE_RTC_DATA_LOOP_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/support/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup pw_rtc_data_loop RTC Data Loop
 *
 * \brief PipeWireAO real-time-control duty-cycle loop
 *
 * An RTC data loop repeatedly calls one bounded process function. It uses the
 * ThreadUtils installed on its PipeWire context by module-rt for thread
 * creation and scheduling. Protocol, negotiation, topology, and other control
 * events remain the responsibility of the context's main loop.
 */

/**
 * \addtogroup pw_rtc_data_loop
 * \{
 */

struct pw_context;
struct pw_loop;
struct pw_rtc_data_loop;

/** Receiver idle behavior after the process function reports no work. */
enum pw_rtc_data_loop_idle {
	/** Poll again immediately. Requires a reserved physical core. */
	PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN,
	/** Block in the embedded PipeWire loop until a registered source wakes it. */
	PW_RTC_DATA_LOOP_IDLE_EVENTFD,
	/** Busy-spin for a fixed iteration count, then block like Eventfd. */
	PW_RTC_DATA_LOOP_IDLE_HYBRID,
};

/** Scheduling policy requested through module-rt ThreadUtils. */
enum pw_rtc_data_loop_scheduler {
	PW_RTC_DATA_LOOP_SCHED_OTHER,
	PW_RTC_DATA_LOOP_SCHED_FIFO,
};

/** RTC data-loop configuration. */
struct pw_rtc_data_loop_config {
#define PW_VERSION_RTC_DATA_LOOP_CONFIG 0
	uint32_t version;
	enum pw_rtc_data_loop_idle idle;
	enum pw_rtc_data_loop_scheduler scheduler;
	/** SCHED_FIFO priority. -1 selects the module-rt configured priority. */
	int priority;
	/** Busy-spin iterations before Hybrid blocks. Must be nonzero. */
	uint32_t hybrid_spin_iterations;
};

/** Loop events, used with \ref pw_rtc_data_loop_add_listener. */
struct pw_rtc_data_loop_events {
#define PW_VERSION_RTC_DATA_LOOP_EVENTS 0
	uint32_t version;
	void (*destroy) (void *data);
};

/**
 * Execute one bounded RTC duty cycle.
 *
 * Return a positive work count when work was completed, zero when no work was
 * available, or a negative errno-style value to terminate the loop. The
 * function runs on the RTC thread and must be real-time safe. It must not
 * allocate, perform file I/O, wait for a peer, or retain unbounded work.
 */
typedef int (*pw_rtc_data_loop_process_t) (void *data);

/**
 * Create an RTC data loop.
 *
 * The context and its module-rt ThreadUtils must outlive the data loop. The
 * properties may contain SPA_KEY_THREAD_NAME, SPA_KEY_THREAD_AFFINITY, and
 * SPA_KEY_THREAD_RESET_ON_FORK. Notification sources for Eventfd or Hybrid
 * must be installed on \ref pw_rtc_data_loop_get_loop before starting.
 */
struct pw_rtc_data_loop *
pw_rtc_data_loop_new(struct pw_context *context,
		const struct spa_dict *props,
		const struct pw_rtc_data_loop_config *config,
		pw_rtc_data_loop_process_t process, void *data);

void pw_rtc_data_loop_add_listener(struct pw_rtc_data_loop *loop,
		struct spa_hook *listener,
		const struct pw_rtc_data_loop_events *events,
		void *data);

/** Get the embedded event loop used by Eventfd and Hybrid idle policies. */
struct pw_loop *pw_rtc_data_loop_get_loop(struct pw_rtc_data_loop *loop);

/** Destroy the loop. The caller must serialize lifecycle operations. */
void pw_rtc_data_loop_destroy(struct pw_rtc_data_loop *loop);

/** Start the RTC thread and apply its requested scheduling policy. */
int pw_rtc_data_loop_start(struct pw_rtc_data_loop *loop);

/** Stop and join the RTC thread. Must not be called from the RTC thread. */
int pw_rtc_data_loop_stop(struct pw_rtc_data_loop *loop);

/** Request exit without joining. Safe to call from the process function. */
void pw_rtc_data_loop_exit(struct pw_rtc_data_loop *loop);

/** Check whether the current thread is this RTC data loop's thread. */
bool pw_rtc_data_loop_in_thread(struct pw_rtc_data_loop *loop);

/** Get the thread object, or NULL when the loop has not been started. */
struct spa_thread *pw_rtc_data_loop_get_thread(struct pw_rtc_data_loop *loop);

/** Get the terminal process or event-loop result. */
int pw_rtc_data_loop_get_result(struct pw_rtc_data_loop *loop);

/** Return true while the RTC duty-cycle loop is running. */
bool pw_rtc_data_loop_is_running(struct pw_rtc_data_loop *loop);

/**
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTC_DATA_LOOP_H */
