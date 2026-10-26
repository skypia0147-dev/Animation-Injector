#include "PCH.h"
#include "core/HitDispatch.h"
#include "runtime/PairFeatures.h"
#include "runtime/Carry.h"
#include "runtime/Strike.h"

namespace AnimationInjector::HitDispatch {

namespace {
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
