/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/image-source.h>
#include <pipewire/stream.h>

#include <spa/buffer/buffer.h>
#include <spa/utils/defs.h>

struct pw_image_buffer {
	struct pw_image_source *source;
	struct pw_buffer *buffer;
	void *user_data;
	uint32_t index;
	enum pw_image_buffer_state state;
};

struct pw_image_source {
	struct pw_stream *stream;
	struct pw_image_source_config config;
	struct pw_image_source_stats stats;
	struct pw_image_buffer buffers[PW_IMAGE_SOURCE_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	bool prepared;
};

static inline void increment(uint64_t *value)
{
	if (*value != UINT64_MAX)
		(*value)++;
}

static int transition_error(struct pw_image_source *source)
{
	increment(&source->stats.invalid_transitions);
	return -EINVAL;
}

static int metadata_error(struct pw_image_source *source, int error)
{
	increment(&source->stats.metadata_errors);
	return error;
}

static struct pw_image_buffer *find_buffer(struct pw_image_source *source,
		struct pw_buffer *buffer)
{
	uint32_t i;

	for (i = 0; i < source->n_buffers; i++) {
		if (source->buffers[i].buffer == buffer)
			return &source->buffers[i];
	}
	return NULL;
}

static bool acquisition_valid(const struct spa_meta_acquisition *acquisition)
{
	struct spa_meta meta = {
		.type = SPA_META_Acquisition,
		.size = sizeof(*acquisition),
		.data = (void *) acquisition,
	};

	return acquisition != NULL && spa_meta_acquisition_is_valid(&meta);
}

static int prepare_frame(struct pw_image_source *source,
		struct pw_image_buffer *image_buffer,
		const struct pw_image_frame *frame)
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
	uint32_t valid_chunk_flags = SPA_CHUNK_FLAG_CORRUPTED | SPA_CHUNK_FLAG_EMPTY;
	uint64_t sequence;
	int64_t pts;

	if (frame == NULL || frame->version != PW_VERSION_IMAGE_FRAME ||
			frame->reserved != 0 ||
			(frame->header_flags & ~valid_header_flags) != 0 ||
			(frame->chunk_flags & ~valid_chunk_flags) != 0 ||
			(frame->pts < 0 && frame->pts != SPA_TIME_INVALID))
		return metadata_error(source, -EINVAL);
	buffer = image_buffer->buffer->buffer;
	if (buffer == NULL || frame->data_index >= buffer->n_datas)
		return metadata_error(source, -EINVAL);
	data = &buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + frame->size;
	if (data->chunk == NULL || end > data->maxsize)
		return metadata_error(source, -ENOSPC);
	if (frame->acquisition != NULL && !acquisition_valid(frame->acquisition))
		return metadata_error(source, -EINVAL);

	header = spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(*header));
	if (header == NULL &&
			(source->config.flags & PW_IMAGE_SOURCE_FLAG_REQUIRE_HEADER) != 0)
		return metadata_error(source, -ENOTSUP);
	acquisition = spa_buffer_find_meta_data(buffer, SPA_META_Acquisition,
			sizeof(*acquisition));
	if (acquisition == NULL &&
			((source->config.flags & PW_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION) != 0 ||
			 frame->acquisition != NULL))
		return metadata_error(source, -ENOTSUP);
	if (acquisition != NULL && !SPA_IS_ALIGNED(acquisition, 8))
		return metadata_error(source, -EINVAL);

	data->chunk->offset = frame->offset;
	data->chunk->size = frame->size;
	data->chunk->stride = frame->stride;
	data->chunk->flags = frame->chunk_flags;

	if (acquisition != NULL) {
		if (frame->acquisition == NULL) {
			if (!spa_meta_acquisition_init(acquisition))
				return metadata_error(source, -EINVAL);
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

SPA_EXPORT
struct pw_image_source *pw_image_source_new(struct pw_stream *stream,
		const struct pw_image_source_config *config)
{
	struct pw_image_source *source;

	if (stream == NULL || config == NULL ||
			config->version != PW_VERSION_IMAGE_SOURCE_CONFIG ||
			config->min_buffers == 0 ||
			config->max_buffers < config->min_buffers ||
			config->max_buffers > PW_IMAGE_SOURCE_MAX_BUFFERS ||
			(config->flags & ~PW_IMAGE_SOURCE_FLAG_ALL) != 0) {
		errno = EINVAL;
		return NULL;
	}
	if ((source = calloc(1, sizeof(*source))) == NULL)
		return NULL;
	source->stream = stream;
	source->config = *config;
	return source;
}

SPA_EXPORT
void pw_image_source_destroy(struct pw_image_source *source)
{
	if (source == NULL)
		return;
	if (source->prepared)
		(void) pw_image_source_teardown(source);
	free(source);
}

SPA_EXPORT
int pw_image_source_prepare(struct pw_image_source *source)
{
	struct pw_buffer *buffer;
	uint32_t i;
	int res;

	if (source == NULL)
		return -EINVAL;
	increment(&source->stats.prepare_calls);
	if (source->prepared)
		return transition_error(source);
	if ((res = pw_stream_buffer_latest_worker_begin(source->stream)) < 0)
		return res;

	while (source->n_buffers < source->config.max_buffers &&
			(buffer = pw_stream_dequeue_buffer(source->stream)) != NULL) {
		struct pw_image_buffer *slot = &source->buffers[source->n_buffers];

		slot->source = source;
		slot->buffer = buffer;
		slot->index = source->n_buffers;
		slot->state = PW_IMAGE_BUFFER_STATE_AVAILABLE;
		source->n_buffers++;
	}
	if (source->n_buffers < source->config.min_buffers) {
		res = -ENOBUFS;
		goto error;
	}
	if (source->n_buffers == source->config.max_buffers &&
			(buffer = pw_stream_dequeue_buffer(source->stream)) != NULL) {
		(void) pw_stream_return_buffer(source->stream, buffer);
		res = -E2BIG;
		goto error;
	}
	source->prepared = true;
	source->scan_hint = 0;
	source->stats.pool_size = source->n_buffers;
	return (int) source->n_buffers;

error:
	for (i = 0; i < source->n_buffers; i++) {
		(void) pw_stream_return_buffer(source->stream,
				source->buffers[i].buffer);
		memset(&source->buffers[i], 0, sizeof(source->buffers[i]));
	}
	source->n_buffers = 0;
	source->stats.pool_size = 0;
	(void) pw_stream_buffer_latest_worker_end(source->stream);
	return res;
}

SPA_EXPORT
uint32_t pw_image_source_get_n_buffers(const struct pw_image_source *source)
{
	return source == NULL ? 0 : source->n_buffers;
}

SPA_EXPORT
struct pw_image_buffer *pw_image_source_get_buffer(
		struct pw_image_source *source, uint32_t index)
{
	return source == NULL || index >= source->n_buffers ? NULL :
		&source->buffers[index];
}

SPA_EXPORT
uint32_t pw_image_buffer_get_index(const struct pw_image_buffer *buffer)
{
	return buffer == NULL ? SPA_ID_INVALID : buffer->index;
}

SPA_EXPORT
enum pw_image_buffer_state pw_image_buffer_get_state(
		const struct pw_image_buffer *buffer)
{
	return buffer == NULL ? PW_IMAGE_BUFFER_STATE_UNUSED : buffer->state;
}

SPA_EXPORT
struct pw_buffer *pw_image_buffer_get_pw_buffer(
		const struct pw_image_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->buffer;
}

SPA_EXPORT
void *pw_image_buffer_get_user_data(const struct pw_image_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->user_data;
}

SPA_EXPORT
void pw_image_buffer_set_user_data(struct pw_image_buffer *buffer,
		void *user_data)
{
	if (buffer != NULL)
		buffer->user_data = user_data;
}

static struct pw_image_buffer *take_available(struct pw_image_source *source,
		uint32_t *probes)
{
	uint32_t i;

	for (i = 0; i < source->n_buffers; i++) {
		uint32_t index = source->scan_hint + i;
		struct pw_image_buffer *buffer;

		if (index >= source->n_buffers)
			index -= source->n_buffers;
		buffer = &source->buffers[index];

		if (buffer->state == PW_IMAGE_BUFFER_STATE_AVAILABLE) {
			buffer->state = PW_IMAGE_BUFFER_STATE_PRODUCER;
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

static int take_dequeued(struct pw_image_source *source,
		struct pw_buffer *pw_buffer, struct pw_image_buffer **buffer)
{
	struct pw_image_buffer *image_buffer = find_buffer(source, pw_buffer);

	if (image_buffer == NULL ||
			image_buffer->state != PW_IMAGE_BUFFER_STATE_PUBLISHED)
		return transition_error(source);
	image_buffer->state = PW_IMAGE_BUFFER_STATE_PRODUCER;
	source->scan_hint = image_buffer->index + 1;
	if (source->scan_hint == source->n_buffers)
		source->scan_hint = 0;
	*buffer = image_buffer;
	return 1;
}

SPA_EXPORT
int pw_image_source_try_acquire(struct pw_image_source *source,
		struct pw_image_buffer **buffer)
{
	struct pw_buffer *pw_buffer = NULL;
	struct pw_image_buffer *image_buffer;
	uint32_t probes;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	increment(&source->stats.acquire_calls);
	if (!source->prepared)
		return transition_error(source);

	image_buffer = take_available(source, &probes);
	if (probes > source->stats.max_available_probes)
		source->stats.max_available_probes = probes;
	if (image_buffer != NULL) {
		increment(&source->stats.available_acquisitions);
		*buffer = image_buffer;
		return 1;
	}

	res = pw_stream_try_dequeue_buffer_reusable(source->stream, &pw_buffer);
	if (res <= 0) {
		if (res == 0)
			increment(&source->stats.pool_exhaustions);
		return res;
	}
	if ((res = take_dequeued(source, pw_buffer, buffer)) < 0)
		return res;
	increment(&source->stats.reusable_acquisitions);
	return 1;
}

SPA_EXPORT
int pw_image_source_return_buffer(struct pw_image_source *source,
		struct pw_image_buffer *buffer)
{
	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared || buffer->state != PW_IMAGE_BUFFER_STATE_PRODUCER)
		return transition_error(source);
	buffer->state = PW_IMAGE_BUFFER_STATE_AVAILABLE;
	source->scan_hint = buffer->index;
	increment(&source->stats.producer_returns);
	return 0;
}

SPA_EXPORT
int pw_image_source_publish_complete(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame)
{
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared || buffer->state != PW_IMAGE_BUFFER_STATE_PRODUCER)
		return transition_error(source);
	if ((res = prepare_frame(source, buffer, frame)) < 0)
		return res;
	if ((res = pw_stream_queue_buffer(source->stream, buffer->buffer)) < 0)
		return res;
	buffer->state = PW_IMAGE_BUFFER_STATE_PUBLISHED;
	increment(&source->stats.complete_publications);
	return 0;
}

SPA_EXPORT
int pw_image_source_begin_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer,
		const struct pw_image_frame *frame,
		const struct pw_image_progressive *progressive)
{
	struct spa_meta_progressive *meta;
	struct spa_data *data;
	uint64_t end;
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source ||
			progressive == NULL)
		return -EINVAL;
	if (!source->prepared || buffer->state != PW_IMAGE_BUFFER_STATE_PRODUCER)
		return transition_error(source);
	if ((source->config.flags & PW_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE) == 0)
		return -ENOTSUP;
	if (progressive->version != PW_VERSION_IMAGE_PROGRESSIVE ||
			progressive->payload_size == 0 ||
			progressive->commit_granularity == 0 ||
			progressive->commit_granularity > progressive->payload_size ||
			progressive->committed > progressive->payload_size ||
			(progressive->committed != progressive->payload_size &&
			 progressive->committed % progressive->commit_granularity != 0) ||
			frame == NULL || frame->size != progressive->payload_size)
		return metadata_error(source, -EINVAL);
	if (frame->data_index >= buffer->buffer->buffer->n_datas)
		return metadata_error(source, -EINVAL);
	data = &buffer->buffer->buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + progressive->payload_size;
	if (data->type == SPA_DATA_DmaBuf)
		return -ENOTSUP;
	if (data->data == NULL || end > data->maxsize)
		return metadata_error(source, -ENOSPC);
	meta = spa_buffer_find_meta_data(buffer->buffer->buffer,
			SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL)
		return metadata_error(source, -ENOTSUP);
	if ((res = prepare_frame(source, buffer, frame)) < 0)
		return res;
	if (!spa_meta_progressive_init(meta, frame->data_index, frame->offset,
			progressive->payload_size, progressive->commit_granularity))
		return metadata_error(source, -EINVAL);
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(progressive->committed,
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	if ((res = pw_stream_begin_progressive_buffer(source->stream,
			buffer->buffer)) < 0) {
		(void) spa_meta_progressive_init(meta, frame->data_index, frame->offset,
				progressive->payload_size, progressive->commit_granularity);
		return res;
	}
	buffer->state = PW_IMAGE_BUFFER_STATE_PROGRESSIVE;
	increment(&source->stats.progressive_started);
	return 0;
}

SPA_EXPORT
int pw_image_source_update_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed)
{
	struct spa_meta_progressive *meta;
	enum spa_meta_progressive_state state;
	uint32_t current;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared || buffer->state != PW_IMAGE_BUFFER_STATE_PROGRESSIVE)
		return transition_error(source);
	meta = spa_buffer_find_meta_data(buffer->buffer->buffer,
			SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL || !spa_meta_progressive_snapshot_decode(
			spa_meta_progressive_load_acquire(meta), &current, &state) ||
			state != SPA_META_PROGRESSIVE_STATE_ACTIVE ||
			committed < current || committed > meta->payload_size ||
			(committed != meta->payload_size &&
			 committed % meta->commit_granularity != 0))
		return metadata_error(source, -EINVAL);
	if (committed == current)
		return 0;
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(committed,
				SPA_META_PROGRESSIVE_STATE_ACTIVE));
	increment(&source->stats.progressive_updates);
	return 1;
}

SPA_EXPORT
int pw_image_source_finish_progressive(struct pw_image_source *source,
		struct pw_image_buffer *buffer, uint32_t committed,
		enum spa_meta_progressive_state state, uint32_t terminal_flags)
{
	struct spa_meta_progressive *meta;
	enum spa_meta_progressive_state current_state;
	uint32_t current;
	int res;

	if (source == NULL || buffer == NULL || buffer->source != source)
		return -EINVAL;
	if (!source->prepared || buffer->state != PW_IMAGE_BUFFER_STATE_PROGRESSIVE)
		return transition_error(source);
	meta = spa_buffer_find_meta_data(buffer->buffer->buffer,
			SPA_META_Progressive, sizeof(*meta));
	if (meta == NULL || !spa_meta_progressive_snapshot_decode(
			spa_meta_progressive_load_acquire(meta), &current, &current_state) ||
			current_state != SPA_META_PROGRESSIVE_STATE_ACTIVE ||
			(state != SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			 state != SPA_META_PROGRESSIVE_STATE_ABORTED) ||
			(terminal_flags & ~SPA_META_PROGRESSIVE_FLAG_ALL) != 0 ||
			committed < current || committed > meta->payload_size ||
			(committed != meta->payload_size &&
			 committed % meta->commit_granularity != 0) ||
			(state == SPA_META_PROGRESSIVE_STATE_COMPLETE &&
			 (committed != meta->payload_size || terminal_flags != 0)))
		return metadata_error(source, -EINVAL);
	meta->terminal_flags = terminal_flags;
	spa_meta_progressive_store_release(meta,
			spa_meta_progressive_snapshot_encode(committed, state));
	if ((res = pw_stream_end_progressive_buffer(source->stream,
			buffer->buffer)) < 0)
		return res;
	buffer->state = PW_IMAGE_BUFFER_STATE_PUBLISHED;
	if (state == SPA_META_PROGRESSIVE_STATE_COMPLETE)
		increment(&source->stats.progressive_completed);
	else
		increment(&source->stats.progressive_aborted);
	return 0;
}

SPA_EXPORT
int pw_image_source_try_reclaim(struct pw_image_source *source,
		struct pw_image_buffer **buffer)
{
	struct pw_buffer *pw_buffer = NULL;
	int res;

	if (source == NULL || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	if (!source->prepared)
		return transition_error(source);
	res = pw_stream_try_reclaim_buffer_latest(source->stream, &pw_buffer);
	if (res <= 0)
		return res;
	if ((res = take_dequeued(source, pw_buffer, buffer)) < 0)
		return res;
	increment(&source->stats.forced_reclaims);
	return 1;
}

SPA_EXPORT
int pw_image_source_teardown(struct pw_image_source *source)
{
	uint32_t i;
	int first_error = 0;

	if (source == NULL)
		return -EINVAL;
	if (!source->prepared)
		return 0;
	for (i = 0; i < source->n_buffers; i++) {
		struct pw_image_buffer *buffer = &source->buffers[i];
		int res = 0;

		if (buffer->state == PW_IMAGE_BUFFER_STATE_PROGRESSIVE) {
			struct spa_meta_progressive *meta = spa_buffer_find_meta_data(
					buffer->buffer->buffer, SPA_META_Progressive,
					sizeof(*meta));
			uint32_t committed = 0;
			enum spa_meta_progressive_state state;

			if (meta == NULL || !spa_meta_progressive_snapshot_decode(
					spa_meta_progressive_load_acquire(meta),
					&committed, &state)) {
				res = -EPROTO;
			} else {
				meta->terminal_flags = SPA_META_PROGRESSIVE_FLAG_CANCELLED;
				spa_meta_progressive_store_release(meta,
						spa_meta_progressive_snapshot_encode(committed,
							SPA_META_PROGRESSIVE_STATE_ABORTED));
				res = pw_stream_end_progressive_buffer(source->stream,
						buffer->buffer);
				if (res == 0) {
					buffer->state = PW_IMAGE_BUFFER_STATE_PUBLISHED;
					increment(&source->stats.progressive_aborted);
				}
			}
		}
		if (res == 0 && (buffer->state == PW_IMAGE_BUFFER_STATE_AVAILABLE ||
				buffer->state == PW_IMAGE_BUFFER_STATE_PRODUCER)) {
			res = pw_stream_return_buffer(source->stream, buffer->buffer);
			if (res == 0)
				increment(&source->stats.teardown_returns);
		}
		if (res < 0 && first_error == 0)
			first_error = res;
	}
	{
		int res = pw_stream_buffer_latest_worker_end(source->stream);
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

SPA_EXPORT
int pw_image_source_get_stats(const struct pw_image_source *source,
		struct pw_image_source_stats *stats, size_t stats_size)
{
	if (source == NULL || stats == NULL)
		return -EINVAL;
	if (stats_size < sizeof(*stats))
		return -ENOSPC;
	memcpy(stats, &source->stats, sizeof(*stats));
	return 0;
}
