#pragma once

namespace AnimationInjector {
// A source can deactivate before or after the destination activates. Retire it
// only after both observations; until then its runner cannot be reused.
struct DirectLinkRetirement {
    bool committed = false, deactivated = false;
    bool Commit() {
        committed = true;
        return deactivated;
    }
    bool Deactivate() {
        deactivated = true;
        return committed;
    }
};
}
