/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_NDARRAY_H
#define SPA_PARAM_NDARRAY_H

#include <stddef.h>
#include <stdint.h>

#include <spa/param/format.h>
#include <spa/pod/pod.h>
#include <spa/utils/type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/** Number of dimensions available in the ordinary stack-allocated structure. */
#ifndef SPA_NDARRAY_MAX_DIMENSIONS
#define SPA_NDARRAY_MAX_DIMENSIONS 64u
#endif

/** Description of one fixed, packed ndarray format.
 *
 * Callers that need more than \ref SPA_NDARRAY_MAX_DIMENSIONS may allocate a
 * larger object and use the sized helper functions. Padding after `shape`
 * contains the additional dimensions, as with `spa_audio_info_raw.position`.
 * A rate with a zero denominator is absent; a present rate is strictly
 * positive.
 */
struct spa_ndarray_info {
	enum spa_element_type element_type;
	enum spa_ndarray_layout layout;
	struct spa_fraction rate;
	uint32_t n_dimensions;
	uint32_t shape[SPA_NDARRAY_MAX_DIMENSIONS];
	/* padding follows here when n_dimensions > SPA_NDARRAY_MAX_DIMENSIONS */
};

#define SPA_NDARRAY_INFO_INIT(...) ((struct spa_ndarray_info) { __VA_ARGS__ })

#define SPA_NDARRAY_INFO_MAX_DIMENSIONS(size) \
	(((size) - offsetof(struct spa_ndarray_info, shape)) / sizeof(uint32_t))

#define SPA_NDARRAY_INFO_VALID_SIZE(size) \
	((size) >= offsetof(struct spa_ndarray_info, shape))

/** Optional alternatives for an enumerated ndarray format.
 *
 * The fixed values in `spa_ndarray_info` are the defaults. Element-type and
 * layout arrays contain additional values for an ordinary SPA Enum Choice;
 * the builder repeats the default in the admissible list as SPA requires.
 * Rate values follow the default and are interpreted according to
 * `rate_choice`: Enum values are additional alternatives (the builder repeats
 * the default), Range values are `[min,max]`,
 * and Step values are `[min,max,step]`. SPA_CHOICE_None requires no values.
 * Shape is deliberately not a choice; offer a separate format POD for each
 * supported exact shape.
 */
struct spa_ndarray_choices {
	uint32_t n_element_types;
	const enum spa_element_type *element_types;
	uint32_t n_layouts;
	const enum spa_ndarray_layout *layouts;
	uint32_t rate_choice;
	uint32_t n_rate_values;
	const struct spa_fraction *rate_values;
};

#define SPA_NDARRAY_CHOICES_INIT(...) ((struct spa_ndarray_choices) { __VA_ARGS__ })

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_PARAM_NDARRAY_H */
