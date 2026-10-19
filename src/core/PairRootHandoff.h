#pragma once
#include "runtime/PairedSession.h"
namespace AnimationInjector::PairRootHandoff {
// One participant's handoff of the root bone's disappearing horizontal offset to the actor as a pair blends out.
// The caller owns it: the AI session keeps one per role (the functions below the State ones), NativePairEnd one per
// participant of each engine-started pair, so several pairs can end at once (kill moves between NPCs).
struct State {
    std::uint64_t owner = 0;  // the AI ticket's request or the native participant; a new owner starts over
    int role = 0;
    RE::ActorHandle actor;
    bool armed = false, started = false, stopped = false, sampled = false;
    bool abruptExitSeen = false, abruptExitPending = false;
    RE::NiPoint3 previous{}, expectedActor{};
    float yaw = 0, elapsed = 0, distance = 0;
    std::uint64_t copySequence = 0;
};
// One main-thread look at the participant, after the animation update. Object arguments are borrowed for the call.
// inheritedAir: the controller's in-air state is the one the previous pair left frozen, not a jump or fall
// (PairedSession::ParticipantStatus::inheritedAir while the pair is live).
struct Frame {
    RE::Actor* actor = nullptr;
    RE::NiAVObject* root = nullptr;  // "NPC Root [Root]" of the actor's third-person skeleton
    RE::NiPoint3 reference{};        // the root's rest translation (skeleton reference pose)
    float delta = 0;
    bool allowedExit = true;
    std::uint64_t sample = 0;  // changes when a new pose was produced
    bool exitRequested = false;
    float exitBudget = 2.0f;
    bool abruptExit = false, locomotion = false, inheritedAir = false;
};
void Update(State& state, std::uint64_t owner, int role, const Frame& frame);
// Sampling or handing over (not yet stopped).
bool Running(const State& state);
void Stop(State& state, const char* reason);

// The AI session's two roles (PairRootRuntime). Main thread only, after graph observations release their locks.
void Update(PairedSession::Ticket ticket, int role, RE::Actor* actor, RE::NiAVObject* root, const char* file,
    float clipTime, float duration, float delta, bool allowedExit, std::uint64_t copySequence, bool exitRequested,
    float exitBudget, const RE::NiPoint3& referenceTranslation = {}, bool abruptExit = false, bool locomotion = false,
    bool inheritedAir = false);
// Drops bookkeeping only. Never teleports an actor back across a load.
void Reset();
void Cancel(int role);
bool Busy(PairedSession::Ticket ticket);
}
