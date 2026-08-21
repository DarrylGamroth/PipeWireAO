/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/private.h>
#include <pipewire/stream.h>

#include <spa/param/video/raw-utils.h>
#include <spa/utils/string.h>

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
		void (*state_changed) (void *data, enum pw_stream_state old,
			enum pw_stream_state state, const char *error);
		void (*control_info) (void *data, uint32_t id, const struct pw_stream_control *control);
		void (*io_changed) (void *data, uint32_t id, void *area, uint32_t size);
		void (*param_changed) (void *data, uint32_t id, const struct spa_pod *param);
		void (*add_buffer) (void *data, struct pw_buffer *buffer);
		void (*remove_buffer) (void *data, struct pw_buffer *buffer);
		void (*process) (void *data);
		void (*drained) (void *data);
		void (*command) (void *data, const struct spa_command *command);
		void (*trigger_done) (void *data);
	} test = { PW_VERSION_STREAM_EVENTS, NULL };

	struct pw_stream_events ev;

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, state_changed);
	TEST_FUNC(ev, test, control_info);
	TEST_FUNC(ev, test, io_changed);
	TEST_FUNC(ev, test, param_changed);
	TEST_FUNC(ev, test, add_buffer);
	TEST_FUNC(ev, test, remove_buffer);
	TEST_FUNC(ev, test, process);
	TEST_FUNC(ev, test, drained);
	TEST_FUNC(ev, test, command);
	TEST_FUNC(ev, test, trigger_done);

#if defined(__x86_64__) && defined(__LP64__)
	spa_assert_se(sizeof(struct pw_buffer) == 40);
	spa_assert_se(sizeof(struct pw_time) == 64);
#else
	fprintf(stderr, "%zd\n", sizeof(struct pw_buffer));
	fprintf(stderr, "%zd\n", sizeof(struct pw_time));
#endif

	spa_assert_se(PW_VERSION_STREAM_EVENTS == 2);
	spa_assert_se(sizeof(ev) == sizeof(test));

	spa_assert_se(PW_STREAM_STATE_ERROR == -1);
	spa_assert_se(PW_STREAM_STATE_UNCONNECTED == 0);
	spa_assert_se(PW_STREAM_STATE_CONNECTING == 1);
	spa_assert_se(PW_STREAM_STATE_PAUSED == 2);
	spa_assert_se(PW_STREAM_STATE_STREAMING == 3);

	spa_assert_se(pw_stream_state_as_string(PW_STREAM_STATE_ERROR) != NULL);
	spa_assert_se(pw_stream_state_as_string(PW_STREAM_STATE_UNCONNECTED) != NULL);
	spa_assert_se(pw_stream_state_as_string(PW_STREAM_STATE_CONNECTING) != NULL);
	spa_assert_se(pw_stream_state_as_string(PW_STREAM_STATE_PAUSED) != NULL);
	spa_assert_se(pw_stream_state_as_string(PW_STREAM_STATE_STREAMING) != NULL);
}

static void stream_destroy_error(void *data)
{
	spa_assert_not_reached();
}
static void stream_state_changed_error(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	spa_assert_not_reached();
}
static void stream_io_changed_error(void *data, uint32_t id, void *area, uint32_t size)
{
	spa_assert_not_reached();
}
static void stream_param_changed_error(void *data, uint32_t id, const struct spa_pod *format)
{
	spa_assert_not_reached();
}
static void stream_add_buffer_error(void *data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void stream_remove_buffer_error(void *data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void stream_process_error(void *data)
{
	spa_assert_not_reached();
}
static void stream_drained_error(void *data)
{
	spa_assert_not_reached();
}

static const struct pw_stream_events stream_events_error =
{
	PW_VERSION_STREAM_EVENTS,
        .destroy = stream_destroy_error,
        .state_changed = stream_state_changed_error,
	.io_changed = stream_io_changed_error,
	.param_changed = stream_param_changed_error,
	.add_buffer = stream_add_buffer_error,
	.remove_buffer = stream_remove_buffer_error,
	.process = stream_process_error,
	.drained = stream_drained_error
};

static int destroy_count = 0;
static void stream_destroy_count(void *data)
{
	destroy_count++;
}
static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct pw_stream_events stream_events = stream_events_error;
	struct spa_hook listener = { 0, };
	const char *error = NULL;
	struct pw_time tm;

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "test", NULL);
	spa_assert_se(stream != NULL);
	pw_stream_add_listener(stream, &listener, &stream_events, stream);

	/* check state */
	spa_assert_se(pw_stream_get_state(stream, &error) == PW_STREAM_STATE_UNCONNECTED);
	spa_assert_se(error == NULL);
	/* check name */
	spa_assert_se(spa_streq(pw_stream_get_name(stream), "test"));

	/* check id, only when connected */
	spa_assert_se(pw_stream_get_node_id(stream) == SPA_ID_INVALID);

	spa_assert_se(pw_stream_get_time_n(stream, &tm, sizeof(tm)) == -EIO);
	spa_assert_se(tm.now == 0);
	spa_assert_se(tm.rate.num == 0);
	spa_assert_se(tm.rate.denom == 0);
	spa_assert_se(tm.ticks == 0);
	spa_assert_se(tm.delay == 0);
	spa_assert_se(tm.queued == 0);
	spa_assert_se(tm.buffered == 0);

	spa_assert_se(pw_stream_dequeue_buffer(stream) == NULL);

	/* check destroy */
	destroy_count = 0;
	stream_events.destroy = stream_destroy_count;
	pw_stream_destroy(stream);
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
	struct pw_stream *stream;
	struct pw_stream_events stream_events = stream_events_error;
	struct spa_hook listener = { { NULL }, };
	struct spa_dict_item items[3];

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "test",
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL));
	spa_assert_se(stream != NULL);
	pw_stream_add_listener(stream, &listener, &stream_events, stream);

	props = pw_stream_get_properties(stream);
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
	pw_stream_update_properties(stream, &SPA_DICT_INIT(items, 3));

	spa_assert_se(props == pw_stream_get_properties(stream));
	spa_assert_se(pw_properties_get(props, "foo") == NULL);
	spa_assert_se(spa_streq(pw_properties_get(props, "biz"), "buzz"));
	spa_assert_se(spa_streq(pw_properties_get(props, "buzz"), "frizz"));

	/* check destroy */
	destroy_count = 0;
	stream_events.destroy = stream_destroy_count;
	pw_context_destroy(context);
	spa_assert_se(destroy_count == 1);

	pw_main_loop_destroy(loop);
}

static void test_latest_buffer_fanout(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_node *node;
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
	struct pw_buffer_latest_stats stats;
	struct pw_buffer *published, *returned;
	uint64_t sequence[2];
	uint32_t id[2];

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "latest-stream-test", NULL);
	spa_assert_se(stream != NULL);
	spa_assert_se(pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NONE, NULL, 0) == 0);
	node = pw_impl_node_get_implementation(stream->node);
	spa_assert_se(node != NULL);

	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &links[0], sizeof(links[0])) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &links[1], sizeof(links[1])) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_begin(stream) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_begin(stream) == -EBUSY);
	spa_assert_se(pw_stream_disconnect(stream) == -EBUSY);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);

	published = pw_stream_dequeue_buffer(stream);
	spa_assert_se(published != NULL);
	spa_assert_se(pw_stream_queue_buffer(stream, published) == 0);
	spa_assert_se(spa_io_buffers_latest_receive(&latest[0], &sequence[0],
			&id[0]) == 0);
	spa_assert_se(spa_io_buffers_latest_receive(&latest[1], &sequence[1],
			&id[1]) == 0);
	spa_assert_se(sequence[0] == sequence[1]);
	spa_assert_se(id[0] == id[1]);
	spa_assert_se(spa_io_buffers_latest_complete(&latest[0], id[0]) == 0);
	spa_assert_se(spa_io_buffers_latest_complete(&latest[1], id[1]) == 0);

	returned = pw_stream_dequeue_buffer(stream);
	spa_assert_se(returned != NULL);
	spa_assert_se(pw_stream_return_buffer(stream, returned) == 0);
	spa_assert_se(pw_stream_get_buffer_latest_stats(stream, &stats,
			sizeof(stats)) == 0);
	spa_assert_se(stats.publications == 1);
	spa_assert_se(stats.subscriber_deliveries == 2);

	links[0].flags = 0;
	links[1].flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &links[0], sizeof(links[0])) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &links[1], sizeof(links[1])) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_end(stream) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_end(stream) == -EINVAL);

	pw_stream_destroy(stream);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void test_exact_latest_video_streams(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *output, *input;
	struct pw_impl_port *output_port, *input_port;
	struct pw_impl_link *link;
	struct spa_video_info_raw info = {
		.format = SPA_VIDEO_FORMAT_GRAY8,
		.size = SPA_RECTANGLE(640, 480),
		.framerate = SPA_FRACTION(25, 1),
	};
	uint8_t storage[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	const struct spa_pod *param;
	const struct pw_properties *props;
	const struct pw_link_info *link_info;

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);

	param = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
	spa_assert_se(param != NULL);
	output = pw_stream_new(core, "exact-latest-video-output", NULL);
	spa_assert_se(output != NULL);
	spa_assert_se(pw_stream_connect(output, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NO_CONVERT | PW_STREAM_FLAG_BUFFER_LATEST,
			&param, 1) == 0);

	spa_pod_builder_init(&builder, storage, sizeof(storage));
	param = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
	spa_assert_se(param != NULL);
	input = pw_stream_new(core, "exact-latest-video-input", NULL);
	spa_assert_se(input != NULL);
	spa_assert_se(pw_stream_connect(input, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NO_CONVERT | PW_STREAM_FLAG_BUFFER_LATEST,
			&param, 1) == 0);

	output_port = pw_impl_node_find_port(output->node, PW_DIRECTION_OUTPUT, 0);
	input_port = pw_impl_node_find_port(input->node, PW_DIRECTION_INPUT, 0);
	spa_assert_se(output_port != NULL);
	spa_assert_se(input_port != NULL);
	spa_assert_se(pw_impl_node_register(output->node, NULL) == 0);
	spa_assert_se(pw_impl_node_register(input->node, NULL) == 0);
	props = pw_impl_port_get_properties(output_port);
	spa_assert_se(pw_properties_get_bool(props,
			PW_KEY_PORT_BUFFER_LATEST, false));
	props = pw_impl_port_get_properties(input_port);
	spa_assert_se(pw_properties_get_bool(props,
			PW_KEY_PORT_BUFFER_LATEST, false));

	link = pw_context_create_link(context, output_port, input_port,
			NULL, NULL, 0);
	spa_assert_se(link != NULL);
	link_info = pw_impl_link_get_info(link);
	spa_assert_se(link_info != NULL);
	spa_assert_se(spa_dict_lookup(link_info->props,
			PW_KEY_LINK_BUFFER_LATEST) != NULL);

	pw_impl_link_destroy(link);
	pw_stream_destroy(input);
	pw_stream_destroy(output);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void test_latest_buffer_input_poller(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_node *node;
	struct spa_io_buffers_latest latest = SPA_IO_BUFFERS_LATEST_INIT;
	struct spa_io_buffers_latest_link link = {
		.id = 10,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest,
		.notify_fd = -1,
	};
	struct spa_buffer storage = { 0 };
	struct spa_buffer *buffers[1] = { &storage };
	struct pw_stream_buffer_latest_poller poller;
	struct pw_buffer *buffer;
	uint64_t sequence;
	uint32_t id;

	loop = pw_main_loop_new(NULL);
	spa_assert_se(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	stream = pw_stream_new(core, "latest-input-stream-test", NULL);
	spa_assert_se(stream != NULL);
	spa_assert_se(pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_NONE, NULL, 0) == 0);
	node = pw_impl_node_get_implementation(stream->node);
	spa_assert_se(node != NULL);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0,
			0, buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_begin(stream) == 0);
	spa_assert_se(pw_stream_buffer_latest_poller_init(&poller, stream) == 0);
	spa_assert_se(pw_stream_buffer_latest_poller_try_dequeue(&poller,
			&buffer, &sequence) == 0);
	spa_assert_se(spa_io_buffers_latest_submit(&latest, 7, 0,
			NULL, NULL) == 0);
	spa_assert_se(pw_stream_buffer_latest_poller_try_dequeue(&poller,
			&buffer, &sequence) == 1);
	spa_assert_se(buffer != NULL);
	spa_assert_se(sequence == 7);
	spa_assert_se(pw_stream_queue_buffer(stream, buffer) == 0);
	spa_assert_se(spa_io_buffers_latest_reclaim_completion(&latest, &id) == 0);
	spa_assert_se(id == 0);

	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(pw_stream_buffer_latest_worker_end(stream) == 0);
	pw_stream_destroy(stream);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();
	test_properties();
	test_exact_latest_video_streams();
	test_latest_buffer_fanout();
	test_latest_buffer_input_poller();

	pw_deinit();

	return 0;
}
