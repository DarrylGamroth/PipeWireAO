/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BUFFER_IMAGE_SOURCE_H
#define SPA_BUFFER_IMAGE_SOURCE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_IMAGE_SOURCE
#ifdef SPA_API_IMPL
#define SPA_API_IMAGE_SOURCE SPA_API_IMPL
#else
#define SPA_API_IMAGE_SOURCE static inline
#endif
#endif

/**
 * \addtogroup spa_buffer
 * \{
 */

/** Fixed upper bound for one image producer's negotiated pool. */
#define SPA_IMAGE_SOURCE_MAX_BUFFERS 64u

enum spa_image_source_flag {
	SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER = (1u << 0),
	SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION = (1u << 1),
	SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE = (1u << 2),
	SPA_IMAGE_SOURCE_FLAG_ALL = ((1u << 3) - 1u),
};

struct spa_image_source_config {
#define SPA_VERSION_IMAGE_SOURCE_CONFIG 0
	uint32_t version;
	uint32_t min_buffers;
	uint32_t max_buffers;
	uint32_t flags;
};

#define SPA_IMAGE_SOURCE_CONFIG_VERSION_0_SIZE 16u
SPA_STATIC_ASSERT(sizeof(struct spa_image_source_config) ==
		SPA_IMAGE_SOURCE_CONFIG_VERSION_0_SIZE,
		"SPA image-source configuration version 0 ABI");

enum spa_image_buffer_state {
	SPA_IMAGE_BUFFER_STATE_UNUSED,
	SPA_IMAGE_BUFFER_STATE_AVAILABLE,
	SPA_IMAGE_BUFFER_STATE_PRODUCER,
	SPA_IMAGE_BUFFER_STATE_PROGRESSIVE,
	SPA_IMAGE_BUFFER_STATE_PUBLISHED,
};

/** Complete-image metadata supplied by the producer. */
struct spa_image_frame {
#define SPA_VERSION_IMAGE_FRAME 0
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
	const struct spa_meta_acquisition *acquisition;
};

#define SPA_IMAGE_FRAME_VERSION_0_SIZE 56u
SPA_STATIC_ASSERT(sizeof(struct spa_image_frame) ==
		SPA_IMAGE_FRAME_VERSION_0_SIZE,
		"SPA image-frame version 0 ABI");

struct spa_image_progressive {
#define SPA_VERSION_IMAGE_PROGRESSIVE 0
	uint32_t version;
	uint32_t payload_size;
	uint32_t commit_granularity;
	uint32_t committed;
};

#define SPA_IMAGE_PROGRESSIVE_VERSION_0_SIZE 16u
SPA_STATIC_ASSERT(sizeof(struct spa_image_progressive) ==
		SPA_IMAGE_PROGRESSIVE_VERSION_0_SIZE,
		"SPA image-progressive version 0 ABI");

/** Single-writer counters. Concurrent snapshots are not supported. */
struct spa_image_source_stats {
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

#define SPA_IMAGE_SOURCE_STATS_VERSION_0_SIZE 128u
SPA_STATIC_ASSERT(sizeof(struct spa_image_source_stats) ==
		SPA_IMAGE_SOURCE_STATS_VERSION_0_SIZE,
		"SPA image-source statistics version 0 ABI");

/** One negotiated payload slot and its transport-specific handle. */
struct spa_image_source_buffer_info {
	struct spa_buffer *buffer;
	void *handle;
};

/**
 * Scheduler-independent publication transport used by an image source.
 *
 * All methods are called by the source's exclusive worker. They must allocate
 * no memory, must not wait, and must bound their work by the configured pool
 * and subscriber limits. A successful publication transfers the handle to
 * the transport until it is returned by try_dequeue_reusable or explicitly
 * withdrawn by try_reclaim_submission.
 */
struct spa_image_source_transport {
	int (*try_dequeue_reusable)(void *data, void **handle);
	int (*try_reclaim_submission)(void *data, void **handle);
	int (*publish_complete)(void *data, void *handle);
	int (*begin_progressive)(void *data, void *handle);
	int (*end_progressive)(void *data, void *handle);
	int (*return_buffer)(void *data, void *handle);
};

struct spa_image_source;

struct spa_image_source_buffer {
	struct spa_image_source *source;
	struct spa_buffer *buffer;
	void *handle;
	void *user_data;
	uint32_t index;
	enum spa_image_buffer_state state;
};

/**
 * Fixed-pool image publication state.
 *
 * One worker owns all mutation. Initialization, preparation, and teardown are
 * control-path operations. Acquire and publication perform no allocation and
 * are bounded by SPA_IMAGE_SOURCE_MAX_BUFFERS and the transport's declared
 * subscriber bound.
 */
struct spa_image_source {
	struct spa_image_source_config config;
	struct spa_image_source_transport transport;
	void *transport_data;
	struct spa_image_source_stats stats;
	struct spa_image_source_buffer buffers[SPA_IMAGE_SOURCE_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	bool prepared;
};

static inline void spa_image_source_increment(uint64_t *value)
{
	if (*value != UINT64_MAX)
		(*value)++;
}

static inline int spa_image_source_transition_error(
		struct spa_image_source *source)
{
	spa_image_source_increment(&source->stats.invalid_transitions);
	return -EINVAL;
}

static inline int spa_image_source_metadata_error(
		struct spa_image_source *source, int error)
{
	spa_image_source_increment(&source->stats.metadata_errors);
	return error;
}

static inline struct spa_image_source_buffer *spa_image_source_find_handle(
		struct spa_image_source *source, void *handle)
{
	uint32_t i;

	for (i = 0; i < source->n_buffers; i++)
		if (source->buffers[i].handle == handle)
			return &source->buffers[i];
	return NULL;
}

static inline bool spa_image_source_acquisition_valid(
		const struct spa_meta_acquisition *acquisition)
{
	struct spa_meta meta = {
		.type = SPA_META_Acquisition,
		.size = sizeof(*acquisition),
		.data = (void *) acquisition,
	};

	return acquisition != NULL && spa_meta_acquisition_is_valid(&meta);
}

static inline int spa_image_source_prepare_frame(
		struct spa_image_source *source,
		struct spa_image_source_buffer *image_buffer,
		const struct spa_image_frame *frame)
{
	struct spa_meta_acquisition *acquisition;
	struct spa_meta_header *header;
	struct spa_buffer *buffer;
	struct spa_data *data;
	uint64_t end;
	uint32_t valid_header_flags = SPA_META_HEADER_FLAG_DISCONT |
		SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_MARKER |
		SPA_META_HEADER_FLAG_HEADER | SPA_META_HEADER_FLAG_GAP |
		SPA_META_HEADER_FLAG_DELTA_UNIT;
	uint32_t valid_chunk_flags = SPA_CHUNK_FLAG_CORRUPTED |
		SPA_CHUNK_FLAG_EMPTY;
	uint64_t sequence;
	int64_t pts;

	if (frame == NULL || frame->version != SPA_VERSION_IMAGE_FRAME ||
			frame->reserved != 0 ||
			(frame->header_flags & ~valid_header_flags) != 0 ||
			(frame->chunk_flags & ~valid_chunk_flags) != 0 ||
			(frame->pts < 0 && frame->pts != SPA_TIME_INVALID))
		return spa_image_source_metadata_error(source, -EINVAL);
	buffer = image_buffer->buffer;
	if (buffer == NULL || frame->data_index >= buffer->n_datas)
		return spa_image_source_metadata_error(source, -EINVAL);
	data = &buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + frame->size;
	if (data->chunk == NULL || end > data->maxsize)
		return spa_image_source_metadata_error(source, -ENOSPC);
	if (frame->acquisition != NULL &&
			!spa_image_source_acquisition_valid(frame->acquisition))
		return spa_image_source_metadata_error(source, -EINVAL);

	header = (struct spa_meta_header *) spa_buffer_find_meta_data(buffer,
			SPA_META_Header, sizeof(*header));
	if (header == NULL && (source->config.flags &
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER) != 0)
		return spa_image_source_metadata_error(source, -ENOTSUP);
	acquisition = (struct spa_meta_acquisition *) spa_buffer_find_meta_data(
			buffer, SPA_META_Acquisition, sizeof(*acquisition));
	if (acquisition == NULL &&
			((source->config.flags &
			  SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION) != 0 ||
			 frame->acquisition != NULL))
		return spa_image_source_metadata_error(source, -ENOTSUP);
	if (acquisition != NULL && !SPA_IS_ALIGNED(acquisition, 8))
		return spa_image_source_metadata_error(source, -EINVAL);

	data->chunk->offset = frame->offset;
	data->chunk->size = frame->size;
	data->chunk->stride = frame->stride;
	data->chunk->flags = frame->chunk_flags;

	if (acquisition != NULL) {
		if (frame->acquisition == NULL) {
			if (!spa_meta_acquisition_init(acquisition))
				return spa_image_source_metadata_error(source, -EINVAL);
		} else if (acquisition != frame->acquisition) {
			memcpy(acquisition, frame->acquisition, sizeof(*acquisition));
		}
	}

	sequence = frame->sequence;
	pts = frame->pts;
	if (frame->acquisition != NULL) {
		if (SPA_FLAG_IS_SET(frame->acquisition->flags,
				SPA_META_ACQUISITION_FLAG_IDENTITY_VALID))
			sequence = frame->acquisition->sequence;
		if (SPA_FLAG_IS_SET(frame->acquisition->flags,
				SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID))
			pts = frame->acquisition->exposure_start_nsec;
	}
	if (header != NULL) {
		header->flags = frame->header_flags;
		header->offset = 0;
		header->seq = sequence;
		header->pts = pts;
		header->dts_offset = 0;
	}
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_init(
		struct spa_image_source *source,
		const struct spa_image_source_config *config,
		const struct spa_image_source_transport *transport,
		void *transport_data)
{
	if (source == NULL || config == NULL || transport == NULL ||
			config->version != SPA_VERSION_IMAGE_SOURCE_CONFIG ||
			config->min_buffers == 0 ||
			config->max_buffers < config->min_buffers ||
			config->max_buffers > SPA_IMAGE_SOURCE_MAX_BUFFERS ||
			(config->flags & ~SPA_IMAGE_SOURCE_FLAG_ALL) != 0 ||
			transport->try_dequeue_reusable == NULL ||
			transport->try_reclaim_submission == NULL ||
			transport->publish_complete == NULL ||
			transport->return_buffer == NULL ||
			((config->flags & SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE) != 0 &&
			 (transport->begin_progressive == NULL ||
			  transport->end_progressive == NULL)))
		return -EINVAL;
	memset(source, 0, sizeof(*source));
	source->config = *config;
	source->transport = *transport;
	source->transport_data = transport_data;
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_prepare(
		struct spa_image_source *source,
		const struct spa_image_source_buffer_info *buffers,
		uint32_t n_buffers)
{
	uint32_t i, j;

	if (source == NULL || buffers == NULL)
		return -EINVAL;
	spa_image_source_increment(&source->stats.prepare_calls);
	if (source->prepared)
		return spa_image_source_transition_error(source);
	if (n_buffers < source->config.min_buffers)
		return -ENOBUFS;
	if (n_buffers > source->config.max_buffers)
		return -E2BIG;
	for (i = 0; i < n_buffers; i++) {
		if (buffers[i].buffer == NULL || buffers[i].handle == NULL)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (buffers[j].buffer == buffers[i].buffer ||
					buffers[j].handle == buffers[i].handle)
				return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++) {
		struct spa_image_source_buffer *slot = &source->buffers[i];

		slot->source = source;
		slot->buffer = buffers[i].buffer;
		slot->handle = buffers[i].handle;
		slot->index = i;
		slot->state = SPA_IMAGE_BUFFER_STATE_AVAILABLE;
	}
	source->n_buffers = n_buffers;
	source->scan_hint = 0;
	source->stats.pool_size = n_buffers;
	source->prepared = true;
	return (int) n_buffers;
}

SPA_API_IMAGE_SOURCE uint32_t spa_image_source_get_n_buffers(
		const struct spa_image_source *source)
{
	return source == NULL ? 0 : source->n_buffers;
}

SPA_API_IMAGE_SOURCE struct spa_image_source_buffer *
spa_image_source_get_buffer(struct spa_image_source *source, uint32_t index)
{
	return source == NULL || index >= source->n_buffers ? NULL :
		&source->buffers[index];
}

SPA_API_IMAGE_SOURCE uint32_t spa_image_source_buffer_get_index(
		const struct spa_image_source_buffer *buffer)
{
	return buffer == NULL ? SPA_ID_INVALID : buffer->index;
}

SPA_API_IMAGE_SOURCE enum spa_image_buffer_state
spa_image_source_buffer_get_state(
		const struct spa_image_source_buffer *buffer)
{
	return buffer == NULL ? SPA_IMAGE_BUFFER_STATE_UNUSED : buffer->state;
}

SPA_API_IMAGE_SOURCE struct spa_buffer *spa_image_source_buffer_get_buffer(
		const struct spa_image_source_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->buffer;
}

SPA_API_IMAGE_SOURCE void *spa_image_source_buffer_get_handle(
		const struct spa_image_source_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->handle;
}

SPA_API_IMAGE_SOURCE void *spa_image_source_buffer_get_user_data(
		const struct spa_image_source_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->user_data;
}

SPA_API_IMAGE_SOURCE void spa_image_source_buffer_set_user_data(
		struct spa_image_source_buffer *buffer, void *user_data)
{
	if (buffer != NULL)
		buffer->user_data = user_data;
}

static inline struct spa_image_source_buffer *spa_image_source_take_available(
		struct spa_image_source *source, uint32_t *probes)
{
	uint32_t i;

	for (i = 0; i < source->n_buffers; i++) {
		uint32_t index = source->scan_hint + i;
		struct spa_image_source_buffer *buffer;

		if (index >= source->n_buffers)
			index -= source->n_buffers;
		buffer = &source->buffers[index];
		if (buffer->state == SPA_IMAGE_BUFFER_STATE_AVAILABLE) {
			buffer->state = SPA_IMAGE_BUFFER_STATE_PRODUCER;
			source->scan_hint = index + 1;
			if (source->scan_hint == source->n_buffers)
				source->scan_hint = 0;
			*probes = i + 1;
			return buffer;
		}
	}
	*probes = source->n_buffers;
	return NULL;
}

static inline int spa_image_source_take_transport_buffer(
		struct spa_image_source *source, void *handle,
		struct spa_image_source_buffer **buffer)
{
	struct spa_image_source_buffer *image_buffer =
		spa_image_source_find_handle(source, handle);

	if (image_buffer == NULL ||
			image_buffer->state != SPA_IMAGE_BUFFER_STATE_PUBLISHED)
		return spa_image_source_transition_error(source);
	image_buffer->state = SPA_IMAGE_BUFFER_STATE_PRODUCER;
	source->scan_hint = image_buffer->index + 1;
	if (source->scan_hint == source->n_buffers)
		source->scan_hint = 0;
	*buffer = image_buffer;
	return 1;
}

SPA_API_IMAGE_SOURCE int spa_image_source_try_acquire(
		struct spa_image_source *source,
		struct spa_image_source_buffer **buffer)
{
	struct spa_image_source_buffer *image_buffer;
	void *handle = NULL;
	uint32_t probes;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	spa_image_source_increment(&source->stats.acquire_calls);
	if (!source->prepared)
		return spa_image_source_transition_error(source);

	image_buffer = spa_image_source_take_available(source, &probes);
	if (probes > source->stats.max_available_probes)
		source->stats.max_available_probes = probes;
	if (image_buffer != NULL) {
		spa_image_source_increment(&source->stats.available_acquisitions);
		*buffer = image_buffer;
		return 1;
	}

	res = source->transport.try_dequeue_reusable(source->transport_data,
			&handle);
	if (res <= 0) {
		if (res == 0)
			spa_image_source_increment(&source->stats.pool_exhaustions);
		return res;
	}
	if ((res = spa_image_source_take_transport_buffer(source, handle,
			buffer)) < 0)
		return res;
	spa_image_source_increment(&source->stats.reusable_acquisitions);
	return 1;
}

SPA_API_IMAGE_SOURCE int spa_image_source_return_buffer(
		struct spa_image_source *source,
		struct spa_image_source_buffer *buffer)
{
	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared ||
			buffer->state != SPA_IMAGE_BUFFER_STATE_PRODUCER)
		return spa_image_source_transition_error(source);
	buffer->state = SPA_IMAGE_BUFFER_STATE_AVAILABLE;
	source->scan_hint = buffer->index;
	spa_image_source_increment(&source->stats.producer_returns);
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_publish_complete(
		struct spa_image_source *source,
		struct spa_image_source_buffer *buffer,
		const struct spa_image_frame *frame)
{
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared ||
			buffer->state != SPA_IMAGE_BUFFER_STATE_PRODUCER)
		return spa_image_source_transition_error(source);
	if ((res = spa_image_source_prepare_frame(source, buffer, frame)) < 0)
		return res;
	if ((res = source->transport.publish_complete(source->transport_data,
			buffer->handle)) < 0)
		return res;
	buffer->state = SPA_IMAGE_BUFFER_STATE_PUBLISHED;
	spa_image_source_increment(&source->stats.complete_publications);
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_begin_progressive(
		struct spa_image_source *source,
		struct spa_image_source_buffer *buffer,
		const struct spa_image_frame *frame,
		const struct spa_image_progressive *progressive)
{
	struct spa_meta_progressive *meta;
	struct spa_data *data;
	uint64_t end;
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source ||
			progressive == NULL)
		return -EINVAL;
	if (!source->prepared ||
			buffer->state != SPA_IMAGE_BUFFER_STATE_PRODUCER)
		return spa_image_source_transition_error(source);
	if ((source->config.flags &
			SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE) == 0)
		return -ENOTSUP;
	if (progressive->version != SPA_VERSION_IMAGE_PROGRESSIVE ||
			progressive->payload_size == 0 ||
			progressive->commit_granularity == 0 ||
			progressive->commit_granularity > progressive->payload_size ||
			progressive->committed > progressive->payload_size ||
			(progressive->committed != progressive->payload_size &&
			 progressive->committed % progressive->commit_granularity != 0) ||
			frame == NULL || frame->size != progressive->payload_size)
		return spa_image_source_metadata_error(source, -EINVAL);
	if (frame->data_index >= buffer->buffer->n_datas)
		return spa_image_source_metadata_error(source, -EINVAL);
	data = &buffer->buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + progressive->payload_size;
	if (data->type == SPA_DATA_DmaBuf)
		return -ENOTSUP;
	if (data->data == NULL || end > data->maxsize)
		return spa_image_source_metadata_error(source, -ENOSPC);
	meta = (struct spa_meta_progressive *) spa_buffer_find_meta_data(
			buffer->buffer, SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL)
		return spa_image_source_metadata_error(source, -ENOTSUP);
	if ((res = spa_image_source_prepare_frame(source, buffer, frame)) < 0)
		return res;
	if (!spa_meta_progressive_init(meta, frame->data_index, frame->offset,
			progressive->payload_size,
			progressive->commit_granularity))
		return spa_image_source_metadata_error(source, -EINVAL);
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(progressive->committed,
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	if ((res = source->transport.begin_progressive(source->transport_data,
			buffer->handle)) < 0) {
		(void) spa_meta_progressive_init(meta, frame->data_index,
				frame->offset, progressive->payload_size,
				progressive->commit_granularity);
		return res;
	}
	buffer->state = SPA_IMAGE_BUFFER_STATE_PROGRESSIVE;
	spa_image_source_increment(&source->stats.progressive_started);
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_update_progressive(
		struct spa_image_source *source,
		struct spa_image_source_buffer *buffer, uint32_t committed)
{
	struct spa_meta_progressive *meta;
	enum spa_meta_progressive_state state;
	uint32_t current;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared ||
			buffer->state != SPA_IMAGE_BUFFER_STATE_PROGRESSIVE)
		return spa_image_source_transition_error(source);
	meta = (struct spa_meta_progressive *) spa_buffer_find_meta_data(
			buffer->buffer, SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL || !spa_meta_progressive_snapshot_decode(
			spa_meta_progressive_load_acquire(meta), &current, &state) ||
			state != SPA_META_PROGRESSIVE_STATE_ACTIVE ||
			committed < current || committed > meta->payload_size ||
			(committed != meta->payload_size &&
			 committed % meta->commit_granularity != 0))
		return spa_image_source_metadata_error(source, -EINVAL);
	if (committed == current)
		return 0;
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(committed,
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	spa_image_source_increment(&source->stats.progressive_updates);
	return 1;
}

SPA_API_IMAGE_SOURCE int spa_image_source_finish_progressive(
		struct spa_image_source *source,
		struct spa_image_source_buffer *buffer, uint32_t committed,
		enum spa_meta_progressive_state state, uint32_t terminal_flags)
{
	struct spa_meta_progressive *meta;
	enum spa_meta_progressive_state current_state;
	uint32_t current;
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared ||
			buffer->state != SPA_IMAGE_BUFFER_STATE_PROGRESSIVE)
		return spa_image_source_transition_error(source);
	meta = (struct spa_meta_progressive *) spa_buffer_find_meta_data(
			buffer->buffer, SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL || !spa_meta_progressive_snapshot_decode(
			spa_meta_progressive_load_acquire(meta), &current,
			&current_state) ||
			current_state != SPA_META_PROGRESSIVE_STATE_ACTIVE ||
			(state != SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			 state != SPA_META_PROGRESSIVE_STATE_ABORTED) ||
			(terminal_flags & ~SPA_META_PROGRESSIVE_FLAG_ALL) != 0 ||
			committed < current || committed > meta->payload_size ||
			(committed != meta->payload_size &&
			 committed % meta->commit_granularity != 0) ||
			(state == SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			 (committed != meta->payload_size || terminal_flags != 0)))
		return spa_image_source_metadata_error(source, -EINVAL);
	meta->terminal_flags = terminal_flags;
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(committed, state));
	if ((res = source->transport.end_progressive(source->transport_data,
			buffer->handle)) < 0)
		return res;
	buffer->state = SPA_IMAGE_BUFFER_STATE_PUBLISHED;
	if (state == SPA_META_PROGRESSIVE_STATE_COMPLETE)
		spa_image_source_increment(&source->stats.progressive_completed);
	else
		spa_image_source_increment(&source->stats.progressive_aborted);
	return 0;
}

SPA_API_IMAGE_SOURCE int spa_image_source_try_reclaim_submission(
		struct spa_image_source *source,
		struct spa_image_source_buffer **buffer)
{
	void *handle = NULL;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	if (!source->prepared)
		return spa_image_source_transition_error(source);
	res = source->transport.try_reclaim_submission(source->transport_data,
			&handle);
	if (res <= 0)
		return res;
	if ((res = spa_image_source_take_transport_buffer(source, handle,
			buffer)) < 0)
		return res;
	spa_image_source_increment(&source->stats.forced_reclaims);
	return 1;
}

SPA_API_IMAGE_SOURCE int spa_image_source_teardown(
		struct spa_image_source *source)
{
	uint32_t i;
	int first_error = 0;

	if (source == NULL)
		return -EINVAL;
	if (!source->prepared)
		return 0;
	for (i = 0; i < source->n_buffers; i++) {
		struct spa_image_source_buffer *buffer = &source->buffers[i];
		int res = 0;

		if (buffer->state == SPA_IMAGE_BUFFER_STATE_PROGRESSIVE) {
			struct spa_meta_progressive *meta =
				(struct spa_meta_progressive *) spa_buffer_find_meta_data(
						buffer->buffer, SPA_META_Progressive, sizeof(*meta));
			uint32_t committed = 0;
			enum spa_meta_progressive_state state;

			if (meta == NULL || !spa_meta_progressive_snapshot_decode(
					spa_meta_progressive_load_acquire(meta),
					&committed, &state)) {
				res = -EPROTO;
			} else {
				meta->terminal_flags =
					SPA_META_PROGRESSIVE_FLAG_CANCELLED;
				spa_meta_progressive_store_release(meta,
						spa_meta_progressive_snapshot_encode(committed,
							SPA_META_PROGRESSIVE_STATE_ABORTED));
				res = source->transport.end_progressive(
						source->transport_data, buffer->handle);
				if (res == 0) {
					buffer->state = SPA_IMAGE_BUFFER_STATE_PUBLISHED;
					spa_image_source_increment(
							&source->stats.progressive_aborted);
				}
			}
		}
		if (res == 0 &&
				(buffer->state == SPA_IMAGE_BUFFER_STATE_AVAILABLE ||
				 buffer->state == SPA_IMAGE_BUFFER_STATE_PRODUCER)) {
			res = source->transport.return_buffer(
					source->transport_data, buffer->handle);
			if (res == 0)
				spa_image_source_increment(&source->stats.teardown_returns);
		}
		if (res < 0 && first_error == 0)
			first_error = res;
	}
	for (i = 0; i < source->n_buffers; i++)
		memset(&source->buffers[i], 0, sizeof(source->buffers[i]));
	source->n_buffers = 0;
	source->scan_hint = 0;
	source->stats.pool_size = 0;
	source->prepared = false;
	return first_error;
}

SPA_API_IMAGE_SOURCE int spa_image_source_get_stats(
		const struct spa_image_source *source,
		struct spa_image_source_stats *stats, size_t stats_size)
{
	if (source == NULL || stats == NULL)
		return -EINVAL;
	if (stats_size < sizeof(*stats))
		return -ENOSPC;
	memcpy(stats, &source->stats, sizeof(*stats));
	return 0;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_BUFFER_IMAGE_SOURCE_H */
