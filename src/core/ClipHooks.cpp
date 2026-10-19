#include "core/PairRootRuntime.h"
#include "core/NativePairAlignment.h"
#include "core/NativePairEnd.h"
#include "compat/OpenAnimationReplacerAPI-Animations.h"
#include "PCH.h"
#include "runtime/PairFeatures.h"
#include "core/ClipHooks.h"
#include "core/NodeFactory.h"
#include "core/GraphInjector.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "Settings.h"
#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "RE/B/BShkbAnimationGraph.h"
#include <mutex>

namespace AnimationInjector::ClipHooks {

// The context's character is the graph's own characterInstance member; step back to the graph.
constexpr std::size_t characterInstanceOffset = offsetof(RE::BShkbAnimationGraph, characterInstance);
static_assert(characterInstanceOffset == 0xC0, "BShkbAnimationGraph::characterInstance moved");
RE::Actor* ActorForContext(const RE::hkbContext& a_ctx) {
    if (!a_ctx.character) return nullptr;
    auto* graph = reinterpret_cast<RE::BShkbAnimationGraph*>(
        reinterpret_cast<std::byte*>(a_ctx.character) - characterInstanceOffset);
    return graph->holder;
}

namespace {
void DumpAnnotations(RE::hkbClipGenerator* a_this, const char* a_when) {
    static std::mutex lock;  // clip hooks can run on several animation threads
    static std::unordered_set<std::string> dumped;
    const char* nm = a_this->name.c_str();
    if (!FrameworkSettings::Get().debug || !nm) return;
    RE::hkaAnimation* anim = nullptr;
    if (auto* ac = a_this->animationControl.get(); ac && ac->binding && ac->binding->animation.get())
        anim = ac->binding->animation.get();
    if (!anim) return;  // 바인딩이 올 때까지 기다린다 (Activate 시점엔 대개 없음)
    {
        std::lock_guard guard(lock);
        if (!dumped.insert(nm).second) return;
    }
    const int nTrig = a_this->triggers.get() ? a_this->triggers->triggers._size : -1;
    logger::debug("[Clip] '{}' ({}) 길이 {:.3f}s 트리거 {}개 주석 트랙 {}개", nm, a_when, anim->duration, nTrig,
        anim->annotationTracks._size);
    for (int t = 0; t < anim->annotationTracks._size; ++t) {
        const auto& tr = anim->annotationTracks._data[t];
        std::string list;
        for (int k = 0; k < tr.annotations._size && k < 24; ++k) {
            const auto& an = tr.annotations._data[k];
            list += fmt::format(" [{:.3f} '{}']", an.time, an.text.c_str() ? an.text.c_str() : "");
        }
        logger::debug("[Clip]   track '{}' {}개:{}", tr.trackName.c_str() ? tr.trackName.c_str() : "",
            tr.annotations._size, list);
    }
}

struct Activate {
    static void thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_ctx) {
        int action = -1, variant = -1;
        const bool ours = Actions::FindClip(a_this->name.c_str(), action, variant);
        // OAR selects synchronized replacements before the internal clip's
        // Activate. Paired base indices were registered before graph linking;
        // rebinding here would overwrite OAR's already selected replacement.
        if (ours && !Actions::Spec(action).paired.enabled && a_ctx.character) {
            const char* want = a_this->animationName.c_str();
            const int idx = NodeFactory::EnsureAnimationName(a_ctx.character, want);
            // OAR can deduplicate identical bytes across differently named paths.
            // Query the active selection instead of inferring ownership from a path.
            static auto* oar = []() -> OAR_API::Animations::IAnimationsInterface* {
                auto module = GetModuleHandleW(L"OpenAnimationReplacer.dll");
                if (!module) return nullptr;
                auto request = reinterpret_cast<OAR_API::Animations::_RequestPluginAPI_Animations>(
                    GetProcAddress(module, "RequestPluginAPI_Animations"));
                if (!request) return nullptr;
                const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
                return request(
                    OAR_API::Animations::InterfaceVersion::V1, plugin->GetName().data(), plugin->GetVersion());
            }();
            const auto info =
                oar ? oar->GetCurrentReplacementAnimationInfo(a_this) : OAR_API::Animations::ReplacementAnimationInfo{};
            const char* selected = info.animationPath.c_str();
            const int selectedIndex = a_this->animationBindingIndex;
            const bool replacement = selected && *selected;
            if (replacement) {
                logger::debug("[Clip] '{}' preserving replacement index={} path='{}'", a_this->name.c_str(),
                    selectedIndex, selected);
            } else if (idx >= 0 && idx != a_this->animationBindingIndex) {
                logger::debug(
                    "[Clip] '{}' bindingIndex {} → {}", a_this->name.c_str(), a_this->animationBindingIndex, idx);
                a_this->animationBindingIndex = static_cast<std::uint16_t>(idx);
            } else if (idx < 0)
                logger::warn("[Clip] '{}' 애니 이름을 animationNames 에 못 넣음", a_this->name.c_str());
        }
        func(a_this, a_ctx);
        if (!ours || Actions::Spec(action).paired.enabled) return;
        float dur = 0.0f;
        bool moves = false;
        if (auto* ac = a_this->animationControl.get(); ac && ac->binding && ac->binding->animation.get()) {
            const auto* animation = ac->binding->animation.get();
            dur = animation->duration;
            // Whether the HKX moves the actor: AMR reads "animmotion x y z" annotations on any track.
            for (const auto& track : animation->annotationTracks)
                for (const auto& note : track.annotations)
                    if (const char* text = note.text.c_str(); text && std::strncmp(text, "animmotion ", 11) == 0)
                        moves = true;
        }
        // Adopts an external entry and updates the runner, now or on the main thread's next update.
        Actions::ClipActivated(ActorForContext(a_ctx), action, variant, dur, moves);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// Baseline observation uses participant identities captured under the graph lock.
void ObservePair(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context, bool exiting) {
    if (node) NativePairAlignment::Observe(*node, context, exiting);
    PairedSession::Ticket ticket;
    RE::ActorHandle actor;
    if (!node) return;
    if (!PairedSession::TryGetParticipant(context, actor, ticket)) {
        // A pair the AI session does not run (a vanilla kill move, another mod's paired idle).
        NativePairEnd::ObserveClip(ActorForContext(context), context, *node, exiting);
        return;
    }
    PairRootRuntime::ObserveClip(ticket, actor, context, *node, exiting);
    Carry::ObserveClip(ticket, actor, context, *node, exiting);
    PairFeatures::ObserveSync(ticket, actor, *node, exiting);
    if (exiting)
        PairedSession::ObserveSyncDeactivate(ticket, actor, context, *node);
    else
        PairedSession::ObserveSyncActivate(ticket, actor, context, *node);
}
struct SyncActivate {
    static void thunk(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context) {
        func(node, context);
        ObservePair(node, context, false);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct SyncUpdate {
    static void thunk(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context, float delta) {
        func(node, context, delta);
        ObservePair(node, context, false);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct SyncDeactivate {
    static void thunk(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context) {
        ObservePair(node, context, false);
        ObservePair(node, context, true);
        func(node, context);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct Deactivate {
    static void thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_ctx) {
        func(a_this, a_ctx);
        int action = -1, variant = -1;
        if (!Actions::FindClip(a_this->name.c_str(), action, variant)) return;
        if (Actions::Spec(action).paired.enabled) return;
        DumpAnnotations(a_this, "비활성");
        Actions::ClipDeactivated(ActorForContext(a_ctx), action, variant);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

}

void Install() {
    REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_hkbClipGenerator[0]};
    Activate::func = vtbl.write_vfunc(0x4, Activate::thunk);
    Deactivate::func = vtbl.write_vfunc(0x7, Deactivate::thunk);
    REL::Relocation<std::uintptr_t> svt{RE::VTABLE_BSSynchronizedClipGenerator[0]};
    SyncActivate::func = svt.write_vfunc(0x4, SyncActivate::thunk);
    SyncDeactivate::func = svt.write_vfunc(0x7, SyncDeactivate::thunk);
    SyncUpdate::func = svt.write_vfunc(0x5, SyncUpdate::thunk);
    logger::debug("[Clip] hooks installed (hkbClipGenerator Activate/Deactivate)");
}

}  // namespace AnimationInjector::ClipHooks
