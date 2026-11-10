#pragma once

#include <cstdint>
#include <string>

// Who turns an actor while a paired start holds its heading (elderwraith/RadiantWings 11-08: a kill move's attacker
// was turned back 148 degrees every frame for 3 s, Kaputt installed). Entry hooks on the engine's two heading
// setters, Actor::SetHeading and TESObjectREFR::SetAngle, record each call on a watched actor that Animation
// Injector did not make, with the module that made it (the first return address outside SkyrimSE.exe and this
// DLL; the engine's own when there is none). A mod that writes the angle in memory without either setter is not
// seen: a summary with no callers says so.
//
// Watching costs one atomic load per setter call while nothing is watched. Hooks are installed at an entry only
// when its first instructions decode with no relative operand (otherwise the setter is left alone and the log
// says so).
namespace AnimationInjector::HeadingWatch {

void Install();

// Watch one actor for one paired start; the scene is the key. A second Watch of the same scene replaces it.
void Watch(std::uintptr_t scene, std::uint32_t actorFormID);
// Stop watching and return the summary line ("" when nothing turned it but Animation Injector).
std::string Stop(std::uintptr_t scene);

// While alive, setter calls on this thread are Animation Injector's own (HoldHeading's SetAngle).
class OwnTurn {
public:
    OwnTurn();
    ~OwnTurn();
    OwnTurn(const OwnTurn&) = delete;
    OwnTurn& operator=(const OwnTurn&) = delete;
};

}  // namespace AnimationInjector::HeadingWatch
