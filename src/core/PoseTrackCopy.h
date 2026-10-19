#pragma once
#include "core/PoseTrackReader.h"
#include <array>
#include <cmath>
#include <cstring>
#include <span>

namespace AnimationInjector::PoseTrackCopy {
// A whole dense Qs pose track, copied out of one generator output and written into later ones
// (the carried target's locked pose). Same format and bounds checks as PoseTrackReader::Read.
constexpr int kMaxBones = 256;
struct Pose {
    int count = 0;
    std::array<std::byte, kMaxBones * 48> data{};
};
namespace detail {
// The track header after the reader has validated the master, header, format and bounds.
inline PoseTrackReader::Header TrackHeader(std::span<const std::byte> buffer, int track) {
    PoseTrackReader::Header header;
    std::memcpy(&header, buffer.data() + sizeof(PoseTrackReader::Master) + track * sizeof(PoseTrackReader::Header),
        sizeof(header));
    return header;
}
}
// Returns the reader's error code (0 = captured), or 8 for more than kMaxBones bones.
inline int Capture(std::span<const std::byte> buffer, int track, Pose& out) {
    const auto read = PoseTrackReader::Read(buffer, track, 0);
    if (read.error) return read.error;
    if (read.count > kMaxBones) return 8;
    const auto header = detail::TrackHeader(buffer, track);
    const auto* bones = buffer.data() + header.offset;
    // Every bone must be finite, not only the one the reader looked at (SIMD padding lanes aside).
    for (int b = 0; b < read.count; ++b) {
        float qs[12];
        std::memcpy(qs, bones + b * 48, sizeof(qs));
        for (int i : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10})
            if (!std::isfinite(qs[i])) return 6;
    }
    std::memcpy(out.data.data(), bones, static_cast<std::size_t>(read.count) * 48);
    out.count = read.count;
    return 0;
}
// Writes `pose` over a track of the same bone count. Returns the reader's error code (0 = written),
// or 9 when the bone count differs; nothing is written on any error.
inline int Apply(std::span<std::byte> buffer, int track, const Pose& pose) {
    const auto read = PoseTrackReader::Read(std::span<const std::byte>(buffer.data(), buffer.size()), track, 0);
    if (read.error) return read.error;
    if (read.count != pose.count) return 9;
    const auto header = detail::TrackHeader(std::span<const std::byte>(buffer.data(), buffer.size()), track);
    std::memcpy(buffer.data() + header.offset, pose.data.data(), static_cast<std::size_t>(pose.count) * 48);
    return 0;
}
// Writes bone 0's translation and rotation (Havok order x, y, z, w) over a track; its scale and the
// SIMD padding lanes are kept. Returns the reader's error code (0 = written), or 6 for a non-finite
// value; nothing is written on any error.
inline int WriteRoot(
    std::span<std::byte> buffer, int track, std::span<const float, 3> translation, std::span<const float, 4> rotation) {
    for (float v : translation)
        if (!std::isfinite(v)) return 6;
    for (float v : rotation)
        if (!std::isfinite(v)) return 6;
    const auto read = PoseTrackReader::Read(std::span<const std::byte>(buffer.data(), buffer.size()), track, 0);
    if (read.error) return read.error;
    const auto header = detail::TrackHeader(std::span<const std::byte>(buffer.data(), buffer.size()), track);
    auto* root = buffer.data() + header.offset;
    std::memcpy(root, translation.data(), 3 * sizeof(float));
    std::memcpy(root + 4 * sizeof(float), rotation.data(), 4 * sizeof(float));
    return 0;
}
// The engine owns the output buffer for the duration of the generate call it was passed to.
inline int Capture(const PoseTrackReader::Output& output, int track, Pose& out) {
    if (!output.tracks || output.tracks->bytes < 16 || output.tracks->bytes > 1024 * 1024) return 1;
    return Capture(
        std::span(reinterpret_cast<const std::byte*>(output.tracks), static_cast<std::size_t>(output.tracks->bytes)),
        track, out);
}
inline int Apply(PoseTrackReader::Output& output, int track, const Pose& pose) {
    if (!output.tracks || output.tracks->bytes < 16 || output.tracks->bytes > 1024 * 1024) return 1;
    auto* base = const_cast<std::byte*>(reinterpret_cast<const std::byte*>(output.tracks));
    return Apply(std::span<std::byte>(base, static_cast<std::size_t>(output.tracks->bytes)), track, pose);
}
inline int WriteRoot(PoseTrackReader::Output& output, int track, std::span<const float, 3> translation,
    std::span<const float, 4> rotation) {
    if (!output.tracks || output.tracks->bytes < 16 || output.tracks->bytes > 1024 * 1024) return 1;
    auto* base = const_cast<std::byte*>(reinterpret_cast<const std::byte*>(output.tracks));
    return WriteRoot(
        std::span<std::byte>(base, static_cast<std::size_t>(output.tracks->bytes)), track, translation, rotation);
}
}
