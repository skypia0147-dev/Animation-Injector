#pragma once
#include "runtime/ConditionValues.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "runtime/PairFeatureSpec.h"

namespace logger {
template<class... Args>
void debug(const char*, Args&&...) {}
template<class... T>
void info(const char*, T&&...) {}
template<class... T>
void warn(const char*, T&&...) {}
}
namespace RE {
struct BSFixedString {
    std::string value;
    BSFixedString(const char* s = "") : value(s) {}
    bool empty() const { return value.empty(); }
    bool operator==(const BSFixedString&) const = default;
};
struct Actor;
struct ActorPtr {
    Actor* value;
    Actor* get() const { return value; }
    Actor* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
struct ActorHandle {
    explicit operator bool() const { return *this != ActorHandle{}; }
    unsigned id = 0;
    bool operator==(const ActorHandle&) const = default;
    ActorPtr get() const;
};
struct Actor {
    unsigned id = 0;
    bool exists = true, loaded = true, dead = false, ragdoll = false;
    bool Is3DLoaded() const { return loaded; }
    bool IsDead() const { return dead; }
    bool IsInRagdollState() const { return ragdoll; }
    unsigned GetFormID() const { return id; }
    bool GetGraphVariableBool(const BSFixedString&, bool& value) {
        value = false;
        return true;
    }
};
inline std::array<Actor, 4> actors;
inline ActorPtr ActorHandle::get() const {
    return {id && id < actors.size() && actors[id].exists ? &actors[id] : nullptr};
}
}
namespace AnimationInjector {
namespace ActionLinks {
void Begin(RE::ActorHandle, int, bool, bool, RE::ActorHandle = {});
void End(RE::ActorHandle, int);
void ObserveTime(RE::ActorHandle, int, float, float = 0);
void ObserveEvent(RE::ActorHandle, const RE::BSFixedString&);
struct InputResult {
    bool claimed = false, taken = false;
};
InputResult Input(std::uint32_t, bool, GameControl = GameControl::kNone, bool = true);
bool ClaimsEvent(RE::ActorHandle, const RE::BSFixedString&);
void Update(float);
void ResetOnLoad();
bool HasPendingTransition(RE::ActorHandle);
bool RequestHandoff(RE::ActorHandle, int, int);
}
struct HarnessSpec {
    std::string id;
    struct {
        bool enabled = false;
    } paired;
    std::vector<int> clips;
    std::vector<ActionLinkSpec> links;
};
inline std::array<HarnessSpec, 4> specs;
class ActionRunner {
public:
    enum class State { kIdle, kActive };
    int index = 0, starts = 0, exits = 0;
    bool preflight = true, condition = true, accept = true;
    State state = State::kIdle;
    bool CanRequestLink() const { return preflight; }
    bool CanRequest() const { return condition; }
    const HarnessSpec& Spec() const { return specs[index]; }
    State GetState() const { return state; }
    bool Request(int = -1) {
        ++starts;
        if (accept) state = State::kActive;
        return accept;
    }
    bool RequestLinked(ActionRunner&, const RE::BSFixedString&, const RE::BSFixedString&) { return Request(0); }
    RE::ActorHandle pairedTarget;
    bool pairTargetValid = true;
    bool ValidPairedTarget(RE::ActorHandle target) const { return pairTargetValid && static_cast<bool>(target); }
    bool CanRequestPairLink(RE::ActorHandle target) const { return preflight && ValidPairedTarget(target); }
    bool RequestPaired(RE::ActorHandle target = {}) {
        pairedTarget = target;
        return Request(0);
    }
    bool ExitForLink() {
        ++exits;
        state = State::kIdle;
        return true;
    }
};
inline std::array<std::array<ActionRunner, 4>, 4> runners;
namespace Actions {
inline RE::ActorHandle CaptureTarget(RE::Actor*, int, RE::ActorHandle partner = {}) {
    return partner ? partner : RE::ActorHandle{2};
}
inline int flowSelection = -2;
inline std::vector<std::uint64_t> requestSeeds;
inline int ResolveFlow(
    RE::Actor*, int action, RE::ActorHandle = {}, bool = false, ConditionValues::Request* request = nullptr) {
    if (request) {
        request->Chance(action, 0, 30);
        requestSeeds.push_back(request->seed);
    }
    return flowSelection == -2 ? action : flowSelection;
}
inline int Count() { return static_cast<int>(specs.size()); }
inline const HarnessSpec& Spec(int index) { return specs[index]; }
inline int IndexOf(std::string_view id) {
    for (int i = 0; i < Count(); ++i)
        if (specs[i].id == id) return i;
    return -1;
}
inline ActionRunner* RunnerFor(RE::Actor* actor, int index, bool) {
    return actor ? &runners[actor->id][index] : nullptr;
}
}
namespace PairedSession {
struct Ticket {
    unsigned generation = 1, request = 1;
    bool operator==(const Ticket&) const = default;
};
enum class Phase { kIdle, kPlaying, kStopping };
enum class EndReason { kCancelled, kUnavailable };
struct Participant {
    RE::ActorHandle actor;
    bool graphStateRestored = true;
};
struct Status {
    Ticket ticket;
    int action = 0;
    Phase phase = Phase::kPlaying;
    EndReason lastEnd = EndReason::kCancelled;
    Participant initiator{{1}}, target{{2}};
};
inline Status status;
inline int cancellations = 0;
inline int handoffs = 0, completions = 0;
inline bool allowHandoff = true, actorStarted = false, partnerStarted = false;
inline Status ReadStatus() { return status; }
inline bool TryGetTicket(RE::ActorHandle actor, Ticket& ticket) {
    if (status.phase == Phase::kIdle || (actor != status.initiator.actor && actor != status.target.actor)) return false;
    ticket = status.ticket;
    return true;
}
inline void Cancel(Ticket) {
    ++cancellations;
    status.phase = Phase::kStopping;
}
inline bool BeginOrdinaryHandoff(Ticket ticket, RE::ActorHandle actor, int, int) {
    if (!allowHandoff || ticket != status.ticket || status.phase != Phase::kPlaying ||
        (actor != status.initiator.actor && actor != status.target.actor))
        return false;
    auto a = status.initiator.actor.get(), b = status.target.actor.get();
    if (!a || !b || !a->loaded || !b->loaded || a->dead || b->dead || a->ragdoll || b->ragdoll) return false;
    ++handoffs;
    Cancel(ticket);
    return true;
}
inline void CompleteOrdinaryHandoff(Ticket, RE::ActorHandle, bool a, bool b) {
    ++completions;
    actorStarted = a;
    partnerStarted = b;
}
}
}
