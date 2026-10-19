#include "PCH.h"
#include "runtime/PairStartCamera.h"
#include <mutex>
#include <atomic>

namespace AnimationInjector::PairStartCamera {
namespace {
struct Owner {
    std::uint64_t token = 0;
    RE::ActorHandle actor;
    RE::NiPointer<RE::NiAVObject> scene;
};
std::mutex mutex;
Owner owner;
std::uint64_t serial = 0;
bool installed = false;
std::atomic_uint traces{0};
std::atomic_bool ordinaryTraced{false};
bool Alive(RE::Actor* actor) { return actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState(); }
struct Hook {
    static void Thunk(RE::ThirdPersonState* state, bool sheathed) {
        Owner copy;
        {
            std::lock_guard lock(mutex);
            copy = owner;
        }
        // No pair owns this camera: leave locomotion entirely to the existing
        // engine/TDM chain. Graph flags alone are not a startup reservation.
        if (!copy.token) {
            original(state, sheathed);
            if (state && sheathed && !ordinaryTraced.exchange(true))
                logger::debug(
                    "[PairStartCamera] ordinary sheathed pass-through freeRotation={} animatedCamera={} (no actor/graph query or camera write)",
                    state->freeRotationEnabled, state->toggleAnimCam);
            return;
        }
        auto actor = copy.actor.get();
        auto* camera = RE::PlayerCamera::GetSingleton();
        const bool alive = Alive(actor.get());
        const bool protect = alive && actor->Get3D(false) == copy.scene.get() && actor->IsPlayerRef() && camera &&
                             camera->currentState.get() == state && state &&
                             state->id == RE::CameraStates::kThirdPerson &&
                             camera->cameraTarget == actor->GetHandle() && std::isfinite(state->freeRotation.x) &&
                             std::isfinite(state->freeRotation.y);
        if (protect && !state->toggleAnimCam && traces.load() < 4 && traces.fetch_add(1) < 4) {
            logger::debug("[PairStartCamera] orbit protected lease={} bodyYaw={:.5f} orbit={:.5f} sheathed={}",
                copy.token, actor->GetAngleZ(), state->freeRotation.x, sheathed);
        }
        if (state)
            SetMode(*state, protect, [&] { original(state, sheathed); });
        else
            original(state, sheathed);
    }
    static inline REL::Relocation<decltype(Thunk)> original;
};
}
void Install() {
    if (installed) return;
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_ThirdPersonState[0]};
    Hook::original = table.write_vfunc(REL::Module::IsVR() ? 0xE : 0xD, Hook::Thunk);
    installed = true;
    logger::info("[PairStartCamera] owned-start-only guard installed next={:X}", Hook::original.address());
}
std::uint64_t Acquire(RE::Actor* self, RE::Actor* target) {
    auto* player = self && self->IsPlayerRef() ? self : target && target->IsPlayerRef() ? target : nullptr;
    if (!installed || !Alive(player)) return 0;
    std::lock_guard lock(mutex);
    if (owner.token) return 0;
    owner = {++serial, player->GetHandle(), RE::NiPointer<RE::NiAVObject>{player->Get3D(false)}};
    traces = 0;
    return owner.token;
}
void Release(std::uint64_t token) {
    if (!token) return;
    std::lock_guard lock(mutex);
    if (owner.token == token) {
        logger::debug("[PairStartCamera] lease={} released without changing live orbit", token);
        owner = {};
    }
}
}
