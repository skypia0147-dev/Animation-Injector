#include "core/NodeIdLayout.h"

// The node id layout of a renumbered graph (2026-11-12: state machines may pass the positive ids; the clone range
// may not).
int main() {
    using namespace AnimationInjector::NodeIdRenumber;
    // Few machines: as before, machines 0..n-1, then the clone range, then the shared ids.
    constexpr auto few = LayoutFor(1200);
    static_assert(few.cloneBase == 1200 && few.sharedFirst == 1200 + kCloneIds);
    static_assert(few.MachineId(0) == 0 && few.MachineId(1199) == 1199);
    // The last count that keeps every machine below the clone range.
    constexpr auto edge = LayoutFor(kPositiveIds - kCloneIds);
    static_assert(
        edge.cloneBase == kPositiveIds - kCloneIds && edge.MachineId(edge.cloneBase - 1) == edge.cloneBase - 1);
    static_assert(edge.cloneBase + kCloneIds == kPositiveIds);  // clone ids end at the last positive id
    // More machines than fit below it: the clone range stays positive, the rest of the machines go above it.
    constexpr std::size_t many = 40000;
    constexpr auto split = LayoutFor(many);
    static_assert(split.cloneBase == kPositiveIds - kCloneIds);
    static_assert(split.cloneBase + kCloneIds - 1 < kPositiveIds);
    static_assert(split.MachineId(split.cloneBase - 1) == split.cloneBase - 1);
    static_assert(split.MachineId(split.cloneBase) == split.cloneBase + kCloneIds);  // first one above the clones
    static_assert(split.MachineId(many - 1) == many + kCloneIds - 1 && split.MachineId(many - 1) < split.sharedFirst);
    static_assert(split.sharedFirst + kSharedIdsMin <= kIdLimit);
    // The limit: what the old one stopped at now fits, and the shared ids are never squeezed out.
    static_assert(MachinesFit(28671) && MachinesFit(60000) && !MachinesFit(kMachineIdLimit - kCloneIds));
    static_assert(LayoutFor(kMachineIdLimit - kCloneIds - 1).sharedFirst + kSharedIdsMin <= kIdLimit);
    // No machine id is the "no id" value.
    static_assert(LayoutFor(kMachineIdLimit - kCloneIds - 1).MachineId(kMachineIdLimit - kCloneIds - 2) < kIdLimit);
    return 0;
}
