/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BUFFER_IMAGE_SOURCE_LATEST_H
#define SPA_BUFFER_IMAGE_SOURCE_LATEST_H

#include <errno.h>
#include <stdint.h>

#include <spa/buffer/image-source.h>
#include <spa/node/buffer-latest.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_IMAGE_SOURCE_LATEST
#ifdef SPA_API_IMPL
#define SPA_API_IMAGE_SOURCE_LATEST SPA_API_IMPL
#else
#define SPA_API_IMAGE_SOURCE_LATEST static inline
#endif
#endif

/**
 * \addtogroup spa_buffer
 * \{
 */

struct spa_image_source_latest;

struct spa_image_source_latest_handle {
	struct spa_image_source_latest *owner;
	uint32_t id;
	uint32_t reserved;
};

/**
 * Bind image-source ownership to a latest-buffer output transport.
 *
 * Preparation marks the complete negotiated pool as producer-owned in both
 * state machines. The exclusive worker may then use the ordinary
 * spa_image_source acquire and publication API without translating buffer
 * IDs or introducing another queue.
 */
struct spa_image_source_latest {
	struct spa_buffer_latest *latest;
	uint32_t n_buffers;
	uint32_t reserved;
	struct spa_image_source_latest_handle handles[SPA_IMAGE_SOURCE_MAX_BUFFERS];
};

static inline int spa_image_source_latest_handle_id(
		struct spa_image_source_latest *transport, void *handle,
		uint32_t *id)
{
	struct spa_image_source_latest_handle *latest_handle =
			(struct spa_image_source_latest_handle *) handle;

	if (transport == NULL || latest_handle == NULL || id == NULL ||
			latest_handle->owner != transport || latest_handle->reserved != 0 ||
			latest_handle->id >= transport->n_buffers)
		return -EINVAL;
	*id = latest_handle->id;
	return 0;
}

static inline int spa_image_source_latest_try_dequeue_reusable(
		void *data, void **handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if (transport == NULL || handle == NULL)
		return -EINVAL;
	*handle = NULL;
	res = spa_buffer_latest_try_dequeue_reusable(transport->latest, &id);
	if (res > 0) {
		if (SPA_UNLIKELY(id >= transport->n_buffers))
			return -EPROTO;
		*handle = &transport->handles[id];
	}
	return res;
}

static inline int spa_image_source_latest_try_reclaim_submission(
		void *data, void **handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if (transport == NULL || handle == NULL)
		return -EINVAL;
	*handle = NULL;
	res = spa_buffer_latest_try_reclaim_submission(transport->latest, &id);
	if (res > 0) {
		if (SPA_UNLIKELY(id >= transport->n_buffers))
			return -EPROTO;
		*handle = &transport->handles[id];
	}
	return res;
}

static inline int spa_image_source_latest_publish_complete(void *data,
		void *handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if ((res = spa_image_source_latest_handle_id(transport, handle, &id)) < 0)
		return res;
	return spa_buffer_latest_queue(transport->latest, id);
}

static inline int spa_image_source_latest_begin_progressive(void *data,
		void *handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if ((res = spa_image_source_latest_handle_id(transport, handle, &id)) < 0)
		return res;
	return spa_buffer_latest_begin_progressive(transport->latest, id);
}

static inline int spa_image_source_latest_end_progressive(void *data,
		void *handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if ((res = spa_image_source_latest_handle_id(transport, handle, &id)) < 0)
		return res;
	return spa_buffer_latest_end_progressive(transport->latest, id);
}

static inline int spa_image_source_latest_return_buffer(void *data,
		void *handle)
{
	struct spa_image_source_latest *transport =
			(struct spa_image_source_latest *) data;
	uint32_t id;
	int res;

	if ((res = spa_image_source_latest_handle_id(transport, handle, &id)) < 0)
		return res;
	return spa_buffer_latest_return(transport->latest, id);
}

static const struct spa_image_source_transport
spa_image_source_latest_transport_methods = {
	.try_dequeue_reusable = spa_image_source_latest_try_dequeue_reusable,
	.try_reclaim_submission = spa_image_source_latest_try_reclaim_submission,
	.publish_complete = spa_image_source_latest_publish_complete,
	.begin_progressive = spa_image_source_latest_begin_progressive,
	.end_progressive = spa_image_source_latest_end_progressive,
	.return_buffer = spa_image_source_latest_return_buffer,
};

SPA_API_IMAGE_SOURCE_LATEST int spa_image_source_latest_init(
		struct spa_image_source_latest *transport,
		struct spa_image_source *source,
		struct spa_buffer_latest *latest,
		const struct spa_image_source_config *config)
{
	if (transport == NULL || source == NULL || latest == NULL)
		return -EINVAL;
	memset(transport, 0, sizeof(*transport));
	transport->latest = latest;
	spa_buffer_latest_enable(latest);
	return spa_image_source_init(source, config,
			&spa_image_source_latest_transport_methods, transport);
}

SPA_API_IMAGE_SOURCE_LATEST int spa_image_source_latest_prepare(
		struct spa_image_source_latest *transport,
		struct spa_image_source *source,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct spa_image_source_buffer_info infos[SPA_IMAGE_SOURCE_MAX_BUFFERS];
	uint64_t acquired = 0;
	uint32_t count = 0, i, id;
	int res;

	if (transport == NULL || source == NULL || buffers == NULL ||
			transport->latest == NULL || transport->n_buffers != 0 ||
			n_buffers == 0 || n_buffers > SPA_IMAGE_SOURCE_MAX_BUFFERS)
		return -EINVAL;
	spa_buffer_latest_set_buffers(transport->latest, buffers, n_buffers);
	transport->n_buffers = n_buffers;
	for (i = 0; i < n_buffers; i++) {
		transport->handles[i] = (struct spa_image_source_latest_handle) {
			.owner = transport,
			.id = i,
		};
		infos[i] = (struct spa_image_source_buffer_info) {
			.buffer = buffers[i],
			.handle = &transport->handles[i],
		};
	}
	while (count < n_buffers) {
		res = spa_buffer_latest_try_dequeue_reusable(transport->latest, &id);
		if (res <= 0)
			goto error;
		if (SPA_UNLIKELY(id >= n_buffers ||
				(acquired & (UINT64_C(1) << id)) != 0)) {
			res = -EPROTO;
			goto error;
		}
		acquired |= UINT64_C(1) << id;
		count++;
	}
	res = spa_image_source_prepare(source, infos, n_buffers);
	if (res >= 0)
		return res;

error:
	for (i = 0; i < n_buffers; i++)
		if ((acquired & (UINT64_C(1) << i)) != 0)
			(void) spa_buffer_latest_return(transport->latest, i);
	spa_buffer_latest_clear_buffers(transport->latest);
	memset(transport->handles, 0, sizeof(transport->handles));
	transport->n_buffers = 0;
	return res < 0 ? res : -EPIPE;
}

/**
 * Tear down a quiescent source after the worker and all links have retired.
 */
SPA_API_IMAGE_SOURCE_LATEST int spa_image_source_latest_teardown(
		struct spa_image_source_latest *transport,
		struct spa_image_source *source)
{
	int res;

	if (transport == NULL || source == NULL || transport->latest == NULL)
		return -EINVAL;
	if (spa_buffer_latest_worker_is_active(transport->latest) ||
			spa_buffer_latest_has_links(transport->latest))
		return -EBUSY;
	if ((res = spa_buffer_latest_service_retirements(transport->latest)) < 0)
		return res;
	res = spa_image_source_teardown(source);
	spa_buffer_latest_clear_buffers(transport->latest);
	memset(transport->handles, 0, sizeof(transport->handles));
	transport->n_buffers = 0;
	return res;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_BUFFER_IMAGE_SOURCE_LATEST_H */
