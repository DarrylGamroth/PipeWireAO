/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_META_H
#define SPA_META_H

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_META
 #ifdef SPA_API_IMPL
  #define SPA_API_META SPA_API_IMPL
 #else
  #define SPA_API_META static inline
 #endif
#endif

/**
 * \addtogroup spa_buffer
 * \{
 */

enum spa_meta_type {
	SPA_META_Invalid,
	SPA_META_Header,		/**< struct spa_meta_header */
	SPA_META_VideoCrop,		/**< struct spa_meta_region with cropping data */
	SPA_META_VideoDamage,		/**< array of struct spa_meta_region with damage, where an invalid entry or end-of-array marks the end. */
	SPA_META_Bitmap,		/**< struct spa_meta_bitmap */
	SPA_META_Cursor,		/**< struct spa_meta_cursor */
	SPA_META_Control,		/**< metadata contains a spa_meta_control
					  *  associated with the data */
	SPA_META_Busy,			/**< don't write to buffer when count > 0 */
	SPA_META_VideoTransform,	/**< struct spa_meta_transform */
	SPA_META_SyncTimeline,		/**< struct spa_meta_sync_timeline */

	/* IDs 11, 12 and 13 are retired PipeWireAO metadata layouts. */
	_SPA_META_LAST = 10,
	SPA_META_START_custom = 0x200,
	/* PipeWireAO custom metadata registry; incompatible layouts need new IDs. */
	SPA_META_START_PipeWireAO = 0x300,
	SPA_META_AcquisitionIdentity = 0x300,
	SPA_META_ExposureTiming = 0x301,
	/* 0x302: retired absolute event array. */
	SPA_META_OpticalChopping = 0x303,
	SPA_META_ElectricalTrigger = 0x304,
	SPA_META_ReferenceTimestamp = 0x305,

	SPA_META_START_features		= 0x10000,	/* features start, these have 0 size, the
							 * type in the upper 16 bits and a bitmask in
							 * the lower 16 bits with type specific features. */
};

#define SPA_META_TYPE_FEATURES(type,features)	(((type)<<16)|(features))

/**
 * A metadata element.
 *
 * This structure is available on the buffer structure and contains
 * the type of the metadata and a pointer/size to the actual metadata
 * itself.
 */
struct spa_meta {
	uint32_t type;		/**< metadata type, one of enum spa_meta_type */
	uint32_t size;		/**< size of metadata */
	void *data;		/**< pointer to metadata */
};

SPA_API_META void *spa_meta_first(const struct spa_meta *m) {
	return m->data;
}

SPA_API_META void *spa_meta_end(const struct spa_meta *m) {
	return SPA_PTROFF(m->data,m->size,void);
}
#define spa_meta_check(p,m)	(SPA_PTROFF(p,sizeof(*(p)),void) <= spa_meta_end(m))

/**
 * Describes essential buffer header metadata such as flags and
 * timestamps.
 */
struct spa_meta_header {
#define SPA_META_HEADER_FLAG_DISCONT	(1 << 0)	/**< data is not continuous with previous buffer */
#define SPA_META_HEADER_FLAG_CORRUPTED	(1 << 1)	/**< data might be corrupted */
#define SPA_META_HEADER_FLAG_MARKER	(1 << 2)	/**< media specific marker */
#define SPA_META_HEADER_FLAG_HEADER	(1 << 3)	/**< data contains a codec specific header */
#define SPA_META_HEADER_FLAG_GAP	(1 << 4)	/**< data contains media neutral data */
#define SPA_META_HEADER_FLAG_DELTA_UNIT	(1 << 5)	/**< cannot be decoded independently */
	uint32_t flags;				/**< flags */
	uint32_t offset;			/**< offset in current cycle */
	int64_t pts;				/**< presentation timestamp in nanoseconds */
	int64_t dts_offset;			/**< decoding timestamp as a difference with pts */
	uint64_t seq;				/**< sequence number, increments with a
						  *  media specific frequency */
};

/** metadata structure for Region or an array of these for RegionArray */
struct spa_meta_region {
	struct spa_region region;
};

SPA_API_META bool spa_meta_region_is_valid(const struct spa_meta_region *m) {
	return m->region.size.width != 0 && m->region.size.height != 0;
}

/** iterate all the items in a metadata */
#define spa_meta_for_each(pos,meta)					\
	for ((pos) = (__typeof(pos))spa_meta_first(meta);		\
	    spa_meta_check(pos, meta);					\
            (pos)++)

/**
 * Bitmap information
 *
 * This metadata contains a bitmap image in the given format and size.
 * It is typically used for cursor images or other small images that are
 * better transferred inline.
 */
struct spa_meta_bitmap {
	uint32_t format;		/**< bitmap video format, one of enum spa_video_format. 0 is
					  *  and invalid format and should be handled as if there is
					  *  no new bitmap information. */
	struct spa_rectangle size;	/**< width and height of bitmap */
	int32_t stride;			/**< stride of bitmap data */
	uint32_t offset;		/**< offset of bitmap data in this structure. An offset of
					  *  0 means no image data (invisible), an offset >=
					  *  sizeof(struct spa_meta_bitmap) contains valid bitmap
					  *  info. */
};

SPA_API_META bool spa_meta_bitmap_is_valid(const struct spa_meta_bitmap *m) {
	return m->format != 0;
}

/**
 * Cursor information
 *
 * Metadata to describe the position and appearance of a pointing device.
 */
struct spa_meta_cursor {
	uint32_t id;			/**< cursor id. an id of 0 is an invalid id and means that
					  *  there is no new cursor data */
	uint32_t flags;			/**< extra flags */
	struct spa_point position;	/**< position on screen */
	struct spa_point hotspot;	/**< offsets for hotspot in bitmap, this field has no meaning
					  *  when there is no valid bitmap (see below) */
	uint32_t bitmap_offset;		/**< offset of bitmap meta in this structure. When the offset
					  *  is 0, there is no new bitmap information. When the offset is
					  *  >= sizeof(struct spa_meta_cursor) there is a
					  *  struct spa_meta_bitmap at the offset. */
};

SPA_API_META bool spa_meta_cursor_is_valid(const struct spa_meta_cursor *m) {
	return m->id != 0;
}

/** a timed set of events associated with the buffer */
struct spa_meta_control {
	struct spa_pod_sequence sequence;
};

/** a busy counter for the buffer */
struct spa_meta_busy {
	uint32_t flags;
	uint32_t count;			/**< number of users busy with the buffer */
};

enum spa_meta_videotransform_value {
	SPA_META_TRANSFORMATION_None = 0,	/**< no transform */
	SPA_META_TRANSFORMATION_90,		/**< 90 degree counter-clockwise */
	SPA_META_TRANSFORMATION_180,		/**< 180 degree counter-clockwise */
	SPA_META_TRANSFORMATION_270,		/**< 270 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped,	/**< 180 degree flipped around the vertical axis. Equivalent
						  * to a reflexion through the vertical line splitting the
						  * buffer in two equal sized parts */
	SPA_META_TRANSFORMATION_Flipped90,	/**< flip then rotate around 90 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped180,	/**< flip then rotate around 180 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped270,	/**< flip then rotate around 270 degree counter-clockwise */
};

/** a transformation of the buffer */
struct spa_meta_videotransform {
	uint32_t transform;			/**< orientation transformation that was applied to the buffer,
						  *  one of enum spa_meta_videotransform_value */
};

/**
 * A timeline point for explicit sync
 *
 * Metadata to describe the time on the timeline when the buffer
 * can be acquired and when it can be reused.
 *
 * This metadata will require negotiation of 2 extra fds for the acquire
 * and release timelines respectively.  One way to achieve this is to place
 * this metadata as SPA_PARAM_BUFFERS_metaType when negotiating a buffer
 * layout with 2 extra fds.
 */
#define SPA_META_FEATURE_SYNC_TIMELINE_RELEASE	(1<<0)	/**< metadata supports RELEASE */

struct spa_meta_sync_timeline {
#define SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE	(1<<0)	/**< this flag is set by the producer and cleared
								  *  by the consumer when it promises to signal
								  *  the release point */
	uint32_t flags;
	uint32_t padding;
	uint64_t acquire_point;			/**< the timeline acquire point, this is when the data
						  *  can be accessed. */
	uint64_t release_point;			/**< the timeline release point, this timeline point should
						  *  be signaled when the data is no longer accessed. */
};

/** Independent PipeWireAO records. Native shared memory, never a wire encoding. */
#define SPA_META_MAX_RECORDS 64u
#define SPA_META_MAX_BYTES 4096u
#define SPA_ACQUISITION_DOMAIN_SIZE 16u

enum spa_meta_record_flag {
	SPA_META_RECORD_VALID = (1u << 0),
};
enum spa_meta_exposure_flag {
	SPA_META_EXPOSURE_START_VALID = (1u << 0),
	SPA_META_EXPOSURE_DURATION_VALID = (1u << 1),
};
enum spa_meta_clock {
	SPA_META_CLOCK_MONOTONIC = 1u,
	SPA_META_CLOCK_REALTIME = 2u,
	SPA_META_CLOCK_TAI = 3u,
	SPA_META_CLOCK_MODEL = 4u,
};
enum spa_meta_optical_classification {
	SPA_META_OPTICAL_ABSENT = 0u,
	SPA_META_OPTICAL_UNFRINGED = 1u,
	SPA_META_OPTICAL_FRINGED = 2u,
	SPA_META_OPTICAL_MIXED = 3u,
};
enum spa_meta_trigger_classification {
	SPA_META_TRIGGER_ABSENT = 0u,
	SPA_META_TRIGGER_LOW = 1u,
	SPA_META_TRIGGER_HIGH = 2u,
	SPA_META_TRIGGER_MIXED = 3u,
};
struct spa_meta_acquisition_identity {
	uint32_t flags;
	uint32_t reserved;
	uint8_t domain[16];
	uint64_t generation;
	uint64_t sequence;
};
/** start_nsec denotes the start of [start, start + duration), in clock units ns. */
struct spa_meta_exposure_timing {
	uint32_t flags;
	uint32_t clock;
	int64_t start_nsec;
	uint64_t duration_nsec;
};
/** One sample-associated timestamp; clock labels do not assert synchronization.
 * UINT64_MAX denotes unspecified duration and is not a valid timestamp. */
struct spa_meta_reference_timestamp {
	uint32_t clock;
	uint32_t reserved;
	uint64_t timestamp_nsec;
	uint64_t duration_nsec;
};
struct spa_meta_optical_chopping {
	uint32_t flags;
	uint32_t classification;
} SPA_ALIGNED(8);
struct spa_meta_electrical_trigger {
	uint32_t flags;
	uint32_t classification;
} SPA_ALIGNED(8);

enum spa_meta_requirement_flag { SPA_META_REQUIREMENT_REQUIRED = (1u << 0) };
struct spa_meta_requirement {
	uint32_t type;
	uint32_t size;
	uint32_t flags;
	uint32_t reserved;
};
enum spa_meta_propagation {
	SPA_META_PROPAGATION_PRODUCE = 0,
	SPA_META_PROPAGATION_PRESERVE = 1,
	SPA_META_PROPAGATION_REPLACE = 2,
	SPA_META_PROPAGATION_DROP = 3,
};

SPA_STATIC_ASSERT(sizeof(struct spa_meta_acquisition_identity) == 40, "identity ABI");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition_identity, generation) == 24,
		  "identity generation ABI");
SPA_STATIC_ASSERT(sizeof(struct spa_meta_exposure_timing) == 24, "exposure ABI");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_exposure_timing, start_nsec) == 8,
		  "exposure start ABI");
SPA_STATIC_ASSERT(sizeof(struct spa_meta_reference_timestamp) == 24, "reference timestamp ABI");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_reference_timestamp, timestamp_nsec) == 8,
		  "reference timestamp offset ABI");
SPA_STATIC_ASSERT(sizeof(struct spa_meta_optical_chopping) == 8, "optical ABI");
SPA_STATIC_ASSERT(sizeof(struct spa_meta_electrical_trigger) == 8, "trigger ABI");

SPA_API_META bool spa_meta_bytes_zero(const void *data, uint32_t size)
{
	const uint8_t *bytes = (const uint8_t *)data;
	uint8_t value = 0;
	for (uint32_t i = 0; i < size; i++)
		value |= bytes[i];
	return value == 0;
}

/** Mutable upstream transport state and allocation capabilities are not fact records. */
SPA_API_META bool spa_meta_type_is_transport_owned(uint32_t type)
{
	return type == SPA_META_Busy || type == SPA_META_SyncTimeline ||
	       type >= SPA_META_START_features;
}
SPA_API_META uint32_t spa_meta_record_size(uint32_t type)
{
	switch (type) {
	case SPA_META_Header:
		return sizeof(struct spa_meta_header);
	case SPA_META_AcquisitionIdentity:
		return sizeof(struct spa_meta_acquisition_identity);
	case SPA_META_ExposureTiming:
		return sizeof(struct spa_meta_exposure_timing);
	case SPA_META_ReferenceTimestamp:
		return sizeof(struct spa_meta_reference_timestamp);
	case SPA_META_OpticalChopping:
		return sizeof(struct spa_meta_optical_chopping);
	case SPA_META_ElectricalTrigger:
		return sizeof(struct spa_meta_electrical_trigger);
	default:
		return 0;
	}
}
/** Pure cold layout check; the caller supplies its profile's finite byte limit. */
SPA_API_META bool spa_meta_record_layout_validate(uint32_t type, uint32_t size, uint32_t max_bytes)
{
	if (type == SPA_META_Invalid || type == 11 || type == 12 || type == 13 ||
	    type == 0x302 || size > max_bytes)
		return false;
	if (type >= SPA_META_START_features)
		return size == 0 && !((type >> 16) >= 11 && (type >> 16) <= 13) &&
		       (type >> 16) != 0x302;
	uint32_t expected = spa_meta_record_size(type);
	return expected == 0 || expected == size;
}
SPA_API_META struct spa_meta *spa_meta_records_find(struct spa_meta *metas, uint32_t count,
						    uint32_t type)
{
	for (uint32_t i = 0; i < count; i++)
		if (metas[i].type == type)
			return &metas[i];
	return NULL;
}
/** Validate layout and understood semantics. require_valid additionally rejects unavailable facts.
 */
SPA_API_META bool spa_meta_record_validate(const struct spa_meta *meta, bool require_valid)
{
	if (meta == NULL ||
	    !spa_meta_record_layout_validate(meta->type, meta->size, SPA_META_MAX_BYTES) ||
	    (meta->size && (meta->data == NULL || !SPA_IS_ALIGNED(meta->data, 8))))
		return false;
	uint32_t size = spa_meta_record_size(meta->type);
	if (size == 0)
		return !require_valid; /* opaque optional payload */
	if (meta->type == SPA_META_AcquisitionIdentity) {
		const struct spa_meta_acquisition_identity *m =
		    (const struct spa_meta_acquisition_identity *)meta->data;
		if (m->flags == 0)
			return !require_valid && spa_meta_bytes_zero(m, sizeof(*m));
		return m->flags == SPA_META_RECORD_VALID && m->reserved == 0 &&
		       !spa_meta_bytes_zero(m->domain, 16);
	} else if (meta->type == SPA_META_ExposureTiming) {
		const struct spa_meta_exposure_timing *m =
		    (const struct spa_meta_exposure_timing *)meta->data;
		if (m->flags == 0)
			return !require_valid && spa_meta_bytes_zero(m, sizeof(*m));
		if (m->flags & ~3u)
			return false;
		if (!(m->flags & SPA_META_EXPOSURE_START_VALID))
			return m->flags == SPA_META_EXPOSURE_DURATION_VALID &&
			       m->duration_nsec > 0 && m->clock == 0 && m->start_nsec == 0;
		return m->clock >= SPA_META_CLOCK_MONOTONIC && m->clock <= SPA_META_CLOCK_MODEL &&
		       ((m->flags & SPA_META_EXPOSURE_DURATION_VALID) ? m->duration_nsec > 0
								      : m->duration_nsec == 0);
	} else if (meta->type == SPA_META_ReferenceTimestamp) {
		const struct spa_meta_reference_timestamp *m =
		    (const struct spa_meta_reference_timestamp *)meta->data;
		if (m->clock == 0)
			return !require_valid && spa_meta_bytes_zero(m, sizeof(*m));
		return m->clock >= SPA_META_CLOCK_MONOTONIC && m->clock <= SPA_META_CLOCK_MODEL &&
		       m->reserved == 0 && m->timestamp_nsec != UINT64_MAX;
	} else if (meta->type == SPA_META_OpticalChopping ||
		   meta->type == SPA_META_ElectricalTrigger) {
		const uint32_t *m = (const uint32_t *)meta->data;
		return m[0] == SPA_META_RECORD_VALID ? m[1] <= 3
						     : (!require_valid && m[0] == 0 && m[1] == 0);
	}
	return true;
}
SPA_API_META bool spa_meta_record_init(struct spa_meta *meta)
{
	if (meta == NULL ||
	    !spa_meta_record_layout_validate(meta->type, meta->size, SPA_META_MAX_BYTES) ||
	    (meta->size && (meta->data == NULL || !SPA_IS_ALIGNED(meta->data, 8))))
		return false;
	if (spa_meta_type_is_transport_owned(meta->type))
		return true;
	if (meta->size)
		memset(meta->data, 0, meta->size);
	return true;
}
/** Validate only immutable storage; reused output bytes may be stale. */
SPA_API_META bool spa_meta_records_layout_validate(const struct spa_meta *metas, uint32_t count)
{
	uint32_t bytes = 0;
	if (count > SPA_META_MAX_RECORDS ||
	    (count && (metas == NULL || !SPA_IS_ALIGNED(metas, SPA_ALIGNOF(struct spa_meta)))))
		return false;
	for (uint32_t i = 0; i < count; i++) {
		const struct spa_meta *m = &metas[i];
		if (!spa_meta_record_layout_validate(m->type, m->size, SPA_META_MAX_BYTES) ||
		    (m->size && (m->data == NULL || !SPA_IS_ALIGNED(m->data, 8))) ||
		    ((m->size + 7u) & ~7u) > SPA_META_MAX_BYTES - bytes)
			return false;
		bytes += ((m->size + 7u) & ~7u);
		uintptr_t start = (uintptr_t)m->data, descriptors = (uintptr_t)metas;
		if (m->size && (start > UINTPTR_MAX - m->size ||
				descriptors > UINTPTR_MAX - count * sizeof(*metas) ||
				(start < descriptors + count * sizeof(*metas) &&
				 descriptors < start + m->size)))
			return false;
		for (uint32_t j = 0; j < i; j++) {
			if (m->type == metas[j].type)
				return false;
			uintptr_t other = (uintptr_t)metas[j].data;
			if (m->size && metas[j].size && start < other + metas[j].size &&
			    other < start + m->size)
				return false;
		}
	}
	return true;
}
/** Envelope validation is independent of the set of understood metadata types. */
SPA_API_META bool spa_meta_records_validate(const struct spa_meta *metas, uint32_t count,
					    const struct spa_meta_requirement *requirements,
					    uint32_t n_requirements)
{
	if (!spa_meta_records_layout_validate(metas, count) ||
	    n_requirements > SPA_META_MAX_RECORDS || (count && metas == NULL) ||
	    (n_requirements &&
	     (requirements == NULL ||
	      !SPA_IS_ALIGNED(requirements, SPA_ALIGNOF(struct spa_meta_requirement)))))
		return false;
	for (uint32_t i = 0; i < count; i++)
		if (!spa_meta_record_validate(&metas[i], false))
			return false;
	for (uint32_t i = 0; i < n_requirements; i++) {
		const struct spa_meta_requirement *r = &requirements[i];
		const struct spa_meta *m = NULL;
		if (r->reserved || r->flags & ~SPA_META_REQUIREMENT_REQUIRED ||
		    !spa_meta_record_layout_validate(r->type, r->size, SPA_META_MAX_BYTES))
			return false;
		for (uint32_t j = 0; j < i; j++)
			if (requirements[j].type == r->type)
				return false;
		for (uint32_t j = 0; j < count; j++)
			if (metas[j].type == r->type)
				m = &metas[j];
		if (m == NULL) {
			if (r->flags & SPA_META_REQUIREMENT_REQUIRED)
				return false;
			continue;
		}
		if (m->size != r->size ||
		    ((r->flags & SPA_META_REQUIREMENT_REQUIRED) && spa_meta_record_size(r->type) &&
		     !spa_meta_record_validate(m, true)))
			return false;
	}
	return true;
}
/** Copy accepted fact records; each pool retains its transport-owned state. Preflight before
 * writes. */
SPA_API_META bool spa_meta_records_copy(struct spa_meta *dst, uint32_t n_dst,
					const struct spa_meta *src, uint32_t n_src)
{
	if (!spa_meta_records_validate(src, n_src, NULL, 0) ||
	    !spa_meta_records_layout_validate(dst, n_dst))
		return false;
	if (dst == src)
		return n_dst == n_src;
	for (uint32_t i = 0; i < n_src; i++) {
		if (spa_meta_type_is_transport_owned(src[i].type))
			continue;
		struct spa_meta *d = spa_meta_records_find(dst, n_dst, src[i].type);
		if (d == NULL || d->size != src[i].size ||
		    (d->size && (d->data == NULL || !SPA_IS_ALIGNED(d->data, 8))))
			return false;
	}
	for (uint32_t j = 0; j < n_dst; j++) {
		if (spa_meta_type_is_transport_owned(dst[j].type))
			continue;
		uintptr_t start = (uintptr_t)dst[j].data, descriptors = (uintptr_t)src;
		if (dst[j].size && start < descriptors + n_src * sizeof(*src) &&
		    descriptors < start + dst[j].size)
			return false;
	}
	for (uint32_t i = 0; i < n_src; i++)
		for (uint32_t j = 0; j < n_dst; j++) {
			if (spa_meta_type_is_transport_owned(src[i].type) &&
			    spa_meta_type_is_transport_owned(dst[j].type))
				continue;
			uintptr_t a = (uintptr_t)src[i].data, b = (uintptr_t)dst[j].data;
			if (src[i].size && dst[j].size &&
			    (a > UINTPTR_MAX - src[i].size || b > UINTPTR_MAX - dst[j].size ||
			     (a < b + dst[j].size && b < a + src[i].size)))
				return false;
		}
	for (uint32_t i = 0; i < n_dst; i++)
		if (!spa_meta_record_init(&dst[i]))
			return false;
	for (uint32_t i = 0; i < n_src; i++) {
		if (spa_meta_type_is_transport_owned(src[i].type))
			continue;
		struct spa_meta *d = spa_meta_records_find(dst, n_dst, src[i].type);
		if (d->size)
			memcpy(d->data, src[i].data, d->size);
	}
	return true;
}
SPA_API_META bool spa_acquisition_identity_equal(const struct spa_meta_acquisition_identity *a,
						 const struct spa_meta_acquisition_identity *b)
{
	return a != NULL && b != NULL && a->flags == SPA_META_RECORD_VALID &&
	       b->flags == SPA_META_RECORD_VALID && a->reserved == 0 && b->reserved == 0 &&
	       !spa_meta_bytes_zero(a->domain, 16) && a->generation == b->generation &&
	       a->sequence == b->sequence && memcmp(a->domain, b->domain, 16) == 0;
}

/** \} */
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* SPA_META_H */
