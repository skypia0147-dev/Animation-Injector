#pragma once
#include "PCH.h"
#include "runtime/QteSpec.h"

namespace AnimationInjector::Qte {
// QTEs on different channels (QteSpec::channel) run, draw and take input independently.
inline constexpr std::size_t kChannels = 4;
struct View {
    bool visible = false;
    std::uint64_t session = 0;
    QteStyle spec;
    QteClock clock;
};
bool Enabled();
void Begin(RE::ActorHandle actor, int action, RE::ActorHandle partner = {}, float clockOrigin = 0);
// QTEs an event starts (an <EventRegistry> event's, 0.12.248): the player only, asked at once, no action to
// belong to, so a ragdoll does not end them (the Ragdoll event's QTE is asked while the player flies). `source`
// names them in logs and keeps one set per event: the event again while they are open is the same start.
// Main thread. False when not the player.
bool BeginEvent(RE::ActorHandle actor, const std::vector<QteSpec>& specs, const std::string& source);
// The registry has event QTEs (EventRegistry::Build): Enabled() is true without any action QTE.
void NoteEventQtes();
// Ordinary runners have no partner; only the owning pair can retire paired QTEs.
void End(RE::ActorHandle actor, int action, RE::ActorHandle partner = {});
void ObserveTime(RE::ActorHandle actor, int action, float time);
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
// Returns ownership for this real event. Claimed down/held/up events stay paired.
bool Input(const RE::ButtonEvent& event, bool allow);
void DisconnectInput();
void Update(float delta);
void ResetOnLoad();
View ReadView(std::size_t channel = 0);
// Every channel's view; the QTE menu draws each visible one.
std::array<View, kChannels> ReadViews();
}
