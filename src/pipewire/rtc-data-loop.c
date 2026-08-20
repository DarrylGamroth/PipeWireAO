/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/atomic.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

#include "pipewire/context.h"
#include "pipewire/keys.h"
#include "pipewire/log.h"
#include "pipewire/loop.h"
#include "pipewire/rtc-data-loop.h"

PW_LOG_TOPIC_EXTERN(log_data_loop);
#define PW_LOG_TOPIC_DEFAULT log_data_loop

#define START_GATE_CLOSED 0u
#define START_GATE_OPEN 1u

struct pw_rtc_data_loop {
	struct pw_context *context;
	struct pw_loop *loop;
	struct spa_thread_utils *thread_utils;
	struct spa_thread *thread;
	struct spa_hook_list listener_list;

	char *name;
	char *affinity;
	bool reset_on_fork;
	struct pw_rtc_data_loop_config config;
	pw_rtc_data_loop_process_t process;
	void *process_data;

	uint32_t start_gate;
	uint32_t running;
	int result;
};

#define pw_rtc_data_loop_emit(o,m,v,...) \
	spa_hook_list_call(&(o)->listener_list, struct pw_rtc_data_loop_events, \
			m, v, ##__VA_ARGS__)

static inline void rtc_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static int validate_config(const struct pw_rtc_data_loop_config *config)
{
	if (config == NULL || config->version != PW_VERSION_RTC_DATA_LOOP_CONFIG)
		return -EINVAL;
	if (config->idle < PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN ||
			config->idle > PW_RTC_DATA_LOOP_IDLE_HYBRID)
		return -EINVAL;
	if (config->scheduler < PW_RTC_DATA_LOOP_SCHED_OTHER ||
			config->scheduler > PW_RTC_DATA_LOOP_SCHED_FIFO)
		return -EINVAL;
	if (config->scheduler == PW_RTC_DATA_LOOP_SCHED_FIFO) {
		if (config->priority < -1 || config->priority == 0)
			return -EINVAL;
	} else if (config->priority != 0)
		return -EINVAL;
	if (config->idle == PW_RTC_DATA_LOOP_IDLE_HYBRID &&
			config->hybrid_spin_iterations == 0)
		return -EINVAL;
	return 0;
}

static int wait_for_event(struct pw_rtc_data_loop *loop)
{
	int res;

	while (SPA_ATOMIC_LOAD(loop->running)) {
		res = pw_loop_iterate(loop->loop, -1);
		if (res >= 0)
			return 0;
		if (res != -EINTR)
			return res;
	}
	return 0;
}

static void *do_loop(void *data)
{
	struct pw_rtc_data_loop *loop = data;
	uint32_t empty_iterations = 0;
	int res = 0;

	while (SPA_ATOMIC_LOAD(loop->start_gate) == START_GATE_CLOSED)
		rtc_cpu_relax();

	if (!SPA_ATOMIC_LOAD(loop->running))
		return NULL;

	pw_loop_enter(loop->loop);

	while (SPA_ATOMIC_LOAD(loop->running)) {
		res = loop->process(loop->process_data);
		if (SPA_UNLIKELY(res < 0)) {
			SPA_ATOMIC_STORE(loop->result, res);
			SPA_ATOMIC_STORE(loop->running, false);
			break;
		}
		if (res > 0) {
			empty_iterations = 0;
			continue;
		}

		switch (loop->config.idle) {
		case PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN:
			rtc_cpu_relax();
			break;
		case PW_RTC_DATA_LOOP_IDLE_EVENTFD:
			if ((res = wait_for_event(loop)) < 0)
				goto event_error;
			break;
		case PW_RTC_DATA_LOOP_IDLE_HYBRID:
			if (empty_iterations < loop->config.hybrid_spin_iterations) {
				empty_iterations++;
				rtc_cpu_relax();
				break;
			}
			empty_iterations = 0;
			if ((res = wait_for_event(loop)) < 0)
				goto event_error;
			break;
		}
	}

	pw_loop_leave(loop->loop);
	return NULL;

event_error:
	SPA_ATOMIC_STORE(loop->result, res);
	SPA_ATOMIC_STORE(loop->running, false);
	pw_loop_leave(loop->loop);
	return NULL;
}

SPA_EXPORT
struct pw_rtc_data_loop *pw_rtc_data_loop_new(struct pw_context *context,
		const struct spa_dict *props,
		const struct pw_rtc_data_loop_config *config,
		pw_rtc_data_loop_process_t process, void *data)
{
	struct pw_rtc_data_loop *loop;
	const char *str, *loop_name;
	int res;

	if (context == NULL || process == NULL || (res = validate_config(config)) < 0) {
		errno = context == NULL || process == NULL ? EINVAL : -res;
		return NULL;
	}

	loop = calloc(1, sizeof(*loop));
	if (loop == NULL)
		return NULL;

	loop->context = context;
	loop->config = *config;
	loop->process = process;
	loop->process_data = data;
	loop->reset_on_fork = true;
	spa_hook_list_init(&loop->listener_list);

	str = props ? spa_dict_lookup(props, SPA_KEY_THREAD_NAME) : NULL;
	loop->name = strdup(str ? str : "rtc-data-loop");
	if (loop->name == NULL)
		goto error;

	if (props && (str = spa_dict_lookup(props, SPA_KEY_THREAD_AFFINITY)) != NULL) {
		loop->affinity = strdup(str);
		if (loop->affinity == NULL)
			goto error;
	}
	if (props && (str = spa_dict_lookup(props, SPA_KEY_THREAD_RESET_ON_FORK)) != NULL)
		loop->reset_on_fork = spa_atob(str);

	loop->loop = pw_loop_new(props);
	if (loop->loop == NULL)
		goto error;
	loop_name = props ? spa_dict_lookup(props, PW_KEY_LOOP_NAME) : NULL;
	if (pw_loop_set_name(loop->loop, loop_name ? loop_name : loop->name) < 0)
		goto error;

	return loop;

error:
	res = errno;
	if (loop->loop)
		pw_loop_destroy(loop->loop);
	free(loop->affinity);
	free(loop->name);
	free(loop);
	errno = res;
	return NULL;
}

SPA_EXPORT
void pw_rtc_data_loop_add_listener(struct pw_rtc_data_loop *loop,
		struct spa_hook *listener,
		const struct pw_rtc_data_loop_events *events, void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SPA_EXPORT
struct pw_loop *pw_rtc_data_loop_get_loop(struct pw_rtc_data_loop *loop)
{
	return loop->loop;
}

SPA_EXPORT
int pw_rtc_data_loop_start(struct pw_rtc_data_loop *loop)
{
	struct spa_dict_item items[3];
	uint32_t n_items = 0;
	int res;

	if (loop->thread != NULL)
		return -EBUSY;

	loop->thread_utils = pw_context_get_object(loop->context,
			SPA_TYPE_INTERFACE_ThreadUtils);
	if (loop->thread_utils == NULL)
		return -ENOTSUP;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_NAME, loop->name);
	if (loop->affinity)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_AFFINITY,
				loop->affinity);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_RESET_ON_FORK,
			loop->reset_on_fork ? "true" : "false");

	SPA_ATOMIC_STORE(loop->result, 0);
	SPA_ATOMIC_STORE(loop->start_gate, START_GATE_CLOSED);
	SPA_ATOMIC_STORE(loop->running, true);

	loop->thread = spa_thread_utils_create(loop->thread_utils,
			&SPA_DICT_INIT(items, n_items), do_loop, loop);
	if (loop->thread == NULL) {
		res = -errno;
		SPA_ATOMIC_STORE(loop->running, false);
		return res;
	}

	if (loop->config.scheduler == PW_RTC_DATA_LOOP_SCHED_FIFO)
		res = spa_thread_utils_acquire_rt(loop->thread_utils, loop->thread,
				loop->config.priority);
	else
		res = spa_thread_utils_drop_rt(loop->thread_utils, loop->thread);

	if (res < 0) {
		int join_res;

		SPA_ATOMIC_STORE(loop->running, false);
		SPA_ATOMIC_STORE(loop->start_gate, START_GATE_OPEN);
		join_res = spa_thread_utils_join(loop->thread_utils, loop->thread, NULL);
		if (join_res < 0)
			return join_res;
		loop->thread = NULL;
		loop->thread_utils = NULL;
		return res;
	}

	SPA_ATOMIC_STORE(loop->start_gate, START_GATE_OPEN);
	return 0;
}

SPA_EXPORT
int pw_rtc_data_loop_stop(struct pw_rtc_data_loop *loop)
{
	int res, join_res;

	if (loop->thread == NULL)
		return 0;
	if (pw_rtc_data_loop_in_thread(loop))
		return -EDEADLK;

	SPA_ATOMIC_STORE(loop->running, false);
	SPA_ATOMIC_STORE(loop->start_gate, START_GATE_OPEN);

	if (loop->config.idle != PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN)
		pw_loop_invoke(loop->loop, NULL, SPA_ID_INVALID, NULL, 0, false, NULL);

	res = spa_thread_utils_join(loop->thread_utils, loop->thread, NULL);
	if (res < 0)
		return res;
	join_res = SPA_ATOMIC_LOAD(loop->result);
	loop->thread = NULL;
	loop->thread_utils = NULL;
	return join_res;
}

SPA_EXPORT
void pw_rtc_data_loop_exit(struct pw_rtc_data_loop *loop)
{
	SPA_ATOMIC_STORE(loop->running, false);
}

SPA_EXPORT
void pw_rtc_data_loop_destroy(struct pw_rtc_data_loop *loop)
{
	int res;

	if (loop == NULL)
		return;

	res = pw_rtc_data_loop_stop(loop);
	if (loop->thread != NULL) {
		pw_log_error("%p: refusing to destroy RTC data loop after join failed: %s",
				loop, spa_strerror(res));
		return;
	}
	pw_rtc_data_loop_emit(loop, destroy, 0);
	spa_hook_list_clean(&loop->listener_list);
	pw_loop_destroy(loop->loop);
	free(loop->affinity);
	free(loop->name);
	free(loop);
}

SPA_EXPORT
bool pw_rtc_data_loop_in_thread(struct pw_rtc_data_loop *loop)
{
	return loop->thread != NULL &&
		pthread_equal((pthread_t)loop->thread, pthread_self());
}

SPA_EXPORT
struct spa_thread *pw_rtc_data_loop_get_thread(struct pw_rtc_data_loop *loop)
{
	return loop->thread;
}

SPA_EXPORT
int pw_rtc_data_loop_get_result(struct pw_rtc_data_loop *loop)
{
	return SPA_ATOMIC_LOAD(loop->result);
}

SPA_EXPORT
bool pw_rtc_data_loop_is_running(struct pw_rtc_data_loop *loop)
{
	return SPA_ATOMIC_LOAD(loop->running);
}
