#pragma once
#include <cstdint>

namespace AnimationInjector::CarryOwnership {
// Restore only bits acquired by this operation, preserving pre-existing state
// and unrelated changes made by the engine or other mods while carried.
struct Flags {
    std::uint32_t acquired = 0;
    std::uint32_t Acquire(std::uint32_t current, std::uint32_t wanted) {
        acquired = wanted & ~current;
        return current | acquired;
    }
    std::uint32_t Restore(std::uint32_t current) {
        const auto result = current & ~acquired;
        acquired = 0;
        return result;
    }
};
// Native filter writes can fail while a cell detaches. Keep the owned bits
// until a confirmed write, including when the engine copies them to new 3D.
struct ContactFlags {
    std::uint32_t acquired = 0;
    std::uint32_t Acquire(std::uint32_t current, std::uint32_t wanted) {
        acquired |= wanted & ~current;
        return current | wanted;
    }
    template<class Write>
    bool Restore(std::uint32_t current, Write&& write) {
        const auto restored = current & ~acquired;
        if (restored != current && !write(restored)) return false;
        acquired = 0;
        return true;
    }
};
struct Scene {
    std::uint32_t cell = 0, world = 0;
    bool interior = false;
};
constexpr bool SameScene(Scene a, Scene b) {
    if (!a.cell || !b.cell) return false;
    if (a.cell == b.cell) return true;
    // Adjacent loaded exterior cells belong to the same continuous space.
    return !a.interior && !b.interior && a.world && a.world == b.world;
}
enum class PairResult { kWait, kSuccess, kFailure };
constexpr PairResult Completed(bool sameTicket, bool idle, bool normalEnd, bool enteredA, bool enteredB, bool exitedA,
    bool exitedB, bool restoredA, bool restoredB) {
    if (!sameTicket) return PairResult::kFailure;
    if (!idle) return PairResult::kWait;
    return normalEnd && enteredA && enteredB && exitedA && exitedB && restoredA && restoredB ? PairResult::kSuccess
                                                                                             : PairResult::kFailure;
}
}
