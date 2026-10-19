#include "PCH.h"
#include "runtime/PrecisionLink.h"
#include "compat/PrecisionAPI.h"
#include "runtime/ExternalHolds.h"

namespace AnimationInjector::PrecisionLink {
namespace {
// Requested on first use, as the API header recommends (PostLoad is unreliable for some users).
PRECISION_API::IVPrecision3* Precision() {
    static auto* api = []() -> PRECISION_API::IVPrecision3* {
        auto* api = static_cast<PRECISION_API::IVPrecision3*>(
            PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V3));
        logger::info("[Precision] {}", api ? "API V3 acquired; a carried target is taken out of Precision"
                                           : "not installed (or no API V3); carries do not touch it");
        return api;
    }();
    return api;
}
ExternalHolds<RE::ActorHandle> holds;
std::uint32_t FormId(RE::ActorHandle handle) {
    const auto actor = handle.get();
    return actor ? actor->GetFormID() : 0;
}
bool Toggle(RE::ActorHandle actor, bool disable) {
    auto* api = Precision();
    return api && api->ToggleDisableActor(actor, disable);
}
}

void Hold(RE::ActorHandle target) {
    if (!target || !Precision()) return;
    const bool held = holds.Hold(target, Toggle);
    logger::info("[Precision] target {:08X} {}", FormId(target),
        held ? "taken out for the carry" : "already out of Precision (not ours to give back)");
}
void Release(RE::ActorHandle target) {
    if (target && holds.Release(target, Toggle)) logger::info("[Precision] target {:08X} given back", FormId(target));
}
void ReleaseAll() {
    if (holds.Count()) logger::info("[Precision] {} carried targets given back at load", holds.Count());
    holds.ReleaseAll(Toggle);
}
}
