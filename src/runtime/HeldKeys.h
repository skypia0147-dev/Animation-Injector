#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace AnimationInjector::HeldKeys {
// Which keys the player holds now (10-30, charge): a transition on="release" goes once its key is up, and the held
// time condition (heldTime) reads how long it was down. Fed by the input sink from every button event, menus and the
// console included, so a release while a menu was open is not missed. Pure (no game types): tests drive it. Main
// thread (the input dispatch and the player's update).

enum class Source : std::uint8_t { kKey, kPad, kControl };  // key: scan code (mouse +256); control: GameControl
struct Key {
    Source source = Source::kKey;
    std::uint32_t code = 0;
    bool operator==(const Key&) const = default;
};

class Tracker {
public:
    // A button went down or up (`now`: the sink's game clock, seconds).
    void Press(Key key, float now);
    void Release(Key key, float now);
    bool Held(Key key) const;
    // The key whose press started the player's running action (a start or a key transition), and when it went down
    // (a tap, double or hold: the first press). Kept through on="end" and on="release" transitions.
    void SetChain(Key key, float downAt);
    std::optional<Key> Chain() const { return _chain; }
    // How long the chain key was down: until its release, or until `now` while it is still down. <0 without one.
    float ChainHeldTime(float now) const;
    // The player's movement direction now, as a movement condition reads it (MoveDir: 0 forward .. 4 none; -1
    // unknown): set every frame by the player's update (Actions::MoveDirection), read by on="move" transitions.
    void SetMoveDirection(int direction) { _moveDirection = direction; }
    int MoveDirection() const { return _moveDirection; }
    // Whether the key that started the player's action is still down (a chain's maxHold counts from its press).
    bool ChainHeld() const { return _chain && Held(*_chain); }
    // A load: nothing is held.
    void Reset();

private:
    struct Record {
        Key key;
        bool down = false;
        float upAt = 0.0f;
    };
    Record* Find(Key key);
    const Record* Find(Key key) const;
    std::vector<Record> _records;
    std::optional<Key> _chain;
    float _chainDownAt = 0.0f;
    int _moveDirection = -1;
};

Tracker& Get();
}  // namespace AnimationInjector::HeldKeys
