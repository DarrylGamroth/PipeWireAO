/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdlib.h>

#include <pipewire/image-source.h>
#include <pipewire/stream.h>

#include <spa/buffer/image-source.h>
#include <spa/utils/defs.h>

struct pw_image_source {
	struct pw_stream *stream;
	struct spa_image_source source;
};

SPA_STATIC_ASSERT(PW_IMAGE_SOURCE_MAX_BUFFERS == SPA_IMAGE_SOURCE_MAX_BUFFERS,
		"PipeWire and SPA image-source pool limits");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_SOURCE_FLAG_REQUIRE_HEADER ==
		(uint32_t) SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER,
		"PipeWire and SPA header requirement flag");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION ==
		(uint32_t) SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION,
		"PipeWire and SPA acquisition requirement flag");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE ==
		(uint32_t) SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE,
		"PipeWire and SPA progressive flag");
SPA_STATIC_ASSERT(PW_IMAGE_SOURCE_STATS_VERSION_0_SIZE ==
		SPA_IMAGE_SOURCE_STATS_VERSION_0_SIZE,
		"PipeWire and SPA image-source statistics ABI");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_BUFFER_STATE_UNUSED ==
		(uint32_t) SPA_IMAGE_BUFFER_STATE_UNUSED,
		"PipeWire and SPA unused buffer state");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_BUFFER_STATE_AVAILABLE ==
		(uint32_t) SPA_IMAGE_BUFFER_STATE_AVAILABLE,
		"PipeWire and SPA available buffer state");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_BUFFER_STATE_PRODUCER ==
		(uint32_t) SPA_IMAGE_BUFFER_STATE_PRODUCER,
		"PipeWire and SPA producer buffer state");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_BUFFER_STATE_PROGRESSIVE ==
		(uint32_t) SPA_IMAGE_BUFFER_STATE_PROGRESSIVE,
		"PipeWire and SPA progressive buffer state");
SPA_STATIC_ASSERT((uint32_t) PW_IMAGE_BUFFER_STATE_PUBLISHED ==
		(uint32_t) SPA_IMAGE_BUFFER_STATE_PUBLISHED,
		"PipeWire and SPA published buffer state");

static inline struct spa_image_source_buffer *spa_buffer_from_pw(
		struct pw_image_buffer *buffer)
{
	return (struct spa_image_source_buffer *) buffer;
}

static inline const struct spa_image_source_buffer *spa_buffer_from_pw_const(
		const struct pw_image_buffer *buffer)
{
	return (const struct spa_image_source_buffer *) buffer;
}

static inline struct pw_image_buffer *pw_buffer_from_spa(
		struct spa_image_source_buffer *buffer)
{
	return (struct pw_image_buffer *) buffer;
}

static int transport_try_dequeue_reusable(void *data, void **handle)
{
	struct pw_buffer *buffer = NULL;
	int res = pw_stream_try_dequeue_buffer_reusable(data, &buffer);

	*handle = buffer;
	return res;
}

static int transport_try_reclaim_submission(void *data, void **handle)
{
	struct pw_buffer *buffer = NULL;
	int res = pw_stream_try_reclaim_buffer_latest(data, &buffer);

	*handle = buffer;
	return res;
}

static int transport_publish_complete(void *data, void *handle)
{
	return pw_stream_queue_buffer(data, handle);
}

static int transport_begin_progressive(void *data, void *handle)
{
	return pw_stream_begin_progressive_buffer(data, handle);
}

static int transport_end_progressive(void *data, void *handle)
{
	return pw_stream_end_progressive_buffer(data, handle);
}

static int transport_return_buffer(void *data, void *handle)
{
	return pw_stream_return_buffer(data, handle);
}

static const struct spa_image_source_transport stream_transport = {
	.try_dequeue_reusable = transport_try_dequeue_reusable,
	.try_reclaim_submission = transport_try_reclaim_submission,
	.publish_complete = transport_publish_complete,
	.begin_progressive = transport_begin_progressive,
	.end_progressive = transport_end_progressive,
	.return_buffer = transport_return_buffer,
};

static inline struct spa_image_frame spa_frame_from_pw(
		const struct pw_image_frame *frame)
{
	return (struct spa_image_frame) {
		.version = frame->version,
		.data_index = frame->data_index,
		.header_flags = frame->header_flags,
		.chunk_flags = frame->chunk_flags,
		.offset = frame->offset,
		.size = frame->size,
		.stride = frame->stride,
		.reserved = frame->reserved,
		.sequence = frame->sequence,
		.pts = frame->pts,
		.acquisition = frame->acquisition,
	};
}

static inline struct spa_image_progressive spa_progressive_from_pw(
		const struct pw_image_progressive *progressive)
{
	return (struct spa_image_progressive) {
		.version = progressive->version,
		.payload_size = progressive->payload_size,
		.commit_granularity = progressive->commit_granularity,
		.committed = progressive->committed,
	};
}

SPA_EXPORT
struct pw_image_source *pw_image_source_new(struct pw_stream *stream,
		const struct pw_image_source_config *config)
{
	struct spa_image_source_config spa_config;
	struct pw_image_source *source;
	int res;

	if (stream == NULL || config == NULL) {
		errno = EINVAL;
		return NULL;
	}
	spa_config = (struct spa_image_source_config) {
		.version = config->version,
		.min_buffers = config->min_buffers,
		.max_buffers = config->max_buffers,
		.flags = config->flags,
	};
	if ((source = calloc(1, sizeof(*source))) == NULL)
		return NULL;
	res = spa_image_source_init(&source->source, &spa_config,
			&stream_transport, stream);
	if (res < 0) {
		free(source);
		errno = -res;
		return NULL;
	}
	source->stream = stream;
	return source;
}

SPA_EXPORT
void pw_image_source_destroy(struct pw_image_source *source)
{
	if (source == NULL)
		return;
	if (source->source.prepared)
		(void) pw_image_source_teardown(source);
	free(source);
}

SPA_EXPORT
int pw_image_source_prepare(struct pw_image_source *source)
{
	struct spa_image_source_buffer_info buffers[PW_IMAGE_SOURCE_MAX_BUFFERS];
	struct pw_buffer *buffer;
	uint32_t n_buffers = 0, i;
	int res;

	if (source == NULL)
		return -EINVAL;
	if (source->source.prepared) {
		spa_image_source_increment(&source->source.stats.prepare_calls);
		return spa_image_source_transition_error(&source->source);
	}
	if ((res = pw_stream_buffer_latest_worker_begin(source->stream)) < 0) {
		spa_image_source_increment(&source->source.stats.prepare_calls);
		return res;
	}
	while (n_buffers < source->source.config.max_buffers &&
			(buffer = pw_stream_dequeue_buffer(source->stream)) != NULL) {
		buffers[n_buffers].buffer = buffer->buffer;
		buffers[n_buffers].handle = buffer;
		n_buffers++;
	}
	if (n_buffers == source->source.config.max_buffers &&
			(buffer = pw_stream_dequeue_buffer(source->stream)) != NULL) {
		(void) pw_stream_return_buffer(source->stream, buffer);
		res = -E2BIG;
		spa_image_source_increment(&source->source.stats.prepare_calls);
		goto error;
	}
	res = spa_image_source_prepare(&source->source, buffers, n_buffers);
	if (res >= 0)
		return res;

error:
	for (i = 0; i < n_buffers; i++)
		(void) pw_stream_return_buffer(source->stream, buffers[i].handle);
	(void) pw_stream_buffer_latest_worker_end(source->stream);
	return res;
}

SPA_EXPORT
uint32_t pw_image_source_get_n_buffers(const struct pw_image_source *source)
{
	return source == NULL ? 0 :
		spa_image_source_get_n_buffers(&source->source);
}

SPA_EXPORT
struct pw_image_buffer *pw_image_source_get_buffer(
		struct pw_image_source *source, uint32_t index)
{
	return source == NULL ? NULL : pw_buffer_from_spa(
			spa_image_source_get_buffer(&source->source, index));
}

SPA_EXPORT
uint32_t pw_image_buffer_get_index(const struct pw_image_buffer *buffer)
{
	return spa_image_source_buffer_get_index(spa_buffer_from_pw_const(buffer));
}

SPA_EXPORT
enum pw_image_buffer_state pw_image_buffer_get_state(
		const struct pw_image_buffer *buffer)
{
	return (enum pw_image_buffer_state) spa_image_source_buffer_get_state(
			spa_buffer_from_pw_const(buffer));
}

SPA_EXPORT
struct pw_buffer *pw_image_buffer_get_pw_buffer(
		const struct pw_image_buffer *buffer)
{
	return spa_image_source_buffer_get_handle(
			spa_buffer_from_pw_const(buffer));
}

SPA_EXPORT
void *pw_image_buffer_get_user_data(const struct pw_image_buffer *buffer)
{
	return spa_image_source_buffer_get_user_data(
			spa_buffer_from_pw_const(buffer));
}

SPA_EXPORT
void pw_image_buffer_set_user_data(struct pw_image_buffer *buffer,
		void *user_data)
{
	spa_image_source_buffer_set_user_data(spa_buffer_from_pw(buffer),
			user_data);
}

SPA_EXPORT
int pw_image_source_try_acquire(struct pw_image_source *source,
		struct pw_image_buffer **buffer)
{
	struct spa_image_source_buffer *spa_buffer = NULL;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	res = spa_image_source_try_acquire(&source->source, &spa_buffer);
	*buffer = pw_buffer_from_spa(spa_buffer);
	return res;
}

SPA_EXPORT
int pw_image_source_return_buffer(struct pw_image_source *source,
		struct pw_image_buffer *buffer)
{
	return source == NULL ? -EINVAL : spa_image_source_return_buffer(
			&source->source, spa_buffer_from_pw(buffer));
}

SPA_EXPORT
int pw_image_source_publish_complete(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame)
{
	struct spa_image_frame spa_frame;
	const struct spa_image_frame *spa_frame_ptr = NULL;

	if (source == NULL)
		return -EINVAL;
	if (frame != NULL) {
		spa_frame = spa_frame_from_pw(frame);
		spa_frame_ptr = &spa_frame;
	}
	return spa_image_source_publish_complete(&source->source,
			spa_buffer_from_pw(buffer), spa_frame_ptr);
}

SPA_EXPORT
int pw_image_source_begin_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame,
		const struct pw_image_progressive *progressive)
{
	struct spa_image_progressive spa_progressive;
	struct spa_image_frame spa_frame;
	const struct spa_image_progressive *spa_progressive_ptr = NULL;
	const struct spa_image_frame *spa_frame_ptr = NULL;

	if (source == NULL)
		return -EINVAL;
	if (frame != NULL) {
		spa_frame = spa_frame_from_pw(frame);
		spa_frame_ptr = &spa_frame;
	}
	if (progressive != NULL) {
		spa_progressive = spa_progressive_from_pw(progressive);
		spa_progressive_ptr = &spa_progressive;
	}
	return spa_image_source_begin_progressive(&source->source,
			spa_buffer_from_pw(buffer), spa_frame_ptr,
			spa_progressive_ptr);
}

SPA_EXPORT
int pw_image_source_update_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed)
{
	return source == NULL ? -EINVAL : spa_image_source_update_progressive(
			&source->source, spa_buffer_from_pw(buffer), committed);
}

SPA_EXPORT
int pw_image_source_finish_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed,
		enum spa_meta_progressive_state state, uint32_t terminal_flags)
{
	return source == NULL ? -EINVAL : spa_image_source_finish_progressive(
			&source->source, spa_buffer_from_pw(buffer), committed, state,
			terminal_flags);
}

SPA_EXPORT
int pw_image_source_try_reclaim(struct pw_image_source *source,
		struct pw_image_buffer **buffer)
{
	struct spa_image_source_buffer *spa_buffer = NULL;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	res = spa_image_source_try_reclaim_submission(&source->source,
			&spa_buffer);
	*buffer = pw_buffer_from_spa(spa_buffer);
	return res;
}

SPA_EXPORT
int pw_image_source_teardown(struct pw_image_source *source)
{
	int first_error, res;

	if (source == NULL)
		return -EINVAL;
	if (!source->source.prepared)
		return 0;
	first_error = spa_image_source_teardown(&source->source);
	res = pw_stream_buffer_latest_worker_end(source->stream);
	return first_error < 0 ? first_error : res;
}

SPA_EXPORT
int pw_image_source_get_stats(const struct pw_image_source *source,
		struct pw_image_source_stats *stats, size_t stats_size)
{
	struct spa_image_source_stats spa_stats;
	int res;

	if (source == NULL || stats == NULL)
		return -EINVAL;
	if (stats_size < sizeof(*stats))
		return -ENOSPC;
	if ((res = spa_image_source_get_stats(&source->source, &spa_stats,
			sizeof(spa_stats))) < 0)
		return res;
	*stats = (struct pw_image_source_stats) {
		.prepare_calls = spa_stats.prepare_calls,
		.acquire_calls = spa_stats.acquire_calls,
		.available_acquisitions = spa_stats.available_acquisitions,
		.reusable_acquisitions = spa_stats.reusable_acquisitions,
		.pool_exhaustions = spa_stats.pool_exhaustions,
		.forced_reclaims = spa_stats.forced_reclaims,
		.producer_returns = spa_stats.producer_returns,
		.complete_publications = spa_stats.complete_publications,
		.progressive_started = spa_stats.progressive_started,
		.progressive_updates = spa_stats.progressive_updates,
		.progressive_completed = spa_stats.progressive_completed,
		.progressive_aborted = spa_stats.progressive_aborted,
		.invalid_transitions = spa_stats.invalid_transitions,
		.metadata_errors = spa_stats.metadata_errors,
		.teardown_returns = spa_stats.teardown_returns,
		.pool_size = spa_stats.pool_size,
		.max_available_probes = spa_stats.max_available_probes,
	};
	return 0;
}
