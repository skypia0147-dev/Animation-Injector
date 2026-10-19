#include "PCH.h"
#include "core/AnimationPreloader.h"

namespace AnimationInjector::AnimationPreloader {
namespace {
class CellEvents final : public RE::BSTEventSink<RE::BGSActorCellEvent> {
    RE::BSEventNotifyControl ProcessEvent(
        const RE::BGSActorCellEvent* event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override {
        if (event && event->flags.get() == RE::BGSActorCellEvent::CellFlag::kEnter) RequestCheck();
        return RE::BSEventNotifyControl::kContinue;
    }
};
CellEvents cellEvents;
}
void InstallCellEvents() {
    static bool installed = false;
    if (installed) return;
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        player->AsBGSActorCellEventSource()->AddEventSink(&cellEvents);
        installed = true;
    }
}
}
