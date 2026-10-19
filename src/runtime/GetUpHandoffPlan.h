#pragma once
#include <cmath>
#include <cstdint>

// Starting an action on a body the engine is getting up (0.12.249). The Ragdoll Recovery plugin sends its pose
// event when the ragdoll comes to rest; an event QTE asked then is answered within the engine's 0.51 s before its
// get-up clip, and the action its result names takes that clip's place.
//
// The engine gets the body up by itself: at rest (knock kQueued) it sends GetUpBegin, FullyRagdoll pulls the body
// toward the clip's first pose and sends GetUpStart 0.5 s later, and at GetUpStart it hands the body back to the
// character controller (knock kGetUp). Right then the clip still lies at its first frames, matched and aligned to
// the body. Getup and GetUpEnd are supplied to the actor one per frame (the knock state returns to normal on
// GetUpEnd, RagdollRecoveryProbe 0.1.7); then, in one frame, GetUpExit and the action's entry.
//
// GetUpExit is the only way out of the get-up state (0_master Master_Behavior state 24 -> its Root state, a 0.2 s
// blend), and the actions' entries live inside that Root state (RootBehaviorGraph's wildcards): while the graph is
// in the get-up state they are refused (0.12.249 in game: every entry refused, the engine's clip played on with the
// knock state already normal, so the player walked and jumped while it lay on the ground). Sent together, the blend
// goes from the engine's first frames to the action's (whose own entry blend should be 0: a longer one mixes in the
// Root state's standing start). Nothing consumes GetUpExit elsewhere. Only notifications that did not come
// naturally are supplied; GetUpBegin and GetUpStart never are (a forced GetUpStart froze the probe's player in the
// air, and a get-up started before rest flips the body). Pure, so the steps are tested without the game.
namespace AnimationInjector::GetUpHandoffPlan {
// Skipping the engine's pull (0.12.251, the user's experiment): the pull toward the clip's first pose twists a body
// that rests oddly (vanilla does too). A body at rest (knock kQueued) is started at once: GetUpStart into the graph
// (FullyRagdoll -> GetUpFromRagdoll, unconditional) and to the actor's sink (the engine hands the body back on it, as
// it does when GetUpTimerMod raises it). The Ragdoll Recovery plugin (1.3.0) holds the engine's GetUpBegin while the
// QTE is open, so the pull never starts. Not handed back within kKickSeconds: the graph is put back into FullyRagdoll
// (RagdollInstant) and asked GetUpBegin kRescueSettleSeconds later (0.12.155's verified resend), and the engine's
// own get-up takes the body; the action is not started.
//
// The pull is not skipped whole (0.12.252): the same modifier computes the character's world-from-model from the
// ragdoll's COM, neck and left upper arm (WORLD_FROM_MODEL_MODE_COMPUTE), which moves the actor onto the body (a jump
// of about 33 units within 0.1 s in RagdollRecoveryProbe's logs). Without it the get-up clip played at the actor's
// place and heading from before the fall, facing away from the body (0.12.251 in game). So GetUpBegin goes first
// (kAlign) and GetUpStart kAlignSeconds later: the alignment happens, the twisting pull barely starts.

// RE::KNOCK_STATE_ENUM values.
inline constexpr int kKnockNormal = 0;
inline constexpr int kKnockQueued = 5;
inline constexpr int kKnockGetUp = 6;
enum class Phase : std::uint8_t { kHandoff, kGetup, kGetUpEnd, kStand, kRescue, kDone };
enum class Act : std::uint8_t {
    kNone,
    kAlign,
    kKick,
    kDeliverGetup,
    kDeliverGetUpEnd,
    kStart,
    kRescueRagdoll,
    kRescueGetUp,
    kGiveUp
};
// Natural notifications seen while waiting (bits of `seen`).
inline constexpr std::uint8_t kSawGetup = 1, kSawGetUpEnd = 2, kSawGetUpExit = 4;
struct Sample {
    bool present = false;     // the actor resolves, is alive and has its 3D
    bool held = false;        // paralysis, bleedout or unconscious keeps it down on purpose
    bool ragdoll = false;     // IsInRagdollState
    bool controller = false;  // a character controller in a havok world
    bool suspended = false;   // the controller still follows the ragdoll or does not simulate
    bool grounded = false;    // the controller's state is not in the air
    int knock = kKnockNormal;
};
struct State {
    Phase phase = Phase::kHandoff;
    float life = 0.0f;
    float airborne = 0.0f;  // knock normal, the controller still in the air
    float aligned = -1.0f;  // seconds since GetUpBegin was sent for the alignment; negative before it
    float kicked = -1.0f;   // seconds since the kick; negative before it
    float rescuing = 0.0f;  // seconds since RagdollInstant
};
inline constexpr float kAlignSeconds = .1f;
inline constexpr float kKickSeconds = .5f;
inline constexpr float kRescueSettleSeconds = .1f;
// With kStart: the graph is to be sent GetUpExit first (it is in, or may be in, the get-up state).
inline bool LeaveGetUp(std::uint8_t seen) { return !(seen & kSawGetUpExit); }
// The longest a start waits: a long fall rests late, and the engine's get-up clip begins 0.51 s after the rest.
inline constexpr float kGiveUpSeconds = 8.0f;
// A controller handed back reads "in air" for a moment while the body lies on the ground (0.12.247 in game: 3 of
// 12). An action that may not start in the air waits this long for the ground; after it the start is tried anyway.
inline constexpr float kGroundWaitSeconds = .5f;
// The engine's get-up clip lies near its first frames for a moment (the back one sits up from 0.2 s, the front one
// from 0.4 s). A result after that would rewind a body already on its way up, so the engine's get-up keeps it.
inline constexpr float kLateGetUpSeconds = .3f;
inline bool Down(int knock, bool ragdoll) { return ragdoll || knock != kKnockNormal; }
inline bool TooLate(int knock, bool ragdoll, float getUpFor) {
    return knock == kKnockGetUp && !ragdoll && getUpFor >= kLateGetUpSeconds;
}
inline bool HandedBack(const Sample& s) { return s.knock == kKnockGetUp && !s.ragdoll && s.controller && !s.suspended; }
inline bool Standing(const Sample& s) { return s.knock == kKnockNormal && !s.ragdoll && s.controller && !s.suspended; }
inline Act Stop(State& st) {
    st.phase = Phase::kDone;
    return Act::kGiveUp;
}
// One frame. At most one notification per frame; each step rechecks the handoff (a new hit or a lost controller
// leaves the engine's own get-up alone). `waitGround`: the action may not start in the air.
inline Act Advance(State& st, const Sample& s, std::uint8_t seen, bool waitGround, float delta) {
    if (st.phase == Phase::kDone) return Act::kNone;
    if (!s.present || s.held) return Stop(st);
    const bool timed = std::isfinite(delta) && delta > 0.0f;
    if (timed) st.life += delta;
    if (st.life >= kGiveUpSeconds) return Stop(st);
    switch (st.phase) {
    case Phase::kHandoff:
        if (s.knock == kKnockNormal && !s.ragdoll) {
            st.phase = Phase::kStand;
            return Act::kNone;
        }
        if (HandedBack(s)) {
            st.phase = Phase::kGetup;
            return Act::kNone;
        }
        if (st.aligned < 0.0f) {
            if (!s.ragdoll || s.knock != kKnockQueued) return Act::kNone;
            st.aligned = 0.0f;
            return Act::kAlign;
        }
        if (st.kicked < 0.0f) {
            if (timed) st.aligned += delta;
            if (st.aligned < kAlignSeconds) return Act::kNone;
            st.kicked = 0.0f;
            return Act::kKick;
        }
        if (timed) st.kicked += delta;
        if (st.kicked < kKickSeconds) return Act::kNone;
        st.phase = Phase::kRescue;
        return Act::kRescueRagdoll;
    case Phase::kRescue:
        if (timed) st.rescuing += delta;
        if (st.rescuing < kRescueSettleSeconds) return Act::kNone;
        st.phase = Phase::kDone;
        return Act::kRescueGetUp;
    case Phase::kGetup:
    case Phase::kGetUpEnd: {
        if (s.ragdoll || (s.knock != kKnockGetUp && s.knock != kKnockNormal) || !s.controller || s.suspended)
            return Stop(st);
        const auto phase = st.phase;
        st.phase = phase == Phase::kGetup ? Phase::kGetUpEnd : Phase::kStand;
        if (phase == Phase::kGetup) return seen & kSawGetup ? Act::kNone : Act::kDeliverGetup;
        return seen & kSawGetUpEnd ? Act::kNone : Act::kDeliverGetUpEnd;
    }
    case Phase::kStand:
        if (s.ragdoll) return Stop(st);
        if (!Standing(s)) return Act::kNone;
        if (waitGround && !s.grounded && st.airborne < kGroundWaitSeconds) {
            if (timed) st.airborne += delta;
            return Act::kNone;
        }
        st.phase = Phase::kDone;
        return Act::kStart;
    case Phase::kDone: break;
    }
    return Act::kNone;
}
}
