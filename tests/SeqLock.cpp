#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "core/SeqLock.h"

// CarryPoseLock's followed root: one writer, readers on other threads. Every stored value has all
// seven floats equal, so a torn read shows up as two different floats in one loaded value.
int main() {
    using AnimationInjector::SeqLock;
    unsigned checks = 0;
    const auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) {
            std::cerr << "SeqLock check failed: " << what << '\n';
            std::exit(1);
        }
    };
    {
        SeqLock<7> lock;
        std::array<float, 7> value{};
        check(!lock.Load(value) && !lock.Published(), "nothing stored yet");
        lock.Store({1, 2, 3, 4, 5, 6, 7});
        check(lock.Load(value) && value == std::array<float, 7>{1, 2, 3, 4, 5, 6, 7}, "stored value reads back");
        lock.Clear();
        check(!lock.Load(value) && !lock.Published(), "cleared");
    }
    SeqLock<7> lock;
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> reads{0}, torn{0}, skipped{0};
    const auto reader = [&] {
        std::array<float, 7> value{};
        while (!stop.load(std::memory_order_relaxed)) {
            if (!lock.Load(value)) {
                ++skipped;
                continue;
            }
            ++reads;
            for (float v : value)
                if (v != value[0]) {
                    ++torn;
                    break;
                }
        }
    };
    std::thread a(reader), b(reader);
    for (unsigned i = 1; i <= 2'000'000; ++i) {
        const float v = static_cast<float>(i);
        lock.Store({v, v, v, v, v, v, v});
    }
    stop = true;
    a.join();
    b.join();
    check(reads > 1000, "readers ran");
    check(torn == 0, "no reader saw a half-written value");
    std::cout << "SeqLock checks passed: " << checks << " (reads " << reads << ", retried out " << skipped << ", torn "
              << torn << ")\n";
    return 0;
}
