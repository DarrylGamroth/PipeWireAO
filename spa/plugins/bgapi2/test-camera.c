/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define N_BUFFERS 4u

struct test_slot {
	BGAPI2_Buffer *buffer;
	void *memory;
};

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void release_slots(struct bgapi2_camera *camera,
		struct test_slot slots[N_BUFFERS])
{
	uint32_t i;

	bgapi2_camera_stop(camera);
	bgapi2_camera_discard_buffers(camera);
	for (i = 0; i < N_BUFFERS; i++) {
		if (slots[i].buffer != NULL)
			bgapi2_camera_revoke(camera, &slots[i].buffer);
		free(slots[i].memory);
	}
}

int main(int argc, char *argv[])
{
	struct bgapi2_camera_options options = {
		.interface_index = BGAPI2_CAMERA_ANY_INTERFACE,
		.interface_timeout_ms = 100,
		.device_timeout_ms = 200,
	};
	struct bgapi2_camera *camera = NULL;
	const struct bgapi2_camera_info *info;
	struct test_slot slots[N_BUFFERS] = { 0 };
	struct bgapi2_camera_completion completion;
	const struct bgapi2_frame_info *frame;
	uint64_t deadline;
	uint32_t i;
	int res;

	if (argc != 2) {
		fprintf(stderr, "usage: %s PRODUCER.cti\n", argv[0]);
		return EXIT_FAILURE;
	}
	options.producer_path = argv[1];
	res = bgapi2_camera_open(&camera, &options);
	if (res == -ENODEV)
		return 77;
	if (res < 0) {
		fprintf(stderr, "could not open camera: %d\n", res);
		return EXIT_FAILURE;
	}
	info = bgapi2_camera_get_info(camera);
	if (info == NULL || info->payload_size == 0 || info->width == 0 ||
			info->height == 0 || info->pixel_format[0] == '\0' ||
			info->model[0] == '\0' || info->serial[0] == '\0') {
		fprintf(stderr, "camera information is incomplete\n");
		bgapi2_camera_close(camera);
		return EXIT_FAILURE;
	}
	printf("%s %s: %llux%llu %s, %llu-byte payload\n", info->model,
			info->serial, (unsigned long long)info->width,
			(unsigned long long)info->height, info->pixel_format,
			(unsigned long long)info->payload_size);
	for (i = 0; i < N_BUFFERS; i++) {
		slots[i].memory = malloc(info->payload_size);
		if (slots[i].memory == NULL ||
				bgapi2_camera_announce(camera, slots[i].memory,
				info->payload_size, &slots[i], &slots[i].buffer) < 0 ||
				bgapi2_camera_queue(camera, slots[i].buffer) < 0) {
			fprintf(stderr, "could not prepare external buffer pool\n");
			release_slots(camera, slots);
			bgapi2_camera_close(camera);
			return EXIT_FAILURE;
		}
	}
	if (bgapi2_camera_start(camera) < 0) {
		fprintf(stderr, "could not start acquisition\n");
		release_slots(camera, slots);
		bgapi2_camera_close(camera);
		return EXIT_FAILURE;
	}
	deadline = monotonic_nsec() + 2000000000u;
	do {
		res = bgapi2_camera_try_get_completion(camera, &completion);
		if (res == 0)
			usleep(1000);
	} while (res == 0 && monotonic_nsec() < deadline);
	frame = &completion.frame;
	if (res != 1 || completion.result < 0 || completion.buffer == NULL ||
			completion.user_data == NULL || frame->size_filled == 0 ||
			frame->width != info->width || frame->height != info->height ||
			frame->incomplete) {
		fprintf(stderr, "camera did not deliver a valid external buffer\n");
		release_slots(camera, slots);
		bgapi2_camera_close(camera);
		return EXIT_FAILURE;
	}
	printf("frame %llu: %llu bytes in slot %td\n",
			(unsigned long long)frame->frame_id,
			(unsigned long long)frame->size_filled,
			(struct test_slot *)completion.user_data - slots);
	release_slots(camera, slots);
	bgapi2_camera_close(camera);
	return EXIT_SUCCESS;
}
