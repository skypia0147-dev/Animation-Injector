#pragma once

namespace AnimationInjector {
// An exit event starts a blend; ownership ends only when that blend's clip is deactivated.
// The graph can write the flag again between these two points.
template<class Write>
bool WriteOwnedDrivenFlag(bool& owned, bool enable, bool finalRelease, Write&& write) {
    if (!enable && !owned) return true;
    const bool accepted = write(enable);
    if (enable)
        owned = accepted;
    else if (finalRelease)
        owned = false;
    return accepted;
}

// Keep native locomotion enabled through a recovery pose blend. Relinquish
// ownership permanently when another action or physics takes over.
template<class Write>
void MaintainRecoveredDrivenFlag(bool& recovering, bool& owned, bool locomotionOwns, Write&& write) {
    if (!recovering) return;
    if (!locomotionOwns) {
        recovering = false;
        owned = false;
        return;
    }
    WriteOwnedDrivenFlag(owned, false, false, write);
}
}
