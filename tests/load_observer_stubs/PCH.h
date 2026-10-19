#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace logger {
template<class... T>
void trace(const char*, T&&...) {}
template<class... T>
void info(const char*, T&&...) {}
template<class... T>
void debug(const char*, T&&...) {}
}
namespace REL {
inline std::array<std::uintptr_t, 4> testVtable{};
template<class T>
struct Relocation {
    std::uintptr_t address = 0;
    Relocation() = default;
    explicit Relocation(std::uintptr_t value) : address(value) {}
    template<class F>
    std::uintptr_t write_vfunc(int index, F hook) {
        return std::exchange(testVtable.at(index), reinterpret_cast<std::uintptr_t>(hook));
    }
    Relocation& operator=(std::uintptr_t value) {
        address = value;
        return *this;
    }
    template<class... A>
    decltype(auto) operator()(A&&... args) const {
        assert(address);
        return reinterpret_cast<T*>(address)(std::forward<A>(args)...);
    }
};
}
namespace RE {
inline constexpr std::uintptr_t VTABLE_hkbClipGenerator[] = {1};
inline constexpr std::uintptr_t VTABLE_AnimationFileManagerSingleton[] = {2};
struct String {
    const char* value = nullptr;
    const char* c_str() const { return value; }
};
struct hkbClipGenerator {
    std::uintptr_t vtable = VTABLE_hkbClipGenerator[0];
    String name{"AI_GrappleStart_Clip"};
    void* animationControl = nullptr;
};
struct hkbCharacter {};
struct hkbContext {
    hkbCharacter* character = nullptr;
};
struct BSSynchronizedClipGenerator {};
struct AnimationFileManagerSingleton {};
}
namespace AnimationInjector::NodeFactory {
inline bool SameVTable(const void* object, const std::uintptr_t* vtable) {
    return object && *static_cast<const std::uintptr_t*>(object) == *vtable;
}
}
namespace AnimationInjector::Actions {
struct SpecType {
    struct {
        bool enabled = false;
    } paired;
};
inline const SpecType& Spec(int action) {
    static const SpecType normal{}, pair{{true}};
    return action == 1 ? pair : normal;
}
inline bool FindClip(const char* name, int& action, int& variant) {
    if (!name) return false;
    const std::string_view text{name};
    if (text != "AI_GrappleStart_Clip" && text != "AI_Pair_Clip") return false;
    action = text == "AI_Pair_Clip" ? 1 : 0;
    variant = 0;
    return true;
}
}
