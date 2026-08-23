/* SPDX-License-Identifier: MIT */
#ifndef SPA_ARAVIS_CAMERA_H
#define SPA_ARAVIS_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arv.h>

struct aravis_camera_options {
	const char *device_id;
};

struct aravis_camera_info {
	uint64_t payload_size;
	uint32_t width;
	uint32_t height;
	uint32_t offset_x;
	uint32_t offset_y;
	char pixel_format[64];
	char model[128];
	char serial[128];
};

struct aravis_frame_info {
	uint64_t frame_id;
	uint64_t camera_timestamp_ns;
	uint64_t size_filled;
	uint32_t width;
	uint32_t height;
	uint32_t offset_x;
	uint32_t offset_y;
	uint32_t x_padding;
	uint32_t y_padding;
	uint32_t image_offset;
	uint32_t image_size;
	bool incomplete;
};

struct aravis_camera_completion {
	ArvBuffer *buffer;
	void *user_data;
	struct aravis_frame_info frame;
	int result;
};

struct aravis_camera;

/* Discovery, GenICam access, and teardown are stopped control-path work. */
int aravis_camera_open(struct aravis_camera **camera,
		const struct aravis_camera_options *options);
void aravis_camera_close(struct aravis_camera *camera);
const struct aravis_camera_info *aravis_camera_get_info(
		const struct aravis_camera *camera);

/* Buffer announcement and revocation are pool-lifecycle operations. */
int aravis_camera_announce(struct aravis_camera *camera, void *memory,
		uint64_t size, void *user_data, ArvBuffer **buffer);
int aravis_camera_revoke(struct aravis_camera *camera, ArvBuffer **buffer);

int aravis_camera_start(struct aravis_camera *camera);
int aravis_camera_stop(struct aravis_camera *camera);

/* RTC-owner operations: one direct GenTL call is made by each poll or queue. */
int aravis_camera_queue(struct aravis_camera *camera, ArvBuffer *buffer);
int aravis_camera_try_get_completion(struct aravis_camera *camera,
		struct aravis_camera_completion *completion);

#endif
