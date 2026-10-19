#pragma once
#include <cstdint>
#include <cmath>
#include <utility>
#include <type_traits>

namespace RE {
inline unsigned actorQueries = 0, cameraQueries = 0;
struct NiAVObject {};
template<class T>
struct NiPointer {
    T* value = nullptr;
    NiPointer() = default;
    explicit NiPointer(T* p) : value(p) {}
    T* get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
class Actor;
struct ActorHandle {
    Actor* value = nullptr;
    NiPointer<Actor> get() const {
        ++actorQueries;
        return NiPointer<Actor>{value};
    }
    bool operator==(const ActorHandle&) const = default;
};
class Actor {
public:
    bool loaded = true, dead = false, ragdoll = false, player = false;
    NiAVObject* scene = nullptr;
    bool Is3DLoaded() const {
        ++actorQueries;
        return loaded;
    }
    bool IsDead() const {
        ++actorQueries;
        return dead;
    }
    bool IsInRagdollState() const {
        ++actorQueries;
        return ragdoll;
    }
    bool IsPlayerRef() const {
        ++actorQueries;
        return player;
    }
    NiAVObject* Get3D(bool) const {
        ++actorQueries;
        return scene;
    }
    ActorHandle GetHandle() {
        ++actorQueries;
        return {this};
    }
    float GetAngleZ() const {
        ++actorQueries;
        return 1.f;
    }
};
namespace CameraStates {
enum : unsigned { kThirdPerson, kOther };
}
struct ThirdPersonState {
    bool freeRotationEnabled = false, toggleAnimCam = false;
    unsigned id = CameraStates::kThirdPerson;
    struct {
        float x = 2.f, y = .5f;
    } freeRotation;
};
class PlayerCamera {
public:
    NiPointer<ThirdPersonState> currentState;
    ActorHandle cameraTarget;
    static inline PlayerCamera* instance = nullptr;
    static PlayerCamera* GetSingleton() {
        ++cameraQueries;
        return instance;
    }
};
inline constexpr std::uintptr_t VTABLE_ThirdPersonState[] = {1};
}
namespace Test {
using CameraFn = void (*)(RE::ThirdPersonState*, bool);
inline CameraFn cameraVfunc = nullptr;
inline unsigned installs = 0;
}
namespace REL {
struct Module {
    static bool IsVR() { return false; }
};
template<class T>
struct Relocation {
    using Value = std::decay_t<T>;
    Value value{};
    Relocation() = default;
    explicit Relocation(Value v) : value(v) {}
    Relocation& operator=(Value v) {
        value = v;
        return *this;
    }
    template<class Fn>
    Fn write_vfunc(unsigned, Fn next) {
        ++Test::installs;
        const auto previous = Test::cameraVfunc;
        Test::cameraVfunc = next;
        return previous;
    }
    template<class... Args>
    auto operator()(Args&&... args) const {
        return value(std::forward<Args>(args)...);
    }
    std::uintptr_t address() const { return reinterpret_cast<std::uintptr_t>(value); }
};
}
namespace logger {
template<class... Args>
void info(const char*, Args&&...) {}
template<class... Args>
void debug(const char*, Args&&...) {}
}
