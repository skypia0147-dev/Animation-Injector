#include "PCH.h"
#include "core/InputDispatchHook.h"
#include "runtime/InputSink.h"
#include "runtime/Qte.h"
#include "runtime/Carry.h"
#include "runtime/EventSuppression.h"
#include "runtime/ActionRegistry.h"

namespace AnimationInjector::InputDispatchHook {
namespace {
using Result = RE::BSEventNotifyControl;
using Source = RE::BSTEventSource<RE::InputEvent*>;
struct Dispatch {
    static Result Thunk(RE::BSTEventSink<RE::InputEvent*>* self, RE::InputEvent* const* events, Source* source) {
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
                                                            Carry::ClaimInput(*ev->AsButtonEvent(), allow));
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
    if (!EventSuppression::Enabled() && !Qte::Enabled() && !Carry::Enabled() && !Actions::UsesGameControls())
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
