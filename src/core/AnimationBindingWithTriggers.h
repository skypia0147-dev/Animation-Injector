#pragma once

#include "RE/H/hkArray.h"
#include "RE/H/hkRefPtr.h"
#include "RE/H/hkReferencedObject.h"
#include "RE/H/hkaAnimationBinding.h"
#include "RE/H/hkbEventBase.h"

namespace RE {
// Complete CommonLib's forward declaration for read-only binding inspection.
// Layout checked against local OAR Havok.h and native GetMark's binding access.
// Never instantiate, copy, mutate or destroy this engine-owned wrapper here.
class hkbAnimationBindingWithTriggers : public hkReferencedObject {
public:
    struct Trigger {
        float time;
        std::uint32_t padding;
        std::int32_t eventId;
        hkRefPtr<hkbEventPayload> payload;
    };
    hkRefPtr<hkaAnimationBinding> binding;
    hkArray<Trigger> triggers;
};
static_assert(sizeof(hkbAnimationBindingWithTriggers::Trigger) == 0x18);
static_assert(sizeof(hkbAnimationBindingWithTriggers) == 0x28);
}
