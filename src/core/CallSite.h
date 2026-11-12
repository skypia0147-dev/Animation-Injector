#pragma once
#include <cstdint>

// Verification of a rel32 call site before a hook is written over it. For sites other plugins also hook
// (so the call target cannot be compared against the engine's own function).
namespace AnimationInjector::CallSite {

struct Site {
    bool ok = false;            // safe to write a 5-byte call here
    std::uintptr_t target = 0;  // where the call goes now
    bool foreign = false;       // target is outside the game's code: another plugin already hooks this site
};

// a_function: first byte of the engine function that contains the site. Logs the reason when it refuses.
Site Verify(std::uintptr_t a_function, std::uintptr_t a_site, const char* a_label);

}  // namespace AnimationInjector::CallSite
