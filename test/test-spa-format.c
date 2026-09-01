/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-types.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/filter.h>

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

	pwtest_int_eq(SPA_MEDIA_SUBTYPE_ndarray, 0x1000000);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_elementType, 0x1000001);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_shape, 0x1000002);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_layout, 0x1000003);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_rate, 0x1000004);
	pwtest_int_eq(SPA_FORMAT_NDARRAY_schema, 0x1000005);
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
	const char *schema;
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
		SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&SPA_FRACTION(1000, 1)),
		SPA_FORMAT_NDARRAY_schema, SPA_POD_String("org.pipewireao.test.vector/1"));
	pwtest_int_eq(spa_pod_parse_object(pod, SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(&element_type),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(&child_size, &child_type,
			&n_dimensions, &shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(&layout),
		SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate),
		SPA_FORMAT_NDARRAY_schema, SPA_POD_String(&schema)), 7);
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
	pwtest_str_eq(schema, "org.pipewireao.test.vector/1");

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

PWTEST(ndarray_format_utils)
{
	uint8_t buffer[2048], invalid_buffer[1024];
	struct spa_pod_builder builder;
	struct spa_pod *pod, *invalid;
	struct spa_ndarray_info parsed;
	struct spa_ndarray_info matrix = SPA_NDARRAY_INFO_INIT(
		.element_type = SPA_ELEMENT_TYPE_F32_LE,
		.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
		.rate = SPA_FRACTION(1000, 1),
		.n_dimensions = 2,
		.shape = { 48, 64 });
	size_t n_elements, n_bytes;

	pwtest_int_eq(spa_ndarray_info_validate(&matrix), 0);
	pwtest_int_eq(spa_ndarray_info_get_n_elements(&matrix, &n_elements), 0);
	pwtest_int_eq(n_elements, 48U * 64U);
	pwtest_int_eq(spa_ndarray_info_get_size(&matrix, &n_bytes), 0);
	pwtest_int_eq(n_bytes, 48U * 64U * sizeof(float));

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	pod = spa_format_ndarray_build(&builder, SPA_PARAM_Format, &matrix);
	pwtest_ptr_notnull(pod);
	spa_zero(parsed);
	pwtest_int_eq(spa_format_ndarray_parse(pod, &parsed), 0);
	pwtest_int_eq(parsed.element_type, matrix.element_type);
	pwtest_int_eq(parsed.layout, matrix.layout);
	pwtest_int_eq(parsed.rate.num, matrix.rate.num);
	pwtest_int_eq(parsed.rate.denom, matrix.rate.denom);
	pwtest_int_eq(parsed.n_dimensions, 2U);
	pwtest_int_eq(parsed.shape[0], 48U);
	pwtest_int_eq(parsed.shape[1], 64U);

	parsed = matrix;
	parsed.element_type = SPA_ELEMENT_TYPE_UNKNOWN;
	pwtest_int_eq(spa_ndarray_info_validate(&parsed), -EINVAL);
	parsed = matrix;
	parsed.layout = SPA_NDARRAY_LAYOUT_UNKNOWN;
	pwtest_int_eq(spa_ndarray_info_validate(&parsed), -EINVAL);
	parsed = matrix;
	parsed.shape[0] = 0;
	pwtest_int_eq(spa_ndarray_info_validate(&parsed), -EINVAL);
	parsed = matrix;
	parsed.rate = SPA_FRACTION(0, 1);
	pwtest_int_eq(spa_ndarray_info_validate(&parsed), -EINVAL);
	parsed = matrix;
	parsed.n_dimensions = 3;
	parsed.shape[0] = INT32_MAX;
	parsed.shape[1] = INT32_MAX;
	parsed.shape[2] = INT32_MAX;
	pwtest_int_eq(spa_ndarray_info_validate(&parsed), -EOVERFLOW);
	pwtest_int_eq(spa_ndarray_info_ext_validate(&matrix,
			offsetof(struct spa_ndarray_info, shape) + sizeof(uint32_t)), -EINVAL);

	spa_pod_builder_init(&builder, invalid_buffer, sizeof(invalid_buffer));
	invalid = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
			matrix.n_dimensions, matrix.shape),
		SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR));
	pwtest_int_eq(spa_format_ndarray_parse(invalid, &parsed), -EINVAL);

	spa_pod_builder_init(&builder, invalid_buffer, sizeof(invalid_buffer));
	invalid = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
		SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_F32_LE),
		SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
			matrix.n_dimensions, matrix.shape));
	pwtest_int_eq(spa_format_ndarray_parse(invalid, &parsed), -EINVAL);

	return PWTEST_PASS;
}

PWTEST(ndarray_format_choices)
{
	uint8_t offered_buffer[2048], fixed_buffer[2048], result_buffer[2048],
		mismatch_buffer[2048];
	struct spa_pod_builder builder, result_builder;
	struct spa_pod *offered, *fixed, *result, *mismatch;
	struct spa_ndarray_info parsed;
	struct spa_ndarray_info offered_info = SPA_NDARRAY_INFO_INIT(
		.element_type = SPA_ELEMENT_TYPE_F64_LE,
		.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
		.rate = SPA_FRACTION(1000, 1),
		.n_dimensions = 2,
		.shape = { 16, 16 });
	struct spa_ndarray_info fixed_info = SPA_NDARRAY_INFO_INIT(
		.element_type = SPA_ELEMENT_TYPE_F32_LE,
		.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
		.rate = SPA_FRACTION(1000, 1),
		.n_dimensions = 2,
		.shape = { 16, 16 });
	const enum spa_element_type element_types[] = { SPA_ELEMENT_TYPE_F32_LE };
	const enum spa_ndarray_layout layouts[] = { SPA_NDARRAY_LAYOUT_ROW_MAJOR };
	const struct spa_fraction rates[] = {
		SPA_FRACTION(500, 1), SPA_FRACTION(2000, 1)
	};
	const struct spa_ndarray_choices choices = SPA_NDARRAY_CHOICES_INIT(
		.n_element_types = SPA_N_ELEMENTS(element_types),
		.element_types = element_types,
		.n_layouts = SPA_N_ELEMENTS(layouts),
		.layouts = layouts,
		.rate_choice = SPA_CHOICE_Range,
		.n_rate_values = SPA_N_ELEMENTS(rates),
		.rate_values = rates);
	const struct spa_pod_prop *property;

	pwtest_int_eq(spa_ndarray_choices_validate(&offered_info,
			sizeof(offered_info), &choices), 0);
	spa_pod_builder_init(&builder, offered_buffer, sizeof(offered_buffer));
	offered = spa_format_ndarray_build_choices(&builder, SPA_PARAM_EnumFormat,
			&offered_info, &choices);
	pwtest_ptr_notnull(offered);
	property = spa_pod_find_prop(offered, NULL, SPA_FORMAT_NDARRAY_elementType);
	pwtest_ptr_notnull(property);
	pwtest_bool_true(spa_pod_is_choice(&property->value));
	pwtest_int_eq(SPA_POD_CHOICE_TYPE(&property->value),
			(uint32_t)SPA_CHOICE_Enum);
	pwtest_int_eq(SPA_POD_CHOICE_N_VALUES(&property->value), 3U);
	property = spa_pod_find_prop(offered, NULL, SPA_FORMAT_NDARRAY_shape);
	pwtest_ptr_notnull(property);
	pwtest_bool_false(spa_pod_is_choice(&property->value));
	property = spa_pod_find_prop(offered, NULL, SPA_FORMAT_NDARRAY_rate);
	pwtest_ptr_notnull(property);
	pwtest_int_eq(SPA_POD_CHOICE_TYPE(&property->value),
			(uint32_t)SPA_CHOICE_Range);
	pwtest_int_lt(spa_format_ndarray_parse(offered, &parsed), 0);

	spa_pod_builder_init(&builder, fixed_buffer, sizeof(fixed_buffer));
	fixed = spa_format_ndarray_build(&builder, SPA_PARAM_EnumFormat, &fixed_info);
	pwtest_ptr_notnull(fixed);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_ge(spa_pod_filter(&result_builder, &result, fixed, offered), 0);
	pwtest_int_ge(spa_pod_filter_make(result), 0);
	pwtest_int_eq(spa_format_ndarray_parse(result, &parsed), 0);
	pwtest_int_eq(parsed.element_type, (enum spa_element_type)SPA_ELEMENT_TYPE_F32_LE);
	pwtest_int_eq(parsed.layout, (enum spa_ndarray_layout)SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	pwtest_int_eq(parsed.rate.num, 1000U);

	fixed_info.element_type = SPA_ELEMENT_TYPE_F64_LE;
	fixed_info.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR;
	spa_pod_builder_init(&builder, fixed_buffer, sizeof(fixed_buffer));
	fixed = spa_format_ndarray_build(&builder, SPA_PARAM_EnumFormat, &fixed_info);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_ge(spa_pod_filter(&result_builder, &result, fixed, offered), 0);
	pwtest_int_ge(spa_pod_filter_make(result), 0);
	pwtest_int_eq(spa_format_ndarray_parse(result, &parsed), 0);
	pwtest_int_eq(parsed.element_type, (enum spa_element_type)SPA_ELEMENT_TYPE_F64_LE);
	pwtest_int_eq(parsed.layout, (enum spa_ndarray_layout)SPA_NDARRAY_LAYOUT_COLUMN_MAJOR);

	fixed_info.shape[1] = 17;
	spa_pod_builder_init(&builder, mismatch_buffer, sizeof(mismatch_buffer));
	mismatch = spa_format_ndarray_build(&builder, SPA_PARAM_EnumFormat, &fixed_info);
	pwtest_ptr_notnull(mismatch);
	spa_pod_builder_init(&result_builder, result_buffer, sizeof(result_buffer));
	pwtest_int_lt(spa_pod_filter(&result_builder, &result, mismatch, offered), 0);

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_format)
{
	pwtest_add(audio_format_sizes, PWTEST_NOARG);
	pwtest_add(ndarray_format_abi, PWTEST_NOARG);
	pwtest_add(ndarray_format_pods, PWTEST_NOARG);
	pwtest_add(ndarray_format_utils, PWTEST_NOARG);
	pwtest_add(ndarray_format_choices, PWTEST_NOARG);

	return PWTEST_PASS;
}
