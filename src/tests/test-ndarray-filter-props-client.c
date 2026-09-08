/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>
#include <pipewire/run-control.h>

enum phase {
	PHASE_INITIAL,
	PHASE_OWNER_REFRESH,
	PHASE_RUN_COMPLETION,
	PHASE_RESET_COMPLETION,
};

struct test_data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_node *node;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	struct spa_hook node_listener;
	struct spa_source *timeout_timer;
	enum phase phase;
	int prop_info_seq;
	int props_seq;
	int sync_seq;
	uint32_t prop_info_count;
	uint32_t props_count;
	bool have_gain_info;
	bool have_offset_info;
	bool have_owner_props;
	bool have_run_status;
	bool have_reset_status;
	float observed_gain;
	int64_t run_token;
	int64_t reset_token;
	int32_t run_result;
	int32_t reset_result;
	enum pw_ao_run_control_state actual_state;
	bool started;
	bool enumerating;
	int result;
};

static const char *phase_name(enum phase phase)
{
	switch (phase) {
	case PHASE_INITIAL:
		return "initial";
	case PHASE_OWNER_REFRESH:
		return "owner refresh";
	case PHASE_RUN_COMPLETION:
		return "run completion";
	case PHASE_RESET_COMPLETION:
		return "reset completion";
	}
	return "unknown";
}

static void fail(struct test_data *data, const char *message)
{
	if (data->result == 0) {
		fprintf(stderr, "%s\n", message);
		data->result = 1;
	}
	pw_main_loop_quit(data->loop);
}

static int send_sync(struct test_data *data)
{
	data->sync_seq = pw_core_sync(data->core, PW_ID_CORE, 0);
	if (data->sync_seq < 0) {
		fail(data, "could not synchronize with the daemon");
		return data->sync_seq;
	}
	return 0;
}

static void begin_enumeration(struct test_data *data)
{
	int result;

	data->prop_info_count = 0;
	data->props_count = 0;
	data->have_gain_info = false;
	data->have_offset_info = false;
	data->have_owner_props = false;
	data->have_run_status = false;
	data->have_reset_status = false;
	data->observed_gain = 0.0f;
	data->run_token = -1;
	data->reset_token = -1;
	data->run_result = -1;
	data->reset_result = -1;
	data->actual_state = PW_AO_RUN_CONTROL_STATE_UNKNOWN;
	data->enumerating = true;
	result = pw_node_enum_params(data->node, 0,
			SPA_PARAM_PropInfo, 0, UINT32_MAX, NULL);
	if (result < 0) {
		fail(data, "could not enumerate PropInfo");
		return;
	}
	data->prop_info_seq = result;
	result = pw_node_enum_params(data->node, 0,
			SPA_PARAM_Props, 0, UINT32_MAX, NULL);
	if (result < 0) {
		fail(data, "could not enumerate Props");
		return;
	}
	data->props_seq = result;
	(void)send_sync(data);
}

static struct spa_pod *build_owner_request(struct spa_pod_builder *builder,
		float gain)
{
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	spa_pod_builder_add(builder,
			SPA_POD_String("owner:gain"), SPA_POD_Float(gain), 0);
	spa_pod_builder_pop(builder, &values);
	return spa_pod_builder_pop(builder, &object);
}

static void request_next_phase(struct test_data *data)
{
	uint8_t buffer[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *request = NULL;
	int result = 0;

	switch (data->phase) {
	case PHASE_INITIAL:
		request = build_owner_request(&builder, 2.0f);
		result = request == NULL ? -ENOSPC : 0;
		break;
	case PHASE_OWNER_REFRESH:
		request = pw_ao_run_control_build_request(&builder, 10,
				PW_AO_RUN_CONTROL_STATE_STOPPED);
		result = request == NULL ? -ENOSPC : 0;
		break;
	case PHASE_RUN_COMPLETION:
		request = pw_ao_reset_control_build_request(&builder, 20);
		result = request == NULL ? -ENOSPC : 0;
		break;
	case PHASE_RESET_COMPLETION:
		puts("RESULT public Props surface retained across all updates");
		fflush(stdout);
		pw_main_loop_quit(data->loop);
		return;
	}
	if (result < 0) {
		fail(data, "could not build Props request");
		return;
	}
	if (request == NULL || pw_node_set_param(data->node, SPA_PARAM_Props,
			0, request) < 0) {
		fail(data, "could not update node Props");
		return;
	}
	(void)send_sync(data);
}

static bool parse_owner_props(const struct spa_pod *param, float *gain)
{
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	const char *name;
	struct spa_pod *value;
	bool found = false;

	params = spa_pod_find_prop(param, NULL, SPA_PROP_params);
	if (params == NULL)
		return false;
	spa_pod_parser_pod(&parser, &params->value);
	if (spa_pod_parser_push_struct(&parser, &frame) < 0)
		return false;
	while (spa_pod_parser_get_string(&parser, &name) == 0 &&
	       spa_pod_parser_get_pod(&parser, &value) == 0) {
		if (!spa_streq(name, "owner:gain") || found ||
		    spa_pod_get_float(value, gain) < 0)
			return false;
		found = true;
	}
	return found;
}

static void node_param(void *userdata, int seq, uint32_t id,
		uint32_t index SPA_UNUSED, uint32_t next SPA_UNUSED,
		const struct spa_pod *param)
{
	struct test_data *data = userdata;

	if (param == NULL || (id == SPA_PARAM_PropInfo &&
			seq != data->prop_info_seq) ||
	    (id == SPA_PARAM_Props && seq != data->props_seq))
		return;
	if (id == SPA_PARAM_PropInfo) {
		const struct spa_pod_prop *name = spa_pod_find_prop(param, NULL,
				SPA_PROP_INFO_name);
		const char *value = NULL;

		if (name == NULL || spa_pod_get_string(&name->value, &value) < 0) {
			fail(data, "malformed PropInfo from public node proxy");
			return;
		}
		data->prop_info_count++;
		data->have_gain_info |= spa_streq(value, "owner:gain");
		data->have_offset_info |= spa_streq(value, "owner:offset");
	} else if (id == SPA_PARAM_Props) {
		struct pw_ao_run_control_status run_status;
		struct pw_ao_reset_control_status reset_status;
		float gain;

		data->props_count++;
		if (pw_ao_run_control_parse_status(param, &run_status) == 0) {
			data->have_run_status = true;
			data->run_token = run_status.completed_token;
			data->run_result = run_status.result;
			data->actual_state = run_status.actual_state;
		} else if (pw_ao_reset_control_parse_status(param, &reset_status) == 0) {
			data->have_reset_status = true;
			data->reset_token = reset_status.completed_token;
			data->reset_result = reset_status.result;
		} else if (parse_owner_props(param, &gain)) {
			data->have_owner_props = true;
			data->observed_gain = gain;
		} else {
			fail(data, "unknown Props object from public node proxy");
		}
	}
}

static void verify_enumeration(struct test_data *data)
{
	float expected_gain = data->phase == PHASE_INITIAL ? 1.0f : 2.0f;
	int64_t expected_run = data->phase < PHASE_RUN_COMPLETION ? 0 : 10;
	int64_t expected_reset = data->phase < PHASE_RESET_COMPLETION ? 0 : 20;

	if (data->prop_info_count != 2 || !data->have_gain_info ||
	    !data->have_offset_info || data->props_count != 3 ||
	    !data->have_owner_props || !data->have_run_status ||
	    !data->have_reset_status || data->observed_gain != expected_gain ||
	    data->run_token != expected_run || data->reset_token != expected_reset) {
		fprintf(stderr, "%s enumeration incomplete: PropInfo=%u gain-info=%u "
				"offset-info=%u Props=%u owner=%u run=%lld reset=%lld gain=%g\n",
				phase_name(data->phase), data->prop_info_count,
				data->have_gain_info, data->have_offset_info, data->props_count,
				data->have_owner_props, (long long)data->run_token,
				(long long)data->reset_token, (double)data->observed_gain);
		fail(data, "public node parameter surface was not retained");
		return;
	}
	if (data->run_result != 0 || data->reset_result != 0 ||
	    data->actual_state != PW_AO_RUN_CONTROL_STATE_STOPPED) {
		fail(data, "public node control status was not successful and stopped");
		return;
	}
	printf("ENUMERATED %s PropInfo=2 Props=3\n", phase_name(data->phase));
	fflush(stdout);
	data->enumerating = false;
	request_next_phase(data);
}

static void core_done(void *userdata, uint32_t id, int seq)
{
	struct test_data *data = userdata;

	if (id != PW_ID_CORE || seq != data->sync_seq)
		return;
	if (!data->started) {
		data->started = true;
		begin_enumeration(data);
		return;
	}
	if (data->enumerating) {
		verify_enumeration(data);
		return;
	}
	data->phase++;
	begin_enumeration(data);
}

static void core_error(void *userdata, uint32_t id SPA_UNUSED,
		int seq SPA_UNUSED, int res, const char *message)
{
	char error[256];

	snprintf(error, sizeof(error), "core error %d: %s", res,
			message == NULL ? "unknown" : message);
	fail(userdata, error);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done,
	.error = core_error,
};

static void node_info(void *userdata, const struct pw_node_info *info)
{
	struct test_data *data = userdata;

	if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) &&
	    info->state == PW_NODE_STATE_ERROR)
		fail(data, "ndarray filter entered ERROR");
	else if (!data->started)
		(void)send_sync(data);
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

	if (data->node != NULL || !spa_streq(type, PW_TYPE_INTERFACE_Node) ||
	    props == NULL || (name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL ||
	    !spa_streq(name, "ndarray-props-filter"))
		return;
	data->node = pw_registry_bind(data->registry, id, PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE, 0);
	if (data->node == NULL) {
		fail(data, "could not bind ndarray filter node");
		return;
	}
	pw_node_add_listener(data->node, &data->node_listener, &node_events, data);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
};

static void timeout(void *userdata, uint64_t count SPA_UNUSED)
{
	fail(userdata, "public node parameter test timed out");
}

int main(int argc, char *argv[])
{
	struct test_data data = { 0 };
	struct timespec deadline = { .tv_sec = 10 };

	pw_init(&argc, &argv);
	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "could not create main loop\n");
		data.result = 1;
		goto done;
	}
	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "could not create context\n");
		data.result = 1;
		goto done;
	}
	data.core = pw_context_connect(data.context, NULL, 0);
	if (data.core == NULL) {
		fprintf(stderr, "could not connect core\n");
		data.result = 1;
		goto done;
	}
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	if (data.registry == NULL) {
		fprintf(stderr, "could not get registry\n");
		data.result = 1;
		goto done;
	}
	pw_registry_add_listener(data.registry, &data.registry_listener,
			&registry_events, &data);
	data.timeout_timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
			timeout, &data);
	if (data.timeout_timer == NULL || pw_loop_update_timer(
			pw_main_loop_get_loop(data.loop), data.timeout_timer, &deadline,
			NULL, false) < 0) {
		fprintf(stderr, "could not schedule timeout\n");
		data.result = 1;
		goto done;
	}
	pw_main_loop_run(data.loop);

done:
	if (data.node != NULL)
		pw_proxy_destroy((struct pw_proxy *)data.node);
	if (data.registry != NULL)
		pw_proxy_destroy((struct pw_proxy *)data.registry);
	if (data.core != NULL)
		pw_core_disconnect(data.core);
	if (data.context != NULL)
		pw_context_destroy(data.context);
	if (data.loop != NULL)
		pw_main_loop_destroy(data.loop);
	pw_deinit();
	return data.result;
}
