#pragma once
#include "PCH.h"
#include <vector>
// Records the late presses handed to the game (core/GameInputReplay.h).
namespace AnimationInjector::GameInputReplay {
struct Sent {
    std::string userEvent;
    bool tap;
};
inline std::vector<Sent> sent;
inline void Send(RE::INPUT_DEVICE, std::uint32_t, const RE::BSFixedString& userEvent, bool tap) {
    sent.push_back({userEvent, tap});
}
inline bool Replaying() { return false; }
}
