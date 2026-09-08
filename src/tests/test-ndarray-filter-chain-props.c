/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/param/ndarray-utils.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>

#include <pipewire/impl.h>

enum test_mode {
	TEST_BACK_PRESSURE,
	TEST_UNSTABLE_SNAPSHOT,
	TEST_TEARDOWN,
};

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_node *node;
	struct pw_stream *source;
	struct pw_impl_module *module;
	struct spa_hook core_listener, registry_listener, node_listener;
	struct spa_hook source_listener;
	struct spa_loop_utils *original_utils;
	struct spa_loop_utils observed_utils;
	struct spa_loop_utils_methods observed_methods;
	pthread_t main_thread;
	sem_t notification;
	_Atomic(struct spa_source *) pending_event;
	atomic_bool teardown_armed;
	atomic_uint produced;
	enum test_mode mode;
	const char *filter_name;
	unsigned int target_frames;
	bool initial_observed;
	bool updates_sent;
	bool final_observed;
	bool pending_event_destroyed;
	bool teardown_done;
	int sync_seq;
	int result;
};

/* One fixture runs per process. Keep the original interface data so every
 * unmodified loop method still receives its own implementation object. */
static struct test_data *observed_data;

static int observe_signal_event(void *object SPA_UNUSED, struct spa_source *source)
{
	struct test_data *data = observed_data;
	int res = spa_loop_utils_signal_event(data->original_utils, source);

	if (res >= 0 && atomic_load_explicit(&data->teardown_armed,
			memory_order_acquire) &&
	    !pthread_equal(pthread_self(), data->main_thread)) {
		struct spa_source *expected = NULL;

		if (atomic_compare_exchange_strong_explicit(&data->pending_event,
				&expected, source, memory_order_release,
				memory_order_relaxed))
			spa_assert_se(sem_post(&data->notification) == 0);
	}
	return res;
}

static void observe_destroy_source(void *object SPA_UNUSED,
		struct spa_source *source)
{
	struct test_data *data = observed_data;

	if (source == atomic_load_explicit(&data->pending_event,
			memory_order_acquire))
		data->pending_event_destroyed = true;
	spa_loop_utils_destroy_source(data->original_utils, source);
}

static void fail(struct test_data *data, const char *message)
{
	fprintf(stderr, "%s\n", message);
	data->result = 1;
	pw_main_loop_quit(data->main_loop);
}

static void set_gain(struct test_data *data, float gain)
{
	uint8_t buffer[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod_frame object, values;
	struct spa_pod *props;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, "scale:gain");
	spa_pod_builder_float(&builder, gain);
	spa_pod_builder_pop(&builder, &values);
	props = spa_pod_builder_pop(&builder, &object);
	if (props == NULL || pw_node_set_param(data->node, SPA_PARAM_Props, 0, props) < 0)
		fail(data, "could not send graph Props");
}

static void node_param(void *userdata, int seq SPA_UNUSED, uint32_t id,
		uint32_t index SPA_UNUSED, uint32_t next SPA_UNUSED,
		const struct spa_pod *param)
{
	struct test_data *data = userdata;
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	float gain = 0.0f, active = 0.0f;
	int64_t test_value = 0, test_mirror = 0;
	bool have_gain = false, have_active = false;
	bool have_test_value = false, have_test_mirror = false;
	const char *name;
	struct spa_pod *value;

	if (id != SPA_PARAM_Props || param == NULL ||
	    (params = spa_pod_find_prop(param, NULL, SPA_PROP_params)) == NULL)
		return;
	spa_pod_parser_pod(&parser, &params->value);
	if (spa_pod_parser_push_struct(&parser, &frame) < 0) {
		fail(data, "graph Props did not contain a value struct");
		return;
	}
	while (spa_pod_parser_get_string(&parser, &name) == 0 &&
	       spa_pod_parser_get_pod(&parser, &value) == 0) {
		if (spa_streq(name, "scale:gain"))
			have_gain = spa_pod_get_float(value, &gain) == 0;
		else if (spa_streq(name, "scale:active-gain"))
			have_active = spa_pod_get_float(value, &active) == 0;
		else if (spa_streq(name, "unstable:value"))
			have_test_value = spa_pod_get_long(value, &test_value) == 0;
		else if (spa_streq(name, "unstable:mirror"))
			have_test_mirror = spa_pod_get_long(value, &test_mirror) == 0;
	}
	if (data->mode == TEST_UNSTABLE_SNAPSHOT) {
		if (!have_test_value || !have_test_mirror ||
		    !((test_value == 0 && test_mirror == 0) ||
		      (test_value == 2 && test_mirror == 20))) {
			fail(data, "graph published a mixed unstable property snapshot");
			return;
		}
		if (!data->initial_observed) {
			if (test_value != 0 || test_mirror != 0) {
				fail(data, "unexpected initial unstable property snapshot");
				return;
			}
			data->initial_observed = true;
			puts("INITIAL value=0 mirror=0");
			fflush(stdout);
		} else if (test_value == 2 && test_mirror == 20) {
			if (atomic_load_explicit(&data->produced,
					memory_order_acquire) != 2) {
				fail(data, "stable snapshot arrived before the recovery frame");
				return;
			}
			data->final_observed = true;
			puts("STABLE value=2 mirror=20");
			fflush(stdout);
			pw_main_loop_quit(data->main_loop);
		}
		return;
	}
	if (!have_gain || !have_active ||
	    (gain != 1.0f && gain != 3.0f) ||
	    (active != 1.0f && active != 3.0f)) {
		fail(data, "graph published an invalid or rejected gain");
		return;
	}
	if (!data->updates_sent) {
		if (gain != 1.0f || active != 1.0f) {
			fail(data, "unexpected initial graph gain");
			return;
		}
		/* The harness has not linked the source yet. No graph process
		 * boundary can consume the first transaction before the second. */
		data->updates_sent = true;
		set_gain(data, 3.0f);
		set_gain(data, 9.0f);
		puts("UPDATES_SENT");
		fflush(stdout);
	} else if (gain == 3.0f && active == 3.0f) {
		if (atomic_load_explicit(&data->produced, memory_order_acquire) != 1) {
			fail(data, "active gain changed without the requested source frame");
			return;
		}
		data->final_observed = true;
		puts("ACTIVE gain=3");
		fflush(stdout);
		pw_main_loop_quit(data->main_loop);
	}
}

static void node_info(void *userdata, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) &&
	    info->state == PW_NODE_STATE_ERROR)
		fail(userdata, "graph node entered ERROR after a property update");
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
	.param = node_param,
};

static void registry_global(void *userdata, uint32_t id,
		uint32_t permissions SPA_UNUSED, const char *type,
		uint32_t version SPA_UNUSED, const struct spa_dict *props)
{
	struct test_data *data = userdata;
	const char *name;
	uint32_t ids[] = { SPA_PARAM_Props };

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) || props == NULL ||
	    (name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL ||
	    !spa_streq(name, data->filter_name))
		return;
	data->node = pw_registry_bind(data->registry, id,
			PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (data->node == NULL) {
		fail(data, "could not bind graph node");
		return;
	}
	pw_node_add_listener(data->node, &data->node_listener, &node_events, data);
	pw_node_subscribe_params(data->node, ids, SPA_N_ELEMENTS(ids));
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
};

static void core_done(void *userdata, uint32_t id, int seq)
{
	struct test_data *data = userdata;

	if (id == PW_ID_CORE && data->teardown_done && seq == data->sync_seq) {
		puts("TEARDOWN pending event removed; core roundtrip completed");
		fflush(stdout);
		pw_main_loop_quit(data->main_loop);
	}
}

static void core_error(void *userdata, uint32_t id SPA_UNUSED,
		int seq SPA_UNUSED, int res, const char *message)
{
	fprintf(stderr, "core error %d: %s\n", res, message);
	fail(userdata, "unexpected protocol error");
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done,
	.error = core_error,
};

static void source_state_changed(void *userdata,
		enum pw_stream_state old SPA_UNUSED, enum pw_stream_state state,
		const char *error SPA_UNUSED)
{
	if (state == PW_STREAM_STATE_ERROR)
		fail(userdata, "source stream entered ERROR");
	else if (state == PW_STREAM_STATE_STREAMING) {
		puts("STREAMING");
		fflush(stdout);
	}
}

static void source_process(void *userdata)
{
	struct test_data *data = userdata;
	struct pw_buffer *buffer = pw_stream_dequeue_buffer(data->source);
	struct spa_data *block;
	const float value = 4.0f;

	if (buffer == NULL) {
		fail(data, "source has no buffer");
		return;
	}
	block = &buffer->buffer->datas[0];
	if (block->data == NULL || block->chunk == NULL || block->maxsize < sizeof(value) ||
	    atomic_fetch_add_explicit(&data->produced, 1,
			memory_order_acq_rel) >= data->target_frames) {
		fail(data, "source received an invalid or unexpected buffer");
		return;
	}
	memcpy(block->data, &value, sizeof(value));
	*block->chunk = (struct spa_chunk) { .size = sizeof(value), .stride = sizeof(value) };
	pw_stream_queue_buffer(data->source, buffer);
}

static const struct pw_stream_events source_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = source_state_changed,
	.process = source_process,
};

static void trigger_source(void *userdata, int fd, uint32_t mask)
{
	struct test_data *data = userdata;
	struct spa_source *pending;
	struct timespec deadline;
	struct pollfd notification;
	char command;
	int res;

	atomic_store_explicit(&data->teardown_armed,
			data->mode == TEST_TEARDOWN, memory_order_release);
	if (!(mask & SPA_IO_IN) || read(fd, &command, 1) != 1 || command != '1' ||
	    pw_stream_get_state(data->source, NULL) != PW_STREAM_STATE_STREAMING ||
	    pw_stream_trigger_process(data->source) < 0) {
		fail(data, "could not trigger source frame");
		return;
	}
	if (data->mode != TEST_TEARDOWN)
		return;

	/* Stay inside this main-loop callback until the data loop has really
	 * signalled an event. Its callback cannot run before teardown below. */
	spa_assert_se(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 5;
	do {
		res = sem_timedwait(&data->notification, &deadline);
	} while (res < 0 && errno == EINTR);
	if (res < 0) {
		fail(data, "no data-loop notification before teardown");
		return;
	}
	pending = atomic_load_explicit(&data->pending_event, memory_order_acquire);
	spa_assert_se(pending != NULL);
	notification = (struct pollfd) { .fd = pending->fd, .events = POLLIN };
	spa_assert_se(poll(&notification, 1, 0) == 1);
	spa_assert_se(notification.revents & POLLIN);
	spa_assert_se(atomic_load_explicit(&data->produced,
			memory_order_acquire) == 1);
	spa_assert_se(!data->final_observed);
	pw_impl_module_destroy(data->module);
	data->module = NULL;
	/* Matching the source removed by module teardown also identifies the
	 * observed notification as the module's event, not another client's wake. */
	spa_assert_se(data->pending_event_destroyed);
	atomic_store_explicit(&data->teardown_armed, false, memory_order_release);
	data->teardown_done = true;
	data->sync_seq = pw_core_sync(data->core, PW_ID_CORE, 0);
	spa_assert_se(data->sync_seq >= 0);
}

static void timeout(void *userdata, uint64_t count SPA_UNUSED)
{
	fail(userdata, "graph Props test timed out");
}

int main(int argc, char *argv[])
{
	struct test_data data = { 0 };
	struct pw_context *context;
	struct pw_loop *loop;
	struct spa_source *timer;
	struct spa_pod_builder builder;
	struct spa_pod *params[1];
	struct timespec deadline = { .tv_sec = 10 };
	const struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT(
		.element_type = SPA_ELEMENT_TYPE_F32_LE,
		.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
		.n_dimensions = 1, .shape = { 1 });
	uint8_t buffer[512];
	char *args;

	if (argc != 3)
		return 2;
	if (spa_streq(argv[2], "back-pressure")) {
		data.mode = TEST_BACK_PRESSURE;
		data.filter_name = "ndarray-props-filter";
		data.target_frames = 1;
	} else if (spa_streq(argv[2], "unstable-snapshot")) {
		data.mode = TEST_UNSTABLE_SNAPSHOT;
		data.filter_name = "ndarray-unstable-props-filter";
		data.target_frames = 2;
	} else if (spa_streq(argv[2], "teardown")) {
		data.mode = TEST_TEARDOWN;
		data.filter_name = "ndarray-props-filter";
		data.target_frames = 1;
	} else {
		return 2;
	}
	pw_init(&argc, &argv);
	atomic_init(&data.produced, 0);
	atomic_init(&data.pending_event, NULL);
	atomic_init(&data.teardown_armed, false);
	spa_assert_se(sem_init(&data.notification, 0, 0) == 0);
	data.main_thread = pthread_self();
	data.main_loop = pw_main_loop_new(NULL);
	spa_assert_se(data.main_loop != NULL);
	loop = pw_main_loop_get_loop(data.main_loop);
	if (data.mode == TEST_TEARDOWN) {
		data.original_utils = loop->utils;
		data.observed_utils = *loop->utils;
		data.observed_methods = *(const struct spa_loop_utils_methods *)
				loop->utils->iface.cb.funcs;
		data.observed_methods.signal_event = observe_signal_event;
		data.observed_methods.destroy_source = observe_destroy_source;
		data.observed_utils.iface.cb.funcs = &data.observed_methods;
		observed_data = &data;
		loop->utils = &data.observed_utils;
	}
	context = pw_context_new(loop, NULL, 0);
	spa_assert_se(context != NULL);
	data.core = pw_context_connect(context, NULL, 0);
	spa_assert_se(data.core != NULL);
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	spa_assert_se(data.registry != NULL);
	pw_registry_add_listener(data.registry, &data.registry_listener,
			&registry_events, &data);
	if (data.mode != TEST_UNSTABLE_SNAPSHOT)
		spa_assert_se(asprintf(&args,
			"node.name = ndarray-props-filter "
			"filter.graph = { nodes = [ { type = ndarray name = scale "
			"plugin = \"%s\" label = scale-f32 "
			"config = { shape = [ 1 ] } } ] "
			"inputs = [ \"scale:in\" ] outputs = [ ] }", argv[1]) > 0);
	else
		spa_assert_se(asprintf(&args,
			"node.name = ndarray-unstable-props-filter "
			"filter.graph = { nodes = [ { type = ndarray name = unstable "
			"plugin = \"%s\" label = recovering-properties } ] "
			"inputs = [ \"unstable:in\" ] outputs = [ ] }", argv[1]) > 0);
	data.module = pw_context_load_module(context,
			"libpipewire-module-ndarray-filter-chain",
			args, NULL);
	free(args);
	spa_assert_se(data.module != NULL);
	data.source = pw_stream_new(data.core, "ndarray Props source",
			pw_properties_new(PW_KEY_NODE_NAME, "ndarray-props-source", NULL));
	spa_assert_se(data.source != NULL);
	pw_stream_add_listener(data.source, &data.source_listener, &source_events, &data);
	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[0] = spa_format_ndarray_build(&builder, SPA_PARAM_EnumFormat, &format);
	spa_assert_se(params[0] != NULL);
	spa_assert_se(pw_stream_connect(data.source, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT,
			(const struct spa_pod **)params, SPA_N_ELEMENTS(params)) == 0);
	timer = pw_loop_add_timer(pw_main_loop_get_loop(data.main_loop), timeout, &data);
	spa_assert_se(timer != NULL);
	spa_assert_se(pw_loop_update_timer(pw_main_loop_get_loop(data.main_loop), timer,
			&deadline, NULL, false) == 0);
	spa_assert_se(pw_loop_add_io(pw_main_loop_get_loop(data.main_loop), STDIN_FILENO,
			SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false, trigger_source, &data) != NULL);
	pw_main_loop_run(data.main_loop);
	if (data.mode == TEST_TEARDOWN ? !data.teardown_done : !data.final_observed)
		data.result = 1;
	pw_stream_destroy(data.source);
	if (data.node != NULL)
		pw_proxy_destroy((struct pw_proxy *)data.node);
	pw_proxy_destroy((struct pw_proxy *)data.registry);
	if (data.module != NULL)
		pw_impl_module_destroy(data.module);
	pw_core_disconnect(data.core);
	pw_context_destroy(context);
	if (data.mode == TEST_TEARDOWN) {
		loop->utils = data.original_utils;
		observed_data = NULL;
	}
	pw_main_loop_destroy(data.main_loop);
	spa_assert_se(sem_destroy(&data.notification) == 0);
	pw_deinit();
	return data.result;
}
