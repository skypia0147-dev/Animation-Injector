#pragma once
#include <algorithm>
#include <vector>

namespace AnimationInjector {
// Accepted/failed input results only live on the native input-dispatch stack.
// A running action must not turn startable into active after that stack unwinds.
class InputAcceptance {
public:
    static constexpr int unseen = -2, failed = -1;
    // Only the sources an input batch touched are stored: a batch records a few requests, while
    // there can be thousands of actions.
    void Reset(int count) {
        depth = 0;
        actions = count;
        results.clear();
    }
    void Begin(int count) {
        if (depth++ == 0) {
            actions = count;
            results.clear();
        }
    }
    void End() {
        if (depth && --depth == 0) results.clear();
    }
    void Record(int source, int destination, bool accepted) {
        if (!depth || source < 0 || source >= actions) return;
        auto* slot = Find(source);
        if (!slot) {
            results.push_back({source, unseen});
            slot = &results.back().second;
        }
        if (accepted || *slot < 0) *slot = accepted ? destination : failed;
    }
    int Result(int source) const {
        if (!depth || source < 0 || source >= actions) return unseen;
        for (const auto& [s, r] : results)
            if (s == source) return r;
        return unseen;
    }

private:
    int* Find(int source) {
        for (auto& [s, r] : results)
            if (s == source) return &r;
        return nullptr;
    }
    unsigned depth = 0;
    int actions = 0;
    std::vector<std::pair<int, int>> results;  // source -> destination or failed
};
}
