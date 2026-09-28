/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Run the release path on one thread and an output-return retry on another.
 * The retry must not reach the source until the exact input ID is released. */
#include <pthread.h>
#include <pipewire/log.h>

PW_LOG_TOPIC(log_node, "pw.node");

#include "../pipewire/impl-node.c"

static pthread_barrier_t release_entered;
static pthread_barrier_t release_resume;
static uint32_t releases;
static uint32_t early_retries;
static uint32_t retry_commands;
static uint32_t wakeups;
static bool borrowed_row;

static int signal_event(void *object SPA_UNUSED,
		struct spa_source *source SPA_UNUSED)
{
	__atomic_add_fetch(&wakeups, 1, __ATOMIC_ACQ_REL);
	return 0;
}

static const struct spa_loop_utils_methods loop_methods = {
	SPA_VERSION_LOOP_UTILS_METHODS,
	.signal_event = signal_event,
};

static int test_clock_gettime(void *object SPA_UNUSED,
		int clockid SPA_UNUSED, struct timespec *value)
{
	*value = (struct timespec) { .tv_sec = 1 };
	return 0;
}

static const struct spa_system_methods system_methods = {
	SPA_VERSION_SYSTEM_METHODS,
	.clock_gettime = test_clock_gettime,
};

static int test_trigger(struct pw_node_target *target SPA_UNUSED,
		uint64_t nsec SPA_UNUSED)
{
	return 0;
}

static void test_first_cycle(void)
{
	struct pw_impl_node driver = { 0 };
	struct pw_node_activation activation = { 0 };
	struct spa_io_position position = { 0 };
	struct spa_system system = {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_System,
				SPA_VERSION_SYSTEM, &system_methods, NULL),
	};

	driver.driver_node = &driver;
	driver.row_transport = true;
	driver.rt.prepared = true;
	driver.rt.target.activation = &activation;
	driver.rt.target.system = &system;
	driver.rt.position = &position;
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_hook_list_init(&driver.rt_listener_list);
	spa_list_init(&driver.rt.input_mix);
	spa_list_init(&driver.rt.output_mix);
	spa_list_init(&driver.rt.target_list);
	spa_assert_se(node_ready(&driver, SPA_STATUS_HAVE_DATA) == 0);
	spa_assert_se(driver.row_cycle_inflight);
	spa_assert_se(activation.position.clock.cycle == 1);
}

static void test_borrowed_cycle(void)
{
	struct pw_impl_node driver = { 0 }, filter = { 0 };
	struct pw_impl_port input = { 0 };
	struct pw_node_activation activation = { 0 },
			filter_activation = { 0 };
	struct pw_node_target filter_target = {
		.id = 2,
		.node = &filter,
		.activation = &filter_activation,
		.active = true,
		.trigger = test_trigger,
	};
	struct spa_io_position position = { 0 };
	struct spa_system system = {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_System,
				SPA_VERSION_SYSTEM, &system_methods, NULL),
	};

	driver.driver_node = &driver;
	driver.info.id = 1;
	driver.row_transport = true;
	driver.row_cycle_inflight = true;
	driver.rt.prepared = true;
	driver.rt.target.activation = &activation;
	driver.rt.target.system = &system;
	driver.rt.position = &position;
	spa_hook_list_init(&driver.rt_listener_list);
	spa_list_init(&driver.rt.input_mix);
	spa_list_init(&driver.rt.output_mix);
	spa_list_init(&driver.rt.target_list);
	spa_list_append(&driver.rt.target_list, &filter_target.link);
	spa_list_init(&filter.rt.input_mix);
	spa_list_init(&filter.input_ports);
	spa_list_append(&filter.input_ports, &input.link);
	spa_list_init(&input.mix_list);
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	SPA_ATOMIC_STORE(filter_activation.status, PW_NODE_ACTIVATION_FINISHED);
	filter_activation.active_driver_id = 1;
	borrowed_row = true;
	spa_assert_se(node_ready(&driver, SPA_STATUS_HAVE_DATA) == -EBUSY);
	spa_assert_se(activation.position.clock.cycle == 0);
	spa_assert_se(SPA_ATOMIC_LOAD(filter_activation.status) ==
			PW_NODE_ACTIVATION_FINISHED);
	borrowed_row = false;
	spa_assert_se(node_ready(&driver, SPA_STATUS_HAVE_DATA) == 0);
	spa_assert_se(activation.position.clock.cycle == 1);
}

void pw_log_log_object(enum spa_log_level level SPA_UNUSED,
		const struct spa_log_topic *topic SPA_UNUSED,
		const char *file SPA_UNUSED, int line SPA_UNUSED,
		const char *func SPA_UNUSED, uint32_t flags SPA_UNUSED,
		const void *object SPA_UNUSED)
{
	spa_assert_not_reached();
}

int pw_impl_port_reuse_reliable_input(struct pw_impl_port *port SPA_UNUSED)
{
	pthread_barrier_wait(&release_entered);
	pthread_barrier_wait(&release_resume);
	__atomic_store_n(&releases, 1, __ATOMIC_RELEASE);
	return 1;
}

bool pw_impl_port_has_reliable_peer(struct pw_impl_port *port SPA_UNUSED)
{
	return false;
}

int pw_impl_port_publish_row_return(struct pw_impl_port *port SPA_UNUSED)
{
	return 0;
}

int pw_impl_port_reuse_remote_row_input(struct pw_impl_port *port SPA_UNUSED)
{
	return 0;
}

bool pw_impl_port_has_borrowed_row(struct pw_impl_port *port SPA_UNUSED)
{
	return borrowed_row;
}

static int source_send_command(void *object SPA_UNUSED,
		const struct spa_command *command)
{
	spa_assert_se(SPA_NODE_COMMAND_ID(command) ==
			SPA_NODE_COMMAND_RequestProcess);
	if (__atomic_load_n(&releases, __ATOMIC_ACQUIRE) == 0)
		__atomic_store_n(&early_retries, 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&retry_commands, 1, __ATOMIC_ACQ_REL);
	return 0;
}

static const struct spa_node_methods source_methods = {
	SPA_VERSION_NODE_METHODS,
	.send_command = source_send_command,
};

static void *run_release(void *data)
{
	flush_reliable_input_returns(data);
	flush_reliable_retry(data);
	return NULL;
}

int main(int argc, char *argv[])
{
	struct impl impl = { 0 };
	struct pw_impl_node *driver = &impl.this;
	struct pw_impl_port input = { 0 };
	struct spa_node source = {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
				SPA_VERSION_NODE, &source_methods, NULL),
	};
	struct pw_node_activation activation = { 0 };
	struct pw_node_activation follower_activation = { 0 };
	struct pw_node_target follower = {
		.activation = &follower_activation,
		.active = true,
	};
	struct spa_io_position position = { 0 };
	struct spa_source request_event = { 0 };
	struct spa_loop_utils utils = {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_LoopUtils,
				SPA_VERSION_LOOP_UTILS, &loop_methods, NULL),
	};
	struct pw_loop loop = { .utils = &utils };
	struct spa_command retry =
		SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_RequestProcess);
	pthread_t thread;

	pw_init(&argc, &argv);
	test_first_cycle();
	test_borrowed_cycle();
	pthread_barrier_init(&release_entered, NULL, 2);
	pthread_barrier_init(&release_resume, NULL, 2);
	driver->node = &source;
	driver->driver_node = driver;
	driver->driving = true;
	driver->reliable = true;
	driver->row_transport = true;
	driver->row_cycle_inflight = true;
	driver->info.state = PW_NODE_STATE_RUNNING;
	driver->rt.target.activation = &activation;
	driver->rt.position = &position;
	driver->rt.prepared = true;
	driver->rt.reliable_event = &request_event;
	driver->data_loop = &loop;
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_list_init(&driver->rt.input_mix);
	spa_list_init(&driver->rt.target_list);
	spa_list_append(&driver->rt.target_list, &follower.link);
	spa_list_append(&driver->rt.input_mix, &input.rt.node_link);
	SPA_ATOMIC_STORE(follower_activation.status, PW_NODE_ACTIVATION_AWAKE);
	SPA_ATOMIC_STORE(driver->rt.reliable_release_pending, 1);
	spa_assert_se(node_ready(driver, SPA_STATUS_HAVE_DATA) == -EBUSY);
	spa_assert_se(SPA_ATOMIC_LOAD(activation.status) ==
			PW_NODE_ACTIVATION_FINISHED);
	SPA_ATOMIC_STORE(follower_activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_assert_se(pthread_create(&thread, NULL, run_release, driver) == 0);
	pthread_barrier_wait(&release_entered);
	handle_request_process_command(driver, &retry);
	pthread_barrier_wait(&release_resume);
	spa_assert_se(pthread_join(thread, NULL) == 0);
	spa_assert_se(releases == 1);
	spa_assert_se(early_retries == 0);
	spa_assert_se(retry_commands == 1);
	/* A buffer returned after completion must wake the source without a
	 * second camera packet or another graph-completion callback. */
	handle_request_process_command(driver, &retry);
	spa_assert_se(wakeups == 2);
	spa_assert_se(retry_commands == 1);
	reliable_request_event(driver, 1);
	spa_assert_se(retry_commands == 2);
	pthread_barrier_destroy(&release_entered);
	pthread_barrier_destroy(&release_resume);
	pw_deinit();
	return 0;
}
