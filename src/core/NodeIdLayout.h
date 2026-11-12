#pragma once

#include <cstddef>

// How core/NodeIdRenumber hands out a graph's 16-bit node ids; no engine types, so the arithmetic is tested alone
// (tests/NodeIdLayout.cpp).
namespace AnimationInjector::NodeIdRenumber {

// Ids 0..0xFFFE; 0xFFFF means "no id".
inline constexpr std::size_t kIdLimit = 0xFFFF;
// Ids the engine's own numbering leaves for runtime clones before a graph counts as over the limit.
inline constexpr std::size_t kFreeForClones = 512;
// Ids kept for runtime clones, right after the state machines' ids.
inline constexpr std::size_t kCloneIds = 4096;
// Runtime clone ids must stay positive 16-bit numbers (the engine returns only ids >= 0 to its pool).
inline constexpr std::size_t kPositiveIds = 0x7FFF;
// Ids always left to the nodes that are not state machines (they share them).
inline constexpr std::size_t kSharedIdsMin = 1024;
// State machines and the clone range together stay below this. A state machine's id may be any 16-bit number (the
// engine's own numbering gives the upper half to machines of any graph past 32768 nodes), so only the clone range
// has to sit in the positive half: until 2026-11-12 the machines stayed there too, which stopped at 28,671 of them.
inline constexpr std::size_t kMachineIdLimit = kIdLimit - kSharedIdsMin;
// Where the ids go for a graph of a_machines state machines: machines 0, 1, 2, ... up to the clone range, the clone
// range (kCloneIds ids, positive), the machines that did not fit below it, then the ids the other nodes share.
struct Layout {
    std::size_t cloneBase = 0;    // first clone id: numStaticNodes and nextUniqueID
    std::size_t sharedFirst = 0;  // first id of the other nodes
    constexpr std::size_t MachineId(std::size_t a_index) const {
        return a_index < cloneBase ? a_index : a_index + kCloneIds;
    }
};
constexpr bool MachinesFit(std::size_t a_machines) { return a_machines + kCloneIds < kMachineIdLimit; }
constexpr Layout LayoutFor(std::size_t a_machines) {
    const std::size_t lowest = kPositiveIds - kCloneIds;  // the clone range ends at the last positive id
    return {a_machines < lowest ? a_machines : lowest, a_machines + kCloneIds};
}

}  // namespace AnimationInjector::NodeIdRenumber
