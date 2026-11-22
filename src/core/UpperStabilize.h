#pragma once
#include <array>
#include <span>

// The upper layer's stabilizer as plain math (core/UpperLayer.h), without engine headers so ctest can check it.
// Havok order: quaternion (x, y, z, w), local transforms parent-relative.
namespace AnimationInjector::UpperStabilize {

using Quat = std::array<float, 4>;

// The model-space rotation at the end of a chain of local rotations (root first).
Quat ChainRotation(std::span<const Quat> chain);
// The rotation's part about the model's up axis (Z): the twist of a swing-twist split. A rotation with no Z part
// (a half turn about a level axis) gives none.
Quat TwistZ(const Quat& q);
// From `from` to `to` by t (0 .. 1) on the shortest arc, normalized.
Quat Slerp(const Quat& from, const Quat& to, float t);
// The spine's new local rotation under the output's model-space rotation of the spine's parent `parent` (NPC COM in
// the game's skeleton; `rest` is that parent's model rotation in the skeleton's reference pose).
//   The spine keeps its rotation relative to the clip's own parent (`clipParent`, `clipSpine` model-space), set on
//   that parent as it would stand upright: the clip's lean of its parent (a crouch, a stance that tilts the hips) is
//   carried by the clip's own legs, which the layer does not play (11-21, user report: the upper body leaned forward
//   by the clip's own crouch, 16~17 degrees over straight legs).
//   The clip's turn of that parent about the up axis stays (a sideways stance's chest still faces where the clip's
//   does) and the output's turn of its parent from rest is added: a side step that turns the body turns the upper
//   body too. The output parent's tilt and sway never reach the spine.
// Blended from the output's own `outputLocal` by `w` (0 .. 1, shortest arc).
Quat SpineLocal(const Quat& parent, const Quat& rest, const Quat& clipParent, const Quat& clipSpine,
    const Quat& outputLocal, float w);

}  // namespace AnimationInjector::UpperStabilize
