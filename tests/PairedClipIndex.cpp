#include "core/PairedClipIndex.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector::PairedClipIndex;
static void Check(bool condition) {
    if (!condition) std::abort();
}
int main() {
    // The same paired file occupies different slots in two character projects.
    std::uint16_t shared = 1787;
    const auto male = Resolve(1791, 3009, true, true, false, false);
    const auto female = Resolve(1787, 3009, true, true, false, false);
    Check(male && female);
    for (int attempt = 0; attempt < 10; ++attempt) {
        {
            Borrow first;
            first.Set(shared, *male);
            Check(shared == 1791);
            {
                Borrow second;
                second.Set(shared, *female);
                Check(shared == 1787);
            }
            Check(shared == 1791);
        }
        Check(shared == 1787);
    }
    try {
        Borrow query;
        query.Set(shared, *male);
        throw std::runtime_error("native failure");
    } catch (const std::runtime_error&) {
    }
    Check(shared == 1787);
    Check(!Resolve(1791, 3009, true, true, true, false));    // Active playback.
    Check(!Resolve(1791, 3009, true, true, false, true));    // OAR owns selection.
    Check(!Resolve(1791, 3009, true, false, false, false));  // Vanilla clip.
    Check(!Resolve(-1, 3009, true, true, false, false));
    Check(!Resolve(3009, 3009, true, true, false, false));
    Check(!Resolve(1791, 3009, false, true, false, false));
    Check(!Resolve(65535, 65536, true, true, false, false));
    Check(Resolve(32768, 40000, true, true, false, false).value() == 32768);
    std::cout << "Paired project indexes, nested/failed query restoration, active/OAR/vanilla guards passed\n";
}
