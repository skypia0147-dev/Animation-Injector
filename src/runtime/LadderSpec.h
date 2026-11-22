#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// <Locomotion> declarations, without engine headers (parser, checker and tests). Design: docs/설계-이동사다리-완충칸.md,
// section 7.
//
//   <Locomotion>
//     <Tree weapon="2HW" use="own"/>
//     <Except set="Sneak"/>
//   </Locomotion>
//
// A vanilla direction blender (`<set>_<Direction>Blend`, axis SpeedSampled) mixes its walk and run clips at every
// steady speed, and plays them at a fixed rate whatever the actor's speed. With a <Locomotion> every direction blender
// of every set (but the <Except> ones) plays a walk clip or a run clip, mixed only while the actor changes between
// the two or stops, at the rate that keeps the feet on the ground: the actor's speed over the file's own ground
// speed (core/LocomotionSplit.h, core/LocomotionDrive.h). There are no numbers to declare.
// Header only: the parser and the loader use it, and every test target that builds them gets it without a source.
namespace AnimationInjector {

// The blender families with a per-direction speed ladder in the character graphs. The blender name is
// `<prefix>_<Direction>Blend` (LadderBlenderName). The Block sets name the same blenders in the BlockBehavior graph
// (moving while blocking) and only there; the others apply everywhere else (LadderInBlock).
enum class LadderSet : std::uint8_t {
    k1HM,        // 1HM_   one-handed and unarmed drawn (iState 6)
    k2HM,        // 2HM_   two-handed sword and axe (iState 7)
    k2HW,        // 2HW_   two-handed axes and hammers with a tree of their own (<Tree weapon="2HW" use="own">,
                 //        core/OwnLocomotion.h); vanilla has no 2HW direction blenders
    kH2H,        // H2H_   unarmed with a tree of its own (<Tree weapon="H2H" use="own">); vanilla plays the 1HM tree
    kBow,        // Bow_
    kCrossBow,   // CrossBow_
    kMagic,      // Magic_      magic readied (iState 9)
    kMagicCast,  // MagicCast_  moving while casting
    kSheathed,   // MT_         weapon sheathed (iState 0)
    kSneak,      // Sneak_      every sneaking state (iState 2)
    // Moving while blocking (BlockBehavior). It moves at walking speed (81, backward 71), yet its axis (96..138 by
    // direction, 11-15) lies between the walk and the next slot, so vanilla mixes a strafe run (side, ~44%) or the run
    // (16..40%) into every blocking step.
    kBlock1HM,    // 1HM_    one-handed, unarmed, magic
    kBlock2HM,    // 2HM_    two-handed (2HW blocks with these too)
    kBlockBow,    // Bow_    bow and crossbow
    kBlockSneak,  // Sneak_  blocking while sneaking
    kBlock2HW,    // 2HW_    two-handed axes and hammers with their own tree (OwnLocomotion::BuildBlock)
    kBlockH2H,    // H2H_    unarmed with its own tree
};

enum class LadderDirection : std::uint8_t {
    kForward,
    kForwardLeft,
    kLeft,
    kBackwardLeft,
    kBackward,
    kBackwardRight,
    kRight,
    kForwardRight,
};

namespace LadderNames {
// XML value, blender prefix. Case-sensitive, as every other XML enum value.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kSets{
    {{"1HM", "1HM"}, {"2HM", "2HM"}, {"2HW", "2HW"}, {"H2H", "H2H"}, {"Bow", "Bow"}, {"CrossBow", "CrossBow"},
        {"Magic", "Magic"}, {"MagicCast", "MagicCast"}, {"Sheathed", "MT"}, {"Sneak", "Sneak"}, {"Block1HM", "1HM"},
        {"Block2HM", "2HM"}, {"BlockBow", "Bow"}, {"BlockSneak", "Sneak"}, {"Block2HW", "2HW"}, {"BlockH2H", "H2H"}}};
// XML value, blender part.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kDirections{{{"forward", "Forward"},
    {"forwardLeft", "ForwardLeft"}, {"left", "Left"}, {"backwardLeft", "BackwardLeft"}, {"backward", "Backward"},
    {"backwardRight", "BackwardRight"}, {"right", "Right"}, {"forwardRight", "ForwardRight"}}};
}  // namespace LadderNames

inline std::optional<LadderSet> ParseLadderSet(std::string_view text) {
    for (std::size_t i = 0; i < LadderNames::kSets.size(); ++i)
        if (LadderNames::kSets[i].first == text) return static_cast<LadderSet>(i);
    return std::nullopt;
}
inline std::optional<LadderDirection> ParseLadderDirection(std::string_view text) {
    for (std::size_t i = 0; i < LadderNames::kDirections.size(); ++i)
        if (LadderNames::kDirections[i].first == text) return static_cast<LadderDirection>(i);
    return std::nullopt;
}
// The graph a set's blenders are looked for in: BlockBehavior for the Block sets, every other graph otherwise.
inline constexpr const char* kBlockGraph = "BlockBehavior";
inline bool LadderInBlock(LadderSet set) { return set >= LadderSet::kBlock1HM; }
inline std::string_view LadderSetName(LadderSet set) { return LadderNames::kSets[static_cast<std::size_t>(set)].first; }
inline std::string_view LadderDirectionName(LadderDirection direction) {
    return LadderNames::kDirections[static_cast<std::size_t>(direction)].first;
}
// "1HM_ForwardBlend", "MT_BackwardLeftBlend", ...
inline std::string LadderBlenderName(LadderSet set, LadderDirection direction) {
    std::string out(LadderNames::kSets[static_cast<std::size_t>(set)].second);
    out += '_';
    out += LadderNames::kDirections[static_cast<std::size_t>(direction)].second;
    out += "Blend";
    return out;
}

// <Tree weapon use>: a weapon kind that moves with another kind's locomotion tree in vanilla gets one of its own.
//   2HW  two-handed axes and hammers. Vanilla splits the body: the legs play the 2HM direction ladders (which the rows
//        change) while the arms play a direction-less walk/run overlay (2HW_WalkArmBlend / 2HW_RunArmBlend), so legs
//        and arms drift apart once the legs' playback speeds change.
//   H2H  unarmed. Vanilla plays the one-handed tree whole (the weapon state machine's state 0 names the same
//        1HM_iStateGen as the one-handed states; checked in game 11-15).
enum class TreeWeapon : std::uint8_t { k2HW, kH2H };
enum class TreeUse : std::uint8_t {
    kVanilla,  // no <Tree> for the weapon
    kShared,   // use="2HM" (2HW only): the 2HW locomotion and its sneak use the 2HM generators whole, as the "2HW
               // Move Fix" behavior patch did (same iState 7; the axe is held like a greatsword while moving)
    kOwn,      // use="own": a copy of the other kind's tree playing <prefix>_*.hkx (core/OwnLocomotion.h)
};
// What a weapon kind's own tree is made from. One row per TreeWeapon, in its order.
struct TreeKind {
    std::string_view xml;     // the XML value of `weapon`, and the copy's prefix (nodes, blenders, files)
    std::string_view from;    // the prefix of the tree, blenders and files it copies
    int handType;             // iRightHandType: the weapon state and the block selector entry that are redirected
    bool sharedAllowed;       // use="2HM" exists
    LadderSet set, blockSet;  // the ladder sets its blenders take
};
inline constexpr std::array<TreeKind, 2> kTreeKinds{{{"2HW", "2HM", 6, true, LadderSet::k2HW, LadderSet::kBlock2HW},
    {"H2H", "1HM", 0, false, LadderSet::kH2H, LadderSet::kBlockH2H}}};
inline const TreeKind& TreeKindOf(TreeWeapon weapon) { return kTreeKinds[static_cast<std::size_t>(weapon)]; }
inline std::optional<TreeWeapon> ParseTreeWeapon(std::string_view text) {
    for (std::size_t i = 0; i < kTreeKinds.size(); ++i)
        if (kTreeKinds[i].xml == text) return static_cast<TreeWeapon>(i);
    return std::nullopt;
}
// XML value of `use` ("" = vanilla, not an XML value).
inline constexpr std::array<std::string_view, 3> kTreeUseNames{"", "2HM", "own"};
inline std::optional<TreeUse> ParseTreeUse(std::string_view text) {
    if (text == "2HM") return TreeUse::kShared;
    if (text == "own") return TreeUse::kOwn;
    return std::nullopt;
}
struct TreeSpec {
    TreeUse use = TreeUse::kVanilla;
    std::string sourceName;  // the file that set it
};

inline constexpr std::size_t kLadderSetCount = 16;

// <Stance side="right|left" from="25" full="45" walk="Animations\bowdrawn_walk" run="...">: while an upper-body part
// action plays (body="upper"), its clip's chest turned that way from the front by more than `from` degrees moves the
// legs onto these walk / run files, wholly at `full` (11-23, user decision). `walk` and `run` are path prefixes the
// direction is appended to (`Forward`, `ForwardLeft`, ... `BackwardRight`, then `.hkx`), as vanilla names
// bowdrawn_walkforward.hkx; a missing file keeps that direction's own walk or run. The runtime measures the turn
// (core/UpperLayer.h StanceTurn) and writes kStanceVariable; every split direction blender's walk and run become a blend
// of [left stance, own, right stance] on it (core/LocomotionSplit.h). Not while sneaking or blocking.
enum class StanceSide : std::uint8_t { kRight, kLeft };
struct StanceSpec {
    bool declared = false;
    float from = 25.0f, full = 45.0f;  // degrees
    std::string walk, run;             // path prefixes under Actors\Character, or empty
    std::string sourceName;
};
inline constexpr const char* kStanceVariable = "AI_Stance";  // -1 (left) .. 0 (own) .. 1 (right)
inline constexpr std::array<std::string_view, 2> kStanceSideNames{"right", "left"};
inline std::optional<StanceSide> ParseStanceSide(std::string_view text) {
    if (text == "right") return StanceSide::kRight;
    if (text == "left") return StanceSide::kLeft;
    return std::nullopt;
}
// "Animations\bowdrawn_walk" + "ForwardLeft" + ".hkx"
inline std::string StanceFile(const std::string& prefix, LadderDirection direction) {
    if (prefix.empty()) return {};
    std::string out = prefix;
    out += LadderNames::kDirections[static_cast<std::size_t>(direction)].second;
    out += ".hkx";
    return out;
}
// The share of a side's stance for a chest turned `turn` degrees that way (0 .. 1, linear from `from` to `full`).
inline float StanceShare(const StanceSpec& s, float turn) {
    if (!s.declared || !(s.full > s.from)) return 0.0f;
    const float t = (turn - s.from) / (s.full - s.from);
    return t <= 0.0f ? 0.0f : t >= 1.0f ? 1.0f : t;
}

// Everything a <Locomotion> declares: the sets it leaves vanilla and its trees.
struct LocomotionSpec {
    bool declared = false;                       // a <Locomotion> was read
    std::array<bool, kLadderSetCount> except{};  // by LadderSet: <Except set>
    std::array<TreeSpec, 2> trees;               // by TreeWeapon
    std::array<StanceSpec, 2> stances;           // by StanceSide: <Stance>
    std::string sourceName;                      // the file that declared it
    // The set's direction blenders are split. The sets of a weapon's own tree exist only with that tree.
    bool Applies(LadderSet set) const {
        if (!declared || except[static_cast<std::size_t>(set)]) return false;
        for (std::size_t i = 0; i < kTreeKinds.size(); ++i)
            if ((kTreeKinds[i].set == set || kTreeKinds[i].blockSet == set) && trees[i].use != TreeUse::kOwn)
                return false;
        return true;
    }
    const TreeSpec& Tree(TreeWeapon weapon) const { return trees[static_cast<std::size_t>(weapon)]; }
    TreeUse Use(TreeWeapon weapon) const { return Tree(weapon).use; }
    bool AnyTree() const {
        for (const auto& tree : trees)
            if (tree.use != TreeUse::kVanilla) return true;
        return false;
    }
    bool AnyOwnTree() const {
        for (const auto& tree : trees)
            if (tree.use == TreeUse::kOwn) return true;
        return false;
    }
    bool Empty() const { return !declared; }
    const StanceSpec& Stance(StanceSide side) const { return stances[static_cast<std::size_t>(side)]; }
    bool AnyStance() const { return stances[0].declared || stances[1].declared; }
    // The sets whose direction blenders take the stances: not sneaking, not blocking (a part action ends on a block).
    static bool StanceSet(LadderSet set) { return set != LadderSet::kSneak && !LadderInBlock(set); }
};

// One file declares the locomotion of the whole game: `<declarations>\Locomotion\Locomotion.xml` (names ignoring
// case), and that file holds nothing else. Two mods that ship one put it at the same path, so the mod manager's
// priority picks one of them whole (trees, excepted sets), the same priority that picks between
// their animation files; nothing of the other is mixed in (user decision 11-15). A <Locomotion> in any other file is
// not used (the loader says so).
inline constexpr std::wstring_view kLocomotionFolder = L"Locomotion";
inline constexpr std::wstring_view kLocomotionFile = L"Locomotion.xml";

}  // namespace AnimationInjector
