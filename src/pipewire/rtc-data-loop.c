/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdlib.h>

#include <spa/utils/atomic.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

#include "pipewire/context.h"
#include "pipewire/data-loop.h"
#include "pipewire/keys.h"
#include "pipewire/log.h"
#include "pipewire/loop.h"
#include "pipewire/private.h"
#include "pipewire/rtc-data-loop.h"

PW_LOG_TOPIC_EXTERN(log_data_loop);
#define PW_LOG_TOPIC_DEFAULT log_data_loop

struct pw_rtc_data_loop {
	struct pw_context *context;
	struct pw_data_loop *data_loop;
	struct spa_hook_list listener_list;

	struct pw_rtc_data_loop_config config;
	pw_rtc_data_loop_process_t process;
	void *process_data;

	int result;
};

#define pw_rtc_data_loop_emit(o,m,v,...) \
	spa_hook_list_call(&(o)->listener_list, struct pw_rtc_data_loop_events, \
			m, v, ##__VA_ARGS__)

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

	while (pw_data_loop_is_running(loop->data_loop)) {
		res = pw_data_loop_wait(loop->data_loop, -1);
		if (res >= 0)
			return 0;
		if (res == -ECANCELED)
			return 0;
		if (res != -EINTR)
			return res;
	}
	return 0;
}

static void *do_loop(void *data)
{
	struct pw_rtc_data_loop *loop = data;
	struct pw_loop *pw_loop = pw_data_loop_get_loop(loop->data_loop);
	uint32_t empty_iterations = 0;
	int res = 0;

	pw_loop_enter(pw_loop);

	while (pw_data_loop_is_running(loop->data_loop)) {
		res = loop->process(loop->process_data);
		if (SPA_UNLIKELY(res < 0)) {
			SPA_ATOMIC_STORE(loop->result, res);
			pw_data_loop_exit(loop->data_loop);
			break;
		}
		if (res > 0) {
			empty_iterations = 0;
			continue;
		}

		switch (loop->config.idle) {
		case PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN:
			pw_data_loop_relax();
			break;
		case PW_RTC_DATA_LOOP_IDLE_EVENTFD:
			if ((res = wait_for_event(loop)) < 0)
				goto event_error;
			break;
		case PW_RTC_DATA_LOOP_IDLE_HYBRID:
			if (empty_iterations < loop->config.hybrid_spin_iterations) {
				empty_iterations++;
				pw_data_loop_relax();
				break;
			}
			empty_iterations = 0;
			if ((res = wait_for_event(loop)) < 0)
				goto event_error;
			break;
		}
	}

	pw_loop_leave(pw_loop);
	return NULL;

event_error:
	SPA_ATOMIC_STORE(loop->result, res);
	pw_data_loop_exit(loop->data_loop);
	pw_loop_leave(pw_loop);
	return NULL;
}

SPA_EXPORT
struct pw_rtc_data_loop *pw_rtc_data_loop_new(struct pw_context *context,
		const struct spa_dict *props,
		const struct pw_rtc_data_loop_config *config,
		pw_rtc_data_loop_process_t process, void *data)
{
	struct pw_rtc_data_loop *loop;
	const char *loop_name, *thread_name;
	enum pw_data_loop_rt_policy policy;
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
	spa_hook_list_init(&loop->listener_list);

	loop->data_loop = pw_data_loop_new(props);
	if (loop->data_loop == NULL)
		goto error;
	loop_name = props ? spa_dict_lookup(props, PW_KEY_LOOP_NAME) : NULL;
	thread_name = props ? spa_dict_lookup(props, SPA_KEY_THREAD_NAME) : NULL;
	if (loop_name == NULL && thread_name == NULL)
		loop_name = "rtc-data-loop";
	if (loop_name != NULL && pw_loop_set_name(
			pw_data_loop_get_loop(loop->data_loop), loop_name) < 0)
		goto error;
	if ((res = pw_data_loop_set_runner(loop->data_loop, do_loop, loop,
			config->idle != PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN)) < 0)
		goto error_result;
	policy = config->scheduler == PW_RTC_DATA_LOOP_SCHED_FIFO ?
		PW_DATA_LOOP_RT_POLICY_FIFO : PW_DATA_LOOP_RT_POLICY_OTHER;
	if ((res = pw_data_loop_set_rt_policy(loop->data_loop, policy,
			config->priority, true)) < 0)
		goto error_result;

	return loop;

error_result:
	errno = -res;
error:
	res = errno;
	if (loop->data_loop)
		pw_data_loop_destroy(loop->data_loop);
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
	return pw_data_loop_get_loop(loop->data_loop);
}

SPA_EXPORT
int pw_rtc_data_loop_start(struct pw_rtc_data_loop *loop)
{
	struct spa_thread_utils *thread_utils;
	int res;

	if (pw_data_loop_get_thread(loop->data_loop) != NULL)
		return -EBUSY;

	thread_utils = pw_context_get_object(loop->context,
			SPA_TYPE_INTERFACE_ThreadUtils);
	if (thread_utils == NULL)
		return -ENOTSUP;

	SPA_ATOMIC_STORE(loop->result, 0);
	pw_data_loop_set_thread_utils(loop->data_loop, thread_utils);
	res = pw_data_loop_start(loop->data_loop);
	if (res < 0 && pw_data_loop_get_thread(loop->data_loop) == NULL)
		pw_data_loop_set_thread_utils(loop->data_loop, NULL);
	return res;
}

SPA_EXPORT
int pw_rtc_data_loop_stop(struct pw_rtc_data_loop *loop)
{
	int res, process_res;

	if (pw_data_loop_get_thread(loop->data_loop) == NULL)
		return 0;
	if (pw_rtc_data_loop_in_thread(loop))
		return -EDEADLK;

	res = pw_data_loop_stop(loop->data_loop);
	if (pw_data_loop_get_thread(loop->data_loop) == NULL)
		pw_data_loop_set_thread_utils(loop->data_loop, NULL);
	process_res = SPA_ATOMIC_LOAD(loop->result);
	return res < 0 ? res : process_res;
}

SPA_EXPORT
void pw_rtc_data_loop_exit(struct pw_rtc_data_loop *loop)
{
	pw_data_loop_exit(loop->data_loop);
}

SPA_EXPORT
void pw_rtc_data_loop_destroy(struct pw_rtc_data_loop *loop)
{
	int res;

	if (loop == NULL)
		return;

	res = pw_rtc_data_loop_stop(loop);
	if (pw_data_loop_get_thread(loop->data_loop) != NULL) {
		pw_log_error("%p: refusing to destroy RTC data loop after join failed: %s",
				loop, spa_strerror(res));
		return;
	}
	pw_rtc_data_loop_emit(loop, destroy, 0);
	spa_hook_list_clean(&loop->listener_list);
	pw_data_loop_destroy(loop->data_loop);
	free(loop);
}

SPA_EXPORT
bool pw_rtc_data_loop_in_thread(struct pw_rtc_data_loop *loop)
{
	return pw_data_loop_in_thread(loop->data_loop);
}

SPA_EXPORT
struct spa_thread *pw_rtc_data_loop_get_thread(struct pw_rtc_data_loop *loop)
{
	return pw_data_loop_get_thread(loop->data_loop);
}

SPA_EXPORT
int pw_rtc_data_loop_get_result(struct pw_rtc_data_loop *loop)
{
	return SPA_ATOMIC_LOAD(loop->result);
}

SPA_EXPORT
bool pw_rtc_data_loop_is_running(struct pw_rtc_data_loop *loop)
{
	return pw_data_loop_is_running(loop->data_loop);
}
