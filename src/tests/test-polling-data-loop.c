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
#include <sys/mman.h>
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
	int process_result;
};

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
	target.trigger = trigger_target_v1;

	child = fork();
	spa_assert_se(child >= 0);
	if (child == 0) {
		struct timespec start, now;
		uint64_t elapsed;

		SPA_ATOMIC_STORE(shared->child_ready, true);
		spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
		while (SPA_ATOMIC_LOAD(shared->activation.status) !=
				PW_NODE_ACTIVATION_TRIGGERED) {
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
		if (shared->activation.signal_time != signal_time)
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
	struct synthetic_node synthetic;
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

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	if (argc >= 3 && spa_streq(argv[1], "--benchmark-activation")) {
		uint32_t samples = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 10000;

		benchmark_regular_activation(argv[2], samples);
		pw_deinit();
		return 0;
	}

	test_polling_data_loop_lifecycle();
	test_regular_node_polling_activation();
	test_poll_driver_cycles_without_eventfd();
	test_cross_process_polling_activation();
	test_exported_node_polling_activation();
	test_remote_node_on_polling_control_loop();
	test_exported_polling_rejects_activation_v0();

	pw_deinit();
	return 0;
}
