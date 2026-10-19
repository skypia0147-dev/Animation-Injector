#include "PCH.h"
#include "core/FullBodyEntry.h"
#include "core/PairEntry.h"
#include "core/PlayerHooks.h"
#include "runtime/EventEffects.h"
#include "runtime/ActionRegistry.h"
#include "runtime/PairFeatures.h"
#include "runtime/ActionLinks.h"
#include "runtime/EventSuppression.h"
#include "runtime/EventRegistry.h"

namespace AnimationInjector::PlayerHooks {

namespace {
// Who sent a trigger event into the graph: the return addresses above the hook as
// module+offset (an engine offset maps to its Address Library id offline).
__declspec(noinline) std::string CallerChain() {
    void* frames[10]{};
    const auto count = RtlCaptureStackBackTrace(2, 10, frames, nullptr);  // skip this and the thunk
    std::string chain;
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char path[MAX_PATH]{};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(frames[i]), &module) ||
            !GetModuleFileNameA(module, path, MAX_PATH)) {
            chain += " ?";
            continue;
        }
        const char* name = std::strrchr(path, '\\');
        chain += fmt::format(" {}+{:X}", name ? name + 1 : path,
            reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module));
    }
    return chain;
}

// IAnimationGraphManagerHolder::NotifyAnimationGraph — vtable[3] 0x1 (OAR 과 같은 자리). HyperArmor/IFrame 구간이면 stagger 이벤트 차단
template<class T>
struct Notify {
    static bool thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event) {
        auto* actor =
            static_cast<RE::Actor*>(a_this);  // Actor 의 IAnimationGraphManagerHolder 부분객체 → 정적 캐스트로 되돌림
        // Pair diagnostics (engine events around paired scenes); every event of every actor passes here.
        if (spdlog::should_log(spdlog::level::debug) && a_event.c_str() &&
            (std::strncmp(a_event.c_str(), "pa_", 3) == 0 || std::strstr(a_event.c_str(), "Paired") ||
                std::strstr(a_event.c_str(), "AI_") || std::strstr(a_event.c_str(), "Hug")))
            logger::debug("[Notify] 액터 {:08X} ← '{}'", actor->GetFormID(), a_event.c_str());
        if (a_event.c_str() && (Actions::HyperArmorFor(actor) || EventEffects::HyperArmorFor(actor->GetHandle()))) {
            static RE::BSFixedString stagger{"staggerStart"}, recoilLarge{"recoilLargeStart"}, recoil{"recoilStart"};
            if (a_event == stagger || a_event == recoilLarge || a_event == recoil) {
                logger::debug("[HyperArmor] '{}' 차단 (액터 {:08X})", a_event.c_str(), actor->GetFormID());
                return true;
            }
        }
        if (EventSuppression::Consume(actor, a_event)) return true;
        if (Actions::CaptureNativeCancel(actor, a_event)) return true;
        // Snapshot an event-bound partner before downstream graph handlers can change the combat target.
        // The graph's answer cannot tell a real SprintStop from the one Papyrus
        // DisablePlayerControls sends to a standing player (both are refused); the sprint flag
        // captured here can, and the start flow reads it through its 'sprinting' test.
        const auto captured = Actions::CaptureEventTrigger(actor, a_event);
        // A full-body or paired entry event first sends its shared root entry (core/FullBodyEntry,
        // core/PairEntry); an event is at most one of them.
        const auto fullBody = FullBodyEntry::BeforeNotify(actor, a_event);
        const bool entry = fullBody.sent ? fullBody.accepted : PairEntry::BeforeNotify(actor, a_event).accepted;
        const bool accepted = func(a_this, a_event) || entry;
        const bool queued = Actions::CommitEventTrigger(captured);
        if (!captured.empty() && spdlog::should_log(spdlog::level::debug))
            logger::debug("[TriggerSource] sent actor={:08X} event='{}' graphAccepted={} queued={} sprinting={} from{}",
                actor->GetFormID(), a_event.c_str(), accepted, queued, captured.front().sprinting, CallerChain());
        ActionLinks::ObserveEvent(actor->GetHandle(), a_event);
        // An event QTE hears the request too (the engine's Ragdoll is only ever sent in).
        EventRegistry::ObserveSent(actor, a_event);
        return queued || accepted;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
}

void Install() {
    REL::Relocation<std::uintptr_t> pc3{RE::VTABLE_PlayerCharacter[3]};
    REL::Relocation<std::uintptr_t> npc3{RE::VTABLE_Character[3]};
    Notify<RE::PlayerCharacter>::func = pc3.write_vfunc(0x1, Notify<RE::PlayerCharacter>::thunk);
    Notify<RE::Character>::func = npc3.write_vfunc(0x1, Notify<RE::Character>::thunk);
    logger::info("[Actor] hooks installed (NotifyAnimationGraph) for PlayerCharacter + Character");
}

}  // namespace AnimationInjector::PlayerHooks
