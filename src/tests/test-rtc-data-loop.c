/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl-link.h>
#include <pipewire/impl-module.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>

#include <spa/buffer/meta.h>
#include <spa/node/utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/atomic.h>

struct test_thread_utils {
	struct spa_thread_utils utils;
	uint32_t creates;
	uint32_t joins;
	uint32_t acquires;
	uint32_t drops;
	int scheduler_result;
	int requested_priority;
};

static struct spa_thread *test_create(void *object, const struct spa_dict *props,
		void *(*start)(void *), void *data)
{
	struct test_thread_utils *utils = object;
	SPA_ATOMIC_INC(utils->creates);
	return spa_thread_utils_create(pw_thread_utils_get(), props, start, data);
}

static int test_join(void *object, struct spa_thread *thread, void **retval)
{
	struct test_thread_utils *utils = object;
	SPA_ATOMIC_INC(utils->joins);
	return spa_thread_utils_join(pw_thread_utils_get(), thread, retval);
}

static int test_get_rt_range(void *object, const struct spa_dict *props,
		int *min, int *max)
{
	if (min)
		*min = 1;
	if (max)
		*max = 99;
	return 0;
}

static int test_acquire_rt(void *object, struct spa_thread *thread, int priority)
{
	struct test_thread_utils *utils = object;
	SPA_ATOMIC_INC(utils->acquires);
	SPA_ATOMIC_STORE(utils->requested_priority, priority);
	return utils->scheduler_result;
}

static int test_drop_rt(void *object, struct spa_thread *thread)
{
	struct test_thread_utils *utils = object;
	struct sched_param param = { 0 };
	int old_policy, policy = SCHED_OTHER, res;

	SPA_ATOMIC_INC(utils->drops);
	if (utils->scheduler_result < 0)
		return utils->scheduler_result;
	res = pthread_getschedparam((pthread_t)thread, &old_policy, &param);
	if (res != 0)
		return -res;
	param.sched_priority = 0;
#ifdef SCHED_RESET_ON_FORK
	if ((old_policy & SCHED_RESET_ON_FORK) != 0)
		policy |= SCHED_RESET_ON_FORK;
#endif
	res = pthread_setschedparam((pthread_t)thread, policy, &param);
	return res == 0 ? 0 : -res;
}

static const struct spa_thread_utils_methods test_thread_utils_methods = {
	SPA_VERSION_THREAD_UTILS_METHODS,
	.create = test_create,
	.join = test_join,
	.get_rt_range = test_get_rt_range,
	.acquire_rt = test_acquire_rt,
	.drop_rt = test_drop_rt,
};

static void init_thread_utils(struct test_thread_utils *utils)
{
	spa_zero(*utils);
	utils->utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&test_thread_utils_methods, utils);
}

struct fixture {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct test_thread_utils thread_utils;
};

static void fixture_init(struct fixture *fixture, bool install_thread_utils)
{
	spa_zero(*fixture);
	fixture->main_loop = pw_main_loop_new(NULL);
	spa_assert_se(fixture->main_loop != NULL);
	fixture->context = pw_context_new(
			pw_main_loop_get_loop(fixture->main_loop), NULL, 0);
	spa_assert_se(fixture->context != NULL);
	if (install_thread_utils) {
		init_thread_utils(&fixture->thread_utils);
		spa_assert_se(pw_context_set_object(fixture->context,
				SPA_TYPE_INTERFACE_ThreadUtils,
				&fixture->thread_utils.utils) == 0);
	} else
		spa_assert_se(pw_context_set_object(fixture->context,
				SPA_TYPE_INTERFACE_ThreadUtils, NULL) == 0);
}

static void fixture_init_data_loops(struct fixture *fixture,
		const char *data_loops)
{
	struct pw_properties *properties;

	spa_zero(*fixture);
	fixture->main_loop = pw_main_loop_new(NULL);
	spa_assert_se(fixture->main_loop != NULL);
	properties = pw_properties_new("context.data-loops", data_loops, NULL);
	spa_assert_se(properties != NULL);
	fixture->context = pw_context_new(
			pw_main_loop_get_loop(fixture->main_loop), properties, 0);
	spa_assert_se(fixture->context != NULL);
}

static void fixture_clear(struct fixture *fixture)
{
	pw_context_destroy(fixture->context);
	pw_main_loop_destroy(fixture->main_loop);
}

static void wait_until_at_least(uint32_t *value, uint32_t target)
{
	struct timespec start, now;
	uint64_t elapsed;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (SPA_ATOMIC_LOAD(*value) < target) {
		sched_yield();
		spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * SPA_NSEC_PER_SEC +
			(uint64_t)(now.tv_nsec - start.tv_nsec);
		spa_assert_se(elapsed < 2 * SPA_NSEC_PER_SEC);
	}
}

struct busy_data {
	struct pw_rtc_data_loop *loop;
	uint32_t calls;
	uint32_t ran_in_thread;
	int observed_cpu;
};

static int process_busy(void *data)
{
	struct busy_data *busy = data;
	if (pw_rtc_data_loop_in_thread(busy->loop))
		SPA_ATOMIC_STORE(busy->ran_in_thread, true);
	SPA_ATOMIC_STORE(busy->observed_cpu, sched_getcpu());
	SPA_ATOMIC_INC(busy->calls);
	return 0;
}

static void test_busy_spin_lifecycle(void)
{
	struct fixture fixture;
	struct busy_data data = { .observed_cpu = -1 };
	struct spa_dict_item items[2];
	char affinity[32];
	int cpu;
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_OTHER,
	};
	struct pw_rtc_data_loop *loop;

	fixture_init(&fixture, true);
	cpu = sched_getcpu();
	spa_assert_se(cpu >= 0);
	spa_scnprintf(affinity, sizeof(affinity), "[ %d ]", cpu);
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_NAME, "rtc-test");
	items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_AFFINITY, affinity);
	loop = pw_rtc_data_loop_new(fixture.context,
			&SPA_DICT_INIT(items, SPA_N_ELEMENTS(items)), &config,
			process_busy, &data);
	spa_assert_se(loop != NULL);
	data.loop = loop;

	spa_assert_se(pw_rtc_data_loop_start(loop) == 0);
	wait_until_at_least(&data.calls, 1000);
	spa_assert_se(SPA_ATOMIC_LOAD(data.ran_in_thread));
	spa_assert_se(SPA_ATOMIC_LOAD(data.observed_cpu) == cpu);
	spa_assert_se(pw_rtc_data_loop_is_running(loop));
	spa_assert_se(pw_rtc_data_loop_get_thread(loop) != NULL);
	spa_assert_se(pw_rtc_data_loop_stop(loop) == 0);
	spa_assert_se(!pw_rtc_data_loop_is_running(loop));
	spa_assert_se(fixture.thread_utils.creates == 1);
	spa_assert_se(fixture.thread_utils.drops == 1);
	spa_assert_se(fixture.thread_utils.acquires == 0);
	spa_assert_se(fixture.thread_utils.joins == 1);

	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);
}

struct event_data {
	int fd;
	uint32_t ready;
	uint32_t processed;
};

static void on_event(struct spa_source *source)
{
	struct event_data *data = source->data;
	uint64_t value;
	spa_assert_se(read(data->fd, &value, sizeof(value)) == sizeof(value));
	SPA_ATOMIC_STORE(data->ready, true);
}

static int process_event(void *user_data)
{
	struct event_data *data = user_data;
	if (!SPA_ATOMIC_XCHG(data->ready, false))
		return 0;
	SPA_ATOMIC_INC(data->processed);
	return 1;
}

static void test_event_idle(enum pw_rtc_data_loop_idle idle)
{
	struct fixture fixture;
	struct event_data data = { .fd = -1 };
	struct spa_source source = { 0 };
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = idle,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_OTHER,
		.hybrid_spin_iterations = 32,
	};
	struct pw_rtc_data_loop *loop;
	uint64_t value = 1;

	fixture_init(&fixture, true);
	loop = pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_event, &data);
	spa_assert_se(loop != NULL);

	data.fd = eventfd(0, EFD_CLOEXEC);
	spa_assert_se(data.fd >= 0);
	source.func = on_event;
	source.fd = data.fd;
	source.mask = SPA_IO_IN;
	source.data = &data;
	spa_assert_se(pw_loop_add_source(pw_rtc_data_loop_get_loop(loop), &source) == 0);

	spa_assert_se(pw_rtc_data_loop_start(loop) == 0);
	spa_assert_se(write(data.fd, &value, sizeof(value)) == sizeof(value));
	wait_until_at_least(&data.processed, 1);
	spa_assert_se(pw_rtc_data_loop_stop(loop) == 0);
	spa_assert_se(pw_loop_remove_source(pw_rtc_data_loop_get_loop(loop), &source) == 0);
	close(data.fd);
	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);
}

static int process_error(void *data)
{
	uint32_t *calls = data;
	SPA_ATOMIC_INC(*calls);
	return -EIO;
}

static void test_process_error(void)
{
	struct fixture fixture;
	uint32_t calls = 0;
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_FIFO,
		.priority = 73,
	};
	struct pw_rtc_data_loop *loop;

	fixture_init(&fixture, true);
	loop = pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_error, &calls);
	spa_assert_se(loop != NULL);
	spa_assert_se(pw_rtc_data_loop_start(loop) == 0);
	wait_until_at_least(&calls, 1);
	spa_assert_se(pw_rtc_data_loop_stop(loop) == -EIO);
	spa_assert_se(pw_rtc_data_loop_get_result(loop) == -EIO);
	spa_assert_se(fixture.thread_utils.acquires == 1);
	spa_assert_se(fixture.thread_utils.requested_priority == 73);
	spa_assert_se(fixture.thread_utils.joins == 1);
	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);
}

static int process_none(void *data)
{
	return 0;
}

static void test_config_validation(void)
{
	struct fixture fixture;
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = PW_RTC_DATA_LOOP_IDLE_HYBRID,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_OTHER,
	};

	fixture_init(&fixture, true);
	errno = 0;
	spa_assert_se(pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_none, NULL) == NULL);
	spa_assert_se(errno == EINVAL);

	config.idle = PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN;
	config.scheduler = PW_RTC_DATA_LOOP_SCHED_FIFO;
	config.priority = 0;
	errno = 0;
	spa_assert_se(pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_none, NULL) == NULL);
	spa_assert_se(errno == EINVAL);

	config.priority = -2;
	errno = 0;
	spa_assert_se(pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_none, NULL) == NULL);
	spa_assert_se(errno == EINVAL);
	fixture_clear(&fixture);
}

static void test_start_failures(void)
{
	struct fixture fixture;
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_OTHER,
	};
	struct pw_rtc_data_loop *loop;

	fixture_init(&fixture, false);
	loop = pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_none, NULL);
	spa_assert_se(loop != NULL);
	spa_assert_se(pw_rtc_data_loop_start(loop) == -ENOTSUP);
	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);

	fixture_init(&fixture, true);
	fixture.thread_utils.scheduler_result = -EPERM;
	loop = pw_rtc_data_loop_new(fixture.context, NULL, &config,
			process_none, NULL);
	spa_assert_se(loop != NULL);
	spa_assert_se(pw_rtc_data_loop_start(loop) == -EPERM);
	spa_assert_se(fixture.thread_utils.creates == 1);
	spa_assert_se(fixture.thread_utils.joins == 1);
	spa_assert_se(pw_rtc_data_loop_get_thread(loop) == NULL);
	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);
}

struct synthetic_rtc_node {
	struct spa_node node;
	struct spa_hook_list hooks;
	struct spa_node_info info;
	uint32_t starts;
	uint32_t pauses;
	uint32_t suspends;
	uint32_t process_calls;
	uint32_t processing;
	uint32_t lifecycle_overlap;
	uint32_t process_delay_us;
	uintptr_t process_thread;
	uint64_t benchmark_start;
	uint64_t *benchmark_latency;
	uint32_t benchmark_index;
	struct spa_system *benchmark_system;
	int process_result;
};

static void synthetic_emit_info(struct synthetic_rtc_node *node)
{
	spa_node_emit_info(&node->hooks, &node->info);
}

static int synthetic_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct synthetic_rtc_node *node = object;
	struct spa_hook_list save;

	spa_hook_list_isolate(&node->hooks, &save, listener, events, data);
	synthetic_emit_info(node);
	spa_hook_list_join(&node->hooks, &save);
	return 0;
}

static int synthetic_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	return 0;
}

static int synthetic_set_io(void *object, uint32_t id, void *data, size_t size)
{
	return 0;
}

static int synthetic_send_command(void *object, const struct spa_command *command)
{
	struct synthetic_rtc_node *node = object;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		SPA_ATOMIC_INC(node->starts);
		break;
	case SPA_NODE_COMMAND_Pause:
		if (SPA_ATOMIC_LOAD(node->processing) != 0)
			SPA_ATOMIC_INC(node->lifecycle_overlap);
		SPA_ATOMIC_INC(node->pauses);
		break;
	case SPA_NODE_COMMAND_Suspend:
		if (SPA_ATOMIC_LOAD(node->processing) != 0)
			SPA_ATOMIC_INC(node->lifecycle_overlap);
		SPA_ATOMIC_INC(node->suspends);
		break;
	default:
		break;
	}
	return 0;
}

static int synthetic_process(void *object)
{
	struct synthetic_rtc_node *node = object;
	uint32_t delay;

	SPA_ATOMIC_INC(node->processing);
	SPA_ATOMIC_STORE(node->process_thread, (uintptr_t)pthread_self());
	if (node->benchmark_latency != NULL)
		node->benchmark_latency[node->benchmark_index] =
				get_time_ns(node->benchmark_system) -
				node->benchmark_start;
	SPA_ATOMIC_INC(node->process_calls);
	delay = SPA_ATOMIC_LOAD(node->process_delay_us);
	if (delay != 0)
		usleep(delay);
	SPA_ATOMIC_DEC(node->processing);
	return node->process_result;
}

static const struct spa_node_methods synthetic_methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = synthetic_add_listener,
	.set_callbacks = synthetic_set_callbacks,
	.set_io = synthetic_set_io,
	.send_command = synthetic_send_command,
	.process = synthetic_process,
};

static void synthetic_init(struct synthetic_rtc_node *node)
{
	spa_zero(*node);
	spa_hook_list_init(&node->hooks);
	node->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &synthetic_methods, node);
	node->info = SPA_NODE_INFO_INIT();
	node->info.change_mask = SPA_NODE_CHANGE_MASK_FLAGS;
	node->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_RTC_PROCESS;
}

static void synthetic_init_regular(struct synthetic_rtc_node *node)
{
	synthetic_init(node);
	node->info.flags = SPA_NODE_FLAG_RT;
}

static void wait_for_node_state(struct fixture *fixture,
		struct pw_impl_node *node, enum pw_node_state state)
{
	struct timespec start, now;
	uint64_t elapsed;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (node->info.state != state) {
		struct pw_loop *loop = pw_main_loop_get_loop(fixture->main_loop);

		pw_loop_enter(loop);
		pw_loop_iterate(loop, 0);
		pw_loop_leave(loop);
		sched_yield();
		spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * SPA_NSEC_PER_SEC +
			(uint64_t)(now.tv_nsec - start.tv_nsec);
		spa_assert_se(elapsed < 2 * SPA_NSEC_PER_SEC);
	}
}

static void wait_for_link_prepared(struct fixture *fixture,
		struct pw_impl_link *link)
{
	struct timespec start, now;
	uint64_t elapsed;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (!link->prepared) {
		struct pw_loop *loop = pw_main_loop_get_loop(fixture->main_loop);

		pw_loop_enter(loop);
		pw_loop_iterate(loop, 0);
		pw_loop_leave(loop);
		sched_yield();
		spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * SPA_NSEC_PER_SEC +
			(uint64_t)(now.tv_nsec - start.tv_nsec);
		spa_assert_se(elapsed < 2 * SPA_NSEC_PER_SEC);
	}
}

static struct pw_impl_node *create_synthetic_node_on_loop(struct fixture *fixture,
		struct synthetic_rtc_node *synthetic, int process_result,
		const char *loop_name);

static struct pw_impl_node *create_synthetic_node(struct fixture *fixture,
		struct synthetic_rtc_node *synthetic, int process_result)
{
	return create_synthetic_node_on_loop(fixture, synthetic, process_result, NULL);
}

static struct pw_impl_node *create_synthetic_node_on_loop(struct fixture *fixture,
		struct synthetic_rtc_node *synthetic, int process_result,
		const char *loop_name)
{
	struct pw_impl_node *node;
	struct pw_properties *properties;

	synthetic_init(synthetic);
	synthetic->process_result = process_result;
	properties = pw_properties_new(
				PW_KEY_NODE_NAME, "synthetic-rtc",
				PW_KEY_NODE_PAUSE_ON_IDLE, "false",
				NULL);
	spa_assert_se(properties != NULL);
	if (loop_name != NULL)
		spa_assert_se(pw_properties_set(properties,
				PW_KEY_NODE_LOOP_NAME, loop_name) >= 0);
	node = pw_context_create_node(fixture->context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic->node) == 0);
	spa_assert_se(SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_RTC_PROCESS));
	spa_assert_se(node->rtc_loop == NULL);
	spa_assert_se(pw_impl_node_register(node, NULL) == 0);
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	return node;
}

static int count_poll_source(void *data)
{
	uint32_t *count = data;

	SPA_ATOMIC_INC(*count);
	return 0;
}

static int set_invoked(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	uint32_t *invoked = user_data;

	SPA_ATOMIC_STORE(*invoked, true);
	return 17;
}

static void test_polling_data_loop_lifecycle(void)
{
	struct pw_properties *properties;
	struct pw_data_loop_source source = { 0 };
	struct spa_source fd_source = { 0 };
	struct pw_data_loop *loop;
	uint32_t count = 0, invoked = 0;
	int fd;

	properties = pw_properties_new(PW_KEY_LOOP_IDLE, "invalid", NULL);
	spa_assert_se(properties != NULL);
	errno = 0;
	spa_assert_se(pw_data_loop_new(&properties->dict) == NULL);
	spa_assert_se(errno == EINVAL);
	pw_properties_free(properties);

	properties = pw_properties_new(PW_KEY_LOOP_IDLE, "busy-spin",
			SPA_KEY_THREAD_NAME, "test-polling-loop", NULL);
	spa_assert_se(properties != NULL);
	loop = pw_data_loop_new(&properties->dict);
	pw_properties_free(properties);
	spa_assert_se(loop != NULL);
	spa_assert_se(loop->polling);
	fd = spa_system_eventfd_create(loop->loop->system,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	spa_assert_se(fd >= 0);
	fd_source.fd = fd;
	fd_source.mask = SPA_IO_IN;
	spa_assert_se(pw_loop_add_source(loop->loop, &fd_source) == -ENOTSUP);
	spa_assert_se(spa_system_close(loop->loop->system, fd) == 0);
	spa_list_init(&source.link);
	source.process = count_poll_source;
	source.data = &count;
	spa_list_append(&loop->poll_source_list, &source.link);
	source.added = true;
	source.enabled = true;
	spa_assert_se(pw_data_loop_start(loop) == 0);
	wait_until_at_least(&count, 1);
	spa_assert_se(pw_data_loop_invoke(loop, set_invoked, 1,
			NULL, 0, true, &invoked) == 17);
	spa_assert_se(SPA_ATOMIC_LOAD(invoked) == 1);
	spa_assert_se(pw_data_loop_stop(loop) == 0);
	spa_list_remove(&source.link);
	source.added = false;
	pw_data_loop_destroy(loop);
}

static int prepare_regular_node(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *node = user_data;
	struct pw_node_activation_state *state =
			&node->rt.target.activation->state[0];
	uint64_t event_count;
	int res;

	if (node->data_loop_impl->polling) {
		spa_list_append(&node->data_loop_impl->poll_source_list,
				&node->poll_source.link);
		node->poll_source.added = true;
		node->poll_source.enabled = true;
	} else {
		res = spa_system_eventfd_read(node->rt.target.system,
				node->source.fd, &event_count);
		spa_assert_se(res == 0 || res == -EAGAIN);
		spa_assert_se(spa_loop_add_source(loop, &node->source) == 0);
	}
	node->rt.prepared = true;
	SPA_ATOMIC_STORE(state->required, 1);
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(node->rt.target.activation->status,
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
	return 0;
}

static int unprepare_regular_node(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *node = user_data;

	SPA_ATOMIC_STORE(node->rt.target.activation->status,
			PW_NODE_ACTIVATION_INACTIVE);
	node->rt.prepared = false;
	if (node->poll_source.added) {
		node->poll_source.enabled = false;
		spa_list_remove(&node->poll_source.link);
		spa_list_init(&node->poll_source.link);
		node->poll_source.added = false;
	} else {
		spa_assert_se(spa_loop_remove_source(loop, &node->source) == 0);
	}
	return 0;
}

static void wait_for_activation_finished(struct pw_impl_node *node);

static void test_regular_node_polling_activation(void)
{
	static const char data_loops[] =
		"[ { loop.name = graph-poll thread.name = graph-poll "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_rtc_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;
	uint64_t event_count;

	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	synthetic.process_result = SPA_STATUS_OK;
	properties = pw_properties_new(PW_KEY_NODE_NAME, "synthetic-regular",
			PW_KEY_NODE_LOOP_NAME, "graph-poll", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(node->data_loop_impl != NULL);
	spa_assert_se(node->data_loop_impl->polling);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);
	spa_assert_se(pw_loop_locked(node->data_loop, prepare_regular_node,
			1, NULL, 0, node) == 0);
	spa_assert_se(node->rt.target.trigger(&node->rt.target,
			get_time_ns(node->rt.target.system)) == 1);
	wait_until_at_least(&synthetic.process_calls, 1);
	wait_for_activation_finished(node);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.process_calls) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(node->rt.target.activation->status) ==
			PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(spa_system_eventfd_read(node->rt.target.system,
			node->source.fd, &event_count) == -EAGAIN);
	spa_assert_se(pw_loop_locked(node->data_loop, unprepare_regular_node,
			1, NULL, 0, node) == 0);
	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static int compare_u64(const void *a, const void *b)
{
	const uint64_t av = *(const uint64_t *)a;
	const uint64_t bv = *(const uint64_t *)b;

	return av < bv ? -1 : av > bv;
}

static uint64_t percentile_u64(const uint64_t *values, uint32_t n,
		uint32_t numerator, uint32_t denominator)
{
	uint64_t rank = ((uint64_t)n * numerator + denominator - 1) / denominator;

	if (rank == 0)
		rank = 1;
	return values[rank - 1];
}

static void wait_for_activation_finished(struct pw_impl_node *node)
{
	while (SPA_ATOMIC_LOAD(node->rt.target.activation->status) !=
			PW_NODE_ACTIVATION_FINISHED)
		pw_data_loop_relax();
}

static void prepare_activation_cycle(struct pw_impl_node *node)
{
	struct pw_node_activation_state *state =
			&node->rt.target.activation->state[0];

	SPA_ATOMIC_STORE(state->required, 1);
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(node->rt.target.activation->status,
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
}

static void benchmark_regular_activation(const char *idle, uint32_t samples)
{
	char data_loops[256];
	struct fixture fixture;
	struct synthetic_rtc_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;
	uint64_t *latency, start, end;
	uint32_t i;
	const uint32_t warmup = 1000;

	spa_assert_se(spa_streq(idle, "eventfd") || spa_streq(idle, "busy-spin"));
	spa_assert_se(samples > 0);
	snprintf(data_loops, sizeof(data_loops),
			"[ { loop.name = benchmark-loop thread.name = benchmark-loop "
			"loop.class = data.rt loop.idle = %s } ]", idle);
	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	synthetic.process_result = SPA_STATUS_OK;
	properties = pw_properties_new(PW_KEY_NODE_NAME, "benchmark-regular",
			PW_KEY_NODE_LOOP_NAME, "benchmark-loop", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);
	spa_assert_se(pw_loop_locked(node->data_loop, prepare_regular_node,
			1, NULL, 0, node) == 0);

	for (i = 0; i < warmup; i++) {
		prepare_activation_cycle(node);
		spa_assert_se(node->rt.target.trigger(&node->rt.target,
				get_time_ns(node->rt.target.system)) == 1);
		wait_for_activation_finished(node);
	}

	latency = calloc(samples, sizeof(*latency));
	spa_assert_se(latency != NULL);
	synthetic.benchmark_latency = latency;
	synthetic.benchmark_system = node->rt.target.system;
	start = get_time_ns(node->rt.target.system);
	for (i = 0; i < samples; i++) {
		prepare_activation_cycle(node);
		synthetic.benchmark_index = i;
		synthetic.benchmark_start = get_time_ns(node->rt.target.system);
		spa_assert_se(node->rt.target.trigger(&node->rt.target,
				synthetic.benchmark_start) == 1);
		wait_for_activation_finished(node);
	}
	end = get_time_ns(node->rt.target.system);
	synthetic.benchmark_latency = NULL;

	qsort(latency, samples, sizeof(*latency), compare_u64);
	printf("activation idle=%s samples=%u rate=%.1f/s "
			"p50=%.3fus p99=%.3fus p99.9=%.3fus max=%.3fus\n",
			idle, samples,
			(double)samples * SPA_NSEC_PER_SEC / (double)(end - start),
			(double)percentile_u64(latency, samples, 50, 100) / 1000.0,
			(double)percentile_u64(latency, samples, 99, 100) / 1000.0,
			(double)percentile_u64(latency, samples, 999, 1000) / 1000.0,
			(double)latency[samples - 1] / 1000.0);

	free(latency);
	spa_assert_se(pw_loop_locked(node->data_loop, unprepare_regular_node,
			1, NULL, 0, node) == 0);
	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_rtc_nodes_share_configured_polling_loops(void)
{
	static const char data_loops[] =
		"[ { loop.name = poll-a thread.name = poll-a "
		"loop.class = data.rt loop.idle = busy-spin } "
		"{ loop.name = poll-b thread.name = poll-b "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_rtc_node first, second;
	struct pw_impl_module *scheduler;
	struct pw_impl_node *first_node, *second_node;

	fixture_init_data_loops(&fixture, data_loops);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	first_node = create_synthetic_node_on_loop(&fixture, &first,
			SPA_STATUS_OK, "poll-a");
	second_node = create_synthetic_node_on_loop(&fixture, &second,
			SPA_STATUS_OK, "poll-b");
	wait_for_node_state(&fixture, first_node, PW_NODE_STATE_RUNNING);
	wait_for_node_state(&fixture, second_node, PW_NODE_STATE_RUNNING);
	wait_until_at_least(&first.process_calls, 1);
	wait_until_at_least(&second.process_calls, 1);

	spa_assert_se(first_node->rtc_loop == NULL);
	spa_assert_se(second_node->rtc_loop == NULL);
	spa_assert_se(first_node->poll_source.added);
	spa_assert_se(second_node->poll_source.added);
	spa_assert_se(first_node->data_loop_impl->polling);
	spa_assert_se(second_node->data_loop_impl->polling);
	spa_assert_se(first_node->data_loop_impl != second_node->data_loop_impl);
	spa_assert_se(SPA_ATOMIC_LOAD(first.process_thread) != 0);
	spa_assert_se(SPA_ATOMIC_LOAD(second.process_thread) != 0);
	spa_assert_se(SPA_ATOMIC_LOAD(first.process_thread) !=
			SPA_ATOMIC_LOAD(second.process_thread));

	SPA_ATOMIC_STORE(first.process_delay_us, 20000);
	wait_until_at_least(&first.processing, 1);
	spa_assert_se(pw_impl_node_set_active(first_node, false) == 0);
	wait_for_node_state(&fixture, first_node, PW_NODE_STATE_IDLE);
	spa_assert_se(!first_node->poll_source.added);
	spa_assert_se(SPA_ATOMIC_LOAD(first.processing) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(first.lifecycle_overlap) == 0);

	spa_assert_se(pw_impl_node_set_active(second_node, false) == 0);
	wait_for_node_state(&fixture, second_node, PW_NODE_STATE_IDLE);
	pw_impl_node_destroy(first_node);
	pw_impl_node_destroy(second_node);
	fixture_clear(&fixture);
}

static void test_rtc_node_lifecycle(void)
{
	struct fixture fixture;
	struct synthetic_rtc_node synthetic;
	struct pw_impl_module *scheduler;
	struct pw_impl_node *node;

	fixture_init(&fixture, true);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	node = create_synthetic_node(&fixture, &synthetic, SPA_STATUS_OK);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_RUNNING);
	wait_until_at_least(&synthetic.process_calls, 1);
	spa_assert_se(node->rtc_loop != NULL);
	spa_assert_se(pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(!node->rt.prepared);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 1);

	SPA_ATOMIC_STORE(synthetic.process_delay_us, 20000);
	wait_until_at_least(&synthetic.processing, 1);
	spa_assert_se(pw_impl_node_send_command(node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause)) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_IDLE);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.processing) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.lifecycle_overlap) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 1);

	SPA_ATOMIC_STORE(synthetic.process_delay_us, 0);
	spa_assert_se(pw_impl_node_send_command(node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start)) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_RUNNING);
	spa_assert_se(pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 2);

	spa_assert_se(pw_impl_node_set_active(node, false) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_IDLE);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 2);

	synthetic.info.flags = SPA_NODE_FLAG_RT;
	synthetic_emit_info(&synthetic);
	spa_assert_se(SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_RTC_PROCESS));

	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_rtc_node_requires_thread_utils(void)
{
	struct fixture fixture;
	struct synthetic_rtc_node synthetic;
	struct pw_impl_module *scheduler;
	struct pw_impl_node *node;

	fixture_init(&fixture, false);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	node = create_synthetic_node(&fixture, &synthetic, SPA_STATUS_OK);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_ERROR);
	spa_assert_se(node->rtc_loop != NULL);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.process_calls) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 1);

	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_rtc_node_terminal_process_error(void)
{
	struct fixture fixture;
	struct synthetic_rtc_node synthetic;
	struct pw_impl_module *scheduler;
	struct pw_impl_node *node;

	fixture_init(&fixture, true);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	node = create_synthetic_node(&fixture, &synthetic, -EIO);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_ERROR);
	spa_assert_se(node->info.error != NULL);
	spa_assert_se(node->rtc_loop != NULL);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.process_calls) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 1);

	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_rtc_port_node_waits_for_prepared_link(void)
{
	struct fixture fixture;
	struct pw_impl_module *scheduler;
	struct pw_impl_node *node;
	struct spa_handle *handle;
	struct spa_node *implementation;
	struct pw_loop *loop;
	uint32_t i;

	fixture_init(&fixture, true);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	handle = pw_load_spa_handle("test/libspa-test", "test.ao-imagesrc",
			NULL, 0, NULL);
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&implementation) == 0);
	node = pw_context_create_node(fixture.context,
			pw_properties_new(PW_KEY_NODE_NAME, "rtc-port-source", NULL), 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, implementation) == 0);
	spa_assert_se(pw_impl_node_register(node, NULL) == 0);
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	/* Model the value left by a previously prepared link. */
	node->runnable = true;
	loop = pw_main_loop_get_loop(fixture.main_loop);
	for (i = 0; i < 32; i++) {
		pw_loop_enter(loop);
		pw_loop_iterate(loop, 0);
		pw_loop_leave(loop);
		sched_yield();
	}
	spa_assert_se(node->info.state == PW_NODE_STATE_IDLE);
	spa_assert_se(node->rtc_loop == NULL);
	spa_assert_se(pw_impl_node_find_port(node, PW_DIRECTION_OUTPUT, 0) != NULL);

	pw_impl_node_destroy(node);
	spa_assert_se(pw_unload_spa_handle(handle) == 0);
	fixture_clear(&fixture);
}

struct final_link_observation {
	struct pw_impl_node *node;
	uint32_t *processing;
	uint32_t destroys;
	uint32_t rtc_running;
	uint32_t processing_on_destroy;
};

static uint32_t *final_link_processing;

static int final_link_process(void *data)
{
	spa_assert_se(final_link_processing != NULL);
	SPA_ATOMIC_INC(*final_link_processing);
	usleep(20000);
	SPA_ATOMIC_DEC(*final_link_processing);
	return SPA_STATUS_OK;
}

static void final_link_destroyed(void *data)
{
	struct final_link_observation *observation = data;

	SPA_ATOMIC_INC(observation->destroys);
	if (observation->node->rtc_loop != NULL &&
			pw_rtc_data_loop_is_running(observation->node->rtc_loop))
		SPA_ATOMIC_STORE(observation->rtc_running, true);
	if (SPA_ATOMIC_LOAD(*observation->processing) != 0)
		SPA_ATOMIC_STORE(observation->processing_on_destroy, true);
}

static const struct pw_impl_link_events final_link_events = {
	PW_VERSION_IMPL_LINK_EVENTS,
	.destroy = final_link_destroyed,
};

static void test_rtc_final_link_quiesce(void)
{
	struct spa_video_info_raw info = {
		.format = SPA_VIDEO_FORMAT_GRAY16_LE,
		.size = SPA_RECTANGLE(64, 64),
		.framerate = SPA_FRACTION(1000, 1),
	};
	struct fixture fixture;
	uint32_t processing = 0;
	struct final_link_observation first_observation = {
		.processing = &processing,
	};
	struct final_link_observation final_observation = {
		.processing = &processing,
	};
	struct pw_impl_module *scheduler;
	struct pw_impl_node *source;
	struct pw_impl_port *output_port, *input_port[2];
	struct pw_impl_link *link[2];
	struct pw_core *core;
	struct pw_stream *input[2];
	struct spa_handle *handle;
	struct spa_node *implementation;
	struct spa_node_methods test_methods;
	struct spa_hook link_listener[2];
	struct spa_pod_builder builder;
	struct spa_pod *format;
	const struct spa_pod *params[1];
	uint8_t pod_buffer[256];
	uint32_t i;

	fixture_init(&fixture, true);
	scheduler = pw_context_load_module(fixture.context,
			"libpipewire-module-scheduler-v1", NULL, NULL);
	spa_assert_se(scheduler != NULL);
	core = pw_context_connect_self(fixture.context, NULL, 0);
	spa_assert_se(core != NULL);

	handle = pw_load_spa_handle("test/libspa-test", "test.ao-imagesrc",
			NULL, 0, NULL);
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&implementation) == 0);
	test_methods = *(const struct spa_node_methods *)
			implementation->iface.cb.funcs;
	test_methods.process = final_link_process;
	implementation->iface.cb.funcs = &test_methods;
	source = pw_context_create_node(fixture.context,
			pw_properties_new(PW_KEY_NODE_NAME, "rtc-final-link-source", NULL), 0);
	spa_assert_se(source != NULL);
	spa_assert_se(pw_impl_node_set_implementation(source, implementation) == 0);
	spa_assert_se(pw_impl_node_register(source, NULL) == 0);

	output_port = pw_impl_node_find_port(source, PW_DIRECTION_OUTPUT, 0);
	spa_assert_se(output_port != NULL);
	final_link_processing = &processing;
	for (i = 0; i < SPA_N_ELEMENTS(input); i++) {
		spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
		format = spa_format_video_raw_build(&builder,
				SPA_PARAM_EnumFormat, &info);
		spa_assert_se(format != NULL);
		input[i] = pw_stream_new(core, "rtc-final-link-input", NULL);
		spa_assert_se(input[i] != NULL);
		params[0] = format;
		spa_assert_se(pw_stream_connect(input[i], PW_DIRECTION_INPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_NO_CONVERT |
				PW_STREAM_FLAG_BUFFER_LATEST,
				params, SPA_N_ELEMENTS(params)) == 0);
		spa_assert_se(pw_impl_node_register(input[i]->node, NULL) == 0);
		input_port[i] = pw_impl_node_find_port(input[i]->node,
				PW_DIRECTION_INPUT, 0);
		spa_assert_se(input_port[i] != NULL);
		link[i] = pw_context_create_link(fixture.context, output_port,
				input_port[i], NULL, NULL, 0);
		spa_assert_se(link[i] != NULL);
		spa_assert_se(pw_impl_link_register(link[i], NULL) == 0);
		spa_assert_se(pw_impl_node_set_active(input[i]->node, true) == 0);
		wait_for_link_prepared(&fixture, link[i]);
		spa_assert_se(pw_stream_buffer_latest_worker_begin(input[i]) == 0);
		if (i == 0) {
			spa_assert_se(pw_impl_node_set_active(source, true) == 0);
			wait_for_node_state(&fixture, source, PW_NODE_STATE_RUNNING);
		}
	}
	spa_assert_se(source->rtc_loop != NULL);
	spa_assert_se(pw_rtc_data_loop_is_running(source->rtc_loop));
	wait_until_at_least(&processing, 1);

	first_observation.node = source;
	pw_impl_link_add_listener(link[0], &link_listener[0], &final_link_events,
			&first_observation);
	pw_impl_link_destroy(link[0]);
	spa_assert_se(SPA_ATOMIC_LOAD(first_observation.destroys) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(first_observation.rtc_running) == 1);
	spa_assert_se(pw_rtc_data_loop_is_running(source->rtc_loop));
	spa_assert_se(pw_stream_buffer_latest_worker_end(input[0]) == 0);

	wait_until_at_least(&processing, 1);
	final_observation.node = source;
	pw_impl_link_add_listener(link[1], &link_listener[1], &final_link_events,
			&final_observation);
	pw_impl_link_destroy(link[1]);
	spa_assert_se(SPA_ATOMIC_LOAD(final_observation.destroys) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(final_observation.rtc_running) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(final_observation.processing_on_destroy) == 0);
	wait_for_node_state(&fixture, source, PW_NODE_STATE_IDLE);

	spa_assert_se(pw_stream_buffer_latest_worker_end(input[1]) == 0);
	pw_stream_destroy(input[0]);
	pw_stream_destroy(input[1]);
	pw_impl_node_destroy(source);
	final_link_processing = NULL;
	spa_assert_se(pw_unload_spa_handle(handle) == 0);
	fixture_clear(&fixture);
}

#define POC_UNITS 4u
#define POC_OUTPUTS 2u
#define POC_MAX_UNITS_PER_CALL 2u

struct poc_calibration_node {
	struct spa_node node;
	struct spa_meta_progressive *input_progress;
	struct spa_meta_progressive *output_progress;
	const int32_t *input;
	int32_t *output;
	uint32_t processed;
	bool terminal_published;
};

struct poc_controller_node {
	struct spa_node node;
	struct spa_meta_progressive *input_progress;
	const int32_t *input;
	int32_t published[POC_OUTPUTS];
	int64_t working[POC_OUTPUTS];
	uint32_t consumed;
	uint32_t commits;
	uint32_t aborts;
	bool started;
	bool terminal_handled;
};

struct poc_island {
	struct poc_calibration_node calibration;
	struct poc_controller_node controller;
	uint32_t duty_cycles;
};

static const int32_t poc_matrix[POC_OUTPUTS][POC_UNITS] = {
	{ 1, 2, 3, 4 },
	{ -1, 1, -1, 1 },
};

static int poc_calibration_process(void *object)
{
	struct poc_calibration_node *node = object;
	enum spa_meta_progressive_state state, output_state;
	uint32_t committed, output_committed, target, end, i;
	uint64_t snapshot;
	bool did_work = false;

	snapshot = spa_meta_progressive_load_acquire(node->input_progress);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	if (state == SPA_META_PROGRESSIVE_STATE_PREPARED ||
			node->terminal_published)
		return SPA_STATUS_OK;

	snapshot = spa_meta_progressive_load_acquire(node->output_progress);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&output_committed, &output_state));
	if (output_state == SPA_META_PROGRESSIVE_STATE_PREPARED) {
		spa_meta_progressive_store_release(node->output_progress,
				spa_meta_progressive_snapshot_encode(0,
					SPA_META_PROGRESSIVE_STATE_ACTIVE));
		did_work = true;
	}

	target = committed / sizeof(node->input[0]);
	spa_assert_se(target <= POC_UNITS);
	end = SPA_MIN(target, node->processed + POC_MAX_UNITS_PER_CALL);
	for (i = node->processed; i < end; i++)
		node->output[i] = 2 * node->input[i] + 1;
	if (end != node->processed) {
		node->processed = end;
		did_work = true;
	}

	if (state == SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			node->processed == POC_UNITS) {
		spa_meta_progressive_store_release(node->output_progress,
				spa_meta_progressive_snapshot_encode(
					POC_UNITS * sizeof(node->output[0]),
					SPA_META_PROGRESSIVE_STATE_COMPLETE));
		node->terminal_published = true;
		did_work = true;
	} else if (state == SPA_META_PROGRESSIVE_STATE_ABORTED &&
			node->processed == target) {
		node->output_progress->terminal_flags =
			node->input_progress->terminal_flags;
		spa_meta_progressive_store_release(node->output_progress,
				spa_meta_progressive_snapshot_encode(
					node->processed * sizeof(node->output[0]),
					SPA_META_PROGRESSIVE_STATE_ABORTED));
		node->terminal_published = true;
		did_work = true;
	} else if (node->processed * sizeof(node->output[0]) != output_committed) {
		spa_meta_progressive_store_release(node->output_progress,
				spa_meta_progressive_snapshot_encode(
					node->processed * sizeof(node->output[0]),
					SPA_META_PROGRESSIVE_STATE_ACTIVE));
	}
	return did_work ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
}

static int poc_controller_process(void *object)
{
	struct poc_controller_node *node = object;
	enum spa_meta_progressive_state state;
	uint32_t committed, target, end, i, row;
	uint64_t snapshot;
	bool did_work = false;

	snapshot = spa_meta_progressive_load_acquire(node->input_progress);
	spa_assert_se(spa_meta_progressive_snapshot_decode(snapshot,
			&committed, &state));
	if (state == SPA_META_PROGRESSIVE_STATE_PREPARED || node->terminal_handled)
		return SPA_STATUS_OK;

	if (!node->started) {
		/* The committed controller state is never changed before finish. */
		for (row = 0; row < POC_OUTPUTS; row++)
			node->working[row] = node->published[row] / 2;
		node->started = true;
		did_work = true;
	}

	target = committed / sizeof(node->input[0]);
	spa_assert_se(target <= POC_UNITS);
	end = SPA_MIN(target, node->consumed + POC_MAX_UNITS_PER_CALL);
	for (i = node->consumed; i < end; i++)
		for (row = 0; row < POC_OUTPUTS; row++)
			node->working[row] += (int64_t)poc_matrix[row][i] * node->input[i];
	if (end != node->consumed) {
		SPA_ATOMIC_STORE(node->consumed, end);
		did_work = true;
	}

	if (state == SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			node->consumed == POC_UNITS) {
		for (row = 0; row < POC_OUTPUTS; row++)
			node->published[row] = node->working[row];
		SPA_ATOMIC_INC(node->commits);
		node->terminal_handled = true;
		did_work = true;
	} else if (state == SPA_META_PROGRESSIVE_STATE_ABORTED &&
			node->consumed == target) {
		/* Discard working state; published controller state is unchanged. */
		SPA_ATOMIC_INC(node->aborts);
		node->terminal_handled = true;
		did_work = true;
	}
	return did_work ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
}

static const struct spa_node_methods poc_calibration_methods = {
	SPA_VERSION_NODE_METHODS,
	.process = poc_calibration_process,
};

static const struct spa_node_methods poc_controller_methods = {
	SPA_VERSION_NODE_METHODS,
	.process = poc_controller_process,
};

static int poc_island_process(void *data)
{
	struct poc_island *island = data;
	int calibration, controller, work = 0;

	SPA_ATOMIC_INC(island->duty_cycles);
	calibration = spa_node_process_fast(&island->calibration.node);
	if (calibration < 0)
		return calibration;
	if (calibration != SPA_STATUS_OK)
		work++;
	controller = spa_node_process_fast(&island->controller.node);
	if (controller < 0)
		return controller;
	if (controller != SPA_STATUS_OK)
		work++;
	return work;
}

static void poc_prepare_frame(struct poc_island *island,
		struct spa_meta_progressive *input_progress,
		struct spa_meta_progressive *calibrated_progress,
		const int32_t input[POC_UNITS], int32_t calibrated[POC_UNITS])
{
	spa_assert_se(spa_meta_progressive_init(input_progress, 0, 0,
			POC_UNITS * sizeof(input[0]), sizeof(input[0])));
	spa_assert_se(spa_meta_progressive_init(calibrated_progress, 0, 0,
			POC_UNITS * sizeof(calibrated[0]), sizeof(calibrated[0])));
	island->calibration.input_progress = input_progress;
	island->calibration.output_progress = calibrated_progress;
	island->calibration.input = input;
	island->calibration.output = calibrated;
	island->calibration.processed = 0;
	island->calibration.terminal_published = false;
	island->controller.input_progress = calibrated_progress;
	island->controller.input = calibrated;
	SPA_ATOMIC_STORE(island->controller.consumed, 0);
	island->controller.started = false;
	island->controller.terminal_handled = false;
}

static void test_rtc_island_progressive_poc(void)
{
	struct fixture fixture;
	struct poc_island island = { 0 };
	struct spa_meta_progressive input_progress, calibrated_progress;
	int32_t input[POC_UNITS] = { 1, 2, 0, 0 };
	int32_t calibrated[POC_UNITS] = { 0 };
	struct pw_rtc_data_loop_config config = {
		PW_VERSION_RTC_DATA_LOOP_CONFIG,
		.idle = PW_RTC_DATA_LOOP_IDLE_BUSY_SPIN,
		.scheduler = PW_RTC_DATA_LOOP_SCHED_OTHER,
	};
	struct pw_rtc_data_loop *loop;

	fixture_init(&fixture, true);
	island.calibration.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &poc_calibration_methods, &island.calibration);
	island.controller.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &poc_controller_methods, &island.controller);
	island.controller.published[0] = 10;
	island.controller.published[1] = -2;
	poc_prepare_frame(&island, &input_progress, &calibrated_progress,
			input, calibrated);

	/* Announce two immutable camera units before the island begins polling. */
	spa_meta_progressive_store_release(&input_progress,
			spa_meta_progressive_snapshot_encode(2 * sizeof(input[0]),
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	loop = pw_rtc_data_loop_new(fixture.context, NULL, &config,
			poc_island_process, &island);
	spa_assert_se(loop != NULL);
	spa_assert_se(pw_rtc_data_loop_start(loop) == 0);
	wait_until_at_least(&island.controller.consumed, 2);
	spa_assert_se(SPA_ATOMIC_LOAD(island.controller.commits) == 0);
	spa_assert_se(island.controller.published[0] == 10);
	spa_assert_se(island.controller.published[1] == -2);

	input[2] = 3;
	input[3] = 4;
	spa_meta_progressive_store_release(&input_progress,
			spa_meta_progressive_snapshot_encode(sizeof(input),
				SPA_META_PROGRESSIVE_STATE_COMPLETE));
	wait_until_at_least(&island.controller.commits, 1);
	spa_assert_se(pw_rtc_data_loop_stop(loop) == 0);
	spa_assert_se(island.controller.published[0] == 75);
	spa_assert_se(island.controller.published[1] == 3);

	/* A partial next frame is accumulated privately and then rolled back. */
	input[0] = 10;
	input[1] = 20;
	poc_prepare_frame(&island, &input_progress, &calibrated_progress,
			input, calibrated);
	spa_meta_progressive_store_release(&input_progress,
			spa_meta_progressive_snapshot_encode(2 * sizeof(input[0]),
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	spa_assert_se(pw_rtc_data_loop_start(loop) == 0);
	wait_until_at_least(&island.controller.consumed, 2);
	input_progress.terminal_flags = SPA_META_PROGRESSIVE_FLAG_CANCELLED;
	spa_meta_progressive_store_release(&input_progress,
			spa_meta_progressive_snapshot_encode(2 * sizeof(input[0]),
				SPA_META_PROGRESSIVE_STATE_ABORTED));
	wait_until_at_least(&island.controller.aborts, 1);
	spa_assert_se(pw_rtc_data_loop_stop(loop) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(island.controller.commits) == 1);
	spa_assert_se(island.controller.published[0] == 75);
	spa_assert_se(island.controller.published[1] == 3);
	spa_assert_se(SPA_ATOMIC_LOAD(island.duty_cycles) > 0);

	pw_rtc_data_loop_destroy(loop);
	fixture_clear(&fixture);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	if (argc >= 2 && spa_streq(argv[1], "--test-polling")) {
		test_polling_data_loop_lifecycle();
		test_regular_node_polling_activation();
		test_rtc_nodes_share_configured_polling_loops();
		pw_deinit();
		return 0;
	}
	if (argc >= 3 && spa_streq(argv[1], "--benchmark-activation")) {
		uint32_t samples = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 10000;

		benchmark_regular_activation(argv[2], samples);
		pw_deinit();
		return 0;
	}

	test_busy_spin_lifecycle();
	test_event_idle(PW_RTC_DATA_LOOP_IDLE_EVENTFD);
	test_event_idle(PW_RTC_DATA_LOOP_IDLE_HYBRID);
	test_process_error();
	test_config_validation();
	test_start_failures();
	test_polling_data_loop_lifecycle();
	test_regular_node_polling_activation();
	test_rtc_nodes_share_configured_polling_loops();
	test_rtc_node_lifecycle();
	test_rtc_node_requires_thread_utils();
	test_rtc_node_terminal_process_error();
	test_rtc_port_node_waits_for_prepared_link();
	test_rtc_final_link_quiesce();
	test_rtc_island_progressive_poc();

	pw_deinit();
	return 0;
}
