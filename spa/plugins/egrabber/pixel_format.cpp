/* SPDX-License-Identifier: MIT */

#include "pixel_format.hpp"

namespace egrabber_pipewire {

std::optional<PixelByteOrder> infer_unpacked_byte_order(
    std::string_view pixel_format, const std::byte *payload, std::size_t size) {
    unsigned int padding_mask = 0;
    if (pixel_format == "Mono10") padding_mask = 0xfcU;
    else if (pixel_format == "Mono12") padding_mask = 0xf0U;
    else if (pixel_format == "Mono14") padding_mask = 0xc0U;
    else return std::nullopt;

    std::size_t even_padding_violations = 0;
    std::size_t odd_padding_violations = 0;
    for (std::size_t offset = 0; offset + 1 < size; offset += 2) {
        const auto even = std::to_integer<unsigned int>(payload[offset]);
        const auto odd = std::to_integer<unsigned int>(payload[offset + 1]);
        if ((even & padding_mask) != 0) ++even_padding_violations;
        if ((odd & padding_mask) != 0) ++odd_padding_violations;
    }
    if (even_padding_violations == 0 && odd_padding_violations != 0)
        return PixelByteOrder::big;
    if (odd_padding_violations == 0 && even_padding_violations != 0)
        return PixelByteOrder::little;
    return std::nullopt;
}

} // namespace egrabber_pipewire
