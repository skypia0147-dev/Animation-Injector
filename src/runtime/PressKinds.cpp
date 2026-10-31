#include "runtime/PressKinds.h"

namespace AnimationInjector::PressKinds {
Step Classifier::Down(float now, Kinds kinds, bool game, const Timing&) {
    // The second press within the double window (Tick settled an expired one): decided now, held or not.
    if (_state == State::kWait) {
        _state = State::kConsumed;
        return {Outcome::kDouble, _game};
    }
    if (_state != State::kIdle || !kinds.Any()) return {};
    _state = State::kDown1;
    _kinds = kinds;
    _game = game;
    _since = now;
    return {Outcome::kNone, game};
}

Step Classifier::Up(float now, const Timing&) {
    switch (_state) {
    case State::kDown1:
        if (_kinds.dbl) {
            _state = State::kWait;
            _since = now;
            return {Outcome::kNone, _game};
        }
        _state = State::kIdle;
        return {_kinds.tap ? Outcome::kTap : _game ? Outcome::kGameTap : Outcome::kNone, _game};
    case State::kConsumed: _state = State::kIdle; return {Outcome::kNone, _game};
    case State::kPassed: _state = State::kIdle; return {};
    default: return {};  // a release this input never kept
    }
}

Outcome Classifier::Tick(float now, const Timing& timing) {
    if (_state == State::kDown1 && now - _since >= timing.hold) {
        if (_kinds.hold) {
            _state = State::kConsumed;
            return Outcome::kHold;
        }
        // A long press no kind takes: a game control's goes to the game now; a key's does nothing.
        _state = _game ? State::kPassed : State::kConsumed;
        return _game ? Outcome::kGameDown : Outcome::kNone;
    }
    if (_state == State::kWait && now - _since >= timing.doubleWindow) {
        _state = State::kIdle;
        return _kinds.tap ? Outcome::kTap : _game ? Outcome::kGameTap : Outcome::kNone;
    }
    return Outcome::kNone;
}

void Classifier::ToGame() {
    if (_state == State::kConsumed) _state = State::kPassed;
}

void Classifier::Reset() { _state = State::kIdle; }
}  // namespace AnimationInjector::PressKinds
