#include "runtime/BoundedMailbox.h"
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace AnimationInjector;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        BoundedMailbox<int> box(3);
        Check(box.Post(1) && box.Post(2) && box.Post(3), "posts up to capacity");
        Check(!box.Post(4), "full mailbox refuses a new item");
        Check(box.TakeDropped() == 1 && box.TakeDropped() == 0, "drop count is reported once");
        const auto first = box.TakeAll();
        Check(first.size() == 3 && first[0] == 1 && first[1] == 2 && first[2] == 3, "items come out in arrival order");
        Check(box.TakeAll().empty(), "taking empties the mailbox");
        Check(box.Post(5), "space returns after taking");
        box.Clear();
        Check(box.TakeAll().empty(), "clear drops pending items (game load)");

        // Worker threads post while the main thread drains; nothing is lost or duplicated.
        BoundedMailbox<int> shared(100000);
        constexpr int threads = 4, per = 5000;
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back([&, t] {
                for (int i = 0; i < per; ++i) shared.Post(t * per + i);
            });
        std::vector<int> seen(threads * per, 0);
        std::size_t total = 0;
        while (total < static_cast<std::size_t>(threads * per)) {
            for (const int v : shared.TakeAll()) {
                ++seen[v];
                ++total;
            }
            std::this_thread::yield();
        }
        for (auto& w : workers) w.join();
        for (const auto& v : shared.TakeAll()) ++seen[v];
        bool exact = true;
        for (const int count : seen) exact &= count == 1;
        Check(exact, "every concurrent post is taken exactly once");
        Check(shared.TakeDropped() == 0, "no drops below capacity");
        std::printf("%u mailbox checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
