#pragma once
#include "runtime/EventRegistrySpec.h"
#include <span>
#include <vector>

// Pairs an action's <EventValues> functions with the registry functions of one event (0.12.281), without engine
// headers (registry build and tests).
namespace AnimationInjector {
// For each registry function of one event, the index into `values` giving its numbers, or -1: matched by op, in
// order among the functions of that op. `unmatched` gets the values no registry function took.
inline std::vector<int> MatchValues(std::span<const EventFunctionSpec> registry,
    std::span<const EventFunctionSpec> values, std::vector<int>* unmatched = nullptr) {
    std::vector<int> out(registry.size(), -1);
    std::vector<bool> taken(values.size(), false);
    for (std::size_t r = 0; r < registry.size(); ++r)
        for (std::size_t v = 0; v < values.size(); ++v)
            if (!taken[v] && values[v].op == registry[r].op) {
                out[r] = static_cast<int>(v);
                taken[v] = true;
                break;
            }
    if (unmatched)
        for (std::size_t v = 0; v < values.size(); ++v)
            if (!taken[v]) unmatched->push_back(static_cast<int>(v));
    return out;
}
}
