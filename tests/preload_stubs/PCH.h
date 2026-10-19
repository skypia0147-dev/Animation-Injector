#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cassert>
#include "runtime/PairedRig.h"
#include "runtime/ViewSpec.h"

namespace logger {
template<class... T>
void trace(const char*, T&&...) {}
template<class... T>
void debug(const char*, T&&...) {}
template<class... T>
void info(const char*, T&&...) {}
template<class... T>
void warn(const char*, T&&...) {}
template<class... T>
void error(const char*, T&&...) {}
}
namespace REL {
template<class T>
struct Relocation {
    Relocation() = default;
    explicit Relocation(std::uintptr_t) {}
    template<class F>
    std::uintptr_t write_vfunc(int, F) {
        return 0;
    }
    Relocation& operator=(std::uintptr_t) { return *this; }
    template<class... A>
    void operator()(A&&...) {}
};
}
namespace RE {
struct BSResourceNiBinaryStream {  // the engine's file lookup: nothing is found here
    explicit BSResourceNiBinaryStream(const std::string&) {}
    bool good() const { return false; }
};
struct UI {
    inline static bool paused = false;
    static UI* GetSingleton() {
        static UI value;
        return &value;
    }
    bool GameIsPaused() { return paused; }
};
template<class T>
struct Ref {
    T* value = nullptr;
    T* get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
template<class T>
struct Array {
    int _size = 0;
    T* _data = nullptr;
};
struct hkaAnimation {};
struct hkaAnimationBinding {
    Ref<hkaAnimation> animation;
};
struct hkbAnimationBindingWithTriggers {
    Ref<hkaAnimationBinding> binding;
    bool readable = true;
};
struct hkbAnimationBindingSet {
    Array<hkbAnimationBindingWithTriggers*> bindings;
};
struct Strings {
    std::string name = "DefaultMale";
    std::vector<std::string> animationNames;
};
struct Data {
    Ref<Strings> stringData;
};
struct Setup {
    Ref<Data> data;
    Ref<hkbAnimationBindingSet> animationBindingSet;
};
struct hkbBehaviorGraph {};
struct hkbCharacter {
    Ref<hkbBehaviorGraph> behaviorGraph;
    Ref<Setup> setup;
    Ref<hkbAnimationBindingSet> animationBindingSet;
};
struct hkbContext {
    hkbCharacter* character = nullptr;
};
struct hkbClipGenerator {
    std::string animationName;
    std::uint16_t animationBindingIndex = 0;
    std::uint64_t userData = 0;
    Ref<int> animationControl;
};
inline constexpr std::uintptr_t VTABLE_hkbBehaviorGraph[] = {1};
inline constexpr std::uintptr_t VTABLE_hkbAnimationBindingWithTriggers[] = {2};
struct AnimationFileManagerSingleton {
    static inline AnimationFileManagerSingleton* instance = nullptr;
    static auto* GetSingleton() { return instance; }
    // The engine's own queue (the members the preloader reads): the file being read, the requests behind it.
    struct AnimationFileInfo {
        std::uint32_t crc32Filename = 0;
        char extension[4]{};
        std::uint32_t crc32Path = 0;
    };
    struct LoadedAnimation {
        void* unk00 = nullptr;
        AnimationFileInfo fileInfo;
        std::uint16_t counter = 0;
    };
    LoadedAnimation currentAnimation;
    std::vector<int> queuedAnimations;
    virtual int Queue(const hkbContext&, hkbClipGenerator*, void*) = 0;
    virtual bool Load(const hkbContext&, hkbClipGenerator*, void*) = 0;
    virtual int Unload(const hkbContext&, hkbClipGenerator*, void*) = 0;
};
}
namespace AnimationInjector {
struct FrameworkSettings {
    bool injectState = true;
    bool preloadOrdinary = true;
    int preloadOrdinaryBudgetMB = 0;
    static FrameworkSettings Get() { return {}; }
};
namespace Actions {
struct Clip {
    std::string file, clipName;
};
struct SpecType {
    struct {
        bool enabled = true;
        PairedRig::Kind targetRig = PairedRig::Kind::kCharacter;
    } paired;
    std::vector<Clip> clips;
    ViewSpec view;
};
inline std::vector<SpecType> specs;
inline int Count() { return static_cast<int>(specs.size()); }
inline const SpecType& Spec(int index) { return specs.at(index); }
}
namespace NodeFactory {
inline int residencyChecks = 0;
inline std::vector<std::unique_ptr<RE::hkbClipGenerator>> clips;
inline auto* MakeClip(const char*, const char*, std::uint16_t index, void*, void*, bool) {
    clips.push_back(std::make_unique<RE::hkbClipGenerator>());
    clips.back()->animationBindingIndex = index;
    return clips.back().get();
}
inline int FindName(const std::vector<std::string>& names, const char* name) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}
// The plugin's cached lookup gives the same first match.
inline int FindAnimationName(const std::vector<std::string>& names, const char* name) { return FindName(names, name); }
inline bool SameVTable(RE::hkbAnimationBindingWithTriggers* slot, const std::uintptr_t*) {
    ++residencyChecks;
    return slot && slot->readable;
}
}
}
