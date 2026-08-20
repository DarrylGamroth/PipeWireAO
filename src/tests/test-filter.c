/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/filter.h>
#include <pipewire/impl-node.h>
#include <pipewire/private.h>

#include <spa/utils/string.h>

#include <pthread.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	spa_assert_se(SPA_PTRDIFF(&a.func, &a) == SPA_PTRDIFF(&b.func, &b)); \
} while(0)

static void test_abi(void)
{
	static const struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*state_changed) (void *data, enum pw_filter_state old,
			enum pw_filter_state state, const char *error);
		void (*io_changed) (void *data, void *port_data, uint32_t id, void *area, uint32_t size);
		void (*param_changed) (void *data, void *port_data, uint32_t id, const struct spa_pod *param);
		void (*add_buffer) (void *data, void *port_data, struct pw_buffer *buffer);
		void (*remove_buffer) (void *data, void *port_data, struct pw_buffer *buffer);
		void (*process) (void *data, struct spa_io_position *position);
		void (*drained) (void *data);
		void (*command) (void *data, const struct spa_command *command);
	} test = { PW_VERSION_FILTER_EVENTS, NULL };

	struct pw_filter_events ev;

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, state_changed);
	TEST_FUNC(ev, test, io_changed);
	TEST_FUNC(ev, test, param_changed);
	TEST_FUNC(ev, test, add_buffer);
	TEST_FUNC(ev, test, remove_buffer);
	TEST_FUNC(ev, test, process);
	TEST_FUNC(ev, test, drained);
	TEST_FUNC(ev, test, command);

	spa_assert_se(PW_VERSION_FILTER_EVENTS == 1);
	spa_assert_se(sizeof(ev) == sizeof(test));

	spa_assert_se(PW_FILTER_STATE_ERROR == -1);
	spa_assert_se(PW_FILTER_STATE_UNCONNECTED == 0);
	spa_assert_se(PW_FILTER_STATE_CONNECTING == 1);
	spa_assert_se(PW_FILTER_STATE_PAUSED == 2);
	spa_assert_se(PW_FILTER_STATE_STREAMING == 3);

	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_ERROR) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_UNCONNECTED) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_CONNECTING) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_PAUSED) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_STREAMING) != NULL);
}

static void filter_destroy_error(void *data)
{
	spa_assert_not_reached();
}
static void filter_state_changed_error(void *data, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	spa_assert_not_reached();
}
static void filter_io_changed_error(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	spa_assert_not_reached();
}
static void filter_param_changed_error(void *data, void *port_data, uint32_t id, const struct spa_pod *format)
{
	spa_assert_not_reached();
}
static void filter_add_buffer_error(void *data, void *port_data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void filter_remove_buffer_error(void *data, void *port_data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void filter_process_error(void *data, struct spa_io_position *position)
{
	spa_assert_not_reached();
}
static void filter_drained_error(void *data)
{
	spa_assert_not_reached();
}

static const struct pw_filter_events filter_events_error =
{
	PW_VERSION_FILTER_EVENTS,
        .destroy = filter_destroy_error,
        .state_changed = filter_state_changed_error,
	.io_changed = filter_io_changed_error,
	.param_changed = filter_param_changed_error,
	.add_buffer = filter_add_buffer_error,
	.remove_buffer = filter_remove_buffer_error,
	.process = filter_process_error,
	.drained = filter_drained_error
};

static int destroy_count = 0;
static void filter_destroy_count(void *data)
{
	destroy_count++;
}
static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct pw_filter_events filter_events = filter_events_error;
	struct spa_hook listener = { 0, };
	const char *error = NULL;

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test", NULL);
	spa_assert_se(filter != NULL);
	pw_filter_add_listener(filter, &listener, &filter_events, filter);

	/* check state */
	spa_assert_se(pw_filter_get_state(filter, &error) == PW_FILTER_STATE_UNCONNECTED);
	spa_assert_se(error == NULL);
	/* check name */
	spa_assert_se(spa_streq(pw_filter_get_name(filter), "test"));

	/* check id, only when connected */
	spa_assert_se(pw_filter_get_node_id(filter) == SPA_ID_INVALID);

	/* check destroy */
	destroy_count = 0;
	filter_events.destroy = filter_destroy_count;
	pw_filter_destroy(filter);
	spa_assert_se(destroy_count == 1);

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void test_properties(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	const struct pw_properties *props;
	struct pw_filter *filter;
	struct pw_filter_events filter_events = filter_events_error;
	struct spa_hook listener = { { NULL }, };
	struct spa_dict_item items[3];

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test",
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL));
	spa_assert_se(filter != NULL);
	pw_filter_add_listener(filter, &listener, &filter_events, filter);

	props = pw_filter_get_properties(filter, NULL);
	spa_assert_se(props != NULL);
	spa_assert_se(spa_streq(pw_properties_get(props, "foo"), "bar"));
	spa_assert_se(spa_streq(pw_properties_get(props, "biz"), "fuzz"));
	spa_assert_se(pw_properties_get(props, "buzz") == NULL);

	/* remove foo */
	items[0] = SPA_DICT_ITEM_INIT("foo", NULL);
	/* change biz */
	items[1] = SPA_DICT_ITEM_INIT("biz", "buzz");
	/* add buzz */
	items[2] = SPA_DICT_ITEM_INIT("buzz", "frizz");
	pw_filter_update_properties(filter, NULL, &SPA_DICT_INIT(items, 3));

	spa_assert_se(props == pw_filter_get_properties(filter, NULL));
	spa_assert_se(pw_properties_get(props, "foo") == NULL);
	spa_assert_se(spa_streq(pw_properties_get(props, "biz"), "buzz"));
	spa_assert_se(spa_streq(pw_properties_get(props, "buzz"), "frizz"));

	/* check destroy */
	destroy_count = 0;
	filter_events.destroy = filter_destroy_count;
	pw_context_destroy(context);
	spa_assert_se(destroy_count == 1);

	pw_main_loop_destroy(loop);
}

struct roundtrip_data
{
	struct pw_main_loop *loop;
	int pending;
	int done;
};

static void core_event_done(void *object, uint32_t id, int seq)
{
	struct roundtrip_data *data = object;
	if (id == PW_ID_CORE && seq == data->pending) {
		data->done = 1;
		printf("done %d\n", seq);
		pw_main_loop_quit(data->loop);
	}
}

static int roundtrip(struct pw_core *core, struct pw_main_loop *loop)
{
	struct spa_hook core_listener;
	struct roundtrip_data data = { .loop = loop };
	const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
		.done = core_event_done,
	};
	spa_zero(core_listener);
	pw_core_add_listener(core, &core_listener,
			&core_events, &data);

	data.pending = pw_core_sync(core, PW_ID_CORE, 0);
	printf("sync %d\n", data.pending);

	while (!data.done) {
		pw_main_loop_run(loop);
	}
	spa_hook_remove(&core_listener);
	return 0;
}

static int node_count = 0;
static int port_count = 0;
static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	printf("object: id:%u type:%s/%d\n", id, type, version);
	if (spa_streq(type, PW_TYPE_INTERFACE_Port))
		port_count++;
	else if (spa_streq(type, PW_TYPE_INTERFACE_Node))
		node_count++;

}
static void registry_event_global_remove(void *data, uint32_t id)
{
	printf("object: id:%u\n", id);
}

struct port {
	struct pw_filter *filter;
};

static void test_create_port(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_filter *filter;
	struct spa_hook registry_listener = { 0, };
	static const struct pw_registry_events registry_events = {
		PW_VERSION_REGISTRY_EVENTS,
		.global = registry_event_global,
		.global_remove = registry_event_global_remove,
	};
	int res;
	struct port *port;
	enum pw_filter_state state;

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test", NULL);
	spa_assert_se(filter != NULL);

	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	spa_assert_se(registry != NULL);
        pw_registry_add_listener(registry, &registry_listener,
                                       &registry_events, NULL);

	state = pw_filter_get_state(filter, NULL);
	printf("state %s\n", pw_filter_state_as_string(state));
	res = pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	spa_assert_se(res >= 0);

	printf("wait connect\n");
	while (true) {
		state = pw_filter_get_state(filter, NULL);
		printf("state %s\n", pw_filter_state_as_string(state));
		spa_assert_se(state != PW_FILTER_STATE_ERROR);

		if (state == PW_FILTER_STATE_PAUSED)
			break;

		roundtrip(core, loop);
	}
	spa_assert_se(node_count == 1);

	printf("add port\n");
	/* make an audio DSP output port */
	port = pw_filter_add_port(filter,
			PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, "output",
				NULL),
			NULL, 0);

	printf("wait port\n");
	roundtrip(core, loop);

	spa_assert_se(port_count == 1);
	printf("port added\n");

	printf("remove port\n");
	pw_filter_remove_port(port);
	roundtrip(core, loop);

	printf("destroy\n");
	/* check destroy */
	pw_filter_destroy(filter);

	pw_proxy_destroy((struct pw_proxy*)registry);

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

struct latest_output_test {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_node *node;
	void *port;
};

static void latest_output_test_init(struct latest_output_test *test)
{
	*test = (struct latest_output_test) { 0 };
	test->loop = pw_main_loop_new(NULL);
	spa_assert_se(test->loop != NULL);
	test->context = pw_context_new(pw_main_loop_get_loop(test->loop), NULL, 12);
	spa_assert_se(test->context != NULL);
	test->core = pw_context_connect_self(test->context, NULL, 0);
	spa_assert_se(test->core != NULL);
	test->filter = pw_filter_new(test->core, "latest-buffer-test", NULL);
	spa_assert_se(test->filter != NULL);
	spa_assert_se(pw_filter_connect(test->filter, PW_FILTER_FLAG_RT_PROCESS,
			NULL, 0) >= 0);
	test->port = pw_filter_add_port(test->filter, PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_NONE, 0, NULL, NULL, 0);
	spa_assert_se(test->port != NULL);
	test->node = pw_impl_node_get_implementation(test->filter->node);
	spa_assert_se(test->node != NULL);
}

static void latest_output_test_add_link(struct latest_output_test *test,
		struct spa_io_buffers_latest_link *link)
{
	spa_assert_se(spa_node_port_set_io(test->node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, link, sizeof(*link)) == 0);
}

static void latest_output_test_use_buffers(struct latest_output_test *test,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	spa_assert_se(spa_node_port_use_buffers(test->node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, n_buffers) == 0);
}

static void latest_output_test_clear(struct latest_output_test *test)
{
	pw_filter_destroy(test->filter);
	pw_context_destroy(test->context);
	pw_main_loop_destroy(test->loop);
}

static void test_latest_buffer_fanout(void)
{
	struct latest_output_test test;
	struct spa_io_buffers_latest latest[2] = {
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
	};
	struct spa_io_buffers_latest_link links[2] = {
		{
			.id = 10,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[0],
			.notify_fd = -1,
		}, {
			.id = 11,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[1],
			.notify_fd = -1,
		},
	};
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_filter_buffer_latest_stats stats;
	struct pw_buffer *b0, *b1;
	uint32_t id;
	int res;

	latest_output_test_init(&test);
	latest_output_test_add_link(&test, &links[0]);
	latest_output_test_add_link(&test, &links[1]);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));

	/* A progressive producer lease and two consumer leases coexist. */
	b0 = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(b0 != NULL);
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, b0) == 0);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[0], &id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[1], &id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(pw_filter_end_progressive_buffer(test.port, b0) == 0);

	/* The other buffer remains usable and can be reclaimed from both ready slots. */
	b1 = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(b1 != NULL && b1 != b0);
	spa_assert_se(pw_filter_queue_buffer(test.port, b1) == 0);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == b1);

	/* Ending the producer lease is not enough: every subscriber must return. */
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);
	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[0], 0) == 0);
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);
	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[1], 0) == 0);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == b0);

	/* Retirement releases only that subscriber's outstanding lease. */
	spa_assert_se(pw_filter_queue_buffer(test.port, b1) == 0);
	links[1].flags = 0;
	links[1].io = NULL;
	latest_output_test_add_link(&test, &links[1]);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == b1);
	res = pw_filter_get_buffer_latest_stats(test.port, &stats);
	spa_assert_se(res == 0);
	spa_assert_se(stats.subscriber_retirements == 1);
	spa_assert_se(stats.retired_leases == 1);

	latest_output_test_clear(&test);
}

static void test_progressive_buffer_consumer_first(void)
{
	struct latest_output_test test;
	struct spa_io_buffers_latest latest[2] = {
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
	};
	struct spa_io_buffers_latest_link links[2] = {
		{
			.id = 20,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[0],
			.notify_fd = -1,
		}, {
			.id = 21,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[1],
			.notify_fd = -1,
		},
	};
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_filter_buffer_latest_stats stats;
	struct pw_buffer *active, *held;
	uint32_t active_id, id;

	latest_output_test_init(&test);
	latest_output_test_add_link(&test, &links[0]);
	latest_output_test_add_link(&test, &links[1]);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));

	active = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(active != NULL);
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, active) == 0);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[0], &active_id) == 0);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[1], &id) == 0);
	spa_assert_se(id == active_id);

	/* Occupy the spare slot so only the progressive allocation can recycle. */
	held = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(held != NULL && held != active);
	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[0],
			active_id) == 0);
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);

	/* One returned consumer lease and producer completion are still insufficient. */
	spa_assert_se(pw_filter_end_progressive_buffer(test.port, active) == 0);
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);

	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[1],
			active_id) == 0);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == active);
	spa_assert_se(pw_filter_get_buffer_latest_stats(test.port, &stats) == 0);
	spa_assert_se(stats.recycle_returns == 2);
	spa_assert_se(stats.pool_exhaustions == 2);

	latest_output_test_clear(&test);
}

static void test_progressive_buffer_live_membership(void)
{
	struct latest_output_test test;
	struct spa_io_buffers_latest latest[4] = {
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
	};
	struct spa_io_buffers_latest_link links[3] = {
		{
			.id = 30,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[0],
			.notify_fd = -1,
		}, {
			.id = 31,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[1],
			.notify_fd = -1,
		}, {
			.id = 32,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[2],
			.notify_fd = -1,
		},
	};
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_filter_buffer_latest_stats stats;
	struct pw_buffer *first, *second;
	uint32_t first_id, second_id, id;

	latest_output_test_init(&test);
	latest_output_test_add_link(&test, &links[0]);
	latest_output_test_add_link(&test, &links[1]);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));

	first = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(first != NULL);
	spa_assert_se(first->buffer == buffers[0] || first->buffer == buffers[1]);
	first_id = first->buffer == buffers[0] ? 0 : 1;
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, first) == 0);

	/* A subscriber joining mid-publication starts with the next buffer. */
	latest_output_test_add_link(&test, &links[2]);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[2], &id) == -EPIPE);

	second = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(second != NULL && second != first);
	spa_assert_se(second->buffer == buffers[0] || second->buffer == buffers[1]);
	second_id = second->buffer == buffers[0] ? 0 : 1;
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, second) == 0);

	/* Supersession releases only the unclaimed consumer leases on the first buffer. */
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);
	spa_assert_se(pw_filter_end_progressive_buffer(test.port, first) == 0);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == first);

	spa_assert_se(spa_io_buffers_latest_acquire(&latest[0], &id) == 0);
	spa_assert_se(id == second_id);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[1], &id) == 0);
	spa_assert_se(id == second_id);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[2], &id) == 0);
	spa_assert_se(id == second_id);

	/* Retiring one claimed subscriber releases only its lease. */
	links[1].flags = 0;
	links[1].io = NULL;
	latest_output_test_add_link(&test, &links[1]);
	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[0], second_id) == 0);
	spa_assert_se(spa_io_buffers_latest_push_recycle(&latest[2], second_id) == 0);
	errno = 0;
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == NULL);
	spa_assert_se(errno == EPIPE);
	spa_assert_se(pw_filter_end_progressive_buffer(test.port, second) == 0);
	spa_assert_se(pw_filter_dequeue_buffer(test.port) == second);

	/* The acknowledged slot can be reused without exposing the retired mailbox. */
	links[1] = (struct spa_io_buffers_latest_link) {
		.id = 33,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest[3],
		.notify_fd = -1,
	};
	latest_output_test_add_link(&test, &links[1]);
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, first) == 0);
	spa_assert_se(spa_io_buffers_latest_acquire(&latest[3], &id) == 0);
	spa_assert_se(id == first_id);

	spa_assert_se(pw_filter_get_buffer_latest_stats(test.port, &stats) == 0);
	spa_assert_se(stats.subscriber_retirements == 1);
	spa_assert_se(stats.retired_leases == 1);
	spa_assert_se(stats.subscriber_supersessions == 2);
	spa_assert_se(stats.max_subscriber_visits == 3);

	latest_output_test_clear(&test);
}

static void test_latest_worker_lifecycle_barrier(void)
{
	struct latest_output_test test;
	struct spa_io_buffers_latest first = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest second = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest_link link = {
		.id = 40,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &first,
		.notify_fd = -1,
	};
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_buffer *active;
	uint32_t id;

	latest_output_test_init(&test);
	latest_output_test_add_link(&test, &link);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));

	spa_assert_se(pw_filter_buffer_latest_worker_begin(test.port) == 0);
	spa_assert_se(pw_filter_buffer_latest_worker_begin(test.port) == -EBUSY);
	active = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(active != NULL);
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, active) == 0);

	/* Destructive control operations cannot retire storage under the writer. */
	spa_assert_se(pw_filter_disconnect(test.filter) == -EBUSY);
	spa_assert_se(pw_filter_remove_port(test.port) == -EBUSY);
	spa_assert_se(spa_node_port_use_buffers(test.node,
			SPA_DIRECTION_OUTPUT, 0, 0, NULL, 0) == -EBUSY);

	spa_assert_se(pw_filter_end_progressive_buffer(test.port, active) == 0);
	spa_assert_se(pw_filter_buffer_latest_worker_end(test.port) == 0);
	spa_assert_se(pw_filter_buffer_latest_worker_end(test.port) == -EINVAL);

	/* A pool generation cannot retire while its link generation is active. */
	spa_assert_se(spa_node_port_use_buffers(test.node,
			SPA_DIRECTION_OUTPUT, 0, 0, NULL, 0) == -EBUSY);
	link.flags = 0;
	link.io = NULL;
	latest_output_test_add_link(&test, &link);
	spa_assert_se(spa_node_port_use_buffers(test.node,
			SPA_DIRECTION_OUTPUT, 0, 0, NULL, 0) == 0);

	/* Restart uses a fresh mailbox identity and a newly installed pool. */
	link = (struct spa_io_buffers_latest_link) {
		.id = 41,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &second,
		.notify_fd = -1,
	};
	latest_output_test_add_link(&test, &link);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));
	spa_assert_se(spa_io_buffers_latest_acquire(&second, &id) == -EPIPE);
	spa_assert_se(pw_filter_buffer_latest_worker_begin(test.port) == 0);
	spa_assert_se(pw_filter_buffer_latest_worker_end(test.port) == 0);

	latest_output_test_clear(&test);
}

static void test_latest_buffer_subscriber_limit(void)
{
	struct latest_output_test test;
	struct spa_io_buffers_latest latest[SPA_IO_BUFFERS_LATEST_MAX_LINKS + 1];
	struct spa_io_buffers_latest_link links[SPA_IO_BUFFERS_LATEST_MAX_LINKS + 1];
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_filter_buffer_latest_stats stats;
	struct pw_buffer *active;
	uint32_t i;

	latest_output_test_init(&test);
	for (i = 0; i < SPA_N_ELEMENTS(links); i++) {
		latest[i] = SPA_IO_BUFFERS_LATEST_INIT;
		links[i] = (struct spa_io_buffers_latest_link) {
			.id = 100 + i,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[i],
			.notify_fd = -1,
		};
	}
	for (i = 0; i < SPA_IO_BUFFERS_LATEST_MAX_LINKS; i++)
		latest_output_test_add_link(&test, &links[i]);
	spa_assert_se(spa_node_port_set_io(test.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &links[i], sizeof(links[i])) == -ENOSPC);
	latest_output_test_use_buffers(&test, buffers, SPA_N_ELEMENTS(buffers));

	active = pw_filter_dequeue_buffer(test.port);
	spa_assert_se(active != NULL);
	spa_assert_se(pw_filter_begin_progressive_buffer(test.port, active) == 0);
	spa_assert_se(pw_filter_get_buffer_latest_stats(test.port, &stats) == 0);
	spa_assert_se(stats.publications == 1);
	spa_assert_se(stats.subscriber_visits == SPA_IO_BUFFERS_LATEST_MAX_LINKS);
	spa_assert_se(stats.subscriber_deliveries == SPA_IO_BUFFERS_LATEST_MAX_LINKS);
	spa_assert_se(stats.max_subscriber_visits == SPA_IO_BUFFERS_LATEST_MAX_LINKS);

	latest_output_test_clear(&test);
}

struct latest_link_update {
	struct spa_node *node;
	struct spa_io_buffers_latest_link *link;
	uint32_t done;
	int result;
};

static void *update_latest_input_link(void *data)
{
	struct latest_link_update *update = data;

	update->result = spa_node_port_set_io(update->node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, update->link,
			sizeof(*update->link));
	SPA_ATOMIC_STORE(update->done, true);
	return NULL;
}

static void poll_until_latest_link_update(
		struct pw_filter_buffer_latest_poller *poller,
		struct latest_link_update *update, pthread_t thread)
{
	struct pw_buffer *buffer;

	while (!SPA_ATOMIC_LOAD(update->done)) {
		spa_assert_se(pw_filter_buffer_latest_poller_try_dequeue(
				poller, &buffer) == 0);
		spa_assert_se(buffer == NULL);
	}
	spa_assert_se(pthread_join(thread, NULL) == 0);
	spa_assert_se(update->result == 0);
}

static void test_latest_input_poller(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_node *node;
	struct spa_io_buffers_latest latest = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest_link link = {
		.id = 20,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest,
		.notify_fd = -1,
	};
	struct spa_buffer storage[2] = { 0 };
	struct spa_buffer *buffers[2] = { &storage[0], &storage[1] };
	struct pw_filter_buffer_latest_poller poller =
		PW_FILTER_BUFFER_LATEST_POLLER_INIT;
	struct {
		struct latest_link_update update;
		pthread_t thread;
	} operation;
	struct pw_buffer *buffer, *claimed;
	void *port;
	uint32_t id;

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "latest-input-poller-test", NULL);
	spa_assert_se(filter != NULL);
	spa_assert_se(pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS,
			NULL, 0) >= 0);

	port = pw_filter_add_port(filter, PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_NONE, 0, NULL, NULL, 0);
	spa_assert_se(port != NULL);
	node = pw_impl_node_get_implementation(filter->node);
	spa_assert_se(node != NULL);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);

	spa_assert_se(pw_filter_buffer_latest_poller_init(&poller, port) == 0);
	spa_assert_se(pw_filter_buffer_latest_poller_try_dequeue(
			&poller, &buffer) == 0);
	spa_assert_se(buffer == NULL);

	/* A notification change makes a retained poller quiesce before returning. */
	link.notify_fd = 7;
	operation.update = (struct latest_link_update) {
		.node = node,
		.link = &link,
	};
	spa_assert_se(pthread_create(&operation.thread, NULL,
			update_latest_input_link, &operation.update) == 0);
	poll_until_latest_link_update(&poller, &operation.update,
			operation.thread);
	pw_filter_buffer_latest_poller_clear(&poller);

	/* Success releases the pin and finishes this polling interval. */
	spa_assert_se(pw_filter_buffer_latest_poller_init(&poller, port) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest, 0, NULL) == 0);
	spa_assert_se(pw_filter_buffer_latest_poller_try_dequeue(
			&poller, &buffer) == 1);
	spa_assert_se(buffer != NULL);
	claimed = buffer;
	spa_assert_se(pw_filter_buffer_latest_poller_try_dequeue(
			&poller, &buffer) == -EINVAL);
	spa_assert_se(pw_filter_queue_buffer(port, claimed) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest, &id) == 0);
	spa_assert_se(id == 0);

	/* Live retirement also waits until the polling worker observes it. */
	spa_assert_se(pw_filter_buffer_latest_poller_init(&poller, port) == 0);
	spa_assert_se(pw_filter_buffer_latest_poller_try_dequeue(
			&poller, &buffer) == 0);
	link.flags = 0;
	link.io = NULL;
	operation.update = (struct latest_link_update) {
		.node = node,
		.link = &link,
	};
	spa_assert_se(pthread_create(&operation.thread, NULL,
			update_latest_input_link, &operation.update) == 0);
	poll_until_latest_link_update(&poller, &operation.update,
			operation.thread);
	pw_filter_buffer_latest_poller_clear(&poller);

	pw_filter_destroy(filter);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void rendezvous_set_acquisition(struct spa_meta_acquisition *acquisition,
		uint8_t domain_byte, uint64_t generation, uint64_t sequence)
{
	uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { domain_byte };

	spa_assert_se(spa_meta_acquisition_init(acquisition));
	spa_assert_se(spa_meta_acquisition_set_identity(acquisition, domain,
			generation, sequence));
}

static void test_complete_buffer_rendezvous(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct spa_node *node;
	struct spa_io_buffers_latest latest[2] = {
		SPA_IO_BUFFERS_LATEST_INIT,
		SPA_IO_BUFFERS_LATEST_INIT,
	};
	struct spa_io_buffers_latest_link links[2] = {
		{
			.id = 50,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[0],
			.notify_fd = -1,
		}, {
			.id = 51,
			.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
			.io = &latest[1],
			.notify_fd = -1,
		},
	};
	struct spa_meta_acquisition acquisitions[2][2];
	struct spa_meta metas[2][2] = {
		{
			{ SPA_META_Acquisition, sizeof(acquisitions[0][0]),
				&acquisitions[0][0] },
			{ SPA_META_Acquisition, sizeof(acquisitions[0][1]),
				&acquisitions[0][1] },
		}, {
			{ SPA_META_Acquisition, sizeof(acquisitions[1][0]),
				&acquisitions[1][0] },
			{ SPA_META_Acquisition, sizeof(acquisitions[1][1]),
				&acquisitions[1][1] },
		},
	};
	struct spa_buffer storage[2][2] = {
		{
			{ .n_metas = 1, .metas = &metas[0][0] },
			{ .n_metas = 1, .metas = &metas[0][1] },
		}, {
			{ .n_metas = 1, .metas = &metas[1][0] },
			{ .n_metas = 1, .metas = &metas[1][1] },
		},
	};
	struct spa_buffer *buffers[2][2] = {
		{ &storage[0][0], &storage[0][1] },
		{ &storage[1][0], &storage[1][1] },
	};
	struct spa_meta_acquisition target;
	struct pw_filter_rendezvous *rendezvous;
	struct pw_filter_rendezvous_result result, repeated;
	struct pw_filter_rendezvous_stats stats;
	struct spa_meta_progressive progressive = { 0 };
	struct spa_meta progressive_metas[2];
	void *ports[2];
	uint32_t id;

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "complete-buffer-rendezvous-test", NULL);
	spa_assert_se(filter != NULL);
	spa_assert_se(pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS,
			NULL, 0) >= 0);
	ports[0] = pw_filter_add_port(filter, PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_NONE, 0, NULL, NULL, 0);
	ports[1] = pw_filter_add_port(filter, PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_NONE, 0, NULL, NULL, 0);
	spa_assert_se(ports[0] != NULL && ports[1] != NULL);
	node = pw_impl_node_get_implementation(filter->node);
	spa_assert_se(node != NULL);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &links[0], sizeof(links[0])) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 1,
			SPA_IO_BuffersLatestLink, &links[1], sizeof(links[1])) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0,
			0, buffers[0], SPA_N_ELEMENTS(buffers[0])) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 1,
			0, buffers[1], SPA_N_ELEMENTS(buffers[1])) == 0);

	spa_assert_se(pw_filter_rendezvous_new(&rendezvous, ports, 1, 1,
			PW_FILTER_RENDEZVOUS_RELEASE_COMPLETE_OR_DEADLINE) == -EINVAL);
	spa_assert_se(pw_filter_rendezvous_new(&rendezvous, ports, 2, 0,
			PW_FILTER_RENDEZVOUS_RELEASE_COMPLETE_OR_DEADLINE) == -EINVAL);
	spa_assert_se(pw_filter_rendezvous_new(&rendezvous, ports, 2, 4,
			PW_FILTER_RENDEZVOUS_RELEASE_COMPLETE_OR_DEADLINE) == -EINVAL);
	spa_assert_se(pw_filter_rendezvous_new(&rendezvous, ports, 2, 3,
			PW_FILTER_RENDEZVOUS_RELEASE_COMPLETE_OR_DEADLINE) == 0);
	spa_assert_se(pw_filter_buffer_latest_worker_begin(ports[0]) == -EBUSY);

	/* An incomplete acquisition releases at its absolute deadline. */
	rendezvous_set_acquisition(&target, 1, 1, 1);
	rendezvous_set_acquisition(&acquisitions[0][0], 1, 1, 1);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 100,
			false) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[0], 0, NULL) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 50, &result) == 0);
	spa_assert_se(pw_filter_rendezvous_get_buffer(rendezvous, 0) == NULL);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 100, &result) == 1);
	spa_assert_se(result.accepted_inputs == 1);
	spa_assert_se(result.missing_required_inputs == 2);
	spa_assert_se(result.cause == PW_FILTER_RENDEZVOUS_CAUSE_DEADLINE);
	spa_assert_se(pw_filter_rendezvous_get_buffer(rendezvous, 0) != NULL);
	spa_assert_se(pw_filter_rendezvous_get_buffer(rendezvous, 1) == NULL);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 101, &repeated) == 1);
	spa_assert_se(memcmp(&result, &repeated, sizeof(result)) == 0);
	spa_assert_se(pw_filter_rendezvous_finish(rendezvous) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[0], &id) == 0);
	spa_assert_se(id == 0);

	/* A complete required mask releases early and keeps both leases borrowed. */
	rendezvous_set_acquisition(&target, 1, 1, 2);
	rendezvous_set_acquisition(&acquisitions[0][1], 1, 1, 2);
	rendezvous_set_acquisition(&acquisitions[1][1], 1, 1, 2);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 200,
			false) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[0], 1, NULL) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[1], 1, NULL) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 150, &result) == 1);
	spa_assert_se(result.accepted_inputs == 3);
	spa_assert_se(result.missing_required_inputs == 0);
	spa_assert_se(result.cause == PW_FILTER_RENDEZVOUS_CAUSE_COMPLETE);
	spa_assert_se(pw_filter_rendezvous_get_buffer(rendezvous, 0) != NULL);
	spa_assert_se(pw_filter_rendezvous_get_buffer(rendezvous, 1) != NULL);
	spa_assert_se(pw_filter_rendezvous_finish(rendezvous) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[0], &id) == 0);
	spa_assert_se(id == 1);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[1], &id) == 0);
	spa_assert_se(id == 1);

	/* Nonmatching observations are returned during the same bounded poll. */
	rendezvous_set_acquisition(&target, 1, 1, 3);
	rendezvous_set_acquisition(&acquisitions[0][0], 1, 1, 2);
	rendezvous_set_acquisition(&acquisitions[1][0], 1, 1, 4);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 300,
			false) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[0], 0, NULL) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[1], 0, NULL) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 300, &result) == 1);
	spa_assert_se(result.accepted_inputs == 0);
	spa_assert_se(result.missing_required_inputs == 3);
	spa_assert_se(pw_filter_rendezvous_finish(rendezvous) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[0], &id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[1], &id) == 0);
	spa_assert_se(id == 0);

	spa_assert_se(pw_filter_rendezvous_get_stats(rendezvous, &stats) == 0);
	spa_assert_se(stats.accepted == 3);
	spa_assert_se(stats.stale == 1);
	spa_assert_se(stats.future == 1);
	spa_assert_se(stats.complete_releases == 1);
	spa_assert_se(stats.deadline_releases == 2);
	spa_assert_se(stats.missing_required_inputs == 3);
	spa_assert_se(stats.lease_returns == 5);

	/* Version 1 progressive buffers are outside this complete-buffer facility. */
	rendezvous_set_acquisition(&target, 1, 1, 4);
	rendezvous_set_acquisition(&acquisitions[0][0], 1, 1, 4);
	progressive_metas[0] = metas[0][0];
	progressive_metas[1] = (struct spa_meta) {
		.type = SPA_META_Progressive,
		.size = sizeof(progressive),
		.data = &progressive,
	};
	storage[0][0].n_metas = SPA_N_ELEMENTS(progressive_metas);
	storage[0][0].metas = progressive_metas;
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 400,
			false) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[0], 0, NULL) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 400, &result) == 1);
	spa_assert_se(result.accepted_inputs == 0);
	spa_assert_se(pw_filter_rendezvous_finish(rendezvous) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[0], &id) == 0);
	spa_assert_se(id == 0);
	storage[0][0].n_metas = 1;
	storage[0][0].metas = &metas[0][0];
	spa_assert_se(pw_filter_rendezvous_get_stats(rendezvous, &stats) == 0);
	spa_assert_se(stats.rejected == 1);

	/* Ordering requires an explicit discontinuity for domain replacement. */
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 400,
			false) == -ESTALE);
	rendezvous_set_acquisition(&target, 2, 1, 1);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 400,
			false) == -EXDEV);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 400,
			true) == 0);
	spa_assert_se(pw_filter_rendezvous_cancel(rendezvous) == 0);
	spa_assert_se(pw_filter_rendezvous_destroy(rendezvous) == 0);

	/* Fixed release remains pending even when every required input is ready. */
	spa_assert_se(pw_filter_rendezvous_new(&rendezvous, ports, 2, 3,
			PW_FILTER_RENDEZVOUS_RELEASE_FIXED) == 0);
	rendezvous_set_acquisition(&target, 1, 1, 4);
	rendezvous_set_acquisition(&acquisitions[0][0], 1, 1, 4);
	rendezvous_set_acquisition(&acquisitions[1][0], 1, 1, 4);
	spa_assert_se(pw_filter_rendezvous_begin(rendezvous, &target, 500,
			false) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[0], 0, NULL) == 0);
	spa_assert_se(spa_io_buffers_latest_publish(&latest[1], 0, NULL) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 499, &result) == 0);
	spa_assert_se(pw_filter_rendezvous_poll(rendezvous, 500, &result) == 1);
	spa_assert_se(result.cause == PW_FILTER_RENDEZVOUS_CAUSE_FIXED);
	spa_assert_se(pw_filter_rendezvous_reset(rendezvous) == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[0], &id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(spa_io_buffers_latest_pop_recycle(&latest[1], &id) == 0);
	spa_assert_se(id == 0);
	spa_assert_se(pw_filter_rendezvous_destroy(rendezvous) == 0);

	pw_filter_destroy(filter);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();
	test_properties();
	test_create_port();
	test_latest_buffer_fanout();
	test_progressive_buffer_consumer_first();
	test_progressive_buffer_live_membership();
	test_latest_worker_lifecycle_barrier();
	test_latest_buffer_subscriber_limit();
	test_latest_input_poller();
	test_complete_buffer_rendezvous();

	pw_deinit();

	return 0;
}
