#include "runtime/ExternalHolds.h"
#include <cstdlib>
#include <iostream>
#include <set>

using AnimationInjector::ExternalHolds;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    // The other plugin's opt-out list (Precision's disabledActors): emplace/erase report a change.
    std::set<int> disabled;
    unsigned calls = 0;
    auto toggle = [&](int actor, bool disable) {
        ++calls;
        return disable ? disabled.insert(actor).second : disabled.erase(actor) > 0;
    };
    ExternalHolds<int> holds;
    // Lift start disables; carry end gives back.
    check(holds.Hold(1, toggle) && disabled.contains(1) && holds.Holds(1));
    check(holds.Release(1, toggle) && !disabled.contains(1) && !holds.Holds(1));
    // A second Hold of the same actor does not toggle again; one Release gives it back.
    calls = 0;
    check(holds.Hold(2, toggle) && holds.Hold(2, toggle) && calls == 1 && holds.Count() == 1);
    check(holds.Release(2, toggle) && !disabled.contains(2));
    check(!holds.Release(2, toggle) && calls == 2);
    // An actor someone else disabled stays disabled after our carry ends.
    disabled.insert(3);
    check(!holds.Hold(3, toggle) && !holds.Holds(3));
    check(!holds.Release(3, toggle) && disabled.contains(3));
    // Release of an actor never held changes nothing.
    disabled.insert(4);
    check(!holds.Release(4, toggle) && disabled.contains(4));
    // Load: everything we hold goes back, nothing else.
    check(holds.Hold(5, toggle) && holds.Hold(6, toggle));
    holds.ReleaseAll(toggle);
    check(holds.Count() == 0 && !disabled.contains(5) && !disabled.contains(6));
    check(disabled.contains(3) && disabled.contains(4));
    std::cout << checks << " external hold checks passed\n";
}
