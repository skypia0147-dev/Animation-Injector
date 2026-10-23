#include "PCH.h"
#include "core/NativePairAlignment.h"
#include "core/SynchronizedSceneView.h"
#include "core/NativePairRoles.h"
#include "core/NativePairMarkCapture.h"
#include "core/NativePairStartPose.h"
#include "core/NativePairObservation.h"
#include "core/NativePairSpace.h"
#include "core/NodeFactory.h"
#include "core/SpaceProbe.h"
#include "runtime/ActionRegistry.h"
#include "runtime/Carry.h"
#include "runtime/PairAlignmentMath.h"
#include "runtime/PairStartBlend.h"
#include "runtime/PairStartCamera.h"
#include "Settings.h"
#include <mutex>
#include <atomic>

namespace AnimationInjector::NativePairAlignment {
namespace {
using Pose = PairAlignmentMath::Pose;
using Mark = PairPlacementMath::Mark;
struct Role {
    RE::ActorHandle actor;
    RE::NiPointer<RE::NiAVObject> scene, root;
    RE::NiPointer<RE::bhkCharacterController> controller;
    Pose before, goal;
    RE::NiTransform captured;
    PairStartBlend::Layer<RE::NiTransform> layer;
    std::uintptr_t generator = 0, character = 0;
    NativePairSpace::Snapshot space;
    RE::FormID lastCell = 0;
    NativePairObservation::Clip observation;
    RE::BSFixedString generatorName;
    bool lead = false;  // The scene's lead character: the engine reorients it smoothly at activation.
};
struct ActivationPlan {
    struct Participant {
        RE::ActorHandle actor;
        Pose goal;
        std::uintptr_t character = 0;
        std::atomic<std::uintptr_t> generator{0};
    };
    std::array<Participant, 2> roles;
    std::uintptr_t nativeScene = 0;
    bool realign = true;
    std::atomic_bool applied{false};
};
struct Blend {
    std::array<Role, 2> roles;
    PairStartBlend::Clock clock;
    std::uint64_t camera = 0;
    float age = 0;
    std::shared_ptr<ActivationPlan> activation;
    unsigned traceSamples = 0;
    std::optional<Carry::NativeStartBridge> carryBridge;
    const char* placement = "shared";
    unsigned headingRestores = 0;
};
// Traced starts (docs/진단-페어시작-순간이동-0.12.161.md): each visible root equalled its actor
// position in every frame, so the root layer written in Update does not reach the rendered frame and
// an instant placement shows as a teleport. At activation the engine reorients the lead smoothly
// from its cached pose to the scene start (rebased onto our goal) but snaps the other role.
// The lead is the victim (prefix 2_; vanilla kill moves too). Every start (0.12.256, user decision)
// keeps the victim's position and only turns it, which the engine does smoothly, and gives the whole
// move to the attacker, which the engine snaps: the player too, when it is the attacker.
std::optional<unsigned> PivotRole(const Blend& blend) {
    if (blend.roles[0].lead != blend.roles[1].lead) return blend.roles[0].lead ? 0u : 1u;
    return std::nullopt;
}
// A player victim keeps its heading (0.12.262): the engine does not turn the player at activation (the
// heading stayed where it was, or went halfway), so the attacker placed for a turned victim stood askew.
bool KeepsHeading(const Blend& blend, unsigned pivot) {
    const auto actor = blend.roles[pivot].actor.get();
    return actor && actor->IsPlayerRef();
}
// Native start / scene writes run on the game update path. Animation workers
// only publish copied clock values, with try_lock to avoid graph-lock inversion.
std::recursive_mutex mutex;
std::vector<std::shared_ptr<Blend>> blends;
// Activation runs under the engine's graph lock. Publish immutable plans so
// that callback never waits for our actor-update mutex (lock inversion).
std::array<std::atomic<std::shared_ptr<ActivationPlan>>, 16> activations;

struct ActivationHook {
    static void Thunk(RE::BGSSynchronizedAnimationInstance* scene) {
        // Native SyncActivate finishes caching both world transforms before
        // this vfunc. The final participant makes the engine align the pair
        // AGAIN from those caches. Do not write character.worldFromModel or
        // replay actor placement every frame to fight that second alignment.
        {
            for (auto& slot : activations) {
                auto blend = slot.load();
                if (!blend || blend->nativeScene != reinterpret_cast<std::uintptr_t>(scene) || blend->applied.load())
                    continue;
                const auto* view = reinterpret_cast<const SynchronizedSceneView*>(scene);
                if (!view || !view->numActors || view->numActivated != view->numActors - 1 || view->actors.size() < 2 ||
                    view->actors.size() > 8)
                    continue;
                std::array<RE::hkQsTransform, 8> starts{};
                bool valid = true;
                unsigned seen = 0;
                for (std::size_t i = 0; i < view->actors.size(); ++i) {
                    const auto& info = view->actors[i];
                    auto* gen = info.generator;
                    const int role = info.actor == blend->roles[0].actor   ? 0
                                     : info.actor == blend->roles[1].actor ? 1
                                                                           : -1;
                    if (role < 0 || !NodeFactory::SameVTable(gen, RE::VTABLE_BSSynchronizedClipGenerator) ||
                        gen->synchronizedScene != scene) {
                        valid = false;
                        break;
                    }
                    if (blend->realign) {
                        const auto start =
                            NativePairStartPose::Rebase(gen->startingWorldFromModel, blend->roles[role].goal);
                        if (!start) {
                            valid = false;
                            break;
                        }
                        starts[i] = *start;
                    }
                    seen |= 1u << role;
                }
                if (!valid || seen != 3) {
                    logger::warn("[NativePairAlign] activation cache unavailable; native caches retained");
                    continue;
                }
                // Validate every participant before committing any cache. The
                // native callback still owns counters and mark composition.
                for (std::size_t i = 0; i < view->actors.size(); ++i) {
                    auto* gen = view->actors[i].generator;
                    if (blend->realign) {
                        gen->startingWorldFromModel = starts[i];
                        gen->worldFromModelWithRootMotion = starts[i];
                    }
                    for (auto& role : blend->roles)
                        if (view->actors[i].actor == role.actor &&
                            reinterpret_cast<std::uintptr_t>(view->actors[i].character) == role.character)
                            role.generator.store(reinterpret_cast<std::uintptr_t>(gen));
                }
                blend->applied.store(true);
                logger::debug(
                    "[NativePairAlign] activation ready scene={:X} realign={} participants={} goals=({:.3f},{:.3f},{:.5f})/({:.3f},{:.3f},{:.5f})",
                    blend->nativeScene, blend->realign, view->actors.size(), blend->roles[0].goal.x,
                    blend->roles[0].goal.y, blend->roles[0].goal.yaw, blend->roles[1].goal.x, blend->roles[1].goal.y,
                    blend->roles[1].goal.yaw);
            }
        }
        original(scene);
    }
    static inline REL::Relocation<decltype(Thunk)> original;
};
bool Alive(RE::Actor* actor) { return actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState(); }
NativePairSpace::Snapshot SpaceOf(RE::Actor* actor) {
    auto* cell = actor ? actor->GetParentCell() : nullptr;
    auto* world = cell ? actor->GetWorldspace() : nullptr;
    auto* controller = actor ? actor->GetCharController() : nullptr;
    auto* physics = controller ? controller->GetHavokWorld() : nullptr;
    return {{cell ? cell->GetFormID() : 0, world ? world->GetFormID() : 0, cell && cell->IsInteriorCell()},
        reinterpret_cast<std::uintptr_t>(physics ? physics->GetWorld1() : nullptr)};
}
Pose Current(RE::Actor* actor) {
    const auto p = actor->GetPosition();
    return {p.x, p.y, p.z, actor->GetAngleZ()};
}
bool Valid(const RE::NiTransform& value) {
    if (!std::isfinite(value.scale) || value.scale < .01f || value.scale > 100) return false;
    for (float x : {value.translate.x, value.translate.y, value.translate.z})
        if (!std::isfinite(x)) return false;
    for (const auto& row : value.rotate.entry)
        for (float x : row)
            if (!std::isfinite(x)) return false;
    return true;
}
bool Equal(const RE::NiTransform& a, const RE::NiTransform& b) {
    if (!Valid(a) || !Valid(b) || (a.translate - b.translate).SqrLength() > .0001f ||
        std::abs(a.scale - b.scale) > .00001f)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            if (std::abs(a.rotate.entry[i][j] - b.rotate.entry[i][j]) > .00001f) return false;
    return true;
}
const char* InvalidRole(const Role& role, RE::Actor* actor) {
    // The blend owns a visible root, not a physics controller. A live controller
    // replacement must not cancel it; Place separately checks its controller.
    if (!Alive(actor)) return "participant-unavailable";
    if (!NativePairSpace::Compatible(role.space, SpaceOf(actor))) return "space-or-physics-changed";
    if (actor->Get3D(false) != role.scene.get()) return "actor-3d-changed";
    if (!role.root || !role.root->parent) return "root-detached";
    auto* node = role.root.get();
    for (unsigned i = 0; node && i < 64; ++i, node = node->parent)
        if (node == role.scene.get()) return nullptr;
    return "root-hierarchy-changed";
}
bool Same(const Role& role, RE::Actor* actor) { return InvalidRole(role, actor) == nullptr; }
void Refresh(Role& role) {
    RE::NiUpdateData update{};
    update.flags = RE::NiUpdateData::Flag::kDisableCollision;
    role.scene->UpdateDownwardPass(update, 0);
}
void Restore(Blend& blend) {
    for (auto& role : blend.roles) {
        auto actor = role.actor.get();
        if (Same(role, actor.get()) && role.layer.Restore(role.root->local, Equal))
            Refresh(role);
        else
            role.layer.owned = false;
    }
}
void Close(Blend& blend, bool restore) {
    if (blend.activation)
        for (auto& slot : activations) {
            auto expected = blend.activation;
            slot.compare_exchange_strong(expected, nullptr);
        }
    if (restore) Restore(blend);
    PairStartCamera::Release(blend.camera);
    blend.camera = 0;
}
bool ReadRole(const SynchronizedSceneView::ActorInfo& info, Role& role, float& duration, std::uintptr_t sceneId,
    unsigned roleIndex) {
    auto actor = info.actor.get();
    auto* gen = info.generator;
    // Startup-only diagnostics. Preserve guard order and never load an animation
    // or retain a borrowed pointer just to describe a failed native start.
    const auto actorId = actor ? actor->GetFormID() : 0;
    const auto fail = [&](const char* reason) {
        logger::debug(
            "[NativePairDiag] scene={:X} role={} actor={:08X} failed={}", sceneId, roleIndex, actorId, reason);
        return false;
    };
    if (!actor) return fail("actor-handle-unavailable");
    if (!actor->Is3DLoaded()) return fail("actor-3d-unloaded");
    if (actor->IsDead()) return fail("actor-dead");
    if (actor->IsInRagdollState()) return fail("actor-ragdoll");
    if (!gen) return fail("sync-generator-null");
    if (!info.character) return fail("character-null");
    if (!NodeFactory::SameVTable(gen, RE::VTABLE_BSSynchronizedClipGenerator)) return fail("sync-generator-type");
    logger::debug(
        "[NativePairDiag] scene={:X} role={} actor={:08X} lead={} applyMotionFromRoot={} ignoreMark={} reorient={} getToMark={:.4f} prefix='{:.120s}'",
        sceneId, roleIndex, actorId, gen->leadCharacter, gen->applyMotionFromRoot, gen->syncClipIgnoreMarkPlacement,
        gen->reorientSupportChar, gen->getToMarkTime,
        gen->syncAnimPrefix.c_str() ? gen->syncAnimPrefix.c_str() : "<null>");
    if (gen->applyMotionFromRoot) return fail("apply-motion-from-root");
    if (gen->syncClipIgnoreMarkPlacement) return fail("ignore-mark-placement");
    if (!gen->clipGenerator) return fail("clip-generator-null");
    if (!actor->GetCharController()) return fail("controller-null");
    RE::BSAnimationGraphManagerPtr manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) return fail("graph-manager-unavailable");
    RE::BSSpinLockGuard graphLock(manager->GetRuntimeData().updateLock);
    bool matched = false;
    for (const auto& graph : manager->graphs)
        if (graph && &graph->characterInstance == info.character) matched = true;
    if (!matched) return fail("character-not-in-actor-graphs");
    auto* scene = actor->Get3D(false);
    static const RE::BSFixedString humanRoot("NPC Root [Root]");
    auto* root = scene ? scene->GetObjectByName(humanRoot) : nullptr;
    if (!root) {
        const auto* setup = info.character->setup.get();
        const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
        if (!skeleton || skeleton->bones._size <= 0 || skeleton->bones._size > 4096 || !skeleton->bones._data)
            return fail("fallback-skeleton-unavailable");
        const char* name = skeleton->bones._data[0].name.c_str();
        logger::debug("[NativePairDiag] scene={:X} role={} humanRoot=false skeletonRoot='{:.120s}' bones={}", sceneId,
            roleIndex, name ? name : "<null>", skeleton->bones._size);
        if (name && *name) root = scene->GetObjectByName(RE::BSFixedString(name));
    }
    if (!root) return fail("visual-root-not-found");
    if (root == scene) return fail("visual-root-is-scene");
    if (!root->parent) return fail("visual-root-detached");
    if (!Valid(root->world)) return fail("visual-root-transform-invalid");
    role.actor = info.actor;
    role.scene.reset(scene);
    role.root.reset(root);
    role.controller.reset(actor->GetCharController());
    role.space = SpaceOf(actor.get());
    role.lastCell = role.space.region.cell;
    role.before = Current(actor.get());
    role.captured = root->world;
    role.generator = reinterpret_cast<std::uintptr_t>(gen);
    role.character = reinterpret_cast<std::uintptr_t>(info.character);
    role.generatorName = gen->name.c_str();
    role.lead = gen->leadCharacter;
    int action = -1, variant = -1;
    if (Actions::FindClip(gen->clipGenerator->name.c_str(), action, variant) && Actions::Spec(action).paired.enabled) {
        const float enter = Actions::Spec(action).enterBlend;
        duration = enter >= 0 && std::isfinite(enter) ? std::clamp(enter, 0.f, 5.f) : .2f;
    } else
        duration = std::isfinite(gen->getToMarkTime) && gen->getToMarkTime > 0
                       ? std::clamp(gen->getToMarkTime, 0.f, 5.f)
                       : .2f;
    if (!PairAlignmentMath::Valid(role.before)) return fail("actor-pose-invalid");
    if (const auto* reason = InvalidRole(role, actor.get())) return fail(reason);
    logger::debug(
        "[NativePairDiag] scene={:X} role={} ready root='{:.120s}' actorXYZ=({:.3f},{:.3f},{:.3f}) yaw={:.5f} blend={:.3f}",
        sceneId, roleIndex, root->name.c_str() ? root->name.c_str() : "<null>", role.before.x, role.before.y,
        role.before.z, role.before.yaw, duration);
    return true;
}
// Keep the third-person view still while the player's body turns during a pair start.
void KeepCameraYaw(float turn) {
    if (!std::isfinite(turn) || turn == 0) return;
    if (auto* camera = RE::PlayerCamera::GetSingleton();
        camera && camera->currentState && camera->currentState->id == RE::CameraStates::kThirdPerson) {
        auto* third = static_cast<RE::ThirdPersonState*>(camera->currentState.get());
        if (third->freeRotationEnabled && std::isfinite(third->freeRotation.x))
            third->freeRotation.x = PairAlignmentMath::Wrap(third->freeRotation.x - turn);
    }
}
void Place(Role& role, Pose pose) {
    auto actor = role.actor.get();
    if (!Same(role, actor.get()) || actor->GetCharController() != role.controller.get()) return;
    const float turn = PairAlignmentMath::Wrap(pose.yaw - actor->GetAngleZ());
    auto angle = actor->GetAngle();
    angle.z = pose.yaw < 0 ? pose.yaw + 2 * PairAlignmentMath::pi : pose.yaw;
    actor->SetAngle(angle);
    actor->SetPosition({pose.x, pose.y, pose.z}, true);
    actor->Update3DPosition(true);
    actor->UpdateActor3DPosition();
    const float scale = RE::bhkWorld::GetWorldScale();
    // SetPosition can cross a cell boundary. Revalidate after engine updates
    // before writing the retained controller rather than a replaced object.
    if (Same(role, actor.get()) && actor->GetCharController() == role.controller.get())
        role.controller->SetPositionImpl({pose.x * scale, pose.y * scale, pose.z * scale, 0}, true, true);
    // No gravity, simulation flags, AI package, movement vector or input locks.
    if (actor->IsPlayerRef()) KeepCameraYaw(turn);
}
// The snapped role keeps its goal heading through the start blend (0.12.279): a turn something else put on it
// (TDM before its yaw lease is granted, or a mod without such an API) is undone, position untouched. A player's
// camera stays where it is, as at the placement.
bool HoldHeading(Role& role) {
    auto actor = role.actor.get();
    if (!Same(role, actor.get()) || actor->GetCharController() != role.controller.get()) return false;
    const float off = PairAlignmentMath::Wrap(actor->GetAngleZ() - role.goal.yaw);
    if (!PairAlignmentMath::HeadingDrifted(off)) return false;
    auto angle = actor->GetAngle();
    angle.z = role.goal.yaw < 0 ? role.goal.yaw + 2 * PairAlignmentMath::pi : role.goal.yaw;
    actor->SetAngle(angle);
    if (actor->IsPlayerRef()) KeepCameraYaw(-off);
    return true;
}
void CaptureCarryBridge(Blend& pending) {
    Carry::NativeStartBridge bridge;
    if (!Carry::ReadNativeStartBridge(pending.roles[0].actor, pending.roles[1].actor,
            {pending.roles[0].generatorName, pending.roles[1].generatorName}, bridge))
        return;
    for (unsigned i = 0; i < 2; ++i) {
        if (bridge.roles[i].scene != pending.roles[i].scene) return;
    }
    pending.carryBridge = std::move(bridge);
}
bool Prepare(const std::shared_ptr<Blend>& pending, const std::array<Mark, 2>& marks, const std::array<float, 2>& times,
    std::uintptr_t sceneId, const std::optional<PairAlignmentMath::Plan>& redirected) {
    auto a = pending->roles[0].actor.get(), b = pending->roles[1].actor.get();
    for (const auto& role : pending->roles) {
        auto actor = role.actor.get();
        if (!Same(role, actor.get()) || actor->GetCharController() != role.controller.get()) return false;
    }
    const auto skipped = [&](const char* reason) {
        logger::debug("[NativePairAlign] scene={:X} prepare skipped reason={}", sceneId, reason);
    };
    std::lock_guard lock(mutex);
    if (blends.size() >= 16) {
        skipped("capacity");
        return false;
    }
    for (const auto& active : blends)
        for (const auto& role : active->roles)
            if (role.actor == pending->roles[0].actor || role.actor == pending->roles[1].actor) {
                skipped("existing-start-owner");
                return false;
            }
    // Native Start has already accepted its original placement. Keep its leader
    // height and authored relative Z; only distribute XY/yaw between both roles.
    auto starts = std::array{pending->roles[0].before, pending->roles[1].before};
    starts[1].z = Current(b.get()).z;
    // pivot-redirected: the engine's attacker placement went to our goal (Start::NativeMove).
    // pivot-placed: the engine placed the attacker at the victim's current facing; our goal makes it exact.
    // shared: no single lead or unsupported marks; both move toward a common midpoint (earlier rule).
    const auto pivot = PivotRole(*pending);
    auto plan = redirected;
    pending->placement = "pivot-redirected";
    if (!plan && pivot) {
        plan = PairAlignmentMath::Pivoted({pending->roles[0].before, pending->roles[1].before}, marks[0], marks[1],
            *pivot, KeepsHeading(*pending, *pivot));
        pending->placement = "pivot-placed";
    }
    const bool pivoted = pivot && plan;
    if (!plan) {
        plan = PairAlignmentMath::Solve(starts, marks[0], marks[1]);
        pending->placement = "shared";
    }
    const bool realign = plan.has_value();
    for (unsigned i = 0; i < 2; ++i)
        pending->roles[i].goal = plan ? plan->goals[i] : Current(pending->roles[i].actor.get().get());
    if (!realign)
        logger::debug(
            "[NativePairAlign] scene={:X} native placement retained for unsupported mark geometry; visual blend and camera guard remain active",
            sceneId);
    pending->activation = std::make_shared<ActivationPlan>();
    pending->activation->realign = realign;
    pending->activation->nativeScene = sceneId;
    for (unsigned i = 0; i < 2; ++i) {
        auto& output = pending->activation->roles[i];
        const auto& role = pending->roles[i];
        output.actor = role.actor;
        output.goal = role.goal;
        output.character = role.character;
        output.generator = role.generator;
    }
    bool published = false;
    for (auto& slot : activations) {
        std::shared_ptr<ActivationPlan> expected;
        if (slot.compare_exchange_strong(expected, pending->activation)) {
            published = true;
            break;
        }
    }
    if (!published) {
        Close(*pending, false);
        skipped("activation-capacity");
        return false;
    }
    if (pending->carryBridge) {
        if (!Carry::CommitNativeStartBridge(*pending->carryBridge)) {
            Close(*pending, false);
            skipped("carry-bridge-changed");
            return false;
        }
        pending->camera = pending->carryBridge->camera;
    } else
        pending->camera = PairStartCamera::Acquire(a.get(), b.get());
    if ((a->IsPlayerRef() || b->IsPlayerRef()) && !pending->camera) {
        Close(*pending, false);
        skipped("camera-owner-unavailable");
        return false;
    }
    pending->clock.Begin(std::max(times[0], times[1]));
    // A pivot places the attacker exactly (position and heading; the engine snaps it anyway) and never
    // turns the victim here: activation turns the lead smoothly. A victim the engine moved goes back.
    if (pivoted) {
        for (unsigned i = 0; i < 2; ++i) {
            auto& role = pending->roles[i];
            if (i != *pivot)
                Place(role, role.goal);
            else if (PairAlignmentMath::Distance(Current(role.actor.get().get()), role.before) > .01f)
                Place(role, role.before);
        }
    } else if (realign)
        for (auto& role : pending->roles) Place(role, role.goal);
    logger::debug(
        "[NativePairAlign] start scene={:X} realign={} policy=native-first placement={} pivot={} actors={:08X}/{:08X} blend={:.3f} move={:.3f}/{:.3f}",
        sceneId, realign, pending->placement, pivot ? static_cast<int>(*pivot) : -1, a->GetFormID(), b->GetFormID(),
        pending->clock.duration, PairAlignmentMath::Distance(pending->roles[0].before, pending->roles[0].goal),
        PairAlignmentMath::Distance(pending->roles[1].before, pending->roles[1].goal));
    // Keep the visual transition ready before the caller activates the scene.
    blends.push_back(pending);
    return true;
}
}
struct Start::State {
    std::shared_ptr<Blend> blend, deferred;
    NativePairMarkCapture::Capture native;
    std::array<float, 2> times{};
    bool completed = false;
    std::uintptr_t diagnosticScene = 0;
    PairedSession::Ticket carryTicket;
    std::optional<PairAlignmentMath::Plan> redirected;  // Victim-pivot goals, decided at native placement.
};
// [Compatibility] iNativePairAlignment: 0 off, 1 only scenes that play an Animation Injector pair
// clip, 2 every native paired scene (kill moves and other mods included).
static bool InScope(const SynchronizedSceneView* view) {
    const int mode = FrameworkSettings::Get().nativePairAlignment;
    if (mode >= 2) return true;
    if (mode <= 0) return false;
    for (std::size_t i = 0; i < view->actors.size(); ++i) {
        const auto* gen = view->actors[i].generator;
        int action = -1, variant = -1;
        if (NodeFactory::SameVTable(gen, RE::VTABLE_BSSynchronizedClipGenerator) && gen->clipGenerator &&
            gen->clipGenerator->name.c_str() && Actions::FindClip(gen->clipGenerator->name.c_str(), action, variant) &&
            Actions::Spec(action).paired.enabled)
            return true;
    }
    return false;
}
Start::Start(RE::BGSSynchronizedAnimationInstance* scene) : state(std::make_unique<State>()) {
    const auto skipped = [&](const char* reason) {
        logger::debug("[NativePairAlign] skip scene={:X} reason={}", reinterpret_cast<std::uintptr_t>(scene), reason);
    };
    if (!scene || !NodeFactory::SameVTable(scene, RE::VTABLE_BGSSynchronizedAnimationInstance)) {
        skipped("scene-type");
        return;
    }
    const auto* view = reinterpret_cast<const SynchronizedSceneView*>(scene);
    if (view->actors.size() < 2 || view->actors.size() > 8) {
        skipped("participant-count");
        return;
    }
    if (!InScope(view)) {
        skipped("compatibility-scope");
        return;
    }
    std::array<NativePairRoles::Role, 8> identities{};
    for (std::size_t i = 0; i < view->actors.size(); ++i) {
        const auto& info = view->actors[i];
        const bool valid = NodeFactory::SameVTable(info.generator, RE::VTABLE_BSSynchronizedClipGenerator);
        identities[i] = {info.actor.native_handle(), valid, valid && info.generator->leadCharacter};
    }
    const auto roles = NativePairRoles::Select({identities.data(), view->actors.size()});
    if (!roles) {
        skipped("physical-role-selection");
        return;
    }
    const auto& first = view->actors[(*roles)[0]];
    const auto& second = view->actors[(*roles)[1]];
    auto a = first.actor.get(), b = second.actor.get();
    if (!Alive(a.get()) || !Alive(b.get()) || a == b) {
        skipped("participant-lifetime");
        return;
    }
    const auto spaceA = SpaceOf(a.get()), spaceB = SpaceOf(b.get());
    logger::debug(
        "[NativePairAlign] participants scene={:X} cells={:08X}/{:08X} worlds={:08X}/{:08X} interior={}/{} physicsShared={}",
        reinterpret_cast<std::uintptr_t>(scene), spaceA.region.cell, spaceB.region.cell, spaceA.region.world,
        spaceB.region.world, spaceA.region.interior, spaceB.region.interior,
        spaceA.physics && spaceA.physics == spaceB.physics);
    if (!NativePairSpace::Compatible(spaceA, spaceB)) {
        skipped("different-space-or-physics");
        return;
    }
    if (Carry::BlocksAction(a.get()) || Carry::BlocksAction(b.get())) {
        skipped("carry-owned");
        return;
    }
    auto pending = std::make_shared<Blend>();
    std::array<float, 2> times{};
    // Solver order: support actor, then native leader (the usual target).
    const auto sceneId = reinterpret_cast<std::uintptr_t>(scene);
    state->diagnosticScene = sceneId;
    logger::debug(
        "[NativePairDiag] scene={:X} roles=({:08X},{:08X}) entries=({},{}) count={} phase=before-native-start", sceneId,
        a->GetFormID(), b->GetFormID(), (*roles)[0], (*roles)[1], view->actors.size());
    // Describe both selected generators even when role capture fails early.
    // Selection validated their types; native Start owns all mark sampling.
    for (unsigned i = 0; i < 2; ++i) {
        const auto& info = view->actors[(*roles)[i]];
        const auto* gen = info.generator;
        logger::debug(
            "[NativePairDiag] scene={:X} role={} selected lead={} prefix='{:.120s}' applyMotionFromRoot={} ignoreMark={} clipPresent={} characterPresent={}",
            sceneId, i, gen->leadCharacter, gen->syncAnimPrefix.c_str() ? gen->syncAnimPrefix.c_str() : "<null>",
            gen->applyMotionFromRoot, gen->syncClipIgnoreMarkPlacement, gen->clipGenerator != nullptr,
            info.character != nullptr);
    }
    if (!ReadRole(first, pending->roles[0], times[0], sceneId, 0) ||
        !ReadRole(second, pending->roles[1], times[1], sceneId, 1))
        return;
    // Capture original visible poses without moving actors or loading bindings.
    // Every entry uses the engine's own GetMark and Start result, including OAR.
    CaptureCarryBridge(*pending);
    if (pending->carryBridge) state->carryTicket = pending->carryBridge->ticket;
    state->deferred = pending;
    state->times = times;
    for (unsigned i = 0; i < 2; ++i)
        state->native.identities[i] = {pending->roles[i].generator, pending->roles[i].character};
    logger::debug("[NativePairAlign] scene={:X} awaiting native approval and marks; original poses captured", sceneId);
}
Start::Move Start::NativeMove(
    std::uint64_t request, const RE::bhkCharacterController* moving, RE::hkVector4& position) {
    if (!state || state->completed || !state->deferred || !moving) return Move::kNative;
    const auto& pending = *state->deferred;
    const auto role = PivotRole(pending);
    if (!role) return Move::kNative;
    const unsigned pivot = *role, mover = 1 - pivot;
    if (!state->redirected) {
        if (!state->native.seen[0] || !state->native.seen[1] || state->native.invalid) return Move::kNative;
        const auto marks = state->native.SolverMarks();
        const auto plan = PairAlignmentMath::Pivoted({pending.roles[0].before, pending.roles[1].before}, marks[0],
            marks[1], pivot, KeepsHeading(pending, pivot));
        auto attacker = pending.roles[mover].actor.get(), victim = pending.roles[pivot].actor.get();
        if (!plan || !attacker || !victim) return Move::kNative;
        const auto& goal = plan->goals[mover];
        // The engine checks only the destination; the sweep also refuses a move through a wall.
        const auto path = SpaceProbe::Path(request, attacker.get(), victim.get(), {goal.x, goal.y, goal.z});
        logger::info(
            "[NativePairAlign] #{} pivot victim={:08X} turn={:.1f}deg attacker={:08X} move={:.1f} turn={:.1f}deg path={}",
            request, victim->GetFormID(),
            std::abs(PairAlignmentMath::Wrap(plan->goals[pivot].yaw - pending.roles[pivot].before.yaw)) * 180 /
                PairAlignmentMath::pi,
            attacker->GetFormID(), PairAlignmentMath::Distance(pending.roles[mover].before, goal),
            std::abs(PairAlignmentMath::Wrap(goal.yaw - pending.roles[mover].before.yaw)) * 180 / PairAlignmentMath::pi,
            path == ConditionFlow::Value::kTrue    ? "clear"
            : path == ConditionFlow::Value::kFalse ? "blocked"
                                                   : "unavailable");
        if (path == ConditionFlow::Value::kFalse) return Move::kBlocked;
        state->redirected = plan;
    }
    if (moving == pending.roles[pivot].controller.get()) return Move::kSkipped;
    if (moving != pending.roles[mover].controller.get()) return Move::kNative;
    const auto& goal = state->redirected->goals[mover];
    position = RE::hkVector4(goal.x, goal.y, goal.z, position.quad.m128_f32[3]);
    return Move::kRedirected;
}
void Start::RecordNativeMark(RE::BSSynchronizedClipGenerator* generator, RE::hkbCharacter* character, bool accepted,
    const RE::hkQsTransform& value) {
    if (!state || state->completed || !state->deferred) return;
    Mark mark;
    if (accepted) {
        for (unsigned i = 0; i < 3; ++i) mark.translation[i] = value.translation.quad.m128_f32[i];
        for (unsigned i = 0; i < 4; ++i) mark.rotation[i] = value.rotation.vec.quad.m128_f32[i];
    }
    const int role = state->native.Record(
        {reinterpret_cast<std::uintptr_t>(generator), reinterpret_cast<std::uintptr_t>(character)}, accepted, mark);
    logger::debug(
        "[NativePairAlign] scene={:X} native mark role={} accepted={} valid={} xyz=({:.4f},{:.4f},{:.4f}) quat=({:.5f},{:.5f},{:.5f},{:.5f})",
        state->diagnosticScene, role, accepted, accepted && PairPlacementMath::Valid(mark), mark.translation[0],
        mark.translation[1], mark.translation[2], mark.rotation[0], mark.rotation[1], mark.rotation[2],
        mark.rotation[3]);
}
Start::~Start() {
    if (state && !state->completed) Complete(false);
}
void Start::Complete(bool accepted) {
    if (!state || state->completed) return;
    state->completed = true;
    if (state->deferred) {
        // Native Start only prepares placement. Its caller activates the scene
        // after return; publish before that existing activation barrier runs.
        const auto* scene = reinterpret_cast<const SynchronizedSceneView*>(state->diagnosticScene);
        const bool ready = state->native.Ready(accepted) && scene && scene->numActivated == 0;
        if (ready && Prepare(state->deferred, state->native.SolverMarks(), state->times, state->diagnosticScene,
                         state->redirected)) {
            state->blend = state->deferred;
            logger::debug("[NativePairAlign] scene={:X} native-approved placement prepared before activation",
                state->diagnosticScene);
        } else
            logger::debug(
                "[NativePairAlign] scene={:X} native-approved placement skipped accepted={} marks={}/{} invalid={} activated={}",
                state->diagnosticScene, accepted, state->native.seen[0], state->native.seen[1], state->native.invalid,
                scene ? scene->numActivated : 0);
        state->deferred.reset();
    }
    if (state->diagnosticScene)
        logger::debug("[NativePairDiag] scene={:X} phase=after-native-start accepted={} alignmentPrepared={}",
            state->diagnosticScene, accepted, state->blend != nullptr);
    if (!state->blend) {
        if (state->carryTicket.request) Carry::AbandonNativeStartBridge(state->carryTicket);
        return;
    }
    std::lock_guard lock(mutex);
    for (unsigned i = 0; i < 2; ++i) {
        const auto& role = state->blend->roles[i];
        auto actor = role.actor.get();
        if (state->blend->activation->realign && Same(role, actor.get()))
            logger::debug(
                "[NativePairAlign] goal role={} placement={} goalZ={:.4f} positionError={:.4f} yawError={:.5f}", i,
                state->blend->placement, role.goal.z, PairAlignmentMath::Distance(Current(actor.get()), role.goal),
                PairAlignmentMath::Wrap(actor->GetAngleZ() - role.goal.yaw));
    }
    state->blend.reset();
}
void Observe(const RE::BSSynchronizedClipGenerator& node, const RE::hkbContext& context, bool exiting) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (auto& blend : blends)
        for (unsigned i = 0; i < 2; ++i) {
            auto& role = blend->roles[i];
            const auto generator = blend->activation ? blend->activation->roles[i].generator.load() : role.generator;
            if (generator == reinterpret_cast<std::uintptr_t>(&node) &&
                role.character == reinterpret_cast<std::uintptr_t>(context.character)) {
                const auto* control = node.clipGenerator ? node.clipGenerator->animationControl.get() : nullptr;
                role.observation.Observe(blend->activation && blend->activation->applied.load(),
                    blend->activation ? blend->activation->nativeScene : 0,
                    reinterpret_cast<std::uintptr_t>(node.synchronizedScene), exiting,
                    control ? control->localTime : -1);
            }
        }
}
void BeforeUpdate() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui || ui->GameIsPaused()) return;
    std::lock_guard lock(mutex);
    for (auto& blend : blends) Restore(*blend);
}
void Update(float delta) {
    if (!std::isfinite(delta) || delta <= 0) return;
    std::lock_guard lock(mutex);
    for (auto it = blends.begin(); it != blends.end();) {
        auto& blend = **it;
        blend.age += delta;
        Restore(blend);
        bool valid = blend.age <= 6;
        const char* reason = valid ? "playing" : "startup-timeout";
        for (auto& role : blend.roles) {
            auto actor = role.actor.get();
            const auto* failure = InvalidRole(role, actor.get());
            const bool same = !failure;
            if (role.observation.ended) {
                valid = false;
                reason = "clip-ended";
            }
            if (!same) {
                valid = false;
                reason = failure;
            }
            if (same) {
                const auto cell = actor->GetParentCell()->GetFormID();
                if (cell != role.lastCell) {
                    logger::debug(
                        "[NativePairAlign] exterior cell crossing retained actor={:08X} cell={:08X}->{:08X} age={:.3f}",
                        actor->GetFormID(), role.lastCell, cell, blend.age);
                    role.lastCell = cell;
                }
            } else if (Alive(actor.get())) {
                const auto now = SpaceOf(actor.get());
                logger::debug(
                    "[NativePairAlign] role invalid actor={:08X} reason={} cell={:08X}->{:08X} world={:08X}->{:08X} interior={}->{} physicsSame={}",
                    actor->GetFormID(), failure, role.space.region.cell, now.region.cell, role.space.region.world,
                    now.region.world, role.space.region.interior, now.region.interior,
                    role.space.physics && role.space.physics == now.physics);
            }
            if (same && blend.age > .25f) {
                static const RE::BSFixedString syncedName("bIsSynced");
                bool synced = false;
                const bool read = actor->GetGraphVariableBool(syncedName, synced);
                if (read && !synced && !actor->IsInKillMove()) {
                    valid = false;
                    reason = "sync-state-ended";
                }
            }
        }
        const float weight = blend.clock.Weight(true, blend.roles[0].observation.time, blend.roles[1].observation.time);
        const auto finish = [&](const char* why) {
            logger::debug(
                "[NativePairAlign] close scene={:X} reason={} age={:.3f} clip={:.3f}/{:.3f} weight={:.3f} activated={} realign={} headingRestores={}",
                blend.activation->nativeScene, why, blend.age, blend.roles[0].observation.time,
                blend.roles[1].observation.time, weight, blend.activation->applied.load(), blend.activation->realign,
                blend.headingRestores);
            Close(blend, true);
            it = blends.erase(it);
        };
        if (blend.activation && blend.activation->applied.load() && blend.activation->realign &&
            blend.age >= .1f * (blend.traceSamples + 1) && blend.traceSamples < 3) {
            ++blend.traceSamples;
            for (unsigned i = 0; i < 2; ++i) {
                auto actor = blend.roles[i].actor.get();
                if (Same(blend.roles[i], actor.get()))
                    logger::debug(
                        "[NativePairAlign] after-activation t={:.3f} role={} positionError={:.3f} yawError={:.5f}",
                        blend.age, i, PairAlignmentMath::Distance(Current(actor.get()), blend.roles[i].goal),
                        PairAlignmentMath::Wrap(actor->GetAngleZ() - blend.roles[i].goal.yaw));
            }
        }
        if (!valid || weight >= 1) {
            finish(valid ? "blend-complete" : reason);
            continue;
        }
        // Only the snapped role (the attacker): the engine turns the pivot (the victim) smoothly itself.
        if (const auto pivot = PivotRole(blend);
            pivot && blend.activation && blend.activation->applied.load() && blend.activation->realign) {
            auto& role = blend.roles[1 - *pivot];
            const auto actor = role.actor.get();
            const float before = actor ? PairAlignmentMath::Wrap(actor->GetAngleZ() - role.goal.yaw) : 0.0f;
            if (HoldHeading(role) && blend.headingRestores++ == 0)
                logger::info(
                    "[NativePairAlign] heading restored scene={:X} role={} actor={:08X} yawError={:.5f} age={:.3f}",
                    blend.activation->nativeScene, 1 - *pivot, actor->GetFormID(), before, blend.age);
        }
        std::array<RE::NiTransform, 2> locals;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& role = blend.roles[i];
            const auto& parent = role.root->parent->world;
            const auto motion = parent * role.root->local;
            locals[i] = PairStartBlend::Local(parent, role.captured, motion, weight);
            if (!Valid(parent) || !Valid(motion) || !Valid(locals[i])) {
                valid = false;
                reason = "invalid-visual-transform";
            }
        }
        if (!valid) {
            finish(reason);
            continue;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto& role = blend.roles[i];
            role.layer.Apply(role.root->local, locals[i]);
            Refresh(role);
        }
        ++it;
    }
}
void Reset(bool restore) {
    std::lock_guard lock(mutex);
    for (auto& blend : blends) Close(*blend, restore);
    blends.clear();
}
void InstallActivationHook() {
    static bool installed = false;
    if (installed || REL::Module::IsVR()) return;
    // SE/AE native SyncActivate tail-calls scene vfunc 08 after initializing
    // startingWorldFromModel/worldFromModelWithRootMotion. See SyncActivate.asm.
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_BGSSynchronizedAnimationInstance[0]};
    ActivationHook::original = table.write_vfunc(0x8, ActivationHook::Thunk);
    installed = true;
    logger::info("[NativePairAlign] native activation barrier installed next={:X}", ActivationHook::original.address());
}
}
