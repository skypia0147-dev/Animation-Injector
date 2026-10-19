// RunnerSet: sparse per-actor runners (0.12.208). Ascending walks, mid-walk creation, word
// boundaries, and lock-free reads while another thread creates.
#include "runtime/RunnerSet.h"
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using AnimationInjector::RunnerSet;
namespace {
struct Fake {
    explicit Fake(int a) : action(a) {}
    int action;
};
std::atomic<int> failures{0};
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
auto Make() {
    return [](int a) { return std::make_unique<Fake>(a); };
}
std::vector<int> Walk(const RunnerSet<Fake>& set) {
    std::vector<int> out;
    set.ForEach([&](int a, Fake& r) {
        if (r.action != a) ++failures;
        out.push_back(a);
    });
    return out;
}
}

int main() {
    {
        RunnerSet<Fake> set(200);
        Check(set.Live() == 0 && set.Find(5) == nullptr && set.Next(-1) == -1, "empty");
        for (int a : {130, 7, 64, 63, 199, 0, 65, 7})
            if (set.Get(a, Make())->action != a) ++failures;
        Check(set.Live() == 7, "each action created once");
        Check(Walk(set) == std::vector<int>{0, 7, 63, 64, 65, 130, 199}, "ascending across word boundaries");
        Check(set.Get(-1, Make()) == nullptr && set.Get(200, Make()) == nullptr && set.Find(200) == nullptr,
            "out of range");
        Check(set.Next(199) == -1 && set.Next(63) == 64 && set.Next(-5) == 0, "Next");
    }
    {
        // A runner created during a walk is visited when it comes after the current action.
        RunnerSet<Fake> set(100);
        set.Get(10, Make());
        set.Get(50, Make());
        std::vector<int> seen;
        set.ForEach([&](int a, Fake&) {
            seen.push_back(a);
            if (a == 10) {
                set.Get(5, Make());   // behind: not visited in this walk
                set.Get(30, Make());  // ahead: visited
            }
        });
        Check(seen == std::vector<int>{10, 30, 50}, "mid-walk creation");
        Check(Walk(set) == std::vector<int>{5, 10, 30, 50}, "next walk sees all");
        Check(set.FindIf([](int a, Fake&) { return a > 20; })->action == 30, "FindIf");
    }
    {
        RunnerSet<Fake> zero(0);
        Check(zero.Next(-1) == -1 && zero.Get(0, Make()) == nullptr, "zero capacity");
    }
    {
        // One creator, two readers: every pointer a reader sees is a complete runner for its action.
        constexpr int N = 5000;
        RunnerSet<Fake> set(N);
        std::atomic_bool done{false};
        std::atomic<long long> reads{0};
        std::atomic<int> started{0};
        auto reader = [&] {
            std::mt19937 rng(7);
            ++started;
            while (!done.load()) {
                int count = 0, last = -1;
                set.ForEach([&](int a, Fake& r) {
                    if (r.action != a || a <= last) ++failures;
                    last = a;
                    ++count;
                });
                const int a = static_cast<int>(rng() % N);
                if (auto* r = set.Find(a); r && r->action != a) ++failures;
                reads += count;
            }
        };
        std::thread r1(reader), r2(reader);
        std::mt19937 rng(1);
        while (started.load() < 2) std::this_thread::yield();
        for (int i = 0; i < 20000; ++i) {
            set.Get(static_cast<int>(rng() % N), Make());
            if (i % 16 == 0) std::this_thread::yield();
        }
        done = true;
        r1.join();
        r2.join();
        int live = 0;
        set.ForEach([&](int, Fake&) { ++live; });
        Check(live == set.Live(), "live count matches walk");
        Check(reads.load() > 0, "readers walked while the set grew");
        std::printf("concurrent: live=%d reads=%lld\n", set.Live(), reads.load());
    }
    std::printf(failures ? "RunnerSet FAILED (%d)\n" : "RunnerSet ok\n", failures.load());
    return failures ? 1 : 0;
}
