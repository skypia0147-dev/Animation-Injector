#include "runtime/GetUpHandoffPlan.h"
#include <cstdlib>
#include <iostream>

using namespace AnimationInjector::GetUpHandoffPlan;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    Sample resting;
    resting.present = true;
    resting.ragdoll = true;
    resting.knock = 5;
    resting.controller = true;
    resting.suspended = true;
    Sample handed = resting;
    handed.ragdoll = false;
    handed.knock = kKnockGetUp;
    handed.suspended = false;
    handed.grounded = true;
    Sample standing = handed;
    standing.knock = kKnockNormal;
    Sample lifted = standing;
    lifted.grounded = false;
    {
        // Kicks the get-up at rest, supplies the two notifications one per frame at the handoff, and starts the
        // action as soon as the knock state is normal.
        State st;
        check(Advance(st, resting, 0, true, .25f) == Act::kAlign && st.phase == Phase::kHandoff);
        check(Advance(st, resting, 0, true, kAlignSeconds) == Act::kKick && st.phase == Phase::kHandoff);
        check(Advance(st, resting, 0, true, .2f) == Act::kNone && st.phase == Phase::kHandoff);
        check(Advance(st, handed, 0, true, .016f) == Act::kNone && st.phase == Phase::kGetup);
        check(Advance(st, handed, 0, true, .016f) == Act::kDeliverGetup);
        check(Advance(st, handed, 0, true, .016f) == Act::kDeliverGetUpEnd && st.phase == Phase::kStand);
        check(Advance(st, handed, 0, true, .016f) == Act::kNone);
        check(Advance(st, standing, 0, true, .016f) == Act::kStart && st.phase == Phase::kDone);
        check(Advance(st, standing, 0, true, .016f) == Act::kNone);
    }
    {
        // Natural notifications are never supplied again; the engine done first starts at once.
        State st;
        Advance(st, handed, 0, true, .016f);
        const auto both = static_cast<std::uint8_t>(kSawGetup | kSawGetUpEnd);
        check(Advance(st, handed, both, true, .016f) == Act::kNone &&
              Advance(st, handed, both, true, .016f) == Act::kNone);
        check(Advance(st, standing, 0, true, .016f) == Act::kStart);
        State done;
        Advance(done, standing, 0, true, .016f);
        check(Advance(done, standing, 0, true, .016f) == Act::kStart);
    }
    {
        // An action that may not start in the air waits for the ground (at most kGroundWaitSeconds); one that may
        // starts at once.
        State ground;
        Advance(ground, standing, 0, true, .016f);
        check(Advance(ground, lifted, 0, true, .016f) == Act::kNone &&
              Advance(ground, standing, 0, true, .016f) == Act::kStart);
        State falling;
        Advance(falling, standing, 0, true, .016f);
        check(Advance(falling, lifted, 0, true, kGroundWaitSeconds / 2) == Act::kNone);
        check(Advance(falling, lifted, 0, true, kGroundWaitSeconds / 2 + .01f) == Act::kNone);
        check(Advance(falling, lifted, 0, true, .016f) == Act::kStart);
        State air;
        Advance(air, standing, 0, false, .016f);
        check(Advance(air, lifted, 0, false, .016f) == Act::kStart);
    }
    {
        // A new hit, a lost controller, held down, gone, too long: the engine's own get-up keeps the body.
        State hit;
        Advance(hit, handed, 0, true, .016f);
        check(Advance(hit, handed, 0, true, .016f) == Act::kDeliverGetup);
        check(Advance(hit, resting, 0, true, .016f) == Act::kGiveUp && hit.phase == Phase::kDone);
        State lost;
        Advance(lost, handed, 0, true, .016f);
        Sample noController = handed;
        noController.controller = false;
        check(Advance(lost, noController, 0, true, .016f) == Act::kGiveUp);
        State held;
        Sample down = resting;
        down.held = true;
        check(Advance(held, down, 0, true, .016f) == Act::kGiveUp);
        State gone;
        check(Advance(gone, Sample{}, 0, true, .016f) == Act::kGiveUp);
        State slow;
        Sample tumbling = resting;  // a long fall: not at rest yet
        tumbling.knock = 1;
        check(Advance(slow, tumbling, 0, true, kGiveUpSeconds - .1f) == Act::kNone);
        check(Advance(slow, tumbling, 0, true, .2f) == Act::kGiveUp);
    }
    {
        // The get-up state is left with GetUpExit together with the start, unless the engine's clip already did.
        check(LeaveGetUp(0) && LeaveGetUp(kSawGetup | kSawGetUpEnd) && !LeaveGetUp(kSawGetUpExit));
    }
    {
        // At rest the get-up is kicked at once (no engine pull); handed back, the usual steps follow.
        State st;
        Sample flying = resting;
        flying.knock = 1;
        check(Advance(st, flying, 0, true, .1f) == Act::kNone);
        // GetUpBegin first (the engine's alignment of the actor to the body), GetUpStart kAlignSeconds later.
        check(Advance(st, resting, 0, true, .016f) == Act::kAlign);
        check(Advance(st, resting, 0, true, kAlignSeconds / 2) == Act::kNone);
        check(Advance(st, resting, 0, true, kAlignSeconds / 2) == Act::kKick);
        check(Advance(st, resting, 0, true, .016f) == Act::kNone);
        check(Advance(st, handed, 0, true, .016f) == Act::kNone && st.phase == Phase::kGetup);
        check(Advance(st, handed, 0, true, .016f) == Act::kDeliverGetup);
        // Not handed back within kKickSeconds: FullyRagdoll again, then GetUpBegin after the settle; nothing more.
        State stuck;
        Advance(stuck, resting, 0, true, .016f);
        check(Advance(stuck, resting, 0, true, kAlignSeconds) == Act::kKick);
        check(Advance(stuck, resting, 0, true, kKickSeconds - .1f) == Act::kNone);
        check(Advance(stuck, resting, 0, true, .2f) == Act::kRescueRagdoll && stuck.phase == Phase::kRescue);
        check(Advance(stuck, resting, 0, true, kRescueSettleSeconds / 2) == Act::kNone);
        check(Advance(stuck, resting, 0, true, kRescueSettleSeconds) == Act::kRescueGetUp);
        check(stuck.phase == Phase::kDone && Advance(stuck, resting, 0, true, .016f) == Act::kNone);
    }
    {
        // Down and late.
        check(Down(5, true) && Down(kKnockGetUp, false) && !Down(kKnockNormal, false));
        check(!TooLate(kKnockGetUp, false, kLateGetUpSeconds - .01f) && TooLate(kKnockGetUp, false, kLateGetUpSeconds));
        check(!TooLate(5, true, 5) && !TooLate(kKnockNormal, false, 5));
    }
    std::cout << checks << " get-up handoff checks passed\n";
}
