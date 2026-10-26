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
    // the filter.
    // Never edit collidable/phantom layouts. Vtables use Address Library.
    REL::RelocateVirtual<void(RE::bhkCharacterController*, std::uint32_t)>(0x09, 0x09, controller, filter);
    RE::CFilter observed{};
    controller->GetCollisionFilterInfo(observed);
    return observed.filter == filter;
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
