#pragma once
#include <cmath>
namespace AnimationInjector {
inline float ExitWatchdog(float blend) { return (std::isfinite(blend) && blend >= 0 ? blend : 5.0f) + 1.0f; }
// Releasing a rotation lock hands bAllowRotation back to the destination state. After an
// action the actor returns to locomotion, which needs it false (see below), so the value seen
// when the action started is never written back: a combat state can leave it true there, and
// restoring that true after the exit left NPCs turning in place without moving (0.12.154).
// Only a true this runner wrote itself, and that no other owner has changed, is cleared.
template<class Read, class Write>
void HandBackRotation(bool& owned, bool lastWritten, Read read, Write write) {
    if (!owned) return;
    owned = false;
    bool current = false;
    if (lastWritten && read(current) && current) write(false);
}
// bAllowRotation is an animation movement mode, not a camera-unlock switch.
// The engine selects StartMotionDriven only when BOTH it and bAnimationDriven
// are false. A true rotation flag selects StartAllowRotation instead.
// Recovery hands control to locomotion permanently; never restore the outgoing
// animation's mode afterward. Stop writing as soon as another owner takes over.
template<class Read, class Write>
void MaintainRecoveredRotation(bool recovering, Read read, Write write) {
    if (!recovering) return;
    bool animationRotation = false;
    if (read(animationRotation) && animationRotation) write(false);
}
// Completion can happen inside send(). Never resurrect a completed runner.
template<class Prepare, class Send, class Pending, class Accepted, class Rejected>
bool RequestPreparedExit(Prepare prepare, Send send, Pending pending, Accepted accept, Rejected reject) {
    prepare();
    const bool ok = send();
    if (pending()) {
        if (ok)
            accept();
        else
            reject();
    }
    return ok;
}
}
