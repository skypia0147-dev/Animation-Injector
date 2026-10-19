#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace AnimationInjector::PartialRagdollPolicy {
constexpr bool LayerAllowsSelf(std::uint64_t mask, unsigned layer) {
    return layer < 64 && (mask & (std::uint64_t{1} << layer)) != 0;
}
// Capture after dynamic-motion restoration; native half-floats quantize writes.
// Restore before switching back to keyframed so saved dynamic state stays clean.
struct DampingOverride {
    float before = 0, applied = 0;
    bool owned = false;
    template<class Scalar>
    bool Apply(Scalar& value, float minimum) {
        const float current = static_cast<float>(value);
        if (owned || !std::isfinite(current) || current < 0 || !std::isfinite(minimum) || minimum < 0) return false;
        before = current;
        value = std::max(current, minimum);
        applied = static_cast<float>(value);
        owned = true;
        return std::isfinite(applied) && applied >= current;
    }
    template<class Scalar>
    void Restore(Scalar& value) {
        if (owned && static_cast<float>(value) == applied) value = before;
        owned = false;
    }
};
// Every owned body simulates except the carry anchor. Equipment bodies are never ours.
constexpr bool Selected(bool owned, bool anchor) { return owned && !anchor; }
// A target body someone else simulates: dynamic in the world while the target is knocked down or getting
// up (knock state not normal): the engine's ragdoll drives it. The carry gives such a target back.
// Dynamic bodies of an upright target are a hit reaction's leftover (Precision): outside the world nobody
// steps them (0.12.240), and in the world nothing drives them once Precision has let the target go at the
// lift's start (PrecisionLink, 0.12.242; in game 18 of 18 stayed dynamic in the world 4 s after that,
// 0.12.243). The carry takes them over; Enable sets every motion.
constexpr bool SimulatedByOther(bool owned, bool inWorld, bool dynamic, bool knocked) {
    return owned && inWorld && dynamic && knocked;
}
// Whether a taken body's own frame is where it sits on the pose: a keyframed body in the world follows
// it. A dynamic one lies wherever it fell and a body outside the world holds a stale state, so the pose
// is the bone's (a plain bhkRigidBody sits on its node, unscaled).
constexpr bool FrameFromBone(bool inWorld, bool dynamic) { return !inWorld || dynamic; }
// Follow native constraints from the retained anchors. Foreign equipment must
// never count as an anchor for a disconnected simulated chain.
template<class Bodies, class Edges>
bool ConnectedToAnchors(const Bodies& bodies, const Edges& edges) {
    std::vector<bool> reached(bodies.size(), false);
    for (std::size_t i = 0; i < bodies.size(); ++i) reached[i] = bodies[i].owned && !bodies[i].selected;
    for (std::size_t pass = 0; pass < bodies.size(); ++pass) {
        bool progress = false;
        for (const auto& edge : edges) {
            const auto a = edge.first, b = edge.second;
            if (a >= bodies.size() || b >= bodies.size() || !bodies[a].owned || !bodies[b].owned) return false;
            if (reached[a] != reached[b]) {
                reached[a] = reached[b] = true;
                progress = true;
            }
        }
        if (!progress) break;
    }
    for (std::size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].owned && !reached[i]) return false;
    return true;
}
constexpr std::uint32_t RestoreBits(
    std::uint32_t current, std::uint32_t before, std::uint32_t applied, std::uint32_t mask) {
    // Restore only individual bits still equal to the value we wrote.
    const auto owned = mask & ~(current ^ applied);
    return (current & ~owned) | (before & owned);
}
// Native layer setters recurse into descendants, including equipment. Snapshot
// all desired layers before any write, then apply in parent-before-child order.
template<class Bodies, class Choose, class Read, class Write>
bool ApplyLayerPlan(Bodies& bodies, Choose choose, Read read, Write write) {
    using Layer = std::decay_t<decltype(read(*bodies.begin()))>;
    std::vector<Layer> desired;
    desired.reserve(bodies.size());
    for (auto& body : bodies) desired.push_back(choose(body, read(body)));
    std::size_t i = 0;
    for (auto& body : bodies) {
        if (read(body) != desired[i]) write(body, desired[i]);
        ++i;
    }
    i = 0;
    for (auto& body : bodies)
        if (read(body) != desired[i++]) return false;
    return true;
}
// Transactional changes must include the failing operation in rollback: the
// native function may have changed state before reporting a failure.
template<class Apply, class Undo>
bool Transaction(unsigned count, Apply apply, Undo undo) {
    for (unsigned i = 0; i < count; ++i)
        if (!apply(i)) {
            for (unsigned j = i + 1; j > 0; --j) undo(j - 1);
            return false;
        }
    return true;
}
}
