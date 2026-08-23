/* SPDX-License-Identifier: MIT */

#include "feature.hpp"

#include <algorithm>
#include <string_view>

namespace egrabber_pipewire {

const char *kind_name(FeatureKind kind) {
    switch (kind) {
    case FeatureKind::boolean: return "boolean";
    case FeatureKind::integer: return "integer";
    case FeatureKind::floating: return "float";
    case FeatureKind::enumeration: return "enum";
    case FeatureKind::string: return "string";
    case FeatureKind::command: return "command";
    case FeatureKind::unsupported: return "unsupported";
    }
    return "unsupported";
}

bool changes_payload_layout(const Feature &feature) {
    const auto &name = feature.name;
    static constexpr std::string_view exact[] = {
        "PixelFormat", "Width", "Height", "OffsetX", "OffsetY", "PayloadSize",
        "ChunkModeActive",
    };
    if (std::find(std::begin(exact), std::end(exact), name) != std::end(exact)) return true;
    static constexpr std::string_view fragments[] = {
        "Binning", "Decimation", "Resolution", "Region", "ComponentEnable",
        "ChunkEnable",
    };
    return std::any_of(std::begin(fragments), std::end(fragments), [&](std::string_view fragment) {
        return name.find(fragment) != std::string::npos;
    });
}

} // namespace egrabber_pipewire
