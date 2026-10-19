#include "PCH.h"
#include "core/AnimationFileLifetime.h"

namespace AnimationInjector::AnimationFileLifetime {
namespace {
Identity Key(const RE::hkbContext& context, RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
    return {reinterpret_cast<std::uintptr_t>(context.character), reinterpret_cast<std::uintptr_t>(clip),
        reinterpret_cast<std::uintptr_t>(sync)};
}
struct ClipDeactivate {
    static void thunk(RE::hkbClipGenerator* clip, const RE::hkbContext& context) {
        ExitScope scope(Key(context, clip, nullptr), clip->animationBindingIndex, clip->userData != 0);
        original(clip, context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct SyncDeactivate {
    static void thunk(RE::BSSynchronizedClipGenerator* sync, const RE::hkbContext& context) {
        auto* clip = sync->clipGenerator;
        ExitScope scope(
            Key(context, clip, sync), clip ? clip->animationBindingIndex : 0xFFFF, clip && clip->userData != 0);
        original(sync, context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct FileUnload {
    static std::int32_t thunk(RE::AnimationFileManagerSingleton* manager, const RE::hkbContext& context,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
        const auto key = Key(context, clip, sync);
        const auto owned = ExitScope::Find(key);
        // File loaders may pass synthetic clips/contexts. Inspect a clip only
        // when it belongs to the current real Deactivate callback.
        if (!owned || !clip) return original(manager, context, clip, sync);
        if (*owned != clip->animationBindingIndex)
            logger::debug(
                "[AnimationFileLifetime] matched release character={:X} clip={:X} paired={} restoredIndex={} playbackIndex={}",
                key.character, key.clip, sync != nullptr, clip->animationBindingIndex, *owned);
        return AnimationFileLifetime::Unload(
            key, clip->animationBindingIndex, [&] { return original(manager, context, clip, sync); });
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void Install() {
    static bool installed = false;
    if (installed) return;
    // DataLoaded wraps the existing OAR/engine Deactivate chain. The outer
    // scope outlives the inner clip cleanup and the native paired Unload.
    REL::Relocation<std::uintptr_t> clip{RE::VTABLE_hkbClipGenerator[0]};
    REL::Relocation<std::uintptr_t> sync{RE::VTABLE_BSSynchronizedClipGenerator[0]};
    REL::Relocation<std::uintptr_t> manager{RE::VTABLE_AnimationFileManagerSingleton[0]};
    ClipDeactivate::original = clip.write_vfunc(7, ClipDeactivate::thunk);
    SyncDeactivate::original = sync.write_vfunc(7, SyncDeactivate::thunk);
    FileUnload::original = manager.write_vfunc(3, FileUnload::thunk);
    installed = true;
    logger::info(
        "[AnimationFileLifetime] global ordinary/paired release ownership installed; OAR optional, no polling");
}
}
