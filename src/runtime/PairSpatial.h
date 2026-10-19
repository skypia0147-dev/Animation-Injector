#pragma once
#include "PCH.h"
#include <optional>
#include <string_view>

namespace AnimationInjector::PairSpatial {
// A conservative point ray for authored selection conditions, not a movement sweep.
inline std::optional<float> Distance(RE::Actor* actor, std::string_view direction) {
    if (!actor || !actor->Is3DLoaded()) return {};
    auto* cell = actor->GetParentCell();
    auto* world = cell ? cell->GetbhkWorld() : nullptr;
    if (!world || !world->GetWorld1()) return {};
    const float yaw = actor->GetAngleZ(), s = std::sin(yaw), c = std::cos(yaw);
    RE::NiPoint3 axis;
    if (direction == "front")
        axis = {s, c, 0};
    else if (direction == "back")
        axis = {-s, -c, 0};
    else if (direction == "right")
        axis = {c, -s, 0};
    else if (direction == "left")
        axis = {-c, s, 0};
    else if (direction == "down")
        axis = {0, 0, -1};
    else
        return {};
    const bool ground = direction == "down";
    const float radius = ground ? 0.0f : 30.0f;
    auto start = actor->GetPosition() + axis * radius;
    start.z += ground ? 5.0f : 80.0f;
    const auto end = start + axis * 1000.0f;
    const float scale = RE::bhkWorld::GetWorldScale();
    RE::hkpWorldRayCastInput input{};
    RE::hkpWorldRayCastOutput output{};
    input.from = {start.x * scale, start.y * scale, start.z * scale, 0};
    input.to = {end.x * scale, end.y * scale, end.z * scale, 0};
    RE::BSReadLockGuard guard(world->worldLock);
    world->GetWorld1()->CastRay(input, output);
    return std::max(0.0f, (output.HasHit() ? output.hitFraction * 1000.0f : 1000.0f) + radius - (ground ? 5.0f : 0.0f));
}
}
