#pragma once
#include "PCH.h"
namespace AnimationInjector::PairedSession {
struct Ticket {
    std::uint64_t generation = 0, request = 0;
    bool operator==(const Ticket&) const = default;
};
}
