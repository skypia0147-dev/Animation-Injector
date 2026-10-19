#include "PCH.h"
#include "core/PairInputGate.h"
#include "runtime/PairFeatures.h"
#include <RE/F/FixedStrings.h>

namespace AnimationInjector::PairInputGate {
namespace {
using Query = bool (*)(void*, const RE::ActorHandle*);
Query original = nullptr;
bool motionInstalled = false;

bool QueryMotionSync(const RE::IAnimationGraphManagerHolder* holder, const RE::BSFixedString& name, bool& value) {
    // Preserve dynamic dispatch and any earlier vtable hooks. Only this native
    // call site sees the override; the graph, scene and other readers do not.
    const bool readable = holder->GetGraphVariableImpl3(name, value);
    static const RE::BSFixedString synced("bIsSynced");
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (readable && value && name == synced && player &&
        holder == static_cast<const RE::IAnimationGraphManagerHolder*>(player) &&
        PairFeatures::AllowRecoveryMotion(player->GetHandle()))
        value = false;
    return readable;
}

void InstallMotion() {
    if (motionInstalled || REL::Module::IsVR()) return;
    // The ID pair is also used by TDM's AIProcess_SetRotationSpeedZ hook.
    const auto begin = REL::RelocationID(36365, 37356).address();
    DWORD64 imageBase = 0;
    const auto* function = ::RtlLookupFunctionEntry(begin, &imageBase, nullptr);
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    const auto end = function ? imageBase + function->EndAddress : 0;
    if (!function || imageBase + function->BeginAddress != begin || begin < text.address() ||
        end > text.address() + text.size()) {
        logger::error("[PairMoveMotion] movement function range unavailable; original motion retained");
        return;
    }
    const auto site = FindMotionCall(begin, end, REL::RelocationID(11308, 11437).address(),
        static_cast<std::uint32_t>(offsetof(RE::FixedStrings, bIsSynced)));
    if (!site) {
        logger::error("[PairMoveMotion] unique sync movement gate not found; original motion retained");
        return;
    }
    // The resolver verified the complete seven-byte indirect virtual call.
    // Replace the whole instruction, including its unused trailing byte.
    REL::safe_fill(site, 0x90, 7);
    SKSE::GetTrampoline().write_call<6>(site, QueryMotionSync);
    motionInstalled = true;
    logger::info("[PairMoveMotion] accepted-exit-only native movement gate installed; sync scene unchanged");
}

bool QuerySync(void* manager, const RE::ActorHandle* actor) {
    // Do not call TDM's sync-camera fallback for this accepted movement exit:
    // normal PlayerControls now processes the real events, including look input.
    // Every other query retains the previous engine/plugin chain unchanged.
    if (actor && PairFeatures::AllowRecoveryInput(*actor)) return false;
    return original(manager, actor);
}
}
void Install() {
    InstallMotion();
    if (original || REL::Module::IsVR()) return;
    const auto begin = REL::RelocationID(41288, 42338).address();
    DWORD64 imageBase = 0;
    const auto* function = ::RtlLookupFunctionEntry(begin, &imageBase, nullptr);
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    const auto end = function ? imageBase + function->EndAddress : 0;
    if (!function || imageBase + function->BeginAddress != begin || begin < text.address() ||
        end > text.address() + text.size()) {
        logger::error("[PairMoveInput] input function range unavailable; original sync gate retained");
        return;
    }
    // OAR Offsets.h documents the same native synchronized-animation singleton.
    const auto global = REL::RelocationID(514960, 401100).address();
    const auto site = FindCall(begin, end, global);
    if (!site) {
        logger::error("[PairMoveInput] unique sync input gate not found; original controls retained");
        return;
    }
    original = reinterpret_cast<Query>(SKSE::GetTrampoline().write_call<5>(site, QuerySync));
    logger::info("[PairMoveInput] accepted-exit-only sync input gate installed; prior chain retained");
}
}
