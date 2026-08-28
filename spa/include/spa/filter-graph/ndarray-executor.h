/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FILTER_GRAPH_NDARRAY_EXECUTOR_H
#define SPA_FILTER_GRAPH_NDARRAY_EXECUTOR_H

#include <stdint.h>

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Version of the transport-neutral ndarray executor interface. */
#define SPA_FGN_EXECUTOR_VERSION 1u

/** Admission limit for persistent helper threads in one worker group. */
#define SPA_FGN_EXECUTOR_MAX_HELPERS 63u

/** Executor capabilities and operating policy. */
enum spa_fgn_executor_flag {
	SPA_FGN_EXECUTOR_FLAG_NONE = 0,
	/** Helper lanes poll; repeated dispatch performs no wake system call. */
	SPA_FGN_EXECUTOR_FLAG_POLLING = (1u << 0),
};

/** Dense-F32 operation flags. */
enum spa_fgn_dense_f32_flag {
	SPA_FGN_DENSE_F32_FLAG_NONE = 0,
	/** Add this column range to the existing output instead of replacing it. */
	SPA_FGN_DENSE_F32_FLAG_ACCUMULATE = (1u << 0),
};

/**
 * One borrowed row-major dense F32 operation.
 *
 * The operation evaluates the matrix column range
 * `[first_column, first_column + n_columns)`. `input` points to the first
 * value of that range, while `matrix` points to element (0, 0) of the complete
 * prepared matrix. The matrix row stride is expressed in F32 elements.
 *
 * With ACCUMULATE clear:
 *
 *     output = matrix[:, range] * input
 *
 * With ACCUMULATE set:
 *
 *     output += matrix[:, range] * input
 *
 * Each range uses the executor's deterministic dense-F32 reduction profile.
 * Splitting one range into multiple ACCUMULATE calls can change floating-point
 * grouping and is not promised to be bitwise equal to one whole-range call.
 * Helper count does not change the result of any one task.
 *
 * The executor borrows every pointer only until run_dense_f32() returns.
 * Mutable output must not overlap matrix or input storage.
 */
struct spa_fgn_dense_f32_task {
	uint32_t struct_size;
	uint32_t flags;              /**< a mask of enum spa_fgn_dense_f32_flag */
	const float *matrix;
	const float *input;
	float *output;
	uint32_t n_rows;
	uint32_t matrix_row_stride;
	uint32_t first_column;
	uint32_t n_columns;
};

/** Function executed once for each lane in one synchronous adapter task. */
typedef int (*spa_fgn_lane_func_t)(void *data, uint32_t lane,
		uint32_t n_lanes);

/**
 * One borrowed fixed-lane adapter task.
 *
 * The executor invokes `run(data, lane, n_lanes)` exactly once for each lane
 * in `[0, n_lanes)`. Lane zero runs on the data-loop coordinator. The caller
 * owns all algorithm-specific validation and must give concurrently executing
 * lanes disjoint mutable storage or an explicitly synchronized reduction.
 * Every pointer is borrowed only until run_lanes() returns.
 */
struct spa_fgn_lanes_task {
	uint32_t struct_size;
	uint32_t flags;              /**< reserved; must be zero */
	void *data;
	spa_fgn_lane_func_t run;
	uint32_t n_lanes;
	uint32_t reserved;
};

/**
 * Host-owned execution interface borrowed by one plugin instance.
 *
 * The interface and data pointer remain valid until instance cleanup. One
 * graph data-loop coordinator may invoke it at a time, only from process() or
 * prepare_process_thread(). A call is synchronous: all lanes have stopped
 * accessing the borrowed task when it returns. `n_lanes` includes the
 * coordinator and is one for the serial fallback.
 */
struct spa_fgn_executor {
	uint32_t struct_size;
	uint32_t version;
	uint32_t flags;              /**< a mask of enum spa_fgn_executor_flag */
	uint32_t n_lanes;
	void *data;
	int (*run_dense_f32)(void *data,
			const struct spa_fgn_dense_f32_task *task);
	int (*run_lanes)(void *data,
			const struct spa_fgn_lanes_task *task);
	uint32_t cache_line_size;     /**< mutable-lane isolation granularity */
	uint32_t reserved;            /**< must be zero */
};

/** Opaque owner of one coordinator and zero or more persistent helper lanes. */
struct spa_fgn_worker_group;

/** Allocate fixed-capacity worker state without starting helper threads. */
int spa_fgn_worker_group_new(uint32_t n_helpers,
		struct spa_fgn_worker_group **group);

/** Start configured polling helpers outside repeated processing. */
int spa_fgn_worker_group_activate(struct spa_fgn_worker_group *group);

/** Stop and join every helper outside repeated processing. */
int spa_fgn_worker_group_deactivate(struct spa_fgn_worker_group *group);

/** Deactivate and release a worker group. */
void spa_fgn_worker_group_free(struct spa_fgn_worker_group *group);

/** Return the stable executor interface owned by `group`. */
const struct spa_fgn_executor *spa_fgn_worker_group_get_executor(
		const struct spa_fgn_worker_group *group);

/** Execute one validated dense operation synchronously. */
int spa_fgn_worker_group_run_dense_f32(struct spa_fgn_worker_group *group,
		const struct spa_fgn_dense_f32_task *task);

/** Execute one validated fixed-lane adapter task synchronously. */
int spa_fgn_worker_group_run_lanes(struct spa_fgn_worker_group *group,
		const struct spa_fgn_lanes_task *task);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_FILTER_GRAPH_NDARRAY_EXECUTOR_H */
