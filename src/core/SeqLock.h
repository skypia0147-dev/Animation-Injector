#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AnimationInjector {
// A few floats one writer publishes and other threads read without taking a lock: the writer never
// waits, and a reader that raced a write retries, so it never sees half of one value and half of
// another. For values written about once a frame (CarryPoseLock's followed root).
template<std::size_t N>
class SeqLock {
public:
    // One writer at a time.
    void Store(const std::array<float, N>& value) {
        const auto s = sequence.load(std::memory_order_relaxed);
        sequence.store(s + 1, std::memory_order_relaxed);  // odd: a write is in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < N; ++i) data[i].store(value[i], std::memory_order_relaxed);
        sequence.store(s + 2, std::memory_order_release);
        published.store(true, std::memory_order_release);
    }
    // false: nothing stored yet, or a write kept racing this read (the caller skips this use).
    bool Load(std::array<float, N>& value) const {
        if (!published.load(std::memory_order_acquire)) return false;
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            const auto before = sequence.load(std::memory_order_acquire);
            if (before & 1) continue;
            for (std::size_t i = 0; i < N; ++i) value[i] = data[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence.load(std::memory_order_relaxed) == before) return true;
        }
        return false;
    }
    bool Published() const { return published.load(std::memory_order_acquire); }
    // Only when no reader can be using it (the writer's Release).
    void Clear() { published.store(false, std::memory_order_release); }

private:
    std::atomic<std::uint32_t> sequence{0};
    std::array<std::atomic<float>, N> data{};
    std::atomic<bool> published{false};
};
}
