#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace logger {
template<class... T>
void info(T&&...) {}
template<class... T>
void warn(T&&...) {}
template<class... T>
void debug(T&&...) {}
}
namespace RE {
using BSFixedString = std::string;
struct Actor;
extern std::array<std::shared_ptr<Actor>, 4> actors;
struct ActorHandle {
    unsigned id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const ActorHandle&) const = default;
    std::shared_ptr<Actor> get() const { return actors[id]; }
};
struct Actor {
    unsigned id = 0;
    bool loaded = true, dead = false, ragdoll = false;
    bool Is3DLoaded() const { return loaded; }
    bool IsDead() const { return dead; }
    bool IsInRagdollState() const { return ragdoll; }
    ActorHandle GetHandle() const { return {id}; }
    unsigned GetFormID() const { return id; }
    void NotifyAnimationGraph(const BSFixedString& event);
};
struct PlayerCharacter {
    static Actor* GetSingleton() { return actors[1].get(); }
};
enum class INPUT_DEVICE { kKeyboard, kMouse, kGamepad };
struct Device {
    INPUT_DEVICE value = INPUT_DEVICE::kKeyboard;
    INPUT_DEVICE get() const { return value; }
};
struct ButtonEvent {
    Device device;
    unsigned code = 18;
    int phase = 0;
    unsigned GetIDCode() const { return code; }
    bool IsDown() const { return phase == 0; }
    bool IsUp() const { return phase == 2; }
};
}
