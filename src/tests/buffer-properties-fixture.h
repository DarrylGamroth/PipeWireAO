/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */
#ifndef TEST_BUFFER_PROPERTIES_FIXTURE_H
#define TEST_BUFFER_PROPERTIES_FIXTURE_H
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>

/* Append complete monotonic facts after any classification keys. */
static inline bool test_add_acquisition(struct spa_meta *meta, uint64_t sequence, int64_t start)
{
	const uint8_t domain[16] = { 1 };
	const uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY;
	const uint32_t timebase = SPA_EXPOSURE_TIMEBASE_MONOTONIC;
	return spa_meta_properties_add(meta, 3, flags, SPA_TYPE_Bytes, sizeof(domain), domain) &&
		spa_meta_properties_add_uint64(meta, 4, flags, 7) &&
		spa_meta_properties_add_uint64(meta, 5, flags, sequence) &&
		spa_meta_properties_add(meta, 7, flags, SPA_TYPE_Long, sizeof(start), &start) &&
		spa_meta_properties_add(meta, 8, flags, SPA_TYPE_Id, sizeof(timebase), &timebase) &&
		spa_meta_properties_add_uint64(meta, 9, flags, 50);
}

static inline bool test_check_acquisition(const struct spa_meta *meta, uint64_t sequence, int64_t start)
{
	struct spa_acquisition_identity identity;
	struct spa_exposure_start exposure;
	return spa_meta_properties_get_acquisition_identity(meta, &identity) &&
		identity.domain[0] == 1 && identity.generation == 7 && identity.sequence == sequence &&
		spa_meta_properties_get_exposure_start(meta, &exposure) && exposure.nsec == start &&
		exposure.uncertainty_nsec == 50 && exposure.timebase == SPA_EXPOSURE_TIMEBASE_MONOTONIC;
}
static inline bool test_build_frame_metadata(struct spa_pod_builder *builder, struct spa_pod **params)
{
	struct spa_pod_frame frame;
	params[0] = spa_pod_builder_add_object(builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(builder, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Properties),
		SPA_PARAM_META_size, SPA_POD_Int(512), 0);
	spa_pod_builder_prop(builder, SPA_PARAM_META_features, SPA_POD_PROP_FLAG_MANDATORY);
	spa_pod_builder_int(builder, SPA_META_FEATURE_PROPERTIES_VERSION_1);
	params[1] = spa_pod_builder_pop(builder, &frame);
	return params[0] != NULL && params[1] != NULL;
}

static inline bool test_init_frame_metadata(struct spa_buffer *buffer)
{
	struct spa_meta_header *header = spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(*header));
	if (header == NULL)
		return false;
	*header = (struct spa_meta_header) { .pts = SPA_TIME_INVALID };
	return spa_meta_properties_init(spa_buffer_find_meta(buffer, SPA_META_Properties));
}
#endif
