/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */
#ifndef TEST_METADATA_FIXTURE_H
#define TEST_METADATA_FIXTURE_H
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#define TEST_OPAQUE_META 0x3f0u
static const struct spa_meta_requirement test_metadata[] = {
    {SPA_META_AcquisitionIdentity, sizeof(struct spa_meta_acquisition_identity),
     SPA_META_REQUIREMENT_REQUIRED, 0},
    {SPA_META_ExposureTiming, sizeof(struct spa_meta_exposure_timing),
     SPA_META_REQUIREMENT_REQUIRED, 0},
    {SPA_META_AbsoluteTime, sizeof(struct spa_meta_absolute_time), SPA_META_REQUIREMENT_REQUIRED,
     0},
    {SPA_META_OpticalChopping, sizeof(struct spa_meta_optical_chopping), 0, 0},
    {SPA_META_ElectricalTrigger, sizeof(struct spa_meta_electrical_trigger), 0, 0},
    {TEST_OPAQUE_META, 8, 0, 0},
};
#ifdef PIPEWIRE_NDARRAY_FILTER_H
static inline struct spa_meta_header *test_header(const struct pw_ndarray_filter_buffer *view)
{
	struct spa_meta *m = spa_meta_records_find(view->metas, view->n_metas, SPA_META_Header);
	return m ? m->data : NULL;
}
#endif
static inline bool test_add_acquisition(struct spa_meta *metas, uint32_t count, uint64_t sequence,
					int64_t start)
{
	struct spa_meta *identity =
	    spa_meta_records_find(metas, count, SPA_META_AcquisitionIdentity);
	struct spa_meta *timing = spa_meta_records_find(metas, count, SPA_META_ExposureTiming);
	struct spa_meta *time = spa_meta_records_find(metas, count, SPA_META_AbsoluteTime);
	if (!identity || !timing || !time)
		return false;
	*(struct spa_meta_acquisition_identity *)identity->data =
	    (struct spa_meta_acquisition_identity){.flags = SPA_META_RECORD_VALID,
						   .domain = {1},
						   .generation = 7,
						   .sequence = sequence};
	*(struct spa_meta_exposure_timing *)timing->data = (struct spa_meta_exposure_timing){
	    .flags = SPA_META_EXPOSURE_START_VALID | SPA_META_EXPOSURE_DURATION_VALID |
		     SPA_META_EXPOSURE_UNCERTAINTY,
	    .clock = SPA_META_CLOCK_MONOTONIC,
	    .clock_domain = {2},
	    .start_nsec = start,
	    .duration_nsec = 100,
	    .uncertainty_nsec = 50};
	*(struct spa_meta_absolute_time *)time->data = (struct spa_meta_absolute_time){
	    .count = 1,
	    .events = {{.seconds = 1700000000,
			.nanoseconds = (uint32_t)sequence,
			.clock = sequence & 1 ? SPA_META_CLOCK_REALTIME : SPA_META_CLOCK_TAI,
			.event = SPA_META_TIME_EVENT_ACQUISITION,
			.flags = SPA_META_RECORD_VALID,
			.origin = {1},
			.clock_domain = {2}}}};
	return true;
}
static inline bool test_check_acquisition(struct spa_meta *metas, uint32_t count, uint64_t sequence,
					  int64_t start)
{
	struct spa_meta *identity =
	    spa_meta_records_find(metas, count, SPA_META_AcquisitionIdentity);
	struct spa_meta *timing = spa_meta_records_find(metas, count, SPA_META_ExposureTiming);
	struct spa_meta *time = spa_meta_records_find(metas, count, SPA_META_AbsoluteTime);
	if (!identity || !timing || !time ||
	    !spa_meta_records_validate(metas, count, test_metadata, SPA_N_ELEMENTS(test_metadata)))
		return false;
	const struct spa_meta_acquisition_identity *id = identity->data;
	const struct spa_meta_exposure_timing *t = timing->data;
	const struct spa_meta_absolute_time *a = time->data;
	return id->domain[0] == 1 && id->generation == 7 && id->sequence == sequence &&
	       t->start_nsec == start && t->uncertainty_nsec == 50 &&
	       t->clock == SPA_META_CLOCK_MONOTONIC && a->count == 1 &&
	       a->events[0].seconds == 1700000000 && a->events[0].nanoseconds == sequence &&
	       a->events[0].event == SPA_META_TIME_EVENT_ACQUISITION;
}
static inline uint32_t test_build_metadata(struct spa_pod_builder *b, struct spa_pod **params)
{
	uint32_t count = 0;
	params[count++] = spa_pod_builder_add_object(
	    b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
	    SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
	    SPA_POD_Int(sizeof(struct spa_meta_header)), SPA_PARAM_META_required,
	    SPA_POD_Bool(true));
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(test_metadata); i++)
		params[count++] = spa_pod_builder_add_object(
		    b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
		    SPA_POD_Id(test_metadata[i].type), SPA_PARAM_META_size,
		    SPA_POD_Int(test_metadata[i].size), SPA_PARAM_META_required,
		    SPA_POD_Bool(test_metadata[i].flags & SPA_META_REQUIREMENT_REQUIRED));
	return count;
}
/* Generic graph profiles in lifecycle tests require Header only. */
static inline bool test_build_frame_metadata(struct spa_pod_builder *b, struct spa_pod **params)
{
	params[0] = spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
					       SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					       SPA_PARAM_META_size,
					       SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[1] = spa_pod_builder_add_object(
	    b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
	    SPA_POD_CHOICE_RANGE_Int(4, 2, 64), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1));
	return params[0] && params[1];
}
static inline bool test_init_frame_metadata(struct spa_buffer *buffer)
{
	for (uint32_t i = 0; i < buffer->n_metas; i++)
		if (!spa_meta_record_init(&buffer->metas[i]))
			return false;
	return spa_buffer_find_meta(buffer, SPA_META_Header) != NULL;
}
#endif
