/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FILTER_GRAPH_NDARRAY_GRAPH_H
#define SPA_FILTER_GRAPH_NDARRAY_GRAPH_H

#include <stdint.h>

#include <spa/buffer/buffer.h>
#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/builder.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque synchronous ndarray operation graph. */
struct spa_fgn_graph;

/**
 * Build a graph from a standard SPA JSON/config-syntax object.
 *
 * Every node uses `plugin` for a shared-library path and `label` for the
 * descriptor within that library. Nodes may contain plugin-specific `config`
 * and initial scalar `props` objects.
 */
int spa_fgn_graph_new(const char *config, struct spa_fgn_graph **graph);
void spa_fgn_graph_free(struct spa_fgn_graph *graph);

uint32_t spa_fgn_graph_get_n_inputs(const struct spa_fgn_graph *graph);
uint32_t spa_fgn_graph_get_n_outputs(const struct spa_fgn_graph *graph);

int spa_fgn_graph_get_port_format(const struct spa_fgn_graph *graph,
		uint32_t direction, uint32_t port,
		const struct spa_fgn_format **format);

/** Get borrowed configured-node and operation-port information. */
int spa_fgn_graph_get_port_info(const struct spa_fgn_graph *graph,
		uint32_t direction, uint32_t port, const char **node_name,
		const struct spa_fgn_port_info **info);

/** Build one namespaced SPA_PARAM_PropInfo object. */
int spa_fgn_graph_enum_prop_info(struct spa_fgn_graph *graph, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param);

/** Build the graph's namespaced SPA_PARAM_Props object. */
int spa_fgn_graph_get_props(struct spa_fgn_graph *graph,
		struct spa_pod_builder *builder, struct spa_pod **props);

/** Atomically prepare and commit one namespaced SPA_PARAM_Props update. */
int spa_fgn_graph_set_props(struct spa_fgn_graph *graph,
		const struct spa_pod *props);

/**
 * Prepare and publish one sparse external ndarray parameter-port update.
 *
 * Call this from a serial control or worker context, never from the graph data
 * loop. The buffer is borrowed only for the duration of this call. A plugin
 * returns -EBUSY when its bounded prepared-state capacity is full.
 */
int spa_fgn_graph_update_parameter(struct spa_fgn_graph *graph,
		uint32_t input_port, struct spa_buffer *buffer);

int spa_fgn_graph_activate(struct spa_fgn_graph *graph);
int spa_fgn_graph_deactivate(struct spa_fgn_graph *graph);
int spa_fgn_graph_reset(struct spa_fgn_graph *graph);

/** Process one complete synchronous graph cycle. */
int spa_fgn_graph_process(struct spa_fgn_graph *graph,
		struct spa_buffer *const inputs[], uint32_t n_inputs,
		struct spa_buffer *const outputs[], uint32_t n_outputs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_FILTER_GRAPH_NDARRAY_GRAPH_H */
