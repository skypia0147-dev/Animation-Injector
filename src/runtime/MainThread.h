#pragma once
#include <atomic>
#include <cstdint>

// The thread that updates action runners. Runner state is mutated only there; callbacks
// from animation worker threads are queued and applied on it (see Actions::DrainCallbacks).
namespace AnimationInjector::MainThread {
namespace detail {
inline std::atomic<std::uint32_t> id{0};
}
std::uint32_t CurrentId();
// Plugin load runs on the game's main thread.
inline void Capture() { detail::id.store(CurrentId(), std::memory_order_release); }
// Called from the per-frame update. Returns the previous id when it had to adopt the
// updating thread (the caller logs it), otherwise 0.
inline std::uint32_t Confirm() {
    const auto current = CurrentId();
    const auto previous = detail::id.exchange(current, std::memory_order_acq_rel);
    return previous != current ? (previous ? previous : current) : 0;
}
inline bool IsCurrent() { return detail::id.load(std::memory_order_acquire) == CurrentId(); }
}
