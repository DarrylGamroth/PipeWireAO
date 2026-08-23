/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aravis_camera {
	ArvCamera *camera;
	ArvStream *stream;
	struct aravis_camera_info info;
	bool started;
};

static int clear_error(GError **error, int result)
{
	g_clear_error(error);
	return result;
}

static int copy_text(char *destination, size_t capacity, const char *source)
{
	int written;

	if (source == NULL)
		source = "";
	written = snprintf(destination, capacity, "%s", source);
	return written >= 0 && (size_t)written < capacity ? 0 : -ENAMETOOLONG;
}

static int query_info(struct aravis_camera *camera)
{
	GError *error = NULL;
	const char *text;
	gint x, y, width, height;
	guint payload;
	int res;

	payload = arv_camera_get_payload(camera->camera, &error);
	if (error != NULL || payload == 0)
		return clear_error(&error, -EIO);
	arv_camera_get_region(camera->camera, &x, &y, &width, &height, &error);
	if (error != NULL || x < 0 || y < 0 || width <= 0 || height <= 0)
		return clear_error(&error, -EIO);

	camera->info.payload_size = payload;
	camera->info.offset_x = (uint32_t)x;
	camera->info.offset_y = (uint32_t)y;
	camera->info.width = (uint32_t)width;
	camera->info.height = (uint32_t)height;

	text = arv_camera_get_pixel_format_as_string(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.pixel_format,
			sizeof(camera->info.pixel_format), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	text = arv_camera_get_model_name(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.model,
			sizeof(camera->info.model), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	text = arv_camera_get_device_serial_number(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.serial,
			sizeof(camera->info.serial), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	return 0;
}

int aravis_camera_open(struct aravis_camera **camera_ptr,
		const struct aravis_camera_options *options)
{
	struct aravis_camera *camera;
	GError *error = NULL;
	int res;

	if (camera_ptr == NULL || options == NULL || options->device_id == NULL)
		return -EINVAL;
	*camera_ptr = NULL;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;

	camera->camera = arv_camera_new(options->device_id, &error);
	if (camera->camera == NULL) {
		res = clear_error(&error, -ENODEV);
		goto error;
	}
	camera->stream = arv_camera_create_stream(camera->camera, NULL, NULL,
			NULL, &error);
	if (camera->stream == NULL) {
		res = clear_error(&error, -EIO);
		goto error;
	}
	if (!ARV_IS_GENTL_STREAM(camera->stream)) {
		res = -ENOTSUP;
		goto error;
	}
	if (!arv_gentl_stream_set_caller_polling(
			ARV_GENTL_STREAM(camera->stream), TRUE, &error)) {
		res = clear_error(&error, -EIO);
		goto error;
	}
	if ((res = query_info(camera)) < 0)
		goto error;

	*camera_ptr = camera;
	return 0;

error:
	aravis_camera_close(camera);
	return res;
}

void aravis_camera_close(struct aravis_camera *camera)
{
	if (camera == NULL)
		return;
	(void)aravis_camera_stop(camera);
	g_clear_object(&camera->stream);
	g_clear_object(&camera->camera);
	free(camera);
}

const struct aravis_camera_info *aravis_camera_get_info(
		const struct aravis_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int aravis_camera_announce(struct aravis_camera *camera, void *memory,
		uint64_t size, void *user_data, ArvBuffer **buffer_ptr)
{
	ArvBuffer *buffer;
	GError *error = NULL;

	if (camera == NULL || memory == NULL || size == 0 ||
			buffer_ptr == NULL || *buffer_ptr != NULL || camera->started)
		return -EINVAL;
	buffer = arv_buffer_new_full((size_t)size, memory, user_data, NULL);
	if (buffer == NULL)
		return -ENOMEM;
	if (!arv_gentl_stream_prepare_buffer(ARV_GENTL_STREAM(camera->stream),
			buffer, &error)) {
		g_object_unref(buffer);
		return clear_error(&error, -EIO);
	}
	*buffer_ptr = buffer;
	return 0;
}

int aravis_camera_revoke(struct aravis_camera *camera, ArvBuffer **buffer)
{
	if (camera == NULL || buffer == NULL || camera->started)
		return -EINVAL;
	g_clear_object(buffer);
	return 0;
}

int aravis_camera_start(struct aravis_camera *camera)
{
	GError *error = NULL;

	if (camera == NULL)
		return -EINVAL;
	if (camera->started)
		return 0;
	if (!arv_camera_start_acquisition(camera->camera, &error))
		return clear_error(&error, -EIO);
	camera->started = true;
	return 0;
}

int aravis_camera_stop(struct aravis_camera *camera)
{
	GError *error = NULL;

	if (camera == NULL)
		return -EINVAL;
	if (!camera->started)
		return 0;
	if (!arv_camera_stop_acquisition(camera->camera, &error))
		return clear_error(&error, -EIO);
	camera->started = false;
	return 0;
}

int aravis_camera_queue(struct aravis_camera *camera, ArvBuffer *buffer)
{
	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	return arv_gentl_stream_queue_buffer(ARV_GENTL_STREAM(camera->stream),
			buffer, NULL) ? 0 : -EIO;
}

int aravis_camera_try_get_completion(struct aravis_camera *camera,
		struct aravis_camera_completion *completion)
{
	ArvGenTLStreamPollResult poll;
	ArvBuffer *buffer;
	const void *base, *image;
	size_t size_filled, image_size;
	gint x, y, width, height, x_padding, y_padding;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	memset(completion, 0, sizeof(*completion));
	poll = arv_gentl_stream_poll_buffer(ARV_GENTL_STREAM(camera->stream),
			&buffer, NULL);
	if (poll == ARV_GENTL_STREAM_POLL_EMPTY)
		return 0;
	if (poll != ARV_GENTL_STREAM_POLL_BUFFER)
		return -EIO;

	base = arv_buffer_get_data(buffer, &size_filled);
	image = arv_buffer_get_image_data(buffer, &image_size);
	arv_buffer_get_image_region(buffer, &x, &y, &width, &height);
	arv_buffer_get_image_padding(buffer, &x_padding, &y_padding);
	if (base == NULL || image == NULL || (uintptr_t)image < (uintptr_t)base ||
			x < 0 || y < 0 ||
			width < 0 || height < 0 || x_padding < 0 || y_padding < 0 ||
			(uintptr_t)image - (uintptr_t)base > UINT32_MAX ||
			image_size > UINT32_MAX)
		return -EPROTO;

	completion->buffer = buffer;
	completion->user_data = (void *)arv_buffer_get_user_data(buffer);
	completion->frame = (struct aravis_frame_info) {
		.frame_id = arv_buffer_get_frame_id(buffer),
		.camera_timestamp_ns = arv_buffer_get_timestamp(buffer),
		.size_filled = size_filled,
		.width = (uint32_t)width,
		.height = (uint32_t)height,
		.offset_x = (uint32_t)x,
		.offset_y = (uint32_t)y,
		.x_padding = (uint32_t)x_padding,
		.y_padding = (uint32_t)y_padding,
		.image_offset = (uint32_t)((uintptr_t)image - (uintptr_t)base),
		.image_size = (uint32_t)image_size,
		.incomplete = arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS,
	};
	completion->result = 0;
	return 1;
}
