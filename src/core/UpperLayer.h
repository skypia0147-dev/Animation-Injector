#pragma once
#include "PCH.h"
#include "runtime/BodyPart.h"
#include <functional>

// The part layers in the third-person master graph (docs/설계-상체레이어.md).
//
// Graph pass (GraphInjector's symbol pass, third-person character project, after the actions and the locomotion):
// RootBehaviorGraph, the state machine of every ordinary state (standing, locomotion, combat, our actions), becomes the
// bottom of up to three stacked blenders, one per layer that has part actions; its four parents (1st3rdPerson_MSG,
// Crossbow1st3rdPerson_MSG, BSBoneSwitchGenerator00, WeaponAdjustment_OffsetAnimationGen) point at the top one
// (11-21 live graph, docs/조사-왼손무기-Shield보정.md). So the hand bone switches above still decide their bones: a
// left-hand one-handed weapon's Shield bone stays DW_ShieldAdjustment's (over them, the 11-20 place, a part's Shield
// track turned that weapon over), a staff's and a crossbow's bones stay theirs; and the root modifiers (LookAt, TDM
// lean, Precision, foot IK) still run after the layers (user decision: kept). A graph without that shape keeps the
// 11-20 place, RootModifierGenerator.generator (RootBoneSwitch_MSG), with a warning.
//
//   (the four parents of RootBehaviorGraph)
//     AI_Layer_LeftArm     [ below, AI_Layer_LeftArm_Mix  (left arm mask)   ]
//       AI_Layer_RightArm    [ below, AI_Layer_RightArm_Mix (right arm mask)  ]
//         AI_Layer_Upper       [ RootBehaviorGraph, AI_Layer_Upper_Mix (upper body mask) ]
//   AI_Layer_<X>_Mix         [ AI_Layer_<X>_A, AI_Layer_<X>_B ]   (two slots, each a selector over its own copy of
//                                                                  every clip of the layer)
//
// Each blender is a parametric blend of two children at weights 0 and 1 with blendParameter bound to kWeightVariable
// + layer index (PartRunner writes w, at most 1 - kBelowFloor): child weights (1 - w, w). No sync (a sync master
// rescales the other child's time, Draw Fix 2026-09), dense pose forced. Child 0, what is below, has no bone weights
// and worldFromModelWeight 1; child 1, the layer's mix, has the part's bone mask and worldFromModelWeight 0.
// Havok normalizes each bone over the children: a masked bone is (1-w)·below + w·clip, any other bone is below alone.
// The mix is a parametric blend of two slots at weights 0 and 1 with blendParameter bound to kMixVariable + layer
// index (PartRunner writes 0 for slot A, 1 for slot B, between them while one action hands over to the next on the
// same layer: a charge's start, hold and release). Each slot is an hkbManualSelectorGenerator over its own copy of
// the layer's clips (selectedGeneratorIndex bound to kClipVariable / kClipBVariable + layer index; a clip node is
// never under two active parents). A child of weight 0 is deactivated (the blender's default), so a clip starts from
// its beginning each time its weight rises from 0: the layer from w 0, a slot from its end of the mix (PartRunner
// never switches a slot's clip while that slot weighs anything).
//
// Masks by bone name of the character's animation skeleton:
//   upper     every spine bone (Spine, Spine1, Spine2), the neck, head and eyes, both arms and the hands' attachment
//             nodes (Weapon, Shield, AnimObjectL/R, magic nodes), the quiver and the back weapon nodes
//   rightArm  the right clavicle, upper arm, forearm (and twist bones), hand, fingers, pauldron, magic node; Weapon,
//             AnimObjectR
//   leftArm   the same on the left; Shield, AnimObjectL
// Skipped (one warning; the part actions of that layer then refuse to start): RootModifierGenerator not found, no
// sample bindings, the node id budget, no animation skeleton.
//
// Stabilizer (upper layer only; the user decisions of 11-20 and 11-21, docs/설계-상체레이어.md 13): the legs' tilt
// and sway never reach the upper body, their heading does, and standing the clip plays whole. Two exits of
// hkbBlenderGenerator::Generate (vfunc 0x17) on the animation thread:
//   - AI_Layer_Upper_Mix: the model-space rotations of NPC Spine [Spn0] and of its parent (Root -> COM) in the slots'
//     pose (both slots blended while one hands over) are kept for the character generated;
//   - AI_Layer_Upper: Spine's local rotation becomes UpperStabilize::SpineLocal: the clip's spine relative to the
//     clip's own COM, set on an upright COM turned about the up axis by the clip's stance and by the output COM's
//     turn from rest. Standing (SetStand), COM and every bone under it but the spine's branch blend toward the clip
//     and the spine toward the clip's own local rotation. All by the blender's parameter w. Translations: only the
//     standing bones'.
// Generate runs on per-actor clones, so our two blenders are told by a marker in a field the copy keeps and that
// changes nothing for them: referencePoseWeightThreshold (their children never weigh under 0.001 together). One
// float compare per other blender.
namespace AnimationInjector::UpperLayer {

inline constexpr const char* kWeightVariable = "AI_LayerW";     // + layer index: w, 0 .. 1 - kBelowFloor
inline constexpr const char* kClipVariable = "AI_LayerClip";    // + layer index: int, slot A's clip
inline constexpr const char* kClipBVariable = "AI_LayerClipB";  // + layer index: int, slot B's clip
inline constexpr const char* kMixVariable = "AI_LayerMix";      // + layer index: 0 slot A .. 1 slot B
inline constexpr float kBelowFloor = 0.001f;  // never 0 below: a bone with no weight at all would have no pose

using RoomFn = std::function<bool(RE::hkbBehaviorGraph* graph, std::size_t nodes)>;
struct Summary {
    int layers = 0;  // made (0..3)
    int clips = 0;   // part action clips put in their layer (each in both slots)
};
// Graph pass, once per third-person character project. Reads Actions (the part actions by layer); does nothing
// without any.
Summary Apply(RE::hkbBehaviorGraph* master, const RE::BSScrapArray<RE::hkbBehaviorGraph*>& graphs,
    RE::hkbCharacter* character, const RoomFn& room);
// Standing (user decision 11-21): while the actor stands, the upper layer's clip also gives NPC COM and every
// bone under it but the spine (pelvis, legs, feet), so its own legs carry its lean (a crouch over straight idle legs
// tipped forward); moving, the legs are the locomotion's. `stand` (0 .. 1, PartRunner from the actor's speed) times the
// layer's weight is how much; 0 forgets the character. On the animation thread the spine then goes from its stabilized
// rotation to the clip's own local one by the same amount.
void SetStand(const RE::hkbCharacter* character, float stand);
// facing="camera" (11-23): the camera's direction in the character's model frame (yaw about the up axis,
// counterclockwise from above, from the actor's front; pitch up), radians. On the animation thread the upper body is
// turned there by the layer's weight, the turn shared by NPC Spine, Spine1 and Spine2. `on` false forgets the character.
void SetAim(const RE::hkbCharacter* character, bool on, float yaw = 0.0f, float pitch = 0.0f);
// <Stance> (runtime/LadderSpec.h): how far the upper layer's clip turns the chest from the front, in degrees about the
// up axis (left positive, right negative), as last generated for the character (animation thread); false before.
bool StanceTurn(const RE::hkbCharacter* character, float& degrees);
// Load: forgets every character's standing amount, aim and stance turn (their graphs go).
void ClearStand();
// The selector index of a part action's clip in its layer's slots; -1 when the layer or the clip was not made.
int ClipIndex(int action);
// Whether the layer exists in the character graphs (a part action of a missing layer does not start).
bool LayerReady(int layer);
// The clip's length in seconds once one of its clips was activated (core/ClipHooks); 0 until then.
float ClipDuration(int action);
// core/ClipHooks Activate (animation thread): notes a part clip's length.
void OnClipActivated(const RE::hkbClipGenerator* clip, float duration);

// The generate hooks (stabilizer). Plugin load.
void Install();

}  // namespace AnimationInjector::UpperLayer
