/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_NDARRAY_UTILS_H
#define SPA_PARAM_NDARRAY_UTILS_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <spa/param/format-utils.h>
#include <spa/param/ndarray.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_NDARRAY_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_NDARRAY_UTILS SPA_API_IMPL
 #else
  #define SPA_API_NDARRAY_UTILS static inline
 #endif
#endif

static inline int spa_ndarray_info_ext_counts(const struct spa_ndarray_info *info,
		size_t size, size_t *n_elements, size_t *n_bytes)
{
	uint32_t element_size, max_dimensions, i;
	size_t elements = 1;

	if (info == NULL || !SPA_NDARRAY_INFO_VALID_SIZE(size))
		return -EINVAL;
	max_dimensions = SPA_NDARRAY_INFO_MAX_DIMENSIONS(size);
	if (info->n_dimensions == 0 || info->n_dimensions > max_dimensions)
		return -EINVAL;
	element_size = spa_element_type_size(info->element_type);
	if (element_size == 0)
		return -EINVAL;
	if (info->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR &&
	    info->layout != SPA_NDARRAY_LAYOUT_COLUMN_MAJOR)
		return -EINVAL;
	if ((info->rate.denom == 0 && info->rate.num != 0) ||
	    (info->rate.denom != 0 && info->rate.num == 0))
		return -EINVAL;

	for (i = 0; i < info->n_dimensions; i++) {
		uint32_t dimension = info->shape[i];

		if (dimension == 0 || dimension > INT32_MAX)
			return -EINVAL;
		if (elements > SIZE_MAX / dimension)
			return -EOVERFLOW;
		elements *= dimension;
	}
	if (elements > SIZE_MAX / element_size)
		return -EOVERFLOW;

	if (n_elements != NULL)
		*n_elements = elements;
	if (n_bytes != NULL)
		*n_bytes = elements * element_size;
	return 0;
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_ext_validate(const struct spa_ndarray_info *info, size_t size)
{
	return spa_ndarray_info_ext_counts(info, size, NULL, NULL);
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_validate(const struct spa_ndarray_info *info)
{
	return spa_ndarray_info_ext_validate(info, sizeof(*info));
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_ext_get_n_elements(const struct spa_ndarray_info *info,
		size_t size, size_t *n_elements)
{
	if (n_elements == NULL)
		return -EINVAL;
	return spa_ndarray_info_ext_counts(info, size, n_elements, NULL);
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_get_n_elements(const struct spa_ndarray_info *info,
		size_t *n_elements)
{
	return spa_ndarray_info_ext_get_n_elements(info, sizeof(*info), n_elements);
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_ext_get_size(const struct spa_ndarray_info *info,
		size_t size, size_t *n_bytes)
{
	if (n_bytes == NULL)
		return -EINVAL;
	return spa_ndarray_info_ext_counts(info, size, NULL, n_bytes);
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_info_get_size(const struct spa_ndarray_info *info, size_t *n_bytes)
{
	return spa_ndarray_info_ext_get_size(info, sizeof(*info), n_bytes);
}

static inline uint32_t spa_ndarray_format_key_count(const struct spa_pod *format,
		uint32_t key)
{
	const struct spa_pod_prop *property;
	uint32_t count = 0;

	if (!spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format))
		return 0;
	SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)format, property) {
		if (property->key == key)
			count++;
	}
	return count;
}

/** Parse one optional fixed string property from an ndarray format.
 *
 * Fixed formats produced by SPA negotiation may represent a value either as
 * its direct pod type or as a \ref SPA_CHOICE_None containing that type. An
 * absent property produces a NULL value.  Duplicate properties and non-fixed
 * choices are rejected.
 */
SPA_API_NDARRAY_UTILS int
spa_format_ndarray_parse_string(const struct spa_pod *format,
		uint32_t key, const char **value)
{
	const struct spa_pod_prop *property;
	int res;

	if (format == NULL || value == NULL)
		return -EINVAL;
	*value = NULL;
	if (spa_ndarray_format_key_count(format, key) > 1)
		return -EINVAL;
	if ((property = spa_pod_find_prop(format, NULL, key)) == NULL)
		return 0;

	res = spa_pod_parse_object(format, SPA_TYPE_OBJECT_Format, NULL,
			key, SPA_POD_String(value));
	return res < 0 ? res : 0;
}

SPA_API_NDARRAY_UTILS int
spa_format_ndarray_ext_parse(const struct spa_pod *format,
		struct spa_ndarray_info *info, size_t size)
{
	struct spa_pod *shape_pod = NULL;
	struct spa_fraction rate = SPA_FRACTION(0, 0);
	uint32_t media_type, media_subtype, element_type, layout;
	uint32_t n_dimensions, child_size, child_type, max_dimensions, i;
	const int32_t *shape;
	int res;

	if (format == NULL || info == NULL || !SPA_NDARRAY_INFO_VALID_SIZE(size))
		return -EINVAL;
	if (spa_ndarray_format_key_count(format, SPA_FORMAT_mediaType) != 1 ||
	    spa_ndarray_format_key_count(format, SPA_FORMAT_mediaSubtype) != 1 ||
	    spa_ndarray_format_key_count(format, SPA_FORMAT_NDARRAY_elementType) != 1 ||
	    spa_ndarray_format_key_count(format, SPA_FORMAT_NDARRAY_shape) != 1 ||
	    spa_ndarray_format_key_count(format, SPA_FORMAT_NDARRAY_layout) != 1 ||
	    spa_ndarray_format_key_count(format, SPA_FORMAT_NDARRAY_rate) > 1)
		return -EINVAL;

	res = spa_pod_parse_object(format, SPA_TYPE_OBJECT_Format, NULL,
			SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype),
			SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(&element_type),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Pod(&shape_pod),
			SPA_FORMAT_NDARRAY_layout, SPA_POD_Id(&layout),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_OPT_Fraction(&rate));
	if (res < 0)
		return res;
	if (media_type != SPA_MEDIA_TYPE_application ||
	    media_subtype != SPA_MEDIA_SUBTYPE_ndarray)
		return -EINVAL;

	shape = (const int32_t *)spa_pod_get_array_full(shape_pod, &n_dimensions,
			&child_size, &child_type);
	max_dimensions = SPA_NDARRAY_INFO_MAX_DIMENSIONS(size);
	if (shape == NULL || child_size != sizeof(int32_t) ||
	    child_type != SPA_TYPE_Int || n_dimensions == 0 ||
	    n_dimensions > max_dimensions)
		return -EINVAL;

	info->element_type = (enum spa_element_type)element_type;
	info->layout = (enum spa_ndarray_layout)layout;
	info->rate = rate;
	info->n_dimensions = n_dimensions;
	for (i = 0; i < n_dimensions; i++) {
		if (shape[i] <= 0)
			return -EINVAL;
		info->shape[i] = (uint32_t)shape[i];
	}
	return spa_ndarray_info_ext_validate(info, size);
}

SPA_API_NDARRAY_UTILS int
spa_format_ndarray_parse(const struct spa_pod *format, struct spa_ndarray_info *info)
{
	return spa_format_ndarray_ext_parse(format, info, sizeof(*info));
}

static inline int spa_ndarray_rate_compare(const struct spa_fraction *a,
		const struct spa_fraction *b)
{
	uint64_t left = (uint64_t)a->num * b->denom;
	uint64_t right = (uint64_t)b->num * a->denom;

	return left < right ? -1 : left > right ? 1 : 0;
}

static inline bool spa_ndarray_rate_valid(const struct spa_fraction *rate)
{
	return rate != NULL && rate->num != 0 && rate->denom != 0;
}

SPA_API_NDARRAY_UTILS int
spa_ndarray_choices_validate(const struct spa_ndarray_info *info, size_t size,
		const struct spa_ndarray_choices *choices)
{
	uint32_t i;
	int res;

	if ((res = spa_ndarray_info_ext_validate(info, size)) < 0 || choices == NULL)
		return res;
	if ((choices->n_element_types != 0 && choices->element_types == NULL) ||
	    (choices->n_layouts != 0 && choices->layouts == NULL) ||
	    (choices->n_rate_values != 0 && choices->rate_values == NULL))
		return -EINVAL;
	for (i = 0; i < choices->n_element_types; i++) {
		if (spa_element_type_size(choices->element_types[i]) == 0)
			return -EINVAL;
	}
	for (i = 0; i < choices->n_layouts; i++) {
		if (choices->layouts[i] != SPA_NDARRAY_LAYOUT_ROW_MAJOR &&
		    choices->layouts[i] != SPA_NDARRAY_LAYOUT_COLUMN_MAJOR)
			return -EINVAL;
	}

	if (info->rate.denom == 0)
		return choices->rate_choice == SPA_CHOICE_None &&
			choices->n_rate_values == 0 ? 0 : -EINVAL;
	for (i = 0; i < choices->n_rate_values; i++) {
		if (!spa_ndarray_rate_valid(&choices->rate_values[i]))
			return -EINVAL;
	}
	switch (choices->rate_choice) {
	case SPA_CHOICE_None:
		return choices->n_rate_values == 0 ? 0 : -EINVAL;
	case SPA_CHOICE_Enum:
		return choices->n_rate_values > 0 ? 0 : -EINVAL;
	case SPA_CHOICE_Range:
		if (choices->n_rate_values != 2)
			return -EINVAL;
		break;
	case SPA_CHOICE_Step:
		if (choices->n_rate_values != 3)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	if (spa_ndarray_rate_compare(&choices->rate_values[0], &info->rate) > 0 ||
	    spa_ndarray_rate_compare(&info->rate, &choices->rate_values[1]) > 0)
		return -EINVAL;
	return 0;
}

static inline void spa_ndarray_build_id(struct spa_pod_builder *builder,
		uint32_t key, uint32_t value, uint32_t n_values, const void *values)
{
	struct spa_pod_frame choice;
	uint32_t i;

	spa_pod_builder_prop(builder, key, 0);
	if (n_values == 0) {
		spa_pod_builder_id(builder, value);
		return;
	}
	spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
	spa_pod_builder_id(builder, value);
	spa_pod_builder_id(builder, value);
	for (i = 0; i < n_values; i++) {
		uint32_t id;

		memcpy(&id, SPA_PTROFF(values, i * sizeof(id), const void), sizeof(id));
		spa_pod_builder_id(builder, id);
	}
	spa_pod_builder_pop(builder, &choice);
}

static inline void spa_ndarray_build_rate(struct spa_pod_builder *builder,
		const struct spa_fraction *rate, const struct spa_ndarray_choices *choices)
{
	struct spa_pod_frame choice;
	uint32_t i;

	spa_pod_builder_prop(builder, SPA_FORMAT_NDARRAY_rate, 0);
	if (choices == NULL || choices->rate_choice == SPA_CHOICE_None) {
		spa_pod_builder_fraction(builder, rate->num, rate->denom);
		return;
	}
	spa_pod_builder_push_choice(builder, &choice, choices->rate_choice, 0);
	spa_pod_builder_fraction(builder, rate->num, rate->denom);
	if (choices->rate_choice == SPA_CHOICE_Enum)
		spa_pod_builder_fraction(builder, rate->num, rate->denom);
	for (i = 0; i < choices->n_rate_values; i++)
		spa_pod_builder_fraction(builder, choices->rate_values[i].num,
				choices->rate_values[i].denom);
	spa_pod_builder_pop(builder, &choice);
}

SPA_API_NDARRAY_UTILS struct spa_pod *
spa_format_ndarray_ext_build_choices(struct spa_pod_builder *builder, uint32_t id,
		const struct spa_ndarray_info *info, size_t size,
		const struct spa_ndarray_choices *choices)
{
	struct spa_pod_frame object;
	uint32_t n_element_types = choices == NULL ? 0 : choices->n_element_types;
	uint32_t n_layouts = choices == NULL ? 0 : choices->n_layouts;
	const void *element_types = choices == NULL ? NULL : choices->element_types;
	const void *layouts = choices == NULL ? NULL : choices->layouts;
	int res;

	if (builder == NULL ||
	    (res = spa_ndarray_choices_validate(info, size, choices)) < 0) {
		errno = builder == NULL ? EINVAL : -res;
		return NULL;
	}

	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			0);
	spa_ndarray_build_id(builder, SPA_FORMAT_NDARRAY_elementType,
			info->element_type, n_element_types, element_types);
	spa_pod_builder_add(builder, SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				info->n_dimensions, info->shape), 0);
	spa_ndarray_build_id(builder, SPA_FORMAT_NDARRAY_layout,
			info->layout, n_layouts, layouts);
	if (info->rate.denom != 0)
		spa_ndarray_build_rate(builder, &info->rate, choices);
	return (struct spa_pod *)spa_pod_builder_pop(builder, &object);
}

SPA_API_NDARRAY_UTILS struct spa_pod *
spa_format_ndarray_build_choices(struct spa_pod_builder *builder, uint32_t id,
		const struct spa_ndarray_info *info,
		const struct spa_ndarray_choices *choices)
{
	return spa_format_ndarray_ext_build_choices(builder, id, info, sizeof(*info), choices);
}

SPA_API_NDARRAY_UTILS struct spa_pod *
spa_format_ndarray_ext_build(struct spa_pod_builder *builder, uint32_t id,
		const struct spa_ndarray_info *info, size_t size)
{
	return spa_format_ndarray_ext_build_choices(builder, id, info, size, NULL);
}

SPA_API_NDARRAY_UTILS struct spa_pod *
spa_format_ndarray_build(struct spa_pod_builder *builder, uint32_t id,
		const struct spa_ndarray_info *info)
{
	return spa_format_ndarray_ext_build(builder, id, info, sizeof(*info));
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_PARAM_NDARRAY_UTILS_H */
