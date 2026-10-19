#pragma once
#include <cmath>
#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>

namespace AnimationInjector::CarryContinuity {
struct HitMailbox {
    std::mutex lifecycle;
    std::array<std::atomic<std::uint64_t>, 2> watched{}, pending{};
    std::uint32_t generation = 0;
    void Reset() {
        std::lock_guard lock(lifecycle);
        for (auto& v : watched) v = 0;
        for (auto& v : pending) v = 0;
    }
    void Arm(std::uint32_t a, std::uint32_t b) {
        std::lock_guard lock(lifecycle);
        for (auto& v : watched) v = 0;
        for (auto& v : pending) v = 0;
        if (!++generation) ++generation;
        watched[0] = (std::uint64_t(generation) << 32) | a;
        watched[1] = (std::uint64_t(generation) << 32) | b;
    }
    std::uint64_t Watch(std::uint32_t id) const {
        if (!id) return 0;
        for (const auto& v : watched) {
            const auto token = v.load();
            if (std::uint32_t(token) == id) return token;
        }
        return 0;
    }
    bool Owns(std::uint64_t token) const { return token && (watched[0].load() == token || watched[1].load() == token); }
    void Hit(std::uint64_t token) {
        if (!token) return;
        std::lock_guard lock(lifecycle);
        for (unsigned i = 0; i < 2; ++i)
            if (token && watched[i].load() == token) pending[i] = token;
    }
    bool Consume(unsigned role) {
        const auto token = pending[role].exchange(0);
        return token && watched[role].load() == token;
    }
};
// This bounds a failed reconstruction attempt, never the carrying duration.
// Loading screens and missing destination cells do not consume recovery time.
struct TransferGate {
    std::uint32_t destination = 0;
    float readyAge = 0, retry = 0;
    unsigned attempts = 0, stable = 0;
    void Destination(std::uint32_t cell) {
        if (cell && cell != destination) {
            *this = {};
            destination = cell;
        }
    }
    bool Tick(float delta, bool paused, bool destinationReady) {
        if (paused || !destinationReady || !std::isfinite(delta) || delta <= 0) return false;
        readyAge += delta;
        retry -= delta;
        return readyAge >= 15.f;
    }
    bool CanMove() const { return destination && attempts < 3 && retry <= 0; }
    void Moved() {
        ++attempts;
        retry = .5f;
        stable = 0;
    }
    bool Ready(bool ready) {
        stable = ready ? stable + 1 : 0;
        return stable >= 2;
    }
};
}
