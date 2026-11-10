#include "PCH.h"
#include "core/InputDispatchHook.h"
#include "runtime/InputSink.h"
#include "core/GameInputReplay.h"
#include "runtime/Qte.h"
#include "runtime/Carry.h"
#include "runtime/EventSuppression.h"
#include "runtime/ActionRegistry.h"
#include "runtime/GameControlBindings.h"

namespace AnimationInjector::InputDispatchHook {
namespace {
using Result = RE::BSEventNotifyControl;
using Source = RE::BSTEventSource<RE::InputEvent*>;

// Activate on a horse or furniture while a player action plays is kept from the game, press to release
// (poisoned0cean 11-10, reproduced in a sprint stop): the engine starts the mount or the sit and waits for its
// animation, but the third-person graph sits in the action's state and never takes HorseEnter (the first-person
// graph does), so the player is left waiting to mount for good, with movement and most keys gone until a reload.
// Other activation (items, doors, talking) goes through as before.
bool activateKept = false;  // input thread: the kept press's release is kept too
bool KeepActivate(const RE::ButtonEvent& event, bool allow) {
    if (GameControls::Of(event.QUserEvent()) != GameControl::kActivate) return false;
    if (activateKept) {
        if (event.IsUp()) activateKept = false;
        return true;
    }
    if (!allow || !event.IsDown() || !Actions::AnyActive()) return false;
    auto* pick = RE::CrosshairPickData::GetSingleton();
    const auto target = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>();
    if (!target) return false;
    const auto* base = target->GetBaseObject();
    const bool furniture = base && base->Is(RE::FormType::Furniture);
    if (!furniture && !target->IsHorse()) return false;
    activateKept = true;
    logger::info("[ActivateGate] activate on {} {:08X} ignored: a player action is playing",
        furniture ? "furniture" : "horse", target->GetFormID());
    return true;
}
struct Dispatch {
    static Result Thunk(RE::BSTEventSink<RE::InputEvent*>* self, RE::InputEvent* const* events, Source* source) {
        // A press handed to the game late (core/GameInputReplay.h) is the game's: nothing here takes it.
        if (GameInputReplay::Replaying()) return original(self, events, source);
        EventSuppression::BeginInput();
        struct Scope {
            ~Scope() { EventSuppression::EndInput(); }
        } scope;
        auto* ui = RE::UI::GetSingleton();
        const bool allow = ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::Console::MENU_NAME);
        RE::InputEvent* head = events ? *events : nullptr;
        // The links as they came, to put back after the sinks: one list per thread kept between dispatches, so an
        // input frame does not allocate (2026-10-28 review R-26). A dispatch inside a dispatch takes its own.
        using Links = std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>>;
        static thread_local Links spare;
        Links saved = std::move(spare);
        saved.clear();
        RE::InputEvent** tail = &head;
        for (auto* ev = head; ev;) {
            auto* next = ev->next;
            saved.emplace_back(ev, next);
            if (ev->eventType == RE::INPUT_EVENT_TYPE::kDeviceConnect &&
                !static_cast<RE::DeviceConnectEvent*>(ev)->connected)
                Qte::DisconnectInput();
            // An open QTE takes its key first; an active carry keeps its putdown key from the game.
            const bool consume = ev->AsButtonEvent() && (Qte::Input(*ev->AsButtonEvent(), allow) ||
                                                            Carry::ClaimInput(*ev->AsButtonEvent(), allow) ||
                                                            KeepActivate(*ev->AsButtonEvent(), allow));
            if (consume)
                *tail = next;
            else
                tail = &ev->next;
            ev = next;
        }
        struct Restore {
            Links& links;
            ~Restore() {
                for (auto [event, next] : links) event->next = next;
                spare = std::move(links);
            }
        } restore{saved};
        auto* sink = InputSink::Get();
        sink->ProcessEvent(&head, source);
        // A game control's press that started one of ours (and its release) is kept from the game (0.12.292).
        if (sink->AnyTaken())
            for (RE::InputEvent** link = &head; *link;)
                if (sink->Taken(*link))
                    *link = (*link)->next;
                else
                    link = &(*link)->next;
        return original(self, &head, source);
    }
    static inline REL::Relocation<decltype(Thunk)> original;
};
}
bool InstallIfNeeded() {
    // Any action needs it: activation on a horse or furniture is kept from the game while one plays.
    if (!EventSuppression::Enabled() && !Qte::Enabled() && !Carry::Enabled() && !Actions::UsesGameControls() &&
        Actions::Count() == 0)
        return false;
    static bool installed = false;
    if (!installed) {
        // PlayerControls inherits BSTEventSink<InputEvent*> as its first base.
        // Slot 1 is ProcessEvent; preserve the existing TDM/engine handler chain.
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_PlayerControls[0]};
        Dispatch::original = table.write_vfunc(1, Dispatch::Thunk);
        installed = true;
        logger::info("[Suppress] ordered PlayerControls input dispatch installed");
    }
    return true;
}
}
