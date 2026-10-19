#pragma once
namespace AnimationInjector {
// Both event-only and buffered windows reject input after their close event.
constexpr bool AcceptNativeCancelInput(int mode, bool open, bool closed) noexcept {
    return mode == 0 || (!closed && (mode == 2 || open));
}
constexpr bool ReadyNativeCancel(int mode, bool open, bool closed) noexcept { return mode == 0 || (open && !closed); }
}
