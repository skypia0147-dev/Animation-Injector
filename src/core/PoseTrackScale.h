#pragma once
#include "core/PoseTrackReader.h"
#include <cmath>
#include <cstring>
#include <span>

namespace AnimationInjector::PoseTrackScale {
// The one write into a generator output: scale one bone's translation in a dense Qs pose track
// in place, only after the exact format and bounds checks PoseTrackReader::Read applies.
// Returns the reader's error code (0 = scaled), or 7 for an invalid factor.
inline int ScaleTranslation(std::span<std::byte> buffer, int track, int bone, float factor) {
    const auto read = PoseTrackReader::Read(std::span<const std::byte>(buffer.data(), buffer.size()), track, bone);
    if (read.error) return read.error;
    if (!std::isfinite(factor) || factor <= 0) return 7;
    PoseTrackReader::Header header;
    std::memcpy(&header, buffer.data() + sizeof(PoseTrackReader::Master) + track * sizeof(PoseTrackReader::Header),
        sizeof(header));
    auto* translation = buffer.data() + header.offset + bone * 48;
    float t[3];
    std::memcpy(t, translation, sizeof(t));
    for (float& v : t) v *= factor;
    std::memcpy(translation, t, sizeof(t));
    return 0;
}
// The engine owns the output buffer for the duration of the generate call it was passed to.
inline int ScaleTranslation(PoseTrackReader::Output& output, int track, int bone, float factor) {
    if (!output.tracks || output.tracks->bytes < 16 || output.tracks->bytes > 1024 * 1024) return 1;
    auto* base = const_cast<std::byte*>(reinterpret_cast<const std::byte*>(output.tracks));
    return ScaleTranslation(
        std::span<std::byte>(base, static_cast<std::size_t>(output.tracks->bytes)), track, bone, factor);
}
}
