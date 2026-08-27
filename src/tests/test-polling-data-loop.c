/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include <spa/node/utils.h>
#include <spa/utils/atomic.h>

struct fixture {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
};

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

static void test_polling_loop_requires_explicit_selection(void)
{
	static const char data_loops[] =
		"[ { loop.name = ordinary thread.name = ordinary "
		"loop.class = data.rt loop.idle = eventfd } "
		"{ loop.name = rtc thread.name = rtc "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct pw_properties *properties;
	struct pw_loop *loop;

	fixture_init_data_loops(&fixture, data_loops);
	loop = pw_context_acquire_loop(fixture.context, NULL);
	spa_assert_se(loop != NULL);
	spa_assert_se(spa_streq(loop->name, "ordinary"));
	loop = pw_context_acquire_loop(fixture.context, NULL);
	spa_assert_se(loop != NULL);
	spa_assert_se(spa_streq(loop->name, "ordinary"));

	properties = pw_properties_new(PW_KEY_NODE_LOOP_NAME, "rtc", NULL);
	spa_assert_se(properties != NULL);
	loop = pw_context_acquire_loop(fixture.context, &properties->dict);
	spa_assert_se(loop != NULL);
	spa_assert_se(spa_streq(loop->name, "rtc"));
	pw_properties_free(properties);
	fixture_clear(&fixture);
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

struct synthetic_node {
	struct spa_node node;
	struct spa_hook_list hooks;
	struct spa_node_info info;
	uint32_t starts;
	uint32_t pauses;
	uint32_t suspends;
	uint32_t process_calls;
	uint32_t processing;
	uint32_t starting;
	uint32_t lifecycle_overlap;
	uint32_t start_delay_us;
	uint32_t process_delay_us;
	uintptr_t process_thread;
	uint64_t benchmark_start;
	uint64_t *benchmark_latency;
	uint32_t benchmark_index;
	struct spa_system *benchmark_system;
	int benchmark_requested_rt_priority;
	int benchmark_process_cpu;
	int benchmark_process_policy;
	int benchmark_process_priority;
	pid_t benchmark_process_tid;
	int benchmark_scheduler_error;
	uint32_t benchmark_observe_after_calls;
	int process_result;
};

static void observe_benchmark_scheduler(int *observed_cpu,
		int *observed_policy, int *observed_priority)
{
	struct sched_param parameter = { 0 };

	spa_assert_se(pthread_getschedparam(pthread_self(), observed_policy,
			&parameter) == 0);
#ifdef SCHED_RESET_ON_FORK
	*observed_policy &= ~SCHED_RESET_ON_FORK;
#endif
	*observed_priority = parameter.sched_priority;
	*observed_cpu = sched_getcpu();
}

static void configure_benchmark_scheduler(int requested_rt_priority,
		int *observed_cpu, int *observed_policy, int *observed_priority,
		int *scheduler_error)
{
	struct sched_param parameter = { 0 };
	int res = 0;

	if (requested_rt_priority > 0) {
		parameter.sched_priority = requested_rt_priority;
		int policy = SCHED_FIFO;

#ifdef SCHED_RESET_ON_FORK
		policy |= SCHED_RESET_ON_FORK;
#endif
		res = pthread_setschedparam(pthread_self(), policy, &parameter);
	}
	*scheduler_error = res;
	observe_benchmark_scheduler(observed_cpu, observed_policy,
			observed_priority);
}

static void synthetic_emit_info(struct synthetic_node *node)
{
	spa_node_emit_info(&node->hooks, &node->info);
}

static int synthetic_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct synthetic_node *node = object;
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
	struct synthetic_node *node = object;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		SPA_ATOMIC_STORE(node->starting, 1);
		if (node->start_delay_us != 0)
			usleep(node->start_delay_us);
		SPA_ATOMIC_INC(node->starts);
		SPA_ATOMIC_STORE(node->starting, 0);
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
	struct synthetic_node *node = object;
	uint32_t delay;

	if (SPA_ATOMIC_LOAD(node->starting) != 0)
		SPA_ATOMIC_INC(node->lifecycle_overlap);
	SPA_ATOMIC_INC(node->processing);
	SPA_ATOMIC_STORE(node->process_thread, (uintptr_t)pthread_self());
	if (node->benchmark_requested_rt_priority >= 0 &&
			node->benchmark_process_cpu == -1) {
		node->benchmark_process_tid = (pid_t)syscall(SYS_gettid);
		configure_benchmark_scheduler(node->benchmark_requested_rt_priority,
				&node->benchmark_process_cpu,
				&node->benchmark_process_policy,
				&node->benchmark_process_priority,
				&node->benchmark_scheduler_error);
	}
	if (node->benchmark_requested_rt_priority >= 0 &&
			SPA_ATOMIC_LOAD(node->process_calls) ==
			node->benchmark_observe_after_calls)
		observe_benchmark_scheduler(&node->benchmark_process_cpu,
				&node->benchmark_process_policy,
				&node->benchmark_process_priority);
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

static void synthetic_init_regular(struct synthetic_node *node)
{
	spa_zero(*node);
	spa_hook_list_init(&node->hooks);
	node->benchmark_requested_rt_priority = -1;
	node->benchmark_process_cpu = -1;
	node->benchmark_process_policy = -1;
	node->benchmark_process_priority = -1;
	node->benchmark_process_tid = -1;
	node->benchmark_scheduler_error = 0;
	node->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &synthetic_methods, node);
	node->info = SPA_NODE_INFO_INIT();
	node->info.change_mask = SPA_NODE_CHANGE_MASK_FLAGS;
	node->info.flags = SPA_NODE_FLAG_RT;
}

static void synthetic_init_poll_driver(struct synthetic_node *node)
{
	synthetic_init_regular(node);
	node->info.flags |= SPA_NODE_FLAG_POLL_DRIVER;
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
	spa_assert_se(!source.enabled);
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

	pw_node_activation_set_polling(node->rt.target.activation,
			node->data_loop_impl->polling);
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
	struct synthetic_node synthetic;
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
	spa_assert_se(pw_node_activation_is_polling(
			node->rt.target.activation));
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

static void test_poll_driver_cycles_without_eventfd(void)
{
	static const char data_loops[] =
		"[ { loop.name = driver-poll thread.name = driver-poll "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;
	uint32_t stopped_calls;
	uint64_t event_count;

	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_poll_driver(&synthetic);
	synthetic.process_result = SPA_STATUS_HAVE_DATA;
	synthetic.start_delay_us = 10000;
	properties = pw_properties_new(PW_KEY_NODE_NAME, "synthetic-poll-driver",
			PW_KEY_NODE_LOOP_NAME, "driver-poll",
			PW_KEY_NODE_DRIVER, "true", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);
	spa_assert_se(SPA_FLAG_IS_SET(node->spa_flags,
			SPA_NODE_FLAG_POLL_DRIVER));
	spa_assert_se(node->driving && node->driver_node == node);
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_RUNNING) == EBUSY);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_RUNNING);
	spa_assert_se(node->info.state == PW_NODE_STATE_RUNNING);
	spa_assert_se(node->poll_source.added);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.lifecycle_overlap) == 0);

	wait_until_at_least(&synthetic.process_calls, 2);
	spa_assert_se(spa_system_eventfd_read(node->rt.target.system,
			node->source.fd, &event_count) == -EAGAIN);
	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_IDLE) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_IDLE);
	spa_assert_se(!node->poll_source.added);
	stopped_calls = SPA_ATOMIC_LOAD(synthetic.process_calls);
	usleep(1000);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.process_calls) == stopped_calls);
	spa_assert_se(SPA_ATOMIC_LOAD(synthetic.lifecycle_overlap) == 0);

	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

struct cross_process_activation {
	struct pw_node_activation activation;
	uint32_t child_ready;
};

static void test_cross_process_polling_activation(void)
{
	static const uint64_t signal_time = UINT64_C(0x123456789abcdef0);
	struct cross_process_activation *shared;
	struct pw_node_target target = { 0 };
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	struct pw_node_activation_state *state;
	uint64_t event_count;
	pid_t child;
	int event_fd, status;

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	spa_assert_se(shared != MAP_FAILED);
	spa_zero(*shared);
	state = &shared->activation.state[0];
	shared->activation.server_version = PW_VERSION_NODE_ACTIVATION;
	SPA_ATOMIC_STORE(state->required, 1);
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(shared->activation.status,
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
	SPA_ATOMIC_STORE(shared->activation.flags,
			PW_NODE_ACTIVATION_FLAG_ASYNC |
			PW_NODE_ACTIVATION_FLAG_PROFILER);
	pw_node_activation_set_polling(&shared->activation, true);
	spa_assert_se(pw_node_activation_is_polling(&shared->activation));
	spa_assert_se(SPA_FLAG_IS_SET(SPA_ATOMIC_LOAD(shared->activation.flags),
			PW_NODE_ACTIVATION_FLAG_ASYNC));
	spa_assert_se(SPA_FLAG_IS_SET(SPA_ATOMIC_LOAD(shared->activation.flags),
			PW_NODE_ACTIVATION_FLAG_PROFILER));

	main_loop = pw_main_loop_new(NULL);
	spa_assert_se(main_loop != NULL);
	loop = pw_main_loop_get_loop(main_loop);
	event_fd = spa_system_eventfd_create(loop->system,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	spa_assert_se(event_fd >= 0);
	target.activation = &shared->activation;
	target.system = loop->system;
	target.fd = event_fd;
	target.trigger = trigger_target_v2;

	child = fork();
	spa_assert_se(child >= 0);
	if (child == 0) {
		struct timespec start, now;
		uint64_t elapsed;

		SPA_ATOMIC_STORE(shared->child_ready, true);
		spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
		while (SPA_ATOMIC_LOAD(shared->activation.status) !=
				PW_NODE_ACTIVATION_TRIGGERED ||
				!pw_node_activation_signal_time_ready(
					&shared->activation)) {
			pw_data_loop_relax();
			spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
			elapsed = (uint64_t)(now.tv_sec - start.tv_sec) *
					SPA_NSEC_PER_SEC +
					(uint64_t)(now.tv_nsec - start.tv_nsec);
			if (elapsed >= 2 * SPA_NSEC_PER_SEC)
				_exit(2);
		}
		if (!SPA_ATOMIC_CAS(shared->activation.status,
				PW_NODE_ACTIVATION_TRIGGERED,
				PW_NODE_ACTIVATION_AWAKE))
			_exit(3);
		if (pw_node_activation_get_signal_time(&shared->activation) !=
				signal_time)
			_exit(4);
		SPA_ATOMIC_STORE(shared->activation.status,
				PW_NODE_ACTIVATION_FINISHED);
		_exit(0);
	}

	wait_until_at_least(&shared->child_ready, 1);
	spa_assert_se(target.trigger(&target, signal_time) == 1);
	spa_assert_se(waitpid(child, &status, 0) == child);
	spa_assert_se(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	spa_assert_se(spa_system_eventfd_read(loop->system, event_fd,
			&event_count) == -EAGAIN);

	/* Wake policy is per target. Clearing POLLING preserves the other shared
	 * flags and restores the normal eventfd notification. */
	pw_node_activation_set_polling(&shared->activation, false);
	spa_assert_se(!pw_node_activation_is_polling(&shared->activation));
	spa_assert_se(SPA_FLAG_IS_SET(SPA_ATOMIC_LOAD(shared->activation.flags),
			PW_NODE_ACTIVATION_FLAG_ASYNC));
	spa_assert_se(SPA_FLAG_IS_SET(SPA_ATOMIC_LOAD(shared->activation.flags),
			PW_NODE_ACTIVATION_FLAG_PROFILER));
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(shared->activation.status,
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
	spa_assert_se(target.trigger(&target, signal_time + 1) == 1);
	spa_assert_se(spa_system_eventfd_read(loop->system, event_fd,
			&event_count) == 0);
	spa_assert_se(event_count == 1);

	spa_assert_se(spa_system_close(loop->system, event_fd) == 0);
	pw_main_loop_destroy(main_loop);
	spa_assert_se(munmap(shared, sizeof(*shared)) == 0);
}

static void test_failed_polling_activation_preserves_signal_time(void)
{
	static const uint64_t current_time = UINT64_C(0x1111222233334444);
	static const uint64_t rejected_time = UINT64_C(0xaaaabbbbccccdddd);
	struct pw_node_activation activation = { 0 };
	struct pw_node_activation_state *state = &activation.state[0];
	struct pw_node_target target = { 0 };
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	uint64_t event_count;
	int event_fd;

	activation.server_version = PW_VERSION_NODE_ACTIVATION;
	activation.client_version = PW_VERSION_NODE_ACTIVATION;
	activation.signal_time = current_time;
	pw_node_activation_set_polling(&activation, true);
	SPA_ATOMIC_STORE(state->required, 1);
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_TRIGGERED);

	main_loop = pw_main_loop_new(NULL);
	spa_assert_se(main_loop != NULL);
	loop = pw_main_loop_get_loop(main_loop);
	event_fd = spa_system_eventfd_create(loop->system,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	spa_assert_se(event_fd >= 0);
	target.activation = &activation;
	target.system = loop->system;
	target.fd = event_fd;
	target.trigger = trigger_target_v2;

	spa_assert_se(target.trigger(&target, rejected_time) == -EIO);
	spa_assert_se(pw_node_activation_signal_time_ready(&activation));
	spa_assert_se(pw_node_activation_get_signal_time(&activation) ==
			current_time);
	spa_assert_se(SPA_ATOMIC_LOAD(activation.status) ==
			PW_NODE_ACTIVATION_TRIGGERED);
	spa_assert_se(spa_system_eventfd_read(loop->system, event_fd,
			&event_count) == -EAGAIN);

	spa_assert_se(spa_system_close(loop->system, event_fd) == 0);
	pw_main_loop_destroy(main_loop);
}

static void test_exported_node_polling_activation(void)
{
	static const char data_loops[] =
		"[ { loop.name = exported-poll thread.name = exported-poll "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;
	struct pw_node_activation_state *state;
	uint64_t event_count;

	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	synthetic.process_result = SPA_STATUS_OK;
	properties = pw_properties_new(PW_KEY_NODE_NAME, "synthetic-exported",
			PW_KEY_NODE_LOOP_NAME, "exported-poll", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);

	/* pw_core_export() marks the implementation-side node this way. Drive the
	 * state directly to exercise its real prepare and teardown paths. */
	node->exported = true;
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_RUNNING) == 0);
	spa_assert_se(node->info.state == PW_NODE_STATE_RUNNING);
	spa_assert_se(node->rt.prepared);
	spa_assert_se(node->poll_source.added);
	spa_assert_se(pw_node_activation_is_polling(
			node->rt.target.activation));

	state = &node->rt.target.activation->state[0];
	SPA_ATOMIC_STORE(state->required, 1);
	SPA_ATOMIC_STORE(state->pending, 1);
	SPA_ATOMIC_STORE(node->rt.target.activation->status,
			PW_NODE_ACTIVATION_NOT_TRIGGERED);
	spa_assert_se(node->rt.target.trigger(&node->rt.target,
			get_time_ns(node->rt.target.system)) == 1);
	wait_until_at_least(&synthetic.process_calls, 1);
	spa_assert_se(spa_system_eventfd_read(node->rt.target.system,
			node->source.fd, &event_count) == -EAGAIN);

	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_IDLE) == 0);
	spa_assert_se(!node->poll_source.added);
	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_remote_node_on_polling_control_loop(void)
{
	static const char data_loops[] =
		"[ { loop.name = remote-control thread.name = remote-control "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;

	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	properties = pw_properties_new(PW_KEY_NODE_NAME, "synthetic-remote",
			PW_KEY_NODE_LOOP_NAME, "remote-control", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);

	/* The daemon-side representation has no local process owner. Its client
	 * publishes the wake policy in this shared activation record. */
	node->remote = true;
	pw_node_activation_set_polling(node->rt.target.activation, true);
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_RUNNING) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_RUNNING);
	spa_assert_se(node->rt.prepared);
	spa_assert_se(!node->poll_source.added);
	spa_assert_se(pw_node_activation_is_polling(
			node->rt.target.activation));

	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_IDLE) == 0);
	wait_for_node_state(&fixture, node, PW_NODE_STATE_IDLE);
	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

static void test_exported_polling_rejects_activation_v0(void)
{
	static const char data_loops[] =
		"[ { loop.name = exported-v0 thread.name = exported-v0 "
		"loop.class = data.rt loop.idle = busy-spin } ]";
	struct fixture fixture;
	struct synthetic_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;

	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	properties = pw_properties_new(PW_KEY_NODE_NAME, "synthetic-exported-v0",
			PW_KEY_NODE_LOOP_NAME, "exported-v0", NULL);
	spa_assert_se(properties != NULL);
	node = pw_context_create_node(fixture.context, properties, 0);
	spa_assert_se(node != NULL);
	spa_assert_se(pw_impl_node_set_implementation(node, &synthetic.node) == 0);
	node->exported = true;
	node->rt.target.activation->server_version = 0;
	spa_assert_se(pw_impl_node_set_active(node, true) == 0);
	spa_assert_se(pw_impl_node_set_state(node, PW_NODE_STATE_RUNNING) == 0);
	spa_assert_se(node->info.state == PW_NODE_STATE_ERROR);
	spa_assert_se(!node->rt.prepared);
	spa_assert_se(!node->poll_source.added);

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

static uint64_t raw_time_nsec(void)
{
	struct timespec now;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC_RAW, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static void prefault_writable_pages(void *data, size_t size)
{
	volatile uint8_t *bytes = data;
	long page_size;
	size_t offset;

	page_size = sysconf(_SC_PAGESIZE);
	spa_assert_se(page_size > 0);
	for (offset = 0; offset < size; offset += (size_t)page_size)
		bytes[offset] = 0;
	if (size > 0)
		bytes[size - 1u] = 0;
}

static void pin_current_thread(uint32_t cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	spa_assert_se(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0);
}

static void write_histogram(const char *path, uint64_t *values, uint32_t count)
{
	FILE *output;
	uint32_t begin, end;

	spa_assert_se(path != NULL);
	qsort(values, count, sizeof(*values), compare_u64);
	output = fopen(path, "w");
	spa_assert_se(output != NULL);
	fprintf(output, "latency_ns,count\n");
	for (begin = 0; begin < count; begin = end) {
		for (end = begin + 1u; end < count && values[end] == values[begin]; end++)
			;
		fprintf(output, "%" PRIu64 ",%u\n", values[begin], end - begin);
	}
	spa_assert_se(fclose(output) == 0);
}

enum scan_contention {
	SCAN_CONTENTION_NONE,
	SCAN_CONTENTION_PERIODIC,
	SCAN_CONTENTION_BURST,
};

struct benchmark_profile_gate {
	int ready_fd;
	int start_fd;
	int done_fd;
	int finish_fd;
	bool enabled;
};

static int profile_gate_fd(const char *name)
{
	const char *value = getenv(name);
	char *end = NULL;
	long fd;

	if (value == NULL)
		return -1;
	errno = 0;
	fd = strtol(value, &end, 10);
	spa_assert_se(errno == 0 && end != value && *end == '\0' &&
			fd >= 0 && fd <= INT_MAX);
	return (int)fd;
}

static void profile_gate_init(struct benchmark_profile_gate *gate)
{
	gate->ready_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_READY_FD");
	gate->start_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_START_FD");
	gate->done_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_DONE_FD");
	gate->finish_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_FINISH_FD");
	gate->enabled = gate->ready_fd >= 0 || gate->start_fd >= 0 ||
			gate->done_fd >= 0 || gate->finish_fd >= 0;
	spa_assert_se(!gate->enabled || (gate->ready_fd >= 0 &&
			gate->start_fd >= 0 && gate->done_fd >= 0 &&
			gate->finish_fd >= 0));
}

static void profile_gate_write(int fd, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	while (size > 0) {
		ssize_t written = write(fd, bytes, size);

		if (written < 0 && errno == EINTR)
			continue;
		spa_assert_se(written > 0);
		bytes += written;
		size -= (size_t)written;
	}
}

static void profile_gate_read(int fd, void *data, size_t size)
{
	uint8_t *bytes = data;

	while (size > 0) {
		ssize_t received = read(fd, bytes, size);

		if (received < 0 && errno == EINTR)
			continue;
		spa_assert_se(received > 0);
		bytes += received;
		size -= (size_t)received;
	}
}

static void profile_gate_begin_tid(struct benchmark_profile_gate *gate, pid_t tid)
{
	uint8_t token;

	if (!gate->enabled)
		return;
	spa_assert_se(tid > 0);
	profile_gate_write(gate->ready_fd, &tid, sizeof(tid));
	profile_gate_read(gate->start_fd, &token, sizeof(token));
}

static void profile_gate_begin(struct benchmark_profile_gate *gate)
{
	profile_gate_begin_tid(gate, (pid_t)syscall(SYS_gettid));
}

static void profile_gate_end(struct benchmark_profile_gate *gate)
{
	const uint8_t token = 1;
	uint8_t reply;

	if (!gate->enabled)
		return;
	profile_gate_write(gate->done_fd, &token, sizeof(token));
	profile_gate_read(gate->finish_fd, &reply, sizeof(reply));
}

struct scan_benchmark {
	struct pw_data_loop *loop;
	uint64_t *gaps;
	uint64_t previous;
	uint32_t warmup;
	uint32_t samples;
	atomic_uint probes;
	atomic_uint work_calls;
	atomic_bool start;
	atomic_bool stop_control;
	atomic_uint control_submitted;
	atomic_uint control_completed;
	atomic_uint control_failed;
	enum scan_contention contention;
	uint32_t caller_cpu;
	int requested_rt_priority;
	int observed_loop_cpu;
	int observed_loop_policy;
	int observed_loop_priority;
	int scheduler_error;
	struct benchmark_profile_gate profile_gate;
};

static int scan_probe(void *data)
{
	struct scan_benchmark *benchmark = data;
	uint64_t now = raw_time_nsec();
	uint32_t probe;

	if (!atomic_load_explicit(&benchmark->start, memory_order_acquire))
		return 0;
	probe = atomic_fetch_add_explicit(&benchmark->probes, 1,
			memory_order_relaxed);

	if (probe == 0)
		configure_benchmark_scheduler(benchmark->requested_rt_priority,
				&benchmark->observed_loop_cpu,
				&benchmark->observed_loop_policy,
				&benchmark->observed_loop_priority,
				&benchmark->scheduler_error);
	if (probe == benchmark->warmup) {
		observe_benchmark_scheduler(&benchmark->observed_loop_cpu,
				&benchmark->observed_loop_policy,
				&benchmark->observed_loop_priority);
		profile_gate_begin(&benchmark->profile_gate);
		now = raw_time_nsec();
	}

	if (probe > benchmark->warmup) {
		uint32_t index = probe - benchmark->warmup - 1u;

		if (index < benchmark->samples)
			benchmark->gaps[index] = now - benchmark->previous;
	}
	benchmark->previous = now;
	if (probe >= benchmark->warmup + benchmark->samples) {
		profile_gate_end(&benchmark->profile_gate);
		pw_data_loop_exit(benchmark->loop);
	}
	return 0;
}

static int scan_work(void *data)
{
	struct scan_benchmark *benchmark = data;

	atomic_fetch_add_explicit(&benchmark->work_calls, 1,
			memory_order_relaxed);
	return 0;
}

static int scan_control(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct scan_benchmark *benchmark = user_data;

	atomic_fetch_add_explicit(&benchmark->control_completed, 1,
			memory_order_relaxed);
	return 0;
}

static void *inject_scan_control(void *data)
{
	struct scan_benchmark *benchmark = data;
	const uint64_t period = benchmark->contention == SCAN_CONTENTION_BURST ?
			SPA_NSEC_PER_MSEC : 50u * SPA_NSEC_PER_USEC;
	const uint32_t burst = benchmark->contention == SCAN_CONTENTION_BURST ?
			16u : 1u;
	struct timespec deadline;

	pin_current_thread(benchmark->caller_cpu);
	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
	while (!atomic_load_explicit(&benchmark->stop_control,
			memory_order_acquire)) {
		uint64_t nsec = (uint64_t)deadline.tv_nsec + period;

		deadline.tv_sec += (time_t)(nsec / SPA_NSEC_PER_SEC);
		deadline.tv_nsec = (long)(nsec % SPA_NSEC_PER_SEC);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				&deadline, NULL) == EINTR)
			;
		for (uint32_t i = 0; i < burst; i++) {
			int res = pw_data_loop_invoke(benchmark->loop, scan_control,
					SPA_ID_INVALID, NULL, 0, false, benchmark);

			if (res < 0)
				atomic_fetch_add_explicit(&benchmark->control_failed, 1,
						memory_order_relaxed);
			else
				atomic_fetch_add_explicit(&benchmark->control_submitted, 1,
						memory_order_relaxed);
		}
	}
	return NULL;
}

static const char *scan_contention_name(enum scan_contention contention)
{
	switch (contention) {
	case SCAN_CONTENTION_NONE:
		return "none";
	case SCAN_CONTENTION_PERIODIC:
		return "periodic";
	case SCAN_CONTENTION_BURST:
		return "burst";
	}
	return "invalid";
}

static enum scan_contention parse_scan_contention(const char *name)
{
	if (spa_streq(name, "none"))
		return SCAN_CONTENTION_NONE;
	if (spa_streq(name, "periodic"))
		return SCAN_CONTENTION_PERIODIC;
	if (spa_streq(name, "burst"))
		return SCAN_CONTENTION_BURST;
	spa_assert_not_reached();
	return SCAN_CONTENTION_NONE;
}

static void benchmark_empty_scans(uint32_t work_sources,
		enum scan_contention contention, uint32_t samples, uint32_t warmup,
		uint32_t caller_cpu, uint32_t loop_cpu, int rt_priority,
		const char *histogram_path)
{
	struct scan_benchmark benchmark = { 0 };
	struct pw_data_loop_source *sources;
	struct pw_properties *properties;
	pthread_t injector;
	char affinity[32];
	uint64_t total = 0;
	uint32_t i;

	spa_assert_se(samples > 0 && caller_cpu != loop_cpu);
	pin_current_thread(caller_cpu);
	benchmark.samples = samples;
	benchmark.warmup = warmup;
	benchmark.contention = contention;
	benchmark.caller_cpu = caller_cpu;
	benchmark.requested_rt_priority = rt_priority;
	benchmark.observed_loop_cpu = -1;
	benchmark.observed_loop_policy = -1;
	benchmark.observed_loop_priority = -1;
	benchmark.scheduler_error = 0;
	profile_gate_init(&benchmark.profile_gate);
	benchmark.gaps = calloc(samples, sizeof(*benchmark.gaps));
	sources = calloc(work_sources + 1u, sizeof(*sources));
	spa_assert_se(benchmark.gaps != NULL && sources != NULL);
	prefault_writable_pages(benchmark.gaps,
			(size_t)samples * sizeof(*benchmark.gaps));
	atomic_init(&benchmark.stop_control, false);
	atomic_init(&benchmark.start, false);
	atomic_init(&benchmark.probes, 0);
	atomic_init(&benchmark.work_calls, 0);
	atomic_init(&benchmark.control_submitted, 0);
	atomic_init(&benchmark.control_completed, 0);
	atomic_init(&benchmark.control_failed, 0);
	snprintf(affinity, sizeof(affinity), "[ %u ]", loop_cpu);
	properties = pw_properties_new(PW_KEY_LOOP_IDLE, "busy-spin",
			PW_KEY_LOOP_RT_PRIO, "0", SPA_KEY_THREAD_NAME, "scan-benchmark",
			SPA_KEY_THREAD_AFFINITY, affinity, NULL);
	spa_assert_se(properties != NULL);
	benchmark.loop = pw_data_loop_new(&properties->dict);
	pw_properties_free(properties);
	spa_assert_se(benchmark.loop != NULL);
	sources[0].process = scan_probe;
	sources[0].data = &benchmark;
	spa_list_append(&benchmark.loop->poll_source_list, &sources[0].link);
	sources[0].added = true;
	sources[0].enabled = true;
	for (i = 0; i < work_sources; i++) {
		sources[i + 1u].process = scan_work;
		sources[i + 1u].data = &benchmark;
		spa_list_append(&benchmark.loop->poll_source_list,
				&sources[i + 1u].link);
		sources[i + 1u].added = true;
		sources[i + 1u].enabled = true;
	}
	spa_assert_se(pw_data_loop_start(benchmark.loop) == 0);
	atomic_store_explicit(&benchmark.start, true, memory_order_release);
	if (contention != SCAN_CONTENTION_NONE)
		spa_assert_se(pthread_create(&injector, NULL, inject_scan_control,
				&benchmark) == 0);
	while (atomic_load_explicit(&benchmark.probes,
			memory_order_acquire) <= warmup + samples)
		usleep(1000);
	atomic_store_explicit(&benchmark.stop_control, true, memory_order_release);
	if (contention != SCAN_CONTENTION_NONE)
		spa_assert_se(pthread_join(injector, NULL) == 0);
	spa_assert_se(pw_data_loop_stop(benchmark.loop) == 0);
	for (i = 0; i < work_sources + 1u; i++) {
		spa_list_remove(&sources[i].link);
		sources[i].added = false;
		sources[i].enabled = false;
	}
	for (i = 0; i < samples; i++)
		total += benchmark.gaps[i];
	write_histogram(histogram_path, benchmark.gaps, samples);
	printf("scan work-sources=%u probe-sources=1 contention=%s samples=%u "
			"warmup=%u caller-cpu=%u loop-cpu=%u requested-rt-priority=%d "
			"observed-loop-cpu=%d observed-policy=%d observed-priority=%d "
			"scheduler-error=%d "
			"rate=%.1f/s "
			"p50=%.3fus p90=%.3fus p99=%.3fus p99.9=%.3fus max=%.3fus "
			"control-submitted=%u control-completed=%u control-failed=%u\n",
			work_sources, scan_contention_name(contention), samples, warmup,
			caller_cpu, loop_cpu, rt_priority, benchmark.observed_loop_cpu,
			benchmark.observed_loop_policy, benchmark.observed_loop_priority,
			benchmark.scheduler_error,
			(double)samples * SPA_NSEC_PER_SEC / (double)total,
			(double)percentile_u64(benchmark.gaps, samples, 50, 100) / 1000.0,
			(double)percentile_u64(benchmark.gaps, samples, 90, 100) / 1000.0,
			(double)percentile_u64(benchmark.gaps, samples, 99, 100) / 1000.0,
			(double)percentile_u64(benchmark.gaps, samples, 999, 1000) / 1000.0,
			(double)benchmark.gaps[samples - 1u] / 1000.0,
			atomic_load_explicit(&benchmark.control_submitted,
					memory_order_relaxed),
			atomic_load_explicit(&benchmark.control_completed,
					memory_order_relaxed),
			atomic_load_explicit(&benchmark.control_failed,
					memory_order_relaxed));
	pw_data_loop_destroy(benchmark.loop);
	free(sources);
	free(benchmark.gaps);
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

static void benchmark_regular_activation(const char *idle, uint32_t samples,
		uint32_t warmup, uint32_t caller_cpu, uint32_t loop_cpu, int rt_priority,
		const char *histogram_path)
{
	char data_loops[256];
	struct fixture fixture;
	struct synthetic_node synthetic;
	struct pw_impl_node *node;
	struct pw_properties *properties;
	struct benchmark_profile_gate profile_gate;
	uint64_t *latency, start, end;
	uint32_t i;

	spa_assert_se(spa_streq(idle, "eventfd") || spa_streq(idle, "busy-spin"));
	spa_assert_se(samples > 0 && caller_cpu != loop_cpu);
	pin_current_thread(caller_cpu);
	profile_gate_init(&profile_gate);
	snprintf(data_loops, sizeof(data_loops),
			"[ { loop.name = benchmark-loop thread.name = benchmark-loop "
			"loop.class = data.rt loop.rt-prio = 0 "
			"thread.affinity = [ %u ] loop.idle = %s } ]", loop_cpu, idle);
	fixture_init_data_loops(&fixture, data_loops);
	synthetic_init_regular(&synthetic);
	synthetic.process_result = SPA_STATUS_OK;
	synthetic.benchmark_requested_rt_priority = rt_priority;
	synthetic.benchmark_observe_after_calls = warmup / 2u;
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
	prefault_writable_pages(latency, (size_t)samples * sizeof(*latency));
	profile_gate_begin_tid(&profile_gate, synthetic.benchmark_process_tid);
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
	profile_gate_end(&profile_gate);

	write_histogram(histogram_path, latency, samples);
	printf("activation idle=%s samples=%u warmup=%u model=closed-loop "
			"caller-cpu=%u loop-cpu=%u requested-rt-priority=%d "
			"observed-loop-cpu=%d observed-policy=%d observed-priority=%d "
			"scheduler-error=%d "
			"rate=%.1f/s "
			"p50=%.3fus p90=%.3fus p99=%.3fus p99.9=%.3fus max=%.3fus\n",
			idle, samples, warmup, caller_cpu, loop_cpu, rt_priority,
			synthetic.benchmark_process_cpu, synthetic.benchmark_process_policy,
			synthetic.benchmark_process_priority,
			synthetic.benchmark_scheduler_error,
			(double)samples * SPA_NSEC_PER_SEC / (double)(end - start),
			(double)percentile_u64(latency, samples, 50, 100) / 1000.0,
			(double)percentile_u64(latency, samples, 90, 100) / 1000.0,
			(double)percentile_u64(latency, samples, 99, 100) / 1000.0,
			(double)percentile_u64(latency, samples, 999, 1000) / 1000.0,
			(double)latency[samples - 1] / 1000.0);

	free(latency);
	spa_assert_se(pw_loop_locked(node->data_loop, unprepare_regular_node,
			1, NULL, 0, node) == 0);
	pw_impl_node_destroy(node);
	fixture_clear(&fixture);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	if (argc == 9 && spa_streq(argv[1], "--benchmark-activation")) {
		benchmark_regular_activation(argv[2],
				(uint32_t)strtoul(argv[3], NULL, 10),
				(uint32_t)strtoul(argv[4], NULL, 10),
				(uint32_t)strtoul(argv[5], NULL, 10),
				(uint32_t)strtoul(argv[6], NULL, 10),
				(int)strtol(argv[7], NULL, 10), argv[8]);
		pw_deinit();
		return 0;
	}
	if (argc == 10 && spa_streq(argv[1], "--benchmark-scan")) {
		benchmark_empty_scans((uint32_t)strtoul(argv[2], NULL, 10),
				parse_scan_contention(argv[3]),
				(uint32_t)strtoul(argv[4], NULL, 10),
				(uint32_t)strtoul(argv[5], NULL, 10),
				(uint32_t)strtoul(argv[6], NULL, 10),
				(uint32_t)strtoul(argv[7], NULL, 10),
				(int)strtol(argv[8], NULL, 10), argv[9]);
		pw_deinit();
		return 0;
	}

	test_polling_data_loop_lifecycle();
	test_polling_loop_requires_explicit_selection();
	test_regular_node_polling_activation();
	test_poll_driver_cycles_without_eventfd();
	test_cross_process_polling_activation();
	test_failed_polling_activation_preserves_signal_time();
	test_exported_node_polling_activation();
	test_remote_node_on_polling_control_loop();
	test_exported_polling_rejects_activation_v0();

	pw_deinit();
	return 0;
}
