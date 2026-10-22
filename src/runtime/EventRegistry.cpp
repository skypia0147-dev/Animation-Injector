#include "runtime/EventRegistry.h"
#include "runtime/EventEffects.h"
#include "runtime/EventRegistryMerge.h"
#include "runtime/Qte.h"
#include <mutex>

namespace AnimationInjector::EventRegistry {
namespace {
struct Event {
    RE::BSFixedString name;
    std::vector<EventFunctionSpec> functions;
    std::vector<QteSpec> qtes;  // started for the player when the event is sent into or out of its graph
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
std::vector<Delayed> delayed;  // main thread

}

void Build(const std::vector<EventRuleSpec>& rules) {
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
        for (const auto& f : e.functions) {
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
