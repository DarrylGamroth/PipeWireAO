/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Compile the helper into this test to control its RT-side queues without a
 * scheduler race or a production fault-injection interface. */
#define pw_filter_dequeue_buffer test_filter_dequeue_buffer
#define pw_filter_queue_buffer test_filter_queue_buffer
#define pw_filter_trigger_process test_filter_trigger_process
#include "../pipewire/ndarray-filter.c"
#undef pw_filter_trigger_process
#undef pw_filter_queue_buffer
#undef pw_filter_dequeue_buffer

#define TEST_CAPACITY 8u

struct test_queue {
	struct pw_buffer *available[TEST_CAPACITY];
	struct pw_buffer *returned[TEST_CAPACITY];
	uint32_t n_available;
	uint32_t next_available;
	uint32_t n_returned;
};

struct test_buffer {
	struct pw_buffer pw;
	struct spa_buffer spa;
	struct spa_data data;
	struct spa_chunk chunk;
	float value;
};

struct test_fixture {
	struct pw_ndarray_filter filter;
	struct ndarray_port input;
	struct ndarray_port output;
	struct ndarray_port *inputs[1];
	struct ndarray_port *data_inputs[1];
	struct ndarray_port *outputs[1];
	struct pw_buffer *input_buffers[1];
	bool input_available[1];
	struct pw_buffer *output_buffers[1];
	struct pw_ndarray_filter_buffer process_inputs[1];
	struct pw_ndarray_filter_buffer process_outputs[1];
	struct memory_region regions[2 * MAX_BUFFER_REGIONS];
	uint32_t n_regions[2];
	struct test_queue input_queue;
	struct test_queue output_queue;
	float observed[TEST_CAPACITY];
	void *first_output;
	uint32_t callbacks;
	uint32_t trigger_calls;
	bool retain_first_output;
};

static struct test_fixture *active_fixture;

struct pw_buffer *test_filter_dequeue_buffer(void *port_data)
{
	struct test_queue *queue = port_data;

	if (queue->next_available == queue->n_available)
		return NULL;
	return queue->available[queue->next_available++];
}

int test_filter_queue_buffer(void *port_data, struct pw_buffer *buffer)
{
	struct test_queue *queue = port_data;

	spa_assert_se(queue->n_returned < TEST_CAPACITY);
	queue->returned[queue->n_returned++] = buffer;
	return 0;
}

int test_filter_trigger_process(struct pw_filter *filter SPA_UNUSED)
{
	active_fixture->trigger_calls++;
	return 0;
}

static int owner_process(void *data,
		const struct pw_ndarray_filter_buffer *inputs, uint32_t n_inputs,
		struct pw_ndarray_filter_buffer *outputs, uint32_t n_outputs)
{
	struct test_fixture *fixture = data;

	spa_assert_se(n_inputs == 1);
	spa_assert_se(n_outputs == 1);
	spa_assert_se(inputs[0].data != NULL);
	spa_assert_se(outputs[0].data != NULL);
	spa_assert_se(fixture->callbacks < TEST_CAPACITY);
	memcpy(&fixture->observed[fixture->callbacks], inputs[0].data,
			sizeof(float));
	if (fixture->callbacks == 0) {
		fixture->first_output = outputs[0].data;
		if (fixture->retain_first_output)
			outputs[0].flags |=
				PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE;
	} else if (fixture->retain_first_output) {
		spa_assert_se(outputs[0].data == fixture->first_output);
	}
	fixture->callbacks++;
	return 0;
}

static void init_buffer(struct test_buffer *buffer, float value)
{
	spa_zero(*buffer);
	buffer->value = value;
	buffer->chunk.size = sizeof(value);
	buffer->spa.n_datas = 1;
	buffer->spa.datas = &buffer->data;
	buffer->data.data = &buffer->value;
	buffer->data.maxsize = sizeof(buffer->value);
	buffer->data.chunk = &buffer->chunk;
	buffer->pw.buffer = &buffer->spa;
}

static void make_available(struct test_queue *queue,
		struct test_buffer *buffer)
{
	spa_assert_se(queue->n_available < TEST_CAPACITY);
	queue->available[queue->n_available++] = &buffer->pw;
}

static void init_fixture(struct test_fixture *fixture, uint32_t flags)
{
	static const uint32_t shape[] = { 1 };
	static const struct pw_ndarray_filter_events events = {
		PW_VERSION_NDARRAY_FILTER_EVENTS,
		.process = owner_process,
	};

	spa_zero(*fixture);
	fixture->input = (struct ndarray_port) {
		.filter = &fixture->filter,
		.data_index = 0,
		.direction = SPA_DIRECTION_INPUT,
		.format = {
			.element_type = SPA_ELEMENT_TYPE_F32_LE,
			.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
			.n_dimensions = 1,
			.shape = (uint32_t *)shape,
		},
		.size = sizeof(float),
		.stride = sizeof(float),
		.filter_port = &fixture->input_queue,
	};
	fixture->output = fixture->input;
	fixture->output.direction = SPA_DIRECTION_OUTPUT;
	fixture->output.filter_port = &fixture->output_queue;
	fixture->inputs[0] = &fixture->input;
	fixture->data_inputs[0] = &fixture->input;
	fixture->outputs[0] = &fixture->output;
	fixture->filter.filter = (struct pw_filter *)(uintptr_t)1;
	fixture->filter.main_loop = pw_main_loop_new(NULL);
	spa_assert_se(fixture->filter.main_loop != NULL);
	if (flags & PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS) {
		fixture->filter.fifo_process_event = pw_loop_add_event(
				pw_main_loop_get_loop(fixture->filter.main_loop),
				fifo_process_event, &fixture->filter);
		spa_assert_se(fixture->filter.fifo_process_event != NULL);
	}
	fixture->filter.events = events;
	fixture->filter.user_data = fixture;
	fixture->filter.flags = flags;
	fixture->filter.inputs = fixture->inputs;
	fixture->filter.data_inputs = fixture->data_inputs;
	fixture->filter.outputs = fixture->outputs;
	fixture->filter.n_inputs = 1;
	fixture->filter.n_data_inputs = 1;
	fixture->filter.n_outputs = 1;
	fixture->filter.input_buffers = fixture->input_buffers;
	fixture->filter.input_available = fixture->input_available;
	fixture->filter.output_buffers = fixture->output_buffers;
	fixture->filter.process_inputs = fixture->process_inputs;
	fixture->filter.process_outputs = fixture->process_outputs;
	fixture->filter.buffer_regions = fixture->regions;
	fixture->filter.n_buffer_regions = fixture->n_regions;
	atomic_init(&fixture->filter.error, 0);
	atomic_init(&fixture->filter.prepared, true);
	atomic_init(&fixture->filter.destroying, false);
	atomic_init(&fixture->filter.fifo_process_scheduled, false);
	active_fixture = fixture;
}

static void dispatch_fifo_event(struct test_fixture *fixture)
{
	struct pw_loop *loop = pw_main_loop_get_loop(fixture->filter.main_loop);

	pw_loop_enter(loop);
	spa_assert_se(pw_loop_iterate(loop, 0) >= 0);
	pw_loop_leave(loop);
}

static void clear_fixture(struct test_fixture *fixture)
{
	if (fixture->filter.fifo_process_event != NULL)
		pw_loop_destroy_source(pw_main_loop_get_loop(
				fixture->filter.main_loop),
				fixture->filter.fifo_process_event);
	pw_main_loop_destroy(fixture->filter.main_loop);
}

static void test_default_admission_keeps_latest(void)
{
	struct test_fixture fixture;
	struct test_buffer input1, input2, output;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_NONE);
	init_buffer(&input1, 1.0f);
	init_buffer(&input2, 2.0f);
	init_buffer(&output, 0.0f);
	make_available(&fixture.input_queue, &input1);
	make_available(&fixture.input_queue, &input2);
	make_available(&fixture.output_queue, &output);

	process(&fixture.filter, NULL);

	spa_assert_se(fixture.callbacks == 1);
	spa_assert_se(fixture.observed[0] == 2.0f);
	spa_assert_se(fixture.input_queue.n_returned == 2);
	spa_assert_se(fixture.trigger_calls == 0);
	clear_fixture(&fixture);
}

static void test_fifo_admission_preserves_order_and_progress(void)
{
	struct test_fixture fixture;
	struct test_buffer input1, input2, output1, output2;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS);
	init_buffer(&input1, 1.0f);
	init_buffer(&input2, 2.0f);
	init_buffer(&output1, 0.0f);
	init_buffer(&output2, 0.0f);
	make_available(&fixture.input_queue, &input1);
	make_available(&fixture.input_queue, &input2);
	make_available(&fixture.output_queue, &output1);
	make_available(&fixture.output_queue, &output2);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 1);
	spa_assert_se(fixture.observed[0] == 1.0f);
	spa_assert_se(fixture.filter.input_buffers[0] == &input2.pw);
	spa_assert_se(fixture.trigger_calls == 0);
	spa_assert_se(atomic_load_explicit(
			&fixture.filter.fifo_process_scheduled,
			memory_order_acquire));

	dispatch_fifo_event(&fixture);
	spa_assert_se(fixture.trigger_calls == 1);
	spa_assert_se(!atomic_load_explicit(
			&fixture.filter.fifo_process_scheduled,
			memory_order_acquire));

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 2);
	spa_assert_se(fixture.observed[1] == 2.0f);
	spa_assert_se(fixture.input_queue.n_returned == 2);
	spa_assert_se(fixture.trigger_calls == 1);
	clear_fixture(&fixture);
}

static void test_fifo_retains_input_while_output_is_unavailable(void)
{
	struct test_fixture fixture;
	struct test_buffer input, output;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS);
	init_buffer(&input, 1.0f);
	init_buffer(&output, 0.0f);
	make_available(&fixture.input_queue, &input);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 0);
	spa_assert_se(fixture.filter.input_buffers[0] == &input.pw);
	spa_assert_se(fixture.input_queue.n_returned == 0);

	make_available(&fixture.output_queue, &output);
	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 1);
	spa_assert_se(fixture.observed[0] == 1.0f);
	spa_assert_se(fixture.input_queue.n_returned == 1);
	clear_fixture(&fixture);
}

static void test_fifo_progressive_output_reuses_buffer(void)
{
	struct test_fixture fixture;
	struct test_buffer input1, input2, output;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS);
	fixture.retain_first_output = true;
	init_buffer(&input1, 1.0f);
	init_buffer(&input2, 2.0f);
	init_buffer(&output, 0.0f);
	make_available(&fixture.input_queue, &input1);
	make_available(&fixture.input_queue, &input2);
	make_available(&fixture.output_queue, &output);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 1);
	spa_assert_se(fixture.output_queue.n_returned == 0);

	dispatch_fifo_event(&fixture);
	spa_assert_se(fixture.trigger_calls == 1);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.callbacks == 2);
	spa_assert_se(fixture.observed[0] == 1.0f);
	spa_assert_se(fixture.observed[1] == 2.0f);
	spa_assert_se(fixture.output_queue.n_returned == 1);
	clear_fixture(&fixture);
}

static void test_removed_fifo_input_fails_instead_of_becoming_stale(void)
{
	struct test_fixture fixture;
	struct test_buffer input;
	struct port_data port_data;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS);
	init_buffer(&input, 1.0f);
	make_available(&fixture.input_queue, &input);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.filter.input_buffers[0] == &input.pw);
	port_data.port = &fixture.input;
	fixture.filter.filter = NULL;
	filter_remove_buffer(&fixture.filter, &port_data, &input.pw);
	spa_assert_se(fixture.filter.input_buffers[0] == NULL);
	spa_assert_se(!fixture.filter.input_available[0]);
	spa_assert_se(!atomic_load_explicit(&fixture.filter.prepared,
			memory_order_acquire));
	spa_assert_se(atomic_load_explicit(&fixture.filter.error,
			memory_order_acquire) == -EPIPE);
	clear_fixture(&fixture);
}

static void test_removed_retained_output_fails_instead_of_becoming_stale(void)
{
	struct test_fixture fixture;
	struct test_buffer input, output;
	struct port_data port_data;

	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_FIFO_INPUTS);
	fixture.retain_first_output = true;
	init_buffer(&input, 1.0f);
	init_buffer(&output, 0.0f);
	make_available(&fixture.input_queue, &input);
	make_available(&fixture.output_queue, &output);

	process(&fixture.filter, NULL);
	spa_assert_se(fixture.filter.output_buffers[0] == &output.pw);
	port_data.port = &fixture.output;
	fixture.filter.filter = NULL;
	filter_remove_buffer(&fixture.filter, &port_data, &output.pw);
	spa_assert_se(fixture.filter.output_buffers[0] == NULL);
	spa_assert_se(!atomic_load_explicit(&fixture.filter.prepared,
			memory_order_acquire));
	spa_assert_se(atomic_load_explicit(&fixture.filter.error,
			memory_order_acquire) == -EPIPE);
	clear_fixture(&fixture);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	pw_init(NULL, NULL);
	test_default_admission_keeps_latest();
	test_fifo_admission_preserves_order_and_progress();
	test_fifo_retains_input_while_output_is_unavailable();
	test_fifo_progressive_output_reuses_buffer();
	test_removed_fifo_input_fails_instead_of_becoming_stale();
	test_removed_retained_output_fails_instead_of_becoming_stale();
	pw_deinit();
	return 0;
}
