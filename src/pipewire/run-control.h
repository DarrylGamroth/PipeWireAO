/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_AO_RUN_CONTROL_H
#define PIPEWIRE_AO_RUN_CONTROL_H

#include <stdint.h>

#include <spa/pod/builder.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup pw_ao_run_control PipeWireAO owner-mediated run control
 *
 * A processing-node controller writes a tokened request through the node's
 * SPA_PARAM_Props parameter. The node owner applies the request with its local
 * activation API and publishes a token-matched status through the same
 * parameter. These helpers encode and validate the Version 1 POD contract.
 */

/** \{ */

#define PW_AO_RUN_CONTROL_VERSION 1u

#define PW_AO_RUN_CONTROL_KEY_ENABLED "pipewireao.run-control"
#define PW_AO_RUN_CONTROL_KEY_VERSION "pipewireao.run-control.version"
#define PW_AO_RUN_CONTROL_KEY_REQUEST_TOKEN "pipewireao.run-control.request-token"
#define PW_AO_RUN_CONTROL_KEY_REQUESTED_STATE "pipewireao.run-control.requested-state"
#define PW_AO_RUN_CONTROL_KEY_COMPLETED_TOKEN "pipewireao.run-control.completed-token"
#define PW_AO_RUN_CONTROL_KEY_RESULT "pipewireao.run-control.result"
#define PW_AO_RUN_CONTROL_KEY_ACTUAL_STATE "pipewireao.run-control.actual-state"

enum pw_ao_run_control_state {
	PW_AO_RUN_CONTROL_STATE_UNKNOWN,
	PW_AO_RUN_CONTROL_STATE_STOPPED,
	PW_AO_RUN_CONTROL_STATE_RUNNING,
};

struct pw_ao_run_control_request {
	uint32_t version;
	int64_t token;
	enum pw_ao_run_control_state requested_state;
};

struct pw_ao_run_control_status {
	uint32_t version;
	int64_t completed_token;
	int32_t result;
	enum pw_ao_run_control_state actual_state;
};

/** Return the stable wire spelling for a run-control state. */
const char *pw_ao_run_control_state_as_string(
		enum pw_ao_run_control_state state);

/** Build one complete Version 1 request as SPA_PARAM_Props. */
struct spa_pod *pw_ao_run_control_build_request(
		struct spa_pod_builder *builder,
		int64_t token,
		enum pw_ao_run_control_state requested_state);

/** Parse one complete request. Unknown non-run-control Props return -ENOENT. */
int pw_ao_run_control_parse_request(
		const struct spa_pod *props,
		struct pw_ao_run_control_request *request);

/** Build one complete Version 1 status as SPA_PARAM_Props. */
struct spa_pod *pw_ao_run_control_build_status(
		struct spa_pod_builder *builder,
		int64_t completed_token,
		int32_t result,
		enum pw_ao_run_control_state actual_state);

/** Parse one complete Version 1 status. */
int pw_ao_run_control_parse_status(
		const struct spa_pod *props,
		struct pw_ao_run_control_status *status);

/** \} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIPEWIRE_AO_RUN_CONTROL_H */
