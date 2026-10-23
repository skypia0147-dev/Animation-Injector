#include "PCH.h"
#include "runtime/FireArrow.h"
#include "runtime/FireArrowState.h"
#include "runtime/FireArrowExecution.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "Settings.h"
#include "core/FireArrowResolver.h"
#include "RE/H/hkpWorldRayCastInput.h"
#include "RE/H/hkpWorldRayCastOutput.h"
#include "RE/H/hkpClosestRayHitCollector.h"
#include "RE/B/bhkPickData.h"
#include <optional>
#include <mutex>

namespace AnimationInjector::FireArrow {
namespace {
std::mutex mutex;
FireArrowCycle cycle;
using Dispatch = bool (*)(RE::PlayerControls*, RE::DEFAULT_OBJECT, std::uint32_t);
Dispatch nativeDispatch = nullptr;
using FinishCharge = void (*)(RE::PlayerCharacter*);
FinishCharge finishCharge = nullptr;
struct NativeArrowEngine {
    RE::PlayerCharacter& player;
    bool ReleaseAction() { return RE::SourceActionMap::DoAction(&player, RE::DEFAULT_OBJECT::kActionRightRelease); }
    void CloseCharge() { finishCharge(&player); }
    bool Available() const {
        return player.Is3DLoaded() && !player.IsDead() && !player.IsInRagdollState() &&
               player.AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal;
    }
    bool ArrowReleased() const {
        return player.AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kBowReleased;
    }
    void DeliverArrowEvent() {
        static const RE::BSFixedString arrowRelease("arrowRelease"), empty;
        RE::BSAnimationGraphEvent event{arrowRelease, &player, empty};
        static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(&player)->ProcessEvent(&event, nullptr);
    }
};
void Trace(const char* phase, RE::DEFAULT_OBJECT action, std::uint32_t flags, bool forward, bool accepted = false) {
    if (!FrameworkSettings::Get().debug) return;
    FireArrowCycle snapshot;
    {
        std::lock_guard lock(mutex);
        snapshot = cycle;
    }
    auto* controls = RE::PlayerControls::GetSingleton();
    auto* h = controls ? controls->attackBlockHandler : nullptr;
    auto* p = RE::PlayerCharacter::GetSingleton();
    const int state = p && p->Is3DLoaded() ? static_cast<int>(p->AsActorState()->GetAttackState()) : -1;
    logger::debug(
        "[FireArrow] native {} action={} flags={} forward={} accepted={} draw={} pressed={} released={} automatic={} fired={} manualPending={} heldRight={} heldLeft={} heldActive={} attackState={}",
        phase, static_cast<int>(action), flags, forward, accepted, snapshot.serial, snapshot.pressed, snapshot.released,
        snapshot.automatic, snapshot.fired, snapshot.manualPending, h && h->heldRight, h && h->heldLeft,
        h && h->heldStateActive, state);
}
bool CodeRange(std::uintptr_t start, std::uintptr_t& end) {
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    if (start < text.address() || start >= text.address() + text.size()) return false;
    DWORD64 base = 0;
    const auto* entry = RtlLookupFunctionEntry(start, &base, nullptr);
    if (!entry || base + entry->BeginAddress != start) return false;
    end = base + entry->EndAddress;
    return end > start && end <= text.address() + text.size();
}
bool Press(RE::PlayerControls* controls, RE::DEFAULT_OBJECT action, std::uint32_t flags) {
    if (action != RE::DEFAULT_OBJECT::kActionRightAttack || flags != 2) {
        Trace("unexpected-press", action, flags, true);
        return nativeDispatch(controls, action, flags);
    }
    {
        std::lock_guard lock(mutex);
        cycle.Press();
    }
    Trace("press-enter", action, flags, true);
    const bool accepted = nativeDispatch(controls, action, flags);
    Trace("press-return", action, flags, true, accepted);
    return accepted;
}
bool Released(RE::PlayerControls* controls, RE::DEFAULT_OBJECT action, std::uint32_t flags) {
    if (action != RE::DEFAULT_OBJECT::kActionRightRelease || flags != 2) {
        Trace("unexpected-release", action, flags, true);
        return nativeDispatch(controls, action, flags);
    }
    bool forward;
    std::uint64_t draw;
    {
        std::lock_guard lock(mutex);
        forward = cycle.NativeRelease();
        draw = cycle.serial;
    }
    Trace("release-enter", action, flags, forward);
    if (!forward) {
        logger::debug(
            "[FireArrow] physical release after automatic shot: duplicate action skipped; native held/charge cleanup continues");
        return true;
    }
    const bool accepted = nativeDispatch(controls, action, flags);
    Trace("release-return", action, flags, true, accepted);
    // Renamed HKX annotations no longer generate the native arrow event. Queue
    // its completion only for our active FireArrow clip. The original caller
    // still closes the charge record and clears held state before actor Update.
    // Only a running runner can take it: every right-hand release used to create a runner for each
    // FireArrow action (934 in the 5000-action test), all updated every frame afterwards.
    Actions::FindRunner(RE::PlayerCharacter::GetSingleton(), [&](int, ActionRunner& runner) {
        runner.QueueFireArrowOnRelease(draw);
        return false;
    });
    return accepted;
}
}
void Install() {
    if (!FrameworkSettings::Get().fireArrow) {
        logger::info("[FireArrow] disabled by Compatibility/bFireArrow; no hooks installed");
        return;
    }
    bool needed = false;
    for (int i = 0; i < Actions::Count(); ++i) needed = needed || Actions::Spec(i).fireArrow.enabled;
    if (!needed || nativeDispatch) return;
    const REL::Relocation<std::uintptr_t*> table{RE::VTABLE_AttackBlockHandler[0]};
    const auto route = FireArrowResolver::Resolve(table.get()[4], CodeRange);
    const REL::Relocation<std::uintptr_t> sourceAction{RELOCATION_ID(33423, 34202)};
    const auto base = REL::Module::get().base();
    if (route)
        logger::info("[FireArrow] route RVA press={:X} release={:X} dispatch={:X} finishCharge={:X}",
            route.pressCall - base, route.releaseCall - base, route.dispatch - base, route.finishCharge - base);
    if (!route || !FireArrowResolver::Validate(route, sourceAction.address(), CodeRange)) {
        logger::error("[FireArrow] native release route did not match; FireArrow disabled (other actions unaffected)");
        return;
    }
    // Only native right-hand press/release call sites are wrapped. The actual
    // ButtonEvent, held flags, other handlers and movement vectors are untouched.
    nativeDispatch = reinterpret_cast<Dispatch>(route.dispatch);
    finishCharge = reinterpret_cast<FinishCharge>(route.finishCharge);
    logger::info("[FireArrow] prepatch verified: E8/E8 targets, native action pipeline, bow charge finalizer");
    SKSE::GetTrampoline().write_call<5>(route.pressCall, Press);
    SKSE::GetTrampoline().write_call<5>(route.releaseCall, Released);
    logger::info("[FireArrow] native right release route ready; no synthetic input; runtime={}",
        REL::Module::get().version().string());
}
void ResetOnLoad() {
    std::lock_guard lock(mutex);
    cycle.Reset();
}
std::uint64_t CurrentDraw() {
    std::lock_guard lock(mutex);
    return cycle.serial;
}
void ObserveArrowRelease(RE::Actor* actor) {
    if (!nativeDispatch || !actor || actor != RE::PlayerCharacter::GetSingleton() || !actor->Is3DLoaded()) return;
    const bool advanced = actor->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kBowReleased;
    if (advanced) {
        std::lock_guard lock(mutex);
        cycle.ObserveArrow();
    }
    logger::debug("[FireArrow] native arrow handler observed stateReleased={}", advanced);
}
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
void Release(RE::Actor* actor, std::uint64_t draw, const char* action, float elapsed) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!actor || actor != player) {
        logger::debug("[FireArrow] action='{}' skipped: player only", action);
        return;
    }
    auto* ui = RE::UI::GetSingleton();
    auto* controls = RE::PlayerControls::GetSingleton();
    auto* handler = controls ? controls->attackBlockHandler : nullptr;
    auto* state = player->AsActorState();
    if (!nativeDispatch || !finishCharge || !player->Is3DLoaded() || player->IsDead() || player->IsInRagdollState() ||
        !state || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || !ui || ui->GameIsPaused() ||
        ui->IsMenuOpen(RE::Console::MENU_NAME) || !handler || controls->blockPlayerInput) {
        logger::debug("[FireArrow] action='{}' skipped: unavailable", action);
        return;
    }
    const auto [left, right] = FireArrowEquipment(*player);
    const auto isBow = [](RE::TESForm* f) {
        auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
        return w && w->GetWeaponType() == RE::WEAPON_TYPE::kBow;
    };
    const auto attackState = state->GetAttackState();
    logger::debug(
        "[FireArrow] eligibility action='{}' left={:08X} leftBow={} right={:08X} rightBow={} drawn={} heldRight={} heldLeft={} heldActive={} attackState={}",
        action, left ? left->GetFormID() : 0, isBow(left), right ? right->GetFormID() : 0, isBow(right),
        player->IsWeaponDrawn(), handler->heldRight, handler->heldLeft, handler->heldStateActive,
        static_cast<int>(attackState));
    if (!isBow(left) && !isBow(right)) {
        logger::debug("[FireArrow] action='{}' skipped: no equipped bow in either hand", action);
        return;
    }
    if (!FireArrowDrawReady(attackState)) {
        logger::debug("[FireArrow] action='{}' at={:.3f} skipped: bow draw not ready attackState={} (expected 9/10)",
            action, elapsed, static_cast<int>(attackState));
        return;
    }
    FireArrowCycle::Shot shot;
    {
        std::lock_guard lock(mutex);
        shot = cycle.Claim(draw);
        if (shot == FireArrowCycle::Shot::kNone && cycle.ClipDraw(draw)) shot = FireArrowCycle::Shot::kClip;
        if (shot == FireArrowCycle::Shot::kNone) {
            logger::debug(
                "[FireArrow] action='{}' skipped: draw mismatch or completed shot; ticket={} current={} pressed={} released={} fired={}",
                action, draw, cycle.serial, cycle.pressed, cycle.released, cycle.fired);
            return;
        }
    }
    const int before = static_cast<int>(state->GetAttackState());
    // RightRelease only completes input/charge. The native arrow handler creates
    // the projectile, consumes ammunition and applies weapon effects. Deliver to
    // the actor's event sink (engine output), NOT NotifyAnimationGraph (input).
    // No attack state, charge/power value or projectile data is manufactured.
    NativeArrowEngine engine{*player};
    // A clip's draw is released as an automatic one: nothing of the player's own release has run.
    const auto [accepted, completed] = CompleteFireArrow(shot != FireArrowCycle::Shot::kManual, engine);
    if (!completed) {
        std::lock_guard lock(mutex);
        if (cycle.serial == draw) cycle.automatic = false;
    }
    logger::debug(
        "[FireArrow] action='{}' at={:.3f} draw={} source={} nativeAccepted={} handlerCompleted={} attackState={}->{} heldRight={} heldLeft={} heldActive={}",
        action, elapsed, draw,
        shot == FireArrowCycle::Shot::kAutomatic ? "annotation/time"
        : shot == FireArrowCycle::Shot::kClip    ? "clip-draw"
                                                 : "physical-release",
        accepted, completed, before, static_cast<int>(state->GetAttackState()), handler->heldRight, handler->heldLeft,
        handler->heldStateActive);
}
}
