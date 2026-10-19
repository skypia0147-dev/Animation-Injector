#pragma once
#include "PCH.h"
namespace AnimationInjector::ActionLinks {
inline int calls = 0;
inline bool Input(std::uint32_t, bool, bool) {
    ++calls;
    return false;
}
}
