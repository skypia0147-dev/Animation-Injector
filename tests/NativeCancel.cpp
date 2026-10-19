#include "runtime/NativeCancelPolicy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace AnimationInjector;
int main() {
    // A press before the window is buffered, but cannot exit the full-body state yet.
    assert(AcceptNativeCancelInput(2, false, false));
    assert(!ReadyNativeCancel(2, false, false));
    assert(ReadyNativeCancel(2, true, false));
    // Closing the window invalidates both new presses and an existing buffer.
    for (int mode : {1, 2}) {
        assert(!AcceptNativeCancelInput(mode, true, true));
        assert(!ReadyNativeCancel(mode, true, true));
        assert(!AcceptNativeCancelInput(mode, false, true));
    }
    assert(!AcceptNativeCancelInput(1, false, false));
    assert(AcceptNativeCancelInput(1, true, false));
    assert(ReadyNativeCancel(1, true, false));
    for (bool open : {false, true})
        for (bool closed : {false, true}) {
            assert(AcceptNativeCancelInput(0, open, closed));
            assert(ReadyNativeCancel(0, open, closed));
        }
    std::cout << "Native cancel early-input, open, close and always-window checks passed\n";
}
