#include "core/NativePairEndPlan.h"
#include <cstdlib>
#include <iostream>

using namespace AnimationInjector::NativePairEndPlan;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    // The end begins in a single-play clip's last 0.1 s, at deactivation, or when the scene stops being synced.
    check(!EndBegins(1.0f, 2.0f, false, false, true));
    check(!EndBegins(1.89f, 2.0f, false, false, true));
    check(EndBegins(1.9f, 2.0f, false, false, true));
    check(EndBegins(0.2f, 2.0f, false, true, true));
    check(EndBegins(0.2f, 2.0f, false, false, false));
    // A looping clip wraps its time: only deactivation or the scene's end count.
    check(!EndBegins(1.95f, 2.0f, true, false, true));
    check(EndBegins(1.0f, 2.0f, true, true, true));
    check(EndBegins(1.0f, 2.0f, true, false, false));
    // An unknown duration (no animation bound yet) never ends by time.
    check(!EndBegins(5.0f, -1.0f, false, false, true));
    check(!EndBegins(5.0f, 0.0f, false, false, true));

    // Mounting and dismounting are the engine's to place.
    check(Excluded("Animations\\HorseEnterLeft.hkx"));
    check(Excluded("Animations\\Horse\\HorseExit.hkx"));
    check(Excluded("Animations\\paired_HORSEkillmove.hkx"));
    check(!Excluded("Animations\\paired_1hmkillmovebackstab.hkx"));
    check(!Excluded(""));

    // Slots: find or claim by identity, a full table claims nothing, identity 0 is never stored.
    Slots slots;
    check(slots.Claim(0) == -1 && slots.Find(0) == -1);
    const int first = slots.Claim(7);
    check(first >= 0 && slots.Claim(7) == first && slots.Find(7) == first);
    for (std::uint32_t id = 100; id < 100 + kCapacity - 1; ++id) check(slots.Claim(id) >= 0);
    check(slots.Claim(999) == -1);  // the 17th participant keeps the engine's own end
    slots.Release(first);
    check(slots.Find(7) == -1 && slots.Claim(999) == first);
    slots.Release(-1);
    slots.Release(static_cast<int>(kCapacity));
    check(slots.Find(999) == first);

    std::cout << "NativePairEndPlan: " << checks << " checks passed\n";
}
