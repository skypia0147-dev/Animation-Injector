#pragma once
#include "PCH.h"

namespace AnimationInjector {
// Owns only a newly constructed, detached query object. Never copies actor listeners.
class QueryPhantom {
public:
    QueryPhantom(const RE::hkpShape* shape, const RE::hkTransform& transform, std::uint32_t filter);
    ~QueryPhantom();
    QueryPhantom(const QueryPhantom&) = delete;
    QueryPhantom& operator=(const QueryPhantom&) = delete;
    RE::hkpCachingShapePhantom* Get() const { return object; }

private:
    RE::hkpCachingShapePhantom* object = nullptr;
};
}
