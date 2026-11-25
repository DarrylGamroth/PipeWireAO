/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "queue.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP_ITEMS 32768u
#define TIMED_ITEMS 1048576u
#define SAMPLE_EVERY 64u
#define SAMPLE_CAPACITY (1u << 20)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct endpoint {
	int cpu;
	uint32_t attempts;
	uint32_t completed;
	uint32_t dropped;
	uint32_t count;
	uint64_t start_ns;
	uint64_t end_ns;
	uint64_t cpu_ns;
	uint64_t *samples;
};

struct benchmark {
	struct pwao_queue_ring ring;
	pthread_barrier_t start;
	bool dropping;
	bool lease;
	uint32_t items;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic bool done;
	_Alignas(SPA_CACHE_LINE_SIZE) struct endpoint producer;
	_Alignas(SPA_CACHE_LINE_SIZE) struct endpoint consumer;
};

static uint64_t now(clockid_t clock)
{
	struct timespec time;

	CHECK(clock_gettime(clock, &time) == 0);
	return (uint64_t)time.tv_sec * UINT64_C(1000000000) + time.tv_nsec;
}

static void start(struct benchmark *bench, struct endpoint *endpoint)
{
	cpu_set_t set;
	int result;

	CPU_ZERO(&set);
	CPU_SET(endpoint->cpu, &set);
	CHECK(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0);
	result = pthread_barrier_wait(&bench->start);
	CHECK(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
	endpoint->start_ns = now(CLOCK_MONOTONIC_RAW);
	endpoint->cpu_ns = now(CLOCK_THREAD_CPUTIME_ID);
}

static void finish(struct endpoint *endpoint)
{
	endpoint->cpu_ns = now(CLOCK_THREAD_CPUTIME_ID) - endpoint->cpu_ns;
	endpoint->end_ns = now(CLOCK_MONOTONIC_RAW);
}

static void *produce(void *data)
{
	struct benchmark *bench = data;
	struct endpoint *endpoint = &bench->producer;
	#ifdef PWAO_QUEUE_BENCH_BASELINE
	enum pwao_queue_overflow overflow = bench->dropping ?
			PWAO_QUEUE_OVERFLOW_DROP_OLDEST : PWAO_QUEUE_OVERFLOW_BACKPRESSURE;
#endif
	uint32_t sequence = 0;

	start(bench, endpoint);
	while (sequence < bench->items) {
		const bool sample = endpoint->attempts % SAMPLE_EVERY == 0 &&
				endpoint->count < SAMPLE_CAPACITY;
		const uint64_t before = sample ? now(CLOCK_MONOTONIC_RAW) : 0;
		uint64_t released;
		int result = pwao_queue_ring_admit(&bench->ring, sequence,
#ifdef PWAO_QUEUE_BENCH_BASELINE
				overflow,
#endif
				&released);

		if (sample)
			endpoint->samples[endpoint->count++] = now(CLOCK_MONOTONIC_RAW) - before;
		endpoint->attempts++;
		if (result == PWAO_QUEUE_ADMIT_BACKPRESSURE) {
			CHECK(!bench->dropping && released == UINT64_MAX);
			continue;
		}
		if (result == PWAO_QUEUE_ADMIT_REPLACED) {
			CHECK(bench->dropping && released < sequence);
			endpoint->dropped++;
		} else {
			CHECK(result == PWAO_QUEUE_ADMIT_QUEUED && released == UINT64_MAX);
		}
		sequence++;
	}
	endpoint->completed = sequence;
	atomic_store_explicit(&bench->done, true, memory_order_release);
	finish(endpoint);
	return NULL;
}

static void *consume(void *data)
{
	struct benchmark *bench = data;
	struct endpoint *endpoint = &bench->consumer;
	uint32_t previous = 0;

	start(bench, endpoint);
	for (;;) {
		const bool done = atomic_load_explicit(&bench->done, memory_order_acquire);
		const bool sample = endpoint->attempts % SAMPLE_EVERY == 0 &&
				endpoint->count < SAMPLE_CAPACITY;
		const uint64_t before = sample ? now(CLOCK_MONOTONIC_RAW) : 0;
		uint64_t value;
		int result;

		if (bench->lease) {
			result = pwao_queue_ring_try_peek(&bench->ring, &value);
			CHECK(result >= 0);
			if (result == 1)
				result = pwao_queue_ring_try_claim(&bench->ring, value);
		} else {
			result = pwao_queue_ring_try_pop(&bench->ring, &value);
		}
		if (sample)
			endpoint->samples[endpoint->count++] = now(CLOCK_MONOTONIC_RAW) - before;
		endpoint->attempts++;
		if (result != 1) {
			CHECK(result == 0 || (bench->dropping && result == -EAGAIN));
			if (done && result == 0)
				break;
			continue;
		}
		CHECK(value < bench->items);
		if (bench->dropping) {
			CHECK(endpoint->completed == 0 || value > previous);
			previous = value;
		} else {
			CHECK(value == endpoint->completed);
		}
		endpoint->completed++;
	}
	finish(endpoint);
	return NULL;
}

static void run(struct benchmark *bench, uint32_t items, int producer_cpu, int consumer_cpu)
{
	pthread_t producer, consumer;

	pwao_queue_ring_reset(&bench->ring);
	atomic_store_explicit(&bench->done, false, memory_order_relaxed);
	bench->producer = (struct endpoint){ .samples = bench->producer.samples };
	bench->consumer = (struct endpoint){ .samples = bench->consumer.samples };
	bench->producer.cpu = producer_cpu;
	bench->consumer.cpu = consumer_cpu;
	bench->items = items;
	CHECK(pthread_barrier_init(&bench->start, NULL, 2) == 0);
	CHECK(pthread_create(&producer, NULL, produce, bench) == 0);
	CHECK(pthread_create(&consumer, NULL, consume, bench) == 0);
	CHECK(pthread_join(producer, NULL) == 0);
	CHECK(pthread_join(consumer, NULL) == 0);
	CHECK(pthread_barrier_destroy(&bench->start) == 0);
	CHECK(bench->producer.completed == items);
	CHECK(bench->producer.dropped + bench->consumer.completed == items);
}

static int compare_samples(const void *a, const void *b)
{
	const uint64_t first = *(const uint64_t *)a, second = *(const uint64_t *)b;

	return (first > second) - (first < second);
}

static void print_endpoint(const char *name, struct endpoint *endpoint)
{
	const uint32_t count = endpoint->count;

	CHECK(count > 0 && count == (endpoint->attempts - 1u) / SAMPLE_EVERY + 1u);
	qsort(endpoint->samples, count, sizeof(endpoint->samples[0]), compare_samples);
	printf(",%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%.3f,%" PRIu32
			",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
			name, endpoint->attempts, endpoint->completed, endpoint->dropped,
			endpoint->cpu_ns / 1e6, count,
			endpoint->samples[(count - 1u) * 50u / 100u],
			endpoint->samples[(count - 1u) * 90u / 100u],
			endpoint->samples[(count - 1u) * 99u / 100u],
			endpoint->samples[(count - 1u) * 999u / 1000u],
			endpoint->samples[count - 1u]);
}

static int number(const char *arg, int limit)
{
	char *end;
	long value = strtol(arg, &end, 10);

	CHECK(end != arg && *end == '\0' && value >= 0 && value < limit);
	return value;
}

int main(int argc, char *argv[])
{
	struct benchmark bench;
	uint32_t depth;
	int producer_cpu, consumer_cpu;
	uint64_t begin, end, clock_samples[1024];

	if (argc != 6) {
		fprintf(stderr, "usage: %s backpressure|drop-oldest copy|lease depth producer-cpu consumer-cpu\n",
				argv[0]);
		return EXIT_FAILURE;
	}
	bench.dropping = strcmp(argv[1], "drop-oldest") == 0;
	bench.lease = strcmp(argv[2], "lease") == 0;
	CHECK(bench.dropping || strcmp(argv[1], "backpressure") == 0);
	CHECK(bench.lease || strcmp(argv[2], "copy") == 0);
	depth = number(argv[3], PWAO_QUEUE_RING_MAX_SLOTS + 1u);
	producer_cpu = number(argv[4], CPU_SETSIZE);
	consumer_cpu = number(argv[5], CPU_SETSIZE);
	CHECK(producer_cpu != consumer_cpu);
	CHECK(pwao_queue_ring_init(&bench.ring, depth
#ifndef PWAO_QUEUE_BENCH_BASELINE
			, bench.dropping ? PWAO_QUEUE_OVERFLOW_DROP_OLDEST :
			PWAO_QUEUE_OVERFLOW_BACKPRESSURE
#endif
			) == 0);
	bench.producer.samples = calloc(SAMPLE_CAPACITY, sizeof(uint64_t));
	bench.consumer.samples = calloc(SAMPLE_CAPACITY, sizeof(uint64_t));
	CHECK(bench.producer.samples && bench.consumer.samples);
	for (size_t i = 0; i < SAMPLE_CAPACITY; i += 512) {
		bench.producer.samples[i] = 1;
		bench.consumer.samples[i] = 1;
	}
	atomic_init(&bench.done, false);
	run(&bench, WARMUP_ITEMS, producer_cpu, consumer_cpu);
	run(&bench, TIMED_ITEMS, producer_cpu, consumer_cpu);
	for (size_t i = 0; i < sizeof(clock_samples) / sizeof(clock_samples[0]); i++) {
		const uint64_t before = now(CLOCK_MONOTONIC_RAW);

		clock_samples[i] = now(CLOCK_MONOTONIC_RAW) - before;
	}
	qsort(clock_samples, 1024, sizeof(clock_samples[0]), compare_samples);
	begin = SPA_MIN(bench.producer.start_ns, bench.consumer.start_ns);
	end = SPA_MAX(bench.producer.end_ns, bench.consumer.end_ns);
	printf("%s,%s,%" PRIu32 ",%d,%d,%.3f,%" PRIu64,
			argv[1], argv[2], depth, producer_cpu, consumer_cpu,
			(end - begin) / 1e6, clock_samples[511]);
	print_endpoint("producer", &bench.producer);
	print_endpoint("consumer", &bench.consumer);
	putchar('\n');
	free(bench.producer.samples);
	free(bench.consumer.samples);
	return EXIT_SUCCESS;
}
