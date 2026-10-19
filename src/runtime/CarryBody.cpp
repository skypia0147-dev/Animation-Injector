#include "PCH.h"
#include "runtime/CarryBody.h"
#include "core/CarryPartialRagdoll.h"
#include "runtime/RagdollProfile.h"
#include "Settings.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace AnimationInjector::CarryBody {
namespace {
using namespace CarryPartialRagdoll;
Session session;
State current = State::kOff;
float stalled = 0, report = 0;
std::uint64_t lastSteps = 0;
std::uint64_t generation = 0;
// The profile read at a carry's first physics start. Restarts after a safety stop reuse it; the next
// lift (a new generation) reads the file again, so an edited profile still applies on the next lift.
struct CachedProfile {
    bool valid = false;
    std::uint64_t generation = 0;
    PairedRig::Kind rig = PairedRig::Kind::kUnsupported;
    std::string clip;
    std::optional<RagdollProfile::Profile> profile;
};
CachedProfile cached;
const std::optional<RagdollProfile::Profile>& ProfileFor(
    std::uint64_t carry, PairedRig::Kind rig, std::string_view liftClip) {
    if (!cached.valid || cached.generation != carry || cached.rig != rig || cached.clip != liftClip)
        cached = {true, carry, rig, std::string(liftClip), RagdollProfile::Load(rig, liftClip)};
    return cached.profile;
}

// The ragdoll body nearest the carrier bone. The session validates that it is an owned body.
RE::BSFixedString Nearest(RE::Actor* target, RE::Actor* carrier, const RE::BSFixedString& carrierBone) {
    auto* scene = target->Get3D(false);
    auto* root = carrier->Get3D(false);
    auto* bone = root ? root->GetObjectByName(carrierBone) : nullptr;
    if (!scene || !bone) return {};
    RE::BSFixedString best;
    float distance = std::numeric_limits<float>::infinity();
    unsigned budget = 4096;
    auto visit = [&](auto&& self, RE::NiAVObject* node, unsigned depth) -> void {
        if (!node || !budget-- || depth > 128) return;
        if (node->collisionObject) {
            const float d = (node->world.translate - bone->world.translate).Length();
            if (d < distance) {
                distance = d;
                best = node->name;
            }
        }
        if (auto* n = node->AsNode())
            for (const auto& child : n->GetChildren()) self(self, child.get(), depth + 1);
    };
    visit(visit, scene, 0);
    return best;
}
void Finish(EndReason reason, const char* why) {
    if (current == State::kOff) return;
    session.Invalidate(reason);
    logger::info("[CarryBody] ended reason={} generation={}", why, generation);
    current = State::kOff;
}
}
Start Begin(RE::ActorHandle carrier, RE::ActorHandle target, PairedRig::Kind rig, std::string_view liftClip,
    const RE::BSFixedString& part, const RE::BSFixedString& carrierBone, std::uint64_t carryGeneration) {
    Reset();
    auto carrierActor = carrier.get();
    auto targetActor = target.get();
    if (!carrierActor || !targetActor) return Start::kPose;
    const auto anchor = part.empty() ? Nearest(targetActor.get(), carrierActor.get(), carrierBone) : part;
    if (anchor.empty()) {
        logger::warn("[CarryBody] no ragdoll body found near '{}'; pose mode", carrierBone.c_str());
        return Start::kPose;
    }
    generation = carryGeneration;
    const auto& profile = ProfileFor(carryGeneration, rig, liftClip);
    Request request{carrier, target, carryGeneration, anchor};
    if (profile) {
        request.dynamics = profile->motion;
        request.bodies = profile->bodies;
        request.traceProfile = profile->source;
    }
    request.trace = FrameworkSettings::Get().ragdollTrace;
    request.traceRig = PairedRig::Name(rig);
    const auto prepared = session.Prepare(request);
    if (prepared != Result::kReady) {
        session.Invalidate(EndReason::kFault);
        if (prepared == Result::kForeignPhysicsOwner) {
            logger::warn("[CarryBody] target's own ragdoll still simulates (anchor='{}')", anchor.c_str());
            return Start::kTargetSimulating;
        }
        logger::warn(
            "[CarryBody] prepare failed result={} anchor='{}'; pose mode", static_cast<int>(prepared), anchor.c_str());
        return Start::kPose;
    }
    const auto enabled = session.Enable();
    if (enabled != Result::kReady) {
        logger::warn(
            "[CarryBody] enable failed result={} anchor='{}'; pose mode", static_cast<int>(enabled), anchor.c_str());
        session.Invalidate(EndReason::kFault);
        return Start::kPose;
    }
    current = State::kSimulating;
    stalled = report = 0;
    lastSteps = 0;
    const auto status = session.ReadStatus();
    logger::info("[CarryBody] started anchor='{}'{} dynamic={} anchored={} generation={}", anchor.c_str(),
        part.empty() ? " (nearest)" : "", status.selectedBodies, status.anchoredBodies, generation);
    return Start::kStarted;
}
State Update(float delta) {
    if (current != State::kSimulating) return current;
    session.BeforePhysics(delta);
    session.AfterPhysics(delta);
    const auto status = session.ReadStatus();
    if (status.phase == Phase::kFaulted) {
        logger::warn("[CarryBody] physics safety stop steps={} maxJointError={:.3f} bone={}", status.physicsSteps,
            status.maximumJointError, status.maximumJointBone.c_str());
        current = State::kFaulted;
        return current;
    }
    // Havok must actually advance while bodies are dynamic.
    stalled = status.physicsSteps != lastSteps ? 0 : stalled + std::clamp(delta, 0.0f, 0.1f);
    lastSteps = status.physicsSteps;
    if ((report += std::clamp(delta, 0.0f, 0.1f)) >= 2) {
        report = 0;
        logger::debug("[CarryBody] steps={} dynamic={} anchored={} maxJointError={:.3f} bone={}", status.physicsSteps,
            status.selectedBodies, status.anchoredBodies, status.maximumJointError, status.maximumJointBone.c_str());
    }
    if (stalled > 2) {
        logger::warn("[CarryBody] Havok did not advance for 2s; safety stop");
        current = State::kFaulted;
    }
    return current;
}
bool BeginRelease() {
    if (current != State::kSimulating) return false;
    const auto result = session.BeginNativeRelease();
    if (result != Result::kReady) {
        logger::warn("[CarryBody] release snapshot rejected result={}", static_cast<int>(result));
        return false;
    }
    current = State::kReleasing;
    return true;
}
Release AdvanceRelease() {
    if (current != State::kReleasing) return Release::kFailed;
    const auto result = session.AdvanceNativeRelease();
    if (result == Result::kBusy) return Release::kWaiting;
    if (result == Result::kReady) {
        // The session yielded the bodies to the engine and cleared itself.
        logger::info("[CarryBody] ragdoll handoff committed generation={}", generation);
        current = State::kOff;
        return Release::kDone;
    }
    logger::warn("[CarryBody] ragdoll handoff failed result={}", static_cast<int>(result));
    session.Invalidate(EndReason::kFault);
    current = State::kOff;
    return Release::kFailed;
}
void End(bool yield) {
    if (current == State::kReleasing) return;
    Finish(yield ? EndReason::kFullRagdoll : EndReason::kSceneChange, yield ? "yield" : "restore");
}
void Reset() {
    if (current == State::kOff) return;
    session.Invalidate(EndReason::kLoadGame);
    logger::info("[CarryBody] reset generation={}", generation);
    current = State::kOff;
}
State Current() { return current; }
}
