#include "PCH.h"
#include "core/HitDispatch.h"
#include "runtime/PairFeatures.h"
#include "runtime/Carry.h"
#include "runtime/Strike.h"
#include "runtime/HitHistory.h"

namespace AnimationInjector::HitDispatch {

namespace {
// The hitReceived condition's record (runtime/HitHistory.h): weapon hits only, so an enchantment's own hit event
// (no power attack flag) does not write over its weapon's.
void Remember(const RE::TESHitEvent& ev) {
    auto* victim = ev.target->As<RE::Actor>();
    auto* source = ev.source ? RE::TESForm::LookupByID(ev.source) : nullptr;
    if (!victim || !source || !source->Is(RE::FormType::Weapon)) return;
    using Flag = RE::TESHitEvent::Flag;
    const HitHistory::Hit hit{ev.flags.any(Flag::kPowerAttack), ev.flags.any(Flag::kBashAttack),
        ev.flags.any(Flag::kSneakAttack), ev.flags.any(Flag::kHitBlocked)};
    HitHistory::Record(victim->GetHandle(), hit);
    logger::debug("[HitReceived] actor={:08X} by={:08X} power={} bash={} sneak={} blocked={}", victim->GetFormID(),
        ev.cause ? ev.cause->GetFormID() : 0, hit.power, hit.bash, hit.sneak, hit.blocked);
}
class HitSink : public RE::BSTEventSink<RE::TESHitEvent> {
public:
    static HitSink* Get() {
        static HitSink s;
        return &s;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_ev, RE::BSTEventSource<RE::TESHitEvent>*) override {
        // A strike's own hit event (Strike::Sending) is the action's: it neither ends a carry nor is a third party's.
        if (Strike::Sending()) return RE::BSEventNotifyControl::kContinue;
        if (a_ev && a_ev->target) Carry::ObserveHit(a_ev->target->As<RE::Actor>());
        if (a_ev && a_ev->target && a_ev->cause) {
            auto* victim = a_ev->target->As<RE::Actor>();
            auto* attacker = a_ev->cause->As<RE::Actor>();
            if (victim && attacker) PairFeatures::ObserveHit(victim->GetHandle(), attacker->GetHandle());
        }
        if (a_ev && a_ev->target && HitHistory::Needed()) Remember(*a_ev);
        return RE::BSEventNotifyControl::kContinue;
    }
};
}

void Register() {
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink<RE::TESHitEvent>(HitSink::Get());
        logger::info("[Hit] TESHitEvent sink registered");
    }
}

}  // namespace AnimationInjector::HitDispatch
