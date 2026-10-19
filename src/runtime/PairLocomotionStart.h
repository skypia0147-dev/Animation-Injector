#pragma once
#include <array>
#include <optional>
#include <string_view>

namespace AnimationInjector {
struct LocomotionStartState {
    int id;
    std::string_view name;
};
inline std::optional<int> PairLocomotionStart(bool ownedExit, bool clone, bool defaultMode, std::string_view machine,
    int start, const std::array<LocomotionStartState, 2>& states) {
    if (!ownedExit || !clone || !defaultMode || machine != "MT_Default_Behavior") return {};
    const LocomotionStartState* standing = nullptr;
    const LocomotionStartState* moving = nullptr;
    for (const auto& state : states) {
        if (state.name == "MT_Standing_State") standing = &state;
        if (state.name == "MT_LocomotionType_State") moving = &state;
    }
    if (!standing || !moving || standing->id != start || moving->id == start || moving->id < 0) return {};
    return moving->id;
}
// Change only the default used by native Activate, never the live state ID.
// Restore the clone's declaration even on nested activation or an exception.
template<class Activate>
void ActivateLocomotionStart(int& start, int destination, Activate&& activate) {
    struct Restore {
        int& field;
        int saved;
        ~Restore() { field = saved; }
    } restore{start, start};
    start = destination;
    activate();
}
}
