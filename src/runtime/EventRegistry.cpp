#include "runtime/EventRegistry.h"
#include "runtime/ActionHistory.h"
#include "runtime/HitHistory.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/EventEffects.h"
#include "runtime/EventRegistryMerge.h"
#include "runtime/EventValuesMatch.h"
#include "runtime/PairedSession.h"
#include "runtime/Qte.h"
#include <mutex>
#include <set>

namespace AnimationInjector::EventRegistry {
namespace {
// An action's <EventValues> for one event, resolved against the event's functions when the registry is built.
struct ActionValues {
    int action = -1;
    std::vector<EventFunctionSpec> functions;  // the event's functions run with this action's numbers
    std::vector<bool> given;                   // which of them the action gave numbers for
};
struct Event {
    RE::BSFixedString name;
    std::vector<EventFunctionSpec> functions;
    std::vector<QteSpec> qtes;  // started for the player when the event is sent into or out of its graph
    std::vector<ActionValues> byAction;
};
// Built once and read-only afterwards; `ready` publishes it to the event threads.
std::vector<Event> events;
std::unordered_map<const char*, std::size_t> byName;  // pooled string (case-insensitive pool) -> event
std::vector<std::string> names;
std::atomic_bool ready{false};

struct Queued {
    RE::ActorHandle actor;
    std::size_t event;
    std::uint64_t epoch;
    bool sent = false;  // sent into the graph: the event's QTEs only
};
std::mutex queueMutex;
std::vector<Queued> queue;
std::atomic<std::uint64_t> epoch{0};
constexpr std::size_t kQueueLimit = 256;

struct Delayed {
    EventFunctionSpec function;
    EventEffects::Context context;
    float remaining;
    std::size_t event;  // keeps the origin name alive
};
std::vector<Delayed> delayed;                         // main thread
std::set<std::pair<std::size_t, int>> missingLogged;  // (event, action) whose numbers were missing, logged once

// Main thread. The action or pair playing on `actor`, or -1.
int PlayingAction(RE::Actor* actor) {
    int playing = -1;
    Actions::FindRunner(actor, [&](int action, ActionRunner& r) {
        if (r.GetState() == ActionRunner::State::kIdle) return false;
        playing = action;
        return true;
    });
    if (playing >= 0) return playing;
    const auto handle = actor->GetHandle();
    const auto status = PairedSession::ReadStatus();
    if (status.phase != PairedSession::Phase::kIdle &&
        (status.initiator.actor == handle || status.target.actor == handle))
        return status.action;
    return -1;
}

// Each action's <EventValues> resolved to the registry's functions by event and function index (0.12.281).
void ResolveValues(std::span<const ActionSpec> actions) {
    std::size_t resolved = 0;
    for (int a = 0; a < static_cast<int>(actions.size()); ++a)
        for (const auto& values : actions[a].eventValues) {
            const auto found = byName.find(RE::BSFixedString(values.event.c_str()).data());
            if (found == byName.end() || events[found->second].functions.empty()) {
                logger::warn("[EventRegistry] '{}' gives values for '{}', which has no registered functions",
                    actions[a].id, values.event);
                continue;
            }
            auto& e = events[found->second];
            std::vector<int> unmatched;
            const auto match = MatchValues(e.functions, values.functions, &unmatched);
            for (const int v : unmatched)
                logger::warn("[EventRegistry] '{}' gives values for op {} of '{}', which the registry does not hold",
                    actions[a].id, static_cast<int>(values.functions[v].op), values.event);
            ActionValues own{a};
            for (std::size_t i = 0; i < e.functions.size(); ++i) {
                const bool given = match[i] >= 0;
                own.functions.push_back(
                    given ? WithValues(e.functions[i], values.functions[match[i]]) : e.functions[i]);
                own.given.push_back(given);
            }
            e.byAction.push_back(std::move(own));
            ++resolved;
        }
    logger::info("[EventRegistry] {} event values of actions resolved", resolved);
}
}

void Build(const std::vector<EventRuleSpec>& rules, std::span<const ActionSpec> actions) {
    const auto merged = EventRegistryMerge::Merge(rules);
    events.clear();
    byName.clear();
    names.clear();
    std::size_t functions = 0;
    for (const auto& entry : merged.entries) {
        Event e{RE::BSFixedString(entry.name.c_str()), entry.functions, entry.qtes};
        if (!e.qtes.empty()) Qte::NoteEventQtes();
        functions += e.functions.size();
        names.push_back(entry.name);
        if (!e.functions.empty() || !e.qtes.empty()) byName.emplace(e.name.data(), events.size());
        events.push_back(std::move(e));
    }
    for (const auto& note : merged.notes) logger::warn("[EventRegistry] {}", note);
    logger::info("[EventRegistry] {} events ({} with functions, {} functions, {} equal ones merged)", events.size(),
        byName.size(), functions, merged.merged);
    ResolveValues(actions);
    ready.store(true, std::memory_order_release);
    // The first reader of the declarations may be on any thread: other plugins hear it on the main one.
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] { Announce(); });
}
const std::vector<std::string>& Names() { return names; }
std::size_t Count() { return events.size(); }

void Announce() {
    // Other plugins that handle the same annotations (FH_Grapple_Plugin) skip what is registered here:
    // the owner notice without an actor means "every actor" (the pair-scoped notice names one).
    auto* source = SKSE::GetModCallbackEventSource();
    if (!source || !ready.load(std::memory_order_acquire)) return;
    static const RE::BSFixedString notice("AnimationInjector_EventOwner");
    for (const auto& e : events)
        if (!e.functions.empty()) {
            SKSE::ModCallbackEvent event{notice, e.name, 1.0f, nullptr};
            source->SendEvent(&event);
        }
}

void Observe(RE::Actor* actor, const RE::BSFixedString& event) {
    if (!actor || !ready.load(std::memory_order_acquire) || byName.empty()) return;
    const auto found = byName.find(event.data());
    if (found == byName.end()) return;
    Queued q{actor->GetHandle(), found->second};
    q.epoch = epoch.load();
    std::lock_guard lock(queueMutex);
    if (queue.size() < kQueueLimit) queue.push_back(q);
}

void ObserveSent(RE::Actor* actor, const RE::BSFixedString& event) {
    if (!actor || !actor->IsPlayerRef() || !ready.load(std::memory_order_acquire) || byName.empty()) return;
    const auto found = byName.find(event.data());
    if (found == byName.end() || events[found->second].qtes.empty()) return;
    Queued q{actor->GetHandle(), found->second};
    q.epoch = epoch.load();
    q.sent = true;
    std::lock_guard lock(queueMutex);
    if (queue.size() < kQueueLimit) queue.push_back(q);
}

void Update(float delta) {
    if (!ready.load(std::memory_order_acquire)) return;
    if (!std::isfinite(delta) || delta < 0) delta = 0.0f;
    std::vector<Queued> arrived;
    {
        std::lock_guard lock(queueMutex);
        arrived.swap(queue);
    }
    const auto now = epoch.load();
    for (const auto& q : arrived) {
        if (q.epoch != now) continue;
        auto actor = q.actor.get();
        if (!actor) continue;
        const auto& e = events[q.event];
        // The event's QTEs (the player only; the same event again while they are open is the same start).
        if (!e.qtes.empty()) Qte::BeginEvent(q.actor, e.qtes, e.name.c_str());
        if (q.sent) continue;
        const auto context = EventEffects::ContextFor(actor.get(), e.name.c_str());
        // The numbers come from the action or pair playing on the sender (0.12.281), else the defaults.
        const int action = e.byAction.empty() ? -1 : PlayingAction(actor.get());
        const ActionValues* own = nullptr;
        for (const auto& v : e.byAction)
            if (v.action == action) own = &v;
        for (std::size_t i = 0; i < e.functions.size(); ++i) {
            const auto& f = own ? own->functions[i] : e.functions[i];
            if (NeedsValues(f.op) && !(own && own->given[i])) {
                if (missingLogged.insert({q.event, action}).second)
                    logger::warn("[EventRegistry] '{}' op {} skipped: the sender's action ({}) gives no values for it",
                        e.name.c_str(), static_cast<int>(f.op), action >= 0 ? Actions::Spec(action).id : "none");
                continue;
            }
            if (f.delay > 0.0f)
                delayed.push_back({f, context, f.delay, q.event});
            else
                EventEffects::Run(f, context);
        }
    }
    // Spells cast here can send events synchronously: take the due ones out first.
    std::vector<Delayed> due;
    for (auto& d : delayed) {
        d.remaining -= delta;
        if (d.remaining <= 0.0f) due.push_back(d);
    }
    std::erase_if(delayed, [](const Delayed& d) { return d.remaining <= 0.0f; });
    for (const auto& d : due) EventEffects::Run(d.function, d.context);
    EventEffects::Update(delta);
    ActionHistory::Advance(delta);
    HitHistory::Advance(delta);
}

void DropDelayed(RE::ActorHandle actor) {
    if (!actor) return;
    const auto before = delayed.size();
    std::erase_if(delayed, [&](const Delayed& d) { return d.context.sender == actor || d.context.partner == actor; });
    if (delayed.size() != before) logger::info("[EventRegistry] {} delayed functions dropped", before - delayed.size());
}

void ResetOnLoad() {
    ++epoch;
    {
        std::lock_guard lock(queueMutex);
        queue.clear();
    }
    delayed.clear();
}
}
