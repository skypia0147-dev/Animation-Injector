#pragma once
#include <cmath>

// How a body lies, for the flow condition `lying` (0.12.253): on its back or on its front. Pure.
//
// Vanilla picks its get-up clip by pose matching (0_master GetUpPoseMatcher between MT_GetUpFromBack and
// MT_GetUpFromFront) on three bones: COM, Neck and L UpperArm. The same three points give the body's facing here: the
// spine runs COM -> neck, the left arm lies to the body's left, and their cross product is the chest's forward. A
// chest facing up is a body on its back. Read when the engine hands a body back (runtime/GetUpHandoff), the nodes
// hold the first frame of the clip the engine matched, so the answer is the engine's own choice.
namespace AnimationInjector::LyingSide {
struct Vec {
    float x = 0, y = 0, z = 0;
};
inline Vec Sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec Scale(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
// The chest's forward in world space (Skyrim: +Z up; an actor faces its +Y with its left arm toward -X, so
// left x spine = forward). Zero when the points do not span a body.
inline Vec Chest(Vec com, Vec neck, Vec leftArm) {
    const auto spine = Sub(neck, com);
    const float spineLength = std::sqrt(Dot(spine, spine));
    if (!(spineLength > 1e-3f)) return {};
    const auto up = Scale(spine, 1.0f / spineLength);
    auto left = Sub(leftArm, com);
    left = Sub(left, Scale(up, Dot(left, up)));
    const float leftLength = std::sqrt(Dot(left, left));
    if (!(leftLength > 1e-3f)) return {};
    return Cross(Scale(left, 1.0f / leftLength), up);
}
// On the back when the chest faces up (a degenerate pose counts as the back: vanilla's first clip).
inline bool OnBack(Vec com, Vec neck, Vec leftArm) { return Chest(com, neck, leftArm).z >= 0; }
}
