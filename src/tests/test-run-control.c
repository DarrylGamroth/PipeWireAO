/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <pipewire/run-control.h>

static struct spa_pod *build_unknown(void *data, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String("algorithm:gain"), SPA_POD_Float(1.0f), 0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_request_values(void *data, size_t size,
		int32_t version, int64_t token, const char *state,
		bool duplicate_version)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_add(&builder,
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_VERSION),
			SPA_POD_Int(version),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_REQUEST_TOKEN),
			SPA_POD_Long(token),
			SPA_POD_String(PW_AO_RUN_CONTROL_KEY_REQUESTED_STATE),
			SPA_POD_String(state), 0);
	if (duplicate_version)
		spa_pod_builder_add(&builder,
				SPA_POD_String(PW_AO_RUN_CONTROL_KEY_VERSION),
				SPA_POD_Int(version), 0);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	uint8_t data[1024];
	struct spa_pod_builder builder;
	struct spa_pod *pod;
	struct pw_ao_run_control_request request;
	struct pw_ao_run_control_status status;

	spa_pod_builder_init(&builder, data, sizeof(data));
	pod = pw_ao_run_control_build_request(&builder, 42,
			PW_AO_RUN_CONTROL_STATE_RUNNING);
	assert(pod != NULL);
	assert(pw_ao_run_control_parse_request(pod, &request) == 0);
	assert(request.version == PW_AO_RUN_CONTROL_VERSION);
	assert(request.token == 42);
	assert(request.requested_state == PW_AO_RUN_CONTROL_STATE_RUNNING);
	assert(pw_ao_run_control_parse_status(pod, &status) == -EINVAL);

	spa_pod_builder_init(&builder, data, sizeof(data));
	pod = pw_ao_run_control_build_status(&builder, 42, -EIO,
			PW_AO_RUN_CONTROL_STATE_STOPPED);
	assert(pod != NULL);
	assert(pw_ao_run_control_parse_status(pod, &status) == 0);
	assert(status.version == PW_AO_RUN_CONTROL_VERSION);
	assert(status.completed_token == 42);
	assert(status.result == -EIO);
	assert(status.actual_state == PW_AO_RUN_CONTROL_STATE_STOPPED);
	assert(pw_ao_run_control_parse_request(pod, &request) == -EINVAL);

	assert(pw_ao_run_control_build_request(&builder, 0,
			PW_AO_RUN_CONTROL_STATE_RUNNING) == NULL);
	assert(pw_ao_run_control_build_request(&builder, 1,
			PW_AO_RUN_CONTROL_STATE_UNKNOWN) == NULL);
	pod = build_request_values(data, sizeof(data), 2, 42, "running", false);
	assert(pw_ao_run_control_parse_request(pod, &request) ==
			-EPROTONOSUPPORT);
	pod = build_request_values(data, sizeof(data), 1, 0, "running", false);
	assert(pw_ao_run_control_parse_request(pod, &request) == -EINVAL);
	pod = build_request_values(data, sizeof(data), 1, 42, "paused", false);
	assert(pw_ao_run_control_parse_request(pod, &request) == -EINVAL);
	pod = build_request_values(data, sizeof(data), 1, 42, "running", true);
	assert(pw_ao_run_control_parse_request(pod, &request) == -EINVAL);

	pod = build_unknown(data, sizeof(data));
	assert(pw_ao_run_control_parse_request(pod, &request) == -ENOENT);
	assert(pw_ao_run_control_parse_status(pod, &status) == -ENOENT);

	assert(strcmp(pw_ao_run_control_state_as_string(
			PW_AO_RUN_CONTROL_STATE_STOPPED), "stopped") == 0);
	assert(strcmp(pw_ao_run_control_state_as_string(
			PW_AO_RUN_CONTROL_STATE_RUNNING), "running") == 0);
	assert(strcmp(pw_ao_run_control_state_as_string(
			PW_AO_RUN_CONTROL_STATE_UNKNOWN), "unknown") == 0);
	return 0;
}
