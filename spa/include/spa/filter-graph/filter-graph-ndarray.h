/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FILTER_GRAPH_NDARRAY_H
#define SPA_FILTER_GRAPH_NDARRAY_H

#include <stdint.h>

#include <spa/buffer/buffer.h>
#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/pod/builder.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque synchronous ndarray filter graph. */
struct spa_fgn_graph;

/**
 * Build a graph from a standard SPA JSON/config-syntax object.
 *
 * Every node uses `plugin` for a shared-library path and `label` for the
 * descriptor within that library. Nodes may contain a plugin-specific relaxed
 * SPA `config` object, which the host canonicalizes to standard JSON before
 * instance construction, and an initial runtime-only scalar `props` object.
 * The graph is acyclic and single-rate: every specified port rate must use the
 * same exact numerator and denominator.
 */
int spa_fgn_graph_new(const char *config, struct spa_fgn_graph **graph);
void spa_fgn_graph_free(struct spa_fgn_graph *graph);

uint32_t spa_fgn_graph_get_n_inputs(const struct spa_fgn_graph *graph);
uint32_t spa_fgn_graph_get_n_outputs(const struct spa_fgn_graph *graph);

int spa_fgn_graph_get_port_format(const struct spa_fgn_graph *graph,
		enum spa_direction direction, uint32_t port,
		const struct spa_fgn_format **format);

/** Get borrowed configured-node and plugin-port information. */
int spa_fgn_graph_get_port_info(const struct spa_fgn_graph *graph,
		enum spa_direction direction, uint32_t port, const char **node_name,
		const struct spa_fgn_port_info **info);

/** Build one namespaced SPA_PARAM_PropInfo object. */
int spa_fgn_graph_enum_prop_info(struct spa_fgn_graph *graph, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param);

/** Build the graph's namespaced SPA_PARAM_Props object. */
int spa_fgn_graph_get_props(struct spa_fgn_graph *graph,
		struct spa_pod_builder *builder, struct spa_pod **props);

/**
 * Failure-atomically prepare one namespaced SPA_PARAM_Props update.
 *
 * A non-empty successful transaction occupies one bounded pending slot. The
 * data-loop owner publishes all affected plugin instances at the beginning
 * of a later graph cycle. Returns -EBUSY until the pending or retired slot is
 * available. An empty update is a successful no-op.
 */
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

/** One external parameter-port assignment in a graph transaction. */
struct spa_fgn_parameter_update {
	uint32_t input_port;
	uint32_t reserved;
	struct spa_buffer *buffer;
};

/**
 * Failure-atomically prepare one nonempty graph parameter transaction.
 *
 * External input ports are unique. Assignments for one node compose one
 * complete replacement plan. A successful transaction occupies one bounded
 * pending graph slot. At a later graph-cycle start, the data-loop owner first
 * publishes every affected node and then adopts all replacements before any
 * numerical process callback. Returns -EBUSY until the pending or retired
 * transaction can be reclaimed on the serial control path. The complete graph
 * transaction admits at most
 * SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS entries.
 */
int spa_fgn_graph_set_parameters(struct spa_fgn_graph *graph,
		const struct spa_fgn_parameter_update *updates,
		uint32_t n_updates);

int spa_fgn_graph_activate(struct spa_fgn_graph *graph);
int spa_fgn_graph_deactivate(struct spa_fgn_graph *graph);
int spa_fgn_graph_reset(struct spa_fgn_graph *graph);

/** Successful graph-process result flags. */
enum spa_fgn_process_result {
	SPA_FGN_PROCESS_RESULT_NONE = 0,
	/** At least one externally visible property value changed. */
	SPA_FGN_PROCESS_RESULT_PROPS_CHANGED = (1u << 0),
};

/**
 * Process one complete synchronous graph cycle.
 *
 * Returns a negative errno-style error or a mask of enum
 * spa_fgn_process_result. An inputs or outputs array may be NULL exactly when
 * its corresponding count is zero. A processing error clears external output
 * sizes but does not roll back state already changed by an earlier node.
 */
int spa_fgn_graph_process(struct spa_fgn_graph *graph,
		struct spa_buffer *const inputs[], uint32_t n_inputs,
		struct spa_buffer *const outputs[], uint32_t n_outputs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_FILTER_GRAPH_NDARRAY_H */
