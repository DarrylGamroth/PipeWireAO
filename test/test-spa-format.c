/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-types.h>

#include "pwtest.h"

PWTEST(audio_format_sizes)
{
	union {
		uint8_t buf[1024];
		struct spa_audio_info align;
	} data;
	struct spa_audio_info info;
	size_t i;

	memset(&info, 0xf3, sizeof(info));
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.channels = 5;
	info.info.raw.format = SPA_AUDIO_FORMAT_F32P;
	info.info.raw.rate = 12345;
	info.info.raw.flags = 0;
	info.info.raw.position[0] = 1;
	info.info.raw.position[1] = 2;
	info.info.raw.position[2] = 3;
	info.info.raw.position[3] = 4;
	info.info.raw.position[4] = 5;

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		memcpy(data.buf, &info, sizeof(info));

		pod = spa_format_audio_ext_build(&b, 123, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ offsetof(struct spa_audio_info_raw, position))
			pwtest_bool_true(!pod);
		else
			pwtest_bool_true(pod);
	}

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;
		int ret;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		pod = spa_format_audio_ext_build(&b, 123, &info, sizeof(info));
		pwtest_bool_true(pod);

		memset(data.buf, 0xf3, sizeof(data.buf));

		ret = spa_format_audio_ext_parse(pod, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ offsetof(struct spa_audio_info_raw, position)
				+ info.info.raw.channels*sizeof(uint32_t)) {
			for (size_t j = i; j < sizeof(data.buf); ++j)
				pwtest_int_eq(data.buf[j], 0xf3);
			pwtest_int_lt(ret, 0);
		} else {
			pwtest_int_ge(ret, 0);
			pwtest_bool_true(memcmp(data.buf, &info, SPA_MIN(i, sizeof(info))) == 0);
		}
	}

	memset(&info, 0xf3, sizeof(info));
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_aac;
	info.info.aac.rate = 12345;
	info.info.aac.channels = 6;
	info.info.aac.bitrate = 54321;
	info.info.aac.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_MP4LATM;

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		memcpy(data.buf, &info, sizeof(info));

		pod = spa_format_audio_ext_build(&b, 123, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ sizeof(struct spa_audio_info_aac))
			pwtest_bool_true(!pod);
		else
			pwtest_bool_true(pod);
	}

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;
		int ret;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		pod = spa_format_audio_ext_build(&b, 123, &info, sizeof(info));
		pwtest_bool_true(pod);

		memset(data.buf, 0xf3, sizeof(data.buf));

		ret = spa_format_audio_ext_parse(pod, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ sizeof(struct spa_audio_info_aac)) {
			for (size_t j = i; j < sizeof(data.buf); ++j)
				pwtest_int_eq(data.buf[j], 0xf3);
			pwtest_int_lt(ret, 0);
		} else {
			pwtest_int_ge(ret, 0);
			pwtest_bool_true(memcmp(data.buf, &info, SPA_MIN(i, sizeof(info))) == 0);
		}
	}

	return PWTEST_PASS;
}

PWTEST(ndarray_format_abi)
{
	static const struct {
		enum spa_element_type type;
		uint32_t size;
	} element_types[] = {
		{ SPA_ELEMENT_TYPE_BOOL8, 1 },
		{ SPA_ELEMENT_TYPE_I8, 1 },
		{ SPA_ELEMENT_TYPE_U8, 1 },
		{ SPA_ELEMENT_TYPE_I16_LE, 2 },
		{ SPA_ELEMENT_TYPE_U16_LE, 2 },
		{ SPA_ELEMENT_TYPE_I32_LE, 4 },
		{ SPA_ELEMENT_TYPE_U32_LE, 4 },
		{ SPA_ELEMENT_TYPE_I64_LE, 8 },
		{ SPA_ELEMENT_TYPE_U64_LE, 8 },
		{ SPA_ELEMENT_TYPE_I128_LE, 16 },
		{ SPA_ELEMENT_TYPE_U128_LE, 16 },
		{ SPA_ELEMENT_TYPE_F8_E4M3FN, 1 },
		{ SPA_ELEMENT_TYPE_F8_E4M3FNUZ, 1 },
		{ SPA_ELEMENT_TYPE_F8_E5M2, 1 },
		{ SPA_ELEMENT_TYPE_F8_E5M2FNUZ, 1 },
		{ SPA_ELEMENT_TYPE_F16_LE, 2 },
		{ SPA_ELEMENT_TYPE_BF16_LE, 2 },
		{ SPA_ELEMENT_TYPE_F32_LE, 4 },
		{ SPA_ELEMENT_TYPE_F64_LE, 8 },
		{ SPA_ELEMENT_TYPE_F128_LE, 16 },
		{ SPA_ELEMENT_TYPE_COMPLEX_F16_LE, 4 },
		{ SPA_ELEMENT_TYPE_COMPLEX_BF16_LE, 4 },
		{ SPA_ELEMENT_TYPE_COMPLEX_F32_LE, 8 },
		{ SPA_ELEMENT_TYPE_COMPLEX_F64_LE, 16 },
		{ SPA_ELEMENT_TYPE_COMPLEX_F128_LE, 32 },
	};
	uint32_t i;

	pwtest_int_eq(SPA_MEDIA_SUBTYPE_ndarray, 0x60002);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_elementType, 0x61001);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_shape, 0x61002);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_layout, 0x61003);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_rate, 0x61004);
	pwtest_int_eq(SPA_NDARRAY_LAYOUT_ROW_MAJOR, 1);
	pwtest_int_eq(SPA_NDARRAY_LAYOUT_COLUMN_MAJOR, 2);
	pwtest_int_eq(SPA_ELEMENT_TYPE_BOOL8, 1);
	pwtest_int_eq(SPA_ELEMENT_TYPE_COMPLEX_F128_LE, 25);
	pwtest_int_eq(SPA_ELEMENT_TYPE_START_CUSTOM, 0x10000);
	pwtest_int_eq(spa_element_type_size(SPA_ELEMENT_TYPE_UNKNOWN), 0U);
	pwtest_int_eq(spa_element_type_size(SPA_ELEMENT_TYPE_START_CUSTOM), 0U);
	for (i = 0; i < SPA_N_ELEMENTS(element_types); i++)
		pwtest_int_eq(spa_element_type_size(element_types[i].type),
				element_types[i].size);

	return PWTEST_PASS;
}

PWTEST(ndarray_format_pods)
{
	uint8_t buffer[1024];
	struct spa_pod_builder builder;
	struct spa_pod *pod;
	uint32_t media_type, media_subtype, element_type, layout;
	uint32_t child_size, child_type, n_dimensions;
	int32_t vector_shape[] = { 2048 };
	int32_t matrix_shape[] = { 48, 64 };
	int32_t *shape;
	struct spa_fraction rate;

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	pod = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int,
			SPA_N_ELEMENTS(vector_shape), vector_shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
		SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&SPA_FRACTION(1000, 1)));
	pwtest_int_eq(spa_pod_parse_object(pod, SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(&element_type),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(&child_size, &child_type,
			&n_dimensions, &shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(&layout),
		SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate)), 6);
	pwtest_int_eq(media_type, (uint32_t) SPA_MEDIA_TYPE_application);
	pwtest_int_eq(media_subtype, (uint32_t) SPA_MEDIA_SUBTYPE_ndarray);
	pwtest_int_eq(element_type, (uint32_t) SPA_ELEMENT_TYPE_F32_LE);
	pwtest_int_eq(child_size, sizeof(int32_t));
	pwtest_int_eq(child_type, (uint32_t) SPA_TYPE_Int);
	pwtest_int_eq(n_dimensions, 1U);
	pwtest_int_eq(shape[0], 2048);
	pwtest_int_eq(layout, (uint32_t) SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	pwtest_int_eq(rate.num, 1000U);
	pwtest_int_eq(rate.denom, 1U);

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	pod = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F64_LE),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int,
			SPA_N_ELEMENTS(matrix_shape), matrix_shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(SPA_NDARRAY_LAYOUT_COLUMN_MAJOR));
	pwtest_int_eq(spa_pod_parse_object(pod, SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(&element_type),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(&child_size, &child_type,
			&n_dimensions, &shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(&layout)), 5);
	pwtest_int_eq(media_subtype, (uint32_t) SPA_MEDIA_SUBTYPE_ndarray);
	pwtest_int_eq(element_type, (uint32_t) SPA_ELEMENT_TYPE_F64_LE);
	pwtest_int_eq(n_dimensions, 2U);
	pwtest_int_eq(shape[0], 48);
	pwtest_int_eq(shape[1], 64);
	pwtest_int_eq(layout, (uint32_t) SPA_NDARRAY_LAYOUT_COLUMN_MAJOR);

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	pod = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int,
			SPA_N_ELEMENTS(matrix_shape), matrix_shape));
	pwtest_int_eq(spa_pod_parse_object(pod, SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(&element_type),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(&child_size, &child_type,
			&n_dimensions, &shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(&layout)), -ESRCH);

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_format)
{
	pwtest_add(audio_format_sizes, PWTEST_NOARG);
	pwtest_add(ndarray_format_abi, PWTEST_NOARG);
	pwtest_add(ndarray_format_pods, PWTEST_NOARG);

	return PWTEST_PASS;
}
