#pragma once
#include <unordered_map>

namespace RE {
struct Actor {
    bool player = false;
    bool IsPlayerRef() const { return player; }
};
}
// Test double: the actor of each character, as the engine's graph holder.
namespace AnimationInjector::ClipHooks {
inline std::unordered_map<const RE::hkbCharacter*, RE::Actor*> actors;
inline RE::Actor* ActorForContext(const RE::hkbContext& context) {
    const auto found = actors.find(context.character);
    return found == actors.end() ? nullptr : found->second;
}
}
