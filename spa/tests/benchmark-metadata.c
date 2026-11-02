/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */
/* Standalone comparison: see acquisition-metadata.md. Baseline mode is built
 * only against the frozen native 2.0 header, never into a runtime product. */
#include <assert.h>
#include <inttypes.h>
#include <spa/buffer/meta.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 10000u
static bool measuring;
static uint64_t allocations;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n)
{
	if (measuring)
		allocations++;
	return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s)
{
	if (measuring)
		allocations++;
	return __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n)
{
	if (measuring)
		allocations++;
	return __real_realloc(p, n);
}
static uint64_t ns(void)
{
	struct timespec t;
	assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
	return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
static int order(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}
static volatile uint64_t checksum;
#ifdef PIPEWIREAO_METADATA_BASELINE
static uint64_t source_bytes[64], destination_bytes[64];
static struct spa_meta source = {SPA_META_Properties, sizeof(source_bytes), source_bytes};
static struct spa_meta destination = {SPA_META_Properties, sizeof(destination_bytes),
				      destination_bytes};
static struct spa_meta_header source_header, destination_header;
static void initialize(bool maximum)
{
	assert(!maximum);
	uint8_t domain[16] = {1};
	uint32_t optical = 2, trigger = 2, timebase = 1, flags = SPA_POD_PROP_FLAG_MANDATORY;
	int64_t start = 1000, optional = 17;
	assert(spa_meta_properties_init(&source));
	assert(spa_meta_properties_add(&source, 1, 0, SPA_TYPE_Id, 4, &optical));
	assert(spa_meta_properties_add(&source, 2, 0, SPA_TYPE_Id, 4, &trigger));
	assert(spa_meta_properties_add(&source, 3, flags, SPA_TYPE_Bytes, 16, domain));
	assert(spa_meta_properties_add_uint64(&source, 4, flags, UINT64_MAX));
	assert(spa_meta_properties_add_uint64(&source, 5, flags, UINT64_MAX));
	assert(spa_meta_properties_add_uint64(&source, 6, flags, 100));
	assert(spa_meta_properties_add(&source, 7, flags, SPA_TYPE_Long, 8, &start));
	assert(spa_meta_properties_add(&source, 8, flags, SPA_TYPE_Id, 4, &timebase));
	assert(spa_meta_properties_add_uint64(&source, 9, flags, 50));
	assert(spa_meta_properties_add(&source, 17, 0, SPA_TYPE_Long, 8, &optional));
	assert(spa_meta_properties_is_valid(&source, NULL, 0));
}
__attribute__((noinline)) static void operation(uint64_t iteration)
{
	source_header.seq = iteration;
	assert(spa_meta_properties_is_valid(&source, NULL, 0));
	assert(spa_meta_properties_init(&destination));
	memcpy(destination.data, source.data, source.size);
	destination_header = source_header;
	assert(spa_meta_properties_is_valid(&destination, NULL, 0));
	checksum += destination_header.seq + ((uint64_t *)destination.data)[1];
}
#else
static uint64_t source_bytes[512], destination_bytes[512];
static struct spa_meta source[64], destination[64];
static uint32_t count;
static void initialize(bool maximum)
{
	count = maximum ? 64 : 6;
	uint32_t offset = 0;
	uint32_t types[] = {
	    SPA_META_Header,	      SPA_META_AcquisitionIdentity, SPA_META_ExposureTiming,
	    SPA_META_OpticalChopping, SPA_META_ElectricalTrigger,   0x3f0};
	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = maximum ? (i == 0 ? SPA_META_Header : 0x400 + i) : types[i];
		uint32_t size = maximum ? (i == 0    ? 32
					   : i == 63 ? 96
						     : 64)
					: spa_meta_record_size(type);
		if (!maximum && i == 5)
			size = 8;
		source[i] = (struct spa_meta){type, size, (uint8_t *)source_bytes + offset};
		destination[i] =
		    (struct spa_meta){type, size, (uint8_t *)destination_bytes + offset};
		offset += size;
	}
	if (maximum) {
		assert(offset == 4096);
	} else {
		*(struct spa_meta_acquisition_identity *)source[1].data =
		    (struct spa_meta_acquisition_identity){.flags = SPA_META_RECORD_VALID,
							   .domain = {1},
							   .generation = UINT64_MAX,
							   .sequence = UINT64_MAX};
		*(struct spa_meta_exposure_timing *)source[2].data =
		    (struct spa_meta_exposure_timing){.flags = 7,
						      .clock = SPA_META_CLOCK_MONOTONIC,
						      .clock_domain = {1},
						      .start_nsec = 1000,
						      .duration_nsec = 100,
						      .uncertainty_nsec = 50};
		*(struct spa_meta_optical_chopping *)source[3].data =
		    (struct spa_meta_optical_chopping){SPA_META_RECORD_VALID,
						       SPA_META_OPTICAL_FRINGED};
		*(struct spa_meta_electrical_trigger *)source[4].data =
		    (struct spa_meta_electrical_trigger){SPA_META_RECORD_VALID,
							 SPA_META_TRIGGER_HIGH};
		*(uint64_t *)source[5].data = 17;
	}
	assert(spa_meta_records_validate(source, count, NULL, 0));
}
__attribute__((noinline)) static void operation(uint64_t iteration)
{
	((struct spa_meta_header *)source[0].data)->seq = iteration;
	assert(spa_meta_records_validate(source, count, NULL, 0));
	assert(spa_meta_records_copy(destination, count, source, count));
	assert(spa_meta_records_validate(destination, count, NULL, 0));
	checksum += ((struct spa_meta_header *)destination[0].data)->seq +
		    *(uint64_t *)destination[count - 1].data;
}
#endif
int main(int argc, char **argv)
{
	bool maximum = argc == 2 && strcmp(argv[1], "--maximum") == 0;
	assert(argc == 1 || maximum);
	initialize(maximum);
	for (uint32_t i = 0; i < SAMPLES; i++)
		operation(i);
	uint64_t times[SAMPLES], clock_times[SAMPLES];
	for (uint32_t i = 0; i < SAMPLES; i++) {
		uint64_t t = ns();
		clock_times[i] = ns() - t;
	}
	measuring = true;
	for (uint32_t i = 0; i < SAMPLES; i++) {
		uint64_t t = ns();
		operation(i);
		times[i] = ns() - t;
	}
	measuring = false;
	assert(allocations == 0);
	qsort(times, SAMPLES, sizeof(times[0]), order);
	qsort(clock_times, SAMPLES, sizeof(clock_times[0]), order);
	printf("samples=%u maximum=%u p50_ns=%" PRIu64 " p90_ns=%" PRIu64 " p99_ns=%" PRIu64
	       " max_ns=%" PRIu64 " clock_p50_ns=%" PRIu64 " allocations=%" PRIu64
	       " checksum=%" PRIu64 "\n",
	       SAMPLES, maximum, times[SAMPLES / 2], times[SAMPLES * 9 / 10],
	       times[SAMPLES * 99 / 100], times[SAMPLES - 1], clock_times[SAMPLES / 2], allocations,
	       checksum);
	return 0;
}
