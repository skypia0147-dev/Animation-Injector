#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace AnimationInjector::NativePairRoles {
struct Role {
    std::uint32_t actor = 0;
    bool valid = false, lead = false;
};
// Match native Start's leader preference and first occurrence per actor.
// First/third-person entries may duplicate the same physical actor.
inline std::optional<std::array<std::size_t, 2>> Select(std::span<const Role> roles) {
    if (roles.size() < 2 || roles.size() > 8) return {};
    std::size_t lead = roles.size(), support = roles.size();
    for (std::size_t i = 0; i < roles.size(); ++i) {
        if (!roles[i].actor || !roles[i].valid) return {};
        if (lead == roles.size() || (!roles[lead].lead && roles[i].lead)) lead = i;
    }
    for (std::size_t i = 0; i < roles.size(); ++i)
        if (roles[i].actor != roles[lead].actor) {
            if (support == roles.size())
                support = i;
            else if (roles[i].actor != roles[support].actor)
                return {};
        }
    if (support == roles.size()) return {};
    return std::array<std::size_t, 2>{support, lead};
}
}
