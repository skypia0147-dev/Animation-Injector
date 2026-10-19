#pragma once
#include <array>
#include <span>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace AnimationInjector::PoseTrackReader {
// Havok output layout cross-checked against local OAR src/Havok/Havok.h.
// CommonLib only forward-declares this output. No writes or ownership are implied.
struct Master {
    std::int32_t bytes, tracks;
    std::array<std::byte, 8> unused;
};
struct Header {
    std::int16_t capacity, count, offset, stride;
    float fraction;
    std::uint8_t flags, type;
    std::uint16_t padding;
};
static_assert(sizeof(Master) == 16 && sizeof(Header) == 16);
struct Output {
    const Master* tracks;
    bool deleteTracks;
};
static_assert(sizeof(Output) == 16);
struct Result {
    int error = 1, count = 0, flags = 0;
    float fraction = 0;
    std::array<float, 12> qs{};
};
// Error: 0 valid; 1 master; 2 track; 3 format; 4 bone index; 5 range; 6 nonfinite.
inline Result Read(std::span<const std::byte> buffer, int track, int bone) {
    Result result;
    if (buffer.size() < sizeof(Master)) return result;
    Master master;
    std::memcpy(&master, buffer.data(), sizeof(master));
    if (master.bytes < 16 || master.bytes > 1024 * 1024 || static_cast<std::size_t>(master.bytes) > buffer.size() ||
        master.tracks < 1 || master.tracks > 128)
        return result;
    const auto end = sizeof(Master) + static_cast<std::size_t>(master.tracks) * sizeof(Header);
    if (end > static_cast<std::size_t>(master.bytes) || track < 0 || track >= master.tracks) {
        result.error = 2;
        return result;
    }
    Header h;
    std::memcpy(&h, buffer.data() + sizeof(Master) + track * sizeof(Header), sizeof(h));
    result.count = h.count;
    result.flags = h.flags;
    result.fraction = h.fraction;
    // Sparse/palette/additive tracks require different interpretation; never guess.
    if (h.type != 1 || h.flags != 0 || h.stride != 48 || !std::isfinite(h.fraction) || h.fraction <= 0) {
        result.error = 3;
        return result;
    }
    if (h.count < 0 || h.capacity < h.count || bone < 0 || bone >= h.count) {
        result.error = 4;
        return result;
    }
    if (h.offset < 0 || static_cast<std::size_t>(h.offset) < end ||
        static_cast<std::size_t>(h.offset) + static_cast<std::size_t>(h.capacity) * 48 >
            static_cast<std::size_t>(master.bytes)) {
        result.error = 5;
        return result;
    }
    std::memcpy(result.qs.data(), buffer.data() + h.offset + bone * 48, 48);
    // SIMD padding lanes are not semantic transform components.
    for (int i : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10})
        if (!std::isfinite(result.qs[i])) {
            result.error = 6;
            return result;
        }
    result.error = 0;
    return result;
}
inline Result Read(const Output& output, int track, int bone) {
    if (!output.tracks || output.tracks->bytes < 16 || output.tracks->bytes > 1024 * 1024) return {};
    return Read(
        std::span(reinterpret_cast<const std::byte*>(output.tracks), static_cast<std::size_t>(output.tracks->bytes)),
        track, bone);
}
}
