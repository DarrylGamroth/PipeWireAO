/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <limits.h>
#include <spa/buffer/meta.h>
#include <stdio.h>

int main(void)
{
	struct spa_meta_acquisition_identity id = {0}, saved_id;
	struct spa_meta_exposure_timing exposure = {0};
	struct spa_meta_absolute_time absolute = {0};
	struct spa_meta_optical_chopping optical = {0};
	struct spa_meta_electrical_trigger trigger = {0};
	struct spa_meta_header header = {0};
	uint64_t opaque[3] = {UINT64_MAX, 0x123456789abcdef0u, 17};
	struct spa_meta source[] = {
	    {SPA_META_Header, sizeof(header), &header},
	    {SPA_META_AcquisitionIdentity, sizeof(id), &id},
	    {SPA_META_ExposureTiming, sizeof(exposure), &exposure},
	    {SPA_META_AbsoluteTime, sizeof(absolute), &absolute},
	    {SPA_META_OpticalChopping, sizeof(optical), &optical},
	    {SPA_META_ElectricalTrigger, sizeof(trigger), &trigger},
	    {0x3f0, sizeof(opaque), opaque},
	};
	struct spa_meta_requirement required = {SPA_META_AcquisitionIdentity, sizeof(id),
						SPA_META_REQUIREMENT_REQUIRED, 0};
	assert(spa_meta_records_validate(source, 7, NULL, 0));
	assert(!spa_meta_records_validate(source, 7, &required, 1));
	id = (struct spa_meta_acquisition_identity){.flags = SPA_META_RECORD_VALID,
						    .domain = {1},
						    .generation = UINT64_MAX,
						    .sequence = UINT64_MAX};
	saved_id = id;
	assert(spa_meta_records_validate(source, 7, &required, 1));
	assert(spa_acquisition_identity_equal(&id, &saved_id));
	header.seq = 1;
	header.pts = 17;
	assert(spa_acquisition_identity_equal(&id, &saved_id));
	saved_id.sequence--;
	assert(!spa_acquisition_identity_equal(&id, &saved_id));
	saved_id = id;
	saved_id.generation--;
	assert(!spa_acquisition_identity_equal(&id, &saved_id));
	saved_id = id;
	saved_id.domain[1] = 1;
	assert(!spa_acquisition_identity_equal(&id, &saved_id));
	id.reserved = 1;
	assert(!spa_meta_record_validate(&source[1], false));
	id.reserved = 0;
	exposure.flags = SPA_META_EXPOSURE_DURATION_VALID;
	exposure.duration_nsec = UINT64_MAX;
	assert(spa_meta_record_validate(&source[2], true));
	exposure.duration_nsec = 0;
	assert(!spa_meta_record_validate(&source[2], true));
	exposure.duration_nsec = 10;
	exposure.flags |= SPA_META_EXPOSURE_START_VALID;
	exposure.clock = SPA_META_CLOCK_MODEL;
	exposure.clock_domain[0] = 1;
	exposure.start_nsec = INT64_MIN;
	assert(spa_meta_record_validate(&source[2], true));
	exposure.mapping[0] = 1;
	assert(!spa_meta_record_validate(&source[2], true));
	exposure.flags |= SPA_META_EXPOSURE_MAPPED;
	assert(spa_meta_record_validate(&source[2], true));
	for (unsigned clock = SPA_META_CLOCK_REALTIME; clock <= SPA_META_CLOCK_TAI; clock++) {
		absolute.count = 1;
		absolute.events[0] =
		    (struct spa_meta_absolute_time_event){.seconds = INT64_MIN,
							  .nanoseconds = 999999999,
							  .clock = clock,
							  .event = SPA_META_TIME_EVENT_PRODUCTION,
							  .flags = SPA_META_RECORD_VALID,
							  .origin = {1},
							  .clock_domain = {2}};
		assert(spa_meta_record_validate(&source[3], true));
		absolute.events[0].seconds = INT64_MAX;
		assert(spa_meta_record_validate(&source[3], true));
		absolute.events[0].nanoseconds = 1000000000;
		assert(!spa_meta_record_validate(&source[3], false));
		absolute.events[0].nanoseconds = 0;
		absolute.events[0].uncertainty_nsec = UINT64_MAX;
		assert(!spa_meta_record_validate(&source[3], false));
		absolute.events[0].flags |= SPA_META_RECORD_UNCERTAINTY;
		assert(spa_meta_record_validate(&source[3], true));
	}
	absolute.events[1] = absolute.events[0];
	assert(!spa_meta_record_validate(&source[3], false));
	absolute.count = 2;
	assert(!spa_meta_record_validate(&source[3], false));
	absolute.events[1].event = SPA_META_TIME_EVENT_ACQUISITION;
	assert(spa_meta_record_validate(&source[3], true));
	for (unsigned v = 0; v < 4; v++) {
		optical.flags = trigger.flags = SPA_META_RECORD_VALID;
		optical.classification = trigger.classification = v;
		assert(spa_meta_record_validate(&source[4], true));
		assert(spa_meta_record_validate(&source[5], true));
	}
	optical.classification = 4;
	assert(!spa_meta_record_validate(&source[4], false));
	optical.classification = 0;
	optical.flags = 0;
	assert(spa_meta_record_validate(&source[4], false));
	assert(!spa_meta_record_validate(&source[4], true));
	uint64_t destination_bytes[4096 / 8];
	memset(destination_bytes, 0xa5, sizeof(destination_bytes));
	struct spa_meta destination[7];
	uint32_t offset = 0;
	for (unsigned i = 0; i < 7; i++) {
		destination[i] = source[i];
		destination[i].data = (uint8_t *)destination_bytes + offset;
		offset += SPA_ROUND_UP_N(source[i].size, 8);
	}
	assert(spa_meta_records_copy(destination, 7, source, 7));
	for (unsigned i = 0; i < 7; i++)
		assert(memcmp(destination[i].data, source[i].data, source[i].size) == 0);
	assert(spa_meta_records_copy(source, 7, source, 7));
	uint64_t before[4096 / 8];
	memcpy(before, destination_bytes, sizeof(before));
	destination[6].size--;
	assert(!spa_meta_records_copy(destination, 7, source, 7));
	assert(memcmp(before, destination_bytes, sizeof(before)) == 0);
	destination[6].size++;
	destination[6].type = 13;
	assert(!spa_meta_records_copy(destination, 7, source, 7));
	assert(memcmp(before, destination_bytes, sizeof(before)) == 0);
	destination[6].type = 0x3f0;
	destination[6].data = source[6].data;
	assert(!spa_meta_records_copy(destination, 7, source, 7));
	assert(memcmp(before, destination_bytes, sizeof(before)) == 0);
	struct spa_meta many[65];
	uint64_t byte = 0;
	for (unsigned i = 0; i < 65; i++)
		many[i] = (struct spa_meta){0x400 + i, 0, NULL};
	assert(spa_meta_records_validate(many, 64, NULL, 0));
	assert(!spa_meta_records_validate(many, 65, NULL, 0));
	many[0].size = 4096;
	many[0].data = destination_bytes;
	assert(spa_meta_records_layout_validate(many, 64));
	many[1].size = 1;
	many[1].data = &byte;
	assert(!spa_meta_records_layout_validate(many, 64));
	for (unsigned retired = 11; retired <= 13; retired++) {
		many[0] = (struct spa_meta){retired, 0, NULL};
		assert(!spa_meta_record_init(&many[0]));
		assert(!spa_meta_record_validate(&many[0], false));
	}
	for (unsigned i = 0; i < 7; i++)
		assert(spa_meta_record_init(&source[i]));
	assert(!spa_meta_records_validate(source, 7, &required, 1));
	assert(spa_meta_records_validate(source, 7, NULL, 0));
	/* Pool-local Busy counters and synchronization state are never cloned or cleared. */
	_Alignas(8) struct spa_meta_busy source_busy = {.flags = 1, .count = 3};
	_Alignas(8) struct spa_meta_busy destination_busy = {.flags = 2, .count = 7};
	struct spa_meta_sync_timeline source_sync = {
	    .flags = 1, .acquire_point = 11, .release_point = 12};
	struct spa_meta_sync_timeline destination_sync = {
	    .flags = 2, .acquire_point = 21, .release_point = 22};
	uint64_t source_fact = UINT64_MAX, destination_fact = 0;
	struct spa_meta source_state[] = {
	    {SPA_META_Busy, sizeof(source_busy), &source_busy},
	    {SPA_META_SyncTimeline, sizeof(source_sync), &source_sync},
	    {0x3f0, sizeof(source_fact), &source_fact},
	};
	struct spa_meta destination_state[] = {
	    {SPA_META_Busy, sizeof(destination_busy), &destination_busy},
	    {SPA_META_SyncTimeline, sizeof(destination_sync), &destination_sync},
	    {0x3f0, sizeof(destination_fact), &destination_fact},
	};
	assert(spa_meta_type_is_transport_owned(SPA_META_Busy));
	assert(spa_meta_type_is_transport_owned(SPA_META_SyncTimeline));
	assert(spa_meta_type_is_transport_owned(SPA_META_TYPE_FEATURES(SPA_META_SyncTimeline, 1)));
	assert(!spa_meta_type_is_transport_owned(SPA_META_AcquisitionIdentity));
	assert(!spa_meta_type_is_transport_owned(0x3f0));
	for (unsigned i = 0; i < 3; i++)
		assert(spa_meta_record_init(&destination_state[i]));
	assert(destination_busy.count == 7 && destination_busy.flags == 2);
	assert(destination_sync.acquire_point == 21 && destination_sync.release_point == 22 &&
	       destination_sync.flags == 2);
	assert(spa_meta_records_copy(destination_state, 3, source_state, 3));
	assert(destination_fact == UINT64_MAX);
	assert(destination_busy.count == 7 && source_busy.count == 3);
	assert(destination_sync.acquire_point == 21 && destination_sync.release_point == 22 &&
	       source_sync.acquire_point == 11);
	assert(spa_meta_records_copy(&destination_state[2], 1, source_state, 3));
	struct spa_meta feature = {SPA_META_TYPE_FEATURES(0x3f0, 1), 8, &source_fact};
	assert(!spa_meta_record_validate(&feature, false));
	feature.size = 0;
	assert(spa_meta_record_validate(&feature, false));
	feature.type = SPA_META_TYPE_FEATURES(13, 1);
	assert(!spa_meta_record_validate(&feature, false));
	assert(spa_meta_record_layout_validate(SPA_META_Bitmap, 8192, INT32_MAX));
	assert(!spa_meta_record_layout_validate(SPA_META_Bitmap, 8192, SPA_META_MAX_BYTES));
	assert(!spa_meta_record_layout_validate(SPA_META_Invalid, 0, INT32_MAX));
	puts("independent metadata: full-width identity, clocks, validity, bounds, opaque copy and "
	     "atomic rejection passed");
	return 0;
}
