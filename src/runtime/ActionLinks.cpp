#include "runtime/ActionLinks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/PairedSession.h"
#include "core/PairRootRuntime.h"
#include "runtime/HeldKeys.h"
#include <mutex>

namespace AnimationInjector::ActionLinks {
namespace {
std::mutex mutex;
struct RouteEvent {
    int target;
    RE::BSFixedString enter, air;
};
struct Window {
    std::vector<RouteEvent> routes;
    int target = -1, partnerTarget = -1;
    ActionLinkSpec spec;
    RE::BSFixedString event, open, close, enter, airEnter;
    bool opened = false, closed = false, pressed = false;
    // on="move" (10-31): a movement press kept for the window (buffered), and the direction it goes with.
    int moveBuffered = -1, moveDirection = -1;
};
struct Entry {
    RE::ActorHandle actor;
    int action;
    bool paired, player;
    float time = 0, duration = 0;
    // Charge (10-30): the runner reached its Exit point (an on="end" transition goes), or a loop's maxHold made its
    // key count as released (an on="release" one goes, an NPC's too).
    bool ended = false, released = false;
    int moveSeen = -1;  // the player's movement direction last frame (a new press is a change to a direction)
    std::vector<Window> windows;
    RE::ActorHandle partner;
};
struct Pending {
    RE::ActorHandle actor;
    int source, target;
    bool paired;
    PairedSession::Ticket ticket;
    float age = 0;
    RE::ActorHandle partner;
    int partnerTarget = -1;
    RE::BSFixedString enter, airEnter;
    bool player = false;
    std::uint32_t key = 0, gamepad = 0;
    GameControl control = GameControl::kNone;
    RE::BSFixedString event;  // an event transition's event, claimed while its destination waits (ClaimsEvent)
    int flow = -1;
    bool explicitBlend = false;
    // Charge: an on="end" transition, or a forced release, whose destination cannot start leaves its source as an
    // exit would (it has no other end).
    bool exitOnFail = false;
    bool onMove = false;
    ConditionValues::Request conditionRequest;
    std::vector<RouteEvent> routes;
};
std::vector<Entry> entries;
std::vector<Pending> pending;
std::vector<Pending> requested;  // RequestHandoff: started on the next Update like a pressed window
const RE::BSFixedString& Synced() {
    static RE::BSFixedString name("bIsSynced");
    return name;
}
const RE::BSFixedString& Driven() {
    static RE::BSFixedString name("bAnimationDriven");
    return name;
}
// Whether an on="release" transition's key is still down (runtime/HeldKeys.h): its own key, pad button or game
// control, else the key that started the player's action. No key at all counts as released.
bool KeyHeld(const ActionLinkSpec& spec) {
    using HeldKeys::Source;
    auto& held = HeldKeys::Get();
    if (spec.key || spec.gamepad || spec.control != GameControl::kNone)
        return (spec.key && held.Held({Source::kKey, spec.key})) ||
               (spec.gamepad && held.Held({Source::kPad, spec.gamepad})) ||
               (spec.control != GameControl::kNone &&
                   held.Held({Source::kControl, static_cast<std::uint32_t>(spec.control)}));
    const auto chain = held.Chain();
    return chain && held.Held(*chain);
}
}
void Begin(RE::ActorHandle actor, int action, bool paired, bool player, RE::ActorHandle partner) {
    std::lock_guard lock(mutex);
    std::erase_if(entries, [&](const Entry& e) { return e.actor == actor && e.action == action; });
    const auto& links = Actions::Spec(action).links;
    if (links.empty()) return;
    Entry entry{actor, action, paired, player};
    // A movement key already down is held, not pressed: it goes as a buffered press would (on="move").
    const int direction = HeldKeys::Get().MoveDirection();
    entry.moveSeen = direction >= 0 && direction < 4 ? direction : -1;
    entry.partner = paired ? partner : RE::ActorHandle{};
    for (const auto& spec : links) {
        if (spec.flowParent >= 0) continue;
        Window w;
        w.spec = spec;
        w.enter = RE::BSFixedString(spec.enterEvent.c_str());
        w.airEnter = RE::BSFixedString(spec.airEnterEvent.c_str());
        w.target = Actions::IndexOf(spec.action);
        if (w.target < 0) {
            logger::warn("[Connect] '{}' missing target '{}'", Actions::Spec(action).id, spec.action);
            continue;
        }
        if (spec.blend >= 0 &&
            (paired || Actions::Spec(action).clips.size() != 1 || Actions::Spec(w.target).paired.enabled ||
                Actions::Spec(w.target).clips.size() != 1)) {
            logger::warn("[Connect] explicit blend requires distinct single ordinary actions; link ignored");
            continue;
        }
        if (!spec.partnerAction.empty()) {
            w.partnerTarget = Actions::IndexOf(spec.partnerAction);
            if (!paired || w.partnerTarget < 0 || Actions::Spec(w.target).paired.enabled ||
                Actions::Spec(w.partnerTarget).paired.enabled) {
                logger::warn("[Connect] '{}' partnerTo requires two valid ordinary destination actions",
                    Actions::Spec(action).id);
                continue;
            }
        }
        for (const auto& route : links)
            if (route.flowParent == static_cast<int>(&spec - links.data()))
                if (const int i = Actions::IndexOf(route.action); i >= 0)
                    w.routes.push_back({i, RE::BSFixedString(route.enterEvent.c_str()),
                        RE::BSFixedString(route.airEnterEvent.c_str())});
        w.event = RE::BSFixedString(spec.event.c_str());
        w.open = RE::BSFixedString(spec.openEvent.c_str());
        w.close = RE::BSFixedString(spec.closeEvent.c_str());
        w.opened = spec.openEvent.empty() && spec.openAt <= 0;
        entry.windows.push_back(std::move(w));
    }
    entries.push_back(std::move(entry));
}
bool RequestHandoff(RE::ActorHandle actor, int target, int partnerTarget) {
    if (target < 0 && partnerTarget < 0) return false;
    const auto status = PairedSession::ReadStatus();
    if (status.phase == PairedSession::Phase::kIdle) return false;
    RE::ActorHandle other;
    if (status.initiator.actor == actor)
        other = status.target.actor;
    else if (status.target.actor == actor)
        other = status.initiator.actor;
    else
        return false;
    // Only the partner moves on: it is the one leaving for an action, the other one released.
    if (target < 0) {
        std::swap(actor, other);
        std::swap(target, partnerTarget);
    }
    std::lock_guard lock(mutex);
    // The pair's Connect entry when it has one. A pair without connections has none (0.12.265, Grapple's suplex:
    // the request was refused and the player's action was turned away by the running pair); the session names
    // the action then.
    const auto e = std::find_if(entries.begin(), entries.end(),
        [&](const Entry& e) { return e.paired && (e.actor == actor || e.actor == other); });
    const int source = e != entries.end() ? e->action : status.action;
    if (source < 0) return false;
    Pending item{actor, source, target, true};
    item.partnerTarget = partnerTarget;
    item.player = e != entries.end() && e->player;
    requested.push_back(item);
    return true;
}
void End(RE::ActorHandle actor, int action) {
    std::lock_guard lock(mutex);
    std::erase_if(entries, [&](const Entry& e) { return e.actor == actor && e.action == action; });
}
void ObserveTime(RE::ActorHandle actor, int action, float time, float duration) {
    if (!std::isfinite(time) || time < 0) return;
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        if (e.actor == actor && e.action == action) {
            // A loop's clip time went back to its start (moving charge, 10-30): its windows open and close again.
            if (time + 1e-4f < e.time)
                for (auto& w : e.windows) {
                    w.closed = w.pressed = false;
                    w.opened = w.spec.openEvent.empty() && w.spec.openAt <= 0;
                    w.moveBuffered = -1;
                }
            e.time = time;
            if (std::isfinite(duration) && duration > 0) e.duration = duration;
        }
}
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event) {
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        if (e.actor == actor || (actor && e.paired && e.partner == actor))
            for (auto& w : e.windows) {
                if (!w.spec.closeEvent.empty() && event == w.close) {
                    w.closed = true;
                    w.pressed = false;
                }
                if (!w.spec.openEvent.empty() && event == w.open && !w.closed) w.opened = true;
                if (!w.spec.event.empty() && event == w.event && !w.closed && (w.opened || w.spec.buffered))
                    w.pressed = true;
            }
}
bool ClaimsEvent(RE::ActorHandle actor, const RE::BSFixedString& event) {
    if (!actor || event.empty()) return false;
    std::lock_guard lock(mutex);
    for (const auto& e : entries)
        if (e.actor == actor || (e.paired && e.partner == actor))
            for (const auto& w : e.windows)
                if (!w.spec.event.empty() && event == w.event) return true;
    return std::any_of(pending.begin(), pending.end(),
        [&](const Pending& p) { return p.actor == actor && !p.event.empty() && event == p.event; });
}
InputResult Input(std::uint32_t code, bool gamepad, GameControl control, bool down) {
    std::lock_guard lock(mutex);
    InputResult result;
    if (!code && control == GameControl::kNone) return result;
    const auto matches = [&](std::uint32_t key, std::uint32_t pad, GameControl own) {
        return own != GameControl::kNone ? own == control : code && (gamepad ? pad : key) == code;
    };
    for (auto& e : entries)
        if (e.player)
            for (auto& w : e.windows) {
                // An end or release transition's key is not pressed into it (runtime/HeldKeys.h reads it).
                if (w.spec.onEnd || w.spec.onRelease || w.spec.onMove ||
                    !matches(w.spec.key, w.spec.gamepad, w.spec.control))
                    continue;
                // A closed window rejects the press; it must not turn into a new start.
                result.claimed = true;
                if (down && !w.closed && (w.opened || w.spec.buffered)) {
                    w.pressed = true;
                    result.taken = true;
                }
            }
    // Source cleanup can finish before a queued destination becomes ready.
    for (const auto& p : pending)
        if (p.player && matches(p.key, p.gamepad, p.control)) result.claimed = true;
    return result;
}
void Update(float delta) {
    if (!std::isfinite(delta) || delta < 0) return;
    std::vector<Pending> starts;
    std::vector<Pending> waiting;
    {
        std::lock_guard lock(mutex);
        for (auto& e : entries) {
            if (std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return p.actor == e.actor; }))
                continue;
            // The player's movement direction now (MoveDir 0..3), -1 with no movement key (MoveDir::kNone 4).
            const int direction = HeldKeys::Get().MoveDirection();
            const int moving = direction >= 0 && direction < 4 ? direction : -1;
            for (auto& w : e.windows) {
                const float close = w.spec.closeRelativeToEnd
                                        ? (e.duration > 0 ? std::max(0.0f, e.duration - w.spec.closeAt) : -1.0f)
                                        : w.spec.closeAt;
                if (w.spec.closeAt >= 0 && close >= 0 && e.time >= close) {
                    w.closed = true;
                    w.pressed = false;
                }
                if (w.spec.openAt >= 0 && e.time >= w.spec.openAt && !w.closed) w.opened = true;
                // Charge (10-30): the end reached, or the key up while the window is open. "Up" is a state, not a
                // moment: a key let go before the window opened goes the moment it opens.
                if (w.opened && !w.closed && !w.pressed &&
                    ((w.spec.onEnd && e.ended) || (w.spec.onRelease && (e.released || (e.player && !KeyHeld(w.spec))))))
                    w.pressed = true;
                // on="move" (user request 10-31): a movement key's press as a key transition takes its key's, buffered
                // before the window as a key is; a key held as the window opens counts as buffered too. Its card
                // reads the direction held now, else the one pressed (a tap may be up by then). Only in a charge: while
                // the key that started the action is down (user decision 10-31; a dodge chains on its own key).
                if (w.spec.onMove && e.player && !w.closed && !w.pressed && !HeldKeys::Get().ChainHeld())
                    w.moveBuffered = -1;
                else if (w.spec.onMove && e.player && !w.closed && !w.pressed) {
                    if (moving >= 0 && moving != e.moveSeen && (w.opened || w.spec.buffered)) w.moveBuffered = moving;
                    const int held = moving >= 0 && w.spec.buffered ? moving : -1;
                    if (w.opened && (w.moveBuffered >= 0 || held >= 0)) {
                        w.moveDirection = held >= 0 ? held : w.moveBuffered;
                        w.moveBuffered = -1;
                        w.pressed = true;
                    }
                }
                if (w.opened && !w.closed && w.pressed) {
                    Pending item{e.actor, e.action, w.target, e.paired};
                    item.exitOnFail = w.spec.onEnd || (w.spec.onRelease && e.released);
                    item.onMove = w.spec.onMove;
                    if (w.spec.onMove) item.conditionRequest.CaptureMovement(w.moveDirection);
                    item.partnerTarget = w.partnerTarget;
                    item.player = e.player;
                    item.key = w.spec.key;
                    item.gamepad = w.spec.gamepad;
                    item.control = w.spec.control;
                    item.event = w.event;
                    item.enter = w.enter;
                    item.airEnter = w.airEnter;
                    item.routes = w.routes;
                    item.explicitBlend = w.spec.blend >= 0;
                    starts.push_back(item);
                    w.pressed = false;
                    break;
                }
            }
            e.moveSeen = moving;
        }
        waiting.swap(pending);
        starts.insert(starts.end(), requested.begin(), requested.end());
        requested.clear();
    }
    for (auto item : starts)
        if (auto actor = item.actor.get(); actor && actor->Is3DLoaded()) {
            // An end or forced-release transition that does not go ends its source (runtime/HeldKeys.h).
            bool handled = false;
            struct ExitOnFail {
                const Pending& item;
                RE::Actor* actor;
                const bool& handled;
                ~ExitOnFail() {
                    if (!item.exitOnFail || handled) return;
                    if (auto* source = Actions::RunnerFor(actor, item.source, false)) source->ExitForLink();
                    logger::debug("[Connect] '{}' end/release transition did not go; source exits",
                        Actions::Spec(item.source).id);
                }
            } exitOnFail{item, actor.get(), handled};
            if (item.paired) {
                if (!PairedSession::TryGetTicket(item.actor, item.ticket)) continue;
                const auto participants = PairedSession::ReadStatus();
                item.partner = participants.initiator.actor == item.actor ? participants.target.actor
                                                                          : participants.initiator.actor;
                if (item.partnerTarget >= 0) {
                    const auto status = PairedSession::ReadStatus();
                    if (!(status.ticket == item.ticket)) continue;
                    // Capture the participant of this ticket; never search for a new target.
                    if (status.initiator.actor == item.actor)
                        item.partner = status.target.actor;
                    else if (status.target.actor == item.actor)
                        item.partner = status.initiator.actor;
                    else
                        continue;
                    auto partner = item.partner.get();
                    item.partnerTarget = Actions::ResolveFlow(partner.get(), item.partnerTarget, item.actor);
                    if (item.partnerTarget < 0 || Actions::Spec(item.partnerTarget).paired.enabled) continue;
                    auto* destination = partner && partner.get() != actor.get()
                                            ? Actions::RunnerFor(partner.get(), item.partnerTarget, true)
                                            : nullptr;
                    if (!destination || !destination->CanRequestLink()) {
                        logger::debug("[Connect] partnerTo='{}' preflight=false; source continues",
                            Actions::Spec(item.partnerTarget).id);
                        continue;
                    }
                }
            }
            item.flow = item.target;
            item.partner = Actions::CaptureTarget(actor.get(), item.target, item.partner);
            const int selected =
                Actions::ResolveFlow(actor.get(), item.target, item.partner, false, &item.conditionRequest);
            if (selected < 0) continue;
            if (!item.routes.empty()) {
                const auto route = std::find_if(
                    item.routes.begin(), item.routes.end(), [&](const auto& r) { return r.target == selected; });
                if (route != item.routes.end()) {
                    item.enter = route->enter;
                    item.airEnter = route->air;
                    item.target = selected;
                } else if (!item.explicitBlend && Actions::Spec(selected).paired.enabled) {
                    item.enter = {};
                    item.airEnter = {};
                    item.target = selected;
                } else
                    continue;
            }
            if (selected != item.target) {
                // Custom transition events were injected for the original destination only.
                if (item.enter != RE::BSFixedString{} || item.airEnter != RE::BSFixedString{}) continue;
                item.target = selected;
            }
            // The same action again (10-31): its clip restarts through the graph's self-transition. A loop moving the
            // same way plays on (a moving charge keeps its cycle).
            if (!item.paired && item.target == item.source) {
                auto* runner = Actions::RunnerFor(actor.get(), item.source, false);
                if (item.onMove && Actions::Spec(item.source).loop.enabled)
                    handled = true;
                else
                    handled = runner && runner->RestartLinked(item.enter, item.airEnter);
                continue;
            }
            auto* destination = Actions::RunnerFor(actor.get(), item.target, true);
            if (Actions::Spec(item.target).paired.enabled &&
                (item.partnerTarget >= 0 || !destination || !destination->CanRequestPairLink(item.partner)))
                continue;
            if (!destination || !destination->CanRequestLink()) {
                logger::debug("[Connect] target='{}' preflight=false; source continues", Actions::Spec(item.target).id);
                continue;
            }
            if (!item.paired && !Actions::Spec(item.target).paired.enabled &&
                Actions::Spec(item.source).clips.size() == 1 && Actions::Spec(item.target).clips.size() == 1) {
                auto* source = Actions::RunnerFor(actor.get(), item.source, false);
                const bool accepted = source && destination->RequestLinked(*source, item.enter, item.airEnter);
                logger::debug("[DirectLink] actor={:08X} from={} to={} requested={}", actor->GetFormID(), item.source,
                    item.target, accepted);
                handled = accepted;
                continue;
            }
            {
                std::lock_guard lock(mutex);
                for (auto& entry : entries)
                    if (entry.actor == item.actor && entry.action == item.source)
                        for (auto& window : entry.windows) {
                            window.closed = true;
                            window.pressed = false;
                        }
            }
            // Paired -> ordinary links transfer in this update. The previous sync
            // clips can finish later, but must not delay or restore over new runners.
            if (item.paired && !Actions::Spec(item.target).paired.enabled) {
                auto partner = item.partner.get();
                auto* partnerRunner = item.partnerTarget >= 0 && partner
                                          ? Actions::RunnerFor(partner.get(), item.partnerTarget, true)
                                          : nullptr;
                if (item.partnerTarget >= 0 && (!partnerRunner || !partnerRunner->CanRequestLink())) continue;
                if (!PairedSession::BeginOrdinaryHandoff(item.ticket, item.actor, item.target, item.partnerTarget))
                    continue;
                // Permission is scoped to these exact destinations and closed even
                // when the second request fails after the first has entered.
                const bool conditions = destination->CanRequest() && (!partnerRunner || partnerRunner->CanRequest());
                const bool partnerAccepted = conditions && partnerRunner && partnerRunner->Request();
                const bool actorAccepted = conditions && (!partnerRunner || partnerAccepted) && destination->Request();
                if (partnerAccepted && !actorAccepted) partnerRunner->ExitForLink();
                PairedSession::CompleteOrdinaryHandoff(item.ticket, item.actor, actorAccepted, partnerAccepted);
                handled = actorAccepted;
                logger::debug(
                    "[Connect] immediate pair->ordinary actor={:08X} to='{}' accepted={} partnerTo={} accepted={} idleWait=0",
                    actor->GetFormID(), Actions::Spec(item.target).id, actorAccepted, item.partnerTarget,
                    partnerAccepted);
                continue;
            }
            bool accepted = false;
            if (item.paired) {
                accepted = PairedSession::TryGetTicket(item.actor, item.ticket);
                if (accepted) PairedSession::Cancel(item.ticket);
            } else if (auto* runner = Actions::RunnerFor(actor.get(), item.source, false))
                accepted = runner->ExitForLink();
            logger::debug("[Connect] actor={:08X} '{}' -> '{}' transitionRequested={}", actor->GetFormID(),
                Actions::Spec(item.source).id, Actions::Spec(item.target).id, accepted);
            handled = accepted;
            if (accepted) waiting.push_back(item);
        }
    std::vector<Pending> remaining;
    for (auto item : waiting) {
        item.age += delta;
        auto actor = item.actor.get();
        if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() || item.age > 8) continue;
        bool ready = false;
        if (item.paired) {
            const auto status = PairedSession::ReadStatus();
            if (!(status.ticket == item.ticket)) continue;
            if (status.phase == PairedSession::Phase::kIdle) {
                if (status.lastEnd != PairedSession::EndReason::kCancelled || !status.initiator.graphStateRestored ||
                    !status.target.graphStateRestored)
                    continue;
                ready = !PairRootRuntime::Busy(item.ticket);
            }
        } else {
            auto* source = Actions::RunnerFor(actor.get(), item.source, false);
            bool synced = false, driven = false;
            ready = (!source || source->GetState() == ActionRunner::State::kIdle) &&
                    actor->GetGraphVariableBool(Synced(), synced) && !synced &&
                    actor->GetGraphVariableBool(Driven(), driven) && !driven;
        }
        if (!ready) {
            remaining.push_back(item);
            continue;
        }
        if (item.flow >= 0) {
            const int selected =
                Actions::ResolveFlow(actor.get(), item.flow, item.partner, false, &item.conditionRequest);
            if (selected < 0) {
                logger::debug("[Connect] recheck actor={:08X} flow='{}' result=stop age={:.3f}", actor->GetFormID(),
                    Actions::Spec(item.flow).id, item.age);
                continue;
            }
            if (selected != item.target) {
                // Bound graph transition events and paired ordinary handoffs
                // cannot be retargeted after their source has been released.
                if (item.partnerTarget >= 0 || item.enter != RE::BSFixedString{} ||
                    item.airEnter != RE::BSFixedString{})
                    continue;
                logger::debug("[Connect] recheck actor={:08X} previous='{}' selected='{}' partner={:08X} age={:.3f}",
                    actor->GetFormID(), Actions::Spec(item.target).id, Actions::Spec(selected).id,
                    item.partner.get() ? item.partner.get()->GetFormID() : 0, item.age);
                item.target = selected;
            }
        }
        auto* runner = Actions::RunnerFor(actor.get(), item.target, true);
        const bool accepted = runner && (runner->Spec().paired.enabled ? (runner->ValidPairedTarget(item.partner) &&
                                                                             runner->RequestPaired(item.partner))
                                                                       : runner->Request());
        logger::debug("[Connect] target='{}' accepted={} transitionTime={:.3f}", Actions::Spec(item.target).id,
            accepted, item.age);
    }
    {
        std::lock_guard lock(mutex);
        pending.insert(pending.end(), remaining.begin(), remaining.end());
    }
}
bool ReachedEnd(RE::ActorHandle actor, int action) {
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        if (e.actor == actor && e.action == action &&
            std::any_of(
                e.windows.begin(), e.windows.end(), [](const Window& w) { return w.spec.onEnd && !w.closed; })) {
            e.ended = true;
            return true;
        }
    return false;
}
bool ForceRelease(RE::ActorHandle actor, int action) {
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        if (e.actor == actor && e.action == action &&
            std::any_of(
                e.windows.begin(), e.windows.end(), [](const Window& w) { return w.spec.onRelease && !w.closed; })) {
            e.released = true;
            return true;
        }
    return false;
}
void ResetOnLoad() {
    std::lock_guard lock(mutex);
    entries.clear();
    pending.clear();
    requested.clear();
}
void Cancel(RE::ActorHandle actor) {
    if (!actor) return;
    std::lock_guard lock(mutex);
    std::erase_if(entries, [&](const Entry& e) { return e.actor == actor || (e.paired && e.partner == actor); });
    std::erase_if(pending, [&](const Pending& p) { return p.actor == actor || p.partner == actor; });
    std::erase_if(requested, [&](const Pending& p) { return p.actor == actor || p.partner == actor; });
}
bool HasPendingTransition(RE::ActorHandle actor) {
    std::lock_guard lock(mutex);
    return std::any_of(pending.begin(), pending.end(), [&](const Pending& item) {
        return item.actor == actor ||
               (item.partner == actor && (item.partnerTarget >= 0 || Actions::Spec(item.target).paired.enabled));
    });
}

}
