#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>
#include <numbers>

namespace AnimationInjector::PairMotionMath {
struct Key {
    float time = 0, x = 0, y = 0, z = 0;
};
struct TranslationBasis {
    bool captured = false;
    float yaw = 0;
    void Observe(float movement, float actorYaw, float appliedRotation) {
        // Zero-time/zero-motion samples can arrive before native paired alignment finishes.
        if (captured || movement <= 0.0001f || !std::isfinite(actorYaw) || !std::isfinite(appliedRotation)) return;
        yaw = std::remainder(actorYaw - appliedRotation, 2 * std::numbers::pi_v<float>);
        captured = true;
    }
};
inline bool Number(std::string_view& text, float& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (result.ec != std::errc{} || !std::isfinite(out)) return false;
    text.remove_prefix(static_cast<std::size_t>(result.ptr - text.data()));
    return true;
}
inline bool Empty(std::string_view text) { return text.find_first_not_of(" \t\r\n") == text.npos; }
inline bool Parse(std::string_view text, float time, Key& key, bool& rotation) {
    if (!std::isfinite(time) || time < 0) return false;
    key = {};
    key.time = time;
    if (text.starts_with("animmotion ")) {
        text.remove_prefix(11);
        rotation = false;
        return Number(text, key.x) && Number(text, key.y) && Number(text, key.z) && Empty(text);
    }
    if (text.starts_with("animrotation ")) {
        text.remove_prefix(13);
        rotation = true;
        return Number(text, key.z) && Empty(text);
    }
    return false;
}
inline void Sort(std::vector<Key>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    // Last annotation at a duplicate timestamp wins deterministically.
    std::vector<Key> result;
    for (auto k : keys) {
        if (!result.empty() && result.back().time == k.time)
            result.back() = k;
        else
            result.push_back(k);
    }
    keys = std::move(result);
}
inline Key Sample(const std::vector<Key>& keys, float time) {
    Key previous;
    for (const auto& key : keys) {
        if (time <= key.time) {
            const float span = key.time - previous.time;
            const float f = span > 0 ? std::clamp((time - previous.time) / span, 0.0f, 1.0f) : 1.0f;
            return {time, previous.x + (key.x - previous.x) * f, previous.y + (key.y - previous.y) * f,
                previous.z + (key.z - previous.z) * f};
        }
        previous = key;
    }
    return previous;
}
}
