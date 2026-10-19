#include "PCH.h"
#include "runtime/CarryGround.h"
#include "RE/B/bhkPickData.h"
#include "RE/H/hkpClosestRayHitCollector.h"

namespace AnimationInjector::CarryGround {
namespace {
bool Finite(const RE::NiPoint3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
// Same fixed-ground collector policy as SpaceProbe. Actors are never ground.
class Ground final : public RE::hkpClosestRayHitCollector {
public:
    Ground() { Reset(); }
    void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hit) override {
        const auto* root = &body;
        unsigned depth = 0;
        while (root->parent && depth++ < 32) root = root->parent;
        if (root->parent) return;
        const auto layer = static_cast<const RE::hkpCollidable*>(root)->GetCollisionLayer();
        if (layer == RE::COL_LAYER::kStatic || layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround)
            RE::hkpClosestRayHitCollector::AddRayHit(body, hit);
    }
};
bool FindGround(RE::Actor* actor, RE::NiPoint3& p) {
    auto* cc = actor->GetCharController();
    auto* world = cc ? cc->GetHavokWorld() : nullptr;
    if (!world || !world->GetWorld1() || !Finite(p)) return false;
    const float scale = RE::bhkWorld::GetWorldScale();
    if (!std::isfinite(scale) || scale <= 0) return false;
    RE::bhkPickData pick{};
    Ground collector;
    pick.rayInput.filterInfo.filter = 0;
    pick.rayInput.from = {p.x * scale, p.y * scale, (p.z + 60) * scale, 0};
    pick.rayInput.to = {p.x * scale, p.y * scale, (p.z - 180) * scale, 0};
    pick.rayOutput.Reset();
    pick.closestRayHitCollector = &collector;
    RE::BSReadLockGuard lock(world->worldLock);
    world->PickObject(pick);
    const auto& hit = collector.rayHit;
    if (!hit.HasHit() || !std::isfinite(hit.hitFraction) || hit.hitFraction < 0 || hit.hitFraction > 1 ||
        !std::isfinite(hit.normal.quad.m128_f32[2]) || hit.normal.quad.m128_f32[2] < 0.65f)
        return false;
    p.z = p.z + 60 - hit.hitFraction * 240 + 2;
    return Finite(p);
}
}
bool InFront(RE::Actor* actor, RE::NiPoint3& out) {
    const float yaw = actor->GetAngleZ();
    out = actor->GetPosition() + RE::NiPoint3{std::sin(yaw) * 80, std::cos(yaw) * 80, 0};
    return FindGround(actor, out);
}
}
