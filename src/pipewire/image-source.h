/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_IMAGE_SOURCE_H
#define PIPEWIREAO_IMAGE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup pw_image_source Image Source
 *
 * First-class fixed-pool publication support for image producers.
 *
 * One application-owned worker calls every function that mutates an image
 * source or its buffers. A camera SDK, synthetic generator, file-playback
 * agent, or algorithm can own the producer side. The source owns no thread,
 * clock, or graph scheduler. The producer supplies timestamps and optional
 * physical-acquisition metadata.
 *
 * Preparation and teardown are control-path operations. Acquire, complete and
 * progressive publication, producer return, and explicit reclaim are RT safe.
 * They allocate no memory and perform work bounded by the configured pool.
 */

struct pw_buffer;
struct pw_stream;
struct pw_image_source;
struct pw_image_buffer;

#define PW_IMAGE_SOURCE_MAX_BUFFERS 64u

enum pw_image_source_flag {
	PW_IMAGE_SOURCE_FLAG_REQUIRE_HEADER = (1u << 0),
	PW_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION = (1u << 1),
	PW_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE = (1u << 2),
	PW_IMAGE_SOURCE_FLAG_ALL = ((1u << 3) - 1u),
};

struct pw_image_source_config {
#define PW_VERSION_IMAGE_SOURCE_CONFIG 0
	uint32_t version;
	uint32_t min_buffers;
	uint32_t max_buffers;
	uint32_t flags;
};

#define PW_IMAGE_SOURCE_CONFIG_VERSION_0_SIZE 16u
SPA_STATIC_ASSERT(sizeof(struct pw_image_source_config) ==
		PW_IMAGE_SOURCE_CONFIG_VERSION_0_SIZE,
		"image-source configuration version 0 ABI");

enum pw_image_buffer_state {
	PW_IMAGE_BUFFER_STATE_UNUSED,
	PW_IMAGE_BUFFER_STATE_AVAILABLE,
	PW_IMAGE_BUFFER_STATE_PRODUCER,
	PW_IMAGE_BUFFER_STATE_PROGRESSIVE,
	PW_IMAGE_BUFFER_STATE_PUBLISHED,
};

/** Common complete-image metadata supplied by the producer. */
struct pw_image_frame {
#define PW_VERSION_IMAGE_FRAME 0
	uint32_t version;
	uint32_t data_index;
	uint32_t header_flags;
	uint32_t chunk_flags;
	uint32_t offset;
	uint32_t size;
	int32_t stride;
	uint32_t reserved;
	uint64_t sequence;
	int64_t pts;
	/** Optional validated Version 1 physical-acquisition observation. */
	const struct spa_meta_acquisition *acquisition;
};

#define PW_IMAGE_FRAME_VERSION_0_SIZE 56u
SPA_STATIC_ASSERT(sizeof(struct pw_image_frame) == PW_IMAGE_FRAME_VERSION_0_SIZE,
		"image-frame version 0 ABI");

struct pw_image_progressive {
#define PW_VERSION_IMAGE_PROGRESSIVE 0
	uint32_t version;
	uint32_t payload_size;
	uint32_t commit_granularity;
	uint32_t committed;
};

#define PW_IMAGE_PROGRESSIVE_VERSION_0_SIZE 16u
SPA_STATIC_ASSERT(sizeof(struct pw_image_progressive) ==
		PW_IMAGE_PROGRESSIVE_VERSION_0_SIZE,
		"image-progressive version 0 ABI");

/** Single-writer counters. Concurrent snapshots are not supported. */
struct pw_image_source_stats {
	uint64_t prepare_calls;
	uint64_t acquire_calls;
	uint64_t available_acquisitions;
	uint64_t reusable_acquisitions;
	uint64_t pool_exhaustions;
	uint64_t forced_reclaims;
	uint64_t producer_returns;
	uint64_t complete_publications;
	uint64_t progressive_started;
	uint64_t progressive_updates;
	uint64_t progressive_completed;
	uint64_t progressive_aborted;
	uint64_t invalid_transitions;
	uint64_t metadata_errors;
	uint64_t teardown_returns;
	uint32_t pool_size;
	uint32_t max_available_probes;
};

#define PW_IMAGE_SOURCE_STATS_VERSION_0_SIZE 128u
SPA_STATIC_ASSERT(sizeof(struct pw_image_source_stats) ==
		PW_IMAGE_SOURCE_STATS_VERSION_0_SIZE,
		"image-source statistics version 0 ABI");

struct pw_image_source *pw_image_source_new(struct pw_stream *stream,
		const struct pw_image_source_config *config);
void pw_image_source_destroy(struct pw_image_source *source);

/**
 * Acquire the exclusive latest-buffer worker and claim the negotiated pool.
 * This preparation operation may be called only while the producer is
 * quiescent. Every pool slot begins Available.
 */
int pw_image_source_prepare(struct pw_image_source *source);

/**
 * Abort active progressive publications, return locally held buffers, and end
 * exclusive worker ownership. A camera adapter must stop acquisition, fence
 * SDK callbacks, and unregister the buffers before this call.
 */
int pw_image_source_teardown(struct pw_image_source *source);

uint32_t pw_image_source_get_n_buffers(const struct pw_image_source *source);
struct pw_image_buffer *pw_image_source_get_buffer(
		struct pw_image_source *source, uint32_t index);

uint32_t pw_image_buffer_get_index(const struct pw_image_buffer *buffer);
enum pw_image_buffer_state pw_image_buffer_get_state(
		const struct pw_image_buffer *buffer);
struct pw_buffer *pw_image_buffer_get_pw_buffer(
		const struct pw_image_buffer *buffer);
void *pw_image_buffer_get_user_data(const struct pw_image_buffer *buffer);
void pw_image_buffer_set_user_data(struct pw_image_buffer *buffer,
		void *user_data);

/**
 * Acquire one producer-owned slot without withdrawing a visible submission.
 *
 * Returns 1 with a buffer, 0 when the bounded pool is exhausted, or a negative
 * errno-style result. The caller owns the returned payload until it publishes,
 * begins progressive publication, or returns the buffer.
 */
int pw_image_source_try_acquire(struct pw_image_source *source,
		struct pw_image_buffer **buffer);

/** Return an unpublished producer-owned buffer to the local available set. */
int pw_image_source_return_buffer(struct pw_image_source *source,
		struct pw_image_buffer *buffer);

/** Publish one terminal complete frame without copying payload bytes. */
int pw_image_source_publish_complete(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame);

/** Announce a mapped-host-memory buffer while its producer is still writing. */
int pw_image_source_begin_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame,
		const struct pw_image_progressive *progressive);

/** Release-publish a larger immutable prefix for an active buffer. */
int pw_image_source_update_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed);

/** Finish the producer lease in Complete or Aborted state. */
int pw_image_source_finish_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed,
		enum spa_meta_progressive_state state, uint32_t terminal_flags);

/**
 * Explicitly withdraw at most one visible, unclaimed submission and return it
 * producer-owned. This is a lossy starvation-recovery operation, not ordinary
 * acquisition.
 */
int pw_image_source_try_reclaim(struct pw_image_source *source,
		struct pw_image_buffer **buffer);

int pw_image_source_get_stats(const struct pw_image_source *source,
		struct pw_image_source_stats *stats, size_t stats_size);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIREAO_IMAGE_SOURCE_H */
