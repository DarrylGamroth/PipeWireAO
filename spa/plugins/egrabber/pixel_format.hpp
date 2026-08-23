/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace egrabber_pipewire {

enum class PixelByteOrder { little, big };

std::optional<PixelByteOrder> infer_unpacked_byte_order(
    std::string_view pixel_format, const std::byte *payload, std::size_t size);

} // namespace egrabber_pipewire
