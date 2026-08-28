/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/*
 * Closed-loop service-time benchmark for one synchronous dense-F32 executor
 * call. Thread creation, activation, correctness checks, and reporting are
 * outside the timed boundary. Use an external cpuset for helper qualification.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/filter-graph/ndarray-executor.h>
#include <spa/utils/overflow.h>

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static uint32_t parse_u32(const char *text, bool allow_zero,
		uint32_t maximum)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    (!allow_zero && value == 0) || value > maximum)
		fail("invalid unsigned benchmark argument");
	return (uint32_t)value;
}

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0)
		fail("clock_gettime failed");
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static size_t percentile(size_t count, uint32_t numerator,
		uint32_t denominator)
{
	return ((count * numerator + denominator - 1u) / denominator) - 1u;
}

static void report(const char *name, uint64_t *samples, size_t count)
{
	long double sum = 0.0;
	size_t i;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (i = 0; i < count; i++)
		sum += samples[i];
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64 " ns p99=%" PRIu64,
			name, count, sum / count, samples[count / 2u],
			samples[percentile(count, 90, 100)],
			samples[percentile(count, 99, 100)]);
	if (count >= 1000u)
		printf(" ns p99.9=%" PRIu64,
				samples[percentile(count, 999, 1000)]);
	printf(" ns max=%" PRIu64 " ns\n", samples[count - 1u]);
}

static void write_csv(const char *path, const uint64_t *dense,
		const uint64_t *clock_pair, size_t count)
{
	FILE *file;
	size_t i;

	if (path == NULL)
		return;
	file = fopen(path, "w");
	if (file == NULL)
		fail("cannot create executor timing CSV");
	if (fputs("iteration,dense_ns,clock_pair_ns\n", file) < 0)
		fail("cannot write executor timing CSV header");
	for (i = 0; i < count; i++)
		if (fprintf(file, "%zu,%" PRIu64 ",%" PRIu64 "\n",
				i, dense[i], clock_pair[i]) < 0)
			fail("cannot write executor timing sample");
	if (fclose(file) != 0)
		fail("cannot close executor timing CSV");
}

static uint32_t random_u32(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static float random_f32(uint32_t *state)
{
	int32_t value = (int32_t)(random_u32(state) & UINT32_C(0x00ffffff)) -
		INT32_C(0x00800000);

	return (float)value / 8388608.0f;
}

static void *allocate_aligned(size_t count, size_t element_size)
{
	void *result;

	if (count == 0 || element_size > SIZE_MAX / count ||
	    posix_memalign(&result, SPA_CACHE_LINE_SIZE,
		    count * element_size) != 0)
		fail("aligned benchmark allocation failed");
	return result;
}

static void run_checked(struct spa_fgn_worker_group *group,
		const struct spa_fgn_dense_f32_task *task)
{
	int result = spa_fgn_worker_group_run_dense_f32(group, task);

	if (result != 0) {
		fprintf(stderr, "dense executor failed: %d\n", result);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	struct spa_fgn_worker_group *serial = NULL, *candidate = NULL;
	struct spa_fgn_dense_f32_task task;
	float *matrix, *input, *reference, *output;
	uint64_t *times, *clock_times;
	uint32_t rows, columns, helpers, samples = 100000u, warmup = 10000u;
	uint32_t random_state = UINT32_C(0x6d2b79f5);
	uint32_t i;
	size_t element, matrix_elements;
	const char *csv = NULL;
	volatile float guard;

	if (argc < 4 || argc > 7) {
		fprintf(stderr,
				"usage: %s ROWS COLUMNS HELPERS [SAMPLES [WARMUP [CSV]]]\n",
				argv[0]);
		return EXIT_FAILURE;
	}
	rows = parse_u32(argv[1], false, UINT32_MAX);
	columns = parse_u32(argv[2], false, UINT32_MAX);
	helpers = parse_u32(argv[3], true, SPA_FGN_EXECUTOR_MAX_HELPERS);
	if (argc >= 5)
		samples = parse_u32(argv[4], false, UINT32_MAX);
	if (argc >= 6)
		warmup = parse_u32(argv[5], true, UINT32_MAX);
	if (argc == 7)
		csv = argv[6];

	if (spa_overflow_mul((size_t)rows, (size_t)columns, &matrix_elements))
		fail("matrix element extent overflow");
	matrix = allocate_aligned(matrix_elements, sizeof(float));
	input = allocate_aligned(columns, sizeof(float));
	reference = allocate_aligned(rows, sizeof(float));
	output = allocate_aligned(rows, sizeof(float));
	times = allocate_aligned(samples, sizeof(uint64_t));
	clock_times = allocate_aligned(samples, sizeof(uint64_t));
	for (element = 0; element < matrix_elements; element++)
		matrix[element] = random_f32(&random_state);
	for (i = 0; i < columns; i++)
		input[i] = random_f32(&random_state);

	task = (struct spa_fgn_dense_f32_task) {
		.struct_size = sizeof(task),
		.matrix = matrix,
		.input = input,
		.output = reference,
		.n_rows = rows,
		.matrix_row_stride = columns,
		.n_columns = columns,
	};
	if (spa_fgn_worker_group_new(0, &serial) != 0 ||
	    spa_fgn_worker_group_activate(serial) != 0)
		fail("cannot activate serial correctness executor");
	run_checked(serial, &task);
	if (spa_fgn_worker_group_deactivate(serial) != 0)
		fail("cannot deactivate serial correctness executor");
	spa_fgn_worker_group_free(serial);

	task.output = output;
	if (spa_fgn_worker_group_new(helpers, &candidate) != 0 ||
	    spa_fgn_worker_group_activate(candidate) != 0)
		fail("cannot activate benchmark executor");
	run_checked(candidate, &task);
	if (memcmp(output, reference, (size_t)rows * sizeof(float)) != 0)
		fail("candidate output differs from one-lane executor");
	for (i = 0; i < warmup; i++)
		run_checked(candidate, &task);
	for (i = 0; i < samples; i++) {
		uint64_t start = monotonic_ns();

		run_checked(candidate, &task);
		times[i] = monotonic_ns() - start;
	}
	if (memcmp(output, reference, (size_t)rows * sizeof(float)) != 0)
		fail("warmed candidate output differs from one-lane executor");
	guard = output[(rows - 1u) / 2u];
	for (i = 0; i < samples; i++) {
		uint64_t start = monotonic_ns();

		clock_times[i] = monotonic_ns() - start;
	}
	write_csv(csv, times, clock_times, samples);
	report("dense-F32 executor", times, samples);
	report("paired clock reads", clock_times, samples);
	printf("benchmark-scope: closed-loop synchronous service time; rows=%u columns=%u helpers=%u lanes=%u warmup=%u samples=%u output-guard=%.9g\n",
			rows, columns, helpers, helpers + 1u, warmup, samples,
			(double)guard);

	if (spa_fgn_worker_group_deactivate(candidate) != 0)
		fail("cannot deactivate benchmark executor");
	spa_fgn_worker_group_free(candidate);
	free(clock_times);
	free(times);
	free(output);
	free(reference);
	free(input);
	free(matrix);
	return EXIT_SUCCESS;
}
