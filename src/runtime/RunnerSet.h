#pragma once
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace AnimationInjector {
// The runners of one actor, one slot per declared action, created when the action is first used.
// Before 0.12.208 every actor held a runner for every action, and every frame, input and animation
// event walked all of them; with thousands of declared actions that walk was the cost, not the work.
//
// Lookups and walks are lock-free from any thread; creation takes a lock. A runner is never
// removed while its set lives, so a pointer read from any thread stays valid while the set is held.
// Walks visit runners in ascending action order, as the former walk over every action did: a
// runner created during a walk is visited when its action comes after the current one.
template<class Runner>
class RunnerSet {
public:
    explicit RunnerSet(int count)
        : _count(count > 0 ? count : 0), _slots(new std::atomic<Runner*>[static_cast<std::size_t>(_count)]),
          _bits(new std::atomic<std::uint64_t>[Words(_count)]) {
        for (int i = 0; i < _count; ++i) _slots[i].store(nullptr, std::memory_order_relaxed);
        for (std::size_t w = 0; w < Words(_count); ++w) _bits[w].store(0, std::memory_order_relaxed);
    }
    RunnerSet(const RunnerSet&) = delete;
    RunnerSet& operator=(const RunnerSet&) = delete;

    int Capacity() const noexcept { return _count; }
    int Live() const noexcept { return _live.load(std::memory_order_acquire); }
    Runner* Find(int action) const noexcept {
        return action >= 0 && action < _count ? _slots[action].load(std::memory_order_acquire) : nullptr;
    }
    // make(action) returns a std::unique_ptr<Runner>; it runs under the set's lock.
    template<class Make>
    Runner* Get(int action, Make&& make) {
        if (auto* runner = Find(action)) return runner;
        if (action < 0 || action >= _count) return nullptr;
        std::lock_guard lock(_lock);
        if (auto* runner = _slots[action].load(std::memory_order_relaxed)) return runner;
        _owned.push_back(make(action));
        auto* runner = _owned.back().get();
        _slots[action].store(runner, std::memory_order_release);
        _bits[static_cast<std::size_t>(action) / 64].fetch_or(
            std::uint64_t{1} << (static_cast<unsigned>(action) % 64), std::memory_order_release);
        _live.fetch_add(1, std::memory_order_release);
        return runner;
    }
    // The first existing runner after `after` (-1 = from the start), or -1.
    int Next(int after) const noexcept {
        int from = after + 1;
        if (from < 0) from = 0;
        for (auto word = static_cast<std::size_t>(from) / 64; word < Words(_count); ++word) {
            auto bits = _bits[word].load(std::memory_order_acquire);
            if (word == static_cast<std::size_t>(from) / 64)
                bits &= ~std::uint64_t{0} << (static_cast<unsigned>(from) % 64);
            if (bits) return static_cast<int>(word * 64 + static_cast<std::size_t>(std::countr_zero(bits)));
        }
        return -1;
    }
    // f(action, runner) in ascending action order.
    template<class F>
    void ForEach(F&& f) const {
        for (int action = Next(-1); action >= 0; action = Next(action))
            if (auto* runner = Find(action)) f(action, *runner);
    }
    // First runner for which f(action, runner) is true, or nullptr.
    template<class F>
    Runner* FindIf(F&& f) const {
        for (int action = Next(-1); action >= 0; action = Next(action))
            if (auto* runner = Find(action); runner && f(action, *runner)) return runner;
        return nullptr;
    }

private:
    static std::size_t Words(int count) { return (static_cast<std::size_t>(count) + 63) / 64 + 1; }
    int _count;
    std::unique_ptr<std::atomic<Runner*>[]> _slots;
    std::unique_ptr<std::atomic<std::uint64_t>[]> _bits;
    std::atomic<int> _live{0};
    std::mutex _lock;
    std::vector<std::unique_ptr<Runner>> _owned;  // guarded by _lock; destroyed with the set
};
}
