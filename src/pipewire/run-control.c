/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdbool.h>

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/string.h>

#include <pipewire/run-control.h>

static int state_from_string(const char *value,
		enum pw_ao_run_control_state *state)
{
	if (spa_streq(value, "stopped"))
		*state = PW_AO_RUN_CONTROL_STATE_STOPPED;
	else if (spa_streq(value, "running"))
		*state = PW_AO_RUN_CONTROL_STATE_RUNNING;
	else if (spa_streq(value, "unknown"))
		*state = PW_AO_RUN_CONTROL_STATE_UNKNOWN;
	else
		return -EINVAL;
	return 0;
}

SPA_EXPORT
const char *pw_ao_run_control_state_as_string(
		enum pw_ao_run_control_state state)
{
	switch (state) {
	case PW_AO_RUN_CONTROL_STATE_STOPPED:
		return "stopped";
	case PW_AO_RUN_CONTROL_STATE_RUNNING:
		return "running";
	case PW_AO_RUN_CONTROL_STATE_UNKNOWN:
	default:
		return "unknown";
	}
}

static int begin_props(struct spa_pod_builder *builder,
		struct spa_pod_frame *object, struct spa_pod_frame *values)
{
	if (spa_pod_builder_push_object(builder, object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props) < 0 ||
	    spa_pod_builder_prop(builder, SPA_PROP_params, 0) < 0 ||
	    spa_pod_builder_push_struct(builder, values) < 0)
		return -ENOSPC;
	return 0;
}

SPA_EXPORT
struct spa_pod *pw_ao_run_control_build_request(
		struct spa_pod_builder *builder, int64_t token,
		enum pw_ao_run_control_state requested_state)
{
	struct spa_pod_frame object, values;

	if (builder == NULL || token <= 0 ||
	    (requested_state != PW_AO_RUN_CONTROL_STATE_STOPPED &&
	     requested_state != PW_AO_RUN_CONTROL_STATE_RUNNING) ||
	    begin_props(builder, &object, &values) < 0)
		return NULL;
	spa_pod_builder_add(builder,
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_VERSION),
			SPA_POD_Int(PW_AO_RUN_CONTROL_VERSION),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_REQUEST_TOKEN),
			SPA_POD_Long(token),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_REQUESTED_STATE),
			SPA_POD_String(pw_ao_run_control_state_as_string(requested_state)),
			0);
	spa_pod_builder_pop(builder, &values);
	return spa_pod_builder_pop(builder, &object);
}

SPA_EXPORT
struct spa_pod *pw_ao_run_control_build_status(
		struct spa_pod_builder *builder, int64_t completed_token,
		int32_t result, enum pw_ao_run_control_state actual_state)
{
	struct spa_pod_frame object, values;

	if (builder == NULL || completed_token < 0 ||
	    actual_state > PW_AO_RUN_CONTROL_STATE_RUNNING ||
	    begin_props(builder, &object, &values) < 0)
		return NULL;
	spa_pod_builder_add(builder,
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_VERSION),
			SPA_POD_Int(PW_AO_RUN_CONTROL_VERSION),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_COMPLETED_TOKEN),
			SPA_POD_Long(completed_token),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_RESULT),
			SPA_POD_Int(result),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_ACTUAL_STATE),
			SPA_POD_String(pw_ao_run_control_state_as_string(actual_state)),
			0);
	spa_pod_builder_pop(builder, &values);
	return spa_pod_builder_pop(builder, &object);
}

static int parse_values(const struct spa_pod *props, bool status,
		uint32_t *version, int64_t *token, int32_t *result,
		enum pw_ao_run_control_state *state)
{
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	bool have_version = false, have_token = false;
	bool have_result = false, have_state = false;
	uint32_t fields = 0;

	if (props == NULL ||
	    !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props) ||
	    SPA_POD_OBJECT_ID(props) != SPA_PARAM_Props)
		return -EINVAL;
	params = spa_pod_find_prop(props, NULL, SPA_PROP_params);
	if (params == NULL || !spa_pod_is_struct(&params->value))
		return -EINVAL;
	spa_pod_parser_pod(&parser, &params->value);
	if (spa_pod_parser_push_struct(&parser, &frame) < 0)
		return -EINVAL;
	for (;;) {
		struct spa_pod *value;
		const char *key;
		int res;

		res = spa_pod_parser_get_string(&parser, &key);
		if (res < 0) {
			if (parser.state.offset == frame.offset + SPA_POD_SIZE(&frame.pod))
				break;
			return -EINVAL;
		}
		if (spa_pod_parser_get_pod(&parser, &value) < 0)
			return -EINVAL;
		fields++;
		if (spa_streq(key, PW_AO_RUN_CONTROL_KEY_VERSION)) {
			int32_t parsed;
			if (have_version || spa_pod_get_int(value, &parsed) < 0 || parsed < 0)
				return -EINVAL;
			*version = (uint32_t)parsed;
			have_version = true;
		} else if (spa_streq(key, status
				? PW_AO_RUN_CONTROL_KEY_COMPLETED_TOKEN
				: PW_AO_RUN_CONTROL_KEY_REQUEST_TOKEN)) {
			if (have_token || spa_pod_get_long(value, token) < 0)
				return -EINVAL;
			have_token = true;
		} else if (status && spa_streq(key, PW_AO_RUN_CONTROL_KEY_RESULT)) {
			if (have_result || spa_pod_get_int(value, result) < 0)
				return -EINVAL;
			have_result = true;
		} else if (spa_streq(key, status
				? PW_AO_RUN_CONTROL_KEY_ACTUAL_STATE
				: PW_AO_RUN_CONTROL_KEY_REQUESTED_STATE)) {
			const char *parsed;
			if (have_state || spa_pod_get_string(value, &parsed) < 0 ||
			    state_from_string(parsed, state) < 0)
				return -EINVAL;
			have_state = true;
		} else {
			return fields == 1 ? -ENOENT : -EINVAL;
		}
	}
	if (!have_version || !have_token || !have_state ||
	    (status && !have_result) || fields != (status ? 4u : 3u))
		return -EINVAL;
	if (*version != PW_AO_RUN_CONTROL_VERSION)
		return -EPROTONOSUPPORT;
	if (*token < (status ? 0 : 1))
		return -EINVAL;
	if (!status && *state == PW_AO_RUN_CONTROL_STATE_UNKNOWN)
		return -EINVAL;
	return 0;
}

SPA_EXPORT
int pw_ao_run_control_parse_request(const struct spa_pod *props,
		struct pw_ao_run_control_request *request)
{
	int32_t unused = 0;

	if (request == NULL)
		return -EINVAL;
	return parse_values(props, false, &request->version, &request->token,
			&unused, &request->requested_state);
}

SPA_EXPORT
int pw_ao_run_control_parse_status(const struct spa_pod *props,
		struct pw_ao_run_control_status *status)
{
	if (status == NULL)
		return -EINVAL;
	return parse_values(props, true, &status->version,
			&status->completed_token, &status->result,
			&status->actual_state);
}
