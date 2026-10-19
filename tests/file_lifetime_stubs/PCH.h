#pragma once
#include <cstdint>
#include <utility>
namespace logger {
template<class... T>
void debug(const char*, T&&...) {}
template<class... T>
void info(const char*, T&&...) {}
}
namespace RE {
struct hkbCharacter {};
struct hkbContext {
    hkbCharacter* character;
};
struct hkbClipGenerator {
    std::uint16_t animationBindingIndex;
    std::uint64_t userData;
};
struct BSSynchronizedClipGenerator {
    hkbClipGenerator* clipGenerator;
};
struct AnimationFileManagerSingleton {};
inline constexpr std::uintptr_t VTABLE_hkbClipGenerator[] = {1};
inline constexpr std::uintptr_t VTABLE_BSSynchronizedClipGenerator[] = {2};
inline constexpr std::uintptr_t VTABLE_AnimationFileManagerSingleton[] = {3};
}
namespace REL {
template<class T>
struct Relocation {
    T* function = nullptr;
    Relocation() = default;
    explicit Relocation(std::uintptr_t) {}
    template<class F>
    std::uintptr_t write_vfunc(int, F) {
        return 0;
    }
    Relocation& operator=(T* value) {
        function = value;
        return *this;
    }
    Relocation& operator=(std::uintptr_t value) {
        function = reinterpret_cast<T*>(value);
        return *this;
    }
    template<class... A>
    auto operator()(A&&... args) {
        return function(std::forward<A>(args)...);
    }
};
}
