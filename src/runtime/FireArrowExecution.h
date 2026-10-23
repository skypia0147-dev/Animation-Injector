#pragma once
#include <RE/A/ActorState.h>
#include <RE/N/NiPoint3.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace AnimationInjector {
// FullBodyPlayerIdles can clear weaponState while the bow remains drawn.
inline bool FireArrowDrawReady(RE::ATTACK_STATE_ENUM state) {
    return state == RE::ATTACK_STATE_ENUM::kBowAttached || state == RE::ATTACK_STATE_ENUM::kBowDrawn;
}

template<class Actor>
auto FireArrowEquipment(Actor& actor) {
    return std::pair{actor.GetEquippedObject(true), actor.GetEquippedObject(false)};
}

struct FireArrowResult {
    bool accepted = false, completed = false;
};

// Input release closes charge; it is not the arrow event. Manual release has
// already executed the first two operations in the original input handler.
template<class Engine>
FireArrowResult CompleteFireArrow(bool automatic, Engine& engine) {
    bool accepted = true;
    if (automatic) {
        accepted = engine.ReleaseAction();
        engine.CloseCharge();
    }
    if (engine.Available() && !engine.ArrowReleased()) engine.DeliverArrowEvent();
    return {accepted, engine.Available() && engine.ArrowReleased()};
}

// A direct shot's aim (0.12.271): toward where the crosshair points, as a drawn shot is. The bow's own node turns
// with the dodge clip and once sent arrows backward. Angles as a projectile reads them: x = pitch (positive is down),
// z = heading clockwise from +Y, in [0, 2pi).
inline std::pair<float, float> AimAngles(const RE::NiPoint3& from, const RE::NiPoint3& to) {
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    float heading = std::atan2(dx, dy);
    if (heading < 0.0f) heading += 2.0f * std::numbers::pi_v<float>;
    return {std::atan2(-dz, std::sqrt(dx * dx + dy * dy)), heading};
}
// The crosshair ray starts beside the shooter, not at the camera: what stands between the camera and the shooter
// (a wall behind them) is not the aim. `forward` is a unit vector.
inline RE::NiPoint3 AimRayStart(const RE::NiPoint3& eye, const RE::NiPoint3& forward, const RE::NiPoint3& shooter) {
    const float along =
        (shooter.x - eye.x) * forward.x + (shooter.y - eye.y) * forward.y + (shooter.z - eye.z) * forward.z;
    const float t = std::max(0.0f, along);
    return {eye.x + forward.x * t, eye.y + forward.y * t, eye.z + forward.z * t};
}

// A direct shot's damage (0.12.283): the engine sets an arrow's damage, like its power, from the player's draw (seen
// in game: power 0.35 and a third of a full draw's damage for a shot with no draw). A full draw's damage is the
// launched damage over that power; `multiplier` (the damage percent / 100) scales it.
inline float DirectShotDamage(float launched, float power, float multiplier) {
    const float full = power > 0.0f && power < 1.0f ? launched / power : launched;
    return std::isfinite(multiplier) && multiplier >= 0.0f ? full * multiplier : full;
}

// A direct shot (FireArrowMode::kDirect, 0.12.268): no draw. The engine launches the equipped arrow from the
// equipped bow at full power with its damage scaled; one arrow is taken unless god mode keeps them, as a shot does.
enum class DirectShotResult : std::uint8_t { kLaunched, kNoBow, kNoAmmo, kLaunchFailed };
template<class Engine>
DirectShotResult CompleteDirectShot(float damage, Engine& engine) {
    if (!engine.HasBow()) return DirectShotResult::kNoBow;
    if (engine.AmmoCount() <= 0) return DirectShotResult::kNoAmmo;
    if (!engine.Launch(damage)) return DirectShotResult::kLaunchFailed;
    if (!engine.GodMode()) engine.TakeArrow();
    return DirectShotResult::kLaunched;
}
}
