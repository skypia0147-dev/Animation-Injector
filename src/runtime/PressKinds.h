#pragma once
#include <cstdint>

namespace AnimationInjector::PressKinds {
// How a start's key, pad button or game control is pressed (10-30): <Start press="down|tap|double|hold">.
//   down   - the moment it goes down, no wait (what every start did before; the default).
//   tap    - one short press: down and up within the hold time.
//   double - two quick presses: the second goes down within the double window after the first came up. It is
//            decided the moment the second goes down, so a held second press is still a double.
//   hold   - held past the hold time; decided then, without waiting for the release.
// An input waits only for the kinds that could start something when it went down (their conditions read then; the
// movement direction is the one of that moment). A game control's press that no kind takes goes to the game, late
// by the wait: a short jump press can dodge while a long one still jumps.
enum class Press : std::uint8_t { kDown, kTap, kDouble, kHold };

// The hold time and the double window, in game seconds (frame deltas: a pause or a menu stops them). From the INI.
struct Timing {
    float hold = 0.2f;
    float doubleWindow = 0.25f;
};

// The kinds that could start something on this press, as read when it went down (kDown is tried before: an
// action it starts takes the press whole).
struct Kinds {
    bool tap = false, dbl = false, hold = false;
    bool Any() const { return tap || dbl || hold; }
};

// What the input does next.
enum class Outcome : std::uint8_t {
    kNone,
    kTap,  // start the actions of that kind now
    kDouble,
    kHold,
    kGameDown,  // a game control: hand the game the press now; the key is still down and its release goes through
    kGameTap,   // a game control: hand the game a whole press now (down and up); the key is already up
};

struct Step {
    Outcome outcome = Outcome::kNone;
    bool keep = false;  // keep this button event from the game
};

// One input's presses: one key, one pad button or one game control. Pure (no game types), so tests drive it.
// Call Tick before Down and Up in the same frame, so an expired wait is settled first.
class Classifier {
public:
    // The input went down. `kinds`: what could start now; `game`: a game control (what no kind takes is the game's).
    Step Down(float now, Kinds kinds, bool game, const Timing& timing);
    // The input came up.
    Step Up(float now, const Timing& timing);
    // Time passed: a hold reached, or a double window expired.
    Outcome Tick(float now, const Timing& timing);
    // The actions of the outcome just given started nothing: the game takes the press after all (a game control),
    // so the release of a key still down goes through.
    void ToGame();
    // Loading, a menu or the console: whatever waits is dropped (a press kept from the game keeps its release).
    void Reset();
    bool Waiting() const { return _state == State::kDown1 || _state == State::kWait; }
    // A game control's press that is still down and ours (waiting, or taken by one of our kinds): its held repeats
    // are kept from the game too, so a held sprint does not start meanwhile.
    bool Keeps() const { return _game && (_state == State::kDown1 || _state == State::kConsumed); }

private:
    enum class State : std::uint8_t {
        kIdle,
        kDown1,     // the first press is down, its kind not known yet
        kWait,      // the first press came up; a second may still make a double
        kConsumed,  // decided while down: the release is kept as the press was (game controls)
        kPassed,    // decided while down and the game took it: the release goes to the game
    };
    State _state = State::kIdle;
    Kinds _kinds;
    bool _game = false;
    float _since = 0.0f;  // when the first press went down (kDown1) or came up (kWait)
};
}  // namespace AnimationInjector::PressKinds
