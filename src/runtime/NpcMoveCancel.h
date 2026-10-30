#pragma once
#include <cctype>
#include <string_view>

namespace AnimationInjector::NpcMoveCancel {
// An NPC's move cancel (10-30, checked in game): an NPC sends no move request while its action plays (the AI waits for
// it to end), so a move cancel cannot wait for one. From the action's move-cancel point, in combat, the AI gets
// movement back while the clip plays on (ActionRunner::OpenRecovery); the first request it then sends ends the action
// with the move-cancel blend, and without one the clip plays out. In the check the AI turned and moved 10 ms after
// getting movement back, every time: an NPC in combat leaves at the move-cancel point.

// Whether a graph event is such a request: to move, sprint, bash, block or attack (any attack event). Names as the
// engine sends them; case does not matter, as in the engine.
inline bool IsRequest(std::string_view event) {
    const auto same = [&](std::string_view name, bool prefix) {
        if (prefix ? event.size() < name.size() : event.size() != name.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(event[i])) != std::tolower(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    };
    return same("moveStart", false) || same("sprintStart", false) || same("bashStart", false) ||
           same("blockStart", false) || same("attack", true);
}
}  // namespace AnimationInjector::NpcMoveCancel
