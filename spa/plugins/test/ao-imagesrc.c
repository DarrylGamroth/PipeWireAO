/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/buffer/image-source-latest.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define WIDTH 64u
#define HEIGHT 64u
#define STRIDE (WIDTH * 2u)
#define FRAME_SIZE (STRIDE * HEIGHT)
#define MIN_BUFFERS 2u
#define MAX_BUFFERS 64u

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.test.ao-imagesrc");

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	struct spa_video_info_raw format;
	bool have_format;
	uint32_t n_buffers;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_log *log;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct port port;
	struct spa_buffer_latest *latest;
	struct spa_image_source_latest transport;
	struct spa_image_source source;
	bool started;
	uint64_t sequence;
	uint8_t acquisition_domain[SPA_META_ACQUISITION_DOMAIN_SIZE];
};

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask != 0) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, bool full)
{
	struct port *port = &this->port;
	uint64_t old = full ? port->info.change_mask : 0;

	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask != 0) {
		spa_node_emit_port_info(&this->hooks, SPA_DIRECTION_OUTPUT, 0,
				&port->info);
		port->info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	emit_node_info(this, true);
	emit_port_info(this, true);
	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_node_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int impl_node_enum_params(void *object SPA_UNUSED, int seq SPA_UNUSED,
		uint32_t id SPA_UNUSED, uint32_t start SPA_UNUSED,
		uint32_t num SPA_UNUSED, const struct spa_pod *filter SPA_UNUSED)
{
	return -ENOENT;
}

static int impl_node_set_param(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		uint32_t flags SPA_UNUSED, const struct spa_pod *param SPA_UNUSED)
{
	return -ENOENT;
}

static int impl_node_set_io(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		void *data SPA_UNUSED, size_t size SPA_UNUSED)
{
	return -ENOTSUP;
}

static int impl_node_send_command(void *object,
		const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->port.have_format || this->port.n_buffers == 0)
			return -EIO;
		if (this->started)
			return 0;
		if ((res = spa_buffer_latest_worker_begin(this->latest)) < 0)
			return res;
		this->started = true;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		if (!this->started)
			return 0;
		this->started = false;
		return spa_buffer_latest_worker_end(this->latest);
	default:
		return -ENOTSUP;
	}
}

static int impl_node_add_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED,
		const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int build_port_param(struct impl *this, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	struct port *port = &this->port;

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_GRAY16_LE),
				SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(WIDTH, HEIGHT)),
				SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(1000, 1)));
		return 1;
	case SPA_PARAM_Format:
		if (index > 0)
			return 0;
		if (!port->have_format)
			return -EIO;
		*param = spa_format_video_raw_build(builder, id, &port->format);
		return 1;
	case SPA_PARAM_Buffers:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size, SPA_POD_Int(FRAME_SIZE),
				SPA_PARAM_BUFFERS_stride, SPA_POD_Int(STRIDE),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return 1;
	case SPA_PARAM_Meta:
		switch (index) {
		case 0:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
			return 1;
		case 1:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
			return 1;
		default:
			return 0;
		}
	case SPA_PARAM_IO:
		switch (index) {
		case 0:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatest),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest)));
			return 1;
		case 1:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatestLink),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
			return 1;
		default:
			return 0;
		}
	default:
		return -ENOENT;
	}
}

static int impl_node_port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod_builder builder;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_init(&builder, buffer, sizeof(buffer));
		res = build_port_param(this, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&this->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int impl_node_port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags SPA_UNUSED,
		const struct spa_pod *param)
{
	struct impl *this = object;
	struct spa_video_info_raw format;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (this->started || this->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		this->port.have_format = false;
		return 0;
	}
	if (spa_format_video_raw_parse(param, &format) < 0 ||
			format.format != SPA_VIDEO_FORMAT_GRAY16_LE ||
			format.size.width != WIDTH || format.size.height != HEIGHT ||
			format.framerate.num != 1000 || format.framerate.denom != 1)
		return -EINVAL;
	this->port.format = format;
	this->port.have_format = true;
	return 0;
}

static int impl_node_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t flags SPA_UNUSED,
		uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	uint32_t i;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (this->started)
		return -EBUSY;
	if (n_buffers == 0) {
		if (this->port.n_buffers == 0)
			return 0;
		res = spa_image_source_latest_teardown(&this->transport, &this->source);
		if (res == 0)
			this->port.n_buffers = 0;
		return res;
	}
	if (!this->port.have_format || this->port.n_buffers != 0 ||
			buffers == NULL || n_buffers > MAX_BUFFERS)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				data->maxsize < FRAME_SIZE || data->chunk == NULL)
			return -EINVAL;
	}
	res = spa_image_source_latest_prepare(&this->transport, &this->source,
			buffers, n_buffers);
	if (res >= 0)
		this->port.n_buffers = n_buffers;
	return res < 0 ? res : 0;
}

static int impl_node_port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	return spa_buffer_latest_set_io(this->latest, id, data, size);
}

static int impl_node_port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int fill_and_publish(struct impl *this,
		struct spa_image_source_buffer *image)
{
	struct spa_buffer *buffer = spa_image_source_buffer_get_buffer(image);
	struct spa_data *data = &buffer->datas[0];
	struct spa_meta_acquisition acquisition;
	struct spa_image_frame frame;
	uint8_t *pixels = data->data;
	uint64_t sequence = ++this->sequence;
	uint32_t i;

	for (i = 0; i < WIDTH * HEIGHT; i++) {
		uint16_t value = (uint16_t)(sequence + i);
		pixels[2u * i] = (uint8_t)value;
		pixels[2u * i + 1u] = (uint8_t)(value >> 8);
	}
	spa_meta_acquisition_init(&acquisition);
	spa_meta_acquisition_set_identity(&acquisition,
			this->acquisition_domain, 1, sequence);
	spa_meta_acquisition_set_exposure_start(&acquisition,
			(int64_t)(sequence * SPA_NSEC_PER_MSEC), 0);
	frame = (struct spa_image_frame) {
		.version = SPA_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.size = FRAME_SIZE,
		.stride = STRIDE,
		.sequence = sequence,
		.pts = (int64_t)(sequence * SPA_NSEC_PER_MSEC),
		.acquisition = &acquisition,
	};
	return spa_image_source_publish_complete(&this->source, image, &frame);
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct spa_image_source_buffer *image;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	if (!this->started)
		return SPA_STATUS_OK;
	res = spa_image_source_try_acquire(&this->source, &image);
	if (res <= 0)
		return res < 0 ? res : SPA_STATUS_OK;
	return fill_and_publish(this, image);
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &this->node;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *)handle;
	int res = 0;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	if (this->started) {
		this->started = false;
		res = spa_buffer_latest_worker_end(this->latest);
	}
	if (res == 0 && this->port.n_buffers != 0) {
		res = spa_image_source_latest_teardown(&this->transport, &this->source);
		if (res == 0)
			this->port.n_buffers = 0;
	}
	spa_buffer_latest_destroy(this->latest);
	this->latest = NULL;
	return res;
}

static size_t impl_get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info SPA_UNUSED,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *this = (struct impl *)handle;
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = MIN_BUFFERS,
		.max_buffers = MAX_BUFFERS,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION,
	};
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_hook_list_init(&this->hooks);
	this->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &impl_node, this);
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_output_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_RTC_PROCESS;
	this->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	this->port.info = SPA_PORT_INFO_INIT();
	this->port.info.flags = SPA_PORT_FLAG_LIVE;
	this->port.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	this->port.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	this->port.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	this->port.params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta,
			SPA_PARAM_INFO_READ);
	this->port.params[4] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	this->port.info.params = this->port.params;
	this->port.info.n_params = SPA_N_ELEMENTS(this->port.params);
	this->acquisition_domain[0] = 0xa0;
	this->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, this, this->log);
	if (this->latest == NULL)
		return -errno;
	res = spa_image_source_latest_init(&this->transport, &this->source,
			this->latest, &config);
	if (res < 0) {
		spa_buffer_latest_destroy(this->latest);
		this->latest = NULL;
	}
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int impl_enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;
	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_ao_imagesrc_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"test.ao-imagesrc",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
