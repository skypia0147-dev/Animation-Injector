#pragma once
#include <cstdint>
#include <utility>

namespace AnimationInjector::PairStartTransfer {
// Move a startup lease without a release/reacquire gap. Stale or repeated
// handoffs cannot take a different request's camera ownership.
inline std::uint64_t ClaimCamera(std::uint64_t& source, std::uint64_t expected) {
    if (!expected || source != expected) return 0;
    return std::exchange(source, 0);
}
}
