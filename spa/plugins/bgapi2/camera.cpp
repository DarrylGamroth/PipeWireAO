/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

template <typename Operation>
static BGAPI2_RESULT guarded_call(Operation operation) noexcept
{
	try {
		return operation();
	} catch (...) {
		return BGAPI2_RESULT_LOWLEVEL_ERROR;
	}
}

#define BGAPI2_CALL(expression) guarded_call([&]() { return (expression); })

struct bgapi2_camera {
	BGAPI2_System *system;
	BGAPI2_Interface *interface;
	BGAPI2_Device *device;
	BGAPI2_DataStream *stream;
	BGAPI2_Node *acquisition_start;
	BGAPI2_Node *acquisition_stop;
	BGAPI2_Node *acquisition_abort;
	struct bgapi2_camera_info info;
	bool system_open;
	bool interface_open;
	bool device_open;
	bool stream_open;
	bool acquiring;
};

static int fail(BGAPI2_RESULT result)
{
	switch (result) {
	case BGAPI2_RESULT_ACCESS_DENIED:
		return -EACCES;
	case BGAPI2_RESULT_RESOURCE_IN_USE:
		return -EBUSY;
	case BGAPI2_RESULT_INVALID_PARAMETER:
	case BGAPI2_RESULT_INVALID_HANDLE:
	case BGAPI2_RESULT_INVALID_BUFFER:
		return -EINVAL;
	case BGAPI2_RESULT_NOT_IMPLEMENTED:
		return -ENOTSUP;
	case BGAPI2_RESULT_NOT_AVAILABLE:
	case BGAPI2_RESULT_NO_DATA:
		return -ENODATA;
	case BGAPI2_RESULT_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

static int checked_result(struct bgapi2_camera *, BGAPI2_RESULT result)
{
	if (result != BGAPI2_RESULT_SUCCESS)
		return fail(result);
	return 0;
}

#define checked(camera, expression) \
	checked_result((camera), BGAPI2_CALL(expression))

static int get_node_int(struct bgapi2_camera *camera, const char *name,
		uint64_t *value)
{
	BGAPI2_Node *node = NULL;
	bo_int64 result = 0;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetRemoteNode(
			camera->device, name, &node))) < 0 ||
			(res = checked(camera, BGAPI2_Node_GetInt(node, &result))) < 0)
		return res;
	if (result < 0)
		return -ERANGE;
	*value = (uint64_t)result;
	return 0;
}

static int get_node_string(struct bgapi2_camera *camera, const char *name,
		char *value, size_t capacity)
{
	BGAPI2_Node *node = NULL;
	bo_uint64 length = capacity;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetRemoteNode(
			camera->device, name, &node))) < 0)
		return res;
	return checked(camera, BGAPI2_Node_GetString(node, value, &length));
}

static void close_interface(struct bgapi2_camera *camera)
{
	if (camera->interface_open)
		BGAPI2_CALL(BGAPI2_Interface_Close(camera->interface));
	camera->interface = NULL;
	camera->interface_open = false;
}

static int find_device(struct bgapi2_camera *camera,
		const struct bgapi2_camera_options *options)
{
	bo_uint n_interfaces = 0;
	bo_bool changed = 0;
	uint32_t first, end, index;
	int res;

	if ((res = checked(camera, BGAPI2_System_UpdateInterfaceList(
			camera->system, &changed, options->interface_timeout_ms))) < 0 ||
			(res = checked(camera, BGAPI2_System_GetNumInterfaces(
			camera->system, &n_interfaces))) < 0)
		return res;
	if (options->interface_index == BGAPI2_CAMERA_ANY_INTERFACE) {
		first = 0;
		end = n_interfaces;
	} else {
		if (options->interface_index >= n_interfaces)
			return -ENODEV;
		first = options->interface_index;
		end = first + 1;
	}
	for (index = first; index < end; index++) {
		bo_uint n_devices = 0;

		if (checked(camera, BGAPI2_System_GetInterface(camera->system,
				index, &camera->interface)) < 0 ||
				checked(camera, BGAPI2_Interface_Open(camera->interface)) < 0) {
			camera->interface = NULL;
			continue;
		}
		camera->interface_open = true;
		if (checked(camera, BGAPI2_Interface_UpdateDeviceList(
				camera->interface, &changed, options->device_timeout_ms)) == 0 &&
				checked(camera, BGAPI2_Interface_GetNumDevices(
				camera->interface, &n_devices)) == 0 &&
				options->device_index < n_devices &&
				checked(camera, BGAPI2_Interface_GetDevice(camera->interface,
				options->device_index, &camera->device)) == 0) {
			camera->info.interface_index = index;
			camera->info.device_index = options->device_index;
			return 0;
		}
		close_interface(camera);
	}
	return -ENODEV;
}

static int query_info(struct bgapi2_camera *camera)
{
	bo_uint64 length;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetPayloadSize(
			camera->device, &camera->info.payload_size))) < 0 ||
			(res = get_node_int(camera, "Width", &camera->info.width)) < 0 ||
			(res = get_node_int(camera, "Height", &camera->info.height)) < 0 ||
			(res = get_node_int(camera, "OffsetX", &camera->info.offset_x)) < 0 ||
			(res = get_node_int(camera, "OffsetY", &camera->info.offset_y)) < 0 ||
			(res = get_node_string(camera, "PixelFormat", camera->info.pixel_format,
				sizeof(camera->info.pixel_format))) < 0)
		return res;
	length = sizeof(camera->info.model);
	if ((res = checked(camera, BGAPI2_Device_GetModel(camera->device,
			camera->info.model, &length))) < 0)
		return res;
	length = sizeof(camera->info.serial);
	return checked(camera, BGAPI2_Device_GetSerialNumber(camera->device,
			camera->info.serial, &length));
}

int bgapi2_camera_open(struct bgapi2_camera **camera_ptr,
		const struct bgapi2_camera_options *options)
{
	struct bgapi2_camera *camera;
	int res;

	if (camera_ptr == NULL || options == NULL || options->producer_path == NULL)
		return -EINVAL;
	*camera_ptr = NULL;
	camera = static_cast<struct bgapi2_camera *>(calloc(1, sizeof(*camera)));
	if (camera == NULL)
		return -ENOMEM;
	if ((res = checked(camera, BGAPI2_LoadSystemFromPath(
			options->producer_path, &camera->system))) < 0 ||
			(res = checked(camera, BGAPI2_System_Open(camera->system))) < 0)
		goto error;
	camera->system_open = true;
	if ((res = find_device(camera, options)) < 0 ||
			(res = checked(camera, BGAPI2_Device_Open(camera->device))) < 0)
		goto error;
	camera->device_open = true;
	if ((res = checked(camera, BGAPI2_Device_GetDataStream(camera->device,
			options->stream_index, &camera->stream))) < 0 ||
			(res = checked(camera, BGAPI2_DataStream_Open(camera->stream))) < 0)
		goto error;
	camera->stream_open = true;
	camera->info.stream_index = options->stream_index;
	if ((res = checked(camera, BGAPI2_DataStream_SetNewBufferEventMode(
			camera->stream, EVENTMODE_POLLING))) < 0 ||
			(res = query_info(camera)) < 0 ||
			(res = checked(camera, BGAPI2_Device_GetRemoteNode(camera->device,
			"AcquisitionStart", &camera->acquisition_start))) < 0 ||
			(res = checked(camera, BGAPI2_Device_GetRemoteNode(camera->device,
			"AcquisitionStop", &camera->acquisition_stop))) < 0)
		goto error;
	if (BGAPI2_CALL(BGAPI2_Device_GetRemoteNode(camera->device, "AcquisitionAbort",
			&camera->acquisition_abort)) != BGAPI2_RESULT_SUCCESS)
		camera->acquisition_abort = NULL;
	*camera_ptr = camera;
	return 0;

error:
	bgapi2_camera_close(camera);
	return res;
}

void bgapi2_camera_close(struct bgapi2_camera *camera)
{
	if (camera == NULL)
		return;
	if (camera->acquiring)
		bgapi2_camera_stop(camera);
	if (camera->stream_open)
		BGAPI2_CALL(BGAPI2_DataStream_Close(camera->stream));
	if (camera->device_open)
		BGAPI2_CALL(BGAPI2_Device_Close(camera->device));
	close_interface(camera);
	if (camera->system_open)
		BGAPI2_CALL(BGAPI2_System_Close(camera->system));
	if (camera->system != NULL)
		BGAPI2_CALL(BGAPI2_ReleaseSystem(camera->system));
	free(camera);
}

const struct bgapi2_camera_info *bgapi2_camera_get_info(
		const struct bgapi2_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int bgapi2_camera_announce(struct bgapi2_camera *camera, void *memory,
		uint64_t size, void *user_data, BGAPI2_Buffer **buffer)
{
	int res;

	if (camera == NULL || memory == NULL || buffer == NULL ||
			size < camera->info.payload_size)
		return -EINVAL;
	*buffer = NULL;
	if ((res = checked(camera, BGAPI2_CreateBufferWithExternalMemory(buffer,
			memory, size, user_data))) < 0)
		return res;
	if ((res = checked(camera, BGAPI2_DataStream_AnnounceBuffer(
			camera->stream, *buffer))) < 0) {
		BGAPI2_CALL(BGAPI2_DeleteBuffer(*buffer, NULL));
		*buffer = NULL;
		return res;
	}
	return 0;
}

int bgapi2_camera_revoke(struct bgapi2_camera *camera, BGAPI2_Buffer **buffer)
{
	BGAPI2_Buffer *value;
	int res;

	if (camera == NULL || buffer == NULL || *buffer == NULL)
		return -EINVAL;
	value = *buffer;
	if ((res = checked(camera, BGAPI2_DataStream_RevokeBuffer(camera->stream,
			value, NULL))) < 0)
		return res;
	if ((res = checked(camera, BGAPI2_DeleteBuffer(value, NULL))) < 0)
		return res;
	*buffer = NULL;
	return 0;
}

int bgapi2_camera_discard_buffers(struct bgapi2_camera *camera)
{
	if (camera == NULL)
		return -EINVAL;
	return checked(camera, BGAPI2_DataStream_DiscardAllBuffers(camera->stream));
}

int bgapi2_camera_start(struct bgapi2_camera *camera)
{
	int res;

	if (camera == NULL)
		return -EINVAL;
	if (camera->acquiring)
		return 0;
	if ((res = checked(camera, BGAPI2_DataStream_StartAcquisitionContinuous(
			camera->stream))) < 0)
		return res;
	if ((res = checked(camera, BGAPI2_Node_Execute(
			camera->acquisition_start))) < 0) {
		BGAPI2_CALL(BGAPI2_DataStream_StopAcquisition(camera->stream));
		return res;
	}
	camera->acquiring = true;
	return 0;
}

int bgapi2_camera_stop(struct bgapi2_camera *camera)
{
	int first_error = 0, res;

	if (camera == NULL)
		return -EINVAL;
	if (!camera->acquiring)
		return 0;
	if (camera->acquisition_abort != NULL)
		BGAPI2_CALL(BGAPI2_Node_Execute(camera->acquisition_abort));
	if ((res = checked(camera, BGAPI2_Node_Execute(
			camera->acquisition_stop))) < 0)
		first_error = res;
	if ((res = checked(camera, BGAPI2_DataStream_StopAcquisition(
			camera->stream))) < 0 && first_error == 0)
		first_error = res;
	camera->acquiring = false;
	return first_error;
}

int bgapi2_camera_queue(struct bgapi2_camera *camera, BGAPI2_Buffer *buffer)
{
	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	return checked(camera, BGAPI2_DataStream_QueueBuffer(camera->stream, buffer));
}

int bgapi2_camera_try_get_filled(struct bgapi2_camera *camera,
		BGAPI2_Buffer **buffer)
{
	BGAPI2_RESULT result;

	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	result = BGAPI2_CALL(BGAPI2_DataStream_GetFilledBuffer(
			camera->stream, buffer, 0));
	if (result == BGAPI2_RESULT_TIMEOUT)
		return 0;
	if (result != BGAPI2_RESULT_SUCCESS)
		return fail(result);
	return *buffer == NULL ? -EIO : 1;
}

#define GET_FRAME_FIELD(function, field) do { \
	if ((res = checked(camera, function(buffer, &info->field))) < 0) \
		return res; \
} while (0)

int bgapi2_camera_get_frame_info(struct bgapi2_camera *camera,
		BGAPI2_Buffer *buffer, struct bgapi2_frame_info *info)
{
	bo_bool incomplete = 0;
	int res;

	if (camera == NULL || buffer == NULL || info == NULL)
		return -EINVAL;
	memset(info, 0, sizeof(*info));
	info->width = camera->info.width;
	info->height = camera->info.height;
	info->offset_x = camera->info.offset_x;
	info->offset_y = camera->info.offset_y;
	info->image_present = true;
	GET_FRAME_FIELD(BGAPI2_Buffer_GetFrameID, frame_id);
	GET_FRAME_FIELD(BGAPI2_Buffer_GetSizeFilled, size_filled);
	if ((res = checked(camera, BGAPI2_Buffer_GetIsIncomplete(buffer,
			&incomplete))) < 0)
		return res;
	info->incomplete = incomplete != 0;
	info->image_length = info->size_filled;

	/* These GenTL buffer fields are optional. Keep fixed negotiated layout and
	 * zero/unknown timestamps when a producer does not implement them. */
	BGAPI2_CALL(BGAPI2_Buffer_GetTimestamp(buffer, &info->timestamp));
	BGAPI2_CALL(BGAPI2_Buffer_GetHostTimestamp(buffer, &info->host_timestamp));
	BGAPI2_CALL(BGAPI2_Buffer_GetXPadding(buffer, &info->x_padding));
	BGAPI2_CALL(BGAPI2_Buffer_GetImageOffset(buffer, &info->image_offset));
	BGAPI2_CALL(BGAPI2_Buffer_GetImageLength(buffer, &info->image_length));
	return 0;
}

#undef GET_FRAME_FIELD

int bgapi2_camera_get_user_data(struct bgapi2_camera *camera,
		BGAPI2_Buffer *buffer, void **user_data)
{
	if (camera == NULL || buffer == NULL || user_data == NULL)
		return -EINVAL;
	return checked(camera, BGAPI2_Buffer_GetUserPtr(buffer, user_data));
}
