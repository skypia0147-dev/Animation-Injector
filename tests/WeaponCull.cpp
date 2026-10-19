#include <cstdlib>
#include <iostream>
#include <map>
#include "runtime/WeaponCull.h"

// The shared weapon hide ledger with stand-in nodes: a node's culled state is an entry in `culled`.
int main() {
    using namespace AnimationInjector::WeaponCull;
    unsigned checks = 0;
    const auto check = [&](bool ok) {
        ++checks;
        if (!ok) {
            std::cerr << "Weapon cull check failed: " << checks << '\n';
            std::exit(1);
        }
    };
    std::map<int, bool> culled;
    Ledger<int> ledger;
    const auto hide = [&](Owner owner, int node) {
        ledger.Hide(owner, node, culled[node]);
        culled[node] = true;
    };
    const auto show = [&](Owner owner) { ledger.Show(owner, [&](int node, bool state) { culled[node] = state; }); };
    // One owner: back to the state before the hide.
    culled = {{1, false}, {2, true}};
    hide(10, 1);
    hide(10, 2);
    hide(10, 1);  // every frame
    check(culled[1] && culled[2] && ledger.Count(10) == 2);
    show(10);
    check(!culled[1] && culled[2] && ledger.Empty());
    // A pair hides, a carry covers the same node, the pair ends first: the carry keeps it hidden, and
    // the carry's end shows it (the state before the pair), not the pair-hidden state it found.
    culled = {{1, false}};
    hide(20, 1);  // pair role
    hide(30, 1);  // carry cover, taken while the pair already hid the node
    show(20);
    check(culled[1] && ledger.Count(30) == 1);
    hide(30, 1);
    show(30);
    check(!culled[1] && ledger.Empty());
    // Released in the other order: the same result.
    culled = {{1, false}};
    hide(20, 1);
    hide(30, 1);
    show(30);
    check(culled[1]);
    show(20);
    check(!culled[1] && ledger.Empty());
    // A node hidden by another mod before any owner stays hidden afterwards.
    culled = {{1, true}};
    hide(40, 1);
    show(40);
    check(culled[1]);
    // A held hide keeps its token: the next pair adopts it and its release is the only one.
    culled = {{1, false}, {2, false}};
    hide(50, 1);
    hide(50, 2);  // weapon equipped later: a new node under the same token
    check(ledger.Count(50) == 2);
    show(50);
    check(!culled[1] && !culled[2]);
    // Unknown or empty owners are inert; Clear forgets without writing (old 3D after a load).
    show(99);
    show(0);
    ledger.Hide(0, 1, false);
    check(ledger.Empty() && !culled[1]);
    hide(60, 1);
    ledger.Clear();
    check(ledger.Empty() && culled[1]);
    std::cout << "Weapon cull checks passed: " << checks << '\n';
    return 0;
}
