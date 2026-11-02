/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/type-info.h>

#define CHECK(expr) spa_assert_se(expr)

static void test_acquisition_facts(void)
{
	uint64_t storage[64], decoded;
	struct spa_meta meta = { SPA_META_Properties, sizeof(storage), storage };
	struct spa_acquisition_identity identity, same;
	struct spa_exposure_start start, peer;
	const uint8_t domain[16] = { 1 }, grandmaster[8] = { 2 };
	uint32_t timebase = SPA_EXPOSURE_TIMEBASE_TAI, ptp_domain = 255;
	int64_t nsec = INT64_MAX;
	int64_t difference;
	uint64_t uncertainty;
	uint32_t mandatory = SPA_POD_PROP_FLAG_MANDATORY;

	CHECK(spa_meta_properties_init(&meta));
	CHECK(!spa_meta_properties_get_acquisition_identity(&meta, &identity));
	CHECK(!spa_meta_properties_add(&meta, 3, 0, SPA_TYPE_Bytes, 16, domain));
	CHECK(spa_meta_properties_add(&meta, 3, mandatory, SPA_TYPE_Bytes, 16, domain));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_add_uint64(&meta, 4, mandatory, UINT64_MAX));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_add_uint64(&meta, 5, mandatory, UINT64_MAX));
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(!spa_meta_properties_add_uint64(&meta, 6, mandatory, 0));
	CHECK(spa_meta_properties_add_uint64(&meta, 6, mandatory, UINT64_MAX));
	CHECK(spa_meta_properties_add(&meta, 7, mandatory, SPA_TYPE_Long, 8, &nsec));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_add(&meta, 8, mandatory, SPA_TYPE_Id, 4, &timebase));
	CHECK(spa_meta_properties_add_uint64(&meta, 9, mandatory, UINT64_MAX));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_add(&meta, 10, mandatory, SPA_TYPE_Bytes, 8, grandmaster));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_add(&meta, 11, mandatory, SPA_TYPE_Id, 4, &ptp_domain));
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	{
		struct spa_meta_property_requirement required = { 19, SPA_TYPE_Id };
		uint32_t custom = 0;
		CHECK(spa_meta_properties_add(&meta, 19, mandatory, SPA_TYPE_Id, 4, &custom));
		CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
		CHECK(spa_meta_properties_is_valid(&meta, &required, 1));
	}
	CHECK(spa_meta_properties_get_uint64(&meta, 4, &decoded) && decoded == UINT64_MAX);
	CHECK(spa_meta_properties_get_acquisition_identity(&meta, &identity));
	CHECK(identity.generation == UINT64_MAX && identity.sequence == UINT64_MAX);
	same = identity;
	CHECK(spa_acquisition_identity_equal(&identity, &same));
	same.sequence = 0;
	CHECK(!spa_acquisition_identity_equal(&identity, &same));
	CHECK(spa_meta_properties_get_exposure_duration(&meta, &decoded) && decoded == UINT64_MAX);
	CHECK(spa_meta_properties_get_exposure_start(&meta, &start));
	CHECK(start.nsec == INT64_MAX && start.uncertainty_nsec == UINT64_MAX);
	peer = start;
	peer.nsec = 0;
	CHECK(spa_exposure_start_time_difference(&start, &peer, &difference, &uncertainty));
	CHECK(difference == INT64_MAX && uncertainty == UINT64_MAX);
	CHECK(spa_exposure_start_times_match(&start, &peer, UINT64_MAX));
	start.uncertainty_nsec = peer.uncertainty_nsec = 0;
	CHECK(!spa_exposure_start_times_match(&start, &peer, INT64_MAX - 1u));
	CHECK(spa_exposure_start_times_match(&start, &peer, INT64_MAX));
	peer.ptp_domain_number--;
	CHECK(!spa_exposure_start_time_difference(&start, &peer, NULL, NULL));
	peer = start;
	peer.timebase = SPA_EXPOSURE_TIMEBASE_MONOTONIC;
	CHECK(!spa_exposure_start_time_difference(&start, &peer, NULL, NULL));
	CHECK(spa_meta_properties_init(&meta));
	CHECK(!spa_meta_properties_get_acquisition_identity(&meta, &identity));
	CHECK(!spa_meta_properties_get_exposure_start(&meta, &start));
	CHECK(!spa_meta_properties_get_exposure_duration(&meta, &decoded));
	puts("acquisition facts: complete groups, full unsigned range, clocks and reuse passed");
}

static void test_fact_ranges(void)
{
	uint64_t storage[64], value;
	struct spa_meta meta = { SPA_META_Properties, sizeof(storage), storage };
	const uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY;
	uint8_t zero[16] = { 0 }, bytes[8] = { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
	const uint64_t values[] = { 0, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX };
	int64_t negative = -1, start = 0;
	uint32_t invalid = 256, monotonic = SPA_EXPOSURE_TIMEBASE_MONOTONIC;
	CHECK(spa_meta_properties_init(&meta));
	CHECK(!spa_meta_properties_add(&meta, 3, flags, SPA_TYPE_Bytes, 16, zero));
	CHECK(!spa_meta_properties_add(&meta, 4, flags, SPA_TYPE_Bytes, 7, bytes));
	CHECK(!spa_meta_properties_add(&meta, 4, flags, SPA_TYPE_Long, 8, bytes));
	CHECK(!spa_meta_properties_add(&meta, 7, flags, SPA_TYPE_Long, 8, &negative));
	CHECK(!spa_meta_properties_add(&meta, 8, flags, SPA_TYPE_Id, 4, &invalid));
	CHECK(!spa_meta_properties_add(&meta, 10, flags, SPA_TYPE_Bytes, 8, zero));
	CHECK(!spa_meta_properties_add(&meta, 11, flags, SPA_TYPE_Id, 4, &invalid));
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(values); i++) {
		CHECK(spa_meta_properties_init(&meta));
		CHECK(spa_meta_properties_add_uint64(&meta, 17, 0, values[i]));
		CHECK(spa_meta_properties_get_uint64(&meta, 17, &value) && value == values[i]);
	}
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_add_uint64(&meta, 17, 0, UINT64_C(0x0102030405060708)));
	CHECK(memcmp(SPA_POD_BODY_CONST(spa_meta_properties_find(&meta, 17)), bytes, 8) == 0);
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_add(&meta, 7, flags, SPA_TYPE_Long, 8, &start));
	CHECK(spa_meta_properties_add(&meta, 8, flags, SPA_TYPE_Id, 4, &monotonic));
	CHECK(spa_meta_properties_add_uint64(&meta, 9, flags, 0));
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	bytes[0] = 1;
	CHECK(spa_meta_properties_add(&meta, 10, flags, SPA_TYPE_Bytes, 8, bytes));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_add(&meta, 10, flags, SPA_TYPE_Bytes, 8, bytes));
	CHECK(!spa_meta_properties_is_valid(&meta, NULL, 0));
	puts("fact ranges: unsigned boundaries/little-endian bytes, exact types and clock groups passed");
}

int main(void)
{
	uint64_t storage[512], saved[512];
	struct spa_meta meta = { SPA_META_Properties, sizeof(storage), storage };
	const struct spa_meta_property_requirement required[] = {
		{ SPA_META_PROPERTY_opticalClassification, SPA_TYPE_Id },
		{ 17, SPA_TYPE_Long },
	};
	struct spa_meta_property_requirement all_required[64];
	struct spa_meta_property_requirement invalid[2] = { required[0], required[0] };
	struct spa_pod_object *object = (void *)storage;
	struct spa_pod_prop *optical = SPA_PTROFF(storage, 16, struct spa_pod_prop);
	struct spa_pod_prop *unknown = SPA_PTROFF(storage, 40, struct spa_pod_prop);
	const struct spa_pod *value;
	uint32_t classification = SPA_META_OPTICAL_FRINGED, bad = 4;
	int64_t number = INT64_MIN;
	uint32_t cases = 0;

	memset(storage, 0xff, sizeof(storage));
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(!spa_meta_properties_is_valid(&meta, required, 1));
	CHECK(spa_meta_properties_find(&meta, 1) == NULL);
	CHECK(!spa_meta_properties_add(&meta, 1, 0, SPA_TYPE_Id, sizeof(bad), &bad));
	CHECK(!spa_meta_properties_add(&meta, 1, 0, SPA_TYPE_Int, sizeof(classification), &classification));
	CHECK(spa_meta_properties_add(&meta, 1, 0, SPA_TYPE_Id, sizeof(classification), &classification));
	CHECK(!spa_meta_properties_add(&meta, 1, 0, SPA_TYPE_Id, sizeof(classification), &classification));
	CHECK(spa_meta_properties_add(&meta, 17, 0, SPA_TYPE_Long, sizeof(number), &number));
	CHECK(spa_meta_properties_add(&meta, 18, 0, SPA_TYPE_String, 4, "abc"));
	CHECK(spa_meta_properties_add(&meta, 19, 0, SPA_TYPE_Bytes, 0, NULL));
	CHECK(spa_meta_properties_is_valid(&meta, required, 2));
	CHECK(!spa_meta_properties_is_valid(&meta, invalid, 2));
	invalid[1] = (struct spa_meta_property_requirement){17, SPA_TYPE_Double};
	CHECK(!spa_meta_properties_is_valid(&meta, invalid, 2));
	value = spa_meta_properties_find(&meta, 17);
	CHECK(value && value->type == SPA_TYPE_Long && value->size == 8);
	CHECK(memcmp(SPA_POD_BODY_CONST(value), &number, sizeof(number)) == 0);
	CHECK(spa_type_buffer_properties[0].type == 1);
	CHECK(strcmp(spa_type_to_name(SPA_META_Properties, spa_type_meta_type, ""), "Spa:Pointer:Meta:Properties") == 0);
	memcpy(saved, storage, sizeof(storage));

#define CORRUPT(statement) do { statement; CHECK(!spa_meta_properties_is_valid(&meta, required, 2)); \
	memcpy(storage, saved, sizeof(storage)); cases++; } while (0)
	CORRUPT(object->pod.size = UINT32_MAX);
	CORRUPT(object->pod.size = 7);
	CORRUPT(object->pod.size--);
	CORRUPT(object->pod.type = SPA_TYPE_Struct);
	CORRUPT(object->body.type++);
	CORRUPT(object->body.id++);
	CORRUPT(optical->value.size = UINT32_MAX);
	CORRUPT(optical->value.size = 8);
	CORRUPT(optical->value.type = SPA_TYPE_Int);
	CORRUPT(*(uint32_t *)SPA_POD_BODY(&optical->value) = 4);
	CORRUPT(optical->key = 0);
	CORRUPT(optical->flags = SPA_POD_PROP_FLAG_READONLY);
	CORRUPT(((uint8_t *)storage)[36] = 1); /* property padding */
	CORRUPT(((uint8_t *)storage)[4095] = 1); /* stale reused-allocation bytes */
	CORRUPT(unknown->key = 1);
	CORRUPT(unknown->value.type = SPA_TYPE_Pointer);
	CORRUPT(unknown->value.type = SPA_TYPE_Fd);
	CORRUPT(unknown->value.type = SPA_TYPE_Object);
	unknown->flags = SPA_POD_PROP_FLAG_MANDATORY;
	CHECK(spa_meta_properties_is_valid(&meta, required, 2));
	CHECK(!spa_meta_properties_is_valid(&meta, required, 1));
	memcpy(storage, saved, sizeof(storage));
	for (uint32_t size = 0; size < SPA_POD_SIZE(&object->pod); size++) {
		struct spa_meta short_meta = meta;
		short_meta.size = size;
		CHECK(!spa_meta_properties_is_valid(&short_meta, required, 2));
		cases++;
	}
	for (uint32_t alignment = 1; alignment < 8; alignment++) {
		struct spa_meta misaligned = meta;
		misaligned.data = SPA_PTROFF(storage, alignment, void);
		CHECK(!spa_meta_properties_is_valid(&misaligned, NULL, 0));
		CHECK(!spa_meta_properties_init(&misaligned));
		cases++;
	}
	CHECK(spa_meta_properties_init(&meta));
	for (uint32_t key = 16; key <= 79; key++) {
		all_required[key - 16] = (struct spa_meta_property_requirement){key, SPA_TYPE_Id};
		CHECK(spa_meta_properties_add(&meta, key, 0, SPA_TYPE_Id, 4, &classification));
	}
	CHECK(spa_meta_properties_is_valid(&meta, all_required, 64));
	CHECK(!spa_meta_properties_add(&meta, 80, 0, SPA_TYPE_Id, 4, &classification));
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_find(&meta, 17) == NULL);
	CHECK(!spa_meta_properties_add(&meta, 2, 0, SPA_TYPE_Bool, 4, &bad));
	CHECK(!spa_meta_properties_add(&meta, 2, 0, SPA_TYPE_String, 3, "abc"));
	meta.size = 16;
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(!spa_meta_properties_add(&meta, 2, 0, SPA_TYPE_Id, 4, &classification));
	test_acquisition_facts();
	test_fact_ranges();
	printf("buffer properties: scalar lookup, requirements, reset, limits and %u malformed cases passed\n", cases);
	return 0;
}
