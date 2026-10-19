#include "PCH.h"
#include "runtime/QteScreenFx.h"
#include "ui/QteConverge.h"

namespace AnimationInjector::QteScreenFx {
namespace {
// The colour drain: the screen's image space as the miss found it, and our drained copy of it, lent to the engine as
// its override while it runs. The copy is always made from that first look, never from what the engine shows while
// the override is ours, so the drain cannot compound frame after frame.
RE::ImageSpaceBaseData base{};
RE::ImageSpaceBaseData drained{};
bool draining = false;
float drainTime = 0;
void GiveBack() {
    if (auto* manager = RE::ImageSpaceManager::GetSingleton()) {
        auto& override = manager->GetOverrideBaseData();
        if (override == &drained) override = nullptr;
    }
    draining = false;
}
}
void Result(QteOutcome outcome, bool reducedMotion) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) return;
    if (!reducedMotion) RE::ShakeCamera(static_cast<float>(QteConverge::Shake(outcome)), player->GetPosition(), .3f);
    if (outcome != QteOutcome::Failure) return;
    auto* manager = RE::ImageSpaceManager::GetSingleton();
    if (!manager) return;
    // Another override (a mod, a scene) keeps the screen: no drain over it.
    const auto override = manager->GetOverrideBaseData();
    if (override && override != &drained) return;
    if (!draining) {
        const auto* current = manager->GetCurrentBaseData();
        if (!current) return;
        base = *current;
    }
    draining = true;
    drainTime = 0;
}
void Update(float delta) {
    if (!draining || !std::isfinite(delta) || delta < 0) return;
    drainTime += delta;
    auto* manager = RE::ImageSpaceManager::GetSingleton();
    if (!manager || drainTime >= QteConverge::kDesatSeconds) {
        GiveBack();
        return;
    }
    auto& override = manager->GetOverrideBaseData();
    if (override && override != &drained) {
        // Someone else took the screen meanwhile: let it be.
        draining = false;
        return;
    }
    // The screen as the miss found it, less saturated and a little darker (the mockup's grey, 60% bright copy).
    const double d = QteConverge::Desat(drainTime);
    drained = base;
    drained.cinematic.saturation = static_cast<float>(base.cinematic.saturation * (1 - d));
    drained.cinematic.brightness = static_cast<float>(base.cinematic.brightness * (1 - .45 * d));
    override = &drained;
}
void ResetOnLoad() { GiveBack(); }
}
