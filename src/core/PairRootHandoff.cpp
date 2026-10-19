#include "PCH.h"
#include "core/PairRootHandoff.h"
#include "core/PairRootExitPolicy.h"
#include <cmath>
#include <cstring>
#include <array>

namespace AnimationInjector::PairRootHandoff {
namespace {
float Length(const RE::NiPoint3& p) { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); }
bool Finite(const RE::NiPoint3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
// The AI session's two roles.
struct Session {
    PairedSession::Ticket ticket;
    State state;
};
std::array<Session, 2> sessions{};
}
void Stop(State& state, const char* reason) {
    state.stopped = true;
    logger::info("[PairRootRebase] #{} role={} stop={} started={} moved={:.5f}", state.owner, state.role, reason,
        state.started, state.distance);
}
bool Running(const State& state) { return !state.stopped && (state.sampled || state.armed); }
void Update(State& state, std::uint64_t owner, int role, const Frame& frame) {
    if (state.owner != owner) {
        state = {};
        state.owner = owner;
        state.role = role;
    }
    if (state.stopped) return;
    if (frame.abruptExit && !state.abruptExitSeen) {
        state.abruptExitSeen = true;
        state.abruptExitPending = true;
    }
    auto* actor = frame.actor;
    if (!actor || !actor->Is3DLoaded()) {
        Stop(state, "actor-unavailable");
        return;
    }
    if (!frame.allowedExit || actor->IsDead() || actor->IsInRagdollState() ||
        (actor->IsInMidair() && !frame.inheritedAir) || static_cast<int>(actor->AsActorState()->GetKnockState()) != 0) {
        Stop(state, "interrupted-or-airborne");
        return;
    }
    auto* controller = actor->GetCharController();
    const auto* root = frame.root;
    if (!root || !root->parent || !controller || !std::isfinite(frame.delta) || frame.delta <= 0 ||
        frame.delta > 0.1f) {
        Stop(state, "invalid-context");
        return;
    }
    // Measure authored displacement from the participant's skeleton rest pose.
    const auto local = root->local.translate - frame.reference;
    const auto position = actor->GetPosition();
    if (!Finite(local) || !Finite(position)) {
        Stop(state, "nonfinite");
        return;
    }
    const float horizontal = std::hypot(local.x, local.y);
    if (!state.armed && !frame.exitRequested) {
        state.previous = local;
        state.expectedActor = position;
        state.yaw = actor->GetAngleZ();
        state.actor = actor->GetHandle();
        state.copySequence = frame.sample;
        state.sampled = true;
        return;
    }
    if (!state.armed) {
        if (frame.sample == 0) return;
        if (state.sampled) {
            // The last playing sample precedes the exit signal; do not lose the first blend step.
            state.armed = true;
        } else {
            if (horizontal < 0.01f) {
                Stop(state, "no-root-displacement");
                return;
            }
            if (horizontal > 500 || std::fabs(local.z) > 2) {
                Stop(state, "unsupported-root-offset");
                return;
            }
            state.armed = true;
            state.actor = actor->GetHandle();
            state.previous = local;
            state.expectedActor = position;
            state.yaw = actor->GetAngleZ();
            state.copySequence = frame.sample;
            logger::info("[PairRootRebase] #{} role={} armed local={:.5f},{:.5f},{:.5f}", owner, role, local.x, local.y,
                local.z);
            return;
        }
    }
    state.elapsed += frame.delta;
    // Accepted locomotion legitimately changes position and heading during the fade.
    // Rebase only the disappearing root delta onto the CURRENT position below.
    if (!PairRootExitMayContinue(state.actor == actor->GetHandle(), frame.locomotion,
            Length(position - state.expectedActor), actor->GetAngleZ() - state.yaw, state.elapsed, frame.exitBudget)) {
        Stop(state, "external-change-or-timeout");
        return;
    }
    if (frame.sample == state.copySequence) return;
    state.copySequence = frame.sample;
    const float previousLength = std::hypot(state.previous.x, state.previous.y);
    if (previousLength > 500 || std::fabs(state.previous.z) > 2) {
        Stop(state, "unsupported-root-offset");
        return;
    }
    if (horizontal > previousLength + 0.05f && !state.started) {
        // An early exit can leave the source clip moving during its blend.
        // Track it until the root begins returning; never undo authored motion.
        state.previous = local;
        return;
    }
    if (horizontal > previousLength + 0.05f || std::fabs(local.z - state.previous.z) > 0.1f) {
        Stop(state, "not-returning-to-idle");
        return;
    }
    // Transfer only the disappearing translation; leave the animation pose intact.
    const auto difference = state.previous - local;
    auto displacement = (root->parent->world.rotate * difference) * root->parent->world.scale;
    if (!Finite(displacement) || std::fabs(displacement.z) > 0.5f ||
        (!state.abruptExitPending && Length(displacement) > 40) || state.distance + Length(displacement) > 550) {
        Stop(state, "unsupported-step");
        return;
    }
    displacement.z = 0;
    if (Length(displacement) > 0.0001f) {
        if (state.abruptExitPending && (Length(displacement) > 40 || horizontal < 0.01f)) {
            logger::info("[PairRootRebase] #{} role={} movement-cancel handoff remaining={:.5f}", owner, role,
                Length(displacement));
            state.abruptExitPending = false;
        }
        const auto target = position + displacement;
        const float scale = RE::bhkWorld::GetWorldScale();
        // Same center/warp arguments as the observed native ragdoll position handoff.
        // This is position rebasing, not swept collision movement.
        controller->SetPositionImpl(
            RE::hkVector4(target.x * scale, target.y * scale, target.z * scale, 0), true, false);
        actor->TESObjectREFR::SetPosition(target);
        RE::hkVector4 physical;
        controller->GetPositionImpl(physical, true);
        const float inv = RE::bhkWorld::GetWorldScaleInverse();
        const RE::NiPoint3 cc{
            physical.quad.m128_f32[0] * inv, physical.quad.m128_f32[1] * inv, physical.quad.m128_f32[2] * inv};
        state.expectedActor = actor->GetPosition();
        state.started = true;
        state.distance += Length(displacement);
        logger::debug(
            "[PairRootRebase] #{} role={} step local={:.5f},{:.5f} delta={:.5f},{:.5f} actor={:.5f},{:.5f} cc={:.5f},{:.5f}",
            owner, role, local.x, local.y, displacement.x, displacement.y, state.expectedActor.x, state.expectedActor.y,
            cc.x, cc.y);
        if (Length(state.expectedActor - target) > 0.25f || std::hypot(cc.x - target.x, cc.y - target.y) > 0.25f) {
            Stop(state, "placement-mismatch");
            return;
        }
    }
    state.previous = local;
    if (horizontal < 0.01f) Stop(state, "complete");
}

void Reset() { sessions = {}; }
bool Busy(PairedSession::Ticket ticket) {
    for (const auto& session : sessions)
        if (session.ticket == ticket && Running(session.state)) return true;
    return false;
}
void Cancel(int role) {
    if (role >= 0 && role < 2 && !sessions[role].state.stopped) {
        sessions[role].state.role = role;
        Stop(sessions[role].state, "actor-or-scene-unavailable");
    }
}
void Update(PairedSession::Ticket ticket, int role, RE::Actor* actor, RE::NiAVObject* root, const char* file,
    float clipTime, float duration, float delta, bool allowedExit, std::uint64_t copySequence, bool exitRequested,
    float exitBudget, const RE::NiPoint3& referenceTranslation, bool abruptExit, bool locomotion, bool inheritedAir) {
    if (role < 0 || role >= 2) return;
    auto& session = sessions[role];
    if (!(session.ticket == ticket)) session = {ticket, {}};
    // The caller owns session lifetime; the file and clip time are kept for its logs only.
    (void)file;
    (void)clipTime;
    (void)duration;
    // A request number of 0 is never issued; the state's owner 0 means "none yet".
    Update(session.state, ticket.request ? ticket.request : ~std::uint64_t{0}, role,
        Frame{actor, root, referenceTranslation, delta, allowedExit, copySequence, exitRequested, exitBudget,
            abruptExit, locomotion, inheritedAir});
}
}
