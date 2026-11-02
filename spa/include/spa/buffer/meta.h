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

	/* Metadata types are negotiated in a 32-bit mask. Reserve these low
	 * PipeWireAO slots explicitly instead of following upstream implicitly. */
	SPA_META_START_PipeWireAO = 10,
	/* Slots 11 (Acquisition) and 12 (ndarray progress) are retired. */
	SPA_META_Properties = 13,	/**< bounded BufferProperties POD Object */
	_SPA_META_LAST = 14,		/**< not part of ABI/API */

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

/** Owned acquisition facts; not metadata allocations or wire records. */
enum spa_acquisition_constant {
	SPA_ACQUISITION_DOMAIN_SIZE = 16u,
	SPA_EXPOSURE_PTP_CLOCK_ID_SIZE = 8u,
};

enum spa_exposure_timebase {
	SPA_EXPOSURE_TIMEBASE_MONOTONIC = 1u,
	SPA_EXPOSURE_TIMEBASE_TAI = 2u,
};

enum spa_meta_trigger_classification {
	SPA_META_TRIGGER_ABSENT = 0u,
	SPA_META_TRIGGER_LOW = 1u,
	SPA_META_TRIGGER_HIGH = 2u,
	SPA_META_TRIGGER_MIXED = 3u,
};

struct spa_acquisition_identity {
	uint8_t domain[SPA_ACQUISITION_DOMAIN_SIZE];
	uint64_t generation;
	uint64_t sequence;
};

struct spa_exposure_start {
	int64_t nsec;
	uint64_t uncertainty_nsec;
	uint32_t timebase;
	uint8_t ptp_grandmaster_identity[SPA_EXPOSURE_PTP_CLOCK_ID_SIZE];
	uint8_t ptp_domain_number;
};

static inline bool _spa_meta_bytes_are_zero(const uint8_t *data, uint32_t size)
{
	uint8_t value = 0;
	uint32_t i;
	for (i = 0; i < size; i++)
		value |= data[i];
	return value == 0;
}

/** Version 1 per-buffer property schema. See acquisition-metadata.md. */
enum spa_meta_properties_constant {
	SPA_META_PROPERTIES_VERSION = 1u,
	SPA_META_FEATURE_PROPERTIES_VERSION_1 = (1u << 0),
	SPA_META_PROPERTIES_MIN_SIZE = 16u,
	SPA_META_PROPERTIES_MAX_SIZE = 4096u,
	SPA_META_PROPERTIES_MAX_FIELDS = 64u,
};

enum spa_meta_property {
	SPA_META_PROPERTY_opticalClassification = 1u,
	SPA_META_PROPERTY_triggerClassification = 2u,
	SPA_META_PROPERTY_acquisitionDomain = 3u,
	SPA_META_PROPERTY_acquisitionGeneration = 4u,
	SPA_META_PROPERTY_acquisitionSequence = 5u,
	SPA_META_PROPERTY_exposureDuration = 6u,
	SPA_META_PROPERTY_exposureStart = 7u,
	SPA_META_PROPERTY_exposureTimebase = 8u,
	SPA_META_PROPERTY_timestampUncertainty = 9u,
	SPA_META_PROPERTY_ptpGrandmasterIdentity = 10u,
	SPA_META_PROPERTY_ptpDomainNumber = 11u,
};

enum spa_meta_optical_classification {
	SPA_META_OPTICAL_ABSENT = 0u,
	SPA_META_OPTICAL_UNFRINGED = 1u,
	SPA_META_OPTICAL_FRINGED = 2u,
	SPA_META_OPTICAL_MIXED = 3u,
};

struct spa_meta_property_requirement {
	uint32_t key;
	uint32_t type;
};

SPA_STATIC_ASSERT(SPA_META_Properties == 13u, "buffer properties metadata ABI");
SPA_STATIC_ASSERT(SPA_TYPE_OBJECT_BufferProperties == 0x40010u,
		"buffer properties object ABI");
SPA_STATIC_ASSERT(sizeof(struct spa_meta_property_requirement) == 8u,
		"buffer property requirement ABI");

static inline bool _spa_meta_properties_storage_is_valid(const struct spa_meta *meta)
{
	return meta != NULL && meta->type == SPA_META_Properties &&
		meta->data != NULL && SPA_IS_ALIGNED(meta->data, 8) &&
		meta->size >= SPA_META_PROPERTIES_MIN_SIZE &&
		meta->size <= SPA_META_PROPERTIES_MAX_SIZE && (meta->size & 7u) == 0;
}

static inline bool _spa_meta_property_type_is_valid(uint32_t type)
{
	switch (type) {
	case SPA_TYPE_Bool: case SPA_TYPE_Id: case SPA_TYPE_Int:
	case SPA_TYPE_Long: case SPA_TYPE_Float: case SPA_TYPE_Double:
	case SPA_TYPE_String: case SPA_TYPE_Bytes:
		return true;
	default:
		return false;
	}
}

static inline uint32_t _spa_meta_property_known_type(uint32_t key)
{
	switch (key) {
	case SPA_META_PROPERTY_opticalClassification:
	case SPA_META_PROPERTY_triggerClassification:
	case SPA_META_PROPERTY_exposureTimebase:
	case SPA_META_PROPERTY_ptpDomainNumber:
		return SPA_TYPE_Id;
	case SPA_META_PROPERTY_acquisitionDomain:
	case SPA_META_PROPERTY_acquisitionGeneration:
	case SPA_META_PROPERTY_acquisitionSequence:
	case SPA_META_PROPERTY_exposureDuration:
	case SPA_META_PROPERTY_timestampUncertainty:
	case SPA_META_PROPERTY_ptpGrandmasterIdentity:
		return SPA_TYPE_Bytes;
	case SPA_META_PROPERTY_exposureStart:
		return SPA_TYPE_Long;
	default:
		return 0;
	}
}

static inline uint64_t _spa_meta_uint64_read(const void *value)
{
	const uint8_t *bytes = (const uint8_t *)value;
	uint64_t result = 0;
	uint32_t i;
	for (i = 0; i < 8; i++)
		result |= (uint64_t)bytes[i] << (8u * i);
	return result;
}

static inline bool _spa_meta_property_value_is_valid(uint32_t key,
		uint32_t type, uint32_t size, const void *value)
{
	uint32_t scalar, known = _spa_meta_property_known_type(key);
	int64_t start;

	if (key == 0 || !_spa_meta_property_type_is_valid(type) ||
	    (size > 0 && value == NULL) || (known != 0 && type != known))
		return false;
	if (type == SPA_TYPE_String) {
		if (size == 0 || memchr(value, '\0', size) !=
		    SPA_PTROFF(value, size - 1u, const void))
			return false;
	} else if (type != SPA_TYPE_Bytes) {
		uint32_t expected = (type == SPA_TYPE_Long || type == SPA_TYPE_Double) ? 8u : 4u;
		if (size != expected)
			return false;
		if (type == SPA_TYPE_Bool) {
			memcpy(&scalar, value, sizeof(scalar));
			if (scalar > 1u)
				return false;
		}
	}
	if (known == SPA_TYPE_Bytes) {
		if (size != (key == SPA_META_PROPERTY_acquisitionDomain ? 16u : 8u))
			return false;
		if ((key == SPA_META_PROPERTY_acquisitionDomain ||
		     key == SPA_META_PROPERTY_ptpGrandmasterIdentity) &&
		    _spa_meta_bytes_are_zero((const uint8_t *)value, size))
			return false;
		if (key == SPA_META_PROPERTY_exposureDuration && _spa_meta_uint64_read(value) == 0)
			return false;
	} else if (known == SPA_TYPE_Id) {
		memcpy(&scalar, value, sizeof(scalar));
		if (key == SPA_META_PROPERTY_exposureTimebase)
			return scalar == SPA_EXPOSURE_TIMEBASE_MONOTONIC || scalar == SPA_EXPOSURE_TIMEBASE_TAI;
		return scalar <= (key == SPA_META_PROPERTY_ptpDomainNumber ? 255u : 3u);
	} else if (key == SPA_META_PROPERTY_exposureStart) {
		memcpy(&start, value, sizeof(start));
		return start >= 0;
	}
	return true;
}

/** Validate builder structure, values and required key/type pairs. */
static inline bool _spa_meta_properties_is_structurally_valid(const struct spa_meta *meta,
		const struct spa_meta_property_requirement *requirements, uint32_t n_requirements,
		bool reject_unknown_mandatory)
{
	const struct spa_pod_object *object;
	uint32_t offset, end, previous = 0, count = 0, i, j;
	uint64_t found = 0;

	if (!_spa_meta_properties_storage_is_valid(meta) ||
	    n_requirements > SPA_META_PROPERTIES_MAX_FIELDS ||
	    (n_requirements != 0 && (requirements == NULL ||
	     !SPA_IS_ALIGNED(requirements, SPA_ALIGNOF(struct spa_meta_property_requirement)))))
		return false;
	for (i = 0; i < n_requirements; i++) {
		if (requirements[i].key == 0 ||
		    !_spa_meta_property_type_is_valid(requirements[i].type) ||
		    (_spa_meta_property_known_type(requirements[i].key) != 0 &&
		     requirements[i].type != _spa_meta_property_known_type(requirements[i].key)))
			return false;
		for (j = 0; j < i; j++)
			if (requirements[i].key == requirements[j].key)
				return false;
	}
	object = (const struct spa_pod_object *)meta->data;
	if (object->pod.type != SPA_TYPE_Object ||
	    object->body.type != SPA_TYPE_OBJECT_BufferProperties ||
	    object->body.id != SPA_META_PROPERTIES_VERSION ||
	    object->pod.size < sizeof(object->body) ||
	    object->pod.size > meta->size - sizeof(struct spa_pod))
		return false;
	end = sizeof(struct spa_pod) + object->pod.size;
	if ((end & 7u) != 0)
		return false;
	for (offset = sizeof(*object); offset < end;) {
		const struct spa_pod_prop *prop;
		uint32_t size, padded;
		bool understood;

		if (++count > SPA_META_PROPERTIES_MAX_FIELDS ||
		    end - offset < sizeof(*prop))
			return false;
		prop = SPA_PTROFF(object, offset, const struct spa_pod_prop);
		if (prop->key <= previous || prop->value.size > end - offset - sizeof(*prop) ||
		    (prop->flags & ~SPA_POD_PROP_FLAG_MANDATORY) != 0 ||
		    (prop->key >= 3 && prop->key <= 11 &&
		     prop->flags != SPA_POD_PROP_FLAG_MANDATORY) ||
		    !_spa_meta_property_value_is_valid(prop->key, prop->value.type,
			    prop->value.size, SPA_POD_BODY_CONST(&prop->value)))
			return false;
		size = sizeof(*prop) + prop->value.size;
		padded = SPA_ROUND_UP_N(size, 8);
		if (padded > end - offset ||
		    !_spa_meta_bytes_are_zero(SPA_PTROFF(prop, size, const uint8_t),
			    padded - size))
			return false;
		understood = _spa_meta_property_known_type(prop->key) != 0;
		for (i = 0; i < n_requirements; i++) {
			if (requirements[i].key != prop->key)
				continue;
			if (requirements[i].type != prop->value.type)
				return false;
			found |= (uint64_t)1 << i;
			understood = true;
		}
		if (reject_unknown_mandatory && !understood && (prop->flags & SPA_POD_PROP_FLAG_MANDATORY))
			return false;
		previous = prop->key;
		offset += padded;
	}
	return (n_requirements == 64 ? found == UINT64_MAX :
		found == (((uint64_t)1 << n_requirements) - 1u)) &&
		_spa_meta_bytes_are_zero(SPA_PTROFF(object, end, const uint8_t),
				meta->size - end);
}

/** Clear every byte of caller-owned storage and initialize an empty Object. */
SPA_API_META bool spa_meta_properties_init(struct spa_meta *meta)
{
	struct spa_pod_object *object;

	if (!_spa_meta_properties_storage_is_valid(meta))
		return false;
	memset(meta->data, 0, meta->size);
	object = (struct spa_pod_object *)meta->data;
	object->pod.size = sizeof(object->body);
	object->pod.type = SPA_TYPE_Object;
	object->body.type = SPA_TYPE_OBJECT_BufferProperties;
	object->body.id = SPA_META_PROPERTIES_VERSION;
	return true;
}

/** Borrow a value after successful validation; never retain it past the buffer lease. */
SPA_API_META const struct spa_pod *spa_meta_properties_find(const struct spa_meta *meta,
		uint32_t key)
{
	const struct spa_pod_object *object;
	uint32_t offset, end;

	if (!_spa_meta_properties_storage_is_valid(meta))
		return NULL;
	object = (const struct spa_pod_object *)meta->data;
	if (object->pod.size > meta->size - sizeof(struct spa_pod))
		return NULL;
	end = sizeof(struct spa_pod) + object->pod.size;
	for (offset = sizeof(*object); offset < end;) {
		const struct spa_pod_prop *prop;
		uint32_t size;
		if (end - offset < sizeof(*prop))
			return NULL;
		prop = SPA_PTROFF(object, offset, const struct spa_pod_prop);
		if (prop->value.size > end - offset - sizeof(*prop))
			return NULL;
		size = SPA_ROUND_UP_N(sizeof(*prop) + prop->value.size, 8);
		if (size > end - offset)
			return NULL;
		if (prop->key == key)
			return &prop->value;
		offset += size;
	}
	return NULL;
}

/** Validate the complete Object, including atomic acquisition and clock groups. */
static inline bool _spa_meta_properties_complete_is_valid(const struct spa_meta *meta,
		const struct spa_meta_property_requirement *requirements, uint32_t n_requirements,
		bool reject_unknown_mandatory)
{
	const struct spa_pod *timebase;
	uint32_t mask = 0, key, value;
	if (!_spa_meta_properties_is_structurally_valid(meta, requirements, n_requirements, reject_unknown_mandatory))
		return false;
	for (key = 3; key <= 11; key++)
		if (spa_meta_properties_find(meta, key) != NULL)
			mask |= 1u << key;
	if ((mask & 0x38u) != 0 && (mask & 0x38u) != 0x38u)
		return false;
	if ((mask & 0x380u) != 0 && (mask & 0x380u) != 0x380u)
		return false;
	timebase = spa_meta_properties_find(meta, SPA_META_PROPERTY_exposureTimebase);
	if (timebase == NULL)
		return (mask & 0xc00u) == 0;
	memcpy(&value, SPA_POD_BODY_CONST(timebase), sizeof(value));
	return value == SPA_EXPOSURE_TIMEBASE_TAI ?
		(mask & 0xc00u) == 0xc00u : (mask & 0xc00u) == 0;
}

/** Validate complete facts and the consumer's understanding of mandatory keys. */
SPA_API_META bool spa_meta_properties_is_valid(const struct spa_meta *meta,
		const struct spa_meta_property_requirement *requirements, uint32_t n_requirements)
{
	return _spa_meta_properties_complete_is_valid(meta, requirements, n_requirements, true);
}

/** Append a scalar in ascending key order. Private groups may be incomplete. */
SPA_API_META bool spa_meta_properties_add(struct spa_meta *meta, uint32_t key,
		uint32_t flags, uint32_t type, uint32_t size, const void *value)
{
	struct spa_pod_object *object;
	struct spa_pod_prop *prop;
	uint32_t offset, end, count = 0, previous = 0, padded;

	if (!_spa_meta_properties_is_structurally_valid(meta, NULL, 0, false) ||
	    (flags & ~SPA_POD_PROP_FLAG_MANDATORY) != 0 ||
	    (key >= 3 && key <= 11 && flags != SPA_POD_PROP_FLAG_MANDATORY) ||
	    size > meta->size || !_spa_meta_property_value_is_valid(key, type, size, value))
		return false;
	object = (struct spa_pod_object *)meta->data;
	end = sizeof(struct spa_pod) + object->pod.size;
	for (offset = sizeof(*object); offset < end;) {
		prop = SPA_PTROFF(object, offset, struct spa_pod_prop);
		previous = prop->key;
		count++;
		offset += SPA_ROUND_UP_N(sizeof(*prop) + prop->value.size, 8);
	}
	padded = SPA_ROUND_UP_N(sizeof(*prop) + size, 8);
	if (count == SPA_META_PROPERTIES_MAX_FIELDS || key <= previous || padded > meta->size - end)
		return false;
	prop = SPA_PTROFF(object, end, struct spa_pod_prop);
	if (size != 0)
		memmove(SPA_POD_BODY(&prop->value), value, size);
	prop->key = key;
	prop->flags = flags;
	prop->value.size = size;
	prop->value.type = type;
	object->pod.size += padded;
	return true;
}

/** Append one unsigned integer as exactly eight little-endian Bytes. */
SPA_API_META bool spa_meta_properties_add_uint64(struct spa_meta *meta,
		uint32_t key, uint32_t flags, uint64_t value)
{
	uint8_t bytes[8];
	uint32_t i;
	for (i = 0; i < 8; i++)
		bytes[i] = (uint8_t)(value >> (8u * i));
	return spa_meta_properties_add(meta, key, flags, SPA_TYPE_Bytes, sizeof(bytes), bytes);
}

/**
 * Read facts after complete admission with the consumer's requirements.
 * These getters recheck values and complete groups, but do not establish
 * understanding of application-defined mandatory keys. As with find(), the
 * caller MUST first call spa_meta_properties_is_valid(meta, requirements, n).
 */
SPA_API_META bool spa_meta_properties_get_uint64(const struct spa_meta *meta,
		uint32_t key, uint64_t *value)
{
	const struct spa_pod *pod;
	if (value == NULL || !_spa_meta_properties_complete_is_valid(meta, NULL, 0, false) ||
	    (pod = spa_meta_properties_find(meta, key)) == NULL ||
	    pod->type != SPA_TYPE_Bytes || pod->size != 8)
		return false;
	*value = _spa_meta_uint64_read(SPA_POD_BODY_CONST(pod));
	return true;
}

SPA_API_META bool spa_meta_properties_get_acquisition_identity(const struct spa_meta *meta,
		struct spa_acquisition_identity *identity)
{
	const struct spa_pod *domain;
	struct spa_acquisition_identity value;
	if (identity == NULL || !_spa_meta_properties_complete_is_valid(meta, NULL, 0, false) ||
	    (domain = spa_meta_properties_find(meta, SPA_META_PROPERTY_acquisitionDomain)) == NULL)
		return false;
	memcpy(value.domain, SPA_POD_BODY_CONST(domain), sizeof(value.domain));
	spa_meta_properties_get_uint64(meta, SPA_META_PROPERTY_acquisitionGeneration, &value.generation);
	spa_meta_properties_get_uint64(meta, SPA_META_PROPERTY_acquisitionSequence, &value.sequence);
	*identity = value;
	return true;
}

SPA_API_META bool spa_meta_properties_get_exposure_duration(const struct spa_meta *meta,
		uint64_t *duration_nsec)
{
	return spa_meta_properties_get_uint64(meta, SPA_META_PROPERTY_exposureDuration, duration_nsec);
}

SPA_API_META bool spa_meta_properties_get_exposure_start(const struct spa_meta *meta,
		struct spa_exposure_start *start)
{
	const struct spa_pod *pod;
	struct spa_exposure_start value = { 0 };
	uint32_t domain;
	if (start == NULL || !_spa_meta_properties_complete_is_valid(meta, NULL, 0, false) ||
	    (pod = spa_meta_properties_find(meta, SPA_META_PROPERTY_exposureStart)) == NULL)
		return false;
	memcpy(&value.nsec, SPA_POD_BODY_CONST(pod), sizeof(value.nsec));
	pod = spa_meta_properties_find(meta, SPA_META_PROPERTY_exposureTimebase);
	memcpy(&value.timebase, SPA_POD_BODY_CONST(pod), sizeof(value.timebase));
	spa_meta_properties_get_uint64(meta, SPA_META_PROPERTY_timestampUncertainty, &value.uncertainty_nsec);
	if (value.timebase == SPA_EXPOSURE_TIMEBASE_TAI) {
		pod = spa_meta_properties_find(meta, SPA_META_PROPERTY_ptpGrandmasterIdentity);
		memcpy(value.ptp_grandmaster_identity, SPA_POD_BODY_CONST(pod), sizeof(value.ptp_grandmaster_identity));
		pod = spa_meta_properties_find(meta, SPA_META_PROPERTY_ptpDomainNumber);
		memcpy(&domain, SPA_POD_BODY_CONST(pod), sizeof(domain));
		value.ptp_domain_number = (uint8_t)domain;
	}
	*start = value;
	return true;
}

SPA_API_META bool spa_acquisition_identity_equal(const struct spa_acquisition_identity *a,
		const struct spa_acquisition_identity *b)
{
	return a != NULL && b != NULL &&
		!_spa_meta_bytes_are_zero(a->domain, sizeof(a->domain)) &&
		!_spa_meta_bytes_are_zero(b->domain, sizeof(b->domain)) &&
		a->generation == b->generation && a->sequence == b->sequence &&
		memcmp(a->domain, b->domain, sizeof(a->domain)) == 0;
}

SPA_API_META bool spa_exposure_start_time_difference(const struct spa_exposure_start *a,
		const struct spa_exposure_start *b, int64_t *difference_nsec,
		uint64_t *combined_uncertainty_nsec)
{
	uint64_t uncertainty;
	if (a == NULL || b == NULL || a->nsec < 0 || b->nsec < 0 ||
	    a->timebase != SPA_EXPOSURE_TIMEBASE_TAI || b->timebase != SPA_EXPOSURE_TIMEBASE_TAI ||
	    _spa_meta_bytes_are_zero(a->ptp_grandmaster_identity, sizeof(a->ptp_grandmaster_identity)) ||
	    a->ptp_domain_number != b->ptp_domain_number ||
	    memcmp(a->ptp_grandmaster_identity, b->ptp_grandmaster_identity,
		    sizeof(a->ptp_grandmaster_identity)) != 0)
		return false;
	uncertainty = UINT64_MAX - a->uncertainty_nsec < b->uncertainty_nsec ?
		UINT64_MAX : a->uncertainty_nsec + b->uncertainty_nsec;
	if (difference_nsec != NULL)
		*difference_nsec = a->nsec - b->nsec;
	if (combined_uncertainty_nsec != NULL)
		*combined_uncertainty_nsec = uncertainty;
	return true;
}

SPA_API_META bool spa_exposure_start_times_match(const struct spa_exposure_start *a,
		const struct spa_exposure_start *b, uint64_t tolerance_nsec)
{
	int64_t difference;
	uint64_t limit, distance;
	if (!spa_exposure_start_time_difference(a, b, &difference, &limit))
		return false;
	distance = difference < 0 ? (uint64_t)-difference : (uint64_t)difference;
	limit = UINT64_MAX - limit < tolerance_nsec ? UINT64_MAX : limit + tolerance_nsec;
	return distance <= limit;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_META_H */
