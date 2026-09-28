/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Exercise the actual default port tee and mix methods with a delayed input
 * release. No server or timing assumptions are needed. */
#include <pipewire/log.h>

PW_LOG_TOPIC(log_port, "pw.port");

#include "../pipewire/impl-port.c"

/* The included implementation exports unrelated port management methods.
 * These private dependencies are unreachable in this focused RT method test. */
void pw_log_log_object(enum spa_log_level level SPA_UNUSED,
		const struct spa_log_topic *topic SPA_UNUSED,
		const char *file SPA_UNUSED, int line SPA_UNUSED,
		const char *func SPA_UNUSED, uint32_t flags SPA_UNUSED,
		const void *object SPA_UNUSED)
{
	spa_assert_not_reached();
}

struct pw_control *pw_control_new(struct pw_context *context SPA_UNUSED,
		struct pw_impl_port *owner SPA_UNUSED, uint32_t id SPA_UNUSED,
		uint32_t size SPA_UNUSED, size_t user_data_size SPA_UNUSED)
{
	spa_assert_not_reached();
	return NULL;
}

void pw_control_destroy(struct pw_control *control SPA_UNUSED)
{
	spa_assert_not_reached();
}

int pw_context_freeze_recalc_graph(struct pw_context *context SPA_UNUSED)
{
	spa_assert_not_reached();
	return -ENOTSUP;
}

int pw_context_thaw_recalc_graph(struct pw_context *context SPA_UNUSED,
		const char *reason SPA_UNUSED)
{
	spa_assert_not_reached();
	return -ENOTSUP;
}

struct source_capture {
	struct spa_node node;
	struct pw_node_activation *consumer_activation;
	uint32_t releases;
	uint32_t last_id;
};

static int source_reuse_buffer(void *object, uint32_t port_id,
		uint32_t buffer_id)
{
	struct source_capture *capture = object;

	spa_assert_se(port_id == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(capture->consumer_activation->status) ==
			PW_NODE_ACTIVATION_FINISHED);
	capture->releases++;
	capture->last_id = buffer_id;
	return 0;
}

static const struct spa_node_methods source_methods = {
	SPA_VERSION_NODE_METHODS,
	.port_reuse_buffer = source_reuse_buffer,
};

static void test_pending_mix_and_release(void)
{
	struct source_capture source = {
		.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
				SPA_VERSION_NODE, &source_methods, &source),
	};
	struct spa_io_position position = { 0 };
	struct pw_impl_node source_node = { 0 }, extra_source_node = { 0 },
			filter_node = { 0 },
			sink_node = { 0 };
	struct pw_node_activation activation = { 0 };
	struct pw_node_activation driver_activation = { 0 };
	struct pw_node_activation sink_activation = { 0 };
	struct pw_node_target filter_target = { 0 }, sink_target = { 0 };
	struct pw_impl_link link = { 0 };
	struct pw_impl_port_mix extra_input = { 0 }, extra_output = { 0 };
	struct pw_impl_port extra_source = { 0 };
	struct impl output = { 0 }, input = { 0 };
	struct spa_io_buffers mix_io = SPA_IO_BUFFERS_INIT;

	source_node.node = &source.node;
	source_node.rt.position = &position;
	source_node.rt.target.activation = &driver_activation;
	source_node.driver_node = &source_node;
	source_node.row_transport = true;
	spa_list_init(&source_node.rt.target_list);
	filter_target.node = &filter_node;
	filter_target.activation = &activation;
	filter_target.active = true;
	sink_node.rt.target.activation = &sink_activation;
	sink_node.driver_node = &source_node;
	sink_target.activation = &sink_activation;
	sink_target.node = &sink_node;
	sink_target.active = true;
	spa_list_append(&source_node.rt.target_list, &filter_target.link);
	spa_list_append(&source_node.rt.target_list, &sink_target.link);
	filter_node.rt.position = &position;
	filter_node.rt.target.activation = &activation;
	filter_node.driver_node = &source_node;
	source.consumer_activation = &activation;
	output.this.node = &source_node;
	output.this.direction = PW_DIRECTION_OUTPUT;
	output.this.port_id = 0;
	output.this.mix = &output.mix_node;
	output.mix_node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &schedule_tee_node_reliable, &output);
	input.this.node = &filter_node;
	input.this.direction = PW_DIRECTION_INPUT;
	input.this.port_id = 0;
	input.this.mix = &input.mix_node;
	input.mix_node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &schedule_mix_node, &input);
	link.output = &output.this;
	link.input = &input.this;
	link.rt.out_mix.p = &output.this;
	link.rt.in_mix.p = &input.this;
	link.rt.out_mix.peer = &link.rt.in_mix;
	link.rt.in_mix.peer = &link.rt.out_mix;
	output.this.reliable = true;
	input.this.n_mix = 1;
	spa_list_init(&input.this.mix_list);
	spa_list_append(&input.this.mix_list, &link.rt.in_mix.link);
	extra_output.p = &extra_source;
	extra_source.node = &extra_source_node;
	extra_input.peer = &extra_output;
	spa_assert_se(pw_impl_port_init_mix(&input.this, &extra_input) == -EBUSY);
	source_node.row_transport = false;
	extra_source_node.row_transport = true;
	spa_assert_se(pw_impl_port_init_mix(&input.this, &extra_input) == -EBUSY);
	source_node.row_transport = true;
	link.rt.out_mix.io[0] = link.rt.out_mix.io[1] = &mix_io;
	link.rt.in_mix.io[0] = link.rt.in_mix.io[1] = &mix_io;
	spa_list_init(&output.rt.mix_list);
	spa_list_init(&input.rt.mix_list);
	spa_list_append(&output.rt.mix_list, &link.rt.out_mix.rt.link);
	spa_list_append(&input.rt.mix_list, &link.rt.in_mix.rt.link);

	output.this.rt.io.status = SPA_STATUS_HAVE_DATA;
	output.this.rt.io.buffer_id = 0;
	mix_io.status = SPA_STATUS_NEED_DATA;
	mix_io.buffer_id = SPA_ID_INVALID;
	spa_assert_se(tee_process_reliable(&output) >= 0);
	spa_assert_se(mix_io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(mix_io.buffer_id == 0);

	/* The camera has a second ready block, but the first activation is
	 * waiting for the consumer and its output buffer. It must stay pending. */
	output.this.rt.io.status = SPA_STATUS_HAVE_DATA;
	output.this.rt.io.buffer_id = 1;
	spa_assert_se(tee_process_reliable(&output) >= 0);
	spa_assert_se(mix_io.buffer_id == 0);
	spa_assert_se(output.this.rt.io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(output.this.rt.io.buffer_id == 1);
	spa_assert_se(source.releases == 0);

	spa_assert_se(schedule_mix_input(&input) >= 0);
	spa_assert_se(input.this.rt.io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(input.this.rt.io.buffer_id == 0);
	spa_assert_se(source.releases == 0);

	/* The first filter activation has no output and keeps its input. A
	 * request-process retry may run the mix again without new input. */
	input.this.rt.io.status = SPA_STATUS_NEED_DATA;
	input.this.rt.io.buffer_id = SPA_ID_INVALID;
	spa_assert_se(schedule_mix_input(&input) >= 0);
	spa_assert_se(input.this.rt.io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(input.this.rt.io.buffer_id == SPA_ID_INVALID);
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 0);

	/* A filter release after output return must wait until its activation
	 * finishes, then reach the source exactly once. */
	input.this.rt.io.status = SPA_STATUS_NEED_DATA;
	input.this.rt.io.buffer_id = 0;
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_AWAKE);
	SPA_ATOMIC_STORE(driver_activation.status, PW_NODE_ACTIVATION_AWAKE);
	SPA_ATOMIC_STORE(sink_activation.status, PW_NODE_ACTIVATION_AWAKE);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 0);
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 0);
	SPA_ATOMIC_STORE(driver_activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 0);
	SPA_ATOMIC_STORE(sink_activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 1);
	spa_assert_se(source.releases == 1);
	spa_assert_se(source.last_id == 0);
	spa_assert_se(input.this.rt.io.buffer_id == SPA_ID_INVALID);
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 1);
	link.rt.in_mix.peer = NULL;
	link.rt.out_mix.peer = NULL;
	input.this.rt.io.buffer_id = 1;
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 1);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	test_pending_mix_and_release();
	pw_deinit();
	return 0;
}
