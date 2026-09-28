/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Hold the consumer loop after it selects a mix, then remove that mix from
 * the control thread. The mix stays allocated so a failed ordering assertion
 * does not turn this test into an uncontrolled use-after-free. */
#include <pthread.h>
#include <time.h>
#include <pipewire/data-loop.h>
#include <pipewire/log.h>
#include <pipewire/private.h>

PW_LOG_TOPIC(log_port, "pw.port");

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static struct spa_list *watched_control_list;
static struct spa_list *watched_rt_list;
static _Thread_local bool in_publisher;
static _Thread_local bool in_replacement;
static bool selected, resume_reader, removed, quiescence_attempt;

static void selected_mix(struct spa_list *head, struct spa_list *item)
{
	if (!in_publisher || item == head ||
	    (head != watched_control_list && head != watched_rt_list))
		return;
	pthread_mutex_lock(&gate_lock);
	selected = true;
	pthread_cond_broadcast(&gate_cond);
	while (!resume_reader)
		pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}

static int observed_loop_locked(struct pw_loop *loop, spa_invoke_func_t func,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	if (in_replacement) {
		pthread_mutex_lock(&gate_lock);
		quiescence_attempt = true;
		pthread_cond_broadcast(&gate_cond);
		pthread_mutex_unlock(&gate_lock);
	}
	return spa_loop_locked(loop->loop, func, seq, data, size, user_data);
}

/* The implementation is compiled unchanged. This test-only iterator hook
 * stops at the selection boundary of whichever mix list it traverses. */
#undef spa_list_is_end
#define spa_list_is_end(pos, head, member) \
	(selected_mix((head), &(pos)->member), &(pos)->member == (head))
#undef PW_API_PORT_IMPL
#define pw_loop_locked observed_loop_locked
#include "../pipewire/impl-port.c"

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

struct fixture {
	struct impl input;
	struct pw_impl_node node;
	struct pw_impl_port_mix mix;
	struct spa_io_position position;
	struct spa_io_buffers shared;
	struct spa_io_buffers replacement;
	int publish_result;
};

static int publish_on_loop(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct fixture *f = user_data;
	in_publisher = true;
	f->publish_result = pw_impl_port_publish_row_return(&f->input.this);
	in_publisher = false;
	return 0;
}

static void *remove_on_control_thread(void *data)
{
	struct fixture *f = data;
	spa_assert_se(pw_impl_port_release_mix(&f->input.this, &f->mix) == 0);
	pthread_mutex_lock(&gate_lock);
	removed = true;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
	return NULL;
}

static void *replace_on_control_thread(void *data)
{
	struct fixture *f = data;
	in_replacement = true;
	spa_assert_se(port_set_io(&f->input, PW_DIRECTION_INPUT,
			f->mix.port.port_id, SPA_IO_Buffers,
			&f->replacement, sizeof(f->replacement)) == 0);
	in_replacement = false;
	return NULL;
}

static struct timespec deadline_after_one_second(void)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec++;
	return deadline;
}

static int noop_on_loop(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data SPA_UNUSED)
{
	return 0;
}

int main(int argc, char *argv[])
{
	struct fixture f = { 0 };
	struct pw_data_loop *driver_loop, *consumer_loop;
	struct timespec deadline;
	pthread_t control_thread;
	bool removed_while_selected;
	bool replace = argc > 1 && strcmp(argv[1], "replace") == 0;
	bool old_io_retained = false;

	pw_init(&argc, &argv);
	driver_loop = pw_data_loop_new(NULL);
	consumer_loop = pw_data_loop_new(NULL);
	spa_assert_se(driver_loop != NULL && consumer_loop != NULL);
	spa_assert_se(pw_data_loop_start(driver_loop) == 0);
	spa_assert_se(pw_data_loop_start(consumer_loop) == 0);
	spa_assert_se(pw_data_loop_invoke(driver_loop, noop_on_loop,
			SPA_ID_INVALID, NULL, 0, true, NULL) == 0);

	f.input.this.node = &f.node;
	f.input.this.direction = PW_DIRECTION_INPUT;
	f.input.this.destroying = true;
	f.node.rt.position = &f.position;
	f.node.data_loop = pw_data_loop_get_loop(consumer_loop);
	f.input.this.rt.io.status = SPA_STATUS_NEED_DATA;
	f.input.this.rt.io.buffer_id = 7;
	f.shared.status = SPA_STATUS_NEED_DATA;
	f.shared.buffer_id = SPA_ID_INVALID;
	f.replacement.status = SPA_STATUS_NEED_DATA;
	f.replacement.buffer_id = SPA_ID_INVALID;
	f.mix.p = &f.input.this;
	f.mix.row_transport = true;
	f.mix.io[0] = f.mix.io[1] = &f.shared;
	f.mix.rt.active = true;
	spa_list_init(&f.input.this.mix_list);
	spa_list_init(&f.input.rt.mix_list);
	spa_list_append(&f.input.this.mix_list, &f.mix.link);
	spa_list_append(&f.input.rt.mix_list, &f.mix.rt.link);
	f.input.this.n_mix = 1;
	pw_map_init(&f.input.this.mix_port_map, 1, 1);
	f.mix.port.port_id = pw_map_insert_new(&f.input.this.mix_port_map, &f.mix);
	spa_assert_se(f.mix.port.port_id != SPA_ID_INVALID);
	/* Keep both possible reader lists observable as the implementation evolves. */
	spa_assert_se(!spa_list_is_empty(&f.input.this.mix_list));
	spa_assert_se(!spa_list_is_empty(&f.input.rt.mix_list));
	watched_control_list = &f.input.this.mix_list;
	watched_rt_list = &f.input.rt.mix_list;
	spa_assert_se(pw_data_loop_invoke(consumer_loop, publish_on_loop,
			SPA_ID_INVALID, NULL, 0, false, &f) == 0);
	pthread_mutex_lock(&gate_lock);
	deadline = deadline_after_one_second();
	while (!selected)
		spa_assert_se(pthread_cond_timedwait(&gate_cond, &gate_lock,
				&deadline) == 0);
	pthread_mutex_unlock(&gate_lock);
	spa_assert_se(pthread_create(&control_thread, NULL,
			replace ? replace_on_control_thread : remove_on_control_thread,
			&f) == 0);
	pthread_mutex_lock(&gate_lock);
	deadline = deadline_after_one_second();
	while (!(replace ? quiescence_attempt : removed) &&
	       pthread_cond_timedwait(&gate_cond, &gate_lock,
			&deadline) == 0)
		;
	removed_while_selected = removed;
	if (replace) {
		spa_assert_se(quiescence_attempt);
		old_io_retained = f.mix.io[0] == &f.shared &&
			f.mix.io[1] == &f.shared;
	}
	resume_reader = true;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
	spa_assert_se(pthread_join(control_thread, NULL) == 0);
	spa_assert_se(pw_data_loop_invoke(consumer_loop, noop_on_loop,
			SPA_ID_INVALID, NULL, 0, true, NULL) == 0);
	spa_assert_se(f.publish_result == 1);
	if (replace)
		spa_assert_se(old_io_retained);
	spa_assert_se(f.shared.buffer_id == 7);
	if (replace) {
		spa_assert_se(f.mix.io[0] == &f.replacement);
	} else {
		spa_assert_se(!removed_while_selected);
	}
	pw_map_clear(&f.input.this.mix_port_map);
	spa_assert_se(pw_data_loop_stop(consumer_loop) == 0);
	spa_assert_se(pw_data_loop_stop(driver_loop) == 0);
	pw_data_loop_destroy(consumer_loop);
	pw_data_loop_destroy(driver_loop);
	pw_deinit();
	return 0;
}
