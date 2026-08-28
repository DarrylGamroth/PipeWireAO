/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/pod.h>

static const uint32_t shape[] = { 1 };

static const struct spa_fgn_format format = {
	.element_type = SPA_ELEMENT_TYPE_F32_LE,
	.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
	.n_dimensions = 1,
	.shape = shape,
};

static const struct spa_fgn_port_info ports[] = {
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 0,
		.direction = SPA_DIRECTION_INPUT,
		.name = "in",
	},
};

static const struct spa_fgn_property_info properties[] = {
	SPA_FGN_PROPERTY_INFO_INIT(
		0, SPA_FGN_PROPERTY_FLAG_READONLY,
		"value", "Test value", "1",
		SPA_FGN_VALUE_LONG_INIT(0),
		SPA_FGN_VALUE_NONE_INIT, SPA_FGN_VALUE_NONE_INIT, 0, NULL),
};

static int instantiate(const struct spa_fgn_descriptor *descriptor,
		const char *config, const struct spa_fgn_executor *executor,
		void **result)
{
	if (descriptor == NULL || config == NULL || executor == NULL ||
	    executor->version != SPA_FGN_EXECUTOR_VERSION || result == NULL)
		return -EINVAL;
	*result = (void *)descriptor;
	return 0;
}

static void cleanup(void *instance SPA_UNUSED)
{
}

static int get_port_format(void *instance, uint32_t port,
		const struct spa_fgn_format **result)
{
	if (instance == NULL || port != 0 || result == NULL)
		return -EINVAL;
	*result = &format;
	return 0;
}

static int enum_prop_info(void *instance, uint32_t index,
		struct spa_fgn_property_info *info)
{
	if (instance == NULL)
		return -EINVAL;
	return spa_fgn_enum_prop_info_table(properties,
			SPA_N_ELEMENTS(properties), index, info);
}

static int get_prop(void *instance, uint32_t id, struct spa_fgn_value *value)
{
	if (instance == NULL || id != 0 || value == NULL)
		return -EINVAL;
	*value = (struct spa_fgn_value)SPA_FGN_VALUE_LONG_INIT(0);
	return 0;
}

static uint64_t get_prop_revision(void *instance SPA_UNUSED)
{
	return 1;
}

static int process(void *instance, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	if (instance == NULL || inputs == NULL || n_inputs != 1 ||
	    outputs != NULL || n_outputs != 0)
		return -EINVAL;
	return 0;
}

static const struct spa_fgn_descriptor descriptor = {
	.struct_size = sizeof(struct spa_fgn_descriptor),
	.version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "unstable-properties",
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.instantiate = instantiate,
	.cleanup = cleanup,
	.get_port_format = get_port_format,
	.enum_prop_info = enum_prop_info,
	.get_prop = get_prop,
	.get_prop_revision = get_prop_revision,
	.process = process,
};

static const struct spa_fgn_descriptor *find_descriptor(const char *name)
{
	return name != NULL && strcmp(name, descriptor.name) == 0 ? &descriptor : NULL;
}

static const struct spa_fgn_plugin plugin = {
	.struct_size = sizeof(struct spa_fgn_plugin),
	.abi_version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "test-unstable-properties",
	.find_descriptor = find_descriptor,
};

SPA_EXPORT
const struct spa_fgn_plugin *spa_filter_graph_ndarray_plugin_get_interface(
		uint32_t abi_version)
{
	return abi_version == SPA_FGN_PLUGIN_ABI_VERSION ? &plugin : NULL;
}
