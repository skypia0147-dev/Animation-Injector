#include "PCH.h"
#include "core/SpaceProbe.h"
#include "core/SpaceQueryMath.h"
#include "core/QueryPhantom.h"
#include "core/SpaceContactPolicy.h"
#include "RE/H/hkpWorldRayCastInput.h"
#include "RE/H/hkpWorldRayCastOutput.h"
#include "RE/H/hkpClosestRayHitCollector.h"
#include "RE/B/bhkPickData.h"
#include "Settings.h"
#include <array>
#include <limits>
#include <optional>

// CommonLib declares these trivial methods without definitions.
namespace RE {
hkpCdPointCollector::~hkpCdPointCollector() = default;
void hkpCdPointCollector::Reset() { earlyOutDistance = std::numeric_limits<float>::max(); }
}
namespace AnimationInjector::SpaceProbe {
namespace {
// Ground support is independent of actors and transient hit capsules.
// Rejecting a hit must not shorten the ray: a floor may lie underneath it.
class GroundCollector final : public RE::hkpClosestRayHitCollector {
public:
    unsigned callbacks = 0, ignored = 0;
    std::uint64_t ignoredLayers = 0;
    GroundCollector() { Reset(); }
    void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hit) override {
        ++callbacks;
        const auto* root = &body;
        while (root->parent) root = root->parent;
        const auto layer = static_cast<const RE::hkpCollidable*>(root)->GetCollisionLayer();
        if (layer != RE::COL_LAYER::kStatic && layer != RE::COL_LAYER::kTerrain && layer != RE::COL_LAYER::kGround) {
            ++ignored;
            const auto index = static_cast<unsigned>(layer);
            if (index < 64) ignoredLayers |= std::uint64_t{1} << index;
            return;
        }
        RE::hkpClosestRayHitCollector::AddRayHit(body, hit);
    }
};
struct Hit {
    int layer = -1;
    float z = 0, w = 0;
    bool participant = false;
};
class Collector final : public RE::hkpCdPointCollector {
public:
    std::array<Hit, 16> hits{};
    std::array<const RE::hkpCollidable*, 2> participants{};
    unsigned count = 0, participantCount = 0, upwardStatic = 0, otherStatic = 0, other = 0;
    float footZ = 0, inverseScale = 1;
    SpaceContactPolicy::Summary policy;
    Collector() { Reset(); }
    void Reset() override {
        count = participantCount = upwardStatic = otherStatic = other = 0;
        policy = {};
        earlyOutDistance = std::numeric_limits<float>::max();
    }
    void AddCdPoint(const RE::hkpCdPoint& point) override {
        Hit hit;
        const auto& normal = point.contact.separatingNormal.quad.m128_f32;
        hit.z = normal[2];
        hit.w = normal[3];
        const auto* body = point.cdBodyB;
        unsigned depth = 0;
        while (body && body->parent && depth++ < 32) body = body->parent;
        if (body && !body->parent) {
            const auto* root = static_cast<const RE::hkpCollidable*>(body);
            hit.layer = static_cast<int>(root->GetCollisionLayer());
            hit.participant = root == participants[0] || root == participants[1];
        }
        if (hit.participant)
            ++participantCount;
        else if (hit.layer == static_cast<int>(RE::COL_LAYER::kStatic)) {
            // Diagnostic buckets only: upward contacts are not automatically walkable ground.
            if (std::isfinite(hit.z) && hit.z >= 0.7f)
                ++upwardStatic;
            else
                ++otherStatic;
        } else
            ++other;
        using Surface = SpaceContactPolicy::Surface;
        auto kind = hit.participant ? Surface::kParticipant : Surface::kUnknown;
        if (!hit.participant) switch (static_cast<RE::COL_LAYER>(hit.layer)) {
            case RE::COL_LAYER::kStatic:
            case RE::COL_LAYER::kTerrain:
            case RE::COL_LAYER::kGround:
            case RE::COL_LAYER::kInvisibleWall: kind = Surface::kFixed; break;
            case RE::COL_LAYER::kSpell:
            case RE::COL_LAYER::kSpellExplosion:
            case RE::COL_LAYER::kTrigger:
            case RE::COL_LAYER::kNonCollidable: kind = Surface::kNonPhysical; break;
            default: break;
            }
        const float height = point.contact.position.quad.m128_f32[2] * inverseScale - footZ;
        policy.Add(kind, hit.z, height);
        if (count < hits.size()) hits[count] = hit;
        ++count;
    }
};
struct Body {
    RE::hkpRigidBody* rigid = nullptr;
    RE::hkpShapePhantom* phantom = nullptr;
    RE::hkpWorldObject* Object() const { return rigid ? static_cast<RE::hkpWorldObject*>(rigid) : phantom; }
};
Body Collision(RE::bhkCharacterController* cc) {
    Body result;
    if (!cc) return result;
    // Classify the controller before interpreting its collision object.
    // GetRigidBody() being non-null does not prove that the result is a rigid body.
    if (auto* controller = skyrim_cast<RE::bhkCharProxyController*>(cc)) {
        auto* proxy = static_cast<RE::hkpCharacterProxy*>(controller->proxy.referencedObject.get());
        if (proxy) result.phantom = proxy->shapePhantom;
    } else if (skyrim_cast<RE::bhkCharRigidBodyController*>(cc)) {
        result.rigid = cc->GetRigidBody();
    }
    return result;
}
SpaceQueryMath::Point Point(const RE::hkVector4& v) {
    const auto& p = v.quad.m128_f32;
    return {p[0], p[1], p[2]};
}
ConditionFlow::Value Read(std::uint64_t id, int role, RE::Actor* actor, RE::Actor* partner,
    RE::bhkCharacterController* moving, const RE::hkVector4& destination, float distance, float yaw, bool diagnostic) {
    constexpr auto unavailable = ConditionFlow::Value::kUnavailable;
    auto* cc = actor->GetCharController();
    auto* world = cc ? cc->GetHavokWorld() : nullptr;
    if (!world || !world->GetWorld1()) return unavailable;
    // The partner (a start's target) is no obstacle. Without one, or with one outside this world, only the actor
    // itself is left out (0.12.299).
    auto* otherCc = partner ? partner->GetCharController() : nullptr;
    if (otherCc && otherCc->GetHavokWorld() != world) otherCc = nullptr;
    const Body body = Collision(cc), otherBody = Collision(otherCc);
    auto* source = body.Object();
    if (!source || (otherCc && !otherBody.Object())) return unavailable;
    const float scale = RE::bhkWorld::GetWorldScale();
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(yaw) || !std::isfinite(distance) || distance <= 0 ||
        distance > 2000)
        return unavailable;
    RE::hkVector4 feet, center;
    cc->GetPositionImpl(feet, true);
    cc->GetPositionImpl(center, false);
    auto desired = Point(destination);
    if (cc != moving) {
        desired = Point(feet);
        for (auto& value : desired) value /= scale;
    }
    Collector start, sweep;
    RE::hkVector4 origin;
    bool unchanged = false;
    {
        RE::BSReadLockGuard lock(world->worldLock);
        if (source->world != world->GetWorld1() || (otherBody.Object() && otherBody.Object()->world != source->world) ||
            !source->collidable.shape)
            return unavailable;
        auto* motion = source->GetMotionState();
        if (!motion) return unavailable;
        const auto before = motion->transform;
        auto predicted = SpaceQueryMath::Origin(Point(center), Point(feet), desired, scale);
        if (!predicted) return unavailable;
        logger::debug(
            "[SpaceProbe] #{} role={} phase=prepare-query moving={} nativeFoot=({:.2f},{:.2f},{:.2f}) yaw={:.5f}", id,
            role, cc == moving, desired[0], desired[1], desired[2], yaw);
        auto transform = before;
        origin = {(*predicted)[0], (*predicted)[1], (*predicted)[2], 0};
        transform.translation = origin;
        const auto filter = source->collidable.broadPhaseHandle.collisionFilterInfo.filter;
        logger::debug("[SpaceProbe] #{} role={} phase=independent-construct", id, role);
        QueryPhantom detached(source->collidable.shape, transform, filter);
        auto* query = detached.Get();
        if (!query) {
            logger::warn("[SpaceProbe] #{} role={} result=unavailable reason=query-construction-unavailable", id, role);
            return unavailable;
        }
        if (query->world || query->GetMotionState() == motion || query->collidable.shape != source->collidable.shape ||
            !query->overlapListeners.empty() || !query->phantomListeners.empty()) {
            logger::error("[SpaceProbe] #{} role={} result=unavailable reason=query-isolation-failed", id, role);
            return unavailable;
        }
        start.participants =
            sweep.participants = {&source->collidable, otherBody.Object() ? &otherBody.Object()->collidable : nullptr};
        start.footZ = sweep.footZ = desired[2];
        start.inverseScale = sweep.inverseScale = 1.0f / scale;
        const auto& pos = origin.quad.m128_f32;
        RE::hkpLinearCastInput input{};
        input.to = {pos[0] + std::sin(yaw) * distance * scale, pos[1] + std::cos(yaw) * distance * scale, pos[2], 0};
        input.startPointTolerance = 0.01f * scale;
        logger::debug("[SpaceProbe] #{} role={} phase=before-cast", id, role);
        world->GetWorld1()->LinearCast(&query->collidable, input, sweep, &start);
        unchanged = std::memcmp(&before, &motion->transform, sizeof(before)) == 0;
    }
    const auto& pos = origin.quad.m128_f32;
    const auto combined = SpaceContactPolicy::Summary{
        0, 0, start.policy.blocked + sweep.policy.blocked, start.policy.unknown + sweep.policy.unknown};
    const auto verdict = combined.Result(unchanged);
    logger::debug(
        "[SpaceProbe] #{} role={} staticVolume={} support={} blocked={} unknown={} step=20 slope=45 diagnosticOnly={} distance={}",
        id, role,
        verdict == ConditionFlow::Value::kTrue    ? "clear"
        : verdict == ConditionFlow::Value::kFalse ? "blocked"
                                                  : "unavailable",
        start.policy.support + sweep.policy.support, combined.blocked, combined.unknown, diagnostic, distance);
    logger::debug(
        "[SpaceProbe] #{} role={} phase=before-native-move actor={:08X} origin=({:.2f},{:.2f},{:.2f}) liveTransformUnchanged={} diagnosticOnly={}",
        id, role, actor->GetFormID(), pos[0] / scale, pos[1] / scale, pos[2] / scale, unchanged, diagnostic);
    if (diagnostic)
        for (const auto* collector : {&start, &sweep}) {
            const auto* kind = collector == &start ? "start" : "sweep";
            logger::debug(
                "[SpaceProbe] #{} role={} {} contacts={} participants={} staticUpward={} staticOther={} other={}", id,
                role, kind, collector->count, collector->participantCount, collector->upwardStatic,
                collector->otherStatic, collector->other);
            for (unsigned i = 0; i < std::min<unsigned>(collector->count, 16); ++i) {
                const auto& hit = collector->hits[i];
                logger::debug("[SpaceProbe] #{} role={} {} hit={} layer={} participant={} normalZ={:.3f} rawW={:.5f}",
                    id, role, kind, i, hit.layer, hit.participant, hit.z, hit.w);
            }
        }
    return verdict;
}
}
ConditionFlow::Value Path(std::uint64_t id, RE::Actor* actor, RE::Actor* partner, const RE::NiPoint3& destination) {
    if (!actor || !partner || !actor->GetCharController()) return ConditionFlow::Value::kUnavailable;
    const auto from = actor->GetPosition();
    const float dx = destination.x - from.x, dy = destination.y - from.y;
    const float length = std::hypot(dx, dy);
    if (!std::isfinite(length)) return ConditionFlow::Value::kUnavailable;
    // Read sweeps from a foot position along a Skyrim heading (forward = sin, cos).
    return Read(id, 1, actor, partner, actor->GetCharController(), RE::hkVector4{from.x, from.y, from.z, 0},
        std::max(length, 1.0f), length > 0 ? std::atan2(dx, dy) : actor->GetAngleZ(), false);
}
namespace {
// Where a check measures (0.12.299): the actor's position now, along its heading now turned by `direction`. Not where an
// action would put it: a check decides before anything is picked.
struct Spot {
    RE::NiPointer<RE::Actor> self, other;  // the actor, and the start's target or none
    RE::NiPoint3 at;
    float yaw = 0;
};
std::optional<Spot> Here(RE::ActorHandle initiator, RE::ActorHandle target, float direction) {
    Spot s{initiator.get(), target.get()};
    if (!std::isfinite(direction) || !s.self || !s.self->Is3DLoaded()) return std::nullopt;
    if (s.other && (s.other.get() == s.self.get() || !s.other->Is3DLoaded())) s.other.reset();
    s.at = s.self->GetPosition();
    s.yaw = s.self->GetAngleZ() + direction;
    if (!std::isfinite(s.yaw)) return std::nullopt;
    return s;
}
}
ConditionFlow::Value Evaluate(RE::ActorHandle initiator, RE::ActorHandle target, float distance, float direction) {
    const auto spot = Here(initiator, target, direction);
    if (!spot) return ConditionFlow::Value::kUnavailable;
    // No moving controller: Read sweeps from the actor's own feet.
    return Read(0, 0, spot->self.get(), spot->other.get(), nullptr,
        RE::hkVector4{spot->at.x, spot->at.y, spot->at.z, 0}, distance, spot->yaw, false);
}
ConditionFlow::Value EvaluateGround(
    RE::ActorHandle initiator, RE::ActorHandle target, float distance, float direction, GroundPolicy::Limits limits) {
    using V = ConditionFlow::Value;
    if (!GroundPolicy::Valid(limits) || !std::isfinite(distance) || distance < 0 || distance > 2000)
        return V::kUnavailable;
    const auto spot = Here(initiator, target, direction);
    if (!spot) return V::kUnavailable;
    auto* actor = spot->self.get();
    auto* cc = actor->GetCharController();
    auto* world = cc ? cc->GetHavokWorld() : nullptr;
    if (!world || !world->GetWorld1()) return V::kUnavailable;
    const float scale = RE::bhkWorld::GetWorldScale(), yaw = spot->yaw;
    if (!std::isfinite(scale) || scale <= 0) return V::kUnavailable;
    const auto p = spot->at;
    const unsigned segments = static_cast<unsigned>(std::ceil(distance / 25.0f));
    const std::array<std::array<float, 2>, 5> offsets{
        {{0, 0}, {limits.radius, 0}, {-limits.radius, 0}, {0, limits.radius}, {0, -limits.radius}}};
    const float top = p.z + limits.rise + 2, span = limits.rise + limits.drop + 4;
    unsigned samples = 0, callbacks = 0, ignored = 0;
    std::uint64_t ignoredLayers = 0;
    V result = V::kTrue;
    float height = 0, normalZ = 0;
    int layer = -1;
    const char* reason = "supported";
    {
        RE::BSReadLockGuard lock(world->worldLock);
        for (unsigned i = 0; i <= segments && result == V::kTrue; ++i)
            for (const auto& offset : offsets) {
                const float t = segments ? distance * i / segments : 0;
                const float x = p.x + std::sin(yaw) * t + offset[0], y = p.y + std::cos(yaw) * t + offset[1];
                layer = -1;
                height = 0;
                normalZ = 0;
                RE::bhkPickData pick{};
                auto& input = pick.rayInput;
                input.filterInfo.filter = 0;
                input.from = {x * scale, y * scale, top * scale, 0};
                input.to = {x * scale, y * scale, (top - span) * scale, 0};
                GroundCollector collector;
                pick.rayOutput.Reset();
                pick.closestRayHitCollector = &collector;
                world->PickObject(pick);
                const auto& output = collector.rayHit;
                callbacks += collector.callbacks;
                ignored += collector.ignored;
                ignoredLayers |= collector.ignoredLayers;
                ++samples;
                if (!collector.callbacks && pick.rayOutput.HasHit()) {
                    result = V::kUnavailable;
                    reason = "collector-not-used";
                    break;
                }
                if (!output.HasHit()) {
                    result = V::kFalse;
                    reason = "no-ground-in-range";
                    break;
                }
                layer = static_cast<int>(output.rootCollidable->GetCollisionLayer());
                const bool fixed = layer == static_cast<int>(RE::COL_LAYER::kStatic) ||
                                   layer == static_cast<int>(RE::COL_LAYER::kTerrain) ||
                                   layer == static_cast<int>(RE::COL_LAYER::kGround);
                const auto& n = output.normal.quad.m128_f32;
                normalZ = n[2];
                height = top - output.hitFraction * span - p.z;
                if (!std::isfinite(output.hitFraction) || output.hitFraction < 0 || output.hitFraction > 1) {
                    result = V::kUnavailable;
                    reason = "invalid-hit";
                    break;
                }
                result = GroundPolicy::Judge(true, fixed, height, n[0], n[1], n[2], limits);
                if (result != V::kTrue) {
                    reason = result == V::kUnavailable ? "unsupported-hit" : "height-or-slope";
                    break;
                }
            }
    }
    logger::debug("[GroundRay] actor={:08X} samples={} callbacks={} ignored={} ignoredLayers={:016X}",
        actor->GetFormID(), samples, callbacks, ignored, ignoredLayers);
    logger::debug(
        "[GroundCondition] actor={:08X} distance={} samples={} result={} reason={} layer={} height={:.3f} normalZ={:.4f} rise={} drop={} slope={} radius={}",
        actor->GetFormID(), distance, samples,
        result == V::kTrue    ? "clear"
        : result == V::kFalse ? "blocked"
                              : "unavailable",
        reason, layer, height, normalZ, limits.rise, limits.drop, limits.slope, limits.radius);
    return result;
}
void BeforePlacement(std::uint64_t id, RE::ActorHandle initiator, RE::ActorHandle target,
    RE::bhkCharacterController* moving, const RE::hkVector4& destination) {
    if (!FrameworkSettings::Get().spaceProbe || !moving) return;
    auto self = initiator.get(), other = target.get();
    if (!self || !other || self.get() == other.get()) return;
    for (auto* actor : {self.get(), other.get()})
        if (!actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState()) return;
    if (self->GetCharController() != moving && other->GetCharController() != moving) return;
    Read(id, 0, self.get(), other.get(), moving, destination, 200, self->GetAngleZ(), true);
    Read(id, 1, other.get(), self.get(), moving, destination, 200, other->GetAngleZ(), true);
}
}
