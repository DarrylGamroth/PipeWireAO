/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Exercise the actual filter output reuse method after a FIFO process kept
 * its input because no output buffer was available. */
#include <pipewire/log.h>

PW_LOG_TOPIC(log_filter, "pw.filter");

#include "../pipewire/filter.c"

void pw_log_log_object(enum spa_log_level level SPA_UNUSED,
		const struct spa_log_topic *topic SPA_UNUSED,
		const char *file SPA_UNUSED, int line SPA_UNUSED,
		const char *func SPA_UNUSED, uint32_t flags SPA_UNUSED,
		const void *object SPA_UNUSED)
{
	spa_assert_not_reached();
}

int pw_impl_node_trigger(struct pw_impl_node *node SPA_UNUSED)
{
	spa_assert_not_reached();
	return -ENOTSUP;
}

struct retry_capture {
	uint32_t requests;
};

static void filter_event(void *data, const struct spa_event *event)
{
	struct retry_capture *capture = data;

	if (SPA_NODE_EVENT_ID(event) == SPA_NODE_EVENT_RequestProcess)
		capture->requests++;
}

static const struct spa_node_events events = {
	SPA_VERSION_NODE_EVENTS,
	.event = filter_event,
};

int main(int argc, char *argv[])
{
	struct filter filter = { 0 };
	struct port output = { 0 };
	struct spa_hook listener = { 0 };
	struct retry_capture capture = { 0 };
	uint32_t index;

	pw_init(&argc, &argv);
	spa_hook_list_init(&filter.hooks);
	spa_hook_list_append(&filter.hooks, &listener, &events, &capture);
	pw_map_init(&filter.ports[SPA_DIRECTION_OUTPUT], 1, 1);
	spa_assert_se(pw_map_insert_new(&filter.ports[SPA_DIRECTION_OUTPUT],
			&output) == 0);
	output.n_buffers = 1;
	output.buffers[0].id = 0;
	filter.flags = PW_FILTER_FLAG_OUTPUT_RETURN_RETRY;
	spa_assert_se(impl_port_reuse_buffer(&filter, 0, 0) == 0);
	spa_ringbuffer_get_write_index(&output.queued.ring, &index);
	spa_assert_se(index == 1);
	spa_assert_se(capture.requests == 1);
	filter.flags = 0;
	spa_assert_se(impl_port_reuse_buffer(&filter, 0, 0) == 0);
	spa_assert_se(capture.requests == 1);
	pw_map_clear(&filter.ports[SPA_DIRECTION_OUTPUT]);
	pw_deinit();
	return 0;
}
