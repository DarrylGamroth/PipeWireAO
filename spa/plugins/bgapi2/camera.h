/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_CAMERA_H
#define SPA_BGAPI2_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bgapi2_genicam/bgapi2_genicam.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BGAPI2_CAMERA_ANY_INTERFACE UINT32_MAX

struct bgapi2_camera_options {
	const char *producer_path;
	uint32_t interface_index;
	uint32_t device_index;
	uint32_t stream_index;
	uint64_t interface_timeout_ms;
	uint64_t device_timeout_ms;
};

struct bgapi2_camera_info {
	uint32_t interface_index;
	uint32_t device_index;
	uint32_t stream_index;
	uint64_t payload_size;
	uint64_t width;
	uint64_t height;
	uint64_t offset_x;
	uint64_t offset_y;
	char pixel_format[64];
	char model[128];
	char serial[128];
};

struct bgapi2_frame_info {
	uint64_t frame_id;
	uint64_t size_filled;
	uint64_t width;
	uint64_t height;
	uint64_t offset_x;
	uint64_t offset_y;
	uint64_t x_padding;
	uint64_t image_offset;
	uint64_t image_length;
	bool incomplete;
};

struct bgapi2_camera_completion {
	BGAPI2_Buffer *buffer;
	void *user_data;
	struct bgapi2_frame_info frame;
	int result;
};

struct bgapi2_camera;

/* Discovery, feature lookup, and teardown are control-path operations. */
int bgapi2_camera_open(struct bgapi2_camera **camera,
		const struct bgapi2_camera_options *options);
void bgapi2_camera_close(struct bgapi2_camera *camera);
const struct bgapi2_camera_info *bgapi2_camera_get_info(
		const struct bgapi2_camera *camera);

/* Buffer creation and revocation are pool-lifecycle operations. */
int bgapi2_camera_announce(struct bgapi2_camera *camera, void *memory,
		uint64_t size, void *user_data, BGAPI2_Buffer **buffer);
int bgapi2_camera_revoke(struct bgapi2_camera *camera,
		BGAPI2_Buffer **buffer);
int bgapi2_camera_discard_buffers(struct bgapi2_camera *camera);

int bgapi2_camera_start(struct bgapi2_camera *camera);
int bgapi2_camera_stop(struct bgapi2_camera *camera);

/*
 * RTC owner operations. Completion polling only consumes the local SPSC and
 * does not call BGAPI2. Queueing returns ownership to the GenTL producer, so
 * the producer's queue behavior remains part of the measured RTC contract.
 */
int bgapi2_camera_queue(struct bgapi2_camera *camera,
		BGAPI2_Buffer *buffer);
int bgapi2_camera_try_get_completion(struct bgapi2_camera *camera,
		struct bgapi2_camera_completion *completion);

#ifdef __cplusplus
}
#endif

#endif
