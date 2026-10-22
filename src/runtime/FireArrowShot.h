#pragma once
#include <cstdint>

namespace AnimationInjector {
// How an automatic shot leaves the bow (0.12.268).
// kDrawn: the bow must be drawn; the engine releases it as the player would (power = how far it was drawn).
// kDirect: no draw; the equipped arrow is launched at full power (a full draw's speed and range), its damage
//   multiplied by `damage`, and one arrow is taken from the inventory.
enum class FireArrowMode : std::uint8_t { kDrawn, kDirect };
struct FireArrowShot {
    FireArrowMode mode = FireArrowMode::kDrawn;
    float damage = 1.0f;  // kDirect: multiplier on a full draw's damage (XML percent / 100)
    bool operator==(const FireArrowShot&) const = default;
};
}
