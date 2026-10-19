#include "PCH.h"
#include "core/CarryPhysics.h"

namespace AnimationInjector::CarryPhysics {
bool Available(RE::bhkCharacterController* controller) {
    if (!controller || !controller->GetHavokWorld()) return false;
    static const REL::Relocation<std::uintptr_t> proxy{RE::VTABLE_bhkCharProxyController[1]};
    static const REL::Relocation<std::uintptr_t> rigid{RE::VTABLE_bhkCharRigidBodyController[0]};
    const auto table = *reinterpret_cast<const std::uintptr_t*>(controller);
    return table == proxy.address() || table == rigid.address();
}
bool WriteFilter(RE::bhkCharacterController* controller, std::uint32_t filter) {
    if (!Available(controller)) return false;
    // CommonLib names slot 09 Unk_09(void). The inspected proxy and rigid-body
    // implementations take the packed CFilter in EDX, not a CFilter pointer.
    // The proxy setter refreshes world contacts; the rigid-body setter only writes
    // the filter. Call RefreshContacts before resuming the human carry probe.
    // Never edit collidable/phantom layouts. Vtables use Address Library.
    REL::RelocateVirtual<void(RE::bhkCharacterController*, std::uint32_t)>(0x09, 0x09, controller, filter);
    RE::CFilter observed{};
    controller->GetCollisionFilterInfo(observed);
    return observed.filter == filter;
}
bool ContactRefreshAvailable(RE::bhkCharacterController* controller) {
    if (!Available(controller)) return false;
    static const REL::Relocation<std::uintptr_t> proxy{RE::VTABLE_bhkCharProxyController[1]};
    return *reinterpret_cast<const std::uintptr_t*>(controller) == proxy.address() || REL::Module::IsAE();
}
bool RefreshContacts(RE::bhkCharacterController* controller) {
    if (!ContactRefreshAvailable(controller)) return false;
    static const REL::Relocation<std::uintptr_t> proxy{RE::VTABLE_bhkCharProxyController[1]};
    if (*reinterpret_cast<const std::uintptr_t*>(controller) == proxy.address()) return true;
    RE::NiPointer<RE::bhkCharacterController> keepController(controller);
    RE::NiPointer<RE::bhkWorld> world(controller->GetHavokWorld());
    if (!world) return false;
    RE::BSWriteLockGuard lock(world->worldLock);
    auto* native = world->GetWorld1();
    auto* body = controller->GetRigidBody();
    if (!native || !body || body->world != native || native->criticalOperationsLockCount ||
        native->criticalOperationsLockCountForPhantoms || native->blockExecutingPendingOperations)
        return false;
    RE::hkRefPtr<RE::hkpRigidBody> keepBody(body);
    // Verified AE world update called by the engine's bhk filter refresh:
    // full broadphase check (0), including shape-collection refresh (1).
    // SE's encrypted local executable could not establish its matching function;
    // do not guess an ID. This restriction affects only the opt-in probe.
    using Update = void(RE::hkpWorld*, RE::hkpEntity*, std::int32_t, std::int32_t);
    static REL::Relocation<Update> update{REL::ID(61321)};
    const auto before = body->collidable.collisionEntries.size();
    update(native, body, 0, 1);
    if (body->world != native || controller->GetRigidBody() != body) return false;
    logger::info("[CarryContactRefresh] rigid=true entries={}->{} filter={:08X} completed=true", before,
        body->collidable.collisionEntries.size(), body->collidable.broadPhaseHandle.collisionFilterInfo.filter);
    return true;
}
bool Follow(RE::bhkCharacterController* controller, const RE::NiPoint3& position) {
    if (!Available(controller) || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z))
        return false;
    RE::CFilter filter{};
    controller->GetCollisionFilterInfo(filter);
    if (!filter.QNoCollision()) return false;
    const float scale = RE::bhkWorld::GetWorldScale();
    if (!std::isfinite(scale) || scale <= 0) return false;
    controller->SetLinearVelocityImpl({0, 0, 0, 0});
    controller->SetPositionImpl({position.x * scale, position.y * scale, position.z * scale, 0}, true, true);
    RE::hkVector4 observed{};
    controller->GetPositionImpl(observed, true);
    const RE::NiPoint3 actual{
        observed.quad.m128_f32[0] / scale, observed.quad.m128_f32[1] / scale, observed.quad.m128_f32[2] / scale};
    return std::isfinite(actual.x) && std::isfinite(actual.y) && std::isfinite(actual.z) &&
           (actual - position).SqrLength() <= 64;
}
}
