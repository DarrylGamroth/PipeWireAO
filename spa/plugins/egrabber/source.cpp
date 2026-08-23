/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <spa/buffer/image-source-latest.h>
#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "camera.hpp"
#include "egrabber.hpp"
#include "frame_layout.hpp"
#include "frame_sequence.hpp"
#include "options.hpp"

namespace {

namespace gc = Euresys::gc;
using Euresys::Buffer;
using Euresys::BufferIndexRange;
using Euresys::NewBufferData;
using egrabber_pipewire::BufferMetadata;
using egrabber_pipewire::Camera;
using egrabber_pipewire::DeliveredFrameLayout;
using egrabber_pipewire::FrameSequence;
using egrabber_pipewire::NegotiatedFrameLayout;
using egrabber_pipewire::Options;
using egrabber_pipewire::PixelByteOrder;

constexpr uint32_t max_buffers = SPA_IMAGE_SOURCE_MAX_BUFFERS;

struct buffer_slot {
	struct spa_image_source_buffer *image = nullptr;
	BufferIndexRange range;
	std::optional<Buffer> completed;
};

struct port {
	uint64_t info_all = 0;
	struct spa_port_info info = SPA_PORT_INFO_INIT();
	struct spa_param_info params[5] = {};
	struct spa_video_info_raw format = {};
	bool have_format = false;
	uint32_t n_buffers = 0;
};

struct impl {
	struct spa_handle handle = {};
	struct spa_node node = {};
	struct spa_log *log = nullptr;
	struct spa_hook_list hooks = {};
	struct spa_callbacks callbacks = {};
	uint64_t info_all = 0;
	struct spa_node_info info = SPA_NODE_INFO_INIT();
	struct spa_dict node_props = {};
	struct spa_dict_item node_items[16] = {};
	Options options;
	std::string node_name;
	std::string description;
	std::string interface_index;
	std::string device_index;
	std::string stream_index;
	port output;
	std::unique_ptr<Camera> camera;
	struct spa_buffer_latest *latest = nullptr;
	struct spa_image_source_latest transport = {};
	struct spa_image_source source = {};
	buffer_slot slots[max_buffers];
	std::vector<BufferIndexRange> ranges;
	FrameSequence frame_sequence;
	spa_fraction frame_rate = SPA_FRACTION(0, 1);
	uint32_t video_format = SPA_VIDEO_FORMAT_UNKNOWN;
	uint32_t queued_buffers = 0;
	bool started = false;
};

uint32_t video_format(const Camera &camera)
{
	const std::string &format = camera.pixel_format();

	if (format == "Mono8")
		return SPA_VIDEO_FORMAT_GRAY8;
	if (format == "Mono10" || format == "Mono12" ||
			format == "Mono14" || format == "Mono16") {
		const auto byte_order = camera.pixel_byte_order();
		if (!byte_order)
			throw std::runtime_error("GenTL did not report the pixel byte order");
		return *byte_order == PixelByteOrder::little
			? SPA_VIDEO_FORMAT_GRAY16_LE : SPA_VIDEO_FORMAT_GRAY16_BE;
	}
	throw std::runtime_error("unsupported zero-copy eGrabber pixel format: " + format);
}

spa_fraction frame_rate(double value)
{
	constexpr uint32_t denominator = 1000;
	if (value > static_cast<double>(
			std::numeric_limits<uint32_t>::max() / denominator))
		throw std::runtime_error("eGrabber frame rate exceeds the SPA fraction range");
	const auto numerator = static_cast<uint32_t>(std::llround(value * denominator));
	const auto divisor = std::gcd(numerator, denominator);

	return SPA_FRACTION(numerator / divisor, denominator / divisor);
}

void emit_node_info(impl *self, bool full)
{
	uint64_t old = full ? self->info.change_mask : 0;

	if (full)
		self->info.change_mask = self->info_all;
	if (self->info.change_mask != 0) {
		spa_node_emit_info(&self->hooks, &self->info);
		self->info.change_mask = old;
	}
}

void emit_port_info(impl *self, bool full)
{
	port &output = self->output;
	uint64_t old = full ? output.info.change_mask : 0;

	if (full)
		output.info.change_mask = output.info_all;
	if (output.info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_OUTPUT, 0,
				&output.info);
		output.info.change_mask = old;
	}
}

int add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	auto *self = static_cast<impl *>(object);
	struct spa_hook_list save;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_node_info(self, true);
	emit_port_info(self, true);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

int set_callbacks(void *object, const struct spa_node_callbacks *callbacks,
		void *data)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

int enum_params(void *, int, uint32_t, uint32_t, uint32_t,
		const struct spa_pod *)
{
	return -ENOENT;
}

int set_param(void *, uint32_t, uint32_t, const struct spa_pod *)
{
	return -ENOENT;
}

int set_io(void *, uint32_t, void *, size_t)
{
	return -ENOTSUP;
}

int add_port(void *, enum spa_direction, uint32_t, const struct spa_dict *)
{
	return -ENOTSUP;
}

int remove_port(void *, enum spa_direction, uint32_t)
{
	return -ENOTSUP;
}

int build_port_param(impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	const Camera &camera = *self->camera;
	port &output = self->output;
	const struct spa_rectangle size = SPA_RECTANGLE(
			static_cast<uint32_t>(camera.width()),
			static_cast<uint32_t>(camera.height()));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(self->video_format),
				SPA_FORMAT_VIDEO_size,
				SPA_POD_Rectangle(&size),
				SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&self->frame_rate));
		return 1;
	case SPA_PARAM_Format:
		if (index > 0)
			return 0;
		if (!output.have_format)
			return -EIO;
		*param = spa_format_video_raw_build(builder, id, &output.format);
		return 1;
	case SPA_PARAM_Buffers:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(
						static_cast<int32_t>(camera.buffer_count()),
						static_cast<int32_t>(camera.announce_minimum()),
						static_cast<int32_t>(max_buffers)),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int(static_cast<int32_t>(camera.payload_size())),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int(static_cast<int32_t>(camera.natural_line_pitch())),
				SPA_PARAM_BUFFERS_align,
				SPA_POD_Int(static_cast<int32_t>(camera.buffer_alignment())),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return 1;
	case SPA_PARAM_Meta:
		if (index == 0) {
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
			return 1;
		}
		if (index == 1) {
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
			return 1;
		}
		return 0;
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatestLink),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
		return 1;
	default:
		return -ENOENT;
	}
}

int port_enum_params(void *object, int seq, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	auto *self = static_cast<impl *>(object);
	struct spa_pod_builder builder;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_init(&builder, buffer, sizeof(buffer));
		res = build_port_param(self, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t,
		const struct spa_pod *param)
{
	auto *self = static_cast<impl *>(object);
	struct spa_video_info_raw format;
	const Camera &camera = *self->camera;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (self->started || self->output.n_buffers != 0)
		return -EBUSY;
	if (param == nullptr) {
		self->output.have_format = false;
		return 0;
	}
	if (spa_format_video_raw_parse(param, &format) < 0 ||
			format.format != self->video_format ||
			format.size.width != camera.width() ||
			format.size.height != camera.height())
		return -EINVAL;
	self->output.format = format;
	self->output.have_format = true;
	return 0;
}

int release_buffers(impl *self)
{
	int res = 0;

	try {
		self->camera->clear_frame_callback();
		self->camera->disable_events();
		for (auto &slot : self->slots)
			slot.completed.reset();
		if (!self->ranges.empty())
			self->camera->release(self->ranges);
	} catch (...) {
		res = -EIO;
	}
	self->ranges.clear();
	self->queued_buffers = 0;
	for (auto &slot : self->slots) {
		if (slot.image != nullptr)
			spa_image_source_buffer_set_user_data(slot.image, nullptr);
		slot.image = nullptr;
	}
	if (self->output.n_buffers != 0) {
		int teardown = spa_image_source_latest_teardown(
				&self->transport, &self->source);
		if (res == 0)
			res = teardown;
		if (teardown == 0)
			self->output.n_buffers = 0;
	}
	return res;
}

int port_use_buffers(void *object, enum spa_direction direction, uint32_t,
		uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	auto *self = static_cast<impl *>(object);
	uint32_t i;
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return release_buffers(self);
	if (!self->output.have_format || self->output.n_buffers != 0 ||
			buffers == nullptr || n_buffers > max_buffers ||
			n_buffers < self->camera->announce_minimum())
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == nullptr || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == nullptr ||
				data->chunk == nullptr ||
				data->maxsize < self->camera->payload_size() ||
				reinterpret_cast<uintptr_t>(data->data) %
						self->camera->buffer_alignment() != 0)
			return -EINVAL;
	}
	res = spa_image_source_latest_prepare(&self->transport, &self->source,
			buffers, n_buffers);
	if (res < 0)
		return res;
	self->output.n_buffers = n_buffers;
	try {
		self->ranges.reserve(n_buffers);
		self->camera->select_memory_type(false);
		for (i = 0; i < n_buffers; i++) {
			struct spa_image_source_buffer *image = nullptr;
			struct spa_data *data = &buffers[i]->datas[0];

			if (spa_image_source_try_acquire(&self->source, &image) != 1 ||
					image == nullptr)
				throw std::runtime_error("PipeWireAO image pool is incomplete");
			buffer_slot &slot = self->slots[i];
			slot.image = image;
			spa_image_source_buffer_set_user_data(image, &slot);
			slot.range = self->camera->announce(data->data, data->fd,
					self->camera->payload_size(), data->mapoffset, false, &slot);
			self->ranges.push_back(slot.range);
			self->queued_buffers++;
		}
		self->camera->set_frame_callback([self](const NewBufferData &data) {
			auto *slot = static_cast<buffer_slot *>(data.userPointer);
			if (slot == nullptr || slot->image == nullptr)
				throw std::runtime_error("eGrabber completion has no image slot");
			if (self->queued_buffers == 0)
				throw std::runtime_error("eGrabber completed an unqueued image slot");
			self->queued_buffers--;

			Buffer completed(data);
			const auto info = self->camera->buffer_info(completed);
			const BufferMetadata metadata = self->camera->buffer_metadata(completed);
			const bool supported_payload = !metadata.payload_type ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_UNKNOWN ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_IMAGE ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_CHUNK_DATA;
			const auto layout = egrabber_pipewire::resolve_frame_layout(
					NegotiatedFrameLayout{
						self->camera->width(), self->camera->height(),
						self->camera->offset_x(), self->camera->offset_y(),
						self->camera->natural_line_pitch(),
						self->camera->payload_size(), self->camera->pixel_format(),
					},
					DeliveredFrameLayout{
						info.width, info.deliveredHeight, info.linePitch,
						info.pixelFormat,
						self->camera->buffer_offset_x(completed),
						self->camera->buffer_offset_y(completed),
						metadata.image_offset, metadata.size_filled,
						metadata.data_size, metadata.x_padding,
						metadata.image_present, metadata.data_larger_than_buffer,
						supported_payload, self->camera->incomplete(completed),
					});
			if (layout.line_pitch >
					static_cast<size_t>(std::numeric_limits<int32_t>::max()))
				throw std::runtime_error("eGrabber line pitch exceeds SPA stride");
			const auto sequence = self->frame_sequence.next(metadata.frame_id);
			struct spa_meta_acquisition acquisition;
			if (!spa_meta_acquisition_init(&acquisition))
				throw std::runtime_error("could not initialize acquisition metadata");
			struct spa_image_frame frame = {
				.version = SPA_VERSION_IMAGE_FRAME,
				.data_index = 0,
				.header_flags = sequence.discontinuity
					? SPA_META_HEADER_FLAG_DISCONT : 0u,
				.chunk_flags = layout.corrupted ? SPA_CHUNK_FLAG_CORRUPTED : 0u,
				.offset = static_cast<uint32_t>(layout.image_offset),
				.size = static_cast<uint32_t>(layout.data_size),
				.stride = static_cast<int32_t>(layout.line_pitch),
				.sequence = sequence.sequence,
				.pts = SPA_TIME_INVALID,
				.acquisition = &acquisition,
			};
			slot->completed.emplace(std::move(completed));
			const int publish = spa_image_source_publish_complete(
					&self->source, slot->image, &frame);
			if (publish < 0) {
				auto failed = std::move(slot->completed);
				self->camera->recycle(*failed);
				self->queued_buffers++;
				throw std::runtime_error("could not publish eGrabber image");
			}
		});
	} catch (...) {
		try {
			self->camera->clear_frame_callback();
			if (!self->ranges.empty())
				self->camera->release(self->ranges);
		} catch (...) {
		}
		self->ranges.clear();
		self->queued_buffers = 0;
		for (auto &slot : self->slots) {
			if (slot.image != nullptr)
				spa_image_source_buffer_set_user_data(slot.image, nullptr);
			slot.image = nullptr;
			slot.completed.reset();
		}
		(void) spa_image_source_teardown(&self->source);
		return -EIO;
	}
	return 0;
}

int port_set_io(void *object, enum spa_direction direction, uint32_t port_id,
		uint32_t id, void *data, size_t size)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	return spa_buffer_latest_set_io(self->latest, id, data, size);
}

int reuse_buffer(void *, uint32_t, uint32_t)
{
	return -ENOTSUP;
}

int recycle_buffers(impl *self, bool reclaim)
{
	uint32_t count = 0;

	while (count++ < self->output.n_buffers) {
		struct spa_image_source_buffer *image = nullptr;
		const int res = reclaim
			? spa_image_source_try_reclaim_submission(&self->source, &image)
			: spa_image_source_try_acquire(&self->source, &image);
		if (res <= 0)
			return res;
		auto *slot = static_cast<buffer_slot *>(
				spa_image_source_buffer_get_user_data(image));
		if (slot == nullptr || slot->image != image || !slot->completed)
			return -EPROTO;
		auto completed = std::move(slot->completed);
		self->camera->recycle(*completed);
		self->queued_buffers++;
		if (reclaim)
			return 1;
	}
	return 0;
}

int process(void *object)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	if (!self->started)
		return SPA_STATUS_OK;
	try {
		int res = recycle_buffers(self, false);
		if (res < 0)
			return res;
		const bool processed = self->camera->process_event(0);
		res = recycle_buffers(self, false);
		if (res < 0)
			return res;
		if (!processed && self->queued_buffers == 0) {
			res = recycle_buffers(self, true);
			if (res < 0)
				return res;
		}
		return SPA_STATUS_OK;
	} catch (...) {
		return -EIO;
	}
}

int send_command(void *object, const struct spa_command *command)
{
	auto *self = static_cast<impl *>(object);
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(command != nullptr, -EINVAL);
	try {
		switch (SPA_NODE_COMMAND_ID(command)) {
		case SPA_NODE_COMMAND_Start:
			if (!self->output.have_format || self->output.n_buffers == 0 ||
					!spa_buffer_latest_has_links(self->latest))
				return -EIO;
			if (self->started)
				return 0;
			if ((res = spa_buffer_latest_worker_begin(self->latest)) < 0)
				return res;
			self->started = true;
			self->frame_sequence.reset();
			self->camera->start();
			return 0;
		case SPA_NODE_COMMAND_Pause:
		case SPA_NODE_COMMAND_Suspend:
			if (!self->started)
				return 0;
			self->camera->stop();
			self->started = false;
			return spa_buffer_latest_worker_end(self->latest);
		default:
			return -ENOTSUP;
		}
	} catch (...) {
		if (self->started) {
			self->started = false;
			(void) spa_buffer_latest_worker_end(self->latest);
		}
		return -EIO;
	}
}

const struct spa_node_methods node_methods = {
	.version = SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.set_callbacks = set_callbacks,
	.enum_params = enum_params,
	.set_param = set_param,
	.set_io = set_io,
	.send_command = send_command,
	.add_port = add_port,
	.remove_port = remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = reuse_buffer,
	.process = process,
};

int get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto *self = reinterpret_cast<impl *>(handle);

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	spa_return_val_if_fail(interface != nullptr, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

int clear(struct spa_handle *handle)
{
	auto *self = reinterpret_cast<impl *>(handle);
	int res = 0;

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	if (self->started) {
		try {
			self->camera->stop();
		} catch (...) {
			res = -EIO;
		}
		self->started = false;
		const int ended = spa_buffer_latest_worker_end(self->latest);
		if (res == 0)
			res = ended;
	}
	if (self->output.n_buffers != 0) {
		const int released = release_buffers(self);
		if (res == 0)
			res = released;
	}
	spa_buffer_latest_destroy(self->latest);
	self->latest = nullptr;
	self->~impl();
	return res;
}

size_t get_size(const struct spa_handle_factory *, const struct spa_dict *)
{
	return sizeof(impl);
}

void configure_node_props(impl *self)
{
	const auto &identity = self->camera->identity();
	uint32_t n_items = 0;
	const std::string stable = !identity.serial.empty() ? identity.serial :
			std::to_string(self->options.interface_index) + "." +
			std::to_string(self->options.device_index) + "." +
			std::to_string(self->options.stream_index);
	self->node_name = "egrabber_source." + stable;
	self->description = identity.vendor;
	if (!identity.model.empty()) {
		if (!self->description.empty())
			self->description += " ";
		self->description += identity.model;
	}
	if (self->description.empty())
		self->description = "eGrabber camera source";
	self->interface_index = std::to_string(self->options.interface_index);
	self->device_index = std::to_string(self->options.device_index);
	self->stream_index = std::to_string(self->options.stream_index);

#define ADD_ITEM(key, value) \
	self->node_items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_API, "egrabber");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name.c_str());
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PRODUCER, self->options.producer.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_INTERFACE_INDEX, self->interface_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_DEVICE_INDEX, self->device_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_STREAM_INDEX, self->stream_index.c_str());
	if (!identity.vendor.empty())
		ADD_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, identity.vendor.c_str());
	if (!identity.model.empty())
		ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, identity.model.c_str());
	if (!identity.serial.empty()) {
		ADD_ITEM(SPA_KEY_DEVICE_SERIAL, identity.serial.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_SERIAL, identity.serial.c_str());
	}
	if (!identity.user_id.empty())
		ADD_ITEM(SPA_KEY_API_EGRABBER_USER_ID, identity.user_id.c_str());
	if (!identity.transport.empty())
		ADD_ITEM(SPA_KEY_API_EGRABBER_TRANSPORT, identity.transport.c_str());
#undef ADD_ITEM
	self->node_props = SPA_DICT_INIT(self->node_items, n_items);
}

int init(const struct spa_handle_factory *, struct spa_handle *handle,
		const struct spa_dict *info, const struct spa_support *support,
		uint32_t n_support)
{
	impl *self;
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = 2,
		.max_buffers = max_buffers,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION,
	};

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	self = new (handle) impl{};
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	read_options(self->options, info);
	self->log = static_cast<spa_log *>(spa_support_find(support, n_support,
			SPA_TYPE_INTERFACE_Log));
	spa_hook_list_init(&self->hooks);
	try {
		self->camera = std::make_unique<Camera>(self->options);
		if (self->camera->width() > std::numeric_limits<uint32_t>::max() ||
				self->camera->height() > std::numeric_limits<uint32_t>::max() ||
				self->camera->payload_size() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->natural_line_pitch() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->buffer_alignment() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->announce_minimum() > max_buffers ||
				self->camera->buffer_count() > max_buffers)
			throw std::runtime_error("eGrabber layout exceeds the SPA image limits");
		self->video_format = video_format(*self->camera);
		if (const auto rate = self->camera->frame_rate(); rate && *rate > 0.0)
			self->frame_rate = frame_rate(*rate);
	} catch (...) {
		self->~impl();
		return -EIO;
	}
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_RTC_PROCESS;
	configure_node_props(self);
	self->info.props = &self->node_props;
	self->output.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->output.info = SPA_PORT_INFO_INIT();
	self->output.info.flags = SPA_PORT_FLAG_LIVE;
	self->output.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	self->output.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	self->output.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	self->output.params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta,
			SPA_PARAM_INFO_READ);
	self->output.params[4] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	self->output.info.params = self->output.params;
	self->output.info.n_params = SPA_N_ELEMENTS(self->output.params);
	config.min_buffers = self->camera->announce_minimum();
	self->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, self, self->log);
	if (self->latest == nullptr) {
		self->~impl();
		return -errno;
	}
	const int res = spa_image_source_latest_init(&self->transport, &self->source,
			self->latest, &config);
	if (res < 0) {
		spa_buffer_latest_destroy(self->latest);
		self->latest = nullptr;
		self->~impl();
	}
	return res;
}

const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

int enum_interface_info(const struct spa_handle_factory *,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

} // namespace

extern "C" const struct spa_handle_factory spa_egrabber_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_EGRABBER_SOURCE,
	nullptr,
	get_size,
	init,
	enum_interface_info,
};
