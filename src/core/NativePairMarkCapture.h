#pragma once
#include "core/PairPlacementMath.h"
#include <cstdint>

namespace AnimationInjector::NativePairMarkCapture {
struct Identity {
    std::uintptr_t generator = 0, character = 0;
    bool operator==(const Identity&) const = default;
};
// Stack-scoped native Start observations. Never retain Havok object pointers.
struct Capture {
    std::array<Identity, 2> identities{};  // support, leader
    std::array<PairPlacementMath::Mark, 2> marks{};
    std::array<bool, 2> seen{};
    bool invalid = false;
    int Record(Identity identity, bool accepted, const PairPlacementMath::Mark& mark) {
        if (!identity.generator || !identity.character) return -1;
        for (unsigned i = 0; i < 2; ++i)
            if (identities[i] == identity) {
                if (!accepted || !PairPlacementMath::Valid(mark))
                    invalid = true;
                else if (seen[i] && (marks[i].translation != mark.translation || marks[i].rotation != mark.rotation))
                    invalid = true;
                else {
                    marks[i] = mark;
                    seen[i] = true;
                }
                return static_cast<int>(i);
            }
        return -1;
    }
    bool Ready(bool nativeAccepted) const { return nativeAccepted && !invalid && seen[0] && seen[1]; }
    std::array<PairPlacementMath::Mark, 2> SolverMarks() const { return {marks[1], marks[0]}; }
};
}
