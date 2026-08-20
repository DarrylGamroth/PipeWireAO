/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <dlfcn.h>

#include <spa/buffer/alloc.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/buffer/type-info.h>
#include <spa/debug/types.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

PWTEST(buffer_abi_types)
{
	/* buffer */
	pwtest_int_eq(SPA_DATA_Invalid, 0);
	pwtest_int_eq(SPA_DATA_MemPtr, 1);
	pwtest_int_eq(SPA_DATA_MemFd, 2);
	pwtest_int_eq(SPA_DATA_DmaBuf, 3);
	pwtest_int_eq(SPA_DATA_MemId, 4);
	pwtest_int_eq(SPA_DATA_SyncObj, 5);
	pwtest_int_eq(_SPA_DATA_LAST, 6);
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_PAGES, 1U << 4);
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_2MB, 1U << 5);
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_1GB, 1U << 6);

	/* buffer allocation hints */
	pwtest_int_eq(SPA_PARAM_BUFFERS_pageSizeHint, 8);
	pwtest_int_eq(SPA_BUFFER_PAGE_SIZE_NORMAL, 0);
	pwtest_int_eq(SPA_BUFFER_PAGE_SIZE_HUGE_DEFAULT, 1);
	pwtest_int_eq(SPA_BUFFER_PAGE_SIZE_HUGE_2MB, 2);
	pwtest_int_eq(SPA_BUFFER_PAGE_SIZE_HUGE_1GB, 3);

	/* meta */
	pwtest_int_eq(SPA_META_Invalid, 0);
	pwtest_int_eq(SPA_META_Header, 1);
	pwtest_int_eq(SPA_META_VideoCrop, 2);
	pwtest_int_eq(SPA_META_VideoDamage, 3);
	pwtest_int_eq(SPA_META_Bitmap, 4);
	pwtest_int_eq(SPA_META_Cursor, 5);
	pwtest_int_eq(SPA_META_Control, 6);
	pwtest_int_eq(SPA_META_Busy, 7);
	pwtest_int_eq(SPA_META_VideoTransform, 8);
	pwtest_int_eq(SPA_META_SyncTimeline, 9);
	pwtest_int_eq(SPA_META_Progressive, 10);
	pwtest_int_eq(SPA_META_Acquisition, 11);
	pwtest_int_eq(_SPA_META_LAST, 12);
	pwtest_str_eq(spa_debug_type_find_name(spa_type_meta_type,
			SPA_META_Acquisition), SPA_TYPE_INFO_META_BASE "Acquisition");

	return PWTEST_PASS;
}

PWTEST(buffer_abi_sizes)
{
#if defined(__x86_64__) && defined(__LP64__)
	pwtest_int_eq(sizeof(struct spa_chunk), 16U);
	pwtest_int_eq(sizeof(struct spa_data), 40U);
	pwtest_int_eq(sizeof(struct spa_buffer), 24U);

	pwtest_int_eq(sizeof(struct spa_meta), 16U);
	pwtest_int_eq(sizeof(struct spa_meta_header), 32U);
	pwtest_int_eq(sizeof(struct spa_meta_region), 16U);
	pwtest_int_eq(sizeof(struct spa_meta_bitmap), 20U);
	pwtest_int_eq(sizeof(struct spa_meta_cursor), 28U);
	pwtest_int_eq(sizeof(struct spa_meta_videotransform), 4U);
	pwtest_int_eq(sizeof(struct spa_meta_acquisition),
			(size_t) SPA_META_ACQUISITION_SIZE);
	pwtest_int_eq(_Alignof(struct spa_meta_acquisition), 8U);
	pwtest_int_eq(offsetof(struct spa_meta_acquisition, domain), 16U);
	pwtest_int_eq(offsetof(struct spa_meta_acquisition, generation), 32U);
	pwtest_int_eq(offsetof(struct spa_meta_acquisition, sequence), 40U);
	pwtest_int_eq(offsetof(struct spa_meta_acquisition, exposure_start_nsec), 48U);
	pwtest_int_eq(offsetof(struct spa_meta_acquisition, reserved), 72U);
	pwtest_int_eq(sizeof(struct spa_meta_progressive), SPA_META_PROGRESSIVE_SIZE);
	pwtest_int_eq(_Alignof(struct spa_meta_progressive), 8U);
	pwtest_int_eq(offsetof(struct spa_meta_progressive, snapshot), 32U);
	pwtest_int_eq(offsetof(struct spa_meta_progressive, reserved1), 40U);

	return PWTEST_PASS;
#else
	fprintf(stderr, "%zd\n", sizeof(struct spa_chunk));
	fprintf(stderr, "%zd\n", sizeof(struct spa_data));
	fprintf(stderr, "%zd\n", sizeof(struct spa_buffer));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_header));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_region));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_bitmap));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_cursor));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_videotransform));
	return PWTEST_SKIP;
#endif
}

PWTEST(buffer_acquisition_meta)
{
	const uint8_t domain_a[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 1 };
	const uint8_t domain_b[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 2 };
	const uint8_t zero_domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 0 };
	struct spa_meta_acquisition acquisition, same, malformed;
	struct spa_meta meta = {
		.type = SPA_META_Acquisition,
		.size = sizeof(acquisition),
		.data = &acquisition,
	};
	uint8_t unaligned[sizeof(acquisition) + 1];

	pwtest_int_eq(SPA_META_ACQUISITION_VERSION, 1);
	pwtest_int_eq(SPA_META_ACQUISITION_SIZE, 96);
	pwtest_int_eq(SPA_META_ACQUISITION_DOMAIN_SIZE, 16);
	pwtest_int_eq(SPA_META_FEATURE_ACQUISITION_VERSION_1, 1 << 0);
	pwtest_bool_false(spa_meta_acquisition_init(NULL));
	pwtest_bool_false(spa_meta_acquisition_init(
			(struct spa_meta_acquisition *)&unaligned[1]));
	pwtest_bool_true(spa_meta_acquisition_init(&acquisition));
	pwtest_int_eq(acquisition.flags, 0U);
	pwtest_int_eq(acquisition.exposure_start_nsec, SPA_TIME_INVALID);
	pwtest_bool_true(spa_meta_acquisition_is_valid(&meta));
	pwtest_bool_false(spa_meta_acquisition_identity_equal(&acquisition, &acquisition));

	pwtest_bool_false(spa_meta_acquisition_set_identity(
			&acquisition, NULL, 7, 42));
	pwtest_bool_false(spa_meta_acquisition_set_identity(
			&acquisition, zero_domain, 7, 42));
	pwtest_bool_true(spa_meta_acquisition_set_identity(
			&acquisition, domain_a, 7, 42));
	same = acquisition;
	pwtest_bool_true(spa_meta_acquisition_identity_equal(&acquisition, &same));
	same.sequence++;
	pwtest_bool_false(spa_meta_acquisition_identity_equal(&acquisition, &same));
	same = acquisition;
	same.generation++;
	pwtest_bool_false(spa_meta_acquisition_identity_equal(&acquisition, &same));
	same = acquisition;
	memcpy(same.domain, domain_b, sizeof(same.domain));
	pwtest_bool_false(spa_meta_acquisition_identity_equal(&acquisition, &same));

	pwtest_bool_false(spa_meta_acquisition_set_exposure_start(
			&acquisition, SPA_TIME_INVALID, 9));
	pwtest_bool_true(spa_meta_acquisition_set_exposure_start(
			&acquisition, 123456, 9));
	pwtest_bool_false(spa_meta_acquisition_set_exposure_duration(&acquisition, 0));
	pwtest_bool_true(spa_meta_acquisition_set_exposure_duration(&acquisition, 5000));
	pwtest_bool_true(spa_meta_acquisition_is_valid(&meta));

	malformed = acquisition;
	malformed.flags |= 1u << 31;
	meta.data = &malformed;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.reserved0 = 1;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.reserved[1] = 1;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.version++;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.abi_size--;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));

	malformed = acquisition;
	SPA_FLAG_CLEAR(malformed.flags, SPA_META_ACQUISITION_FLAG_IDENTITY_VALID);
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	memset(malformed.domain, 0, sizeof(malformed.domain));
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	SPA_FLAG_CLEAR(malformed.flags, SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID);
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.exposure_start_nsec = SPA_TIME_INVALID;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	SPA_FLAG_CLEAR(malformed.flags, SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID);
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	malformed = acquisition;
	malformed.exposure_duration_nsec = 0;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));

	meta.data = &acquisition;
	meta.type = SPA_META_Progressive;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	meta.type = SPA_META_Acquisition;
	meta.size = sizeof(acquisition) - 1;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	meta.size = sizeof(acquisition);
	meta.data = NULL;
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));
	meta.data = &unaligned[1];
	pwtest_bool_false(spa_meta_acquisition_is_valid(&meta));

	return PWTEST_PASS;
}

PWTEST(buffer_acquisition_meta_exports)
{
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_init"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_set_identity"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_set_exposure_start"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_set_exposure_duration"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_is_valid"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_acquisition_identity_equal"));

	return PWTEST_PASS;
}

static struct spa_pod *build_acquisition_meta_param(struct spa_pod_builder *builder,
		uint32_t type, int32_t size, bool with_features,
		uint32_t feature_flags, int32_t features)
{
	struct spa_pod_frame frame;

	spa_pod_builder_push_object(builder, &frame,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(builder,
			SPA_PARAM_META_type, SPA_POD_Id(type),
			SPA_PARAM_META_size, SPA_POD_Int(size),
			0);
	if (with_features) {
		spa_pod_builder_prop(builder, SPA_PARAM_META_features, feature_flags);
		spa_pod_builder_int(builder, features);
	}
	return spa_pod_builder_pop(builder, &frame);
}

PWTEST(buffer_acquisition_meta_param)
{
	uint8_t fixed_buffer[256], offered_buffer[256], result_buffer[512];
	struct spa_pod_builder fixed_builder, offered_builder, result_builder;
	struct spa_pod *fixed, *offered, *result;
	uint32_t type;
	int32_t size, features;

	spa_pod_builder_init(&fixed_builder, fixed_buffer, sizeof(fixed_buffer));
	fixed = build_acquisition_meta_param(&fixed_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE, true,
			SPA_POD_PROP_FLAG_MANDATORY,
			SPA_META_FEATURE_ACQUISITION_VERSION_1);
	spa_pod_builder_init(&offered_builder, offered_buffer, sizeof(offered_buffer));
	offered = build_acquisition_meta_param(&offered_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE, true,
			SPA_POD_PROP_FLAG_MANDATORY,
			SPA_META_FEATURE_ACQUISITION_VERSION_1);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_ge(spa_pod_filter(&result_builder, &result, fixed, offered), 0);
	pwtest_int_ge(spa_pod_filter_make(result), 0);
	pwtest_int_eq(spa_pod_parse_object(result, SPA_TYPE_OBJECT_ParamMeta, NULL,
			SPA_PARAM_META_type, SPA_POD_Id(&type),
			SPA_PARAM_META_size, SPA_POD_Int(&size),
			SPA_PARAM_META_features, SPA_POD_Int(&features)), 3);
	pwtest_int_eq(type, (uint32_t) SPA_META_Acquisition);
	pwtest_int_eq(size, (int32_t) SPA_META_ACQUISITION_SIZE);
	pwtest_int_eq(features,
			(int32_t) SPA_META_FEATURE_ACQUISITION_VERSION_1);

	spa_pod_builder_init(&offered_builder, offered_buffer, sizeof(offered_buffer));
	offered = build_acquisition_meta_param(&offered_builder, SPA_META_Progressive,
			SPA_META_ACQUISITION_SIZE, true,
			SPA_POD_PROP_FLAG_MANDATORY,
			SPA_META_FEATURE_ACQUISITION_VERSION_1);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_lt(spa_pod_filter(&result_builder, &result, fixed, offered), 0);

	spa_pod_builder_init(&offered_builder, offered_buffer, sizeof(offered_buffer));
	offered = build_acquisition_meta_param(&offered_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE - 1, true,
			SPA_POD_PROP_FLAG_MANDATORY,
			SPA_META_FEATURE_ACQUISITION_VERSION_1);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_lt(spa_pod_filter(&result_builder, &result, fixed, offered), 0);

	spa_pod_builder_init(&offered_builder, offered_buffer, sizeof(offered_buffer));
	offered = build_acquisition_meta_param(&offered_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE, true,
			SPA_POD_PROP_FLAG_MANDATORY, 1u << 1);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_lt(spa_pod_filter(&result_builder, &result, fixed, offered), 0);

	spa_pod_builder_init(&offered_builder, offered_buffer, sizeof(offered_buffer));
	offered = build_acquisition_meta_param(&offered_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE, false, 0, 0);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_lt(spa_pod_filter(&result_builder, &result, fixed, offered), 0);

	spa_pod_builder_init(&fixed_builder, fixed_buffer, sizeof(fixed_buffer));
	fixed = build_acquisition_meta_param(&fixed_builder, SPA_META_Acquisition,
			SPA_META_ACQUISITION_SIZE, true, SPA_POD_PROP_FLAG_DROP,
			SPA_META_FEATURE_ACQUISITION_VERSION_1);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_ge(spa_pod_filter(&result_builder, &result, fixed, offered), 0);
	pwtest_ptr_null(spa_pod_find_prop(result, NULL, SPA_PARAM_META_features));

	return PWTEST_PASS;
}

PWTEST(buffer_progressive_meta)
{
	struct spa_meta_progressive progressive;
	struct spa_meta meta = {
		.type = SPA_META_Progressive,
		.size = sizeof(progressive),
		.data = &progressive,
	};
	enum spa_meta_progressive_state state;
	uint32_t committed;
	uint64_t snapshot;
	uint8_t unaligned[sizeof(progressive) + 1];

	pwtest_int_eq(SPA_META_PROGRESSIVE_STATE_PREPARED, 0);
	pwtest_int_eq(SPA_META_PROGRESSIVE_STATE_ACTIVE, 1);
	pwtest_int_eq(SPA_META_PROGRESSIVE_STATE_COMPLETE, 2);
	pwtest_int_eq(SPA_META_PROGRESSIVE_STATE_ABORTED, 3);
	pwtest_bool_true(spa_meta_progressive_init(&progressive, 1, 128, 4096, 256));
	pwtest_bool_false(spa_meta_progressive_init(&progressive, 1, 128, 0, 256));
	pwtest_bool_false(spa_meta_progressive_init(&progressive, 1, 128, 4096, 0));
	pwtest_bool_true(spa_meta_progressive_init(&progressive, 1, 128, 4096, 256));

	snapshot = spa_meta_progressive_snapshot_encode(1024,
			SPA_META_PROGRESSIVE_STATE_ACTIVE);
	spa_meta_progressive_store_release(&progressive, snapshot);
	pwtest_int_eq(spa_meta_progressive_load_acquire(&progressive), snapshot);
	pwtest_bool_true(spa_meta_progressive_snapshot_decode(snapshot, &committed, &state));
	pwtest_int_eq(committed, 1024U);
	pwtest_int_eq((uint32_t) state,
			(uint32_t) SPA_META_PROGRESSIVE_STATE_ACTIVE);
	pwtest_bool_true(spa_meta_progressive_is_valid(&meta));
	spa_meta_progressive_store_release(&progressive,
			spa_meta_progressive_snapshot_encode(1025,
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	spa_meta_progressive_store_release(&progressive,
			spa_meta_progressive_snapshot_encode(1024,
				SPA_META_PROGRESSIVE_STATE_COMPLETE));
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	spa_meta_progressive_store_release(&progressive,
			spa_meta_progressive_snapshot_encode(1,
				SPA_META_PROGRESSIVE_STATE_PREPARED));
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	progressive.terminal_flags = SPA_META_PROGRESSIVE_FLAG_CANCELLED;
	spa_meta_progressive_store_release(&progressive,
			spa_meta_progressive_snapshot_encode(1024,
				SPA_META_PROGRESSIVE_STATE_ABORTED));
	pwtest_bool_true(spa_meta_progressive_is_valid(&meta));
	progressive.terminal_flags = ~SPA_META_PROGRESSIVE_FLAG_ALL;
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	progressive.terminal_flags = 0;

	pwtest_bool_false(spa_meta_progressive_snapshot_decode(
			SPA_META_PROGRESSIVE_RESERVED_MASK, NULL, NULL));
	spa_meta_progressive_store_release(&progressive,
			SPA_META_PROGRESSIVE_RESERVED_MASK);
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));

	spa_meta_progressive_store_release(&progressive, snapshot);
	progressive.reserved0 = 1;
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	progressive.reserved0 = 0;

	meta.size = sizeof(progressive) - 1;
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));
	meta.size = sizeof(progressive);
	meta.data = &unaligned[1];
	pwtest_bool_false(spa_meta_progressive_is_valid(&meta));

	return PWTEST_PASS;
}

PWTEST(buffer_progressive_meta_exports)
{
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_snapshot_encode"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_snapshot_decode"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_load_acquire"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_store_release"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_init"));
	pwtest_ptr_notnull(dlsym(RTLD_DEFAULT, "spa_meta_progressive_is_valid"));

	return PWTEST_PASS;
}

PWTEST(buffer_alloc)
{
	struct spa_buffer **buffers;
	struct spa_meta metas[5];
	struct spa_data datas[2];
	uint32_t aligns[2];
	uint32_t i, j;

	metas[0].type = SPA_META_Header;
	metas[0].size = sizeof(struct spa_meta_header);
	metas[1].type = SPA_META_VideoDamage;
	metas[1].size = sizeof(struct spa_meta_region) * 16;
#define CURSOR_META_SIZE(w,h,bpp) (sizeof(struct spa_meta_cursor) + \
                                   sizeof(struct spa_meta_bitmap) + w * h * bpp)
	metas[2].type = SPA_META_Cursor;
	metas[2].size = CURSOR_META_SIZE(64,64,4);
	metas[3].type = SPA_META_Acquisition;
	metas[3].size = SPA_META_ACQUISITION_SIZE;
	metas[4].type = 101;
	metas[4].size = 11;

	datas[0].maxsize = 4000;
	datas[1].maxsize = 2011;

	aligns[0] = 32;
	aligns[1] = 16;

	buffers = spa_buffer_alloc_array(16, 0,
			SPA_N_ELEMENTS(metas), metas,
			SPA_N_ELEMENTS(datas), datas, aligns);

	fprintf(stderr, "buffers %p\n", buffers);

	for (i = 0; i < 16; i++) {
		struct spa_buffer *b = buffers[i];
		fprintf(stderr, "buffer %d %p\n", i, b);

		pwtest_int_eq(b->n_metas, SPA_N_ELEMENTS(metas));
		pwtest_int_eq(b->n_datas, SPA_N_ELEMENTS(datas));

		for (j = 0; j < SPA_N_ELEMENTS(metas); j++) {
			pwtest_int_eq(b->metas[j].type, metas[j].type);
			pwtest_int_eq(b->metas[j].size, metas[j].size);
			fprintf(stderr, " meta %d %p\n", j, b->metas[j].data);
			pwtest_bool_true(SPA_IS_ALIGNED(b->metas[j].data, 8));
		}

		for (j = 0; j < SPA_N_ELEMENTS(datas); j++) {
			pwtest_int_eq(b->datas[j].maxsize, datas[j].maxsize);
			fprintf(stderr, " data %d %p %p\n", j, b->datas[j].chunk, b->datas[j].data);
			pwtest_bool_true(SPA_IS_ALIGNED(b->datas[j].chunk, 8));
			pwtest_bool_true(SPA_IS_ALIGNED(b->datas[j].data, aligns[j]));
		}
	}
	free(buffers);

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_buffer)
{
	pwtest_add(buffer_abi_types, PWTEST_NOARG);
	pwtest_add(buffer_abi_sizes, PWTEST_NOARG);
	pwtest_add(buffer_acquisition_meta, PWTEST_NOARG);
	pwtest_add(buffer_acquisition_meta_exports, PWTEST_NOARG);
	pwtest_add(buffer_acquisition_meta_param, PWTEST_NOARG);
	pwtest_add(buffer_progressive_meta, PWTEST_NOARG);
	pwtest_add(buffer_progressive_meta_exports, PWTEST_NOARG);
	pwtest_add(buffer_alloc, PWTEST_NOARG);

	return PWTEST_PASS;
}
