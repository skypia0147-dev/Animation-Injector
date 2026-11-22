#pragma once
#include "PCH.h"
#include "core/AnimDataRegistry.h"
#include "runtime/LadderSpec.h"

#include <functional>

// A weapon kind with a locomotion tree of its own: <Tree weapon="2HW|H2H" use="own">.
//
// Two weapon kinds move with another kind's tree in vanilla (checked against the game's graphs, 11-15):
//   2HW (two-handed axes and hammers): the legs play the 2HM direction ladders, the arms a direction-less overlay
//       (2HW_WalkArmBlend / 2HW_RunArmBlend) under 2HW_iStateGen.
//   H2H (unarmed): state 0 of the weapon state machine Melee_Direction_Behavior names the same 1HM_iStateGen as the
//       one-handed states (1-4, 10, 11), so unarmed plays the one-handed tree whole.
// use="own" gives the kind a full copy of that tree under its own prefix, so a mod can ship walk and run animations
// for it alone:
//
//   <prefix>_BSCyclicBlendTransitionGenerator      (copy of the source kind's cyclic generator)
//     -> <prefix>_Direction_Blend                  (copy of its direction blend, children remapped)
//        -> <prefix>_<Direction>Blend x 8          (copies of <from>_<Direction>Blend)
//           -> <prefix>_<Clip>                     (copies of the clips, <from>_ -> <prefix>_ in the clip name)
//
//   2HW: 2HW_iStateGen.pDefaultGenerator -> the copy. Sneak2HW_DirectionBlend's upper-body child
//        (2HW_LocomotionArmBlend) -> 2HW_Direction_Blend, as Sneak2HM_DirectionBlend uses 2HM_Direction_Blend.
//   H2H: a copy of 1HM_iStateGen (H2H_iStateGen, the same iState) over the tree copy; state 0's generator -> it. The
//        one-handed states keep 1HM_iStateGen. Unarmed sneaking plays the common sneak tree and is not changed.
//   BlockBehavior (moving while blocking): the same copy again from that graph's own tree (BuildBlock).
//
// Files: each clip of the copy plays `Animations\<prefix>_<name>.hkx` when that file is in Files(), else the source
// clip's own file. One log line per weapon lists the files found and the ones that fell back.
//
// Motion is the source clips' (user decision 11-14: the files keep the source clips' length and root speed): a copy
// is registered in animationdatasinglefile as a copy of the source clip (AnimData::ClipCopy, rate 1), so it gets that
// motion block, and keeps the source clip's playback speed and mode. Foot timing is the file's own (11-15): a clip
// that plays an own file takes no trigger array, converts its file's FootLeft / FootRight annotations into its sync
// points, and its animation data copy carries no source annotations. Sharing the source triggers gave the Slow slots
// (which convert annotations) two sets of sync points and the stops shook (CopyTree). A clip that falls back to a
// source file shares the source clip's trigger array and flags. The variable binding sets are shared with the source.
//
// The copy's direction blenders are then split like any other set's (sets 2HW, H2H, Block2HW, BlockH2H); the
// vanilla record they are checked against is the source kind's (VanillaLadders::Find). A clip that plays an own
// file gets a rate variable of its own, whose ground speed the file's animmotion annotations replace when the clip
// is first activated (core/LocomotionDrive.h FileSpeedKnown); without them it is the source file's.
//
// Self-check before anything is changed (any failure: one warning, the graph is left as it was): the tagging
// generators have the expected iState; the source cyclic generator, its direction blend and the eight direction
// blenders are found with the expected vtables and the source prefix; every direction blender's children are
// hkbClipGenerator; the per-instance arrays are empty (templates); for H2H, state 0 of Melee_Direction_Behavior names
// 1HM_iStateGen. Third-person character project only (LocomotionSplit::Apply's caller).
namespace AnimationInjector::OwnLocomotion {

// The files the own trees play: for every weapon with use="own", every file of its source kind's direction ladders of
// Melee_Locomotion_Behavior (the vanilla records, in their order) with the prefix replaced (2HM_ -> 2HW_, 1HM_ ->
// H2H_), kept when it exists (loose or in an archive, through the game's file system, so MO2's virtual folder
// counts). Decided once, before any project loads a clip: GraphInjector's RegisterAnimationFiles appends these after
// the action files to every third-person character project's animation table, so the male and female projects, which
// share the graphs and so the clips' binding indices, give each file the same index. (11-14: indexing only the
// project that ran the graph pass made the other project's clips play another mod's animations at those indices.)
const std::vector<std::string>& Files();
// Whether `file` is one of Files() (found on disk), ignoring case: a <Stance> file the ladders may play.
bool Listed(const std::string& file);

struct Built {
    std::vector<RE::hkbNode*> directionBlenders;  // the eight <prefix>_<Direction>Blend, for the ladder pass
    RE::hkbNode* direction = nullptr;             // <prefix>_Direction_Blend
    int nodes = 0;                                // generators made (node id budget)
    int ownFiles = 0;                             // clips that play an own file
    int fallbackFiles = 0;                        // clips that play the source file (no own file, or no index)
};

// The nodes of Melee_Locomotion_Behavior the trees hang from (LocomotionSplit::Apply's walk finds them by name).
struct Found {
    RE::hkbNode* tag2HW = nullptr;    // 2HW_iStateGen
    RE::hkbNode* tag2HM = nullptr;    // 2HM_iStateGen
    RE::hkbNode* sneak2HW = nullptr;  // Sneak2HW_DirectionBlend
    RE::hkbNode* tag1HM = nullptr;    // 1HM_iStateGen
    RE::hkbNode* weapons = nullptr;   // Melee_Direction_Behavior (hkbStateMachine; its state 0 is unarmed)
};

// Melee_Locomotion_Behavior, before its blenders are split. Empty `directionBlenders`: a check failed, or `room`
// (the node id budget, asked with the number of nodes the tree needs) refused, and nothing was changed. `copies` gets
// one entry per clip of the copy (source = the clip it copies); queue them before any copy made from them.
Built Build(TreeWeapon weapon, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, const Found& found,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room);

// A switch at a place where an own tree replaced vanilla's node: a two-way hkbManualSelectorGenerator (a copy of a
// vanilla one, LocomotionDrive::SwitchSample) that plays `mod` while the split is on and `vanilla` while it is off
// (LocomotionDrive::kOffVariable), so LocomotionSplit::Toggle compares the mod with the game without it (11-20, user
// request: until then the toggle left the own trees' files and whole-body movement on). The change is not blended.
// Returns `mod` itself (the place then keeps the mod's node with the split off too, as before; one warning) when
// there is no sample, the binding cannot be made, or `room` refuses the one node.
RE::hkbNode* Switch(RE::hkbBehaviorGraph* graph, RE::hkbNode* mod, RE::hkbNode* vanilla, const char* name,
    const std::function<bool(std::size_t)>& room);

// BlockBehavior, before its blenders are split. Moving while blocking picks its locomotion by iRightHandType in
// Shd_Locomotion_MSG (`selector`), whose entry for the weapon (2HW 6, H2H 0) is the source kind's (2HM 5, 1HM 1). The
// same copy of that tree is made (the block graph's own clips: their triggers carry its event ids) and the weapon's
// entry points at it; its direction blenders take the Block2HW / BlockH2H rows. Block locomotion is the lower body
// only (Block_3rdP_BoneSwitch puts the block pose over it), so the files show on the legs (11-15, user report: 2HW
// blocking moved with the 2HM legs). Same self-check and failure rule as Build.
Built BuildBlock(TreeWeapon weapon, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, RE::hkbNode* selector,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room);

// Unarmed with its own tree moves with the whole body (11-15, user decision: as 2HW's own tree does, no option).
// In vanilla 1HM_Behavior, unarmed blends two children by bone (H2H_1HM_Readied_Blend): the arms from
// H2H_Readied_BlendPose_Behavior (standing H2H_Idle; moving H2H_SprintPoseBehavior, whose state
// H2H_LocomotionPoseState plays the one direction-less clip H2H_LocomotionPose), everything else from 1HM_Readied_MSG
// (the locomotion). So the arms of an H2H_*.hkx file never showed. This points H2H_LocomotionPoseState at
// 1HM_Readied_MSG itself (`readied`, through a Switch): while moving both children give the locomotion pose, so the arms are the
// file's. Standing, sprinting and first person keep their generators. `armPose` is H2H_SprintPoseBehavior. False (and
// a warning, nothing changed) when the nodes are not as expected.
bool FullBodyArms(RE::hkbBehaviorGraph* graph, RE::hkbNode* armPose, RE::hkbNode* readied,
    const std::function<bool(std::size_t)>& room);

}  // namespace AnimationInjector::OwnLocomotion
