/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <pipewire/log.h>

PW_LOG_TOPIC(log_port, "pw.port");

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

/* Expose the real control-path removal without freeing the port while the
 * deliberately paused driver may still hold its address. */
struct pw_impl_port *test_alloc_target_port(struct pw_impl_node *node,
		uint32_t port_id)
{
	struct impl *impl = calloc(1, sizeof(*impl));
	struct pw_impl_port *port;

	if (impl == NULL)
		return NULL;
	port = &impl->this;
	port->node = node;
	port->direction = PW_DIRECTION_INPUT;
	port->port_id = port_id;
	port->destroying = true;
	spa_list_init(&port->mix_list);
	spa_hook_list_init(&port->listener_list);
	return port;
}

struct pw_impl_port *test_alloc_driver_output(struct pw_impl_node *node)
{
	struct impl *impl = calloc(1, sizeof(*impl));
	struct pw_impl_port *port;

	if (impl == NULL)
		return NULL;
	port = &impl->this;
	port->node = node;
	port->direction = PW_DIRECTION_OUTPUT;
	spa_list_init(&impl->rt.mix_list);
	return port;
}

void test_attach_output_mix(struct pw_impl_port *port,
		struct pw_impl_port_mix *mix)
{
	struct impl *impl = SPA_CONTAINER_OF(port, struct impl, this);
	spa_list_append(&impl->rt.mix_list, &mix->rt.link);
	mix->rt.active = true;
}

void test_remove_target_port(struct pw_impl_port *port)
{
	pw_impl_port_remove(port);
}

void test_free_target_port(struct pw_impl_port *port)
{
	free(SPA_CONTAINER_OF(port, struct impl, this));
}
