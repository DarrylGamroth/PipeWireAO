/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Pause the real driver scan at an unrelated target input port. Remove that
 * port through the real control path while another input keeps the target
 * active. Storage remains allocated until both threads finish. */
#include <pthread.h>
#include <time.h>
#include <pipewire/data-loop.h>
#include <pipewire/log.h>
#include <pipewire/private.h>

PW_LOG_TOPIC(log_node, "pw.node");

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static struct spa_list *watched_ports;
static struct spa_list *watched_port_link;
static _Thread_local bool in_driver_scan;
static bool selected, resume_reader, removed, scan_done;

static void selected_target_port(struct spa_list *head, struct spa_list *item)
{
	if (!in_driver_scan || head != watched_ports || item != watched_port_link)
		return;
	pthread_mutex_lock(&gate_lock);
	selected = true;
	pthread_cond_broadcast(&gate_cond);
	while (!resume_reader)
		pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}

#undef spa_list_is_end
#define spa_list_is_end(pos, head, member) \
	(selected_target_port((head), &(pos)->member), &(pos)->member == (head))
#undef PW_API_NODE_IMPL
#include "../pipewire/impl-node.c"

struct pw_impl_port *test_alloc_target_port(struct pw_impl_node *node,
		uint32_t port_id);
struct pw_impl_port *test_alloc_driver_output(struct pw_impl_node *node);
void test_attach_output_mix(struct pw_impl_port *port,
		struct pw_impl_port_mix *mix);
void test_remove_target_port(struct pw_impl_port *port);
void test_free_target_port(struct pw_impl_port *port);

struct fixture {
	struct pw_impl_node driver, target;
	struct pw_node_target target_entry;
	struct pw_impl_port *row_port, *unrelated_port, *source_output;
	struct pw_impl_port_mix output_mix, input_mix;
	struct pw_data_loop *driver_loop, *target_loop;
	bool borrowed;
};

static int scan_on_driver(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct fixture *f = user_data;
	in_driver_scan = true;
	f->borrowed = driver_has_borrowed_row(&f->driver);
	in_driver_scan = false;
	pthread_mutex_lock(&gate_lock);
	scan_done = true;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
	return 0;
}

static void *remove_on_control_thread(void *data)
{
	struct fixture *f = data;
	test_remove_target_port(f->unrelated_port);
	pthread_mutex_lock(&gate_lock);
	removed = true;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
	return NULL;
}

static struct timespec deadline_after_one_second(void)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec++;
	return deadline;
}

int main(int argc, char *argv[])
{
	struct fixture f = { 0 };
	struct timespec deadline;
	pthread_t control_thread;
	bool removed_while_selected;

	pw_init(&argc, &argv);
	f.driver_loop = pw_data_loop_new(NULL);
	f.target_loop = pw_data_loop_new(NULL);
	spa_assert_se(f.driver_loop != NULL && f.target_loop != NULL);
	spa_assert_se(pw_data_loop_start(f.driver_loop) == 0);
	spa_assert_se(pw_data_loop_start(f.target_loop) == 0);
	f.driver.data_loop = pw_data_loop_get_loop(f.driver_loop);
	f.target.data_loop = pw_data_loop_get_loop(f.target_loop);
	spa_hook_list_init(&f.target.listener_list);
	spa_list_init(&f.driver.rt.target_list);
	spa_list_init(&f.driver.rt.output_mix);
	spa_list_init(&f.target.input_ports);
	pw_map_init(&f.target.input_port_map, 2, 2);
	f.target.info.n_input_ports = 2;
	f.row_port = test_alloc_target_port(&f.target, 0);
	f.unrelated_port = test_alloc_target_port(&f.target, 1);
	f.source_output = test_alloc_driver_output(&f.driver);
	spa_assert_se(f.row_port != NULL && f.unrelated_port != NULL &&
			f.source_output != NULL);
	spa_assert_se(pw_map_insert_at(&f.target.input_port_map, 0,
			f.row_port) == 0);
	spa_assert_se(pw_map_insert_at(&f.target.input_port_map, 1,
			f.unrelated_port) == 0);
	spa_list_append(&f.target.input_ports, &f.row_port->link);
	spa_list_append(&f.target.input_ports, &f.unrelated_port->link);
	f.output_mix.p = f.source_output;
	f.output_mix.peer = &f.input_mix;
	f.input_mix.p = f.row_port;
	f.input_mix.peer = &f.output_mix;
	f.input_mix.row_transport = true;
	spa_list_append(&f.driver.rt.output_mix, &f.source_output->rt.node_link);
	test_attach_output_mix(f.source_output, &f.output_mix);
	f.target_entry.node = &f.target;
	f.target_entry.active = true;
	spa_list_append(&f.driver.rt.target_list, &f.target_entry.link);
	watched_ports = &f.target.input_ports;
	watched_port_link = &f.unrelated_port->link;
	spa_assert_se(pw_data_loop_invoke(f.driver_loop, scan_on_driver,
			SPA_ID_INVALID, NULL, 0, false, &f) == 0);
	pthread_mutex_lock(&gate_lock);
	deadline = deadline_after_one_second();
	while (!selected && !scan_done)
		spa_assert_se(pthread_cond_timedwait(&gate_cond, &gate_lock,
				&deadline) == 0);
	pthread_mutex_unlock(&gate_lock);
	spa_assert_se(pthread_create(&control_thread, NULL,
			remove_on_control_thread, &f) == 0);
	pthread_mutex_lock(&gate_lock);
	deadline = deadline_after_one_second();
	while (!removed && pthread_cond_timedwait(&gate_cond, &gate_lock,
			&deadline) == 0)
		;
	removed_while_selected = selected && removed && !scan_done;
	resume_reader = true;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
	spa_assert_se(pthread_join(control_thread, NULL) == 0);
	spa_assert_se(pw_data_loop_invoke(f.driver_loop, scan_on_driver,
			SPA_ID_INVALID, NULL, 0, true, &f) == 0);
	spa_assert_se(scan_done && !f.borrowed);
	spa_assert_se(!removed_while_selected);
	spa_assert_se(f.target.info.n_input_ports == 1);
	spa_assert_se(f.row_port->node == &f.target);
	test_free_target_port(f.unrelated_port);
	test_free_target_port(f.row_port);
	test_free_target_port(f.source_output);
	pw_map_clear(&f.target.input_port_map);
	spa_assert_se(pw_data_loop_stop(f.target_loop) == 0);
	spa_assert_se(pw_data_loop_stop(f.driver_loop) == 0);
	pw_data_loop_destroy(f.target_loop);
	pw_data_loop_destroy(f.driver_loop);
	pw_deinit();
	return 0;
}
