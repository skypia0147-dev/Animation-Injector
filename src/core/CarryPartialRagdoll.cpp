#include "PCH.h"
#include "core/CarryPartialRagdoll.h"
#include "core/PartialRagdollPolicy.h"
#include "core/CarryLimbFrame.h"
#include "core/CarryJointAngles.h"
#include "core/RagdollTrace.h"
#include "RE/H/hkpLimitedHingeConstraintData.h"
#include "RE/H/hkpRagdollConstraintData.h"
#include "RE/H/hkpWorldPostSimulationListener.h"
#include <array>
#include <mutex>
#include <numbers>
#include <optional>

namespace AnimationInjector::CarryPartialRagdoll {
namespace {
using Motion = RE::hkpMotion::MotionType;
constexpr std::uint32_t syncMask = (1u << 7) | (1u << 10);
constexpr auto carryLayer = RE::COL_LAYER::kDeadBip;
// A larger per-frame attachment move is a teleport: the bodies are carried rigidly for that frame.
constexpr float worldJump = 60.0f;
// So is a larger per-frame turn: the carrier snapping to a new heading (56 degrees in one 10 ms
// step was recorded) swings a held neck 40 units from the root across the room in one frame.
constexpr float worldTurn = 30.0f * std::numbers::pi_v<float> / 180.0f;
// A start pose past a joint limit is the lift animation's pose: the limit opens to it with this
// much to spare (HKX_Edit's widen_to_start), instead of Havok snapping the joint back in the first
// step. 0.12.198 traces: a lift ending with the hip 14.5 degrees past its cone kicked the leg at
// 780 to 970 units/s once the pelvis was held (simulate="false").
constexpr float startPad = 1.0f * std::numbers::pi_v<float> / 180.0f;
// Joint check (AfterPhysics): past faultError a joint has come apart; a frame whose worst joint is
// within soundError is a shape the bodies may be put back to. At most repairLimit repairs in
// repairWindow seconds, then a safety stop (the carry restarts the physics).
constexpr float faultError = 35.0f, soundError = 12.0f, repairWindow = 1.0f;
constexpr unsigned repairLimit = 3;
// [Debug] bRagdollTrace: about two minutes at 100 physics steps per second; later steps are
// dropped and counted.
constexpr std::size_t traceSteps = 12000;
RE::NiPoint3 Vector(const RE::hkVector4& v) { return {v.quad.m128_f32[0], v.quad.m128_f32[1], v.quad.m128_f32[2]}; }
RE::hkVector4 Vector(const RE::NiPoint3& v) { return {v.x, v.y, v.z, 0}; }
bool Finite(const RE::NiTransform& t) {
    if (!std::isfinite(t.scale) || t.scale < 0.01f || t.scale > 10) return false;
    if (!std::isfinite(t.translate.x) || !std::isfinite(t.translate.y) || !std::isfinite(t.translate.z)) return false;
    for (const auto& row : t.rotate.entry)
        for (const float v : row)
            if (!std::isfinite(v)) return false;
    return true;
}
RE::NiTransform ReadPose(const RE::hkpRigidBody& body) {
    const auto& t = body.motion.motionState.transform;
    RE::NiTransform r;
    const RE::hkVector4* cols[]{&t.rotation.col0, &t.rotation.col1, &t.rotation.col2};
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned y = 0; y < 3; ++y) r.rotate.entry[y][c] = cols[c]->quad.m128_f32[y];
    const auto& p = t.translation.quad;
    r.translate = {p.m128_f32[0], p.m128_f32[1], p.m128_f32[2]};
    r.translate *= RE::bhkWorld::GetWorldScaleInverse();
    return r;
}
void WritePose(RE::bhkRigidBody& body, const RE::NiTransform& t) {
    const auto p = t.translate * RE::bhkWorld::GetWorldScale();
    RE::hkVector4 hp{p.x, p.y, p.z, 0};
    const RE::NiQuaternion q(t.rotate);
    RE::hkQuaternion hq;
    hq.vec = {q.x, q.y, q.z, q.w};
    body.SetPositionAndRotation(hp, hq);
}
void Refresh(RE::NiAVObject* node) {
    RE::NiUpdateData data{};
    data.flags = RE::NiUpdateData::Flag::kDisableCollision;
    node->UpdateDownwardPass(data, 0);
}
// The ragdoll joint limits a rig profile may set, in radians: cone maximum, plane minimum and
// maximum, twist minimum and maximum (the fields hkpRagdollConstraintData's limit setters write).
std::array<float*, 5> Limits(RE::hkpRagdollConstraintData::Atoms& atoms) {
    return {&atoms.coneLimit.maxAngle, &atoms.planesLimit.minAngle, &atoms.planesLimit.maxAngle,
        &atoms.twistLimit.minAngle, &atoms.twistLimit.maxAngle};
}
float Degrees(float radians) { return radians * 180.0f / std::numbers::pi_v<float>; }
// A constraint atom's local frame rotation, as a matrix acting on column vectors (see ReadPose).
RE::NiMatrix3 FrameRotation(const RE::hkTransform& t) {
    RE::NiMatrix3 r;
    const RE::hkVector4* cols[]{&t.rotation.col0, &t.rotation.col1, &t.rotation.col2};
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned y = 0; y < 3; ++y) r.entry[y][c] = cols[c]->quad.m128_f32[y];
    return r;
}
// The axes CarryJointAngles measures: Havok's own ragdoll layout (twist 0, plane 1).
bool StandardAxes(const RE::hkpRagdollConstraintData::Atoms& atoms) {
    using Mode = RE::hkpConeLimitConstraintAtom::MeasurementMode;
    return atoms.twistLimit.twistAxis == 0 && atoms.twistLimit.refAxis == 1 && atoms.coneLimit.twistAxisInA == 0 &&
           atoms.coneLimit.refAxisInB == 0 && atoms.coneLimit.angleMeasurementMode == Mode::kZeroWhenVectorsAligned &&
           atoms.planesLimit.twistAxisInA == 0 && atoms.planesLimit.refAxisInB == 1 &&
           atoms.planesLimit.angleMeasurementMode == Mode::kZeroWhenVectorsPerpendicular;
}
// An attachment change the bodies follow rigidly rather than through physics.
bool Carried(const RE::NiTransform& from, const RE::NiTransform& to) {
    return (to.translate - from.translate).Length() > worldJump ||
           CarryLimbFrame::TurnAngle(from.rotate, to.rotate) > worldTurn;
}
// The largest one-frame jump of a held body's keyframe the joints absorb. Running and
// jumping move them 2 to 10 units a frame at 100 fps. A carrier's jump at its apex dropped the
// attachment 41 units and turned it 12.7 degrees in one frame, both under the limits above; the held
// pelvis jumped 120 and pulled a thigh 40 off its joint (0.12.202, physics safety stop). Beyond this
// the bodies are carried rigidly. A long frame may cover a fast carrier's legitimate travel.
float KeyedJumpLimit(float delta) { return std::max(25.0f, 1200.0f * std::clamp(delta, 0.0f, 0.1f)); }
RagdollTrace::Pose TracePose(const RE::NiTransform& t) {
    const RE::NiQuaternion q(t.rotate);
    return {{t.translate.x, t.translate.y, t.translate.z}, {q.w, q.x, q.y, q.z}};
}
// bhkRigidBodyT carries its own frame relative to the node (private base: compare RTTI directly).
bool OffsetBody(const RE::bhkRigidBody* body) {
    static REL::Relocation<const RE::NiRTTI*> offset{RE::NiRTTI_bhkRigidBodyT};
    for (const auto* rtti = body ? body->GetRTTI() : nullptr; rtti; rtti = rtti->GetBaseRTTI())
        if (rtti == offset.get()) return true;
    return false;
}
}

struct Session::State final : RE::hkpWorldPostSimulationListener {
    struct Body {
        RE::NiPointer<RE::NiAVObject> node;
        RE::NiPointer<RE::bhkNiCollisionObject> collision;
        RE::NiPointer<RE::bhkRigidBody> wrapper;
        RE::hkRefPtr<RE::hkpRigidBody> rigid;
        RE::NiTransform offset, local, sample;
        RE::NiTransform releasePose;
        RE::NiPoint3 releaseLinear{}, releaseAngular{};
        Motion original = Motion::kInvalid, applied = Motion::kInvalid;
        RE::COL_LAYER layer{};
        std::uint32_t flags = 0, writtenFlags = 0;
        unsigned part = 0;
        bool selected = false, anchor = false, changed = false, owned = false;
        // An anchor below simulated bodies (a chicken's neck) is pinned to the scene frame at its
        // captured model-space pose, never to its node, whose ancestors physics moves.
        bool pinned = false;
        // A simulated body with no body above it (the pelvis hanging from that neck) is free
        // relative to its parent; its stretch is measured from the anchor instead.
        bool hanging = false;
        float reach = 0;
        RE::NiTransform inScene;
        // Whole body: the body frame relative to the anchor body in the last sound frame (scale 1).
        RE::NiTransform sound;
        // The last keyframe written, to give the next one its velocity.
        RE::NiTransform keyframe;
        bool keyed = false;
        // BodyTuning::swingFrom: a bodiless bone above this body (a chicken's thigh above its foot).
        // The body swings about it; the chain from it turns with the body.
        RE::NiPointer<RE::NiAVObject> limbRoot;
        RE::NiTransform limbRootLocal, limbRootInParent, nodeInParent;
        std::size_t parentBody = 0;
        // Dynamics for this body: the rig profile's Body values over its Motion.
        float drag = 0, spin = 0, gravityMin = 1, inertiaScale = 1;
        PartialRagdollPolicy::DampingOverride linearDamping, angularDamping;
        // Dynamics::gravity above the native 1.0, as a velocity change per second (Havok units).
        // Not written to hkpMotion::gravityFactor: CommonLibSSE places that field at 0x134, and
        // recorded carries (0.12.182 trace) moved as if the factor stayed 1.0 while reading back
        // 3.12 there — Havok 2010 declares it right after the 16-bit savedQualityTypeIndex (0x132).
        // Every vanilla ragdoll body has factor 1.0 (HKX_Edit's 49 extracted rigs), so the extra
        // is (gravity - 1) times the world's gravity.
        RE::NiPoint3 extraGravity{};
        // Dynamics::inertia: the inverse inertia divided while simulating (xyz of inertiaAndMassInv).
        RE::hkVector4 inertiaBefore, inertiaApplied;
        bool inertiaOwned = false;
        bool ScaleInertia(float factor) {
            auto& value = rigid->motion.inertiaAndMassInv.quad;
            if (inertiaOwned || !std::isfinite(factor) || factor < 1) return false;
            inertiaBefore = rigid->motion.inertiaAndMassInv;
            for (unsigned i = 0; i < 3; ++i) value.m128_f32[i] /= factor;
            inertiaApplied = rigid->motion.inertiaAndMassInv;
            inertiaOwned = true;
            return true;
        }
        void RestoreOverrides() {
            auto& motion = rigid->motion;
            linearDamping.Restore(motion.motionState.linearDamping);
            angularDamping.Restore(motion.motionState.angularDamping);
            const auto& value = motion.inertiaAndMassInv.quad.m128_f32;
            const auto& applied = inertiaApplied.quad.m128_f32;
            if (inertiaOwned && value[0] == applied[0] && value[1] == applied[1] && value[2] == applied[2] &&
                value[3] == applied[3])
                motion.inertiaAndMassInv = inertiaBefore;
            inertiaOwned = false;
        }
    };
    // A native ragdoll joint between two owned bodies, with the values the carry changed.
    struct Joint {
        RE::hkRefPtr<RE::hkpConstraintInstance> instance;
        RE::hkpRagdollConstraintData* data = nullptr;  // null: not a ragdoll joint, left alone
        std::size_t a = 0, b = 0;                      // body indices of entities A and B
        std::size_t limb = SIZE_MAX;                   // body whose chain root takes the pivot
        float frictionBefore = 0, frictionApplied = 0;
        bool frictionOwned = false, pivotOwned = false;
        RE::hkTransform before[2];
        RE::hkVector4 applied[2];
        // Rig profile values (from the BodyTuning of the joint's lower body), radians.
        std::optional<float> friction, cone;
        std::optional<std::pair<float, float>> plane, twist;
        float limitsBefore[5]{}, limitsApplied[5]{};  // Limits() order
        bool limitsOwned = false;
        // A limited hinge (knee, elbow, ankle, wrist): only its angle limits change, and only to
        // take the start pose.
        RE::hkpLimitedHingeConstraintData* hinge = nullptr;
        float hingeBefore[2]{}, hingeApplied[2]{};
        bool hingeOwned = false;
    };
    std::vector<Joint> joints;
    // Limits opened to the start pose at Enable, for the log and the trace header.
    std::vector<RagdollTrace::WidenedLimit> widened;
    // Before the bodies return to keyframed motion or to the engine. Values another system
    // changed since are left as they are.
    void RestoreJoints() {
        for (auto& j : joints) {
            if (j.hinge && j.hingeOwned) {
                auto& limit = j.hinge->atoms.angLimit;
                if (limit.minAngle == j.hingeApplied[0]) limit.minAngle = j.hingeBefore[0];
                if (limit.maxAngle == j.hingeApplied[1]) limit.maxAngle = j.hingeBefore[1];
            }
            j.hingeOwned = false;
            if (!j.data) continue;
            auto& atoms = j.data->atoms;
            if (j.frictionOwned && atoms.angFriction.maxFrictionTorque == j.frictionApplied)
                atoms.angFriction.maxFrictionTorque = j.frictionBefore;
            j.frictionOwned = false;
            if (j.pivotOwned)
                for (unsigned side = 0; side < 2; ++side) {
                    auto& t = side ? atoms.transforms.transformB : atoms.transforms.transformA;
                    const auto& now = t.translation.quad.m128_f32;
                    const auto& set = j.applied[side].quad.m128_f32;
                    if (now[0] == set[0] && now[1] == set[1] && now[2] == set[2]) t = j.before[side];
                }
            j.pivotOwned = false;
            if (j.limitsOwned) {
                const auto limits = Limits(atoms);
                for (unsigned k = 0; k < limits.size(); ++k)
                    if (*limits[k] == j.limitsApplied[k]) *limits[k] = j.limitsBefore[k];
            }
            j.limitsOwned = false;
        }
    }
    Request request;
    Status status;
    RE::NiPointer<RE::NiAVObject> scene;
    RE::NiPointer<RE::bhkWorld> world;
    RE::hkRefPtr<RE::hkpWorld> nativeWorld;
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    RE::BSTSmartPointer<RE::BShkbAnimationGraph> graph;
    std::vector<Body> bodies;
    std::mutex samplesMutex;
    bool listening = false, haveSample = false, invalid = false, registered = false;
    std::uint64_t samples = 0;
    RE::NiTransform physicsFrame, sampleFrame;
    RE::NiPoint3 previousFrameVelocity{};
    // Repair (whole body): Body::sound holds a shape to go back to; BeforePhysics writes it next.
    bool haveSound = false, repairPending = false;
    std::uint64_t repairSample = 0;  // the first physics sample taken after a repair was written
    float clock = 0;
    std::array<float, repairLimit> repairAt{};
    unsigned repairNext = 0;
    // [Debug] bRagdollTrace (core/RagdollTrace): the owned bodies in recording order and their
    // per-step states, sized at Enable so the physics callback never allocates.
    RagdollTrace::Recorder trace;
    std::vector<std::size_t> traceBodies;
    std::vector<RagdollTrace::BodyState> traceStates;

    // What Havok holds once the carry values are applied: the calibration replays these.
    void BeginTrace() {
        const float inv = RE::bhkWorld::GetWorldScaleInverse();
        RagdollTrace::Header h;
        h.rig = request.traceRig;
        h.profile = request.traceProfile;
        h.anchor = request.anchor.c_str();
        h.actorScale = scene->world.scale;
        h.follow = request.dynamics.follow;
        const auto& g = nativeWorld->gravity.quad.m128_f32;
        h.gravity = {g[0] * inv, g[1] * inv, g[2] * inv};
        const auto& solver = nativeWorld->dynamicsStepInfo.solverInfo;
        h.solverTau = solver.tau;
        h.solverDamping = solver.damping;
        h.solverSteps = solver.numSteps;
        h.solverMicroSteps = solver.numMicroSteps;
        traceBodies.clear();
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto& b = bodies[i];
            if (!b.owned) continue;
            const auto& motion = b.rigid->motion;
            const auto& state = motion.motionState;
            const auto& inverse = motion.inertiaAndMassInv.quad.m128_f32;
            const auto& center = state.sweptTransform.centerOfMassLocal.quad.m128_f32;
            RagdollTrace::BodyInfo info;
            info.bone = b.node->name.c_str();
            info.simulated = b.selected;
            info.mass = motion.GetMass();
            for (unsigned k = 0; k < 3; ++k) info.inertia[k] = inverse[k] > 0 ? inv * inv / inverse[k] : 0.0f;
            info.centerOfMass = {center[0] * inv, center[1] * inv, center[2] * inv};
            info.linearDamping = static_cast<float>(state.linearDamping);
            info.angularDamping = static_cast<float>(state.angularDamping);
            info.gravityFactor = b.selected ? b.gravityMin : 1.0f;  // native 1.0 plus extraGravity
            info.timeFactor = static_cast<float>(state.timeFactor);
            info.maxLinearVelocityCode = state.maxLinearVelocity.value;
            info.maxAngularVelocityCode = state.maxAngularVelocity.value;
            h.bodies.push_back(std::move(info));
            traceBodies.push_back(i);
        }
        for (const auto& j : joints) {
            if (!j.data) continue;
            const auto limits = Limits(j.data->atoms);
            h.joints.push_back({bodies[j.a].node->name.c_str(), bodies[j.b].node->name.c_str(),
                j.data->atoms.angFriction.maxFrictionTorque * inv * inv, *limits[0], {*limits[1], *limits[2]},
                {*limits[3], *limits[4]}});
        }
        h.widened = widened;
        traceStates.assign(traceBodies.size(), {});
        logger::info("[RagdollTrace] recording rig={} bodies={} joints={} gravity={:.1f} solver tau={:.2f} "
                     "damping={:.2f} steps={} microSteps={}",
            h.rig, h.bodies.size(), h.joints.size(), h.gravity[2], h.solverTau, h.solverDamping, h.solverSteps,
            h.solverMicroSteps);
        trace.Begin(std::move(h), traceSteps);
    }
    // Main thread, after Unlisten and outside the world lock: a long trace takes a while to write.
    void FlushTrace() {
        if (!trace.Active()) return;
        if (trace.Steps()) {
            const auto file = RagdollTrace::NextFile(request.traceRig);
            const auto name = file.u8string();
            if (!file.empty() && trace.Write(file))
                logger::info("[RagdollTrace] steps={} dropped={} file={}", trace.Steps(), trace.Dropped(),
                    reinterpret_cast<const char*>(name.c_str()));
            else
                logger::warn("[RagdollTrace] could not write {} steps to {}", trace.Steps(),
                    reinterpret_cast<const char*>(name.c_str()));
        }
        trace.Clear();
    }

    // This callback copies completed poses. It never traverses scene nodes, resolves actors, calls
    // gameplay APIs or allocates memory.
    void PostSimulationCallback(RE::hkpWorld* completed) override {
        std::lock_guard lock(samplesMutex);
        if (completed != nativeWorld.get() || !listening) return;
        for (auto& b : bodies) {
            if (!b.owned) continue;
            if (b.rigid->world != completed) {
                invalid = true;
                return;
            }
            b.sample = ReadPose(*b.rigid);
            if (!Finite(b.sample)) {
                invalid = true;
                return;
            }
        }
        sampleFrame = physicsFrame;
        haveSample = true;
        ++samples;
        if (trace.Active()) {
            const float inv = RE::bhkWorld::GetWorldScaleInverse();
            for (std::size_t k = 0; k < traceBodies.size(); ++k) {
                const auto& b = bodies[traceBodies[k]];
                const auto& v = b.rigid->motion.linearVelocity.quad.m128_f32;
                const auto& w = b.rigid->motion.angularVelocity.quad.m128_f32;
                traceStates[k] = {TracePose(b.sample), {v[0] * inv, v[1] * inv, v[2] * inv}, {w[0], w[1], w[2]}};
            }
            trace.Add(completed->dynamicsStepInfo.stepInfo.deltaTime, TracePose(physicsFrame), traceStates);
        }
    }
    bool Current() const {
        auto target = request.target.get();
        auto carrier = request.carrier.get();
        return target && carrier && target->Is3DLoaded() && carrier->Is3DLoaded() &&
               target->Get3D(false) == scene.get() && target->GetParentCell() && carrier->GetParentCell() &&
               target->GetParentCell()->GetbhkWorld() == world.get() &&
               carrier->GetParentCell()->GetbhkWorld() == world.get();
    }
    void Unlisten() {
        if (!listening || !nativeWorld) return;
        // Caller owns the world's write lock, so callbacks cannot race removal.
        auto& listeners = nativeWorld->worldPostSimulationListeners;
        for (int i = 0; i < listeners.size(); ++i)
            if (listeners[i] == this) {
                for (int j = i + 1; j < listeners.size(); ++j) listeners[j - 1] = listeners[j];
                listeners.resize(listeners.size() - 1);
                break;
            }
        listening = false;
    }
    bool SetLayers(bool activate) {
        return PartialRagdollPolicy::ApplyLayerPlan(
            bodies,
            [activate](const Body& b, RE::COL_LAYER current) {
                if (!b.owned || !b.changed) return current;
                return activate ? carryLayer : current == carryLayer ? b.layer : current;
            },
            [](const Body& b) { return b.rigid->collidable.GetCollisionLayer(); },
            [](Body& b, RE::COL_LAYER layer) { b.node->SetCollisionLayer(layer); });
    }
    void RestoreModes(bool yieldToEngine) {
        RestoreJoints();
        // Snapshot current equipment layers before recursive body-layer restoration.
        if (!SetLayers(false)) logger::error("[CarryPartial] layer restoration verification failed");
        for (auto& b : bodies)
            if (b.changed) {
                b.RestoreOverrides();
                if (b.rigid->motion.type.get() == b.applied)
                    b.node->SetMotionType(yieldToEngine ? Motion::kDynamic : b.original, false, true, true);
                const auto flags = b.collision->flags.underlying();
                b.collision->flags = static_cast<RE::bhkNiCollisionObject::Flag>(
                    PartialRagdollPolicy::RestoreBits(flags, b.flags, b.writtenFlags, syncMask));
                if (!yieldToEngine) {
                    b.wrapper->SetLinearVelocity({0, 0, 0, 0});
                    b.wrapper->SetAngularVelocity({0, 0, 0, 0});
                }
                b.changed = false;
            }
    }
};

Session::Session() : state_(std::make_unique<State>()) {}
Session::~Session() { Invalidate(EndReason::kActorRemoved); }

Result Session::Prepare(const Request& request) {
    auto& s = *state_;
    if (s.status.phase != Phase::kIdle) return Result::kBusy;
    auto fail = [&](Result why) {
        s.status.result = why;
        logger::warn(
            "[CarryPartial] prepare rejected result={} mappedBodies={}", static_cast<int>(why), s.bodies.size());
        return why;
    };
    auto target = request.target.get();
    auto carrier = request.carrier.get();
    if (!target || !carrier || target == carrier || target->IsDead() || target->IsInRagdollState())
        return fail(Result::kInvalidActor);
    if (!target->Is3DLoaded() || !carrier->Is3DLoaded()) return fail(Result::kSceneUnavailable);
    auto* cell = target->GetParentCell();
    auto* other = carrier->GetParentCell();
    auto* world = cell ? cell->GetbhkWorld() : nullptr;
    if (!world || !other || other->GetbhkWorld() != world || !world->GetWorld1()) return fail(Result::kWorldMismatch);
    s.request = request;
    {
        auto& d = s.request.dynamics;
        const auto clamp = [](float value, float low, float high, float fallback) {
            return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
        };
        d.linearDamping = clamp(d.linearDamping, 0, 10, Dynamics{}.linearDamping);
        d.angularDamping = clamp(d.angularDamping, 0, 10, Dynamics{}.angularDamping);
        d.gravity = clamp(d.gravity, 1, 4, 1);
        d.inertia = clamp(d.inertia, 1, 10, 1);
        d.follow = clamp(d.follow, 0, 1, 0);
        d.jointFriction =
            std::isfinite(d.jointFriction) && d.jointFriction >= 0 ? std::min(d.jointFriction, 100.0f) : -1.0f;
    }
    s.scene.reset(target->Get3D(false));
    s.world.reset(world);
    s.nativeWorld = RE::hkRefPtr<RE::hkpWorld>(world->GetWorld1());
    if (!target->GetAnimationGraphManager(s.manager) || !s.manager) return fail(Result::kSceneUnavailable);
    {
        RE::BSSpinLockGuard guard(s.manager->GetRuntimeData().updateLock);
        for (const auto& graph : s.manager->graphs)
            if (graph && graph->characterInstance.ragdollDriver && graph->characterInstance.ragdollDriver->ragdoll) {
                if (s.graph) return fail(Result::kUnsupportedSkeleton);
                s.graph = graph;
            }
    }
    if (!s.graph) return fail(Result::kUnsupportedSkeleton);
    const auto& anchorName = request.anchor;
    const auto* anchorNode = anchorName.empty() ? nullptr : s.scene->GetObjectByName(anchorName);
    if (!anchorNode) {
        logger::warn("[CarryPartial] anchor '{}' not found", anchorName.c_str());
        return fail(Result::kUnsupportedSkeleton);
    }
    RE::BSReadLockGuard lock(world->worldLock);
    const auto* filter = RE::bhkCollisionFilter::GetSingleton();
    if (!filter || !PartialRagdollPolicy::LayerAllowsSelf(
                       filter->layerBitfields[static_cast<unsigned>(carryLayer)], static_cast<unsigned>(carryLayer))) {
        logger::warn("[CarryPartial] native body self-collision unavailable layer={} mask={:016X}",
            static_cast<unsigned>(carryLayer), filter ? filter->layerBitfields[static_cast<unsigned>(carryLayer)] : 0);
        return fail(Result::kUnsupportedSkeleton);
    }
    const auto* ragdoll = s.graph->characterInstance.ragdollDriver->ragdoll;
    logger::debug("[CarryPartial] inspect target={:08X} nativeBodies={} constraints={}", target->GetFormID(),
        ragdoll->rigidBodies.size(), ragdoll->constraints.size());
    if (ragdoll->rigidBodies.size() < 2 || ragdoll->rigidBodies.size() > 128) return fail(Result::kUnsupportedSkeleton);
    unsigned budget = 4096;
    bool unsupported = false;
    bool engineSimulating = false;  // the target's own ragdoll bodies are dynamic: the engine simulates them
    // Who moves the target's bodies now (PartialRagdollPolicy::SimulatedByOther).
    const auto knock = target->AsActorState()->GetKnockState();
    const bool knocked = knock != RE::KNOCK_STATE_ENUM::kNormal;
    unsigned dynamicInWorld = 0, dynamicOutside = 0;
    for (const auto* r : ragdoll->rigidBodies)
        if (r && r->motion.type.get() != Motion::kKeyframed && r->motion.type.get() != Motion::kFixed)
            ++(r->world ? dynamicInWorld : dynamicOutside);
    float farthestDynamic = 0;  // how far a taken dynamic body lay from its bone: ~0 = something drove it
    auto visit = [&](auto&& self, RE::NiAVObject* node, unsigned depth) -> void {
        if (!node || unsupported) return;
        if (!budget-- || depth > 128) {
            unsupported = true;
            return;
        }
        if (node->collisionObject) {
            auto* c = node->collisionObject->AsBhkNiCollisionObject();
            auto* w = c && c->body ? c->body->AsBhkRigidBody() : nullptr;
            auto* r = w ? w->GetRigidBody() : nullptr;
            if (c && c->body && !r) {
                logger::warn("[CarryPartial] unsupported non-rigid collision bone={}", node->name.c_str());
                unsupported = true;
                return;
            }
            if (r) {
                const bool owned = std::find(ragdoll->rigidBodies.begin(), ragdoll->rigidBodies.end(), r) !=
                                   ragdoll->rigidBodies.end();
                if (r->world && r->world != s.nativeWorld.get()) {
                    logger::warn("[CarryPartial] foreign world bone={}", node->name.c_str());
                    unsupported = true;
                    return;
                }
                const bool dynamic =
                    r->motion.type.get() != Motion::kKeyframed && r->motion.type.get() != Motion::kFixed;
                if (PartialRagdollPolicy::SimulatedByOther(owned, r->world != nullptr, dynamic, knocked)) {
                    logger::warn("[CarryPartial] existing dynamic owner bone={} motion={}", node->name.c_str(),
                        static_cast<unsigned>(r->motion.type.get()));
                    unsupported = true;
                    engineSimulating = true;
                    return;
                }
                State::Body b;
                b.node.reset(node);
                b.collision.reset(c);
                b.wrapper.reset(w);
                b.rigid = RE::hkRefPtr<RE::hkpRigidBody>(r);
                b.owned = owned;
                b.part = static_cast<unsigned>(r->collidable.broadPhaseHandle.collisionFilterInfo.GetBipedPart());
                if (b.part >= 32) {
                    unsupported = true;
                    return;
                }
                b.anchor = owned && node == anchorNode;
                b.selected = PartialRagdollPolicy::Selected(owned, b.anchor);
                b.original = r->motion.type.get();
                b.layer = r->collidable.GetCollisionLayer();
                b.flags = c->flags.underlying();
                b.local = node->local;
                if (owned) {
                    if (std::any_of(s.bodies.begin(), s.bodies.end(),
                            [&](const auto& previous) { return previous.owned && previous.rigid.get() == r; })) {
                        unsupported = true;
                        return;
                    }
                    // PartialRagdollPolicy::FrameFromBone: a keyframed body in the world sits on the
                    // pose, read its actual frame; otherwise the bone's (an ambient creature's idle ragdoll
                    // outside the world, a hit reaction's leftover).
                    const auto stale = ReadPose(*r);
                    auto bodyPose = stale;
                    if (PartialRagdollPolicy::FrameFromBone(r->world != nullptr, dynamic)) {
                        if (OffsetBody(w)) {
                            logger::warn(
                                "[CarryPartial] offset body without a usable frame bone={}", node->name.c_str());
                            unsupported = true;
                            return;
                        }
                        bodyPose = node->world;
                        bodyPose.scale = 1;
                        if (dynamic)
                            farthestDynamic =
                                std::max(farthestDynamic, (stale.translate - node->world.translate).Length());
                    }
                    if (!Finite(node->world) || !Finite(bodyPose)) {
                        unsupported = true;
                        return;
                    }
                    b.offset = node->world.Invert() * bodyPose;
                    logger::debug(
                        "[CarryPartial] frame bone={} inWorld={} nodeScale={:.3f} staleDistance={:.2f} offset=({:.2f},{:.2f},{:.2f}) offsetScale={:.3f}",
                        node->name.c_str(), r->world != nullptr, node->world.scale,
                        (stale.translate - node->world.translate).Length(), b.offset.translate.x, b.offset.translate.y,
                        b.offset.translate.z, b.offset.scale);
                } else
                    logger::debug("[CarryPartial] equipment excluded from simulation bone={}", node->name.c_str());
                s.bodies.push_back(std::move(b));
            }
        }
        if (auto* n = node->AsNode())
            for (const auto& child : n->GetChildren()) self(self, child.get(), depth + 1);
    };
    visit(visit, s.scene.get(), 0);
    const auto ownedCount = std::count_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.owned; });
    if (engineSimulating) {
        logger::warn("[CarryPartial] {} of {} target bodies dynamic in the world ({} dynamic outside) knock={}",
            dynamicInWorld, ragdoll->rigidBodies.size(), dynamicOutside, static_cast<int>(knock));
        return fail(Result::kForeignPhysicsOwner);
    }
    if (dynamicInWorld || dynamicOutside)
        logger::info(
            "[CarryPartial] {} of {} target bodies dynamic ({} in the world, {} outside) knock={}, farthest {:.1f} "
            "from its bone; taken over",
            dynamicInWorld + dynamicOutside, ragdoll->rigidBodies.size(), dynamicInWorld, dynamicOutside,
            static_cast<int>(knock), farthestDynamic);
    if (unsupported || ownedCount != ragdoll->rigidBodies.size()) {
        logger::warn("[CarryPartial] mapping rejected traversal={} mapped={} expected={}", !unsupported, ownedCount,
            ragdoll->rigidBodies.size());
        return fail(Result::kUnsupportedSkeleton);
    }
    // Exactly one owned body is the anchor.
    const auto anchors = std::count_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.anchor; });
    if (anchors != 1) {
        logger::warn(
            "[CarryPartial] anchor '{}' is not exactly one owned ragdoll body (found {})", anchorName.c_str(), anchors);
        return fail(Result::kUnsupportedSkeleton);
    }
    // Every selected chain must reach an anchored body through actual ragdoll
    // constraints. An isolated limb with only internal joints is not sufficient.
    if (ragdoll->constraints.size() > 256) return fail(Result::kUnsupportedSkeleton);
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    s.joints.clear();
    for (auto* constraint : ragdoll->constraints) {
        if (!constraint) return fail(Result::kUnsupportedSkeleton);
        const auto find = [&](const RE::hkpRigidBody* body) {
            return std::find_if(
                s.bodies.begin(), s.bodies.end(), [&](const auto& b) { return b.owned && b.rigid.get() == body; });
        };
        const auto a = find(constraint->GetRigidBodyA()), b = find(constraint->GetRigidBodyB());
        if (a == s.bodies.end() || b == s.bodies.end()) return fail(Result::kUnsupportedSkeleton);
        edges.emplace_back(
            static_cast<std::size_t>(a - s.bodies.begin()), static_cast<std::size_t>(b - s.bodies.begin()));
        State::Joint joint;
        joint.instance = RE::hkRefPtr<RE::hkpConstraintInstance>(constraint);
        if (constraint->data && constraint->data->GetType() == RE::hkpConstraintData::ConstraintType::kRagdoll)
            joint.data = static_cast<RE::hkpRagdollConstraintData*>(constraint->data);
        else if (constraint->data &&
                 constraint->data->GetType() == RE::hkpConstraintData::ConstraintType::kLimitedHinge)
            joint.hinge = static_cast<RE::hkpLimitedHingeConstraintData*>(constraint->data);
        joint.a = edges.back().first;
        joint.b = edges.back().second;
        s.joints.push_back(std::move(joint));
    }
    // Rig profile entries name bodies by bone, matched once here.
    const auto named = [&](const std::string& bone) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < s.bodies.size(); ++i)
            if (s.bodies[i].owned && _stricmp(s.bodies[i].node->name.c_str(), bone.c_str()) == 0) return i;
        return std::nullopt;
    };
    for (const auto& t : s.request.bodies)
        if (const auto i = named(t.bone); i && !t.simulate && !s.bodies[*i].anchor) s.bodies[*i].selected = false;
    if (!PartialRagdollPolicy::ConnectedToAnchors(s.bodies, edges)) return fail(Result::kUnsupportedSkeleton);
    {
        const auto body = [&](const RE::NiAVObject* node, bool selectedOnly) {
            return std::any_of(s.bodies.begin(), s.bodies.end(),
                [&](const auto& b) { return b.owned && b.node.get() == node && (!selectedOnly || b.selected); });
        };
        const auto ancestor = [&](const RE::NiAVObject* node, bool selectedOnly) {
            for (const auto* p = node->parent; p && p != s.scene.get(); p = p->parent)
                if (body(p, selectedOnly)) return true;
            return false;
        };
        const auto anchor = std::find_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.anchor; });
        for (auto& b : s.bodies) {
            if (!b.owned) continue;
            b.inScene = s.scene->world.Invert() * b.node->world;
            b.pinned = b.anchor && ancestor(b.node.get(), true);
            b.hanging = b.selected && !ancestor(b.node.get(), false);
            if (b.hanging && anchor != s.bodies.end())
                b.reach = (b.node->world.translate - anchor->node->world.translate).Length();
            if (b.pinned || b.hanging)
                logger::debug("[CarryPartial] {} bone={} reach={:.2f}", b.pinned ? "pinned anchor" : "hanging root",
                    b.node->name.c_str(), b.reach);
        }
    }
    for (auto& b : s.bodies) {
        const auto& d = s.request.dynamics;
        b.drag = d.linearDamping;
        b.spin = d.angularDamping;
        b.gravityMin = d.gravity;
        b.inertiaScale = d.inertia;
    }
    // The owned body nearest above a node, if any.
    const auto bodyAbove = [&](const RE::NiAVObject* node) -> std::optional<std::size_t> {
        for (const auto* p = node->parent; p && p != s.scene.get(); p = p->parent)
            for (std::size_t i = 0; i < s.bodies.size(); ++i)
                if (s.bodies[i].owned && s.bodies[i].node.get() == p) return i;
        return std::nullopt;
    };
    for (const auto& t : s.request.bodies) {
        const auto i = named(t.bone);
        if (!i) {
            logger::warn("[RagdollProfile] bone '{}' has no ragdoll body; entry ignored", t.bone);
            continue;
        }
        auto& b = s.bodies[*i];
        if (!t.simulate && b.anchor)
            logger::warn("[RagdollProfile] bone '{}' is the carry anchor; simulate ignored", t.bone);
        if (t.linearDamping) b.drag = *t.linearDamping;
        if (t.angularDamping) b.spin = *t.angularDamping;
        if (t.gravity) b.gravityMin = *t.gravity;
        if (t.inertia) b.inertiaScale = *t.inertia;
        if (!t.friction && !t.cone && !t.plane && !t.twist && t.swingFrom.empty()) continue;
        const auto above = bodyAbove(b.node.get());
        const auto joint = !above ? s.joints.end() : std::find_if(s.joints.begin(), s.joints.end(), [&](const auto& j) {
            return j.data && ((j.a == *i && j.b == *above) || (j.b == *i && j.a == *above));
        });
        if (joint == s.joints.end()) {
            logger::warn(
                "[RagdollProfile] bone '{}' has no ragdoll joint to a body above; joint values ignored", t.bone);
            continue;
        }
        joint->friction = t.friction;
        joint->cone = t.cone;
        joint->plane = t.plane;
        joint->twist = t.twist;
        if (t.swingFrom.empty()) continue;
        RE::NiAVObject* root = nullptr;
        for (auto* p = b.node->parent; p && p != s.bodies[*above].node.get(); p = p->parent)
            if (_stricmp(p->name.c_str(), t.swingFrom.c_str()) == 0) {
                root = p;
                break;
            }
        if (!root || !b.selected) {
            logger::warn("[RagdollProfile] swingFrom '{}' of '{}' ignored: {}", t.swingFrom, t.bone,
                root ? "the body does not simulate" : "not a bone between it and the body above");
            continue;
        }
        const auto& parent = s.bodies[*above].node->world;
        b.limbRoot.reset(root);
        b.limbRootLocal = root->local;
        b.limbRootInParent = parent.Invert() * root->world;
        b.nodeInParent = parent.Invert() * b.node->world;
        b.parentBody = *above;
        joint->limb = *i;
        logger::debug("[CarryPartial] limb bone={} root={} parent={} length={:.2f} jointShared={}",
            b.node->name.c_str(), root->name.c_str(), s.bodies[*above].node->name.c_str(),
            (b.node->world.translate - root->world.translate).Length(), joint->data->referenceCount > 1);
    }
    s.status.selectedBodies = static_cast<std::uint32_t>(
        std::count_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.selected; }));
    s.status.anchoredBodies = static_cast<std::uint32_t>(ownedCount) - s.status.selectedBodies;
    s.status.phase = Phase::kReady;
    s.status.result = Result::kReady;
    logger::info("[CarryPartial] prepared target={:08X} anchor='{}' selected={} anchored={}", target->GetFormID(),
        anchorName.c_str(), s.status.selectedBodies, s.status.anchoredBodies);
    for (const auto& b : s.bodies)
        if (b.owned)
            logger::debug("[CarryPartial] body bone={} part={} selected={} anchor={} inWorld={}", b.node->name.c_str(),
                b.part, b.selected, b.anchor, b.rigid->world != nullptr);
    return Result::kReady;
}

Result Session::Enable() {
    auto& s = *state_;
    if (s.status.phase != Phase::kReady || !s.Current()) return Result::kSceneUnavailable;
    RE::BSWriteLockGuard lock(s.world->worldLock);
    if (s.nativeWorld->criticalOperationsLockCount) return Result::kUnsafeUpdatePath;
    const auto* nativeFilter = RE::bhkCollisionFilter::GetSingleton();
    if (!nativeFilter ||
        !PartialRagdollPolicy::LayerAllowsSelf(
            nativeFilter->layerBitfields[static_cast<unsigned>(carryLayer)], static_cast<unsigned>(carryLayer)))
        return Result::kUnsupportedSkeleton;
    auto failed = [&](Result result) {
        s.Unlisten();
        s.RestoreModes(false);
        if (s.registered) {
            s.graph->RemoveRagdollFromWorld();
            s.registered = false;
        }
        s.status.phase = Phase::kFaulted;
        s.status.result = result;
        return result;
    };
    bool anyWorld = false, allWorld = true;
    for (const auto& b : s.bodies)
        if (b.owned) {
            anyWorld |= b.rigid->world != nullptr;
            allWorld &= b.rigid->world == s.nativeWorld.get();
        }
    if (anyWorld && !allWorld) return Result::kWorldMismatch;
    if (!anyWorld) {
        // Registration is owned only if every body was absent before this call.
        s.registered = true;
        if (!s.graph->AddRagdollToWorld()) return failed(Result::kActivationFailed);
    }
    for (const auto& b : s.bodies)
        if (b.owned && b.rigid->world != s.nativeWorld.get()) return failed(Result::kWorldMismatch);
    // Recursive layer setters can touch descendants before their own turn.
    // Include every captured body in rollback, even after partial activation.
    for (auto& b : s.bodies)
        if (b.owned) {
            b.changed = true;
            b.applied = b.rigid->motion.type.get();
            b.writtenFlags = b.collision->flags.underlying();
        }
    const bool applied = PartialRagdollPolicy::Transaction(
        static_cast<unsigned>(s.bodies.size()),
        [&](unsigned i) {
            auto& b = s.bodies[i];
            if (!b.owned) return true;
            b.changed = true;
            b.applied = b.selected ? Motion::kDynamic : Motion::kKeyframed;
            b.writtenFlags = b.collision->flags.underlying() & ~syncMask;
            b.collision->flags = static_cast<RE::bhkNiCollisionObject::Flag>(b.writtenFlags);
            b.node->SetMotionType(b.applied, false, true, true);
            const auto actual = b.rigid->motion.type.get();
            b.applied = actual;
            const bool dynamic = actual == Motion::kDynamic || actual == Motion::kSphereInertia ||
                                 actual == Motion::kBoxInertia || actual == Motion::kThinBoxInertia;
            if ((b.selected && !dynamic) || (!b.selected && actual != Motion::kKeyframed)) return false;
            if (b.selected) {
                auto& motion = b.rigid->motion.motionState;
                if (!b.linearDamping.Apply(motion.linearDamping, b.drag) ||
                    !b.angularDamping.Apply(motion.angularDamping, b.spin) ||
                    // Defaults leave the native inertia untouched.
                    (b.inertiaScale > 1 && !b.ScaleInertia(b.inertiaScale)))
                    return false;
                b.extraGravity = Vector(s.nativeWorld->gravity) * (b.gravityMin - 1.0f);
            }
            b.applied = actual;
            WritePose(*b.wrapper, b.node->world * b.offset);
            b.wrapper->SetLinearVelocity({0, 0, 0, 0});
            b.wrapper->SetAngularVelocity({0, 0, 0, 0});
            return true;
        },
        [&](unsigned) { /* Rollback below also restores recursive layer changes. */ });
    if (!applied || !s.SetLayers(true)) return failed(Result::kActivationFailed);
    // Joints: the bodies now sit at the poses written above (node * offset).
    const auto& tuning = s.request.dynamics;
    for (auto& j : s.joints) {
        if (!j.data) continue;
        auto& atoms = j.data->atoms;
        auto& friction = atoms.angFriction.maxFrictionTorque;
        // A profile joint value replaces the friction; otherwise the Motion cap lowers it.
        const float wanted = j.friction                                                     ? *j.friction
                             : tuning.jointFriction >= 0 && friction > tuning.jointFriction ? tuning.jointFriction
                                                                                            : friction;
        if (std::isfinite(friction) && wanted != friction) {
            j.frictionBefore = friction;
            friction = wanted;
            j.frictionApplied = friction;
            j.frictionOwned = true;
        }
        if (j.cone || j.plane || j.twist) {
            const auto limits = Limits(atoms);
            for (unsigned k = 0; k < limits.size(); ++k) j.limitsBefore[k] = *limits[k];
            if (j.cone) *limits[0] = *j.cone;
            if (j.plane) *limits[1] = j.plane->first, *limits[2] = j.plane->second;
            if (j.twist) *limits[3] = j.twist->first, *limits[4] = j.twist->second;
            for (unsigned k = 0; k < limits.size(); ++k) j.limitsApplied[k] = *limits[k];
            j.limitsOwned = true;
        }
        if (j.limb == SIZE_MAX) continue;
        // Both pivot frames move to the chain root; their rotations (the limit axes) stay.
        const auto root = s.bodies[j.limb].limbRoot->world.translate;
        RE::NiPoint3 pivots[2];
        for (unsigned side = 0; side < 2; ++side) {
            const auto& body = s.bodies[side ? j.b : j.a];
            pivots[side] = (body.node->world * body.offset).Invert() * root * RE::bhkWorld::GetWorldScale();
            if (!std::isfinite(pivots[side].x) || !std::isfinite(pivots[side].y) || !std::isfinite(pivots[side].z))
                return failed(Result::kActivationFailed);
        }
        for (unsigned side = 0; side < 2; ++side) {
            auto& t = side ? atoms.transforms.transformB : atoms.transforms.transformA;
            j.before[side] = t;
            t.translation = RE::hkVector4{pivots[side].x, pivots[side].y, pivots[side].z, 0};
            j.applied[side] = t.translation;
        }
        j.pivotOwned = true;
    }
    // Limits the start pose is already past: open them to it. Joints between two keyframed bodies
    // hold nothing and stay as they are.
    s.widened.clear();
    static constexpr const char* ragdollLimits[5]{"cone", "planeMin", "planeMax", "twistMin", "twistMax"};
    for (auto& j : s.joints) {
        const auto& a = s.bodies[j.a];
        const auto& b = s.bodies[j.b];
        if ((!j.data && !j.hinge) || (!a.selected && !b.selected)) continue;
        const auto bodyA = (a.node->world * a.offset).rotate, bodyB = (b.node->world * b.offset).rotate;
        const auto note = [&](const char* limit, float from, float to) {
            s.widened.push_back({a.node->name.c_str(), b.node->name.c_str(), limit, from, to});
            logger::info("[CarryPartial] start pose past joint {}-{} {}: {:.1f} -> {:.1f}", a.node->name.c_str(),
                b.node->name.c_str(), limit, Degrees(from), Degrees(to));
        };
        if (j.hinge) {
            auto& limit = j.hinge->atoms.angLimit;
            if (limit.limitAxis != 0) continue;
            const auto& t = j.hinge->atoms.transforms;
            const float angle =
                CarryJointAngles::HingeAngle(bodyA, FrameRotation(t.transformA), bodyB, FrameRotation(t.transformB));
            const auto wide = CarryJointAngles::Widen(std::pair{limit.minAngle, limit.maxAngle}, angle, startPad);
            if (wide.first == limit.minAngle && wide.second == limit.maxAngle) continue;
            j.hingeBefore[0] = limit.minAngle;
            j.hingeBefore[1] = limit.maxAngle;
            if (wide.first != limit.minAngle) note("hingeMin", limit.minAngle, wide.first);
            if (wide.second != limit.maxAngle) note("hingeMax", limit.maxAngle, wide.second);
            limit.minAngle = j.hingeApplied[0] = wide.first;
            limit.maxAngle = j.hingeApplied[1] = wide.second;
            j.hingeOwned = true;
            continue;
        }
        auto& atoms = j.data->atoms;
        if (!StandardAxes(atoms)) {
            logger::warn("[CarryPartial] joint {}-{} measures other axes; start pose not checked", a.node->name.c_str(),
                b.node->name.c_str());
            continue;
        }
        const auto start = CarryJointAngles::RagdollAngles(
            bodyA, FrameRotation(atoms.transforms.transformA), bodyB, FrameRotation(atoms.transforms.transformB));
        const auto limits = Limits(atoms);
        std::array<float, 5> now;
        for (unsigned k = 0; k < limits.size(); ++k) now[k] = *limits[k];
        const auto wide = CarryJointAngles::Widen(now, start, startPad);
        if (wide == now) continue;
        if (!j.limitsOwned)
            for (unsigned k = 0; k < limits.size(); ++k) j.limitsBefore[k] = now[k];
        for (unsigned k = 0; k < limits.size(); ++k) {
            if (wide[k] != now[k]) note(ragdollLimits[k], now[k], wide[k]);
            *limits[k] = j.limitsApplied[k] = wide[k];
        }
        j.limitsOwned = true;
    }
    for (auto& j : s.joints)
        if (j.data) {
            const auto limits = Limits(j.data->atoms);
            float before[5], after[5];
            for (unsigned k = 0; k < limits.size(); ++k) {
                after[k] = Degrees(*limits[k]);
                before[k] = j.limitsOwned ? Degrees(j.limitsBefore[k]) : after[k];
            }
            logger::debug(
                "[CarryPartial] joint {}-{} friction={:.2f}->{:.2f} cone={:.1f}->{:.1f} plane=({:.1f},{:.1f})->({:.1f},{:.1f}) twist=({:.1f},{:.1f})->({:.1f},{:.1f}) pivot={}",
                s.bodies[j.a].node->name.c_str(), s.bodies[j.b].node->name.c_str(),
                j.frictionOwned ? j.frictionBefore : j.data->atoms.angFriction.maxFrictionTorque,
                j.data->atoms.angFriction.maxFrictionTorque, before[0], after[0], before[1], before[2], after[1],
                after[2], before[3], before[4], after[3], after[4],
                j.pivotOwned ? s.bodies[j.limb].limbRoot->name.c_str() : "native");
        } else if (j.hinge)
            logger::debug("[CarryPartial] hinge {}-{} limits=({:.1f},{:.1f})", s.bodies[j.a].node->name.c_str(),
                s.bodies[j.b].node->name.c_str(), Degrees(j.hinge->atoms.angLimit.minAngle),
                Degrees(j.hinge->atoms.angLimit.maxAngle));
        else if (j.instance && j.instance->data)
            logger::debug("[CarryPartial] joint {}-{} type={} (limits not read)", s.bodies[j.a].node->name.c_str(),
                s.bodies[j.b].node->name.c_str(), static_cast<int>(j.instance->data->GetType()));
    // Preserve native system groups, part masks and joint exclusions. Never
    // modify the global collision table to force contacts for one carried actor.
    const auto* filter = RE::bhkCollisionFilter::GetSingleton();
    for (const auto& b : s.bodies)
        if (b.selected) {
            const auto& info = b.rigid->collidable.broadPhaseHandle.collisionFilterInfo;
            logger::debug(
                "[CarryPartial] dynamic collision bone={} part={} filter={:08X} bipedMask={:08X} layerMask={:016X} mass={:.2f} dampingLinear={:.3f}->{:.3f} dampingAngular={:.3f}->{:.3f} gravity={:.2f} inertia x{:.2f}",
                b.node->name.c_str(), b.part, info.filter, filter ? filter->bipedBitfields[b.part] : 0,
                filter ? filter->layerBitfields[static_cast<unsigned>(carryLayer)] : 0, b.rigid->motion.GetMass(),
                b.linearDamping.before, b.linearDamping.applied, b.angularDamping.before, b.angularDamping.applied,
                b.gravityMin, b.inertiaOwned ? b.inertiaScale : 1.0f);
        }
    auto& listeners = s.nativeWorld->worldPostSimulationListeners;
    if (listeners.size() < 0 || listeners.size() > 1024) return failed(Result::kUnsafeUpdatePath);
    s.physicsFrame = s.scene->world;
    s.sampleFrame = s.physicsFrame;
    s.previousFrameVelocity = {};
    // The start pose is the first sound shape: the bodies sit at node * offset (written above).
    s.haveSound = s.repairPending = false;
    s.repairSample = 0;
    s.clock = 0;
    s.repairAt.fill(-repairWindow - 1);
    s.repairNext = 0;
    s.status.repairs = 0;
    if (const auto anchor = std::find_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.anchor; });
        anchor != s.bodies.end()) {
        auto base = anchor->node->world * anchor->offset;
        base.scale = 1;
        for (auto& b : s.bodies)
            if (b.owned) {
                auto body = b.node->world * b.offset;
                body.scale = 1;
                b.sound = base.Invert() * body;
            }
        s.haveSound = true;
    }
    s.haveSample = false;
    s.invalid = false;
    s.samples = 0;
    if (s.request.trace) s.BeginTrace();
    s.listening = true;
    listeners.push_back(&s);
    s.status.phase = Phase::kSimulating;
    logger::info("[CarryPartial] enabled generation={} registered={}", s.request.carryGeneration, s.registered);
    return Result::kReady;
}

void Session::BeforePhysics(float delta) {
    auto& s = *state_;
    if ((s.status.phase != Phase::kSimulating && s.status.phase != Phase::kReady) || !s.Current()) return;
    RE::BSWriteLockGuard lock(s.world->worldLock);
    // Main-thread writes occur between native steps, protected by the same world
    // lock as the engine. PostSimulation is our evidence of actual Havok progress.
    if (s.nativeWorld->criticalOperationsLockCount) return;
    const auto frame = s.scene->world;
    if (!Finite(frame) || std::abs(frame.scale - s.physicsFrame.scale) > 0.001f ||
        (frame.translate - s.physicsFrame.translate).Length() > 300) {
        logger::warn("[CarryPartial] attachment frame discontinuity");
        s.status.phase = Phase::kFaulted;
        return;
    }
    const auto velocity = delta > 0.00001f ? (frame.translate - s.physicsFrame.translate) / delta : RE::NiPoint3{};
    // How far a held body's keyframe jumps this frame: the whole attachment move at its lever, which
    // the joints must absorb at once (keyframes are written, not travelled).
    float keyedMove = 0;
    if (s.status.phase == Phase::kSimulating)
        for (const auto& b : s.bodies)
            if (b.changed && !b.selected && b.keyed) {
                const auto target = (b.pinned ? frame * b.inScene : b.node->world) * b.offset;
                keyedMove = std::max(keyedMove, (target.translate - b.keyframe.translate).Length());
            }
    const bool keyedJump = keyedMove > KeyedJumpLimit(delta);
    // Simulated bodies stay where Havok put them; only a teleport is carried.
    const bool world = s.status.phase == Phase::kSimulating && !Carried(s.physicsFrame, frame) && !keyedJump;
    if (keyedJump)
        logger::info(
            "[CarryPartial] carried rigidly: held bodies jumped {:.1f} in one frame (limit {:.1f}, delta {:.4f}) "
            "attachment moved {:.1f} turned {:.1f}",
            keyedMove, KeyedJumpLimit(delta), delta, (frame.translate - s.physicsFrame.translate).Length(),
            Degrees(CarryLimbFrame::TurnAngle(s.physicsFrame.rotate, frame.rotate)));
    // Dynamics::follow: part of the carrier's speed change reaches the bodies directly, as if the
    // carrying arm absorbed it, so starting, stopping and turning swing a heavy body less.
    const bool moved = delta > 0.00001f;
    const auto share = world && moved ? (velocity - s.previousFrameVelocity) *
                                            (s.request.dynamics.follow * RE::bhkWorld::GetWorldScale())
                                      : RE::NiPoint3{};
    const float gravityTime = moved && delta <= 0.1f ? delta : 0.0f;
    for (auto& b : s.bodies) {
        if (!b.changed) continue;
        if (b.rigid->world != s.nativeWorld.get()) {
            s.status.phase = Phase::kFaulted;
            return;
        }
        if (!b.selected || s.status.phase != Phase::kSimulating) {
            const auto target = (b.pinned ? s.scene->world * b.inScene : b.node->world) * b.offset;
            WritePose(*b.wrapper, target);
            // Move with the target's velocity so the constraints hand its momentum to the rest.
            const bool timed = world && b.keyed && delta > 0.00001f && delta <= 0.1f;
            b.wrapper->SetLinearVelocity(
                Vector(timed ? (target.translate - b.keyframe.translate) * (RE::bhkWorld::GetWorldScale() / delta)
                             : RE::NiPoint3{}));
            b.wrapper->SetAngularVelocity(
                Vector(timed ? CarryLimbFrame::AngularVelocity<RE::NiPoint3>(b.keyframe.rotate, target.rotate, delta)
                             : RE::NiPoint3{}));
            b.keyframe = target;
            b.keyed = true;
        } else if (world) {
            const auto push = share + b.extraGravity * gravityTime;
            if (push.x != 0 || push.y != 0 || push.z != 0)
                b.wrapper->SetLinearVelocity(Vector(Vector(b.rigid->motion.linearVelocity) + push));
        } else {
            // Carried rigidly this frame: the simulated bodies keep their pose relative to the attachment.
            const auto rotation = frame.rotate * s.physicsFrame.rotate.Transpose();
            const auto linear = rotation * Vector(b.rigid->motion.linearVelocity) + b.extraGravity * gravityTime;
            const auto angular = rotation * Vector(b.rigid->motion.angularVelocity);
            WritePose(*b.wrapper, CarryLimbFrame::Rebase(ReadPose(*b.rigid), s.physicsFrame, frame));
            b.wrapper->SetLinearVelocity(Vector(linear));
            b.wrapper->SetAngularVelocity(Vector(angular));
        }
    }
    // A repair asked by AfterPhysics: the simulated bodies go back to the last sound shape around the
    // anchor's keyframe written above, moving with the carrier.
    if (s.repairPending && s.status.phase == Phase::kSimulating) {
        s.repairPending = false;
        const auto anchor = std::find_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.anchor; });
        if (anchor != s.bodies.end() && anchor->keyed) {
            auto base = anchor->keyframe;
            base.scale = 1;
            const auto carried = world ? velocity * RE::bhkWorld::GetWorldScale() : RE::NiPoint3{};
            for (auto& b : s.bodies)
                if (b.changed && b.selected) {
                    WritePose(*b.wrapper, base * b.sound);
                    b.wrapper->SetLinearVelocity(Vector(carried));
                    b.wrapper->SetAngularVelocity({0, 0, 0, 0});
                }
        }
        std::lock_guard lock(s.samplesMutex);
        s.repairSample = s.samples + 1;
    }
    s.physicsFrame = frame;
    // A teleport's velocity is not a speed change for the next frame.
    if (world && moved) s.previousFrameVelocity = velocity;
}
void Session::AfterPhysics(float delta) {
    auto& s = *state_;
    if (!s.Current()) {
        if (s.status.phase != Phase::kIdle) s.status.phase = Phase::kFaulted;
        return;
    }
    if (s.status.phase != Phase::kSimulating) return;
    std::lock_guard lock(s.samplesMutex);
    if (s.invalid) {
        s.status.phase = Phase::kFaulted;
        return;
    }
    if (!s.haveSample) return;
    const auto anchor = std::find_if(s.bodies.begin(), s.bodies.end(), [](const auto& b) { return b.anchor; });
    // Show every sample at the instant the held nodes are at: now. The samples come from the last
    // Havok step, solved for that step's attachment (a frame or two back, a whole hitch back) with
    // the keyframed bodies a step ahead along their velocity. The whole-body anchor maps that
    // instant onto its keyframe target now (BeforePhysics' target) and every body keeps its
    // physics relation to it, like the nodes held without physics (a pinned anchor,
    // simulate="false" bodies, the locked pose), so the joint check measures the physics alone.
    // 0.12.198: a sprint-jump moved the attachment 60 units between a step and its display, and a
    // thigh under a held pelvis faulted at 59.9 with every joint intact.
    auto sampleFrom = s.sampleFrame, sampleTo = s.scene->world;
    if (anchor != s.bodies.end()) {
        sampleFrom = anchor->sample;
        sampleTo = (anchor->pinned ? s.scene->world * anchor->inScene : anchor->node->world) * anchor->offset;
        sampleTo.scale = 1.0f;  // a rigid body frame, like the sample
    }
    // Places every body, a parent before its children: from the physics sample, or (sound) from the
    // last sound shape around the anchor now. The sample pass stops at the first joint that came
    // apart; the sound pass does not check.
    struct Pass {
        bool structural = false;  // a node without a parent, or a non-finite pose
        float worst = 0;
        RE::BSFixedString worstBone;
        const State::Body* broken = nullptr;
        float brokenError = 0;
    };
    const auto place = [&](bool sound) {
        Pass pass;
        for (auto& b : s.bodies) {
            if (!b.selected && !b.pinned) continue;
            auto* parent = b.node->parent;
            if (!parent) {
                pass.structural = true;
                return pass;
            }
            if (b.pinned) {
                // Its simulated ancestors have moved; hold the node where the carry attachment puts it.
                b.node->local = parent->world.Invert() * (s.scene->world * b.inScene);
                Refresh(b.node.get());
                continue;
            }
            const auto body = sound ? sampleTo * b.sound : CarryLimbFrame::Rebase(b.sample, sampleFrom, sampleTo);
            const auto desired = body * b.offset.Invert();
            if (b.limbRoot) {
                // Turn the bodiless chain about its root by the body's swing from its rest under the
                // parent body. The joint pivot sits on that root, so the node lands on the body.
                const auto& above = s.bodies[b.parentBody].node->world;
                const auto rest = above * b.nodeInParent;
                auto root = above * b.limbRootInParent;
                root.rotate = desired.rotate * rest.rotate.Transpose() * root.rotate;
                // Chosen with a parent (above); a body part detached since has none (2026-10-28 review R-28).
                if (!b.limbRoot->parent) {
                    pass.structural = true;
                    return pass;
                }
                const auto rootLocal = b.limbRoot->parent->world.Invert() * root;
                if (!Finite(rootLocal)) {
                    pass.structural = true;
                    return pass;
                }
                b.limbRoot->local = rootLocal;
                Refresh(b.limbRoot.get());
                const float error = (b.node->world.translate - desired.translate).Length();
                if (!sound && error > faultError) {
                    pass.broken = &b;
                    pass.brokenError = error;
                    return pass;
                }
                if (error > pass.worst) {
                    pass.worst = error;
                    pass.worstBone = b.node->name;
                }
                continue;
            }
            const auto local = parent->world.Invert() * desired;
            if (!Finite(local)) {
                pass.structural = true;
                return pass;
            }
            // Within one physics result: the held body's own sample, not the attachment's newer frame,
            // which a fast turn moves by itself (0.12.182 faulted at 45 with the chain intact).
            const auto anchorWorld =
                anchor != s.bodies.end()
                    ? (sound ? sampleTo : CarryLimbFrame::Rebase(anchor->sample, sampleFrom, sampleTo)) *
                          anchor->offset.Invert()
                    : desired;
            const float error = b.hanging ? std::abs((desired.translate - anchorWorld.translate).Length() - b.reach)
                                          : (local.translate - b.local.translate).Length();
            if (!sound && error > faultError) {
                pass.broken = &b;
                pass.brokenError = error;
                return pass;
            }
            if (error > pass.worst) {
                pass.worst = error;
                pass.worstBone = b.node->name;
            }
            b.node->local = local;
            Refresh(b.node.get());
        }
        return pass;
    };
    const bool wholeBody = anchor != s.bodies.end();
    s.clock += std::clamp(delta, 0.0f, 0.1f);
    // Until Havok has stepped from a repair, the samples still hold the broken shape.
    if (wholeBody && (s.repairPending || s.samples < s.repairSample)) {
        if (place(true).structural) s.status.phase = Phase::kFaulted;
        Refresh(s.scene.get());
        return;
    }
    const auto pass = place(false);
    if (pass.structural) {
        s.status.phase = Phase::kFaulted;
        return;
    }
    if (pass.broken) {
        const auto& b = *pass.broken;
        // The oldest of the last repairLimit repairs, still inside the window: too many.
        const bool allowed = wholeBody && s.haveSound && s.clock - s.repairAt[s.repairNext] > repairWindow;
        if (!allowed) {
            if (b.limbRoot)
                logger::warn("[CarryPartial] limb fault bone={} root={} error={:.3f} repairs={}", b.node->name.c_str(),
                    b.limbRoot->name.c_str(), pass.brokenError, s.status.repairs);
            else
                logger::warn("[CarryPartial] joint fault bone={} part={} error={:.3f} hanging={} repairs={}",
                    b.node->name.c_str(), b.part, pass.brokenError, b.hanging, s.status.repairs);
            s.status.phase = Phase::kFaulted;
            return;
        }
        s.repairAt[s.repairNext] = s.clock;
        s.repairNext = (s.repairNext + 1) % repairLimit;
        ++s.status.repairs;
        logger::info("[CarryPartial] repaired bone={} error={:.3f} repairs={}: back to the last sound shape",
            b.node->name.c_str(), pass.brokenError, s.status.repairs);
        s.repairPending = true;
        if (place(true).structural) s.status.phase = Phase::kFaulted;
        Refresh(s.scene.get());
        return;
    }
    if (pass.worst > s.status.maximumJointError) {
        s.status.maximumJointError = pass.worst;
        s.status.maximumJointBone = pass.worstBone;
    }
    // A sound frame is the shape a later repair goes back to (bodies relative to the anchor body).
    if (wholeBody && pass.worst <= soundError) {
        const auto base = anchor->sample.Invert();
        for (auto& b : s.bodies)
            if (b.owned) b.sound = base * b.sample;
        s.haveSound = true;
    }
    Refresh(s.scene.get());
}
Result Session::BeginNativeRelease() {
    auto& s = *state_;
    if (s.status.phase != Phase::kSimulating && s.status.phase != Phase::kReady) return Result::kBusy;
    if (!s.Current()) return Result::kSceneUnavailable;
    RE::BSWriteLockGuard lock(s.world->worldLock);
    if (s.nativeWorld->criticalOperationsLockCount) return Result::kUnsafeUpdatePath;
    const auto frame = s.scene->world;
    if (!Finite(frame) || !Finite(s.physicsFrame)) return Result::kActivationFailed;
    // Validate the entire snapshot before relinquishing any owned state.
    for (auto& b : s.bodies)
        if (b.owned) {
            if (b.rigid->world != s.nativeWorld.get()) return Result::kWorldMismatch;
            if (s.status.phase == Phase::kSimulating) {
                // World-space state already includes the carrier's motion (the anchor moves with it).
                b.releasePose = ReadPose(*b.rigid);
                b.releaseLinear = Vector(b.rigid->motion.linearVelocity);
                b.releaseAngular = Vector(b.rigid->motion.angularVelocity);
            } else {
                b.releasePose = b.node->world * b.offset;
                b.releaseLinear = b.releaseAngular = {};
            }
            const auto finite = [](const RE::NiPoint3& v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            };
            if (!Finite(b.releasePose) || !finite(b.releaseLinear) || !finite(b.releaseAngular))
                return Result::kActivationFailed;
        }
    s.Unlisten();
    // Restore our damping/filter/sync overrides before the engine acquires them.
    // Keep registered native bodies and visible locals; do not stand up first.
    s.RestoreModes(false);
    for (auto& b : s.bodies)
        if (b.owned) WritePose(*b.wrapper, b.releasePose);
    s.status.phase = Phase::kAwaitNativeRelease;
    logger::info(
        "[CarryPartial] native release prepared generation={} bodies={}", s.request.carryGeneration, s.bodies.size());
    return Result::kReady;
}
Result Session::AdvanceNativeRelease() {
    auto& s = *state_;
    if (s.status.phase != Phase::kAwaitNativeRelease || !s.Current()) return Result::kSceneUnavailable;
    auto target = s.request.target.get();
    if (!target || target->IsDead()) return Result::kInvalidActor;
    {
        RE::BSWriteLockGuard lock(s.world->worldLock);
        if (s.nativeWorld->criticalOperationsLockCount) return Result::kBusy;
        const auto* driver = s.graph->characterInstance.ragdollDriver.get();
        const auto* ragdoll = driver ? driver->ragdoll : nullptr;
        if (!ragdoll) return Result::kSceneUnavailable;
        bool ready = target->IsInRagdollState();
        for (const auto& b : s.bodies)
            if (b.owned) {
                if (b.rigid->world != s.nativeWorld.get() ||
                    std::find(ragdoll->rigidBodies.begin(), ragdoll->rigidBodies.end(), b.rigid.get()) ==
                        ragdoll->rigidBodies.end())
                    return Result::kWorldMismatch;
                const auto motion = b.rigid->motion.type.get();
                ready &= motion == Motion::kDynamic || motion == Motion::kSphereInertia ||
                         motion == Motion::kBoxInertia || motion == Motion::kThinBoxInertia;
            }
        if (!ready) {
            // Keep the visible carry pose during the short gameplay handoff.
            // Do not overwrite native bodies while their ownership is changing.
            for (auto& b : s.bodies)
                if (b.owned) {
                    const auto desired = b.releasePose * b.offset.Invert();
                    b.node->local = b.node->parent ? b.node->parent->world.Invert() * desired : desired;
                    Refresh(b.node.get());
                }
            Refresh(s.scene.get());
            return Result::kBusy;
        }
        // Native knockdown has now acquired ALL bodies, including the old COM
        // anchor. Seed the preserved pose/velocity once, then never drive it.
        for (auto& b : s.bodies)
            if (b.owned) {
                WritePose(*b.wrapper, b.releasePose);
                b.wrapper->SetLinearVelocity(Vector(b.releaseLinear));
                b.wrapper->SetAngularVelocity(Vector(b.releaseAngular));
                const auto desired = b.releasePose * b.offset.Invert();
                b.node->local = b.node->parent ? b.node->parent->world.Invert() * desired : desired;
                Refresh(b.node.get());
            }
        Refresh(s.scene.get());
        s.registered = false;
    }
    logger::info("[CarryPartial] native release committed generation={} target={:08X}; engine owns recovery",
        s.request.carryGeneration, target->GetFormID());
    // All overrides were released before knockdown. Yield leaves engine sync
    // flags, dynamic motion and world registration untouched.
    Invalidate(EndReason::kFullRagdoll);
    return Result::kReady;
}
void Session::Invalidate(EndReason reason) {
    auto& s = *state_;
    const bool yield = reason == EndReason::kFullRagdoll;
    if (s.world && s.nativeWorld) {
        RE::BSWriteLockGuard lock(s.world->worldLock);
        s.Unlisten();
        s.RestoreModes(yield);
        if (s.registered && !yield && s.graph) s.graph->RemoveRagdollFromWorld();
    }
    s.FlushTrace();
    if (!yield && s.Current()) {
        for (auto& b : s.bodies)
            if (b.selected || b.pinned) {
                if (b.limbRoot) {
                    b.limbRoot->local = b.limbRootLocal;
                    Refresh(b.limbRoot.get());
                }
                b.node->local = b.local;
                Refresh(b.node.get());
            }
        Refresh(s.scene.get());
    }
    s.joints.clear();
    s.bodies.clear();
    s.graph.reset();
    s.manager.reset();
    s.scene.reset();
    s.world.reset();
    s.nativeWorld.reset();
    s.registered = false;
    s.haveSample = false;
    s.invalid = false;
    s.request = {};
    s.status = {};
}
Status Session::ReadStatus() const {
    std::lock_guard lock(state_->samplesMutex);
    auto status = state_->status;
    status.physicsSteps = state_->samples;
    return status;
}
}
