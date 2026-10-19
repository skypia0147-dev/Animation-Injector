#pragma once
#include <atomic>
#include <cstdint>

namespace AnimationInjector {
// One request per playback step. Graph callbacks only enqueue; the actor update
// consumes it outside the graph event dispatch stack.
class FireArrowStep {
    std::atomic_uint8_t state{0};  // 0 waiting, 1 queued, 2 consumed
public:
    std::uint64_t draw = 0;
    void Reset(std::uint64_t ticket) {
        draw = ticket;
        state.store(0);
    }
    void Cancel() { state.store(2); }
    void Request() {
        std::uint8_t expected = 0;
        state.compare_exchange_strong(expected, 1);
    }
    bool Take() {
        std::uint8_t expected = 1;
        return state.compare_exchange_strong(expected, 2);
    }
};

// Tracks native press/release cycles, without owning any ButtonEvent or held bit.
struct FireArrowCycle {
    enum class Shot { kNone, kAutomatic, kManual };
    std::uint64_t serial = 1;
    bool released = true, automatic = false, pressed = false, fired = false, manualPending = false;
    void Press() {
        ++serial;
        released = false;
        automatic = false;
        pressed = true;
        fired = false;
        manualPending = false;
    }
    void Reset() {
        ++serial;
        released = true;
        automatic = false;
        pressed = false;
        fired = false;
        manualPending = false;
    }
    bool CanRelease(std::uint64_t ticket) const { return ticket == serial && pressed && !released && !fired; }
    bool Automatic(std::uint64_t ticket) {
        if (!CanRelease(ticket)) return false;
        released = true;
        automatic = true;
        fired = true;
        return true;
    }
    Shot Claim(std::uint64_t ticket) {
        if (ticket != serial || !pressed || fired || (released && !manualPending)) return Shot::kNone;
        if (!released) {
            Automatic(ticket);
            return Shot::kAutomatic;
        }
        fired = true;
        manualPending = false;
        return Shot::kManual;
    }
    void ObserveArrow() {
        if (pressed) {
            fired = true;
            released = true;
            manualPending = false;
        }
    }
    bool NativeRelease() {
        if (!automatic) manualPending = pressed && !fired;
        released = true;
        // Repeated release dispatches in this physical press cycle must not fire
        // again. The next real press or save/load reset retires this guard.
        return !automatic;
    }
};
}
