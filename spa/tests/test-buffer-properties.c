/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/type-info.h>

#define CHECK(expr) spa_assert_se(expr)

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
	CHECK(!spa_meta_properties_add(&meta, 1, SPA_TYPE_Id, sizeof(bad), &bad));
	CHECK(!spa_meta_properties_add(&meta, 1, SPA_TYPE_Int, sizeof(classification), &classification));
	CHECK(spa_meta_properties_add(&meta, 1, SPA_TYPE_Id, sizeof(classification), &classification));
	CHECK(!spa_meta_properties_add(&meta, 1, SPA_TYPE_Id, sizeof(classification), &classification));
	CHECK(spa_meta_properties_add(&meta, 17, SPA_TYPE_Long, sizeof(number), &number));
	CHECK(spa_meta_properties_add(&meta, 18, SPA_TYPE_String, 4, "abc"));
	CHECK(spa_meta_properties_add(&meta, 19, SPA_TYPE_Bytes, 0, NULL));
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
	for (uint32_t key = 2; key <= 65; key++) {
		all_required[key - 2] = (struct spa_meta_property_requirement){key, SPA_TYPE_Id};
		CHECK(spa_meta_properties_add(&meta, key, SPA_TYPE_Id, 4, &classification));
	}
	CHECK(spa_meta_properties_is_valid(&meta, all_required, 64));
	CHECK(!spa_meta_properties_add(&meta, 66, SPA_TYPE_Id, 4, &classification));
	CHECK(spa_meta_properties_init(&meta));
	CHECK(spa_meta_properties_find(&meta, 17) == NULL);
	CHECK(!spa_meta_properties_add(&meta, 2, SPA_TYPE_Bool, 4, &bad));
	CHECK(!spa_meta_properties_add(&meta, 2, SPA_TYPE_String, 3, "abc"));
	meta.size = 16;
	CHECK(spa_meta_properties_is_valid(&meta, NULL, 0));
	CHECK(!spa_meta_properties_add(&meta, 2, SPA_TYPE_Id, 4, &classification));
	printf("buffer properties: scalar lookup, requirements, reset, limits and %u malformed cases passed\n", cases);
	return 0;
}
