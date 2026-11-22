#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Which part of the body an ordinary action plays on (docs/설계-상체레이어.md).
//
// XML: <Action id="Drink" clip="Animations\Drink.hkx" body="rightArm"> ... </Action>
//   body omitted     the whole body, as every action before: a state of the root machine, the actor stops moving
//   body="upper"     the spine and everything above it; the legs keep the locomotion and the actor moves freely.
//                    The upper body keeps the clip's own rotation whatever the legs do (core/UpperLayer.h, stabilizer)
//   body="rightArm"  the right clavicle to the fingers, the right hand's attachment nodes and twist bones; the arm
//   body="leftArm"   (or the left one) rides on the locomotion's torso, so a hand to the mouth stays at the mouth
//
// A part action is not a state: it never sets bAnimationDriven, never takes the actor's movement, and is not entered
// through the root machine. Third person only (user decision 11-20): it does not start in first person and ends when
// the view switches to first person.
//
// What a part action may declare (the parser refuses the rest with the element's name, as other kinds' refusals do):
//   clip, view (third only), loop / maxHold (a part loop plays on until ended; maxHold only when written, or its
//   default 3 s when it has an on="release" transition: a charge's hold), <Require cooldown> (air is always "no": a
//   jump ends it), <Exit blend at>, enterBlend, <Do at> (sound, spell, hide/show, event), the event registry's
//   functions through the clip's annotations, being the destination of a <Start> (key, pad button, game control or
//   event) with its condition cards, and <Transition>s both ways as a full-body action's (11-20, charge: start, hold
//   and release each on its own part or the whole body): key / event / press kinds / on="end" / on="release" /
//   end="true" / condition cards, to and from part and full-body actions.
// Refused for a part action: <Pair>, Carry, QTE, Rotate, IFrame, HyperArmor, TimeScale, Cost, MoveCancel, Recovery,
//   on="move" transitions (it plays while moving), a transition's blend (the destination's enter blend crosses),
//   transitions to or from a pair, being a QTE result's or a partnerTo destination.
namespace AnimationInjector {

enum class BodyPart : std::uint8_t {
    kWhole,     // the default: a full-body state action
    kUpper,     // body="upper"
    kRightArm,  // body="rightArm"
    kLeftArm,   // body="leftArm"
};
inline constexpr std::size_t kBodyLayers = 3;  // the part layers, in their stacking order: upper, right arm, left arm
inline int LayerOf(BodyPart part) { return part == BodyPart::kWhole ? -1 : static_cast<int>(part) - 1; }

inline std::optional<BodyPart> ParseBodyPart(std::string_view text) {
    if (text == "upper") return BodyPart::kUpper;
    if (text == "rightArm") return BodyPart::kRightArm;
    if (text == "leftArm") return BodyPart::kLeftArm;
    return std::nullopt;  // "whole" is not written: the attribute is left out
}
inline std::string_view BodyPartName(BodyPart part) {
    switch (part) {
    case BodyPart::kUpper: return "upper";
    case BodyPart::kRightArm: return "rightArm";
    case BodyPart::kLeftArm: return "leftArm";
    default: return "";
    }
}

}  // namespace AnimationInjector
