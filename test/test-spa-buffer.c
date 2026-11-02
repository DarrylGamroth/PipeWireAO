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
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_PAGES, 1U << 28);
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_2MB, 1U << 29);
	pwtest_int_eq(SPA_DATA_FLAG_HUGE_1GB, 1U << 30);

	/* buffer allocation hints */
	pwtest_int_eq(SPA_PARAM_BUFFERS_pageSizeHint, 0x1000000);
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
	/* PipeWireAO records use the custom namespace; 11/12/13 stay retired. */
	pwtest_int_eq(SPA_META_AcquisitionIdentity, 0x300);
	pwtest_int_eq(SPA_META_ExposureTiming, 0x301);
	pwtest_int_eq(SPA_META_AbsoluteTime, 0x302);
	pwtest_int_eq(SPA_META_OpticalChopping, 0x303);
	pwtest_int_eq(SPA_META_ElectricalTrigger, 0x304);
	pwtest_int_eq(_SPA_META_LAST, 10);
	pwtest_str_eq(spa_debug_type_find_name(spa_type_meta_type,
			SPA_META_AcquisitionIdentity), SPA_TYPE_INFO_META_BASE "AcquisitionIdentity");

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
	metas[3].type = SPA_META_AcquisitionIdentity;
	metas[3].size = sizeof(struct spa_meta_acquisition_identity);
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
	pwtest_add(buffer_alloc, PWTEST_NOARG);

	return PWTEST_PASS;
}
