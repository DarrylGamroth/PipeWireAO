/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_DEBUG_FILTER_GRAPH_NDARRAY_H
#define SPA_DEBUG_FILTER_GRAPH_NDARRAY_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <spa/debug/context.h>
#include <spa/filter-graph/filter-graph-ndarray.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_debug
 * \{
 */

#ifndef SPA_API_DEBUG_FILTER_GRAPH_NDARRAY
 #ifdef SPA_API_IMPL
  #define SPA_API_DEBUG_FILTER_GRAPH_NDARRAY SPA_API_IMPL
 #else
  #define SPA_API_DEBUG_FILTER_GRAPH_NDARRAY static inline
 #endif
#endif

SPA_API_DEBUG_FILTER_GRAPH_NDARRAY const char *
spa_debug_fgn_element_type_name(uint32_t type)
{
	switch (type) {
	case SPA_ELEMENT_TYPE_BOOL8: return "bool8";
	case SPA_ELEMENT_TYPE_I8: return "i8";
	case SPA_ELEMENT_TYPE_U8: return "u8";
	case SPA_ELEMENT_TYPE_I16_LE: return "i16-le";
	case SPA_ELEMENT_TYPE_U16_LE: return "u16-le";
	case SPA_ELEMENT_TYPE_I32_LE: return "i32-le";
	case SPA_ELEMENT_TYPE_U32_LE: return "u32-le";
	case SPA_ELEMENT_TYPE_I64_LE: return "i64-le";
	case SPA_ELEMENT_TYPE_U64_LE: return "u64-le";
	case SPA_ELEMENT_TYPE_I128_LE: return "i128-le";
	case SPA_ELEMENT_TYPE_U128_LE: return "u128-le";
	case SPA_ELEMENT_TYPE_F8_E4M3FN: return "f8-e4m3fn";
	case SPA_ELEMENT_TYPE_F8_E4M3FNUZ: return "f8-e4m3fnuz";
	case SPA_ELEMENT_TYPE_F8_E5M2: return "f8-e5m2";
	case SPA_ELEMENT_TYPE_F8_E5M2FNUZ: return "f8-e5m2fnuz";
	case SPA_ELEMENT_TYPE_F16_LE: return "f16-le";
	case SPA_ELEMENT_TYPE_BF16_LE: return "bf16-le";
	case SPA_ELEMENT_TYPE_F32_LE: return "f32-le";
	case SPA_ELEMENT_TYPE_F64_LE: return "f64-le";
	case SPA_ELEMENT_TYPE_F128_LE: return "f128-le";
	case SPA_ELEMENT_TYPE_COMPLEX_F16_LE: return "complex-f16-le";
	case SPA_ELEMENT_TYPE_COMPLEX_BF16_LE: return "complex-bf16-le";
	case SPA_ELEMENT_TYPE_COMPLEX_F32_LE: return "complex-f32-le";
	case SPA_ELEMENT_TYPE_COMPLEX_F64_LE: return "complex-f64-le";
	case SPA_ELEMENT_TYPE_COMPLEX_F128_LE: return "complex-f128-le";
	default: return "unknown/custom";
	}
}

SPA_API_DEBUG_FILTER_GRAPH_NDARRAY const char *
spa_debug_fgn_layout_name(uint32_t layout)
{
	switch (layout) {
	case SPA_NDARRAY_LAYOUT_ROW_MAJOR: return "row-major";
	case SPA_NDARRAY_LAYOUT_COLUMN_MAJOR: return "column-major";
	default: return "unknown";
	}
}

SPA_API_DEBUG_FILTER_GRAPH_NDARRAY void
spa_debug_fgn_shape(const struct spa_fgn_format *format,
		char *buffer, size_t size)
{
	size_t offset = 0;
	uint32_t i;

	if (buffer == NULL || size == 0)
		return;
	buffer[0] = '\0';
	if (format == NULL || format->shape == NULL)
		return;
	for (i = 0; i < format->n_dimensions; i++) {
		int written = snprintf(buffer + offset, size - offset,
				"%s%" PRIu32, i == 0 ? "[" : ",",
				format->shape[i]);

		if (written < 0 || (size_t)written >= size - offset) {
			buffer[size - 1] = '\0';
			return;
		}
		offset += (size_t)written;
	}
	if (offset < size - 1) {
		buffer[offset++] = ']';
		buffer[offset] = '\0';
	}
}

SPA_API_DEBUG_FILTER_GRAPH_NDARRAY int
spa_debugc_fgn_graph(struct spa_debug_context *ctx, int indent,
		const struct spa_fgn_graph *graph)
{
	struct spa_fgn_graph_info graph_info;
	uint32_t i;
	int res;

	memset(&graph_info, 0, sizeof(graph_info));
	graph_info.struct_size = sizeof(graph_info);
	if ((res = spa_fgn_graph_get_info(graph, &graph_info)) < 0)
		return res;
	spa_debugc(ctx, "%*sndarray graph: nodes=%" PRIu32 " links=%" PRIu32
			" inputs=%" PRIu32 " outputs=%" PRIu32 " lanes=%" PRIu32,
			indent, "", graph_info.n_nodes, graph_info.n_links,
			graph_info.n_inputs, graph_info.n_outputs, graph_info.n_lanes);
	spa_debugc(ctx, "%*sexecutor flags=0x%08" PRIx32, indent + 2, "",
			graph_info.executor_flags);
	spa_debugc(ctx, "%*soutput buffers: allocated=%" PRIu64
			" internal=%" PRIu64 " bytes", indent + 2, "",
			graph_info.allocated_output_buffer_bytes,
			graph_info.internal_output_buffer_bytes);

	for (i = 0; i < graph_info.n_nodes; i++) {
		struct spa_fgn_node_info node;
		uint32_t direction_index, j;

		memset(&node, 0, sizeof(node));
		node.struct_size = sizeof(node);
		if ((res = spa_fgn_graph_get_node_info(graph, i, &node)) < 0)
			return res;
		spa_debugc(ctx, "%*s[%" PRIu32 "] %s: %s/%s", indent + 2, "",
				i, node.name, node.plugin_name, node.descriptor_name);
		spa_debugc(ctx, "%*splugin=%s flags=0x%08" PRIx32,
				indent + 4, "", node.plugin_path, node.plugin_flags);
		spa_debugc(ctx, "%*sports: inputs=%" PRIu32 " outputs=%" PRIu32
				" properties=%" PRIu32, indent + 4, "", node.n_inputs,
				node.n_outputs, node.n_properties);
		spa_debugc(ctx, "%*scapabilities:%s%s%s%s%s%s", indent + 4, "",
				node.capabilities == 0 ? " none" : "",
				node.capabilities & SPA_FGN_NODE_CAPABILITY_RESETTABLE
					? " resettable" : "",
				node.capabilities & SPA_FGN_NODE_CAPABILITY_RUNTIME_PROPERTIES
					? " runtime-properties" : "",
				node.capabilities & SPA_FGN_NODE_CAPABILITY_PARAMETER_INPUTS
					? " parameter-inputs" : "",
				node.capabilities & SPA_FGN_NODE_CAPABILITY_CONDITIONAL_OUTPUTS
					? " conditional-outputs" : "",
				node.capabilities &
					SPA_FGN_NODE_CAPABILITY_PROCESS_THREAD_PREPARATION
					? " process-thread-preparation" : "");
		spa_debugc(ctx, "%*soutput buffers: allocated=%" PRIu64
				" internal=%" PRIu64 " bytes", indent + 4, "",
				node.allocated_output_buffer_bytes,
				node.internal_output_buffer_bytes);

		for (direction_index = 0; direction_index < 2; direction_index++) {
			enum spa_direction direction = direction_index == 0
				? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
			uint32_t n_ports = direction == SPA_DIRECTION_INPUT
				? node.n_inputs : node.n_outputs;

			for (j = 0; j < n_ports; j++) {
				struct spa_fgn_graph_port_info port;
				char shape[SPA_NDARRAY_MAX_DIMENSIONS * 12u + 3u];
				const char *direction_name = direction == SPA_DIRECTION_INPUT
					? "input" : "output";

				memset(&port, 0, sizeof(port));
				port.struct_size = sizeof(port);
				if ((res = spa_fgn_graph_get_node_port_info(graph, i,
						direction, j, &port)) < 0)
					return res;
				spa_debug_fgn_shape(port.format, shape, sizeof(shape));
				spa_debugc(ctx, "%*s%s[%" PRIu32 "] %s id=%" PRIu32
						" flags=0x%08" PRIx32 ": %s%s %s, %" PRIu32
						" bytes", indent + 6, "", direction_name, j,
						port.info->name, port.info->index, port.info->flags,
						spa_debug_fgn_element_type_name(
							port.format->element_type), shape,
						spa_debug_fgn_layout_name(port.format->layout),
						port.payload_bytes);
				if (port.format->rate_denom != 0)
					spa_debugc(ctx, "%*srate=%" PRIu32 "/%" PRIu32,
							indent + 8, "", port.format->rate_num,
							port.format->rate_denom);
				if (port.format->schema != NULL)
					spa_debugc(ctx, "%*sschema=%s", indent + 8, "",
							port.format->schema);
				if (port.format->profile != NULL)
					spa_debugc(ctx, "%*sprofile=%s", indent + 8, "",
							port.format->profile);
				if (port.external_index != SPA_ID_INVALID)
					spa_debugc(ctx, "%*sexternal %s[%" PRIu32 "]",
							indent + 8, "", direction_name,
							port.external_index);
				if (port.source_execution_index != SPA_ID_INVALID) {
					struct spa_fgn_node_info source_node;
					struct spa_fgn_graph_port_info source_port;

					memset(&source_node, 0, sizeof(source_node));
					memset(&source_port, 0, sizeof(source_port));
					source_node.struct_size = sizeof(source_node);
					source_port.struct_size = sizeof(source_port);
					if ((res = spa_fgn_graph_get_node_info(graph,
							port.source_execution_index,
							&source_node)) < 0 ||
					    (res = spa_fgn_graph_get_node_port_info(graph,
							port.source_execution_index,
							SPA_DIRECTION_OUTPUT,
							port.source_port_ordinal,
							&source_port)) < 0)
						return res;
					spa_debugc(ctx, "%*ssource=[%" PRIu32 "] %s:%s",
							indent + 8, "",
							port.source_execution_index, source_node.name,
							source_port.info->name);
				}
				if (direction == SPA_DIRECTION_OUTPUT) {
					spa_debugc(ctx, "%*sconsumers=%" PRIu32,
							indent + 8, "", port.n_consumers);
					spa_debugc(ctx, "%*sbuffer: allocated=%" PRIu32
							" internal=%" PRIu32 " bytes",
							indent + 8, "", port.allocated_buffer_bytes,
							port.internal_buffer_bytes);
				}
			}
		}
	}

	spa_debugc(ctx, "%*slinks:", indent + 2, "");
	for (i = 0; i < graph_info.n_links; i++) {
		struct spa_fgn_link_info link;
		struct spa_fgn_node_info input_node;
		struct spa_fgn_node_info output_node;
		struct spa_fgn_graph_port_info input_port;
		struct spa_fgn_graph_port_info output_port;

		memset(&link, 0, sizeof(link));
		memset(&input_node, 0, sizeof(input_node));
		memset(&output_node, 0, sizeof(output_node));
		memset(&input_port, 0, sizeof(input_port));
		memset(&output_port, 0, sizeof(output_port));
		link.struct_size = sizeof(link);
		input_node.struct_size = sizeof(input_node);
		output_node.struct_size = sizeof(output_node);
		input_port.struct_size = sizeof(input_port);
		output_port.struct_size = sizeof(output_port);
		if ((res = spa_fgn_graph_get_link_info(graph, i, &link)) < 0 ||
		    (res = spa_fgn_graph_get_node_info(graph,
				link.output_execution_index, &output_node)) < 0 ||
		    (res = spa_fgn_graph_get_node_port_info(graph,
				link.output_execution_index, SPA_DIRECTION_OUTPUT,
				link.output_port_ordinal, &output_port)) < 0 ||
		    (res = spa_fgn_graph_get_node_info(graph,
				link.input_execution_index, &input_node)) < 0 ||
		    (res = spa_fgn_graph_get_node_port_info(graph,
				link.input_execution_index, SPA_DIRECTION_INPUT,
				link.input_port_ordinal, &input_port)) < 0)
			return res;
		spa_debugc(ctx, "%*s[%" PRIu32 "] %s:%s -> %s:%s", indent + 4,
				"", i, output_node.name, output_port.info->name,
				input_node.name, input_port.info->name);
	}
	return 0;
}

SPA_API_DEBUG_FILTER_GRAPH_NDARRAY int
spa_debug_fgn_graph(int indent, const struct spa_fgn_graph *graph)
{
	return spa_debugc_fgn_graph(NULL, indent, graph);
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_DEBUG_FILTER_GRAPH_NDARRAY_H */
