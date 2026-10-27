#include "PCH.h"
#include "runtime/FireArrow.h"
#include "runtime/FireArrowExecution.h"
#include "RE/H/hkpWorldRayCastInput.h"
#include "RE/H/hkpWorldRayCastOutput.h"
#include "RE/H/hkpClosestRayHitCollector.h"
#include "RE/B/bhkPickData.h"
#include <optional>

namespace AnimationInjector::FireArrow {
namespace {
// The crosshair's target for a direct shot: the first solid thing an arrow would stop on, never the shooter.
class AimCollector final : public RE::hkpClosestRayHitCollector {
public:
    RE::TESObjectREFR* shooter = nullptr;
    AimCollector() { Reset(); }
    void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hit) override {
        const auto* root = &body;
        while (root->parent) root = root->parent;
        const auto* collidable = static_cast<const RE::hkpCollidable*>(root);
        using L = RE::COL_LAYER;
        switch (collidable->GetCollisionLayer()) {
        case L::kStatic:
        case L::kAnimStatic:
        case L::kTransparent:
        case L::kClutter:
        case L::kBiped:
        case L::kTrees:
        case L::kProps:
        case L::kTerrain:
        case L::kGround:
        case L::kDebrisLarge:
        case L::kTransparentSmall:
        case L::kTransparentSmallAnim:
        case L::kCharController:
        case L::kDeadBip:
        case L::kBipedNoCC: break;
        default: return;
        }
        if (shooter && RE::TESHavokUtilities::FindCollidableRef(*collidable) == shooter) return;
        RE::hkpClosestRayHitCollector::AddRayHit(body, hit);
    }
};
struct Aim {
    RE::NiPoint3 target;
    bool hit = false;
};
// The camera's forward ray from beside the shooter: its first hit, or a point far ahead (8000 units, ~114 m).
std::optional<Aim> CrosshairAim(RE::PlayerCharacter& player) {
    auto* camera = RE::PlayerCamera::GetSingleton();
    const auto root = camera ? camera->cameraRoot : nullptr;
    if (!root) return std::nullopt;
    auto forward = root->world.rotate.GetVectorY();
    const float length = forward.Length();
    if (!std::isfinite(length) || length < 0.0001f) return std::nullopt;
    forward *= 1.0f / length;
    auto chest = player.GetPosition();
    chest.z += player.GetHeight() * 0.75f;
    const auto start = AimRayStart(root->world.translate, forward, chest);
    constexpr float kReach = 8000.0f;
    const RE::NiPoint3 end{start.x + forward.x * kReach, start.y + forward.y * kReach, start.z + forward.z * kReach};
    auto* cell = player.GetParentCell();
    auto* world = cell ? cell->GetbhkWorld() : nullptr;
    if (!world) return Aim{end, false};
    const float scale = RE::bhkWorld::GetWorldScale();
    RE::bhkPickData pick{};
    pick.rayInput.filterInfo.filter = 0;
    pick.rayInput.from = {start.x * scale, start.y * scale, start.z * scale, 0};
    pick.rayInput.to = {end.x * scale, end.y * scale, end.z * scale, 0};
    AimCollector collector;
    collector.shooter = &player;
    pick.rayOutput.Reset();
    pick.closestRayHitCollector = &collector;
    {
        RE::BSReadLockGuard lock(world->worldLock);
        world->PickObject(pick);
    }
    const float fraction = collector.rayHit.hitFraction;
    if (!collector.rayHit.HasHit() || !std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
        return Aim{end, false};
    return Aim{{start.x + forward.x * kReach * fraction, start.y + forward.y * kReach * fraction,
                   start.z + forward.z * kReach * fraction},
        true};
}
RE::TESObjectWEAP* AsBow(RE::TESForm* f) {
    auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
    return w && w->GetWeaponType() == RE::WEAPON_TYPE::kBow ? w : nullptr;
}
// The engine side of CompleteDirectShot. Nothing is kept past the call: the projectile is reached through its
// handle once, right after the launch.
struct NativeDirectShot {
    RE::PlayerCharacter& player;
    RE::TESObjectWEAP* bow = nullptr;
    RE::TESAmmo* ammo = nullptr;
    const char* ammoFrom = "none";
    float before = 0.0f, after = 0.0f;
    float powerBefore = 0.0f;
    std::optional<Aim> aim;
    RE::NiPoint3 origin;
    bool HasBow() {
        const auto [left, right] = FireArrowEquipment(player);
        bow = AsBow(right);
        if (!bow) bow = AsBow(left);
        return bow != nullptr;
    }
    // The process's current ammo is empty while one of our bow clips plays (seen in game, 0.12.269): the arrows
    // worn in the inventory are the equipped ones then.
    std::int32_t AmmoCount() {
        ammo = player.GetCurrentAmmo();
        ammoFrom = ammo ? "process" : "none";
        std::int32_t worn = 0;
        for (auto& [object, data] : player.GetInventory([](RE::TESBoundObject& o) { return o.IsAmmo(); })) {
            if (data.first <= 0 || !data.second) continue;
            if (ammo ? object == ammo : data.second->IsWorn()) {
                if (!ammo) {
                    ammo = object->As<RE::TESAmmo>();
                    ammoFrom = "worn";
                }
                worn = data.first;
                break;
            }
        }
        return ammo ? worn : 0;
    }
    bool Launch(float damage) {
        RE::ProjectileHandle handle;
        // LaunchData's power stays 1.0: a full draw's speed and range. It leaves from the bow toward the crosshair's
        // target; without a camera it falls back to the engine's own aim from the bow node.
        aim = CrosshairAim(player);
        auto* process = player.GetActorRuntimeData().currentProcess;
        auto* node = process ? process->GetWeaponNode(player.GetBiped2()) : nullptr;
        if (aim && node) {
            origin = node->world.translate;
            const auto [pitch, heading] = AimAngles(origin, aim->target);
            RE::Projectile::LaunchArrow(&handle, &player, ammo, bow, origin, {pitch, heading});
        } else
            RE::Projectile::LaunchArrow(&handle, &player, ammo, bow);
        const auto projectile = handle.get();
        if (!projectile) return false;
        auto& data = projectile->GetProjectileRuntimeData();
        // The arrow's power (its speed and gravity) and its damage come from the player's draw, not from LaunchData:
        // a shot with no draw left at power 0.35 and dropped in front (seen with BFCO off, whose GMSTs raise the
        // least power; 0.12.282). Its speed is still zero here and follows the power on its first update; its damage
        // was set at that power (0.12.283).
        powerBefore = data.power;
        if (data.power < 1.0f) data.power = 1.0f;
        before = data.weaponDamage;
        data.weaponDamage = DirectShotDamage(before, powerBefore, damage);
        after = data.weaponDamage;
        return true;
    }
    bool GodMode() { return RE::PlayerCharacter::IsGodMode(); }
    void TakeArrow() { player.RemoveItem(ammo, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr); }
};
}
void Direct(RE::Actor* actor, float damage, const char* action, float elapsed) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!actor || actor != player) {
        logger::debug("[FireArrow] direct action='{}' skipped: player only", action);
        return;
    }
    auto* ui = RE::UI::GetSingleton();
    auto* state = player->AsActorState();
    if (!player->Is3DLoaded() || player->IsDead() || player->IsInRagdollState() || !state ||
        state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || !ui || ui->GameIsPaused() ||
        ui->IsMenuOpen(RE::Console::MENU_NAME)) {
        logger::debug("[FireArrow] direct action='{}' skipped: unavailable", action);
        return;
    }
    NativeDirectShot engine{*player};
    const auto result = CompleteDirectShot(damage, engine);
    static constexpr const char* kResults[] = {"launched", "no bow", "no ammo", "launch failed"};
    // The damage before and after the multiplier: whether the hit follows it is still to be seen in game.
    logger::debug(
        "[FireArrow] direct action='{}' at={:.3f} result={} ammo={:08X} ({}) power {:.2f} -> 1 damage {:.2f} at that "
        "power -> {:.2f} (full draw x{:.2f}) aim={} from=({:.0f},{:.0f},{:.0f}) to=({:.0f},{:.0f},{:.0f})",
        action, elapsed, kResults[static_cast<int>(result)], engine.ammo ? engine.ammo->GetFormID() : 0,
        engine.ammoFrom, engine.powerBefore, engine.before, engine.after, damage,
        engine.aim ? (engine.aim->hit ? "crosshair-hit" : "crosshair-far") : "bow-node", engine.origin.x,
        engine.origin.y, engine.origin.z, engine.aim ? engine.aim->target.x : 0.0f,
        engine.aim ? engine.aim->target.y : 0.0f, engine.aim ? engine.aim->target.z : 0.0f);
}
}
