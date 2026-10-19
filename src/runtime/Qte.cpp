#include "runtime/Qte.h"
#include "runtime/ActionRegistry.h"
#include "runtime/QteServices.h"
#include "runtime/QteScreenFx.h"
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace AnimationInjector::Qte {
namespace {
std::mutex mutex;
struct Armed {
    QteSpec spec;
    RE::BSFixedString event, stop;
    std::array<RE::BSFixedString, 3> results;
    std::array<std::uint32_t, 3> sounds{};
    bool ready = false, fired = false;
};
struct Entry {
    RE::ActorHandle actor, partner;
    int action;
    std::uint64_t token;
    float time = -1;
    std::vector<Armed> nodes;
    float clockOrigin = 0;
    // An event's QTEs (BeginEvent): no action (action = -1), named by their event.
    bool standalone = false;
    std::string source;
};
struct Active {
    std::uint64_t token = 0;
    std::size_t node = 0;
};
// One challenge per channel. Channels run, draw and take input independently (QteSpec::channel).
struct Channel {
    Active active;
    View view;
    bool soundSent = false;
    std::uint64_t holdKey = 0, holdSession = 0;
};
std::array<Channel, kChannels> channels;
std::vector<Entry> entries;
std::uint64_t serial = 0;
std::uint64_t generation = 0;
std::unordered_set<std::uint64_t> held, claimed;
std::size_t ChannelOf(const QteSpec& q) { return std::min<std::size_t>(q.channel, kChannels - 1); }
std::size_t ResultIndex(const Channel& c, const QteSpec& q) {
    const auto outcome = c.view.clock.outcome;
    return outcome == QteOutcome::Failure ? 1 : outcome == QteOutcome::Perfect && !q.results[2].event.empty() ? 2 : 0;
}
// A ragdoll ends the action a QTE belongs to; an event's QTE has none and may be asked in one.
bool Available(RE::ActorHandle handle, bool ragdollAllowed = false) {
    auto actor = handle.get();
    return actor && actor->Is3DLoaded() && !actor->IsDead() && (ragdollAllowed || !actor->IsInRagdollState());
}
Armed Arm(const QteSpec& spec) {
    Armed node;
    node.spec = spec;
    node.event = RE::BSFixedString(spec.event);
    node.stop = RE::BSFixedString(spec.stopEvent);
    for (std::size_t i = 0; i < 3; ++i) {
        node.results[i] = RE::BSFixedString(spec.results[i].event);
        node.sounds[i] = QteServices::ResolveSound(spec.results[i]);
    }
    return node;
}
std::string_view Name(const Entry& e) {
    return e.standalone ? std::string_view(e.source) : std::string_view(Actions::Spec(e.action).id);
}
std::atomic_bool eventQtes{false};
void Cancel(Channel& c) {
    c.active = {};
    c.view.visible = false;
    c.holdSession = 0;
}
}
bool Enabled() {
    if (eventQtes.load()) return true;
    for (int i = 0; i < Actions::Count(); ++i)
        if (!Actions::Spec(i).qtes.empty()) return true;
    return false;
}
void Begin(RE::ActorHandle actor, int action, RE::ActorHandle partner, float clockOrigin) {
    const auto& specs = Actions::Spec(action).qtes;
    if (specs.empty()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) return;
    const auto playerHandle = player->GetHandle();
    if (actor != playerHandle && partner != playerHandle) return;
    std::lock_guard lock(mutex);
    const auto run = [&](const Entry& e) { return e.actor == actor && e.action == action && !e.standalone; };
    for (const auto& e : entries)
        if (run(e))
            for (auto& c : channels)
                if (c.active.token == e.token) Cancel(c);
    std::erase_if(entries, run);
    Entry entry{actor, partner, action, ++serial};
    entry.clockOrigin = std::isfinite(clockOrigin) ? std::max(0.0f, clockOrigin) : 0;
    for (const auto& spec : specs) {
        if ((spec.inputOwner == "self" && actor != playerHandle) ||
            (spec.inputOwner == "partner" && partner != playerHandle))
            continue;
        entry.nodes.push_back(Arm(spec));
    }
    if (!entry.nodes.empty()) {
        logger::debug("[QTE] armed action={} rules={} paired={}", Actions::Spec(action).id, entry.nodes.size(),
            static_cast<bool>(partner));
        entries.push_back(std::move(entry));
    }
}
bool BeginEvent(RE::ActorHandle actor, const std::vector<QteSpec>& specs, const std::string& source) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (specs.empty() || !player || !player->Is3DLoaded() || actor != player->GetHandle()) return false;
    std::lock_guard lock(mutex);
    for (const auto& e : entries)
        if (e.standalone && e.actor == actor && e.source == source) return true;
    Entry entry{actor, {}, -1, ++serial};
    entry.standalone = true;
    entry.source = source;
    entry.time = 0;  // asked at once (at = 0)
    for (const auto& spec : specs) entry.nodes.push_back(Arm(spec));
    logger::debug("[QTE] event '{}' armed rules={}", source, entry.nodes.size());
    entries.push_back(std::move(entry));
    return true;
}
void NoteEventQtes() { eventQtes.store(true); }
void End(RE::ActorHandle actor, int action, RE::ActorHandle partner) {
    std::lock_guard lock(mutex);
    const auto owns = [&](const Entry& e) {
        return e.actor == actor && e.action == action && e.partner == partner && !e.standalone;
    };
    for (const auto& e : entries)
        if (owns(e))
            for (auto& c : channels)
                if (c.active.token == e.token) {
                    // A dispatched transition may synchronously retire its own source.
                    if (!c.view.clock.sent)
                        Cancel(c);
                    else
                        c.active = {};
                }
    std::erase_if(entries, owns);
}
void ObserveTime(RE::ActorHandle actor, int action, float time) {
    if (!std::isfinite(time) || time < 0) return;
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        if (e.actor == actor && e.action == action && !e.standalone) e.time = std::max(0.0f, time - e.clockOrigin);
}
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event) {
    std::lock_guard lock(mutex);
    for (auto& e : entries)
        for (std::size_t i = 0; i < e.nodes.size(); ++i) {
            auto& q = e.nodes[i];
            const auto& source = q.spec.eventSource;
            const bool matches =
                (source != "partner" && actor == e.actor) || (source != "self" && actor && actor == e.partner);
            if (!matches) continue;
            if (!q.spec.stopEvent.empty() && event == q.stop) {
                q.ready = false;
                q.fired = true;
                auto& c = channels[ChannelOf(q.spec)];
                if (c.active.token == e.token && c.active.node == i) Cancel(c);
            } else if (!q.ready && !q.spec.event.empty() && event == q.event) {
                // Each time its event comes the rule asks again (0.12.262: an HKX may send the same event several
                // times), unless it is still on screen from the last time.
                auto& c = channels[ChannelOf(q.spec)];
                if (q.fired && c.active.token == e.token && c.active.node == i && c.view.visible) continue;
                q.fired = false;
                q.ready = true;
                logger::debug("[QTE] event ready id={} clipTime={:.3f}", q.spec.id, e.time);
            }
        }
}
bool Input(const RE::ButtonEvent& event, bool allow) {
    const auto device = event.device.get();
    if (device != RE::INPUT_DEVICE::kKeyboard && device != RE::INPUT_DEVICE::kMouse &&
        device != RE::INPUT_DEVICE::kGamepad)
        return false;
    const bool pad = device == RE::INPUT_DEVICE::kGamepad;
    const auto code = event.GetIDCode() + (device == RE::INPUT_DEVICE::kMouse ? 256 : 0);
    const auto key = static_cast<std::uint64_t>(code) | (pad ? 1ull << 32 : 0);
    std::lock_guard lock(mutex);
    const bool already = held.contains(key);
    if (event.IsUp()) {
        held.erase(key);
        for (auto& c : channels)
            if (c.holdSession == c.view.session && c.holdSession && c.holdKey == key) {
                if (allow && c.view.visible && c.active.token)
                    c.view.clock.Release(c.view.spec);
                else if (c.view.clock.outcome == QteOutcome::None)
                    Cancel(c);
                c.holdSession = 0;
            }
        return claimed.erase(key) != 0;
    }
    held.insert(key);
    if (claimed.contains(key)) return true;
    if (!allow || !event.IsDown() || already) return false;
    const auto matches = [&](QteKey binding) { return code && code == (pad ? binding.gamepad : binding.key); };
    // The first open challenge that uses this key takes the press; the others never see it.
    for (auto& c : channels) {
        auto& view = c.view;
        if (!view.visible || !c.active.token || view.clock.outcome != QteOutcome::None) continue;
        bool expected = true;
        if (view.spec.type == QteType::KeySequence) {
            bool relevant = false;
            for (unsigned i = 0; i < view.spec.sequenceCount; ++i) relevant |= matches(view.spec.sequence[i]);
            if (!relevant) continue;
            expected =
                view.clock.completed < view.spec.sequenceCount && matches(view.spec.sequence[view.clock.completed]);
        } else if (!matches({view.spec.key, view.spec.gamepad}) &&
                   !(!pad && view.spec.alternateKey && code == view.spec.alternateKey))
            continue;
        claimed.insert(key);
        if (view.spec.type == QteType::HoldRelease) {
            if (view.clock.holding) return true;
            c.holdKey = key;
            c.holdSession = view.session;
        }
        view.clock.Press(view.spec, expected);
        return true;
    }
    return false;
}
void DisconnectInput() {
    std::lock_guard lock(mutex);
    for (auto& c : channels)
        if (c.view.visible && !c.view.clock.sent) Cancel(c);
    held.clear();
    claimed.clear();
}
void Update(float delta) {
    if (!std::isfinite(delta) || delta < 0) return;
    struct Dispatch {
        RE::ActorHandle actor;
        RE::BSFixedString event;
    };
    std::vector<Dispatch> sends;
    struct Start {
        RE::ActorHandle actor;
        std::string to, partnerTo;
    };
    std::vector<Start> starts;  // an event QTE result's actions
    std::vector<std::uint32_t> sounds;
    std::vector<std::pair<QteOutcome, bool>> screenResults;
    std::vector<std::uint64_t> dispatchTokens;
    std::uint64_t sendGeneration = 0;
    {
        std::lock_guard lock(mutex);
        sendGeneration = generation;
        for (auto it = entries.begin(); it != entries.end();) {
            if (!Available(it->actor, it->standalone) || (it->partner && !Available(it->partner))) {
                for (auto& c : channels)
                    if (c.active.token == it->token) Cancel(c);
                it = entries.erase(it);
            } else
                ++it;
        }
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            auto& c = channels[channel];
            auto& view = c.view;
            if (!view.visible) continue;
            view.clock.Advance(delta, view.spec);
            const auto owner =
                std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.token == c.active.token; });
            if (view.clock.outcome != QteOutcome::None && !c.soundSent) {
                c.soundSent = true;
                // The converging lines' result reaches past their drawing: camera shake, a miss's colour drain.
                if (view.spec.type == QteType::SplitArc && view.spec.design == QteDesign::Converge && !view.spec.hidden)
                    screenResults.emplace_back(view.clock.outcome, view.spec.reducedMotion);
                if (owner != entries.end()) {
                    const auto& q = owner->nodes[c.active.node];
                    const auto index = view.clock.outcome == QteOutcome::Failure                  ? 1
                                       : view.clock.outcome == QteOutcome::Perfect && q.sounds[2] ? 2
                                                                                                  : 0;
                    if (q.sounds[index]) sounds.push_back(q.sounds[index]);
                }
            }
            if (view.clock.outcome != QteOutcome::None && !view.clock.sent &&
                view.clock.resultElapsed >= view.spec.delay) {
                view.clock.sent = true;
                if (owner != entries.end()) {
                    const auto& q = owner->nodes[c.active.node];
                    std::size_t index = view.clock.outcome == QteOutcome::Failure   ? 1
                                        : view.clock.outcome == QteOutcome::Perfect ? 2
                                                                                    : 0;
                    if (index == 2 && q.spec.results[2].event.empty()) index = 0;
                    const auto& r = q.spec.results[index];
                    if (!r.event.empty()) {
                        dispatchTokens.push_back(owner->token);
                        if (r.recipient != "partner") sends.push_back({owner->actor, q.results[index]});
                        if (r.recipient != "self" && owner->partner)
                            sends.push_back({owner->partner, q.results[index]});
                    }
                    if ((!r.to.empty() || !r.partnerTo.empty()) && owner->standalone)
                        starts.push_back({owner->actor, r.to, r.partnerTo});
                    logger::info("[QTE] result session={} channel={} id={} outcome={} actor={:08X} from='{}'",
                        view.session, channel, q.spec.id, static_cast<int>(view.clock.outcome),
                        owner->actor.get()->GetFormID(), Name(*owner));
                }
            }
            // A hidden QTE has no result to show: its channel is free once the result is sent.
            if (view.clock.sent &&
                view.clock.resultElapsed >= std::max(view.spec.hidden ? 0.0f : view.spec.feedback, view.spec.delay)) {
                view.visible = false;
                c.active = {};
            }
        }
        for (auto& e : entries)
            for (std::size_t i = 0; i < e.nodes.size(); ++i) {
                auto& q = e.nodes[i];
                const bool due = e.time >= 0 && (q.ready || (q.spec.at >= 0 && e.time >= q.spec.at) ||
                                                    (q.spec.fallback >= 0 && e.time >= q.spec.fallback));
                if (q.fired || !due) continue;
                q.fired = true;
                q.ready = false;  // the next time its event comes asks again (ObserveEvent)
                auto& c = channels[ChannelOf(q.spec)];
                // A second active challenge on the same channel never queues a stale prompt behind the first.
                const bool same = c.active.token == e.token;
                const bool pendingRoute =
                    same && c.view.clock.outcome != QteOutcome::None &&
                    !e.nodes[c.active.node].spec.results[ResultIndex(c, e.nodes[c.active.node].spec)].event.empty();
                const bool dispatched =
                    std::find(dispatchTokens.begin(), dispatchTokens.end(), e.token) != dispatchTokens.end();
                if (dispatched ||
                    (c.active.token && !c.view.clock.sent && !(q.spec.replaceActive && same && !pendingRoute))) {
                    logger::warn("[QTE] busy; skipped id={} channel={} without executing a failure route", q.spec.id,
                        ChannelOf(q.spec));
                    continue;
                }
                c.active = {e.token, i};
                c.view = {};
                c.view.visible = true;
                c.view.session = ++serial;
                c.view.spec = q.spec;
                c.soundSent = false;
                c.holdSession = 0;
                if (q.spec.control == "rightAttack") QteServices::ResolveAttack(c.view.spec);
                logger::info("[QTE] start session={} channel={} from='{}' id={}", c.view.session, ChannelOf(q.spec),
                    Name(e), q.spec.id);
            }
        // An event's QTEs are done once each was asked and none is on screen any more (or one never got there).
        std::erase_if(entries, [&](const Entry& e) {
            return e.standalone &&
                   std::all_of(e.nodes.begin(), e.nodes.end(), [](const Armed& q) { return q.fired; }) &&
                   std::none_of(
                       channels.begin(), channels.end(), [&](const Channel& c) { return c.active.token == e.token; });
        });
    }
    for (auto [outcome, still] : screenResults) QteScreenFx::Result(outcome, still);
    QteScreenFx::Update(delta);
    for (auto sound : sounds) {
        {
            std::lock_guard lock(mutex);
            if (generation != sendGeneration) break;
        }
        QteServices::PlaySound(sound);
    }
    // No state lock is held across graph callbacks or paired handoffs.
    for (const auto& send : sends) {
        {
            std::lock_guard lock(mutex);
            if (generation != sendGeneration) break;
        }
        if (auto actor = send.actor.get(); actor && actor->Is3DLoaded() && !actor->IsDead()) {
            // Use the same dispatch as HKX annotations: effects, runner slots,
            // another QTE and action links must all receive the result event.
            Actions::OnAnimEventFrom(actor.get(), send.event);
            {
                std::lock_guard lock(mutex);
                if (generation != sendGeneration) break;
            }
            actor->NotifyAnimationGraph(send.event);
        }
    }
    for (const auto& start : starts) {
        {
            std::lock_guard lock(mutex);
            if (generation != sendGeneration) break;
        }
        QteServices::StartAction(start.actor, start.to, start.partnerTo);
    }
}
void ResetOnLoad() {
    QteScreenFx::ResetOnLoad();
    std::lock_guard lock(mutex);
    ++serial;
    ++generation;
    entries.clear();
    for (auto& c : channels) Cancel(c);
    held.clear();
    claimed.clear();
}
View ReadView(std::size_t channel) {
    std::lock_guard lock(mutex);
    return channels[std::min(channel, kChannels - 1)].view;
}
std::array<View, kChannels> ReadViews() {
    std::lock_guard lock(mutex);
    std::array<View, kChannels> out;
    for (std::size_t i = 0; i < kChannels; ++i) out[i] = channels[i].view;
    return out;
}
}
