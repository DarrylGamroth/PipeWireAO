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

static void test_non_row_reliable_recycling(void)
{
	struct spa_io_position position = { 0 };
	struct pw_node_activation activation = { .status = PW_NODE_ACTIVATION_FINISHED };
	struct source_capture source = {
		.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
				SPA_VERSION_NODE, &source_methods, &source),
		.consumer_activation = &activation,
	};
	struct pw_impl_node source_node = { .node = &source.node,
		.rt.position = &position }, consumer_node = { .rt.position = &position };
	struct impl output = { 0 }, input = { 0 };
	struct pw_impl_port_mix out_mix = { 0 }, in_mix = { 0 };
	struct spa_io_buffers shared = SPA_IO_BUFFERS_INIT;

	output.this.node = &source_node;
	output.this.direction = PW_DIRECTION_OUTPUT;
	output.this.output_mode = OUTPUT_MODE_RELIABLE;
	output.this.buffers.n_buffers = 3;
	input.this.node = &consumer_node;
	input.this.direction = PW_DIRECTION_INPUT;
	input.this.rt.io = SPA_IO_BUFFERS_INIT;
	out_mix.p = &output.this;
	out_mix.peer = &in_mix;
	out_mix.io[0] = out_mix.io[1] = &shared;
	in_mix.p = &input.this;
	in_mix.peer = &out_mix;
	in_mix.io[0] = in_mix.io[1] = &shared;
	spa_list_init(&output.rt.mix_list);
	spa_list_init(&input.rt.mix_list);
	spa_list_append(&output.rt.mix_list, &out_mix.rt.link);
	spa_list_append(&input.rt.mix_list, &in_mix.rt.link);

	for (uint32_t i = 0; i < 4; i++) {
		uint32_t buffer_id = i % output.this.buffers.n_buffers;

		output.this.rt.io.status = SPA_STATUS_HAVE_DATA;
		output.this.rt.io.buffer_id = buffer_id;
		spa_assert_se(tee_process_reliable(&output) >= 0);
		spa_assert_se(output.this.rt.io.status == SPA_STATUS_NEED_DATA);
		spa_assert_se(output.this.rt.io.buffer_id ==
				(i < 2 ? SPA_ID_INVALID : i - 2));
		spa_assert_se(shared.status == SPA_STATUS_HAVE_DATA);
		spa_assert_se(shared.buffer_id == buffer_id);
		input.this.rt.io.status = SPA_STATUS_NEED_DATA;
		spa_assert_se(mix_process(&input) >= 0);
		spa_assert_se(input.this.rt.io.status == SPA_STATUS_HAVE_DATA);
		spa_assert_se(input.this.rt.io.buffer_id == buffer_id);
		spa_assert_se(shared.status == SPA_STATUS_NEED_DATA);
		spa_assert_se(shared.buffer_id == (i == 0 ? SPA_ID_INVALID : i - 1));
		spa_assert_se(!in_mix.row_borrowed);
		spa_assert_se(source.releases == 0);
	}
}

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
	output.this.buffers.n_buffers = 2;
	output.this.mix = &output.mix_node;
	output.mix_methods = (struct spa_node_methods) {
		SPA_VERSION_NODE_METHODS,
		.port_reuse_buffer = tee_reuse_buffer,
		.process = tee_process_reliable,
	};
	output.mix_node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &output.mix_methods, &output);
	input.this.node = &filter_node;
	input.this.direction = PW_DIRECTION_INPUT;
	input.this.port_id = 0;
	input.this.mix = &input.mix_node;
	input.mix_methods = (struct spa_node_methods) {
		SPA_VERSION_NODE_METHODS,
		.port_reuse_buffer = mix_reuse_buffer,
		.process = mix_process,
	};
	input.mix_node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &input.mix_methods, &input);
	link.output = &output.this;
	link.input = &input.this;
	link.rt.out_mix.p = &output.this;
	link.rt.in_mix.p = &input.this;
	link.rt.out_mix.peer = &link.rt.in_mix;
	link.rt.in_mix.peer = &link.rt.out_mix;
	output.this.output_mode = OUTPUT_MODE_RELIABLE;
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

	spa_assert_se(mix_process(&input) >= 0);
	spa_assert_se(input.this.rt.io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(input.this.rt.io.buffer_id == 0);
	spa_assert_se(source.releases == 0);

	/* The first filter activation has no output and keeps its input. A
	 * request-process retry may run the mix again without new input. */
	input.this.rt.io.status = SPA_STATUS_NEED_DATA;
	input.this.rt.io.buffer_id = SPA_ID_INVALID;
	spa_assert_se(mix_process(&input) >= 0);
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
	spa_assert_se(pw_impl_port_reuse_row_output(&output.this, true) == 1);
	spa_assert_se(source.releases == 1);
	spa_assert_se(source.last_id == 0);
	spa_assert_se(input.this.rt.io.buffer_id == SPA_ID_INVALID);
	spa_assert_se(pw_impl_port_reuse_row_output(&output.this, true) == 0);
	spa_assert_se(source.releases == 1);
	link.rt.in_mix.peer = NULL;
	link.rt.out_mix.peer = NULL;
	input.this.rt.io.buffer_id = 1;
	spa_assert_se(pw_impl_port_reuse_reliable_input(&input.this) == 0);
	spa_assert_se(source.releases == 1);
}

static void test_exported_filter_return(void)
{
	struct source_capture source = {
		.node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
				SPA_VERSION_NODE, &source_methods, &source),
	};
	struct spa_io_position position = { 0 };
	struct pw_node_activation driver_activation = { 0 },
			filter_activation = { 0 }, sink_activation = { 0 };
	struct pw_node_target filter_target = { .activation = &filter_activation,
			.active = true }, sink_target = {
			.activation = &sink_activation, .active = true };
	struct pw_impl_node source_node = { 0 }, filter_node = { 0 },
			client_node = { 0 };
	struct impl server_output = { 0 }, client_input = { 0 };
	struct pw_impl_port server_input = { 0 };
	struct pw_impl_port_mix server_out_mix = { 0 },
			server_in_mix = { 0 }, client_in_mix = { 0 };
	struct spa_io_buffers shared = SPA_IO_BUFFERS_INIT;

	source.consumer_activation = &filter_activation;
	source_node.node = &source.node;
	source_node.row_transport = true;
	source_node.rt.position = &position;
	source_node.rt.target.activation = &driver_activation;
	spa_list_init(&source_node.rt.target_list);
	spa_list_append(&source_node.rt.target_list, &filter_target.link);
	spa_list_append(&source_node.rt.target_list, &sink_target.link);
	filter_node.remote = true;
	client_node.exported = true;
	client_node.rt.position = &position;
	filter_node.rt.position = &position;
	filter_node.rt.target.activation = &filter_activation;
	server_output.this.node = &source_node;
	server_output.this.mix = &server_output.mix_node;
	server_output.this.buffers.n_buffers = 2;
	server_output.mix_methods = (struct spa_node_methods) {
		SPA_VERSION_NODE_METHODS,
		.port_reuse_buffer = tee_reuse_buffer,
		.process = tee_process_reliable,
	};
	server_output.mix_node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &server_output.mix_methods, &server_output);
	server_input.node = &filter_node;
	server_in_mix.p = &server_input;
	server_in_mix.peer = &server_out_mix;
	server_in_mix.row_transport = true;
	server_in_mix.io[0] = server_in_mix.io[1] = &shared;
	server_out_mix.p = &server_output.this;
	server_out_mix.peer = &server_in_mix;
	spa_list_init(&server_input.mix_list);
	spa_list_append(&server_input.mix_list, &server_in_mix.link);
	client_input.this.node = &client_node;
	client_input.this.direction = PW_DIRECTION_INPUT;
	client_in_mix.p = &client_input.this;
	client_in_mix.row_transport = true;
	client_in_mix.io[0] = client_in_mix.io[1] = &shared;
	spa_list_init(&client_input.this.mix_list);
	spa_list_init(&client_input.rt.mix_list);
	spa_list_append(&client_input.this.mix_list, &client_in_mix.link);
	spa_list_append(&client_input.rt.mix_list, &client_in_mix.rt.link);
	shared.buffer_id = SPA_ID_INVALID;
	server_output.this.rt.io.status = SPA_STATUS_HAVE_DATA;
	server_output.this.rt.io.buffer_id = 0;
	server_out_mix.io[0] = server_out_mix.io[1] = &shared;
	spa_list_init(&server_output.rt.mix_list);
	spa_list_append(&server_output.rt.mix_list, &server_out_mix.rt.link);
	spa_assert_se(tee_process_reliable(&server_output) >= 0);
	spa_assert_se(shared.status == SPA_STATUS_HAVE_DATA && shared.buffer_id == 0);
	spa_assert_se(mix_process(&client_input) >= 0);
	spa_assert_se(client_input.this.rt.io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(client_input.this.rt.io.buffer_id == 0);
	spa_assert_se(shared.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(shared.buffer_id == SPA_ID_INVALID);

	/* The exported filter retains ID 0 while output is unavailable. */
	SPA_ATOMIC_STORE(filter_activation.status, PW_NODE_ACTIVATION_AWAKE);
	SPA_ATOMIC_STORE(driver_activation.status, PW_NODE_ACTIVATION_FINISHED);
	SPA_ATOMIC_STORE(sink_activation.status, PW_NODE_ACTIVATION_AWAKE);
	spa_assert_se(pw_impl_port_publish_row_return(&client_input.this) == 0);
	spa_assert_se(!pw_impl_node_reliable_cycle_complete(&source_node));
	spa_assert_se(source.releases == 0);

	/* A later filter activation returns the borrowed ID after its callback. */
	client_input.this.rt.io.status = SPA_STATUS_NEED_DATA;
	client_input.this.rt.io.buffer_id = 0;
	spa_assert_se(pw_impl_port_publish_row_return(&client_input.this) == 1);
	spa_assert_se(shared.buffer_id == 0);
	spa_assert_se(client_input.this.rt.io.buffer_id == SPA_ID_INVALID);
	SPA_ATOMIC_STORE(filter_activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(!pw_impl_node_reliable_cycle_complete(&source_node));
	spa_assert_se(source.releases == 0);
	SPA_ATOMIC_STORE(sink_activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pw_impl_node_reliable_cycle_complete(&source_node));
	spa_assert_se(pw_impl_port_reuse_row_output(&server_output.this, false) == 1);
	spa_assert_se(source.releases == 1 && source.last_id == 0);
	spa_assert_se(shared.buffer_id == SPA_ID_INVALID);
	spa_assert_se(pw_impl_port_reuse_row_output(&server_output.this, false) == 0);
	spa_assert_se(source.releases == 1);
	shared.buffer_id = 0;
	spa_assert_se(pw_impl_port_reuse_row_output(&server_output.this, false) == -EINVAL);
	spa_assert_se(source.releases == 1);
	shared.buffer_id = SPA_ID_INVALID;

	/* Releasing ID 0 makes the next row eligible for publication. */
	server_output.this.rt.io.status = SPA_STATUS_HAVE_DATA;
	server_output.this.rt.io.buffer_id = 1;
	spa_assert_se(tee_process_reliable(&server_output) >= 0);
	spa_assert_se(shared.status == SPA_STATUS_HAVE_DATA && shared.buffer_id == 1);
}

static void test_poll_buffer_fanout(void)
{
	struct spa_io_position position = { 0 };
	struct pw_impl_node node = { .rt.position = &position };
	struct impl output = { 0 }, inputs[2] = { 0 };
	struct spa_io_buffers io[2] = { SPA_IO_BUFFERS_INIT, SPA_IO_BUFFERS_INIT };
	struct pw_impl_port_mix send[2] = { 0 }, receive[2] = { 0 };
	struct pw_poll_buffer_loans loans = { 0 }, *result = &loans;
	struct mix_io_update unbind = { .mix = &send[1] };

	output.this.node = &node;
	output.this.direction = PW_DIRECTION_OUTPUT;
	output.this.buffers.n_buffers = 2;
	output.this.rt.io = SPA_IO_BUFFERS_INIT;
	output.poll_pool = true;
	output.poll_links = 2;
	spa_list_init(&output.rt.mix_list);
	for (uint32_t i = 0; i < 2; i++) {
		inputs[i].this.node = &node;
		inputs[i].this.direction = PW_DIRECTION_INPUT;
		inputs[i].this.rt.io = SPA_IO_BUFFERS_INIT;
		send[i].p = &output.this;
		send[i].poll_buffers = receive[i].poll_buffers = true;
		send[i].io[0] = receive[i].io[0] = &io[i];
		send[i].rt.active = true;
		spa_list_init(&inputs[i].rt.mix_list);
		spa_list_append(&inputs[i].rt.mix_list, &receive[i].rt.link);
		spa_list_append(&output.rt.mix_list, &send[i].rt.link);
	}

	output.this.rt.io = (struct spa_io_buffers) { SPA_STATUS_HAVE_DATA, 0 };
	tee_process(&output);
	spa_assert_se(output.this.rt.io.buffer_id == SPA_ID_INVALID);
	schedule_mix_input(&inputs[0]);
	schedule_mix_input(&inputs[1]);
	spa_assert_se(inputs[0].this.rt.io.buffer_id == 0);
	spa_assert_se(inputs[1].this.rt.io.buffer_id == 0);

	/* The first client returns through the SPA callback. The other still
	 * owns its buffer even if its mix I/O is detached. */
	inputs[0].this.rt.io = SPA_IO_BUFFERS_INIT;
	spa_assert_se(schedule_mix_reuse_buffer(&inputs[0], 0, 0) == 0);
	schedule_mix_input(&inputs[0]);
	do_set_mix_io(NULL, false, 0, NULL, 0, &unbind);
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == SPA_ID_INVALID);

	detach_poll_loans(NULL, false, 0, &result, sizeof(result), &send[1]);
	spa_assert_se(loans.ids == 1);
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == SPA_ID_INVALID);

	restore_poll_loans(NULL, false, 0, &loans, sizeof(loans), &output);
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == 0);
	output.this.rt.io.buffer_id = SPA_ID_INVALID;
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == SPA_ID_INVALID);

	/* A stale acknowledgment cannot return an ID from a new pool. */
	reset_poll_pool(NULL, false, 0, NULL, 0, &output);
	output.this.rt.io = (struct spa_io_buffers) { SPA_STATUS_HAVE_DATA, 0 };
	tee_process(&output);
	restore_poll_loans(NULL, false, 0, &loans, sizeof(loans), &output);
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == SPA_ID_INVALID);

	/* A full pool must allow a return to reach the driver without a new frame. */
	schedule_mix_input(&inputs[0]);
	inputs[0].this.rt.io = SPA_IO_BUFFERS_INIT;
	output.this.rt.io = (struct spa_io_buffers) { SPA_STATUS_HAVE_DATA, 1 };
	tee_process(&output);
	schedule_mix_input(&inputs[0]);
	inputs[0].this.rt.io = SPA_IO_BUFFERS_INIT;
	spa_assert_se(pw_impl_port_collect_poll_buffers(&output.this));
	inputs[0].this.rt.io = (struct spa_io_buffers) { SPA_STATUS_NEED_DATA, 0 };
	schedule_mix_input(&inputs[0]);
	spa_assert_se(!pw_impl_port_collect_poll_buffers(&output.this));
	spa_assert_se(output.this.rt.io.buffer_id == 0);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	test_non_row_reliable_recycling();
	test_poll_buffer_fanout();
	test_pending_mix_and_release();
	test_exported_filter_return();
	pw_deinit();
	return 0;
}
