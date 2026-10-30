#include "PCH.h"
#include "runtime/TdmMovement.h"
#include "compat/TrueDirectionalMovementAPI.h"
#include <atomic>

namespace AnimationInjector::TdmMovement {
namespace {
std::atomic<TDM_API::IVTDM3*> g_api{nullptr};
}

void Load() {
    if (!REX::W32::GetModuleHandleW(L"TrueDirectionalMovement.dll")) {
        logger::info("[TDM] movement condition: the keys (TDM not loaded)");
        return;
    }
    // An older TDM answers null for V3, and the keys decide.
    auto* api = static_cast<TDM_API::IVTDM3*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V3));
    g_api = api;
    logger::info("[TDM] movement condition: {}", api ? "follows TDM's mode" : "the keys (TDM has no API v3)");
}

Mode Current() {
    auto* api = g_api.load();
    if (!api) return Mode::kAbsent;
    switch (api->GetDirectionalMovementMode()) {
    case TDM_API::DirectionalMovementMode::kDisabled: return Mode::kDisabled;
    case TDM_API::DirectionalMovementMode::kVanillaStyle: return Mode::kVanillaStyle;
    case TDM_API::DirectionalMovementMode::kDirectional: return Mode::kDirectional;
    case TDM_API::DirectionalMovementMode::kTargetLock: return Mode::kTargetLock;
    }
    return Mode::kAbsent;
}
}  // namespace AnimationInjector::TdmMovement
