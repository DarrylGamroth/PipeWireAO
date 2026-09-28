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
	pthread_barrier_init(&release_entered, NULL, 2);
	pthread_barrier_init(&release_resume, NULL, 2);
	driver->node = &source;
	driver->driver_node = driver;
	driver->driving = true;
	driver->reliable = true;
	driver->row_transport = true;
	driver->info.state = PW_NODE_STATE_RUNNING;
	driver->rt.target.activation = &activation;
	driver->rt.position = &position;
	driver->rt.prepared = true;
	driver->rt.reliable_event = &request_event;
	driver->data_loop = &loop;
	SPA_ATOMIC_STORE(activation.status, PW_NODE_ACTIVATION_FINISHED);
	spa_list_init(&driver->rt.input_mix);
	spa_list_init(&driver->rt.target_list);
	spa_list_append(&driver->rt.input_mix, &input.rt.node_link);
	SPA_ATOMIC_STORE(driver->rt.reliable_release_pending, 1);
	spa_assert_se(node_ready(driver, SPA_STATUS_HAVE_DATA) == -EBUSY);
	spa_assert_se(SPA_ATOMIC_LOAD(activation.status) ==
			PW_NODE_ACTIVATION_FINISHED);
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
