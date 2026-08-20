/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_META_H
#define SPA_META_H

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

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
	SPA_META_Progressive,		/**< struct spa_meta_progressive */
	SPA_META_Acquisition,		/**< struct spa_meta_acquisition */
	_SPA_META_LAST,			/**< not part of ABI/API */

	SPA_META_START_custom		= 0x200,

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

/** Version 1 acquisition metadata ABI. */
enum spa_meta_acquisition_constant {
	SPA_META_ACQUISITION_VERSION = 1u,
	SPA_META_ACQUISITION_SIZE = 96u,
	SPA_META_ACQUISITION_DOMAIN_SIZE = 16u,
	SPA_META_FEATURE_ACQUISITION_VERSION_1 = (1u << 0),
};

enum spa_meta_acquisition_flag {
	SPA_META_ACQUISITION_FLAG_IDENTITY_VALID = (1u << 0),
	SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID = (1u << 1),
	SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID = (1u << 2),
	SPA_META_ACQUISITION_FLAG_ALL = ((1u << 3) - 1u),
};

/**
 * Physical acquisition identity and qualified exposure time.
 *
 * The producer initializes this structure before publishing a complete buffer
 * or before the first ACTIVE publication of a progressive buffer. It remains
 * immutable until every producer and consumer lease on that buffer ends.
 *
 * Identity is the complete (domain, generation, sequence) tuple. The domain is
 * opaque and compared bytewise. exposure_start_nsec is the physical exposure
 * start mapped into the local Linux CLOCK_MONOTONIC domain. A valid timestamp
 * has a nonnegative value and an inclusive uncertainty bound. Version 1
 * timestamps from different Linux kernels are not comparable.
 */
struct SPA_ALIGNED(8) spa_meta_acquisition {
	uint32_t version;
	uint32_t abi_size;
	uint32_t flags;
	uint32_t reserved0;
	uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE];
	uint64_t generation;
	uint64_t sequence;
	int64_t exposure_start_nsec;
	uint64_t exposure_duration_nsec;
	uint64_t timestamp_uncertainty_nsec;
	uint64_t reserved[3];
};

SPA_STATIC_ASSERT(sizeof(struct spa_meta_acquisition) == SPA_META_ACQUISITION_SIZE,
		"spa_meta_acquisition ABI size");
SPA_STATIC_ASSERT(SPA_ALIGNOF(struct spa_meta_acquisition) == 8u,
		"spa_meta_acquisition ABI alignment");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition, domain) == 16u,
		"spa_meta_acquisition domain offset");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition, generation) == 32u,
		"spa_meta_acquisition generation offset");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition, sequence) == 40u,
		"spa_meta_acquisition sequence offset");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition, exposure_start_nsec) == 48u,
		"spa_meta_acquisition exposure-start offset");
SPA_STATIC_ASSERT(offsetof(struct spa_meta_acquisition, reserved) == 72u,
		"spa_meta_acquisition reserved offset");

static inline bool _spa_meta_acquisition_domain_is_zero(
		const uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE])
{
	uint8_t value = 0;
	uint32_t i;

	for (i = 0; i < SPA_META_ACQUISITION_DOMAIN_SIZE; i++)
		value |= domain[i];
	return value == 0;
}

static inline bool _spa_meta_acquisition_fields_are_valid(
		const struct spa_meta_acquisition *acquisition)
{
	bool identity_valid, start_valid, duration_valid;

	if (acquisition == NULL || !SPA_IS_ALIGNED(acquisition, 8) ||
	    acquisition->version != SPA_META_ACQUISITION_VERSION ||
	    acquisition->abi_size != SPA_META_ACQUISITION_SIZE ||
	    (acquisition->flags & ~SPA_META_ACQUISITION_FLAG_ALL) != 0 ||
	    acquisition->reserved0 != 0 ||
	    acquisition->reserved[0] != 0 || acquisition->reserved[1] != 0 ||
	    acquisition->reserved[2] != 0)
		return false;

	identity_valid = SPA_FLAG_IS_SET(acquisition->flags,
			SPA_META_ACQUISITION_FLAG_IDENTITY_VALID);
	if (identity_valid) {
		if (_spa_meta_acquisition_domain_is_zero(acquisition->domain))
			return false;
	} else if (!_spa_meta_acquisition_domain_is_zero(acquisition->domain) ||
		   acquisition->generation != 0 || acquisition->sequence != 0) {
		return false;
	}

	start_valid = SPA_FLAG_IS_SET(acquisition->flags,
			SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID);
	if (start_valid) {
		if (acquisition->exposure_start_nsec < 0)
			return false;
	} else if (acquisition->exposure_start_nsec != SPA_TIME_INVALID ||
		   acquisition->timestamp_uncertainty_nsec != 0) {
		return false;
	}

	duration_valid = SPA_FLAG_IS_SET(acquisition->flags,
			SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID);
	if (duration_valid) {
		if (acquisition->exposure_duration_nsec == 0)
			return false;
	} else if (acquisition->exposure_duration_nsec != 0) {
		return false;
	}
	return true;
}

/** Initialize reusable acquisition metadata with no valid identity or time. */
SPA_API_META bool spa_meta_acquisition_init(struct spa_meta_acquisition *acquisition)
{
	if (acquisition == NULL || !SPA_IS_ALIGNED(acquisition, 8))
		return false;

	memset(acquisition, 0, sizeof(*acquisition));
	acquisition->version = SPA_META_ACQUISITION_VERSION;
	acquisition->abi_size = SPA_META_ACQUISITION_SIZE;
	acquisition->exposure_start_nsec = SPA_TIME_INVALID;
	return true;
}

/** Establish the acquisition identity tuple. */
SPA_API_META bool spa_meta_acquisition_set_identity(
		struct spa_meta_acquisition *acquisition,
		const uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE],
		uint64_t generation, uint64_t sequence)
{
	if (!_spa_meta_acquisition_fields_are_valid(acquisition) || domain == NULL ||
	    _spa_meta_acquisition_domain_is_zero(domain))
		return false;

	memcpy(acquisition->domain, domain, sizeof(acquisition->domain));
	acquisition->generation = generation;
	acquisition->sequence = sequence;
	SPA_FLAG_SET(acquisition->flags, SPA_META_ACQUISITION_FLAG_IDENTITY_VALID);
	return true;
}

/** Establish a local CLOCK_MONOTONIC exposure-start mapping. */
SPA_API_META bool spa_meta_acquisition_set_exposure_start(
		struct spa_meta_acquisition *acquisition, int64_t exposure_start_nsec,
		uint64_t timestamp_uncertainty_nsec)
{
	if (!_spa_meta_acquisition_fields_are_valid(acquisition) || exposure_start_nsec < 0)
		return false;

	acquisition->exposure_start_nsec = exposure_start_nsec;
	acquisition->timestamp_uncertainty_nsec = timestamp_uncertainty_nsec;
	SPA_FLAG_SET(acquisition->flags,
			SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID);
	return true;
}

/** Establish a positive exposure duration. */
SPA_API_META bool spa_meta_acquisition_set_exposure_duration(
		struct spa_meta_acquisition *acquisition, uint64_t exposure_duration_nsec)
{
	if (!_spa_meta_acquisition_fields_are_valid(acquisition) ||
	    exposure_duration_nsec == 0)
		return false;

	acquisition->exposure_duration_nsec = exposure_duration_nsec;
	SPA_FLAG_SET(acquisition->flags,
			SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID);
	return true;
}

/** Validate a mapped Version 1 acquisition metadata allocation. */
SPA_API_META bool spa_meta_acquisition_is_valid(const struct spa_meta *meta)
{
	return meta != NULL && meta->type == SPA_META_Acquisition &&
		meta->data != NULL && meta->size >= sizeof(struct spa_meta_acquisition) &&
		_spa_meta_acquisition_fields_are_valid(
				(const struct spa_meta_acquisition *)meta->data);
}

/** Compare two complete, valid acquisition identity tuples. */
SPA_API_META bool spa_meta_acquisition_identity_equal(
		const struct spa_meta_acquisition *a,
		const struct spa_meta_acquisition *b)
{
	if (!_spa_meta_acquisition_fields_are_valid(a) ||
	    !_spa_meta_acquisition_fields_are_valid(b) ||
	    !SPA_FLAG_IS_SET(a->flags, SPA_META_ACQUISITION_FLAG_IDENTITY_VALID) ||
	    !SPA_FLAG_IS_SET(b->flags, SPA_META_ACQUISITION_FLAG_IDENTITY_VALID))
		return false;

	return a->generation == b->generation && a->sequence == b->sequence &&
		memcmp(a->domain, b->domain, sizeof(a->domain)) == 0;
}

/** Version 1 progressive metadata ABI. */
#define SPA_META_PROGRESSIVE_VERSION		1u
#define SPA_META_PROGRESSIVE_SIZE		48u
#define SPA_META_FEATURE_PROGRESSIVE_VERSION_1	(1u << 0)

#define SPA_META_PROGRESSIVE_COMMITTED_MASK	0x00000000ffffffffULL
#define SPA_META_PROGRESSIVE_STATE_MASK		0x0000000300000000ULL
#define SPA_META_PROGRESSIVE_STATE_SHIFT	32u
#define SPA_META_PROGRESSIVE_RESERVED_MASK	0xfffffffc00000000ULL

#define SPA_META_PROGRESSIVE_FLAG_INCOMPLETE	(1u << 0)
#define SPA_META_PROGRESSIVE_FLAG_INVALID_LAYOUT	(1u << 1)
#define SPA_META_PROGRESSIVE_FLAG_CANCELLED	(1u << 2)
#define SPA_META_PROGRESSIVE_FLAG_DEVICE_ERROR	(1u << 3)
#define SPA_META_PROGRESSIVE_FLAG_CORRUPTED	(1u << 4)
#define SPA_META_PROGRESSIVE_FLAG_PROTOCOL_ERROR	(1u << 5)
#define SPA_META_PROGRESSIVE_FLAG_ALL		((1u << 6) - 1u)

enum spa_meta_progressive_state {
	SPA_META_PROGRESSIVE_STATE_PREPARED,
	SPA_META_PROGRESSIVE_STATE_ACTIVE,
	SPA_META_PROGRESSIVE_STATE_COMPLETE,
	SPA_META_PROGRESSIVE_STATE_ABORTED,
};

/**
 * Progressive payload publication state shared by producer and consumer.
 *
 * One producer owns updates for the duration of a producer lease. Version,
 * layout, granularity, and reserved fields are immutable from PREPARED until
 * all producer and consumer leases end. The committed count never decreases.
 * Before release-storing an ACTIVE snapshot, the producer makes the newly
 * committed payload prefix immutable. A consumer acquire-loads snapshot before
 * reading that prefix and does not read beyond the observed committed count.
 *
 * terminal_flags is zero in PREPARED and ACTIVE. To finish, the producer first
 * writes terminal_flags and any final payload bytes, then release-stores a
 * COMPLETE or ABORTED snapshot. A consumer reads terminal_flags only after an
 * acquire-load observes that terminal snapshot. This ordering deliberately
 * keeps terminal_flags out of validation while publication is active.
 */
struct SPA_ALIGNED(8) spa_meta_progressive {
	uint32_t version;
	uint32_t abi_size;
	uint32_t data_index;
	uint32_t payload_offset;
	uint32_t payload_size;
	uint32_t commit_granularity;
	uint32_t terminal_flags;
	uint32_t reserved0;
	uint64_t snapshot;
	uint64_t reserved1;
};

SPA_API_META uint64_t spa_meta_progressive_snapshot_encode(uint32_t committed,
		enum spa_meta_progressive_state state)
{
	return (uint64_t) committed | ((uint64_t) state << SPA_META_PROGRESSIVE_STATE_SHIFT);
}

SPA_API_META bool spa_meta_progressive_snapshot_decode(uint64_t snapshot,
		uint32_t *committed, enum spa_meta_progressive_state *state)
{
	if (snapshot & SPA_META_PROGRESSIVE_RESERVED_MASK)
		return false;
	if (committed != NULL)
		*committed = (uint32_t) (snapshot & SPA_META_PROGRESSIVE_COMMITTED_MASK);
	if (state != NULL)
		*state = (enum spa_meta_progressive_state)
			((snapshot & SPA_META_PROGRESSIVE_STATE_MASK) >>
			 SPA_META_PROGRESSIVE_STATE_SHIFT);
	return true;
}

SPA_API_META uint64_t spa_meta_progressive_load_acquire(
		const struct spa_meta_progressive *meta)
{
	return __atomic_load_n(&meta->snapshot, __ATOMIC_ACQUIRE);
}

SPA_API_META void spa_meta_progressive_store_release(
		struct spa_meta_progressive *meta, uint64_t snapshot)
{
	__atomic_store_n(&meta->snapshot, snapshot, __ATOMIC_RELEASE);
}

/** Initialize a reusable Version 1 progressive metadata allocation. */
SPA_API_META bool spa_meta_progressive_init(struct spa_meta_progressive *meta,
		uint32_t data_index, uint32_t payload_offset, uint32_t payload_size,
		uint32_t commit_granularity)
{
	if (meta == NULL || !SPA_IS_ALIGNED(meta, 8) || payload_size == 0 ||
	    commit_granularity == 0 || commit_granularity > payload_size)
		return false;

	meta->version = SPA_META_PROGRESSIVE_VERSION;
	meta->abi_size = SPA_META_PROGRESSIVE_SIZE;
	meta->data_index = data_index;
	meta->payload_offset = payload_offset;
	meta->payload_size = payload_size;
	meta->commit_granularity = commit_granularity;
	meta->terminal_flags = 0;
	meta->reserved0 = 0;
	meta->reserved1 = 0;
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(0,
				SPA_META_PROGRESSIVE_STATE_PREPARED));
	return true;
}

/** Validate a mapped Version 1 progressive metadata allocation. */
SPA_API_META bool spa_meta_progressive_is_valid(const struct spa_meta *meta)
{
	const struct spa_meta_progressive *progressive;
	enum spa_meta_progressive_state state;
	uint32_t committed;
	uint64_t snapshot;

	if (meta == NULL || meta->type != SPA_META_Progressive ||
	    meta->data == NULL || meta->size < sizeof(struct spa_meta_progressive) ||
	    !SPA_IS_ALIGNED(meta->data, 8))
		return false;

	progressive = (const struct spa_meta_progressive *) meta->data;
	snapshot = spa_meta_progressive_load_acquire(progressive);
	if (progressive->version != SPA_META_PROGRESSIVE_VERSION ||
	    progressive->abi_size != SPA_META_PROGRESSIVE_SIZE ||
	    progressive->reserved0 != 0 || progressive->reserved1 != 0 ||
	    progressive->payload_size == 0 || progressive->commit_granularity == 0 ||
	    progressive->commit_granularity > progressive->payload_size)
		return false;

	if (!spa_meta_progressive_snapshot_decode(snapshot, &committed, &state) ||
	    committed > progressive->payload_size)
		return false;
	if ((state == SPA_META_PROGRESSIVE_STATE_COMPLETE ||
	     state == SPA_META_PROGRESSIVE_STATE_ABORTED) &&
	    (progressive->terminal_flags & ~SPA_META_PROGRESSIVE_FLAG_ALL) != 0)
		return false;
	return true;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_META_H */
