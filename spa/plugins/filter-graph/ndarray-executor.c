/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/filter-graph/ndarray-executor.h>
#include <spa/utils/overflow.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define FGN_HAVE_X86_AVX2_FMA 1
#define FGN_TARGET_AVX2_FMA __attribute__((target("avx2,fma")))
#else
#define FGN_HAVE_X86_AVX2_FMA 0
#define FGN_TARGET_AVX2_FMA
#endif

typedef int (*fgn_worker_task_func_t)(void *data, uint32_t lane,
		uint32_t n_lanes);

union SPA_ALIGNED(SPA_CACHE_LINE_SIZE) fgn_worker_command_line {
	struct {
		_Atomic uint32_t generation;
		fgn_worker_task_func_t task;
		void *data;
		uint32_t n_lanes;
		uint32_t next_generation;
	} value;
	uint8_t cache_line[SPA_CACHE_LINE_SIZE];
};

union SPA_ALIGNED(SPA_CACHE_LINE_SIZE) fgn_worker_completion_line {
	struct {
		_Atomic uint32_t generation;
		int status;
	} value;
	uint8_t cache_line[SPA_CACHE_LINE_SIZE];
};

struct fgn_worker_context {
	uint32_t lane;
	_Atomic bool started;
};

struct SPA_ALIGNED(SPA_CACHE_LINE_SIZE) fgn_worker_slot {
	union fgn_worker_command_line command;
	union fgn_worker_completion_line completion;
	struct fgn_worker_context context;
};

SPA_STATIC_ASSERT(sizeof(union fgn_worker_command_line) == SPA_CACHE_LINE_SIZE,
		"worker command must occupy one cache line");
SPA_STATIC_ASSERT(sizeof(union fgn_worker_completion_line) == SPA_CACHE_LINE_SIZE,
		"worker completion must occupy one cache line");
SPA_STATIC_ASSERT(offsetof(struct fgn_worker_slot, command) == 0u,
		"worker command must start on a cache-line boundary");
SPA_STATIC_ASSERT(offsetof(struct fgn_worker_slot, completion) == SPA_CACHE_LINE_SIZE,
		"worker completion must use a separate cache line");
SPA_STATIC_ASSERT(offsetof(struct fgn_worker_slot, context) ==
		2u * SPA_CACHE_LINE_SIZE,
		"worker-only context must not share a publication cache line");
SPA_STATIC_ASSERT(sizeof(struct fgn_worker_slot) % SPA_CACHE_LINE_SIZE == 0u,
		"worker slots must preserve cache-line alignment in arrays");

struct spa_fgn_worker_group {
	uint32_t n_helpers;
	uint32_t n_created;
	bool active;
	struct fgn_worker_slot *slots;
	pthread_t *threads;
	struct spa_fgn_executor executor;
};

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield");
#else
	atomic_signal_fence(memory_order_seq_cst);
#endif
}

static void *worker_main(void *data)
{
	struct fgn_worker_slot *slot = data;
	uint32_t observed = atomic_load_explicit(
			&slot->command.value.generation, memory_order_relaxed);

	atomic_store_explicit(&slot->context.started, true, memory_order_release);

	for (;;) {
		fgn_worker_task_func_t task;
		void *task_data;
		uint32_t generation, n_lanes;
		int status;

		do {
			generation = atomic_load_explicit(
					&slot->command.value.generation,
					memory_order_acquire);
			if (generation == observed)
				cpu_relax();
		} while (generation == observed);

		observed = generation;
		task = slot->command.value.task;
		if (task == NULL)
			break;
		task_data = slot->command.value.data;
		n_lanes = slot->command.value.n_lanes;
		status = task(task_data, slot->context.lane, n_lanes);
		slot->completion.value.status = status;
		atomic_store_explicit(&slot->completion.value.generation,
				generation, memory_order_release);
	}
	return NULL;
}

static uint32_t next_generation(struct fgn_worker_slot *slot)
{
	return ++slot->command.value.next_generation;
}

static void publish_command(struct fgn_worker_slot *slot,
		fgn_worker_task_func_t task, void *data, uint32_t n_lanes)
{
	uint32_t generation = next_generation(slot);

	/*
	 * Rearm completion before publishing the next command. This is required
	 * when the 32-bit generation wraps: an old equal completion value must not
	 * let the coordinator return before the worker executes the new command.
	 * The preceding command was acquired as complete before this slot can be
	 * reused, so the helper no longer writes its old completion.
	 */
	atomic_store_explicit(&slot->completion.value.generation,
			generation - 1u, memory_order_relaxed);
	slot->command.value.task = task;
	slot->command.value.data = data;
	slot->command.value.n_lanes = n_lanes;
	atomic_store_explicit(&slot->command.value.generation, generation,
			memory_order_release);
}

static int stop_created_workers(struct spa_fgn_worker_group *group)
{
	uint32_t i;
	int result = 0;

	for (i = 0; i < group->n_created; i++)
		while (!atomic_load_explicit(&group->slots[i].context.started,
				memory_order_acquire))
			cpu_relax();
	for (i = 0; i < group->n_created; i++)
		publish_command(&group->slots[i], NULL, NULL, 0);
	for (i = 0; i < group->n_created; i++) {
		int res = pthread_join(group->threads[i], NULL);
		if (result == 0 && res != 0)
			result = -res;
	}
	group->n_created = 0;
	return result;
}

static int run_task(struct spa_fgn_worker_group *group,
		fgn_worker_task_func_t task, void *data, uint32_t n_lanes)
{
	uint32_t i;
	int result;

	if (group == NULL || task == NULL || !group->active || n_lanes == 0 ||
	    n_lanes > group->n_helpers + 1u)
		return -EINVAL;

	for (i = 0; i + 1u < n_lanes; i++)
		publish_command(&group->slots[i], task, data, n_lanes);

	result = task(data, 0, n_lanes);
	for (i = 0; i + 1u < n_lanes; i++) {
		struct fgn_worker_slot *slot = &group->slots[i];
		uint32_t expected = slot->command.value.next_generation;
		uint32_t observed;

		do {
			observed = atomic_load_explicit(
					&slot->completion.value.generation,
					memory_order_acquire);
			if (observed != expected)
				cpu_relax();
		} while (observed != expected);
		if (result == 0 && slot->completion.value.status != 0)
			result = slot->completion.value.status;
	}
	return result;
}

static bool region(const void *pointer, size_t size, uintptr_t *begin,
		uintptr_t *end)
{
	uintptr_t address = (uintptr_t)pointer;

	if (size > UINTPTR_MAX - address)
		return false;
	*begin = address;
	*end = address + size;
	return true;
}

static bool regions_overlap(uintptr_t a_begin, uintptr_t a_end,
		uintptr_t b_begin, uintptr_t b_end)
{
	return a_begin < b_end && b_begin < a_end;
}

static int validate_dense_task(const struct spa_fgn_dense_f32_task *task)
{
	size_t last_row, matrix_elements, matrix_bytes, input_bytes, output_bytes;
	uintptr_t matrix_begin, matrix_end, input_begin, input_end;
	uintptr_t output_begin, output_end;

	if (task == NULL || task->struct_size < sizeof(*task) ||
	    (task->flags & ~SPA_FGN_DENSE_F32_FLAG_ACCUMULATE) != 0 ||
	    task->matrix == NULL || task->input == NULL || task->output == NULL ||
	    task->n_rows == 0 || task->n_columns == 0 ||
	    task->matrix_row_stride == 0 ||
	    task->first_column > task->matrix_row_stride ||
	    task->n_columns > task->matrix_row_stride - task->first_column ||
	    (uintptr_t)task->matrix % SPA_ALIGNOF(float) != 0 ||
	    (uintptr_t)task->input % SPA_ALIGNOF(float) != 0 ||
	    (uintptr_t)task->output % SPA_ALIGNOF(float) != 0)
		return -EINVAL;

	if (spa_overflow_mul((size_t)task->n_rows - 1u,
			(size_t)task->matrix_row_stride, &last_row) ||
	    spa_overflow_add(last_row, (size_t)task->first_column,
			&matrix_elements) ||
	    spa_overflow_add(matrix_elements, (size_t)task->n_columns,
			&matrix_elements) ||
	    spa_overflow_mul(matrix_elements, sizeof(float), &matrix_bytes) ||
	    spa_overflow_mul((size_t)task->n_columns, sizeof(float),
			&input_bytes) ||
	    spa_overflow_mul((size_t)task->n_rows, sizeof(float),
			&output_bytes))
		return -EOVERFLOW;
	if (!region(task->matrix, matrix_bytes, &matrix_begin, &matrix_end) ||
	    !region(task->input, input_bytes, &input_begin, &input_end) ||
	    !region(task->output, output_bytes, &output_begin, &output_end))
		return -EOVERFLOW;
	if (regions_overlap(output_begin, output_end, matrix_begin, matrix_end) ||
	    regions_overlap(output_begin, output_end, input_begin, input_end))
		return -EINVAL;
	return 0;
}

static uint32_t proportional_row(uint32_t rows, uint32_t lane,
		uint32_t n_lanes)
{
	uint32_t quotient = rows / n_lanes;
	uint32_t remainder = rows % n_lanes;

	return lane * quotient + (lane < remainder ? lane : remainder);
}

static uint32_t cache_line_boundary(const struct spa_fgn_dense_f32_task *task,
		uint32_t lane, uint32_t n_lanes)
{
	uint32_t row;
	uintptr_t address, misalignment;
	size_t adjustment, adjusted_row;

	if (lane == 0)
		return 0;
	if (lane >= n_lanes)
		return task->n_rows;
	row = proportional_row(task->n_rows, lane, n_lanes);
	address = (uintptr_t)task->output + (size_t)row * sizeof(float);
	misalignment = address & (SPA_CACHE_LINE_SIZE - 1u);
	adjustment = misalignment == 0 ? 0 : SPA_CACHE_LINE_SIZE - misalignment;
	adjusted_row = (size_t)row + adjustment / sizeof(float);
	return adjusted_row < task->n_rows ? (uint32_t)adjusted_row : task->n_rows;
}

typedef float (*dense_dot_f32_func_t)(const float *left,
		const float *right, uint32_t count);

/*
 * This reduction is the portable dense-F32 numerical fallback used by the
 * Calculon PreparedGemv implementation: four fused accumulators, followed by
 * the same balanced reduction. Keep its order stable across executor lanes.
 */
static float dense_dot_f32_scalar(const float *left, const float *right,
		uint32_t count)
{
	float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
	uint32_t chunks = count / 4u;
	uint32_t remainder = count % 4u;
	uint32_t i;

	for (i = 0; i < chunks; i++) {
		uint32_t base = i * 4u;

		sum0 = fmaf(left[base], right[base], sum0);
		sum1 = fmaf(left[base + 1u], right[base + 1u], sum1);
		sum2 = fmaf(left[base + 2u], right[base + 2u], sum2);
		sum3 = fmaf(left[base + 3u], right[base + 3u], sum3);
	}
	for (i = 0; i < remainder; i++) {
		uint32_t index = chunks * 4u + i;

		sum0 = fmaf(left[index], right[index], sum0);
	}
	return (sum0 + sum1) + (sum2 + sum3);
}

#if FGN_HAVE_X86_AVX2_FMA
static FGN_TARGET_AVX2_FMA float dense_dot_f32_avx2(const float *left,
		const float *right, uint32_t count)
{
	__m256 sum0 = _mm256_setzero_ps();
	__m256 sum1 = _mm256_setzero_ps();
	__m256 sum2 = _mm256_setzero_ps();
	__m256 sum3 = _mm256_setzero_ps();
	float values[8];
	float sum;
	uint32_t chunks = count / 32u;
	uint32_t remainder = count % 32u;
	uint32_t i;

	for (i = 0; i < chunks; i++) {
		uint32_t base = i * 32u;

		sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(left + base),
				_mm256_loadu_ps(right + base), sum0);
		sum1 = _mm256_fmadd_ps(_mm256_loadu_ps(left + base + 8u),
				_mm256_loadu_ps(right + base + 8u), sum1);
		sum2 = _mm256_fmadd_ps(_mm256_loadu_ps(left + base + 16u),
				_mm256_loadu_ps(right + base + 16u), sum2);
		sum3 = _mm256_fmadd_ps(_mm256_loadu_ps(left + base + 24u),
				_mm256_loadu_ps(right + base + 24u), sum3);
	}
	sum0 = _mm256_add_ps(_mm256_add_ps(sum0, sum1),
			_mm256_add_ps(sum2, sum3));
	_mm256_storeu_ps(values, sum0);
	sum = values[0] + values[1];
	for (i = 2u; i < 8u; i++)
		sum += values[i];
	for (i = 0; i < remainder; i++) {
		uint32_t index = chunks * 32u + i;

		sum = fmaf(left[index], right[index], sum);
	}
	return sum;
}
#endif

#if defined(__aarch64__)
static float dense_dot_f32_neon(const float *left, const float *right,
		uint32_t count)
{
	float32x4_t sum0 = vdupq_n_f32(0.0f);
	float32x4_t sum1 = vdupq_n_f32(0.0f);
	float32x4_t sum2 = vdupq_n_f32(0.0f);
	float32x4_t sum3 = vdupq_n_f32(0.0f);
	uint32_t chunks = count / 16u;
	uint32_t remainder = count % 16u;
	uint32_t i;
	float sum;

	for (i = 0; i < chunks; i++) {
		uint32_t base = i * 16u;

		sum0 = vfmaq_f32(sum0, vld1q_f32(left + base),
				vld1q_f32(right + base));
		sum1 = vfmaq_f32(sum1, vld1q_f32(left + base + 4u),
				vld1q_f32(right + base + 4u));
		sum2 = vfmaq_f32(sum2, vld1q_f32(left + base + 8u),
				vld1q_f32(right + base + 8u));
		sum3 = vfmaq_f32(sum3, vld1q_f32(left + base + 12u),
				vld1q_f32(right + base + 12u));
	}
	sum = vaddvq_f32(vaddq_f32(vaddq_f32(sum0, sum1),
			vaddq_f32(sum2, sum3)));
	for (i = 0; i < remainder; i++) {
		uint32_t index = chunks * 16u + i;

		sum = fmaf(left[index], right[index], sum);
	}
	return sum;
}
#endif

static float dense_dot_f32_blocked(const float *left, const float *right,
		uint32_t count)
{
	const uint32_t block_size = 2048u;
	float sum = 0.0f;
	uint32_t first;
	dense_dot_f32_func_t block;

#if FGN_HAVE_X86_AVX2_FMA
	block = dense_dot_f32_avx2;
#elif defined(__aarch64__)
	block = dense_dot_f32_neon;
#else
	block = dense_dot_f32_scalar;
#endif
	for (first = 0; first < count; first += block_size) {
		uint32_t remaining = count - first;
		uint32_t size = SPA_MIN(remaining, block_size);

		sum += block(left + first, right + first, size);
	}
	return sum;
}

static dense_dot_f32_func_t select_dense_dot_f32(uint32_t count)
{
	if (count < 32u)
		return dense_dot_f32_scalar;
#if FGN_HAVE_X86_AVX2_FMA
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
		return count < 8192u ? dense_dot_f32_avx2 : dense_dot_f32_blocked;
#elif defined(__aarch64__)
	return count < 8192u ? dense_dot_f32_neon : dense_dot_f32_blocked;
#endif
	return dense_dot_f32_scalar;
}

static int dense_task(void *data, uint32_t lane, uint32_t n_lanes)
{
	const struct spa_fgn_dense_f32_task *task = data;
	uint32_t first = cache_line_boundary(task, lane, n_lanes);
	uint32_t last = cache_line_boundary(task, lane + 1u, n_lanes);
	bool accumulate = (task->flags & SPA_FGN_DENSE_F32_FLAG_ACCUMULATE) != 0;
	dense_dot_f32_func_t dot = select_dense_dot_f32(task->n_columns);
	uint32_t row;

	for (row = first; row < last; row++) {
		const float *matrix = task->matrix +
				(size_t)row * task->matrix_row_stride +
				task->first_column;
		float sum = dot(matrix, task->input, task->n_columns);

		task->output[row] = accumulate ? task->output[row] + sum : sum;
	}
	return 0;
}

static int executor_run_dense_f32(void *data,
		const struct spa_fgn_dense_f32_task *task)
{
	return spa_fgn_worker_group_run_dense_f32(data, task);
}

static int lanes_task(void *data, uint32_t lane, uint32_t n_lanes)
{
	const struct spa_fgn_lanes_task *task = data;

	return task->run(task->data, lane, n_lanes);
}

static int executor_run_lanes(void *data,
		const struct spa_fgn_lanes_task *task)
{
	return spa_fgn_worker_group_run_lanes(data, task);
}

int spa_fgn_worker_group_new(uint32_t n_helpers,
		struct spa_fgn_worker_group **result)
{
	struct spa_fgn_worker_group *group;
	size_t slots_size, threads_size;
	uint32_t i;
	int res;

	if (result == NULL || n_helpers > SPA_FGN_EXECUTOR_MAX_HELPERS)
		return -EINVAL;
	*result = NULL;
	if ((group = calloc(1, sizeof(*group))) == NULL)
		return -ENOMEM;
	group->n_helpers = n_helpers;
	if (n_helpers > 0) {
		if (spa_overflow_mul((size_t)n_helpers, sizeof(*group->slots),
				&slots_size) ||
		    spa_overflow_mul((size_t)n_helpers, sizeof(*group->threads),
				&threads_size)) {
			res = -EOVERFLOW;
			goto error;
		}
		res = posix_memalign((void **)&group->slots, SPA_CACHE_LINE_SIZE,
				slots_size);
		if (res != 0) {
			res = -res;
			goto error;
		}
		memset(group->slots, 0, slots_size);
		if ((group->threads = calloc(1, threads_size)) == NULL) {
			res = -ENOMEM;
			goto error;
		}
		for (i = 0; i < n_helpers; i++) {
			struct fgn_worker_slot *slot = &group->slots[i];

			slot->context.lane = i + 1u;
			atomic_init(&slot->command.value.generation, 0);
			atomic_init(&slot->completion.value.generation, 0);
			atomic_init(&slot->context.started, false);
			if (!atomic_is_lock_free(&slot->command.value.generation) ||
			    !atomic_is_lock_free(&slot->completion.value.generation) ||
			    !atomic_is_lock_free(&slot->context.started)) {
				res = -ENOTSUP;
				goto error;
			}
		}
	}
	group->executor = (struct spa_fgn_executor) {
		.struct_size = sizeof(struct spa_fgn_executor),
		.version = SPA_FGN_EXECUTOR_VERSION,
		.flags = n_helpers == 0 ? SPA_FGN_EXECUTOR_FLAG_NONE :
			SPA_FGN_EXECUTOR_FLAG_POLLING,
		.n_lanes = n_helpers + 1u,
		.data = group,
		.run_dense_f32 = executor_run_dense_f32,
		.run_lanes = executor_run_lanes,
		.cache_line_size = SPA_CACHE_LINE_SIZE,
	};
	*result = group;
	return 0;

error:
	free(group->threads);
	free(group->slots);
	free(group);
	return res;
}

int spa_fgn_worker_group_activate(struct spa_fgn_worker_group *group)
{
	uint32_t i;

	if (group == NULL)
		return -EINVAL;
	if (group->active)
		return 0;
	for (i = 0; i < group->n_helpers; i++) {
		atomic_store_explicit(&group->slots[i].context.started, false,
				memory_order_relaxed);
		int res = pthread_create(&group->threads[i], NULL, worker_main,
				&group->slots[i]);
		if (res != 0) {
			int stop_res;

			group->n_created = i;
			stop_res = stop_created_workers(group);
			return stop_res < 0 ? stop_res : -res;
		}
		group->n_created = i + 1u;
	}
	for (i = 0; i < group->n_helpers; i++)
		while (!atomic_load_explicit(&group->slots[i].context.started,
				memory_order_acquire))
			cpu_relax();
	group->active = true;
	return 0;
}

int spa_fgn_worker_group_deactivate(struct spa_fgn_worker_group *group)
{
	if (group == NULL)
		return -EINVAL;
	if (!group->active)
		return 0;
	group->active = false;
	return stop_created_workers(group);
}

void spa_fgn_worker_group_free(struct spa_fgn_worker_group *group)
{
	if (group == NULL)
		return;
	spa_fgn_worker_group_deactivate(group);
	free(group->threads);
	free(group->slots);
	free(group);
}

const struct spa_fgn_executor *spa_fgn_worker_group_get_executor(
		const struct spa_fgn_worker_group *group)
{
	return group != NULL ? &group->executor : NULL;
}

int spa_fgn_worker_group_run_dense_f32(struct spa_fgn_worker_group *group,
		const struct spa_fgn_dense_f32_task *task)
{
	int res;

	if (group == NULL || !group->active)
		return -EINVAL;
	if ((res = validate_dense_task(task)) < 0)
		return res;
	return run_task(group, dense_task, (void *)task, group->n_helpers + 1u);
}

int spa_fgn_worker_group_run_lanes(struct spa_fgn_worker_group *group,
		const struct spa_fgn_lanes_task *task)
{
	if (group == NULL || !group->active || task == NULL ||
	    task->struct_size < sizeof(*task) || task->flags != 0 ||
	    task->run == NULL || task->n_lanes == 0 ||
	    task->n_lanes > group->n_helpers + 1u || task->reserved != 0)
		return -EINVAL;

	return run_task(group, lanes_task, (void *)task, task->n_lanes);
}
