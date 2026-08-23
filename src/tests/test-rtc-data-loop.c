/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>

#include <spa/node/utils.h>
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
		SPA_ATOMIC_INC(node->pauses);
		break;
	case SPA_NODE_COMMAND_Suspend:
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

	SPA_ATOMIC_INC(node->process_calls);
	return SPA_STATUS_OK;
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

static struct pw_impl_node *create_synthetic_node(struct fixture *fixture,
		struct synthetic_rtc_node *synthetic)
{
	struct pw_impl_node *node;

	synthetic_init(synthetic);
	node = pw_context_create_node(fixture->context,
			pw_properties_new(
				PW_KEY_NODE_NAME, "synthetic-rtc",
				PW_KEY_NODE_PAUSE_ON_IDLE, "false",
				NULL), 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic->node) == 0);
	spa_assert_se(SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_RTC_PROCESS));
	spa_assert_se(node->rtc_loop == NULL);
	spa_assert_se(pw_impl_node_register(node, NULL) == 0);
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	return node;
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
	node = create_synthetic_node(&fixture, &synthetic);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_RUNNING);
	wait_until_at_least(&synthetic.process_calls, 1);
	spa_assert_se(node->rtc_loop != NULL);
	spa_assert_se(pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(!node->rt.prepared);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 1);

	spa_assert_se(pw_impl_node_set_active(node, false) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_IDLE);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 1);

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
	node = create_synthetic_node(&fixture, &synthetic);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_ERROR);
	spa_assert_se(node->rtc_loop != NULL);
	spa_assert_se(!pw_rtc_data_loop_is_running(node->rtc_loop));
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.process_calls) == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.starts) == 1);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.pauses) == 1);

	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_busy_spin_lifecycle();
	test_event_idle(PW_RTC_DATA_LOOP_IDLE_EVENTFD);
	test_event_idle(PW_RTC_DATA_LOOP_IDLE_HYBRID);
	test_process_error();
	test_config_validation();
	test_start_failures();
	test_rtc_node_lifecycle();
	test_rtc_node_requires_thread_utils();

	pw_deinit();
	return 0;
}
